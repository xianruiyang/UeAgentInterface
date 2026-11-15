# Internal text line difference

`Private/Core/HjsonWorkspace/UeAgentTextLineDiff.h/.cpp` provides one pure C++
implementation for two caller-owned versions of one UTF-8 text file. It has no
Git, filesystem, SDK, UObject or parser dependency. The original HJSON parser's
`TryUpdateHjsonDocument` now consumes it when the matching old raw text and AST
are available. The Source resolver calls that parser entry; SDK and publisher
do not implement a second diff. Missing old text or an unsafe syntax boundary
uses the original parser for that actual document, never a complete workspace
or UE capture. Text ranges remain syntax candidates, not author intent.

## Call and result

```cpp
#include "Core/HjsonWorkspace/UeAgentTextLineDiff.h"

namespace Lines = UE::UAI::HjsonWorkspace::TextLineDiff;
Lines::FResult Difference;
Lines::FError Error;
const std::string_view Before = "A\nB\nC\n";
const std::string_view After = "X\nA\nB\nC\n";
if (Lines::TryDiff(Before, After, Difference, Error))
{
    // One Insert: OldLines [0,0), NewLines [0,1),
    // OldBytes [0,0), NewBytes [0,2). A/B/C remain unchanged.
}
else
{
    // Inspect Error.Code; input errors also identify Side and ByteOffset.
    // A failed comparison is not an empty successful diff.
}
```

All ranges are **zero-based and half-open**. Each edit has old/new line ranges
and corresponding raw byte ranges. Empty ranges identify insertion/deletion
points, including EOF. Adjacent deletions and additions form one `Replace`
block; their counts can differ. Equal gaps separate blocks. No unique pairing
of duplicate lines or semantic move identity is inferred.

The caller supplies both actual versions. The algorithm does not recover old
text from a digest, parsed tree, UE snapshot or canonical reserialization.
An empty old input means a real empty version, not a missing old version.
Inputs must remain immutable during the call; the result contains offsets,
not borrowed text pointers. Extracting text later requires the corresponding
version to remain available to the caller.

## Exact text rules

- LF byte `0x0a` alone ends a line and belongs to that line. CR is content;
  CRLF and LF therefore differ. Bare CR, VT, FF and Unicode separators do not
  silently become extra line separators.
- Empty bytes have zero lines. A final LF does not create a phantom extra
  line. `a` and `a\n` are different one-line versions; the result records both
  final-LF flags.
- UTF-8, Chinese, supplementary code points and a UTF-8 BOM are preserved
  byte-for-byte. BOM is content, not silently removed.
- NUL is rejected as `BinaryNul`; malformed UTF-8, overlong encodings,
  surrogate encodings, truncated code points and values above U+10FFFF are
  rejected as `InvalidUtf8`. This is a UTF-8 text contract, not a general
  binary/UTF-16 comparator. A future HJSON caller must retain its existing
  encoding support instead of using this contract to reject legal imports.
- `InputTooLarge` denotes the implementation's signed index representability,
  not a configured document quota. Allocator failures follow the hosting C++
  runtime policy; the function does not install a second allocation/recovery
  system or claim to return an error after every host out-of-memory failure.

Text changes do not imply author edits or UE changes. Numeric spellings,
formatting and system writeback are reported literally. Native type/presence,
semantic equivalence, actual effects, publication baselines and concurrency
remain the responsibility of their existing callers.

## Algorithm and cost

The implementation builds LF byte boundaries, strips exact common line
prefixes/suffixes, then uses forward/reverse Myers bisection on the remaining
middle. Explicit pending spans replace recursive copying. Frontier arrays are
released before either child is processed; no complete O(D²) trace is kept.
Exact byte comparisons determine equality; there is no line hash or autojunk.
Fixed traversal/tie rules make the output deterministic for the same inputs.

It produces a shortest insertion/deletion script, consolidating adjacent
changes into blocks. Replacing a whole middle is allowed only when the exact
search proves no common line, or a one-line base case has no match. There is
no deadline/size heuristic that silently marks an uncertain middle all changed.
Git's indentation/hunk heuristics are not reproduced, so equivalent alignments
of repeated lines need not match its patch text.

For B total input bytes, LF indexing/validation and prefix/suffix work are
linear. For n remaining lines and D shortest insertions/deletions, Myers has
O(nD) line-comparison steps and linear auxiliary space. Byte comparisons of
long lines have their actual comparison cost; they are not universally O(1).
Large reorderings or largely disjoint inputs can still take quadratic time.
No claim of universally fastest or constant-time insertion is made.

Optional `FMetrics` measures the work of this one call. It introduces no
extra traversal/hash/allocation. `PeakAuxiliaryCapacityBytes` is the maximum
observed capacity of the two offset tables, pending spans and live frontiers;
it excludes caller input buffers, result backing, allocator overhead and
transient allocator reallocation peaks. It is **not process peak RAM**.
`ResultCapacityBytes` is reported separately. Passing no metrics pointer is
the normal unmeasured call.

## Independent verification

The consumer at `tests/standalone/text_line_diff` compiles this exact source,
reconstructs the new bytes from the output, checks short scripts against an
independent LCS reference and optionally compares fixed Git Myers hunks.
It is a test executable, not a public command or a second runtime algorithm.
See that directory's README for the invocation. No UE business integration is
implied by standalone verification or by a normal plugin Development build.
