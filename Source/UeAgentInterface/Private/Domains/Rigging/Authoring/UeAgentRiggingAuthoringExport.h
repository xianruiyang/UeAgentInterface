// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentRiggingAuthoring
{
	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		const FProfile& Profile,
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ExportProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult);
}
