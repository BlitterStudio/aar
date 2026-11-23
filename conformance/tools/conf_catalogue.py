#!/usr/bin/env python3
# Reads conformance/TESTS.md. Commands:
#
#   conf_catalogue.py check      validate the table, check the ids against the
#                                sources, check the generated runner header
#   conf_catalogue.py gen        write conformance/runner/conf_catalogue.h
#   conf_catalogue.py summary    counts per area, level and judge
#
# The catalogue is the single list of test ids. The payloads print the ids that
# are judged by "payload" and have impl "yes"; the runner prints the ones judged
# by "runner" or "operator" and the SKIP lines for the rest. `check` fails when
# an id that is printed is not in the catalogue, an impl "yes" id is printed by
# nothing, or an impl "todo" id is already printed (flip the row).
#
# SPDX-License-Identifier: MIT
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
TESTS_MD = os.path.join(ROOT, "TESTS.md")
HEADER = os.path.join(ROOT, "runner", "conf_catalogue.h")
PAYLOAD_DIR = os.path.join(ROOT, "payloads")
RUNNER_SRC = os.path.join(ROOT, "runner", "aar-conf.c")

LEVELS = ("MUST", "SHOULD", "MAY")
GROUPS = {"core": 0x00, "display": 0x01, "input": 0x02, "audio": 0x04,
          "gl": 0x08, "services": 0x10, "zz9000": 0x100}
PROFILES = {"any": 3, "a32": 1, "a64": 2}
JUDGES = {"payload": 1, "runner": 2, "operator": 3, "none": 4}
IMPLS = ("yes", "todo")

ROW = re.compile(r"^\|\s*(C-[A-Z]+\.\d+)\s*\|(.*)\|\s*$")
ID_LITERAL = re.compile(r'"(C-[A-Z]+\.[0-9]+)"')


class Test:
    def __init__(self, id_, spec, level, group, profile, judge, impl, check):
        self.id = id_
        self.area, num = id_.rsplit(".", 1)
        self.num = int(num)
        self.spec = spec
        self.level = level
        self.group = group
        self.profile = profile
        self.judge = judge
        self.impl = impl
        self.check = check

    @property
    def payload(self):
        """Name of the payload (without .elf) for a payload-judged test."""
        return "conf_" + self.area[2:].lower()

    @property
    def tested(self):
        return self.judge != "none"


def load(path=TESTS_MD):
    tests = []
    errors = []
    version = None
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            m = re.match(r"^Suite version:\s*(\S+)\s*$", line)
            if m:
                version = m.group(1)
            m = ROW.match(line)
            if not m:
                continue
            cells = [c.strip() for c in m.group(2).split("|")]
            # The check text never holds a pipe, so the split is exact.
            if len(cells) != 7:
                errors.append("%s:%d: %s has %d cells after the id, want 7"
                              % (path, n, m.group(1), len(cells)))
                continue
            spec, level, group, profile, judge, impl, check = cells
            t = Test(m.group(1), spec, level, group, profile, judge, impl, check)
            where = "%s:%d: %s" % (path, n, t.id)
            if level not in LEVELS:
                errors.append("%s: level %r" % (where, level))
            if group not in GROUPS:
                errors.append("%s: group %r" % (where, group))
            if profile not in PROFILES:
                errors.append("%s: profile %r" % (where, profile))
            if judge not in JUDGES:
                errors.append("%s: judged by %r" % (where, judge))
            if impl not in IMPLS:
                errors.append("%s: impl %r" % (where, impl))
            if not spec or not check:
                errors.append("%s: empty spec or check" % where)
            if judge == "none" and not check.startswith("not tested"):
                errors.append("%s: judge none needs a check text that starts"
                              " with 'not tested'" % where)
            if judge != "none" and check.startswith("not tested"):
                errors.append("%s: 'not tested' with judge %s" % (where, judge))
            if level == "MAY" and judge != "none":
                errors.append("%s: a MAY row can only be judge none" % where)
            if judge == "none" and impl != "todo":
                errors.append("%s: judge none rows keep impl todo" % where)
            tests.append(t)
    if version is None:
        errors.append("%s: no 'Suite version:' line" % path)
    seen = {}
    last = {}
    for t in tests:
        if t.id in seen:
            errors.append("duplicate id %s" % t.id)
        seen[t.id] = t
        want = last.get(t.area, 0) + 1
        if t.num != want:
            errors.append("%s: numbers of %s must run 1, 2, 3 without gaps"
                          " (expected %d)" % (t.id, t.area, want))
        last[t.area] = t.num
    return version, tests, errors


