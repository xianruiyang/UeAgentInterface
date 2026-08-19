#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T53
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("ai.behavior_tree.asset.create"),
			TEXT("ai.behavior_tree.graph.apply"),
			TEXT("ai.blackboard.asset.create"),
			TEXT("ai.blackboard.schema.apply"),
			TEXT("ai.eqs.asset.create"),
			TEXT("ai.eqs.graph.apply"),
			TEXT("ai.smart_object_definition.apply"),
			TEXT("ai.smart_object_definition.asset.create"),
			TEXT("ai.state_tree.asset.create"),
			TEXT("ai.state_tree.hierarchy.apply"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T53AiCommandMigrationTest,
	"UAI.StrictV2.T53.AI.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T53AiCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T53;
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
	TestEqual(TEXT("T53 owns exactly ten non-HJSON commands"),
		ExpectedCommands().Num(), 10);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T53 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("ai-command-owner")));
		}
	}
	return !HasAnyErrors();
}
