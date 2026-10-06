/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Compress.
 *
 * Ghoti.io Compress is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Compress is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file deflate_encode.c
 *
 * Streaming DEFLATE (RFC 1951) encoder for the Ghoti.io Compress library.
 *
 * Implements multiple compression strategies based on level:
 * - Level 0: Stored blocks (no compression, data copied verbatim)
 * - Levels 1-3: Fixed Huffman codes with LZ77 (shorter hash chains)
 * - Levels 4-9: Dynamic Huffman codes with LZ77 (optimal code lengths from
 *   symbol frequency histograms, longer hash chains for better matching)
 *
 * The encoder maintains a sliding window for LZ77 back-references and uses
 * hash chains for efficient match finding. Dynamic Huffman blocks are built
 * by collecting symbol frequencies during LZ77 matching, then constructing
 * optimal length-limited (15-bit max) Huffman codes.
 *
 * See the "Dynamic Huffman Encoding" section below for algorithm details.
 */

#include <ghoti.io/compress/macros.h>
#include "../../core/alloc_internal.h"
#include "../../core/bitcost.h"
#include "../../core/huffman_lengths.h"
#include "../../core/stepdown.h"
#include "../../core/registry_internal.h"
#include "../../core/stream_internal.h"
#include "bitwriter.h"
#include "deflate_internal.h"
#include "huffman.h"
#include <ghoti.io/compress/limits.h>
#include <stdint.h>
#include <string.h>

//
// Constants (RFC 1951)
//

#define DEFLATE_WINDOW_BITS_DEFAULT 15u
#define DEFLATE_WINDOW_BITS_MIN 8u
#define DEFLATE_WINDOW_BITS_MAX 15u

#define DEFLATE_MAX_STORED_BLOCK 65535u

/**
 * @brief Fewest symbols a block may be allowed to hold, whatever the window.
 *
 * The symbol buffer used to be exactly `window_size` entries, which tied the
 * length of a block to the reach of the match finder although the two have
 * nothing to do with each other.  The cost falls on incompressible data at a
 * small window: a block that covers 256 bytes pays five bytes of RFC 1951
 * section 3.2.4 framing on them, which is 2%, and the framing is the whole
 * point of storing the block.
 *
 * zlib separates the same two things - its symbol buffer comes from
 * `memLevel`, its window from `windowBits` - and that is why its expansion on
 * incompressible input is about 0.23% at every window size.  4096 symbols is a
 * quarter of what zlib's default asks for and brings the framing to about
 * 0.12%.
 */
#define DEFLATE_SYM_BUF_MIN 4096u

/**
 * @brief Size of the ring a stored block is written from, when one is needed.
 *
 * A stored block covers at most DEFLATE_SYM_BUF_MIN bytes of literal-heavy
 * input, and the lookahead standing in front of it is bounded by
 * DEFLATE_REFILL_LOOKAHEAD, so twice the symbol floor holds both with room to
 * spare.  A window at least this large can serve the block itself and no ring
 * is allocated, which is every window_bits from 13 up.
 */
#define DEFLATE_STORED_RING_SIZE (2u * DEFLATE_SYM_BUF_MIN)
#define DEFLATE_MAX_LITLEN_SYMBOLS 288u
#define DEFLATE_MAX_DIST_SYMBOLS 32u
#define DEFLATE_MIN_MATCH_LENGTH 3u
#define DEFLATE_MAX_MATCH_LENGTH 258u
#define DEFLATE_MAX_DISTANCE 32768u

// Distance past which a three-byte match stops paying for itself.
#define DEFLATE_TOO_FAR 4096u

/**
 * @brief How hard one compression level searches.
 *
 * WHAT A LEVEL MEANS
 * ==================
 *
 * RFC 1951 says nothing about any of this.  It describes the stream, not how
 * to choose what goes in it, so a level is not a specified thing -- it is a
 * promise about how much work the encoder will spend, and the only way to set
 * one is to measure.
 *
 * Two numbers decide it:
 *
 * - `max_chain`: how many candidates the hash chain walk will consider.
 *
 * - `max_lazy`: hold a match shorter than this back one byte, to see whether
 *   the next position starts a longer one.  Levels 1 to 3 do not defer at all
 *   (see use_lazy in the caller), so for them this is read only when the lazy
 *   strategy is asked for by name -- which is a request for the effort, and
 *   gets the threshold the deferring levels use.  At the level-shaped value of
 *   4 it would mean "hold a match back only if it is exactly three bytes
 *   long", and a three-byte match is the one case where deferring cannot pay:
 *   the byte given up is a literal, the match that displaces it is four bytes
 *   at best, and the sequence it replaces would have been found at the next
 *   position anyway.
 *
 * NINE LEVELS THAT WERE FOUR
 * ==========================
 *
 * These used to be set by three range tests, and the ranges made levels into
 * aliases of each other.  Compressing 11 MB of source, manuals, XML and an ELF
 * binary at each level in turn:
 *
 * | Level | Size      | Against the level below |
 * |-------|-----------|-------------------------|
 * | 1     | 2,594,541 |                         |
 * | 2     | 2,594,541 | identical               |
 * | 3     | 2,594,541 | identical               |
 * | 4     | 2,351,738 | -9.36%                  |
 * | 5     | 2,351,738 | identical               |
 * | 6     | 2,351,738 | identical               |
 * | 7     | 2,318,112 | -1.43%                  |
 * | 8     | 2,318,112 | identical               |
 * | 9     | 2,315,952 | -0.09%                  |
 *
 * Byte for byte identical, not merely close.  Five of the nine levels did
 * nothing but round down to another level, and a caller asking for 8 got 7
 * while paying 8's reputation.  A table has one row per level because there
 * are nine levels.
 *
 * WHAT ZLIB HAS THAT IS NOT HERE
 * ==============================
 *
 * zlib's table carries two more numbers, and both were tried here:
 *
 * - `nice_length`: stop walking the chain once a match is this long.
 * - `good_length`: when the match already held is this long, walk a quarter
 *   as far.
 *
 * Neither does anything measurable in this encoder.  At level 6 with a chain
 * of 128, adding zlib's good_length moved the encode rate from 31.2 MB/s to
 * 31.5 and the output from 0.11% above zlib to 0.16%; adding its nice_length
 * moved it to 29.7 MB/s and 0.12%.  Both are inside the noise on speed and
 * slightly worse on size.  On the input where nice_length should bind hardest
 * -- runs, structured records and skewed bytes, where matches are long and
 * chains are deep -- cutting it from off to 128 to 64 left the rate at 1.4
 * MB/s throughout and cost bytes each time.
 *
 * The reason is the guard in deflate_find_match that stops the walk when the
 * chain stops going backwards through the stream.  That guard already ends
 * the average walk after about three candidates, so a knob whose job is to
 * end the walk early has nothing left to end.  They are not here because
 * carrying configuration that does not configure anything is how the last
 * dead setting went unnoticed for months.
 */
typedef struct {
  uint32_t max_lazy; ///< Defer a match shorter than this.
  int max_chain;     ///< Longest hash chain walk.
  int use_opt;       ///< Parse by shortest path instead of deferring.
  uint32_t opt_budget; ///< Shortened matches one swept position may try.
} deflate_effort_t;

/// Indexed by compression level; entry 0 is unused (level 0 stores).
static const deflate_effort_t k_deflate_effort[10] = {
    {0, 0, 0, 0},      // 0: stored, nothing is searched
    {16, 4, 0, 0},     // 1
    {16, 8, 0, 0},     // 2
    {16, 16, 0, 0},    // 3
    {16, 16, 0, 0},    // 4: the first level that defers
    {16, 32, 0, 0},    // 5
    {16, 128, 0, 0},   // 6: the default
    {32, 64, 1, 32},   // 7: first to parse by shortest path
    {128, 96, 1, 64},  // 8
    {258, 128, 1, 128} // 9
};

//
// The optimal parse
// =================
//
// WHAT A PARSE IS CHOOSING
// ------------------------
//
// The match finder answers "what can I match here".  The parse answers the
// harder question: which of those matches to actually emit.  A match taken at
// one position blocks every match that starts inside it, so the stream that
// costs the fewest bits is often not the stream of longest matches.
//
// Everything above this is greedy, with a look-ahead of one byte (lazy
// matching).  That look-ahead cannot see what either choice costs three
// matches later, and no amount of deepening will make it.
//
// This replaces it with a shortest path search.  Think of the lookahead as a
// graph: one vertex per byte position, one edge for "write this byte as a
// literal", and one edge per (length, distance) pair for "emit this match".
// Give every edge the bits writing it would cost and the cheapest encoding is
// the shortest path across it.  Every edge goes forward and there are no
// cycles, so one sweep left to right settles it: when the sweep reaches a
// position, every edge into it has been relaxed and its cost is final.
//
// DEFLATE IS THE EASY CASE
// ------------------------
//
// The zstd parse in zstd_optimal.c has to approximate, because a sequence
// carries a literal length code: what a run of literals costs depends on how
// long the run is, so the cost of a path is not the sum of the costs of its
// edges.  DEFLATE has no such code.  Every literal is its own symbol in the
// same alphabet as the lengths (RFC 1951 section 3.2.5), and a match costs a
// length code, a distance code, and their extra bits, and nothing else.
//
// The cost is therefore exactly additive, and this sweep finds the true
// minimum for the prices it is given -- not an approximation of it.  What is
// approximate is only the prices, and they are approximate for a reason no
// parse can avoid: the Huffman codes are not built until the block is
// finished, and the block cannot be finished until the parse has chosen it.
//
// WHAT AN EDGE COSTS
// ------------------
//
// The entropy of the symbol under the statistics of what has been encoded
// recently -- a symbol that has been coming up often is cheap, one that has
// not is dear, which is what a Huffman code will charge for it.  Prices are
// in 256ths of a bit; see ../../core/bitcost.h.
//
// The extra bits after a length or distance code are priced exactly, because
// the format fixes them: they are written uncompressed, so their count is
// their cost (RFC 1951 section 3.2.5).  That is what lets the parse prefer a
// shorter match nearby to a longer one far away -- a distance over 24576
// carries thirteen raw bits, which is more than a literal byte usually
// costs.
//
// WHERE THE STATISTICS COME FROM
// ------------------------------
//
// Before anything has been encoded there is nothing to measure, so the model
// starts from the fixed Huffman code the format itself defines (RFC 1951
// section 3.2.6): eight bits for the common literals, nine for the rest,
// seven for the short length codes, five for every distance.  A count of
// 2^(15-len) prices each symbol at exactly the length that code gives it, so
// the first block is parsed as though it were going to be written with the
// fixed code -- which is the format's own guess at what a block looks like.
//
// From there the model counts what it emits as it emits it, and the prices
// are rebuilt at every sweep.  The counts are halved at the start of each
// block, which makes the model a moving average rather than a running total:
// the block being parsed counts fully, the one before it half as much, the
// one before that a quarter.
//

/// Bits an optimal-parse price carries.  See ../../core/bitcost.h.
#define DEFLATE_OPT_PRICE_ONE GCOMP_BITCOST_ONE
#define DEFLATE_OPT_PRICE_SHIFT GCOMP_BITCOST_SHIFT

/// No path reaches here yet.  Large enough that adding any one edge cannot
/// wrap: the dearest edge is a length code, a distance code and their extra
/// bits, well under a hundred bits.
#define DEFLATE_OPT_PRICE_INF 0xF0000000u

/// Most lookahead the optimal levels hold, against 1 KB for the rest.
///
/// This does NOT simply want to be as large as it fits, and the reason is
/// worth stating.  The sweep cannot see past the lookahead, so a longer one
/// sees further -- but the window holds history and lookahead together, so
/// every byte of one is a byte of reachable history the other does not have.
/// The two cross, and they cross early.  On 9 MB of manuals, C source and
/// XML at a chain of 64:
///
///   bytes of lookahead   1024    2048    4096    8192   16384
///   output bytes      1852559 1848256 1851696 1861539 1890667
///
/// Sixteen kilobytes is 2.3% worse than two, and slower for having swept
/// more to get there: half the window spent on lookahead is half the
/// distance range the matches can reach into.
#define DEFLATE_OPT_LOOKAHEAD 2048u

/**
 * @brief Lookahead an optimal level holds over a window of @p window_size.
 *
 * A sixteenth of the window, which is where the measurement above puts it
 * for the 32 KB window the format allows, and keeps the same share of a
 * smaller one.
 */
static inline size_t deflate_opt_lookahead(size_t window_size) {
  size_t want = window_size / 16u;
  if (want > DEFLATE_OPT_LOOKAHEAD) {
    want = DEFLATE_OPT_LOOKAHEAD;
  }
  return want;
}

/// Shortened versions of matches one position may relax, over and above each
/// match's whole length, which is always relaxed.  Set per level; this is
/// what the table calls opt_budget.
///
/// Truncating a match is worth trying because it moves where the next one may
/// start, but trying every truncation of every match would make a run of
/// identical bytes cost time quadratic in its length.  The budget is spent
/// from the short end upwards, and candidates are offered shortest first,
/// because that is where a truncation can change a parse: shortening a
/// five-byte match to four moves where the next one may start, shortening a
/// two-hundred byte match to a hundred and ninety-nine almost never does.

/// Fewest positions worth sweeping.  A block is closed rather than swept in
/// smaller pieces than this, which is also what guarantees the sweep always
/// makes progress: without a floor, a symbol buffer with one or two entries
/// left produced a sweep of one or two positions, which is shorter than the
/// shortest match, and the encoder went round its loop for ever emitting
/// nothing.
#define DEFLATE_OPT_MIN_SWEEP 64u

/// Matches one position may consider.  A walk records one per improvement,
/// and no level walks a chain further than this many candidates deep without
/// the improvements having long since run out.
#define DEFLATE_OPT_MAX_CANDIDATES 32u

/**
 * @brief One position in the sweep.
 *
 * `price` and the edge that arrives are filled going forwards; `out_length`
 * and `out_distance` are filled afterwards by walking the finished path
 * backwards from its end, which is what lets the emit walk it forwards.
 */
typedef struct deflate_opt_node_s {
  uint32_t price;         ///< Bits*256 to encode everything before here.
  uint32_t length;        ///< Arriving edge's match length; 0 for a literal.
  uint32_t distance;      ///< Arriving edge's distance.
  /**
   * @brief How long the arriving match could have run, had it been allowed.
   *
   * A match is relaxed at every length up to where the sweep ends, so a
   * match that would have run past the end arrives truncated.  For every
   * step but the last that is the right answer -- the path continues from
   * there -- but the last step of a path has nothing after it, and cutting
   * it is the boundary deciding rather than the cost.  Keeping the whole
   * length lets the last step be put back to it; see the emit loop.
   */
  uint32_t full;
  uint32_t out_length;    ///< Leaving edge's match length; 0 for a literal.
  uint32_t out_distance;  ///< Leaving edge's distance.
  uint32_t out_full;      ///< Leaving edge's untruncated length.
} deflate_opt_node_t;

// The window holds history and lookahead in one circular buffer, so every
// byte of lookahead is a byte of history the encoder does not have.  A match
// is at most DEFLATE_MAX_MATCH_LENGTH bytes and needs three more to be worth
// looking for, so anything past that is lookahead held for no reason.
//
// Refilling in batches of DEFLATE_REFILL_LOOKAHEAD amortises the copy without
// giving up a meaningful amount of history: 1 KB out of a 32 KB window leaves
// 97% of the legal distance range reachable.
#define DEFLATE_MIN_LOOKAHEAD \
  (DEFLATE_MAX_MATCH_LENGTH + DEFLATE_MIN_MATCH_LENGTH + 1u)
#define DEFLATE_REFILL_LOOKAHEAD 1024u

// Hash chain configuration
#define DEFLATE_HASH_BITS 15u
#define DEFLATE_HASH_SIZE (1u << DEFLATE_HASH_BITS)
#define DEFLATE_HASH_MASK (DEFLATE_HASH_SIZE - 1u)
#define DEFLATE_NIL 0u

//
// Encoder state machine
//

typedef enum {
  DEFLATE_ENC_STAGE_INIT = 0,
  DEFLATE_ENC_STAGE_ACCEPTING,
  DEFLATE_ENC_STAGE_FLUSHING,
  DEFLATE_ENC_STAGE_DONE,
} gcomp_deflate_encoder_stage_t;

//
// Compression strategies
// ======================
//
// The `deflate.strategy` option controls how the encoder finds and encodes
// matches. Each strategy optimizes for different data characteristics:
//
// DEFLATE_STRATEGY_DEFAULT (strategy="default")
// ---------------------------------------------
// Standard LZ77 with hash-chain match finding, suitable for most data.
// - Uses hash chains to find repeated byte sequences in the sliding window
// - Chain length varies by compression level (4/32/128 at L1-3/L4-6/L7-9)
// - Levels 4 and up defer a match one byte to see whether the next position
//   starts a longer one; levels 1 to 3 take what they find.  This is where
//   zlib switches from deflate_fast to deflate_slow, and it is the same
//   trade: across a 19 MB corpus of source, prose, XML, binaries and images
//   it is worth 2.3% at level 6 and 2.6% at level 9, for about half the
//   encode throughput on text.
// - Chooses fixed or dynamic Huffman based on compression level
// - Good balance of speed and compression for general-purpose data
//
// DEFLATE_STRATEGY_LAZY (strategy="lazy")
// ---------------------------------------
// DEFAULT, with match deferral at every level instead of from level 4.
//
// Implementation differences from DEFAULT:
// - Defers a match at every level, including 1 to 3: a match is held back one
//   byte to see whether the next position starts a longer one, and the search
//   that position performs anyway is what settles it.  See
//   deflate_find_match()'s caller.
// - Uses the deferral threshold of the levels that defer, so that deferring
//   at levels 1 to 3 is worth doing.  See max_lazy in that caller.
// - Nothing else.  It searches exactly as hard as DEFAULT: same hash chain
//   lengths, same everything.
//
// Since DEFAULT defers from level 4 up, this is identical to DEFAULT at
// levels 4 to 9 and differs only at levels 1 to 3.  What it offers there is
// the fast levels' search effort with the slow levels' deferral: on 2 MB of
// filter-shaped bytes it reaches 22.428% against DEFAULT's 25.065%, and on
// 12 MB of source, prose, XML and binaries 25.954% against 26.680%, for about
// 10% of the encode throughput on general data and none of it on filter output.
//
// WHY IT IS NOT CALLED "FILTERED"
// -------------------------------
// It was, until the name was measured against what it does.  RFC 1951 has no
// notion of a strategy at all -- it defines the stream and leaves the encoder
// free, and nothing in a DEFLATE stream records which strategy produced it.
// "Filtered" is zlib's word, from Z_FILTERED, and zlib gives it a specific
// meaning:
//
//     The effect of Z_FILTERED is to force more Huffman coding and less
//     string matching; it is somewhat intermediate between
//     Z_DEFAULT_STRATEGY and Z_HUFFMAN_ONLY.
//
// This strategy does the opposite: more string matching, at the cost of
// speed.  zlib's Z_FILTERED is faster than its default; this was slower than
// ours.  Borrowing the word and inverting the meaning is a worse trap than an
// unfamiliar name, because a caller who knows zlib would get the opposite of
// what they asked for -- and the other three names here (huffman_only, rle,
// fixed) do match zlib's meanings, so this one was the odd one out.
//
// zlib's actual rule was measured here and is not worth having: discarding
// matches shorter than six bytes costs 0.3% on filter-shaped data and 6.1%
// on general data.
//
// "Lazy" is the term of art for what this does -- zlib's own source calls the
// path deflate_slow and the threshold lazy_match -- so the name now says the
// mechanism rather than a use case it was never tuned for.
//
// It remains a good choice for PNG filter output, which is what the image
// library uses it for; it is simply not the only thing it is good for.
//
// It used to search four times as deep as DEFAULT as well - 16/128/256
// against 4/32/128 - on the reasoning that filter output hides longer
// patterns behind short chains.  Measured across 52 files of real PNG
// filtered rows, 7.3 MB, that is not where the win is:
//
//     chain   with deferral   without
//        32      31.22%       32.91%
//        64      31.13%       33.42%
//       128      31.13%       33.41%
//       256      31.12%       33.40%
//
// Chain length is worth 0.1 points across a factor of eight.  Deferral is
// worth 1.7.  So the chains came back down.
//
// DEFLATE_STRATEGY_HUFFMAN_ONLY (strategy="huffman_only")
// -------------------------------------------------------
// Skip LZ77 entirely; emit all input bytes as literals.
// - No hash table lookups or match searching
// - Only entropy encoding via Huffman codes
// - Extremely fast encoding, minimal compression
//
// Use cases:
// - Pre-compressed data (JPEG, PNG, ZIP contents) where LZ77 finds few matches
// - High-entropy data (random, encrypted) where searching is wasted effort
// - When encoding speed is more important than compression ratio
//
// Note: Even with huffman_only, the encoder builds optimal dynamic Huffman
// codes from literal byte frequencies (at levels 4+), providing some
// compression for non-uniform byte distributions.
//
// DEFLATE_STRATEGY_RLE (strategy="rle")
// ------------------------------------
// Run-length encoding: only find matches at distance 1.
// - No hash chains needed; just check if current byte equals previous byte
// - Very fast O(n) matching with no memory overhead
// - Good compression for data with long runs of repeated bytes
//
// Use cases:
// - Simple graphics (icons, diagrams) with solid color regions
// - Sparse data (arrays with many zeros)
// - Any data dominated by repeated byte patterns
//
// Implementation: At each position, scans forward while bytes match the
// immediately preceding byte, up to DEFLATE_MAX_MATCH_LENGTH (258).
//
// DEFLATE_STRATEGY_FIXED (strategy="fixed")
// ----------------------------------------
// Always use fixed Huffman tables, skip dynamic tree building.
// - Avoids the overhead of computing optimal Huffman codes
// - Avoids transmitting custom Huffman tree in block header
// - Faster encoding, slightly worse compression ratio
//
// Combines with any compression level:
// - Level 0 + fixed: stored blocks (fixed has no effect)
// - Level 1-9 + fixed: LZ77 matching at that level, but fixed Huffman output
//
// Useful when:
// - Encoding many small blocks where tree overhead dominates
// - Very speed-sensitive applications
// - Data that compresses similarly with fixed vs dynamic codes
//

