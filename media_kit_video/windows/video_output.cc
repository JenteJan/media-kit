// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "video_output.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "dxgi.lib")

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

}  // namespace

// Limit the frame size to 1080p in software rendering.
// This is for performance reasons & to avoid allocating too much memory.
#define SW_RENDERING_MAX_WIDTH 1920
#define SW_RENDERING_MAX_HEIGHT 1080
#define SW_RENDERING_PIXEL_BUFFER_SIZE \
  (SW_RENDERING_MAX_WIDTH) * (SW_RENDERING_MAX_HEIGHT) * (4)

VideoOutput::VideoOutput(int64_t handle,
                         VideoOutputConfiguration configuration,
                         flutter::PluginRegistrarWindows* registrar,
                         ThreadPool* thread_pool_ref)
    : handle_(reinterpret_cast<mpv_handle*>(handle)),
      width_(configuration.width),
      height_(configuration.height),
      configuration_(configuration),
      registrar_(registrar),
      thread_pool_ref_(thread_pool_ref) {
  // The constructor must be invoked through the thread pool, because
  // |ANGLESurfaceManager| & libmpv render context creation can conflict with
  // the existing |Render| or |Resize| calls from another |VideoOutput|
  // instances (which will result in access violation).
  auto future = thread_pool_ref_->Post([&]() {
    mpv_set_option_string(handle_, "video-sync", "audio");
    mpv_set_option_string(handle_, "video-timing-offset", "0");
    // First try to initialize video playback with hardware acceleration &
    // |ANGLESurfaceManager|, use S/W API as fallback.
    auto is_hardware_acceleration_enabled = false;
    // Attempt to use H/W rendering.
    if (configuration.enable_hardware_acceleration) {
      try {
        // OpenGL context needs to be set before |mpv_render_context_create|.
        surface_manager_ = std::make_unique<ANGLESurfaceManager>(
            static_cast<int32_t>(width_.value_or(1)),
            static_cast<int32_t>(height_.value_or(1)));
        surface_manager_->MakeCurrent(true);
        Resize(width_.value_or(1), height_.value_or(1));
        mpv_opengl_init_params gl_init_params{
            [](auto, auto name) {
              return reinterpret_cast<void*>(eglGetProcAddress(name));
            },
            nullptr,
        };
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        // Create render context.
        if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
          mpv_render_context_set_update_callback(
              render_context_,
              [](void* context) {
                // Notify Flutter that a new frame is available. The actual
                // rendering will take place in the |Render| method, which will
                // be called by Flutter on the render thread.
                auto that = reinterpret_cast<VideoOutput*>(context);
                that->NotifyRender();
              },
              reinterpret_cast<void*>(this));
          // Set flag to true, indicating that H/W rendering is supported.
          is_hardware_acceleration_enabled = true;
          std::cout << "media_kit: VideoOutput: Using H/W rendering."
                    << std::endl;
        }
      } catch (...) {
        // Do nothing.
        // Likely received an |std::runtime_error| from |ANGLESurfaceManager|,
        // which indicates that H/W rendering is not supported.
      }
    }
    if (!is_hardware_acceleration_enabled) {
      std::cout << "media_kit: VideoOutput: Using S/W rendering." << std::endl;
      // Allocate a "large enough" buffer ahead of time.
      pixel_buffer_ =
          std::make_unique<uint8_t[]>(SW_RENDERING_PIXEL_BUFFER_SIZE);
      Resize(width_.value_or(1), height_.value_or(1));
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      if (mpv_render_context_create(&render_context_, handle_, params) == 0) {
        mpv_render_context_set_update_callback(
            render_context_,
            [](void* context) {
              // Notify Flutter that a new frame is available. The actual
              // rendering will take place in the |Render| method, which will be
              // called by Flutter on the render thread.
              auto that = reinterpret_cast<VideoOutput*>(context);
              that->NotifyRender();
            },
            reinterpret_cast<void*>(this));
      }
    }
  });
  future.wait();
}

