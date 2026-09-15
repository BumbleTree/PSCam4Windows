#pragma once
//
// Timestamped installer log. Every line goes to a UTF-8 file (flushed per
// line, so the log survives a crash mid-step) and is optionally mirrored to
// the wizard's progress page via PostMessage. The engine worker thread and
// the UI thread may both log, so writes are serialized.
//
#include <windows.h>
#include <string>

class Logger
{
public:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Opens (creates/truncates) the log file. Returns false if the file could
    // not be created; logging then still works UI-only.
    bool Open(const std::wstring& path);
    void Close();

    // Mirror every line to `wnd` as PostMessage(wnd, msg, 0, new std::wstring*)
    // -- the receiver takes ownership of the heap string.
    void SetMirror(HWND wnd, UINT msg);

    void Line(const wchar_t* fmt, ...);

    const std::wstring& Path() const { return _path; }

private:
    void Emit(const std::wstring& text);

    HANDLE           _file = INVALID_HANDLE_VALUE;
    HWND             _mirrorWnd = nullptr;
    UINT             _mirrorMsg = 0;
    std::wstring     _path;
    CRITICAL_SECTION _cs;
};
