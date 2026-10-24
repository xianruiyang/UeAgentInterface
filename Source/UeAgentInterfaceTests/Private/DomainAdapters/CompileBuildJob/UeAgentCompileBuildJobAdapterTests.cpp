#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

// The production header is intentionally absent in LS-002. LS-003 must force
// this translation unit to rebuild after adding it because a missing optional
// include cannot form a UBT dependency.
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobAdapter.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::CompileBuildJob;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentCanonicalValueRef MakeParameters(
		FUeAgentResourceTracker& Resources,
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items = {})
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			Items,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef MakeParametersWithString(
		FUeAgentResourceTracker& Resources,
		const FString& Field,
		const FString& Text)
	{
		FUeAgentCanonicalValuePtr StringValue;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			StringValue,
			Error));
		return MakeParameters(
			Resources,
			{{Field, StringValue.ToSharedRef()}});
	}

	FUeAgentCanonicalValueRef MakeParametersWithInteger(
		FUeAgentResourceTracker& Resources,
		const FString& Field,
		const FString& Lexeme)
	{
		FUeAgentCanonicalValuePtr IntegerValue;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryInteger(
			Lexeme,
			Resources,
			IntegerValue,
			Error));
		return MakeParameters(
			Resources,
			{{Field, IntegerValue.ToSharedRef()}});
	}

	FUeAgentCanonicalValueRef MakeNestedParametersWithString(
		FUeAgentResourceTracker& Resources,
		const FString& Field,
		const FString& Text)
	{
		return MakeParameters(
			Resources,
			{{TEXT("nested"),
			  MakeParametersWithString(Resources, Field, Text)}});
	}

	FUeAgentDescriptorIdentity MakeDescriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.compile_build_job.") + Suffix + TEXT("/v1"),
			1,
			TEXT(
				"sha256:8ed8eca1a02139e7824c880a87076561"
				"b8cb8073bc3530bc508721241641af71"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-002");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-002");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:ls-002:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
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

	FLongRunningOperationRequest MakeOperationRequest(
		const FString& Suffix,
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>& Resources,
		double& Now,
		double TimeoutSeconds = 30.0)
	{
		const FUeAgentDescriptorIdentity Descriptor =
			MakeDescriptor(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:ls-002:") + Suffix,
			TEXT("lease:ls-002:owner"),
			TEXT("server-session:ls-002"),
			Descriptor,
			MakeContext(Descriptor),
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				TimeoutSeconds,
				[&Now]() { return Now; }));
	}

	class FRecordingCompileBuildJobPort final
		: public ILongRunningOperationPort
	{
	public:
		FRecordingCompileBuildJobPort(
			EUeAgentCompileBuildJobFamily InFamily,
			int64 InGeneration)
			: Family(InFamily)
			, Generation(InGeneration)
		{
		}

		virtual bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			LastOperationId = Request.GetOperationId();
			OutIdentity = FLongRunningWorkIdentity(
				TEXT("resource:ls-002:") + LastOperationId,
				Generation);
			return true;
		}

		virtual bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			++ObserveCalls;
			if (QueuedSignal.IsSet())
			{
				FUeAgentCanonicalValuePtr Payload;
				FUeAgentStateSemanticError CanonicalError;
				if (!FUeAgentCanonicalValue::TryString(
						QueuedSignal->Payload,
						Resources,
						Payload,
						CanonicalError))
				{
					OutError = FExecutionMutationError(
						CanonicalError.Code,
						CanonicalError.Detail);
					return false;
				}
				OutObservation = FLongRunningWorkObservation(
					Generation,
					QueuedState,
					QueuedErrorCode,
					FLongRunningOperationSignal(
						QueuedSignal->TypeId,
						QueuedSignal->Revision,
						Payload.ToSharedRef()));
			}
			else
			{
				OutObservation = FLongRunningWorkObservation(
					Generation,
					QueuedState,
					QueuedErrorCode);
			}
			ResetObservation();
			return true;
		}

		virtual bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			++StopCalls;
			LastStopReason = Reason;
			return true;
		}

		virtual bool Join(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++JoinCalls;
			return true;
		}

		virtual bool Release(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			return true;
		}

		void EmitSignal(
			const FString& TypeId,
			int64 Revision,
			const FString& Payload)
		{
			QueuedState = ELongRunningWorkState::Running;
			QueuedErrorCode.Reset();
			QueuedSignal.Emplace(FQueuedSignal{
				TypeId,
				Revision,
				Payload,
			});
		}

		void EmitRunning()
		{
			ResetObservation();
		}

		void EmitExitedSuccess()
		{
			QueuedState = ELongRunningWorkState::ExitedSuccess;
			QueuedErrorCode.Reset();
			QueuedSignal.Reset();
		}

		EUeAgentCompileBuildJobFamily Family;
		int64 Generation = 0;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		FString LastOperationId;
		FString LastStopReason;

	private:
		struct FQueuedSignal
		{
			FString TypeId;
			int64 Revision = 0;
			FString Payload;
		};

		void ResetObservation()
		{
			QueuedState = ELongRunningWorkState::Running;
			QueuedErrorCode.Reset();
			QueuedSignal.Reset();
		}

		ELongRunningWorkState QueuedState =
			ELongRunningWorkState::Running;
		FString QueuedErrorCode;
		TOptional<FQueuedSignal> QueuedSignal;
	};

	class FRecordingCompileBuildJobPortFactory final
		: public IUeAgentCompileBuildJobPortFactory
	{
	public:
		virtual bool TryCreateOperationPort(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			TSharedPtr<
				ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			++Calls;
			LastFamily = Request.GetFamily();
			LastSubjectRef = Request.GetSubjectRef();
			LastPolicyId = Request.GetPolicyId();
			if (bFail)
			{
				OutError = FExecutionMutationError(
					TEXT("fixture_compile_build_job_factory_failed"));
				return false;
			}
			const TSharedRef<
				FRecordingCompileBuildJobPort,
				ESPMode::ThreadSafe> Port = MakeShared<
					FRecordingCompileBuildJobPort,
					ESPMode::ThreadSafe>(
					Request.GetFamily(),
					100 + Ports.Num());
			Ports.Add(Port);
			OutPort = Port;
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		EUeAgentCompileBuildJobFamily LastFamily =
			EUeAgentCompileBuildJobFamily::AssetCompile;
		FString LastSubjectRef;
		FString LastPolicyId;
		TArray<TSharedRef<
			FRecordingCompileBuildJobPort,
			ESPMode::ThreadSafe>> Ports;
	};

	struct FFamilyCase
	{
		EUeAgentCompileBuildJobFamily Family;
		FString SubjectRef;
		FString PolicyId;
		FString SignalTypeId;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobAdapterBehaviorTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.E1.Adapter."
		"OwnerSignalCancellationAndNoFallback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobAdapterBehaviorTest::RunTest(
	const FString& Parameters)
{
	TestEqual(
		TEXT("production adapter version"),
		UAI_COMPILE_BUILD_JOB_ADAPTER_V1,
		1);
	double Now = 100.0;
	const TSharedRef<
		FRecordingCompileBuildJobPort,
		ESPMode::ThreadSafe> UnexpectedDefaultPort = MakeShared<
			FRecordingCompileBuildJobPort,
			ESPMode::ThreadSafe>(
			EUeAgentCompileBuildJobFamily::AssetCompile,
			9000);
	FLongRunningOperationOwner Owner(
		UnexpectedDefaultPort,
		[&Now]() { return Now; });
	FRecordingCompileBuildJobPortFactory Factory;
	const TArray<FFamilyCase> Cases = {
		{EUeAgentCompileBuildJobFamily::AssetCompile,
		 TEXT("asset:/Game/Fixture/BP_Compile"),
		 TEXT("policy:asset-compile:default/v1"),
		 TEXT("uai.compile_build.asset_compile.progress/v1")},
		{EUeAgentCompileBuildJobFamily::WorldBuild,
		 TEXT("world:/Game/Fixture/BuildMap"),
		 TEXT("policy:world-build:navmesh/v1"),
		 TEXT("uai.compile_build.world_build.progress/v1")},
		{EUeAgentCompileBuildJobFamily::ExternalProcess,
		 TEXT("project:fixture"),
		 TEXT("policy:owned-tool:packaging/v1"),
		 TEXT("uai.compile_build.external_process.report_ref/v1")},
		{EUeAgentCompileBuildJobFamily::WorldPartitionResave,
		 TEXT("world:/Game/Fixture/PartitionedMap"),
		 TEXT("policy:world-partition-resave:actors/v1"),
		 TEXT("uai.compile_build.world_partition_resave.progress/v1")},
	};

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FFamilyCase& Case = Cases[Index];
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources = MakeResources();
		const FUeAgentCompileBuildJobTypedRequest TypedRequest(
			Case.Family,
			Case.SubjectRef,
			Case.PolicyId,
			MakeParameters(*Resources));
		TestTrue(TEXT("typed family request is valid"),
			TypedRequest.IsValid());
		const FString Suffix = TEXT("family-") + FString::FromInt(Index);
		const FLongRunningOperationRequest OperationRequest =
			MakeOperationRequest(Suffix, Resources, Now);
		FLongRunningOperationSnapshot Snapshot;
		FExecutionMutationError Error;
		const int32 FactoryCallsBefore = Factory.Calls;
		TestTrue(
			TEXT("Adapter accepts one operation-scoped family port"),
			TryAcceptCompileBuildJob(
				TypedRequest,
				OperationRequest,
				Factory,
				Owner,
				Snapshot,
				Error));
		TestEqual(TEXT("factory called exactly once"),
			Factory.Calls - FactoryCallsBefore, 1);
		TestEqual(TEXT("exact family reaches factory"),
			Factory.LastFamily, Case.Family);
		TestEqual(TEXT("semantic subject reaches factory"),
			Factory.LastSubjectRef, Case.SubjectRef);
		TestEqual(TEXT("internal policy ref reaches factory"),
			Factory.LastPolicyId, Case.PolicyId);
		TestEqual(TEXT("accept remains non-terminal"),
			Snapshot.GetPhase(), ELongRunningOperationPhase::Accepted);
		TestEqual(TEXT("accept does not start native work"),
			Factory.Ports.Last()->StartCalls, 0);
		TestEqual(TEXT("central owner default port is never used"),
			UnexpectedDefaultPort->StartCalls, 0);

		const TSharedRef<
			FRecordingCompileBuildJobPort,
			ESPMode::ThreadSafe> Port = Factory.Ports.Last();
		TestTrue(TEXT("first advance starts the dedicated port"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				0,
				Snapshot,
				Error));
		TestEqual(TEXT("dedicated port starts once"), Port->StartCalls, 1);
		Port->EmitSignal(
			Case.SignalTypeId,
			1,
			TEXT("artifact:ls-002:progress:") + FString::FromInt(Index));
		TestTrue(TEXT("current generation signal is observed"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation,
				Snapshot,
				Error));
		TestTrue(TEXT("snapshot exposes family signal"),
			Snapshot.GetLatestSignal().IsSet());
		if (Snapshot.GetLatestSignal().IsSet())
		{
			TestEqual(TEXT("family signal type is exact"),
				Snapshot.GetLatestSignal()->GetTypeId(),
				Case.SignalTypeId);
			TestEqual(TEXT("family signal revision starts at one"),
				Snapshot.GetLatestSignal()->GetRevision(), int64(1));
			TestEqual(TEXT("family signal payload stays canonical"),
				Snapshot.GetLatestSignal()->GetPayload()->GetKind(),
				EUeAgentValueKind::String);
		}
		FLongRunningOperationSnapshot UnauthorizedSnapshot;
		TestFalse(TEXT("wrong session cannot read typed signal"),
			Owner.GetSnapshot(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				TEXT("server-session:other"),
				UnauthorizedSnapshot,
				Error));
		TestFalse(TEXT("failed read leaks no signal"),
			UnauthorizedSnapshot.GetLatestSignal().IsSet());

		Port->EmitExitedSuccess();
		TestTrue(TEXT("physical exit joins and releases"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation,
				Snapshot,
				Error));
		TestTrue(TEXT("operation is terminal only after physical exit"),
			Snapshot.IsTerminal());
		TestEqual(TEXT("family port joins once"), Port->JoinCalls, 1);
		TestEqual(TEXT("family port releases once"), Port->ReleaseCalls, 1);
		FLongRunningOperationReceipt Receipt;
		TestTrue(TEXT("terminal receipt is lifecycle-only"),
			Owner.GetTerminalReceipt(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Receipt,
				Error));
		TestEqual(TEXT("native success does not come from signal"),
			Receipt.GetStatus(), ELongRunningOperationStatus::Succeeded);
		TestTrue(TEXT("caller explicitly releases retained receipt"),
			Owner.ReleaseTerminal(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Error));
	}

	const int32 FactoryCallsBeforeInvalid = Factory.Calls;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		InvalidLocalizationTimeoutResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest InvalidLocalizationTimeoutRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("localization:Game"),
		TEXT("policy:owned-tool:localization-gather-text/v1"),
		MakeParametersWithString(
			*InvalidLocalizationTimeoutResources,
			TEXT("timeout_seconds"),
			TEXT("120")));
	TestFalse(
		TEXT("localization timeout must remain a numeric value"),
		InvalidLocalizationTimeoutRequest.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		UnownedTimeoutResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest UnownedTimeoutRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:fixture"),
		TEXT("policy:owned-tool:packaging/v1"),
		MakeParametersWithInteger(
			*UnownedTimeoutResources,
			TEXT("timeout_seconds"),
			TEXT("120")));
	TestFalse(
		TEXT("other owned-tool policies cannot gain a caller-controlled timeout"),
		UnownedTimeoutRequest.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		NestedLocalizationTimeoutResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest NestedLocalizationTimeoutRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("localization:Game"),
		TEXT("policy:owned-tool:localization-compile-text/v1"),
		MakeParameters(
			*NestedLocalizationTimeoutResources,
			{{TEXT("nested"), MakeParametersWithInteger(
				*NestedLocalizationTimeoutResources,
				TEXT("timeout_seconds"),
				TEXT("120"))}}));
	TestFalse(
		TEXT("the localization timeout exemption is top-level only"),
		NestedLocalizationTimeoutRequest.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		InvalidResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest InvalidRawRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:fixture"),
		TEXT("policy:owned-tool:packaging/v1"),
		MakeParametersWithString(
			*InvalidResources,
			TEXT("executable"),
			TEXT("C:/arbitrary/tool.exe")));
	TestFalse(TEXT("raw executable is not a valid domain request"),
		InvalidRawRequest.IsValid());
	const FLongRunningOperationRequest InvalidOperationRequest =
		MakeOperationRequest(
			TEXT("invalid-raw-executable"),
			InvalidResources,
			Now);
	FLongRunningOperationSnapshot Snapshot;
	FExecutionMutationError Error;
	TestFalse(TEXT("invalid request fails before factory and owner"),
		TryAcceptCompileBuildJob(
			InvalidRawRequest,
			InvalidOperationRequest,
			Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(TEXT("invalid request has stable failure code"),
		Error.GetCode(),
		FString(TEXT("compile_build_job_request_forbidden_field")));
	TestEqual(TEXT("invalid request never calls factory"),
		Factory.Calls, FactoryCallsBeforeInvalid);

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		NestedInvalidResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest NestedInvalidRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:fixture"),
		TEXT("policy:owned-tool:packaging/v1"),
		MakeNestedParametersWithString(
			*NestedInvalidResources,
			TEXT("executable"),
			TEXT("C:/arbitrary/nested-tool.exe")));
	TestFalse(TEXT("nested raw executable cannot bypass validation"),
		NestedInvalidRequest.IsValid());
	TestFalse(TEXT("nested forbidden field fails before factory"),
		TryAcceptCompileBuildJob(
			NestedInvalidRequest,
			MakeOperationRequest(
				TEXT("invalid-nested-executable"),
				NestedInvalidResources,
				Now),
			Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(TEXT("nested field has the same stable failure code"),
		Error.GetCode(),
		FString(TEXT("compile_build_job_request_forbidden_field")));
	TestEqual(TEXT("nested invalid request never calls factory"),
		Factory.Calls, FactoryCallsBeforeInvalid);

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		InvalidFamilyResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest InvalidFamilyRequest(
		static_cast<EUeAgentCompileBuildJobFamily>(255),
		TEXT("asset:/Game/Fixture/BP_InvalidFamily"),
		TEXT("policy:asset-compile:default/v1"),
		MakeParameters(*InvalidFamilyResources));
	TestFalse(TEXT("unknown family is not a valid typed request"),
		InvalidFamilyRequest.IsValid());
	TestFalse(TEXT("unknown family fails before factory"),
		TryAcceptCompileBuildJob(
			InvalidFamilyRequest,
			MakeOperationRequest(
				TEXT("invalid-family"),
				InvalidFamilyResources,
				Now),
			Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(TEXT("unknown family has a stable failure code"),
		Error.GetCode(),
		FString(TEXT("compile_build_job_family_invalid")));
	TestEqual(TEXT("unknown family never calls factory"),
		Factory.Calls, FactoryCallsBeforeInvalid);

	FRecordingCompileBuildJobPortFactory FailingFactory;
	FailingFactory.bFail = true;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		FailureResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest FactoryFailureRequest(
		EUeAgentCompileBuildJobFamily::AssetCompile,
		TEXT("asset:/Game/Fixture/BP_FactoryFailure"),
		TEXT("policy:asset-compile:default/v1"),
		MakeParameters(*FailureResources));
	const FLongRunningOperationRequest FactoryFailureOperation =
		MakeOperationRequest(
			TEXT("factory-failure"),
			FailureResources,
			Now);
	TestFalse(TEXT("factory failure is not hidden by default port"),
		TryAcceptCompileBuildJob(
			FactoryFailureRequest,
			FactoryFailureOperation,
			FailingFactory,
			Owner,
			Snapshot,
			Error));
	TestEqual(TEXT("factory failure remains stable"),
		Error.GetCode(),
		FString(TEXT("fixture_compile_build_job_factory_failed")));
	TestFalse(TEXT("failed factory creates no owner entry"),
		Owner.GetSnapshot(
			FactoryFailureOperation.GetOperationId(),
			FactoryFailureOperation.GetOwnerLeaseId(),
			FactoryFailureOperation.GetServerSessionId(),
			Snapshot,
			Error));
	TestEqual(TEXT("central default port still remains unused"),
		UnexpectedDefaultPort->StartCalls, 0);

	double CancelNow = 300.0;
	const TSharedRef<
		FRecordingCompileBuildJobPort,
		ESPMode::ThreadSafe> CancelDefaultPort = MakeShared<
			FRecordingCompileBuildJobPort,
			ESPMode::ThreadSafe>(
			EUeAgentCompileBuildJobFamily::AssetCompile,
			9100);
	FLongRunningOperationOwner CancelOwner(
		CancelDefaultPort,
		[&CancelNow]() { return CancelNow; });
	FRecordingCompileBuildJobPortFactory CancelFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		CancelResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest ResaveRequest(
		EUeAgentCompileBuildJobFamily::WorldPartitionResave,
		TEXT("world:/Game/Fixture/PartitionedCancelMap"),
		TEXT("policy:world-partition-resave:actors/v1"),
		MakeParameters(*CancelResources));
	const FLongRunningOperationRequest ResaveOperation =
		MakeOperationRequest(
			TEXT("resave-cancel"),
			CancelResources,
			CancelNow);
	TestTrue(TEXT("resave operation accepts"),
		TryAcceptCompileBuildJob(
			ResaveRequest,
			ResaveOperation,
			CancelFactory,
			CancelOwner,
			Snapshot,
			Error));
	const TSharedRef<
		FRecordingCompileBuildJobPort,
		ESPMode::ThreadSafe> ResavePort = CancelFactory.Ports.Last();
	TestTrue(TEXT("resave operation starts"),
		CancelOwner.Advance(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	ResavePort->EmitSignal(
		TEXT("uai.compile_build.world_partition_resave.progress/v1"),
		1,
		TEXT("artifact:ls-002:resave-progress"));
	TestTrue(TEXT("resave progress is observable"),
		CancelOwner.Advance(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			ResavePort->Generation,
			Snapshot,
			Error));
	TestTrue(TEXT("resave cancellation requests safe stop"),
		CancelOwner.RequestCancellation(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			TEXT("caller_cancelled"),
			Snapshot,
			Error));
	TestEqual(TEXT("resave stop delivered once"), ResavePort->StopCalls, 1);
	TestFalse(TEXT("stop request does not fabricate terminal"),
		Snapshot.IsTerminal());
	ResavePort->EmitRunning();
	TestTrue(TEXT("running-after-stop remains observable"),
		CancelOwner.Advance(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			ResavePort->Generation,
			Snapshot,
			Error));
	TestFalse(TEXT("running-after-stop remains non-terminal"),
		Snapshot.IsTerminal());
	ResavePort->EmitExitedSuccess();
	TestTrue(TEXT("resave exit reaches join/release"),
		CancelOwner.Advance(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			ResavePort->Generation,
			Snapshot,
			Error));
	TestEqual(TEXT("cancelled resave has lifecycle status"),
		Snapshot.GetStatus(), ELongRunningOperationStatus::Cancelled);
	TestEqual(TEXT("cancelled resave joins once"), ResavePort->JoinCalls, 1);
	TestEqual(TEXT("cancelled resave releases once"),
		ResavePort->ReleaseCalls, 1);
	const int32 ObservationsAtTerminal = ResavePort->ObserveCalls;
	TestFalse(TEXT("terminal resave rejects late continuation"),
		CancelOwner.Advance(
			ResaveOperation.GetOperationId(),
			ResaveOperation.GetOwnerLeaseId(),
			ResaveOperation.GetServerSessionId(),
			ResavePort->Generation,
			Snapshot,
			Error));
	TestEqual(TEXT("late continuation performs no native observation"),
		ResavePort->ObserveCalls, ObservationsAtTerminal);

	double DeadlineNow = 500.0;
	const TSharedRef<
		FRecordingCompileBuildJobPort,
		ESPMode::ThreadSafe> DeadlineDefaultPort = MakeShared<
			FRecordingCompileBuildJobPort,
			ESPMode::ThreadSafe>(
			EUeAgentCompileBuildJobFamily::AssetCompile,
			9200);
	FLongRunningOperationOwner DeadlineOwner(
		DeadlineDefaultPort,
		[&DeadlineNow]() { return DeadlineNow; });
	FRecordingCompileBuildJobPortFactory DeadlineFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeadlineResources = MakeResources();
	const FUeAgentCompileBuildJobTypedRequest DeadlineRequest(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:fixture"),
		TEXT("policy:owned-tool:packaging/v1"),
		MakeParameters(*DeadlineResources));
	const FLongRunningOperationRequest DeadlineOperation =
		MakeOperationRequest(
			TEXT("deadline-before-start"),
			DeadlineResources,
			DeadlineNow,
			1.0);
	TestTrue(TEXT("deadline fixture accepts"),
		TryAcceptCompileBuildJob(
			DeadlineRequest,
			DeadlineOperation,
			DeadlineFactory,
			DeadlineOwner,
			Snapshot,
			Error));
	DeadlineNow = 502.0;
	TestTrue(TEXT("expired pre-start deadline settles without start"),
		DeadlineOwner.Advance(
			DeadlineOperation.GetOperationId(),
			DeadlineOperation.GetOwnerLeaseId(),
			DeadlineOperation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestTrue(TEXT("pre-start deadline is terminal"), Snapshot.IsTerminal());
	TestEqual(TEXT("pre-start deadline is cancelled"),
		Snapshot.GetStatus(), ELongRunningOperationStatus::Cancelled);
	TestEqual(TEXT("expired deadline starts no native process"),
		DeadlineFactory.Ports.Last()->StartCalls, 0);
	TestEqual(TEXT("expired deadline uses no default fallback"),
		DeadlineDefaultPort->StartCalls, 0);

	return !HasAnyErrors();
}
