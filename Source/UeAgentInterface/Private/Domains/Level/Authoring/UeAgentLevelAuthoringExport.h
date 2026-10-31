// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

class UWorld;
class FJsonObject;

namespace UeAgentLevelAuthoring
{
	/**
	 * The single Level capture boundary shared by export and semantic import.
	 * The legacy snapshot is retained for compatibility; the canonical snapshot
	 * and model are the inputs for semantic workspace planning.
	 */
	struct UEAGENTINTERFACE_API FLevelAuthoringCapture
	{
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
		UE::UAI::HjsonWorkspace::FAuthoringModelPtr Model;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;

		bool IsValid() const
		{
			return !Snapshot.IsEmpty()
				&& CanonicalSnapshot.IsValid()
				&& Model.IsValid();
		}
	};
	/** Same-operation preparation and activation for a subsequent native read.
	 * Activation is reserved for the isolated, non-structural persisted observer. */
	void PrepareLevelCaptureReuse(UWorld& World, FLevelAuthoringCapture& Capture,
		const FString& ProfileId);
	void ActivateLevelCaptureReuse(UWorld& World);
	void DiscardLevelCaptureReuse();
	void StartLevelCaptureObservation();
	void StopLevelCaptureObservation();

	/** Captures the current Level without mutating the world or package dirty state. */
	UEAGENTINTERFACE_API bool TryCaptureLevelAuthoringState(
		UWorld& World,
		FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId = TEXT("llm_authoring"));
	/** Continues verification across an external save/reload, using the same semantic capture. */
	UEAGENTINTERFACE_API bool VerifyLevelAuthoringReadback(UWorld& World,
		const UeAgentAuthoringState::FStateSnapshot& Expected,
		UE::UAI::Contract::FDiagnostic& OutError);

	struct UEAGENTINTERFACE_API FLevelExportResult
	{
		FString Status;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString WorkspaceRoot;
		FString ProfileId;
		FString ExpectedRevision;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		int32 FileCount = 0;
		UeAgentExportWorkspaceWriter::FTextMetrics Metrics;
		UeAgentExportWorkspaceWriter::FTextMetrics ModelMetrics;
		FString EntryFile;
		TArray<FString> WrittenFiles;
		TArray<FString> WrittenRelativeFiles;
		TSharedPtr<FJsonObject> PublisherReceipt;

		bool IsWritten() const { return Status == TEXT("written"); }
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool ExportLevelWorkspace(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		FLevelExportResult& OutResult,
		UE::UAI::HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
