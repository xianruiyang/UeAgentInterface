// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"

#include "Components/ActorComponent.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		void SetFailure(
			FLevelExportResult& OutResult,
			const FString& ErrorCode,
			const FString& ErrorDetail,
			FString& OutError)
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = ErrorCode;
			OutResult.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
			OutError = ErrorDetail.IsEmpty() ? ErrorCode : ErrorCode + TEXT(":") + ErrorDetail;
		}

		bool CaptureLevelExportState(
			UWorld& World,
			UeAgentAuthoringState::FCaptureResult& OutCapture,
			int32& OutActorCount,
			int32& OutComponentCount,
			FString& OutError)
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
				OutError = OutCapture.ErrorCode + TEXT(":")
					+ OutCapture.ErrorDetail;
				return false;
			}
			return true;
		}

	}

	TSharedRef<FJsonObject> FLevelExportResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		if (!ErrorCode.IsEmpty()) Result->SetStringField(TEXT("error_code"), ErrorCode);
		if (!ErrorDetail.IsEmpty()) Result->SetStringField(TEXT("error_detail"), ErrorDetail);
		Result->SetStringField(TEXT("workspace_root"), WorkspaceRoot);
		Result->SetStringField(TEXT("profile"), ProfileId);
		Result->SetNumberField(TEXT("actor_count"), ActorCount);
		Result->SetNumberField(TEXT("component_count"), ComponentCount);
		Result->SetNumberField(TEXT("file_count"), FileCount);
		Result->SetNumberField(TEXT("token_budget"), TokenBudget);
		Result->SetObjectField(TEXT("metrics"), Metrics.ToJsonObject());
		if (!EntryFile.IsEmpty()) Result->SetStringField(TEXT("entry_file"), EntryFile);
		if (PublisherReceipt.IsValid())
		{
			Result->SetObjectField(TEXT("publisher"), PublisherReceipt.ToSharedRef());
		}
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FString& Path : WrittenFiles) Files.Add(MakeShared<FJsonValueString>(Path));
		Result->SetArrayField(TEXT("written_files"), Files);
		TArray<TSharedPtr<FJsonValue>> RelativeFiles;
		for (const FString& Path : WrittenRelativeFiles)
		{
			RelativeFiles.Add(MakeShared<FJsonValueString>(Path));
		}
		Result->SetArrayField(TEXT("written_relative_files"), RelativeFiles);
		return Result;
	}

	bool ExportLevelWorkspace(
		UWorld& World,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		FLevelExportResult& OutResult,
		FString& OutError)
	{
		OutResult = FLevelExportResult();
		OutResult.Status = TEXT("failed");
		OutResult.WorkspaceRoot = WorkspaceRoot;
		OutResult.ProfileId = ProfileId.IsEmpty() ? TEXT("llm_authoring") : ProfileId;
		OutError.Reset();
		if (WorkspaceRoot.IsEmpty())
		{
			OutError = TEXT("level_export_workspace_root_missing");
			return false;
		}

		const bool bPackageDirtyBefore = World.GetOutermost()->IsDirty();
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!CaptureLevelExportState(
			World,
			Capture,
			OutResult.ActorCount,
			OutResult.ComponentCount,
			OutError))
		{
			SetFailure(
				OutResult,
				TEXT("export_level_state_capture_failed"),
				OutError,
				OutError);
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			SetFailure(OutResult, TEXT("level_export_mutated_package_dirty_state"), FString(), OutError);
			return false;
		}
		UeAgentExportWorkspaceWriter::FWorkspaceResult WorkspaceResult;
		if (!UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
			TEXT("ue.level.asset"),
			OutResult.ProfileId,
			Capture.Snapshot,
			WorkspaceRoot,
			WorkspaceResult,
			OutError))
		{
			SetFailure(
				OutResult,
				WorkspaceResult.ErrorCode.IsEmpty()
					? TEXT("export_level_publish_failed")
					: WorkspaceResult.ErrorCode,
				OutError,
				OutError);
			return false;
		}
		if (World.GetOutermost()->IsDirty() != bPackageDirtyBefore)
		{
			SetFailure(OutResult, TEXT("level_export_mutated_package_dirty_state"), FString(), OutError);
			return false;
		}
		OutResult.WorkspaceRoot = WorkspaceResult.WorkspaceRoot;
		OutResult.Metrics = WorkspaceResult.Metrics;
		OutResult.FileCount = WorkspaceResult.Files.Num();
		OutResult.TokenBudget = WorkspaceResult.TokenBudget;
		OutResult.EntryFile = WorkspaceResult.EntryFile;
		OutResult.WrittenFiles = WorkspaceResult.WrittenFiles;
		OutResult.WrittenRelativeFiles =
			WorkspaceResult.WrittenRelativeFiles;
		OutResult.PublisherReceipt =
			WorkspaceResult.PublisherReceipt;
		OutResult.Status = TEXT("written");
		OutResult.ErrorCode.Reset();
		OutResult.ErrorDetail.Reset();
		OutError.Reset();
		return true;
	}
}
