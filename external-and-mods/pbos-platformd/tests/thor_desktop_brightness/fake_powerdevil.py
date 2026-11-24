#!/usr/bin/env python3
"""A fake PowerDevil brightness service (org.kde.ScreenBrightness) on the
session bus, for the thor-desktop-brightness equivalence test.

  fake_powerdevil.py CONFIG.json LOG

CONFIG: {"displays": [[name, max], ...],
         "bottom": the display whose SetBrightness reaches the bottom panel's
                   file (BOTTOM, on BOTTOM_MAX's scale), or null,
         "delay": seconds before that write, "steps": writes in an animation
                  (the last one the level asked for), "append": write without
                  truncating, as Steam's file may be written,
         "fail": every SetBrightness fails,
         "levels": {name: the Brightness it starts at} (else no such property)}
Each SetBrightness is logged (name level flags); org.test.Fake.Emit(s, i)
sends BrightnessChanged as a slider move would.
"""
from __future__ import annotations

import json
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

SERVICE = "org.kde.ScreenBrightness"
PATH = "/org/kde/ScreenBrightness"
DISPLAY = "org.kde.ScreenBrightness.Display"
PROPS = "org.freedesktop.DBus.Properties"
BOTTOM = "/sys/class/backlight/ae94000.dsi.0/brightness"
BOTTOM_MAX = "/sys/class/backlight/ae94000.dsi.0/max_brightness"

cfg = json.load(open(sys.argv[1]))
LOG = sys.argv[2]


def log(line: str) -> None:
    with open(LOG, "a") as fh:
        fh.write(line + "\n")


class Root(dbus.service.Object):
    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        if iface == SERVICE and prop == "DisplaysDBusNames":
            return dbus.Array([d[0] for d in cfg["displays"]], signature="s")
        raise dbus.exceptions.DBusException(f"no property {prop}", name="org.freedesktop.DBus.Error.InvalidArgs")

    @dbus.service.signal(SERVICE, signature="siss")
    def BrightnessChanged(self, name, value, client, context):
        pass

    @dbus.service.method("org.test.Fake", in_signature="si")
    def Emit(self, name, value):
        self.BrightnessChanged(name, value, "test", "")


class Display(dbus.service.Object):
    def __init__(self, bus, name, maximum, root):
        super().__init__(bus, f"{PATH}/{name}")
        self.name, self.max, self.root = name, maximum, root

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, prop):
        if iface == DISPLAY and prop == "MaxBrightness":
            return dbus.Int32(self.max)
        if iface == DISPLAY and prop == "Brightness" and self.name in cfg.get("levels", {}):
            return dbus.Int32(cfg["levels"][self.name])
        raise dbus.exceptions.DBusException(f"no property {prop}", name="org.freedesktop.DBus.Error.InvalidArgs")

    @dbus.service.method(DISPLAY, in_signature="iu")
    def SetBrightness(self, value, flags):
        log(f"SetBrightness {self.name} {int(value)} {int(flags)}")
        if cfg.get("fail"):
            raise dbus.exceptions.DBusException("no", name="org.kde.Test.Failed")
        if cfg.get("bottom") == self.name:
            try:
                bmax = int(open(BOTTOM_MAX).read())
            except (OSError, ValueError):
                bmax = self.max
            target = round(int(value) * bmax / self.max)
            steps = cfg.get("steps", 1)
            start = target // 2
            for i in range(1, steps + 1):
                level = target if i == steps else start + (target - start) * i // steps
                GLib.timeout_add(int(cfg.get("delay", 0) * 1000 * i / steps), self.write, level)
        self.root.BrightnessChanged(self.name, int(value), "powerdevil", "")

    def write(self, level):
        with open(BOTTOM, "a" if cfg.get("append") else "w") as fh:
            fh.write(f"{level}\n")
        return False


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
root = Root(bus, PATH)
keep = [Display(bus, n, m, root) for n, m in cfg["displays"]]
name = dbus.service.BusName(SERVICE, bus)
log("owner")
GLib.MainLoop().run()
