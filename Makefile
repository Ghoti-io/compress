SUITE := ghoti.io
PROJECT := compress

BUILD ?= release
# The version of this library. MINOR_VERSION carries the minor and the patch as
# one dotted string; the two are split out below for the places that need three
# separate integers. See CONVENTIONS.md section 4.
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
# Substituted into the .pc file; an empty Version: field makes every
# pkg-config version constraint fail.
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. It defaults to the major version, so an ordinary build
# of 1.x is "-1" and two majors cannot be loaded into one process by mistake.
# Override it for a build that wants its own identity:  make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

# What the library reports as its version. The branch is appended only when it
# is not the default, so an ordinary build says "1.2.3" and an overridden one
# says "1.2.3-dev". Computed before BUILD=debug rewrites BRANCH below.
ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# If BUILD is debug, append -debug.
#
# "override" because BRANCH may have come from the command line, and a
# command-line variable otherwise wins over a plain assignment here: without it
# `make BRANCH=-dev BUILD=debug` produced a debug build carrying the release
# token, whose symbols collide with the release build's.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# PC_INSTALL_PATH names where this project's own .pc file is installed.
# PKG_CONFIG_PATH is the environment's and is never assigned here: make exports
# an inherited variable with whatever value the makefile last gave it, so
# overwriting it handed every sub-make a different PKG_CONFIG_PATH from the
# parent's. The sub-make then derived different flags, found the flag stamp
# changed, and rebuilt everything - which check-rebuild reports as a settled
# tree that will not settle. It showed first under MSYS2, whose login shell
# exports PKG_CONFIG_PATH, and happens on Linux whenever the exported value is
# not exactly the install location. cutil made the same change.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

# `override` on each of those: BUILD may arrive on the command line, and a
# command-line variable beats a plain makefile assignment, so without it
# `make BUILD=debug` skips the rewrite and builds into ./build/debug --
# outside the platform tree, and a different tree from the one plain `make`
# uses. The platform segment exists to keep linux/mac/win builds apart.

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	override BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	override BUILD := mac/$(BUILD)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	override BUILD := win32/$(BUILD)

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))

endif

# ---------------------------------------------------------------------------
# Installation prefix
#
# Defaults to the system location chosen above. Override it to install
# somewhere else - the suite's bootstrap installs every library into a local
# prefix so that each build resolves its dependencies through pkg-config,
# exactly as a consumer would, rather than through a second code path that
# only in-tree builds exercise. See CONVENTIONS.md section 1.
#
#     make install PREFIX=/path/to/prefix
# ---------------------------------------------------------------------------
ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
# A non-system prefix has no /etc/ld.so.conf.d, and writing to it would need
# root anyway. Everything built here carries an rpath to the prefix instead.
LDCONF_INSTALL_PATH :=
endif

# Dependencies are looked up along the inherited PKG_CONFIG_PATH as well as the
# install location chosen above, so that exporting PKG_CONFIG_PATH works as the
# errors below say it does. The inherited value comes first: it is an explicit
# request for this build, where the install location may be only a default.
PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)


CXX := g++
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wfatal-errors -std=c++20 -O1 -g $(EXTRA_CXXFLAGS)
CC := cc
# The optimization level is the one thing that should distinguish the two
# builds' compile flags, and until now it did not: the BUILD=debug block above
# renamed the artifact and changed nothing about how the code was compiled, so
# `make BUILD=debug` produced -O3 objects carrying a -debug filename - a debug
# build that cannot be stepped through. -g stays in both, because a release
# build nobody can read in a debugger is a release build nobody can diagnose,
# and the symbols cost only file size.
#
# -O3 is retained for release, and as of 2026-09-23 it is measured rather than
# inherited. One encode per case over a fixed 256 KB input, deterministic
# counters because the machine was far too loaded for a clock (a 12-round
# alternating wall-clock sweep put the reference library's own rate 8.3% apart
# between runs, which is more than the effect being looked for):
#
#   instructions, -O3 vs -O2   deflate-9 -9.80%   zstd-9 -10.80%
#                              zstd-16   -4.37%   median over 9 cases -1.35%
#                              cheap cases within 0.1% either way
#   data refs                  deflate-9 -9.80%   zstd-9 -16.94%  zstd-16 -7.61%
#   D1 misses                  -0.06% .. +0.01%   LLd misses +0.00% .. +0.01%
#   incompressible input       deflate-9 -1.92%   zstd-9 -11.31% (instructions)
#
# So -O3 is not buying instructions with cache misses, which is the way this
# question usually goes wrong: it does strictly less work on every axis. The
# miss counts are flat because the working set belongs to the algorithm and the
# tables, which the optimizer does not change; what it removes is loads and
# stores. Compressed output is byte-identical at both levels.
#
# What appends its own -O after this one, all relying on the last -O winning:
# `make coverage` passes EXTRA_CFLAGS="--coverage -O0", which lands at the end
# of CFLAGS below. What does NOT: AFL_CFLAGS is built standalone with its own
# -O2 and never sees CFLAGS.
#
# ASAN_UBSAN_FLAGS carries no -O at all, so the sanitizer build inherits
# whatever this says. That was not a decision anyone recorded - it is what
# happens when the flag set omits an -O - so measure it rather than assume:
#
#   make -n test-asan PREFIX=... | grep -e ' -c ' | grep -oE -e '-O[0-3s]' \
#     | sort | uniq -c
#
# gives 76 C library TUs at -O3 and 113 C++ test TUs at -O1 (a literal in
# CXXFLAGS). Filter on ` -c `: without it the count says 226 -O1, because the
# shared-library link line is g++ as linker driver carrying CXXFLAGS, and an
# -O on a link line is inert without LTO.
#
# Leaving the inheritance alone, but NOT because inheriting is better. An
# earlier version of this comment argued that a gate pinned below the shipping
# level tests code nobody installs, naming the aliasing and signed-overflow
# assumptions UBSan exists to catch. Measured, that argument does not hold:
#
#   UBSan signed overflow   reported identically at -O0, -O1, -O2 and -O3
#   strict aliasing         not reported at ANY level, by any sanitizer. The
#                           only instrument is -Wstrict-aliasing at compile
#                           time, and the line below used to claim it "also
#                           fires at -O0". It does not. What arms the warning
#                           is -fstrict-aliasing, the optimisation, which gcc
#                           enables at -O2 and above and DISABLES at -O0 and
#                           -O1. Measured on both shapes, at every level from
#                           1 to 3: silent at -O0 and -O1, fires from -O2 up,
#                           and armed at -O0 by naming -fstrict-aliasing
#                           explicitly. So the claim was right about the
#                           release build and wrong about the debug one, which
#                           is the build it was written to describe.
#   -Warray-bounds          -O0 silent, -O2 warns
#
# The level-dependent diagnostics are compiler *warnings*, and those already
# come from the release build, which compiles every TU at -O3 with -Werror. A
# sanitizer's own -O buys essentially nothing in detection, so on this evidence
# chron's choice - pin -O1 and get readable stack traces for free - is the
# better one. It is not changed here because the suite-wide question is not
# this Makefile's to settle; the measurement is recorded so whoever settles it
# is not relying on the argument above, which was wrong.
ifeq ($(BUILD),debug)
OPT_CFLAGS := -O0
# -fstrict-aliasing is named ONLY here, and only because it was measured here.
# It is what arms -Wstrict-aliasing, gcc disables it below -O2, and naming it
# also turns the ASSUMPTION on - a codegen change, not only a warning. So it
# belongs wherever it is codegen-neutral and nowhere else. Measured on all 76
# library TUs, comparing `objdump -d` text rather than object bytes, since
# debug info records the command line and makes every .o differ:
#
#   -O0, flag off vs on     0 of 76 objects differ
#   -O1, flag off vs on    30 of 76 differ
#   -O2, -fno- vs -f       44 of 76 differ   (positive control: the
#                                             comparison can see a change)
#
# So -O0 is free and -O1 is not, which is why this sits in the debug branch
# rather than in ALIASING_CFLAGS where every configuration would inherit it.
# A sibling library measured 0 of 9 at BOTH -O0 and -O1; that zero does not
# transfer, and the 30 above is what inheriting it would have bought.
#
# No C compilation in this file uses -O1 today, so the 30 is a hazard rather
# than a live change - but `EXTRA_CFLAGS=-O1` would have taken it silently.
ALIASING_FFLAGS := -fstrict-aliasing
else
OPT_CFLAGS := -O3
# Nothing to name: -O3 enables -fstrict-aliasing already, so the warning is
# armed and the assumption is one this build has always made.
ALIASING_FFLAGS :=
endif

# Strict aliasing, named rather than inherited. Two flags doing two jobs:
#
#   -fstrict-aliasing     licenses the optimisation AND is what arms the
#                         warning. Already on at -O2/-O3, off at -O0/-O1.
#   -Wstrict-aliasing=2   chooses what the warning diagnoses.
#
# Both were previously implicit, and each was implicit in a way that hid
# something.
#
# The LEVEL was 3, because -Wall implies 3 - it does not leave the level
# unset, which is what reading the flag list suggests. 3 is gcc's least
# aggressive setting. Measured here across five shapes at -O2, varying one
# thing at a time:
#
#                                              L0  L1  L2  L3
#   &visible object, direct deref               0   1   1   1
#   &visible object, via pointer variable       0   1   1   0
#   parameter, direct deref                     0   1   0   0
#   parameter, via pointer variable             0   1   0   0
#   local via void * -> int *                   0   0   0   0
#
# Two independent axes: taking the address of an object the compiler can see
# is what level 2 needs, and routing the cast through a separate pointer
# variable is what defeats level 3. So level 2 adds the second row over the 3
# that was in force, and costs nothing: all 76 library TUs compile clean at 2
# under -Werror, in both the release and debug configurations and with
# -DGCOMP_TEST_BUILD. Level 1 was measured too and is not adoptable - 7
# diagnostics in 5 files, every one a construct C17 sanctions (six are
# initial-member conversions, one is a trailing-array carve), and no true
# positive among them.
#
# The last row is the standing limit and bounds what this can claim: no level
# catches punning through a void *, which is the shape real code reaches for.
# This covers three of the five known shapes and is not aliasing coverage in
# general.
#
# The OPTIMISATION was on only by virtue of -O3, so `BUILD=debug` compiled at
# -O0 with the warning silent at every level - the debug build had no aliasing
# diagnostic at all, and looked identical to one that did. $(ALIASING_FFLAGS)
# names -fstrict-aliasing in the debug branch, and ONLY there, because naming it
# arms the assumption as well as the warning: measured codegen-neutral at -O0 and
# not at -O1. The note by OPT_CFLAGS has the figures.
#
# EXTRA_CFLAGS comes last and so can still displace the level: an explicit
# level beats -Wall's implicit 3 from either side, but a later explicit level
# displaces an earlier one, which makes EXTRA_CFLAGS=-Wstrict-aliasing=3 a
# silent disarming. check-aliasing is the reason that is not a silent one.
ALIASING_CFLAGS := $(ALIASING_FFLAGS) -Wstrict-aliasing=2

# -Wno-error=unused-function is deliberately NOT here, and its absence is a
# compress choice rather than the suite convention it was copied from. It was the
# reason seventeen dead functions in two files could sit behind eleven warnings
# indefinitely: nothing in the build said anything, because the warnings were
# demoted and `make test` prints thousands of lines. With them deleted the whole
# library, the examples, the benchmarks and the 112 test binaries compile clean,
# measured before removing it - so the escape now costs nothing and an unused
# static becomes an error at the moment it appears rather than a line nobody
# reads.
#
# If a legitimately-unused static ever needs to land, say so at the site with
# __attribute__((unused)) rather than restoring this: the attribute names the one
# function, and the flag names all of them forever.
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wfatal-errors -std=c17 $(OPT_CFLAGS) -g $(ALIASING_CFLAGS) $(EXTRA_CFLAGS)
# Library-specific compile flags (export symbols on Windows, PIC on Linux)
# GCOMP_BUILD enables DLL export on Windows (checked by GCOMP_API macro)
# GCOMP_TEST_BUILD enables export of internal functions for testing (checked by GCOMP_INTERNAL_API macro)
# No -DGCOMP_TEST_BUILD here: the shipped library exports its public API and
# nothing else. Tests reach the internals by linking the static archive, which
# a static link can do even for hidden symbols.
ifeq ($(OS_NAME), Windows)
# Everything built here but the library itself links the static archive, so
# the headers must not say dllimport to it: an archive has no __imp_ thunks.
# The library's own objects also get GCOMP_BUILD, which the header tests first.
# See GCOMP_API in macros.h.
CFLAGS += -DGCOMP_STATIC
CXXFLAGS += -DGCOMP_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGCOMP_BUILD $(EXTRA_CFLAGS)
# ---------------------------------------------------------------------------
# Goals that need no dependency
#
# The pkg-config check below is an $(error), and make evaluates that while it
# reads this file - before it has decided which target to build. So it fires
# for `make clean` too, and a tree whose prefix has since moved cannot be
# cleaned: clean exits 2 having removed nothing, and the message tells you to
# run bootstrap.sh, which is confusing when bootstrap.sh is what invoked it.
# Removing nothing is the harmful part - `clean` is least able to run in
# exactly the situation that makes someone type it.
#
# These goals read no header and link no library, so the check is skipped when
# every goal named is one of them.
#
# uninstall is here because it finds what it removes through PREFIX and asks
# pkg-config nothing, so needing cutil in order to REMOVE this library is the
# same defect as needing it to clean - and worse, because something is already
# broken by the time anyone types it. uninstall-debug has to be listed beside
# it: the recipe is `make uninstall BUILD=debug`, so the outer invocation is
# the one that parses this file first, and it would fail before the inner ran.
#
# Deliberately not here: check-symbols and coverage, which build the library
# before they can say anything.
#
# `$(or $(MAKECMDGOALS),all)` is load-bearing. A bare `make` names no goal, so
# MAKECMDGOALS is empty, and $(filter-out ...) of an empty list is empty -
# which would skip the check in the one case it exists for. Substituting the
# default goal gives filter-out something that is not in the list.
#
# check-clean-guard tests both directions; it is in TEST_GATES.
# ---------------------------------------------------------------------------
DEPLESS_GOALS := clean cloc docs docs-pdf help fuzz-help sanitizer-help uninstall uninstall-debug
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# cutil, found through pkg-config. The name carries the branch, which is how a
# consumer picks a version; CUTIL_PC is overridable so this library can be
# built against a cutil on a different branch from its own.
CUTIL_PC ?= ghoti.io-cutil$(BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
LDFLAGS := -L /usr/lib -lstdc++ -lm $(CUTIL_LIBS) -lpthread $(EXTRA_LDFLAGS)
ifdef PREFIX
# So that a library, a test or an example finds its Ghoti.io dependencies in the
# prefix at run time without LD_LIBRARY_PATH.
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
# Windows has no rpath: a program finds its DLLs through PATH. Putting the
# prefix's bin/ on it for everything make runs is the equivalent, so that a
# test or an example finds its dependencies without the caller arranging it.
# Without this they die before main() with 0xC0000135 and make reports 127.
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

# The symbol namespace token. Derived from BRANCH so that the token inside every
# exported symbol is the same one that names the .pc file, the install directory
# and the shared library. See CONVENTIONS.md section 4.
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')

BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
LINK_FLAGS_STAMP := $(OBJ_DIR)/.linkflags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps


# Add OS-specific flags
ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC

else ifeq ($(UNAME_S), Darwin)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows

else
	$(error Unsupported OS: $(UNAME_S))

endif

# The standard include directories for the project.
# Include cutil headers for threading support (via pkg-config)
INCLUDE := -I include/ -I $(GEN_DIR)/ $(CUTIL_CFLAGS)

# Additional include directories for tests (common helpers, method-specific data)
TEST_INCLUDE := $(INCLUDE) -I src/ -I tests/common/ -I tests/methods/deflate/

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))


TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`

# The same query as a make variable, for the link stamps to record. TESTFLAGS is
# a BACKTICK string, expanded by the shell when a recipe runs, so its characters
# are the same however far gtest moves -- a stamp recording $(TESTFLAGS) alone
# would be present, correct-looking and permanently equal. It is still worth
# recording, because a command-line `make TESTFLAGS=...` replaces the whole
# value and so does change those characters; the two spellings catch different
# halves and a stamp needs both. Checked non-empty: an empty $(shell) result
# would be the same permanently-equal trap wearing the other hat.
TESTFLAGS_PC := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest 2>/dev/null)

# The checks `make test` runs besides the tests themselves. Named in a
# variable so that a build which cannot satisfy them can clear it: the
# coverage target does, because --coverage links the gcov runtime, whose
# mangle_path check-symbols is right to reject in a shipping library and
# wrong to reject in an instrumented one. Spelled as text's TEST_GATES is.
TEST_GATES ?= check-symbols check-clean-guard check-aliasing check-test-build check-stamps \
	check-oracle-coverage \
	check-san-report


# Valgrind flags (exclude "still reachable" as it's not a leak)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1

# Told to the tests themselves, because a handful of them ask for more memory
# than Memcheck can shadow. Memcheck keeps validity and addressability bits
# for every allocated byte, so a test that deliberately allocates gigabytes
# costs several times that here and is killed rather than reporting anything.
# Such a test skips on this, and says why; nothing else reads it.
VALGRIND_TEST_ENV := GCOMP_UNDER_VALGRIND=1

# Test helper object file
TEST_HELPER_OBJ := $(OBJ_DIR)/tests/common/test_helpers.o

# The static archive, not -l: a static link resolves hidden symbols, so the
# tests can exercise internals that the shared library does not export.
#
# --whole-archive is required, not decorative. Each compression method registers
# itself from a constructor (GCOMP_AUTOREG_METHOD), and a plain archive link
# only pulls in an object file that something references by name. Nothing
# references the registration objects, so without this the methods are silently
# absent and every test that asks the registry for one fails.
COMPRESSLIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive

# Single shell: discover test sources and compute executable name for each (path|name per line).
# test.cpp -> testCompress; test_foo.cpp -> testFoo. Avoids hundreds of $(call test-name) / CreateProcess.
TEST_PAIRS := $(shell find tests -type f -name 'test*.cpp' 2>/dev/null | sort | grep -v test_helpers.cpp | while read f; do \
	if [ "$$f" = "tests/test.cpp" ]; then echo "$$f|testCompress"; \
	else echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; fi; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))

# Generate list of test executables (no $(call test-name) - use precomputed TEST_NAMES)
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# Automatically collect all example .c files under examples directories.
EXAMPLE_SOURCES := $(shell find examples -type f -name '*.c' 2>/dev/null)

# Convert each example source file path to an executable path.
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))

# Automatically collect all benchmark .c files under bench directories.
BENCH_SOURCES := $(shell find bench -type f -name '*.c' 2>/dev/null)

# Convert each benchmark source file path to an executable path.
BENCHMARKS := $(patsubst bench/%.c,$(APP_DIR)/bench/%$(EXE_EXTENSION),$(BENCH_SOURCES))

all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

####################################################################
# Dependency Inclusion
####################################################################

# Include generated dependency files (computed list; no wildcard so same set on all platforms).
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_HELPER_OBJ:.o=.d) $(addprefix $(APP_DIR)/,$(addsuffix .d,$(TEST_NAMES)))
-include $(DEPFILES)


####################################################################
# Object Files
####################################################################

# Pattern rule for C source files: compile .c files to .o files, generating dependency files.
####################################################################
# Generated version header
####################################################################

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h

# libver_gen.h is regenerated on every build and rewritten only when its content
# changes, so a variable given on the command line - make MAJOR_VERSION=2, or
# make BRANCH=-dev - takes effect. Keying the rule on the Makefile's timestamp
# alone left the previous token and version baked into the build, and nothing
# said so.

# EVERY rule that compiles a translation unit lists this header as an
# order-only prerequisite, and they have to stay that way. namespace.h
# includes libver.h, which includes libver_gen.h, so any source that reaches a
# public header needs it to exist before the compiler runs. Only the two
# release library rules had it. The result was that `make test-asan`,
# `make test-tsan` and `make fuzz-build` could not build a clean checkout at
# all: each stopped on
#
#     libver.h:45:10: fatal error: ghoti.io/compress/libver_gen.h
#
# after eleven lines of output, exit 2. Nobody hit it because nobody runs a
# sanitizer target as the first command in a fresh tree - a plain `make` first
# generates the header as a side effect of the release build, and from then on
# the file is simply there, so the missing prerequisite is invisible for the
# life of the checkout. It surfaced only in a scratch clone.
#
# The release paths were not affected and are worth distinguishing from the
# broken ones, so this is not read as a wider outage than it was: test
# executables take the static archive as a normal prerequisite, and
# test_helpers.cpp includes no compress header (the libver.h in its .d file is
# cutil's). The helper and release test rules are listed for uniformity, so
# that the rule set stops encoding a guess about which sources include what -
# adding one include to test_helpers.h would otherwise reintroduce this.

.PHONY: force-libver
force-libver:

$(LIBVER_GEN): force-libver
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace, and two\n" >&2; \
		printf "versions of this library could not be loaded into one process.\n" >&2; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GCOMP_LIBVER_GEN_H' \
		'#define GHOTI_IO_GCOMP_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_COMPRESS_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_COMPRESS_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_COMPRESS_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_COMPRESS_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_COMPRESS_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GCOMP_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for C++ source files (if any):
$(OBJ_DIR)/%.o: src/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@


####################################################################
# Shared Library
####################################################################

$(APP_DIR)/$(TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Compiling Compress Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Static Library
####################################################################

$(APP_DIR)/$(STATIC_TARGET): \
		$(LIBOBJECTS)
	@printf "\n### Archiving Compress Static Library ###\n"
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

####################################################################
# Unit Tests
####################################################################

# Test helper object (compiled once, linked into all tests)
$(TEST_HELPER_OBJ): tests/common/test_helpers.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for compiling test source files to object files
# This allows tests to be compiled separately from linking
# Only test_helpers is built as .o (shared by all tests). Individual test .cpp files compile directly to exe.
$(OBJ_DIR)/tests/%.o: tests/%.cpp $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Compile each test .cpp directly to executable (link test_helpers.o in same step). Fewer targets = faster make graph.
# Args: $1 = source path, $2 = executable name (from TEST_PAIRS; no $(call test-name) in expansion).
# The static archive is a NORMAL prerequisite, not an order-only one.  Tests
# link it with --whole-archive, so a test binary built against an older archive
# keeps running the older code: an order-only prerequisite is built first but
# never causes a relink, which meant a library-only change left every test
# exercising the previous build and reporting green on it.  The shared target
# stays order-only -- the tests do not link it.
define test-executable-rule
$(APP_DIR)/$2$(EXE_EXTENSION): \
		$1 \
		$(TEST_HELPER_OBJ) \
		$(APP_DIR)/$(STATIC_TARGET) \
		$(FLAGS_STAMP) \
		$(LINK_FLAGS_STAMP) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling and linking %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(TEST_INCLUDE) -MMD -MP -MF $$(@D)/$2.d -o $$@ $$< $$(TEST_HELPER_OBJ) $$(COMPRESSLIBRARY) $$(LDFLAGS) $$(TESTFLAGS)
endef

# Generate build rules from TEST_PAIRS (one pair = source|name)
$(foreach pair,$(TEST_PAIRS),$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

####################################################################
# Examples
####################################################################

# Pattern rule for example executables
# The archive must precede $(LDFLAGS): the linker resolves left to right, and
# LDFLAGS is where cutil lives.  With cutil first, nothing had referenced its
# symbols yet, so the default --as-needed dropped it and every example failed
# to link with undefined ghotiio_cutil_0_* references.
$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(COMPRESSLIBRARY) $(LDFLAGS)

####################################################################
# Benchmarks
####################################################################

# Pattern rule for benchmark executables
# The archive must precede $(LDFLAGS): the linker resolves left to right, and
# LDFLAGS is where cutil lives.  With cutil first, nothing had referenced its
# symbols yet, so the default --as-needed dropped it and every example failed
# to link with undefined ghotiio_cutil_0_* references.
# The static archive is a real prerequisite, not an order-only one: these link
# $(COMPRESSLIBRARY), which is the archive.  Naming only the shared library
# meant a clean tree could reach this rule before the archive existed - `make
# bench` on a fresh checkout failed with "cannot find ...-0.a" - and, worse,
# that a tree where the archive already existed would link a stale copy of it
# without rebuilding.  That second failure is the one that cost a day of
# untrustworthy test runs before the test rules were given the same fix.
$(APP_DIR)/bench/%$(EXE_EXTENSION): bench/%.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Benchmark: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(COMPRESSLIBRARY) $(LDFLAGS)

####################################################################
# Fuzz Testing (AFL++)
####################################################################

# Fuzz harness source files
FUZZ_SOURCES := $(shell find fuzz -maxdepth 1 -type f -name 'fuzz_*.c' 2>/dev/null)

# Fuzz executables (built with afl-gcc)
FUZZ_EXECUTABLES := $(patsubst fuzz/%.c,$(APP_DIR)/fuzz/%$(EXE_EXTENSION),$(FUZZ_SOURCES))

# AFL compiler. AFL++ 4.x ships several front ends; afl-clang-fast gives the
# best instrumentation and is the one that supports the sanitizers below. The
# legacy afl-gcc wrapper still works if you have a reason to prefer it:
#   make fuzz-build AFL_CC=afl-gcc
AFL_CC ?= afl-clang-fast

# Sanitizers in the fuzz build.
#
# AFL on its own only notices a bug that crashes the process. A heap overflow
# that happens to land inside another allocation, or an out-of-bounds read that
# returns garbage, runs to completion and is recorded as a normal execution.
# ASan and UBSan turn both into an abort that AFL sees as a crash, which is the
# difference between fuzzing that finds memory-safety bugs and fuzzing that
# finds segfaults.
#
# The cost is roughly 2x throughput and a much larger address space. Build
# without them with: make fuzz-build FUZZ_SAN=0
FUZZ_SAN ?= 1
ifeq ($(FUZZ_SAN),1)
AFL_SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
                 -fno-omit-frame-pointer
# ASan reserves terabytes of address space for its shadow map, so AFL's per
# execution memory cap has to come off or every run dies as a false OOM.
AFL_MEM_LIMIT := none
else
AFL_SAN_FLAGS :=
AFL_MEM_LIMIT := 200
endif

AFL_CFLAGS := -O2 -g $(AFL_SAN_FLAGS) $(INCLUDE)
AFL_LDFLAGS := $(AFL_SAN_FLAGS)

# Sanitizer runtime options for the fuzz targets.
#
# abort_on_error/symbolize: required by afl-fuzz, which refuses to start with
# custom options that would let an error be reported without crashing.
# allocator_may_return_null: a fuzzed length field asking for 100 GB is an
# allocation failure the library is supposed to handle, not a finding. Without
# this ASan aborts on the request itself and every such input looks like a bug.
# detect_leaks: off because these harnesses fork per execution, so LeakSanitizer
# would run a full scan on every input. Leaks are worth hunting separately:
#   make fuzz-rle-decoder AFL_ENV='AFL_CRASH_EXITCODE=23' ASAN_LEAKS=1
ASAN_LEAKS ?= 0
AFL_SAN_ENV := ASAN_OPTIONS=abort_on_error=1:symbolize=0:allocator_may_return_null=1:detect_leaks=$(ASAN_LEAKS) \
               UBSAN_OPTIONS=abort_on_error=1:symbolize=0

# AFL environment variables for running fuzzer
# Set AFL_SKIP_CRASHES=1 to skip core_pattern check (for WSL2/testing)
# For proper crash detection: echo core | sudo tee /proc/sys/kernel/core_pattern
AFL_ENV ?=
# Stop after a fixed number of seconds instead of running until Ctrl+C, which
# is what a scripted or CI run wants: make fuzz-rle-decoder FUZZ_TIME=300
# Empty (the default) keeps the interactive behaviour of running until stopped.
FUZZ_TIME ?=
AFL_TIME_FLAG := $(if $(FUZZ_TIME),-V $(FUZZ_TIME),)

# cutil is a shared library, and the harnesses call into it through the
# allocator, so the fuzz targets need it on the library path the same way the
# test binaries do.
AFL_RUN_ENV = LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(AFL_SAN_ENV) $(AFL_ENV)

# AFL-instrumented object files and library (separate from regular build)
AFL_OBJ_DIR := $(BUILD_DIR)/afl-objects
AFL_FLAGS_STAMP := $(AFL_OBJ_DIR)/.flags
AFL_LIBOBJECTS := $(patsubst src/%.c,$(AFL_OBJ_DIR)/%.o,$(SOURCES))
AFL_STATIC_TARGET := $(BASE_NAME_PREFIX)-afl.a

