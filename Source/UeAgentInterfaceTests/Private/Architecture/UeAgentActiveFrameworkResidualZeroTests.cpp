#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentActiveCommandManifest.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Module::CommandRegistration::Active;

	const TArray<FString> RetiredLegacyIds{
		TEXT("asset_delete"), TEXT("asset_move"),
		TEXT("asset_fixup_redirectors"), TEXT("texture_reimport"),
		TEXT("sound_wave_reimport"), TEXT("static_mesh_reimport"),
		TEXT("sparse_volume_texture_reimport"),
		TEXT("skeletal_mesh_remove_morph_target"),
		TEXT("skeletal_mesh_remove_skin_weight_profile"),
		TEXT("data_layer_delete_instance"),
		TEXT("level_content_delete_scope"), TEXT("pcg_cleanup"),
		TEXT("level_streaming_move_actors"),
		TEXT("level_streaming_remove"), TEXT("modeling_replace_actor_mesh"),
		TEXT("modeling_remove_material_slot"),
		TEXT("modeling_voxel_grid_remove_bounds"),
		TEXT("asset_import_fbx_animation"),
		TEXT("asset_import_fbx_skeletal_mesh"),
		TEXT("asset_import_geometry_cache"), TEXT("asset_import_texture"),
		TEXT("light_profile_import"), TEXT("sparse_volume_texture_import"),
		TEXT("sound_wave_import"), TEXT("localization_import_po"),
		TEXT("skeletal_mesh_import_skin_weight_profile"),
		TEXT("blueprint_add_component"), TEXT("material_add_expression"),
		TEXT("ik_rig_apply_folder"),
		TEXT("niagara_emitter_add_renderer"),
		TEXT("audio_submix_record_start"),
	};

	const TArray<FString> RemovedLegacySurfaceFiles{
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentCommandRegistry.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentCommandRegistry.cpp"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/Generated/UeAgentCommandRegistry_NodeGraphExact.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/Generated/UeAgentHttpServerCommandMembers.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentHttpServer.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentHttpServer.cpp"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentHttpServerTypes.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Composition/UeAgentQueryServiceComposition.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Composition/UeAgentQueryServiceComposition.cpp"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Composition/ReferenceCommand/UeAgentReferenceCommandHandlers.h"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Composition/ReferenceCommand/UeAgentReferenceCommandHandlers.cpp"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterfaceTests/Private/Composition/UeAgentQueryServiceCompositionTests.cpp"),
		TEXT("Plugins/UeAgentInterface/Source/UeAgentInterfaceTests/Private/StrictV2/t99_final_audit_spec.cpp"),
		TEXT("Plugins/UeAgentInterface/docs/active/t99_final_audit.md"),
		TEXT("Plugins/UeAgentInterface/docs/active/legacy_asset_retirement_manifest.json"),
	};

	bool ReadProjectFile(const FString& Relative, FString& Out)
	{
		return FFileHelper::LoadFileToString(
			Out, *(FPaths::ProjectDir() / Relative));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentActiveFrameworkResidualZeroTest,
	"UeAgentInterface.Architecture.ActiveFrameworkResidualZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentActiveFrameworkResidualZeroTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("one process immutable registry and exact dispatch are available"),
			TryComposeProcessActiveCommandComposition(
				Registry,
				Dispatch,
				Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	TestTrue(
		TEXT("active framework has a non-empty formal command set"),
		!Registry->GetContracts().IsEmpty());
	TSet<FString> CommandIds;
	for (const FUeAgentFrozenCommandContract& Contract :
		Registry->GetContracts())
	{
		CommandIds.Add(Contract.GetCommandId());
	}
	TestEqual(
		TEXT("formal command IDs are unique"),
		CommandIds.Num(),
		Registry->GetContracts().Num());
	TestNull(
		TEXT("retired global reference catalog is absent"),
		Registry->FindExact(TEXT("reference.catalog_search")));
	TestNotNull(
		TEXT("command discovery search is active"),
		Registry->FindExact(TEXT("query.command.search")));
	TestNotNull(
		TEXT("command discovery detail is active"),
		Registry->FindExact(TEXT("query.command.detail")));
	const FUeAgentFrozenCommandContract* RuntimeScenario =
		Registry->FindExact(TEXT("runtime.scenario.execute"));
	TestNotNull(
		TEXT("bounded PIE runtime scenario is active"),
		RuntimeScenario);
	if (RuntimeScenario)
	{
		TestEqual(
			TEXT("runtime scenario has one public owner"),
			RuntimeScenario->GetPublicOwner(),
			FString(TEXT("pie-runtime-debug-command-owner")));
		TestEqual(
			TEXT("runtime scenario keeps the canonical Python namespace"),
			RuntimeScenario->GetPythonNamespace(),
			FString(TEXT("runtime")));
		TestEqual(
			TEXT("runtime scenario keeps the canonical Python function"),
			RuntimeScenario->GetPythonFunction(),
			FString(TEXT("scenario_execute")));
		TestEqual(
			TEXT("runtime scenario exposes one descriptor-backed variant"),
			RuntimeScenario->GetVariants().Num(),
			1);
		if (RuntimeScenario->GetVariants().Num() == 1)
		{
			TestEqual(
				TEXT("runtime scenario variant identity is exact"),
				RuntimeScenario->GetVariants()[0].VariantId,
				FString(TEXT("policy:runtime-scenario:bounded/v1")));
			TestFalse(
				TEXT("runtime scenario variant publishes the activation request schema"),
				RuntimeScenario->GetVariants()[0]
					.RequestSchemaJson.IsEmpty());
		}
	}

	Error.Reset();
	TestTrue(
		TEXT("committed manifest has no generated second truth"),
		FUeAgentActiveCommandManifest::TryVerify(
			Registry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			GetDefaultCommandManifestPath(),
			Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}

	for (const FString& LegacyId : RetiredLegacyIds)
	{
		TestNull(
			TEXT("retired identity is absent from the sole current registry: ")
				+ LegacyId,
			Registry->FindExact(LegacyId));
	}

	FString ModuleSource;
	TestTrue(
		TEXT("module startup source is inspectable"),
		ReadProjectFile(
			TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Module/UeAgentInterfaceModule.cpp"),
			ModuleSource));
	TestTrue(
		TEXT("module obtains the DomainCapability process authority"),
		ModuleSource.Contains(
			TEXT("TryGetOrBuildProcessDomainCapabilityAuthorities")));
	TestTrue(
		TEXT("module owns the transport-neutral command service"),
		ModuleSource.Contains(TEXT("FUeAgentCommandService::TryCreate")));
	TestFalse(
		TEXT("module does not activate the old query composition"),
		ModuleSource.Contains(TEXT("FUeAgentQueryServiceComposition")));

	for (const FString& RelativePath : RemovedLegacySurfaceFiles)
	{
		TestFalse(
			TEXT("legacy compiled public surface is physically absent: ")
				+ RelativePath,
			IFileManager::Get().FileExists(
				*(FPaths::ProjectDir() / RelativePath)));
	}
	TestTrue(
		TEXT("retained internal mechanics has an explicit non-server owner"),
		IFileManager::Get().FileExists(
			*(FPaths::ProjectDir()
				/ TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/LegacyMechanics/UeAgentLegacyMechanics.h"))));
	TestTrue(
		TEXT("retired final-audit evidence bundle is explicitly isolated"),
		IFileManager::Get().FileExists(
			*(FPaths::ProjectDir()
				/ TEXT("Plugins/UeAgentInterface/retired/strict_v2/final-audit/README.md"))));

	FString GeneratedBinding;
	TestTrue(
		TEXT("generated public SDK binding is inspectable"),
		ReadProjectFile(
			TEXT("UeAgentInterfaceCMD/cli/uai/commands/generated.py"),
			GeneratedBinding));
	for (const TCHAR* Forbidden : {
		TEXT("/api/exec"), TEXT("uai_core"), TEXT("urllib"),
		TEXT("requests."), TEXT("http.client")})
	{
		TestFalse(
			TEXT("generated binding contains no business transport: ")
				+ FString(Forbidden),
			GeneratedBinding.Contains(
				Forbidden,
				ESearchCase::IgnoreCase));
	}
	TestFalse(
		TEXT("removed Python second owner directory is absent"),
		IFileManager::Get().DirectoryExists(
			*(FPaths::ProjectDir()
				/ TEXT("UeAgentInterfaceCMD/cli/uai_core"))));
	return !HasAnyErrors();
}
