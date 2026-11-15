#include "UeAgentTextLineDiff.h"

#include <algorithm>
#include <limits>

namespace UE::UAI::HjsonWorkspace::TextLineDiff
{
	namespace
	{
		using FIndex = std::ptrdiff_t;

		bool ValidateUtf8(std::string_view Bytes, ESide Side, FError& Error)
		{
			const auto Fail = [&](EErrorCode Code, std::size_t Offset) {
				Error = {Code, Side, Offset}; return false;
			};
			for (std::size_t At = 0; At < Bytes.size();)
			{
				const auto Lead = static_cast<unsigned char>(Bytes[At]);
				if (Lead == 0) return Fail(EErrorCode::BinaryNul, At);
				if (Lead < 0x80) { ++At; continue; }
				const std::size_t Count = Lead >= 0xc2 && Lead <= 0xdf ? 2
					: Lead >= 0xe0 && Lead <= 0xef ? 3 : Lead >= 0xf0 && Lead <= 0xf4 ? 4 : 0;
				if (!Count || Bytes.size() - At < Count) return Fail(EErrorCode::InvalidUtf8, At);
				for (std::size_t Part = 1; Part < Count; ++Part) {
					const auto Byte = static_cast<unsigned char>(Bytes[At + Part]);
					if (!Byte) return Fail(EErrorCode::BinaryNul, At + Part);
					if ((Byte & 0xc0) != 0x80) return Fail(EErrorCode::InvalidUtf8, At + Part);
				}
				const auto Second = static_cast<unsigned char>(Bytes[At + 1]);
				if ((Lead == 0xe0 && Second < 0xa0) || (Lead == 0xed && Second >= 0xa0)
					|| (Lead == 0xf0 && Second < 0x90) || (Lead == 0xf4 && Second >= 0x90))
					return Fail(EErrorCode::InvalidUtf8, At);
				At += Count;
			}
			return true;
		}

		struct FLines
		{
			std::string_view Bytes;
			std::vector<std::size_t> Offsets{0};
			explicit FLines(std::string_view Input) : Bytes(Input)
			{
				std::size_t At = 0;
				while (At < Bytes.size()) {
					const auto Lf = Bytes.find('\n', At);
					At = Lf == std::string_view::npos ? Bytes.size() : Lf + 1;
					Offsets.push_back(At);
				}
			}
			FIndex Count() const { return static_cast<FIndex>(Offsets.size() - 1); }
			std::string_view At(FIndex Index) const {
				const auto I = static_cast<std::size_t>(Index);
				return Bytes.substr(Offsets[I], Offsets[I + 1] - Offsets[I]);
			}
		};

		struct FSpan { FIndex A, EndA, B, EndB; };
		struct FSplit { FIndex A = 0, B = 0; bool Found = false; };

		class FDiffer
		{
			const FLines& Old;
			const FLines& New;
			FResult& Out;
			FMetrics* Metrics;
			std::vector<FSpan> Pending;