VideoOutput::~VideoOutput() {
  destroyed_ = true;
  auto promise = std::promise<void>();
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(
        texture_id_, [&, texture_id = texture_id_]() {
          // Add one more task into the thread pool queue & exit the destructor
          // only when it gets executed. This will ensure that all the tasks
          // posted to the thread pool i.e. render or resize before this are
          // executed (and won't reference the dead object anymore), most
          // notably |CheckAndResize| & |Render|.
          auto future = thread_pool_ref_->Post([&, id = texture_id]() {
            std::cout << "media_kit: VideoOutput: Free Texture: " << id
                      << std::endl;
            std::cout << "VideoOutput::~VideoOutput: "
                      << reinterpret_cast<int64_t>(handle_) << std::endl;
            std::lock_guard<std::mutex> lock(textures_mutex_);
            texture_variants_.clear();
            // H/W
            textures_.clear();
            // S/W
            pixel_buffer_textures_.clear();
            // Free (call destructor) |ANGLESurfaceManager| through the thread
            // pool. This will ensure synchronized EGL or ANGLE usage & won't
            // conflict with |Render| or |CheckAndResize| of other
            // |VideoOutput|s.
            surface_manager_.reset(nullptr);
            promise.set_value();
          });
        });
  }

  promise.get_future().wait();
  texture_id_ = 0;

  thread_pool_ref_->Post([render_context = render_context_]() {
    mpv_render_context_free(render_context);
  });
}

void VideoOutput::NotifyRender() {
  if (destroyed_) {
    return;
  }
  thread_pool_ref_->Post(std::bind(&VideoOutput::CheckAndResize, this));
  thread_pool_ref_->Post(std::bind(&VideoOutput::Render, this));
}

void VideoOutput::Render() {
  if (texture_id_) {
    // mpv calls back for more than new frames (about twice per frame while
    // a film plays). Rendering and telling Flutter on every call drew each
    // frame twice: twice the GL work, the driver's, Flutter's compositing
    // and the window's present. Only a new frame, or a surface that was just
    // (re)made and is still empty, is worth drawing.
    const auto flags = mpv_render_context_update(render_context_);
    if (!(flags & MPV_RENDER_UPDATE_FRAME) && !redraw_pending_) {
      return;
    }
    redraw_pending_ = false;
    // H/W
    if (surface_manager_ != nullptr) {
      surface_manager_->Draw([&]() {
        mpv_opengl_fbo fbo{
            static_cast<int>(surface_manager_->render_fbo()),
            surface_manager_->width(),
            surface_manager_->height(),
            surface_manager_->render_internal_format(),
        };
        mpv_render_param params[]{
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(render_context_, params);
      });
      // The SDR brightness slider can move during a film.
      if (surface_manager_->hdr() && ++hdr_frames_ >= 120) {
        ApplyHdrBrightness();
      }
    }
    // S/W
    if (pixel_buffer_ != nullptr) {
      int32_t size[]{
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->width),
          static_cast<int32_t>(pixel_buffer_textures_.at(texture_id_)->height),
      };
      auto pitch = 4 * size[0];
      mpv_render_param params[]{
          {MPV_RENDER_PARAM_SW_SIZE, size},
          {MPV_RENDER_PARAM_SW_FORMAT, "rgb0"},
          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
          {MPV_RENDER_PARAM_SW_POINTER, pixel_buffer_.get()},
          {MPV_RENDER_PARAM_INVALID, nullptr},
      };
      mpv_render_context_render(render_context_, params);
    }
    try {
      // Notify Flutter that a new frame is available.
      registrar_->texture_registrar()->MarkTextureFrameAvailable(texture_id_);
    } catch (...) {
      // Prevent any redundant exceptions if the texture is unregistered etc.
    }
  }
}

void VideoOutput::SetTextureUpdateCallback(
    std::function<void(int64_t, int64_t, int64_t)> callback) {
  texture_update_callback_ = callback;
  texture_update_callback_(texture_id_, GetVideoWidth(), GetVideoHeight());
}

void VideoOutput::SetSize(std::optional<int64_t> width,
                          std::optional<int64_t> height) {
  thread_pool_ref_->Post([&, width, height]() {
    if (width.has_value()) {
      // H/W
      if (surface_manager_ != nullptr) {
        width_ = width.value();
      }
      // S/W
      if (pixel_buffer_ != nullptr) {
        // Limit width if software rendering is being used.
        width_ = std::clamp(width.value(), static_cast<int64_t>(0),
                            static_cast<int64_t>(SW_RENDERING_MAX_WIDTH));
      }
    } else {
      width_ = std::nullopt;
    }
    if (height.has_value()) {
      // H/W
      if (surface_manager_ != nullptr) {
        height_ = height.value();
      }
      // S/W
      if (pixel_buffer_ != nullptr) {
        // Limit width if software rendering is being used.
        height_ = std::clamp(height.value(), static_cast<int64_t>(0),
                             static_cast<int64_t>(SW_RENDERING_MAX_HEIGHT));
      }
    } else {
      height_ = std::nullopt;
    }
  });
}

