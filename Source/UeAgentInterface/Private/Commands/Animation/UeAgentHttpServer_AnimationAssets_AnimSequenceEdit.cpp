// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"

#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/AnimEnums.h"
#include "Animation/AnimTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimPose.h"
#include "AnimPreviewInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "IAnimationEditor.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "Misc/PackageName.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

bool FUeAgentHttpServer::CmdAnimSequenceSetMetadata(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bClearAll = false;
	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_all"), bClearAll);
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	Sequence->Modify();

	if (bClearAll)
	{
		UAnimationBlueprintLibrary::RemoveAllMetaData(Sequence);
	}
	else
	{
		FString MetadataClassPath;
		if (!JsonTryGetString(Ctx.Params, TEXT("metadata_class_path"), MetadataClassPath) || MetadataClassPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_metadata_class_path");
			return false;
		}

		UClass* MetadataClass = UeAgentAnimationAssetOps::LoadClassByPath(MetadataClassPath);
		if (!MetadataClass || !MetadataClass->IsChildOf(UAnimMetaData::StaticClass()))
		{
			OutError = TEXT("invalid_metadata_class");
			return false;
		}

		if (bRemove)
		{
			UAnimationBlueprintLibrary::RemoveMetaDataOfClass(Sequence, MetadataClass);
		}
		else
		{
			if (MetadataClass->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = TEXT("metadata_class_is_abstract");
				return false;
			}

			UAnimMetaData* TargetMetaData = Sequence->FindMetaDataByClass<UAnimMetaData>();
			TArray<UAnimMetaData*> MetaDataOfClass;
			UAnimationBlueprintLibrary::GetMetaDataOfClass(Sequence, MetadataClass, MetaDataOfClass);
			TargetMetaData = MetaDataOfClass.Num() > 0 ? MetaDataOfClass[0] : nullptr;

			if (!TargetMetaData)
			{
				UAnimMetaData* CreatedMetaData = nullptr;
				UAnimationBlueprintLibrary::AddMetaData(Sequence, MetadataClass, CreatedMetaData);
				if (!CreatedMetaData)
				{
					OutError = TEXT("add_metadata_failed");
					return false;
				}
				TargetMetaData = CreatedMetaData;
			}

			const TSharedPtr<FJsonObject>* MetadataValues = nullptr;
			if (Ctx.Params->TryGetObjectField(TEXT("metadata_values"), MetadataValues) && MetadataValues && MetadataValues->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*MetadataValues)->Values)
				{
					FProperty* Property = UeAgentAnimationAssetOps::FindEditablePropertyByFlexibleName(TargetMetaData->GetClass(), Pair.Key);
					if (!Property)
					{
						OutError = TEXT("metadata_property_not_found");
						return false;
					}
					if (!UeAgentAnimationAssetOps::ApplySimpleJsonValueToProperty(TargetMetaData, Property, Pair.Value, OutError))
					{
						return false;
					}
				}
				TargetMetaData->MarkPackageDirty();
			}
		}
	}

	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	TArray<UAnimMetaData*> MetaDataObjects;
	UAnimationBlueprintLibrary::GetMetaData(Sequence, MetaDataObjects);
	TArray<TSharedPtr<FJsonValue>> MetaDataJson;
	for (UAnimMetaData* MetaDataObject : MetaDataObjects)
	{
		if (MetaDataObject)
		{
			MetaDataJson.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildMetadataJson(MetaDataObject)));
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("cleared_all"), bClearAll);
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetNumberField(TEXT("metadata_count"), MetaDataJson.Num());
	OutData->SetArrayField(TEXT("metadata"), MetaDataJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetNotify(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	double NotifyIndexValue = 0.0;
	const bool bHasNotifyIndex = JsonTryGetNumber(Ctx.Params, TEXT("notify_index"), NotifyIndexValue);
	const int32 NotifyIndex = bHasNotifyIndex ? static_cast<int32>(NotifyIndexValue) : INDEX_NONE;

	Sequence->Modify();

	if (bRemove)
	{
		if (NotifyIndex == INDEX_NONE)
		{
			OutError = TEXT("missing_notify_index");
			return false;
		}
		if (!Sequence->Notifies.IsValidIndex(NotifyIndex))
		{
			OutError = TEXT("notify_not_found");
			return false;
		}

		Sequence->Notifies.RemoveAt(NotifyIndex);
		Sequence->SortNotifies();
		Sequence->MarkPackageDirty();

		bool bSaveAfterSet = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
		if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}

		OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
		OutData->SetNumberField(TEXT("removed_notify_index"), NotifyIndex);
		OutData->SetNumberField(TEXT("notify_count"), Sequence->Notifies.Num());
		OutData->SetBoolField(TEXT("removed"), true);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
		return true;
	}

	bool bAdded = false;
	int32 UpdatedNotifyIndex = NotifyIndex;

	if (UpdatedNotifyIndex == INDEX_NONE)
	{
		FString TrackNameText = TEXT("1");
		JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackNameText);
		TrackNameText = TrackNameText.TrimStartAndEnd();
		if (TrackNameText.IsEmpty())
		{
			TrackNameText = TEXT("1");
		}

		int32 TrackIndex = INDEX_NONE;
		if (!UeAgentAnimationAssetOps::EnsureNotifyTrack(Sequence, FName(*TrackNameText), TrackIndex))
		{
			OutError = TEXT("ensure_notify_track_failed");
			return false;
		}

		double TimeSeconds = 0.0;
		if (!JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds) || TimeSeconds < 0.0 || TimeSeconds > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_time_seconds");
			return false;
		}

		FString NotifyStateClassPath;
		FString NotifyClassPath;
		JsonTryGetString(Ctx.Params, TEXT("notify_state_class"), NotifyStateClassPath);
		JsonTryGetString(Ctx.Params, TEXT("notify_class"), NotifyClassPath);
		NotifyStateClassPath = NotifyStateClassPath.TrimStartAndEnd();
		NotifyClassPath = NotifyClassPath.TrimStartAndEnd();

		if (!NotifyStateClassPath.IsEmpty())
		{
			UClass* NotifyStateClass = UeAgentAnimationAssetOps::LoadClassByPath(NotifyStateClassPath);
			if (!NotifyStateClass || !NotifyStateClass->IsChildOf(UAnimNotifyState::StaticClass()))
			{
				OutError = TEXT("invalid_notify_state_class");
				return false;
			}

			double DurationSeconds = 0.0;
			if (!JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds) || DurationSeconds <= 0.0)
			{
				OutError = TEXT("missing_duration_seconds");
				return false;
			}

			UAnimNotifyState* CreatedNotifyState = UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(Sequence, FName(*TrackNameText), static_cast<float>(TimeSeconds), static_cast<float>(DurationSeconds), NotifyStateClass);
			if (!CreatedNotifyState)
			{
				OutError = TEXT("add_notify_state_failed");
				return false;
			}

			for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
			{
				if (Sequence->Notifies[Index].NotifyStateClass == CreatedNotifyState)
				{
					UpdatedNotifyIndex = Index;
					break;
				}
			}
		}
		else
		{
			if (!NotifyClassPath.IsEmpty())
			{
				UClass* NotifyClass = UeAgentAnimationAssetOps::LoadClassByPath(NotifyClassPath);
				if (!NotifyClass || !NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
				{
					OutError = TEXT("invalid_notify_class");
					return false;
				}
				UAnimNotify* CreatedNotify = UAnimationBlueprintLibrary::AddAnimationNotifyEvent(Sequence, FName(*TrackNameText), static_cast<float>(TimeSeconds), NotifyClass);
				if (!CreatedNotify)
				{
					OutError = TEXT("add_notify_failed");
					return false;
				}

				for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
				{
					if (Sequence->Notifies[Index].Notify == CreatedNotify)
					{
						UpdatedNotifyIndex = Index;
						break;
					}
				}
			}
			else
			{
				FAnimNotifyEvent NewEvent;
				NewEvent.TrackIndex = TrackIndex;
				NewEvent.Link(Sequence, static_cast<float>(TimeSeconds));
				Sequence->Notifies.Add(NewEvent);
				UpdatedNotifyIndex = Sequence->Notifies.Num() - 1;
			}
		}

		if (!Sequence->Notifies.IsValidIndex(UpdatedNotifyIndex))
		{
			OutError = TEXT("resolve_created_notify_failed");
			return false;
		}
		bAdded = true;
	}
	else if (!Sequence->Notifies.IsValidIndex(UpdatedNotifyIndex))
	{
		OutError = TEXT("notify_not_found");
		return false;
	}

	FAnimNotifyEvent& Notify = Sequence->Notifies[UpdatedNotifyIndex];

	FString TrackNameText;
	if (JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackNameText) && !TrackNameText.TrimStartAndEnd().IsEmpty())
	{
		int32 TrackIndex = INDEX_NONE;
		if (!UeAgentAnimationAssetOps::EnsureNotifyTrack(Sequence, FName(*TrackNameText.TrimStartAndEnd()), TrackIndex))
		{
			OutError = TEXT("ensure_notify_track_failed");
			return false;
		}
		Notify.TrackIndex = TrackIndex;
	}

	double TimeSeconds = Notify.GetTime();
	if (JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSeconds))
	{
		if (TimeSeconds < 0.0 || TimeSeconds > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_time_seconds");
			return false;
		}
		Notify.SetTime(static_cast<float>(TimeSeconds));
	}

	if (Notify.NotifyStateClass != nullptr)
	{
		double DurationSeconds = Notify.GetDuration();
		if (JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds))
		{
			if (DurationSeconds <= 0.0)
			{
				OutError = TEXT("invalid_duration_seconds");
				return false;
			}
			Notify.SetDuration(static_cast<float>(DurationSeconds));
			Notify.EndLink.Link(Sequence, Notify.EndLink.GetTime());
		}
	}

	if (!UeAgentAnimationAssetOps::ApplyNotifyCommonSettings(Ctx.Params, Notify, OutError))
	{
		return false;
	}

	FString NotifyNameText;
	if (JsonTryGetString(Ctx.Params, TEXT("notify_name"), NotifyNameText) && !NotifyNameText.TrimStartAndEnd().IsEmpty())
	{
		Notify.NotifyName = FName(*NotifyNameText.TrimStartAndEnd());
	}

	FLinearColor NotifyColorLinear = FLinearColor(Notify.NotifyColor);
	const bool bHasNotifyColor = UeAgentAnimationAssetOps::TryParseLinearColorField(Ctx.Params, TEXT("notify_color"), NotifyColorLinear);
	if (bHasNotifyColor)
	{
		Notify.NotifyColor = NotifyColorLinear.ToFColor(true);
		if (Notify.Notify)
		{
			Notify.Notify->NotifyColor = Notify.NotifyColor;
		}
		if (Notify.NotifyStateClass)
		{
			Notify.NotifyStateClass->NotifyColor = Notify.NotifyColor;
		}
	}

	UAnimNotify* NotifyObject = Notify.Notify;
	UAnimNotifyState* NotifyStateObject = Notify.NotifyStateClass;
