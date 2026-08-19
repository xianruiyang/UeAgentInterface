#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T56
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.blend_space.sample"),
			TEXT("animation.montage.create"),
			TEXT("animation.montage.read"),
			TEXT("animation.montage.structure.replace"),
			TEXT("animation.sequence.bone_tracks.replace"),
			TEXT("animation.sequence.curves.update"),
			TEXT("animation.sequence.events.update"),
			TEXT("animation.sequence.metadata.update"),
			TEXT("animation.sequence.pose.sample"),
			TEXT("animation.sequence.read"),
			TEXT("animation.sequence.root_motion.analyze"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T56AnimationCurveCommandMigrationTest,
	"UAI.StrictV2.T56.ANIMATION.CURVE.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T56AnimationCurveCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T56;
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
	TestEqual(TEXT("T56 owns exactly thirteen non-HJSON commands"), ExpectedCommands().Num(), 13);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T56 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("animation-curve-command-owner")));
		}
	}
	return !HasAnyErrors();
}
