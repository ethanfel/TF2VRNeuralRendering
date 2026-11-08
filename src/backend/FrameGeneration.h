#pragma once
#include "FrameCamera.h"
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string>
namespace tf2vrnr {
class FrameGeneration {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  FrameGeneration();
  ~FrameGeneration();
  bool Init(ID3D11Device *, unsigned width, unsigned height,
            const std::wstring &runtime);
  bool Prepare(ID3D11Texture2D *depth, ID3D11Texture2D *motion, unsigned eye,
               uint64_t frame, float jitterX, float jitterY);
  // Before xrReleaseSwapchainImage, while the application still owns color.
  bool Generate(ID3D11Texture2D *color, unsigned eye, uint64_t frame,
                const FrameCamera &, bool reset);
  ID3D11Texture2D *Real(unsigned eye) const;
  ID3D11Texture2D *Generated(unsigned eye) const;
  bool Interpolated(unsigned eye) const;
  bool Shutdown();
};
} // namespace tf2vrnr
