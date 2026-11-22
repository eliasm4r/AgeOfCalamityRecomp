"""Tests of the extraction tools with synthetic files (no game data)."""
from __future__ import annotations

import argparse
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import check_content  # noqa: E402
import inspect_dump  # noqa: E402


def fake_nso(build_id_prefix: str, text=0x1000, ro=0x2000, data=0x800, bss=0x3000) -> bytes:
    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    struct.pack_into("<I", hdr, 0x0C, 7)
    struct.pack_into("<III", hdr, 0x10, 0x100, 0, text)
    struct.pack_into("<III", hdr, 0x20, 0x100 + text, text, ro)
    struct.pack_into("<III", hdr, 0x30, 0x100 + text + ro, text + ro, data)
    struct.pack_into("<I", hdr, 0x3C, bss)
    hdr[0x40:0x50] = bytes.fromhex(build_id_prefix)
    return bytes(hdr)


def fake_npdm(sys_res=0) -> bytes:
    meta = bytearray(0x80)
    meta[0:4] = b"META"
    meta[0x0C] = 1
    meta[0x0E] = 44
    meta[0x0F] = 0
    struct.pack_into("<I", meta, 0x14, sys_res)
    struct.pack_into("<I", meta, 0x1C, 0x100000)
    meta[0x20:0x24] = b"test"
    return bytes(meta)


