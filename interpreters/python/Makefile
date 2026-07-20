############################################################################
# apps/interpreters/python/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

CPYTHON_URL ?= "https://github.com/python/cpython/archive"
CPYTHON_VERSION = $(patsubst "%",%,$(strip $(CONFIG_INTERPRETERS_CPYTHON_VERSION)))
CPYTHON_VERSION_MINOR=$(basename $(CPYTHON_VERSION))
CPYTHON_ZIP = v$(CPYTHON_VERSION).zip

CPYTHON_UNPACKNAME = Python
CPYTHON_PATCHES = $(wildcard patch$(DELIM)*.patch)
CPYTHON_PATCH_VARIANT = $(if $(filter y,$(CONFIG_ARCH_P2)),p2,generic)
CPYTHON_PATCH_STAMP = $(CPYTHON_UNPACKNAME)$(DELIM).nuttx-patched-$(CPYTHON_PATCH_VARIANT)
UNPACK ?= unzip -q -o

MACHDEP=nuttx
CONFIG_SITE=${CURDIR}/config.site
SETUP_LOCAL=${CURDIR}/Setup.local
CPYTHON_PATH=$(CURDIR)/$(CPYTHON_UNPACKNAME)

BUILDIR=$(CURDIR)/build
INSTALLDIR=$(CURDIR)/install
HOSTBUILD=$(BUILDIR)/host
HOSTINSTALL=$(INSTALLDIR)/host
HOSTPYTHON=$(HOSTINSTALL)/bin/python3
TARGETBUILD=$(BUILDIR)/target
TARGETINSTALL=$(INSTALLDIR)/target
TARGETLIBPYTHON=$(TARGETINSTALL)/libpython$(CPYTHON_VERSION_MINOR).a
TARGETMODULESPACK=$(TARGETBUILD)/lib/python$(shell echo $(CPYTHON_VERSION_MINOR) | tr -d .).zip
TARGETMODULES=$(TARGETINSTALL)/lib/
P2_CONFIGURE_LINKDIR=$(TARGETBUILD)/p2-configure-link
P2_ROMFS_DIR=$(TARGETBUILD)/p2-romfs
P2_CONFIGURE_CC=$(CC)
P2_CONFIGURE_CXX=$(CXX)

ifeq ($(CONFIG_ARCH_P2),y)
P2_CPYTHON_REGEN_ENV=NUTTX_P2_FREEZE_STARTUP_ENCODINGS=1
P2_CONFIGURE_LINK_STAMP=$(P2_CONFIGURE_LINKDIR)/.prepared
P2_CONFIGURE_LIBC=$(P2_CONFIGURE_LINKDIR)/libc.a
P2_CONFIGURE_LIBP2=$(P2_CONFIGURE_LINKDIR)/libp2.a
P2_CONFIGURE_LINKER=$(P2_CONFIGURE_LINKDIR)/p2.ld
P2_CONFIGURE_DEPS=$(TOPDIR)$(DELIM).config \
			  $(realpath $(TOPDIR)$(DELIM)Make.defs)
P2_CONFIGURE_CC=$(CC) --target=p2
P2_CONFIGURE_CXX=$(CXX) --target=p2
CFLAGS += $(DEFINE_PREFIX)__NuttX__
ifeq ($(CONFIG_P2_EC32MB_PSRAM_UNIFIED),y)
P2_CPYTHON_CFLAGS += -mllvm -p2-unified-memory
endif
ifeq ($(CONFIG_P2_HUB_OVERLAYS),y)
P2_OVERLAY_SLOT_ADDRESS=$(shell printf '0x%x' \
	$$((0x7c000 - $(CONFIG_P2_HUB_OVERLAY_SLOT_SIZE))))
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlays
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlays-all
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlay-link-assigned
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlay-source-root=$(abspath $(CPYTHON_PATH))
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlay-source-namespace=cpython
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlay-source-variant=$(CPYTHON_VERSION)-nuttx-p2
P2_CPYTHON_CFLAGS += -mllvm -p2-hub-overlay-slot-address=$(P2_OVERLAY_SLOT_ADDRESS)
P2_CPYTHON_CFLAGS += -mllvm -p2-externalize-data
endif
endif

