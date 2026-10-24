#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UeAgentAuthoringWorkspacePaths
{
	using FDiagnostic = UE::UAI::Contract::FDiagnostic;
	struct UEAGENTINTERFACE_API FResolveOptions
	{
		bool bRequireHjson = false;
		bool bAllowRoot = false;
		bool bRequireExists = false;
		bool bRequireDirectory = false;
	};

	struct UEAGENTINTERFACE_API FResolvedPath
	{
		FString WorkspaceRoot;
		FString AbsolutePath;
		FString RelativePath;
		FString CanonicalKey;
		bool bExists = false;
	};

	UEAGENTINTERFACE_API FString NormalizeAbsolutePath(const FString& PathInput);

	UEAGENTINTERFACE_API bool ResolveUnderRoot(
		const FString& WorkspaceRootInput,
		const FString& PathInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResolveProjectPath(
		const FString& PathInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResolveReference(
		const FString& WorkspaceRootInput,
		const FString& SourceFileInput,
		const FString& ReferenceInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool DetectCaseCollisions(
		const FString& WorkspaceRootInput,
		const TArray<FString>& PathInputs,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool MakeAssetRelativePath(
		const FString& AssetPathInput,
		FString& OutRelativePath,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ResolveAssetWorkspaceFolder(
		const FString& WorkspaceRootInput,
		const FString& AssetPathInput,
		FString& OutFolderPath,
		FDiagnostic& OutError);
}