void VideoOutput::CheckAndResize() {
  // Check if a new texture with different dimensions is needed.
  auto required_width = GetVideoWidth(), required_height = GetVideoHeight();
  if (required_width < 1 || required_height < 1) {
    // Invalid.
    return;
  }
  int64_t current_width = -1, current_height = -1;
  if (surface_manager_ != nullptr) {
    current_width = surface_manager_->width();
    current_height = surface_manager_->height();
  }
  if (pixel_buffer_ != nullptr) {
    current_width = pixel_buffer_textures_.at(texture_id_)->width;
    current_height = pixel_buffer_textures_.at(texture_id_)->height;
  }
  // Currently rendered video output dimensions.
  // Either H/W or S/W rendered.
  assert(current_width > 0);
  assert(current_height > 0);
  if (required_width == current_width && required_height == current_height) {
    // No creation of new texture required.
    return;
  }
  Resize(required_width, required_height);
}

void VideoOutput::Resize(int64_t required_width, int64_t required_height) {
  std::cout << required_width << " " << required_height << std::endl;
  // Unregister previously registered texture & delete underlying objects.
  if (texture_id_) {
    registrar_->texture_registrar()->UnregisterTexture(
        texture_id_, [&, id = texture_id_]() {
          if (id) {
            std::cout << "media_kit: VideoOutput: Free Texture: " << id
                      << std::endl;
            std::lock_guard<std::mutex> lock(textures_mutex_);
            if (destroyed_) {
              return;
            }
            if (texture_variants_.find(id) != texture_variants_.end()) {
              texture_variants_.erase(id);
            }
            // H/W
            if (textures_.find(id) != textures_.end()) {
              textures_.erase(id);
            }
            // S/W
            if (pixel_buffer_textures_.find(id) !=
                pixel_buffer_textures_.end()) {
              pixel_buffer_textures_.erase(id);
            }
          }
        });
    texture_id_ = 0;
  }
  // H/W
  if (surface_manager_ != nullptr) {
    // Destroy internal ID3D11Texture2D & EGLSurface & create new with updated
    // dimensions while preserving previous EGLDisplay & EGLContext.
    surface_manager_->SetSize(static_cast<int32_t>(required_width),
                              static_cast<int32_t>(required_height));
    auto texture = std::make_unique<FlutterDesktopGpuSurfaceDescriptor>();
    texture->struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    texture->handle = surface_manager_->handle();
    texture->width = texture->visible_width = surface_manager_->width();
    texture->height = texture->visible_height = surface_manager_->height();
    texture->release_context = nullptr;
    texture->release_callback = [](void*) {};
    // kFlutterDesktopPixelFormatRGBA16Float (3) is only in engines with
    // FlutterDesktopEngineSupportsHdr; |SetHdr| checks for that.
    texture->format = surface_manager_->hdr()
                          ? static_cast<FlutterDesktopPixelFormat>(3)
                          : kFlutterDesktopPixelFormatBGRA8888;
    auto texture_variant =
        std::make_unique<flutter::TextureVariant>(flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle, [&](auto, auto) {
              std::lock_guard<std::mutex> lock(textures_mutex_);
              if (texture_id_) {
                surface_manager_->Read();
                return textures_.at(texture_id_).get();
              } else {
                return (FlutterDesktopGpuSurfaceDescriptor*)nullptr;
              }
            }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    std::lock_guard<std::mutex> lock(textures_mutex_);
    textures_.emplace(std::make_pair(texture_id_, std::move(texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
    redraw_pending_ = true;
  }
  // S/W
  if (pixel_buffer_ != nullptr) {
    auto pixel_buffer_texture = std::make_unique<FlutterDesktopPixelBuffer>();
    pixel_buffer_texture->buffer = pixel_buffer_.get();
    pixel_buffer_texture->width = required_width;
    pixel_buffer_texture->height = required_height;
    pixel_buffer_texture->release_context = nullptr;
    pixel_buffer_texture->release_callback = [](void*) {};
    auto texture_variant = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([&](auto, auto) {
          std::lock_guard<std::mutex> lock(textures_mutex_);
          if (texture_id_) {
            return pixel_buffer_textures_.at(texture_id_).get();
          } else {
            return (FlutterDesktopPixelBuffer*)nullptr;
          }
        }));
    // Register new texture.
    texture_id_ =
        registrar_->texture_registrar()->RegisterTexture(texture_variant.get());
    std::cout << "media_kit: VideoOutput: Create Texture: " << texture_id_
              << std::endl;
    std::lock_guard<std::mutex> lock(textures_mutex_);
    pixel_buffer_textures_.emplace(
        std::make_pair(texture_id_, std::move(pixel_buffer_texture)));
    texture_variants_.emplace(
        std::make_pair(texture_id_, std::move(texture_variant)));
    // Notify public texture update callback.
    texture_update_callback_(texture_id_, required_width, required_height);
    redraw_pending_ = true;
  }
}

void VideoOutput::ApplyHdrBrightness() {
  hdr_frames_ = 0;
  float sdr_white = 80.0f, peak = 1000.0f;
  HWND window = registrar_->GetView() != nullptr
                    ? registrar_->GetView()->GetNativeWindow()
                    : nullptr;
  QueryDisplayLuminance(window, &sdr_white, &peak);
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
  surface_manager_->SetHdrScale(paper_white / sdr_white);
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

void VideoOutput::SetHdrPaperWhite(float nits) {
  if (surface_manager_ == nullptr) {
    return;
  }
  thread_pool_ref_->Post([&, nits]() {
    hdr_paper_white_ = nits;
    if (surface_manager_->hdr()) {
      hdr_target_peak_ = 0;
      ApplyHdrBrightness();
    }
  });
}

bool VideoOutput::SetHdr(bool enabled) {
  if (surface_manager_ == nullptr) {
    return !enabled;
  }
  if (enabled && !EngineSupportsHdr()) {
    std::cout << "media_kit: VideoOutput: engine has no HDR support"
              << std::endl;
    return false;
  }
  // The pool's tasks return nothing; the result comes back through this.
  auto done = std::make_shared<std::promise<bool>>();
  auto result = done->get_future();
  thread_pool_ref_->Post([&, done]() {
    done->set_value([&]() -> bool {
    // The scale is set by |ApplyHdrBrightness| from the paper white level.
    if (!surface_manager_->SetHdr(enabled, 1.0f)) {
      return false;
    }
    if (enabled) {
      // Linear BT.709 light for the encode pass.
      mpv_set_property_string(handle_, "target-trc", "linear");
      mpv_set_property_string(handle_, "target-prim", "bt.709");
      // Highlight roll-off: the ITU's BT.2390 curve, in PQ (perceptual)
      // space, against each scene's measured peak, so a dark scene with no
      // bright highlights is left alone. Luminance-based, so hues hold.
      mpv_set_property_string(handle_, "tone-mapping", "bt.2390");
      mpv_set_property_string(handle_, "hdr-compute-peak", "yes");
      hdr_target_peak_ = 0;
      ApplyHdrBrightness();
    } else {
      mpv_set_property_string(handle_, "target-trc", "auto");
      mpv_set_property_string(handle_, "target-prim", "auto");
      mpv_set_property_string(handle_, "target-peak", "auto");
      mpv_set_property_string(handle_, "tone-mapping", "auto");
      mpv_set_property_string(handle_, "hdr-compute-peak", "auto");
      hdr_target_peak_ = 0;
    }
    std::cout << "media_kit: VideoOutput: HDR " << (enabled ? "on" : "off")
              << std::endl;
    // A texture of the new format.
    if (texture_id_) {
      Resize(surface_manager_->width(), surface_manager_->height());
    }
    return true;
    }());
  });
  return result.get();
}

int64_t VideoOutput::GetVideoWidth() {
  // Fixed width.
  if (width_) {
    return width_.value();
  }
  // Video resolution dependent width.
  int64_t width = 0;
  int64_t height = 0;

  mpv_node params;
  mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (pixel_buffer_ != nullptr) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect-ratio.
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

int64_t VideoOutput::GetVideoHeight() {
  // Fixed height.
  if (height_) {
    return height_.value();
  }
  // Video resolution dependent height.
  int64_t width = 0;
  int64_t height = 0;

  mpv_node params;
  mpv_get_property(handle_, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (pixel_buffer_ != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect-ratio.
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}
