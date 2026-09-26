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
 * @file zstd_internal.h
 *
 * Internal declarations for the Zstandard method implementation.
 *
 * This header is intended for use only by the Zstd method sources. It
 * exposes internal helpers used by the method registration vtable and
 * defines structures shared between encoder and decoder.
 *
 * ## Zstd Frame Format Overview
 *
 * A Zstandard frame consists of:
 * 1. Magic number (4 bytes): 0xFD2FB528
 * 2. Frame header (2-14 bytes):
 *    - Frame Header Descriptor (1 byte)
 *    - Window Descriptor (0 or 1 byte)
 *    - Dictionary ID (0, 1, 2, or 4 bytes)
 *    - Frame Content Size (0, 1, 2, 4, or 8 bytes)
 * 3. Data blocks (variable):
 *    - Block header (3 bytes)
 *    - Block data (variable)
 * 4. Content checksum (0 or 4 bytes, if flag set)
 *
 * Reference:
 * https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md
 */

#ifndef GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_INTERNAL_H
#define GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_INTERNAL_H

#include <ghoti.io/compress/macros.h>

#include "../../core/stepdown.h"

#include "../../core/alloc_internal.h"
#include "../../core/endian.h"
#include "../../core/fastcopy.h"
#include "../../core/registry_internal.h"
#include <ghoti.io/cutil/safemath.h>
#include "../../core/seek_table.h"
#include "../../core/stream_internal.h"
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/limits.h>
#include <ghoti.io/compress/zstd.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/xxhash64.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//
// Zstd Frame Format Constants
//

#define ZSTD_MAGIC 0xFD2FB528U      ///< Zstd frame magic number
#define ZSTD_DICT_MAGIC 0xEC30A437U ///< Zstd dictionary magic (RFC 8878 §5)
#define ZSTD_MAGIC_SKIPPABLE_MIN 0x184D2A50U ///< Skippable frame magic (min)
#define ZSTD_MAGIC_SKIPPABLE_MAX 0x184D2A5FU ///< Skippable frame magic (max)
#define ZSTD_HEADER_MIN_SIZE 2               ///< FHD(1) + min window/size
#define ZSTD_HEADER_MAX_SIZE 14      ///< FHD(1) + window(1) + dict(4) + fcs(8)
#define ZSTD_BLOCK_HEADER_SIZE 3     ///< Block header size
#define ZSTD_FRAME_HEADER_MIN_SIZE 5 ///< Magic_Number and the descriptor
#define ZSTD_CONTENT_CHECKSUM_SIZE 4 ///< Content checksum (xxHash64 low 32)

// Frame Header Descriptor bits
#define ZSTD_FHD_DICT_ID_FLAG_MASK 0x03   ///< Dictionary ID flag (2 bits)
#define ZSTD_FHD_CHECKSUM_FLAG 0x04       ///< Content checksum flag
#define ZSTD_FHD_RESERVED_BIT 0x08        ///< Reserved (must be 0)
#define ZSTD_FHD_UNUSED_BIT 0x10          ///< Unused (must be 0)
#define ZSTD_FHD_SINGLE_SEGMENT_FLAG 0x20 ///< Single segment flag
#define ZSTD_FHD_FCS_FLAG_MASK 0xC0       ///< Frame content size flag (2 bits)
#define ZSTD_FHD_FCS_FLAG_SHIFT 6

// Block header bits (per RFC 8878 Section 3.1.1.2)
// Bit 0: Last_Block flag
// Bits 1-2: Block_Type (0=raw, 1=RLE, 2=compressed, 3=reserved)
// Bits 3-23: Block_Size (21 bits)
#define ZSTD_BLOCK_LAST_FLAG 0x01 ///< Last block flag (bit 0)
#define ZSTD_BLOCK_TYPE_MASK 0x06 ///< Block type (bits 1-2)
#define ZSTD_BLOCK_TYPE_SHIFT 1
#define ZSTD_BLOCK_SIZE_MASK 0xFFFFF8 ///< Block size (bits 3-23)
#define ZSTD_BLOCK_SIZE_SHIFT 3

// Block types
#define ZSTD_BLOCK_TYPE_RAW 0        ///< Raw uncompressed block
#define ZSTD_BLOCK_TYPE_RLE 1        ///< RLE block (single repeated byte)
#define ZSTD_BLOCK_TYPE_COMPRESSED 2 ///< Compressed block
#define ZSTD_BLOCK_TYPE_RESERVED 3   ///< Reserved (invalid)

// Window size constraints
#define ZSTD_WINDOW_LOG_MIN 10     ///< Minimum window log (1 KB)
#define ZSTD_WINDOW_LOG_MAX 31     ///< Maximum window log (2 GB)
#define ZSTD_WINDOW_LOG_DEFAULT 22 ///< Default window log (4 MB)

// Block size constraints
#define ZSTD_BLOCK_SIZE_MAX 131072 ///< Maximum block size (128 KB)

/**
 * @brief Spare bytes past the end of a decode buffer, for overshooting into.
 *
 * The sequence loop copies literals and matches with fixed-width moves and
 * lets the excess fall past the length it was asked for; see fastcopy.h.  A
 * buffer it writes to, and a buffer it reads from, is allocated this much
 * larger than its capacity says, and the capacity itself does not count it -
 * so every bound the decoder checks is the real one and the slack is only
 * ever reached by an overshoot.
 *
 * The two buffers this applies to are zstd_decoder_state_t::output_buffer and
 * the literals buffer in zstd_block_decompress_compressed().
 */
#define ZSTD_DECODE_SLACK GCOMP_FASTCOPY_SLACK

// Compression level constraints
/**
 * @brief Lowest compression level.
 *
 * Zero is the fast strategy: one hash probe, no chain, and positions skipped
 * on a miss.  It was refused by the option schema until it meant something,
 * so nothing that used to work now means something else.
 *
 * libzstd spells this differently -- there level 0 means "use the default"
 * and the fast modes are negative.  This library has no use for a spelling
 * of "default" (the option carries its own, applied when it is absent), so
 * zero is spent on the strategy instead.
 */
#define ZSTD_LEVEL_MIN 0
#define ZSTD_LEVEL_MAX 22    ///< Maximum compression level
#define ZSTD_LEVEL_DEFAULT 3 ///< Default compression level

// Default limits
/**
 * Decode every frame in the input, per RFC 8878 section 3.1: "The decompressed
 * content of multiple concatenated frames is the concatenation of each frame's
 * decompressed content."  Set zstd.concat false to stop after the first frame.
 */
#define ZSTD_DEFAULT_CONCAT 1

#define ZSTD_DEFAULT_MAX_OUTPUT_BYTES (512ULL * 1024 * 1024) ///< 512 MiB
#define ZSTD_DEFAULT_MAX_MEMORY_BYTES (256ULL * 1024 * 1024) ///< 256 MiB
/// The format's own ceiling; see ::GCOMP_ZSTD_MAX_EXPANSION_RATIO.
#define ZSTD_DEFAULT_MAX_EXPANSION_RATIO GCOMP_ZSTD_MAX_EXPANSION_RATIO
#define ZSTD_DEFAULT_MAX_WINDOW_BYTES (1ULL << 27)           ///< 128 MiB

// Parallel compression constants
#define ZSTD_DEFAULT_JOB_SIZE (512 * 1024)   ///< Default job size: 512 KB
#define ZSTD_MIN_JOB_SIZE (64 * 1024)        ///< Minimum job size: 64 KB
#define ZSTD_MAX_JOB_SIZE (16 * 1024 * 1024) ///< Maximum job size: 16 MB
#define ZSTD_DEFAULT_MAX_IN_FLIGHT_MULT 2 ///< In-flight multiplier per thread

// Initial repeat offsets per specification
#define ZSTD_REP_OFFSET_1_INIT 1
#define ZSTD_REP_OFFSET_2_INIT 4
#define ZSTD_REP_OFFSET_3_INIT 8

//
// Encoder State Machine
//

