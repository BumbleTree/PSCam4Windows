#include <windows.h>
#include <objbase.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "TestSupport.h"
#include "../host/CaptureController.h"
#include "../host/ui/Widgets.h"
#define private public
#include "../host/CameraPreview.h"
#undef private

void CaptureController::SetPreviewHold(bool) {}
Settings CaptureController::ActiveSettings() const { return {}; }
static bool failDc = false, failSelect = false;
static HDC TestCreateDc(HDC dc) { return failDc ? nullptr : CreateCompatibleDC(dc); }
static HGDIOBJ TestSelect(HDC dc, HGDIOBJ object) { return failSelect ? nullptr : SelectObject(dc, object); }
#define CreateCompatibleDC TestCreateDc
#define SelectObject TestSelect
#include "../host/CameraPreview.cpp"
#undef CreateCompatibleDC
#undef SelectObject

int main()
{
    theme::Init();
    bool colors = true;
    for (int i = 0; i < 64; ++i)
    {
        const COLORREF color = RGB(i, i + 1, i + 2);
        LOGBRUSH brush{}; LOGPEN pen{};
        colors &= GetObjectW(theme::Brush(color), sizeof(brush), &brush) == sizeof(brush) && brush.lbColor == color;
        colors &= GetObjectW(theme::Pen(color, i + 1), sizeof(pen), &pen) == sizeof(pen) && pen.lopnColor == color && pen.lopnWidth.x == i + 1;
    }
    Check(colors, "theme caches preserve requested colors and widths beyond 24 entries");
    LOGFONTW font{};
    theme::SetDpi(192); GetObjectW(theme::GetFont(theme::Font::Body), sizeof(font), &font);
    Check(font.lfHeight == -24, "DPI transition rebuilds fonts at the new monitor scale");
    theme::SetDpi(96);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND parent = CreateWindowW(L"STATIC", L"hidden UI test", WS_POPUP, 0, 0, 600, 300, nullptr, nullptr, instance, nullptr);
    Check(parent && ui::Register(instance), "hidden widget fixture created");
    if (parent)
    {
        RECT rect{ 0, 0, 600, 26 };
        ui::SliderRow slider; slider.Create(parent, rect, L"Test"); slider.SetRange(255, 0);
        int commits = 0; slider.onCommit = [&] { ++commits; };
        SendMessageW(slider.Hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(200, 10));
        SendMessageW(slider.Hwnd(), WM_CANCELMODE, 0, 0);
        const auto value = slider.Value();
        SendMessageW(slider.Hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(450, 10));
        Check(slider.Value() == value && commits == 1, "cancelled slider drag commits once and cannot move without a new press");
        SendMessageW(slider.Hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(200, 10));
        SetCapture(parent);
        const auto second = slider.Value();
        SendMessageW(slider.Hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(450, 10));
        Check(slider.Value() == second && commits == 2, "capture theft cancels a slider drag");
        ReleaseCapture();
        ui::Button button; button.Create(parent, rect, L"Test", ui::Button::Kind::Secondary);
        int clicks = 0; button.onClick = [&] { ++clicks; };
        SendMessageW(button.Hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(100, 10));
        SendMessageW(button.Hwnd(), WM_CANCELMODE, 0, 0);
        SendMessageW(button.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(100, 10));
        Check(clicks == 0, "cancelled button press cannot fire a delayed click");
        DestroyWindow(parent);
    }

    CameraPreview preview;
    const DWORD initial = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    failDc = true;
    Check(!preview.RecreateDib(16, 16) && !preview._dib && !preview._dibDc && !preview._dibBits,
          "preview DC creation failure cleans every partial DIB allocation");
    failDc = false; failSelect = true;
    Check(!preview.RecreateDib(16, 16) && !preview._dib && !preview._dibDc && !preview._dibBits,
          "preview bitmap selection failure cleans every partial allocation");
    failSelect = false;
    Check(preview.RecreateDib(16, 16), "preview retries successfully after an allocation failure");
    preview.Detach();
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == initial, "preview failure/retry cycle does not leak GDI handles");
    theme::Shutdown();
    printf("UI: %d failures\n", testFailures);
    return testFailures ? 1 : 0;
}
