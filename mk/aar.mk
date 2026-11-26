# One AAR app. The app Makefile sets AAR_SDK, APP and SRCS, then includes this
# file. Optional CFLAGS, CXXFLAGS and LDLIBS are appended.
#
#   AAR_SDK ?= /opt/aar-sdk
#   AAR_PROFILE ?= a32
#   APP := hello
#   SRCS := hello.c
#   include $(AAR_SDK)/mk/aar.mk
#
# One source must use AAR_NOTE() from aar/note.h. `make`, `make check` and
# `make clean` build build/<profile>/<app>.elf (and .map), run aar-check on it,
# and remove the objects. SRCS lists C sources (.c) and C++ sources (.cpp, .cc,
# .cxx).
#
#   AAR_SDK            SDK root, the directory that holds mk/aar.mk (required)
#   AAR_PROFILE        a32 (default) or a64
#   AAR_SYSROOT_A32    PIC newlib and C++ runtime sysroots from
#   AAR_SYSROOT_A64    build_pic_newlib.sh and build_pic_libstdcxx.sh; the one
#                      for the profile is needed unless AAR_FREESTANDING=1
#   AAR_FREESTANDING=1 build without newlib. crt0 calls aar_exit, libaar
#                      leaves out syscalls.c and malloc, the link takes -lgcc
#                      but no -lc or -lm, and the build fails if the link map
#                      holds an archive other than libaar.a and libgcc.a. No
#                      sysroot is needed in that mode. C++ sources are refused:
#                      the C++ runtime needs newlib.
#   AAR_CXX_EXCEPTIONS 1 (default) compiles C++ with -fexceptions and keeps the
#                      unwind tables in the image, so throw and catch work. 0
#                      compiles with -fno-exceptions and no unwind tables.
#                      Either way the link takes libstdc++, libgcc_eh and
#                      libc, and a C++ image uses libaar/<profile>/aar-cxx.ld.
#   CROSS              toolchain prefix; arm-none-eabi- for a32,
#                      aarch64-none-elf- for a64
#   BUILD              output directory, default build/<profile>
#   PYTHON             default python3
#
# See the developer guide under docs/.
#
# SPDX-License-Identifier: MIT

ifeq ($(strip $(AAR_SDK)),)
$(error Set AAR_SDK to the SDK root, the directory that holds mk/aar.mk)
endif
ifeq ($(strip $(APP)),)
$(error Set APP to the output name)
endif
ifeq ($(strip $(SRCS)),)
$(error Set SRCS to the app sources)
endif

AAR_PROFILE ?= a32
override AAR_PROFILE := $(strip $(AAR_PROFILE))
ifeq ($(filter $(AAR_PROFILE),a32 a64),)
$(error AAR_PROFILE must be a32 or a64)
endif

override AAR_FREESTANDING := $(strip $(or $(AAR_FREESTANDING),0))
ifneq ($(filter-out 0 1,$(AAR_FREESTANDING)),)
$(error AAR_FREESTANDING must be 0 or 1)
endif

ifeq ($(AAR_PROFILE),a32)
CROSS ?= arm-none-eabi-
AAR_PROFILE_FLAGS := -mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard
AAR_SYSROOT := $(AAR_SYSROOT_A32)
AAR_SYSROOT_LIBDIR := $(AAR_SYSROOT)/arm-none-eabi/lib
AAR_SYSROOT_VAR := AAR_SYSROOT_A32
else
CROSS ?= aarch64-none-elf-
AAR_PROFILE_FLAGS := -march=armv8-a
AAR_SYSROOT := $(AAR_SYSROOT_A64)
AAR_SYSROOT_LIBDIR := $(AAR_SYSROOT)/aarch64-none-elf/lib
AAR_SYSROOT_VAR := AAR_SYSROOT_A64
endif

CC := $(CROSS)gcc
AR := $(CROSS)ar
PYTHON ?= python3
BUILD ?= build/$(AAR_PROFILE)

AAR_CXX_SRCS := $(filter %.cpp %.cc %.cxx,$(SRCS))
ifneq ($(strip $(filter-out %.c %.cpp %.cc %.cxx,$(SRCS))),)
$(error SRCS holds a file that is not .c, .cpp, .cc or .cxx: $(filter-out %.c %.cpp %.cc %.cxx,$(SRCS)))
endif
CXX := $(CROSS)g++

