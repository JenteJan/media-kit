// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "video_output.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// mpv's linear light: 1.0 is HDR reference white ("paper white"), 203 nits
// (BT.2408).
constexpr float kMpvReferenceWhiteNits = 203.0f;

// Brightening never takes highlights below this much room above paper white:
// on a panel too dim for the asked level, paper white stops at peak / 2 so
// highlights still read as highlights instead of flattening into SDR.
constexpr float kMinHighlightHeadroom = 2.0f;

bool EngineSupportsHdr() {
  HMODULE engine = ::GetModuleHandleW(L"flutter_windows.dll");
  if (engine == nullptr) {
    return false;
  }
  using SupportsHdr = bool (*)();
  auto supports = reinterpret_cast<SupportsHdr>(
      ::GetProcAddress(engine, "FlutterDesktopEngineSupportsHdr"));
  return supports != nullptr && supports();
}

// The SDR white and peak, in nits, of the display |window| is on.
void QueryDisplayLuminance(HWND window, float* sdr_white, float* peak) {
  *sdr_white = 80.0f;
  *peak = 1000.0f;
  HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  ::GetMonitorInfoW(monitor, &info);

  UINT32 path_count = 0, mode_count = 0;
  if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                    &mode_count) == ERROR_SUCCESS) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                             &mode_count, modes.data(),
                             nullptr) == ERROR_SUCCESS) {
      for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) {
          continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
            white.SDRWhiteLevel > 0) {
          *sdr_white = white.SDRWhiteLevel / 1000.0f * 80.0f;
        }
      }
    }
  }

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
       a++) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
         o++) {
      Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 desc = {};
      if (SUCCEEDED(output.As(&output6)) &&
          SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor &&
          desc.MaxLuminance > 0) {
        *peak = desc.MaxLuminance;
        return;
      }
    }
  }
}

// The software renderer scales the frame before rotation.
// The GPU renderer applies rotation itself.
void RotateSoftwareFrame(const uint8_t* source, uint8_t* destination,
                         int width, int height, int clockwise) {
  const int output_width = clockwise == 180 ? width : height;
  for (int y = 0; y < height; ++y) {
    int index;
    int step;
    if (clockwise == 90) {
      index = height - 1 - y;
      step = output_width;
    } else if (clockwise == 270) {
      index = (width - 1) * output_width + y;
      step = -output_width;
    } else {
      index = (height - y) * output_width - 1;
      step = -1;
    }
    for (int x = 0; x < width; ++x, index += step, source += 4)
      std::memcpy(destination + index * 4, source, 4);
  }
}
}  // namespace

