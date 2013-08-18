#include "pj_optree.h"
#include <stdlib.h>
#include <stdio.h>

#include <OPTreeVisitor.h>

#include "ppport.h"
#include "pj_inline.h"
#include "pj_debug.h"

#include "pj_ast_terms.h"

#include "pj_global_state.h"

#include <vector>
#include <tr1/unordered_map>

/* inspired by B.xs */
#define PMOP_pmreplstart(o)	o->op_pmstashstartu.op_pmreplstart
#define PMOP_pmreplroot(o)	o->op_pmreplrootu.op_pmreplroot

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

#define IS_JITTABLE_ROOT_OP_TYPE(otype) \
        ( otype == OP_ADD || otype == OP_SUBTRACT || otype == OP_MULTIPLY || otype == OP_DIVIDE \
          || otype == OP_SIN || otype == OP_COS || otype == OP_SQRT || otype == OP_EXP \
          || otype == OP_LOG || otype == OP_POW || otype == OP_INT || otype == OP_NOT \
          || otype == OP_LEFT_SHIFT || otype == OP_RIGHT_SHIFT /* || otype == OP_COMPLEMENT */ \
          || otype == OP_EQ || otype == OP_COND_EXPR || otype == OP_NEGATE )

/* AND and OR at top level can be used in "interesting" places such as looping constructs.
 * Thus, we'll -- for now -- only support them as OPs within a tree.
 * NULLs may need to be skipped occasionally, so we do something similar.
 * PADSVs are recognized as subtrees now, so no use making them jittable root OP.
 * CONSTs would be further constant folded if they were a candidate root OP, so
 * no sense trying to JIT them if they're free-standing. */
#define IS_JITTABLE_OP_TYPE(otype) \
        (IS_JITTABLE_ROOT_OP_TYPE(otype) \
          || otype == OP_PADSV \
          || otype == OP_CONST \
          || otype == OP_AND \
          || otype == OP_OR \
          || otype == OP_NULL )


namespace PerlJIT {
  class OPTreeJITCandidateFinder : public OPTreeVisitor
  {
  public:
    OPTreeJITCandidateFinder() {}

    visit_control_t
    visit_op(pTHX_ OP *o, OP *parentop)
    {
      unsigned int otype;
      otype = o->op_type;

      PJ_DEBUG_1("Considering %s\n", OP_NAME(o));

      /* Grab attribute information */
      if (otype == OP_ENTERSUB) {
        PerlJIT::AST::Term *ast = build_attributes_ast(aTHX_ cLISTOPo);
        if (ast)
            candidates.push_back(ast);
        return VISIT_SKIP;
      }
      /* Attempt JIT if the right OP type. Don't recurse if so. */
      if (IS_JITTABLE_ROOT_OP_TYPE(otype)) {
        PerlJIT::AST::Term *ast = pj_attempt_jit(aTHX_ o, *this);
        if (ast)
            candidates.push_back(ast);
        return VISIT_SKIP;
      }
      return VISIT_CONT;
    } // end 'visit_op'

    // declaration points to an op with OPpLVAL_INTRO, reference points
    // the op that is being processed (which might or might not have the
    // OPpLVAL_INTRO flag)
    // this is required because we might process a variable reference
    // before seeing the declaration
    AST::VariableDeclaration *
    get_declaration(OP *declaration, OP *reference)
    {
      AST::VariableDeclaration *decl = variables[reference->op_targ];
      if (decl)
        return decl;

      decl = new AST::VariableDeclaration(declaration, variables.size());
      variables[reference->op_targ] = decl;

      return decl;
    }

    PerlJIT::AST::Term *
    build_attributes_ast(pTHX_ LISTOP *o)
    {
      bool can_remove = false;
      AST::Type *type = 0;
      OP *variable = 0;

      if (!pj_parse_attributes(aTHX_ o, can_remove, type, variable))
        return 0;

      AST::VariableDeclaration *decl = get_declaration(0, variable);

      decl->set_value_type(type);

      if (can_remove)
        return new AST::NullOptree((OP *) o);
      else
        return 0;
    }

    vector<PerlJIT::AST::Term *> candidates;
    unordered_map<PADOFFSET, AST::VariableDeclaration *> variables;
  }; // end class OPTreeJITCandidateFinder
}


/* Scan a section of the OP tree and find whichever OP is
 * going to be executed first. This is done by doing pure
 * left-hugging depth-first traversal. Ignores op_next. */
PJ_STATIC_INLINE OP *
pj_find_first_executed_op(pTHX_ OP *o)
{
  PERL_UNUSED_CONTEXT;
  while (1) {
    if (o->op_flags & OPf_KIDS) {
      o = cUNOPo->op_first;
    }
    else {
      return o;
    }
  }
  /* TODO: handle PMOP? */
  /*
    if (o && OP_CLASS(o) == OA_PMOP && o->op_type != OP_PUSHRE
          && (kid = PMOP_pmreplroot(cPMOPo)))
    {}
  */
  abort(); /* not reached */
}