typedef enum {
  ZSTD_ENC_STAGE_HEADER = 0, ///< Writing frame header
  ZSTD_ENC_STAGE_BLOCKS,     ///< Writing data blocks
  ZSTD_ENC_STAGE_CHECKSUM,   ///< Writing content checksum (if enabled)
  ZSTD_ENC_STAGE_DONE,       ///< Frame complete
  ZSTD_ENC_STAGE_ERROR,      ///< Unrecoverable error
} zstd_encoder_stage_t;

//
// Decoder State Machine
//

typedef enum {
  ZSTD_DEC_STAGE_HEADER = 0,       ///< Parsing frame header
  ZSTD_DEC_STAGE_BLOCK_HEADER,     ///< Reading 3-byte block header
  ZSTD_DEC_STAGE_BLOCK_DATA,       ///< Decompressing block content
  ZSTD_DEC_STAGE_CONTENT_CHECKSUM, ///< Reading content checksum (4 bytes)
  ZSTD_DEC_STAGE_SKIPPABLE,        ///< Discarding a skippable frame's payload
  ZSTD_DEC_STAGE_DONE,             ///< Frame complete
  ZSTD_DEC_STAGE_ERROR,            ///< Unrecoverable error
} zstd_decoder_stage_t;

//
// Header Parser State Machine (for streaming header parse)
//

typedef enum {
  ZSTD_HEADER_MAGIC = 0,    ///< Reading 4-byte magic number
  ZSTD_HEADER_DESCRIPTOR,   ///< Reading frame header descriptor byte
  ZSTD_HEADER_WINDOW,       ///< Reading window descriptor (if present)
  ZSTD_HEADER_DICT_ID,      ///< Reading dictionary ID (if present)
  ZSTD_HEADER_CONTENT_SIZE, ///< Reading content size (if present)
  ZSTD_HEADER_DONE,         ///< Header complete
} zstd_header_parse_stage_t;

//
// Frame Header Info Structure
//

typedef struct {
  uint8_t descriptor;        ///< Frame header descriptor byte
  bool single_segment;       ///< Single segment flag
  bool content_checksum;     ///< Content checksum flag
  bool content_size_present; ///< Frame content size present
  uint8_t dict_id_flag;      ///< Dictionary ID flag (0-3)
  uint8_t fcs_flag;          ///< Frame content size flag (0-3)
  uint8_t window_log;        ///< Window log (10-31)
  uint32_t window_size;      ///< Computed window size
  uint32_t dict_id;          ///< Dictionary ID (0 if not present)
  uint64_t content_size;     ///< Frame content size (0 if not present)
} zstd_frame_header_t;

//
// FSE Table Entry
//

typedef struct {
  uint16_t new_state; ///< New state after transition
  uint8_t nb_bits;    ///< Number of bits to read
  uint8_t symbol;     ///< Output symbol
} zstd_fse_entry_t;

//
// Huffman Decoding Table Entry
//

typedef struct {
  uint8_t symbol;  ///< Output symbol
  uint8_t nb_bits; ///< Number of bits consumed
} zstd_huf_entry_t;

//
// Match Finder Context (for encoder)
//

typedef struct {
  /**
   * @brief Hash of four bytes -> the latest position with that hash, plus one.
   *
   * Zero means "no entry", which is why every position stored here and in
   * @ref chain_table is one more than itself.
   *
   * POSITIONS ARE ABSOLUTE, in both tables and in @ref dict_hash_table: they
   * count from the start of the stream rather than from the front of the
   * caller's buffer, exactly as the tree's do.  Sliding the window is then one
   * addition to @ref base_pos.  Named from the front of the buffer, as they
   * were, a slide had to walk every entry of both tables and subtract - which
   * measured 12% of encoding at level 1 and 9.7% at level 3, spent entirely on
   * bookkeeping.
   */
  uint32_t * hash_table;

  /**
   * @brief Position -> the previous position with the same hash, plus one.
   *
   * A ring: the slot for a position is its absolute position masked with
   * @ref chain_mask, so an entry is overwritten only by one at least
   * @ref chain_size ahead of it, which is already further back than a
   * sequence may reach.  An entry that has fallen out of the window is
   * before @ref base_pos or before the window floor, and the walk stops at
   * it - which prunes the chain rather than corrupting it: fewer candidates,
   * never wrong ones.
   */
  uint32_t * chain_table;

  /**
   * @brief Where the chain enters the dictionary, per hash.  NULL if none.
   *
   * A copy of @ref hash_table taken once the dictionary has been indexed and
   * before any stream position has been, so each entry is the latest
   * dictionary position with that hash rather than the latest position of any
   * kind.  @ref chain_table is shared: a link from a dictionary position
   * already points at an earlier dictionary position, so following it from
   * here stays inside the dictionary.
   *
   * Only the chain levels allocate it.  The tree levels reach the dictionary
   * on their own - a descent is logarithmic, so a bounded number of steps
   * gets there - and levels 6 to 8 have chain budgets deep enough to walk
   * back into it unaided.
   */
  uint32_t * dict_hash_table;

  /**
   * @brief One past the last dictionary position, absolute.
   *
   * Zero when there is no dictionary.  Absolute like everything else here, so
   * a slide leaves it alone: whether any of the dictionary is still reachable
   * is decided by the same window floor that bounds the ordinary walk, rather
   * than by counting this down until it hits zero.
   */
  size_t dict_end;
  unsigned hash_log;      ///< Log2 of hash table size
  size_t hash_size;       ///< Hash table size
  size_t chain_size;      ///< Chain ring entries; a power of two.
  size_t chain_mask;      ///< chain_size - 1.
  unsigned search_depth;  ///< Maximum chain search depth
  unsigned lazy_depth;    ///< Positions a match may be deferred through (0 = greedy)
  uint32_t nice_length;   ///< Length at which a match is taken without looking further
  /**
   * @brief How the positions a match covers are indexed, at the chain levels.
   *
   * Dense for @ref fill_dense positions from the start of the match, then
   * every @ref fill_sparse -th.  Both are zero and one at the tree levels,
   * which sample by nice_length instead and never read these.  See the effort
   * table in zstd_matchfinder.c for what each shape cost and bought.
   */
  unsigned fill_dense;
  unsigned fill_sparse;

  /**
   * @brief One hash probe and no chain: the level 0 strategy.
   *
   * The chain levels keep two structures -- a head per hash and a link per
   * position -- and walk the links.  This keeps only the heads, so an
   * insertion is one store rather than two and a search is one probe rather
   * than a walk, and @ref chain_table is not allocated at all.
   */
  unsigned use_fast;
  size_t window_size;     ///< Window size for match offsets

  // Binary tree match finder, used instead of the chain at the levels whose
  // effort entry asks for it.  `bt_table` holds two children per position,
  // the smaller suffix at 2i and the larger at 2i+1, indexed by position
  // modulo `bt_size` -- a power of two at least as large as the window the
  // caller can present, so no two positions that are live at the same time
  // land in the same pair of slots.
  //
  // Positions here are absolute: counted from the start of the stream, not
  // from the front of the caller's buffer.  `base_pos` is the absolute
  // position of data[0], so sliding the window is one addition instead of a
  // pass over every entry rebasing it.
  uint32_t * bt_table;    ///< Two children per position; 0 means none.
  size_t bt_size;         ///< Positions the tree can hold; a power of two.
  size_t bt_mask;         ///< bt_size - 1.
  size_t base_pos;        ///< Absolute position of data[0].
  unsigned use_bt;        ///< Non-zero when this level uses the tree.

  // Optimal parse, used instead of the deferred one at the levels whose
  // effort entry asks for it.  It needs the tree -- it prices candidates
  // against one another and the chain cannot produce a list to price -- so
  // use_opt implies use_bt.  See zstd_optimal.c.
  struct zstd_opt_state_s * opt; ///< Cost model and parse table, or NULL.
  unsigned use_opt;              ///< Non-zero when this level parses optimally.
  uint32_t opt_segment;          ///< Positions one sweep covers.
  uint32_t opt_budget;           ///< Shortened matches one position may try.
  unsigned opt_two_pass;         ///< Parse each sweep twice (levels 20 and up).

  /**
   * @brief The last position put into the tree, plus one; 0 before any.
   *
   * Positions must go into the tree in increasing order and once each, or a
   * node becomes its own descendant.  The parses arrange that in different
   * ways -- the deferred one tracks which positions its look-ahead already
   * covered, the optimal one fills in the positions a forced match jumped
   * over -- and neither arrangement is obviously right by inspection, so a
   * test build checks the invariant itself on every insertion.
   *
   * Kept unconditionally so that the structure is the same shape whether or
   * not GCOMP_TEST_BUILD is defined; one store per insertion is far cheaper
   * than a layout that depends on a build flag.
   */
  size_t last_insert_abs;

  /**
   * @brief Bytes the TREE has compared, counted in test builds only.
   *
   * The comparison is what the tree spends nearly all its time on, and the
   * only thing that has ever made it pathological is comparing far more
   * than the answer needed.  Counting it lets a test say "this input must
   * not cost more than so much work" without measuring a clock.
   *
   * The chain does not count, and adding it would not make this a measure
   * of a level's cost: a chain candidate is a pointer chase and a compare
   * from the first byte, a tree candidate is a compare that starts where
   * the last one stopped, and the two are not the same unit.  Measured
   * across the levels of one file, the chain levels compare about 2 bytes
   * per input byte and the tree levels 44 to 127, while running at similar
   * speeds.
   *
   * It is not counted in a release build.  One add per candidate sounds
   * free and is not: it measured 0.87% of encoding at level 19, which is
   * too much to charge every caller for the benefit of one test.
   *
   * Kept unconditionally so that the structure is the same shape whether or
   * not GCOMP_TEST_BUILD is defined.
   */
  size_t compared_bytes;

  /**
   * @brief The long-distance index, or NULL when `zstd.long` is off.
   *
   * A second, far coarser table that covers the whole declared window where
   * the tables above reach only MF_MAX_DISTANCE. It is consulted before the
   * ordinary search at each position and its matches are preferred when they
   * are longer, which is the only way a match beyond about 8 MB is ever
   * found. See zstd_ldm.h.
   */
  struct zstd_ldm_s * ldm;

} zstd_match_finder_t;

