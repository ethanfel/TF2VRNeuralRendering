#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
namespace tf2vrnr {
struct SharedTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> d12;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d11;
};
// One graphics-thread owner. Shared resources cross APIs in COMMON state;
// separate monotonic fences order D3D11 -> D3D12 -> D3D11 without CPU copies.
class GpuInterop {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device5> device11_;
    Ptr<ID3D11DeviceContext4> context11_;
    Ptr<ID3D12Device> device12_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12Fence> input12_, output12_;
    Ptr<ID3D11Fence> input11_, output11_;
    struct Commands {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> list;
        uint64_t completedAt = 0;
    };
    std::array<Commands, 4> commands_;
    HANDLE event_ = nullptr;
    uint64_t serial_ = 0;
    Commands* recording_ = nullptr;
    bool usable_ = false;
    bool Check(HRESULT, const char*);
    bool Wait(uint64_t, DWORD);
public:
    GpuInterop() = default;
    GpuInterop(const GpuInterop&) = delete;
    GpuInterop& operator=(const GpuInterop&) = delete;
    ~GpuInterop();
    bool Init(ID3D11Device*);
    bool CreateTexture(UINT width, UINT height, DXGI_FORMAT, SharedTexture&);
    ID3D12GraphicsCommandList* Begin();
    // Call after all input-producing D3D11 commands and D3D12 recording.
    // Subsequent D3D11 commands are ordered after the D3D12 output fence.
    bool Submit();
    bool Drain(DWORD timeoutMs = 5000);
    ID3D12Device* Device12() const { return device12_.Get(); }
    ID3D11DeviceContext4* Context11() const { return context11_.Get(); }
    static void Transition(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES);
};
}
