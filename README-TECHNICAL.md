# PSCam4Win — Technical Notes

Developer-facing companion to the [README](README.md). How the thing is built, how it fits
together, and which of its limits are hardware rather than bugs.

**PSCam4Win 4.1** — a Windows 11 virtual-camera hub for PlayStation cameras. C++17, Win32, no
framework, GPLv2.

---

## Design constraint

Everything runs in **user space**. Capture uses Microsoft's in-box WinUSB driver through libusb;
presentation uses Media Foundation's `IMFVirtualCamera`. There is no custom kernel driver anywhere
in the product, so Memory Integrity / Core Isolation can stay enabled. The only `.inf` files
shipped are WinUSB binding stubs that tell Windows which in-box driver to attach to which device.

The one thing this costs us: **no virtual microphone**. Every recording endpoint on Windows is a
kernel-mode audio driver. The PS4 array is therefore rendered into an output endpoint
(`host/AudioRender.cpp`) rather than exposed as an input.

---

## Build

```bat
build.bat            :: full build -> build\PSCam4Win.dll, PSCam4WinTray.exe, PSCam4Win-Setup.exe
build.bat installer  :: re-embed + relink only the setup exe (needs a prior full build)
```

Requires **VS 2019/2022 Build Tools** with the C++ workload; `build.bat` locates them with
`vswhere`. It is a batch file — from PowerShell, run it as `cmd /c build.bat`. There is no CMake
file, no solution file and no linter.

Things that bite:

* `build.bat` lists **every `.cpp` explicitly**. A new source file needs both a `cl` line and a
  `link` argument added by hand.
* Everything is **static CRT (`/MT`)** to match the vendored prebuilt libraries: libusb 1.0.27
  (`third_party/libusb`) and libjpeg-turbo 3.1.4 (`third_party/libjpeg-turbo`, EyeToy JPEG decode
  only). Do not switch CRT flavours.
* **`/utf-8` is required.** Sources are UTF-8 without BOM and contain em-dashes in string literals.
* **`/W4`, and the tree builds clean at it.** `VENDORFLAGS` drops to `/W3` for `third_party/ps3eye`
  only.
* The repository path contains a space — quote paths in scripts.
* The installer embeds `PSCam4Win.dll` and `PSCam4WinTray.exe` as **uncompressed RCDATA
  resources**, so a payload change means relinking the setup exe. A full `build.bat` does this
  automatically.

### Tests

```bat
tests\build.bat     :: hardware-free unit suites — installer, FrameBus, source, JPEG, host, UI
```

Run these after a full build; CI runs them too. They need no camera plugged in.

There is also an end-to-end regression suite that drives every layer through the **running** tray
and the real Frame Server, with absent hardware reported as SKIP rather than FAIL. It lives with the
maintainer's local diagnostic tooling and is not part of this tree. If you are working on the
capture path, the thing worth knowing is that such a suite tests whatever tray is *running* — which
may be a stale installed copy rather than the build you just made.

CI (`.github/workflows/build.yml`) runs `build.bat` then the unit suites.
`.github/workflows/release.yml` does the same on a `v*` tag and attaches `PSCam4Win-Setup.exe` to the
GitHub Release. There is no external code-signing service: driver catalogs are self-signed and setup
installs their certificates.

---

## Architecture

One pipeline, three processes.

```
camera USB
  |
  +-- transports/{usb_bulk = PS3 Eye | usb_iso = EyeToy | usb_ps4}  ->  ICameraDevice
        |
        v
  host/CaptureController            one per slot: sleep/wake state machine, mode advertise, controls
        |
        v
  common/FrameBus.h                 lock-free shared-memory frame queue (hot YUY2 + cold caps +
                                    JPEG sidecar), per slot:
                                    Global\PSCam4Win{N}.FrameBus + .FrameReady auto-reset event
        |
        v
  source/VCamSource.cpp             PSCam4Win.dll — an MF IMFVirtualCamera media source, loaded
  (in the Camera Frame Server)      into the Frame Server service as LOCAL SERVICE.
                                    YUY2 native; NV12 and scaling on the fly.
        ^
        |
  common/ControlBus.h               keepalive timestamp back from the DLL; no keepalive and the
                                    tray sleeps the hardware ~3 s later
```

### Processes

