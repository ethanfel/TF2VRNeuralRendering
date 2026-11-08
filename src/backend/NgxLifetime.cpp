#include "NgxLifetime.h"
#include "../layer/Support.h"
#include <nvsdk_ngx_defs.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>
namespace tf2vrnr {
namespace {
std::mutex mutex;
unsigned users=0;
// Keep retired devices alive until NGX discards its device registrations.
std::vector<Microsoft::WRL::ComPtr<ID3D12Device>> devices;
}
void JoinNgxRuntime(ID3D12Device* device){
    std::lock_guard lock(mutex);devices.emplace_back(device);++users;
    Log("ngx_runtime_join users=%u devices=%zu",users,devices.size());
}
unsigned LeaveNgxRuntime(HMODULE module){
    std::lock_guard lock(mutex);
    if(!users)return NVSDK_NGX_Result_Success;
    if(users>1){--users;Log("ngx_runtime_leave users=%u shutdown_deferred=1",users);return NVSDK_NGX_Result_Success;}
    // The driver's no-argument global entry point supplies its own status
    // pointer. Unlike the SDK's documented Shutdown1(nullptr), it is safe on
    // this driver (whose Shutdown1 dereferences the supplied device).
    using Shutdown=NVSDK_NGX_Result(NVSDK_CONV*)();
    auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(module,"NVSDK_NGX_D3D12_Shutdown"));
    if(!shutdown)return NVSDK_NGX_Result_Fail;
    auto result=shutdown();Log("ngx_runtime_shutdown users=%u result=0x%08x devices=%zu",users,unsigned(result),devices.size());
    if(result==NVSDK_NGX_Result_Success){users=0;devices.clear();}
    return result;
}
}
