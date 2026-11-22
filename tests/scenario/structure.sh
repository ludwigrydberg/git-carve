#!/bin/sh
# SPDX-License-Identifier: MIT
# The rules for new code (CONTRIBUTING.md) that a script can check, on the
# built objects and the sources. An allow-list holds what is known and
# still to be fixed, and it may only shrink.
. "$(dirname "$0")/../lib.sh"
top=$(cd "$TESTS_DIR/.." && pwd)
new_repo	# $tmp
cd "$top" || fail "cd $top"
ls src/git.o src/ui/ui.o >/dev/null 2>&1 || fail "build first: no objects"
bad=0
problem() {
	echo "structure: $*" >&2
	bad=1
}

# --- the layers, util < git < diff, status < patch, index, rename,
# conflict, subst < opstate, rebase, cdiff, message (what operations share)
# < the operations < the frontends. A module uses its own layer or lower
# ones, and the modules do not use each other in a cycle.
rank() {
	case $1 in
	util) echo 0 ;;
	git) echo 1 ;;
	diff | status) echo 2 ;;
	patch | index | rename | conflict | subst) echo 3 ;;
	opstate | rebase | cdiff | message) echo 4 ;;
	split | series | move | undo | absorb | gc | ops) echo 5 ;;
	*) echo 9 ;;
	esac
}
# edges still to remove, none now, and an entry needs a reason
allowed_edges=''
for o in src/*.o src/ui/*.o src/cli/*.o; do
	m=$(echo "$o" | sed 's,^src/,,; s,\.o$,,')
	nm -g "$o" | awk -v m="$m" '$2 ~ /^[TDBSC]$/ {
		s = $3; sub(/^_/, "", s); print "D", m, s }'
	nm -u "$o" | awk -v m="$m" '{ s = $NF; sub(/^_/, "", s); print "U", m, s }'
done | awk '$1 == "D" { def[$3] = $2; next } { use[++n] = $2 " " $3 }
	END { for (i = 1; i <= n; i++) { split(use[i], a, " ")
		if ((a[2] in def) && def[a[2]] != a[1]) e[a[1] " " def[a[2]]] = 1 }
		for (k in e) print k }' | sort >"$tmp/edges"
[ "$(wc -l <"$tmp/edges")" -gt 30 ] || fail "too few edges: $(cat "$tmp/edges")"
while read -r from to; do
	rf=$(rank "$from")
	[ "$rf" = 9 ] && continue
	if [ "$(rank "$to")" -gt "$rf" ]; then
		echo "$allowed_edges" | grep -qx "$from $to" ||
			problem "$from uses $to, a higher layer"
	fi
done <"$tmp/edges"
[ -n "$allowed_edges" ] && echo "$allowed_edges" | while read -r from to; do
	grep -qx "$from $to" "$tmp/edges" ||
		echo "structure: $from no longer uses $to: drop it from" \
			"allowed_edges" >&2
done
# tsort reports longer cycles
grep -v '^ui/\|^cli\|^main' "$tmp/edges" |
	grep -vxF "$allowed_edges" | tsort >/dev/null 2>"$tmp/tsort"
[ -s "$tmp/tsort" ] && problem "a cycle between core modules: $(cat "$tmp/tsort")"

# --- only git.c starts processes
grep -nE '(^|[^a-z_.>])(posix_spawnp?|fork|vfork|execv[pe]*|execl[pe]*|popen|system) *\(' \
	src/*.c src/ui/*.c src/cli/*.c | grep -v '^src/git\.c:' >"$tmp/spawn" &&
	problem "a process started outside git.c: $(cat "$tmp/spawn")"

# --- the frontends ask the core, not git. The exceptions are the two
# background runs, the untracked scan and the diff cache, which start git
# with arguments the core builds and read it while keys come.
allowed_git='src/ui/dcache.c 1
src/ui/state.c 1'
for f in src/ui/*.c src/cli/*.c src/main.c; do
	n=$(grep -cE '(^|[^a-z_])(git_out|git_quiet|git_line|git_status_of|git_env|git_in|run_git|bg_start)\(' "$f")
	max=$(echo "$allowed_git" | awk -v f="$f" '$1 == f { print $2 }')
	[ "$n" -le "${max:-0}" ] ||
		problem "$f runs git $n times itself (allowed ${max:-0})"
done

# --- no mutable state in the core (zero-initialized data)
for o in src/*.o; do
	case $o in src/main.o) continue ;; esac
	# (the counters of a coverage build and the markers of the address
	# sanitizer are not the code's)
	nm "$o" | awk '$2 ~ /^[BbCc]$/ { print $3 }' |
		grep -v -e '^ltmp' -e gcov -e '__llvm' -e '__profc' \
			-e '_asan[._]' |
		sed "s,^,$o: ," >>"$tmp/globals"
done
[ -s "$tmp/globals" ] && problem "mutable state in the core: $(cat "$tmp/globals")"

# --- options that make git's output parseable are in git.h only
grep -n 'core\.quotePath' src/*.c src/*.h src/ui/*.c src/ui/*.h src/cli/*.[ch] |
	grep -v '^src/git\.h:' >"$tmp/plain" &&
	problem "use GIT_PLAIN: $(cat "$tmp/plain")"

# --- one name, one helper, no static function name in two files
for f in src/*.c src/ui/*.c src/cli/*.c; do
	sed -n 's/^static [^(=]*[ *]\([a-z_][a-z0-9_]*\)(.*/\1/p' "$f" | sort -u