#if WITH_EDITORONLY_DATA
	const FGuid NotifyGuid = Notify.Guid;
#endif

	Sequence->SortNotifies();
	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	for (int32 Index = 0; Index < Sequence->Notifies.Num(); ++Index)
	{
		const FAnimNotifyEvent& Candidate = Sequence->Notifies[Index];
#if WITH_EDITORONLY_DATA
		if (NotifyGuid.IsValid() && Candidate.Guid == NotifyGuid)
		{
			UpdatedNotifyIndex = Index;
			break;
		}
#endif
		if (Candidate.Notify == NotifyObject && Candidate.NotifyStateClass == NotifyStateObject)
		{
			UpdatedNotifyIndex = Index;
			break;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("added"), bAdded);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetNumberField(TEXT("notify_count"), Sequence->Notifies.Num());
	OutData->SetObjectField(TEXT("notify"), UeAgentAnimationAssetOps::BuildNotifyJson(Sequence, Sequence->Notifies[UpdatedNotifyIndex], UpdatedNotifyIndex));
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetNotifyTrack(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString TrackNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackNameText) || TrackNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_track_name");
		return false;
	}
	const FName TrackName(*TrackNameText.TrimStartAndEnd());

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	FLinearColor TrackColor = FLinearColor::White;
	const bool bHasTrackColor = UeAgentAnimationAssetOps::TryParseLinearColorField(Ctx.Params, TEXT("track_color"), TrackColor);

	bool bTrackExists = false;
