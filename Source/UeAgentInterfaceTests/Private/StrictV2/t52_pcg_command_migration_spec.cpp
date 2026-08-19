#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T52
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("pcg.component.apply"),
			TEXT("pcg.component.world_binding.apply"),
			TEXT("pcg.data_asset.apply"),
			TEXT("pcg.data_asset.create_from_level"),
			TEXT("pcg.generated_spatial_data.read"),
			TEXT("pcg.graph.apply"),
			TEXT("pcg.graph_instance.apply"),
			TEXT("pcg.graph_parameters.apply"),
			TEXT("pcg.partition.spatial_state.read"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T52PcgCommandMigrationTest,
	"UAI.StrictV2.T52.PCG.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T52PcgCommandMigrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T52;
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
	TestEqual(TEXT("T52 owns exactly nine non-HJSON commands"),
		ExpectedCommands().Num(), 9);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T52 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("pcg-command-owner")));
		}
	}
	return !HasAnyErrors();
}
