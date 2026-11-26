# AAR app SDK: Arm GNU Toolchain 15.3.rel2 for arm-none-eabi (profile A32) and
# aarch64-none-elf (profile A64), a PIC newlib and a PIC C++ runtime (libstdc++,
# libsupc++, libgcc_eh) for each, and the SDK sources in /opt/aar-sdk. Build
# from the repository root:
#   docker build -f docker/Dockerfile -t ghcr.io/blitterstudio/aar-sdk .
#
# The image sets AAR_SDK, AAR_SYSROOT_A32 and AAR_SYSROOT_A64, which
# mk/aar.mk, cmake/aar-toolchain-*.cmake and tools use. Both toolchains are
# x86_64 or aarch64 builds, chosen by the build machine.
#
# SPDX-License-Identifier: MIT

FROM debian:bookworm-slim AS build-base

ENV DEBIAN_FRONTEND=noninteractive
# texinfo: libgloss builds an info page, and the firmware CI installs it too.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl xz-utils texinfo \
    && rm -rf /var/lib/apt/lists/*

# ---- Toolchains. Checksums are from Arm's published .sha256asc files next to
# each archive on https://gitlab.arm.com/tooling/gnu-toolchains-for-arm.
FROM build-base AS toolchain-a32

ARG ARM_TOOLCHAIN_VERSION=15.3.rel2
ARG SHA256_X86_64=463df9665c246fa13bab2102115bbf83440661b6eb70b44f3dfd4712ca30037a
ARG SHA256_AARCH64=a0dc18893171d39dec82fd8d57eeab3da836fd3df4b61978db3eccbf4a6bb948
RUN set -eu; \
    case "$(uname -m)" in \
        x86_64)  host=x86_64;  sha="${SHA256_X86_64}" ;; \
        aarch64) host=aarch64; sha="${SHA256_AARCH64}" ;; \
        *) echo "unsupported build machine: $(uname -m)" >&2; exit 1 ;; \
    esac; \
    base="arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-${host}-arm-none-eabi"; \
    curl -fsSL "https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/${ARM_TOOLCHAIN_VERSION}/${base}.tar.xz" -o /tmp/toolchain.tar.xz; \
    echo "${sha}  /tmp/toolchain.tar.xz" | sha256sum -c -; \
    mkdir -p /opt/arm-gnu-toolchain-a32; \
    tar -xJf /tmp/toolchain.tar.xz --strip-components=1 -C /opt/arm-gnu-toolchain-a32; \
    rm /tmp/toolchain.tar.xz; \
    rm -rf /opt/arm-gnu-toolchain-a32/share/doc /opt/arm-gnu-toolchain-a32/share/info /opt/arm-gnu-toolchain-a32/share/man

FROM build-base AS toolchain-a64

ARG ARM_TOOLCHAIN_VERSION=15.3.rel2
ARG SHA256_X86_64=1e4dc409cf4d33ef0a2e92c345f9e340302d355858de9bc3353758b6463895b0
ARG SHA256_AARCH64=63942f3f86e69d7beefa2b946bdb23437628529e7b29f97d32229f65d0905e40
RUN set -eu; \
    case "$(uname -m)" in \
        x86_64)  host=x86_64;  sha="${SHA256_X86_64}" ;; \
        aarch64) host=aarch64; sha="${SHA256_AARCH64}" ;; \
        *) echo "unsupported build machine: $(uname -m)" >&2; exit 1 ;; \
    esac; \
    base="arm-gnu-toolchain-${ARM_TOOLCHAIN_VERSION}-${host}-aarch64-none-elf"; \
    curl -fsSL "https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/${ARM_TOOLCHAIN_VERSION}/${base}.tar.xz" -o /tmp/toolchain.tar.xz; \
    echo "${sha}  /tmp/toolchain.tar.xz" | sha256sum -c -; \
    mkdir -p /opt/arm-gnu-toolchain-a64; \
    tar -xJf /tmp/toolchain.tar.xz --strip-components=1 -C /opt/arm-gnu-toolchain-a64; \
    rm /tmp/toolchain.tar.xz; \
    rm -rf /opt/arm-gnu-toolchain-a64/share/doc /opt/arm-gnu-toolchain-a64/share/info /opt/arm-gnu-toolchain-a64/share/man

# ---- PIC newlib, one build stage per profile so BuildKit runs them side by
# side.
FROM toolchain-a32 AS newlib-a32

COPY build_pic_newlib.sh /tmp/build_pic_newlib.sh
RUN chmod 755 /tmp/build_pic_newlib.sh \
    && PATH=/opt/arm-gnu-toolchain-a32/bin:${PATH} \
       AAR_PROFILE=a32 \
       AAR_SYSROOT=/opt/aar-sysroot-a32 \
       AAR_NEWLIB_CACHE=/tmp/newlib-cache \
       AAR_NEWLIB_BUILD=/tmp/newlib-build \
       JOBS="$(nproc)" \
       /tmp/build_pic_newlib.sh \
    && rm -rf /tmp/newlib-cache /tmp/newlib-build /tmp/build_pic_newlib.sh

FROM toolchain-a64 AS newlib-a64

COPY build_pic_newlib.sh /tmp/build_pic_newlib.sh
RUN chmod 755 /tmp/build_pic_newlib.sh \
    && PATH=/opt/arm-gnu-toolchain-a64/bin:${PATH} \
       AAR_PROFILE=a64 \
       AAR_SYSROOT=/opt/aar-sysroot-a64 \
       AAR_NEWLIB_CACHE=/tmp/newlib-cache \
       AAR_NEWLIB_BUILD=/tmp/newlib-build \
       JOBS="$(nproc)" \
       /tmp/build_pic_newlib.sh \
    && rm -rf /tmp/newlib-cache /tmp/newlib-build /tmp/build_pic_newlib.sh

# ---- PIC C++ runtime, built from the GCC source of the toolchain release into
# the same sysroot as newlib. The stage needs the host libraries that GCC's own
# configure wants.
FROM build-base AS cxx-base

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        flex libgmp-dev libmpfr-dev libmpc-dev \
    && rm -rf /var/lib/apt/lists/*

FROM cxx-base AS cxx-a32

COPY --from=toolchain-a32 /opt/arm-gnu-toolchain-a32 /opt/arm-gnu-toolchain-a32
COPY build_pic_libstdcxx.sh /tmp/build_pic_libstdcxx.sh
RUN chmod 755 /tmp/build_pic_libstdcxx.sh \
    && PATH=/opt/arm-gnu-toolchain-a32/bin:${PATH} \
       AAR_PROFILE=a32 \
       AAR_SYSROOT=/opt/aar-sysroot-a32 \
       AAR_GCC_CACHE=/tmp/gcc-cache \
       AAR_GCC_BUILD=/tmp/gcc-build \
       JOBS="$(nproc)" \
       /tmp/build_pic_libstdcxx.sh \
    && rm -rf /tmp/gcc-cache /tmp/gcc-build /tmp/build_pic_libstdcxx.sh

FROM cxx-base AS cxx-a64

COPY --from=toolchain-a64 /opt/arm-gnu-toolchain-a64 /opt/arm-gnu-toolchain-a64
COPY build_pic_libstdcxx.sh /tmp/build_pic_libstdcxx.sh
RUN chmod 755 /tmp/build_pic_libstdcxx.sh \
    && PATH=/opt/arm-gnu-toolchain-a64/bin:${PATH} \
       AAR_PROFILE=a64 \
       AAR_SYSROOT=/opt/aar-sysroot-a64 \
       AAR_GCC_CACHE=/tmp/gcc-cache \
       AAR_GCC_BUILD=/tmp/gcc-build \
       JOBS="$(nproc)" \
       /tmp/build_pic_libstdcxx.sh \
    && rm -rf /tmp/gcc-cache /tmp/gcc-build /tmp/build_pic_libstdcxx.sh

# ---- The SDK image.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates make cmake python3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain-a32 /opt/arm-gnu-toolchain-a32 /opt/arm-gnu-toolchain-a32
COPY --from=toolchain-a64 /opt/arm-gnu-toolchain-a64 /opt/arm-gnu-toolchain-a64
COPY --from=newlib-a32 /opt/aar-sysroot-a32 /opt/aar-sysroot-a32
COPY --from=newlib-a64 /opt/aar-sysroot-a64 /opt/aar-sysroot-a64
COPY --from=cxx-a32 /opt/aar-sysroot-a32 /opt/aar-sysroot-a32
COPY --from=cxx-a64 /opt/aar-sysroot-a64 /opt/aar-sysroot-a64
COPY . /opt/aar-sdk

ENV PATH=/opt/arm-gnu-toolchain-a32/bin:/opt/arm-gnu-toolchain-a64/bin:${PATH}
ENV AAR_SDK=/opt/aar-sdk
ENV AAR_SYSROOT_A32=/opt/aar-sysroot-a32
ENV AAR_SYSROOT_A64=/opt/aar-sysroot-a64
WORKDIR /opt/aar-sdk
