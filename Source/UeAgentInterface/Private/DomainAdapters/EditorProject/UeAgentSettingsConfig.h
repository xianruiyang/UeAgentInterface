#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FProperty;
class UObject;

namespace UE::UAI::DomainAdapters::EditorProject
{
UEAGENTINTERFACE_API FString SettingsConfigKey(const FProperty& Property);
UEAGENTINTERFACE_API FString SettingsConfigFilename(const UObject& Settings);
UEAGENTINTERFACE_API bool StageSettingsConfig(UObject& Settings, TFunctionRef<bool(const FProperty&)> HasChanged,
	const FString& FinalPath, const FString& StagingPath, UE::UAI::Contract::FDiagnostic& Error);
// Reads the actual file through native LoadConfig into a detached pre-edit object.
// The temporary cache key cannot overwrite a live config branch or modify CVars.
UEAGENTINTERFACE_API bool ReadSettingsConfig(UObject& Snapshot, const UObject& Owner, const FString& Filename, UE::UAI::Contract::FDiagnostic& Error);
}
