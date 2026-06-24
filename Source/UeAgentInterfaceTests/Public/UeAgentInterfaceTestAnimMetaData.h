// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimMetaData.h"
#include "UeAgentInterfaceTestAnimMetaData.generated.h"

UCLASS()
class UEAGENTINTERFACETESTS_API UUeAgentInterfaceTestAnimMetaData : public UAnimMetaData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "UAI")
	FString Tag;

	UPROPERTY(EditAnywhere, Category = "UAI")
	FString Note;
};
