#!/usr/bin/env perl

use t::lib::Perl::JIT::Test;

my @tests = (
  { name   => 'addition sequence',
    func   => build_jit_test_sub('$x', '$x += 30; $x += 5', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  { name   => 'mixed jittable/non-jittable',
    func   => build_jit_test_sub('$x', '$x += 30; srand(1); $x += 5', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  { name   => 'mixed jittable/non-jittable',
    func   => build_jit_test_sub('$x', 'for (1..35) { $x += 1 }', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  # test that variable optimization does not change the logic
  { name   => 'addition sequence - typed',
    func   => build_jit_typed_test_sub('Double', '$x', '$x += 30; $x += 5', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  { name   => 'mixed jittable/non-jittable - typed',
    func   => build_jit_typed_test_sub('Double', '$x', '$x += 30; srand(1); $x += 5', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  { name   => 'mixed jittable/non-jittable - typed',
    func   => build_jit_typed_test_sub('Double', '$x', 'for (1..35) { $x += 1 }', '$x'),
    opgrep => [{ name => 'nextstate', sibling => { name => 'add' } }],
    input  => [7], },
  { name   => 'logical or and assignment - not assigned',
    func   => build_jit_typed_test_sub('Double', '$x, $y', 'my $z = ($x = 1) || ($y = 2)', '$y'),
    opgrep => [{ name => 'or' }, { name => 'sassign' }],
    input  => [1, 42], },
  { name   => 'logical or and assignment - assigned',
    func   => build_jit_typed_test_sub('Double', '$x, $y', 'my $z = ($x = 0) || ($y = 42)', '$y'),
    opgrep => [{ name => 'or' }, { name => 'sassign' }],
    input  => [1, 2], },
  { name   => 'logical and and assignment - not assigned',
    func   => build_jit_typed_test_sub('Double', '$x, $y', 'my $z = ($x = 0) && ($y = 2)', '$y'),
    opgrep => [{ name => 'and' }, { name => 'sassign' }],
    input  => [1, 42], },
  { name   => 'logical and and assignment - assigned',
    func   => build_jit_typed_test_sub('Double', '$x, $y', 'my $z = ($x = 1) && ($y = 42)', '$y'),
    opgrep => [{ name => 'and' }, { name => 'sassign' }],
    input  => [1, 42], },
  { name   => 'ternary and assignment',
    func   => build_jit_typed_test_sub('Double', '$x, $y', 'my $z = ($x = 0) ? ($y = 2) : ($x = 2)', '$y + $x'),
    opgrep => [{ name => 'cond_expr' }, { name => 'sassign' }],
    input  => [1, 40], },
);

# save typing
$_->{output} = 42 for @tests;

plan tests => count_jit_tests(\@tests);

run_jit_tests(\@tests);
