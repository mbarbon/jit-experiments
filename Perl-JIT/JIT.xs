#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "ppport.h"

#include <assert.h>

#include "stack.h"

#include <jit/jit.h>
#include <pj_terms.h>
#include <pj_jit.h>

/* The struct of pertinent per-OP instance
 * data that we attach to each JIT OP. */
typedef struct {
  void (*jit_fun)(void);
  NV *paramslist;
  UV nparams;
} pj_jitop_aux_t;

/* The actual custom op definition structure */
static XOP my_xop_jitop;
/* The generic custom OP implementation - push/pop function */
static OP *my_pp_jit(pTHX);
/* Original peephole optimizer */
static peep_t orig_peepp;

/* Original opfreehook - we wrap this to free JIT OP aux structs */
static Perl_ophook_t orig_opfreehook;

/* Global state. Obviously not thread-safe.
 * Thread-safety would require this to be dangling off the
 * interpreter struct in some fashion. */
static jit_context_t pj_jit_context = NULL; /* jit_context_t is a ptr */


/* End-of-global-destruction cleanup hook.
 * Actually installed in BOOT XS section. */
void
pj_jit_final_cleanup(pTHX_ void *ptr)
{
  printf("pj_jit_final_cleanup after global destruction.\n");
  if (pj_jit_context == NULL)
    jit_context_destroy(pj_jit_context);
}

/* Hook that will free the JIT OP aux structure of our custom ops */
/* FIXME this doesn't appear to actually be called for all ops -
 *       specifically NOT for our custom OP. Is this because the
 *       custom OP isn't wired up correctly? */
static void
my_opfreehook(pTHX_ OP *o)
{
  if (orig_opfreehook != NULL)
    orig_opfreehook(aTHX_ o);

  /* printf("cleaning %s\n", OP_NAME(o)); */
  if (o->op_ppaddr == my_pp_jit) {
    printf("Cleaning up custom OP's pj_jitop_aux_t\n");
    pj_jitop_aux_t *aux = (pj_jitop_aux_t *)o->op_targ;
    free(aux->paramslist);
    free(aux);
    o->op_targ = 0; /* important or Perl will use it to access the pad */
  }
}


static void
fixup_parent_op(pTHX_ OP *parent, OP *oldkid, OP *newkid)
{
  OP *kid;
  /* fixup parent's basic order ptr */

  if (((BINOP *)parent)->op_first == (OP *)oldkid) {
    ((BINOP *)parent)->op_first = newkid;
    printf("Replaced parent pointer in op_first!\n");
    return;
  }

  for (kid = ((BINOP *)parent)->op_first; kid != NULL; kid = kid->op_sibling) {
    if (kid->op_sibling == (OP *)oldkid) {
      ((BINOP *)kid)->op_first = newkid;
      printf("Replaced parent pointer in op_first => op_last list!\n");
      return;
    }
  }

  if (OP_CLASS(parent) == OA_COP) {
    for (; parent!= NULL; parent = parent->op_sibling) {
      if (parent->op_sibling == (OP *)oldkid) {
        ((BINOP *)parent)->op_sibling = newkid;
        printf("Replaced parent pointer in sibling list!\n");
        return;
      }
    }
  }

  abort();
}

static void
attempt_add_jit_proof_of_principle(pTHX_ BINOP *addop, OP *parent)
{
  OP *jitop;
  OP *left  = addop->op_first;
  OP *right = addop->op_last;
  OP *kid;
  pj_jitop_aux_t *jit_aux;
  pj_term_t *jit_ast;
  jit_function_t func = NULL;
  pj_basic_type funtype;

  printf("left input: %s (%s)\n", OP_NAME(left), OP_DESC(left));
  printf("right input: %s (%s)\n", OP_NAME(right), OP_DESC(right));

  /* Create a custom op! */
  jitop = newBINOP(OP_CUSTOM, 0, left, right);
  jitop->op_flags |= OPf_STACKED; /* OP receives some args via the stack */

  /* Attach JIT info to it */
  jit_aux = malloc(sizeof(pj_jitop_aux_t));
  jit_aux->nparams = 2;
  jit_aux->paramslist = (NV *)malloc(sizeof(NV) * 2); /* FIXME hardcoded to double params for now */
  jit_aux->jit_fun = NULL;

  /* FIXME FIXME FIXME
   * It turns out that op_targ is probably not safe to use for custom OPs because
   * some core functions may meddle with it. */
  jitop->op_targ = (PADOFFSET)PTR2UV(jit_aux);

  /* JIT IT! */
  jit_ast = pj_make_binop(
    pj_binop_add,
    pj_make_variable(0, pj_double_type),
    pj_make_variable(1, pj_double_type)
  );

  if (0 == pj_tree_jit(pj_jit_context, jit_ast, &func, &funtype)) {
    printf("JIT succeeded!\n");
  } else {
    printf("JIT failed!\n");
  }
  void *cl = jit_function_to_closure(func);
  jit_aux->jit_fun = cl;

  /* release JIT AST after being done with compilation */
  free(jit_ast);
  jit_ast = NULL;

  /* Set it's implementation ptr */
  jitop->op_ppaddr = my_pp_jit;

  /* Expected output execution order:
   * ---> left -> right -> jitop ---> */

  /* wire right -> jitop */
  right->op_next = (OP *)jitop;

  /* wire jitop -> old pp_next */
  jitop->op_next = addop->op_next;

  /* Expected final basic structure:
   *  L   R
   *   \ /
   *    J
   *     \
   *      P
   */

  /* Fixup new OP's sibling ptr to fall into place of the old one (if any) */
  jitop->op_sibling = addop->op_sibling;

  /* fixup parent's basic order ptr */
  fixup_parent_op(aTHX_ (OP *)parent, (OP *)addop, (OP *)jitop);

  /* FIXME something still refers to the addop, so this segfaults.
   *       Maybe the parent and op_next fixups don't work?
   *       This would explain why the custom OP free hook never reaches
   *       the JIT OP. But execution of JIT OP seems to work just fine.
   *       That suggests that either I'm missing something fundamental, or the
   *       parent fixup is the part that's not right? */
  /* Perl_op_free(aTHX_ (OP *)addop); */
}

