#include "Misc/AutomationTest.h"

#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
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
	UControlRigBlueprintFactory* const Factory =
		NewObject<UControlRigBlueprintFactory>();
	Factory->ParentClass = UControlRig::StaticClass();
	UControlRigBlueprint* const Target = Cast<UControlRigBlueprint>(
		Factory->FactoryCreateNew(
			UControlRigBlueprint::StaticClass(),
			GetTransientPackage(),
			TEXT("CR_T24NativeCandidates"),
			RF_Transient | RF_Transactional,
			nullptr,
			GWarn));
	if (!TestNotNull(TEXT("a native Control Rig target is created"), Target))
	{
		return false;
	}
	FAnimationRigGraphCandidateQueryResult Result;
	FAnimationRigGraphCandidateQueryError QueryError;
	if (!TestTrue(TEXT("the rig owner resolves the native action menu"),
		TryQueryAnimationRigGraphCandidates(
			*Target, TEXT("Add Spaces"), Result, QueryError)))
	{
		AddError(QueryError.Code + TEXT(":") + QueryError.Detail);
		return false;
	}
	TestTrue(TEXT("the native RigVM action result is non-empty"),
		!Result.Candidates.IsEmpty());
	TestTrue(TEXT("native menu coverage is explicit and complete"),
		Result.bCoverageComplete && Result.NativeActionCount > 0);
	TestTrue(TEXT("at least one native menu action is executable"),
		Result.ExecutableActionCount > 0);
	TestEqual(TEXT("the coverage scope is the default blank context"),
		Result.CoverageScope,
		FString(TEXT("control_rig_default_graph_blank_context")));
	if (!Result.Candidates.IsEmpty())
	{
		const FAnimationRigGraphCandidate& Candidate = Result.Candidates[0];
		TestEqual(TEXT("candidate kind is rigvm_action"), Candidate.Kind,
			FString(TEXT("rigvm_action")));
		TestFalse(TEXT("candidate id is opaque and present"),
			Candidate.Id.IsEmpty());
		TestFalse(TEXT("candidate native menu name is present"),
			Candidate.Name.IsEmpty());
		TestTrue(TEXT("the selected unit spawner supports native create"),
			Candidate.bSupported
				&& Candidate.SupportedOperations.Contains(TEXT("create")));
		FAnimationRigGraphCandidate Resolved;
		TestTrue(TEXT("the exact id re-resolves in the same native context"),
			TryResolveAnimationRigGraphCandidate(
				*Target, FString(), Candidate.Id, Resolved, QueryError));
		TestEqual(TEXT("the resolved id is unchanged"),
			Resolved.Id, Candidate.Id);
	}
	TestEqual(TEXT("complete is the inverse of has_more"),
		Result.bComplete, !Result.bHasMore);
	return !HasAnyErrors();
#endif
}
