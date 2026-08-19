// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UWorld;
class FJsonObject;

namespace UeAgentLevelAuthoring
{
	struct UEAGENTINTERFACE_API FLevelExportResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString WorkspaceRoot;
		FString ProfileId;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		int32 FileCount = 0;
		int32 TokenBudget = 0;
		UeAgentExportWorkspaceWriter::FTextMetrics Metrics;
		FString EntryFile;
		TArray<FString> WrittenFiles;
		TArray<FString> WrittenRelativeFiles;
		TSharedPtr<FJsonObject> PublisherReceipt;

		bool IsWritten() const { return Status == TEXT("written"); }
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool ExportLevelWorkspace(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		FLevelExportResult& OutResult,
		FString& OutError);
}
