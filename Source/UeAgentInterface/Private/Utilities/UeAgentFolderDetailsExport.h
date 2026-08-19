#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"

class UObject;
class FJsonValue;

namespace UeAgentDetailsAuthoring
{
	struct FOwnerDetailsExportRequest;
}

namespace UeAgentFolderDetailsExport
{
	struct FOwnerDetailsAttachOptions
	{
		FString RefFieldName;
		bool bUseOwnerScope;
		TArray<FString>* OutWrittenFiles;
		TFunction<bool(
			const UeAgentDetailsAuthoring::FOwnerDetailsExportRequest& Request,
			TSharedPtr<FJsonObject>& InOutDetailsSummary,
			FString& OutError)> PostExportDetails;

		FOwnerDetailsAttachOptions()
			: RefFieldName(TEXT("details"))
			, bUseOwnerScope(true)
			, OutWrittenFiles(nullptr)
		{
		}
	};

	struct FPropertyArrayDetailsAttachOptions
	{
		FString RefFieldName;
		FString PropertiesFieldName;
		FString CountFieldName;
		bool bUseOwnerScope;
		TArray<FString>* OutWrittenFiles;

		FPropertyArrayDetailsAttachOptions()
			: RefFieldName(TEXT("details"))
			, PropertiesFieldName(TEXT("properties"))
			, bUseOwnerScope(true)
			, OutWrittenFiles(nullptr)
		{
		}
	};

	FString MakeDetailsStem(const FString& InValue);

	bool AttachOwnerDetails(
		UObject* Owner,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError,
		const FOwnerDetailsAttachOptions& Options);

	bool AttachOwnerDetails(
		UObject* Owner,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError,
		const FString& RefFieldName = TEXT("details"));

	bool AttachPropertyArrayDetails(
		const TArray<TSharedPtr<FJsonValue>>& Properties,
		const FString& EntryDirectory,
		const FString& SourceCommand,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& OwnerName,
		const FString& OwnerPath,
		const FString& OwnerClassPath,
		const FString& Category,
		const TSharedPtr<FJsonObject>& EntryObject,
		FString& OutError,
		const FPropertyArrayDetailsAttachOptions& Options);
}
