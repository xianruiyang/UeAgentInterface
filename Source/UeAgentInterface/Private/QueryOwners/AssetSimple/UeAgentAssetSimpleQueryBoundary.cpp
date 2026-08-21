#include "QueryOwners/AssetSimple/UeAgentAssetSimpleQueryBoundary.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "HAL/PlatformTime.h"

namespace UE::UAI::QueryOwners::AssetSimple
{
	using namespace ContextResource;
	using namespace Discovery;
	using namespace DomainAdapters::AssetDataSimpleEffect;

	namespace
	{
		const FString FactoryCatalogKind = TEXT("asset_data_factory");

		bool Fail(
			FAssetFactoryCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
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
		const int32 Limit,
		const int32 Cursor,
		FAssetFactoryCandidateQueryResult& OutResult,
		FAssetFactoryCandidateQueryError& OutError)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("asset_factory_candidate_game_thread_required"),
				CreateCommandId);
		}
		if (Limit < 1 || Limit > 20 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("asset_factory_candidate_page_invalid"),
				CreateCommandId,
				TEXT("limit must be 1..20 and cursor must be non-negative"));
		}

		TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
		FString ProfileError;
		if (!TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles, ProfileError))
		{
			return Fail(
				OutError,
				TEXT("asset_factory_candidate_profiles_unavailable"),
				CreateCommandId,
				ProfileError);
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
			return Fail(
				OutError,
				TEXT("asset_factory_candidate_command_unsupported"),
				CreateCommandId,
				TEXT("the command does not consume a factory_ref candidate"));
		}

		TSharedPtr<IUeAgentCatalogSource> Source;
		FString SourceError;
		if (!Composition::SimpleAuthoring::OperationHost::
				TryResolveCatalogSource(
					FactoryCatalogKind, Source, SourceError)
			|| !Source.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_factory_candidate_catalog_unavailable"),
				CreateCommandId,
				SourceError);
		}

		FUeAgentResourceTracker Resources(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::Nodes, 64},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::WorkUnits, 8192},
			{EUeAgentResourceDimension::EstimatedMemoryBytes,
				8 * 1024 * 1024},
		}));
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(10.0, Clock);
		FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentDiscoveryError DiscoveryError;
		const FUeAgentDiscoveryQuery DiscoveryQuery(
			FactoryCatalogKind,
			Profile->ProfileId,
			EUeAgentDiscoveryMatchMode::Broad,
			EUeAgentMaterializationPolicy::IndexOnly);
		if (!Source->TrySearch(
				DiscoveryQuery,
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Catalog,
				DiscoveryError)
			|| !Catalog.IsValid())
		{
			return Fail(
				OutError,
				DiscoveryError.Code.IsEmpty()
					? FString(TEXT("asset_factory_candidate_query_failed"))
					: DiscoveryError.Code,
				DiscoveryError.SubjectId,
				DiscoveryError.Detail);
		}

		TArray<TSharedRef<const FUeAgentCandidateBinding>> Matches;
		for (const FUeAgentCandidateSummary& Summary : Catalog->GetCandidates())
		{
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			DiscoveryError = {};
			if (!Source->TryResolveExact(
					Summary.GetIdentity().GetCanonicalKey(),
					Catalog->GetRevision(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					Binding,
					DiscoveryError)
				|| !Binding.IsValid())
			{
				return Fail(
					OutError,
					DiscoveryError.Code.IsEmpty()
						? FString(TEXT("asset_factory_candidate_binding_failed"))
						: DiscoveryError.Code,
					Summary.GetIdentity().GetCanonicalKey(),
					DiscoveryError.Detail);
			}
			const TArray<FString>& Category =
				Binding->GetSummary().GetCategoryPath();
			if (!Category.Contains(Profile->ProfileId)
				|| !ContainsQuery(Query, *Binding))
			{
				continue;
			}
			Matches.Add(Binding.ToSharedRef());
		}

		Matches.Sort(
			[](const TSharedRef<const FUeAgentCandidateBinding>& Left,
				const TSharedRef<const FUeAgentCandidateBinding>& Right)
			{
				return Left->GetPublicRef() < Right->GetPublicRef();
			});
		OutResult.CreateCommandId = CreateCommandId;
		OutResult.ProfileId = Profile->ProfileId;
		OutResult.CatalogSourceId = Catalog->GetRevision().GetSourceId();
		OutResult.CatalogGeneration = Catalog->GetRevision().GetGeneration();
		OutResult.CatalogSequence = Catalog->GetRevision().GetSequence();
		OutResult.TotalMatches = Matches.Num();
		const int32 Start = FMath::Min(Cursor, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			const FUeAgentCandidateBinding& Binding = *Matches[Index];
			FAssetFactoryCandidate Candidate;
			Candidate.DisplayName = Binding.GetSummary().GetDisplayName();
			Candidate.ClassPath = Binding.GetSummary().GetSummary();
			Candidate.ProfileId = Profile->ProfileId;
			Candidate.CandidateRef = Binding.GetPublicRef();
			Candidate.CatalogSourceId =
				Binding.GetCatalogRevision().GetSourceId();
			Candidate.CatalogGeneration =
				Binding.GetCatalogRevision().GetGeneration();
			Candidate.CatalogSequence =
				Binding.GetCatalogRevision().GetSequence();
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		const int32 Consumed = Start + OutResult.Candidates.Num();
		OutResult.bHasMore = Consumed < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? Consumed : INDEX_NONE;
		return true;
	}
}
