// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.
#include "angle_surface_manager.h"

#include <iostream>

// The bundled ANGLE ships GLES2 headers only; the few ES 3 pieces the HDR
// pass needs are declared here and the vertex array calls resolved at run
// time (the context is ES 3 whenever HDR is available).
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

namespace {
using GenVertexArraysFn = void(GL_APIENTRY*)(GLsizei, GLuint*);
using BindVertexArrayFn = void(GL_APIENTRY*)(GLuint);
using DeleteVertexArraysFn = void(GL_APIENTRY*)(GLsizei, const GLuint*);
GenVertexArraysFn gen_vertex_arrays = nullptr;
BindVertexArrayFn bind_vertex_array = nullptr;
DeleteVertexArraysFn delete_vertex_arrays = nullptr;

bool LoadVertexArrayFunctions() {
  if (bind_vertex_array == nullptr) {
    gen_vertex_arrays = reinterpret_cast<GenVertexArraysFn>(
        eglGetProcAddress("glGenVertexArrays"));
    bind_vertex_array = reinterpret_cast<BindVertexArrayFn>(
        eglGetProcAddress("glBindVertexArray"));
    delete_vertex_arrays = reinterpret_cast<DeleteVertexArraysFn>(
        eglGetProcAddress("glDeleteVertexArrays"));
  }
  return gen_vertex_arrays && bind_vertex_array && delete_vertex_arrays;
}
}  // namespace

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

#define FAIL(message)                                                 \
  std::cout << "media_kit: ANGLESurfaceManager: Failure: " << message \
            << std::endl;                                             \
  return false

#define CHECK_HRESULT(message) \
  if (FAILED(hr)) {            \
    FAIL(message);             \
  }

int ANGLESurfaceManager::instance_count_ = 0;

ANGLESurfaceManager::ANGLESurfaceManager(int32_t width, int32_t height)
    : width_(width), height_(height) {
  mutex_ = ::CreateMutex(NULL, FALSE, NULL);
  Create();
  instance_count_++;
}

ANGLESurfaceManager::~ANGLESurfaceManager() {
  CleanUp(true);
  ::ReleaseMutex(mutex_);
  ::CloseHandle(mutex_);
  instance_count_--;
}

void ANGLESurfaceManager::SetSize(int32_t width, int32_t height) {
  if (width == width_ && height == height_) {
    return;
  }
  width_ = width;
  height_ = height;
  Create();
}

void ANGLESurfaceManager::Draw(std::function<void()> callback) {
  ::WaitForSingleObject(mutex_, INFINITE);
  MakeCurrent(true);
  if (hdr_ && !EnsureHdrResources()) {
    std::cout << "media_kit: ANGLESurfaceManager: HDR target failed, SDR output"
              << std::endl;
    hdr_ = false;
  }
  callback();
  if (hdr_) {
    EncodeHdr();
  }
  SwapBuffers();
  MakeCurrent(false);
  ::ReleaseMutex(mutex_);
}

void ANGLESurfaceManager::Read() {
  ::WaitForSingleObject(mutex_, INFINITE);
  if (d3d_11_device_context_ != nullptr) {
    d3d_11_device_context_->CopyResource(d3d_11_texture_2D_.Get(),
                                         internal_d3d_11_texture_2D_.Get());
    d3d_11_device_context_->Flush();
  }
  ::ReleaseMutex(mutex_);
}

void ANGLESurfaceManager::MakeCurrent(bool value) {
  if (value) {
    eglMakeCurrent(display_, surface_, surface_, context_);
  } else {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
}

void ANGLESurfaceManager::SwapBuffers() {
  glFinish();
}

void ANGLESurfaceManager::Create() {
  CleanUp(false);
  if (!CreateD3DTexture()) {
    throw std::runtime_error("Unable to create Windows Direct3D device.");
    return;
  }
  if (!CreateEGLDisplay()) {
    throw std::runtime_error("Unable to create ANGLE EGL display.");
    return;
  }
  if (!CreateAndBindEGLSurface()) {
    throw std::runtime_error("Unable to create ANGLE EGL surface.");
    return;
  }
  if (internal_handle_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("Unable to retrieve Direct3D shared HANDLE.");
    return;
  }
}

void ANGLESurfaceManager::CleanUp(bool release_context) {
  if (release_context) {
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
        surface_ != EGL_NO_SURFACE) {
      eglMakeCurrent(display_, surface_, surface_, context_);
      ReleaseHdrResources();
      eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
      eglReleaseTexImage(display_, surface_, EGL_BACK_BUFFER);
    }
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(display_, context_);
      context_ = EGL_NO_CONTEXT;
    }
    if (surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(display_, surface_);
      surface_ = EGL_NO_SURFACE;
    }
    if (instance_count_ == 1) {
      eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    // Release D3D device & context if the instance is being destroyed.
    if (d3d_11_device_context_) {
      d3d_11_device_context_->Release();
      d3d_11_device_context_ = nullptr;
    }
    if (d3d_11_device_) {
      d3d_11_device_->Release();
      d3d_11_device_ = nullptr;
    }
  } else {
    // Clear context & destroy existing |surface_|.
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(display_, surface_);
    }
    surface_ = EGL_NO_SURFACE;
  }
  // Release D3D 11 texture(s).
  if (internal_d3d_11_texture_2D_) {
    internal_d3d_11_texture_2D_->Release();
    internal_d3d_11_texture_2D_ = nullptr;
  }
  if (d3d_11_texture_2D_) {
    d3d_11_texture_2D_->Release();
    d3d_11_texture_2D_ = nullptr;
  }
}

