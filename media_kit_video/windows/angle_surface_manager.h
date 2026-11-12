// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#ifndef ANGLE_SURFACE_MANAGER_H_
#define ANGLE_SURFACE_MANAGER_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglplatform.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <Windows.h>

#include <d3d.h>
#include <d3d11.h>
#include <wrl.h>

#include "utils.h"

#include <cstdint>
#include <functional>

// |ANGLESurfaceManager| provides an abstraction around ANGLE to easily draw
// OpenGL ES 2.0 content & read as D3D 11 texture using shared |HANDLE|.
// * |Draw|: Takes callback where OpenGL ES 2.0 calls can be made for rendering.
// * |Read|: Copies the drawn content to D3D 11 texture & makes it available to
//           the shared |handle| for access.

// A large part of implementation is inspired from Flutter.
// https://github.com/flutter/engine/blob/master/shell/platform/windows/angle_surface_manager.h

class ANGLESurfaceManager {
 public:
  const int32_t width() const { return width_; }
  const int32_t height() const { return height_; }
  const HANDLE handle() const { return handle_; }

  ANGLESurfaceManager(int32_t width, int32_t height);

  ~ANGLESurfaceManager();

  void SetSize(int32_t width, int32_t height);

  void Draw(std::function<void()> callback);

  void Read();

  void MakeCurrent(bool value);

  // HDR output: the shared texture becomes R16G16B16A16_FLOAT and |Draw|'s
  // callback renders into |render_fbo| (half float, linear light), which is
  // then encoded as extended sRGB times |scale| into the shared texture.
  // Returns false when this ANGLE cannot do it (no ES 3 or no-config context).
  bool SetHdr(bool hdr, float scale);
  // The encode pass's scale, without recreating anything.
  void SetHdrScale(float scale);
  bool hdr() const { return hdr_; }
  // The framebuffer mpv renders into: 0 (the shared texture) unless HDR.
  uint32_t render_fbo() const { return hdr_ ? hdr_fbo_ : 0; }
  // Its internal format, for |mpv_opengl_fbo|: 0 (unknown) unless HDR.
  int32_t render_internal_format() const { return hdr_ ? 0x881A : 0; }

 private:
  bool EnsureHdrResources();
  void EncodeHdr();
  void ReleaseHdrResources();
  void SwapBuffers();

  void Create();

  void CleanUp(bool release_context);

  bool CreateD3DTexture();

  bool CreateEGLDisplay();

  bool CreateAndBindEGLSurface();

  int32_t width_ = 1;
  int32_t height_ = 1;
  HANDLE internal_handle_ = nullptr;
  HANDLE handle_ = nullptr;

  // Sync |Draw| & |Read| calls.
  HANDLE mutex_ = nullptr;
  // D3D 11
  ID3D11Device* d3d_11_device_ = nullptr;
  ID3D11DeviceContext* d3d_11_device_context_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> internal_d3d_11_texture_2D_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d_11_texture_2D_;
  // ANGLE
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = nullptr;
  EGLConfig config_ = nullptr;
  // Half-float config for the HDR shared texture.
  EGLConfig hdr_config_ = nullptr;
  // Whether |context_| is an ES 3, no-config context (needed for HDR).
  bool hdr_capable_ = false;
  bool hdr_ = false;
  float hdr_scale_ = 1.0f;
  // Intermediate half-float target and the encode program.
  uint32_t hdr_texture_ = 0;
  uint32_t hdr_fbo_ = 0;
  int32_t hdr_fbo_width_ = 0;
  int32_t hdr_fbo_height_ = 0;
  uint32_t hdr_program_ = 0;
  uint32_t hdr_vertex_array_ = 0;
  uint32_t hdr_vertex_buffer_ = 0;
  int32_t hdr_scale_location_ = -1;

  static constexpr EGLint kEGLConfigurationAttributes[] = {
      EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE,    8,
      EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 8, EGL_STENCIL_SIZE, 8,
      EGL_NONE,
  };
  static constexpr EGLint kEGLContextAttributes[] = {
      EGL_CONTEXT_CLIENT_VERSION,
      2,
      EGL_NONE,
  };
  static constexpr EGLint kD3D11DisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };
  static constexpr EGLint kD3D11_9_3DisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE,
      9,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE,
      3,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };
  static constexpr EGLint kD3D9DisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D9_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
      EGL_NONE,
  };
  static constexpr EGLint kWrapDisplayAttributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };

  // Number of active instances of ANGLESurfaceManager.
  static int32_t instance_count_;
};

#endif  // ANGLE_SURFACE_MANAGER_H_
