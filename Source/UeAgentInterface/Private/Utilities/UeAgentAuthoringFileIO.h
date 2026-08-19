#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentAuthoringFileIO
{
	struct UEAGENTINTERFACE_API FSaveOptions
	{
		FString SourceCommand;
		FString WorkspaceRoot;
		bool bExternalizeInlineDetails = true;
		bool bScopeInlineDetailsToEntryFile = false;
		bool bRemoveDiagnosticAuthoringFields = true;
		bool bNormalizeAuthoringFields = true;
		bool bRequireObject = true;
		bool bRecordResolvedPathInWrittenFiles = true;
		TArray<FString>* OutWrittenFiles = nullptr;
	};

	struct UEAGENTINTERFACE_API FLoadOptions
	{
		FString WorkspaceRoot;
		bool bOptional = false;
		bool bRehydrateGroupedDetails = false;
	};

	UEAGENTINTERFACE_API bool SaveObject(
		const FString& FilePathInput,
		const TSharedPtr<FJsonObject>& Object,
		const FSaveOptions& Options,
		FString& OutResolvedPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool SaveObject(
		const FString& FilePathInput,
		const TSharedPtr<FJsonObject>& Object,
		const FSaveOptions& Options,
		FString* OutError = nullptr);

	UEAGENTINTERFACE_API bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		FString& OutResolvedPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		FString& OutError);
}
