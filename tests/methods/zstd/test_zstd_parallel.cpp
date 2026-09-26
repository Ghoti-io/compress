/**
 * @file test_zstd_parallel.cpp
 *
 * Unit tests for Zstd parallel compression infrastructure.
 *
 * Tests the direct API (submit/get_result) for both inline and threaded modes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "../common/test_helpers.h"
#include <algorithm>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/errors.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/compress/stream.h>
#include <ghoti.io/compress/zstd.h>
#include <gtest/gtest.h>
#include <vector>

// Include the parallel header
extern "C" {
#include "../../../src/methods/zstd/zstd_parallel.h"
}

/**
 * @brief Destroys a parallel context when the scope ends, however it ends.
 *
 * A gtest ASSERT returns from the test function on the spot, so an assertion
 * between creating a context and destroying it leaks the context -- and with
 * more than one thread that leaks a thread pool whose workers are parked on
 * a semaphore with nothing left to set their shutdown flag.  The thread
 * cleanup that runs at process exit then joins them and never returns, so
 * the process hangs AFTER every test has finished and a failing test reports
 * as a timeout with no name attached.
 */
class ParallelCtxGuard {
public:
  explicit ParallelCtxGuard(zstd_parallel_ctx_t *& ctx) : ctx_(ctx) {}
  ~ParallelCtxGuard() {
    if (ctx_) {
      zstd_parallel_destroy(ctx_);
      ctx_ = nullptr;
    }
  }
  ParallelCtxGuard(const ParallelCtxGuard &) = delete;
  ParallelCtxGuard & operator=(const ParallelCtxGuard &) = delete;

private:
  zstd_parallel_ctx_t *& ctx_;
};

class ZstdParallelTest : public ::testing::Test {
protected:
  void SetUp() override {
    registry_ = gcomp_registry_default();
  }

  // Helper to decode data using the standard decoder
  // A job's output is a run of BLOCKS, not a frame.
  //
  // Each job used to emit a complete frame of its own, so a test could hand
  // job->base.output straight to the decoder.  Jobs are now pieces of one
  // shared frame: they carry no magic, no frame header and no terminator,
  // because only the encoder knows where the content ends.  To decode one on
  // its own a test has to supply the frame around it, which is what this does
  // -- a header declaring the job's window and no content size, the job's
  // blocks, then the empty Last_Block the encoder would have appended.
  std::vector<uint8_t> decodeJobBlocks(
      const void * blocks, size_t len, uint8_t window_log) {
    std::vector<uint8_t> frame;
    frame.insert(frame.end(), {0x28, 0xb5, 0x2f, 0xfd});
    // Frame_Header_Descriptor: no content size, not single segment, no
    // dictionary, no checksum -- so a Window_Descriptor byte follows.
    frame.push_back(0x00);
    frame.push_back((uint8_t)((window_log - 10u) << 3));
    const uint8_t * b = (const uint8_t *)blocks;
    frame.insert(frame.end(), b, b + len);
    // Last_Block set, Raw, size zero.
    frame.insert(frame.end(), {0x01, 0x00, 0x00});
    return decode(frame.data(), frame.size());
  }

  std::vector<uint8_t> decode(
      const void * data, size_t len, bool concat = true) {
    gcomp_decoder_t * dec = nullptr;
    gcomp_options_t * opts = nullptr;
    gcomp_options_create(&opts);
    gcomp_options_set_bool(opts, "zstd.concat", concat);

    if (gcomp_decoder_create(registry_, "zstd", opts, &dec) != GCOMP_OK) {
      gcomp_options_destroy(opts);
      return {};
    }
    std::vector<uint8_t> out(len * 1000 + 65536);
    gcomp_buffer_t in = {const_cast<void *>(data), len, 0};
    gcomp_buffer_t ob = {out.data(), out.size(), 0};

    // Loop until all input is consumed (for multi-block frames)
    gcomp_status_t status;
    while (in.used < in.size) {
      status = gcomp_decoder_update(dec, &in, &ob);
      if (status != GCOMP_OK) {
        gcomp_decoder_destroy(dec);
        gcomp_options_destroy(opts);
        return {};
      }
    }

    status = gcomp_decoder_finish(dec, &ob);
    gcomp_decoder_destroy(dec);
    gcomp_options_destroy(opts);
    if (status != GCOMP_OK) {
      return {};
    }
    out.resize(ob.used);
    return out;
  }

