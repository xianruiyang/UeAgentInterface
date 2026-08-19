// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;
class UBlueprint;
class FJsonObject;

namespace UeAgentBlueprintAuthoring {
UEAGENTINTERFACE_API bool
ExportAssetWorkspace(UObject &Asset, const FString &WorkspaceRoot,
                     const FString &ProfileId,
                     UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
                     FString &OutError);
UEAGENTINTERFACE_API bool BuildReusableProviderExtension(
    UBlueprint &Blueprint, TSharedPtr<FJsonObject> &OutExtension,
    FString &OutError, const FString &CanonicalAssetPath = FString());
} // namespace UeAgentBlueprintAuthoring