CFLAGS += ${INCDIR_PREFIX}$(CPYTHON_PATH)$(DELIM)Include
CFLAGS += ${INCDIR_PREFIX}$(CPYTHON_PATH)$(DELIM)Test
CFLAGS += ${INCDIR_PREFIX}$(CPYTHON_PATH)$(DELIM)Include$(DELIM)internal
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)$(DELIM)system
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)$(DELIM)system$(DELIM)zlib$(DELIM)zlib
CFLAGS += ${INCDIR_PREFIX}$(TARGETBUILD)
CFLAGS += -Wno-shadow -Wno-undef -Wno-format -Wno-builtin-macro-redefined
CFLAGS += -Wno-type-limits -Wno-implicit-fallthrough -Wno-char-subscripts
CFLAGS += -Wno-sign-compare -Wno-unused-const-variable -Wno-unused-function
CFLAGS += -Wno-unused-variable -Wno-overflow -Wno-unused-but-set-variable
CFLAGS += -Wno-strict-prototypes -Wno-maybe-uninitialized -nostdlib

DEPPATH += --dep-path $(CPYTHON_UNPACKNAME)$(DELIM)Programs
VPATH   += :$(CPYTHON_UNPACKNAME)$(DELIM)Programs

$(CPYTHON_ZIP):
	@echo "Downloading: $(CPYTHON_URL)/$(CPYTHON_ZIP)"
	$(Q) $(call DOWNLOAD,$(CPYTHON_URL),$(CPYTHON_ZIP))

$(CPYTHON_PATCH_STAMP): $(CPYTHON_ZIP) $(CPYTHON_PATCHES)
	@echo "Unpacking: $(CPYTHON_ZIP) -> $(CPYTHON_UNPACKNAME)"
	$(Q) $(call DELDIR,$(CPYTHON_UNPACKNAME))
	$(Q) $(UNPACK) $(CPYTHON_ZIP)
	$(Q) mv	cpython-$(CPYTHON_VERSION) $(CPYTHON_UNPACKNAME)
	@echo "Patching $(CPYTHON_UNPACKNAME)"
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0001-workaround-newlib-resource.h-limitations.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0002-fix-various-uint32_t-unsigned-int-type-mismatch-issu.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0003-reuse-wasm_assets.py-for-generating-an-archive-of-py.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0004-recognize-nuttx-as-a-supported-OS.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0005-gh-122907-Fix-Builds-Without-HAVE_DYNAMIC_LOADING-Se.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0006-change-var-name-to-avoid-conflict-with-nuttx-unused_.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0007-undef-atexit_register.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0008-declare-struct-timeval.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0009-include-nuttx-sys-select-header-to-define-FD_SETSIZE.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0010-check-for-the-d_ino-member-of-the-structure-dirent.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0011-avoid-redefinition-warning-if-UNUSED-is-already-defi.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0012-hack-place-_PyRuntime-structure-into-PSRAM-bss-regio.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0013-transform-functions-used-by-NuttX-to-lowercase.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0014-insert-prefix-to-list_length-to-avoid-symbol-collisi.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0015-keep-ensurepip-in-stdlib-archive.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0016-fix-timezone-offset-check-when-time-t-is-unsigned.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0017-stdlib-zip-keep-pydecimal-and-trim-tooling-extras.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0018-ignore-chmod-on-nuttx-like-wasi.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0019-undef-get_errno-set_errno-to-avoid-nuttx-macro-coll.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0020-ctypes-skip-pythonapi-on-nuttx.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0021-posixmodule-ignore-utime-enosys-on-nuttx.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0022-p2-use-dedicated-tss-for-current-thread-state.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0023-p2-co-locate-cpython-startup-overlays.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0024-p2-collapse-cpython-startup-transitions.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0025-p2-co-locate-type-initialization-overlay.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0026-p2-co-locate-type-initialization-success-path.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0027-p2-trace-gil-and-static-type-initialization.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0028-p2-avoid-eager-type-cache-psram-writes.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0029-p2-co-locate-unicode-static-interning.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0030-fix-static-unicode-intern-failure.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0031-p2-use-integer-hashtable-load-factors.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0032-p2-co-locate-unicode-deallocation-with-type-init.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0033-p2-co-locate-dictionary-hot-paths-with-type-init.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0034-p2-co-locate-module-attribute-startup-loop.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0035-p2-co-locate-gc-traversal-working-set.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0036-p2-co-locate-frozen-code-bootstrap-loops.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0037-p2-co-locate-cyclic-gc-collector-core.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0038-p2-keep-code-name-interning-with-type-init.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0039-p2-trace-importlib-bootstrap-steps.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0040-allow-importlib-without-thread-module.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0041-add-lock-only-thread-compatibility.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0042-p2-co-locate-importlib-startup-hot-paths.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0043-p2-trace-main-interpreter-initialization.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0044-p2-trace-fill-time-softfloat-boundaries.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0045-p2-add-default-no-site-config-hook.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0046-p2-add-fixed-path-config-fast-path.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0047-p2-inline-static-unicode-compare-hot-edge.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0048-p2-inline-exact-tuple-iterator-hot-edge.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0049-p2-trace-main-interpreter-subphases.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0050-p2-inline-marshal-reference-list-append.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0051-p2-use-utf8-decoder-for-marshal-ascii.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0052-p2-pin-posix-constdef-comparator.patch
ifeq ($(CONFIG_ARCH_P2),y)
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0053-p2-freeze-startup-encodings.patch
	$(Q) patch -p1 -d $(CPYTHON_UNPACKNAME) < patch$(DELIM)0054-p2-record-frozen-encoding-pcbuild.patch
