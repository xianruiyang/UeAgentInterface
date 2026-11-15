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
	struct FLevelSemanticFactsSet;
	/** Stable logical slots, never native pointers, across persisted readback. */
	struct FLevelActorCaptureScope
	{
		FGuid ActorGuid;
		FString ManagedId;
		FString RelocatedActorPath;
		bool bAllowMissing = false;
		bool bActorState = false;
		bool bActorProperties = false;
		bool bAllComponents = false;
		TSet<FString> ComponentNames;
		bool bComponentProperties = true;
		// Empty retains the existing complete-field capture contract.
		TSet<FString> PropertyNames;
	};
	/**
	 * The single Level capture boundary shared by export and semantic import.
	 * The legacy snapshot is retained for compatibility; the canonical snapshot
	 * and model are the inputs for semantic workspace planning.
	 */
	struct UEAGENTINTERFACE_API FLevelAuthoringCapture
	{
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotPtr CanonicalSnapshot;
		TOptional<UE::UAI::HjsonWorkspace::FCompositionSnapshotBinding> DirectBinding;
		UE::UAI::HjsonWorkspace::FAuthoringModelPtr Model;
		TSharedPtr<const FLevelSemanticFactsSet> SemanticFacts;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		bool bScopedPreflight = false;
		// Shared with the transaction’s partition custody and native relocation producer.
		TSharedPtr<TArray<FLevelActorCaptureScope>> AttributionScope;
		// WorldSettings is a distinct owner, not a reason to observe all Actors.
		TSet<FString> WorldPropertyNames;

		bool IsValid() const
		{
			return !Snapshot.IsEmpty()
				&& (CanonicalSnapshot.IsValid() || (DirectBinding.IsSet() && DirectBinding->IsValid()))
				&& Model.IsValid();
		}
	};
	/** Same-operation preparation and activation for a subsequent native read.
	 * Activation follows successful independent persisted observation.
	 * On success consumes Capture.Snapshot; canonical/model outputs stay valid. */
	void PrepareLevelCaptureReuse(UWorld& World, FLevelAuthoringCapture& Capture,
		const FString& ProfileId);
	void ActivateLevelCaptureReuse(UWorld& World);
	/** Retain only instance-free comparison material across the required map reload.
	 * The next capture still enumerates and observes every current request. */
	void ReleaseLevelCaptureForPersistentReload();
	void RebindLevelCaptureAfterPersistentReload(UWorld& World);
	void DiscardLevelCaptureReuse();
	void StartLevelCaptureObservation();
	void StopLevelCaptureObservation();

	/** Captures the current Level without mutating the world or package dirty state. */
	UEAGENTINTERFACE_API bool TryCaptureLevelAuthoringState(
		UWorld& World,
		FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId = TEXT("llm_authoring"));
	/** Complete actual observation without canonical/model materialization. */
	bool TryCaptureLevelNativeState(UWorld& World, FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId);
	/** Complete Actor/component content for an explicit Level-owned scope. */
	bool TryCaptureLevelActorNativeState(UWorld& World, const TArray<FGuid>& ActorGuids,
		FLevelAuthoringCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId);
	bool TryCaptureLevelScopedNativeState(UWorld& World, const TArray<FLevelActorCaptureScope>& Scope,
		FLevelAuthoringCapture& OutCapture, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ProfileId, const TSet<FString>& WorldPropertyNames = {});
	bool TryCompleteLevelCanonicalCapture(FLevelAuthoringCapture& Capture,
		UE::UAI::Contract::FDiagnostic& OutError, const FString& ProfileId);
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
