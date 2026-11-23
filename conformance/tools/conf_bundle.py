#!/usr/bin/env python3
# Assembles the conformance bundle a platform developer downloads from CI:
#
#   <bundle>/aar-conf                the m68k runner (built by client/tests/m68k_check.sh)
#   <bundle>/a32/conf_<area>.elf     the payloads of every area, profile A32
#   <bundle>/a64/conf_<area>.elf     the same for profile A64
#   <bundle>/negative/MANIFEST       the C-ELF image corpus and its manifest, as
#   <bundle>/negative/a32/*.aarimg   the runner reads them
#   <bundle>/negative/a64/*.aarimg
#   <bundle>/MANIFEST.txt            suite version, source commit, run commands,
#                                    sha256 of every file
#
# Commands:
#
#   conf_bundle.py stage <bundle> [--build <dir>]
#       Copies the payloads of <dir>/a32 and <dir>/a64 (default: build/ of the
#       checkout) and the corpus into <bundle>. Fails when a profile or an area
#       is missing. `make conf-bundle` runs it after the payloads were built
#       and checked with aar-check.
#   conf_bundle.py manifest <bundle> --commit <sha>
#       Checks the whole tree (aar-conf present, 16 areas for each profile, every
#       corpus image the MANIFEST names) and writes <bundle>/MANIFEST.txt.
#
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
import conf_catalogue  # noqa: E402

PROFILES = ("a32", "a64")
RUNNER = "aar-conf"
MANIFEST_TXT = "MANIFEST.txt"
NEGATIVE_SRC = os.path.join(ROOT, "negative")


def payload_areas():
    """The areas that have a payload: conformance/payloads/conf_<area>.c."""
    names = []
    for f in os.listdir(conf_catalogue.PAYLOAD_DIR):
        if f.startswith("conf_") and f.endswith(".c"):
            names.append(f[len("conf_"):-len(".c")])
    return sorted(names)


def manifest_images(negative_dir):
    """(name, profile) of every image the corpus MANIFEST lists."""
    images = []
    with open(os.path.join(negative_dir, "MANIFEST"), encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            images.append((fields[0], fields[1]))
    return images


def problems_of(bundle):
    """Everything wrong with the tree in <bundle>, as a list of messages."""
    problems = []
    if not os.path.isfile(os.path.join(bundle, RUNNER)):
        problems.append("%s is missing (build it with client/tests/m68k_check.sh)" % RUNNER)
    areas = payload_areas()
    for profile in PROFILES:
        for area in areas:
            rel = "%s/conf_%s.elf" % (profile, area)
            if not os.path.isfile(os.path.join(bundle, rel)):
                problems.append("%s is missing" % rel)
    negative = os.path.join(bundle, "negative")
    if not os.path.isfile(os.path.join(negative, "MANIFEST")):
        problems.append("negative/MANIFEST is missing")
    else:
        for name, profile in manifest_images(negative):
            rel = "negative/%s/%s.aarimg" % (profile, name)
            if not os.path.isfile(os.path.join(bundle, rel)):
                problems.append("%s is missing (named by negative/MANIFEST)" % rel)
    return problems


def stage(bundle, build):
    os.makedirs(bundle, exist_ok=True)
    for name in PROFILES + ("negative", MANIFEST_TXT):
        path = os.path.join(bundle, name)
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.exists(path):
            os.remove(path)
    missing = []
    for profile in PROFILES:
        os.makedirs(os.path.join(bundle, profile))
        for area in payload_areas():
            src = os.path.join(build, profile, "conf_%s.elf" % area)
            if not os.path.isfile(src):
                missing.append(src)
                continue
            shutil.copyfile(src, os.path.join(bundle, profile, "conf_%s.elf" % area))
    if missing:
        for path in missing:
            print("conf_bundle: %s is missing: build the payloads for both profiles first" % path,
                  file=sys.stderr)
        return 1
    negative = os.path.join(bundle, "negative")
    os.makedirs(negative)
    shutil.copyfile(os.path.join(NEGATIVE_SRC, "MANIFEST"), os.path.join(negative, "MANIFEST"))
    for profile in PROFILES:
        os.makedirs(os.path.join(negative, profile))
        src_dir = os.path.join(NEGATIVE_SRC, profile)
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".aarimg"):
                shutil.copyfile(os.path.join(src_dir, f), os.path.join(negative, profile, f))
    return 0


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_of(bundle):
    """Relative paths (with /) of every file except MANIFEST.txt, sorted."""
    found = []
    for base, _dirs, names in os.walk(bundle):
        for name in names:
            rel = os.path.relpath(os.path.join(base, name), bundle).replace(os.sep, "/")
            if rel != MANIFEST_TXT:
                found.append(rel)
    return sorted(found)


