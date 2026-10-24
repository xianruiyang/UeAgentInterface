#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRelations.h"

class FJsonValue;
class USoundSubmixBase;
class UPackage;

namespace UeAgentReflectionAuthoring::SoundSubmixRelations
{
bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
TSharedPtr<FJsonValue> Read(const USoundSubmixBase& Asset);
bool Prepare(USoundSubmixBase& Root, const TSharedPtr<FJsonValue>& Value,
	UeAgentAuthoringAssetRelations::FPlan& OutPlan, UE::UAI::Contract::FDiagnostic& OutError);
void RefreshDevices(USoundSubmixBase& Asset);
UObject* CreatePreview(USoundSubmixBase& Source, UPackage& Package, UE::UAI::Contract::FDiagnostic& OutError);
}
