// Standalone consumer of the production source, not an alternative diff.
#include "UeAgentTextLineDiff.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <utility>

namespace Diff = UE::UAI::HjsonWorkspace::TextLineDiff;

static void Require(bool Value, const char* Reason)
{
	if (!Value) { std::cerr << Reason << '\n'; std::exit(3); }
}

static std::vector<std::string_view> Lines(std::string_view Text)
{
	std::vector<std::string_view> Result;
	for (std::size_t Begin = 0; Begin < Text.size();) {
		const auto Found = Text.find('\n', Begin);
		const auto End = Found == std::string_view::npos ? Text.size() : Found + 1;
		Result.push_back(Text.substr(Begin, End - Begin)); Begin = End;
	}
	return Result;
}

// Independent small LCS dynamic program, used only on short validation inputs.
static std::size_t ReferenceDistance(std::string_view A, std::string_view B)
{
	const auto Old = Lines(A), New = Lines(B);
	std::vector<std::size_t> Row(New.size() + 1, 0);
	for (const auto& Left : Old) {
		std::size_t Diagonal = 0;
		for (std::size_t J = 1; J <= New.size(); ++J) {
			const auto Previous = Row[J];
			Row[J] = Left == New[J - 1] ? Diagonal + 1 : (std::max)(Row[J], Row[J - 1]);
			Diagonal = Previous;
		}
	}
	return Old.size() + New.size() - 2 * Row.back();
}

static void Verify(std::string_view A, std::string_view B, const Diff::FResult& Result, bool CheckMinimum)
{
	const auto Old = Lines(A), New = Lines(B);
	Require(Result.OldLineCount == Old.size() && Result.NewLineCount == New.size(), "line counts");
	Require(Result.OldEndsWithLf == (!A.empty() && A.back() == '\n'), "old EOF");
	Require(Result.NewEndsWithLf == (!B.empty() && B.back() == '\n'), "new EOF");
	const auto Boundary = [](const auto& Sequence, std::size_t At) {
		std::size_t Bytes = 0; for (std::size_t I = 0; I < At; ++I) Bytes += Sequence[I].size(); return Bytes;
	};
	std::string Rebuilt;
	std::size_t PreviousA = 0, PreviousB = 0, Distance = 0;
	for (const auto& Edit : Result.Edits) {
		Require(Edit.OldLines.Begin <= Edit.OldLines.End && Edit.OldLines.End <= Old.size(), "old line bounds");
		Require(Edit.NewLines.Begin <= Edit.NewLines.End && Edit.NewLines.End <= New.size(), "new line bounds");
		Require(Edit.OldBytes.Begin == Boundary(Old, Edit.OldLines.Begin)
			&& Edit.OldBytes.End == Boundary(Old, Edit.OldLines.End), "old byte boundaries");
		Require(Edit.NewBytes.Begin == Boundary(New, Edit.NewLines.Begin)
			&& Edit.NewBytes.End == Boundary(New, Edit.NewLines.End), "new byte boundaries");
		Require(Edit.OldBytes.Begin >= PreviousA && Edit.NewBytes.Begin >= PreviousB, "ordered edits");
		const auto GapA = A.substr(PreviousA, Edit.OldBytes.Begin - PreviousA);
		const auto GapB = B.substr(PreviousB, Edit.NewBytes.Begin - PreviousB);
		Require(GapA == GapB, "unchanged gap differs");
		Rebuilt.append(GapA); Rebuilt.append(B.substr(Edit.NewBytes.Begin, Edit.NewBytes.End - Edit.NewBytes.Begin));
		const auto Removed = Edit.OldLines.End - Edit.OldLines.Begin, Added = Edit.NewLines.End - Edit.NewLines.Begin;
		Require(Removed || Added, "empty edit");
		Require(Edit.Kind == (!Removed ? Diff::EEditKind::Insert : !Added ? Diff::EEditKind::Delete : Diff::EEditKind::Replace), "edit kind");
		Distance += Removed + Added; PreviousA = Edit.OldBytes.End; PreviousB = Edit.NewBytes.End;
	}
	Require(A.substr(PreviousA) == B.substr(PreviousB), "unchanged tail differs");
	Rebuilt.append(A.substr(PreviousA)); Require(Rebuilt == B, "new bytes not reconstructed");
	if (CheckMinimum) Require(Distance == ReferenceDistance(A, B), "non-minimal insert/delete script");
}

static Diff::FResult Checked(std::string_view A, std::string_view B)
{
	Diff::FResult Result; Diff::FError Error;
	Require(Diff::TryDiff(A, B, Result, Error), "production diff failed");
	Verify(A, B, Result, true); return Result;
}

