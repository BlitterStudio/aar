# Builds every template through mk/aar.mk, the include a third party uses, for
# profile A32 and, when its toolchain is installed, profile A64. Set
# AAR_SYSROOT_A32 and AAR_SYSROOT_A64 to the outputs of build_pic_newlib.sh
# (the SDK image sets them). The freestanding template builds with
# AAR_FREESTANDING=1 and needs no sysroot.
#
#   make                build every template for every available profile
#   make check          build, run aar-check on every ELF, run the tests
#   make check-runtime  run libaar and the conformance payloads under qemu (needs
#                       qemu and Linux cross gcc)
#   make PROFILES=a32   limit the profiles
#
# Output goes to build/<profile>/<app>.elf.
#
# SPDX-License-Identifier: MIT

AAR_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3

# The per-app rules below are expanded before `all` is defined; without this
# the first of them would be the default goal.
.DEFAULT_GOAL := all

CROSS_a32 ?= arm-none-eabi-
CROSS_a64 ?= aarch64-none-elf-
SYSROOT_a32 = $(AAR_SYSROOT_A32)
SYSROOT_a64 = $(AAR_SYSROOT_A64)
TARGET_a32 := arm-none-eabi
TARGET_a64 := aarch64-none-elf

# A32 always. A64 when its compiler is on PATH.
HAVE_A64 := $(shell command -v $(CROSS_a64)gcc >/dev/null 2>&1 && echo 1)
PROFILES ?= a32 $(if $(HAVE_A64),a64)

hello_SRCS := templates/hello.c
pixels_SRCS := templates/pixels.c
fault_SRCS := templates/fault.c
tone_SRCS := templates/tone.c
echo_SRCS := templates/echo.c
console_SRCS := templates/console.c
membench_SRCS := templates/membench.c
isolate_SRCS := templates/isolate.c
spin_SRCS := templates/spin.c
svcbench_SRCS := templates/svcbench.c
memhog_SRCS := templates/memhog.c
svcprobe_SRCS := templates/svcprobe.c
mgl-conformance_SRCS := templates/mgl_conformance.c
filter_SRCS := templates/filter.c
freestanding_SRCS := templates/freestanding.c
freestanding_FREESTANDING := 1
fpenv_SRCS := templates/fpenv.c
ringflood_SRCS := templates/ringflood.c
sbrk_smoke_SRCS := templates/sbrk_smoke.c

# The conformance payloads (conformance/): apps that judge one area of the
# standard from the inside, built per profile as build/<profile>/conf_<area>.elf.
# There is no list to edit: an area is a file conformance/payloads/conf_<area>.c,
# and it runs under qemu when conformance/payloads/conf_<area>.qemu exists (the
# loader of libaar has a partial table; add the marker when it grows what the
# payload needs). Two optional markers set how a payload links, and
# conformance/tests/qemu_run.sh reads the same files: conf_<area>.ldlibs holds
# extra linker words (conf_entry owns its entry symbol with -Wl,-e,...), and
# conf_<area>.freestanding builds it with AAR_FREESTANDING=1.
CONF_DIR := $(AAR_SDK)/conformance/payloads
CONF_AREAS := $(sort $(patsubst $(CONF_DIR)/conf_%.c,%,$(wildcard $(CONF_DIR)/conf_*.c)))
CONF_QEMU_AREAS := $(sort $(patsubst $(CONF_DIR)/conf_%.qemu,%,$(wildcard $(CONF_DIR)/conf_*.qemu)))
CONF_APPS := $(addprefix conf_,$(CONF_AREAS))
$(foreach a,$(CONF_AREAS),$(eval conf_$(a)_SRCS := conformance/payloads/conf_$(a).c))
$(foreach a,$(CONF_AREAS),$(if $(wildcard $(CONF_DIR)/conf_$(a).ldlibs),$(eval conf_$(a)_LDLIBS := $(shell cat $(CONF_DIR)/conf_$(a).ldlibs))))
$(foreach a,$(CONF_AREAS),$(if $(wildcard $(CONF_DIR)/conf_$(a).freestanding),$(eval conf_$(a)_FREESTANDING := 1)))

TEMPLATES := hello pixels fault tone echo console membench isolate spin svcbench memhog svcprobe mgl-conformance filter freestanding fpenv ringflood
CHECK_APPS := $(TEMPLATES) sbrk_smoke $(CONF_APPS)