  gcomp_registry_t * registry_ = nullptr;
};

//
// Context Creation Tests
//

TEST_F(ZstdParallelTest, CreateInlineContext) {
  zstd_parallel_config_t config = {
      .num_threads = 1, // Inline mode
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(zstd_parallel_is_inline(ctx));
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

}

TEST_F(ZstdParallelTest, CreateThreadedContext) {
  zstd_parallel_config_t config = {
      .num_threads = 2, // Threaded mode
      .max_in_flight = 4,
      .job_size = 64 * 1024, // 64KB
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

}

TEST_F(ZstdParallelTest, CreateZeroThreadsIsInline) {
  zstd_parallel_config_t config = {
      .num_threads = 0, // Should be inline
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(zstd_parallel_is_inline(ctx));

}

TEST_F(ZstdParallelTest, NullCtxOutFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 0,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  EXPECT_EQ(zstd_parallel_create(&config, nullptr), GCOMP_ERR_INVALID_ARG);
}

//
// Job Allocation Tests
//

TEST_F(ZstdParallelTest, AllocAndFreeJob) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  ASSERT_NE(job, nullptr);
  EXPECT_NE(job->base.input, nullptr);
  EXPECT_NE(job->base.output, nullptr);
  EXPECT_NE(job->match_finder, nullptr);

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, AllocJobNullCtxFails) {
  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_alloc_job(nullptr, &job), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, AllocJobNullJobOutFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  EXPECT_EQ(zstd_parallel_alloc_job(ctx, nullptr), GCOMP_ERR_INVALID_ARG);

}

//
// Inline Mode Submit/GetResult Tests
//

TEST_F(ZstdParallelTest, InlineSubmitGetResult) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Allocate job
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill input with test data
  const char test_data[] = "Hello from parallel compression test!";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  // Submit
  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 1u);
  EXPECT_TRUE(zstd_parallel_result_ready(ctx));

  // Get result
  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_EQ(result, job);
  EXPECT_GT(result->base.output_size, 0u);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  // Verify the output is decodable
  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, InlineMultipleJobs) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  const char * test_strings[] = {"First job", "Second job", "Third job"};
  std::vector<zstd_parallel_job_t *> jobs;

  // Submit multiple jobs
  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
    memcpy((void *)job->base.input, test_strings[i], strlen(test_strings[i]));
    job->base.input_size = strlen(test_strings[i]);
    ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);
    jobs.push_back(job);
  }

  EXPECT_EQ(zstd_parallel_pending_count(ctx), 3u);

  // Get results in order
  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
    EXPECT_EQ(result, jobs[i]);

    auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
    ASSERT_EQ(decoded.size(), strlen(test_strings[i]));
    EXPECT_EQ(memcmp(decoded.data(), test_strings[i], decoded.size()), 0);
  }

  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  for (auto job : jobs) {
    zstd_parallel_free_job(ctx, job);
  }
}

TEST_F(ZstdParallelTest, InlineWithChecksum) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = true, // Enable checksum
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  const char test_data[] = "Data with checksum enabled";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  // The checksum is no longer a job's business: it covers the whole frame's
  // content, which no single job sees.  The encoder takes it over the input
  // as it hands it to the jobs, and writes it after the last block; see
  // ZstdParallelEncoderTest and the zstd CLI oracle tests for that.  What
  // this test still checks is that asking for a checksum does not change
  // what a job produces.

  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
}

