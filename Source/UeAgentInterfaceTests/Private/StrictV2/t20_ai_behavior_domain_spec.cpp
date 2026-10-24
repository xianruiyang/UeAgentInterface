#include "Misc/AutomationTest.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "GameplayInteractionSmartObjectBehaviorDefinition.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "Components/StateTreeAIComponentSchema.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"

#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T20AiBehaviorDomainTest,
	"UAI.StrictV2.T20.AI.BEHAVIOR.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T20AiBehaviorDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::ContentDiscovery;
	{
		const TArray<FScopedNameDiscoveryEntry> FocusedEntries = {
			{TEXT("Blocked"), {TEXT("same_scope")}},
			{TEXT("Blocked"), {TEXT("same_scope")}},
			{TEXT("Unique"), {TEXT("behavior_task"), TEXT("native")}},
		};
		TArray<FString> Names;
		FString ScopeError;
		TestTrue(TEXT("a focused AI behavior match ignores unrelated ambiguity"),
			TryBuildScopedDiscoveryNamesForIndices(
				FocusedEntries, {2}, ENameComparison::IgnoreCase, Names, ScopeError));
		TestEqual(TEXT("the focused AI behavior name stays readable"),
			Names.Num() == 1 ? Names[0] : FString(), FString(TEXT("Unique")));
		TestFalse(TEXT("a matched AI behavior ambiguity still fails closed"),
			TryBuildScopedDiscoveryNamesForIndices(
				FocusedEntries, {0}, ENameComparison::IgnoreCase, Names, ScopeError));
		TestEqual(TEXT("the AI behavior ambiguity error is typed"), ScopeError,
			FString(TEXT("name_discovery_semantic_scope_ambiguous")));
	}
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
		{TEXT("/Script/AIModule.EnvQuery"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.eqs.root/1")},
		{TEXT("/Script/StateTreeModule.StateTree"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.state_tree.root/1")},
		{TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"),
			EInitializationNodeRole::RootTarget,
			TEXT("uai.init.ai.smart_object_definition.root/1")},
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
			TEXT("Wait"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	if (!TestTrue(TEXT("the real AI behavior catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FAiBehaviorCandidate* Candidate =
		CandidateResult.Candidates.FindByPredicate(
			[](const FAiBehaviorCandidate& Item)
			{
				return Item.PayloadType
					== UBTTask_Wait::StaticClass()->GetPathName();
			});
	if (!TestNotNull(TEXT("the native menu returns the exact Wait task"),
		Candidate))
	{
		return false;
	}
	TestTrue(TEXT("candidate.name is the stable task semantic name"),
		Candidate->Name == TEXT("Wait"));
	TestFalse(TEXT("candidate.name is not a native object path"),
		Candidate->Name.StartsWith(TEXT("/")));
	TestFalse(TEXT("candidate.name is a readable selector"),
		Candidate->Name.IsEmpty());
	TestTrue(TEXT("candidate kind identifies the native task surface"),
		Candidate->Kind == TEXT("behavior_task"));
	TestTrue(TEXT("candidate declares native create support"),
		Candidate->bSupported
			&& Candidate->SupportedOperations.Contains(TEXT("create")));
	FAiBehaviorCandidate Resolved;
	TestTrue(TEXT("the readable name re-enumerates one exact native action"),
		TryResolveAiBehaviorCandidate(
			*CandidateTarget,
			Candidate->Name,
			FString(),
			Resolved,
			CandidateError)
			&& Resolved.PayloadType
				== UBTTask_Wait::StaticClass()->GetPathName());
	auto ResolvesExactCandidate = [this](
		UObject& Target,
		const FString& Query,
		const FString& ExpectedName,
		const FString& ExpectedKind)
	{
		using namespace UE::UAI::QueryOwners::AiBehavior;
		FAiBehaviorCandidateQueryResult Result;
		FAiBehaviorCandidateQueryError QueryError;
		if (!TestTrue(
				*FString::Printf(TEXT("%s candidate query succeeds"), *ExpectedKind),
				TryQueryAiBehaviorCandidates(
					Target, Query, Result, QueryError)))
		{
			AddError(QueryError.Code + TEXT(":") + QueryError.PublicMessage);
			return false;
		}
		const FAiBehaviorCandidate* Match = Result.Candidates.FindByPredicate(
			[&](const FAiBehaviorCandidate& Item)
			{
				return Item.Name == ExpectedName && Item.Kind == ExpectedKind;
			});
		if (!TestNotNull(
				*FString::Printf(TEXT("%s exact candidate is returned"), *ExpectedKind),
				Match))
		{
			return false;
		}
		FAiBehaviorCandidate ResolvedByName;
		TestTrue(
			*FString::Printf(TEXT("%s resolves by exact name"), *ExpectedKind),
			TryResolveAiBehaviorCandidate(
				Target, Match->Name, FString(), ResolvedByName, QueryError)
				&& ResolvedByName.Name == Match->Name);
		TestTrue(
			*FString::Printf(TEXT("%s binding validates without remapping"), *ExpectedKind),
			ResolvedByName.Id == Match->Id && ResolvedByName.Kind == Match->Kind
				&& ResolvedByName.PayloadType == Match->PayloadType
				&& ResolvedByName.BindingRevision == Match->BindingRevision);
		return true;
	};

	UBlackboardData* BlackboardTarget =
		NewObject<UBlackboardData>(GetTransientPackage());
	auto ReadableTypeName = [](const UObject& Type)
	{
		FString Name = Type.GetName();
		Name.RemoveFromStart(TEXT("BT"), ESearchCase::IgnoreCase);
		return FName::NameToDisplayString(Name, false);
	};
	ResolvesExactCandidate(
		*BlackboardTarget,
		TEXT("BlackboardKeyType_Bool"),
		ReadableTypeName(*UBlackboardKeyType_Bool::StaticClass()),
		TEXT("blackboard_key"));

	UEnvQuery* EqsTarget = NewObject<UEnvQuery>(GetTransientPackage());
	ResolvesExactCandidate(
		*EqsTarget,
		TEXT("Actors Of Class"),
		TEXT("Actors Of Class"),
		TEXT("eqs_generator"));
	USmartObjectDefinition* SmartObjectTarget =
		NewObject<USmartObjectDefinition>(GetTransientPackage());
	ResolvesExactCandidate(
		*SmartObjectTarget,
		TEXT("GameplayInteractionSmartObjectBehaviorDefinition"),
		ReadableTypeName(
			*UGameplayInteractionSmartObjectBehaviorDefinition::StaticClass()),
		TEXT("smart_object_behavior"));
	UStateTree* StateTreeTarget = NewObject<UStateTree>(GetTransientPackage());
	auto* StateTreeData = NewObject<UStateTreeEditorData>(StateTreeTarget);
	StateTreeTarget->EditorData = StateTreeData;
	StateTreeData->Schema = NewObject<UStateTreeAIComponentSchema>(StateTreeData);
	FAiBehaviorCandidateQueryResult StateTreeResult;
	FAiBehaviorCandidateQueryError StateTreeError;
	if (TestTrue(
			TEXT("state tree candidate query returns readable names and aliases"),
			TryQueryAiBehaviorCandidates(
				*StateTreeTarget, FString(), StateTreeResult, StateTreeError)
				&& !StateTreeResult.Candidates.IsEmpty()))
	{
		const FAiBehaviorCandidate& StateTreeCandidate =
			StateTreeResult.Candidates[0];
		FAiBehaviorCandidate StateTreeResolved;
		TestTrue(
			TEXT("state tree readable name resolves the exact candidate"),
			TryResolveAiBehaviorCandidate(
					*StateTreeTarget,
					StateTreeCandidate.Name,
					FString(),
					StateTreeResolved,
					StateTreeError)
				&& StateTreeResolved.Name == StateTreeCandidate.Name);
	}
	return !HasAnyErrors();
}
