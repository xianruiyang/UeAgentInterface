#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T58
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.ik_probe_candidate.apply"),
			TEXT("animation.subject.morph_weights.update"),
			TEXT("animation.subject.pose.update"),
			TEXT("rig.control.bake.animation"),
			TEXT("rig.control.bake.controls"),
			TEXT("rig.ik.definition.update"),
			TEXT("rig.retarget.assets.generate"),
			TEXT("rig.retarget.definition.update"),
			TEXT("rig.retarget.pose.auto_align"),
			TEXT("rig.retarget.pose.update"),
			TEXT("rig.skeleton.preview_mesh.bind"),
			TEXT("rig.skeleton.socket.update"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T58RigSpatialCommandMigrationTest,
	"UAI.StrictV2.T58.RIG.SPATIAL.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T58RigSpatialCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T58;
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
	TestEqual(TEXT("T58 owns exactly twelve non-HJSON commands"), ExpectedCommands().Num(), 12);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T58 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("rig-spatial-command-owner")));
		}
	}
	return !HasAnyErrors();
}
