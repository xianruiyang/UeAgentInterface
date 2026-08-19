#include "Misc/AutomationTest.h"

#if __has_include("OperationOwners/Sequence/UeAgentSequenceDomainService.h")
#define UAI_T22_SEQUENCE_DOMAIN_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Sequence/UeAgentSequenceDomainService.h"
#else
#define UAI_T22_SEQUENCE_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T22SequenceDomainProviderTest,
	"UAI.StrictV2.T22.SEQUENCE.DOMAIN.ProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T22SequenceDomainProviderTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T22_SEQUENCE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T22 Sequence domain provider is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process provider registry freezes with T22"),
		TryFreezeProcessImportInitializationDomainProviders(Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("a Level Sequence root resolves the T22 provider"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.sequence.edit/1"),
				TEXT("/Game/UAI/StrictV2/T22/LS_T22.LS_T22"),
				TEXT("/Script/LevelSequence.LevelSequence"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestTrue(TEXT("the T22 resolution is valid"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the Sequence provider identity is unique"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("sequence-domain-provider")));
		TestEqual(
			TEXT("the root initialization contract is provider-owned"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.sequence.root/1")));
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T22SequenceDomainRejectsUnknownTypeTest,
	"UAI.StrictV2.T22.SEQUENCE.DOMAIN.UnknownTypePrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T22SequenceDomainRejectsUnknownTypeTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T22_SEQUENCE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T22 Sequence domain prewrite gate is available"), false);
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
		TEXT("an unregistered Sequence-like root fails before execution"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.sequence.edit/1"),
				TEXT("/Game/UAI/StrictV2/T22/Unknown.Unknown"),
				TEXT("/Script/CoreUObject.Object"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestFalse(TEXT("the rejected resolution stays invalid"), Resolution.IsValid());
	return true;
#endif
}
