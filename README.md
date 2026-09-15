# PSCam4Win — PlayStation cameras as Windows Virtual Cameras (Windows 11 Only)

PSCam4Win lets you use classic PlayStation cameras as standard web cameras (and audio inputs) on modern Windows 11 systems. It currently supports the **PlayStation 3 Eye**, the **PlayStation 2 EyeToy**, and the **PlayStation 4 Camera**, behind one common user-space pipeline.

Once installed, each plugged-in camera appears to apps like **Discord, Zoom, OBS, PCSX2 / RPCS3, and web browsers** as a regular built-in camera, named after the device that occupies the slot (e.g. **"PS3 Eye"**, **"PS2 EyeToy"**, or **"PS4 Camera"**). The integrated microphones are also exposed as standard recording devices named **"USB Camera-B4.09.24.1"** (for the PS3 Eye's 4-channel array) and **"Logitech EyeToy USB Camera"** (for the PS2 EyeToy). Everything operates in user space (no risky kernel drivers — only Microsoft's inbox WinUSB for video) and is fully safe for Memory Integrity / Core Isolation settings.

> **Device support:** PS3 Eye streams raw Bayer that is debayered to YUY2 in-process; the PS2 EyeToy (OmniVision OV519/OV7648) streams JPEG over WinUSB isochronous and is decoded to YUY2 with libjpeg-turbo; the PS4 Camera (OmniVision OV580 bridge + dual OV9713 sensors) uploads firmware over USB, then streams full-colour stereo YUY2 from its in-camera image processor (up to 60 fps at full resolution, 240 fps at the smallest, both eyes every frame). All three ride the same FrameBus → virtual-camera path. The PS4 camera additionally lets you pick which eye to show (Left / Right / Side-by-side) or **split it into two separate virtual cameras**. PS5 camera support is planned.
>
> **PS4 status — functional, not feature-complete.** The PS4 camera works as a colour-stereo virtual camera at **nine resolutions and frame rates — up to 1280x800 @ 60 fps, 640x400 @ 120 fps and 320x192 @ 240 fps** — with view selection, split mode, **brightness, contrast, gain, saturation, sharpness, white-balance and mains anti-flicker controls**, and a working **4-microphone array** (meters, gain and a WAV recorder). It is still **not** a full reproduction of the device. Verified hardware limits (measured on real hardware): **shutter/exposure time is always automatic** — the ISP accepts manual exposure writes but its auto-exposure loop compensates them away (gain, however, is a real always-live trim); the **status LED** is not controllable in this mode; and the microphone array, being embedded in the video stream rather than exposed as USB audio, cannot be turned into a **Windows recording device by PSCam4Win itself** — every virtual microphone on Windows is a kernel-mode audio driver, and this project ships none. It can still be used in any app: **Settings > Send to:** plays the array into an output device of your choice, so pointing it at a virtual audio cable makes it a microphone everywhere. See the [PlayStation 4 Camera](#playstation-4-camera) section.

```
                              ┌── Interface 0 (MI_00) ── WinUSB ── libusb ── PS3EYEDriver ──► PSCam4WinTray.exe
                              │                                                                 │  Bayer→YUY2
                              │                                                                 ▼
                              │                                        Global\PSCam4Win{N}.FrameBus + .Control (shared memory)
                              │                                                                 │
                              │                                                                 ▼
PS3 Eye (Composite Device) ───┤                                                            PSCam4Win.dll (in Camera Frame Server)
                              │                                                                 │  YUY2 native / NV12 (on-the-fly)
                              │                                                                 ▼
                              │                                                  "PS3 Eye" virtual camera in every app
                              │
                              └── Interface 1 (MI_01) ── usbaudio.sys ──► "USB Camera-B4.09.24.1" (Microphone) in every app

(The PS2 EyeToy follows the same right-hand path: its JPEG iso stream is reassembled and
 decoded to YUY2 in PSCam4WinTray.exe, then published to its own Global\PSCam4Win{N}.FrameBus.
 Its microphone is also automatically driver-mapped via usbaudio.sys to "Logitech EyeToy USB Camera".)
```

---

## What It Does For You

* **Connect Up to 8 Cameras:** Use up to 8 PlayStation cameras (any mix of PS3 Eye, PS2 EyeToy, and PS4 Camera) at the same time — note a PS4 camera in **split** mode claims two of the eight slots (Left + Right). The virtual cameras appear in your apps automatically only when a physical camera is plugged in, named after whichever device occupies the slot, and disappear when unplugged so you never see clutter.
* **Smart Sleep & Wake:** The physical cameras automatically power down (turning off the red LED and using 0% CPU) when not in use by any app, and wake up instantly when needed.
* **Integrated Microphone Support:** Works seamlessly with the built-in microphones of both devices: the PS3 Eye's 4-channel microphone array (**"USB Camera-B4.09.24.1"**) and the PS2 EyeToy microphone (**"Logitech EyeToy USB Camera"**) are both automatically exposed as standard Windows recording devices.
* **Quick Control from System Tray:** Adjust camera settings easily via a menu right next to your Windows clock.
* **Auto-Saved Settings:** Your adjustments for mirroring, gain, exposure, and white balance are automatically remembered for each camera.
* **Silent Windows Startup:** Launches quietly when you log in without showing any annoying security prompts.
* **No Complicated Setup:** You don't need to manually configure drivers or download third-party tools like Zadig—the installer does everything for you.

---

## How to Install & Uninstall

### Installation
1. Download **`PSCam4Win-Setup.exe`** from the [latest release](../../releases/latest) — release builds are Authenticode-signed by **SignPath Foundation**. (Building from source? `build.bat` produces the same installer at `build\PSCam4Win-Setup.exe`, just unsigned.)
2. Double-click it and accept the elevation prompt. The setup wizard will:
   * Let you choose which camera drivers to install (PS3 Eye / PS2 EyeToy / PS4 Camera) and whether to start at logon.
   * Upgrade any older **PS3EyeVCam** install in place (migrating your saved per-camera settings, then removing the old app, task, and registration).
   * Copy files to `C:\Program Files\PSCam4Win` (required for system camera integration).
   * Install the selected WinUSB video drivers. Because their INF catalogs are self-signed, setup adds each package's certificate (subject `CN=PSCam4Win Driver Signing (autogenerated)`) to the machine's **Trusted Root Certification Authorities** and **Trusted Publishers** stores — Windows will not accept a self-signed driver package otherwise. The signing keys are destroyed at package-build time and never leave the build machine, and **uninstalling removes the certificates again**.
   * Register the Virtual Camera DLL (handling all 8 camera CLSIDs).
   * Create an Apps & Features entry, set up the silent logon task, and launch the tray controller.
3. Every step is shown live in the wizard and written to `C:\ProgramData\PSCam4Win\install.log`. If anything fails, setup **rolls back automatically** — no half-installed leftovers.

Running the installer again offers **Reinstall / repair** and **Uninstall**.

### Uninstallation
1. Use **Windows Settings → Apps → Installed apps → PSCam4Win Virtual Camera → Uninstall** (or run `PSCam4Win-Setup.exe` again and pick Uninstall).
2. It stops the services, deletes the scheduled task, unregisters all CLSIDs, removes the video drivers and their certificates, deletes the Apps & Features entry, and removes all installed files.
3. Your saved camera settings and the PS4 firmware cache are **kept by default** (so a reinstall picks them right back up) — tick *"Also remove saved camera settings and the PS4 firmware cache"* in the uninstaller for a scorched-earth removal.

---

## Using the Camera and Settings

### Accessing Settings
Once installed, look for the **PS3 Eye camera icon** in your Windows System Tray (near the clock).
<img width="401" height="174" alt="SystemTray" src="https://github.com/user-attachments/assets/694ab46e-8f88-4642-b00e-b44de9beb11a" />

* **Right-click the tray icon** to:
  * Open **Settings** (or double-click the icon).
  * Toggle **Start with Windows** on/off.
  * Exit the tray application.
* **Settings Dialog**:
  * Choose which camera to configure using the **Camera dropdown selector** (independent settings are loaded and persisted per camera).
  * Adjust **Gain**, **Exposure**, **Brightness**, **Contrast**, **Saturation**, **Sharpness**, **White balance**, **Red/Blue/Green Balance** manually via sliders in real time. Only the sliders the selected camera actually backs are shown — a PS3 Eye, an EyeToy and a PS4 Camera each present a different set.
  * Toggle **Auto Gain & Exposure** and **Auto White Balance**.
  * Enable **Test Pattern** output for debugging or virtual camera validation.
  * Set **Video Presets** (e.g., standard 640×480 @ 60fps up to 75fps, or high-speed 320×240 up to 187fps).
  * Enable **Mirroring** (horizontal or vertical flip).
<img width="376" height="420" alt="camera settings (1)" src="https://github.com/user-attachments/assets/71843b34-0c2c-494c-b3e3-68eac5a2a482" />


*Note: If a program is currently streaming video from the selected camera, mode/preset changes for that camera are queued and will automatically apply as soon as you close/restart the stream.*

### PlayStation 4 Camera
The PS4 Camera is a stereo camera — setup is fully automatic (the installer handles its firmware), but a few things are worth knowing:

* **Firmware.** Unlike the PS3/PS2 cameras, the PS4 camera holds no firmware of its own — a small firmware blob must be uploaded over USB on every plug-in. For licensing reasons PSCam4Win does **not** ship Sony's firmware; instead **the setup exe downloads it automatically** during installation into `C:\ProgramData\PSCam4Win\`. Exactly one build is supported: the **final OV580 firmware** (byte-identical across PS4 system 6.00–7.02 — the last one Sony shipped), fetched from the community [`OrbisEyeCam`](https://github.com/psxdev/OrbisEyeCam) project (with [`PS4-CAMERA-DRIVERS`](https://github.com/Hackinside/PS4-CAMERA-DRIVERS) as a byte-identical mirror). It was hardware-validated against the older community blob and is strictly better: same clean 60 fps colour stereo, **and the camera survives being closed and reopened**, so tray or PC restarts never need a replug. The download is pinned to a known-good SHA-1 — bytes that don't match are discarded, an outdated cached copy is replaced on upgrade, and the app re-verifies the same SHA-1 before every USB upload. **Offline / manual install:** place a verified `firmware.bin` next to `PSCam4Win-Setup.exe` before running it, or drop it straight into `C:\ProgramData\PSCam4Win\` (anything that isn't the supported build is refused). (`startup.bin`, fetched the same way, is only needed for the optional RAW diagnostic mode below.) The PS3 Eye and PS2 EyeToy work with no such step.
* **Image quality.** The camera streams full-colour, auto-exposed video using the PS4's in-camera image processor (YUYV ISP) — both eyes are captured every frame, with no extra CPU cost. (Advanced/diagnostic: set the environment variable `PS4_RAW=1` to fall back to the software Bayer-demosaic path, which needs `startup.bin`.)
* **A single switchable camera.** As one camera it is simply named **"PS4 Camera"**; the eye you pick is an internal setting, not part of the name.
* **View selection.** In the Settings dialog, a PS4 camera shows a **PS4 view** dropdown — pick **Left eye**, **Right eye**, or **Side-by-side** (both eyes in one wide frame). Switching between **Left and Right is instant and seamless** while an app is open (both are 1280×800). Switching **to or from Side-by-side changes the resolution** (1280×800 ↔ 2560×800), so — like any webcam resolution change — the camera briefly drops and reappears and the app re-selects it. Side-by-side is a very wide 3.2∶1 frame, so apps will letterbox it.
* **Split into two cameras.** Tick **Split into two cameras** to expose the PS4 as two independent virtual cameras at once — **"PS4 Camera (Left)"** and **"PS4 Camera (Right)"** — sharing the single USB stream. (This uses two of the eight camera slots.)
* **Camera controls (brightness, contrast, gain, white balance).** The PS4 camera's image processor keeps the **shutter** on automatic — that's hardware behaviour, not a software choice: manual exposure-time writes are accepted on the wire and then cancelled by the ISP's auto-exposure loop, so no exposure slider is offered. Everything else it advertises was measured on real hardware and the ones that actually move the image are exposed: **Gain** (an always-live trim that darkens or lifts the picture *without* clipping — the best control for matching the PS4's level to a PS3 Eye's), **Brightness** (a strong luma offset; at minimum the image goes genuinely dark, ideal for **PS Move / light-gun tracking**, but pushed high it will clip highlights), **Contrast** (a real tone curve), and **Auto white balance** with a manual white-balance slider when it's off. **Saturation** and **sharpness** are live too (sharpness is worth turning *down* — on this sensor it amplifies grain almost as much as detail), and a **mains anti-flicker** selector picks 50/60 Hz — set it to match your region if you see banding under artificial light. Only **hue** is advertised by the chip but measured inert, so it isn't shown. Flip, view selection, and split are software features and always work.
* **Use with the shadPS4 emulator.** shadPS4 opens a standard webcam at **1280×800**, so point its camera setting at the **PS4 Camera (Left eye)** (or Right) view — not Side-by-side. shadPS4 builds the PS4's stereo/raw frames from that feed itself.
* **When a replug is needed.** The PS4 camera's firmware lives in volatile memory and is reloaded automatically on every physical connection — exactly as on a real PS4; PSCam4Win handles the upload invisibly. **A replug is essentially never needed**: reopening the camera is clean (hardware-verified), so view switching, opening/closing apps, and even tray or PC restarts all just work. (If the camera ever does get into a bad state — e.g. it was last driven by an old firmware build — PSCam4Win tells you: a **Windows notification** plus *"PS4 camera needs a replug"* in the Settings dialog.)
* **Microphone.** The PS4 camera's 4-microphone array is not USB audio — it is embedded inside the video stream — so Windows cannot see it as a normal recording device. PSCam4Win decodes it anyway: the Settings dialog's microphone block shows **four live level meters**, a **Mic gain** slider and a **Record** button that writes a 4-channel WAV to your Videos folder. The file is tagged with the rate actually measured during the recording — near 48,023 Hz rather than a round 48,000, because the camera runs at 60.029 fps — so it plays back at the right speed. The camera's own firmware leaves the array very quiet, so the gain slider is a real analog gain on the microphone ADC — about 40 dB of range, defaulting to +14 dB over the camera's fixed value. Turn it up if the camera is across the room; turn it down if you are sitting close to it. Note that raising it buys **level, not clarity**: the array is limited by room noise reaching the capsules, not by the converter, so more gain lifts the room along with your voice. The camera has to be streaming for audio to exist (the mics ride in the video frames), so the meters appear once something is using the camera — the settings preview is enough. Expect **far-field quality**: this is a room-scale array in a plastic bar, and at across-the-room distance it records about as much room as voice. Measured against an unprocessed capture of the PS3 Eye's own array, the two are within 0.1 dB of each other, so that character is the hardware rather than this software.

* **Using the PS4 microphone in other apps.** Windows will not let any program create a recording device without a kernel-mode audio driver, and PSCam4Win ships none — so instead of inventing a microphone, it **plays the array into an output device you choose**. Pick one under **Settings > Send to:**. Every entry in that list says what it will actually get you: an ordinary output is marked *"monitor only"* (you hear the array, nothing else can record it), while a **virtual audio cable** is marked with the name other apps must select. If no cable is installed the line underneath says so and links to [VB-CABLE](https://vb-audio.com/Cable/), which is free — install it, choose **CABLE Input** here, and pick **CABLE Output** as the microphone in Discord, Teams, OBS, browsers, RPCS3 or PCSX2. Once routing is live that same line reports what is really playing, so a device that failed to open cannot look healthy. Routing keeps the camera awake on its own, so the microphone works with no video app open — that is what **Keep mic awake when no app is using the camera** controls, and it is on by default. Untick it and the array still works, but only while something else has the camera open; the camera then sleeps (LED off, no USB bandwidth) the moment nothing needs it. Two things worth knowing: the audio is a **mono mix** of the four capsules, and letting the receiving app apply its own **noise suppression** is the single biggest quality win available here — that is exactly why this feature exists.

* **Video modes.** Nine, all verified on real hardware: **1280x800** at 60/30/15 fps (sensor-native stereo), **640x400** at 120/60/30 fps, and **320x192** at 240/120/60 fps — the high-speed modes are genuinely useful for tracking and light-gun work. Side-by-side views offer the same nine at double width. Pick one in the Settings dialog like any other camera.

* **Hardware limits (what the PS4 camera can't do here).** Three things are **not** offered, by hardware/OS constraint (verified on real hardware): **manual exposure time** (the shutter is permanently auto — see *Camera controls* above); the **status LED** is not controllable in this streaming mode; and the **microphone array** does not appear as a Windows recording device *of its own* — creating one needs a kernel-mode audio driver, out of scope for this driver-free, Memory-Integrity-safe design. This is the weakest of the three limits: the mics work, and **Send to:** routes them into any output device, so with a virtual audio cable installed (PSCam4Win links to one) they reach every app anyway (see *Microphone* above). PSCam4Win itself installs no audio driver.

### Setting up with Emulators (e.g., RPCS3)
1. Ensure the tray app is running (check your system tray).
2. Open **RPCS3** and go to **Settings** > **I/O**.
3. Set **Camera Handler** to **Qt** and select **PS3 Eye (Windows Virtual Camera)** as the device.
4. Ensure camera access is allowed under Windows Settings (*Privacy & security* > *Camera* > *Allow desktop apps to access your camera*).
5. For microphone support in RPCS3 or PCSX2, go to the emulator's audio settings, configure the microphone handler (e.g., **USBD** or standard Windows audio input), and select the appropriate recording device (**USB Camera-B4.09.24.1** for PS3 Eye, or **Logitech EyeToy USB Camera** for PS2 EyeToy).

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Camera missing in apps | Check if the tray icon is present and has a green status. |
| Tooltip says "camera not detected" | Try unplugging and replugging the USB cable. The app will automatically find the camera once connected. |
| Apps show black screen | The camera takes ~1 second to wake up from sleep mode. Wait a moment; if it stays black, ensure the tray app is running. |
| Notification / status says "PS4 camera needs a replug" | The PS4 camera's firmware session was lost (usually because the tray app or PC restarted). Unplug the PS4 camera and plug it back in — it reloads firmware and recovers automatically. |
| Settings status shows registration fail | Go to Windows Settings > Privacy & security > Camera and ensure "Allow desktop apps to access your camera" is enabled. |
| Camera's microphone records nothing | Windows gives the "default" recording device to whichever audio device was plugged in most recently, so plugging in a second camera silently demotes the first — the demoted mic still works, but apps left on **Default** are listening to something else. Either pick the mic explicitly in the app (**USB Camera-B4.09.24.1** for PS3 Eye, **Logitech EyeToy USB Camera** for PS2 EyeToy) or set it as the default input under Windows Settings > System > Sound. |

---

## Performance, Latency & Resources

* **Ultra-Low Latency:** Delivers video frames instantly to your apps with no noticeable delay or lag.
* **High Framerates:** PS3 Eye — 60 FPS (up to 75 FPS) at 640×480, and high-speed modes up to 187 FPS at 320×240. PS4 Camera — 60 FPS at 1280×800 stereo, 120 FPS at 640×400, and 240 FPS at 320×192.
* **Minimal CPU Impact:** Highly optimized video processing uses negligible CPU, even in high-speed modes.
* **Zero Idle Resource Usage:** When no apps are using the camera, the background system uses 0% CPU and puts the camera into low-power mode.

---

## Architecture & Technical Notes (For Developers)

### Multi-Camera Routing
* The host daemon spawns an independent thread for each physical camera `i` (0 to 7) mapped dynamically. 
* Each thread registers a virtual camera using the static class ID `CLSID_PS3EyeVCams[i]` (GUIDs retained across the rebrand to avoid COM re-registration), and communicates over custom slot-specific IPC channels: `Global\PSCam4Win[i].FrameBus` and `Global\PSCam4Win[i].Control`. The friendly name shown to apps is chosen per slot from the occupying device's profile.
* The virtual camera registers (`IMFVirtualCamera`) dynamically *only* when a physical camera is connected to a slot, and unregisters it on removal. This ensures client applications see exactly the number of cameras physically plugged in. Camera arrival and removal are event-driven via WinUSB device interface notifications fanned out to per-slot controllers (with a 5-second polling fallback for empty slots).

### High-Performance IPC (FrameBus)
* **Sub-Millisecond Latency via Auto-Reset Events:** Video frames are passed from the capture thread to the virtual camera DLL via a lock-free shared-memory queue (`FrameBus`). The reader waits on a per-camera auto-reset event (`Global\PSCam4Win[N].FrameReady`) with restricted ACLs rather than polling or sleeping, avoiding the 15.6 ms system timer quantum. This allows high-speed modes (100–187 fps) to achieve their full target framerate with sub-millisecond latency.
* **Sleep/Wake Coordination:** Sleep/wake state is coordinated via a shared keepalive timestamp (`common\ControlBus.h`). If the virtual camera DLL stops requesting frames, the tray app puts the hardware to sleep after ~3 seconds.

### On-the-Fly Image Processing & Color Conversion
* **Fused Debayering Pipeline:** To minimize CPU and memory footprint, `ps3eye.cpp` uses a fused single-pass Bayer-to-YUY2 debayering pipeline that outputs directly to BT.601 YUY2 via a 1.9 KB cache-resident row scratch buffer, completely bypassing the need for a 1.2 MB BGRA intermediate buffer.
* **Native Original Format, On-Demand Conversion:** The shared-memory frame buffer carries `YUY2` (YUYV 4:2:2) — the PS3 Eye's original output format — preserving the sensor's full vertical chroma resolution end to end. A client requesting `YUY2` (such as the RPCS3 emulator) receives the camera frame byte-for-byte with no conversion at all; clients using the modern `NV12` format get a proper chroma-averaged 4:2:2 → 4:2:0 conversion performed on the fly at delivery time.
* **On-Demand Scaling:** When the camera preset and the application's requested resolution mismatch (e.g., camera is capturing at 320x240 but Discord requests 640x480), the DLL scales 2x on the fly: bilinear interpolation when upscaling and a 2x2 box filter when downscaling, both in integer math in well under a millisecond (negligible CPU usage even at 187 FPS). No scaling is performed when the formats match.
* **Optimized Bandwidth:** In high-speed 320x240 modes, raw data is sent across the memory bus at just 29 MB/s. Scaling occurs within the client process on-demand, saving system memory bandwidth.

### USB Bandwidth & Device Configuration
* **USB Bandwidth Bottlenecks:** Each PS3 Eye camera requires ~185 Mbps of USB bandwidth at 640x480 @ 60 FPS. Although modern PCs use USB 3.x (xHCI) ports, the PS3 Eye is a USB 2.0 High-Speed device and is restricted to the USB 2.0 protocol layer, which shares a 480 Mbps bandwidth pool on the controller's High-Speed bus. A single physical controller can therefore support at most 2 cameras before saturating the bus. To run 3 to 8 cameras concurrently, you must distribute the cameras across separate USB controllers (e.g., separating them between rear motherboard ports, front panel ports, or dedicated PCIe USB expansion cards).
* **Composite Device Mapping:** Both the PS3 Eye and the PS2 EyeToy are composite USB devices. For both cameras, Interface 0 (`MI_00`) handles video streaming (via WinUSB and the tray app), while Interface 1 (`MI_01`) is automatically handled by the standard Windows USB Audio driver (`usbaudio.sys`) to expose their respective microphones.

### System Integration & OS Details
* **Security & Permissions:** The virtual camera media source DLL (`PSCam4Win.dll`) runs inside the **Camera Frame Server service** (`LOCAL SERVICE`). The tray app requires admin rights to create IPC objects in the `Global\` kernel namespace.
* **Startup Task:** The startup task uses the `ITaskService` COM API instead of `schtasks.exe` to bypass battery limits and execution duration limits.

### The Installer (`installer/`)
* `PSCam4Win-Setup.exe` is a dependency-free Win32 app built by `build.bat` alongside the other binaries. The entire payload (DLL, tray app, driver packages, co-installers, license) is embedded as **uncompressed RCDATA resources** and extracted with **SHA-256 read-back verification** — a single file is the whole distribution.
* Installation is a sequence of do/undo steps with **automatic rollback**: services (native SCM calls), COM registration (`DllRegisterServer` in-process for real HRESULTs), certificate stores (crypt32), driver staging (`pnputil`, whose staged/reboot exit codes are preserved), scheduled task, and the Apps & Features entry. Recursive deletes are restricted to an exact-match whitelist of the install directories.
* The same exe is the **uninstaller** (`--uninstall`); when launched from inside `C:\Program Files\PSCam4Win` it restages itself from `%TEMP%` so it can overwrite or delete its own directory.

---

## License

This project is licensed under the **GNU General Public License v2.0** (GPLv2) - see the [LICENSE](LICENSE) file for details.

### Third-Party Components & Licenses:
* **PS3EYEDriver wrapper** (`third_party/ps3eye/`): Ported from the [inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver) library, which is derived from the Linux Kernel `gspca_ov534` driver and licensed under the **GNU General Public License v2.0**.
* **libusb**: Used for low-level USB communications, licensed under the **GNU Lesser General Public License v2.1** (LGPLv2.1) or later.
