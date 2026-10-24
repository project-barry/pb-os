#!/usr/bin/env python3
"""Easy UFS Installer — GUI for installing Linux to internal UFS."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk, Pango

APP_NAME = "Easy UFS Installer"
APP_VERSION = "2.2.0"

SHARE = Path(os.environ.get("EASY_UFS_INSTALL_ROOT", "/usr/share/easy-ufs-install"))
INSTALL_SH = SHARE / "install-masios-to-internal.sh"
PROBE_SH = SHARE / "ufs-probe-sizes.sh"
DIAGNOSE_SH = SHARE / "ufs-diagnose.sh"

HOME_CHOICES = [
    ("all", "Everything, installed games included"),
    ("essentials", "Settings, saves and Steam login (no games)"),
    ("none", "Fresh start (no Steam login, saves or games)"),
]
# PB-OS over PB-OS: the internal home can stay as it is.
KEEP_CHOICE = ("keep", "Keep the internal home (its games, saves and Steam login)")
# What the probe found on internal storage -> the installer's --replace target.
OVER = {"installed": "pbos", "armada": "armada"}
OVER_NAME = {"pbos": "PB-OS", "armada": "ArmadaOS"}

# Installer log lines -> what the user sees, in order. The installer prints
# each as "[ufs] ..."; step 2 is the first one that changes internal storage.
STEPS = [
    (re.compile(r"\[ufs\] .*: internal disk"), "Checking the device and internal storage"),
    (re.compile(r"\[ufs\] repartitioning"), "Repartitioning internal storage"),
    (re.compile(r"\[ufs\] formatting"), "Formatting the new partitions"),
    (re.compile(r"\[ufs\] copying the system"), "Copying the system (several minutes)"),
    (re.compile(r"\[ufs\] copying .*/home"), "Copying the home folder"),
    (re.compile(r"\[ufs\] installing KERNEL"), "Installing the boot kernel"),
    (re.compile(r"\[ufs\] checking the result"), "Checking the result"),
]
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PERCENT_RE = re.compile(r"\s(\d{1,3})%\s")


def _root_argv(argv: list[str]) -> list[str]:
    if os.geteuid() == 0:
        return argv
    if shutil.which("pkexec"):
        return ["pkexec", *argv]
    return ["sudo", "--", *argv]


def _pkexec(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(_root_argv(argv), check=False, text=True,
                          capture_output=True, stdin=subprocess.DEVNULL)


def _run_streaming(argv: list[str], on_text) -> int:
    """Run as root; call on_text(text, is_progress) for each output line.

    rsync --info=progress2 redraws one line with carriage returns; those
    segments come through with is_progress=True instead of flooding the log.
    stdin is closed so an unexpected prompt fails instead of hanging.
    """
    proc = subprocess.Popen(_root_argv(argv), stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert proc.stdout is not None
    buf = b""
    while True:
        chunk = proc.stdout.read1(4096)
        if not chunk:
            break
        buf += chunk
        while True:
            i_n, i_r = buf.find(b"\n"), buf.find(b"\r")
            cands = [i for i in (i_n, i_r) if i >= 0]
            if not cands:
                break
            i = min(cands)
            seg = ANSI_RE.sub("", buf[:i].decode("utf-8", "replace"))
            buf = buf[i + 1:]
            on_text(seg, i == i_r)
    if buf:
        on_text(ANSI_RE.sub("", buf.decode("utf-8", "replace")), False)
    return proc.wait()


def probe_sizes() -> dict[str, str]:
    script = PROBE_SH if PROBE_SH.is_file() else Path(__file__).resolve().parent / "ufs-probe-sizes.sh"
    if not script.is_file():
        raise RuntimeError(f"Missing probe script: {script}")
    # Probe may need root for parted on some devices
    proc = _pkexec(["bash", str(script)])
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
        raise RuntimeError(err)
    out: dict[str, str] = {}
    for line in (proc.stdout or "").splitlines():
        if "=" in line and not line.startswith("ERROR="):
            k, v = line.split("=", 1)
            out[k] = v
        elif line.startswith("ERROR="):
            raise RuntimeError(line.split("=", 1)[1])
    if "MAX_ANDROID_GIB" not in out:
        raise RuntimeError("Probe did not return sizing info")
    return out


class MainWindow(Gtk.Window):
    def __init__(self) -> None:
        super().__init__(title=APP_NAME)
        self._fit_to_screen()
        self._busy = False
        self._info: dict[str, str] = {}

        # Everything scrolls, so the window fits small handheld screens
        # (the Pocket FIT's panel with desktop scaling is only ~700 px tall).
        outer = Gtk.ScrolledWindow()
        outer.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.add(outer)
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        root.set_border_width(14)
        outer.add(root)

        title = Gtk.Label()
        title.set_markup(f"<span size='x-large'><b>{APP_NAME}</b></span>")
        title.set_xalign(0)
        root.pack_start(title, False, False, 0)

        frame = Gtk.Frame(label="Read carefully")
        frame.set_shadow_type(Gtk.ShadowType.ETCHED_IN)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.set_border_width(10)
        # Red-ish emphasis via markup
        warn_m = Gtk.Label()
        warn_m.set_markup(
            "<span foreground='#b00020'><b>WARNING — THIS WILL MODIFY THE INTERNAL "
            "STORAGE OF YOUR DEVICE</b></span>\n\n"
            + GLib.markup_escape_text(
                "This tool repartitions internal UFS and installs SteamOS alongside Android "
                "(ROCKNIX boot + STORAGE root + HOME, started by OpenABL).\n\n"
                "• Android userdata will be ERASED (factory-reset style).\n"
                "• Already have PB-OS or ArmadaOS on internal storage? It goes over that "
                "instead, in the same space: Android keeps its size, apps and data, and "
                "only the old Linux install is replaced.\n"
                "• The old partition table is saved to the SD card first (/boot/ufs-backup), "
                "so the space can be given back to Android later.\n"
                "• Incorrect use MAY cause data loss or make Android/Linux unbootable.\n"
                "• Run this from microSD Linux, not from an already-installed UFS root.\n"
                "• Keep OpenABL installed and a working /boot/KERNEL on the SD.\n\n"
                "STEAMOS PASSWORD: official SteamOS ships with no user password. "
                "Create one later (passwd in Konsole, or Users in System Settings). "
                "This installer asks for that password (pkexec/sudo). "
                "Without it the internal UFS install cannot run.\n\n"
                "Nothing should go wrong if you follow the steps, but DATA LOSS IS POSSIBLE.\n"
                "Proceed only if you understand the risk."
            )
        )
        warn_m.set_xalign(0)
        warn_m.set_line_wrap(True)
        warn_m.set_selectable(True)
        box.pack_start(warn_m, False, False, 0)
        frame.add(box)
        root.pack_start(frame, False, False, 0)

        self.status = Gtk.Label(label="Click Refresh to probe internal UFS…")
        self.status.set_xalign(0)
        self.status.set_line_wrap(True)
        root.pack_start(self.status, False, False, 0)

        grid = Gtk.Grid(column_spacing=12, row_spacing=8)
        grid.attach(Gtk.Label(label="Android partition size (GB):", xalign=0), 0, 0, 1, 1)
        adj = Gtk.Adjustment(value=64, lower=16, upper=512, step_increment=1, page_increment=8)
        self.size_spin = Gtk.SpinButton(adjustment=adj, climb_rate=1, digits=0)
        self.size_spin.set_numeric(True)
        grid.attach(self.size_spin, 1, 0, 1, 1)
        self.linux_label = Gtk.Label(label="SteamOS partitions: —", xalign=0)
        self.linux_label.set_line_wrap(True)
        grid.attach(self.linux_label, 0, 1, 2, 1)
        grid.attach(Gtk.Label(label="Bring from the SD card's /home:", xalign=0), 0, 2, 1, 1)
        self.home_combo = Gtk.ComboBoxText()
        self._over = ""
        self._set_home_choices(keep=False)
        grid.attach(self.home_combo, 1, 2, 1, 1)
        root.pack_start(grid, False, False, 0)
        self.size_spin.connect("value-changed", lambda *_: self._update_linux_label())

        self.ack = Gtk.CheckButton(
            label="I understand this modifies internal UFS and may erase Android data"
        )
        root.pack_start(self.ack, False, False, 0)

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.refresh_btn = Gtk.Button(label="Refresh UFS info")
        self.refresh_btn.connect("clicked", lambda *_: self.refresh_async())
        self.install_btn = Gtk.Button(label="Install to internal UFS")
        self.install_btn.connect("clicked", self._on_install)
        buttons.pack_start(self.refresh_btn, False, False, 0)
        buttons.pack_start(self.install_btn, False, False, 0)
        buttons.pack_end(Gtk.Label(label=f"v{APP_VERSION}"), False, False, 0)
        root.pack_start(buttons, False, False, 0)

        self.step_label = Gtk.Label(xalign=0)
        self.step_label.set_line_wrap(True)
        root.pack_start(self.step_label, False, False, 0)
        self.progress = Gtk.ProgressBar()
        self.progress.set_show_text(True)
        self.progress.set_no_show_all(True)
        root.pack_start(self.progress, False, False, 0)

        self.log = Gtk.TextView()
        self.log.set_editable(False)
        self.log.set_monospace(True)
        self.log.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        scroll = Gtk.ScrolledWindow()
        scroll.set_min_content_height(280)
        scroll.set_vexpand(True)
        scroll.add(self.log)
        root.pack_start(scroll, True, True, 0)

        self._installing = False
        self._log_file = None
        self.connect("delete-event", self._on_delete)
        self.connect("destroy", Gtk.main_quit)
        self.refresh_async()

    def _set_home_choices(self, keep: bool) -> None:
        """The /home picks; Keep (the default then) only over PB-OS."""
        current = self.home_combo.get_active_id()
        self.home_combo.remove_all()
        for cid, label in ([KEEP_CHOICE] if keep else []) + HOME_CHOICES:
            self.home_combo.append(cid, label)
        if keep:
            self.home_combo.set_active_id("keep")
        elif not current or current == "keep" or not self.home_combo.set_active_id(current):
            self.home_combo.set_active_id("all")

    def _fit_to_screen(self) -> None:
        width, height = 640, 720
        display = Gdk.Display.get_default()
        monitor = (display.get_primary_monitor() or display.get_monitor(0)) if display else None
        if monitor is not None:
            area = monitor.get_workarea()
            width = min(width, int(area.width * 0.95))
            height = min(height, int(area.height * 0.95))
            if area.height < 800:
                self.maximize()
        self.set_default_size(width, height)

    def _append(self, text: str) -> None:
        buf = self.log.get_buffer()
        buf.insert(buf.get_end_iter(), text.rstrip() + "\n")
        mark = buf.create_mark(None, buf.get_end_iter(), False)
        self.log.scroll_mark_onscreen(mark)
        buf.delete_mark(mark)
        if self._log_file:
            self._log_file.write(text.rstrip() + "\n")
            self._log_file.flush()

    def _on_delete(self, *_args) -> bool:
        if not self._installing:
            return False
        self._message(Gtk.MessageType.WARNING, "Installation in progress",
                      "Closing now could leave internal storage half-installed. "
                      "Wait until the installer says it has finished.")
        return True  # keep the window open

    def _message(self, kind: Gtk.MessageType, title: str, body: str) -> None:
        dialog = Gtk.MessageDialog(transient_for=self, modal=True,
                                   message_type=kind,
                                   buttons=Gtk.ButtonsType.OK, text=title)
        dialog.format_secondary_text(body)
        dialog.run()
        dialog.destroy()

    def _update_linux_label(self) -> None:
        if not self._info:
            return
        if self._over:
            home = int(self._info.get("HOME_ROOM_BYTES", "0") or 0) / (1 << 30)
            self.linux_label.set_text(
                f"{OVER_NAME[self._over]} on internal storage: {self._info.get('LINUX_GIB')} GB  |  "
                f"after the install: ROCKNIX boot · STORAGE root · HOME ~{home:.0f} GB")
            return
        try:
            android = int(self.size_spin.get_value())
            boot = int(self._info.get("BOOT_PART_GIB", "2"))
            root = int(self._info.get("ROOT_PART_GIB", "16"))
            total = int(self._info.get("ORIG_ANDROID_GIB", "0"))
            home = total - android - boot - root
            self.linux_label.set_text(
                f"ROCKNIX boot {boot} GB  |  STORAGE root {root} GB  |  HOME ~{home} GB"
            )
        except ValueError:
            pass

    def refresh_async(self) -> None:
        if self._busy:
            return
        self._busy = True
        self.refresh_btn.set_sensitive(False)
        self.status.set_text("Probing internal UFS…")

        def worker() -> None:
            err = ""
            info: dict[str, str] = {}
            try:
                info = probe_sizes()
            except Exception as exc:  # noqa: BLE001
                err = str(exc)

            def done() -> None:
                self._busy = False
                self.refresh_btn.set_sensitive(True)
                if err:
                    self.status.set_text(f"Probe failed: {err}")
                    self._append(err)
                    return
                self._info = info
                fresh = info.get("MODE") == "fresh"
                from_sd = info.get("RUNNING_FROM_SD") == "1"
                self._over = OVER.get(info.get("MODE", ""), "")
                why = info.get("REPLACE_ERROR", "") if self._over else ""
                self.install_btn.set_sensitive(from_sd and (fresh or bool(self._over and not why)))
                self.install_btn.set_label(
                    f"Install over {OVER_NAME[self._over]}" if self._over else "Install to internal UFS")
                self.size_spin.set_sensitive(not self._over)
                self._set_home_choices(keep=self._over == "pbos")
                if not from_sd:
                    self._append("Running from internal storage: boot the SD card to install.")
                elif why:
                    self._append(f"{OVER_NAME[self._over]} is on internal storage, but this can't go over it: {why}.")
                elif info.get("MODE") == "occupied":
                    self._append(f"Other partitions follow userdata ({info.get('OCCUPIED')}). "
                                 "Remove them in the OpenABL menu (Uninstall CFW, which also erases Android's user data) first.")
                mn = int(info["MIN_ANDROID_GIB"])
                mx = int(info["MAX_ANDROID_GIB"])
                rec = int(info["RECOMMENDED_ANDROID_GIB"])
                self.size_spin.set_range(mn, mx)
                self.size_spin.set_value(rec)
                old_two = info.get("OLD_TWOPART_INSTALL", "0") == "1"
                extra = ""
                if self._over == "pbos":
                    extra = (" (PB-OS already installed: this replaces it and Android stays; "
                             "to only update it, use Software Update there)")
                elif self._over == "armada":
                    extra = " (ArmadaOS installed: this replaces it and Android stays)"
                elif old_two:
                    extra = " (old 2-partition ROCKNIX+STORAGE — OpenABL Uninstall CFW before a fresh SteamOS install)"
                self.status.set_text(
                    f"UFS {info.get('DEVICE')} · total ~{info.get('DISK_TOTAL_GIB')} GB · "
                    + (f"Android keeps its {info.get('USERDATA_GIB')} GB{extra}" if self._over else
                       f"userdata now ~{info.get('ORIG_ANDROID_GIB')} GB · "
                       f"Android size {mn}–{mx} GB (recommended {rec}){extra}")
                )
                self._update_linux_label()
                self._append(f"Probe OK: {info}")

            GLib.idle_add(done)

        threading.Thread(target=worker, daemon=True).start()

    def _on_install(self, *_args) -> None:
        if self._busy:
            return
        if not self.ack.get_active():
            self._append("Tick the confirmation checkbox first.")
            return
        script = INSTALL_SH if INSTALL_SH.is_file() else Path(__file__).resolve().parent / "install-masios-to-internal.sh"
        if not script.is_file():
            self._append(f"Missing installer: {script}")
            return
        android_gb = int(self.size_spin.get_value())
        root_gb = int(self._info.get("ROOT_PART_GIB", "16")) if self._info else 16
        boot_gb = int(self._info.get("BOOT_PART_GIB", "2")) if self._info else 2
        over = self._over
        home_mode = self.home_combo.get_active_id() or "all"
        if over == "armada":
            confirm = ("Everything on ArmadaOS (ARMADA, ARMADA_BOOT, ARMADA_ROOT) is ERASED.\n"
                       f"Its space becomes ROCKNIX ({boot_gb} GB boot) + STORAGE ({root_gb} GB root) + HOME (remaining).\n"
                       "Android and its data are not changed. Continue?")
        elif over == "pbos" and home_mode == "keep":
            confirm = ("The internal PB-OS system is replaced; its home (games, saves, Steam login) stays.\n"
                       "Android and its data are not changed. Continue?")
        elif over == "pbos":
            confirm = ("The internal PB-OS is ERASED, games, saves and Steam login included, "
                       "and replaced by this SD install.\nAndroid and its data are not changed. Continue?")
        else:
            confirm = (f"Android userdata will be set to {android_gb} GB and ERASED.\n"
                       f"Creates ROCKNIX ({boot_gb} GB boot) + STORAGE ({root_gb} GB root) + HOME (remaining).\n"
                       "Internal partitions will be rewritten. Continue?")
        dialog = Gtk.MessageDialog(
            transient_for=self,
            modal=True,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.OK_CANCEL,
            text="Confirm internal UFS install",
        )
        dialog.format_secondary_text(confirm)
        response = dialog.run()
        dialog.destroy()
        if response != Gtk.ResponseType.OK:
            return

        self._busy = True
        self._installing = True
        self.install_btn.set_sensitive(False)
        self.refresh_btn.set_sensitive(False)
        self.size_spin.set_sensitive(False)
        self.home_combo.set_sensitive(False)
        self.ack.set_sensitive(False)
        self.status.set_text(
            (f"Installing SteamOS over {OVER_NAME[over]} (Android is not changed). " if over else
             f"Installing SteamOS to internal storage (Android: {android_gb} GB). ")
            + "Don't power off or close this window.")

        fingerprint = self._info.get("TABLE_FINGERPRINT", "")
        # The table is about to change: the next install needs a fresh probe.
        self._info = {}

        log_path = Path.home() / f"easy-ufs-install-{time.strftime('%Y%m%d-%H%M%S')}.log"
        try:
            self._log_file = open(log_path, "w", encoding="utf-8")
        except OSError:
            self._log_file = None
        self._append(f"=== Install started {time.strftime('%Y-%m-%d %H:%M:%S')} "
                     f"(log: {log_path}) ===")
        self.progress.show()
        self.progress.set_fraction(0.0)
        started = time.monotonic()
        state = {"step": 0, "pct": 0, "error": ""}

        def show_step() -> None:
            n = state["step"]
            label = STEPS[n - 1][1] if n else "Starting (enter your password when asked)"
            mins, secs = divmod(int(time.monotonic() - started), 60)
            self.step_label.set_markup(
                f"<b>Step {max(n, 1)} of {len(STEPS)}:</b> {GLib.markup_escape_text(label)}"
                f"   <small>({mins}:{secs:02d} elapsed)</small>")
            # Each step is an equal slice; the copy steps fill theirs with rsync %.
            frac = (max(n, 1) - 1 + state["pct"] / 100) / len(STEPS)
            self.progress.set_fraction(min(frac, 1.0))
            self.progress.set_text(f"{int(frac * 100)}%")

        def tick() -> bool:
            if not self._installing:
                return False
            show_step()
            return True

        GLib.timeout_add_seconds(1, tick)
        show_step()

        def on_text(text: str, is_progress: bool) -> None:
            def ui() -> None:
                if is_progress:
                    m = PERCENT_RE.search(" " + text + " ")
                    if m:
                        state["pct"] = min(int(m.group(1)), 100)
                        show_step()
                    return
                for i, (pattern, _label) in enumerate(STEPS, start=1):
                    if i > state["step"] and pattern.search(text):
                        state["step"], state["pct"] = i, 0
                        show_step()
                if "ERROR:" in text:
                    state["error"] = text.split("ERROR:", 1)[1].strip()
                if text.strip():
                    self._append(text)
            GLib.idle_add(ui)

        def worker() -> None:
            if over:
                argv = ["bash", str(script), "--force", "--replace",
                        "--expect", fingerprint, "--home", home_mode]
            else:
                argv = ["bash", str(script), "--force", "--android-gb", str(android_gb),
                        "--expect", fingerprint, "--home", home_mode]
            try:
                rc = _run_streaming(argv, on_text)
            except OSError as exc:
                on_text(f"ERROR: could not start the installer: {exc}", False)
                rc = 127

            def done() -> None:
                self._busy = False
                self._installing = False
                # Refresh re-probes the new table; Install stays off until then.
                self.refresh_btn.set_sensitive(True)
                self.size_spin.set_sensitive(True)
                self.home_combo.set_sensitive(True)
                self.ack.set_sensitive(True)
                mins, secs = divmod(int(time.monotonic() - started), 60)
                self._append(f"=== Installer exited with code {rc} after {mins}:{secs:02d} ===")
                if self._log_file:
                    self._log_file.close()
                    self._log_file = None
                if rc == 0:
                    self.progress.set_fraction(1.0)
                    self.progress.set_text("Done")
                    self.step_label.set_markup("<b>Installation complete.</b>")
                    self.status.set_text("Install finished. Power off, remove the SD card, then in the OpenABL menu set Boot source to Internal (or Auto) and Start.")
                    self._message(
                        Gtk.MessageType.INFO, "SteamOS is installed on internal storage",
                        "Next:\n"
                        "1. Power off the device and remove the microSD card.\n"
                        "2. Hold Volume Down while powering on to open the OpenABL menu.\n"
                        "3. Set Boot source to Internal (or Auto), Boot mode to Linux, then Start.\n\n"
                        + ("Android: hold Volume Up while powering on. It is as it was.\n\n" if over else
                           "Android: hold Volume Up while powering on. It sets itself up again "
                           "because its user data was erased.\n\n")
                        + "Never pick Uninstall CFW in the OpenABL menu: it erases this install "
                        "and Android's user data.\n\n"
                        f"The full log is saved as {log_path}.")
                    return
                # Put a copy where it's easy to find and attach to a report.
                saved = log_path
                desktop = Path(GLib.get_user_special_dir(
                    GLib.UserDirectory.DIRECTORY_DESKTOP) or Path.home() / "Desktop")
                try:
                    desktop.mkdir(parents=True, exist_ok=True)
                    saved = desktop / f"easy-ufs-install-FAILED-{time.strftime('%Y%m%d-%H%M%S')}.log"
                    shutil.copyfile(log_path, saved)
                except OSError as exc:
                    self._append(f"Could not copy the log to the desktop: {exc}")
                    saved = log_path
                self.step_label.set_markup("<b>Installation failed.</b>")
                self.status.set_text(f"Install failed (exit {rc}). See the log below.")
                self._message(
                    Gtk.MessageType.ERROR, "Installation failed",
                    (f"{state['error']}\n\n" if state["error"] else "")
                    + ("The installer stopped before changing internal storage."
                       if state["step"] < 2 else
                       "Internal storage was already being changed, so the internal SteamOS "
                       "may not start until this is done again. Android and its data were "
                       "not changed. Refresh and install once more."
                       if over else
                       "Internal storage was already being changed, so it may be "
                       "partly installed. Android may not boot until this is fixed; "
                       "the OpenABL menu's Uninstall CFW removes the Linux partitions "
                       "(and Android's user data), "
                       "and the old partition table is saved on the SD card in "
                       "/boot/ufs-backup.")
                    + (f"\n\nThe full log is saved on the desktop as {saved.name}."
                       if saved != log_path else
                       f"\n\nThe full log is saved as {log_path}."))

            GLib.idle_add(done)

        threading.Thread(target=worker, daemon=True).start()


def main() -> int:
    win = MainWindow()
    win.show_all()
    Gtk.main()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