typedef enum {
  DEFLATE_STRATEGY_DEFAULT = 0,
  DEFLATE_STRATEGY_LAZY,
  DEFLATE_STRATEGY_HUFFMAN_ONLY,
  DEFLATE_STRATEGY_RLE,
  DEFLATE_STRATEGY_FIXED,
} gcomp_deflate_strategy_t;

/**
 * @brief LZ77 match result.
 */
typedef struct {
  uint32_t length;   ///< Match length (0 if no match found).
  uint32_t distance; ///< Match distance (1-based).
} deflate_match_t;

typedef struct gcomp_deflate_encoder_state_s {
  //
  // Allocator (for internal memory operations)
  //
  const gcomp_allocator_t * allocator;

  //
  // Configuration
  //
  int level;
  size_t window_bits;
  size_t window_size;
  /**
   * @brief window_size - 1, for wrapping indices into the circular window.
   *
   * window_size is always a power of two (it is `1 << window_bits`, and
   * RFC 1951 section 3.2.1 bounds window_bits to 8..15), so wrapping is a
   * mask rather than a division.  The compiler cannot make that rewrite on
   * its own because window_size is a runtime value, so `% window_size` had
   * been compiling to a 64-bit `div` - including two per byte compared in
   * the innermost loop of deflate_find_match().
   */
  size_t window_mask;
  gcomp_deflate_strategy_t strategy;

  //
  // Limits
  //
  uint64_t max_memory_bytes;

  //
  // Memory tracking
  //
  gcomp_memory_tracker_t mem_tracker;

  /**
   * @brief Times this encoder settled for a weaker encoding, and why.
   *
   * Read by tests, which assert that nothing was forced.  See
   * src/core/stepdown.h for why a count is needed at all.
   */
  gcomp_stepdown_tally_t stepdowns;

  /**
   * @brief Input bytes the buffered symbols stand for.
   *
   * Only used to decide when to close a block early so that a stored block
   * stays reachable; deflate_block_input_length() is what the block writer
   * trusts.  A drift here costs a block boundary, never a wrong block.
   */
  size_t block_input_len;

  //
  // State machine
  //
  gcomp_deflate_encoder_stage_t stage;
  int final_block_written;

  //
  // Sliding window buffer for LZ77
  //
  uint8_t * window;
  size_t window_pos;  ///< Next write position (circular index).
  size_t window_fill; ///< Total bytes written (capped at window_size).
  size_t lookahead;   ///< Bytes available for matching.

  /**
   * @brief Recent input kept only so that a stored block can be written.
   *
   * NULL whenever the window is already at least DEFLATE_STORED_RING_SIZE, in
   * which case the window serves and this costs nothing.  Below that the
   * window cannot hold a block and its lookahead at once - at window_bits 8 it
   * cannot hold even one maximum-length match - so the bytes are kept here
   * instead.
   *
   * It holds stream bytes only.  A preset dictionary is laid into the window
   * because the match finder may reference it (RFC 1950 section 2.2), but a
   * stored block never covers dictionary bytes, so priming this would only
   * make @ref stored_fill claim history the block cannot use.
   */
  uint8_t * stored_buf;
  size_t stored_size; ///< Power of two; DEFLATE_STORED_RING_SIZE.
  size_t stored_mask; ///< stored_size - 1.
  size_t stored_pos;  ///< Next write position, mirroring window_pos.
  size_t stored_fill; ///< Stream bytes written (capped at stored_size).

  /**
   * @brief A match found at the previous position and not yet emitted.
   *
   * Lazy matching asks whether the byte at p is better spent as a literal,
   * because the match starting at p+1 is longer than the one starting at p.
   * Answering it needs the search at p+1, so the match at p is held here
   * while the encoder moves on one byte; the next iteration's own search is
   * the one that settles it.  @ref lazy_length is 0 when nothing is held.
   *
   * It lives in the encoder state, not on the stack, because the batch loop
   * can return to the caller between the two positions.  Carrying it means
   * streaming a stream in small pieces produces the same bytes as encoding it
   * in one call, which is the property the previous arrangement could not
   * have offered had it kept anything at all.
   *
   * The deferred position has already been consumed from @ref lookahead and
   * entered into the hash chains, so emitting the match consumes only its
   * remaining @ref lazy_length - 1 bytes.
   */
  uint32_t lazy_length;
  uint32_t lazy_distance; ///< Distance of the held match; valid with length.
  size_t total_in;    ///< Total bytes written to window (for hash validity).

  /**
   * @brief A preset dictionary, copied because the options may not outlive us.
   *
   * RFC 1950 section 2.2's dictionary is history, not data: these bytes go
   * into the window and the hash chains ahead of the first input byte and are
   * never emitted.  Kept so that gcomp_encoder_reset() can lay them down
   * again - a reset starts a new stream, and a new stream starts from the
   * same history this one did.
   */
  uint8_t * dict_bytes;
  size_t dict_len;

  //
  // Hash chain for LZ77 match finding
  // ==================================
  //
  // The encoder uses hash chains to efficiently find repeated byte sequences
  // in the sliding window. For each 3-byte sequence, a hash value is computed.
  // Positions with the same hash are linked together in a chain, allowing
  // quick traversal of potential match candidates.
  //
  // Data structures:
  // - hash_head[hash]: Buffer index of most recent position with this hash
  // - hash_prev[idx]:  Buffer index of previous position in same chain
  // - hash_pos[idx]:   Stream position when this entry was inserted (for
  //                    validity checking - entries older than window_size
  //                    bytes are stale)
  // - hash_at[idx]:    Hash value that was used when inserting at this buffer
  //                    index (for proactive invalidation - see below)
  //
  // HASH CHAIN INVALIDATION (Critical for correctness)
  // ---------------------------------------------------
  //
  // Because the sliding window is circular, buffer indices are reused when
  // the window wraps. This creates a subtle corruption problem:
  //
  // Consider buffer index 100:
  //   1. First use: hash("abc")=500 → hash_head[500]=100, hash_prev[100]=...
  //   2. Window wraps, index 100 now contains different data
  //   3. Second use: hash("xyz")=700 → hash_head[700]=100
  //
  // Problem: hash_head[500] still points to 100, but 100 is now in chain 700!
  // If we search chain 500, we'll follow hash_prev[100] into chain 700's
  // history, causing incorrect matches or infinite loops.
  //
  // Solution: Proactive invalidation using hash_at[]:
  //   - When inserting at buffer index idx with new hash:
  //     1. Check old_hash = hash_at[idx]
  //     2. If hash_head[old_hash] == idx (this index is head of old chain)
  //        AND old_hash != new_hash, set hash_head[old_hash] = NIL
  //     3. This ensures the old chain doesn't dangle into the wrong chain
  //
  // The check "old_hash != new_hash" is important: if we're re-inserting into
  // the same chain (same data at same position), we don't want to invalidate
  // the chain head, as that would corrupt hash_prev linkage.
  //
  uint16_t * hash_head; ///< Head of each hash chain (hash → buffer index).
  uint16_t * hash_prev; ///< Previous link in hash chain (buffer index → index).
  size_t * hash_pos;    ///< Stream position when entry was inserted.
  uint16_t * hash_at;   ///< Hash value at each buffer position (for proactive
                        ///< invalidation when buffer indices are reused).
  uint32_t hash_value; ///< Running hash value.

  //
  // Output bitstream
  //
  gcomp_deflate_bitwriter_t bitwriter;

  //
  // Block buffering for stored blocks (level 0)
  //
  uint8_t * block_buffer;
  size_t block_buffer_size;
  size_t block_buffer_used;

  //
  // Symbol buffer for Huffman encoding
  //
  uint16_t * lit_buf;  ///< Literal/length symbols.
  uint16_t * dist_buf; ///< Distance values (0 for literals).
  size_t sym_buf_size; ///< Capacity of symbol buffers.
  size_t sym_buf_used; ///< Number of symbols buffered.

  //
  // Histograms for dynamic Huffman
  //
  uint32_t * lit_freq;  ///< Literal/length frequencies.
  uint32_t * dist_freq; ///< Distance frequencies.

  //
  // Fixed Huffman codes (precomputed)
  //
  uint16_t fixed_lit_codes[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint8_t fixed_lit_lens[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint16_t fixed_dist_codes[DEFLATE_MAX_DIST_SYMBOLS];
  uint8_t fixed_dist_lens[DEFLATE_MAX_DIST_SYMBOLS];
  int fixed_ready;

  //
  // Finish buffer for incremental output during finish()
  //
  // When finish() is called, the entire final output is rendered to this
  // internal buffer first, then copied incrementally to the user's output
  // buffer. This allows finish() to work with arbitrarily small output
  // buffers (even 1 byte at a time) without corrupting the output stream.
  //
  uint8_t * finish_buf;     ///< Buffer holding rendered finish output.
  size_t finish_buf_size;   ///< Allocated size of finish_buf.
  size_t finish_buf_used;   ///< Bytes written to finish_buf.
  size_t finish_buf_copied; ///< Bytes already copied to user output.
  int finish_buf_ready;     ///< Non-zero if finish output is fully rendered.
  /**
   * Output staged by update() before it is handed to the caller.
   *
   * Block flushes used to be written straight into the caller's buffer, so a
   * flush that did not fit returned GCOMP_ERR_LIMIT *after* the input it came
   * from had already been consumed - an unrecoverable state rather than a
   * retryable one, which made streaming through a bounded output buffer
   * impossible. Blocks are now rendered here first and copied out as space
   * allows, exactly as finish() already did with finish_buf.
   */
  //
  // Optimal parse (see "The optimal parse" below).  The counts are a moving
  // average of what has been emitted, the prices are built from them, and
  // the nodes are the sweep's table -- one per position of a sweep, plus the
  // one past its end.
  //
  uint32_t opt_lit_freq[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint32_t opt_dist_freq[DEFLATE_MAX_DIST_SYMBOLS];
  uint32_t opt_lit_price[DEFLATE_MAX_LITLEN_SYMBOLS];
  uint32_t opt_dist_price[DEFLATE_MAX_DIST_SYMBOLS];
  uint32_t opt_budget;                   ///< Shortenings a position may try.
  size_t opt_inserted_to;                ///< Stream position the sweeps have
                                         ///< entered into the chains up to.
  struct deflate_opt_node_s * opt_nodes; ///< NULL unless this level uses it.
  size_t opt_node_cap;                   ///< Entries in opt_nodes.
  int use_opt;                           ///< Parse by shortest path.

  /** Non-zero while a flush tail is staged in pending_buf but not delivered. */
  int flush_staged;

  uint8_t * pending_buf;    ///< Output staged by update() before delivery.
  size_t pending_size;      ///< Allocated size of pending_buf.
  size_t pending_used;      ///< Bytes rendered into pending_buf.
  size_t pending_copied;    ///< Bytes of pending_buf already delivered.
} gcomp_deflate_encoder_state_t;

/**
 * @brief Whether deflate_find_match() may compare eight bytes at a time.
 *
 * Requires a little-endian target and __builtin_ctzll, so that the index of
 * the first differing byte in `a ^ b` is `ctz(diff) / 8`.  Everything else
 * falls back to the byte-at-a-time loop, which is what the word loop is
 * checked against.
 *
 * Define GCOMP_DEFLATE_NO_WORD_COMPARE to force the byte path on a platform
 * that would otherwise qualify; the tests build both.
 */
#if !defined(GCOMP_DEFLATE_NO_WORD_COMPARE) &&                                 \
    (defined(__GNUC__) || defined(__clang__)) &&                               \
    defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&             \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define GCOMP_DEFLATE_WORD_COMPARE 1
#endif

//
// Hash function for LZ77
//

static void deflate_opt_decay(gcomp_deflate_encoder_state_t * st);
static void deflate_opt_prime(gcomp_deflate_encoder_state_t * st);

/**
 * @brief Clear what a block accumulated, ready for the next one.
 *
 * Two places finish a block, and both used to clear this by hand.  A third
 * thing to keep in step -- the optimal parse's moving average, which is
 * halved here so that a block weighs against its predecessors -- is two
 * places too many to add it in.
 */
static void deflate_reset_block_stats(gcomp_deflate_encoder_state_t * st) {
  st->sym_buf_used = 0;
  st->block_input_len = 0;
  if (st->lit_freq) {
    memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  }
  if (st->dist_freq) {
    memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));
  }
  if (st->use_opt) {
    deflate_opt_decay(st);
  }
}

static uint32_t deflate_hash_update(uint32_t h, uint8_t b) {
  // Simple multiplicative hash
  return ((h << 5u) ^ (h >> (DEFLATE_HASH_BITS - 5u)) ^ b) & DEFLATE_HASH_MASK;
}

/**
 * @brief Compute hash of 3 bytes from a circular buffer.
 *
 * Handles wrapping around the end of the circular window buffer.
 */
static uint32_t deflate_hash_3bytes_wrap(
    const uint8_t * data, size_t pos, size_t window_mask) {
  uint32_t h = 0;
  h = deflate_hash_update(h, data[pos & window_mask]);
  h = deflate_hash_update(h, data[(pos + 1) & window_mask]);
  h = deflate_hash_update(h, data[(pos + 2) & window_mask]);
  return h;
}

//
// Forward declarations
//

static gcomp_status_t deflate_build_fixed_codes(
    gcomp_deflate_encoder_state_t * st);
/**
 * @brief How many input bytes the buffered symbols stand for.
 *
 * Derived from the symbols rather than tracked alongside them, so it cannot
 * drift out of step with what was actually recorded.
 *
 * @param st Encoder state.
 * @return Total bytes the current block covers.
 */
static size_t deflate_block_input_length(
    const gcomp_deflate_encoder_state_t * st) {
  size_t total = 0;
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    total += (st->dist_buf[i] != 0) ? (size_t)st->lit_buf[i] : 1u;
  }
  return total;
}

/**
 * @brief Where a stored block's bytes are kept, and how far back they go.
 *
 * The ring when there is one, the window otherwise.  Collected in one place so
 * that the writer and the test deciding whether to call it cannot disagree
 * about which buffer is in play.
 */
typedef struct {
  const uint8_t * data;
  size_t size; ///< Capacity, a power of two.
  size_t mask; ///< size - 1.
  size_t pos;  ///< One past the most recent byte.
  size_t fill; ///< Bytes actually held, capped at size.
} deflate_stored_source_t;

static deflate_stored_source_t deflate_stored_source(
    const gcomp_deflate_encoder_state_t * st) {
  deflate_stored_source_t s;
  if (st->stored_buf) {
    s.data = st->stored_buf;
    s.size = st->stored_size;
    s.mask = st->stored_mask;
    s.pos = st->stored_pos;
    s.fill = st->stored_fill;
  }
  else {
    s.data = st->window;
    s.size = st->window_size;
    s.mask = st->window_mask;
    s.pos = st->window_pos;
    s.fill = st->window_fill;
  }
  return s;
}

/**
 * @brief The longest stored block that could be written right now.
 *
 * What the source holds, less the lookahead sitting in front of the encoder's
 * position - those bytes have been read but not yet encoded, so they are not
 * part of any block yet.
 */
static size_t deflate_stored_reachable(
    const gcomp_deflate_encoder_state_t * st) {
  const deflate_stored_source_t s = deflate_stored_source(st);
  size_t held = (s.fill < s.size) ? s.fill : s.size;
  return (held > st->lookahead) ? held - st->lookahead : 0u;
}

/**
 * @brief Write the block as a stored block, taking the bytes from history.
 *
 * RFC 1951 section 3.2.4: three header bits, padding to the next byte
 * boundary, LEN and its complement, then the bytes themselves.  Nothing is
 * compressed, so this is the ceiling on what a block can cost - about five
 * bytes over its own length - and it is the answer whenever the coded forms
 * would cost more.
 *
 * The bytes are the @p data_len ending where the encoder has reached, which is
 * @ref gcomp_deflate_encoder_state_t::lookahead bytes before the end of what
 * has been read.  They come from @ref gcomp_deflate_encoder_state_t::stored_buf
 * when the window is too small to have kept them, and from the window
 * otherwise; deflate_stored_source() decides which, and
 * deflate_stored_reachable() is what the caller checks first.
 *
 * @param st Encoder state.
 * @param final Non-zero if this is the last block in the stream.
 * @param data_len Bytes to store; at most 65535.
 * @return GCOMP_OK, or a bit writer error.
 */
