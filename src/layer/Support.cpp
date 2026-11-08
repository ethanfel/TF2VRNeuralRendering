#include "Support.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
namespace tf2vrnr {
std::wstring Directory() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&Directory), &module);
    wchar_t path[32768]{};
    const auto n = GetModuleFileNameW(module, path, 32768);
    if (!n || n >= 32768) return {};
    std::wstring result(path, n);
    return result.substr(0, result.find_last_of(L"\\/"));
}
Config ReadConfig() {
    Config c;
    const auto path = Directory() + L"\\TF2VRNeuralRendering.ini";
    c.enabled = GetPrivateProfileIntW(L"layer", L"enabled", 1, path.c_str()) != 0;
    c.capture = GetPrivateProfileIntW(L"capture", L"enabled", 1, path.c_str()) != 0;
    c.captureFirst = GetPrivateProfileIntW(L"capture", L"first_stereo_frame", 0, path.c_str()) != 0;
    c.inspectBindings = GetPrivateProfileIntW(L"capture", L"inspect_bindings", 1, path.c_str()) != 0;
    c.captureCandidates = GetPrivateProfileIntW(L"capture", L"capture_candidates", 1, path.c_str()) != 0;
    c.depthBeforeClear = GetPrivateProfileIntW(L"capture", L"depth_before_clear", 1, path.c_str()) != 0;
    c.renderPasses = GetPrivateProfileIntW(L"capture", L"render_passes", 1, path.c_str()) != 0;
    c.enginePost = GetPrivateProfileIntW(L"capture", L"engine_post", 0, path.c_str()) != 0;
    c.burstFrames = std::clamp(GetPrivateProfileIntW(L"capture", L"burst_frames", 1, path.c_str()), 1u, 2u);
    c.logEvery = std::clamp(GetPrivateProfileIntW(L"diagnostics", L"log_every_frames", 600, path.c_str()), 60u, 36000u);
    c.neural = GetPrivateProfileIntW(L"neural_rendering", L"enabled", 0, path.c_str()) != 0;
    c.dlaa = GetPrivateProfileIntW(L"dlaa", L"enabled", 0, path.c_str()) != 0;
    c.frameGeneration = GetPrivateProfileIntW(L"frame_generation", L"enabled", 0, path.c_str()) != 0;
    c.neuralResolution = std::clamp(GetPrivateProfileIntW(L"neural_rendering", L"resolution_percent", 50, path.c_str()), 25u, 100u);
    c.neuralIntensity = std::clamp(GetPrivateProfileIntW(L"neural_rendering", L"intensity_percent", 50, path.c_str()), 0u, 100u);
    c.neuralTone = static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"neural_rendering", L"tone_percent", 100, path.c_str())), 0, 100));
    c.neuralDetail = static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"neural_rendering", L"detail_percent", 100, path.c_str())), 0, 100));
    wchar_t preset[16]{};
    GetPrivateProfileStringW(L"neural_rendering", L"model", L"B", preset, 16, path.c_str());
    if (preset[0] >= L'a' && preset[0] <= L'b') preset[0] -= L'a' - L'A';
    if (!preset[1] && preset[0] >= L'A' && preset[0] <= L'B') c.neuralPreset = preset[0] - L'A' + 1;
    else Log("neural_invalid_model fallback=B");
    return c;
}
bool IsTarget() {
    wchar_t path[32768]{};
    const auto n = GetModuleFileNameW(nullptr, path, 32768);
    if (!n || n >= 32768) return false;
    std::wstring name(path, n);
    name = name.substr(name.find_last_of(L"\\/") + 1);
    return !_wcsicmp(name.c_str(), L"Titanfall2.exe") || !_wcsicmp(name.c_str(), L"Titanfall2VRLauncher.exe");
}
void Log(const char* fmt, ...) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    // Open per entry, so neither process termination nor DLL unload loses logs.
    FILE* file = _wfopen((Directory() + L"\\TF2VRNeuralRendering.log").c_str(), L"ab");
    if (!file) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    std::fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list args; va_start(args, fmt); std::vfprintf(file, fmt, args); va_end(args);
    std::fputc('\n', file); std::fclose(file);
}
bool ConsumeCaptureRequest() {
    return DeleteFileW((Directory() + L"\\capture.request").c_str()) != FALSE;
}
std::wstring CaptureDirectory() {
    const auto path = Directory() + L"\\captures";
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log("capture_directory_failed error=%lu", GetLastError()); return {};
    }
    return path;
}
}