endif
	$(Q) touch $@

$(CPYTHON_UNPACKNAME): $(CPYTHON_PATCH_STAMP)

$(HOSTPYTHON): $(CPYTHON_PATCH_STAMP)
	mkdir -p $(HOSTBUILD)
	mkdir -p $(HOSTINSTALL)
	$(Q) ( \
			cd $(HOSTBUILD) && $(CPYTHON_PATH)/configure \
			 --with-pydebug \
			 --prefix=$(HOSTINSTALL) \
			 --disable-test-modules \
		 )
	$(MAKE) -C $(HOSTBUILD) install

# The `config.site` file contains settings that override the configuration
# settings provided by the `configure` script. Depending on the features
# enabled on NuttX, this file may need to be adjusted.

$(CONFIG_SITE):
	$(Q) ( cp $(CONFIG_SITE).in $(CONFIG_SITE))
ifeq ($(CONFIG_ARCH_HAVE_FORK),y)
	@echo "export ac_cv_func_fork=\"yes\"" >> $@
else
	@echo "export ac_cv_func_fork=\"no\"" >> $@
endif
ifeq ($(CONFIG_SYSTEM_SYSTEM),y)
	@echo "export ac_cv_func_system=\"yes\"" >> $@
else
	@echo "export ac_cv_func_system=\"no\"" >> $@
endif

# The `Setup.local` file enables or disables Python modules.
# Depending on the features enabled on NuttX, this file may need to be
# adjusted. Please note that the base `Setup.local.in` file  only contains
# a section to disable Python modules. Inserting lines to it will disable
# such modules.

$(SETUP_LOCAL):
	$(Q) ( cp $(SETUP_LOCAL).in $(SETUP_LOCAL))
ifneq ($(CONFIG_ARCH_HAVE_FORK),y)
	@echo "_posixsubprocess" >> $@
endif
ifneq ($(CONFIG_LIBC_DLFCN),y)
	@echo "_ctypes" >> $@
endif
ifeq ($(CONFIG_ARCH_P2),y)
	# Setup.local is in *disabled* mode.  P2 deliberately omits _thread:
	# Hub overlays have one task owner and CPython is a single-interpreter,
	# single-Python-task runtime on this profile.
	@echo "_socket" >> $@
	@echo "_interpreters" >> $@
	@echo "_thread" >> $@
	@echo "fcntl" >> $@
	@echo "mmap" >> $@
	@echo "syslog" >> $@
	@echo "termios" >> $@
endif

# For the Python's `configure` script, please consider the following
# when building for NuttX:
#
# Use sed to remove optimization flags from NuttX's CFLAGS because
# Python's configure script requires them in OPT. Having the flags in
# both places causes a conflict.
#
# Also, use -O0 for OPT because -Os is causing problems in
# Python/Modules/getpath.c (issue will be filed soon to track this
# problem).

