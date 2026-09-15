#pragma once
//
// DeviceRegistry — the single owner of slot -> device mapping.
//
// One global 8-slot map behind its own lock, unifying three enumerators keyed by
// USB port path: the vendored ps3eye::PS3EYECam::getDevices() pool (which is
// authoritative on its own slot indices), eyetoy::EnumeratePortPaths(), and
// ps4::EnumeratePortPaths(). Capture code never touches an enumerator directly.
//
// A PS4 in split mode is the one camera that can occupy TWO slots; the pure
// reconciliation math for that lives in Ps4SlotAssign.h and is unit-tested.
//
#include <cstdint>
#include <memory>
#include <string>
#include "ICameraDevice.h"

namespace framebus { class Writer; }

namespace deviceregistry {

// PS4-specific occupancy of a slot, for the Settings dialog's View/Split controls.
// `homeSlot` is the slot whose persisted Settings carry this camera's ps4View /
// ps4Split (the switchable slot, or the LEFT half of a split pair) — the dialog
// reads/writes those there regardless of which half the user is viewing.
struct Ps4SlotInfo
{
    bool        isPs4    = false;                    // this slot presents a PS4 view
    bool        split    = false;                    // the camera is split into two
    int         homeSlot = -1;                       // where ps4View/ps4Split live
    Ps4ViewKind view     = Ps4ViewKind::SideBySide;  // this slot's current view
    std::string portPath;                            // USB port path of the camera (empty if none)
};
Ps4SlotInfo QueryPs4Slot(int slot);

// Stable FNV-1a/32 hash of a camera's USB port path, used to tag a persisted
// ps4Split with the camera that set it (Settings::ps4SplitOwner). A persisted
// split is honored only when this hash matches the camera now in the slot, so a
// DIFFERENT camera that later reuses the same slot index cannot inherit a stale
// "split" flag and silently claim a second slot. Never returns 0: the stored value
// 0 is reserved as the legacy wildcard owner (split saved before owner tagging, or
// with the camera briefly absent), which the registry honors for any camera. Must
// be deterministic across processes/builds, so std::hash (which may be randomized)
// is deliberately not used.
inline uint32_t Ps4OwnerHash(const std::string& portPath)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : portPath) { h ^= c; h *= 16777619u; }
    return h ? h : 1u;
}

// The DeviceProfile of whatever camera currently occupies `slot` (PS3 Eye,
// EyeToy, or a PS4 view), or nullptr if the slot is empty (which also answers
// "is the slot occupied?"). For a PS4 slot the profile is the per-slot view
// (Left/Right/SBS) from Settings. Forces a re-enumeration. Used by the host to
// pick a device-appropriate initial mode and to re-advertise on hot-plug.
const DeviceProfile* ProfileForSlot(int slot);

// Bit i set when slot i holds a device. Occupancy only: unlike ProfileForSlot
// it does not resolve a PS4 slot's VIEW, so it costs no registry read and is
// cheap enough for a UI refresh that runs on every capture-state change.
unsigned OccupiedSlotMask();

// App-visible display name for `slot`: the occupying device's profile name
// ("PS3 Eye" / "PS2 EyeToy" / "PS4 Camera[ (Left|Right)]"), suffixed " #<slot>"
// only when more than one camera of that type is present (so a lone device of a
// type is unnumbered). Empty slots yield "Camera #<slot>". Used for both the
// virtual-camera friendly name and the Settings combo so they agree. Forces a
// re-enumeration.
void SlotDisplayName(int slot, wchar_t* buf, size_t cap);

// Publish the slot's device capabilities (modes / formats / default) into the
// FrameBus ColdBlock. Called once after bus creation, before the virtual camera
// is registered, so the DLL always has a capability block to build media types
// from. The profile is resolved per-slot from the unified port-path map; an
// EMPTY slot falls back to the PS3 Eye profile.
void PublishColdBlock(framebus::Writer& bus, int slot);

// Build the ICameraDevice for the camera in `slot`, or nullptr if empty.
std::unique_ptr<ICameraDevice> Acquire(int slot);

// Force a re-enumeration: drops unplugged devices and repairs replugged slots.
void Rescan();

// Mark the cached slot map stale without scanning; the next query rebuilds.
// Call on PnP device-change. Read-only queries otherwise serve a short cache.
void Invalidate();

} // namespace deviceregistry
