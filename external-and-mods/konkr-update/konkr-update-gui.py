#!/usr/bin/env python3
import json
import subprocess
import threading
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

UPDATER = '/usr/share/konkr-update/konkr-update.py'


class Window(Gtk.Window):
    def __init__(self):
        super().__init__(title='SteamOS Update')
        self.set_default_size(560, 380)
        self.set_border_width(20)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=14); self.add(box)
        label = Gtk.Label(label='Update pb-os while keeping your games, saves and login.\nThe update installs on the next restart and keeps a recovery backup.')
        label.set_line_wrap(True); label.set_xalign(0); box.pack_start(label, False, False, 0)
        self.status = Gtk.Label(label='Looking for updates…')
        self.status.set_line_wrap(True); self.status.set_selectable(True); self.status.set_xalign(0)
        box.pack_start(self.status, False, False, 0)
        self.progress = Gtk.ProgressBar(); self.progress.set_show_text(True); self.progress.set_no_show_all(True)
        box.pack_start(self.progress, False, False, 0)
        self.download = Gtk.Button(label='Download and install'); self.download.set_sensitive(False)
        self.download.connect('clicked', self.start_download)
        box.pack_start(self.download, False, False, 0)

        # Offline: a package downloaded elsewhere.
        offline = Gtk.Expander(label='Install from a file'); box.pack_start(offline, False, False, 0)
        inner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10); inner.set_margin_top(8); offline.add(inner)
        self.file = Gtk.FileChooserButton(title='Choose an update package')
        f = Gtk.FileFilter(); f.set_name('pb-os update package'); f.add_pattern('*.tar.gz'); self.file.add_filter(f)
        inner.pack_start(self.file, False, False, 0)
        self.sha = Gtk.Entry(); self.sha.set_placeholder_text('SHA-256 checksum from the release (SHA256SUMS)')
        inner.pack_start(self.sha, False, False, 0)
        self.prepare = Gtk.Button(label='Prepare update'); self.prepare.connect('clicked', self.start_file)
        inner.pack_start(self.prepare, False, False, 0)

        self.reboot = Gtk.Button(label='Restart and install'); self.reboot.set_sensitive(False)
        self.reboot.connect('clicked', lambda _: subprocess.run(['systemctl', 'reboot'], check=False))
        box.pack_end(self.reboot, False, False, 0)
        threading.Thread(target=self.check, daemon=True).start()

    def check(self):
        p = subprocess.run(['/usr/bin/python3', UPDATER, 'check'], capture_output=True, text=True)
        GLib.idle_add(self.checked, p)

    def checked(self, p):
        if p.returncode != 0:
            self.status.set_text('Could not look for updates (no internet?).\n' + (p.stderr or p.stdout).strip()[-300:])
            return False
        info = json.loads(p.stdout); up = info['update']; current = info['current'] or 'unknown'
        if not up:
            self.status.set_text(f'No update for this device in the pb-os releases yet. Installed: {current}.')
        elif not info['available']:
            self.status.set_text(f'pb-os is up to date ({current}).')
        else:
            self.status.set_text(f'{up["title"]} is available ({up["size"] / 1e9:.1f} GB download). Installed: {current}.\n'
                                 f'Release notes: {up["page"]}')
            self.download.set_sensitive(True)
        return False

    def busy(self, on):
        for w in (self.download, self.prepare, self.file, self.sha): w.set_sensitive(not on)

    def start_download(self, _):
        self.run(['update'], 'Starting the download…')

    def start_file(self, _):
        package, sha = self.file.get_filename(), self.sha.get_text().strip()
        if not package or len(sha) != 64:
            self.status.set_text('Choose a package and enter its release checksum.'); return
        self.run(['stage', package, '--sha256', sha], 'Checking and preparing the update. This can take several minutes.')

    def run(self, args, text):
        self.busy(True); self.status.set_text(text)
        def worker():
            p = subprocess.Popen(['pkexec', '/usr/bin/python3', UPDATER, *args], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, bufsize=1)
            last = []
            for line in p.stdout:
                last = (last + [line.strip()])[-20:]
                GLib.idle_add(self.line, line.strip())
            GLib.idle_add(self.finished, p.wait(), last)
        threading.Thread(target=worker, daemon=True).start()

    def line(self, line):
        if line.startswith('PROGRESS '):
            have, total = map(int, line.split()[1:3])
            self.progress.show(); self.progress.set_fraction(have / total)
            self.progress.set_text(f'{have / 1e9:.2f} of {total / 1e9:.2f} GB')
        elif line.startswith('STEP '):
            self.status.set_text(line[5:] + '…')
            if not line.startswith('STEP Downloading'): self.progress.hide()
        return False

    def finished(self, rc, last):
        self.progress.hide()
        if rc == 0 and any(l.startswith('Update staged') for l in last):
            self.status.set_text('Ready to restart. Keep the device charged and leave it on during the update; the screen can stay dark for several minutes. Games and saves will be kept.')
            self.reboot.set_sensitive(True)
        elif rc == 0:
            self.status.set_text(last[-1] if last else 'Nothing to do.'); self.busy(False)
        else:
            error = next((l for l in reversed(last) if l.startswith('ERROR:')), last[-1] if last else '')
            self.status.set_text(error.removeprefix('ERROR: ') or 'The update could not be prepared.')
            self.busy(False)
        return False


w = Window(); w.connect('destroy', Gtk.main_quit); w.show_all(); Gtk.main()
