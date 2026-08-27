#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::Contract
{
	class FUeAgentContractSchema;

	/** Builds one validated minimal instance for discovery examples. */
	UEAGENTINTERFACE_API bool TryBuildMinimalContractExample(
		const FUeAgentContractSchema& Schema,
		FUeAgentContractValue& OutCanonicalInstance,
		FString& OutPythonLiteral,
		FString& OutError);
}
