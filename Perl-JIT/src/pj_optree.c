#include "pj_optree.h"
#include <stdlib.h>
#include <stdio.h>

#include <OPTreeVisitor.h>

#include "ppport.h"
#include "pj_inline.h"
#include "pj_debug.h"

#include "pj_ast_terms.h"
#include "pj_global_state.h"
#include "pj_keyword_plugin.h"

#include <vector>
#include <string>
#include <tr1/unordered_map>

/* inspired by B.xs */
#define PMOP_pmreplstart(o)	o->op_pmstashstartu.op_pmreplstart
#define PMOP_pmreplroot(o)	o->op_pmreplrootu.op_pmreplroot

/* From B::Generate */
#ifndef PadARRAY
# if PERL_VERSION < 8 || (PERL_VERSION == 8 && !PERL_SUBVERSION)
typedef AV PADLIST;
typedef AV PAD;
# endif
# define PadlistARRAY(pl)	((PAD **)AvARRAY(pl))
# define PadlistNAMES(pl)	(*PadlistARRAY(pl))
# define PadARRAY		AvARRAY
#endif

namespace PerlJIT {
  class OPTreeJITCandidateFinder;
}

using namespace PerlJIT;
using namespace std::tr1;
using namespace std;

static vector<PerlJIT::AST::Term *>
pj_find_jit_candidates_internal(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor);
static PerlJIT::AST::Term *
pj_attempt_jit(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor);

// This will import two macros IS_AST_COMPATIBLE_ROOT_OP_TYPE
// and IS_AST_COMPATIBLE_OP_TYPE. The macros determine whether to attempt
// to ASTify a given Perl OP. Difference between the two macros described
// below.
#include "src/pj_ast_op_switch-gen.inc"
/* AND and OR at top level can be used in "interesting" places such as looping constructs.
 * Thus, we'll -- for now -- only support them as OPs within a tree.
 * NULLs may need to be skipped occasionally, so we do something similar.
 * PADSVs are recognized as subtrees now, so no use making them jittable root OP.
 * CONSTs would be further constant folded if they were a candidate root OP, so
 * no sense trying to JIT them if they're free-standing. */

namespace PerlJIT {
  class OPTreeJITCandidateFinder : public OPTreeVisitor
  {
  public:
    OPTreeJITCandidateFinder(pTHX_ CV *cv)
      : containing_cv(cv)
    {
      // typed_declarations may end up being NULL!
      if (cv != NULL)
        typed_declarations = pj_get_typed_variable_declarations(aTHX_ cv);
    }

    visit_control_t
    visit_op(pTHX_ OP *o, OP *parentop)
    {
      unsigned int otype;
      otype = o->op_type;

      PJ_DEBUG_1("Considering %s\n", OP_NAME(o));

      /* Attempt JIT if the right OP type. Don't recurse if so. */
      if (IS_AST_COMPATIBLE_ROOT_OP_TYPE(otype)) {
        PerlJIT::AST::Term *ast = pj_attempt_jit(aTHX_ o, *this);
        if (ast)
            candidates.push_back(ast);
        return VISIT_SKIP;
      }
      return VISIT_CONT;
    } // end 'visit_op'

    // Declaration points to an op with OPpLVAL_INTRO, reference points
    // the op that is being processed (which might or might not have the
    // OPpLVAL_INTRO flag).
    // This is required because we might process a variable reference
    // before seeing the declaration.
    AST::VariableDeclaration *
    get_declaration(OP *declaration, OP *reference)
    {
      AST::VariableDeclaration *decl = variables[reference->op_targ];
      if (decl)
        return decl;

      // Use type from CV's MAGIC annotation to tag VariableDeclaration here
      // or otherwise create default type
      decl = new AST::VariableDeclaration(declaration, variables.size());
      if (typed_declarations) {
        pj_declaration_map_t::iterator it = typed_declarations->find(reference->op_targ);

        if (it != typed_declarations->end())
          decl->set_value_type(it->second.get_type());
      }
      if (!decl->get_value_type())
        decl->set_value_type(new AST::Scalar(pj_unspecified_type));

      variables[reference->op_targ] = decl;

      return decl;
    }

