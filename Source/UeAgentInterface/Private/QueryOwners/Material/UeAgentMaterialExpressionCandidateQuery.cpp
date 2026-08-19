#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"

#include "Algo/Sort.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Material
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("material_expression");
		const FString InputContract = TEXT("uai.material.expression.create/1");
		const FString PlacementContract =
			TEXT("uai.material.expression.graph-placement/1");
		const FString RequiredContext =
			TEXT("editor_game_thread:material_or_material_function");

		bool Fail(
			FMaterialExpressionCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.IsA<UMaterial>() || Target.IsA<UMaterialFunction>();
		}

		FString CandidateRevision(
			const UObject& Target,
			const FString& CandidateName)
		{
			return Sha256(
				CandidateName + TEXT("|") + CandidateKind + TEXT("|")
				+ InputContract + TEXT("|") + PlacementContract + TEXT("|")
				+ RequiredContext + TEXT("|")
				+ Target.GetClass()->GetPathName());
		}

		FString NormalizeSearch(const FString& Value)
		{
			FString Result;
			for (const TCHAR Character : Value.ToLower())
			{
				if (FChar::IsAlnum(Character))
				{
					Result.AppendChar(Character);
				}
			}
			return Result;
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FMaterialExpressionCandidateQueryError& OutError)
		{
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				|| Capture.Revision.IsEmpty())
			{
				return Fail(
					OutError,
					ReadError.Code.IsEmpty()
						? FString(TEXT("material_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryMaterialExpressionCandidates(
		UObject& Target,
		const FString& Query,
		FMaterialExpressionCandidateQueryResult& OutResult,
		FMaterialExpressionCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("material_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError,
				TEXT("material_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 20 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("material_candidate_page_invalid"),
				Target.GetPathName(),
				TEXT("limit must be 1..20 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("material_expression");
		Params.SearchText = Query;
		Params.SearchMode = TEXT("contains");
		Params.AssetPath = Target.GetPathName();
		Params.Limit = Limit;
		Params.Offset = Cursor;
		Params.bIncludeItems = true;
		Params.bIncludeGroups = false;
		Params.bIncludeCategoryTree = false;
		TArray<UeAgentPaletteSearch::FCandidate> PaletteCandidates;
		TArray<FString> PaletteWarnings;
		FString SearchError;
		if (!UeAgentPaletteSearch::CollectMaterialCandidates(
				Params, PaletteCandidates, PaletteWarnings, SearchError))
		{
			return Fail(
				OutError,
				TEXT("material_candidate_palette_query_failed"),
				Target.GetPathName(),
				SearchError);
		}

		const FString NormalizedQuery = NormalizeSearch(Query);
		TArray<UeAgentPaletteSearch::FCandidate> MatchingCandidates;
		for (UeAgentPaletteSearch::FCandidate& Candidate : PaletteCandidates)
		{
			const FString Searchable = NormalizeSearch(
				Candidate.Name + TEXT("|") + Candidate.DisplayName
				+ TEXT("|") + Candidate.FullName + TEXT("|")
				+ Candidate.Keywords);
			if (NormalizedQuery.IsEmpty() || Searchable.Contains(NormalizedQuery))
			{
				MatchingCandidates.Add(MoveTemp(Candidate));
			}
		}
		Algo::SortBy(
			MatchingCandidates,
			[](const UeAgentPaletteSearch::FCandidate& Candidate)
			{
				return Candidate.FullName;
			});
		OutResult.TotalMatches = MatchingCandidates.Num();
		const int32 End = FMath::Min(Cursor + Limit, MatchingCandidates.Num());
		for (int32 Index = FMath::Min(Cursor, MatchingCandidates.Num());
			Index < End;
			++Index)
		{
			const UeAgentPaletteSearch::FCandidate& Source =
				MatchingCandidates[Index];
			if (Source.Kind != CandidateKind || Source.FullName.IsEmpty())
			{
				continue;
			}
			FMaterialExpressionCandidate Candidate;
			Candidate.Name = Source.FullName;
			Candidate.Label = Source.DisplayName.IsEmpty()
				? Source.Name
				: Source.DisplayName;
			Candidate.Kind = CandidateKind;
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract;
			Candidate.PlacementContract = PlacementContract;
			Candidate.RequiredContext = RequiredContext;
			Candidate.BindingRevision = CandidateRevision(Target, Candidate.Name);
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}

		const int32 Consumed = Cursor + OutResult.Candidates.Num();
		OutResult.bHasMore = Consumed < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? Consumed : INDEX_NONE;
		return true;
	}

	bool IsCurrentMaterialExpressionCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& BindingRevision,
		FMaterialExpressionCandidateQueryError& OutError)
	{
		OutError = {};
		if (CandidateName.IsEmpty() || BindingRevision.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("material_candidate_binding_required"),
				Target.GetPathName());
		}
		FMaterialExpressionCandidateQueryResult Result;
		if (!TryQueryMaterialExpressionCandidates(
				Target,
				CandidateName,
				Result,
				OutError,
				20,
				0))
		{
			return false;
		}
		const FMaterialExpressionCandidate* Candidate =
			Result.Candidates.FindByPredicate(
				[&CandidateName](const FMaterialExpressionCandidate& Value)
				{
					return Value.Name == CandidateName;
				});
		if (!Candidate)
		{
			return Fail(
				OutError,
				TEXT("material_candidate_name_unresolved"),
				CandidateName);
		}
		if (Candidate->BindingRevision != BindingRevision)
		{
			return Fail(
				OutError,
				TEXT("material_candidate_binding_stale"),
				CandidateName,
				Candidate->BindingRevision);
		}
		return true;
	}
}
