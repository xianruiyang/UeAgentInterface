// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UPCGGraph;

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UeAgentPCGAuthoring
{
	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		UPCGGraph& Graph,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
