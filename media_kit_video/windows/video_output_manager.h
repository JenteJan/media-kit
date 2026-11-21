// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef VIDEO_OUTPUT_MANAGER_H_
#define VIDEO_OUTPUT_MANAGER_H_

#include <flutter/plugin_registrar_windows.h>

#include <unordered_map>

#include "thread_pool.h"
#include "video_output.h"

// Creates & disposes |VideoOutput| instances for video embedding.
//
// Create, SetSize and Dispose dispatch asynchronously and synchronize access
// to the outputs. Rendering uses a shared single-worker |ThreadPool|.
class VideoOutputManager {
 public:
  VideoOutputManager(flutter::TextureRegistrar* registrar,
                     Microsoft::WRL::ComPtr<IDXGIAdapter> adapter = nullptr);

  // Creates a new |VideoOutput| unless |handle| already exists. The callback
  // reports (texture ID, width, height) initially and after resizing, from
  // background threads; platform-channel work must be dispatched separately.
  void Create(
      int64_t handle,
      VideoOutputConfiguration configuration,
      std::function<void(int64_t, int64_t, int64_t)> texture_update_callback,
      std::function<void(std::string)> completion);

  // Queues size overrides for an existing output; nullopt follows the video.
  // The rendering worker also processes the request while paused.
  void SetSize(int64_t handle,
               std::optional<int64_t> width,
               std::optional<int64_t> height);

  // Destroys the |VideoOutput| with given handle.
  void Dispose(int64_t handle, std::function<void()> completion);

  ~VideoOutputManager();

 private:
  // Serialize rendering, resize and context teardown across outputs.
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
  std::unique_ptr<ThreadPool> thread_pool_ = std::make_unique<ThreadPool>(1);
  flutter::TextureRegistrar* registrar_ = nullptr;
  std::atomic<bool> shutting_down_{false};
  std::unordered_map<int64_t, std::unique_ptr<VideoOutput>> video_outputs_ = {};
  // Separate from rendering: output destruction may wait for rendering and
  // raster-thread unregister callbacks. FIFO preserves platform-call order.
  std::unique_ptr<ThreadPool> operations_ = std::make_unique<ThreadPool>(1);
};

#endif  // VIDEO_OUTPUT_MANAGER_H_
