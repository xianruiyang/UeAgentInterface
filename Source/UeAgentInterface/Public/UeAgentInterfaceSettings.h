// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UeAgentInterfaceSettings.generated.h"

UCLASS(Config=UeAgentInterface, DefaultConfig)
class UEAGENTINTERFACE_API UUeAgentInterfaceSettings : public UObject
{
	GENERATED_BODY()

public:
	UUeAgentInterfaceSettings();

	UPROPERTY(Config, EditAnywhere, Category="Server")
	FString ListenAddress;

	UPROPERTY(Config, EditAnywhere, Category="Server")
	int32 Port;

	UPROPERTY(Config, EditAnywhere, Category="Server")
	FString AuthToken;

	UPROPERTY(Config, EditAnywhere, Category="Artifacts")
	FString ArtifactsDirRelative;

	UPROPERTY(Config, EditAnywhere, Category="Import")
	TArray<FDirectoryPath> ImportRootDirs;

	UPROPERTY(Config, EditAnywhere, Category="Import")
	FString DefaultImportDestPath;

	static const UUeAgentInterfaceSettings* Get();
	static UUeAgentInterfaceSettings* GetMutable();
};
