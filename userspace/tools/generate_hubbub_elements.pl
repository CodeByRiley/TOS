use strict;
use warnings;

my ($input_path, $output_path) = @ARGV;
die "usage: $0 INPUT OUTPUT\n" unless defined $output_path;

open my $input, '<', $input_path or die "cannot open $input_path: $!\n";

my @elements;
my $in_table = 0;
while (my $line = <$input>) {
	if ($line =~ /^%%\s*$/) {
		$in_table = !$in_table;
		next;
	}
	next unless $in_table;
	next unless $line =~ /^\s*([^,\s]+)\s*,\s*(\w+)\s*$/;
	push @elements, [$1, $2];
}
close $input;

die "no elements found in $input_path\n" unless @elements;
open my $output, '>', $output_path or die "cannot open $output_path: $!\n";

print {$output} "static const struct element_type_map wordlist[] = {\n";
for my $element (@elements) {
	print {$output} "\t{\"$element->[0]\", $element->[1]},\n";
}
print {$output} <<'C_SOURCE';
};

static int hubbub_element_name_equal(const char *left, const char *right,
		size_t length)
{
	for (size_t i = 0; i < length; i++) {
		unsigned char a = (unsigned char)left[i];
		unsigned char b = (unsigned char)right[i];
		if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
		if (a != b) return 0;
	}
	return right[length] == '\0';
}

static const struct element_type_map *
hubbub_element_type_generated_lookup(const char *name, size_t length)
{
	for (size_t i = 0; i < sizeof(wordlist) / sizeof(wordlist[0]); i++) {
		if (hubbub_element_name_equal(name, wordlist[i].name, length)) {
			return &wordlist[i];
		}
	}
	return NULL;
}
C_SOURCE

close $output;
