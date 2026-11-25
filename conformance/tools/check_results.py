#!/usr/bin/env python3
# Checks AAR-CONF output against conformance/TESTS.md.
#
#   check_results.py payload --area C-TIME --profile host|a32|a64 [--timeout S]
#                            -- command...
#       Runs one payload (the host executable, or qemu and a loader) and checks
#       its lines: every id the catalogue says the payload prints is there once,
#       nothing else is, each is PASS (or SKIP with a "host:" reason, which only
#       a run outside a platform may have), the end line counts match and the
#       command exits 0.
#
#   check_results.py report [--require-conforms] FILE
#       Validates a result file written by aar-conf (a platform's report): the
#       suite line, the AARInfo lines, one line per applicable test, a summary
#       whose counts match, and a verdict that is the one spec 11.1 gives.
#
# SPDX-License-Identifier: MIT
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conf_catalogue  # noqa: E402

RESULT = re.compile(r"^AAR-CONF (\S+) (PASS|FAIL|SKIP)(?: (.*))?$")
END = re.compile(r"^AAR-CONF-END (\S+) pass=(\d+) fail=(\d+) skip=(\d+)$")


def applies(test, profile):
    if profile == "host":
        return test.profile == "any"
    return test.profile in ("any", profile)


def _must_skip_reason():
    """The reason a payload may skip a MUST row with: CONF_SKIP_NO_PLATFORM_ROUTE of
    payloads/conf_proto.h, the text the runner accepts too (runner/conf_select.h)."""
    path = os.path.join(conf_catalogue.PAYLOAD_DIR, "conf_proto.h")
    with open(path, encoding="utf-8") as fh:
        m = re.search(r'#define CONF_SKIP_NO_PLATFORM_ROUTE "([^"]+)"', fh.read())
    if not m:
        raise SystemExit("conf_proto.h has no CONF_SKIP_NO_PLATFORM_ROUTE")
    return m.group(1)


MUST_SKIP_OK = _must_skip_reason()


