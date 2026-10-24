#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
struct FUeAgentStructuredHjsonDocument;
struct FUeAgentStructuredHjsonWriteOptions;

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
			UE::UAI::Contract::FDiagnostic* OutError = nullptr);

	UEAGENTINTERFACE_API bool SaveObject(
			const FString& FilePath,
			const TSharedPtr<FJsonObject>& Object,
			const FUeAgentStructuredHjsonWriteOptions& Options,
			UE::UAI::Contract::FDiagnostic* OutError = nullptr);

	UEAGENTINTERFACE_API bool LoadObject(
			const FString& FilePath,
			TSharedPtr<FJsonObject>& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool LoadDocument(
			const FString& FilePath,
			FUeAgentStructuredHjsonDocument& OutDocument,
			UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool LoadOptionalObject(
			const FString& FilePath,
			TSharedPtr<FJsonObject>& OutObject,
			UE::UAI::Contract::FDiagnostic& OutError);
}
