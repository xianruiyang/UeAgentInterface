#pragma once

#include "CoreMinimal.h"

class FJsonValue;

namespace UeAgentNumericFormatting
{
    // Native binary values only. The selected overload preserves the actual
    // storage width; exact decimal lexemes and ordinary strings never use it.
    // PreserveDecimalKind retains the .0 marker required by untyped JSON/HJSON.
    bool TryFormatNative(float Value, bool PreserveDecimalKind, FString& OutText);
    bool TryFormatNative(double Value, bool PreserveDecimalKind, FString& OutText);
    TSharedPtr<FJsonValue> MakeNativeValue(float Value);
    TSharedPtr<FJsonValue> MakeNativeValue(double Value);
}