VideoOutput::VideoOutput(int64_t handle,
                         VideoOutputConfiguration configuration,
                         flutter::TextureRegistrar* registrar,
                         ThreadPool* thread_pool_ref,
                         const std::atomic<bool>* shutting_down,
                         IDXGIAdapter* adapter)
    : width_(configuration.width),
      height_(configuration.height),
      configuration_(configuration),
      handle_(reinterpret_cast<mpv_handle*>(handle)),
      registrar_(registrar),
      thread_pool_ref_(thread_pool_ref),
      shutting_down_(shutting_down) {
  // All outputs share one worker for D3D11 and libmpv rendering.
  auto future = thread_pool_ref_->Post([this, configuration, adapter]() {
    try {
      mpv_set_option_string(handle_, "video-sync", "audio");
      mpv_set_option_string(handle_, "video-timing-offset", "0");
      std::shared_ptr<D3D11SurfaceManager::Surface> first_target;
      if (configuration.enable_hardware_acceleration) {
        try {
          surface_manager_ = std::make_unique<D3D11SurfaceManager>(adapter);
          const auto size =
              media_kit::ResolveVideoFrameSize(1, 1, 0, width_, height_, false)
                  .value_or(media_kit::VideoFrameSize{1, 1});
          first_target =
              surface_manager_->CreateSurface(size.width, size.height);
          mpv_d3d11_init_params init{surface_manager_->device()};
          mpv_render_param params[] = {
              {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_D3D11},
              {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
              {MPV_RENDER_PARAM_INVALID, nullptr}};
          const auto error =
              mpv_render_context_create(&render_context_, handle_, params);
          if (error < 0)
            throw std::runtime_error(mpv_error_string(error));
        } catch (const std::exception& error) {
          std::cerr << "media_kit: D3D11 fallback: " << error.what()
                    << std::endl;
          first_target.reset();
          surface_manager_.reset();
        }
      }
      software_ = !surface_manager_;
      if (software_) {
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        const auto error =
            mpv_render_context_create(&render_context_, handle_, params);
        if (error < 0)
          throw std::runtime_error(mpv_error_string(error));
      }
      const auto size =
          media_kit::ResolveVideoFrameSize(1, 1, 0, width_, height_, software_)
              .value_or(media_kit::VideoFrameSize{1, 1});
      Resize(size.width, size.height, std::move(first_target));
      mpv_render_context_set_update_callback(
          render_context_,
          [](void* context) {
            static_cast<VideoOutput*>(context)->NotifyRender();
          },
          this);
      std::cout << "media_kit: VideoOutput: Using "
                << (software_ ? "S/W" : "D3D11") << " rendering." << std::endl;
    } catch (...) {
      CleanUp();
      throw;
    }
  });
  // Wait for initialization. Report any error to the caller.
  future.get();
}

VideoOutput::~VideoOutput() {
  {
    std::lock_guard<std::mutex> lock(notification_mutex_);
    destroyed_ = true;
  }
  // The worker finishes all accepted tasks before cleanup.
  // The shutdown flag blocks new render requests.
  // Unregister callbacks retain their own resources.
  thread_pool_ref_->Post([this]() { CleanUp(); }).get();
  for (auto& retirement : retirements_) {
    while (retirement.wait_for(std::chrono::milliseconds(10)) !=
           std::future_status::ready) {
      if (shutting_down_ && shutting_down_->load())
        break;
    }
  }
}

void VideoOutput::CleanUp() {
  if (render_context_) {
    mpv_render_context_set_update_callback(render_context_, nullptr, nullptr);
    mpv_render_context_free(render_context_);
    render_context_ = nullptr;
  }
  RetireTexture();
  surface_manager_.reset();
}

void VideoOutput::RetireTexture() {
  if (!texture_)
    return;
  auto retired = std::move(texture_);
  auto completion = std::make_shared<std::promise<void>>();
  retirements_.erase(
      std::remove_if(retirements_.begin(), retirements_.end(),
                     [](auto& future) {
                       return future.wait_for(std::chrono::seconds(0)) ==
                              std::future_status::ready;
                     }),
      retirements_.end());
  retirements_.push_back(completion->get_future());
  const auto id = retired->id;
  registrar_->UnregisterTexture(
      id, [retired = std::move(retired), completion]() mutable {
        // Keep the callback, descriptors, and COM references alive until
        // the engine releases this registration.
        retired.reset();
        completion->set_value();
      });
}

void VideoOutput::NotifyRender() {
  std::lock_guard<std::mutex> lock(notification_mutex_);
  if (destroyed_ || render_queued_)
    return;
  thread_pool_ref_->Post([this]() {
    {
      std::lock_guard<std::mutex> notification_lock(notification_mutex_);
      // Updates during this render may queue one more pass for this output.
      render_queued_ = false;
      if (destroyed_)
        return;
    }
    try {
      CheckAndResize();
      Render();
    } catch (const std::exception& error) {
      std::cerr << "media_kit: VideoOutput render failed: " << error.what()
                << std::endl;
    }
  });
  render_queued_ = true;
}

void VideoOutput::Render() {
  if (!texture_ || !render_context_)
    return;
  if (!NeedsRender())
    return;
  const auto& frame = texture_->frame;
  if (surface_manager_) {
    const auto format = frame->hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                   : DXGI_FORMAT_B8G8R8A8_UNORM;
    auto target = std::move(frame->first_target);
    if (!target)
      target = surface_manager_->CreateSurface(frame->size.width,
                                               frame->size.height, format);
    auto* render_target = target->texture.Get();
    if (frame->hdr) {
      if (!hdr_source_ || hdr_source_size_.width != frame->size.width ||
          hdr_source_size_.height != frame->size.height) {
        hdr_source_ = surface_manager_->CreateSurface(
            frame->size.width, frame->size.height,
            DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        hdr_source_size_ = frame->size;
      }
      render_target = hdr_source_->texture.Get();
    }
    mpv_d3d11_fbo fbo{render_target};
    mpv_render_param params[] = {{MPV_RENDER_PARAM_D3D11_FBO, &fbo},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    const auto error = mpv_render_context_render(render_context_, params);
    if (error < 0)
      throw std::runtime_error(mpv_error_string(error));
    if (frame->hdr) {
      hdr_encoder_->Encode(hdr_source_->texture.Get(), target->texture.Get(),
                           frame->size.width, frame->size.height, hdr_scale_);
    }
    surface_manager_->WaitForWrites();
    std::atomic_store(&frame->latest, std::move(target));
    // The SDR brightness slider and the display can change during a film.
    if (frame->hdr && ++hdr_frames_ >= 120)
      ApplyHdrBrightness();
  } else {
    // Flutter releases this lock only after uploading the returned pixel data.
    std::lock_guard<std::mutex> lock(frame->pixels_mutex);
    const auto layout =
        media_kit::GetSoftwareFrameLayout(frame->size, frame->pixels.size());
    if (!layout)
      throw std::runtime_error("Invalid software frame capacity");
    int size[]{frame->size.width, frame->size.height};
    size_t stride = layout->stride;
    auto* pixels = frame->pixels.data();
    if (software_rotation_) {
      if (software_rotation_ != 180)
        std::swap(size[0], size[1]);
      stride = static_cast<size_t>(size[0]) * 4;
      unrotated_pixels_.resize(layout->bytes);
      pixels = unrotated_pixels_.data();
    }
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, pixels},
        {MPV_RENDER_PARAM_INVALID, nullptr}};
    const auto error = mpv_render_context_render(render_context_, params);
    if (error < 0)
      throw std::runtime_error(mpv_error_string(error));
    if (software_rotation_)
      RotateSoftwareFrame(pixels, frame->pixels.data(), size[0], size[1],
                          software_rotation_);
  }
  registrar_->MarkTextureFrameAvailable(texture_->id);
}

void VideoOutput::SetTextureUpdateCallback(
    std::function<void(int64_t, int64_t, int64_t)> callback) {
  thread_pool_ref_
      ->Post([this, callback = std::move(callback)]() {
        texture_update_callback_ = callback;
        const auto size = GetVideoSize();
        texture_update_callback_(texture_->id, size ? size->width : 0,
                                 size ? size->height : 0);
      })
      .get();
}

void VideoOutput::SetSize(std::optional<int64_t> width,
                          std::optional<int64_t> height) {
  thread_pool_ref_->Post([this, width, height]() {
    if (destroyed_)
      return;
    width_ = width;
    height_ = height;
    redraw_ = true;
    // A paused player may never issue another update callback.
    try {
      CheckAndResize();
      Render();
    } catch (const std::exception& error) {
      std::cerr << "media_kit: VideoOutput resize failed: " << error.what()
                << std::endl;
    }
  });
}

void VideoOutput::CheckAndResize() {
  const auto size = GetVideoSize();
  if (!size)
    return;
  if (texture_ && texture_->frame->size.width == size->width &&
      texture_->frame->size.height == size->height)
    return;
  Resize(size->width, size->height);
}

void VideoOutput::Resize(
    int64_t required_width,
    int64_t required_height,
    std::shared_ptr<D3D11SurfaceManager::Surface> first_target) {
  redraw_ = true;
  auto next = std::make_shared<TextureRegistration>();
  auto frame = std::make_shared<Frame>();
  frame->size = {static_cast<int32_t>(required_width),
                 static_cast<int32_t>(required_height)};
  frame->hdr = hdr_ && surface_manager_;
  next->frame = frame;
  const auto register_texture = [&]() {
    next->id = registrar_->RegisterTexture(next->variant.get());
    if (next->id < 0)
      throw std::runtime_error("Flutter texture registration failed");
  };
  if (surface_manager_) {
    // Allocate the new surface before replacing the current registration.
    frame->first_target =
        first_target && !frame->hdr
            ? std::move(first_target)
            : surface_manager_->CreateSurface(
                  frame->size.width, frame->size.height,
                  frame->hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                             : DXGI_FORMAT_B8G8R8A8_UNORM);
    auto& descriptor = frame->gpu_descriptor;
    descriptor.struct_size = sizeof(descriptor);
    descriptor.width = descriptor.visible_width = frame->size.width;
    descriptor.height = descriptor.visible_height = frame->size.height;
    // kFlutterDesktopPixelFormatRGBA16Float (3) is only in engines with
    // FlutterDesktopEngineSupportsHdr; |SetHdr| checks for that.
    descriptor.format = frame->hdr ? static_cast<FlutterDesktopPixelFormat>(3)
                                   : kFlutterDesktopPixelFormatBGRA8888;
    next->variant =
        std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [frame](auto, auto) -> const FlutterDesktopGpuSurfaceDescriptor* {
              frame->imported = std::atomic_load(&frame->latest);
              if (!frame->imported)
                return nullptr;
              frame->gpu_descriptor.handle = frame->imported->handle;
              return &frame->gpu_descriptor;
            }));
    register_texture();
  } else {
    const auto layout = media_kit::GetSoftwareFrameLayout(
        frame->size, media_kit::kSoftwareBufferCapacity);
    if (!layout)
      throw std::runtime_error("Invalid software texture size");
    frame->pixels.resize(layout->bytes);
    auto& descriptor = frame->pixel_descriptor;
    descriptor.buffer = frame->pixels.data();
    descriptor.width = frame->size.width;
    descriptor.height = frame->size.height;
    descriptor.release_context = frame.get();
    descriptor.release_callback = [](void* context) {
      static_cast<Frame*>(context)->pixels_mutex.unlock();
    };
    next->variant = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([frame](auto, auto) {
          frame->pixels_mutex.lock();
          return &frame->pixel_descriptor;
        }));
    register_texture();
  }
  // The new callback owns its resources before Flutter can call it.
  RetireTexture();
  texture_ = std::move(next);
  texture_update_callback_(texture_->id, required_width, required_height);
}
std::optional<media_kit::VideoFrameSize> VideoOutput::GetVideoSize() {
  int64_t dw = 0, dh = 0, rotate = 0;
  // Read both dimensions from the same mpv property.
  // mpv can omit this property before playback or for audio-only media.
  mpv_node params{};
  if (mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params) >=
      0) {
    if (params.format == MPV_FORMAT_NODE_MAP && params.u.list != nullptr) {
      for (int32_t i = 0; i < params.u.list->num; ++i) {
        const char* key = params.u.list->keys[i];
        const auto& value = params.u.list->values[i];
        if (key != nullptr && value.format == MPV_FORMAT_INT64) {
          if (strcmp(key, "dw") == 0)
            dw = value.u.int64;
          if (strcmp(key, "dh") == 0)
            dh = value.u.int64;
          if (strcmp(key, "rotate") == 0)
            rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }
  if (software_) {
    const int angle = static_cast<int>((rotate % 360 + 360) % 360);
    software_rotation_ = angle % 90 == 0 ? angle : 0;
  }
  return media_kit::ResolveVideoFrameSize(dw, dh, rotate, width_, height_,
                                          software_);
}

bool VideoOutput::NeedsRender() {
  const auto frame =
      mpv_render_context_update(render_context_) & MPV_RENDER_UPDATE_FRAME;
  if (!frame && !redraw_)
    return false;
  redraw_ = false;
  return true;
}

bool VideoOutput::SetHdr(bool enabled, HWND window) {
  if (enabled && !EngineSupportsHdr()) {
    std::cout << "media_kit: VideoOutput: engine has no HDR support"
              << std::endl;
    return false;
  }
  // The pool's tasks return nothing; the result comes back through this.
  auto done = std::make_shared<std::promise<bool>>();
  auto result = done->get_future();
  thread_pool_ref_->Post([this, enabled, window, done]() {
    done->set_value(SwitchHdr(enabled, window));
  });
  return result.get();
}

bool VideoOutput::SwitchHdr(bool enabled, HWND window) {
  if (destroyed_)
    return false;
  if (window != nullptr)
    window_ = window;
  if (!surface_manager_)
    return !enabled;
  if (enabled == hdr_)
    return true;
  if (enabled && !hdr_encoder_) {
    try {
      hdr_encoder_ =
          std::make_unique<D3D11HdrEncoder>(surface_manager_->device());
    } catch (const std::exception& error) {
      std::cerr << "media_kit: " << error.what() << std::endl;
      return false;
    }
  }
  hdr_ = enabled;
  hdr_target_peak_ = 0;
  if (enabled) {
    // Linear BT.709 light for the encode pass.
    mpv_set_property_string(handle_, "target-trc", "linear");
    mpv_set_property_string(handle_, "target-prim", "bt.709");
    // Highlight roll-off: the ITU's BT.2390 curve, in PQ (perceptual)
    // space, against each scene's measured peak, so a dark scene with
    // no bright highlights is left alone. Luminance-based, so hues
    // hold.
    mpv_set_property_string(handle_, "tone-mapping", "bt.2390");
    mpv_set_property_string(handle_, "hdr-compute-peak", "yes");
    ApplyHdrBrightness();
  } else {
    mpv_set_property_string(handle_, "target-trc", "auto");
    mpv_set_property_string(handle_, "target-prim", "auto");
    mpv_set_property_string(handle_, "target-peak", "auto");
    mpv_set_property_string(handle_, "tone-mapping", "auto");
    mpv_set_property_string(handle_, "hdr-compute-peak", "auto");
    hdr_source_.reset();
  }
  std::cout << "media_kit: VideoOutput: HDR " << (enabled ? "on" : "off")
            << std::endl;
  // A texture of the new format, drawn at once: a paused film too.
  try {
    if (texture_)
      Resize(texture_->frame->size.width, texture_->frame->size.height);
    Render();
  } catch (const std::exception& error) {
    std::cerr << "media_kit: VideoOutput HDR switch failed: "
              << error.what() << std::endl;
  }
  return true;
}

void VideoOutput::SetHdrPaperWhite(float nits, HWND window) {
  thread_pool_ref_->Post([this, nits, window]() {
    if (destroyed_)
      return;
    if (window != nullptr)
      window_ = window;
    hdr_paper_white_ = nits;
    if (!hdr_)
      return;
    hdr_target_peak_ = 0;
    ApplyHdrBrightness();
    // Paused, no new frame comes to carry the new level: draw this one
    // again.
    redraw_ = true;
    try {
      Render();
    } catch (const std::exception& error) {
      std::cerr << "media_kit: VideoOutput render failed: " << error.what()
                << std::endl;
    }
  });
}

void VideoOutput::ApplyHdrBrightness() {
  hdr_frames_ = 0;
  float sdr_white = 80.0f, peak = 1000.0f;
  QueryDisplayLuminance(window_, &sdr_white, &peak);
  // mpv's 1.0 is paper white; the compositor shows 1.0 at SDR white, so
  // paper white lands at |hdr_paper_white_| nits with this scale. The SDR
  // brightness slider only cancels out here: the picture does not follow it.
  //
  // Brightening is a gain, not an offset: black stays black and shadows keep
  // their contrast. The film's highlights are not lifted with it; mpv rolls
  // them off (BT.2390, per-scene peak) into the room the panel has above the
  // new paper white, so the brightest still reach the panel's peak.
  const float paper_white = (std::max)(
      kMpvReferenceWhiteNits,
      (std::min)(hdr_paper_white_, peak / kMinHighlightHeadroom));
  hdr_scale_ = paper_white / sdr_white;
  // The panel's headroom above paper white, in mpv's nits (paper white =
  // 203): tone mapping fits the film's highlights into it.
  const float headroom = (std::max)(1.0f, peak / paper_white);
  const int target_peak =
      static_cast<int>(kMpvReferenceWhiteNits * headroom + 0.5f);
  if (target_peak == hdr_target_peak_) {
    return;
  }
  hdr_target_peak_ = target_peak;
  mpv_set_property_string(handle_, "target-peak",
                          std::to_string(target_peak).c_str());
  std::cout << "media_kit: VideoOutput: HDR paper white " << paper_white
            << " nits (asked " << hdr_paper_white_ << "), headroom " << headroom << "x (SDR white " << sdr_white
            << " nits, peak " << peak << " nits), mpv target-peak "
            << target_peak << std::endl;
}
