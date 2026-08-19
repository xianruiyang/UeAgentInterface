// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_IKAssets.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "Animation/Skeleton.h"
#include "Camera/CameraTypes.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Developer/AssetTools/Public/IAssetTools.h"
#include "EditorAnimUtils.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Materials/Material.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "GameFramework/Actor.h"
#include "Engine/PointLight.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneTrack.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargetDeprecated.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/RetargetOps/StrideWarpingOp.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/IKRigProcessor.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"

namespace
{
	struct FProbeIKGoalBaselineControl
	{
		FString ControlName;
		FString BoneName;
		FString ControlType;
	};

	struct FProbeIKGoalCachedFrame
	{
		int32 FrameIndex = INDEX_NONE;
		int32 CandidateIndex = INDEX_NONE;
		FString Label;
		FString Kind;
		FString CandidateLabel;
		FString TransformMode;
		FTransform GoalTransform = FTransform::Identity;
		FTransform ControlTransform = FTransform::Identity;
		FTransform DeltaTransform = FTransform::Identity;
		bool bHasDeltaTransform = false;
		TArray<FColor> Pixels;
	};

	struct FProbeSequenceBoneSample
	{
		FString BoneName;
		FTransform ComponentTransform = FTransform::Identity;
	};

	struct FProbeIKGoalCandidateCacheEntry
	{
		FString RequestId;
		FString ContextMode = TEXT("ik_rig_asset");
		FString AssetPath;
		FString ObjectPath;
		FString GoalName;
		FString BoneName;
		FTransform OriginalGoalTransform = FTransform::Identity;
		FString SequencePath;
		FString SequenceObjectPath;
		FString BindingGuid;
		FString BindingName;
		FString ControlName;
		FString ControlType;
		FString ControlRigClass;
		int32 SequenceFrameIndex = 0;
		double SequenceTimeSeconds = 0.0;
		FFrameNumber EvaluationFrame = FFrameNumber(0);
		FTransform OriginalControlTransform = FTransform::Identity;
		FString BaselineMode = TEXT("control_transform");
		TArray<FProbeIKGoalBaselineControl> BaselineControls;
		FDateTime CreatedUtc;
		FIntPoint FrameSize = FIntPoint::ZeroValue;
		FBox FixedCaptureBounds = FBox(ForceInit);
		double BoundsPadding = 1.35;
		FString Format = TEXT("png");
		int32 Quality = 95;
		FString FrameSheetPath;
		TArray<FProbeIKGoalCachedFrame> Frames;
	};

	static TMap<FString, FProbeIKGoalCandidateCacheEntry> GProbeIKGoalCandidateCacheByRequestId;
	static FString GLastProbeIKGoalCandidateRequestId;

	static int64 GetProbeIKGoalCachedPixelBytes(const FProbeIKGoalCandidateCacheEntry& Entry)
	{
		int64 Bytes = 0;
		for (const FProbeIKGoalCachedFrame& Frame : Entry.Frames)
		{
			Bytes += static_cast<int64>(Frame.Pixels.Num()) * static_cast<int64>(sizeof(FColor));
		}
		return Bytes;
	}

	static void ClearProbeIKGoalCachedPixels(FProbeIKGoalCandidateCacheEntry& Entry)
	{
		for (FProbeIKGoalCachedFrame& Frame : Entry.Frames)
		{
			Frame.Pixels.Reset();
		}
	}

	static void TrimProbeIKGoalCandidateCache()
	{
		constexpr int32 MaxEntries = 32;
		if (GProbeIKGoalCandidateCacheByRequestId.Num() > MaxEntries)
		{
			TArray<FString> RequestIds;
			GProbeIKGoalCandidateCacheByRequestId.GetKeys(RequestIds);
			RequestIds.Sort([](const FString& A, const FString& B)
			{
				const FProbeIKGoalCandidateCacheEntry* EntryA = GProbeIKGoalCandidateCacheByRequestId.Find(A);
				const FProbeIKGoalCandidateCacheEntry* EntryB = GProbeIKGoalCandidateCacheByRequestId.Find(B);
				const FDateTime TimeA = EntryA ? EntryA->CreatedUtc : FDateTime::MinValue();
				const FDateTime TimeB = EntryB ? EntryB->CreatedUtc : FDateTime::MinValue();
				return TimeA < TimeB;
			});

			for (int32 Index = 0; Index < RequestIds.Num() - MaxEntries; ++Index)
			{
				GProbeIKGoalCandidateCacheByRequestId.Remove(RequestIds[Index]);
			}
		}

		constexpr int64 MaxCachedPixelBytes = 512LL * 1024LL * 1024LL;
		int64 CachedPixelBytes = 0;
		TArray<FString> RemainingRequestIds;
		GProbeIKGoalCandidateCacheByRequestId.GetKeys(RemainingRequestIds);
		for (const FString& RequestId : RemainingRequestIds)
		{
			if (const FProbeIKGoalCandidateCacheEntry* Entry = GProbeIKGoalCandidateCacheByRequestId.Find(RequestId))
			{
				CachedPixelBytes += GetProbeIKGoalCachedPixelBytes(*Entry);
			}
		}
		if (CachedPixelBytes <= MaxCachedPixelBytes)
		{
			return;
		}

		RemainingRequestIds.Sort([](const FString& A, const FString& B)
		{
			const FProbeIKGoalCandidateCacheEntry* EntryA = GProbeIKGoalCandidateCacheByRequestId.Find(A);
			const FProbeIKGoalCandidateCacheEntry* EntryB = GProbeIKGoalCandidateCacheByRequestId.Find(B);
			const FDateTime TimeA = EntryA ? EntryA->CreatedUtc : FDateTime::MinValue();
			const FDateTime TimeB = EntryB ? EntryB->CreatedUtc : FDateTime::MinValue();
			return TimeA < TimeB;
		});

		for (const FString& RequestId : RemainingRequestIds)
		{
			if (CachedPixelBytes <= MaxCachedPixelBytes)
			{
				break;
			}
			FProbeIKGoalCandidateCacheEntry* Entry = GProbeIKGoalCandidateCacheByRequestId.Find(RequestId);
			if (!Entry || RequestId == GLastProbeIKGoalCandidateRequestId)
			{
				continue;
			}
			CachedPixelBytes -= GetProbeIKGoalCachedPixelBytes(*Entry);
			ClearProbeIKGoalCachedPixels(*Entry);
		}
	}