static gcomp_status_t deflate_flush_stored_block_from_window(
    gcomp_deflate_encoder_state_t * st, int final, size_t data_len) {
  gcomp_status_t s = gcomp_deflate_bitwriter_write_bits(
      &st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 2); // BTYPE=00
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    return s;
  }

  uint16_t len = (uint16_t)data_len;
  uint16_t nlen = (uint16_t)(~len);
  const uint16_t header[4] = {(uint16_t)(len & 0xFF),
      (uint16_t)((len >> 8) & 0xFF), (uint16_t)(nlen & 0xFF),
      (uint16_t)((nlen >> 8) & 0xFF)};
  for (size_t i = 0; i < 4; i++) {
    s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, header[i], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  // The block ends at the encoder's position, which is `lookahead` bytes
  // behind where history has been filled to.
  const deflate_stored_source_t src = deflate_stored_source(st);
  size_t end = (src.pos + src.size - st->lookahead) & src.mask;
  size_t start = (end + src.size - data_len) & src.mask;
  for (size_t i = 0; i < data_len; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, src.data[(start + i) & src.mask], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  deflate_reset_block_stats(st);
  return GCOMP_OK;
}

static gcomp_status_t deflate_flush_stored_block(
    gcomp_deflate_encoder_state_t * st, int final);
static gcomp_status_t deflate_flush_fixed_block(
    gcomp_deflate_encoder_state_t * st, int final);
static gcomp_status_t deflate_flush_dynamic_block(
    gcomp_deflate_encoder_state_t * st, int final);

//
// Length/Distance encoding tables (RFC 1951)
//

static const uint16_t k_len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
    19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,
    2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

static const uint16_t k_dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
    49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
    5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/**
 * @brief Find the length code (257..285) for a given match length (3..258).
 */
/**
 * @brief Length and distance code lookups (RFC 1951 section 3.2.5).
 *
 * These were linear scans over k_len_base and k_dist_base, run up to three
 * times per emitted match - once to count symbols and again in whichever
 * block writer runs - and together they were about 8% of the encoder's
 * instruction count.  They are lookups now.
 *
 * @ref k_len_code is indexed by `length - 3`, covering lengths 3..258.
 *
 * Distances need 32768 entries to index directly, so they are split the way
 * zlib splits them: @ref k_dist_code_low covers 1..256 by `distance - 1`,
 * and @ref k_dist_code_high covers 257..32768 by `(distance - 1) >> 7`.  The
 * high half works because every distance code from 257 up spans a whole
 * number of 128-wide buckets.
 *
 * Both tables were generated from the same k_len_base/k_dist_base the scans
 * used.  DeflateEncodeCodeTables in the test suite re-derives the scan for
 * every one of the 256 lengths and 32768 distances and checks it against the
 * table, so the tables cannot drift from the bases they came from.
 */
static const uint8_t k_len_code[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 12, 13,
    13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16,
    16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19,
    19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    28
};

static const uint8_t k_dist_code_low[256] = {
    0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 9,
    9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
};

static const uint8_t k_dist_code_high[256] = {
    0, 14, 16, 17, 18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22,
    22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29
};

uint32_t gcomp_deflate_length_code(uint32_t length) {
  if (length < 3 || length > 258) {
    return 0; // Invalid
  }
  return 257u + k_len_code[length - 3u];
}

/**
 * @brief Find the distance code (0..29) for a given distance (1..32768).
 */
uint32_t gcomp_deflate_distance_code(uint32_t distance) {
  if (distance < 1 || distance > 32768) {
    return 0; // Invalid
  }
  return (distance <= 256u) ? k_dist_code_low[distance - 1u]
                            : k_dist_code_high[(distance - 1u) >> 7u];
}

//
// Fixed Huffman codes (RFC 1951, Section 3.2.6)
//

static uint16_t reverse_code(uint16_t code, uint32_t bits) {
  uint16_t r = 0;
  for (uint32_t i = 0; i < bits; i++) {
    r = (uint16_t)((r << 1) | (code & 1));
    code >>= 1;
  }
  return r;
}

static gcomp_status_t deflate_build_fixed_codes(
    gcomp_deflate_encoder_state_t * st) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  // Build fixed literal/length code lengths per RFC 1951
  uint8_t lit_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  for (int i = 0; i <= 143; i++) {
    lit_lengths[i] = 8;
  }
  for (int i = 144; i <= 255; i++) {
    lit_lengths[i] = 9;
  }
  for (int i = 256; i <= 279; i++) {
    lit_lengths[i] = 7;
  }
  for (int i = 280; i <= 287; i++) {
    lit_lengths[i] = 8;
  }

  // Build canonical codes
  gcomp_status_t status = gcomp_deflate_huffman_build_codes(lit_lengths,
      DEFLATE_MAX_LITLEN_SYMBOLS, 15, st->fixed_lit_codes, st->fixed_lit_lens);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reverse codes for LSB-first output
  for (int i = 0; i < (int)DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
    if (st->fixed_lit_lens[i] > 0) {
      st->fixed_lit_codes[i] =
          reverse_code(st->fixed_lit_codes[i], st->fixed_lit_lens[i]);
    }
  }

  // Build fixed distance codes (all 5-bit codes)
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];
  for (int i = 0; i < (int)DEFLATE_MAX_DIST_SYMBOLS; i++) {
    dist_lengths[i] = 5;
  }

  status = gcomp_deflate_huffman_build_codes(dist_lengths,
      DEFLATE_MAX_DIST_SYMBOLS, 15, st->fixed_dist_codes, st->fixed_dist_lens);
  if (status != GCOMP_OK) {
    return status;
  }

  // Reverse codes for LSB-first output
  for (int i = 0; i < (int)DEFLATE_MAX_DIST_SYMBOLS; i++) {
    if (st->fixed_dist_lens[i] > 0) {
      st->fixed_dist_codes[i] =
          reverse_code(st->fixed_dist_codes[i], st->fixed_dist_lens[i]);
    }
  }

  st->fixed_ready = 1;
  return GCOMP_OK;
}

/*
 * ===========================================================================
 * LZ77 Match Finding with Hash Chain
 * ===========================================================================
 *
 * DEFLATE uses LZ77 compression: replace repeated byte sequences with
 * (length, distance) pairs that reference earlier occurrences.
 *
 * Data Structures
 * ---------------
 * - window[window_size]: Circular buffer holding recent input bytes.
 * - window_pos: Next write position (wraps at window_size).
 * - total_in: Total bytes ever written to window (monotonically increasing).
 * - lookahead: Bytes in window not yet encoded.
 *
 * Hash Chain
 * ----------
 * To find matches efficiently, we maintain a hash chain:
 *
 * - hash_head[HASH_SIZE]: For each hash value, the buffer index of the most
 *   recent position that hashed to that value.
 *
 * - hash_prev[window_size]: Linked list of previous positions with the same
 *   hash. hash_prev[i] points to the previous position that had the same
 *   3-byte hash as position i.
 *
 * - hash_pos[window_size]: Stream position when each buffer index was last
 *   inserted. This is CRITICAL for validity checking (see below).
 *
 * The Circular Buffer Problem
 * ---------------------------
 * The window is circular: after writing to index (window_size-1), we wrap
 * to index 0 and overwrite old data. This creates a subtle bug:
 *
 *   1. At stream position 100, we write byte 'A' to window[100].
 *   2. We insert hash chain entry: hash_head[h] = 100.
 *   3. Later, at stream position 32868 (= 32768 + 100), we write byte 'X'
 *      to window[100] (same index due to wrap).
 *   4. The old hash entry still says "look at index 100" but that now
 *      contains 'X', not 'A'!
 *
 * Solution: Stream Position Tracking
 * ----------------------------------
 * When inserting, we record hash_pos[idx] = stream_pos (the true position).
 * When matching, we check:
 *
 *   stream_dist = current_stream_pos - hash_pos[match_idx]
 *   buf_dist = (current_buf_idx - match_idx + window_size) % window_size
 *
 *   if (stream_dist != buf_dist):
 *       The entry is stale (buffer wrapped) - skip it
 *
 * This ensures we only use matches that are actually valid in the current
 * window contents.
 *
 * ===========================================================================
 */

/**
 * @brief Find the best match for the current position.
 *
 * Walks the hash chain for the current 3-byte sequence, checking each
 * candidate against the actual window contents. Uses stream position
 * tracking to skip stale entries from before the circular buffer wrapped.
 *
 * @param st Encoder state
 * @param pos Position in circular window buffer
 * @param stream_pos Current position in the total input stream
 * @param max_chain Maximum hash chain length to search
 * @param avail Bytes ahead of @p pos that may be matched.  The greedy parse
 *        passes the whole lookahead; a parse that searches ahead of where it
 *        has committed passes what is left from that position.
 * @param out Receives every improving match, in increasing length order, or
 *        NULL for a caller that only wants the longest.
 * @param out_cap Entries @p out can hold.
 * @param found_out Receives how many were written to @p out.
 * @return Match with length >= 3, or length == 0 if no match found
 */
static deflate_match_t deflate_find_match_list(
    gcomp_deflate_encoder_state_t * st, size_t pos, size_t stream_pos,
    int max_chain, size_t avail, deflate_match_t * out, size_t out_cap,
    size_t * found_out) {
  deflate_match_t result = {0, 0};
  size_t found = 0;
  if (found_out) {
    *found_out = 0;
  }

  if (!st || !st->window || avail < DEFLATE_MIN_MATCH_LENGTH) {
    return result;
  }

  size_t scan = pos & st->window_mask;
  const uint8_t * data = st->window;
  size_t max_len = avail;
  if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
    max_len = DEFLATE_MAX_MATCH_LENGTH;
  }

  uint32_t hash = deflate_hash_3bytes_wrap(data, scan, st->window_mask);
  uint16_t cur = st->hash_head[hash];
  int chain_count = 0;

  // The byte one past the end of the best match so far, which any candidate
  // that is going to beat it has to match.  Kept here rather than reloaded per
  // candidate: it only changes when result.length does.
  uint8_t probe_byte = 0;

  // A chain is built by prepending, so following it must walk strictly
  // backwards through the stream.  When it stops doing that the chain has
  // left this hash's history: a window slot is reused every window_size
  // bytes, and a slot reused for a different hash still has the hash_prev
  // link from its previous life, so a walk that reaches it continues into
  // some other hash's older entries.
  //
  // Those entries are not candidates - they were stored because different
  // bytes hashed to a different bucket - so the walk was doing nothing but
  // reading memory at random.  On incompressible data it did that all the way
  // to max_chain on every position: the mean walk at level 9 was 106.9 of a
  // possible 128, where a clean chain over a 32 KB window with 32768 buckets
  // is about one.  Stopping at the break takes it to 2.8, and the encoder's
  // output does not change - on 2 MB of random bytes, 4.7 MB of C source and
  // an XML registry it is byte for byte identical, because a candidate from
  // the wrong bucket had no reason to match anyway.
  size_t chain_bound = stream_pos;
  while (cur != DEFLATE_NIL && chain_count < max_chain) {
    size_t match_idx = cur;

    // Check if this hash entry is still valid (not overwritten in circular
    // buf), and that the chain is still going backwards.
    size_t match_stream_pos = st->hash_pos[match_idx];
    if (match_stream_pos >= chain_bound) {
      break;
    }
    chain_bound = match_stream_pos;

    // Then the cheapest rejection.  Only a candidate that matches at the byte
    // just past the end of the best match so far can beat it, and one load
    // settles that.
    //
    // This cannot change which match is chosen: a candidate it rejects has
    // length <= result.length, and the code below only takes one whose length
    // is strictly greater.  A stale entry that survives the probe is still
    // caught by the checks that follow.
    //
    // result.length < max_len always holds here, because the loop breaks as
    // soon as a match reaches max_len, so `scan + result.length` is still
    // inside the lookahead.
    if (result.length >= DEFLATE_MIN_MATCH_LENGTH &&
        data[(match_idx + result.length) & st->window_mask] != probe_byte) {
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    size_t stream_dist = stream_pos - match_stream_pos;

    // Staleness must be measured against total_in, not stream_pos.  The
    // window is filled ahead of the encoding position, so window[idx] holds
    // the most recent byte written there - the largest position <= total_in-1
    // congruent to idx.  hash_pos[idx] still names the byte that *was* there
    // when the entry was inserted.  The two agree only while
    // total_in - hash_pos[idx] <= window_size; beyond that the refill has
    // already overwritten those bytes with lookahead data.
    //
    // Using stream_pos here left a blind spot exactly `lookahead` bytes wide:
    // entries whose data had been overwritten still passed, and the buf_dist
    // check below could not catch them (buf_dist is stream_dist modulo
    // window_size, so it agrees for any distance under one window).  The
    // encoder then matched against future data and emitted a distance that
    // decodes to the wrong bytes.
    if (stream_dist > DEFLATE_MAX_DISTANCE ||
        st->total_in - match_stream_pos > st->window_size) {
      // Entry has been overwritten or is too far back - skip
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    // Calculate circular buffer distance for byte comparisons
    size_t buf_dist = (scan >= match_idx)
        ? (scan - match_idx)
        : (st->window_size - match_idx + scan);

    // The stream distance should equal the buffer distance for valid matches
    if (buf_dist != stream_dist) {
      cur = st->hash_prev[cur];
      chain_count++;
      continue;
    }

    // Check match length.
    size_t len = 0;

#ifdef GCOMP_DEFLATE_WORD_COMPARE
    // Both cursors run forward from scan and match_idx until one of them
    // reaches the end of the circular window and wraps.  Up to that point the
    // two runs are flat, so eight bytes can be compared at once; the loop
    // below finishes the tail and handles the wrap.
    //
    // The bound keeps the loads in bounds without a separate check:
    // len + 8 <= flat <= window_size - scan means scan + len + 8 <=
    // window_size, and the same for match_idx.  A match is at most 258 bytes
    // (RFC 1951 section 3.2.5) against a window of at least 256, so a wrap is
    // rare and the tail loop is short.
    {
      size_t scan_room = st->window_size - scan;
      size_t match_room = st->window_size - match_idx;
      size_t flat = scan_room < match_room ? scan_room : match_room;
      if (flat > max_len) {
        flat = max_len;
      }
      while (len + 8u <= flat) {
        uint64_t a;
        uint64_t b;
        memcpy(&a, data + scan + len, sizeof(a));
        memcpy(&b, data + match_idx + len, sizeof(b));
        uint64_t diff = a ^ b;
        if (diff) {
          // Little-endian: the lowest set bit sits in the first byte that
          // differs.  Land on it and let the byte loop below stop there.
          len += (size_t)(__builtin_ctzll(diff) / 8u);
          break;
        }
        len += 8u;
      }
    }
#endif

    while (len < max_len &&
        data[(scan + len) & st->window_mask] ==
            data[(match_idx + len) & st->window_mask]) {
      len++;
    }

    if (len >= DEFLATE_MIN_MATCH_LENGTH && len > result.length) {
      result.length = (uint32_t)len;
      result.distance = (uint32_t)stream_dist;
      // Every improvement is a match worth reporting, not only the last.  A
      // parse that prices candidates against one another needs the short and
      // near ones too: a longer match at a greater distance can cost more
      // bits than a shorter one close by, because the distance code carries
      // up to thirteen extra bits (RFC 1951 section 3.2.5).  A caller that
      // wants only the longest passes no list and this folds away.
      if (out && found < out_cap) {
        out[found] = result;
        found++;
      }

      if (len >= max_len) {
        break; // Max length found
      }
      probe_byte = data[(scan + len) & st->window_mask];
    }

    cur = st->hash_prev[cur];
    chain_count++;
  }

  if (found_out) {
    *found_out = found;
  }
  return result;
}

/**
 * @brief The chain walk, reporting only the longest match it met.
 */
static deflate_match_t deflate_find_match(gcomp_deflate_encoder_state_t * st,
    size_t pos, size_t stream_pos, int max_chain) {
  return deflate_find_match_list(
      st, pos, stream_pos, max_chain, st ? st->lookahead : 0u, NULL, 0, NULL);
}

/**
 * @brief Insert a position into the hash chain.
 *
 * Updates the hash chain so that future searches can find this position.
 * Records both the buffer index (in hash_head/hash_prev) and the stream
 * position (in hash_pos) so that stale entries can be detected after the
 * circular buffer wraps.
 *
 * ## Proactive Hash Chain Invalidation
 *
 * This function implements proactive invalidation to prevent hash chain
 * corruption when buffer indices are reused. The problem and solution:
 *
 * **Problem**: When the circular window buffer wraps, a buffer index that
 * was previously part of hash chain A may be reused for data that hashes
 * to chain B. If hash_head[A] still points to this index, searches on
 * chain A will incorrectly follow hash_prev into chain B's history.
 *
 * **Solution**: Before inserting at index `idx` with hash `new_hash`:
 * 1. Look up `old_hash = hash_at[idx]` (the hash from the previous insert)
 * 2. If `hash_head[old_hash] == idx` AND `old_hash != new_hash`:
 *    - This index is still the head of the old chain
 *    - Set `hash_head[old_hash] = hash_prev[idx]`, which unlinks this index
 *      from the old chain and leaves the rest of it intact
 * 3. The condition `old_hash != new_hash` is critical: if we're reinserting
 *    into the same chain, invalidating would corrupt hash_prev linkage.
 *
 * This proactive approach ensures hash chains are always clean, avoiding
 * the need for expensive validation during match searches.
 *
 * @param st Encoder state
 * @param pos Position in circular window buffer
 * @param stream_pos Position in the total input stream (for validity checking)
 */
static void deflate_insert_hash(
    gcomp_deflate_encoder_state_t * st, size_t pos, size_t stream_pos) {
  if (!st || !st->window || st->lookahead < 3) {
    return;
  }

  size_t idx = pos & st->window_mask;
  uint32_t hash = deflate_hash_3bytes_wrap(st->window, idx, st->window_mask);

  // Proactive invalidation: If this buffer index was previously the head of
  // a different hash chain, clear that chain head to prevent corruption.
  // See function documentation above for detailed explanation.
  uint16_t old_hash = st->hash_at[idx];
  if (old_hash != hash && st->hash_head[old_hash] == idx) {
    // Splice this index out of its old chain rather than discarding the
    // chain.  hash_prev[idx] still names the entry that was behind it, and
    // those entries are older positions in the same window - exactly the
    // candidates a search on old_hash wants.  Setting the head to NIL threw
    // all of them away.
    //
    // Measured, this changes nothing: the case fires on 0% to 5% of inserts
    // depending on the data, and the corpus came out byte for byte identical
    // either way.  It is here because keeping the older entries is free and
    // discarding them is not defensible, not because it was costing anything
    // that could be found.
    st->hash_head[old_hash] = st->hash_prev[idx];
  }

  // Standard hash chain insertion: prepend to chain, record metadata
  st->hash_prev[idx] = st->hash_head[hash];
  st->hash_head[hash] = (uint16_t)idx;
  st->hash_pos[idx] = stream_pos;
  st->hash_at[idx] = (uint16_t)hash;
}

/**
 * @brief Lay a preset dictionary down as the history the stream starts from.
 *
 * RFC 1951 has no notion of a dictionary, and that is what makes this simple:
 * the bytes are history.  They go into the window and into the hash chains by
 * the same route input takes, and then the encoder starts on the real input
 * with the window already full.  Nothing is emitted for them.
 *
 * Only the last @c window_size bytes are laid down, because a distance cannot
 * exceed the window and anything further back could never be referenced.  zlib's
 * @c deflateSetDictionary keeps the same tail.
 *
 * Two details that are easy to get wrong:
 *
 * - Only positions with three whole dictionary bytes ahead of them are
 *   indexed.  The hash reads three bytes and wraps at the window's end, so
 *   indexing the last two would hash dictionary bytes together with whatever
 *   the window holds at position zero.
 * - deflate_insert_hash() refuses to insert while the lookahead is under
 *   three bytes, which is right for input and wrong here: the dictionary is
 *   not lookahead, there is simply a lot of it.  It is lent the length for the
 *   duration and given back zero, because none of these bytes is waiting to be
 *   encoded.
 */
static void deflate_prime_dictionary(gcomp_deflate_encoder_state_t * st) {
  if (!st || !st->window || !st->dict_bytes || st->dict_len == 0) {
    return;
  }

  size_t take = st->dict_len;
  if (take > st->window_size) {
    take = st->window_size;
  }
  const uint8_t * tail = st->dict_bytes + (st->dict_len - take);

  const size_t start = st->window_pos;
  for (size_t i = 0; i < take; i++) {
    st->window[st->window_pos] = tail[i];
    st->window_pos = (st->window_pos + 1u) & st->window_mask;
  }
  st->total_in += take;
  st->window_fill += take;
  if (st->window_fill > st->window_size) {
    st->window_fill = st->window_size;
  }

  if (take >= DEFLATE_MIN_MATCH_LENGTH) {
    const size_t saved_lookahead = st->lookahead;
    st->lookahead = take;
    const size_t base_stream = st->total_in - take;
    for (size_t i = 0; i + DEFLATE_MIN_MATCH_LENGTH <= take; i++) {
      deflate_insert_hash(
          st, (start + i) & st->window_mask, base_stream + i);
    }
    st->lookahead = saved_lookahead;
  }
}

//
// Block Flushing
//

static gcomp_status_t deflate_flush_stored_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st) {
    return GCOMP_ERR_INVALID_ARG;
  }

  size_t data_len = st->block_buffer_used;
  if (data_len > DEFLATE_MAX_STORED_BLOCK) {
    data_len = DEFLATE_MAX_STORED_BLOCK;
  }

  gcomp_status_t s;

  // Write block header: BFINAL (1 bit), BTYPE=00 (2 bits)
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 2); // BTYPE=00
  if (s != GCOMP_OK) {
    return s;
  }

  // Align to byte boundary
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    return s;
  }

  // Write LEN and NLEN
  uint16_t len = (uint16_t)data_len;
  uint16_t nlen = (uint16_t)(~len);

  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, len & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (len >> 8) & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, nlen & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (nlen >> 8) & 0xFF, 8);
  if (s != GCOMP_OK) {
    return s;
  }

  // Write data bytes
  for (size_t i = 0; i < data_len; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, st->block_buffer[i], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  // Remove written data from buffer
  if (data_len < st->block_buffer_used) {
    memmove(st->block_buffer, st->block_buffer + data_len,
        st->block_buffer_used - data_len);
  }
  st->block_buffer_used -= data_len;

  return GCOMP_OK;
}

