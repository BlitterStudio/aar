#!/usr/bin/env python3
# Behavioral checks for the SDL2 host runner. Not a source-text grep.
# SPDX-License-Identifier: MIT
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

import threading

def run(exe, args, *, input_bytes=None, env=None, cwd=None, timeout=30):
    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    if env:
        merged.update(env)
    return subprocess.run(
        [exe, *args],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
        cwd=cwd,
        timeout=timeout,
        check=False,
    )


def show(proc):
    sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
    sys.stderr.write(proc.stderr.decode("utf-8", "replace"))


def expect_ok(proc):
    if proc.returncode != 0:
        show(proc)
        raise SystemExit(f"exit {proc.returncode}")


def checkerboard():
    width, height = 320, 240
    out = bytearray(width * 2 * height)
    for y in range(height):
        for x in range(width):
            pixel = (
                (0xF800 if (x & 32) else 0)
                | (0x07E0 if (y & 32) else 0)
                | 0x001F
            )
            i = (y * width + x) * 2
            out[i] = (pixel >> 8) & 0xFF
            out[i + 1] = pixel & 0xFF
    return bytes(out)


def tone(frames):
    out = bytearray()
    for i in range(frames):
        phase = i % 50
        value = 12000 if phase < 25 else -12000
        out += struct.pack("<hh", value, value)
    return bytes(out)


def check_hello(exe):
    proc = run(exe, ["--headless"])
    expect_ok(proc)
    if b"hello aar" not in proc.stdout:
        show(proc)
        raise SystemExit("hello text missing")


def check_console(exe):
    proc = run(exe, ["--headless", "--", "one"], input_bytes=b"Hello, aar!\n")
    expect_ok(proc)
    text = proc.stdout.decode("utf-8", "replace")
    err = proc.stderr.decode("utf-8", "replace")
    if "console: 1 argument(s) [one]" not in text:
        show(proc)
        raise SystemExit("console banner")
    if "HELLO, AAR!\n" not in text:
        show(proc)
        raise SystemExit("console echo")
    if "EOF after 1 line" not in text:
        show(proc)
        raise SystemExit("console eof")
    if "console: stderr works" not in err:
        show(proc)
        raise SystemExit("console stderr")