//
// Sequence Structure (for encoder)
//

typedef struct {
  uint32_t lit_length;   ///< Literal length (bytes to copy from input)
  uint32_t match_offset; ///< Match offset (encoded, +3 for new offsets)
  uint32_t match_length; ///< Match length
} zstd_sequence_t;

//
// Parsed Zstd Dictionary (RFC 8878 Section 5)
//
typedef struct {
  uint32_t dict_id;
  const uint8_t * content;
  size_t content_size;
  bool has_entropy_tables;

  zstd_huf_entry_t * huf_table;
  size_t huf_table_size;
  unsigned huf_max_bits;

  zstd_fse_entry_t * fse_ll_table;
  zstd_fse_entry_t * fse_ml_table;
  zstd_fse_entry_t * fse_of_table;
  size_t fse_ll_size;
  size_t fse_ml_size;
  size_t fse_of_size;
  unsigned fse_ll_log;
  unsigned fse_ml_log;
  unsigned fse_of_log;

  uint32_t rep_offset_1;
  uint32_t rep_offset_2;
  uint32_t rep_offset_3;

  const gcomp_allocator_t * _allocator;
} zstd_dict_parsed_t;

gcomp_status_t zstd_dict_parse(const uint8_t * buf, size_t buf_size,
    const gcomp_allocator_t * alloc, zstd_dict_parsed_t * out);
void zstd_dict_destroy(zstd_dict_parsed_t * parsed);

//
// Encoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  zstd_encoder_stage_t stage;

  // Frame header configuration
  zstd_frame_header_t header;
  int compression_level;
  bool checksum_enabled;

  // Running content checksum (if enabled)
  gcomp_xxhash64_state_t content_hash;

  // Block buffer for collecting input
  uint8_t * block_buffer;       ///< Input buffer for current block
  size_t block_buffer_capacity; ///< Block buffer capacity
  size_t block_buffer_pos;      ///< Bytes buffered so far

  // Output staging buffers
  uint8_t header_buf[ZSTD_HEADER_MAX_SIZE + 4]; ///< Magic + header
  size_t header_len;                            ///< Total header length
  size_t header_pos;                            ///< Bytes written so far

  uint8_t checksum_buf[ZSTD_CONTENT_CHECKSUM_SIZE];
  size_t checksum_pos; ///< Bytes written so far

  // Compressed block output buffer
  uint8_t * compressed_buffer; ///< Compressed block output
  size_t compressed_buffer_capacity;
  size_t compressed_buffer_pos; ///< Read position for output
  size_t compressed_buffer_len; ///< Valid bytes in buffer

  // Match finder for compression
  zstd_match_finder_t * match_finder; ///< Match finder context

  // Sequence buffer for compression
  zstd_sequence_t * seq_buffer; ///< Sequence buffer
  size_t seq_buffer_capacity;   ///< Sequence buffer capacity

  // Literals buffer for compression
  uint8_t * literals_buffer;       ///< Literals buffer
  size_t literals_buffer_capacity; ///< Literals buffer capacity

  // Content tracking
  uint64_t total_input_bytes; ///< Total uncompressed bytes

  // Repeat offsets
  uint32_t rep_offset_1;
  uint32_t rep_offset_2;
  uint32_t rep_offset_3;

  // Dictionary (optional; from zstd.dictionary option)
  uint8_t * dict_bytes;
  size_t dict_size;
  zstd_dict_parsed_t dict_parsed;
  uint8_t * dict_block_buffer; ///< [dict_content][block] for match finder
  size_t dict_block_buffer_capacity;

  // Match finder window: the history a sequence can reach back into,
  // followed by the block being compressed.  Blocks share it, so a match may
  // point at data from an earlier block, which is what the window size in the
  // frame header promises the decoder (RFC 8878 section 3.1.1.1.2).
  uint8_t * mf_window;         ///< History followed by the current block
  size_t mf_window_capacity;   ///< mf_window_max + zstd_window_slack()
  /**
   * @brief Where the history begins inside mf_window.
   *
   * Dropping history moves this forward rather than moving the bytes back; see
   * zstd_window_slack().  The history the match finder is given therefore
   * starts at `mf_window + mf_window_start`, not at `mf_window`, and only the
   * compaction in zstd_block.c and the paths that seed the buffer from a
   * dictionary may set it back to zero.
   */
  size_t mf_window_start;
  size_t mf_window_len;        ///< History bytes currently held
  size_t mf_window_max;        ///< Most history to keep; the declared window
  /// Times the history was moved to the front of mf_window.  A diagnostic:
  /// it must grow with input/slack, not with input/block_size.
  uint64_t window_compactions;

  // Memory tracking
  /**
   * @brief Times this encoder settled for a weaker encoding, and why.
   *
   * Read by tests, which assert that nothing was forced.  See
   * src/core/stepdown.h.
   */
  gcomp_stepdown_tally_t stepdowns;

  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;

  // ─────────────────────────────────────────────────────────────────────
  // Seekable streaming state (zstd.seekable)
  // ─────────────────────────────────────────────────────────────────────
  //
  // A seekable file is independent frames followed by a seek table, and each
  // frame declares its own decompressed size so the file stays indexable even
  // if the table is ignored. Declaring that size means knowing it before the
  // frame is written, so input is accumulated to seek_frame_size and then
  // encoded whole - the same shape as parallel mode, for the same reason.
  //
  // One frame's compressed bytes at a time sit in seek_out and are handed to
  // the caller across as many calls as it takes; the seek table goes through
  // the same buffer at finish.
  bool seekable;               ///< zstd.seekable is on.
  uint64_t seek_frame_size;    ///< Decompressed bytes per frame.
  gcomp_seek_table_t seek_table;
  gcomp_options_t * seek_frame_options; ///< Per-frame options; seekable off.

  uint8_t * seek_in;           ///< Input awaiting a frame.
  size_t seek_in_cap;
  size_t seek_in_len;

  uint8_t * seek_out;          ///< A finished frame, or the table.
  size_t seek_out_cap;
  size_t seek_out_len;
  size_t seek_out_pos;         ///< How much of it the caller has taken.

  bool seek_table_staged;      ///< The table has been put in seek_out.

  // Finish state
  bool finish_called;
  bool blocks_finished;
  bool parallel_epilogue_staged; ///< Frame terminator + checksum written

  // ─────────────────────────────────────────────────────────────────────
  // Parallel Compression State (when threads.count > 1)
  // ─────────────────────────────────────────────────────────────────────
  //
  // When parallel mode is active (parallel_ctx != NULL):
  // - Input is accumulated in parallel_job's buffer until job_size
  // - Full jobs are submitted to parallel_ctx for compression
  // - Each job produces an independent zstd frame
  // - Results are collected in order via parallel_output_buf
  // - Output is valid concatenated zstd frames
  //
  // When single-threaded (parallel_ctx == NULL):
  // - Uses block_buffer, match_finder for streaming compression
  // - Produces a single zstd frame
  //
  struct zstd_parallel_ctx_s *
      parallel_ctx; ///< Parallel context (NULL = single-threaded)
  struct zstd_parallel_job_s *
      parallel_job;     ///< Current job being filled with input
  uint32_t num_threads; ///< Thread count from options (0/1 = single-threaded)
  uint64_t job_size;    ///< Target bytes per parallel job
  uint8_t *
      parallel_output_buf; ///< Buffer for compressed frames awaiting output
  size_t parallel_output_buf_cap; ///< Allocated capacity of parallel_output_buf
  uint8_t * parallel_overlap_buf; ///< Tail of the job just submitted
  uint32_t parallel_overlap_cap;  ///< Capacity of parallel_overlap_buf
  uint32_t parallel_overlap_len;  ///< Bytes of it currently valid
  size_t
      parallel_output_buf_pos; ///< Current read position (bytes already output)
  size_t parallel_output_buf_len; ///< Valid bytes in buffer (write position)
} zstd_encoder_state_t;