ifeq ($(CONFIG_ARCH_P2),y)
$(P2_CONFIGURE_LINKDIR):
	$(Q) mkdir -p $@

$(P2_CONFIGURE_LIBC) $(P2_CONFIGURE_LIBP2): | $(P2_CONFIGURE_LINKDIR)
	$(Q) $(AR) $@

$(P2_CONFIGURE_LINKER): p2-configure.ld | $(P2_CONFIGURE_LINKDIR)
	$(Q) cp $< $@

$(P2_CONFIGURE_LINK_STAMP): $(P2_CONFIGURE_LIBC) \
				   $(P2_CONFIGURE_LIBP2) $(P2_CONFIGURE_LINKER)
	$(Q) touch $@
endif

# Configure probes must remain resident and use ordinary P2 addressing.  The
# unified-memory pass intentionally fails closed on unsupported intrinsics,
# while overlay probes can acquire dispatcher references.  Neither outcome is
# a truthful feature result.  Production objects receive all P2 transforms via
# EXTRA_CFLAGS in the TARGETLIBPYTHON rule.

# TARGETINSTALL is only a host staging directory.  Configure the target
# runtime with target-native paths so sys.prefix, getpath, and packaged
# _sysconfigdata never contain a host worktree pathname.
$(TARGETBUILD)/Makefile: $(HOSTPYTHON) $(CONFIG_SITE) $(SETUP_LOCAL) \
			 $(CPYTHON_PATCH_STAMP) \
			 $(CURDIR)$(DELIM)Makefile $(P2_CONFIGURE_DEPS) \
			 $(P2_CONFIGURE_LINK_STAMP)
	$(Q) mkdir -p $(TARGETBUILD)/Modules
	$(Q) mkdir -p $(TARGETMODULES)/python$(CPYTHON_VERSION_MINOR)
	$(Q) ( cp Setup.local $(TARGETBUILD)/Modules/Setup.local )
	$(Q) ( \
			cd $(TARGETBUILD); \
			CFLAGS="$(CFLAGS)"; \
			ARCH=$(CONFIG_ARCH); \
			ARCH_CHIP=$(CONFIG_ARCH_CHIP); \
			ARCH="$${ARCH//-/}"; \
			ARCH_CHIP="$${ARCH_CHIP//-/}"; \
			CPYTHON_HOST="$${ARCH}-$${ARCH_CHIP}-nuttx"; \
			if [ "$(CONFIG_ARCH_P2)" = "y" ]; then \
				CPYTHON_HOST="p2-unknown-nuttx"; \
				LDFLAGS="-L$(P2_CONFIGURE_LINKDIR)"; \
				PKG_CONFIG=false; \
				export LDFLAGS PKG_CONFIG; \
			fi; \
			CFLAGS="$$(echo "$${CFLAGS}" | sed -E \
				-e 's/-Os[[:space:]]//g' \
				-e 's/(^|[[:space:]])--target=p2([[:space:]]|$$)/\1/g' \
				-e 's/-mllvm[[:space:]]+-p2-unified-memory([[:space:]]|$$)/\1/g' \
				-e 's/-mllvm[[:space:]]+-p2-externalize-constant-data([[:space:]]|$$)/\1/g')"; \
			CPP="$(P2_CONFIGURE_CC) $${CFLAGS} -E"; \
			CFLAGS="$${CFLAGS}" \
			CPP="$${CPP}" \
			CC="$(P2_CONFIGURE_CC)" \
			CXX="$(P2_CONFIGURE_CXX)" \
			AR="$(AR)" \
			ARFLAGS=" " \
			MACHDEP="$(MACHDEP)" \
			OPT="-Os" \
			CONFIG_SITE="$(CONFIG_SITE)" \
			$(CPYTHON_PATH)/configure \
			 --prefix=/usr/local \
			 --disable-shared \
			 --host=$${CPYTHON_HOST} \
			 --build=$(shell $(CPYTHON_PATH)/config.guess) \
			 --with-build-python=${HOSTPYTHON} \
			 --without-mimalloc \
			 --without-pymalloc \
			 --without-computed-gotos \
			 --disable-test-modules \
			 --with-ensurepip=no \
		)
	$(Q) sed 's/^#define HAVE_LIBB2 1/\/* #undef HAVE_LIBB2 *\//' \
		$(TARGETBUILD)/pyconfig.h > $(TARGETBUILD)/pyconfig.h.tmp && \
		mv $(TARGETBUILD)/pyconfig.h.tmp $(TARGETBUILD)/pyconfig.h
	$(Q) sed 's/-lb2//g' $(TARGETBUILD)/Makefile > \
		$(TARGETBUILD)/Makefile.tmp && \
		mv $(TARGETBUILD)/Makefile.tmp $(TARGETBUILD)/Makefile