static void my_peep(pTHX_ OP *o)
{
  OP *root = o;
  /* SV *hint; */

  orig_peepp(aTHX_ o);

  printf("Looking at: %s (%s)\n", OP_NAME(o), OP_DESC(o));
  /* Currently disabled lexicalization hacks/experiments */
  /*
  hint = cop_hints_fetch_pvs(PL_curcop, "jit", 0);
  if (hint != NULL && SvTRUE(hint)) {
    printf("Optimizing this op!\n");
  }
  else {
    printf("NOT optimizing this op!\n");
  }
  */

  ptrstack_t *tovisit;
  tovisit = ptrstack_make(20, 0);

  OP *kid = o;
  while (kid->op_sibling != NULL) {
    kid = kid->op_sibling;
    ptrstack_push(tovisit, root);
    ptrstack_push(tovisit, kid);
  }

  OP *parent = root;
  while (!ptrstack_empty(tovisit)) {
    int do_recurse = 1;
    o = ptrstack_pop(tovisit);
    parent = ptrstack_pop(tovisit);

    printf("Walking: Looking at: %s (%s); parent: %s\n", OP_NAME(o), OP_DESC(o), OP_NAME(parent));

    if (o->op_type == OP_ADD) {
      if (cBINOPo->op_first->op_type == OP_PADSV && cBINOPo->op_last->op_type == OP_PADSV) {
        printf("Found candidate!\n");
        attempt_add_jit_proof_of_principle(aTHX_ cBINOPo, parent);
        do_recurse = 0; /* for now */
      }
    }

    /* TODO s/// can have more stuff inside but not OPf_KIDS set? (can steal from B::Concise later) */
    if (do_recurse && o->op_flags & OPf_KIDS) {
      printf("Who knew? %s has kids.\n", OP_NAME(o));
      for (kid = cBINOPo->op_first; kid != NULL; kid = kid->op_sibling) {
        ptrstack_push(tovisit, o);
        ptrstack_push(tovisit, kid);
      }
    }
  }

  ptrstack_free(tovisit);
}


OP *
my_pp_jit(pTHX)
{
  dVAR; dSP; dATARGET;
  //bool useleft;
  SV *svl, *svr;
  tryAMAGICbin_MG(add_amg, AMGf_assign|AMGf_numeric);
  svr = POPs;
  svl = TOPs;
  printf("Custom op called\n");

  pj_jitop_aux_t *aux = (pj_jitop_aux_t *) ((BINOP *)PL_op)->op_targ;
  double result;
  /* FIXME function ret type should be dynamic */
  double *params = aux->paramslist;
  params[0] = SvNV_nomg(svr);
  params[1] = SvNV_nomg(svl);
  printf("In: %f %f\n", params[0], params[1]);
  pj_invoke_func((pj_invoke_func_t) aux->jit_fun, params, aux->nparams, pj_double_type, (void *)&result);



/* This section is the classical Perl add op, give or take.
 * Requires changing
 *   svr = POPs;
 *   svl = TOPs;
 * to 
 *   svr = TOPs;
 *   svl = TOPm1s;
 */

  //useleft = USE_LEFT(svl);

  /* The real pp_add has a big block of code for PERL_PRESERVE_UV_IV here */
  //NV value = SvNV_nomg(svr);
  //(void)POPs;

  //if (!useleft) {
    /* left operand is undef, treat as zero. + 0.0 is identity. */
  //  SETn(value);
  //  RETURN;
  //}
  //value += SvNV_nomg(svl);
  //SETn( value );

  printf("Add result from JIT: %f\n", (float)result);
  SETn((NV)result);

  RETURN;
}


MODULE = Perl::JIT	PACKAGE = Perl::JIT

BOOT:
  {
    /* Setup our new peephole optimizer */
    orig_peepp = PL_peepp;
    PL_peepp = my_peep;

    /* Set up JIT compiler */
    pj_jit_context = jit_context_create();

    /* Setup our callback for cleaning up JIT OPs during global cleanup */
    orig_opfreehook = PL_opfreehook;
    PL_opfreehook = my_opfreehook;

    /* Setup our custom op */
    XopENTRY_set(&my_xop_jitop, xop_name, "jitaddop");
    XopENTRY_set(&my_xop_jitop, xop_desc, "an addop that makes useless use of JIT");
    XopENTRY_set(&my_xop_jitop, xop_class, OA_BINOP);
    Perl_custom_op_register(aTHX_ my_pp_jit, &my_xop_jitop);

    /* Register super-late global cleanup hook for global JIT state */
    Perl_call_atexit(aTHX_ pj_jit_final_cleanup, NULL);
  }

void
import(char *cl)
  PPCODE:
    /* Currently disabled lexicalization hacks/experiments */
    /*
    const PERL_CONTEXT *cxt = caller_cx(1, NULL);
    cophh_store_pvs(cxt->cx_u.cx_blk.blku_oldcop->cop_hints_hash, "jit", sv_2mortal(newSViv(1)), 0);
    */
    XSRETURN_EMPTY;