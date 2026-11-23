#!/usr/bin/env python3
# Tests of conformance/tools/conf_bundle.py: the bundle a platform developer
# downloads must hold the runner, every payload of both profiles and the whole
# corpus, and its MANIFEST.txt must carry the right hashes. A bundle that lacks
# one of them must not get a manifest.
#
# SPDX-License-Identifier: MIT
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
import conf_bundle  # noqa: E402
import conf_catalogue  # noqa: E402

BUNDLE = os.path.join(TOOLS, "conf_bundle.py")
COMMIT = "0123456789abcdef0123456789abcdef01234567"
failures = []


def expect(name, cond):
    if not cond:
        failures.append(name)
        print("FAIL " + name)


def run(*args):
    return subprocess.run([sys.executable, BUNDLE, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60)


def fake_build(root, skip=None):
    """A build tree with one fake ELF per payload and profile."""
    for profile in conf_bundle.PROFILES:
        os.makedirs(os.path.join(root, profile))
        for area in conf_bundle.payload_areas():
            if skip == (profile, area):
                continue
            with open(os.path.join(root, profile, "conf_%s.elf" % area), "wb") as fh:
                fh.write(("%s %s\n" % (profile, area)).encode())


tmp = tempfile.mkdtemp(prefix="aar-bundle-test-")
try:
    areas = conf_bundle.payload_areas()
    expect("the payload areas are found", len(areas) >= 16)

    build = os.path.join(tmp, "build")
    fake_build(build)
    bundle = os.path.join(tmp, "bundle")

    # ---- stage ------------------------------------------------------------------
    r = run("stage", bundle, "--build", build)
    expect("stage succeeds on a full build tree", r.returncode == 0)
    for profile in conf_bundle.PROFILES:
        got = sorted(os.listdir(os.path.join(bundle, profile)))
        expect("stage: every payload of %s" % profile,
               got == ["conf_%s.elf" % a for a in areas])
    expect("stage: the corpus MANIFEST is copied",
           os.path.isfile(os.path.join(bundle, "negative", "MANIFEST")))
    expect("stage: the generator is not in the bundle",
           not os.path.exists(os.path.join(bundle, "negative", "gen_negative.py")))
    for profile in conf_bundle.PROFILES:
        src = sorted(f for f in os.listdir(os.path.join(conf_bundle.NEGATIVE_SRC, profile))
                     if f.endswith(".aarimg"))
        got = sorted(os.listdir(os.path.join(bundle, "negative", profile)))
        expect("stage: the whole %s corpus (%d images)" % (profile, len(src)),
               got == src and len(src) > 0)

    shutil.rmtree(os.path.join(tmp, "bundle2"), ignore_errors=True)
    broken = os.path.join(tmp, "broken")
    fake_build(broken, skip=("a64", "time"))
    r = run("stage", os.path.join(tmp, "bundle2"), "--build", broken)
    expect("stage fails when one A64 payload is missing", r.returncode != 0)
    expect("... and names it", b"a64" in r.stderr and b"conf_time.elf" in r.stderr)

    # ---- manifest ---------------------------------------------------------------
    r = run("manifest", bundle, "--commit", COMMIT)
    expect("manifest fails without aar-conf", r.returncode != 0 and b"aar-conf" in r.stderr)
    expect("... and writes no manifest", not os.path.exists(os.path.join(bundle, "MANIFEST.txt")))

    with open(os.path.join(bundle, "aar-conf"), "wb") as fh:
        fh.write(b"fake runner\n")
    r = run("manifest", bundle, "--commit", COMMIT)
    expect("manifest succeeds on a full tree", r.returncode == 0)
    text = open(os.path.join(bundle, "MANIFEST.txt"), encoding="utf-8", newline="").read()
    version = conf_catalogue.load()[0]
    expect("manifest: suite version", ("suite_version: %s\n" % version) in text)
    expect("manifest: source commit", ("source_commit: %s\n" % COMMIT) in text)
    expect("manifest: LF only", "\r" not in text)
    expect("manifest: run command for a32",
           "aar-conf DIR=<this directory> PROFILE=a32 TO=RAM:conf-a32.txt" in text)
    expect("manifest: run command for an A64 platform",
           "PROFILE=a64 TO=RAM:conf-a64.txt" in text)
    expect("manifest: INTERACTIVE", " INTERACTIVE\n" in text)
    _version, tests, _errors = conf_catalogue.load()
    for area in sorted({t.area for t in tests}):
        expect("manifest: ONLY for %s" % area, "ONLY=%s\n" % area in text)

    listed = {}
    for m in re.finditer(r"^([0-9a-f]{64})  (\S+)$", text, re.M):
        listed[m.group(2)] = m.group(1)
    on_disk = {}
    for base, _dirs, names in os.walk(bundle):
        for name in names:
            rel = os.path.relpath(os.path.join(base, name), bundle).replace(os.sep, "/")
            if rel != "MANIFEST.txt":
                on_disk[rel] = hashlib.sha256(open(os.path.join(base, name), "rb").read()).hexdigest()
    expect("manifest: a sha256 line for every file and no other", listed == on_disk)
    expect("manifest: the runner is listed", "aar-conf" in listed)
    expect("manifest: payloads of both profiles",
           all(("%s/conf_%s.elf" % (p, a)) in listed
               for p in conf_bundle.PROFILES for a in areas))

    # A second run replaces the manifest and does not list it.
    r = run("manifest", bundle, "--commit", COMMIT)
    text2 = open(os.path.join(bundle, "MANIFEST.txt"), encoding="utf-8", newline="").read()
    expect("manifest: running again gives the same manifest", r.returncode == 0 and text2 == text)
    expect("manifest: MANIFEST.txt does not list itself", "  MANIFEST.txt" not in text)

    # ---- a tree with a hole gets no manifest ----------------------------------------
    os.remove(os.path.join(bundle, "a64", "conf_mem.elf"))
    os.remove(os.path.join(bundle, "MANIFEST.txt"))
    r = run("manifest", bundle, "--commit", COMMIT)
    expect("manifest fails when a payload is missing", r.returncode != 0)
    expect("... and names it", b"a64/conf_mem.elf" in r.stderr)
    shutil.copyfile(os.path.join(build, "a64", "conf_mem.elf"),
                    os.path.join(bundle, "a64", "conf_mem.elf"))
    images = conf_bundle.manifest_images(os.path.join(bundle, "negative"))
    name, profile = images[0]
    os.remove(os.path.join(bundle, "negative", profile, name + ".aarimg"))
    r = run("manifest", bundle, "--commit", COMMIT)
    expect("manifest fails when a corpus image is missing", r.returncode != 0)
    expect("... and names it", ("negative/%s/%s.aarimg" % (profile, name)).encode() in r.stderr)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if failures:
    print("%d conformance bundle tests failed" % len(failures))
    sys.exit(1)
print("conformance bundle: ok")
