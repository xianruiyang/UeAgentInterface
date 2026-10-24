#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "Algo/Sort.h"
#include "Blueprint/UserWidget.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonValue.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::QueryOwners::UmgWidget
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic CandidateDiagnostic(FString Code, FString Subject, FString PublicReason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason), TEXT("candidate_query"), MoveTemp(InternalContext), MoveTemp(Subject));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic CandidateCause(FString Code, const FString& Subject,
			const UE::UAI::Contract::FDiagnostic& Cause, FString FallbackReason)
		{
			auto Diagnostic = Cause.IsEmpty()
				? CandidateDiagnostic(MoveTemp(Code), Subject, MoveTemp(FallbackReason))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			return Diagnostic;
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
			const bool bCaptured = FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				Target, Capture, ReadError);
			if (!bCaptured || Capture.Revision.IsEmpty())
			{
				if (!ReadError.IsEmpty())
				{
					if (ReadError.TargetPath.IsEmpty()) ReadError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
					return ForwardDiagnostic(OutError, MoveTemp(ReadError));
				}
				auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_target_readback_failed"), Target.GetPathName(),
					bCaptured ? TEXT("The Widget Blueprint readback did not provide a target revision.")
						: TEXT("The Widget Blueprint readback failed without a diagnostic."));
				Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
				if (bCaptured)
				{
					Diagnostic.PropertyPath = TEXT("revision");
					Diagnostic.ActualValuePresent = false;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("target revision"));
				}
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
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
			UE::UAI::Contract::FDiagnostic SearchError;
			if (!UeAgentPaletteSearch::CollectComponentWidgetCandidates(
					Params, RawCandidates, Warnings, SearchError))
			{
				auto Diagnostic = CandidateCause(TEXT("umg_widget_candidate_catalog_failed"), Target.GetPathName(),
					SearchError, TEXT("The Widget class/template catalog could not be collected."));
				if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
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
				Candidate.SelectionFields.Add(TEXT("form"),
					WidgetClass && !WidgetClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
						? TEXT("native widget") : TEXT("widget blueprint"));
				Candidate.SelectionFields.Add(TEXT("class"), FSoftClassPath(Candidate.ClassPath).GetAssetName());
				const FString Source = ContentDiscovery::MakeReadableDiscoveryPath(Candidate.ClassPath);
				if (!Source.IsEmpty()) Candidate.SelectionFields.Add(TEXT("class_source"), Source);
				// The native palette policy accepts both loaded classes and asset-backed templates.
				// Class loading belongs to selection/creation, not catalog eligibility.
				Candidate.bSupported = true;
				Candidate.ActionKind = TEXT("widget_template_class");
				Candidate.WriteSupport = TEXT("widget_template_class_create");
				Candidate.SupportReason = TEXT("native_widget_template");

				Raw.Kind = TEXT("umg_widget_target_context");
				Raw.Name = Candidate.Name;
				Raw.DisplayName = Candidate.Label;
				Raw.FullName = Candidate.ClassPath;
				Raw.CategoryPath = Candidate.Category;
				Raw.Tooltip = Candidate.Summary;
				UE::UAI::Contract::FDiagnostic IdentityError;
				if (!UeAgentPaletteSearch::SetCandidateIdentity(
						TEXT("umg_widget_target_context"),
						Target.GetPathName() + TEXT("|") + Candidate.ClassPath,
						Candidate.Name,
						Raw,
						IdentityError))
				{
					auto Diagnostic = CandidateCause(TEXT("umg_widget_candidate_identity_failed"), Candidate.ClassPath,
						IdentityError, TEXT("The Widget candidate could not be assigned an identity."));
					if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
					if (Diagnostic.CandidateName.IsEmpty()) Diagnostic.CandidateName = Candidate.Name;
					return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
				}
				Candidate.Identity = Raw.Identity;
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
			const FString& Query,
			int32& MatchScore)
		{
			ContentDiscovery::FContentDiscoveryRequest Request;
			Request.Query = Query;
			Request.MatchMode =
				ContentDiscovery::EContentDiscoveryMatchMode::Text;
			const TArray<FString> ExtraTerms{
				Candidate.Name,
				Candidate.Category,
				Candidate.ClassPath,
				Candidate.Summary,
				Candidate.ActionKind,
				Candidate.Kind,
			};
			FString MatchReason;
			return ContentDiscovery::TryScoreContentDiscoveryMatch(
				Request,
				Candidate.Id,
				Candidate.Label,
				ExtraTerms,
				MatchScore,
				MatchReason);
		}
	}

	bool TryQueryUmgWidgetCandidates(
		UWidgetBlueprint& Target,
		const FString& Query,
		FUmgWidgetCandidateQueryResult& OutResult,
		FUmgWidgetCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_game_thread_required"), Target.GetPathName(),
				TEXT("Widget candidates must be queried on the Editor game thread."));
			Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other thread"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		OutResult.Target = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		TArray<FUmgWidgetCandidate> Catalog;
		if (!BuildCatalog(Target, Catalog, OutError))
		{
			return false;
		}
		TArray<int32> MatchedIndices;
		TMap<int32, int32> Scores;
		for (int32 CatalogIndex = 0; CatalogIndex < Catalog.Num(); ++CatalogIndex)
		{
			int32 Score = 0;
			if (MatchesQuery(Catalog[CatalogIndex], Query, Score))
			{
				if (!ContentDiscovery::IsReadableDiscoveryName(Catalog[CatalogIndex].Name)) continue;
				MatchedIndices.Add(CatalogIndex);
				Scores.Add(CatalogIndex, Score);
			}
		}
		MatchedIndices.Sort([&Scores](int32 A, int32 B)
		{
			return Scores[A] != Scores[B] ? Scores[A] > Scores[B] : A < B;
		});
		OutResult.TotalMatches = MatchedIndices.Num();
		for (const int32 MatchedIndex : MatchedIndices)
		{
			OutResult.Candidates.Add(MoveTemp(Catalog[MatchedIndex]));
		}
		return true;
	}

	bool TrySelectUmgWidgetCandidate(
		UWidgetBlueprint& Target,
		const FString& Name,
		FUmgWidgetCandidate& OutCandidate,
		FUmgWidgetCandidateQueryError& OutError,
		const TMap<FString, FString>& Select,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch)
	{
		OutCandidate = {};
		OutError = {};
		const FString CleanName = Name.TrimStartAndEnd();
		if (CleanName.IsEmpty())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("umg_widget_candidate_selector_required"), Name,
				TEXT("A widget candidate requires a non-empty name."));
			Diagnostic.SourceKind.Reset();
			Diagnostic.PropertyPath = TEXT("name");
			Diagnostic.Source.JsonPath = TEXT("/name");
			Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target);
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Name);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty candidate name"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
		}
		struct FPreparedCatalog
		{
			TArray<FUmgWidgetCandidate> Catalog;
			TArray<ContentDiscovery::FNameSelectionEntry> Entries;
		};
		const auto Prepared = ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FPreparedCatalog>(
			Batch, &Target, FString(), [&](FPreparedCatalog& Value) {
				if (!BuildCatalog(Target, Value.Catalog, OutError)) return false;
				for (const FUmgWidgetCandidate& Candidate : Value.Catalog)
					Value.Entries.Add({Candidate.Name, Candidate.Id, Candidate.SelectionFields});
				return true;
			});
		if (!Prepared) return false;
		int32 ResolvedIndex = INDEX_NONE;
		UE::UAI::Contract::FDiagnostic ResolveError;
		if (!ContentDiscovery::TryResolveNameSelection(Prepared->Entries, CleanName, Select, ResolvedIndex, ResolveError))
		{
			return ForwardDiagnostic(OutError, UeAgentAuthoringDiagnostics::CandidateFailure(ResolveError.Code, CleanName, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Target), TEXT("widget palette"), TEXT("umg.widget.candidates"), ResolveError), CleanName);
		}
		OutCandidate = Prepared->Catalog[ResolvedIndex];
		return true;
	}

	bool TryResolveUmgWidgetCandidate(UWidgetBlueprint& Target, const FString& Name,
		FUmgWidgetCandidate& OutCandidate, FUmgWidgetCandidateQueryError& OutError,
		const TMap<FString, FString>& Select, ContentDiscovery::FReadOnlyCandidateBatch* Batch)
	{
		FUmgWidgetCandidate Selected;
		OutCandidate = {};
		if (!TrySelectUmgWidgetCandidate(Target, Name, Selected, OutError, Select, Batch)) return false;
		if (!Selected.bSupported)
		{
			return ForwardDiagnostic(OutError, Selected.SupportDiagnostic, Selected.Name);
		}
		OutCandidate = Selected;
		return true;
	}

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
