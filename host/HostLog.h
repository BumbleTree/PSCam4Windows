#pragma once
//
// The tray app's one logging call: printf when a console is attached
// (--console) and OutputDebugString always.
//
// It lives in its own header because it belongs to no subsystem: declaring it
// alongside one would make anything wanting a single line of diagnostics pull
// in that subsystem's whole include tree to reach it.
//
// Safe to call from any thread; the two sinks are independently serialized and
// a line is at most 512 characters (longer is truncated, never dropped).
//

void HostLog(const wchar_t* fmt, ...);
