// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

class FJsonObject;

namespace UeAgentExportPublisher
{
	UEAGENTINTERFACE_API bool IsOwnershipMetadataPath(const FString& RelativePath);

	struct UEAGENTINTERFACE_API FPublishContext
	{
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
	};

	struct UEAGENTINTERFACE_API FFilePlan
	{
		FString RelativePath;
		TSharedPtr<FJsonObject> Root;
		FUeAgentStructuredHjsonWriteOptions WriteOptions;
	};

	struct UEAGENTINTERFACE_API FPublishResult
	{
		TSharedPtr<FJsonObject> Receipt;
		TArray<FString> WrittenFiles;
		TArray<FString> WrittenRelativeFiles;
	};

	UEAGENTINTERFACE_API bool PublishWorkspace(
		const FString& WorkspaceRoot,
		const TArray<FFilePlan>& Files,
		const FPublishContext& Context,
		FPublishResult& OutResult,
		FString& OutErrorCode,
		FString& OutErrorDetail);

	UEAGENTINTERFACE_API bool PublishSingleFile(
		const FString& OutputFile,
		const FFilePlan& File,
		const FPublishContext& Context,
		FPublishResult& OutResult,
		FString& OutErrorCode,
		FString& OutErrorDetail);

	UEAGENTINTERFACE_API bool ResetOwnedWorkspace(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		FString& OutError);
}
