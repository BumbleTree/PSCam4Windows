# Public release signing

SignPath approval and a production certificate are external prerequisites, not
provided by this repository. Configure `SIGNPATH_API_TOKEN` (secret) and
`SIGNPATH_ORG_ID` (variable), project `pscam4win`, policy `release-signing`.
Have SignPath approve catalog signing for these INF-only inbox WinUSB packages.

Artifact configurations (ZIP entries at the root):

* `driver-catalogs`: `usb_device.cat`, `eyetoy_device.cat`, `ps4cam_device.cat`,
  each a `catalog-file` with `authenticode-sign` (replace the development signature).
* `payload-binaries`: `PSCam4Win.dll`, `PSCam4WinTray.exe`, each a `pe-file`
  with `authenticode-sign` and the Foundation-required product metadata restrictions.
* `setup-exe`: `PSCam4Win-Setup.exe`, likewise a `pe-file`.

Use SHA-256 and trusted timestamping. Configure trusted GitHub build origin,
restrict release tags and signing credentials, and require release approval.
Do not upload private signing keys to GitHub; SignPath performs signing remotely.

The tag workflow signs and verifies catalogs, checks INF membership, exports
matching public certificates, builds and signs the payload, then rebuilds and
signs setup. Failures stop publication. `PSCAM_PUBLIC_RELEASE=1` compiles out
certificate-store additions. Normal local builds retain development certificates
and their disclosed trust-store changes; they are not public release artifacts.

Before the first public release, validate installation and removal on a clean
Windows 11 VM with Secure Boot and Memory Integrity enabled, no development
certificates, and all three camera families. Authenticode validation alone does
not prove driver installation policy acceptance. Check publisher prompts and
confirm Root and TrustedPublisher stores are unchanged. Do not claim this gate
has passed until the production-signed installer has actually been tested.

Signing identifies the publisher and detects modification; it does not guarantee
SmartScreen reputation or warning-free installation.
