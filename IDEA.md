# LockingGlass Idea

LockingGlass is meant to fix a frustrating gap in how virtual desktops work on Windows for people using multiple monitors. On macOS, when you swipe between desktops, the desktop change can apply only to the monitor your cursor is currently on, which lets you keep the rest of your screens in the same context. On Windows, switching desktops changes all monitors at once, which makes it much harder to preserve a stable setup across multiple displays.

The goal of LockingGlass is to let the user lock individual monitors so those monitors stay fixed while other monitors continue changing when the user switches desktops. For example, someone might want to keep chat, reference material, or music pinned on one display while cycling workspaces on another.

This should work as a lightweight background executable on Windows that starts automatically with the system. It should live in the system tray so it is always available without getting in the way. When the user clicks the tray icon, the app should show the available monitor numbers or identities. Hovering a monitor in the menu should make it obvious which physical display is being referenced, such as by showing a temporary overlay, outline, label, or dimming effect on that monitor.

The user should be able to click any monitor to toggle its lock state. The UI should clearly show whether a monitor is locked or unlocked, for example with a padlock indicator. It should support locking none, one, some, or all monitors depending on the setup.

Once configured, when the user changes virtual desktops on Windows, whether by keyboard shortcuts or other normal methods, locked monitors should remain on their current desktop while unlocked monitors can continue following the desktop switch. The core value is preserving multi-monitor context instead of forcing every screen to move together every time the desktop changes.
