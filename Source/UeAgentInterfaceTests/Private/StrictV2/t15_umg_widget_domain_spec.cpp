#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

#if __has_include("OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h")
#define UAI_T15_UMG_WIDGET_DOMAIN_AVAILABLE 1
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"
#else
#define UAI_T15_UMG_WIDGET_DOMAIN_AVAILABLE 0
#endif

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T15UmgWidgetDomainTest,
	"UAI.StrictV2.T15.UMG.WIDGET.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T15UmgWidgetDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T15_UMG_WIDGET_DOMAIN_AVAILABLE
	TestTrue(TEXT("T15 UMG/Widget domain authority is available"), false);
	return false;
#else
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::UmgWidget;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("the UMG/Widget provider is created"),
		TryCreateUmgWidgetDomainProvider(Provider, WorkspaceError))
		|| !Provider.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}

	struct FContractCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FContractCase> ContractCases = {
		{TEXT("/Script/UMGEditor.WidgetBlueprint"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.umg.widget_blueprint/1")},
		{TEXT("uai.umg.widget"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.umg.widget/1")},
		{TEXT("uai.umg.slot"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.umg.slot/1")},
		{TEXT("uai.umg.binding"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.umg.binding/1")},
		{TEXT("uai.umg.animation"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.umg.animation/1")},
	};
	for (const FContractCase& ContractCase : ContractCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.umg.edit/1"),
			TEXT("/Game/UAI/StrictV2/T15/WBP_T15.WBP_T15"),
			ContractCase.Type,
			ContractCase.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("UMG surface resolves: %s"),
			ContractCase.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, WorkspaceError));
		TestEqual(TEXT("the initialization contract is exact"), Contract,
			FString(ContractCase.Contract));
	}
	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"), TEXT("uai.umg.edit/1"),
		TEXT("/Game/UAI/StrictV2/T15/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered UMG root is rejected before write"),
		Provider->Matches(Unsupported));

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* SharedIngress = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared HJSON ingress is active"), SharedIngress);
	if (SharedIngress)
	{
		TestEqual(TEXT("the shared ingress retains one public owner"),
			SharedIngress->GetPublicOwner(),
			FString(TEXT("uobject-dataasset-recursive-initialization-owner")));
	}
	TestNull(TEXT("the replaced UMG HJSON command is inactive"),
		Registry->FindExact(TEXT("umg.authoring.apply")));
	for (const FString& CommandId : {
		FString(TEXT("umg.asset.create")),
		FString(TEXT("umg.widget.events.discover"))})
	{
		const auto* NonHjsonContract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains active"), *CommandId),
			NonHjsonContract);
		if (NonHjsonContract)
		{
			TestEqual(TEXT("T48 remains the non-HJSON owner"),
				NonHjsonContract->GetPublicOwner(),
				FString(TEXT("umg-command-owner")));
		}
	}

	if (!TestTrue(TEXT("UMG authoring registration succeeds"),
		UeAgentUmgAuthoring::RegisterAuthoring(Error)))
	{
		AddError(Error);
		return false;
	}
	FTargetCompositionContract CompositionContract;
	TestTrue(TEXT("UMG registers its shared export composition contract"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.widget_blueprint.asset"), CompositionContract));
	return !HasAnyErrors();
#endif
}