def manifest_text(bundle, commit):
    version, tests, errors = conf_catalogue.load()
    if errors:
        raise SystemExit("conf_bundle: the catalogue has errors: " + "; ".join(errors))
    areas = []
    for t in tests:
        if t.area not in areas:
            areas.append(t.area)
    out = [
        "AAR conformance bundle",
        "",
        "suite_version: %s" % version,
        "source_commit: %s" % commit,
        "",
        "This directory is what the runner reads. Copy it to the platform as it is",
        "(the runner is the m68k program aar-conf, the payloads are ARM apps).",
        "",
        "Run (from an AmigaOS Shell, once per profile the platform declares):",
        "",
        "    Stack 32768",
        "    aar-conf DIR=<this directory> PROFILE=a32 TO=RAM:conf-a32.txt",
        "    aar-conf DIR=<this directory> PROFILE=a64 TO=RAM:conf-a64.txt",
        "",
        "A platform that runs only A64 (Emu68 on a Cortex-A72, a CM5 card):",
        "",
        "    aar-conf DIR=<this directory> PROFILE=a64 TO=RAM:conf-a64.txt",
        "",
        "With the operator tests (C-LIFE.24, C-DISPLAY.17, C-INPUT.11, C-AUDIO.12):",
        "",
        "    aar-conf DIR=<this directory> PROFILE=a64 TO=RAM:conf-a64.txt INTERACTIVE",
        "",
        "One test, or one area (the verdict is then PARTIAL):",
        "",
        "    aar-conf DIR=<this directory> PROFILE=a64 ONLY=C-INPUT.11 INTERACTIVE",
    ]
    for area in areas:
        out.append("    aar-conf DIR=<this directory> PROFILE=a64 ONLY=%s" % area)
    out += [
        "",
        "Check the report with conformance/tools/check_results.py of the source",
        "commit above:",
        "",
        "    python3 conformance/tools/check_results.py report conf-a64.txt --require-conforms",
        "",
        "Check the files of this bundle against the sha256 lines below:",
        "",
        "    grep -E '^[0-9a-f]{64}  ' MANIFEST.txt | sha256sum -c",
        "",
        "Files (sha256, two spaces, path relative to this directory):",
        "",
    ]
    for rel in files_of(bundle):
        out.append("%s  %s" % (sha256_of(os.path.join(bundle, rel)), rel))
    return "\n".join(out) + "\n"


def write_manifest(bundle, commit):
    problems = problems_of(bundle)
    if problems:
        for p in problems:
            print("conf_bundle: " + p, file=sys.stderr)
        return 1
    text = manifest_text(bundle, commit)
    with open(os.path.join(bundle, MANIFEST_TXT), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="Assembles the AAR conformance bundle")
    sub = parser.add_subparsers(dest="command", required=True)
    p_stage = sub.add_parser("stage")
    p_stage.add_argument("bundle")
    p_stage.add_argument("--build", default=os.path.join(ROOT, "..", "build"))
    p_manifest = sub.add_parser("manifest")
    p_manifest.add_argument("bundle")
    p_manifest.add_argument("--commit", required=True)
    args = parser.parse_args(argv)
    if args.command == "stage":
        return stage(args.bundle, os.path.normpath(args.build))
    return write_manifest(args.bundle, args.commit)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
