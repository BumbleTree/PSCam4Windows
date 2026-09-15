#pragma once
//
// Pure PS4 slot-assignment math, factored out of DeviceRegistry so it can be unit
// tested without USB or the registry. Given which slots PS3/EyeToy already hold,
// which PS4 cameras are present (by port path), and whether each camera should
// split, it reconciles the persistent per-slot PS4 map:
//
//   * switchable (split=false): the camera occupies ONE slot (its "home"); the
//     presented view is decided later from that slot's Settings (ps4View);
//   * split (split=true): the camera occupies TWO slots — the home presents the
//     fixed LEFT view, a second free slot presents the fixed RIGHT view — i.e.
//     two independent virtual cameras fanned out over one shared capture source.
//
// Assignment is persistent and stable: a camera keeps its slot(s) across rescans
// (matched by port path, which survives the 0580->058A firmware re-enumeration),
// and toggling split only adds/removes the second slot. New cameras fill the
// first free slot counting down from the top (matching the EyeToy rule), so the
// PS3 Eye stays authoritative on its low indices.
//
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "ICameraDevice.h"   // Ps4ViewKind

namespace deviceregistry {

// One slot's PS4 occupancy. `split`/`view` are meaningful only while `portPath`
// is set; a switchable slot leaves `view` at its default (resolved from Settings).
struct Ps4SlotRec
{
    std::string portPath;                          // empty = no PS4 in this slot
    bool        split = false;                      // part of a split (Left|Right) pair
    Ps4ViewKind view  = Ps4ViewKind::SideBySide;    // the fixed view when split
};

// Reconcile `slots[0..count)` for the cameras in `present` (their port paths).
// `ps3[i]`/`eyeToy[i]` mark slots already claimed by a PS3 Eye / EyeToy. For each
// camera, `wantSplit(homeSlot, portPath)` is queried once (the split flag lives on
// the home slot's Settings, but must belong to THIS camera — see ps4SplitOwner) to
// decide one-slot vs two-slot occupancy.
inline void AssignPs4Slots(
    Ps4SlotRec* slots, int count,
    const bool* ps3, const bool* eyeToy,
    std::vector<std::string> present,
    const std::function<bool(int, const std::string&)>& wantSplit)
{
    std::sort(present.begin(), present.end());

    // Drop stale slots: the camera is gone, or a PS3 Eye / EyeToy has since taken
    // this slot. Reconciliation below re-establishes every present camera.
    for (int i = 0; i < count; ++i)
    {
        if (slots[i].portPath.empty())
            continue;
        const bool isPresent =
            std::find(present.begin(), present.end(), slots[i].portPath) != present.end();
        if (!isPresent || ps3[i] || eyeToy[i])
            slots[i] = Ps4SlotRec{};
    }

    auto firstFree = [&]() -> int {
        for (int i = count - 1; i >= 0; --i)
            if (!ps3[i] && !eyeToy[i] && slots[i].portPath.empty())
                return i;
        return -1;
    };
    // Home = the slot presenting `pp` switchably or as the split LEFT half
    // (anything but the split RIGHT half); -1 if not yet assigned.
    auto findHome = [&](const std::string& pp) -> int {
        for (int i = 0; i < count; ++i)
            if (slots[i].portPath == pp &&
                !(slots[i].split && slots[i].view == Ps4ViewKind::Right))
                return i;
        return -1;
    };
    auto findRight = [&](const std::string& pp) -> int {
        for (int i = 0; i < count; ++i)
            if (slots[i].portPath == pp &&
                slots[i].split && slots[i].view == Ps4ViewKind::Right)
                return i;
        return -1;
    };

    for (const auto& pp : present)
    {
        int home = findHome(pp);
        if (home < 0)
            home = firstFree();
        if (home < 0)
            continue;                  // no slots free; skip this camera
        slots[home].portPath = pp;

        if (wantSplit(home, pp))
        {
            slots[home].split = true;
            slots[home].view  = Ps4ViewKind::Left;
            int right = findRight(pp);
            if (right < 0)
                right = firstFree();
            if (right >= 0)
            {
                slots[right].portPath = pp;
                slots[right].split    = true;
                slots[right].view     = Ps4ViewKind::Right;
            }
            // If no second slot is free the camera degrades to a lone Left view
            // (still usable); it gains its Right half when a slot frees up.
        }
        else
        {
            slots[home].split = false;
            slots[home].view  = Ps4ViewKind::SideBySide;   // unused (view from Settings)
            if (int right = findRight(pp); right >= 0)
                slots[right] = Ps4SlotRec{};               // release the Right slot
        }
    }
}

} // namespace deviceregistry
