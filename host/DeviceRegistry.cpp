#include "DeviceRegistry.h"

#include <windows.h>
#include <algorithm>
#include <cwchar>     // wcscmp (display-name disambiguation)
#include <string>
#include <vector>

#include "ps3eye.h"
#include "DeviceProfiles.h"
#include "Ps4SlotAssign.h"                // Ps4SlotRec + pure AssignPs4Slots
#include "../common/FrameBus.h"
#include "../common/Settings.h"           // settings::Load (PS4 view selection)
#include "../common/VCamGuids.h"          // kVCamCount
#include "../transports/usb_bulk/Ps3EyeDevice.h"
#include "../transports/usb_iso/EyeToyDevice.h"
#include "../transports/usb_iso/EyeToyUsb.h"
#include "../transports/usb_ps4/Ps4Usb.h"
#include "../transports/usb_ps4/Ps4ViewDevice.h"
#include "../transports/usb_ps4/Ps4CaptureSource.h"   // RetireAbsent

// DeviceRegistry — the single authoritative owner of the slot -> device map.
// Two enumerators feed it:
//   * the vendored PS3 driver's getDevices() pool (ctx #1), which owns its own
//     stable 0..7 port-path index for PS3 Eyes;
//   * eyetoy::EnumeratePortPaths() (ctx #2) for EyeToys.
//
// PS3 Eyes are AUTHORITATIVE on their getDevices() slot index — that index is
// the slot, so a PS3-only system's slot map is fixed by the driver pool and
// nothing here can perturb it. EyeToys then fill the slots PS3 does not claim,
// assigned top-down (slot 7 first) and kept stable by port path across rescans,
// so two EyeToys keep distinct, stable virtual-camera slots through replug.

namespace {

using deviceregistry::Ps4SlotRec;
using deviceregistry::AssignPs4Slots;

// Serializes both enumerations and the persistent EyeToy slot assignment across
// every per-slot capture thread (was CaptureController's g_devicesLock).
SRWLOCK g_lock = SRWLOCK_INIT;

// Persistent EyeToy assignment: g_eyeToySlot[i] is the port path of the EyeToy
// mapped to slot i (empty = none). Reconciled on every rebuild; stable by port
// path. PS3 slots are taken from getDevices() each rebuild, so an EyeToy is
// never left occupying a slot a PS3 Eye has claimed.
std::string g_eyeToySlot[kVCamCount];

// Persistent PS4 assignment, mirroring g_eyeToySlot. Each occupied slot records
// the camera's USB port path plus how it presents that camera (switchable = one
// slot, view from Settings; split = two slots with fixed Left/Right views). The
// reconciliation math is in Ps4SlotAssign.h (pure + unit-tested). The port path
// is stable across the 0580->058A firmware re-enumeration, so a slot survives it.
Ps4SlotRec g_ps4Slot[kVCamCount];

// The Ps4ViewKind a PS4 slot presents. A split slot uses its fixed view; a
// switchable slot reads its persisted per-camera Settings (ps4View; default
// SideBySide = the "PS4 Camera" brand). Takes a COPY of the slot's record (taken
// under g_lock by the caller) so it can run lock-free — settings::Load touches
// the registry, not our map.
Ps4ViewKind EffectiveView(const Ps4SlotRec& rec, int slot)
{
    if (rec.split)
        return rec.view;
    const uint32_t v = settings::Load(slot).ps4View;
    return (v <= static_cast<uint32_t>(Ps4ViewKind::SideBySide))
               ? static_cast<Ps4ViewKind>(v)
               : Ps4ViewKind::SideBySide;
}

// Cache state for the slot map. A rebuild is three USB enumerations, so
// read-only queries serve a short-lived cache instead: Invalidate() on PnP, plus
// a TTL covering the window where the device-interface notification beats libusb
// to the device. Acquire() still forces -- it is about to open hardware.
bool      g_cacheValid   = false;
ULONGLONG g_lastScanTick = 0;
constexpr ULONGLONG kCacheTtlMs = 2000;

// Rebuild the unified map. Caller holds g_lock. Returns the PS3 pool (sparse
// 8-vector indexed by port path) so callers can mint a PS3EYERef by slot index.
const std::vector<ps3eye::PS3EYECam::PS3EYERef>& RebuildLocked(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (!force && g_cacheValid && (now - g_lastScanTick) < kCacheTtlMs)
    {
        // getDevices(false) returns the existing pool without re-enumerating.
        return ps3eye::PS3EYECam::getDevices(false);
    }
    g_cacheValid   = true;
    g_lastScanTick = now;

    const auto& ps3 = ps3eye::PS3EYECam::getDevices(true);

    bool ps3Slot[kVCamCount] = {};
    for (int i = 0; i < kVCamCount && i < static_cast<int>(ps3.size()); ++i)
        ps3Slot[i] = (ps3[i] != nullptr);

    // Candidate EyeToys, in deterministic order so assignment is reproducible.
    std::vector<std::string> eyes = eyetoy::EnumeratePortPaths();
    std::sort(eyes.begin(), eyes.end());

    // Phase 1 (keep): an existing EyeToy slot whose port path is still present
    // and whose slot PS3 has not since claimed stays put; consume that path.
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (g_eyeToySlot[i].empty())
            continue;
        auto it = std::find(eyes.begin(), eyes.end(), g_eyeToySlot[i]);
        if (it != eyes.end() && !ps3Slot[i])
            eyes.erase(it);            // stayed plugged here; not a new candidate
        else
            g_eyeToySlot[i].clear();   // unplugged, or PS3 took this slot
    }

