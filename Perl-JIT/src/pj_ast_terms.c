#include "pj_ast_terms.h"

#include <stdio.h>
#include <stdlib.h>

/* keep in sync with pj_op_type in .h file */
const char *pj_ast_op_names[] = {
  /* unops */
  "unary -",  /* pj_unop_negate */
  "sin",      /* pj_unop_sin */
  "cos",      /* pj_unop_cos */
  "abs",      /* pj_unop_abs */
  "sqrt",     /* pj_unop_sqrt */
  "log",      /* pj_unop_log */
  "exp",      /* pj_unop_exp */
  "int",      /* pj_unop_int */
  "~",        /* pj_unop_bitwise_not */
  "!",        /* pj_unop_bool_not */

  /* binops */
  "+",        /* pj_binop_add */
  "-",        /* pj_binop_subtract */
  "*",        /* pj_binop_multiply */
  "/",        /* pj_binop_divide */
  "%",        /* pj_binop_modulo */
  "atan2",    /* pj_binop_atan2 */
  "pow",      /* pj_binop_pow */
  "<<",       /* pj_binop_left_shift */
  ">>",       /* pj_binop_right_shift */
  "&",        /* pj_binop_bitwise_and */
  "|",        /* pj_binop_bitwise_or */
  "^",        /* pj_binop_bitwise_xor */
  "==",       /* pj_binop_eq */
  "!=",       /* pj_binop_ne */
  "<",        /* pj_binop_lt */
  "<=",       /* pj_binop_le */
  ">",        /* pj_binop_gt */
  ">=",       /* pj_binop_ge */
  "&&",       /* pj_binop_bool_and */
  "||",       /* pj_binop_bool_or */

  /* listops */
  "?:",       /* pj_listop_ternary */
};

unsigned int pj_ast_op_flags[] = {
  /* unops */
  0,                              /* pj_unop_negate */
  0,                              /* pj_unop_sin */
  0,                              /* pj_unop_cos */
  0,                              /* pj_unop_abs */
  0,                              /* pj_unop_sqrt */
  0,                              /* pj_unop_log */
  0,                              /* pj_unop_exp */
  0,                              /* pj_unop_int */
  0,                              /* pj_unop_bitwise_not */
  0,                              /* pj_unop_bool_not */

  /* binops */
  0,                              /* pj_binop_add */
  0,                              /* pj_binop_subtract */
  0,                              /* pj_binop_multiply */
  0,                              /* pj_binop_divide */
  0,                              /* pj_binop_modulo */
  0,                              /* pj_binop_atan2 */
  0,                              /* pj_binop_pow */
  0,                              /* pj_binop_left_shift */
  0,                              /* pj_binop_right_shift */
  0,                              /* pj_binop_bitwise_and */
  0,                              /* pj_binop_bitwise_or */
  0,                              /* pj_binop_bitwise_xor */
  0,                              /* pj_binop_eq */
  0,                              /* pj_binop_ne */
  0,                              /* pj_binop_lt */
  0,                              /* pj_binop_le */
  0,                              /* pj_binop_gt */
  0,                              /* pj_binop_ge */
  PJ_ASTf_CONDITIONAL,            /* pj_binop_bool_and */
  PJ_ASTf_CONDITIONAL,            /* pj_binop_bool_or */

  /* listops */
  PJ_ASTf_CONDITIONAL,            /* pj_listop_ternary */
};


pj_term_t *
pj_make_const_dbl(OP *perl_op, double c)
{
  pj_constant_t *co = new pj_constant_t();
  co->type = pj_ttype_constant;
  co->perl_op = perl_op;
  co->dbl_value = c;
  co->const_type = pj_double_type;
  return (pj_term_t *)co;
}

pj_term_t *
pj_make_const_int(OP *perl_op, int c)
{
  pj_constant_t *co = new pj_constant_t();
  co->type = pj_ttype_constant;
  co->perl_op = perl_op;
  co->int_value = c;
  co->const_type = pj_int_type;
  return (pj_term_t *)co;
}

