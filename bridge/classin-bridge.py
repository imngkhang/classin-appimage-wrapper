#!/usr/bin/env python3

import glob
import os
import re
import signal
import subprocess
import sys
import threading
import time

DISPLAY2 = os.environ.get("CLASSIN_BRIDGE_DISPLAY", ":99")
REQ = os.environ.get("CLASSIN_PROXY_REQUEST", "")
REG = os.environ.get("CLASSIN_PROXY_REGION", "")
HB = os.environ.get("CLASSIN_PROXY_HEARTBEAT", "")
STOP = os.environ.get("CLASSIN_PROXY_STOP", "")
TOKEN = os.environ.get("CLASSIN_BRIDGE_TOKEN", "")
HERE = os.path.dirname(os.path.realpath(__file__))
# End capture quickly so the next share request creates a fresh portal session.
COOLDOWN = 0
STALE = 3

xvfb_p = None
py_p = None
gst_pid = 0
granted = False
last_place = None
bridge_geom = (0, 0, 1920, 1080)
saw_hb = False


def log(msg):
    print("classin-bridge: %s" % msg, flush=True)


def screen_res():
    try:
        out = subprocess.check_output(["xrandr", "--current"], stderr=subprocess.DEVNULL, text=True)
        m = re.search(r"current (\d+) x (\d+)", out)
        if m:
            return int(m.group(1)), int(m.group(2))
    except (OSError, subprocess.CalledProcessError):
        pass
    return 1920, 1080


def app_alive():
    if not TOKEN:
        return True
    me = os.getpid()
    for p in glob.glob("/proc/[0-9]*/environ"):
        try:
            pid = int(p.split("/")[2])
        except ValueError:
            continue
        if pid == me:
            continue
        try:
            with open(p, "rb") as f:
                if TOKEN.encode() in f.read():
                    return True
        except OSError:
            pass
    return False


def active():
    return (xvfb_p and xvfb_p.poll() is None) or (py_p and py_p.poll() is None)


def stop():
    global granted, gst_pid, xvfb_p, py_p, last_place, saw_hb
    if gst_pid and py_p and py_p.poll() is None:
        try:
            os.kill(gst_pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        for _ in range(50):
            if py_p.poll() is not None:
                break
            time.sleep(0.1)
    for p in (py_p, xvfb_p):
        if p and p.poll() is None:
            try:
                if getattr(p, "grp", False):
                    os.killpg(p.pid, signal.SIGTERM)
                else:
                    p.terminate()
            except ProcessLookupError:
                pass
            try:
                p.wait(timeout=5)
            except Exception:
                pass
    granted = False
    gst_pid = 0
    xvfb_p = py_p = None
    last_place = None
    saw_hb = False


def read_region():
    try:
        with open(REG) as f:
            vals = [int(v) for v in f.read().split()]
        if len(vals) == 4:
            return tuple(vals)
    except (OSError, ValueError):
        pass
    return None


def hb_age():
    try:
        return time.time() - os.stat(HB).st_mtime
    except OSError:
        return None


def place(geom):
    global last_place
    if geom == last_place:
        return
    last_place = geom
    x, y, w, h = geom
    try:
        env = dict(os.environ, DISPLAY=DISPLAY2)
        env.pop("CLASSIN_BRIDGE_TOKEN", None)
        subprocess.run(
            ["sh", "-c",
             'xdotool search "" 2>/dev/null | while read win; do xdotool windowsize $win %d %d windowmove $win %d %d; done' % (w, h, x, y)],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def bridge():
    global xvfb_p, py_p, gst_pid, granted, bridge_geom, saw_hb
    granted = False
    gst_pid = 0
    saw_hb = False
    for f in (REG, HB, STOP):
        try:
            os.remove(f)
        except OSError:
            pass
    w, h = screen_res()
    bridge_geom = (0, 0, w, h)
    env0 = dict(os.environ)
    env0.pop("CLASSIN_BRIDGE_TOKEN", None)
    xvfb_p = subprocess.Popen(["Xvfb", DISPLAY2, "-screen", "0", "%dx%dx24" % (w, h)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env0)
    time.sleep(0.8)
    if xvfb_p.poll() is not None:
        log("Xvfb died right after start")
        stop()
        return
    env = dict(os.environ)
    env.pop("CLASSIN_BRIDGE_TOKEN", None)
    py_p = subprocess.Popen([sys.executable, os.path.join(HERE, "screenbridge.py"), DISPLAY2],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True, env=env)
    py_p.grp = True

    def reader():
        global granted, gst_pid
        for line in py_p.stdout:
            line = line.strip()
            log(line)
            m = re.search(r"gst pid (\d+)", line)
            if m:
                gst_pid = int(m.group(1))
            if "got pipewire node" in line:
                granted = True

    threading.Thread(target=reader, daemon=True).start()


def main():
    global saw_hb, bridge_geom
    if os.environ.get("XDG_SESSION_TYPE") != "wayland":
        return
    last_auto = 0.0
    while True:
        if not app_alive():
            stop()
            return
        if STOP and os.path.exists(STOP):
            try:
                os.remove(STOP)
            except OSError:
                pass
            stop()
        if os.path.exists(REQ):
            try:
                os.remove(REQ)
            except OSError:
                pass
            if not active() and time.time() - last_auto > COOLDOWN:
                last_auto = time.time()
                log("share request detected, starting bridge")
                bridge()
        if py_p and py_p.poll() is not None:
            if not granted:
                log("screenbridge exited (denied or failed)")
            stop()
        elif active() and granted:
            place(read_region() or bridge_geom)
            age = hb_age()
            if age is not None:
                saw_hb = True
            if saw_hb and (age is None or age > STALE):
                log("capture heartbeat stale, stopping bridge")
                stop()
        time.sleep(0.2)


if __name__ == "__main__":
    main()
