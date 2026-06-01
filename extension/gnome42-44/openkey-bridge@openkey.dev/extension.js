/* exported init enable disable */

const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const Shell = imports.gi.Shell;

const BUS_NAME = 'org.openkey.Bridge';
const OBJECT_PATH = '/org/openkey/Bridge';
const IFACE_XML = `
<node>
  <interface name="org.openkey.Bridge1">
    <method name="GetFocusedApp">
      <arg name="appId" type="s" direction="out"/>
      <arg name="appName" type="s" direction="out"/>
    </method>
    <signal name="FocusedAppChanged">
      <arg name="appId" type="s"/>
      <arg name="appName" type="s"/>
    </signal>
  </interface>
</node>`;

class BridgeService {
    constructor() {
        this._tracker = Shell.WindowTracker.get_default();
        this._lastAppId = '';
        this._lastAppName = '';

        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(IFACE_XML, this);
        this._dbusImpl.export(Gio.DBus.session, OBJECT_PATH);
        this._nameOwnerId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            BUS_NAME,
            Gio.BusNameOwnerFlags.NONE,
            null,
            null,
            null
        );

        this._focusSignalId = 0;
        this._pollSourceId = 0;

        try {
            this._focusSignalId = global.display.connect(
                'notify::focus-window',
                () => this._maybeEmitChanged()
            );
        } catch (e) {
            this._focusSignalId = 0;
        }

        // Fallback polling to cover cases where focus notifications are missed.
        this._pollSourceId = GLib.timeout_add(
            GLib.PRIORITY_DEFAULT,
            200,
            () => {
                this._maybeEmitChanged();
                return GLib.SOURCE_CONTINUE;
            }
        );

        this._maybeEmitChanged();
    }

    GetFocusedApp() {
        const [appId, appName] = this._getFocusedApp();
        return [appId, appName];
    }

    _getFocusedApp() {
        let app = this._tracker.focus_app;
        if (!app) {
            const win = global.display.get_focus_window?.() ?? null;
            if (win) {
                app = this._tracker.get_window_app(win);
            }
        }
        const appId = app?.get_id?.() ?? '';
        const appName = app?.get_name?.() ?? '';
        return [String(appId), String(appName)];
    }

    _maybeEmitChanged() {
        const [appId, appName] = this._getFocusedApp();
        if (appId === this._lastAppId && appName === this._lastAppName) {
            return;
        }
        this._lastAppId = appId;
        this._lastAppName = appName;
        try {
            this._dbusImpl.emit_signal(
                'FocusedAppChanged',
                new GLib.Variant('(ss)', [appId, appName])
            );
        } catch (e) {
            // Ignore signal failures when no clients are listening.
        }
    }

    destroy() {
        if (this._focusSignalId) {
            try {
                global.display.disconnect(this._focusSignalId);
            } catch (_) {}
            this._focusSignalId = 0;
        }
        if (this._pollSourceId) {
            GLib.source_remove(this._pollSourceId);
            this._pollSourceId = 0;
        }
        if (this._nameOwnerId) {
            Gio.bus_unown_name(this._nameOwnerId);
            this._nameOwnerId = 0;
        }
        if (this._dbusImpl) {
            try {
                this._dbusImpl.unexport();
            } catch (_) {}
            this._dbusImpl = null;
        }
    }
}

let _service = null;

function init() {}

function enable() {
    _service = new BridgeService();
}

function disable() {
    if (_service) {
        _service.destroy();
        _service = null;
    }
}
