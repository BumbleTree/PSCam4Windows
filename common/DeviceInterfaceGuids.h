#pragma once
//
// Device-interface GUIDs the tray subscribes to for USB arrival/removal, one per
// supported camera transport. Each value is the DeviceInterfaceGUIDs written by
// that camera's WinUSB INF (driver/*.inf); arrival/removal of the interface is
// exactly the moment libusb can/can't open the device, so the capture threads
// re-evaluate slot occupancy on the notification instead of polling.
//
//   PS3 Eye  — driver/usb_device.inf     {4BCC4C51-4249-4AE8-98BF-356B2C530E77}
//   PS2 EyeToy — driver/eyetoy_device.inf {A659968A-BBFB-4C59-8E95-1B658F0A13E2}
//   PS4 Camera — driver/ps4cam_device.inf {B8F4A2C0-5D31-4E76-9A1B-6C0E3F8D2A47}
//
#include <guiddef.h>

inline constexpr GUID kCameraInterfaceGuids[] = {
    { 0x4bcc4c51, 0x4249, 0x4ae8, { 0x98, 0xbf, 0x35, 0x6b, 0x2c, 0x53, 0x0e, 0x77 } },  // PS3 Eye
    { 0xa659968a, 0xbbfb, 0x4c59, { 0x8e, 0x95, 0x1b, 0x65, 0x8f, 0x0a, 0x13, 0xe2 } },  // PS2 EyeToy
    // PS4 camera: the SAME interface GUID is bound to 0580 (boot) and BOTH
    // streaming identities — 058A (legacy firmware, whole device) and 058B MI_00
    // (final 6.00+ firmware, composite child) — so arrival of any of them,
    // including the post-firmware re-enumerate, wakes the capture threads to
    // re-evaluate slot occupancy.
    { 0xb8f4a2c0, 0x5d31, 0x4e76, { 0x9a, 0x1b, 0x6c, 0x0e, 0x3f, 0x8d, 0x2a, 0x47 } },  // PS4 Camera
};

inline constexpr int kCameraInterfaceGuidCount =
    static_cast<int>(sizeof(kCameraInterfaceGuids) / sizeof(kCameraInterfaceGuids[0]));