bool ANGLESurfaceManager::CreateD3DTexture() {
  if (d3d_11_device_ == nullptr) {
    // NOTE: Not enabling Feature Level 12. It crashes directly on Windows 7.
    const auto feature_levels = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
    };

    IDXGIAdapter* adapter = nullptr;
    D3D_DRIVER_TYPE driver_type = D3D_DRIVER_TYPE_UNKNOWN;

    // NOTE: Automatically selecting adapter on Windows 10 RTM or greater.
    if (Utils::IsWindows10RTMOrGreater()) {
      adapter = NULL;
      driver_type = D3D_DRIVER_TYPE_HARDWARE;
    } else {
      IDXGIFactory* dxgi = nullptr;
      ::CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&dxgi);
      // As far as my experience goes, this is the safest approach. Passing NULL
      // (so-called default) seems to cause issues on Windows 7 or maybe some
      // older graphics drivers. First adapter is the default.
      // D3D_DRIVER_TYPE_UNKNOWN| must be passed with manual adapter selection.
      dxgi->EnumAdapters(0, &adapter);
      dxgi->Release();
    }

    auto hr = ::D3D11CreateDevice(
        adapter, driver_type, 0, 0, feature_levels.begin(),
        static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION,
        &d3d_11_device_, 0, &d3d_11_device_context_);
    CHECK_HRESULT("D3D11CreateDevice");
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device = nullptr;
  auto dxgi_device_success = d3d_11_device_->QueryInterface(
      __uuidof(IDXGIDevice), (void**)&dxgi_device);
  if (SUCCEEDED(dxgi_device_success) && dxgi_device != nullptr) {
    dxgi_device->SetGPUThreadPriority(5);  // Must be in interval [-7, 7].
  }

  auto level = d3d_11_device_->GetFeatureLevel();
  std::cout << "media_kit: ANGLESurfaceManager: Direct3D Feature Level: "
            << (((unsigned)level) >> 12) << "_"
            << ((((unsigned)level) >> 8) & 0xf) << std::endl;
  auto d3d11_texture2D_desc = D3D11_TEXTURE2D_DESC{0};
  d3d11_texture2D_desc.Width = width_;
  d3d11_texture2D_desc.Height = height_;
  d3d11_texture2D_desc.Format =
      hdr_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
  d3d11_texture2D_desc.MipLevels = 1;
  d3d11_texture2D_desc.ArraySize = 1;
  d3d11_texture2D_desc.SampleDesc.Count = 1;
  d3d11_texture2D_desc.SampleDesc.Quality = 0;
  d3d11_texture2D_desc.Usage = D3D11_USAGE_DEFAULT;
  d3d11_texture2D_desc.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d3d11_texture2D_desc.CPUAccessFlags = 0;
  d3d11_texture2D_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  // The general idea is to create two textures, one that is used to |Draw|
  // using ANGLE & another one that is used for |Read| the rendered content
  // using |handle|.
  // The internal texture is copied to the public texture once a frame is
  // requested using |ID3D11DeviceContext::CopyResource|. This prevents any kind
  // of synchronization issues.

  // Internal.
  auto hr = d3d_11_device_->CreateTexture2D(&d3d11_texture2D_desc, nullptr,
                                            &internal_d3d_11_texture_2D_);
  CHECK_HRESULT("ID3D11Device::CreateTexture2D");
  auto resource = Microsoft::WRL::ComPtr<IDXGIResource>{};
  hr = internal_d3d_11_texture_2D_.As(&resource);
  CHECK_HRESULT("ID3D11Texture2D::As");
  // Retrieve the shared |HANDLE| for interop.
  hr = resource->GetSharedHandle(&internal_handle_);
  CHECK_HRESULT("IDXGIResource::GetSharedHandle");
  internal_d3d_11_texture_2D_->AddRef();

  // External.
  hr = d3d_11_device_->CreateTexture2D(&d3d11_texture2D_desc, nullptr,
                                       &d3d_11_texture_2D_);
  CHECK_HRESULT("ID3D11Device::CreateTexture2D");
  hr = d3d_11_texture_2D_.As(&resource);
  CHECK_HRESULT("ID3D11Texture2D::As");
  // Retrieve the shared |HANDLE| for interop.
  hr = resource->GetSharedHandle(&handle_);
  CHECK_HRESULT("IDXGIResource::GetSharedHandle");
  d3d_11_texture_2D_->AddRef();

  return true;
}