	static bool TryGetIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& OutValue)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		double NumberValue = 0.0;
		if (!Params->TryGetNumberField(FieldName, NumberValue))
		{
			return false;
		}
		OutValue = FMath::RoundToInt(NumberValue);
		return true;
	}

	static bool TryParseProbeFrameSelectionValue(const TSharedPtr<FJsonValue>& Value, int32& OutFrameIndex)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutFrameIndex = FMath::RoundToInt(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::String)
		{
			FString Text = Value->AsString();
			Text.TrimStartAndEndInline();
			if (Text.StartsWith(TEXT("F"), ESearchCase::IgnoreCase))
			{
				Text = Text.RightChop(1);
				Text.TrimStartAndEndInline();
			}
			return LexTryParseString(OutFrameIndex, *Text);
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			return TryGetIntParam(Obj, TEXT("frame_index"), OutFrameIndex)
				|| TryGetIntParam(Obj, TEXT("frame_number"), OutFrameIndex)
				|| TryGetIntParam(Obj, TEXT("number"), OutFrameIndex)
				|| TryGetIntParam(Obj, TEXT("frame"), OutFrameIndex)
				|| TryGetIntParam(Obj, TEXT("index"), OutFrameIndex)
				|| TryGetIntParam(Obj, TEXT("selection"), OutFrameIndex);
		}
		return false;
	}

	static bool TryGetProbeFrameSelectionArray(const TSharedPtr<FJsonObject>& Params, TArray<int32>& OutFrameIndices, FString& OutError)
	{
		OutFrameIndices.Reset();
		if (!Params.IsValid())
		{
			OutError = TEXT("invalid_params");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		const bool bHasArray = Params->TryGetArrayField(TEXT("frames"), Values)
			|| Params->TryGetArrayField(TEXT("frame_indices"), Values)
			|| Params->TryGetArrayField(TEXT("frame_indexes"), Values)
			|| Params->TryGetArrayField(TEXT("numbers"), Values)
			|| Params->TryGetArrayField(TEXT("selections"), Values);
		if (bHasArray && Values)
		{
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				int32 FrameIndex = INDEX_NONE;
				if (!TryParseProbeFrameSelectionValue((*Values)[Index], FrameIndex))
				{
					OutError = FString::Printf(TEXT("invalid_frame_selection_at_index_%d"), Index);
					return false;
				}
				OutFrameIndices.Add(FrameIndex);
			}
		}
		else
		{
			int32 SingleFrameIndex = INDEX_NONE;
			if (TryGetIntParam(Params, TEXT("frame_index"), SingleFrameIndex)
				|| TryGetIntParam(Params, TEXT("frame_number"), SingleFrameIndex)
				|| TryGetIntParam(Params, TEXT("number"), SingleFrameIndex)
				|| TryGetIntParam(Params, TEXT("index"), SingleFrameIndex)
				|| TryGetIntParam(Params, TEXT("selection"), SingleFrameIndex))
			{
				OutFrameIndices.Add(SingleFrameIndex);
			}
		}

		if (OutFrameIndices.Num() <= 0)
		{
			OutError = TEXT("missing_frames");
			return false;
		}
		return true;
	}

	static ULevelSequence* LoadProbeLevelSequence(const FString& InPath)
	{
		return Cast<ULevelSequence>(UeAgentIKAssetOps::LoadAssetObject(InPath));
	}

	static FString ProbeControlTypeToString(const ERigControlType Type)
	{
		switch (Type)
		{
		case ERigControlType::Position: return TEXT("position");
		case ERigControlType::Scale: return TEXT("scale");
		case ERigControlType::Rotator: return TEXT("rotator");
		case ERigControlType::Transform: return TEXT("transform");
		case ERigControlType::TransformNoScale: return TEXT("transform_no_scale");
		case ERigControlType::EulerTransform: return TEXT("euler_transform");
		default: return TEXT("unsupported");
		}
	}

	static bool IsProbeTransformControlType(const ERigControlType Type)
	{
		return Type == ERigControlType::Position
			|| Type == ERigControlType::Scale
			|| Type == ERigControlType::Rotator
			|| Type == ERigControlType::Transform
			|| Type == ERigControlType::TransformNoScale
			|| Type == ERigControlType::EulerTransform;
	}

	static bool TryGetProbeNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Key : Keys)
		{
			if (Obj->TryGetNumberField(Key, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool ProbeJsonTryGetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool ProbeJsonTryGetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool ProbeJsonTryGetBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static bool ProbeJsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FVector& OutValue)
	{
		return UeAgentIKAssetOps::TryGetVectorObject(Obj, FString(Key), OutValue);
	}

	static bool ProbeJsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FRotator& OutValue)
	{
		return UeAgentIKAssetOps::TryGetRotatorObject(Obj, FString(Key), OutValue);
	}

	static FLevelEditorViewportClient* GetProbePreferredLevelViewportClient()
	{
		if (GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->Viewport == ActiveViewport)
					{
						return ViewportClient;
					}
				}
			}
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (ViewportClient && ViewportClient->Viewport)
				{
					return ViewportClient;
				}
			}
		}
		return nullptr;
	}

	static bool ResolveProbeSequenceFrame(
		UMovieScene* MovieScene,
		const TSharedPtr<FJsonObject>& Params,
		FFrameNumber& OutEvaluationFrame,
		int32& OutFrameIndex,
		double& OutTimeSeconds,
		FString& OutError)
	{
		if (!MovieScene || !Params.IsValid())
		{
			OutError = TEXT("invalid_sequence_frame_context");
			return false;
		}

		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		const double FramesPerSecond = DisplayRate.IsValid() && DisplayRate.AsDecimal() > KINDA_SMALL_NUMBER
			? DisplayRate.AsDecimal()
			: 30.0;
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
		const FFrameNumber StartTick = PlaybackRange.GetLowerBoundValue();
		const FFrameNumber EndExclusiveTick = PlaybackRange.GetUpperBoundValue();
		if (EndExclusiveTick.Value <= StartTick.Value)
		{
			OutError = TEXT("invalid_sequence_playback_range");
			return false;
		}

		double EvaluationFrameValue = 0.0;
		if (TryGetProbeNumberAnyField(Params, { TEXT("evaluation_frame"), TEXT("tick_frame") }, EvaluationFrameValue))
		{
			OutEvaluationFrame = FFrameNumber(static_cast<int32>(FMath::RoundToInt(EvaluationFrameValue)));
			OutTimeSeconds = FMath::Max(0.0, TickResolution.AsSeconds(OutEvaluationFrame - StartTick));
			OutFrameIndex = FMath::RoundToInt(OutTimeSeconds * FramesPerSecond);
		}
		else
		{
			double TimeSeconds = 0.0;
			const bool bUseTime = TryGetProbeNumberAnyField(Params, { TEXT("time_seconds"), TEXT("seconds") }, TimeSeconds);
			double FrameValue = 0.0;
			const bool bUseFrame = TryGetProbeNumberAnyField(Params, { TEXT("sequence_frame_index"), TEXT("frame_index"), TEXT("frame"), TEXT("display_frame") }, FrameValue);
			if (!bUseTime && !bUseFrame)
			{
				FrameValue = 0.0;
			}

			OutTimeSeconds = bUseTime ? TimeSeconds : static_cast<double>(FMath::RoundToInt(FrameValue)) / FramesPerSecond;
			if (OutTimeSeconds < -KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("invalid_time_seconds");
				return false;
			}
			OutFrameIndex = bUseTime ? FMath::RoundToInt(OutTimeSeconds * FramesPerSecond) : FMath::RoundToInt(FrameValue);
			const double AbsoluteTimeSeconds = TickResolution.AsSeconds(StartTick) + OutTimeSeconds;
			OutEvaluationFrame = TickResolution.AsFrameTime(AbsoluteTimeSeconds).RoundToFrame();
		}

		if (OutEvaluationFrame.Value < StartTick.Value || OutEvaluationFrame.Value >= EndExclusiveTick.Value)
		{
			OutError = TEXT("probe_sequence_frame_out_of_range");
			return false;
		}
		return true;
	}

	static bool TryResolveProbeBindingGuid(
		UMovieScene* MovieScene,
		const TSharedPtr<FJsonObject>& Params,
		const FString& ControlNameText,
		FGuid& OutBindingGuid,
		FString& OutBindingName,
		FString& OutError)
	{
		OutBindingGuid.Invalidate();
		OutBindingName.Reset();
		if (!MovieScene || !Params.IsValid())
		{
			OutError = TEXT("invalid_binding_context");
			return false;
		}

		FString BindingGuidText;
		if (Params->TryGetStringField(TEXT("binding_guid"), BindingGuidText)
			|| Params->TryGetStringField(TEXT("binding_id"), BindingGuidText)
			|| Params->TryGetStringField(TEXT("object_binding_guid"), BindingGuidText))
		{
			BindingGuidText.TrimStartAndEndInline();
			if (!FGuid::Parse(BindingGuidText, OutBindingGuid) || !MovieScene->FindBinding(OutBindingGuid))
			{
				OutError = TEXT("binding_guid_not_found");
				return false;
			}
			OutBindingName = MovieScene->FindBinding(OutBindingGuid)->GetName();
			return true;
		}

		FString BindingNameText;
		if (Params->TryGetStringField(TEXT("binding_name"), BindingNameText)
			|| Params->TryGetStringField(TEXT("binding_display_name"), BindingNameText)
			|| Params->TryGetStringField(TEXT("object_name"), BindingNameText))
		{
			BindingNameText.TrimStartAndEndInline();
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
			{
				if (Binding.GetName().Equals(BindingNameText, ESearchCase::IgnoreCase))
				{
					OutBindingGuid = Binding.GetObjectGuid();
					OutBindingName = Binding.GetName();
					return true;
				}
			}
			OutError = TEXT("binding_name_not_found");
			return false;
		}

		TArray<FGuid> MatchingBindings;
		TArray<FString> MatchingBindingNames;
		const FName ControlName(*ControlNameText);
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			TArray<UMovieSceneTrack*> Tracks = MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), Binding.GetObjectGuid(), NAME_None);
			for (UMovieSceneTrack* Track : Tracks)
			{
				UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
				UControlRig* ControlRig = ControlRigTrack ? ControlRigTrack->GetControlRig() : nullptr;
				if (ControlRig && ControlRig->FindControl(ControlName))
				{
					MatchingBindings.Add(Binding.GetObjectGuid());
					MatchingBindingNames.Add(Binding.GetName());
					break;
				}
			}
		}
		if (MatchingBindings.Num() == 1)
		{
			OutBindingGuid = MatchingBindings[0];
			OutBindingName = MatchingBindingNames[0];
			return true;
		}
		OutError = MatchingBindings.Num() > 1 ? TEXT("ambiguous_control_rig_binding") : TEXT("missing_binding_guid");
		return false;
	}

	static bool TryFindProbeControlRigTrack(
		UMovieScene* MovieScene,
		const FGuid& BindingGuid,
		const FName ControlName,
		const FString& TrackNameFilter,
		const FFrameNumber EvaluationFrame,
		const bool bFindOrAddSection,
		UMovieSceneControlRigParameterTrack*& OutTrack,
		UMovieSceneControlRigParameterSection*& OutSection,
		UControlRig*& OutControlRig,
		FString& OutError)
	{
		OutTrack = nullptr;
		OutSection = nullptr;
		OutControlRig = nullptr;
		if (!MovieScene || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_control_rig_track_context");
			return false;
		}

		const FString TrackFilter = TrackNameFilter.TrimStartAndEnd();
		TArray<UMovieSceneTrack*> Tracks = MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), BindingGuid, NAME_None);
		for (UMovieSceneTrack* Track : Tracks)
		{
			UMovieSceneControlRigParameterTrack* ControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(Track);
			if (!ControlRigTrack)
			{
				continue;
			}
			if (!TrackFilter.IsEmpty()
				&& !ControlRigTrack->GetTrackName().ToString().Equals(TrackFilter, ESearchCase::IgnoreCase)
				&& !ControlRigTrack->GetDisplayName().ToString().Equals(TrackFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			UControlRig* ControlRig = ControlRigTrack->GetControlRig();
			if (!ControlRig || !ControlRig->FindControl(ControlName))
			{
				continue;
			}

			UMovieSceneControlRigParameterSection* ControlRigSection = nullptr;
			if (bFindOrAddSection)
			{
				bool bSectionAdded = false;
				ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(ControlRigTrack->FindOrAddSection(EvaluationFrame, bSectionAdded));
				if (ControlRigSection && bSectionAdded)
				{
					ControlRigSection->SetControlRig(ControlRig);
				}
			}
			else
			{
				ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(ControlRigTrack->FindSection(EvaluationFrame));
				if (!ControlRigSection)
				{
					for (UMovieSceneSection* Section : ControlRigTrack->GetAllSections())
					{
						ControlRigSection = Cast<UMovieSceneControlRigParameterSection>(Section);
						if (ControlRigSection)
						{
							break;
						}
					}
				}
			}

			if (!ControlRigSection)
			{
				continue;
			}
			if (!ControlRigSection->GetControlRig())
			{
				ControlRigSection->SetControlRig(ControlRig);
			}

			OutTrack = ControlRigTrack;
			OutSection = ControlRigSection;
			OutControlRig = ControlRig;
			return true;
		}

		OutError = TrackFilter.IsEmpty() ? TEXT("control_rig_track_or_control_not_found") : TEXT("control_rig_track_filter_not_found");
		return false;
	}

	static int32 GetProbeControlFloatChannelCount(const ERigControlType ControlType)
	{
		switch (ControlType)
		{
		case ERigControlType::Position:
		case ERigControlType::Scale:
		case ERigControlType::Rotator:
			return 3;
		case ERigControlType::TransformNoScale:
			return 6;
		case ERigControlType::Transform:
		case ERigControlType::EulerTransform:
			return 9;
		default:
			break;
		}
		return 0;
	}

	static bool TryEvaluateProbeControlTransform(
		UControlRig* ControlRig,
		UMovieSceneControlRigParameterSection* Section,
		const FName ControlName,
		const ERigControlType ControlType,
		const FFrameNumber EvaluationFrame,
		FTransform& OutTransform,
		FString& OutError)
	{
		if (!ControlRig)
		{
			OutError = TEXT("invalid_control_rig_evaluate_context");
			return false;
		}

		auto ValidateFloatChannelRange = [&OutError](
			UMovieSceneControlRigParameterSection* InSection,
			const FName InControlName,
			const ERigControlType InControlType,
			bool& bOutCanUseSectionChannels) -> bool
		{
			bOutCanUseSectionChannels = false;
			if (!InSection)
			{
				return true;
			}

			const int32 RequiredChannelCount = GetProbeControlFloatChannelCount(InControlType);
			if (RequiredChannelCount <= 0)
			{
				OutError = TEXT("control_rig_control_type_not_supported");
				return false;
			}

			InSection->ReconstructChannelProxy();

			auto TryValidateCurrentProxy = [&OutError, InSection, InControlName, RequiredChannelCount, &bOutCanUseSectionChannels]() -> bool
			{
				const TArrayView<FMovieSceneFloatChannel*> FloatChannels = InSection->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
				const FChannelMapInfo* ChannelInfo = InSection->ControlChannelMap.Find(InControlName);
				if (!ChannelInfo)
				{
					return true;
				}

				const int32 StartIndex = ChannelInfo->ChannelIndex;
				const int32 AvailableChannelCount = FloatChannels.Num();
				if (StartIndex < 0
					|| StartIndex > AvailableChannelCount
					|| RequiredChannelCount > AvailableChannelCount - StartIndex)
				{
					OutError = FString::Printf(
						TEXT("control_rig_float_channel_range_invalid:start=%d,required=%d,available=%d"),
						StartIndex,
						RequiredChannelCount,
						AvailableChannelCount);
					return false;
				}

				for (int32 Offset = 0; Offset < RequiredChannelCount; ++Offset)
				{
					if (!FloatChannels[StartIndex + Offset])
					{
						OutError = FString::Printf(
							TEXT("control_rig_float_channel_null:start=%d,offset=%d"),
							StartIndex,
							Offset);
						return false;
					}
				}

				bOutCanUseSectionChannels = true;
				return true;
			};

			OutError.Reset();
			if (TryValidateCurrentProxy())
			{
				return true;
			}

			InSection->ReconstructChannelProxy();
			bOutCanUseSectionChannels = false;
			OutError.Reset();
			if (TryValidateCurrentProxy())
			{
				return true;
			}

			if (OutError.IsEmpty())
			{
				OutError = TEXT("control_rig_float_channel_validation_failed");
			}
			return false;
		};

		const FFrameTime EvalTime(EvaluationFrame);
		if (Section)
		{
			bool bCanUseSectionChannels = false;
			if (!ValidateFloatChannelRange(Section, ControlName, ControlType, bCanUseSectionChannels))
			{
				return false;
			}

			if (ControlType == ERigControlType::Transform
				|| ControlType == ERigControlType::TransformNoScale
				|| ControlType == ERigControlType::EulerTransform)
			{
				if (bCanUseSectionChannels)
				{
					if (TOptional<FEulerTransform> Value = Section->EvaluateTransformParameter(EvalTime, ControlName))
					{
						OutTransform = Value.GetValue().ToFTransform();
						return true;
					}
				}
			}
			else if (ControlType == ERigControlType::Position)
			{
				if (bCanUseSectionChannels)
				{
					if (TOptional<FVector> Value = Section->EvaluateVectorParameter(EvalTime, ControlName))
					{
						OutTransform = FTransform(FRotator::ZeroRotator, Value.GetValue(), FVector::OneVector);
						return true;
					}
				}
			}
			else if (ControlType == ERigControlType::Scale)
			{
				if (bCanUseSectionChannels)
				{
					if (TOptional<FVector> Value = Section->EvaluateVectorParameter(EvalTime, ControlName))
					{
						OutTransform = FTransform(FRotator::ZeroRotator, FVector::ZeroVector, Value.GetValue());
						return true;
					}
				}
			}
			else if (ControlType == ERigControlType::Rotator)
			{
				if (bCanUseSectionChannels)
				{
					if (TOptional<FVector> Value = Section->EvaluateVectorParameter(EvalTime, ControlName))
					{
						OutTransform = FTransform(FRotator(Value.GetValue().Y, Value.GetValue().Z, Value.GetValue().X), FVector::ZeroVector, FVector::OneVector);
						return true;
					}
				}
			}
		}

		OutTransform = ControlRig->GetControlLocalTransform(ControlName);
		return true;
	}

	static bool EnsureProbeControlFloatChannelRange(
		UMovieSceneControlRigParameterSection* Section,
		const FName ControlName,
		const ERigControlType ControlType,
		FString& OutError)
	{
		if (!Section)
		{
			OutError = TEXT("invalid_control_rig_section");
			return false;
		}

		const int32 RequiredChannelCount = GetProbeControlFloatChannelCount(ControlType);
		if (RequiredChannelCount <= 0)
		{
			OutError = TEXT("control_rig_control_type_not_supported");
			return false;
		}

		auto ValidateCurrentProxy = [&OutError, Section, ControlName, RequiredChannelCount]() -> bool
		{
			const TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			const FChannelMapInfo* ChannelInfo = Section->ControlChannelMap.Find(ControlName);
			if (!ChannelInfo)
			{
				OutError = TEXT("control_rig_float_channel_map_missing");
				return false;
			}

			const int32 StartIndex = ChannelInfo->ChannelIndex;
			const int32 AvailableChannelCount = FloatChannels.Num();
			if (StartIndex < 0
				|| StartIndex > AvailableChannelCount
				|| RequiredChannelCount > AvailableChannelCount - StartIndex)
			{
				OutError = FString::Printf(
					TEXT("control_rig_float_channel_range_invalid:start=%d,required=%d,available=%d"),
					StartIndex,
					RequiredChannelCount,
					AvailableChannelCount);
				return false;
			}

			for (int32 Offset = 0; Offset < RequiredChannelCount; ++Offset)
			{
				if (!FloatChannels[StartIndex + Offset])
				{
					OutError = FString::Printf(
						TEXT("control_rig_float_channel_null:start=%d,offset=%d"),
						StartIndex,
						Offset);
					return false;
				}
			}

			return true;
		};

		Section->ReconstructChannelProxy();
		OutError.Reset();
		if (ValidateCurrentProxy())
		{
			return true;
		}

		Section->ReconstructChannelProxy();
		OutError.Reset();
		if (ValidateCurrentProxy())
		{
			return true;
		}

		if (OutError.IsEmpty())
		{
			OutError = TEXT("control_rig_float_channel_validation_failed");
		}
		return false;
	}

	static bool ApplyProbeControlTransformKey(
		UMovieSceneControlRigParameterSection* Section,
		UControlRig* ControlRig,
		const FName ControlName,
		const ERigControlType ControlType,
		const FFrameNumber EvaluationFrame,
		const FTransform& Transform,
		FString& OutError)
	{
		if (!Section || !ControlRig)
		{
			OutError = TEXT("invalid_control_rig_apply_context");
			return false;
		}
		FRigControlElement* ControlElement = ControlRig->FindControl(ControlName);
		if (!ControlElement)
		{
			OutError = TEXT("control_rig_control_not_found");
			return false;
		}
		if (URigHierarchy* Hierarchy = ControlRig->GetHierarchy())
		{
			if (!Hierarchy->IsAnimatable(ControlElement))
			{
				OutError = TEXT("control_rig_control_not_animatable");
				return false;
			}
		}

		Section->Modify();
		Section->ReconstructChannelProxy();
		if (ControlType == ERigControlType::Transform
			|| ControlType == ERigControlType::TransformNoScale
			|| ControlType == ERigControlType::EulerTransform)
		{
			Section->AddTransformParameterKey(ControlName, EvaluationFrame, Transform);
		}
		else if (ControlType == ERigControlType::Position)
		{
			Section->AddVectorParameterKey(ControlName, EvaluationFrame, Transform.GetLocation());
		}
		else if (ControlType == ERigControlType::Scale)
		{
			Section->AddVectorParameterKey(ControlName, EvaluationFrame, Transform.GetScale3D());
		}
		else if (ControlType == ERigControlType::Rotator)
		{
			const FRotator Rotator = Transform.Rotator();
			Section->AddVectorParameterKey(ControlName, EvaluationFrame, FVector(Rotator.Roll, Rotator.Pitch, Rotator.Yaw));
		}
		else
		{
			OutError = TEXT("control_rig_control_type_not_supported");
			return false;
		}

		Section->ReconstructChannelProxy();
		if (!EnsureProbeControlFloatChannelRange(Section, ControlName, ControlType, OutError))
		{
			return false;
		}
		Section->AutoSetTangents(ControlName);
		return true;
	}

	static bool TryBuildProbeCandidateControlTransform(
		const TSharedPtr<FJsonObject>& CandidateObj,
		const FTransform& OriginalTransform,
		FTransform& OutControlTransform,
		FTransform& OutDeltaTransform,
		FString& OutMode,
		FString& OutError)
	{
		if (!CandidateObj.IsValid())
		{
			OutControlTransform = OriginalTransform;
			OutDeltaTransform = FTransform::Identity;
			OutMode = TEXT("original");
			return true;
		}

		auto TryGetTransformObjectByAliases = [](const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, TSharedPtr<FJsonObject>& OutObject) -> bool
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			for (const FString& Key : Keys)
			{
				const TSharedPtr<FJsonObject>* Nested = nullptr;
				if (Obj->TryGetObjectField(Key, Nested) && Nested && Nested->IsValid())
				{
					OutObject = *Nested;
					return true;
				}
			}
			return false;
		};

		TSharedPtr<FJsonObject> TransformObj = CandidateObj;
		bool bRelativeTransformObject = false;
		bool bAbsoluteTransformObject = false;
		if (TryGetTransformObjectByAliases(CandidateObj, { TEXT("control"), TEXT("goal") }, TransformObj))
		{
			TSharedPtr<FJsonObject> NestedTransformObj;
			if (TryGetTransformObjectByAliases(TransformObj, { TEXT("relative_transform"), TEXT("delta_transform"), TEXT("transform_delta") }, NestedTransformObj))
			{
				TransformObj = NestedTransformObj;
				bRelativeTransformObject = true;
			}
			else if (TryGetTransformObjectByAliases(TransformObj, { TEXT("absolute_transform") }, NestedTransformObj))
			{
				TransformObj = NestedTransformObj;
				bAbsoluteTransformObject = true;
			}
			else if (TryGetTransformObjectByAliases(TransformObj, { TEXT("transform") }, NestedTransformObj))
			{
				TransformObj = NestedTransformObj;
				bRelativeTransformObject = true;
			}
		}
		else if (TryGetTransformObjectByAliases(CandidateObj, { TEXT("relative_transform"), TEXT("delta_transform"), TEXT("transform_delta") }, TransformObj))
		{
			bRelativeTransformObject = true;
		}
		else if (TryGetTransformObjectByAliases(CandidateObj, { TEXT("absolute_transform") }, TransformObj))
		{
			bAbsoluteTransformObject = true;
		}
		else if (TryGetTransformObjectByAliases(CandidateObj, { TEXT("transform") }, TransformObj))
		{
			bRelativeTransformObject = true;
		}

		bool bRelativeToOriginal = bRelativeTransformObject || !bAbsoluteTransformObject;
		CandidateObj->TryGetBoolField(TEXT("relative"), bRelativeToOriginal);
		CandidateObj->TryGetBoolField(TEXT("relative_to_original"), bRelativeToOriginal);
		TransformObj->TryGetBoolField(TEXT("relative"), bRelativeToOriginal);
		TransformObj->TryGetBoolField(TEXT("relative_to_original"), bRelativeToOriginal);

		FString ModeText;
		if (CandidateObj->TryGetStringField(TEXT("mode"), ModeText) || TransformObj->TryGetStringField(TEXT("mode"), ModeText))
		{
			ModeText = ModeText.TrimStartAndEnd().ToLower();
			if (ModeText == TEXT("absolute") || ModeText == TEXT("absolute_transform"))
			{
				bRelativeToOriginal = false;
			}
			else if (ModeText == TEXT("relative") || ModeText == TEXT("delta") || ModeText == TEXT("relative_transform"))
			{
				bRelativeToOriginal = true;
			}
		}

		FVector LocationValue = bRelativeToOriginal ? FVector::ZeroVector : OriginalTransform.GetLocation();
		FRotator RotationValue = bRelativeToOriginal ? FRotator::ZeroRotator : OriginalTransform.Rotator();
		FVector ScaleValue = bRelativeToOriginal ? FVector::OneVector : OriginalTransform.GetScale3D();
		bool bHasLocationField = false;
		bool bHasRotationField = false;
		bool bHasScaleField = false;
		if (bRelativeToOriginal)
		{
			bHasLocationField = ProbeJsonTryGetVector(TransformObj, TEXT("location"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("translation"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("position"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("location_offset"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("delta_location"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("position_offset"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("delta_position"), LocationValue);
			bHasRotationField = ProbeJsonTryGetRotator(TransformObj, TEXT("rotation"), RotationValue)
				|| ProbeJsonTryGetRotator(TransformObj, TEXT("rotation_offset"), RotationValue)
				|| ProbeJsonTryGetRotator(TransformObj, TEXT("delta_rotation"), RotationValue);
			bHasScaleField = ProbeJsonTryGetVector(TransformObj, TEXT("scale"), ScaleValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("scale_multiplier"), ScaleValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("scale_delta"), ScaleValue);
		}
		else
		{
			bHasLocationField = ProbeJsonTryGetVector(TransformObj, TEXT("location"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("translation"), LocationValue)
				|| ProbeJsonTryGetVector(TransformObj, TEXT("position"), LocationValue);
			bHasRotationField = ProbeJsonTryGetRotator(TransformObj, TEXT("rotation"), RotationValue);
			bHasScaleField = ProbeJsonTryGetVector(TransformObj, TEXT("scale"), ScaleValue);
		}

		if (!bHasLocationField && !bHasRotationField && !bHasScaleField)
		{
			OutError = TEXT("candidate_missing_transform_fields");
			return false;
		}

		if (bRelativeToOriginal)
		{
			OutControlTransform = FTransform(
				OriginalTransform.Rotator() + RotationValue,
				OriginalTransform.GetLocation() + LocationValue,
				OriginalTransform.GetScale3D() * ScaleValue);
			OutDeltaTransform = FTransform(RotationValue, LocationValue, ScaleValue);
			OutMode = TEXT("relative_to_original_control");
		}
		else
		{
			OutControlTransform = FTransform(RotationValue, LocationValue, ScaleValue);
			const FVector OriginalScale = OriginalTransform.GetScale3D();
			const FVector SafeOriginalScale(
				FMath::IsNearlyZero(OriginalScale.X) ? 1.0 : OriginalScale.X,
				FMath::IsNearlyZero(OriginalScale.Y) ? 1.0 : OriginalScale.Y,
				FMath::IsNearlyZero(OriginalScale.Z) ? 1.0 : OriginalScale.Z);
			OutDeltaTransform = FTransform(
				OutControlTransform.Rotator() - OriginalTransform.Rotator(),
				OutControlTransform.GetLocation() - OriginalTransform.GetLocation(),
				OutControlTransform.GetScale3D() / SafeOriginalScale);
			OutMode = TEXT("absolute_control");
		}
		return true;
	}

	static bool ParseProbeBaselineControlBoneMap(
		const TSharedPtr<FJsonObject>& Params,
		UControlRig* SourceControlRig,
		TArray<FProbeIKGoalBaselineControl>& OutBaselineControls,
		FString& OutError)
	{
		OutBaselineControls.Reset();
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* MapObjectPtr = nullptr;
		const bool bHasMap = Params->TryGetObjectField(TEXT("baseline_control_bone_map"), MapObjectPtr)
			|| Params->TryGetObjectField(TEXT("control_bone_map"), MapObjectPtr)
			|| Params->TryGetObjectField(TEXT("baseline_skeletal_pose_control_bone_map"), MapObjectPtr);

		bool bBaselineFromSkeletalPose = false;
		ProbeJsonTryGetBool(Params, TEXT("baseline_from_skeletal_pose"), bBaselineFromSkeletalPose);
		if (!bHasMap || !MapObjectPtr || !MapObjectPtr->IsValid())
		{
			return true;
		}
		if (!SourceControlRig)
		{
			OutError = TEXT("missing_control_rig_for_baseline_map");
			return false;
		}

		TSet<FString> SeenControls;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*MapObjectPtr)->Values)
		{
			FString ControlNameText = Pair.Key.TrimStartAndEnd();
			FString BoneNameText;
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
			{
				BoneNameText = Pair.Value->AsString().TrimStartAndEnd();
			}
			else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> EntryObject = Pair.Value->AsObject();
				if (EntryObject.IsValid())
				{
					EntryObject->TryGetStringField(TEXT("bone_name"), BoneNameText);
					if (BoneNameText.TrimStartAndEnd().IsEmpty())
					{
						EntryObject->TryGetStringField(TEXT("bone"), BoneNameText);
					}
				}
				BoneNameText.TrimStartAndEndInline();
			}

			if (ControlNameText.IsEmpty() || BoneNameText.IsEmpty())
			{
				OutError = TEXT("invalid_baseline_control_bone_map_entry");
				return false;
			}
			if (SeenControls.Contains(ControlNameText))
			{
				OutError = TEXT("duplicate_baseline_control_name");
				return false;
			}

			const FName ControlName(*ControlNameText);
			FRigControlElement* ControlElement = SourceControlRig->FindControl(ControlName);
			if (!ControlElement)
			{
				OutError = FString::Printf(TEXT("baseline_control_not_found:%s"), *ControlNameText);
				return false;
			}
			const ERigControlType ControlType = ControlElement->Settings.ControlType;
			if (!IsProbeTransformControlType(ControlType))
			{
				OutError = FString::Printf(TEXT("baseline_control_type_not_supported:%s"), *ControlNameText);
				return false;
			}

			FProbeIKGoalBaselineControl BaselineControl;
			BaselineControl.ControlName = ControlNameText;
			BaselineControl.BoneName = BoneNameText;
			BaselineControl.ControlType = ProbeControlTypeToString(ControlType);
			OutBaselineControls.Add(BaselineControl);
			SeenControls.Add(ControlNameText);
		}
		return true;
	}

	static FBox BuildProbeBoundObjectsBox(const TArray<UObject*>& BoundObjects)
	{
		FBox Bounds(ForceInit);
		for (UObject* Object : BoundObjects)
		{
			if (!Object)
			{
				continue;
			}
			if (AActor* Actor = Cast<AActor>(Object))
			{
				const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
				if (ActorBox.IsValid)
				{
					Bounds += ActorBox;
				}
				continue;
			}
			if (USceneComponent* SceneComponent = Cast<USceneComponent>(Object))
			{
				const FBox ComponentBox = SceneComponent->Bounds.GetBox();
				if (ComponentBox.IsValid)
				{
					Bounds += ComponentBox;
				}
			}
		}
		return Bounds;
	}

	static bool ResolveProbeBoundObjects(ULevelSequence* Sequence, const FGuid& BindingGuid, UWorld* World, TArray<UObject*>& OutObjects)
	{
		OutObjects.Reset();
		if (!Sequence || !BindingGuid.IsValid() || !World)
		{
			return false;
		}
		TArray<UObject*, TInlineAllocator<1>> ResolvedObjects;
		Sequence->LocateBoundObjects(BindingGuid, UE::UniversalObjectLocator::FResolveParams(World), nullptr, ResolvedObjects);
		OutObjects.Append(ResolvedObjects);
		return OutObjects.Num() > 0;
	}

	static USkeletalMeshComponent* FindProbeSkeletalMeshComponent(UObject* Object)
	{
		if (!Object)
		{
			return nullptr;
		}
		if (USkeletalMeshComponent* SkeletalComponent = Cast<USkeletalMeshComponent>(Object))
		{
			return SkeletalComponent;
		}
		if (AActor* Actor = Cast<AActor>(Object))
		{
			return Actor->FindComponentByClass<USkeletalMeshComponent>();
		}
		return nullptr;
	}

	static UObject* ResolveProbeControlRigBoundObject(ULevelSequence* Sequence, const FGuid& BindingGuid, UWorld* World)
	{
		TArray<UObject*> BoundObjects;
		if (!ResolveProbeBoundObjects(Sequence, BindingGuid, World, BoundObjects))
		{
			return nullptr;
		}
		for (UObject* BoundObject : BoundObjects)
		{
			if (USkeletalMeshComponent* SkeletalComponent = FindProbeSkeletalMeshComponent(BoundObject))
			{
				return SkeletalComponent;
			}
		}
		return BoundObjects.Num() > 0 ? BoundObjects[0] : nullptr;
	}

	static bool RebindProbeControlRigToSequenceObject(
		ULevelSequence* Sequence,
		const FGuid& BindingGuid,
		UControlRig* ControlRig,
		UWorld* World,
		FString& OutError)
	{
		if (!Sequence || !BindingGuid.IsValid() || !ControlRig || !World)
		{
			OutError = TEXT("invalid_control_rig_rebind_context");
			return false;
		}
		UObject* BoundObject = ResolveProbeControlRigBoundObject(Sequence, BindingGuid, World);
		if (!BoundObject)
		{
			OutError = TEXT("sequence_control_rig_bound_object_not_found");
			return false;
		}

		ControlRig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
		const TSharedPtr<IControlRigObjectBinding> ObjectBinding = ControlRig->GetObjectBinding();
		if (!ObjectBinding.IsValid())
		{
			OutError = TEXT("control_rig_object_binding_create_failed");
			return false;
		}
		ObjectBinding->BindToObject(BoundObject);
		FControlRigBindingHelper::BindToSequencerInstance(ControlRig);
		ControlRig->RequestInit();
		ControlRig->Evaluate_AnyThread();
		return true;
	}

	static void RefreshProbeSkeletalBoundObjects(const TArray<UObject*>& BoundObjects)
	{
		for (UObject* BoundObject : BoundObjects)
		{
			USkeletalMeshComponent* SkeletalComponent = FindProbeSkeletalMeshComponent(BoundObject);
			if (!SkeletalComponent)
			{
				continue;
			}
			SkeletalComponent->SetUpdateAnimationInEditor(true);
			SkeletalComponent->RefreshBoneTransforms();
			SkeletalComponent->RefreshFollowerComponents();
			SkeletalComponent->UpdateComponentToWorld();
			SkeletalComponent->FinalizeBoneTransform();
			SkeletalComponent->UpdateBounds();
			SkeletalComponent->MarkRenderTransformDirty();
			SkeletalComponent->MarkRenderDynamicDataDirty();
			SkeletalComponent->MarkRenderStateDirty();
		}
	}

	static FFrameTime ConvertProbeEvaluationTickToPlaybackFrame(UMovieScene* MovieScene, const FFrameNumber EvaluationFrame)
	{
		if (!MovieScene)
		{
			return FFrameTime(EvaluationFrame);
		}
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		if (!DisplayRate.IsValid() || DisplayRate.AsDecimal() <= KINDA_SMALL_NUMBER)
		{
			DisplayRate = FFrameRate(30, 1);
		}
		const double AbsoluteSeconds = TickResolution.AsSeconds(FFrameTime(EvaluationFrame));
		return DisplayRate.AsFrameTime(AbsoluteSeconds);
	}

	template <typename CallbackType>
	static bool WithProbeSequenceFrameEvaluation(
		UWorld* World,
		ULevelSequence* Sequence,
		const FGuid& BindingGuid,
		const FFrameNumber EvaluationFrame,
		CallbackType&& Callback,
		FString& OutError)
	{
		if (!World || !Sequence || !BindingGuid.IsValid())
		{
			OutError = TEXT("invalid_sequence_evaluation_context");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}

		struct FProbeSequenceEvaluationCleanup
		{
			UWorld* World = nullptr;
			ULevelSequencePlayer* Player = nullptr;
			ALevelSequenceActor* SequenceActor = nullptr;

			~FProbeSequenceEvaluationCleanup()
			{
				if (Player)
				{
					Player->Stop();
				}
				if (World && SequenceActor && IsValid(SequenceActor))
				{
					World->DestroyActor(SequenceActor, false, false);
				}
			}
		};

		FProbeSequenceEvaluationCleanup Cleanup;
		Cleanup.World = World;
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

		TArray<UObject*> BoundObjects;
		if (!ResolveProbeBoundObjects(Sequence, BindingGuid, World, BoundObjects))
		{
			OutError = TEXT("sequence_binding_bound_objects_not_found");
			return false;
		}

		const FFrameTime PlaybackFrame = ConvertProbeEvaluationTickToPlaybackFrame(MovieScene, EvaluationFrame);
		Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(PlaybackFrame, EUpdatePositionMethod::Jump));
		RefreshProbeSkeletalBoundObjects(BoundObjects);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();

		return Callback(BoundObjects);
	}

	static void AppendProbeStringArrayField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				Text = Value->AsString();
			}
			else if (Value.IsValid() && Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Obj = Value->AsObject();
				if (Obj.IsValid())
				{
					Obj->TryGetStringField(TEXT("bone_name"), Text)
						|| Obj->TryGetStringField(TEXT("bone"), Text)
						|| Obj->TryGetStringField(TEXT("name"), Text);
				}
			}
			Text.TrimStartAndEndInline();
			if (!Text.IsEmpty())
			{
				OutValues.AddUnique(Text);
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> BuildProbeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static void AddProbeWatchedBoneIfPresent(
		USkeletalMeshComponent* SkeletalComponent,
		const FString& BoneNameText,
		const bool bExplicit,
		TArray<FString>& OutBoneNames,
		TArray<FString>& OutWarnings)
	{
		FString TrimmedBoneName = BoneNameText;
		TrimmedBoneName.TrimStartAndEndInline();
		if (TrimmedBoneName.IsEmpty())
		{
			return;
		}
		if (!SkeletalComponent)
		{
			if (bExplicit)
			{
				OutWarnings.Add(FString::Printf(TEXT("watched_bone_no_skeletal_component:%s"), *TrimmedBoneName));
			}
			return;
		}
		if (SkeletalComponent->GetBoneIndex(FName(*TrimmedBoneName)) == INDEX_NONE)
		{
			if (bExplicit)
			{
				OutWarnings.Add(FString::Printf(TEXT("watched_bone_not_found:%s"), *TrimmedBoneName));
			}
			return;
		}
		OutBoneNames.AddUnique(TrimmedBoneName);
	}

	static void ResolveProbeWatchedBones(
		const TSharedPtr<FJsonObject>& Params,
		USkeletalMeshComponent* SkeletalComponent,
		const TArray<FProbeIKGoalBaselineControl>& BaselineControls,
		TArray<FString>& OutBoneNames,
		TArray<FString>& OutWarnings)
	{
		OutBoneNames.Reset();
		TArray<FString> ExplicitBoneNames;
		AppendProbeStringArrayField(Params, TEXT("watched_bones"), ExplicitBoneNames);
		AppendProbeStringArrayField(Params, TEXT("probe_bones"), ExplicitBoneNames);
		AppendProbeStringArrayField(Params, TEXT("diagnostic_bones"), ExplicitBoneNames);
		for (const FString& BoneName : ExplicitBoneNames)
		{
			AddProbeWatchedBoneIfPresent(SkeletalComponent, BoneName, true, OutBoneNames, OutWarnings);
		}

		for (const FProbeIKGoalBaselineControl& BaselineControl : BaselineControls)
		{
			AddProbeWatchedBoneIfPresent(SkeletalComponent, BaselineControl.BoneName, false, OutBoneNames, OutWarnings);
		}

		const FString DefaultBones[] = {
			TEXT("RightHand"),
			TEXT("RightForeArm"),
			TEXT("RightArm"),
			TEXT("LeftHand"),
			TEXT("LeftForeArm"),
			TEXT("LeftArm"),
			TEXT("Spine2"),
			TEXT("Spine1"),
			TEXT("Hips"),
			TEXT("Head"),
			TEXT("RightFoot"),
			TEXT("LeftFoot")
		};
		for (const FString& DefaultBone : DefaultBones)
		{
			AddProbeWatchedBoneIfPresent(SkeletalComponent, DefaultBone, false, OutBoneNames, OutWarnings);
		}
		if (OutBoneNames.Num() <= 0)
		{
			OutWarnings.Add(SkeletalComponent ? TEXT("no_watched_bones_resolved") : TEXT("watched_bones_skeletal_mesh_component_not_found"));
		}
	}

	static bool SampleProbeBoneTransforms(
		const TArray<UObject*>& BoundObjects,
		const TArray<FString>& BoneNames,
		TArray<FProbeSequenceBoneSample>& OutSamples,
		TArray<FString>& OutWarnings)
	{
		OutSamples.Reset();
		USkeletalMeshComponent* SkeletalComponent = nullptr;
		for (UObject* BoundObject : BoundObjects)
		{
			SkeletalComponent = FindProbeSkeletalMeshComponent(BoundObject);
			if (SkeletalComponent)
			{
				break;
			}
		}
		if (!SkeletalComponent)
		{
			OutWarnings.Add(TEXT("sample_bones_skeletal_mesh_component_not_found"));
			return false;
		}

		for (const FString& BoneNameText : BoneNames)
		{
			const FName BoneName(*BoneNameText);
			if (SkeletalComponent->GetBoneIndex(BoneName) == INDEX_NONE)
			{
				OutWarnings.Add(FString::Printf(TEXT("sample_bone_not_found:%s"), *BoneNameText));
				continue;
			}
			FProbeSequenceBoneSample Sample;
			Sample.BoneName = BoneNameText;
			Sample.ComponentTransform = SkeletalComponent->GetBoneTransform(BoneName, RTS_Component);
			OutSamples.Add(Sample);
		}
		return OutSamples.Num() > 0;
	}

	static TSharedPtr<FJsonObject> BuildProbeBoneSampleJson(const FProbeSequenceBoneSample& Sample)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("bone_name"), Sample.BoneName);
		Obj->SetObjectField(TEXT("component_transform"), UeAgentIKAssetOps::BuildTransformJson(Sample.ComponentTransform));
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildProbeBoneSamplesJson(const TArray<FProbeSequenceBoneSample>& Samples)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FProbeSequenceBoneSample& Sample : Samples)
		{
			Result.Add(MakeShared<FJsonValueObject>(BuildProbeBoneSampleJson(Sample)));
		}
		return Result;
	}

	static const FProbeSequenceBoneSample* FindProbeBoneSample(const TArray<FProbeSequenceBoneSample>& Samples, const FString& BoneName)
	{
		return Samples.FindByPredicate([&BoneName](const FProbeSequenceBoneSample& Sample)
		{
			return Sample.BoneName == BoneName;
		});
	}

	static double ComputeProbeMaxBoneLocationDelta(
		const TArray<FProbeSequenceBoneSample>& BaseSamples,
		const TArray<FProbeSequenceBoneSample>& CandidateSamples)
	{
		double MaxDelta = 0.0;
		for (const FProbeSequenceBoneSample& CandidateSample : CandidateSamples)
		{
			if (const FProbeSequenceBoneSample* BaseSample = FindProbeBoneSample(BaseSamples, CandidateSample.BoneName))
			{
				MaxDelta = FMath::Max(MaxDelta, FVector::Distance(
					BaseSample->ComponentTransform.GetLocation(),
					CandidateSample.ComponentTransform.GetLocation()));
			}
		}
		return MaxDelta;
	}

	static double ComputeProbeMeanPixelDiff(const TArray<FColor>& BasePixels, const TArray<FColor>& CandidatePixels)
	{
		if (BasePixels.Num() <= 0 || BasePixels.Num() != CandidatePixels.Num())
		{
			return 0.0;
		}
		double Sum = 0.0;
		for (int32 Index = 0; Index < BasePixels.Num(); ++Index)
		{
			const FColor& A = BasePixels[Index];
			const FColor& B = CandidatePixels[Index];
			Sum += (FMath::Abs(static_cast<int32>(A.R) - static_cast<int32>(B.R))
				+ FMath::Abs(static_cast<int32>(A.G) - static_cast<int32>(B.G))
				+ FMath::Abs(static_cast<int32>(A.B) - static_cast<int32>(B.B))) / 3.0;
		}
		return Sum / static_cast<double>(BasePixels.Num());
	}

	static TSharedPtr<FJsonObject> BuildProbeBoxJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Box.IsValid != 0);
		if (Box.IsValid)
		{
			Obj->SetObjectField(TEXT("min"), UeAgentIKAssetOps::BuildVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), UeAgentIKAssetOps::BuildVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), UeAgentIKAssetOps::BuildVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), UeAgentIKAssetOps::BuildVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	static bool CaptureProbeBoundObjectsFramePixels(
		UWorld* World,
		const TArray<UObject*>& BoundObjects,
		const FBox& Bounds,
		const FIntPoint FrameSize,
		const double BoundsPadding,
		TArray<FColor>& OutPixels,
		FString& OutCameraSource,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("invalid_sequence_capture_context");
			return false;
		}
		if (!Bounds.IsValid)
		{
			OutError = TEXT("sequence_binding_bounds_invalid");
			return false;
		}

		struct FProbeSequenceCaptureCleanup
		{
			UWorld* World = nullptr;
			ASceneCapture2D* CaptureActor = nullptr;
			APointLight* KeyLightActor = nullptr;
			APointLight* FillLightActor = nullptr;
			UTextureRenderTarget2D* RenderTarget = nullptr;

			~FProbeSequenceCaptureCleanup()
			{
				if (CaptureActor && IsValid(CaptureActor))
				{
					CaptureActor->Destroy();
				}
				if (KeyLightActor && IsValid(KeyLightActor))
				{
					KeyLightActor->Destroy();
				}
				if (FillLightActor && IsValid(FillLightActor))
				{
					FillLightActor->Destroy();
				}
			}
		};

		FProbeSequenceCaptureCleanup Cleanup;
		Cleanup.World = World;

		const int32 Pad = 18;
		const int32 Gap = 18;
		const FIntPoint SubviewSize(
			FMath::Max(64, (FrameSize.X - Pad * 2 - Gap) / 2),
			FMath::Max(64, FrameSize.Y - Pad * 2));
		Cleanup.RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!Cleanup.RenderTarget)
		{
			OutError = TEXT("render_target_alloc_failed");
			return false;
		}
		Cleanup.RenderTarget->ClearColor = FLinearColor::Black;
		Cleanup.RenderTarget->InitCustomFormat(SubviewSize.X, SubviewSize.Y, PF_B8G8R8A8, false);
		Cleanup.RenderTarget->UpdateResourceImmediate(true);

		FActorSpawnParameters CaptureSpawnParams;
		CaptureSpawnParams.Name = MakeUniqueObjectName(World, ASceneCapture2D::StaticClass(), TEXT("UeAgentProbeControlRigCapture"));
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
		CaptureComp->ProjectionType = ECameraProjectionMode::Orthographic;
		CaptureComp->bAutoCalculateOrthoPlanes = true;
		CaptureComp->bUpdateOrthoPlanes = true;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		for (UObject* Object : BoundObjects)
		{
			if (AActor* Actor = Cast<AActor>(Object))
			{
				CaptureComp->ShowOnlyActorComponents(Actor, true);
			}
			else if (UPrimitiveComponent* SceneComponent = Cast<UPrimitiveComponent>(Object))
			{
				CaptureComp->ShowOnlyComponent(SceneComponent);
			}
		}
		CaptureComp->ShowFlags.SetAtmosphere(false);
		CaptureComp->ShowFlags.SetFog(false);
		CaptureComp->ShowFlags.SetEyeAdaptation(false);
		CaptureComp->ShowFlags.SetMotionBlur(false);
		CaptureComp->ShowFlags.SetScreenSpaceReflections(false);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;

		auto SpawnProbePointLight = [&](const TCHAR* BaseName, const float IntensityScale) -> APointLight*
		{
			FActorSpawnParameters LightSpawnParams;
			LightSpawnParams.Name = MakeUniqueObjectName(World, APointLight::StaticClass(), BaseName);
			LightSpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
			LightSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			LightSpawnParams.bTemporaryEditorActor = true;
			LightSpawnParams.bHideFromSceneOutliner = true;
			APointLight* LightActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), FTransform::Identity, LightSpawnParams);
			if (!LightActor || !LightActor->PointLightComponent)
			{
				return nullptr;
			}
			LightActor->SetActorEnableCollision(false);
			LightActor->PointLightComponent->SetCastShadows(false);
			LightActor->PointLightComponent->SetIntensity(45000.0f * IntensityScale);
			LightActor->PointLightComponent->SetAttenuationRadius(4000.0f);
			return LightActor;
		};
		Cleanup.KeyLightActor = SpawnProbePointLight(TEXT("UeAgentProbeControlRigKeyLight"), 1.0f);
		Cleanup.FillLightActor = SpawnProbePointLight(TEXT("UeAgentProbeControlRigFillLight"), 0.35f);

		auto CaptureSubview = [&](const FVector& Direction, TArray<FColor>& OutSubviewPixels) -> bool
		{
			const FVector SafeDirection = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
			const FVector Origin = Bounds.GetCenter();
			const FVector Extent = Bounds.GetExtent();
			const bool bFrontView = FMath::Abs(SafeDirection.Y) >= FMath::Abs(SafeDirection.X);
			const float HorizontalExtent = FMath::Max(bFrontView ? Extent.X : Extent.Y, 1.0f);
			const float VerticalExtent = FMath::Max(Extent.Z, 1.0f);
			const float DepthExtent = FMath::Max(bFrontView ? Extent.Y : Extent.X, 1.0f);
			const double AspectRatio = static_cast<double>(SubviewSize.X) / static_cast<double>(SubviewSize.Y);
			const double RequiredOrthoWidth = FMath::Max(
				static_cast<double>(HorizontalExtent) * 2.0,
				(static_cast<double>(VerticalExtent) * 2.0) * AspectRatio);
			CaptureComp->OrthoWidth = static_cast<float>(FMath::Max(RequiredOrthoWidth * BoundsPadding, 10.0));
			const float CaptureDistance = FMath::Max(static_cast<float>(CaptureComp->OrthoWidth) + DepthExtent * 4.0f + 100.0f, 250.0f);
			const FVector CaptureLocation = Origin - SafeDirection * CaptureDistance;
			Cleanup.CaptureActor->SetActorLocationAndRotation(CaptureLocation, (Origin - CaptureLocation).Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
			if (Cleanup.KeyLightActor && IsValid(Cleanup.KeyLightActor) && Cleanup.KeyLightActor->PointLightComponent)
			{
				Cleanup.KeyLightActor->SetActorLocation(CaptureLocation + FVector(0.0f, 0.0f, CaptureComp->OrthoWidth * 0.2f), false, nullptr, ETeleportType::TeleportPhysics);
				Cleanup.KeyLightActor->PointLightComponent->SetAttenuationRadius(FMath::Max(4000.0f, CaptureComp->OrthoWidth * 5.0f));
			}
			if (Cleanup.FillLightActor && IsValid(Cleanup.FillLightActor) && Cleanup.FillLightActor->PointLightComponent)
			{
				Cleanup.FillLightActor->SetActorLocation(Origin + FVector(0.0f, 0.0f, CaptureComp->OrthoWidth * 1.5f), false, nullptr, ETeleportType::TeleportPhysics);
				Cleanup.FillLightActor->PointLightComponent->SetAttenuationRadius(FMath::Max(4000.0f, CaptureComp->OrthoWidth * 6.0f));
			}
			World->SendAllEndOfFrameUpdates();
			CaptureComp->CaptureScene();
			FlushRenderingCommands();
			FRenderTarget* RenderTarget = Cleanup.RenderTarget->GameThread_GetRenderTargetResource();
			if (!RenderTarget || !RenderTarget->ReadPixels(OutSubviewPixels))
			{
				OutError = TEXT("read_render_target_failed");
				return false;
			}
			for (FColor& Pixel : OutSubviewPixels)
			{
				Pixel.A = 255;
			}
			return true;
		};

		TArray<FColor> FrontPixels;
		TArray<FColor> SidePixels;
		if (!CaptureSubview(FVector(0.0f, 1.0f, 0.0f), FrontPixels))
		{
			return false;
		}
		if (!CaptureSubview(FVector(1.0f, 0.0f, 0.0f), SidePixels))
		{
			return false;
		}

		OutPixels.Init(FColor(22, 22, 22, 255), FrameSize.X * FrameSize.Y);
		auto PutPixel = [&](const int32 X, const int32 Y, const FColor& Color)
		{
			if (X >= 0 && X < FrameSize.X && Y >= 0 && Y < FrameSize.Y)
			{
				OutPixels[Y * FrameSize.X + X] = Color;
			}
		};
		auto Blit = [&](const TArray<FColor>& Src, const int32 DstX, const int32 DstY)
		{
			for (int32 Y = 0; Y < SubviewSize.Y; ++Y)
			{
				for (int32 X = 0; X < SubviewSize.X; ++X)
				{
					const int32 SrcIndex = Y * SubviewSize.X + X;
					if (Src.IsValidIndex(SrcIndex))
					{
						PutPixel(DstX + X, DstY + Y, Src[SrcIndex]);
					}
				}
			}
		};
		Blit(FrontPixels, Pad, Pad);
		Blit(SidePixels, Pad + SubviewSize.X + Gap, Pad);
		OutCameraSource = TEXT("union_bound_object_aabb_front_side");
		return true;
	}

	static bool SelectProbeCachedFrame(
		const FUeAgentRequestContext& Ctx,
		const FProbeIKGoalCandidateCacheEntry& CacheEntry,
		const FProbeIKGoalCachedFrame*& OutSelectedFrame,
		FString& OutSelectionSemantics,
		FString& OutError)
	{
		OutSelectedFrame = nullptr;
		OutSelectionSemantics.Reset();

		int32 RequestedFrameIndex = INDEX_NONE;
		bool bUseFrameIndex = TryGetIntParam(Ctx.Params, TEXT("frame_index"), RequestedFrameIndex)
			|| TryGetIntParam(Ctx.Params, TEXT("frame_number"), RequestedFrameIndex)
			|| TryGetIntParam(Ctx.Params, TEXT("number"), RequestedFrameIndex)
			|| TryGetIntParam(Ctx.Params, TEXT("index"), RequestedFrameIndex)
			|| TryGetIntParam(Ctx.Params, TEXT("selection"), RequestedFrameIndex);

		int32 RequestedCandidateIndex = INDEX_NONE;
		const bool bUseCandidateIndex = !bUseFrameIndex && (
			TryGetIntParam(Ctx.Params, TEXT("candidate_index"), RequestedCandidateIndex)
			|| TryGetIntParam(Ctx.Params, TEXT("candidate_number"), RequestedCandidateIndex));

		if (!bUseFrameIndex && !bUseCandidateIndex)
		{
			OutError = TEXT("missing_frame_index_or_candidate_index");
			return false;
		}

		for (const FProbeIKGoalCachedFrame& Frame : CacheEntry.Frames)
		{
			if ((bUseFrameIndex && Frame.FrameIndex == RequestedFrameIndex)
				|| (bUseCandidateIndex && Frame.CandidateIndex == RequestedCandidateIndex))
			{
				OutSelectedFrame = &Frame;
				break;
			}
		}
		if (!OutSelectedFrame)
		{
			OutError = bUseFrameIndex ? TEXT("frame_index_not_found_in_probe_cache") : TEXT("candidate_index_not_found_in_probe_cache");
			return false;
		}

		OutSelectionSemantics = bUseFrameIndex ? TEXT("frame_index_matches_sheet_label") : TEXT("candidate_index_zero_based");
		return true;
	}

	static bool CmdProbeIKGoalApplyCandidateSequenceControlRig(
		const FUeAgentRequestContext& Ctx,
		const FString& ProbeRequestId,
		const FProbeIKGoalCandidateCacheEntry& CacheEntry,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}

		FString SequencePathOverride;
		if (ProbeJsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePathOverride) && !SequencePathOverride.TrimStartAndEnd().IsEmpty())
		{
			if (UeAgentIKAssetOps::NormalizeAssetPath(SequencePathOverride) != UeAgentIKAssetOps::NormalizeAssetPath(CacheEntry.SequencePath))
			{
				OutError = TEXT("probe_candidate_sequence_mismatch");
				return false;
			}
		}

		FString ControlNameOverride;
		if ((ProbeJsonTryGetString(Ctx.Params, TEXT("control_name"), ControlNameOverride)
				|| ProbeJsonTryGetString(Ctx.Params, TEXT("goal_name"), ControlNameOverride))
			&& !ControlNameOverride.TrimStartAndEnd().IsEmpty())
		{
			if (ControlNameOverride.TrimStartAndEnd() != CacheEntry.ControlName)
			{
				OutError = TEXT("probe_candidate_control_mismatch");
				return false;
			}
		}

		const FProbeIKGoalCachedFrame* SelectedFrame = nullptr;
		FString SelectionSemantics;
		if (!SelectProbeCachedFrame(Ctx, CacheEntry, SelectedFrame, SelectionSemantics, OutError) || !SelectedFrame)
		{
			return false;
		}
		if (SelectedFrame->Kind == TEXT("original"))
		{
			OutError = TEXT("cannot_apply_original_probe_frame");
			return false;
		}

		ULevelSequence* Sequence = LoadProbeLevelSequence(CacheEntry.SequencePath);
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

		FGuid BindingGuid;
		if (!FGuid::Parse(CacheEntry.BindingGuid, BindingGuid) || !MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("cached_binding_guid_not_found");
			return false;
		}

		const FName ControlName(*CacheEntry.ControlName);
		UMovieSceneControlRigParameterTrack* ControlRigTrack = nullptr;
		UMovieSceneControlRigParameterSection* ControlRigSection = nullptr;
		UControlRig* ControlRig = nullptr;
		if (!TryFindProbeControlRigTrack(MovieScene, BindingGuid, ControlName, FString(), CacheEntry.EvaluationFrame, true, ControlRigTrack, ControlRigSection, ControlRig, OutError))
		{
			return false;
		}

		FRigControlElement* ControlElement = ControlRig ? ControlRig->FindControl(ControlName) : nullptr;
		if (!ControlElement)
		{
			OutError = TEXT("control_rig_control_not_found");
			return false;
		}
		const ERigControlType ControlType = ControlElement->Settings.ControlType;
		if (!IsProbeTransformControlType(ControlType))
		{
			OutError = TEXT("control_rig_control_type_not_supported");
			return false;
		}

		FTransform PreviousControlTransform = FTransform::Identity;
		if (!TryEvaluateProbeControlTransform(ControlRig, ControlRigSection, ControlName, ControlType, CacheEntry.EvaluationFrame, PreviousControlTransform, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("evaluate_previous_control_transform_failed");
			}
			return false;
		}

		const int32 IgnoredCachedBaselineControlCount = CacheEntry.BaselineControls.Num();
		const bool bAlreadyApplied = PreviousControlTransform.Equals(SelectedFrame->ControlTransform, 0.001f);
		if (!bAlreadyApplied)
		{
			Sequence->Modify();
			if (ControlRigTrack)
			{
				ControlRigTrack->Modify();
			}
			if (!ApplyProbeControlTransformKey(ControlRigSection, ControlRig, ControlName, ControlType, CacheEntry.EvaluationFrame, SelectedFrame->ControlTransform, OutError))
			{
				return false;
			}
			MovieScene->Modify();
		}

		FTransform FinalControlTransform = FTransform::Identity;
		if (!TryEvaluateProbeControlTransform(ControlRig, ControlRigSection, ControlName, ControlType, CacheEntry.EvaluationFrame, FinalControlTransform, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("evaluate_final_control_transform_failed");
			}
			return false;
		}
		const bool bAppliedMatches = FinalControlTransform.Equals(SelectedFrame->ControlTransform, 0.001f);
		if (!bAppliedMatches)
		{
			OutError = TEXT("applied_control_transform_mismatch");
			return false;
		}

		bool bSaveAfterApply = false;
		ProbeJsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		ProbeJsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentIKAssetOps::SaveAssetPackage(Sequence, OutError))
		{
			return false;
		}

		OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.ik_goal_apply_candidate.sequence_control_rig.v1"));
		OutData->SetStringField(TEXT("context_mode"), TEXT("sequence_control_rig"));
		OutData->SetStringField(TEXT("probe_request_id"), ProbeRequestId);
		OutData->SetStringField(TEXT("sequence_path"), CacheEntry.SequencePath);
		OutData->SetStringField(TEXT("sequence_object_path"), Sequence->GetPathName());
		OutData->SetStringField(TEXT("binding_guid"), CacheEntry.BindingGuid);
		OutData->SetStringField(TEXT("binding_name"), CacheEntry.BindingName);
		OutData->SetStringField(TEXT("control_name"), CacheEntry.ControlName);
		OutData->SetStringField(TEXT("goal_name"), CacheEntry.ControlName);
		OutData->SetStringField(TEXT("control_type"), ProbeControlTypeToString(ControlType));
		OutData->SetStringField(TEXT("control_rig_class"), ControlRig ? ControlRig->GetClass()->GetPathName() : CacheEntry.ControlRigClass);
		OutData->SetNumberField(TEXT("sequence_frame_index"), CacheEntry.SequenceFrameIndex);
		OutData->SetNumberField(TEXT("sequence_time_seconds"), CacheEntry.SequenceTimeSeconds);
		OutData->SetNumberField(TEXT("evaluation_frame"), CacheEntry.EvaluationFrame.Value);
		OutData->SetNumberField(TEXT("applied_frame_index"), SelectedFrame->FrameIndex);
		OutData->SetStringField(TEXT("applied_frame_label"), SelectedFrame->Label);
		OutData->SetStringField(TEXT("applied_kind"), SelectedFrame->Kind);
		OutData->SetNumberField(TEXT("applied_candidate_index"), SelectedFrame->CandidateIndex);
		OutData->SetStringField(TEXT("transform_mode"), SelectedFrame->TransformMode);
		OutData->SetObjectField(TEXT("original_control_transform"), UeAgentIKAssetOps::BuildTransformJson(CacheEntry.OriginalControlTransform));
		OutData->SetObjectField(TEXT("previous_control_transform"), UeAgentIKAssetOps::BuildTransformJson(PreviousControlTransform));
		OutData->SetObjectField(TEXT("applied_control_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->ControlTransform));
		OutData->SetObjectField(TEXT("final_control_transform"), UeAgentIKAssetOps::BuildTransformJson(FinalControlTransform));
		OutData->SetStringField(TEXT("baseline_mode"), CacheEntry.BaselineMode);
		OutData->SetNumberField(TEXT("baseline_control_count"), 0);
		OutData->SetArrayField(TEXT("baseline_controls"), TArray<TSharedPtr<FJsonValue>>());
		OutData->SetBoolField(TEXT("baseline_controls_applied"), false);
		OutData->SetNumberField(TEXT("ignored_cached_baseline_control_count"), IgnoredCachedBaselineControlCount);
		if (SelectedFrame->bHasDeltaTransform)
		{
			OutData->SetObjectField(TEXT("delta_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->DeltaTransform));
		}
		OutData->SetBoolField(TEXT("already_applied"), bAlreadyApplied);
		OutData->SetBoolField(TEXT("applied_matches_selected"), bAppliedMatches);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
		OutData->SetNumberField(TEXT("cached_frame_count"), CacheEntry.Frames.Num());
		OutData->SetStringField(TEXT("selection_semantics"), SelectionSemantics);
		return true;
	}
}

bool FUeAgentHttpServer::CmdProbeIKGoalCandidates(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::IKProbeCandidates,
		UnifiedOptionsReport);

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString SequencePath;
	if (!JsonTryGetString(Ctx.Params, TEXT("sequence_path"), SequencePath) || SequencePath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_sequence_path");
		return false;
	}
	SequencePath = SequencePath.TrimStartAndEnd();
	ULevelSequence* Sequence = LoadProbeLevelSequence(SequencePath);
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

	FString ControlNameText;
	if ((!JsonTryGetString(Ctx.Params, TEXT("control_name"), ControlNameText)
			&& !JsonTryGetString(Ctx.Params, TEXT("goal_name"), ControlNameText))
		|| ControlNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_control_name");
		return false;
	}
	ControlNameText.TrimStartAndEndInline();
	const FName ControlName(*ControlNameText);

	const TArray<TSharedPtr<FJsonValue>>* CandidateValues = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("candidates"), CandidateValues) || !CandidateValues)
	{
		OutError = TEXT("missing_candidates");
		return false;
	}
	if (CandidateValues->Num() <= 0)
	{
		OutError = TEXT("empty_candidates");
		return false;
	}
	if (CandidateValues->Num() > 64)
	{
		OutError = TEXT("too_many_candidates");
		return false;
	}

	FFrameNumber EvaluationFrame;
	int32 SequenceFrameIndex = 0;
	double SequenceTimeSeconds = 0.0;
	if (!ResolveProbeSequenceFrame(MovieScene, Ctx.Params, EvaluationFrame, SequenceFrameIndex, SequenceTimeSeconds, OutError))
	{
		return false;
	}

	FGuid BindingGuid;
	FString BindingName;
	if (!TryResolveProbeBindingGuid(MovieScene, Ctx.Params, ControlNameText, BindingGuid, BindingName, OutError))
	{
		return false;
	}

	FString TrackNameFilter;
	JsonTryGetString(Ctx.Params, TEXT("track_name"), TrackNameFilter);
	TrackNameFilter.TrimStartAndEndInline();

	UMovieSceneControlRigParameterTrack* ControlRigTrack = nullptr;
	UMovieSceneControlRigParameterSection* ControlRigSection = nullptr;
	UControlRig* ControlRig = nullptr;
	if (!TryFindProbeControlRigTrack(MovieScene, BindingGuid, ControlName, TrackNameFilter, EvaluationFrame, false, ControlRigTrack, ControlRigSection, ControlRig, OutError))
	{
		return false;
	}

	FRigControlElement* ControlElement = ControlRig ? ControlRig->FindControl(ControlName) : nullptr;
	if (!ControlElement)
	{
		OutError = TEXT("control_rig_control_not_found");
		return false;
	}
	const ERigControlType ControlType = ControlElement->Settings.ControlType;
	if (!IsProbeTransformControlType(ControlType))
	{
		OutError = TEXT("control_rig_control_type_not_supported");
		return false;
	}

	TArray<FProbeIKGoalBaselineControl> BaselineControls;
	if (!ParseProbeBaselineControlBoneMap(Ctx.Params, ControlRig, BaselineControls, OutError))
	{
		return false;
	}

	int32 FrameWidth = 1920;
	int32 FrameHeight = 1280;
	double NumberValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("frame_width"), NumberValue) || JsonTryGetNumber(Ctx.Params, TEXT("width"), NumberValue))
	{
		FrameWidth = FMath::Clamp(FMath::RoundToInt(NumberValue), 128, 4096);
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("frame_height"), NumberValue) || JsonTryGetNumber(Ctx.Params, TEXT("height"), NumberValue))
	{
		FrameHeight = FMath::Clamp(FMath::RoundToInt(NumberValue), 128, 4096);
	}
	const FIntPoint FrameSize(FrameWidth, FrameHeight);

	double BoundsPadding = 1.35;
	if (JsonTryGetNumber(Ctx.Params, TEXT("mesh_capture_bounds_padding"), BoundsPadding)
		|| JsonTryGetNumber(Ctx.Params, TEXT("bounds_padding"), BoundsPadding))
	{
		BoundsPadding = FMath::Clamp(BoundsPadding, 1.0, 4.0);
	}

	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.TrimStartAndEnd().ToLower();
	if (Format.IsEmpty())
	{
		Format = TEXT("png");
	}
	if (Format == TEXT("jpg"))
	{
		Format = TEXT("jpeg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpeg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_image_format");
		return false;
	}

	int32 Quality = 95;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), NumberValue))
	{
		Quality = FMath::Clamp(FMath::RoundToInt(NumberValue), 1, 100);
	}

	bool bCreateFrameSheet = true;
	ProbeJsonTryGetBool(Ctx.Params, TEXT("create_frame_sheet"), bCreateFrameSheet);
	bool bWriteIndividualFrames = true;
	ProbeJsonTryGetBool(Ctx.Params, TEXT("write_individual_frames"), bWriteIndividualFrames);

	struct FProbeLiveSequenceCleanup
	{
		UWorld* World = nullptr;
		ULevelSequencePlayer* Player = nullptr;
		ALevelSequenceActor* SequenceActor = nullptr;

		~FProbeLiveSequenceCleanup()
		{
			if (Player)
			{
				Player->Stop();
			}
			if (World && SequenceActor && IsValid(SequenceActor))
			{
				World->DestroyActor(SequenceActor, false, false);
			}
		}
	};

	FString PrebindError;
	const bool bPreboundControlRig = RebindProbeControlRigToSequenceObject(Sequence, BindingGuid, ControlRig, World, PrebindError);

	FProbeLiveSequenceCleanup Cleanup;
	Cleanup.World = World;
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

	const FFrameTime PlaybackFrame = ConvertProbeEvaluationTickToPlaybackFrame(MovieScene, EvaluationFrame);
	Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(PlaybackFrame, EUpdatePositionMethod::Jump));

	if (!bPreboundControlRig)
	{
		if (!RebindProbeControlRigToSequenceObject(Sequence, BindingGuid, ControlRig, World, OutError))
		{
			OutError = FString::Printf(TEXT("control_rig_rebind_failed: pre=%s post=%s"), *PrebindError, *OutError);
			return false;
		}
		Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(PlaybackFrame, EUpdatePositionMethod::Jump));
	}

	TArray<UObject*> BoundObjects;
	if (!ResolveProbeBoundObjects(Sequence, BindingGuid, World, BoundObjects))
	{
		OutError = TEXT("sequence_binding_bound_objects_not_found");
		return false;
	}

	RefreshProbeSkeletalBoundObjects(BoundObjects);
	World->SendAllEndOfFrameUpdates();
	FlushRenderingCommands();

	USkeletalMeshComponent* SkeletalComponent = nullptr;
	for (UObject* BoundObject : BoundObjects)
	{
		SkeletalComponent = FindProbeSkeletalMeshComponent(BoundObject);
		if (SkeletalComponent)
		{
			break;
		}
	}

	TArray<FString> Warnings;
	if (!bPreboundControlRig)
	{
		Warnings.Add(FString::Printf(TEXT("control_rig_prebind_failed_then_rebound_after_player:%s"), *PrebindError));
	}

	TArray<FString> WatchedBoneNames;
	ResolveProbeWatchedBones(Ctx.Params, SkeletalComponent, BaselineControls, WatchedBoneNames, Warnings);

	const FTransform OriginalControlTransform = ControlRig->GetControlLocalTransform(ControlName);

	struct FProbeRestoreCurrentControl
	{
		UWorld* World = nullptr;
		UControlRig* ControlRig = nullptr;
		FName ControlName = NAME_None;
		FTransform OriginalTransform = FTransform::Identity;
		TArray<UObject*> BoundObjects;
		bool bEnabled = false;

		void RestoreNow()
		{
			if (!bEnabled || !ControlRig)
			{
				return;
			}
			const FRigControlModifiedContext NoKeyContext(EControlRigSetKey::Never);
			ControlRig->SetControlLocalTransform(ControlName, OriginalTransform, true, NoKeyContext, false, true);
			ControlRig->Evaluate_AnyThread();
			RefreshProbeSkeletalBoundObjects(BoundObjects);
			if (World)
			{
				World->SendAllEndOfFrameUpdates();
			}
			FlushRenderingCommands();
			bEnabled = false;
		}

		~FProbeRestoreCurrentControl()
		{
			RestoreNow();
		}
	};

	FProbeRestoreCurrentControl RestoreScope;
	RestoreScope.World = World;
	RestoreScope.ControlRig = ControlRig;
	RestoreScope.ControlName = ControlName;
	RestoreScope.OriginalTransform = OriginalControlTransform;
	RestoreScope.BoundObjects = BoundObjects;
	RestoreScope.bEnabled = true;

	struct FProbeCandidateFrameState
	{
		int32 FrameIndex = INDEX_NONE;
		int32 CandidateIndex = INDEX_NONE;
		FString Label;
		FString Kind;
		FString CandidateLabel;
		FString TransformMode;
		FTransform RequestedControlTransform = FTransform::Identity;
		FTransform AppliedControlTransform = FTransform::Identity;
		FTransform DeltaTransform = FTransform::Identity;
		bool bHasDeltaTransform = false;
		FBox Bounds = FBox(ForceInit);
		TArray<FProbeSequenceBoneSample> BoneSamples;
		TArray<FColor> Pixels;
		double MaxBoneLocationDeltaFromF0 = 0.0;
		double MeanPixelDiffFromF0 = 0.0;
		bool bReadbackMatchesRequest = true;
	};

	auto ApplyControlState = [&](const FTransform& ControlTransform, FTransform& OutAppliedTransform) -> bool
	{
		const FRigControlModifiedContext NoKeyContext(EControlRigSetKey::Never);
		ControlRig->SetControlLocalTransform(ControlName, ControlTransform, true, NoKeyContext, false, true);
		ControlRig->Evaluate_AnyThread();
		RefreshProbeSkeletalBoundObjects(BoundObjects);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();
		OutAppliedTransform = ControlRig->GetControlLocalTransform(ControlName);
		return true;
	};

	TArray<FProbeCandidateFrameState> Frames;
	FProbeCandidateFrameState OriginalFrame;
	OriginalFrame.FrameIndex = 0;
	OriginalFrame.CandidateIndex = INDEX_NONE;
	OriginalFrame.Label = TEXT("F0");
	OriginalFrame.Kind = TEXT("original");
	OriginalFrame.CandidateLabel = TEXT("original");
	OriginalFrame.TransformMode = TEXT("current_sequence_frame_control_transform");
	OriginalFrame.RequestedControlTransform = OriginalControlTransform;
	OriginalFrame.AppliedControlTransform = OriginalControlTransform;
	Frames.Add(OriginalFrame);

	for (int32 CandidateIndex = 0; CandidateIndex < CandidateValues->Num(); ++CandidateIndex)
	{
		const TSharedPtr<FJsonValue>& CandidateValue = (*CandidateValues)[CandidateIndex];
		const TSharedPtr<FJsonObject>* CandidateObjPtr = nullptr;
		if (!CandidateValue.IsValid() || !CandidateValue->TryGetObject(CandidateObjPtr) || !CandidateObjPtr || !CandidateObjPtr->IsValid())
		{
			OutError = TEXT("invalid_candidate");
			return false;
		}
		const TSharedPtr<FJsonObject> CandidateObj = *CandidateObjPtr;

		FProbeCandidateFrameState Frame;
		Frame.FrameIndex = Frames.Num();
		Frame.CandidateIndex = CandidateIndex;
		Frame.Label = FString::Printf(TEXT("F%d"), Frame.FrameIndex);
		Frame.Kind = TEXT("candidate");
		CandidateObj->TryGetStringField(TEXT("label"), Frame.CandidateLabel);
		if (Frame.CandidateLabel.TrimStartAndEnd().IsEmpty())
		{
			Frame.CandidateLabel = FString::Printf(TEXT("candidate_%d"), CandidateIndex);
		}
		if (!TryBuildProbeCandidateControlTransform(CandidateObj, OriginalControlTransform, Frame.RequestedControlTransform, Frame.DeltaTransform, Frame.TransformMode, OutError))
		{
			OutError = FString::Printf(TEXT("candidate_%d_%s"), CandidateIndex, *OutError);
			return false;
		}
		Frame.bHasDeltaTransform = true;
		Frames.Add(Frame);
	}

	auto EvaluateFramePose = [&](FProbeCandidateFrameState& Frame, const FBox* CaptureBounds) -> bool
	{
		if (!ApplyControlState(Frame.RequestedControlTransform, Frame.AppliedControlTransform))
		{
			OutError = TEXT("apply_control_transform_failed");
			return false;
		}
		Frame.bReadbackMatchesRequest = Frame.AppliedControlTransform.Equals(Frame.RequestedControlTransform, 0.01f);
		if (!Frame.bReadbackMatchesRequest)
		{
			Warnings.Add(FString::Printf(TEXT("control_readback_mismatch:%s"), *Frame.Label));
		}

		Frame.Bounds = BuildProbeBoundObjectsBox(BoundObjects);
		if (!Frame.Bounds.IsValid)
		{
			OutError = FString::Printf(TEXT("probe_frame_bounds_invalid:%s"), *Frame.Label);
			return false;
		}

		TArray<FString> SampleWarnings;
		SampleProbeBoneTransforms(BoundObjects, WatchedBoneNames, Frame.BoneSamples, SampleWarnings);
		for (const FString& Warning : SampleWarnings)
		{
			Warnings.AddUnique(Warning);
		}

		if (CaptureBounds)
		{
			FString CameraSource;
			if (!CaptureProbeBoundObjectsFramePixels(World, BoundObjects, *CaptureBounds, FrameSize, BoundsPadding, Frame.Pixels, CameraSource, OutError))
			{
				OutError = FString::Printf(TEXT("capture_probe_frame_failed:%s:%s"), *Frame.Label, *OutError);
				return false;
			}
		}
		return true;
	};

	FBox UnionBounds(ForceInit);
	for (FProbeCandidateFrameState& Frame : Frames)
	{
		if (!EvaluateFramePose(Frame, nullptr))
		{
			return false;
		}
		UnionBounds += Frame.Bounds;
	}
	if (!UnionBounds.IsValid)
	{
		OutError = TEXT("probe_union_bounds_invalid");
		return false;
	}

	for (FProbeCandidateFrameState& Frame : Frames)
	{
		if (!EvaluateFramePose(Frame, &UnionBounds))
		{
			return false;
		}
	}

	const TArray<FProbeSequenceBoneSample> BaseBoneSamples = Frames[0].BoneSamples;
	const TArray<FColor> BasePixels = Frames[0].Pixels;
	bool bAnyCandidatePoseChanged = false;
	bool bAnyCandidatePixelsChanged = false;
	for (int32 FrameIndex = 1; FrameIndex < Frames.Num(); ++FrameIndex)
	{
		FProbeCandidateFrameState& Frame = Frames[FrameIndex];
		Frame.MaxBoneLocationDeltaFromF0 = ComputeProbeMaxBoneLocationDelta(BaseBoneSamples, Frame.BoneSamples);
		Frame.MeanPixelDiffFromF0 = ComputeProbeMeanPixelDiff(BasePixels, Frame.Pixels);
		bAnyCandidatePoseChanged = bAnyCandidatePoseChanged || Frame.MaxBoneLocationDeltaFromF0 > 0.1;
		bAnyCandidatePixelsChanged = bAnyCandidatePixelsChanged || Frame.MeanPixelDiffFromF0 > 0.25;
	}
	if (!bAnyCandidatePoseChanged)
	{
		Warnings.Add(TEXT("all_candidate_bone_samples_match_f0"));
	}
	if (!bAnyCandidatePixelsChanged)
	{
		Warnings.Add(TEXT("all_candidate_pixels_match_f0"));
	}

	RestoreScope.RestoreNow();
	const FTransform RestoredControlTransform = ControlRig->GetControlLocalTransform(ControlName);
	const bool bRestored = RestoredControlTransform.Equals(OriginalControlTransform, 0.01f);
	if (!bRestored)
	{
		Warnings.Add(TEXT("control_restore_readback_mismatch"));
	}

	TArray<TArray<FColor>> FramePixels;
	TArray<FString> FrameLabels;
	TArray<TSharedPtr<FJsonValue>> FrameResults;
	TArray<TSharedPtr<FJsonValue>> CandidateResults;
	TArray<FProbeIKGoalCachedFrame> CacheFrames;

	auto AddArtifact = [&OutArtifacts, &Format](const FString& Kind, const FString& FilePath, const FIntPoint& Size)
	{
		const int64 Bytes = IFileManager::Get().FileSize(*FilePath);
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("kind"), Kind);
		Artifact->SetStringField(TEXT("file_path"), FilePath);
		Artifact->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
		Artifact->SetNumberField(TEXT("width"), Size.X);
		Artifact->SetNumberField(TEXT("height"), Size.Y);
		OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	};

	for (const FProbeCandidateFrameState& Frame : Frames)
	{
		FramePixels.Add(Frame.Pixels);
		FrameLabels.Add(Frame.Label);

		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("frame_index"), Frame.FrameIndex);
		FrameObj->SetStringField(TEXT("frame_label"), Frame.Label);
		FrameObj->SetStringField(TEXT("kind"), Frame.Kind);
		FrameObj->SetNumberField(TEXT("candidate_index"), Frame.CandidateIndex);
		FrameObj->SetStringField(TEXT("candidate_label"), Frame.CandidateLabel);
		FrameObj->SetStringField(TEXT("transform_mode"), Frame.TransformMode);
		FrameObj->SetObjectField(TEXT("requested_control_transform"), UeAgentIKAssetOps::BuildTransformJson(Frame.RequestedControlTransform));
		FrameObj->SetObjectField(TEXT("applied_control_transform"), UeAgentIKAssetOps::BuildTransformJson(Frame.AppliedControlTransform));
		if (Frame.bHasDeltaTransform)
		{
			FrameObj->SetObjectField(TEXT("delta_transform"), UeAgentIKAssetOps::BuildTransformJson(Frame.DeltaTransform));
		}
		FrameObj->SetBoolField(TEXT("readback_matches_request"), Frame.bReadbackMatchesRequest);
		FrameObj->SetObjectField(TEXT("bounds"), BuildProbeBoxJson(Frame.Bounds));
		FrameObj->SetArrayField(TEXT("bone_samples"), BuildProbeBoneSamplesJson(Frame.BoneSamples));
		FrameObj->SetNumberField(TEXT("max_bone_location_delta_from_f0"), Frame.MaxBoneLocationDeltaFromF0);
		FrameObj->SetNumberField(TEXT("mean_pixel_diff_from_f0"), Frame.MeanPixelDiffFromF0);
		FrameObj->SetNumberField(TEXT("width"), FrameSize.X);
		FrameObj->SetNumberField(TEXT("height"), FrameSize.Y);

		if (bWriteIndividualFrames)
		{
			const FString FramePath = MakeShotPath(Format);
			if (!WriteCompressedImage(Frame.Pixels, FrameSize, Format, Quality, FramePath, OutError))
			{
				return false;
			}
			const int64 Bytes = IFileManager::Get().FileSize(*FramePath);
			FrameObj->SetStringField(TEXT("file_path"), FramePath);
			FrameObj->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
			AddArtifact(TEXT("probe_ik_goal_frame"), FramePath, FrameSize);
		}

		FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));

		if (Frame.Kind == TEXT("candidate"))
		{
			TSharedPtr<FJsonObject> CandidateObj = MakeShared<FJsonObject>();
			CandidateObj->SetNumberField(TEXT("candidate_index"), Frame.CandidateIndex);
			CandidateObj->SetStringField(TEXT("label"), Frame.CandidateLabel);
			CandidateObj->SetNumberField(TEXT("frame_index"), Frame.FrameIndex);
			CandidateObj->SetStringField(TEXT("frame_label"), Frame.Label);
			CandidateObj->SetStringField(TEXT("transform_mode"), Frame.TransformMode);
			CandidateObj->SetObjectField(TEXT("control_transform"), UeAgentIKAssetOps::BuildTransformJson(Frame.AppliedControlTransform));
			CandidateObj->SetObjectField(TEXT("delta_transform"), UeAgentIKAssetOps::BuildTransformJson(Frame.DeltaTransform));
			CandidateObj->SetNumberField(TEXT("max_bone_location_delta_from_f0"), Frame.MaxBoneLocationDeltaFromF0);
			CandidateObj->SetNumberField(TEXT("mean_pixel_diff_from_f0"), Frame.MeanPixelDiffFromF0);
			CandidateResults.Add(MakeShared<FJsonValueObject>(CandidateObj));
		}

		FProbeIKGoalCachedFrame CacheFrame;
		CacheFrame.FrameIndex = Frame.FrameIndex;
		CacheFrame.CandidateIndex = Frame.CandidateIndex;
		CacheFrame.Label = Frame.Label;
		CacheFrame.Kind = Frame.Kind;
		CacheFrame.CandidateLabel = Frame.CandidateLabel;
		CacheFrame.TransformMode = Frame.TransformMode;
		CacheFrame.GoalTransform = Frame.AppliedControlTransform;
		CacheFrame.ControlTransform = Frame.AppliedControlTransform;
		CacheFrame.DeltaTransform = Frame.DeltaTransform;
		CacheFrame.bHasDeltaTransform = Frame.bHasDeltaTransform;
		CacheFrame.Pixels = Frame.Pixels;
		CacheFrames.Add(CacheFrame);
	}

	FString SheetPath;
	TSharedPtr<FJsonObject> SheetData;
	if (bCreateFrameSheet)
	{
		if (!WriteFrameSequenceSheetImage(FramePixels, FrameLabels, FrameSize, Ctx.Params, Format, Quality, SheetPath, SheetData, OutError))
		{
			return false;
		}
		AddArtifact(TEXT("probe_ik_goal_frame_sheet"), SheetPath, FIntPoint(
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("width"))) : 0,
			SheetData.IsValid() ? FMath::RoundToInt(SheetData->GetNumberField(TEXT("height"))) : 0));
	}

	FProbeIKGoalCandidateCacheEntry CacheEntry;
	CacheEntry.RequestId = Ctx.RequestId;
	CacheEntry.ContextMode = TEXT("sequence_control_rig");
	CacheEntry.SequencePath = UeAgentIKAssetOps::NormalizeAssetPath(SequencePath);
	CacheEntry.SequenceObjectPath = Sequence->GetPathName();
	CacheEntry.BindingGuid = BindingGuid.ToString(EGuidFormats::DigitsWithHyphens);
	CacheEntry.BindingName = BindingName;
	CacheEntry.ControlName = ControlNameText;
	CacheEntry.ControlType = ProbeControlTypeToString(ControlType);
	CacheEntry.ControlRigClass = ControlRig->GetClass() ? ControlRig->GetClass()->GetPathName() : TEXT("");
	CacheEntry.SequenceFrameIndex = SequenceFrameIndex;
	CacheEntry.SequenceTimeSeconds = SequenceTimeSeconds;
	CacheEntry.EvaluationFrame = EvaluationFrame;
	CacheEntry.OriginalControlTransform = OriginalControlTransform;
	CacheEntry.BaselineMode = TEXT("current_sequence_frame_control_transform");
	CacheEntry.BaselineControls = BaselineControls;
	CacheEntry.CreatedUtc = FDateTime::UtcNow();
	CacheEntry.FrameSize = FrameSize;
	CacheEntry.FixedCaptureBounds = UnionBounds;
	CacheEntry.BoundsPadding = BoundsPadding;
	CacheEntry.Format = Format;
	CacheEntry.Quality = Quality;
	CacheEntry.FrameSheetPath = SheetPath;
	CacheEntry.Frames = CacheFrames;
	GProbeIKGoalCandidateCacheByRequestId.Add(Ctx.RequestId, CacheEntry);
	GLastProbeIKGoalCandidateRequestId = Ctx.RequestId;
	TrimProbeIKGoalCandidateCache();

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.ik_goal_candidates.v2"));
	OutData->SetStringField(TEXT("implementation"), TEXT("single_sequence_player_live_control_rig_current_state_no_key_no_sequence_copy"));
	OutData->SetStringField(TEXT("context_mode"), TEXT("sequence_control_rig"));
	OutData->SetStringField(TEXT("request_id"), Ctx.RequestId);
	OutData->SetStringField(TEXT("sequence_path"), CacheEntry.SequencePath);
	OutData->SetStringField(TEXT("sequence_object_path"), CacheEntry.SequenceObjectPath);
	OutData->SetStringField(TEXT("binding_guid"), CacheEntry.BindingGuid);
	OutData->SetStringField(TEXT("binding_name"), BindingName);
	OutData->SetStringField(TEXT("control_name"), ControlNameText);
	OutData->SetStringField(TEXT("control_type"), CacheEntry.ControlType);
	OutData->SetStringField(TEXT("control_rig_class"), CacheEntry.ControlRigClass);
	OutData->SetNumberField(TEXT("sequence_frame_index"), SequenceFrameIndex);
	OutData->SetNumberField(TEXT("sequence_time_seconds"), SequenceTimeSeconds);
	OutData->SetNumberField(TEXT("evaluation_frame"), EvaluationFrame.Value);
	OutData->SetNumberField(TEXT("candidate_count"), CandidateValues->Num());
	OutData->SetNumberField(TEXT("frame_count"), Frames.Num());
	OutData->SetStringField(TEXT("candidate_transform_semantics"), TEXT("relative_transform_from_original_sequence_control_at_frame"));
	OutData->SetStringField(TEXT("transform_space"), TEXT("control_local"));
	OutData->SetStringField(TEXT("f0_source"), TEXT("source_sequence_unmodified_current_state"));
	OutData->SetStringField(TEXT("capture_mode"), TEXT("level_sequence_control_rig_fixed_bounds_scene_capture"));
	OutData->SetStringField(TEXT("evaluation_mode"), TEXT("single_sequence_player_live_control_rig_current_state_no_key"));
	OutData->SetStringField(TEXT("visualization_mode"), TEXT("sequence_bound_object_union_aabb_front_side"));
	OutData->SetStringField(TEXT("baseline_mode"), CacheEntry.BaselineMode);
	OutData->SetNumberField(TEXT("baseline_control_count"), 0);
	OutData->SetArrayField(TEXT("baseline_controls"), TArray<TSharedPtr<FJsonValue>>());
	OutData->SetNumberField(TEXT("diagnostic_control_bone_map_count"), BaselineControls.Num());
	OutData->SetObjectField(TEXT("original_control_transform"), UeAgentIKAssetOps::BuildTransformJson(OriginalControlTransform));
	OutData->SetObjectField(TEXT("restored_control_transform"), UeAgentIKAssetOps::BuildTransformJson(RestoredControlTransform));
	OutData->SetBoolField(TEXT("restored"), bRestored);
	OutData->SetBoolField(TEXT("prebound_control_rig_to_sequence_object"), bPreboundControlRig);
	OutData->SetBoolField(TEXT("writes_sequence_keys"), false);
	OutData->SetBoolField(TEXT("duplicates_sequence"), false);
	OutData->SetBoolField(TEXT("uses_external_simulation"), false);
	OutData->SetBoolField(TEXT("visual_validation_passed"), bAnyCandidatePixelsChanged);
	OutData->SetBoolField(TEXT("pose_sample_validation_passed"), bAnyCandidatePoseChanged);
	OutData->SetNumberField(TEXT("frame_width"), FrameSize.X);
	OutData->SetNumberField(TEXT("frame_height"), FrameSize.Y);
	OutData->SetNumberField(TEXT("bounds_padding"), BoundsPadding);
	OutData->SetObjectField(TEXT("union_bounds"), BuildProbeBoxJson(UnionBounds));
	OutData->SetObjectField(TEXT("fixed_capture_bounds"), BuildProbeBoxJson(UnionBounds));
	OutData->SetArrayField(TEXT("watched_bones"), BuildProbeStringArrayJson(WatchedBoneNames));
	OutData->SetNumberField(TEXT("watched_bone_count"), WatchedBoneNames.Num());
	OutData->SetArrayField(TEXT("warnings"), BuildProbeStringArrayJson(Warnings));
	OutData->SetArrayField(TEXT("frames"), FrameResults);
	OutData->SetArrayField(TEXT("candidates"), CandidateResults);
	OutData->SetBoolField(TEXT("write_individual_frames"), bWriteIndividualFrames);
	OutData->SetBoolField(TEXT("create_frame_sheet"), bCreateFrameSheet);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("quality"), Quality);
	OutData->SetStringField(TEXT("probe_request_id"), Ctx.RequestId);
	OutData->SetBoolField(TEXT("candidate_cache_available"), true);
	OutData->SetStringField(TEXT("candidate_cache_request_id"), Ctx.RequestId);
	if (SheetData.IsValid())
	{
		OutData->SetObjectField(TEXT("frame_sheet"), SheetData);
		OutData->SetStringField(TEXT("frame_sheet_file_path"), SheetPath);
		OutData->SetStringField(TEXT("sequence_frame_image_path"), SheetPath);
	}
	OutData->SetNumberField(TEXT("artifact_count"), OutArtifacts.Num());
	UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}

