// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Elements/PCGExecuteBlueprint.h"

#include "UeAgentInterfacePCGSmokeBlueprintElement.generated.h"

UCLASS()
class UUeAgentInterfacePCGSmokeBlueprintElement : public UPCGBlueprintElement
{
	GENERATED_BODY()

public:
	UUeAgentInterfacePCGSmokeBlueprintElement();

	virtual void ExecuteWithContext_Implementation(FPCGContext& InContext, const FPCGDataCollection& Input, FPCGDataCollection& Output) override;
	virtual FName NodeTitleOverride_Implementation() const override;
};
