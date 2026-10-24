#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Components/Button.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "WidgetBlueprint.h"

#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T15UmgWidgetDomainTest,
	"UAI.StrictV2.T15.UMG.WIDGET.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T15UmgWidgetDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::UmgWidget;
	using namespace UE::UAI::QueryOwners::UmgWidget;

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

	UWidgetBlueprint* const CandidateTarget =
		NewObject<UWidgetBlueprint>(GetTransientPackage());
	UButton::StaticClass();
	FUmgWidgetCandidateQueryResult CandidateResult;
	FUmgWidgetCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the target-bound UMG catalog finds Button"),
		TryQueryUmgWidgetCandidates(
			*CandidateTarget,
			TEXT("Button"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FUmgWidgetCandidate* const ButtonCandidate =
		CandidateResult.Candidates.FindByPredicate(
			[](const FUmgWidgetCandidate& Candidate)
			{
				return Candidate.ClassPath == UButton::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("Button has one writable candidate"),
		ButtonCandidate))
	{
		return false;
	}
	TestFalse(TEXT("the Button candidate name is non-empty"),
		ButtonCandidate->Name.IsEmpty());
	TestTrue(TEXT("the public class template is writable"),
		ButtonCandidate->bSupported);
	FUmgWidgetCandidate ResolvedButton;
	TestTrue(TEXT("the exact name re-resolves in the same target context"),
		TryResolveUmgWidgetCandidate(
			*CandidateTarget,
			ButtonCandidate->Name,
			ResolvedButton,
			CandidateError));
	TestEqual(TEXT("the exact name retains the native Button class"),
		ResolvedButton.ClassPath,
		UButton::StaticClass()->GetPathName());
	TestFalse(TEXT("a mismatched name is rejected"),
		TryResolveUmgWidgetCandidate(
			*CandidateTarget,
			TEXT("DefinitelyNotButton"),
			ResolvedButton,
			CandidateError));
	TestEqual(TEXT("the conjunctive mismatch is typed"),
		CandidateError.Code,
		FString(TEXT("candidate_name_not_found")));

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	const auto* SharedIngress = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared HJSON ingress is active"), SharedIngress);
	if (SharedIngress)
	{
		TestEqual(TEXT("the shared ingress retains one public owner"),
			SharedIngress->GetPublicOwner(),
			FString(TEXT("content-graph-workspace-owner")));
	}
	const auto* CandidateContract = Registry->FindExact(
		TEXT("umg.widget.candidates"));
	TestNotNull(TEXT("the compact UMG candidate command is active"),
		CandidateContract);
	if (CandidateContract)
	{
		TestEqual(TEXT("the UMG domain owns candidate discovery"),
			CandidateContract->GetPublicOwner(),
			FString(TEXT("umg-widget-domain-owner")));
	}
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
		AddError(Error.ToPublicDetail());
		return false;
	}
	FTargetCompositionContract CompositionContract;
	TestTrue(TEXT("UMG registers its shared export composition contract"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.widget_blueprint.asset"), CompositionContract));
	return !HasAnyErrors();
}
