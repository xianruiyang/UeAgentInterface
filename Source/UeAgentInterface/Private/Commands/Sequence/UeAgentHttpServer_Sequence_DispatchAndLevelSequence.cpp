// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Sequence.h"
#include "UeAgentHttpServer_SequenceOps.h"

#include "Commands/Assets/UeAgentReflectionAuthoringCommandBridge.h"
#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/ActorComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/Widget.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Camera/CameraComponent.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "MovieSceneSequencePlayer.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneDoubleSection.h"
#include "Sections/MovieSceneObjectPropertySection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneIntegerSection.h"
#include "Sections/MovieSceneActorReferenceSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Sections/MovieSceneStringSection.h"
#include "Sections/MovieSceneSpawnSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Sections/MovieSceneRotatorSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneVisibilitySection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneIntegerTrack.h"
#include "Tracks/MovieSceneActorReferenceTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneStringTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "Tracks/MovieSceneRotatorTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "UeAgentInterfaceSettings.h"
#include "WidgetBlueprint.h"

bool FUeAgentLegacyMechanics::ExecuteSequenceCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}
	else
	{
		OutData->Values.Reset();
	}

	if (CommandLower == TEXT("sequence_list_level_sequences")) return CmdSequenceListLevelSequences(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_create_level_sequence")) return CmdSequenceCreateLevelSequence(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_open_level_sequence")) return CmdSequenceOpenLevelSequence(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_get_level_sequence_info")) return CmdSequenceGetLevelSequenceInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_screenshot")) return CmdSequenceScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_export_folder")) return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("sequence"), Ctx.Params, OutData, OutError);
	if (CommandLower == TEXT("sequence_apply_folder")) return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("sequence"), Ctx.Params, OutData, OutError, Ctx.RequestId);
	if (CommandLower == TEXT("sequence_set_level_sequence_playback_range")) return CmdSequenceSetLevelSequencePlaybackRange(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_set_level_sequence_display_rate")) return CmdSequenceSetLevelSequenceDisplayRate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_actor_binding")) return CmdSequenceAddActorBinding(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_remove_actor_binding")) return CmdSequenceRemoveActorBinding(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_property_track")) return CmdSequenceAddPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_property_key")) return CmdSequenceAddPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_visibility_track")) return CmdSequenceAddVisibilityTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_visibility_key")) return CmdSequenceAddVisibilityKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_bool_property_track")) return CmdSequenceAddBoolPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_bool_property_key")) return CmdSequenceAddBoolPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_float_property_track")) return CmdSequenceAddFloatPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_float_property_key")) return CmdSequenceAddFloatPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_integer_property_track")) return CmdSequenceAddIntegerPropertyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_integer_property_key")) return CmdSequenceAddIntegerPropertyKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_transform_track")) return CmdSequenceAddTransformTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_transform_key")) return CmdSequenceAddTransformKey(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_skeletal_animation_track")) return CmdSequenceAddSkeletalAnimationTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_add_skeletal_animation_section")) return CmdSequenceAddSkeletalAnimationSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_update_skeletal_animation_section")) return CmdSequenceUpdateSkeletalAnimationSection(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_remove_skeletal_animation_section")) return CmdSequenceRemoveSkeletalAnimationSection(Ctx, OutData, OutError);

	if (CommandLower == TEXT("sequence_list_umg_animations")) return CmdSequenceListUmgAnimations(Ctx, OutData, OutError);
	if (CommandLower == TEXT("sequence_get_umg_animation_info")) return CmdSequenceGetUmgAnimationInfo(Ctx, OutData, OutError);

	OutError = TEXT("unknown_sequence_command");
	return false;
}

