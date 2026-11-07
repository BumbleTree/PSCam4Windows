#pragma once
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

inline int testFailures = 0;
inline int testSkips = 0;
inline void Check(bool ok, const char* name)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++testFailures;
}
inline std::wstring ScratchDirectory()
{
    wchar_t temp[MAX_PATH]{}, id[64]{};
    GUID guid{};
    if (!GetTempPathW(_countof(temp), temp) || FAILED(CoCreateGuid(&guid)) ||
        !StringFromGUID2(guid, id, _countof(id)))
        return {};
    const std::wstring path = std::wstring(temp) + L"PSCam4Win-tests-" + id;
    if (!CreateDirectoryW(path.c_str(), nullptr))
        return {};
    return path;
}
inline bool WriteBytes(const std::wstring& path, const std::vector<BYTE>& bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok;
}
inline std::vector<BYTE> ReadBytes(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD length = GetFileSize(file, nullptr);
    std::vector<BYTE> bytes(length == INVALID_FILE_SIZE ? 0 : length);
    DWORD got = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) ||
        got != bytes.size()) bytes.clear();
    CloseHandle(file);
    return bytes;
}
