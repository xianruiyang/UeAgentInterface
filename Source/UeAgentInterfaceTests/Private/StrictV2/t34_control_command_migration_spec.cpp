#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T34
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("editor.safe_exit"),
			TEXT("editor.session"),
			TEXT("job.compile_build"),
			TEXT("operation.plan.execute"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T34ControlCommandMigrationTest,
	"UAI.StrictV2.T34.CONTROL.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T34ControlCommandMigrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T34;
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

	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has the T34 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("editor-session-build-command-owner")));
		}
	}

	const auto* Lifecycle = Registry->FindExact(TEXT("operation.status"));
	TestNotNull(TEXT("T32 lifecycle remains active"), Lifecycle);
	if (Lifecycle)
	{
		TestEqual(TEXT("T34 does not absorb lifecycle commands"),
			Lifecycle->GetPublicOwner(),
			FString(TEXT("long-running-session-owner")));
	}
	return !HasAnyErrors();
}
