"""Builds or runs tests with the full log on disk and only errors, failures and a summary on stdout.

quiet.py build <build dir> [config] [-nounity] [target ...]   -nounity turns unity builds off in that build dir
quiet.py test <build dir> [config] [ctest args ...]
"""

import os
import re
import subprocess
import sys
import time

MAX_LINES = 40

BUILD_ERROR = re.compile(r"\berror\b|: fatal error|\bLNK\d{4}\b|^FAILED:|CMake Error|ninja: build stopped", re.I)
TEST_DETAIL = re.compile(r"ERROR:|FATAL|logged:|REQUIRE|CHECK|TEST CASE:|SUBCASE:|Exception|\*\*\*|values:|Timeout|Failed|SEGFAULT")
TEST_SUMMARY = re.compile(r"tests passed|The following tests FAILED|^\s*\d+ - .*\((Failed|Timeout|SEGFAULT|Exception|Not Run)|Total Test time")


def run(command, log_path):
    start = time.time()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    with open(log_path, encoding="utf-8", errors="replace") as log:
        lines = log.read().splitlines()
    return process.returncode, lines, time.time() - start


def emit(lines):
    seen = []
    for line in lines:
        line = line.rstrip()
        if line and line not in seen:
            seen.append(line)
    for line in seen[:MAX_LINES]:
        print(line)
    if len(seen) > MAX_LINES:
        print(f"... {len(seen) - MAX_LINES} more")


def config_and_rest(args):
    if args and args[0] in ("Debug", "RelWithDebInfo", "Release"):
        return args[0], args[1:]
    return "RelWithDebInfo", args


def configure(build_dir, extra=()):
    if os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        command = ["cmake", build_dir, *extra]
    else:
        command = ["cmake", "--preset", "windows", "-B", build_dir, *extra]
    os.makedirs(build_dir, exist_ok=True)
    code, lines, _ = run(command, os.path.join(build_dir, "last_configure.log"))
    if code != 0:
        emit(line for line in lines if BUILD_ERROR.search(line))
        print(f"configure FAILED, log {os.path.join(build_dir, 'last_configure.log')}")
    return code


def build(build_dir, args):
    if "-nounity" in args:
        args = [a for a in args if a != "-nounity"]
        code = configure(build_dir, ["-DLILA_UNITY_BUILD=OFF"])
        if code != 0:
            return code
    elif not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        code = configure(build_dir)
        if code != 0:
            return code
    config, targets = config_and_rest(args)
    command = ["cmake", "--build", build_dir, "--config", config]
    for target in targets:
        command += ["--target", target]
    # Keep going past the first failure so one pass reports every broken file.
    command += ["--", "-k", "0"]
    log_path = os.path.join(build_dir, "last_build.log")
    code, lines, seconds = run(command, log_path)
    # MSVC's "warning C4100" and slangc's "warning[E30056]" with the "-->" location line after it.
    warned = [i for i, line in enumerate(lines) if re.search(r"\bwarning(\[[A-Z]+\d+\]| [A-Z]+\d+)", line)]
    if code == 0:
        emit(lines[i].rstrip() + (" " + lines[i + 1].strip() if i + 1 < len(lines) and "-->" in lines[i + 1] else "") for i in warned)
        print(f"build OK {seconds:.0f}s, {len(warned)} warnings")
        return 0
    emit(line for line in lines if BUILD_ERROR.search(line))
    print(f"build FAILED {seconds:.0f}s, log {log_path}")
    return code


def test(build_dir, args):
    config, extra = config_and_rest(args)
    command = ["ctest", "--test-dir", build_dir, "-C", config, "-j", str(os.cpu_count() // 2), "--output-on-failure"] + extra
    log_path = os.path.join(build_dir, "last_test.log")
    code, lines, seconds = run(command, log_path)
    if code == 0:
        summary = next((line for line in lines if "tests passed" in line), "")
        print(f"tests OK {seconds:.0f}s {summary.strip()}")
        return 0
    emit(line for line in lines if TEST_DETAIL.search(line) or TEST_SUMMARY.search(line))
    print(f"tests FAILED {seconds:.0f}s, log {log_path}")
    return code


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("build", "test"):
        print(__doc__.strip())
        return 2
    mode, build_dir, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    return build(build_dir, rest) if mode == "build" else test(build_dir, rest)


if __name__ == "__main__":
    sys.exit(main())
