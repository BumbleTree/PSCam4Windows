#include "SetupLog.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

Logger::Logger()
{
    InitializeCriticalSection(&_cs);
}

Logger::~Logger()
{
    Close();
    DeleteCriticalSection(&_cs);
}

bool Logger::Open(const std::wstring& path)
{
    Close();
    _file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (_file == INVALID_HANDLE_VALUE)
        return false;
    _path = path;
    // UTF-8 BOM so Notepad renders the log correctly.
    const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    WriteFile(_file, bom, sizeof(bom), &written, nullptr);
    return true;
}

void Logger::Close()
{
    if (_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_file);
        _file = INVALID_HANDLE_VALUE;
    }
}

void Logger::SetMirror(HWND wnd, UINT msg)
{
    EnterCriticalSection(&_cs);
    _mirrorWnd = wnd;
    _mirrorMsg = msg;
    LeaveCriticalSection(&_cs);
}

void Logger::Line(const wchar_t* fmt, ...)
{
    wchar_t text[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(text, _TRUNCATE, fmt, args);
    va_end(args);
    Emit(text);
}

void Logger::Emit(const std::wstring& text)
{
    EnterCriticalSection(&_cs);

    if (_file != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t stamped[2200];
        _snwprintf_s(stamped, _TRUNCATE, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\r\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, text.c_str());
        const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, stamped, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 1)
        {
            std::vector<char> utf8(static_cast<size_t>(utf8Len));
            WideCharToMultiByte(CP_UTF8, 0, stamped, -1, utf8.data(), utf8Len, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(_file, utf8.data(), static_cast<DWORD>(utf8Len - 1), &written, nullptr);
            FlushFileBuffers(_file);
        }
    }

    const HWND wnd = _mirrorWnd;
    const UINT msg = _mirrorMsg;
    LeaveCriticalSection(&_cs);

    if (wnd)
    {
        auto* heapCopy = new std::wstring(text);
        if (!PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(heapCopy)))
            delete heapCopy;   // window gone; don't leak
    }
}