def fnv1a32(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def check_freestanding(exe):
    # The template fills 3 MiB from an xorshift32 stream and hashes it. This
    # recomputes the hash independently of the C code.
    seed = 0x2545F491
    data = bytearray(3 << 20)
    for i in range(len(data)):
        seed ^= (seed << 13) & 0xFFFFFFFF
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xFFFFFFFF
        data[i] = seed & 0xFF
    want = "freestanding ok: hash=%08x " % fnv1a32(data)
    proc = run(exe, ["--headless"])
    expect_ok(proc)
    out = proc.stdout.decode("utf-8", "replace")
    if want not in out or "FAIL" in out:
        show(proc)
        raise SystemExit("freestanding line %r missing" % want)


def ring_mix(h):
    h &= 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def ring_ping(ident):
    return struct.pack("<8I", 5, ident, 0x11110000 + ident, 0x22, 0, 0, 0, 0)


def ring_ctrl(ident, mode):
    return struct.pack("<8I", 7, ident, mode, 0, 0, 0, 0, 0)


def ring_run(exe, script_messages, args, stop=False):
    """Runs ringflood with injected messages and returns what it sent."""
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            for message in script_messages:
                fh.write(message.hex() + "\n")
            if stop:
                fh.write("stop\n")
        argv = ["--headless", "--msg-script", script, "--msg-out", out]
        if args:
            argv += ["--"] + args
        proc = run(exe, argv)
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            lines = [line.strip() for line in fh if line.strip()]
    return [struct.unpack("<8I", bytes.fromhex(line)) for line in lines]


def check_ringflood(exe):
    # Known answers shared with sdk/tests/app_ringtest_logic_test.c in the
    # zz9000-firmware repository: pattern word i of message seq is
    # ring_mix(seq * 4 + i).
    vectors = {
        1: (0x249CB285, 0xCC0D53CD, 0x5CEB4D08, 0x18C9AEC4),
        2: (0x4939650B, 0xC27C2913, 0xE9250490, 0x93A0FE04),
        1000: (0x1F0A4224, 0x9F34547F, 0xB764BC10, 0x4F4FFC5F),
        0xFFFFFFFE: (0xF94C9009, 0x0CC81F63, 0xC24094C8, 0x937F414E),
    }
    for seq, words in vectors.items():
        got = tuple(ring_mix((seq * 4 + i) & 0xFFFFFFFF) for i in range(4))
        if got != words:
            raise SystemExit(f"pattern vector for seq {seq}: {got}")
    # Run A: the app starts quiet and answers pings, floods after CTRL, and
    # leaves the flood after `count` DATA messages.
    count = 500
    msgs = ring_run(exe, [ring_ping(7), ring_ping(8), ring_ping(9),
                          ring_ctrl(1, 1)], [str(count)])
    info = msgs[0]
    # Word 2 is the shared block token (aar_shared.haddr), word 3 its size.
    if info[0] != 1 or info[1] != 1 or info[3] != 4096 or info[2] == 0xFFFFFFFF:
        raise SystemExit(f"INFO message {info}")
    if info[2] == 0 or info[2] & 0xFFF:
        raise SystemExit(f"INFO token {info[2]:#x} is not a granule-aligned token")
    kinds = [m[0] for m in msgs]
    if kinds[:5] != [1, 6, 6, 6, 8]:
        raise SystemExit(f"quiet start sent {kinds[:5]}, not INFO, 3 PONG, ACK")
    pongs = [m for m in msgs if m[0] == 6]
    if [m[1] for m in pongs] != [7, 8, 9]:
        raise SystemExit(f"pong ids {[m[1] for m in pongs]}")
    for m in pongs:
        if m[2] != 0x11110000 + m[1] or m[3] != 0x22 or m[5] != 0:
            raise SystemExit(f"pong echo {m}")
    ack = msgs[4]
    if ack[1:4] != (1, 1, 0):
        raise SystemExit(f"flood ACK {ack}")
    data = [m for m in msgs if m[0] == 2]
    if [m[1] for m in data] != list(range(1, count + 1)):
        raise SystemExit("data sequence is not 1..%d" % count)
    last_counter = 0
    for m in data:
        want = tuple(ring_mix(m[1] * 4 + i) for i in range(4))
        if m[4:8] != want:
            raise SystemExit(f"pattern of message {m[1]}: {m[4:8]}")
        if m[3] < last_counter or m[3] < m[1]:
            raise SystemExit(f"counter {m[3]} at message {m[1]}")
        last_counter = m[3]
    done = msgs[-1]
    if done[0] != 3 or done[1] != count or done[3] < last_counter or done[5] != 3:
        raise SystemExit(f"DONE message {done}")
    if len(msgs) != 1 + 3 + 1 + count + 1:
        raise SystemExit(f"{len(msgs)} messages recorded")

    # Run B: flood, ten pings, back to quiet, one more ping, then stop. The
    # app sends no DATA after the ACK for quiet and still answers.
    script = [ring_ctrl(1, 1)] + [ring_ping(20 + i) for i in range(10)]
    script += [ring_ctrl(2, 0)] + [ring_ping(97 + i) for i in range(3)]
    msgs = ring_run(exe, script, [], stop=True)
    kinds = [m[0] for m in msgs]
    if kinds[:2] != [1, 8] or kinds[-1] != 3 or 2 in kinds:
        raise SystemExit(f"quiet switch sent {kinds}")
    acks = [m for m in msgs if m[0] == 8]
    if [a[1:4] for a in acks] != [(1, 1, 0), (2, 0, 0)]:
        raise SystemExit(f"ACKs {acks}")
    after = msgs[msgs.index(acks[1]) + 1:-1]
    if not after or any(m[0] != 6 for m in after):
        raise SystemExit(f"after the quiet ACK the app sent {after}")
    done = msgs[-1]
    if done[1] != 0 or done[5] != len([m for m in msgs if m[0] == 6]):
        raise SystemExit(f"DONE {done}")


def check_filter(exe):
    with tempfile.TemporaryDirectory() as root:
        data = b"aar-host filter\n"
        with open(os.path.join(root, "in.bin"), "wb") as fh:
            fh.write(data)
        proc = run(exe, ["--root", root, "--", "in.bin", "out.bin"])
        expect_ok(proc)
        with open(os.path.join(root, "out.bin"), "rb") as fh:
            got = fh.read()
        want = hashlib.sha256(data).digest()
        if got != want:
            show(proc)
            raise SystemExit(f"filter digest {got.hex()} != {want.hex()}")


def check_echo(exe):
    messages = [
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    ]
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("\n".join(messages + ["stop"]) + "\n")
        proc = run(
            exe,
            ["--headless", "--msg-script", script, "--msg-out", out],
        )
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            got = [line.strip() for line in fh if line.strip()]
        if got != messages:
            show(proc)
            raise SystemExit(f"echo messages {got}")


def check_msgflood(exe):
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "msg.txt")
        out = os.path.join(root, "msg.out")
        with open(script, "w", encoding="ascii") as fh:
            fh.write('"ping"\nstop\n')
        proc = run(
            exe,
            ["--headless", "--msg-script", script, "--msg-out", out],
        )
        expect_ok(proc)
        with open(out, "r", encoding="ascii") as fh:
            got = [line for line in fh if line.strip()]
        if len(got) != 100:
            show(proc)
            raise SystemExit(f"msgflood recorded {len(got)} of 100 messages")