override AAR_CXX_EXCEPTIONS := $(strip $(or $(AAR_CXX_EXCEPTIONS),1))
ifneq ($(filter-out 0 1,$(AAR_CXX_EXCEPTIONS)),)
$(error AAR_CXX_EXCEPTIONS must be 0 or 1)
endif

ifeq ($(AAR_FREESTANDING),0)
ifeq ($(strip $(AAR_SYSROOT)),)
$(error Set $(AAR_SYSROOT_VAR) to the output of build_pic_newlib.sh for profile $(AAR_PROFILE))
endif
ifneq ($(strip $(AAR_CXX_SRCS)),)
ifeq ($(wildcard $(AAR_SYSROOT_LIBDIR)/libstdc++.a),)
$(error C++ sources need the PIC C++ runtime: run build_pic_libstdcxx.sh for profile $(AAR_PROFILE) into $(AAR_SYSROOT_VAR) ($(AAR_SYSROOT_LIBDIR) has no libstdc++.a))
endif
ifeq ($(wildcard $(AAR_SYSROOT_LIBDIR)/libgcc_eh.a),)
$(error C++ sources need the PIC C++ runtime: run build_pic_libstdcxx.sh for profile $(AAR_PROFILE) into $(AAR_SYSROOT_VAR) ($(AAR_SYSROOT_LIBDIR) has no libgcc_eh.a))
endif
endif
else
ifneq ($(strip $(AAR_CXX_SRCS)),)
$(error AAR_FREESTANDING=1 cannot build the C++ sources $(AAR_CXX_SRCS): the freestanding flavour has no C++ runtime, use the libc flavour)
endif
endif

# Capture the app's flags before this file appends the required ones.
AAR_EXTRA_CFLAGS := $(CFLAGS)
AAR_EXTRA_CXXFLAGS := $(CXXFLAGS)
AAR_EXTRA_LDLIBS := $(LDLIBS)

ifeq ($(AAR_FREESTANDING),1)
AAR_MODE_DEFS := -DAAR_FREESTANDING=1
endif
# -ffp-contract=off keeps a multiply and an add separate, so results do not
# depend on whether the profile has a fused instruction (A64 always has).
AAR_APP_CFLAGS := $(AAR_PROFILE_FLAGS) -fPIE -O2 -fno-strict-aliasing \
	-ffunction-sections -fdata-sections -fno-exceptions \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -ffp-contract=off \
	-Wall -Wextra -Werror -std=c11 -I$(AAR_SDK)/include \
	$(AAR_MODE_DEFS) $(AAR_EXTRA_CFLAGS)
AAR_LIB_CFLAGS := $(AAR_APP_CFLAGS) -I$(AAR_SDK)/libaar/src
# C++ gets the same profile and code-generation flags. Exceptions are on by
# default: -fexceptions makes the compiler emit .ARM.exidx (A32) or .eh_frame
# (A64), which aar-cxx.ld keeps. RTTI stays on; add -fno-rtti to CXXFLAGS to
# drop it.
ifeq ($(AAR_CXX_EXCEPTIONS),1)
AAR_CXX_EH_FLAGS := -fexceptions
else
AAR_CXX_EH_FLAGS := -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables
endif
AAR_APP_CXXFLAGS := $(AAR_PROFILE_FLAGS) -fPIE -O2 -fno-strict-aliasing \
	-ffunction-sections -fdata-sections $(AAR_CXX_EH_FLAGS) \
	-ffp-contract=off -Wall -Wextra -Werror -std=c++17 \
	-I$(AAR_SDK)/include $(AAR_MODE_DEFS) $(AAR_EXTRA_CXXFLAGS)
AAR_DEPFLAGS := -MMD -MP
AAR_ELF := $(BUILD)/$(APP).elf
AAR_MAP := $(BUILD)/$(APP).map
# A C++ image links with aar-cxx.ld, which keeps the unwind tables.
AAR_LD := $(AAR_SDK)/libaar/$(AAR_PROFILE)/aar$(if $(AAR_CXX_SRCS),-cxx).ld
AAR_LDFLAGS := $(AAR_PROFILE_FLAGS) -nostdlib -fPIE -pie \
	-Wl,-T,$(AAR_LD) -Wl,--gc-sections \
	-Wl,--no-dynamic-linker -Wl,-z,text -Wl,-Bstatic -Wl,--no-undefined \
	-Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
	-Wl,-Map=$(AAR_MAP)

