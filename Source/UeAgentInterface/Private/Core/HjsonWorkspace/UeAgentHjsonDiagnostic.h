#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonValue;

namespace UE::UAI::HjsonWorkspace
{
	class FHjsonNode;

	// Scalar facts retain their exact value; composites report only kind and count.
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> HjsonDiagnosticValue(const FHjsonNode* Node);

	// The source owner explicitly chooses whether the actual value is public.
	UEAGENTINTERFACE_API Contract::FDiagnostic HjsonNodeFailure(const FString& Code,
		const FString& Reason, const FString& Stage, const FHjsonNode& Node,
		TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true);
	UEAGENTINTERFACE_API Contract::FDiagnostic HjsonFieldFailure(const FString& Code,
		const FString& Reason, const FString& Stage, const FHjsonNode& Object,
		const FString& Field, TSharedPtr<FJsonValue> Expected = nullptr, bool bPublicActual = true);
}