static gcomp_status_t deflate_write_symbol(
    gcomp_deflate_encoder_state_t * st, uint16_t code, uint8_t len) {
  return gcomp_deflate_bitwriter_write_bits(&st->bitwriter, code, len);
}

static gcomp_status_t deflate_flush_fixed_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st || !st->fixed_ready) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t s;

  // Write block header: BFINAL (1 bit), BTYPE=01 (2 bits) = fixed Huffman
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 1u, 2); // BTYPE=01
  if (s != GCOMP_OK) {
    return s;
  }

  // Write all buffered symbols
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    uint16_t lit = st->lit_buf[i];
    uint16_t dist = st->dist_buf[i];

    if (dist == 0) {
      // Literal byte
      s = deflate_write_symbol(
          st, st->fixed_lit_codes[lit], st->fixed_lit_lens[lit]);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    else {
      // Length/distance pair
      // lit contains the length (3..258)
      uint32_t len_code = gcomp_deflate_length_code(lit);
      uint32_t len_sym = len_code - 257;

      s = deflate_write_symbol(
          st, st->fixed_lit_codes[len_code], st->fixed_lit_lens[len_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write length extra bits
      if (k_len_extra[len_sym] > 0) {
        uint32_t extra = lit - k_len_base[len_sym];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_len_extra[len_sym]);
        if (s != GCOMP_OK) {
          return s;
        }
      }

      // Write distance code
      uint32_t dist_code = gcomp_deflate_distance_code(dist);
      s = deflate_write_symbol(
          st, st->fixed_dist_codes[dist_code], st->fixed_dist_lens[dist_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write distance extra bits
      if (k_dist_extra[dist_code] > 0) {
        uint32_t extra = dist - k_dist_base[dist_code];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_dist_extra[dist_code]);
        if (s != GCOMP_OK) {
          return s;
        }
      }
    }
  }

  // Write end-of-block symbol (256)
  s = deflate_write_symbol(
      st, st->fixed_lit_codes[256], st->fixed_lit_lens[256]);
  if (s != GCOMP_OK) {
    return s;
  }

  // Reset for next block
  deflate_reset_block_stats(st);
  return GCOMP_OK;
}

/*
 * ===========================================================================
 * Dynamic Huffman Encoding (RFC 1951 Section 3.2.7)
 * ===========================================================================
 *
 * OVERVIEW
 * --------
 * For compression levels 4-9, we build optimal Huffman codes based on actual
 * symbol frequencies observed in the data. Dynamic Huffman typically achieves
 * better compression than fixed Huffman because the code lengths are tailored
 * to the specific input data's statistical properties.
 *
 * ALGORITHM STEPS
 * ---------------
 * 1. FREQUENCY COLLECTION (during LZ77 matching in encoder_update):
 *    - lit_freq[0..255]: Count of each literal byte
 *    - lit_freq[257..285]: Count of each length code (match lengths 3-258)
 *    - dist_freq[0..29]: Count of each distance code (distances 1-32768)
 *    - lit_freq[256] is incremented for the end-of-block marker
 *
 * 2. CODE LENGTH CONSTRUCTION (build_code_lengths):
 *    - Hand the frequencies to the shared package-merge implementation in
 *      src/core/huffman_lengths.c
 *    - It returns the code lengths that encode these frequencies in the
 *      fewest bits of any code whose longest code word fits the cap - 15 bits
 *      for the literal/length and distance alphabets, 7 for the code-length
 *      alphabet - and that always describe a complete code
 *
 * 3. (no separate limiting step)
 *    - The cap is part of what package-merge solves, so there is no clamp to
 *      repair afterwards.  Deciding the cap and the code separately is what
 *      this encoder used to do, and it cost bits: a clamp cannot see which
 *      lengthening elsewhere is cheapest, so it spent the code space in the
 *      wrong place
 *
 * 4. CODE LENGTH ENCODING (RFC 1951 Section 3.2.7):
 *    - Combine literal/length and distance code lengths into one sequence
 *    - Run-length encode using the code-length alphabet:
 *      * 0-15: Literal code length values
 *      * 16: Copy previous code length 3-6 times (2 extra bits)
 *      * 17: Repeat code length 0 for 3-10 times (3 extra bits)
 *      * 18: Repeat code length 0 for 11-138 times (7 extra bits)
 *    - Build a Huffman tree for the code-length symbols themselves
 *
 * 5. BLOCK HEADER WRITING:
 *    - BFINAL (1 bit): 1 if this is the final block
 *    - BTYPE (2 bits): 10 binary = dynamic Huffman
 *    - HLIT (5 bits): Number of literal/length codes - 257 (range 0-29)
 *    - HDIST (5 bits): Number of distance codes - 1 (range 0-29)
 *    - HCLEN (4 bits): Number of code-length codes - 4 (range 0-15)
 *    - Code-length code lengths (3 bits each, in permuted order k_cl_order)
 *    - Encoded code lengths for literal/length alphabet
 *    - Encoded code lengths for distance alphabet
 *
 * 6. DATA ENCODING:
 *    - Emit each buffered symbol using its dynamic Huffman code
 *    - End with end-of-block symbol (256)
 *
 * DESIGN RATIONALE
 * ----------------
 * - Package-merge is O(n * max_bits) and is exact under the cap; the 15-bit
 *   limit is mandated by RFC 1951 section 3.2.7
 * - Fallback to fixed Huffman occurs on memory allocation failure
 * - Empty distance trees are valid when no LZ77 matches are used (e.g.,
 *   incompressible data or very short inputs)
 *
 * MEMORY LAYOUT
 * -------------
 * - lit_freq: 288 uint32_t entries (allocated for levels > 3)
 * - dist_freq: 32 uint32_t entries (allocated for levels > 3)
 * - Temporary allocations in build_code_lengths: 2 * used * max_bits chain
 *   records, where `used` counts the symbols that actually occur
 *
 * ===========================================================================
 */

/**
 * @brief Build code lengths for an alphabet, none longer than @p max_bits.
 *
 * Thin wrapper over the shared package-merge implementation in
 * src/core/huffman_lengths.c, which is where the algorithm and the reason
 * for it are documented.  The lengths it returns are optimal under the cap
 * and always describe a complete code.
 *
 * @param alloc Allocator for scratch memory.
 * @param freq Frequency of each symbol.
 * @param num_symbols Size of the alphabet.
 * @param lengths Receives one length per symbol.
 * @param max_bits Longest code word allowed: 15 for the literal/length and
 *        distance alphabets, 7 for the code-length alphabet (RFC 1951
 *        sections 3.2.2 and 3.2.7).
 * @return GCOMP_OK, or GCOMP_ERR_MEMORY when scratch space cannot be
 *         allocated.  A caller that cannot proceed without lengths emits a
 *         fixed-Huffman block instead, which needs none.
 */
static gcomp_status_t build_code_lengths(const gcomp_allocator_t * alloc,
    const uint32_t * freq, size_t num_symbols, uint8_t * lengths,
    unsigned max_bits) {
  return gcomp_huffman_code_lengths(
      alloc, freq, num_symbols, max_bits, lengths);
}

/**
 * Code length alphabet transmission order (RFC 1951 Section 3.2.7).
 *
 * The code lengths for the code-length alphabet are transmitted in this
 * permuted order to maximize trailing zeros (which can be omitted via HCLEN).
 * The most commonly used code-length symbols (0, 17, 18 for runs of zeros,
 * and small literal lengths like 1-4) appear at positions that allow HCLEN
 * to exclude the rarely-used symbols at the end.
 */
static const uint8_t k_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/**
 * @brief Run-length encode code lengths using the code-length alphabet.
 *
 * Compresses a sequence of code lengths by encoding runs:
 *   0-15: Literal code length value (no extra bits)
 *   16:   Copy previous code length 3-6 times (2 extra bits: 0-3)
 *   17:   Repeat code length 0 for 3-10 times (3 extra bits: 0-7)
 *   18:   Repeat code length 0 for 11-138 times (7 extra bits: 0-127)
 *
 * @param all_lengths   Array of code lengths to encode
 * @param total_codes   Number of code lengths in the array
 * @param cl_symbols    Output: symbols (0-18) to emit
 * @param cl_extra      Output: extra bits for each symbol
 * @return Number of symbols written to cl_symbols/cl_extra
 */
static size_t deflate_rle_encode_lengths(const uint8_t * all_lengths,
    size_t total_codes, uint8_t * cl_symbols, uint8_t * cl_extra) {
  size_t cl_count = 0;
  size_t i = 0;

  while (i < total_codes) {
    uint8_t len = all_lengths[i];

    // Count consecutive occurrences of this length
    size_t run = 1;
    while (i + run < total_codes && all_lengths[i + run] == len) {
      run++;
    }

    if (len == 0) {
      // Encode run of zeros
      while (run > 0) {
        if (run >= 11) {
          size_t emit = (run > 138) ? 138 : run;
          cl_symbols[cl_count] = 18;
          cl_extra[cl_count] = (uint8_t)(emit - 11);
          cl_count++;
          run -= emit;
          i += emit;
        }
        else if (run >= 3) {
          cl_symbols[cl_count] = 17;
          cl_extra[cl_count] = (uint8_t)(run - 3);
          cl_count++;
          i += run;
          run = 0;
        }
        else {
          cl_symbols[cl_count] = 0;
          cl_extra[cl_count] = 0;
          cl_count++;
          run--;
          i++;
        }
      }
    }
    else {
      // Non-zero length: emit it first
      cl_symbols[cl_count] = len;
      cl_extra[cl_count] = 0;
      cl_count++;
      i++;
      run--;

      // Then encode repeats using symbol 16
      while (run >= 3) {
        size_t emit = (run > 6) ? 6 : run;
        cl_symbols[cl_count] = 16;
        cl_extra[cl_count] = (uint8_t)(emit - 3);
        cl_count++;
        run -= emit;
        i += emit;
      }

      // Remaining repeats (0-2) will be handled in next iteration
    }
  }

  return cl_count;
}

/**
 * @brief Ensure code-length alphabet is complete for zlib compatibility.
 *
 * RFC 1951 allows incomplete Huffman trees (Kraft sum < 2^max_bits), but
 * zlib's inflate_table() function rejects incomplete trees for the CODES
 * type (code-length alphabet). This is stricter than the RFC requires.
 *
 * Background: The Kraft inequality states that for a valid prefix code,
 * sum(2^(-length_i)) <= 1. An "under-subscribed" or "incomplete" tree has
 * sum < 1, meaning some bit patterns don't decode to any symbol.
 *
 * Solution: If the code-length alphabet is under-subscribed, add unused
 * symbols with appropriate code lengths to make the Kraft sum exactly 2^7.
 * We prefer symbols late in k_cl_order[] to minimize HCLEN.
 *
 * @param cl_lengths  Code lengths array (19 elements), modified in place
 */
/**
 * @brief Write all buffered symbols using dynamic Huffman codes.
 *
 * Writes literals and length/distance pairs from the symbol buffer using
 * the provided Huffman codes. Also writes the end-of-block symbol.
 *
 * @param st           Encoder state with symbol buffer and bitwriter
 * @param lit_codes    Huffman codes for literal/length symbols
 * @param lit_lengths  Code lengths for literal/length symbols
 * @param dist_codes   Huffman codes for distance symbols
 * @param dist_lengths Code lengths for distance symbols
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t deflate_write_dynamic_block_data(
    gcomp_deflate_encoder_state_t * st, const uint16_t * lit_codes,
    const uint8_t * lit_lengths, const uint16_t * dist_codes,
    const uint8_t * dist_lengths) {
  gcomp_status_t s;

  // Write all buffered symbols using the dynamic codes
  for (size_t i = 0; i < st->sym_buf_used; i++) {
    uint16_t lit = st->lit_buf[i];
    uint16_t dist = st->dist_buf[i];

    if (dist == 0) {
      // Literal byte
      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, lit_codes[lit], lit_lengths[lit]);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    else {
      // Length/distance pair
      uint32_t len_code = gcomp_deflate_length_code(lit);
      uint32_t len_sym = len_code - 257;

      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, lit_codes[len_code], lit_lengths[len_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write length extra bits
      if (k_len_extra[len_sym] > 0) {
        uint32_t extra = lit - k_len_base[len_sym];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_len_extra[len_sym]);
        if (s != GCOMP_OK) {
          return s;
        }
      }

      // Write distance code
      uint32_t dist_code = gcomp_deflate_distance_code(dist);
      s = gcomp_deflate_bitwriter_write_bits(
          &st->bitwriter, dist_codes[dist_code], dist_lengths[dist_code]);
      if (s != GCOMP_OK) {
        return s;
      }

      // Write distance extra bits
      if (k_dist_extra[dist_code] > 0) {
        uint32_t extra = dist - k_dist_base[dist_code];
        s = gcomp_deflate_bitwriter_write_bits(
            &st->bitwriter, extra, k_dist_extra[dist_code]);
        if (s != GCOMP_OK) {
          return s;
        }
      }
    }
  }

  // Write end-of-block symbol (256)
  s = gcomp_deflate_bitwriter_write_bits(
      &st->bitwriter, lit_codes[256], lit_lengths[256]);
  if (s != GCOMP_OK) {
    return s;
  }

  return GCOMP_OK;
}

static void deflate_ensure_cl_kraft_complete(uint8_t * cl_lengths) {
  // Calculate current Kraft sum (scaled by 2^7 = 128)
  uint32_t kraft = 0;
  for (int i = 0; i < 19; i++) {
    if (cl_lengths[i] > 0) {
      kraft += 1u << (7 - cl_lengths[i]);
    }
  }

  // If under-subscribed (kraft < 128), fill remaining space
  if (kraft < 128) {
    uint32_t remaining = 128 - kraft;

    // Find unused symbols to fill the space
    // Prefer symbols that come late in k_cl_order (to minimize HCLEN)
    // Symbols at positions 15-18 in k_cl_order are: 2, 14, 1, 15
    while (remaining > 0) {
      int best_ord = -1;
      int best_len = 0;
      uint32_t best_contrib = 0;

      // Find the best unused symbol and length that fits
      for (int ord = 18; ord >= 0; ord--) {
        uint8_t sym = k_cl_order[ord];
        if (cl_lengths[sym] == 0) {
          for (int len = 7; len >= 1; len--) {
            uint32_t contribution = 1u << (7 - len);
            if (contribution <= remaining && contribution > best_contrib) {
              best_ord = ord;
              best_len = len;
              best_contrib = contribution;
            }
          }
        }
      }

      if (best_ord < 0) {
        // No suitable unused symbol found - should not happen in practice
        break;
      }

      uint8_t sym = k_cl_order[best_ord];
      cl_lengths[sym] = (uint8_t)best_len;
      remaining -= best_contrib;
    }
  }
}

/**
 * @brief Write a complete dynamic Huffman block.
 *
 * This function performs the following steps:
 * 1. Builds optimal code lengths from the frequency histograms
 * 2. Calculates HLIT, HDIST (number of codes to transmit)
 * 3. Run-length encodes the code lengths using symbols 16, 17, 18
 * 4. Builds a Huffman tree for the code-length alphabet
 * 5. Writes the complete block header (BFINAL, BTYPE, HLIT, HDIST, HCLEN)
 * 6. Writes the code-length code lengths in permuted order
 * 7. Writes the encoded literal/length and distance code lengths
 * 8. Writes all buffered symbols using the dynamic Huffman codes
 * 9. Writes the end-of-block symbol (256)
 *
 * Falls back to fixed Huffman on memory allocation failure or if no
 * frequency data is available.
 *
 * @param st    Encoder state with frequency histograms and symbol buffer
 * @param final Non-zero if this is the final block (sets BFINAL bit)
 * @return GCOMP_OK on success, error code on failure
 */
static gcomp_status_t deflate_flush_dynamic_block(
    gcomp_deflate_encoder_state_t * st, int final) {
  if (!st || !st->lit_freq) {
    // No frequency histograms means no code to build from, which only
    // happens if they could not be allocated.
    gcomp_stepdown_note(st ? &st->stepdowns : NULL, GCOMP_STEPDOWN_NO_MEMORY);
    return deflate_flush_fixed_block(st, final);
  }

  gcomp_status_t s;

  // Ensure end-of-block symbol is counted
  st->lit_freq[256]++;

  // Build code lengths for literal/length alphabet
  uint8_t lit_lengths[DEFLATE_MAX_LITLEN_SYMBOLS];
  s = build_code_lengths(
      st->allocator, st->lit_freq, DEFLATE_MAX_LITLEN_SYMBOLS, lit_lengths, 15);
  if (s != GCOMP_OK) {
    // Without lengths there is no dynamic block to write.  The fixed code is
    // defined by RFC 1951 section 3.2.6 and needs no scratch memory, so it
    // remains available when this does not.
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Ensure end-of-block (256) has a code
  if (lit_lengths[256] == 0) {
    lit_lengths[256] = 1;
  }

  // Build code lengths for distance alphabet
  uint8_t dist_lengths[DEFLATE_MAX_DIST_SYMBOLS];
  s = build_code_lengths(
      st->allocator, st->dist_freq, DEFLATE_MAX_DIST_SYMBOLS, dist_lengths, 15);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Determine HLIT (number of literal/length codes - 257)
  int hlit = DEFLATE_MAX_LITLEN_SYMBOLS - 257;
  while (hlit > 0 && lit_lengths[256 + hlit] == 0) {
    hlit--;
  }

  // Determine HDIST (number of distance codes - 1)
  int hdist = DEFLATE_MAX_DIST_SYMBOLS - 1;
  while (hdist > 0 && dist_lengths[hdist] == 0) {
    hdist--;
  }

  // A block that uses no distances at all still has to declare a distance
  // code, and RFC 1951 section 3.2.7 names the case: "If only one distance
  // code is used, it is encoded using one bit ... Note that in this case
  // there is an incomplete Huffman tree with only one code."
  //
  // The condition has to be "no distance code is used", not "distance code 0
  // is not used".  Testing the latter - which is what this did - gave a
  // length-1 code to distance code 0 on top of a complete code for the
  // distances the block does use, and a complete code plus another one-bit
  // code is over-subscribed.  gcomp_deflate_huffman_build_codes() then
  // refused it and this function fell back to a fixed-Huffman block, quietly,
  // for the whole block.
  //
  // Distance code 0 is a match at distance 1, so run-heavy data has it and
  // structured text often does not.  On an XML registry every block took the
  // fallback: 135,514 bytes where the dynamic code its own frequencies called
  // for would have cost 113,265, against zlib's 112,703.  The parse was
  // already as good as zlib's - 30,519 matches averaging 39.8 bytes against
  // 30,320 averaging 40.1 - and all of the difference was this.
  int any_distance_used = 0;
  for (size_t j = 0; j < DEFLATE_MAX_DIST_SYMBOLS; j++) {
    if (dist_lengths[j] > 0) {
      any_distance_used = 1;
      break;
    }
  }
  if (!any_distance_used) {
    dist_lengths[0] = 1;
  }

  // Combine lit/dist lengths for encoding
  size_t total_codes = (size_t)(257 + hlit + 1 + hdist);
  uint8_t * all_lengths = (uint8_t *)gcomp_malloc(st->allocator, total_codes);
  if (!all_lengths) {
    // Fall back to fixed on memory error
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  memcpy(all_lengths, lit_lengths, 257 + hlit);
  memcpy(all_lengths + 257 + hlit, dist_lengths, 1 + hdist);

  // Run-length encode the code lengths
  uint8_t
      cl_symbols[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
  uint8_t cl_extra[DEFLATE_MAX_LITLEN_SYMBOLS + DEFLATE_MAX_DIST_SYMBOLS + 32];
  size_t cl_count = deflate_rle_encode_lengths(
      all_lengths, total_codes, cl_symbols, cl_extra);

  // Build code length Huffman tree
  uint32_t cl_freq[19] = {0};
  for (size_t i = 0; i < cl_count; i++) {
    cl_freq[cl_symbols[i]]++;
  }

  uint8_t cl_lengths[19];
  s = build_code_lengths(st->allocator, cl_freq, 19, cl_lengths, 7);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_NO_MEMORY);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Ensure code-length alphabet is complete for zlib compatibility
  deflate_ensure_cl_kraft_complete(cl_lengths);

  // Determine HCLEN (number of code length codes - 4)
  int hclen = 19 - 4;
  while (hclen > 0 && cl_lengths[k_cl_order[hclen + 3]] == 0) {
    hclen--;
  }

  // Price this block both ways and take the cheaper.
  //
  // RFC 1951 section 3.2.6 defines a fixed code that costs no header at all;
  // section 3.2.7's dynamic code costs one but fits the block's own symbol
  // frequencies.  Which wins depends on the block, so the block decides.  A
  // level cannot: a short block, or one whose symbols are close to uniform,
  // pays more for the table than the table saves.
  //
  // Extra bits for length and distance codes are identical under both
  // codings, so they are left out of both sides rather than counted twice.
  {
    static const uint8_t k_cl_extra_bits[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 2, 3, 7};

    uint64_t dynamic_bits = 3u + 5u + 5u + 4u + 3u * (uint64_t)(hclen + 4);
    for (size_t i = 0; i < cl_count; i++) {
      dynamic_bits += cl_lengths[cl_symbols[i]] + k_cl_extra_bits[cl_symbols[i]];
    }
    for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
      dynamic_bits += (uint64_t)st->lit_freq[i] * lit_lengths[i];
    }
    for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
      dynamic_bits += (uint64_t)st->dist_freq[i] * dist_lengths[i];
    }

    // The fixed code of RFC 1951 section 3.2.6: literals 0-143 and 280-287
    // are eight bits, 144-255 are nine, 256-279 are seven, and every distance
    // code is five.
    uint64_t fixed_bits = 3u;
    for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
      unsigned len = (i < 144u) ? 8u : (i < 256u) ? 9u : (i < 280u) ? 7u : 8u;
      fixed_bits += (uint64_t)st->lit_freq[i] * len;
    }
    for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
      fixed_bits += (uint64_t)st->dist_freq[i] * 5u;
    }

    // And against storing the block uncompressed.  RFC 1951 section 3.2.4
    // costs three header bits, up to seven more to reach a byte boundary,
    // four bytes of LEN and its complement, and then the bytes themselves.
    // That is the ceiling on what a block can cost, and without it an encoder
    // can expand its input: level 1 used to make a JPEG 1.6% larger, and
    // nothing prevented it in general.
    //
    // The bytes come from history, so they have to still be in it, and a
    // stored block's length field is sixteen bits.  Neither bound binds in the
    // case that matters - incompressible data is nearly all literals, so the
    // block covers about one byte per symbol - but a block of long matches can
    // exceed both, and then there is nothing to store from.
    uint64_t stored_bits = 0;
    size_t block_input = deflate_block_input_length(st);
    const size_t reachable = deflate_stored_reachable(st);
    if (block_input > 0 && block_input <= 65535u &&
        block_input <= reachable) {
      stored_bits = 3u + 7u + 32u + 8u * (uint64_t)block_input;
    }

    if (stored_bits > 0 && stored_bits < dynamic_bits &&
        (!st->fixed_ready || stored_bits < fixed_bits)) {
      gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_STORED_IS_SMALLER);
      gcomp_free(st->allocator, all_lengths);
      return deflate_flush_stored_block_from_window(st, final, block_input);
    }

    if (fixed_bits <= dynamic_bits && st->fixed_ready) {
      gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_FIXED_IS_SMALLER);
      gcomp_free(st->allocator, all_lengths);
      return deflate_flush_fixed_block(st, final);
    }
  }

  // Build canonical codes for code lengths
  uint16_t cl_codes[19];
  s = gcomp_deflate_huffman_build_codes(cl_lengths, 19, 7, cl_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }

  // Reverse codes for LSB-first output
  for (int j = 0; j < 19; j++) {
    if (cl_lengths[j] > 0) {
      cl_codes[j] = reverse_code(cl_codes[j], cl_lengths[j]);
    }
  }

  // Build canonical codes for lit/len and dist
  uint16_t lit_codes[DEFLATE_MAX_LITLEN_SYMBOLS];
  s = gcomp_deflate_huffman_build_codes(
      lit_lengths, DEFLATE_MAX_LITLEN_SYMBOLS, 15, lit_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }
  for (size_t j = 0; j < DEFLATE_MAX_LITLEN_SYMBOLS; j++) {
    if (lit_lengths[j] > 0) {
      lit_codes[j] = reverse_code(lit_codes[j], lit_lengths[j]);
    }
  }

  uint16_t dist_codes[DEFLATE_MAX_DIST_SYMBOLS];
  s = gcomp_deflate_huffman_build_codes(
      dist_lengths, DEFLATE_MAX_DIST_SYMBOLS, 15, dist_codes, NULL);
  if (s != GCOMP_OK) {
    gcomp_stepdown_note(&st->stepdowns, GCOMP_STEPDOWN_CODE_REJECTED);
    gcomp_free(st->allocator, all_lengths);
    st->lit_freq[256]--;
    return deflate_flush_fixed_block(st, final);
  }
  for (size_t j = 0; j < DEFLATE_MAX_DIST_SYMBOLS; j++) {
    if (dist_lengths[j] > 0) {
      dist_codes[j] = reverse_code(dist_codes[j], dist_lengths[j]);
    }
  }

  // Write block header: BFINAL (1 bit), BTYPE=10 (2 bits) = dynamic Huffman
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, final ? 1u : 0u, 1);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 2u, 2); // BTYPE=10
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }

  // Write HLIT (5 bits), HDIST (5 bits), HCLEN (4 bits)
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hlit, 5);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hdist, 5);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, (uint32_t)hclen, 4);
  if (s != GCOMP_OK) {
    gcomp_free(st->allocator, all_lengths);
    return s;
  }

  // Write code length code lengths (3 bits each, in permuted order)
  for (int i = 0; i < hclen + 4; i++) {
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_lengths[k_cl_order[i]], 3);
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }
  }

  // Write the code lengths for lit/len and dist alphabets
  for (size_t i = 0; i < cl_count; i++) {
    uint8_t sym = cl_symbols[i];
    s = gcomp_deflate_bitwriter_write_bits(
        &st->bitwriter, cl_codes[sym], cl_lengths[sym]);
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }

    // Write extra bits for run-length symbols
    if (sym == 16) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 2);
    }
    else if (sym == 17) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 3);
    }
    else if (sym == 18) {
      s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, cl_extra[i], 7);
    }
    if (s != GCOMP_OK) {
      gcomp_free(st->allocator, all_lengths);
      return s;
    }
  }

  gcomp_free(st->allocator, all_lengths);

  // Write all buffered symbols and end-of-block
  s = deflate_write_dynamic_block_data(
      st, lit_codes, lit_lengths, dist_codes, dist_lengths);
  if (s != GCOMP_OK) {
    return s;
  }

  // Reset for next block
  st->sym_buf_used = 0;
  st->block_input_len = 0;
  memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));

  return GCOMP_OK;
}

