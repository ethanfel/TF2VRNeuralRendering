#pragma once
#include <d3d12.h>
namespace tf2vrnr {
// Release each backend's features before leaving. Driver 615's Shutdown1 also
// invalidates features on the other D3D12 device, so shutdown must be shared.
void JoinNgxRuntime(ID3D12Device*);
unsigned LeaveNgxRuntime(HMODULE);
}