    // Phase 2 (assign new): each remaining EyeToy fills the first free slot
    // (no PS3, no EyeToy) counting down from the top.
    for (const auto& pp : eyes)
    {
        for (int i = kVCamCount - 1; i >= 0; --i)
        {
            if (!ps3Slot[i] && g_eyeToySlot[i].empty())
            {
                g_eyeToySlot[i] = pp;
                break;
            }
        }
    }

    // PS4 cameras fill the slots PS3 and EyeToy leave free (EyeToy assignment
    // above is finalized first, so a PS4 never lands on an EyeToy's slot). Split
    // mode lets one camera occupy two slots; the pure reconciliation lives in
    // AssignPs4Slots, which queries the split flag from each home slot's Settings.
    bool eyeOcc[kVCamCount] = {};
    for (int i = 0; i < kVCamCount; ++i)
        eyeOcc[i] = !g_eyeToySlot[i].empty();

    const std::vector<std::string> ps4Present = ps4::EnumeratePortPaths();
    // Close + drop any PS4 capture engine whose camera is no longer plugged in:
    // the engine keeps its USB handle open across sleep/wake (for clean restarts),
    // so a removed camera must be retired here to release the handle (and force a
    // firmware re-upload when it returns).
    ps4::Ps4CaptureSource::RetireAbsent(ps4Present);

    AssignPs4Slots(g_ps4Slot, kVCamCount, ps3Slot, eyeOcc, ps4Present,
                   [](int homeSlot, const std::string& pp) {
                       // Honor a persisted split only if it was set for THIS camera
                       // (port path); otherwise a different camera reusing this slot
                       // index would inherit a stale split and grab a second slot.
                       // Owner 0 is accepted as a wildcard: it marks a split saved
                       // before owner tagging existed (or with the camera briefly
                       // absent) — rejecting it would silently un-split every
                       // pre-upgrade setup. It gets stamped on the next Split toggle.
                       const Settings s = settings::Load(homeSlot);
                       return s.ps4Split &&
                              (s.ps4SplitOwner == 0 ||
                               s.ps4SplitOwner == deviceregistry::Ps4OwnerHash(pp));
                   });
    return ps3;
}

} // namespace

