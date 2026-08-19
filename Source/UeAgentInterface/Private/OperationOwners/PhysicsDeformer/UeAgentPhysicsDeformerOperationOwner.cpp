#include "OperationOwners/PhysicsDeformer/UeAgentPhysicsDeformerOperationOwner.h"

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
		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail,
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), SubjectId);
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Object->SetBoolField(TEXT("retry_safe"), bRetrySafe);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
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
					OutError.Code = Cancelled.Code;
					OutError.SubjectId = Cancelled.Reason;
					OutError.Detail = TEXT("PhysicsDeformer projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("physics_deformer.projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("PhysicsDeformer projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("physics_deformer.projection"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("PhysicsDeformer projection budget exceeded");
					OutError.ResourceDimension = BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved = BudgetError.Observed;
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
			FString& OutError)
		{
			if (!DomainAdapters::PhysicsDeformer::IsCommand(
					Handler->GetCommandId()))
			{
				return true;
			}
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutError = TEXT("physics_deformer_mechanics_handler_duplicate:")
					+ Handler->GetCommandId();
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
			return true;
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
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
		GraphConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundAnimationProductionConfig AnimationConfig;
		AnimationConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/PhysicsDeformer");
		AnimationConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> GraphRuntime;
		FGraphCompoundRuntimeError GraphError;
		if (!TryCreateAnimationGraphProductionRuntime(
				GraphConfig, AnimationConfig, GraphRuntime, GraphError)
			|| !GraphRuntime.IsValid())
		{
			OutError = GraphError.Code + TEXT(":") + GraphError.Detail;
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
			OutError = TEXT("physics_deformer_game_thread_required");
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			OutHandlers.Reset();
			OutError = TEXT("physics_deformer_editor_world_unavailable");
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
			OutError = TicketError.Code + TEXT(":") + TicketError.Detail;
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
			OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
			return false;
		}

		FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
		SimpleConfig.ProjectInstanceId = TEXT("project:")
			+ FString(FApp::GetProjectName());
		SimpleConfig.EditorInstanceId = FString::Printf(
			TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
		SimpleConfig.ServerSessionId = ServerSessionId;
		SimpleConfig.RegistryEpoch = 1;
		SimpleConfig.TimeoutSeconds = 120.0;
		SimpleConfig.MaxFrameBytes = 16 * 1024 * 1024;
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
			OutError = SimpleError.Code + TEXT(":") + SimpleError.Detail;
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
			OutError = DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail;
			return false;
		}

		FUeAgentTimelineSpatialRuntimeConfig TimelineConfig;
		TimelineConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
		TimelineConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
		TimelineConfig.ServerSessionId = ServerSessionId;
		TimelineConfig.RegistryEpoch = 1;
		TimelineConfig.TimeoutSeconds = 120.0;
		TimelineConfig.MaxFrameBytes = 16 * 1024 * 1024;
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
			OutError = PhysicsSessionError.Code + TEXT(":")
				+ PhysicsSessionError.Detail;
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
			OutError = TimelineError.Code + TEXT(":") + TimelineError.Detail;
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
			OutHandlers.Reset();
			OutError = TEXT("physics_deformer_mechanics_handler_count_mismatch");
			return false;
		}
		return true;
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
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
			OutError = TEXT("physics_deformer_mechanics_handler_missing:")
				+ CommandId;
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
				ErrorJson(
					TEXT("physics_deformer_runtime_unavailable"),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
			*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			FString Detail = Invocation.GetContext()->GetFailureDetail();
			if (Detail.IsEmpty())
			{
				Detail = TEXT("phase_history:")
					+ FString::Join(Receipt.GetPhaseHistory(), TEXT(","));
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"),
				ErrorJson(
					Receipt.GetErrorCode(), Request.GetCommandId(), Detail,
					BindingGateway::Operation::LexToString(Receipt.GetStatus()),
					Receipt.IsRetrySafe()));
		}
		FString Payload;
		FString PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
			Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(),
				TEXT("domain_operation_result_invalid"), PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
