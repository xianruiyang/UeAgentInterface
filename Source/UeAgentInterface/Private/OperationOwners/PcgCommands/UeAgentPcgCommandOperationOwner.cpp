#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::OperationOwners::PcgCommands
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
		struct FProcessMechanicsHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<FString> AuthorityScopes;
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
					OutError.Detail = TEXT("PCG command projection cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("pcg_command.projection"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = Expired.Stage;
					OutError.Detail = TEXT("PCG command projection deadline expired");
					return false;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("pcg_command.projection"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("PCG command projection budget exceeded");
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
			if (!DomainAdapters::PcgCommands::IsCommand(Handler->GetCommandId()))
			{
				return true;
			}
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutError = TEXT("pcg_command_mechanics_handler_duplicate:")
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
		if (!DomainAdapters::PcgCommands::TryGetGraphProfiles(
				GraphProfiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig GraphConfig;
		GraphConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundPcgAiProductionConfig PcgConfig;
		PcgConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/PcgCommands");
		PcgConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> GraphRuntime;
		FGraphCompoundRuntimeError GraphError;
		if (!TryCreatePcgAiGraphProductionRuntime(
				GraphConfig, PcgConfig, GraphRuntime, GraphError)
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
		OutAuthorityScopes.Add(TEXT("pcg_ai_node_graph.write"));

		if (!IsInGameThread())
		{
			OutHandlers.Reset();
			OutError = TEXT("pcg_command_game_thread_required");
			return false;
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			OutHandlers.Reset();
			OutError = TEXT("pcg_command_editor_world_unavailable");
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
			/ TEXT("UeAgentInterface/SimpleAuthoring/PcgCommandFoundation");
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
		if (OutHandlers.Num() != DomainAdapters::PcgCommands::GetCommandIds().Num())
		{
			OutHandlers.Reset();
			OutError = TEXT("pcg_command_mechanics_handler_count_mismatch");
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
			OutError = TEXT("pcg_command_mechanics_handler_missing:") + CommandId;
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
