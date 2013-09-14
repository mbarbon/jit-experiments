package Perl::JIT::VariableUse;

use Moo;

has _variables => ( is => 'ro' );

sub BUILD {
    my ($self) = @_;

    $self->{_variables} = {};
}

sub _set {
    my ($self, $variable, $state) = @_;

    $self->_variables->{$variable->get_pad_index} = [$variable, $state];
}

sub _get {
    my ($self, $variable) = @_;
    my $info = $self->_variables->{$variable->get_pad_index};

    return $info ? $info->[1] : 0;
}

sub _grep {
    my ($self, $state) = @_;

    return map  { $_->[0] }
           grep { $_->[1] == $state }
                values %{$self->_variables};
}

sub set_fresh { $_[0]->_set($_[1], 1) }
sub set_dirty { $_[0]->_set($_[1], 2) }
sub set_stale { $_[0]->_set($_[1], 3) }

sub is_set   { $_[0]->_get($_[1]) != 0 }
sub is_fresh { $_[0]->_get($_[1]) == 1 }
sub is_dirty { $_[0]->_get($_[1]) == 2 }
sub is_stale { $_[0]->_get($_[1]) == 3 }

sub fresh { $_[0]->_grep(1) }
sub dirty { $_[0]->_grep(2) }
sub stale { $_[0]->_grep(3) }

1;
