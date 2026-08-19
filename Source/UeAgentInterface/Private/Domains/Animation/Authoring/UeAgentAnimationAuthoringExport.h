// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentAnimationAuthoring {
UEAGENTINTERFACE_API bool
ResolveDefaultWorkspaceFolder(const FProfile &Profile, const FString &AssetPath,
                              FString &OutWorkspaceRoot, FString &OutError);
UEAGENTINTERFACE_API bool
ExportAssetWorkspace(const FProfile &Profile, UObject &Asset,
                     const FString &WorkspaceRoot,
                     const FString &ExportProfileId,
                     UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
                     FString &OutError);
} // namespace UeAgentAnimationAuthoring
