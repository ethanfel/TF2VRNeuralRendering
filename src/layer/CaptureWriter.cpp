#include "Support.h"
#include <atomic>
#include <thread>
namespace tf2vrnr {
namespace { std::atomic<bool> captureWriteBusy{false}; }
bool CaptureWriteBusy() { return captureWriteBusy.load(std::memory_order_acquire); }
bool QueueCaptureWrite(std::function<void()> work) {
    bool idle = false;
    if (!captureWriteBusy.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return false;
    // A worker can outlive its XR session. Pin code until process exit, and
    // capture CPU data only; teardown need not block the graphics thread on IO.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&QueueCaptureWrite), &module)) {
        captureWriteBusy.store(false, std::memory_order_release); return false;
    }
    try {
        std::thread([work = std::move(work)]() mutable {
            try { work(); }
            catch (...) { Log("capture_async_write_failed exception"); }
            work = {}; // release buffers before admitting a new request
            captureWriteBusy.store(false, std::memory_order_release);
        }).detach();
    } catch (...) {
        captureWriteBusy.store(false, std::memory_order_release); return false;
    }
    return true;
}
}