//
// Threaded Mode Submit/GetResult Tests
//

TEST_F(ZstdParallelTest, ThreadedSubmitGetResult) {
  zstd_parallel_config_t config = {
      .num_threads = 2,
      .max_in_flight = 4,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  const char test_data[] = "Hello from threaded parallel compression!";
  memcpy((void *)job->base.input, test_data, strlen(test_data));
  job->base.input_size = strlen(test_data);

  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_EQ(result, job);
  EXPECT_GT(result->base.output_size, 0u);

  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  ASSERT_EQ(decoded.size(), strlen(test_data));
  EXPECT_EQ(memcmp(decoded.data(), test_data, decoded.size()), 0);

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, ThreadedMultipleJobsOrderPreserved) {
  zstd_parallel_config_t config = {
      .num_threads = 4,
      .max_in_flight = 8,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Create jobs with different sizes to potentially complete out of order
  std::vector<std::vector<uint8_t>> inputs;
  std::vector<zstd_parallel_job_t *> jobs;

  for (int i = 0; i < 6; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

    // Different sizes: small, medium, small, large, small, medium
    size_t size = (i % 3 == 0) ? 100 : (i % 3 == 1) ? 5000 : 500;
    std::vector<uint8_t> data(size);
    test_helpers_generate_random(data.data(), data.size(), 12345 + i);
    inputs.push_back(data);

    memcpy((void *)job->base.input, data.data(), data.size());
    job->base.input_size = data.size();
    ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);
    jobs.push_back(job);
  }

  // Get results - they must come back in submission order
  for (size_t i = 0; i < jobs.size(); i++) {
    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
    EXPECT_EQ(result, jobs[i]) << "Results should be in submission order";

    auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
    ASSERT_EQ(decoded.size(), inputs[i].size());
    EXPECT_EQ(memcmp(decoded.data(), inputs[i].data(), decoded.size()), 0);
  }

  for (auto job : jobs) {
    zstd_parallel_free_job(ctx, job);
  }
}

//
// Concatenated Frame Tests
//

TEST_F(ZstdParallelTest, ConcatenatedFramesDecodable) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Create multiple jobs
  std::vector<std::vector<uint8_t>> inputs;
  std::vector<uint8_t> concatenated_output;
  uint8_t last_window_log = 0;

  for (int i = 0; i < 3; i++) {
    zstd_parallel_job_t * job = nullptr;
    ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

    std::vector<uint8_t> data(1000);
    test_helpers_generate_random(data.data(), data.size(), 11111 + i);
    inputs.push_back(data);

    memcpy((void *)job->base.input, data.data(), data.size());
    job->base.input_size = data.size();
    ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

    zstd_parallel_job_t * result = nullptr;
    ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
    last_window_log = result->window_log;

    // Concatenate output
    concatenated_output.insert(concatenated_output.end(), result->base.output,
        result->base.output + result->base.output_size);

    zstd_parallel_free_job(ctx, job);
  }


  // The jobs' outputs concatenate into ONE run of blocks, which is the point:
  // they are pieces of a single frame, so the whole run decodes under one
  // frame header rather than as a frame each.
  auto decoded = decodeJobBlocks(concatenated_output.data(),
      concatenated_output.size(), last_window_log);

  // Expected: all inputs concatenated
  size_t expected_size = 0;
  for (const auto & inp : inputs) {
    expected_size += inp.size();
  }
  ASSERT_EQ(decoded.size(), expected_size);

  // Verify content
  size_t offset = 0;
  for (const auto & inp : inputs) {
    EXPECT_EQ(memcmp(decoded.data() + offset, inp.data(), inp.size()), 0);
    offset += inp.size();
  }
}

//
// Memory Limit Tests
//

