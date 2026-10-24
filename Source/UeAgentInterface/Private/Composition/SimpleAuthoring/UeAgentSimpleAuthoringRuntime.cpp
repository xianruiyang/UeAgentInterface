#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"

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
#include "Dom/JsonValue.h"
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
		void SetError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
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
				{},
				{});
		}
	}

	bool FUeAgentSimpleAuthoringRuntimeConfig::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentSimpleAuthoringRuntimeConfig::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto RequireText = [&](const FString& Value, const FString& Field)
		{
			if (!Value.TrimStartAndEnd().IsEmpty()) return true;
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_config_invalid"),
				FString::Printf(TEXT("Authoring runtime configuration '%s' must not be empty or whitespace."), *Field),
				TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			OutError.ActualValue = MakeShared<FJsonValueString>(Value);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-whitespace string"));
			return false;
		};
		if (!RequireText(ProjectInstanceId, TEXT("project_instance_id"))
			|| !RequireText(EditorInstanceId, TEXT("editor_instance_id"))
			|| !RequireText(ServerSessionId, TEXT("server_session_id")))
			return false;
		if (RegistryEpoch <= 0)
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_config_invalid"),
				TEXT("The authoring registry epoch must be greater than zero."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("registry_epoch");
			OutError.ActualValue = MakeShared<FJsonValueString>(::LexToString(RegistryEpoch));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("> 0"));
			return false;
		}
		return RequireText(JournalRoot, TEXT("journal_root"));
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
		if (!Config.TryValidate(OutError)) return false;
		if (!IsInGameThread())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_config_invalid"),
				TEXT("The authoring runtime must be created on the game thread."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("game_thread");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		if (!IsValid(&ExplicitWorld))
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_config_invalid"),
				TEXT("The supplied authoring world is no longer valid."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("world/valid");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		if (!ExplicitWorld.PersistentLevel)
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_config_invalid"),
				TEXT("The supplied authoring world has no persistent level."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = ExplicitWorld.GetPathName();
			OutError.PropertyPath = TEXT("world/persistent_level");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValuePresent = true;
			return false;
		}

		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
		UE::UAI::Contract::FDiagnostic CompositionError;
		if (!TryCreateSimpleAuthoringDescriptorRegistry(
				Descriptors,
				CompositionError))
		{
			SetError(OutError, CompositionError, FString());
			return false;
		}

		TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
		if (!TryCreateProductionCommandContractRegistry(
				Registry,
				CompositionError))
		{
			SetError(OutError, CompositionError, FString());
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
			OutError = WorldError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = ExplicitWorld.GetPathName();
			return false;
		}
		const FUeAgentWorldSimpleEffectWorldCandidate* WorldCandidate =
			Worlds.FindByPredicate(
				[&ExplicitWorld](
					const FUeAgentWorldSimpleEffectWorldCandidate& Candidate)
				{
					return Candidate.GetWorld() == &ExplicitWorld;
				});
		if (!WorldCandidate)
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_world_not_cataloged"),
				TEXT("The supplied authoring world is absent from the execution world catalog."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = ExplicitWorld.GetPathName();
			OutError.PropertyPath = TEXT("world_candidate");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValuePresent = true;
			return false;
		}
		if (!WorldCandidate->IsValid())
		{
			FString Field;
			FString Expected;
			TSharedPtr<FJsonValue> Actual;
			if (!::IsValid(WorldCandidate->GetWorld()))
			{
				Field = TEXT("world/valid");
				Expected = TEXT("valid world");
				Actual = MakeShared<FJsonValueBoolean>(false);
			}
			else if (WorldCandidate->GetWorldIdentity().IsEmpty())
			{
				Field = TEXT("world_identity");
				Expected = TEXT("non-empty identity");
				Actual = MakeShared<FJsonValueString>(FString());
			}
			else if (WorldCandidate->GetLevelIdentity().IsEmpty())
			{
				Field = TEXT("level_identity");
				Expected = TEXT("non-empty identity");
				Actual = MakeShared<FJsonValueString>(FString());
			}
			else if (WorldCandidate->GetWorldType() == EUeAgentExecutionWorldType::Invalid)
			{
				Field = TEXT("world_type");
				Expected = TEXT("valid execution world type");
				Actual = MakeShared<FJsonValueString>(TEXT("invalid"));
			}
			else if (WorldCandidate->GetContextRevision().IsEmpty())
			{
				Field = TEXT("context_revision");
				Expected = TEXT("non-empty revision");
				Actual = MakeShared<FJsonValueString>(FString());
			}
			else
			{
				Field = TEXT("pie_instance_id");
				Expected = TEXT("instance ID for a PIE world");
			}
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_runtime_world_not_cataloged"),
				FString::Printf(TEXT("The cataloged authoring world requires %s for '%s'."), *Expected, *Field),
				TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = ExplicitWorld.GetPathName();
			OutError.PropertyPath = Field;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
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
			OutError = ResolverError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = ResolverError.SubjectId;
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
			SetError(OutError, MoveTemp(StageError));
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
			OutError = DurableError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = DurableError.SubjectId;
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
			OutError = WorldProviderError;
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
			OutError = LandscapeProviderError;
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
			OutError = ModelingProviderError;
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
			OutError = AssetDataProviderError;
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
			OutError = EditorProjectProviderError;
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
			OutError = ApplicationError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = ApplicationError.SubjectId;
			return false;
		}

		TArray<TSharedRef<IUeAgentCommandHandler>> Handlers;
		if (!TryCreateSimpleAuthoringCommandHandlers(
				GetSimpleAuthoringCommandRoutes(),
				Application.ToSharedRef(),
				Handlers,
				CompositionError))
		{
			SetError(OutError, CompositionError, FString());
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
			SetError(OutError, CompositionError, TEXT("editor_project"));
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