			void ObserveCapacity(std::size_t FrontierBytes = 0) {
				if (Metrics) Metrics->PeakAuxiliaryCapacityBytes = (std::max)(Metrics->PeakAuxiliaryCapacityBytes,
					(Old.Offsets.capacity() + New.Offsets.capacity()) * sizeof(std::size_t)
					+ Pending.capacity() * sizeof(FSpan) + FrontierBytes);
			}
			bool Equal(FIndex A, FIndex B) {
				if (Metrics) ++Metrics->LineComparisons;
				return Old.At(A) == New.At(B); // Exact bytes; no hash equality assumption.
			}
			void Emit(FSpan Span) {
				if (Span.A == Span.EndA && Span.B == Span.EndB) return;
				FEdit Edit;
				Edit.OldLines = {static_cast<std::size_t>(Span.A), static_cast<std::size_t>(Span.EndA)};
				Edit.NewLines = {static_cast<std::size_t>(Span.B), static_cast<std::size_t>(Span.EndB)};
				Edit.OldBytes = {Old.Offsets[Edit.OldLines.Begin], Old.Offsets[Edit.OldLines.End]};
				Edit.NewBytes = {New.Offsets[Edit.NewLines.Begin], New.Offsets[Edit.NewLines.End]};
				if (!Out.Edits.empty() && Out.Edits.back().OldLines.End == Edit.OldLines.Begin
					&& Out.Edits.back().NewLines.End == Edit.NewLines.Begin) {
					auto& Back = Out.Edits.back();
					Back.OldLines.End = Edit.OldLines.End; Back.NewLines.End = Edit.NewLines.End;
					Back.OldBytes.End = Edit.OldBytes.End; Back.NewBytes.End = Edit.NewBytes.End;
				} else Out.Edits.push_back(Edit);
				auto& Back = Out.Edits.back();
				Back.Kind = Back.OldLines.Begin == Back.OldLines.End ? EEditKind::Insert
					: Back.NewLines.Begin == Back.NewLines.End ? EEditKind::Delete : EEditKind::Replace;
			}

			FSplit Bisect(FSpan Span)
			{
				if (Metrics) ++Metrics->BisectCalls;
				const FIndex N = Span.EndA - Span.A, M = Span.EndB - Span.B;
				const FIndex MaxD = (N + M + 1) / 2, Offset = MaxD + 1;
				const auto Length = static_cast<std::size_t>(2 * MaxD + 3);
				std::vector<FIndex> Forward(Length, -1), Reverse(Length, -1);
				ObserveCapacity((Forward.capacity() + Reverse.capacity()) * sizeof(FIndex));
				Forward[static_cast<std::size_t>(Offset + 1)] = 0;
				Reverse[static_cast<std::size_t>(Offset + 1)] = 0;
				const FIndex Delta = N - M;
				const bool Odd = Delta % 2 != 0;
				FIndex FStart = 0, FEnd = 0, RStart = 0, REnd = 0;
				for (FIndex D = 0; D < MaxD; ++D)
				{
					for (FIndex K = -D + FStart; K <= D - FEnd; K += 2) {
						const auto Slot = static_cast<std::size_t>(Offset + K);
						FIndex X = K == -D || (K != D && Forward[Slot - 1] < Forward[Slot + 1])
							? Forward[Slot + 1] : Forward[Slot - 1] + 1;
						FIndex Y = X - K;
						while (X < N && Y < M && Equal(Span.A + X, Span.B + Y)) { ++X; ++Y; }
						Forward[Slot] = X;
						if (X > N) FEnd += 2;
						else if (Y > M) FStart += 2;
						else if (Odd) {
							const FIndex Other = Offset + Delta - K;
							if (Other >= 0 && Other < static_cast<FIndex>(Length)
								&& Reverse[static_cast<std::size_t>(Other)] >= 0
								&& X >= N - Reverse[static_cast<std::size_t>(Other)])
								return {Span.A + X, Span.B + Y, true};
						}
					}
					for (FIndex K = -D + RStart; K <= D - REnd; K += 2) {
						const auto Slot = static_cast<std::size_t>(Offset + K);
						FIndex X = K == -D || (K != D && Reverse[Slot - 1] < Reverse[Slot + 1])
							? Reverse[Slot + 1] : Reverse[Slot - 1] + 1;
						FIndex Y = X - K;
						while (X < N && Y < M && Equal(Span.EndA - X - 1, Span.EndB - Y - 1)) { ++X; ++Y; }
						Reverse[Slot] = X;
						if (X > N) REnd += 2;
						else if (Y > M) RStart += 2;
						else if (!Odd) {
							const FIndex Other = Offset + Delta - K;
							if (Other >= 0 && Other < static_cast<FIndex>(Length)
								&& Forward[static_cast<std::size_t>(Other)] >= 0
								&& Forward[static_cast<std::size_t>(Other)] >= N - X) {
								const FIndex AX = Forward[static_cast<std::size_t>(Other)];
								return {Span.A + AX, Span.B + AX - (Delta - K), true};
							}
						}
					}
				}
				// Searching all shorter distances without overlap proves no common
				// line: the exact shortest script deletes and inserts the whole span.
				return {};
			}

