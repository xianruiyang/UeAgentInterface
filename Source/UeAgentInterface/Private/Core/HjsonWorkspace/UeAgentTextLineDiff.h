#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace UE::UAI::HjsonWorkspace::TextLineDiff
{
	struct FRange
	{
		std::size_t Begin = 0;
		std::size_t End = 0;
	};

	enum class EEditKind { Insert, Delete, Replace };

	struct FEdit
	{
		EEditKind Kind = EEditKind::Replace;
		FRange OldLines, NewLines;
		FRange OldBytes, NewBytes;
	};

	struct FResult
	{
		std::size_t OldLineCount = 0, NewLineCount = 0;
		bool OldEndsWithLf = false, NewEndsWithLf = false;
		std::vector<FEdit> Edits;
	};

	enum class EErrorCode { None, BinaryNul, InvalidUtf8, InputTooLarge, InternalSplit };
	enum class ESide { Old, New };
	struct FError
	{
		EErrorCode Code = EErrorCode::None;
		ESide Side = ESide::Old;
		std::size_t ByteOffset = 0;
	};

	// Optional, request-local observations for the standalone consumer. No extra
	// traversal, hashing, persistent state or allocation is done to collect them.
	struct FMetrics
	{
		std::uint64_t LineComparisons = 0, PrefixLines = 0, SuffixLines = 0, BisectCalls = 0;
		std::size_t PeakAuxiliaryCapacityBytes = 0, ResultCapacityBytes = 0;
	};

	// One UTF-8 file, two caller-owned immutable byte versions. LF alone ends a
	// line; CR, BOM and other separators remain content. The terminating LF is
	// part of its line, so a missing final LF is a real change. Empty input has
	// zero lines. All line/byte ranges are zero-based and half-open; an empty
	// range denotes the exact insertion/deletion point, including EOF.
	//
	// Exact linear-space Myers insert/delete script, with equal-prefix/suffix
	// stripping and deterministic ties. Adjacent deletions/insertions form one
	// Replace block, without inventing a one-to-one mapping or move identity.
	// No Git/process/filesystem/UE dependency; no semantic or manual attribution.
	// False reports a concrete input/internal error, never an empty successful
	// diff. Allocation failures retain the hosting C++ runtime's allocation policy.
	bool TryDiff(std::string_view OldBytes, std::string_view NewBytes,
		FResult& Out, FError& Error, FMetrics* Metrics = nullptr);
}
