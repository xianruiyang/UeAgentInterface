#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::StateSemantic::Private
{
	bool TryNormalizeNfcText(const FString& Value, FString& OutValue);
}
