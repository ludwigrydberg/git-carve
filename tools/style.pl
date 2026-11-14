#!/usr/bin/perl
# SPDX-License-Identifier: MIT
#
# The code style clang-format does not check, for the files given:
#
# - Comments as in the kernel: a comment over several lines opens with
#   "/*" and closes with " */" on lines of their own, each line between
#   starts with " * "; no "//" comments but the SPDX line.
# - Local declarations in reverse xmas tree order: the declarations at
#   the start of a block go from the longest line to the shortest. A
#   declaration whose initializer uses a variable declared above it may
#   break the order (it has to come after that one).
#
# Prints "file:line: problem" for each problem; exits 1 if there is one.
use strict;
use warnings;

my $bad = 0;

sub problem {
	my ($f, $n, $msg) = @_;
	print "$f:$n: $msg\n";
	$bad = 1;
}

# Columns of a line, tabs at 8.
sub width {
	my ($s) = @_;
	my $col = 0;
	for my $c (split //, $s) {
		$col += $c eq "\t" ? 8 - $col % 8 : 1;
	}
	return $col;
}

# A line without its string and character literals.
sub code_of {
	my ($l) = @_;
	$l =~ s/"(?:[^"\\]|\\.)*"/""/g;
	$l =~ s/'(?:[^'\\]|\\.)*'/''/g;
	return $l;
}

sub check_comments {
	my ($f, $lines) = @_;
	my $in = 0;

	for my $i (0 .. $#$lines) {
		my $n = $i + 1;
		my $l = code_of($lines->[$i]);

		if ($in) {
			if ($l =~ m{\*/}) {
				$in = 0;
				problem($f, $n, "a comment's last line is \" */\" alone")
					unless $l =~ m{^\s*\*/\s*$};
			} elsif ($l !~ m{^\s*\*(\s|$)}) {
				problem($f, $n, "a comment's lines start with \" * \"");
			}
			next;
		}
		problem($f, $n, "a // comment")
			if $l =~ m{(^|[^:])//} && !($i == 0 && $l =~ m{^// SPDX});
		# comments that open on this line and do not close on it
		my $rest = $l;
		while ($rest =~ m{/\*}) {
			my $after = $';
			if ($after =~ m{\*/}) {
				$rest = $';
				next;
			}
			problem($f, $n, "a comment over several lines opens with " .
				"\"/*\" alone on its line")
				unless $l =~ m{^\s*/\*\s*$};
			$in = 1;
			last;
		}
	}
}

# Whether a line starts a local declaration (roughly: a type, then a
# name), not a statement.
sub is_decl {
	my ($l) = @_;
	return 0 unless $l =~ /^\t+((?:const|static|unsigned|signed|struct|enum|union|volatile|register|long|short)\s+)*[A-Za-z_]\w*(\s+\**\s*|\s*\*+\s*)[A-Za-z_(]/;
	my ($w) = $l =~ /^\s*(\w+)/;
	return 0 if $w =~ /^(return|if|while|for|switch|case|goto|else|do|sizeof|break|continue|default)$/;
	return 0 if $l =~ /^\s*[A-Za-z_]\w*\s*(=|\(|\[|->|\.|\+\+|--|[-+*\/|&^%]=)/;
	return 0 if $l =~ /:\s*$/;
	return 1;
}

# The names a declaration declares.
sub names_of {
	my ($t) = @_;
	my $c = code_of($t);
	$c =~ s/;\s*$//;
	# the initializers out, at the top level
	my ($out, $depth, $skip) = ('', 0, 0);
	for my $ch (split //, $c) {
		if ($ch =~ /[({\[]/) { $depth++; }
		if ($ch =~ /[)}\]]/) { $depth--; next if $skip; }
		if ($depth == 0 && $ch eq '=') { $skip = 1; next; }
		if ($depth == 0 && $ch eq ',') { $skip = 0; $out .= ','; next; }
		$out .= $ch unless $skip || $depth;
	}
	my @n;
	for my $seg (split /,/, $out) {
		my @w = $seg =~ /([A-Za-z_]\w*)/g;
		push @n, $w[-1] if @w;
	}
	return @n;
}

sub init_of {
	my ($t) = @_;
	my $c = code_of($t);
	return $c =~ /[^=!<>]=(?!=)(.*)/s ? $1 : '';
}

sub check_rxt {
	my ($f, $lines) = @_;
	my $i = 0;

	while ($i < @$lines) {
		my $l = $lines->[$i++];
		next unless $l =~ /\{\s*$/;
		next if $l =~ m{^\s*(/\*|\*)};
		next unless $l eq '{' || $l =~ /\)\s*\{$/ ||
			    $l =~ /^\t+(else|do) \{$/ || $l =~ /^\t+\{$/ ||
			    $l =~ /^\t+(case .*|default): \{$/;
		next if $l =~ /\b(struct|union|enum)\b[^(]*\{$/ ||
			$l =~ /=\s*\{$/;
		# the declarations right after it
		my (@decl, @names);
		my $j = $i;
		while ($j < @$lines) {
			my $d = $lines->[$j];
			last if $d !~ /\S/;
			if ($d =~ m{^\s*/\*}) {
				$j++ while $j < @$lines && $lines->[$j] !~ m{\*/};
				$j++;
				next;
			}
			last unless is_decl($d);
			my ($text, $depth) = ('', 0);
			my $start = $j;
			while ($j < @$lines) {
				my $c = code_of($lines->[$j]);
				$text .= $lines->[$j] . "\n";
				$depth += () = $c =~ /[({]/g;
				$depth -= () = $c =~ /[)}]/g;
				$j++;
				last if $depth <= 0 && $c =~ /;\s*$/;
			}
			my $init = init_of($text);
			my $uses = 0;
			for my $n (@names) {
				$uses = 1 if $init =~ /\b\Q$n\E\b/;
			}
			push @decl, { n => $start + 1, w => width($lines->[$start]),
				      uses => $uses };
			push @names, names_of($text);
		}
		# the order of those that use none of the others
		my $prev;
		for my $d (grep { !$_->{uses} } @decl) {
			problem($f, $d->{n}, "declarations go from the longest " .
				"line to the shortest (reverse xmas tree)")
				if defined $prev && $d->{w} > $prev;
			$prev = $d->{w};
		}
	}
}

for my $f (@ARGV) {
	open my $fh, '<', $f or die "$f: $!";
	my @lines = <$fh>;
	close $fh;
	chomp @lines;
	check_comments($f, \@lines);
	check_rxt($f, \@lines) if $f =~ /\.c$/;
}
exit $bad;
