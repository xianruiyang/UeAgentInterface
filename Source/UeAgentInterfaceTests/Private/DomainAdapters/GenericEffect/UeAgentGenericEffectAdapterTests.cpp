#include "Misc/AutomationTest.h"

#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	static TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeTracker()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16777216
				},
			}));
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const FString& Suffix = TEXT("Title"))
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.reference.settings.")
				+ Suffix.ToLower(),
			TEXT("reference_setting"),
			TEXT("/Game/UAI/P4_007"),
			Suffix,
			TEXT("property"),
			TEXT("editor-session-31"));
	}

	static FUeAgentCanonicalValueRef MakeStringValue(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static FUeAgentCandidateBinding MakeCandidateBinding(
		const FUeAgentCanonicalIdentity& Identity,
		TArray<EUeAgentCandidateOperation> Operations = {
			EUeAgentCandidateOperation::Read,
			EUeAgentCandidateOperation::Create,
			EUeAgentCandidateOperation::Update,
			EUeAgentCandidateOperation::Replace,
			EUeAgentCandidateOperation::Delete,
		})
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:p4-007:reference-title"),
			FUeAgentCandidateSummary(
				Identity,
				FUeAgentDescriptorIdentity(
					TEXT("uai.reference.settings.effect/v1"),
					1,
					TEXT(
						"sha256:5555555555555555555555555555555555555555555555555555555555555555")),
				TEXT("reference.setting"),
				TEXT("Reference Title"),
				TEXT("P4-007 typed GenericEffect fixture"),
				{TEXT("Reference"), TEXT("Settings")}),
			FUeAgentCatalogRevision(
				TEXT("reference-setting-catalog"),
				TEXT("catalog-generation-9"),
				37),
			TEXT("uai://asset/%2FGame%2FUAI%2FP4_007#Title"),
			TEXT("generic-read"),
			MoveTemp(Operations),
			TEXT("fixture:p4-007:candidate-binding"));
	}

	static FUeAgentGenericEffectBinding MakeEffectBinding(
		const FUeAgentCanonicalIdentity& Identity,
		TArray<EUeAgentCandidateOperation> Operations = {
			EUeAgentCandidateOperation::Read,
			EUeAgentCandidateOperation::Create,
			EUeAgentCandidateOperation::Update,
			EUeAgentCandidateOperation::Replace,
			EUeAgentCandidateOperation::Delete,
		})
	{
		return FUeAgentGenericEffectBinding(
			MakeCandidateBinding(Identity, MoveTemp(Operations)),
			GetUeStringPropertySourceTypeId(),
			FUeAgentCanonicalType(
				TEXT("string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			TEXT("fixture:p4-007:ue-type-binding"));
	}

	static FPlanningPreflightPlanRef MakePlan(
		FUeAgentResourceTracker& Resources,
		const EChangeIntent Intent,
		const FUeAgentCanonicalIdentity& Identity = MakeIdentity())
	{
		FUeAgentCanonicalValuePtr Before;
		FUeAgentCanonicalValuePtr After;
		if (Intent != EChangeIntent::Create)
		{
			Before = MakeStringValue(TEXT("before"), Resources);
		}
		if (Intent != EChangeIntent::Delete)
		{
			After = MakeStringValue(TEXT("after"), Resources);
		}

		TArray<FPlannedChange> Changes;
		Changes.Emplace(
			Identity,
			Intent,
			Before,
			After,
			TArray<FString>{TEXT("uai:reference:write")},
			TEXT("fixture:p4-007:direct-impact"));
		TArray<FImpact> Impacts;
		Impacts.Emplace(
			Identity,
			Identity,
			TEXT("direct"),
			TEXT("fixture:p4-007:direct-impact"));
		return MakeShared<FPlanningPreflightPlan>(
			FUeAgentDescriptorIdentity(
				TEXT("uai.reference.settings.effect/v1"),
				1,
				TEXT(
					"sha256:5555555555555555555555555555555555555555555555555555555555555555")),
			TEXT("request:p4-007"),
			TEXT("idempotency:p4-007"),
			TEXT("agent:p4-007"),
			TArray<FString>{TEXT("uai:reference:write")},
			TEXT("snapshot:p4-007:before"),
			FUeAgentStateRevision(
				TEXT("generic-read"),
				TEXT("editor-session-31"),
				71),
			TEXT("fixture:p4-007:independent-before"),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:6666666666666666666666666666666666666666666666666666666666666666"));
	}

	static FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& DescriptorIdentity)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:p4-007");
		Input.EditorInstanceId = TEXT("fixture-editor:p4-007");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:p4-007:r1");

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			DescriptorIdentity,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::Persisted},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::ReadOnly),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	struct FRequestFixture
	{
		explicit FRequestFixture(
			const EChangeIntent Intent = EChangeIntent::Update,
			const FUeAgentCanonicalIdentity& Identity = MakeIdentity())
			: Resources(MakeTracker())
			, Cancellation([this]() { return Now; })
			, Plan(MakePlan(*Resources, Intent, Identity))
			, ExecutionContext(
				MakeExecutionContext(
					Plan->GetDescriptorIdentity()))
		{
		}

		FExecutionMutationRequest MakeRequest(
			const double TimeoutSeconds = 30.0)
		{
			return FExecutionMutationRequest(
				TEXT("operation:p4-007"),
				TEXT("owned-worker:p4-007"),
				Plan,
				ExecutionContext,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					TimeoutSeconds,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = 100.0;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner ExecutionContext;
	};

	class FRecordingGenericEffectUeHook final
		: public IUeAgentGenericEffectUeHook
	{
	public:
		virtual bool TryApply(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++ApplyCalls;
			LastRequest = &Request;
			LastLease = Lease;
			LastBindings = Bindings;
			LastResources = &Resources;
			OutResult = ApplyResult;
			if (bFailApply)
			{
				OutError = Failure;
				return false;
			}
			return true;
		}

		virtual bool TryRollback(
			const FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) override
		{
			++RollbackCalls;
			LastRollback = Applied;
			LastResources = &Resources;
			bOutConfirmed = bRollbackConfirmed;
			if (bFailRollback)
			{
				OutError = Failure;
				return false;
			}
			return true;
		}

		virtual void ReleaseVerifiedOperation(
			const FString& OperationId) override
		{
			++ReleaseCalls;
			LastReleasedOperationId = OperationId;
		}

		virtual bool TryWaitForSettle(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FBarrierResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++WaitCalls;
			LastBarrier = Request;
			LastResources = &Resources;
			OutResult = BarrierResult;
			if (bFailWait)
			{
				OutError = Failure;
				return false;
			}
			return true;
		}

		FUeAgentGenericEffectApplyResult ApplyResult = {
			91,
			{MakeIdentity().GetCanonicalKey()},
			{TEXT("property_notifications_flushed"),
			 TEXT("generated_state_settled")},
			true,
			TEXT("rollback:p4-007:reference-title"),
			true,
		};
		FBarrierResult BarrierResult = {
			91,
			{TEXT("property_notifications_flushed"),
			 TEXT("generated_state_settled")},
			TEXT("revision:p4-007:settled"),
			true,
		};
		FExecutionMutationError Failure = FExecutionMutationError(
			TEXT("fixture_generic_effect_failed"),
			TEXT("injected UE hook failure"));
		bool bFailApply = false;
		bool bFailRollback = false;
		bool bFailWait = false;
		bool bRollbackConfirmed = true;
		int32 ApplyCalls = 0;
		int32 RollbackCalls = 0;
		int32 ReleaseCalls = 0;
		int32 WaitCalls = 0;
		const FExecutionMutationRequest* LastRequest = nullptr;
		TOptional<FLockFenceLease> LastLease;
		TArray<FUeAgentGenericEffectBinding> LastBindings;
		TOptional<FUeAgentGenericEffectApplyResult> LastRollback;
		FString LastReleasedOperationId;
		TOptional<FBarrierRequest> LastBarrier;
		const FUeAgentResourceTracker* LastResources = nullptr;
	};

	static TSharedRef<FUeAgentGenericEffectMutationHook, ESPMode::ThreadSafe>
	MakeMutationHook(
		const TSharedRef<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe>& UeHook,
		const FUeAgentGenericEffectBinding& Binding)
	{
		return MakeShared<
			FUeAgentGenericEffectMutationHook,
			ESPMode::ThreadSafe>(
			TArray<FUeAgentGenericEffectBinding>{Binding},
			UeHook);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectTypedCrudTest,
	"UeAgentInterface.DomainAdapters.GenericEffect.Adapter.TypedCrudAndBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectTypedCrudTest::RunTest(
	const FString& Parameters)
{
	TestEqual(
		TEXT("FString property source type has one shared identity"),
		GetUeStringPropertySourceTypeId(),
		FString(TEXT("UE.StrProperty:FString")));
	const TArray<EChangeIntent> Intents = {
		EChangeIntent::Create,
		EChangeIntent::Update,
		EChangeIntent::Replace,
		EChangeIntent::Delete,
	};
	const TArray<EUeAgentCandidateOperation> Operations = {
		EUeAgentCandidateOperation::Create,
		EUeAgentCandidateOperation::Update,
		EUeAgentCandidateOperation::Replace,
		EUeAgentCandidateOperation::Delete,
	};
	for (int32 Index = 0; Index < Intents.Num(); ++Index)
	{
		FRequestFixture Fixture(Intents[Index]);
		const FExecutionMutationRequest Request =
			Fixture.MakeRequest();
		const FUeAgentCanonicalIdentity& Identity =
			Request.GetPlan()->GetChangeSet()
				.GetChanges()[0].GetIdentity();
		const TSharedRef<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe> UeHook =
			MakeShared<
				FRecordingGenericEffectUeHook,
				ESPMode::ThreadSafe>();
		const TSharedRef<
			FUeAgentGenericEffectMutationHook,
			ESPMode::ThreadSafe> Hook =
			MakeMutationHook(
				UeHook,
				MakeEffectBinding(Identity));
		const FLockFenceLease Lease(
			TEXT("owned-worker:p4-007"),
			{Identity.GetCanonicalKey()},
			101 + Index);
		FMutationFacts Facts;
		FExecutionMutationError Error;
		TestTrue(
			*FString::Printf(
				TEXT("%s reaches UE hook"),
				*LexToString(Intents[Index])),
			Hook->Execute(
				Request,
				Lease,
				*Fixture.Resources,
				Facts,
				Error));
		TestEqual(TEXT("one UE apply"), UeHook->ApplyCalls, 1);
		TestEqual(
			TEXT("planned intent remains typed"),
			UeHook->LastRequest->GetPlan()->GetChangeSet()
				.GetChanges()[0].GetIntent(),
			Intents[Index]);
		TestTrue(
			TEXT("candidate operation remains supported"),
			UeHook->LastBindings[0].GetCandidateBinding()
				.Supports(Operations[Index]));
		TestEqual(
			TEXT("catalog revision is not discarded"),
			UeHook->LastBindings[0].GetCandidateBinding()
				.GetCatalogRevision().GetSequence(),
			static_cast<int64>(37));
		TestEqual(
			TEXT("public ref is not re-resolved from display name"),
			UeHook->LastBindings[0].GetCandidateBinding()
				.GetPublicRef(),
			FString(
				TEXT("uai://asset/%2FGame%2FUAI%2FP4_007#Title")));
		TestEqual(
			TEXT("UE source type remains explicit"),
			UeHook->LastBindings[0].GetSourceTypeId(),
			GetUeStringPropertySourceTypeId());
		TestEqual(
			TEXT("canonical type remains C4 string"),
			UeHook->LastBindings[0].GetCanonicalType()
				.GetValueKind(),
			EUeAgentValueKind::String);
		TestTrue(
			TEXT("same resource tracker reaches UE hook"),
			UeHook->LastResources == &Fixture.Resources.Get());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectFactsRollbackTest,
	"UeAgentInterface.DomainAdapters.GenericEffect.Adapter.FactsAndOwnedRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectFactsRollbackTest::RunTest(
	const FString& Parameters)
{
	FRequestFixture Fixture;
	const FExecutionMutationRequest Request = Fixture.MakeRequest();
	const FUeAgentCanonicalIdentity& Identity =
		Request.GetPlan()->GetChangeSet().GetChanges()[0].GetIdentity();
	const TSharedRef<
		FRecordingGenericEffectUeHook,
		ESPMode::ThreadSafe> UeHook =
		MakeShared<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe>();
	UeHook->ApplyResult = FUeAgentGenericEffectApplyResult(
		91,
		{Identity.GetCanonicalKey()},
		{
			TEXT("property_notifications_flushed"),
			TEXT("generated_state_settled"),
		},
		true,
		TEXT("rollback:p4-007:reference-title"),
		true,
		{
			UE::UAI::AssetDurable::FAssetDurableTarget(
				Identity,
				TEXT("/Game/UAI/P4_007"),
				UE::UAI::AssetDurable::EAssetDurableIntent::Save,
				UE::UAI::AssetDurable::ERollbackGrade::DurableBackup),
		});
	const TSharedRef<
		FUeAgentGenericEffectMutationHook,
		ESPMode::ThreadSafe> Hook =
		MakeMutationHook(UeHook, MakeEffectBinding(Identity));
	const FLockFenceLease Lease(
		TEXT("owned-worker:p4-007"),
		{Identity.GetCanonicalKey()},
		141);
	FMutationFacts Facts;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("typed mutation succeeds"),
		Hook->Execute(
			Request,
			Lease,
			*Fixture.Resources,
			Facts,
			Error));
	TestEqual(
		TEXT("subject generation comes from actual UE effect"),
		Facts.GetSubjectGeneration(),
		static_cast<int64>(91));
	TestTrue(
		TEXT("actual mutated keys are propagated"),
		Facts.GetMutatedIdentityKeys()
			== UeHook->ApplyResult.GetMutatedIdentityKeys());
	TestTrue(
		TEXT("actual settle signals are propagated"),
		Facts.GetRequiredBarrierSignals()
			== UeHook->ApplyResult.GetRequiredBarrierSignals());
	TestTrue(
		TEXT("rollback support comes from UE hook"),
		Facts.IsRollbackSupported());
	TestEqual(
		TEXT("mutation-produced durable targets cross the generic adapter"),
		Facts.GetDurableTargets().Num(),
		1);
	if (Facts.GetDurableTargets().Num() == 1)
	{
		TestEqual(
			TEXT("the durable package identity is unchanged"),
			Facts.GetDurableTargets()[0].GetDurableLocation(),
			FString(TEXT("/Game/UAI/P4_007")));
	}

	bool bConfirmed = false;
	const bool bRolledBack = Hook->Rollback(
		Request,
		Facts,
		*Fixture.Resources,
		bConfirmed,
		Error);
	TestTrue(
		TEXT("rollback delegates to same operation hook"),
		bRolledBack);
	if (!bRolledBack || !UeHook->LastRollback.IsSet())
	{
		AddError(
			TEXT("rollback did not reach the typed UE hook: ")
			+ Error.GetCode()
			+ TEXT(":")
			+ Error.GetDetail());
		return false;
	}
	TestTrue(TEXT("rollback is confirmed"), bConfirmed);
	TestEqual(TEXT("one rollback"), UeHook->RollbackCalls, 1);
	TestEqual(
		TEXT("opaque rollback token is preserved"),
		UeHook->LastRollback->GetRollbackToken(),
		UeHook->ApplyResult.GetRollbackToken());

	TestFalse(
		TEXT("same operation cannot rollback twice"),
		Hook->Rollback(
			Request,
			Facts,
			*Fixture.Resources,
			bConfirmed,
			Error));
	TestEqual(
		TEXT("double rollback is typed"),
		Error.GetCode(),
		FString(TEXT("generic_effect_rollback_state_invalid")));
	TestEqual(
		TEXT("double rollback has no second UE effect"),
		UeHook->RollbackCalls,
		1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectBoundaryTest,
	"UeAgentInterface.DomainAdapters.GenericEffect.Adapter.FailClosedBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectBoundaryTest::RunTest(
	const FString& Parameters)
{
	auto ExpectNoEffect = [this](
		const FUeAgentCanonicalIdentity& PlanIdentity,
		const FUeAgentGenericEffectBinding& Binding,
		const FString& ExpectedCode,
		const bool bCancel)
	{
		FRequestFixture Fixture(
			EChangeIntent::Update,
			PlanIdentity);
		if (bCancel)
		{
			Fixture.Cancellation.Request(
				TEXT("fixture cancellation"));
		}
		const FExecutionMutationRequest Request =
			Fixture.MakeRequest();
		const TSharedRef<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe> UeHook =
			MakeShared<
				FRecordingGenericEffectUeHook,
				ESPMode::ThreadSafe>();
		const TSharedRef<
			FUeAgentGenericEffectMutationHook,
			ESPMode::ThreadSafe> Hook =
			MakeMutationHook(UeHook, Binding);
		const FLockFenceLease Lease(
			TEXT("owned-worker:p4-007"),
			{PlanIdentity.GetCanonicalKey()},
			181);
		FMutationFacts Facts;
		FExecutionMutationError Error;
		TestFalse(
			*FString::Printf(
				TEXT("reject %s"),
				*ExpectedCode),
			Hook->Execute(
				Request,
				Lease,
				*Fixture.Resources,
				Facts,
				Error));
		TestEqual(
			*FString::Printf(
				TEXT("%s remains typed"),
				*ExpectedCode),
			Error.GetCode(),
			ExpectedCode);
		TestEqual(
			TEXT("rejection has zero UE effects"),
			UeHook->ApplyCalls,
			0);
	};

	const FUeAgentCanonicalIdentity Identity = MakeIdentity();
	ExpectNoEffect(
		Identity,
		MakeEffectBinding(MakeIdentity(TEXT("Other"))),
		TEXT("generic_effect_binding_missing"),
		false);
	ExpectNoEffect(
		Identity,
		MakeEffectBinding(
			Identity,
			{EUeAgentCandidateOperation::Read}),
		TEXT("generic_effect_operation_unsupported"),
		false);
	ExpectNoEffect(
		Identity,
		MakeEffectBinding(Identity),
		TEXT("cancellation_requested"),
		true);

	FRequestFixture Fixture;
	const FExecutionMutationRequest Request = Fixture.MakeRequest();
	const TSharedRef<
		FRecordingGenericEffectUeHook,
		ESPMode::ThreadSafe> UeHook =
		MakeShared<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe>();
	UeHook->bFailApply = true;
	const TSharedRef<
		FUeAgentGenericEffectMutationHook,
		ESPMode::ThreadSafe> Hook =
		MakeMutationHook(UeHook, MakeEffectBinding(Identity));
	const FLockFenceLease Lease(
		TEXT("owned-worker:p4-007"),
		{Identity.GetCanonicalKey()},
		191);
	FMutationFacts Facts;
	FExecutionMutationError Error;
	TestFalse(
		TEXT("UE apply failure is propagated"),
		Hook->Execute(
			Request,
			Lease,
			*Fixture.Resources,
			Facts,
			Error));
	TestEqual(
		TEXT("UE error code is preserved"),
		Error.GetCode(),
		UeHook->Failure.GetCode());
	TestTrue(
		TEXT("partial effect still exposes valid facts for C7 rollback"),
		Facts.IsValid());
	TestTrue(
		TEXT("partial effect remains rollback-supported"),
		Facts.IsRollbackSupported());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectSettleTest,
	"UeAgentInterface.DomainAdapters.GenericEffect.Adapter.CompileSettleProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectSettleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<
		FRecordingGenericEffectUeHook,
		ESPMode::ThreadSafe> UeHook =
		MakeShared<
			FRecordingGenericEffectUeHook,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FUeAgentGenericEffectBarrierProvider,
		ESPMode::ThreadSafe> Barrier =
		MakeShared<
			FUeAgentGenericEffectBarrierProvider,
			ESPMode::ThreadSafe>(UeHook);
	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> Resources = MakeTracker();
	const FBarrierRequest Request(
		91,
		{TEXT("property_notifications_flushed"),
		 TEXT("generated_state_settled")});
	FBarrierResult Result;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("settle delegates to UE hook"),
		Barrier->Wait(
			Request,
			*Resources,
			Result,
			Error));
	TestEqual(TEXT("one settle wait"), UeHook->WaitCalls, 1);
	TestEqual(
		TEXT("subject generation remains exact"),
		UeHook->LastBarrier->GetSubjectGeneration(),
		static_cast<int64>(91));
	TestTrue(
		TEXT("required signals remain exact"),
		UeHook->LastBarrier->GetRequiredSignals()
			== Request.GetRequiredSignals());
	TestTrue(
		TEXT("same resource tracker reaches settle hook"),
		UeHook->LastResources == &Resources.Get());
	TestTrue(TEXT("settled result is not fabricated"), Result.IsSettled());

	UeHook->bFailWait = true;
	TestFalse(
		TEXT("compile settle failure is explicit"),
		Barrier->Wait(
			Request,
			*Resources,
			Result,
			Error));
	TestEqual(
		TEXT("settle error code is preserved"),
		Error.GetCode(),
		UeHook->Failure.GetCode());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectReflectionE2Test,
	"UeAgentInterface.DomainAdapters.GenericEffect.Reflection.TypedEffectSettleRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectReflectionE2Test::RunTest(
	const FString& Parameters)
{
	UBlueprint* Blueprint = NewObject<UBlueprint>(
		GetTransientPackage(),
		MakeUniqueObjectName(
			GetTransientPackage(),
			UBlueprint::StaticClass(),
			TEXT("P4_008_GenericEffectFixture")),
		RF_Transient | RF_Transactional);
	TestNotNull(TEXT("transient Blueprint fixture"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}
	Blueprint->BlueprintDisplayName = TEXT("before");

	const FUeAgentCanonicalIdentity Identity(
		TEXT("uai.test.generic_effect.blueprint_display_name"),
		TEXT("transient_blueprint"),
		Blueprint->GetPathName(),
		TEXT("BlueprintDisplayName"),
		TEXT("property"),
		TEXT("editor-session-31"));
	FRequestFixture Fixture(EChangeIntent::Update, Identity);
	const FExecutionMutationRequest Request = Fixture.MakeRequest();
	const FUeAgentGenericEffectBinding Binding(
		MakeCandidateBinding(
			Identity,
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Update,
			}),
		TEXT("UE.StrProperty:FString"),
		FUeAgentCanonicalType(
			TEXT("string"),
			EUeAgentValueKind::String,
			false,
			TEXT("scalar")),
		TEXT("reflection:FStrProperty->C4:string"));
	const TSharedRef<
		FUeAgentReflectionGenericEffectUeHook,
		ESPMode::ThreadSafe> UeHook =
		MakeShared<
			FUeAgentReflectionGenericEffectUeHook,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FUeAgentGenericEffectMutationHook,
		ESPMode::ThreadSafe> Mutation =
		MakeShared<
			FUeAgentGenericEffectMutationHook,
			ESPMode::ThreadSafe>(
			TArray<FUeAgentGenericEffectBinding>{Binding},
			UeHook);
	const FLockFenceLease Lease(
		TEXT("owned-worker:p4-007"),
		{Identity.GetCanonicalKey()},
		211);
	FMutationFacts Facts;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("real reflection effect succeeds"),
		Mutation->Execute(
			Request,
			Lease,
			*Fixture.Resources,
			Facts,
			Error));
	TestEqual(
		TEXT("real FString property changed"),
		Blueprint->BlueprintDisplayName,
		FString(TEXT("after")));
	TestTrue(TEXT("real mutation facts are valid"), Facts.IsValid());
	TestTrue(
		TEXT("real effect records its canonical identity"),
		Facts.GetMutatedIdentityKeys()
			== TArray<FString>{Identity.GetCanonicalKey()});

	const TSharedRef<
		FUeAgentGenericEffectBarrierProvider,
		ESPMode::ThreadSafe> Barrier =
		MakeShared<
			FUeAgentGenericEffectBarrierProvider,
			ESPMode::ThreadSafe>(UeHook);
	const FBarrierRequest BarrierRequest(
		Facts.GetSubjectGeneration(),
		Facts.GetRequiredBarrierSignals());
	FBarrierResult BarrierResult;
	TestTrue(
		TEXT("real synchronous UE notification settles"),
		Barrier->Wait(
			BarrierRequest,
			*Fixture.Resources,
			BarrierResult,
			Error));
	TestTrue(
		TEXT("real settle is generation-bound"),
		BarrierResult.IsSettled());
	TestEqual(
		TEXT("settle generation matches mutation"),
		BarrierResult.GetSubjectGeneration(),
		Facts.GetSubjectGeneration());

	bool bRollbackConfirmed = false;
	TestTrue(
		TEXT("real reflection rollback succeeds"),
		Mutation->Rollback(
			Request,
			Facts,
			*Fixture.Resources,
			bRollbackConfirmed,
			Error));
	TestTrue(
		TEXT("real reflection rollback is confirmed"),
		bRollbackConfirmed);
	TestEqual(
		TEXT("real FString property restored"),
		Blueprint->BlueprintDisplayName,
		FString(TEXT("before")));
	Blueprint->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectVerifiedReleaseE2Test,
	"UeAgentInterface.DomainAdapters.GenericEffect.Reflection.VerifiedRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectVerifiedReleaseE2Test::RunTest(
	const FString& Parameters)
{
	UBlueprint* Blueprint = NewObject<UBlueprint>(
		GetTransientPackage(),
		MakeUniqueObjectName(
			GetTransientPackage(),
			UBlueprint::StaticClass(),
			TEXT("P5_005_GenericEffectVerifiedRelease")),
		RF_Transient | RF_Transactional);
	TestNotNull(TEXT("transient Blueprint fixture"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}
	Blueprint->BlueprintDisplayName = TEXT("before");

	const FUeAgentCanonicalIdentity Identity(
		TEXT("uai.test.generic_effect.verified_release"),
		TEXT("transient_blueprint"),
		Blueprint->GetPathName(),
		TEXT("BlueprintDisplayName"),
		TEXT("property"),
		TEXT("editor-session-31"));
	FRequestFixture Fixture(EChangeIntent::Update, Identity);
	const FExecutionMutationRequest Request = Fixture.MakeRequest();
	const FUeAgentGenericEffectBinding Binding(
		MakeCandidateBinding(
			Identity,
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Update,
			}),
		TEXT("UE.StrProperty:FString"),
		FUeAgentCanonicalType(
			TEXT("string"),
			EUeAgentValueKind::String,
			false,
			TEXT("scalar")),
		TEXT("reflection:FStrProperty->C4:string"));
	const TSharedRef<
		FUeAgentReflectionGenericEffectUeHook,
		ESPMode::ThreadSafe> UeHook =
		MakeShared<
			FUeAgentReflectionGenericEffectUeHook,
			ESPMode::ThreadSafe>();
	const FLockFenceLease Lease(
		TEXT("owned-worker:p5-005"),
		{Identity.GetCanonicalKey()},
		311);
	FUeAgentGenericEffectApplyResult Applied;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("real reflection effect succeeds"),
		UeHook->TryApply(
			Request,
			Lease,
			{Binding},
			*Fixture.Resources,
			Applied,
			Error));
	TestEqual(
		TEXT("real FString property changed"),
		Blueprint->BlueprintDisplayName,
		FString(TEXT("after")));

	FBarrierResult BarrierResult;
	TestTrue(
		TEXT("real reflection effect settles"),
		UeHook->TryWaitForSettle(
			FBarrierRequest(
				Applied.GetSubjectGeneration(),
				Applied.GetRequiredBarrierSignals()),
			*Fixture.Resources,
			BarrierResult,
			Error));
	TestTrue(TEXT("real settle is confirmed"), BarrierResult.IsSettled());

	UeHook->ReleaseVerifiedOperation(Request.GetOperationId());
	bool bRollbackConfirmed = true;
	TestFalse(
		TEXT("verified operation no longer retains rollback state"),
		UeHook->TryRollback(
			Request,
			Applied,
			*Fixture.Resources,
			bRollbackConfirmed,
			Error));
	TestFalse(
		TEXT("released operation cannot report rollback"),
		bRollbackConfirmed);
	TestEqual(
		TEXT("released rollback failure is explicit"),
		Error.GetCode(),
		FString(TEXT("generic_effect_rollback_record_invalid")));
	TestEqual(
		TEXT("verified state remains applied"),
		Blueprint->BlueprintDisplayName,
		FString(TEXT("after")));

	Blueprint->BlueprintDisplayName = TEXT("before");
	Blueprint->MarkAsGarbage();
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGenericEffectAdapterModuleAvailableTest,
	"UeAgentInterface.DomainAdapters.GenericEffect.Adapter.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGenericEffectAdapterModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}
