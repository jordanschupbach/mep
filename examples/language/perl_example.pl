#!/usr/bin/env perl
# A small Perl tour: a hash-based object, list utilities, regexes, and a hash
# of counts.
use strict;
use warnings;
use List::Util qw(sum);

package Point {
    sub new {
        my ($class, %args) = @_;
        return bless { x => $args{x}, y => $args{y} }, $class;
    }

    sub distance_to {
        my ($self, $other) = @_;
        return sqrt(($self->{x} - $other->{x})**2 + ($self->{y} - $other->{y})**2);
    }
}

print "hello, world\n";

my $a = Point->new(x => 0, y => 0);
my $b = Point->new(x => 3, y => 4);
printf "distance = %.2f\n", $a->distance_to($b);

my @nums = (1 .. 5);
print "sum = ", sum(@nums), "\n";
print "squares = ", join(", ", map { $_ * $_ } @nums), "\n";
print "evens = @{[ grep { $_ % 2 == 0 } @nums ]}\n";

my %by_letter;
for my $word (qw(apple banana avocado cherry)) {
    my ($first) = $word =~ /^(\w)/;
    $by_letter{$first}++;
}
print "$_: $by_letter{$_}\n" for sort keys %by_letter;
