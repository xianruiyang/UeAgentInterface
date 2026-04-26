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
	UPROPERTY(Config, EditAnywhere, Category="Server")
	FString ListenAddress = TEXT("127.0.0.1");

	UPROPERTY(Config, EditAnywhere, Category="Server")
	int32 Port = 17777;

	UPROPERTY(Config, EditAnywhere, Category="Server")
	FString AuthToken;

	UPROPERTY(Config, EditAnywhere, Category="Screenshots")
	bool bEnableScreenshots = true;

	UPROPERTY(Config, EditAnywhere, Category="Artifacts")
	FString ArtifactsDirRelative = TEXT("Saved/RemoteArtifacts");

	UPROPERTY(Config, EditAnywhere, Category="Import")
	TArray<FDirectoryPath> ImportRootDirs;

	UPROPERTY(Config, EditAnywhere, Category="Import")
	FString DefaultImportDestPath = TEXT("/Game/Imported");

	static const UUeAgentInterfaceSettings* Get();
	static UUeAgentInterfaceSettings* GetMutable();
};

