// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	enum class EUeAgentPieScenarioStepResult : uint8
	{
		Succeeded,
		Failed,
		TimedOut,
		Cancelled,
	};

	struct FUeAgentPieScenarioRequest
	{
		FString SessionIdentity;
		int64 ExpectedPauseEpoch = 0;
		int64 ExpectedWorkspaceRevision = 0;
		double WallTimeLimitSeconds = 0.0;
		double GameTimeLimitSeconds = 0.0;
		int64 FrameLimit = 0;

		bool IsValid() const;
	};

	struct FUeAgentPieScenarioReceipt
	{
		FString TerminalStatus;
		FString ErrorCode;
		FString SessionIdentity;
		int64 PauseEpoch = 0;
		int64 WorkspaceRevision = 0;
		bool bStateKnown = true;
	};

	class IUeAgentPieScenarioPorts
	{
	public:
		virtual ~IUeAgentPieScenarioPorts() = default;

		virtual bool IsCurrentPausedSnapshot(
			const FString& SessionIdentity,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision) const = 0;
		virtual bool Resume() = 0;
		virtual EUeAgentPieScenarioStepResult ExecuteSteps(
			double WallTimeLimitSeconds,
			double GameTimeLimitSeconds,
			int64 FrameLimit) = 0;
		virtual bool StopDispatch() = 0;
		virtual bool ReleaseAllForOperation() = 0;
		virtual bool RepauseAndVerify(
			const FString& SessionIdentity,
			int64& OutPauseEpoch) = 0;
		virtual bool RefreshCurrentWorkspace(
			const FString& SessionIdentity,
			int64 PreviousWorkspaceRevision,
			int64& OutWorkspaceRevision) = 0;
	};

	UEAGENTINTERFACE_API FUeAgentPieScenarioReceipt FinalizePieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		EUeAgentPieScenarioStepResult StepResult,
		IUeAgentPieScenarioPorts& Ports);

	UEAGENTINTERFACE_API FUeAgentPieScenarioReceipt RunPieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		IUeAgentPieScenarioPorts& Ports);
}
