#include "Misc/AutomationTest.h"

#if __has_include("Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h")
#define UAI_T10_OPERATION_LIFECYCLE_AVAILABLE 1
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#else
#define UAI_T10_OPERATION_LIFECYCLE_AVAILABLE 0
#endif

#if UAI_T10_OPERATION_LIFECYCLE_AVAILABLE
namespace
{
	using namespace UE::UAI::BindingGateway::Operation;

	static FString Fingerprint(const TCHAR Fill)
	{
		return TEXT("sha256:") + FString::ChrN(64, Fill);
	}

	class FScriptedLifecyclePorts final
		: public IOperationVersionFencePort
		, public IOperationDomainMutationPort
		, public IOperationStabilityPort
		, public IOperationPersistencePort
		, public IOperationIndependentVerificationPort
	{
	public:
		bool bVersionCurrent = true;
		bool bExecuteSucceeds = true;
		bool bMutationStarted = true;
		bool bRollbackSupported = true;
		bool bSettleSucceeds = true;
		bool bPersistSucceeds = true;
		bool bVerifySucceeds = true;
		bool bEquivalent = true;
		bool bRollbackCallSucceeds = true;
		bool bRollbackConfirmed = true;
		TArray<FString> Calls;

		virtual bool Check(
			const FOperationLifecyclePlan& Plan,
			const EOperationLifecycleStage Stage,
			bool& bOutCurrent,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(Stage == EOperationLifecycleStage::PreExecuteFence
				? TEXT("fence:pre_execute")
				: TEXT("fence:pre_persist"));
			bOutCurrent = bVersionCurrent;
			if (!bVersionCurrent)
			{
				OutError = FOperationLifecycleError(
					TEXT("operation_version_conflict"));
			}
			return true;
		}

		virtual bool Execute(
			const FOperationLifecyclePlan& Plan,
			FOperationMutationFacts& OutFacts,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(TEXT("domain:execute"));
			if (bMutationStarted)
			{
				OutFacts = FOperationMutationFacts(
					TEXT("mutation:t10"),
					Plan.GetImpactScope(),
					bRollbackSupported);
			}
			if (!bExecuteSucceeds)
			{
				OutError = FOperationLifecycleError(
					TEXT("domain_execute_failed"));
			}
			return bExecuteSucceeds;
		}

		virtual bool Rollback(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			bool& bOutConfirmed,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(TEXT("domain:rollback"));
			bOutConfirmed = bRollbackConfirmed;
			if (!bRollbackCallSucceeds)
			{
				OutError = FOperationLifecycleError(
					TEXT("domain_rollback_failed"));
			}
			return bRollbackCallSucceeds;
		}

		virtual bool Settle(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(TEXT("stability:settle"));
			if (!bSettleSucceeds)
			{
				OutError = FOperationLifecycleError(
					TEXT("operation_settle_failed"));
			}
			return bSettleSucceeds;
		}

		virtual bool Persist(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			FString& OutPersistenceReceiptId,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(TEXT("persistence:persist"));
			if (!bPersistSucceeds)
			{
				OutError = FOperationLifecycleError(
					TEXT("operation_persist_failed"));
				return false;
			}
			OutPersistenceReceiptId = TEXT("persist:t10");
			return true;
		}

		virtual bool Verify(
			const FOperationLifecyclePlan& Plan,
			const FOperationMutationFacts& Facts,
			const FString& PersistenceReceiptId,
			bool& bOutEquivalent,
			FOperationLifecycleError& OutError) override
		{
			Calls.Add(TEXT("readback:verify"));
			bOutEquivalent = bEquivalent;
			if (!bVerifySucceeds)
			{
				OutError = FOperationLifecycleError(
					TEXT("operation_readback_failed"));
			}
			return bVerifySucceeds;
		}
	};

	static FOperationLifecyclePlan MakePlan()
	{
		return FOperationLifecyclePlan(
			TEXT("plan:t10"),
			Fingerprint(TEXT('a')),
			{TEXT("asset:/Game/A"), TEXT("asset:/Game/B")},
			{
				{TEXT("asset:/Game/A"), TEXT("revision:a:1")},
				{TEXT("asset:/Game/B"), TEXT("revision:b:1")},
			},
			{TEXT("compile"), TEXT("asset_registry")});
	}

	static FOperationImpactAuthorization MakeAuthorization()
	{
		return FOperationImpactAuthorization(
			Fingerprint(TEXT('a')),
			{TEXT("asset:/Game/A"), TEXT("asset:/Game/B")});
	}

