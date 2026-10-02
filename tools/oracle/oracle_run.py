#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Compress.
#
# Ghoti.io Compress is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Run the oracle test binaries against pinned references, and count the skips.

    oracle_run.py <test-binary> [<test-binary>...]

**What this adds over `make test`.** The same binaries run in the ordinary suite
and pass there, against whatever the machine has. Here they run against the
versions `containers/IMAGES` names, and the gate is not only that they pass:

  **A skip is a failure in here.** On the host a skip means "this machine has no
  such reference", which is a fact about the machine. In a pinned image every
  reference is present by construction, so a skip means the binary could not
  reach something the image promises - and that reads identically to a green run
  in every summary line. A skip that reads as a pass is failing open; this is
  the same rule one layer in, where the thing that could fail open is a
  `GTEST_SKIP` rather than a shell `exit 0`.

  The library's own sentinels (`OracleIsActuallyAvailable`, one per reference)
  already fail when a reference is absent. This is the independent check on the
  same claim, and it is not redundant: a sentinel is code somebody remembered to
  write, and `test_lz4_spec_oracle.cpp` shipped one that answered for only one
  of its file's two references. Counting skips needs nobody to have remembered.

**Provenance, printed before the numbers.** Every pin is resolved and asked its
version *first*, so a run that gets as far as a test has already proved what
answered. That line is the difference between "the round trip passes" and "the
round trip passes against zstd 1.5.7".

Exit status is the verdict: nonzero if any binary fails, if any test skipped, or
if any reference could not be reached or did not match its pin.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle_env  # noqa: E402

# Which references each binary is allowed to need. Not used to gate anything -
# every pin is checked before any binary runs, because a partial check would let
# a run start against an unverified reference - but printed, so the line above a
# failure names the references that failure was measured against.
ALL = ["zstd", "liblz4", "libbrotli", "zlib", "gzip", "pyzstd"]

RAN = re.compile(r"^\[=+\] (\d+) tests? from \d+ test suites? ran", re.M)
PASSED = re.compile(r"^\[  PASSED  \] (\d+) tests?", re.M)
SKIPPED_N = re.compile(r"^\[  SKIPPED \] (\d+) tests?, listed below:", re.M)
SKIPPED_ONE = re.compile(r"^\[  SKIPPED \] ([A-Za-z_][\w.]*)\s*$", re.M)
FAILED_ONE = re.compile(r"^\[  FAILED  \] ([A-Za-z_][\w.]*)\s*$", re.M)


def main(argv):
    binaries = [a for a in argv if not a.startswith("-")]
    if not binaries:
        sys.stderr.write("oracle_run.py: no test binaries named\n")
        return 2

    try:
        sys.stdout.write(oracle_env.provenance(ALL) + "\n")
    except oracle_env.OracleUnavailable as problem:
        sys.stderr.write("oracle: %s\n" % problem)
        return 1
    sys.stdout.flush()

    # A writable directory for the tests' temporary files, named so that a
    # binary which writes somewhere it did not declare fails on a missing path
    # rather than into the image's own /tmp where nothing reads it. The tree is
    # mounted read-only, which is what makes that distinction real.
    scratch = os.path.join(oracle_env.ROOT, "build", "oracle", "scratch")
    os.makedirs(scratch, exist_ok=True)

    total_ran = total_passed = total_skipped = 0
    bad = []
    for binary in binaries:
        path = os.path.abspath(binary)
        if not os.path.exists(path):
            sys.stderr.write("oracle: not built: %s\n" % binary)
            return 2
        argv_out = oracle_env.command("zstd", argv=[path], scratch=scratch,
            env={"TMPDIR": scratch})
        finished = subprocess.run(argv_out, capture_output=True, text=True)
        out = finished.stdout + finished.stderr
        ran = int(RAN.search(out).group(1)) if RAN.search(out) else 0
        passed = int(PASSED.search(out).group(1)) if PASSED.search(out) else 0
        skipped = (int(SKIPPED_N.search(out).group(1))
            if SKIPPED_N.search(out) else 0)
        total_ran += ran
        total_passed += passed
        total_skipped += skipped

        name = os.path.basename(path)
        note = ""
        if finished.returncode != 0:
            bad.append((name, "exit %d" % finished.returncode))
            note = "  FAILED: " + ", ".join(sorted(set(FAILED_ONE.findall(out))))
        elif ran == 0:
            # A binary that ran nothing is the shape a green gate hides best.
            bad.append((name, "ran no tests"))
        if skipped:
            bad.append((name, "%d skipped" % skipped))
            note += "  SKIPPED: " + ", ".join(
                sorted(set(SKIPPED_ONE.findall(out))))
        sys.stdout.write("  %-24s ran=%-5d passed=%-5d skipped=%-3d%s\n"
            % (name, ran, passed, skipped, note))
        sys.stdout.flush()

    sys.stdout.write("  %-24s ran=%-5d passed=%-5d skipped=%-3d\n"
        % ("TOTAL", total_ran, total_passed, total_skipped))
    # Flushed before anything goes to stderr: the two streams interleave when
    # either is redirected, and a TOTAL line printed after the explanation of
    # why the run failed reads as though it belonged to a later run.
    sys.stdout.flush()
    if bad:
        sys.stderr.write("\noracle: the pinned run is not clean:\n")
        for name, why in bad:
            sys.stderr.write("  %s: %s\n" % (name, why))
        sys.stderr.write(
            "\nA skip in here is a failure: every reference this suite asks for\n"
            "is present in the pinned image by construction, so a skip means a\n"
            "test could not reach one that the image promises.\n")
        return 1
    sys.stdout.write("\noracle: clean against the pinned references\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
