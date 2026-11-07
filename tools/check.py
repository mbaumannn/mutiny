#!/usr/bin/env python3
"""Build and test MUTINY.

    python3 tools/check.py test          host C tests, Python tests, format check
    python3 tools/check.py mod           the above, then build, patch and lint the mod

`mod` needs these environment variables (absolute paths):

    MUTINY_ELEKLOADER   an Elekloader checkout
    MUTINY_STOCK        your own Digitone OS 1.44 .syx
    MUTINY_CORE_MOD     core-dn1-4.1-os1.44.elemod
    MUTINY_SEQ_MOD      seq-events-0.1-os1.44.elemod

CC and CLANG_FORMAT choose the host compiler and formatter. A check that cannot
run is reported as BLOCKED and fails the run.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The inputs this release was built and tested with.
ELEKLOADER_COMMIT = "b893c9317936633d7a32829b618c2867ee99277c"
STOCK_SHA256 = "d4f200d04484333d82822db7744e6484d0def8f2db8ddf55ee2b780cc13c9659"
CORE_SHA256 = "616714428ca824824efd2bc67eb7b80155ae5547d83abdd60914b063a28b83a9"
SEQ_SHA256 = "0727ece76a560c30ee27e4d19d18536e81340f60d7deaf294fc213d05124cd21"
CLANG_FORMAT_VERSION = "23.1.4"

HOST_TESTS = [
    ("engine", "tests/test_engine.c", ["src/engine/engine.c"], ["-DMU_ENGINE_PROBE"]),
    ("acid-cache", "tests/test_acid_cache.c", ["src/engine/engine.c"], ["-DMU_ENGINE_PROBE"]),
    ("runtime", "tests/test_runtime.c", ["src/runtime/runtime.c", "src/engine/engine.c"], []),
    ("input", "tests/test_input.c", ["mods/mutiny/input.c", "src/engine/engine.c"], []),
    ("playback", "tests/test_playback.c", ["mods/mutiny/playback.c", "src/engine/engine.c"], []),
    ("commit", "tests/test_commit.c", ["mods/mutiny/commit.c"], []),
    ("memo", "tests/test_memo_publish.c", ["mods/mutiny/commit.c"], ["-DMU_LEN_MEMO_PROBE"]),
    ("cmd", "tests/test_cmd.c", ["mods/mutiny/cmd.c"], []),
    ("port", "tests/test_port144.c", [], []),
    ("storage", "tests/test_storage.c",
     ["mods/mutiny/storage.c", "src/runtime/runtime.c", "src/engine/engine.c"], []),
    ("page", "tests/test_page.c",
     ["mods/mutiny/page.c", "mods/mutiny/preview.c", "src/engine/engine.c"], ["-DMU_ENGINE_PROBE"]),
    ("tick", "tests/test_tick.c",
     ["mods/mutiny/tick.c", "mods/mutiny/preview.c", "mods/mutiny/playback.c",
      "mods/mutiny/storage.c", "src/runtime/runtime.c", "src/engine/engine.c"],
     ["-DMU_STORE_PROBE", "-DMU_ENGINE_PROBE"]),
]

FORMAT_GLOBS = ["src/*/*.c", "src/*/*.h", "mods/mutiny/*.c", "mods/mutiny/*.h"]


def run(cmd, cwd=None, env=None, timeout=1800):
    started = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout + p.stderr, time.time() - started
    except FileNotFoundError as e:
        return 127, str(e), time.time() - started
    except subprocess.TimeoutExpired:
        return 124, "timed out after %ds" % timeout, time.time() - started


def tail(text, n=400):
    text = text.strip()
    return text if len(text) <= n else "..." + text[-n:]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def host_test(name, source, sources, flags, work):
    cc = os.environ.get("CC") or "cc"
    binary = work / name
    cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-g",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           str(REPO / source)] + flags + [str(REPO / s) for s in sources] + ["-o", str(binary)]
    code, out, _ = run(cmd)
    if code != 0:
        return "fail", "compiler: " + tail(out)
    code, out, _ = run([str(binary)])
    if code == 0 and "tests passed" in out:
        return "pass", "sanitizers clean"
    return "fail", tail(out)


def python_tests(work):
    code, out, _ = run([sys.executable, "-m", "unittest", "discover", "-s", "tests"], cwd=REPO)
    return ("pass", "unittest") if code == 0 else ("fail", tail(out))


def format_check(work):
    fmt = os.environ.get("CLANG_FORMAT") or "clang-format"
    if not shutil.which(fmt):
        return "blocked", "%s not found; install clang-format %s" % (fmt, CLANG_FORMAT_VERSION)
    _, version, _ = run([fmt, "--version"])
    if CLANG_FORMAT_VERSION not in version:
        return "blocked", "needs clang-format %s, found: %s" % (CLANG_FORMAT_VERSION,
                                                                 version.strip())
    files = sorted({str(p) for g in FORMAT_GLOBS for p in REPO.glob(g)})
    code, out, _ = run([fmt, "--dry-run", "--Werror", "--style=file"] + files)
    return ("pass", "%d files" % len(files)) if code == 0 else ("fail", tail(out))


def sdk_inputs():
    names = ("MUTINY_ELEKLOADER", "MUTINY_STOCK", "MUTINY_CORE_MOD", "MUTINY_SEQ_MOD")
    missing = [n for n in names if not os.environ.get(n)]
    if missing:
        return None, "set " + ", ".join(missing)
    env = {n: os.path.abspath(os.environ[n]) for n in names}
    for n, path in env.items():
        if not Path(path).exists():
            return None, "%s does not exist: %s" % (n, path)
    for n, want in (("MUTINY_STOCK", STOCK_SHA256), ("MUTINY_CORE_MOD", CORE_SHA256),
                    ("MUTINY_SEQ_MOD", SEQ_SHA256)):
        if sha256(env[n]) != want:
            return None, "%s is not the expected file (sha256 %s)" % (n, want)
    return env, ""


def build_mod(work):
    env, why = sdk_inputs()
    if env is None:
        return "blocked", why
    lines = []
    code, head, _ = run(["git", "rev-parse", "HEAD"], cwd=env["MUTINY_ELEKLOADER"])
    head = head.strip() if code == 0 else "unknown"
    if head != ELEKLOADER_COMMIT:
        lines.append("note: Elekloader is at %s, this release used %s" % (head, ELEKLOADER_COMMIT))
    out = work / "out"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    el = env["MUTINY_ELEKLOADER"]
    code, log, _ = run([sys.executable, "-m", "elekloader.sdk.build", str(REPO / "mods" / "mutiny"),
                        "--stock", env["MUTINY_STOCK"], "--out", str(out)],
                       cwd=el, env=dict(os.environ, ELEKLOADER_CROSS="m68k-elf-"))
    mods = sorted(out.glob("mutiny-*.elemod"))
    if code != 0 or not mods:
        return "fail", "build: " + tail(log)
    mod, image = mods[0], out / "mutiny.syx"
    code, log, _ = run([sys.executable, "-m", "elekloader.patch", "--stock", env["MUTINY_STOCK"],
                        "--mod", env["MUTINY_CORE_MOD"], "--mod", env["MUTINY_SEQ_MOD"],
                        "--mod", str(mod), "--out", str(image), "--version", "1.44"], cwd=el)
    if code != 0:
        return "fail", "patch: " + tail(log)
    code, log, _ = run([sys.executable, "-m", "elekloader.lint", str(mod),
                        "--stock", env["MUTINY_STOCK"], "--with", env["MUTINY_CORE_MOD"],
                        "--with", env["MUTINY_SEQ_MOD"], "--json"], cwd=el)
    try:
        ok = code == 0 and json.loads(log).get("ok") is True
    except ValueError:
        ok = False
    if not ok:
        return "fail", "lint: " + tail(log)
    lines += ["%s  %s" % (sha256(mod), mod), "%s  %s" % (sha256(image), image)]
    return "pass", "\n        ".join(["lint ok"] + lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build and test MUTINY.")
    ap.add_argument("what", choices=("test", "mod"))
    ap.add_argument("--work", help="keep the build files here")
    args = ap.parse_args(argv)
    work = Path(args.work).resolve() if args.work else Path(tempfile.mkdtemp(prefix="mutiny-"))
    work.mkdir(parents=True, exist_ok=True)

    checks = [(name, lambda n=name, s=src, d=deps, f=flags: host_test(n, s, d, f, work))
              for name, src, deps, flags in HOST_TESTS]
    checks += [("python", lambda: python_tests(work)), ("format", lambda: format_check(work))]
    if args.what == "mod":
        checks.append(("mod", lambda: build_mod(work)))

    failed = 0
    for name, check in checks:
        status, detail = check()
        failed += status != "pass"
        print("%-8s %-11s %s" % (status.upper(), name, detail))
    print("\n%d of %d passed" % (len(checks) - failed, len(checks)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
