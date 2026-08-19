#pragma once

#include "CoreMinimal.h"
#include "LifecycleHost/UeAgentLifecycleHost.h"

namespace UE::UAI::LifecycleHost
{
	enum class EUeAgentAutomationExecutorKind : uint8
	{
		HeadlessNullRhi,
		HeadlessRhi,
		Interactive,
	};

	struct UEAGENTINTERFACE_API FUeAgentAutomationHostContract
	{
		EUeAgentAutomationExecutorKind ExecutorKind =
			EUeAgentAutomationExecutorKind::HeadlessNullRhi;
		TArray<FString> LaunchArguments;
		bool bOwnedProcessOnly = true;
		bool bIsolatedUserDir = true;
		bool bIsolatedPorts = true;
		bool bPreserveMainEditor = true;
		bool bMayDisableBackgroundThrottling = false;
		bool bMayForceForeground = false;
		bool bSupported = false;
		FString FailureCode;
	};

	UEAGENTINTERFACE_API bool TryResolveAutomationHostContract(
		const FUeAgentAutomationRequirement& Requirement,
		bool bInteractiveRunnerAvailable,
		FUeAgentAutomationHostContract& OutContract);
}