//
// Decoder State Structure
//

typedef struct {
  // Allocator for memory management
  const gcomp_allocator_t * allocator;

  // Stage tracking
  zstd_decoder_stage_t stage;
  zstd_header_parse_stage_t header_stage;

  // Parsed frame header
  zstd_frame_header_t header;

  // Header parsing state
  uint8_t header_accum[ZSTD_HEADER_MAX_SIZE + 4]; ///< Magic + header
  size_t header_accum_pos;
  uint32_t skippable_remaining; ///< Bytes of a skippable frame still to discard
  unsigned frames_completed;    ///< Frames finished, skippable ones included
  int saw_data_frame;           ///< A non-skippable frame has completed
  size_t header_expected_len; ///< Expected total header length

  // Running content checksum (if enabled)
  gcomp_xxhash64_state_t content_hash;

  // Block state
  uint8_t block_header_buf[ZSTD_BLOCK_HEADER_SIZE];
  size_t block_header_buf_pos;
  uint32_t current_block_size;
  uint8_t current_block_type;
  bool current_block_last;
  size_t block_bytes_remaining;

  // Block data buffer (for compressed blocks)
  uint8_t * block_buffer;
  size_t block_buffer_capacity;
  size_t block_buffer_pos;

  // Decompressed block output buffer
  uint8_t * output_buffer;
  size_t output_buffer_capacity;
  size_t output_buffer_pos; ///< Read position
  size_t output_buffer_len; ///< Valid bytes

  // Content checksum accumulator
  uint8_t content_checksum_buf[ZSTD_CONTENT_CHECKSUM_SIZE];
  size_t content_checksum_buf_pos;

  // Window buffer for back-references
  uint8_t * window_buffer;
  size_t window_size;
  size_t window_capacity;
  size_t window_pos; ///< Write position (circular)

  // Repeat offsets
  uint32_t rep_offset_1;
  uint32_t rep_offset_2;
  uint32_t rep_offset_3;

  // FSE decoding tables
  zstd_fse_entry_t * fse_lit_table;
  zstd_fse_entry_t * fse_match_table;
  zstd_fse_entry_t * fse_offset_table;
  size_t fse_lit_table_size;
  size_t fse_match_table_size;
  size_t fse_offset_table_size;
  unsigned fse_ll_log; ///< Literal length FSE table log
  unsigned fse_ml_log; ///< Match length FSE table log
  unsigned fse_of_log; ///< Offset FSE table log

  /**
   * @brief Whether each sequence table holds a table at all.
   *
   * RFC 8878 section 3.1.1.3.2.1.1: Repeat_Mode re-uses the table from an
   * earlier block, or from a dictionary, and a block asking for one before
   * there is one is corrupt.  Nothing said so, and the allocation is not
   * zeroed, so such a block decoded against whatever the allocator handed
   * back - valgrind reports the read, and a single bit flip in a good frame
   * reaches it.
   */
  bool fse_ll_ready;
  bool fse_ml_ready;
  bool fse_of_ready;

  // Huffman decoding table
  zstd_huf_entry_t * huf_table;
  size_t huf_table_size;
  unsigned huf_max_bits; ///< Max bits for current Huffman table
  bool huf_table_valid;  ///< For treeless literals

  // Options
  bool concat_enabled;

  // Dictionary (optional; from zstd.dictionary option)
  uint8_t * dict_bytes; ///< Owned copy of dictionary buffer (NULL if none)
  size_t dict_size;     ///< Size of dict_bytes
  zstd_dict_parsed_t
      dict_parsed; ///< Parsed view (content points into dict_bytes)

  // Limit configuration
  uint64_t max_output_bytes;
  uint64_t max_expansion_ratio;
  uint64_t max_window_bytes;

  // Limit tracking
  uint64_t total_input_bytes;
  uint64_t total_output_bytes;
  uint64_t frame_output_bytes; ///< Output bytes for current frame (for content
                               ///< size check)

  // Memory tracking
  gcomp_memory_tracker_t mem_tracker;
  uint64_t max_memory_bytes;
} zstd_decoder_state_t;

//
// Internal API: Encoder
//

/**
 * @brief Initialize Zstd encoder state.
 */
gcomp_status_t zstd_encoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_encoder_t * encoder);

/**
 * @brief Destroy Zstd encoder state.
 */
void zstd_encoder_destroy(gcomp_encoder_t * encoder);

/**
 * @brief Zstd encoder update implementation.
 */
