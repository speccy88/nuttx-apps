#!/usr/bin/env python3
"""Host regression test for the guarded CPython command launcher."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest


PYTHON_DIR = pathlib.Path(__file__).resolve().parents[1]
LAUNCHER = PYTHON_DIR / "python_launcher.c"
HARNESS = pathlib.Path(__file__).with_name("launcher_harness.c")
WRAPPER = PYTHON_DIR / "python_wrapper.c"
WRAPPER_HARNESS = pathlib.Path(__file__).with_name("wrapper_harness.c")

CONFIG_HEADER = """\
#ifndef __NUTTX_CONFIG_H
#define __NUTTX_CONFIG_H
#define FAR
#define CONFIG_INTERPRETERS_CPYTHON_STACKSIZE 65536
#endif
"""

MUTEX_HEADER = """\
#ifndef __NUTTX_MUTEX_H
#define __NUTTX_MUTEX_H
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline int nxmutex_trylock(mutex_t *mutex)
{
  int ret = pthread_mutex_trylock(mutex);
  return ret == 0 ? 0 : -ret;
}
static inline int nxmutex_unlock(mutex_t *mutex)
{
  int ret = pthread_mutex_unlock(mutex);
  return ret == 0 ? 0 : -ret;
}
#endif
"""

BOARDCTL_HEADER = """\
#ifndef __SYS_BOARDCTL_H
#define __SYS_BOARDCTL_H
#include <stdint.h>
#define BOARDIOC_ROMDISK 7
int boardctl(int command, uintptr_t arg);
#endif
"""

MOUNT_HEADER = """\
#ifndef __SYS_MOUNT_H
#define __SYS_MOUNT_H
#define MS_RDONLY 1ul
int mount(const char *source, const char *target, const char *filesystem,
          unsigned long flags, const void *data);
#endif
"""

RAMDISK_HEADER = """\
#ifndef __NUTTX_DRIVERS_RAMDISK_H
#define __NUTTX_DRIVERS_RAMDISK_H
#include <stdint.h>
struct boardioc_romdisk_s
{
  int minor;
  unsigned int nsectors;
  unsigned int sectsize;
  uint8_t *image;
};
#endif
"""

DEBUG_HEADER = """\
#ifndef __NUTTX_DEBUG_H
#define __NUTTX_DEBUG_H
#define _info(...) ((void)0)
#define UNUSED(value) ((void)(value))
#endif
"""

PYTHON_HEADER = """\
#ifndef __PYTHON_H
#define __PYTHON_H
void _pyruntime_early_init(void);
int py_bytesmain(int argc, char *argv[]);
#endif
"""


class PythonLauncherTest(unittest.TestCase):
    def test_guarded_worker_lifecycle(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            include = temp / "include" / "nuttx"
            include.mkdir(parents=True)
            (include / "config.h").write_text(CONFIG_HEADER, encoding="utf-8")
            (include / "mutex.h").write_text(MUTEX_HEADER, encoding="utf-8")

            launcher_object = temp / "python_launcher.o"
            executable = temp / "python-launcher-test"
            common = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                f"-I{temp / 'include'}",
            ]
            subprocess.run(
                [
                    *common,
                    "-Dmain=python_builtin_main",
                    "-Dpthread_create=test_pthread_create",
                    "-c",
                    str(LAUNCHER),
                    "-o",
                    str(launcher_object),
                ],
                check=True,
                text=True,
            )
            subprocess.run(
                [
                    *common,
                    str(HARNESS),
                    str(launcher_object),
                    "-o",
                    str(executable),
                ],
                check=True,
                text=True,
            )
            result = subprocess.run(
                [str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("P2PY:RUNTIME:BUSY:CODE=", result.stdout)
        self.assertIn("ERROR: CPython worker creation failed:", result.stdout)
        self.assertIn("PASS: guarded CPython launcher", result.stdout)

    def test_registration_uses_small_launcher_stack(self) -> None:
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        kconfig = (PYTHON_DIR / "Kconfig").read_text(encoding="utf-8")

        self.assertIn(
            "STACKSIZE += $(CONFIG_INTERPRETERS_CPYTHON_LAUNCHER_STACKSIZE)",
            makefile,
        )
        self.assertIn("MAINSRC += python_launcher.c", makefile)
        self.assertIn("CSRCS   += python_wrapper.c", makefile)
        self.assertIn(
            "-p2-externalize-constant-data([[:space:]]|$$)/\\1/g'",
            makefile,
        )
        self.assertIn("config INTERPRETERS_CPYTHON_LAUNCHER_STACKSIZE", kconfig)
        self.assertIn("depends on !DISABLE_PTHREAD", kconfig)
        self.assertIn("default 2048", kconfig)

    def test_romfs_mount_state_retries_without_procfs(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        wrapper_config = CONFIG_HEADER + """\
#define CONFIG_FS_ROMFS 1
#define CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS 1
#define CONFIG_INTERPRETERS_CPYTHON_PYTHONPATH "/tmp"
#define OK 0
"""

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            include = temp / "include"
            (include / "nuttx" / "drivers").mkdir(parents=True)
            (include / "sys").mkdir(parents=True)
            (include / "nuttx" / "config.h").write_text(
                wrapper_config, encoding="utf-8"
            )
            (include / "nuttx" / "debug.h").write_text(
                DEBUG_HEADER, encoding="utf-8"
            )
            (include / "nuttx" / "drivers" / "ramdisk.h").write_text(
                RAMDISK_HEADER, encoding="utf-8"
            )
            (include / "sys" / "boardctl.h").write_text(
                BOARDCTL_HEADER, encoding="utf-8"
            )
            (include / "sys" / "mount.h").write_text(
                MOUNT_HEADER, encoding="utf-8"
            )
            (include / "Python.h").write_text(PYTHON_HEADER, encoding="utf-8")

            executable = temp / "python-wrapper-test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{include}",
                    str(WRAPPER),
                    str(WRAPPER_HARNESS),
                    "-o",
                    str(executable),
                ],
                check=True,
                text=True,
            )
            result = subprocess.run(
                [str(executable)],
                check=True,
                capture_output=True,
                text=True,
            )

        wrapper_source = WRAPPER.read_text(encoding="utf-8")
        self.assertNotIn("/proc/fs/mount", wrapper_source)
        self.assertIn("g_cpython_romfs_mounted", wrapper_source)
        self.assertIn(
            "PASS: persistent CPython ROMFS mount state", result.stdout
        )


if __name__ == "__main__":
    unittest.main()
