#include "Misc/AutomationTest.h"

#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T55
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("timeline.sequence.create"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedDormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("timeline.sequence.read"),
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
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is enabled"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Enabled);
			TestEqual(*FString::Printf(TEXT("%s has one T55 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("timeline-sequence-command-owner")));
		}
	}
	for (const FString& CommandId : ExpectedDormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered as dormant"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
		}
	}
	TestNull(
		TEXT("settings update exits the public registry after Sequence HJSON coverage"),
		Registry->FindExact(TEXT("timeline.sequence.settings.update")));
	TestTrue(
		TEXT("settings update remains an internal Sequence operation kind"),
		DomainAdapters::TimelineSequenceCommands::GetCommandIds().Contains(
			TEXT("timeline.sequence.settings.update")));
	TestFalse(
		TEXT("settings update is absent from the public Sequence command surface"),
		DomainAdapters::TimelineSequenceCommands::GetPublicCommandIds().Contains(
			TEXT("timeline.sequence.settings.update")));
	TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry> InternalContracts;
	FString InternalContractError;
	TestTrue(
		TEXT("the private production contract retains settings update for internal execution"),
		Composition::TimelineSpatial::TryCreateTimelineSpatialProductionCommandContractRegistry(
			InternalContracts, InternalContractError));
	if (!InternalContracts.IsValid())
	{
		AddError(InternalContractError);
	}
	else
	{
		TestNotNull(
			TEXT("settings update has a private canonical contract"),
			InternalContracts->FindExact(TEXT("timeline.sequence.settings.update")));
	}
	return !HasAnyErrors();
}