| | |
|---|---|
| **`PSCam4WinTray.exe`** (`host/`) | Admin. Owns every capture thread, the 8-slot `DeviceRegistry` (slots keyed by USB port path), PnP device-interface notifications, the settings window with its live preview, and autostart via `ITaskService`. |
| **`PSCam4Win.dll`** (`source/`) | The Media Foundation media source. Runs inside the **Camera Frame Server service** as `LOCAL SERVICE`, which is why the tray needs admin: the IPC objects live in the `Global\` kernel namespace. |
| **`PSCam4Win-Setup.exe`** (`installer/`) | Native Win32 wizard, and its own uninstaller (`--uninstall`). |

### Repository layout

| Path | What's in it |
|---|---|
| `host/` | Tray app: capture controllers, device registry, device profiles, audio render, mic watch |
| `host/ui/` | The settings window — owner-drawn widget set, theme tokens, layout, tray menu |
| `transports/` | One `ICameraDevice` implementation per device family |
| `source/` | The virtual-camera media source DLL |
| `common/` | FrameBus, ControlBus, Settings, IPC names, the frozen vcam GUIDs |
| `installer/` | Wizard, do/undo steps, rollback |
| `driver/` | Self-signed WinUSB INFs, regenerated by `generate_driver.ps1` |
| `third_party/` | libusb, libjpeg-turbo, the PS3EYEDriver port |
| `tests/` | Hardware-free unit suites |
| `res/` | Icon, manifest, resource ids, and `version.h` — the single source of truth for the product version |
| `.github/` | CI and release workflows |
| `Github Assets/` | README images |

---

## Transports

Each device family is an `ICameraDevice` with **its own libusb context** — three contexts in total.

**PS3 Eye** (`transports/usb_bulk`, `third_party/ps3eye`) streams raw Bayer over bulk endpoints.
`ps3eye.cpp` runs a **fused single-pass Bayer→YUY2 debayer** that writes straight to BT.601 YUY2
through a ~1.9 KB cache-resident row scratch buffer, skipping the 1.2 MB BGRA intermediate the
original driver used. 13 native modes, 640×480 @ 75 fps down to 320×240 @ 187 fps; serves YUY2 and
NV12, advertising NV12 640×480 @ 60 by default.

Its OV534 register writes are **copy-on-write restored** (`restore_written_regs`) rather than reset
wholesale. Blind-writing the whole register space drops the device into Code 43, and a bring-up
combined with a USB re-enumeration used to wedge the camera's microphone array — invisibly, because
PnP reports no problem, the endpoint enumerates normally and `IAudioClient` starts cleanly. Only the
packet count gives it away. `host/MicWatch.cpp` opens the endpoint and counts, so a camera that was
already wedged when the tray started can be reported to the user, who is the only one who can fix it
by replugging.

**PS2 EyeToy** (`transports/usb_iso`) — OV519 bridge plus OV7648 sensor — streams JFIF over
**USB 1.1 full-speed isochronous**. WinUSB's isochronous support is sufficient here; libusbK turned
out to be unnecessary. Frames are reassembled and decoded to YUY2 with libjpeg-turbo, and MJPEG is
also served as a straight passthrough. Three modes: 320×240 @ 30 (default) and @ 15, and
640×480 @ 15. Flip is done on the CPU during the YUY2 repack because the OV7648 has no hardware flip
register. Gain and exposure stay on the sensor's auto defaults — gspca exposes neither for this
sensor — and the AWB toggle is deliberately omitted, since the OV7648 has no manual R/G/B gain
registers for it to unlock.

**PS4 Camera** (`transports/usb_ps4`) — OV580 bridge plus dual OV9713 sensors. The bootloader
enumerates as **`05A9:0580`**; firmware is uploaded to it on every plug-in and the device then
**re-enumerates as `05A9:058B`** (the final build; `058A` is the identity the dropped legacy build
presented and stays openable only so such a camera can be recognised and reported). After that the
OV580's internal ISP streams calibrated-colour YUY2 with **both eyes in every frame**.
`Ps4ViewDevice` copies or stitches the per-eye bands into Left, Right or Side-by-side. Split mode
runs two slots off the single USB stream.

### Slot assignment

`host/Ps4SlotAssign.h` holds the slot math as pure functions, factored out of `DeviceRegistry` so it
is unit-testable without USB or the registry. Slots are **keyed by USB port path**, which survives
the PS4's `0580`→`058B` re-enumeration, so a camera keeps its slot across rescans. A switchable PS4
occupies one slot — its *home* — and the presented view comes from that slot's `ps4View` setting; a
split PS4 occupies **two**, the home presenting a fixed Left and a second free slot a fixed Right.
Toggling split only adds or removes that second slot. New cameras fill the first free slot counting
down from the top, which keeps the PS3 Eye authoritative on the low indices.

### Virtual camera registration

A virtual camera is registered **only while a physical camera occupies a slot**, and unregistered on
removal — so apps see exactly as many cameras as are plugged in. Arrival and removal are
event-driven through WinUSB device-interface notifications fanned out to the per-slot controllers,
with a 5-second polling fallback for empty slots.

The **8 CLSIDs in `common/VCamGuids.h` are frozen.** They predate the rebrand and must never change,
or every install re-registers COM from scratch. The friendly name shown to apps is chosen per slot
from the occupying device's profile (`host/DeviceProfiles.cpp`). IPC names live in
`common/IpcNames.h`.

---

## FrameBus

Frames move from the capture thread to the media source over a lock-free shared-memory queue.

* **Sub-millisecond latency.** The reader waits on a per-camera auto-reset event
  (`Global\PSCam4Win{N}.FrameReady`, restricted ACLs) rather than polling or sleeping, which avoids
  the 15.6 ms system timer quantum. This is what lets the 100–187 fps modes actually hit their
  target rate.
* **Native format end to end.** The buffer carries **YUY2** — the sensors' own output. A client
  asking for YUY2 (RPCS3, for one) gets the frame byte-for-byte with no conversion. NV12 clients get
  a proper chroma-averaged 4:2:2 → 4:2:0 conversion at delivery time.
* **On-demand scaling, 2:1 only.** When the capture mode and the app's requested resolution differ,
  the DLL scales in the client process: bilinear up, 2×2 box filter down, SWAR integer math, well
  under a millisecond. `ScalePlanFor` in `common/Yuv.h` is the whole policy — `Direct` when the
  sizes match, `Upscale2x` for **320×240 → 640×480 and nothing else**, `Downscale2x` for any exact
  2:1 halving (width must divide by 4, since the kernel walks whole YUY2 macropixel pairs), and
  `Unsupported` otherwise. Both the advertised type list and the delivery path consult that one
  function, so they cannot disagree: a size that delivery could not produce is never advertised,
  which is what stops a client silently receiving black at full frame rate. In the 320×240
  high-speed modes this keeps the memory bus at ~29 MB/s.
* **Sleep/wake.** Coordinated through a shared keepalive timestamp (`common/ControlBus.h`). If the
  DLL stops asking for frames, the tray sleeps the hardware after ~3 seconds.

---

## USB bandwidth

A PS3 Eye at 640×480 @ 60 fps needs roughly **185 Mbps**. It is a USB 2.0 High-Speed device, so even
on an xHCI port it is confined to the USB 2.0 protocol layer and its shared 480 Mbps pool. **A
single controller supports about two cameras** before saturating. Running 3–8 means spreading them
across separate controllers — rear ports, front-panel header, a PCIe USB card.

**The PS4 Camera is a USB 3.0 device and requires a SuperSpeed port.** It streams both 1280 × 800
sensors at 60 fps over isochronous IN on EP `0x81`, with a 49152-byte effective max-packet via the
SuperSpeed companion descriptor — about **275 MB/s**, more than four times what USB 2.0's entire
480 Mbps bus could carry even uncontended. On a 2.0 port it will not stream. This is also why its
transport gets its own libusb context and event thread: sharing a completion thread with the lighter
PS3/EyeToy paths would let either starve the other.

Both the PS3 Eye and the PS2 EyeToy are **composite** devices: interface 0 (`MI_00`) is video, bound
to WinUSB and driven by the tray; interface 1 (`MI_01`) is picked up automatically by the in-box
`usbaudio.sys` and becomes the microphone Windows sees.

---

## The PS4 Camera in detail

### Firmware

The camera holds no firmware of its own; a blob must be uploaded over USB on every physical
connection, exactly as a real PS4 does. PSCam4Win does not redistribute Sony's firmware — **setup
downloads it** into `C:\ProgramData\PSCam4Win\`.

**Exactly one build is supported**: the final OV580 firmware, byte-identical across PS4 system
6.00–7.02, SHA-1 `8b8a6621…`. It is fetched from the community
[OrbisEyeCam](https://github.com/psxdev/OrbisEyeCam) project, with
[PS4-CAMERA-DRIVERS](https://github.com/Hackinside/PS4-CAMERA-DRIVERS) as a byte-identical mirror.
The download is SHA-1 pinned; mismatched bytes are discarded, stale cached copies are replaced on
upgrade, and the hash is re-verified before every USB upload. Any other blob is **refused
everywhere** — the legacy community build was dropped because it wedged the device on reopen.

Offline install: drop a verified `firmware.bin` next to the setup exe, or straight into
`C:\ProgramData\PSCam4Win\`. (`startup.bin` is only needed for the optional `PS4_RAW=1` diagnostic
path, which falls back to a software Bayer demosaic.)

### Modes

Nine per view, all hardware-verified. Side-by-side offers the same nine at double width.

| Geometry | Frame rates |
|---|---|
| 1280 × 800 (sensor-native stereo) | 60 / 30 / 15 |
| 640 × 400 (half-height readout) | 120 / 60 / 30 |
| 320 × 192 | 240 / 120 / 60 |

120 and 240 fps work **provided each frame index is committed with its own
`dwMaxVideoFrameSize`**. Sending the 1280×800 size with every index makes the faster modes look
unsupported — that was our bug, not a chip limit.

### Controls

Measured per-eye on the shipped firmware, on a healthy control pipe:

| Control | Range | Behaviour |
|---|---|---|
| `PU_GAIN` | 0–8 | Mean luma 100 → 148 with **no clipping at either end**. The honest exposure trim. |
| `PU_BRIGHTNESS` | 0–8 | Linear luma offset, ~15/step, but it **clips**: 3.7 % of the frame pinned white at 4, 18 % at 8. Useful at minimum for PS Move / light-gun tracking. |
| `PU_CONTRAST` | 0–8 | A real tone curve — mean 63 → 179 across the range. |
| `PU_SATURATION` | 0–8 | Live. 0 gives U = V = 128 exactly; 8 doubles chroma magnitude. |
| `PU_SHARPNESS` | 0–8 | Live — mean \|dY/dx\| 3.05 → 5.14. Worth turning *down*; on this sensor it amplifies grain nearly as much as detail. |
| `PU_WB_TEMP` + auto | 2800–6500 K | Real white balance. |
| Anti-flicker | 50 / 60 Hz | Match your mains or you get banding under artificial light. |
| `PU_HUE` | — | Advertised by the chip, **measured inert**. Not shown. |

Saturation and sharpness look inert to luma-only photometry, which is why they must be measured in
chroma — that mistake is what hid them for so long.

### The microphone array

4 channels × 16-bit LE, 8 samples per video row, embedded in the video stream. It needs the 72-register
AK5703 bring-up at sub-address `0x24`, **and that only lands before the streaming alt setting is
selected**. Registers `0x02`/`0x12` bits 5:4 carry ~18 dB of analog mic-amp gain that the reference
table omits; the Mic gain slider drives that, about 40 dB of range, defaulting to +14 dB.

The real clock is **48,023 Hz**, not 48,000 — the camera runs at 60.029 fps, and the UVC PTS/SCR was
carrying that all along. Recordings are tagged with the rate measured during the capture so they
play back at the right speed.

Quality is **settled and should not be reopened speculatively**: an unprocessed PS3 Eye reference
measures within 0.1 dB of it (HNR), so the residual roughness is far-field room capture, not the
decode. 39–75 % of the power is acoustic sub-30 Hz rumble; a 60–80 Hz high-pass is the one real
lever (more taps, more gain and dedup all measured nil). **Never reference against Windows Voice
Recorder** — it gates to digital silence and fakes a 39 dB win. Compare against a raw WASAPI
exclusive-mode capture that reports the format it actually got.

---

## Hardware walls

These are **measured limits**, not missing features. Do not "fix" them.

* **The shutter is permanently automatic.** Both AE modes the OV580 accepts are auto. Re-tested on a
  healthy control pipe, where a deliberately low exposure under mode 4 made the image *brighter*.
  `PU_GAIN` is the exposure trim instead; there is no manual-exposure slider and there will not be
  one.
* **The LED shows no light** in streaming mode, though its registers provably round-trip.
* **No recording endpoint of our own** is possible without a kernel audio driver. `host/AudioRender.cpp`
  plus Settings → *Send to:* renders into an endpoint the user already has.

And a standing warning: **a negative measured through a stalled control pipe is not a hardware
wall.** One failed transfer wedges the OV580's control pipe, so everything after it fails too and
results depend on probe *order*. Clear the halt, resync, retry, then re-measure before recording a
limit.

What is **not** a wall, despite earlier claims: 120 and 240 fps, contrast, gain, white balance,
saturation, sharpness, anti-flicker, and the microphone. Each was our bug — a stalled control pipe,
a vendor-register payload offset that was wrong by the 8-byte control SETUP header (the reference
driver's offsets include it; `libusb_control_transfer` excludes it), and the `dwMaxVideoFrameSize`
mistake above.

---

## Persistent state and diagnostics

Settings live in **`HKLM\SOFTWARE\PSCam4Win`** (`common/Settings.cpp`). Slot 0 uses that key
directly; slots 1–7 use `…\PSCam4Win\Camera<N>`. Per-camera values cover the capture mode, flip,
the picture controls, the idle timeout, mic gain and the PS4's `Ps4View` / `Ps4Split` /
`Ps4SplitOwner`. A few are global and live only on the root key — `MicKeepAwake` and `UiTheme`
(0 System, 1 Dark, 2 Light). HKLM rather than HKCU because the tray runs elevated and the values
describe the machine's hardware, not a user's preferences.

Runtime files live in **`C:\ProgramData\PSCam4Win\`**: the PS4 firmware cache and
`install.log`. The installed binaries go to `C:\Program Files\PSCam4Win`.

The tray writes **no log file**. `HostLog` goes to `OutputDebugStringW` (prefixed
`PSCam4WinTray:`) plus `stdout` when a console happens to be attached, so live diagnostics mean
attaching a debugger or running DebugView. The installer is the exception — it logs to
`install.log` as above.

---

## The settings window

`host/ui/` is a hand-rolled owner-drawn widget set. The comctl32 trackbar and progress bar have no
dark-theme class, which is why these are drawn rather than themed; combo boxes stay native so their
drop-downs keep normal keyboard and mouse behaviour.

One widget is **one child window per row**, not per control — a slider row draws its label, track and
value together. That keeps the window count low, makes a half-laid-out row impossible, and lets the
meter bank repaint as a single rect at 10 Hz.

The governing rule is **capability hides, interlock greys**:

* A control the selected camera cannot back is **never emitted** — so hiding it leaves no gap.
* A control that is temporarily unavailable (the Right half of a split PS4 pair, say) is **greyed and
  explained**.

Which controls exist, what they are worth and when they grey all come from `host/ui/ControlModel.h`.

---

## The installer

`PSCam4Win-Setup.exe` is a dependency-free Win32 app built by `build.bat` alongside the other
binaries. The entire payload is embedded as **uncompressed RCDATA resources** and extracted with
**SHA-256 read-back verification**: `PSCam4Win.dll`, `PSCam4WinTray.exe`, the three driver packages
(an `.inf`, `.cat` and `.cer` each for PS3 Eye, EyeToy and PS4), `LICENSE` and
`THIRD_PARTY_NOTICES.txt`. One file is the whole distribution.

Installation is a sequence of **do/undo steps with automatic rollback**: services through native SCM
calls, COM registration via in-process `DllRegisterServer` (for real HRESULTs), certificate stores
through crypt32, driver staging through `pnputil` (whose staged/reboot exit codes are preserved), the
scheduled task, and the Apps & Features entry. Recursive deletes are restricted to an exact-match
whitelist of the install directories. Everything is logged to
`C:\ProgramData\PSCam4Win\install.log`.

The same exe is the uninstaller. Launched from inside `C:\Program Files\PSCam4Win` it restages itself
from `%TEMP%` so it can delete its own directory. It removes every driver package PSCam4Win has ever
installed — including ones from earlier releases, and the early whole-device PS3 Eye package that
stopped Windows loading that camera's microphone.

The startup task uses the `ITaskService` COM API rather than `schtasks.exe`, which is what bypasses
the battery and execution-duration limits.

---

## License

GPLv2 — see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
