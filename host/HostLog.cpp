#include "HostLog.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

void HostLog(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    if (GetConsoleWindow())
        wprintf(L"%s\n", buf);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"PSCam4WinTray: %s\n", buf);
    OutputDebugStringW(line);
}
