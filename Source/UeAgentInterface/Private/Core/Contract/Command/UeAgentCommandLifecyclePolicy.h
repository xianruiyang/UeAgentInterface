#pragma once

#include "Core/Contract/Command/UeAgentCommandContractModel.h"

namespace UE::UAI::CommandLifecycle
{
	struct UEAGENTINTERFACE_API FCommandLifecyclePolicy final
	{
		FString CommandId;
		CommandContract::EUeAgentCommandAvailability Availability =
			CommandContract::EUeAgentCommandAvailability::Enabled;
		TArray<FString> SuccessorCommandIds;
		FString Reason;
		bool bRequiresUserConfirmation = false;
	};

	UEAGENTINTERFACE_API const TArray<FCommandLifecyclePolicy>& GetPolicies();

	UEAGENTINTERFACE_API const FCommandLifecyclePolicy* FindExact(
		const FString& CommandId);

	UEAGENTINTERFACE_API bool TryApply(
		TArray<CommandContract::FUeAgentCommandDeclaration>& Declarations,
		FString& OutError);

	UEAGENTINTERFACE_API const TCHAR* LexToString(
		CommandContract::EUeAgentCommandAvailability Availability);

	UEAGENTINTERFACE_API bool IsExecutable(
		CommandContract::EUeAgentCommandAvailability Availability);
}
