# Oracles

Most of this suite checks the library against itself: an encoder against its own
decoder, a bound against what was written, a walk against the encoder that
produced the bytes. Those are round trips, and a round trip cannot see a
misreading of the specification that the encoder and decoder share.

The oracle tests are the part that is not self-referential. They compare against
implementations nobody here wrote: the `zstd` CLI, `liblz4`, `libbrotli`,
`gzip`, Python's `zlib` module, and `pyzstd`. Fifteen test files carry **sixteen
availability sentinels** (`OracleIsActuallyAvailable`, and `RealImplementationIsActuallyAvailable`
where one file has two references) which *fail* when their reference is absent,
because a skipped oracle test and an absent one are the same line in a summary.

This page is about the other half of that: not whether a reference is there, but
**which version of it answered.**

## The problem a container solves here

Presence was enforced; the version was not recorded anywhere. A run on this
machine compared against `zstd` 1.5.7, `liblz4` 1.10.0, `libbrotli` 1.1.0,
`zlib` 1.3.1, `gzip`
1.13 and whatever `pyzstd` `pip` last resolved - and nothing in the repository
said any of that, so a green run named a set of tool names rather than a set of
behaviours. Two consequences, and the second is worse:

- A reference that changes an answer reads as a regression in this library.
- A reference that is *already* wrong for the comparison reads as correct
  forever, because nothing states what it was supposed to be.

`pkg-config` made the second concrete. A survey of the libraries' oracles ran
`pkg-config --modversion liblz4`, saw it fail, and concluded that fourteen LZ4
oracle tests were skipping. They were all passing: `liblz4.so.1` was installed
and every one of those tests loads it by name at run time with
`gcu_library_open`. `pkg-config` answers *can I build against this*; the tests
ask *can I load this*. There is no development package here and there does not
need to be.

## What is pinned

`tools/oracle/containers/IMAGES` is the manifest: one line per reference, with
the version it must claim when asked. All five lines name one image, built from
`tools/oracle/containers/refs/Containerfile`. Six references, one image.

The base is pinned by digest, the pip packages by exact version, and Debian's
packages by full apt version **against a dated snapshot** rather than the live
archive - at the date the base image itself records in its own sources file. The
archive keeps one version of a package and drops it when an update lands, so an
exact version against the live archive resolves today and fails to resolve in
some number of weeks; `snapshot.debian.org` keeps all of them. That also pins
more than it names: the five packages are exact and everything they pull comes
from one frozen archive state, so the whole transitive closure is reproducible.

| reference | version | answers for |
| --- | --- | --- |
| `zstd` | zstd 1.5.7 | frames, blocks, dictionaries, the seek table |
| `liblz4` | liblz4 1.10.0 | the LZ4 frame and block formats |
| `libbrotli` | libbrotli 1.1.0 | RFC 7932, both directions |
| `zlib` | zlib 1.3.1 | RFC 1950 and RFC 1951 |
| `gzip` | gzip 1.13 | RFC 1952 member structure |
| `pyzstd` | pyzstd 0.19.1 zstd 1.5.7 | the seekable format, dictionary training |

Three of those version strings are deliberately not the name of a package:

- **`zlib`** is the zlib `python3` links at run time
  (`ZLIB_RUNTIME_VERSION`), not the interpreter's version. Only the first
  decides whether a stream round-trips.
- **`pyzstd`** names *two* versions, because `pyzstd` 0.19.1 declares
  `backports-zstd>=1.0.0` and, below Python 3.14, that dependency **is** the
  zstd implementation - a C extension with its own libzstd compiled in, linking
  none. Pinning only `pyzstd` would leave the thing that actually answers
  floating on an open range.
- **`liblz4`** is asked by loading the soname and calling
  `LZ4_versionString()`, which is how the tests reach it. No `lz4` CLI is
  installed; a CLI's version is a fact about a different file.
- **`libbrotli`** is asked the same way: `dlopen` of `libbrotlienc.so.1` and
  `BrotliEncoderVersion()`. The package pin is `libbrotli1` 1.1.0-2+b7; the
  library reports 1.1.0. No CLI and no `-dev` package.

### One image, not six

Every other library in the suite has an image per reference. This one
cannot, for two measured reasons:

- `/usr/bin/zstd` links `liblz4.so.1`. An image with a pinned zstd CLI and no
  liblz4 has no working zstd CLI either.
- What runs inside the image is a **compiled test binary**, not a driver script.
  A single binary consults up to three of these references in one run, and
  `liblz4` and `libbrotli` are reached by `dlopen` - so the only way to pin
  them at all is for the binary that loads them to be the process running in
  the image.

The binaries are built on the host, against the host's own compiler, and only
*run* inside. This image pins the references; it does not pin the build.

## A second zstd, ahead of the release

`zstd-next` is a second pin for the same questions, and it is **not gating**:
`unicode`'s python/python-next shape, where one reference is a release and the
other is ahead of it, and a disagreement from the second is a finding to triage
rather than a failure to fix.

There is no newer release to point it at - upstream's latest is v1.5.7, which is
what Debian 13 carries and what the gating pin already names - so "next" means
the development branch, pinned to a commit rather than a branch, and it reports
itself as **v1.6.0**. That is the one place where building from source earns its
keep here: Debian will never carry two zstds. It is also why the same argument did
*not* apply to the pins expiring, where a dated snapshot was the better answer.

The image overlays the `refs` one, so every other reference is byte-identical to
the gating run and only zstd's answers can differ - which is what makes a
disagreement attributable. It does not move pyzstd's zstd, which is compiled into
`backports.zstd` and stays at 1.5.7.

