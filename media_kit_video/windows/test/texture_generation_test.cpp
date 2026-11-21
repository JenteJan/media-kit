#include "../d3d11_surface_manager.h"
#include "../thread_pool.h"

#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Clear(D3D11SurfaceManager& manager,
           const D3D11SurfaceManager::Surface& frame,
           bool red) {
  ComPtr<ID3D11RenderTargetView> view;
  Check(SUCCEEDED(manager.device()->CreateRenderTargetView(frame.texture.Get(),
                                                           nullptr, &view)),
        "create RTV");
  ComPtr<ID3D11DeviceContext> context;
  manager.device()->GetImmediateContext(&context);
  FLOAT color[]{red ? 1.f : 0.f, red ? 0.f : 1.f, 0, 1};
  context->ClearRenderTargetView(view.Get(), color);
  manager.WaitForWrites();
}

void CheckColor(ID3D11Device* consumer, HANDLE handle, UINT width, bool red) {
  ComPtr<ID3D11Texture2D> imported;
  Check(
      SUCCEEDED(consumer->OpenSharedResource(handle, IID_PPV_ARGS(&imported))),
      "consumer import");
  D3D11_TEXTURE2D_DESC desc{};
  imported->GetDesc(&desc);
  Check(desc.Width == width, "wrong generation");
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.BindFlags = desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  Check(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &staging)),
        "staging");
  ComPtr<ID3D11DeviceContext> context;
  consumer->GetImmediateContext(&context);
  context->CopyResource(staging.Get(), imported.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
        "readback");
  const auto* pixel = static_cast<const uint8_t*>(mapped.pData);
  const bool matches =
      pixel[2] == (red ? 255 : 0) && pixel[1] == (red ? 0 : 255);
  context->Unmap(staging.Get(), 0);
  Check(matches, "retained generation pixels changed");
}

int main() {
  try {
    ComPtr<IDXGIFactory> factory;
    ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->EnumAdapters(0, &adapter)),
          "test adapter");
    D3D11SurfaceManager consumer(adapter.Get());
    std::shared_ptr<D3D11SurfaceManager::Surface> old, current;
    {
      D3D11SurfaceManager producer(adapter.Get());
      old = producer.CreateSurface(64, 36);
      Clear(producer, *old, true);
      auto reader = std::async(std::launch::async, [&] {
        for (int i = 0; i < 100; ++i)
          CheckColor(consumer.device(), old->handle, 64, true);
      });
      for (int i = 0; i < 100; ++i) {
        current = producer.CreateSurface(80, 60);
        Clear(producer, *current, false);
      }
      reader.get();
    }
    CheckColor(consumer.device(), old->handle, 64, true);
    CheckColor(consumer.device(), current->handle, 80, false);

    std::vector<int> order;
    {
      ThreadPool operations(1);
      for (int i = 0; i < 100; ++i) {
        operations.Post([&order, i] { order.push_back(i); });
      }
    }
    Check(order.size() == 100, "shutdown must drain accepted operations");
    for (int i = 0; i < 100; ++i)
      Check(order[i] == i, "FIFO operation order");
    std::cout << "PASS: retained GPU generations, concurrent resize/read, "
                 "teardown, FIFO shutdown\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
}
