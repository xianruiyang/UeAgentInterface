#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"

namespace UE::UAI::OperationOwners::ViewportGraph
{
	using namespace BindingGateway::Ticket;
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;
	using namespace Composition::TimelineSpatial;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace Projection;

	namespace
	{
		FString ErrorJson(
			const FString& Code,
			const FString& Subject,
			const FString& Detail,
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Error->SetBoolField(TEXT("retry_safe"), bRetrySafe);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return Text;
		}

		struct FProcessHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<FString> AuthorityScopes;
		};

		FProcessHost& ProcessHost()
		{
			static FProcessHost Host;
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
					OutError.Detail = TEXT("ViewportGraph projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("viewport_graph.projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("ViewportGraph projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("viewport_graph.projection"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("ViewportGraph projection budget exceeded");
					OutError.ResourceDimension = BudgetError.Dimension;
					OutError.ResourceLimit = BudgetError.Limit;
					OutError.ResourceObserved = BudgetError.Observed;
					return false;
				}
				int64 Bytes = 512 + Artifacts.Num() * 512 + Diagnostics.Num() * 256;
				if (Payload.GetSnapshot().IsValid())
				{
					Bytes += Payload.GetSnapshot()->GetEntries().Num() * 2048;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Bytes + 2) / 3),
					Bytes);
				return true;
			}
		};

		bool TryCreateMechanics(
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
			TArray<FString>& OutScopes,
			FString& OutError)
		{
			OutHandlers.Reset();
			OutScopes.Reset();
			OutError.Reset();
			if (!IsInGameThread())
			{
				OutError = TEXT("viewport_graph_game_thread_required");
				return false;
			}
			UWorld* const World = GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
			if (!World)
			{
				OutError = TEXT("viewport_graph_editor_world_unavailable");
				return false;
			}

			const FString SessionId = TEXT("viewport-graph-session:")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			TSharedPtr<const FUeAgentOperationTicketService> Tickets;
			FUeAgentOperationTicketError TicketError;
			if (!FUeAgentOperationTicketService::TryCreate(
					FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
					300.0,
					[]() { return FPlatformTime::Seconds(); },
					[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
					Tickets,
					TicketError))
			{
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
				OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
			SimpleConfig.ProjectInstanceId = TEXT("project:")
				+ FString(FApp::GetProjectName());
			SimpleConfig.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			SimpleConfig.ServerSessionId = SessionId;
			SimpleConfig.RegistryEpoch = 1;
			SimpleConfig.TimeoutSeconds = 120.0;
			SimpleConfig.MaxFrameBytes = 16 * 1024 * 1024;
			SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring/ViewportGraphFoundation");
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
			FUeAgentSimpleAuthoringRuntimeError SimpleError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					SimpleConfig,
					*World,
					Tickets.ToSharedRef(),
					Projector.ToSharedRef(),
					SimpleRuntime,
					SimpleError)
				|| !SimpleRuntime.IsValid())
			{
				OutError = SimpleError.Code + TEXT(":") + SimpleError.Detail;
				return false;
			}
			TSharedPtr<const FUeAgentDiscoveryService> Discovery;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentDiscoveryService::TryCreate(
					SimpleRuntime->GetCatalogSources(),
					{},
					Discovery,
					DiscoveryError)
				|| !Discovery.IsValid())
			{
				OutError = DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail;
				return false;
			}

			FUeAgentTimelineSpatialRuntimeConfig RuntimeConfig;
			RuntimeConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
			RuntimeConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
			RuntimeConfig.ServerSessionId = SessionId;
			RuntimeConfig.RegistryEpoch = 1;
			RuntimeConfig.TimeoutSeconds = 120.0;
			RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
			TSharedPtr<FUeAgentTimelineSpatialRuntime> Runtime;
			FUeAgentTimelineSpatialRuntimeError RuntimeError;
			if (!FUeAgentTimelineSpatialRuntime::TryCreate(
					RuntimeConfig,
					*World,
					Tickets.ToSharedRef(),
					Discovery.ToSharedRef(),
					Projector.ToSharedRef(),
					Runtime,
					RuntimeError)
				|| !Runtime.IsValid())
			{
				OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
				return false;
			}
			for (const TSharedRef<IUeAgentCommandHandler>& Handler
				: Runtime->GetCommandHandlers())
			{
				if (!QueryOwners::ViewportGraph::IsCommand(Handler->GetCommandId()))
				{
					continue;
				}
				if (OutHandlers.Contains(Handler->GetCommandId()))
				{
					OutHandlers.Reset();
					OutError = TEXT("viewport_graph_mechanics_duplicate:")
						+ Handler->GetCommandId();
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			OutScopes = Runtime->GetAuthorityScopes();
			if (OutHandlers.Num() != QueryOwners::ViewportGraph::GetCommandIds().Num())
			{
				OutHandlers.Reset();
				OutError = TEXT("viewport_graph_mechanics_count_mismatch");
				return false;
			}
			return true;
		}
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& CommandId)
	{
		static const TMap<FString, TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory>> Factories = []()
		{
			TMap<FString, TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory>> Result;
			for (const FString& Id : QueryOwners::ViewportGraph::GetCommandIds())
			{
				if (!QueryOwners::ViewportGraph::IsQuery(Id))
				{
					Result.Add(
						Id,
						DomainCapability::DelegatedCommandOperation::
							CreateInvocationFactory(
								Id, &TryResolveMechanics, false, true));
				}
			}
			return Result;
		}();
		return Factories.FindRef(CommandId);
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
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (Host.Handlers.IsEmpty()
			&& !TryCreateMechanics(Host.Handlers, Host.AuthorityScopes, OutError))
		{
			return false;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found =
			Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = TEXT("viewport_graph_mechanics_missing:") + CommandId;
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		if (QueryOwners::ViewportGraph::IsQuery(Request.GetCommandId()))
		{
			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> MechanicsScopes;
			FString ResolveError;
			if (!TryResolveMechanics(
					Request.GetCommandId(),
					Mechanics,
					MechanicsScopes,
					ResolveError)
				|| !Mechanics.IsValid())
			{
				return FUeAgentCommandResult::Failure(
					Request,
					TEXT("{}"),
					ErrorJson(
						TEXT("viewport_graph_runtime_unavailable"),
						Request.GetCommandId(),
						ResolveError));
			}
			TArray<FString> Scopes = Caller.GetAuthorityScopes();
			for (const FString& Scope : MechanicsScopes)
			{
				Scopes.AddUnique(Scope);
			}
			return Mechanics->Submit(
				Request,
				Contract::FUeAgentCallerContext(
					Caller.GetCallerId(),
					MoveTemp(Scopes),
					Caller.GetCorrelationId()),
				Cancellation);
		}

		const TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
			FindOperationInvocationFactory(Request.GetCommandId());
		if (!Factory.IsValid())
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("viewport_graph_operation_factory_missing"),
					Request.GetCommandId(),
					TEXT("the exact transient operation has no factory")));
		}
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("viewport_graph_runtime_unavailable"),
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
				Request,
				TEXT("{}"),
				ErrorJson(
					Receipt.GetErrorCode(),
					Request.GetCommandId(),
					Detail,
					BindingGateway::Operation::LexToString(Receipt.GetStatus()),
					Receipt.IsRetrySafe()));
		}
		FString Payload;
		FString PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("viewport_graph_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