static void SelfCheck()
{
	const std::pair<std::string, std::string> Cases[] = {
		{"", ""}, {"", "\n"}, {"a\n", ""}, {"A\nB\nC\n", "X\nA\nB\nC\n"},
		{"A\nB\nC\n", "A\nC\n"}, {"A\nB\nC\n", "A\nX\nY\nC\n"},
		{"a\nb\nc\nd\ne\n", "A\nb\nC\nd\nE\n"}, {"a\n", "a"}, {"a", "a\n"},
		{"a\r\nb\r\n", "a\nb\n"}, {"a\rb", "a\rc"}, {"a\vb\fc\n", "a\vb\fd\n"},
		{u8"中文\n灯A: 100\n", u8"中文\n灯A: 200\n"}, {u8"a\u2028b\n", u8"a\u2028c\n"},
		{"a\nb\na\nb\n", "b\na\nb\na\n"}, {"a\nb\nc\n", "c\na\nb\n"},
		{"a\nb\n", "a\nb\nlast"}, {"a\nb\nlast", "a\nb\n"},
		{"\xef\xbb\xbf" "a\n", "a\n"}, {u8"😀\n", u8"𐀀\n"},
	};
	for (const auto& Pair : Cases) Checked(Pair.first, Pair.second);
	const auto Front = Checked("A\nB\nC\n", "X\nA\nB\nC\n");
	Require(Front.Edits.size() == 1 && Front.Edits[0].OldLines.Begin == 0 && Front.Edits[0].OldLines.End == 0
		&& Front.Edits[0].NewLines.Begin == 0 && Front.Edits[0].NewLines.End == 1, "front insertion scope");
	const auto Replace = Checked("A\nB\nC\n", "A\nX\nY\nC\n");
	Require(Replace.Edits.size() == 1 && Replace.Edits[0].OldLines.Begin == 1 && Replace.Edits[0].OldLines.End == 2
		&& Replace.Edits[0].NewLines.Begin == 1 && Replace.Edits[0].NewLines.End == 3, "replacement scope");
	std::mt19937 Random(20261108);
	for (int Case = 0; Case < 128; ++Case) {
		std::string A, B;
		for (auto* Text : {&A, &B}) {
			const auto Count = Random() % 10;
			for (unsigned I = 0; I < Count; ++I) { *Text += static_cast<char>('a' + Random() % 3); *Text += '\n'; }
			if (!Text->empty() && Random() % 2) Text->pop_back();
		}
		Checked(A, B);
	}
	const std::string Invalid[] = {std::string("a\0b", 3), "\xff", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8"};
	for (const auto& Input : Invalid) for (const bool NewSide : {false, true}) {
		Diff::FResult Result; Diff::FError Error;
		Require(!Diff::TryDiff(NewSide ? "valid\n" : Input, NewSide ? Input : "valid\n", Result, Error), "invalid accepted");
		Require(Error.Code != Diff::EErrorCode::None && Error.Side == (NewSide ? Diff::ESide::New : Diff::ESide::Old)
			&& Result.Edits.empty(), "invalid diagnostic");
	}
	std::cout << "{\"self_check\":true,\"golden_cases\":20,\"short_lcs_cases\":128,\"invalid_cases\":12}\n";
}

static void PrintResult(const Diff::FResult& Result, const Diff::FMetrics& Metrics,
	double Milliseconds, double ReadMilliseconds, double VerifyMilliseconds)
{
	std::cout << std::setprecision(12) << "{\"old_lines\":" << Result.OldLineCount << ",\"new_lines\":" << Result.NewLineCount
		<< ",\"old_final_lf\":" << (Result.OldEndsWithLf ? "true" : "false") << ",\"new_final_lf\":" << (Result.NewEndsWithLf ? "true" : "false") << ",\"edits\":[";
	bool First = true;
	for (const auto& Edit : Result.Edits) {
		if (!First) std::cout << ','; First = false;
		const char* Kind = Edit.Kind == Diff::EEditKind::Insert ? "insert" : Edit.Kind == Diff::EEditKind::Delete ? "delete" : "replace";
		std::cout << "{\"kind\":\"" << Kind << "\",\"old\":[" << Edit.OldLines.Begin << ',' << Edit.OldLines.End
			<< "],\"new\":[" << Edit.NewLines.Begin << ',' << Edit.NewLines.End << "],\"old_bytes\":[" << Edit.OldBytes.Begin << ',' << Edit.OldBytes.End
			<< "],\"new_bytes\":[" << Edit.NewBytes.Begin << ',' << Edit.NewBytes.End << "]}";
	}
	std::cout << "],\"algorithm_ms\":" << Milliseconds << ",\"input_read_ms\":" << ReadMilliseconds
		<< ",\"verification_ms\":" << VerifyMilliseconds << ",\"line_comparisons\":" << Metrics.LineComparisons
		<< ",\"prefix_lines\":" << Metrics.PrefixLines << ",\"suffix_lines\":" << Metrics.SuffixLines << ",\"bisect_calls\":" << Metrics.BisectCalls
		<< ",\"peak_auxiliary_capacity_bytes\":" << Metrics.PeakAuxiliaryCapacityBytes << ",\"result_capacity_bytes\":" << Metrics.ResultCapacityBytes << "}\n";
}

static bool Read(const char* Path, std::string& Out)
{
	std::ifstream Input(Path, std::ios::binary);
	if (!Input) return false;
	Out.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
	return !Input.bad();
}

int main(int Argc, char** Argv)
{
	if (Argc == 2 && std::string_view(Argv[1]) == "self-check") { SelfCheck(); return 0; }
	if (Argc != 4 || std::string_view(Argv[1]) != "files") { std::cerr << "self-check | files OLD NEW\n"; return 2; }
	std::string Old, New;
	const auto ReadStart = std::chrono::steady_clock::now();
	if (!Read(Argv[2], Old) || !Read(Argv[3], New)) { std::cerr << "input_read_failed\n"; return 2; }
	Diff::FResult Result; Diff::FError Error; Diff::FMetrics Metrics;
	const auto Start = std::chrono::steady_clock::now();
	const double ReadMilliseconds = std::chrono::duration<double, std::milli>(Start - ReadStart).count();
	const bool Ok = Diff::TryDiff(Old, New, Result, Error, &Metrics);
	const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	if (!Ok) { std::cerr << "line_diff_error=" << static_cast<int>(Error.Code) << " side=" << static_cast<int>(Error.Side) << " offset=" << Error.ByteOffset << '\n'; return 2; }
	const auto VerifyStart = std::chrono::steady_clock::now();
	Verify(Old, New, Result, false);
	const double VerifyMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - VerifyStart).count();
	PrintResult(Result, Metrics, Milliseconds, ReadMilliseconds, VerifyMilliseconds); return 0;
}
