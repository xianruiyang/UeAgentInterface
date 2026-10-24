// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentAnimationAuthoring {
UEAGENTINTERFACE_API bool
ResolveDefaultWorkspaceFolder(const FProfile &Profile, const FString &AssetPath,
                              FString &OutWorkspaceRoot, UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool
ExportAssetWorkspace(const FProfile &Profile, UObject &Asset,
                     const FString &WorkspaceRoot,
                     const FString &ExportProfileId,
                     UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult);
} // namespace UeAgentAnimationAuthoring
