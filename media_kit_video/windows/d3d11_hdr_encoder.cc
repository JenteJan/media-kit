// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.
#include "d3d11_hdr_encoder.h"

#include <d3dcompiler.h>

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// One triangle over the whole target; the pixel shader reads the source
// texel under each pixel, so source and target are the same size.
constexpr char kShader[] = R"(
cbuffer Constants : register(b0) {
  float scale;
  float3 padding;
};
Texture2D<float4> source : register(t0);

float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2((id << 1) & 2, id & 2);
  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// sRGB's curve, extended to values outside [0, 1] by symmetry: what
// Flutter's scRGB compositor reads back as linear light.
float3 FromLinear(float3 l) {
  float3 a = abs(l);
  float3 low = a * 12.92;
  float3 high = 1.055 * pow(a, 1.0 / 2.4) - 0.055;
  return sign(l) * lerp(low, high, step(0.0031308, a));
}

float4 ps(float4 position : SV_Position) : SV_Target {
  float4 c = source.Load(int3(position.xy, 0));
  return float4(FromLinear(c.rgb * scale), 1.0);
}
)";

void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::ostringstream message;
    message << "HDR encoder: " << operation << " failed (HRESULT 0x"
            << std::hex << static_cast<unsigned long>(result) << ")";
    throw std::runtime_error(message.str());
  }
}

using D3DCompileFunction = decltype(&D3DCompile);

Microsoft::WRL::ComPtr<ID3DBlob> Compile(D3DCompileFunction compile,
                                         const char* entry,
                                         const char* target) {
  Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
  const auto result =
      compile(kShader, std::strlen(kShader), "media_kit_hdr", nullptr, nullptr,
              entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(result)) {
    std::string message = "HDR encoder: compiling ";
    message += entry;
    if (errors) {
      message += ": ";
      message += static_cast<const char*>(errors->GetBufferPointer());
    }
    throw std::runtime_error(message);
  }
  return code;
}

}  // namespace

D3D11HdrEncoder::D3D11HdrEncoder(ID3D11Device* device) : device_(device) {
  device_->GetImmediateContext(&context_);
  // Part of Windows since 8.1; loaded here rather than linked, so that the
  // plugin loads without it and only HDR is unavailable.
  static HMODULE compiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
  if (compiler == nullptr) {
    throw std::runtime_error("HDR encoder: d3dcompiler_47.dll unavailable");
  }
  auto compile = reinterpret_cast<D3DCompileFunction>(
      ::GetProcAddress(compiler, "D3DCompile"));
  if (compile == nullptr) {
    throw std::runtime_error("HDR encoder: D3DCompile unavailable");
  }
  auto vs = Compile(compile, "vs", "vs_4_0");
  auto ps = Compile(compile, "ps", "ps_4_0");
  Check(device_->CreateVertexShader(vs->GetBufferPointer(),
                                    vs->GetBufferSize(), nullptr,
                                    &vertex_shader_),
        "CreateVertexShader");
  Check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(),
                                   nullptr, &pixel_shader_),
        "CreatePixelShader");
  D3D11_BUFFER_DESC desc{};
  desc.ByteWidth = 16;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  Check(device_->CreateBuffer(&desc, nullptr, &constants_), "CreateBuffer");
}

void D3D11HdrEncoder::Encode(ID3D11Texture2D* source,
                             ID3D11Texture2D* target,
                             int width,
                             int height,
                             float scale) {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output;
  Check(device_->CreateShaderResourceView(source, nullptr, &input),
        "CreateShaderResourceView");
  Check(device_->CreateRenderTargetView(target, nullptr, &output),
        "CreateRenderTargetView");
  if (scale != constants_scale_) {
    const float constants[4] = {scale, 0.0f, 0.0f, 0.0f};
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, constants, 0, 0);
    constants_scale_ = scale;
  }
  // mpv leaves its own state behind on this context: set all this pass uses.
  context_->ClearState();
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
  context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
  context_->PSSetShaderResources(0, 1, input.GetAddressOf());
  context_->OMSetRenderTargets(1, output.GetAddressOf(), nullptr);
  D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width),
                          static_cast<float>(height), 0.0f, 1.0f};
  context_->RSSetViewports(1, &viewport);
  context_->Draw(3, 0);
  // Nothing bound for mpv's next frame to trip over.
  context_->ClearState();
}
