#pragma once
//
// Static DeviceProfile table. Data, not code: CaptureController / DeviceRegistry
// / the DLL parameterise off these instead of switching on a product enum.
//
#include "ICameraDevice.h"

// The PS3 Eye profile.
const DeviceProfile& Ps3EyeProfile();

// The PS2 EyeToy profile (OV519/OV7648 over WinUSB iso).
const DeviceProfile& EyeToyProfile();

// The PS4 camera profile for a given logical view (Left / Right / SideBySide).
// Each view is its own virtual camera with its own advertised resolution; all
// share TransportClass::Usb_Ps4 and FMT_YUY2. FindDeviceProfile resolves the
// physical 05A9 IDs to a base profile; this picks the view on top of it.
const DeviceProfile& Ps4ViewProfile(Ps4ViewKind view);

// Profile for a USB VID/PID, or nullptr if the device is not supported.
const DeviceProfile* FindDeviceProfile(uint16_t vid, uint16_t pid);
