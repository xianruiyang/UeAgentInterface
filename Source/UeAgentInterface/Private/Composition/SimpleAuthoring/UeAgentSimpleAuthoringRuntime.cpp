#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"

#include "Composition/SimpleAuthoring/Families/AssetData/UeAgentAssetDataSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/Families/Landscape/UeAgentLandscapeSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/Families/World/UeAgentWorldSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Verification/UeAgentVerification.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "WorldPartition/WorldPartition.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	using namespace AssetDurable;
	using namespace BindingGateway::Ticket;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace DomainAdapters::AssetDataSimpleEffect;
	using namespace DomainAdapters::EditorProjectSimpleEffect;
	using namespace DomainAdapters::LandscapeGeometryEffect;
	using namespace DomainAdapters::ModelingGeometryEffect;
	using namespace DomainAdapters::WorldSimpleEffect;
	using namespace Projection;
	using namespace Verification;

	namespace
	{
		void SetError(
			FUeAgentSimpleAuthoringRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		class FUnavailableAbsenceOracle final : public IAbsenceOracle
		{
		public:
			bool TryObserve(
				const StateSemantic::FUeAgentCanonicalIdentity& Identity,
				const TArray<FDurableArtifact>& Artifacts,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FAbsenceObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutError = {};
				OutObservation = FAbsenceObservation(
					Identity,
					false,
					true,
					TEXT("simple-authoring:absence-not-selected"));
				return true;
			}
		};

		class FUnavailableReferenceOracle final : public IReferenceOracle
		{
		public:
			bool TryObserve(
				const StateSemantic::FUeAgentCanonicalIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FReferenceObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutError = {};
				OutObservation = FReferenceObservation(
					Identity,
					{},
					true,
					TEXT("simple-authoring:reference-not-selected"));
				return true;
			}
		};

		FUeAgentProjectionRequest CompactProjection()
		{
			return FUeAgentProjectionRequest(
				FUeAgentProjectionProfile(
					TEXT("compact"),
					false,
					false,
					false),
				FUeAgentPageRequest(),
				TOptional<int64>(),
				{},
				{});
		}
	}

	bool FUeAgentSimpleAuthoringRuntimeConfig::IsValid() const
	{
		return !ProjectInstanceId.TrimStartAndEnd().IsEmpty()
			&& !EditorInstanceId.TrimStartAndEnd().IsEmpty()
			&& !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& RegistryEpoch > 0
			&& TimeoutSeconds > 0.0
			&& MaxFrameBytes > 0
			&& !JournalRoot.TrimStartAndEnd().IsEmpty();
	}

	bool FUeAgentSimpleAuthoringRuntime::TryCreate(
		const FUeAgentSimpleAuthoringRuntimeConfig& Config,
		UWorld& ExplicitWorld,
		TSharedRef<const FUeAgentOperationTicketService> TicketService,
		TSharedRef<const FUeAgentResponseProjector> Projector,
		TSharedPtr<FUeAgentSimpleAuthoringRuntime>& OutRuntime,
		FUeAgentSimpleAuthoringRuntimeError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid()
			|| !IsInGameThread()
			|| !IsValid(&ExplicitWorld)
			|| !ExplicitWorld.PersistentLevel)
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_config_invalid"));
			return false;
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
		FString CompositionError;
		if (!TryCreateSimpleAuthoringDescriptorRegistry(
				Descriptors,
				CompositionError)
			|| !Descriptors.IsValid())
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_descriptors_unavailable"),
				FString(),
				CompositionError);
			return false;
		}

		TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
		if (!TryCreateProductionCommandContractRegistry(
				Registry,
				CompositionError)
			|| !Registry.IsValid())
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_registry_unavailable"),
				FString(),
				CompositionError);
			return false;
		}

		const TSharedRef<FUeAgentWorldSimpleEffectEngineWorldCatalog,
			ESPMode::ThreadSafe> WorldCatalog = MakeShared<
				FUeAgentWorldSimpleEffectEngineWorldCatalog,
				ESPMode::ThreadSafe>();
		const TSharedRef<FUeAgentWorldSimpleEffectEngineWorldCatalog,
			ESPMode::ThreadSafe> PersistedWorldCatalog = MakeShared<
				FUeAgentWorldSimpleEffectEngineWorldCatalog,
				ESPMode::ThreadSafe>();
		TArray<FUeAgentWorldSimpleEffectWorldCandidate> Worlds;
		ExecutionMutation::FExecutionMutationError WorldError;
		if (!WorldCatalog->TryEnumerate(Worlds, WorldError))
		{
			SetError(
				OutError,
				WorldError.GetCode(),
				ExplicitWorld.GetPathName(),
				WorldError.GetDetail());
			return false;
		}
		const FUeAgentWorldSimpleEffectWorldCandidate* WorldCandidate =
			Worlds.FindByPredicate(
				[&ExplicitWorld](
					const FUeAgentWorldSimpleEffectWorldCandidate& Candidate)
				{
					return Candidate.GetWorld() == &ExplicitWorld;
				});
		if (!WorldCandidate || !WorldCandidate->IsValid())
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_world_not_cataloged"),
				ExplicitWorld.GetPathName());
			return false;
		}

		FUeAgentSimpleAuthoringRuntimeResolverError ResolverError;
		TSharedPtr<IUeAgentWorldSimpleEffectReferenceResolver,
			ESPMode::ThreadSafe> WorldReferenceResolver;
		TSharedPtr<IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> LandscapeReferenceResolver;
		TSharedPtr<IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> ModelingReferenceResolver;
		TSharedPtr<IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> MutationAssetResolver;
		TSharedPtr<IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PreStateAssetResolver;
		TSharedPtr<IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PersistedAssetResolver;
		TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe>
			FactoryCatalogSource;
		TSharedPtr<IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryResolver;
		FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers
			EditorProjectResolvers;
		if (!TryCreateWorldSimpleAuthoringReferenceResolver(
				WorldReferenceResolver,
				ResolverError)
			|| !TryCreateLandscapeSimpleAuthoringReferenceResolver(
					LandscapeReferenceResolver,
					ResolverError)
			|| !TryCreateModelingSimpleAuthoringReferenceResolver(
					ModelingReferenceResolver,
					ResolverError)
			|| !TryCreateAssetDataSimpleAuthoringAssetResolver(
					MutationAssetResolver,
					ResolverError)
			|| !TryCreateAssetDataSimpleAuthoringAssetResolver(
					PreStateAssetResolver,
					ResolverError)
			|| !TryCreateAssetDataSimpleAuthoringAssetResolver(
					PersistedAssetResolver,
					ResolverError)
			|| !TryCreateAssetDataSimpleAuthoringFactoryCatalog(
					FactoryCatalogSource,
					FactoryResolver,
					ResolverError)
			|| !TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
					EditorProjectResolvers,
					ResolverError))
		{
			SetError(
				OutError,
				ResolverError.Code,
				ResolverError.SubjectId,
				ResolverError.Detail);
			return false;
		}

		TSharedPtr<IProjectConfigDurableStageSource> ConfigStageSource;
		EditorProject::FUeAgentEditorProjectSimpleAuthoringError
			StageError;
		if (!EditorProject::FUeAgentEditorProjectConfigStageSourceFactory::
				TryCreate(
					EditorProjectResolvers.MutationTargetResolver,
					EditorProjectResolvers.DurableBindingResolver,
					ConfigStageSource,
					StageError)
			|| !ConfigStageSource.IsValid())
		{
			SetError(
				OutError,
				StageError.Code,
				StageError.SubjectId,
				StageError.Detail);
			return false;
		}

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Config.JournalRoot;
		DurableConfig.StableJournalOwnerId =
			Config.ProjectInstanceId
			+ TEXT(":simple-authoring:asset-durable");
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(FPaths::ProjectContentDir());
		DurableConfig.ProjectConfigBackend = MakeShared<
			FProjectConfigAssetDurableBackend,
			ESPMode::ThreadSafe>(
				TArray<FString>{FPaths::ProjectConfigDir()},
				ConfigStageSource.ToSharedRef());
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
				DurableConfig,
				DurableService,
				DurableError)
			|| !DurableService.IsValid())
		{
			SetError(
				OutError,
				DurableError.Code,
				DurableError.SubjectId,
				DurableError.Detail);
			return false;
		}

		const TSharedRef<FUnavailableAbsenceOracle, ESPMode::ThreadSafe>
			AbsenceOracle = MakeShared<
				FUnavailableAbsenceOracle,
				ESPMode::ThreadSafe>();
		const TSharedRef<FUnavailableReferenceOracle, ESPMode::ThreadSafe>
			ReferenceOracle = MakeShared<
				FUnavailableReferenceOracle,
				ESPMode::ThreadSafe>();
		const FUeAgentProjectionRequest Projection = CompactProjection();
		TArray<TSharedRef<IUeAgentSimpleAuthoringCapabilityProvider>>
			Providers;
		Providers.Reserve(5);

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		World::FUeAgentWorldSimpleAuthoringProviderConfig WorldConfig;
		WorldConfig.DescriptorRegistry = Descriptors;
		WorldConfig.WorldCatalog = WorldCatalog;
		WorldConfig.ReferenceResolver = WorldReferenceResolver;
		WorldConfig.DurableService = DurableService;
		WorldConfig.AbsenceOracle = AbsenceOracle;
		WorldConfig.ReferenceOracle = ReferenceOracle;
		WorldConfig.Projection = Projection;
		WorldConfig.OwnerLeaseId = Config.ServerSessionId + TEXT(":world");
		World::FUeAgentWorldSimpleAuthoringError WorldProviderError;
		if (!World::FUeAgentWorldSimpleAuthoringProviderFactory::TryCreate(
				WorldConfig,
				Provider,
				WorldProviderError))
		{
			SetError(OutError, WorldProviderError.Code,
				WorldProviderError.SubjectId, WorldProviderError.Detail);
			return false;
		}
		Providers.Add(Provider.ToSharedRef());

		Landscape::FUeAgentLandscapeSimpleAuthoringProviderConfig
			LandscapeConfig;
		LandscapeConfig.DescriptorRegistry = Descriptors;
		LandscapeConfig.WorldCatalog = WorldCatalog;
		LandscapeConfig.PersistedWorldCatalog = PersistedWorldCatalog;
		LandscapeConfig.ReferenceResolver = LandscapeReferenceResolver;
		LandscapeConfig.PersistedReferenceResolver =
			LandscapeReferenceResolver;
		LandscapeConfig.DurableService = DurableService;
		LandscapeConfig.AbsenceOracle = AbsenceOracle;
		LandscapeConfig.ReferenceOracle = ReferenceOracle;
		LandscapeConfig.Projection = Projection;
		LandscapeConfig.OwnerLeaseId =
			Config.ServerSessionId + TEXT(":landscape");
		Landscape::FUeAgentLandscapeSimpleAuthoringError
			LandscapeProviderError;
		Provider.Reset();
		if (!Landscape::FUeAgentLandscapeSimpleAuthoringProviderFactory::
				TryCreate(
					LandscapeConfig,
					Provider,
					LandscapeProviderError))
		{
			SetError(OutError, LandscapeProviderError.Code,
				LandscapeProviderError.SubjectId,
				LandscapeProviderError.Detail);
			return false;
		}
		Providers.Add(Provider.ToSharedRef());

		Modeling::FUeAgentModelingSimpleAuthoringProviderConfig
			ModelingConfig;
		ModelingConfig.DescriptorRegistry = Descriptors;
		ModelingConfig.WorldCatalog = WorldCatalog;
		ModelingConfig.PersistedWorldCatalog = PersistedWorldCatalog;
		ModelingConfig.ReferenceResolver = ModelingReferenceResolver;
		ModelingConfig.DurableService = DurableService;
		ModelingConfig.AbsenceOracle = AbsenceOracle;
		ModelingConfig.ReferenceOracle = ReferenceOracle;
		ModelingConfig.Projection = Projection;
		ModelingConfig.OwnerLeaseId =
			Config.ServerSessionId + TEXT(":mesh");
		Modeling::FUeAgentModelingSimpleAuthoringError ModelingProviderError;
		Provider.Reset();
		if (!Modeling::FUeAgentModelingSimpleAuthoringProviderFactory::
				TryCreate(
					ModelingConfig,
					Provider,
					ModelingProviderError))
		{
			SetError(OutError, ModelingProviderError.Code,
				ModelingProviderError.SubjectId,
				ModelingProviderError.Detail);
			return false;
		}
		Providers.Add(Provider.ToSharedRef());

		AssetData::FUeAgentAssetDataSimpleAuthoringProviderConfig
			AssetDataConfig;
		AssetDataConfig.DescriptorRegistry = Descriptors;
		AssetDataConfig.MutationAssetResolver = MutationAssetResolver;
		AssetDataConfig.PreStateAssetResolver = PreStateAssetResolver;
		AssetDataConfig.PersistedAssetResolver = PersistedAssetResolver;
		AssetDataConfig.FactoryResolver = FactoryResolver;
		AssetDataConfig.DurableService = DurableService;
		AssetDataConfig.AbsenceOracle = AbsenceOracle;
		AssetDataConfig.ReferenceOracle = ReferenceOracle;
		AssetDataConfig.Projection = Projection;
		AssetDataConfig.OwnerLeaseId =
			Config.ServerSessionId + TEXT(":asset-data");
		AssetData::FUeAgentAssetDataSimpleAuthoringError
			AssetDataProviderError;
		Provider.Reset();
		if (!AssetData::FUeAgentAssetDataSimpleAuthoringProviderFactory::
				TryCreate(
					AssetDataConfig,
					Provider,
					AssetDataProviderError))
		{
			SetError(OutError, AssetDataProviderError.Code,
				AssetDataProviderError.SubjectId,
				AssetDataProviderError.Detail);
			return false;
		}
		Providers.Add(Provider.ToSharedRef());

		EditorProject::FUeAgentEditorProjectSimpleAuthoringProviderConfig
			EditorProjectConfig;
		EditorProjectConfig.DescriptorRegistry = Descriptors;
		EditorProjectConfig.MutationTargetResolver =
			EditorProjectResolvers.MutationTargetResolver;
		EditorProjectConfig.PreStateTargetResolver =
			EditorProjectResolvers.PreStateTargetResolver;
		EditorProjectConfig.PersistedTargetResolver =
			EditorProjectResolvers.PersistedTargetResolver;
		EditorProjectConfig.DurableBindingResolver =
			EditorProjectResolvers.DurableBindingResolver;
		EditorProjectConfig.DurableService = DurableService;
		EditorProjectConfig.AbsenceOracle = AbsenceOracle;
		EditorProjectConfig.ReferenceOracle = ReferenceOracle;
		EditorProjectConfig.Projection = Projection;
		EditorProjectConfig.OwnerLeaseId =
			Config.ServerSessionId + TEXT(":editor-project");
		EditorProject::FUeAgentEditorProjectSimpleAuthoringError
			EditorProjectProviderError;
		Provider.Reset();
		if (!EditorProject::
				FUeAgentEditorProjectSimpleAuthoringProviderFactory::TryCreate(
					EditorProjectConfig,
					Provider,
					EditorProjectProviderError))
		{
			SetError(OutError, EditorProjectProviderError.Code,
				EditorProjectProviderError.SubjectId,
				EditorProjectProviderError.Detail);
			return false;
		}
		Providers.Add(Provider.ToSharedRef());

		FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Descriptors;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			Config.ProjectInstanceId;
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			Config.EditorInstanceId;
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			WorldCandidate->GetWorldType();
		ApplicationConfig.ExecutionContext.WorldIdentity =
			WorldCandidate->GetWorldIdentity();
		ApplicationConfig.ExecutionContext.LevelIdentity =
			WorldCandidate->GetLevelIdentity();
		ApplicationConfig.ExecutionContext.PieInstanceId =
			WorldCandidate->GetPieInstanceId();
		if (UWorldPartition* WorldPartition = ExplicitWorld.GetWorldPartition())
		{
			ApplicationConfig.ExecutionContext.WorldPartitionIdentity =
				WorldPartition->GetPathName();
		}
		ApplicationConfig.ExecutionContext.ContextRevision =
			WorldCandidate->GetContextRevision();
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
		ApplicationConfig.ServerSessionId = Config.ServerSessionId;
		ApplicationConfig.RegistryEpoch = Config.RegistryEpoch;
		ApplicationConfig.TimeoutSeconds = Config.TimeoutSeconds;
		ApplicationConfig.MaxFrameBytes = Config.MaxFrameBytes;

		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!FUeAgentSimpleAuthoringApplication::TryCreate(
				ApplicationConfig,
				GetSimpleAuthoringCommandRoutes(),
				Providers,
				TicketService,
				Projector,
				Application,
				ApplicationError)
			|| !Application.IsValid())
		{
			SetError(OutError, ApplicationError.Code,
				ApplicationError.SubjectId, ApplicationError.Detail);
			return false;
		}

		TArray<TSharedRef<IUeAgentCommandHandler>> Handlers;
		if (!TryCreateSimpleAuthoringCommandHandlers(
				GetSimpleAuthoringCommandRoutes(),
				Application.ToSharedRef(),
				Handlers,
				CompositionError))
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_handlers_unavailable"),
				FString(),
				CompositionError);
			return false;
		}

		TArray<TSharedRef<IUeAgentCatalogSource>> CatalogSources;
		CatalogSources.Add(FactoryCatalogSource.ToSharedRef());
		CatalogSources.Add(EditorProjectResolvers.CatalogSource.ToSharedRef());
		TArray<FString> FamilyIds;
		FamilyIds.Reserve(Providers.Num());
		for (const TSharedRef<IUeAgentSimpleAuthoringCapabilityProvider>& Item
			: Providers)
		{
			FamilyIds.Add(Item->GetFamilyId());
		}
		FamilyIds.Sort();

		TArray<FString> AuthorityScopes;
		auto AppendAuthorityScopes = [&AuthorityScopes](
			const TArray<FString>& FamilyScopes)
		{
			for (const FString& Scope : FamilyScopes)
			{
				AuthorityScopes.AddUnique(Scope);
			}
		};
		AppendAuthorityScopes(GetWorldSimpleEffectAuthorityScopes());
		AppendAuthorityScopes(GetLandscapeGeometryEffectAuthorityScopes());
		AppendAuthorityScopes(GetModelingGeometryEffectAuthorityScopes());
		AppendAuthorityScopes(GetAssetDataSimpleEffectAuthorityScopes());
		TArray<FUeAgentEditorProjectSimpleEffectProductionProfile>
			EditorProjectProfiles;
		if (!TryGetEditorProjectSimpleEffectProductionProfiles(
				EditorProjectProfiles,
				CompositionError))
		{
			SetError(
				OutError,
				TEXT("simple_authoring_runtime_authority_unavailable"),
				TEXT("editor_project"),
				CompositionError);
			return false;
		}
		for (const FUeAgentEditorProjectSimpleEffectProductionProfile& Profile
			: EditorProjectProfiles)
		{
			AppendAuthorityScopes(
				GetEditorProjectSimpleEffectAuthorityScopes(Profile.ProfileId));
		}
		AuthorityScopes.Sort();

		OutRuntime = MakeShareable(new FUeAgentSimpleAuthoringRuntime(
			Registry.ToSharedRef(),
			Application.ToSharedRef(),
			DurableService.ToSharedRef(),
			MoveTemp(Handlers),
			MoveTemp(CatalogSources),
			MoveTemp(FamilyIds),
			MoveTemp(AuthorityScopes)));
		return OutRuntime.IsValid();
	}

	FUeAgentSimpleAuthoringRuntime::FUeAgentSimpleAuthoringRuntime(
		TSharedRef<const FUeAgentCommandContractRegistry> InCommandRegistry,
		TSharedRef<FUeAgentSimpleAuthoringApplication> InApplication,
		TSharedRef<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> InDurableService,
		TArray<TSharedRef<IUeAgentCommandHandler>> InCommandHandlers,
		TArray<TSharedRef<IUeAgentCatalogSource>> InCatalogSources,
		TArray<FString> InFamilyIds,
		TArray<FString> InAuthorityScopes)
		: CommandRegistry(MoveTemp(InCommandRegistry))
		, Application(MoveTemp(InApplication))
		, DurableService(MoveTemp(InDurableService))
		, CommandHandlers(MoveTemp(InCommandHandlers))
		, CatalogSources(MoveTemp(InCatalogSources))
		, FamilyIds(MoveTemp(InFamilyIds))
		, AuthorityScopes(MoveTemp(InAuthorityScopes))
	{
	}

	TSharedRef<const FUeAgentCommandContractRegistry>
	FUeAgentSimpleAuthoringRuntime::GetCommandRegistry() const
	{
		return CommandRegistry;
	}

	TSharedRef<FUeAgentSimpleAuthoringApplication>
	FUeAgentSimpleAuthoringRuntime::GetApplication() const
	{
		return Application;
	}

	TSharedRef<IUeAgentSimpleAuthoringAssetDurableService,
		ESPMode::ThreadSafe>
	FUeAgentSimpleAuthoringRuntime::GetDurableService() const
	{
		return DurableService;
	}

	const TArray<TSharedRef<IUeAgentCommandHandler>>&
	FUeAgentSimpleAuthoringRuntime::GetCommandHandlers() const
	{
		return CommandHandlers;
	}

	const TArray<TSharedRef<IUeAgentCatalogSource>>&
	FUeAgentSimpleAuthoringRuntime::GetCatalogSources() const
	{
		return CatalogSources;
	}

	const TArray<FString>&
	FUeAgentSimpleAuthoringRuntime::GetFamilyIds() const
	{
		return FamilyIds;
	}

	const TArray<FString>&
	FUeAgentSimpleAuthoringRuntime::GetAuthorityScopes() const
	{
		return AuthorityScopes;
	}
}