# One target pair per profile and app: build-<profile>-<app> and
# check-<profile>-<app> (build, then aar-check).
# $(1) profile, $(2) app
define APP_RULES
.PHONY: build-$(1)-$(2) check-$(1)-$(2)
build-$(1)-$(2):
	$$(MAKE) -f $$(AAR_SDK)/mk/aar.mk \
		AAR_SDK=$$(AAR_SDK) AAR_PROFILE=$(1) CROSS=$$(CROSS_$(1)) \
		APP=$(2) SRCS="$$($(2)_SRCS)" BUILD=$$(AAR_SDK)/build/$(1) \
		LDLIBS="$$($(2)_LDLIBS)" \
		AAR_FREESTANDING=$$(or $$($(2)_FREESTANDING),0) all
check-$(1)-$(2):
	$$(MAKE) -f $$(AAR_SDK)/mk/aar.mk \
		AAR_SDK=$$(AAR_SDK) AAR_PROFILE=$(1) CROSS=$$(CROSS_$(1)) \
		APP=$(2) SRCS="$$($(2)_SRCS)" BUILD=$$(AAR_SDK)/build/$(1) \
		LDLIBS="$$($(2)_LDLIBS)" \
		AAR_FREESTANDING=$$(or $$($(2)_FREESTANDING),0) check
endef
$(foreach p,$(PROFILES),$(foreach a,$(CHECK_APPS),$(eval $(call APP_RULES,$(p),$(a)))))

BUILD_TARGETS := $(foreach p,$(PROFILES),$(foreach a,$(TEMPLATES) $(CONF_APPS),build-$(p)-$(a)))
CHECK_TARGETS := $(foreach p,$(PROFILES),$(foreach a,$(CHECK_APPS),check-$(p)-$(a)))
EXPECT_TARGETS := $(foreach p,$(PROFILES),expect-note-$(p))
FP_TARGETS := $(foreach p,$(PROFILES),check-fp-codegen-$(p))
MODE_TARGETS := $(foreach p,$(PROFILES),check-mode-switch-$(p))
GUARD_TARGETS := $(foreach p,$(PROFILES),check-freestanding-guard-$(p))
ABI_TARGETS := $(foreach p,$(PROFILES),check-abi-$(p))

.PHONY: all check clean check-tool check-fp-codegen check-mode-switch \
	check-freestanding-guard check-abi check-abi-host check-loader-host \
	check-expect-note check-runtime check-conf-catalogue check-conf-payloads

all: $(BUILD_TARGETS)

check: check-tool check-abi check-fp-codegen $(CHECK_TARGETS) \
	check-expect-note check-mode-switch check-freestanding-guard check-conf-catalogue

# The checker's own tests, with synthetic images for both profiles.
check-tool:
	$(PYTHON) $(AAR_SDK)/tests/aar_check_test.py

# Header layout and AAR_NOTE bytes with the profile's compiler. check-abi-host
# (layout and transport model on the host, 64-bit and -m32, plus C++) needs a
# host gcc, g++ and -m32 support, which the SDK image does not carry; run it
# by hand where they exist.
check-abi: $(ABI_TARGETS)

define ABI_RULE
.PHONY: check-abi-$(1)
check-abi-$(1):
	$$(AAR_SDK)/tests/abi_check.sh $(1)
endef
$(foreach p,$(PROFILES),$(eval $(call ABI_RULE,$(p))))

check-abi-host:
	$(AAR_SDK)/tests/abi_check.sh host

# The reference loader (loader/) on the host: plan and place, the conformance
# corpus, the aar-check corpus and a mutation fuzz run, 64-bit, -m32 and with
# the sanitizers. Needs a host gcc and python3 (the SDK image has neither);
# make check-runtime runs the same loader under qemu.
check-loader-host:
	sh $(AAR_SDK)/loader/tests/run_host.sh

check-fp-codegen: $(FP_TARGETS)

define FP_RULE
.PHONY: check-fp-codegen-$(1)
check-fp-codegen-$(1):
	CROSS=$$(CROSS_$(1)) $$(PYTHON) $$(AAR_SDK)/tests/fp_codegen_test.py $(1)
endef
$(foreach p,$(PROFILES),$(eval $(call FP_RULE,$(p))))

# The note of the templates that set one. Arguments: ELF STACK_BYTES
# MIN_HEAP_MIB GROUPS_REQUIRED GROUPS_OPTIONAL. Group bits: DISPLAY 1,
# INPUT 2, AUDIO 4, GL 8. pixels, console and membench list the group they
# use but can run without as optional.
check-expect-note: $(EXPECT_TARGETS)

