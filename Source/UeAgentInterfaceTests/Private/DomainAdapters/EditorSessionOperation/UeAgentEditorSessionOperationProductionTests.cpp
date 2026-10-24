#include "Misc/AutomationTest.h"

#include "Containers/Ticker.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/PlatformTime.h"

#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::EditorSessionOperation;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
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
			TEXT("uai.test.editor_session.production.") + Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:ba16a33db14ce64c93071f5baf186a1e"
				"4617c6f949628642a837eba9a8dfdca9"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor,
		const EUeAgentStatePlane StatePlane,
		const EUeAgentExecutionWorldType WorldType,
		const FString& EditorInstanceId = TEXT("editor-instance:ls011-owned"))
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:ls011");
		Input.EditorInstanceId = EditorInstanceId;
		Input.StatePlane = StatePlane;
		Input.WorldType = WorldType;
		Input.WorldIdentity = WorldType == EUeAgentExecutionWorldType::Invalid
			? FString()
			: FString(TEXT("world:ls011"));
		if (WorldType == EUeAgentExecutionWorldType::Pie
			|| WorldType == EUeAgentExecutionWorldType::SimulateInEditor)
		{
			Input.PieInstanceId = 0;
		}
		Input.ContextRevision = TEXT("context:ls011:r1");
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
			EUeAgentExecutionWorldType::Invalid,
		const FString& EditorInstanceId = TEXT("editor-instance:ls011-owned"))
	{
		const FUeAgentDescriptorIdentity Descriptor = DescriptorIdentity(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:ls011:") + Suffix,
			TEXT("lease:ls011:owner"),
			TEXT("server-session:ls011"),
			Descriptor,
			MakeContext(Descriptor, StatePlane, WorldType, EditorInstanceId),
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
				TEXT("native:editor-session:noop:") + Request.GetOperationId(), 1);
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

	class FRecordingBuilder final
		: public IUeAgentEditorSessionOperationPortBuilder
	{
	public:
		bool TryCreatePort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FLongRunningOperationRequest& OperationRequest,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
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
		TArray<EUeAgentEditorSessionOperationFamily> Families;
		TArray<FString> Policies;
		TArray<TSharedRef<FNoopPort, ESPMode::ThreadSafe>> Ports;
	};

	class FRecordingNativeSession final
		: public IUeAgentEditorSessionNativeSession
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
			OutNativeResourceId = TEXT("native-session:ls011-owned");
			return true;
		}

		bool Observe(
			FUeAgentEditorSessionNativeObservation& OutObservation,
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
			return true;
		}

		FUeAgentEditorSessionNativeObservation Observation = {
			ELongRunningWorkState::Running,
			TEXT("running"),
			FString(),
			TEXT("session:ls011-owned")};
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		FString LastOperationId;
		FString LastStopReason;
	};

	class FRecordingNativeBuilder final
		: public IUeAgentEditorSessionOperationPortBuilder
	{
	public:
		bool TryCreatePort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FLongRunningOperationRequest& OperationRequest,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
			FExecutionMutationError& OutError) override
		{
			const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
				MakeShared<FRecordingNativeSession, ESPMode::ThreadSafe>();
			Sessions.Add(Native);
			return TryCreateEditorSessionNativeResourcePort(
				Request, Policy, Native, OutPort, OutError);
		}

		TArray<TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe>> Sessions;
	};

	class FRecordingHostExitTarget final
		: public IUeAgentEditorHostExitTarget
	{
	public:
		explicit FRecordingHostExitTarget(FString InEditorInstanceId)
			: EditorInstanceId(MoveTemp(InEditorInstanceId))
		{
		}

		const FString& GetEditorInstanceId() const override
		{
			return EditorInstanceId;
		}

		void RequestExit() override
		{
			++RequestExitCalls;
		}

		FString EditorInstanceId;
		int32 RequestExitCalls = 0;
	};

	FUeAgentEditorSessionOperationPolicyDescriptor Policy(
		const EUeAgentEditorSessionOperationFamily Family,
		const FString& PolicyId,
		const FString& Mechanism,
		const FString& Oracle,
		const EUeAgentStatePlane StatePlane,
		TArray<EUeAgentExecutionWorldType> AllowedWorldTypes = {})
	{
		return FUeAgentEditorSessionOperationPolicyDescriptor(
			Family,
			PolicyId,
			Mechanism,
			Oracle,
			StatePlane,
			MoveTemp(AllowedWorldTypes));
	}

	FUeAgentEditorSessionOperationPolicyDescriptor AssetPolicy()
	{
		return Policy(
			EUeAgentEditorSessionOperationFamily::AssetEditor,
			TEXT("policy:asset-editor:open/v1"),
			TEXT("native_session_resource"),
			TEXT("uai.readback.editor.asset_editor/v1"),
			EUeAgentStatePlane::AuthoringMemory);
	}

	FUeAgentEditorSessionOperationPolicyDescriptor HostPolicy()
	{
		return Policy(
			EUeAgentEditorSessionOperationFamily::HostLifecycle,
			TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
			TEXT("host_exit_schedule"),
			TEXT("uai.readback.editor.host_exit/v1"),
			EUeAgentStatePlane::RuntimeInstance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionExactPolicyCatalogTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E0."
		"ExactPolicyCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionExactPolicyCatalogTest::RunTest(
	const FString& Parameters)
{
	TestEqual(TEXT("production contract version"),
		UAI_EDITOR_SESSION_OPERATION_PRODUCTION_V1, 1);
	const TSharedRef<FRecordingBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingBuilder, ESPMode::ThreadSafe>();
	const TArray<FUeAgentEditorSessionOperationPolicyRegistration>
		Registrations = {
			{AssetPolicy(), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:pie/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.runtime_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Pie,
					EUeAgentExecutionWorldType::SimulateInEditor,
					EUeAgentExecutionWorldType::Game,
					EUeAgentExecutionWorldType::Preview}), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:set-current/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.world_context/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::ModelingTool,
				TEXT("policy:modeling-tool:invoke-action/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.modeling_tool/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}), Builder},
			{HostPolicy(), Builder},
		};
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("immutable exact policy catalog constructs"),
		FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
			Registrations, Factory, Error));
	if (!Factory.IsValid())
	{
		AddError(TEXT("editor_session_policy_catalog_unavailable"));
		return false;
	}
	for (const FUeAgentEditorSessionOperationPolicyDescriptor& Descriptor
		: Factory->GetPolicyDescriptors())
	{
		TestTrue(TEXT("policy descriptor is valid"), Descriptor.IsValid());
		TestTrue(TEXT("policy declares independent readback oracle"),
			Descriptor.GetReadbackOracleId().StartsWith(TEXT("uai.readback.")));
		TestNotEqual(TEXT("policy declares an exact state plane"),
			Descriptor.GetRequiredStatePlane(), EUeAgentStatePlane::Invalid);
	}

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::AssetEditor,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-editor:open/v1"),
		EmptyParameters(*Resources));
	const FLongRunningOperationRequest Operation = MakeOperationRequest(
		TEXT("policy-factory"), Resources);
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> FirstPort;
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> SecondPort;
	TestTrue(TEXT("exact policy creates first operation port"),
		Factory->TryCreateOperationPort(Request, Operation, FirstPort, Error));
	TestTrue(TEXT("same policy creates second operation port"),
		Factory->TryCreateOperationPort(Request, Operation, SecondPort, Error));
	TestEqual(TEXT("builder called once per operation"), Builder->Calls, 2);
	TestTrue(TEXT("ports are operation scoped and distinct"),
		FirstPort.IsValid() && SecondPort.IsValid()
			&& FirstPort.Get() != SecondPort.Get());

	const FUeAgentEditorSessionOperationTypedRequest UnknownPolicy(
		EUeAgentEditorSessionOperationFamily::AssetEditor,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-editor:open/v2"),
		EmptyParameters(*Resources));
	TestFalse(TEXT("unknown suffix is not accepted by prefix"),
		Factory->TryCreateOperationPort(
			UnknownPolicy, Operation, FirstPort, Error));
	TestEqual(TEXT("unknown policy fails with stable code"), Error.GetCode(),
		FString(TEXT("editor_session_operation_policy_not_registered")));

	const FUeAgentEditorSessionOperationTypedRequest FamilyMismatch(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/UAI/Fixture/Map"),
		TEXT("policy:asset-editor:open/v1"),
		EmptyParameters(*Resources));
	TestFalse(TEXT("family mismatch cannot select another policy"),
		Factory->TryCreateOperationPort(
			FamilyMismatch, Operation, FirstPort, Error));
	TestEqual(TEXT("family mismatch is exact policy miss"), Error.GetCode(),
		FString(TEXT("editor_session_operation_policy_not_registered")));

	TArray<FUeAgentEditorSessionOperationPolicyRegistration> Duplicate =
		Registrations;
	Duplicate.Add(Registrations[0]);
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory>
		DuplicateFactory;
	TestFalse(TEXT("duplicate policy registration fails closed"),
		FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
			Duplicate, DuplicateFactory, Error));
	TestEqual(TEXT("duplicate policy has stable error"), Error.GetCode(),
		FString(TEXT("editor_session_operation_policy_duplicate")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionFourFamilyNativePortTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E1B."
		"FourFamilyNativePortCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionFourFamilyNativePortTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	const TArray<FUeAgentEditorSessionOperationPolicyRegistration>
		Registrations = {
			{AssetPolicy(), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::RuntimePreview,
				TEXT("policy:runtime-session:pie/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.runtime_preview/v1"),
				EUeAgentStatePlane::RuntimeInstance,
				{EUeAgentExecutionWorldType::Pie}), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::WorldContext,
				TEXT("policy:world-context:create-blank/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.world_context/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}), Builder},
			{Policy(EUeAgentEditorSessionOperationFamily::ModelingTool,
				TEXT("policy:modeling-tool:invoke-action/v1"),
				TEXT("native_session_resource"),
				TEXT("uai.readback.editor.modeling_tool/v1"),
				EUeAgentStatePlane::AuthoringMemory,
				{EUeAgentExecutionWorldType::Editor}), Builder},
		};
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("four-family production factory constructs"),
		FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
			Registrations, Factory, Error));
	if (!Factory.IsValid())
	{
		return false;
	}

	struct FFamilyCase final
	{
		EUeAgentEditorSessionOperationFamily Family;
		const TCHAR* SubjectRef;
		const TCHAR* PolicyId;
		EUeAgentStatePlane StatePlane;
		EUeAgentExecutionWorldType WorldType;
	};
	const TArray<FFamilyCase> Cases = {
		{EUeAgentEditorSessionOperationFamily::AssetEditor,
			TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
			TEXT("policy:asset-editor:open/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			EUeAgentExecutionWorldType::Invalid},
		{EUeAgentEditorSessionOperationFamily::RuntimePreview,
			TEXT("pie-session:ls011-owned"),
			TEXT("policy:runtime-session:pie/v1"),
			EUeAgentStatePlane::RuntimeInstance,
			EUeAgentExecutionWorldType::Pie},
		{EUeAgentEditorSessionOperationFamily::WorldContext,
			TEXT("world:/Game/UAI/Fixture/Map"),
			TEXT("policy:world-context:create-blank/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			EUeAgentExecutionWorldType::Editor},
		{EUeAgentEditorSessionOperationFamily::ModelingTool,
			TEXT("modeling-session:ls011-owned"),
			TEXT("policy:modeling-tool:invoke-action/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			EUeAgentExecutionWorldType::Editor},
	};
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FFamilyCase& Case = Cases[Index];
		const FUeAgentEditorSessionOperationTypedRequest Request(
			Case.Family,
			Case.SubjectRef,
			Case.PolicyId,
			EmptyParameters(*Resources));
		const FLongRunningOperationRequest Operation = MakeOperationRequest(
			FString::Printf(TEXT("family-%d"), Index),
			Resources,
			Case.StatePlane,
			Case.WorldType);
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
		TestTrue(TEXT("family creates production native port"),
			Factory->TryCreateOperationPort(Request, Operation, Port, Error));
		if (!Port.IsValid())
		{
			continue;
		}
		FLongRunningWorkIdentity Identity;
		TestTrue(TEXT("family starts one operation-scoped native session"),
			Port->Start(Operation, UncancelledToken(), Identity, Error));
		const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
			Builder->Sessions.Last();
		TestEqual(TEXT("family native session starts once"),
			Native->StartCalls, 1);
		FLongRunningWorkObservation Observation;
		TestTrue(TEXT("family running state is observable"),
			Port->Observe(
				Operation.GetOperationId(),
				Identity,
				*Resources,
				Observation,
				Error));
		Native->Observation = FUeAgentEditorSessionNativeObservation(
			ELongRunningWorkState::ExitedSuccess,
			TEXT("native_exited"),
			FString(),
			TEXT("session:ls011-owned"));
		TestTrue(TEXT("family physical exit is observable"),
			Port->Observe(
				Operation.GetOperationId(),
				Identity,
				*Resources,
				Observation,
				Error));
		TestTrue(TEXT("family native session joins"),
			Port->Join(Operation.GetOperationId(), Identity, Error));
		TestTrue(TEXT("family native session releases"),
			Port->Release(Operation.GetOperationId(), Identity, Error));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionNativeLifecycleTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E1."
		"NativeSessionLifecycleAndSignal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionNativeLifecycleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::AssetEditor,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-editor:open/v1"),
		EmptyParameters(*Resources));
	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
		MakeShared<FRecordingNativeSession, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("native resource port constructs"),
		TryCreateEditorSessionNativeResourcePort(
			Request, AssetPolicy(), Native, Port, Error));
	if (!Port.IsValid())
	{
		return false;
	}
	const FLongRunningOperationRequest Operation =
		MakeOperationRequest(TEXT("native-lifecycle"), Resources);
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("native session starts"),
		Port->Start(
			Operation,
			UncancelledToken(),
			Identity,
			Error));
	TestEqual(TEXT("one exact native session starts"), Native->StartCalls, 1);
	TestTrue(TEXT("native resource identity is operation scoped"),
		Identity.GetResourceId().Contains(TEXT("native-session:ls011-owned")));

	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("running native session is observed"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestEqual(TEXT("native session remains running"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestTrue(TEXT("first phase emits typed signal"),
		Observation.GetSignal().IsSet());
	if (Observation.GetSignal().IsSet())
	{
		const FUeAgentCanonicalValueRef* Phase = FindObjectValue(
			Observation.GetSignal()->GetPayload(), TEXT("phase"));
		const FUeAgentCanonicalValueRef* Session = FindObjectValue(
			Observation.GetSignal()->GetPayload(), TEXT("session_ref"));
		TestTrue(TEXT("signal phase is canonical string"),
			Phase && (*Phase)->GetKind() == EUeAgentValueKind::String
				&& (*Phase)->GetText() == TEXT("running"));
		TestTrue(TEXT("signal carries exact session ref"),
			Session && (*Session)->GetKind() == EUeAgentValueKind::String
				&& (*Session)->GetText() == TEXT("session:ls011-owned"));
	}
	TestTrue(TEXT("unchanged phase remains observable"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestFalse(TEXT("unchanged phase does not forge a new revision"),
		Observation.GetSignal().IsSet());

	Native->Observation = FUeAgentEditorSessionNativeObservation(
		ELongRunningWorkState::ExitedSuccess,
		TEXT("native_exited"),
		FString(),
		TEXT("session:ls011-owned"));
	TestTrue(TEXT("physical native exit is observed"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestEqual(TEXT("physical native exit maps to lifecycle only"),
		Observation.GetState(), ELongRunningWorkState::ExitedSuccess);
	TestTrue(TEXT("port joins only after native exit"),
		Port->Join(Operation.GetOperationId(), Identity, Error));
	TestTrue(TEXT("port releases exact native resource after join"),
		Port->Release(Operation.GetOperationId(), Identity, Error));
	TestEqual(TEXT("native join called once"), Native->JoinCalls, 1);
	TestEqual(TEXT("native release called once"), Native->ReleaseCalls, 1);

	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> WrongNative =
		MakeShared<FRecordingNativeSession, ESPMode::ThreadSafe>();
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> WrongPort;
	TestTrue(TEXT("second native resource port constructs"),
		TryCreateEditorSessionNativeResourcePort(
			Request, AssetPolicy(), WrongNative, WrongPort, Error));
	const FLongRunningOperationRequest WrongContext = MakeOperationRequest(
		TEXT("wrong-context"),
		Resources,
		EUeAgentStatePlane::RuntimeInstance);
	TestFalse(TEXT("wrong state plane cannot start native session"),
		WrongPort->Start(
			WrongContext,
			UncancelledToken(),
			Identity,
			Error));
	TestEqual(TEXT("wrong state plane has stable error"), Error.GetCode(),
		FString(TEXT(
			"editor_session_operation_context_state_plane_mismatch")));
	TestEqual(TEXT("context rejection starts no native resource"),
		WrongNative->StartCalls, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionCancellationLifecycleTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E1."
		"CancellationWaitsForNativeExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionCancellationLifecycleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FRecordingNativeBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingNativeBuilder, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory> Factory;
	FExecutionMutationError Error;
	TestTrue(TEXT("single-policy production factory constructs"),
		FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
			{{AssetPolicy(), Builder}}, Factory, Error));
	if (!Factory.IsValid())
	{
		return false;
	}
	const TSharedRef<FNoopPort, ESPMode::ThreadSafe> DefaultPort =
		MakeShared<FNoopPort, ESPMode::ThreadSafe>();
	double Now = 100.0;
	FLongRunningOperationOwner Owner(
		DefaultPort, [&Now]() { return Now; });
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::AssetEditor,
		TEXT("asset:/Game/UAI/Fixture.BP_Fixture"),
		TEXT("policy:asset-editor:open/v1"),
		EmptyParameters(*Resources));
	const FLongRunningOperationRequest Operation =
		MakeOperationRequest(TEXT("cancellation"), Resources);
	FLongRunningOperationSnapshot Snapshot;
	TestTrue(TEXT("operation accepts through production factory"),
		TryAcceptEditorSessionOperation(
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
		AddError(TEXT("production builder did not create native session"));
		return false;
	}
	const TSharedRef<FRecordingNativeSession, ESPMode::ThreadSafe> Native =
		Builder->Sessions.Last();
	TestTrue(TEXT("cancellation reaches exact native session"),
		Owner.RequestCancellation(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			TEXT("fixture_cancel"),
			Snapshot,
			Error));
	TestEqual(TEXT("native stop requested once"), Native->StopCalls, 1);
	TestFalse(TEXT("stop request alone is not terminal"), Snapshot.IsTerminal());
	TestTrue(TEXT("running native session remains non-terminal after stop"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Snapshot.GetGeneration(),
			Snapshot,
			Error));
	TestFalse(TEXT("cancel waits for physical native exit"),
		Snapshot.IsTerminal());

	Native->Observation = FUeAgentEditorSessionNativeObservation(
		ELongRunningWorkState::ExitedFailure,
		TEXT("stopped"),
		TEXT("fixture_native_stopped"),
		TEXT("session:ls011-owned"));
	TestTrue(TEXT("native exit allows C7 join and release"),
		Owner.Advance(
			Operation.GetOperationId(),
			Operation.GetOwnerLeaseId(),
			Operation.GetServerSessionId(),
			Snapshot.GetGeneration(),
			Snapshot,
			Error));
	TestTrue(TEXT("cancelled operation is terminal after native exit"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("C7 preserves cancellation status"),
		Snapshot.GetStatus(), ELongRunningOperationStatus::Cancelled);
	TestEqual(TEXT("native join called once"), Native->JoinCalls, 1);
	TestEqual(TEXT("native release called once"), Native->ReleaseCalls, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionOwnedHostCancellationTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E1."
		"OwnedHostScheduleCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionOwnedHostCancellationTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(TEXT("host ticker test executes on GameThread"),
		IsInGameThread()))
	{
		return false;
	}
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::HostLifecycle,
		TEXT("editor-instance:ls011-owned"),
		TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
		EmptyParameters(*Resources));
	const TSharedRef<FRecordingHostExitTarget, ESPMode::ThreadSafe> Target =
		MakeShared<FRecordingHostExitTarget, ESPMode::ThreadSafe>(
			TEXT("editor-instance:ls011-owned"));
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("owned host schedule port constructs"),
		TryCreateEditorHostExitSchedulePort(
			Request, HostPolicy(), 60.0, Target, Port, Error));
	const FLongRunningOperationRequest Operation = MakeOperationRequest(
		TEXT("host-cancel"),
		Resources,
		EUeAgentStatePlane::RuntimeInstance);
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("host exit ticker is scheduled"),
		Port->Start(
			Operation,
			UncancelledToken(),
			Identity,
			Error));
	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("scheduled exit is observable"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestEqual(TEXT("exit_scheduled remains running"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestTrue(TEXT("exit_scheduled emits typed signal"),
		Observation.GetSignal().IsSet());
	TestTrue(TEXT("stop removes the exact owned ticker"),
		Port->RequestStop(
			Operation.GetOperationId(),
			Identity,
			TEXT("fixture_cancel_before_exit"),
			Error));
	TestTrue(TEXT("cancelled ticker quiescence is observable"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestEqual(TEXT("cancelled ticker is a physical failed exit"),
		Observation.GetState(), ELongRunningWorkState::ExitedFailure);
	TestEqual(TEXT("owned target was never called"),
		Target->RequestExitCalls, 0);
	TestTrue(TEXT("cancelled ticker can join"),
		Port->Join(Operation.GetOperationId(), Identity, Error));
	TestTrue(TEXT("joined ticker can release"),
		Port->Release(Operation.GetOperationId(), Identity, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionHostWitnessTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.Production.E1."
		"HostExitRequiresExternalWitness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionHostWitnessTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(TEXT("host witness test executes on GameThread"),
		IsInGameThread()))
	{
		return false;
	}
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeResources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::HostLifecycle,
		TEXT("editor-instance:ls011-owned"),
		TEXT("policy:host-lifecycle:prepare-safe-exit/v1"),
		EmptyParameters(*Resources));
	const TSharedRef<FRecordingHostExitTarget, ESPMode::ThreadSafe> Target =
		MakeShared<FRecordingHostExitTarget, ESPMode::ThreadSafe>(
			TEXT("editor-instance:ls011-owned"));
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	TestTrue(TEXT("immediate owned host schedule constructs"),
		TryCreateEditorHostExitSchedulePort(
			Request, HostPolicy(), 0.0, Target, Port, Error));
	const FLongRunningOperationRequest Operation = MakeOperationRequest(
		TEXT("host-witness"),
		Resources,
		EUeAgentStatePlane::RuntimeInstance);
	FLongRunningWorkIdentity Identity;
	TestTrue(TEXT("owned host exit schedules"),
		Port->Start(
			Operation,
			UncancelledToken(),
			Identity,
			Error));
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("owned exit target called exactly once"),
		Target->RequestExitCalls, 1);
	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("same process can observe only exit request"),
		Port->Observe(
			Operation.GetOperationId(),
			Identity,
			*Resources,
			Observation,
			Error));
	TestEqual(TEXT("target return does not forge host exit"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestTrue(TEXT("exit request observation has typed signal"),
		Observation.GetSignal().IsSet());
	if (Observation.GetSignal().IsSet())
	{
		const FUeAgentCanonicalValueRef* Phase = FindObjectValue(
			Observation.GetSignal()->GetPayload(), TEXT("phase"));
		TestTrue(TEXT("same-process phase requires external witness"),
			Phase && (*Phase)->GetKind() == EUeAgentValueKind::String
				&& (*Phase)->GetText()
					== TEXT("exit_requested_awaiting_external_witness"));
	}
	TestFalse(TEXT("same process cannot join an unproven host exit"),
		Port->Join(Operation.GetOperationId(), Identity, Error));
	TestEqual(TEXT("external witness requirement has stable error"),
		Error.GetCode(),
		FString(TEXT("editor_host_exit_requires_external_witness")));
	Port.Reset();
	return !HasAnyErrors();
}
