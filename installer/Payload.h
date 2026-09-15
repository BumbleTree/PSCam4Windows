#pragma once
//
// Embedded payload access: every file the installer ships is an uncompressed
// RCDATA resource in PSCam4Win-Setup.exe (see installer.rc). Extraction writes
// the resource bytes and then re-reads the file to compare BCrypt SHA-256
// digests, so a failed/verified-corrupt write can never be silently installed.
//
#include <windows.h>
#include <string>

class Logger;

// Which install component a payload item belongs to. Core is always
// installed; driver items follow the wizard's component checkboxes.
enum class Component
{
    Core,
    Ps3Driver,
    EyeToyDriver,
    Ps4Driver,
};

namespace payload
{

// The two shipped binaries, named once. They are referenced from the payload
// table, the COM registration, the process/service handling and the Add/Remove
// entry — and a typo in any one of those is a silent install failure rather
// than a build error.
inline constexpr wchar_t kDllName[]     = L"PSCam4Win.dll";
inline constexpr wchar_t kTrayExeName[] = L"PSCam4WinTray.exe";

struct Item
{
    WORD           resId;      // IDR_* in installer_resource.h
    const wchar_t* relTarget;  // path relative to the install dir
    Component      comp;
};

// Table of everything that gets extracted into the install dir.
const Item* Items(size_t& count);

// Raw view of an embedded resource (points into the mapped image; no copy).
bool GetResourceBytes(WORD id, const BYTE*& data, DWORD& size);

// BCrypt SHA-256 of a memory buffer.
bool Sha256(const BYTE* data, size_t len, BYTE out[32]);

// CREATE_ALWAYS write + flush + read-back + digest compare. Logs the failure
// reason; on digest mismatch the bad file is deleted.
bool WriteFileVerified(const std::wstring& absPath, const BYTE* data, DWORD size, Logger& log);

// Sum of all payload sizes in KB (Add/Remove Programs EstimatedSize).
DWORD TotalPayloadSizeKb();

// GPLv2 text from IDR_LICENSE, converted to UTF-16 with CRLF line endings
// (multiline EDIT controls render bare '\n' as boxes).
std::wstring LoadLicenseText();

} // namespace payload
