#pragma once

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::EditorSessionBuild
{
	struct UEAGENTINTERFACE_API FUeAgentEditorSessionBuildRoute final
	{
		FString CommandId;
		FString PythonFunction;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentEditorSessionBuildRoute>&
	GetRoutes();

	UEAGENTINTERFACE_API bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError);
}
