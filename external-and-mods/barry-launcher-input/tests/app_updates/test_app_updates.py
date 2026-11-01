"""barry_updates (app updates after a PB-OS update) and barry_apps' "minApi":
the catalog, versions, the marker, release and file picks, the check, the
download's checks and the offer, against a fake GitHub (no network).

  python3 test_app_updates.py [sm8550-overlay]   (run.sh)
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OVERLAY = os.path.abspath(sys.argv.pop(1) if len(sys.argv) > 1 and os.path.isdir(sys.argv[1])
                          else os.path.join(HERE, "../../../../sm8550-overlay"))
sys.path.insert(0, os.path.join(OVERLAY, "usr/lib/barry_launcher"))
import barry_apps  # noqa: E402
import barry_updates as bu  # noqa: E402

SHIPPED = os.path.join(OVERLAY, "usr/share/barry_launcher/app-updates.json")
API = bu.API_URL
RAW = bu.RAW_URL


def app_zip(app_id: str, version: str, top: str = "app", **extra) -> bytes:
    """A small QML app's zip, as a release would have it."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(f"{top}/barry-app.json", json.dumps(dict(
            {"format": 1, "id": app_id, "name": "Test", "version": version, "main": "main.qml"}, **extra)))
        zf.writestr(f"{top}/main.qml", "import QtQuick\nItem {}\n")
    return buf.getvalue()


def release(tag: str, assets: dict[str, bytes], **kw) -> dict:
    return dict({"tag_name": tag, "draft": False, "prerelease": False,
                 "assets": [{"name": n, "size": len(b), "digest": "sha256:" + hashlib.sha256(b).hexdigest(),
                             "browser_download_url": f"https://github.com/x/y/releases/download/{tag}/{n}"}
                            for n, b in assets.items()]}, **kw)