namespace deviceregistry {

const DeviceProfile* ProfileForSlot(int slot)
{
    if (slot < 0 || slot >= kVCamCount)
        return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked(false);   // read-only query: cache is fine
    const bool isPs3 = ps3.size() > static_cast<size_t>(slot) && ps3[slot] != nullptr;
    const bool isEye = !g_eyeToySlot[slot].empty();
    const Ps4SlotRec ps4 = g_ps4Slot[slot];   // copy under lock
    ReleaseSRWLockExclusive(&g_lock);
    if (isPs3) return &Ps3EyeProfile();
    if (isEye) return &EyeToyProfile();
    if (!ps4.portPath.empty()) return &Ps4ViewProfile(EffectiveView(ps4, slot));
    return nullptr;
}

unsigned OccupiedSlotMask()
{
    unsigned m = 0;
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked(false);   // read-only query: cache is fine
    for (int i = 0; i < kVCamCount; ++i)
        if ((ps3.size() > static_cast<size_t>(i) && ps3[i] != nullptr) ||
            !g_eyeToySlot[i].empty() || !g_ps4Slot[i].portPath.empty())
            m |= (1u << i);
    ReleaseSRWLockExclusive(&g_lock);
    return m;
}

void SlotDisplayName(int slot, wchar_t* buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    if (slot < 0 || slot >= kVCamCount)
    {
        buf[0] = 0;
        return;
    }
    // Capture every slot's occupant under the lock; resolve names afterwards.
    // 1 = PS3 Eye, 2 = EyeToy, 3 = PS4, 0 = empty.
    int        type[kVCamCount] = {};
    Ps4SlotRec ps4[kVCamCount];
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked(false);   // read-only query: cache is fine
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (ps3.size() > static_cast<size_t>(i) && ps3[i] != nullptr) type[i] = 1;
        else if (!g_eyeToySlot[i].empty())                           type[i] = 2;
        else if (!g_ps4Slot[i].portPath.empty())                   { type[i] = 3; ps4[i] = g_ps4Slot[i]; }
        else                                                         type[i] = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);

    // Unsuffixed display name for a slot. A SWITCHABLE PS4 is one camera whose
    // Left/Right/SBS view is an internal Setting, so it is always branded plainly
    // "PS4 Camera" (the view is not part of its identity, and the OS camera name
    // can't change live anyway). Only SPLIT mode exposes two distinct cameras, so
    // only it carries the "(Left)" / "(Right)" suffixes to tell them apart.
    auto baseName = [&](int i) -> const wchar_t* {
        switch (type[i])
        {
        case 1:  return Ps3EyeProfile().displayName;
        case 2:  return EyeToyProfile().displayName;
        case 3:  return ps4[i].split
                          ? Ps4ViewProfile(EffectiveView(ps4[i], i)).displayName
                          : Ps4ViewProfile(Ps4ViewKind::SideBySide).displayName;  // "PS4 Camera"
        default: return nullptr;
        }
    };

    // Resolve every slot's name once: baseName hits settings::Load (an HKLM
    // round-trip) for a PS4 slot, and the duplicate scan below reads them all.
    const wchar_t* names[kVCamCount] = {};
    for (int i = 0; i < kVCamCount; ++i)
        names[i] = baseName(i);

    const wchar_t* name = names[slot];
    if (!name)
    {
        swprintf_s(buf, cap, L"Camera #%d", slot);
        return;
    }
    // Suffix " #<slot>" only when another slot resolves to the SAME name, so a
    // lone camera (and each half of a split pair, which have distinct names) stays
    // unnumbered while two identical cameras get stable, unique labels.
    int dup = 0;
    for (int i = 0; i < kVCamCount; ++i)
        if (names[i] && wcscmp(names[i], name) == 0)
            ++dup;
    if (dup > 1)
        swprintf_s(buf, cap, L"%s #%d", name, slot);
    else
        swprintf_s(buf, cap, L"%s", name);
}

