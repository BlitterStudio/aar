#!/usr/bin/env python3
# Tests of the conformance scripts themselves: the catalogue parser, the payload
# output checker and the report validator must accept good output and reject the
# bad kinds, so that a failing payload cannot pass by a sloppy checker.
#
# SPDX-License-Identifier: MIT
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
import conf_catalogue  # noqa: E402

CHECK = os.path.join(TOOLS, "check_results.py")
failures = []


def expect(name, cond):
    if not cond:
        failures.append(name)
        print("FAIL " + name)


def check(*args):
    return subprocess.run([sys.executable, CHECK, *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60).returncode


def fake_payload(lines):
    """A command that prints the lines and exits 0."""
    text = "\n".join(lines) + "\n"
    return [sys.executable, "-c", "import sys; sys.stdout.write(%r)" % text]


version, tests, errors = conf_catalogue.load()
expect("catalogue loads", not errors)
by_id = {t.id: t for t in tests}

# ---- payload output ---------------------------------------------------------------

area_ids = [t.id for t in tests if t.area == "C-TIME" and t.judge == "payload"
            and t.impl == "yes" and t.group == "core"]
expect("C-TIME has implemented payload ids", len(area_ids) >= 5)


def good(ids, skip=None):
    out = []
    for i in ids:
        if i == skip:
            out.append("AAR-CONF %s SKIP host: no peer" % i)
        else:
            out.append("AAR-CONF %s PASS fine" % i)
    n_skip = 1 if skip else 0
    out.append("AAR-CONF-END C-TIME pass=%d fail=0 skip=%d" % (len(ids) - n_skip, n_skip))
    return out


def payload(lines):
    return check("payload", "--area", "C-TIME", "--profile", "a32", "--",
                 *fake_payload(lines))


expect("good payload", payload(good(area_ids)) == 0)
expect("host skip is allowed", payload(good(area_ids, skip=area_ids[-1])) == 0)
expect("a missing id fails", payload(good(area_ids[:-1])) != 0)
expect("a FAIL fails", payload(
    [l.replace("PASS fine", "FAIL no") if "C-TIME.1 " in l else l
     for l in good(area_ids)]) != 0)
expect("a skip without host: fails", payload(
    [l.replace("PASS fine", "SKIP because") if "C-TIME.1 " in l else l
     for l in good(area_ids)]) != 0)
expect("a duplicate id fails", payload(good(area_ids) + good(area_ids[:1])[:1]) != 0)
expect("an unknown id fails", payload(
    good(area_ids)[:-1] + ["AAR-CONF C-TIME.99 PASS x",
                           "AAR-CONF-END C-TIME pass=%d fail=0 skip=0" % (len(area_ids) + 1)]) != 0)
expect("no end line fails", payload(good(area_ids)[:-1]) != 0)
expect("wrong end counts fail", payload(
    good(area_ids)[:-1] + ["AAR-CONF-END C-TIME pass=1 fail=0 skip=0"]) != 0)
expect("a nonzero exit fails", check(
    "payload", "--area", "C-TIME", "--profile", "a32", "--", sys.executable, "-c",
    "import sys; sys.stdout.write(%r); sys.exit(3)" % "\n".join(good(area_ids))) != 0)

# ---- reports ------------------------------------------------------------------------


def report(groups=0x17, platform=0, omit=(), override=None, summary_verdict=None,
           profile="a32"):
    override = override or {}
    lines = ["AAR-CONF-SUITE %s %s" % (version, profile),
             "AAR-CONF-INFO groups=0x%x" % groups,
             "AAR-CONF-INFO platform_id=%d" % platform,
             "# a comment"]
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    must_bad = False
    for t in tests:
        if not t.tested or t.profile not in ("any", profile) or t.id in omit:
            continue
        declared = (t.group == "core" or
                    (t.group == "zz9000" and platform == 1) or
                    (t.group in conf_catalogue.GROUPS and
                     groups & conf_catalogue.GROUPS[t.group]))
        if t.id in override:
            verdict, detail = override[t.id]
        elif not declared:
            verdict, detail = "SKIP", "group not declared"
        elif t.impl == "yes":
            verdict, detail = "PASS", "ok"
        else:
            verdict, detail = "SKIP", "suite: not implemented"
        counts[verdict] += 1
        if t.level == "MUST" and declared and verdict != "PASS":
            must_bad = True
        lines.append("AAR-CONF %s %s %s" % (t.id, verdict, detail))
    lines.append("AAR-CONF-SUMMARY pass=%d fail=%d skip=%d verdict=%s" % (
        counts["PASS"], counts["FAIL"], counts["SKIP"],
        summary_verdict or ("DOES-NOT-CONFORM" if must_bad else "CONFORMS")))
    return "\n".join(lines) + "\n"


def check_report(text, *extra):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
        fh.write(text)
        path = fh.name
    try:
        return check("report", *extra, path)
    finally:
        os.unlink(path)


# A report of a suite with a test that is not written (a MUST skipped).
UNFINISHED = {"C-TIME.1": ("SKIP", "suite: not implemented in this version")}
expect("a report of an unfinished suite is valid",
       check_report(report(override=UNFINISHED)) == 0)
expect("... and does not conform",
       check_report(report(override=UNFINISHED), "--require-conforms") != 0)
expect("a wrong verdict is rejected",
       check_report(report(override=UNFINISHED, summary_verdict="CONFORMS")) != 0)
expect("a missing line is rejected", check_report(report(omit=("C-TIME.1",))) != 0)
expect("a FAIL of a MUST test gives DOES-NOT-CONFORM", check_report(
    report(override={"C-TIME.1": ("FAIL", "went back")})) == 0)
expect("a SKIP without a reason is rejected", check_report(
    report(override={"C-TIME.2": ("SKIP", "")})) != 0)
expect("an a64 report skips the a32 tests", check_report(report(profile="a64")) == 0)
expect("an unknown profile is rejected, also for a report that would conform",
       check_report(report(profile="a16"), "--require-conforms") != 0)
expect("an unknown profile is rejected without --require-conforms",
       check_report(report(profile="x86")) != 0)

# A suite where every test passes conforms: flip the impl flags in memory.
for t in tests:
    t.impl = "yes"
conf_text = report()
expect("an all-PASS report conforms", "verdict=CONFORMS" in conf_text)
expect("... and is accepted with --require-conforms",
       check_report(conf_text, "--require-conforms") == 0)

if failures:
    print("%d conformance tool tests failed" % len(failures))
    sys.exit(1)
print("conformance tools: ok")
