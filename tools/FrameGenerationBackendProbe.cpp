// Tests the production guide conversion and per-eye backend without a game.
#include "../src/backend/FrameGeneration.h"
#include "../src/layer/Support.h"
#include <chrono>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>
using namespace tf2vrnr;
using Microsoft::WRL::ComPtr;
static void Check(HRESULT r) {
  if (FAILED(r))
    throw std::runtime_error("d3d11");
}
static uint16_t Half(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return uint16_t(((bits >> 16) & 0x8000) |
                  ((((bits >> 23) & 255) - 112) << 10) | ((bits >> 13) & 1023));
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 4)
    return 2;
  try {
    unsigned w = std::stoul(argv[2]), h = std::stoul(argv[3]);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                            &context));
    FrameGeneration fg;
    if (!fg.Init(device.Get(), w, h, argv[1]))
      return 3;
    auto texture = [&](DXGI_FORMAT format) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.Format = format;
      d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ComPtr<ID3D11Texture2D> t;
      Check(device->CreateTexture2D(&d, nullptr, &t));
      return t;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM),
         depth = texture(DXGI_FORMAT_R32_FLOAT),
         motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    float cameraBytes[144]{};
    cameraBytes[0] = 1;
    cameraBytes[4] = 1;
    cameraBytes[9] = 1;
    cameraBytes[14] = 1;
    cameraBytes[15] = -1;
    cameraBytes[18] = 1;
    cameraBytes[114] = float(w);
    cameraBytes[115] = float(h);
    cameraBytes[135] = cameraBytes[136] = cameraBytes[137] = 1;
    auto camera = DecodeFrameCamera(cameraBytes, w, h);
    if (!camera.valid)
      throw std::runtime_error("camera");
    auto d = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D11_TEXTURE2D_DESC desc{};
    d->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging));
    bool passed = true;
    for (unsigned frame = 0; frame < 8; ++frame)
      for (unsigned eye = 0; eye < 2; ++eye) {
        std::vector<unsigned char> pixels(size_t(w) * h * 4);
        std::vector<float> depths(size_t(w) * h, .999f);
        std::vector<uint16_t> motions(size_t(w) * h * 2);
        const unsigned left = w / 4 + frame * 4 + eye * 12;
        for (unsigned y = 0; y < h; ++y)
          for (unsigned x = 0; x < w; ++x) {
            size_t k = size_t(y) * w + x;
            bool box =
                x >= left && x < left + w / 4 && y >= h / 3 && y < h * 2 / 3;
            auto v = static_cast<unsigned char>(
                box ? 220 : 20 + ((x / 16 + y / 16) % 2) * 25);
            pixels[k * 4] = v;
            pixels[k * 4 + 1] = box ? 60 : v;
            pixels[k * 4 + 2] = box ? 40 : v;
            pixels[k * 4 + 3] = 255;
            if (box) {
              depths[k] = .98f;
              motions[k * 2] = Half(4.f * 1024 / w);
            }
          }
        context->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(),
                                   w * 4, 0);
        context->UpdateSubresource(depth.Get(), 0, nullptr, depths.data(),
                                   w * 4, 0);
        context->UpdateSubresource(motion.Get(), 0, nullptr, motions.data(),
                                   w * 4, 0);
        auto start = std::chrono::steady_clock::now();
        if (!fg.Prepare(depth.Get(), motion.Get(), eye, frame, 0, 0) ||
            !fg.Generate(color.Get(), eye, frame, camera, frame == 0))
          throw std::runtime_error("generate");
        context->CopyResource(staging.Get(), fg.Generated(eye));
        D3D11_MAPPED_SUBRESOURCE map{};
        Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
        size_t bright = 0;
        double centroid = 0;
        for (unsigned y = 0; y < h; ++y) {
          auto *row =
              static_cast<unsigned char *>(map.pData) + y * map.RowPitch;
          for (unsigned x = 0; x < w; ++x)
            if (row[x * 4] > 150 && row[x * 4 + 1] < 100) {
              ++bright;
              centroid += x;
            }
        }
        context->Unmap(staging.Get(), 0);
        centroid /= std::max(size_t(1), bright);
        double expected = left + (w / 4 - 1) / 2.0 - (frame ? 2 : 0);
        double elapsed = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        Log("fg_backend_output frame=%u eye=%u interpolated=%d centroid=%.3f "
            "expected=%.3f gpu_and_readback_ms=%.3f",
            frame, eye, fg.Interpolated(eye), centroid, expected, elapsed);
        if (frame > 1)
          passed = passed && bright > 100 &&
                   std::abs(centroid - expected) < 1.5 && fg.Interpolated(eye);
      }
    passed = fg.Shutdown() && passed;
    Log("fg_backend_probe_passed=%d", passed);
    return passed ? 0 : 4;
  } catch (const std::exception &e) {
    Log("fg_backend_probe_failed %s", e.what());
    return 1;
  }
}
