#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Server/UeAgentCommandRegistry.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Module::CommandRegistration::Active;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPublicActivationAndLegacyAbsenceTest,
	"UeAgentInterface.Composition.GraphCompound.PublicActivationAndLegacyAbsence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPublicActivationAndLegacyAbsenceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles =
		GetGraphCompoundActivationProfiles();
	TestEqual(TEXT("all canonical Graph profiles exist"), Profiles.Num(), 77);
	TestEqual(TEXT("Blueprint/Material route owners"),
		GetBlueprintMaterialGraphRouteProfiles().Num(), 19);
	TestEqual(TEXT("PCG/Animation route owners"),
		GetPcgAnimationGraphRouteProfiles().Num(), 43);
	TestEqual(TEXT("Audio/Media route owners"),
		GetAudioMediaGraphRouteProfiles().Num(), 15);

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("formal active command composition is available"),
			TryComposeProcessActiveCommandComposition(
				Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TSet<FString> RetiredHjsonApplyCommands{
		TEXT("blueprint.authoring.apply"),
		TEXT("umg.authoring.apply"),
	};
	TArray<FString> MissingOwners;
	for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
	{
		if (RetiredHjsonApplyCommands.Contains(Profile.GetCommandId()))
		{
			TestNull(
				TEXT("retired domain HJSON apply command stays absent: ")
					+ Profile.GetCommandId(),
				Registry->FindExact(Profile.GetCommandId()));
			continue;
		}
		const FUeAgentFrozenCommandContract* const Contract =
			Registry->FindExact(Profile.GetCommandId());
		if (!Contract)
		{
			MissingOwners.Add(Profile.GetCommandId() + TEXT(":active_registry"));
			continue;
		}
		if (Contract->GetPublicOwner().IsEmpty())
		{
			MissingOwners.Add(Profile.GetCommandId() + TEXT(":public_owner"));
		}
	}
	if (!MissingOwners.IsEmpty())
	{
		const int32 PreviewCount = FMath::Min(MissingOwners.Num(), 8);
		TArray<FString> Preview;
		for (int32 Index = 0; Index < PreviewCount; ++Index)
		{
			Preview.Add(MissingOwners[Index]);
		}
		AddError(FString::Printf(
			TEXT("%d canonical Graph commands lack the formal public owner; first %d: %s"),
			MissingOwners.Num(), PreviewCount,
			*FString::Join(Preview, TEXT(" | "))));
	}
	TestEqual(TEXT("all non-retired Graph commands enter the formal active authority"),
		MissingOwners.Num(), 0);
	TestTrue(
		TEXT("the exact dispatch proves one matching handler for every enabled active contract"),
		Dispatch.IsValid());

	const TArray<FString> RepresentativeRetiredLegacyIds{
		TEXT("blueprint_add_component"),
		TEXT("material_add_expression"),
		TEXT("ik_rig_apply_folder"),
		TEXT("niagara_emitter_add_renderer"),
		TEXT("audio_submix_record_start"),
	};
	for (const FString& LegacyId : RepresentativeRetiredLegacyIds)
	{
		TestNull(TEXT("retired Graph contract is absent: ") + LegacyId,
			Registry->FindExact(LegacyId));
		TestNull(TEXT("retired exact Graph owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("retired prefix Graph owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("retired Graph command fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}

	const TArray<FString> ReplacedNiagaraIds{
		TEXT("niagara.script.graph.apply"),
		TEXT("niagara.stack.apply"),
	};
	for (const FString& CommandId : ReplacedNiagaraIds)
	{
		TestNull(TEXT("replaced Niagara contract is absent: ") + CommandId,
			Registry->FindExact(CommandId));
		TestNull(TEXT("replaced Niagara exact owner is absent: ") + CommandId,
			UeAgentCommandRegistry::FindExactCommandInvoker(CommandId));
		TestNull(TEXT("replaced Niagara prefix owner is absent: ") + CommandId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(CommandId));
	}

	return !HasAnyErrors();
}
