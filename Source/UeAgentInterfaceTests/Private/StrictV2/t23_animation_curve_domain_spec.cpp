#include "Misc/AutomationTest.h"

#if __has_include("OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h")
#define UAI_T23_ANIMATION_CURVE_DOMAIN_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h"
#else
#define UAI_T23_ANIMATION_CURVE_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T23AnimationCurveDomainProviderTest,
	"UAI.StrictV2.T23.ANIMATION.CURVE.DOMAIN.ProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T23AnimationCurveDomainProviderTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T23_ANIMATION_CURVE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T23 Animation/Curve domain provider is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process provider registry freezes with T23"),
		TryFreezeProcessImportInitializationDomainProviders(Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("a BlendSpace root resolves the T23 provider"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.animation_curve.edit/1"),
				TEXT("/Game/UAI/StrictV2/T23/BS_T23.BS_T23"),
				TEXT("/Script/Engine.BlendSpace"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestTrue(TEXT("the T23 resolution is valid"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the Animation/Curve provider identity is unique"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("animation-curve-domain-provider")));
		TestEqual(
			TEXT("the root initialization contract is provider-owned"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.animation_curve.blend_space.root/1")));
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T23AnimationCurveDomainRejectsUnknownTypeTest,
	"UAI.StrictV2.T23.ANIMATION.CURVE.DOMAIN.UnknownTypePrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T23AnimationCurveDomainRejectsUnknownTypeTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T23_ANIMATION_CURVE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T23 Animation/Curve prewrite gate is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TryFreezeProcessImportInitializationDomainProviders(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestFalse(
		TEXT("an unregistered Animation-like root fails before execution"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.animation_curve.edit/1"),
				TEXT("/Game/UAI/StrictV2/T23/Unknown.Unknown"),
				TEXT("/Script/CoreUObject.Object"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestFalse(TEXT("the rejected resolution stays invalid"), Resolution.IsValid());
	return true;
#endif
}
