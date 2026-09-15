//
// The settings window.
//
// Children are created in code and stacked by Layout; the dialog template is an
// empty shell kept only so the dialog manager still gives us Tab, Esc and
// mnemonics through the main message loop's IsDialogMessageW.
//
// A control the selected camera cannot back is never emitted, so hiding one
// leaves no gap to close. Which controls exist, what they are worth and when
// they grey all come from ControlModel.h.
//
#include "../SettingsDialog.h"

#include <math.h>
#include <memory>
#include <shellapi.h>
#include <string>
#include <vector>

#include "ControlModel.h"
#include "Layout.h"
#include "Theme.h"
#include "Widgets.h"

#include "../AudioRender.h"
#include "../Autostart.h"
#include "../CameraPreview.h"
#include "../DeviceRegistry.h"
#include "../TrayUI.h"
#include "../../common/AudioStatusBus.h"
#include "../../common/Settings.h"
#include "../../common/VCamGuids.h"
#include "../../res/resource.h"

namespace {

using model::Sec;

// Layout metrics, DIP.
constexpr int kMargin   = 18;
constexpr int kColW     = 300;
constexpr int kColGap   = 22;
constexpr int kRowH     = 26;
constexpr int kHdrH     = 22;
constexpr int kChipH    = 28;
constexpr int kPillH    = 20;
constexpr int kSecGap   = 14;
constexpr int kRowGap   = 2;
constexpr int kComboH   = 22;
constexpr int kCapW     = 56;
constexpr int kBtnH     = 28;
constexpr int kPreviewH = 200;
constexpr int kMeterH   = 44;

constexpr UINT_PTR kPersistTimer = 1;
constexpr UINT_PTR kTickTimer    = 2;
constexpr UINT     kTickMs       = 100;
constexpr UINT     kPersistMs    = 500;
// Text refreshes ride the meter tick rather than a timer of their own.
constexpr int      kTextEveryNTicks = 10;

constexpr wchar_t kCableUrl[] = L"https://vb-audio.com/Cable/";

// Everything the window owns. One object rather than a drift of file statics.
struct Ui
{
    HWND               dlg  = nullptr;
    HINSTANCE          inst = nullptr;
    CaptureController* ctl  = nullptr;
    TrayUI*            tray = nullptr;

    int      slot          = 0;
    unsigned shownSlotMask = 0;
    bool     suppress      = false;
    bool     centered      = false;
    unsigned themeGen      = 0;
    int      tick          = 0;

    // Capability snapshot for `slot`, refreshed by Reload().
    const DeviceProfile* prof       = nullptr;
    uint32_t             mask       = 0;
    const VideoMode*     modes      = kVideoModes;
    int                  modeCount  = kVideoModeCount;
    bool                 haveCamera = false;
    bool                 isPs4      = false;
    int                  ps4Home    = -1;
    bool                 ownsShared = true;

    ui::Chip          chips[kVCamCount];
    ui::StatusPill    pill;
    ui::SectionHeader secHdr[(int)Sec::Count];
    ui::SectionHeader captureHdr;
    ui::SliderRow     sliders[model::kSliderCount];
    ui::ToggleRow     toggles[model::kToggleCount];

    ui::InfoText modeCap, viewCap, flickCap, micOutCap, themeCap;
    ui::Combo    modeCombo, viewCombo, flickCombo, micOutCombo, themeCombo;
    ui::ToggleRow splitTgl, keepAwakeTgl, autostartTgl;

    ui::InfoText  sharedNote;   // "these are set on the Left half"
    ui::MeterBank meters;
    ui::InfoText  micStatus, micOutStatus;
    ui::Button    recordBtn, resetBtn, closeBtn;
    ui::InfoText  emptyCard;

    HWND                           previewHost = nullptr;
    std::unique_ptr<CameraPreview> preview;

