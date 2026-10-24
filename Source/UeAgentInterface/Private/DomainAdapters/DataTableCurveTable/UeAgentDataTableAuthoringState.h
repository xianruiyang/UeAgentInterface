#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UDataTable;
class FJsonObject;

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	bool IsDataTableTargetAbsent(const FString& AssetPath);
	bool BuildDataTablePresenceReadRequests(const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildDataTableRowSchema(const UDataTable& Table, TSharedPtr<FJsonObject>& OutSchema, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterDataTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildDataTableDomainReadRequests(
		const UDataTable& Table, const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildDataTableAuthoringReadRequests(
		const UDataTable& Table,
		const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
}
