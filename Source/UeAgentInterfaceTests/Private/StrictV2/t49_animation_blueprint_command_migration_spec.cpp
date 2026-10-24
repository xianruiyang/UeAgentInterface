#include "Misc/AutomationTest.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T49
{
	const TArray<FString>& ExpectedPublicCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.anim_blueprint.layer_interface.asset.create"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedDormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.anim_blueprint.asset.create"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedInternalOnlyCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.anim_blueprint.graph.apply"),
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
	for (const FString& CommandId : ExpectedPublicCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T49 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("animation-blueprint-command-owner")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable animation namespace"),
				*CommandId), Contract->GetPythonNamespace(),
				FString(TEXT("animation")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable generated method"),
				*CommandId), Contract->GetPythonFunction(),
				CommandId.RightChop(10).Replace(TEXT("."), TEXT("_")));
		}
	}
	for (const FString& CommandId : ExpectedDormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered as dormant"),
			*CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
			TestEqual(*FString::Printf(TEXT("%s retains its compatibility owner"),
				*CommandId), Contract->GetPublicOwner(),
				FString(TEXT("animation-blueprint-command-owner")));
		}
		const CommandLifecycle::FCommandLifecyclePolicy* Policy =
			CommandLifecycle::FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s has one central lifecycle policy"),
			*CommandId), Policy);
		if (Policy)
		{
			TestTrue(TEXT("the HJSON import successor is declared"),
				Policy->SuccessorCommandIds.Contains(
					TEXT("content_graph.import_workspace")));
		}
	}
	for (const FString& CommandId : ExpectedInternalOnlyCommands())
	{
		TestNull(*FString::Printf(TEXT("%s is internal only"), *CommandId),
			Registry->FindExact(CommandId));
	}
	TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>
		Profiles;
	if (!TestTrue(TEXT("all internal mechanics profiles remain available"),
		DomainAdapters::AnimationBlueprintCommands::TryGetActivationProfiles(
			Profiles, Error)))
	{
		AddError(Error);
		return false;
	}
	for (const FString& CommandId : ExpectedInternalOnlyCommands())
	{
		TestTrue(*FString::Printf(TEXT("%s retains an internal profile"),
			*CommandId), Profiles.ContainsByPredicate(
				[&CommandId](const auto& Profile)
				{
					return Profile.GetCommandId() == CommandId;
				}));
	}
	return !HasAnyErrors();
}
