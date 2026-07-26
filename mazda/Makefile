# Multi-target cross-compile Makefile for LD_PRELOAD-able shared libraries
# that run on the Mazda CMU (ARM Cortex-A9 NEON, glibc/Linux). Uses the
# m3-toolchain submodule.
#
# Layout convention: each entry in $(PATCHES) names an OEM .so we patch
# with our own preload shim. Sources for patch <name> live in
# patches/<name>/, and the build produces build/<cfg>/libpatch-<name>.so.
#
# Adding a new patch:
#   1. mkdir patches/<oem-lib-name>
#   2. drop one or more .cpp files into it
#   3. append <oem-lib-name> to $(PATCHES) below
#
# Targets:
#   make                -> all release libs (default)
#   make debug          -> all debug libs
#   make release        -> all release libs
#   make all            -> debug + release
#   make <name>         -> release lib for one patch (e.g. `make blmjciaapa`)
#   make <name>-debug   -> debug   lib for one patch (e.g. `make blmjciaapa-debug`)
#   make clean          -> wipe build/

M3TOOLCHAIN := ./m3-toolchain
TRIPLE      := arm-cortexa9_neon-linux-gnueabi
CXX         := $(M3TOOLCHAIN)/bin/$(TRIPLE)-g++
SYSROOT     := $(M3TOOLCHAIN)/$(TRIPLE)/sysroot

# OEM libraries we patch. Order doesn't matter.
PATCHES := blmjciaapa svcjcinavi aap_service

# pkg-config wrapper pointed at the device's sysroot so it resolves
# against the cross-compiled .pc files (not the host's). Same pattern
# as the upstream headunit project's Makefile — inlined here so we
# don't need a separate shell-script wrapper file in the repo.
#
# Add libraries the patches need to PKGS and both -cflags and -libs
# expand them with proper transitive deps + sysroot-prepended paths.
#
# (Empty for now: the HUD sender talks to the OEM D-Bus stack
# (libjcidbus / libjcivbsnaviclient) via dlsym(RTLD_DEFAULT) at
# runtime — those libs are already mapped in the jciAAPA process — so
# we add no link-time D-Bus dependency and no longer use dbus-c++.)
PKG_CONFIG := PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/pkgconfig \
              PKG_CONFIG_SYSROOT_DIR=$(SYSROOT) \
              pkg-config
PKGS       :=
PKG_CFLAGS := $(if $(PKGS),$(shell $(PKG_CONFIG) --cflags $(PKGS)))
PKG_LIBS   := $(if $(PKGS),$(shell $(PKG_CONFIG) --libs   $(PKGS)))

# Flags shared by all patches and both configurations.
# -fPIC is required for code that goes into a shared object.
# -static-libstdc++ keeps the .so off the device's older libstdc++ at
# load time (same rationale as the original single-binary build).
# -Idbus exposes the vendored CMU D-Bus proxies (generated_cmu.h) from
# the community headunit project (mirrored under mazda/dbus/). All
# patches can `#include "generated_cmu.h"` to talk to OEM D-Bus
# interfaces without re-deriving the C++ proxy classes by hand.
# $(PKG_CFLAGS) brings in dbus-c++-1 + libdbus-1 include dirs from
# the sysroot's pkg-config metadata; matching $(PKG_LIBS) in LDFLAGS
# below carries the right -L / -l invocations.
COMMON_CXXFLAGS := -std=c++11 \
                   --sysroot=$(SYSROOT) \
                   -march=armv7-a -mtune=cortex-a9 -mfpu=neon \
                   -fPIC \
                   -fvisibility=hidden \
                   -D_GLIBCXX_USE_CXX11_ABI=0 \
                   -Ipatches \
                   $(PKG_CFLAGS) \
                   $(EXTRA_CXXFLAGS)
# EXTRA_CXXFLAGS: appended to every compile. E.g. verbose logs in a release lib:
#   make ... EXTRA_CXXFLAGS='-DLOG_LEVEL=LOG_LEVEL_VERBOSE'
# -D_GLIBCXX_USE_CXX11_ABI=0 selects the pre-C++11 (COW) std::string
# and std::list ABI. The device ships libstdc++.so.6.0.14 (GCC 4.6
# era) which only knows that ABI; libdbus-c++-1.so is linked against
# it. Our toolchain is GCC 4.9.1 with libstdc++.so.6.0.20 which
# defaults to the new SSO std::string ABI. Without this flag, any
# std::string crossing the libdbus-c++ boundary (e.g. our
# guidancePointData._1 assignment in hud_send.cpp) gets interpreted
# with two different layouts and corrupts the heap — manifested as
# a SEGV in std::has_facet when the corrupted bytes happened to land
# in a libstdc++ locale _Impl. See git log for the crash analysis.
#
# Likewise -static-libstdc++ used to be on both lines. It was added
# defensively ("the device's libstdc++ is older than ours") but in
# practice it gave us two libstdc++ runtimes in the same process —
# ours (statically baked) vs the device's (loaded via libdbus-c++-1).
# Both had their own global C locale, both shared the heap, and the
# ABI mismatch above corrupted state across the boundary. Dropping
# it leaves us with one runtime (the device's 6.0.14), which is
# also what libdbus-c++-1 uses, so all C++ stdlib state is shared
# correctly. We pick up libstdc++.so.6 as a DT_NEEDED instead.
COMMON_LDFLAGS  := --sysroot=$(SYSROOT) -shared \
                   -Wl,--no-undefined \
                   -lpthread -ldl -lrt \
                   $(PKG_LIBS)