def run_payload(args):
    version, tests, errors = conf_catalogue.load()
    if errors:
        for e in errors:
            print("catalogue: " + e, file=sys.stderr)
        return 1
    expected = {t.id: t for t in tests
                if t.area == args.area and t.judge == "payload"
                and t.impl == "yes" and t.group == "core"
                and applies(t, args.profile)}
    # A payload may also print the ids of a group that the host runner or the
    # loader declares (C-TABLE.4 with the display group); those are allowed, not
    # required: the standalone run has no say over what the environment declares.
    allowed = {t.id for t in tests
               if t.area == args.area and t.judge == "payload"
               and t.impl == "yes" and applies(t, args.profile)}
    if not expected:
        print("%s: the catalogue has no implemented core payload test for"
              " profile %s" % (args.area, args.profile), file=sys.stderr)
        return 1
    env = dict(os.environ)
    env.setdefault("SDL_VIDEODRIVER", "dummy")
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    try:
        proc = subprocess.run(args.command, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env,
                              timeout=args.timeout, check=False)
    except subprocess.TimeoutExpired:
        print("%s: the payload did not finish in %d s" % (args.area, args.timeout),
              file=sys.stderr)
        return 1
    text = proc.stdout.decode("utf-8", "replace")
    problems = []
    seen = {}
    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    end = None
    for line in text.splitlines():
        line = line.rstrip("\r")
        m = RESULT.match(line)
        if m:
            id_, verdict, detail = m.group(1), m.group(2), m.group(3) or ""
            counts[verdict] += 1
            if id_ in seen:
                problems.append("%s reported twice" % id_)
            seen[id_] = (verdict, detail)
            if id_ not in allowed:
                problems.append("%s is not an id of this payload" % id_)
            elif verdict == "FAIL":
                problems.append("%s FAIL %s" % (id_, detail))
            elif verdict == "SKIP" and not detail.startswith("host:"):
                problems.append("%s SKIP without a host: reason: %r" % (id_, detail))
            continue
        m = END.match(line)
        if m:
            if end is not None:
                problems.append("two end lines")
            end = (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
    for id_ in sorted(expected):
        if id_ not in seen:
            problems.append("%s was not reported" % id_)
    if end is None:
        problems.append("no AAR-CONF-END line")
    else:
        if end[0] != args.area:
            problems.append("end line names %s" % end[0])
        if (end[1], end[2], end[3]) != (counts["PASS"], counts["FAIL"], counts["SKIP"]):
            problems.append("end line counts %s do not match the %s result lines"
                            % (end[1:], counts))
    if proc.returncode != 0:
        problems.append("exit code %d" % proc.returncode)
    if problems:
        sys.stderr.write(text)
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        for p in problems:
            print("%s: %s" % (args.area, p), file=sys.stderr)
        return 1
    skips = counts["SKIP"]
    print("%s: %d ids, %d PASS, %d SKIP (host)" % (args.area, len(seen),
                                                   counts["PASS"], skips))
    return 0


def check_report(args):
    version, tests, errors = conf_catalogue.load()
    if errors:
        for e in errors:
            print("catalogue: " + e, file=sys.stderr)
        return 1
    with open(args.file, encoding="utf-8", errors="replace") as fh:
        lines = [l.rstrip("\r\n") for l in fh]
    problems = []
    suite = None
    info = {}
    results = {}
    summary = None
    for line in lines:
        if not line or line.startswith("#"):
            continue
        if line.startswith("AAR-CONF-SUITE "):
            suite = line.split()[1:]
        elif line.startswith("AAR-CONF-INFO "):
            k, _, v = line[len("AAR-CONF-INFO "):].partition("=")
            info[k] = v
        elif line.startswith("AAR-CONF-SUMMARY "):
            summary = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
        else:
            m = RESULT.match(line)
            if m:
                if m.group(1) in results:
                    problems.append("%s appears twice" % m.group(1))
                results[m.group(1)] = (m.group(2), m.group(3) or "")
    if suite is None or len(suite) != 2:
        problems.append("no AAR-CONF-SUITE <version> <profile> line")
        profile = "a32"
    else:
        profile = suite[1]
        if profile not in ("a32", "a64"):
            # The coverage below depends on the profile: a report that names another
            # one would be checked against too few tests.
            problems.append("unknown profile %r in the suite line (a32 or a64)" % profile)
            profile = "a32"
        if suite[0] != version:
            problems.append("suite version %s, the catalogue is %s"
                            % (suite[0], version))
    if "platform_id" not in info or "groups" not in info:
        problems.append("the AARInfo dump lacks platform_id or groups")
    groups = int(info.get("groups", "0"), 0)
    platform = int(info.get("platform_id", "0"), 0)

    def declared(t):
        if t.group == "core":
            return True
        if t.group == "zz9000":
            return platform == 1
        return bool(groups & conf_catalogue.GROUPS[t.group])

    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    must_fail = must_skip = 0
    filtered = summary is not None and summary.get("verdict") == "PARTIAL"
    by_id = {t.id: t for t in tests}
    for id_ in results:
        if id_ not in by_id:
            problems.append("%s is not in the catalogue" % id_)
    for t in tests:
        if not t.tested or not applies(t, profile):
            if t.id in results:
                problems.append("%s is reported but is not a test for %s" % (t.id, profile))
            continue
        if t.id not in results:
            if not filtered:
                problems.append("%s is missing" % t.id)
            continue
        verdict, detail = results[t.id]
        counts[verdict] += 1
        if verdict == "FAIL" and t.level == "MUST":
            must_fail += 1
        if verdict == "SKIP":
            if not detail:
                problems.append("%s SKIP without a reason" % t.id)
            if t.level == "MUST" and declared(t) and not (
                    t.route_skip and detail.startswith(MUST_SKIP_OK)):
                must_skip += 1
    conforms = must_fail == 0 and must_skip == 0
    if summary is None:
        problems.append("no AAR-CONF-SUMMARY line")
    else:
        for key, word in (("pass", "PASS"), ("fail", "FAIL"), ("skip", "SKIP")):
            if summary.get(key) != str(counts[word]):
                problems.append("summary %s=%s, the lines say %d"
                                % (key, summary.get(key), counts[word]))
        if not filtered:
            want = "CONFORMS" if conforms else "DOES-NOT-CONFORM"
            if summary.get("verdict") != want:
                problems.append("verdict %s, spec 11.1 gives %s"
                                % (summary.get("verdict"), want))
    if args.require_conforms and not (conforms and not filtered):
        problems.append("the platform does not conform: %d MUST failed, %d MUST"
                        " skipped" % (must_fail, must_skip))
    if problems:
        for p in problems:
            print("report: " + p, file=sys.stderr)
        return 1
    print("report: valid, %d PASS, %d FAIL, %d SKIP, %s" % (
        counts["PASS"], counts["FAIL"], counts["SKIP"],
        "CONFORMS" if conforms else "DOES-NOT-CONFORM"))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    p = sub.add_parser("payload")
    p.add_argument("--area", required=True)
    p.add_argument("--profile", required=True, choices=("host", "a32", "a64"))
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("command", nargs=argparse.REMAINDER)
    r = sub.add_parser("report")
    r.add_argument("--require-conforms", action="store_true")
    r.add_argument("file")
    args = ap.parse_args(argv[1:])
    if args.mode == "payload":
        if args.command and args.command[0] == "--":
            args.command = args.command[1:]
        if not args.command:
            ap.error("payload needs a command after --")
        return run_payload(args)
    return check_report(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