TEST_F(ZstdParallelTest, MemoryLimitReducesInFlight) {
  // With a small memory limit, max_in_flight should be reduced
  zstd_parallel_config_t config = {
      .num_threads = 4,
      .max_in_flight = 10, // Request 10
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 1024 * 1024, // 1MB limit
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);
  EXPECT_FALSE(zstd_parallel_is_inline(ctx));

  // Context should have been created with reduced in-flight count
  // We can verify by checking job_size was respected
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 64 * 1024u);

}

//
// Wait and Reset Tests
//

TEST_F(ZstdParallelTest, WaitInlineMode) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Wait should succeed immediately in inline mode
  EXPECT_EQ(zstd_parallel_wait(ctx), GCOMP_OK);

}

TEST_F(ZstdParallelTest, ResetAfterAllJobsRetrieved) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Submit and retrieve a job
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  job->base.input_size = 10;
  memset((void *)job->base.input, 'A', 10);
  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);

  // Reset should succeed after all jobs retrieved
  EXPECT_EQ(zstd_parallel_reset(ctx), GCOMP_OK);
  EXPECT_EQ(zstd_parallel_pending_count(ctx), 0u);

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, ResetWithPendingJobsFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Submit a job but don't retrieve it
  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);
  job->base.input_size = 10;
  memset((void *)job->base.input, 'A', 10);
  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  // Reset should fail with pending jobs
  EXPECT_EQ(zstd_parallel_reset(ctx), GCOMP_ERR_INVALID_ARG);

  // Clean up
  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  zstd_parallel_free_job(ctx, job);
}

//
// Edge Cases
//

TEST_F(ZstdParallelTest, EmptyInput) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  job->base.input_size = 0; // Empty input

  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  // No input, so no blocks.  A job no longer emits a frame of its own, and
  // there is nothing else for it to say: the encoder's terminating block is
  // what makes the empty frame.
  EXPECT_EQ(result->base.output_size, 0u);

  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  EXPECT_EQ(decoded.size(), 0u);

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, RLECompressibleInput) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill with repeated byte (should use RLE block)
  memset((void *)job->base.input, 'X', 10000);
  job->base.input_size = 10000;

  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_LT(result->base.output_size, 100u); // Should compress very well

  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  ASSERT_EQ(decoded.size(), 10000u);
  for (size_t i = 0; i < decoded.size(); i++) {
    EXPECT_EQ(decoded[i], 'X');
  }

  zstd_parallel_free_job(ctx, job);
}

TEST_F(ZstdParallelTest, LargeInputMultipleBlocks) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 256 * 1024, // 256KB
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  ASSERT_EQ(zstd_parallel_alloc_job(ctx, &job), GCOMP_OK);

  // Fill with data that will span multiple blocks (block max is 128KB)
  std::vector<uint8_t> data(200 * 1024);
  test_helpers_generate_random(data.data(), data.size(), 99999);
  memcpy((void *)job->base.input, data.data(), data.size());
  job->base.input_size = data.size();

  ASSERT_EQ(zstd_parallel_try_submit(ctx, job), GCOMP_OK);

  zstd_parallel_job_t * result = nullptr;
  ASSERT_EQ(zstd_parallel_get_result(ctx, &result), GCOMP_OK);
  EXPECT_GT(result->base.output_size, 0u);

  auto decoded = decodeJobBlocks(result->base.output, result->base.output_size,
        result->window_log);
  ASSERT_EQ(decoded.size(), data.size());
  EXPECT_EQ(memcmp(decoded.data(), data.data(), data.size()), 0);

  zstd_parallel_free_job(ctx, job);
}

//
// Error Handling Tests
//

TEST_F(ZstdParallelTest, SubmitNullCtxFails) {
  EXPECT_EQ(zstd_parallel_try_submit(nullptr, nullptr), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, GetResultNullCtxFails) {
  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_get_result(nullptr, &job), GCOMP_ERR_INVALID_ARG);
}

TEST_F(ZstdParallelTest, GetResultNoPendingFails) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 64 * 1024,
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  zstd_parallel_job_t * job = nullptr;
  EXPECT_EQ(zstd_parallel_get_result(ctx, &job), GCOMP_ERR_INVALID_ARG);

}

