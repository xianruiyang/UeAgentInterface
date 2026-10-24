#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "Curves/SimpleCurve.h"
#include "UObject/Object.h"
#include "UeAgentAuthoringExampleTestTypes.generated.h"

UENUM(meta=(Bitflags, UseEnumValuesAsMaskValuesInEditor="true"))
enum class EUeAgentExampleFlags : uint8 { First = 1, Second = 2 };

UCLASS()
class UUeAgentExampleValueTarget : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category=Test)
	FRichCurve Rich;
	UPROPERTY(EditAnywhere, Category=Test)
	FSimpleCurve Simple;
	UPROPERTY(EditAnywhere, Category=Test)
	FRuntimeFloatCurve Runtime;
	UPROPERTY(EditAnywhere, Category=Test, meta=(Bitmask, BitmaskEnum="/Script/UeAgentInterfaceTests.EUeAgentExampleFlags"))
	int32 Flags = 0;
};