class FakeGitHub:
    """urls -> JSON (None: 404; an exception: raised); files by URL."""

    def __init__(self, answers: dict | None = None, files: dict | None = None) -> None:
        self.answers = answers or {}
        self.files = files or {}
        self.asked: list[str] = []

    def get_json(self, url: str):
        self.asked.append(url)
        a = self.answers.get(url)
        if isinstance(a, Exception):
            raise a
        return a

    def download(self, url: str, dest: str, limit: int) -> None:
        self.asked.append(url)
        data = self.files.get(url)
        if data is None:
            raise bu.UpdateError("the download failed (HTTP 404)")
        if len(data) > limit:
            raise bu.UpdateError("the download is bigger than the release says")
        with open(dest, "wb") as fh:
            fh.write(data)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="app-updates-test-")
        self.saved = {k: getattr(barry_apps, k) for k in ("APPS_DIR", "APP_DATA_DIR", "REMOVED", "BUNDLED",
                                                          "HOME_LAYOUT", "LOGS")}
        barry_apps.APPS_DIR = os.path.join(self.tmp, "apps")
        barry_apps.APP_DATA_DIR = os.path.join(self.tmp, "app-data")
        barry_apps.REMOVED = os.path.join(self.tmp, "config", "removed-apps.json")
        barry_apps.HOME_LAYOUT = os.path.join(self.tmp, "config", "home.json")
        barry_apps.LOGS = os.path.join(self.tmp, "logs")
        barry_apps.BUNDLED = os.path.join(self.tmp, "bundled")
        os.makedirs(barry_apps.BUNDLED)
        self.logs: list[str] = []

    def tearDown(self) -> None:
        for k, v in self.saved.items():
            setattr(barry_apps, k, v)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def install(self, app_id: str, version: str, **extra) -> None:
        path = os.path.join(self.tmp, f"{app_id}.zip")
        with open(path, "wb") as fh:
            fh.write(app_zip(app_id, version, **extra))
        barry_apps.install(path)

    def catalog(self, apps: list[dict]) -> str:
        path = os.path.join(self.tmp, "catalog.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"format": 1, "apps": apps}, fh)
        return path


class CatalogTest(Base):
    def test_shipped_catalog(self):
        entries = bu.load_catalog(SHIPPED, self.logs.append)
        self.assertEqual(self.logs, [])
        ids = [e["id"] for e in entries]
        self.assertEqual(ids, ["io.github.project-barry.oni-companion", "io.github.project-barry.pipboy",
                               "io.github.project-barry.stardew", "io.github.project-barry.dice",
                               "io.github.project-barry.whatsapp"])
        # Not an app the image brings (Dino), not the template, never Wayfarer.
        text = open(SHIPPED, encoding="utf-8").read().lower()
        for word in ("dino", "template", "zelda", "wayfarer"):
            self.assertNotIn(word, text)
        by_id = {e["id"]: e for e in entries}
        self.assertTrue(by_id["io.github.project-barry.oni-companion"]["asset"].search("oni-companion-0.2.0.zip"))
        self.assertTrue(by_id["io.github.project-barry.pipboy"]["asset"].search("pipboy-1.0.0.zip"))
        self.assertTrue(by_id["io.github.project-barry.stardew"]["asset"].search("stardew-1.0.0.zip"))
        self.assertTrue(by_id["io.github.project-barry.dice"]["asset"].search("dice.zip"))
        self.assertFalse(by_id["io.github.project-barry.dice"]["asset"].search("template.zip"))
        self.assertFalse(by_id["io.github.project-barry.dice"]["tags"].search("simulator-v1.1.0"))

    def test_bad_entries_left_out(self):
        good = {"id": "io.github.a.b", "repo": "a/b", "asset": "^b\\.zip$"}
        path = self.catalog([
            good,
            dict(good),  # twice
            {"id": "NoDots", "repo": "a/b", "asset": "x"},
            {"id": "io.github.a.c", "repo": "a b", "asset": "x"},
            {"id": "io.github.a.d", "repo": "a/d", "asset": "("},
            {"id": "io.github.a.e", "repo": "a/e", "asset": "x", "manifest": "../barry-app.json"},
            {"id": "io.github.a.f", "repo": "a/f", "asset": "x", "manifest": "/etc/passwd"},
            {"id": "io.github.a.g", "repo": "a/g"},
            "nonsense",
        ])
        entries = bu.load_catalog(path, self.logs.append)
        self.assertEqual([e["id"] for e in entries], ["io.github.a.b"])
        self.assertEqual(entries[0]["name"], "io.github.a.b")
        self.assertEqual(len(self.logs), 8)

    def test_wrong_format_or_missing(self):
        path = os.path.join(self.tmp, "c.json")
        with open(path, "w") as fh:
            json.dump({"format": 2, "apps": []}, fh)
        self.assertEqual(bu.load_catalog(path, self.logs.append), [])
        self.assertEqual(bu.load_catalog(os.path.join(self.tmp, "none.json"), self.logs.append), [])


class VersionTest(unittest.TestCase):
    def test_tag_version(self):
        self.assertEqual(bu.tag_version("v1.2.0"), "1.2.0")
        self.assertEqual(bu.tag_version("0.3"), "0.3")
        for tag in ("simulator-v1.1.0", "art-1", "v1.0.0-beta", "", None):
            self.assertIsNone(bu.tag_version(tag))

    def test_newer(self):
        self.assertTrue(bu.newer("1.0.1", "1.0.0"))
        self.assertTrue(bu.newer("1.10.0", "1.9.9"))
        self.assertTrue(bu.newer("2", "1.9"))
        self.assertFalse(bu.newer("1.0.0", "1.0.0"))
        self.assertFalse(bu.newer("1.0.0", "1.0"))
        self.assertFalse(bu.newer("1.0", "1.0.0"))
        self.assertFalse(bu.newer("0.9.9", "1.0.0"))


class MarkerTest(Base):
    def test_due_and_written(self):
        state = os.path.join(self.tmp, "data", "app-updates.json")
        self.assertTrue(bu.due("0.5.5", bu.read_state(state)))
        self.assertFalse(bu.due("", bu.read_state(state)))  # not PB-OS: never
        bu.write_state("0.5.5", state)
        self.assertFalse(bu.due("0.5.5", bu.read_state(state)))
        self.assertTrue(bu.due("0.5.6", bu.read_state(state)))
        with open(state, "w") as fh:
            fh.write("not json")
        self.assertTrue(bu.due("0.5.5", bu.read_state(state)))

    def test_os_version(self):
        path = os.path.join(self.tmp, "version")
        self.assertEqual(bu.os_version(path), "")
        with open(path, "w") as fh:
            fh.write("0.5.4.31\n")
        self.assertEqual(bu.os_version(path), "0.5.4.31")


class PickTest(Base):
    def entry(self, **kw):
        path = self.catalog([dict({"id": "io.github.p.dice", "repo": "p/apps", "asset": "^dice\\.zip$",
                                   "tags": "^v[0-9]"}, **kw)])
        return bu.load_catalog(path)[0]

    def test_pick(self):
        rels = [
            release("simulator-v1.1.0", {"dice.zip": b"x"}),
            release("v1.4.0", {"dice.zip": b"x"}, draft=True),
            release("v1.3.0", {"dice.zip": b"x"}, prerelease=True),
            release("v1.2.5", {"dino.zip": b"x"}),  # no dice in it
            release("v1.2.0", {"dice.zip": b"x", "template.zip": b"x"}),
            release("v1.10.0", {"dice.zip": b"y"}),
            release("v1.9.0", {"dice.zip": b"x"}),
        ]
        rel, asset = bu.pick_release(rels, self.entry())
        self.assertEqual((rel["tag_name"], asset["name"]), ("v1.10.0", "dice.zip"))

    def test_none(self):
        self.assertIsNone(bu.pick_release([], self.entry()))
        self.assertIsNone(bu.pick_release(None, self.entry()))
        self.assertIsNone(bu.pick_release([release("v1.0.0", {"template.zip": b"x"})], self.entry()))


class CheckTest(Base):
    def setUp(self):
        super().setUp()
        self.install("io.github.p.dice", "1.0.0")
        self.install("io.github.p.solo", "1.0.0")
        self.cat = bu.load_catalog(self.catalog([
            {"id": "io.github.p.dice", "repo": "p/apps", "asset": "^dice\\.zip$",
             "manifest": "apps/dice/barry-app.json", "tags": "^v[0-9]"},
            {"id": "io.github.p.solo", "repo": "p/solo", "asset": "^solo-[0-9.]+\\.zip$"},
            {"id": "io.github.p.absent", "repo": "p/absent", "asset": "^a\\.zip$"},
        ]))

    def gh(self, dice_manifest=None, solo=None, apps_rel=None):
        answers = {
            f"{API}/repos/p/apps/releases?per_page=30": apps_rel if apps_rel is not None
            else [release("v1.2.0", {"dice.zip": b"z"})],
            f"{RAW}/p/apps/v1.2.0/apps/dice/barry-app.json": dice_manifest if dice_manifest is not None
            else {"id": "io.github.p.dice", "version": "1.1.0"},
            f"{API}/repos/p/solo/releases?per_page=30": solo if solo is not None
            else [release("v1.0.0", {"solo-1.0.0.zip": b"s"})],
        }
        return FakeGitHub(answers)

    def test_offers_newer_installed(self):
        gh = self.gh()
        offers, complete = bu.check(self.cat, barry_apps.installed(), set(), gh, self.logs.append)
        self.assertTrue(complete)
        self.assertEqual([(o["id"], o["installed"], o["version"], o["tag"], o["file"]) for o in offers],
                         [("io.github.p.dice", "1.0.0", "1.1.0", "v1.2.0", "dice.zip")])
        self.assertTrue(offers[0]["digest"].startswith("sha256:"))
        # Not installed: not asked about at all.
        self.assertFalse(any("absent" in u for u in gh.asked))

    def test_tag_version_entry(self):
        gh = self.gh(solo=[release("v1.2.0", {"solo-1.2.0.zip": b"s"})])
        offers, _ = bu.check(self.cat, barry_apps.installed(), set(), gh)
        self.assertEqual(sorted(o["id"] for o in offers), ["io.github.p.dice", "io.github.p.solo"])

    def test_bundled_not_offered(self):
        offers, complete = bu.check(self.cat, barry_apps.installed(), {"io.github.p.dice"}, self.gh())
        self.assertEqual((offers, complete), ([], True))

    def test_no_release_is_quiet(self):
        gh = self.gh(apps_rel=[])
        gh.answers[f"{API}/repos/p/solo/releases?per_page=30"] = None  # 404: no such repository
        offers, complete = bu.check(self.cat, barry_apps.installed(), set(), gh, self.logs.append)
        self.assertEqual((offers, complete), ([], True))

    def test_offline_is_incomplete(self):
        gh = self.gh()
        gh.answers[f"{API}/repos/p/solo/releases?per_page=30"] = bu.NetError("offline")
        offers, complete = bu.check(self.cat, barry_apps.installed(), set(), gh, self.logs.append)
        self.assertFalse(complete)
        self.assertEqual([o["id"] for o in offers], ["io.github.p.dice"])  # what did answer

    def test_manifest_must_be_the_app(self):
        gh = self.gh(dice_manifest={"id": "io.github.p.other", "version": "9.0.0"})
        offers, _ = bu.check(self.cat, barry_apps.installed(), set(), gh, self.logs.append)
        self.assertEqual(offers, [])

    def test_needs_newer_launcher_not_offered(self):
        gh = self.gh(dice_manifest={"id": "io.github.p.dice", "version": "2.0.0", "minApi": barry_apps.API + 1})
        offers, _ = bu.check(self.cat, barry_apps.installed(), set(), gh, self.logs.append)
        self.assertEqual(offers, [])

    def test_same_version_not_offered(self):
        gh = self.gh(dice_manifest={"id": "io.github.p.dice", "version": "1.0.0"})
        offers, _ = bu.check(self.cat, barry_apps.installed(), set(), gh)
        self.assertEqual(offers, [])


class FetchTest(Base):
    def offer(self, data: bytes, **kw) -> dict:
        rel = release("v1.1.0", {"dice.zip": data})
        a = rel["assets"][0]
        return dict({"id": "io.github.p.dice", "name": "Dice", "installed": "1.0.0", "version": "1.1.0",
                     "file": a["name"], "url": a["browser_download_url"], "size": a["size"],
                     "digest": a["digest"]}, **kw)

    def fetch(self, offer: dict, data: bytes):
        folder = tempfile.mkdtemp(dir=self.tmp)
        return bu.fetch(offer, FakeGitHub(files={offer["url"]: data}), folder)

    def test_good(self):
        data = app_zip("io.github.p.dice", "1.1.0")
        path, m = self.fetch(self.offer(data), data)
        self.assertEqual((m["id"], m["version"]), ("io.github.p.dice", "1.1.0"))
        self.assertTrue(os.path.isfile(path))

    def test_digest_mismatch(self):
        data = app_zip("io.github.p.dice", "1.1.0")
        other = app_zip("io.github.p.dice", "1.1.1")
        with self.assertRaisesRegex(bu.UpdateError, "checksum"):
            self.fetch(self.offer(data, size=len(other)), other)

    def test_size_mismatch(self):
        data = app_zip("io.github.p.dice", "1.1.0")
        with self.assertRaisesRegex(bu.UpdateError, "bytes|bigger"):
            self.fetch(self.offer(data, size=len(data) + 5, digest=None), data)

    def test_too_big(self):
        with self.assertRaisesRegex(bu.UpdateError, "limit"):
            self.fetch(self.offer(b"x", size=barry_apps.MAX_ARCHIVE + 1), b"x")

    def test_not_zip(self):
        data = b"#!/bin/sh\necho hi\n"
        with self.assertRaisesRegex(bu.UpdateError, "not a zip"):
            self.fetch(self.offer(data), data)

    def test_other_app(self):
        data = app_zip("io.github.p.other", "1.1.0")
        with self.assertRaisesRegex(bu.UpdateError, "another app"):
            self.fetch(self.offer(data), data)

    def test_not_newer(self):
        data = app_zip("io.github.p.dice", "1.0.0")
        with self.assertRaisesRegex(bu.UpdateError, "not newer"):
            self.fetch(self.offer(data), data)

    def test_not_github(self):
        with self.assertRaisesRegex(bu.UpdateError, "not on GitHub"):
            bu.GitHub().download("https://example.com/dice.zip", os.path.join(self.tmp, "x"), 10)
        with self.assertRaisesRegex(bu.UpdateError, "not on GitHub"):
            bu.GitHub().download("http://github.com/dice.zip", os.path.join(self.tmp, "x"), 10)


class UpdaterTest(Base):
    def setUp(self):
        super().setUp()
        self.install("io.github.p.dice", "1.0.0")
        self.cat = self.catalog([{"id": "io.github.p.dice", "repo": "p/apps", "asset": "^dice\\.zip$",
                                  "manifest": "apps/dice/barry-app.json", "tags": "^v[0-9]"}])
        self.state = os.path.join(self.tmp, "data", "app-updates.json")
        self.new = app_zip("io.github.p.dice", "1.1.0")
        rel = release("v1.2.0", {"dice.zip": self.new})
        self.gh = FakeGitHub({f"{API}/repos/p/apps/releases?per_page=30": [rel],
                              f"{RAW}/p/apps/v1.2.0/apps/dice/barry-app.json":
                                  {"id": "io.github.p.dice", "version": "1.1.0"}},
                             {rel["assets"][0]["browser_download_url"]: self.new})
        self.changes = 0

    def updater(self, install=None) -> bu.Updater:
        def changed():
            self.changes += 1

        def default_install(path):
            try:
                barry_apps.install(path)
                return True, ""
            except barry_apps.AppError as err:
                return False, str(err)
        return bu.Updater(self.gh, install or default_install, changed, catalog=self.cat, state=self.state,
                          version="0.5.5", log=self.logs.append, background=False)

    def test_offer_then_update(self):
        u = self.updater()
        # The app's data survives the update.
        data = os.path.join(barry_apps.APP_DATA_DIR, "io.github.p.dice", "kept.txt")
        with open(data, "w") as fh:
            fh.write("score")
        self.assertTrue(u.run_check())
        p = u.payload()
        self.assertEqual((p["phase"], p["serial"]), ("offer", 1))
        self.assertEqual([(a["id"], a["installed"], a["version"]) for a in p["apps"]],
                         [("io.github.p.dice", "1.0.0", "1.1.0")])
        self.assertTrue(bu.due("0.5.5", bu.read_state(self.state)))  # not answered yet
        self.assertEqual(u.answer({"do": "close"})[0], 409)
        self.assertEqual(u.answer({"do": "update"}), (200, {"ok": True}))
        p = u.payload()
        self.assertEqual(p["phase"], "done")
        self.assertEqual(p["apps"][0]["status"], "updated")
        self.assertEqual(barry_apps.installed()["io.github.p.dice"]["version"], "1.1.0")
        self.assertTrue(os.path.isfile(data))
        self.assertFalse(bu.due("0.5.5", bu.read_state(self.state)))
        self.assertEqual(u.answer({"do": "close"})[0], 200)
        self.assertEqual(u.payload()["phase"], "idle")
        # Answered: not again for this PB-OS version.
        asked = len(self.gh.asked)
        self.assertTrue(u.run_check())
        self.assertEqual(len(self.gh.asked), asked)

    def test_failed_update_keeps_old(self):
        u = self.updater(install=lambda path: (False, "no room"))
        u.run_check()
        u.answer({"do": "update"})
        p = u.payload()
        self.assertEqual((p["phase"], p["apps"][0]["status"], p["apps"][0]["error"]), ("done", "failed", "no room"))
        self.assertEqual(barry_apps.installed()["io.github.p.dice"]["version"], "1.0.0")

    def test_bad_download_fails_cleanly(self):
        url = next(iter(self.gh.files))
        self.gh.files[url] = b"garbage"
        u = self.updater()
        u.run_check()
        u.answer({"do": "update"})
        app = u.payload()["apps"][0]
        self.assertEqual(app["status"], "failed")
        self.assertEqual(barry_apps.installed()["io.github.p.dice"]["version"], "1.0.0")

    def test_not_now(self):
        u = self.updater()
        u.run_check()
        self.assertEqual(u.answer({"do": "later"})[0], 200)
        self.assertEqual(u.payload()["phase"], "idle")
        self.assertFalse(u.due())
        self.assertEqual(barry_apps.installed()["io.github.p.dice"]["version"], "1.0.0")
        # The next PB-OS version asks again.
        u.version = "0.5.6"
        self.assertTrue(u.due())

    def test_offline_tries_again(self):
        self.gh.answers[f"{API}/repos/p/apps/releases?per_page=30"] = bu.NetError("offline")
        u = self.updater()
        self.assertFalse(u.run_check())
        self.assertEqual(u.payload()["phase"], "idle")
        self.assertTrue(u.due())  # the next start (or the network coming up) checks again
        self.assertEqual(self.changes, 0)  # nothing shown

    def test_nothing_new_is_done(self):
        self.gh.answers[f"{RAW}/p/apps/v1.2.0/apps/dice/barry-app.json"] = {"id": "io.github.p.dice",
                                                                           "version": "1.0.0"}
        u = self.updater()
        self.assertTrue(u.run_check())
        self.assertEqual(u.payload()["phase"], "idle")
        self.assertFalse(u.due())
        self.assertEqual(self.changes, 0)


class MinApiTest(Base):
    def folder(self, **extra) -> str:
        root = tempfile.mkdtemp(dir=self.tmp)
        with open(os.path.join(root, "barry-app.json"), "w") as fh:
            json.dump(dict({"format": 1, "id": "io.github.p.t", "name": "T", "version": "1.0.0",
                            "main": "main.qml"}, **extra), fh)
        with open(os.path.join(root, "main.qml"), "w") as fh:
            fh.write("import QtQuick\nItem {}\n")
        return root

    def test_default_and_current(self):
        self.assertEqual(barry_apps.read_manifest(self.folder())["minApi"], 1)
        self.assertEqual(barry_apps.read_manifest(self.folder(minApi=barry_apps.API))["minApi"], barry_apps.API)

    def test_newer_refused(self):
        with self.assertRaisesRegex(barry_apps.AppError, "needs a newer Barry Launcher"):
            barry_apps.install(self.folder(minApi=barry_apps.API + 1))
        self.assertEqual(barry_apps.installed(), {})

    def test_bad_values(self):
        for bad in (0, "2", True, 1.5):
            with self.assertRaisesRegex(barry_apps.AppError, "minApi"):
                barry_apps.read_manifest(self.folder(minApi=bad))


if __name__ == "__main__":
    unittest.main(verbosity=2)
