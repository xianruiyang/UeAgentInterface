# Standalone line-diff consumer

This compiles the **same** production `UeAgentTextLineDiff.cpp`; it contains no
replacement line-diff implementation. A short LCS dynamic program is only an
independent minimality oracle. The private API contract is documented in
[`docs/HJSON_TEXT_LINE_DIFF.md`](../../../docs/HJSON_TEXT_LINE_DIFF.md).

From the project root, with a Clang-compatible C++17 driver and standard C++
headers/linker available:

```powershell
python.exe Plugins/UeAgentInterface/tests/standalone/text_line_diff/verify.py --output tmp/uai-observe/hjson-line-diff/<run-id> --git-oracle
```

`--compiler` can select the compiler executable. Omit `--git-oracle` to run
without Git. Git is used only by this optional offline oracle, with isolated
configuration/attributes, fixed Myers/no-indent-heuristic/U0 arguments and
`--no-index`; exit 1 means differences. No repository/index is modified.

The runner writes compiler evidence, fixed old/new files, exact ranges and a
`verification.json` receipt under the requested run. It covers 20 boundary
examples, 128 deterministic short LCS cases and 12 invalid-side cases; the
eight Git references reuse boundary cases. Five fixed-size text cases report
algorithm time separately from file reading, reconstruction verification and
outer process wall time. That outer remainder includes startup, output and
scheduling; it is not a separately measured startup time.

The executable also supports `files OLD NEW`, strictly for this standalone
consumer, and prints line/byte blocks plus request-local measurements. Its
read failures are errors. It reads both files once; the production algorithm
itself only accepts immutable byte views and performs no file I/O.

These are algorithm-only cases, not UE/asset tests. Normal project rules for
plugin C++ changes, Development builds and Editor preservation still apply.
