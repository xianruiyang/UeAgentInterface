"""Build the actual Windows candidate owner and run its isolated NTFS consumer."""
from pathlib import Path
import argparse
import json
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--case", choices=("full", "start-contract", "buffer-contract"), default="full")
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    source = here.parents[2] / "Source/UeAgentInterface/Private/Core/HjsonWorkspace"
    exe = out / "workspace_file_changes.exe"
    argv = ["clang++.exe", "-std=c++17", "-O2", "-DNDEBUG", "-DUAI_FILE_OBSERVER_STANDALONE",
            "-D_WIN32_WINNT=0x0A00", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(source),
            str(source / "UeAgentWorkspaceChangeObserver.cpp"), str(here / "main.cpp"), "-o", str(exe)]
    started = time.perf_counter()
    built = subprocess.run(argv, capture_output=True, shell=False)
    build_s = time.perf_counter() - started
    (out / "build.log").write_bytes(built.stdout + built.stderr)
    (out / "build-command.json").write_text(json.dumps(argv, indent=2), encoding="utf-8")
    if built.returncode:
        raise RuntimeError(f"build failed: {out / 'build.log'}")
    started = time.perf_counter()
    run_argv = [str(exe)]
    if args.case != "full":
        run_argv.append("--" + args.case)
    run_argv.append(str(out / "fixture"))
    run = subprocess.run(run_argv, capture_output=True, shell=False, timeout=90)
    wall_s = time.perf_counter() - started
    (out / "stdout.json").write_bytes(run.stdout)
    (out / "stderr.log").write_bytes(run.stderr)
    if run.returncode:
        raise RuntimeError(f"consumer exit={run.returncode}: {run.stderr.decode('utf-8', 'replace')}")
    result = json.loads(run.stdout)
    result.update(build_s=build_s, process_wall_s=wall_s, compiler=subprocess.check_output(["clang++.exe", "--version"]).decode().splitlines()[0])
    (out / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
