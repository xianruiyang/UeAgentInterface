#include "OperationOwners/PieRuntimeDebug/UeAgentPieRuntimeDebugOwner.h"

#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::OperationOwners::PieRuntimeDebug
{
	using namespace Composition::LongRunningSession;

	bool FUeAgentPieRuntimeDebugRoute::IsValid() const
	{
		return CommandId == TEXT("runtime.scenario.execute")
			&& PythonNamespace == TEXT("runtime")
			&& PythonFunction == TEXT("scenario_execute")
			&& DescriptorIdentity.IsValid();
	}

	const FUeAgentPieRuntimeDebugRoute& GetRoute()
	{
		static const FUeAgentPieRuntimeDebugRoute Route = []()
		{
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: GetLongRunningSessionActivationProfiles())
			{
				if (Profile.CommandId == TEXT("runtime.scenario.execute"))
				{
					return FUeAgentPieRuntimeDebugRoute{
						Profile.CommandId,
						Profile.Namespace,
						Profile.Method,
						Profile.DescriptorIdentity,
					};
				}
			}
			return FUeAgentPieRuntimeDebugRoute{
				FString(),
				FString(),
				FString(),
				Contract::FUeAgentDescriptorIdentity(
					FString(), 0, FString()),
			};
		}();
		return Route;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(
				OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}
}
