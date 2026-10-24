#include "Misc/AutomationTest.h"

// Keep the production seam compile-visible after the header first appears.
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringEditorProjectProductionTestSupport.h"

namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace
		UE::UAI::Composition::SimpleAuthoring::EditorProject;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::Tests::SimpleAuthoring::EditorProject;

	bool TryCreateApplication(
		FAutomationTestBase& Test,
		const TArray<
			FUeAgentEditorProjectSimpleEffectProductionProfile>& Profiles,
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe>& MutationResolver,
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe>& PreStateResolver,
		const TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe>& PersistedResolver,
		const TSharedRef<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe>& DurableBindingResolver,
		const TSharedRef<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe>& Durable,
		TSharedPtr<FUeAgentSimpleAuthoringApplication>& OutApplication)
	{
		OutApplication.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!Test.TestTrue(
				TEXT("Simple Authoring descriptors compose"),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Registry.IsValid())
		{
			Test.AddError(
				TEXT("sa008_editor_project_registry_invalid:")
					+ RegistryError);
			return false;
		}

		FUeAgentEditorProjectSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.MutationTargetResolver = MutationResolver;
		ProviderConfig.PreStateTargetResolver = PreStateResolver;
		ProviderConfig.PersistedTargetResolver = PersistedResolver;
		ProviderConfig.DurableBindingResolver = DurableBindingResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle = MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle = MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa008-editor-project-production");
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		FUeAgentEditorProjectSimpleAuthoringError ProviderError;
		if (!Test.TestTrue(
				TEXT("EditorProject production family composes"),
				FUeAgentEditorProjectSimpleAuthoringProviderFactory::
					TryCreate(
						ProviderConfig,
						Provider,
						ProviderError))
			|| !Provider.IsValid())
		{
			Test.AddError(
				TEXT("sa008_editor_project_provider_invalid:")
					+ ProviderError.Code + TEXT(":")
					+ ProviderError.Detail);
			return false;
		}

		TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
		for (const FUeAgentEditorProjectSimpleEffectProductionProfile&
			Profile : Profiles)
		{
			const FUeAgentSimpleAuthoringCommandRoute* Route =
				FindRoute(Profile.CapabilityId);
			if (!Test.TestNotNull(
					*(Profile.CapabilityId
						+ TEXT(" has one exact route")),
					Route))
			{
				return false;
			}
			Routes.Add(*Route);
		}

		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		if (!Test.TestTrue(
				TEXT("ticket service creates"),
				FUeAgentOperationTicketService::TryCreate(
					TEXT("sa008-editor-project-integrity"),
					60.0,
					[]() { return 100.0; },
					[]()
					{
						return FString(
							TEXT("ticket:sa008-editor-project"));
					},
					TicketService,
					TicketError))
			|| !TicketService.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!Test.TestTrue(
				TEXT("response projector creates"),
				FUeAgentResponseProjector::TryCreate(
					MakeShared<FTokenEstimator>(),
					Projector,
					ProjectionError))
			|| !Projector.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:sa008-editor-project");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-editor-project-worker");
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Editor;
		ApplicationConfig.ExecutionContext.ContextRevision =
			TEXT("project:sa008-editor-project:r1");
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:sa008-editor-project");
		ApplicationConfig.RegistryEpoch = 1;


		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!Test.TestTrue(
				TEXT("application accepts seven EditorProject routes"),
				FUeAgentSimpleAuthoringApplication::TryCreate(
					ApplicationConfig,
					Routes,
					{Provider.ToSharedRef()},
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					OutApplication,
					ApplicationError))
			|| !OutApplication.IsValid())
		{
			Test.AddError(
				TEXT("sa008_editor_project_application_invalid:")
					+ ApplicationError.Code + TEXT(":")
					+ ApplicationError.Detail);
			return false;
		}
		return true;
	}

	FAssetDurableTarget MakeStageTarget(
		const FString& ProfileId,
		const FString& DurableLocation)
	{
		const FString TargetRef = TargetRefForProfile(ProfileId);
		FUeAgentCanonicalIdentity Identity(
				TEXT("uai.editor_project:") + TargetRef,
				TEXT("editor_project.typed_update"),
				TEXT("project:sa008-editor-project"),
				TargetRef,
				ProfileId,
				TEXT("project:sa008-editor-project:r1"));
		if (ProfileId == TEXT("string_table"))
		{
			return FAssetDurableTarget(
				MoveTemp(Identity),
				DurableLocation,
				EPackageHeaderKind::Asset,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup);
		}
		return FAssetDurableTarget(
			MoveTemp(Identity),
			EDurableArtifactKind::ProjectConfig,
			DurableLocation,
			EAssetDurableIntent::Save,
			ERollbackGrade::DurableBackup);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringEditorProjectProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"EditorProjectProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringEditorProjectProductionContractTest::RunTest(
	const FString& Parameters)
{
	static_assert(
		UAI_EDITOR_PROJECT_SIMPLE_AUTHORING_PRODUCTION_V1 == 1,
		"EditorProject Simple Authoring production contract changed");

	TArray<FUeAgentEditorProjectSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT("formal EditorProject catalog projects production profiles"),
			TryGetEditorProjectSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		AddError(
			TEXT("sa008_editor_project_profile_catalog_invalid:")
				+ ProfileError);
		return false;
	}
	FOwnedEditorProjectProductionState Owner;
	if (!TestTrue(TEXT("owned EditorProject state is isolated"), Owner.IsValid()))
	{
		return false;
	}
	const TSharedRef<FOwnedEditorProjectTargetResolver, ESPMode::ThreadSafe>
		MutationResolver = MakeShared<
			FOwnedEditorProjectTargetResolver,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<FOwnedEditorProjectTargetResolver, ESPMode::ThreadSafe>
		PreStateResolver = MakeShared<
			FOwnedEditorProjectTargetResolver,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<FOwnedEditorProjectTargetResolver, ESPMode::ThreadSafe>
		PersistedResolver = MakeShared<
			FOwnedEditorProjectTargetResolver,
			ESPMode::ThreadSafe>(
				Owner,
				TEXT("target:post-persist:r2"));
	const TSharedRef<
		FOwnedEditorProjectDurableBindingResolver,
		ESPMode::ThreadSafe> DurableBindingResolver = MakeShared<
			FOwnedEditorProjectDurableBindingResolver,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<
		FRecordingEditorProjectDurableService,
		ESPMode::ThreadSafe> Durable = MakeShared<
			FRecordingEditorProjectDurableService,
			ESPMode::ThreadSafe>();

	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	if (!TryCreateApplication(
			*this,
			Profiles,
			MutationResolver,
			PreStateResolver,
			PersistedResolver,
			DurableBindingResolver,
			Durable,
			Application))
	{
		return false;
	}

	for (int32 Index = 0; Index < Profiles.Num(); ++Index)
	{
		const FUeAgentEditorProjectSimpleEffectProductionProfile& Profile =
			Profiles[Index];
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Profile.CapabilityId);
		if (!Route)
		{
			return false;
		}
		FString PayloadJson;
		if (!TestTrue(
				*(Profile.ProfileId + TEXT(" payload serializes")),
				SerializeJson(
					BuildPayloadForProfile(Profile.ProfileId),
					PayloadJson)))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-editor-project:")
					+ Profile.ProfileId,
				TEXT("idempotency:sa008-editor-project:")
					+ Profile.ProfileId,
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-editor-project"),
				{
					TEXT("uai:editor-project:write"),
					TEXT("uai:project-config:write"),
					TEXT("uai:asset:write"),
				},
				TEXT("correlation:sa008-editor-project:")
					+ Profile.ProfileId),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				*(Profile.ProfileId
					+ TEXT(" completes verified lifecycle")),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}
		if (!TestTrue(
				*(Profile.ProfileId
					+ TEXT(" mutates exact owned state")),
				Owner.IsApplied(Profile.ProfileId)))
		{
			return false;
		}
	}

	TestEqual(TEXT("one durable prepare per profile"), Durable->PrepareCount, Profiles.Num());
	TestEqual(TEXT("one durable persist per profile"), Durable->PersistCount, Profiles.Num());
	TestEqual(
		TEXT("C9 acknowledgement follows every profile"),
		Durable->AcknowledgeCount,
		Profiles.Num());
	TestTrue(
		TEXT("pre-state uses its own resolver"),
		PreStateResolver->ResolveCalls >= Profiles.Num());
	TestTrue(
		TEXT("C9 uses a different persisted resolver"),
		PersistedResolver->ResolveCalls >= Profiles.Num());
	TestTrue(
		TEXT("native hook resolves targets independently"),
		MutationResolver->ResolveCalls >= Profiles.Num());

	TSharedPtr<IProjectConfigDurableStageSource> StageSource;
	FUeAgentEditorProjectSimpleAuthoringError StageFactoryError;
	if (!TestTrue(
			TEXT("production ProjectConfig stage source composes"),
			FUeAgentEditorProjectConfigStageSourceFactory::TryCreate(
				PersistedResolver,
				DurableBindingResolver,
				StageSource,
				StageFactoryError))
		|| !StageSource.IsValid())
	{
		return false;
	}
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	FAssetDurableError StageError;
	const FString ConfigStagePath = FPaths::Combine(
		Owner.TempRoot,
		TEXT("staged-config-section.ini"));
	TestTrue(
		TEXT("config source writes only the C8D staging path"),
		StageSource->TryWriteStagedArtifact(
			MakeStageTarget(
				TEXT("config_section"),
				Owner.DurableLocationForProfile(TEXT("config_section"))),
			ConfigStagePath,
			*Resources,
			StageError));
	FString ConfigStageText;
	TestTrue(
		TEXT("staged config is readable"),
		FFileHelper::LoadFileToString(ConfigStageText, *ConfigStagePath));
	TestTrue(
		TEXT("staged config contains current in-memory value"),
		ConfigStageText.Contains(TEXT("UAI.TestValue=after")));
	TestFalse(
		TEXT("stage source does not publish the final config path"),
		IFileManager::Get().FileExists(
			*Owner.DurableLocationForProfile(TEXT("config_section"))));

	const FString SettingsStagePath = FPaths::Combine(
		Owner.TempRoot,
		TEXT("staged-project-settings.ini"));
	StageError = FAssetDurableError();
	TestTrue(
		TEXT("settings source serializes to C8D staging"),
		StageSource->TryWriteStagedArtifact(
			MakeStageTarget(
				TEXT("project_settings"),
				Owner.DurableLocationForProfile(TEXT("project_settings"))),
			SettingsStagePath,
			*Resources,
			StageError));
	FString SettingsStageText;
	TestTrue(
		TEXT("staged settings are readable"),
		FFileHelper::LoadFileToString(
			SettingsStageText,
			*SettingsStagePath));
	TestTrue(
		TEXT("staged settings contain the current reflected value"),
		SettingsStageText.Contains(TEXT("True")));
	TestFalse(
		TEXT("settings source does not publish the final config path"),
		IFileManager::Get().FileExists(
			*Owner.DurableLocationForProfile(TEXT("project_settings"))));

	StageError = FAssetDurableError();
	TestFalse(
		TEXT("StringTable cannot enter ProjectConfig staging"),
		StageSource->TryWriteStagedArtifact(
			MakeStageTarget(
				TEXT("string_table"),
				Owner.DurableLocationForProfile(TEXT("string_table"))),
			FPaths::Combine(Owner.TempRoot, TEXT("invalid.ini")),
			*Resources,
			StageError));
	TestEqual(
		TEXT("StringTable rejection is stable"),
		StageError.GetCode(),
		FString(TEXT("editor_project_config_stage_target_kind_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringEditorProjectOwnedConfigTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"EditorProjectOwnedConfig",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringEditorProjectOwnedConfigTest::RunTest(
	const FString& Parameters)
{
	TArray<FUeAgentEditorProjectSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT("formal EditorProject catalog projects E4 profiles"),
			TryGetEditorProjectSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}

	FString OwnedRoot;
	{
		FOwnedEditorProjectProductionState Owner;
		OwnedRoot = Owner.TempRoot;
		if (!TestTrue(TEXT("owned EditorProject E4 state is valid"), Owner.IsValid()))
		{
			return false;
		}
		const FString ProfileId = TEXT("config_section");
		const FString FinalPath = Owner.DurableLocationForProfile(ProfileId);
		TestFalse(
			TEXT("owned config final path starts absent"),
			IFileManager::Get().FileExists(*FinalPath));

		const TSharedRef<FOwnedEditorProjectTargetResolver, ESPMode::ThreadSafe>
			MutationResolver = MakeShared<
				FOwnedEditorProjectTargetResolver,
				ESPMode::ThreadSafe>(Owner);
		const TSharedRef<FOwnedEditorProjectTargetResolver, ESPMode::ThreadSafe>
			PreStateResolver = MakeShared<
				FOwnedEditorProjectTargetResolver,
				ESPMode::ThreadSafe>(Owner);
		const TSharedRef<
			FReloadedEditorProjectConfigTargetResolver,
			ESPMode::ThreadSafe> PersistedResolver = MakeShared<
				FReloadedEditorProjectConfigTargetResolver,
				ESPMode::ThreadSafe>(Owner);
		const TSharedRef<
			FOwnedEditorProjectDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver = MakeShared<
				FOwnedEditorProjectDurableBindingResolver,
				ESPMode::ThreadSafe>(Owner);

		TSharedPtr<IProjectConfigDurableStageSource> StageSource;
		FUeAgentEditorProjectSimpleAuthoringError StageError;
		if (!TestTrue(
				TEXT("EditorProject E4 stage source composes"),
				FUeAgentEditorProjectConfigStageSourceFactory::TryCreate(
					MutationResolver,
					DurableBindingResolver,
					StageSource,
					StageError))
			|| !StageSource.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = FPaths::Combine(
			Owner.TempRoot,
			TEXT("Journal"));
		DurableConfig.StableJournalOwnerId =
			TEXT("test:editor-project:durable");
		DurableConfig.ProjectConfigBackend = MakeShared<
			FProjectConfigAssetDurableBackend,
			ESPMode::ThreadSafe>(
				TArray<FString>{Owner.TempRoot},
				StageSource.ToSharedRef());
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService> Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real ProjectConfig C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_editor_project_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		if (!TryCreateApplication(
				*this,
				Profiles,
				MutationResolver,
				PreStateResolver,
				PersistedResolver,
				DurableBindingResolver,
				Durable.ToSharedRef(),
				Application))
		{
			return false;
		}
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(TEXT("editor_project.config_section.update"));
		if (!TestNotNull(
				TEXT("config section has one exact E4 route"),
				Route))
		{
			return false;
		}
		FString PayloadJson;
		if (!TestTrue(
				TEXT("EditorProject E4 payload serializes"),
				SerializeJson(
					BuildPayloadForProfile(ProfileId),
					PayloadJson)))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-editor-project-e4"),
				TEXT("idempotency:sa008-editor-project-e4"),
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-editor-project-e4"),
				{
					TEXT("uai:editor-project:write"),
					TEXT("uai:project-config:write"),
				},
				TEXT("correlation:sa008-editor-project-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("EditorProject E4 completes after reparsed config readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		TestTrue(
			TEXT("ProjectConfig backend publishes the owned config"),
			IFileManager::Get().FileExists(*FinalPath));
		TestTrue(
			TEXT("C9 reparses through a dedicated resolver"),
			PersistedResolver->ReloadCount >= 1);
		TestEqual(
			TEXT("reparsed config preserves the exact mutation"),
			PersistedResolver->ReadPersistedValue(),
			FString(TEXT("after")));

		TArray<FString> CompletedJournals;
		IFileManager::Get().FindFiles(
			CompletedJournals,
			*(DurableConfig.JournalRoot / TEXT("completed/*.json")),
			true,
			false);
		TArray<FString> ActiveTransactions;
		IFileManager::Get().FindFiles(
			ActiveTransactions,
			*(DurableConfig.JournalRoot / TEXT("active/*")),
			false,
			true);
		TestEqual(
			TEXT("verified ProjectConfig journal archives once"),
			CompletedJournals.Num(),
			1);
		TestEqual(
			TEXT("verified ProjectConfig has no active transaction"),
			ActiveTransactions.Num(),
			0);
	}

	TestFalse(
		TEXT("EditorProject E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return true;
}
