#include "Misc/AutomationTest.h"

#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T53AiCommandMigrationTest,
	"UAI.StrictV2.T53.AI.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T53AiCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
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
	const TArray<FString>& PublicCommandIds =
		DomainAdapters::AiCommands::GetPublicCommandIds();
	for (const FString& CommandId : PublicCommandIds)
	{
		TestTrue(*FString::Printf(TEXT("%s is part of the complete command inventory"),
			*CommandId),
			DomainAdapters::AiCommands::GetCommandIds().Contains(CommandId));
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T53 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("ai-command-owner")));
		}
	}
	for (const FString& CommandId : DomainAdapters::AiCommands::GetCommandIds())
	{
		if (!PublicCommandIds.Contains(CommandId))
		{
			TestNull(*FString::Printf(TEXT("%s is internal-only"), *CommandId),
				Registry->FindExact(CommandId));
		}
	}
	return !HasAnyErrors();
}
