#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Editor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Server/UeAgentCommandRegistry.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Composition::DestructiveImport;
	using namespace UE::UAI::Tests::PublicCommand;

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
	TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
	FString Error;
	if (!TestTrue(TEXT("outer production registry is available"),
			TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("active framework has the complete formal command set"),
		Registry->GetContracts().Num(), 329);
	TSet<FString> CommandIds;
	for (const FUeAgentCommandContract& Contract : Registry->GetContracts())
	{
		CommandIds.Add(Contract.GetCommandId());
		TestNull(TEXT("canonical command has no legacy exact co-owner: ")
				+ Contract.GetCommandId(),
			UeAgentCommandRegistry::FindExactCommandInvoker(
				Contract.GetCommandId()));
		TestNull(TEXT("canonical command has no legacy prefix co-owner: ")
				+ Contract.GetCommandId(),
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(
				Contract.GetCommandId()));
	}
	TestEqual(TEXT("formal command IDs are unique"), CommandIds.Num(), 329);
	Error.Reset();
	TestTrue(TEXT("committed manifest has no generated second truth"),
		FUeAgentCommandManifestExporter::TryVerify(
			*Registry, GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(), GetDefaultCommandManifestPath(),
			Error));
	if (!Error.IsEmpty()) AddError(Error);

	for (const FString& LegacyId : RetiredLegacyIds)
	{
		TestNull(TEXT("retired exact ingress is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("retired prefix ingress is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("retired ingress fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}

	FString LegacyServerSource;
	TestTrue(TEXT("legacy server source is inspectable"),
		ReadProjectFile(
			TEXT("Plugins/UeAgentInterface/Source/UeAgentInterface/Private/Server/UeAgentHttpServer.cpp"),
			LegacyServerSource));
	TestFalse(TEXT("legacy /api/exec ingress is not bound"),
		LegacyServerSource.Contains(
			TEXT("BindCheckedRoute(TEXT(\"/api/exec\")"),
			ESearchCase::CaseSensitive));

	FString GeneratedBinding;
	TestTrue(TEXT("generated public SDK binding is inspectable"),
		ReadProjectFile(
			TEXT("UeAgentInterfaceCMD/cli/uai/commands/generated.py"),
			GeneratedBinding));
	for (const TCHAR* Forbidden : {
		TEXT("/api/exec"), TEXT("uai_core"), TEXT("urllib"),
		TEXT("requests."), TEXT("http.client")})
	{
		TestFalse(TEXT("generated binding contains no business transport: ")
				+ FString(Forbidden),
			GeneratedBinding.Contains(Forbidden, ESearchCase::IgnoreCase));
	}
	TestFalse(TEXT("removed Python second owner directory is absent"),
		IFileManager::Get().DirectoryExists(
			*(FPaths::ProjectDir() / TEXT("UeAgentInterfaceCMD/cli/uai_core"))));

	AWorldSettings* const WorldSettings = GEditor
		? GEditor->GetEditorWorldContext().World()->GetWorldSettings() : nullptr;
	if (!TestNotNull(TEXT("editor world settings"), WorldSettings)) return false;
	FScopedPublicCommandComposition Harness(TEXT("active-framework-residual-zero"));
	if (!TestTrue(TEXT("one formal public composition starts"),
			Harness.TryStart(*WorldSettings, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("exactly one business route is bound"),
		Harness.GetRouteHost().Handlers.Num(), 1);
	TestTrue(TEXT("formal composition releases owned route"), Harness.Stop());
	TestEqual(TEXT("no route remains after cleanup"),
		Harness.GetRouteHost().Handlers.Num(), 0);
	TestEqual(TEXT("no instance ownership remains after cleanup"),
		Harness.GetRouteHost().RouteByInstance.Num(), 0);
	return !HasAnyErrors();
}