```bash
make oracle-build-next   # builds refs, then this on top of it
make check-oracle-next   # the same fourteen suites, against v1.6.0
```

**What it is worth today, measured rather than assumed.** All 164 tests pass
against v1.6.0, and each version reads what the other writes. But a clean run
only means something if the two references differ, so that was checked: across
**128 configurations** - four input shapes (prose, incompressible, zeros, a
repeat beyond the ordinary match window), four sizes from 100 B to 3 MB, and
eight option sets including `-22 --ultra`, `--long=27` and `-T4` - the two
produce **byte-identical output in every one**. The binaries themselves differ
(different SHA-256, different size, different `--version`, `--help` and `-b`
output), and the comparison was controlled by checking that it does report `-1`
against `-9` as different.

So the honest reading: **v1.6.0 has not changed the encoder's output for anything
this suite asks**, and the pin is currently a tripwire rather than a live second
opinion. That is itself the answer to "will the next zstd move our ratios" - it
does not - and the day upstream does change something, this is what sees it
before the release lands rather than after.

It is deliberately not in CI. It needs a source build from a network clone on
every run, and an advisory reference whose disagreement is not a failure does not
belong in a gate that must be green to merge.

## Using it

```bash
make oracle-build      # build the image: once, and whenever a pin moves
make oracle-version    # print every reference and its version, or fail
make check-oracle      # run the fifteen oracle suites against the pinned set
make oracle-help       # the short version of this page
```

`check-oracle` is **not** in `TEST_GATES`, which is what every library that
landed this pattern also decided: `make test` must not require a container
engine. The ordinary suite still runs the same binaries against whatever the
machine has.

`GHOTI_ORACLE_MODE=host` uses this machine's own tools instead - and still
checks them against the pins. On a machine whose `pyzstd` is not 0.19.1 that
fails, which is the point rather than a gap: a seekable-format differential
against an unrecorded `pyzstd` is not the claim the gate prints.

## How it fails

Every one of these was produced deliberately before the gate was believed.

| provoked | result |
| --- | --- |
| `IMAGES` says `zstd 1.5.8`, image has 1.5.7 | refuses, printing both |
| the pinned image is not built | refuses, naming `make oracle-build` |
| a gate asks for a reference `IMAGES` does not name | refuses |
| any test skips inside the image | the run is **red** |
| a binary runs no tests at all | the run is red |

**A skip is a failure in here**, which is the rule that differs from the host
suite. On the host a skip means "this machine has no such reference" - a fact
about the machine. In the pinned image every reference is present by
construction, so a skip means a test could not reach something the image
promises, and that reads identically to a green run in every summary line.

The pin check runs before any binary does, so a missing or drifted reference
cannot reach a test at all. The skip count is the second line of defence, for a
reference that is present but unusable, and it needs nobody to have remembered
to write a sentinel - which matters, because `test_lz4_spec_oracle.cpp` shipped
a sentinel that answered for only one of its file's two references.

## The golden vectors, and what checking them was worth

Four `golden_vectors.h` files - deflate, gzip, lz4 and zstd - hold committed
compressed bytes and the output they should decode to. The decoder suites check
this library against them, and the whole value of that is that this project did
not write the bytes: a vector from elsewhere can catch a misreading of a format
that our own encoder and decoder share.

That value rested on a sentence in a comment, and **nothing checked the
sentence**:

| file | claims | now checked against |
| --- | --- | --- |
| deflate | "generated using Python's zlib module" | python `zlib`, raw stream |
| gzip | "generated using Python's gzip/zlib modules" | python `gzip` |
| lz4 | "VERIFIED using Python's lz4 library (version 4.4.5)" | `liblz4` itself |
| zstd | "minimal valid Zstandard frames (RFC 8878 / zstd format spec)" | the `zstd` CLI |

If a vector were in fact produced by this library, or typed out and never
confirmed, a decoder suite passing against it is a round trip wearing a disguise
- it reads as an external check and is not one, and it passes either way.

`test_golden_provenance.cpp` hands every committed vector to its reference and
compares with the committed expectation; lz4's are done in
`test_lz4_spec_oracle.cpp`, where the `liblz4` loader already lives. Nothing is
regenerated and no fixture changes. **All of them pass**, so the attributions
are true.

Three things worth knowing about that gate:

- **zstd's file is the interesting one.** It names no tool at all - the frames
  were read out of the specification by hand - so this is the first time
  anything external has seen them, which makes it the most valuable of the four
  rather than the weakest.
- **lz4 is checked against a stronger reference than its file names.** The file
  says the Python `lz4` binding; this uses `liblz4` itself, which is what that
  binding wraps, and is the one already pinned.
- **Two deflate vectors sit outside the table** because their expected output is
  generated at run time. They are checked for decoding to the length the file
  declares. They are in the gate because the compiler asked for them: the file
  would not build without touching `golden_v8_compressed_ptr`, which only
  `test_deflate_decoder.cpp` had ever referenced - an unused-variable error
  pointing straight at the two vectors nothing was checking.

**What this gate does not yet demonstrate.** Proved discriminating by planting a
corrupted vector two ways - a broken CRC and a truncated trailer - and both
turned it red. Both also turned the existing decoder suite red, so on today's
code the two overlap. The case only this gate can settle is a committed vector
that our decoder handles exactly as the file expects but the reference would
reject; that cannot be planted without an actual defect, which is the point of
having the gate rather than an argument against it. When the two disagree, this
is the one that says which side is wrong.

Regenerating the vectors is a different job and is not done: it matters only if
more cases are wanted, and then the generator belongs in the pinned image so a
fixture's bytes depend on a recorded version rather than on whoever ran it.
