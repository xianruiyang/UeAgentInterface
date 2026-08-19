// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UWidgetBlueprint;

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		UWidgetBlueprint& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError);
}
