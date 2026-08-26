#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Dom/JsonObject.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

#if __has_include("OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h")
#define UAI_T16_ANIMATION_BLUEPRINT_DOMAIN_AVAILABLE 1
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"
#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"
#else
#define UAI_T16_ANIMATION_BLUEPRINT_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T16AnimationBlueprintDomainTest,
	"UAI.StrictV2.T16.ANIMATION.BLUEPRINT.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T16AnimationBlueprintDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T16_ANIMATION_BLUEPRINT_DOMAIN_AVAILABLE
	TestTrue(TEXT("T16 Animation Blueprint domain authority is available"), false);
	return false;
#else
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::AnimationBlueprint;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("the Animation Blueprint provider is created"),
		TryCreateAnimationBlueprintDomainProvider(Provider, WorkspaceError))
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
		{TEXT("/Script/Engine.AnimBlueprint"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.animation_blueprint.asset/1")},
		{TEXT("uai.animation_blueprint.graph"),
			EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.animation_blueprint.graph/1")},
		{TEXT("uai.animation_blueprint.state_machine"),
			EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.animation_blueprint.state_machine/1")},
		{TEXT("uai.animation_blueprint.state"),
			EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.animation_blueprint.state/1")},
		{TEXT("uai.animation_blueprint.transition"),
			EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.animation_blueprint.transition/1")},
		{TEXT("uai.animation_blueprint.class_settings"),
			EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.animation_blueprint.class_settings/1")},
	};
	for (const FContractCase& ContractCase : ContractCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.animation_blueprint.edit/1"),
			TEXT("/Game/UAI/StrictV2/T16/ABP_T16.ABP_T16"),
			ContractCase.Type,
			ContractCase.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("Animation Blueprint surface resolves: %s"),
			ContractCase.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, WorkspaceError));
		TestEqual(TEXT("the initialization contract is exact"), Contract,
			FString(ContractCase.Contract));
	}
	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.animation_blueprint.edit/1"),
		TEXT("/Game/UAI/StrictV2/T16/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered Animation Blueprint root is rejected before write"),
		Provider->Matches(Unsupported));

	const FString CandidateTarget =
		TEXT("/Game/UAI/StrictV2/T16/ABP_T16_CandidateMissing.ABP_T16_CandidateMissing");
	const TSharedPtr<FJsonObject> ClassSettings = MakeShared<FJsonObject>();
	ClassSettings->SetBoolField(TEXT("template"), true);
	ClassSettings->SetStringField(
		TEXT("parent_class"), TEXT("/Script/Engine.AnimInstance"));
	QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidateQueryResult
		ParentCandidates;
	QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidateQueryError
		CandidateError;
	if (!TestTrue(TEXT("the native AnimGraph StateMachine menu is queryable"),
		QueryOwners::AnimationBlueprint::TryQueryAnimationBlueprintCandidates(
			CandidateTarget,
			TEXT("animation_graph"),
			ClassSettings,
			TEXT("State Machine"),
			ParentCandidates,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	TestEqual(TEXT("the future target revision is explicit"),
		ParentCandidates.TargetRevision, FString(TEXT("missing")));
	TestTrue(TEXT("the parent native menu coverage is exact"),
		ParentCandidates.bCoverageComplete);
	FString ParentCandidateId;
	for (const QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidate&
		Candidate : ParentCandidates.Candidates)
	{
		if (Candidate.bSupported)
		{
			ParentCandidateId = Candidate.Id;
			break;
		}
	}
	TestFalse(TEXT("one writable native StateMachine spawner is exposed"),
		ParentCandidateId.IsEmpty());
	QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidateQueryResult
		ExactParentCandidate;
	CandidateError = {};
	if (!ParentCandidateId.IsEmpty())
	{
		TestTrue(TEXT("the compact parent id is stable in the same factory context"),
			QueryOwners::AnimationBlueprint::TryQueryAnimationBlueprintCandidates(
				CandidateTarget,
				TEXT("animation_graph"),
				ClassSettings,
				FString(),
				ExactParentCandidate,
				CandidateError,
				64,
				0,
				ParentCandidateId));
		TestEqual(TEXT("the exact parent id resolves one candidate"),
			ExactParentCandidate.Candidates.Num(), 1);
	}

	QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidateQueryResult
		StateCandidates;
	CandidateError = {};
	if (!TestTrue(TEXT("the native StateMachine graph menu is queryable"),
		QueryOwners::AnimationBlueprint::TryQueryAnimationBlueprintCandidates(
			CandidateTarget,
			TEXT("state_machine_graph"),
			ClassSettings,
			TEXT("State"),
			StateCandidates,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	TestTrue(TEXT("the child schema menu coverage is exact"),
		StateCandidates.bCoverageComplete);
	TestFalse(TEXT("unsupported sibling actions remain truthfully search-only"),
		StateCandidates.bCreationCoverageComplete);
	FString StateCandidateId;
	for (const QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidate&
		Candidate : StateCandidates.Candidates)
	{
		if (Candidate.bSupported)
		{
			StateCandidateId = Candidate.Id;
			break;
		}
	}
	TestFalse(TEXT("one writable native State action is exposed"),
		StateCandidateId.IsEmpty());
	QueryOwners::AnimationBlueprint::FAnimationBlueprintCandidateQueryResult
		ExactStateCandidate;
	CandidateError = {};
	if (!StateCandidateId.IsEmpty())
	{
		TestTrue(TEXT("the compact State id is stable in the same factory context"),
			QueryOwners::AnimationBlueprint::TryQueryAnimationBlueprintCandidates(
				CandidateTarget,
				TEXT("state_machine_graph"),
				ClassSettings,
				FString(),
				ExactStateCandidate,
				CandidateError,
				64,
				0,
				StateCandidateId));
		TestEqual(TEXT("the exact State id resolves one candidate"),
			ExactStateCandidate.Candidates.Num(), 1);
	}

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
	TestNull(TEXT("the replaced Animation Blueprint HJSON command is inactive"),
		Registry->FindExact(TEXT("animation_blueprint.authoring.apply")));
	const auto* CandidateContract = Registry->FindExact(
		TEXT("animation.state_machine.candidates"));
	TestNotNull(TEXT("the Animation Blueprint candidate query is active"),
		CandidateContract);
	if (CandidateContract)
	{
		TestEqual(TEXT("the candidate query has one domain owner"),
			CandidateContract->GetPublicOwner(),
			FString(TEXT("animation-blueprint-domain-owner")));
		TestEqual(TEXT("the candidate query is read-only"),
			CandidateContract->GetSideEffect(),
			CommandContract::EUeAgentCommandSideEffect::None);
	}
	for (const FString& CommandId : {
		FString(TEXT("animation.anim_blueprint.asset.create")),
		FString(TEXT("animation.anim_blueprint.graph.apply")),
		FString(TEXT("animation.anim_blueprint.layer_interface.asset.create"))})
	{
		const auto* NonHjsonContract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains active"), *CommandId),
			NonHjsonContract);
		if (NonHjsonContract)
		{
			TestEqual(TEXT("T49 remains the non-HJSON owner"),
				NonHjsonContract->GetPublicOwner(),
				FString(TEXT("animation-blueprint-command-owner")));
		}
	}
	return !HasAnyErrors();
#endif
}
