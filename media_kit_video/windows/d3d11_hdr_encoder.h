// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

// HDR output for HDR Flutter engines: mpv renders linear light (BT.709
// primaries, 1.0 = paper white at scale 1) into a half-float texture, and this
// pass writes it into the shared half-float texture as extended sRGB times a
// scale, which Flutter's HDR (scRGB) compositor shows as is.
class D3D11HdrEncoder {
 public:
  // Compiles its shaders with the system's d3dcompiler_47.dll. Throws when
  // that or anything else is missing.
  explicit D3D11HdrEncoder(ID3D11Device* device);

  // |source| and |target| are the same size; |source| is bindable as a shader
  // resource, |target| as a render target.
  void Encode(ID3D11Texture2D* source,
              ID3D11Texture2D* target,
              int width,
              int height,
              float scale);

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  float constants_scale_ = -1.0f;
};
