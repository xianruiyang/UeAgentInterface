// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

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

		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError,
			const TMap<FString, UE::UAI::Contract::FSourceLocation>& InputSources = {}) const;
		bool IsValid() const;
	};

	struct FUeAgentPieScenarioReceipt
	{
		FString TerminalStatus;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString SessionIdentity;
		int64 PauseEpoch = 0;
		int64 WorkspaceRevision = 0;
		bool bStateKnown = true;
		bool bPaused = false;
		bool bWorkspaceRefreshed = false;
		TArray<UE::UAI::Contract::FDiagnostic> CleanupDiagnostics;
	};

	class IUeAgentPieScenarioPorts
	{
	public:
		virtual ~IUeAgentPieScenarioPorts() = default;

		virtual bool IsCurrentPausedSnapshot(
			const FString& SessionIdentity,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision, UE::UAI::Contract::FDiagnostic& OutError) const = 0;
		virtual bool Resume(UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual EUeAgentPieScenarioStepResult ExecuteSteps(
			double WallTimeLimitSeconds,
			double GameTimeLimitSeconds,
			int64 FrameLimit, UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool StopDispatch(UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool ReleaseAllForOperation(const FUeAgentPieScenarioReceipt& Receipt, UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool CapturePausedObservations(const FUeAgentPieScenarioReceipt& Receipt, UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool RepauseAndVerify(
			const FString& SessionIdentity,
			int64& OutPauseEpoch, UE::UAI::Contract::FDiagnostic& OutError) = 0;
		virtual bool RefreshCurrentWorkspace(
			const FString& SessionIdentity,
			int64 PreviousWorkspaceRevision,
			int64& OutWorkspaceRevision, UE::UAI::Contract::FDiagnostic& OutError) = 0;
	};

	UEAGENTINTERFACE_API FUeAgentPieScenarioReceipt FinalizePieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		EUeAgentPieScenarioStepResult StepResult,
		IUeAgentPieScenarioPorts& Ports,
        const UE::UAI::Contract::FDiagnostic& StepDiagnostic);

	UEAGENTINTERFACE_API FUeAgentPieScenarioReceipt RunPieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		IUeAgentPieScenarioPorts& Ports);
}