bool FUeAgentHttpServer::CmdProbeIKGoalReviewFrames(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::IKProbeReviewFrames,
		UnifiedOptionsReport);

	FString ProbeRequestId;
	JsonTryGetString(Ctx.Params, TEXT("probe_request_id"), ProbeRequestId);
	if (ProbeRequestId.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("candidate_cache_request_id"), ProbeRequestId);
	}
	ProbeRequestId = ProbeRequestId.TrimStartAndEnd();
	if (ProbeRequestId.IsEmpty())
	{
		ProbeRequestId = GLastProbeIKGoalCandidateRequestId;
	}
	if (ProbeRequestId.IsEmpty())
	{
		OutError = TEXT("missing_probe_request_id");
		return false;
	}

	const FProbeIKGoalCandidateCacheEntry* CacheEntry = GProbeIKGoalCandidateCacheByRequestId.Find(ProbeRequestId);
	if (!CacheEntry)
	{
		OutError = TEXT("probe_candidate_cache_not_found");
		return false;
	}
	if (CacheEntry->FrameSize.X <= 0 || CacheEntry->FrameSize.Y <= 0)
	{
		OutError = TEXT("probe_frame_size_not_cached");
		return false;
	}

	TArray<int32> RequestedFrameIndices;
	if (!TryGetProbeFrameSelectionArray(Ctx.Params, RequestedFrameIndices, OutError))
	{
		return false;
	}

	FString Format = CacheEntry->Format.IsEmpty() ? FString(TEXT("png")) : CacheEntry->Format.ToLower();
	FString FormatOverride;
	if (ProbeJsonTryGetString(Ctx.Params, TEXT("format"), FormatOverride) && !FormatOverride.TrimStartAndEnd().IsEmpty())
	{
		Format = FormatOverride.TrimStartAndEnd().ToLower();
	}
	if (Format == TEXT("jpg"))
	{
		Format = TEXT("jpeg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpeg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_image_format");
		return false;
	}

	int32 Quality = FMath::Clamp(CacheEntry->Quality, 1, 100);
	double QualityValue = static_cast<double>(Quality);
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
	{
		Quality = FMath::Clamp(FMath::RoundToInt(QualityValue), 1, 100);
	}

	bool bRerenderSelectedFrames = CacheEntry->ContextMode == TEXT("sequence_control_rig");
	ProbeJsonTryGetBool(Ctx.Params, TEXT("rerender"), bRerenderSelectedFrames);
	ProbeJsonTryGetBool(Ctx.Params, TEXT("recapture"), bRerenderSelectedFrames);
	ProbeJsonTryGetBool(Ctx.Params, TEXT("rerender_selected_frames"), bRerenderSelectedFrames);
	bool bUseCachedPixels = false;
	if (ProbeJsonTryGetBool(Ctx.Params, TEXT("use_cached_pixels"), bUseCachedPixels) && bUseCachedPixels)
	{
		bRerenderSelectedFrames = false;
	}
	if (CacheEntry->ContextMode != TEXT("sequence_control_rig"))
	{
		bRerenderSelectedFrames = false;
	}

	int32 ReviewFrameWidth = bRerenderSelectedFrames ? FMath::Max(CacheEntry->FrameSize.X, 1920) : CacheEntry->FrameSize.X;
	int32 ReviewFrameHeight = bRerenderSelectedFrames ? FMath::Max(CacheEntry->FrameSize.Y, 1280) : CacheEntry->FrameSize.Y;
	double FrameDimensionValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("frame_width"), FrameDimensionValue) || JsonTryGetNumber(Ctx.Params, TEXT("width"), FrameDimensionValue))
	{
		ReviewFrameWidth = FMath::Clamp(FMath::RoundToInt(FrameDimensionValue), 128, 4096);
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("frame_height"), FrameDimensionValue) || JsonTryGetNumber(Ctx.Params, TEXT("height"), FrameDimensionValue))
	{
		ReviewFrameHeight = FMath::Clamp(FMath::RoundToInt(FrameDimensionValue), 128, 4096);
	}
	const FIntPoint ReviewFrameSize(ReviewFrameWidth, ReviewFrameHeight);

	double BoundsPadding = CacheEntry->BoundsPadding > 0.0 ? CacheEntry->BoundsPadding : 1.35;
	if (JsonTryGetNumber(Ctx.Params, TEXT("mesh_capture_bounds_padding"), BoundsPadding)
		|| JsonTryGetNumber(Ctx.Params, TEXT("bounds_padding"), BoundsPadding))
	{
		BoundsPadding = FMath::Clamp(BoundsPadding, 1.0, 4.0);
	}

	TArray<const FProbeIKGoalCachedFrame*> SelectedFrames;
	TArray<TArray<FColor>> SelectedPixels;
	TArray<FString> SelectedLabels;
	TArray<TSharedPtr<FJsonValue>> SelectedFrameResults;
	TArray<TSharedPtr<FJsonValue>> RequestedFrameResults;
	TArray<FString> Warnings;
	TSet<int32> SeenFrameIndices;

	for (int32 SelectionIndex = 0; SelectionIndex < RequestedFrameIndices.Num(); ++SelectionIndex)
	{
		const int32 RequestedFrameIndex = RequestedFrameIndices[SelectionIndex];
		RequestedFrameResults.Add(MakeShared<FJsonValueNumber>(RequestedFrameIndex));
		if (SeenFrameIndices.Contains(RequestedFrameIndex))
		{
			Warnings.Add(FString::Printf(TEXT("duplicate_frame_selection:F%d"), RequestedFrameIndex));
		}
		SeenFrameIndices.Add(RequestedFrameIndex);

		const FProbeIKGoalCachedFrame* SelectedFrame = nullptr;
		for (const FProbeIKGoalCachedFrame& Frame : CacheEntry->Frames)
		{
			if (Frame.FrameIndex == RequestedFrameIndex)
			{
				SelectedFrame = &Frame;
				break;
			}
		}
		if (!SelectedFrame)
		{
			OutError = FString::Printf(TEXT("frame_index_not_found_in_probe_cache:F%d"), RequestedFrameIndex);
			return false;
		}

		SelectedFrames.Add(SelectedFrame);
		SelectedLabels.Add(!SelectedFrame->Label.IsEmpty() ? SelectedFrame->Label : FString::Printf(TEXT("F%d"), SelectedFrame->FrameIndex));
	}

	if (bRerenderSelectedFrames)
	{
		UWorld* World = GetEditorWorld(OutError);
		if (!World)
		{
			return false;
		}
		ULevelSequence* Sequence = LoadProbeLevelSequence(CacheEntry->SequencePath);
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

		FGuid BindingGuid;
		if (!FGuid::Parse(CacheEntry->BindingGuid, BindingGuid) || !MovieScene->FindBinding(BindingGuid))
		{
			OutError = TEXT("cached_sequence_binding_not_found");
			return false;
		}
		const FName ControlName(*CacheEntry->ControlName);
		UMovieSceneControlRigParameterTrack* ControlRigTrack = nullptr;
		UMovieSceneControlRigParameterSection* ControlRigSection = nullptr;
		UControlRig* ControlRig = nullptr;
		if (!TryFindProbeControlRigTrack(MovieScene, BindingGuid, ControlName, FString(), CacheEntry->EvaluationFrame, false, ControlRigTrack, ControlRigSection, ControlRig, OutError))
		{
			return false;
		}
		FRigControlElement* ControlElement = ControlRig ? ControlRig->FindControl(ControlName) : nullptr;
		if (!ControlElement)
		{
			OutError = TEXT("control_rig_control_not_found");
			return false;
		}
		if (!IsProbeTransformControlType(ControlElement->Settings.ControlType))
		{
			OutError = TEXT("control_rig_control_type_not_supported");
			return false;
		}

		struct FProbeReviewSequenceCleanup
		{
			UWorld* World = nullptr;
			ULevelSequencePlayer* Player = nullptr;
			ALevelSequenceActor* SequenceActor = nullptr;

			~FProbeReviewSequenceCleanup()
			{
				if (Player)
				{
					Player->Stop();
				}
				if (World && SequenceActor && IsValid(SequenceActor))
				{
					World->DestroyActor(SequenceActor, false, false);
				}
			}
		};

		FString PrebindError;
		const bool bPreboundControlRig = RebindProbeControlRigToSequenceObject(Sequence, BindingGuid, ControlRig, World, PrebindError);
		FProbeReviewSequenceCleanup Cleanup;
		Cleanup.World = World;
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

		const FFrameTime PlaybackFrame = ConvertProbeEvaluationTickToPlaybackFrame(MovieScene, CacheEntry->EvaluationFrame);
		Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(PlaybackFrame, EUpdatePositionMethod::Jump));
		if (!bPreboundControlRig)
		{
			if (!RebindProbeControlRigToSequenceObject(Sequence, BindingGuid, ControlRig, World, OutError))
			{
				OutError = FString::Printf(TEXT("control_rig_rebind_failed: pre=%s post=%s"), *PrebindError, *OutError);
				return false;
			}
			Cleanup.Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(PlaybackFrame, EUpdatePositionMethod::Jump));
		}

		TArray<UObject*> BoundObjects;
		if (!ResolveProbeBoundObjects(Sequence, BindingGuid, World, BoundObjects))
		{
			OutError = TEXT("sequence_binding_bound_objects_not_found");
			return false;
		}
		RefreshProbeSkeletalBoundObjects(BoundObjects);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();

		struct FProbeReviewRestoreCurrentControl
		{
			UWorld* World = nullptr;
			UControlRig* ControlRig = nullptr;
			FName ControlName = NAME_None;
			FTransform OriginalTransform = FTransform::Identity;
			TArray<UObject*> BoundObjects;
			bool bEnabled = false;

			void RestoreNow()
			{
				if (!bEnabled || !ControlRig)
				{
					return;
				}
				const FRigControlModifiedContext NoKeyContext(EControlRigSetKey::Never);
				ControlRig->SetControlLocalTransform(ControlName, OriginalTransform, true, NoKeyContext, false, true);
				ControlRig->Evaluate_AnyThread();
				RefreshProbeSkeletalBoundObjects(BoundObjects);
				if (World)
				{
					World->SendAllEndOfFrameUpdates();
				}
				FlushRenderingCommands();
				bEnabled = false;
			}

			~FProbeReviewRestoreCurrentControl()
			{
				RestoreNow();
			}
		};

		FProbeReviewRestoreCurrentControl RestoreScope;
		RestoreScope.World = World;
		RestoreScope.ControlRig = ControlRig;
		RestoreScope.ControlName = ControlName;
		RestoreScope.OriginalTransform = ControlRig->GetControlLocalTransform(ControlName);
		RestoreScope.BoundObjects = BoundObjects;
		RestoreScope.bEnabled = true;

		auto ApplyControlState = [&](const FTransform& ControlTransform) -> bool
		{
			const FRigControlModifiedContext NoKeyContext(EControlRigSetKey::Never);
			ControlRig->SetControlLocalTransform(ControlName, ControlTransform, true, NoKeyContext, false, true);
			ControlRig->Evaluate_AnyThread();
			RefreshProbeSkeletalBoundObjects(BoundObjects);
			World->SendAllEndOfFrameUpdates();
			FlushRenderingCommands();
			return true;
		};

		FBox ReviewBounds(ForceInit);
		for (const FProbeIKGoalCachedFrame* SelectedFrame : SelectedFrames)
		{
			if (!SelectedFrame || !ApplyControlState(SelectedFrame->ControlTransform))
			{
				OutError = TEXT("apply_control_transform_failed");
				return false;
			}
			const FBox FrameBounds = BuildProbeBoundObjectsBox(BoundObjects);
			if (FrameBounds.IsValid)
			{
				ReviewBounds += FrameBounds;
			}
		}
		if (!ReviewBounds.IsValid && CacheEntry->FixedCaptureBounds.IsValid)
		{
			ReviewBounds = CacheEntry->FixedCaptureBounds;
			Warnings.Add(TEXT("review_recapture_used_source_fixed_bounds"));
		}
		if (!ReviewBounds.IsValid)
		{
			OutError = TEXT("review_recapture_bounds_invalid");
			return false;
		}

		for (const FProbeIKGoalCachedFrame* SelectedFrame : SelectedFrames)
		{
			if (!SelectedFrame || !ApplyControlState(SelectedFrame->ControlTransform))
			{
				OutError = TEXT("apply_control_transform_failed");
				return false;
			}
			TArray<FColor> FramePixels;
			FString CameraSource;
			if (!CaptureProbeBoundObjectsFramePixels(World, BoundObjects, ReviewBounds, ReviewFrameSize, BoundsPadding, FramePixels, CameraSource, OutError))
			{
				OutError = FString::Printf(TEXT("review_recapture_frame_failed:%s:%s"), *SelectedFrame->Label, *OutError);
				return false;
			}
			SelectedPixels.Add(MoveTemp(FramePixels));
		}
		RestoreScope.RestoreNow();
		if (!bPreboundControlRig)
		{
			Warnings.Add(FString::Printf(TEXT("control_rig_prebind_failed_then_rebound_after_player:%s"), *PrebindError));
		}
	}
	else
	{
		for (const FProbeIKGoalCachedFrame* SelectedFrame : SelectedFrames)
		{
			if (!SelectedFrame)
			{
				OutError = TEXT("invalid_selected_frame");
				return false;
			}
			if (SelectedFrame->Pixels.Num() != CacheEntry->FrameSize.X * CacheEntry->FrameSize.Y)
			{
				OutError = FString::Printf(TEXT("probe_frame_pixels_not_available:F%d"), SelectedFrame->FrameIndex);
				return false;
			}
			SelectedPixels.Add(SelectedFrame->Pixels);
		}
	}

	for (int32 SelectionIndex = 0; SelectionIndex < SelectedFrames.Num(); ++SelectionIndex)
	{
		const FProbeIKGoalCachedFrame* SelectedFrame = SelectedFrames[SelectionIndex];
		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("selection_index"), SelectionIndex);
		FrameObj->SetNumberField(TEXT("source_frame_index"), SelectedFrame->FrameIndex);
		FrameObj->SetStringField(TEXT("source_frame_label"), !SelectedFrame->Label.IsEmpty() ? SelectedFrame->Label : FString::Printf(TEXT("F%d"), SelectedFrame->FrameIndex));
		FrameObj->SetStringField(TEXT("kind"), SelectedFrame->Kind);
		FrameObj->SetNumberField(TEXT("candidate_index"), SelectedFrame->CandidateIndex);
		FrameObj->SetStringField(TEXT("candidate_label"), SelectedFrame->CandidateLabel);
		FrameObj->SetStringField(TEXT("transform_mode"), SelectedFrame->TransformMode);
		FrameObj->SetObjectField(TEXT("control_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->ControlTransform));
		if (SelectedFrame->bHasDeltaTransform)
		{
			FrameObj->SetObjectField(TEXT("delta_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->DeltaTransform));
		}
		FrameObj->SetNumberField(TEXT("width"), ReviewFrameSize.X);
		FrameObj->SetNumberField(TEXT("height"), ReviewFrameSize.Y);
		FrameObj->SetBoolField(TEXT("rerendered"), bRerenderSelectedFrames);
		SelectedFrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
	}

	FString SheetPath;
	TSharedPtr<FJsonObject> SheetData;
	if (!WriteFrameSequenceSheetImage(SelectedPixels, SelectedLabels, ReviewFrameSize, Ctx.Params, Format, Quality, SheetPath, SheetData, OutError))
	{
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*SheetPath);
	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("probe_ik_goal_review_frame_sheet"));
	Artifact->SetStringField(TEXT("file_path"), SheetPath);
	Artifact->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), SheetData.IsValid() ? SheetData->GetNumberField(TEXT("width")) : 0.0);
	Artifact->SetNumberField(TEXT("height"), SheetData.IsValid() ? SheetData->GetNumberField(TEXT("height")) : 0.0);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.ik_goal_review_frames.v1"));
	OutData->SetStringField(TEXT("implementation"), bRerenderSelectedFrames ? TEXT("sequence_control_rig_recapture_selected_frames_no_key") : TEXT("cached_probe_frame_pixels_no_sequence_eval_no_key"));
	OutData->SetStringField(TEXT("context_mode"), CacheEntry->ContextMode);
	OutData->SetStringField(TEXT("request_id"), Ctx.RequestId);
	OutData->SetStringField(TEXT("probe_request_id"), ProbeRequestId);
	OutData->SetStringField(TEXT("source_probe_request_id"), ProbeRequestId);
	OutData->SetStringField(TEXT("sequence_path"), CacheEntry->SequencePath);
	OutData->SetStringField(TEXT("sequence_object_path"), CacheEntry->SequenceObjectPath);
	OutData->SetStringField(TEXT("binding_guid"), CacheEntry->BindingGuid);
	OutData->SetStringField(TEXT("binding_name"), CacheEntry->BindingName);
	OutData->SetStringField(TEXT("control_name"), CacheEntry->ControlName);
	OutData->SetStringField(TEXT("control_type"), CacheEntry->ControlType);
	OutData->SetStringField(TEXT("control_rig_class"), CacheEntry->ControlRigClass);
	OutData->SetNumberField(TEXT("sequence_frame_index"), CacheEntry->SequenceFrameIndex);
	OutData->SetNumberField(TEXT("sequence_time_seconds"), CacheEntry->SequenceTimeSeconds);
	OutData->SetNumberField(TEXT("evaluation_frame"), CacheEntry->EvaluationFrame.Value);
	OutData->SetStringField(TEXT("selection_semantics"), TEXT("frame_index_matches_source_sheet_label"));
	OutData->SetStringField(TEXT("label_mode"), TEXT("source_frame_label"));
	OutData->SetArrayField(TEXT("requested_frames"), RequestedFrameResults);
	OutData->SetNumberField(TEXT("source_frame_count"), CacheEntry->Frames.Num());
	OutData->SetNumberField(TEXT("selected_frame_count"), SelectedFrameResults.Num());
	OutData->SetNumberField(TEXT("frame_count"), SelectedFrameResults.Num());
	OutData->SetNumberField(TEXT("frame_width"), ReviewFrameSize.X);
	OutData->SetNumberField(TEXT("frame_height"), ReviewFrameSize.Y);
	OutData->SetNumberField(TEXT("source_frame_width"), CacheEntry->FrameSize.X);
	OutData->SetNumberField(TEXT("source_frame_height"), CacheEntry->FrameSize.Y);
	OutData->SetNumberField(TEXT("bounds_padding"), BoundsPadding);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("quality"), Quality);
	OutData->SetBoolField(TEXT("reuses_cached_pixels"), !bRerenderSelectedFrames);
	OutData->SetBoolField(TEXT("rerenders_selected_frames"), bRerenderSelectedFrames);
	OutData->SetBoolField(TEXT("evaluates_sequence"), bRerenderSelectedFrames);
	OutData->SetBoolField(TEXT("writes_sequence_keys"), false);
	OutData->SetBoolField(TEXT("duplicates_sequence"), false);
	OutData->SetBoolField(TEXT("uses_external_simulation"), false);
	OutData->SetStringField(TEXT("source_frame_sheet_file_path"), CacheEntry->FrameSheetPath);
	OutData->SetArrayField(TEXT("warnings"), BuildProbeStringArrayJson(Warnings));
	OutData->SetArrayField(TEXT("frames"), SelectedFrameResults);
	if (SheetData.IsValid())
	{
		OutData->SetObjectField(TEXT("frame_sheet"), SheetData);
		OutData->SetStringField(TEXT("frame_sheet_file_path"), SheetPath);
		OutData->SetStringField(TEXT("sequence_frame_image_path"), SheetPath);
	}
	OutData->SetNumberField(TEXT("artifact_count"), OutArtifacts.Num());
	UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}

