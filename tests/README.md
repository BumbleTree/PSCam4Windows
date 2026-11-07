# Software regression tests

From the repository root, after a full build:

```bat
build.bat
tests\build.bat
```

The tests build with the application's C++17, static CRT and warning settings.
They exercise the actual installer engine/primitives, FrameBus reader/writer,
Media Foundation source/activator, EyeToy decoder, PS4 frame/session helpers,
audio/recording helpers and hidden native UI widgets. They do not install cameras or
drivers, change trust stores, or access camera hardware.

- Installer fixtures cover partial writes, retained backups after failed
  restoration, registry tree snapshots, junction-safe deletion, process timeout
  and cancellation, large child output, and certificate ownership. Certificate
  calls are replaced by in-memory fault injection.
- FrameBus fixtures use unique Local object names, including a restricted-token
  write-denial check, malformed headers, geometry mismatches and concurrent
  capability updates.
- Source fixtures inject presentation-descriptor failure, retain a stream
  across parent shutdown, activate independent sources, and check black MJPEG
  fallback, restart cancellation, negotiated geometry and timestamps.
- JPEG fixtures encode valid grayscale/color inputs with the vendored library
  and run the actual decoder with output guards, plus delayed USB cancellation.
- Host fixtures check source/sink rate ratios, recording command generations,
  exclusive WAV creation and segmentation, PS4 ownership/coherent snapshots,
  delayed cancellation, per-slot audio-status permissions and exact SBS pixels.
  An offline SBS benchmark compares the reference copies with direct composition.
- UI fixtures check capture cancellation, theme-cache growth, DPI fonts and
  preview GDI allocation failure/retry without leaking handles.

Temporary file and HKCU registry fixtures are named uniquely and removed after
testing. Restricted Windows sandboxes can deny file moves, registry writes or
junction creation; run these tests in a normal Windows process for full coverage.
Unavailable registry/junction/restricted-token checks print SKIP. FAIL produces
a nonzero exit code.

To check the public installer policy, build its objects and the tests with the
same flag:

```bat
tests\build.bat public
```

Rebuild the installer with `build.bat installer` to return to development policy.
Hardware streaming and full install/upgrade/uninstall acceptance remain separate
checks; successful software fixtures do not establish hardware behavior.
