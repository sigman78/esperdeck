"""Run against a built simulator; all writes stay in temporary directories."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


class StorageDirectoryTests(unittest.TestCase):
    def test_invalid_directory_never_falls_back(self):
        with tempfile.TemporaryDirectory(prefix="deck-storage-") as tmp:
            root = Path(tmp)
            default = root / "sim_storage"
            default.mkdir()
            occupied = root / "file"
            occupied.write_text("not a directory")
            for value in ("", "x" * 800, str(occupied), str(root / "missing" / "data")):
                with self.subTest(value=value):
                    result = subprocess.run(
                        [str(BINARY), "--storage-dir", value, "--keystore-status"],
                        cwd=root, capture_output=True, text=True, timeout=15,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(list(default.iterdir()), [])

    def test_missing_directory_argument_fails_before_storage(self):
        with tempfile.TemporaryDirectory(prefix="deck-storage-") as tmp:
            for options in (["--storage-dir"], ["--storage-dir", "--keystore-status"]):
                result = subprocess.run([str(BINARY), *options], cwd=tmp,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_discovery_still_works_without_option(self):
        with tempfile.TemporaryDirectory(prefix="deck-storage-") as tmp:
            root = Path(tmp)
            default = root / "sim_storage"
            default.mkdir()
            child = root / "child"
            child.mkdir()
            result = subprocess.run([str(BINARY), "--keystore-status"], cwd=child,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((default / "keys").is_dir())
            self.assertFalse((child / "sim_storage").exists())

    def test_long_directory_roundtrips_keystore(self):
        with tempfile.TemporaryDirectory(prefix="deck-storage-") as tmp:
            selected = Path(tmp) / ("long path " * 16 + "folder") / "vault"
            selected.parent.mkdir()
            for command in ("--keystore-init", "--unlock-test"):
                result = subprocess.run(
                    [str(BINARY), command, "--pin", "1234",
                     "--storage-dir", str(selected)],
                    cwd=tmp, capture_output=True, text=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((selected / "keystore.kv1").is_file())
            self.assertFalse((Path(tmp) / "sim_storage").exists())

    def test_explicit_directory_overrides_discovery(self):
        with tempfile.TemporaryDirectory(prefix="deck-storage-") as tmp:
            root = Path(tmp)
            default = root / "sim_storage"
            default.mkdir()
            sentinel = default / "settings.ini"
            sentinel.write_text("untouched\n")
            selected = root / "selected data"
            result = subprocess.run(
                [str(BINARY), "--storage-dir", str(selected), "--keystore-status"],
                cwd=root, capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((selected / "keys").is_dir(), result.stderr)
            self.assertEqual(list(default.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_text(), "untouched\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=[__file__, *remaining])
