#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
struct FUeAgentStructuredHjsonDocument;
struct FUeAgentStructuredHjsonWriteOptions;
struct FUeAgentStructuredResourceBudget;

namespace UeAgentStructuredAuthoringIO
{
	UEAGENTINTERFACE_API FString NormalizeAuthoringPath(const FString& FilePathInput);

	UEAGENTINTERFACE_API FString WriteObject(const TSharedRef<FJsonObject>& Object);
	UEAGENTINTERFACE_API FString WriteObject(
		const TSharedRef<FJsonObject>& Object,
		const FUeAgentStructuredHjsonWriteOptions& Options);

	UEAGENTINTERFACE_API bool SaveObject(
			const FString& FilePath,
			const TSharedPtr<FJsonObject>& Object,
			FString* OutError = nullptr);

	UEAGENTINTERFACE_API bool SaveObject(
			const FString& FilePath,
			const TSharedPtr<FJsonObject>& Object,
			const FUeAgentStructuredHjsonWriteOptions& Options,
			FString* OutError = nullptr);

	UEAGENTINTERFACE_API bool LoadObject(
			const FString& FilePath,
			TSharedPtr<FJsonObject>& OutObject,
			FString& OutError);

	UEAGENTINTERFACE_API bool LoadDocument(
			const FString& FilePath,
			FUeAgentStructuredHjsonDocument& OutDocument,
			FString& OutError);

	UEAGENTINTERFACE_API bool LoadDocument(
			const FString& FilePath,
			const FUeAgentStructuredResourceBudget& ResourceBudget,
			FUeAgentStructuredHjsonDocument& OutDocument,
			FString& OutError);

	UEAGENTINTERFACE_API bool LoadOptionalObject(
			const FString& FilePath,
			TSharedPtr<FJsonObject>& OutObject,
			FString& OutError);
}
