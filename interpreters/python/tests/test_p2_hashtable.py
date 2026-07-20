#!/usr/bin/env python3
"""Compile and execute CPython's post-patch P2 hashtable implementation."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile


PYTHON_DIR = pathlib.Path(__file__).resolve().parents[1]
ARCHIVE = PYTHON_DIR / "v3.13.0.zip"
PATCH_DIR = PYTHON_DIR / "patch"
HARNESS = pathlib.Path(__file__).with_name("hashtable_harness.c")
STUBS = pathlib.Path(__file__).with_name("hashtable_stubs")


class P2HashtableActualSourceTest(unittest.TestCase):
    def test_actual_post_patch_source_behavior(self) -> None:
        compiler = shutil.which("cc")
        patch_tool = shutil.which("patch")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")
        if patch_tool is None:
            self.skipTest("patch utility is unavailable")
        if not ARCHIVE.is_file():
            self.skipTest("cached CPython v3.13.0 source archive is unavailable")

        patches = sorted(PATCH_DIR.glob("*.patch"))
        self.assertTrue(patches, "CPython patch series is empty")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            with zipfile.ZipFile(ARCHIVE) as archive:
                roots = {
                    name.split("/", 1)[0]
                    for name in archive.namelist()
                    if "/" in name
                }
                self.assertEqual(len(roots), 1)
                archive.extractall(temp)

            source_root = temp / roots.pop()
            for patch_path in patches:
                try:
                    subprocess.run(
                        [
                            patch_tool,
                            "--batch",
                            "--forward",
                            "-p1",
                            "-i",
                            str(patch_path),
                        ],
                        cwd=source_root,
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                except subprocess.CalledProcessError as error:
                    self.fail(
                        f"failed to apply {patch_path.name}\n"
                        f"stdout:\n{error.stdout}\n"
                        f"stderr:\n{error.stderr}"
                    )

            hashtable_source = source_root / "Python" / "hashtable.c"
            executable = temp / "p2-hashtable-test"
            compile_command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D__NuttX__=1",
                "-D__propeller2__=1",
                f"-I{STUBS}",
                f'-DHASHTABLE_SOURCE="{hashtable_source}"',
                str(HARNESS),
                "-o",
                str(executable),
            ]
            try:
                subprocess.run(
                    compile_command,
                    check=True,
                    capture_output=True,
                    text=True,
                )
            except subprocess.CalledProcessError as error:
                self.fail(
                    "failed to compile patched Python/hashtable.c\n"
                    f"stdout:\n{error.stdout}\n"
                    f"stderr:\n{error.stderr}"
                )
            result = subprocess.run(
                [str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.stderr, "")
        self.assertEqual(
            result.stdout,
            "PASS: actual patched P2 CPython hashtable behavior\n",
        )


if __name__ == "__main__":
    unittest.main()