bool FUeAgentHttpServer::CmdProbeIKGoalApplyCandidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ProbeRequestId;
	JsonTryGetString(Ctx.Params, TEXT("probe_request_id"), ProbeRequestId);
	if (ProbeRequestId.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("candidate_cache_request_id"), ProbeRequestId);
	}
	ProbeRequestId = ProbeRequestId.TrimStartAndEnd();
	if (ProbeRequestId.IsEmpty())
	{
		ProbeRequestId = GLastProbeIKGoalCandidateRequestId;
	}
	if (ProbeRequestId.IsEmpty())
	{
		OutError = TEXT("missing_probe_request_id");
		return false;
	}

	const FProbeIKGoalCandidateCacheEntry* CacheEntry = GProbeIKGoalCandidateCacheByRequestId.Find(ProbeRequestId);
	if (!CacheEntry)
	{
		OutError = TEXT("probe_candidate_cache_not_found");
		return false;
	}
	if (CacheEntry->ContextMode == TEXT("sequence_control_rig"))
	{
		return CmdProbeIKGoalApplyCandidateSequenceControlRig(Ctx, ProbeRequestId, *CacheEntry, OutData, OutError);
	}

	FString AssetPathOverride;
	if (JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathOverride) && !AssetPathOverride.TrimStartAndEnd().IsEmpty())
	{
		if (UeAgentIKAssetOps::NormalizeAssetPath(AssetPathOverride) != UeAgentIKAssetOps::NormalizeAssetPath(CacheEntry->AssetPath))
		{
			OutError = TEXT("probe_candidate_asset_mismatch");
			return false;
		}
	}

	FString GoalNameOverride;
	if (JsonTryGetString(Ctx.Params, TEXT("goal_name"), GoalNameOverride) && !GoalNameOverride.TrimStartAndEnd().IsEmpty())
	{
		if (GoalNameOverride.TrimStartAndEnd() != CacheEntry->GoalName)
		{
			OutError = TEXT("probe_candidate_goal_mismatch");
			return false;
		}
	}

	int32 RequestedFrameIndex = INDEX_NONE;
	bool bUseFrameIndex = TryGetIntParam(Ctx.Params, TEXT("frame_index"), RequestedFrameIndex)
		|| TryGetIntParam(Ctx.Params, TEXT("frame_number"), RequestedFrameIndex)
		|| TryGetIntParam(Ctx.Params, TEXT("number"), RequestedFrameIndex)
		|| TryGetIntParam(Ctx.Params, TEXT("index"), RequestedFrameIndex)
		|| TryGetIntParam(Ctx.Params, TEXT("selection"), RequestedFrameIndex);

	int32 RequestedCandidateIndex = INDEX_NONE;
	const bool bUseCandidateIndex = !bUseFrameIndex && (
		TryGetIntParam(Ctx.Params, TEXT("candidate_index"), RequestedCandidateIndex)
		|| TryGetIntParam(Ctx.Params, TEXT("candidate_number"), RequestedCandidateIndex));

	if (!bUseFrameIndex && !bUseCandidateIndex)
	{
		OutError = TEXT("missing_frame_index_or_candidate_index");
		return false;
	}

	const FProbeIKGoalCachedFrame* SelectedFrame = nullptr;
	for (const FProbeIKGoalCachedFrame& Frame : CacheEntry->Frames)
	{
		if ((bUseFrameIndex && Frame.FrameIndex == RequestedFrameIndex)
			|| (bUseCandidateIndex && Frame.CandidateIndex == RequestedCandidateIndex))
		{
			SelectedFrame = &Frame;
			break;
		}
	}
	if (!SelectedFrame)
	{
		OutError = bUseFrameIndex ? TEXT("frame_index_not_found_in_probe_cache") : TEXT("candidate_index_not_found_in_probe_cache");
		return false;
	}

	UIKRigDefinition* IKRig = UeAgentIKAssetOps::LoadIKRig(CacheEntry->AssetPath);
	if (!IKRig)
	{
		OutError = TEXT("ik_rig_not_found");
		return false;
	}

	UIKRigController* Controller = UIKRigController::GetController(IKRig);
	if (!Controller)
	{
		OutError = TEXT("ik_rig_controller_not_available");
		return false;
	}

	const FName GoalName(*CacheEntry->GoalName);
	UIKRigEffectorGoal* Goal = Controller->GetGoal(GoalName);
	if (!Goal)
	{
		OutError = TEXT("goal_not_found");
		return false;
	}

	const FTransform PreviousGoalTransform = Goal->CurrentTransform;
	const bool bAlreadyApplied = PreviousGoalTransform.Equals(SelectedFrame->GoalTransform, 0.001f);
	if (!bAlreadyApplied)
	{
		IKRig->Modify();
		Goal->Modify();
		Controller->SetGoalCurrentTransform(GoalName, SelectedFrame->GoalTransform);
		Goal = Controller->GetGoal(GoalName);
	}

	if (!Goal)
	{
		OutError = TEXT("goal_not_found_after_apply");
		return false;
	}

	const FTransform FinalGoalTransform = Goal->CurrentTransform;
	const bool bAppliedMatches = FinalGoalTransform.Equals(SelectedFrame->GoalTransform, 0.001f);
	if (!bAppliedMatches)
	{
		OutError = TEXT("applied_goal_transform_mismatch");
		return false;
	}

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentIKAssetOps::SaveAssetPackage(IKRig, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("schema"), TEXT("uai.probe.ik_goal_apply_candidate.v1"));
	OutData->SetStringField(TEXT("probe_request_id"), ProbeRequestId);
	OutData->SetStringField(TEXT("asset_path"), CacheEntry->AssetPath);
	OutData->SetStringField(TEXT("object_path"), CacheEntry->ObjectPath);
	OutData->SetStringField(TEXT("goal_name"), CacheEntry->GoalName);
	OutData->SetStringField(TEXT("bone_name"), CacheEntry->BoneName);
	OutData->SetNumberField(TEXT("applied_frame_index"), SelectedFrame->FrameIndex);
	OutData->SetStringField(TEXT("applied_frame_label"), SelectedFrame->Label);
	OutData->SetStringField(TEXT("applied_kind"), SelectedFrame->Kind);
	OutData->SetNumberField(TEXT("applied_candidate_index"), SelectedFrame->CandidateIndex);
	OutData->SetStringField(TEXT("transform_mode"), SelectedFrame->TransformMode);
	OutData->SetObjectField(TEXT("original_goal_transform"), UeAgentIKAssetOps::BuildTransformJson(CacheEntry->OriginalGoalTransform));
	OutData->SetObjectField(TEXT("previous_goal_transform"), UeAgentIKAssetOps::BuildTransformJson(PreviousGoalTransform));
	OutData->SetObjectField(TEXT("applied_goal_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->GoalTransform));
	OutData->SetObjectField(TEXT("final_goal_transform"), UeAgentIKAssetOps::BuildTransformJson(FinalGoalTransform));
	if (SelectedFrame->bHasDeltaTransform)
	{
		OutData->SetObjectField(TEXT("delta_transform"), UeAgentIKAssetOps::BuildTransformJson(SelectedFrame->DeltaTransform));
	}
	OutData->SetBoolField(TEXT("already_applied"), bAlreadyApplied);
	OutData->SetBoolField(TEXT("applied_matches_selected"), bAppliedMatches);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	OutData->SetNumberField(TEXT("cached_frame_count"), CacheEntry->Frames.Num());
	OutData->SetStringField(TEXT("selection_semantics"), bUseFrameIndex ? TEXT("frame_index_matches_sheet_label") : TEXT("candidate_index_zero_based"));
	return true;
}
