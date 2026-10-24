#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UObject;

namespace UeAgentAuthoringObjectMetadata {
// Object-key package metadata, distinct from an asset's instanced MetaData UPROPERTY.
TSharedPtr<FJsonObject> Read(const UObject& Object);
bool ApplyDesired(UObject& Object, const FJsonObject& Desired, UE::UAI::Contract::FDiagnostic& Error);
void ApplyChanges(UObject& Object, const TMap<FName, TOptional<FString>>& Changes);
}
