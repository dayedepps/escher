// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "escher/renderer/renderer.h"

#include "escher/impl/command_buffer_pool.h"
#include "escher/impl/escher_impl.h"
#include "escher/impl/image_cache.h"
#include "escher/impl/vulkan_utils.h"
#include "escher/profiling/timestamp_profiler.h"
#include "escher/renderer/framebuffer.h"
#include "escher/renderer/image.h"
#include "escher/scene/stage.h"
#include "escher/util/stopwatch.h"

namespace escher {

Renderer::Renderer(impl::EscherImpl* escher)
    : escher_(escher),
      context_(escher_->vulkan_context()),
      pool_(escher->command_buffer_pool()) {
  escher_->IncrementRendererCount();
}

Renderer::~Renderer() {
  FTL_DCHECK(!current_frame_);
  escher_->DecrementRendererCount();
}

void Renderer::BeginFrame() {
  FTL_DCHECK(!current_frame_);
  ++frame_number_;
  current_frame_ = pool_->GetCommandBuffer();

  FTL_DCHECK(!profiler_);
  if (enable_profiling_ && escher_->supports_timer_queries()) {
    profiler_ = ftl::MakeRefCounted<TimestampProfiler>(
        context_.device, escher_->timestamp_period());
    AddTimestamp("throwaway");  // Intel/Mesa workaround; see EndFrame().
    AddTimestamp("start of frame");
  }
}

void Renderer::SubmitPartialFrame() {
  FTL_DCHECK(current_frame_);
  current_frame_->Submit(context_.queue, nullptr);
  current_frame_ = pool_->GetCommandBuffer();
}

void Renderer::EndFrame(const SemaphorePtr& frame_done,
                        FrameRetiredCallback frame_retired_callback) {
  FTL_DCHECK(current_frame_);
  current_frame_->AddSignalSemaphore(frame_done);
  if (profiler_) {
    // Avoid implicit reference to this in closure.
    TimestampProfilerPtr profiler = std::move(profiler_);
    auto frame_number = frame_number_;
    current_frame_->Submit(context_.queue, [frame_retired_callback, profiler,
                                            frame_number]() {
      if (frame_retired_callback) {
        frame_retired_callback();
      }
      FTL_LOG(INFO) << "------------------------------------------------------";
      FTL_LOG(INFO) << "Timestamps for frame #" << frame_number;
      FTL_LOG(INFO) << "total\t | \tsince previous (all times in microseconds)";
      FTL_LOG(INFO) << "------------------------------------------------------";
      auto timestamps = profiler->GetQueryResults();
      // Workaround: Intel/Mesa gives a screwed-up value for the second time.
      // So, we add a throwaway value first (see BeginFrame()), and then fix up
      // the first value that we actually print.
      timestamps[1].time = 0;
      timestamps[1].elapsed = 0;
      timestamps[2].elapsed = timestamps[2].time;
      for (size_t i = 1; i < timestamps.size(); ++i) {
        FTL_LOG(INFO) << timestamps[i].time << " \t | \t"
                      << timestamps[i].elapsed << "   \t" << timestamps[i].name;
      }
      FTL_LOG(INFO) << "------------------------------------------------------";
    });
  } else {
    current_frame_->Submit(context_.queue, std::move(frame_retired_callback));
  }
  current_frame_ = nullptr;

  escher_->Cleanup();
}

void Renderer::AddTimestamp(const char* name) {
  FTL_DCHECK(current_frame_);
  if (profiler_) {
    profiler_->AddTimestamp(current_frame_,
                            vk::PipelineStageFlagBits::eBottomOfPipe, name);
  }
}

void Renderer::RunOffscreenBenchmark(const VulkanContext& context,
                                     const Stage& stage,
                                     const Model& model,
                                     vk::Format framebuffer_format,
                                     size_t frame_count) {
  constexpr uint64_t kSecondsToNanoseconds = 1000000000;

  // Create the images that we will render into, and the semaphores that will
  // prevent us from rendering into the same image concurrently.  At the same
  // time, draw a few throwaway frames, to warm things up before beginning the
  // benchmark (this also signals the semaphores so that they can be waited
  // upon in the actual benchmark run).
  constexpr size_t kSwapchainSize = 3;
  std::vector<SemaphorePtr> semaphores;
  std::vector<ImagePtr> images;
  {
    const uint32_t width = stage.physical_size().width();
    const uint32_t height = stage.physical_size().height();

    auto image_cache = escher_->image_cache();
    for (size_t i = 0; i < kSwapchainSize; ++i) {
      auto im =
          image_cache->NewImage({framebuffer_format, width, height, 1,
                                 vk::ImageUsageFlagBits::eColorAttachment |
                                     vk::ImageUsageFlagBits::eTransferSrc});
      images.push_back(im);
      semaphores.push_back(Semaphore::New(context.device));

      DrawFrame(stage, model, images[i], semaphores[i], nullptr);
    }

    // Prepare all semaphores to be waited-upon, and wait for the throwaway
    // frames to finish.
    auto command_buffer = pool_->GetCommandBuffer();
    command_buffer->Submit(context_.queue, nullptr);
    FTL_CHECK(vk::Result::eSuccess ==
              command_buffer->Wait(kSwapchainSize * kSecondsToNanoseconds));
  }

  // Render the benchmark frames.
  Stopwatch stopwatch;
  stopwatch.Start();

  impl::CommandBuffer* throttle = nullptr;
  for (size_t current_frame = 0; current_frame < frame_count; ++current_frame) {
    size_t image_index = current_frame % kSwapchainSize;

    auto command_buffer = pool_->GetCommandBuffer();
    command_buffer->AddWaitSemaphore(semaphores[image_index],
                                     vk::PipelineStageFlagBits::eBottomOfPipe);
    command_buffer->Submit(context_.queue, nullptr);

    // Don't get too many frames ahead of the GPU.  Every time we cycle through
    // all images, wait for the previous frame that was rendered to that image
    // to finish.
    if (image_index == 0) {
      if (throttle) {
        FTL_CHECK(vk::Result::eSuccess ==
                  throttle->Wait(kSwapchainSize * kSecondsToNanoseconds));
      }
      throttle = command_buffer;
    }

    if (current_frame == frame_count - 1) {
      set_enable_profiling(true);
    }

    DrawFrame(stage, model, images[image_index], semaphores[image_index],
              nullptr);

    set_enable_profiling(false);
  }

  // Wait for the last frame to finish.
  auto command_buffer = pool_->GetCommandBuffer();
  command_buffer->AddWaitSemaphore(
      semaphores[(frame_count - 1) % kSwapchainSize],
      vk::PipelineStageFlagBits::eBottomOfPipe);
  command_buffer->Submit(context.queue, nullptr);
  FTL_CHECK(vk::Result::eSuccess ==
            command_buffer->Wait(kSwapchainSize * kSecondsToNanoseconds));
  stopwatch.Stop();

  FTL_LOG(INFO) << "------------------------------------------------------";
  FTL_LOG(INFO) << "Offscreen benchmark";
  FTL_LOG(INFO) << "Rendered " << frame_count << " frames in "
                << stopwatch.GetElapsedSeconds() << " seconds";
  FTL_LOG(INFO) << (frame_count / stopwatch.GetElapsedSeconds()) << " FPS";
  FTL_LOG(INFO) << "------------------------------------------------------";
}

}  // namespace escher
