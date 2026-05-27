// Copyright Epic Games, Inc. All Rights Reserved.

#include "Smoke/PCG/UeAgentInterfacePCGSmokeBlueprintElement.h"

#include "PCGPin.h"

UUeAgentInterfacePCGSmokeBlueprintElement::UUeAgentInterfacePCGSmokeBlueprintElement()
{
	bHasDefaultOutPin = false;
	CustomOutputPins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
}

void UUeAgentInterfacePCGSmokeBlueprintElement::ExecuteWithContext_Implementation(FPCGContext& InContext, const FPCGDataCollection& Input, FPCGDataCollection& Output)
{
	Output = Input;
}

FName UUeAgentInterfacePCGSmokeBlueprintElement::NodeTitleOverride_Implementation() const
{
	return TEXT("UAI PCG Smoke Blueprint Element");
}
