#include "Misc/AutomationTest.h"
#include <type_traits>
#include <utility>

#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Composition/NiagaraCompound/UeAgentNiagaraCompoundApplication.h"
#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"


namespace
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::ExecutionMutation;

	using FPayloadContextAccessor =
		const FUeAgentExecutionContextInput& (
			FUeAgentOperationBindingPayload::*)() const;
	using FRequestContextAccessor =
		const FUeAgentExecutionContextRef& (
			FExecutionMutationRequest::*)() const;
	using FOperationContextOwnerAccessor =
		const FUeAgentExecutionContextRefOwner& (
			FUeAgentOperationContext::*)() const;
	using FResourceOwnerAccessor =
		const FUeAgentResourceTrackerOwner& (
			FUeAgentOperationContext::*)() const;
	static_assert(std::is_same_v<
		decltype(
			&FUeAgentOperationBindingPayload::GetExecutionContext),
		FPayloadContextAccessor>);
	static_assert(std::is_same_v<
		decltype(&FExecutionMutationRequest::GetExecutionContext),
		FRequestContextAccessor>);
	static_assert(std::is_same_v<
		decltype(&FUeAgentOperationContext::
			GetExecutionContextOwner),
		FOperationContextOwnerAccessor>);
	static_assert(std::is_same_v<
		decltype(&FUeAgentOperationContext::
			GetResourceTrackerOwner),
		FResourceOwnerAccessor>);
	static_assert(std::is_same_v<
		decltype(UE::UAI::Composition::ReferenceCrud::
			FUeAgentReferenceCrudApplicationConfig::
				ExecutionContext),
		FUeAgentExecutionContextInput>);
	static_assert(std::is_same_v<
		decltype(UE::UAI::Composition::NiagaraCompound::
			FUeAgentNiagaraCompoundApplicationConfig::
				ExecutionContext),
		FUeAgentExecutionContextInput>);

	class FContextValidatorContract final
		: public IUeAgentExecutionContextValidator
	{
	public:
		bool TryValidate(
			const FUeAgentExecutionContextRef& Context,
			FUeAgentExecutionContextError& OutError)
			const override
		{
			return false;
		}
	};

	class FContextPlannerContract final
		: public IUeAgentOperationPlanner
	{
	public:
		bool TryPlan(
			const UE::UAI::PlanningPreflight::FPlanningRequest& Request,
			const FUeAgentOperationContext& Context,
			UE::UAI::PlanningPreflight::FPlanningResult& OutResult,
			FUeAgentOperationBindingError& OutError) override
		{
			return false;
		}
	};

	class FContextPreparerContract final
		: public IUeAgentOperationRequestPreparer
	{
	public:
		bool TryPrepare(
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			const TArray<FUeAgentOperationTargetBinding>&
				RequestedTargets,
			const FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FPlanningRequest>&
				OutPlanning,
			FUeAgentOperationBindingError& OutError) override
		{
			return false;
		}
	};

	static_assert(std::is_constructible_v<
		FUeAgentOperationBindingPayload,
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationSessionIdentity,
		TArray<FUeAgentOperationTargetBinding>,
		FUeAgentExecutionContextInput,
		UE::UAI::Contract::FUeAgentContractValue,
		TSharedRef<IUeAgentOperationRequestPreparer>,
		UE::UAI::Projection::FUeAgentProjectionRequest>);

	class FContextExecutorContract final
		: public IUeAgentOperationExecutor
	{
	public:
		bool TryExecute(
			const UE::UAI::PlanningPreflight::FPlanningResult& Planning,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FExecutionReceipt& OutReceipt,
			FUeAgentOperationBindingError& OutError) override
		{
			return false;
		}
	};

	class FContextVerifierContract final
		: public IUeAgentOperationVerifier
	{
	public:
		bool TryVerify(
			const FExecutionReceipt& Receipt,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			UE::UAI::Verification::FVerificationOutcome& OutOutcome,
			FUeAgentOperationBindingError& OutError) override
		{
			return false;
		}
	};

	static_assert(std::is_default_constructible_v<
		FContextPlannerContract>);
	static_assert(std::is_default_constructible_v<
		FContextExecutorContract>);
	static_assert(std::is_default_constructible_v<
		FContextVerifierContract>);
	static_assert(std::is_default_constructible_v<
		FContextValidatorContract>);
	static_assert(std::is_default_constructible_v<
		FContextPreparerContract>);
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationBindingPreparationContractTest,
	"UeAgentInterface.Core.BindingGateway.Operation.ContextPropagation.PreparationContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationBindingPreparationContractTest::RunTest(
	const FString& Parameters)
{
	TestTrue(
		TEXT("C3O owns preparation inside its single OperationContext"),
		std::is_default_constructible_v<FContextPreparerContract>);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationContextPropagationModuleAvailableTest,
	"UeAgentInterface.Core.BindingGateway.Operation.ContextPropagation.ModuleAvailable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationContextPropagationModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationContextPropagationContractShapeTest,
	"UeAgentInterface.Core.BindingGateway.Operation.ContextPropagation.ContractShape",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationContextPropagationContractShapeTest::RunTest(
	const FString& Parameters)
{
	TestTrue(
		TEXT("planner consumes the shared operation context"),
		std::is_default_constructible_v<FContextPlannerContract>);
	TestTrue(
		TEXT("executor consumes the shared operation context"),
		std::is_default_constructible_v<FContextExecutorContract>);
	TestTrue(
		TEXT("verifier consumes the shared operation context"),
		std::is_default_constructible_v<FContextVerifierContract>);
	TestTrue(
		TEXT("binding validates the explicit context before planning"),
		std::is_default_constructible_v<
			FContextValidatorContract>);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationContextPropagationFrozenPieContextTest,
	"UeAgentInterface.Core.BindingGateway.Operation.ContextPropagation.FrozenPieContext",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationContextPropagationFrozenPieContextTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;

	const FUeAgentDescriptorIdentity Descriptor(
		TEXT("uai.test.operation.context-propagation"),
		1,
		TEXT(
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	const FUeAgentExecutionContextPolicy Policy(
		{EUeAgentStatePlane::AuthoringMemory},
		{
			EUeAgentExecutionContextFacet::World,
			EUeAgentExecutionContextFacet::Level,
			EUeAgentExecutionContextFacet::PieInstance,
		},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
	FUeAgentExecutionContextInput Input;
	Input.ProjectInstanceId = TEXT("project:sa-002a");
	Input.EditorInstanceId = TEXT("editor:sa-002a");
	Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
	Input.WorldType = EUeAgentExecutionWorldType::Pie;
	Input.WorldIdentity = TEXT("/Game/UAI/SA_002A:PIE_3");
	Input.LevelIdentity =
		TEXT("/Game/UAI/SA_002A:PersistentLevel");
	Input.PieInstanceId = 3;
	Input.ContextRevision = TEXT("context:sa-002a:17");
	Input.bResolvedFromCurrent = false;

	FUeAgentExecutionContextRefPtr Context;
	FUeAgentExecutionContextError Error;
	TestTrue(
		TEXT("explicit PIE context freezes under descriptor policy"),
		FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			Policy,
			Context,
			Error));
	if (!Context.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("project identity"),
		Context->GetProjectInstanceId(),
		Input.ProjectInstanceId);
	TestEqual(
		TEXT("editor identity"),
		Context->GetEditorInstanceId(),
		Input.EditorInstanceId);
	TestEqual(
		TEXT("world identity"),
		Context->GetWorldIdentity(),
		Input.WorldIdentity);
	TestEqual(
		TEXT("level identity"),
		Context->GetLevelIdentity(),
		Input.LevelIdentity);
	TestTrue(
		TEXT("PIE identity remains present"),
		Context->GetPieInstanceId().IsSet());
	TestEqual(
		TEXT("PIE identity"),
		Context->GetPieInstanceId().GetValue(),
		3);
	TestEqual(
		TEXT("context revision"),
		Context->GetContextRevision(),
		Input.ContextRevision);
	TestFalse(
		TEXT("context was not resolved from current"),
		Context->WasResolvedFromCurrent());

	FUeAgentExactExecutionContextValidator Validator(
		Context.ToSharedRef());
	FUeAgentExecutionContextError ValidationError;
	TestTrue(
		TEXT("exact validator accepts the frozen context"),
		Validator.TryValidate(*Context, ValidationError));

	FUeAgentExecutionContextInput StaleInput = Input;
	StaleInput.ContextRevision = TEXT("context:sa-002a:18");
	FUeAgentExecutionContextRefPtr StaleContext;
	FUeAgentExecutionContextError StaleCreateError;
	TestTrue(
		TEXT("stale fixture still satisfies descriptor policy"),
		FUeAgentExecutionContextRef::TryCreate(
			StaleInput,
			Descriptor,
			EUeAgentRequestKind::Operation,
			Policy,
			StaleContext,
			StaleCreateError));
	if (!StaleContext.IsValid())
	{
		return false;
	}
	TestFalse(
		TEXT("exact validator rejects a stale context revision"),
		Validator.TryValidate(*StaleContext, ValidationError));
	TestEqual(
		TEXT("stale revision has one stable failure code"),
		ValidationError.Code,
		FString(TEXT("execution_context_revision_conflict")));
	return true;
}

