# MiniGL conformance probe

`mgl-conformance.elf` exercises the in-process Arm MiniGL API through
`aar_gl_bind` and the `gl*` wrappers. It requires `AAR_GROUP_DISPLAY` and
`AAR_GROUP_GL` in its note, so a runtime without GL refuses to load it with
`AAR_E_NOGROUP`; the host runner has no GL group and the template exits with 2
there. The matching 68k executable is `mgl-conformance` from the
`amiberry-minigl-library` checkout. Both compile the one shared matrix in
`mgl_conformance_matrix.h`; set `MGL_CONFORMANCE_MATRIX_DIR` when the two
repositories are not sibling checkouts.

Build the Arm app with the normal SDK build, then copy it to the Amiga. The
`AAR_SYSROOT_A32` variable names the PIC newlib sysroot (the SDK image sets it):

```text
make build-a32-mgl-conformance
ZZRun RAM:mgl-conformance.elf
```

The ELF is `build/a32/mgl-conformance.elf`; `build-a64-mgl-conformance` builds
the A64 image for a platform that runs one.

Build the 68k app with the installed-kit library contract and run it from an
Amiga CLI after installing `minigl.library` and `MINIGL.ZZM`:

```text
make -C D:/Github/amiberry-minigl-library \
  MGL_CONFORMANCE_MATRIX_DIR=D:/Github/aar/templates \
  all importlib mgl-conformance
mgl-conformance
```

By default, each probe writes one `MGL` line for every texture-environment,
format, and draw-path group, followed by `MGL summary`. `pass` and `fail` are
the 112 cases in that group; `first-fail` records its first mismatched input
and observed/expected RGBA result. The Arm app also writes `MGL identity` and
`MGL start` first and spaces each log line by 150 ms. The identity line carries
the firmware and GL module revisions from the ZZ9000 extension table
(`aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)`) and says `unavailable`
where there is none. Use `-v` to print every failing case, or `-vv` to print
every case. `--surface-readback` also reads one pixel of the surface after the
swap. RGB565 readback is accepted within 8 per component; a nonzero exit
status means at least one case failed.