def check_conshort(exe):
    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        proc.stdin.write(b"hello\n")
        proc.stdin.flush()  # the pipe stays open: a full-length read must not block
        out = []
        reader = threading.Thread(
            target=lambda: out.append(proc.stdout.readline()), daemon=True
        )
        reader.start()
        reader.join(10)
        if not out or b"got 6: hello" not in out[0]:
            raise SystemExit(f"console read blocked or wrong: {out!r}")
        rc = proc.wait(timeout=10)
        if rc != 0:
            raise SystemExit(f"exit {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_mailbox(exe):
    proc = run(exe, ["--headless"])
    expect_ok(proc)


def check_nogl(exe):
    # --no-gl takes the group away: aar_gl_bind returns NULL, and the template
    # ends with 2 before it draws anything.
    proc = run(exe, ["--headless", "--no-gl"])
    if proc.returncode != 2:
        show(proc)
        raise SystemExit(f"expected exit 2 without the GL group, got {proc.returncode}")


def gl_missing(proc, what):
    # The test apps exit 77 and the template exits 2 when the host offers no
    # GL context. CTest shows 77 as skipped, unless CI wants GL to work.
    show(proc)
    if os.environ.get("AAR_TEST_REQUIRE_GL"):
        raise SystemExit(f"{what}: no GL group, and AAR_TEST_REQUIRE_GL is set")
    sys.stderr.write(f"SKIP {what}: the host runner has no GL group here\n")
    raise SystemExit(77)


def check_glabsent(exe):
    # Without a GL context the group is absent, and so is it with --no-gl on a
    # host that has one. Both forms must behave as a runner without GL.
    expect_ok(run(exe, ["--headless", "--no-gl", "--", "absent"]))
    expect_ok(run(exe, ["--no-gl", "--", "absent"]))


def rgb565_at(data, width, x, y_from_bottom, height):
    row = height - 1 - y_from_bottom
    i = (row * width + x) * 2
    return data[i], data[i + 1]


def check_gl_dump(path, width, height):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) != width * height * 2:
        raise SystemExit(f"gl dump is {len(data)} bytes, expected {width * height * 2}")
    # The scene of gltest: red clear colour, a green triangle at the lower left.
    if rgb565_at(data, width, 8, 8, height) != (0x07, 0xE0):
        raise SystemExit("gl dump: the triangle is not green at the lower left")
    if rgb565_at(data, width, 60, 44, height) != (0xF8, 0x00):
        raise SystemExit("gl dump: the clear colour is not red at the upper right")


def check_gl(exe):
    # Headless: the offscreen driver gives a context without a display.
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "gl.bin")
        proc = run(exe, ["--headless", "--dump-front", path, "--", "front"])
        if proc.returncode == 77:
            gl_missing(proc, "gl")
        expect_ok(proc)
        err = proc.stderr.decode("utf-8", "replace")
        # The count starts again when the app reopens the display, and gltest
        # swaps once after that.
        if "flips=1" not in err:
            show(proc)
            raise SystemExit("gl: expected flips=1")
        check_gl_dump(path, 64, 48)
    # Without --dump-front a swap does not read the frame back.
    proc = run(exe, ["--headless", "--"])
    if proc.returncode == 77:
        gl_missing(proc, "gl")
    expect_ok(proc)


def check_glpump(exe):
    # A loop of aar_gl_swap, aar_event_poll and aar_should_stop, with no yield
    # or sleep, must still see input and a close request.
    proc = run(exe, ["--headless", "--", "pump"])
    if proc.returncode == 77:
        gl_missing(proc, "glpump")
    expect_ok(proc)


def check_glresize(exe):
    # aar_display_resize with a context bound, unbound and bound again. The
    # last frame is 100x70 and left bound, so --dump-front holds it.
    width, height = 100, 70
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "gl.bin")
        proc = run(exe, ["--headless", "--dump-front", path, "--", "resize"])
        if proc.returncode == 77:
            gl_missing(proc, "glresize")
        expect_ok(proc)
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) != width * height * 2:
            raise SystemExit(f"glresize dump is {len(data)} bytes, expected {width * height * 2}")
        if rgb565_at(data, width, 8, 8, height) != (0x07, 0xE0):
            raise SystemExit("glresize dump: the triangle is not green at the lower left")
        if rgb565_at(data, width, 96, 66, height) != (0xF8, 0x00):
            raise SystemExit("glresize dump: the clear colour is not red at the upper right")


