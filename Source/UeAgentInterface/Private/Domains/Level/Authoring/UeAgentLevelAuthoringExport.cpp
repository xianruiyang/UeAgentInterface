// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"

#include "Components/ActorComponent.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		void SetFailure(FLevelExportResult& OutResult, const UE::UAI::Contract::FDiagnostic& Diagnostic)
        {
            OutResult.Status = TEXT("failed");
            OutResult.Diagnostic = Diagnostic;
        }

		bool CaptureLevelExportState(
			UWorld& World,
			UeAgentAuthoringState::FCaptureResult& OutCapture,
			int32& OutActorCount,
			int32& OutComponentCount,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutActorCount = 0;
			OutComponentCount = 0;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			TArray<UeAgentAuthoringState::FReadRequest> TargetRequests;
			if (!BuildWorldExportReadRequests(
				World,
				TargetRequests,
				OutError))
			{
				return false;
			}
			Requests.Append(MoveTemp(TargetRequests));

			TArray<AActor*> Actors;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor
					&& EvaluateActorAuthorability(
						World,
						*Actor).bAuthorable)
				{
					Actors.Add(Actor);
				}
			}
			Actors.Sort(
				[](const AActor& Left, const AActor& Right)
				{
					return Left.GetActorGuid().ToString(
							EGuidFormats::DigitsWithHyphensLower)
						< Right.GetActorGuid().ToString(
							EGuidFormats::DigitsWithHyphensLower);
				});
			for (AActor* Actor : Actors)
			{
				if (!BuildActorExportReadRequests(
					*Actor,
					TargetRequests,
					OutError))
				{
					return false;
				}
				Requests.Append(MoveTemp(TargetRequests));
				++OutActorCount;
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				Components.Sort(
					[](const UActorComponent& Left,
						const UActorComponent& Right)
					{
						return Left.GetName() < Right.GetName();
					});
				for (UActorComponent* Component : Components)
				{
					if (!Component
						|| !EvaluateComponentAuthorability(
							World,
							*Component).bAuthorable)
					{
						continue;
					}
					if (!BuildComponentExportReadRequests(
						*Component,
						TargetRequests,
						OutError))
					{
						return false;
					}
					Requests.Append(MoveTemp(TargetRequests));
					++OutComponentCount;
				}
			}
			if (!UeAgentAuthoringState::CaptureSnapshot(
				Requests,
				OutCapture))
			{
				OutError = OutCapture.Diagnostic;
				return false;
			}
			return true;
		}

	}

	TSharedRef<FJsonObject> FLevelExportResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		if (!Diagnostic.IsEmpty()) Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		Result->SetStringField(TEXT("workspace_root"), WorkspaceRoot);
		Result->SetStringField(TEXT("profile"), ProfileId);
		Result->SetNumberField(TEXT("actor_count"), ActorCount);
		Result->SetNumberField(TEXT("component_count"), ComponentCount);
		Result->SetNumberField(TEXT("file_count"), FileCount);
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		Result->SetObjectField(
			TEXT("model_metrics"), ModelMetrics.ToJsonObject());
		if (!EntryFile.IsEmpty()) Result->SetStringField(TEXT("entry_file"), EntryFile);
		if (PublisherReceipt.IsValid())
		{
			Result->SetObjectField(TEXT("publisher"), PublisherReceipt.ToSharedRef());
		}
		Result->SetNumberField(
			TEXT("written_file_count"), WrittenFiles.Num());
		return Result;
	}

	bool ExportLevelWorkspace(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		FLevelExportResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = FLevelExportResult();
		OutResult.Status = TEXT("failed");
		OutResult.WorkspaceRoot = WorkspaceRoot;
		OutResult.ProfileId = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		OutError.Reset();
		if (WorkspaceRoot.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_workspace_root_missing"),
				TEXT("Level export requires a non-empty workspace output directory."), TEXT("level_export"));
			OutError.PropertyPath = TEXT("workspace_root");
			OutError.ActualValue = MakeShared<FJsonValueString>(WorkspaceRoot);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty output directory"));
			return false;
		}

		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		FLevelAuthoringCapture Capture;
		if (!TryCaptureLevelAuthoringState(World, Capture, OutError))
		{
			SetFailure(OutResult, OutError);
			return false;
		}
		OutResult.ActorCount = Capture.ActorCount;
		OutResult.ComponentCount = Capture.ComponentCount;
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while capturing the Level state."), TEXT("level_export"));
			Error.TargetPath = GetWorldPath(World);
			Error.PropertyPath = TEXT("package_dirty");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			Error.SourceKind = TEXT("native_result");
			SetFailure(OutResult, Error);
			return false;
		}
		UeAgentExportWorkspaceWriter::FWorkspaceResult WorkspaceResult;
		if (!UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(TEXT("ue.level.asset"), OutResult.ProfileId, Capture.Snapshot, WorkspaceRoot, WorkspaceResult))
		{
			SetFailure(OutResult, WorkspaceResult.Diagnostic);
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while writing the Level workspace."), TEXT("level_export"));
			Error.TargetPath = GetWorldPath(World);
			Error.PropertyPath = TEXT("package_dirty");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			Error.SourceKind = TEXT("native_result");
			SetFailure(OutResult, Error);
			return false;
		}
		OutResult.WorkspaceRoot = WorkspaceResult.WorkspaceRoot;
		OutResult.ExpectedRevision = WorkspaceResult.ExpectedRevision;
		OutResult.Metrics = WorkspaceResult.Metrics;
		OutResult.ModelMetrics = WorkspaceResult.ModelMetrics;
		OutResult.FileCount = WorkspaceResult.Files.Num();
		OutResult.EntryFile = WorkspaceResult.EntryFile;
		OutResult.WrittenFiles = WorkspaceResult.WrittenFiles;
		OutResult.WrittenRelativeFiles =
			WorkspaceResult.WrittenRelativeFiles;
		OutResult.PublisherReceipt =
			WorkspaceResult.PublisherReceipt;
		OutResult.Status = TEXT("written");
		OutResult.Diagnostic.Reset();
		OutError.Reset();
		return true;
	}

	bool VerifyLevelAuthoringReadback(UWorld& World,
		const UeAgentAuthoringState::FStateSnapshot& Expected,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FLevelAuthoringCapture Actual;
		if (!TryCaptureLevelAuthoringState(World, Actual, OutError)) return false;
		UeAgentAuthoringSemanticCompare::FComparisonReport Comparison;
		if (!UeAgentAuthoringSemanticCompare::CompareSnapshots(Expected, Actual.Snapshot,
			UeAgentAuthoringSemanticCompare::FCompareOptions(), Comparison))
		{ OutError = Comparison.Diagnostic; return false; }
		if (Comparison.bEquivalent) return true;
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_persistent_readback_mismatch"),
			TEXT("Reloaded level differs from the state verified before saving."), TEXT("persistent_readback"));
		for (const auto& Difference : Comparison.Entries)
		{
			if (Difference.bEquivalent) continue;
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(OutError.Code, Difference.Diagnostic,
				OutError.Stage, OutError.PublicMessage);
			break;
		}
		OutError.TargetPath = World.GetPathName();
		OutError.Stage = TEXT("persistent_readback");
		return false;
	}

	bool TryCaptureLevelAuthoringState(
		UWorld& World,
		FLevelAuthoringCapture& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture = FLevelAuthoringCapture();
		OutError.Reset();
		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!CaptureLevelExportState(
				World,
				Capture,
				OutCapture.ActorCount,
				OutCapture.ComponentCount,
				OutError))
		{
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_capture_mutated_package_dirty_state"),
				TEXT("The world's dirty flag changed while capturing the Level state."), TEXT("level_export"));
			OutError.TargetPath = GetWorldPath(World);
			OutError.PropertyPath = TEXT("package_dirty");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(World.GetOutermost()->IsDirty());
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(bPackageDirtyBefore);
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutCapture.Snapshot = MoveTemp(Capture.Snapshot);
		UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!UE::UAI::HjsonWorkspace::TryBuildCanonicalAuthoringCapture(
				TEXT("ue.level.asset"),
				TEXT("llm_authoring"),
				OutCapture.Snapshot,
				Resources,
				OutCapture.CanonicalSnapshot,
				OutCapture.Model,
				WorkspaceError))
		{
			OutError = WorkspaceError.GetDiagnostic();
			return false;
		}
		return OutCapture.IsValid();
	}
}
