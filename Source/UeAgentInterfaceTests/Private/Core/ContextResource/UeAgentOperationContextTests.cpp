#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Misc/AutomationTest.h"

// Keep the contract source compilable before and after the C1 header exists.
#include "Core/ContextResource/UeAgentOperationContext.h"


namespace
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;

	FUeAgentDescriptorIdentity DescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.context"),
			1,
			TEXT(
				"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
				"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	}

	FUeAgentExecutionContextPolicy ExplicitEditorPolicy()
	{
		return FUeAgentExecutionContextPolicy(
			{
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted,
			},
			{
				EUeAgentExecutionContextFacet::World,
				EUeAgentExecutionContextFacet::Level,
				EUeAgentExecutionContextFacet::Viewport,
				EUeAgentExecutionContextFacet::Selection,
			},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::ReadOnly);
	}

	FUeAgentExecutionContextInput ExplicitEditorInput()
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project-instance");
		Input.EditorInstanceId = TEXT("editor-instance");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.WorldIdentity = TEXT("/Game/Maps/TestMap:EditorWorld");
		Input.LevelIdentity = TEXT("/Game/Maps/TestMap:PersistentLevel");
		Input.ViewportIdentity = TEXT("level-editor:viewport-1");
		Input.SelectionIdentities = {TEXT("/Game/Maps/TestMap.Actor_1")};
		Input.ContextRevision = TEXT("ctx-rev-7");
		return Input;
	}

	FUeAgentResourceTrackerOwner ResourceTracker()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::Nodes, 3},
				{EUeAgentResourceDimension::WorkUnits, 4},
			}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationContextBindingAndOwnershipTest,
	"UeAgentInterface.Core.ContextResource.OperationContext.BindingAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationContextBindingAndOwnershipTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;

	const FUeAgentDescriptorIdentity Identity = DescriptorIdentity();
	const FUeAgentQueryRequest Request(
		Identity,
		TEXT("request-1"),
		FUeAgentContractValue(TEXT("{\"query\":\"value\"}")));
	const FUeAgentCallerContext Caller(
		TEXT("caller-1"),
		{TEXT("query.read")},
		TEXT("correlation-1"));
	const FUeAgentExecutionContextPolicy Policy =
		ExplicitEditorPolicy();
	FUeAgentExecutionContextRefPtr ExecutionContext;
	FUeAgentExecutionContextError ContextError;
	TestTrue(
		TEXT("explicit context freezes"),
		FUeAgentExecutionContextRef::TryCreate(
			ExplicitEditorInput(),
			Identity,
			EUeAgentRequestKind::Query,
			Policy,
			ExecutionContext,
			ContextError));
	if (!ExecutionContext.IsValid())
	{
		return false;
	}

	double Now = 100.0;
	const FUeAgentResourceTrackerOwner Tracker =
		ResourceTracker();
	FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(
			10.0,
			[&Now]() { return Now; });
	FUeAgentCancellationSource CancellationSource(
		[&Now]() { return Now; });
	TSharedPtr<FUeAgentOperationContext> Context;
	FUeAgentOperationContextError Error;
	TestTrue(
		TEXT("query operation context composes"),
		FUeAgentOperationContext::TryCreate(
			Request,
			Caller,
			ExecutionContext.ToSharedRef(),
			Tracker,
			MoveTemp(Deadline),
			CancellationSource.GetToken(),
			Context,
			Error));
	if (!Context.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("request kind"),
		Context->GetRequestKind(),
		EUeAgentRequestKind::Query);
	TestEqual(
		TEXT("request id"),
		Context->GetRequestId(),
		FString(TEXT("request-1")));
	TestTrue(
		TEXT("exact descriptor identity is bound"),
		Context->GetDescriptorIdentity() == Identity);
	TestEqual(
		TEXT("caller id"),
		Context->GetCallerContext().GetCallerId(),
		FString(TEXT("caller-1")));
	TestTrue(
		TEXT("execution context owner is preserved"),
		&Context->GetExecutionContext() == ExecutionContext.Get());
	TestTrue(
		TEXT("resource owner is preserved"),
		&Context->GetResourceTracker() == &Tracker.Get());

	FUeAgentResourceBudgetExceeded BudgetError;
	FUeAgentResourceScope First = Context->Scope(TEXT("/first"));
	FUeAgentResourceScope Second = Context->Scope(TEXT("/second"));
	TestTrue(
		TEXT("first scope charges owner"),
		First.TryCharge(
			EUeAgentResourceDimension::WorkUnits,
			2,
			BudgetError));
	TestTrue(
		TEXT("second scope charges same owner"),
		Second.TryCharge(
			EUeAgentResourceDimension::WorkUnits,
			2,
			BudgetError));
	TestEqual(
		TEXT("cross-scope usage accumulates"),
		Context->GetResourceTracker().Snapshot().Consumed(
			EUeAgentResourceDimension::WorkUnits),
		int64(4));
	TestFalse(
		TEXT("N+1 is rejected by same owner"),
		First.TryCharge(
			EUeAgentResourceDimension::WorkUnits,
			1,
			BudgetError));
	TestEqual(
		TEXT("absolute deadline remains bound"),
		Context->GetDeadline().GetDeadlineAt(),
		110.0);
	TestFalse(
		TEXT("cancellation is initially clear"),
		Context->GetCancellationToken().IsCancelled());
	CancellationSource.Request(TEXT("test_cancel"));
	TestTrue(
		TEXT("same cancellation state is observed"),
		Context->GetCancellationToken().IsCancelled());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationContextValidationTest,
	"UeAgentInterface.Core.ContextResource.OperationContext.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationContextValidationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;

	const FUeAgentDescriptorIdentity Identity = DescriptorIdentity();
	const FUeAgentOperationRequest Request(
		Identity,
		TEXT("operation-request-1"),
		TEXT("idempotency-1"),
		FUeAgentContractValue(TEXT("{\"change\":\"value\"}")));
	const FUeAgentCallerContext Caller(
		TEXT("caller-1"),
		{TEXT("operation.write")},
		TEXT("correlation-1"));
	FUeAgentExecutionContextRefPtr ExecutionContext;
	FUeAgentExecutionContextError ContextError;
	TestTrue(
		TEXT("operation execution context binds"),
		FUeAgentExecutionContextRef::TryCreate(
			ExplicitEditorInput(),
			Identity,
			EUeAgentRequestKind::Operation,
			ExplicitEditorPolicy(),
			ExecutionContext,
			ContextError));
	if (!ExecutionContext.IsValid())
	{
		return false;
	}

	double Now = 200.0;
	FUeAgentCancellationSource CancellationSource(
		[&Now]() { return Now; });
	TSharedPtr<FUeAgentOperationContext> Context;
	FUeAgentOperationContextError Error;
	TestTrue(
		TEXT("operation request composes"),
		FUeAgentOperationContext::TryCreate(
			Request,
			Caller,
			ExecutionContext.ToSharedRef(),
			ResourceTracker(),
			FUeAgentOperationDeadline::FromTimeout(
				10.0,
				[&Now]() { return Now; }),
			CancellationSource.GetToken(),
			Context,
			Error));
	if (!Context.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("operation request kind"),
		Context->GetRequestKind(),
		EUeAgentRequestKind::Operation);
	TestEqual(
		TEXT("idempotency key is bound"),
		Context->GetIdempotencyKey(),
		FString(TEXT("idempotency-1")));

	const FUeAgentCallerContext InvalidCaller(
		FString(),
		{TEXT("operation.write")},
		TEXT("correlation-1"));
	TestFalse(
		TEXT("invalid caller is rejected"),
		FUeAgentOperationContext::TryCreate(
			Request,
			InvalidCaller,
			ExecutionContext.ToSharedRef(),
			ResourceTracker(),
			FUeAgentOperationDeadline::FromTimeout(
				10.0,
				[&Now]() { return Now; }),
			CancellationSource.GetToken(),
			Context,
			Error));
	TestEqual(
		TEXT("invalid caller code"),
		Error.Code,
		FString(TEXT("operation_context_caller_invalid")));

	const FUeAgentDescriptorIdentity DifferentIdentity(
		TEXT("uai.test.other-context"),
		1,
		TEXT(
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
	const FUeAgentOperationRequest DifferentRequest(
		DifferentIdentity,
		TEXT("operation-request-2"),
		TEXT("idempotency-2"),
		FUeAgentContractValue(TEXT("{\"change\":\"other\"}")));
	TestFalse(
		TEXT("descriptor mismatch is rejected"),
		FUeAgentOperationContext::TryCreate(
			DifferentRequest,
			Caller,
			ExecutionContext.ToSharedRef(),
			ResourceTracker(),
			FUeAgentOperationDeadline::FromTimeout(
				10.0,
				[&Now]() { return Now; }),
			CancellationSource.GetToken(),
			Context,
			Error));
	TestEqual(
		TEXT("descriptor mismatch code"),
		Error.Code,
		FString(TEXT("operation_context_descriptor_mismatch")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionContextPolicyAndAmbiguityTest,
	"UeAgentInterface.Core.ContextResource.ExecutionContext.PolicyAndAmbiguity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionContextPolicyAndAmbiguityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ContextResource;

	const FUeAgentDescriptorIdentity Identity = DescriptorIdentity();
	const FUeAgentExecutionContextPolicy Policy =
		ExplicitEditorPolicy();
	FUeAgentExecutionContextRefPtr Context;
	FUeAgentExecutionContextError Error;

	FUeAgentExecutionContextInput MissingSelection =
		ExplicitEditorInput();
	MissingSelection.SelectionIdentities.Reset();
	TestFalse(
		TEXT("required selection cannot be implicit"),
		FUeAgentExecutionContextRef::TryCreate(
			MissingSelection,
			Identity,
			EUeAgentRequestKind::Query,
			Policy,
			Context,
			Error));
	TestEqual(
		TEXT("missing required facet code"),
		Error.Code,
		FString(TEXT("execution_context_required_facet_missing")));
	TestEqual(
		TEXT("missing required facet identity"),
		Error.Facet,
		EUeAgentExecutionContextFacet::Selection);

	FUeAgentExecutionContextInput ResolveCurrent =
		ExplicitEditorInput();
	ResolveCurrent.bResolvedFromCurrent = true;
	TestFalse(
		TEXT("explicit-only rejects resolve-current provenance"),
		FUeAgentExecutionContextRef::TryCreate(
			ResolveCurrent,
			Identity,
			EUeAgentRequestKind::Query,
			Policy,
			Context,
			Error));
	TestEqual(
		TEXT("resolve-current policy code"),
		Error.Code,
		FString(TEXT(
			"execution_context_resolve_current_not_allowed")));

	FUeAgentExecutionContextInput Persisted =
		ExplicitEditorInput();
	Persisted.StatePlane = EUeAgentStatePlane::RuntimeInstance;
	TestFalse(
		TEXT("state plane must be descriptor-allowed"),
		FUeAgentExecutionContextRef::TryCreate(
			Persisted,
			Identity,
			EUeAgentRequestKind::Query,
			Policy,
			Context,
			Error));
	TestEqual(
		TEXT("state plane policy code"),
		Error.Code,
		FString(TEXT(
			"execution_context_state_plane_not_allowed")));

	const FUeAgentExecutionContextPolicy PiePolicy(
		{EUeAgentStatePlane::RuntimeInstance},
		{
			EUeAgentExecutionContextFacet::World,
			EUeAgentExecutionContextFacet::PieInstance,
		},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	FUeAgentExecutionContextInput Pie;
	Pie.ProjectInstanceId = TEXT("project-instance");
	Pie.EditorInstanceId = TEXT("editor-instance");
	Pie.StatePlane = EUeAgentStatePlane::RuntimeInstance;
	Pie.WorldType = EUeAgentExecutionWorldType::Pie;
	Pie.WorldIdentity = TEXT("pie-world");
	Pie.ContextRevision = TEXT("pie-rev-1");
	TestFalse(
		TEXT("PIE instance ambiguity is rejected"),
		FUeAgentExecutionContextRef::TryCreate(
			Pie,
			Identity,
			EUeAgentRequestKind::Query,
			PiePolicy,
			Context,
			Error));
	TestEqual(
		TEXT("PIE ambiguity code"),
		Error.Code,
		FString(TEXT("execution_context_pie_instance_required")));

	const FUeAgentExecutionContextPolicy PreviewReadOnly(
		{EUeAgentStatePlane::AuthoringMemory},
		{EUeAgentExecutionContextFacet::World},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	FUeAgentExecutionContextInput Preview;
	Preview.ProjectInstanceId = TEXT("project-instance");
	Preview.EditorInstanceId = TEXT("editor-instance");
	Preview.StatePlane = EUeAgentStatePlane::AuthoringMemory;
	Preview.WorldType = EUeAgentExecutionWorldType::Preview;
	Preview.WorldIdentity = TEXT("preview-world");
	Preview.ContextRevision = TEXT("preview-rev-1");
	TestTrue(
		TEXT("preview query is read-only valid"),
		FUeAgentExecutionContextRef::TryCreate(
			Preview,
			Identity,
			EUeAgentRequestKind::Query,
			PreviewReadOnly,
			Context,
			Error));
	TestFalse(
		TEXT("preview mutation is rejected by default"),
		FUeAgentExecutionContextRef::TryCreate(
			Preview,
			Identity,
			EUeAgentRequestKind::Operation,
			PreviewReadOnly,
			Context,
			Error));
	TestEqual(
		TEXT("preview mutation policy code"),
		Error.Code,
		FString(TEXT(
			"execution_context_transient_mutation_not_allowed")));

	const FUeAgentExecutionContextPolicy PreviewMutation(
		{EUeAgentStatePlane::AuthoringMemory},
		{EUeAgentExecutionContextFacet::World},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::
			TemporaryMutationAllowed);
	TestTrue(
		TEXT("explicit temporary preview mutation can bind"),
		FUeAgentExecutionContextRef::TryCreate(
			Preview,
			Identity,
			EUeAgentRequestKind::Operation,
			PreviewMutation,
			Context,
			Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionContextRevisionAndFreezeTest,
	"UeAgentInterface.Core.ContextResource.ExecutionContext.RevisionAndFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionContextRevisionAndFreezeTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;

	FUeAgentExecutionContextInput Input = ExplicitEditorInput();
	FUeAgentExecutionContextRefPtr Context;
	FUeAgentExecutionContextError Error;
	TestTrue(
		TEXT("context freezes from explicit input"),
		FUeAgentExecutionContextRef::TryCreate(
			Input,
			DescriptorIdentity(),
			EUeAgentRequestKind::Query,
			ExplicitEditorPolicy(),
			Context,
			Error));
	if (!Context.IsValid())
	{
		return false;
	}

	Input.WorldIdentity = TEXT("different-world");
	Input.ViewportIdentity = TEXT("different-viewport");
	Input.SelectionIdentities = {TEXT("different-selection")};
	Input.ContextRevision = TEXT("ctx-rev-8");
	TestEqual(
		TEXT("world identity is frozen"),
		Context->GetWorldIdentity(),
		FString(TEXT("/Game/Maps/TestMap:EditorWorld")));
	TestEqual(
		TEXT("viewport identity is frozen"),
		Context->GetViewportIdentity(),
		FString(TEXT("level-editor:viewport-1")));
	TestEqual(
		TEXT("selection identities are frozen"),
		Context->GetSelectionIdentities()[0],
		FString(TEXT("/Game/Maps/TestMap.Actor_1")));
	TestEqual(
		TEXT("context revision is frozen"),
		Context->GetContextRevision(),
		FString(TEXT("ctx-rev-7")));

	TestTrue(
		TEXT("same revision validates"),
		Context->TryValidateRevision(TEXT("ctx-rev-7"), Error));
	TestFalse(
		TEXT("stale revision is rejected"),
		Context->TryValidateRevision(TEXT("ctx-rev-8"), Error));
	TestEqual(
		TEXT("stale revision code"),
		Error.Code,
		FString(TEXT("execution_context_revision_conflict")));
	return true;
}

