#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/Families/AssetData/UeAgentAssetDataSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Factories/Factory.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UAI::Tests::AssetDataSimpleEffect
{
	using namespace Composition::SimpleAuthoring;
	using namespace Composition::SimpleAuthoring::AssetData;
	using namespace ContextResource;
	using namespace AssetDurable;
	using namespace Discovery;
	using namespace DomainAdapters::AssetDataSimpleEffect;
	using namespace ExecutionMutation;

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUeAgentAssetDataSimpleEffectGenericFactoryPersistedArtifactTest,
		"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.C9."
			"GenericFactoryPersistedArtifact",
		EAutomationTestFlags::EditorContext
			| EAutomationTestFlags::EngineFilter)

	bool FUeAgentAssetDataSimpleEffectGenericFactoryPersistedArtifactTest::
	RunTest(const FString& Parameters)
	{
		const FString PackageName =
			TEXT("/Game/UAI/Tests/T103/LoadedObjectFalsePositive");
		const FString ObjectName = TEXT("LoadedObjectFalsePositive");
		const FString ObjectPath =
			PackageName + TEXT(".") + ObjectName;
		UPackage* LivePackage = CreatePackage(*PackageName);
		UTexture2D* LiveObject = NewObject<UTexture2D>(
			LivePackage,
			FName(*ObjectName),
			RF_Public | RF_Standalone);
		if (!TestNotNull(TEXT("live source package exists"), LivePackage)
			|| !TestNotNull(TEXT("live source object exists"), LiveObject))
		{
			return false;
		}

		const FUeAgentCanonicalIdentity Identity(
			TEXT("uai.test.persisted-artifact"),
			TEXT("asset.object.create"),
			PackageName,
			ObjectPath,
			TEXT("asset_data"),
			TEXT("test"));
		const FString MissingPath =
			FPaths::ProjectSavedDir()
			+ TEXT("UAI/Tests/does-not-exist.uasset");
		const TArray<FDurableArtifact> Artifacts = {
			FDurableArtifact(
				TEXT("artifact:test:missing-persisted-package"),
				Identity,
				PackageName,
				MissingPath,
				EAssetDurableIntent::Save,
				TEXT("package_header")),
		};
		FUeAgentPersistedCreatedAssetObservation Observation;
		FUeAgentAssetDataSimpleAuthoringError Error;
		TestFalse(
			TEXT("a still-loaded object cannot satisfy persisted readback"),
			TryObservePersistedCreatedAsset(
				Artifacts,
				PackageName,
				ObjectName,
				ObjectPath,
				UTexture2D::StaticClass()->GetPathName(),
				Observation,
				Error));
		TestEqual(
			TEXT("missing durable artifact is the typed failure"),
			Error.Code,
			FString(TEXT("persisted_package_artifact_invalid")));
		TestTrue(
			TEXT("persisted observation does not unload the live source"),
			IsValid(LiveObject)
				&& LiveObject->GetOutermost() == LivePackage);
		LiveObject->ClearFlags(RF_Public | RF_Standalone);
		LiveObject->MarkAsGarbage();
		LivePackage->MarkAsGarbage();
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUeAgentAssetDataSimpleEffectGenericFactoryCatalogTest,
		"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
			"GenericNativeFactoryCatalog",
		EAutomationTestFlags::EditorContext
			| EAutomationTestFlags::EngineFilter)

	bool FUeAgentAssetDataSimpleEffectGenericFactoryCatalogTest::RunTest(
		const FString& Parameters)
	{
		TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
		FString Error;
		if (!TestTrue(
				TEXT("production profiles are available"),
				TryGetAssetDataSimpleEffectProductionProfiles(
					Profiles,
					Error)))
		{
			AddError(Error);
			return false;
		}
		const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
			Profiles.FindByPredicate(
				[](const FUeAgentAssetDataSimpleEffectProductionProfile& Value)
				{
					return Value.CapabilityId
						== TEXT("asset.object.create");
				});
		if (!TestNotNull(
				TEXT("generic public create profile exists"),
				Profile))
		{
			return false;
		}
		TestTrue(
			TEXT("generic create consumes the native factory catalog"),
			Profile->UsesFactoryCandidate());

		TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe> Source;
		TSharedPtr<
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe> Resolver;
		FUeAgentSimpleAuthoringRuntimeResolverError ResolverError;
		if (!TestTrue(
				TEXT("production factory catalog is available"),
				TryCreateAssetDataSimpleAuthoringFactoryCatalog(
					Source,
					Resolver,
					ResolverError)))
		{
			AddError(ResolverError.Code + TEXT(":") + ResolverError.Detail);
			return false;
		}

		FUeAgentResourceTracker Resources;
		const auto Clock = []() { return FPlatformTime::Seconds(); };
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(10.0, Clock);
		FUeAgentCancellationSource Cancellation(Clock);
		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentDiscoveryError DiscoveryError;
		if (!TestTrue(
				TEXT("generic profile enumerates the live native catalog"),
				Source->TrySearch(
					FUeAgentDiscoveryQuery(
						TEXT("asset_data_factory"),
						Profile->ProfileId,
						EUeAgentDiscoveryMatchMode::Broad,
						EUeAgentMaterializationPolicy::IndexOnly),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					Catalog,
					DiscoveryError)))
		{
			AddError(
				DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			return false;
		}
		if (!TestTrue(
				TEXT("generic catalog is complete and non-empty"),
				Catalog.IsValid()
					&& Catalog->GetCompleteness()
						== EUeAgentCatalogCompleteness::Complete
					&& !Catalog->GetCandidates().IsEmpty()))
		{
			return false;
		}
		TArray<ContentDiscovery::FScopedNameDiscoveryEntry> DiscoveryEntries;
		DiscoveryEntries.Reserve(Catalog->GetCandidates().Num());
		for (const FUeAgentCandidateSummary& Candidate
			: Catalog->GetCandidates())
		{
			ContentDiscovery::FScopedNameDiscoveryEntry Entry;
			Entry.Name = Candidate.GetDisplayName();
			Entry.Scopes = Candidate.GetDiscoveryScopes();
			Entry.Identity = Candidate.GetIdentity().GetCanonicalKey();
			DiscoveryEntries.Add(MoveTemp(Entry));
		}
		TArray<FString> PublicNames;
		FString PublicNameError;
		if (!TestTrue(
				TEXT("generic catalog has readable resolvable public names"),
				ContentDiscovery::TryBuildScopedDiscoveryNames(
					DiscoveryEntries,
					ContentDiscovery::ENameComparison::IgnoreCase,
					PublicNames,
					PublicNameError)))
		{
			AddError(PublicNameError);
			return false;
		}
		const auto PublicNameFor = [&Catalog, &PublicNames](
			const FUeAgentCandidateSummary* Candidate)
		{
			for (int32 Index = 0; Index < Catalog->GetCandidates().Num(); ++Index)
			{
				if (&Catalog->GetCandidates()[Index] == Candidate
					&& PublicNames.IsValidIndex(Index))
				{
					return PublicNames[Index];
				}
			}
			return FString();
		};

		const FUeAgentCandidateSummary* World =
			Catalog->GetCandidates().FindByPredicate(
				[](const FUeAgentCandidateSummary& Candidate)
				{
					return Candidate.GetDiscoveryScopes().Contains(
							TEXT("World Factory"))
						&& Candidate.GetSummary()
							== TEXT("/Script/Engine.World");
				});
		const FUeAgentCandidateSummary* Material =
			Catalog->GetCandidates().FindByPredicate(
				[](const FUeAgentCandidateSummary& Candidate)
				{
					return Candidate.GetDiscoveryScopes().Contains(
							TEXT("Material Factory New"))
						&& Candidate.GetSummary()
							== TEXT("/Script/Engine.Material");
				});
		if (!TestNotNull(TEXT("UWorldFactory is discoverable"), World)
			|| !TestNotNull(
				TEXT("one non-World default factory is discoverable"),
				Material))
		{
			return false;
		}

		TSharedPtr<const FUeAgentCandidateBinding> WorldBinding;
		DiscoveryError = {};
		if (!TestTrue(
				TEXT("World candidate resolves from the same revision"),
				Source->TryResolveExact(
					World->GetIdentity().GetCanonicalKey(),
					Catalog->GetRevision(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					WorldBinding,
					DiscoveryError)))
		{
			AddError(
				DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			return false;
		}
		TestTrue(
			TEXT("World factory is generic-write-ready"),
			WorldBinding.IsValid()
				&& WorldBinding->Supports(
					EUeAgentCandidateOperation::Create));
		TSharedPtr<const FUeAgentCandidateBinding> MaterialBinding;
		DiscoveryError = {};
		if (!TestTrue(
				TEXT("non-World candidate resolves from the same revision"),
				Source->TryResolveExact(
					Material->GetIdentity().GetCanonicalKey(),
					Catalog->GetRevision(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					MaterialBinding,
					DiscoveryError)))
		{
			AddError(
				DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			return false;
		}
		TestTrue(
			TEXT("MaterialFactoryNew proves the shared non-World write seam"),
			MaterialBinding.IsValid()
				&& MaterialBinding->Supports(
					EUeAgentCandidateOperation::Create));

		auto ResolveGenericConstructor = [
			this,
			&Catalog,
			&Source,
			&Resolver,
			&Resources,
			&Deadline,
			&Cancellation,
			&Profile,
			&PublicNameFor](
				const FString& ClassPath,
				const FString& Label,
				FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding)
		{
			FString ExpectedName;
			ClassPath.Split(TEXT("."), nullptr, &ExpectedName,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			const FUeAgentCandidateSummary* Summary =
				Catalog->GetCandidates().FindByPredicate(
					[&ClassPath, &ExpectedName](
						const FUeAgentCandidateSummary& Candidate)
					{
						return Candidate.GetSummary() == ClassPath
							&& Candidate.GetDisplayName() == ExpectedName;
					});
			if (!TestNotNull(*Label, Summary))
			{
				return false;
			}
			TSharedPtr<const FUeAgentCandidateBinding> PublicBinding;
			FUeAgentDiscoveryError LocalDiscoveryError;
			if (!TestTrue(
					*(Label + TEXT(" exact public identity resolves")),
					Source->TryResolveExact(
						Summary->GetIdentity().GetCanonicalKey(),
						Catalog->GetRevision(),
						Resources,
						Deadline,
						Cancellation.GetToken(),
						PublicBinding,
						LocalDiscoveryError)))
			{
				AddError(
					LocalDiscoveryError.Code
						+ TEXT(":")
						+ LocalDiscoveryError.Detail);
				return false;
			}
			FExecutionMutationError LocalMutationError;
			if (!TestTrue(
					*(Label + TEXT(" public ref binds through asset.object.create")),
					Resolver->TryResolveForPlanning(
						PublicNameFor(Summary),
						{},
						Profile->ProfileId,
						OutBinding,
						LocalMutationError)))
			{
				AddError(
					LocalMutationError.GetCode()
						+ TEXT(":")
						+ LocalMutationError.GetDetail());
				return false;
			}
			return TestEqual(
				*(Label + TEXT(" resolves the expected class")),
				OutBinding.GetConcreteClass()->GetPathName(),
				ClassPath);
		};

		FUeAgentAssetDataSimpleEffectFactoryBinding MediaPlayerConstructor;
		FUeAgentAssetDataSimpleEffectFactoryBinding RenderTargetConstructor;
		FUeAgentAssetDataSimpleEffectFactoryBinding MediaSourceConstructor;
		if (!ResolveGenericConstructor(
				TEXT("/Script/MediaAssets.MediaPlayer"),
				TEXT("static MediaPlayer constructor"),
				MediaPlayerConstructor)
			|| !ResolveGenericConstructor(
				TEXT("/Script/Engine.TextureRenderTarget2D"),
				TEXT("static render-target constructor"),
				RenderTargetConstructor)
			|| !ResolveGenericConstructor(
				TEXT("/Script/MediaAssets.FileMediaSource"),
				TEXT("domain media-source constructor"),
				MediaSourceConstructor))
		{
			return false;
		}
		TestEqual(
			TEXT("static constructor uses the shared object mechanic"),
			MediaPlayerConstructor.GetFactoryMechanicId(),
			FString(TEXT("uai.asset_data.factory:new_object:v1")));
		TestEqual(
			TEXT("second static constructor uses the same object mechanic"),
			RenderTargetConstructor.GetFactoryMechanicId(),
			FString(TEXT("uai.asset_data.factory:new_object:v1")));
		TestEqual(
			TEXT("domain constructor is projected through the same public route"),
			MediaSourceConstructor.GetFactoryMechanicId(),
			FString(TEXT("uai.asset_data.factory:new_object:v1")));

		FUeAgentAssetDataSimpleEffectFactoryBinding ResolvedByName;
		FExecutionMutationError MutationError;
		if (!TestTrue(
				TEXT("stable name resolves without a client alias"),
				Resolver->TryResolveForPlanning(
					PublicNameFor(World),
					{},
					Profile->ProfileId,
					ResolvedByName,
					MutationError)))
		{
			AddError(
				MutationError.GetCode()
					+ TEXT(":")
					+ MutationError.GetDetail());
			return false;
		}
		TestTrue(
			TEXT("World binding retains the actual native factory CDO"),
			ResolvedByName.GetFactory() != nullptr
				&& ResolvedByName.GetFactory()->HasAnyFlags(
					RF_ClassDefaultObject));

		FUeAgentAssetDataSimpleEffectFactoryBinding MachineRefRejected;
		MutationError = FExecutionMutationError();
		TestFalse(
			TEXT("machine public refs are not accepted as selectors"),
			Resolver->TryResolveForPlanning(
				WorldBinding->GetPublicRef(),
				{},
				Profile->ProfileId,
				MachineRefRejected,
				MutationError));

		FUeAgentAssetDataSimpleEffectFactoryBinding WrongContextBinding;
		MutationError = FExecutionMutationError();
		TestFalse(
			TEXT("a generic factory id cannot be reused in a typed context"),
			Resolver->TryResolveForPlanning(
				PublicNameFor(World),
				{},
				TEXT("data_object"),
				WrongContextBinding,
				MutationError));
		TestEqual(
			TEXT("wrong-context rejection is typed"),
			MutationError.GetCode(),
			FString(TEXT("asset_data_factory_candidate_wrong_context")));

		const FUeAgentCandidateSummary* InteractiveCandidate = nullptr;
		for (const FUeAgentCandidateSummary& Candidate
			: Catalog->GetCandidates())
		{
			TSharedPtr<const FUeAgentCandidateBinding> CandidateBinding;
			DiscoveryError = {};
			if (Source->TryResolveExact(
					Candidate.GetIdentity().GetCanonicalKey(),
					Catalog->GetRevision(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					CandidateBinding,
					DiscoveryError)
				&& CandidateBinding.IsValid()
				&& !CandidateBinding->Supports(
					EUeAgentCandidateOperation::Create))
			{
				InteractiveCandidate = &Candidate;
				break;
			}
		}
		if (TestNotNull(
				TEXT("catalog retains at least one interactive-only factory"),
				InteractiveCandidate))
		{
			FUeAgentAssetDataSimpleEffectFactoryBinding InteractiveBinding;
			MutationError = FExecutionMutationError();
			TestFalse(
				TEXT("interactive-only factory is rejected before mutation"),
				Resolver->TryResolveForPlanning(
					PublicNameFor(InteractiveCandidate),
					{},
					Profile->ProfileId,
					InteractiveBinding,
					MutationError));
			TestEqual(
				TEXT("interactive rejection is typed"),
				MutationError.GetCode(),
				FString(TEXT("asset_data_factory_interactive_required")));
		}

		return true;
	}
}

#endif
