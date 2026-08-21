#include "Misc/AutomationTest.h"

#include "ControlRigBlueprint.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

#if __has_include("OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h")
#define UAI_T24_RIG_DOMAIN_AVAILABLE 1
#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"
#else
#define UAI_T24_RIG_DOMAIN_AVAILABLE 0
#endif

#if __has_include("QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h")
#define UAI_T24_RIG_CANDIDATES_AVAILABLE 1
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#else
#define UAI_T24_RIG_CANDIDATES_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T24AnimationRigGraphDomainTest,
	"UAI.StrictV2.T24.ANIMATION.RIG.GRAPH.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T24AnimationRigGraphDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T24_RIG_DOMAIN_AVAILABLE
	TestTrue(TEXT("T24 AnimationRigGraph domain authority is available"), false);
	return false;
#elif !UAI_T24_RIG_CANDIDATES_AVAILABLE
	TestTrue(TEXT("T24 AnimationRigGraph candidates are available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::AnimationRigGraph;
	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("the AnimationRigGraph provider is created"),
		TryCreateAnimationRigGraphDomainProvider(Provider, Error))
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
		{TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.rig.control_rig.root/1")},
		{TEXT("/Script/IKRig.IKRigDefinition"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.rig.ik_rig.root/1")},
		{TEXT("/Script/IKRig.IKRetargeter"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.rig.ik_retargeter.root/1")},
		{TEXT("/Script/Engine.Skeleton"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.rig.skeleton.root/1")},
		{TEXT("uai.rig.control"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.rig.control/1")},
		{TEXT("uai.rig.bone"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.rig.bone/1")},
		{TEXT("uai.rig.constraint"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.rig.constraint/1")},
		{TEXT("uai.rig.space"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.rig.space/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.animation_rig_graph.edit/1"),
			TEXT("/Game/UAI/StrictV2/T24/Target.Target"),
			Case.Type, Case.Role, TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("the rig init surface resolves: %s"),
			Case.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the rig initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.animation_rig_graph.edit/1"),
		TEXT("/Game/UAI/StrictV2/T24/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"),
		EInitializationNodeRole::RootTarget, TEXT("{}"));
	TestFalse(TEXT("an unregistered rig root is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::QueryOwners::AnimationRigGraph;
	UControlRigBlueprint* Target =
		NewObject<UControlRigBlueprint>(GetTransientPackage());
	FAnimationRigGraphCandidateQueryResult Result;
	FAnimationRigGraphCandidateQueryError QueryError;
	if (!TestTrue(TEXT("the rig owner resolves native unit structs"),
		TryQueryAnimationRigGraphCandidates(
			*Target, TEXT("Transform"), Result, QueryError)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	TestTrue(TEXT("the native rig unit catalog is non-empty"),
		!Result.Candidates.IsEmpty());
	for (const FAnimationRigGraphCandidate& Candidate : Result.Candidates)
	{
		TestEqual(TEXT("candidate kind is rig_unit_struct"), Candidate.Kind,
			FString(TEXT("rig_unit_struct")));
		TestEqual(TEXT("candidate is bound to the queried target"),
			Candidate.TargetRef, Result.Target);
		TestEqual(TEXT("candidate carries the queried target revision"),
			Candidate.TargetRevision, Result.TargetRevision);
		TestTrue(TEXT("candidate binding revision is sha256"),
			Candidate.BindingRevision.StartsWith(TEXT("sha256:")));
		TestEqual(TEXT("candidate declares native graph placement"),
			Candidate.PlacementContract,
			FString(TEXT("uai.rig.graph-placement/1")));
		TestEqual(TEXT("candidate declares direct-use node context"),
			Candidate.RequiredContext,
			FString(TEXT("target_ref:node_id:position")));
		TestTrue(TEXT("candidate binding is current and directly usable"),
			IsCurrentAnimationRigGraphCandidateBinding(
				*Target,
				Candidate.TargetRef,
				Candidate.TargetRevision,
				Candidate.Name,
				Candidate.Kind,
				Candidate.SupportedOperations,
				Candidate.InputContract,
				Candidate.PlacementContract,
				Candidate.RequiredContext,
				Candidate.BindingRevision,
				QueryError));
	}
	TestEqual(TEXT("complete is the inverse of has_more"),
		Result.bComplete, !Result.bHasMore);
	return !HasAnyErrors();
#endif
}