# Pattern rule for AFL-instrumented object files
#
# -MMD, and the -include below, are not optional here even though the rule
# worked without them for a long time.  Without a depfile these objects are
# rebuilt only when their own .c changes, so a change to a header left the AFL
# library built from a mix of old and new declarations - and a fuzzing campaign
# against a mixed build reports crashes that are artefacts of the build rather
# than defects in the code.  It happened: adding two fields to
# zstd_match_finder_t made `make fuzz-replay` fail four inputs with a
# misaligned-pointer report from UBSan, in a build where one translation unit
# still had the old struct layout.  The regular and ASan builds already do
# this; this rule was the one that did not.
$(AFL_OBJ_DIR)/%.o: src/%.c $(AFL_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling (AFL instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(AFL_CC) $(AFL_CFLAGS) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

AFL_DEPFILES := $(AFL_LIBOBJECTS:.o=.d)
-include $(AFL_DEPFILES)

# AFL-instrumented static library
$(APP_DIR)/$(AFL_STATIC_TARGET): $(AFL_LIBOBJECTS)
	@printf "\n### Archiving AFL-instrumented Static Library ###\n"
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

# Corpus generator (built with regular gcc, no AFL instrumentation)
#
# Linked against the ordinary static library, not the AFL one: it is a tool
# that runs once, and it needs the encoders to produce a seed for every method
# rather than carrying hand-written frames for seven formats - the hand-written
# ones went stale for four of them and nobody noticed, because an empty seed
# directory only shows up when somebody starts a campaign.
$(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION): fuzz/generate_corpus.c \
		$(APP_DIR)/$(STATIC_TARGET)
	@printf "\n### Compiling Corpus Generator ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< \
		-Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive \
		$(CUTIL_LIBS) -lm -lpthread -Wl,-rpath,$(PREFIX)/lib/$(SUITE)

# Pattern rule for fuzz executables (linked against AFL-instrumented library)
#
# --whole-archive for the same reason the test binaries use it, one line 300
# above: a method registers itself from a constructor, nothing references that
# object, and a static link drops it. Without it the registry comes up empty,
# every gcomp_decoder_create() fails with an unknown method, and the harness
# returns having done nothing.
#
# That is not a theory about what could go wrong. afl-showmap on the harness as
# it was linked reports the same 73 edges for a valid zstd frame and for 200
# bytes of /dev/urandom, because neither reaches a decoder; with the archive
# linked whole it is 340 against 175. Thirteen million executions were spent
# fuzzing read_stdin().
$(APP_DIR)/fuzz/%$(EXE_EXTENSION): fuzz/%.c $(APP_DIR)/$(AFL_STATIC_TARGET)
	@printf "\n### Compiling Fuzz Harness: $* ###\n"
	@mkdir -p $(@D)
	$(AFL_CC) $(AFL_CFLAGS) $(AFL_LDFLAGS) -o $@ $< \
		-MMD -MP -MF $(@:$(EXE_EXTENSION)=.d) \
		-Wl,--whole-archive $(APP_DIR)/$(AFL_STATIC_TARGET) -Wl,--no-whole-archive \
		$(CUTIL_LIBS) -lm -lpthread

# Same reason as the objects above: a harness includes the public headers.
FUZZ_DEPFILES := $(patsubst fuzz/%.c,$(APP_DIR)/fuzz/%.d,$(FUZZ_SOURCES))
-include $(FUZZ_DEPFILES)

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf examples bench bench-deflate coverage check-symbols check-clean-guard check-aliasing check-test-build check-stamps check-san-report
# Release build commands
.PHONY: all install test test-quiet test-valgrind test-valgrind-quiet test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug
# Fuzz commands
.PHONY: fuzz-build fuzz-corpus fuzz-replay fuzz-decoder fuzz-encoder fuzz-roundtrip fuzz-help
.PHONY: fuzz-gzip-decoder fuzz-gzip-encoder fuzz-gzip-roundtrip
.PHONY: fuzz-lz4-decoder fuzz-lz4-encoder fuzz-lz4-roundtrip
.PHONY: fuzz-rle-decoder fuzz-rle-encoder fuzz-rle-roundtrip
.PHONY: fuzz-lzw-decoder fuzz-lzw-encoder fuzz-lzw-roundtrip
.PHONY: fuzz-zstd-decoder fuzz-zstd-encoder fuzz-zstd-roundtrip
.PHONY: fuzz-zlib-decoder fuzz-zlib-encoder fuzz-zlib-roundtrip
.PHONY: fuzz-brotli-decoder fuzz-brotli-encoder fuzz-brotli-roundtrip
# Sanitizer commands
.PHONY: test-asan test-asan-quiet test-ubsan sanitizer-help
.PHONY: test-tsan test-tsan-quiet test-tsan-threads
watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make --no-print-directory all; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make --no-print-directory all; \
		make --no-print-directory test; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

examples: ## Build all examples
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Examples built       ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Examples are available in: $(APP_DIR)/examples/\n"
	@printf "\n"
	@printf "\033[0;33mTo run examples:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export LD_LIBRARY_PATH=\"$(APP_DIR):$$LD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export DYLD_LIBRARY_PATH=\"$(APP_DIR):$$DYLD_LIBRARY_PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): The DLL must be in the same directory or in PATH.\n"
	@printf "  Option 1 - Run from the library directory:\n"
	@printf "    cd $(APP_DIR)\n"
	@printf "    ./examples/<example>$(EXE_EXTENSION)\n"
	@printf "  Option 2 - Add library directory to PATH:\n"
	@printf "    export PATH=\"$(APP_DIR):$$PATH\"\n"
	@printf "    $(APP_DIR)/examples/<example>$(EXE_EXTENSION)\n"
	@printf "  Option 3 - Copy DLL to example directories:\n"
	@printf "    cp $(APP_DIR)/$(TARGET) $(APP_DIR)/examples/\n"
	@printf "    Then run: $(APP_DIR)/examples/<example>$(EXE_EXTENSION)\n"
endif
	@printf "\n"

bench: ## Build all benchmarks
bench: $(APP_DIR)/$(TARGET) $(BENCHMARKS)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Benchmarks built     ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Benchmarks are available in: $(APP_DIR)/bench/\n"
	@printf "\n"
	@printf "\033[0;33mTo run benchmarks:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH and run:\n"
	@printf "    LD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_deflate\n"
	@printf "    LD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_lzw\n"
	@printf "    LD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_ratio [FILE...]\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH and run:\n"
	@printf "    DYLD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_deflate\n"
	@printf "    DYLD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_lzw\n"
	@printf "    DYLD_LIBRARY_PATH=\"$(APP_DIR)\" $(APP_DIR)/bench/bench_ratio [FILE...]\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): Run from library directory:\n"
	@printf "    cd $(APP_DIR) && ./bench/bench_deflate$(EXE_EXTENSION)\n"
	@printf "    cd $(APP_DIR) && ./bench/bench_ratio$(EXE_EXTENSION) [FILE...]\n"
	@printf "    cd $(APP_DIR) && ./bench/bench_lzw$(EXE_EXTENSION)\n"
endif
	@printf "\n"

bench-deflate: ## Build and run the deflate benchmark
bench-deflate: $(APP_DIR)/$(TARGET) $(APP_DIR)/bench/bench_deflate$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Running Deflate Benchmark ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(APP_DIR)/bench/bench_deflate$(EXE_EXTENSION)

fuzz-help: ## Show fuzzing help and instructions
	@printf "\033[0;36m"
	@printf "####################################\n"
	@printf "### Fuzz Testing with AFL++      ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@printf "Prerequisites:\n"
	@printf "  sudo apt install afl++\n"
	@printf "\n"
	@printf "Available targets:\n"
	@printf "  make fuzz-build           - Build library and harnesses with AFL instrumentation\n"
	@printf "  make fuzz-corpus          - Generate seed corpus from test vectors\n"
	@printf "\n"
	@printf "  Deflate fuzzers:\n"
	@printf "    make fuzz-decoder       - Run deflate decoder fuzzer\n"
	@printf "    make fuzz-encoder       - Run deflate encoder fuzzer\n"
	@printf "    make fuzz-roundtrip     - Run deflate roundtrip fuzzer\n"
	@printf "\n"
	@printf "  Gzip fuzzers:\n"
	@printf "    make fuzz-gzip-decoder  - Run gzip decoder fuzzer\n"
	@printf "    make fuzz-gzip-encoder  - Run gzip encoder fuzzer\n"
	@printf "    make fuzz-gzip-roundtrip- Run gzip roundtrip fuzzer\n"
	@printf "\n"
	@printf "  LZ4 fuzzers:\n"
	@printf "    make fuzz-lz4-decoder   - Run LZ4 decoder fuzzer\n"
	@printf "    make fuzz-lz4-encoder   - Run LZ4 encoder fuzzer\n"
	@printf "    make fuzz-lz4-roundtrip - Run LZ4 roundtrip fuzzer\n"
	@printf "\n"
	@printf "  RLE fuzzers:\n"
	@printf "    make fuzz-rle-decoder   - Run RLE decoder fuzzer\n"
	@printf "    make fuzz-rle-encoder   - Run RLE encoder fuzzer\n"
	@printf "    make fuzz-rle-roundtrip - Run RLE roundtrip fuzzer\n"
	@printf "\n"
	@printf "  LZW fuzzers:\n"
	@printf "    make fuzz-lzw-decoder   - Run LZW decoder fuzzer\n"
	@printf "    make fuzz-lzw-encoder   - Run LZW encoder fuzzer\n"
	@printf "    make fuzz-lzw-roundtrip - Run LZW roundtrip fuzzer\n"
	@printf "\n"
	@printf "  Zstd fuzzers:\n"
	@printf "    make fuzz-zstd-decoder  - Run Zstd decoder fuzzer\n"
	@printf "    make fuzz-zstd-encoder  - Run Zstd encoder fuzzer\n"
	@printf "    make fuzz-zstd-roundtrip- Run Zstd roundtrip fuzzer\n"
	@printf "\n"
	@printf "  Zlib fuzzers:\n"
	@printf "    make fuzz-zlib-decoder  - Run zlib decoder fuzzer\n"
	@printf "    make fuzz-zlib-encoder  - Run zlib encoder fuzzer\n"
	@printf "    make fuzz-zlib-roundtrip- Run zlib roundtrip fuzzer\n"
	@printf "\n"
	@printf "  Brotli fuzzers:\n"
	@printf "    make fuzz-brotli-decoder  - Run Brotli decoder fuzzer\n"
	@printf "    make fuzz-brotli-encoder  - Run Brotli encoder fuzzer\n"
	@printf "    make fuzz-brotli-roundtrip- Run Brotli roundtrip fuzzer\n"
	@printf "\n"
	@printf "Workflow:\n"
	@printf "  1. make fuzz-corpus        # Generate seed inputs\n"
	@printf "  2. make fuzz-build         # Build library + harnesses with AFL\n"
	@printf "  3. make fuzz-lz4-decoder   # Start fuzzing (Ctrl+C to stop)\n"
	@printf "\n"
	@printf "Core pattern setup (for crash detection):\n"
	@printf "  echo core | sudo tee /proc/sys/kernel/core_pattern\n"
	@printf "\n"
	@printf "Or skip the check (for quick testing on WSL2):\n"
	@printf "  make fuzz-decoder AFL_ENV='AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1'\n"
	@printf "\n"
	@printf "Sanitizers:\n"
	@printf "  The fuzz build uses ASan and UBSan by default, so that a heap\n"
	@printf "  overflow or undefined behaviour becomes a crash AFL can see\n"
	@printf "  instead of a silently successful run.\n"
	@printf "  Faster, less thorough:  make fuzz-build FUZZ_SAN=0\n"
	@printf "  Hunt leaks as well:     make fuzz-<target> ASAN_LEAKS=1 AFL_ENV='AFL_CRASH_EXITCODE=23'\n"
	@printf "\n"
	@printf "Findings are saved to: fuzz/findings/<target>/\n"
	@printf "  - crashes/  : Inputs that caused crashes\n"
	@printf "  - hangs/    : Inputs that caused timeouts\n"
	@printf "  - queue/    : Interesting inputs for coverage\n"
	@printf "\n"
	@printf "Documentation: documentation/testing/fuzzing.md\n"
	@printf "\n"

fuzz-replay: ## Replay the tracked corpus through every harness (no fuzzing)
# What this is for
# ================
#
# afl-fuzz mutates and writes; this only reads. Every file in fuzz/regression
# is fed to every harness, and any harness that does not exit cleanly fails the
# target. That makes it a regression check rather than a search: the inputs
# that once found something stay found, and a change that reintroduces one is
# caught in seconds rather than on whoever next runs a campaign.
#
# fuzz/corpus and fuzz/findings are gitignored working directories - they are
# afl-fuzz's, they grow, and they are not a record of anything. Running
# afl-fuzz against a corpus directory *adds to it*, so they cannot be the thing
# that is checked in. fuzz/regression is tracked, small, and never written to.
#
# Every file goes to every harness deliberately. Each one reads arbitrary bytes
# from stdin, so a deflate stream is a perfectly good input to the zstd decoder
# - a decoder must reject what is not its format as safely as it rejects a
# corrupt example of it. The harnesses are built with ASan and UBSan through
# the same FUZZ_SAN path afl-fuzz uses, so a memory error aborts.
fuzz-replay: $(FUZZ_EXECUTABLES)
	@printf "\033[0;36m\n"
	@printf "###############################################\n"
	@printf "### Replaying the tracked corpus (read-only) ###\n"
	@printf "###############################################\n"
	@printf "\033[0m\n"
	@before=$$(find fuzz/regression -type f | sort | xargs cat 2>/dev/null | md5sum); \
	files=$$(find fuzz/regression -type f | sort); \
	nfiles=$$(printf "%s\n" "$$files" | grep -c . || true); \
	total=0; failed=0; \
	for exe in $(FUZZ_EXECUTABLES); do \
		name=$$(basename $$exe); \
		for f in $$files; do \
			total=$$((total+1)); \
			$(AFL_RUN_ENV) $$exe < $$f > /dev/null 2>&1; \
			rc=$$?; \
			if [ $$rc -ne 0 ]; then \
				printf "\033[0;31m  %s <- %s (exit %s)\033[0m\n" "$$name" "$$f" "$$rc" >&2; \
				failed=$$((failed+1)); \
			fi; \
		done; \
		printf "  %-28s %s inputs\n" "$$name" "$$nfiles"; \
	done; \
	after=$$(find fuzz/regression -type f | sort | xargs cat 2>/dev/null | md5sum); \
	if [ "$$before" != "$$after" ]; then \
		printf "\033[0;31m\nThe corpus changed during a replay. It must not.\033[0m\n" >&2; \
		exit 1; \
	fi; \
	if [ $$failed -ne 0 ]; then \
		printf "\033[0;31m\n%d of %d replays failed.\033[0m\n" "$$failed" "$$total" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32m\n%d replays, no crashes, corpus unchanged.\033[0m\n" "$$total"

fuzz-build: ## Build all fuzz harnesses with AFL instrumentation
fuzz-build: $(FUZZ_EXECUTABLES)
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### Fuzz harnesses built (instrumented) ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@printf "AFL-instrumented library: $(APP_DIR)/$(AFL_STATIC_TARGET)\n"
	@printf "Harnesses are available in: $(APP_DIR)/fuzz/\n"
	@for exe in $(FUZZ_EXECUTABLES); do \
		printf "  - %s\n" "$$exe"; \
	done
	@printf "\n"
	@printf "Run 'make fuzz-help' for usage instructions.\n"
	@printf "\n"

fuzz-corpus: ## Generate seed corpus from test vectors
fuzz-corpus: $(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Generating Seed Corpus       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@$(APP_DIR)/fuzz/generate_corpus$(EXE_EXTENSION)
# Every campaign target below names its seed directory with -i, so the list of
# directories that must exist is read out of this file rather than kept as a
# second list beside it. The two lists had already drifted: the LZ4 and zstd
# campaigns named directories nothing created, and fuzz-gzip-roundtrip named
# one that was created and left empty. afl-fuzz refuses to start on an empty
# input directory, so each of those was a campaign that could not be run.
	@missing=""; \
	for dir in $$(grep -oE '\-i fuzz/corpus/[a-z0-9_]+' $(MAKEFILE_LIST) \
			| awk '{print $$2}' | sort -u); do \
		if [ -z "$$(find $$dir -type f 2>/dev/null | head -n 1)" ]; then \
			missing="$$missing $$dir"; \
		fi; \
	done; \
	if [ -n "$$missing" ]; then \
		printf "\033[0;31m\nNo seeds for:%s\n" "$$missing" >&2; \
		printf "A campaign target names a corpus directory that fuzz/generate_corpus.c does not fill.\033[0m\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery campaign target has a seed corpus.\033[0m\n"

fuzz-decoder: ## Run decoder fuzzer (Ctrl+C to stop)
fuzz-decoder: $(APP_DIR)/fuzz/fuzz_deflate_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Decoder Fuzzer       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/decoder
	@if [ ! -d fuzz/corpus/decoder ] || [ -z "$$(ls -A fuzz/corpus/decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/decoder; \
		printf '\x01\x00\x00\xff\xff' > fuzz/corpus/decoder/empty.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/decoder -o fuzz/findings/decoder -- $(APP_DIR)/fuzz/fuzz_deflate_decoder$(EXE_EXTENSION)

fuzz-encoder: ## Run encoder fuzzer (Ctrl+C to stop)
fuzz-encoder: $(APP_DIR)/fuzz/fuzz_deflate_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Encoder Fuzzer       ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/encoder
	@if [ ! -d fuzz/corpus/encoder ] || [ -z "$$(ls -A fuzz/corpus/encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/encoder; \
		printf 'Hello' > fuzz/corpus/encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/encoder -o fuzz/findings/encoder -- $(APP_DIR)/fuzz/fuzz_deflate_encoder$(EXE_EXTENSION)

fuzz-roundtrip: ## Run roundtrip fuzzer (Ctrl+C to stop)
fuzz-roundtrip: $(APP_DIR)/fuzz/fuzz_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "####################################\n"
	@printf "### Running Roundtrip Fuzzer     ###\n"
	@printf "####################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/roundtrip
	@if [ ! -d fuzz/corpus/roundtrip ] || [ -z "$$(ls -A fuzz/corpus/roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/roundtrip; \
		printf 'Hello' > fuzz/corpus/roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/roundtrip -o fuzz/findings/roundtrip -- $(APP_DIR)/fuzz/fuzz_roundtrip$(EXE_EXTENSION)

fuzz-gzip-decoder: ## Run gzip decoder fuzzer (Ctrl+C to stop)
fuzz-gzip-decoder: $(APP_DIR)/fuzz/fuzz_gzip_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Decoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_decoder
	@if [ ! -d fuzz/corpus/gzip_decoder ] || [ -z "$$(ls -A fuzz/corpus/gzip_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_decoder; \
		printf '\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00' > fuzz/corpus/gzip_decoder/empty.gz; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/gzip_decoder -o fuzz/findings/gzip_decoder -- $(APP_DIR)/fuzz/fuzz_gzip_decoder$(EXE_EXTENSION)

fuzz-gzip-encoder: ## Run gzip encoder fuzzer (Ctrl+C to stop)
fuzz-gzip-encoder: $(APP_DIR)/fuzz/fuzz_gzip_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Encoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_encoder
	@if [ ! -d fuzz/corpus/gzip_encoder ] || [ -z "$$(ls -A fuzz/corpus/gzip_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_encoder; \
		printf 'Hello' > fuzz/corpus/gzip_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/gzip_encoder -o fuzz/findings/gzip_encoder -- $(APP_DIR)/fuzz/fuzz_gzip_encoder$(EXE_EXTENSION)

fuzz-gzip-roundtrip: ## Run gzip roundtrip fuzzer (Ctrl+C to stop)
fuzz-gzip-roundtrip: $(APP_DIR)/fuzz/fuzz_gzip_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Gzip Roundtrip Fuzzer   ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/gzip_roundtrip
	@if [ ! -d fuzz/corpus/gzip_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/gzip_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/gzip_roundtrip; \
		printf 'Hello' > fuzz/corpus/gzip_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/gzip_roundtrip -o fuzz/findings/gzip_roundtrip -- $(APP_DIR)/fuzz/fuzz_gzip_roundtrip$(EXE_EXTENSION)

fuzz-lz4-decoder: ## Run LZ4 decoder fuzzer (Ctrl+C to stop)
fuzz-lz4-decoder: $(APP_DIR)/fuzz/fuzz_lz4_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZ4 Decoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lz4_decoder
	@if [ ! -d fuzz/corpus/lz4_decoder ] || [ -z "$$(ls -A fuzz/corpus/lz4_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/lz4_decoder; \
		printf '\x04\x22\x4d\x18\x60\x70\xdf\x00\x00\x00\x00' > fuzz/corpus/lz4_decoder/empty.lz4; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lz4_decoder -o fuzz/findings/lz4_decoder -- $(APP_DIR)/fuzz/fuzz_lz4_decoder$(EXE_EXTENSION)

fuzz-lz4-encoder: ## Run LZ4 encoder fuzzer (Ctrl+C to stop)
fuzz-lz4-encoder: $(APP_DIR)/fuzz/fuzz_lz4_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZ4 Encoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lz4_encoder
	@if [ ! -d fuzz/corpus/lz4_encoder ] || [ -z "$$(ls -A fuzz/corpus/lz4_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/lz4_encoder; \
		printf 'Hello' > fuzz/corpus/lz4_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lz4_encoder -o fuzz/findings/lz4_encoder -- $(APP_DIR)/fuzz/fuzz_lz4_encoder$(EXE_EXTENSION)

fuzz-lz4-roundtrip: ## Run LZ4 roundtrip fuzzer (Ctrl+C to stop)
fuzz-lz4-roundtrip: $(APP_DIR)/fuzz/fuzz_lz4_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZ4 Roundtrip Fuzzer   ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lz4_roundtrip
	@if [ ! -d fuzz/corpus/lz4_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/lz4_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/lz4_roundtrip; \
		printf 'Hello' > fuzz/corpus/lz4_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lz4_roundtrip -o fuzz/findings/lz4_roundtrip -- $(APP_DIR)/fuzz/fuzz_lz4_roundtrip$(EXE_EXTENSION)

fuzz-rle-decoder: ## Run RLE decoder fuzzer (Ctrl+C to stop)
fuzz-rle-decoder: $(APP_DIR)/fuzz/fuzz_rle_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running RLE Decoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/rle_decoder
	@if [ ! -d fuzz/corpus/rle_decoder ] || [ -z "$$(ls -A fuzz/corpus/rle_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/rle_decoder; \
		printf '\x02ABC' > fuzz/corpus/rle_decoder/abc.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/rle_decoder -o fuzz/findings/rle_decoder -- $(APP_DIR)/fuzz/fuzz_rle_decoder$(EXE_EXTENSION)

fuzz-rle-encoder: ## Run RLE encoder fuzzer (Ctrl+C to stop)
fuzz-rle-encoder: $(APP_DIR)/fuzz/fuzz_rle_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running RLE Encoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/rle_encoder
	@if [ ! -d fuzz/corpus/rle_encoder ] || [ -z "$$(ls -A fuzz/corpus/rle_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/rle_encoder; \
		printf 'Hello' > fuzz/corpus/rle_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/rle_encoder -o fuzz/findings/rle_encoder -- $(APP_DIR)/fuzz/fuzz_rle_encoder$(EXE_EXTENSION)

fuzz-rle-roundtrip: ## Run RLE roundtrip fuzzer (Ctrl+C to stop)
fuzz-rle-roundtrip: $(APP_DIR)/fuzz/fuzz_rle_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running RLE Roundtrip Fuzzer   ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/rle_roundtrip
	@if [ ! -d fuzz/corpus/rle_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/rle_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/rle_roundtrip; \
		printf 'Hello' > fuzz/corpus/rle_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/rle_roundtrip -o fuzz/findings/rle_roundtrip -- $(APP_DIR)/fuzz/fuzz_rle_roundtrip$(EXE_EXTENSION)

fuzz-lzw-decoder: ## Run LZW decoder fuzzer (Ctrl+C to stop)
fuzz-lzw-decoder: $(APP_DIR)/fuzz/fuzz_lzw_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZW Decoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lzw_decoder
	@if [ ! -d fuzz/corpus/lzw_decoder ] || [ -z "$$(ls -A fuzz/corpus/lzw_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/lzw_decoder; \
		printf '\x00\x03\x02' > fuzz/corpus/lzw_decoder/clear_eoi.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lzw_decoder -o fuzz/findings/lzw_decoder -- $(APP_DIR)/fuzz/fuzz_lzw_decoder$(EXE_EXTENSION)

fuzz-lzw-encoder: ## Run LZW encoder fuzzer (Ctrl+C to stop)
fuzz-lzw-encoder: $(APP_DIR)/fuzz/fuzz_lzw_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZW Encoder Fuzzer     ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lzw_encoder
	@if [ ! -d fuzz/corpus/lzw_encoder ] || [ -z "$$(ls -A fuzz/corpus/lzw_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/lzw_encoder; \
		printf 'Hello' > fuzz/corpus/lzw_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lzw_encoder -o fuzz/findings/lzw_encoder -- $(APP_DIR)/fuzz/fuzz_lzw_encoder$(EXE_EXTENSION)

fuzz-lzw-roundtrip: ## Run LZW roundtrip fuzzer (Ctrl+C to stop)
fuzz-lzw-roundtrip: $(APP_DIR)/fuzz/fuzz_lzw_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "######################################\n"
	@printf "### Running LZW Roundtrip Fuzzer   ###\n"
	@printf "######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/lzw_roundtrip
	@if [ ! -d fuzz/corpus/lzw_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/lzw_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Run 'make fuzz-corpus' first.\033[0m\n"; \
		printf "Creating minimal seed...\n"; \
		mkdir -p fuzz/corpus/lzw_roundtrip; \
		printf 'Hello' > fuzz/corpus/lzw_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/lzw_roundtrip -o fuzz/findings/lzw_roundtrip -- $(APP_DIR)/fuzz/fuzz_lzw_roundtrip$(EXE_EXTENSION)

fuzz-zstd-decoder: ## Run Zstd decoder fuzzer (Ctrl+C to stop)
fuzz-zstd-decoder: $(APP_DIR)/fuzz/fuzz_zstd_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Zstd Decoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zstd_decoder
	@if [ ! -d fuzz/corpus/zstd_decoder ] || [ -z "$$(ls -A fuzz/corpus/zstd_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zstd_decoder; \
		printf '\x28\xb5\x2f\xfd\x00\x00\x01\x00\x00' > fuzz/corpus/zstd_decoder/empty.zst; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zstd_decoder -o fuzz/findings/zstd_decoder -- $(APP_DIR)/fuzz/fuzz_zstd_decoder$(EXE_EXTENSION)

fuzz-zstd-encoder: ## Run Zstd encoder fuzzer (Ctrl+C to stop)
fuzz-zstd-encoder: $(APP_DIR)/fuzz/fuzz_zstd_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Zstd Encoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zstd_encoder
	@if [ ! -d fuzz/corpus/zstd_encoder ] || [ -z "$$(ls -A fuzz/corpus/zstd_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zstd_encoder; \
		printf 'Hello' > fuzz/corpus/zstd_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zstd_encoder -o fuzz/findings/zstd_encoder -- $(APP_DIR)/fuzz/fuzz_zstd_encoder$(EXE_EXTENSION)

fuzz-zstd-roundtrip: ## Run Zstd roundtrip fuzzer (Ctrl+C to stop)
fuzz-zstd-roundtrip: $(APP_DIR)/fuzz/fuzz_zstd_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running Zstd Roundtrip Fuzzer   ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zstd_roundtrip
	@if [ ! -d fuzz/corpus/zstd_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/zstd_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zstd_roundtrip; \
		printf 'Hello' > fuzz/corpus/zstd_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zstd_roundtrip -o fuzz/findings/zstd_roundtrip -- $(APP_DIR)/fuzz/fuzz_zstd_roundtrip$(EXE_EXTENSION)

fuzz-brotli-decoder: ## Run Brotli decoder fuzzer (Ctrl+C to stop)
fuzz-brotli-decoder: $(APP_DIR)/fuzz/fuzz_brotli_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#########################################\n"
	@printf "### Running Brotli Decoder Fuzzer     ###\n"
	@printf "#########################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/brotli_decoder
	@if [ ! -d fuzz/corpus/brotli_decoder ] || [ -z "$$(ls -A fuzz/corpus/brotli_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/brotli_decoder; \
		printf '\x06' > fuzz/corpus/brotli_decoder/empty.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/brotli_decoder -o fuzz/findings/brotli_decoder -- $(APP_DIR)/fuzz/fuzz_brotli_decoder$(EXE_EXTENSION)

fuzz-brotli-encoder: ## Run Brotli encoder fuzzer (Ctrl+C to stop)
fuzz-brotli-encoder: $(APP_DIR)/fuzz/fuzz_brotli_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#########################################\n"
	@printf "### Running Brotli Encoder Fuzzer     ###\n"
	@printf "#########################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/brotli_encoder
	@if [ ! -d fuzz/corpus/brotli_encoder ] || [ -z "$$(ls -A fuzz/corpus/brotli_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/brotli_encoder; \
		printf 'Hello' > fuzz/corpus/brotli_encoder/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/brotli_encoder -o fuzz/findings/brotli_encoder -- $(APP_DIR)/fuzz/fuzz_brotli_encoder$(EXE_EXTENSION)

fuzz-brotli-roundtrip: ## Run Brotli roundtrip fuzzer (Ctrl+C to stop)
fuzz-brotli-roundtrip: $(APP_DIR)/fuzz/fuzz_brotli_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#########################################\n"
	@printf "### Running Brotli Roundtrip Fuzzer   ###\n"
	@printf "#########################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/brotli_roundtrip
	@if [ ! -d fuzz/corpus/brotli_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/brotli_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/brotli_roundtrip; \
		printf 'Hello' > fuzz/corpus/brotli_roundtrip/hello.bin; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/brotli_roundtrip -o fuzz/findings/brotli_roundtrip -- $(APP_DIR)/fuzz/fuzz_brotli_roundtrip$(EXE_EXTENSION)

# The zlib harnesses existed for a while with no way to be run: fuzz-replay fed
# them, because it feeds every harness it finds, but there was no campaign
# target for any of them and fuzz-help did not mention zlib at all.
#
# There is a zlib encoder harness now. The note that used to stand here said
# fuzz_zlib_roundtrip covered that direction "the way the other roundtrip
# harnesses do", and that was wrong twice over: deflate, gzip, lz4, lzw, rle
# and zstd each have an encoder harness *and* a roundtrip one, so zlib was the
# only format missing one; and fuzz_zlib_roundtrip calls gcomp_encode_buffer
# only, with an output buffer sized for the whole stream and no zlib option
# set, so it never drove the streaming encoder, never drove flush, and never
# once set zlib.dictionary - the FDICT/DICTID encode path had never been fuzzed.
fuzz-zlib-decoder: ## Run zlib decoder fuzzer (Ctrl+C to stop)
fuzz-zlib-decoder: $(APP_DIR)/fuzz/fuzz_zlib_decoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running zlib Decoder Fuzzer     ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zlib_decoder
	@if [ ! -d fuzz/corpus/zlib_decoder ] || [ -z "$$(ls -A fuzz/corpus/zlib_decoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zlib_decoder; \
		printf '\x78\x9c\x03\x00\x00\x00\x00\x01' > fuzz/corpus/zlib_decoder/empty.zz; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zlib_decoder -o fuzz/findings/zlib_decoder -- $(APP_DIR)/fuzz/fuzz_zlib_decoder$(EXE_EXTENSION)

fuzz-zlib-encoder: ## Run zlib encoder fuzzer (Ctrl+C to stop)
fuzz-zlib-encoder: $(APP_DIR)/fuzz/fuzz_zlib_encoder$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "########################################\n"
	@printf "### Running zlib Encoder Fuzzer     ###\n"
	@printf "########################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zlib_encoder
	@if [ ! -d fuzz/corpus/zlib_encoder ] || [ -z "$$(ls -A fuzz/corpus/zlib_encoder 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zlib_encoder; \
		printf 'Hello, world!' > fuzz/corpus/zlib_encoder/hello.txt; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zlib_encoder -o fuzz/findings/zlib_encoder -- $(APP_DIR)/fuzz/fuzz_zlib_encoder$(EXE_EXTENSION)

fuzz-zlib-roundtrip: ## Run zlib roundtrip fuzzer (Ctrl+C to stop)
fuzz-zlib-roundtrip: $(APP_DIR)/fuzz/fuzz_zlib_roundtrip$(EXE_EXTENSION)
	@printf "\033[0;32m\n"
	@printf "#######################################\n"
	@printf "### Running zlib Roundtrip Fuzzer   ###\n"
	@printf "#######################################\n"
	@printf "\033[0m\n"
	@mkdir -p fuzz/findings/zlib_roundtrip
	@if [ ! -d fuzz/corpus/zlib_roundtrip ] || [ -z "$$(ls -A fuzz/corpus/zlib_roundtrip 2>/dev/null)" ]; then \
		printf "\033[0;33mWarning: No seed corpus found. Creating minimal seed...\033[0m\n"; \
		mkdir -p fuzz/corpus/zlib_roundtrip; \
		printf 'Hello, world!' > fuzz/corpus/zlib_roundtrip/hello.txt; \
	fi
	$(AFL_RUN_ENV) afl-fuzz -m $(AFL_MEM_LIMIT) $(AFL_TIME_FLAG) -i fuzz/corpus/zlib_roundtrip -o fuzz/findings/zlib_roundtrip -- $(APP_DIR)/fuzz/fuzz_zlib_roundtrip$(EXE_EXTENSION)

# So tests and fuzz harnesses can load the compress library and its cutil
# dependency.
#
# CUTIL_SIBLING_DIR was left behind when the sibling-checkout fallback was
# removed in favour of resolving dependencies through pkg-config only. Being
# undefined, it expanded to nothing and this variable named "/apps" - a
# directory that does not exist. The test binaries never noticed: they link the
# static archive and carry an rpath to the prefix. The fuzz harnesses link
# cutil dynamically and do not, so every one of them failed to start:
#
#   error while loading shared libraries: libghoti.io-cutil-0.so.0
#
# which is a large part of why so little fuzzing has been done here.
TEST_LD_PATH := $(APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)

####################################################################
# Symbol namespace check
####################################################################

check-symbols: ## Fail if any exported symbol lacks the version namespace
check-symbols: $(APP_DIR)/$(TARGET)
ifeq ($(OS_NAME), Linux)
# mangle_path is gcov's, not ours: a --coverage build links it into the library
# and it is the only symbol libgcov exports whose name does not begin with an
# underscore, so it is the only one the '^_' filter above misses. Without this
# line `make coverage` fails here - after the instrumented build and before the
# clean that would undo it - leaving instrumented objects that a later plain
# `make` silently links.
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' \
		| grep -v '^_' \
		| grep -v '^mangle_path$$' || true); \
	if [ -n "$$leaked" ]; then \
		printf "\033[0;31m\n### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\033[0m\n" >&2; \
		printf "%s\n" "$$leaked" >&2; \
		printf "\nEach needs a '#define <name> GHOTIIO_COMPRESS(<name>)' line in the\n" >&2; \
		printf "header that declares it. Without one, two versions of this library\n" >&2; \
		printf "cannot be loaded into the same process. See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@unexported=$$(grep -hE '^[a-z_][A-Za-z0-9_ ]*\**[[:space:]]*\bgcomp_[a-z0-9_]+[[:space:]]*\(' \
		include/ghoti.io/$(PROJECT)/*.h | grep -v '^typedef' || true); \
	if [ -n "$$unexported" ]; then \
		printf "\033[0;31m\n### Public declarations without GCOMP_API ###\033[0m\n" >&2; \
		printf "%s\n" "$$unexported" >&2; \
		printf "\nA public header declares these without the export macro, so they are\n" >&2; \
		printf "hidden in the shared library. The tests link the archive and would not\n" >&2; \
		printf "notice; a consumer linking the .so gets an undefined reference.\n" >&2; \
		exit 1; \
	fi
# A type has no linkage, so nothing in the built library can be inspected to
# find one that was never renamed - the checks above read `nm` output and are
# structurally blind to this. Read the headers instead, and require every type
# name and struct tag a public header declares to have a line in namespace.h.
#
# The gap was real: gcomp_seekable_t and its _s tag went un-renamed when
# seekable.h was added, while all eight of its functions were listed, and every
# check-symbols run in between passed. Two versions of this library would have
# shared one spelling for a struct whose layout is free to differ between them,
# which is the exact confusion CONVENTIONS.md section 4 exists to prevent.
	@untyped=$$( { \
		grep -hoE 'typedef +(struct|enum|union) +gcomp_[a-z0-9_]+ +gcomp_[a-z0-9_]+' include/ghoti.io/$(PROJECT)/*.h | grep -oE 'gcomp_[a-z0-9_]+'; \
		grep -hoE '(struct|enum|union) +gcomp_[a-z0-9_]+' include/ghoti.io/$(PROJECT)/*.h | awk '{print $$2}'; \
		grep -hoE '^\} *gcomp_[a-z0-9_]+' include/ghoti.io/$(PROJECT)/*.h | grep -oE 'gcomp_[a-z0-9_]+'; \
		grep -hoE 'typedef +[a-z0-9_ ]*\(\* *gcomp_[a-z0-9_]+\)' include/ghoti.io/$(PROJECT)/*.h | grep -oE 'gcomp_[a-z0-9_]+'; \
	} | sort -u | while read -r t; do \
		grep -q "^#define $$t " include/ghoti.io/$(PROJECT)/namespace.h || printf '%s\n' "$$t"; \
	done); \
	if [ -n "$$untyped" ]; then \
		printf "\033[0;31m\n### Public types missing the $(LIBVER_SYMBOL)_ namespace ###\033[0m\n" >&2; \
		printf "%s\n" "$$untyped" >&2; \
		printf "\nEach needs a '#define <name> GHOTIIO_COMPRESS(<name>)' line in\n" >&2; \
		printf "include/ghoti.io/$(PROJECT)/namespace.h. A type produces no symbol, so\n" >&2; \
		printf "the nm checks above cannot see this one. See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf "\033[0;31m\n### Renamed but undefined - a split symbol ###\033[0m\n" >&2; \
		printf "%s\n" "$$split" >&2; \
		printf "\nA translation unit referenced the namespaced name while the one that\n" >&2; \
		printf "defines it did not see the rename - usually an internal header that\n" >&2; \
		printf "declares or defines something without including macros.h first.\n" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/compress/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf "\033[0;31m\n### Headers that do not include macros.h ###\033[0m\n" >&2; \
		printf "%s\n" "$$nomacros" >&2; \
		printf "\nEvery header must include <ghoti.io/compress/macros.h> before it declares\n" >&2; \
		printf "anything, so that the renames in namespace.h are already in effect. A\n" >&2; \
		printf "header that skips it can name a type before that type has been renamed,\n" >&2; \
		printf "producing two different types under one spelling.\n" >&2; \
		printf "See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GCOMP_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf "\033[0;31m\n### Include guards with the wrong prefix ###\033[0m\n" >&2; \
		printf "%s\n" "$$badguards" >&2; \
		printf "\nGuards mirror the path: GHOTI_IO_GCOMP_<PATH>_H. A guard without the\n" >&2; \
		printf "library token is one rename away from colliding with another library's.\n" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf "\033[0;31m\n### Headers sharing an include guard ###\033[0m\n" >&2; \
		printf "%s\n" "$$dupguards" >&2; \
		printf "\nTwo headers with one guard means whichever is included second is\n" >&2; \
		printf "silently empty. Guards mirror the path: GHOTI_IO_GCOMP_<PATH>_H.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery exported symbol carries the $(LIBVER_SYMBOL)_ namespace.\033[0m\n"
	@printf "\033[0;32mEvery public type carries it too.\033[0m\n"
	@printf "\033[0;32mEvery public declaration carries GCOMP_API.\033[0m\n"
	@printf "\033[0;32mEvery header includes macros.h.\033[0m\n"
	@printf "\033[0;32mEvery include guard is unique and correctly prefixed.\033[0m\n"
else
	@printf "check-symbols: skipped (Linux only)\n"
endif

####################################################################
# Dependency-check guard
####################################################################

check-clean-guard: ## Fail if the pkg-config check gates the wrong goals
# Both directions, because either one alone passes for the wrong reason.
#
# Without SKIP_DEP_CHECK, `clean` inherits the $(error) that make evaluates
# while reading this file, so it exits 2 having removed nothing - and a clean
# that removed nothing looks exactly like a clean that had nothing to remove.
# With SKIP_DEP_CHECK set for every goal, the check never fires at all, and a
# build against a missing dependency fails later in the compiler, complaining
# about a header rather than about the prefix. With the suite installed those
# two states are indistinguishable, which is why "clean works now" is not a
# test of this.
#
# CUTIL_PC names a package that cannot exist, so this asks the question
# without depending on what happens to be installed: no PKG_CONFIG_PATH to
# unset, nothing to move out of the way, and the same answer on a machine
# where the real cutil sits in /usr/lib. -n throughout, so the goals that
# delete things evaluate this file without acting on it.
#
# The exempt half drives itself from DEPLESS_GOALS rather than a second copy
# of the list, so a goal added there cannot go untested.
#
# Three of the checked goals are chosen rather than obvious:
#
#   _none_      names NO goal. This is what proves $(or $(MAKECMDGOALS),all)
#               is live: written $(filter-out $(DEPLESS_GOALS),$(MAKECMDGOALS))
#               an empty goal list filters to empty and skips the check - and
#               every other spelling here passes anyway, because each names a
#               goal, which is exactly what that broken form handles.
#   clean+all   a mixed goal line. filter-out leaves `all`, so the check must
#               still fire even though `clean` alone is exempt.
#   fuzz-corpus links generate_corpus outside LDFLAGS, so it reaches cutil by
#               a route the rest of this file does not describe. It must not
#               become reachable from a listed goal.
	@absent=ghoti.io-no-such-package-0; \
		refused=""; \
		for goal in $(DEPLESS_GOALS); do \
			$(MAKE) -n -f $(firstword $(MAKEFILE_LIST)) $$goal CUTIL_PC=$$absent \
				>/dev/null 2>&1 || refused="$$refused $$goal"; \
		done; \
		if [ -n "$$refused" ]; then \
			printf "\033[0;31m\n### goals in DEPLESS_GOALS that need a dependency ###\033[0m\n" >&2; \
			printf "%s\n" "$$refused" >&2; \
			printf "\nThese failed with no cutil present, so the \$$(error) still\n" >&2; \
			printf "reaches them - and each exits having done nothing, which reads\n" >&2; \
			printf "as success. Check the ifndef SKIP_DEP_CHECK wrapper.\n" >&2; \
			exit 1; \
		fi; \
		unchecked=""; \
		for goal in _none_ all test install fuzz-build fuzz-corpus clean+all; do \
			case $$goal in \
				_none_) named="" ;; \
				clean+all) named="clean all" ;; \
				*) named=$$goal ;; \
			esac; \
			if $(MAKE) -n -f $(firstword $(MAKEFILE_LIST)) $$named CUTIL_PC=$$absent \
				>/dev/null 2>&1; then unchecked="$$unchecked $$goal"; fi; \
		done; \
		if [ -n "$$unchecked" ]; then \
			printf "\033[0;31m\n### goals that parsed with no dependency ###\033[0m\n" >&2; \
			printf "%s\n" "$$unchecked" >&2; \
			printf "\nThese succeeded with no cutil present. DEPLESS_GOALS is too\n" >&2; \
			printf "wide, or \$$(or \$$(MAKECMDGOALS),all) lost its default - an empty\n" >&2; \
			printf "goal list filters to empty and skips the check. _none_ alone\n" >&2; \
			printf "catches that one; the named goals cannot see it.\n" >&2; \
			exit 1; \
		fi
	@printf "\033[0;32mEvery dependency-free goal runs without one; every other goal is refused.\033[0m\n"

####################################################################
# Strict-aliasing gate
####################################################################

check-aliasing: ## Fail if the strict-aliasing warning is no longer armed
# $(ALIASING_CFLAGS) is what detects these violations, and it lives in CFLAGS
# under -Werror - so a real violation fails the build and no sweep is needed.
# A disarmed warning fails nothing and looks exactly like a clean library.
# This compiles a planted violation with the library's OWN flags and fails if
# it is accepted.
#
# Deliberately $(CFLAGS) and not a copy: a control compiled with flags written
# out beside it proves those flags work, which is not the question.
#
# THE SHAPE OF THE CONTROL IS LOAD-BEARING, and the requirement is that a
# control for a gate at level N must be caught at N and MISSED at N+1. The
# grid is in the comment on ALIASING_CFLAGS. This control is the address of a
# visible object cast through a pointer variable - caught at 1 and 2, missed
# at 3 - so it certifies "2 or stricter" and fails if the level falls back to
# the 3 that -Wall implies, or to 0. The natural way to write a type pun,
# *(int32_t *)&obj, is diagnosed from level 1 up including 3, so a control
# spelled that way would pass with the gate switched off. Do not simplify it.
#
# The second compile is what stops this gate going vacuous on its own. It
# repeats the control with -Wstrict-aliasing=3 appended, which displaces the
# level (a later explicit level beats an earlier one - that is the same
# mechanism as the EXTRA_CFLAGS hazard, used here as an instrument), and
# requires it to be ACCEPTED. If a future gcc starts diagnosing this shape at
# 3, the control stops separating the levels and this gate would keep passing
# while asserting nothing about the level in force. Then it says so instead.
#
# Two failure causes this library has that chron's copy does not, both
# reported separately because they want different fixes:
#
#   -fstrict-aliasing [disabled]   the OPTIMISATION is off, so nothing arms
#                                  the warning at any level. That is what
#                                  -O0 and -O1 do, and why ALIASING_CFLAGS
#                                  names the flag.
#   level absent, query exit 0     the flag string is malformed. Not "unset"
#                                  and not clang - an empty -Q result has
#                                  three causes and exit status separates
#                                  them.
#
# clang accepts -Wstrict-aliasing=1 and =2 and implements neither, and rejects
# =3 outright, so `make CC=clang` reaches this gate with the aliasing flags on
# every compile line and no aliasing coverage behind them. That is a true
# failure and the gate reports it, naming the compiler rather than the flags.
check-aliasing: $(LIBVER_GEN)
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' \
		'#include <stdint.h>' \
		'static double gcomp_alias_object;' \
		'int32_t gcomp_alias_control(void);' \
		'int32_t gcomp_alias_control(void) {' \
		'  int32_t * p = (int32_t *)&gcomp_alias_object;' \
		'  gcomp_alias_object = 1.0;' \
		'  return *p;' \
		'}' > $(BUILD_DIR)/alias_control.c
# qrc below is read on the same line the compiler runs on and must stay there.
# Any $(...) evaluated in between - including one building the message that
# reports the status - replaces $? with the subshell's, and the clang branch
# stops being selected. Adding a substitution to the lines above it looks like
# editing prose.
	@if $(CC) $(CFLAGS) $(INCLUDE) -fsyntax-only \
			$(BUILD_DIR)/alias_control.c 2> $(BUILD_DIR)/alias_control.log; then \
		qout=$$($(CC) -Q --help=warnings $(CFLAGS) 2>/dev/null); qrc=$$?; \
		oout=$$($(CC) -Q --help=optimizers $(CFLAGS) 2>/dev/null); \
		lvl=$$(printf '%s\n' "$$qout" \
			| awk '/-Wstrict-aliasing=<0,3>/ { print $$2 }'); \
		opt=$$(printf '%s\n' "$$oout" \
			| awk '$$1 == "-fstrict-aliasing" { print $$2 }'); \
		printf "\033[0;31mcheck-aliasing: %s accepted a planted type-punning violation, so this build has no aliasing coverage.\033[0m\n" "$$($(CC) --version 2>/dev/null | head -1)" >&2; \
		if [ "$$opt" = "[disabled]" ]; then \
			printf '%s\n' \
				'  -fstrict-aliasing is DISABLED, which silences this warning at every level - the' \
				'  level below is beside the point. gcc disables it below -O2, and this file names it' \
				'  explicitly only in the debug branch, where it was measured to change no code. So a' \
				'  release build forced to -O0 or -O1 reaches here, and the fix is NOT simply to name' \
				'  the flag: at -O1 naming it changes 30 of 76 objects. Measure that configuration' \
				'  before arming it, or build at the -O this library ships.' >&2; \
		elif [ -z "$$lvl" ] && [ "$$qrc" = "0" ]; then \
			printf '%s\n' \
				'  -Q --help=warnings succeeded and named no -Wstrict-aliasing level at all, which is' \
				'  neither compiler behaviour seen here. Do NOT read this as the clang case: check what' \
				'  CFLAGS was actually passed before concluding anything about the warning.' >&2; \
		elif [ -z "$$lvl" ]; then \
			printf '%s\n' \
				'  This compiler would not report an effective -Wstrict-aliasing level, which gcc gives' \
				'  through -Q --help=warnings. Expect clang: it accepts -fstrict-aliasing' \
				'  -Wstrict-aliasing=2 in silence and implements no such diagnostic, so the flags ride' \
				'  every compile line of a clang build while detecting nothing. compress aliasing' \
				'  coverage is gcc-only, and a clang run does not have it.' >&2; \
		elif [ "$$lvl" = "1" ] || [ "$$lvl" = "2" ]; then \
			printf '  The effective -Wstrict-aliasing level is %s, which is a level that DOES diagnose this control.\n' "$$lvl" >&2; \
			printf '%s\n' \
				'  So the flags are right and the compiler is not implementing them - that is clang,' \
				'  which accepts these flags in silence. compress aliasing coverage is gcc-only.' >&2; \
		else \
			printf '  The effective -Wstrict-aliasing level is %s, and this control is diagnosed only at 1 and 2.\n' "$$lvl" >&2; \
			printf '%s\n' \
				'  So the warning is at the WRONG LEVEL rather than missing, and ALIASING_CFLAGS is' \
				'  likely untouched. What displaces it is a later EXPLICIT level, since an explicit' \
				'  level beats the 3 that -Wall implies from either side. CFLAGS ends with' \
				'  EXTRA_CFLAGS, so EXTRA_CFLAGS=-Wstrict-aliasing=3 does exactly this. Note that 3 is' \
				'  also what -Wall implies on its own, so a level of 3 is equally what removing' \
				'  -Wstrict-aliasing=2 from ALIASING_CFLAGS looks like; 0 can only have been asked' \
				'  for.' >&2; \
		fi; \
		exit 1; \
	fi
	@if ! grep -q 'strict-aliasing' $(BUILD_DIR)/alias_control.log; then \
		printf "\033[0;31mcheck-aliasing: the control failed to compile, but not for aliasing - so this says nothing about whether the warning is armed:\033[0m\n" >&2; \
		cat $(BUILD_DIR)/alias_control.log >&2; \
		exit 1; \
	fi
	@if ! $(CC) $(CFLAGS) -Wstrict-aliasing=3 $(INCLUDE) -fsyntax-only \
			$(BUILD_DIR)/alias_control.c 2> $(BUILD_DIR)/alias_control3.log; then \
		if grep -q 'strict-aliasing' $(BUILD_DIR)/alias_control3.log; then \
			printf "\033[0;31mcheck-aliasing: the control is now diagnosed at -Wstrict-aliasing=3 as well, so it no longer certifies the level in force.\033[0m\n" >&2; \
			printf '%s\n' \
				'  It was chosen because level 3 accepts it: that is what makes its rejection evidence' \
				'  that the level is 2 or stricter rather than the 3 -Wall implies. Now it would be' \
				'  rejected either way, and this gate would pass with the level silently back at 3.' \
				'  Respell the control to a shape this compiler accepts at 3 and rejects at 2, and' \
				'  re-measure the grid on ALIASING_CFLAGS; do not simply delete this check.' >&2; \
		else \
			printf "\033[0;31mcheck-aliasing: the vacuity control failed to compile, but not for aliasing:\033[0m\n" >&2; \
			cat $(BUILD_DIR)/alias_control3.log >&2; \
		fi; \
		exit 1; \
	fi
	@printf "\033[0;32mA planted type-punning violation is refused by the library's own flags, and accepted at -Wstrict-aliasing=3 - so the level in force is doing work the default would not.\033[0m\n"

####################################################################
# GCOMP_TEST_BUILD gate
####################################################################

check-test-build: ## Fail if the GCOMP_TEST_BUILD arms do not compile
# -DGCOMP_TEST_BUILD appears only in ASAN_CFLAGS and TSAN_CFLAGS, so `make
# test` never preprocesses the arms it selects: four files carry an
# `#ifdef GCOMP_TEST_BUILD` block, and thirteen more expand
# GCOMP_INTERNAL_API differently under it. Code inside an arm the
# preprocessor discards is not compiled, so -Werror has nothing to say about
# it - the release build is not lenient about those lines, it cannot see them.
#
# That is not hypothetical. A sign-compare added to an assertion inside one of
# these arms compiled clean under `make test` and failed `make test-asan`
# immediately, which is a slow and confusing way to find a one-line mistake:
# the sanitizer tree has to build first, and the failure arrives labelled as a
# sanitizer run. This is the same defect class as an unbuilt preprocessor
# branch anywhere else, and the fix is to compile the branch.
#
# A real compile to /dev/null, not -fsyntax-only, and that distinction was
# measured after -fsyntax-only gave a wrong answer elsewhere in this library.
# Warnings that need the middle end are simply absent under -fsyntax-only:
#
#                        -fsyntax-only   -c -o /dev/null
#   -Wsign-compare             1               1
#   -Wstrict-aliasing          1               1
#   -Wunused-function          0               1
#   -Warray-bounds             0               1
#
# -Warray-bounds is not in the -Wno-error= list, so it IS an error under
# -Werror - which means a syntax-only sweep could pass while the sanitizer
# build failed on exactly the kind of defect this gate exists to catch early.
# The cost of closing that is 11s against 1.4s for all 76 TUs, on a `make test`
# that takes minutes.
#
# It is still not a substitute for building the sanitizer trees and does not try
# to be. It answers "does this compile", which is now true rather than
# approximately true.
#
# TWO CONTROLS, EACH A PAIR. Both plant a defect inside a GCOMP_TEST_BUILD arm
# and require it REJECTED with the macro and ACCEPTED without it. The rejection
# proves the sweep's flags can see that class; the acceptance proves the
# premise, that the release build really is blind to these lines, so this gate
# covers something `make test` does not.
#
#   A  a sign-compare      the defect that prompted this gate. Visible to
#                          -fsyntax-only as well, so it says nothing about how
#                          the sweep compiles.
#   B  an array-bounds     invisible to -fsyntax-only, measured. This is the
#                          arm that certifies the sweep is a REAL compile: if
#                          anyone trades the 11s back for 1.4s by returning to
#                          -fsyntax-only, B stops firing and the gate says so.
#
# If a pair's two halves both fail, the sweep is not reading the macro. If both
# pass, that warning has left -Wextra and the control wants respelling rather
# than deleting.
#
# Scored by exit status per TU, not by grepping for "warning". A broken
# include path produces ERRORS, so a text search for warnings finds nothing
# and a sweep scored that way reports a clean library.
check-test-build: $(LIBVER_GEN)
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' \
		'#include <stdint.h>' \
		'int32_t gcomp_test_build_control(void);' \
		'int32_t gcomp_test_build_control(void) {' \
		'#ifdef GCOMP_TEST_BUILD' \
		'  int32_t i = -1;' \
		'  uint32_t u = 1u;' \
		'  return (i < u) ? 1 : 0;' \
		'#else' \
		'  return 0;' \
		'#endif' \
		'}' > $(BUILD_DIR)/test_build_control.c
	@printf '%s\n' \
		'#include <stdint.h>' \
		'#include <string.h>' \
		'int32_t gcomp_test_build_codegen_control(void);' \
		'int32_t gcomp_test_build_codegen_control(void) {' \
		'#ifdef GCOMP_TEST_BUILD' \
		'  int32_t a[4];' \
		'  memset(a, 0, sizeof a);' \
		'  return a[7];' \
		'#else' \
		'  return 0;' \
		'#endif' \
		'}' > $(BUILD_DIR)/test_build_codegen_control.c
	@for pair in 'test_build_control sign-compare' 'test_build_codegen_control array-bounds'; do \
		set -- $$pair; ctl=$$1; want=$$2; \
		if $(CC) $(LIB_CFLAGS) -DGCOMP_TEST_BUILD $(INCLUDE) -c -o /dev/null \
				$(BUILD_DIR)/$$ctl.c 2> $(BUILD_DIR)/$$ctl.log; then \
			printf "\033[0;31mcheck-test-build: a planted %s inside a GCOMP_TEST_BUILD arm was accepted, so this sweep would not see the defect it exists for.\033[0m\n" "$$want" >&2; \
			if [ "$$want" = "array-bounds" ]; then \
				printf '%s\n' \
					'  This is the arm that certifies the sweep is a real compile. -fsyntax-only does not' \
					'  report -Warray-bounds at all, so it passing while the sign-compare arm still fails' \
					'  is what returning this sweep to -fsyntax-only looks like.' >&2; \
			fi; \
			exit 1; \
		fi; \
		if ! grep -q "$$want" $(BUILD_DIR)/$$ctl.log; then \
			printf "\033[0;31mcheck-test-build: the %s control was refused, but not for the planted %s - so this says nothing about what the sweep can see:\033[0m\n" "$$ctl" "$$want" >&2; \
			cat $(BUILD_DIR)/$$ctl.log >&2; \
			exit 1; \
		fi; \
		if ! $(CC) $(LIB_CFLAGS) $(INCLUDE) -c -o /dev/null \
				$(BUILD_DIR)/$$ctl.c 2> $(BUILD_DIR)/$$ctl-off.log; then \
			printf "\033[0;31mcheck-test-build: the %s control is refused WITHOUT -DGCOMP_TEST_BUILD too, so the release build was never blind to it and this gate's premise is wrong:\033[0m\n" "$$ctl" >&2; \
			cat $(BUILD_DIR)/$$ctl-off.log >&2; \
			exit 1; \
		fi; \
	done
	@broken=""; n=0; \
	for src in $(SOURCES); do \
		n=$$((n + 1)); \
		if ! $(CC) $(LIB_CFLAGS) -DGCOMP_TEST_BUILD $(INCLUDE) -c -o /dev/null \
				$$src > $(BUILD_DIR)/test_build_sweep.log 2>&1; then \
			broken="$$broken $$src"; \
			printf "\033[0;31m\n### %s ###\033[0m\n" "$$src" >&2; \
			cat $(BUILD_DIR)/test_build_sweep.log >&2; \
		fi; \
	done; \
	if [ -n "$$broken" ]; then \
		printf "\033[0;31m\n### GCOMP_TEST_BUILD arms that do not compile ###\033[0m\n" >&2; \
		printf "%s\n" "$$broken" >&2; \
		printf "\nThese compile in the sanitizer trees, which define\n" >&2; \
		printf "GCOMP_TEST_BUILD, and nowhere else - so \`make test\` passes and\n" >&2; \
		printf "\`make test-asan\` fails at the compile step.\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mAll %s library TUs compile with -DGCOMP_TEST_BUILD, which no build in \`make test\` defines - a real compile, so the codegen-stage warnings are included.\033[0m\n" "$$n"

####################################################################
# Flags-stamp gate
####################################################################

# The flag stamps only work on the rules that name one. Nothing in make
# requires it: an object rule added without its stamp compiles with whatever
# flags are in force and is then never rebuilt when they change, which looks
# exactly like a correct incremental build. Three rules in this file went
# unstamped that way, and a fourth defect was subtler - four stamps recorded
# the variables their recipes were DERIVED from rather than the ones the
# recipes expand, so editing the literal text in LIB_CFLAGS left the recorded
# string byte-identical and rebuilt nothing.
#
# This is chron's check-stamps, ported, with three changes noted where they
# occur. It reads the makefile TEXT rather than asking make for its rule
# database, because a rule inside an `ifneq` does not exist in the database at
# all and that is exactly how one of the unstamped rules hid.
#
# The population is "every recipe that compiles a prerequisite", spelled as the
# -c $< that all eleven carry. Two things are checked per rule: that some flags
# stamp is a prerequisite, and that it is the stamp for the tree the object is
# built into - a rule copied between trees keeps the old stamp and then misses
# exactly the flag changes it was meant to catch. The tree's name is the prefix
# of both $(<TREE>_OBJ_DIR) and $(<TREE>_FLAGS_STAMP), so the pairing is
# derived rather than tabulated and a fifth tree needs no edit here.
#
# CHANGE 1, against chron's copy: a variable is accepted if ANY stamp the rule
# names records it, not just the first. The test executables here compile and
# link in one command, so they legitimately depend on two stamps - the tree's
# flags stamp for $(CXXFLAGS) and $(TEST_INCLUDE), and its link stamp for
# $(TESTFLAGS). Consulting only one reports the other's variables as
# unrecorded, which is how this arm read before the change.
#
# CHANGE 2: a -fsyntax-only invocation is counted as a PROBE, not as an
# unmodelled build rule. Nothing it produces can go stale, and both gates in
# TEST_GATES compile a control, so without this the unmodelled pin grows by one
# every time a gate is added and stops measuring what it is for.
#
# CHANGE 3: the head test accepts bare `cc` and `c++` as well as gcc/g++/clang.
# A recipe spelled that way with no -c $< marker was skipped by both arms
# silently. Nothing in this file is spelled that way; the control now carries
# one, so the hole cannot come back unnoticed.
#
# What it does NOT check: that a stamp's recipe records the flags in the same
# ORDER, or that the flags it records are the right ones. A rule compiling with
# some variable no stamp mentions is caught; a stamp recording a variable that
# is never used is not.

# Names the sweep skips, because the rule already lists the file as a normal
# prerequisite and mtime is the real check there. Pinned so the list cannot
# grow into an excuse.
#
# The three *COMPRESSLIBRARY names are the honest case to state: each expands
# to link flags AROUND a file that IS a prerequisite - for the release tree
# -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive, for
# the sanitizer trees -L and -l naming the shared library. The file is covered;
# a change to the --whole-archive spelling itself would not relink, and that is
# a real if narrow gap rather than something this list makes safe.
STAMP_PREREQ_NAMES := LIBOBJECTS ASAN_LIBOBJECTS TSAN_LIBOBJECTS \
	TEST_HELPER_OBJ ASAN_TEST_HELPER_OBJ TSAN_TEST_HELPER_OBJ \
	COMPRESSLIBRARY ASAN_COMPRESSLIBRARY TSAN_COMPRESSLIBRARY

# Compiler invocations outside the stamp model, pinned so the set cannot grow
# in silence. The stamp invariant is about rules that compile $< incrementally,
# which is narrower than "every compile". The seven are:
#
#   3  shared-library links (release, ASan, TSan), which consume stamped
#      objects: a flag change moves the stamp, the objects rebuild and the
#      link follows from mtime.
#   3  compile-to-executable rules - two examples and the corpus generator -
#      each naming the static archive as a normal prerequisite, so the same
#      chain covers them.
#   1  the AFL harness link, in a tree with its own stamp on its objects.
#
# This is a pin, not a judgement. It fails when the number moves, so a new
# compile-to-executable rule in a tree whose binaries do NOT depend on a
# stamped object has to be looked at instead of passing.
STAMP_UNMODELLED_EXPECTED := 7

# How many names the skip list holds. Pinned because a name added to it
# silences the check for that variable, and "it was already in the list" is the
# easiest review to pass. It is deliberately NOT part of the control's expected
# string: a fact checked in two places takes the first failure and reports the
# wrong cause.
STAMP_PREREQ_EXPECTED := 9

# -fsyntax-only invocations at the head of a recipe: check-aliasing's control
# and its vacuity compile. Pinned separately from the number above so that
# adding a gate and weakening the build cannot hide behind one figure.
#
# It read 4 until check-test-build's sweep became a real compile to /dev/null
# rather than -fsyntax-only, which moved its control compiles into a shell loop.
# The pin caught that, which is what it is for: a change in how a gate compiles
# should require someone to look. The two it lost are not unchecked - they are
# outside this model for the shell-loop reason below.
#
# Both of these are spelled `@if [!] $(CC) ...`, and the sweep could not see the
# negated form until the head normalisation learned to strip a leading `!`. That
# mattered beyond the count: a LINK rule spelled that way was skipped by both
# arms, appearing in neither LINKED nor UNMODELLED, which is the shape of
# blindness that reads as a clean result. The control carries a negated probe so
# it cannot come back.
#
# NOT counted, and why that is a limit rather than a rule: this model does not
# look past the head of a recipe, so the compiler invocations inside
# check-test-build's two shell loops - its four control compiles and its 76-TU
# sweep - are invisible to it. They are gate probes that write to /dev/null and
# produce nothing that can go stale, so nothing is lost here; the limit is that
# a real build step could hide the same way. Measured rather than assumed: every
# compiler name this file mentions away from a recipe head is a gate's own
# probe, a printf writing check-stamps' planted control, or a stamp recipe
# recording its flag string. No build rule hides a compiler in a shell recipe.
#
# Deliberately NOT done: teaching the probe test to recognise `-o /dev/null` as
# well as -fsyntax-only. No recipe in this file is spelled that way at a head
# position, so the branch would be unexercised, and an untested branch in a
# sweep is worse than a limit that is written down.
STAMP_PROBES_EXPECTED := 2

# What the planted control must produce. Four compile recipes, one of them
# wrapped; one with no stamp; four variables no stamp records, two of them past
# a line break; three link rules; two -fsyntax-only probes, one spelled with a
# bare compiler name; and two compiler invocations with no stamp at all.
#
# PREREQ is deliberately absent from this string. It counts the skip list,
# which is a constant of the sweep rather than something the control exercises,
# and while it was in here adding a name to that list failed the CONTROL first,
# reporting that the sweep had stopped reading rules correctly - which was not
# what had happened. The pin written for exactly that change could never fire.
# A second check on the same fact is not redundancy: it takes the first failure
# and reports the wrong cause.
STAMP_CONTROL_EXPECTED := TOTAL 4 BAD 1 UNMODELLED 2 UNRECORDED 4 LINKED 3 PROBES 3

STAMP_CHECK_MAKEFILE := $(firstword $(MAKEFILE_LIST))
STAMP_CHECK_AWK := $(BUILD_DIR)/stamp_check.awk

define stamp-check-awk
{ L[NR] = $$0 }
function vars(s, out,   v) {
  while (match(s, /\$$\([A-Za-z0-9_]+\)/)) {
    v = substr(s, RSTART + 2, RLENGTH - 3)
    out[v] = 1
    s = substr(s, RSTART + RLENGTH)
  }
}
function stamps(s, out,   t) {
  while (match(s, /\$$\([A-Z_]*FLAGS_STAMP\)/)) {
    t = substr(s, RSTART + 2, RLENGTH - 3)
    out[t] = 1
    s = substr(s, RSTART + RLENGTH)
  }
}
function anyrecords(v, sset,   t) {
  for (t in sset) if ((t "|" v) in SV) return 1
  return 0
}
function namelist(sset,   t, out) {
  out = ""
  for (t in sset) out = (out == "" ? t : out ", " t)
  return out
}
BEGIN {
  PREREQ_N = split(PREREQ_NAMES, pa, " ")
  for (x = 1; x <= PREREQ_N; x++) PREREQ[pa[x]] = 1
}
END {
  total = 0; bad = 0; unmodelled = 0; unrecorded = 0; linked = 0; probes = 0
  for (i = 1; i <= NR; i++) {
    if (L[i] !~ /^\$$\([A-Z_]*FLAGS_STAMP\):/) continue
    name = L[i]; sub(/^\$$\(/, "", name); sub(/\).*/, "", name)
    for (j = i + 1; j <= NR && j < i + 8; j++) {
      if (L[j] !~ /printf/) continue
      pf = L[j]; pe = j
      while (pe < NR && L[pe] ~ /\\[ \t]*$$/) { pe++; pf = pf " " L[pe] }
      delete tmp; vars(pf, tmp)
      for (v in tmp) if (v != "") SV[name "|" v] = 1
      break
    }
  }
  cur = ""; curline = 0; skipto = 0
  for (i = 1; i <= NR; i++) {
    if (i <= skipto) continue
    if (L[i] !~ /^\t/) {
      if (L[i] ~ /:/ && L[i] !~ /:=/ && L[i] !~ /^\043/ && L[i] !~ /^[ ]/) {
        cur = L[i]; curline = i; m = i
        while (m < NR && L[m] ~ /\\[ \t]*$$/) { m++; cur = cur " " L[m] }
        skipto = m
      }
      continue
    }
    rec = L[i]; e = i
    while (e < NR && L[e] ~ /\\[ \t]*$$/) { e++; rec = rec " " L[e] }
    skipto = e
    if (rec !~ /-c \$$</) {
      if (rec ~ /^\t[ \t]*\043/) continue
      if (rec ~ /-c \$$\$$</) continue
      head = rec
      sub(/^\t[ \t]*/, "", head)
      sub(/^[-@]+[ \t]*/, "", head)
      sub(/^if[ \t]+/, "", head)
      sub(/^![ \t]*/, "", head)
      sub(/^[-@]+[ \t]*/, "", head)
      if (head !~ /^\$$\$$?\([A-Z_]*(CC|CXX)\)[ \t]/ &&
          head !~ /^(cc|c\+\+|gcc|g\+\+|clang|clang\+\+)[ \t]/) continue
      if (rec ~ /-fsyntax-only/) { probes++; continue }
      hdr = cur; j = curline
      if (hdr !~ /LINK_FLAGS_STAMP/) { unmodelled++; continue }
      linked++
      delete sset; stamps(hdr, sset)
      delete rv; vars(rec, rv)
      for (v in rv) {
        if (v == "" || v in PREREQ) continue
        if (!anyrecords(v, sset)) {
          unrecorded++
          printf "  %s:%d: link recipe expands $$(%s), recorded by none of the stamps it names (%s)\n", FILENAME, i, v, namelist(sset)
        }
      }
      continue
    }
    total++
    hdr = cur; j = curline
    if (hdr !~ /FLAGS_STAMP/) {
      bad++
      printf "  %s:%d: compiles with no flags stamp: %s\n", FILENAME, j, hdr
      continue
    }
    tgt = hdr; sub(/:.*/, "", tgt)
    if (tgt ~ /OBJ_DIR/) {
      tree = tgt; sub(/.*\$$\(/, "", tree); sub(/OBJ_DIR.*/, "", tree)
      want = "$$(" tree "FLAGS_STAMP)"
      if (index(hdr, want) == 0) {
        bad++
        printf "  %s:%d: stamped for another tree, wants %s: %s\n", FILENAME, j, want, hdr
        continue
      }
    }
    delete sset; stamps(hdr, sset)
    delete rv; vars(rec, rv)
    for (v in rv) {
      if (v == "") continue
      if (v in PREREQ) continue
      if (!anyrecords(v, sset)) {
        unrecorded++
        printf "  %s:%d: recipe expands $$(%s), recorded by none of the stamps it names (%s)\n", FILENAME, i, v, namelist(sset)
      }
    }
  }
  printf "TOTAL %d BAD %d UNMODELLED %d UNRECORDED %d LINKED %d PROBES %d PREREQ %d\n", total, bad, unmodelled, unrecorded, linked, probes, PREREQ_N
}
endef

# $(file ...) is expanded when make expands this recipe, which happens before
# ANY line of it runs - so an @mkdir on the line above cannot have created the
# directory yet, and the write fails with "No such file or directory". It never
# showed in a warm tree, because one earlier build makes $(BUILD_DIR) and it
# then persists for the life of the checkout. Found by running this gate as the
# FIRST command in a fresh clone, which is the only state that can see it.
#
# So the awk goes in its own rule, whose recipe make expands only when it
# decides to run it - after the order-only directory prerequisite exists. The
# makefile is a normal prerequisite because the program lives inside it: without
# that the file would be written once and never refreshed, so an edit to the awk
# would leave the gate running the previous version.
$(STAMP_CHECK_AWK): $(STAMP_CHECK_MAKEFILE) | $(BUILD_DIR)
	$(file >$@,$(stamp-check-awk))

$(BUILD_DIR):
	@mkdir -p $@

check-stamps: ## Fail if a compile or link rule has no flags stamp, or the wrong one
check-stamps: $(STAMP_CHECK_AWK)
# The control comes first, and is a planted set rather than a single bad rule:
# stamped and unstamped, recorded and unrecorded, wrapped and unwrapped, one
# probe and one link spelled with a bare compiler name. A sweep that has
# stopped matching recipes reports nothing wrong, which is indistinguishable
# from a clean makefile - so require it to find the planted ones and only the
# planted ones.
	@printf '%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n' \
		'$$(FLAGS_STAMP): force-flags' \
		"@printf '%s' '\$$(CFLAGS) \$$(INCLUDE)' > \$$@.new" \
		'$$(OBJ_DIR)/%.o: src/%.c $$(FLAGS_STAMP)' \
		'cc $$(CFLAGS) $$(INCLUDE) -c $$< -o $$@' \
		'$$(OBJ_DIR)/planted_nostamp.o: src/planted.c' \
		'cc $$(CFLAGS) $$(INCLUDE) -c $$< -o $$@' \
		'$$(OBJ_DIR)/planted_unrecorded.o: src/planted2.c $$(FLAGS_STAMP)' \
		'cc $$(CFLAGS) $$(PLANTED_UNRECORDED) $$(INCLUDE) -c $$< -o $$@' \
		'$$(LINK_FLAGS_STAMP): force-flags' \
		"@printf '%s' '\$$(LDFLAGS)' > \$$@.new" \
		'$$(APP_DIR)/planted_link_ok: planted.o $$(LINK_FLAGS_STAMP)' \
		'g++ $$(LDFLAGS) -o $$@ planted.o' \
		'$$(APP_DIR)/planted_link_nostamp: planted.o' \
		'g++ $$(LDFLAGS) -o $$@ planted.o' \
		'$$(APP_DIR)/planted_link_unrec: planted.o $$(LINK_FLAGS_STAMP)' \
		'g++ $$(LDFLAGS) $$(PLANTED_LINK_UNRECORDED) -o $$@ planted.o' \
		> $(BUILD_DIR)/stamp_control.mk
# Two of the planted rules wrap, because every read of a recipe used to take
# one physical line and a wrapped recipe hid everything past the backslash. A
# correct makefile prints the same fingerprint either way, so without a wrapped
# rule in the control the fix is not demonstrable and the bug returns the next
# time this is edited.
	@printf '%s\n\t%s \\\n\t%s\n%s\n\t%s \\\n\t%s\n' \
		'$$(APP_DIR)/planted_link_wrap: planted.o $$(LINK_FLAGS_STAMP)' \
		'g++ $$(LDFLAGS) -o $$@ planted.o' \
		'  $$(PLANTED_WRAP_UNRECORDED)' \
		'$$(OBJ_DIR)/planted_wrapc.o: src/planted3.c $$(FLAGS_STAMP)' \
		'cc $$(CFLAGS) $$(PLANTED_WRAPC_UNRECORDED) $$(INCLUDE)' \
		'  -c $$< -o $$@' \
		>> $(BUILD_DIR)/stamp_control.mk
# The probe and bare-name arms. Two spellings of a -fsyntax-only compile, so
# the PROBES counter cannot be permanently zero, and one link spelled `c++`,
# which both arms skipped in silence before the head test was widened.
	@printf '%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n%s\n\t%s\n' \
		'planted-probe:' \
		'$$(CC) $$(CFLAGS) $$(PLANTED_PROBE_UNRECORDED) -fsyntax-only probe.c' \
		'planted-probe-bare:' \
		'cc $$(CFLAGS) $$(PLANTED_PROBE2_UNRECORDED) -fsyntax-only probe2.c' \
		'planted-link-bare: planted.o' \
		'c++ $$(LDFLAGS) -o $$@ planted.o' \
		'planted-probe-negated:' \
		'@if ! $$(CC) $$(CFLAGS) -fsyntax-only probe3.c; then exit 1; fi' \
		>> $(BUILD_DIR)/stamp_control.mk
	@ctl=$$(awk -v PREREQ_NAMES='$(STAMP_PREREQ_NAMES)' \
			-f $(STAMP_CHECK_AWK) \
			$(BUILD_DIR)/stamp_control.mk | tail -1 \
			| sed 's/ PREREQ [0-9]*$$//'); \
	if [ "$$ctl" != "$(STAMP_CONTROL_EXPECTED)" ]; then \
		printf "\033[0;31mcheck-stamps: the control says '%s', not '%s', so a clean result from this sweep means nothing.\033[0m\n" "$$ctl" "$(STAMP_CONTROL_EXPECTED)" >&2; \
		printf '%s\n' \
			'  TWO causes produce this, they need opposite fixes, and the fingerprint cannot tell' \
			'  them apart: the sweep has stopped reading rules the way it thinks, OR the planted' \
			'  control no longer contains the shape that figure counts. Removing the `!` strip from' \
			'  the awk and removing the negated arm from the control both print PROBES one short.' \
			'  The control it actually wrote is left at:' >&2; \
		printf '    %s\n' '$(BUILD_DIR)/stamp_control.mk' >&2; \
		printf '%s\n' \
			'  Read it first. If the shape is there, the sweep is what changed.' >&2; \
		exit 1; \
	fi
# Two counts of the same population, and they are NOT independent - both key on
# the same -c $< marker, so a marker that stopped matching would move them
# together. Kept because it catches the sweep failing for any OTHER reason,
# and recorded as a limit rather than presented as corroboration. Comment lines
# are dropped first, because the prose above names the marker and counted
# itself as an extra compile recipe the first time this ran.
	@want=$$(grep -v '^#' $(STAMP_CHECK_MAKEFILE) \
		| grep -cF -- '-c $$<'); \
	out=$$(awk -v PREREQ_NAMES='$(STAMP_PREREQ_NAMES)' \
		-f $(STAMP_CHECK_AWK) $(STAMP_CHECK_MAKEFILE)); \
	got=$$(printf '%s\n' "$$out" | sed -n 's/^TOTAL \([0-9]*\) .*/\1/p'); \
	bad=$$(printf '%s\n' "$$out" | sed -n 's/^TOTAL [0-9]* BAD \([0-9]*\) .*/\1/p'); \
	unmodelled=$$(printf '%s\n' "$$out" | sed -n 's/.* UNMODELLED \([0-9]*\) .*/\1/p'); \
	unrecorded=$$(printf '%s\n' "$$out" | sed -n 's/.* UNRECORDED \([0-9]*\) .*/\1/p'); \
	linked=$$(printf '%s\n' "$$out" | sed -n 's/.* LINKED \([0-9]*\) .*/\1/p'); \
	probes=$$(printf '%s\n' "$$out" | sed -n 's/.* PROBES \([0-9]*\) .*/\1/p'); \
	prereq=$$(printf '%s\n' "$$out" | sed -n 's/.* PREREQ \([0-9]*\)$$/\1/p'); \
	if [ "$$got" != "$$want" ]; then \
		printf "\033[0;31mcheck-stamps: the sweep saw %s compile recipes and grep found %s. One of them is wrong, so neither count can be trusted.\033[0m\n" "$$got" "$$want" >&2; \
		exit 1; \
	fi; \
	if [ "$$unmodelled" != "$(STAMP_UNMODELLED_EXPECTED)" ]; then \
		printf "\033[0;31mcheck-stamps: %s compiler invocations are outside what this gate models, not the %s it is pinned to. A rule that compiles or links straight to an executable goes stale on its own unless it depends on a stamped object; this gate does not check that, so the change needs a look.\033[0m\n" \
			"$$unmodelled" "$(STAMP_UNMODELLED_EXPECTED)" >&2; \
		exit 1; \
	fi; \
	if [ "$$probes" != "$(STAMP_PROBES_EXPECTED)" ]; then \
		printf "\033[0;31mcheck-stamps: %s -fsyntax-only invocations, not the %s pinned. Those are excluded from the model because nothing they produce can go stale; a build step that really produces an artifact must not be spelled that way.\033[0m\n" \
			"$$probes" "$(STAMP_PROBES_EXPECTED)" >&2; \
		exit 1; \
	fi; \
	if [ "$$prereq" != "$(STAMP_PREREQ_EXPECTED)" ]; then \
		printf "\033[0;31mcheck-stamps: the sweep skips %s variable names, not the %s it is pinned to. Those names are skipped because the rule already lists the file as a prerequisite, so mtime covers it; a name added for any other reason silences this check for that variable.\033[0m\n" \
			"$$prereq" "$(STAMP_PREREQ_EXPECTED)" >&2; \
		exit 1; \
	fi; \
	if [ "$$unrecorded" != "0" ]; then \
		printf "\033[0;31m\n### %s recipes expand a variable no stamp they name records ###\033[0m\n" "$$unrecorded" >&2; \
		printf '%s\n' "$$out" | grep 'recorded by none of' >&2; \
		printf "\nNaming a stamp is not enough: a stamp only moves when the\n" >&2; \
		printf "variables inside its own printf change. A flag living only in a\n" >&2; \
		printf "variable every stamp omits rebuilds nothing at all.\n" >&2; \
		exit 1; \
	fi; \
	if [ "$$bad" != "0" ]; then \
		printf "\033[0;31m\n### %s compile rules carry the wrong flags stamp, or none ###\033[0m\n" "$$bad" >&2; \
		printf '%s\n' "$$out" | grep -v '^TOTAL ' >&2; \
		printf "\nAn object built without its tree's stamp as a prerequisite is\n" >&2; \
		printf "never rebuilt when the flags change, and the stale object links\n" >&2; \
		printf "into everything downstream of it.\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mAll %s compile rules carry the flags stamp for their own tree and %s link rules carry a link stamp, every variable either recorded or a file prerequisite; %s invocations outside the model and %s syntax-only probes, as pinned.\033[0m\n" "$$got" "$$linked" "$$unmodelled" "$$probes"

####################################################################
# Sanitizer-report gate
####################################################################

# How many recipes consult $(SAN_REPORT_RE): the ASan and TSan quiet runners.
# Pinned because the check lives inside a shell branch, and simplifying that
# branch back to "exit status decides" is a one-line edit that no test would
# notice - the runners would go green on a reporting binary again.
SAN_SCAN_EXPECTED := 2

check-san-report: ## Fail if the quiet runners could not recognise a sanitizer report
# The quiet runners capture each binary's output and print it only when the run
# is judged a failure, so whatever the judgement misses is deleted rather than
# shown. That is not hypothetical here: a planted signed overflow once ran
# under test-asan-quiet, printed its diagnostic into the captured string, exited
# 0, and produced a green PASS row with the diagnostic discarded - the report
# existed and nothing kept it.
#
# Exit status alone cannot carry that judgement. -fno-sanitize-recover covers
# the checks it names and nothing else, a report from a child process does not
# set the parent's status, and the runtimes take their options from the
# environment, where a caller can turn halting off. So the runners also scan the
# output they already hold, which costs one grep per suite.
#
# That makes $(SAN_REPORT_RE) load-bearing, and a regex that matches nothing is
# indistinguishable from a clean run - the exact failure this gate exists to
# prevent, one level up. So: five report shapes must each be recognised, and two
# gtest summary lines must not be. The negatives are the two that come closest
# to the positives, since those are what a careless widening would catch.
#
# Counting matches rather than testing for non-empty output: a regex that
# matched every line would pass a "does it find anything" check.
check-san-report:
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' \
		'src/foo.c:12:5: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type int' \
		'==1234==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010' \
		'WARNING: ThreadSanitizer: data race (pid=999)' \
		'==999==ERROR: LeakSanitizer: detected memory leaks' \
		'SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior src/foo.c:12:5 in' \
		> $(BUILD_DIR)/san_report_control.txt
	@printf '%s\n' \
		'[  PASSED  ] 21 tests.' \
		'[==========] 21 tests from 3 test suites ran. (4 ms total)' \
		'[ RUN      ] TestDeflate.RoundTripsAnEmptyInput' \
		'Note: Google Test filter = *Zstd*' \
		> $(BUILD_DIR)/san_clean_control.txt
	@hits=$$(grep -cE '$(SAN_REPORT_RE)' $(BUILD_DIR)/san_report_control.txt || true); \
	want=$$(wc -l < $(BUILD_DIR)/san_report_control.txt); \
	if [ "$$hits" != "$$want" ]; then \
		printf "\033[0;31mcheck-san-report: %s of %s sanitizer report shapes are recognised, so a quiet runner would delete the ones that are not.\033[0m\n" "$$hits" "$$want" >&2; \
		printf '%s\n' 'Unmatched:' >&2; \
		grep -vE '$(SAN_REPORT_RE)' $(BUILD_DIR)/san_report_control.txt >&2; \
		exit 1; \
	fi
	@false=$$(grep -cE '$(SAN_REPORT_RE)' $(BUILD_DIR)/san_clean_control.txt || true); \
	if [ "$$false" != "0" ]; then \
		printf "\033[0;31mcheck-san-report: %s lines of ordinary gtest output match the report pattern, so every passing suite would be reported as failing.\033[0m\n" "$$false" >&2; \
		grep -E '$(SAN_REPORT_RE)' $(BUILD_DIR)/san_clean_control.txt >&2; \
		exit 1; \
	fi
	@scans=$$(grep -c "^[[:space:]]*reports=.*grep -cE '.(SAN_REPORT_RE)'" $(STAMP_CHECK_MAKEFILE) || true); \
	if [ "$$scans" != "$(SAN_SCAN_EXPECTED)" ]; then \
		printf "\033[0;31mcheck-san-report: %s recipes scan their captured output for a sanitizer report, not the %s pinned. A quiet runner that judges on exit status alone discards a report from a binary that still exits 0.\033[0m\n" \
			"$$scans" "$(SAN_SCAN_EXPECTED)" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mAll %s sanitizer report shapes are recognised, no ordinary gtest line is, and both quiet runners scan for them.\033[0m\n" "$$(wc -l < $(BUILD_DIR)/san_report_control.txt)"

# ---------------------------------------------------------------------------
# Oracles, against pinned references
# ---------------------------------------------------------------------------
#
# The suite's oracles run against whatever the machine has, which is a fact
# about the machine rather than about this library: `zstd` 1.5.7 and `liblz4`
# 1.10.0 here, an unrecorded pyzstd from pip, and nothing anywhere saying so.
# These targets run the same binaries against the versions
# tools/oracle/containers/IMAGES names.
#
# Not in TEST_GATES, and deliberately, which is what every library that landed
# this pattern also decided: `make test` must not need a container engine. What
# this buys instead is a gate that can say which reference answered.

ORACLE := tools/oracle

# The binaries that consult a reference. Spelled out rather than globbed,
# because the property that makes one of these an oracle - it compares against
# something this project did not write - is not in its file name: four of these
# have no "oracle" in their path and one file called test_zlib_dictionary.cpp
# is one.
#
# The list being written by hand is exactly why check-oracle-coverage exists.
# The first version of it was short by four suites - testDeflate_oracle,
# testLzw_spec_oracle, testRle_spec_oracle and testZlib_dictionary - because it
# was built by reading the files that contain a GTEST_SKIP, and those four
# carry an availability sentinel without one. A hand-written list of the things
# that must be checked is a thing that must itself be checked.
ORACLE_TEST_NAMES := testZstd_oracle testZstd_walk testZstd_dict_format \
	testGzip_oracle testZlib_oracle testZlib_dictionary testLz4_spec_oracle \
	testLz4_walk testDeflate_oracle testLzw_spec_oracle testRle_spec_oracle \
	testBrotli testBrotli_encoder testBrotli_robustness \
	testOracle testSeekable testGolden_provenance
ORACLE_TESTS := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(ORACLE_TEST_NAMES)))

.PHONY: oracle-build oracle-build-next oracle-version check-oracle check-oracle-next
.PHONY: check-oracle-coverage oracle-help

oracle-build: ## Build the pinned oracle image from its Containerfile
	@printf "\n### Building the oracle reference image ###\n"
	podman build -t ghoti-compress-oracle-refs:deb13-3 \
		-f $(ORACLE)/containers/refs/Containerfile \
		$(ORACLE)/containers/refs

oracle-build-next: ## Build the second zstd, the one ahead of the release
oracle-build-next: oracle-build
	@printf "\n### Building the zstd-next oracle image ###\n"
	podman build -t ghoti-compress-oracle-zstd-next:dev-01b7154 \
		-f $(ORACLE)/containers/zstd-next/Containerfile \
		$(ORACLE)/containers/zstd-next

oracle-version: ## Print which references would answer, and fail if any would not
oracle-version:
	@python3 $(ORACLE)/oracle_env.py

check-oracle: ## Run every oracle test against the pinned references; a skip is a failure
check-oracle: $(ORACLE_TESTS)
	@printf "\n### Oracle tests, against pinned references ###\n"
	@python3 $(ORACLE)/oracle_run.py $(ORACLE_TESTS)

check-oracle-next: ## Run the oracle suites against the zstd ahead of the release
check-oracle-next: $(ORACLE_TESTS)
	@printf "\n### Oracle tests, against the next zstd ###\n"
	@printf "### A disagreement here is a finding to triage, not a failure to fix: ###\n"
	@printf "### this reference is unreleased, so it has not decided anything yet.  ###\n"
	@GHOTI_ORACLE_ALIAS=zstd=zstd-next python3 $(ORACLE)/oracle_run.py $(ORACLE_TESTS)

# The name every availability sentinel ends with. A variable so that the gate's
# null case is reachable: `make check-oracle-coverage ORACLE_SENTINEL=NoSuchThing`
# must fail with "measured nothing" rather than pass, which is the difference
# between a gate that found no omissions and one that found no sentinels.
ORACLE_SENTINEL ?= IsActuallyAvailable

check-oracle-coverage: ## Fail if a suite carries an availability sentinel but is not in ORACLE_TEST_NAMES
check-oracle-coverage: $(TEST_EXECUTABLES)
	@printf "\n### Every suite with a reference is in the oracle gate ###\n"
	@missing=""; found=0; \
	for exe in $(TEST_EXECUTABLES); do \
		n=$$("$$exe" --gtest_list_tests 2>/dev/null | grep -c '$(ORACLE_SENTINEL)' || true); \
		if [ "$$n" -gt 0 ]; then \
			found=$$((found + n)); \
			case " $(ORACLE_TEST_NAMES) " in \
				*" $$(basename $$exe) "*) ;; \
				*) missing="$$missing $$(basename $$exe)";; \
			esac; \
		fi; \
	done; \
	if [ "$$found" -eq 0 ]; then \
		printf "### no sentinel was found in any binary, so this gate measured nothing ###\n" >&2; \
		exit 1; \
	fi; \
	if [ -n "$$missing" ]; then \
		printf "### these carry an availability sentinel and are not in ORACLE_TEST_NAMES:%s ###\n" "$$missing" >&2; \
		printf "### so \`make check-oracle\` does not run them, and their reference is pinned by nothing ###\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32m%s sentinels across %s suites, every one of them in the oracle gate.\033[0m\n" \
		"$$found" "$(words $(ORACLE_TEST_NAMES))"

oracle-help: ## Explain the oracle targets and the pins
	@printf "\nOracles run against pinned references in a container image.\n\n"
	@printf "  make oracle-build     build the image (needed once, and when a pin moves)\n"
	@printf "  make oracle-version   print every reference and its version\n"
	@printf "  make check-oracle     run the %s oracle suites in the image\n" "$(words $(ORACLE_TEST_NAMES))"
	@printf "\n  make oracle-build-next  build the second zstd, ahead of the release\n"
	@printf "  make check-oracle-next  the same suites against it; a disagreement there\n"
	@printf "                          is a finding to triage, not a failure to fix\n"
	@printf "\nThe pins are in %s/containers/IMAGES.\n" "$(ORACLE)"
	@printf "GHOTI_ORACLE_MODE=host uses this machine's own tools instead, and\n"
	@printf "still checks them against those pins - which is how a drifting\n"
	@printf "reference is found rather than silently used.\n\n"


test: ## Make and run the Unit tests
test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
# The loop used to end with the test run itself, so the recipe exited with the
# status of the LAST binary and every failure before it printed and was
# discarded. `make test` reported success with a failing suite, which is the
# one thing this target exists to do. It was found by deliberately breaking a
# test to check that a new one could fail: the suite exited 0.
#
# Failures are collected rather than stopping at the first, so one run names
# every suite that failed. Spelled as text's is, which was fixed first.
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		if ! LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Failing suites:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi

test-quiet: ## Run tests with minimal output (one line per test suite)
test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;36m%-30s %8s %10s %s\033[0m\n" "Test Suite" "Tests" "Time" "Status"; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			[ "$$failures" -eq 0 ] && failures=1; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi

test-valgrind: ## Run all tests under valgrind (Linux only)
test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
# Collected and reported at the end, for the same reason `test` does it: a
# loop whose last command is the run exits with the status of the last binary
# only, and every failure before it is discarded.
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests under Valgrind ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		if ! LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(VALGRIND_TEST_ENV) valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Failing suites:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-valgrind-quiet: ## Run tests under valgrind with minimal output (Linux only)
test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;35m%-30s %8s %10s %s\033[0m\n" "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(VALGRIND_TEST_ENV) valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -cE "(definitely|indirectly|possibly) lost: [1-9]" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=0; \
			if [ $$has_leak -gt 0 ]; then \
				status_msg="LEAK"; \
			else \
				status_msg="FAIL"; \
			fi; \
			total_failed=$$((total_failed + 1)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31m%s\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms" "$$status_msg"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d suites)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Valgrind is only available on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

####################################################################
# Sanitizer Builds (ASan, UBSan)
####################################################################

# Sanitizer flags
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -g
UBSAN_FLAGS := -fsanitize=undefined -fno-omit-frame-pointer -g
# Combined sanitizer flags (ASan + UBSan work well together).
#
# UBSan recovers by default: without -fno-sanitize-recover it prints the
# diagnostic, continues, and the process still exits 0 - so every UBSan finding
# this target has ever made was reported and then passed.  The checks are named
# in one variable because -fsanitize= and -fno-sanitize-recover= have to agree;
# spelling them twice is how they drifted apart.  float-cast-overflow is in
# clang's `undefined` group but not gcc's, and converting a float that does not
# fit the destination integer is undefined behaviour, so it is named here.
#
# Deliberately absent: float-divide-by-zero, which IEEE 754 defines and which
# fires on correct code that records an infinity.  bounds-strict and
# pointer-overflow are already inside gcc 14's `undefined` and add nothing.
UBSAN_CHECKS := undefined,float-cast-overflow
ASAN_UBSAN_FLAGS := $(ASAN_FLAGS) -fsanitize=$(UBSAN_CHECKS) \
                    -fno-sanitize-recover=$(UBSAN_CHECKS)

# Sanitizer-specific build directories
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_LINK_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.linkflags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps

# ASan-instrumented object files
ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.so
ASAN_STATIC_TARGET := $(BASE_NAME_PREFIX)-asan.a

# ASan test helper and executables
ASAN_TEST_HELPER_OBJ := $(ASAN_OBJ_DIR)/tests/common/test_helpers.o
ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# Compile flags for ASan builds (include UBSan for comprehensive checking)
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGCOMP_BUILD -DGCOMP_TEST_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_COMPRESSLIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

# The instrumented library is loaded through the executable's NEEDED list, and
# the ASan runtime insists on being initialised before anything it has to
# intercept.  Any LD_PRELOAD inherited from the environment loads ahead of it
# and the runtime then refuses to start at all - "ASan runtime does not come
# first in initial library list" - which aborts every test before a single one
# runs.  Desktop sessions set LD_PRELOAD for unrelated reasons, so this is not
# a hypothetical.  Naming the runtime here both overrides whatever was
# inherited and puts it first, which is the remedy the runtime itself names.
ASAN_RUNTIME := $(shell $(CC) -print-file-name=libasan.so)

# One environment for both ASan runners, the way TSAN_RUN_ENV already is. It
# was written out twice and the two copies drifted: the quiet runner carried
# neither halt_on_error, so it kept going after the first report while the loud
# one stopped. Two spellings of one setting is the defect, not the setting.
ASAN_RUN_ENV := LD_LIBRARY_PATH="$(ASAN_APP_DIR)" LD_PRELOAD="$(ASAN_RUNTIME)" \
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

# What a sanitizer report looks like, for the quiet runners, which capture a
# binary's output and print it only when the run is judged a failure. Judging
# that on exit status alone throws the report away whenever the process still
# exits 0 - and several things make it do so: a check outside
# -fno-sanitize-recover's set, a report from a child process, a runtime whose
# options were overridden from the environment. The valgrind quiet runner
# already scans its output for leak lines rather than trusting its exit code;
# this is the same check for the sanitizers, and check-san-report is what keeps
# it able to see.
#
# Calibrated against 3,343 lines of real output from 336 suite runs: zero
# matches. The two gtest summary lines that look closest - the [ PASSED ] line
# and the "N tests from M test suites ran" line - are in the control as
# negatives.
SAN_REPORT_RE := (ERROR|WARNING): (AddressSanitizer|LeakSanitizer|ThreadSanitizer|MemorySanitizer)|SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer)|runtime error:

# Add PIC on Linux
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

# Pattern rule for ASan-instrumented C object files
# Every ASan compile below writes a .d file and every one of them is included
# at the bottom of this block.  Without that the sanitizer build tracks source
# timestamps only: edit a header and the objects that include it are not
# rebuilt, so the run links objects compiled against different versions of the
# same struct.  That happened -- a field added to zstd_match_finder_t left one
# object sizing it at 56 bytes and another at 64, and ASan reported a
# heap-buffer-overflow in code that was correct.  A sanitizer build that can be
# assembled from mismatched objects is worse than no sanitizer build: it can
# invent a failure, and it can just as easily hide a real one.
$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling (ASan+UBSan instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# ASan-instrumented static library
$(ASAN_APP_DIR)/$(ASAN_STATIC_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Archiving ASan+UBSan-instrumented Static Library ###\n"
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

# ASan-instrumented shared library
$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Compiling ASan+UBSan-instrumented Shared Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -shared -o $@ $^ $(ASAN_LDFLAGS)

# ASan test helper object
$(ASAN_OBJ_DIR)/tests/common/test_helpers.o: tests/common/test_helpers.cpp $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Pattern rule for ASan test object files
$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(ASAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling ASan+UBSan Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Header dependencies for the ASan build; see the comment above the first
# ASan compile rule for why these matter.
ASAN_TEST_OBJECTS := $(patsubst tests/%.cpp,$(ASAN_OBJ_DIR)/tests/%.o,$(TEST_SOURCES))
ASAN_DEPFILES := $(ASAN_LIBOBJECTS:.o=.d) $(ASAN_TEST_HELPER_OBJ:.o=.d) \
	$(ASAN_TEST_OBJECTS:.o=.d)
-include $(ASAN_DEPFILES)

# Pattern rule for ASan test executables. Args: $1 = source path, $2 = executable name (from TEST_PAIRS).
define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(patsubst tests/%.cpp,%.o,$1)

# Normal prerequisite, for the same reason as the release rule above.
$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): \
		$$(ASAN_TEST_OBJ_$1) \
		$(ASAN_TEST_HELPER_OBJ) \
		$(ASAN_APP_DIR)/$(ASAN_TARGET) \
		$(ASAN_LINK_FLAGS_STAMP)
	@printf "\n### Linking ASan+UBSan %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $$(ASAN_TEST_HELPER_OBJ) $$(ASAN_LDFLAGS) $$(TESTFLAGS) $$(ASAN_COMPRESSLIBRARY)
endef

# Generate ASan build rules from TEST_PAIRS
$(foreach pair,$(TEST_PAIRS),$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

test-asan: ## Run all tests with AddressSanitizer + UndefinedBehaviorSanitizer (Linux only)
test-asan: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ASan + UBSan    ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests (ASan+UBSan) ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		$(ASAN_RUN_ENV) $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### All tests passed with ASan + UBSan ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-ubsan: ## Alias for test-asan (ASan+UBSan are run together)
test-ubsan: test-asan

test-asan-quiet: ## Run ASan+UBSan tests with minimal output (Linux only)
test-asan-quiet: $(ASAN_APP_DIR)/$(ASAN_TARGET) $(ASAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;33m%-30s %8s %10s %s\033[0m\n" "Test Suite (ASan+UBSan)" "Tests" "Time" "Status"; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$($(ASAN_RUN_ENV) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		reports=$$(printf '%s\n' "$$output" | grep -cE '$(SAN_REPORT_RE)' || true); \
		if [ $$exit_code -eq 0 ] && [ $$reports -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			[ "$$failures" -eq 0 ] && failures=1; \
			reason=""; \
			if [ $$exit_code -eq 0 ]; then \
				failures=1; \
				reason="\n\033[0;31mThe binary exited 0 and gtest reported no failure. What failed is the $$reports sanitizer report line(s) in the output below, which a runner judging on exit status alone would have discarded.\033[0m"; \
			fi; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m$$reason\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

####################################################################
# Sanitizer Builds (TSan)
####################################################################

# ThreadSanitizer is a separate tree from the ASan one because the two cannot
# be combined: both replace the allocator, and a build asking for either after
# the other is rejected by the compiler.
#
# It answers a question nothing else in this Makefile asks.  Valgrind's
# memcheck does not look for races; helgrind does, but the library is not run
# under it.  ASan finds a use-after-free only once the timing that produces it
# has actually happened, so a race that the test machine happens to lose
# leaves no trace.  TSan reports the race itself, from a single interleaving,
# whether or not that interleaving corrupted anything - which is the only way
# to have any confidence in a threaded path that passes because the scheduler
# has been kind.
#
# There are four such paths now: parallel encode for lz4 and Zstandard, and
# parallel decode for both.
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g

TSAN_BUILD_DIR := ./build/$(BUILD)-tsan
TSAN_OBJ_DIR := $(TSAN_BUILD_DIR)/objects
TSAN_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.flags
TSAN_LINK_FLAGS_STAMP := $(TSAN_OBJ_DIR)/.linkflags
TSAN_APP_DIR := $(TSAN_BUILD_DIR)/apps

TSAN_LIBOBJECTS := $(patsubst src/%.c,$(TSAN_OBJ_DIR)/%.o,$(SOURCES))
TSAN_TARGET := $(BASE_NAME_PREFIX)-tsan.so

TSAN_TEST_HELPER_OBJ := $(TSAN_OBJ_DIR)/tests/common/test_helpers.o
TSAN_TEST_EXECUTABLES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

TSAN_CFLAGS := $(CFLAGS) $(TSAN_FLAGS) -DGCOMP_BUILD -DGCOMP_TEST_BUILD
TSAN_CXXFLAGS := $(CXXFLAGS) $(TSAN_FLAGS)
TSAN_LDFLAGS := $(LDFLAGS) $(TSAN_FLAGS)
TSAN_COMPRESSLIBRARY := -L $(TSAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-tsan

# Unlike the ASan build, this one does *not* preload its runtime, and must not.
#
# -fsanitize=thread at link time puts libtsan.so.2 first in the executable's
# own NEEDED list, so the runtime already initialises before anything it has to
# intercept; the preload the ASan block needs would be redundant here.  It is
# also actively harmful.  With libtsan preloaded, every system() call in the
# test suite fails - the shell it spawns inherits the preload and dies, and
# system() returns 11 whatever it was asked to run.
#
# That is not a small blast radius.  Every oracle in this project shells out:
# python3 for zlib, gzip and deflate, the zstd CLI for Zstandard and for the
# seekable format.  A preloaded run reports all of them as "reference not
# installed".  The first full run of this target failed 135 tests for that
# reason and not one of them was a race.
#
# What caught it is worth keeping: those suites treat an absent oracle as a
# failure rather than a skip.  Had they skipped, this would have been a green
# ThreadSanitizer run in which nothing was ever compared against a reference.
#
# LD_PRELOAD is emptied rather than left alone so that a preload inherited from
# the environment cannot reintroduce the problem.

# TSan requires position-independent code and refuses to start without it.
ifeq ($(UNAME_S), Linux)
	TSAN_CFLAGS += -fPIC
	TSAN_CXXFLAGS += -fPIE
endif

# Every TSan compile writes a .d file and every one is included below, for the
# reason spelled out above the first ASan compile rule: a sanitizer build
# assembled from objects that disagree about a struct can invent a failure and
# can hide a real one.
$(TSAN_OBJ_DIR)/%.o: src/%.c $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling (TSan instrumented): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(TSAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_APP_DIR)/$(TSAN_TARGET): $(TSAN_LIBOBJECTS)
	@printf "\n### Compiling TSan-instrumented Shared Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) -shared -o $@ $^ $(TSAN_LDFLAGS)

$(TSAN_OBJ_DIR)/tests/common/test_helpers.o: tests/common/test_helpers.cpp $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(TSAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(TSAN_FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling TSan Test Object: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(TSAN_CXXFLAGS) $(TEST_INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

TSAN_TEST_OBJECTS := $(patsubst tests/%.cpp,$(TSAN_OBJ_DIR)/tests/%.o,$(TEST_SOURCES))
TSAN_DEPFILES := $(TSAN_LIBOBJECTS:.o=.d) $(TSAN_TEST_HELPER_OBJ:.o=.d) \
	$(TSAN_TEST_OBJECTS:.o=.d)
-include $(TSAN_DEPFILES)

define tsan-test-executable-rule
TSAN_TEST_OBJ_$1 := $(TSAN_OBJ_DIR)/tests/$(patsubst tests/%.cpp,%.o,$1)

$(TSAN_APP_DIR)/$2$(EXE_EXTENSION): \
		$$(TSAN_TEST_OBJ_$1) \
		$(TSAN_TEST_HELPER_OBJ) \
		$(TSAN_APP_DIR)/$(TSAN_TARGET) \
		$(TSAN_LINK_FLAGS_STAMP)
	@printf "\n### Linking TSan %s Test ###\n" "$2"
	@mkdir -p $$(@D)
	$$(CXX) $$(TSAN_CXXFLAGS) -o $$@ $$(TSAN_TEST_OBJ_$1) $$(TSAN_TEST_HELPER_OBJ) $$(TSAN_LDFLAGS) $$(TESTFLAGS) $$(TSAN_COMPRESSLIBRARY)
endef

$(foreach pair,$(TEST_PAIRS),$(eval $(call tsan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

# halt_on_error stops at the first race rather than reporting the same one from
# every subsequent test, and history_size buys a deeper record of the other
# thread's accesses - the shallow default frequently reports a race with no
# stack for one side, which is not enough to act on.
# GCOMP_UNDER_SLOW_BUILD tells a test to run a reduced sweep. TSan is roughly
# twenty times slower than the plain build, and the sweeps that take the longest
# - test_bound's grid of every method against every option shape and size - are
# measuring format arithmetic, which a race detector has nothing to say about.
# Running them in full here costs many minutes and buys nothing; the same grid
# runs whole in the ordinary build and under ASan.
#
# The valgrind targets set GCOMP_UNDER_VALGRIND for the same reason, and the
# tests honour either.
TSAN_RUN_ENV := LD_LIBRARY_PATH="$(TSAN_APP_DIR)" LD_PRELOAD= \
	GCOMP_UNDER_SLOW_BUILD=1 \
	TSAN_OPTIONS="halt_on_error=1:history_size=7:second_deadlock_stack=1"

test-tsan: ## Run all tests with ThreadSanitizer (Linux only)
test-tsan: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@printf "\033[0;36m\n"
	@printf "###########################################\n"
	@printf "### Running tests with ThreadSanitizer  ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION) | sed 's/test/\u&/'); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests (TSan) ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		$(TSAN_RUN_ENV) $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\n"
	@printf "###########################################\n"
	@printf "### All tests passed with ThreadSanitizer ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

test-tsan-quiet: ## Run ThreadSanitizer tests with minimal output (Linux only)
test-tsan-quiet: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_TEST_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;33m%-30s %8s %10s %s\033[0m\n" "Test Suite (TSan)" "Tests" "Time" "Status"; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TSAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$($(TSAN_RUN_ENV) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		reports=$$(printf '%s\n' "$$output" | grep -cE '$(SAN_REPORT_RE)' || true); \
		if [ $$exit_code -eq 0 ] && [ $$reports -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			[ "$$failures" -eq 0 ] && failures=1; \
			reason=""; \
			if [ $$exit_code -eq 0 ]; then \
				failures=1; \
				reason="\n\033[0;31mThe binary exited 0 and gtest reported no failure. What failed is the $$reports sanitizer report line(s) in the output below, which a runner judging on exit status alone would have discarded.\033[0m"; \
			fi; \
			total_failed=$$((total_failed + failures)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m$$reason\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;33m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\n"
	@printf "Sanitizer builds are currently only supported on Linux\n"
	@printf "\033[0m\n"
	@exit 1
endif

# The threaded suites on their own.  The full run is the gate; this is what a
# change to an encoder's job scheduling gets run against while it is being
# written.
TSAN_THREADED_NAMES := testLz4_parallel testLz4_parallel_decode \
	testZstd_parallel testZstd_parallel_decode testThread_safety
TSAN_THREADED_EXECUTABLES := $(addprefix $(TSAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TSAN_THREADED_NAMES)))

test-tsan-threads: ## Run only the threaded suites under ThreadSanitizer
test-tsan-threads: $(TSAN_APP_DIR)/$(TSAN_TARGET) $(TSAN_THREADED_EXECUTABLES)
ifeq ($(OS_NAME), Linux)
	@for test_exe in $(TSAN_THREADED_EXECUTABLES); do \
		printf "\n\033[0;36m### %s (TSan) ###\033[0m\n" "$$(basename $$test_exe)"; \
		$(TSAN_RUN_ENV) $$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\nThreaded suites clean under ThreadSanitizer\n\033[0m\n"
else
	@printf "\033[0;31m\nSanitizer builds are currently only supported on Linux\n\033[0m\n"
	@exit 1
endif

sanitizer-help: ## Show sanitizer build help
	@printf "\033[0;36m"
	@printf "###########################################\n"
	@printf "### Sanitizer Builds                   ###\n"
	@printf "###########################################\n"
	@printf "\033[0m\n"
	@printf "Available targets:\n"
	@printf "  make test-asan   - Run tests with AddressSanitizer + UndefinedBehaviorSanitizer\n"
	@printf "  make test-ubsan  - Alias for test-asan (both run together)\n"
	@printf "  make test-tsan   - Run tests with ThreadSanitizer\n"
	@printf "  make test-tsan-threads - ThreadSanitizer, threaded suites only\n"
	@printf "\n"
	@printf "What these sanitizers detect:\n"
	@printf "  ASan (AddressSanitizer):\n"
	@printf "    - Buffer overflows (heap, stack, global)\n"
	@printf "    - Use-after-free\n"
	@printf "    - Use-after-return (with ASAN_OPTIONS=detect_stack_use_after_return=1)\n"
	@printf "    - Double-free\n"
	@printf "    - Memory leaks\n"
	@printf "  UBSan (UndefinedBehaviorSanitizer):\n"
	@printf "    - Signed integer overflow\n"
	@printf "    - Divide by zero\n"
	@printf "    - Null pointer dereference\n"
	@printf "    - Invalid shift operations\n"
	@printf "    - Out-of-bounds array access\n"
	@printf "    - Misaligned memory access\n"
	@printf "  TSan (ThreadSanitizer):\n"
	@printf "    - Data races between threads\n"
	@printf "    - Lock-order inversions (potential deadlocks)\n"
	@printf "    - Unsafe use of a thread-unsafe object from two threads\n"
	@printf "\n"
	@printf "TSan cannot be combined with ASan; it builds into its own tree.\n"
	@printf "It reports a race from one interleaving whether or not that run\n"
	@printf "corrupted anything, which is what ASan and valgrind cannot do.\n"
	@printf "\n"
	@printf "Note: Sanitizer builds are slower than regular builds but catch\n"
	@printf "bugs that may not cause immediate crashes in release builds.\n"
	@printf "\n"

clean: ## Remove all contents of the build directories.
# The sanitizer tree is removed too. It is a sibling of the ordinary build
# directory rather than a child, so a clean that names only the ordinary one
# leaves instrumented objects behind - and they are the ones a stale-binary
# mistake is hardest to notice with, because they still run.
	-@rm -rvf $(BUILD_DIR) $(ASAN_BUILD_DIR) $(TSAN_BUILD_DIR)

# Files will be as follows:
# /usr/local/lib/(SUITE)/
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR).(MINOR)
#   lib(SUITE)-(PROJECT)(BRANCH).so.(MAJOR) link to previous
#   lib(SUITE)-(PROJECT)(BRANCH).so link to previous
# Where the dynamic loader configuration fragment goes. Overridable so a
# staged or user-prefix install has somewhere to write it; the default is the
# system location, which is what an ordinary `sudo make install` uses.
LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d

# What goes in the .pc Requires: field. Built from the same variables the
# compile uses, so a dependency on another branch cannot be named one way for
# the build and another way for consumers.
PC_REQUIRES := $(CUTIL_PC)

# Where this project's own .pc file is installed. Defaults to the directory
# pkg-config is already being told to search, but separate from it so a
# staged install can write somewhere else without also redirecting lookups.
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)

# $(LDCONF_INSTALL_PATH)/(SUITE)-(PROJECT)(BRANCH).conf will point to $(LIB_INSTALL_PATH)/(SUITE)
# /usr/local/include/(SUITE)/(PROJECT)(BRANCH)
#   *.h copied from ./include/(PROJECT)
# /usr/local/share/pkgconfig
#   (SUITE)-(PROJECT)(BRANCH).pc created

install: ## Install the library globally, requires sudo
# Depends on all: install used to copy whatever happened to be in the build
# directory, so it could install a stale artifact or fail outright on a clean
# tree.
install: all
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
# Install the .so file
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	# Installing the ld configuration file.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
# The .dll goes in bin/, where the loader finds it once that directory is on
# PATH - Windows has no rpath. The import library goes where the .pc's -L
# points, lib/$(SUITE)/, as the .so does on Linux; in lib/ no -L named it.
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	# Installing the headers.
	# Removed first: this directory is owned entirely by this project and
	# branch, and copying over the top of it would leave headers behind that
	# have since been renamed or deleted.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -n "$$(find $(GEN_DIR) -maxdepth 1 -name '*.h' 2>/dev/null)" ]; then \
		mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT); \
		cp $(GEN_DIR)/*.h $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ghoti.io/$(PROJECT)/; \
	fi
	# Installing the pkg-config files.
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(LIB)|$(LIB_INSTALL_PATH)|g; s|(INCLUDE)|$(INCLUDE_INSTALL_PATH)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
	# Deleting the shared library.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	# Deleting the ld configuration file.
	@rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	# Deleting the headers.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	# Deleting the pkg-config files.
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	# Cleaning up (potentially) no longer needed directories.
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	# Running ldconfig.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the project in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Run all tests under valgrind in DEBUG mode (Linux only)
	make test-valgrind BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
# Cleans first because the object files would otherwise be reused without the
# instrumentation, then cleans and rebuilds at the end: leaving the
# instrumented objects behind would have a later `make` silently link them,
# and leaving the tree cleaned would break any sibling project that links
# this one. The cost is one extra build; coverage is not run often.
	@$(MAKE) --no-print-directory clean > /dev/null
# The instrumented build, the report and the restoration of the tree are one
# shell command so that the cleanup runs whatever fails. Letting a failure
# stop the recipe leaves the --coverage objects in build/, and the next
# ordinary `make` links them into a library that needs the gcov runtime; every
# later build then fails with undefined references to __gcov_init until
# somebody works out why.
#
# TEST_GATES is cleared because --coverage links the gcov runtime, which
# exports mangle_path. check-symbols is right to reject that in a shipping
# build and wrong to reject it here, and it made this target fail before it
# ever produced a report.
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		EXTRA_CFLAGS="--coverage -O0" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh --self-test || status=$$?; \
	fi; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(OBJ_DIR) || status=$$?; \
	else \
		printf "coverage: the instrumented test run failed; no report\n" >&2; \
	fi; \
	$(MAKE) --no-print-directory clean > /dev/null; \
	$(MAKE) --no-print-directory all > /dev/null; \
	exit $$status

help: ## Display this help
	@grep -E '^[ a-zA-Z0-9_-]+:.*?## .*$$' Makefile | sort | sed 's/\([^:]*\):.*## \(.*\)/\1:\2/' | awk -F: '{printf "%-15s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"


####################################################################
# Flag stamps
####################################################################
# Each build tree carries the flag string it was built with. The stamp is
# rewritten only when that string differs -- written to a scratch file,
# compared, moved into place only on a difference -- so its mtime moves on a
# flag change and on nothing else. EVERY object rule above depends on the stamp
# for its own build tree, and that has to stay true of any rule added later,
# because a stamp is only worth its coverage.
#
# Three rules were left out when these were introduced: the release, ASan and
# TSan test_helpers.o rules, each an explicit rule sitting apart from the
# pattern rules that did get the stamp. So `make EXTRA_CXXFLAGS=-O2`
# recompiled every object except those three and linked the result, and
# nothing said so -- an object records nowhere make can see what flags built
# it, and a mixed build links and runs like any other.
#
# **A stamp must record the variables the recipes EXPAND, not the ones those
# derive from.** This is a separate failure from missing the prerequisite, it is
# invisible to a check that only asks whether a rule names a stamp, and it was
# live here in eleven distinct (stamp, variable) pairs. The library objects
# compile with $(LIB_CFLAGS) while this stamp recorded $(CFLAGS); every test
# object compiles with $(TEST_INCLUDE) while all three stamps recorded
# $(INCLUDE); and no stamp recorded $(CC), $(CXX) or $(AFL_CC) at all. Each of
# those is $(PARENT) plus literal text, so editing the literal -- changing
# -fvisibility=hidden, adding an -I -- leaves the recorded parent byte for byte
# identical and rebuilds nothing.
#
# Measured, with the control in the same run: changing -fvisibility=hidden to
# -fvisibility=default and adding an -I to TEST_INCLUDE, both as plain Makefile
# edits, recompiled 0 objects and moved no mtime, while a command-line
# EXTRA_CFLAGS change moved both in the same tree. `make CC=clang` recompiled 0
# objects and left DW_AT_producer reading "GNU C17 14.2.0" -- so a clang
# cross-check was comparing gcc against gcc, which matters because clang's UBSan
# reports what gcc's does not.
#
# The witness has to be a paired test with a control, because "it rebuilt" and
# "make found nothing to do" are the same empty output: build two objects,
# change a flag on the command line, then compare mtimes and the -O tokens gcc
# records in .debug_str. The stamped object moved and its recorded command line
# gained the new flag; the unstamped one did neither. Read the tokens as
# occurrences, not decisions -- both -O3 and -O2 appear on a command line where
# EXTRA_CFLAGS appends, and the last one is what the compiler used.
#
# This replaces listing `Makefile` as a prerequisite, which was too broad (a
# comment-only edit recompiled everything) and too narrow (a command-line
# override such as `make EXTRA_CFLAGS=-O2` changes no file's mtime and so was
# invisible).
#
# These rules sit at the end of the file for two reasons. A rule's target
# expands when make reads the line, so a stamp rule above its own OBJ_DIR
# definition has an empty target: not an error, just a rule that silently does
# not exist. And the first target in a makefile is the default goal, so a stamp
# rule above `all:` makes a bare `make` build the stamp and nothing else.
.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(LIB_CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE) $(TEST_INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(AFL_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(AFL_CC) $(AFL_CFLAGS) $(AFL_LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE) $(TEST_INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CXX) $(TSAN_CFLAGS) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(INCLUDE) $(TEST_INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

# Link stamps, one per tree, kept separate from the flag stamps above so that a
# gtest move relinks the test binaries without recompiling 189 library objects.
# The test executables were the population no flag stamp covered: they compile
# and link in one step, so they are not object rules, and $(TESTFLAGS) appears
# nowhere else. Measured before this existed: changing TESTFLAGS relinked 0 of
# the test binaries and ran 0 compiler invocations, while an EXTRA_LDFLAGS
# change relinked them (transitively, through LDFLAGS -> flag stamp -> objects
# -> archive) and touching a test source relinked one, so the probe could move.
$(LINK_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CXX) $(LDFLAGS) $(TESTFLAGS) $(TESTFLAGS_PC)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_LINK_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CXX) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(TESTFLAGS) $(TESTFLAGS_PC)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(TSAN_LINK_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CXX) $(TSAN_CXXFLAGS) $(TSAN_LDFLAGS) $(TESTFLAGS) $(TESTFLAGS_PC)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
