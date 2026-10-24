#include "OperationOwners/PhysicsDeformer/UeAgentPhysicsDeformerOperationOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/PhysicsDeformer/UeAgentPhysicsDeformerMechanicsBoundary.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsSession.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::PhysicsDeformer
{
	using namespace BindingGateway::Ticket;
	using namespace CommandEdge;
	using namespace Composition::GraphCompound;
	using namespace Composition::SimpleAuthoring;
	using namespace Composition::TimelineSpatial;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace Projection;

	namespace
	{
		UE::UAI::Contract::FDiagnostic CommandFailure(FString Code, const FString& Subject, FString Reason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("command"), MoveTemp(InternalContext), Subject);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		Contract::FDiagnostic UnavailableFailure(FString Code, FString Reason, const TCHAR* ReadinessField, const FString& Subject = FString())
		{
			auto Diagnostic = CommandFailure(MoveTemp(Code), Subject, MoveTemp(Reason));
			Diagnostic.Stage = TEXT("runtime_prepare");
			Diagnostic.PropertyPath = ReadinessField;
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = Cause.IsEmpty()
				? UnavailableFailure(Code, TEXT("The operation runtime could not be prepared."), TEXT("runtime_ready"), Subject)
				: Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("command"), TEXT("The operation runtime could not be prepared."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = Cause.IsEmpty()
				? UnavailableFailure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_payload_ready"))
				: Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("result_projection"), TEXT("The operation completed, but its result payload could not be produced."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.Stage = TEXT("result_projection");
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
		}

		struct FProcessMechanicsHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<FString> AuthorityScopes;
			TMap<FString, TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> OperationFactories;
		};

		FProcessMechanicsHost& ProcessMechanicsHost()
		{
			static FProcessMechanicsHost Host;
			return Host;
		}

		class FConservativeTokenEstimator final : public IUeAgentTokenEstimator
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
				OutError = {};
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					OutError = Cancelled;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("physics_deformer.projection"),
						ChargeError))
				{
					OutError = ChargeError;
					return false;
				}
				int64 Utf8Bytes = 512 + Artifacts.Num() * 512
					+ Diagnostics.Num() * 256;
				if (Payload.GetSnapshot().IsValid())
				{
					Utf8Bytes += Payload.GetSnapshot()->GetEntries().Num() * 2048;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Utf8Bytes + 2) / 3),
					Utf8Bytes);
				return true;
			}
		};

		bool AddMechanics(
			const TSharedRef<IUeAgentCommandHandler>& Handler,
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!DomainAdapters::PhysicsDeformer::IsCommand(
					Handler->GetCommandId()))
			{
				return true;
			}
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutError = CommandFailure(TEXT("physics_deformer_mechanics_handler_duplicate"), Handler->GetCommandId(),
					TEXT("More than one mechanics handler is registered for the same physics or deformer command."));
				OutError.PropertyPath = TEXT("handler_registration_count");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
			return true;
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();

		TArray<FUeAgentGraphCompoundActivationProfile> GraphProfiles;
		if (!DomainAdapters::PhysicsDeformer::TryGetGraphProfiles(
				GraphProfiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig GraphConfig;

		FGraphCompoundAnimationProductionConfig AnimationConfig;
		AnimationConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/PhysicsDeformer");
		TSharedPtr<FGraphCompoundRuntime> GraphRuntime;
		FGraphCompoundRuntimeError GraphError;
		if (!TryCreateAnimationGraphProductionRuntime(
				GraphConfig, AnimationConfig, GraphRuntime, GraphError)
			|| !GraphRuntime.IsValid())
		{
			OutError = GraphError.IsEmpty()
				? UnavailableFailure(TEXT("physics_deformer_graph_runtime_unavailable"),
					TEXT("The animation graph runtime is unavailable and did not provide a diagnostic."), TEXT("graph_runtime_ready"))
				: MoveTemp(GraphError);
			return false;
		}
		TArray<TSharedRef<IUeAgentCommandHandler>> GraphHandlers;
		if (!TryCreateGraphCompoundCommandHandlers(
				GraphProfiles,
				GraphRuntime->GetQueryPort(),
				GraphRuntime->GetOperationPort(),
				GraphHandlers,
				OutError))
		{
			return false;
		}
		for (const TSharedRef<IUeAgentCommandHandler>& Handler : GraphHandlers)
		{
			if (!AddMechanics(Handler, OutHandlers, OutError))
			{
				OutHandlers.Reset();
				return false;
			}
		}
		OutAuthorityScopes.Add(TEXT("animation_rig_graph.read"));
		OutAuthorityScopes.Add(TEXT("animation_rig_graph.write"));

		if (!IsInGameThread())
		{
			OutHandlers.Reset();
			OutError = UnavailableFailure(TEXT("physics_deformer_game_thread_required"),
				TEXT("The physics and deformer mechanics must be prepared on the game thread."), TEXT("game_thread"));
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			OutHandlers.Reset();
			OutError = UnavailableFailure(TEXT("physics_deformer_editor_world_unavailable"),
				TEXT("The physics and deformer mechanics require an available Editor world."), TEXT("editor_world_available"));
			return false;
		}
		const FString ServerSessionId = TEXT("server-session:")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		if (!FUeAgentOperationTicketService::TryCreate(
				FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
				300.0,
				[]() { return FPlatformTime::Seconds(); },
				[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
				TicketService,
				TicketError))
		{
			OutHandlers.Reset();
			OutError = TicketError;
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				MakeShared<FConservativeTokenEstimator>(),
				Projector,
				ProjectionError))
		{
			OutHandlers.Reset();
			OutError = ProjectionError;
			return false;
		}

		FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
		SimpleConfig.ProjectInstanceId = TEXT("project:")
			+ FString(FApp::GetProjectName());
		SimpleConfig.EditorInstanceId = FString::Printf(
			TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
		SimpleConfig.ServerSessionId = ServerSessionId;
		SimpleConfig.RegistryEpoch = 1;

		SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/SimpleAuthoring/PhysicsDeformerFoundation");
		TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
		FUeAgentSimpleAuthoringRuntimeError SimpleError;
		if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
				SimpleConfig,
				*World,
				TicketService.ToSharedRef(),
				Projector.ToSharedRef(),
				SimpleRuntime,
				SimpleError)
			|| !SimpleRuntime.IsValid())
		{
			OutHandlers.Reset();
			OutError = SimpleError.IsEmpty()
				? UnavailableFailure(TEXT("physics_deformer_authoring_runtime_unavailable"),
					TEXT("The authoring runtime is unavailable and did not provide a diagnostic."), TEXT("authoring_runtime_ready"))
				: MoveTemp(SimpleError);
			return false;
		}
		TSharedPtr<const FUeAgentDiscoveryService> DiscoveryService;
		FUeAgentDiscoveryError DiscoveryError;
		if (!FUeAgentDiscoveryService::TryCreate(
				SimpleRuntime->GetCatalogSources(),
				{},
				DiscoveryService,
				DiscoveryError)
			|| !DiscoveryService.IsValid())
		{
			OutHandlers.Reset();
			OutError = DiscoveryError.IsEmpty()
				? UnavailableFailure(TEXT("physics_deformer_discovery_service_unavailable"),
					TEXT("The discovery service is unavailable and did not provide a diagnostic."), TEXT("discovery_service_ready"))
				: MoveTemp(DiscoveryError);
			return false;
		}

		FUeAgentTimelineSpatialRuntimeConfig TimelineConfig;
		TimelineConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
		TimelineConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
		TimelineConfig.ServerSessionId = ServerSessionId;
		TimelineConfig.RegistryEpoch = 1;

		TimelineConfig.RigClockBindings.Add(
			DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialClockBinding(
					TEXT("ue.physics.seconds"),
					TEXT("ue.physics.seconds"),
					TEXT("uai.physics.seconds.native.v1")));
		TimelineConfig.RigFrameBindings.Add(
			DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialFrameBinding(
					StateSemantic::FUeAgentSpatialFrame(
						TEXT("ue.physics.world"),
						TEXT("ue.world"),
						3,
						{{TEXT("1"), TEXT("0"), TEXT("0")},
						 {TEXT("0"), TEXT("1"), TEXT("0")},
						 {TEXT("0"), TEXT("0"), TEXT("1")}},
						{TEXT("0"), TEXT("0"), TEXT("0")},
						TEXT("cm"),
						TEXT("uai.physics.world.identity.v1"),
						StateSemantic::FUeAgentTemporalPoint(
							TEXT("ue.physics.seconds"), TEXT("0"),
							TEXT("60"), TEXT("1"), TEXT("simulation"))),
					true));
		TSharedPtr<DomainAdapters::RigSimulationSpatial::
			IUeAgentRigSimulationSpatialPhysicsSessionPort> PhysicsSessionPort;
		DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialExecutionError PhysicsSessionError;
		if (!DomainAdapters::RigSimulationSpatial::
				TryCreateProductionPhysicsSessionPort(
					PhysicsSessionPort, PhysicsSessionError)
			|| !PhysicsSessionPort.IsValid())
		{
			OutHandlers.Reset();
			OutError = PhysicsSessionError.IsEmpty()
				? UnavailableFailure(TEXT("physics_deformer_physics_session_unavailable"),
					TEXT("The physics session port is unavailable and did not provide a diagnostic."), TEXT("physics_session_ready"))
				: MoveTemp(PhysicsSessionError);
			return false;
		}
		TimelineConfig.RigPhysicsSessionPort = MoveTemp(PhysicsSessionPort);
		TSharedPtr<FUeAgentTimelineSpatialRuntime> TimelineRuntime;
		FUeAgentTimelineSpatialRuntimeError TimelineError;
		if (!FUeAgentTimelineSpatialRuntime::TryCreate(
				TimelineConfig,
				*World,
				TicketService.ToSharedRef(),
				DiscoveryService.ToSharedRef(),
				Projector.ToSharedRef(),
				TimelineRuntime,
				TimelineError)
			|| !TimelineRuntime.IsValid())
		{
			OutHandlers.Reset();
			OutError = TimelineError.IsEmpty()
				? UnavailableFailure(TEXT("physics_deformer_timeline_runtime_unavailable"),
					TEXT("The timeline runtime is unavailable and did not provide a diagnostic."), TEXT("timeline_runtime_ready"))
				: MoveTemp(TimelineError);
			return false;
		}
		for (const TSharedRef<IUeAgentCommandHandler>& Handler
			: TimelineRuntime->GetCommandHandlers())
		{
			if (!AddMechanics(Handler, OutHandlers, OutError))
			{
				OutHandlers.Reset();
				return false;
			}
		}
		for (const FString& Scope : TimelineRuntime->GetAuthorityScopes())
		{
			OutAuthorityScopes.AddUnique(Scope);
		}
		if (OutHandlers.Num()
			!= DomainAdapters::PhysicsDeformer::GetCommandIds().Num())
		{
			const int32 ActualCount = OutHandlers.Num();
			OutHandlers.Reset();
			OutError = CommandFailure(TEXT("physics_deformer_mechanics_handler_count_mismatch"), FString(),
				TEXT("The mechanics handlers do not cover the declared physics and deformer commands."));
			OutError.PropertyPath = TEXT("handler_count");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(ActualCount);
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(DomainAdapters::PhysicsDeformer::GetCommandIds().Num());
			return false;
		}
		return true;
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		FProcessMechanicsHost& Host = ProcessMechanicsHost();
		FScopeLock Lock(&Host.Mutex);
		if (Host.Handlers.IsEmpty()
			&& !TryCreateMechanicsHandlers(
				Host.Handlers, Host.AuthorityScopes, OutError))
		{
			return false;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found =
			Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = UnavailableFailure(TEXT("physics_deformer_mechanics_handler_missing"),
				TEXT("No mechanics handler is registered for the requested physics or deformer command."),
				TEXT("command_handler_registered"), CommandId);
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		FProcessMechanicsHost& Host = ProcessMechanicsHost();
		FScopeLock Lock(&Host.Mutex);
		if (const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>* Existing =
				Host.OperationFactories.Find(OperationId))
		{
			return *Existing;
		}
		const bool bPersists = OperationId != TEXT("physics.simulation.probe");
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				DomainCapability::DelegatedCommandOperation::
					CreateInvocationFactory(
						OperationId, &TryResolveMechanics, bPersists, true);
		Host.OperationFactories.Add(OperationId, Factory);
		return Factory;
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				FindOperationInvocationFactory(Request.GetCommandId());
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
			Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"),
				InvocationFailure(TEXT("physics_deformer_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
			*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			auto Diagnostic = Receipt.GetDiagnostic();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}
		FString Payload;
		UE::UAI::Contract::FDiagnostic PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
			Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("domain_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
