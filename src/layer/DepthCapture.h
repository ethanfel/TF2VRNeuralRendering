#pragma once
#include "Graphics.h"
#include <cstdint>
#include <vector>
namespace tf2vrnr {
struct DepthBeforeClear {
    std::shared_ptr<CapturedImage> snapshot;
    std::string bindings;
    std::string name, stage;
    uintptr_t resourceId = 0;
    uint64_t frame = 0;
    unsigned ordinal = 0, flags = 0, stencil = 0;
    float clearValue = 0;
    int64_t sourceFormat = 0, fileFormat = 0;
    int slot = -1;
    uint32_t width = 0, height = 0;
};
struct DepthBatch {
    std::vector<DepthBeforeClear> copies;
    unsigned observed = 0, skipped = 0;
    std::vector<std::string> passEvents;
    unsigned passEventsDropped = 0, passGroups = 0;
    unsigned enginePostObserved = 0, enginePostSkipped = 0;
};
// Arms only a requested frame, for the selected eye resolution. Ordinary
// frames forward clears immediately without inspecting/copying resources.
void BeginDepthCapture(ID3D11Device*, uint32_t width, uint32_t height, uint64_t frame,
    bool renderPasses = false, bool enginePost = false);
void EndDepthCapture(ID3D11Device*);
DepthBatch TakeDepthCapture(ID3D11Device*);
#ifdef TF2VRNR_BACKEND
struct Config;
void ConfigureNeuralFrame(ID3D11Device*, bool capture, const Config&,
    const uint64_t (&swapchains)[2], bool reset);
bool ReleaseNeuralView(ID3D11Device*, uint64_t swapchain);
bool NeuralFrameApplied(ID3D11Device*);
bool DlaaFrameApplied(ID3D11Device*);
bool SetNeuralJitter(ID3D11Device*, float x, float y);
void DestroyNeuralBackend(ID3D11Device*);
#endif
#ifdef TF2VRNR_FRAMEGEN
class FrameGeneration;
void SetFrameCameraProjection(ID3D11Device*,const float (&tangents)[2][4]);
bool GenerateReleasedView(ID3D11Device*,uint64_t swapchain,ID3D11Texture2D*);
std::shared_ptr<FrameGeneration> GeneratedStereoFrame(ID3D11Device*);
#endif
}
