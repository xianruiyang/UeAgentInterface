#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

// LS-007 production marker: force a rebuild after the formerly missing
// optional Adapter header becomes available to UBT.
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentCanonicalValueRef MakeObject(
		FUeAgentResourceTracker& Resources,
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items = {})
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			Items, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef MakeString(
		FUeAgentResourceTracker& Resources,
		const FString& Text)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef MakeDeepParameters(
		FUeAgentResourceTracker& Resources,
		int32 Depth,
		const FString& LeafField)
	{
		FUeAgentCanonicalValueRef Current = MakeObject(
			Resources,
			{{LeafField, MakeString(Resources, TEXT("fixture"))}});
		for (int32 Index = 0; Index < Depth; ++Index)
		{
			Current = MakeObject(
				Resources,
				{{TEXT("nested_") + FString::FromInt(Index), Current}});
		}
		return Current;
	}

	FUeAgentDescriptorIdentity MakeDescriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.capture_media_long_job.")
				+ Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:a30df56a3c339531883f213f33a78e82"
				"8c0acfbcafc47c5f444a9d1bc64d5f74"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-006");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-006");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.ContextRevision = TEXT("fixture-context:ls-006:r1");
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
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& Resources,
		double& Now,
		double TimeoutSeconds = 30.0)
	{
		const FUeAgentDescriptorIdentity Descriptor = MakeDescriptor(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:ls-006:") + Suffix,
			TEXT("lease:ls-006:owner"),
			TEXT("server-session:ls-006"),
			Descriptor,
			MakeContext(Descriptor),
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				TimeoutSeconds, [&Now]() { return Now; }));
	}

	class FRecordingCapturePort final : public ILongRunningOperationPort
	{
	public:
		FRecordingCapturePort(
			EUeAgentCaptureMediaLongJobFamily InFamily,
			int64 InGeneration)
			: Family(InFamily), Generation(InGeneration)
		{
		}

		virtual bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			OwnedTemporaryResource =
				TEXT("owned-temp:") + Request.GetOperationId();
			OutIdentity = FLongRunningWorkIdentity(
				OwnedTemporaryResource, Generation);
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
			TOptional<FLongRunningOperationSignal> Signal;
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
						CanonicalError.Code, CanonicalError.Detail);
					return false;
				}
				Signal.Emplace(
					QueuedSignal->TypeId,
					QueuedSignal->Revision,
					Payload.ToSharedRef());
			}
			OutObservation = FLongRunningWorkObservation(
				Generation, QueuedState, FString(), Signal);
			QueuedState = ELongRunningWorkState::Running;
			QueuedSignal.Reset();
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
			bOwnedTemporaryResourceCleaned = true;
			return true;
		}

		void EmitSignal(
			const FString& TypeId,
			int64 Revision,
			const FString& Payload)
		{
			QueuedState = ELongRunningWorkState::Running;
			QueuedSignal.Emplace(FQueuedSignal{TypeId, Revision, Payload});
		}

		void EmitRunning()
		{
			QueuedState = ELongRunningWorkState::Running;
			QueuedSignal.Reset();
		}

		void EmitExitedSuccess()
		{
			QueuedState = ELongRunningWorkState::ExitedSuccess;
			QueuedSignal.Reset();
		}

		EUeAgentCaptureMediaLongJobFamily Family;
		int64 Generation = 0;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		FString LastStopReason;
		FString OwnedTemporaryResource;
		const FString NonOwnedResource = TEXT("user-editor:must-survive");
		bool bOwnedTemporaryResourceCleaned = false;
		bool bNonOwnedResourceTouched = false;

	private:
		struct FQueuedSignal
		{
			FString TypeId;
			int64 Revision = 0;
			FString Payload;
		};
		ELongRunningWorkState QueuedState = ELongRunningWorkState::Running;
		TOptional<FQueuedSignal> QueuedSignal;
	};

	class FRecordingCapturePortFactory final
		: public IUeAgentCaptureMediaLongJobPortFactory
	{
	public:
		virtual bool TryCreateOperationPort(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			++Calls;
			LastFamily = Request.GetFamily();
			LastSubjectRef = Request.GetSubjectRef();
			LastPolicyId = Request.GetPolicyId();
			const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe> Port =
				MakeShared<FRecordingCapturePort, ESPMode::ThreadSafe>(
					Request.GetFamily(), 600 + Ports.Num());
			Ports.Add(Port);
			OutPort = Port;
			return true;
		}

		int32 Calls = 0;
		EUeAgentCaptureMediaLongJobFamily LastFamily =
			EUeAgentCaptureMediaLongJobFamily::ImageCapture;
		FString LastSubjectRef;
		FString LastPolicyId;
		TArray<TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe>> Ports;
	};

	class FFailingArtifactPublisher final
	{
	public:
		bool Publish(const FString& ArtifactRef)
		{
			++Calls;
			LastArtifactRef = ArtifactRef;
			return false;
		}
		int32 Calls = 0;
		FString LastArtifactRef;
	};

	struct FFamilyCase
	{
		EUeAgentCaptureMediaLongJobFamily Family;
		FString SubjectRef;
		FString PolicyId;
		FString SignalTypeId;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaLongJobAdapterBehaviorTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.E1.Adapter."
		"ArtifactProgressCancelCleanupAndRenderResource",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaLongJobAdapterBehaviorTest::RunTest(
	const FString& Parameters)
{
	TestEqual(TEXT("production adapter version"),
		UAI_CAPTURE_MEDIA_LONG_JOB_ADAPTER_V1, 1);
	double Now = 100.0;
	const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe>
		UnexpectedDefaultPort = MakeShared<
			FRecordingCapturePort, ESPMode::ThreadSafe>(
				EUeAgentCaptureMediaLongJobFamily::ImageCapture, 9000);
	FLongRunningOperationOwner Owner(
		UnexpectedDefaultPort, [&Now]() { return Now; });
	FRecordingCapturePortFactory Factory;
	const TArray<FFamilyCase> Cases = {
		{EUeAgentCaptureMediaLongJobFamily::ImageCapture,
		 TEXT("capture-subject:asset:/Game/Fixture/BP_Capture"),
		 TEXT("policy:image-capture:editor-surface/v1"),
		 TEXT("uai.capture.image.progress/v1")},
		{EUeAgentCaptureMediaLongJobFamily::MediaRecording,
		 TEXT("media-source:submix:/Game/Fixture/Submix"),
		 TEXT("policy:media-recording:wav/v1"),
		 TEXT("uai.capture.media.progress/v1")},
		{EUeAgentCaptureMediaLongJobFamily::RenderResource,
		 TEXT("render-resource:/Game/Fixture/RT_Output"),
		 TEXT("policy:render-operation:draw/v1"),
		 TEXT("uai.capture.render.progress/v1")},
	};

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FFamilyCase& Case = Cases[Index];
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources = MakeResources();
		const FUeAgentCaptureMediaLongJobTypedRequest TypedRequest(
			Case.Family,
			Case.SubjectRef,
			Case.PolicyId,
			MakeObject(*Resources));
		TestTrue(TEXT("typed family request is valid"),
			TypedRequest.IsValid());
		const FLongRunningOperationRequest OperationRequest =
			MakeOperationRequest(
				TEXT("family-") + FString::FromInt(Index), Resources, Now);
		FLongRunningOperationSnapshot Snapshot;
		FExecutionMutationError Error;
		const int32 FactoryCallsBefore = Factory.Calls;
		TestTrue(TEXT("Adapter accepts one operation-scoped port"),
			TryAcceptCaptureMediaLongJob(
				TypedRequest, OperationRequest, Factory, Owner, Snapshot, Error));
		TestEqual(TEXT("factory called exactly once"),
			Factory.Calls - FactoryCallsBefore, 1);
		TestEqual(TEXT("family reaches factory"), Factory.LastFamily,
			Case.Family);
		TestEqual(TEXT("explicit subject reaches factory"),
			Factory.LastSubjectRef, Case.SubjectRef);
		TestEqual(TEXT("policy reaches factory"),
			Factory.LastPolicyId, Case.PolicyId);
		TestEqual(TEXT("accepted is non-terminal"), Snapshot.GetPhase(),
			ELongRunningOperationPhase::Accepted);
		TestEqual(TEXT("accept does not start native work"),
			Factory.Ports.Last()->StartCalls, 0);
		TestEqual(TEXT("central default port is unused"),
			UnexpectedDefaultPort->StartCalls, 0);

		const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe> Port =
			Factory.Ports.Last();
		TestTrue(TEXT("advance starts dedicated native work"),
			Owner.Advance(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				0, Snapshot, Error));
		TestEqual(TEXT("native work starts once"), Port->StartCalls, 1);

		Port->EmitSignal(Case.SignalTypeId, 1, TEXT("progress:0.5"));
		TestTrue(TEXT("progress is observed"), Owner.Advance(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(),
			Port->Generation, Snapshot, Error));
		TestFalse(TEXT("progress cannot terminalize job"), Snapshot.IsTerminal());
		TestTrue(TEXT("progress is a typed canonical signal"),
			Snapshot.GetLatestSignal().IsSet());
		if (Snapshot.GetLatestSignal().IsSet())
		{
			TestEqual(TEXT("progress type id is exact"),
				Snapshot.GetLatestSignal()->GetTypeId(), Case.SignalTypeId);
			TestEqual(TEXT("progress revision is one"),
				Snapshot.GetLatestSignal()->GetRevision(), int64(1));
		}

		Port->EmitSignal(
			TEXT("uai.capture.artifact.ready/v1"),
			2,
			TEXT("artifact:ls-006:") + FString::FromInt(Index));
		TestTrue(TEXT("artifact readiness is observed"), Owner.Advance(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(),
			Port->Generation, Snapshot, Error));
		TestFalse(TEXT("artifact readiness does not complete the job"),
			Snapshot.IsTerminal());

		const int32 ObserveBeforeWrongSession = Port->ObserveCalls;
		FLongRunningOperationSnapshot Unauthorized;
		TestFalse(TEXT("wrong session cannot observe job"), Owner.Advance(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			TEXT("server-session:other"),
			Port->Generation, Unauthorized, Error));
		TestEqual(TEXT("wrong session performs no native observation"),
			Port->ObserveCalls, ObserveBeforeWrongSession);
		TestFalse(TEXT("stale generation cannot continue job"), Owner.Advance(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(),
			Port->Generation - 1, Snapshot, Error));
		TestEqual(TEXT("stale generation performs no native observation"),
			Port->ObserveCalls, ObserveBeforeWrongSession);

		Port->EmitExitedSuccess();
		TestTrue(TEXT("physical exit settles job"), Owner.Advance(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(),
			Port->Generation, Snapshot, Error));
		TestTrue(TEXT("job is terminal only after physical exit"),
			Snapshot.IsTerminal());
		TestEqual(TEXT("native resource joined once"), Port->JoinCalls, 1);
		TestEqual(TEXT("native resource released once"), Port->ReleaseCalls, 1);
		TestTrue(TEXT("exact owned temporary resource cleaned"),
			Port->bOwnedTemporaryResourceCleaned);
		TestFalse(TEXT("non-owned resource survives cleanup"),
			Port->bNonOwnedResourceTouched);

		FLongRunningOperationReceipt ReceiptBeforeDelivery;
		TestTrue(TEXT("read lifecycle receipt"), Owner.GetTerminalReceipt(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(),
			ReceiptBeforeDelivery, Error));
		FFailingArtifactPublisher Publisher;
		TestFalse(TEXT("fixture artifact delivery fails independently"),
			Publisher.Publish(
				TEXT("artifact:ls-006:") + FString::FromInt(Index)));
		FLongRunningOperationReceipt ReceiptAfterDelivery;
		TestTrue(TEXT("delivery failure leaves receipt readable"),
			Owner.GetTerminalReceipt(
				OperationRequest.GetOperationId(),
				OperationRequest.GetOwnerLeaseId(),
				OperationRequest.GetServerSessionId(),
				ReceiptAfterDelivery, Error));
		TestEqual(TEXT("delivery failure does not rewrite job status"),
			ReceiptAfterDelivery.GetStatus(), ReceiptBeforeDelivery.GetStatus());
		TestEqual(TEXT("delivery failure does not rewrite job error"),
			ReceiptAfterDelivery.GetErrorCode(),
			ReceiptBeforeDelivery.GetErrorCode());
		TestTrue(TEXT("caller releases terminal receipt"), Owner.ReleaseTerminal(
			OperationRequest.GetOperationId(),
			OperationRequest.GetOwnerLeaseId(),
			OperationRequest.GetServerSessionId(), Error));
	}

	const TArray<TPair<FString, FString>> RenderActions = {
		{TEXT("clear"), TEXT("target_pixel_stats")},
		{TEXT("copy"), TEXT("source_target_pixel_relation")},
		{TEXT("bake"), TEXT("created_texture_pixel_readback")},
		{TEXT("draw"), TEXT("target_pixel_stats")},
	};
	for (const TPair<FString, FString>& RenderAction : RenderActions)
	{
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources = MakeResources();
		const FUeAgentCaptureMediaLongJobTypedRequest Request(
			EUeAgentCaptureMediaLongJobFamily::RenderResource,
			TEXT("render-resource:/Game/Fixture/RT_Output"),
			TEXT("policy:render-operation:") + RenderAction.Key + TEXT("/v1"),
			MakeObject(*Resources, {
				{TEXT("render_action"), MakeString(*Resources, RenderAction.Key)},
				{TEXT("readback_contract"),
				 MakeString(*Resources, RenderAction.Value)},
			}));
		TestTrue(TEXT("RenderTarget GPU request is typed: ")
			+ RenderAction.Key, Request.IsValid());
		TestEqual(TEXT("RenderTarget action stays in render-resource family"),
			Request.GetFamily(),
			EUeAgentCaptureMediaLongJobFamily::RenderResource);
	}

	const int32 CallsBeforeInvalid = Factory.Calls;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeepValidResources = MakeResources();
	const FUeAgentCaptureMediaLongJobTypedRequest DeepValid(
		EUeAgentCaptureMediaLongJobFamily::ImageCapture,
		TEXT("capture-subject:deep-valid"),
		TEXT("policy:image-capture:fixture/v1"),
		MakeDeepParameters(*DeepValidResources, 256, TEXT("projection")));
	TestTrue(TEXT("deep canonical params have no artificial depth gate"),
		DeepValid.IsValid());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeepInvalidResources = MakeResources();
	const FUeAgentCaptureMediaLongJobTypedRequest DeepInvalid(
		EUeAgentCaptureMediaLongJobFamily::ImageCapture,
		TEXT("capture-subject:deep-invalid"),
		TEXT("policy:image-capture:fixture/v1"),
		MakeDeepParameters(
			*DeepInvalidResources, 256, TEXT("raw_image_bytes")));
	TestFalse(TEXT("deep raw payload cannot bypass validation"),
		DeepInvalid.IsValid());
	FLongRunningOperationSnapshot Snapshot;
	FExecutionMutationError Error;
	TestFalse(TEXT("invalid request fails before factory and owner"),
		TryAcceptCaptureMediaLongJob(
			DeepInvalid,
			MakeOperationRequest(
				TEXT("deep-invalid"), DeepInvalidResources, Now),
			Factory, Owner, Snapshot, Error));
	TestEqual(TEXT("deep forbidden field has stable error"),
		Error.GetCode(),
		FString(TEXT("capture_media_long_job_request_forbidden_field")));
	TestEqual(TEXT("invalid request never calls factory"),
		Factory.Calls, CallsBeforeInvalid);

	double CancelNow = 300.0;
	const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe>
		CancelDefaultPort = MakeShared<
			FRecordingCapturePort, ESPMode::ThreadSafe>(
				EUeAgentCaptureMediaLongJobFamily::MediaRecording, 9100);
	FLongRunningOperationOwner CancelOwner(
		CancelDefaultPort, [&CancelNow]() { return CancelNow; });
	FRecordingCapturePortFactory CancelFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		CancelResources = MakeResources();
	const FLongRunningOperationRequest CancelOperation =
		MakeOperationRequest(TEXT("cancel"), CancelResources, CancelNow);
	const FUeAgentCaptureMediaLongJobTypedRequest CancelRequest(
		EUeAgentCaptureMediaLongJobFamily::MediaRecording,
		TEXT("media-source:submix:/Game/Fixture/Submix"),
		TEXT("policy:media-recording:wav/v1"),
		MakeObject(*CancelResources));
	TestTrue(TEXT("cancel fixture accepts"), TryAcceptCaptureMediaLongJob(
		CancelRequest, CancelOperation, CancelFactory,
		CancelOwner, Snapshot, Error));
	const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe> CancelPort =
		CancelFactory.Ports.Last();
	TestTrue(TEXT("cancel fixture starts"), CancelOwner.Advance(
		CancelOperation.GetOperationId(), CancelOperation.GetOwnerLeaseId(),
		CancelOperation.GetServerSessionId(), 0, Snapshot, Error));
	TestTrue(TEXT("cancel requests safe stop"),
		CancelOwner.RequestCancellation(
			CancelOperation.GetOperationId(),
			CancelOperation.GetOwnerLeaseId(),
			CancelOperation.GetServerSessionId(),
			TEXT("caller_cancelled"), Snapshot, Error));
	TestEqual(TEXT("safe stop delivered once"), CancelPort->StopCalls, 1);
	TestFalse(TEXT("stop request cannot fabricate terminal"),
		Snapshot.IsTerminal());
	CancelPort->EmitRunning();
	TestTrue(TEXT("running after stop remains observable"), CancelOwner.Advance(
		CancelOperation.GetOperationId(), CancelOperation.GetOwnerLeaseId(),
		CancelOperation.GetServerSessionId(), CancelPort->Generation,
		Snapshot, Error));
	TestFalse(TEXT("running after stop remains non-terminal"),
		Snapshot.IsTerminal());
	CancelPort->EmitExitedSuccess();
	TestTrue(TEXT("physical exit settles cancellation"), CancelOwner.Advance(
		CancelOperation.GetOperationId(), CancelOperation.GetOwnerLeaseId(),
		CancelOperation.GetServerSessionId(), CancelPort->Generation,
		Snapshot, Error));
	TestEqual(TEXT("cancelled job lifecycle status"), Snapshot.GetStatus(),
		ELongRunningOperationStatus::Cancelled);
	TestTrue(TEXT("cancel cleanup releases exact owned resource"),
		CancelPort->bOwnedTemporaryResourceCleaned);
	TestFalse(TEXT("cancel cleanup preserves non-owned resource"),
		CancelPort->bNonOwnedResourceTouched);

	double DeadlineNow = 500.0;
	const TSharedRef<FRecordingCapturePort, ESPMode::ThreadSafe>
		DeadlineDefaultPort = MakeShared<
			FRecordingCapturePort, ESPMode::ThreadSafe>(
				EUeAgentCaptureMediaLongJobFamily::ImageCapture, 9200);
	FLongRunningOperationOwner DeadlineOwner(
		DeadlineDefaultPort, [&DeadlineNow]() { return DeadlineNow; });
	FRecordingCapturePortFactory DeadlineFactory;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		DeadlineResources = MakeResources();
	const FLongRunningOperationRequest DeadlineOperation =
		MakeOperationRequest(
			TEXT("deadline-before-start"), DeadlineResources, DeadlineNow, 1.0);
	const FUeAgentCaptureMediaLongJobTypedRequest DeadlineRequest(
		EUeAgentCaptureMediaLongJobFamily::ImageCapture,
		TEXT("capture-subject:viewport:fixture"),
		TEXT("policy:image-capture:viewport/v1"),
		MakeObject(*DeadlineResources));
	TestTrue(TEXT("deadline fixture accepts"), TryAcceptCaptureMediaLongJob(
		DeadlineRequest, DeadlineOperation, DeadlineFactory,
		DeadlineOwner, Snapshot, Error));
	DeadlineNow = 502.0;
	TestTrue(TEXT("expired accepted job settles without native start"),
		DeadlineOwner.Advance(
			DeadlineOperation.GetOperationId(),
			DeadlineOperation.GetOwnerLeaseId(),
			DeadlineOperation.GetServerSessionId(), 0, Snapshot, Error));
	TestEqual(TEXT("deadline before start starts no native work"),
		DeadlineFactory.Ports.Last()->StartCalls, 0);
	TestTrue(TEXT("deadline result is terminal"), Snapshot.IsTerminal());
	return !HasAnyErrors();
}
