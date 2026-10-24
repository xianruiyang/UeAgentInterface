#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UBlackboardData;

namespace UeAgentBlackboardAuthoring
{
	inline constexpr const TCHAR* DocumentSemanticId = TEXT("uai.semantic.blackboard.document");
	inline constexpr const TCHAR* KeySemanticId = TEXT("uai.semantic.blackboard.key");
	inline constexpr const TCHAR* KeyPropertySemanticId = TEXT("uai.semantic.blackboard.key_property");

	UEAGENTINTERFACE_API bool RegisterBlackboardState(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool AppendStructuralReadRequests(
		const UBlackboardData& Blackboard,
		const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
}
