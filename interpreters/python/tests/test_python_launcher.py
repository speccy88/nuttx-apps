#!/usr/bin/env python3
"""Host regression test for the guarded CPython command launcher."""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


PYTHON_DIR = pathlib.Path(__file__).resolve().parents[1]
LAUNCHER = PYTHON_DIR / "python_launcher.c"
HARNESS = pathlib.Path(__file__).with_name("launcher_harness.c")
WRAPPER = PYTHON_DIR / "python_wrapper.c"
WRAPPER_HARNESS = pathlib.Path(__file__).with_name("wrapper_harness.c")

CONFIG_HEADER = """\
#ifndef __NUTTX_CONFIG_H
#define __NUTTX_CONFIG_H
#define FAR
#define CONFIG_ARCH_P2 1
#define CONFIG_STACK_COLORATION 1
#define CONFIG_INTERPRETERS_CPYTHON_STACKSIZE 65536
#endif
"""

ARCH_HEADER = """\
#ifndef __NUTTX_ARCH_H
#define __NUTTX_ARCH_H
#include <stddef.h>
struct tcb_s;
size_t up_check_tcbstack(struct tcb_s *tcb, size_t check_size);
#endif
"""

SCHED_HEADER = """\
#ifndef __NUTTX_SCHED_H
#define __NUTTX_SCHED_H
#include <stddef.h>
struct tcb_s
{
  size_t adj_stack_size;
};
struct tcb_s *nxsched_self(void);
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
            (include / "arch.h").write_text(ARCH_HEADER, encoding="utf-8")
            (include / "sched.h").write_text(SCHED_HEADER, encoding="utf-8")

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
        for code in (37, 19, 23):
            self.assertIn(
                "P2PY:WORKER:EXIT:CODE={}".format(code), result.stdout
            )
        stack_marker = "P2PY:WORKER:STACK:FREE=57344:SIZE=65536"
        self.assertIn(stack_marker, result.stdout)
        self.assertLess(
            result.stdout.index("P2PY:WORKER:EXIT:CODE=37"),
            result.stdout.index(stack_marker),
        )
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
        self.assertIn(
            "config INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY", kconfig
        )
        self.assertIn(
            "config INTERPRETERS_CPYTHON_P2_DEFAULT_NO_SITE", kconfig
        )
        self.assertIn(
            "config INTERPRETERS_CPYTHON_P2_FIXED_PATH_CONFIG", kconfig
        )
        launcher = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("p2_overlay_get_stats", launcher)
        self.assertIn("g_python_overlay_telemetry_ready", launcher)
        self.assertIn("nxsem_wait_uninterruptible", launcher)
        self.assertIn("nxsem_tickwait_uninterruptible", launcher)
        self.assertIn('python_overlay_report("LAUNCH")', launcher)
        self.assertIn('python_overlay_report("SAMPLE")', launcher)
        self.assertIn('python_overlay_report("FINAL")', launcher)
        self.assertIn("p2_psram_get_cache_stats", launcher)
        self.assertIn("P2PY:XMEM:%s:H=%016llX", launcher)
        self.assertIn("p2_overlay_get_hot_snapshot", launcher)
        self.assertIn('python_overlay_report_hot("SAMPLE")', launcher)
        self.assertIn('python_overlay_report_hot("FINAL")', launcher)
        wrapper = WRAPPER.read_text(encoding="utf-8")
        ready = wrapper.index("python_overlay_telemetry_start()")
        begin = wrapper.index('python_overlay_report("BEGIN")')
        run = wrapper.index("ret = py_bytesmain(argc, argv)", begin)
        end = wrapper.index('python_overlay_report("END")', run)
        self.assertLess(ready, begin)
        self.assertLess(begin, run)
        self.assertLess(run, end)
        self.assertIn("depends on !DISABLE_PTHREAD", kconfig)
        self.assertIn("default 2048", kconfig)

    def test_overlay_hot_records_are_bounded_and_deterministic(self) -> None:
        launcher = LAUNCHER.read_text(encoding="utf-8")
        kconfig = (PYTHON_DIR / "Kconfig").read_text(encoding="utf-8")
        hot = launcher[
            launcher.index("static void python_overlay_report_hot") :
            launcher.index("#endif", launcher.index("static void python_overlay_report_hot"))
        ]

        self.assertIn("struct p2_overlay_hot_snapshot_s snapshot", hot)
        self.assertIn("snapshot.capacity != P2_OVERLAY_HOT_CAPACITY", hot)
        self.assertIn("snapshot.used > snapshot.capacity", hot)
        self.assertIn('P2PY:HOT:%s:N=%02lX:T=%016llX', hot)
        for field in (
            "R=%02lX",
            "CG=%08lX",
            "CO=%08lX",
            "TG=%08lX",
            "TS=%08lX",
            "C=%016llX",
            "E=%016llX",
        ):
            self.assertIn(field, hot)

        self.assertIn("&snapshot.entries[index]", hot)
        self.assertNotIn("python_overlay_hot_precedes", launcher)
        self.assertNotIn("Insertion sort", hot)
        self.assertNotIn("p2_psram_get_cache_stats", hot)
        self.assertNotIn("P2PY:XMEM", hot)

        interval = kconfig[
            kconfig.index(
                "config INTERPRETERS_CPYTHON_P2_OVERLAY_TELEMETRY_INTERVAL_MS"
            ) :
            kconfig.index(
                "config INTERPRETERS_CPYTHON_P2_DEFAULT_NO_SITE"
            )
        ]
        self.assertIn("default 60000", interval)
        self.assertIn("range 1000 60000", interval)
        self.assertIn("P2PY:HOT", interval)

    def test_p2_default_no_site_uses_pyconfig_without_argv_rewrite(self) -> None:
        patch_name = "0045-p2-add-default-no-site-config-hook.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        kconfig = (PYTHON_DIR / "Kconfig").read_text(encoding="utf-8")
        wrapper = WRAPPER.read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0044-p2-trace-fill-time-softfloat-boundaries.patch"),
            makefile.index(patch_name),
        )
        self.assertIn("depends on ARCH_P2", kconfig)
        self.assertIn("default n", kconfig)
        self.assertIn("sets sys.flags.no_site", kconfig)

        declaration = wrapper.index("void py_p2_set_default_no_site(void);")
        call = wrapper.index("py_p2_set_default_no_site();", declaration)
        run = wrapper.index("ret = py_bytesmain(argc, argv);", call)
        self.assertLess(declaration, call)
        self.assertLess(call, run)
        self.assertEqual(wrapper.count("py_p2_set_default_no_site();"), 1)
        self.assertNotIn("argv[", wrapper[call:run])

        removed = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line != "-- "
        ]
        self.assertEqual(removed, [])
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn('section(".p2.hub.data")', added)
        self.assertIn("_Py_P2_HUB_RESIDENT void", added)
        self.assertIn("config.site_import = 0;", added)
        self.assertNotIn("bytes_argv", added)
        self.assertNotIn("wchar_argv", added)
        self.assertNotIn('"-S"', added)

        guard = "#if defined(__NuttX__) && defined(__propeller2__)"
        depth = 0
        for line in added.splitlines():
            stripped = line.strip()
            if not stripped:
                continue
            if stripped == guard:
                depth += 1
            elif stripped == "#endif":
                depth -= 1
            else:
                self.assertGreater(depth, 0, stripped)
        self.assertEqual(depth, 0)

        # CPython's standard no-site configuration exposes the intended
        # observable flag while leaving explicit site processing available.

        result = subprocess.run(
            [
                sys.executable,
                "-S",
                "-c",
                "import sys;assert sys.flags.no_site==1;"
                "assert 'site' not in sys.modules;import site;site.main()",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_p2_fixed_path_config_is_opt_in_and_skips_frozen_getpath(self) -> None:
        patch_name = "0046-p2-add-fixed-path-config-fast-path.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        kconfig = (PYTHON_DIR / "Kconfig").read_text(encoding="utf-8")
        wrapper = WRAPPER.read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0045-p2-add-default-no-site-config-hook.patch"),
            makefile.index(patch_name),
        )
        option = kconfig.index(
            "config INTERPRETERS_CPYTHON_P2_FIXED_PATH_CONFIG"
        )
        next_option = kconfig.index("\nconfig ", option + 1)
        option_text = kconfig[option:next_option]
        self.assertIn("default n", option_text)
        self.assertIn("depends on ARCH_P2", option_text)
        self.assertIn("exact layout", option_text)

        declaration = wrapper.index(
            "void py_p2_set_fixed_path_config(FAR const wchar_t *writable_path);"
        )
        call = wrapper.index("py_p2_set_fixed_path_config(", declaration + 1)
        run = wrapper.index("ret = py_bytesmain(argc, argv);", call)
        self.assertLess(declaration, call)
        self.assertLess(call, run)
        self.assertIn(
            "CPYTHON_WIDEN_LITERAL(CONFIG_INTERPRETERS_CPYTHON_PYTHONPATH)",
            wrapper[call:run],
        )

        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn(
            "defined(CONFIG_INTERPRETERS_CPYTHON_P2_FIXED_PATH_CONFIG)",
            added,
        )
        self.assertIn("p2_init_fixed_path_config(config)", added)
        self.assertIn("config->module_search_paths_set = 1;", added)
        self.assertIn("p2_fixed_pythonpath = writable_path;", added)
        self.assertIn('L"/usr/local/lib/python"', added)
        self.assertEqual(added.count('printf("P2PY:PATHCONFIG:BEGIN\\n");'), 1)
        self.assertEqual(added.count('printf("P2PY:PATHCONFIG:PASS\\n");'), 1)
        self.assertEqual(added.count('printf("P2PY:PATHCONFIG:FAIL\\n");'), 1)
        self.assertIn("if (p2_fixed_pythonpath)", added)
        self.assertNotIn("PyEval_EvalCode", added)

    def test_p2_static_unicode_hot_edge_is_inlined_in_startup_group(self) -> None:
        patch_name = "0047-p2-inline-static-unicode-compare-hot-edge.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0046-p2-add-fixed-path-config-fast-path.patch"),
            makefile.index(patch_name),
        )
        self.assertIn("at least 1,553 calls", patch)
        self.assertIn("static inline Py_ALWAYS_INLINE int", added)
        self.assertIn("p2_unicode_compare_eq_inline", added)
        self.assertEqual(
            added.count("return p2_unicode_compare_eq_inline"), 2
        )
        self.assertIn(
            "bytes = (size_t)len << (kind >> 1);",
            added,
        )
        self.assertNotIn("PyObject_ClearManagedDict", patch)
        self.assertNotIn("len * kind", added)
        self.assertNotIn("_Py_P2_HUB_RESIDENT", added)

    def test_p2_exact_tuple_iterator_hot_edge_uses_shared_inline_core(self) -> None:
        patch_name = "0048-p2-inline-exact-tuple-iterator-hot-edge.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        removed = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line != "-- "
        )

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0047-p2-inline-static-unicode-compare-hot-edge.patch"),
            makefile.index(patch_name),
        )
        self.assertIn("at least 290 certain transitions", patch)
        self.assertIn("at least 12,821,480 overlay bytes", patch)
        self.assertIn("static inline Py_ALWAYS_INLINE PyObject *", added)
        self.assertEqual(added.count("_PyP2_TupleIterNextInline("), 3)
        self.assertIn("Py_IS_TYPE(iter, &PyTupleIter_Type)", added)
        self.assertIn(
            "result = (*Py_TYPE(iter)->tp_iternext)(iter);",
            added,
        )
        self.assertIn("result = _PyP2_TupleIterNextInline", added)
        self.assertIn("return _PyP2_TupleIterNextInline(it);", added)
        self.assertIn("item = PyTuple_GET_ITEM(seq, it->it_index);", added)
        self.assertIn("++it->it_index;", added)
        self.assertIn("it->it_seq = NULL;", added)
        self.assertIn("Py_DECREF(seq);", added)
        self.assertNotIn("_PyErr_Clear", removed)
        self.assertNotIn("p2_hub_overlay(177)", patch)
        self.assertNotIn("_Py_P2_HUB_RESIDENT", added)

    def test_p2_main_interpreter_subphase_trace_is_ordered_and_guarded(self) -> None:
        patch_name = "0049-p2-trace-main-interpreter-subphases.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added_lines = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ]
        removed_lines = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line != "-- "
        ]
        added = "\n".join(added_lines)

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0048-p2-inline-exact-tuple-iterator-hot-edge.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(
            re.findall(r"^diff --git a/(\S+) b/\S+$", patch, re.MULTILINE),
            ["Python/pylifecycle.c"],
        )
        self.assertEqual(removed_lines, [])

        expected_markers = [
            "P2PY:MAINSTEP:BEGIN",
            "P2PY:MAINSTEP:CONFIG:PASS",
            "P2PY:MAINSTEP:EXTERNAL:PASS",
            "P2PY:MAINSTEP:ENCODINGS:PASS",
            "P2PY:MAINSTEP:STREAMS:PASS",
            "P2PY:MAINSTEP:BUILTINS_OPEN:PASS",
            "P2PY:MAINSTEP:MAIN_MODULE:PASS",
        ]
        self.assertEqual(
            re.findall(r'printf\("(P2PY:MAINSTEP:[A-Z_:]+)\\n"\);', added),
            expected_markers,
        )
        self.assertEqual(added.count("P2PY:MAINSTEP:"), 7)
        self.assertNotIn("P2PY:MAIN:PASS", added)

        guard = "#if defined(__NuttX__) && defined(__propeller2__)"
        self.assertEqual(added.count(guard), len(expected_markers))
        self.assertEqual(added.count("#endif"), len(expected_markers))
        guard_depth = 0
        for line in added_lines:
            stripped = line.strip()
            if stripped == guard:
                guard_depth += 1
            elif stripped == "#endif":
                guard_depth -= 1
            else:
                self.assertEqual(guard_depth, 1, stripped)
                self.assertRegex(stripped, r'^printf\("P2PY:MAINSTEP:')
        self.assertEqual(guard_depth, 0)

    def test_p2_marshal_reference_append_inlines_only_the_fast_path(self) -> None:
        patch_name = "0050-p2-inline-marshal-reference-list-append.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        removed = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line not in ("-- ", "--")
        ]

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0049-p2-trace-main-interpreter-subphases.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(
            re.findall(r"^diff --git a/(\S+) b/\S+$", patch, re.MULTILINE),
            ["Python/marshal.c"],
        )
        self.assertEqual(removed, [])
        self.assertIn("at least 468 transitions", patch)
        self.assertIn("at least 25,127,856 overlay bytes", patch)
        self.assertIn("from 0x5468 to 0x5584 (284 bytes)", patch)
        self.assertIn("68,220 bytes of slot headroom", patch)
        self.assertIn("growth-boundary appends can still cross overlays", patch)

        guard = "#if defined(__NuttX__) && defined(__propeller2__)"
        self.assertEqual(added.count(guard), 3)
        self.assertEqual(added.count("#endif"), 3)
        self.assertEqual(added.count("#else"), 2)
        self.assertEqual(added.count('include "pycore_list.h"'), 1)
        self.assertEqual(added.count("err = _PyList_AppendTakeRef("), 2)
        self.assertEqual(
            added.count("Py_BEGIN_CRITICAL_SECTION(p->refs);"), 2
        )
        self.assertEqual(added.count("Py_END_CRITICAL_SECTION();"), 2)
        self.assertEqual(added.count("if (err < 0)"), 2)
        self.assertEqual(added.count("Py_NewRef(Py_None)"), 1)
        self.assertEqual(added.count("Py_NewRef(o)"), 1)
        self.assertNotIn("_PyList_AppendTakeRefListResize(", added)
        self.assertNotIn("p2_hub_overlay", added)
        self.assertNotIn("_Py_P2_HUB_RESIDENT", added)

        reserve_start = patch.index("@@ -953")
        direct_start = patch.index("@@ -992", reserve_start)
        reserve_patch = patch[reserve_start:direct_start]
        direct_patch = patch[direct_start:]

        def patched_hunk(text: str) -> str:
            return "\n".join(
                line[1:]
                for line in text.splitlines()
                if line.startswith((" ", "+"))
                and not line.startswith("+++")
            )

        reserve = patched_hunk(reserve_patch)
        direct = patched_hunk(direct_patch)
        self.assertIn(
            "#else\n"
            "        if (PyList_Append(p->refs, Py_None) < 0)\n"
            "#endif",
            reserve,
        )
        self.assertIn(
            "#else\n"
            "    if (PyList_Append(p->refs, o) < 0) {\n"
            "#endif",
            direct,
        )
        self.assertIn("Py_DECREF(o); /* release the new object */", direct)

        for block, new_ref in (
            (reserve, "Py_NewRef(Py_None)"),
            (direct, "Py_NewRef(o)"),
        ):
            with self.subTest(new_ref=new_ref):
                begin = block.index("Py_BEGIN_CRITICAL_SECTION(p->refs);")
                take = block.index("_PyList_AppendTakeRef(", begin)
                reference = block.index(new_ref, take)
                end = block.index("Py_END_CRITICAL_SECTION();", reference)
                branch = block.index("if (err < 0)", end)
                self.assertLess(begin, take)
                self.assertLess(take, reference)
                self.assertLess(reference, end)
                self.assertLess(end, branch)
                self.assertNotIn("return ", block[begin:end])

    def test_p2_marshal_ascii_uses_utf8_with_high_byte_fallback(self) -> None:
        patch_name = "0051-p2-use-utf8-decoder-for-marshal-ascii.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added_lines = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ]
        removed_lines = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line not in ("-- ", "--")
        ]
        added = "\n".join(added_lines)

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0050-p2-inline-marshal-reference-list-append.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(
            re.findall(r"^diff --git a/(\S+) b/\S+$", patch, re.MULTILINE),
            ["Python/marshal.c"],
        )
        self.assertEqual(removed_lines, [])
        self.assertEqual(
            added.count("#if defined(__NuttX__) && defined(__propeller2__)"),
            2,
        )
        self.assertIn("static inline Py_ALWAYS_INLINE int", added)
        self.assertIn("p2_marshal_is_ascii(const char *data, Py_ssize_t size)", added)
        self.assertIn("if ((unsigned char)*data >= 0x80)", added)
        self.assertIn("return 0;", added)
        self.assertIn("return 1;", added)
        self.assertEqual(added.count("PyUnicode_DecodeUTF8Stateful("), 1)
        self.assertNotIn("_Py_P2_HUB_RESIDENT", added)
        self.assertNotIn("p2_hub_overlay", added)

        dispatch = patch[patch.index("@@ -1204") :]
        patched_dispatch = "\n".join(
            line[1:]
            for line in dispatch.splitlines()
            if line.startswith((" ", "+"))
            and not line.startswith("+++")
        )
        expected_dispatch = (
            "if (p2_marshal_is_ascii(ptr, n)) {\n"
            "                v = PyUnicode_DecodeUTF8Stateful(ptr, n, NULL, NULL);\n"
            "            }\n"
            "            else\n"
            "#endif\n"
            "            {\n"
            "            v = PyUnicode_FromKindAndData("
        )
        self.assertIn(expected_dispatch, patched_dispatch)
        self.assertLess(
            patched_dispatch.index("p2_marshal_is_ascii(ptr, n)"),
            patched_dispatch.index("PyUnicode_DecodeUTF8Stateful("),
        )
        self.assertLess(
            patched_dispatch.index("PyUnicode_DecodeUTF8Stateful("),
            patched_dispatch.index("PyUnicode_FromKindAndData("),
        )
        self.assertIn("TYPE_ASCII and TYPE_SHORT_ASCII", patch)
        self.assertIn("converge at _read_ascii", patch)

    def test_p2_posix_constdef_comparator_pin_has_exact_fit_evidence(self) -> None:
        patch_name = "0052-p2-pin-posix-constdef-comparator.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        removed = [
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-")
            and not line.startswith("---")
            and line not in ("-- ", "--")
        ]

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0051-p2-use-utf8-decoder-for-marshal-ascii.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(
            re.findall(r"^diff --git a/(\S+) b/\S+$", patch, re.MULTILINE),
            ["Modules/posixmodule.c"],
        )
        self.assertEqual(removed, [])
        self.assertEqual(added.strip(), "_Py_P2_HUB_RESIDENT")
        self.assertEqual(patch.count("_Py_P2_HUB_RESIDENT"), 1)
        for evidence in (
            "0x30-byte cmp_constdefs overlay body",
            "0x4-byte resident entry stub",
            "436 bytes of raw staging",
            "adds exactly 44 bytes",
            "leaves 392 bytes",
        ):
            self.assertIn(evidence, patch)
        self.assertIn("cmp_constdefs(const void *v1", patch)
        self.assertNotIn("p2_hub_overlay", added)

    def test_p2_frozen_startup_encodings_are_exact_and_idempotent(self) -> None:
        generator_patch_name = "0053-p2-freeze-startup-encodings.patch"
        pcbuild_patch_name = "0054-p2-record-frozen-encoding-pcbuild.patch"
        generator_patch = (
            PYTHON_DIR / "patch" / generator_patch_name
        ).read_text(encoding="utf-8")
        pcbuild_patch = (
            PYTHON_DIR / "patch" / pcbuild_patch_name
        ).read_text(encoding="utf-8")
        patch_attributes = (
            PYTHON_DIR / "patch" / ".gitattributes"
        ).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        combined_patch = generator_patch + pcbuild_patch

        for patch_name in (generator_patch_name, pcbuild_patch_name):
            self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0052-p2-pin-posix-constdef-comparator.patch"),
            makefile.index(generator_patch_name),
        )
        self.assertLess(
            makefile.index(generator_patch_name),
            makefile.index(pcbuild_patch_name),
        )
        conditional = makefile.index(
            "ifeq ($(CONFIG_ARCH_P2),y)",
            makefile.index("0052-p2-pin-posix-constdef-comparator.patch"),
        )
        patch_block = makefile[
            conditional : makefile.index("\t$(Q) touch $@", conditional)
        ]
        self.assertIn(generator_patch_name, patch_block)
        self.assertIn(pcbuild_patch_name, patch_block)
        self.assertTrue(patch_block.rstrip().endswith("endif"))
        self.assertIn(
            "P2_CPYTHON_REGEN_ENV=NUTTX_P2_FREEZE_STARTUP_ENCODINGS=1",
            makefile,
        )
        self.assertIn(
            "$(Q) $(P2_CPYTHON_REGEN_ENV) $(MAKE) -C $(TARGETBUILD)",
            makefile,
        )

        expected_specs = (
            "'<encodings>',",
            "'encodings.aliases',",
            "'encodings.utf_8',",
        )
        for spec in expected_specs:
            self.assertEqual(generator_patch.count(spec), 1)
        self.assertNotIn("encodings.ascii", combined_patch)
        self.assertEqual(
            patch_attributes.strip(),
            f"{pcbuild_patch_name} -text",
        )
        self.assertIn("20,587 bytes", generator_patch)
        self.assertIn("0x10000000..0x11ffffff", generator_patch)
        self.assertEqual(
            set(re.findall(r"^diff --git a/(\S+) b/\S+$", combined_patch,
                           re.MULTILINE)),
            {
                "Tools/build/freeze_modules.py",
                "Makefile.pre.in",
                "Python/frozen.c",
                "PCbuild/_freeze_module.vcxproj",
                "PCbuild/_freeze_module.vcxproj.filters",
            },
        )

        patch_tool = shutil.which("patch")
        archive = PYTHON_DIR / "v3.13.0.zip"
        if patch_tool is None:
            self.skipTest("patch utility is unavailable")
        if not archive.is_file():
            self.skipTest("cached CPython v3.13.0 source archive is unavailable")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            with zipfile.ZipFile(archive) as source_archive:
                roots = {
                    name.split("/", 1)[0]
                    for name in source_archive.namelist()
                    if "/" in name
                }
                self.assertEqual(len(roots), 1)
                source_archive.extractall(temp)
            source_root = temp / roots.pop()

            for patch_name in (generator_patch_name, pcbuild_patch_name):
                result = subprocess.run(
                    [
                        patch_tool,
                        "--batch",
                        "--forward",
                        "-p1",
                        "-i",
                        str(PYTHON_DIR / "patch" / patch_name),
                    ],
                    cwd=source_root,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    f"failed to apply {patch_name}\n{result.stdout}\n{result.stderr}",
                )

            generated_paths = [
                source_root / "Makefile.pre.in",
                source_root / "Python" / "frozen.c",
                source_root / "PCbuild" / "_freeze_module.vcxproj",
                source_root / "PCbuild" / "_freeze_module.vcxproj.filters",
            ]
            before = {path: path.read_bytes() for path in generated_paths}
            base_env = os.environ.copy()
            base_env.pop("NUTTX_P2_FREEZE_STARTUP_ENCODINGS", None)
            p2_env = base_env.copy()
            p2_env["NUTTX_P2_FREEZE_STARTUP_ENCODINGS"] = "1"
            generator = source_root / "Tools" / "build" / "freeze_modules.py"
            subprocess.run(
                [sys.executable, str(generator)],
                cwd=source_root,
                env=p2_env,
                check=True,
                capture_output=True,
                text=True,
            )
            after = {path: path.read_bytes() for path in generated_paths}
            self.assertEqual(after, before)

            probe = (
                "import importlib.util, json, pathlib, sys; "
                "p = pathlib.Path(sys.argv[1]); "
                "sys.path.insert(0, str(p.parent)); "
                "s = importlib.util.spec_from_file_location('freeze_probe', p); "
                "m = importlib.util.module_from_spec(s); "
                "s.loader.exec_module(m); "
                "print(json.dumps([item.name for item in m.parse_frozen_specs()]))"
            )
            generic = subprocess.run(
                [sys.executable, "-c", probe, str(generator)],
                cwd=source_root,
                env=base_env,
                check=True,
                capture_output=True,
                text=True,
            )
            p2 = subprocess.run(
                [sys.executable, "-c", probe, str(generator)],
                cwd=source_root,
                env=p2_env,
                check=True,
                capture_output=True,
                text=True,
            )
            generic_names = set(json.loads(generic.stdout))
            p2_names = set(json.loads(p2.stdout))
            self.assertEqual(
                p2_names - generic_names,
                {"encodings", "encodings.aliases", "encodings.utf_8"},
            )
            self.assertNotIn("encodings.ascii", p2_names)

    def test_cpython_patch_stamp_tracks_architecture_variant(self) -> None:
        make_tool = shutil.which("make")
        if make_tool is None:
            self.skipTest("make utility is unavailable")

        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        def assignment(name: str) -> str:
            match = re.search(rf"^{name}\s*=.*$", makefile, re.MULTILINE)
            if match is None:
                self.fail(f"missing {name} assignment")
            return match.group(0)

        unpack_assignment = assignment("CPYTHON_UNPACKNAME")
        variant_assignment = assignment("CPYTHON_PATCH_VARIANT")
        stamp_assignment = assignment("CPYTHON_PATCH_STAMP")
        self.assertIn("$(filter y,$(CONFIG_ARCH_P2))", variant_assignment)
        self.assertIn("$(CPYTHON_PATCH_VARIANT)", stamp_assignment)

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = pathlib.Path(temp_dir)
            fixture = temp / "Makefile"
            fixture.write_text(
                "\n".join(
                    (
                        "DELIM = /",
                        "CONFIG_ARCH_P2 ?=",
                        unpack_assignment,
                        variant_assignment,
                        stamp_assignment,
                        ".PHONY: all",
                        "all: $(CPYTHON_PATCH_STAMP)",
                        "",
                        "$(CPYTHON_PATCH_STAMP):",
                        "\t@$(RM) -r $(CPYTHON_UNPACKNAME)",
                        "\t@mkdir -p $(CPYTHON_UNPACKNAME)",
                        "\t@printf '%s\\n' '$(CPYTHON_PATCH_VARIANT)' > $@",
                        "\t@printf '%s\\n' '$(CPYTHON_PATCH_VARIANT)' >> rebuilds.txt",
                        "",
                    )
                ),
                encoding="utf-8",
            )

            def run_make(arch_p2: str | None) -> None:
                command = [make_tool, "-s", "-f", str(fixture)]
                if arch_p2 is not None:
                    command.append(f"CONFIG_ARCH_P2={arch_p2}")
                command.append("all")
                subprocess.run(
                    command,
                    cwd=temp,
                    check=True,
                    capture_output=True,
                    text=True,
                )

            def stamps() -> list[str]:
                return sorted(
                    path.name
                    for path in (temp / "Python").glob(".nuttx-patched-*")
                )

            run_make(None)
            self.assertEqual(stamps(), [".nuttx-patched-generic"])
            run_make("y")
            self.assertEqual(stamps(), [".nuttx-patched-p2"])
            run_make("y")
            run_make("n")
            self.assertEqual(stamps(), [".nuttx-patched-generic"])
            self.assertEqual(
                (temp / "rebuilds.txt").read_text(encoding="utf-8").splitlines(),
                ["generic", "p2", "generic"],
            )

    def test_compressed_stdlib_has_builtin_zlib_bootstrap(self) -> None:
        setup = (PYTHON_DIR / "Setup.local.in").read_text(encoding="utf-8")
        active = []
        disabled = []
        destination = active

        for raw_line in setup.splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line == "*disabled*":
                destination = disabled
                continue
            destination.append(line.split()[0])

        self.assertEqual(active.count("zlib"), 1)
        self.assertNotIn("zlib", disabled)
        self.assertIn('@echo "_thread" >> $@',
                      (PYTHON_DIR / "Makefile").read_text(encoding="utf-8"))
        self.assertIn('@echo "_interpreters" >> $@',
                      (PYTHON_DIR / "Makefile").read_text(encoding="utf-8"))

    def test_threadless_importlib_uses_dummy_module_lock(self) -> None:
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        patch = (
            PYTHON_DIR
            / "patch"
            / "0040-allow-importlib-without-thread-module.patch"
        ).read_text(encoding="utf-8")

        previous = makefile.index(
            "0039-p2-trace-importlib-bootstrap-steps.patch"
        )
        fallback = makefile.index(
            "0040-allow-importlib-without-thread-module.patch"
        )
        self.assertLess(previous, fallback)

        self.assertIn(
            "for builtin_name in ('_thread', '_warnings', '_weakref'):",
            patch,
        )
        self.assertIn("builtin_name == '_thread'", patch)
        self.assertIn(
            "BuiltinImporter.find_spec(builtin_name) is None", patch
        )
        self.assertIn("+            continue", patch)
        self.assertNotIn("except ImportError", patch)
        self.assertLess(
            patch.index("builtin_name == '_thread'"),
            patch.index("if builtin_name not in sys.modules:"),
        )
        self.assertIn("path module preloaded under this name", patch)
        self.assertIn("if _thread is None:", patch)
        self.assertIn("lock = _DummyModuleLock(name)", patch)
        self.assertIn("lock = _ModuleLock(name)", patch)

    def test_lock_only_thread_is_a_pure_stdlib_zip_compatibility_layer(self) -> None:
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")
        patch = (
            PYTHON_DIR
            / "patch"
            / "0041-add-lock-only-thread-compatibility.patch"
        ).read_text(encoding="utf-8")

        self.assertLess(
            makefile.index("0040-allow-importlib-without-thread-module.patch"),
            makefile.index("0041-add-lock-only-thread-compatibility.patch"),
        )
        self.assertIn("create mode 100644 Lib/_thread.py", patch)
        self.assertIn("_NUTTX_LOCK_ONLY = True", patch)
        self.assertIn("class LockType:", patch)
        self.assertIn("def allocate_lock():", patch)
        self.assertIn("allocate = allocate_lock", patch)
        self.assertIn("class RLock:", patch)
        self.assertIn("def get_ident():", patch)
        for entry in (
            "def start_new_thread(function, args, kwargs=None):",
            "start_new = start_new_thread",
            "def start_joinable_thread(function, handle=None, daemon=True):",
        ):
            self.assertIn(entry, patch)
        self.assertEqual(patch.count("raise NotImplementedError"), 2)
        self.assertIn('if getattr(_thread, "_NUTTX_LOCK_ONLY", False):', patch)
        self.assertIn("threading is unavailable:", patch)
        self.assertIn('-    "_pyio.py",', patch)
        for forbidden in (
            "PyInit__thread",
            "_threadmodule.c",
            "p2_hub_resident",
            "p2_hub_overlay",
            ".p2.overlay.stubs",
            "__p2_ovlbody",
        ):
            self.assertNotIn(forbidden, patch)

    def test_p2_startup_translation_units_have_overlay_affinity(self) -> None:
        patch = (
            PYTHON_DIR
            / "patch"
            / "0023-p2-co-locate-cpython-startup-overlays.patch"
        ).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertIn(
            "0023-p2-co-locate-cpython-startup-overlays.patch", makefile
        )
        self.assertIn("CPYTHON_PATCHES = $(wildcard patch$(DELIM)*.patch)", makefile)
        self.assertIn("CPYTHON_PATCH_STAMP =", makefile)
        self.assertIn(
            "$(CPYTHON_PATCH_STAMP): $(CPYTHON_ZIP) $(CPYTHON_PATCHES)",
            makefile,
        )
        self.assertIn("$(HOSTPYTHON): $(CPYTHON_PATCH_STAMP)", makefile)
        self.assertIn("touch $@", makefile)
        self.assertEqual(patch.count("p2_hub_overlay(4)"), 5)
        self.assertEqual(patch.count("p2_hub_resident"), 1)
        self.assertGreaterEqual(patch.count("_Py_P2_HUB_RESIDENT"), 56)
        self.assertNotIn("p2_hub_overlay(5)", patch)
        self.assertIn(
            "pin the allocator frontends and startup-hot thread services", patch
        )
        for source in (
            "--- a/Include/pymem.h",
            "--- a/Include/objimpl.h",
            "--- a/Objects/obmalloc.c",
            "--- a/Python/thread.c",
            "--- a/Python/thread_pthread.h",
        ):
            self.assertIn(source, patch)
        for declaration_pin in (
            "PyMem_Malloc(size_t size) _Py_P2_HUB_RESIDENT;",
            "PyMem_Calloc(size_t nelem, size_t elsize) _Py_P2_HUB_RESIDENT;",
            "PyMem_Realloc(void *ptr, size_t new_size) _Py_P2_HUB_RESIDENT;",
            "PyMem_Free(void *ptr) _Py_P2_HUB_RESIDENT;",
            "PyObject_Malloc(size_t size) _Py_P2_HUB_RESIDENT;",
            "PyObject_Calloc(size_t nelem, size_t elsize) _Py_P2_HUB_RESIDENT;",
            "PyObject_Realloc(void *ptr, size_t new_size) _Py_P2_HUB_RESIDENT;",
            "PyObject_Free(void *ptr) _Py_P2_HUB_RESIDENT;",
        ):
            self.assertIn(declaration_pin, patch)
        for definition_pin in (
            "+_Py_P2_HUB_RESIDENT void *\n _PyMem_RawMalloc(",
            "+_Py_P2_HUB_RESIDENT void *\n _PyMem_RawCalloc(",
            "+_Py_P2_HUB_RESIDENT void *\n _PyMem_RawRealloc(",
            "+_Py_P2_HUB_RESIDENT void\n _PyMem_RawFree(",
            "+_Py_P2_HUB_RESIDENT static int\n set_default_allocator_unlocked(",
            "+_Py_P2_HUB_RESIDENT int\n _PyMem_SetDefaultAllocator(",
            "+_Py_P2_HUB_RESIDENT void\n PyMem_SetAllocator(",
            "+_Py_P2_HUB_RESIDENT void *\n PyMem_RawMalloc(",
            "+_Py_P2_HUB_RESIDENT void *\n PyMem_RawCalloc(",
            "+_Py_P2_HUB_RESIDENT void*\n PyMem_RawRealloc(",
            "+_Py_P2_HUB_RESIDENT void PyMem_RawFree(void *ptr)",
            "+_Py_P2_HUB_RESIDENT wchar_t*\n _PyMem_RawWcsdup(",
            "+_Py_P2_HUB_RESIDENT char *\n _PyMem_RawStrdup(",
            "+_Py_P2_HUB_RESIDENT void *\n PyMem_Malloc(",
            "+_Py_P2_HUB_RESIDENT void *\n PyMem_Calloc(",
            "+_Py_P2_HUB_RESIDENT void *\n PyMem_Realloc(",
            "+_Py_P2_HUB_RESIDENT void\n PyMem_Free(",
            "+_Py_P2_HUB_RESIDENT void *\n PyObject_Malloc(",
            "+_Py_P2_HUB_RESIDENT void *\n PyObject_Calloc(",
            "+_Py_P2_HUB_RESIDENT void *\n PyObject_Realloc(",
            "+_Py_P2_HUB_RESIDENT void\n PyObject_Free(",
            "+_Py_P2_HUB_RESIDENT PyThread_ident_t\n PyThread_get_thread_ident_ex(",
            "+_Py_P2_HUB_RESIDENT unsigned long\n PyThread_get_thread_ident(",
            "+_Py_P2_HUB_RESIDENT PyThread_type_lock\n PyThread_allocate_lock(",
            "+_Py_P2_HUB_RESIDENT void\n PyThread_free_lock(",
            "+_Py_P2_HUB_RESIDENT Py_tss_t *\n PyThread_tss_alloc(",
            "+_Py_P2_HUB_RESIDENT void\n PyThread_tss_free(",
            "+_Py_P2_HUB_RESIDENT int\n PyThread_tss_is_created(",
            "+_Py_P2_HUB_RESIDENT int\n PyThread_tss_create(",
            "+_Py_P2_HUB_RESIDENT void\n PyThread_tss_delete(",
            "+_Py_P2_HUB_RESIDENT int\n PyThread_tss_set(",
            "+_Py_P2_HUB_RESIDENT void *\n PyThread_tss_get(",
        ):
            self.assertIn(definition_pin, patch)
        for cold_callback in (
            "+_Py_P2_HUB_RESIDENT void*\n _PyObject_Malloc(",
            "+_Py_P2_HUB_RESIDENT void*\n _PyObject_Calloc(",
            "+_Py_P2_HUB_RESIDENT void\n _PyObject_Free(",
            "+_Py_P2_HUB_RESIDENT void*\n _PyObject_Realloc(",
        ):
            self.assertNotIn(cold_callback, patch)
        for hot_service in (
            "PyMem_RawMalloc",
            "_PyMem_RawFree",
            "PyMem_Malloc",
            "PyObject_Free",
            "PyMem_SetAllocator",
            "PyThread_allocate_lock",
            "PyThread_tss_create",
            "PyThread_tss_get",
        ):
            self.assertIn(hot_service, patch)
        for marker in (
            "ENTER",
            "RUNTIME:PASS",
            "PREINIT:PASS",
            "ARGV:PASS",
            "INITIALIZED:PASS",
        ):
            self.assertIn(f'P2_CPYTHON_STAGE("{marker}")', patch)

    def test_p2_measured_startup_transitions_are_collapsed(self) -> None:
        patch_name = "0024-p2-collapse-cpython-startup-transitions.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0023-p2-co-locate-cpython-startup-overlays.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("p2_hub_overlay(6)"), 1)
        self.assertIn(
            "_Py_P2_UTF8_OVERLAY __attribute__((p2_hub_overlay(6)))", patch
        )
        for source in (
            "--- a/Include/internal/pycore_fileutils.h",
            "--- a/Objects/stringlib/codecs.h",
            "--- a/Objects/unicodeobject.c",
        ):
            self.assertIn(source, patch)
        for utf8_placement in (
            "_Py_error_handler errors) _Py_P2_UTF8_OVERLAY;",
            "Py_LOCAL_INLINE(Py_UCS4) _Py_P2_UTF8_OVERLAY",
            "+_Py_P2_UTF8_OVERLAY int\n _Py_DecodeUTF8Ex(",
        ):
            self.assertIn(utf8_placement, patch)

        for source in (
            "--- a/Include/internal/pycore_getopt.h",
            "--- a/Include/internal/pycore_hashtable.h",
            "--- a/Include/internal/pycore_object.h",
            "--- a/Include/internal/pycore_pystate.h",
            "--- a/Include/internal/pycore_unicodeobject.h",
            "--- a/Include/pyerrors.h",
            "--- a/Objects/typeobject.c",
            "--- a/Python/getopt.c",
            "--- a/Python/hashtable.c",
            "--- a/Python/mysnprintf.c",
            "--- a/Python/pystate.c",
        ):
            self.assertIn(source, patch)

        for declaration_pin in (
            "_PyOS_ResetGetOpt(void) _Py_P2_HUB_RESIDENT;",
            "void *value) _Py_P2_HUB_RESIDENT;",
            "_PyThreadState_MustExit(PyThreadState *tstate) "
            "_Py_P2_HUB_RESIDENT;",
            "static int hashtable_rehash(_Py_hashtable_t *ht) "
            "_Py_P2_HUB_RESIDENT;",
        ):
            self.assertIn(declaration_pin, patch)
        for definition_pin in (
            "+_Py_P2_HUB_RESIDENT void\n _PyUnicode_InternStatic(",
            "+_Py_P2_HUB_RESIDENT _Py_hashtable_entry_t *\n "
            "_Py_hashtable_get_entry_generic(",
            "+_Py_P2_HUB_RESIDENT int\n _Py_hashtable_set(",
            "+_Py_P2_HUB_RESIDENT void*\n _Py_hashtable_get(",
            "+_Py_P2_HUB_RESIDENT static int\n hashtable_rehash(",
            "+_Py_P2_HUB_RESIDENT void _PyOS_ResetGetOpt(",
            "+_Py_P2_HUB_RESIDENT int\n+_PyOS_GetOpt(",
            "+_Py_P2_HUB_RESIDENT int\n _PyThreadState_MustExit(",
            "+_Py_P2_HUB_RESIDENT int\n PyOS_snprintf(",
            "+_Py_P2_HUB_RESIDENT int\n PyOS_vsnprintf(",
            "+_Py_P2_HUB_RESIDENT void\n _PyType_InitCache(",
        ):
            self.assertIn(definition_pin, patch)

        for resident_symbol in (
            "_PyUnicode_InternStatic",
            "_Py_hashtable_get",
            "_Py_hashtable_get_entry_generic",
            "_Py_hashtable_set",
            "hashtable_rehash",
            "_PyOS_GetOpt",
            "_PyOS_ResetGetOpt",
            "_PyThreadState_MustExit",
            "PyOS_snprintf",
            "PyOS_vsnprintf",
            "_PyType_InitCache",
        ):
            self.assertIn(resident_symbol, patch)

        pystate_patch = patch[patch.index("--- a/Python/pystate.c") :]
        pop = pystate_patch.index("#  pragma clang attribute pop")
        resident = pystate_patch.index(
            "+_Py_P2_HUB_RESIDENT int\n _PyThreadState_MustExit("
        )
        restore = pystate_patch.index(
            "#  pragma clang attribute push(__attribute__((p2_hub_overlay(4)))"
        )
        self.assertLess(pop, resident)
        self.assertLess(resident, restore)
        self.assertEqual(pystate_patch.count("attribute pop"), 1)
        self.assertEqual(pystate_patch.count("p2_hub_overlay(4)"), 1)

    def test_p2_type_initialization_closure_is_co_located(self) -> None:
        patch_name = "0025-p2-co-locate-type-initialization-overlay.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0024-p2-collapse-cpython-startup-transitions.patch"),
            makefile.index(patch_name),
        )
        self.assertIn(
            "_Py_P2_INIT_OVERLAY __attribute__((p2_hub_overlay(7)))", patch
        )
        self.assertIn(
            "-#  pragma clang attribute push(__attribute__((p2_hub_overlay(4)))",
            patch,
        )
        self.assertIn(
            "+#  pragma clang attribute push(__attribute__((p2_hub_overlay(7)))",
            patch,
        )

        for source in (
            "--- a/Include/internal/pycore_pystate.h",
            "--- a/Include/internal/pycore_typeobject.h",
            "--- a/Include/object.h",
            "--- a/Include/pyerrors.h",
            "--- a/Include/pyport.h",
            "--- a/Objects/object.c",
            "--- a/Objects/typeobject.c",
            "--- a/Python/errors.c",
            "--- a/Python/pylifecycle.c",
            "--- a/Python/pystate.c",
        ):
            self.assertIn(source, patch)

        for overlay_definition in (
            "+_Py_P2_INIT_OVERLAY static int\n type_ready(",
            "+_Py_P2_INIT_OVERLAY static PyObject *\n mro_implementation_unlocked(",
            "+_Py_P2_INIT_OVERLAY static int\n mro_internal_unlocked(",
            "+_Py_P2_INIT_OVERLAY static int\n add_subclass(",
            "+_Py_P2_INIT_OVERLAY static void\n type_modified_unlocked(",
            "+_Py_P2_INIT_OVERLAY static int\n init_static_type(",
            "+_Py_P2_INIT_OVERLAY static void\n type_mro_modified(",
            "+_Py_P2_INIT_OVERLAY static void\n managed_static_type_state_clear(",
            "+_Py_P2_INIT_OVERLAY static int\n type_ready_post_checks(",
            "+_Py_P2_INIT_OVERLAY static int\n type_ready_managed_dict(",
            "+_Py_P2_INIT_OVERLAY static PyObject *\n lookup_maybe_method(",
            "+_Py_P2_INIT_OVERLAY static int\n is_subtype_with_mro(",
            "+_Py_P2_INIT_OVERLAY static inline void\n stop_readying(",
            "+_Py_P2_INIT_OVERLAY static void **\n slotptr(",
            "+_Py_P2_INIT_OVERLAY PyObject *\n _PyType_GetDict(",
            "+_Py_P2_INIT_OVERLAY static const char *\n skip_signature(",
            "+_Py_P2_INIT_OVERLAY static PyTypeObject *\n solid_base(",
            "+_Py_P2_INIT_OVERLAY static const char *\n find_signature(",
            "+_Py_P2_INIT_OVERLAY static PyObject *\n class_name(",
            "+_Py_P2_INIT_OVERLAY int\n _PyStaticType_InitBuiltin(",
            "+_Py_P2_INIT_OVERLAY static PyObject *\n lookup_method(",
            "+_Py_P2_INIT_OVERLAY static const char *\n _PyType_DocWithoutSignature(",
            "+_Py_P2_INIT_OVERLAY PyStatus\n _PyTypes_InitTypes(",
            "+_Py_P2_INIT_OVERLAY int\n PyType_Ready(",
        ):
            self.assertIn(overlay_definition, patch)

        typeobject_patch = patch[
            patch.index("--- a/Objects/typeobject.c") :
            patch.index("--- a/Python/errors.c")
        ]
        # Twenty-three definitions (the 22-function closure plus
        # PyType_Ready) and four necessary forward declarations.
        self.assertEqual(typeobject_patch.count("_Py_P2_INIT_OVERLAY"), 27)

        for resident_definition in (
            "+_Py_P2_HUB_RESIDENT PyThreadState *\n _PyThreadState_GetCurrent(",
            "+_Py_P2_HUB_RESIDENT void\n _Py_Dealloc(",
            "+_Py_P2_HUB_RESIDENT PyObject* _Py_HOT_FUNCTION\n PyErr_Occurred(",
        ):
            self.assertIn(resident_definition, patch)
        for resident_declaration in (
            "_PyThreadState_GetCurrent(void)\n+    _Py_P2_HUB_RESIDENT;",
            "_Py_Dealloc(PyObject *) _Py_P2_HUB_RESIDENT;",
            "PyErr_Occurred(void) _Py_P2_HUB_RESIDENT;",
        ):
            self.assertIn(resident_declaration, patch)

        pystate_patch = patch[patch.index("--- a/Python/pystate.c") :]
        get_current = pystate_patch.index("_PyThreadState_GetCurrent(void)")
        pop = pystate_patch.rindex("attribute pop", 0, get_current)
        restore = pystate_patch.index("p2_hub_overlay(4)", get_current)
        self.assertLess(pop, get_current)
        self.assertLess(get_current, restore)

    def test_p2_type_initialization_success_path_is_co_located(self) -> None:
        patch_name = (
            "0026-p2-co-locate-type-initialization-success-path.patch"
        )
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0025-p2-co-locate-type-initialization-overlay.patch"),
            makefile.index(patch_name),
        )
        self.assertIn(
            "-#  pragma clang attribute push(__attribute__((p2_hub_overlay(7)))",
            patch,
        )
        self.assertIn(
            "+#  pragma clang attribute push(__attribute__((p2_hub_overlay(4)))",
            patch,
        )
        self.assertNotIn(
            "+#  pragma clang attribute push(__attribute__((p2_hub_overlay(7)))",
            patch,
        )

        helpers = {
            "Objects/dictobject.c": (
                "PyDict_Contains", "PyDict_New", "PyDict_SetDefaultRef",
                "PyDict_SetItem", "_PyDict_NewKeysForClass",
                "_PyDict_SendEvent", "_PyDict_SetItem_Take2",
                "_PyObject_InitInlineValues", "_Py_dict_lookup",
                "build_indices_unicode", "dict_setdefault_ref_lock_held",
                "dictresize", "find_empty_slot", "free_keys_object",
                "insert_combined_dict", "insert_to_emptydict", "insertdict",
                "new_dict", "new_keys_object", "setitem_take2_lock_held",
                "unicodekeys_lookup_unicode",
            ),
            "Objects/descrobject.c": (
                "descr_new", "PyDescr_NewMethod", "PyDescr_NewClassMethod",
                "PyDescr_NewMember", "PyDescr_NewGetSet", "PyDescr_NewWrapper",
            ),
            "Objects/methodobject.c": ("PyCMethod_New",),
            "Objects/funcobject.c": ("PyStaticMethod_New",),
            "Objects/tupleobject.c": (
                "tuple_alloc", "PyTuple_New", "PyTuple_Pack",
            ),
            "Objects/unicodeobject.c": (
                "PyUnicode_New", "PyUnicode_FromString", "ascii_decode",
                "unicode_decode_utf8", "PyUnicode_DecodeUTF8Stateful",
                "unicode_hash", "intern_common", "_PyUnicode_InternMortal",
                "PyUnicode_InternFromString",
            ),
            "Python/pyhash.c": ("_Py_HashBytes", "fnv"),
            "Objects/weakrefobject.c": (
                "get_basic_refs", "insert_weakref", "allocate_weakref",
                "get_or_create_weakref", "PyWeakref_NewRef",
            ),
            "Objects/longobject.c": (
                "_PyLong_New", "PyLong_FromUnsignedLong", "PyLong_FromVoidPtr",
                "long_hash",
            ),
            "Python/lock.c": (
                "_PyMutex_TryUnlock", "PyMutex_Lock", "PyMutex_Unlock",
            ),
            "Objects/object.c": (
                "PyObject_Hash", "_Py_NewReference", "_Py_NewReferenceNoTotal",
            ),
            "Objects/typeobject.c": (
                "_PyStaticType_GetState", "_PyType_AllocNoTrack",
                "PyType_GenericAlloc",
            ),
            "Python/gc.c": (
                "PyObject_IS_GC", "_Py_ScheduleGC", "_PyObject_GC_Link",
                "gc_alloc", "_PyObject_GC_New", "_PyObject_GC_NewVar",
            ),
        }
        self.assertEqual(sum(len(names) for names in helpers.values()), 67)
        for source, names in helpers.items():
            marker = f"diff --git a/{source} b/{source}"
            self.assertIn(marker, patch)
            section = patch[patch.index(marker):]
            next_diff = section.find("\ndiff --git ", 1)
            if next_diff >= 0:
                section = section[:next_diff]
            for name in names:
                definition = re.compile(
                    rf"^\+_Py_P2_INIT_OVERLAY [^\n]+\n {re.escape(name)}\(",
                    re.MULTILINE,
                )
                self.assertRegex(section, definition, msg=f"{source}:{name}")

        self.assertIn(
            "+extern void _PyObject_SetDeferredRefcount(PyObject *op) "
            "_Py_P2_HUB_RESIDENT;",
            patch,
        )
        self.assertRegex(
            patch,
            re.compile(
                r"^\+_Py_P2_HUB_RESIDENT void\n "
                r"_PyObject_SetDeferredRefcount\(",
                re.MULTILINE,
            ),
        )

        self.assertEqual(
            len(re.findall(r"^\+_Py_P2_INIT_OVERLAY ", patch, re.MULTILINE)),
            72,
        )
        self.assertEqual(
            len(re.findall(
                r"^\+.*_Py_P2_INIT_OVERLAY;$", patch, re.MULTILINE
            )),
            42,
        )
        for forward in (
            "+_Py_P2_INIT_OVERLAY static int dictresize(",
            "+_Py_P2_INIT_OVERLAY static void free_keys_object(",
            "+_Py_P2_INIT_OVERLAY static Py_hash_t unicode_hash(PyObject *);",
        ):
            self.assertIn(forward, patch)
        self.assertEqual(
            patch.count(
                "+_Py_P2_INIT_OVERLAY static int\n"
                " dict_setdefault_ref_lock_held("
            ),
            2,
        )
        self.assertEqual(
            patch.count(
                "+_Py_P2_INIT_OVERLAY static PyObject *\n"
                " unicode_decode_utf8("
            ),
            2,
        )

    def test_p2_gil_and_type_initialization_diagnostics_are_decisive(self) -> None:
        patch_name = "0027-p2-trace-gil-and-static-type-initialization.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index(
                "0026-p2-co-locate-type-initialization-success-path.patch"
            ),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 2)
        self.assertEqual(
            patch.count(
                "+#if defined(__NuttX__) && defined(__propeller2__)"
            ),
            8,
        )

        lifecycle = patch[patch.index("--- a/Python/pylifecycle.c"):]
        set_tstate = lifecycle.index(" _PyGILState_SetTstate(tstate);")
        set_tstate_pass = lifecycle.index(
            'printf("P2PY:INIT:GIL:TSTATE:PASS\\n")', set_tstate
        )
        init_gil = lifecycle.index(" _PyEval_InitGIL(tstate, own_gil);")
        init_gil_pass = lifecycle.index(
            'printf("P2PY:INIT:GIL:READY:PASS\\n")', init_gil
        )
        self.assertLess(set_tstate, set_tstate_pass)
        self.assertLess(set_tstate_pass, init_gil)
        self.assertLess(init_gil, init_gil_pass)

        pycore_begin = lifecycle.index("P2PY:INIT:PYCORE_TYPES:BEGIN")
        pycore_call = lifecycle.index(" _PyTypes_InitTypes(interp)", pycore_begin)
        pycore_pass = lifecycle.index("P2PY:INIT:PYCORE_TYPES:PASS", pycore_call)
        self.assertLess(pycore_begin, pycore_call)
        self.assertLess(pycore_call, pycore_pass)

        objects = patch[
            patch.index("--- a/Objects/object.c"):
            patch.index("diff --git a/Python/pylifecycle.c")
        ]
        types_begin = objects.index("P2PY:INIT:TYPES:BEGIN:N=%u")
        loop = objects.index(
            " for (size_t i=0; i < Py_ARRAY_LENGTH(static_types); i++)",
            types_begin,
        )
        before = objects.index("P2PY:INIT:TYPE:I=%u:BEFORE", loop)
        call = objects.index(" _PyStaticType_InitBuiltin(interp, type)", before)
        after = objects.index("P2PY:INIT:TYPE:I=%u:AFTER:R=%d", call)
        result_check = objects.index(" if (result < 0)", after)
        types_pass = objects.index("P2PY:INIT:TYPES:PASS:N=%u", result_check)
        self.assertLess(types_begin, loop)
        self.assertLess(loop, before)
        self.assertLess(before, call)
        self.assertLess(call, after)
        self.assertLess(after, result_check)
        self.assertLess(result_check, types_pass)
        self.assertNotIn("%p", patch)
        self.assertNotIn("tp_name", patch)

    def test_p2_type_cache_avoids_eager_external_writes(self) -> None:
        patch_name = "0028-p2-avoid-eager-type-cache-psram-writes.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index(
                "0027-p2-trace-gil-and-static-type-initialization.patch"
            ),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 2)
        for source in (
            "--- a/Include/internal/pycore_typeobject.h",
            "--- a/Objects/typeobject.c",
        ):
            self.assertIn(source, patch)

        p2_guard = "+#if defined(__NuttX__) && defined(__propeller2__)"
        self.assertEqual(patch.count(p2_guard), 2)
        self.assertIn(
            "+    PyObject *name;        // exact str/None reference; "
            "NULL is empty",
            patch,
        )
        self.assertIn("+#  define MCACHE_SIZE_EXP 9", patch)
        self.assertIn("+#  define MCACHE_SIZE_EXP 12", patch)
        self.assertIn("-#define MCACHE_SIZE_EXP 12", patch)

        self.assertIn(
            p2_guard
            + "\n+    // P2 interpreter state is zero-initialized.  A NULL name "
            "is an empty\n"
            "+    // cache entry, so avoid thousands of scalar PSRAM writes "
            "at startup.\n"
            "+    (void)interp;\n+#else\n"
            "     struct type_cache *cache = &interp->types.type_cache;",
            patch,
        )
        self.assertIn(
            "+                         entry->name != NULL && "
            "entry->name != Py_None &&",
            patch,
        )
        self.assertEqual(
            len(
                re.findall(
                    r"^\+\s+Py_XDECREF\(old_value\);$", patch, re.MULTILINE
                )
            ),
            2,
        )
        self.assertEqual(
            patch.count("+            Py_XSETREF(entry->name, Py_None);"), 1
        )
        self.assertEqual(
            len(
                re.findall(
                    r"^-\s+Py_DECREF\(old_value\);$", patch, re.MULTILINE
                )
            ),
            2,
        )
        self.assertEqual(
            patch.count("-            Py_SETREF(entry->name, Py_None);"), 1
        )

    def test_p2_unicode_static_interning_is_co_located_and_traced(self) -> None:
        patch_name = "0029-p2-co-locate-unicode-static-interning.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0028-p2-avoid-eager-type-cache-psram-writes.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 5)
        for source in (
            "--- a/Include/internal/pycore_unicodeobject.h",
            "--- a/Include/internal/pycore_unicodeobject_generated.h",
            "--- a/Objects/unicodeobject.c",
            "--- a/Tools/build/generate_global_objects.py",
            "--- a/Python/pylifecycle.c",
        ):
            self.assertIn(source, patch)

        for placement in (
            "+    _Py_P2_INIT_OVERLAY;",
            "+_Py_P2_INIT_OVERLAY static inline void\n"
            " _PyUnicode_InitStaticStrings(",
            "+_Py_P2_INIT_OVERLAY static int "
            "unicode_compare_eq(PyObject *, PyObject *);",
            "+_Py_P2_INIT_OVERLAY static Py_uhash_t\n"
            " hashtable_unicode_hash(",
            "+_Py_P2_INIT_OVERLAY static int\n"
            " hashtable_unicode_compare(",
            "+_Py_P2_INIT_OVERLAY static int\n init_interned_dict(",
            "+_Py_P2_INIT_OVERLAY static PyStatus\n"
            " init_global_interned_strings(",
            "+_Py_P2_INIT_OVERLAY static int\n unicode_compare_eq(",
            "+_Py_P2_INIT_OVERLAY PyStatus\n"
            " _PyUnicode_InitGlobalObjects(",
            '+        printer.write("_Py_P2_INIT_OVERLAY static inline void")',
        ):
            self.assertIn(placement, patch)

        unicode = patch[
            patch.index("--- a/Objects/unicodeobject.c"):
            patch.index("diff --git a/Python/pylifecycle.c")
        ]
        static_begin = unicode.index("P2PY:INIT:UNICODE_STATIC:BEGIN")
        static_call = unicode.index(" _PyUnicode_InitStaticStrings(interp)")
        static_pass = unicode.index("P2PY:INIT:UNICODE_STATIC:PASS")
        latin_begin = unicode.index("P2PY:INIT:LATIN1:BEGIN")
        latin_loop = unicode.index(" for (int i = 0; i < 256; i++)")
        latin_pass = unicode.index("P2PY:INIT:LATIN1:PASS")
        self.assertLess(static_begin, static_call)
        self.assertLess(static_call, static_pass)
        self.assertLess(static_pass, latin_begin)
        self.assertLess(latin_begin, latin_loop)
        self.assertLess(latin_loop, latin_pass)

        lifecycle = patch[patch.index("--- a/Python/pylifecycle.c"):]
        phases = (
            ("GLOBAL_OBJECTS", " pycore_init_global_objects(interp)"),
            ("CODE", " _PyCode_Init(interp)"),
            ("DTOA", " _PyDtoa_Init(interp)"),
            ("GC", " _PyGC_Init(interp)"),
        )
        previous = -1
        for phase, call in phases:
            begin = lifecycle.index(f"P2PY:INIT:{phase}:BEGIN")
            invocation = lifecycle.index(call, begin)
            passed = lifecycle.index(f"P2PY:INIT:{phase}:PASS", invocation)
            self.assertLess(previous, begin)
            self.assertLess(begin, invocation)
            self.assertLess(invocation, passed)
            previous = passed
        self.assertLess(previous, lifecycle.index(" pycore_init_types(interp)"))

        expected_markers = {
            "P2PY:INIT:UNICODE_STATIC:BEGIN",
            "P2PY:INIT:UNICODE_STATIC:PASS",
            "P2PY:INIT:LATIN1:BEGIN",
            "P2PY:INIT:LATIN1:PASS",
            "P2PY:INIT:GLOBAL_OBJECTS:BEGIN",
            "P2PY:INIT:GLOBAL_OBJECTS:PASS",
            "P2PY:INIT:CODE:BEGIN",
            "P2PY:INIT:CODE:PASS",
            "P2PY:INIT:DTOA:BEGIN",
            "P2PY:INIT:DTOA:PASS",
            "P2PY:INIT:GC:BEGIN",
            "P2PY:INIT:GC:PASS",
        }
        actual_markers = set(re.findall(r'printf\("(P2PY:[^"%]+)\\n"\);', patch))
        self.assertEqual(actual_markers, expected_markers)
        self.assertEqual(patch.count('+    printf("P2PY:'), 12)
        self.assertNotRegex(
            patch,
            re.compile(r'^\+\s+printf\("P2PY:[^"]*%', re.MULTILINE),
        )
        self.assertEqual(
            patch.count("+#if defined(__NuttX__) && defined(__propeller2__)"),
            8,
        )

    def test_static_unicode_interning_hashtable_failure_is_fatal(self) -> None:
        patch_name = "0030-fix-static-unicode-intern-failure.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0029-p2-co-locate-unicode-static-interning.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 1)
        self.assertIn("--- a/Objects/unicodeobject.c", patch)
        self.assertIn(
            "-    if (_Py_hashtable_set(INTERNED_STRINGS, s, s) < -1)",
            patch,
        )
        self.assertIn(
            "+    if (_Py_hashtable_set(INTERNED_STRINGS, s, s) < 0)",
            patch,
        )
        for diagnostic in (
            "up_putc(",
            "P2_FIRST_STATIC_TRACE",
            "<nuttx/arch.h>",
            "pycore_runtime.h",
            "pycore_global_strings.h",
            "pycore_unicodeobject_generated.h",
            "generate_global_objects.py",
            "--- a/Python/hashtable.c",
        ):
            self.assertNotIn(diagnostic, patch)

    def test_p2_hashtable_load_factors_use_checked_integer_arithmetic(
        self,
    ) -> None:
        patch_name = "0031-p2-use-integer-hashtable-load-factors.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        intern_fix_name = "0030-fix-static-unicode-intern-failure.patch"
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index(intern_fix_name),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 1)
        self.assertIn("--- a/Python/hashtable.c", patch)
        self.assertIn(
            "+    ((ht)->nentries > (ht)->nbuckets / 2)", patch
        )
        self.assertIn(
            "+    ((ht)->nentries <= ((ht)->nbuckets - 1) / 10)", patch
        )
        self.assertIn("+    if (ht->nentries > SIZE_MAX / 10)", patch)
        self.assertIn("+    target_size = ht->nentries * 10 / 3;", patch)
        self.assertIn(
            "+    if (target_size > (SIZE_MAX >> 1) + 1)", patch
        )
        self.assertIn(
            "+    if (new_size > SIZE_MAX / sizeof(ht->buckets[0]))", patch
        )
        self.assertIn("+    if (HASHTABLE_LOW_REACHED(ht)) {", patch)
        self.assertIn("+    if (HASHTABLE_HIGH_EXCEEDED(ht)) {", patch)
        setter = patch.index(" _Py_hashtable_set(")
        increment = patch.index(" ht->nentries++", setter)
        threshold = patch.index("HASHTABLE_HIGH_EXCEEDED(ht)", increment)
        rehash = patch.index("hashtable_rehash(ht)", threshold)
        self.assertLess(increment, threshold)
        self.assertLess(threshold, rehash)

        scale_guard = patch.index("ht->nentries > SIZE_MAX / 10")
        scale = patch.index("target_size = ht->nentries * 10 / 3", scale_guard)
        round_target = patch.index("new_size = round_size(target_size)", scale)
        bucket_guard = patch.index(
            "new_size > SIZE_MAX / sizeof(ht->buckets[0])", round_target
        )
        self.assertLess(scale_guard, scale)
        self.assertLess(scale, round_target)
        self.assertLess(round_target, bucket_guard)

        for diagnostic in (
            "P2_FIRST_STATIC_TRACE",
            "up_putc(",
            "needs_rehash",
            "pycore_runtime.h",
            "pycore_global_strings.h",
            "<nuttx/arch.h>",
        ):
            self.assertNotIn(diagnostic, patch)

        # P2 has power-of-two bucket counts.  At every threshold boundary
        # reachable in 32 MiB, these integer predicates preserve the strict
        # one-half and one-tenth comparisons without a scaling overflow.
        for bucket_bits in range(4, 23):
            buckets = 1 << bucket_bits
            for entries in {
                0,
                buckets // 10,
                (buckets - 1) // 10,
                buckets // 2,
                buckets // 2 + 1,
            }:
                self.assertEqual(
                    entries > buckets // 2,
                    2 * entries > buckets,
                )
                self.assertEqual(
                    entries <= (buckets - 1) // 10,
                    10 * entries < buckets,
                )

    def test_p2_unicode_deallocation_stays_in_type_init_overlay(self) -> None:
        patch_name = (
            "0032-p2-co-locate-unicode-deallocation-with-type-init.patch"
        )
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0031-p2-use-integer-hashtable-load-factors.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 2)
        self.assertIn(
            "--- a/Include/internal/pycore_unicodeobject.h", patch
        )
        self.assertIn("--- a/Objects/unicodeobject.c", patch)
        self.assertIn("Stub 0x448 is unicode_dealloc()", patch)
        self.assertIn("The two linked bodies total 516 bytes", patch)
        self.assertIn(
            "+PyAPI_FUNC(void) _PyUnicode_ExactDealloc(PyObject *op)\n"
            "+    _Py_P2_INIT_OVERLAY;",
            patch,
        )
        self.assertRegex(
            patch,
            re.compile(
                r"^\+_Py_P2_INIT_OVERLAY static void\n"
                r" unicode_dealloc\(",
                re.MULTILINE,
            ),
        )
        self.assertRegex(
            patch,
            re.compile(
                r"^\+_Py_P2_INIT_OVERLAY void\n"
                r" _PyUnicode_ExactDealloc\(",
                re.MULTILINE,
            ),
        )
        self.assertNotIn("p2_hub_resident", patch)
        self.assertNotIn("pragma clang attribute", patch)

    def test_p2_module_attribute_startup_loop_stays_in_type_init_overlay(
        self,
    ) -> None:
        patch_name = "0034-p2-co-locate-module-attribute-startup-loop.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index(
                "0033-p2-co-locate-dictionary-hot-paths-with-type-init.patch"
            ),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 7)
        for source in (
            "Include/cpython/object.h",
            "Include/internal/pycore_dict.h",
            "Include/object.h",
            "Objects/dictobject.c",
            "Objects/moduleobject.c",
            "Objects/object.c",
            "Objects/typeobject.c",
        ):
            self.assertIn(f"--- a/{source}", patch)

        self.assertIn("stub 0x39c", patch)
        self.assertIn("The measured\nlinked bodies total 5,836 bytes", patch)
        self.assertIn("predicted result is 89,816 bytes", patch)
        self.assertIn("90,112-byte slot, leaving 296", patch)
        self.assertIn("post-link residency verifier remains the hard authority", patch)
        self.assertEqual(
            sum(
                1
                for line in patch.splitlines()
                if line.startswith("+")
                and not line.startswith("+++")
                and "_Py_P2_INIT_OVERLAY" in line
            ),
            18,
        )
        self.assertNotIn("p2_hub_resident", patch)
        self.assertNotIn("pragma clang attribute", patch)

        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        patched = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith((" ", "+")) and not line.startswith("+++")
        )
        for declaration in (
            "_PyType_LookupRef(PyTypeObject *, PyObject *)\n"
            "    _Py_P2_INIT_OVERLAY;",
            "_PyObject_GenericSetAttrWithDict(PyObject *, PyObject *,\n"
            "                                 PyObject *, PyObject *)\n"
            "    _Py_P2_INIT_OVERLAY;",
            "_PyDict_SetItem_LockHeld(PyDictObject *dict, PyObject *name,\n"
            "                                    PyObject *value) "
            "_Py_P2_INIT_OVERLAY;",
            "_PyObjectDict_SetItem(PyTypeObject *tp, PyObject *obj,\n"
            "                                 PyObject **dictptr, "
            "PyObject *name,\n"
            "                                 PyObject *value) "
            "_Py_P2_INIT_OVERLAY;",
            "PyObject_SetAttrString(PyObject *, const char *, PyObject *) "
            "_Py_P2_INIT_OVERLAY;",
            "PyObject_SetAttr(PyObject *, PyObject *, PyObject *) "
            "_Py_P2_INIT_OVERLAY;",
            "PyObject_GenericSetAttr(PyObject *, PyObject *, PyObject *) "
            "_Py_P2_INIT_OVERLAY;",
        ):
            with self.subTest(declaration=declaration):
                self.assertIn(declaration, patched)

        for definition in (
            "_add_methods_to_object",
            "PyObject_SetAttrString",
            "PyObject_SetAttr",
            "PyObject_GenericSetAttr",
            "_PyObject_GenericSetAttrWithDict",
            "_PyObjectDict_SetItem",
            "_PyDict_SetItem_LockHeld",
            "_PyType_LookupRef",
            "assign_version_tag",
            "find_name_in_mro",
        ):
            with self.subTest(definition=definition):
                self.assertRegex(
                    patched,
                    re.compile(
                        rf"_Py_P2_INIT_OVERLAY[^;\n]*\n{definition}\(",
                        re.MULTILINE,
                    ),
                )

        self.assertIn(
            "_Py_P2_INIT_OVERLAY static int assign_version_tag("
            "PyInterpreterState *interp, PyTypeObject *type);",
            added,
        )

    def test_p2_frozen_code_bootstrap_loops_are_co_located(self) -> None:
        patch_name = "0036-p2-co-locate-frozen-code-bootstrap-loops.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0035-p2-co-locate-gc-traversal-working-set.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 9)
        for source in (
            "Include/pyport.h",
            "Include/internal/pycore_code.h",
            "Include/internal/pycore_object.h",
            "Include/internal/pycore_unicodeobject.h",
            "Objects/codeobject.c",
            "Objects/object.c",
            "Objects/unicodeobject.c",
            "Python/instrumentation.c",
            "Python/specialize.c",
        ):
            self.assertIn(f"--- a/{source}", patch)

        self.assertIn(
            "_Py_P2_CODEINIT_OVERLAY __attribute__((p2_hub_overlay(9)))",
            patch,
        )
        self.assertIn("Their previous linked sizes plus the group header total 440", patch)
        self.assertIn("previous linked bodies total 260 bytes", patch)
        self.assertIn("group becomes 90,076 bytes", patch)
        self.assertIn("90,112-byte slot, leaving 36 bytes", patch)
        self.assertIn("post-link residency verifier remains the hard authority", patch)
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        patched = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith((" ", "+")) and not line.startswith("+++")
        )
        for placement in (
            "_Py_GetBaseOpcode(PyCodeObject *code, int offset)\n"
            "    _Py_P2_CODEINIT_OVERLAY;",
            "_PyCode_Quicken(PyCodeObject *code) _Py_P2_CODEINIT_OVERLAY;",
            "_Py_P2_CODEINIT_OVERLAY int\n_Py_GetBaseOpcode(",
            "_Py_P2_CODEINIT_OVERLAY void\n_PyCode_Quicken(",
            "_Py_SetImmortal(PyObject *op) _Py_P2_INIT_OVERLAY;",
            "_Py_SetImmortalUntracked(PyObject *op) _Py_P2_INIT_OVERLAY;",
            "_PyUnicode_InternImmortal(PyInterpreterState *interp, PyObject **)\n"
            "    _Py_P2_INIT_OVERLAY;",
            "_Py_P2_INIT_OVERLAY void\n_PyUnicode_InternImmortal(",
            "_Py_P2_INIT_OVERLAY void\n_Py_SetImmortalUntracked(",
            "_Py_P2_INIT_OVERLAY void\n_Py_SetImmortal(",
        ):
            with self.subTest(placement=placement):
                self.assertIn(placement, patched)

        self.assertNotIn("p2_hub_resident", added)
        self.assertNotIn("pragma clang attribute", patch)

    def test_p2_cyclic_gc_collector_core_is_co_located(self) -> None:
        patch_name = "0037-p2-co-locate-cyclic-gc-collector-core.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0036-p2-co-locate-frozen-code-bootstrap-loops.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 1)
        self.assertIn("--- a/Python/gc.c", patch)
        self.assertIn("all 31 linked bodies", patch)
        self.assertIn("14,156-byte", patch)
        self.assertIn("leaving 75,956 bytes", patch)

        patched = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith((" ", "+")) and not line.startswith("+++")
        )
        collector_functions = (
            "PyGC_Collect",
            "gc_collect_main",
            "gc_list_merge",
            "deduce_unreachable",
            "invoke_gc_callback",
            "PyGC_Disable",
            "PyGC_Enable",
            "PyGC_IsEnabled",
            "PyObject_GC_Del",
            "PyObject_GC_IsFinalized",
            "PyObject_GC_IsTracked",
            "PyObject_GC_Track",
            "PyObject_GC_UnTrack",
            "PyUnstable_GC_VisitObjects",
            "PyUnstable_Object_GC_NewWithExtraData",
            "_PyGC_Collect",
            "_PyGC_CollectNoFail",
            "_PyGC_Dump",
            "_PyGC_DumpShutdownStats",
            "_PyGC_Fini",
            "_PyGC_Freeze",
            "_PyGC_GetFreezeCount",
            "_PyGC_GetObjects",
            "append_objects",
            "_PyGC_GetReferrers",
            "_PyGC_Init",
            "_PyGC_InitState",
            "_PyGC_Unfreeze",
            "_PyObject_GC_Resize",
            "_Py_RunGC",
            "referrersvisit",
        )
        self.assertEqual(len(collector_functions), 31)
        self.assertEqual(len(set(collector_functions)), 31)
        self.assertEqual(patch.count("+_Py_P2_GC_OVERLAY"), 31)
        for function in collector_functions:
            with self.subTest(function=function):
                self.assertRegex(
                    patched,
                    re.compile(
                        rf"^_Py_P2_GC_OVERLAY[^\n]*\n{re.escape(function)}\(",
                        re.MULTILINE,
                    ),
                )

    def test_p2_code_name_interning_rebalances_type_init_group(self) -> None:
        patch_name = "0038-p2-keep-code-name-interning-with-type-init.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0037-p2-co-locate-cyclic-gc-collector-core.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 2)
        self.assertIn("--- a/Objects/codeobject.c", patch)
        self.assertIn("--- a/Objects/typeobject.c", patch)
        self.assertIn("89,836 bytes (0x15eec)", patch)
        self.assertIn("leaving 276 bytes", patch)

        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        removed = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("-") and not line.startswith("---")
        )
        self.assertIn(
            "+_Py_P2_INIT_OVERLAY static int\n intern_strings(", patch
        )
        self.assertIn(
            "-_Py_P2_INIT_OVERLAY static void\n+static void\n "
            "managed_static_type_state_clear(",
            patch,
        )
        self.assertNotIn("intern_constants", added)
        self.assertEqual(added.count("_Py_P2_INIT_OVERLAY"), 1)
        self.assertEqual(removed.count("_Py_P2_INIT_OVERLAY"), 1)

    def test_p2_importlib_bootstrap_steps_are_traced(self) -> None:
        patch_name = "0039-p2-trace-importlib-bootstrap-steps.patch"
        patch = (PYTHON_DIR / "patch" / patch_name).read_text(encoding="utf-8")
        makefile = (PYTHON_DIR / "Makefile").read_text(encoding="utf-8")

        self.assertEqual(makefile.count(patch_name), 1)
        self.assertLess(
            makefile.index("0038-p2-keep-code-name-interning-with-type-init.patch"),
            makefile.index(patch_name),
        )
        self.assertEqual(patch.count("diff --git "), 1)
        self.assertIn("--- a/Python/import.c", patch)
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertNotIn("#else", added)
        self.assertNotIn("goto ", added)
        for marker in (
            "P2PY:IMPORTLIB:BEGIN",
            "P2PY:IMPORTLIB:FROZEN:BEGIN",
            "P2PY:IMPORTLIB:FROZEN:FAIL",
            "P2PY:IMPORTLIB:FROZEN:PASS",
            "P2PY:IMPORTLIB:ADD_MODULE_REF:BEGIN",
            "P2PY:IMPORTLIB:ADD_MODULE_REF:FAIL",
            "P2PY:IMPORTLIB:ADD_MODULE_REF:PASS",
            "P2PY:IMPORTLIB:BOOTSTRAP_IMP:BEGIN",
            "P2PY:IMPORTLIB:BOOTSTRAP_IMP:FAIL",
            "P2PY:IMPORTLIB:BOOTSTRAP_IMP:PASS",
            "P2PY:IMPORTLIB:SET_MODULE_STRING:BEGIN",
            "P2PY:IMPORTLIB:SET_MODULE_STRING:FAIL",
            "P2PY:IMPORTLIB:SET_MODULE_STRING:PASS",
            "P2PY:IMPORTLIB:INSTALL:BEGIN",
            "P2PY:IMPORTLIB:INSTALL:FAIL",
            "P2PY:IMPORTLIB:INSTALL:PASS",
            "P2PY:IMPORTLIB:PASS",
        ):
            with self.subTest(marker=marker):
                self.assertEqual(added.count(f'printf("{marker}\\n");'), 1)

        self.assertEqual(
            added.count("#if defined(__NuttX__) && defined(__propeller2__)"),
            13,
        )
        self.assertEqual(added.count("#endif"), 13)
        for preserved_step in (
            'if (PyImport_ImportFrozenModule("_frozen_importlib") <= 0) {',
            'PyObject *importlib = PyImport_AddModuleRef("_frozen_importlib");',
            "PyObject *imp_mod = bootstrap_imp(tstate);",
            'if (_PyImport_SetModuleString("_imp", imp_mod) < 0) {',
            'PyObject *value = PyObject_CallMethod(importlib, "_install",',
        ):
            with self.subTest(step=preserved_step):
                self.assertIn(preserved_step, patch)

    def test_romfs_mount_state_retries_without_procfs(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        wrapper_config = CONFIG_HEADER + """\
