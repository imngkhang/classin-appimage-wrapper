#!/usr/bin/env python3

import os
import subprocess
import sys
import time
import uuid

import dbus
import dbus.mainloop.glib
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

DISPLAY2 = sys.argv[1] if len(sys.argv) > 1 else ":99"


def make_iface(bus, name):
    portal = bus.get_object(name, "/org/freedesktop/portal/desktop")
    return dbus.Interface(portal, "org.freedesktop.portal.ScreenCast")


def run():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    iface = make_iface(bus, "org.freedesktop.portal.Desktop")
    # New portal identity per share attempt; never reuse a prior session token.
    token = "classin_screenbridge_%s" % uuid.uuid4().hex
    state = {
        "phase": "session",
        "session": None,
        "node_id": None,
        "denied": False,
        "request": None,
    }

    def on_response(response, results, request_path=None):
        try:
            # Portal emits responses on per-call Request objects. Ignore stale
            # or duplicate signals so one user action cannot open two dialogs.
            if state["request"] is not None and str(request_path) != str(state["request"]):
                return
            if response != 0:
                print("portal response %s, denying" % response, file=sys.stderr)
                state["denied"] = True
                state["loop"].quit()
                return
            if state["phase"] == "session":
                for k, v in results.items():
                    if str(k) == "session_handle":
                        state["session"] = dbus.ObjectPath(str(v))
                        state["phase"] = "sources"
                        print("session handle: %s" % state["session"], file=sys.stderr)
                        break
            elif state["phase"] == "sources":
                state["phase"] = "start"
            elif state["phase"] == "start":
                for k, v in results.items():
                    if str(k) == "streams":
                        streams = v
                        if not streams:
                            print("no streams in result", file=sys.stderr)
                            state["denied"] = True
                            state["loop"].quit()
                            return
                        node_id, second = streams[0]
                        state["node_id"] = int(node_id)
                        print("got pipewire node %d" % state["node_id"], file=sys.stderr)
                        state["loop"].quit()
                        return
        except Exception as e:
            print("response error: %s" % e, file=sys.stderr)
            state["loop"].quit()

    bus.add_signal_receiver(
        on_response,
        signal_name="Response",
        dbus_interface="org.freedesktop.portal.Request",
        path_keyword="request_path",
    )

    loop = GLib.MainLoop()
    state["loop"] = loop

    def wait_phase(target, seconds):
        ctx = GLib.MainContext.default()
        waited = 0
        while not state["denied"] and state["phase"] != target and waited < seconds * 50:
            ctx.iteration(False)
            time.sleep(0.02)
            waited += 1
        return state["phase"] == target

    def create_session():
        state["request"] = iface.CreateSession(
            {
                "handle_token": token,
                "session_handle_token": token + "_session",
                "persist_mode": dbus.UInt32(0),
            }
        )

    try:
        create_session()
        if not wait_phase("sources", 15):
            print("timed out waiting for session", file=sys.stderr)
            sys.exit(1)
    except Exception as e:
        print("portal attempt failed: %s" % e, file=sys.stderr)
        sys.exit(1)

    try:
        state["request"] = iface.SelectSources(
            state["session"],
            {
                "types": dbus.UInt32(1),
                "multiple": False,
                # Embedded cursor is supported by KDE portal; metadata mode
                # was rendered twice by the Xvfb/XCB path.
                "cursor_mode": dbus.UInt32(1),
            },
        )
        if not wait_phase("start", 120):
            print("timed out or denied waiting for source selection", file=sys.stderr)
            sys.exit(1)
        state["request"] = iface.Start(
            state["session"], "", {"handle_token": token + "_start"}
        )
        loop.run()
    except Exception as e:
        print("start failed: %s" % e, file=sys.stderr)
        sys.exit(1)

    if state["denied"] or state.get("node_id") is None:
        print("portal denied or no streams", file=sys.stderr)
        sys.exit(1)

    cmd = [
        "gst-launch-1.0",
        "pipewiresrc",
        "path=%d" % state["node_id"],
        "!",
        "videoconvert",
        "!",
        "video/x-raw,format=BGRx",
        "!",
        "queue",
        "max-size-buffers=1",
        "leaky=downstream",
        "!",
        "ximagesink",
        "display=%s" % DISPLAY2,
        "force-aspect-ratio=false",
        "sync=false",
    ]
    os.environ["DISPLAY"] = DISPLAY2
    proc = subprocess.Popen(cmd)
    print("gst pid %d" % proc.pid, file=sys.stderr)
    proc.wait()
    try:
        session_iface = dbus.Interface(
            bus.get_object("org.freedesktop.portal.Desktop", state["session"]),
            "org.freedesktop.portal.Session",
        )
        session_iface.Close()
        ctx = GLib.MainContext.default()
        for _ in range(50):
            if ctx.pending():
                ctx.iteration(False)
            time.sleep(0.02)
        print("portal session closed", file=sys.stderr)
    except Exception as e:
        print("close session failed: %s" % e, file=sys.stderr)


if __name__ == "__main__":
    run()
