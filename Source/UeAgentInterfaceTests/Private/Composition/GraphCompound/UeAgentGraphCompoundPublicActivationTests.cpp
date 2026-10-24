#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::Module::CommandRegistration::Active;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundPublicActivationTest,
	"UeAgentInterface.Composition.GraphCompound.PublicActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundPublicActivationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<FUeAgentGraphCompoundActivationProfile>* CachedGraphProfiles = nullptr;
	UE::UAI::Contract::FDiagnostic ProfileError;
	if (!TryGetGraphCompoundActivationProfiles(CachedGraphProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	const auto& Profiles = *CachedGraphProfiles;

	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic Error;
	if (!TestTrue(TEXT("formal active command composition is available"),
			TryComposeProcessActiveCommandComposition(
				Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error.PublicMessage);
		return false;
	}

	TArray<FString> MissingOwners;
	for (const FUeAgentGraphCompoundActivationProfile& Profile : Profiles)
	{
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
	TestEqual(TEXT("all Graph commands enter the formal active authority"),
		MissingOwners.Num(), 0);
	TestTrue(
		TEXT("the exact dispatch proves one matching handler for every enabled active contract"),
		Dispatch.IsValid());

	return !HasAnyErrors();
}
