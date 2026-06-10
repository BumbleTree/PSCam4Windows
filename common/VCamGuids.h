#pragma once
//
// Identity of the PS3 Eye virtual camera media source.
//
// CLSID_PS3EyeVCam is the COM class registered by PS3EyeVCam.dll (the IMFActivate
// implementation). The host passes the string form to MFCreateVirtualCamera as
// `sourceId`, and the Frame Server CoCreates it inside its own service process.
//
#include <guiddef.h>

// {E5C9A2D4-7B3F-4C81-9A06-2F51D08B6E73}
inline constexpr GUID CLSID_PS3EyeVCam =
    { 0xe5c9a2d4, 0x7b3f, 0x4c81, { 0x9a, 0x06, 0x2f, 0x51, 0xd0, 0x8b, 0x6e, 0x73 } };

inline constexpr wchar_t kVCamClsidString[] = L"{E5C9A2D4-7B3F-4C81-9A06-2F51D08B6E73}";

inline constexpr wchar_t kVCamFriendlyName[] = L"PS3 Eye";
