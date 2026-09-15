#pragma once
#include <windows.h>

// Watches whether the PS3 Eye's 4-mic array is actually delivering audio.
//
// It exists because the wedge is INVISIBLE at every layer that normally reports
// trouble: PnP says CM_PROB_NONE, nothing is logged, the endpoint enumerates at
// 4 ch / 16 kHz, and IAudioClient's Initialize and Start both succeed. Only the
// packet count gives it away — a wedged array is not quiet, it is empty. So the
// check has to open the endpoint and count, and the answer has to reach the
// user, because the cure (replug the camera) is something only they can do.
//
// The register restore in the PS3 transport prevents this happening in the
// first place; this covers what prevention cannot reach — a camera already
// wedged when the tray starts, and any path the fix misses.
namespace micwatch
{
    enum class Health : int
    {
        Unknown = 0,   // not probed yet, or the endpoint could not be opened
        Alive,         // packets arrived
        Wedged,        // opened and started cleanly, then delivered nothing
    };

    // Probes on a background thread and posts `msg` to `wnd` with
    // WPARAM = (int)Health once it finishes. At most one probe in flight, and
    // never re-probes a result already reported unless Reset() intervenes, so
    // this cannot nag. Safe to call from the UI thread.
    void CheckPs3Async(HWND wnd, UINT msg);

    // Last result, for the Settings status line.
    Health LastPs3Result();

    // Forget the last result so the next arrival probes again. Call on a
    // device change: a replug is exactly what cures the wedge, so a camera that
    // just arrived deserves a fresh answer.
    void Reset();

    // Joins any probe still running. Call once during shutdown, before the
    // process starts tearing down COM and the CRT — the probe holds both.
    // Blocks for at most the probe's 1.2 s listening window.
    void Shutdown();
}
