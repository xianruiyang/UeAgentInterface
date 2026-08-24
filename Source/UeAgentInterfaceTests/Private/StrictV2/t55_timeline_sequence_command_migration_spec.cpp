#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractSchema.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T55
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("timeline.sequence.create"),
			TEXT("timeline.sequence.read"),
			TEXT("timeline.sequence.settings.update"),
			TEXT("timeline.sequence.structure.replace"),
			TEXT("timeline.widget_animation.read"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T55TimelineSequenceCommandMigrationTest,
	"UAI.StrictV2.T55.TIMELINE.SEQUENCE.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T55TimelineSequenceCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T55;
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
	TestEqual(TEXT("T55 owns exactly five non-HJSON commands"), ExpectedCommands().Num(), 5);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T55 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("timeline-sequence-command-owner")));
		}
	}
	const auto* SettingsUpdate =
		Registry->FindExact(TEXT("timeline.sequence.settings.update"));
	const UE::UAI::CommandContract::FUeAgentCommandFieldDeclaration* SettingsInput =
		SettingsUpdate
			? SettingsUpdate->GetInputFields().FindByPredicate(
				[](const UE::UAI::CommandContract::FUeAgentCommandFieldDeclaration& Field)
				{
					return Field.Name == TEXT("input");
				})
			: nullptr;
	TestNotNull(
		TEXT("settings update exposes one model-consumable input schema"),
		SettingsInput);
	if (SettingsInput)
	{
		TestTrue(
			TEXT("settings schema exposes only supported mutable settings and revision"),
			SettingsInput->SchemaJson.Contains(TEXT("\"display_rate\""))
				&& SettingsInput->SchemaJson.Contains(TEXT("\"playback_range\""))
				&& SettingsInput->SchemaJson.Contains(TEXT("\"expected_revision\""))
				&& !SettingsInput->SchemaJson.Contains(TEXT("\"tick_resolution\"")));
		TSharedPtr<const UE::UAI::Contract::FUeAgentContractSchema> CompiledSchema;
		UE::UAI::Contract::FUeAgentContractSchemaError SchemaError;
		TestTrue(
			TEXT("settings input schema compiles as ContractSchema v1"),
			UE::UAI::Contract::FUeAgentContractSchema::TryCompile(
				UE::UAI::Contract::FUeAgentContractValue(SettingsInput->SchemaJson),
				CompiledSchema,
				SchemaError));
	}
	return !HasAnyErrors();
}
