#include "DomainAdapters/CaptureMediaLongJob/UeAgentSequenceCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/EditorCapture/UeAgentAsyncCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentSequenceCaptureOps.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureSequenceFrameOptions.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "Editor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHITypes.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		bool TryGetString(const TSharedPtr<FJsonObject>& Object, const FString& Key, FString& OutValue)
		{
			return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
		}

		bool TryGetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Key, double& OutValue)
		{
			return Object.IsValid() && Object->TryGetNumberField(Key, OutValue);
		}

		bool TryGetBool(const TSharedPtr<FJsonObject>& Object, const FString& Key, bool& OutValue)
		{
			return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
		}
	}

	bool FSequenceCapture::Capture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace CaptureDiagnostics;
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}

		FString AssetPath;
		if (!TryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			const auto* Value = Ctx.Params.IsValid() ? Ctx.Params->Values.Find(TEXT("asset_path")) : nullptr;
			OutError = Input(TEXT("missing_asset_path"), TEXT("Sequence capture requires a non-empty asset_path."),
				TEXT("sequence_capture"), TEXT("/asset_path"), Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty level sequence asset path")));
			return false;
		}

		ULevelSequence* Sequence = LoadLevelSequence(AssetPath);
		if (!Sequence)
		{
			OutError = Input(TEXT("level_sequence_not_found"), TEXT("The requested level sequence asset could not be loaded."),
				TEXT("sequence_capture"), TEXT("/asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("loadable LevelSequence asset")));
			OutError.TargetPath = AssetPath;
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = Native(TEXT("movie_scene_not_found"), TEXT("The level sequence has no movie scene."),
				TEXT("sequence_capture"), TEXT("movie_scene_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}

		UeAgentCaptureSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
		UeAgentCaptureSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
			Ctx.Params, UeAgentCaptureSequenceFrameOptions::ESequenceFrameCommandKind::LevelSequence, UnifiedOptionsReport);
		ON_SCOPE_EXIT { UnifiedOptionsReport.LocateDiagnosticSource(OutError); };

		TArray<FLevelSequenceFrameRequest> FrameRequests;
		if (!ResolveLevelSequenceFrameSequence(Sequence, Ctx.Params, FrameRequests, OutError))
		{
			return false;
		}
		if (FrameRequests.Num() <= 0)
		{
			OutError = Native(TEXT("preview_frame_sequence_empty"), TEXT("The sequence frame resolver returned no frame requests."),
				TEXT("sequence_capture"), TEXT("frame_request_count"), Number(FrameRequests.Num()), MakeShared<FJsonValueString>(TEXT("at least one frame")));
			OutError.TargetPath = AssetPath;
			return false;
		}

		if (!GEditor)
		{
			OutError = Native(TEXT("no_geditor"), TEXT("Sequence capture requires an active Editor."),
				TEXT("sequence_capture"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = Native(TEXT("no_editor_world"), TEXT("Sequence capture requires an Editor world."),
				TEXT("sequence_capture"), TEXT("editor_world_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}

		FString Target = TEXT("viewport");
		TryGetString(Ctx.Params, TEXT("target"), Target);
		Target = Target.TrimStartAndEnd().ToLower();
		if (Target != TEXT("viewport") && Target != TEXT("camera") && Target != TEXT("sequence_camera"))
		{
			const auto* Value = Ctx.Params->Values.Find(TEXT("target"));
			OutError = Input(TEXT("invalid_target"), TEXT("The sequence capture target is unsupported."),
				TEXT("sequence_capture"), TEXT("/target"), Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("viewport, camera or sequence_camera")));
			return false;
		}

		FString Format = TEXT("png");
		int32 Quality = 95;
		int32 MaxSize = 0;
		TryGetString(Ctx.Params, TEXT("format"), Format);
		Format = Format.ToLower();
		if (Format == TEXT("jpeg"))
		{
			Format = TEXT("jpg");
		}
		if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
		{
			const auto* Value = Ctx.Params->Values.Find(TEXT("format"));
			OutError = Input(TEXT("unsupported_format"), TEXT("The sequence capture image format is unsupported."),
				TEXT("sequence_capture"), TEXT("/format"), Value ? *Value : nullptr, MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg or webp")));
			return false;
		}

		double QualityValue = Quality;
		if (TryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
		{
			Quality = FMath::Clamp(static_cast<int32>(QualityValue), 1, 100);
		}

		double MaxSizeValue = MaxSize;
		if (TryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeValue))
		{
			MaxSize = FMath::Max(static_cast<int32>(MaxSizeValue), 1);
		}

		FIntPoint BaseCaptureSize(1920, 1080);
		double WidthValue = 0.0;
		double HeightValue = 0.0;
		const bool bHasExplicitWidth = TryGetNumber(Ctx.Params, TEXT("width"), WidthValue);
		const bool bHasExplicitHeight = TryGetNumber(Ctx.Params, TEXT("height"), HeightValue);
		if (bHasExplicitWidth && bHasExplicitHeight && WidthValue > 0.0 && HeightValue > 0.0)
		{
			BaseCaptureSize = FIntPoint(FMath::Max(FMath::RoundToInt(WidthValue), 1), FMath::Max(FMath::RoundToInt(HeightValue), 1));
		}
		else if (FLevelEditorViewportClient* ViewportClient = GetPreferredLevelViewportClient())
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
		const float CaptureScale = (MaxSize > 0 && MaxDim > MaxSize)
			? (static_cast<float>(MaxSize) / static_cast<float>(MaxDim))
			: 1.0f;
		const int32 CaptureWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(BaseCaptureSize.X) * CaptureScale));
		const int32 CaptureHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(BaseCaptureSize.Y) * CaptureScale));

		bool bFallbackToViewportCamera = true;
		TryGetBool(Ctx.Params, TEXT("fallback_to_viewport_camera"), bFallbackToViewportCamera);

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
			OutError = Native(TEXT("level_sequence_player_create_failed"), TEXT("The Editor could not create a player for the level sequence."),
				TEXT("sequence_capture"), TEXT("sequence_player_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
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
			OutError = Native(TEXT("render_target_alloc_failed"), TEXT("A render target could not be allocated for sequence capture."),
				TEXT("sequence_capture"), TEXT("render_target_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
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
			OutError = Native(TEXT("spawn_scene_capture_failed"), TEXT("The Editor world could not spawn the sequence capture actor."),
				TEXT("sequence_capture"), TEXT("capture_actor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}
		Cleanup.CaptureActor->SetActorEnableCollision(false);
		Cleanup.CaptureActor->SetActorHiddenInGame(true);
		Cleanup.CaptureActor->SetIsTemporarilyHiddenInEditor(true);

		USceneCaptureComponent2D* CaptureComp = Cleanup.CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = Native(TEXT("scene_capture_component_missing"), TEXT("The sequence capture actor has no capture component."),
				TEXT("sequence_capture"), TEXT("capture_component_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}
		CaptureComp->TextureTarget = Cleanup.RenderTarget;
		CaptureComp->ShowFlags.SetMotionBlur(false);

		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();

		TArray<TSharedPtr<FJsonValue>> CapturedFrames;
		TSharedPtr<FJsonObject> FirstFrameObject;
		TArray<TArray<FColor>> CapturedFramePixelsForSheet;
		TArray<FString> CapturedFrameLabelsForSheet;
		FIntPoint FrameSheetTileSize(0, 0);
		UE::UAI::Contract::FDiagnostic FrameSheetMismatch;
		FString FirstCameraSource;
		int32 SequenceCameraFrameCount = 0;
		int32 ViewportFallbackFrameCount = 0;

		bool bCreateFrameSheet = FrameRequests.Num() > 1;
		TryGetBool(Ctx.Params, TEXT("create_frame_sheet"), bCreateFrameSheet) || TryGetBool(Ctx.Params, TEXT("frame_sheet"), bCreateFrameSheet) ||
			TryGetBool(Ctx.Params, TEXT("sequence_frame_image"), bCreateFrameSheet);
		bool bWriteIndividualFrames = FrameRequests.Num() == 1;
		TryGetBool(Ctx.Params, TEXT("write_individual_frames"), bWriteIndividualFrames) ||
			TryGetBool(Ctx.Params, TEXT("keep_individual_frames"), bWriteIndividualFrames) ||
			TryGetBool(Ctx.Params, TEXT("output_individual_frames"), bWriteIndividualFrames);
		if (FrameRequests.Num() > 1 && !bCreateFrameSheet && !bWriteIndividualFrames)
		{
			OutError = Input(TEXT("multi_frame_output_disabled"), TEXT("Multi-frame capture requires a frame sheet or individual frame output."),
				TEXT("sequence_capture"), FString(),
				Facts({ { TEXT("frame_count"), Number(FrameRequests.Num()) }, { TEXT("create_frame_sheet"), MakeShared<FJsonValueBoolean>(bCreateFrameSheet) },
					{ TEXT("write_individual_frames"), MakeShared<FJsonValueBoolean>(bWriteIndividualFrames) } }),
				MakeShared<FJsonValueString>(TEXT("frame sheet or individual frame output enabled")));
			return false;
		}

		auto CaptureRequestedFrame = [&](const FLevelSequenceFrameRequest& Request, const int32 SequenceIndex,
										 TSharedPtr<FJsonObject>& OutFrameObject) -> bool
		{
			const FFrameTime DisplayFrame =
				ConvertFrameTime(FFrameTime(Request.EvaluationFrame), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate());
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
			FMinimalViewInfo CaptureView;

			if (UCameraComponent* ActiveCamera = Cleanup.Player->GetActiveCameraComponent())
			{
				ActiveCamera->GetCameraView(0.0f, CaptureView);
				CameraObjectPath = ActiveCamera->GetPathName();
				++SequenceCameraFrameCount;
			}
			else if (bFallbackToViewportCamera)
			{
				FLevelEditorViewportClient* ViewportClient = GetPreferredLevelViewportClient();
				if (!ViewportClient)
				{
					OutError = Native(TEXT("sequence_camera_not_found_and_no_viewport_fallback"), FString::Printf(TEXT("No sequence camera or viewport fallback is available at sample %d (frame %d, %.9g seconds)."), SequenceIndex, Request.FrameIndex, Request.TimeSeconds),
						TEXT("sequence_capture"), TEXT("camera_or_viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.TargetPath = AssetPath;
					return false;
				}
				CameraSource = TEXT("viewport_fallback");
				// Fallback borrows the view geometry; it is an independent world render,
				// not a copy of the editor backbuffer or its exposure history.
				CaptureView.Location = ViewportClient->GetViewLocation();
				CaptureView.Rotation = ViewportClient->GetViewRotation();
				CaptureView.FOV = ViewportClient->ViewFOV;
				++ViewportFallbackFrameCount;
			}
			else
			{
				OutError = Native(TEXT("sequence_camera_not_found"), FString::Printf(TEXT("No sequence camera is active and viewport fallback is disabled at sample %d (frame %d, %.9g seconds)."), SequenceIndex, Request.FrameIndex, Request.TimeSeconds),
					TEXT("sequence_capture"), TEXT("sequence_camera_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = AssetPath;
				return false;
			}

			CaptureView.FOV = FMath::Clamp(CaptureView.FOV, 5.0f, 170.0f);
			// Every requested sequence time is an explicit jump. Reset temporal history
			// at the cut instead of adapting from a different requested sample.
			UE::UAI::EditorCapture::ConfigureCameraCapture(*CaptureComp, CaptureView, true);
			CaptureComp->CaptureScene();
			FlushRenderingCommands();

			FRenderTarget* RenderTarget = Cleanup.RenderTarget->GameThread_GetRenderTargetResource();
			if (!RenderTarget)
			{
				OutError = Native(TEXT("render_target_resource_missing"), FString::Printf(TEXT("The sequence capture render resource is unavailable at sample %d (frame %d, %.9g seconds)."), SequenceIndex, Request.FrameIndex, Request.TimeSeconds),
					TEXT("sequence_capture"), TEXT("render_resource_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = AssetPath;
				return false;
			}

			TArray<FColor> Pixels;
			const FIntPoint ShotSize(CaptureWidth, CaptureHeight);
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
			ReadFlags.SetLinearToGamma(false);
			if (!RenderTarget->ReadPixels(Pixels, ReadFlags))
			{
				OutError = Native(TEXT("read_render_target_failed"), FString::Printf(TEXT("The sequence capture render target could not be read at sample %d (frame %d, %.9g seconds)."), SequenceIndex, Request.FrameIndex, Request.TimeSeconds),
					TEXT("sequence_capture"), TEXT("read_pixels_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = AssetPath;
				return false;
			}
			for (FColor& Pixel : Pixels)
			{
				Pixel.A = 255;
			}

			TArray<FColor> ResizedPixels;
			FIntPoint ResizedSize(0, 0);
			if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels,
																											ResizedSize, OutError))
			{
				return false;
			}

			FString OutPath;
			int64 Bytes = 0;
			if (bWriteIndividualFrames)
			{
				OutPath = FViewportCapture::MakeShotPath(Format);
				if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality,
																												 OutPath, OutError))
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
			else if (FrameSheetMismatch.IsEmpty())
			{
				FrameSheetMismatch = Native(TEXT("frame_sheet_capture_size_mismatch"),
					FString::Printf(TEXT("Captured sample %d has a different size from the first frame-sheet tile."), SequenceIndex),
					TEXT("sequence_capture"), TEXT("frame_size"), Size(ResizedSize), Size(FrameSheetTileSize));
				FrameSheetMismatch.TargetPath = AssetPath;
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
			OutFrameObject->SetNumberField(TEXT("camera_fov"), CaptureView.FOV);
			OutFrameObject->SetStringField(TEXT("projection_mode"), CaptureView.ProjectionMode == ECameraProjectionMode::Orthographic ? TEXT("orthographic") : TEXT("perspective"));
			OutFrameObject->SetNumberField(TEXT("post_process_blend_weight"), CaptureView.PostProcessBlendWeight);
			OutFrameObject->SetStringField(TEXT("exposure_history"), TEXT("independent_camera_cut"));
			OutFrameObject->SetBoolField(TEXT("persistent_rendering_state"), CaptureComp->bAlwaysPersistRenderingState);
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
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
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
			OutError = Native(TEXT("preview_frame_capture_empty"), TEXT("Sequence capture did not produce its first frame."),
				TEXT("sequence_capture"), TEXT("first_frame_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = AssetPath;
			return false;
		}

		FString FrameSheetPath;
		TSharedPtr<FJsonObject> FrameSheetData;
		if (bCreateFrameSheet && FrameRequests.Num() > 1)
		{
			if (CapturedFramePixelsForSheet.Num() != FrameRequests.Num())
			{
				OutError = !FrameSheetMismatch.IsEmpty() ? FrameSheetMismatch
					: Native(TEXT("frame_sheet_capture_size_mismatch"), TEXT("The frame-sheet tile count differs from the requested sequence frame count."),
						TEXT("sequence_capture"), TEXT("frame_sheet_tile_count"), Number(CapturedFramePixelsForSheet.Num()), Number(FrameRequests.Num()));
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
				return false;
			}
			if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteFrameSequenceSheetImage(
					CapturedFramePixelsForSheet, CapturedFrameLabelsForSheet, FrameSheetTileSize, Ctx.Params, Format, Quality, FrameSheetPath, FrameSheetData,
					OutError))
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
		OutData->SetStringField(TEXT("render_path"), TEXT("scene_capture_final_color_ldr"));
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
		UeAgentCaptureSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
		return true;
	}
}
