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
"""How a reference is spelled, so that no tool here spells one itself.

**Copied from `libs/font/tools/oracle/oracle_env.py`**, which copied it from
`libs/unicode`, which landed the pattern. What
is changed, and each of the three is forced by something about this library
rather than chosen:

  1. **The PROBE table names six references and they share one image.** The
     other libraries have an image per reference. Here one test binary consults
     three of them in a single run, and `/usr/bin/zstd` links `liblz4.so.1`, so
     the set cannot be split without an image whose pinned CLI does not run.

  2. **What runs inside is a compiled test binary, not a Python driver.** So
     `command()` grew `scratch` and `env`: the tree is mounted read-only and the
     tests write temporary files, so they need a writable directory and a
     `TMPDIR` pointing into it. `gcu_path_temp_dir()` reads that variable.

  3. **`check_pin()` runs in both modes**, as in `font` and for the same reason:
     the image named here is built here, so there is no digest to trust and the
     run-time check is the whole guarantee. On this machine that makes host mode
     fail rather than run - the host's pyzstd is 0.17.0 against a pinned 0.19.1
     - and that failure is the point of the exercise, not a gap in it.

Three properties, in the order they matter:

  1. **It does not fail open.** A missing image, a missing reference, a version
     that does not match its pin - each raises. "Skipped" printed where a
     comparison should be is the failure this directory exists to avoid, and it
     is the failure this library shipped for months: `pkg-config` could not see
     two of these references, and nothing anywhere recorded what version any of them
     was.

  2. **It says which instrument answered.** `provenance()` returns the line a
     gate prints beside its numbers. A clean round trip against zstd 1.5.7 is a
     different claim from one against some zstd.

  3. **Paths mean the same thing on both sides.** The repository is mounted at
     its own host path, so a path a caller built already resolves - including
     the `.local` prefix a test binary's RUNPATH names, which is why the mount
     is the workspace rather than this library.

Modes, from GHOTI_ORACLE_MODE:

  container  (default) run the reference in its pinned image
  host                 run this machine's own tools, and still check the pins
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Two directories up, so this file has to live at <repo>/tools/oracle/.
ROOT = os.path.dirname(os.path.dirname(HERE))
# One further: the workspace, because a test binary's RUNPATH names
# <workspace>/.local/lib/ghoti.io and a mount of this library alone would leave
# libghoti.io-cutil unresolvable inside the image.
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))
IMAGES = os.path.join(HERE, "containers", "IMAGES")

MODE = os.environ.get("GHOTI_ORACLE_MODE", "container")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "podman")


class OracleUnavailable(Exception):
    """The reference cannot be reached. Never caught into a skip."""


# How to ask each reference for its version, and what the answer must contain.
#
# The probe is a program *in the image* rather than a shell one-liner here,
# because a one-liner puts a quoting layer between the check and the fact it
# checks - `font`'s first draft of its image wrote the script with printf and
# its \x27 escapes reached the file literally.
#
# Several of these name something other than the package, and the comments in
# containers/IMAGES say why: `zlib` is the zlib python3 links rather than
# python3, `pyzstd` names the zstd its dependency compiled in as well as its
# own version, and `liblz4` and `libbrotli` are what those libraries report
# when the tests dlopen them, not the Debian revision.
PROBE = {
    "zstd": (["zstd-version"], "zstd "),
    "liblz4": (["lz4-version"], "liblz4 "),
    "libbrotli": (["brotli-version"], "libbrotli "),
    "zlib": (["zlib-version"], "zlib "),
    "gzip": (["gzip-version"], "gzip "),
    "pyzstd": (["pyzstd-version"], "pyzstd "),
    # The alias target. Same probe script: the image puts its zstd in
    # /usr/local/bin, which precedes /usr/bin, so `zstd --version` reports the
    # built one without the probe knowing anything about it.
    "zstd-next": (["zstd-version"], "zstd "),
}

# What a host-mode run asks instead, when there is no image to run a probe in.
# Spelled out rather than derived, because the host has no `zstd-version` on its
# PATH and guessing at one would report "not installed" for a reference that is
# sitting there.
HOST_PROBE = {
    "zstd": ["sh", "-c",
        "zstd --version 2>&1 | sed -n 's/.*v\\([0-9][0-9.]*\\).*/zstd \\1/p'"],
    "liblz4": ["python3", "-c", "import ctypes;"
        "l=ctypes.CDLL('liblz4.so.1');"
        "l.LZ4_versionString.restype=ctypes.c_char_p;"
        "print('liblz4', l.LZ4_versionString().decode())"],
    "libbrotli": ["python3", "-c", "import ctypes;"
        "l=ctypes.CDLL('libbrotlienc.so.1');"
        "l.BrotliEncoderVersion.restype=ctypes.c_uint32;"
        "v=l.BrotliEncoderVersion();"
        "print('libbrotli %d.%d.%d' % ((v >> 24) & 0xFF, (v >> 12) & 0xFFF, v & 0xFFF))"],
    "zlib": ["python3", "-c",
        "import zlib; print('zlib', zlib.ZLIB_RUNTIME_VERSION)"],
    "gzip": ["sh", "-c",
        "gzip --version 2>&1 | sed -n '1s/^gzip \\([0-9][0-9.]*\\).*/gzip \\1/p'"],
    "pyzstd": ["python3", "-c", "import pyzstd;"
        "print('pyzstd', pyzstd.__version__, 'zstd', pyzstd.zstd_version)"],
    # No zstd-next: a host has one zstd, and a probe that reported it under both
    # names would make an aliased host run look like a two-version reading when
    # it is one version answering twice. ensure() refuses instead.
}

_pins = None
_cache = {}


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    _pins = {}
    if not os.path.exists(IMAGES):
        return _pins
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r"
                    % line)
            name, image, version = parts[0], parts[1], parts[2]
            _pins[name] = (image, version, parts[3] if len(parts) > 3 else "")
    return _pins


def _engine_ok():
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable(
            "%s is not on PATH, and GHOTI_ORACLE_MODE is 'container'.\n"
            "Install it, or run with GHOTI_ORACLE_MODE=host to use this "
            "machine's own tools - which answers a different question, and "
            "says so in the line it prints." % ENGINE)


def _have_image(image):
    finished = subprocess.run([ENGINE, "image", "exists", image],
        capture_output=True)
    if finished.returncode == 0:
        return True
    # `image exists` is podman's. Fall back to a docker-portable spelling.
    finished = subprocess.run([ENGINE, "image", "inspect", image],
        capture_output=True)
    return finished.returncode == 0


def ensure(name):
    """Make the reference runnable, or raise saying what is missing."""
    if MODE == "host":
        if name not in HOST_PROBE:
            raise OracleUnavailable("no host probe for %r" % name)
        return
    if MODE != "container":
        raise OracleUnavailable("GHOTI_ORACLE_MODE=%r is not a mode" % MODE)
    _engine_ok()
    table = pins()
    if name not in table:
        raise OracleUnavailable(
            "no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if _have_image(image):
        return
    # No pull path, unlike the libraries whose pins are stock images: this one
    # is built here and exists nowhere to pull from. Saying so beats a pull that
    # fails with a registry error about a name that was never pushed.
    raise OracleUnavailable(
        "the pinned image is not built on this machine:\n  %s\n"
        "Build it with `make oracle-build`." % image)


# Ask one reference's questions of a different pin, e.g.
#   GHOTI_ORACLE_ALIAS=zstd=zstd-next make check-oracle
# Nothing uses this yet, and it is kept for the reading it exists for: the same
# corpus against two reference versions, where a disagreement is what upstream
# changed rather than what this library got wrong. containers/IMAGES names
# `zstd-next` as the pin that would want it.
ALIAS = dict(
    pair.split("=", 1)
    for pair in os.environ.get("GHOTI_ORACLE_ALIAS", "").split(",")
    if "=" in pair)


def command(name, argv=None, scratch=None, env=None):
    """The argv prefix that runs `name`'s reference.

    `argv` is what to run *inside*, defaulting to the reference's own version
    probe. The workspace is bind-mounted at its own path, so any path a caller
    has built already resolves - a test binary named by its absolute path needs
    no translation, and neither does the `.local` prefix its RUNPATH carries.

    `scratch` is a directory the reference must be able to *write*, named the
    same way. The tests need one: the tree is mounted read-only, and every
    oracle here writes a temporary file. Passing it is deliberate rather than
    relying on the image's own /tmp, so that a caller which forgets to declare
    it fails on a path that does not exist rather than writing somewhere nobody
    reads.

    `env` is passed in with `--env`, which is how `TMPDIR` reaches the binary.

    Everything else is closed. `--network none` because no reference here has
    business reaching the network, and the tree is read-only because a corpus
    quietly edited by the thing being compared against it is not a comparison.
    """
    name = ALIAS.get(name, name)
    ensure(name)
    if MODE == "host":
        return list(argv) if argv is not None else list(HOST_PROBE[name])
    inner = argv if argv is not None else list(PROBE[name][0])
    image = pins()[name][0]
    argv_out = [ENGINE, "run", "--rm", "-i",
            "--network", "none",
            "--volume", "%s:%s:ro" % (WORKSPACE, WORKSPACE)]
    for path in ([scratch] if isinstance(scratch, str) else (scratch or [])):
        argv_out += ["--volume", "%s:%s:rw" % (path, path)]
    for key, value in sorted((env or {}).items()):
        argv_out += ["--env", "%s=%s" % (key, value)]
    return argv_out + ["--workdir", ROOT, image] + list(inner)


def version(name):
    """What the reference says it is. Runs it; the answer is cached."""
    name = ALIAS.get(name, name)
    key = ("version", name)
    if key in _cache:
        return _cache[key]
    expect = PROBE.get(name, ([], ""))[1]
    finished = subprocess.run(command(name), capture_output=True, text=True)
    # stdout only. A `docker` that is a podman shim prints a banner to stderr on
    # every invocation, and a probe reading both streams reads the banner.
    text = finished.stdout.strip().splitlines()
    text = text[0] if text else ""
    if not text:
        raise OracleUnavailable(
            "%s answered nothing.%s" % (name,
                ("\n  " + finished.stderr.strip()) if finished.stderr else ""))
    if expect and expect not in text:
        raise OracleUnavailable(
            "%s answered %r, which does not look like a version" %
            (name, text))
    _cache[key] = text
    return text


def check_pin(name):
    """Raise unless the reference's version matches containers/IMAGES.

    **In both modes**, following `font` rather than `unicode`: the run-time
    check is the real guarantee for an image built here, and every image named
    here is built here. `unicode` could relax it in host mode because all of its
    pins are stock images pinned by digest.

    The cost is that host mode fails on this machine, because its pyzstd is
    0.17.0 and the pin is 0.19.1. That is the honest outcome: a seekable-format
    differential against an unrecorded pyzstd is not the claim the gate prints,
    and it is what the gate printed before this directory existed.
    """
    name = ALIAS.get(name, name)
    table = pins()
    if name not in table:
        return version(name)
    said = table[name][1]
    got = version(name)
    if said not in got:
        raise OracleUnavailable(
            "%s: IMAGES says %r and it answers %r" % (name, said, got))
    return got


def provenance(names):
    """One line naming every reference that answered, and how.

    The name printed is the one that *answered*, not the one the gate asked
    for; under an alias those differ, and printing the requested name makes the
    line name the wrong pin.
    """
    where = MODE
    parts = []
    for name in names:
        resolved = ALIAS.get(name, name)
        label = resolved if resolved == name else "%s as %s" % (resolved, name)
        parts.append("%s %s" % (label, check_pin(name)))
    return "oracle(%s): %s" % (where, ", ".join(parts))


if __name__ == "__main__":
    # `make oracle-version`: prove every reference is reachable and print it.
    try:
        sys.stdout.write(provenance(sorted(PROBE)) + "\n")
    except OracleUnavailable as problem:
        sys.stderr.write("oracle: %s\n" % problem)
        raise SystemExit(1)
