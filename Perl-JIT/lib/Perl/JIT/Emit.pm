package Perl::JIT::Emit;

use v5.14;

use Moo;

use warnings;
# This is to make given/when work on 5.14 and 5.18. *sigh*, and it
# needs to be after 'use Moo'
no warnings $] < 5.018 ? 'redefine' : 'experimental';
use warnings 'redefine';

use B::Generate;
use B::Replace;
use B qw(OPf_STACKED OPf_KIDS);

use Perl::JIT qw(:all);

use LibJIT::API qw(:all);
use LibJIT::PerlAPI qw(:all);

has jit_context => ( is => 'ro' );
has current_cv  => ( is => 'ro' );
has subtrees    => ( is => 'ro' );

sub BUILD {
    my ($self) = @_;

    $self->{jit_context} = jit_context_create();
}

sub jit_sub {
    my ($self, $sub) = @_;
    my @asts = Perl::JIT::find_jit_candidates($sub);
    local $self->{current_cv} = B::svref_2object($sub);
    local $self->{subtrees} = [];

    for my $ast (@asts) {
        my $op = $self->jit_tree($ast);

        # TODO add B::Generate API taking the CV
        B::Replace::replace_tree($self->current_cv->ROOT, $ast->get_perl_op, $op);
    }
}

sub jit_tree {
    my ($self, $ast) = @_;
    my $cxt = $self->jit_context;
    my $op;

    if (@{$self->subtrees}) {
        my $tree = $self->subtrees;

        $tree->[$_]->sibling($tree->[$_ + 1]) for 0 .. $#$tree;
        $op = B::LISTOP->new('stub', OPf_KIDS, $tree->[0], $tree->[-1]);
    } else {
        $op = B::OP->new('stub', 0);
    }

    jit_context_build_start($self->jit_context);

    my $fun = pa_create_pp($cxt);
    $self->_jit_emit_root($fun, $ast);

    jit_function_compile($fun);
    jit_context_build_end($self->jit_context);

    $op->ppaddr(jit_function_to_closure($fun));
    $op->targ($ast->get_perl_op->targ);

    return $op;
}

sub _jit_emit_root {
    my ($self, $fun, $ast) = @_;
    my $val = $self->_jit_emit($fun, $ast);

    $self->_jit_emit_return($fun, $ast, $val) if $val;

    jit_insn_return($fun, pa_get_op_next($fun));
}

sub _jit_emit_return {
    my ($self, $fun, $ast, $val) = @_;

    given ($ast->op_class) {
        when (pj_opc_binop) {
            # the assumption here is that the OPf_STACKED assignment
            # has been handled by _jit_emit below, and here we only need
            # to handle casses like '$x = $y += 7'
            my $targ = pa_get_targ($fun);

            pa_sv_set_nv($fun, $targ, $val);
            pa_push_sv($fun, $targ);
        }
        when (pj_opc_unop) {
            my $targ = pa_get_targ($fun);

            pa_sv_set_nv($fun, $targ, $val);
            pa_push_sv($fun, $targ);
        }
        default {
            pa_push_sv($fun, pa_sv_2mortal(pa_new_sv_nv($val)));
        }
    }
}

sub _jit_emit {
    my ($self, $fun, $ast) = @_;

    # TODO only doubles for now...
    given ($ast->get_type) {
        when (pj_ttype_constant) {
            return $self->_jit_emit_const($fun, $ast);
        }
        when (pj_ttype_variable) {
            my $padix = jit_value_create_nint_constant($fun, jit_type_nint, $ast->get_perl_op->targ);

            return pa_get_pad_sv($fun, $padix);
        }
        when (pj_ttype_optree) {
            # unfortunately there is (currently) no way to clone an optree,
            # so just detach the ops from the root tree
            B::Replace::detach_tree($self->current_cv->ROOT, $ast->get_perl_op, 1);
            $ast->get_perl_op->next(0);
            push @{$self->subtrees}, $ast->get_perl_op;

            my $op = jit_value_create_long_constant($fun, jit_type_ulong, ${$ast->get_start_op});
            pa_call_runloop($fun, $op);

            # TODO only works for scalar context
            return pa_pop_sv($fun);
        }
        when (pj_ttype_op) {
            return $self->_jit_emit_op($fun, $ast);
        }
    }
}

