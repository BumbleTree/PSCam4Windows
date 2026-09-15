#include "Payload.h"

#include <bcrypt.h>
#include <vector>

#include "SetupLog.h"
#include "installer_resource.h"

#pragma comment(lib, "bcrypt.lib")

namespace payload
{

namespace
{

// Binaries and the license text into the install root, driver packages into
// driver\.
const Item kItems[] = {
    { IDR_PAYLOAD_DLL,   kDllName,                             Component::Core },
    { IDR_PAYLOAD_TRAY,  kTrayExeName,                         Component::Core },
    { IDR_LICENSE,       L"LICENSE.txt",                       Component::Core },
    { IDR_INF_PS3,       L"driver\\usb_device.inf",            Component::Ps3Driver },
    { IDR_CAT_PS3,       L"driver\\usb_device.cat",            Component::Ps3Driver },
    { IDR_CER_PS3,       L"driver\\usb_device.cer",            Component::Ps3Driver },
    { IDR_INF_EYETOY,    L"driver\\eyetoy_device.inf",         Component::EyeToyDriver },
    { IDR_CAT_EYETOY,    L"driver\\eyetoy_device.cat",         Component::EyeToyDriver },
    { IDR_CER_EYETOY,    L"driver\\eyetoy_device.cer",         Component::EyeToyDriver },
    { IDR_INF_PS4,       L"driver\\ps4cam_device.inf",         Component::Ps4Driver },
    { IDR_CAT_PS4,       L"driver\\ps4cam_device.cat",         Component::Ps4Driver },
    { IDR_CER_PS4,       L"driver\\ps4cam_device.cer",         Component::Ps4Driver },
};

} // namespace

const Item* Items(size_t& count)
{
    count = _countof(kItems);
    return kItems;
}

bool GetResourceBytes(WORD id, const BYTE*& data, DWORD& size)
{
    data = nullptr;
    size = 0;
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res)
        return false;
    const HGLOBAL block = LoadResource(nullptr, res);
    if (!block)
        return false;
    data = static_cast<const BYTE*>(LockResource(block));
    size = SizeofResource(nullptr, res);
    return data != nullptr && size > 0;
}

bool Sha256(const BYTE* data, size_t len, BYTE out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;

    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
            BCryptFinishHash(hash, out, 32, 0) == 0)
            ok = true;
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool WriteFileVerified(const std::wstring& absPath, const BYTE* data, DWORD size, Logger& log)
{
    BYTE expected[32];
    if (!Sha256(data, size, expected))
    {
        log.Line(L"error: SHA-256 provider unavailable");
        return false;
    }

    HANDLE file = CreateFileW(absPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        log.Line(L"error: cannot create %s (code %lu)", absPath.c_str(), GetLastError());
        return false;
    }

    DWORD written = 0;
    const BOOL writeOk = WriteFile(file, data, size, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!writeOk || written != size)
    {
        log.Line(L"error: short write on %s (%lu of %lu bytes)", absPath.c_str(), written, size);
        DeleteFileW(absPath.c_str());
        return false;
    }

    // Read back and compare digests: catches disk-level corruption and any
    // interference with the freshly written file.
    file = CreateFileW(absPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        log.Line(L"error: cannot re-open %s for verification (code %lu)", absPath.c_str(), GetLastError());
        return false;
    }

    std::vector<BYTE> readBack(size);
    DWORD readTotal = 0;
    BOOL readOk = TRUE;
    while (readTotal < size)
    {
        DWORD chunk = 0;
        readOk = ReadFile(file, readBack.data() + readTotal, size - readTotal, &chunk, nullptr);
        if (!readOk || chunk == 0)
            break;
        readTotal += chunk;
    }
    CloseHandle(file);

    BYTE actual[32] = {};
    if (!readOk || readTotal != size || !Sha256(readBack.data(), size, actual) ||
        memcmp(expected, actual, sizeof(expected)) != 0)
    {
        log.Line(L"error: verification failed for %s -- digest mismatch after write", absPath.c_str());
        DeleteFileW(absPath.c_str());
        return false;
    }
    return true;
}

DWORD TotalPayloadSizeKb()
{
    ULONGLONG total = 0;
    for (const Item& item : kItems)
    {
        const BYTE* data = nullptr;
        DWORD size = 0;
        if (GetResourceBytes(item.resId, data, size))
            total += size;
    }
    return static_cast<DWORD>(total / 1024);
}

std::wstring LoadLicenseText()
{
    const BYTE* data = nullptr;
    DWORD size = 0;
    if (!GetResourceBytes(IDR_LICENSE, data, size))
        return L"(license text missing from this build)";

    // LICENSE is plain ASCII/UTF-8; convert then normalize LF -> CRLF.
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data),
                                            static_cast<int>(size), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data),
                        static_cast<int>(size), wide.data(), wideLen);

    std::wstring out;
    out.reserve(wide.size() + wide.size() / 20);
    for (size_t i = 0; i < wide.size(); ++i)
    {
        if (wide[i] == L'\n' && (i == 0 || wide[i - 1] != L'\r'))
            out += L'\r';
        out += wide[i];
    }
    return out;
}

} // namespace payload