//
// Public API
//

const gcomp_stepdown_tally_t * gcomp_deflate_encoder_stepdowns(
    const gcomp_encoder_t * encoder) {
  if (!encoder || !encoder->method_state) {
    return NULL;
  }
  const gcomp_deflate_encoder_state_t * st =
      (const gcomp_deflate_encoder_state_t *)encoder->method_state;
  return &st->stepdowns;
}

gcomp_status_t gcomp_deflate_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder) {
  if (!registry || !encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_status_t status = GCOMP_OK;
  const gcomp_allocator_t * alloc = gcomp_registry_get_allocator(registry);

  // Read max memory limit early so we can check it during allocation
  uint64_t max_mem =
      gcomp_limits_read_memory_max(options, GCOMP_DEFAULT_MAX_MEMORY_BYTES);

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)gcomp_calloc(
          alloc, 1, sizeof(gcomp_deflate_encoder_state_t));
  if (!st) {
    return GCOMP_ERR_MEMORY;
  }

  // Store allocator for internal use
  st->allocator = alloc;

  // Initialize memory tracker and track state struct allocation
  st->mem_tracker.current_bytes = 0;
  gcomp_memory_track_alloc(
      &st->mem_tracker, sizeof(gcomp_deflate_encoder_state_t));
  st->max_memory_bytes = max_mem;

  // Read compression level
  st->level = 6; // Default
  if (options) {
    int64_t v = 0;
    if (gcomp_options_get_int64(options, "deflate.level", &v) == GCOMP_OK) {
      if (v >= 0 && v <= 9) {
        st->level = (int)v;
      }
    }
  }

  // A preset dictionary is history the stream starts from.  Copied because the
  // options object need not outlive the encoder, and because a reset has to be
  // able to lay the same history down again.
  st->dict_bytes = NULL;
  st->dict_len = 0;
  if (options) {
    const void * dict = NULL;
    size_t dict_len = 0;
    if (gcomp_options_get_bytes(options, "deflate.dictionary", &dict,
            &dict_len) == GCOMP_OK &&
        dict && dict_len > 0) {
      st->dict_bytes = (uint8_t *)gcomp_malloc(alloc, dict_len);
      if (!st->dict_bytes) {
        status = GCOMP_ERR_MEMORY;
        goto cleanup;
      }
      memcpy(st->dict_bytes, dict, dict_len);
      st->dict_len = dict_len;
      gcomp_memory_track_alloc(&st->mem_tracker, dict_len);
    }
  }

  // Read window bits
  st->window_bits = DEFLATE_WINDOW_BITS_DEFAULT;
  if (options) {
    uint64_t v = 0;
    if (gcomp_options_get_uint64(options, "deflate.window_bits", &v) ==
        GCOMP_OK) {
      if (v >= DEFLATE_WINDOW_BITS_MIN && v <= DEFLATE_WINDOW_BITS_MAX) {
        st->window_bits = (size_t)v;
      }
    }
  }

  // Read compression strategy
  st->strategy = DEFLATE_STRATEGY_DEFAULT;
  if (options) {
    const char * strategy_str = NULL;
    if (gcomp_options_get_string(options, "deflate.strategy", &strategy_str) ==
            GCOMP_OK &&
        strategy_str != NULL) {
      if (strcmp(strategy_str, "default") == 0) {
        st->strategy = DEFLATE_STRATEGY_DEFAULT;
      }
      else if (strcmp(strategy_str, "lazy") == 0) {
        st->strategy = DEFLATE_STRATEGY_LAZY;
      }
      else if (strcmp(strategy_str, "huffman_only") == 0) {
        st->strategy = DEFLATE_STRATEGY_HUFFMAN_ONLY;
      }
      else if (strcmp(strategy_str, "rle") == 0) {
        st->strategy = DEFLATE_STRATEGY_RLE;
      }
      else if (strcmp(strategy_str, "fixed") == 0) {
        st->strategy = DEFLATE_STRATEGY_FIXED;
      }
      else {
        // A name this encoder does not know is a mistake worth hearing
        // about.  Falling back to the default silently, which is what this
        // did, turns a typo -- or a name that has been renamed out from
        // under the caller -- into output that is merely different, and
        // nothing says which strategy actually ran.
        gcomp_free(alloc, st);
        return GCOMP_ERR_INVALID_ARG;
      }
    }
  }

  st->window_size = (size_t)1u << st->window_bits;
  st->window_mask = st->window_size - 1u;
  st->stage = DEFLATE_ENC_STAGE_INIT;
  st->final_block_written = 0;

  // Allocate sliding window
  st->window = (uint8_t *)gcomp_malloc(alloc, st->window_size);
  if (!st->window) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, st->window_size);

  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;

  // Allocate hash tables for LZ77
  size_t hash_head_size = DEFLATE_HASH_SIZE * sizeof(uint16_t);
  size_t hash_prev_size = st->window_size * sizeof(uint16_t);
  size_t hash_pos_size = st->window_size * sizeof(size_t);
  size_t hash_at_size = st->window_size * sizeof(uint16_t);

  st->hash_head =
      (uint16_t *)gcomp_calloc(alloc, DEFLATE_HASH_SIZE, sizeof(uint16_t));
  st->hash_prev =
      (uint16_t *)gcomp_calloc(alloc, st->window_size, sizeof(uint16_t));
  st->hash_pos = (size_t *)gcomp_calloc(alloc, st->window_size, sizeof(size_t));
  st->hash_at =
      (uint16_t *)gcomp_calloc(alloc, st->window_size, sizeof(uint16_t));
  if (!st->hash_head || !st->hash_prev || !st->hash_pos || !st->hash_at) {
    status = GCOMP_ERR_MEMORY;
    goto cleanup;
  }
  gcomp_memory_track_alloc(&st->mem_tracker, hash_head_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_prev_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_pos_size);
  gcomp_memory_track_alloc(&st->mem_tracker, hash_at_size);

  st->total_in = 0;

  // The window and the hash chains both exist now, which is everything the
  // dictionary needs to be laid into.
  deflate_prime_dictionary(st);

  // A window smaller than the ring cannot hold a block and its lookahead at
  // once, so the bytes a stored block is written from are kept separately.  At
  // window_bits 13 and above the window is already at least this large and
  // serves directly, so nothing is allocated and nothing is copied.
  if (st->window_size < DEFLATE_STORED_RING_SIZE) {
    st->stored_size = DEFLATE_STORED_RING_SIZE;
    st->stored_mask = st->stored_size - 1u;
    st->stored_pos = 0;
    st->stored_fill = 0;
    st->stored_buf = (uint8_t *)gcomp_malloc(alloc, st->stored_size);
    if (!st->stored_buf) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, st->stored_size);
  }

  // For level 0, allocate block buffer
  if (st->level == 0) {
    st->block_buffer_size = DEFLATE_MAX_STORED_BLOCK;
    st->block_buffer = (uint8_t *)gcomp_malloc(alloc, st->block_buffer_size);
    if (!st->block_buffer) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, st->block_buffer_size);
    st->block_buffer_used = 0;
  }

  // For levels > 0, allocate symbol buffers
  if (st->level > 0) {
    // A window's worth of literals, but never fewer than DEFLATE_SYM_BUF_MIN:
    // the length of a block and the reach of the match finder are unrelated,
    // and tying them together made a small window pay RFC 1951 section 3.2.4's
    // five bytes of framing on a very short block.
    st->sym_buf_size = st->window_size < DEFLATE_SYM_BUF_MIN
        ? (size_t)DEFLATE_SYM_BUF_MIN
        : st->window_size;
    size_t sym_buf_bytes = st->sym_buf_size * sizeof(uint16_t);

    st->lit_buf = (uint16_t *)gcomp_malloc(alloc, sym_buf_bytes);
    st->dist_buf = (uint16_t *)gcomp_malloc(alloc, sym_buf_bytes);
    if (!st->lit_buf || !st->dist_buf) {
      status = GCOMP_ERR_MEMORY;
      goto cleanup;
    }
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // lit_buf
    gcomp_memory_track_alloc(&st->mem_tracker, sym_buf_bytes); // dist_buf
    st->sym_buf_used = 0;

    // Frequency histograms for dynamic Huffman.  These used to be allocated
    // only above level 3, which is what made the fast levels emit fixed
    // Huffman blocks: deflate_flush_dynamic_block() falls back to fixed when
    // there are no frequencies to build a code from.  Counting symbols costs
    // an increment each and building the code costs one package-merge per
    // block, and on a 12 MB corpus level 1 came out 12.8% smaller for 5% of
    // the encode throughput.  There is no level at which that is a bad trade.
    {
      size_t lit_freq_size = DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t);
      size_t dist_freq_size = DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t);

      st->lit_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_LITLEN_SYMBOLS, sizeof(uint32_t));
      st->dist_freq = (uint32_t *)gcomp_calloc(
          alloc, DEFLATE_MAX_DIST_SYMBOLS, sizeof(uint32_t));
      if (!st->lit_freq || !st->dist_freq) {
        status = GCOMP_ERR_MEMORY;
        goto cleanup;
      }
      gcomp_memory_track_alloc(&st->mem_tracker, lit_freq_size);
      gcomp_memory_track_alloc(&st->mem_tracker, dist_freq_size);
    }

    const deflate_effort_t * effort =
        &k_deflate_effort[(st->level >= 1 && st->level <= 9) ? st->level : 6];

    // The optimal parse's table, for the levels that use it.  One entry per
    // position of a sweep, plus the one past its end: a match that would
    // reach beyond the sweep ends it instead of being relaxed, so nothing is
    // written past this.
    //
    // HUFFMAN_ONLY emits no matches and RLE looks only at distance 1, so
    // neither has a parse for this to replace.  The other three do: LAZY is
    // DEFAULT plus deferral at levels 1 to 3 and FIXED is DEFAULT with the
    // coding forced, and both are meant to be DEFAULT everywhere else --
    // which they only stay by taking this too.
    st->use_opt = effort->use_opt &&
        (st->strategy == DEFLATE_STRATEGY_DEFAULT ||
            st->strategy == DEFLATE_STRATEGY_LAZY ||
            st->strategy == DEFLATE_STRATEGY_FIXED);
    if (st->use_opt) {
      st->opt_budget = effort->opt_budget;
      st->opt_node_cap = deflate_opt_lookahead(st->window_size) + 1u;
      size_t opt_bytes = st->opt_node_cap * sizeof(deflate_opt_node_t);
      st->opt_nodes = (deflate_opt_node_t *)gcomp_malloc(alloc, opt_bytes);
      if (!st->opt_nodes) {
        status = GCOMP_ERR_MEMORY;
        goto cleanup;
      }
      gcomp_memory_track_alloc(&st->mem_tracker, opt_bytes);
      deflate_opt_prime(st);
    }

    // Build fixed Huffman codes
    status = deflate_build_fixed_codes(st);
    if (status != GCOMP_OK) {
      goto cleanup;
    }
  }

  // Check memory limit after all allocations
  status = gcomp_memory_check_limit(&st->mem_tracker, st->max_memory_bytes);
  if (status != GCOMP_OK) {
    goto cleanup;
  }

  // Success path
  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;
  encoder->method_state = st;
  encoder->update_fn = gcomp_deflate_encoder_update;
  encoder->finish_fn = gcomp_deflate_encoder_finish;
  encoder->flush_fn = gcomp_deflate_encoder_flush;
  encoder->reset_fn = gcomp_deflate_encoder_reset;
  return GCOMP_OK;

cleanup:
  // Clean up all allocations on error (gcomp_free handles NULL safely)
  gcomp_free(alloc, st->opt_nodes);
  gcomp_free(alloc, st->dist_freq);
  gcomp_free(alloc, st->lit_freq);
  gcomp_free(alloc, st->dist_buf);
  gcomp_free(alloc, st->lit_buf);
  gcomp_free(alloc, st->block_buffer);
  gcomp_free(alloc, st->stored_buf);
  gcomp_free(alloc, st->dict_bytes);
  gcomp_free(alloc, st->hash_at);
  gcomp_free(alloc, st->hash_pos);
  gcomp_free(alloc, st->hash_prev);
  gcomp_free(alloc, st->hash_head);
  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  return status;
}

void gcomp_deflate_encoder_destroy(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  gcomp_free(alloc, st->pending_buf);
  gcomp_free(alloc, st->finish_buf);
  gcomp_free(alloc, st->opt_nodes);
  gcomp_free(alloc, st->dist_freq);
  gcomp_free(alloc, st->lit_freq);
  gcomp_free(alloc, st->dist_buf);
  gcomp_free(alloc, st->lit_buf);
  gcomp_free(alloc, st->block_buffer);
  gcomp_free(alloc, st->stored_buf);
  gcomp_free(alloc, st->dict_bytes);
  gcomp_free(alloc, st->hash_at);
  gcomp_free(alloc, st->hash_pos);
  gcomp_free(alloc, st->hash_prev);
  gcomp_free(alloc, st->hash_head);
  gcomp_free(alloc, st->window);
  gcomp_free(alloc, st);
  encoder->method_state = NULL;
}