bool FUeAgentLegacyMechanics::CmdSequenceListLevelSequences(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RootPath = TEXT("/Game");
	JsonTryGetString(Ctx.Params, TEXT("root_path"), RootPath);
	RootPath.TrimStartAndEndInline();
	if (RootPath.IsEmpty())
	{
		RootPath = TEXT("/Game");
	}
	if (!RootPath.StartsWith(TEXT("/")))
	{
		RootPath = TEXT("/") + RootPath;
	}
	if (!FPackageName::IsValidPath(RootPath))
	{
		OutError = TEXT("invalid_root_path");
		return false;
	}

	int32 Limit = 200;
	double LimitNumber = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 1, 5000);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(*RootPath);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(FMath::Min(AssetList.Num(), Limit));
	for (const FAssetData& AssetData : AssetList)
	{
		if (Items.Num() >= Limit)
		{
			break;
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Item->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Item->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Item->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutData->SetStringField(TEXT("root_path"), RootPath);
	OutData->SetNumberField(TEXT("total_count"), AssetList.Num());
	OutData->SetNumberField(TEXT("returned_count"), Items.Num());
	OutData->SetArrayField(TEXT("items"), Items);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceCreateLevelSequence(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString PackageName = UeAgentSequenceOps::NormalizeAssetPath(AssetPathInput);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	if (AssetName.IsEmpty())
	{
		OutError = TEXT("invalid_asset_name");
		return false;
	}

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	bool bAllowEditorOpen = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_editor_open"), bAllowEditorOpen);
	if (bOpenEditor && !bAllowEditorOpen)
	{
		OutError = TEXT("open_editor_requires_allow_editor_open");
		return false;
	}

	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	if (UeAgentSequenceOps::AssetExists(ObjectPath))
	{
		OutError = TEXT("asset_already_exists");
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = TEXT("create_package_failed");
		return false;
	}

	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Sequence)
	{
		OutError = TEXT("create_level_sequence_failed");
		return false;
	}
	Sequence->Initialize();
	if (!Sequence->GetMovieScene())
	{
		OutError = TEXT("create_movie_scene_failed");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 5.0;
	JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds);
	JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds);
	if (DurationSeconds <= 0.0)
	{
		DurationSeconds = 5.0;
	}
	UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(Sequence->GetMovieScene(), StartSeconds, DurationSeconds);

	double DisplayRateNum = 30.0;
	double DisplayRateDen = 1.0;
	JsonTryGetNumber(Ctx.Params, TEXT("display_rate_num"), DisplayRateNum);
	JsonTryGetNumber(Ctx.Params, TEXT("display_rate_den"), DisplayRateDen);
	const int32 DisplayRateNumInt = FMath::Max(1, FMath::RoundToInt(DisplayRateNum));
	const int32 DisplayRateDenInt = FMath::Max(1, FMath::RoundToInt(DisplayRateDen));
	Sequence->GetMovieScene()->SetDisplayRate(FFrameRate(DisplayRateNumInt, DisplayRateDenInt));

	FAssetRegistryModule::AssetCreated(Sequence);
	Sequence->MarkPackageDirty();
	Package->MarkPackageDirty();

	if (bOpenEditor && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->OpenEditorForAsset(Sequence);
		}
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), PackageName);
	OutData->SetStringField(TEXT("asset_name"), AssetName);
	OutData->SetStringField(TEXT("object_path"), ObjectPath);
	OutData->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRateNumInt, DisplayRateDenInt));
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("opened_editor"), bOpenEditor);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceOpenLevelSequence(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bAllowEditorOpen = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_editor_open"), bAllowEditorOpen);
	if (!bAllowEditorOpen)
	{
		OutError = TEXT("sequence_open_level_sequence_requires_allow_editor_open");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("editor_not_available");
		return false;
	}
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("asset_editor_subsystem_not_available");
		return false;
	}

	AssetEditorSubsystem->OpenEditorForAsset(Sequence);
	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceGetLevelSequenceInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("display_rate"), MovieScene->GetDisplayRate().ToPrettyText().ToString());
	OutData->SetStringField(TEXT("tick_resolution"), MovieScene->GetTickResolution().ToPrettyText().ToString());
	OutData->SetNumberField(TEXT("playback_start_frame"), MovieScene->GetPlaybackRange().GetLowerBoundValue().Value);
	OutData->SetNumberField(TEXT("playback_end_exclusive_frame"), MovieScene->GetPlaybackRange().GetUpperBoundValue().Value);
	OutData->SetNumberField(TEXT("master_track_count"), MovieScene->GetTracks().Num());
	OutData->SetNumberField(TEXT("binding_count"), MovieScene->GetBindings().Num());

	TArray<TSharedPtr<FJsonValue>> MasterTracksJson;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (Track)
		{
			MasterTracksJson.Add(MakeShared<FJsonValueObject>(UeAgentSequenceOps::BuildMovieSceneTrackSummary(MovieScene, Track)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> BindingsJson;
	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		BindingsJson.Add(MakeShared<FJsonValueObject>(UeAgentSequenceOps::BuildMovieSceneBindingSummary(MovieScene, Binding)));
	}
	OutData->SetArrayField(TEXT("master_tracks"), MasterTracksJson);
	OutData->SetArrayField(TEXT("bindings"), BindingsJson);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		return false;
	}

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::LevelSequence,
		UnifiedOptionsReport);

	TArray<UeAgentSequenceOps::FLevelSequenceFrameRequest> FrameRequests;
	if (!UeAgentSequenceOps::ResolveLevelSequenceFrameSequence(Sequence, Ctx.Params, FrameRequests, OutError))
	{
		return false;
	}
	if (FrameRequests.Num() <= 0)
	{
		OutError = TEXT("preview_frame_sequence_empty");
		return false;
	}

	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return false;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("no_editor_world");
		return false;
	}

	FString Target = TEXT("viewport");
	JsonTryGetString(Ctx.Params, TEXT("target"), Target);
	Target = Target.TrimStartAndEnd().ToLower();
	if (Target != TEXT("viewport") && Target != TEXT("camera") && Target != TEXT("sequence_camera"))
	{
		OutError = TEXT("invalid_target");
		return false;
	}

	FString Format = TEXT("png");
	int32 Quality = 95;
	int32 MaxSize = 1920;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	double QualityValue = Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
	{
		Quality = FMath::Clamp(static_cast<int32>(QualityValue), 1, 100);
	}

	double MaxSizeValue = MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeValue))
	{
		MaxSize = FMath::Clamp(static_cast<int32>(MaxSizeValue), 64, 8192);
	}

	FIntPoint BaseCaptureSize(1920, 1080);
	double WidthValue = 0.0;
	double HeightValue = 0.0;
	const bool bHasExplicitWidth = JsonTryGetNumber(Ctx.Params, TEXT("width"), WidthValue);
	const bool bHasExplicitHeight = JsonTryGetNumber(Ctx.Params, TEXT("height"), HeightValue);
	if (bHasExplicitWidth && bHasExplicitHeight && WidthValue > 0.0 && HeightValue > 0.0)
	{
		BaseCaptureSize = FIntPoint(FMath::Clamp(FMath::RoundToInt(WidthValue), 64, 8192), FMath::Clamp(FMath::RoundToInt(HeightValue), 64, 8192));
	}
	else if (FLevelEditorViewportClient* ViewportClient = UeAgentSequenceOps::GetPreferredLevelViewportClient())
	{
		if (ViewportClient->Viewport)
		{
			const FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();
			if (ViewportSize.X > 0 && ViewportSize.Y > 0)
			{
				BaseCaptureSize = ViewportSize;
			}
		}
	}
	const int32 MaxDim = FMath::Max(BaseCaptureSize.X, BaseCaptureSize.Y);
	const float CaptureScale = (MaxDim > MaxSize) ? (static_cast<float>(MaxSize) / static_cast<float>(MaxDim)) : 1.0f;
	const int32 CaptureWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(BaseCaptureSize.X) * CaptureScale));
	const int32 CaptureHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(BaseCaptureSize.Y) * CaptureScale));

	bool bFallbackToViewportCamera = true;
	JsonTryGetBool(Ctx.Params, TEXT("fallback_to_viewport_camera"), bFallbackToViewportCamera);

	struct FSequenceScreenshotCleanup
	{
		UWorld* World = nullptr;
		UPackage* WorldPackage = nullptr;
		bool bWorldPackageWasDirty = false;
		ULevelSequencePlayer* Player = nullptr;
		ALevelSequenceActor* SequenceActor = nullptr;
		ASceneCapture2D* CaptureActor = nullptr;
		UTextureRenderTarget2D* RenderTarget = nullptr;

		~FSequenceScreenshotCleanup()
		{
			if (Player)
			{
				Player->Stop();
			}
			if (CaptureActor && IsValid(CaptureActor))
			{
				CaptureActor->Destroy();
			}
			if (World && SequenceActor && IsValid(SequenceActor))
			{
				World->DestroyActor(SequenceActor, false, false);
			}
			if (WorldPackage && !bWorldPackageWasDirty && WorldPackage->IsDirty())
			{
				WorldPackage->SetDirtyFlag(false);
			}
		}
	};
	FSequenceScreenshotCleanup Cleanup;
	Cleanup.World = World;
	Cleanup.WorldPackage = World->GetOutermost();
	Cleanup.bWorldPackageWasDirty = Cleanup.WorldPackage ? Cleanup.WorldPackage->IsDirty() : false;

	FMovieSceneSequencePlaybackSettings PlaybackSettings;
	PlaybackSettings.bAutoPlay = false;
	PlaybackSettings.FinishCompletionStateOverride = EMovieSceneCompletionModeOverride::ForceRestoreState;
	Cleanup.Player = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, PlaybackSettings, Cleanup.SequenceActor);
	if (!Cleanup.Player)
	{
		OutError = TEXT("level_sequence_player_create_failed");
		return false;
	}
	if (Cleanup.SequenceActor)
	{
		Cleanup.SequenceActor->SetFlags(RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		Cleanup.SequenceActor->SetActorEnableCollision(false);
		Cleanup.SequenceActor->SetActorHiddenInGame(true);
		Cleanup.SequenceActor->SetIsTemporarilyHiddenInEditor(true);
	}

	Cleanup.RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!Cleanup.RenderTarget)
	{
		OutError = TEXT("render_target_alloc_failed");
		return false;
	}
	Cleanup.RenderTarget->ClearColor = FLinearColor::Black;
	Cleanup.RenderTarget->InitCustomFormat(CaptureWidth, CaptureHeight, PF_B8G8R8A8, false);
	Cleanup.RenderTarget->UpdateResourceImmediate(true);

	FActorSpawnParameters CaptureSpawnParams;
	CaptureSpawnParams.Name = MakeUniqueObjectName(World, ASceneCapture2D::StaticClass(), TEXT("UeAgentSequenceFrameCapture"));
	CaptureSpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
	CaptureSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CaptureSpawnParams.bTemporaryEditorActor = true;
	CaptureSpawnParams.bHideFromSceneOutliner = true;

	Cleanup.CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), FTransform::Identity, CaptureSpawnParams);
	if (!Cleanup.CaptureActor)
	{
		OutError = TEXT("spawn_scene_capture_failed");
		return false;
	}
	Cleanup.CaptureActor->SetActorEnableCollision(false);
	Cleanup.CaptureActor->SetActorHiddenInGame(true);
	Cleanup.CaptureActor->SetIsTemporarilyHiddenInEditor(true);

	USceneCaptureComponent2D* CaptureComp = Cleanup.CaptureActor->GetCaptureComponent2D();
	if (!CaptureComp)
	{
		OutError = TEXT("scene_capture_component_missing");
		return false;
	}
	CaptureComp->TextureTarget = Cleanup.RenderTarget;
	CaptureComp->CaptureSource = SCS_FinalColorLDR;
	CaptureComp->FOVAngle = 60.0f;
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->ShowFlags.SetMotionBlur(false);

	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const float TickDeltaSeconds = DisplayRate.IsValid() && DisplayRate.AsDecimal() > KINDA_SMALL_NUMBER
		? static_cast<float>(1.0 / DisplayRate.AsDecimal())
		: (1.0f / 30.0f);

	TArray<TSharedPtr<FJsonValue>> CapturedFrames;
	TSharedPtr<FJsonObject> FirstFrameObject;
	TArray<TArray<FColor>> CapturedFramePixelsForSheet;
	TArray<FString> CapturedFrameLabelsForSheet;
	FIntPoint FrameSheetTileSize(0, 0);
	FString FirstCameraSource;
	int32 SequenceCameraFrameCount = 0;
	int32 ViewportFallbackFrameCount = 0;

	bool bCreateFrameSheet = FrameRequests.Num() > 1;
	JsonTryGetBool(Ctx.Params, TEXT("create_frame_sheet"), bCreateFrameSheet) ||
		JsonTryGetBool(Ctx.Params, TEXT("frame_sheet"), bCreateFrameSheet) ||
		JsonTryGetBool(Ctx.Params, TEXT("sequence_frame_image"), bCreateFrameSheet);
	bool bWriteIndividualFrames = FrameRequests.Num() == 1;
	JsonTryGetBool(Ctx.Params, TEXT("write_individual_frames"), bWriteIndividualFrames) ||
		JsonTryGetBool(Ctx.Params, TEXT("keep_individual_frames"), bWriteIndividualFrames) ||
		JsonTryGetBool(Ctx.Params, TEXT("output_individual_frames"), bWriteIndividualFrames);
	if (FrameRequests.Num() > 1 && !bCreateFrameSheet && !bWriteIndividualFrames)
	{
		OutError = TEXT("multi_frame_output_disabled");
		return false;
	}

	auto CaptureRequestedFrame = [&](const UeAgentSequenceOps::FLevelSequenceFrameRequest& Request, const int32 SequenceIndex, TSharedPtr<FJsonObject>& OutFrameObject) -> bool
	{
		const FFrameTime DisplayFrame = ConvertFrameTime(
			FFrameTime(Request.EvaluationFrame),
			MovieScene->GetTickResolution(),
			MovieScene->GetDisplayRate());
		Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(DisplayFrame, EUpdatePositionMethod::Jump));
		for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
		{
			if (AActor* Actor = *ActorIt)
			{
				TArray<USkeletalMeshComponent*> SkeletalComponents;
				Actor->GetComponents<USkeletalMeshComponent>(SkeletalComponents);
				for (USkeletalMeshComponent* SkeletalComponent : SkeletalComponents)
				{
					if (SkeletalComponent)
					{
						SkeletalComponent->RefreshBoneTransforms();
						SkeletalComponent->UpdateComponentToWorld();
						SkeletalComponent->MarkRenderDynamicDataDirty();
					}
				}
			}
		}
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();

		FString CameraSource = TEXT("sequence_camera");
		FString CameraObjectPath;
		FVector CaptureLocation = FVector::ZeroVector;
		FRotator CaptureRotation = FRotator::ZeroRotator;
		float CaptureFov = 60.0f;

		if (UCameraComponent* ActiveCamera = Cleanup.Player->GetActiveCameraComponent())
		{
			CaptureLocation = ActiveCamera->GetComponentLocation();
			CaptureRotation = ActiveCamera->GetComponentRotation();
			CaptureFov = ActiveCamera->FieldOfView;
			CameraObjectPath = ActiveCamera->GetPathName();
			++SequenceCameraFrameCount;
		}
		else if (bFallbackToViewportCamera)
		{
			FLevelEditorViewportClient* ViewportClient = UeAgentSequenceOps::GetPreferredLevelViewportClient();
			if (!ViewportClient)
			{
				OutError = TEXT("sequence_camera_not_found_and_no_viewport_fallback");
				return false;
			}
			CameraSource = TEXT("viewport_fallback");
			CaptureLocation = ViewportClient->GetViewLocation();
			CaptureRotation = ViewportClient->GetViewRotation();
			CaptureFov = ViewportClient->ViewFOV;
			++ViewportFallbackFrameCount;
		}
		else
		{
			OutError = TEXT("sequence_camera_not_found");
			return false;
		}

		Cleanup.CaptureActor->SetActorLocationAndRotation(CaptureLocation, CaptureRotation, false, nullptr, ETeleportType::TeleportPhysics);
		CaptureComp->FOVAngle = FMath::Clamp(CaptureFov, 5.0f, 170.0f);
		CaptureComp->CaptureScene();
		FlushRenderingCommands();

		FRenderTarget* RenderTarget = Cleanup.RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTarget)
		{
			OutError = TEXT("render_target_resource_missing");
			return false;
		}

		TArray<FColor> Pixels;
		const FIntPoint ShotSize(CaptureWidth, CaptureHeight);
		if (!RenderTarget->ReadPixels(Pixels))
		{
			OutError = TEXT("read_render_target_failed");
			return false;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}

		TArray<FColor> ResizedPixels;
		FIntPoint ResizedSize(0, 0);
		if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
		{
			return false;
		}

		FString OutPath;
		int64 Bytes = 0;
		if (bWriteIndividualFrames)
		{
			OutPath = MakeShotPath(Format);
			if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
			{
				return false;
			}
			Bytes = IFileManager::Get().FileSize(*OutPath);
		}

		if (FrameSheetTileSize.X <= 0 || FrameSheetTileSize.Y <= 0)
		{
			FrameSheetTileSize = ResizedSize;
		}
		if (ResizedSize == FrameSheetTileSize)
		{
			CapturedFramePixelsForSheet.Add(ResizedPixels);
			CapturedFrameLabelsForSheet.Add(FString::Printf(TEXT("F%d"), Request.FrameIndex));
		}

		OutFrameObject = MakeShared<FJsonObject>();
		OutFrameObject->SetNumberField(TEXT("sequence_index"), SequenceIndex);
		if (!OutPath.IsEmpty())
		{
			OutFrameObject->SetStringField(TEXT("file_path"), OutPath);
			OutFrameObject->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		}
		OutFrameObject->SetNumberField(TEXT("width"), ResizedSize.X);
		OutFrameObject->SetNumberField(TEXT("height"), ResizedSize.Y);
		OutFrameObject->SetNumberField(TEXT("frame_index"), Request.FrameIndex);
		OutFrameObject->SetStringField(TEXT("frame_label"), FString::Printf(TEXT("F%d"), Request.FrameIndex));
		OutFrameObject->SetNumberField(TEXT("time_seconds"), Request.TimeSeconds);
		OutFrameObject->SetNumberField(TEXT("evaluation_frame"), Request.EvaluationFrame.Value);
		OutFrameObject->SetStringField(TEXT("camera_source"), CameraSource);
		OutFrameObject->SetStringField(TEXT("camera_object_path"), CameraObjectPath);
		OutFrameObject->SetNumberField(TEXT("camera_fov"), CaptureFov);
		if (SequenceIndex == 0)
		{
			FirstCameraSource = CameraSource;
		}
		return true;
	};

	for (int32 RequestIndex = 0; RequestIndex < FrameRequests.Num(); ++RequestIndex)
	{
		TSharedPtr<FJsonObject> FrameObject;
		if (!CaptureRequestedFrame(FrameRequests[RequestIndex], RequestIndex, FrameObject))
		{
			return false;
		}
		if (RequestIndex == 0)
		{
			FirstFrameObject = FrameObject;
		}
		CapturedFrames.Add(MakeShared<FJsonValueObject>(FrameObject));
	}

	if (!FirstFrameObject.IsValid())
	{
		OutError = TEXT("preview_frame_capture_empty");
		return false;
	}

	FString FrameSheetPath;
	TSharedPtr<FJsonObject> FrameSheetData;
	if (bCreateFrameSheet && FrameRequests.Num() > 1)
	{
		if (CapturedFramePixelsForSheet.Num() != FrameRequests.Num())
		{
			OutError = TEXT("frame_sheet_capture_size_mismatch");
			return false;
		}
		if (!WriteFrameSequenceSheetImage(CapturedFramePixelsForSheet, CapturedFrameLabelsForSheet, FrameSheetTileSize, Ctx.Params, Format, Quality, FrameSheetPath, FrameSheetData, OutError))
		{
			return false;
		}
	}

	Cleanup.Player->Stop();
	if (Cleanup.WorldPackage && !Cleanup.bWorldPackageWasDirty && Cleanup.WorldPackage->IsDirty())
	{
		Cleanup.WorldPackage->SetDirtyFlag(false);
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	if (FrameSheetData.IsValid())
	{
		OutData->SetStringField(TEXT("file_path"), FrameSheetPath);
		OutData->SetNumberField(TEXT("width"), FrameSheetData->GetNumberField(TEXT("width")));
		OutData->SetNumberField(TEXT("height"), FrameSheetData->GetNumberField(TEXT("height")));
		OutData->SetNumberField(TEXT("bytes"), FrameSheetData->GetNumberField(TEXT("bytes")));
	}
	else
	{
		FString FirstFramePath;
		FirstFrameObject->TryGetStringField(TEXT("file_path"), FirstFramePath);
		OutData->SetStringField(TEXT("file_path"), FirstFramePath);
		OutData->SetNumberField(TEXT("width"), FirstFrameObject->GetNumberField(TEXT("width")));
		OutData->SetNumberField(TEXT("height"), FirstFrameObject->GetNumberField(TEXT("height")));
		double FirstFrameBytes = 0.0;
		FirstFrameObject->TryGetNumberField(TEXT("bytes"), FirstFrameBytes);
		OutData->SetNumberField(TEXT("bytes"), FirstFrameBytes);
	}
	OutData->SetStringField(TEXT("target"), TEXT("viewport"));
	OutData->SetStringField(TEXT("capture_mode"), TEXT("level_sequence_player_scene_capture"));
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("frame_index"), FirstFrameObject->GetNumberField(TEXT("frame_index")));
	OutData->SetNumberField(TEXT("time_seconds"), FirstFrameObject->GetNumberField(TEXT("time_seconds")));
	OutData->SetNumberField(TEXT("frame_count"), FrameRequests.Num());
	OutData->SetArrayField(TEXT("frames"), CapturedFrames);
	OutData->SetBoolField(TEXT("frame_sheet_created"), FrameSheetData.IsValid());
	OutData->SetBoolField(TEXT("individual_frame_files_created"), bWriteIndividualFrames);
	if (FrameSheetData.IsValid())
	{
		OutData->SetStringField(TEXT("frame_sheet_file_path"), FrameSheetPath);
		OutData->SetStringField(TEXT("sequence_frame_image_path"), FrameSheetPath);
		OutData->SetObjectField(TEXT("frame_sheet"), FrameSheetData);
	}
	OutData->SetNumberField(TEXT("display_rate_numerator"), DisplayRate.Numerator);
	OutData->SetNumberField(TEXT("display_rate_denominator"), DisplayRate.Denominator);
	OutData->SetNumberField(TEXT("tick_resolution_numerator"), MovieScene->GetTickResolution().Numerator);
	OutData->SetNumberField(TEXT("tick_resolution_denominator"), MovieScene->GetTickResolution().Denominator);
	OutData->SetStringField(TEXT("camera_source"), FirstCameraSource);
	OutData->SetNumberField(TEXT("sequence_camera_frame_count"), SequenceCameraFrameCount);
	OutData->SetNumberField(TEXT("viewport_fallback_frame_count"), ViewportFallbackFrameCount);
	OutData->SetBoolField(TEXT("fallback_to_viewport_camera"), bFallbackToViewportCamera);
	OutData->SetBoolField(TEXT("world_package_was_dirty_before_capture"), Cleanup.bWorldPackageWasDirty);
	OutData->SetBoolField(TEXT("restored_world_dirty_state"), Cleanup.WorldPackage && !Cleanup.bWorldPackageWasDirty && !Cleanup.WorldPackage->IsDirty());
	UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceSetLevelSequencePlaybackRange(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double StartSeconds = 0.0;
	double DurationSeconds = 5.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("start_seconds"), StartSeconds))
	{
		OutError = TEXT("missing_start_seconds");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("duration_seconds"), DurationSeconds))
	{
		OutError = TEXT("missing_duration_seconds");
		return false;
	}
	if (DurationSeconds <= 0.0)
	{
		OutError = TEXT("invalid_duration_seconds");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	MovieScene->Modify();
	Sequence->Modify();
	if (!UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(MovieScene, StartSeconds, DurationSeconds))
	{
		OutError = TEXT("set_playback_range_failed");
		return false;
	}
	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetNumberField(TEXT("start_seconds"), StartSeconds);
	OutData->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentLegacyMechanics::CmdSequenceSetLevelSequenceDisplayRate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	double DisplayRateNumValue = 0.0;
	double DisplayRateDenValue = 0.0;
	if (!JsonTryGetNumber(Ctx.Params, TEXT("display_rate_num"), DisplayRateNumValue))
	{
		OutError = TEXT("missing_display_rate_num");
		return false;
	}
	if (!JsonTryGetNumber(Ctx.Params, TEXT("display_rate_den"), DisplayRateDenValue))
	{
		OutError = TEXT("missing_display_rate_den");
		return false;
	}
	const int32 DisplayRateNum = FMath::RoundToInt(DisplayRateNumValue);
	const int32 DisplayRateDen = FMath::RoundToInt(DisplayRateDenValue);
	if (DisplayRateNum <= 0 || DisplayRateDen <= 0)
	{
		OutError = TEXT("invalid_display_rate");
		return false;
	}

	ULevelSequence* Sequence = UeAgentSequenceOps::LoadLevelSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("level_sequence_not_found");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("movie_scene_not_found");
		return false;
	}

	MovieScene->Modify();
	Sequence->Modify();
	MovieScene->SetDisplayRate(FFrameRate(DisplayRateNum, DisplayRateDen));
	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet)
	{
		if (!UeAgentSequenceOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRateNum, DisplayRateDen));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