BUNDLED_WHEELS_DIR = $(CPYTHON_PATH)/Lib/ensurepip/_bundled
# PyPI downloads use the host system pip, not $(HOSTPYTHON)'s ensurepip bundle:
# pip 24.x vendors urllib3 that breaks on Python 3.13 (zlib.Decompress.unused_data).
HOSTPIP = python3 -m pip

$(TARGETLIBPYTHON): $(TARGETBUILD)/Makefile
ifneq ($(or $(filter y,$(CONFIG_INTERPRETERS_CPYTHON_ENABLE_PIP)),$(filter y,$(CONFIG_INTERPRETERS_CPYTHON_INSTALL_NUTTX_PACKAGE))),)
	$(Q) mkdir -p $(BUNDLED_WHEELS_DIR)
endif
ifeq ($(CONFIG_INTERPRETERS_CPYTHON_ENABLE_PIP),y)
	$(Q) ( \
		PIP_WHEEL_VERSION=$$($(HOSTPYTHON) -c "import ensurepip; print(ensurepip._PIP_VERSION)"); \
		PIP_WHEEL=$(BUNDLED_WHEELS_DIR)/pip-$${PIP_WHEEL_VERSION}-py3-none-any.whl; \
		if [ ! -f "$${PIP_WHEEL}" ]; then \
			echo "Fetching pip wheel $${PIP_WHEEL_VERSION} for ensurepip bundle"; \
			$(HOSTPIP) download --only-binary=:all: --no-deps --dest $(BUNDLED_WHEELS_DIR) pip==$${PIP_WHEEL_VERSION}; \
		fi; \
		echo "Pre-compiling pip wheel with build Python (must match embedded CPython version)"; \
		$(HOSTPYTHON) $(CURDIR)/repack_wheel_add_pyc.py --package pip "$${PIP_WHEEL}"; \
	)
endif
ifeq ($(CONFIG_INTERPRETERS_CPYTHON_INSTALL_NUTTX_PACKAGE),y)
	$(Q) ( \
		set -e; \
		NUTTX_WHEEL=$$(ls $(BUNDLED_WHEELS_DIR)/nuttx_periphery-*-py3-none-any.whl 2>/dev/null | head -1 || true); \
		if [ -z "$${NUTTX_WHEEL}" ]; then \
			echo "Fetching latest nuttx-periphery wheel for ensurepip bundle"; \
			$(HOSTPIP) download --only-binary=:all: --no-deps --dest $(BUNDLED_WHEELS_DIR) nuttx-periphery; \
			NUTTX_WHEEL=$$(ls $(BUNDLED_WHEELS_DIR)/nuttx_periphery-*-py3-none-any.whl); \
		fi; \
		echo "Pre-compiling nuttx-periphery wheel with build Python (must match embedded CPython version)"; \
		$(HOSTPYTHON) $(CURDIR)/repack_wheel_add_pyc.py --package nuttx_periphery "$${NUTTX_WHEEL}"; \
	)
endif
	$(Q) $(P2_CPYTHON_REGEN_ENV) $(MAKE) -C $(TARGETBUILD) \
		EXTRA_CFLAGS="$(P2_CPYTHON_CFLAGS)" regen-frozen
	$(MAKE) -C $(TARGETBUILD) \
		EXTRA_CFLAGS="$(P2_CPYTHON_CFLAGS)" \
		libpython$(CPYTHON_VERSION_MINOR).a wasm_stdlib
	$(Q) ( cp $(TARGETBUILD)/libpython$(CPYTHON_VERSION_MINOR).a $(TARGETLIBPYTHON) )
	$(Q) $(UNPACK) $(TARGETMODULESPACK) -d $(TARGETMODULES)/python$(CPYTHON_VERSION_MINOR)
