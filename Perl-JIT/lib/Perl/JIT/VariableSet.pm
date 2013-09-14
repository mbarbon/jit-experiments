package Perl::JIT::VariableSet;

use Moo;

has _read    => ( is => 'ro' );
has _written => ( is => 'ro' );

sub BUILD {
    my ($self) = @_;

    $self->{_read} = {};
    $self->{_written} = {};
}

sub add_read {
    my ($self, $variable) = @_;

    $self->_read->{$variable} = 1;
}

sub add_written {
    my ($self, $variable) = @_;

    $self->_written->{$variable} = 1;
}

sub read {
    my ($self) = @_;

    return values ${$self->_read};
}

sub written {
    my ($self) = @_;

    return values ${$self->_written};
}

sub is_read {
    my ($self, $variable) = @_;

    return $self->_read->{$variable};
}

sub is_written {
    my ($self, $variable) = @_;

    return $self->_written->{$variable};
}

sub merge {
    my ($self, $other) = @_;

    $self->add_read($_) for $other->read;
    $self->add_written($_) for $other->written;
}

1;
