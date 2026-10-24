#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class UObject;

namespace UeAgentMlDeformerAuthoring
{
bool Read(UObject& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error);
bool Apply(UObject& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error);
}
