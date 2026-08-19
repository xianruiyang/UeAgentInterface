#include "Misc/AutomationTest.h"

#include "BehaviorTree/BehaviorTree.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

#if __has_include("OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h")
#define UAI_T20_AI_BEHAVIOR_DOMAIN_AVAILABLE 1
#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"
#else
#define UAI_T20_AI_BEHAVIOR_DOMAIN_AVAILABLE 0
#endif

#if __has_include("QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h")
#define UAI_T20_AI_BEHAVIOR_CANDIDATE_QUERY_AVAILABLE 1
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#else
#define UAI_T20_AI_BEHAVIOR_CANDIDATE_QUERY_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T20AiBehaviorDomainTest,
	"UAI.StrictV2.T20.AI.BEHAVIOR.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T20AiBehaviorDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T20_AI_BEHAVIOR_DOMAIN_AVAILABLE
	TestTrue(TEXT("T20 AI behavior domain authority is available"), false);
	return false;
#elif !UAI_T20_AI_BEHAVIOR_CANDIDATE_QUERY_AVAILABLE
	TestTrue(TEXT("T20 AI behavior candidate query is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::AiBehavior;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the AI behavior domain provider is created"),
		TryCreateAiBehaviorDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FCase> Cases = {
		{TEXT("/Script/AIModule.BehaviorTree"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.behavior_tree.root/1")},
		{TEXT("/Script/AIModule.BlackboardData"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.blackboard.root/1")},
		{TEXT("/Script/StateTreeModule.StateTree"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.state_tree.root/1")},
		{TEXT("/Script/AIModule.BTNode"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.ai.behavior_node/1")},
		{TEXT("/Script/AIModule.BTService"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.ai.behavior_service/1")},
		{TEXT("/Script/AIModule.BTDecorator"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.ai.behavior_decorator/1")},
		{TEXT("/Script/StateTreeModule.StateTreeNodeBlueprintBase"),
			EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.ai.state_tree_node/1")},
		{TEXT("uai.ai.blackboard_entry"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.ai.blackboard_entry/1")},
		{TEXT("uai.ai.connection"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.ai.connection/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.ai_behavior.edit/1"),
			TEXT("/Game/UAI/StrictV2/T20/Target.Target"),
			Case.Type,
			Case.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the AI initialization surface resolves: %s"), Case.Type),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the AI initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.ai_behavior.edit/1"),
		TEXT("/Game/UAI/StrictV2/T20/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered AI root is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::QueryOwners::AiBehavior;
	UBehaviorTree* CandidateTarget = NewObject<UBehaviorTree>(GetTransientPackage());
	FAiBehaviorCandidateQueryResult CandidateResult;
	FAiBehaviorCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the AI owner resolves real behavior candidates"),
		TryQueryAiBehaviorCandidates(
			*CandidateTarget,
			TEXT("Sequence"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	if (!TestTrue(TEXT("the real AI behavior catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FAiBehaviorCandidate& Candidate = CandidateResult.Candidates[0];
	TestTrue(TEXT("candidate.name is an exact class identity"),
		Candidate.Name.StartsWith(TEXT("/Script/")));
	TestTrue(TEXT("candidate kind identifies node, service, decorator, state node, or key"),
		Candidate.Kind == TEXT("behavior_node")
			|| Candidate.Kind == TEXT("behavior_service")
			|| Candidate.Kind == TEXT("behavior_decorator")
			|| Candidate.Kind == TEXT("state_tree_node")
			|| Candidate.Kind == TEXT("blackboard_key"));
	TestTrue(TEXT("candidate declares create support"),
		Candidate.SupportedOperations.Contains(TEXT("create")));
	TestFalse(TEXT("candidate input contract is present"),
		Candidate.InputContract.IsEmpty());
	TestFalse(TEXT("candidate placement contract is present"),
		Candidate.PlacementContract.IsEmpty());
	TestFalse(TEXT("candidate required context is present"),
		Candidate.RequiredContext.IsEmpty());
	TestTrue(TEXT("candidate binding revision is a sha256 identity"),
		Candidate.BindingRevision.StartsWith(TEXT("sha256:")));
	TestTrue(TEXT("candidate query reports complete bounded results"),
		CandidateResult.bComplete && !CandidateResult.bHasMore);
	return !HasAnyErrors();
#endif
}
