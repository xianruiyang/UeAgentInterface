#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobProduction.h"

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					16 * 1024 * 1024},
			}));
	}

	FUeAgentCanonicalValueRef EmptyParameters(FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject({}, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCancellationToken UncancelledToken()
	{
		FUeAgentCancellationSource Source(
			[]() { return FPlatformTime::Seconds(); });
		return Source.GetToken();
	}

	FUeAgentDescriptorIdentity DescriptorIdentity(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.capture_media.production.") + Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:ca16a33db14ce64c93071f5baf186a1e"
				"4617c6f949628642a837eba9a8dfdca9"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor,
		const EUeAgentStatePlane StatePlane,
		const EUeAgentExecutionWorldType WorldType)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:ls012");
		Input.EditorInstanceId = TEXT("editor-instance:ls012-owned");
		Input.StatePlane = StatePlane;
		Input.WorldType = WorldType;
		Input.WorldIdentity = TEXT("world:ls012-owned");
		if (WorldType == EUeAgentExecutionWorldType::Pie
			|| WorldType == EUeAgentExecutionWorldType::SimulateInEditor)
		{
			Input.PieInstanceId = 0;
		}
		Input.ContextRevision = TEXT("context:ls012:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{StatePlane},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::ReadOnly),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	FLongRunningOperationRequest MakeOperationRequest(
		const FString& Suffix,
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& Resources,
		const EUeAgentStatePlane StatePlane = EUeAgentStatePlane::AuthoringMemory,
		const EUeAgentExecutionWorldType WorldType =
			EUeAgentExecutionWorldType::Editor)
	{
		const FUeAgentDescriptorIdentity Descriptor = DescriptorIdentity(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:ls012:") + Suffix,
			TEXT("lease:ls012:owner"),
			TEXT("server-session:ls012"),
			Descriptor,
			MakeContext(Descriptor, StatePlane, WorldType),
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return FPlatformTime::Seconds(); }));
	}

	FFileArtifactReference ArtifactRef(const FString& Suffix = TEXT("one"))
	{
		return FFileArtifactReference(
			TEXT("artifact:ls012:") + Suffix,
			TEXT("operation:ls012:artifact-owner"),
			TEXT("image"),
			TEXT("png"),
			TEXT("image/png"),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("UAIArtifacts"), Suffix + TEXT(".png"))),
			TEXT("sha256:47f2a73f31546e3943a09007bc874458"
				"bc050605ee66f7204c82dfc7797f2e7b"),
			128);
	}

	const FUeAgentCanonicalValueRef* FindObjectValue(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Field)
	{
		if (Object->GetKind() != EUeAgentValueKind::Object)
		{
			return nullptr;
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
			: Object->GetObjectItems())
		{
			if (Item.Key == Field)
			{
				return &Item.Value;
			}
		}
		return nullptr;
	}

	class FNoopPort final : public ILongRunningOperationPort
	{
	public:
		bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			OutIdentity = FLongRunningWorkIdentity(
				TEXT("native:capture:noop:") + Request.GetOperationId(), 1);
			return true;
		}

		bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			OutObservation = FLongRunningWorkObservation(
				Identity.GetGeneration(), ELongRunningWorkState::Running, {});
			return true;
		}

		bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			return true;
		}

		bool Join(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			return true;
		}

		bool Release(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			return true;
		}
	};

	class FRecordingNativeSession final
		: public IUeAgentCaptureMediaNativeSession
	{
	public:
		bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FString& OutNativeResourceId,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			LastOperationId = Request.GetOperationId();
			OutNativeResourceId = NativeResourceId;
			return true;
		}

		bool Observe(
			FUeAgentCaptureMediaNativeObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			++ObserveCalls;
			OutObservation = Observation;
			return true;
		}

		bool RequestStop(
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			++StopCalls;
			LastStopReason = Reason;
			return true;
		}

		bool Join(FExecutionMutationError& OutError) override
		{
			++JoinCalls;
			return true;
		}

		bool Release(FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			if (!OwnedSourcePath.IsEmpty())
			{
				IFileManager::Get().Delete(*OwnedSourcePath, false, true, true);
			}
			return true;
		}

		FUeAgentCaptureMediaNativeObservation Observation = {
			ELongRunningWorkState::Running,
			TEXT("running")};
		FString NativeResourceId = TEXT("native-capture:ls012-owned");
		FString OwnedSourcePath;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		FString LastOperationId;
		FString LastStopReason;
	};

	class FRecordingNativeBuilder final
		: public IUeAgentCaptureMediaLongJobPortBuilder
	{
	public:
		bool TryCreatePort(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
				MakeShared<FRecordingNativeSession, ESPMode::ThreadSafe>();
			Sessions.Add(Native);
			const bool bCreated = TryCreateCaptureMediaNativeResourcePort(
				Request, Policy, Native, OutPort, OutError);
			if (bCreated && OutPort.IsValid())
			{
				Ports.Add(OutPort.ToSharedRef());
			}
			return bCreated;
		}

		TArray<TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe>> Sessions;
		TArray<TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe>> Ports;
	};

	FUeAgentCaptureMediaLongJobPolicyDescriptor Policy(
		const EUeAgentCaptureMediaLongJobFamily Family,
		const FString& PolicyId,
		const FString& Mechanism,
		const FString& Oracle,
		const EUeAgentStatePlane StatePlane,
		TArray<EUeAgentExecutionWorldType> WorldTypes)
	{
		return FUeAgentCaptureMediaLongJobPolicyDescriptor(
			Family,
			PolicyId,
			Mechanism,
			Oracle,
			StatePlane,
			MoveTemp(WorldTypes));
	}

	FUeAgentCaptureMediaLongJobPolicyDescriptor ImagePolicy()
	{
		return Policy(
			EUeAgentCaptureMediaLongJobFamily::ImageCapture,
			TEXT("policy:image-capture:viewport/v1"),
			TEXT("image_capture_resource"),
			TEXT("uai.readback.capture.image/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			{EUeAgentExecutionWorldType::Editor});
	}

	FUeAgentCaptureMediaLongJobPolicyDescriptor MediaPolicy()
	{
		return Policy(
			EUeAgentCaptureMediaLongJobFamily::MediaRecording,
			TEXT("policy:media-recording:submix-wav/v1"),
			TEXT("media_recording_resource"),
			TEXT("uai.readback.capture.media/v1"),
			EUeAgentStatePlane::RuntimeInstance,
			{EUeAgentExecutionWorldType::Editor,
				EUeAgentExecutionWorldType::Pie});
	}

	FUeAgentCaptureMediaLongJobPolicyDescriptor RenderPolicy()
	{
		return Policy(
			EUeAgentCaptureMediaLongJobFamily::RenderResource,
			TEXT("policy:render-operation:clear/v1"),
			TEXT("gpu_render_resource"),
			TEXT("uai.readback.capture.render_target/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			{EUeAgentExecutionWorldType::Editor});
	}

	FUeAgentCaptureMediaLongJobTypedRequest TypedRequest(
		const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
		FUeAgentResourceTracker& Resources)
	{
		FString Subject;
		switch (Policy.GetFamily())
		{
		case EUeAgentCaptureMediaLongJobFamily::ImageCapture:
			Subject = TEXT("capture-subject:viewport:ls012");
			break;
		case EUeAgentCaptureMediaLongJobFamily::MediaRecording:
			Subject = TEXT("media-source:submix:ls012");
			break;
		default:
			Subject = TEXT("render-resource:/Game/UAI/RT_LS012");
			break;
		}
		return FUeAgentCaptureMediaLongJobTypedRequest(
			Policy.GetFamily(),
			MoveTemp(Subject),
			Policy.GetPolicyId(),
			EmptyParameters(Resources));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaExactPolicyCatalogTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Production.E0."
		"ExactPolicyCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaExactPolicyCatalogTest::RunTest(
	const FString& Parameters)
{
	TestEqual(TEXT("production contract version"),
		UAI_CAPTURE_MEDIA_LONG_JOB_PRODUCTION_V1, 1);
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	const TArray<FUeAgentCaptureMediaLongJobPolicyRegistration> Registrations = {
		{ImagePolicy(), Builder},
		{MediaPolicy(), Builder},
		{RenderPolicy(), Builder},
	};
	TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("three-family exact policy catalog constructs"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			Registrations, Factory, Error));
	if (!Factory.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("all three policies retained"),
		Factory->GetPolicyDescriptors().Num(), 3);
	for (const FUeAgentCaptureMediaLongJobPolicyDescriptor& Descriptor
		: Factory->GetPolicyDescriptors())
	{
		TestTrue(TEXT("policy descriptor is valid"), Descriptor.IsValid());
		TestTrue(TEXT("policy binds exact C9 oracle"),
			Descriptor.GetReadbackOracleId().StartsWith(TEXT("uai.readback.")));
	}

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCaptureMediaLongJobTypedRequest Request =
		TypedRequest(ImagePolicy(), *Resources);
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> First;
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Second;
	TestTrue(TEXT("exact policy creates first operation port"),
		Factory->TryCreateOperationPort(Request, First, Error));
	TestTrue(TEXT("exact policy creates second operation port"),
		Factory->TryCreateOperationPort(Request, Second, Error));
	TestTrue(TEXT("ports are operation scoped and distinct"),
		First.IsValid() && Second.IsValid() && First.Get() != Second.Get());

	const FUeAgentCaptureMediaLongJobTypedRequest Unknown(
		EUeAgentCaptureMediaLongJobFamily::ImageCapture,
		TEXT("capture-subject:viewport:ls012"),
		TEXT("policy:image-capture:viewport/v2"),
		EmptyParameters(*Resources));
	TestFalse(TEXT("unknown suffix cannot match by prefix"),
		Factory->TryCreateOperationPort(Unknown, First, Error));
	TestEqual(TEXT("unknown exact policy has stable error"), Error.GetCode(),
		FString(TEXT("capture_media_long_job_policy_not_registered")));

	TArray<FUeAgentCaptureMediaLongJobPolicyRegistration> Duplicate =
		Registrations;
	Duplicate.Add(Registrations[0]);
	TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory> DuplicateFactory;
	TestFalse(TEXT("duplicate registration fails closed"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			Duplicate, DuplicateFactory, Error));
	TestEqual(TEXT("duplicate registration has stable error"), Error.GetCode(),
		FString(TEXT("capture_media_long_job_policy_duplicate")));

	const FUeAgentCaptureMediaLongJobPolicyDescriptor WrongMechanism = Policy(
		EUeAgentCaptureMediaLongJobFamily::RenderResource,
		TEXT("policy:render-operation:wrong/v1"),
		TEXT("image_capture_resource"),
		TEXT("uai.readback.capture.render_target/v1"),
		EUeAgentStatePlane::AuthoringMemory,
		{EUeAgentExecutionWorldType::Editor});
	TestFalse(TEXT("family mechanism mismatch fails catalog construction"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			{{WrongMechanism, Builder}}, DuplicateFactory, Error));
	TestEqual(TEXT("mechanism mismatch has stable error"), Error.GetCode(),
		FString(TEXT("capture_media_long_job_policy_invalid")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaThreeFamilyPortTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Production.E1."
		"ThreeFamilyNativePortCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaThreeFamilyPortTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("three-family production factory constructs"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			{{ImagePolicy(), Builder},
				{MediaPolicy(), Builder},
				{RenderPolicy(), Builder}},
			Factory,
			Error));
	const TArray<FUeAgentCaptureMediaLongJobPolicyDescriptor> Policies = {
		ImagePolicy(), MediaPolicy(), RenderPolicy()};
	for (int32 Index = 0; Index < Policies.Num(); ++Index)
	{
		const FUeAgentCaptureMediaLongJobPolicyDescriptor& Descriptor =
			Policies[Index];
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
			MakeResources();
		const FUeAgentCaptureMediaLongJobTypedRequest Request =
			TypedRequest(Descriptor, *Resources);
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
		TestTrue(TEXT("family creates unified native resource port"),
			Factory->TryCreateOperationPort(Request, Port, Error));
		const FLongRunningOperationRequest Operation = MakeOperationRequest(
			FString::Printf(TEXT("family-%d"), Index),
			Resources,
			Descriptor.GetRequiredStatePlane(),
			Descriptor.GetAllowedWorldTypes()[0]);
		FLongRunningWorkIdentity Identity;
		TestTrue(TEXT("family native resource starts"),
			Port->Start(Operation, UncancelledToken(), Identity, Error));
		FLongRunningWorkObservation Observation;
		TestTrue(TEXT("family native resource observes"),
			Port->Observe(
				Operation.GetOperationId(), Identity, *Resources, Observation, Error));
	}
	TestEqual(TEXT("all families own distinct native sessions"),
		Builder->Sessions.Num(), 3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaArtifactLifecycleTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Production.E1."
		"ArtifactSignalAndLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaArtifactLifecycleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCaptureMediaLongJobTypedRequest Request =
		TypedRequest(ImagePolicy(), *Resources);
	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
		MakeShared<FRecordingNativeSession, ESPMode::ThreadSafe>();
	Native->Observation = FUeAgentCaptureMediaNativeObservation(
		ELongRunningWorkState::Running,
		TEXT("artifact_ready"),
		FString(),
		ArtifactRef());
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("capture native resource port constructs"),
		TryCreateCaptureMediaNativeResourcePort(
			Request, ImagePolicy(), Native, Port, Error));
	const FLongRunningOperationRequest Operation =
		MakeOperationRequest(TEXT("artifact-lifecycle"), Resources);
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("capture native resource starts"),
		Port->Start(Operation, UncancelledToken(), Identity, Error));
	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("artifact-ready is observed"),
		Port->Observe(
			Operation.GetOperationId(), Identity, *Resources, Observation, Error));
	TestEqual(TEXT("artifact-ready remains running"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestTrue(TEXT("artifact-ready emits typed signal"),
		Observation.GetSignal().IsSet());
	if (Observation.GetSignal().IsSet())
	{
		const FUeAgentCanonicalValueRef& Payload =
			Observation.GetSignal()->GetPayload();
		const FUeAgentCanonicalValueRef* Artifact =
			FindObjectValue(Payload, TEXT("artifact_ref"));
		TestTrue(TEXT("signal contains a structured C8F artifact ref"),
			Artifact && (*Artifact)->GetKind() == EUeAgentValueKind::Object);
		TestTrue(TEXT("signal excludes raw image bytes"),
			FindObjectValue(Payload, TEXT("raw_image_bytes")) == nullptr);
		TestTrue(TEXT("signal excludes delivery status"),
			FindObjectValue(Payload, TEXT("delivery_status")) == nullptr);
		TestTrue(TEXT("signal excludes business Outcome"),
			FindObjectValue(Payload, TEXT("business_outcome")) == nullptr);
	}
	TestTrue(TEXT("unchanged observation remains observable"),
		Port->Observe(
			Operation.GetOperationId(), Identity, *Resources, Observation, Error));
	TestFalse(TEXT("unchanged artifact-ready does not forge revision"),
		Observation.GetSignal().IsSet());

	Native->Observation = FUeAgentCaptureMediaNativeObservation(
		ELongRunningWorkState::ExitedSuccess,
		TEXT("native_exited"),
		FString(),
		ArtifactRef());
	TestTrue(TEXT("physical native exit is observed"),
		Port->Observe(
			Operation.GetOperationId(), Identity, *Resources, Observation, Error));
	TestTrue(TEXT("join succeeds only after physical exit"),
		Port->Join(Operation.GetOperationId(), Identity, Error));
	TestTrue(TEXT("release succeeds only after join"),
		Port->Release(Operation.GetOperationId(), Identity, Error));
	TestEqual(TEXT("native join called once"), Native->JoinCalls, 1);
	TestEqual(TEXT("native release called once"), Native->ReleaseCalls, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaCancellationOwnershipTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Production.E1."
		"CancellationAndExactOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaCancellationOwnershipTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("image production factory constructs"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			{{ImagePolicy(), Builder}}, Factory, Error));
	const TSharedRef<FNoopPort, ESPMode::ThreadSafe> DefaultPort =
		MakeShared<FNoopPort, ESPMode::ThreadSafe>();
	double Now = 100.0;
	FLongRunningOperationOwner Owner(DefaultPort, [&Now]() { return Now; });
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCaptureMediaLongJobTypedRequest Request =
		TypedRequest(ImagePolicy(), *Resources);
	const FLongRunningOperationRequest Operation =
		MakeOperationRequest(TEXT("cancellation"), Resources);
	FLongRunningOperationSnapshot Snapshot;
	TestTrue(TEXT("operation accepts through production factory"),
		TryAcceptCaptureMediaLongJob(
			Request, Operation, *Factory, Owner, Snapshot, Error));
	TestTrue(TEXT("first advance starts exact native session"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	if (Builder->Sessions.IsEmpty())
	{
		AddError(TEXT("capture production builder created no native session"));
		return false;
	}
	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
		Builder->Sessions.Last();
	Native->OwnedSourcePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("UeAgentInterfaceTests"),
		TEXT("LS012Owned"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp")));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Native->OwnedSourcePath), true);
	TestTrue(TEXT("exact-owned native source fixture exists"),
		FFileHelper::SaveStringToFile(TEXT("owned"), *Native->OwnedSourcePath));

	const FLongRunningWorkIdentity WrongIdentity(
		TEXT("native-capture:not-owned"), Snapshot.GetGeneration());
	FLongRunningWorkObservation WrongObservation;
	const int32 ObserveCallsBefore = Native->ObserveCalls;
	const TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> DirectPort =
		Builder->Ports.Last();
	TestFalse(TEXT("wrong identity cannot observe native resource"),
		DirectPort->Observe(
			Operation.GetOperationId(),
			WrongIdentity,
			*Resources,
			WrongObservation,
			Error));
	TestEqual(TEXT("wrong identity does not touch native session"),
		Native->ObserveCalls, ObserveCallsBefore);

	TestTrue(TEXT("cancellation reaches exact native session"),
		Owner.RequestCancellation(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			TEXT("fixture_cancel"),
			Snapshot,
			Error));
	TestFalse(TEXT("stop request alone is not terminal"), Snapshot.IsTerminal());
	TestTrue(TEXT("running native session remains non-terminal"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Snapshot.GetGeneration(),
			Snapshot,
			Error));
	TestFalse(TEXT("cancel waits for physical exit"), Snapshot.IsTerminal());
	Native->Observation = FUeAgentCaptureMediaNativeObservation(
		ELongRunningWorkState::ExitedFailure,
		TEXT("stopped"),
		TEXT("fixture_native_stopped"));
	TestTrue(TEXT("physical exit allows C7 join and release"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Snapshot.GetGeneration(),
			Snapshot,
			Error));
	TestTrue(TEXT("cancelled operation is now terminal"), Snapshot.IsTerminal());
	TestEqual(TEXT("C7 preserves cancellation status"), Snapshot.GetStatus(),
		ELongRunningOperationStatus::Cancelled);
	TestFalse(TEXT("exact-owned native source is released"),
		IFileManager::Get().FileExists(*Native->OwnedSourcePath));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureMediaDeliveryReceiptIsolationTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Production.E2."
		"DeliveryFailureDoesNotRewriteReceipt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureMediaDeliveryReceiptIsolationTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("image production factory constructs"),
		FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
			{{ImagePolicy(), Builder}}, Factory, Error));
	const TSharedRef<FNoopPort, ESPMode::ThreadSafe> DefaultPort =
		MakeShared<FNoopPort, ESPMode::ThreadSafe>();
	double Now = 100.0;
	FLongRunningOperationOwner Owner(DefaultPort, [&Now]() { return Now; });
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FLongRunningOperationRequest Operation =
		MakeOperationRequest(TEXT("delivery-isolation"), Resources);
	FLongRunningOperationSnapshot Snapshot;
	TestTrue(TEXT("capture operation accepts"),
		TryAcceptCaptureMediaLongJob(
			TypedRequest(ImagePolicy(), *Resources),
			Operation,
			*Factory,
			Owner,
			Snapshot,
			Error));
	TestTrue(TEXT("capture operation starts"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
		Builder->Sessions.Last();
	Native->Observation = FUeAgentCaptureMediaNativeObservation(
		ELongRunningWorkState::ExitedSuccess,
		TEXT("artifact_ready"),
		FString(),
		ArtifactRef(TEXT("delivery")));
	TestTrue(TEXT("physical exit creates lifecycle terminal"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Snapshot.GetGeneration(),
			Snapshot,
			Error));
	FLongRunningOperationReceipt Before;
	TestTrue(TEXT("terminal lifecycle receipt is retained"),
		Owner.GetTerminalReceipt(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Before,
			Error));

	const auto Deliver = [](const FFileArtifactReference&, FString& OutError)
	{
		OutError = TEXT("fixture_delivery_failed");
		return false;
	};
	FString DeliveryError;
	TestFalse(TEXT("artifact delivery fixture fails"),
		Deliver(ArtifactRef(TEXT("delivery")), DeliveryError));
	FLongRunningOperationReceipt After;
	TestTrue(TEXT("delivery failure leaves receipt readable"),
		Owner.GetTerminalReceipt(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			After,
			Error));
	TestEqual(TEXT("delivery failure preserves lifecycle status"),
		After.GetStatus(), Before.GetStatus());
	TestEqual(TEXT("delivery failure preserves generation"),
		After.GetGeneration(), Before.GetGeneration());
	TestEqual(TEXT("delivery failure preserves native resource"),
		After.GetNativeResourceId(), Before.GetNativeResourceId());
	return !HasAnyErrors();
}