gcomp_status_t gcomp_deflate_encoder_reset(gcomp_encoder_t * encoder) {
  if (!encoder) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // Reset state machine
  st->stage = DEFLATE_ENC_STAGE_ACCEPTING;
  st->final_block_written = 0;

  // Reset sliding window state (keep buffer allocated)
  st->window_pos = 0;
  st->window_fill = 0;
  st->lookahead = 0;
  st->total_in = 0;
  st->lazy_length = 0;
  st->lazy_distance = 0;

  // Reset hash tables (clear to zeros)
  // A reset starts a new stream, so the record of what the previous one had
  // to settle for does not carry into it.
  memset(&st->stepdowns, 0, sizeof(st->stepdowns));
  st->block_input_len = 0;

  memset(st->hash_head, 0, DEFLATE_HASH_SIZE * sizeof(uint16_t));
  memset(st->hash_prev, 0, st->window_size * sizeof(uint16_t));
  memset(st->hash_pos, 0, st->window_size * sizeof(size_t));
  memset(st->hash_at, 0, st->window_size * sizeof(uint16_t));
  st->hash_value = 0;

  // A reset starts a new stream, and a new stream starts from the same history
  // this one did.  After the tables are cleared, not before.  A *flush* is a
  // different thing: deflate_drop_history() throws history away on purpose and
  // the decoder does the same, so nothing is laid back down there.
  deflate_prime_dictionary(st);

  // Reset bitwriter state
  gcomp_deflate_bitwriter_reset(&st->bitwriter);
  st->flush_staged = 0;

  // Reset block buffer (level 0)
  if (st->block_buffer) {
    st->block_buffer_used = 0;
  }

  // A new stream carries no history a block could be stored from.  Unlike the
  // window, this is not primed from the dictionary: a stored block covers
  // stream bytes only.
  st->stored_pos = 0;
  st->stored_fill = 0;

  // Reset symbol buffers (levels > 0)
  if (st->lit_buf) {
    st->sym_buf_used = 0;
  }

  // Reset frequency histograms (levels > 3)
  if (st->lit_freq) {
    memset(st->lit_freq, 0, DEFLATE_MAX_LITLEN_SYMBOLS * sizeof(uint32_t));
  }
  if (st->dist_freq) {
    memset(st->dist_freq, 0, DEFLATE_MAX_DIST_SYMBOLS * sizeof(uint32_t));
  }

  // A reset starts a new stream, so what the optimal parse learned about the
  // last one goes back to the format's own default coding.
  if (st->use_opt) {
    deflate_opt_prime(st);
    st->opt_inserted_to = 0;
  }

  // Free and reset finish buffer (if any partial finish was in progress)
  if (st->finish_buf) {
    gcomp_free(alloc, st->finish_buf);
    st->finish_buf = NULL;
  }
  st->finish_buf_size = 0;
  st->finish_buf_used = 0;
  st->finish_buf_copied = 0;
  st->finish_buf_ready = 0;

  // Discard anything update() had staged but not yet delivered.
  if (st->pending_buf) {
    gcomp_free(alloc, st->pending_buf);
    st->pending_buf = NULL;
  }
  st->pending_size = 0;
  st->pending_used = 0;
  st->pending_copied = 0;

  // Note: We don't reset fixed_ready because the fixed Huffman tables don't
  // need to be rebuilt - they're static and can be reused.

  return GCOMP_OK;
}

/**
 * @brief Worst-case byte size of a single flushed block.
 *
 * A Huffman block holds at most sym_buf_size symbols. Four bytes per symbol
 * is the same conservative figure deflate_estimate_finish_size() uses (the
 * true worst case is a length/distance pair at about 43 bits), plus the
 * dynamic Huffman tree, the block header and end-of-block marker, byte
 * alignment and a margin. A stored block is bounded separately by
 * DEFLATE_MAX_STORED_BLOCK plus its header.
 *
 * pending_buf is sized to this, so a flush into an empty pending_buf always
 * fits and update() can always make progress.
 *
 * @param st Encoder state.
 * @return Upper bound, in bytes, on one flushed block.
 */
static size_t deflate_max_block_bytes(
    const gcomp_deflate_encoder_state_t * st) {
  size_t huffman = (st->sym_buf_size * 4u) + 512u + 8u + 1u + 64u;
  size_t stored = (size_t)DEFLATE_MAX_STORED_BLOCK + 16u;
  return huffman > stored ? huffman : stored;
}

/**
 * @brief Copy staged output to the caller, as far as it will fit.
 *
 * @param st Encoder state.
 * @param output The caller's output buffer.
 * @return Non-zero if everything staged has been delivered.
 */
static int deflate_drain_pending(
    gcomp_deflate_encoder_state_t * st, gcomp_buffer_t * output) {
  size_t avail = st->pending_used - st->pending_copied;
  if (avail) {
    size_t space = output->size - output->used;
    size_t n = (avail < space) ? avail : space;
    if (n) {
      memcpy((uint8_t *)output->data + output->used,
          st->pending_buf + st->pending_copied, n);
      output->used += n;
      st->pending_copied += n;
    }
  }
  if (st->pending_copied >= st->pending_used) {
    st->pending_used = 0;
    st->pending_copied = 0;
    return 1;
  }
  return 0;
}

/**
 * @brief Encode one batch of input into the staging buffer.
 *
 * Consumes as much of @p input as will fit in pending_buf, stopping before any
 * flush that could overflow it. pending_buf holds at least one worst-case
 * block and is empty on entry, so a batch always makes progress.
 *
 * @param st Encoder state.
 * @param input The caller's input buffer; used is advanced by what was taken.
 * @return Status code.
 */

/**
 * @brief Start the cost model from the fixed Huffman code.
 *
 * RFC 1951 section 3.2.6 fixes those code lengths, and a count of 2^(15-len)
 * prices a symbol at exactly the length the fixed code gives it.  So before
 * anything has been measured, the parse prices a block as the format's own
 * default coding would.
 */
static void deflate_opt_prime(gcomp_deflate_encoder_state_t * st) {
  for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
    unsigned len = (i < 144u) ? 8u : (i < 256u) ? 9u : (i < 280u) ? 7u : 8u;
    st->opt_lit_freq[i] = 1u << (15u - len);
  }
  for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
    st->opt_dist_freq[i] = 1u << (15u - 5u);
  }
}

/**
 * @brief Halve every count, so a block weighs against its predecessors.
 *
 * No count reaches zero: a symbol that has not come up is unseen, not
 * impossible, and a count of zero would price it at infinity -- a symbol the
 * parse would refuse to emit however much it saved.
 */
static void deflate_opt_decay(gcomp_deflate_encoder_state_t * st) {
  for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
    uint32_t v = st->opt_lit_freq[i] >> 1;
    st->opt_lit_freq[i] = v ? v : 1u;
  }
  for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
    uint32_t v = st->opt_dist_freq[i] >> 1;
    st->opt_dist_freq[i] = v ? v : 1u;
  }
}

/**
 * @brief Rebuild the prices from the counts.
 *
 * A Huffman code is at least one bit long however common its symbol, so no
 * symbol is priced below one bit; RFC 1951 section 3.2.7 caps them at
 * fifteen, so none is priced above that either.  Pricing outside that range
 * would be promising something the format cannot deliver.
 */
static void deflate_opt_rebuild_prices(gcomp_deflate_encoder_state_t * st) {
  gcomp_bitcost_from_freq(st->opt_lit_freq, st->opt_lit_price,
      DEFLATE_MAX_LITLEN_SYMBOLS, DEFLATE_OPT_PRICE_ONE);
  gcomp_bitcost_from_freq(st->opt_dist_freq, st->opt_dist_price,
      DEFLATE_MAX_DIST_SYMBOLS, DEFLATE_OPT_PRICE_ONE);
  for (size_t i = 0; i < DEFLATE_MAX_LITLEN_SYMBOLS; i++) {
    if (st->opt_lit_price[i] > 15u * DEFLATE_OPT_PRICE_ONE) {
      st->opt_lit_price[i] = 15u * DEFLATE_OPT_PRICE_ONE;
    }
  }
  for (size_t i = 0; i < DEFLATE_MAX_DIST_SYMBOLS; i++) {
    if (st->opt_dist_price[i] > 15u * DEFLATE_OPT_PRICE_ONE) {
      st->opt_dist_price[i] = 15u * DEFLATE_OPT_PRICE_ONE;
    }
  }
}

/**
 * @brief What a match of @p length at @p distance costs, in 256ths of a bit.
 */
static inline uint32_t deflate_opt_match_price(
    const gcomp_deflate_encoder_state_t * st, uint32_t length,
    uint32_t distance) {
  uint32_t lc = gcomp_deflate_length_code(length);
  uint32_t dc = gcomp_deflate_distance_code(distance);
  return st->opt_lit_price[lc] +
      ((uint32_t)k_len_extra[lc - 257u] << DEFLATE_OPT_PRICE_SHIFT) +
      st->opt_dist_price[dc] +
      ((uint32_t)k_dist_extra[dc] << DEFLATE_OPT_PRICE_SHIFT);
}

/**
 * @brief Record one symbol against the model and in the block.
 */
static inline void deflate_opt_emit(gcomp_deflate_encoder_state_t * st,
    uint32_t length, uint32_t distance, uint8_t literal) {
  if (length >= DEFLATE_MIN_MATCH_LENGTH) {
    st->lit_buf[st->sym_buf_used] = (uint16_t)length;
    st->dist_buf[st->sym_buf_used] = (uint16_t)distance;
    st->sym_buf_used++;
    st->block_input_len += (size_t)length;
    uint32_t lc = gcomp_deflate_length_code(length);
    uint32_t dc = gcomp_deflate_distance_code(distance);
    if (st->lit_freq) {
      st->lit_freq[lc]++;
      st->dist_freq[dc]++;
    }
    st->opt_lit_freq[lc]++;
    st->opt_dist_freq[dc]++;
  }
  else {
    st->lit_buf[st->sym_buf_used] = literal;
    st->dist_buf[st->sym_buf_used] = 0;
    st->sym_buf_used++;
    st->block_input_len += 1u;
    if (st->lit_freq) {
      st->lit_freq[literal]++;
    }
    st->opt_lit_freq[literal]++;
  }
}

/**
 * @brief Parse a stretch of the lookahead by shortest path, and emit it.
 *
 * @param st Encoder state.
 * @param pos Window index of the first position to parse.
 * @param stream_pos Stream position of that first position.
 * @param max_chain How many chain candidates a position may consider.
 * @param sweep Positions to sweep.  Matches leaving the last of them may
 *        reach up to DEFLATE_MAX_MATCH_LENGTH further, which is why the
 *        caller keeps that much lookahead in hand when there is more input.
 */
static void deflate_optimal_parse(gcomp_deflate_encoder_state_t * st,
    size_t pos, size_t stream_pos, int max_chain, size_t sweep) {
  deflate_opt_node_t * nodes = st->opt_nodes;
  const size_t mask = st->window_mask;

  deflate_opt_rebuild_prices(st);

  // Nothing has reached any of these yet.  A match edge can land well past
  // the next position, so a node the sweep has not arrived at may already
  // hold a price -- which means "unreached" has to be a value, not the
  // absence of one.
  for (size_t t = 1; t <= sweep; t++) {
    nodes[t].price = DEFLATE_OPT_PRICE_INF;
  }
  nodes[0].price = 0;
  nodes[0].length = 0;
  nodes[0].distance = 0;
  nodes[0].full = 0;

  for (size_t j = 0; j < sweep; j++) {
    const size_t p = (pos + j) & mask;
    const size_t avail = st->lookahead - j;
    const uint32_t here = nodes[j].price;

    // The literal edge.  Every literal is its own symbol, so this is the
    // whole of its cost -- there is no run length to account for, which is
    // what makes this sweep exact rather than approximate.
    {
      uint32_t next = here + st->opt_lit_price[st->window[p]];
      if (next < nodes[j + 1u].price) {
        nodes[j + 1u].price = next;
        nodes[j + 1u].length = 0;
        nodes[j + 1u].distance = 0;
        nodes[j + 1u].full = 0;
      }
    }

    if (avail < DEFLATE_MIN_MATCH_LENGTH) {
      continue;
    }

    deflate_match_t cand[DEFLATE_OPT_MAX_CANDIDATES];
    size_t ncand = 0;
    (void)deflate_find_match_list(st, p, stream_pos + j, max_chain, avail, cand,
        DEFLATE_OPT_MAX_CANDIDATES, &ncand);

    // Searching a position does not enter it into the chains; that is a
    // separate step.  It has to happen here rather than when the path is
    // emitted, because a match at a later position of this same sweep may
    // point at this one and can only find it if it is already in the chain.
    deflate_insert_hash(st, p, stream_pos + j);

    // The match edges, shortest candidate first.  Each covers the lengths
    // the one before it could not reach, so no length is priced twice
    // against a worse distance.
    uint32_t budget = st->opt_budget;
    uint32_t covered = DEFLATE_MIN_MATCH_LENGTH - 1u;
    for (size_t c = 0; c < ncand; c++) {
      const uint32_t whole = cand[c].length;
      if (whole <= covered) {
        continue;
      }
      // A match may not be relaxed past the end of the sweep; the path has
      // to land there.  What it would have been is kept, for the one step
      // where being cut short is the boundary talking and not the cost.
      uint32_t hi = whole;
      if (j + hi > sweep) {
        hi = (uint32_t)(sweep - j);
      }
      const uint32_t lo = covered + 1u;
      covered = whole;
      if (hi < lo) {
        continue;
      }

      uint32_t span = hi - lo;
      if (span > budget) {
        span = budget;
      }
      budget -= span;

      for (uint32_t len = lo;; len++) {
        if (len > lo + span) {
          len = hi; // Only the whole match is left in the budget.
        }
        uint32_t price =
            here + deflate_opt_match_price(st, len, cand[c].distance);
        size_t t = j + len;
        if (price < nodes[t].price) {
          nodes[t].price = price;
          nodes[t].length = len;
          nodes[t].distance = cand[c].distance;
          nodes[t].full = whole;
        }
        if (len >= hi) {
          break;
        }
      }
    }
  }

  // Walk the best path to the end of the sweep backwards, recording at each
  // position the edge the path leaves by, so it can then be walked forwards
  // to emit.
  // `last` is where the path's final step starts, which is the FIRST thing
  // this walk meets, since it goes backwards.  Taking it from the last
  // iteration instead makes it zero, and then only one step of each sweep is
  // emitted and everything else the sweep worked out is thrown away -- which
  // still terminates, so it shows up as the encoder being a hundred times
  // slower rather than as anything failing.
  size_t last = 0;
  {
    size_t k = sweep;
    int seen_last = 0;
    while (k > 0) {
      const uint32_t m = nodes[k].length;
      const size_t prev = (m == 0u) ? (k - 1u) : (k - m);
      nodes[prev].out_length = m;
      nodes[prev].out_distance = (m == 0u) ? 0u : nodes[k].distance;
      nodes[prev].out_full = (m == 0u) ? 0u : nodes[k].full;
      if (!seen_last) {
        last = prev;
        seen_last = 1;
      }
      k = prev;
    }
  }

  // Emit forwards, consuming the lookahead as it goes.  Positions inside a
  // match were entered into the chains by the sweep, so nothing is inserted
  // here -- except past the end of the sweep, which the last step may reach.
  size_t i = 0;
  while (i < last) {
    const uint32_t m = nodes[i].out_length;
    const size_t p = (pos + i) & mask;
    if (m == 0u) {
      deflate_opt_emit(st, 0, 0, st->window[p]);
      st->lookahead--;
      i++;
      continue;
    }
    deflate_opt_emit(st, m, nodes[i].out_distance, 0);
    st->lookahead -= m;
    i += m;
  }

  // The last step, put back to its whole length.
  //
  // Every other step of the path is followed by another, so a match cut to
  // fit the end of the sweep would only have displaced the step after it.
  // The last one has nothing after it: cutting it is the boundary deciding,
  // not the cost.  On ordinary text that is one truncated match in a hundred
  // and fifty and does not show, but on a run of identical bytes, where every
  // match is the full 258, it is one in seven -- 4 MB of zeroes came out 44%
  // larger at level 9 than at level 6, which is the wrong way round.
  {
    const size_t p = (pos + i) & mask;
    uint32_t m = nodes[i].out_length;
    if (m >= DEFLATE_MIN_MATCH_LENGTH) {
      uint32_t whole = nodes[i].out_full;
      if (whole > m && (size_t)whole <= st->lookahead) {
        m = whole;
      }
      deflate_opt_emit(st, m, nodes[i].out_distance, 0);
      // Whatever this reached past the end of the sweep was never swept, so
      // it is not in the chains yet.
      size_t q = (pos + sweep) & mask;
      size_t qs = stream_pos + sweep;
      for (size_t f = sweep; f < i + m; f++) {
        deflate_insert_hash(st, q, qs);
        q = (q + 1u) & mask;
        qs++;
      }
      st->lookahead -= m;
    }
    else {
      deflate_opt_emit(st, 0, 0, st->window[p]);
      st->lookahead--;
    }
  }
}

