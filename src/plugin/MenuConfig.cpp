#include "MenuConfig.h"
#include <windows.h>

namespace tf2vrnr::menu
{
namespace
{
constexpr const wchar_t* kSections[] = {L"neural_rendering", L"dlaa", L"frame_generation", L"neural_rendering",
    L"neural_rendering", L"neural_rendering", L"neural_rendering", L"neural_rendering"};
constexpr const wchar_t* kKeys[] = {L"enabled", L"enabled", L"enabled", L"model", L"resolution_percent",
    L"intensity_percent", L"tone_percent", L"detail_percent"};
bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
}

bool IniStore::Load(Snapshot& snapshot)
{
    const auto path = directory_ + L"TF2VRNeuralRendering.ini";
    if (!FileExists(path)) return false;
    // Flush the Win32 profile cache so CLI atomic replacements are observed.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    for (size_t i = 0; i < snapshot.values.size(); ++i)
        snapshot.values[i] = static_cast<int>(GetPrivateProfileIntW(kSections[i], kKeys[i], kDefaults[i], path.c_str()));
    wchar_t model[16] = {};
    GetPrivateProfileStringW(L"neural_rendering", L"model", L"B", model, 16, path.c_str());
    snapshot.values[3] = (model[0] == L'A' || model[0] == L'a') ? 0 : 1;
    snapshot.layerEnabled = GetPrivateProfileIntW(L"layer", L"enabled", 1, path.c_str()) != 0;
    snapshot.runtimes = (FileExists(directory_ + L"runtime\\nvngx_dlssnr.dll") ? 1 : 0) |
        (FileExists(directory_ + L"runtime\\nvngx_dlss.dll") ? 2 : 0) |
        (FileExists(directory_ + L"runtime\\nvngx_dlssg.dll") ? 4 : 0);
    return true;
}

int IniStore::Save(const Settings& values)
{
    if (!Valid(values)) return -3;
    const auto path = directory_ + L"TF2VRNeuralRendering.ini";
    const auto temporary = path + L".menu-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    // Copy all unrelated capture/diagnostic settings, edit our keys, then commit
    // once. The renderer can never observe a half-written group of settings.
    if (!CopyFileW(path.c_str(), temporary.c_str(), FALSE)) return -1;
    bool ok = true;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const auto text = i == 3 ? std::wstring(values[i] == 0 ? L"A" : L"B") : std::to_wstring(values[i]);
        ok = WritePrivateProfileStringW(kSections[i], kKeys[i], text.c_str(), temporary.c_str()) && ok;
    }
    // This API returns zero on a successful cache flush as well as on failure.
    // Check each actual key write above, then flush the file handle itself.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) ok = false;
    else
    {
        ok = FlushFileBuffers(output) && ok;
        CloseHandle(output);
    }
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return -1;
    }
    HANDLE signal = CreateFileW((directory_ + L"neural.request").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (signal == INVALID_HANDLE_VALUE) return -6;
    CloseHandle(signal);
    return 2;
}
}
