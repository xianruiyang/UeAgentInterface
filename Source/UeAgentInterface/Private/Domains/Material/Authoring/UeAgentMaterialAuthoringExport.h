// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentMaterialAuthoring
{
	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError);
}
