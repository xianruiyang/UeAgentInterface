#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class FJsonValue;
struct FSplineCurves;

// Authored control points only; reparameterization and automatic tangents remain native derived data.
namespace UeAgentSplineValue
{
using UE::UAI::Contract::FDiagnostic;
TSharedPtr<FJsonObject> Encode(const FSplineCurves& Curves);
bool Decode(const TSharedPtr<FJsonValue>& Value, FSplineCurves& Curves, FDiagnostic& Error);
}