pj_term_t *
pj_make_const_uint(OP *perl_op, unsigned int c)
{
  pj_constant_t *co = new pj_constant_t();
  co->type = pj_ttype_constant;
  co->perl_op = perl_op;
  co->uint_value = c;
  co->const_type = pj_uint_type;
  return (pj_term_t *)co;
}


pj_term_t *
pj_make_variable(OP *perl_op, int iv, pj_basic_type t)
{
  pj_variable_t *v = new pj_variable_t();
  v->type = pj_ttype_variable;
  v->perl_op = perl_op;
  v->var_type = t;
  v->ivar = iv;
  return (pj_term_t *)v;
}


pj_term_t *
pj_make_binop(OP *perl_op, pj_optype t, pj_term_t *o1, pj_term_t *o2)
{
  pj_op_t *o = new pj_binop_t();
  o->type = pj_ttype_op;
  o->perl_op = perl_op;
  o->optype = t;
  o->kids.resize(2);
  o->kids[0] = o1;
  o->kids[1] = o2;
  return (pj_term_t *)o;
}


pj_term_t *
pj_make_unop(OP *perl_op, pj_optype t, pj_term_t *o1)
{
  pj_op_t *o = new pj_unop_t();
  o->type = pj_ttype_op;
  o->perl_op = perl_op;
  o->optype = t;
  o->kids.resize(1);
  o->kids[0] = o1;
  return (pj_term_t *)o;
}


pj_term_t *
pj_make_listop(OP *perl_op, pj_optype t, const std::vector<pj_term_t *> &children)
{
  pj_op_t *o = new pj_listop_t();
  o->type = pj_ttype_op;
  o->perl_op = perl_op;
  o->optype = t;
  o->kids = children;
  return (pj_term_t *)o;
}


pj_term_t *
pj_make_optree(OP *perl_op)
{
  pj_op_t *o = new pj_op_t();
  o->type = pj_ttype_optree;
  o->perl_op = perl_op;
  return (pj_term_t *)o;
}


void
pj_free_tree(pj_term_t *t)
{
  if (t == NULL)
    return;

  if (t->type == pj_ttype_op) {
    std::vector<pj_term_t *> &k = ((pj_op_t *)t)->kids;
    const unsigned int n = k.size();
    for (unsigned int i = 0; i < n; ++i)
      pj_free_tree(k[i]);
  }

  delete t;
}


/* pinnacle of software engineering, but it's just for debugging anyway...  */
static void
pj_dump_tree_indent(int lvl)
{
  int i;
  for (i = 0; i < lvl; ++i)
    printf("  ");
}

static void
pj_dump_tree_internal(pj_term_t *term, int lvl)
{
  if (term->type == pj_ttype_constant)
  {
    pj_constant_t *c = (pj_constant_t *)term;
    pj_dump_tree_indent(lvl);
    if (c->const_type == pj_double_type)
      printf("C = %f\n", (float)c->dbl_value);
    else if (c->const_type == pj_int_type)
      printf("C = %i\n", (int)c->int_value);
    else if (c->const_type == pj_uint_type)
      printf("C = %lu\n", (unsigned long)c->uint_value);
    else
      abort();
  }
  else if (term->type == pj_ttype_variable)
  {
    pj_dump_tree_indent(lvl);
    printf("V = %i\n", ((pj_variable_t *)term)->ivar);
  }
  else if (term->type == pj_ttype_op)
  {
    pj_op_t *o = (pj_op_t *)term;

    pj_dump_tree_indent(lvl);

    printf("OP '%s' (\n", pj_ast_op_names[o->optype]);

    const unsigned int n = o->kids.size();
    for (unsigned int i = 0; i < n; ++i) {
      pj_dump_tree_internal(o->kids[i], lvl+1);
    }

    pj_dump_tree_indent(lvl);
    printf(")\n");
  }
  else
    abort();
}


void
pj_dump_tree(pj_term_t *term)
{
  int lvl = 0;
  pj_dump_tree_internal(term, lvl);
}
