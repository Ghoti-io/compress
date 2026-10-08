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
 * @file
 * Renames every identifier of this library into its version namespace.
 *
 * Each line pastes the token from libver_gen.h onto the short name, so that
 * `gcomp_stream_create` becomes `ghotiio_compress_dev_gcomp_stream_create` by
 * the time the compiler sees it.  A program keeps writing the short name; two
 * builds of this library with different BRANCH values produce disjoint symbol
 * sets and can be loaded into one process.  See CONVENTIONS.md section 4.
 *
 * This is one file rather than a block per header on purpose.  A rename must
 * be seen before any declaration that uses the name, and an internal header
 * may name a public type; with the renames scattered across headers, one could
 * be read too late and produce two different types under one spelling.
 * Including this file first makes that impossible.
 *
 * Internal names are listed here too.  They are hidden by -fvisibility=hidden
 * and so cannot collide, but renaming them keeps one rule instead of two.
 *
 * Add a line here whenever you add a function, type, or variable.
 * `make check-symbols` fails the build if you forget.
 *
 * It checks the two in different ways, because it has to.  A function that is
 * not renamed shows up in `nm -D` without the namespace prefix, so the linker's
 * own output is the evidence.  A type has no linkage and produces no symbol at
 * all, so nothing in the built library can be inspected to find one missing;
 * that half is checked by reading the public headers and requiring every type
 * name and struct tag they declare to appear below.
 *
 * That gap was not theoretical.  `gcomp_seekable_t` and its `_s` tag went
 * un-renamed when seekable.h was added - every one of its eight functions was
 * listed here, and the type they all take was not - and check-symbols passed
 * on every run in between.
 */

#ifndef GHOTI_IO_GCOMP_NAMESPACE_H
#define GHOTI_IO_GCOMP_NAMESPACE_H

#include <ghoti.io/compress/libver.h>

/// @cond HIDDEN_SYMBOLS

// Public types, and the struct tags behind them. Renamed as well as the
// functions, so that two versions whose structs differ in layout cannot be
// confused for one another - which is the point of the scheme, and which
// renaming only the functions leaves undone. The `_s` tags are listed
// alongside their `_t` typedefs because an internal header may name the tag
// directly. GCU_* names are absent on purpose: they are cutil's, and cutil
// has already renamed them.
#define gcomp_allocator_t GHOTIIO_COMPRESS(gcomp_allocator_t)
#define gcomp_block_job_s GHOTIIO_COMPRESS(gcomp_block_job_s)
#define gcomp_block_job_t GHOTIIO_COMPRESS(gcomp_block_job_t)
#define gcomp_buffer_t GHOTIIO_COMPRESS(gcomp_buffer_t)
#define gcomp_capabilities_t GHOTIIO_COMPRESS(gcomp_capabilities_t)
#define gcomp_decoder_s GHOTIIO_COMPRESS(gcomp_decoder_s)
#define gcomp_decoder_t GHOTIIO_COMPRESS(gcomp_decoder_t)
#define gcomp_encoder_s GHOTIIO_COMPRESS(gcomp_encoder_s)
#define gcomp_encoder_t GHOTIIO_COMPRESS(gcomp_encoder_t)
#define gcomp_flush_t GHOTIIO_COMPRESS(gcomp_flush_t)
#define gcomp_job_complete_cb_t GHOTIIO_COMPRESS(gcomp_job_complete_cb_t)
#define gcomp_job_func_t GHOTIIO_COMPRESS(gcomp_job_func_t)
#define gcomp_job_status_t GHOTIIO_COMPRESS(gcomp_job_status_t)
#define gcomp_lz4_frame_info_t GHOTIIO_COMPRESS(gcomp_lz4_frame_info_t)
#define gcomp_lz4_skippable_cb GHOTIIO_COMPRESS(gcomp_lz4_skippable_cb)
#define gcomp_memory_tracker_t GHOTIIO_COMPRESS(gcomp_memory_tracker_t)
#define gcomp_method_s GHOTIIO_COMPRESS(gcomp_method_s)
#define gcomp_method_schema_s GHOTIIO_COMPRESS(gcomp_method_schema_s)
#define gcomp_method_schema_t GHOTIIO_COMPRESS(gcomp_method_schema_t)
#define gcomp_method_t GHOTIIO_COMPRESS(gcomp_method_t)
#define gcomp_option_schema_s GHOTIIO_COMPRESS(gcomp_option_schema_s)
#define gcomp_option_schema_t GHOTIIO_COMPRESS(gcomp_option_schema_t)
#define gcomp_option_type_t GHOTIIO_COMPRESS(gcomp_option_type_t)
#define gcomp_options_s GHOTIIO_COMPRESS(gcomp_options_s)
#define gcomp_options_t GHOTIIO_COMPRESS(gcomp_options_t)
#define gcomp_read_cb GHOTIIO_COMPRESS(gcomp_read_cb)
#define gcomp_registry_s GHOTIIO_COMPRESS(gcomp_registry_s)
#define gcomp_registry_t GHOTIIO_COMPRESS(gcomp_registry_t)
#define gcomp_seekable_s GHOTIIO_COMPRESS(gcomp_seekable_s)
#define gcomp_seekable_t GHOTIIO_COMPRESS(gcomp_seekable_t)
#define gcomp_stream_info_s GHOTIIO_COMPRESS(gcomp_stream_info_s)
#define gcomp_stream_info_t GHOTIIO_COMPRESS(gcomp_stream_info_t)
#define gcomp_status_t GHOTIIO_COMPRESS(gcomp_status_t)
#define gcomp_unknown_key_policy_t GHOTIIO_COMPRESS(gcomp_unknown_key_policy_t)
#define gcomp_write_cb GHOTIIO_COMPRESS(gcomp_write_cb)
#define gcomp_xxhash32_state_t GHOTIIO_COMPRESS(gcomp_xxhash32_state_t)
#define gcomp_xxhash64_state_t GHOTIIO_COMPRESS(gcomp_xxhash64_state_t)


