#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentAuthoringWorkspaceTransaction::DurableDirectory
{
	bool Snapshot(const FString& Root, bool& bExists, FString& Hash, UE::UAI::Contract::FDiagnostic& Error);
	bool Copy(const FString& Destination, const FString& Source, UE::UAI::Contract::FDiagnostic& Error);
	bool Remove(const FString& Root, UE::UAI::Contract::FDiagnostic& Error);
}
