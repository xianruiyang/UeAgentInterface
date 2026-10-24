#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringApplication.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"


namespace
{
	static_assert(UAI_SIMPLE_AUTHORING_APPLICATION_V1 == 1);

	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	FUeAgentDescriptorIdentity TestDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.fixture.asset_apply"),
			1,
			TEXT(
				"sha256:d5d77b007fce21ae3743d655c3bc324e"
				"f69d3f08f899faa0be224b7bea6695fe"));
	}

	bool TryCreateTestRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		return FUeAgentDescriptorRegistry::TryCreate(
			{
				FUeAgentOperationDescriptor(
					TestDescriptorIdentity(),
					TEXT("fixture.asset_apply"),
					TEXT("reference"),
					EUeAgentDescriptorOperationKind::Operation,
					FUeAgentExecutionContextPolicy(
						{
							EUeAgentStatePlane::AuthoringMemory,
							EUeAgentStatePlane::Persisted,
						},
						{},
						EUeAgentContextResolutionPolicy::
							ExplicitOnly,
						EUeAgentTransientContextPolicy::
							ReadOnly),
					FUeAgentContractValue(
						TEXT(
							"{\"required\":[\"object_path\"],"
							"\"type\":\"object\"}")),
					FUeAgentContractValue(
						TEXT("{\"type\":\"object\"}"))),
			},
			OutRegistry,
			OutError);
	}

	FUeAgentSimpleAuthoringCommandRoute TestRoute()
	{
		return FUeAgentSimpleAuthoringCommandRoute(
			TEXT("fixture.asset_apply"),
			TEXT("fixture.asset_apply"),
			TEXT("reference"),
			TEXT("asset_apply"),
			TEXT("reference"),
			TestDescriptorIdentity());
	}

	FUeAgentExecutionContextInput TestExecutionContext()
	{
		FUeAgentExecutionContextInput Result;
		Result.ProjectInstanceId =
			TEXT("project:sa-008-application");
		Result.EditorInstanceId =
			TEXT("editor:sa-008-application");
		Result.StatePlane = EUeAgentStatePlane::Persisted;
		Result.ContextRevision =
			TEXT("context:sa-008-application:1");
		Result.bResolvedFromCurrent = false;
		return Result;
	}

	FUeAgentCanonicalIdentity TestTargetIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.test.simple-authoring.value"),
			TEXT("simple_authoring_fixture"),
			TEXT("/Game/UAI/SA_008/ApplicationFixture"),
			TEXT("value"),
			TEXT("fixture_property"),
			TEXT("sa-008-generation"));
	}

	FUeAgentCanonicalValueRef MakeString(
		const FString& Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Value,
			Resources,
			Result,
			Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("read-port:sa-008-application"),
			TEXT("sa-008-generation"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		Entries.Emplace(
			TestTargetIdentity(),
			FUeAgentCanonicalType(
				TEXT("string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			MakeString(TEXT("before"), Resources),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("sa-008-default"),
				TEXT("recording fixture"),
				MakeString(TEXT(""), Resources)),
			FUeAgentStateDetails(
				TEXT("value"),
				TEXT(""),
				{TEXT("SA-008")},
				true,
				TEXT("available"),
				{}),
			Revision);
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:sa-008-application:before"),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("sa-008-clock"),
				TEXT("1")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("read-port:sa-008-application"),
				TEXT("evidence:sa-008-application:before")),
			Entries,
			Resources,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	FUeAgentCandidateBinding MakeCandidateBinding()
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:sa-008-application"),
			FUeAgentCandidateSummary(
				TestTargetIdentity(),
				TestDescriptorIdentity(),
				TEXT("simple_authoring_fixture"),
				TEXT("SA-008 application fixture"),
				TEXT("recording lifecycle target"),
				{TEXT("SA-008")}),
			FUeAgentCatalogRevision(
				TEXT("catalog:sa-008-application"),
				TEXT("sa-008-generation"),
				1),
			TEXT("/Game/UAI/SA_008/ApplicationFixture.value"),
			TEXT("read-port:sa-008-application"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Update,
			},
			TEXT("evidence:sa-008-application:binding"));
	}

	class FRecordingPlanner final
		: public IUeAgentOperationPlanner
	{
	public:
		bool TryPlan(
			const FPlanningRequest& Request,
			const FUeAgentOperationContext& Context,
			FPlanningResult& OutResult,
			FUeAgentOperationBindingError& OutError) override
		{
			++CallCount;
			SeenProjectId =
				Context.GetExecutionContext().GetProjectInstanceId();
			FPlanningPreflightError Error;
			if (!TryPlanChanges(
				Request,
				Context.GetResourceTracker(),
				OutResult,
				Error))
			{
			OutError = Error;
				return false;
			}
			return true;
		}

		int32 CallCount = 0;
		FString SeenProjectId;
	};

	class FRecordingExecutor final
		: public IUeAgentOperationExecutor
	{
	public:
		bool TryExecute(
			const FPlanningResult& Planning,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FExecutionReceipt& OutReceipt,
			FUeAgentOperationBindingError& OutError) override
		{
			++CallCount;
			SeenTargetCount = Targets.Num();
			SeenProjectId =
				Context.GetExecutionContext().GetProjectInstanceId();
			OutReceipt = FExecutionReceipt(
				TEXT("operation:sa-008-application"),
				EExecutionPhase::Terminal,
				bFail
					? EExecutionStatus::Failed
					: EExecutionStatus::PersistedUnverified,
				bFail ? TEXT("fixture_execution_failed") : FString(),
				EExecutionCancellationMode::NotRequested,
				true,
				1,
				false,
				{},
				{
					TEXT("queued"),
					TEXT("executing"),
					TEXT("persisting"),
					TEXT("terminal"),
				});
			return Planning.IsReady() && OutReceipt.IsValid();
		}

		int32 CallCount = 0;
		int32 SeenTargetCount = 0;
		FString SeenProjectId;
		bool bFail = false;
	};

	class FRecordingVerifier final
		: public IUeAgentOperationVerifier
	{
	public:
		bool TryVerify(
			const FExecutionReceipt& Receipt,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FVerificationOutcome& OutOutcome,
			FUeAgentOperationBindingError& OutError) override
		{
			++CallCount;
			SeenTargetCount = Targets.Num();
			SeenProjectId =
				Context.GetExecutionContext().GetProjectInstanceId();
			OutOutcome = FVerificationOutcome(
				TEXT("verification:sa-008-application"),
				bMismatch
					? EVerificationStatus::Mismatch
					: EVerificationStatus::Verified,
				bMismatch
					? EEffectOutcome::Partial
					: EEffectOutcome::Complete,
				bMismatch
					? TEXT("fixture_verification_mismatch")
					: FString(),
				false,
				{},
				{TEXT("evidence:sa-008-independent-readback")},
				{});
			return Receipt.IsValid();
		}

		int32 CallCount = 0;
		int32 SeenTargetCount = 0;
		FString SeenProjectId;
		bool bMismatch = false;
	};

	class FRecordingProvider final
		: public IUeAgentSimpleAuthoringCapabilityProvider
	{
	public:
		FRecordingProvider(
			TSharedRef<FRecordingPlanner> InPlanner,
			TSharedRef<FRecordingExecutor> InExecutor,
			TSharedRef<FRecordingVerifier> InVerifier)
			: Planner(MoveTemp(InPlanner))
			, Executor(MoveTemp(InExecutor))
			, Verifier(MoveTemp(InVerifier))
		{
		}

		const FString& GetFamilyId() const override
		{
			return FamilyId;
		}

		TSharedRef<IUeAgentOperationPlanner>
		GetPlanner() override
		{
			return Planner;
		}

		TSharedRef<IUeAgentOperationExecutor>
		GetExecutor() override
		{
			return Executor;
		}

		TSharedRef<IUeAgentOperationVerifier>
		GetVerifier() override
		{
			return Verifier;
		}

		bool TryPrepare(
			const FUeAgentOperationRequest& CanonicalRequest,
			const FUeAgentExecutionContextRef& Context,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TOptional<FUeAgentSimpleAuthoringPreparedOperation>&
				OutPrepared,
			FUeAgentSimpleAuthoringApplicationError& OutError)
			override
		{
			++PrepareCount;
			SeenPayload =
				CanonicalRequest.GetPayload().GetJsonValue();
			SeenProjectId = Context.GetProjectInstanceId();
			const FUeAgentCanonicalSnapshotRef Snapshot =
				MakeSnapshot(Resources);
			const FChangeDraft Change(
				TestTargetIdentity(),
				EChangeIntent::Update,
				Snapshot->GetEntries()[0].GetValue(),
				MakeString(TEXT("after"), Resources),
				{TEXT("uai:test:write")},
				TEXT("impact:sa-008-application"),
				{TestTargetIdentity()});
			OutPrepared.Emplace(
				TArray<FUeAgentCandidateBinding>{
					MakeCandidateBinding(),
				},
				FPlanningRequest(
					FChangeSetDraft(
						CanonicalRequest.GetDescriptorIdentity(),
						CanonicalRequest.GetRequestId(),
						CanonicalRequest.GetIdempotencyKey(),
						Snapshot,
						{Change}),
					CallerContext,
					false),
				FUeAgentProjectionRequest(
					FUeAgentProjectionProfile(
						TEXT("compact"),
						false,
						false,
						false),
					{},
					{}));
			return OutPrepared.IsSet()
				&& OutPrepared->IsValid();
		}

		int32 PrepareCount = 0;
		FString SeenPayload;
		FString SeenProjectId;

	private:
		const FString FamilyId = TEXT("reference");
		TSharedRef<FRecordingPlanner> Planner;
		TSharedRef<FRecordingExecutor> Executor;
		TSharedRef<FRecordingVerifier> Verifier;
	};

	class FRecordingTokenEstimator final
		: public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				24,
				96);
			return true;
		}
	};

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringUnifiedLifecycleTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.UnifiedLifecycle",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringUnifiedLifecycleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingPlanner> Planner =
		MakeShared<FRecordingPlanner>();
	const TSharedRef<FRecordingExecutor> Executor =
		MakeShared<FRecordingExecutor>();
	const TSharedRef<FRecordingVerifier> Verifier =
		MakeShared<FRecordingVerifier>();
	const TSharedRef<FRecordingProvider> Provider =
		MakeShared<FRecordingProvider>(
			Planner,
			Executor,
			Verifier);

	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("server ticket service is available"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa-008-application-integrity-key"),
			30.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT("ticket:sa-008-application"));
			},
			TicketService,
			TicketError));

	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	TestTrue(
		TEXT("C10 recording projector is available"),
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FRecordingTokenEstimator>(),
			Projector,
			ProjectionError));
	if (!TicketService.IsValid() || !Projector.IsValid())
	{
		return false;
	}

	FUeAgentSimpleAuthoringApplicationConfig Config;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(
		TEXT("recording descriptor registry is valid"),
		TryCreateTestRegistry(Registry, RegistryError));
	if (!Registry.IsValid())
	{
		AddError(
			TEXT(
				"simple_authoring_test_registry_invalid:")
			+ RegistryError);
		return false;
	}
	Config.DescriptorRegistry = Registry;
	Config.ExecutionContext = TestExecutionContext();
	Config.ServerSessionId =
		TEXT("session:sa-008-application");
	Config.RegistryEpoch = 1;


	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	TestTrue(
		TEXT("one exact route composes one C3O binding"),
		FUeAgentSimpleAuthoringApplication::TryCreate(
			Config,
			{TestRoute()},
			{
				StaticCastSharedRef<
					IUeAgentSimpleAuthoringCapabilityProvider>(
						Provider),
			},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Application,
			ApplicationError));
	if (!Application.IsValid())
	{
		AddError(
			TEXT("simple_authoring_application_create_failed:")
			+ ApplicationError.Code
			+ TEXT(":")
			+ ApplicationError.PublicMessage);
		return false;
	}
	TestEqual(
		TEXT("application owns exactly one immutable binding"),
		Application->GetBindingCount(),
		1);

	const FUeAgentCallerContext Caller(
		TEXT("sa-008-recording-caller"),
		{TEXT("uai:test:write")},
		TEXT("sa-008-recording-correlation"));
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();

	const FUeAgentOperationResult InvalidResult =
		Application->Submit(
			FUeAgentOperationRequest(
				TestDescriptorIdentity(),
				TEXT("request:sa-008-invalid"),
				TEXT("idempotency:sa-008-invalid"),
				FUeAgentContractValue(TEXT("{}"))),
			Caller,
			Cancellation);
	TestTrue(
		TEXT("C0 rejects missing required input"),
		InvalidResult.GetValue()
			.GetJsonValue()
			.Contains(TEXT("operation_error")));
	TestEqual(
		TEXT("invalid input never reaches family provider"),
		Provider->PrepareCount,
		0);

	const FUeAgentOperationResult Result =
		Application->Submit(
			FUeAgentOperationRequest(
				TestDescriptorIdentity(),
				TEXT("request:sa-008-valid"),
				TEXT("idempotency:sa-008-valid"),
				FUeAgentContractValue(
					TEXT(
						"{  \"object_path\" : "
						"\"/Game/UAI/SA_008/"
						"ApplicationFixture\" }"))),
			Caller,
			Cancellation);
	TestFalse(
		TEXT("valid lifecycle returns no operation error"),
		Result.GetValue()
			.GetJsonValue()
			.Contains(TEXT("operation_error")));
	TestTrue(
		TEXT("valid lifecycle returns the C3O operation id"),
		Result.GetValue()
			.GetJsonValue()
			.Contains(
				TEXT("operation:sa-008-application")));
	TestEqual(
		TEXT("exact family provider is selected once"),
		Provider->PrepareCount,
		1);
	TestEqual(
		TEXT("provider receives C0 canonical payload"),
		Provider->SeenPayload,
		FString(
			TEXT(
				"{\"object_path\":\"/Game/UAI/SA_008/"
				"ApplicationFixture\"}")));
	TestEqual(
		TEXT("planner executes once"),
		Planner->CallCount,
		1);
	TestEqual(
		TEXT("executor executes once"),
		Executor->CallCount,
		1);
	TestEqual(
		TEXT("verifier executes once"),
		Verifier->CallCount,
		1);
	TestEqual(
		TEXT("one server-ticketed target reaches execution"),
		Executor->SeenTargetCount,
		1);
	TestEqual(
		TEXT("one target reaches independent verifier"),
		Verifier->SeenTargetCount,
		1);
	TestEqual(
		TEXT("provider receives the explicit context"),
		Provider->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);
	TestEqual(
		TEXT("planner receives the same explicit context"),
		Planner->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);
	TestEqual(
		TEXT("executor receives the same explicit context"),
		Executor->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);
	TestEqual(
		TEXT("verifier receives the same explicit context"),
		Verifier->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);

	Executor->bFail = true;
	const FUeAgentOperationResult FailedResult =
		Application->Submit(
			FUeAgentOperationRequest(
				TestDescriptorIdentity(),
				TEXT("request:sa-008-execution-failed"),
				TEXT("idempotency:sa-008-execution-failed"),
				FUeAgentContractValue(
					TEXT(
						"{\"object_path\":\"/Game/UAI/SA_008/"
						"ApplicationFixture\"}"))),
			Caller,
			Cancellation);
	TestTrue(
		TEXT("terminal execution failure preserves its error code"),
		FailedResult.GetValue().GetJsonValue().Contains(
			TEXT("fixture_execution_failed")));
	TestEqual(
		TEXT("terminal execution failure does not invoke verification"),
		Verifier->CallCount,
		1);

	Executor->bFail = false;
	Verifier->bMismatch = true;
	const FUeAgentOperationResult MismatchResult =
		Application->Submit(
			FUeAgentOperationRequest(
				TestDescriptorIdentity(),
				TEXT("request:sa-008-verification-mismatch"),
				TEXT("idempotency:sa-008-verification-mismatch"),
				FUeAgentContractValue(
					TEXT(
						"{\"object_path\":\"/Game/UAI/SA_008/"
						"ApplicationFixture\"}"))),
			Caller,
			Cancellation);
	TestTrue(
		TEXT("verification mismatch preserves its diagnostic code"),
		MismatchResult.GetValue().GetJsonValue().Contains(
			TEXT("fixture_verification_mismatch")));
	TestEqual(
		TEXT("verification mismatch is evaluated exactly once"),
		Verifier->CallCount,
		2);
	return true;
}
