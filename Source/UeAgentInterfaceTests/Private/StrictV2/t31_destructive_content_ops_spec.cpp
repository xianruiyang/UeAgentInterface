#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T31
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("asset.delete"),
			TEXT("asset.fixup_redirectors"),
			TEXT("asset.move"),
			TEXT("asset.object.clone"),
			TEXT("modeling.actor.mesh.replace"),
			TEXT("modeling.asset.material_slot.remove"),
			TEXT("modeling.voxel.bounds.remove"),
			TEXT("skeletal_mesh.remove_morph_target"),
			TEXT("skeletal_mesh.remove_skin_weight_profile"),
			TEXT("world.data_layer.delete"),
			TEXT("world.level_content.delete_scope"),
			TEXT("world.level_streaming.move_actors"),
			TEXT("world.level_streaming.remove"),
			TEXT("world.pcg.cleanup"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T31DestructiveContentInventoryTest,
	"UAI.StrictV2.T31.DESTRUCTIVE.CONTENT.OPS.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T31DestructiveContentInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T31;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T31 owns exactly 14 inventory commands"),
		ExpectedCommands().Num(), 14);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T31 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("destructive-content-owner")));
		}
	}
	return !HasAnyErrors();
}