#define gcomp_allocator_default GHOTIIO_COMPRESS(gcomp_allocator_default)
#define gcomp_adler32 GHOTIIO_COMPRESS(gcomp_adler32)
#define gcomp_adler32_combine GHOTIIO_COMPRESS(gcomp_adler32_combine)
#define gcomp_adler32_update GHOTIIO_COMPRESS(gcomp_adler32_update)
#define gcomp_crc32 GHOTIIO_COMPRESS(gcomp_crc32)
#define gcomp_crc32_finalize GHOTIIO_COMPRESS(gcomp_crc32_finalize)
#define gcomp_crc32_update GHOTIIO_COMPRESS(gcomp_crc32_update)
#define gcomp_decode_buffer GHOTIIO_COMPRESS(gcomp_decode_buffer)
#define gcomp_decoder_create GHOTIIO_COMPRESS(gcomp_decoder_create)
#define gcomp_decoder_destroy GHOTIIO_COMPRESS(gcomp_decoder_destroy)
#define gcomp_decoder_finish GHOTIIO_COMPRESS(gcomp_decoder_finish)
#define gcomp_decoder_get_error GHOTIIO_COMPRESS(gcomp_decoder_get_error)
#define gcomp_decoder_get_error_detail GHOTIIO_COMPRESS(gcomp_decoder_get_error_detail)
#define gcomp_decoder_reset GHOTIIO_COMPRESS(gcomp_decoder_reset)
#define gcomp_decoder_set_error GHOTIIO_COMPRESS(gcomp_decoder_set_error)
#define gcomp_decoder_update GHOTIIO_COMPRESS(gcomp_decoder_update)
#define gcomp_decode_stream_cb GHOTIIO_COMPRESS(gcomp_decode_stream_cb)
#define gcomp_deflate_bitreader_align_to_byte GHOTIIO_COMPRESS(gcomp_deflate_bitreader_align_to_byte)
#define gcomp_deflate_bitreader_init GHOTIIO_COMPRESS(gcomp_deflate_bitreader_init)
#define gcomp_deflate_bitreader_is_eof GHOTIIO_COMPRESS(gcomp_deflate_bitreader_is_eof)
#define gcomp_deflate_bitreader_read_bits GHOTIIO_COMPRESS(gcomp_deflate_bitreader_read_bits)
#define gcomp_deflate_distance_code GHOTIIO_COMPRESS(gcomp_deflate_distance_code)
#define gcomp_deflate_length_code GHOTIIO_COMPRESS(gcomp_deflate_length_code)
#define gcomp_deflate_bitwriter_bytes_written GHOTIIO_COMPRESS(gcomp_deflate_bitwriter_bytes_written)
#define gcomp_deflate_bitwriter_flush_to_byte GHOTIIO_COMPRESS(gcomp_deflate_bitwriter_flush_to_byte)
#define gcomp_deflate_bitwriter_init GHOTIIO_COMPRESS(gcomp_deflate_bitwriter_init)
#define gcomp_deflate_bitwriter_set_buffer GHOTIIO_COMPRESS(gcomp_deflate_bitwriter_set_buffer)
#define gcomp_deflate_bitwriter_write_bits GHOTIIO_COMPRESS(gcomp_deflate_bitwriter_write_bits)
#define gcomp_deflate_huffman_build_codes GHOTIIO_COMPRESS(gcomp_deflate_huffman_build_codes)
#define gcomp_deflate_huffman_build_decode_table GHOTIIO_COMPRESS(gcomp_deflate_huffman_build_decode_table)
#define gcomp_deflate_huffman_decode_table_cleanup GHOTIIO_COMPRESS(gcomp_deflate_huffman_decode_table_cleanup)
#define gcomp_deflate_huffman_validate GHOTIIO_COMPRESS(gcomp_deflate_huffman_validate)
#define gcomp_buffer_free GHOTIIO_COMPRESS(gcomp_buffer_free)
#define gcomp_decode_alloc GHOTIIO_COMPRESS(gcomp_decode_alloc)
#define gcomp_detect GHOTIIO_COMPRESS(gcomp_detect)
#define gcomp_dict_train GHOTIIO_COMPRESS(gcomp_dict_train)
#define gcomp_encode_alloc GHOTIIO_COMPRESS(gcomp_encode_alloc)
#define gcomp_encode_bound GHOTIIO_COMPRESS(gcomp_encode_bound)
#define gcomp_peek GHOTIIO_COMPRESS(gcomp_peek)
#define gcomp_seek_cb GHOTIIO_COMPRESS(gcomp_seek_cb)
#define gcomp_seekable_open_buffer GHOTIIO_COMPRESS(gcomp_seekable_open_buffer)
#define gcomp_seekable_open_cb GHOTIIO_COMPRESS(gcomp_seekable_open_cb)
#define gcomp_seekable_size GHOTIIO_COMPRESS(gcomp_seekable_size)
#define gcomp_seekable_frame_count GHOTIIO_COMPRESS(gcomp_seekable_frame_count)
#define gcomp_seekable_has_table GHOTIIO_COMPRESS(gcomp_seekable_has_table)
#define gcomp_seekable_read GHOTIIO_COMPRESS(gcomp_seekable_read)
#define gcomp_seekable_close GHOTIIO_COMPRESS(gcomp_seekable_close)
#define gcomp_seekable_write_bound GHOTIIO_COMPRESS(gcomp_seekable_write_bound)
#define gcomp_seekable_write_buffer GHOTIIO_COMPRESS(gcomp_seekable_write_buffer)
#define gcomp_encode_buffer GHOTIIO_COMPRESS(gcomp_encode_buffer)
#define gcomp_encoder_create GHOTIIO_COMPRESS(gcomp_encoder_create)
#define gcomp_encoder_destroy GHOTIIO_COMPRESS(gcomp_encoder_destroy)
#define gcomp_encoder_finish GHOTIIO_COMPRESS(gcomp_encoder_finish)
#define gcomp_encoder_get_error GHOTIIO_COMPRESS(gcomp_encoder_get_error)
#define gcomp_encoder_get_error_detail GHOTIIO_COMPRESS(gcomp_encoder_get_error_detail)
#define gcomp_encoder_flush GHOTIIO_COMPRESS(gcomp_encoder_flush)
#define gcomp_encoder_reset GHOTIIO_COMPRESS(gcomp_encoder_reset)
#define gcomp_encoder_set_error GHOTIIO_COMPRESS(gcomp_encoder_set_error)
#define gcomp_encoder_update GHOTIIO_COMPRESS(gcomp_encoder_update)
#define gcomp_encode_stream_cb GHOTIIO_COMPRESS(gcomp_encode_stream_cb)
#define gcomp_limits_check_expansion_ratio GHOTIIO_COMPRESS(gcomp_limits_check_expansion_ratio)
#define gcomp_limits_check_memory GHOTIIO_COMPRESS(gcomp_limits_check_memory)
#define gcomp_limits_check_output GHOTIIO_COMPRESS(gcomp_limits_check_output)
#define gcomp_limits_read_expansion_ratio_max GHOTIIO_COMPRESS(gcomp_limits_read_expansion_ratio_max)
#define gcomp_limits_read_memory_max GHOTIIO_COMPRESS(gcomp_limits_read_memory_max)
#define gcomp_limits_read_output_max GHOTIIO_COMPRESS(gcomp_limits_read_output_max)
#define gcomp_limits_read_window_max GHOTIIO_COMPRESS(gcomp_limits_read_window_max)
#define gcomp_memory_check_limit GHOTIIO_COMPRESS(gcomp_memory_check_limit)
#define gcomp_memory_track_alloc GHOTIIO_COMPRESS(gcomp_memory_track_alloc)
#define gcomp_memory_track_free GHOTIIO_COMPRESS(gcomp_memory_track_free)
#define gcomp_method_brotli_register GHOTIIO_COMPRESS(gcomp_method_brotli_register)
#define gcomp_method_deflate_register GHOTIIO_COMPRESS(gcomp_method_deflate_register)
#define gcomp_method_lzma_register GHOTIIO_COMPRESS(gcomp_method_lzma_register)
#define gcomp_method_lzma2_register GHOTIIO_COMPRESS(gcomp_method_lzma2_register)
#define gcomp_method_bzip2_register GHOTIIO_COMPRESS(gcomp_method_bzip2_register)
#define gcomp_method_delta_register GHOTIIO_COMPRESS(gcomp_method_delta_register)
#define gcomp_method_bcj_register GHOTIIO_COMPRESS(gcomp_method_bcj_register)
#define gcomp_method_xz_register GHOTIIO_COMPRESS(gcomp_method_xz_register)
#define gcomp_method_get_all_schemas GHOTIIO_COMPRESS(gcomp_method_get_all_schemas)
#define gcomp_method_zlib_register GHOTIIO_COMPRESS(gcomp_method_zlib_register)
#define gcomp_zlib_header_info_t GHOTIIO_COMPRESS(gcomp_zlib_header_info_t)
#define gcomp_zlib_peek_header GHOTIIO_COMPRESS(gcomp_zlib_peek_header)
#define gcomp_method_get_option_keys GHOTIIO_COMPRESS(gcomp_method_get_option_keys)
#define gcomp_method_get_option_schema GHOTIIO_COMPRESS(gcomp_method_get_option_schema)
#define gcomp_method_gzip_register GHOTIIO_COMPRESS(gcomp_method_gzip_register)
#define gcomp_lz4_decoder_on_skippable_frame \
  GHOTIIO_COMPRESS(gcomp_lz4_decoder_on_skippable_frame)
