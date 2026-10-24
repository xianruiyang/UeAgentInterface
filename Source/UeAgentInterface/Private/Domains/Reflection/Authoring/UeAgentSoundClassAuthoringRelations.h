#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRelations.h"

class FJsonValue;
class USoundClass;

namespace UeAgentReflectionAuthoring::SoundClassRelations
{
bool Validate(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
TSharedPtr<FJsonValue> Read(const USoundClass& Asset);
void RefreshDerivedGraphs(USoundClass& Asset);

bool Prepare(USoundClass& Root, const TSharedPtr<FJsonValue>& Value,
	UeAgentAuthoringAssetRelations::FPlan& OutPlan, UE::UAI::Contract::FDiagnostic& OutError);
}
