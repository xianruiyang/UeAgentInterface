#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Server/UeAgentCommandRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldDestructiveBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldDestructive.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldDestructiveBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Registry;
	FString Error;
	TestTrue(TEXT("formal destructive/import registry is created"),
		UE::UAI::Composition::DestructiveImport::
			TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, Error));
	const TArray<FString> CanonicalIds{
		TEXT("world.data_layer.delete"),
		TEXT("world.level_content.delete_scope"),
		TEXT("world.pcg.cleanup"),
		TEXT("world.level_streaming.move_actors"),
		TEXT("world.level_streaming.remove"),
		TEXT("modeling.actor.mesh.replace"),
		TEXT("modeling.asset.material_slot.remove"),
		TEXT("modeling.voxel.bounds.remove"),
	};
	for (const FString& CommandId : CanonicalIds)
	{
		TestNotNull(TEXT("canonical world command contract: ") + CommandId,
			Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr);
	}
	const TArray<FString> LegacyIds{
		TEXT("data_layer_delete_instance"),
		TEXT("level_content_delete_scope"),
		TEXT("pcg_cleanup"),
		TEXT("level_streaming_move_actors"),
		TEXT("level_streaming_remove"),
		TEXT("modeling_replace_actor_mesh"),
		TEXT("modeling_remove_material_slot"),
		TEXT("modeling_voxel_grid_remove_bounds"),
	};
	for (const FString& LegacyId : LegacyIds)
	{
		TestNull(TEXT("legacy exact owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("legacy prefix owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("legacy route fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}
	return !HasAnyErrors();
}