/* Expects to be called on an OP_ENTERSUB */
static bool
pj_parse_attributes(pTHX_ LISTOP *o, bool &can_remove, AST::Type *&type, OP *&variable)
{
  // recognizes the ops for
  // attributes::->import(__PACKAGE__, \$x, 'Bent');
  if (o->op_type != OP_ENTERSUB || !(o->op_flags & OPf_SPECIAL)
      || cLISTOPo->op_last->op_type != OP_METHOD_NAMED
      || strcmp(SvPVx(cSVOPx_sv(cLISTOPo->op_last), PL_na), "import") != 0)
    return false;

  OP *attrpackage = cLISTOPo->op_first->op_sibling;
  OP *currpackage = attrpackage ? attrpackage->op_sibling : 0;
  OP *makeref = currpackage ? currpackage->op_sibling : 0;
  OP *attr = makeref ? makeref->op_sibling : 0;

  if (!attrpackage || !currpackage || !makeref || !attr)
    return false;
  // check that the first 3 arguments and the invocant are what we expect
  if (attrpackage->op_type != OP_CONST
      || currpackage->op_type != OP_CONST
      || makeref->op_type != OP_SREFGEN)
    return false;

  // and an additional check that the 2nd argument is a scalar reference
  OP *lexical = cLISTOPx(cLISTOPx(makeref)->op_first)->op_first;

  if (lexical->op_type != OP_PADSV
      && lexical->op_type != OP_PADAV
      && lexical->op_type != OP_PADHV)
    return false;

  // check the invocant is actually "attributes"
  if (strcmp(SvPVx(cSVOPx_sv(attrpackage), PL_na),
             "attributes") != 0)
    return false;

  // check all arguments are constants
  for (OP *arg = attr; arg && arg != cLOOPo->op_last; arg = arg->op_sibling)
    if (arg->op_type != OP_CONST)
      return false;

  // we have a winner! now try parsing the attributes as a type
  type = 0;
  int remaining = 0;

  for (OP *pred = makeref; attr && attr != cLOOPo->op_last; pred = attr, attr = attr->op_sibling) {
    ++remaining;
    if (type)
      break;
    SV *cval = cSVOPx_sv(attr);

    PJ_DEBUG_1("Checking potential type (%s)\n", SvPV_nolen(cval));
    type = AST::parse_type(SvPV_nolen(cval));

    // parse the type: erase the parameter from the method call
    if (type)
    {
      PJ_DEBUG_1("  Parsed type declaration (%s)\n", SvPV_nolen(cval));
      --remaining;
      pred->op_next = attr->op_next;
    }
  }

  if (!type)
    return false;

  can_remove = !remaining;
  variable = lexical;

  return true;
}

