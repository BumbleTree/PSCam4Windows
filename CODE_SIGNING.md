# Code Signing Policy

Free code signing is provided by [SignPath.io](https://signpath.io/), with certificates issued by the [SignPath Foundation](https://signpath.org/).

## Overview

PSCam4Win signs its production releases to ensure software authenticity, protect against tampering, and provide Windows users with a verified publisher identity.

## Signing Architecture & Integrity

* **Automated CI/CD Builds**: All official release binaries and driver catalogs are built directly from public source code on GitHub Actions runners (`.github/workflows/release.yml`).
* **Hardware Security & Key Protection**: Private signing keys are never stored in this repository, on build runners, or on developer machines. Signing requests are submitted to SignPath via authenticated API tokens and processed remotely in secure hardware security modules (HSM).
* **Inside-Out Verification**:
  1. **Driver Catalogs**: `usb_device.cat`, `eyetoy_device.cat`, and `ps4cam_device.cat` are signed and verified with `signtool.exe` against the checked-out driver INF files.
  2. **Payload Binaries**: `PSCam4Win.dll` and `PSCam4WinTray.exe` are built and Authenticode-signed with trusted timestamping.
  3. **Setup Executable**: `PSCam4Win-Setup.exe` is relinked around the verified signed payload and signed as the final release artifact.
* **Certificate Store Safety**: Official public releases (`PSCAM_PUBLIC_RELEASE=1`) rely solely on standard Windows trust validation and never modify the user's `Trusted Root Certification Authorities` or `Trusted Publishers` system certificate stores.

## Release vs. Development Builds

* **Official Public Releases**: Built automatically on tagged commits (`v*`), signed via the SignPath Foundation, and published directly to [GitHub Releases](https://github.com/BumbleTree/PSCam4Windows/releases).
* **Local Development Builds**: Running `build.bat` locally produces development binaries that use local self-signed certificates for testing. These are disclosed by the development installer and are not intended for public redistribution.
