# Changelog

## v1.0.0

- Added live Windows monitor pinning backed by `VirtualDesktopAccessor.dll`.
- Added a helper-owned `Locking Glass` holding desktop for destination-workspace windows while a monitor is locked.
- When unlocking, current movable top-level windows on the borrowed monitor return to the monitor's remembered original virtual desktop where Windows allows it.
- When visiting the `Locking Glass` holding desktop, parked workspace windows are restored to their own desktops so they do not start following the locked monitor.
- Added current-user installer startup, one-time runner packaging, release hygiene checks, icon contract checks, and package smoke tests.
- Public release downloads are `Locking Glass Installer.exe` and `Locking Glass.exe`.
