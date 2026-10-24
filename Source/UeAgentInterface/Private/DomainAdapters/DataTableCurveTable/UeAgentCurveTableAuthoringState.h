#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UCurveTable;
class FJsonObject;

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	UEAGENTINTERFACE_API bool RegisterCurveTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool CaptureCurveTableRows(const UCurveTable& Table, TSharedPtr<FJsonObject>& OutValue, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool IsCurveTableTargetAbsent(const FString& Path);
	bool BuildCurveTablePresenceReadRequests(const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildCurveTableDomainReadRequests(const UCurveTable& Table, const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildCurveTableAuthoringReadRequests(const UCurveTable& Table, const FString& Path,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
}
