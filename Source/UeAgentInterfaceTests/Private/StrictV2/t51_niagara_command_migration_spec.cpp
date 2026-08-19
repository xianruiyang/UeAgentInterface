#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T51
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("niagara.asset.duplicate"),
			TEXT("niagara.compile_refresh_settle"),
			TEXT("niagara.compound.apply"),
			TEXT("niagara.emitter.asset.create"),
			TEXT("niagara.renderer_event_parameter.apply"),
			TEXT("niagara.stack.issue_fix"),
			TEXT("niagara.stack.reorder"),
			TEXT("niagara.system.asset.create"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T51NiagaraCommandMigrationTest,
	"UAI.StrictV2.T51.NIAGARA.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T51NiagaraCommandMigrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T51;
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
	TestEqual(TEXT("T51 activates exactly eight non-HJSON commands"),
		ExpectedCommands().Num(), 8);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T51 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("niagara-command-owner")));
		}
	}
	TestNull(TEXT("the replaced Niagara script HJSON command is inactive"),
		Registry->FindExact(TEXT("niagara.script.graph.apply")));
	TestNull(TEXT("the replaced Niagara stack HJSON command is inactive"),
		Registry->FindExact(TEXT("niagara.stack.apply")));
	return !HasAnyErrors();
}
