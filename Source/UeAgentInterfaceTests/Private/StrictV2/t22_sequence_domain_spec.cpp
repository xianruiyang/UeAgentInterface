#include "Misc/AutomationTest.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Sequence/UeAgentSequenceDomainService.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T22SequenceDomainProviderTest,
	"UAI.StrictV2.T22.SEQUENCE.DOMAIN.ProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T22SequenceDomainProviderTest::RunTest(
	const FString& Parameters)
{
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
	const TSharedRef<const FDomainCapabilityContribution>* SequenceContribution =
		FrozenCapabilities->GetContributions().FindByPredicate(
			[&ExpectedDomainId](
				const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == ExpectedDomainId;
			});
	TestNotNull(
		TEXT("the frozen Sequence capability contribution is present"),
		SequenceContribution);
	if (SequenceContribution)
	{
		const FDomainCapabilityDeclaration& Declaration =
			(*SequenceContribution)->GetDeclaration();
		TestEqual(
			TEXT("the Sequence HJSON capability is supported"),
			Declaration.Hjson.Support,
			EDomainCapabilitySupport::Supported);
		TestTrue(
			TEXT("the Sequence capability owns the expected provider"),
			Declaration.Hjson.Providers.ContainsByPredicate(
				[](const TSharedRef<IImportInitializationDomainProvider>& Provider)
				{
					return Provider->GetProviderId()
						== TEXT("sequence-domain-provider");
				}));
		TestEqual(
			TEXT("the Sequence initialization capability is supported"),
			Declaration.Initialization.Support,
			EDomainCapabilitySupport::Supported);
		TestTrue(
			TEXT("the Sequence root initialization contract is present"),
			Declaration.Initialization.ContractIds.Contains(
				TEXT("uai.init.sequence.root/1")));
		const FDomainHjsonRoute* Route =
			Declaration.Hjson.Routes.FindByPredicate(
				[](const FDomainHjsonRoute& Item)
				{
					return Item.CommandId
							== TEXT("content_graph.import_workspace")
						&& Item.SchemaId == TEXT("uai.sequence.edit/1")
						&& Item.RootTypeIdentity
							== TEXT("/Script/LevelSequence.LevelSequence");
				});
		TestNotNull(TEXT("the Sequence HJSON route is present"), Route);
		if (Route)
		{
			TestEqual(TEXT("route provider"), Route->ProviderId,
				FString(TEXT("sequence-domain-provider")));
			TestEqual(TEXT("route command"), Route->CommandId,
				FString(TEXT("content_graph.import_workspace")));
			TestEqual(TEXT("route schema"), Route->SchemaId,
				FString(TEXT("uai.sequence.edit/1")));
			TestEqual(TEXT("route target kind"), Route->TargetKind,
				FString(TEXT("ue.level_sequence.asset")));
			TestEqual(TEXT("route root type"), Route->RootTypeIdentity,
				FString(TEXT("/Script/LevelSequence.LevelSequence")));
			TestEqual(TEXT("route root role"), Route->Role,
				EInitializationNodeRole::RootTarget);
			TestEqual(TEXT("route initialization contract"),
				Route->InitializationContractId,
				FString(TEXT("uai.init.sequence.root/1")));
			TestTrue(TEXT("route supports create"), Route->bCreate);
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
			TEXT("the Sequence provider explicitly owns its round-trip export"),
			Resolution.GetProvider()->SupportsExplicitCommand(
				TEXT("content_graph.export_workspace")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T22SequenceDomainRejectsUnknownTypeTest,
	"UAI.StrictV2.T22.SEQUENCE.DOMAIN.UnknownTypePrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T22SequenceDomainRejectsUnknownTypeTest::RunTest(
	const FString& Parameters)
{
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
}
