#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"

#include "Algo/Sort.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Niagara
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("niagara_module");
		const FString InputContract = TEXT("uai.niagara.module.create/1");
		const FString PlacementContract = TEXT("uai.niagara.stack-placement/1");
		const FString RequiredContext =
			TEXT("target_ref:emitter_handle_id:stage_ref:target_usage");

		bool Fail(
			FNiagaraModuleCandidateQueryError& OutError,
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
			SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
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

		FString NormalizeUsage(const FString& Value)
		{
			FString Result = Value.TrimStartAndEnd().ToLower();
			Result.ReplaceInline(TEXT("-"), TEXT("_"));
			Result.ReplaceInline(TEXT(" "), TEXT("_"));
			return Result;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.IsA<UNiagaraSystem>()
				|| Target.IsA<UNiagaraEmitter>()
				|| Target.IsA<UNiagaraScript>();
		}

		FString CandidateRevision(
			const UObject& Target,
			const FString& CandidateName,
			const FString& TargetUsage)
		{
			return Sha256(
				CandidateName + TEXT("|") + CandidateKind + TEXT("|")
					+ InputContract + TEXT("|") + PlacementContract + TEXT("|")
					+ RequiredContext + TEXT("|")
					+ Target.GetClass()->GetPathName() + TEXT("|")
					+ NormalizeUsage(TargetUsage));
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				|| Capture.Revision.IsEmpty())
			{
				return Fail(
					OutError,
					ReadError.Code.IsEmpty()
						? FString(TEXT("niagara_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryNiagaraModuleCandidates(
		UObject& Target,
		const FString& Query,
		const FString& TargetUsage,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError, TEXT("niagara_candidate_game_thread_required"), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(OutError, TEXT("niagara_candidate_target_type_unsupported"), Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 20 || Cursor < 0)
		{
			return Fail(OutError, TEXT("niagara_candidate_page_invalid"), Target.GetPathName(), TEXT("limit must be 1..20 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = CandidateKind;
		Params.SearchText = Query;
		Params.SearchMode = TEXT("contains");
		Params.AssetPath = Target.GetPathName();
		Params.TargetUsage = TargetUsage;
		Params.bLibraryOnly = false;
		Params.bIncludeItems = true;
		Params.bIncludeGroups = false;
		Params.bIncludeCategoryTree = false;
		Params.Limit = Limit;
		Params.Offset = Cursor;
		TArray<UeAgentPaletteSearch::FCandidate> PaletteCandidates;
		TArray<FString> PaletteWarnings;
		FString SearchError;
		if (!UeAgentPaletteSearch::CollectNiagaraCandidates(
				Params, PaletteCandidates, PaletteWarnings, SearchError))
		{
			return Fail(OutError, TEXT("niagara_candidate_palette_query_failed"), Target.GetPathName(), SearchError);
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
		Algo::SortBy(MatchingCandidates,
			[](const UeAgentPaletteSearch::FCandidate& Candidate)
			{
				return Candidate.FullName;
			});

		OutResult.TotalMatches = MatchingCandidates.Num();
		const int32 Start = FMath::Min(Cursor, MatchingCandidates.Num());
		const int32 End = FMath::Min(Start + Limit, MatchingCandidates.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			const UeAgentPaletteSearch::FCandidate& Source = MatchingCandidates[Index];
			if (Source.Kind != CandidateKind || Source.FullName.IsEmpty())
			{
				continue;
			}
			FNiagaraModuleCandidate Candidate;
			Candidate.Name = Source.FullName;
			Candidate.Label = Source.DisplayName.IsEmpty() ? Source.Name : Source.DisplayName;
			Candidate.Kind = CandidateKind;
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract;
			Candidate.PlacementContract = PlacementContract;
			Candidate.RequiredContext = RequiredContext;
			Candidate.BindingRevision = CandidateRevision(Target, Candidate.Name, TargetUsage);
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool IsCurrentNiagaraModuleCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& TargetUsage,
		const FString& BindingRevision,
		FNiagaraModuleCandidateQueryError& OutError)
	{
		OutError = {};
		if (CandidateName.IsEmpty() || BindingRevision.IsEmpty())
		{
			return Fail(OutError, TEXT("niagara_candidate_binding_required"), Target.GetPathName());
		}
		FNiagaraModuleCandidateQueryResult Result;
		if (!TryQueryNiagaraModuleCandidates(
				Target, CandidateName, TargetUsage, Result, OutError, 20, 0))
		{
			return false;
		}
		const FNiagaraModuleCandidate* Candidate = Result.Candidates.FindByPredicate(
			[&CandidateName](const FNiagaraModuleCandidate& Value)
			{
				return Value.Name == CandidateName;
			});
		if (!Candidate)
		{
			return Fail(OutError, TEXT("niagara_candidate_name_unresolved"), CandidateName);
		}
		if (Candidate->BindingRevision != BindingRevision)
		{
			return Fail(OutError, TEXT("niagara_candidate_binding_stale"), CandidateName, Candidate->BindingRevision);
		}
		return true;
	}
}
