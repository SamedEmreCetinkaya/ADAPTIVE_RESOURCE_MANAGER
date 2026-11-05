// Sends the process ID of the focused window to the local daemon as one datagram.
import Gio from 'gi://Gio';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const FOCUS_SOCKET_PATH = '/run/adaptive-resource-manager/focus.sock';

export default class FocusReporterExtension extends Extension {
    enable() {
        this._socket = Gio.Socket.new(
            Gio.SocketFamily.UNIX, Gio.SocketType.DATAGRAM, Gio.SocketProtocol.DEFAULT);
        this._socket.set_blocking(false);
        this._address = Gio.UnixSocketAddress.new(FOCUS_SOCKET_PATH);
        this._encoder = new TextEncoder();
        this._lastPid = 0;
        this._focusHandler = global.display.connect('notify::focus-window', () => this._report());
        this._report();
    }

    disable() {
        global.display.disconnect(this._focusHandler);
        this._socket.close();
        this._focusHandler = null;
        this._socket = null;
        this._address = null;
        this._encoder = null;
    }

    _report() {
        const pid = global.display.focus_window?.get_pid() ?? 0;
        if (pid <= 0 || pid === this._lastPid)
            return;
        this._lastPid = pid;
        try {
            this._socket.send_to(this._address, this._encoder.encode(`${pid}\n`), null);
        } catch (error) {
            // The daemon may not be running. A later focus change reports again.
            this._lastPid = 0;
        }
    }
}
