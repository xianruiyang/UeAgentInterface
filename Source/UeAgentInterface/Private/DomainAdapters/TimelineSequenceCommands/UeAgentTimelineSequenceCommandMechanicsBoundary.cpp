#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Dom/JsonValue.h"
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

	const TArray<FString>& GetCommandIds()
	{
		return QueryOwners::TimelineSequenceCommands::GetCommandIds();
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = []
		{
			TArray<FString> Result = GetCommandIds();
			Result.Remove(TEXT("timeline.sequence.settings.update"));
			return Result;
		}();
		return Ids;
	}

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
					OutError = Cancelled;
					return false;
				}
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits, 1,
						TEXT("timeline_sequence.projection"), ChargeError))
				{
					OutError = ChargeError;
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutHandlers.Reset();
			OutScopes.Reset();
			OutError.Reset();
			if (!IsInGameThread())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_game_thread_required"), TEXT("Timeline sequence mechanics must be initialized on the game thread."), TEXT("runtime_factory"));
				return false;
			}
			UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_editor_world_unavailable"), TEXT("No editor world is available for timeline sequence mechanics."), TEXT("runtime_factory"));
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
				OutError = TicketError;
				return false;
			}
			TSharedPtr<const FUeAgentResponseProjector> Projector;
			FUeAgentProjectionError ProjectionError;
			if (!FUeAgentResponseProjector::TryCreate(
					MakeShared<FConservativeTokenEstimator>(), Projector, ProjectionError))
			{
				OutError = ProjectionError;
				return false;
			}

			FUeAgentSimpleAuthoringRuntimeConfig SimpleConfig;
			SimpleConfig.ProjectInstanceId = TEXT("project:") + FString(FApp::GetProjectName());
			SimpleConfig.EditorInstanceId = FString::Printf(
				TEXT("editor-process:%u"), FPlatformProcess::GetCurrentProcessId());
			SimpleConfig.ServerSessionId = SessionId;
			SimpleConfig.RegistryEpoch = 1;

			SimpleConfig.JournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SimpleAuthoring/TimelineSequenceFoundation");
			TSharedPtr<FUeAgentSimpleAuthoringRuntime> SimpleRuntime;
			FUeAgentSimpleAuthoringRuntimeError SimpleError;
			if (!FUeAgentSimpleAuthoringRuntime::TryCreate(
					SimpleConfig, *World, Tickets.ToSharedRef(), Projector.ToSharedRef(),
					SimpleRuntime, SimpleError)
				|| !SimpleRuntime.IsValid())
			{
				OutError = SimpleError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_sequence_simple_runtime_missing"), TEXT("The simple authoring runtime factory did not return a runtime."), TEXT("runtime_factory"));
				return false;
			}
			TSharedPtr<const FUeAgentDiscoveryService> Discovery;
			FUeAgentDiscoveryError DiscoveryError;
			if (!FUeAgentDiscoveryService::TryCreate(
					SimpleRuntime->GetCatalogSources(), {}, Discovery, DiscoveryError)
				|| !Discovery.IsValid())
			{
				OutError = DiscoveryError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_sequence_discovery_missing"), TEXT("The discovery factory did not return a service."), TEXT("runtime_factory"));
				return false;
			}

			FUeAgentTimelineSpatialRuntimeConfig RuntimeConfig;
			RuntimeConfig.ProjectInstanceId = SimpleConfig.ProjectInstanceId;
			RuntimeConfig.EditorInstanceId = SimpleConfig.EditorInstanceId;
			RuntimeConfig.ServerSessionId = SessionId;
			RuntimeConfig.RegistryEpoch = 1;

			TSharedPtr<FUeAgentTimelineSpatialRuntime> Runtime;
			FUeAgentTimelineSpatialRuntimeError RuntimeError;
			if (!FUeAgentTimelineSpatialRuntime::TryCreate(
					RuntimeConfig, *World, Tickets.ToSharedRef(), Discovery.ToSharedRef(),
					Projector.ToSharedRef(), Runtime, RuntimeError)
				|| !Runtime.IsValid())
			{
				OutError = RuntimeError;
				if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(
					TEXT("timeline_sequence_runtime_missing"), TEXT("The timeline spatial factory did not return a runtime."), TEXT("runtime_factory"));
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
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_mechanics_duplicate"), TEXT("More than one handler is registered for this sequence command."), TEXT("runtime_factory"));
					OutError.PropertyPath = TEXT("command_id");
					OutError.ActualValuePresent = true;
					OutError.ActualValue = MakeShared<FJsonValueString>(Handler->GetCommandId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("one handler per command"));
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			OutScopes = Runtime->GetAuthorityScopes();
			if (OutHandlers.Num() != QueryOwners::TimelineSequenceCommands::GetCommandIds().Num())
			{
				const int32 ActualCount = OutHandlers.Num();
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_mechanics_count_mismatch"), TEXT("The registered handler count differs from the declared sequence command count."), TEXT("runtime_factory"));
				OutError.PropertyPath = TEXT("handlers.count");
				OutError.ActualValuePresent = true;
				OutError.ActualValue = MakeShared<FJsonValueNumber>(ActualCount);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(QueryOwners::TimelineSequenceCommands::GetCommandIds().Num());
				return false;
			}
			return true;
		}
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_mechanics_missing"), TEXT("No timeline sequence handler is registered for this command."), TEXT("runtime_factory"));
			OutError.PropertyPath = TEXT("command_id");
			OutError.ActualValuePresent = true;
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
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