    std::vector<audiorender::Endpoint> micOutputs;
    bool                               micNoCable = false;
    audiostatus::Reader                micStatusReader;
};

std::unique_ptr<Ui> g;

CaptureController* Ctl() { return g->ctl + g->slot; }

// ---------------------------------------------------------------------------
// Model helpers

bool SliderShown(int i)
{
    return model::Shown(model::kSliders[i].cap, g->mask);
}

bool ToggleShown(int i)
{
    return model::Shown(model::kToggles[i].cap, g->mask);
}

// A section exists only if something in it does.
bool SectionHasRows(Sec s)
{
    for (int i = 0; i < model::kSliderCount; ++i)
        if (model::kSliders[i].section == s && SliderShown(i)) return true;
    for (int i = 0; i < model::kToggleCount; ++i)
        if (model::kToggles[i].section == s && ToggleShown(i)) return true;
    if (s == Sec::Power && (g->mask & CTRL_POWERLINE)) return true;
    return false;
}

// The settings the window currently represents. Sensor fields come from the
// widgets; the PS4 slot-map fields are re-read from the registry because the
// controller's snapshot never tracks them, so persisting from here would
// otherwise write back their service-start values and undo a Split or View
// change made since.
Settings Snapshot()
{
    Settings s = Ctl()->ActiveSettings();
    {
        const Settings fresh = settings::Load(g->slot);
        s.ps4View       = fresh.ps4View;
        s.ps4Split      = fresh.ps4Split;
        s.ps4SplitOwner = fresh.ps4SplitOwner;
    }

    const int m = g->modeCombo.CurSel();
    if (m >= 0 && m < g->modeCount)
    {
        s.width  = g->modes[m].width;
        s.height = g->modes[m].height;
        s.fps    = g->modes[m].fps;
    }

    for (int i = 0; i < model::kSliderCount; ++i)
        if (SliderShown(i))
            s.*model::kSliders[i].field = g->sliders[i].Value();
    for (int i = 0; i < model::kToggleCount; ++i)
        if (ToggleShown(i))
            s.*model::kToggles[i].field = g->toggles[i].Checked();

    if (g->mask & CTRL_POWERLINE)
    {
        const int f = g->flickCombo.CurSel();
        if (f >= 0) s.powerlineFreq = (uint32_t)f;
    }
    return s;
}

void RefreshSliderText(int i)
{
    wchar_t buf[32];
    model::FormatValue(model::kSliders[i], g->prof, g->sliders[i].Value(), buf, 32);
    g->sliders[i].SetValueText(buf);
}

void UpdateEnables();
void Relayout();
void Reload(bool withGlobals = true);

void ApplyLive()
{
    if (g->suppress) return;
    g->tray->ApplySettings(g->slot, Snapshot(), false);
    SetTimer(g->dlg, kPersistTimer, kPersistMs, nullptr);
}

void ApplyAndPersist()
{
    if (g->suppress) return;
    g->tray->ApplySettings(g->slot, Snapshot(), true);
}

// Flush a pending debounced save. Called before the selected camera changes and
// on close, so the last edits are never dropped by the timer firing late.
void FlushPersist()
{
    if (g->dlg && KillTimer(g->dlg, kPersistTimer))
        settings::Save(g->slot, Snapshot());
}

// ---------------------------------------------------------------------------
// Status

struct StatusLine { COLORREF dot; wchar_t text[192]; };

StatusLine DescribeState(CaptureController& c)
{
    const theme::Palette& p = theme::C();
    StatusLine s{ p.fgMuted, L"" };
    const Settings a = c.ActiveSettings();

    switch (c.GetState())
    {
    case CaptureController::State::Streaming:
        s.dot = p.good;
        swprintf_s(s.text, L"Streaming %u × %u · %.1f fps",
                   a.width, a.height, c.MeasuredFpsX10() / 10.0);
        if (c.HasExternalClient())
            wcscat_s(s.text, L" · in use by another app");
        break;
    case CaptureController::State::Asleep:
        s.dot = p.fgMuted;
        wcscpy_s(s.text, L"Idle — the camera sleeps until an app opens it");
        break;
    case CaptureController::State::Waking:
        s.dot = p.warn;
        wcscpy_s(s.text, L"Waking the camera…");
        break;
    case CaptureController::State::CameraMissing:
        s.dot = p.fgMuted;
        wcscpy_s(s.text, L"No camera in this slot");
        break;
    case CaptureController::State::VCamFailed:
        s.dot = p.bad;
        wcscpy_s(s.text, L"Virtual camera registration failing — retrying");
        break;
    case CaptureController::State::Ps4NeedsReplug:
        s.dot = p.warn;
        wcscpy_s(s.text, L"Unplug the PS4 camera and plug it back in to reload its firmware");
        break;
    case CaptureController::State::Fatal:
        s.dot = p.bad;
        wcscpy_s(s.text, L"Fatal error — see the debug log");
        break;
    default:
        s.dot = p.fgMuted;
        wcscpy_s(s.text, L"Starting…");
        break;
    }

    if (c.HasPendingModeChange() && !c.IsPreviewOnly())
        wcscat_s(s.text, L" · new mode applies when no app is using it");
    return s;
}

void UpdateStatus()
{
    // Chip dots track every slot's state, not just the selected one, so they
    // are refreshed on the tick rather than only when the camera list changes.
    for (int i = 0; i < kVCamCount; ++i)
        if (g->shownSlotMask & (1u << i))
            g->chips[i].SetDot(DescribeState(*(g->ctl + i)).dot);

    if (!g->haveCamera)
    {
        g->pill.Set(theme::C().fgMuted, L"No camera connected");
        return;
    }
    const StatusLine s = DescribeState(*Ctl());
    g->pill.Set(s.dot, s.text);
}

// ---------------------------------------------------------------------------
// Microphone

void PopulateMicOutputs()
{
    const std::wstring cur = settings::LoadMicRenderDevice();
    g->micOutputs = audiorender::ListEndpoints();

    g->micOutCombo.Clear();
    g->micOutCombo.Add(L"Off");

    int sel = 0;
    g->micNoCable = true;
    for (size_t i = 0; i < g->micOutputs.size(); ++i)
    {
        const audiorender::Endpoint& e = g->micOutputs[i];
        // Each entry states its outcome: only a loopback cable makes the array
        // visible to another app, and which name that app will show is not
        // guessable from the output's own name.
        std::wstring label = e.name;
        if (e.looksLikeVirtualCable)
        {
            g->micNoCable = false;
            label += L"  →  apps pick \"" + e.captureName + L"\"";
        }
        else
        {
            label += L"  — monitor only";
        }
        g->micOutCombo.Add(label.c_str());
        if (e.id == cur) sel = (int)i + 1;
    }

    // A persisted device that is absent right now keeps its place: unplugging
    // headphones must not silently reset the routing.
    if (!cur.empty() && sel == 0)
    {
        audiorender::Endpoint absent;
        absent.id = cur;
        g->micOutputs.push_back(absent);
        g->micOutCombo.Add(L"(selected device not connected)");
        sel = (int)g->micOutputs.size();
    }
    g->micOutCombo.SetCurSel(sel);
}

// Routing decides this interlock: with nothing to send to, keeping the camera
// awake would hold the stream open to feed a stopped renderer. Shown unchecked
// in that state without writing the stored preference, so it returns intact.
void UpdateKeepAwake()
{
    const bool routed = g->micOutCombo.CurSel() > 0;
    g->keepAwakeTgl.Enable(routed);
    g->keepAwakeTgl.SetChecked(routed && settings::LoadMicKeepAwake());
}

bool UpdateMicOutputStatus()
{
    const int sel = g->micOutCombo.CurSel();

    if (g->micNoCable)
    {
        g->micOutStatus.SetLink(true);
        return g->micOutStatus.SetText(
            L"No virtual audio cable is installed, so other apps cannot see this "
            L"microphone — the outputs above only let you hear it. "
            L"Click here to get VB-CABLE (free), then choose it above.");
    }
    g->micOutStatus.SetLink(false);

    // sel indexes micOutputs offset by the "Off" row. Bounds-checked rather than
    // trusted: the combo and the vector are filled together, but the index comes
    // from a control and an out-of-range one would read past the vector.
    if (sel <= 0 || (size_t)sel > g->micOutputs.size())
        return g->micOutStatus.SetText(
            L"Not sent anywhere. Choose the cable above to use this array as a "
            L"microphone in other apps.");

    audiostatus::Block b{};
    // Fresh() as well as Read(): the block is published per captured frame, so a
    // sleeping camera would otherwise leave the last snapshot on screen.
    const bool live = g->micStatusReader.Open() && g->micStatusReader.Read(b) &&
                      audiostatus::Reader::Fresh(b) && b.renderActive;
    const audiorender::Endpoint& e = g->micOutputs[(size_t)sel - 1];

    wchar_t text[320];
    if (!live)
        wcscpy_s(text, L"Waiting for the camera to stream — audio only exists "
                       L"while video does.");
    else if (e.looksLikeVirtualCable)
        // _TRUNCATE: the endpoint's friendly name comes from its driver and has
        // no length this code controls. swprintf_s would abort the tray on an
        // overlong one; losing the tail of a sentence is the right failure.
        _snwprintf_s(text, _TRUNCATE,
                   L"Playing at %u Hz, %u ms buffered. In other apps, choose "
                   L"\"%s\" as the microphone.",
                   b.endpointRate, b.renderFillMs, e.captureName.c_str());
    else
        swprintf_s(text, L"Playing at %u Hz, %u ms buffered. Monitoring only — "
                         L"other apps cannot record from this.",
                   b.endpointRate, b.renderFillMs);
    return g->micOutStatus.SetText(text);
}

void UpdateMic(bool withText)
{
    if (!(g->mask & CTRL_MICARRAY) || !g->haveCamera)
        return;

    CaptureController* c = Ctl();
    const uint32_t ch   = c->MicChannels();
    const bool     live = ch > 0;

    float levels[CaptureController::kMicMaxChannels] = {};
    if (live)
        c->MicLevels(levels, CaptureController::kMicMaxChannels);
    if (g->meters.SetLevels(levels, live ? (int)ch : 4, live))
        g->meters.Invalidate();

    g->recordBtn.Enable(live);
    g->recordBtn.SetText(c->MicRecording() ? L"Stop" : L"Record");

    if (!withText)
        return;

    // Visibility follows CTRL_MICARRAY, never live audio: no audio is arriving
    // at the moment a camera is selected. This is the interlock half, which
    // says why the block is inactive.
    wchar_t text[320];
    if (c->MicRecording())
        swprintf_s(text, L"Recording… %u s (%u ch, 48 kHz)",
                   c->MicRecordedSeconds(), ch);
    else if (!c->MicRecordingPath().empty())
    {
        const wchar_t* full = c->MicRecordingPath().c_str();
        const wchar_t* name = wcsrchr(full, L'\\');
        // _TRUNCATE: a path with no separator falls back to the whole thing,
        // which can be MAX_PATH long — swprintf_s would abort rather than fit.
        _snwprintf_s(text, _TRUNCATE, L"Saved %s to your Videos folder — click to show it.",
                     name ? name + 1 : full);
    }
    else if (live && c->MicDropouts())
        // A video transport problem wearing a microphone costume, and audible
        // on speech, so it is worth saying out loud.
        swprintf_s(text, L"%u channels · 48 kHz · %u dropped frame%s concealed",
                   ch, c->MicDropouts(), c->MicDropouts() == 1 ? L"" : L"s");
    else if (live)
        swprintf_s(text, L"%u channels · 48 kHz · embedded in the video stream", ch);
    else if (!g->ownsShared)
        wcscpy_s(text, L"The array is recorded on the Left camera of the split pair.");
    else if (c->GetState() != CaptureController::State::Streaming)
        wcscpy_s(text, L"Available while the camera is streaming.");
    else
        wcscpy_s(text, L"The microphone ADC did not start — the camera is "
                       L"streaming but its array stayed silent.");

    const bool haveRec = !c->MicRecordingPath().empty() && !c->MicRecording();
    g->micStatus.SetLink(haveRec);
    g->micStatus.SetText(text);
    UpdateMicOutputStatus();
}

// ---------------------------------------------------------------------------
// Enable / disable

void UpdateEnables()
{
    const Settings s = g->haveCamera ? Snapshot() : settings::Defaults();

    for (int i = 0; i < model::kSliderCount; ++i)
        if (SliderShown(i))
            g->sliders[i].Enable(model::Live(model::kSliders[i], g->mask, s, g->ownsShared));

    for (int i = 0; i < model::kToggleCount; ++i)
        if (ToggleShown(i))
            g->toggles[i].Enable(model::Live(model::kToggles[i], g->mask, s, g->ownsShared));

    if (g->mask & CTRL_POWERLINE)
        g->flickCombo.Enable(g->ownsShared);

    if (g->mask & CTRL_MICARRAY)
    {
        // Routing is deliberately not interlocked on live audio: it is the one
        // control that WAKES the camera.
        g->micOutCombo.Enable(true);
        UpdateKeepAwake();
    }
}

// ---------------------------------------------------------------------------
// Layout

void PlaceCaptionCombo(layout::Column& col, ui::InfoText& cap, ui::Combo& combo,
                       int visibleItems, int dropWidthMul = 1)
{
    const RECT r  = col.Next(kRowH);
    const layout::Cells c = layout::Split(r, kCapW, 0, 8);
    cap.Move(c.label);
    cap.Show(true);

    const int closed = theme::Dp(kComboH);
    const int top    = r.top + ((r.bottom - r.top) - closed) / 2;
    combo.Move({ c.body.left, top, c.body.right,
                 top + closed + ui::Combo::ItemHeightPx() * visibleItems });
    combo.SetDroppedWidthPx((c.body.right - c.body.left) * dropWidthMul);
    combo.Show(true);
}

void PlaceSection(layout::Column& col, Sec s)
{
    col.Gap(kSecGap);
    g->secHdr[(int)s].Move(col.Next(kHdrH));
    g->secHdr[(int)s].Show(true);
    col.Gap(kRowGap);

    // Say why the picture controls are inactive on a split pair's Right half,
    // the way the microphone block already does. Greying with no reason reads
    // as a fault.
    if (s == Sec::Image && !g->ownsShared)
    {
        g->sharedNote.Move(col.Next(30));
        g->sharedNote.Show(true);
    }

    // Toggles above the sliders they gate: "Auto white balance" reads as the
    // switch for the Temperature row below it.
    for (int i = 0; i < model::kToggleCount; ++i)
        if (model::kToggles[i].section == s && ToggleShown(i))
        {
            g->toggles[i].Move(col.Next(kRowH));
            g->toggles[i].Show(true);
        }
    for (int i = 0; i < model::kSliderCount; ++i)
        if (model::kSliders[i].section == s && SliderShown(i))
        {
            g->sliders[i].Move(col.Next(kRowH));
            g->sliders[i].Show(true);
        }
}

void HideEverything()
{
    for (int i = 0; i < model::kSliderCount; ++i) g->sliders[i].Show(false);
    for (int i = 0; i < model::kToggleCount; ++i) g->toggles[i].Show(false);
    for (int i = 0; i < (int)Sec::Count; ++i)     g->secHdr[i].Show(false);
    g->captureHdr.Show(false);
    g->modeCap.Show(false);   g->modeCombo.Show(false);
    g->viewCap.Show(false);   g->viewCombo.Show(false);
    g->flickCap.Show(false);  g->flickCombo.Show(false);
    g->micOutCap.Show(false); g->micOutCombo.Show(false);
    g->splitTgl.Show(false);
    g->keepAwakeTgl.Show(false);
    g->sharedNote.Show(false);
    g->meters.Show(false);
    g->micStatus.Show(false);
    g->micOutStatus.Show(false);
    g->recordBtn.Show(false);
    if (g->previewHost) ShowWindow(g->previewHost, SW_HIDE);
}

void Relayout()
{
    if (!g->dlg) return;
    const unsigned epochBefore = ui::LayoutEpoch();
    ui::BeginLayoutPass();

    // A theme or DPI change frees the font the native combos were handed, and
    // relayout follows both. Gated on the generation so an ordinary camera
    // switch does not re-theme five controls for nothing.
    if (g->themeGen != theme::Generation())
    {
        g->themeGen = theme::Generation();
        for (ui::Combo* c : { &g->modeCombo, &g->viewCombo, &g->flickCombo,
                              &g->micOutCombo, &g->themeCombo })
            c->Restyle();
    }

    HideEverything();

    const int contentTop = kMargin + kChipH + 8 + kPillH + kSecGap;

    // Header: one chip per connected camera, then the state pill.
    {
        HDC dc = GetDC(g->dlg);
        int x = kMargin;
        for (int i = 0; i < kVCamCount; ++i)
        {
            if (!(g->shownSlotMask & (1u << i)))
            {
                g->chips[i].Show(false);
                continue;
            }
            const int w = g->chips[i].MeasureWidth(dc);
            RECT r = layout::Px(0, kMargin, 0, kChipH);
            r.left  = theme::Dp(x);
            r.right = r.left + w;
            g->chips[i].Move(r);
            g->chips[i].Show(true);
            x += MulDiv(w, 96, theme::Dpi()) + 8;
        }
        ReleaseDC(g->dlg, dc);

        RECT pr = layout::Px(kMargin, kMargin + kChipH + 8,
                             kColW * 2 + kColGap, kPillH);
        g->pill.Move(pr);
        g->pill.Show(true);
    }

    if (!g->haveCamera)
    {
        RECT card = layout::Px(kMargin, contentTop + 30, kColW * 2 + kColGap, 90);
        g->emptyCard.Move(card);
        g->emptyCard.Show(true);
        const int h = contentTop + 30 + 90 + kSecGap;

        RECT foot = layout::Px(kMargin, h, kColW * 2 + kColGap, kBtnH);
        g->autostartTgl.Move({ foot.left, foot.top, foot.left + theme::Dp(180), foot.bottom });
        g->autostartTgl.Show(true);
        g->themeCap.Show(false);
        g->themeCombo.Show(false);
        g->resetBtn.Show(false);
        RECT close = { foot.right - theme::Dp(84), foot.top, foot.right, foot.bottom };
        g->closeBtn.Move(close);

        RECT cli{}, win{};
        GetClientRect(g->dlg, &cli);
        GetWindowRect(g->dlg, &win);
        const int chrome = (win.bottom - win.top) - (cli.bottom - cli.top);
        SetWindowPos(g->dlg, nullptr, 0, 0,
                     theme::Dp(kMargin * 2 + kColW * 2 + kColGap) +
                         ((win.right - win.left) - (cli.right - cli.left)),
                     theme::Dp(h + kBtnH + kMargin) + chrome,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        // This branch returns early, so it closes the pass itself. Leaving it
        // open would strand every later Show() in the deferred state.
        ui::EndLayoutPass();
        RECT vacatedEmpty{};
        if (ui::TakeVacatedRegion(vacatedEmpty))
            InvalidateRect(g->dlg, &vacatedEmpty, TRUE);
        return;
    }
    g->emptyCard.Show(false);

    layout::Column left (kMargin, contentTop, kColW, kRowGap);
    layout::Column right(kMargin + kColW + kColGap, contentTop, kColW, kRowGap);

    // ---- left: preview, capture, microphone --------------------------------
    {
        const RECT cell = left.Next(kPreviewH);
        if (g->previewHost)
        {
            SetWindowPos(g->previewHost, nullptr, cell.left, cell.top,
                         cell.right - cell.left, cell.bottom - cell.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(g->previewHost, SW_SHOW);
            if (g->preview) g->preview->SetCell(cell);
        }
    }

    left.Gap(kSecGap);
    g->captureHdr.Move(left.Next(kHdrH));
    g->captureHdr.Show(true);
    left.Gap(kRowGap);
    PlaceCaptionCombo(left, g->modeCap, g->modeCombo, 10);
    if (g->isPs4)
    {
        PlaceCaptionCombo(left, g->viewCap, g->viewCombo, 3);
        g->splitTgl.Move(left.Next(kRowH));
        g->splitTgl.Show(true);
    }

    if (g->mask & CTRL_MICARRAY)
    {
        left.Gap(kSecGap);
        g->secHdr[(int)Sec::Mic].Move(left.Next(kHdrH));
        g->secHdr[(int)Sec::Mic].Show(true);
        left.Gap(kRowGap);

        g->meters.Move(left.Next(kMeterH));
        g->meters.Show(true);

        g->micStatus.Move(left.Next(30));
        g->micStatus.Show(true);

        for (int i = 0; i < model::kSliderCount; ++i)
            if (model::kSliders[i].section == Sec::Mic && SliderShown(i))
            {
                g->sliders[i].Move(left.Next(kRowH));
                g->sliders[i].Show(true);
            }

        PlaceCaptionCombo(left, g->micOutCap, g->micOutCombo, 8, 2);
        g->micOutStatus.Move(left.Next(46));
        g->micOutStatus.Show(true);
        g->keepAwakeTgl.Move(left.Next(kRowH));
        g->keepAwakeTgl.Show(true);

        RECT rec = left.Next(kBtnH);
        rec.right = rec.left + theme::Dp(92);
        g->recordBtn.Move(rec);
        g->recordBtn.Show(true);
    }

    // ---- right: the table-driven sections ----------------------------------
    // Orientation moves to the left column for a camera with no microphone
    // block, which would otherwise leave that column stranded under the preview
    // while the right one runs long.
    const bool orientationLeft = !(g->mask & CTRL_MICARRAY);
    if (orientationLeft && SectionHasRows(Sec::Orientation))
        PlaceSection(left, Sec::Orientation);

    static const Sec kRightOrder[] = { Sec::Image, Sec::WhiteBal, Sec::Orientation, Sec::Power };
    for (Sec s : kRightOrder)
    {
        if (!SectionHasRows(s))
            continue;
        if (s == Sec::Orientation && orientationLeft)
            continue;
        PlaceSection(right, s);
        if (s == Sec::Power)
            PlaceCaptionCombo(right, g->flickCap, g->flickCombo, 3);
    }

    // ---- footer ------------------------------------------------------------
    const int bottom = (left.BottomDip() > right.BottomDip() ? left.BottomDip()
                                                             : right.BottomDip()) + kSecGap + 6;
    const RECT foot = layout::Px(kMargin, bottom, kColW * 2 + kColGap, kBtnH);

    g->autostartTgl.Move({ foot.left, foot.top, foot.left + theme::Dp(170), foot.bottom });
    g->autostartTgl.Show(true);

    // Right to left, each element placed against the edge of the one before it,
    // so widths cannot be double-counted into an overlap.
    int edge = foot.right;

    RECT close{ edge - theme::Dp(84), foot.top, edge, foot.bottom };
    g->closeBtn.Move(close);
    g->closeBtn.Show(true);
    edge = close.left - theme::Dp(8);

    RECT reset{ edge - theme::Dp(104), foot.top, edge, foot.bottom };
    g->resetBtn.Move(reset);
    g->resetBtn.Show(true);
    edge = reset.left - theme::Dp(14);

    {
        const int closed = theme::Dp(kComboH);
        const int top    = foot.top + ((foot.bottom - foot.top) - closed) / 2;
        const int cboLeft = edge - theme::Dp(100);   // not `left`: the layout
                                                     // column of that name is
                                                     // still live here
        g->themeCombo.Move({ cboLeft, top, edge,
                             top + closed + ui::Combo::ItemHeightPx() * 3 });
        g->themeCombo.Show(true);
        g->themeCap.Move({ cboLeft - theme::Dp(48), foot.top,
                           cboLeft - theme::Dp(6), foot.bottom });
        g->themeCap.Show(true);
    }

    // ---- window size -------------------------------------------------------
    RECT cli{}, win{};
    GetClientRect(g->dlg, &cli);
    GetWindowRect(g->dlg, &win);
    const int chromeX = (win.right - win.left) - (cli.right - cli.left);
    const int chromeY = (win.bottom - win.top) - (cli.bottom - cli.top);
    const int wantW = theme::Dp(kMargin * 2 + kColW * 2 + kColGap) + chromeX;
    const int wantH = theme::Dp(bottom + kBtnH + kMargin) + chromeY;
    const bool resized = (win.right - win.left) != wantW || (win.bottom - win.top) != wantH;
    if (resized)
        SetWindowPos(g->dlg, nullptr, 0, 0, wantW, wantH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    ui::EndLayoutPass();   // applies only the visibility that really changed

    // Repaint the PARENT only if the layout actually moved. A widget that
    // shifted repaints itself, but the background it vacated is the dialog's,
    // so that case needs the erase. A pass where nothing moved -- the common
    // one, since most reloads re-run the identical layout -- needs neither;
    // erasing regardless flashes the whole window on every settings change.
    // A resize re-flows everything, so that one repaints wholesale. Otherwise
    // repaint ONLY the ground widgets vacated -- everything that moved or
    // appeared has already painted itself. A pass that shifted nothing (the
    // common case) invalidates nothing at all.
    RECT vacated{};
    if (resized)
        InvalidateRect(g->dlg, nullptr, TRUE);
    else if (ui::TakeVacatedRegion(vacated))
        InvalidateRect(g->dlg, &vacated, TRUE);
}

void CenterOnce()
{
    if (g->centered) return;
    g->centered = true;
    RECT w{};
    GetWindowRect(g->dlg, &w);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(g->dlg, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    const int cx = w.right - w.left, cy = w.bottom - w.top;
    SetWindowPos(g->dlg, nullptr,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - cx) / 2,
                 mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - cy) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Load

// Which slots get a chip: whichever ones the slot map has a device for, and
// nothing else.
//
// The registry ALONE, deliberately. It is the authoritative slot map and it
// updates synchronously with the thing that changed it, so a PS4 Split toggle
// adds or removes the second chip in the same pass that handled the click.
// A capture thread's state is not a second opinion on occupancy -- it lags by
// however long that thread takes to wake. Mixing it in here settles the chip
// row in TWO steps: one layout pass on the click, another when the thread
// catches up, and the second one visibly re-flows the window.
//
// Controller state still decides the DOT COLOUR on each chip and the text in
// the status pill, which is what it is actually authoritative for.
unsigned ConnectedSlotMask()
{
    return deviceregistry::OccupiedSlotMask();
}

void RefreshChips()
{
    g->shownSlotMask = ConnectedSlotMask();
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (!(g->shownSlotMask & (1u << i)))
            continue;
        wchar_t name[48];
        deviceregistry::SlotDisplayName(i, name, 48);
        g->chips[i].SetText(name);
        g->chips[i].SetSelected(i == g->slot);
        g->chips[i].SetDot(DescribeState(*(g->ctl + i)).dot);
    }
    g->haveCamera = g->shownSlotMask != 0;
    if (g->haveCamera && !(g->shownSlotMask & (1u << g->slot)))
    {
        for (int i = 0; i < kVCamCount; ++i)
            if (g->shownSlotMask & (1u << i)) { g->slot = i; break; }
        for (int i = 0; i < kVCamCount; ++i)
            g->chips[i].SetSelected(i == g->slot);
    }
}

// withGlobals=false skips the state that has nothing to do with which camera
// is selected: the autostart query is a COM round-trip to the Task Scheduler
// service, and the mic-output list is a COM sweep of every render AND capture
// endpoint on the machine. Re-running those for a Split click is why toggling
// one would feel like reloading the entire window.
void Reload(bool withGlobals)
{
    const bool wasSuppressed = g->suppress;
    g->suppress = true;

    RefreshChips();

    g->prof = g->haveCamera ? deviceregistry::ProfileForSlot(g->slot) : nullptr;
    if (g->prof)
    {
        g->modes     = g->prof->modes;
        g->modeCount = (int)g->prof->modeCount;
        g->mask      = g->prof->controlMask;
    }
    else
    {
        g->modes     = kVideoModes;
        g->modeCount = kVideoModeCount;
        g->mask      = 0;
    }

    const deviceregistry::Ps4SlotInfo ps4 =
        g->haveCamera ? deviceregistry::QueryPs4Slot(g->slot)
                      : deviceregistry::Ps4SlotInfo{};
    g->isPs4   = ps4.isPs4;
    g->ps4Home = ps4.homeSlot;
    // The Right half of a split pair does not own the shared ISP, so its ISP
    // controls grey. Flip is per-view software mirroring and stays live.
    g->ownsShared = !(ps4.isPs4 && ps4.split && ps4.view == Ps4ViewKind::Right);

    const Settings s = settings::Load(g->slot);

    g->modeCombo.Clear();
    for (int i = 0; i < g->modeCount; ++i)
    {
        wchar_t item[48];
        swprintf_s(item, L"%u × %u  @  %u fps",
                   g->modes[i].width, g->modes[i].height, g->modes[i].fps);
        g->modeCombo.Add(item);
    }
    int sel = -1;
    for (int i = 0; i < g->modeCount; ++i)
        if (g->modes[i].width == s.width && g->modes[i].height == s.height &&
            g->modes[i].fps == s.fps) { sel = i; break; }
    if (sel < 0 && g->prof)
        for (int i = 0; i < g->modeCount; ++i)
            if (g->modes[i].width == g->prof->defaultMode.width &&
                g->modes[i].height == g->prof->defaultMode.height &&
                g->modes[i].fps == g->prof->defaultMode.fps) { sel = i; break; }
    g->modeCombo.SetCurSel(sel < 0 ? 0 : sel);

    // A split pair shares one ISP, driven solely by the home (Left) half, so the
    // shared rows read from the home slot. The non-owner's own stored values are
    // never sent anywhere -- showing them would put two different numbers on one
    // piece of hardware. Per-view rows (flip) still come from this slot.
    const Settings shared =
        (!g->ownsShared && g->ps4Home >= 0 && g->ps4Home != g->slot)
            ? settings::Load(g->ps4Home) : s;

    for (int i = 0; i < model::kSliderCount; ++i)
    {
        if (!SliderShown(i)) continue;
        const model::SliderDef& d = model::kSliders[i];
        g->sliders[i].SetRange(d.hi, model::StepsFor(d, g->prof));
        g->sliders[i].SetValue((d.sharedHw ? shared : s).*d.field);
        RefreshSliderText(i);
    }
    for (int i = 0; i < model::kToggleCount; ++i)
    {
        if (!ToggleShown(i)) continue;
        const model::ToggleDef& d = model::kToggles[i];
        g->toggles[i].SetChecked((d.sharedHw ? shared : s).*d.field);
    }

    if (g->mask & CTRL_POWERLINE)
    {
        g->flickCombo.Clear();
        // Index is the UVC value: 0 off, 1 = 50 Hz, 2 = 60 Hz.
        g->flickCombo.Add(L"Off");
        g->flickCombo.Add(L"50 Hz");
        g->flickCombo.Add(L"60 Hz");
        g->flickCombo.SetCurSel((int)(s.powerlineFreq > 2 ? 1 : s.powerlineFreq));
    }

    if (g->isPs4)
    {
        g->viewCombo.Clear();
        // Item order is Ps4ViewKind, so the index is the enum value.
        g->viewCombo.Add(L"Left eye");
        g->viewCombo.Add(L"Right eye");
        g->viewCombo.Add(L"Side-by-side");
        g->viewCombo.SetCurSel((int)ps4.view);
        g->viewCombo.Enable(!ps4.split);
        g->splitTgl.SetChecked(ps4.split);
    }

    // The endpoint list is global, so it is refreshed when the dialog opens or
    // the camera set changes -- plus whenever it is still empty, which is how a
    // first camera WITH an array gets a populated combo after starting on one
    // without.
    if ((g->mask & CTRL_MICARRAY) && (withGlobals || g->micOutputs.empty()))
        PopulateMicOutputs();

    if (withGlobals)
    {
        g->autostartTgl.SetChecked(autostart::IsEnabled());
        g->themeCombo.SetCurSel((int)theme::GetMode());
    }

    UpdateEnables();
    UpdateStatus();
    Relayout();
    UpdateMic(true);

    g->suppress = wasSuppressed;
}

void SelectSlot(int slot)
{
    if (slot == g->slot || slot < 0 || slot >= kVCamCount)
        return;
    FlushPersist();       // the outgoing camera's last edits
    g->slot = slot;
    Reload(false);        // a different camera, but the same machine
    if (g->preview) g->preview->SetCamera(g->slot);
}

void ResetDefaults()
{
    Settings d = settings::Defaults();
    if (g->prof)
    {
        d.width  = g->prof->defaultMode.width;
        d.height = g->prof->defaultMode.height;
        d.fps    = g->prof->defaultMode.fps;
    }
    settings::Save(g->slot, d);
    // Defaults carry ps4Split=false, so resetting a split pair's home slot
    // dissolves it -- a slot-map change, which needs a rescan.
    if (g->isPs4)
        g->tray->RescanAllControllers();
    Reload(false);
    g->tray->ApplySettings(g->slot, d, false);
}

// ---------------------------------------------------------------------------
// Commands

void OnViewChanged()
{
    if (g->suppress || !g->isPs4 || g->ps4Home < 0) return;
    const int v = g->viewCombo.CurSel();
    if (v < 0) return;
    // Persist first: the rescan is what makes a streaming slot re-create its
    // device, and it must read the new value.
    Settings hs = settings::Load(g->ps4Home);
    hs.ps4View = (uint32_t)v;
    settings::Save(g->ps4Home, hs);
    g->tray->RescanAllControllers();
    Reload(false);
    if (g->preview) g->preview->SetCamera(g->slot);
}

void OnSplitToggled()
{
    if (g->suppress || !g->isPs4 || g->ps4Home < 0) return;
    Settings hs = settings::Load(g->ps4Home);
    const bool want = g->splitTgl.Checked();
    hs.ps4Split = want;
    if (want)
    {
        // Tag the split with this camera so a different one reusing the slot
        // index cannot inherit it. An empty port path stores 0, the wildcard: a
        // hash of "" matches no camera and would persist a split that never
        // engages.
        const std::string pp = deviceregistry::QueryPs4Slot(g->slot).portPath;
        hs.ps4SplitOwner = pp.empty() ? 0u : deviceregistry::Ps4OwnerHash(pp);
    }
    settings::Save(g->ps4Home, hs);
    g->tray->RescanAllControllers();
    Reload(false);
    if (g->preview) g->preview->SetCamera(g->slot);
}

void OnMicOutChanged()
{
    if (g->suppress) return;
    const int i = g->micOutCombo.CurSel();
    if (i < 0) return;
    // A single global REG_SZ rather than part of the per-slot Settings struct.
    // The tray's registry watch picks it up within ~1 s and starts or stops the
    // renderer, including waking the camera.
    settings::SaveMicRenderDevice(i == 0 ? std::wstring() : g->micOutputs[(size_t)i - 1].id);
    UpdateKeepAwake();
    UpdateMicOutputStatus();
}

void OnRecord()
{
    CaptureController* c = Ctl();
    if (c->MicRecording())
    {
        c->MicStopRecording();
    }
    else if (c->MicStartRecording().empty())
    {
        MessageBoxW(g->dlg,
                    L"No microphone audio is available yet. The PS4 camera's array is "
                    L"embedded in its video stream, so the camera has to be streaming "
                    L"first.",
                    L"PSCam4Win", MB_ICONINFORMATION | MB_OK);
    }
    UpdateMic(true);
}

void OnAutostart()
{
    if (g->suppress) return;
    const bool want = g->autostartTgl.Checked();
    if (!(want ? autostart::Enable() : autostart::Disable()))
    {
        g->autostartTgl.SetChecked(!want);
        MessageBoxW(g->dlg, L"Could not update the scheduled task.", L"PSCam4Win",
                    MB_ICONWARNING | MB_OK);
    }
}

void OnThemeChanged()
{
    if (g->suppress) return;
    const int i = g->themeCombo.CurSel();
    if (i < 0) return;
    theme::SetMode((theme::Mode)i);
    theme::ApplyWindowFrame(g->dlg);
    Relayout();
}

// ---------------------------------------------------------------------------
// Construction

void CreateWidgets()
{
    const RECT z{ 0, 0, 0, 0 };
    HWND d = g->dlg;

    for (int i = 0; i < kVCamCount; ++i)
    {
        g->chips[i].Create(d, z, L"");
        g->chips[i].Show(false);
        const int slot = i;
        g->chips[i].onClick = [slot] { SelectSlot(slot); };
    }
    g->pill.Create(d, z);

    for (int i = 0; i < (int)Sec::Count; ++i)
    {
        g->secHdr[i].Create(d, z, model::SectionName((Sec)i));
        g->secHdr[i].Show(false);
    }
    g->captureHdr.Create(d, z, L"CAPTURE");

    for (int i = 0; i < model::kSliderCount; ++i)
    {
        const model::SliderDef& def = model::kSliders[i];
        g->sliders[i].Create(d, z, def.label);
        g->sliders[i].Show(false);
        const bool micGain = def.section == Sec::Mic;
        const int  idx = i;
        // Mic gain is the one slider whose write restarts the stream, so it
        // applies at the end of the interaction rather than per drag tick.
        g->sliders[i].onMove = [idx, micGain] {
            RefreshSliderText(idx);
            if (!micGain) ApplyLive();
        };
        g->sliders[i].onCommit = [micGain] { if (micGain) ApplyLive(); };
    }

    for (int i = 0; i < model::kToggleCount; ++i)
    {
        const model::ToggleDef& def = model::kToggles[i];
        g->toggles[i].Create(d, z, def.label,
                             def.asSwitch ? ui::ToggleRow::Look::Switch
                                          : ui::ToggleRow::Look::Check);
        g->toggles[i].Show(false);
        g->toggles[i].onToggle = [] { UpdateEnables(); ApplyAndPersist(); };
    }

    auto caption = [&](ui::InfoText& t, const wchar_t* s) {
        t.Create(d, z);
        t.SetFont(theme::Font::Body);
        t.SetSingleLine(true);
        t.SetText(s);
        t.Show(false);
    };
    caption(g->modeCap,   L"Mode");
    caption(g->viewCap,   L"View");
    caption(g->flickCap,  L"Mains");
    caption(g->micOutCap, L"Send to");
    caption(g->themeCap,  L"Theme");

    g->modeCombo.Create(d, z, IDC_MODECOMBO);
    g->viewCombo.Create(d, z, IDC_VIEWCOMBO);
    g->flickCombo.Create(d, z, IDC_FLICKCOMBO);
    g->micOutCombo.Create(d, z, IDC_MICOUTCOMBO);
    g->themeCombo.Create(d, z, IDC_THEMECOMBO);
    g->themeCombo.Add(L"System");
    g->themeCombo.Add(L"Dark");
    g->themeCombo.Add(L"Light");

    g->splitTgl.Create(d, z, L"Split into two cameras", ui::ToggleRow::Look::Check);
    g->splitTgl.onToggle = [] { OnSplitToggled(); };

    g->keepAwakeTgl.Create(d, z, L"Keep the mic awake when no app is using the camera",
                           ui::ToggleRow::Look::Check);
    g->keepAwakeTgl.onToggle = [] {
        if (!g->suppress) settings::SaveMicKeepAwake(g->keepAwakeTgl.Checked());
    };

    g->autostartTgl.Create(d, z, L"Start with Windows", ui::ToggleRow::Look::Check);
    g->autostartTgl.onToggle = [] { OnAutostart(); };

    g->sharedNote.Create(d, z);
    g->sharedNote.SetText(L"Both halves share one image processor, so these are set on "
                          L"the Left camera of the split pair.");
    g->sharedNote.Show(false);

    g->meters.Create(d, z);
    g->micStatus.Create(d, z);
    g->micStatus.onClick = [] {
        const std::wstring& p = Ctl()->MicRecordingPath();
        if (!p.empty())
        {
            const std::wstring args = L"/select,\"" + p + L"\"";
            ShellExecuteW(g->dlg, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    };
    g->micOutStatus.Create(d, z);
    g->micOutStatus.onClick = [] {
        if (g->micNoCable)
            ShellExecuteW(g->dlg, L"open", kCableUrl, nullptr, nullptr, SW_SHOWNORMAL);
    };

    g->recordBtn.Create(d, z, L"Record", ui::Button::Kind::Secondary);
    g->recordBtn.onClick = [] { OnRecord(); };

    g->resetBtn.Create(d, z, L"Reset defaults", ui::Button::Kind::Secondary);
    g->resetBtn.onClick = [] { ResetDefaults(); };

    g->closeBtn.Create(d, z, L"Close", ui::Button::Kind::Primary);
    g->closeBtn.onClick = [] { DestroyWindow(g->dlg); };

    g->emptyCard.Create(d, z);
    g->emptyCard.SetCentered(true);
    g->emptyCard.SetText(L"No PlayStation camera is connected.\n\n"
                         L"Plug in a PS3 Eye, a PS2 EyeToy or a PS4 Camera and it will "
                         L"appear here, and as a webcam in your other apps.");
    g->emptyCard.Show(false);

    g->previewHost = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                     0, 0, 10, 10, d, (HMENU)(INT_PTR)IDC_PREVIEW,
                                     g->inst, nullptr);
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g->dlg = dlg;
        theme::SetDpi((int)GetDpiForWindow(dlg));
        theme::ApplyWindowFrame(dlg);

        HICON big   = (HICON)LoadImageW(g->inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
        HICON small = (HICON)LoadImageW(g->inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
        SendMessageW(dlg, WM_SETICON, ICON_BIG,   (LPARAM)big);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)small);

        CreateWidgets();

        g->preview = std::make_unique<CameraPreview>();
        g->preview->SetControllerArray(g->ctl);
        g->preview->Attach(dlg, IDC_PREVIEW, g->inst);

        Reload();
        g->preview->SetCamera(g->slot);
        CenterOnce();

        SetTimer(dlg, kTickTimer, kTickMs, nullptr);
        return TRUE;
    }

    case WM_ERASEBKGND:
    {
        RECT r{};
        GetClientRect(dlg, &r);
        theme::Fill((HDC)wp, r, theme::C().bg);
        return TRUE;
    }

    case WM_MEASUREITEM:
        if (auto* m = (MEASUREITEMSTRUCT*)lp)
        {
            m->itemHeight = ui::Combo::ItemHeightPx();
            return TRUE;
        }
        return FALSE;

    case WM_DRAWITEM:
        ui::Combo::DrawItem((const DRAWITEMSTRUCT*)lp);
        return TRUE;

    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0 &&
            theme::OnSystemThemeChanged())
        {
            theme::ApplyWindowFrame(dlg);
            Relayout();
        }
        return TRUE;

    case WM_DPICHANGED:
        if (theme::SetDpi((int)HIWORD(wp)))
        {
            const RECT* sug = (const RECT*)lp;
            SetWindowPos(dlg, nullptr, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Relayout();
        }
        return TRUE;

    case WM_TIMER:
        if (wp == kPersistTimer)
        {
            KillTimer(dlg, kPersistTimer);
            settings::Save(g->slot, Snapshot());
            UpdateStatus();
        }
        else if (wp == kTickTimer)
        {
            UpdateMic(++g->tick % kTextEveryNTicks == 0);
            if (g->tick % kTextEveryNTicks == 0)
                UpdateStatus();
        }
        return TRUE;

    case WM_COMMAND:
        if (HIWORD(wp) == CBN_SELCHANGE)
        {
            switch (LOWORD(wp))
            {
            case IDC_MODECOMBO:   ApplyAndPersist(); UpdateStatus(); return TRUE;
            case IDC_FLICKCOMBO:  ApplyLive();       return TRUE;
            case IDC_VIEWCOMBO:   OnViewChanged();   return TRUE;
            case IDC_MICOUTCOMBO: OnMicOutChanged(); return TRUE;
            case IDC_THEMECOMBO:  OnThemeChanged();  return TRUE;
            }
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK)
        {
            DestroyWindow(dlg);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        DestroyWindow(dlg);
        return TRUE;

    case WM_DESTROY:
        KillTimer(dlg, kTickTimer);
        FlushPersist();
        // Nothing of ours stays mapped or running while the window is closed:
        // no preview thread, no FrameBus mapping, no preview hold, no reader.
        g->preview.reset();
        g->micStatusReader.Close();
        g->micOutputs.clear();
        g->dlg = nullptr;
        return TRUE;
    }
    return FALSE;
}

} // namespace

namespace settingsdialog {

void SetTray(TrayUI* tray)
{
    if (!g) g = std::make_unique<Ui>();
    g->tray = tray;
}

void Show(HINSTANCE instance, CaptureController* controller)
{
    if (!g) g = std::make_unique<Ui>();
    g->inst = instance;
    g->ctl  = controller;

    if (g->dlg)
    {
        ShowWindow(g->dlg, SW_SHOWNORMAL);
        SetForegroundWindow(g->dlg);
        return;
    }

    ui::Register(instance);   // theme::Init ran at startup: the tray menu needs it too

    g->centered = false;
    HWND dlg = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS),
                                  nullptr, DlgProc, 0);
    if (!dlg)
        return;
    ShowWindow(dlg, SW_SHOWNORMAL);
    SetForegroundWindow(dlg);
}

void RefreshStatus()
{
    if (g && g->dlg) UpdateStatus();
}

void RefreshCameraList()
{
    if (!g || !g->dlg)
        return;
    const unsigned now = ConnectedSlotMask();
    if (now == g->shownSlotMask)
        return;   // membership unchanged, so nothing to rebuild

    // A change in some OTHER slot only touches the chip row; the viewed camera's
    // controls, mode list and preview are left alone.
    const bool keepView = (g->shownSlotMask & (1u << g->slot)) && (now & (1u << g->slot));
    if (keepView)
    {
        g->suppress = true;
        RefreshChips();
        Relayout();
        g->suppress = false;
        return;
    }

    FlushPersist();
    Reload(false);
    if (g->preview) g->preview->SetCamera(g->slot);
}

HWND Hwnd() { return g ? g->dlg : nullptr; }

void Close()
{
    if (g && g->dlg) DestroyWindow(g->dlg);
}

} // namespace settingsdialog