//
// Job Size Tests
//

TEST_F(ZstdParallelTest, JobSizeAutoDefault) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 0, // Auto
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should use default job size (512KB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 512 * 1024u);

}

TEST_F(ZstdParallelTest, JobSizeMinEnforced) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 1024, // Too small
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should be clamped to minimum (64KB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 64 * 1024u);

}

TEST_F(ZstdParallelTest, JobSizeMaxEnforced) {
  zstd_parallel_config_t config = {
      .num_threads = 1,
      .max_in_flight = 0,
      .job_size = 100 * 1024 * 1024, // Too large
      .checksum_enabled = false,
      .compression_level = 3,
      .window_log = 0,
      .max_memory_bytes = 0,
      .ldm_enabled = false,
      .ldm_min_match = 0,
      .ldm_hash_log = 0,
      .ldm_hash_rate_log = 0,
      .allocator = nullptr,
      .mem_tracker = nullptr,
  };

  zstd_parallel_ctx_t * ctx = nullptr;
  ParallelCtxGuard ctx_guard(ctx);
  ASSERT_EQ(zstd_parallel_create(&config, &ctx), GCOMP_OK);

  // Should be clamped to maximum (16MB)
  EXPECT_EQ(zstd_parallel_get_job_size(ctx), 16 * 1024 * 1024u);

}

//
// End to end: the encoder driven with threads.count, which is how a caller
// reaches any of the above.  Nothing tested this, and two defects lived
// behind that: the encoder deadlocked once it had handed over as many jobs
// as the context would hold, and every job longer than one block decoded to
// the wrong bytes.
//

namespace {

/**
 * @brief Words drawn from a small vocabulary: text-shaped bytes.
 *
 * Chosen because it is what actually catches a mis-carried repeat offset.
 * Every match here is a word that has appeared before, so the same handful
 * of distances recur constantly and a parse reaches for the one-symbol
 * offset codes in every block.  Synthetic alternatives did not: noise with
 * phrases planted at fixed periods, and fixed-width records, both
 * round-tripped through the broken encoder without complaint.
 */
std::vector<uint8_t> WordyText(size_t n) {
  static const char * words[] = {"the", "quick", "brown", "fox", "jumps",
      "over", "lazy", "dog", "and", "then", "returns", "home", "with",
      "another", "message", "for", "everyone", "who", "waited", "encoder",
      "decoder", "window", "offset", "literal", "sequence"};
  const size_t count = sizeof(words) / sizeof(words[0]);

  std::vector<uint8_t> v;
  v.reserve(n + 16);
  uint32_t seed = 90210u;
  while (v.size() < n) {
    seed = seed * 1103515245u + 12345u;
    const char * w = words[(seed >> 16) % count];
    while (*w) {
      v.push_back(static_cast<uint8_t>(*w++));
    }
    v.push_back((seed & 0x1Fu) == 0 ? '\n' : ' ');
  }
  v.resize(n);
  return v;
}

} // namespace

/**
 * @brief The automatic job size follows the window, exactly.
 *
 * `zstd.job_size = 0` means four windows, clamped to 512 KB - 16 MB, and this
 * pins that formula through the public API rather than trusting the constant.
 * The job size decides where block seams fall, so the output bytes are the
 * observable: an automatic encode must be byte-identical to the same encode with
 * the figure spelled out, and must differ from the fixed 512 KB this used to
 * default to.
 *
 * ## Why the formula and not just "it is not slower"
 *
 * The reason for it is a throughput one - each job re-indexes a window of the
 * preceding stream, so wasted work per useful byte is window/job, and a fixed
 * 512 KB job made four threads 0.09x of one at window_log 25. But a timing
 * assertion on a shared machine is a flaky test, and the multiplier was chosen by
 * measurement that a test cannot repeat cheaply (four against eight windows, two
 * input shapes; see notes/compress/PERFORMANCE.md). So what is gated here is the
 * decision, not the speed: if somebody changes the multiplier or returns to a
 * fixed size, this fails and points at the measurement.
 *
 * The default level is checked too, and it is the reassuring half: window_log 17
 * gives 4 x 128 KB = 512 KB, exactly what callers had before, so no existing
 * parallel encode changed shape.
 */