class InspectDumpTest(unittest.TestCase):
    def make_exefs(self, d: Path, ids: dict[str, str]) -> None:
        for m, bid in ids.items():
            (d / m).write_bytes(fake_nso(bid))
        (d / "main.npdm").write_bytes(fake_npdm())

    def test_identifies_130(self):
        known = json.loads(inspect_dump.KNOWN.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self.make_exefs(d, known["builds"]["1.3.0"]["modules"])
            out = d / "r.json"
            with redirect_stdout(io.StringIO()):
                rc = inspect_dump.main(["--exefs", str(d), "--out", str(out)])
            self.assertEqual(rc, 0)
            r = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(r["version"], "1.3.0")
            self.assertEqual(r["npdm"]["system_resource_size"], 0)
            self.assertEqual(r["modules"]["main"]["image_size"], 0x7000)

    def test_unknown_build_fails(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            self.make_exefs(d, {m: "00" * 16 for m in inspect_dump.MODULES})
            with redirect_stdout(io.StringIO()):
                self.assertEqual(inspect_dump.main(["--exefs", str(d)]), 1)

    def test_romfs_classify(self):
        self.assertEqual(inspect_dump.classify("asset/system.rdb"), "rdb-index")
        self.assertEqual(inspect_dump.classify("asset/system.rdb.bin1"), "rdb-data")
        self.assertEqual(inspect_dump.classify("asset/x.rdb.bin3_0"), "rdb-dlc")
        self.assertEqual(inspect_dump.classify("asset/data/0001.file"), "rdb-file")


class CheckContentTest(unittest.TestCase):
    def check(self, rel: str, content: bytes) -> list[str]:
        # Writes to a temporary folder: the test never creates files inside the repository.
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            return check_content.check_file(rel, root=Path(t))

    def test_clean_source_passes(self):
        self.assertEqual(self.check("tests/_tmp_ok.py", b"print('ok')\n"), [])

    def test_blocks_game_files(self):
        self.assertTrue(check_content.check_file("asset/system.rdb.bin1"))
        self.assertTrue(check_content.check_file("asset/x.rdb.bin3_0"))
        self.assertTrue(check_content.check_file("game/base.nsp"))
        self.assertTrue(check_content.check_file("build/v130/exefs/main"))
        self.assertTrue(check_content.check_file("prod.keys"))
        self.assertEqual(check_content.check_file("runtime/binding.c"), [])

    def test_blocks_binary_magic(self):
        self.assertTrue(self.check("tests/_tmp_nso.dat", b"NSO0" + b"\0" * 60))

    def test_blocks_keys_text(self):
        self.assertTrue(self.check("tests/_tmp_k.txt", b"header_key = " + b"ab" * 32 + b"\n"))

    def test_blocks_generated_marker(self):
        self.assertTrue(self.check("tests/_tmp_g.c", f"// {check_content.GENERATED_MARKER}\n".encode()))


try:   # nx.py needs the cryptography package, which the minimal CI does not install
    import extract_dlc
except ImportError:
    extract_dlc = None


def fake_ivfc(data: bytes, block_log2: int = 12):
    """IVFC section header and section bytes for `data` in the data level: (fs, section)."""
    import hashlib
    block = 1 << block_log2

    def hashes(level: bytes) -> bytes:
        out = b""
        for pos in range(0, len(level), block):
            part = level[pos:pos + block]
            out += hashlib.sha256(part + bytes(block - len(part))).digest()
        return out

    levels = [data]
    for _ in range(5):
        levels.insert(0, hashes(levels[0]))
    fs = bytearray(0x200)
    fs[8:12] = b"IVFC"
    struct.pack_into("<II", fs, 0x10, 0x20, 7)
    section, off = b"", 0
    for k, level in enumerate(levels):
        struct.pack_into("<QQII", fs, 0x18 + 0x18 * k, off, len(level), block_log2, 0)
        padded = level + bytes(-len(level) % block)
        section += padded
        off += len(padded)
    fs[0xC8:0xE8] = hashes(levels[0])[:32]
    return bytes(fs), section


class FakeSection:
    def __init__(self, fs: bytes, data: bytes):
        self.fs, self.data = fs, data

    def read(self, offset: int, size: int) -> bytes:
        return self.data[offset:offset + size]


@unittest.skipUnless(extract_dlc, "no cryptography package")
class ExtractDlcTest(unittest.TestCase):
    DATA = bytes(range(256)) * 97 + b"fim"   # not a multiple of the block: the last block is padded with zeros

    def test_copy_checks_the_hash_tree(self):
        fs, section = fake_ivfc(self.DATA)
        with tempfile.TemporaryDirectory() as d:
            size, sha = extract_dlc.copy_verified(FakeSection(fs, section), Path(d) / "dlc.bin")
            self.assertEqual(size, len(self.DATA))
            self.assertEqual((Path(d) / "dlc.bin").read_bytes(), self.DATA)
            import hashlib
            self.assertEqual(sha, hashlib.sha256(self.DATA).hexdigest())

    def test_changed_data_block_fails(self):
        fs, section = fake_ivfc(self.DATA)
        bad = bytearray(section)
        bad[len(section) - (-len(self.DATA) % 4096) - 100] ^= 1   # one byte of the data level (before the padding)
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError):
                extract_dlc.copy_verified(FakeSection(fs, bytes(bad)), Path(d) / "dlc.bin")
            self.assertFalse((Path(d) / "dlc.bin").exists())

    def test_changed_hash_table_fails(self):
        fs, section = fake_ivfc(self.DATA)
        bad = bytearray(section)
        bad[3] ^= 1   # level 0
        with tempfile.TemporaryDirectory() as d, self.assertRaises(ValueError):
            extract_dlc.copy_verified(FakeSection(fs, bytes(bad)), Path(d) / "dlc.bin")


try:
    import extract
except ImportError:
    extract = None
import make_build  # noqa: E402  (standard library only)


def fake_pkg(role: str, version, name: str) -> dict:
    tid = {"base": "01002B00111A2000", "dlc": "01002B00111A3001"}.get(role, "01002B00111A2800")
    return {"path": Path(name), "container": "nsp", "role": role, "title_id": tid, "version": version, "ncas": [],
            "tickets": 0}


@unittest.skipUnless(extract, "no cryptography package")
class InventoryTest(unittest.TestCase):
    """The inventory goes by the role scan() took from the contents; the file name plays no part."""

    def inventory(self, pkgs):
        from types import SimpleNamespace
        real, extract.scan = extract.scan, lambda game_dir, keys: pkgs
        buf = io.StringIO()
        try:
            with redirect_stdout(buf):
                rc = extract.cmd_inventory(SimpleNamespace(game_dir=Path("."), nsp=[]), {})
        finally:
            extract.scan = real
        return rc, [l.split(" ", 2)[1:] for l in buf.getvalue().splitlines() if l.startswith("@@PROBLEM ")]

    def test_good_dump_with_any_names(self):
        rc, problems = self.inventory([fake_pkg("base", 0, "a.nsp"), fake_pkg("update", 327680, "b.nsp"),
                                       fake_pkg("dlc", 0, "c.nsp")])
        self.assertEqual((rc, problems), (0, []))

    def test_update_of_another_version(self):
        rc, problems = self.inventory([fake_pkg("base", 0, "a.nsp"), fake_pkg("update-x", 458752, "b.nsp")])
        self.assertEqual((rc, problems), (1, [["wrong_update", "458752"]]))

    def test_right_update_wins_over_another_version(self):
        rc, problems = self.inventory([fake_pkg("base", 0, "a.nsp"), fake_pkg("update-x", 458752, "b.nsp"),
                                       fake_pkg("update", 327680, "c.nsp")])
        self.assertEqual((rc, problems), (0, []))

    def test_missing_packages(self):
        rc, problems = self.inventory([fake_pkg("dlc", 0, "c.nsp")])
        self.assertEqual((rc, sorted(p[0] for p in problems)), (1, ["base", "update"]))

    def test_every_problem_has_a_message_for_the_player(self):
        for code in ("base", "update", "updates", "wrong_update"):
            self.assertIn("p_" + code, make_build.TEXT)


class DumpSourcesTest(unittest.TestCase):
    """The dump as one folder, as files chosen one by one (--nsp), or both: each package once."""

    def layout(self, d):
        d = Path(d)
        (d / "dump" / "sub").mkdir(parents=True)
        for name in ("dump/a.nsp", "dump/sub/b.XCI", "dump/notes.txt", "elsewhere.nsp"):
            (d / name).write_bytes(b"x")
        return d

    @unittest.skipUnless(extract, "no cryptography package")
    def test_package_paths(self):
        with tempfile.TemporaryDirectory() as d:
            d = self.layout(d)
            got = extract.package_paths(d / "dump", [d / "elsewhere.nsp", d / "dump" / "a.nsp"])
            self.assertEqual(sorted(p.name for p in got), ["a.nsp", "b.XCI", "elsewhere.nsp"])
            self.assertEqual([p.name for p in extract.package_paths(None, [d / "elsewhere.nsp"])], ["elsewhere.nsp"])

    @unittest.skipUnless(extract, "no cryptography package")
    def test_folder_default_only_without_chosen_files(self):
        from types import SimpleNamespace
        self.assertEqual(extract.game_dir_of(SimpleNamespace(game_dir=None, nsp=[])), extract.ROOT / "Game Files")
        self.assertIsNone(extract.game_dir_of(SimpleNamespace(game_dir=None, nsp=[Path("a.nsp")])))
        self.assertEqual(extract.game_dir_of(SimpleNamespace(game_dir=Path("x"), nsp=[Path("a.nsp")])), Path("x"))

    def test_make_build_passes_the_sources_on(self):
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as d:
            d = self.layout(d)
            args = SimpleNamespace(game_dir=d / "dump", nsp=[d / "elsewhere.nsp", d / "dump" / "a.nsp"])
            self.assertEqual(sorted(p.name for p in make_build.dump_nsps(args)), ["a.nsp", "elsewhere.nsp"])
            self.assertEqual(make_build.dump_args(args), ["--game-dir", d / "dump", "--nsp", d / "elsewhere.nsp",
                                                          "--nsp", d / "dump" / "a.nsp"])
            args = SimpleNamespace(game_dir=None, nsp=[d / "elsewhere.nsp"])
            self.assertEqual(make_build.dump_args(args), ["--nsp", d / "elsewhere.nsp"])


class ProgressTest(unittest.TestCase):
    """make_build's progress and time left, on a fake clock, without its ticker thread."""

    def setUp(self):
        from unittest import mock
        self.now = 1000.0
        patcher = mock.patch.object(make_build.time, "monotonic", lambda: self.now)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.p = make_build.Progress()
        self.p.ticker = "none"   # no thread: the test calls tick()
        self.lines = []
        real = make_build.out
        make_build.out = self.lines.append
        self.addCleanup(setattr, make_build, "out", real)

    def last(self):
        value, left = self.lines[-1].split()[1:]
        return float(value), int(left)

    def test_weights_first_then_this_pc(self):
        p = self.p
        p.start([("check", None), ("winbuild", None), ("shaders", None)], False)
        p.step(0)
        self.assertEqual(self.last(), (0.0, 885))   # 5 + 480 + 400 weight seconds, nothing measured yet
        self.now += 10
        p.finish("ok")
        p.step(1)
        self.now += 240   # this PC builds at half the guessed speed
        p.line("[50/100] cc main.c")
        value, left = self.last()
        self.assertAlmostEqual(value, (5 + 480 * 0.475) / 885, places=3)
        self.assertAlmostEqual(left / ((885 - 5 - 480 * 0.475) * 250 / (5 + 480 * 0.475)), 1, places=2)

    def test_quick_step_weighs_nothing(self):
        p = self.p
        p.start([("tools", None), ("winbuild", None)], False)
        p.step(0)
        self.now += 1   # the tools were already there: 1 s for a weight of 120
        p.finish("ok")
        p.step(1)
        value, left = self.last()
        self.assertEqual(left, 480)   # the guessed pace, not 1/120 of it
        self.now += 120
        p.line("[20/100] cc main.c")   # 91 weight seconds took 120 s: this PC's pace, slower than the guess
        self.assertAlmostEqual(self.last()[1] / ((480 - 480 * 0.19) * 120 / (480 * 0.19)), 1, places=2)

    def test_skipped_step_weighs_nothing(self):
        p = self.p
        p.start([("romfs", None), ("winbuild", None)], False)
        p.step(0)
        p.finish("skip")
        p.step(1)
        self.assertEqual(self.last(), (0.0, 480))

    def test_silent_step_moves_with_time(self):
        p = self.p
        p.start([("tools", None), ("winbuild", None)], False)
        p.step(0)
        self.now += 60
        p.tick()
        self.assertAlmostEqual(self.last()[0], 60 / 600, places=3)   # half of tools' 120 s
        self.now += 1000
        p.tick()
        self.assertAlmostEqual(self.last()[0], 0.9 * 120 / 600, places=3)   # never past 90 % on time alone

    def test_romfs_bytes(self):
        p = self.p
        p.start([("romfs", None)], False)
        p.step(0)
        p.line("romfs progress: 5000 of 20000 MiB")
        self.assertAlmostEqual(self.last()[0], 0.25, places=3)

    def test_prepare_line(self):
        p = self.p
        p.start([("winbuild", None)], True)
        p.step(0)
        self.assertEqual(self.lines[-1], "progress 0.000 " + make_build.tr("t_winbuild") + " (about 8 min left)")


class NoCryptographyTest(unittest.TestCase):
    """MSYS2's Python runs romfs_index.py and prepare_shaders.py on decrypted data: neither may need cryptography."""

    def test_readers_import_without_cryptography(self):
        import builtins
        import importlib
        real = builtins.__import__

        def block(name, *a, **k):
            if name.startswith("cryptography"):
                raise ImportError("blocked " + name)
            return real(name, *a, **k)

        saved = {n: sys.modules.pop(n) for n in ("nx", "romfs_index", "prepare_shaders") if n in sys.modules}
        self.addCleanup(sys.modules.update, saved)
        with mock.patch.object(builtins, "__import__", block):
            for name in ("nx", "romfs_index", "prepare_shaders"):
                importlib.import_module(name)
                sys.modules.pop(name, None)


class MakeBuildTest(unittest.TestCase):
    def test_run_collects_problem_lines(self):
        found = []
        with redirect_stdout(io.StringIO()), self.assertRaises(make_build.StepFailed):
            make_build.run([sys.executable, "-c",
                            "import sys; print('@@PROBLEM wrong_update 458752'); print('text'); sys.exit(1)"],
                           problems=found)
        self.assertEqual(found, [("wrong_update", "458752")])

    def test_messages_are_english_text(self):
        for key, text in make_build.TEXT.items():
            self.assertIsInstance(text, str, key)
            self.assertTrue(text, key)

    def test_known_romfs_size(self):
        self.assertEqual(make_build.romfs_size(), 11719230892)

    def msys2_step(self, d, works):
        """tools_msys2 with the player's MSYS2 and our copy both unpacked; works: msys_works' answers, in order."""
        theirs, own = d / "theirs/msys64", d / "own/msys64"
        for root in (theirs, own):
            (root / "usr/bin").mkdir(parents=True)
            (root / "usr/bin/bash.exe").write_bytes(b"")
        args = argparse.Namespace(msys2=str(theirs))
        listed = subprocess.CompletedProcess([], 0, b"", b"")
        with mock.patch.object(make_build, "MSYS2_OWN", own), mock.patch.object(make_build, "capture", lambda *a, **k: listed), \
                mock.patch.object(make_build, "msys_works", side_effect=works), mock.patch.object(make_build, "out"), \
                mock.patch.object(make_build, "msys") as commands:
            make_build.tools_msys2(args)
        commands.assert_not_called()   # nothing is installed into the player's MSYS2
        return args.msys2, theirs, own

    def test_working_msys2_is_kept(self):
        with tempfile.TemporaryDirectory() as d:
            used, theirs, _own = self.msys2_step(Path(d), [True])
            self.assertEqual(used, str(theirs))

    def test_broken_msys2_moves_to_our_copy(self):
        with tempfile.TemporaryDirectory() as d:
            used, _theirs, own = self.msys2_step(Path(d), [False, True])
            self.assertEqual(used, str(own))

    def test_our_broken_copy_stops_the_build(self):
        with tempfile.TemporaryDirectory() as d, self.assertRaises(make_build.StepFailed):
            self.msys2_step(Path(d), [False, False])

    def test_our_copy_never_unpacks_over_c_msys64(self):
        self.assertEqual(make_build.MSYS2_OWN.name, "msys64")
        self.assertNotEqual(make_build.MSYS2_OWN.parent, Path("C:/"))

    def pacman_lock(self, d, returncode, stdout):
        """msys_unlock with a lock on disk and tasklist's answer; True when the lock is still there."""
        lock = d / "var/lib/pacman/db.lck"
        lock.parent.mkdir(parents=True)
        lock.write_bytes(b"")
        answer = subprocess.CompletedProcess([], returncode, stdout, b"")
        with mock.patch.object(make_build, "capture", lambda *a, **k: answer), mock.patch.object(make_build, "out"):
            make_build.msys_unlock(d)
        return lock.exists()

    def test_stale_pacman_lock_is_removed(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(self.pacman_lock(Path(d), 0, b"INFO: No tasks are running"))

    def test_lock_of_a_running_pacman_stays(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.pacman_lock(Path(d), 0, b'"pacman.exe","1234","Console","1","9,000 K"'))

    def test_lock_stays_when_tasklist_fails(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertTrue(self.pacman_lock(Path(d), 1, b""))


class ReadOnlySystemTest(unittest.TestCase):
    """SteamOS / Bazzite: the build tools go into a distrobox (make_build.use_box)."""

    def setUp(self):
        make_build._use_box = None
        self.addCleanup(setattr, make_build, "_use_box", None)

    def box(self, readonly=True, distrobox=True, clang=False):
        which = {"distrobox": "/usr/bin/distrobox" if distrobox else None,
                 "clang": "/usr/bin/clang" if clang else None, "ninja": "/usr/bin/ninja" if clang else None}
        return (mock.patch.object(make_build, "system_readonly", lambda: readonly),
                mock.patch.object(make_build.shutil, "which", lambda n: which.get(n)))

    def test_box_only_on_a_read_only_system_without_tools(self):
        for readonly, distrobox, clang, want in ((True, True, False, True), (False, True, False, False),
                                                 (True, False, False, False), (True, True, True, False)):
            make_build._use_box = None
            a, b = self.box(readonly, distrobox, clang)
            with a, b:
                self.assertEqual(make_build.use_box(), want, (readonly, distrobox, clang))

    def test_wraps_the_venv_python_only(self):
        make_build._use_box = True
        venv = [make_build.VENV_PY, "-u", "tools/build.py"]
        self.assertEqual(make_build.box_wrap(venv), ["distrobox", "enter", make_build.BOX, "--"] + venv)
        self.assertEqual(make_build.box_wrap(["bash", "x.sh"]), ["bash", "x.sh"])
        make_build._use_box = False
        self.assertEqual(make_build.box_wrap(venv), venv)

    def test_tools_step_makes_the_box_once(self):
        make_build._use_box = True
        calls = []
        table = ("ID | NAME | STATUS | IMAGE\n"
                 "1a | acr-build | Up | docker.io/library/archlinux:latest\n").encode()
        args = mock.Mock(deck=True, windows=False)
        for listed, creates in ((b"", True), (table, False)):
            calls.clear()
            with mock.patch.object(make_build, "WIN", False),                     mock.patch.object(make_build, "run", lambda cmd, **kw: calls.append([str(c) for c in cmd])),                     mock.patch.object(make_build, "capture", lambda cmd, **kw: mock.Mock(stdout=listed)),                     redirect_stdout(io.StringIO()):
                self.assertEqual(make_build.s_tools(args), "ok")
            self.assertEqual(any(c[:2] == ["distrobox", "create"] for c in calls), creates)
            self.assertEqual(calls[-1][:4], ["distrobox", "enter", make_build.BOX, "--"])
            self.assertTrue(calls[-1][-2].endswith("setup_wsl.sh") and calls[-1][-1] == "--minimal")

    def test_read_only_without_distrobox_stops_with_a_message(self):
        a, b = self.box(readonly=True, distrobox=False)
        with a, b, mock.patch.object(make_build, "WIN", False), redirect_stdout(io.StringIO()),                 self.assertRaises(make_build.StepFailed) as e:
            make_build.s_tools(mock.Mock(deck=True, windows=False))
        self.assertIn("distrobox", str(e.exception))


class MakeBuildUpdateTest(unittest.TestCase):
    """--update: an install updates from the game data its first build extracted, without reading the dump."""

    def setUp(self):
        from types import SimpleNamespace
        self.tmp = tempfile.TemporaryDirectory()
        self.root, make_build.ROOT = make_build.ROOT, Path(self.tmp.name)
        self.args = SimpleNamespace(update=True, windows=False, deck=False, send=False,
                                    game_dir=Path(self.tmp.name) / "no dump here", keys=Path("no.keys"))

    def tearDown(self):
        make_build.ROOT = self.root
        self.tmp.cleanup()

    def extract(self):
        v130 = make_build.ROOT / "build/v130"
        (v130 / "exefs").mkdir(parents=True)
        (v130 / "exefs/main").write_bytes(b"")
        (v130 / "romfs.bin").write_bytes(b"")

    def test_check_needs_the_extracted_data(self):
        with redirect_stdout(io.StringIO()), self.assertRaises(make_build.StepFailed):
            make_build.s_check(self.args)

    def test_check_passes_without_the_dump(self):
        self.extract()
        with redirect_stdout(io.StringIO()) as buf:
            make_build.s_check(self.args)
        self.assertIn("without the NSPs", buf.getvalue())

    def test_no_dump_step_runs(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(make_build.s_dlc(self.args), "skip")
            with self.assertRaises(make_build.StepFailed):
                make_build.s_romfs(self.args)


class MakePromptsTest(unittest.TestCase):
    """tools/make_prompts.py: BC3 both ways, the fingerprint runtime/prompts.c checks, the drawings."""

    def setUp(self):
        try:
            import make_prompts
        except ImportError as e:   # numpy (tools/requirements.txt)
            self.skipTest(str(e))
        self.m = make_prompts

    def test_fnv_matches_the_runtime(self):
        # FNV-1a 64 test vectors (prompts.c prompts_hash computes the same)
        self.assertEqual(self.m.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(self.m.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)

    def test_bc3_round_trip(self):
        import numpy as np
        rng = np.random.default_rng(1)
        px = np.zeros((64, 16, 4), np.uint8)
        for i in range(64):   # two colours and an alpha ramp per block, as the icons have
            a, b = rng.integers(0, 256, 3), rng.integers(0, 256, 3)
            pick = rng.integers(0, 2, 16)
            px[i, :, :3] = np.where(pick[:, None], a, b)
            px[i, :, 3] = np.linspace(0, 255, 16).astype(np.uint8)
        blocks = self.m.bc3_encode(px)
        img = self.m.bc3_decode(blocks.tobytes(), 4, 4 * 64)
        back = img.reshape(64, 4, 4, 4).reshape(64, 16, 4)
        self.assertLessEqual(int(np.abs(back[:, :, 3].astype(int) - px[:, :, 3]).max()), 20)
        self.assertLessEqual(int(np.abs(back[:, :, :3].astype(int) - px[:, :, :3]).max()), 12)

    def test_face_buttons_by_position(self):
        # prompts-xbox.bin (Nintendo layout, by position) names A as B; the label file keeps the letters
        self.assertEqual(self.m.OUTPUTS["prompts-xbox.bin"], ("xbox", True))
        self.assertEqual(self.m.OUTPUTS["prompts-xbox-label.bin"], ("xbox", False))
        self.assertEqual(self.m.SWAP, {"A": "B", "B": "A", "X": "Y", "Y": "X"})
        # PlayStation: cross is A on the bottom button, circle is A by position (Nintendo layout)
        self.assertEqual(self.m.OUTPUTS["prompts-ps.bin"], ("ps", True))
        self.assertEqual(self.m.OUTPUTS["prompts-ps-label.bin"], ("ps", False))
        self.assertEqual(self.m.PS_LABEL["A"], "cross")
        self.assertEqual(self.m.PS_POSITION["A"], "circle")
        self.assertEqual(set(self.m.PS_LABEL.values()), set(self.m.PS_POSITION.values()))

    def test_letters_stay_in_their_box(self):
        import numpy as np
        y, x = self.m.grid(40, 60)
        for word in ("A", "B", "X", "Y", "LB", "RT", "L1", "R2"):
            cov = self.m.coverage(self.m.sd_text(y, x, word, 20, 30, 20), 40, 60)
            ys, xs = np.nonzero(cov > 0)
            self.assertTrue(len(ys), word)
            self.assertGreaterEqual(ys.min(), 9, word)
            self.assertLessEqual(ys.max(), 30, word)
            self.assertGreater(xs.min(), 0, word)
            self.assertLess(xs.max(), 59, word)


if __name__ == "__main__":
    unittest.main()