std::unique_ptr<ICameraDevice> Acquire(int slot)
{
    if (slot < 0 || slot >= kVCamCount)
        return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    const auto& ps3 = RebuildLocked(true);    // about to open hardware: be current
    ps3eye::PS3EYECam::PS3EYERef eye =
        (ps3.size() > static_cast<size_t>(slot)) ? ps3[slot] : nullptr;
    std::string eyeToyPath = g_eyeToySlot[slot];
    const Ps4SlotRec ps4   = g_ps4Slot[slot];
    ReleaseSRWLockExclusive(&g_lock);

    // PS3 Eye takes precedence, then EyeToy, then PS4 (slot assignment guarantees
    // the three never share a slot; the ordering is just defensive). For a split
    // PS4 the two slots build two Ps4ViewDevices (Left/Right) that share one
    // Ps4CaptureSource via its port-path-keyed Acquire().
    if (eye)
        return std::make_unique<Ps3EyeDevice>(std::move(eye));
    if (!eyeToyPath.empty())
        return std::make_unique<EyeToyDevice>(std::move(eyeToyPath));
    if (!ps4.portPath.empty())
    {
        // A split pair's Right half does NOT own the shared ISP controls (only the
        // home/Left view drives exposure/gain/brightness), so the two halves can't
        // fight over the single physical control state.
        const bool isSplitRight = ps4.split && ps4.view == Ps4ViewKind::Right;
        return std::make_unique<Ps4ViewDevice>(ps4.portPath, EffectiveView(ps4, slot),
                                               !isSplitRight);
    }
    return nullptr;
}

void Rescan()
{
    AcquireSRWLockExclusive(&g_lock);
    RebuildLocked(true);
    ReleaseSRWLockExclusive(&g_lock);
}

// Drop the cache so the next query re-enumerates. Deliberately does not scan:
// one PnP notification wakes all eight controllers, and only the first pays.
void Invalidate()
{
    AcquireSRWLockExclusive(&g_lock);
    g_cacheValid = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void PublishColdBlock(framebus::Writer& bus, int slot)
{
    // Resolve the slot's device profile (PS3 Eye / EyeToy / PS4 view). An empty
    // slot advertises the PS3 Eye: the block must describe SOMETHING or the DLL
    // has no media types to build, and the most common camera is the least
    // surprising placeholder.
    const DeviceProfile* prof = ProfileForSlot(slot);
    const DeviceProfile& p = prof ? *prof : Ps3EyeProfile();

    framebus::ColdMode modes[framebus::kMaxModes];
    uint32_t count = p.modeCount;
    if (count > framebus::kMaxModes)
        count = framebus::kMaxModes;
    for (uint32_t i = 0; i < count; ++i)
    {
        modes[i].width      = p.modes[i].width;
        modes[i].height     = p.modes[i].height;
        modes[i].fps        = p.modes[i].fps;
        modes[i].formatMask = p.formatMask;   // per-device, not per-mode
    }
    bus.WriteColdBlock(static_cast<uint32_t>(p.transport), p.defaultFormat, modes, count);
}

Ps4SlotInfo QueryPs4Slot(int slot)
{
    Ps4SlotInfo info;
    if (slot < 0 || slot >= kVCamCount)
        return info;

    AcquireSRWLockExclusive(&g_lock);
    RebuildLocked(false);                     // read-only query: cache is fine
    const Ps4SlotRec rec = g_ps4Slot[slot];
    // For a split RIGHT half, the home (where ps4View/ps4Split live) is the LEFT
    // slot sharing this port path; otherwise the slot is its own home.
    int home = slot;
    if (rec.split && rec.view == Ps4ViewKind::Right)
    {
        for (int i = 0; i < kVCamCount; ++i)
            if (g_ps4Slot[i].portPath == rec.portPath &&
                !(g_ps4Slot[i].split && g_ps4Slot[i].view == Ps4ViewKind::Right))
            {
                home = i;
                break;
            }
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (rec.portPath.empty())
        return info;                          // not a PS4 slot
    info.isPs4    = true;
    info.split    = rec.split;
    info.homeSlot = home;
    info.view     = EffectiveView(rec, slot);
    info.portPath = rec.portPath;
    return info;
}

} // namespace deviceregistry