static gcomp_status_t deflate_encode_batch(
    gcomp_deflate_encoder_state_t * st, gcomp_buffer_t * input) {

  if (st->stage == DEFLATE_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  // Set bitwriter output buffer, preserving any partial bits from previous
  // call.
  //
  // IMPORTANT: We use set_buffer() instead of init() because DEFLATE blocks
  // do NOT end on byte boundaries. If a previous call to update() wrote a
  // partial byte (e.g., 5 bits), we must preserve those bits. Using init()
  // would reset the bit buffer and corrupt the stream.
  //
  // The bitwriter tracks: data (output pointer), size (capacity), byte_pos
  // (full bytes written), bit_buffer (partial byte), bit_count (bits in
  // buffer). set_buffer() updates data/size/byte_pos but preserves
  // bit_buffer/bit_count.
  gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(
      &st->bitwriter, st->pending_buf, st->pending_size);
  if (s != GCOMP_OK) {
    return s;
  }

  const uint8_t * src = (const uint8_t *)input->data;
  const size_t max_block = deflate_max_block_bytes(st);
  int batch_full = 0;

  // Level 0: stored blocks (no compression)
  if (st->level == 0) {
    while (input->used < input->size) {
      // Fill block buffer
      size_t avail = input->size - input->used;
      size_t space = st->block_buffer_size - st->block_buffer_used;
      size_t copy = (avail < space) ? avail : space;

      if (copy > 0) {
        memcpy(
            st->block_buffer + st->block_buffer_used, src + input->used, copy);
        st->block_buffer_used += copy;
        input->used += copy;
      }

      // Flush if buffer is full
      if (st->block_buffer_used >= st->block_buffer_size) {
        s = deflate_flush_stored_block(st, 0);
        if (s != GCOMP_OK) {
          st->pending_used = gcomp_deflate_bitwriter_bytes_written(
              &st->bitwriter);
          return s;
        }
        // Stop the batch if the staging buffer could not hold another block.
        // The caller drains what is staged and calls again.
        if (gcomp_deflate_bitwriter_bytes_written(&st->bitwriter) + max_block >
            st->pending_size) {
          break;
        }
      }
    }
  }
  else {
    // Levels 1-9: LZ77 + Huffman compression
    //
    // Strategy affects match finding and Huffman mode:
    // - DEFAULT: Standard LZ77 with chain length based on level
    // - LAZY: defers matches at every level, not only from 4 up
    // - HUFFMAN_ONLY: No LZ77, emit all bytes as literals
    // - RLE: Only find matches at distance 1
    // - FIXED: Standard LZ77 but always use fixed Huffman codes
    //
    int max_chain;
    int use_fixed_huffman;
    int skip_lz77 = (st->strategy == DEFLATE_STRATEGY_HUFFMAN_ONLY);

    // Lazy matching: hold a match back one byte to see whether the next
    // position starts a longer one.
    //
    // zlib draws this line between its deflate_fast and deflate_slow paths at
    // level 4, and this now draws it in the same place: levels 4 and up defer
    // a match, levels 1 to 3 take what they find.  A level is a statement
    // about how much work to spend, and deferring is the cheapest large gain
    // available at that price -- it costs no extra searching at all, because
    // the search that settles a held match is the one the next position was
    // going to perform anyway.
    //
    // The lazy strategy keeps deferring at every level: asking for it by
    // name is asking for the effort.
    //
    // A match already at or above max_lazy is taken as it stands: the search
    // that would test it costs as much as the search that found it, and a
    // match that long has little room to improve.  The thresholds follow
    // zlib's, which spends more of this at higher levels.
    int use_lazy = (st->strategy == DEFLATE_STRATEGY_LAZY) ||
        ((st->strategy == DEFLATE_STRATEGY_DEFAULT ||
             st->strategy == DEFLATE_STRATEGY_FIXED) &&
            st->level >= 4);
    const deflate_effort_t * effort =
        &k_deflate_effort[(st->level >= 1 && st->level <= 9) ? st->level : 6];
    uint32_t max_lazy = effort->max_lazy;

    // Levels 1 to 3 carry the deferring threshold rather than a level-shaped
    // one, because the only thing that reads it there is the lazy strategy,
    // and asking for that strategy by name is asking for the effort.  See
    // max_lazy in deflate_effort_t for why the level-shaped value of 4 is the
    // one setting at which deferring cannot pay: on filter-shaped bytes it
    // made the lazy strategy come out *larger* than DEFAULT, 507,624 against
    // 501,304, which is the opposite of what the strategy is for.  With this
    // threshold the same file is 448,562.

    // Determine hash chain length based on level.
    //
    // The lazy strategy used to quadruple this on the reasoning that filter
    // output hides longer patterns behind short hash chains. Measured across
    // 52 files of real PNG filtered rows, 7.3 MB, it does not pay:
    //
    //   chain   with lazy matching   without
    //      32        31.22%           32.91%
    //      64        31.13%           33.42%
    //     128        31.13%           33.41%
    //     256        31.12%           33.40%
    //
    // Chain length is worth 0.1 points across a factor of eight. Lazy matching
    // is worth 1.7. So it now searches exactly as hard as DEFAULT and
    // differs from it only by holding matches back, which is the same
    // relationship zlib's levels 4-9 have to its levels 1-3.
    if (st->strategy == DEFLATE_STRATEGY_RLE) {
      // RLE doesn't use hash chains (only checks distance 1)
      max_chain = 0;
    }
    else {
      max_chain = effort->max_chain;
    }

    // Only the fixed strategy forces fixed codes now.  Every other block goes
    // to deflate_flush_dynamic_block(), which prices both codings and writes
    // whichever is smaller -- so a block whose symbols happen to suit the
    // fixed code still gets it, without a level having to guess in advance.
    use_fixed_huffman = (st->strategy == DEFLATE_STRATEGY_FIXED);

    while (input->used < input->size) {
      // Fill window with input data.
      //
      // Only up to refill_to bytes of lookahead are held at a time.  The
      // window is circular and exactly window_size bytes long, so filling it
      // to the brim - which is what this did - overwrote every byte of
      // history in one go: the encoder ran through 32 KB of lookahead with
      // nothing behind it, refilled, and started again with an empty window.
      // Matches could not cross those boundaries at all, and inside a batch
      // the reachable history was whatever had been consumed so far rather
      // than the 32 KB the format allows.
      //
      // Holding the lookahead to a kilobyte instead keeps the rest of the
      // window as history.  Across 12 MB of source, prose, XML and binaries
      // that is 8.0% fewer bytes out, and faster: the encoder finds longer
      // matches, so it emits fewer symbols for the same input.
      // The optimal parse cannot see past the lookahead, and a boundary it
      // cannot see past costs whatever the match crossing it would have been
      // worth, so those levels hold more of it.  See DEFLATE_OPT_LOOKAHEAD.
      size_t refill_to = st->window_size / 2u;
      size_t want_lookahead = st->use_opt
          ? deflate_opt_lookahead(st->window_size)
          : (size_t)DEFLATE_REFILL_LOOKAHEAD;
      if (refill_to > want_lookahead) {
        refill_to = want_lookahead;
      }
      size_t avail = input->size - input->used;
      size_t space = (st->lookahead < refill_to) ? (refill_to - st->lookahead)
                                                 : 0u;
      size_t copy = (avail < space) ? avail : space;

      if (copy > 0) {
        for (size_t i = 0; i < copy; i++) {
          st->window[st->window_pos] = src[input->used + i];
          st->window_pos = (st->window_pos + 1) & st->window_mask;
        }
        // The same bytes, kept a second time when the window is too small to
        // hold a block and its lookahead together.  One pass rather than a
        // branch per byte, and nothing to do at all for the window sizes that
        // can serve the block themselves.
        if (st->stored_buf) {
          for (size_t i = 0; i < copy; i++) {
            st->stored_buf[st->stored_pos] = src[input->used + i];
            st->stored_pos = (st->stored_pos + 1) & st->stored_mask;
          }
          if (st->stored_fill < st->stored_size) {
            st->stored_fill += copy;
            if (st->stored_fill > st->stored_size) {
              st->stored_fill = st->stored_size;
            }
          }
        }
        st->lookahead += copy;
        st->total_in += copy;
        if (st->window_fill < st->window_size) {
          st->window_fill += copy;
          if (st->window_fill > st->window_size) {
            st->window_fill = st->window_size;
          }
        }
        input->used += copy;
      }

      // Process lookahead data
      // Go back for more input before the lookahead falls short of a full
      // match, so that a match near the end of a batch is not cut off by the
      // batch boundary.  With no input left there is nothing to wait for, and
      // the remaining lookahead is encoded as it stands.
      size_t refill_at = refill_to / 2u;
      if (refill_at > DEFLATE_MIN_LOOKAHEAD) {
        refill_at = DEFLATE_MIN_LOOKAHEAD;
      }
      while (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH ||
          (skip_lz77 && st->lookahead > 0)) {
        if (!skip_lz77 && st->lookahead < refill_at &&
            input->used < input->size) {
          break;
        }
        // A block is closed when the symbol buffer fills, and also when it
        // is nearly all literals and has grown to what the window can still
        // hand back.
        //
        // A stored block's bytes come from the window, so they have to still
        // be in it: the block may cover at most window_size minus the
        // lookahead in front of it.  A literal-heavy block covers about one
        // byte per symbol, so with a symbol buffer the size of the window it
        // runs past that, and the stored form - the one thing that stops an
        // encoder expanding its input - is never available where it is most
        // wanted.
        //
        // Shortening every block instead costs real data real bytes: taking
        // two refills' worth off the symbol buffer was 0.34% at level 1
        // across the corpus.  The mean-coverage test keeps that cost off
        // compressible data, where blocks cover three to fourteen bytes per
        // symbol and a stored block could never have won anyway.
        // The reach and the step are both measured against how much
        // lookahead this level holds, not against a constant.  A block's
        // bytes have to still be in the window when it is written, so the
        // lookahead in front of them is what they cannot cover -- and the
        // check has to fire a whole step early, because one step is the
        // most the block can grow by before it is asked again.  For the
        // greedy parse a step is a match; for a sweep it is the whole
        // stretch swept, which is why this is not DEFLATE_MAX_MATCH_LENGTH.
        //
        // Both figures used to be flat constants, and both were wrong below a
        // 2 KiB window.  The refill above will not ask for more lookahead than
        // half the window, so `held` cannot be DEFLATE_REFILL_LOOKAHEAD there;
        // and a match cannot be longer than the lookahead holding it, so
        // `step` cannot be DEFLATE_MAX_MATCH_LENGTH either.  Overstating both
        // made the test fire on every symbol at window_bits 8, where a single
        // assumed step of 258 already exceeded the whole 256-byte window.
        size_t held = st->use_opt ? deflate_opt_lookahead(st->window_size)
                                  : (size_t)DEFLATE_REFILL_LOOKAHEAD;
        if (held > st->window_size / 2u) {
          held = st->window_size / 2u;
        }
        size_t step = st->use_opt ? held : (size_t)DEFLATE_MAX_MATCH_LENGTH;
        if (step > held) {
          step = held;
        }
        // Measured against what a stored block can actually be written from,
        // which is the ring when the window is too small to serve.
        const size_t span =
            st->stored_buf ? st->stored_size : st->window_size;
        size_t stored_reach =
            (span > 4u * held) ? span - 2u * held : span;
        // Within an eighth of one byte per symbol: essentially nothing is
        // matching.  A looser test - twice a byte per symbol - also fires on
        // data that compresses a little, such as a JPEG, where the extra
        // block header costs more than the stored form could ever save.
        int literal_heavy = st->block_input_len <
            (size_t)st->sym_buf_used + (size_t)st->sym_buf_used / 8u;
        int out_of_window_reach = literal_heavy && st->sym_buf_used > 0 &&
            st->block_input_len + step >= stored_reach;

        // A sweep emits at most one symbol per position and will not run in
        // less room than DEFLATE_OPT_MIN_SWEEP, so the block is closed once
        // that much is all that is left.
        int no_room_to_sweep = st->use_opt &&
            st->sym_buf_used + DEFLATE_OPT_MIN_SWEEP > st->sym_buf_size - 2u;

        if (st->sym_buf_used >= st->sym_buf_size - 2 || out_of_window_reach ||
            no_room_to_sweep) {
          if (use_fixed_huffman) {
            s = deflate_flush_fixed_block(st, 0);
          }
          else {
            s = deflate_flush_dynamic_block(st, 0);
          }
          if (s != GCOMP_OK) {
            st->pending_used = gcomp_deflate_bitwriter_bytes_written(
                &st->bitwriter);
            return s;
          }
          // Stop the batch if the staging buffer could not hold another
          // block. The caller drains what is staged and calls again; the
          // window, symbol buffer and partial bits all persist in st.
          if (gcomp_deflate_bitwriter_bytes_written(&st->bitwriter) +
                  max_block >
              st->pending_size) {
            batch_full = 1;
            break;
          }
        }

        size_t pos = (st->window_pos + st->window_size - st->lookahead) %
            st->window_size;
        size_t stream_pos = st->total_in - st->lookahead;

        // The levels that parse by shortest path do a stretch at a time.
        //
        // When there is more input to come, the sweep stops a whole match
        // short of the end of the lookahead, so that a match leaving its
        // last position is not cut off by data that has not arrived.  When
        // the input is finished there is nothing to wait for and the rest is
        // swept as it stands.
        //
        // The symbol buffer bounds it too: a sweep emits at most one symbol
        // per position, and the block is closed above when the buffer fills.
        if (st->use_opt) {
          size_t sweep = st->lookahead;
          // With more input to come, stop a whole match short of the end of
          // the lookahead, so that a match leaving the last position swept is
          // not cut off by data that has not arrived.  That only makes sense
          // while the lookahead is comfortably longer than a match; where it
          // is not -- a small declared window -- the boundary is taken as it
          // comes, which costs a match at the edge rather than the sweep.
          if (input->used < input->size &&
              sweep > 2u * DEFLATE_MAX_MATCH_LENGTH) {
            sweep -= DEFLATE_MAX_MATCH_LENGTH;
          }
          if (sweep > st->opt_node_cap - 1u) {
            sweep = st->opt_node_cap - 1u;
          }
          size_t room = st->sym_buf_size - 2u - st->sym_buf_used;
          if (sweep > room) {
            sweep = room;
          }
          // Every bound above is at least DEFLATE_MIN_MATCH_LENGTH here: the
          // loop only runs with that much lookahead, the table holds at least
          // half a window, and the block was closed if less than
          // DEFLATE_OPT_MIN_SWEEP symbols were left.  So a sweep always
          // consumes something, which is what stops this looping.
          deflate_optimal_parse(st, pos, stream_pos, max_chain, sweep);
          continue;
        }

        // Strategy-specific match finding
        deflate_match_t match = {0, 0};

        if (skip_lz77) {
          // HUFFMAN_ONLY: No match finding at all
          // match stays {0, 0} - will emit literal
        }
        else if (st->strategy == DEFLATE_STRATEGY_RLE) {
          // RLE: Only look for matches at distance 1.
          //
          // window_fill > lookahead is the test for the window holding any
          // history at all, and stream_pos > 0 is not it.  The refill takes
          // the window up to window_size - lookahead bytes at a time, so the
          // lookahead can fill the window completely; the byte before pos is
          // then the last byte of the lookahead - data not yet emitted -
          // rather than the byte a distance of 1 refers to.  Reading it there
          // made the encoder emit a distance-1 match against a byte it had
          // not written, which decodes to whatever really did precede it.
          //
          // It showed up as a run continuing one byte past its end: with an
          // 8-bit window, 40,000 bytes of 1,000-byte runs decoded correctly
          // until offset 32,000 and then carried 0x1f where 0x20 belonged.
          // Both this library's decoder and zlib reproduced the same wrong
          // bytes, which is what says the fault is on this side.
          if (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH && stream_pos > 0 &&
              st->window_fill > st->lookahead) {
            // Check for run at distance 1
            size_t prev_pos = (pos + st->window_size - 1) & st->window_mask;
            uint8_t run_byte = st->window[prev_pos];
            size_t run_len = 0;
            size_t max_len = st->lookahead;
            if (max_len > DEFLATE_MAX_MATCH_LENGTH) {
              max_len = DEFLATE_MAX_MATCH_LENGTH;
            }

            // Count how many bytes match the previous byte
            while (run_len < max_len &&
                st->window[(pos + run_len) & st->window_mask] == run_byte) {
              run_len++;
            }

            if (run_len >= DEFLATE_MIN_MATCH_LENGTH) {
              match.length = (uint32_t)run_len;
              match.distance = 1;
            }
          }
        }
        else if (st->lookahead >= DEFLATE_MIN_MATCH_LENGTH) {
          // DEFAULT/LAZY/FIXED: Standard LZ77 match finding.
          //
          match = deflate_find_match(st, pos, stream_pos, max_chain);

          // A three-byte match far away is not worth its distance code.  The
          // code for a distance past 4096 carries eleven extra bits on top of
          // the code itself (RFC 1951 section 3.2.5), so the sequence costs
          // about as much as the three literals it replaces -- and unlike
          // them it interrupts the literal run, which the Huffman code was
          // about to encode cheaply.
          //
          // Measured over a 12 MB corpus: 1.0% smaller at level 1, 0.1% at
          // level 6, and faster at every level, because a discarded match is
          // one the encoder does not have to emit.  zlib draws the same line
          // in the same place.
          if (match.length == DEFLATE_MIN_MATCH_LENGTH &&
              match.distance > DEFLATE_TOO_FAR) {
            match.length = 0;
            match.distance = 0;
          }
        }

        // Lazy matching, as a deferral rather than a second search.
        //
        // The question at position p is whether p is better spent as a
        // literal because the match at p+1 is longer.  This used to answer it
        // by searching p+1 and then throwing the answer away, so the next
        // iteration searched p+1 again: two full walks of a 128-deep chain
        // for every position that found a short match.
        //
        // Now the match at p is held in the encoder state and the encoder
        // moves on, and the search the next iteration performs anyway is the
        // one that settles it.  One search per position.
        //
        // Deferring also finds matches the old arrangement could not see.  A
        // position is entered into the hash chains as it is passed, so the
        // chain searched at p+1 now contains p - which is where a match at
        // distance 1 comes from, and those are exactly the runs that PNG
        // filter output is full of.  The old lookahead search ran before p was
        // inserted and could never find one.
        if (use_lazy) {
          if (st->lazy_length >= DEFLATE_MIN_MATCH_LENGTH) {
            uint32_t held_length = st->lazy_length;
            uint32_t held_distance = st->lazy_distance;
            st->lazy_length = 0;

            if (match.length > held_length) {
              // The later match is strictly longer, so the byte the held
              // match started on is spent as a literal.  That position was
              // consumed and hashed when it was held, so only the symbol is
              // recorded here; `match` is reconsidered below.
              size_t held_pos = (pos + st->window_size - 1u) & st->window_mask;
              uint8_t lit = st->window[held_pos];
              st->lit_buf[st->sym_buf_used] = lit;
              st->dist_buf[st->sym_buf_used] = 0;
              st->sym_buf_used++;
              st->block_input_len += 1u;
              if (st->lit_freq) {
                st->lit_freq[lit]++;
              }
            }
            else {
              // Nothing better turned up; take the held match.  It began one
              // byte back, so its first byte is already accounted for and
              // only the rest is consumed here.
              st->lit_buf[st->sym_buf_used] = (uint16_t)held_length;
              st->dist_buf[st->sym_buf_used] = (uint16_t)held_distance;
              st->sym_buf_used++;
              st->block_input_len += (size_t)held_length;
              if (st->lit_freq) {
                st->lit_freq[gcomp_deflate_length_code(held_length)]++;
                st->dist_freq[gcomp_deflate_distance_code(held_distance)]++;
              }
              for (uint32_t i = 0; i + 1u < held_length && st->lookahead > 0;
                  i++) {
                if (st->lookahead >= 3) {
                  deflate_insert_hash(st, pos, stream_pos);
                }
                pos = (pos + 1) & st->window_mask;
                stream_pos++;
                st->lookahead--;
              }
              continue;
            }
          }

          // Hold this match back if there is any prospect of improving on it.
          // st->lookahead > match.length keeps a byte in hand for the next
          // position to be searched at all.
          if (match.length >= DEFLATE_MIN_MATCH_LENGTH &&
              match.length < max_lazy && st->lookahead > match.length) {
            st->lazy_length = match.length;
            st->lazy_distance = match.distance;
            if (st->lookahead >= 3) {
              deflate_insert_hash(st, pos, stream_pos);
            }
            st->lookahead--;
            continue;
          }
        }

        if (match.length >= DEFLATE_MIN_MATCH_LENGTH) {
          // Record length/distance pair
          st->lit_buf[st->sym_buf_used] = (uint16_t)match.length;
          st->dist_buf[st->sym_buf_used] = (uint16_t)match.distance;
          st->sym_buf_used++;
          st->block_input_len += (size_t)match.length;

          // Track frequencies for dynamic Huffman
          if (st->lit_freq) {
            uint32_t len_code = gcomp_deflate_length_code(match.length);
            st->lit_freq[len_code]++;
            uint32_t dist_code = gcomp_deflate_distance_code(match.distance);
            st->dist_freq[dist_code]++;
          }

          // Insert all bytes of the match into the hash table
          for (uint32_t i = 0; i < match.length && st->lookahead > 0; i++) {
            if (st->lookahead >= 3) {
              deflate_insert_hash(st, pos, stream_pos);
            }
            pos = (pos + 1) & st->window_mask;
            stream_pos++;
            st->lookahead--;
          }
        }
        else {
          // Record literal
          uint8_t lit = st->window[pos];
          st->lit_buf[st->sym_buf_used] = lit;
          st->dist_buf[st->sym_buf_used] = 0;
          st->sym_buf_used++;
          st->block_input_len += 1u;

          // Track frequencies for dynamic Huffman
          if (st->lit_freq) {
            st->lit_freq[lit]++;
          }

          // Insert byte into hash table (unless HUFFMAN_ONLY)
          if (!skip_lz77 && st->lookahead >= 3) {
            deflate_insert_hash(st, pos, stream_pos);
          }
          st->lookahead--;
        }
      }

      if (batch_full) {
        break;
      }

      // If we can't make progress, break
      if (copy == 0 && st->lookahead < DEFLATE_MIN_MATCH_LENGTH &&
          !(skip_lz77 && st->lookahead > 0)) {
        break;
      }
    }
  }

  st->pending_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_encoder_update(gcomp_encoder_t * encoder,
    gcomp_buffer_t * input, gcomp_buffer_t * output) {
  if (!encoder || !input || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  // Check data pointers if size > 0
  if ((input->size > 0 && !input->data) ||
      (output->size > 0 && !output->data)) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  if (st->stage == DEFLATE_ENC_STAGE_DONE) {
    return GCOMP_OK;
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // Rendered blocks are staged here and copied out as the caller's buffer
  // allows, so no input is ever consumed that cannot later be delivered.
  if (!st->pending_buf) {
    size_t size = deflate_max_block_bytes(st);
    st->pending_buf = (uint8_t *)gcomp_malloc(alloc, size);
    if (!st->pending_buf) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
          "failed to allocate deflate staging buffer (%zu bytes)", size);
    }
    st->pending_size = size;
    st->pending_used = 0;
    st->pending_copied = 0;
  }

  for (;;) {
    // Hand over whatever is already staged. If it does not all fit, the
    // caller must drain and call again; no input is taken this round.
    if (!deflate_drain_pending(st, output)) {
      return GCOMP_OK;
    }
    if (input->used >= input->size || output->used >= output->size) {
      return GCOMP_OK;
    }

    size_t before = input->used;
    gcomp_status_t s = deflate_encode_batch(st, input);
    if (s != GCOMP_OK) {
      // Deliver what the failed batch had already rendered, then report.
      (void)deflate_drain_pending(st, output);
      return s;
    }
    if (input->used == before && st->pending_used == 0) {
      // Neither consumed nor produced: nothing more to do this call.
      return GCOMP_OK;
    }
  }
}

/**
 * @brief Estimate the maximum size needed for finish() output.
 *
 * This is a conservative upper bound to ensure we allocate enough buffer
 * space to render the entire finish output in one pass.
 *
 * The estimate accounts for:
 * - Remaining lookahead bytes (each becomes a literal or part of a match)
 * - Buffered symbols that need to be flushed
 * - Dynamic Huffman tree overhead
 * - Block headers and end-of-block markers
 * - Byte alignment padding
 *
 * @param st Encoder state
 * @return Conservative upper bound in bytes
 */
static size_t deflate_estimate_finish_size(
    const gcomp_deflate_encoder_state_t * st) {
  // Each literal/length can be up to 15 bits (dynamic Huffman max)
  // Each distance can be up to 15 bits + 13 extra bits = 28 bits
  // Worst case for a length/distance pair: ~43 bits
  // For a literal: 15 bits
  //
  // Conservative: assume 4 bytes per symbol (32 bits) which is more than
  // enough for any symbol type.
  size_t sym_overhead = 4;

  // Count symbols: remaining lookahead + already buffered symbols
  size_t total_symbols = st->lookahead + st->sym_buf_used;

  // For level 0, each byte becomes a stored block byte (1:1 plus header)
  if (st->level == 0) {
    // Stored block: 3 bits header, byte-align, 4 bytes LEN/NLEN, then data
    // Plus we might have multiple blocks if data is large
    size_t data_bytes = st->block_buffer_used + st->lookahead;
    size_t num_blocks =
        (data_bytes + DEFLATE_MAX_STORED_BLOCK - 1) / DEFLATE_MAX_STORED_BLOCK;
    if (num_blocks == 0) {
      num_blocks = 1; // At least one final block
    }
    // Each block: up to 5 bytes header (3 bits rounded + 4 bytes LEN/NLEN)
    // plus the data
    // Check for overflow: num_blocks * 5 + data_bytes + 8
    if (num_blocks > SIZE_MAX / 5) {
      return SIZE_MAX / 2;
    }
    size_t header_bytes = num_blocks * 5;
    if (header_bytes > SIZE_MAX - data_bytes - 8) {
      return SIZE_MAX / 2;
    }
    return header_bytes + data_bytes + 8; // +8 for safety margin
  }

  // For Huffman blocks, estimate based on symbols
  // Check for overflow: total_symbols * sym_overhead
  if (total_symbols > SIZE_MAX / sym_overhead) {
    // Overflow would occur; return a large but safe value
    return SIZE_MAX / 2;
  }
  size_t symbol_bytes = total_symbols * sym_overhead;

  // Dynamic Huffman tree overhead: up to ~300 bytes for the tree encoding
  // (HLIT/HDIST/HCLEN headers, code-length codes, encoded code lengths)
  size_t tree_overhead = 512;

  // Block headers (3 bits each) and end-of-block (up to 15 bits)
  // Multiple blocks may be needed if sym_buf fills up
  size_t num_blocks = (total_symbols + st->sym_buf_size - 1) / st->sym_buf_size;
  if (num_blocks == 0) {
    num_blocks = 1;
  }

  // Check for overflow: num_blocks * (tree_overhead + 8)
  size_t per_block = tree_overhead + 8;
  if (num_blocks > SIZE_MAX / per_block) {
    return SIZE_MAX / 2;
  }
  size_t block_overhead = num_blocks * per_block;

  // Byte alignment (up to 7 bits = 1 byte)
  size_t alignment = 1;

  // Safety margin
  size_t margin = 64;

  // Check for overflow in final addition
  size_t result = symbol_bytes;
  if (result > SIZE_MAX - block_overhead) {
    return SIZE_MAX / 2;
  }
  result += block_overhead;
  if (result > SIZE_MAX - alignment - margin) {
    return SIZE_MAX / 2;
  }

  return result + alignment + margin;
}

/**
 * @brief Write the empty stored block that ends a flush.
 *
 * RFC 1951 3.2.3 and 3.2.4: a block header of BFINAL=0, BTYPE=00, then
 * padding to the next byte boundary, then LEN=0x0000 and NLEN=0xFFFF.  Five
 * bytes at most, carrying no data.
 *
 * WHY A FLUSH CANNOT JUST STOP
 * ============================
 * DEFLATE blocks do not end on byte boundaries, so ending a block and handing
 * over the whole bytes written leaves the decoder mid-byte with padding bits
 * it has no way to recognise as padding.  It would read them as the next
 * block's header -- three zero bits say "a stored block follows" -- and then
 * take LEN and NLEN from whatever bytes arrive next, which are the start of
 * real data.
 *
 * Writing the empty stored block explicitly makes that reading correct: the
 * header is real, the padding is consumed by the alignment the format
 * requires after it, LEN says zero bytes follow, and both sides come out of
 * it byte-aligned at the start of the next block header.  This is the same
 * four-byte 00 00 FF FF tail zlib's Z_SYNC_FLUSH produces, for the same
 * reason.
 */
static gcomp_status_t deflate_write_sync_marker(
    gcomp_deflate_encoder_state_t * st) {
  gcomp_status_t s =
      gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 1); // BFINAL=0
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, 0u, 2); // BTYPE=stored
  if (s != GCOMP_OK) {
    return s;
  }
  s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
  if (s != GCOMP_OK) {
    return s;
  }
  static const uint8_t marker[4] = {0x00u, 0x00u, 0xFFu, 0xFFu};
  for (size_t i = 0; i < sizeof(marker); i++) {
    s = gcomp_deflate_bitwriter_write_bits(&st->bitwriter, marker[i], 8);
    if (s != GCOMP_OK) {
      return s;
    }
  }
  return GCOMP_OK;
}

