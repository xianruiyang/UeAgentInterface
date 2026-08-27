#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
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
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;
		int32 MaxActiveOperations = 0;
		int32 MaxRetainedOperations = 0;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentLongRunningSessionRuntimeError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

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

	private:
		FUeAgentLongRunningSessionRuntime(
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<Contract::IUeAgentOperationApplicationPort>
				InOperationPort);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort;
	};
}
