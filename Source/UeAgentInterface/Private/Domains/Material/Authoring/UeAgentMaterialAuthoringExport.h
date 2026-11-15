// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UObject;
namespace UE::UAI::HjsonWorkspace { class FResolvedSemanticWorkspace; }

namespace UeAgentMaterialAuthoring
{
	/**
	 * The single Material capture boundary shared by semantic export, import,
	 * and independent content-graph readback.
	 */
	struct UEAGENTINTERFACE_API FMaterialAuthoringCapture
	{
		FString TargetKind;
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
		UE::UAI::HjsonWorkspace::FAuthoringModelPtr Model;

		bool IsValid() const
		{
			return !TargetKind.IsEmpty()
				&& !Snapshot.IsEmpty()
				&& CanonicalSnapshot.IsValid()
				&& Model.IsValid();
		}
	};

	UEAGENTINTERFACE_API bool TryCaptureMaterialAuthoringState(
		UObject& Asset,
		const FString& ProfileId,
		FMaterialAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspace* ImportWorkspace = nullptr);

	UEAGENTINTERFACE_API bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