		public:
			FDiffer(const FLines& A, const FLines& B, FResult& Result, FMetrics* Stats)
				: Old(A), New(B), Out(Result), Metrics(Stats) {}
			bool Run(FError& Error)
			{
				Pending.push_back({0, Old.Count(), 0, New.Count()});
				while (!Pending.empty()) {
					ObserveCapacity();
					FSpan Span = Pending.back(); Pending.pop_back();
					while (Span.A < Span.EndA && Span.B < Span.EndB && Equal(Span.A, Span.B)) {
						++Span.A; ++Span.B; if (Metrics) ++Metrics->PrefixLines;
					}
					while (Span.A < Span.EndA && Span.B < Span.EndB && Equal(Span.EndA - 1, Span.EndB - 1)) {
						--Span.EndA; --Span.EndB; if (Metrics) ++Metrics->SuffixLines;
					}
					if (Span.A == Span.EndA || Span.B == Span.EndB) { Emit(Span); continue; }
					if (Span.EndA - Span.A == 1) {
						FIndex Match = Span.B;
						while (Match < Span.EndB && !Equal(Span.A, Match)) ++Match;
						if (Match == Span.EndB) Emit(Span);
						else { Emit({Span.A, Span.A, Span.B, Match}); Emit({Span.EndA, Span.EndA, Match + 1, Span.EndB}); }
						continue;
					}
					if (Span.EndB - Span.B == 1) {
						FIndex Match = Span.A;
						while (Match < Span.EndA && !Equal(Match, Span.B)) ++Match;
						if (Match == Span.EndA) Emit(Span);
						else { Emit({Span.A, Match, Span.B, Span.B}); Emit({Match + 1, Span.EndA, Span.EndB, Span.EndB}); }
						continue;
					}
					const auto Split = Bisect(Span); // Frontier dies before either child runs.
					if (!Split.Found) { Emit(Span); continue; }
					if ((Split.A == Span.A && Split.B == Span.B) || (Split.A == Span.EndA && Split.B == Span.EndB)) {
						Error.Code = EErrorCode::InternalSplit; return false;
					}
					Pending.push_back({Split.A, Span.EndA, Split.B, Span.EndB});
					Pending.push_back({Span.A, Split.A, Span.B, Split.B});
				}
				if (Metrics) Metrics->ResultCapacityBytes = Out.Edits.capacity() * sizeof(FEdit);
				return true;
			}
		};
	}

	bool TryDiff(std::string_view OldBytes, std::string_view NewBytes,
		FResult& Out, FError& Error, FMetrics* Metrics)
	{
		Out = {}; Error = {}; if (Metrics) *Metrics = {};
		const auto Limit = static_cast<std::size_t>((std::numeric_limits<FIndex>::max)() - 8);
		if (OldBytes.size() > Limit || NewBytes.size() > Limit - OldBytes.size()) {
			Error.Code = EErrorCode::InputTooLarge; return false;
		}
		if (!ValidateUtf8(OldBytes, ESide::Old, Error) || !ValidateUtf8(NewBytes, ESide::New, Error)) return false;
		const FLines Old(OldBytes), New(NewBytes);
		Out.OldLineCount = static_cast<std::size_t>(Old.Count()); Out.NewLineCount = static_cast<std::size_t>(New.Count());
		Out.OldEndsWithLf = !OldBytes.empty() && OldBytes.back() == '\n';
		Out.NewEndsWithLf = !NewBytes.empty() && NewBytes.back() == '\n';
		if (!FDiffer(Old, New, Out, Metrics).Run(Error)) { Out = {}; return false; }
		return true;
	}
}
