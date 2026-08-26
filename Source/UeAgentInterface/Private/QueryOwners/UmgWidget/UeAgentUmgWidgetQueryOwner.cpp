#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"

#include "Algo/Sort.h"
#include "Blueprint/UserWidget.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::QueryOwners::UmgWidget
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CoverageScope =
			TEXT("umg_loaded_widget_class_and_blueprint_asset_approximation");

		bool Fail(
			FUmgWidgetCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Normalize(const FString& Value)
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
			UWidgetBlueprint& Target,
			FString& OutRevision,
			FUmgWidgetCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = TEXT("transient:") + Target.GetClass()->GetPathName();
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
						? FString(TEXT("umg_widget_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}

		bool BuildCatalog(
			UWidgetBlueprint& Target,
			TArray<FUmgWidgetCandidate>& OutCandidates,
			FUmgWidgetCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FSearchParams Params;
			Params.CatalogType = TEXT("umg_widget");
			Params.AssetPath = Target.GetPathName();
			TArray<UeAgentPaletteSearch::FCandidate> RawCandidates;
			TArray<FString> Warnings;
			FString SearchError;
			if (!UeAgentPaletteSearch::CollectComponentWidgetCandidates(
					Params, RawCandidates, Warnings, SearchError))
			{
				return Fail(
					OutError,
					TEXT("umg_widget_candidate_catalog_failed"),
					Target.GetPathName(),
					SearchError);
			}

			OutCandidates.Reset();
			for (UeAgentPaletteSearch::FCandidate& Raw : RawCandidates)
			{
				if (Raw.FullName.IsEmpty())
				{
					continue;
				}
				UClass* const WidgetClass = FindObject<UClass>(nullptr, *Raw.FullName);
				if (Target.GeneratedClass
					&& WidgetClass == Target.GeneratedClass)
				{
					continue;
				}

				FUmgWidgetCandidate Candidate;
				Candidate.Name = Raw.Name;
				Candidate.Label = Raw.DisplayName.IsEmpty()
					? Raw.Name
					: Raw.DisplayName;
				Candidate.Kind = TEXT("umg_widget_class");
				Candidate.Category = Raw.CategoryPath;
				Candidate.Summary = Raw.Tooltip;
				Candidate.ClassPath = Raw.FullName;
				Candidate.SourceOrigin = Raw.SourceOrigin;
				Candidate.bSupported = WidgetClass
					&& WidgetClass->IsChildOf(UWidget::StaticClass())
					&& !WidgetClass->IsChildOf(UUserWidget::StaticClass())
					&& !WidgetClass->HasAnyClassFlags(
						CLASS_Abstract | CLASS_Deprecated
						| CLASS_NewerVersionExists | CLASS_CompiledFromBlueprint);
				Candidate.ActionKind = Candidate.bSupported
					? FString(TEXT("widget_template_class"))
					: FString(TEXT("widget_template_asset_search_only"));
				Candidate.WriteSupport = Candidate.bSupported
					? FString(TEXT("widget_template_class_create"))
					: FString(TEXT("search_only"));
				Candidate.SupportReason = Candidate.bSupported
					? FString(TEXT("public_widget_template_class"))
					: FString(TEXT("asset_or_user_widget_template_not_yet_supported"));

				Raw.Kind = TEXT("umg_widget_target_context");
				Raw.Name = Candidate.Name;
				Raw.DisplayName = Candidate.Label;
				Raw.FullName = Candidate.ClassPath;
				Raw.CategoryPath = Candidate.Category;
				Raw.Tooltip = Candidate.Summary;
				FString IdentityError;
				if (!UeAgentPaletteSearch::SetCandidateIdentity(
						TEXT("umg_widget_target_context"),
						Target.GetPathName() + TEXT("|") + Candidate.ClassPath,
						Candidate.Name,
						Raw,
						IdentityError))
				{
					return Fail(
						OutError,
						TEXT("umg_widget_candidate_identity_failed"),
						Candidate.ClassPath,
						IdentityError);
				}
				Candidate.Id = Raw.GetPublicId();
				OutCandidates.Add(MoveTemp(Candidate));
			}

			Algo::Sort(OutCandidates,
				[](const FUmgWidgetCandidate& A, const FUmgWidgetCandidate& B)
				{
					const int32 LabelOrder = A.Label.Compare(B.Label);
					return LabelOrder != 0
						? LabelOrder < 0
						: A.ClassPath < B.ClassPath;
				});
			return true;
		}

		bool MatchesQuery(
			const FUmgWidgetCandidate& Candidate,
			const FString& Query)
		{
			const FString Needle = Normalize(Query);
			return Needle.IsEmpty()
				|| Normalize(Candidate.Name + TEXT("|")
					+ Candidate.Label + TEXT("|")
					+ Candidate.Category + TEXT("|")
					+ Candidate.ClassPath).Contains(Needle);
		}
	}

	bool TryQueryUmgWidgetCandidates(
		UWidgetBlueprint& Target,
		const FString& Query,
		FUmgWidgetCandidateQueryResult& OutResult,
		FUmgWidgetCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_page_invalid"),
				Target.GetPathName(),
				TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		OutResult.CoverageScope = CoverageScope;
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		TArray<FUmgWidgetCandidate> Catalog;
		if (!BuildCatalog(Target, Catalog, OutError))
		{
			return false;
		}
		const FString CleanId = CandidateId.TrimStartAndEnd();
		TArray<FUmgWidgetCandidate> Matches;
		for (FUmgWidgetCandidate& Candidate : Catalog)
		{
			if ((!CleanId.IsEmpty()
					&& !Candidate.Id.Equals(CleanId, ESearchCase::IgnoreCase))
				|| !MatchesQuery(Candidate, Query))
			{
				continue;
			}
			Matches.Add(MoveTemp(Candidate));
		}
		if (!CleanId.IsEmpty() && Matches.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_not_found"),
				CleanId);
		}

		OutResult.TotalMatches = Matches.Num();
		const int32 End = FMath::Min(Cursor + Limit, Matches.Num());
		for (int32 Index = Cursor; Index < End; ++Index)
		{
			OutResult.Candidates.Add(MoveTemp(Matches[Index]));
		}
		OutResult.bHasMore = End < Matches.Num();
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.bCoverageComplete = false;
		OutResult.bCreationCoverageComplete = false;
		return true;
	}

	bool TryResolveUmgWidgetCandidate(
		UWidgetBlueprint& Target,
		const FString& Name,
		const FString& Id,
		FUmgWidgetCandidate& OutCandidate,
		FUmgWidgetCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutError = {};
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		if (CleanName.IsEmpty() && CleanId.IsEmpty())
		{
			return Fail(OutError, TEXT("umg_widget_candidate_selector_required"));
		}
		TArray<FUmgWidgetCandidate> Catalog;
		if (!BuildCatalog(Target, Catalog, OutError))
		{
			return false;
		}
		TArray<const FUmgWidgetCandidate*> Matches;
		for (const FUmgWidgetCandidate& Candidate : Catalog)
		{
			if ((!CleanId.IsEmpty()
					&& !Candidate.Id.Equals(CleanId, ESearchCase::IgnoreCase))
				|| (!CleanName.IsEmpty()
					&& !Candidate.Name.Equals(
						CleanName, ESearchCase::CaseSensitive)))
			{
				continue;
			}
			Matches.Add(&Candidate);
		}
		if (Matches.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_not_found"),
				CleanId.IsEmpty() ? CleanName : CleanId);
		}
		if (Matches.Num() != 1)
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_ambiguous"),
				CleanName,
				TEXT("use exact candidate id"));
		}
		if (!Matches[0]->bSupported)
		{
			return Fail(
				OutError,
				TEXT("umg_widget_candidate_not_writable"),
				Matches[0]->Id,
				Matches[0]->SupportReason);
		}
		OutCandidate = *Matches[0];
		return true;
	}

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
