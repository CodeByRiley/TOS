#!/usr/bin/env perl
use strict;
use warnings;

my ($input, $output) = @ARGV;
die "usage: $0 INPUT OUTPUT\n" unless defined $input && defined $output;

open my $in, '<:encoding(UTF-8)', $input or die "$input: $!\n";
open my $out, '>:encoding(UTF-8)', $output or die "$output: $!\n";

print {$out} "# Generated from NetSurf FatMessages.\n\n";
while (my $line = <$in>) {
    next unless $line =~ /^en\.(all|fb)\.([^:]+):(.*)$/;
    print {$out} "$2:$3\n";
}