# Debug: no optimization, full debug info, no LTO so symbols/inlining are sane.
DEBUG_CXXFLAGS   := $(COMMON_CXXFLAGS) -O0 -g3 -DDEBUG
DEBUG_LDFLAGS    := $(COMMON_LDFLAGS)

# Release: -O3 with LTO across translation units, dead-section GC, stripped.
RELEASE_CXXFLAGS := $(COMMON_CXXFLAGS) -O3 -DNDEBUG \
                    -ffunction-sections -fdata-sections \
                    -flto
RELEASE_LDFLAGS  := $(COMMON_LDFLAGS) -O3 -flto \
                    -Wl,--gc-sections -Wl,-s

BUILD_DIR   := build
DEBUG_DIR   := $(BUILD_DIR)/debug
RELEASE_DIR := $(BUILD_DIR)/release

# Per-patch derived names: build/<cfg>/libpatch-<name>.so
DEBUG_LIBS   := $(addprefix $(DEBUG_DIR)/libpatch-,$(addsuffix .so,$(PATCHES)))
RELEASE_LIBS := $(addprefix $(RELEASE_DIR)/libpatch-,$(addsuffix .so,$(PATCHES)))

.PHONY: all debug release clean $(PATCHES) $(addsuffix -debug,$(PATCHES))
.DEFAULT_GOAL := release

all: debug release
debug: $(DEBUG_LIBS)
release: $(RELEASE_LIBS)

# Convenience aliases:
#   `make blmjciaapa`        -> build/release/libpatch-blmjciaapa.so
#   `make blmjciaapa-debug`  -> build/debug/libpatch-blmjciaapa.so
$(PATCHES): %: $(RELEASE_DIR)/libpatch-%.so
$(addsuffix -debug,$(PATCHES)): %-debug: $(DEBUG_DIR)/libpatch-%.so

# Object-file pattern rules: mirror the patches/ tree under build/<cfg>/.
#   build/release/<patch>/foo.o  <-  patches/<patch>/foo.cpp
$(DEBUG_DIR)/%.o: patches/%.cpp
	@mkdir -p $(@D)
	$(CXX) -MD $(DEBUG_CXXFLAGS) -c $< -o $@

$(RELEASE_DIR)/%.o: patches/%.cpp
	@mkdir -p $(@D)
	$(CXX) -MD $(RELEASE_CXXFLAGS) -c $< -o $@

# Per-patch link rules generated via $(eval). Sources are discovered
# recursively (via `find`) at parse time, so subdivision of a patch
# into subfolders — e.g. patches/blmjciaapa/{touch,hud}/*.cpp — is a
# no-op for the Makefile. Adding a new .cpp anywhere under an
# existing patches/<name>/ tree just works. Adding a new patch
# directory still means appending its name to $(PATCHES) above.
define gen_patch_rules
SRCS_$(1)         := $$(shell find patches/$(1) -name '*.cpp' -type f)
DEBUG_OBJS_$(1)   := $$(patsubst patches/$(1)/%.cpp,$(DEBUG_DIR)/$(1)/%.o,$$(SRCS_$(1)))
RELEASE_OBJS_$(1) := $$(patsubst patches/$(1)/%.cpp,$(RELEASE_DIR)/$(1)/%.o,$$(SRCS_$(1)))

$(DEBUG_DIR)/libpatch-$(1).so: $$(DEBUG_OBJS_$(1))
	$$(CXX) $$^ $$(DEBUG_LDFLAGS) -Wl,-soname,libpatch-$(1).so -o $$@

$(RELEASE_DIR)/libpatch-$(1).so: $$(RELEASE_OBJS_$(1))
	$$(CXX) $$^ $$(RELEASE_LDFLAGS) -Wl,-soname,libpatch-$(1).so -o $$@

-include $$(DEBUG_OBJS_$(1):.o=.d)
-include $$(RELEASE_OBJS_$(1):.o=.d)
endef

$(foreach p,$(PATCHES),$(eval $(call gen_patch_rules,$(p))))

clean:
	rm -rf $(BUILD_DIR)
