#include "GpuInterop.h"
#include "../layer/Support.h"
namespace tf2vrnr {
namespace {
struct SharedHandle {
    HANDLE value = nullptr;
    ~SharedHandle() { if (value) CloseHandle(value); }
};
}
bool GpuInterop::Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    usable_ = false;
    Log("interop_failure operation=%s hr=0x%08lx", operation, static_cast<unsigned long>(hr));
    return false;
}
bool GpuInterop::Wait(uint64_t value, DWORD timeout) {
    if (!value || output12_->GetCompletedValue() >= value) return true;
    if (!Check(output12_->SetEventOnCompletion(value, event_), "fence_event")) return false;
    if (WaitForSingleObject(event_, timeout) != WAIT_OBJECT_0) {
        usable_ = false;
        Log("interop_timeout serial=%llu device_status=0x%08lx", static_cast<unsigned long long>(value),
            static_cast<unsigned long>(device12_->GetDeviceRemovedReason()));
        return false;
    }
    return Check(device12_->GetDeviceRemovedReason(), "device_status");
}
GpuInterop::~GpuInterop() {
    if (output12_ && event_) Wait(serial_, 5000);
    if (event_) CloseHandle(event_);
}
bool GpuInterop::Init(ID3D11Device* device) {
    if (!device || device12_) return false;
    Ptr<ID3D11DeviceContext> context; device->GetImmediateContext(context.GetAddressOf());
    Ptr<IDXGIDevice> dxgi; Ptr<IDXGIAdapter> adapter;
    if (!Check(device->QueryInterface(IID_PPV_ARGS(device11_.GetAddressOf())), "device11_5") ||
        !context || !Check(context.As(&context11_), "context11_4") ||
        !Check(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())), "dxgi_device") ||
        !Check(dxgi->GetAdapter(adapter.GetAddressOf()), "game_adapter") ||
        !Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(device12_.GetAddressOf())), "device12_same_adapter")) return false;
    DXGI_ADAPTER_DESC desc{}; adapter->GetDesc(&desc);
    const auto luid = device12_->GetAdapterLuid();
    if (desc.AdapterLuid.HighPart != luid.HighPart || desc.AdapterLuid.LowPart != luid.LowPart) {
        Log("interop_adapter_mismatch"); return false;
    }
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Check(device12_->CreateCommandQueue(&q, IID_PPV_ARGS(queue_.GetAddressOf())), "queue12")) return false;
    auto fence = [&](Ptr<ID3D12Fence>& f12, Ptr<ID3D11Fence>& f11) {
        SharedHandle shared;
        return Check(device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f12.GetAddressOf())), "shared_fence") &&
            Check(device12_->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value), "export_fence") &&
            Check(device11_->OpenSharedFence(shared.value, IID_PPV_ARGS(f11.GetAddressOf())), "import_fence11");
    };
    if (!fence(input12_, input11_) || !fence(output12_, output11_)) return false;
    for (auto& c : commands_) {
        if (!Check(device12_->CreateCommandAllocator(q.Type, IID_PPV_ARGS(c.allocator.GetAddressOf())), "allocator") ||
            !Check(device12_->CreateCommandList(0, q.Type, c.allocator.Get(), nullptr,
                IID_PPV_ARGS(c.list.GetAddressOf())), "list") || !Check(c.list->Close(), "initial_close")) return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return false;
    usable_ = true;
    Log("interop_ready adapter=%ls shared_fences=2 allocator_slots=%zu", desc.Description, commands_.size());
    return true;
}
bool GpuInterop::CreateTexture(UINT width, UINT height, DXGI_FORMAT format, SharedTexture& result) {
    if (!usable_ || !width || !height || result.d11 || result.d12) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width; d.Height = height; d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = format; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    SharedTexture texture; SharedHandle handle;
    if (!Check(device12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(texture.d12.GetAddressOf())), "shared_texture12") ||
        !Check(device12_->CreateSharedHandle(texture.d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value), "export_texture") ||
        !Check(device11_->OpenSharedResource1(handle.value, IID_PPV_ARGS(texture.d11.GetAddressOf())), "import_texture11")) return false;
    D3D11_TEXTURE2D_DESC actual{}; texture.d11->GetDesc(&actual);
    if (actual.Width != width || actual.Height != height || actual.Format != format ||
        actual.ArraySize != 1 || actual.MipLevels != 1 || actual.SampleDesc.Count != 1) return false;
    result = std::move(texture); return true;
}
ID3D12GraphicsCommandList* GpuInterop::Begin() {
    if (!usable_ || recording_) return nullptr;
    auto& c = commands_[serial_ % commands_.size()];
    if (!Wait(c.completedAt, 5000) || !Check(c.allocator->Reset(), "allocator_reset") ||
        !Check(c.list->Reset(c.allocator.Get(), nullptr), "list_reset")) return nullptr;
    recording_ = &c; return c.list.Get();
}
bool GpuInterop::Submit() {
    if (!usable_ || !recording_) return false;
    auto* c = recording_; recording_ = nullptr;
    if (!Check(c->list->Close(), "list_close")) return false;
    const auto value = serial_ + 1;
    if (!Check(context11_->Signal(input11_.Get(), value), "signal_input11")) return false;
    context11_->Flush();
    if (!Check(queue_->Wait(input12_.Get(), value), "wait_input12")) return false;
    ID3D12CommandList* lists[] = {c->list.Get()}; queue_->ExecuteCommandLists(1, lists);
    if (!Check(queue_->Signal(output12_.Get(), value), "signal_output12")) return false;
    serial_ = c->completedAt = value;
    if (!Check(context11_->Wait(output11_.Get(), value), "wait_output11")) return false;
    context11_->Flush();
    return true;
}
bool GpuInterop::Drain(DWORD timeout) { return output12_ && event_ && Wait(serial_, timeout); }
void GpuInterop::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
}