    vector<PerlJIT::AST::Term *> candidates;
    unordered_map<PADOFFSET, AST::VariableDeclaration *> variables;
    CV *containing_cv;
    pj_declaration_map_t *typed_declarations;
  }; // end class OPTreeJITCandidateFinder
}


/* Walk OP tree recursively, build ASTs, build subtrees */
static PerlJIT::AST::Term *
pj_build_ast(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor)
{
  PerlJIT::AST::Term *retval = NULL;

  assert(o);

  const unsigned int otype = o->op_type;
  PJ_DEBUG_1("pj_build_ast ASTing OP of type %s\n", OP_NAME(o));
  if (!IS_AST_COMPATIBLE_OP_TYPE(otype)) {
    // Can't represent OP with AST. So instead, recursively scan for
    // separate candidates and treat as subtree.
    PJ_DEBUG_1("Cannot represent this OP with AST. Emitting OP tree term in AST (Perl OP=%s).\n", OP_NAME(o));
    retval = new AST::Optree(o);
    pj_find_jit_candidates_internal(aTHX_ o, visitor);
  }

  // Done with all bizarre special cases. Return if those were a match.
  if (retval != NULL) {
    if (PJ_DEBUGGING)
      retval->dump();
    return retval;
  }

  // Build child list if applicable
  vector<PerlJIT::AST::Term *> kid_terms;
  unsigned int ikid = 0;
  if (o->op_flags & OPf_KIDS) {
    for (OP *kid = ((UNOP*)o)->op_first; kid; kid = kid->op_sibling) {
      PJ_DEBUG_2("pj_build_ast considering kid (%u) type %s\n", ikid, OP_NAME(kid));

      if (kid->op_type == OP_NULL && !(kid->op_flags & OPf_KIDS)) {
        PJ_DEBUG_1("Skipping kid (%u) since it's an OP_NULL without kids.\n", ikid);
        continue;
      }

      // FIXME possibly wrong. PUSHMARK assumed to be an implementation detail that is not
      //       strictly necessary in an AST listop. Totally speculative.
      if (kid->op_type == OP_PUSHMARK && !(kid->op_flags & OPf_KIDS)) {
        PJ_DEBUG_1("Skipping kid (%u) since it's an OP_PUSHMARK without kids.\n", ikid);
        continue;
      }

      AST::Term *kid_term = pj_build_ast(aTHX_ kid, visitor);

      // Handle a few special kid cases
      if (kid_term == NULL) {
        // Failed to build sub-AST, free ASTs build thus far before bailing
        PJ_DEBUG("Failed to build sub-AST - unwinding.\n");
        vector<PerlJIT::AST::Term *>::iterator it = kid_terms.begin();
        for (; it != kid_terms.end(); ++it)
          delete *it;
        return NULL;
      }
      else if (kid_term->type == pj_ttype_op && ((AST::Op *)kid_term)->optype == pj_unop_empty) {
        // empty list is not really a kid, don't include in child list
      }
      else {
        kid_terms.push_back(kid_term);
      }

      if (PJ_DEBUGGING)
        printf("pj_build_ast got kid (%u, %p) of type %s in return\n", ikid, kid_term, kid_term->perl_class());
      ++ikid;
    } // end for kids
  } // end if have kids

  /* TODO modulo may have (very?) different behaviour in Perl than in C (or libjit or the platform...) */
#define EMIT_UNOP_CODE(perl_op_type, pj_op_type)          \
  case perl_op_type:                                      \
    assert(kid_terms.size() == 1);                        \
    retval = new AST::Unop(o, pj_op_type, kid_terms[0]);  \
    break;
#define EMIT_UNOP_CODE_OPTIONAL(perl_op_type, pj_op_type)   \
  case perl_op_type:                                        \
    assert(kid_terms.size() == 1 || kid_terms.size() == 0); \
    if (kid_terms.size() == 1)                              \
      retval = new AST::Unop(o, pj_op_type, kid_terms[0]);  \
    else /* no kids */                                      \
      retval = new AST::Unop(o, pj_op_type, NULL);          \
    break;
#define EMIT_BINOP_CODE(perl_op_type, pj_op_type)                         \
  case perl_op_type:                                                      \
    assert(ikid == 2);                                                    \
    retval = new AST::Binop(o, pj_op_type, kid_terms[0], kid_terms[1]);   \
    break;
#define EMIT_BINOP_CODE_OPTIONAL(perl_op_type, pj_op_type)              \
  case perl_op_type:                                                    \
    assert(kid_terms.size() <= 2);                                      \
    if (kid_terms.size() < 2)                                           \
      kid_terms.push_back(NULL);                                        \
    retval = new AST::Binop(o, pj_op_type, kid_terms[0], kid_terms[1]); \
    break;
#define EMIT_LISTOP_CODE(perl_op_type, pj_op_type)      \
  case perl_op_type:                                    \
    retval = new AST::Listop(o, pj_op_type, kid_terms); \
    break;

  switch (otype) {
  case OP_CONST: {
      // FIXME OP_CONST can also be  string and who-knows-what-else
      SV *constsv = cSVOPx_sv(o);
      if (SvIOK(constsv)) {
        retval = new AST::NumericConstant(o, (IV)SvIV(constsv));
      }
      else if (SvUOK(constsv)) {
        retval = new AST::NumericConstant(o, (UV)SvUV(constsv));
      }
      else if (SvNOK(constsv)) {
        retval = new AST::NumericConstant(o, (NV)SvNV(constsv));
      }
      else if (SvPOK(constsv)) {
        retval = new AST::StringConstant(aTHX_ o, constsv);
      }
      else { // FAIL. Cast to NV
        if (PJ_DEBUGGING) {
          PJ_DEBUG("Casting OP_CONST's SV to an NV since type is unclear. SV dump follows:");
          sv_dump(constsv);
        }
        retval = new AST::NumericConstant(o, (NV)SvNV(constsv));
      }

      break;
    }
  case OP_PADSV:
    if (o->op_flags & OPpLVAL_INTRO)
      retval = visitor.get_declaration(o, o);
    else
      retval = new AST::Variable(o, visitor.get_declaration(0, o));
    break;
  case OP_GVSV:
    // FIXME OP_GVSV can have OPpLVAL_INTRO - Not sure what that means...
    //if (o->op_flags & OPpLVAL_INTRO)
    //  retval = visitor.get_declaration(o, o);
    //else
    retval = new AST::Variable(o, NULL); // FIXME want to support a declaration, too! (our)
    break;
  case OP_NULL:
    if (kid_terms.size() == 1) {
      if (o->op_targ == 0) {
        // attempt to pass through this untyped null-op. FIXME likely WRONG
        PJ_DEBUG("Passing through kid of OP_NULL\n");
        retval = kid_terms[0];
      }
      else {
        const unsigned int targ_otype = (unsigned int)o->op_targ;
        switch (targ_otype) {
        case OP_RV2SV: // Skip into ex-rv2sv for optimized global scalar access
          PJ_DEBUG("Passing through kid of ex-rv2sv\n");
          retval = kid_terms[0];
          break;
        default:
          PJ_DEBUG_1("Cannot represent this NULL OP with AST. Emitting OP tree term in AST. (%s)", OP_NAME(o));
          pj_find_jit_candidates_internal(aTHX_ o, visitor);
          retval = new AST::Optree(o);
          break;
        }
      }
    }
    else {
      PJ_DEBUG_1("Cannot represent this NULL OP with AST. Emitting OP tree term in AST. (%s)", OP_NAME(o));
      pj_find_jit_candidates_internal(aTHX_ o, visitor);
      retval = new AST::Optree(o);
    }
    break;
  case OP_ANDASSIGN:
  case OP_ORASSIGN:
  case OP_DORASSIGN: {
      //  6        <|> orassign(other->7) vK/1 ->9
      //  5           <0> padsv[$x:1,2] sRM ->6
      //  8           <1> sassign sK/BKWARD,1 ->9
      //  7              <$> const[IV 123] s ->8
      // Patch out the sassign!
      assert(kid_terms.size() == 2);
      if (kid_terms[1]->type == pj_ttype_op
          && ((AST::Op *)kid_terms[1])->optype == pj_binop_sassign
          && ((AST::Op *)kid_terms[1])->kids[1] == NULL)
      {
        // one of them funny sassigns...
        PJ_DEBUG("Patching out uninteresting sassign without and/or/dor-assign.\n");
        AST::Binop *tmp = (AST::Binop *)kid_terms[1];
        kid_terms[1] = tmp->kids[0];
        tmp->kids[0] = NULL; // ownership fix
        delete tmp;
      }
      else {
        kid_terms[1]->dump();
        abort();
      }
      pj_op_type t =   otype == OP_ANDASSIGN ? pj_binop_bool_and
                     : otype == OP_ORASSIGN  ? pj_binop_bool_or
                     :                         pj_binop_definedor;
      retval = new AST::Binop(o, t, kid_terms[0], kid_terms[1]);
      ((AST::Binop *)retval)->set_assignment_form(true);
      break;
    }
  case OP_STUB: {
      const int gimme = OP_GIMME(o, 0);
      if (gimme) {
        if (gimme == OPf_WANT_SCALAR) {
          retval = new AST::UndefConstant();
        }
        else { // list or void context
          // FIXME really, empty list
          retval = new AST::Unop(o, pj_unop_empty, NULL);
        }
      }
      else { // undecidable yet
        retval = new AST::Unop(o, pj_unop_empty, NULL);
      }
      break;
    }
  case OP_LIST:
    if (kid_terms.size() == 1)
      retval = kid_terms[0];
    else
      retval = new AST::Listop(o, pj_listop_list2scalar, kid_terms);
    break;
    EMIT_BINOP_CODE(OP_ADD, pj_binop_add)
    EMIT_BINOP_CODE(OP_SUBTRACT, pj_binop_subtract)
    EMIT_BINOP_CODE(OP_MULTIPLY, pj_binop_multiply)
    EMIT_BINOP_CODE(OP_DIVIDE, pj_binop_divide)
    EMIT_BINOP_CODE(OP_MODULO, pj_binop_modulo)
    EMIT_BINOP_CODE(OP_ATAN2, pj_binop_atan2)
    EMIT_BINOP_CODE(OP_POW, pj_binop_pow)
    EMIT_BINOP_CODE(OP_LEFT_SHIFT, pj_binop_left_shift)
    EMIT_BINOP_CODE(OP_RIGHT_SHIFT, pj_binop_right_shift)
    EMIT_BINOP_CODE(OP_BIT_AND, pj_binop_bitwise_and)
    EMIT_BINOP_CODE(OP_BIT_OR, pj_binop_bitwise_or)
    EMIT_BINOP_CODE(OP_BIT_XOR, pj_binop_bitwise_xor)
    EMIT_BINOP_CODE(OP_AND, pj_binop_bool_and)
    EMIT_BINOP_CODE(OP_OR, pj_binop_bool_or)
    EMIT_BINOP_CODE(OP_DOR, pj_binop_definedor)
    EMIT_BINOP_CODE(OP_EQ, pj_binop_num_eq)
    EMIT_BINOP_CODE(OP_NE, pj_binop_num_ne)
    EMIT_BINOP_CODE(OP_GT, pj_binop_num_gt)
    EMIT_BINOP_CODE(OP_LT, pj_binop_num_lt)
    EMIT_BINOP_CODE(OP_GE, pj_binop_num_ge)
    EMIT_BINOP_CODE(OP_LE, pj_binop_num_le)
    EMIT_BINOP_CODE(OP_NCMP, pj_binop_num_cmp)
    EMIT_BINOP_CODE(OP_SEQ, pj_binop_str_eq)
    EMIT_BINOP_CODE(OP_SNE, pj_binop_str_ne)
    EMIT_BINOP_CODE(OP_SGT, pj_binop_str_gt)
    EMIT_BINOP_CODE(OP_SLT, pj_binop_str_lt)
    EMIT_BINOP_CODE(OP_SGE, pj_binop_str_ge)
    EMIT_BINOP_CODE(OP_SLE, pj_binop_str_le)
    EMIT_BINOP_CODE(OP_SCMP, pj_binop_str_cmp)
    EMIT_BINOP_CODE(OP_CONCAT, pj_binop_concat)
    EMIT_BINOP_CODE_OPTIONAL(OP_SASSIGN, pj_binop_sassign)
    EMIT_UNOP_CODE(OP_NOT, pj_unop_bool_not)
    EMIT_UNOP_CODE(OP_NEGATE, pj_unop_negate)
    EMIT_UNOP_CODE(OP_COMPLEMENT, pj_unop_bitwise_not)
    EMIT_UNOP_CODE(OP_PREINC, pj_unop_preinc)
    EMIT_UNOP_CODE(OP_POSTINC, pj_unop_postinc)
    EMIT_UNOP_CODE(OP_PREDEC, pj_unop_predec)
    EMIT_UNOP_CODE(OP_POSTDEC, pj_unop_postdec)
    EMIT_UNOP_CODE(OP_RV2SV, pj_unop_sv_deref)
    EMIT_UNOP_CODE(OP_SREFGEN, pj_unop_sv_ref)
    EMIT_UNOP_CODE(OP_AV2ARYLEN, pj_unop_array_len)
    EMIT_UNOP_CODE_OPTIONAL(OP_ABS, pj_unop_abs)
    EMIT_UNOP_CODE_OPTIONAL(OP_SIN, pj_unop_sin)
    EMIT_UNOP_CODE_OPTIONAL(OP_COS, pj_unop_cos)
    EMIT_UNOP_CODE_OPTIONAL(OP_SQRT, pj_unop_sqrt)
    EMIT_UNOP_CODE_OPTIONAL(OP_LOG, pj_unop_log)
    EMIT_UNOP_CODE_OPTIONAL(OP_EXP, pj_unop_exp)
    EMIT_UNOP_CODE_OPTIONAL(OP_INT, pj_unop_perl_int)
    EMIT_UNOP_CODE_OPTIONAL(OP_DEFINED, pj_unop_defined)
    EMIT_UNOP_CODE_OPTIONAL(OP_RAND, pj_unop_rand)
    EMIT_UNOP_CODE_OPTIONAL(OP_SRAND, pj_unop_srand)
    EMIT_UNOP_CODE_OPTIONAL(OP_HEX, pj_unop_hex)
    EMIT_UNOP_CODE_OPTIONAL(OP_OCT, pj_unop_oct)
    EMIT_UNOP_CODE_OPTIONAL(OP_LENGTH, pj_unop_length)
    EMIT_UNOP_CODE_OPTIONAL(OP_ORD, pj_unop_ord)
    EMIT_UNOP_CODE_OPTIONAL(OP_CHR, pj_unop_chr)
    EMIT_UNOP_CODE_OPTIONAL(OP_LC, pj_unop_lc)
    EMIT_UNOP_CODE_OPTIONAL(OP_UC, pj_unop_uc)
    EMIT_UNOP_CODE_OPTIONAL(OP_LCFIRST, pj_unop_lcfirst)
    EMIT_UNOP_CODE_OPTIONAL(OP_UCFIRST, pj_unop_ucfirst)
    EMIT_UNOP_CODE_OPTIONAL(OP_QUOTEMETA, pj_unop_quotemeta)
    EMIT_UNOP_CODE_OPTIONAL(OP_UNDEF, pj_unop_undef)
    EMIT_UNOP_CODE_OPTIONAL(OP_GETC, pj_unop_getc)
    EMIT_UNOP_CODE_OPTIONAL(OP_TIME, pj_unop_time)
    EMIT_UNOP_CODE_OPTIONAL(OP_TMS, pj_unop_times)
    EMIT_UNOP_CODE_OPTIONAL(OP_LOCALTIME, pj_unop_localtime)
    EMIT_UNOP_CODE_OPTIONAL(OP_GMTIME, pj_unop_gmtime)
    EMIT_UNOP_CODE_OPTIONAL(OP_ALARM, pj_unop_alarm)
    EMIT_UNOP_CODE_OPTIONAL(OP_SLEEP, pj_unop_sleep)
    EMIT_LISTOP_CODE(OP_COND_EXPR, pj_listop_ternary)
    EMIT_LISTOP_CODE(OP_SUBSTR, pj_listop_substr)
    EMIT_LISTOP_CODE(OP_CHOP, pj_listop_chop)
    EMIT_LISTOP_CODE(OP_SCHOP, pj_listop_chop)
    EMIT_LISTOP_CODE(OP_CHOMP, pj_listop_chomp)
    EMIT_LISTOP_CODE(OP_SCHOMP, pj_listop_chomp)
    EMIT_LISTOP_CODE(OP_VEC, pj_listop_vec)
    EMIT_LISTOP_CODE(OP_SPRINTF, pj_listop_sprintf)
    EMIT_LISTOP_CODE(OP_PRTF, pj_listop_printf)
    EMIT_LISTOP_CODE(OP_PRINT, pj_listop_print)
    EMIT_LISTOP_CODE(OP_SAY, pj_listop_say)
    EMIT_LISTOP_CODE(OP_JOIN, pj_listop_join)
    EMIT_LISTOP_CODE(OP_READ, pj_listop_read)
  default:
    warn("Shouldn't happen! Unsupported OP!? %s\n", OP_NAME(o));
    abort();
  }
#undef EMIT_BINOP_CODE
#undef EMIT_BINOP_CODE_OPTIONAL
#undef EMIT_UNOP_CODE
#undef EMIT_UNOP_CODE_OPTIONAL
#undef EMIT_LISTOP_CODE

  /* PMOP doesn't matter for JIT right now */
  /*
    if (o && OP_CLASS(o) == OA_PMOP && o->op_type != OP_PUSHRE
          && (kid = PMOP_pmreplroot(cPMOPo)))
    {}
  */

  if (PJ_DEBUGGING && retval != NULL)
    retval->dump();
  return retval;
}

