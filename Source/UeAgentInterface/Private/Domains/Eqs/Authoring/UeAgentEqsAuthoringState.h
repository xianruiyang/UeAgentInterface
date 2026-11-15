#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UEnvQuery;

namespace UeAgentEqsAuthoring
{
	struct FGraphCapture;
	inline constexpr const TCHAR* GraphSemanticId = TEXT("uai.semantic.eqs.graph");
	inline constexpr const TCHAR* NodeSemanticId = TEXT("uai.semantic.eqs.node");
	inline constexpr const TCHAR* NodePropertySemanticId = TEXT("uai.semantic.eqs.node_property");
	inline constexpr const TCHAR* EdgeSemanticId = TEXT("uai.semantic.eqs.edge");

	bool RegisterGraphState(UE::UAI::Contract::FDiagnostic& OutError);
	bool AppendGraphExportRequests(UEnvQuery& Query,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	bool AppendGraphExportRequests(UEnvQuery& Query, const FGraphCapture& Capture,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
}
