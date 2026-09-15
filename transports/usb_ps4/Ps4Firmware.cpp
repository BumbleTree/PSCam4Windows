#include "Ps4Firmware.h"

#include <windows.h>
#include <bcrypt.h>
#include <libusb.h>

#include <cstdio>
#include <string>

namespace ps4 {

const FirmwareInfo kKnownFirmware[1] = {
    // Final OV580 build, byte-identical across PS4 system 6.00–7.02 (verified
    // against psxdev/luke_firmwares 600/650/700/702 and the OrbisEyeCam blob).
    { "8b8a6621d358f6782c2e3b31aac2cdfb25102c00", 68032, L"final (PS4 sys 6.00-7.02)" },
};

namespace {

// Named to avoid colliding with the Win32 ReadFile API used inside.
std::vector<uint8_t> ReadWholeFile(const std::wstring& path)
{
    std::vector<uint8_t> data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return data;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20))
    {
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!::ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size())
            data.clear();
    }
    CloseHandle(f);
    return data;
}

// Directory of the running executable (trailing backslash), e.g. for a portable
// install where the blobs sit next to PSCam4WinTray.exe.
std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
}

std::wstring ProgramDataDir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"";
    return std::wstring(buf, n) + L"\\PSCam4Win\\";
}

// Try each candidate directory in priority order; return the first that yields
// a non-empty read of `name`.
// Candidate directories, in priority order. Deliberately ALL ABSOLUTE: a
// relative entry resolves against the CWD, and this process runs elevated.
std::vector<uint8_t> LocateBlob(const wchar_t* name)
{
    const std::wstring dirs[] = { ProgramDataDir(), ExeDir() };
    for (const auto& d : dirs)
    {
        if (d.empty())
            continue;
        std::vector<uint8_t> blob = ReadWholeFile(d + name);
        if (!blob.empty())
            return blob;
    }
    return {};
}

} // namespace

std::string Sha1Hex(const std::vector<uint8_t>& data)
{
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return hex;

    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[20];
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data.data()), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        {
            static const char* h = "0123456789abcdef";
            hex.resize(40);
            for (int i = 0; i < 20; ++i)
            {
                hex[i * 2]     = h[digest[i] >> 4];
                hex[i * 2 + 1] = h[digest[i] & 0xF];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

int IdentifyFirmware(const std::vector<uint8_t>& data)
{
    if (data.empty())
        return -1;
    const std::string hex = Sha1Hex(data);
    for (int i = 0; i < kFirmwareCount; ++i)
        if (hex == kKnownFirmware[i].sha1)
            return i;
    return -1;
}

// startup.bin is replayed verbatim as USB control transfers, so it is pinned
// like firmware.bin. Same SHA-1 the installer downloads (kFwSha1[1]).
static const char* const kStartupSha1 = "48d103615008f73a286014ee5408ea9617bb1d3d";

FirmwareBlobs EnsureFirmware()
{
    FirmwareBlobs out;
    out.startup = LocateBlob(L"startup.bin");
    if (!out.startup.empty() && Sha1Hex(out.startup) != kStartupSha1)
    {
        OutputDebugStringW(L"[PSCam4Win] PS4 startup.bin failed SHA-1 verification "
                           L"-- ignored (RAW mode unavailable; re-run setup)\n");
        out.startup.clear();
    }

    // firmware.bin must hash-match the supported build BEFORE acceptance — a
    // corrupt, outdated, or unknown file is never uploaded to hardware (the
    // installer replaces such a file with the pinned download).
    std::vector<uint8_t> fw = LocateBlob(L"firmware.bin");
    if (!fw.empty())
    {
        const int idx = IdentifyFirmware(fw);
        if (idx < 0)
            OutputDebugStringW(L"[PSCam4Win] PS4 firmware.bin is not the supported "
                               L"build -- ignored (re-run setup)\n");
        else
        {
            out.firmware = std::move(fw);
            out.firmwareIndex = idx;
            wchar_t msg[160];
            swprintf_s(msg, L"[PSCam4Win] PS4 firmware: %s\n", kKnownFirmware[idx].name);
            OutputDebugStringW(msg);
        }
    }
    return out;
}

bool UploadFirmware(libusb_device_handle* h, const std::vector<uint8_t>& fw)
{
    if (!h || fw.empty())
        return false;

    libusb_set_configuration(h, 0);
    libusb_claim_interface(h, 0);   // best-effort; some hosts auto-claim

    const uint32_t chunkSize = 512;
    uint16_t index = 0x14, value = 0;
    bool ok = true;
    for (uint32_t pos = 0; pos < fw.size(); pos += chunkSize)
    {
        const uint16_t size =
            (uint16_t)((chunkSize > (fw.size() - pos)) ? (fw.size() - pos) : chunkSize);
        int sent = libusb_control_transfer(h, 0x40, 0x00, value, index,
                                           const_cast<uint8_t*>(fw.data() + pos), size, 1000);
        if (sent != size)
        {
            ok = false;
            break;
        }
        if ((uint32_t)value + size > 0xFFFF)   // wValue rolls over each 64 KiB
            index += 1;
        value += size;
    }

    // Reboot into the freshly-loaded firmware (re-enumerates as 058A). This
    // tears the device down, so the transfer usually errors -- expected.
    uint8_t go = 0x5b;
    libusb_control_transfer(h, 0x40, 0x00, 0x2200, 0x8018, &go, 1, 1000);
    libusb_release_interface(h, 0);
    return ok;
}

} // namespace ps4