gcomp_status_t zstd_encoder_update(
    gcomp_encoder_t * encoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Zstd encoder finish implementation.
 */
gcomp_status_t zstd_encoder_finish(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output);

/**
 * @brief Reset Zstd encoder to initial state.
 */
/**
 * @brief Emit the block being filled, without ending the frame.
 *
 * See gcomp_encoder_flush().  A zstd frame is a run of blocks, so a flushed
 * block is an ordinary one and the frame stays open.  GCOMP_FLUSH_FULL
 * additionally empties the match finder and resets the repeat offsets, so no
 * sequence after the flush names a distance into what came before.
 *
 * In parallel mode a job is a whole frame, so a flush closes the frame in
 * hand -- the same concatenated stream parallel mode always produces.
 */
gcomp_status_t zstd_encoder_flush(
    gcomp_encoder_t * encoder, gcomp_buffer_t * output, gcomp_flush_t mode);

gcomp_status_t zstd_encoder_reset(gcomp_encoder_t * encoder);

//
// Internal API: Decoder
//

/**
 * @brief Initialize Zstd decoder state.
 */
gcomp_status_t zstd_decoder_init(gcomp_registry_t * registry,
    gcomp_options_t * options, gcomp_decoder_t * decoder);

/**
 * @brief Destroy Zstd decoder state.
 */
void zstd_decoder_destroy(gcomp_decoder_t * decoder);

/**
 * @brief Zstd decoder update implementation.
 */
gcomp_status_t zstd_decoder_update(
    gcomp_decoder_t * decoder, gcomp_buffer_t * input, gcomp_buffer_t * output);

/**
 * @brief Zstd decoder finish implementation.
 */
gcomp_status_t zstd_decoder_finish(
    gcomp_decoder_t * decoder, gcomp_buffer_t * output);

/**
 * @brief Reset Zstd decoder to initial state.
 */
gcomp_status_t zstd_decoder_reset(gcomp_decoder_t * decoder);

//
// Internal API: Frame Format Helpers
//

/**
 * @brief Build Zstd frame header into buffer.
 *
 * @param header Frame header configuration
 * @param buf Output buffer
 * @param buf_size Buffer capacity
 * @param header_len_out Output: actual header length written
 * @return GCOMP_OK on success
 */
/**
 * @brief Size of the Dictionary_ID field a descriptor's flag selects.
 *
 * RFC 8878 section 3.1.1.1.3.
 */
size_t zstd_dict_id_size(uint8_t flag);

/**
 * @brief Size of the Frame_Content_Size field a descriptor selects.
 *
 * RFC 8878 section 3.1.1.1.4.  Single_Segment_Flag changes the meaning of a
 * flag of zero from "absent" to "one byte", which is why it is a parameter.
 */
size_t zstd_fcs_size(uint8_t flag, bool single_segment);

/**
 * @brief Total bytes of Magic_Number and Frame_Header for this descriptor.
 *
 * Between 5 and 18.  Lets a streaming reader know how much to accumulate after
 * it has seen the descriptor byte, without a second reading of which optional
 * fields are present.
 */
size_t zstd_frame_header_length(uint8_t fhd);

/**
 * @brief Read a frame header, and only read it.
 *
 * Shared by the decoder and by gcomp_peek(), so that what a caller is shown
 * and what the decoder acts on cannot disagree.  Nothing here is policy: an
 * enormous declared window is reported faithfully and left for the caller to
 * accept or refuse, and a named dictionary is reported without any view on
 * whether one is available.
 *
 * @param buf Start of the frame
 * @param buf_size How much of it is available
 * @param header_out Receives the parsed fields
 * @param window_size_out Receives Window_Size in full, which may exceed what a
 *        uint32_t holds and what @ref zstd_frame_header_t can carry
 * @param needed_out Receives the header length on success, or how many bytes
 *        are needed when there are not enough yet
 * @return ::GCOMP_OK; ::GCOMP_ERR_LIMIT when more input is needed;
 *         ::GCOMP_ERR_CORRUPT for a bad magic number or a set reserved bit
 */
gcomp_status_t zstd_frame_header_parse(const uint8_t * buf, size_t buf_size,
    zstd_frame_header_t * header_out, uint64_t * window_size_out,
    size_t * needed_out);

gcomp_status_t zstd_write_frame_header(const zstd_frame_header_t * header,
    uint8_t * buf, size_t buf_size, size_t * header_len_out);

/**
 * @brief Parse block header.
 *
 * @param buf 3-byte buffer containing block header
 * @param last_out Output: true if last block
 * @param type_out Output: block type (0-3)
 * @param size_out Output: block data size
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_parse_block_header(const uint8_t * buf, bool * last_out,
    uint8_t * type_out, uint32_t * size_out);

/**
 * @brief Write block header.
 *
 * @param buf Output buffer (must be at least 3 bytes)
 * @param last True if last block
 * @param type Block type (0-3)
 * @param size Block data size
 */
void zstd_write_block_header(
    uint8_t * buf, bool last, uint8_t type, uint32_t size);

/**
 * @brief Compute window size from window log.
 *
 * @param window_log Window log (10-31)
 * @return Window size in bytes
 */
uint32_t zstd_window_log_to_size(uint8_t window_log);

/**
 * @brief How much room to leave past one window in the match window buffer.
 *
 * The buffer holds one window of history followed by the block being
 * compressed, and history that falls out of the window has to be dropped from
 * the front.  Dropping it by moving the bytes costs the whole window every
 * time, and that happened once per block: at window_log 25 a 128 KB block into
 * a 32 MB window is 256 bytes moved for every byte compressed.  Leaving room
 * past the window instead lets the history start walking forward inside the
 * buffer, so the bytes move only when the room runs out - once per `slack`
 * bytes of input rather than once per block.
 *
 * **UNIT: MiB of uncompressed input per second**, 64 MB of an 8 KB pattern -
 * the shape that shows this, because its own compression is nearly free, so the
 * memmove is what is left.  Best of five in one process; the two libraries were
 * built separately and run alternately, so machine load cancels.  Output is
 * byte for byte identical either way, at every window_log and thread count.
 *
 *     window_log  window    one thread        four threads
 *                           before  after     before  after
 *     17          128 KB     2074   2085       1804   2050    (unchanged)
 *     19          512 KB     1712   1732       1594   1683    (unchanged)
 *     21            2 MB     1175   1620       1008   1209
 *     23            8 MB      478   1323        302    747
 *     25           32 MB      126   1340        109    400
 *
 * Nothing changes below window_log 21 because a quarter of a window is smaller
 * than a block there and the floor below wins; 21 is where the slack first
 * exceeds one block.  At window_log 25 the 64 MB case moved 8 GiB, and removing
 * that was 91% of the encode.
 *
 * **A quarter is the figure, and it was measured rather than reasoned.**  More
 * slack is monotonically faster on this shape and costs memory in proportion,
 * so there is no knee to find - only a trade to price.  One library per divisor,
 * one thread:
 *
 *     slack    window_log 21   window_log 23   window_log 25   buffer
 *     none             1175             478             126    w + 1 block
 *     w/8              1260            1002            1042    +12.5%
 *     w/4              1506            1220            1233    +25%
 *     w/2              1619            1377            1360    +50%
 *     w                1771            1544            1452    +100%
 *
 * A quarter takes 90% of what a whole window of slack would at window_log 25,
 * for a quarter more buffer rather than twice as much, and on input with
 * realistic redundancy - where the match finder rather than the memmove is the
 * bottleneck - everything from w/8 up is within a few per cent of the best.
 * The window buffer is also the small allocation here: the chain table is four
 * bytes per window byte, so a quarter more window buffer is 6% more memory at
 * window_log 25, not 25%.  notes/compress/PERFORMANCE.md has the sweep.
 *
 * Never less than one block, because the buffer must always have room to append
 * the block being compressed - that is the invariant the stream path in
 * zstd_block.c relies on.
 *
 * @param window_max Declared window size in bytes
 * @param block_max Largest block the encoder will append
 * @return Bytes to allocate past @p window_max
 */
static inline size_t zstd_window_slack(size_t window_max, size_t block_max) {
  const size_t quarter = window_max / 4u;
  return quarter > block_max ? quarter : block_max;
}

/**
 * @brief Get window log from compression level.
 *
 * @param level Compression level (0-22)
 * @return Appropriate window log
 */
uint8_t zstd_level_to_window_log(int level);

//
// Internal API: Block Compression/Decompression
//

/**
 * @brief Decompress a raw block (copy).
 */
gcomp_status_t zstd_block_decompress_raw(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

/**
 * @brief Decompress an RLE block.
 */
gcomp_status_t zstd_block_decompress_rle(const uint8_t * input,
    size_t input_len, uint8_t * output, size_t output_cap,
    size_t * output_len_out, uint32_t regenerated_size);

/**
 * @brief Decompress a compressed block.
 *
 * @p output must own @ref ZSTD_DECODE_SLACK writable bytes past
 * @p output_cap, which the sequence loop overshoots its copies into; nothing
 * that lands there is part of the result.  zstd_decoder_state_t::output_buffer
 * is allocated that way.  The raw and RLE forms above have no such
 * requirement.
 */
gcomp_status_t zstd_block_decompress_compressed(zstd_decoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Compress a block.
 */
gcomp_status_t zstd_block_compress(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out);

//
// Internal API: FSE (Finite State Entropy)
//

/**
 * @brief Read FSE table header and decode normalized counts.
 *
 * @param src Source data
 * @param src_size Source size
 * @param norm_counts Output: normalized counts for each symbol
 * @param max_symbol_out Output: maximum symbol value
 * @param table_log_out Output: log2 of table size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_fse_read_table_header(const uint8_t * src, size_t src_size,
    int16_t * norm_counts, unsigned * max_symbol_out, unsigned * table_log_out,
    size_t * bytes_read_out);

/**
 * @brief Build complete FSE decoding table from bitstream.
 *
 * @param src Source data containing table header
 * @param src_size Source size
 * @param table Output: decoding table
 * @param table_capacity Table capacity (must be >= 1 << table_log)
 * @param table_log_out Output: log2 of table size
 * @param max_symbol_out Output: maximum symbol value
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_fse_build_decoding_table(const uint8_t * src,
    size_t src_size, zstd_fse_entry_t * table, size_t table_capacity,
    unsigned * table_log_out, unsigned * max_symbol_out,
    size_t * bytes_read_out);

/**
 * @brief Choose an Accuracy_Log for a distribution.
 *
 * Small inputs do not repay a large table, and the table must be at least as
 * large as the alphabet.  Result is clamped to [5, max_table_log].
 *
 * @param max_table_log Format limit for this symbol type (RFC 8878 3.1.1.3.2.1:
 *                      9 for literal lengths and match lengths, 8 for offsets,
 *                      6 for Huffman weights).
 * @param num_values    How many symbols the table will encode.
 * @param max_symbol    Highest symbol value present.
 */
GCOMP_INTERNAL_API unsigned zstd_fse_optimal_table_log(
    unsigned max_table_log, size_t num_values, unsigned max_symbol);

/**
 * @brief Normalize a frequency histogram into FSE normalized counts.
 *
 * RFC 8878 section 4.1.1: a normalized count is a symbol's number of table
 * slots, and the counts sum to exactly 2^table_log; -1 means "probability
 * below one slot" and consumes one slot.  Every symbol with a non-zero
 * frequency receives a non-zero count.
 *
 * @param freq       Frequency per symbol, indexed 0..max_symbol.
 * @param max_symbol Highest symbol value to consider.
 * @param total      Sum of freq[0..max_symbol]; must be non-zero.
 * @param table_log  Accuracy_Log to normalize to.
 * @param norm_out   Receives counts for 0..max_symbol.
 * @return GCOMP_OK, or GCOMP_ERR_LIMIT if the alphabet cannot fit the table.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_fse_normalize_counts(
    const uint32_t * freq, unsigned max_symbol, uint64_t total,
    unsigned table_log, int16_t * norm_out);

/**
 * @brief Write FSE table header from normalized counts (for encoding).
 *
 * Used by the Huffman encoder when the weight table has >127 symbols
 * (FSE-compressed weights). Produces the same bit format that
 * zstd_fse_read_table_header() expects.
 */
gcomp_status_t zstd_fse_write_table_header(uint8_t * dst, size_t dst_cap,
    const int16_t * norm_counts, unsigned max_symbol, unsigned table_log,
    size_t * bytes_written_out);

/**
 * @brief Build FSE decoding table from normalized counts (for encoding).
 *
 * Used with zstd_fse_write_table_header() when encoding Huffman weight
 * sequences with FSE. The encoder uses the table to drive the FSE state
 * machine in reverse order.
 */
gcomp_status_t zstd_fse_build_table_from_norm(const int16_t * norm_counts,
    unsigned max_symbol, unsigned table_log, zstd_fse_entry_t * table);

/**
 * @brief Build predefined literal length FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_ll_table(
    zstd_fse_entry_t * table, size_t table_capacity);

/**
 * @brief Build predefined match length FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_ml_table(
    zstd_fse_entry_t * table, size_t table_capacity);

/**
 * @brief Build predefined offset FSE table.
 */
gcomp_status_t zstd_fse_build_predefined_of_table(
    zstd_fse_entry_t * table, size_t table_capacity);

//
// Internal API: Huffman Decoding
//

/**
 * @brief Read Huffman table from bitstream.
 *
 * Parses the Huffman tree description and builds a decoding table.
 *
 * @param src Source data containing Huffman tree description
 * @param src_size Source size
 * @param table Output: decoding table (must be at least 2^11 entries)
 * @param table_capacity Table capacity
 * @param max_bits_out Output: maximum code length (table log)
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_read_table(const uint8_t * src, size_t src_size,
    zstd_huf_entry_t * table, size_t table_capacity, unsigned * max_bits_out,
    size_t * bytes_read_out);

//
// Internal API: Huffman Encoding
//
// The Huffman encoder compresses literal bytes by assigning shorter codes
// to more frequent symbols. It is used by zstd_literals_encode_compressed()
// when compression provides sufficient benefit over raw literals.
//
// Format requirements (RFC 8878):
//   - Maximum code length: 11 bits
//   - Weights encoded as 4-bit values (direct) or FSE-compressed
//   - Bitstream written backwards with marker bit
//
// Implementation choices (tunable heuristics):
//   - When to use Huffman vs raw (see zstd_literals.c)
//   - Tree building algorithm (two-queue, O(n log n))
//   - Code length limiting strategy (greedy redistribution)
//
// Typical usage:
//   1. Count symbol frequencies in input literals
//   2. Call zstd_huf_build_enc_table() to build encoding table
//   3. Call zstd_huf_write_weights() to write table description
//   4. Call zstd_huf_encode_1stream() to write compressed bitstream
//

/**
 * @brief Huffman encoding table entry.
 *
 * Maps a symbol (byte value) to its Huffman code.
 */
typedef struct {
  uint16_t code;   ///< Huffman code bits (canonical, right-aligned)
  uint8_t nb_bits; ///< Number of bits in code (1-11, or 0 if unused)
} zstd_huf_enc_entry_t;

/**
 * @brief Huffman encoder context.
 *
 * Holds the encoding table and metadata needed for Huffman compression.
 * Built from symbol frequencies via zstd_huf_build_enc_table().
 */
typedef struct {
  zstd_huf_enc_entry_t symbols[256]; ///< Encoding table: symbol -> code
  uint8_t weights[256];              ///< Weights for each symbol (for header)
  unsigned max_bits;                 ///< Maximum code length (1-11)
  unsigned num_symbols;              ///< Number of symbols with non-zero weight
} zstd_huf_enc_table_t;

/**
 * @brief Build Huffman encoding table from symbol frequencies.
 *
 * Builds the code that encodes these frequencies in the fewest bits of any
 * code whose longest code word fits Zstandard's 11-bit cap (RFC 8878 section
 * 4.2.1), and generates the canonical codes for it.  The cap is part of what
 * the shared builder in src/core/huffman_lengths.c solves, so there is no
 * separate limiting step here.
 *
 * The resulting table can be used for encoding via zstd_huf_encode_1stream()
 * and the weights can be written via zstd_huf_write_weights().
 *
 * @param alloc Allocator for scratch memory; NULL uses the default.
 * @param freq Symbol frequency array (256 entries, one per byte value).
 *             Symbols with freq[i] == 0 are not included in the tree.
 * @param table Output: encoding table with codes, weights, and metadata.
 * @return GCOMP_OK on success, GCOMP_ERR_INVALID_ARG if freq or table is NULL,
 *         GCOMP_ERR_MEMORY if scratch space cannot be allocated.
 */
gcomp_status_t zstd_huf_build_enc_table(const gcomp_allocator_t * alloc,
    const uint32_t * freq, zstd_huf_enc_table_t * table);

/**
 * @brief Write Huffman table description (weights) to output.
 *
 * Writes the Huffman tree in Zstd's weight format (RFC 8878 Section 4.2.1.2).
 *
 * - Direct (1..128 symbols): header_byte = 127 + num_weights (>= 128),
 *   then 4-bit weights packed high nibble first.
 * - FSE-compressed (>127 symbols): header_byte = compressed size (< 128),
 *   then FSE table header + backward FSE bitstream (see zstd_huf.c).
 *
 * @param table Encoding table with weights (from zstd_huf_build_enc_table).
 * @param output Output buffer.
 * @param output_cap Output buffer capacity.
 * @param output_len_out Output: number of bytes written.
 * @return GCOMP_OK on success,
 *         GCOMP_ERR_LIMIT if output buffer too small or FSE size >= 128.
 */
gcomp_status_t zstd_huf_write_weights(const zstd_huf_enc_table_t * table,
    uint8_t * output, size_t output_cap, size_t * output_len_out);

/**
 * @brief Encode literals using Huffman coding (single stream).
 *
 * Encodes literal bytes as a Huffman bitstream. The stream is written
 * forward, then a marker bit (1) is appended, and finally the bytes are
 * reversed. This matches Zstd's backward reading convention.
 *
 * The decoder reads the stream backwards, finding the marker bit in the
 * last byte to determine where data starts.
 *
 * @param table Encoding table (from zstd_huf_build_enc_table).
 * @param literals Input literal bytes to encode.
 * @param literals_size Number of literals (0 for empty stream).
 * @param output Output buffer.
 * @param output_cap Output buffer capacity.
 * @param output_len_out Output: number of bytes written.
 * @return GCOMP_OK on success,
 *         GCOMP_ERR_LIMIT if output buffer too small,
 *         GCOMP_ERR_CORRUPT if a literal has no code in the table.
 */
gcomp_status_t zstd_huf_encode_1stream(const zstd_huf_enc_table_t * table,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Encode literals using Huffman coding (4 streams).
 *
 * Used for large literal sections (>= 1024 bytes). Splits literals into
 * 4 roughly equal segments, encodes each independently, and writes a
 * jump table (3 × 2-byte LE offsets) followed by the 4 concatenated streams.
 *
 * @param table Encoding table (from zstd_huf_build_enc_table).
 * @param literals Input literal bytes to encode.
 * @param literals_size Number of literals.
 * @param output Output buffer.
 * @param output_cap Output buffer capacity.
 * @param output_len_out Output: number of bytes written.
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_encode_4streams(const zstd_huf_enc_table_t * table,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Decode a single Huffman stream.
 *
 * @param table Huffman decoding table
 * @param max_bits Table log (maximum code length)
 * @param src Source compressed data
 * @param src_size Source size
 * @param dst Destination buffer
 * @param dst_size Destination capacity
 * @param decoded_size_out Output: actual bytes decoded
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_decode_1stream(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size, uint8_t * dst,
    size_t dst_size, size_t * decoded_size_out);

/**
 * @brief Decode 4 interleaved Huffman streams.
 *
 * Used for large literal sections (> 1024 bytes typically).
 *
 * @param table Huffman decoding table
 * @param max_bits Table log (maximum code length)
 * @param src Source compressed data
 * @param src_size Source size
 * @param jump_table Array of 3 offsets for streams 2, 3, 4
 * @param dst Destination buffer
 * @param dst_size Destination capacity
 * @param decoded_size_out Output: actual bytes decoded
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_huf_decode_4streams(const zstd_huf_entry_t * table,
    unsigned max_bits, const uint8_t * src, size_t src_size,
    const uint32_t * jump_table, uint8_t * dst, size_t dst_size,
    size_t * decoded_size_out);

//
// Internal API: Literals Section Decoding
//

// RFC 8878 section 3.1.1.3.2.1: Number_of_Sequences is written in one, two
// or three bytes; the three-byte form encodes 0xFFFF + 0x7F00 at most.
#define ZSTD_MAX_NUM_SEQUENCES (0xFFFFu + 0x7F00u)

// Maximum Huffman table size
#define HUF_MAX_TABLE_SIZE 2048

/**
 * @brief Decode literals section from compressed block.
 *
 * Handles all four literals types: raw, RLE, compressed, treeless.
 *
 * @param state Decoder state (for Huffman table storage)
 * @param src Source data (literals section)
 * @param src_size Source size
 * @param dst Destination buffer for decoded literals
 * @param dst_capacity Destination capacity
 * @param regenerated_size_out Output: actual decoded size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, uint8_t * dst, size_t dst_capacity,
    size_t * regenerated_size_out, size_t * bytes_read_out);

/**
 * @brief Get size of literals section header.
 *
 * @param src Source data
 * @param src_size Source size
 * @return Header size in bytes, or 0 on error
 */
size_t zstd_literals_header_size(const uint8_t * src, size_t src_size);

//
// Internal API: Sequences Section Decoding
//

/**
 * @brief Decode sequences section and execute to produce output.
 *
 * @param state Decoder state (for FSE tables and repeat offsets)
 * @param src Source data (sequences section)
 * @param src_size Source size
 * @param literals Decoded literals from literals section
 * @param literals_size Size of literals buffer
 * @param dst Destination buffer for output, owning @ref ZSTD_DECODE_SLACK
 *            writable bytes past @p dst_capacity for copies to overshoot into
 * @param dst_capacity Destination capacity, not counting that slack
 * @param output_size_out Output: actual output size
 * @param bytes_read_out Output: bytes consumed from source
 * @return GCOMP_OK on success
 *
 * @p literals likewise owns @ref ZSTD_DECODE_SLACK readable bytes past
 * @p literals_size, because a literal copy reads the same fixed width it
 * writes.  See fastcopy.h for why the copies are shaped that way.
 */
gcomp_status_t zstd_sequences_decode(zstd_decoder_state_t * state,
    const uint8_t * src, size_t src_size, const uint8_t * literals,
    size_t literals_size, uint8_t * dst, size_t dst_capacity,
    size_t * output_size_out, size_t * bytes_read_out);

//
// Internal API: Encoder - Match Finding and Compression
//

/**
 * @brief Initialize match finder.
 */
gcomp_status_t zstd_mf_init(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, int level, size_t window_size,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Turn long-distance matching on for this match finder.
 *
 * Separate from zstd_mf_init() because it is off by default and costs a table
 * of its own: a caller that does not ask for it pays nothing, and one that
 * does can be told what it will cost first, through
 * zstd_ldm_memory_estimate().
 *
 * @param window_size The declared window; an LDM offset may not exceed it
 *   (RFC 8878 section 3.1.1.1.2).
 * @param min_match Bytes a long match must have, or 0 for the default.
 * @param hash_log Log2 of the table size, or 0 to size it from the window.
 * @param hash_rate_log Index one position in `1 << this`.
 */
gcomp_status_t zstd_mf_enable_ldm(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, size_t window_size, unsigned min_match,
    unsigned hash_log, unsigned hash_rate_log,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief What zstd_mf_init() will allocate, without allocating it.
 *
 * Exact, not approximate: it is computed from the same sizing zstd_mf_init()
 * uses, so a caller can answer limits.max_memory_bytes before building a
 * match finder rather than after. A window log the caller chose can ask for
 * gigabytes, and a limit that is checked once those gigabytes are allocated
 * has not limited anything.
 *
 * @param level Compression level, clamped the way zstd_mf_init() clamps it
 * @param window_size Bytes of history the match finder would search
 * @return Bytes zstd_mf_init() would allocate and track
 */
size_t zstd_mf_memory_estimate(int level, size_t window_size);

/**
 * @brief Destroy match finder.
 */
void zstd_mf_destroy(zstd_match_finder_t * mf, const gcomp_allocator_t * alloc,
    gcomp_memory_tracker_t * mem_tracker);

/**
 * @brief Reset match finder for a new stream.
 *
 * Forgets both the positions it has recorded and everything the optimal
 * parse has learned about what it is compressing.
 */
void zstd_mf_reset(zstd_match_finder_t * mf);

/**
 * @brief Forget every recorded position, keeping the cost model.
 *
 * For a new buffer within the same frame: the tables name positions in the
 * buffer they were given and have to go, but what the optimal parse has
 * learned about the data is still true.
 */
void zstd_mf_reset_positions(zstd_match_finder_t * mf);

/**
 * @brief Move every recorded position back by @p shift.
 *
 * Called when the match finder's window slides: positions are relative to the
 * start of the window, so moving the window moves them.  Entries naming bytes
 * that fell off the front are dropped.
 *
 * @param mf Match finder context.
 * @param shift Bytes the window moved.
 */
void zstd_mf_slide(zstd_match_finder_t * mf, size_t shift);

/**
 * @brief Index positions [from, to) without searching for matches.
 *
 * Used to make a dictionary's content available as match history before any
 * of the stream is encoded.
 *
 * @param mf Match finder context.
 * @param data Window contents.
 * @param from First position to index.
 * @param to One past the last position to index.
 * @param data_size Total bytes in @p data.
 */
/**
 * @brief Record where the dictionary ends, so its part of the chain can be
 *        entered directly.
 *
 * Call once, after indexing the dictionary and before indexing anything else:
 * the snapshot it takes is only correct while every head still names a
 * dictionary position.
 *
 * Does nothing for a tree level, which reaches the dictionary unaided, and
 * nothing when @p dict_end is zero.  Failing to allocate is not an error -
 * the dictionary is still in the shared chain and still found by a deep
 * enough search, so the encoder carries on without the shortcut.
 *
 * @param mf Match finder
 * @param alloc Allocator for the one extra table
 * @param dict_end One past the last dictionary position, in window coordinates
 *   (converted to an absolute position on the way in)
 * @param mem_tracker Optional; charged for the table
 */
/**
 * @brief Decode a whole Zstandard stream with several threads, or decline.
 *
 * The ::gcomp_method_s::decode_parallel hook. Only a stream of several frames
 * that each declare their size can be split; see zstd_decode_parallel.c.
 */
gcomp_status_t zstd_decode_parallel(gcomp_registry_t * registry,
    gcomp_options_t * options, const void * input, size_t input_size,
    void * output, size_t output_capacity, size_t * output_size_out);

void zstd_mf_note_dictionary(zstd_match_finder_t * mf,
    const gcomp_allocator_t * alloc, size_t dict_end,
    gcomp_memory_tracker_t * mem_tracker);

void zstd_mf_index_range(zstd_match_finder_t * mf, const uint8_t * data,
    size_t from, size_t to, size_t data_size);

/**
 * @brief Index `[from, to)` into the long-distance table, for history.
 *
 * zstd_mf_index_range() fills the hash, chain and binary-tree tables. The
 * long-distance table is not one of those and is filled only by the scan that
 * runs at the head of zstd_mf_generate_sequences(), over the block being
 * compressed. In a stream that is enough, because every block's scan indexes
 * its own positions and the history is simply the blocks already done.
 *
 * A parallel job is the case where it is not enough: its history arrives
 * pre-made, as an overlap copied into the window, and was scanned by some other
 * job's match finder or by none. Without this the long-distance table is empty
 * for everything before the job's own content, and no long match into the
 * history can be found - which is not a wrong answer, just never a match, so it
 * shows up as a ratio and not as a failure.
 *
 * Does nothing when long-distance matching is off.
 *
 * @param mf The match finder.
 * @param data The window.
 * @param from First position to index.
 * @param to One past the last.
 * @param data_size Bytes readable in @p data.
 * @return ::GCOMP_OK, or ::GCOMP_ERR_MEMORY from the scan's match list.
 */
gcomp_status_t zstd_mf_index_ldm_range(zstd_match_finder_t * mf,
    const uint8_t * data, size_t from, size_t to, size_t data_size);

/**
 * @brief Generate sequences from input data.
 *
 * When dict_prefix_size > 0, data = [dict_content][block]; the first
 * dict_prefix_size bytes are loaded into the hash/chain and sequences
 * are generated only for the block part (positions
 * dict_prefix_size..data_size).
 */
gcomp_status_t zstd_mf_generate_sequences(zstd_match_finder_t * mf,
    const uint8_t * data, size_t data_size, size_t start_pos,
    zstd_sequence_t * sequences, size_t max_sequences,
    size_t * num_sequences_out, uint8_t * literals_out,
    size_t * literals_size_out, uint32_t * rep_offset_1,
    uint32_t * rep_offset_2, uint32_t * rep_offset_3);

/**
 * @brief Encode literals section (raw mode).
 *
 * @param literals Literals data
 * @param literals_size Literals size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_encode_raw(const uint8_t * literals,
    size_t literals_size, uint8_t * output, size_t output_cap,
    size_t * output_len_out);

/**
 * @brief Encode literals section using Huffman compression.
 *
 * Builds a Huffman table from symbol frequencies and encodes
 * the literals as a compressed bitstream. Falls back to raw
 * encoding if compression doesn't provide benefit.
 *
 * @param literals Literals data
 * @param literals_size Literals size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: bytes written
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_literals_encode_compressed(
    const gcomp_allocator_t * alloc, gcomp_stepdown_tally_t * stepdowns,
    const uint8_t * literals, size_t literals_size, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

// Symbol alphabet sizes for the three sequence code types
// (RFC 8878 3.1.1.3.2.1 and the tables in 3.1.1.3.2.2).
#define ZSTD_SEQ_LL_CODES 36
#define ZSTD_SEQ_ML_CODES 53
#define ZSTD_SEQ_OF_CODES 32

// The distributions RFC 8878 section 3.1.1.3.2.2 fixes for Predefined mode,
// as normalized counts over a table of 2^6, 2^6 and 2^5 states.  A count of
// -1 is the spec's "less likely than one state in the table".
//
// The decoder builds tables from these.  The optimal parse reads them as a
// prior instead: before a block has been parsed there is nothing to measure,
// and the spec's own guess at what sequence statistics look like is a better
// starting point than assuming every code equally likely.
extern const int16_t zstd_ll_predefined_norm[ZSTD_SEQ_LL_CODES];
extern const int16_t zstd_ml_predefined_norm[ZSTD_SEQ_ML_CODES];
extern const int16_t zstd_of_predefined_norm[29];

/**
 * @brief Encode a block's sequences section.
 *
 * Chooses, per symbol type, between the predefined FSE table, an RLE table
 * when the block uses a single symbol, and a per-block FSE table -- whichever
 * encodes the block smallest.
 */
GCOMP_INTERNAL_API gcomp_status_t zstd_sequences_encode(
    const zstd_sequence_t * sequences, size_t num_sequences, uint8_t * output,
    size_t output_cap, size_t * output_len_out);

/**
 * @brief Compress a block using full LZ77 + entropy encoding.
 *
 * @param state Encoder state
 * @param input Input data
 * @param input_len Input size
 * @param output Output buffer
 * @param output_cap Output capacity
 * @param output_len_out Output: compressed size
 * @param type_out Output: block type
 * @return GCOMP_OK on success
 */
gcomp_status_t zstd_compress_block_full(zstd_encoder_state_t * state,
    const uint8_t * input, size_t input_len, uint8_t * output,
    size_t output_cap, size_t * output_len_out, uint8_t * type_out);


/**
 * @brief The encoder's record of when it settled for a weaker encoding.
 *
 * Exists so that a test can assert nothing was forced - see
 * src/core/stepdown.h for why that needs counting.  The tally accumulates
 * over the encoder's life and is cleared by a reset.
 *
 * @param encoder Encoder to read; NULL returns NULL.
 * @return The tally, owned by the encoder, or NULL if there is no state.
 */
const gcomp_stepdown_tally_t * gcomp_zstd_encoder_stepdowns(
    const gcomp_encoder_t * encoder);

/**
 * @brief Times the encoder moved its history to the front of the window buffer.
 *
 * Exists so that a test can assert the cost is amortised: the buffer holds a
 * window plus zstd_window_slack() bytes, so the bytes move once per slack bytes
 * of input.  Moving them once per block instead was 93% of the encode at
 * window_log 25, and a throughput assertion is not something a test suite can
 * make on a loaded machine - this count can be asserted exactly.
 *
 * Serial mode only.  A parallel job compresses into its own state and that
 * state is discarded, which is the same reason the stepdown tally is empty
 * above one thread.
 *
 * @param encoder Encoder to read; NULL returns 0.
 * @return Compactions since the encoder was created or last reset.
 */
uint64_t gcomp_zstd_encoder_window_compactions(
    const gcomp_encoder_t * encoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_SRC_METHODS_ZSTD_ZSTD_INTERNAL_H
