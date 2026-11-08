#include "Registration.h"

#include <vector>

namespace tf2vrnr
{
namespace
{
std::wstring FileName(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::vector<std::wstring> ValueNames(HKEY key)
{
    std::vector<std::wstring> names;
    std::vector<wchar_t> buffer(16384);
    for (DWORD index = 0;; ++index)
    {
        DWORD length = static_cast<DWORD>(buffer.size());
        LSTATUS status = RegEnumValueW(key, index, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status != ERROR_SUCCESS)
            break;
        names.emplace_back(buffer.data(), length);
    }
    return names;
}

int RemoveMatching(HKEY key, const std::wstring& manifestFileName, const std::wstring& keep)
{
    int removed = 0;
    for (const std::wstring& name : ValueNames(key))
    {
        if (_wcsicmp(FileName(name).c_str(), manifestFileName.c_str()) != 0 || _wcsicmp(name.c_str(), keep.c_str()) == 0)
            continue;
        if (RegDeleteValueW(key, name.c_str()) == ERROR_SUCCESS)
            ++removed;
    }
    return removed;
}
}

RegistrationReport RegisterImplicitLayer(HKEY root, const std::wstring& subkey, const std::wstring& manifestPath, DWORD initialValue)
{
    RegistrationReport report;
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return report;

    report.removedStale = RemoveMatching(key, FileName(manifestPath), manifestPath);

    DWORD value = 1;
    DWORD type = 0;
    DWORD size = sizeof(value);
    bool present = RegQueryValueExW(key, manifestPath.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS;
    if (present && type == REG_DWORD)
    {
        report.result = value == 0 ? RegistrationResult::AlreadyRegistered : RegistrationResult::DisabledByUser;
    }
    else
    {
        LSTATUS status = RegSetValueExW(key, manifestPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&initialValue), sizeof(initialValue));
        report.result = status != ERROR_SUCCESS ? RegistrationResult::Failed :
            initialValue == 0 ? RegistrationResult::Registered : RegistrationResult::DisabledByUser;
    }
    RegCloseKey(key);
    return report;
}

int UnregisterImplicitLayer(HKEY root, const std::wstring& subkey, const std::wstring& manifestFileName)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
        return 0;
    int removed = RemoveMatching(key, manifestFileName, L"");
    RegCloseKey(key);
    return removed;
}

bool IsWine()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

RegistrationReport RegisterForEnvironment(const std::wstring& manifestPath, bool wine)
{
    if (!wine)
        return RegisterImplicitLayer(HKEY_CURRENT_USER, kImplicitLayersKey, manifestPath);

    // Proton normally reports high integrity. Khronos' Windows loader skips
    // HKCU in that context, so register in HKLM inside this Wine prefix.
    // Carry an explicit user disable across migration, and preserve any
    // existing HKLM value. Remove old HKCU entries only after success.
    DWORD initialValue = 0;
    HKEY previous = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kImplicitLayersKey, 0, KEY_QUERY_VALUE, &previous) == ERROR_SUCCESS)
    {
        DWORD type = 0, size = sizeof(initialValue);
        if (RegQueryValueExW(previous, manifestPath.c_str(), nullptr, &type,
            reinterpret_cast<BYTE*>(&initialValue), &size) != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(initialValue))
            initialValue = 0;
        RegCloseKey(previous);
    }
    auto report = RegisterImplicitLayer(HKEY_LOCAL_MACHINE, kImplicitLayersKey, manifestPath, initialValue);
    if (report.result != RegistrationResult::Failed)
        report.removedStale += UnregisterImplicitLayer(HKEY_CURRENT_USER, kImplicitLayersKey, kManifestName);
    return report;
}
}
