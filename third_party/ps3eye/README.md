# Vendored PS3EYEDriver (forked)

This directory vendors the PS3 Eye capture driver used by the `transports/usb_bulk` transport.
Unlike the two prebuilt libraries beside it (`third_party/libusb`, `third_party/libjpeg-turbo`),
this one is **source, and it is a fork** — not a pristine upstream drop. Sixteen local changes are
applied on top of upstream, every one of them marked in place with a `PS3EyeVCam patch:` comment.

> **If you are about to "upgrade" this file:** you cannot simply replace it with upstream. Read the
> list of local changes below and re-apply them, or the multi-camera slot map, the fused YUY2
> debayer, and several shutdown-lifetime fixes all disappear silently.

## Contents

| Path | What |
|---|---|
| `ps3eye.cpp` | The driver: USB bring-up, OV534/OV7720 register tables, bulk transfer ring, debayer |
| `ps3eye.h` | Public `ps3eye::PS3EYECam` interface (the only header consumers need) |

Consumed by `transports/usb_bulk/Ps3EyeDevice.cpp` and by `host/DeviceRegistry.cpp`
(`PS3EYECam::getDevices`), and compiled by `build.bat` into `ps3eye.obj`.

## Upstream and licence

* Upstream: **[inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver)** — the attribution
  carried at the top of both files (`// source code from https://github.com/inspirit/PS3EYEDriver`).
* That project is itself derived from the Linux kernel's `gspca_ov534` driver, and is therefore
  **GPLv2**. PSCam4Win is GPLv2 for the same reason — see the repository `LICENSE`.
* **No upstream commit is pinned.** The vendored copy predates this record; if you re-sync, pin the
  commit here at the same time so the next person has a base to diff against.

Because this is a modified GPLv2 work, the changes below are stated here as well as in the source,
and each carries its `PS3EyeVCam patch:` marker at the point of change.

## Local modifications

Grep the source for `PS3EyeVCam patch:` to see all sixteen in context. By theme:

**Multi-camera slot mapping** — the reason this fork exists at all.
* `getDevices()` (line ~1256) was rewritten into a stable **8-slot map keyed by USB port path**,
  with `GetDevicePortPath()` added. Upstream returns a flat list in enumeration order; this project
  needs a slot index that survives replug, because the slot index selects the virtual-camera CLSID.
  Pointer identity distinguishes "still plugged in" from "replugged into the same port".
* Device open accepts only an exact **interface-0** match (line ~291); the old loose match could
  bind the wrong interface of the composite device.

**Fused YUY2 output** — the performance change.
* `EOutputFormat::YUY2` and `DebayerYUY2()` (line ~747) add a single-pass Bayer(GRBG)→YUY2 debayer
  with BT.601 limited-range packing, replacing the Bayer→BGRA→YUY2 double conversion. Removes a
  1.2 MB intermediate buffer from the hot path.

**Shutdown and lifetime correctness** — the fixes that make sleep/wake safe.
* Bounded, abort-aware `Dequeue` (line ~489) and an explicit wake of blocked consumers on stop
  (lines ~436, ~968), so a sleeping capture thread cannot hang on teardown.
* Frame-queue deletion deferred out of `close_transfers` (lines ~907, ~1013) — freeing it there
  raced the libusb event thread.
* Only *submitted* transfers are counted as in flight (lines ~944, ~1188), and a failed submission
  is no longer silently ignored (line ~1511) — upstream left the count wrong on both paths, which
  stalled teardown.
* `transfer_buffer` doubles as the "transfers were allocated" flag (line ~973).
* The constructor owns the libusb device reference (line ~1384), and a failed `claim_interface` no
  longer leaks the open handle (line ~1592).
* A mutex serialises camera-count transitions (line ~339).

**Build hygiene**
* The `#define snprintf _snprintf` shim is gone (line ~83) — it broke under the project's `/utf-8`
  MSVC settings and is unnecessary on the supported toolchain.

## A note on the "unmodifiable" wording

`transports/usb_iso/EyeToyUsb.h` and `transports/usb_ps4/Ps4Usb.h` explain why the EyeToy and PS4
transports each own a *separate* libusb context. That reasoning is correct — this driver calls
`libusb_init` itself and owns its own context and event thread, so no other transport can share it.
The word to read there is **"separate"**, not "unmodifiable": this file is very much modifiable, and
has been modified sixteen times.