sub _to_nv {
    my ($self, $fun, $val) = @_;
    my $type = jit_value_get_type($val);

    if ($$type == ${jit_type_NV()}) {
        return $val;
    } elsif (jit_type_is_primitive($type) and $$type != ${jit_type_void()}) {
        return jit_insn_convert($fun, $val, jit_type_NV, 0);
    } elsif ($$type == ${jit_type_void_ptr()}) {
        return pa_sv_nv($fun, $val);
    } else {
        die "Handle more coercion cases";
    }
}

sub _to_numeric_type {
    my ($self, $fun, $val) = @_;
    my $type = jit_value_get_type($val);

    if (jit_type_is_primitive($type) and $$type != ${jit_type_void()}) {
        return $val;
    } elsif ($$type == ${jit_type_void_ptr()}) {
        return pa_sv_nv($fun, $val); # somewhat dubious
    } else {
        die "Handle more coercion cases";
    }
}


sub _jit_emit_op {
    my ($self, $fun, $ast) = @_;

    given ($ast->op_class) {
        when (pj_opc_binop) {
            my $v1 = $self->_jit_emit($fun, $ast->get_left_kid);
            my $v2 = $self->_jit_emit($fun, $ast->get_right_kid);
            my $res;

            given ($ast->get_optype) {
                when (pj_binop_add) {
                    $res = jit_insn_add($fun, $self->_to_nv($fun, $v1), $self->_to_nv($fun, $v2));
                }
                when (pj_binop_subtract) {
                    $res = jit_insn_sub($fun, $self->_to_nv($fun, $v1), $self->_to_nv($fun, $v2));
                }
                when (pj_binop_multiply) {
                    $res = jit_insn_mul($fun, $self->_to_nv($fun, $v1), $self->_to_nv($fun, $v2));
                }
                when (pj_binop_divide) {
                    $res = jit_insn_div($fun, $self->_to_nv($fun, $v1), $self->_to_nv($fun, $v2));
                }
                default {
                    die "I'm sorry, I've not been implemented yet";
                }
            }

            # this should be a pj_binop_<xxx>_assign; or maybe better a flag
            if ($ast->get_perl_op->flags & OPf_STACKED) {
                pa_sv_set_nv($fun, $v1, $res);
            }

            return $res;
        }
        when (pj_opc_unop) {
            my $v1 = $self->_jit_emit($fun, $ast->get_kid);
            my $res;

            given ($ast->get_optype) {
                when (pj_unop_negate) {
                    $res = jit_insn_neg($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_abs) {
                    $res = jit_insn_abs($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_sin) {
                    $res = jit_insn_sin($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_cos) {
                    $res = jit_insn_cos($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_sqrt) {
                    $res = jit_insn_sqrt($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_log) {
                    $res = jit_insn_log($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_exp) {
                    $res = jit_insn_exp($fun, $self->_to_nv($fun, $v1));
                }
                when (pj_unop_bool_not) {
                    $res = jit_insn_to_not_bool($fun, $self->_to_numeric_type($fun, $v1));
                }
                default {
                    die "I'm sorry, I've not been implemented yet";
                }
            }

            return $res;
        }
        default {
            die "I'm sorry, I've not been implemented yet";
        }
    }
}

sub _jit_emit_const {
    my ($self, $fun, $ast) = @_;

    given ($ast->const_type) {
        when (pj_double_type) {
            return jit_value_create_NV_constant($fun, $ast->get_dbl_value);
        }
        when (pj_int_type) {
            return jit_value_create_IV_constant($fun, $ast->get_int_value);
        }
        when (pj_uint_type) {
            return jit_value_create_UV_constant($fun, $ast->get_uint_value);
        }
    }
}

1;
