#!/usr/bin/env python3
import subprocess
import threading
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

class Window(Gtk.Window):
    def __init__(self):
        super().__init__(title='SteamOS Update')
        self.set_default_size(560, 330)
        self.set_border_width(20)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14); self.add(box)
        label = Gtk.Label(label='Update SteamOS while keeping your games, saves and login.\nThe update runs on the next restart and keeps a recovery backup.')
        label.set_line_wrap(True); label.set_xalign(0); box.pack_start(label, False, False, 0)
        self.file = Gtk.FileChooserButton(title='Choose an official update package')
        f = Gtk.FileFilter(); f.set_name('SteamOS update package'); f.add_pattern('*.tar.gz'); self.file.add_filter(f)
        box.pack_start(self.file, False, False, 0)
        self.sha = Gtk.Entry(); self.sha.set_placeholder_text('SHA-256 checksum from the official release')
        box.pack_start(self.sha, False, False, 0)
        self.prepare = Gtk.Button(label='Prepare update'); self.prepare.connect('clicked', self.start)
        box.pack_start(self.prepare, False, False, 0)
        self.status = Gtk.Label(label='Download the package for this device and its checksum from github.com/project-barry/pb-os (Releases).')
        self.status.set_line_wrap(True); self.status.set_selectable(True); self.status.set_xalign(0)
        box.pack_start(self.status, True, True, 0)
        self.reboot = Gtk.Button(label='Restart and install'); self.reboot.set_sensitive(False)
        self.reboot.connect('clicked', lambda _: subprocess.run(['systemctl', 'reboot'], check=False))
        box.pack_start(self.reboot, False, False, 0)

    def start(self, _):
        package, sha = self.file.get_filename(), self.sha.get_text().strip()
        if not package or len(sha) != 64:
            self.status.set_text('Choose a package and enter its release checksum.'); return
        self.prepare.set_sensitive(False); self.file.set_sensitive(False); self.sha.set_sensitive(False)
        self.status.set_text('Checking and preparing the update. This can take several minutes.')
        def worker():
            p = subprocess.run(['pkexec', '/usr/bin/python3', '/usr/share/konkr-update/konkr-update.py',
                                'stage', package, '--sha256', sha], capture_output=True, text=True)
            GLib.idle_add(self.finished, p)
        threading.Thread(target=worker, daemon=True).start()

    def finished(self, p):
        if p.returncode == 0:
            self.status.set_text('Ready to restart. Keep the device charged and leave it on during the update. Games and saves will be kept.')
            self.reboot.set_sensitive(True)
        else:
            self.status.set_text((p.stderr or p.stdout).strip()[-1000:] or 'The update could not be prepared.')
            self.prepare.set_sensitive(True); self.file.set_sensitive(True); self.sha.set_sensitive(True)
        return False

w = Window(); w.connect('destroy', Gtk.main_quit); w.show_all(); Gtk.main()