TEST(ZstdParallel, AutomaticJobSizeIsFourWindows) {
  gcomp_registry_t * registry = gcomp_registry_default();
  ASSERT_NE(registry, nullptr);
  // Several jobs' worth at the largest window tested, so a seam difference is
  // actually reachable.
  const std::vector<uint8_t> data = WordyText(40u * 1024u * 1024u);

  auto encode = [&](uint64_t window_log, uint64_t job_size) {
    gcomp_options_t * o = nullptr;
    EXPECT_EQ(gcomp_options_create(&o), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_uint64(o, "threads.count", 4), GCOMP_OK);
    EXPECT_EQ(gcomp_options_set_uint64(o, "zstd.window_log", window_log),
        GCOMP_OK);
    if (job_size) {
      EXPECT_EQ(gcomp_options_set_uint64(o, "zstd.job_size", job_size),
          GCOMP_OK);
    }
    size_t bound = 0;
    EXPECT_EQ(gcomp_encode_bound(registry, "zstd", o, data.size(), &bound),
        GCOMP_OK);
    std::vector<uint8_t> out(bound);
    size_t w = 0;
    EXPECT_EQ(gcomp_encode_buffer(registry, "zstd", o, data.data(),
                  data.size(), out.data(), out.size(), &w),
        GCOMP_OK)
        << "window_log=" << window_log << " job_size=" << job_size;
    out.resize(w);
    gcomp_options_destroy(o);
    return out;
  };

  struct Case {
    uint64_t window_log;
    uint64_t expected_job;
  };
  // 4 x window, clamped to [512 KB, 16 MB].
  const std::vector<Case> cases = {
      {15, 524288u},   // 4 x 32 KB = 128 KB, floored at 512 KB
      {17, 524288u},   // 4 x 128 KB = 512 KB exactly - the old default
      {19, 2097152u},  // 4 x 512 KB
      {21, 8388608u},  // 4 x 2 MB
      {23, 16777216u}, // 4 x 8 MB = 32 MB, capped at 16 MB
  };

  for (const auto & c : cases) {
    const std::vector<uint8_t> automatic = encode(c.window_log, 0);
    const std::vector<uint8_t> spelled = encode(c.window_log, c.expected_job);
    EXPECT_EQ(automatic, spelled)
        << "window_log=" << c.window_log
        << ": the automatic job size is not " << c.expected_job
        << " bytes; the default no longer follows the window as four times it";

    if (c.expected_job != 524288u) {
      const std::vector<uint8_t> fixed = encode(c.window_log, 524288u);
      EXPECT_NE(automatic, fixed)
          << "window_log=" << c.window_log
          << ": the automatic job size still behaves like a fixed 512 KB, which "
             "is the setting that made four threads slower than one";
    }
  }
}

/**
 * @brief Compress with threads and read it back.
 */
