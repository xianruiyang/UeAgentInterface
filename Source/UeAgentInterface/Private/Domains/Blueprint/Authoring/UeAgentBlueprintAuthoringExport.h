// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;
class UBlueprint;
class FJsonObject;

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UeAgentBlueprintAuthoring {
UEAGENTINTERFACE_API bool
ExportAssetWorkspace(UObject &Asset, const FString &WorkspaceRoot,
                     const FString &ProfileId,
                     UeAgentExportWorkspaceWriter::FWorkspaceResult &OutResult,
                     UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
UEAGENTINTERFACE_API bool BuildReusableProviderExtension(
    UBlueprint &Blueprint, TSharedPtr<FJsonObject> &OutExtension,
    UE::UAI::Contract::FDiagnostic& OutError, const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool BuildReusableVerificationState(
    UBlueprint &Blueprint, TSharedPtr<FJsonObject> &OutState,
    UE::UAI::Contract::FDiagnostic& OutError, const FString &CanonicalAssetPath = FString());
} // namespace UeAgentBlueprintAuthoring