/* Starting from a candidate for JITing, walk the OP tree to accumulate
 * a subtree that can be replaced with a single JIT OP. */
/* TODO: Needs to walk the OPs, checking whether they qualify. If
 *       not, then that subtree needs to be added to the list of
 *       trees to be executed before executing the JIT OP itself,
 *       so that their return values end up on the stack
 *       (warning: TARG optimizations!). Also needs to record the
 *       kind of OP that includes the unJITable subtree so that
 *       "type context" can be inferred. Needs recurse depth-first,
 *       left-hugging in order to get the sub tree is normal
 *       execution order. */

static PerlJIT::AST::Term *
pj_attempt_jit(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor)
{
  PerlJIT::AST::Term *ast;

  if (PJ_DEBUGGING)
    printf("Attempting JIT on %s (%p, %p)\n", OP_NAME(o), o, o->op_next);

  ast = pj_build_ast(aTHX_ o, visitor);

  return ast;
}

/* Traverse OP tree from o until done OR a candidate for JITing was found.
 * For candidates, invoke JIT attempt and then move on without going into
 * the particular sub-tree; tree walking in OPTreeWalker, actual logic in
 * OPTreeJITCandidateFinder! */
static vector<PerlJIT::AST::Term *>
pj_find_jit_candidates_internal(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor)
{
  visitor.visit(aTHX_ o, NULL);
  return visitor.candidates;
}

vector<PerlJIT::AST::Term *>
pj_find_jit_candidates(pTHX_ SV *coderef)
{
  if (!SvROK(coderef) || SvTYPE(SvRV(coderef)) != SVt_PVCV)
    croak("Need a code reference");
  CV *cv = (CV *) SvRV(coderef);

  ENTER;
  SAVECOMPPAD(); // restores both PL_comppad and PL_curpad

  PL_comppad = PadlistARRAY(CvPADLIST(cv))[1];
  PL_curpad = AvARRAY(PL_comppad);

  OPTreeJITCandidateFinder f(aTHX_ cv);
  vector<PerlJIT::AST::Term *> tmp = pj_find_jit_candidates_internal(aTHX_ CvROOT(cv), f);
  if (PJ_DEBUGGING) {
    printf("%i JIT candidate ASTs:\n", (int)tmp.size());
    for (unsigned int i = 0; i < (unsigned int)tmp.size(); ++i) {
      printf("===========================\n");
      tmp[i]->dump();
    }
    printf("===========================\n");
  }

  LEAVE;

  return tmp;
}
