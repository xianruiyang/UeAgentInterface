#pragma once

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::PieRuntimeDebug
{
	struct UEAGENTINTERFACE_API FUeAgentPieRuntimeDebugRoute final
	{
		FString CommandId;
		FString PythonNamespace;
		FString PythonFunction;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const FUeAgentPieRuntimeDebugRoute& GetRoute();

	UEAGENTINTERFACE_API bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError);
}