bool ANGLESurfaceManager::CreateEGLDisplay() {
  if (display_ == EGL_NO_DISPLAY) {
    auto eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (eglGetPlatformDisplayEXT) {
      display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                          EGL_DEFAULT_DISPLAY,
                                          kD3D11DisplayAttributes);
      if (eglInitialize(display_, 0, 0) == EGL_FALSE) {
        display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                            EGL_DEFAULT_DISPLAY,
                                            kD3D11_9_3DisplayAttributes);
        if (eglInitialize(display_, 0, 0) == EGL_FALSE) {
          display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                              EGL_DEFAULT_DISPLAY,
                                              kD3D9DisplayAttributes);
          if (eglInitialize(display_, 0, 0) == EGL_FALSE) {
            display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE,
                                                EGL_DEFAULT_DISPLAY,
                                                kWrapDisplayAttributes);
            if (eglInitialize(display_, 0, 0) == EGL_FALSE) {
              FAIL("eglGetPlatformDisplayEXT");
            }
          }
        }
      }
    } else {
      FAIL("eglGetProcAddress");
    }
  }
  return true;
}

bool ANGLESurfaceManager::CreateAndBindEGLSurface() {
  // Do not create |context_| again, likely due to |Resize|.
  if (context_ == EGL_NO_CONTEXT) {
    // First time from the constructor itself.
    auto count = 0;
    auto result = eglChooseConfig(display_, kEGLConfigurationAttributes,
                                  &config_, 1, &count);
    if (result == EGL_FALSE || count == 0) {
      FAIL("eglChooseConfig");
    }
    // HDR needs a half-float config, an ES 3 context (float render targets)
    // and a no-config context, so one context serves both texture formats.
    // Any of them missing: the original ES 2 context, SDR only.
    const EGLint hdr_config_attributes[] = {
        EGL_RED_SIZE,   16, EGL_GREEN_SIZE, 16, EGL_BLUE_SIZE, 16,
        EGL_ALPHA_SIZE, 16, EGL_COLOR_COMPONENT_TYPE_EXT,
        EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT, EGL_NONE,
    };
    const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
    auto hdr_count = 0;
    if (extensions != nullptr &&
        strstr(extensions, "EGL_KHR_no_config_context") != nullptr &&
        strstr(extensions, "EGL_EXT_pixel_format_float") != nullptr &&
        eglChooseConfig(display_, hdr_config_attributes, &hdr_config_, 1,
                        &hdr_count) == EGL_TRUE &&
        hdr_count > 0) {
      const EGLint es3_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                       EGL_NONE};
      context_ = eglCreateContext(display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                                  es3_attributes);
      hdr_capable_ = context_ != EGL_NO_CONTEXT;
    }
    if (!hdr_capable_) {
      context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT,
                                  kEGLContextAttributes);
    }
    std::cout << "media_kit: ANGLESurfaceManager: HDR output "
              << (hdr_capable_ ? "available" : "unavailable") << std::endl;
    if (context_ == EGL_NO_CONTEXT) {
      FAIL("eglCreateContext");
    }
  }
  EGLint buffer_attributes[] = {
      EGL_WIDTH,          width_,         EGL_HEIGHT,         height_,
      EGL_TEXTURE_TARGET, EGL_TEXTURE_2D, EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
      EGL_NONE,
  };
  surface_ = eglCreatePbufferFromClientBuffer(
      display_, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, internal_handle_,
      hdr_ ? hdr_config_ : config_, buffer_attributes);
  if (surface_ == EGL_NO_SURFACE) {
    FAIL("eglCreatePbufferFromClientBuffer");
  }
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  eglBindTexImage(display_, surface_, EGL_BACK_BUFFER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return true;
}

