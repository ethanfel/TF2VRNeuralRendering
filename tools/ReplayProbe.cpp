// Real captured scene replay through the production GPU backend, outside the
// game.
#include "../src/backend/NeuralBackend.h"
#include "../src/layer/Support.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr) {
  if (FAILED(hr))
    throw std::runtime_error("D3D11 failure");
}
struct Image {
  std::array<char, 148> header;
  std::vector<char> data;
  ComPtr<ID3D11Texture2D> texture;
  unsigned width = 0, height = 0, bpp = 0;
  explicit Image(ID3D11Device *device, const std::string &name) {
    std::ifstream f(std::filesystem::path(name), std::ios::binary);
    f.read(header.data(), header.size());
    if (!f || std::memcmp(header.data(), "DDS ", 4))
      throw std::runtime_error("Invalid DDS");
    std::memcpy(&height, header.data() + 12, 4);
    std::memcpy(&width, header.data() + 16, 4);
    unsigned format;
    std::memcpy(&format, header.data() + 128, 4);
    bpp = format == 10 ? 8 : format == 54 ? 2 : 4;
    data.resize(size_t(width) * height * bpp);
    f.read(data.data(), data.size());
    if (!f)
      throw std::runtime_error("Incomplete DDS");
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format == 45 ? DXGI_FORMAT_R24G8_TYPELESS
                            : static_cast<DXGI_FORMAT>(format);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA pixels{data.data(), width * bpp, 0};
    Check(device->CreateTexture2D(&d, &pixels, &texture));
  }
  bool Save(ID3D11Device *device, const std::filesystem::path &path,
            bool identity) {
    ComPtr<ID3D11DeviceContext> c;
    device->GetImmediateContext(&c);
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&d, nullptr, &staging));
    c->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<char> output(data.size());
    for (unsigned y = 0; y < height; ++y)
      std::memcpy(output.data() + size_t(y) * width * bpp,
                  static_cast<char *>(map.pData) + size_t(y) * map.RowPitch,
                  width * bpp);
    c->Unmap(staging.Get(), 0);
    std::ofstream f(path, std::ios::binary);
    f.write(header.data(), header.size());
    f.write(output.data(), output.size());
    size_t different = 0, nonfinite = 0, outsideHalfRounding = 0, alphaChanged = 0;
    for (size_t i = 0; i < data.size(); i += 2) {
      uint16_t a, b;
      std::memcpy(&a, data.data() + i, 2);
      std::memcpy(&b, output.data() + i, 2);
      different += a != b;
      nonfinite += (b & 0x7c00) == 0x7c00;
      // Equal FP16 model input/output passes through two independently
      // compiled transfer curves; allow at most two half-precision ULPs.
      outsideHalfRounding += (a > b ? a-b : b-a) > 2;
      if ((i/2)%4 == 3) alphaChanged += a != b;
    }
    tf2vrnr::Log(
        "output=%s changed_components=%zu nonfinite=%zu identity_expected=%u outside_half_rounding=%zu alpha_changed=%zu",
        path.filename().string().c_str(), different, nonfinite, identity, outsideHalfRounding, alphaChanged);
    return f.good() && nonfinite == 0 &&
           alphaChanged == 0 && (identity ? outsideHalfRounding == 0 : different > data.size() / 200);
  }
};
} // namespace
int wmain(int argc, wchar_t **argv) {
  if (argc < 4)
    return 2;
  tf2vrnr::NeuralOptions opt;
  for (int i=4;i<argc;++i) {
    if (!wcscmp(argv[i],L"--identity")) opt.passthrough=true;
    else if (!wcscmp(argv[i],L"--dlaa")) opt.dlaa=true;
    else if (!wcscmp(argv[i],L"--no-neural")) opt.neural=false;
    else if (i+1<argc && !wcscmp(argv[i],L"--tone")) opt.tone=std::stof(argv[++i])/100.f;
    else if (i+1<argc && !wcscmp(argv[i],L"--detail")) opt.detail=std::stof(argv[++i])/100.f;
    else if (i+1<argc && !wcscmp(argv[i],L"--strength")) opt.intensity=std::stof(argv[++i])/100.f;
    else if (i+1<argc && !wcscmp(argv[i],L"--quality")) opt.resolutionPercent=std::stoul(argv[++i]);
    else if (i+1<argc && !wcscmp(argv[i],L"--model")) opt.preset=argv[++i][0]-L'A'+1;
    else return 2;
  }
  const bool identity=opt.passthrough;
  try {
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL level;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                            nullptr));
    std::ifstream manifest{std::filesystem::path(argv[1])};
    std::filesystem::path directory = argv[3];
    tf2vrnr::NeuralBackend backend;
    bool initialized = false, passed = true;
    std::ifstream jitterFile(directory / "jitter.txt");
    for (unsigned frame = 0; manifest.peek()!=std::char_traits<char>::eof(); ++frame)
      for (unsigned eye = 0; eye < 2; ++eye) {
        std::array<std::string, 4> names;
        for (auto &name : names) {
          std::getline(manifest, name);
          if (!name.empty() && name.back() == '\r')
            name.pop_back();
        }
        Image color(device.Get(), names[0]), depth(device.Get(), names[1]),
            motion(device.Get(), names[2]), exposure(device.Get(), names[3]);
        if (!initialized) {
          if (!backend.Init(device.Get(), color.width, color.height, argv[2],
                            opt))
            return 3;
          initialized = true;
        }
        float jitterX=0,jitterY=0;
        if (jitterFile) jitterFile>>jitterX>>jitterY;
        if (!backend.Render(color.texture.Get(), depth.texture.Get(),
                            motion.texture.Get(), exposure.texture.Get(), eye,
                            frame, frame == 0,jitterX,jitterY))
          return 4;
        passed = color.Save(device.Get(),
                            directory / ("frame" + std::to_string(frame) +
                                         "-eye" + std::to_string(eye) + ".dds"),
                            identity) &&
                 passed;
      }
    passed = backend.Shutdown() && passed;
    tf2vrnr::Log("replay_passed=%u mode=%s", passed,
                identity ? "identity" : "neural");
    return passed ? 0 : 5;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "replay_error=%s\n", e.what());
    return 1;
  }
}
