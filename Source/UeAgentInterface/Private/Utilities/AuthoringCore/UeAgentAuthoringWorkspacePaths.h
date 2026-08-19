#pragma once

#include "CoreMinimal.h"

namespace UeAgentAuthoringWorkspacePaths
{
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
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveProjectPath(
		const FString& PathInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveReference(
		const FString& WorkspaceRootInput,
		const FString& SourceFileInput,
		const FString& ReferenceInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool DetectCaseCollisions(
		const FString& WorkspaceRootInput,
		const TArray<FString>& PathInputs,
		FString& OutError);

	UEAGENTINTERFACE_API bool MakeAssetRelativePath(
		const FString& AssetPathInput,
		FString& OutRelativePath,
		FString& OutError);

	UEAGENTINTERFACE_API bool ResolveAssetWorkspaceFolder(
		const FString& WorkspaceRootInput,
		const FString& AssetPathInput,
		FString& OutFolderPath,
		FString& OutError);
}