# Objects and the archive live in a per-app, per-flavour directory, so
# switching AAR_FREESTANDING in one BUILD never reuses the other flavour's
# crt0.o, aar.o or archive members.
ifeq ($(AAR_FREESTANDING),1)
OBJ := $(BUILD)/obj/$(APP)/freestanding
AAR_SYSLIBS := -lgcc
LIB_OBJS := $(OBJ)/libaar/aar.o $(OBJ)/libaar/gl.o $(OBJ)/libaar/freestanding.o
else
# The object directory also records the exception mode, so switching it
# recompiles the C++ sources.
OBJ := $(BUILD)/obj/$(APP)/libc$(if $(filter 0,$(AAR_CXX_EXCEPTIONS)),-noexc)
AAR_LDFLAGS += -L$(AAR_SYSROOT_LIBDIR)
LIB_OBJS := $(OBJ)/libaar/aar.o $(OBJ)/libaar/gl.o $(OBJ)/libaar/syscalls.o
ifneq ($(strip $(AAR_CXX_SRCS)),)
# The C++ runtime comes before libc and libgcc, and libgcc_eh before libgcc. The
# libaar support object is linked as an object so its constructor is always in.
AAR_SYSLIBS := -lstdc++ -lc -lgcc_eh -lgcc -lm
AAR_CXX_OBJ := $(OBJ)/libaar/cxx.o
else
AAR_SYSLIBS := -lc -lgcc -lm
endif
endif

APP_OBJS := $(addprefix $(OBJ)/,$(addsuffix .o,$(basename $(SRCS))))
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(AAR_CXX_OBJ:.o=.d) $(OBJ)/crt0.d

.PHONY: all check clean FORCE
.DELETE_ON_ERROR:

all: $(AAR_ELF)

check: $(AAR_ELF)
	$(PYTHON) $(AAR_SDK)/tools/aar-check --profile $(AAR_PROFILE) $(AAR_ELF)

# The image is the same file in both flavours. The stamp holds the flavour of
# the last link and is rewritten only when that changes, so a change makes the
# image older than the stamp and forces the link.
AAR_MODE_STAMP := $(BUILD)/$(APP).mode

$(AAR_MODE_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo "$(AAR_FREESTANDING)" | cmp -s - $@ || echo "$(AAR_FREESTANDING)" > $@

FORCE:

$(AAR_ELF): $(OBJ)/crt0.o $(APP_OBJS) $(AAR_CXX_OBJ) $(OBJ)/libaar.a \
		$(AAR_MODE_STAMP) $(AAR_LD)
	@mkdir -p $(dir $@)
	$(CC) $(AAR_LDFLAGS) -o $@ -Wl,--start-group \
		$(filter %.o %.a,$^) $(AAR_EXTRA_LDLIBS) $(AAR_SYSLIBS) -Wl,--end-group
ifeq ($(AAR_FREESTANDING),1)
	$(PYTHON) $(AAR_SDK)/tools/aar-check --no-libc $(AAR_MAP)
endif

$(OBJ)/libaar.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(OBJ)/crt0.o: $(AAR_SDK)/libaar/$(AAR_PROFILE)/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(AAR_PROFILE_FLAGS) $(AAR_MODE_DEFS) -MMD -MP -MF $(OBJ)/crt0.d -c -o $@ $<

# The mem* routines must not be rewritten into calls to themselves.
$(OBJ)/libaar/freestanding.o: $(AAR_SDK)/libaar/src/freestanding.c
	@mkdir -p $(dir $@)
	$(CC) $(AAR_LIB_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns $(AAR_DEPFLAGS) -c -o $@ $<

$(OBJ)/libaar/%.o: $(AAR_SDK)/libaar/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(AAR_LIB_CFLAGS) $(AAR_DEPFLAGS) -c -o $@ $<

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AAR_APP_CFLAGS) $(AAR_DEPFLAGS) -c -o $@ $<

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(AAR_APP_CXXFLAGS) $(AAR_DEPFLAGS) -c -o $@ $<

$(OBJ)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(AAR_APP_CXXFLAGS) $(AAR_DEPFLAGS) -c -o $@ $<

$(OBJ)/%.o: %.cxx
	@mkdir -p $(dir $@)
	$(CXX) $(AAR_APP_CXXFLAGS) $(AAR_DEPFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)/obj/$(APP) $(AAR_ELF) $(AAR_MAP) $(AAR_MODE_STAMP)

-include $(DEPS)
