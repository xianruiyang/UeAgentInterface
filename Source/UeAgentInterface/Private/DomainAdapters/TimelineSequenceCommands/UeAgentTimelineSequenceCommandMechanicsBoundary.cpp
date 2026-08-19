#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "QueryOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::TimelineSequenceCommands
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
					OutError.Detail = TEXT("TimelineSequence projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("timeline_sequence.projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("TimelineSequence projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits, 1,
						TEXT("timeline_sequence.projection"), BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("TimelineSequence projection budget exceeded");
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
					FMath::Max<int64>(1, (Bytes + 2) / 3), Bytes);
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
				OutError = TEXT("timeline_sequence_game_thread_required");
				return false;
			}
			UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = TEXT("timeline_sequence_editor_world_unavailable");
				return false;
			}

			const FString SessionId = TEXT("timeline-sequence-session:")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			TSharedPtr<const FUeAgentOperationTicketService> Tickets;
			FUeAgentOperationTicketError TicketError;
			if (!FUeAgentOperationTicketService::TryCreate(
					FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
					300.0,
					[]() { return FPlatformTime::Seconds(); },
					[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
					Tickets, TicketError))
			{
				OutError = TicketError.Code + TEXT(":") + TicketError.Detail;
				return false;
			}
			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(), Projector, ProjectionError))
			{
				OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
			SimpleConfig.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			SimpleConfig.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			SimpleConfig.ServerSessionId = SessionId;
			SimpleConfig.RegistryEpoch = 1;
			SimpleConfig.TimeoutSeconds = 120.0;
			SimpleConfig.MaxFrameBytes = 16 * 1024 * 1024;
			SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring/TimelineSequenceFoundation");
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
			FUeAgentSimpleAuthoringRuntimeError SimpleError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					SimpleConfig, *World, Tickets.ToSharedRef(), Projector.ToSharedRef(),
					SimpleRuntime, SimpleError)
				|| !SimpleRuntime.IsValid())
			{
				OutError = SimpleError.Code + TEXT(":") + SimpleError.Detail;
				return false;
			}
			TSharedPtr<const FUeAgentDiscoveryService> Discovery;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentDiscoveryService::TryCreate(
					SimpleRuntime->GetCatalogSources(), {}, Discovery, DiscoveryError)
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
					RuntimeConfig, *World, Tickets.ToSharedRef(), Discovery.ToSharedRef(),
					Projector.ToSharedRef(), Runtime, RuntimeError)
				|| !Runtime.IsValid())
			{
				OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
				return false;
			}
			for (const TSharedRef<IUeAgentCommandHandler>& Handler : Runtime->GetCommandHandlers())
			{
				if (!QueryOwners::TimelineSequenceCommands::IsCommand(Handler->GetCommandId()))
				{
					continue;
				}
				if (OutHandlers.Contains(Handler->GetCommandId()))
				{
					OutHandlers.Reset();
					OutError = TEXT("timeline_sequence_mechanics_duplicate:") + Handler->GetCommandId();
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			OutScopes = Runtime->GetAuthorityScopes();
			if (OutHandlers.Num() != QueryOwners::TimelineSequenceCommands::GetCommandIds().Num())
			{
				OutHandlers.Reset();
				OutError = TEXT("timeline_sequence_mechanics_count_mismatch");
				return false;
			}
			return true;
		}
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
		const TSharedRef<IUeAgentCommandHandler>* Found = Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = TEXT("timeline_sequence_mechanics_missing:") + CommandId;
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}

	FUeAgentCommandResult Submit(
		const TSharedRef<IUeAgentCommandHandler>& Mechanics,
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}
