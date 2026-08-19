#include "LifecycleHost/UeAgentAutomationHostContract.h"

namespace UE::UAI::LifecycleHost
{
	bool TryResolveAutomationHostContract(
		const FUeAgentAutomationRequirement& Requirement,
		const bool bInteractiveRunnerAvailable,
		FUeAgentAutomationHostContract& OutContract)
	{
		OutContract = FUeAgentAutomationHostContract{};
		if (Requirement.MinimumFps < 0.0)
		{
			OutContract.FailureCode =
				TEXT("automation_minimum_fps_invalid");
			return false;
		}

		const bool bInteractive =
			Requirement.bRequiresSlate
			|| Requirement.bRequiresForeground;
		if (bInteractive)
		{
			OutContract.ExecutorKind =
				EUeAgentAutomationExecutorKind::Interactive;
			if (!bInteractiveRunnerAvailable)
			{
				OutContract.FailureCode =
					TEXT("interactive_requires_owned_interactive_runner");
				return false;
			}
			OutContract.bSupported = true;
			return true;
		}

		OutContract.LaunchArguments.Add(TEXT("-unattended"));
		if (Requirement.bRequiresRhi)
		{
			OutContract.ExecutorKind =
				EUeAgentAutomationExecutorKind::HeadlessRhi;
		}
		else
		{
			OutContract.ExecutorKind =
				EUeAgentAutomationExecutorKind::HeadlessNullRhi;
			OutContract.LaunchArguments.Add(TEXT("-NullRHI"));
		}
		OutContract.bSupported = true;
		return true;
	}
}