/**
 * @brief Render a flush into the staging buffer.
 *
 * The same three steps finish() takes -- emit the match held back by lazy
 * matching, turn the remaining lookahead into literals, close the block --
 * with two differences: the block is not marked final, and the lookahead
 * bytes are entered into the hash chains as they go.
 *
 * That second difference is the point of a *sync* flush.  finish() does not
 * bother, because there is no next position to match from; here there is, and
 * without it every byte of lookahead at every flush would be invisible to
 * later matches.  For a caller flushing once per protocol message that is
 * most of the stream.
 */
static gcomp_status_t deflate_stage_flush(
    gcomp_deflate_encoder_state_t * st) {
  gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(
      &st->bitwriter, st->pending_buf, st->pending_size);
  if (s != GCOMP_OK) {
    return s;
  }

  if (st->level == 0) {
    // Stored blocks are byte-aligned and self-terminating, so emitting the
    // buffered data is the whole flush: no marker is needed, and the decoder
    // is left exactly at the next block header.
    while (st->block_buffer_used > 0) {
      s = deflate_flush_stored_block(st, 0);
      if (s != GCOMP_OK) {
        return s;
      }
    }
    st->pending_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
    return GCOMP_OK;
  }

  const int use_fixed_huffman = (st->strategy == DEFLATE_STRATEGY_FIXED);
  const int skip_lz77 = (st->strategy == DEFLATE_STRATEGY_HUFFMAN_ONLY);

  // A match held back by lazy matching has to go out, or the byte it starts
  // on is emitted twice -- once as part of a match that never arrives and
  // once as a literal below.  Same reasoning as finish(); the difference is
  // that here the match may still be beaten by input that has not arrived, so
  // this costs a little ratio.  That is what asking for a flush buys.
  if (st->lazy_length >= DEFLATE_MIN_MATCH_LENGTH) {
    uint32_t held_length = st->lazy_length;
    uint32_t held_distance = st->lazy_distance;
    st->lazy_length = 0;

    if (st->sym_buf_used >= st->sym_buf_size) {
      s = use_fixed_huffman ? deflate_flush_fixed_block(st, 0)
                            : deflate_flush_dynamic_block(st, 0);
      if (s != GCOMP_OK) {
        return s;
      }
    }

    st->lit_buf[st->sym_buf_used] = (uint16_t)held_length;
    st->dist_buf[st->sym_buf_used] = (uint16_t)held_distance;
    st->sym_buf_used++;
    st->block_input_len += (size_t)held_length;
    if (st->lit_freq) {
      st->lit_freq[gcomp_deflate_length_code(held_length)]++;
      st->dist_freq[gcomp_deflate_distance_code(held_distance)]++;
    }

    uint32_t remaining_bytes = held_length - 1u;
    if ((size_t)remaining_bytes > st->lookahead) {
      remaining_bytes = (uint32_t)st->lookahead;
    }
    st->lookahead -= remaining_bytes;
  }

  // Everything still in the lookahead is input the caller has handed over, so
  // it has to be in the output by the time this returns.  There is no more
  // input to match it against, so it goes out as literals.
  while (st->lookahead > 0) {
    if (st->sym_buf_used >= st->sym_buf_size) {
      s = use_fixed_huffman ? deflate_flush_fixed_block(st, 0)
                            : deflate_flush_dynamic_block(st, 0);
      if (s != GCOMP_OK) {
        return s;
      }
    }

    size_t pos = (st->window_pos + st->window_size - st->lookahead) %
        st->window_size;
    uint8_t lit = st->window[pos];
    st->lit_buf[st->sym_buf_used] = lit;
    st->dist_buf[st->sym_buf_used] = 0;
    st->sym_buf_used++;
    st->block_input_len += 1u;
    if (st->lit_freq) {
      st->lit_freq[lit]++;
    }

    // Keep the byte findable.  See the note on this function.
    if (!skip_lz77 && st->lookahead >= 3) {
      deflate_insert_hash(st, pos, st->total_in - st->lookahead);
    }

    st->lookahead--;
  }

  // Close the block, if there is one.  When there are no symbols the previous
  // block already ended with its end-of-block symbol, and an empty block here
  // would only cost bytes.
  if (st->sym_buf_used > 0) {
    s = use_fixed_huffman ? deflate_flush_fixed_block(st, 0)
                          : deflate_flush_dynamic_block(st, 0);
    if (s != GCOMP_OK) {
      return s;
    }
  }

  s = deflate_write_sync_marker(st);
  if (s != GCOMP_OK) {
    return s;
  }

  st->pending_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
  return GCOMP_OK;
}

/**
 * @brief Forget the match history, so nothing after this point reaches back.
 *
 * What GCOMP_FLUSH_FULL adds to a sync flush.  Every back-reference this
 * encoder can emit comes from a hash chain, so emptying the chains is what
 * makes the guarantee -- except under the RLE strategy, which looks at the
 * byte before the current position directly and asks `window_fill >
 * lookahead` whether the window holds any history at all.  So the window has
 * to be told it is empty too, which also stops a block being stored from
 * bytes written before the flush.
 */
static void deflate_drop_history(gcomp_deflate_encoder_state_t * st) {
  memset(st->hash_head, 0, DEFLATE_HASH_SIZE * sizeof(uint16_t));
  memset(st->hash_prev, 0, st->window_size * sizeof(uint16_t));
  memset(st->hash_pos, 0, st->window_size * sizeof(size_t));
  memset(st->hash_at, 0, st->window_size * sizeof(uint16_t));
  st->hash_value = 0;
  st->window_fill = 0;
  // Same reason the window is emptied: after a full flush the decoder has no
  // history either, so nothing written before it may be stored from.
  st->stored_fill = 0;
}

gcomp_status_t gcomp_deflate_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode) {
  if (!encoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }
  if (output->size > 0 && !output->data) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }
  if (st->final_block_written) {
    return gcomp_encoder_set_error(encoder, GCOMP_ERR_INVALID_ARG,
        "deflate encoder cannot flush after finish");
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  if (!st->flush_staged) {
    // Whatever update() staged but could not deliver goes first; the flush is
    // rendered into the same buffer, so it has to be empty before we start.
    if (st->pending_buf && st->pending_used > st->pending_copied) {
      if (!deflate_drain_pending(st, output)) {
        return GCOMP_ERR_LIMIT;
      }
    }

    // A flush renders more than one batch can: every symbol still buffered,
    // every byte of lookahead as a literal, and the marker.  That is what
    // finish() sizes itself for, so size this the same way.
    //
    // It is also the buffer update() stages into, and update() allocates only
    // when there is none.  A flush before the first update() therefore has to
    // leave a buffer a worst-case block fits in; sized from the finish
    // estimate alone it held a few dozen bytes, and the next large update()
    // stopped every batch at once and failed with GCOMP_ERR_LIMIT.
    size_t need = deflate_estimate_finish_size(st) + 16u;
    const size_t block = deflate_max_block_bytes(st);
    if (need < block) {
      need = block;
    }
    if (st->pending_size < need) {
      uint8_t * grown = (uint8_t *)gcomp_malloc(alloc, need);
      if (!grown) {
        return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
            "failed to allocate deflate flush buffer (%zu bytes)", need);
      }
      gcomp_free(alloc, st->pending_buf);
      st->pending_buf = grown;
      st->pending_size = need;
      st->pending_used = 0;
      st->pending_copied = 0;
    }

    gcomp_status_t s = deflate_stage_flush(st);
    if (s != GCOMP_OK) {
      return gcomp_encoder_set_error(encoder, s, "deflate flush failed");
    }
    st->flush_staged = 1;

    // Dropping the history is safe here and not before: the lookahead has
    // been turned into symbols and those symbols are written, so nothing
    // still to be emitted refers to what is being forgotten.
    if (mode == GCOMP_FLUSH_FULL) {
      deflate_drop_history(st);
    }
  }

  if (!deflate_drain_pending(st, output)) {
    return GCOMP_ERR_LIMIT;
  }
  st->flush_staged = 0;
  return GCOMP_OK;
}

gcomp_status_t gcomp_deflate_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output) {
  if (!encoder || !output) {
    return GCOMP_ERR_INVALID_ARG;
  }

  gcomp_deflate_encoder_state_t * st =
      (gcomp_deflate_encoder_state_t *)encoder->method_state;
  if (!st) {
    return gcomp_encoder_set_error(
        encoder, GCOMP_ERR_INTERNAL, "deflate encoder state is NULL");
  }

  if (st->final_block_written) {
    return GCOMP_OK;
  }

  // Anything update() staged but could not deliver must go out first, ahead
  // of the final block. Report GCOMP_ERR_LIMIT until it has all been handed
  // over, the same "call me again" contract finish() already uses below.
  if (st->pending_buf && st->pending_used > st->pending_copied) {
    if (!deflate_drain_pending(st, output)) {
      return GCOMP_ERR_LIMIT;
    }
  }

  const gcomp_allocator_t * alloc =
      gcomp_registry_get_allocator(encoder->registry);

  // If we haven't rendered the finish output yet, do so now
  if (!st->finish_buf_ready) {
    // Estimate how much buffer we need
    size_t buf_size = deflate_estimate_finish_size(st);

    // Allocate the finish buffer
    st->finish_buf = (uint8_t *)gcomp_malloc(alloc, buf_size);
    if (!st->finish_buf) {
      return gcomp_encoder_set_error(encoder, GCOMP_ERR_MEMORY,
          "failed to allocate deflate finish buffer (%zu bytes)", buf_size);
    }
    st->finish_buf_size = buf_size;
    st->finish_buf_used = 0;
    st->finish_buf_copied = 0;

    // Set up bitwriter to write to our internal buffer
    gcomp_status_t s = gcomp_deflate_bitwriter_set_buffer(
        &st->bitwriter, st->finish_buf, st->finish_buf_size);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    if (st->level == 0) {
      // Flush remaining stored data as final block
      s = deflate_flush_stored_block(st, 1);
    }
    else {
      // The same rule as the streaming loop above: only the fixed strategy
      // forces fixed codes.  Every other block goes to
      // deflate_flush_dynamic_block(), which prices the dynamic coding
      // against the fixed one and writes whichever is smaller.
      //
      // This condition used to carry `|| (st->level <= 3)` as well.  That is
      // the assumption 44e963c removed from the loop -- a level cannot know
      // whether a table will pay for itself, because that depends on the
      // block -- and it was left behind here, so the blocks flushed at the
      // end of the stream were still decided by the level.  It was worth
      // 0.56% at level 1 over a 19 MB corpus, 0.74% at level 2 and 0.59% at
      // level 3; and for a buffer small enough to be encoded in one call the
      // final block is the whole output, where it is worth far more: 3,360
      // bytes of English at level 1 came out 112 bytes forced fixed against
      // the 96 a priced block gives.
      int use_fixed_huffman = (st->strategy == DEFLATE_STRATEGY_FIXED);

      // A match held back by lazy matching has to go out before the tail is
      // flushed, or the byte it starts on is emitted twice: once as part of
      // the match that never arrives, and once as a literal below.  Nothing
      // better can turn up now - there is no next position to search.
      if (st->lazy_length >= DEFLATE_MIN_MATCH_LENGTH) {
        uint32_t held_length = st->lazy_length;
        uint32_t held_distance = st->lazy_distance;
        st->lazy_length = 0;

        if (st->sym_buf_used >= st->sym_buf_size) {
          s = use_fixed_huffman ? deflate_flush_fixed_block(st, 0)
                                : deflate_flush_dynamic_block(st, 0);
          if (s != GCOMP_OK) {
            gcomp_free(alloc, st->finish_buf);
            st->finish_buf = NULL;
            st->finish_buf_size = 0;
            return s;
          }
        }

        st->lit_buf[st->sym_buf_used] = (uint16_t)held_length;
        st->dist_buf[st->sym_buf_used] = (uint16_t)held_distance;
        st->sym_buf_used++;
        st->block_input_len += (size_t)held_length;
        if (st->lit_freq) {
          st->lit_freq[gcomp_deflate_length_code(held_length)]++;
          st->dist_freq[gcomp_deflate_distance_code(held_distance)]++;
        }

        // Its first byte was consumed when it was held, so only the rest is
        // taken off the lookahead.
        uint32_t remaining_bytes = held_length - 1u;
        if ((size_t)remaining_bytes > st->lookahead) {
          remaining_bytes = (uint32_t)st->lookahead;
        }
        st->lookahead -= remaining_bytes;
      }

      // Flush any remaining lookahead as literals
      while (st->lookahead > 0) {
        if (st->sym_buf_used >= st->sym_buf_size) {
          if (use_fixed_huffman) {
            s = deflate_flush_fixed_block(st, 0);
          }
          else {
            s = deflate_flush_dynamic_block(st, 0);
          }
          if (s != GCOMP_OK) {
            gcomp_free(alloc, st->finish_buf);
            st->finish_buf = NULL;
            st->finish_buf_size = 0;
            return s;
          }
        }

        size_t pos = (st->window_pos + st->window_size - st->lookahead) %
            st->window_size;
        uint8_t lit = st->window[pos];
        st->lit_buf[st->sym_buf_used] = lit;
        st->dist_buf[st->sym_buf_used] = 0;
        st->sym_buf_used++;
        st->block_input_len += 1u;

        // Track frequency for dynamic Huffman
        if (st->lit_freq) {
          st->lit_freq[lit]++;
        }

        st->lookahead--;
      }

      // Flush final block
      if (use_fixed_huffman) {
        s = deflate_flush_fixed_block(st, 1);
      }
      else {
        s = deflate_flush_dynamic_block(st, 1);
      }
    }

    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    // Flush bitwriter to byte boundary
    s = gcomp_deflate_bitwriter_flush_to_byte(&st->bitwriter);
    if (s != GCOMP_OK) {
      gcomp_free(alloc, st->finish_buf);
      st->finish_buf = NULL;
      st->finish_buf_size = 0;
      return s;
    }

    // Record how many bytes were written
    st->finish_buf_used = gcomp_deflate_bitwriter_bytes_written(&st->bitwriter);
    st->finish_buf_ready = 1;
  }

  // Copy from finish buffer to user output
  size_t remaining = st->finish_buf_used - st->finish_buf_copied;
  size_t out_space = output->size - output->used;
  size_t to_copy = (remaining < out_space) ? remaining : out_space;

  if (to_copy > 0) {
    memcpy((uint8_t *)output->data + output->used,
        st->finish_buf + st->finish_buf_copied, to_copy);
    output->used += to_copy;
    st->finish_buf_copied += to_copy;
  }

  // Check if we've copied everything
  if (st->finish_buf_copied >= st->finish_buf_used) {
    // All done - clean up and mark complete
    gcomp_free(alloc, st->finish_buf);
    st->finish_buf = NULL;
    st->finish_buf_size = 0;
    st->finish_buf_used = 0;
    st->finish_buf_copied = 0;
    st->finish_buf_ready = 0;
    st->final_block_written = 1;
    st->stage = DEFLATE_ENC_STAGE_DONE;
    return GCOMP_OK;
  }

  // More data to copy - caller should call finish() again
  return GCOMP_ERR_LIMIT;
}