#define CONFIG_FS_ROMFS 1
#define CONFIG_FS_HEAPSIZE 1048576
#define CONFIG_LIBC_TMPDIR "/tmp"
#define CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS 1
#define CONFIG_INTERPRETERS_CPYTHON_P2_DEFAULT_NO_SITE 1
#define CONFIG_INTERPRETERS_CPYTHON_P2_FIXED_PATH_CONFIG 1
#define CONFIG_INTERPRETERS_CPYTHON_PYTHONPATH "/tmp"
#define CONFIG_INTERPRETERS_CPYTHON_ROMFS_SECTORSIZE 512
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
        self.assertIn("board_cpython_romdisk_register", wrapper_source)
        self.assertIn("P2PY:ROMDISK:READY:MODE=BUFFERED", result.stdout)
        self.assertIn("P2PY:ROMFS:MOUNTED", result.stdout)
        self.assertIn("P2PY:TMPFS:READY:PATH=/tmp:HEAP=1048576", result.stdout)
        self.assertIn("P2PY:CPYTHON:EARLY:PASS", result.stdout)
        self.assertIn("P2PY:CPYTHON:RUN", result.stdout)
        self.assertIn(
            "PASS: persistent CPython ROMFS mount state", result.stdout
        )


if __name__ == "__main__":
    unittest.main()
