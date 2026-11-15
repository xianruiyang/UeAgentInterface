"""Compile and consume the one production line diff; Git is an offline oracle only."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def run(argv, cwd, *, env=None, accepted=(0,)):
    started = time.perf_counter()
    result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, shell=False,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    elapsed = time.perf_counter() - started
    if result.returncode not in accepted:
        raise RuntimeError(f"{argv[0]} exit={result.returncode}: {result.stderr.decode('utf-8', 'replace')}")
    return result, elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++.exe")
    parser.add_argument("--git-oracle", action="store_true")
    options = parser.parse_args()
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    test = Path(__file__).resolve().parent
    plugin = test.parents[2]
    source = plugin / "Source/UeAgentInterface/Private/Core/HjsonWorkspace"
    compiler = shutil.which(options.compiler)
    if compiler is None:
        raise RuntimeError("standalone C++ compiler unavailable")
    executable = output / ("text_line_diff_check.exe" if os.name == "nt" else "text_line_diff_check")
    compile_argv = [compiler, "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(source), str(source / "UeAgentTextLineDiff.cpp"), str(test / "main.cpp"), "-o", str(executable)]
    version, _ = run([compiler, "--version"], test)
    # Preserve compile evidence even on failure.
    started = time.perf_counter()
    built = subprocess.run(compile_argv, cwd=test, capture_output=True, shell=False,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    build_seconds = time.perf_counter() - started
    (output / "standalone-build.log").write_bytes(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError(f"standalone compilation failed: {output / 'standalone-build.log'}")
    checked, _ = run([str(executable), "self-check"], test)
    self_check = json.loads(checked.stdout)
    cases = {
        "empty_insert": (b"", b"\n"),
        "front_insert": (b"A\nB\nC\n", b"X\nA\nB\nC\n"),
        "middle_delete": (b"A\nB\nC\n", b"A\nC\n"),
        "separate_edits": (b"a\nb\nc\nd\ne\n", b"A\nb\nC\nd\nE\n"),
        "missing_final_lf": (b"a\n", b"a"),
        "crlf": (b"a\r\nb\r\n", b"a\nb\n"),
        "chinese": ("中文\n灯A: 100\n".encode(), "中文\n灯A: 200\n".encode()),
        "repeat_move": (b"a\nb\na\nb\n", b"b\na\nb\na\n"),
    }
    git = shutil.which("git") if options.git_oracle else None
    if options.git_oracle and git is None:
        raise RuntimeError("offline Git oracle unavailable")
    git_version = None
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull,
               GIT_ATTR_NOSYSTEM="1", GIT_OPTIONAL_LOCKS="0", LC_ALL="C", LANG="C")
    if git:
        git_version = run([git, "--version"], output, env=env)[0].stdout.decode().strip()
    hunk = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", re.M)
    rows = []
    for name, (old, new) in cases.items():
        folder = output / "cases" / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "old").write_bytes(old)
        (folder / "new").write_bytes(new)
        actual, wall = run([str(executable), "files", str(folder / "old"), str(folder / "new")], test)
        result = json.loads(actual.stdout)
        (folder / "result.json").write_bytes(actual.stdout)
        row = {"case": name, "result": result, "process_wall_s": wall}
        if git:
            git_env = {**env, "GIT_CEILING_DIRECTORIES": str(folder.parent)}
            argv = [git, "--no-pager", "-c", "core.attributesFile=" + os.devnull,
                    "diff", "--no-index", "--no-ext-diff", "--no-textconv", "--no-color", "--no-renames",
                    "--diff-algorithm=myers", "--no-indent-heuristic", "--unified=0", "--inter-hunk-context=0", "--", "old", "new"]
            oracle, oracle_wall = run(argv, folder, env=git_env, accepted=(0, 1))
            (folder / "git.patch").write_bytes(oracle.stdout)
            ranges = []
            for match in hunk.finditer(oracle.stdout):
                a, ac, b, bc = (int(value) if value is not None else 1 for value in match.groups())
                ranges.append(([a - 1 if ac else a, a - 1 + ac if ac else a],
                               [b - 1 if bc else b, b - 1 + bc if bc else b]))
            if oracle.returncode == 1 and not ranges:
                raise AssertionError(f"oracle produced no text hunks: {name}")
            oracle_distance = sum(a[1] - a[0] + b[1] - b[0] for a, b in ranges)
            distance = sum(edit["old"][1] - edit["old"][0] + edit["new"][1] - edit["new"][0] for edit in result["edits"])
            assert distance == oracle_distance, name
            if name != "repeat_move":
                assert [(edit["old"], edit["new"]) for edit in result["edits"]] == ranges, name
            row.update(git_exit=oracle.returncode, git_ranges=ranges, git_process_wall_s=oracle_wall)
        rows.append(row)
    base_lines = [f"line {index:06d} 中文 value unchanged\n".encode() for index in range(100_000)]
    base = b"".join(base_lines)
    middle = list(base_lines)
    middle[50_000] = b"line 050000 changed value\n"
    scattered = list(base_lines)
    for index in (10, 50_000, 99_990):
        scattered[index] = f"modified {index}\n".encode()
    benchmarks = {
        "100k_middle_replace": (base, b"".join(middle)),
        "100k_front_insert": (base, b"X\n" + base),
        "100k_scattered_replace": (base, b"".join(scattered)),
        "2k_disjoint": (b"a\n" * 2_000, b"b\n" * 2_000),
        "20k_repeated": (b"a\nb\n" * 10_000, b"a\nb\n" * 5_000 + b"a\n" + b"a\nb\n" * 5_000),
    }
    measured = []
    for name, (old, new) in benchmarks.items():
        folder = output / "cases" / name
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "old").write_bytes(old)
        (folder / "new").write_bytes(new)
        actual, wall = run([str(executable), "files", str(folder / "old"), str(folder / "new")], test)
        result = json.loads(actual.stdout)
        (folder / "result.json").write_bytes(actual.stdout)
        measured.append({"case": name, "old_bytes": len(old), "new_bytes": len(new), "result": result, "process_wall_s": wall})
    receipt = {"compiler": version.stdout.decode().split("\n", 1)[0], "compile_argv": compile_argv,
               "standalone_build_s": build_seconds, "self_check": self_check, "git_version": git_version,
               "git_policy": "myers; no-indent-heuristic; U0; no config/attributes/ext-diff/textconv; no-index; no repository writes",
               "cases": rows, "benchmarks": measured,
               "scope": "same production C++ source; standalone only; no UE/native import or public command integration"}
    (output / "verification.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"receipt": str(output / "verification.json"), "self_check": self_check,
                      "git_cases": len(rows) if git else 0,
                      "algorithm_ms": {row["case"]: row["result"]["algorithm_ms"] for row in measured}}, ensure_ascii=False))


if __name__ == "__main__":
    main()