ifneq ($(or $(filter y,$(CONFIG_INTERPRETERS_CPYTHON_ENABLE_PIP)),$(filter y,$(CONFIG_INTERPRETERS_CPYTHON_INSTALL_NUTTX_PACKAGE))),)
	$(Q) mkdir -p $(TARGETMODULES)/python$(CPYTHON_VERSION_MINOR)/site-packages
	$(Q) ( \
		set -e; \
		BUNDLED_DIR=$(TARGETMODULES)/python$(CPYTHON_VERSION_MINOR)/ensurepip/_bundled; \
		SITE_PACKAGES=$(TARGETMODULES)/python$(CPYTHON_VERSION_MINOR)/site-packages; \
		: > "$${SITE_PACKAGES}/bundled_wheels.pth"; \
		for wheel in $${BUNDLED_DIR}/*.whl; do \
			[ -f "$${wheel}" ] || continue; \
			whl_name=$$(basename "$${wheel}"); \
			echo "Pre-installing wheel via zipimport into target sys.path: $${whl_name}"; \
			echo "../ensurepip/_bundled/$${whl_name}" >> "$${SITE_PACKAGES}/bundled_wheels.pth"; \
		done; \
	)
endif

MODULE    = $(CONFIG_INTERPRETERS_CPYTHON)

PROGNAME  += $(CONFIG_INTERPRETERS_CPYTHON_PROGNAME)
PRIORITY  += $(CONFIG_INTERPRETERS_CPYTHON_PRIORITY)
STACKSIZE += $(CONFIG_INTERPRETERS_CPYTHON_LAUNCHER_STACKSIZE)

MAINSRC += python_launcher.c
CSRCS   += python_wrapper.c

checkgenromfs:
	@genromfs -h 1>/dev/null 2>&1 || { \
 echo "Host executable genromfs not available in PATH"; \
 echo "You may need to download in from https://romfs.sourceforge.net/"; \
 exit 1; \
	}

ifeq ($(CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS),y)
romfs_cpython_modules.img : $(TARGETLIBPYTHON) checkgenromfs
	$(Q) $(call DELDIR, $(P2_ROMFS_DIR))
	$(Q) mkdir -p $(P2_ROMFS_DIR)
	$(Q) cp $(TARGETMODULESPACK) \
		$(P2_ROMFS_DIR)$(DELIM)python$(shell echo $(CPYTHON_VERSION_MINOR) | tr -d .).zip
	@genromfs -f $@ -d $(P2_ROMFS_DIR) -V "CPython_P2" || { echo "genromfs failed" ; exit 1 ; }
else
romfs_cpython_modules.img : $(TARGETLIBPYTHON) checkgenromfs
	@genromfs -f $@ -d $(TARGETMODULES) -V "ROMFS_Test" || { echo "genromfs failed" ; exit 1 ; }
endif

ifneq ($(CONFIG_INTERPRETERS_CPYTHON_EXTERNAL_ROMFS),y)
romfs_cpython_modules.h : romfs_cpython_modules.img
	@xxd -i $< | sed -e "s/^unsigned/static const unsigned/g" >$@ || { echo "xxd of $< failed" ; exit 1 ; }

depend:: romfs_cpython_modules.h
else
depend:: romfs_cpython_modules.img
endif

context:: $(CPYTHON_PATCH_STAMP)

distclean::
	$(call DELDIR, $(BUILDIR))
	$(call DELDIR, $(INSTALLDIR))
	$(call DELDIR, $(CPYTHON_UNPACKNAME))
	$(call DELFILE, $(CPYTHON_ZIP))
	$(call DELFILE, romfs_cpython_modules.img)
	$(call DELFILE, romfs_cpython_modules.h)
	$(call DELFILE, config.site)
	$(call DELFILE, Setup.local)

include $(APPDIR)/Application.mk
