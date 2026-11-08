#include "FrameGeneration.h"
#include "../layer/Support.h"
#include "GpuInterop.h"
#include "NgxLifetime.h"
#include <d3dcompiler.h>
#include <limits>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <stdexcept>

extern "C" unsigned tf2vrnr_parameter_get_int(void *, const char *, int *);
extern "C" void tf2vrnr_parameter_set_uint(void *, const char *, unsigned);
extern "C" void tf2vrnr_parameter_set_float(void *, const char *, float);
extern "C" void tf2vrnr_parameter_set_resource(void *, const char *,
                                               ID3D12Resource *);
extern "C" void tf2vrnr_parameter_set_pointer(void *, const char *, void *);
namespace tf2vrnr {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT r, const char *name) {
  if (FAILED(r)) {
    Log("fg_failure %s hr=0x%08lx", name, (unsigned long)r);
    throw std::runtime_error(name);
  }
}
void Ngx(NVSDK_NGX_Result r, const char *name) {
  if (r != NVSDK_NGX_Result_Success) {
    Log("fg_failure %s ngx=0x%08x", name, (unsigned)r);
    throw std::runtime_error(name);
  }
}
template <class T> T Proc(HMODULE m, const char *name) {
  auto p = reinterpret_cast<T>(GetProcAddress(m, name));
  if (!p)
    throw std::runtime_error(name);
  return p;
}
constexpr const char *Shader = R"(
cbuffer Dimensions:register(b0){uint2 size;float2 offset;float2 correction;float2 unused;};
Texture2D<float> inputDepth:register(t0);
Texture2D<float2> inputMotion:register(t1);
RWTexture2D<float> depth:register(u0);
RWTexture2D<float2> motion:register(u1);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
 if(any(p.xy>=size))return;
 uint2 q=uint2(clamp(float2(p.xy)+0.5+offset,0,float2(size)-1));
 depth[p.xy]=inputDepth.Load(int3(q,0));
 motion[p.xy]=inputMotion.Load(int3(q,0))*(-float2(size)/1024.0)+correction;
}
)";
struct ContextState {
  ID3D11DeviceContext *c;
  ID3D11ComputeShader *shader = nullptr;
  ID3D11ClassInstance *classes[256]{};
  UINT count = 256;
  ID3D11ShaderResourceView *srvs[2]{};
  ID3D11UnorderedAccessView *uavs[2]{};
  ID3D11Buffer *cb = nullptr;
  ID3D11RenderTargetView *rt[8]{};
  ID3D11DepthStencilView *ds = nullptr;
  explicit ContextState(ID3D11DeviceContext *context) : c(context) {
    c->CSGetShader(&shader, classes, &count);
    c->CSGetShaderResources(0, 2, srvs);
    c->CSGetUnorderedAccessViews(0, 2, uavs);
    c->CSGetConstantBuffers(0, 1, &cb);
    c->OMGetRenderTargets(8, rt, &ds);
    c->OMSetRenderTargets(0, nullptr, nullptr);
  }
  ~ContextState() {
    ID3D11ShaderResourceView *empty[2]{};
    ID3D11UnorderedAccessView *none[2]{};
    c->CSSetShaderResources(0, 2, empty);
    c->CSSetUnorderedAccessViews(0, 2, none, nullptr);
    c->CSSetShader(shader, classes, count);
    c->CSSetConstantBuffers(0, 1, &cb);
    c->OMSetRenderTargets(8, rt, ds);
    c->CSSetShaderResources(0, 2, srvs);
    UINT counts[2] = {~0u, ~0u};
    c->CSSetUnorderedAccessViews(0, 2, uavs, counts);
    if (shader)
      shader->Release();
    if (cb)
      cb->Release();
    if (ds)
      ds->Release();
    for (auto *p : srvs)
      if (p)
        p->Release();
    for (auto *p : uavs)
      if (p)
        p->Release();
    for (auto *p : rt)
      if (p)
        p->Release();
    for (UINT k = 0; k < count; ++k)
      if (classes[k])
        classes[k]->Release();
  }
};
} // namespace
struct FrameGeneration::Impl {
  GpuInterop gpu;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11ComputeShader> shader;
  ComPtr<ID3D11Buffer> dimensions;
  unsigned width = 0, height = 0;
  HMODULE core = nullptr;
  bool initialized = false, failed = false;
  using InitFn = NVSDK_NGX_Result(NVSDK_CONV *)(
      const char *, NVSDK_NGX_EngineType, const char *, const wchar_t *,
      ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *);
  using Allocate = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
  using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *,
                                                NVSDK_NGX_Feature,
                                                NVSDK_NGX_Parameter *,
                                                NVSDK_NGX_Handle **);
  using Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(
      ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
      const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
  using Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
  using Destroy = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
  using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *, unsigned *);
  Evaluate evaluate = nullptr;
  Release release = nullptr;
  Destroy destroy = nullptr;
  ShutdownFn shutdown = nullptr;
  NVSDK_NGX_Parameter *caps = nullptr;
  struct Eye {
    SharedTexture color, depth, motion, output;
    ComPtr<ID3D11UnorderedAccessView> depthUav, motionUav;
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    uint64_t prepared = ~uint64_t(0), previousFrame = ~uint64_t(0);
    FrameCamera previousCamera;
    float previousJitter[2]{};
    bool interpolated = false;
  };
  std::array<Eye, 2> eyes;
  bool Close() {
    if(!gpu.Device12()&&!core)return true;
    if (!gpu.Drain())
      return false;
    if (failed)
      return false;
    for (auto &e : eyes) {
      if (e.feature) {
        Ngx(release(e.feature), "release");
        e.feature = nullptr;
      }
      if (e.params) {
        Ngx(destroy(e.params), "parameters");
        e.params = nullptr;
      }
    }
    if (caps) {
      Ngx(destroy(caps), "caps");
      caps = nullptr;
    }
    if (initialized) {
      Ngx(static_cast<NVSDK_NGX_Result>(LeaveNgxRuntime(core)), "shared_shutdown");
      initialized = false;
    }
    if (core) {
      FreeLibrary(core);
      core = nullptr;
    }
    return true;
  }
};
FrameGeneration::FrameGeneration() : impl_(new Impl) {}
FrameGeneration::~FrameGeneration() {
  if (!Shutdown()) {
    Log("fg_backend_quarantined until_process_exit=1");
    impl_.release();
  }
}
bool FrameGeneration::Init(ID3D11Device *device, unsigned width,
                           unsigned height, const std::wstring &runtime) {
  auto &i = *impl_;
  try {
    i.device = device;
    i.width = width;
    i.height = height;
    if (!i.gpu.Init(device))
      throw std::runtime_error("interop");
    i.core = LoadLibraryW(L"_nvngx.dll");
    if (!i.core)
      throw std::runtime_error("core");
    auto init = Proc<Impl::InitFn>(i.core, "NVSDK_NGX_D3D12_Init_ProjectID");
    auto allocate =
        Proc<Impl::Allocate>(i.core, "NVSDK_NGX_D3D12_AllocateParameters");
    auto capabilities =
        Proc<Impl::Allocate>(i.core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto create = Proc<Impl::Create>(i.core, "NVSDK_NGX_D3D12_CreateFeature");
    i.evaluate =
        Proc<Impl::Evaluate>(i.core, "NVSDK_NGX_D3D12_EvaluateFeature");
    i.release = Proc<Impl::Release>(i.core, "NVSDK_NGX_D3D12_ReleaseFeature");
    i.destroy =
        Proc<Impl::Destroy>(i.core, "NVSDK_NGX_D3D12_DestroyParameters");
    i.shutdown = Proc<Impl::ShutdownFn>(i.core, "NVSDK_NGX_D3D12_Shutdown1");
    const wchar_t *path = runtime.c_str();
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = &path;
    common.PathListInfo.Length = 1;
    Ngx(init("0ca94998-cffe-473b-bf26-fbd774613980",
             NVSDK_NGX_ENGINE_TYPE_CUSTOM, "TF2VR-FG", Directory().c_str(),
             i.gpu.Device12(), NVSDK_NGX_Version_API, &common),
        "init");
    i.initialized = true;
    JoinNgxRuntime(i.gpu.Device12());
    Ngx(capabilities(&i.caps), "capabilities");
    int available = 0;
    Ngx(static_cast<NVSDK_NGX_Result>(tf2vrnr_parameter_get_int(
            i.caps, "FrameGeneration.Available", &available)),
        "available");
    if (!available)
      throw std::runtime_error("unavailable");
    ComPtr<ID3DBlob> code, error;
    Check(D3DCompile(Shader, std::strlen(Shader), nullptr, nullptr, nullptr,
                     "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
                     &error),
          "shader_compile");
    Check(device->CreateComputeShader(code->GetBufferPointer(),
                                      code->GetBufferSize(), nullptr,
                                      &i.shader),
          "shader");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Check(device->CreateBuffer(&desc, nullptr, &i.dimensions), "dimensions");
    for (unsigned eye = 0; eye < 2; ++eye) {
      auto &e = i.eyes[eye];
      if (!i.gpu.CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                               e.color) ||
          !i.gpu.CreateTexture(width, height, DXGI_FORMAT_R32_FLOAT, e.depth) ||
          !i.gpu.CreateTexture(width, height, DXGI_FORMAT_R32G32_FLOAT,
                               e.motion) ||
          !i.gpu.CreateTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                               e.output))
        throw std::runtime_error("textures");
      Check(device->CreateUnorderedAccessView(e.depth.d11.Get(), nullptr,
                                              &e.depthUav),
            "depth_uav");
      Check(device->CreateUnorderedAccessView(e.motion.d11.Get(), nullptr,
                                              &e.motionUav),
            "motion_uav");
      Ngx(allocate(&e.params), "allocate");
      auto *p = e.params;
      for (auto pair : {std::pair<const char *, unsigned>{"Width", width},
                        {"Height", height},
                        {"CreationNodeMask", 1},
                        {"VisibilityNodeMask", 1},
                        {"DLSSG.BackbufferFormat", DXGI_FORMAT_R8G8B8A8_UNORM},
                        {"DLSSG.InternalWidth", width},
                        {"DLSSG.InternalHeight", height},
                        {"DLSSG.DynamicResolution", 0}})
        tf2vrnr_parameter_set_uint(p, pair.first, pair.second);
      auto *list = i.gpu.Begin();
      if (!list)
        throw std::runtime_error("begin_create");
      Ngx(create(list, NVSDK_NGX_Feature_FrameGeneration, p, &e.feature),
          "create");
      if (!i.gpu.Submit() || !i.gpu.Drain())
        throw std::runtime_error("submit_create");
      tf2vrnr_parameter_set_resource(p, "DLSSG.Backbuffer", e.color.d12.Get());
      // HUDLess is optional. The final eye contains UI; do not mislabel it.
      tf2vrnr_parameter_set_resource(p, "DLSSG.Depth", e.depth.d12.Get());
      tf2vrnr_parameter_set_resource(p, "DLSSG.MVecs", e.motion.d12.Get());
      tf2vrnr_parameter_set_resource(p, "DLSSG.OutputInterpolated",
                                     e.output.d12.Get());
      for (const char *name :
           {"MVecs", "Depth", "Backbuffer", "OutputInterpolated"}) {
        const std::string prefix = std::string("DLSSG.") + name + "Subrect";
        tf2vrnr_parameter_set_uint(p, (prefix + "BaseX").c_str(), 0);
        tf2vrnr_parameter_set_uint(p, (prefix + "BaseY").c_str(), 0);
        tf2vrnr_parameter_set_uint(p, (prefix + "Width").c_str(), width);
        tf2vrnr_parameter_set_uint(p, (prefix + "Height").c_str(), height);
      }
      Log("fg_feature_created eye=%u size=%ux%u", eye, width, height);
    }
    return true;
  } catch (const std::exception &e) {
    Log("fg_init_failed %s", e.what());
    i.failed = true;
    return false;
  }
}
bool FrameGeneration::Prepare(ID3D11Texture2D *depth, ID3D11Texture2D *motion,
                              unsigned eye, uint64_t frame, float jitterX,
                              float jitterY) {
  auto &i = *impl_;
  if (i.failed || !i.initialized || eye >= 2 || !depth || !motion)
    return false;
  try {
    auto &e = i.eyes[eye];
    D3D11_TEXTURE2D_DESC d{}, m{};
    depth->GetDesc(&d);
    motion->GetDesc(&m);
    if (d.Width != i.width || d.Height != i.height || m.Width != i.width ||
        m.Height != i.height || d.SampleDesc.Count != 1 ||
        m.SampleDesc.Count != 1 || d.ArraySize != 1 || m.ArraySize != 1)
      return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (d.Format == DXGI_FORMAT_R24G8_TYPELESS)
      sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    else if (d.Format == DXGI_FORMAT_R32_TYPELESS ||
             d.Format == DXGI_FORMAT_R32_FLOAT)
      sd.Format = DXGI_FORMAT_R32_FLOAT;
    else
      return false;
    ComPtr<ID3D11ShaderResourceView> ds, ms;
    Check(i.device->CreateShaderResourceView(depth, &sd, &ds), "depth_srv");
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    Check(i.device->CreateShaderResourceView(motion, &sd, &ms), "motion_srv");
    auto *c = i.gpu.Context11();
    ContextState saved(c);
    struct Constants {
      unsigned w, h;
      float x, y, dx, dy, pad[2];
    } values{i.width,
             i.height,
             jitterX,
             jitterY,
             jitterX - e.previousJitter[0],
             jitterY - e.previousJitter[1],
             {0, 0}};
    c->UpdateSubresource(i.dimensions.Get(), 0, nullptr, &values, 0, 0);
    c->CSSetShader(i.shader.Get(), nullptr, 0);
    ID3D11Buffer *cb = i.dimensions.Get();
    c->CSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView *inputs[] = {ds.Get(), ms.Get()};
    ID3D11UnorderedAccessView *outputs[] = {e.depthUav.Get(),
                                            e.motionUav.Get()};
    c->CSSetShaderResources(0, 2, inputs);
    c->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
    c->Dispatch((i.width + 7) / 8, (i.height + 7) / 8, 1);
    e.prepared = frame;
    e.previousJitter[0] = jitterX;
    e.previousJitter[1] = jitterY;
    return true;
  } catch (const std::exception &e) {
    Log("fg_prepare_failed %s", e.what());
    i.failed = true;
    return false;
  }
}
bool FrameGeneration::Generate(ID3D11Texture2D *color, unsigned eye,
                               uint64_t frame, const FrameCamera &camera,
                               bool reset) {
  auto &i = *impl_;
  if (i.failed || !i.initialized || eye >= 2 || !color || !camera.valid)
    return false;
  auto &e = i.eyes[eye];
  e.interpolated = false;
  if (e.prepared != frame)
    return false;
  try {
    D3D11_TEXTURE2D_DESC d{};
    color->GetDesc(&d);
    if (d.Width != i.width || d.Height != i.height || d.ArraySize != 1 ||
        d.SampleDesc.Count != 1 || d.MipLevels != 1 ||
        (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS))
      return false;
    i.gpu.Context11()->CopyResource(e.color.d11.Get(), color);
    reset = reset || e.previousFrame + 1 != frame ||
            CameraCut(e.previousCamera, camera);
    Matrix4 inverse, prevInverse;
    if (!Inverse4(camera.worldToClip, inverse))
      return false;
    auto projection = Transpose4(camera.viewToClip);
    Matrix4 invProjection;
    if (!Inverse4(projection, invProjection))
      return false;
    auto clipToPrevious = Identity4(), previousToClip = Identity4(),
         identity = Identity4();
    if (!reset) {
      clipToPrevious =
          Transpose4(Multiply4(e.previousCamera.worldToClip, inverse));
      if (!Inverse4(e.previousCamera.worldToClip, prevInverse))
        return false;
      previousToClip = Transpose4(Multiply4(camera.worldToClip, prevInverse));
    }
    auto *p = e.params;
    tf2vrnr_parameter_set_pointer(p, "DLSSG.CameraViewToClip",
                                  projection.data());
    tf2vrnr_parameter_set_pointer(p, "DLSSG.ClipToCameraView",
                                  invProjection.data());
    tf2vrnr_parameter_set_pointer(p, "DLSSG.ClipToPrevClip",
                                  clipToPrevious.data());
    tf2vrnr_parameter_set_pointer(p, "DLSSG.PrevClipToClip",
                                  previousToClip.data());
    tf2vrnr_parameter_set_pointer(p, "DLSSG.ClipToLensClip", identity.data());
    for (auto pair : {std::pair<const char *, float>{"DLSSG.MvecScaleX", 1},
                      {"DLSSG.MvecScaleY", 1},
                      {"DLSSG.CameraNear", camera.nearPlane},
                      {"DLSSG.CameraFar", camera.farPlane},
                      {"DLSSG.CameraFOV", camera.fov},
                      {"DLSSG.CameraAspectRatio", camera.aspect},
                      {"DLSSG.CameraPosX", camera.position[0]},
                      {"DLSSG.CameraPosY", camera.position[1]},
                      {"DLSSG.CameraPosZ", camera.position[2]},
                      {"DLSSG.CameraRightX", camera.right[0]},
                      {"DLSSG.CameraRightY", camera.right[1]},
                      {"DLSSG.CameraRightZ", camera.right[2]},
                      {"DLSSG.CameraUpX", camera.up[0]},
                      {"DLSSG.CameraUpY", camera.up[1]},
                      {"DLSSG.CameraUpZ", camera.up[2]},
                      {"DLSSG.CameraFwdX", camera.forward[0]},
                      {"DLSSG.CameraFwdY", camera.forward[1]},
                      {"DLSSG.CameraFwdZ", camera.forward[2]}})
      tf2vrnr_parameter_set_float(p, pair.first, pair.second);
    for (auto pair :
         {std::pair<const char *, unsigned>{"DLSSG.MultiFrameCount", 1},
          {"DLSSG.MultiFrameIndex", 1},
          {"DLSSG.Reset", reset},
          {"DLSSG.CameraMotionIncluded", 1},
          {"DLSSG.DepthInverted", 0},
          {"DLSSG.ColorBuffersHDR", 0},
          {"DLSSG.MenuDetectionEnabled", 0}})
      tf2vrnr_parameter_set_uint(p, pair.first, pair.second);
    auto *list = i.gpu.Begin();
    if (!list)
      throw std::runtime_error("begin_evaluate");
    for (auto *t : {&e.color, &e.depth, &e.motion})
      GpuInterop::Transition(list, t->d12.Get(), D3D12_RESOURCE_STATE_COMMON,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    GpuInterop::Transition(list, e.output.d12.Get(),
                           D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Ngx(i.evaluate(list, e.feature, p, nullptr), "evaluate");
    for (auto *t : {&e.color, &e.depth, &e.motion})
      GpuInterop::Transition(list, t->d12.Get(),
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_COMMON);
    GpuInterop::Transition(list, e.output.d12.Get(),
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COMMON);
    if (!i.gpu.Submit())
      throw std::runtime_error("submit_evaluate");
    e.previousFrame = frame;
    e.previousCamera = camera;
    e.interpolated = !reset;
    if (reset || frame % 300 == 0)
      Log("fg_evaluate frame=%llu eye=%u reset=%d near=%.4f fov=%.4f",
          (unsigned long long)frame, eye, reset, camera.nearPlane, camera.fov);
    return true;
  } catch (const std::exception &error) {
    Log("fg_evaluate_failed %s", error.what());
    i.failed = true;
    return false;
  }
}
ID3D11Texture2D *FrameGeneration::Real(unsigned eye) const {
  return eye < 2 ? impl_->eyes[eye].color.d11.Get() : nullptr;
}
ID3D11Texture2D *FrameGeneration::Generated(unsigned eye) const {
  return eye < 2 ? impl_->eyes[eye].output.d11.Get() : nullptr;
}
bool FrameGeneration::Interpolated(unsigned eye) const {
  return eye < 2 && impl_->eyes[eye].interpolated;
}
bool FrameGeneration::Shutdown() {
  try {
    return impl_->Close();
  } catch (...) {
    return false;
  }
}
} // namespace tf2vrnr