static void ExpectThreadedRoundTrip(gcomp_registry_t * registry,
    const std::vector<uint8_t> & data, int level, uint64_t threads,
    uint64_t job_size) {
  gcomp_options_t * opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
  gcomp_options_set_int64(opts, "zstd.level", level);
  gcomp_options_set_uint64(opts, "threads.count", threads);
  if (job_size) {
    gcomp_options_set_uint64(opts, "zstd.job_size", job_size);
  }

  std::vector<uint8_t> out(data.size() + data.size() / 2 + 65536);
  size_t out_len = 0;
  gcomp_status_t status = gcomp_encode_buffer(registry, "zstd", opts,
      data.data(), data.size(), out.data(), out.size(), &out_len);
  gcomp_options_destroy(opts);
  ASSERT_EQ(status, GCOMP_OK) << "level " << level << ", " << threads
                              << " threads";
  ASSERT_GT(out_len, 0u);

  // Threaded output is a run of independent frames, one per job.
  gcomp_options_t * dec_opts = nullptr;
  ASSERT_EQ(gcomp_options_create(&dec_opts), GCOMP_OK);
  gcomp_options_set_bool(dec_opts, "zstd.concat", true);
  std::vector<uint8_t> back(data.size() + 1024);
  size_t back_len = 0;
  status = gcomp_decode_buffer(registry, "zstd", dec_opts, out.data(), out_len,
      back.data(), back.size(), &back_len);
  gcomp_options_destroy(dec_opts);
  ASSERT_EQ(status, GCOMP_OK) << "level " << level;
  ASSERT_EQ(back_len, data.size()) << "level " << level;
  EXPECT_EQ(memcmp(back.data(), data.data(), data.size()), 0)
      << "level " << level << ", " << threads << " threads";
}

// More jobs than the context will hold at once.
//
// The encoder used to wait for room to hand over the next one, and the only
// thing that makes room is taking a result back -- which the same thread
// does.  So it waited for itself: four workers idle, encoder asleep, forever.
// At the defaults that was any input over max_in_flight * job_size, about
// 4 MB with threads.count of 4.  A regression here does not fail this test,
// it hangs it.
TEST_F(ZstdParallelTest, MoreJobsThanTheContextHolds) {
  // 64 KB jobs and two threads means the context holds four; sixteen jobs of
  // input is four times over, without making the test slow.
  std::vector<uint8_t> data = WordyText(16u * 64u * 1024u);
  for (int level : {1, 3, 9, 16, 19}) {
    ExpectThreadedRoundTrip(registry_, data, level, 2, 64u * 1024u);
  }
}

// Every block of a job after the first.
//
// The three repeat offsets are reset at the start of a FRAME (RFC 8878
// section 3.1.1.3.2.1.1); within one, each block continues from where the
// last left off, and a decoder does exactly that.  The threaded encoder
// reset them per block, so from the second block of every job onwards it
// wrote a code meaning one distance and the decoder read another.
//
// It needs a job of more than one block, and data whose later blocks reach
// for a repeat offset.  Not all data does: 2.97 MB of manual pages
// round-tripped through the broken encoder while 4.69 MB of C source did
// not, and two synthetic shapes that looked like they should catch it --
// noise with phrases at fixed periods, and fixed-width records -- did not
// either.  See WordyText().
TEST_F(ZstdParallelTest, RepeatOffsetsCarryAcrossTheBlocksOfAJob) {
  // 512 KB jobs are four blocks each; three jobs of input.
  std::vector<uint8_t> data = WordyText(3u * 512u * 1024u);
  for (int level : {3, 9, 16, 19}) {
    ExpectThreadedRoundTrip(registry_, data, level, 2, 512u * 1024u);
  }
}


/**
 * A caller whose output buffer is smaller than one job's output must still
 * get a stream that decodes to what it put in.
 *
 * It did not.  The encoder can hold back one job's output while the caller
 * drains it, and it collected the next result straight over the top of it:
 * the held-back bytes were gone, the stream would not decode, and its length
 * varied from run to run.  Everything was fine as long as the output buffer
 * was big enough to take a whole job at once, which is what every test here
 * had done.
 *
 * The repeats are not decoration.  Whether a second result is ready at the
 * moment the encoder collects is a race between this thread and the workers,
 * so a single pass can miss it; before the fix, four passes never did.
 */
