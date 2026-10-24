#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FStringTable;
class FJsonObject;

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> CaptureStringTableState(const FStringTable& Table);
	UEAGENTINTERFACE_API bool RegisterStringTableAuthoringState(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool IsStringTableTargetAbsent(const FString& AssetPath);
	UEAGENTINTERFACE_API bool BuildStringTablePresenceReadRequests(const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildStringTableAuthoringReadRequests(const FStringTable& Table,
		const FString& AssetPath, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError);
}