def check_glwindow(exe):
    # A window, scaled, the way a developer runs it: a real display, or Xvfb.
    merged = os.environ.copy()
    merged["SDL_AUDIODRIVER"] = "dummy"
    merged.pop("SDL_VIDEODRIVER", None)
    prefix = []
    if sys.platform.startswith("linux") and not (
        merged.get("DISPLAY") or merged.get("WAYLAND_DISPLAY")
    ):
        import shutil

        xvfb = shutil.which("xvfb-run")
        if not xvfb:
            gl_missing(subprocess.CompletedProcess([], 77, b"", b""), "glwindow (no display, no xvfb-run)")
        prefix = [xvfb, "-a"]
        merged["SDL_VIDEODRIVER"] = "x11"
        merged["LIBGL_ALWAYS_SOFTWARE"] = "1"
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "gl.bin")
        proc = subprocess.run(
            [*prefix, exe, "--scale", "2", "--dump-front", path, "--", "front"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=merged,
            timeout=60,
            check=False,
        )
        if proc.returncode == 77:
            gl_missing(proc, "glwindow")
        expect_ok(proc)
        # xvfb-run folds the app's stderr into its stdout.
        err = (proc.stdout + proc.stderr).decode("utf-8", "replace")
        if "flips=1" not in err:
            show(proc)
            raise SystemExit("glwindow: expected flips=1")
        check_gl_dump(path, 64, 48)


def check_glconf(exe):
    # The MiniGL texture environment matrix of templates/mgl_conformance.c on
    # the host's GL. The template exits 2 when it cannot bind, and 1 when a
    # case differs. The cases whose result the GL 1.1 specification fixes (the
    # groups below) must pass on any conforming GL. The other groups hold
    # MiniGL's own choices, which the host GL does not have to share (see
    # host-runner/README.md).
    import re

    proc = run(exe, ["--headless"], timeout=170)
    if proc.returncode == 2:
        gl_missing(proc, "glconf")
    out = proc.stdout.decode("utf-8", "replace")
    summary = re.search(r"MGL summary cases=(\d+) failures=(\d+)", out)
    if proc.returncode not in (0, 1) or not summary:
        show(proc)
        raise SystemExit(f"mgl-conformance on the host GL: exit {proc.returncode}")
    cases, failures = int(summary.group(1)), int(summary.group(2))
    start = re.search(r"MGL start cases=(\d+)", out)
    if not start or int(start.group(1)) != cases or cases == 0 or failures >= cases:
        show(proc)
        raise SystemExit("mgl-conformance did not run the matrix")
    groups = {}
    for m in re.finditer(r"^MGL env=(\w+) fmt=(\w+) draw=(\w+) pass=(\d+) fail=(\d+)", out, re.M):
        groups[(m.group(1), m.group(2), m.group(3))] = (int(m.group(4)), int(m.group(5)))
    for env, fmt in [("MODULATE", f) for f in ("RGBA", "RGB", "LUMINANCE", "ALPHA", "LUMINANCE_ALPHA")] + [
        ("REPLACE", "RGBA"),
        ("DECAL", "RGBA"),
    ]:
        for draw in ("immediate", "arrays", "elements"):
            got = groups.get((env, fmt, draw))
            if not got or got[0] == 0 or got[1] != 0:
                show(proc)
                raise SystemExit(f"mgl-conformance: {env}/{fmt}/{draw} did not pass on the host GL: {got}")


def check_constop(exe):
    if os.name != "posix":
        return
    import signal
    import time

    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        time.sleep(0.5)  # let the app block in aar_console_read on the empty pipe
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("console read not interrupted by SIGTERM")
        if rc != 1:
            raise SystemExit(f"expected the stop result 1, got {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()

def check_pixelsstop(exe):
    if os.name != "posix":
        return
    import signal
    import time

    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        time.sleep(0.5)  # let pixels block in aar_display_flip
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("display flip not interrupted by SIGTERM")
        if rc != 0:
            raise SystemExit(f"expected clean exit 0, got {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_msgwaitidle(exe):
    if os.name != "posix":
        return
    import resource
    import time

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    proc = run(exe, ["--headless", "--", "msgwaitidle"])
    wall = time.monotonic() - start
    expect_ok(proc)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    if wall < 1.4:
        raise SystemExit(f"idle wait ended after {wall:.2f} s")
    if cpu > 0.3 * wall:
        raise SystemExit(f"idle wait burned {cpu:.2f} s CPU in {wall:.2f} s")


def check_msgwaitstop(exe):
    if os.name != "posix":
        return
    import signal
    import time

    merged = os.environ.copy()
    merged["SDL_VIDEODRIVER"] = "dummy"
    merged["SDL_AUDIODRIVER"] = "dummy"
    proc = subprocess.Popen(
        [exe, "--headless", "--", "msgwaitstop"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=merged,
    )
    try:
        time.sleep(0.5)  # let the app park in aar_msg_wait with 30 s to go
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            raise SystemExit("aar_msg_wait not interrupted by SIGTERM")
        if rc != 0:
            raise SystemExit(f"expected AAR_E_IO and exit 0, got {rc}")
    finally:
        proc.stdin.close()
        proc.kill()
        proc.wait()


def check_pixels(exe):
    want = checkerboard()
    with tempfile.TemporaryDirectory() as root:
        dumps = []
        for name in ("a.bin", "b.bin"):
            path = os.path.join(root, name)
            proc = run(
                exe,
                ["--headless", "--frames", "2", "--dump-front", path],
            )
            expect_ok(proc)
            err = proc.stderr.decode("utf-8", "replace")
            if "flips=2" not in err:
                show(proc)
                raise SystemExit("pixels flip count")
            with open(path, "rb") as fh:
                dumps.append(fh.read())
        if dumps[0] != dumps[1]:
            raise SystemExit("pixels dumps differ")
        if dumps[0] != want:
            raise SystemExit(
                f"pixels bytes {len(dumps[0])} != expected {len(want)}"
            )


def check_tone(exe):
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "tone.pcm")
        proc = run(
            exe,
            [
                "--headless",
                "--audio-null",
                "--audio-frames",
                "256",
                "--audio-dump",
                path,
            ],
        )
        expect_ok(proc)
        err = proc.stderr.decode("utf-8", "replace")
        if "audio_frames=256" not in err:
            show(proc)
            raise SystemExit("tone frame count")
        with open(path, "rb") as fh:
            got = fh.read()
        if got != tone(256):
            raise SystemExit("tone samples")


def check_keys(exe):
    proc = run(exe, ["--headless"], env={"AAR_HOST_SELFTEST": "1"})
    expect_ok(proc)


def prepare_root(root):
    os.makedirs(os.path.join(root, "foo"))
    with open(os.path.join(root, "ok.txt"), "w", encoding="ascii") as fh:
        fh.write("ok\n")
    with open(os.path.join(root, "bar.txt"), "w", encoding="ascii") as fh:
        fh.write("bar\n")
    with open(os.path.join(root, "empty.txt"), "wb") as fh:
        fh.write(b"")
    with open(os.path.join(root, "big.bin"), "wb") as fh:
        fh.seek(16 * 1024 * 1024)
        fh.write(b"x")


def check_contract(exe):
    with tempfile.TemporaryDirectory() as root:
        prepare_root(root)
        proc = run(exe, ["--headless", "--root", root, "--", "reject"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--", "table"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--no-gl", "--", "table", "nogl"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--", "display"])
        expect_ok(proc)
        proc = run(exe, ["--", "services"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--", "memory"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--audio-null", "--", "audio"])
        expect_ok(proc)
        proc = run(exe, ["--headless", "--", "msgwait"])
        expect_ok(proc)
        script = os.path.join(root, "input.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("key down 0x22 8\nbreak 4\nlowmem\n")
        proc = run(exe, ["--headless", "--input", script, "--", "input"])
        expect_ok(proc)
    with tempfile.TemporaryDirectory() as cwd:
        with open(os.path.join(cwd, "vol:data"), "wb") as fh:
            fh.write(b"abs-ok")
        proc = run(exe, ["--abs", "--", "abs"], cwd=cwd)
        expect_ok(proc)


# ---- API 0.6 (tests/api06.c) ----------------------------------------------

def api06_run(exe, mode, *, options=(), env=None, cwd=None, expect=0):
    proc = run(exe, ["--headless", *options, "--", *mode], env=env, cwd=cwd)
    if proc.returncode == 77:
        show(proc)
        sys.stderr.write("SKIP: " + " ".join(mode) + "\n")
        raise SystemExit(77)
    if proc.returncode != expect:
        show(proc)
        raise SystemExit(f"api06 {' '.join(mode)}: exit {proc.returncode}")
    return proc.stdout.decode("utf-8", "replace")


def check_api06_failure(exe):
    for mode, check_id in (("fail-check", 256), ("fail-rc", 333)):
        proc = run(exe, ["--headless", "--", mode])
        if proc.returncode != 1 or f"check {check_id}:".encode("ascii") not in proc.stderr:
            show(proc)
            raise SystemExit(f"api06 {mode}: expected check {check_id} and exit 1, got {proc.returncode}")


def check_api06_files(exe):
    import re
    import stat

    with tempfile.TemporaryDirectory() as root:
        # A file next to the root: every call that names it through "../" is
        # refused, and it is as it was afterwards.
        outside = os.path.join(os.path.dirname(root), "aar06-outside.txt")
        with open(outside, "w", encoding="ascii") as fh:
            fh.write("keep")
        os.utime(outside, (1234567890, 1234567890))
        outside_mode = stat.S_IMODE(os.stat(outside).st_mode)
        with open(os.path.join(root, "ok.txt"), "w", encoding="ascii") as fh:
            fh.write("ok\n")
        try:
            out = api06_run(exe, ["files"], options=["--root", root])
            st = os.stat(outside)
            if open(outside, encoding="ascii").read() != "keep" or \
                    int(st.st_mtime) != 1234567890 or stat.S_IMODE(st.st_mode) != outside_mode:
                raise SystemExit("a call changed the file outside the root")
            if os.path.exists(os.path.join(os.path.dirname(root), "aar06-outside-dir")):
                raise SystemExit("dir_create made a directory outside the root")
        finally:
            os.remove(outside)
        # What the calls did on the host.
        with open(os.path.join(root, "ok.txt"), "rb") as fh:
            if fh.read() != b"he\0\0\0\0":
                raise SystemExit("rename with replace did not put the file over ok.txt")
        if os.path.exists(os.path.join(root, "sub")):
            raise SystemExit("the directory was not deleted")
        if os.path.exists(os.path.join(root, "ok2.txt")):
            raise SystemExit("a refused rename created its target")
        with open(os.path.join(root, "eof.txt"), "rb") as fh:
            if fh.read() != b"abc\0\0\0":
                raise SystemExit("truncate after a read to the end did not extend eof.txt with zeros")
        if int(os.stat(os.path.join(root, "dated.txt")).st_mtime) != 1000000000:
            raise SystemExit("set_date did not set the modification time")
        perm = os.path.join(root, "perm.txt")
        if os.name == "posix":
            mode = stat.S_IMODE(os.stat(perm).st_mode)
            # WRITE, DELETE and EXECUTE were set: the owner's w and x bits
            # are clear and r stays.
            if mode & stat.S_IWUSR or not mode & stat.S_IRUSR or mode & stat.S_IXUSR:
                raise SystemExit(f"set_protect left mode {mode:o} on perm.txt")
            os.chmod(perm, 0o644)
        m = re.search(r"volume block=(\d+) total=(\d+) free=(\d+)", out)
        if not m:
            raise SystemExit("no volume line")
        block, total, free = (int(x) for x in m.groups())
        if os.name == "posix":
            sv = os.statvfs(root)
            unit = sv.f_frsize or sv.f_bsize
            if block != unit or total != sv.f_blocks * unit or free > total:
                raise SystemExit(f"volume_info {block}/{total}/{free} vs statvfs {unit}/{sv.f_blocks * unit}")


def check_api06_time(exe):
    import time

    before = time.time_ns() // 1000
    out = api06_run(exe, ["time"])
    after = time.time_ns() // 1000
    line = [l for l in out.splitlines() if l.startswith("unix_us=")]
    if not line:
        raise SystemExit("no unix_us line")
    value = int(line[0].split("=")[1])
    if not before <= value <= after:
        raise SystemExit(f"time_unix_us {value} is outside [{before}, {after}]")


def locale_fields(exe, env):
    import re

    base = {"LC_ALL": "", "LC_MESSAGES": "", "LANG": "", "LC_NUMERIC": "", "TZ": "UTC"}
    base.update(env)
    out = api06_run(exe, ["locale"], env=base)
    m = re.search(r"utc=(-?\d+) flags=(\d+) lang=(\S*) country=(\S*) dec=(\S*) grp=(.*)", out)
    if not m:
        raise SystemExit(f"locale line: {out!r}")
    return m.groups()


def check_api06_locale(exe):
    # The offset is minutes east; a POSIX TZ string "XYZ-5:30" is 5:30 east.
    utc, flags, lang, country, dec, grp = locale_fields(
        exe, {"LC_ALL": "de_DE.UTF-8", "TZ": "XYZ-5:30"})
    if (int(utc), int(flags), lang, country) != (330, 1, "deutsch", "D"):
        raise SystemExit(f"de_DE: {(utc, flags, lang, country)}")
    if not dec:
        raise SystemExit("no decimal point")
    got = locale_fields(exe, {"LANG": "fr_CA.UTF-8", "TZ": "XYZ+3"})
    if (int(got[0]), got[2], got[3]) != (-180, "francais", "CDN"):
        raise SystemExit(f"fr_CA by LANG: {got}")
    got = locale_fields(exe, {"LC_ALL": "C"})
    if (int(got[0]), got[2], got[3]) != (0, "", ""):
        raise SystemExit(f"C: {got}")
    got = locale_fields(exe, {"LC_ALL": "en_GB", "LANG": "de_DE"})
    if (got[2], got[3]) != ("english", "GB"):
        raise SystemExit(f"en_GB over LANG: {got}")
    got = locale_fields(exe, {"LC_MESSAGES": "sv_SE.UTF-8", "LANG": "de_DE"})
    if (got[2], got[3]) != ("svenska", "S"):
        raise SystemExit(f"sv_SE by LC_MESSAGES: {got}")
    got = locale_fields(exe, {"LC_ALL": "xx_YY"})
    if (got[2], got[3]) != ("", "YY"):
        raise SystemExit(f"unknown language: {got}")
    got = locale_fields(exe, {"LC_ALL": "pt_BR.UTF-8"})
    if (got[2], got[3]) != ("portugues", "BR"):
        raise SystemExit(f"pt_BR: {got}")
    for name, want in [("de_AT.UTF-8", "A"), ("de_CH", "CH"), ("it_IT", "I"),
                       ("es_ES", "E"), ("fr_FR", "F"), ("en_US.UTF-8", "USA"),
                       ("en_GB.UTF-8", "GB"), ("de_DE@euro", "D"), ("de", ""),
                       ("de_DEU", "")]:
        got = locale_fields(exe, {"LC_ALL": name})
        if got[3] != want:
            raise SystemExit(f"country of {name}: {got[3]!r}, want {want!r}")


def check_api06_env(exe):
    with tempfile.TemporaryDirectory() as env_dir:
        with open(os.path.join(env_dir, "greeting"), "wb") as fh:
            fh.write(b"hello world")
        open(os.path.join(env_dir, "empty"), "wb").close()
        os.makedirs(os.path.join(env_dir, "sub"))
        with open(os.path.join(env_dir, "sub", "inner"), "wb") as fh:
            fh.write(b"in")
        with open(os.path.join(env_dir, "bin"), "wb") as fh:
            fh.write(b"\0\1\2")
        api06_run(exe, ["envdir"], options=["--env", env_dir],
                  env={"AARTEST_ENV": "from-process"})
    env = {"AARTEST_ENV": "from-process"}
    env.pop("AARTEST_NOT_SET", None)
    api06_run(exe, ["envproc"], env=env)
    # --env names a directory.
    proc = run(exe, ["--headless", "--env", os.path.join(tempfile.gettempdir(), "aar-no-such-dir"), "--", "time"])
    if proc.returncode != 10 or b"--env" not in proc.stderr:
        show(proc)
        raise SystemExit(f"--env with a missing directory: exit {proc.returncode}")
    proc = run(exe, ["--headless", "--env"])
    if proc.returncode != 10:
        raise SystemExit("--env without a value")


def check_api06_clip(exe):
    api06_run(exe, ["clip"])
    api06_run(exe, ["clip"], options=["--no-gl"])


def check_api06_display(exe):
    import re

    out = api06_run(exe, ["display"])
    if not re.search(r"refresh_mhz=\d+ screen=\d+x\d+", out):
        raise SystemExit("no display info line")
    api06_run(exe, ["display"], options=["--no-gl"])
    api06_run(exe, ["display"], options=["--scale", "2"])


def check_api06_present(exe):
    out = api06_run(exe, ["present"])
    if "vblank=" not in out:
        raise SystemExit("no vblank line")
    api06_run(exe, ["present"], options=["--no-gl"])


def check_api06_resize(exe):
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "input.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("resize 80 60\n")
        api06_run(exe, ["resize"], options=["--input", script])
        api06_run(exe, ["resize"], options=["--input", script, "--scale", "2"])


def check_api06_events(exe):
    api06_run(exe, ["events"])
    api06_run(exe, ["events_nocap"])
    api06_run(exe, ["events_nocap"], options=["--no-gl"])
    api06_run(exe, ["events_scaled"], options=["--scale", "2"])


SCRIPT_EVENTS = """\
text 65
wheel 1 -2 8
active 1
close
resize 80 60
mouse 1 2 0x8000
text 0x20ac 8
"""

WHEEL = "ev 10 0 1 -2 8"
ACTIVE = "ev 12 1 0 0 0"
MOUSE = "ev 3 0 1 2 32768"
TEXT_A = "ev 9 65 0 0 0"
TEXT_EURO = "ev 9 8364 0 0 8"
CLOSE = "ev 11 0 0 0 0"
RESIZE = "ev 6 0 80 60 0"


def check_api06_input(exe):
    # An event that needs a cap reaches the app only with that cap; the wheel
    # and focus events always do.
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "input.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write(SCRIPT_EVENTS)
        cases = [
            (0, [WHEEL, ACTIVE, MOUSE]),
            (12, [WHEEL, ACTIVE, RESIZE, MOUSE]),  # WINDOW | RESIZABLE
            (32, [TEXT_A, WHEEL, ACTIVE, MOUSE, TEXT_EURO]),  # TEXT_INPUT
            (64, [WHEEL, ACTIVE, CLOSE, MOUSE]),  # CLOSE_REQUEST
            (8, [WHEEL, ACTIVE, MOUSE]),  # RESIZABLE needs WINDOW
            (252, [TEXT_A, WHEEL, ACTIVE, CLOSE, RESIZE, MOUSE, TEXT_EURO]),
        ]
        for flags, want in cases:
            out = api06_run(exe, ["script", str(flags)], options=["--input", script])
            got = [l for l in out.splitlines() if l.startswith("ev ")]
            if got != want:
                raise SystemExit(f"script events with flags {flags}: {got} != {want}")
        import re

        # Motion that the queue cannot hold is merged, and no delta is lost.
        script = os.path.join(root, "merge.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("mouse 1 2\n" * 300)
        out = api06_run(exe, ["merge"], options=["--input", script])
        m = re.search(r"rel events=(\d+) before=(-?\d+),(-?\d+) buttons=(\d+) after=(-?\d+),(-?\d+)", out)
        if not m:
            raise SystemExit(f"merge output {out!r}")
        events, bx, by, buttons, ax, ay = (int(x) for x in m.groups())
        if (bx, by, buttons, ax, ay) != (300, 600, 0, 0, 0):
            raise SystemExit(f"merged motion lost deltas: {m.groups()}")
        if events != 257:
            raise SystemExit(f"{events} events for 300 scripted: the queue holds 256 and one merged event")
        # Below the capacity nothing is merged, and a button keeps its place.
        script = os.path.join(root, "order.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("mouse 1 2\n" * 5)
            fh.write("mousebtn 1 down\n")
            fh.write("mouse 3 -1 1\n" * 10)
        out = api06_run(exe, ["merge"], options=["--input", script])
        m = re.search(r"rel events=(\d+) before=(-?\d+),(-?\d+) buttons=(\d+) after=(-?\d+),(-?\d+)", out)
        if not m or tuple(int(x) for x in m.groups()) != (16, 5, 10, 1, 30, -10):
            raise SystemExit(f"motion around a button: {out!r}")
        # The merged sum saturates at the int16 range.
        script = os.path.join(root, "sat.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("mouse 1000 -1000\n" * 300)
        out = api06_run(exe, ["merge"], options=["--input", script])
        m = re.search(r"rel events=(\d+) before=(-?\d+),(-?\d+)", out)
        if not m or (int(m.group(2)), int(m.group(3))) != (256 * 1000 + 32767, -(256 * 1000 + 32768)):
            raise SystemExit(f"saturating motion: {out!r}")
        # An absolute event keeps the position of the latest one.
        script = os.path.join(root, "abs.txt")
        with open(script, "w", encoding="ascii") as fh:
            for i in range(1, 301):
                fh.write(f"mouse {i} {2 * i} 0x8000\n")
        out = api06_run(exe, ["merge", "abs"], options=["--input", script])
        m = re.search(r"abs events=(\d+) last=(\d+),(\d+)", out)
        if not m or (int(m.group(2)), int(m.group(3))) != (300, 600) or int(m.group(1)) >= 300:
            raise SystemExit(f"absolute merge: {out!r}")


def check_api06_pads(exe):
    api06_run(exe, ["pads"])


def check_api06_window(exe):
    # The software window the way a developer runs it: the X11 driver on a real
    # display, or on Xvfb. Headless runs have no renderer, so the texture and
    # logical size paths of the display run only here.
    merged = os.environ.copy()
    merged["SDL_AUDIODRIVER"] = "dummy"
    merged.pop("SDL_VIDEODRIVER", None)
    prefix = []
    if sys.platform.startswith("linux") and not (
        merged.get("DISPLAY") or merged.get("WAYLAND_DISPLAY")
    ):
        import shutil

        xvfb = shutil.which("xvfb-run")
        if not xvfb:
            gl_missing(subprocess.CompletedProcess([], 77, b"", b""), "api06_window (no display, no xvfb-run)")
        prefix = [xvfb, "-a"]
        merged["SDL_VIDEODRIVER"] = "x11"
        merged["LIBGL_ALWAYS_SOFTWARE"] = "1"
    with tempfile.TemporaryDirectory() as root:
        script = os.path.join(root, "input.txt")
        with open(script, "w", encoding="ascii") as fh:
            fh.write("resize 80 60\n")
        runs = [
            (["display"], []),
            (["present"], []),
            (["resize"], ["--input", script]),
            (["resize"], ["--input", script, "--scale", "2"]),
            (["events"], []),
            (["events_nocap"], []),
            (["events_scaled"], ["--scale", "2"]),
            (["clip"], []),
        ]
        for mode, options in runs:
            proc = subprocess.run(
                [*prefix, exe, *options, "--", *mode],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=merged,
                timeout=60,
                check=False,
            )
            if proc.returncode != 0:
                show(proc)
                raise SystemExit(f"api06 {' '.join(mode)} in a window: exit {proc.returncode}")


CHECKS = {
    "ringflood": check_ringflood,
    "freestanding": check_freestanding,
    "hello": check_hello,
    "console": check_console,
    "filter": check_filter,
    "echo": check_echo,
    "pixels": check_pixels,
    "tone": check_tone,
    "keys": check_keys,
    "contract": check_contract,
    "msgflood": check_msgflood,
    "conshort": check_conshort,
    "mailbox": check_mailbox,
    "nogl": check_nogl,
    "glabsent": check_glabsent,
    "gl": check_gl,
    "glpump": check_glpump,
    "glwindow": check_glwindow,
    "glresize": check_glresize,
    "glconf": check_glconf,
    "constop": check_constop,
    "pixelsstop": check_pixelsstop,
    "msgwaitstop": check_msgwaitstop,
    "msgwaitidle": check_msgwaitidle,
    "api06_failure": check_api06_failure,
    "api06_files": check_api06_files,
    "api06_time": check_api06_time,
    "api06_locale": check_api06_locale,
    "api06_env": check_api06_env,
    "api06_clip": check_api06_clip,
    "api06_display": check_api06_display,
    "api06_present": check_api06_present,
    "api06_resize": check_api06_resize,
    "api06_events": check_api06_events,
    "api06_input": check_api06_input,
    "api06_pads": check_api06_pads,
    "api06_window": check_api06_window,
}


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in CHECKS:
        raise SystemExit("usage: check_runner.py <test> <exe>")
    CHECKS[sys.argv[1]](sys.argv[2])


if __name__ == "__main__":
    main()
