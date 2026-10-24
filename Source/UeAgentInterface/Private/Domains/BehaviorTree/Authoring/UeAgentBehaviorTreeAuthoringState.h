#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UBehaviorTree;

namespace UeAgentBehaviorTreeAuthoring
{
	inline constexpr const TCHAR* GraphSemanticId = TEXT("uai.semantic.behavior_tree.graph");
	inline constexpr const TCHAR* NodeSemanticId = TEXT("uai.semantic.behavior_tree.node");
	inline constexpr const TCHAR* NodePropertySemanticId = TEXT("uai.semantic.behavior_tree.node_property");
	inline constexpr const TCHAR* EdgeSemanticId = TEXT("uai.semantic.behavior_tree.edge");

	bool RegisterGraphState(UE::UAI::Contract::FDiagnostic& OutError);
	bool AppendGraphExportRequests(UBehaviorTree& Tree,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
}