void ANGLESurfaceManager::SetHdrScale(float scale) {
  ::WaitForSingleObject(mutex_, INFINITE);
  hdr_scale_ = scale;
  ::ReleaseMutex(mutex_);
}

bool ANGLESurfaceManager::SetHdr(bool hdr, float scale) {
  if (hdr && !hdr_capable_) {
    return false;
  }
  ::WaitForSingleObject(mutex_, INFINITE);
  hdr_scale_ = scale;
  if (hdr != hdr_) {
    hdr_ = hdr;
    // New shared textures in the other format, same size.
    Create();
  }
  ::ReleaseMutex(mutex_);
  return true;
}

namespace {

constexpr char kHdrVertexShader[] = R"(#version 300 es
in vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
)";

// Linear light (mpv's reference white = 1) to extended sRGB, scaled so the
// display's SDR white is 1: what Flutter's HDR compositor decodes.
constexpr char kHdrFragmentShader[] = R"(#version 300 es
precision highp float;
uniform highp sampler2D u_texture;
uniform float u_scale;
out vec4 o_color;
vec3 FromLinear(vec3 l) {
  vec3 a = abs(l);
  vec3 low = a * 12.92;
  vec3 high = 1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055;
  return sign(l) * mix(low, high, step(vec3(0.0031308), a));
}
void main() {
  vec4 c = texelFetch(u_texture, ivec2(gl_FragCoord.xy), 0);
  o_color = vec4(FromLinear(c.rgb * u_scale), 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::cout << "media_kit: HDR shader: " << log << std::endl;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

bool ANGLESurfaceManager::EnsureHdrResources() {
  if (!LoadVertexArrayFunctions()) {
    return false;
  }
  if (hdr_program_ == 0) {
    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kHdrVertexShader);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kHdrFragmentShader);
    if (vertex == 0 || fragment == 0) {
      return false;
    }
    hdr_program_ = glCreateProgram();
    glAttachShader(hdr_program_, vertex);
    glAttachShader(hdr_program_, fragment);
    glBindAttribLocation(hdr_program_, 0, "a_position");
    glLinkProgram(hdr_program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(hdr_program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      return false;
    }
    hdr_scale_location_ = glGetUniformLocation(hdr_program_, "u_scale");
    static const GLfloat kTriangle[] = {-1.0f, -1.0f, 3.0f,
                                        -1.0f, -1.0f, 3.0f};
    gen_vertex_arrays(1, &hdr_vertex_array_);
    bind_vertex_array(hdr_vertex_array_);
    glGenBuffers(1, &hdr_vertex_buffer_);
    glBindBuffer(GL_ARRAY_BUFFER, hdr_vertex_buffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kTriangle), kTriangle,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    bind_vertex_array(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  if (hdr_fbo_ == 0 || hdr_fbo_width_ != width_ ||
      hdr_fbo_height_ != height_) {
    if (hdr_fbo_ == 0) {
      glGenFramebuffers(1, &hdr_fbo_);
      glGenTextures(1, &hdr_texture_);
    }
    glBindTexture(GL_TEXTURE_2D, hdr_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA,
                 GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, hdr_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, hdr_texture_, 0);
    const bool complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
      return false;
    }
    hdr_fbo_width_ = width_;
    hdr_fbo_height_ = height_;
  }
  return true;
}

void ANGLESurfaceManager::EncodeHdr() {
  // mpv leaves its own state behind; this context is only mpv's and this
  // pass, so set what the pass needs rather than restoring.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width_, height_);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(hdr_program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, hdr_texture_);
  glUniform1f(hdr_scale_location_, hdr_scale_);
  bind_vertex_array(hdr_vertex_array_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  bind_vertex_array(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

void ANGLESurfaceManager::ReleaseHdrResources() {
  if (hdr_fbo_ != 0) {
    glDeleteFramebuffers(1, &hdr_fbo_);
    glDeleteTextures(1, &hdr_texture_);
    hdr_fbo_ = hdr_texture_ = 0;
  }
  if (hdr_program_ != 0) {
    glDeleteProgram(hdr_program_);
    delete_vertex_arrays(1, &hdr_vertex_array_);
    glDeleteBuffers(1, &hdr_vertex_buffer_);
    hdr_program_ = hdr_vertex_array_ = hdr_vertex_buffer_ = 0;
  }
}