define EXPECT_RULE
.PHONY: expect-note-$(1)
expect-note-$(1): $(foreach a,hello pixels tone mgl-conformance freestanding console membench,check-$(1)-$(a))
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/hello.elf 0 0 0 0
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/pixels.elf 0 0 1 2
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/tone.elf 0 0 4 0
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/mgl-conformance.elf 0 0 9 0
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/freestanding.elf 0 0 0 0
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/console.elf 0 0 0 2
	$$(PYTHON) $$(AAR_SDK)/tools/aar-check --expect-note $$(AAR_SDK)/build/$(1)/membench.elf 0 0 0 1
endef
$(foreach p,$(PROFILES),$(eval $(call EXPECT_RULE,$(p))))

# libc, freestanding, libc again in one build directory.
check-mode-switch: $(MODE_TARGETS)

define MODE_RULE
.PHONY: check-mode-switch-$(1)
check-mode-switch-$(1):
	CROSS=$$(CROSS_$(1)) sh $$(AAR_SDK)/tests/mode_switch_test.sh $$(AAR_SDK) $(1)
endef
$(foreach p,$(PROFILES),$(eval $(call MODE_RULE,$(p))))

# A freestanding link that pulls a libc member must fail the map check. The
# leak source calls strlen, which needs no syscall stubs, and the link gets
# -lc on purpose.
check-freestanding-guard: $(GUARD_TARGETS)

define GUARD_RULE
.PHONY: check-freestanding-guard-$(1)
check-freestanding-guard-$(1):
	@log=$$(AAR_SDK)/build/$(1)/freestanding-leak.log; mkdir -p $$(AAR_SDK)/build/$(1); \
	if $$(MAKE) -f $$(AAR_SDK)/mk/aar.mk \
		AAR_SDK=$$(AAR_SDK) AAR_PROFILE=$(1) CROSS=$$(CROSS_$(1)) \
		APP=freestanding-leak \
		SRCS="templates/freestanding.c tests/freestanding_leak.c" \
		BUILD=$$(AAR_SDK)/build/$(1) \
		AAR_FREESTANDING=1 \
		LDLIBS="-Wl,-u,aar_leak -L$$(SYSROOT_$(1))/$$(TARGET_$(1))/lib -lc" \
		all >$$$$log 2>&1; then \
		cat $$$$log; echo "freestanding guard ($(1)): a libc link was accepted"; exit 1; \
	fi; \
	grep -q "freestanding link pulled libc.a" $$$$log || { cat $$$$log; echo "freestanding guard ($(1)): the link failed for another reason"; exit 1; }; \
	echo "freestanding guard ($(1)): libc link rejected"
endef
$(foreach p,$(PROFILES),$(eval $(call GUARD_RULE,$(p))))

# The conformance catalogue (conformance/TESTS.md) against the payload and runner
# sources and the runner's generated header: no orphan ids either way.
check-conf-catalogue:
	$(PYTHON) $(AAR_SDK)/conformance/tools/conf_catalogue.py check
	$(PYTHON) $(AAR_SDK)/conformance/tests/tools_test.py
	$(PYTHON) $(AAR_SDK)/conformance/negative/gen_negative.py --check

# Every conformance payload built for every profile and checked by aar-check.
check-conf-payloads: $(foreach p,$(PROFILES),$(foreach a,$(CONF_APPS),check-$(p)-$(a)))

# libaar and the conformance payloads on a real instruction stream under qemu
# user mode: init arrays, the libc glue, the freestanding flavour, and the
# payloads of CONF_QEMU_AREAS. Not part of check, because it needs qemu and
# Linux cross compilers that the SDK image does not carry; each profile prints
# "skipped" when they are missing.
check-runtime: $(foreach p,$(PROFILES),check-runtime-$(p))

define RUNTIME_RULE
.PHONY: check-runtime-$(1)
check-runtime-$(1):
	CROSS=$$(CROSS_$(1)) sh $$(AAR_SDK)/libaar/tests/runtime_test.sh $$(AAR_SDK) $(1)
	CROSS=$$(CROSS_$(1)) sh $$(AAR_SDK)/conformance/tests/qemu_run.sh $$(AAR_SDK) $(1) $$(CONF_QEMU_AREAS)
	sh $$(AAR_SDK)/conformance/tests/negative_run.sh $$(AAR_SDK) $(1)
endef
$(foreach p,$(PROFILES),$(eval $(call RUNTIME_RULE,$(p))))

clean:
	rm -rf $(AAR_SDK)/build
