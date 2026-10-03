#!/bin/sh
#
# Summarize gcov data for a build that was compiled with --coverage.
#
# Run from the project root, with the object directory as the only argument.
# Prints per-file line coverage, the project total, and - separately - the
# growth and resize lines that never executed.
#
# That last list is the point of the report. A dynamic structure whose
# reallocation path is never taken by any test is not covered by "all tests
# pass"; a defect of exactly that shape (a hash table that corrupted itself
# when it grew) survived a full green suite in this codebase.

set -eu

# The reduction, in a file so that the report and --self-test run the same
# text.  A quoted heredoc, so nothing in it is expanded by the shell.
COVERAGE_AWK="$(mktemp)"
COVERAGE_ALL=""
cleanup() {
  rm -f "$COVERAGE_AWK"
  if [ -n "$COVERAGE_ALL" ]; then rm -f "$COVERAGE_ALL"; fi
}
trap cleanup EXIT HUP INT TERM

cat > "$COVERAGE_AWK" <<'COVERAGE_AWK_EOF'

  {
    # Each line is "<count>:<lineno>:<text>", where the count is a number, a
    # dash for a non-executable line, or ##### for one never executed.
    # Line number 0 carries gcov's own tags, of which Source: names the file
    # the records that follow belong to.
    c1 = index($0, ":")
    if (c1 == 0) next
    count = substr($0, 1, c1 - 1)
    rest  = substr($0, c1 + 1)
    c2 = index(rest, ":")
    if (c2 == 0) next
    lineno = substr(rest, 1, c2 - 1) + 0
    text   = substr(rest, c2 + 1)
    gsub(/^[ \t]+|[ \t]+$/, "", count)

    # The tag has to be read from the parsed line number rather than from a
    # bare /Source:/ match: with every object's report in one stream the
    # source changes many times, and a /Source:/ anywhere in a line of C would
    # otherwise be taken for a new file.
    if (lineno == 0) {
      if (text ~ /^Source:/) {
        src = substr(text, 8)
        sub(/^[ \t]+/, "", src)
        # Only report on the library itself, not tests or system headers.
        if (src !~ /^src\//) { src = "SKIP" }
      }
      next
    }
    if (src == "" || src == "SKIP") { next }
    if (count == "-") { next }

    # A header included more than once per object appears repeatedly; a line
    # counts as executed if any instantiation reached it.
    #
    # gcov marks a line it never ran "#####", or "=====" when the line is
    # reachable only by an exceptional path. This asked for "$$$$$", which is
    # not a marker gcov writes, so every "=====" line was counted as covered.
    key = src ":" lineno
    seen[key] = 1
    if (count != "#####" && count != "=====") {
      hit[key] = 1
    } else if (!(key in hit)) {
      body[key] = text
    }
    file_of[key] = src
  }

  END {
    for (key in seen) {
      f = file_of[key]
      total[f]++
      grand_total++
      if (key in hit) {
        covered[f]++
        grand_covered++
      } else {
        t = body[key]
        if (t ~ /realloc|capacity|[Gg]row|GROW|rehash|resize|reserve/) {
          split(key, parts, ":")
          gaps[f] = gaps[f] " " parts[2]
          gap_count++
        }
      }
    }

    printf "\n%-52s %8s %s\n", "FILE", "LINES", "COVERED"
    n = 0
    for (f in total) { files[++n] = f }
    # Simple insertion sort: least covered first, so the gaps lead.
    for (i = 2; i <= n; i++) {
      v = files[i]; rv = covered[v] / total[v]
      j = i - 1
      while (j >= 1 && (covered[files[j]] / total[files[j]]) > rv) {
        files[j + 1] = files[j]; j--
      }
      files[j + 1] = v
    }
    for (i = 1; i <= n; i++) {
      f = files[i]
      printf "%-52s %8d %6.1f%%\n", f, total[f], covered[f] * 100 / total[f]
    }
    pct = (grand_total ? grand_covered * 100 / grand_total : 0)
    printf "%-52s %8d %6.1f%%\n", "TOTAL", grand_total, pct

    if (gap_count > 0) {
      printf "\n%d growth/capacity lines never executed:\n", gap_count
      for (i = 1; i <= n; i++) {
        f = files[i]
        if (f in gaps) printf "  %s:%s\n", f, gaps[f]
      }
      printf "\nA reallocation path no test reaches is untested, not working.\n"
    } else {
      printf "\nEvery growth/capacity line was executed.\n"
    }

    # A floor, so that coverage can only be argued upward.  Off unless
    # COVERAGE_MIN is set, because the number is only meaningful for a full
    # instrumented run of the whole suite.
    if (min > 0) {
      if (pct + 0.05 < min) {
        printf "\ncoverage: %.1f%% is below the floor of %s%%\n", pct, min
        exit 1
      }
      printf "\ncoverage: %.1f%% meets the floor of %s%%\n", pct, min
    }
  }

COVERAGE_AWK_EOF

# A control for the reduction itself, because two of its rules were wrong for
# as long as it existed and the report said nothing either time.  Run as
# `tools/coverage.sh --self-test`; the feed below is what two objects' reports
# look like for one header, with a line each object reaches differently.
if [ "${1:-}" = "--self-test" ]; then
  feed="$(mktemp)"
  cat > "$feed" <<'SELFTEST_EOF'
        -:    0:Source:src/a.c
        1:    1:int a(void) {
    #####:    2:  return never();
        -:    3:}
        -:    0:Source:src/shared.h
        7:    1:  reached_by_a();
    #####:    2:  not_reached_by_a();
        -:    0:Source:src/b.c
        2:    1:int b(void) {
    =====:    2:  exception_only();
        -:    3:}
        -:    0:Source:src/shared.h
    #####:    1:  reached_by_a();
        3:    2:  not_reached_by_a();
        -:    0:Source:tests/t.cpp
    #####:    1:  not_counted();
SELFTEST_EOF
  out="$(awk -v min=0 -f "$COVERAGE_AWK" "$feed")"
  rm -f "$feed"
  fail=0
  check() {
    if printf '%s\n' "$out" | grep -qE "$2"; then
      printf 'ok    %s\n' "$1"
    else
      printf 'FAIL  %s\n' "$1" >&2
      fail=1
    fi
  }
  # Both of shared.h's lines are reached, each by a different object. The
  # version of this script that wrote one .gcov file per source saw only
  # whichever object gcov processed last, and reported it as 50%.
  check "a header's objects are merged" '^src/shared\.h +2 +100\.0%'
  # "=====" is gcov's marker for a line reached only by an exceptional path.
  # The script asked for "$$$$$", which gcov never writes, so those lines
  # were counted as covered: b.c read as 100% instead of 50%.
  check "an ===== line is not covered" '^src/b\.c +2 +50\.0%'
  check "a ##### line is not covered" '^src/a\.c +2 +50\.0%'
  # Six library lines, four of them reached.
  check "the total counts src/ only" '^TOTAL +6 +66\.7%'
  if printf '%s\n' "$out" | grep -q 'tests/t'; then
    printf 'FAIL  tests are left out\n' >&2
    fail=1
  else
    printf 'ok    tests are left out\n'
  fi
  if [ "$fail" -ne 0 ]; then
    printf '\ncoverage: the reduction does not do what the report claims\n' >&2
    exit 1
  fi
  printf '\ncoverage: self-test passed\n'
  exit 0
fi

OBJ_DIR="${1:?usage: coverage.sh <object-dir> | --self-test}"

if ! command -v gcov >/dev/null 2>&1; then
  echo "coverage: gcov not found (install gcc's gcov)" >&2
  exit 1
fi

GCDA=$(find "$OBJ_DIR" -name '*.gcda' 2>/dev/null || true)
if [ -z "$GCDA" ]; then
  echo "coverage: no profile data under $OBJ_DIR" >&2
  echo "coverage: the tests must run after an instrumented build" >&2
  exit 1
fi

# gcov resolves the Source: paths relative to the directory it runs in, so it
# is invoked from the project root with the object directory passed per file.
#
# -t sends each report to stdout instead of writing <mangled>.gcov files, and
# that is not a tidiness choice. A header with code in it - an inline function,
# a static table - is compiled into every translation unit that includes it,
# and gcov names its report after the *source* file, so each run overwrote the
# previous one and the report described whichever object happened to be
# processed last. brotli_bw.h read as 77.8% that way, from the half of its
# callers that never fill a buffer, while the other half covers the arm that
# does. Collecting the runs into one stream and merging them per line below is
# what makes a header's number mean the same thing a .c file's does.
COVERAGE_ALL="$(mktemp)"
for g in $GCDA; do
  gcov -p -r -t -o "$(dirname "$g")" "$g" >> "$COVERAGE_ALL" 2>/dev/null || true
done

status=0
awk -v min="${COVERAGE_MIN:-0}" -f "$COVERAGE_AWK" "$COVERAGE_ALL" || status=$?

if [ "${status:-0}" -ne 0 ]; then
  exit "$status"
fi
