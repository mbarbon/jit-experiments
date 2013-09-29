package # hide from PAUSE
  Perl::JIT::ASTTest;
use v5.14;
use warnings;
# This is to make given/when work on 5.14 and 5.18. *sigh*, and it
# needs to be after 'use Moo'
no warnings $] < 5.018 ? 'redefine' : 'experimental';
use warnings 'redefine';

use Exporter 'import';

use Perl::JIT qw(:all);
use Test::Simple;

our @EXPORT = (qw(
  ast_anything
  ast_constant
  ast_lexical
  ast_statement
  ast_statementsequence
  ast_unop
  ast_binop
  ast_listop
  ast_block

  ast_contains
), @Perl::JIT::EXPORT_OK);

sub _matches {
  my ($ast, $pattern) = @_;

  given ($pattern->{type}) {
    when ('anything') { return 1 }
    when ('constant') {
      return 0 unless $ast->get_type == pj_ttype_constant;
      return 0 if defined $pattern->{value_type} &&
                  $ast->const_type != $pattern->{value_type};

      given ($ast->const_type) {
        when (pj_double_type) {
          return $ast->get_dbl_value == $pattern->{value};
        }
        when (pj_int_type) {
          return $ast->get_int_value == $pattern->{value};
        }
        when (pj_uint_type) {
          return $ast->get_uint_value == $pattern->{value};
        }
        default {
          die "Unhandled constant type in pattern" ;
        }
      }
    }
    when ('lexical') {
      return 0 unless $ast->get_type == pj_ttype_lexical || $ast->get_type == pj_ttype_variabledeclaration;
      return 0 unless $ast->get_sigil == $pattern->{sigil};

      return 1;
    }
    when ('statement') {
      return 0 unless $ast->get_type == pj_ttype_statement;
      return _matches($ast->get_kid, $pattern->{term});
    }
    when ('statementsequence') {
      return 0 unless $ast->get_type == pj_ttype_statementsequence;
      my @kids = $ast->get_kids;
      return 0 unless @kids == @{$pattern->{statements}};

      for my $i (0 .. $#kids) {
        return 0 if !_matches($kids[$i], $pattern->{statements}[$i]);
      }

      return 1;
    }
    when ('unop') {
      return 0 unless $ast->get_type == pj_ttype_op;
      return 0 unless $ast->op_class == pj_opc_unop;
      return 0 unless $ast->get_optype == $pattern->{op};

      return _matches($ast->get_kid, $pattern->{term});
    }
    when ('binop') {
      return 0 unless $ast->get_type == pj_ttype_op;
      return 0 unless $ast->op_class == pj_opc_binop;
      return 0 unless $ast->get_optype == $pattern->{op};

      return _matches($ast->get_left_kid, $pattern->{left}) &&
             _matches($ast->get_right_kid, $pattern->{right});
    }
    when ('listop') {
      return 0 unless $ast->get_type == pj_ttype_op;
      return 0 unless $ast->op_class == pj_opc_listop;
      return 0 unless $ast->get_optype == $pattern->{op};
      my @kids = $ast->get_kids;
      return 0 unless @kids == @{$pattern->{terms}};

      for my $i (0 .. $#kids) {
        return 0 if !_matches($kids[$i], $pattern->{terms}[$i]);
      }

      return 1;
    }
    when ('block') {
      return 0 unless $ast->get_type == pj_ttype_op;
      return 0 unless $ast->op_class == pj_opc_block;
      my @kids = $ast->get_kids;
      die "Blocks can only have one kid" if @kids != 1;
      return _matches($kids[0], $pattern->{body});
    }
  }
}

sub _contains {
  my ($ast, $pattern) = @_;
  my @queue = $ast;

  while (@queue) {
    my $term = shift @queue;

    return 1 if _matches($term, $pattern);
    push @queue, $term->get_kids;
  }

  return 0;
}

sub ast_contains {
  my ($sub, $pattern, $diag) = @_;
  local $Test::Builder::Level = $Test::Builder::Level + 1;
  my @asts = Perl::JIT::find_jit_candidates($sub);
  for my $ast (@asts) {
    return ok(1, $diag) if _contains($ast, $pattern);
  }

  $_->dump for @asts;
  ok(0, $diag);
}

sub ast_anything {
  return {type => 'anything'};
}

sub ast_constant {
  my ($value, $type) = @_;

  return {type => 'constant', value => $value, value_type => $type};
}

sub ast_lexical {
  my ($name) = @_;
  die "Invalid name '$name'" unless $name =~ /^([\$\@\%])(.+)$/;
  my $sigil = $1 eq '$' ? pj_sigil_scalar :
              $1 eq '@' ? pj_sigil_array :
                          pj_sigil_hash;

  return {type => 'lexical', sigil => $sigil, name => $2};
}

sub ast_statement {
  my ($term) = @_;

  return {type => 'statement', term => $term};
}

sub _force_statements {
  my ($maybe_statements) = @_;

  return [
    map {
      $_->{type} eq 'anything' || $_->{type} eq 'statement' ? $_ : ast_statement($_)
    } @$maybe_statements
  ];
}

sub ast_statementsequence {
  my ($statements) = @_;

  return {type => 'statementsequence', statements => _force_statements($statements)};
}

sub ast_unop {
  my ($op, $term) = @_;

  return {type => 'unop', op => $op, term => $term};
}

sub ast_binop {
  my ($op, $left, $right) = @_;

  return {type => 'binop', op => $op, left => $left, right => $right};
}

sub ast_listop {
  my ($op, $terms) = @_;

  return {type => 'listop', op => $op, terms => $terms};
}

sub ast_block {
  my ($body) = @_;

  return {type => 'block', body => $body};
}

package t::lib::Perl::JIT::ASTTest;

use strict;
use warnings;
use parent 'Test::Builder::Module';

use Test::More;
use Test::Differences;
use Perl::JIT;

Perl::JIT::ASTTest->import;

our @EXPORT = (
  @Test::More::EXPORT,
  @Test::Differences::EXPORT,
  @Perl::JIT::ASTTest::EXPORT,
);

sub import {
    unshift @INC, 't/lib';

    strict->import;
    warnings->import;

    goto &Test::Builder::Module::import;
}

1;