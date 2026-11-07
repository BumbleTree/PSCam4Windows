<div align="center">

<img src="Github%20Assets/logo.png" width="104" alt="PSCam4Win">

# PSCam4Win

### Your PlayStation cameras. As ordinary webcams.

Plug in a **PS3 Eye**, a **PS2 EyeToy** or a **PS4 Camera**, and it shows up in Discord, OBS, Zoom, your browser and your emulators like any other webcam.

No kernel drivers, and no Zadig.

[![Download](https://img.shields.io/github/v/release/BumbleTree/PSCam4Windows?style=for-the-badge&label=Download&color=2f81f7)](https://github.com/BumbleTree/PSCam4Windows/releases/latest)
&nbsp;
![Windows 11](https://img.shields.io/badge/Windows%2011-only-444?style=for-the-badge)
&nbsp;
![GPLv2](https://img.shields.io/badge/license-GPLv2-444?style=for-the-badge)

<br>

<img src="Github%20Assets/settings-ps3-eye.png" width="820" alt="The PSCam4Win settings window with a live PS3 Eye preview">

</div>

<br>

## What it does

PSCam4Win runs as a tray application. It reads the camera over USB and publishes the video to Windows as a standard camera, so any program that can open a webcam can open it. There is no per-application plugin and no capture software to run alongside it.

- **Automatic detection.** A camera that is plugged in appears as a webcam, and unplugging it removes it again. Nothing needs to be started or selected.
- **Up to 8 cameras.** Any mix of supported models. Each one has its own name and its own saved settings.
- **Idle power down.** When no program is using a camera, it is put into low-power mode (LED off) and uses no CPU. Waking takes about a second.
- **Microphones.** The PS3 Eye and PS2 EyeToy microphones appear as standard Windows recording devices. The PS4 camera's 4-microphone array works differently, as described below.
- **Optional start at logon.** Launched by a scheduled task, so there is no console window and no UAC prompt.
- **No kernel drivers.** Capture uses Microsoft's in-box WinUSB driver and the video is published through Media Foundation. Memory Integrity / Core Isolation can stay enabled.

<br>

## The cameras

<div align="center">
<img src="Github%20Assets/cameras-compared.png" width="900" alt="The same scene through a PS2 EyeToy, a PS3 Eye and a PS4 Camera">
<sub><i>The same shelf, same evening, through all three.</i></sub>
</div>

<br>

| Camera | Resolutions | Top speed | Microphone |
|---|---|---|---|
| **PS3 Eye** | 640 × 480 · 320 × 240 | **187 fps** at 320 × 240, 75 fps at 640 × 480 | 4-channel array, as a Windows input |
| **PS2 EyeToy** | 640 × 480 · 320 × 240 | 30 fps | Mono, as a Windows input |
| **PS4 Camera** | 1280 × 800 · 640 × 400 · 320 × 192 (×2 wide in stereo) | **240 fps** at 320 × 192, 60 fps at full size | 4-mic array, routed to an output you pick |

> **The PS4 Camera needs a USB 3.0 port** (a blue one, or anything faster). It streams roughly **275 MB/s** of stereo video, which is several times what USB 2.0 can carry, so on a 2.0 port it will not run. The PS3 Eye and the PS2 EyeToy are USB 2.0 devices and work on any port.

> **PS5 camera support is planned.** It is not here yet.

<br>

## Install

1. **[Download `PSCam4Win-Setup.exe`](https://github.com/BumbleTree/PSCam4Windows/releases/latest)** and run it.
2. Tick the cameras you own, and whether it should start with Windows.
3. Click through, then plug a camera in and open Discord.

The wizard does everything: installs the USB drivers for the cameras you picked, registers the virtual cameras, sets up the startup task and starts the tray app. The PS4 camera's firmware is fetched automatically during setup.

Every step is shown as it happens. **If anything fails, setup rolls itself back**, so there is no half-installed state to clean up.

<details>
<summary><b>A note on the driver warning</b></summary>

<br>

The camera drivers are signed with a certificate this project generates itself, because there is no paid signing service behind it. Windows won't load a self-signed driver unless it trusts the certificate, so setup adds it to the machine's trusted stores. The wizard tells you before it does, and **uninstalling removes it again**, along with every driver PSCam4Win ever installed.

</details>

<details>
<summary><b>Uninstalling</b></summary>

<br>

**Settings → Apps → Installed apps → PSCam4Win Virtual Camera → Uninstall** (or run the setup exe again and pick Uninstall).

It removes the app, the drivers, the certificates, the startup task and the virtual cameras. Your per-camera settings and the PS4 firmware are **kept** so a reinstall picks up where you left off. Tick *"Also remove saved camera settings and the PS4 firmware cache"* if you want it all gone.

</details>

<br>

## Using it

PSCam4Win lives in the system tray, next to the clock.

<div align="center">
<img src="Github%20Assets/tray-menu.png" width="300" alt="The tray menu, listing every connected camera">
</div>

**Right-click** for a quick menu, where every connected camera gets its own submenu for resolution and frame rate. **Double-click** (or pick *Settings…*) for the full window.

### The settings window

<div align="center">
<img src="Github%20Assets/settings-ps2-eyetoy.png" width="760" alt="Settings for a PS2 EyeToy">
</div>

Pick a camera from the chips along the top. Everything is **live**: drag a slider and the preview moves with it. Changes are saved per camera, automatically.

You only ever see controls the selected camera actually has. A PS3 Eye offers gain, exposure and manual white balance; an EyeToy offers brightness and saturation; a PS4 Camera offers a different set again. Controls the camera does not support are not displayed.

> If an app is streaming from the camera right now, a **resolution or frame-rate** change waits until that app lets go. Everything else applies instantly.

### Light or dark

It follows your Windows theme, or you can pin it.

<div align="center">
<img src="Github%20Assets/settings-ps3-eye-light.png" width="700" alt="The same window in light theme">
</div>

<br>

## The PS4 Camera

The PS4 Camera is a **stereo** camera, with two sensors in one bar, so it gets a few extra options. Setup handles its firmware for you; there is nothing to download or install by hand.

**Plug it into a USB 3.0 port.** Two 1280 × 800 sensors at 60 fps is about 275 MB/s, which USB 2.0 cannot carry.

<div align="center">
<img src="Github%20Assets/settings-ps4-camera.png" width="760" alt="Settings for a PS4 Camera, with live microphone meters">
</div>

### Pick an eye, or take both

<div align="center">
<img src="Github%20Assets/ps4-view-detail.png" width="520" alt="The View dropdown offering Left eye, Right eye and Side-by-side">
</div>

**Left eye** or **Right eye** gives you an ordinary 1280 × 800 webcam. Switching between the two is instant, even mid-call.

**Side-by-side** gives you the camera's **raw stereo output**: both eyes in one 2560 × 800 frame, still at 60 fps. Use it if you want the depth information, or if whatever you are feeding expects the camera's native stereo picture.

<div align="center">
<img src="Github%20Assets/ps4-side-by-side-detail.png" width="520" alt="Side-by-side view: one 2560x800 frame containing both eyes">
</div>

### Or split it into two separate webcams

Tick **Split into two cameras** and the one bar becomes **two independent webcams**, *PS4 Camera (Left)* and *PS4 Camera (Right)*, running at the same time off the single USB stream. Point one app at the left eye and another at the right.

<div align="center">
<img src="Github%20Assets/ps4-split-chips.png" width="760" alt="Split mode: the PS4 now appears as two separate cameras">
</div>

Both halves are the same sensor pair behind one image processor, so the picture controls and the capture mode are set once, on the **Left** camera. Select the Right one and it says so, instead of showing dead sliders:

<div align="center">
<img src="Github%20Assets/ps4-split-shared-detail.png" width="500" alt="The Right half of a split pair: shared controls greyed out, with an explanation">
</div>

> Split mode uses **two** of the eight camera slots.

### The 4-microphone array

The PS4 Camera's microphones are not USB audio. They ride *inside* the video stream, so Windows cannot see them as a recording device on its own, and PSCam4Win ships no audio driver to add one.

Instead, **Send to:** plays the array into any output device you choose. Point it at a **virtual audio cable** and the array becomes a microphone in every application: Discord, Teams, OBS, your browser, RPCS3, PCSX2. The settings window links you to [VB-CABLE](https://vb-audio.com/Cable/) (free) if you don't have one, and tells you exactly what each output in the list will and won't get you.

You also get four live level meters, a **Mic gain** slider (real analog gain, about 40 dB of range), and a **Record** button that writes a 4-channel WAV to your Videos folder.

Note that this is a room-scale array in a plastic bar a metre or two away, so expect it to sound like a room. Letting the receiving application apply its own noise suppression is the largest available quality improvement, and is the main reason this routing exists.

<br>

## With emulators and apps

**Any app** (Discord, Zoom, Teams, OBS, browsers): pick the camera by name. Nothing else to do.

**RPCS3**: Settings → I/O → Camera Handler **Qt**, then select **PS3 Eye**. For the microphone, set the mic handler in the audio settings and pick **USB Camera-B4.09.24.1**.

**PCSX2**: pick **PS2 EyeToy** as the camera, and **Logitech EyeToy USB Camera** as the microphone.

**shadPS4**: it opens a standard 1280 × 800 webcam, so set the PS4 Camera's view to **Left eye** (or Right), *not* side-by-side. shadPS4 builds the stereo frames itself.

> First time only: make sure **Settings → Privacy & security → Camera → Allow desktop apps to access your camera** is on.

<br>

## If something's wrong

| What you see | What to do |
|---|---|
| The camera isn't in any app | Check the tray icon is there. Then check camera access is allowed under Privacy & security → Camera. |
| Tooltip says *camera not detected* | Unplug and replug the USB cable. It'll be picked up on its own. |
| Black picture for a moment | It's waking up, which takes about a second. If it stays black, make sure the tray app is running. |
| *"PS4 camera needs a replug"* | Unplug the PS4 camera and plug it back in. It reloads its firmware and recovers by itself. |
| The PS4 camera never starts, or drops frames badly | It's in a USB 2.0 port. It needs **USB 3.0**. Try a blue socket, preferably one on the back of the PC rather than a hub. |
| A camera's mic records silence | Windows hands "Default" to whatever was plugged in most recently, so a second camera quietly demotes the first. Pick the mic by name in the app, or set it as default under Settings → System → Sound. |
| Three or more cameras stutter | Each camera needs real USB bandwidth. Two per USB controller is the practical limit, so spread them between front and rear ports. |

<br>

## Honest limits

A few things are the hardware, not the software, and no update will change them:

- **The PS4 camera's shutter is always automatic.** It accepts manual exposure and then quietly overrides it, so there's no exposure slider. **Gain** is the honest brightness trim instead.
- **The PS4 camera's light never comes on** while streaming in this mode.
- **The PS4 microphone array cannot be a Windows recording device of its own.** That needs a kernel audio driver, and shipping one would undo the point of this project. *Send to:* plus a virtual cable gets you there anyway.

<br>

## Build it yourself

Everything is in [**README-TECHNICAL.md**](README-TECHNICAL.md): how to build, how the pipeline fits together, the USB and performance details, and what was measured on real hardware.

<br>

## License

**GNU General Public License v2.0**. See [LICENSE](LICENSE).

Built on [PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver) (GPLv2, derived from the Linux `gspca_ov534` driver), [libusb](https://libusb.info/) (LGPLv2.1) and [libjpeg-turbo](https://libjpeg-turbo.org/). PS4 firmware is fetched from the community [OrbisEyeCam](https://github.com/psxdev/OrbisEyeCam) project. None of Sony's code is redistributed here. Full notices in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

<div align="center">
<br>
<sub>Not affiliated with Sony Interactive Entertainment. PlayStation is a trademark of its respective owner.</sub>
</div>
