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
 * @file options.h
 *
 * Key/value option system for the Ghoti.io Compress library.
 */

#ifndef GHOTI_IO_GCOMP_OPTIONS_H
#define GHOTI_IO_GCOMP_OPTIONS_H

#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/macros.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Forward declarations
 */
typedef struct gcomp_options_s gcomp_options_t;

/**
 * @brief Forward declaration for method type (defined in method.h).
 *
 * This allows the options validation helpers to reference the method type
 * without creating a circular include between options.h and method.h.
 */
struct gcomp_method_s;

/**
 * @brief Option value types
 */
typedef enum {
  GCOMP_OPT_INT64,  ///< 64-bit signed integer
  GCOMP_OPT_UINT64, ///< 64-bit unsigned integer
  GCOMP_OPT_BOOL,   ///< Boolean
  GCOMP_OPT_STRING, ///< String (null-terminated)
  GCOMP_OPT_BYTES,  ///< Byte array
  GCOMP_OPT_FLOAT,  ///< Floating point (optional)
} gcomp_option_type_t;

/**
 * @brief Policy for handling unknown option keys during validation.
 *
 * This controls how `gcomp_options_validate()` treats keys that are present
 * in a `gcomp_options_t` instance but not described by a method's option
 * schema.
 */
typedef enum {
  /**
   * @brief Treat unknown keys as an error.
   *
   * Validation functions will return ::GCOMP_ERR_INVALID_ARG if any unknown
   * keys are encountered.
   */
  GCOMP_UNKNOWN_KEY_ERROR = 0,

  /**
   * @brief Silently ignore unknown keys.
   *
   * Validation functions will skip keys that are not present in the schema.
   */
  GCOMP_UNKNOWN_KEY_IGNORE = 1,
} gcomp_unknown_key_policy_t;

/**
 * @brief Create a new options object
 *
 * @param options_out Output parameter for the created options object
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_create(gcomp_options_t ** options_out);

/**
 * @brief Create a new options object using a specific allocator.
 *
 * @param allocator Optional allocator (NULL for default).
 * @param options_out Output parameter for the created options object.
 * @return Status code.
 */
GCOMP_API gcomp_status_t gcomp_options_create_with_allocator(
    const gcomp_allocator_t * allocator, gcomp_options_t ** options_out);

/**
 * @brief Destroy an options object
 *
 * @param options The options object to destroy
 */
GCOMP_API void gcomp_options_destroy(gcomp_options_t * options);

/**
 * @brief Clone an options object
 *
 * @param options The options object to clone
 * @param cloned_out Output parameter for the cloned options object
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_clone(
    const gcomp_options_t * options, gcomp_options_t ** cloned_out);

/**
 * @brief Set an integer option value
 *
 * @param options The options object
 * @param key The option key (e.g., "deflate.level")
 * @param value The integer value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_set_int64(
    gcomp_options_t * options, const char * key, int64_t value);

/**
 * @brief Set an unsigned integer option value
 *
 * @param options The options object
 * @param key The option key
 * @param value The unsigned integer value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_set_uint64(
    gcomp_options_t * options, const char * key, uint64_t value);

/**
 * @brief Set a boolean option value
 *
 * @param options The options object
 * @param key The option key
 * @param value The boolean value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_set_bool(
    gcomp_options_t * options, const char * key, int value);

/**
 * @brief Set a string option value
 *
 * @param options The options object
 * @param key The option key
 * @param value The string value (copied)
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_set_string(
    gcomp_options_t * options, const char * key, const char * value);

/**
 * @brief Set a bytes option value
 *
 * @param options The options object
 * @param key The option key
 * @param data The byte data
 * @param size The size of the data
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_set_bytes(gcomp_options_t * options,
    const char * key, const void * data, size_t size);

/**
 * @brief Get an integer option value
 *
 * @param options The options object
 * @param key The option key
 * @param value_out Output parameter for the value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_get_int64(
    const gcomp_options_t * options, const char * key, int64_t * value_out);

/**
 * @brief Get an unsigned integer option value
 *
 * @param options The options object
 * @param key The option key
 * @param value_out Output parameter for the value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_get_uint64(
    const gcomp_options_t * options, const char * key, uint64_t * value_out);

/**
 * @brief Get a boolean option value
 *
 * @param options The options object
 * @param key The option key
 * @param value_out Output parameter for the value
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_get_bool(
    const gcomp_options_t * options, const char * key, int * value_out);

/**
 * @brief Get a string option value.
 *
 * Returns a pointer to the string value stored internally in the options
 * object. The returned pointer is valid only while:
 * 1. The options object has not been destroyed
 * 2. The key has not been modified (via set_string) or removed
 * 3. For frozen options: the pointer remains valid until destroy
 *
 * @warning Do not store this pointer long-term. If you need the value after
 * any options modification, copy the string immediately using strdup() or
 * similar.
 *
 * @warning For thread safety, only access the returned pointer while holding
 * any necessary synchronization. For frozen options, concurrent reads of the
 * same key are safe without synchronization.
 *
 * @param options The options object
 * @param key The option key
 * @param[out] value_out Output parameter for the string value (pointer to
 *             internal storage, valid until options is modified or destroyed)
 * @return ::GCOMP_OK on success
 * @return ::GCOMP_ERR_INVALID_ARG if options, key, or value_out is NULL
 * @return ::GCOMP_ERR_INVALID_ARG if key does not exist or is not a string
 *
 * @code
 * const char *name = NULL;
 * if (gcomp_options_get_string(opts, "gzip.name", &name) == GCOMP_OK) {
 *     // Use name immediately or copy it
 *     printf("Filename: %s\n", name);
 *
 *     // If needed later, copy it:
 *     char *name_copy = strdup(name);
 * }
 * @endcode
 */
