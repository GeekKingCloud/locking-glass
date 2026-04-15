# LockingGlass

Initial scaffold for a Windows-resident tray utility that will keep selected monitors pinned while other displays continue following Windows virtual desktop changes.

## Layout

- `include/locking_glass/core`: runtime assembly and startup diagnostics.
- `include/locking_glass/platform`: monitor-facing abstractions and Win32 monitor enumeration seam.
- `include/locking_glass/integration`: FFmpeg, Windows API, and Windows autostart probes.
- `src`: application entrypoint and default adapter implementations.
- `tests`: smoke coverage plus a fake FFmpeg shared library used to verify the loader path.

## Build

```bash
make
make test
make smoke
```

`make test` injects `build/lib/libfakeavutil.so` through `LOCKING_GLASS_FFMPEG_LIBRARY` so the FFmpeg seam is verified without requiring system FFmpeg packages.

## Autostart

- `locking_glass --install-autostart` writes a `LockingGlass` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- The Run entry launches the binary with `--background`, which is wired to a hidden Win32 message loop on Windows so the process can stay resident after sign-in without foreground UI.
- `locking_glass --self-check` prints the exact autostart command that the Windows registration path will install, which keeps the contract host-verifiable from Linux workers.

## Windows Notes

- `src/platform/monitor_gateway.cpp` is where Win32 monitor enumeration is wired with `EnumDisplayMonitors` and `GetMonitorInfoW`.
- `src/integration/windows_api_probe.cpp` is where tray and monitor entry points are probed with `user32.dll`, `shell32.dll`, and COM initialization.
- `src/platform/background_session.cpp` is where background launches enter a hidden Win32 message loop.
- `src/integration/autostart.cpp` is where current-user Run-key registration is built and installed for sign-in autostart.
- `src/integration/ffmpeg_probe.cpp` uses runtime loading instead of static linkage so future Windows packaging can decide where FFmpeg DLLs live without changing the call site contract.
