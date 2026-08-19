// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	UEAGENTINTERFACE_API bool ResolveDefaultWorkspaceFolder(
		const FProfile& Profile,
		const FString& AssetPath,
		FString& OutWorkspaceRoot,
		FString& OutError);

	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		const FProfile& Profile,
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError);

	UEAGENTINTERFACE_API bool ExportAssetFile(
		const FProfile& Profile,
		UObject& Asset,
		const FString& OutputFile,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError);
}

