OpenKey Focus Bridge (GNOME Shell Extension)

This GNOME Shell extension exposes the currently focused application over D-Bus,
so the OpenKey addon (or any other client) can query it.

Why two builds?
- GNOME Shell 45+ switched extensions to ESM modules and requires a different
  entrypoint than GNOME Shell 42–44. A single folder cannot support both
  runtimes, so this repo ships two variants.

**D-Bus**
- Bus name: `org.openkey.Bridge`
- Object path: `/org/openkey/Bridge`
- Interface: `org.openkey.Bridge1`
- Method: `GetFocusedApp() -> (appId, appName)`
- Signal: `FocusedAppChanged(appId, appName)`

Example:
- `gdbus call --session --dest org.openkey.Bridge --object-path /org/openkey/Bridge --method org.openkey.Bridge1.GetFocusedApp`

**Install**
- Run `extension/install.sh` to install the correct variant for your
  `gnome-shell` major version into `~/.local/share/gnome-shell/extensions/`.
- Then enable it:
  - `gnome-extensions enable openkey-bridge@openkey.dev`
  - (restart GNOME Shell / relogin if needed)

