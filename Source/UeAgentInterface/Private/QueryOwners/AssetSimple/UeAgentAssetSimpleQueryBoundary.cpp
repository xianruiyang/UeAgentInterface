#include "QueryOwners/AssetSimple/UeAgentAssetSimpleQueryBoundary.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

namespace UE::UAI::QueryOwners::AssetSimple
{
	using namespace ContextResource;
	using namespace Discovery;
	using namespace DomainAdapters::AssetDataSimpleEffect;

	namespace
	{
		const FString FactoryCatalogKind = TEXT("asset_data_factory");

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

		bool ContainsQuery(
			const FString& Query,
			const FUeAgentCandidateBinding& Binding)
		{
			const FString Search = Query.TrimStartAndEnd();
			if (Search.IsEmpty())
			{
				return true;
			}
			const FUeAgentCandidateSummary& Summary = Binding.GetSummary();
			return Summary.GetDisplayName().Contains(
					Search, ESearchCase::IgnoreCase)
				|| Summary.GetSummary().Contains(
					Search, ESearchCase::IgnoreCase)
				|| Binding.GetPublicRef().Contains(
					Search, ESearchCase::IgnoreCase);
		}
	}

	bool TryQueryAssetFactoryCandidates(
		const FString& CreateCommandId,
		const FString& Query,
		FAssetFactoryCandidateQueryResult& OutResult,
		FAssetFactoryCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_game_thread_required"), CreateCommandId,
				TEXT("Asset factory candidates must be queried on the Editor game thread."));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game thread"));
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("other thread"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), CreateCommandId);
		}
		TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
		UE::UAI::Contract::FDiagnostic ProfileError;
		if (!TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles, ProfileError))
		{
			return ForwardDiagnostic(OutError, CandidateCause(TEXT("asset_factory_candidate_profiles_unavailable"),
				CreateCommandId, ProfileError, TEXT("The asset factory candidate production profiles are unavailable.")), CreateCommandId);
		}
		const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
			Profiles.FindByPredicate(
				[&CreateCommandId](
					const FUeAgentAssetDataSimpleEffectProductionProfile& Value)
				{
					return Value.CapabilityId == CreateCommandId;
				});
		if (!Profile || !Profile->UsesFactoryCandidate())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_command_unsupported"), CreateCommandId,
				FString::Printf(TEXT("Command '%s' does not consume a factory_ref candidate."), *CreateCommandId));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = TEXT("/create_command");
			Diagnostic.PropertyPath = TEXT("create_command");
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(CreateCommandId);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("command that consumes factory_ref"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), CreateCommandId);
		}

		TSharedPtr<IUeAgentCatalogSource> Source;
		UE::UAI::Contract::FDiagnostic SourceError;
		if (!Composition::SimpleAuthoring::OperationHost::
				TryResolveCatalogSource(
					FactoryCatalogKind, Source, SourceError)
			|| !Source.IsValid())
		{
			return ForwardDiagnostic(OutError, CandidateCause(TEXT("asset_factory_candidate_catalog_unavailable"),
				CreateCommandId, SourceError, TEXT("The asset factory candidate catalog source is unavailable.")), CreateCommandId);
		}

		FUeAgentResourceTracker Resources;
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline;
		FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		TArray<TSharedRef<const FUeAgentCandidateBinding>> ResolvedBindings;
		FUeAgentDiscoveryError DiscoveryError;
		const FUeAgentDiscoveryQuery DiscoveryQuery(
			FactoryCatalogKind,
			Profile->ProfileId,
			EUeAgentDiscoveryMatchMode::Broad,
			EUeAgentMaterializationPolicy::IndexOnly);
		const bool bSearchSucceeded = Source->TrySearchResolved(
				DiscoveryQuery,
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Catalog,
				ResolvedBindings,
				DiscoveryError);
		if (!bSearchSucceeded || !Catalog.IsValid())
		{
			if (!DiscoveryError.IsEmpty()) return ForwardDiagnostic(OutError, DiscoveryError, CreateCommandId);
			auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_query_failed"), CreateCommandId,
				bSearchSucceeded ? TEXT("The asset factory candidate search completed without a catalog snapshot.")
					: TEXT("The asset factory candidate search failed without a diagnostic."));
			Diagnostic.PropertyPath = TEXT("catalog");
			Diagnostic.ActualValuePresent = Catalog.IsValid();
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolved catalog snapshot"));
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), CreateCommandId);
		}
		if (ResolvedBindings.Num() != Catalog->GetCandidates().Num())
		{
			auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_binding_set_mismatch"), CreateCommandId,
				TEXT("The resolved factory binding count differs from the catalog candidate count."));
			Diagnostic.PropertyPath = TEXT("candidate_bindings");
			Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(ResolvedBindings.Num());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(Catalog->GetCandidates().Num());
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), CreateCommandId);
		}
		TSet<FString> ExpectedIdentities;
		for (const FUeAgentCandidateSummary& Summary
			: Catalog->GetCandidates())
		{
			ExpectedIdentities.Add(
				Summary.GetIdentity().GetCanonicalKey());
		}
		TSet<FString> ResolvedIdentities;
		TArray<TSharedRef<const FUeAgentCandidateBinding>> EligibleBindings;
		for (const TSharedRef<const FUeAgentCandidateBinding>& Binding
			: ResolvedBindings)
		{
			if (Binding->GetCatalogRevision() != Catalog->GetRevision())
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_revision_mismatch"),
					Binding->GetSummary().GetIdentity().GetCanonicalKey(),
					TEXT("A resolved factory candidate binding belongs to a different catalog revision."));
				Diagnostic.CandidateName = Binding->GetSummary().GetDisplayName();
				Diagnostic.PropertyPath = TEXT("catalog_revision");
				const auto RevisionEvidence = [](const FUeAgentCatalogRevision& Revision)
				{
					auto Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("source_id"), Revision.GetSourceId());
					Value->SetStringField(TEXT("generation"), Revision.GetGeneration());
					Value->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
					return MakeShared<FJsonValueObject>(Value);
				};
				Diagnostic.ActualEvidence = RevisionEvidence(Binding->GetCatalogRevision());
				Diagnostic.ExpectedEvidence = RevisionEvidence(Catalog->GetRevision());
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic));
			}
			const FString& Identity =
				Binding->GetSummary().GetIdentity().GetCanonicalKey();
			if (!ExpectedIdentities.Contains(Identity)
				|| ResolvedIdentities.Contains(Identity))
			{
				auto Diagnostic = CandidateDiagnostic(TEXT("asset_factory_candidate_binding_set_mismatch"), Identity,
					!ExpectedIdentities.Contains(Identity)
						? TEXT("A resolved factory binding is absent from the catalog candidate set.")
						: TEXT("The resolved factory binding set contains a duplicate candidate."));
				Diagnostic.CandidateName = Binding->GetSummary().GetDisplayName();
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Identity);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one binding per catalog candidate"));
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Identity);
			}
			ResolvedIdentities.Add(Identity);
			const TArray<FString>& Category =
				Binding->GetSummary().GetCategoryPath();
			if (!Category.Contains(Profile->ProfileId)
				|| !Binding->Supports(EUeAgentCandidateOperation::Create))
			{
				continue;
			}
			EligibleBindings.Add(Binding);
		}

		EligibleBindings.Sort(
			[](const TSharedRef<const FUeAgentCandidateBinding>& Left,
				const TSharedRef<const FUeAgentCandidateBinding>& Right)
			{
				return Left->GetPublicRef() < Right->GetPublicRef();
			});
		TArray<int32> MatchedIndices;
		MatchedIndices.Reserve(EligibleBindings.Num());
		for (int32 Index = 0; Index < EligibleBindings.Num(); ++Index)
		{
			if (ContainsQuery(Query, *EligibleBindings[Index]))
			{
				MatchedIndices.Add(Index);
			}
		}
		OutResult.CreateCommandId = CreateCommandId;
		OutResult.ProfileId = Profile->ProfileId;
		OutResult.CatalogSourceId = Catalog->GetRevision().GetSourceId();
		OutResult.CatalogGeneration = Catalog->GetRevision().GetGeneration();
		OutResult.CatalogSequence = Catalog->GetRevision().GetSequence();
		OutResult.TotalMatches = MatchedIndices.Num();
		for (int32 MatchOffset = 0; MatchOffset < MatchedIndices.Num(); ++MatchOffset)
		{
			const FUeAgentCandidateBinding& Binding =
				*EligibleBindings[MatchedIndices[MatchOffset]];
			FAssetFactoryCandidate Candidate;
			Candidate.DisplayName = Binding.GetSummary().GetDisplayName();
			Candidate.Name = Candidate.DisplayName;
			Candidate.ClassPath = Binding.GetSummary().GetSummary();
			Candidate.ProfileId = Profile->ProfileId;
			Candidate.CandidateRef = Binding.GetPublicRef();
			if (Profile->CapabilityId == TEXT("asset.object.create"))
			{
				Candidate.Id = Binding.GetPublicRef();
				Candidate.SupportedClassPath =
					Binding.GetSummary().GetSummary();
				Candidate.bGenericReady = Binding.Supports(
					EUeAgentCandidateOperation::Create);
				Candidate.WriteStatus = Candidate.bGenericReady
					? TEXT("ready")
					: TEXT("interactive_required");
			}
			Candidate.CatalogSourceId =
				Binding.GetCatalogRevision().GetSourceId();
			Candidate.CatalogGeneration =
				Binding.GetCatalogRevision().GetGeneration();
			Candidate.CatalogSequence =
				Binding.GetCatalogRevision().GetSequence();
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		return true;
	}
}
