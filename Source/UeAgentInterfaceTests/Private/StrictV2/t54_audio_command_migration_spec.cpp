#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T54
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("audio_media.asset.create"),
			TEXT("metasound.graph.compile_settle"),
			TEXT("sound_cue.graph.compile_settle"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T54AudioCommandMigrationTest,
	"UAI.StrictV2.T54.AUDIO.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T54AudioCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T54;
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
			TestEqual(*FString::Printf(TEXT("%s has one T54 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("audio-media-command-owner")));
		}
	}
	const TArray<FString> InternalHjsonOperationIds = {
		TEXT("metasound.graph.default.mutate"),
		TEXT("metasound.graph.interface.mutate"),
		TEXT("metasound.graph.link.mutate"),
		TEXT("metasound.graph.node.mutate"),
		TEXT("sound_cue.graph.node.mutate"),
		TEXT("sound_cue.graph.relation.mutate"),
		TEXT("sound_cue.graph.root.mutate"),
	};
	for (const FString& CommandId : InternalHjsonOperationIds)
	{
		TestNull(*FString::Printf(TEXT("%s is not a public command"), *CommandId),
			Registry->FindExact(CommandId));
	}
	return !HasAnyErrors();
}
