#include "Misc/AutomationTest.h"

#if __has_include("OperationOwners/Sequence/UeAgentSequenceDomainService.h")
#define UAI_T22_SEQUENCE_DOMAIN_AVAILABLE 1
#include "Core/DomainCapability/UeAgentDomainCapability.h"
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
	using namespace UE::UAI::DomainCapability;
	FString CapabilityError;
	TSharedPtr<const FFrozenDomainCapabilitySet> FrozenCapabilities;
	if (!TestTrue(
		TEXT("the process domain capabilities freeze with T22"),
		TryFreezeProcessDomainCapabilities(
			FrozenCapabilities, CapabilityError))
		|| !FrozenCapabilities.IsValid())
	{
		AddError(CapabilityError);
		return false;
	}

	const FString ExpectedDomainId =
		TEXT("timeline-sequence-command-migration");
	TArray<const FDomainCapabilityDeclaration*> SequenceDeclarations;
	for (const TSharedRef<const FDomainCapabilityContribution>& Contribution
		: FrozenCapabilities->GetContributions())
	{
		if (Contribution->GetDomainId() == ExpectedDomainId)
		{
			SequenceDeclarations.Add(&Contribution->GetDeclaration());
		}
	}
	TestEqual(
		TEXT("the frozen Sequence capability contribution is unique"),
		SequenceDeclarations.Num(),
		1);
	if (SequenceDeclarations.Num() == 1)
	{
		const FDomainCapabilityDeclaration& Declaration =
			*SequenceDeclarations[0];
		TestEqual(
			TEXT("the Sequence HJSON capability is supported"),
			Declaration.Hjson.Support,
			EDomainCapabilitySupport::Supported);
		TestEqual(
			TEXT("the Sequence capability owns one provider"),
			Declaration.Hjson.Providers.Num(),
			1);
		TestEqual(
			TEXT("the Sequence capability freezes one HJSON route"),
			Declaration.Hjson.Routes.Num(),
			1);
		TestEqual(
			TEXT("the Sequence initialization capability is supported"),
			Declaration.Initialization.Support,
			EDomainCapabilitySupport::Supported);
		TestEqual(
			TEXT("the Sequence capability freezes one initialization contract"),
			Declaration.Initialization.ContractIds.Num(),
			1);
		if (Declaration.Initialization.ContractIds.Num() == 1)
		{
			TestEqual(
				TEXT("the Sequence root initialization contract is exact"),
				Declaration.Initialization.ContractIds[0],
				FString(TEXT("uai.init.sequence.root/1")));
		}
		if (Declaration.Hjson.Routes.Num() == 1)
		{
			const FDomainHjsonRoute& Route = Declaration.Hjson.Routes[0];
			TestEqual(TEXT("route provider"), Route.ProviderId,
				FString(TEXT("sequence-domain-provider")));
			TestEqual(TEXT("route command"), Route.CommandId,
				FString(TEXT("content_graph.import_workspace")));
			TestEqual(TEXT("route schema"), Route.SchemaId,
				FString(TEXT("uai.sequence.edit/1")));
			TestEqual(TEXT("route target kind"), Route.TargetKind,
				FString(TEXT("ue.level_sequence.asset")));
			TestEqual(TEXT("route root type"), Route.RootTypeIdentity,
				FString(TEXT("/Script/LevelSequence.LevelSequence")));
			TestEqual(TEXT("route root role"), Route.Role,
				EInitializationNodeRole::RootTarget);
			TestEqual(TEXT("route initialization contract"),
				Route.InitializationContractId,
				FString(TEXT("uai.init.sequence.root/1")));
			TestTrue(TEXT("route supports create"), Route.bCreate);
		}
	}

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
		TestTrue(
			TEXT("the Sequence provider explicitly supports authoring-schema discovery"),
			Resolution.GetProvider()->SupportsExplicitCommand(
				TEXT("content_graph.authoring_schema")));
		TestTrue(
			TEXT("the Sequence provider explicitly owns its round-trip export"),
			Resolution.GetProvider()->SupportsExplicitCommand(
				TEXT("content_graph.export_workspace")));
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