def printed_ids(paths):
    """ids that appear as string literals in the given source files."""
    found = {}
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            for m in ID_LITERAL.finditer(fh.read()):
                found.setdefault(m.group(1), set()).add(os.path.basename(path))
    return found


def payload_sources():
    return sorted(os.path.join(PAYLOAD_DIR, f) for f in os.listdir(PAYLOAD_DIR)
                  if f.startswith("conf_") and f.endswith(".c"))


def runner_sources():
    d = os.path.join(ROOT, "runner")
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c"))


def c_string(text):
    text = text.replace("`", "")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(version, tests):
    areas = []
    for t in tests:
        if t.area not in areas:
            areas.append(t.area)
    out = []
    out.append("/*")
    out.append(" * Generated by conformance/tools/conf_catalogue.py from TESTS.md.")
    out.append(" * Do not edit; run `conf_catalogue.py gen`.")
    out.append(" *")
    out.append(" * SPDX-License-Identifier: MIT")
    out.append(" */")
    out.append("#ifndef CONF_CATALOGUE_H")
    out.append("#define CONF_CATALOGUE_H")
    out.append("")
    out.append("#define CONF_SUITE_VERSION %s" % c_string(version))
    out.append("")
    out.append("/* level */")
    out.append("#define CONF_MUST   0")
    out.append("#define CONF_SHOULD 1")
    out.append("#define CONF_MAY    2")
    out.append("/* group masks: AAR_GROUP_* bits, and the ZZ9000 extension */")
    out.append("#define CONF_GROUP_CORE    0x000u")
    out.append("#define CONF_GROUP_ZZ9000  0x100u")
    out.append("/* profile masks: bit 0 A32, bit 1 A64 */")
    out.append("/* judge */")
    out.append("#define CONF_J_PAYLOAD  1")
    out.append("#define CONF_J_RUNNER   2")
    out.append("#define CONF_J_OPERATOR 3")
    out.append("#define CONF_J_NONE     4")
    out.append("")
    out.append("struct conf_test {")
    out.append("    const char *id;")
    out.append("    unsigned short area;    /* index into conf_areas */")
    out.append("    unsigned char level;")
    out.append("    unsigned char judge;")
    out.append("    unsigned char profiles;")
    out.append("    unsigned char impl;     /* 1: the suite prints it today */")
    out.append("    unsigned short group;")
    out.append("    const char *prompt;     /* check text of operator tests, else \"\" */")
    out.append("};")
    out.append("")
    out.append("/* Area names; the payload of area C-X is conf_x.elf. */")
    out.append("extern const char *const conf_areas[];")
    out.append("#define CONF_AREA_COUNT %d" % len(areas))
    out.append("/* Every row of TESTS.md, in order. */")
    out.append("extern const struct conf_test conf_tests[];")
    out.append("#define CONF_TEST_COUNT %d" % len(tests))
    out.append("")
    out.append("/* The data is defined in one translation unit, the one that defines")
    out.append("   CONF_CATALOGUE_DEFINE before it includes this file. */")
    out.append("#ifdef CONF_CATALOGUE_DEFINE")
    out.append("const char *const conf_areas[] = {")
    for a in areas:
        out.append("    %s," % c_string(a))
    out.append("};")
    out.append("")
    out.append("const struct conf_test conf_tests[] = {")
    for t in tests:
        prompt = t.check if t.judge == "operator" else ""
        out.append("    { %s, %d, CONF_%s, CONF_J_%s, %d, %d, 0x%03xu, %s }," % (
            c_string(t.id), areas.index(t.area), t.level, t.judge.upper(),
            PROFILES[t.profile], 1 if t.impl == "yes" else 0, GROUPS[t.group],
            c_string(prompt)))
    out.append("};")
    out.append("#endif")
    out.append("")
    out.append("#endif")
    out.append("")
    return "\n".join(out)


