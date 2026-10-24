// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;

namespace UeAgentNiagaraAuthoring
{
	inline constexpr const TCHAR* ExportContractId = TEXT("uai.export.niagara.asset");
	inline constexpr const TCHAR* StackProjectionId = TEXT("uai.export.niagara.stack");

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult);
}
