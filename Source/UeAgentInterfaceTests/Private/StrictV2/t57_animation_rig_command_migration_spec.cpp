#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/AnimationRigCommands/UeAgentAnimationRigCommandMechanicsBoundary.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T57
{
	const TArray<FString>& ExpectedDormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.control_rig.shape_library.apply"),
			TEXT("animation.skeleton.slots.read"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T57AnimationRigCommandMigrationTest,
	"UAI.StrictV2.T57.ANIMATION.RIG.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T57AnimationRigCommandMigrationTest::RunTest(const FString& Parameters)
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
	TSharedPtr<const Contract::FUeAgentDescriptorRegistry> DescriptorRegistry;
	if (!TestTrue(TEXT("the animation graph descriptor registry builds"),
		Composition::GraphCompound::TryCreatePcgAnimationGraphDescriptorRegistry(
			DescriptorRegistry, Error))
		|| !DescriptorRegistry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const TArray<FString>& PublicCommandIds =
		DomainAdapters::AnimationRigCommands::GetPublicCommandIds();
	for (const FString& CommandId : PublicCommandIds)
	{
		TestTrue(*FString::Printf(TEXT("%s is part of the complete command inventory"),
			*CommandId),
			DomainAdapters::AnimationRigCommands::GetCommandIds().Contains(CommandId));
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId), Contract);
		if (Contract)
		{
			const bool bDormant =
				UE::UAI::Tests::StrictV2::T57::ExpectedDormantCommands().Contains(CommandId);
			TestEqual(*FString::Printf(TEXT("%s lifecycle state"), *CommandId),
				Contract->GetAvailability(),
				bDormant
					? CommandContract::EUeAgentCommandAvailability::Dormant
					: CommandContract::EUeAgentCommandAvailability::Enabled);
			TestEqual(*FString::Printf(TEXT("%s has one T57 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("animation-rig-command-owner")));
			if (CommandId == TEXT("animation.ik_rig.definition.auto_generate"))
			{
				const auto& ModelFields = Contract->GetModelResultFields();
				TestEqual(TEXT("IK auto-generate exposes one model result field"),
					ModelFields.Num(), 1);
				if (ModelFields.Num() == 1)
				{
					TestEqual(TEXT("IK auto-generate exposes the affected target"),
						ModelFields[0].Name, FString(TEXT("target_ref")));
					TestEqual(TEXT("IK auto-generate reads the native root result"),
						ModelFields[0].SourcePath,
						TArray<FString>({TEXT("target_ref")}));
				}
				const auto* Profile = Composition::GraphCompound::
					FindPcgAnimationGraphRouteProfile(CommandId);
				const auto* Descriptor = Profile
					? DescriptorRegistry->FindExact(
						Profile->GetExecutionContract().GetDescriptorIdentity())
					: nullptr;
				TestNotNull(TEXT("IK auto-generate owns an exact native descriptor"),
					Descriptor);
				TSharedPtr<FJsonObject> ResultSchema;
				FString SchemaError;
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				TestTrue(TEXT("IK auto-generate native result schema exposes target_ref"),
					Descriptor
					&& UE::UAI::Contract::Json::TryParseObject(
						Descriptor->GetResultSchema().GetJsonValue(),
						ResultSchema, SchemaError)
					&& ResultSchema.IsValid()
					&& ResultSchema->TryGetObjectField(
						TEXT("properties"), Properties)
					&& Properties && Properties->IsValid()
					&& (*Properties)->HasField(TEXT("target_ref")));
			}
		}
	}
	for (const FString& CommandId
		: DomainAdapters::AnimationRigCommands::GetCommandIds())
	{
		if (!PublicCommandIds.Contains(CommandId))
		{
			TestNull(*FString::Printf(TEXT("%s is internal-only"), *CommandId),
				Registry->FindExact(CommandId));
		}
	}
	return !HasAnyErrors();
}
