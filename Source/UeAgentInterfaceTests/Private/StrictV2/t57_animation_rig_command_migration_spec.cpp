#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T57
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.control_rig.asset.create"),
			TEXT("animation.control_rig.hierarchy_graph.apply"),
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.control_rig.shape_library.asset.create"),
			TEXT("animation.ik_retargeter.asset.create"),
			TEXT("animation.ik_retargeter.definition.apply"),
			TEXT("animation.ik_retargeter.settings.apply"),
			TEXT("animation.ik_rig.asset.create"),
			TEXT("animation.ik_rig.definition.apply"),
			TEXT("animation.ik_rig.definition.auto_generate"),
			TEXT("animation.skeleton.compatibility.apply"),
			TEXT("animation.skeleton.definition.apply"),
			TEXT("animation.skeleton.slots.read"),
			TEXT("animation.skeleton.virtual_bone.apply"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T57AnimationRigCommandMigrationTest,
	"UAI.StrictV2.T57.ANIMATION.RIG.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T57AnimationRigCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T57;
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
	TestEqual(TEXT("T57 owns exactly fourteen non-HJSON commands"), ExpectedCommands().Num(), 14);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T57 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("animation-rig-command-owner")));
		}
	}
	return !HasAnyErrors();
}
