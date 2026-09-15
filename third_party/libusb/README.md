# Vendored libusb (prebuilt)

This directory vendors a prebuilt **libusb 1.0.27** so the project is self-contained: a fresh
`git clone` of PS3EyeVCam builds with `build.bat` without a separate libusb checkout.

## Contents

| Path | What |
|---|---|
| `include/libusb.h` | Public API header (the only header consumers need) |
| `lib/x64/libusb-1.0.lib` | Static library, **x64, Release, `/MT`** (matches the app's static CRT) |
| `COPYING` | libusb license (**LGPL-2.1-or-later**) |

## Version

- **libusb 1.0.27** (tag `v1.0.27`, commit `d52e355`).
- Chosen because isochronous transfers on Windows require the full-speed frame-calculation fix
  (libusb commit `8102b75`, first released in **1.0.25**). The PS2 EyeToy path — USB 1.1 full-speed
  isochronous — depends on it.
- The EyeToy runs on the **inbox WinUSB backend**, without a libusbK driver install.

## How it was built (to reproduce / upgrade)

```bat
git clone --depth 1 --branch v1.0.27 https://github.com/libusb/libusb.git libusb-src
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
msbuild libusb-src\msvc\libusb_static.vcxproj /p:Configuration=Release /p:Platform=x64
:: output: libusb-src\build\v142\x64\Release\lib\libusb-1.0.lib  ->  lib\x64\libusb-1.0.lib
:: header: libusb-src\libusb\libusb.h                            ->  include\libusb.h
```

The static-library Release config already compiles with `/MT` (`msvc/Configuration.Base.props`),
so no project edits are needed. To upgrade: bump the tag, rebuild, and replace `include/libusb.h`
and `lib/x64/libusb-1.0.lib` here.
