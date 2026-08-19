#include "Misc/AutomationTest.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapterTestFixture.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"


namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::DomainAdapters::GenericRead;
	using namespace UE::UAI::DomainAdapters::NiagaraCompound;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Tests::NiagaraCompound;

	class FRecordingNiagaraCompoundUeHook final
		: public IUeAgentNiagaraCompoundUeHook
	{
	public:
		bool TryCapture(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
			FUeAgentGenericReadError& OutError) override
		{
			++CaptureCalls;
			LastReadRequest = &Request;
			LastResources = &Resources;
			if (bFailCapture)
			{
				OutError = ReadFailure;
				return false;
			}
			if (Captures.IsEmpty())
			{
				OutError = {
					TEXT("niagara_capture_fixture_exhausted"),
					Request.GetBinding().GetSummary()
						.GetIdentity().GetCanonicalKey(),
					TEXT("independent capture queue is empty"),
				};
				return false;
			}
			OutCapture = Captures[0];
			Captures.RemoveAt(0);
			return true;
		}

		bool TryApply(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++ApplyCalls;
			LastResources = &Resources;
			LastBindings = Bindings;
			OutResult = ApplyResult;
			if (bFailApply)
			{
				OutError = EffectFailure;
				return false;
			}
			return true;
		}

		bool TryRollback(
			const FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) override
		{
			++RollbackCalls;
			LastResources = &Resources;
			bOutConfirmed = bRollbackConfirmed;
			if (bFailRollback)
			{
				OutError = EffectFailure;
				return false;
			}
			return true;
		}

		void ReleaseVerifiedOperation(
			const FString& OperationId) override
		{
			++ReleaseCalls;
			LastReleasedOperationId = OperationId;
		}

		bool TryCompileAndSettle(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentNiagaraCompileSettleReceipt& OutReceipt,
			FExecutionMutationError& OutError) override
		{
			++SettleCalls;
			LastBarrierRequest = Request;
			LastResources = &Resources;
			OutReceipt = SettleReceipt;
			if (bFailSettle)
			{
				OutError = EffectFailure;
				return false;
			}
			return true;
		}

		TArray<TSharedPtr<const FUeAgentUeReadCapture>> Captures;
		FUeAgentGenericEffectApplyResult ApplyResult;
		FUeAgentNiagaraCompileSettleReceipt SettleReceipt;
		FUeAgentGenericReadError ReadFailure;
		FExecutionMutationError EffectFailure;
		TArray<FUeAgentGenericEffectBinding> LastBindings;
		FBarrierRequest LastBarrierRequest;
		const FUeAgentStateReadRequest* LastReadRequest = nullptr;
		FUeAgentResourceTracker* LastResources = nullptr;
		int32 CaptureCalls = 0;
		int32 ApplyCalls = 0;
		int32 RollbackCalls = 0;
		int32 ReleaseCalls = 0;
		int32 SettleCalls = 0;
		FString LastReleasedOperationId;
		bool bFailCapture = false;
		bool bFailApply = false;
		bool bFailRollback = false;
		bool bFailSettle = false;
		bool bRollbackConfirmed = true;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundProjectionTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Adapter.CompoundProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundProjectionTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const FUeAgentCanonicalValueRef Compound = MakeDynamicInput(
		TEXT("Particles.Scale"),
		TEXT("/Niagara/Functions/Dynamic/MultiplyFloat"),
		{
			MakeLiteralInput(
				TEXT("A"),
				TEXT("2.5"),
				*Resources),
			MakeLiteralInput(
				TEXT("B"),
				TEXT("0.125"),
				*Resources),
		},
		*Resources);
	const TSharedRef<FRecordingNiagaraCompoundUeHook> NativeHook =
		MakeShared<FRecordingNiagaraCompoundUeHook>();
	NativeHook->Captures.Add(
		MakeCapture(
			*Resources,
			43,
			TEXT("fixture:p5-003:independent-graph-capture"),
			Compound));
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);

	const FUeAgentCandidateBinding Binding = MakeCandidateBinding();
	const FUeAgentStateReadRequest Request(
		Binding,
		EUeAgentStatePlane::Persisted);
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	TSharedPtr<const FUeAgentUeReadCapture> Capture;
	FUeAgentGenericReadError Error;
	TestTrue(
		TEXT("Niagara hook projects an independently captured compound value"),
		ReadHook->TryCapture(
			Request,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(10.0, Clock),
			Cancellation.GetToken(),
			Capture,
			Error));
	if (!Capture.IsValid() || Capture->GetEntries().Num() != 1)
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	TestEqual(
		TEXT("compound identity is the same C2/C4 identity"),
		Capture->GetEntries()[0].GetIdentity().GetCanonicalKey(),
		Binding.GetSummary().GetIdentity().GetCanonicalKey());
	TestEqual(
		TEXT("capture revision comes from the UE graph"),
		Capture->GetRevision().GetSequence(),
		43LL);
	TestEqual(
		TEXT("capture evidence is not a request echo"),
		Capture->GetEvidenceId(),
		FString(TEXT("fixture:p5-003:independent-graph-capture")));
	TestTrue(
		TEXT("compound value remains a typed C4 object"),
		Capture->GetEntries()[0].GetValue()->GetKind()
			== EUeAgentValueKind::Object);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundDynamicInputTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Adapter.DynamicInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundDynamicInputTest::RunTest(
	const FString& Parameters)
{
	{
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources = MakeTracker();
		const FUeAgentCanonicalValueRef DeepValue =
			MakeDeepDynamicInput(96, *Resources);
		FExecutionMutationError Error;
		TestTrue(
			TEXT(
				"dynamic-input validation is iterative and has no "
				"domain nesting limit"),
			TryValidateNiagaraCompoundInput(
				DeepValue,
				*Resources,
				Error));
	}

	{
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			ValueResources = MakeTracker();
		const FUeAgentCanonicalValueRef Valid =
			MakeDeepDynamicInput(2, *ValueResources);
		FRequestFixture ValidFixture(Valid);
		const FExecutionMutationRequest ValidRequest =
			ValidFixture.MakeRequest();
		const TSharedRef<FRecordingNiagaraCompoundUeHook> ValidNativeHook =
			MakeShared<FRecordingNiagaraCompoundUeHook>();
		ValidNativeHook->ApplyResult =
			FUeAgentGenericEffectApplyResult(
				71,
				{MakeIdentity().GetCanonicalKey()},
				{TEXT("niagara.compile"), TEXT("niagara.stack")},
				true,
				TEXT("rollback:p5-004:valid-dynamic-input"),
				true);
		const TSharedRef<FUeAgentNiagaraCompoundEffectHook>
			ValidEffectHook =
				MakeShared<FUeAgentNiagaraCompoundEffectHook>(
					ValidNativeHook);
		FUeAgentGenericEffectApplyResult ApplyResult;
		FExecutionMutationError ApplyError;
		TestTrue(
			TEXT(
				"a valid typed dynamic input reaches the native hook "
				"exactly once"),
			ValidEffectHook->TryApply(
				ValidRequest,
				FLockFenceLease(
					TEXT("owned-worker:p5-003"),
					{MakeIdentity().GetCanonicalKey()},
					19),
				{MakeEffectBinding()},
				*ValidFixture.Resources,
				ApplyResult,
				ApplyError));
		TestEqual(
			TEXT("valid dynamic input produces one native apply"),
			ValidNativeHook->ApplyCalls,
			1);
		TestTrue(
			TEXT("the operation-scoped C1 tracker is preserved"),
			ValidNativeHook->LastResources
				== &ValidFixture.Resources.Get());
		ValidEffectHook->ReleaseVerifiedOperation(
			ValidRequest.GetOperationId());
		TestEqual(
			TEXT("verified finalization reaches the native owner"),
			ValidNativeHook->ReleaseCalls,
			1);
		TestEqual(
			TEXT("verified finalization preserves operation identity"),
			ValidNativeHook->LastReleasedOperationId,
			ValidRequest.GetOperationId());
	}

	{
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			ValueResources = MakeTracker();
		const FUeAgentCanonicalValueRef BudgetedValue =
			MakeDeepDynamicInput(2, *ValueResources);
		FRequestFixture BudgetFixture(BudgetedValue, 1);
		const FExecutionMutationRequest BudgetRequest =
			BudgetFixture.MakeRequest();
		const TSharedRef<FRecordingNiagaraCompoundUeHook> BudgetNativeHook =
			MakeShared<FRecordingNiagaraCompoundUeHook>();
		const TSharedRef<FUeAgentNiagaraCompoundEffectHook>
			BudgetEffectHook =
				MakeShared<FUeAgentNiagaraCompoundEffectHook>(
					BudgetNativeHook);
		FUeAgentGenericEffectApplyResult BudgetResult;
		FExecutionMutationError BudgetError;
		TestFalse(
			TEXT("operation-scoped budget rejects before UE mutation"),
			BudgetEffectHook->TryApply(
				BudgetRequest,
				FLockFenceLease(
					TEXT("owned-worker:p5-003"),
					{MakeIdentity().GetCanonicalKey()},
					19),
				{MakeEffectBinding()},
				*BudgetFixture.Resources,
				BudgetResult,
				BudgetError));
		TestEqual(
			TEXT("budget failure produces zero native apply"),
			BudgetNativeHook->ApplyCalls,
			0);
		TestEqual(
			TEXT("budget failure preserves the C1 diagnostic code"),
			BudgetError.GetCode(),
			FString(TEXT("resource_budget_exceeded")));
	}

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const FUeAgentCanonicalValueRef Invalid =
		MakeInvalidMixedBranchInput(*Resources);
	FRequestFixture Fixture(Invalid);
	const FExecutionMutationRequest Request = Fixture.MakeRequest();
	const TSharedRef<FRecordingNiagaraCompoundUeHook> NativeHook =
		MakeShared<FRecordingNiagaraCompoundUeHook>();
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	FUeAgentGenericEffectApplyResult Result;
	FExecutionMutationError Error;
	TestFalse(
		TEXT(
			"mixed literal and dynamic-input branches fail before UE "
			"mutation"),
		EffectHook->TryApply(
			Request,
			FLockFenceLease(
				TEXT("owned-worker:p5-003"),
				{MakeIdentity().GetCanonicalKey()},
				19),
			{MakeEffectBinding()},
			*Fixture.Resources,
			Result,
			Error));
	TestEqual(
		TEXT("invalid branch produces zero UE mutation calls"),
		NativeHook->ApplyCalls,
		0);
	TestEqual(
		TEXT("branch error is stable"),
		Error.GetCode(),
		FString(TEXT("niagara_compound_branch_conflict")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundCompileSettleTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Adapter.CompileSettleReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundCompileSettleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const TSharedRef<FRecordingNiagaraCompoundUeHook> NativeHook =
		MakeShared<FRecordingNiagaraCompoundUeHook>();
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	const FBarrierRequest Barrier(
		71,
		{TEXT("niagara.compile"), TEXT("niagara.stack")});

	NativeHook->SettleReceipt = FUeAgentNiagaraCompileSettleReceipt(
		71,
		false,
		0,
		0,
		0,
		0,
		{},
		{},
		TEXT(""),
		TEXT("fixture:p5-003:compile-pending"));
	FBarrierResult BarrierResult;
	FExecutionMutationError EffectError;
	TestFalse(
		TEXT("incomplete compile cannot become settled"),
		EffectHook->TryWaitForSettle(
			Barrier,
			*Resources,
			BarrierResult,
			EffectError));
	TestEqual(
		TEXT("compile failure has a stable code"),
		EffectError.GetCode(),
		FString(TEXT("niagara_compile_incomplete")));

	NativeHook->SettleReceipt = FUeAgentNiagaraCompileSettleReceipt(
		71,
		true,
		0,
		1,
		0,
		0,
		{TEXT("/Game/UAI/P5_003/NS_CompoundFixture")},
		{TEXT("niagara.compile"), TEXT("niagara.stack")},
		TEXT("niagara-fixture-generation-18:44"),
		TEXT("fixture:p5-003:stack-error"));
	EffectError = FExecutionMutationError();
	TestFalse(
		TEXT("a blocking Stack issue cannot become settled"),
		EffectHook->TryWaitForSettle(
			Barrier,
			*Resources,
			BarrierResult,
			EffectError));
	TestEqual(
		TEXT("Stack issue has a stable code"),
		EffectError.GetCode(),
		FString(TEXT("niagara_stack_issue")));

	NativeHook->SettleReceipt = FUeAgentNiagaraCompileSettleReceipt(
		71,
		true,
		0,
		0,
		0,
		0,
		{TEXT("/Game/UAI/P5_003/NS_CompoundFixture")},
		{TEXT("niagara.compile"), TEXT("niagara.stack")},
		TEXT("niagara-fixture-generation-18:44"),
		TEXT("fixture:p5-003:compile-and-stack-probe"));
	EffectError = FExecutionMutationError();
	TestTrue(
		TEXT("complete compile and clean Stack preserve exact settle facts"),
		EffectHook->TryWaitForSettle(
			Barrier,
			*Resources,
			BarrierResult,
			EffectError));
	TestTrue(
		TEXT("settle result is generation-bound"),
		BarrierResult.IsValid()
			&& BarrierResult.GetSubjectGeneration() == 71
			&& BarrierResult.IsSettled());
	TestEqual(
		TEXT("post-barrier revision comes from the UE probe"),
		BarrierResult.GetPostBarrierRevision(),
		FString(TEXT("niagara-fixture-generation-18:44")));

	const FUeAgentCanonicalValueRef Before = MakeDynamicInput(
		TEXT("Particles.Scale"),
		TEXT("/Niagara/Functions/Dynamic/MultiplyFloat"),
		{MakeLiteralInput(TEXT("A"), TEXT("1.0"), *Resources)},
		*Resources);
	const FUeAgentCanonicalValueRef After = MakeDynamicInput(
		TEXT("Particles.Scale"),
		TEXT("/Niagara/Functions/Dynamic/MultiplyFloat"),
		{MakeLiteralInput(TEXT("A"), TEXT("2.0"), *Resources)},
		*Resources);
	NativeHook->Captures = {
		MakeCapture(
			*Resources,
			43,
			TEXT("fixture:p5-003:before-read"),
			Before),
		MakeCapture(
			*Resources,
			44,
			TEXT("fixture:p5-003:fresh-post-settle-read"),
			After),
	};
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const FUeAgentStateReadRequest ReadRequest(
		MakeCandidateBinding(),
		EUeAgentStatePlane::Persisted);
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	TSharedPtr<const FUeAgentUeReadCapture> FirstCapture;
	TSharedPtr<const FUeAgentUeReadCapture> FreshCapture;
	FUeAgentGenericReadError ReadError;
	TestTrue(
		TEXT("before state is captured independently"),
		ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(10.0, Clock),
			Cancellation.GetToken(),
			FirstCapture,
			ReadError));
	TestTrue(
		TEXT("post-settle readback performs a fresh hook call"),
		ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(10.0, Clock),
			Cancellation.GetToken(),
			FreshCapture,
			ReadError));
	TestEqual(
		TEXT("readback is not mutation or request echo"),
		NativeHook->CaptureCalls,
		2);
	TestTrue(
		TEXT("fresh readback advances the observed revision"),
		FirstCapture.IsValid()
			&& FreshCapture.IsValid()
			&& FirstCapture->GetRevision().GetSequence() == 43
			&& FreshCapture->GetRevision().GetSequence() == 44);
	TestEqual(
		TEXT("fresh readback carries independent evidence"),
		FreshCapture->GetEvidenceId(),
		FString(TEXT("fixture:p5-003:fresh-post-settle-read")));
	return true;
}

