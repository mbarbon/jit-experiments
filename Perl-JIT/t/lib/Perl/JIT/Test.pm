package # hide from PAUSE
  Perl::JIT::Test;
use strict;
use warnings;
use Capture::Tiny qw(capture);

use Config '%Config';
use Exporter 'import';
use Test::More;
use Data::Dumper;
use B;
use B::Utils qw(walkoptree_filtered opgrep);
use Perl::JIT::Emit;

our @EXPORT = qw(
  runperl_output_is
  runperl_output_like
  runperl_output
  is_approx
  run_ctest
  is_jitting
  run_jit_tests
  count_jit_tests
);

SCOPE: {
  my $in_testdir = not(-d 't');
  my $base_dir;
  my $ctest_dir;
  if ($in_testdir) {
    $base_dir = File::Spec->updir;
    #$USE_VALGRIND = -e File::Spec->catfile(File::Spec->updir, 'USE_VALGRIND');
    #$USE_GDB = -e File::Spec->catfile(File::Spec->updir, 'USE_GDB');
  }
  else {
    $base_dir = File::Spec->curdir;
    #$USE_VALGRIND = -e 'USE_VALGRIND';
    #$USE_GDB = -e 'USE_GDB';
  }
  $ctest_dir = File::Spec->catdir($base_dir, 'ctest');

  my @ctests = glob( "$ctest_dir/*.c" );
  my @exe = grep -f $_, map {s/\.c$/$Config{exe_ext}/; $_} @ctests;

  sub _locate_exe {
    my $exe = shift;
    return $exe if -f $exe;
    my $inctest = File::Spec->catfile($ctest_dir, $exe);
    return $inctest if -f $inctest;
    return;
  }

  sub run_ctest {
    my ($executable, $options) = @_;
    my $to_run = _locate_exe($executable);
    return if not defined $to_run;

    #my ($stdout, $stderr) = capture {
      my @cmd;
      #if ($USE_VALGRIND) {
      #  push @cmd, "valgrind", "--suppressions=" .  File::Spec->catfile($base_dir, 'perl.supp');
      #}
      #elsif ($USE_GDB) {
      #  push @cmd, "gdb";
      #}
      push @cmd, $to_run, ref($options)?@$options:();
      note("@cmd");
      system(@cmd)
        and fail("C test did not exit with 0");
    #};
    #print $stdout;
    #warn $stderr if defined $stderr and $stderr ne '';
    return 1;
  } # end run_ctest()
} # end SCOPE

sub runperl_output_is {
  my ($cmd, $ref, $name) = @_;
  my $stdout = _runperl_exec_ok($cmd, $name);
  chomp $stdout;
  is($stdout, $ref, $name);
}

sub runperl_output_like {
  my ($cmd, $ref, $name) = @_;
  my $stdout = _runperl_exec_ok($cmd, $name);
  like($stdout, $ref, $name);
}

sub runperl_output {
  my ($cmd, $name) = @_;
  my $stdout = _runperl_exec_ok($cmd, $name);
  return $stdout;
}

sub _runperl_exec_ok {
  my ($cmd, $name) = @_;

  my ($stdout, $stderr, $rc) = _runperl($cmd);
  is($rc, 0, "$name - Return code is 0")
    or diag("Apparently failed to run " . Data::Dumper::Dumper($cmd));
  $stderr =~ s/-e syntax OK\n?//;
  is($stderr, "", "$name - No output to STDERR")
    or diag("Apparently failed to run " . Data::Dumper::Dumper($cmd));

  return $stdout;
}

sub _runperl {
  my ($cmd) = @_;
  my $rc;
  my ($stdout, $stderr) = capture {
    $rc = system($^X, "-Mblib", (ref($cmd) ? @$cmd : $cmd));
  };
  return ($stdout, $stderr, $rc);
}

sub is_approx {
  my ($t, $ref, $name) = @_;
  ok($t + 1e-9 > $ref && $t - 1e9 < $ref, $name)
    or diag("$t appears to be different from $ref");
}

my $emit = Perl::JIT::Emit->new; # keep it around for now

sub is_jitting {
  my ($sub, $opgrep_patterns, $diag) = @_;
  local $Test::Builder::Level = $Test::Builder::Level + 1;

  for my $pat (@$opgrep_patterns) {
    my $matched;

    walkoptree_filtered(
      B::svref_2object($sub)->ROOT,
      sub { opgrep($pat, @_) },
      sub { $matched = 1 }
    );

    unless ($matched) {
      ok(0, "$diag: could not find op matching");
      diag(Dumper($pat));
      return 0;
    }
  }

  $emit->jit_sub($sub);

  for my $pat (@$opgrep_patterns) {
    my $matched;

    walkoptree_filtered(
      B::svref_2object($sub)->ROOT,
      sub { opgrep($pat, @_) },
      sub { $matched = 1 }
    );

    if ($matched) {
      ok(0, "$diag: some op has not been JITted");
      diag(Dumper($pat));
      return 0;
    }
  }

  return ok(1, $diag);
}

sub count_jit_tests {
  my $tests = shift;
  return 2 * @$tests;
}

sub run_jit_tests {
  my $tests = shift;

  foreach my $test (@$tests) {
    is_jitting($test->{func}, $test->{opgrep}, $test->{name});

    is_deeply( $test->{func}->(@{$test->{input}}),
               $test->{output},
               "$test->{name}: Checking output");
  }
}

package t::lib::Perl::JIT::Test;

use strict;
use warnings;
use parent 'Test::Builder::Module';

use Test::More;

Perl::JIT::Test->import;

our @EXPORT = (
  @Test::More::EXPORT,
  @Perl::JIT::Test::EXPORT,
);

sub import {
    unshift @INC, 't/lib';

    strict->import;
    warnings->import;

    goto &Test::Builder::Module::import;
}

1;