done | sort | uniq -d >"$tmp/statics"
[ -s "$tmp/statics" ] && problem "static functions of the same name in" \
	"two files (share one, or name them apart): $(cat "$tmp/statics")"

# --- test helpers live in the libraries
grep -h -o '^[a-z_]*()' tests/*.sh tests/*/*.sh | sort | uniq -d \
	>"$tmp/helpers"
[ -s "$tmp/helpers" ] && problem "a test helper defined twice (move it to" \
	"tests/lib.sh): $(cat "$tmp/helpers")"

# --- one text, one place. A string of 25 characters or more written twice
# is a #define, though git arguments that read the same are fine.
cat src/*.c src/ui/*.c src/cli/*.c | grep -ohE '"[^"]{25,}"' | sort | uniq -d |
	grep -vF -e '"--decorate-refs=' -e ') && saved && ' >"$tmp/dups"
[ -s "$tmp/dups" ] && problem "the same text twice (make it a #define):" \
	"$(cat "$tmp/dups")"

# --- a global function has a user outside its file (or a unit test)
cat tests/unit/*.c tests/fuzz/*.c | grep -oE '[a-z_][a-z0-9_]*\(' |
	tr -d '(' | sort -u >"$tmp/testuses"
for o in src/*.o src/ui/*.o src/cli/*.o; do
	m=$(echo "$o" | sed 's,^src/,,; s,\.o$,,')
	nm -g "$o" | awk -v m="$m" '$2 == "T" { s = $3; sub(/^_/, "", s)
		print "D", m, s }'
	nm -u "$o" | awk -v m="$m" '{ s = $NF; sub(/^_/, "", s); print "U", m, s }'
done | awk 'NR == FNR { t[$1] = 1; next }
	$1 == "D" { def[$3] = $2; next } { if (!(($3) in used)) used[$3] = ""
		used[$3] = used[$3] " " $2 }
	END { for (s in def) { u = used[s]; gsub(" " def[s] "( |$)", " ", u)
		if (u !~ /[a-z]/ && !(s in t) && s != "main")
			print def[s] ": " s } }' "$tmp/testuses" - |
	sort >"$tmp/unused"
[ -s "$tmp/unused" ] && problem "global but used only in its own file" \
	"(make it static) or nowhere: $(cat "$tmp/unused")"

# --- functions stay small, at most 150 lines, except the diff parser, a
# state machine with golden tests
allowed_long='diff_parse'
for f in src/*.c src/ui/*.c src/cli/*.c; do
	awk -v F="$f" '
	/^[A-Za-z_].*\(/ && !/;[ \t]*$/ { sig = $0 }
	/^\{/ { if (sig != "") { start = NR; name = sig; infn = 1 } next }
	infn && /^\}/ { n = split(name, p, "("); k = split(p[1], q, /[ \t*]+/)
		if (NR - start - 1 > 150) print q[k], NR - start - 1, F
		infn = 0; sig = "" }' "$f"
done >"$tmp/long"
while read -r name lines file; do
	echo "$allowed_long" | grep -qx "$name" ||
		problem "$file: $name has $lines lines (at most 150)"
done <"$tmp/long"

# --- every key action does something, through a function in action_fn
# (src/ui/ui.c), except quit, which handle_key answers itself
grep -o 'ACT_[A-Z_]*' src/ui/keys.h | sort -u >"$tmp/acts"
awk '/action_fn\[NACTIONS\]/,/^};/' src/ui/ui.c | grep -o 'ACT_[A-Z_]*' |
	sort -u >"$tmp/tbl"
comm -23 "$tmp/acts" "$tmp/tbl" | grep -vx 'ACT_NONE\|ACT_PREFIX\|ACT_QUIT' \
	>"$tmp/noact" && problem "actions without a function in action_fn:" \
	"$(cat "$tmp/noact")"

# --- refusals read "reason; what to do", with one "; " at most, no full
# stop, and lower case, though a name such as HEAD may start it
perl -e 'undef $/;
for my $f (@ARGV) {
	open my $h, "<", $f or die; my $s = <$h>;
	while ($s =~ /\b(?:repo_err\(\s*[a-z.>-]+,|set_msg\(true,)\s*((?:"(?:[^"\\]|\\.)*"\s*)+)/g) {
		my $lit = $1; my $line = 1 + (substr($s, 0, $-[0]) =~ tr/\n//);
		my $t = join "", ($lit =~ /"((?:[^"\\]|\\.)*)"/g);
		my @semi = ($t =~ /; /g);
		print "$f:$line: two \"; \": $t\n" if @semi > 1;
		print "$f:$line: a full stop: $t\n" if $t =~ /\.$/;
		print "$f:$line: upper case: $t\n" if $t =~ /^[A-Z][a-z]/;
	}
}' src/*.c src/ui/*.c src/cli/*.c >"$tmp/refusals"
[ -s "$tmp/refusals" ] && problem "refusal texts: $(cat "$tmp/refusals")"

# --- dropped features are gone from the code and the user documentation
# (gc.c cleans up what they left in repositories)
grep -rnI -iE 'carve review|carve check|checkCmd|checkDir|REVIEW [0-9]' \
	src tests README.md CONTRIBUTING.md docs/*.1 \
	docs/KPI.md 2>/dev/null |
	grep -v -e '^src/gc\.c:' -e '^tests/scenario/gc' \
		-e '^tests/scenario/cleanup\.sh:' \
		-e '^tests/scenario/structure\.sh:' >"$tmp/dropped" &&
	problem "a dropped feature is still named: $(cat "$tmp/dropped")"

[ "$bad" = 0 ] || fail "structure rules broken (CONTRIBUTING.md)"
exit 0
