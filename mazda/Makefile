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
# -fstack-protector-strong adds a stack canary to catch many buffer
# overruns before they can corrupt control flow.
# -D_FORTIFY_SOURCE=2 enables extra safety checks in libc wrappers such
# as memcpy(), strcpy(), and snprintf() from glibc when the compiler can
# validate them.
# -Wl,-z,relro,-z,now and -Wl,-z,noexecstack tighten the ELF loader
# protections for the output .so without changing the runtime logic.
# $(PKG_CFLAGS) / $(PKG_LIBS) are intentionally empty for now: the patch
# set does not link against a static D-Bus or proprietary SDK bundle.
COMMON_CXXFLAGS := -std=c++11 \
                   --sysroot=$(SYSROOT) \
                   -march=armv7-a -mtune=cortex-a9 -mfpu=neon \
                   -fPIC \
                   -fvisibility=hidden \
                   -fstack-protector-strong \
                   -D_FORTIFY_SOURCE=2 \
                   -Wall -Wextra \
                   -D_GLIBCXX_USE_CXX11_ABI=0 \
                   -Ipatches \
                   $(PKG_CFLAGS) \
                   $(EXTRA_CXXFLAGS)
# EXTRA_CXXFLAGS: appended to every compile. E.g. verbose logs in a release lib:
#   make ... EXTRA_CXXFLAGS='-DLOG_LEVEL=LOG_LEVEL_VERBOSE'
# -D_GLIBCXX_USE_CXX11_ABI=0 selects the pre-C++11 (COW) std::string and
# std::list ABI used by the CMU's GCC 4.6-era libstdc++. Without this flag,
# a std::string crossing the OEM boundary can be interpreted with a different
# layout and corrupt the shared heap.
# We intentionally do not statically link a newer libstdc++ into the .so:
# that would create two C++ runtimes in one process. The shim uses the CMU's
# shipped runtime, matching the ABI used by the OEM libraries.
COMMON_LDFLAGS  := --sysroot=$(SYSROOT) -shared \
                   -Wl,--no-undefined \
                   -Wl,-z,relro,-z,now \
                   -Wl,-z,noexecstack \
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
	$(CXX) -MD -MP $(DEBUG_CXXFLAGS) -c $< -o $@

$(RELEASE_DIR)/%.o: patches/%.cpp
	@mkdir -p $(@D)
	$(CXX) -MD -MP $(RELEASE_CXXFLAGS) -c $< -o $@

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
