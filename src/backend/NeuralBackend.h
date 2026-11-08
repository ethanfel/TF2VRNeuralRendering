#pragma once
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string>
namespace tf2vrnr {
struct NeuralOptions {
  unsigned resolutionPercent = 50;
  float intensity = 0.5f;
  float tone = 1.f;
  float detail = 1.f;
  unsigned preset = 2; // NVIDIA NR preset B; A..G are 1..7.
  bool neural = true;
  bool dlaa = false;
  bool passthrough =
      false; // Exercises the complete GPU conversion route without NGX.
};
class NeuralBackend {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  NeuralBackend();
  ~NeuralBackend();
  bool Init(ID3D11Device *, unsigned width, unsigned height,
            const std::wstring &runtimeDirectory, const NeuralOptions & = {});
  // GPU-only. Replaces scene HDR before the original engine-post draw.
  // One feature/history per eye; the owner supplies eye identity and reset.
  bool Render(ID3D11Texture2D *hdr, ID3D11Texture2D *depth,
              ID3D11Texture2D *motion, ID3D11Texture2D *exposure, unsigned eye,
              uint64_t frame, bool reset, float jitterX = 0, float jitterY = 0);
  bool Shutdown();
};
} // namespace tf2vrnr