def cmd_gen(version, tests):
    with open(HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(generate(version, tests))
    print("wrote %s (%d ids)" % (HEADER, len(tests)))
    return 0


def cmd_check(version, tests, errors):
    by_id = {t.id: t for t in tests}
    payload_found = printed_ids(payload_sources())
    runner_found = printed_ids(runner_sources())

    for pid, files in sorted(payload_found.items()):
        t = by_id.get(pid)
        if t is None:
            errors.append("%s is printed by %s but is not in TESTS.md"
                          % (pid, ", ".join(sorted(files))))
        elif t.judge != "payload":
            errors.append("%s is printed by a payload but TESTS.md says judged"
                          " by %s" % (pid, t.judge))
        elif t.impl != "yes":
            errors.append("%s is printed by a payload but its impl is todo"
                          % pid)
        else:
            want = t.payload + ".c"
            if want not in files:
                errors.append("%s should be printed by %s, found in %s"
                              % (pid, want, ", ".join(sorted(files))))
    for rid, files in sorted(runner_found.items()):
        t = by_id.get(rid)
        for f in sorted(files):
            if f == "aar-conf.c":
                errors.append("%s is in aar-conf.c: the core prints no test ids;"
                              " put it in r_%s.c" % (rid, rid[2:].split(".")[0].lower()))
            elif t is not None and f != "r_%s.c" % t.area[2:].lower():
                errors.append("%s belongs to area %s but is in %s (want r_%s.c)"
                              % (rid, t.area, f, t.area[2:].lower()))
        if t is None:
            errors.append("%s is in the runner sources but not in TESTS.md" % rid)
        elif t.judge not in ("runner", "operator"):
            errors.append("%s is in the runner sources but TESTS.md says judged"
                          " by %s" % (rid, t.judge))
        elif t.impl != "yes":
            errors.append("%s is in the runner sources but its impl is todo" % rid)
    for t in tests:
        if t.impl != "yes":
            continue
        if t.judge == "payload" and t.id not in payload_found:
            errors.append("%s is impl yes but no payload prints it" % t.id)
        if t.judge == "runner" and t.id not in runner_found:
            errors.append("%s is impl yes but no r_<area>.c prints it" % t.id)

    # A runner-judged id needs a function in a tests[] table (it runs when selected) or a
    # needs[] entry (it is judged while a variant of the area's payload runs, and the core
    # runs that variant when the id is selected). Without one, ONLY=<id> would leave it
    # without a result. The observer ids are judged on every payload run (r_life.c).
    observed = {"C-LIFE.1", "C-LIFE.2", "C-LIFE.3"}
    tabled = set()
    for path in runner_sources():
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        for m in re.finditer(
                r"static const struct conf_(?:r_test|needs) \w+\[\] = \{(.*?)\n\};", text,
                re.S):
            tabled |= set(re.findall(r'"(C-[A-Z]+\.[0-9]+)"', m.group(1)))
    for t in tests:
        if t.judge == "runner" and t.impl == "yes" and t.id not in tabled \
                and t.id not in observed:
            errors.append("%s is judged by the runner but is in no tests[] or needs[] table of"
                          " its r_%s.c: ONLY=%s would leave it without a result"
                          % (t.id, t.area[2:].lower(), t.id))

    # Markers: conf_<area>.host and conf_<area>.qemu need their payload.
    for suffix in (".host", ".qemu"):
        for f in sorted(os.listdir(PAYLOAD_DIR)):
            if f.startswith("conf_") and f.endswith(suffix):
                if not os.path.exists(os.path.join(PAYLOAD_DIR, f[:-len(suffix)] + ".c")):
                    errors.append("%s has no payload %s.c" % (f, f[:-len(suffix)]))

    # Every area with tests has its runner file and its line in r_areas.h.
    areas = sorted({t.area[2:].lower() for t in tests})
    listed = sorted(re.findall(r"CONF_R_AREA\((\w+)\)", open(
        os.path.join(ROOT, "runner", "r_areas.h"), encoding="utf-8").read()))
    if areas != listed:
        errors.append("runner/r_areas.h lists %s, the catalogue has areas %s"
                      % (listed, areas))
    for a in areas:
        if not os.path.exists(os.path.join(ROOT, "runner", "r_%s.c" % a)):
            errors.append("runner/r_%s.c is missing" % a)
    try:
        generate(version, tests)
    except Exception as exc:  # noqa: BLE001
        errors.append("the runner header cannot be generated: %s" % exc)

    if errors:
        for e in errors:
            print("catalogue: " + e, file=sys.stderr)
        return 1
    done = sum(1 for t in tests if t.impl == "yes")
    tested = sum(1 for t in tests if t.tested)
    print("catalogue: %d ids, %d tested, %d implemented, no orphan ids"
          % (len(tests), tested, done))
    return 0


def cmd_summary(tests):
    areas = []
    for t in tests:
        if t.area not in areas:
            areas.append(t.area)
    print("%-11s %5s %5s %6s %5s %7s %8s %5s %5s" % (
        "area", "ids", "MUST", "SHOULD", "MAY", "payload", "runner+op", "none",
        "yes"))
    tot = [0] * 8
    for a in areas:
        ts = [t for t in tests if t.area == a]
        row = [len(ts),
               sum(1 for t in ts if t.level == "MUST"),
               sum(1 for t in ts if t.level == "SHOULD"),
               sum(1 for t in ts if t.level == "MAY"),
               sum(1 for t in ts if t.judge == "payload"),
               sum(1 for t in ts if t.judge in ("runner", "operator")),
               sum(1 for t in ts if t.judge == "none"),
               sum(1 for t in ts if t.impl == "yes")]
        print("%-11s %5d %5d %6d %5d %7d %8d %5d %5d" % ((a,) + tuple(row)))
        tot = [x + y for x, y in zip(tot, row)]
    print("%-11s %5d %5d %6d %5d %7d %8d %5d %5d" % (("total",) + tuple(tot)))
    print()
    judges = {}
    for t in tests:
        judges[t.judge] = judges.get(t.judge, 0) + 1
    print("judged by: " + ", ".join("%s %d" % (k, judges[k]) for k in
                                    ("payload", "runner", "operator", "none")
                                    if k in judges))
    tested = [t for t in tests if t.tested]
    print("tested ids %d: MUST %d, SHOULD %d" % (
        len(tested), sum(1 for t in tested if t.level == "MUST"),
        sum(1 for t in tested if t.level == "SHOULD")))
    return 0


def main(argv):
    if len(argv) != 2 or argv[1] not in ("check", "gen", "summary"):
        print(__doc__ or "usage: conf_catalogue.py check|gen|summary",
              file=sys.stderr)
        return 2
    version, tests, errors = load()
    if argv[1] == "gen":
        if errors:
            for e in errors:
                print("catalogue: " + e, file=sys.stderr)
            return 1
        return cmd_gen(version, tests)
    if argv[1] == "summary":
        if errors:
            for e in errors:
                print("catalogue: " + e, file=sys.stderr)
            return 1
        return cmd_summary(tests)
    return cmd_check(version, tests, errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
