#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
struct FUeAgentStructuredHjsonDocument;

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
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool SaveObject(
		const FString& FilePathInput,
		const TSharedPtr<FJsonObject>& Object,
		const FSaveOptions& Options,
		UE::UAI::Contract::FDiagnostic* OutError = nullptr);

	// Parse required input under the default authoring path policy and retain its source map.
	// The returned document describes the input before optional grouped-details rehydration.
	UEAGENTINTERFACE_API bool LoadDocument(
		const FString& FilePathInput,
		FUeAgentStructuredHjsonDocument& OutDocument,
		FString& OutResolvedPath,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		FString& OutResolvedPath,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool LoadObject(
		const FString& FilePathInput,
		TSharedPtr<FJsonObject>& OutObject,
		const FLoadOptions& Options,
		UE::UAI::Contract::FDiagnostic& OutError);
}
