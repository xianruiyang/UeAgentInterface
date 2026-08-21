// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Level.h"

bool FUeAgentLegacyMechanics::WriteCompressedImage(const TArray<FColor>& Pixels, const FIntPoint& Size, const FString& Format, int32 Quality, const FString& OutFilePath, FString& OutError)
{
	return WriteCompressedImage(Pixels, Size, Format, Quality, OutFilePath, OutError, true);
}