#if WITH_EDITORONLY_DATA
	int32 ExistingTrackIndex = INDEX_NONE;
	for (int32 TrackIndex = 0; TrackIndex < Sequence->AnimNotifyTracks.Num(); ++TrackIndex)
	{
		if (Sequence->AnimNotifyTracks[TrackIndex].TrackName == TrackName)
		{
			ExistingTrackIndex = TrackIndex;
			bTrackExists = true;
			break;
		}
	}
#endif

	Sequence->Modify();
	if (bRemove)
	{
		if (!bTrackExists)
		{
			OutError = TEXT("notify_track_not_found");
			return false;
		}
		UAnimationBlueprintLibrary::RemoveAnimationNotifyTrack(Sequence, TrackName);
	}
	else
	{
		if (!bTrackExists)
		{
			UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Sequence, TrackName, bHasTrackColor ? TrackColor : FLinearColor::White);
		}
#if WITH_EDITORONLY_DATA
		else if (bHasTrackColor)
		{
			Sequence->AnimNotifyTracks[ExistingTrackIndex].TrackColor = TrackColor;
			Sequence->MarkPackageDirty();
		}
#endif
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("track_name"), TrackName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetSyncMarkers(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	Sequence->Modify();

	bool bClearAll = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_all"), bClearAll);
	int32 AddedCount = 0;
	int32 RemovedCount = 0;

	if (bClearAll)
	{
		RemovedCount += Sequence->AuthoredSyncMarkers.Num();
		UAnimationBlueprintLibrary::RemoveAllAnimationSyncMarkers(Sequence);
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveMarkerNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_marker_names"), RemoveMarkerNames) && RemoveMarkerNames)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RemoveMarkerNames)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString MarkerName = Value->AsString().TrimStartAndEnd();
			if (MarkerName.IsEmpty())
			{
				continue;
			}
			RemovedCount += UAnimationBlueprintLibrary::RemoveAnimationSyncMarkersByName(Sequence, FName(*MarkerName));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveTrackNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_notify_track_names"), RemoveTrackNames) && RemoveTrackNames)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RemoveTrackNames)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString TrackName = Value->AsString().TrimStartAndEnd();
			if (TrackName.IsEmpty())
			{
				continue;
			}
			RemovedCount += UAnimationBlueprintLibrary::RemoveAnimationSyncMarkersByTrack(Sequence, FName(*TrackName));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* AddMarkers = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("add_markers"), AddMarkers) && AddMarkers)
	{
		for (const TSharedPtr<FJsonValue>& MarkerValue : *AddMarkers)
		{
			const TSharedPtr<FJsonObject>* MarkerObject = nullptr;
			if (!MarkerValue.IsValid() || !MarkerValue->TryGetObject(MarkerObject) || !MarkerObject || !MarkerObject->IsValid())
			{
				continue;
			}

			FString MarkerName;
			double TimeSeconds = 0.0;
			if (!(*MarkerObject)->TryGetStringField(TEXT("marker_name"), MarkerName) || MarkerName.TrimStartAndEnd().IsEmpty() || !(*MarkerObject)->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
			{
				OutError = TEXT("invalid_add_marker_entry");
				return false;
			}
			if (TimeSeconds < 0.0 || TimeSeconds > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("invalid_marker_time_seconds");
				return false;
			}

			FString TrackName = TEXT("1");
			(*MarkerObject)->TryGetStringField(TEXT("notify_track_name"), TrackName);
			TrackName = TrackName.TrimStartAndEnd();
			if (TrackName.IsEmpty())
			{
				TrackName = TEXT("1");
			}

			bool bTrackExists = false;
#if WITH_EDITORONLY_DATA
			for (const FAnimNotifyTrack& Track : Sequence->AnimNotifyTracks)
			{
				if (Track.TrackName.ToString().Equals(TrackName, ESearchCase::IgnoreCase))
				{
					bTrackExists = true;
					break;
				}
			}
#endif
			if (!bTrackExists)
			{
				UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Sequence, FName(*TrackName));
			}

			UAnimationBlueprintLibrary::AddAnimationSyncMarker(Sequence, FName(*MarkerName.TrimStartAndEnd()), static_cast<float>(TimeSeconds), FName(*TrackName));
			++AddedCount;
		}
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> SyncMarkers;
	for (int32 MarkerIndex = 0; MarkerIndex < Sequence->AuthoredSyncMarkers.Num(); ++MarkerIndex)
	{
		SyncMarkers.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSyncMarkerJson(Sequence, Sequence->AuthoredSyncMarkers[MarkerIndex], MarkerIndex)));
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetNumberField(TEXT("added_marker_count"), AddedCount);
	OutData->SetNumberField(TEXT("removed_marker_count"), RemovedCount);
	OutData->SetNumberField(TEXT("sync_marker_count"), SyncMarkers.Num());
	OutData->SetArrayField(TEXT("sync_markers"), SyncMarkers);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}