TEST(ZstdParallelStreamingTest, ASmallOutputBufferDoesNotLoseAJobsOutput) {
  gcomp_registry_t * registry = gcomp_registry_default();
  ASSERT_NE(registry, nullptr);

  // Compressible, so jobs produce output worth losing.
  std::vector<uint8_t> data(2 * 1024 * 1024);
  static const char * words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
      "over ", "lazy ", "dog ", "and then ", "again "};
  unsigned state = 67;
  size_t pos = 0;
  while (pos < data.size()) {
    state = state * 1103515245u + 12345u;
    const char * w = words[(state >> 16) % 10];
    size_t n = strlen(w);
    if (pos + n > data.size()) {
      n = data.size() - pos;
    }
    memcpy(data.data() + pos, w, n);
    pos += n;
  }

  for (int repeat = 0; repeat < 4; repeat++) {
    for (uint64_t threads : {2u, 4u}) {
      gcomp_options_t * opts = nullptr;
      ASSERT_EQ(gcomp_options_create(&opts), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_uint64(opts, "threads.count", threads),
          GCOMP_OK);
      ASSERT_EQ(
          gcomp_options_set_uint64(opts, "zstd.job_size", 65536), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bool(opts, "zstd.concat", 1), GCOMP_OK);

      gcomp_encoder_t * encoder = nullptr;
      ASSERT_EQ(gcomp_encoder_create(registry, "zstd", opts, &encoder),
          GCOMP_OK);

      // A kilobyte at a time: far less than one job's compressed output, so
      // every result has to be handed out across many calls.
      std::vector<uint8_t> chunk(1024);
      std::vector<uint8_t> stream;
      size_t consumed = 0;
      while (consumed < data.size()) {
        size_t take = std::min<size_t>(4096, data.size() - consumed);
        gcomp_buffer_t in_buf = {data.data() + consumed, take, 0};
        while (in_buf.used < in_buf.size) {
          gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
          size_t before = in_buf.used;
          ASSERT_EQ(gcomp_encoder_update(encoder, &in_buf, &out_buf), GCOMP_OK);
          stream.insert(
              stream.end(), chunk.begin(), chunk.begin() + out_buf.used);
          ASSERT_FALSE(in_buf.used == before && out_buf.used == 0)
              << "update() made no progress";
        }
        consumed += take;
      }
      for (;;) {
        gcomp_buffer_t out_buf = {chunk.data(), chunk.size(), 0};
        gcomp_status_t status = gcomp_encoder_finish(encoder, &out_buf);
        stream.insert(
            stream.end(), chunk.begin(), chunk.begin() + out_buf.used);
        if (status == GCOMP_OK) {
          break;
        }
        ASSERT_EQ(status, GCOMP_ERR_LIMIT);
        ASSERT_GT(out_buf.used, 0u) << "finish() made no progress";
      }
      gcomp_encoder_destroy(encoder);
      gcomp_options_destroy(opts);

      gcomp_options_t * dopts = nullptr;
      ASSERT_EQ(gcomp_options_create(&dopts), GCOMP_OK);
      ASSERT_EQ(gcomp_options_set_bool(dopts, "zstd.concat", 1), GCOMP_OK);
      gcomp_decoder_t * decoder = nullptr;
      ASSERT_EQ(gcomp_decoder_create(registry, "zstd", dopts, &decoder),
          GCOMP_OK);
      std::vector<uint8_t> back(data.size() + 4096);
      gcomp_buffer_t din = {stream.data(), stream.size(), 0};
      gcomp_buffer_t dout = {back.data(), back.size(), 0};
      EXPECT_EQ(gcomp_decoder_update(decoder, &din, &dout), GCOMP_OK)
          << "threads=" << threads << " repeat=" << repeat;
      EXPECT_EQ(gcomp_decoder_finish(decoder, &dout), GCOMP_OK);
      EXPECT_EQ(dout.used, data.size());
      if (dout.used == data.size()) {
        EXPECT_EQ(memcmp(back.data(), data.data(), data.size()), 0)
            << "threads=" << threads << " repeat=" << repeat;
      }
      gcomp_decoder_destroy(decoder);
      gcomp_options_destroy(dopts);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
