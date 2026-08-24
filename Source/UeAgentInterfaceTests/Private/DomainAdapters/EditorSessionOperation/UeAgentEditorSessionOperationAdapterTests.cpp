#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

// LS-004 intentionally landed before the production Adapter. This LS-005
// marker forces the translation unit to rebuild after adding the header.
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationAdapter.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::EditorSessionOperation;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					16 * 1024 * 1024},
			}));
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

	FUeAgentCanonicalValueRef MakeString(
		FUeAgentResourceTracker& Resources,
		const FString& Text)
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

	FUeAgentCanonicalValueRef MakeDeepParameters(
		FUeAgentResourceTracker& Resources,
		int32 Depth,
		const FString& LeafField)
	{
		FUeAgentCanonicalValueRef Current = MakeParameters(
			Resources,
			{{LeafField, MakeString(Resources, TEXT("fixture"))}});
		for (int32 Index = 0; Index < Depth; ++Index)
		{
			Current = MakeParameters(
				Resources,
				{{TEXT("nested_") + FString::FromInt(Index), Current}});
		}
		return Current;
	}

	FUeAgentDescriptorIdentity MakeDescriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.editor_session_operation.")
				+ Suffix + TEXT("/v1"),
			1,
			TEXT(
				"sha256:f33691825429f378879ebd26890ec047"
				"ec30538212080bfc0d0ba5b4e6ec3f3d"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-004");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-004");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.ContextRevision = TEXT("fixture-context:ls-004:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::TemporaryMutationAllowed),
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
			TEXT("operation:ls-004:") + Suffix,
			TEXT("lease:ls-004:owner"),
			TEXT("server-session:ls-004"),
			Descriptor,
			MakeContext(Descriptor),
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				TimeoutSeconds,
				[&Now]() { return Now; }));
	}

	class FRecordingEditorSessionPort final
		: public ILongRunningOperationPort
	{
	public:
		FRecordingEditorSessionPort(
			EUeAgentEditorSessionOperationFamily InFamily,
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
				TEXT("resource:ls-004:") + LastOperationId,
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

		EUeAgentEditorSessionOperationFamily Family;
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

	class FRecordingEditorSessionPortFactory final
		: public IUeAgentEditorSessionOperationPortFactory
	{
	public:
		virtual bool TryCreateOperationPort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FLongRunningOperationRequest& OperationRequest,
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
					TEXT("fixture_editor_session_factory_failed"));
				return false;
			}
			const TSharedRef<
				FRecordingEditorSessionPort,
				ESPMode::ThreadSafe> Port = MakeShared<
					FRecordingEditorSessionPort,
					ESPMode::ThreadSafe>(
					Request.GetFamily(),
					200 + Ports.Num());
			Ports.Add(Port);
			OutPort = Port;
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		EUeAgentEditorSessionOperationFamily LastFamily =
			EUeAgentEditorSessionOperationFamily::AssetEditor;
		FString LastSubjectRef;
		FString LastPolicyId;
		TArray<TSharedRef<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe>> Ports;
	};

	struct FFamilyCase
	{
		EUeAgentEditorSessionOperationFamily Family;
		FString SubjectRef;
		FString PolicyId;
		FString SignalTypeId;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionOperationAdapterBehaviorTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.E1.Adapter."
		"SessionRevisionDisconnectExpiryAndSafeExit",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionOperationAdapterBehaviorTest::RunTest(
	const FString& Parameters)
{
	TestEqual(
		TEXT("production adapter version"),
		UAI_EDITOR_SESSION_OPERATION_ADAPTER_V1,
		1);

	double Now = 100.0;
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> UnexpectedDefaultPort = MakeShared<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe>(
			EUeAgentEditorSessionOperationFamily::AssetEditor,
			9000);
	FLongRunningOperationOwner Owner(
		UnexpectedDefaultPort,
		[&Now]() { return Now; });
	FRecordingEditorSessionPortFactory Factory;
	const TArray<FFamilyCase> Cases = {
		{EUeAgentEditorSessionOperationFamily::AssetEditor,
		 TEXT("asset:/Game/Fixture/BP_Session"),
		 TEXT("policy:asset-editor:open/v1"),
		 TEXT("uai.editor_session.asset_editor.state/v1")},
		{EUeAgentEditorSessionOperationFamily::RuntimePreview,
		 TEXT("pie-session:fixture"),
		 TEXT("policy:runtime-session:pie/v1"),
		 TEXT("uai.editor_session.runtime_preview.state/v1")},
		{EUeAgentEditorSessionOperationFamily::WorldContext,
		 TEXT("world:/Game/Fixture/ExplicitLevel"),
		 TEXT("policy:world-context:set-current/v1"),
		 TEXT("uai.editor_session.world_context.state/v1")},
		{EUeAgentEditorSessionOperationFamily::ModelingTool,
		 TEXT("modeling-session:fixture-tool"),
		 TEXT("policy:modeling-tool:invoke-action/v1"),
		 TEXT("uai.editor_session.modeling_tool.state/v1")},
		{EUeAgentEditorSessionOperationFamily::VoxelGrid,
		 TEXT("voxel-session:fixture-grid"),
		 TEXT("policy:voxel-session:add-bounds/v1"),
		 TEXT("uai.editor_session.voxel_grid.state/v1")},
		{EUeAgentEditorSessionOperationFamily::HostLifecycle,
		 TEXT("editor-instance:fixture-editor"),
		 TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
		 TEXT("uai.editor_session.host_lifecycle.state/v1")},
	};

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FFamilyCase& Case = Cases[Index];
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources = MakeResources();
		const FUeAgentEditorSessionOperationTypedRequest TypedRequest(
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
			TryAcceptEditorSessionOperation(
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
		TestEqual(TEXT("explicit semantic subject reaches factory"),
			Factory.LastSubjectRef, Case.SubjectRef);
		TestEqual(TEXT("internal policy reaches factory"),
			Factory.LastPolicyId, Case.PolicyId);
		TestEqual(TEXT("accept remains non-terminal"),
			Snapshot.GetPhase(), ELongRunningOperationPhase::Accepted);
		TestEqual(TEXT("accept does not start native work"),
			Factory.Ports.Last()->StartCalls, 0);
		TestEqual(TEXT("central default port is never used"),
			UnexpectedDefaultPort->StartCalls, 0);

		const TSharedRef<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe> Port = Factory.Ports.Last();
		TestTrue(TEXT("run starts the dedicated session port"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				0,
				Snapshot,
				Error));
		TestEqual(TEXT("dedicated port starts once"),
			Port->StartCalls, 1);
		Port->EmitSignal(
			Case.SignalTypeId,
			1,
			TEXT("health:ls-004:") + FString::FromInt(Index));
		TestTrue(TEXT("current generation health signal is observed"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation,
				Snapshot,
				Error));
		TestTrue(TEXT("snapshot exposes typed health signal"),
			Snapshot.GetLatestSignal().IsSet());
		if (Snapshot.GetLatestSignal().IsSet())
		{
			TestEqual(TEXT("family signal type is exact"),
				Snapshot.GetLatestSignal()->GetTypeId(),
				Case.SignalTypeId);
			TestEqual(TEXT("signal revision starts at one"),
				Snapshot.GetLatestSignal()->GetRevision(), int64(1));
			TestEqual(TEXT("signal payload stays canonical"),
				Snapshot.GetLatestSignal()->GetPayload()->GetKind(),
				EUeAgentValueKind::String);
		}

		const int32 StopCallsBeforeDisconnect = Port->StopCalls;
		const int32 ObserveCallsBeforeWrongSession = Port->ObserveCalls;
		FLongRunningOperationSnapshot UnauthorizedSnapshot;
		TestFalse(TEXT("different server session cannot reattach"),
			Owner.GetSnapshot(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				TEXT("server-session:disconnected-replacement"),
				UnauthorizedSnapshot,
				Error));
		TestFalse(TEXT("failed reattach leaks no signal"),
			UnauthorizedSnapshot.GetLatestSignal().IsSet());
		TestEqual(TEXT("disconnect/wrong session cannot infer cancel"),
			Port->StopCalls, StopCallsBeforeDisconnect);
		TestEqual(TEXT("wrong session performs no native observation"),
			Port->ObserveCalls, ObserveCallsBeforeWrongSession);
		TestTrue(TEXT("original server session can reattach"),
			Owner.GetSnapshot(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Snapshot,
				Error));
		TestEqual(TEXT("reattach remains generation bound"),
			Snapshot.GetGeneration(), Port->Generation);
		TestTrue(TEXT("reattach retains latest revision"),
			Snapshot.GetLatestSignal().IsSet());
		if (Snapshot.GetLatestSignal().IsSet())
		{
			TestEqual(TEXT("reattach revision is exact"),
				Snapshot.GetLatestSignal()->GetRevision(), int64(1));
		}

		const int32 ObserveCallsBeforeStale = Port->ObserveCalls;
		TestFalse(TEXT("stale generation cannot continue session"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation - 1,
				Snapshot,
				Error));
		TestEqual(TEXT("stale generation performs no native work"),
			Port->ObserveCalls, ObserveCallsBeforeStale);

		Port->EmitSignal(
			Case.SignalTypeId,
			2,
			TEXT("health:ls-004:r2:") + FString::FromInt(Index));
		TestTrue(TEXT("monotonic signal revision advances"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation,
				Snapshot,
				Error));
		TestTrue(TEXT("second observation exposes a signal"),
			Snapshot.GetLatestSignal().IsSet());
		if (Snapshot.GetLatestSignal().IsSet())
		{
			TestEqual(TEXT("latest revision is two"),
				Snapshot.GetLatestSignal()->GetRevision(), int64(2));
		}

		Port->EmitExitedSuccess();
		TestTrue(TEXT("physical exit joins and releases"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Port->Generation,
				Snapshot,
				Error));
		TestTrue(TEXT("terminal requires physical exit"),
			Snapshot.IsTerminal());
		TestEqual(TEXT("session port joins once"), Port->JoinCalls, 1);
		TestEqual(TEXT("session port releases once"),
			Port->ReleaseCalls, 1);
		FLongRunningOperationReceipt Receipt;
		TestTrue(TEXT("terminal receipt is lifecycle-only"),
			Owner.GetTerminalReceipt(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Receipt,
				Error));
		TestTrue(TEXT("caller explicitly releases terminal receipt"),
			Owner.ReleaseTerminal(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				Error));
	}

	const int32 FactoryCallsBeforeInvalid = Factory.Calls;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeepValidResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest DeepValidRequest(
		EUeAgentEditorSessionOperationFamily::ModelingTool,
		TEXT("modeling-session:deep-valid"),
		TEXT("policy:modeling-tool:set-property/v1"),
		MakeDeepParameters(*DeepValidResources, 256, TEXT("property_value")));
	TestTrue(
		TEXT("deep canonical params have no artificial nesting gate"),
		DeepValidRequest.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeepInvalidResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest DeepInvalidRequest(
		EUeAgentEditorSessionOperationFamily::HostLifecycle,
		TEXT("editor-instance:fixture"),
		TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
		MakeDeepParameters(*DeepInvalidResources, 256, TEXT("force_exit")));
	TestFalse(TEXT("deep forbidden field cannot bypass validation"),
		DeepInvalidRequest.IsValid());
	FLongRunningOperationSnapshot Snapshot;
	FExecutionMutationError Error;
	TestFalse(TEXT("invalid request fails before factory and owner"),
		TryAcceptEditorSessionOperation(
			DeepInvalidRequest,
			MakeOperationRequest(
				TEXT("deep-invalid"),
				DeepInvalidResources,
				Now),
			Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(TEXT("nested forbidden field has stable error"),
		Error.GetCode(),
		FString(TEXT(
			"editor_session_operation_request_forbidden_field")));
	TestEqual(TEXT("invalid request never calls factory"),
		Factory.Calls, FactoryCallsBeforeInvalid);

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		BatchResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest BatchRequest(
		EUeAgentEditorSessionOperationFamily::RuntimePreview,
		TEXT("pie-session:fixture"),
		TEXT("policy:runtime-session:pie/v1"),
		MakeParameters(
			*BatchResources,
			{{TEXT("batch"), MakeString(*BatchResources, TEXT("legacy"))}}));
	TestFalse(TEXT("raw batch cannot become a session family"),
		BatchRequest.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ImplicitResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest ImplicitRequest(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/Fixture/ExplicitLevel"),
		TEXT("policy:world-context:set-current/v1"),
		MakeParameters(
			*ImplicitResources,
			{{TEXT("use_current_level"),
			  MakeString(*ImplicitResources, TEXT("true"))}}));
	TestFalse(TEXT("implicit current-level fallback is rejected"),
		ImplicitRequest.IsValid());

	double CancelNow = 300.0;
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> CancelDefaultPort = MakeShared<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe>(
			EUeAgentEditorSessionOperationFamily::AssetEditor,
			9100);
	FLongRunningOperationOwner CancelOwner(
		CancelDefaultPort,
		[&CancelNow]() { return CancelNow; });
	FRecordingEditorSessionPortFactory CancelFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		CancelResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest ModelingRequest(
		EUeAgentEditorSessionOperationFamily::ModelingTool,
		TEXT("modeling-session:cancel-fixture"),
		TEXT("policy:modeling-tool:invoke-action/v1"),
		MakeParameters(*CancelResources));
	const FLongRunningOperationRequest ModelingOperation =
		MakeOperationRequest(
			TEXT("modeling-cancel"),
			CancelResources,
			CancelNow);
	TestTrue(TEXT("modeling session accepts with explicit owner/expiry"),
		TryAcceptEditorSessionOperation(
			ModelingRequest,
			ModelingOperation,
			CancelFactory,
			CancelOwner,
			Snapshot,
			Error));
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> ModelingPort = CancelFactory.Ports.Last();
	TestTrue(TEXT("modeling session starts"),
		CancelOwner.Advance(
			ModelingOperation.GetOperationId(),
			ModelingOperation.GetOwnerLeaseId(),
			ModelingOperation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestFalse(TEXT("wrong server session cannot cancel modeling session"),
		CancelOwner.RequestCancellation(
			ModelingOperation.GetOperationId(),
			ModelingOperation.GetOwnerLeaseId(),
			TEXT("server-session:other"),
			TEXT("caller_cancelled"),
			Snapshot,
			Error));
	TestEqual(TEXT("wrong-session cancel performs no stop"),
		ModelingPort->StopCalls, 0);
	TestTrue(TEXT("original owner can request safe stop"),
		CancelOwner.RequestCancellation(
			ModelingOperation.GetOperationId(),
			ModelingOperation.GetOwnerLeaseId(),
			ModelingOperation.GetServerSessionId(),
			TEXT("caller_cancelled"),
			Snapshot,
			Error));
	TestEqual(TEXT("safe stop delivered once"),
		ModelingPort->StopCalls, 1);
	TestFalse(TEXT("stop request cannot fabricate terminal"),
		Snapshot.IsTerminal());
	ModelingPort->EmitRunning();
	TestTrue(TEXT("running after stop remains observable"),
		CancelOwner.Advance(
			ModelingOperation.GetOperationId(),
			ModelingOperation.GetOwnerLeaseId(),
			ModelingOperation.GetServerSessionId(),
			ModelingPort->Generation,
			Snapshot,
			Error));
	TestFalse(TEXT("running after stop stays non-terminal"),
		Snapshot.IsTerminal());
	ModelingPort->EmitExitedSuccess();
	TestTrue(TEXT("physical exit settles cancellation"),
		CancelOwner.Advance(
			ModelingOperation.GetOperationId(),
			ModelingOperation.GetOwnerLeaseId(),
			ModelingOperation.GetServerSessionId(),
			ModelingPort->Generation,
			Snapshot,
			Error));
	TestEqual(TEXT("cancelled session lifecycle status"),
		Snapshot.GetStatus(), ELongRunningOperationStatus::Cancelled);

	double DeadlineNow = 500.0;
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> DeadlineDefaultPort = MakeShared<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe>(
			EUeAgentEditorSessionOperationFamily::AssetEditor,
			9200);
	FLongRunningOperationOwner DeadlineOwner(
		DeadlineDefaultPort,
		[&DeadlineNow]() { return DeadlineNow; });
	FRecordingEditorSessionPortFactory DeadlineFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeadlineResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest CurrentLevelRequest(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/Fixture/ExplicitDeadlineLevel"),
		TEXT("policy:world-context:set-current/v1"),
		MakeParameters(*DeadlineResources));
	const FLongRunningOperationRequest CurrentLevelOperation =
		MakeOperationRequest(
			TEXT("current-level-expiry"),
			DeadlineResources,
			DeadlineNow,
			1.0);
	TestTrue(TEXT("current-level operation accepts"),
		TryAcceptEditorSessionOperation(
			CurrentLevelRequest,
			CurrentLevelOperation,
			DeadlineFactory,
			DeadlineOwner,
			Snapshot,
			Error));
	DeadlineNow = 502.0;
	TestTrue(TEXT("expired operation settles without native start"),
		DeadlineOwner.Advance(
			CurrentLevelOperation.GetOperationId(),
			CurrentLevelOperation.GetOwnerLeaseId(),
			CurrentLevelOperation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestEqual(TEXT("expired operation is cancelled"),
		Snapshot.GetStatus(), ELongRunningOperationStatus::Cancelled);
	TestEqual(TEXT("expiry starts no current-level work"),
		DeadlineFactory.Ports.Last()->StartCalls, 0);

	double ExitNow = 700.0;
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> ExitDefaultPort = MakeShared<
			FRecordingEditorSessionPort,
			ESPMode::ThreadSafe>(
			EUeAgentEditorSessionOperationFamily::AssetEditor,
			9300);
	FLongRunningOperationOwner ExitOwner(
		ExitDefaultPort,
		[&ExitNow]() { return ExitNow; });
	FRecordingEditorSessionPortFactory ExitFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ExitResources = MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest SafeExitRequest(
		EUeAgentEditorSessionOperationFamily::HostLifecycle,
		TEXT("editor-instance:fixture-editor"),
		TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
		MakeParameters(*ExitResources));
	const FLongRunningOperationRequest SafeExitOperation =
		MakeOperationRequest(
			TEXT("safe-exit"),
			ExitResources,
			ExitNow);
	TestTrue(TEXT("safe-exit operation accepts"),
		TryAcceptEditorSessionOperation(
			SafeExitRequest,
			SafeExitOperation,
			ExitFactory,
			ExitOwner,
			Snapshot,
			Error));
	const TSharedRef<
		FRecordingEditorSessionPort,
		ESPMode::ThreadSafe> ExitPort = ExitFactory.Ports.Last();
	TestTrue(TEXT("safe-exit operation starts its exact port"),
		ExitOwner.Advance(
			SafeExitOperation.GetOperationId(),
			SafeExitOperation.GetOwnerLeaseId(),
			SafeExitOperation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	ExitPort->EmitSignal(
		TEXT("uai.editor_session.host_lifecycle.exit_scheduled/v1"),
		1,
		TEXT("scheduled"));
	TestTrue(TEXT("scheduled exit is observable"),
		ExitOwner.Advance(
			SafeExitOperation.GetOperationId(),
			SafeExitOperation.GetOwnerLeaseId(),
			SafeExitOperation.GetServerSessionId(),
			ExitPort->Generation,
			Snapshot,
			Error));
	TestFalse(TEXT("scheduled exit is not closed or complete"),
		Snapshot.IsTerminal());
	TestTrue(TEXT("scheduled exit exposes a typed signal"),
		Snapshot.GetLatestSignal().IsSet());
	if (Snapshot.GetLatestSignal().IsSet())
	{
		TestEqual(TEXT("scheduled signal remains observation only"),
			Snapshot.GetLatestSignal()->GetTypeId(),
			FString(TEXT(
				"uai.editor_session.host_lifecycle.exit_scheduled/v1")));
	}
	ExitPort->EmitExitedSuccess();
	TestTrue(TEXT("only a native-port exit observation settles C7"),
		ExitOwner.Advance(
			SafeExitOperation.GetOperationId(),
			SafeExitOperation.GetOwnerLeaseId(),
			SafeExitOperation.GetServerSessionId(),
			ExitPort->Generation,
			Snapshot,
			Error));
	TestTrue(TEXT("native-port exit observation is terminal"),
		Snapshot.IsTerminal());

	return !HasAnyErrors();
}