	static TSharedRef<FOperationLifecycleService> MakeService(
		const TSharedRef<FScriptedLifecyclePorts>& Ports)
	{
		return MakeShared<FOperationLifecycleService>(
			StaticCastSharedRef<IOperationVersionFencePort>(Ports),
			StaticCastSharedRef<IOperationDomainMutationPort>(Ports),
			StaticCastSharedRef<IOperationStabilityPort>(Ports),
			StaticCastSharedRef<IOperationPersistencePort>(Ports),
			StaticCastSharedRef<IOperationIndependentVerificationPort>(Ports));
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T10SuccessOrderTest,
	"UAI.StrictV2.T10.OPERATION.LIFECYCLE.CORE.SuccessOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T10SuccessOrderTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T10_OPERATION_LIFECYCLE_AVAILABLE
	TestTrue(TEXT("T10 operation lifecycle service is implemented"), false);
	return false;
#else
	using namespace UE::UAI::BindingGateway::Operation;
	const TSharedRef<FScriptedLifecyclePorts> Ports =
		MakeShared<FScriptedLifecyclePorts>();
	const TSharedRef<FOperationLifecycleService> Service = MakeService(Ports);
	FOperationLifecycleReceipt Receipt;
	TestTrue(
		TEXT("operation service owns the complete successful lifecycle"),
		Service->Run(MakePlan(), MakeAuthorization(), Receipt));
	TestEqual(
		TEXT("verified persistence is the only success terminal"),
		Receipt.GetStatus(),
		EOperationLifecycleStatus::Succeeded);
	TestEqual(
		TEXT("service order is plan fence execute settle fence persist readback"),
		Ports->Calls,
		TArray<FString>{
			TEXT("fence:pre_execute"),
			TEXT("domain:execute"),
			TEXT("stability:settle"),
			TEXT("fence:pre_persist"),
			TEXT("persistence:persist"),
			TEXT("readback:verify"),
		});
	TestTrue(TEXT("successful receipt records mutation"), Receipt.DidMutationStart());
	TestFalse(TEXT("successful operation is not automatically replayable"), Receipt.IsRetrySafe());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T10PrewriteFenceTest,
	"UAI.StrictV2.T10.OPERATION.LIFECYCLE.CORE.AuthorizationAndVersionFence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T10PrewriteFenceTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T10_OPERATION_LIFECYCLE_AVAILABLE
	TestTrue(TEXT("T10 operation lifecycle service is implemented"), false);
	return false;
#else
	using namespace UE::UAI::BindingGateway::Operation;
	TSharedRef<FScriptedLifecyclePorts> Ports =
		MakeShared<FScriptedLifecyclePorts>();
	TSharedRef<FOperationLifecycleService> Service = MakeService(Ports);
	FOperationLifecycleReceipt Receipt;
	const FOperationImpactAuthorization NarrowAuthorization(
		Fingerprint(TEXT('a')),
		{TEXT("asset:/Game/A")});
	TestFalse(
		TEXT("authorization must cover the exact planned impact set"),
		Service->Run(MakePlan(), NarrowAuthorization, Receipt));
	TestEqual(
		TEXT("authorization mismatch is rejected before mutation"),
		Receipt.GetStatus(),
		EOperationLifecycleStatus::Rejected);
	TestTrue(TEXT("domain was not called"), Ports->Calls.IsEmpty());
	TestTrue(TEXT("prewrite rejection is retry safe"), Receipt.IsRetrySafe());

	Ports = MakeShared<FScriptedLifecyclePorts>();
	Ports->bVersionCurrent = false;
	Service = MakeService(Ports);
	TestFalse(
		TEXT("stale target revision is rejected before mutation"),
		Service->Run(MakePlan(), MakeAuthorization(), Receipt));
	TestEqual(
		TEXT("only the pre-execute fence ran"),
		Ports->Calls,
		TArray<FString>{TEXT("fence:pre_execute")});
	TestFalse(TEXT("no mutation started"), Receipt.DidMutationStart());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T10RollbackTest,
	"UAI.StrictV2.T10.OPERATION.LIFECYCLE.CORE.RollbackTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T10RollbackTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T10_OPERATION_LIFECYCLE_AVAILABLE
	TestTrue(TEXT("T10 operation lifecycle service is implemented"), false);
	return false;
#else
	using namespace UE::UAI::BindingGateway::Operation;
	const TSharedRef<FScriptedLifecyclePorts> Ports =
		MakeShared<FScriptedLifecyclePorts>();
	Ports->bSettleSucceeds = false;
	const TSharedRef<FOperationLifecycleService> Service = MakeService(Ports);
	FOperationLifecycleReceipt Receipt;
	TestFalse(
		TEXT("settle failure cannot report success"),
		Service->Run(MakePlan(), MakeAuthorization(), Receipt));
	TestEqual(
		TEXT("confirmed rollback has a distinct terminal"),
		Receipt.GetStatus(),
		EOperationLifecycleStatus::RolledBack);
	TestEqual(
		TEXT("rollback follows the failing mutation stage"),
		Ports->Calls,
		TArray<FString>{
			TEXT("fence:pre_execute"),
			TEXT("domain:execute"),
			TEXT("stability:settle"),
			TEXT("domain:rollback"),
		});
	TestTrue(TEXT("confirmed rollback permits an explicit retry"), Receipt.IsRetrySafe());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T10StateUnknownTest,
	"UAI.StrictV2.T10.OPERATION.LIFECYCLE.CORE.StateUnknownNoReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T10StateUnknownTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T10_OPERATION_LIFECYCLE_AVAILABLE
	TestTrue(TEXT("T10 operation lifecycle service is implemented"), false);
	return false;
#else
	using namespace UE::UAI::BindingGateway::Operation;
	const TSharedRef<FScriptedLifecyclePorts> Ports =
		MakeShared<FScriptedLifecyclePorts>();
	Ports->bPersistSucceeds = false;
	Ports->bRollbackCallSucceeds = false;
	Ports->bRollbackConfirmed = false;
	const TSharedRef<FOperationLifecycleService> Service = MakeService(Ports);
	FOperationLifecycleReceipt Receipt;
	TestFalse(
		TEXT("unconfirmed final state cannot report completion"),
		Service->Run(MakePlan(), MakeAuthorization(), Receipt));
	TestEqual(
		TEXT("failed rollback produces state_unknown"),
		Receipt.GetStatus(),
		EOperationLifecycleStatus::StateUnknown);
	TestFalse(TEXT("state_unknown is never replay safe"), Receipt.IsRetrySafe());
	TestTrue(TEXT("receipt preserves the failing stage"), Receipt.GetPhaseHistory().Contains(TEXT("rollback")));
	return true;
#endif
}
