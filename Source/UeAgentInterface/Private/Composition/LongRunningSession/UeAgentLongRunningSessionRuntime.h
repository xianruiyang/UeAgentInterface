#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobAdapter.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationAdapter.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioAdapter.h"
#include "Templates/SharedPointer.h"

#define UAI_LONG_RUNNING_SESSION_RUNTIME_V1 1

namespace UE::UAI::Composition::LongRunningSession
{
	struct UEAGENTINTERFACE_API FUeAgentLongRunningSessionRuntimeConfig final
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		FString OwnerLeaseSecret;
		FString EditorWorldIdentity;
		FString EditorLevelIdentity;
		// Production resolves the current Editor context for each new request.
		// Fixed identities remain useful for hosts without a live Editor.
		TFunction<void(FString&, FString&)> ResolveEditorIdentities;

		bool IsValid() const;
	};

	using FUeAgentLongRunningSessionRuntimeError = Contract::FDiagnostic;

	/**
	 * Production C3O runtime for LongRunningSession.
	 *
	 * The runtime owns exactly one C7 owner. Domain factories create only
	 * operation-scoped C11 ports and never own a second job/session registry.
	 * The outer production composition installs this port behind thirteen exact
	 * command handlers.
	 */
	class UEAGENTINTERFACE_API FUeAgentLongRunningSessionRuntime final
	{
	public:
		class FState;

		static bool TryCreate(
			const FUeAgentLongRunningSessionRuntimeConfig& Config,
			TSharedRef<DomainAdapters::CompileBuildJob::
				IUeAgentCompileBuildJobPortFactory,
				ESPMode::ThreadSafe> CompileBuildFactory,
			TSharedRef<DomainAdapters::EditorSessionOperation::
				IUeAgentEditorSessionOperationPortFactory,
				ESPMode::ThreadSafe> EditorSessionFactory,
			TSharedRef<DomainAdapters::CaptureMediaLongJob::
				IUeAgentCaptureMediaLongJobPortFactory,
				ESPMode::ThreadSafe> CaptureMediaFactory,
			TSharedRef<DomainAdapters::PieRuntimeDebug::
				IUeAgentPieRuntimeScenarioPortFactory,
				ESPMode::ThreadSafe> PieRuntimeScenarioFactory,
			TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
			FUeAgentLongRunningSessionRuntimeError& OutError);

		const TSharedRef<Contract::IUeAgentOperationApplicationPort>&
		GetOperationPort() const;
		/** Schedule one shared, non-GameThread observation/cleanup pass. */
		void Pump();
		void RequestShutdown();
		bool IsPolling() const;
		int32 GetPendingCount() const;

		/** Keep a deferred command's exact reply on the existing operation owner. */
		CommandEdge::FUeAgentCommandResult AcceptDeferredCommand(
			const CommandEdge::FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			TFunction<void(TFunction<bool()>, TFunction<void(CommandEdge::FUeAgentCommandResult)>)> Work);

	private:
		FUeAgentLongRunningSessionRuntime(
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<Contract::IUeAgentOperationApplicationPort>
				InOperationPort);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort;
	};
}
