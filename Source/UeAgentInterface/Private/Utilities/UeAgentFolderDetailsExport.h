#pragma once

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"

class UObject;

namespace UeAgentFolderDetailsExport
{
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
		const FString& RefFieldName = TEXT("details"));
}