#define gcomp_lz4_peek_frame_info GHOTIIO_COMPRESS(gcomp_lz4_peek_frame_info)
#define gcomp_lz4_read_skippable_frame \
  GHOTIIO_COMPRESS(gcomp_lz4_read_skippable_frame)
#define gcomp_lz4_write_skippable_frame \
  GHOTIIO_COMPRESS(gcomp_lz4_write_skippable_frame)
#define gcomp_method_lz4_register GHOTIIO_COMPRESS(gcomp_method_lz4_register)
#define gcomp_method_lzw_register GHOTIIO_COMPRESS(gcomp_method_lzw_register)
#define gcomp_method_rle_register GHOTIIO_COMPRESS(gcomp_method_rle_register)
#define gcomp_method_zstd_register GHOTIIO_COMPRESS(gcomp_method_zstd_register)
#define gcomp_options_clone GHOTIIO_COMPRESS(gcomp_options_clone)
#define gcomp_options_create GHOTIIO_COMPRESS(gcomp_options_create)
#define gcomp_options_create_with_allocator GHOTIIO_COMPRESS(gcomp_options_create_with_allocator)
#define gcomp_options_destroy GHOTIIO_COMPRESS(gcomp_options_destroy)
#define gcomp_options_freeze GHOTIIO_COMPRESS(gcomp_options_freeze)
#define gcomp_options_get_bool GHOTIIO_COMPRESS(gcomp_options_get_bool)
#define gcomp_options_get_bytes GHOTIIO_COMPRESS(gcomp_options_get_bytes)
#define gcomp_options_get_int64 GHOTIIO_COMPRESS(gcomp_options_get_int64)
#define gcomp_options_get_string GHOTIIO_COMPRESS(gcomp_options_get_string)
#define gcomp_options_get_uint64 GHOTIIO_COMPRESS(gcomp_options_get_uint64)
#define gcomp_options_set_bool GHOTIIO_COMPRESS(gcomp_options_set_bool)
#define gcomp_options_set_bytes GHOTIIO_COMPRESS(gcomp_options_set_bytes)
#define gcomp_options_set_int64 GHOTIIO_COMPRESS(gcomp_options_set_int64)
#define gcomp_options_set_string GHOTIIO_COMPRESS(gcomp_options_set_string)
#define gcomp_options_set_uint64 GHOTIIO_COMPRESS(gcomp_options_set_uint64)
#define gcomp_options_validate GHOTIIO_COMPRESS(gcomp_options_validate)
#define gcomp_options_validate_key GHOTIIO_COMPRESS(gcomp_options_validate_key)
#define gcomp_registry_create GHOTIIO_COMPRESS(gcomp_registry_create)
#define gcomp_registry_default GHOTIIO_COMPRESS(gcomp_registry_default)
#define gcomp_registry_destroy GHOTIIO_COMPRESS(gcomp_registry_destroy)
#define gcomp_registry_find GHOTIIO_COMPRESS(gcomp_registry_find)
#define gcomp_registry_register GHOTIIO_COMPRESS(gcomp_registry_register)
#define gcomp_status_to_string GHOTIIO_COMPRESS(gcomp_status_to_string)
#define gcomp_version_major GHOTIIO_COMPRESS(gcomp_version_major)
#define gcomp_version_minor GHOTIIO_COMPRESS(gcomp_version_minor)
#define gcomp_version_patch GHOTIIO_COMPRESS(gcomp_version_patch)
#define gcomp_version_string GHOTIIO_COMPRESS(gcomp_version_string)
#define gcomp_xxhash32 GHOTIIO_COMPRESS(gcomp_xxhash32)
#define gcomp_xxhash32_finalize GHOTIIO_COMPRESS(gcomp_xxhash32_finalize)
#define gcomp_xxhash32_reset GHOTIIO_COMPRESS(gcomp_xxhash32_reset)
#define gcomp_xxhash32_update GHOTIIO_COMPRESS(gcomp_xxhash32_update)
#define gcomp_xxhash64 GHOTIIO_COMPRESS(gcomp_xxhash64)
#define gcomp_xxhash64_finalize GHOTIIO_COMPRESS(gcomp_xxhash64_finalize)
#define gcomp_xxhash64_reset GHOTIIO_COMPRESS(gcomp_xxhash64_reset)
#define gcomp_xxhash64_update GHOTIIO_COMPRESS(gcomp_xxhash64_update)
#define lz4_parallel_alloc_job GHOTIIO_COMPRESS(lz4_parallel_alloc_job)
#define lz4_parallel_create GHOTIIO_COMPRESS(lz4_parallel_create)
#define lz4_parallel_destroy GHOTIIO_COMPRESS(lz4_parallel_destroy)
#define lz4_parallel_free_job GHOTIIO_COMPRESS(lz4_parallel_free_job)
#define lz4_parallel_get_result GHOTIIO_COMPRESS(lz4_parallel_get_result)
#define lz4_parallel_is_inline GHOTIIO_COMPRESS(lz4_parallel_is_inline)
#define lz4_parallel_pending_count GHOTIIO_COMPRESS(lz4_parallel_pending_count)
#define lz4_parallel_reset GHOTIIO_COMPRESS(lz4_parallel_reset)
#define lz4_parallel_result_ready GHOTIIO_COMPRESS(lz4_parallel_result_ready)
#define lz4_parallel_submit GHOTIIO_COMPRESS(lz4_parallel_submit)
#define lz4_parallel_wait GHOTIIO_COMPRESS(lz4_parallel_wait)
#define lzw_bitreader_init GHOTIIO_COMPRESS(lzw_bitreader_init)
#define lzw_bitreader_read_bits GHOTIIO_COMPRESS(lzw_bitreader_read_bits)
#define lzw_bitwriter_bytes_written GHOTIIO_COMPRESS(lzw_bitwriter_bytes_written)
#define lzw_bitwriter_flush GHOTIIO_COMPRESS(lzw_bitwriter_flush)
#define lzw_bitwriter_init GHOTIIO_COMPRESS(lzw_bitwriter_init)
#define lzw_bitwriter_write_bits GHOTIIO_COMPRESS(lzw_bitwriter_write_bits)
#define lzw_core_decoder_decode GHOTIIO_COMPRESS(lzw_core_decoder_decode)
#define lzw_core_decoder_destroy GHOTIIO_COMPRESS(lzw_core_decoder_destroy)
#define lzw_core_decoder_init GHOTIIO_COMPRESS(lzw_core_decoder_init)
#define lzw_core_decoder_reset GHOTIIO_COMPRESS(lzw_core_decoder_reset)
#define lzw_core_encoder_add GHOTIIO_COMPRESS(lzw_core_encoder_add)
#define lzw_core_encoder_destroy GHOTIIO_COMPRESS(lzw_core_encoder_destroy)
#define lzw_core_encoder_find GHOTIIO_COMPRESS(lzw_core_encoder_find)
#define lzw_core_encoder_init GHOTIIO_COMPRESS(lzw_core_encoder_init)
#define lzw_core_encoder_is_full GHOTIIO_COMPRESS(lzw_core_encoder_is_full)
#define lzw_core_encoder_reset GHOTIIO_COMPRESS(lzw_core_encoder_reset)
#define lzw_profile_bit_order GHOTIIO_COMPRESS(lzw_profile_bit_order)
#define lzw_profile_clear_code GHOTIIO_COMPRESS(lzw_profile_clear_code)
#define lzw_profile_eoi_code GHOTIIO_COMPRESS(lzw_profile_eoi_code)
#define lzw_profile_from_string GHOTIIO_COMPRESS(lzw_profile_from_string)
#define lzw_profile_should_increment_bits GHOTIIO_COMPRESS(lzw_profile_should_increment_bits)
#define rle_emit_literal GHOTIIO_COMPRESS(rle_emit_literal)
#define rle_emit_repeat GHOTIIO_COMPRESS(rle_emit_repeat)
#define zstd_parallel_alloc_job GHOTIIO_COMPRESS(zstd_parallel_alloc_job)
#define zstd_parallel_create GHOTIIO_COMPRESS(zstd_parallel_create)
#define zstd_parallel_destroy GHOTIIO_COMPRESS(zstd_parallel_destroy)
#define zstd_parallel_free_job GHOTIIO_COMPRESS(zstd_parallel_free_job)
#define zstd_parallel_get_job_size GHOTIIO_COMPRESS(zstd_parallel_get_job_size)
#define zstd_parallel_get_result GHOTIIO_COMPRESS(zstd_parallel_get_result)
#define zstd_parallel_is_inline GHOTIIO_COMPRESS(zstd_parallel_is_inline)
#define zstd_parallel_pending_count GHOTIIO_COMPRESS(zstd_parallel_pending_count)
#define zstd_parallel_reset GHOTIIO_COMPRESS(zstd_parallel_reset)
#define zstd_parallel_result_ready GHOTIIO_COMPRESS(zstd_parallel_result_ready)
#define zstd_parallel_submit GHOTIIO_COMPRESS(zstd_parallel_submit)
#define zstd_parallel_wait GHOTIIO_COMPRESS(zstd_parallel_wait)

/// @endcond

#endif // GHOTI_IO_GCOMP_NAMESPACE_H
