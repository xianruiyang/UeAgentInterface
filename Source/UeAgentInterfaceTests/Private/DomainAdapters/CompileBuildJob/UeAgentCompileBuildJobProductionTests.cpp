#include "Misc/AutomationTest.h"

#include "Async/TaskGraphInterfaces.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobProduction.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::CompileBuildJob;
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

	FUeAgentDescriptorIdentity DescriptorIdentity(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.compile_build.production.") + Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:aa16a33db14ce64c93071f5baf186a1e"
				"4617c6f949628642a837eba9a8dfdca8"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:ls010");
		Input.EditorInstanceId = TEXT("editor:ls010");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("context:ls010:r1");
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
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& Resources)
	{
		const FUeAgentDescriptorIdentity Descriptor = DescriptorIdentity(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:ls010:") + Suffix,
			TEXT("lease:ls010:owner"),
			TEXT("server-session:ls010"),
			Descriptor,
			MakeContext(Descriptor),
			Resources,
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return FPlatformTime::Seconds(); }));
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
				TEXT("resource:noop:") + Request.GetOperationId(), 1);
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

	class FRecordingBuilder final : public IUeAgentCompileBuildJobPortBuilder
	{
	public:
		bool TryCreatePort(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			++Calls;
			Families.Add(Request.GetFamily());
			Policies.Add(Policy.GetPolicyId());
			const TSharedRef<FNoopPort, ESPMode::ThreadSafe> Port =
				MakeShared<FNoopPort, ESPMode::ThreadSafe>();
			Ports.Add(Port);
			OutPort = Port;
			return true;
		}

		int32 Calls = 0;
		TArray<EUeAgentCompileBuildJobFamily> Families;
		TArray<FString> Policies;
		TArray<TSharedRef<FNoopPort, ESPMode::ThreadSafe>> Ports;
	};

	class FRecordingStage final : public IUeAgentCompileBuildJobGameThreadStage
	{
	public:
		bool Execute(
			const FUeAgentCancellationToken& Cancellation,
			FExecutionMutationError& OutError) override
		{
			++ExecuteCalls;
			bObservedOnGameThread = IsInGameThread();
			bReadbackMarker = true;
			return true;
		}

		void RequestStop(const FString& Reason) override
		{
			++StopCalls;
			LastStopReason = Reason;
		}

		int32 ExecuteCalls = 0;
		int32 StopCalls = 0;
		bool bObservedOnGameThread = false;
		bool bReadbackMarker = false;
		FString LastStopReason;
	};

	class FRecordingOutputSink final
		: public IUeAgentCompileBuildJobProcessOutputSink
	{
	public:
		bool Append(
			const FString& Chunk,
			FExecutionMutationError& OutError) override
		{
			Output += Chunk;
			return true;
		}

		FString GetReportRef() const override
		{
			return TEXT("artifact:ls010:owned-process-report");
		}

		bool Release(FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			return true;
		}

		FString Output;
		int32 ReleaseCalls = 0;
	};

	class FFailOnceOutputSink final
		: public IUeAgentCompileBuildJobProcessOutputSink
	{
	public:
		bool Append(
			const FString& Chunk,
			FExecutionMutationError& OutError) override
		{
			if (FailedAppends == 0)
			{
				++FailedAppends;
				OutError = FExecutionMutationError(
					TEXT("fixture_sink_append_failed"));
				return false;
			}
			Output += Chunk;
			return true;
		}

		FString GetReportRef() const override
		{
			return TEXT("artifact:ls010:fail-once-report");
		}

		bool Release(FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			return true;
		}

		int32 FailedAppends = 0;
		int32 ReleaseCalls = 0;
		FString Output;
	};

	FUeAgentCompileBuildJobPolicyDescriptor Policy(
		const EUeAgentCompileBuildJobFamily Family,
		const FString& PolicyId,
		const FString& Mechanism,
		const FString& Oracle)
	{
		return FUeAgentCompileBuildJobPolicyDescriptor(
			Family, PolicyId, Mechanism, Oracle);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobExactPolicyCatalogTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E0."
		"ExactPolicyCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobExactPolicyCatalogTest::RunTest(
	const FString& Parameters)
{
	TestEqual(TEXT("production contract version"),
		UAI_COMPILE_BUILD_JOB_PRODUCTION_V1, 1);
	const TSharedRef<FRecordingBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingBuilder, ESPMode::ThreadSafe>();
	const TArray<FUeAgentCompileBuildJobPolicyRegistration> Registrations = {
		{Policy(EUeAgentCompileBuildJobFamily::AssetCompile,
			TEXT("policy:asset-compile:standard/v1"),
			TEXT("game_thread_stage"),
			TEXT("uai.readback.asset.compile_status/v1")), Builder},
		{Policy(EUeAgentCompileBuildJobFamily::WorldBuild,
			TEXT("policy:world-build:standard/v1"),
			TEXT("game_thread_stage"),
			TEXT("uai.readback.world.build_state/v1")), Builder},
		{Policy(EUeAgentCompileBuildJobFamily::ExternalProcess,
			TEXT("policy:owned-tool:standard/v1"),
			TEXT("owned_process"),
			TEXT("uai.readback.process.effect/v1")), Builder},
		{Policy(EUeAgentCompileBuildJobFamily::WorldPartitionResave,
			TEXT("policy:world-partition-resave:actors/v1"),
			TEXT("owned_process"),
			TEXT("uai.readback.world_partition.resave/v1")), Builder},
	};
	TSharedPtr<FUeAgentCompileBuildJobProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("immutable exact policy catalog constructs"),
		FUeAgentCompileBuildJobProductionPortFactory::TryCreate(
			Registrations, Factory, Error));
	if (!Factory.IsValid())
	{
		AddError(TEXT("compile_build_job_policy_catalog_unavailable"));
		return false;
	}
	TestEqual(TEXT("all four family policies are retained"),
		Factory->GetPolicyDescriptors().Num(), 4);
	for (const FUeAgentCompileBuildJobPolicyDescriptor& Descriptor
		: Factory->GetPolicyDescriptors())
	{
		TestTrue(TEXT("policy descriptor is valid"), Descriptor.IsValid());
		TestFalse(TEXT("policy declares native mechanism"),
			Descriptor.GetNativeMechanismId().IsEmpty());
		TestTrue(TEXT("policy declares independent readback oracle"),
			Descriptor.GetReadbackOracleId().StartsWith(TEXT("uai.readback.")));
	}

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::AssetCompile,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-compile:standard/v1"),
		EmptyParameters(*Resources));
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> FirstPort;
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> SecondPort;
	TestTrue(TEXT("exact policy creates first operation port"),
		Factory->TryCreateOperationPort(Request, FirstPort, Error));
	TestTrue(TEXT("same policy creates second operation port"),
		Factory->TryCreateOperationPort(Request, SecondPort, Error));
	TestEqual(TEXT("builder called once per operation"), Builder->Calls, 2);
	TestTrue(TEXT("ports are operation scoped and distinct"),
		FirstPort.IsValid() && SecondPort.IsValid()
			&& FirstPort.Get() != SecondPort.Get());

	const FUeAgentCompileBuildJobTypedRequest UnknownPolicy(
		EUeAgentCompileBuildJobFamily::AssetCompile,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-compile:standard/v2"),
		EmptyParameters(*Resources));
	TestFalse(TEXT("unknown suffix is not accepted by prefix"),
		Factory->TryCreateOperationPort(UnknownPolicy, FirstPort, Error));
	TestEqual(TEXT("unknown policy fails with stable code"), Error.GetCode(),
		FString(TEXT("compile_build_job_policy_not_registered")));

	const FUeAgentCompileBuildJobTypedRequest FamilyMismatch(
		EUeAgentCompileBuildJobFamily::WorldBuild,
		TEXT("world:/Game/UAI/Fixture/Map"),
		TEXT("policy:asset-compile:standard/v1"),
		EmptyParameters(*Resources));
	TestFalse(TEXT("family mismatch cannot select another policy"),
		Factory->TryCreateOperationPort(FamilyMismatch, FirstPort, Error));
	TestEqual(TEXT("family mismatch is exact policy miss"), Error.GetCode(),
		FString(TEXT("compile_build_job_policy_not_registered")));

	TArray<FUeAgentCompileBuildJobPolicyRegistration> Duplicate = Registrations;
	Duplicate.Add(Registrations[0]);
	TSharedPtr<FUeAgentCompileBuildJobProductionPortFactory> DuplicateFactory;
	TestFalse(TEXT("duplicate policy registration fails closed"),
		FUeAgentCompileBuildJobProductionPortFactory::TryCreate(
			Duplicate, DuplicateFactory, Error));
	TestEqual(TEXT("duplicate policy has stable error"), Error.GetCode(),
		FString(TEXT("compile_build_job_policy_duplicate")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobGameThreadStageLifecycleTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E1."
		"GameThreadStageLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobGameThreadStageLifecycleTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(TEXT("automation test executes on GameThread"), IsInGameThread()))
	{
		return false;
	}
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::AssetCompile,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-compile:standard/v1"),
		EmptyParameters(*Resources));
	const FUeAgentCompileBuildJobPolicyDescriptor Descriptor = Policy(
		Request.GetFamily(), Request.GetPolicyId(),
		TEXT("game_thread_stage"),
		TEXT("uai.readback.asset.compile_status/v1"));
	const TSharedRef<FRecordingStage, ESPMode::ThreadSafe> Stage =
		MakeShared<FRecordingStage, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("game-thread stage port constructs"),
		TryCreateCompileBuildJobGameThreadStagePort(
			Request, Descriptor, Stage, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	FUeAgentCancellationSource CancellationSource(
		[]() { return FPlatformTime::Seconds(); });
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("start only schedules owned stage"),
		Port->Start(
			MakeOperationRequest(TEXT("game-thread"), Resources),
			CancellationSource.GetToken(), Identity, Error));
	TestTrue(TEXT("work identity is generation bound"), Identity.IsValid());
	TestEqual(TEXT("start does not inline execute stage"), Stage->ExecuteCalls, 0);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("owned stage executes exactly once"), Stage->ExecuteCalls, 1);
	TestTrue(TEXT("stage executes on GameThread"), Stage->bObservedOnGameThread);
	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("observe reports physical exit"),
		Port->Observe(
			TEXT("operation:ls010:game-thread"), Identity,
			*Resources, Observation, Error));
	TestEqual(TEXT("stage exits successfully"), Observation.GetState(),
		ELongRunningWorkState::ExitedSuccess);
	TestEqual(TEXT("observation generation is exact"),
		Observation.GetGeneration(), Identity.GetGeneration());
	TestTrue(TEXT("stage exposes typed signal"), Observation.GetSignal().IsSet());
	if (Observation.GetSignal().IsSet())
	{
		const FLongRunningOperationSignal& Signal =
			Observation.GetSignal().GetValue();
		TestEqual(TEXT("family signal type is exact"), Signal.GetTypeId(),
			FString(TEXT("uai.compile_build.asset_compile.progress/v1")));
		TestEqual(TEXT("signal revision begins at one"),
			Signal.GetRevision(), int64(1));
		TestTrue(TEXT("signal contains independent oracle id"),
			FindObjectValue(Signal.GetPayload(), TEXT("readback_oracle_id"))
				!= nullptr);
		TestTrue(TEXT("signal does not expose executable"),
			FindObjectValue(Signal.GetPayload(), TEXT("executable")) == nullptr);
		TestTrue(TEXT("signal does not expose native Outcome"),
			FindObjectValue(Signal.GetPayload(), TEXT("outcome")) == nullptr);
	}
	FLongRunningWorkObservation RepeatedObservation;
	TestTrue(TEXT("unchanged stage observation remains readable"),
		Port->Observe(
			TEXT("operation:ls010:game-thread"), Identity,
			*Resources, RepeatedObservation, Error));
	TestFalse(TEXT("unchanged stage observation emits no duplicate revision"),
		RepeatedObservation.GetSignal().IsSet());
	TestTrue(TEXT("independent stage readback sees effect"),
		Stage->bReadbackMarker);
	TestTrue(TEXT("join follows actual exit"),
		Port->Join(TEXT("operation:ls010:game-thread"), Identity, Error));
	TestTrue(TEXT("release follows join"),
		Port->Release(TEXT("operation:ls010:game-thread"), Identity, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobWorldBuildStopBeforeStageTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E1B."
		"WorldBuildStopBeforeStage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobWorldBuildStopBeforeStageTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::WorldBuild,
		TEXT("world:/Game/UAI/Fixture/Map"),
		TEXT("policy:world-build:standard/v1"),
		EmptyParameters(*Resources));
	const FUeAgentCompileBuildJobPolicyDescriptor Descriptor = Policy(
		Request.GetFamily(), Request.GetPolicyId(),
		TEXT("game_thread_stage"),
		TEXT("uai.readback.world.build_state/v1"));
	const TSharedRef<FRecordingStage, ESPMode::ThreadSafe> Stage =
		MakeShared<FRecordingStage, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("world-build stage port constructs"),
		TryCreateCompileBuildJobGameThreadStagePort(
			Request, Descriptor, Stage, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	FUeAgentCancellationSource CancellationSource(
		[]() { return FPlatformTime::Seconds(); });
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("world-build stage schedules"),
		Port->Start(
			MakeOperationRequest(TEXT("world-build-stop"), Resources),
			CancellationSource.GetToken(), Identity, Error));
	TestTrue(TEXT("stop request targets exact queued stage"),
		Port->RequestStop(
			TEXT("operation:ls010:world-build-stop"), Identity,
			TEXT("fixture stop before stage"), Error));
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	TestEqual(TEXT("stopped queued stage never executes"),
		Stage->ExecuteCalls, 0);
	TestEqual(TEXT("native stage receives one stop request"),
		Stage->StopCalls, 1);
	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("observe waits for queued stage callback to exit"),
		Port->Observe(
			TEXT("operation:ls010:world-build-stop"), Identity,
			*Resources, Observation, Error));
	TestEqual(TEXT("stopped stage is physically terminal"),
		Observation.GetState(), ELongRunningWorkState::ExitedFailure);
	TestFalse(TEXT("independent world-build marker proves no effect"),
		Stage->bReadbackMarker);
	TestTrue(TEXT("stopped world-build joins after exit"),
		Port->Join(
			TEXT("operation:ls010:world-build-stop"), Identity, Error));
	TestTrue(TEXT("stopped world-build releases after join"),
		Port->Release(
			TEXT("operation:ls010:world-build-stop"), Identity, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobOwnedProcessLifecycleTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E2."
		"OwnedProcessLifecycleAndReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobOwnedProcessLifecycleTest::RunTest(
	const FString& Parameters)
{
	const FString FixtureRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectIntermediateDir(), TEXT("UAI/LS010")));
	IFileManager::Get().MakeDirectory(*FixtureRoot, true);
	const FString MarkerPath = FPaths::Combine(
		FixtureRoot,
		TEXT("owned_process_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ TEXT(".txt"));
	const FString Arguments = FString::Printf(
		TEXT("/D /S /C \"echo UAI_LS010>%s\""), *MarkerPath);
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:ls010"),
		TEXT("policy:owned-tool:fixture/v1"),
		EmptyParameters(*Resources));
	const FUeAgentCompileBuildJobPolicyDescriptor Descriptor = Policy(
		Request.GetFamily(), Request.GetPolicyId(),
		TEXT("owned_process"),
		TEXT("uai.readback.process.effect/v1"));
	const FUeAgentCompileBuildJobProcessLaunchSpec Launch(
		TEXT("cmd.exe"), Arguments, FPaths::ProjectDir());
	const TSharedRef<FRecordingOutputSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FRecordingOutputSink, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("owned process port constructs from trusted spec"),
		TryCreateCompileBuildJobOwnedProcessPort(
			Request, Descriptor, Launch, Sink, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	FUeAgentCancellationSource CancellationSource(
		[]() { return FPlatformTime::Seconds(); });
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("owned process starts"),
		Port->Start(
			MakeOperationRequest(TEXT("process"), Resources),
			CancellationSource.GetToken(), Identity, Error));
	FLongRunningWorkObservation Observation;
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	do
	{
		TestTrue(TEXT("owned process observe succeeds"),
			Port->Observe(
				TEXT("operation:ls010:process"), Identity,
				*Resources, Observation, Error));
		if (Observation.GetState() != ELongRunningWorkState::Running)
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	while (FPlatformTime::Seconds() < Deadline);
	TestEqual(TEXT("owned process physically exits successfully"),
		Observation.GetState(), ELongRunningWorkState::ExitedSuccess);
	TestTrue(TEXT("process signal is typed"), Observation.GetSignal().IsSet());
	if (Observation.GetSignal().IsSet())
	{
		const FUeAgentCanonicalValueRef& Payload =
			Observation.GetSignal()->GetPayload();
		TestTrue(TEXT("report ref comes from owned sink"),
			FindObjectValue(Payload, TEXT("report_ref")) != nullptr);
		TestTrue(TEXT("signal hides executable"),
			FindObjectValue(Payload, TEXT("executable")) == nullptr);
		TestTrue(TEXT("signal hides process id"),
			FindObjectValue(Payload, TEXT("process_id")) == nullptr);
	}
	FLongRunningWorkObservation RepeatedObservation;
	TestTrue(TEXT("unchanged process observation remains readable"),
		Port->Observe(
			TEXT("operation:ls010:process"), Identity,
			*Resources, RepeatedObservation, Error));
	TestFalse(TEXT("unchanged process observation emits no duplicate revision"),
		RepeatedObservation.GetSignal().IsSet());
	TestTrue(TEXT("join follows observed process exit"),
		Port->Join(TEXT("operation:ls010:process"), Identity, Error));
	TestTrue(TEXT("release closes exact owned resources"),
		Port->Release(TEXT("operation:ls010:process"), Identity, Error));
	TestEqual(TEXT("output sink released once"), Sink->ReleaseCalls, 1);
	FString Marker;
	TestTrue(TEXT("independent marker readback proves process effect"),
		FFileHelper::LoadFileToString(Marker, *MarkerPath)
			&& Marker.Contains(TEXT("UAI_LS010")));
	TestTrue(TEXT("temporary marker cleanup succeeds"),
		IFileManager::Get().Delete(*MarkerPath, false, true, true));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobProcessSinkFailureOwnershipTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E2B."
		"SinkFailureDoesNotForgeExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobProcessSinkFailureOwnershipTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::ExternalProcess,
		TEXT("project:ls010"),
		TEXT("policy:owned-tool:sink-failure/v1"),
		EmptyParameters(*Resources));
	const FUeAgentCompileBuildJobPolicyDescriptor Descriptor = Policy(
		Request.GetFamily(), Request.GetPolicyId(),
		TEXT("owned_process"),
		TEXT("uai.readback.process.effect/v1"));
	const FUeAgentCompileBuildJobProcessLaunchSpec Launch(
		TEXT("cmd.exe"),
		TEXT("/D /S /C \"echo OUTPUT & ping -n 20 127.0.0.1 >nul\""),
		FPaths::ProjectDir());
	const TSharedRef<FFailOnceOutputSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FFailOnceOutputSink, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("sink-failure process port constructs"),
		TryCreateCompileBuildJobOwnedProcessPort(
			Request, Descriptor, Launch, Sink, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	FUeAgentCancellationSource CancellationSource(
		[]() { return FPlatformTime::Seconds(); });
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("sink-failure fixture process starts"),
		Port->Start(
			MakeOperationRequest(TEXT("sink-failure"), Resources),
			CancellationSource.GetToken(), Identity, Error));
	FLongRunningWorkObservation Observation;
	bool bSawExpectedSinkFailure = false;
	const double FailureDeadline = FPlatformTime::Seconds() + 5.0;
	do
	{
		const bool bObserved = Port->Observe(
			TEXT("operation:ls010:sink-failure"), Identity,
			*Resources, Observation, Error);
		if (Sink->FailedAppends > 0)
		{
			if (bObserved)
			{
				AddError(TEXT("sink failure forged a successful observation"));
				Port.Reset();
				return false;
			}
			bSawExpectedSinkFailure = true;
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	while (FPlatformTime::Seconds() < FailureDeadline);
	TestTrue(TEXT("output sink failure is reported"), bSawExpectedSinkFailure);
	const bool bJoinedBeforeExit = Port->Join(
		TEXT("operation:ls010:sink-failure"), Identity, Error);
	TestFalse(TEXT("sink failure cannot be used as physical exit"),
		bJoinedBeforeExit);
	if (bJoinedBeforeExit)
	{
		Port.Reset();
		return false;
	}
	TestTrue(TEXT("exact process remains stoppable after sink failure"),
		Port->RequestStop(
			TEXT("operation:ls010:sink-failure"), Identity,
			TEXT("fixture cleanup"), Error));
	const double ExitDeadline = FPlatformTime::Seconds() + 10.0;
	do
	{
		if (!Port->Observe(
				TEXT("operation:ls010:sink-failure"), Identity,
				*Resources, Observation, Error))
		{
			AddError(TEXT("observe failed after one-shot sink recovery"));
			break;
		}
		if (Observation.GetState() != ELongRunningWorkState::Running)
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	while (FPlatformTime::Seconds() < ExitDeadline);
	TestTrue(TEXT("physical process exit is eventually observed"),
		Observation.GetState() == ELongRunningWorkState::ExitedFailure
			|| Observation.GetState() == ELongRunningWorkState::ExitedSuccess);
	TestTrue(TEXT("process joins after physical exit"),
		Port->Join(
			TEXT("operation:ls010:sink-failure"), Identity, Error));
	TestTrue(TEXT("process releases after join"),
		Port->Release(
			TEXT("operation:ls010:sink-failure"), Identity, Error));
	TestEqual(TEXT("recovered sink releases once"), Sink->ReleaseCalls, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCompileBuildJobWorldPartitionResaveStopTest,
	"UeAgentInterface.DomainAdapters.CompileBuildJob.Production.E3."
		"WorldPartitionResaveStopAndReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCompileBuildJobWorldPartitionResaveStopTest::RunTest(
	const FString& Parameters)
{
	const FString FixtureRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectIntermediateDir(), TEXT("UAI/LS010")));
	IFileManager::Get().MakeDirectory(*FixtureRoot, true);
	const FString FixtureId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString StartedPath = FPaths::Combine(
		FixtureRoot, TEXT("resave_started_") + FixtureId + TEXT(".txt"));
	const FString CompletedPath = FPaths::Combine(
		FixtureRoot, TEXT("resave_completed_") + FixtureId + TEXT(".txt"));
	const FString Arguments = FString::Printf(
		TEXT("/D /S /C \"echo STARTED>%s & ping -n 20 127.0.0.1 >nul "
			"& echo COMPLETED>%s\""),
		*StartedPath, *CompletedPath);
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentCompileBuildJobTypedRequest Request(
		EUeAgentCompileBuildJobFamily::WorldPartitionResave,
		TEXT("world:/Game/UAI/Fixture/WP_Map"),
		TEXT("policy:world-partition-resave:actors/v1"),
		EmptyParameters(*Resources));
	const FUeAgentCompileBuildJobPolicyDescriptor Descriptor = Policy(
		Request.GetFamily(), Request.GetPolicyId(),
		TEXT("owned_process"),
		TEXT("uai.readback.world_partition.resave/v1"));
	const FUeAgentCompileBuildJobProcessLaunchSpec Launch(
		TEXT("cmd.exe"), Arguments, FPaths::ProjectDir());
	const TSharedRef<FRecordingOutputSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FRecordingOutputSink, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("world-partition resave process port constructs"),
		TryCreateCompileBuildJobOwnedProcessPort(
			Request, Descriptor, Launch, Sink, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	FUeAgentCancellationSource CancellationSource(
		[]() { return FPlatformTime::Seconds(); });
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("world-partition resave process starts"),
		Port->Start(
			MakeOperationRequest(TEXT("resave-stop"), Resources),
			CancellationSource.GetToken(), Identity, Error));
	FLongRunningWorkObservation Observation;
	const double StartDeadline = FPlatformTime::Seconds() + 5.0;
	do
	{
		if (!Port->Observe(
				TEXT("operation:ls010:resave-stop"), Identity,
				*Resources, Observation, Error))
		{
			AddError(TEXT("resave observe failed before stop"));
			break;
		}
		if (IFileManager::Get().FileExists(*StartedPath))
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	while (FPlatformTime::Seconds() < StartDeadline);
	TestTrue(TEXT("independent readback sees process start"),
		IFileManager::Get().FileExists(*StartedPath));
	TestEqual(TEXT("resave remains running before stop"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestTrue(TEXT("safe stop targets exact owned process"),
		Port->RequestStop(
			TEXT("operation:ls010:resave-stop"), Identity,
			TEXT("fixture cancellation"), Error));
	const double ExitDeadline = FPlatformTime::Seconds() + 10.0;
	do
	{
		if (!Port->Observe(
				TEXT("operation:ls010:resave-stop"), Identity,
				*Resources, Observation, Error))
		{
			AddError(TEXT("resave observe failed after stop"));
			break;
		}
		if (Observation.GetState() != ELongRunningWorkState::Running)
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	while (FPlatformTime::Seconds() < ExitDeadline);
	TestTrue(TEXT("stop does not report terminal until physical exit"),
		Observation.GetState() == ELongRunningWorkState::ExitedFailure
			|| Observation.GetState() == ELongRunningWorkState::ExitedSuccess);
	TestFalse(TEXT("independent readback proves completion was stopped"),
		IFileManager::Get().FileExists(*CompletedPath));
	TestTrue(TEXT("stopped resave joins after physical exit"),
		Port->Join(
			TEXT("operation:ls010:resave-stop"), Identity, Error));
	TestTrue(TEXT("stopped resave releases exact owned resources"),
		Port->Release(
			TEXT("operation:ls010:resave-stop"), Identity, Error));
	TestEqual(TEXT("resave output sink releases once"),
		Sink->ReleaseCalls, 1);
	TestTrue(TEXT("started marker cleanup succeeds"),
		IFileManager::Get().Delete(*StartedPath, false, true, true));
	if (IFileManager::Get().FileExists(*CompletedPath))
	{
		IFileManager::Get().Delete(*CompletedPath, false, true, true);
	}
	return !HasAnyErrors();
}