/* Walk OP tree recursively, build ASTs, build subtrees */
static PerlJIT::AST::Term *
pj_build_ast(pTHX_ OP *o, OPTreeJITCandidateFinder &visitor)
{
  PerlJIT::AST::Term *retval = NULL;

  assert(o);

  const unsigned int otype = o->op_type;
  // found a SPECIAL entersub, which is a call to an "import" method:
  // check whether it might be handling the attribute for a lexical;
  // if so the call looks like
  if (otype == OP_ENTERSUB) {
    retval = visitor.build_attributes_ast(aTHX_ cLISTOPo);

    // if all attributes have been recognized, return a tree node that
    // is then jitted to nothing
    if (retval)
      return retval;
  }

  if (!IS_JITTABLE_OP_TYPE(otype)) {
    // Can't represent OP with AST. So instead, recursively scan for
    // separate candidates and treat as subtree.
    PJ_DEBUG_1("Cannot represent this OP with AST. Emitting OP tree term in AST. (%s)", OP_NAME(o));
    pj_find_jit_candidates_internal(aTHX_ o, visitor);
    retval = new AST::Optree(o, pj_find_first_executed_op(aTHX_ o));
  }
  else if (!(o->op_flags & OPf_KIDS)) {
    // Handle ops without kids first
    if (otype == OP_CONST) {
      /* FIXME OP_CONST can also be an int or a string and who-knows-what-else */
      SV *constsv = cSVOPx_sv(o);

      PJ_DEBUG_1("CONST being inlined (%f).\n", SvNV(constsv));
      retval = new AST::Constant(o, SvNV(constsv)); /* FIXME replace type by inferred type */
    }
    else if (otype == OP_PADSV) {
      if (o->op_flags & OPpLVAL_INTRO)
        retval = visitor.get_declaration(o, o);
      else
        retval = new AST::Variable(o, visitor.get_declaration(0, o));
    }
    else {
      croak("Shouldn't happen! Unsupported nullary OP!? %s", OP_NAME(o));
    }
  }

  if (retval != NULL) {
    if (PJ_DEBUGGING)
      retval->dump();
    return retval;
  }

  // If we reach this, then the OP has kids and we build the child optrees
  // before building this node.

  // Build child list
  vector<PerlJIT::AST::Term *> kid_terms;
  unsigned int ikid = 0;
  for (OP *kid = ((UNOP*)o)->op_first; kid; kid = kid->op_sibling) {
    PJ_DEBUG_2("pj_build_ast considering kid (%u) type %s\n", ikid, OP_NAME(kid));

    kid_terms.push_back( pj_build_ast(aTHX_ kid, visitor) );
    if (kid_terms.back() == NULL) {
      // Failed to build sub-AST, free ASTs build thus far before bailing
      kid_terms.pop_back();
      vector<PerlJIT::AST::Term *>::iterator it = kid_terms.begin();
      for (; it != kid_terms.end(); ++it)
        delete *it;
      return NULL;
    }
    ++ikid;
  } /* end for kids */

  /* TODO modulo may have (very?) different behaviour in Perl than in C (or libjit or the platform...) */
#define EMIT_UNOP_CODE(perl_op_type, pj_op_type)          \
  case perl_op_type:                                      \
    assert(ikid == 1);                                    \
    retval = new AST::Unop(o, pj_op_type, kid_terms[0]);  \
    break;
#define EMIT_BINOP_CODE(perl_op_type, pj_op_type)                         \
  case perl_op_type:                                                      \
    assert(ikid == 2);                                                    \
    retval = new AST::Binop(o, pj_op_type, kid_terms[0], kid_terms[1]);   \
    break;
#define EMIT_LISTOP_CODE(perl_op_type, pj_op_type)  \
  case perl_op_type: {                              \
    vector<PerlJIT::AST::Term *> kids;              \
    for (unsigned int i = 0; i < ikid-1; ++i)       \
      kids.push_back(kid_terms[i]);                 \
    retval = new AST::Listop(o, pj_op_type, kids);  \
    break;                                          \
  }

  switch (otype) {
    EMIT_BINOP_CODE(OP_ADD, pj_binop_add)
    EMIT_BINOP_CODE(OP_SUBTRACT, pj_binop_subtract)
    EMIT_BINOP_CODE(OP_MULTIPLY, pj_binop_multiply)
    EMIT_BINOP_CODE(OP_DIVIDE, pj_binop_divide)
    EMIT_BINOP_CODE(OP_POW, pj_binop_pow)
    EMIT_BINOP_CODE(OP_LEFT_SHIFT, pj_binop_left_shift)
    EMIT_BINOP_CODE(OP_RIGHT_SHIFT, pj_binop_right_shift)
    EMIT_BINOP_CODE(OP_EQ, pj_binop_eq)
    EMIT_BINOP_CODE(OP_AND, pj_binop_bool_and)
    EMIT_BINOP_CODE(OP_OR, pj_binop_bool_or)
    EMIT_UNOP_CODE(OP_SIN, pj_unop_sin)
    EMIT_UNOP_CODE(OP_COS, pj_unop_cos)
    EMIT_UNOP_CODE(OP_SQRT, pj_unop_sqrt)
    EMIT_UNOP_CODE(OP_LOG, pj_unop_log)
    EMIT_UNOP_CODE(OP_EXP, pj_unop_exp)
    EMIT_UNOP_CODE(OP_INT, pj_unop_perl_int)
    EMIT_UNOP_CODE(OP_NOT, pj_unop_bool_not)
    EMIT_UNOP_CODE(OP_NEGATE, pj_unop_negate)
    /* EMIT_UNOP_CODE(OP_COMPLEMENT, pj_unop_bitwise_not) */ /* FIXME not same as perl */
    EMIT_LISTOP_CODE(OP_COND_EXPR, pj_listop_ternary)
  case OP_NULL:
    if (kid_terms.size() == 1 && o->op_targ == 0) {
      // attempt a pass-through this null-op. FIXME likely WRONG
      retval = kid_terms[0];
      break;
    }
  default:
    warn("Shouldn't happen! Unsupported OP!? %s\n", OP_NAME(o));
    if (otype == OP_NULL)
      warn("This is an OP_NULL that might have been a %s before\n", PL_op_name[o->op_targ]);
    abort();
  }
#undef EMIT_BINOP_CODE
#undef EMIT_UNOP_CODE
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

  // allows using macros that refer to current pad
  AV *tmp_comppad = PL_comppad;
  SV **tmp_curpad = PL_curpad;

  PL_comppad = PadlistARRAY(CvPADLIST(cv))[1];
  PL_curpad = AvARRAY(PL_comppad);

  OPTreeJITCandidateFinder f;
  vector<PerlJIT::AST::Term *> tmp = pj_find_jit_candidates_internal(aTHX_ CvROOT(cv), f);
  if (PJ_DEBUGGING) {
    printf("%i JIT candidate ASTs:\n", (int)tmp.size());
    for (unsigned int i = 0; i < (unsigned int)tmp.size(); ++i) {
      printf("===========================\n");
      tmp[i]->dump();
    }
    printf("===========================\n");
  }

  PL_comppad = tmp_comppad;
  PL_curpad = tmp_curpad;

  return tmp;
}