GCOMP_API gcomp_status_t gcomp_options_get_string(
    const gcomp_options_t * options, const char * key, const char ** value_out);

/**
 * @brief Get a bytes option value.
 *
 * Returns a pointer to the byte data stored internally in the options object.
 * The returned pointer is valid only while:
 * 1. The options object has not been destroyed
 * 2. The key has not been modified (via set_bytes) or removed
 * 3. For frozen options: the pointer remains valid until destroy
 *
 * @warning Do not store this pointer long-term. If you need the data after
 * any options modification, copy it immediately using memcpy() or similar.
 *
 * @warning For thread safety, only access the returned pointer while holding
 * any necessary synchronization. For frozen options, concurrent reads of the
 * same key are safe without synchronization.
 *
 * @param options The options object
 * @param key The option key
 * @param[out] data_out Output parameter for the data pointer (pointer to
 *             internal storage, valid until options is modified or destroyed)
 * @param[out] size_out Output parameter for the size in bytes
 * @return ::GCOMP_OK on success
 * @return ::GCOMP_ERR_INVALID_ARG if options, key, data_out, or size_out is
 * NULL
 * @return ::GCOMP_ERR_INVALID_ARG if key does not exist or is not bytes type
 *
 * @code
 * const void *extra_data = NULL;
 * size_t extra_len = 0;
 * if (gcomp_options_get_bytes(opts, "gzip.extra", &extra_data, &extra_len) ==
 *     GCOMP_OK) {
 *     // Use data immediately or copy it
 *     process_extra(extra_data, extra_len);
 *
 *     // If needed later, copy it:
 *     void *copy = malloc(extra_len);
 *     memcpy(copy, extra_data, extra_len);
 * }
 * @endcode
 */
GCOMP_API gcomp_status_t gcomp_options_get_bytes(
    const gcomp_options_t * options, const char * key, const void ** data_out,
    size_t * size_out);

/**
 * @brief Freeze an options object, making it immutable
 *
 * After calling this function, the options object becomes immutable and
 * cannot be modified. This is useful for thread-safety when sharing
 * options across multiple threads.
 *
 * @param options The options object to freeze
 * @return Status code
 */
GCOMP_API gcomp_status_t gcomp_options_freeze(gcomp_options_t * options);

/**
 * @brief Validate all options against a method's option schema.
 *
 * This function checks that:
 * - All option keys conform to the method's unknown key policy.
 * - All option value types match the schema type.
 * - Integer/unsigned integer values fall within any min/max constraints.
 *
 * The method must provide a schema via its @c get_schema vtable hook;
 * otherwise this function will return ::GCOMP_ERR_UNSUPPORTED.
 *
 * @param options The options object to validate (may be NULL to indicate
 *   "no options", which is always valid).
 * @param method The method whose schema should be used for validation.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_options_validate(
    const gcomp_options_t * options, const struct gcomp_method_s * method);

/**
 * @brief Validate a single option key against a method's option schema.
 *
 * This behaves similarly to `gcomp_options_validate()`, but only checks
 * the specified key. If the key is not present in @p options, this function
 * returns ::GCOMP_ERR_INVALID_ARG.
 *
 * @param options The options object containing the key to validate.
 * @param method The method whose schema should be used for validation.
 * @param key The option key to validate.
 * @return ::GCOMP_OK on success, or an error code on failure.
 */
GCOMP_API gcomp_status_t gcomp_options_validate_key(
    const gcomp_options_t * options, const struct gcomp_method_s * method,
    const char * key);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GCOMP_OPTIONS_H
