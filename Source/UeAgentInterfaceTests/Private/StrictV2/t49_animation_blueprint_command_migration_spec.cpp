#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T49
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.anim_blueprint.asset.create"),
			TEXT("animation.anim_blueprint.graph.apply"),
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T49AnimationBlueprintCommandMigrationTest,
	"UAI.StrictV2.T49.ANIMATION.BLUEPRINT.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T49AnimationBlueprintCommandMigrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T49;
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
	TestEqual(TEXT("T49 owns exactly three non-HJSON commands"),
		ExpectedCommands().Num(), 3);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T49 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("animation-blueprint-command-owner")));
		}
	}
	return !HasAnyErrors();
}
