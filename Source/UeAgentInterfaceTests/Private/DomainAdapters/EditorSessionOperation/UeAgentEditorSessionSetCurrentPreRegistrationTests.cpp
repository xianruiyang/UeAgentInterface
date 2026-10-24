#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery::Command;
	using namespace UE::UAI::DomainAdapters::EditorSessionOperation;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentCanonicalValueRef EmptyParameters(
		FUeAgentResourceTracker& ResourceTracker)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			{}, ResourceTracker, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentDescriptorIdentity Descriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.editor_session_pre_registration.")
				+ Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:f33691825429f378879ebd26890ec047")
				TEXT("ec30538212080bfc0d0ba5b4e6ec3f3d"));
	}

	FUeAgentExecutionContextRefOwner Context(
		const FUeAgentDescriptorIdentity& Identity)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:session-pre-registration");
		Input.EditorInstanceId = TEXT("fixture-editor:session-pre-registration");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.WorldIdentity = TEXT("world:/Game/UAI/Fixture/ExistingMap");
		Input.ContextRevision = TEXT("fixture-context:session-pre-registration:r1");
		FUeAgentExecutionContextRefPtr Result;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Identity,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::TemporaryMutationAllowed),
			Result,
			Error));
		return Result.ToSharedRef();
	}

	FLongRunningOperationRequest OperationRequest(
		const FString& Suffix,
		const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>&
			ResourceTracker,
		double& Now)
	{
		const FUeAgentDescriptorIdentity Identity = Descriptor(Suffix);
		return FLongRunningOperationRequest(
			TEXT("operation:session-pre-registration:") + Suffix,
			TEXT("lease:session-pre-registration"),
			TEXT("server-session:session-pre-registration"),
			Identity,
			Context(Identity),
			ResourceTracker,
			FUeAgentOperationDeadline::FromTimeout(
				30.0, [&Now]() { return Now; }));
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
				TEXT("native:") + Request.GetOperationId(), 1);
			return true;
		}
		bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FUeAgentResourceTracker& ResourceTracker,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			OutObservation = FLongRunningWorkObservation(
				Identity.GetGeneration(),
				ELongRunningWorkState::Running,
				FString());
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
			OutPort = MakeShared<FNoopPort, ESPMode::ThreadSafe>();
			return true;
		}

		int32 Calls = 0;
	};

	class FRecordingTargetResolver final
		: public IUeAgentEditorSessionOperationTargetResolver
	{
	public:
		bool TryResolveTarget(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			FExecutionMutationError& OutError) override
		{
			++Calls;
			LastSubjectRef = Request.GetSubjectRef();
			if (bTargetExists)
			{
				return true;
			}
			OutError = FExecutionMutationError(
				TEXT("world_context_map_not_found"),
				Request.GetSubjectRef());
			return false;
		}

		bool bTargetExists = false;
		int32 Calls = 0;
		FString LastSubjectRef;
	};

	class FActiveNativeSession final
		: public IUeAgentEditorSessionNativeSession
	{
	public:
		bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FString& OutNativeResourceId,
			FExecutionMutationError& OutError) override
		{
			OutNativeResourceId = TEXT("native:ready-signal");
			return true;
		}
		bool Observe(
			FUeAgentEditorSessionNativeObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			OutObservation = FUeAgentEditorSessionNativeObservation(
				ELongRunningWorkState::Running,
				TEXT("active"),
				FString(),
				TEXT("world-context:/Game/UAI/Fixture/ExistingMap"));
			return true;
		}
		bool RequestStop(
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			return true;
		}
		bool Join(FExecutionMutationError& OutError) override
		{
			return true;
		}
		bool Release(FExecutionMutationError& OutError) override
		{
			return true;
		}
	};

	const FUeAgentCanonicalValueRef* FindObjectValue(
		const FUeAgentCanonicalValueRef& Object,
		const FString& Key)
	{
		const TPair<FString, FUeAgentCanonicalValueRef>* Found =
			Object->GetObjectItems().FindByPredicate(
			[&Key](const TPair<FString, FUeAgentCanonicalValueRef>& Pair)
			{
				return Pair.Key == Key;
			});
		return Found ? &Found->Value : nullptr;
	}

	FUeAgentEditorSessionOperationPolicyDescriptor SetCurrentPolicy()
	{
		return FUeAgentEditorSessionOperationPolicyDescriptor(
			EUeAgentEditorSessionOperationFamily::WorldContext,
			TEXT("policy:world-context:set-current/v1"),
			TEXT("native_session_resource"),
			TEXT("uai.readback.editor.world_context/v1"),
			EUeAgentStatePlane::AuthoringMemory,
			{EUeAgentExecutionWorldType::Editor});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionSetCurrentPreRegistrationTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.E1."
		"SetCurrentPreRegistrationTargetResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionSetCurrentPreRegistrationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FRecordingBuilder, ESPMode::ThreadSafe> Builder =
		MakeShared<FRecordingBuilder, ESPMode::ThreadSafe>();
	const TSharedRef<FRecordingTargetResolver, ESPMode::ThreadSafe> Resolver =
		MakeShared<FRecordingTargetResolver, ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory> Factory;
	FExecutionMutationError Error;
	const TArray<FUeAgentEditorSessionOperationPolicyRegistration>
		Registrations = {{SetCurrentPolicy(), Builder}};
	if (!TestTrue(
			TEXT("production factory constructs with its real target resolver"),
			FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
				Registrations, Factory, Error))
		|| !Factory.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		return false;
	}

	double Now = 100.0;
	const TSharedRef<FNoopPort, ESPMode::ThreadSafe> DefaultPort =
		MakeShared<FNoopPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner Owner(DefaultPort, [&Now]() { return Now; });
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		InvalidResources = Resources();
	const FUeAgentEditorSessionOperationTypedRequest InvalidTarget(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:not-a-package"),
		TEXT("policy:world-context:set-current/v1"),
		EmptyParameters(*InvalidResources));
	const FLongRunningOperationRequest InvalidOperation = OperationRequest(
		TEXT("invalid-map"), InvalidResources, Now);
	FLongRunningOperationSnapshot Snapshot;
	TestFalse(
		TEXT("unresolvable set-current identity fails before acceptance"),
		TryAcceptEditorSessionOperation(
			InvalidTarget,
			InvalidOperation,
			*Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(
		TEXT("unresolvable map identity has a stable typed failure"),
		Error.GetCode(),
		FString(TEXT("world_context_subject_invalid")));
	TestFalse(
		TEXT("unresolvable identity leaves no registered operation"),
		Owner.GetSnapshot(
			InvalidOperation.GetOperationId(),
			InvalidOperation.GetOwnerLeaseId(),
			InvalidOperation.GetServerSessionId(),
			Snapshot,
			Error));

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		MissingResources = Resources();
	const FUeAgentEditorSessionOperationTypedRequest MissingTarget(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/UAI/DefinitelyMissing/Map_ExpectedRed"),
		TEXT("policy:world-context:set-current/v1"),
		EmptyParameters(*MissingResources));
	const FLongRunningOperationRequest MissingOperation = OperationRequest(
		TEXT("missing-map"), MissingResources, Now);
	TestFalse(
		TEXT("unresolved set-current target fails before acceptance"),
		TryAcceptEditorSessionOperation(
			MissingTarget,
			MissingOperation,
			*Factory,
			Owner,
			Snapshot,
			Error));
	TestEqual(
		TEXT("unresolved map has a stable typed failure"),
		Error.GetCode(),
		FString(TEXT("world_context_map_not_found")));
	TestEqual(
		TEXT("unresolved target never constructs an operation port"),
		Builder->Calls,
		0);
	TestFalse(
		TEXT("unresolved target leaves no registered operation"),
		Owner.GetSnapshot(
			MissingOperation.GetOperationId(),
			MissingOperation.GetOwnerLeaseId(),
			MissingOperation.GetServerSessionId(),
			Snapshot,
			Error));

	Resolver->bTargetExists = true;
	TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory>
		ResolvedFactory;
	if (!TestTrue(
			TEXT("resolved-target fixture factory constructs"),
			FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
				Registrations,
				Resolver,
				ResolvedFactory,
				Error))
		|| !ResolvedFactory.IsValid())
	{
		return false;
	}
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ExistingResources = Resources();
	const FUeAgentEditorSessionOperationTypedRequest ExistingTarget(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/UAI/Fixture/ExistingMap"),
		TEXT("policy:world-context:set-current/v1"),
		EmptyParameters(*ExistingResources));
	const FLongRunningOperationRequest ExistingOperation = OperationRequest(
		TEXT("existing-map"), ExistingResources, Now);
	TestTrue(
		TEXT("resolved set-current target keeps the existing accept mechanism"),
		TryAcceptEditorSessionOperation(
			ExistingTarget,
			ExistingOperation,
			*ResolvedFactory,
			Owner,
			Snapshot,
			Error));
	TestEqual(
		TEXT("resolved target constructs exactly one operation port"),
		Builder->Calls,
		1);
	TestEqual(
		TEXT("accepted still means registration only"),
		Snapshot.GetPhase(),
		ELongRunningOperationPhase::Accepted);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionReadySignalTest,
	"UeAgentInterface.DomainAdapters.EditorSessionOperation.E1."
		"ActiveNativeStateProjectsTypedReadySignal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionReadySignalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	double Now = 200.0;
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ResourceTracker = Resources();
	const FUeAgentEditorSessionOperationTypedRequest Request(
		EUeAgentEditorSessionOperationFamily::WorldContext,
		TEXT("world:/Game/UAI/Fixture/ExistingMap"),
		TEXT("policy:world-context:set-current/v1"),
		EmptyParameters(*ResourceTracker));
	TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
	FExecutionMutationError Error;
	if (!TestTrue(
			TEXT("native editor session port constructs"),
			TryCreateEditorSessionNativeResourcePort(
				Request,
				SetCurrentPolicy(),
				MakeShared<FActiveNativeSession, ESPMode::ThreadSafe>(),
				Port,
				Error))
		|| !Port.IsValid())
	{
		return false;
	}
	const FLongRunningOperationRequest Operation = OperationRequest(
		TEXT("ready-signal"), ResourceTracker, Now);
	FUeAgentCancellationSource Cancellation([&Now]() { return Now; });
	FLongRunningWorkIdentity Identity;
	if (!TestTrue(
			TEXT("native editor session starts"),
			Port->Start(
				Operation,
				Cancellation.GetToken(),
				Identity,
				Error)))
	{
		return false;
	}
	FLongRunningWorkObservation Observation;
	if (!TestTrue(
			TEXT("active native state is observed"),
			Port->Observe(
				Operation.GetOperationId(),
				Identity,
				*ResourceTracker,
				Observation,
				Error))
		|| !TestTrue(
			TEXT("active native state emits a typed signal"),
			Observation.GetSignal().IsSet()))
	{
		return false;
	}
	const FUeAgentCanonicalValueRef* Phase = FindObjectValue(
		Observation.GetSignal()->GetPayload(), TEXT("phase"));
	TestTrue(
		TEXT("typed latest signal uses the public ready boundary"),
		Phase
			&& (*Phase)->GetKind() == EUeAgentValueKind::String
			&& (*Phase)->GetText() == TEXT("ready"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorSessionSetCurrentMachineTutorialTest,
	"UeAgentInterface.Module.CommandRegistration.EditorSession.E1."
		"SetCurrentMachineTutorial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorSessionSetCurrentMachineTutorialTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("process command registry composes"),
			TryComposeProcessActiveCommandComposition(
				Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<const FCommandDiscoveryService> Discovery;
	FCommandDiscoveryError DiscoveryError;
	if (!TestTrue(
			TEXT("command detail service composes"),
			FCommandDiscoveryService::TryCreate(
				Registry.ToSharedRef(), Discovery, DiscoveryError))
		|| !Discovery.IsValid())
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	const FUeAgentFrozenCommandContract* Contract =
		Registry->FindExact(TEXT("editor.session"));
	if (!TestNotNull(TEXT("editor.session is registered"), Contract))
	{
		return false;
	}
	FCommandDetail Detail;
	if (!TestTrue(
			TEXT("set-current exact variant detail resolves"),
			Discovery->GetDetail(
				Contract->GetCommandId(),
				TEXT("policy:world-context:set-current/v1"),
				Detail,
				DiscoveryError)))
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	const FString Tutorial = Detail.Purpose.Summary + TEXT(" ")
		+ Detail.Purpose.Target + TEXT(" ")
		+ FString::Join(Detail.Purpose.Preconditions, TEXT(" ")) + TEXT(" ")
		+ Detail.Purpose.ParameterGuidance + TEXT(" ")
		+ Detail.Purpose.ResultGuidance + TEXT(" ")
		+ Detail.Purpose.FailureGuidance + TEXT(" ")
		+ Detail.Purpose.SideEffectGuidance;
	for (const FString& Required : {
		TEXT("existing target"),
		TEXT("does not create"),
		TEXT("accepted"),
		TEXT("operation_ref"),
		TEXT("latest generation"),
		TEXT("resume"),
		TEXT("latest_signal"),
		TEXT("entered/ready"),
		TEXT("cancel"),
		TEXT("terminal"),
		TEXT("release"),
	})
	{
		TestTrue(
			TEXT("machine tutorial states lifecycle term: ") + Required,
			Tutorial.Contains(Required, ESearchCase::IgnoreCase));
	}
	return !HasAnyErrors();
}
