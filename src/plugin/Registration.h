#pragma once

#include <windows.h>

#include <string>

namespace tf2vrnr
{
inline constexpr auto kImplicitLayersKey = L"Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
inline constexpr auto kManifestName = L"XR_APILAYER_TF2VR_neural_rendering.json";
enum class RegistrationResult
{
    AlreadyRegistered,
    Registered,
    DisabledByUser,
    Failed,
};

struct RegistrationReport
{
    RegistrationResult result = RegistrationResult::Failed;
    int removedStale = 0;
};

RegistrationReport RegisterImplicitLayer(HKEY root, const std::wstring& subkey, const std::wstring& manifestPath, DWORD initialValue = 0);
int UnregisterImplicitLayer(HKEY root, const std::wstring& subkey, const std::wstring& manifestFileName);
bool IsWine();
// HKLM here is Wine's prefix-local registry, never the Linux host registry.
// Native Windows keeps using HKCU and never requests elevation.
RegistrationReport RegisterForEnvironment(const std::wstring& manifestPath, bool wine);
}
