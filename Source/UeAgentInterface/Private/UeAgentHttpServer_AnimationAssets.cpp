// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "UeAgentJsonDiagnostics.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimLinkableElement.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/AnimEnums.h"
#include "Animation/AnimTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationBlueprintLibrary.h"
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
#include "Framework/Application/SlateApplication.h"
#include "IAnimationEditor.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "Misc/PackageName.h"
#include "LevelEditorViewport.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UeAgentAnimationAssetOps
{
	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UAnimSequence* LoadAnimSequence(const FString& InPath)
	{
		return Cast<UAnimSequence>(LoadAssetObject(InPath));
	}

	static UAnimationAsset* LoadAnimationAsset(const FString& InPath)
	{
		return Cast<UAnimationAsset>(LoadAssetObject(InPath));
	}

	static USkeleton* LoadSkeleton(const FString& InPath)
	{
		return Cast<USkeleton>(LoadAssetObject(InPath));
	}

	static USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	static UAnimBoneCompressionSettings* LoadBoneCompressionSettings(const FString& InPath)
	{
		return Cast<UAnimBoneCompressionSettings>(LoadAssetObject(InPath));
	}

	static UAnimCurveCompressionSettings* LoadCurveCompressionSettings(const FString& InPath)
	{
		return Cast<UAnimCurveCompressionSettings>(LoadAssetObject(InPath));
	}

	static UVariableFrameStrippingSettings* LoadVariableFrameStrippingSettings(const FString& InPath)
	{
		return Cast<UVariableFrameStrippingSettings>(LoadAssetObject(InPath));
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
	}

	static bool TryParseLinearColorField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FLinearColor& OutColor)
	{
		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, ColorObject) || !ColorObject || !ColorObject->IsValid())
		{
			return false;
		}

		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		const bool bHasR = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("r"), TEXT("R"), TEXT("red"), TEXT("Red") }, R);
		const bool bHasG = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("g"), TEXT("G"), TEXT("green"), TEXT("Green") }, G);
		const bool bHasB = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("b"), TEXT("B"), TEXT("blue"), TEXT("Blue") }, B);
		UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(*ColorObject, { TEXT("a"), TEXT("A"), TEXT("alpha"), TEXT("Alpha") }, A);
		if (!bHasR || !bHasG || !bHasB)
		{
			return false;
		}

		const bool bLooksByteRange = R > 1.0 || G > 1.0 || B > 1.0 || A > 1.0;
		if (bLooksByteRange)
		{
			OutColor = FLinearColor(static_cast<float>(R / 255.0), static_cast<float>(G / 255.0), static_cast<float>(B / 255.0), static_cast<float>(A / 255.0));
		}
		else
		{
			OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
		}
		return true;
	}

	static const TCHAR* InterpolationTypeToString(const EAnimInterpolationType Type)
	{
		switch (Type)
		{
		case EAnimInterpolationType::Step:
			return TEXT("step");
		case EAnimInterpolationType::Linear:
		default:
			return TEXT("linear");
		}
	}

	static bool ParseInterpolationType(const FString& InValue, EAnimInterpolationType& OutType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("linear"))
		{
			OutType = EAnimInterpolationType::Linear;
			return true;
		}
		if (Normalized == TEXT("step") || Normalized == TEXT("constant"))
		{
			OutType = EAnimInterpolationType::Step;
			return true;
		}
		return false;
	}

	static const TCHAR* CurveTypeToString(const ERawCurveTrackTypes Type)
	{
		switch (Type)
		{
		case ERawCurveTrackTypes::RCT_Float:
		default:
			return TEXT("float");
		}
	}

	static bool ParseCurveType(const FString& InValue, ERawCurveTrackTypes& OutType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("float"))
		{
			OutType = ERawCurveTrackTypes::RCT_Float;
			return true;
		}
		return false;
	}

	static const TCHAR* AdditiveAnimationTypeToString(const EAdditiveAnimationType Type)
	{
		switch (Type)
		{
		case AAT_LocalSpaceBase:
			return TEXT("local_space");
		case AAT_RotationOffsetMeshSpace:
			return TEXT("mesh_space_rotation_offset");
		case AAT_None:
		default:
			return TEXT("none");
		}
	}

	static const TCHAR* AdditiveBasePoseTypeToString(const EAdditiveBasePoseType Type)
	{
		switch (Type)
		{
		case ABPT_RefPose:
			return TEXT("ref_pose");
		case ABPT_AnimScaled:
			return TEXT("anim_scaled");
		case ABPT_AnimFrame:
			return TEXT("anim_frame");
		case ABPT_None:
		default:
			return TEXT("none");
		}
	}

	static bool ParseAdditiveAnimationType(const FString& InValue, EAdditiveAnimationType& OutType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none"))
		{
			OutType = AAT_None;
			return true;
		}
		if (Normalized == TEXT("localspace") || Normalized == TEXT("local"))
		{
			OutType = AAT_LocalSpaceBase;
			return true;
		}
		if (Normalized == TEXT("meshspacerotationoffset") || Normalized == TEXT("meshspace") || Normalized == TEXT("rotationoffsetmeshspace"))
		{
			OutType = AAT_RotationOffsetMeshSpace;
			return true;
		}
		return false;
	}

	static bool ParseAdditiveBasePoseType(const FString& InValue, EAdditiveBasePoseType& OutType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none"))
		{
			OutType = ABPT_None;
			return true;
		}
		if (Normalized == TEXT("refpose") || Normalized == TEXT("referencepose"))
		{
			OutType = ABPT_RefPose;
			return true;
		}
		if (Normalized == TEXT("animscaled") || Normalized == TEXT("scaled"))
		{
			OutType = ABPT_AnimScaled;
			return true;
		}
		if (Normalized == TEXT("animframe") || Normalized == TEXT("frame"))
		{
			OutType = ABPT_AnimFrame;
			return true;
		}
		return false;
	}

	static const TCHAR* RootMotionLockTypeToString(const ERootMotionRootLock::Type Type)
	{
		switch (Type)
		{
		case ERootMotionRootLock::AnimFirstFrame:
			return TEXT("anim_first_frame");
		case ERootMotionRootLock::Zero:
			return TEXT("zero");
		case ERootMotionRootLock::RefPose:
		default:
			return TEXT("ref_pose");
		}
	}

	static bool ParseRootMotionLockType(const FString& InValue, ERootMotionRootLock::Type& OutType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("refpose") || Normalized == TEXT("referencepose"))
		{
			OutType = ERootMotionRootLock::RefPose;
			return true;
		}
		if (Normalized == TEXT("animfirstframe") || Normalized == TEXT("firstframe"))
		{
			OutType = ERootMotionRootLock::AnimFirstFrame;
			return true;
		}
		if (Normalized == TEXT("zero"))
		{
			OutType = ERootMotionRootLock::Zero;
			return true;
		}
		return false;
	}

	static FString NotifyTickTypeToString(const EMontageNotifyTickType::Type TickType)
	{
		return TickType == EMontageNotifyTickType::BranchingPoint ? TEXT("branching_point") : TEXT("queued");
	}

	static bool ParseNotifyTickType(const FString& InValue, EMontageNotifyTickType::Type& OutTickType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("queued") || Normalized == TEXT("queue"))
		{
			OutTickType = EMontageNotifyTickType::Queued;
			return true;
		}
		if (Normalized == TEXT("branchingpoint") || Normalized == TEXT("branch"))
		{
			OutTickType = EMontageNotifyTickType::BranchingPoint;
			return true;
		}
		return false;
	}

	static FString NotifyFilterTypeToString(const ENotifyFilterType::Type FilterType)
	{
		switch (FilterType)
		{
		case ENotifyFilterType::LOD:
			return TEXT("lod");
		case ENotifyFilterType::NoFiltering:
		default:
			return TEXT("none");
		}
	}

	static bool ParseNotifyFilterType(const FString& InValue, ENotifyFilterType::Type& OutFilterType)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none") || Normalized == TEXT("nofiltering") || Normalized == TEXT("nofilter"))
		{
			OutFilterType = ENotifyFilterType::NoFiltering;
			return true;
		}
		if (Normalized == TEXT("lod"))
		{
			OutFilterType = ENotifyFilterType::LOD;
			return true;
		}
		return false;
	}

	static bool ApplyNotifyCommonSettings(const TSharedPtr<FJsonObject>& Params, FAnimNotifyEvent& Notify, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		double TriggerWeightThreshold = 0.0;
		if (Params->TryGetNumberField(TEXT("trigger_weight_threshold"), TriggerWeightThreshold))
		{
			if (TriggerWeightThreshold < 0.0)
			{
				OutError = TEXT("invalid_trigger_weight_threshold");
				return false;
			}
			Notify.TriggerWeightThreshold = static_cast<float>(TriggerWeightThreshold);
		}

		double NotifyTriggerChance = 0.0;
		if (Params->TryGetNumberField(TEXT("notify_trigger_chance"), NotifyTriggerChance))
		{
			if (NotifyTriggerChance < 0.0 || NotifyTriggerChance > 1.0)
			{
				OutError = TEXT("invalid_notify_trigger_chance");
				return false;
			}
			Notify.NotifyTriggerChance = static_cast<float>(NotifyTriggerChance);
		}

		FString TickTypeText;
		if (Params->TryGetStringField(TEXT("tick_type"), TickTypeText) && !TickTypeText.TrimStartAndEnd().IsEmpty())
		{
			EMontageNotifyTickType::Type TickType = EMontageNotifyTickType::Queued;
			if (!ParseNotifyTickType(TickTypeText, TickType))
			{
				OutError = TEXT("invalid_tick_type");
				return false;
			}
			Notify.MontageTickType = TickType;
		}
		else
		{
			bool bBranchingPoint = (Notify.MontageTickType == EMontageNotifyTickType::BranchingPoint);
			if (Params->TryGetBoolField(TEXT("branching_point"), bBranchingPoint))
			{
				Notify.MontageTickType = bBranchingPoint ? EMontageNotifyTickType::BranchingPoint : EMontageNotifyTickType::Queued;
			}
		}

		FString FilterTypeText;
		if (Params->TryGetStringField(TEXT("notify_filter_type"), FilterTypeText) && !FilterTypeText.TrimStartAndEnd().IsEmpty())
		{
			ENotifyFilterType::Type FilterType = ENotifyFilterType::NoFiltering;
			if (!ParseNotifyFilterType(FilterTypeText, FilterType))
			{
				OutError = TEXT("invalid_notify_filter_type");
				return false;
			}
			Notify.NotifyFilterType = FilterType;
		}

		double NotifyFilterLod = 0.0;
		if (Params->TryGetNumberField(TEXT("notify_filter_lod"), NotifyFilterLod))
		{
			if (NotifyFilterLod < 0.0)
			{
				OutError = TEXT("invalid_notify_filter_lod");
				return false;
			}
			Notify.NotifyFilterLOD = static_cast<int32>(NotifyFilterLod);
		}

		bool BoolValue = false;
		if (Params->TryGetBoolField(TEXT("can_be_filtered_via_request"), BoolValue))
		{
			Notify.bCanBeFilteredViaRequest = BoolValue;
		}
		if (Params->TryGetBoolField(TEXT("trigger_on_dedicated_server"), BoolValue))
		{
			Notify.bTriggerOnDedicatedServer = BoolValue;
		}
		if (Params->TryGetBoolField(TEXT("trigger_on_follower"), BoolValue))
		{
			Notify.bTriggerOnFollower = BoolValue;
		}

		return true;
	}

	static FString GetSyncMarkerTrackName(const UAnimSequence* Sequence, const FAnimSyncMarker& Marker)
	{
#if WITH_EDITORONLY_DATA
		if (Sequence && Sequence->AnimNotifyTracks.IsValidIndex(Marker.TrackIndex))
		{
			return Sequence->AnimNotifyTracks[Marker.TrackIndex].TrackName.ToString();
		}
#endif
		return TEXT("");
	}

	static FString GetNotifyTrackName(const UAnimSequenceBase* Sequence, const int32 TrackIndex)
	{
#if WITH_EDITORONLY_DATA
		if (Sequence && Sequence->AnimNotifyTracks.IsValidIndex(TrackIndex))
		{
			return Sequence->AnimNotifyTracks[TrackIndex].TrackName.ToString();
		}
#endif
		return TEXT("");
	}

	static TSharedPtr<FJsonObject> BuildNotifyTrackJson(UAnimSequenceBase* Sequence, const FAnimNotifyTrack& Track, const int32 TrackIndex)
	{
		TArray<FAnimNotifyEvent> NotifyEvents;
		UAnimationBlueprintLibrary::GetAnimationNotifyEventsForTrack(Sequence, Track.TrackName, NotifyEvents);

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("track_index"), TrackIndex);
		Item->SetStringField(TEXT("track_name"), Track.TrackName.ToString());
		Item->SetStringField(TEXT("track_color"), Track.TrackColor.ToString());
		Item->SetObjectField(TEXT("track_color_linear"), BuildLinearColorJson(Track.TrackColor));
		Item->SetNumberField(TEXT("notify_count"), NotifyEvents.Num());
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildNotifyJson(const UAnimSequenceBase* Sequence, const FAnimNotifyEvent& Notify, const int32 NotifyIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("notify_index"), NotifyIndex);
		Item->SetStringField(TEXT("notify_name"), Notify.NotifyName.ToString());
		Item->SetStringField(TEXT("track_name"), GetNotifyTrackName(Sequence, Notify.TrackIndex));
		Item->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
		Item->SetNumberField(TEXT("time_seconds"), Notify.GetTime());
		Item->SetNumberField(TEXT("trigger_time_seconds"), Notify.GetTriggerTime());
		Item->SetNumberField(TEXT("duration_seconds"), Notify.GetDuration());
		Item->SetBoolField(TEXT("is_state"), Notify.NotifyStateClass != nullptr);
		Item->SetBoolField(TEXT("is_blueprint_notify"), Notify.IsBlueprintNotify());
		Item->SetBoolField(TEXT("branching_point"), Notify.MontageTickType == EMontageNotifyTickType::BranchingPoint);
		Item->SetStringField(TEXT("tick_type"), NotifyTickTypeToString(Notify.MontageTickType));
		Item->SetNumberField(TEXT("trigger_weight_threshold"), Notify.TriggerWeightThreshold);
		Item->SetNumberField(TEXT("notify_trigger_chance"), Notify.NotifyTriggerChance);
		Item->SetStringField(TEXT("notify_filter_type"), NotifyFilterTypeToString(Notify.NotifyFilterType));
		Item->SetNumberField(TEXT("notify_filter_lod"), Notify.NotifyFilterLOD);
		Item->SetBoolField(TEXT("can_be_filtered_via_request"), Notify.bCanBeFilteredViaRequest);
		Item->SetBoolField(TEXT("trigger_on_dedicated_server"), Notify.bTriggerOnDedicatedServer);
		Item->SetBoolField(TEXT("trigger_on_follower"), Notify.bTriggerOnFollower);
		Item->SetStringField(TEXT("notify_color"), Notify.NotifyColor.ToString());
		Item->SetObjectField(TEXT("notify_color_linear"), BuildLinearColorJson(FLinearColor(Notify.NotifyColor)));
		Item->SetStringField(TEXT("notify_class"), Notify.Notify ? Notify.Notify->GetClass()->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("notify_state_class"), Notify.NotifyStateClass ? Notify.NotifyStateClass->GetClass()->GetPathName() : TEXT(""));
#if WITH_EDITORONLY_DATA
		Item->SetStringField(TEXT("guid"), Notify.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
#endif
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildSyncMarkerJson(const UAnimSequence* Sequence, const FAnimSyncMarker& Marker, const int32 MarkerIndex)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("marker_index"), MarkerIndex);
		Item->SetStringField(TEXT("marker_name"), Marker.MarkerName.ToString());
		Item->SetNumberField(TEXT("time_seconds"), Marker.Time);
#if WITH_EDITORONLY_DATA
		Item->SetNumberField(TEXT("track_index"), Marker.TrackIndex);
		Item->SetStringField(TEXT("track_name"), GetSyncMarkerTrackName(Sequence, Marker));
		Item->SetStringField(TEXT("guid"), Marker.Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
#endif
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), BuildVectorJson(Value.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), BuildRotatorJson(Value.Rotator()));
		Obj->SetObjectField(TEXT("scale"), BuildVectorJson(Value.GetScale3D()));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildCurveJson(UAnimSequenceBase* Sequence, const FName CurveName, const ERawCurveTrackTypes CurveType, const bool bIncludeKeys)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("curve_name"), CurveName.ToString());
		Item->SetStringField(TEXT("curve_type"), CurveTypeToString(CurveType));

		TArray<TSharedPtr<FJsonValue>> Keys;
		switch (CurveType)
		{
		case ERawCurveTrackTypes::RCT_Float:
		default:
		{
			TArray<float> Times;
			TArray<float> Values;
			UAnimationBlueprintLibrary::GetFloatKeys(Sequence, CurveName, Times, Values);
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				if (!bIncludeKeys)
				{
					continue;
				}
				TSharedPtr<FJsonObject> KeyObject = MakeShared<FJsonObject>();
				KeyObject->SetNumberField(TEXT("time_seconds"), Times[Index]);
				KeyObject->SetNumberField(TEXT("value"), Values[Index]);
				Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
			}
			Item->SetNumberField(TEXT("key_count"), FMath::Min(Times.Num(), Values.Num()));
			break;
		}
		}

		if (bIncludeKeys)
		{
			Item->SetArrayField(TEXT("keys"), Keys);
		}
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildMetadataJson(const UAnimMetaData* MetaDataObject)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("class"), MetaDataObject ? MetaDataObject->GetClass()->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("object_path"), MetaDataObject ? MetaDataObject->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("name"), MetaDataObject ? MetaDataObject->GetName() : TEXT(""));
		if (MetaDataObject)
		{
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(MetaDataObject->GetClass()); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
				{
					continue;
				}

				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(MetaDataObject);
				if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
				{
					Properties->SetStringField(Property->GetName(), NameProperty->GetPropertyValue(ValuePtr).ToString());
				}
				else if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
				{
					Properties->SetStringField(Property->GetName(), StringProperty->GetPropertyValue(ValuePtr));
				}
				else if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
				{
					Properties->SetStringField(Property->GetName(), TextProperty->GetPropertyValue(ValuePtr).ToString());
				}
				else if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
				{
					Properties->SetBoolField(Property->GetName(), BoolProperty->GetPropertyValue(ValuePtr));
				}
				else if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
				{
					const double NumericValue = NumericProperty->IsFloatingPoint()
						? NumericProperty->GetFloatingPointPropertyValue(ValuePtr)
						: static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr));
					Properties->SetNumberField(Property->GetName(), NumericValue);
				}
			}
			Item->SetObjectField(TEXT("properties"), Properties);
		}
		return Item;
	}

	static FString NormalizeFieldKey(const FString& InValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		return Normalized;
	}

	static FProperty* FindEditablePropertyByFlexibleName(UClass* Class, const FString& InName)
	{
		if (!Class)
		{
			return nullptr;
		}

		const FString NormalizedTarget = NormalizeFieldKey(InName);
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			if (NormalizeFieldKey(Property->GetName()) == NormalizedTarget)
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ApplySimpleJsonValueToProperty(UObject* TargetObject, FProperty* Property, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (!TargetObject || !Property || !JsonValue.IsValid())
		{
			OutError = TEXT("invalid_metadata_property_update");
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(TargetObject);
		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*JsonValue->AsString().TrimStartAndEnd()));
			return true;
		}
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			StringProperty->SetPropertyValue(ValuePtr, JsonValue->AsString());
			return true;
		}
		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(JsonValue->AsString()));
			return true;
		}
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(ValuePtr, JsonValue->AsBool());
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			const double NumericValue = JsonValue->AsNumber();
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(ValuePtr, NumericValue);
			}
			else
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumericValue));
			}
			return true;
		}

		OutError = TEXT("unsupported_metadata_property_type");
		return false;
	}

	static TSharedPtr<FJsonObject> BuildSocketJson(const USkeletalMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("socket_name"), Socket ? Socket->SocketName.ToString() : TEXT(""));
		Item->SetStringField(TEXT("bone_name"), Socket ? Socket->BoneName.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("relative_location"), BuildVectorJson(Socket ? Socket->RelativeLocation : FVector::ZeroVector));
		Item->SetObjectField(TEXT("relative_rotation"), BuildRotatorJson(Socket ? Socket->RelativeRotation : FRotator::ZeroRotator));
		Item->SetObjectField(TEXT("relative_scale"), BuildVectorJson(Socket ? Socket->RelativeScale : FVector::OneVector));
		return Item;
	}

	static TSharedPtr<FJsonObject> BuildVirtualBoneJson(const FVirtualBone& VirtualBone)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source_bone_name"), VirtualBone.SourceBoneName.ToString());
		Item->SetStringField(TEXT("target_bone_name"), VirtualBone.TargetBoneName.ToString());
		Item->SetStringField(TEXT("virtual_bone_name"), VirtualBone.VirtualBoneName.ToString());
		return Item;
	}

	static USkeletalMeshSocket* FindSocketByName(USkeleton* Skeleton, const FName SocketName)
	{
		if (!Skeleton)
		{
			return nullptr;
		}

		for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
		{
			if (Socket && Socket->SocketName == SocketName)
			{
				return Socket;
			}
		}
		return nullptr;
	}

	static const FVirtualBone* FindVirtualBoneByName(const USkeleton* Skeleton, const FName VirtualBoneName)
	{
		if (!Skeleton)
		{
			return nullptr;
		}

		for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
		{
			if (VirtualBone.VirtualBoneName == VirtualBoneName)
			{
				return &VirtualBone;
			}
		}
		return nullptr;
	}

	static int32 FindNotifyTrackIndexByName(const UAnimSequenceBase* Sequence, const FName TrackName)
	{
#if WITH_EDITORONLY_DATA
		if (Sequence)
		{
			for (int32 TrackIndex = 0; TrackIndex < Sequence->AnimNotifyTracks.Num(); ++TrackIndex)
			{
				if (Sequence->AnimNotifyTracks[TrackIndex].TrackName.IsEqual(TrackName, ENameCase::IgnoreCase))
				{
					return TrackIndex;
				}
			}
		}
#endif
		return INDEX_NONE;
	}

	static bool EnsureNotifyTrack(UAnimSequenceBase* Sequence, const FName TrackName, int32& OutTrackIndex)
	{
		OutTrackIndex = FindNotifyTrackIndexByName(Sequence, TrackName);
		if (OutTrackIndex != INDEX_NONE)
		{
			return true;
		}

		UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Sequence, TrackName, FLinearColor::White);
		OutTrackIndex = FindNotifyTrackIndexByName(Sequence, TrackName);
		return OutTrackIndex != INDEX_NONE;
	}

	static UClass* LoadClassByPath(const FString& InPath)
	{
		FString ClassPath = ToObjectPath(InPath);
		if (ClassPath.IsEmpty())
		{
			ClassPath = InPath.TrimStartAndEnd();
		}
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *(ClassPath + TEXT("_C"))))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}
}

namespace UeAgentAnimationEditorOps
{
	static IAnimationEditor* GetAnimationEditor(UAnimationAsset* AnimationAsset, const bool bOpenIfNeeded, FString& OutError)
	{
		if (!AnimationAsset)
		{
			OutError = TEXT("animation_asset_not_found");
			return nullptr;
		}
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			OutError = TEXT("asset_editor_subsystem_unavailable");
			return nullptr;
		}

		if (bOpenIfNeeded)
		{
			AssetEditorSubsystem->OpenEditorForAsset(AnimationAsset);
		}

		IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(AnimationAsset, true);
		if (!AssetEditor)
		{
			OutError = bOpenIfNeeded ? TEXT("animation_editor_open_failed") : TEXT("animation_editor_not_opened");
			return nullptr;
		}

		return static_cast<IAnimationEditor*>(AssetEditor);
	}

	static bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		bool bFoundAny = false;
		if (RootWidget->GetType() == TypeName)
		{
			OutWidgets.Add(RootWidget);
			bFoundAny = true;
		}

		FChildren* Children = RootWidget->GetChildren();
		if (!Children)
		{
			return bFoundAny;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			bFoundAny |= FindWidgetByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidgets);
		}
		return bFoundAny;
	}

	static TSharedPtr<SWindow> ResolveAnimationEditorWindow(IAnimationEditor* AnimationEditor, FString& OutError)
	{
		if (!AnimationEditor)
		{
			OutError = TEXT("animation_editor_not_opened");
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = AnimationEditor->GetAssociatedTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("animation_tab_manager_not_found");
			return nullptr;
		}

		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = TEXT("animation_owner_tab_not_found");
			return nullptr;
		}

		const TSharedPtr<SWindow> ParentWindow = OwnerTab->GetParentWindow();
		if (!ParentWindow.IsValid())
		{
			OutError = TEXT("animation_window_not_found");
			return nullptr;
		}
		return ParentWindow;
	}

	static TSharedPtr<SEditorViewport> ResolveAnimationEditorViewportWidget(IAnimationEditor* AnimationEditor, FString& OutError)
	{
		const TSharedPtr<SWindow> AnimationWindow = ResolveAnimationEditorWindow(AnimationEditor, OutError);
		if (!AnimationWindow.IsValid())
		{
			return nullptr;
		}

		TArray<TSharedRef<SWidget>> CandidateWidgets;
		FindWidgetByTypeRecursive(AnimationWindow.ToSharedRef(), FName(TEXT("SAnimationEditorViewport")), CandidateWidgets);
		if (CandidateWidgets.Num() <= 0)
		{
			FindWidgetByTypeRecursive(AnimationWindow.ToSharedRef(), FName(TEXT("SEditorViewport")), CandidateWidgets);
		}
		if (CandidateWidgets.Num() <= 0)
		{
			OutError = TEXT("animation_viewport_widget_not_found");
			return nullptr;
		}

		TSharedPtr<SWidget> BestWidget;
		float BestArea = -1.0f;
		for (const TSharedRef<SWidget>& Candidate : CandidateWidgets)
		{
			const FVector2D Size = Candidate->GetTickSpaceGeometry().GetAbsoluteSize();
			const float Area = Size.X * Size.Y;
			if (Area > BestArea)
			{
				BestArea = Area;
				BestWidget = Candidate;
			}
		}

		const TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(BestWidget);
		if (!ViewportWidget.IsValid() || !ViewportWidget->GetViewportClient().IsValid())
		{
			OutError = TEXT("animation_viewport_client_not_found");
			return nullptr;
		}
		return ViewportWidget;
	}

	static TSharedPtr<SWidget> ResolveAnimationViewportSceneWidget(const TSharedRef<SEditorViewport>& ViewportWidget)
	{
		TArray<TSharedRef<SWidget>> ViewportChildren;
		FindWidgetByTypeRecursive(ViewportWidget, FName(TEXT("SViewport")), ViewportChildren);
		if (ViewportChildren.Num() <= 0)
		{
			return nullptr;
		}

		TSharedRef<SWidget> BestWidget = ViewportChildren[0];
		float BestArea = 0.0f;
		for (const TSharedRef<SWidget>& Candidate : ViewportChildren)
		{
			const FVector2D Size = Candidate->GetTickSpaceGeometry().GetAbsoluteSize();
			const float Area = Size.X * Size.Y;
			if (Area > BestArea)
			{
				BestArea = Area;
				BestWidget = Candidate;
			}
		}
		return BestWidget;
	}

	static void PrepareSlateWindowForCapture(const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();
		FSlateApplication::Get().ForceRedrawWindow(Window.ToSharedRef());
	}

	static bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		FIntVector ShotSize(0, 0, 0);
		if (!FSlateApplication::Get().TakeScreenshot(Widget, OutPixels, ShotSize))
		{
			OutError = TEXT("widget_screenshot_failed");
			return false;
		}
		if (ShotSize.X <= 0 || ShotSize.Y <= 0 || OutPixels.Num() <= 0)
		{
			OutError = TEXT("widget_screenshot_empty");
			return false;
		}

		OutSize = FIntPoint(ShotSize.X, ShotSize.Y);
		return true;
	}

	static bool ScreenshotEditorViewportPixels(const TSharedRef<SEditorViewport>& ViewportWidget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (!ViewportClient.IsValid())
		{
			OutError = TEXT("animation_viewport_client_not_found");
			return false;
		}

		FViewport* RawViewport = ViewportClient->Viewport;
		if (!RawViewport)
		{
			OutError = TEXT("animation_viewport_not_found");
			return false;
		}

		const FIntPoint ViewportSize = RawViewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = TEXT("animation_scene_viewport_zero_size");
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(true);
		OutPixels.Reset();
		if (!RawViewport->ReadPixels(OutPixels, ReadFlags))
		{
			OutError = TEXT("animation_scene_viewport_read_pixels_failed");
			return false;
		}
		if (OutPixels.Num() <= 0)
		{
			OutError = TEXT("animation_scene_viewport_empty_pixels");
			return false;
		}

		OutSize = ViewportSize;
		return true;
	}

	static bool BuildWidgetCropRectInWindowPixels(
		const TSharedRef<SWindow>& Window,
		const TSharedRef<SWidget>& Widget,
		const FIntPoint& WindowShotSize,
		FIntRect& OutRect,
		FString& OutError)
	{
		const FGeometry& WindowGeometry = Window->GetTickSpaceGeometry();
		const FGeometry& WidgetGeometry = Widget->GetTickSpaceGeometry();

		const FVector2D WindowLocalSize = WindowGeometry.GetLocalSize();
		const FVector2D WidgetAbsPos = WidgetGeometry.GetAbsolutePosition();
		const FVector2D WidgetAbsSize = WidgetGeometry.GetAbsoluteSize();
		const FVector2D LocalTopLeft = WindowGeometry.AbsoluteToLocal(WidgetAbsPos);
		const FVector2D LocalBottomRight = WindowGeometry.AbsoluteToLocal(WidgetAbsPos + WidgetAbsSize);

		if (WindowLocalSize.X <= 1.0 || WindowLocalSize.Y <= 1.0)
		{
			OutError = TEXT("animation_window_geometry_invalid");
			return false;
		}
		if (WidgetAbsSize.X <= 1.0 || WidgetAbsSize.Y <= 1.0)
		{
			OutError = TEXT("animation_widget_geometry_invalid");
			return false;
		}

		const double ScaleX = (double)WindowShotSize.X / (double)WindowLocalSize.X;
		const double ScaleY = (double)WindowShotSize.Y / (double)WindowLocalSize.Y;
		int32 Left = FMath::FloorToInt(LocalTopLeft.X * ScaleX);
		int32 Top = FMath::FloorToInt(LocalTopLeft.Y * ScaleY);
		int32 Right = FMath::CeilToInt(LocalBottomRight.X * ScaleX);
		int32 Bottom = FMath::CeilToInt(LocalBottomRight.Y * ScaleY);

		Left = FMath::Clamp(Left, 0, WindowShotSize.X - 1);
		Top = FMath::Clamp(Top, 0, WindowShotSize.Y - 1);
		Right = FMath::Clamp(Right, Left + 1, WindowShotSize.X);
		Bottom = FMath::Clamp(Bottom, Top + 1, WindowShotSize.Y);
		if (Right <= Left || Bottom <= Top)
		{
			OutError = TEXT("animation_widget_crop_invalid");
			return false;
		}

		OutRect = FIntRect(Left, Top, Right, Bottom);
		return true;
	}

	static bool CropPixelsRect(
		const TArray<FColor>& InPixels,
		const FIntPoint& InSize,
		const FIntRect& CropRect,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != InSize.X * InSize.Y)
		{
			OutError = TEXT("invalid_source_pixels");
			return false;
		}

		const int32 CropWidth = CropRect.Width();
		const int32 CropHeight = CropRect.Height();
		if (CropWidth <= 0 || CropHeight <= 0)
		{
			OutError = TEXT("invalid_crop_rect");
			return false;
		}

		OutPixels.Reset();
		OutPixels.AddUninitialized(CropWidth * CropHeight);
		for (int32 Row = 0; Row < CropHeight; ++Row)
		{
			const int32 SourceIndex = (CropRect.Min.Y + Row) * InSize.X + CropRect.Min.X;
			const int32 DestIndex = Row * CropWidth;
			FMemory::Memcpy(
				OutPixels.GetData() + DestIndex,
				InPixels.GetData() + SourceIndex,
				sizeof(FColor) * CropWidth);
		}

		OutSize = FIntPoint(CropWidth, CropHeight);
		return true;
	}

	static bool ScreenshotAnimationEditorWindowCropWidget(
		IAnimationEditor* AnimationEditor,
		const TSharedRef<SWidget>& Widget,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		const TSharedPtr<SWindow> AnimationWindow = ResolveAnimationEditorWindow(AnimationEditor, OutError);
		if (!AnimationWindow.IsValid())
		{
			return false;
		}

		PrepareSlateWindowForCapture(AnimationWindow);

		TArray<FColor> WindowPixels;
		FIntPoint WindowSize(0, 0);
		if (!ScreenshotSlateWidget(AnimationWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			return false;
		}

		PrepareSlateWindowForCapture(AnimationWindow);
		if (!ScreenshotSlateWidget(AnimationWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			return false;
		}

		FIntRect CropRect;
		if (!BuildWidgetCropRectInWindowPixels(AnimationWindow.ToSharedRef(), Widget, WindowSize, CropRect, OutError))
		{
			return false;
		}

		return CropPixelsRect(WindowPixels, WindowSize, CropRect, OutPixels, OutSize, OutError);
	}

	static void PrepareAnimationViewportForCapture(
		IAnimationEditor* AnimationEditor,
		const TSharedPtr<SEditorViewport>& ViewportWidget,
		UDebugSkelMeshComponent* PreviewMeshComponent,
		const bool bFocusViewport)
	{
		if (ViewportWidget.IsValid())
		{
			if (const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
			{
				const bool bNeedsInitialFocus =
					ViewportClient->GetViewLocation().IsNearlyZero(0.01f) &&
					ViewportClient->GetViewRotation().Equals(FRotator::ZeroRotator, 0.01f);
				if (PreviewMeshComponent && (bFocusViewport || bNeedsInitialFocus))
				{
					ViewportClient->FocusViewportOnBox(PreviewMeshComponent->Bounds.GetBox(), true);
				}
				ViewportClient->Invalidate();
			}

			if (const TSharedPtr<FSceneViewport> SceneViewport = ViewportWidget->GetSceneViewport())
			{
				SceneViewport->InvalidateDisplay();
				SceneViewport->DeferInvalidateHitProxy();
			}
		}

		FString WindowError;
		const TSharedPtr<SWindow> AnimationWindow = ResolveAnimationEditorWindow(AnimationEditor, WindowError);
		if (AnimationWindow.IsValid())
		{
			PrepareSlateWindowForCapture(AnimationWindow);
		}
	}

	static bool JsonTryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static bool ResolvePreviewFrame(UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Params, int32& OutFrameIndex, float& OutTimeSeconds, FString& OutError)
	{
		if (!Sequence || !Params.IsValid())
		{
			OutError = TEXT("invalid_anim_sequence");
			return false;
		}

		const int32 MaxFrameIndex = FMath::Max(0, Sequence->GetNumberOfSampledKeys() - 1);
		const FFrameRate SamplingFrameRate = Sequence->GetSamplingFrameRate();

		double TimeSecondsValue = 0.0;
		if (JsonTryGetNumberField(Params, TEXT("time_seconds"), TimeSecondsValue))
		{
			const float SequenceLength = Sequence->GetPlayLength();
			OutTimeSeconds = FMath::Clamp(static_cast<float>(TimeSecondsValue), 0.0f, SequenceLength);
			OutFrameIndex = FMath::Clamp(SamplingFrameRate.AsFrameTime(OutTimeSeconds).RoundToFrame().Value, 0, MaxFrameIndex);
			return true;
		}

		double FrameIndexValue = 0.0;
		const bool bHasFrame =
			JsonTryGetNumberField(Params, TEXT("frame_index"), FrameIndexValue)
			|| JsonTryGetNumberField(Params, TEXT("frame"), FrameIndexValue);
		if (!bHasFrame)
		{
			FrameIndexValue = 0.0;
		}

		const int32 RequestedFrameIndex = static_cast<int32>(FrameIndexValue);
		if (RequestedFrameIndex < 0 || RequestedFrameIndex > MaxFrameIndex)
		{
			OutError = TEXT("invalid_frame_index");
			return false;
		}

		OutFrameIndex = RequestedFrameIndex;
		OutTimeSeconds = static_cast<float>(SamplingFrameRate.AsSeconds(FFrameNumber(OutFrameIndex)));
		return true;
	}

	static bool PrepareAnimationPreviewAtFrame(
		IAnimationEditor* AnimationEditor,
		UAnimSequence* Sequence,
		USkeletalMesh* OverridePreviewMesh,
		const int32 FrameIndex,
		float& OutTimeSeconds,
		FString& OutPreviewMeshPath,
		FString& OutError)
	{
		if (!AnimationEditor || !Sequence)
		{
			OutError = TEXT("invalid_anim_sequence");
			return false;
		}

		AnimationEditor->SetAnimationAsset(Sequence);
		const TSharedRef<IPersonaToolkit> PersonaToolkit = AnimationEditor->GetPersonaToolkit();
		const TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit->GetPreviewScene();

		USkeletalMesh* EffectivePreviewMesh = OverridePreviewMesh;
		if (!EffectivePreviewMesh)
		{
			EffectivePreviewMesh = Sequence->GetPreviewMesh();
		}
		if (!EffectivePreviewMesh && Sequence->GetSkeleton())
		{
			EffectivePreviewMesh = Sequence->GetSkeleton()->GetPreviewMesh(true);
		}
		if (EffectivePreviewMesh)
		{
			PreviewScene->SetPreviewMesh(EffectivePreviewMesh, true);
		}

		PreviewScene->SetPreviewAnimationAsset(Sequence, true);
		PreviewScene->ShowReferencePose(false, true);

		UDebugSkelMeshComponent* PreviewMeshComponent = PreviewScene->GetPreviewMeshComponent();
		if (!PreviewMeshComponent)
		{
			OutError = TEXT("preview_mesh_component_not_found");
			return false;
		}
		if (!PreviewMeshComponent->PreviewInstance)
		{
			PreviewMeshComponent->InitAnim(true);
		}
		if (!PreviewMeshComponent->PreviewInstance)
		{
			OutError = TEXT("preview_instance_not_found");
			return false;
		}

		const FFrameRate SamplingFrameRate = Sequence->GetSamplingFrameRate();
		OutTimeSeconds = static_cast<float>(SamplingFrameRate.AsSeconds(FFrameNumber(FrameIndex)));
		const float TickDeltaSeconds =
			SamplingFrameRate.IsValid() && SamplingFrameRate.AsDecimal() > KINDA_SMALL_NUMBER
				? static_cast<float>(1.0 / SamplingFrameRate.AsDecimal())
				: (1.0f / 30.0f);

		PreviewMeshComponent->PreviewInstance->SetAnimationAsset(Sequence, true, 1.0f);
		PreviewMeshComponent->PreviewInstance->SetPlaying(false);
		PreviewMeshComponent->PreviewInstance->SetPosition(OutTimeSeconds, false);
		PreviewMeshComponent->GlobalAnimRateScale = 0.0f;
		PreviewMeshComponent->TickAnimation(0.0f, false);
		PreviewMeshComponent->RefreshBoneTransforms(nullptr);
		PreviewMeshComponent->RefreshFollowerComponents();
		PreviewMeshComponent->UpdateComponentToWorld();
		PreviewScene->Tick(TickDeltaSeconds);
		OutTimeSeconds = PreviewMeshComponent->PreviewInstance->GetCurrentTime();
		PreviewScene->InvalidateViews();

		OutPreviewMeshPath = PreviewMeshComponent->GetSkeletalMeshAsset() ? PreviewMeshComponent->GetSkeletalMeshAsset()->GetPathName() : TEXT("");
		if (OutPreviewMeshPath.IsEmpty())
		{
			OutError = TEXT("preview_skeletal_mesh_missing");
			return false;
		}
		return true;
	}
}

namespace UeAgentAnimationViewportOps
{
	static FLevelEditorViewportClient* GetPreferredLevelViewportClient()
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
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			return GCurrentLevelEditingViewportClient;
		}

		if (GEditor)
		{
			FLevelEditorViewportClient* FirstValid = nullptr;
			FLevelEditorViewportClient* BestPerspectiveByArea = nullptr;
			int64 BestPerspectiveArea = -1;
			FLevelEditorViewportClient* BestAnyByArea = nullptr;
			int64 BestAnyArea = -1;

			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (!ViewportClient || !ViewportClient->Viewport)
				{
					continue;
				}
				if (!FirstValid)
				{
					FirstValid = ViewportClient;
				}

				const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
				const int64 Area = static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
				if (Area > BestAnyArea)
				{
					BestAnyArea = Area;
					BestAnyByArea = ViewportClient;
				}
				if (ViewportClient->ViewportType == LVT_Perspective && Area > BestPerspectiveArea)
				{
					BestPerspectiveArea = Area;
					BestPerspectiveByArea = ViewportClient;
				}
			}

			if (BestPerspectiveByArea)
			{
				return BestPerspectiveByArea;
			}
			if (BestAnyByArea)
			{
				return BestAnyByArea;
			}
			return FirstValid;
		}

		return nullptr;
	}
}

bool FUeAgentHttpServer::ExecuteAnimSequenceCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("anim_sequence_get_info")) return CmdAnimSequenceGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_screenshot")) return CmdAnimSequenceScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_settings")) return CmdAnimSequenceSetSettings(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_preview_mesh")) return CmdAnimSequenceSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_curve")) return CmdAnimSequenceSetCurve(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_bones")) return CmdAnimSequenceSetBones(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_metadata")) return CmdAnimSequenceSetMetadata(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_notify")) return CmdAnimSequenceSetNotify(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_notify_track")) return CmdAnimSequenceSetNotifyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_sync_markers")) return CmdAnimSequenceSetSyncMarkers(Ctx, OutData, OutError);

	OutError = TEXT("unknown_anim_sequence_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteSkeletonCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("skeleton_get_info")) return CmdSkeletonGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_list_bones")) return CmdSkeletonListBones(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_compatible_skeletons")) return CmdSkeletonSetCompatibleSkeletons(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_preview_mesh")) return CmdSkeletonSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_socket")) return CmdSkeletonSetSocket(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_virtual_bone")) return CmdSkeletonSetVirtualBone(Ctx, OutData, OutError);

	OutError = TEXT("unknown_skeleton_command");
	return false;
}

bool FUeAgentHttpServer::CmdAnimSequenceGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	float SequenceLength = 0.0f;
	float RateScale = 0.0f;
	int32 NumFrames = 0;
	EAnimInterpolationType InterpolationType = EAnimInterpolationType::Linear;
	UAnimationBlueprintLibrary::GetSequenceLength(Sequence, SequenceLength);
	UAnimationBlueprintLibrary::GetRateScale(Sequence, RateScale);
	UAnimationBlueprintLibrary::GetNumFrames(Sequence, NumFrames);
	UAnimationBlueprintLibrary::GetAnimationInterpolationType(Sequence, InterpolationType);

	TEnumAsByte<EAdditiveAnimationType> AdditiveAnimationType = AAT_None;
	TEnumAsByte<EAdditiveBasePoseType> AdditiveBasePoseType = ABPT_None;
	TEnumAsByte<ERootMotionRootLock::Type> RootMotionLockType = ERootMotionRootLock::RefPose;
	UAnimBoneCompressionSettings* BoneCompressionSettings = nullptr;
	UAnimCurveCompressionSettings* CurveCompressionSettings = nullptr;
	UVariableFrameStrippingSettings* VariableFrameStrippingSettings = nullptr;
	UAnimationBlueprintLibrary::GetAdditiveAnimationType(Sequence, AdditiveAnimationType);
	UAnimationBlueprintLibrary::GetAdditiveBasePoseType(Sequence, AdditiveBasePoseType);
	UAnimationBlueprintLibrary::GetRootMotionLockType(Sequence, RootMotionLockType);
	UAnimationBlueprintLibrary::GetBoneCompressionSettings(Sequence, BoneCompressionSettings);
	UAnimationBlueprintLibrary::GetCurveCompressionSettings(Sequence, CurveCompressionSettings);
	UAnimationBlueprintLibrary::GetVariableFrameStrippingSettings(Sequence, VariableFrameStrippingSettings);

	TArray<TSharedPtr<FJsonValue>> NotifyTracks;
#if WITH_EDITORONLY_DATA
	for (int32 TrackIndex = 0; TrackIndex < Sequence->AnimNotifyTracks.Num(); ++TrackIndex)
	{
		NotifyTracks.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildNotifyTrackJson(Sequence, Sequence->AnimNotifyTracks[TrackIndex], TrackIndex)));
	}
#endif

	TArray<TSharedPtr<FJsonValue>> Notifies;
	for (int32 NotifyIndex = 0; NotifyIndex < Sequence->Notifies.Num(); ++NotifyIndex)
	{
		Notifies.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildNotifyJson(Sequence, Sequence->Notifies[NotifyIndex], NotifyIndex)));
	}

	TArray<TSharedPtr<FJsonValue>> SyncMarkers;
	for (int32 MarkerIndex = 0; MarkerIndex < Sequence->AuthoredSyncMarkers.Num(); ++MarkerIndex)
	{
		SyncMarkers.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSyncMarkerJson(Sequence, Sequence->AuthoredSyncMarkers[MarkerIndex], MarkerIndex)));
	}

	TArray<FName> UniqueMarkerNames;
	UAnimationBlueprintLibrary::GetUniqueMarkerNames(Sequence, UniqueMarkerNames);
	TArray<TSharedPtr<FJsonValue>> UniqueMarkerNamesJson;
	for (const FName MarkerName : UniqueMarkerNames)
	{
		UniqueMarkerNamesJson.Add(MakeShared<FJsonValueString>(MarkerName.ToString()));
	}

	TArray<FName> UniqueNotifyNames;
	UAnimationBlueprintLibrary::GetAnimationNotifyEventNames(Sequence, UniqueNotifyNames);
	TArray<TSharedPtr<FJsonValue>> UniqueNotifyNamesJson;
	for (const FName NotifyName : UniqueNotifyNames)
	{
		UniqueNotifyNamesJson.Add(MakeShared<FJsonValueString>(NotifyName.ToString()));
	}

	TArray<FName> TrackNames;
	UAnimationBlueprintLibrary::GetAnimationTrackNames(Sequence, TrackNames);
	TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
	for (const FName TrackName : TrackNames)
	{
		TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackName.ToString()));
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

	bool bIncludeCurveKeys = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_curve_keys"), bIncludeCurveKeys);

	TArray<TSharedPtr<FJsonValue>> Curves;
	{
		TArray<FName> CurveNames;
		UAnimationBlueprintLibrary::GetAnimationCurveNames(Sequence, ERawCurveTrackTypes::RCT_Float, CurveNames);
		for (const FName CurveName : CurveNames)
		{
			Curves.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildCurveJson(Sequence, CurveName, ERawCurveTrackTypes::RCT_Float, bIncludeCurveKeys)));
		}
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("skeleton"), Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Sequence->GetPreviewMesh() ? Sequence->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("sequence_length"), SequenceLength);
	OutData->SetNumberField(TEXT("rate_scale"), RateScale);
	OutData->SetNumberField(TEXT("num_frames"), NumFrames);
	OutData->SetStringField(TEXT("interpolation_type"), UeAgentAnimationAssetOps::InterpolationTypeToString(InterpolationType));
	OutData->SetStringField(TEXT("additive_animation_type"), UeAgentAnimationAssetOps::AdditiveAnimationTypeToString(AdditiveAnimationType));
	OutData->SetStringField(TEXT("additive_base_pose_type"), UeAgentAnimationAssetOps::AdditiveBasePoseTypeToString(AdditiveBasePoseType));
	OutData->SetStringField(TEXT("additive_base_pose_sequence"), Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("additive_base_pose_frame_index"), Sequence->RefFrameIndex);
	OutData->SetBoolField(TEXT("root_motion_enabled"), UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence));
	OutData->SetBoolField(TEXT("force_root_lock"), UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence));
	OutData->SetStringField(TEXT("root_motion_lock_type"), UeAgentAnimationAssetOps::RootMotionLockTypeToString(RootMotionLockType));
	OutData->SetStringField(TEXT("retarget_source"), Sequence->RetargetSource.ToString());
	OutData->SetStringField(TEXT("retarget_source_asset"), Sequence->GetRetargetSourceAsset().ToSoftObjectPath().ToString());
	OutData->SetStringField(TEXT("bone_compression_settings"), BoneCompressionSettings ? BoneCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("curve_compression_settings"), CurveCompressionSettings ? CurveCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("variable_frame_stripping_settings"), VariableFrameStrippingSettings ? VariableFrameStrippingSettings->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("metadata_count"), MetaDataJson.Num());
	OutData->SetArrayField(TEXT("metadata"), MetaDataJson);
	OutData->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
	OutData->SetArrayField(TEXT("track_names"), TrackNamesJson);
	OutData->SetNumberField(TEXT("curve_count"), Curves.Num());
	OutData->SetArrayField(TEXT("curves"), Curves);
	OutData->SetNumberField(TEXT("notify_count"), Notifies.Num());
	OutData->SetArrayField(TEXT("notifies"), Notifies);
	OutData->SetNumberField(TEXT("notify_track_count"), NotifyTracks.Num());
	OutData->SetArrayField(TEXT("notify_tracks"), NotifyTracks);
	OutData->SetNumberField(TEXT("sync_marker_count"), SyncMarkers.Num());
	OutData->SetArrayField(TEXT("sync_markers"), SyncMarkers);
	OutData->SetArrayField(TEXT("unique_marker_names"), UniqueMarkerNamesJson);
	OutData->SetArrayField(TEXT("unique_notify_names"), UniqueNotifyNamesJson);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);

	USkeletalMesh* OverridePreviewMesh = nullptr;
	if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		OverridePreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!OverridePreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && OverridePreviewMesh->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(OverridePreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_mesh_incompatible_with_sequence_skeleton");
			return false;
		}
	}

	int32 FrameIndex = 0;
	float TimeSeconds = 0.0f;
	if (!UeAgentAnimationEditorOps::ResolvePreviewFrame(Sequence, Ctx.Params, FrameIndex, TimeSeconds, OutError))
	{
		return false;
	}

	USkeletalMesh* EffectivePreviewMesh = OverridePreviewMesh ? OverridePreviewMesh : Sequence->GetPreviewMesh();
	if (!EffectivePreviewMesh && Sequence->GetSkeleton())
	{
		EffectivePreviewMesh = Sequence->GetSkeleton()->GetPreviewMesh(true);
	}
	if (!EffectivePreviewMesh)
	{
		OutError = TEXT("preview_skeletal_mesh_missing");
		return false;
	}
	const FString EffectivePreviewMeshPath = EffectivePreviewMesh->GetPathName();

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

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 1024;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);

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
	if (Target != TEXT("viewport"))
	{
		OutError = TEXT("invalid_target");
		return false;
	}

	FIntPoint BaseCaptureSize(1280, 720);
	if (FLevelEditorViewportClient* LevelViewportClient = UeAgentAnimationViewportOps::GetPreferredLevelViewportClient())
	{
		if (LevelViewportClient->Viewport)
		{
			const FIntPoint ViewportSize = LevelViewportClient->Viewport->GetSizeXY();
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

	struct FPreviewCaptureCleanup
	{
		UWorld* World = nullptr;
		ASkeletalMeshActor* PreviewActor = nullptr;
		AStaticMeshActor* BackdropActor = nullptr;
		ASceneCapture2D* CaptureActor = nullptr;
		ADirectionalLight* KeyLightActor = nullptr;
		APointLight* FillLightActor = nullptr;
		UTextureRenderTarget2D* RenderTarget = nullptr;

		~FPreviewCaptureCleanup()
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
			if (World && BackdropActor && IsValid(BackdropActor))
			{
				World->DestroyActor(BackdropActor, false, false);
			}
			if (World && PreviewActor && IsValid(PreviewActor))
			{
				World->DestroyActor(PreviewActor, false, false);
			}
		}
	};
	FPreviewCaptureCleanup Cleanup;

	Cleanup.World = World;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(World, ASkeletalMeshActor::StaticClass(), TEXT("UeAgentAnimSequencePreview"));
	SpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bTemporaryEditorActor = true;
	SpawnParams.bHideFromSceneOutliner = true;

	Cleanup.PreviewActor = World->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!Cleanup.PreviewActor)
	{
		OutError = TEXT("preview_actor_spawn_failed");
		return false;
	}

	Cleanup.PreviewActor->SetActorEnableCollision(false);
	Cleanup.PreviewActor->SetIsTemporarilyHiddenInEditor(false);
	Cleanup.PreviewActor->SetFolderPath(NAME_None);

	USkeletalMeshComponent* PreviewMeshComponent = Cleanup.PreviewActor->GetSkeletalMeshComponent();
	if (!PreviewMeshComponent)
	{
		OutError = TEXT("preview_mesh_component_not_found");
		return false;
	}

	const FFrameRate SamplingFrameRate = Sequence->GetSamplingFrameRate();
	const float TickDeltaSeconds =
		SamplingFrameRate.IsValid() && SamplingFrameRate.AsDecimal() > KINDA_SMALL_NUMBER
			? static_cast<float>(1.0 / SamplingFrameRate.AsDecimal())
			: (1.0f / 30.0f);

	PreviewMeshComponent->SetSkeletalMesh(EffectivePreviewMesh);
	PreviewMeshComponent->SetCastShadow(false);
	PreviewMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	PreviewMeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	PreviewMeshComponent->SetAnimation(Sequence);
	PreviewMeshComponent->SetPlayRate(0.0f);
	PreviewMeshComponent->Stop();
	PreviewMeshComponent->SetPosition(TimeSeconds, false);
	PreviewMeshComponent->TickAnimation(0.0f, false);
	PreviewMeshComponent->RefreshBoneTransforms(nullptr);
	PreviewMeshComponent->RefreshFollowerComponents();
	PreviewMeshComponent->UpdateComponentToWorld();
	World->Tick(LEVELTICK_All, TickDeltaSeconds);
	PreviewMeshComponent->SetPosition(TimeSeconds, false);
	PreviewMeshComponent->TickAnimation(0.0f, false);
	PreviewMeshComponent->RefreshBoneTransforms(nullptr);
	PreviewMeshComponent->RefreshFollowerComponents();
	Cleanup.PreviewActor->SetActorLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
	PreviewMeshComponent->SetRelativeLocation(FVector::ZeroVector);
	PreviewMeshComponent->SetRelativeRotation(FRotator::ZeroRotator);
	PreviewMeshComponent->bPauseAnims = true;
	PreviewMeshComponent->SetComponentTickEnabled(false);
	PreviewMeshComponent->UpdateComponentToWorld();
	PreviewMeshComponent->MarkRenderStateDirty();
	FlushRenderingCommands();

	Cleanup.RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!Cleanup.RenderTarget)
	{
		OutError = TEXT("render_target_alloc_failed");
		return false;
	}
	const FColor BackgroundColor(46, 50, 56, 255);
	Cleanup.RenderTarget->ClearColor = FLinearColor::FromSRGBColor(BackgroundColor);
	Cleanup.RenderTarget->InitCustomFormat(CaptureWidth, CaptureHeight, PF_B8G8R8A8, false);
	Cleanup.RenderTarget->UpdateResourceImmediate(true);

	const FBoxSphereBounds PreviewBounds = PreviewMeshComponent->CalcBounds(PreviewMeshComponent->GetComponentTransform());
	const FVector Origin = PreviewBounds.Origin;
	const float Radius = FMath::Max(PreviewBounds.SphereRadius, 80.0f);
	const float FovDegrees = 30.0f;
	const double AspectRatio = static_cast<double>(CaptureWidth) / static_cast<double>(CaptureHeight);
	const double HorizontalHalfFovRadians = FMath::DegreesToRadians(static_cast<double>(FovDegrees) * 0.5);
	const double VerticalHalfFovRadians = FMath::Atan(FMath::Tan(HorizontalHalfFovRadians) / FMath::Max(AspectRatio, KINDA_SMALL_NUMBER));
	const double FitHalfFovRadians = FMath::Min(HorizontalHalfFovRadians, VerticalHalfFovRadians);
	const double FitDistance = static_cast<double>(Radius) / FMath::Tan(FitHalfFovRadians);
	const FRotator CaptureRotation(-8.0f, -135.0f, 0.0f);
	const FVector CaptureDirection = CaptureRotation.Vector();
	const FVector CaptureLocation =
		Origin
		- CaptureDirection * static_cast<float>(FMath::Max(FitDistance * 1.15, 320.0))
		+ FVector(0.0f, 0.0f, Radius * 0.12f);

	FActorSpawnParameters LightSpawnParams;
	LightSpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
	LightSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	LightSpawnParams.bTemporaryEditorActor = true;
	LightSpawnParams.bHideFromSceneOutliner = true;

	LightSpawnParams.Name = MakeUniqueObjectName(World, ADirectionalLight::StaticClass(), TEXT("UeAgentAnimSequenceKeyLight"));
	Cleanup.KeyLightActor = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), CaptureLocation, (Origin - CaptureLocation).Rotation(), LightSpawnParams);
	if (Cleanup.KeyLightActor)
	{
		Cleanup.KeyLightActor->SetActorEnableCollision(false);
		if (UDirectionalLightComponent* KeyLight = Cleanup.KeyLightActor->GetComponent())
		{
			KeyLight->SetMobility(EComponentMobility::Movable);
			KeyLight->SetIntensity(10.0f);
			KeyLight->SetLightColor(FLinearColor::White);
			KeyLight->SetCastShadows(false);
		}
	}

	LightSpawnParams.Name = MakeUniqueObjectName(World, APointLight::StaticClass(), TEXT("UeAgentAnimSequenceFillLight"));
	const FVector FillLightLocation = Origin - CaptureDirection * FMath::Max(Radius * 1.6f, 220.0f) + FVector(0.0f, 0.0f, Radius * 0.65f);
	Cleanup.FillLightActor = World->SpawnActor<APointLight>(APointLight::StaticClass(), FillLightLocation, FRotator::ZeroRotator, LightSpawnParams);
	if (Cleanup.FillLightActor)
	{
		Cleanup.FillLightActor->SetActorEnableCollision(false);
		if (UPointLightComponent* FillLight = Cleanup.FillLightActor->FindComponentByClass<UPointLightComponent>())
		{
			FillLight->SetMobility(EComponentMobility::Movable);
			FillLight->SetIntensity(45000.0f);
			FillLight->SetAttenuationRadius(FMath::Max(Radius * 8.0f, 1200.0f));
			FillLight->SetLightColor(FLinearColor(1.0f, 0.96f, 0.9f));
			FillLight->SetCastShadows(false);
		}
	}

	if (UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")))
	{
		FActorSpawnParameters BackdropSpawnParams;
		BackdropSpawnParams.Name = MakeUniqueObjectName(World, AStaticMeshActor::StaticClass(), TEXT("UeAgentAnimSequenceBackdrop"));
		BackdropSpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
		BackdropSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		BackdropSpawnParams.bTemporaryEditorActor = true;
		BackdropSpawnParams.bHideFromSceneOutliner = true;

		const float BackdropSize = FMath::Max(Radius * 10.0f, 1800.0f);
		const FVector BackdropLocation = Origin + CaptureDirection * FMath::Max(Radius * 0.8f, 120.0f);
		const FRotator BackdropRotation = FRotationMatrix::MakeFromZ(-CaptureDirection).Rotator();
		const FVector BackdropScale(BackdropSize / 100.0f, BackdropSize / 100.0f, 1.0f);
		Cleanup.BackdropActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(BackdropRotation, BackdropLocation, BackdropScale), BackdropSpawnParams);
		if (Cleanup.BackdropActor)
		{
			Cleanup.BackdropActor->SetActorEnableCollision(false);
			UStaticMeshComponent* BackdropComponent = Cleanup.BackdropActor->GetStaticMeshComponent();
			if (BackdropComponent)
			{
				BackdropComponent->SetStaticMesh(PlaneMesh);
				BackdropComponent->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
				BackdropComponent->SetCastShadow(false);
				BackdropComponent->SetVisibility(true, true);
				BackdropComponent->UpdateComponentToWorld();
				BackdropComponent->MarkRenderStateDirty();
			}
		}
	}

	FActorSpawnParameters CaptureSpawnParams;
	CaptureSpawnParams.Name = MakeUniqueObjectName(World, ASceneCapture2D::StaticClass(), TEXT("UeAgentAnimSequenceCapture"));
	CaptureSpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
	CaptureSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	CaptureSpawnParams.bTemporaryEditorActor = true;
	CaptureSpawnParams.bHideFromSceneOutliner = true;

	Cleanup.CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), CaptureLocation, CaptureRotation, CaptureSpawnParams);
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
	CaptureComp->FOVAngle = FovDegrees;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureComp->ShowOnlyActorComponents(Cleanup.PreviewActor, true);
	if (Cleanup.BackdropActor)
	{
		CaptureComp->ShowOnlyActorComponents(Cleanup.BackdropActor, true);
	}
	CaptureComp->ShowFlags.SetAtmosphere(false);
	CaptureComp->ShowFlags.SetFog(false);
	CaptureComp->ShowFlags.SetMotionBlur(false);
	CaptureComp->ShowFlags.SetScreenSpaceReflections(false);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->CaptureScene();
	FlushRenderingCommands();

	FRenderTarget* RenderTarget = Cleanup.RenderTarget->GameThread_GetRenderTargetResource();
	if (!RenderTarget)
	{
		OutError = TEXT("render_target_resource_missing");
		return false;
	}

	TArray<FColor> Pixels;
	FIntPoint ShotSize(CaptureWidth, CaptureHeight);
	if (!RenderTarget->ReadPixels(Pixels))
	{
		OutError = TEXT("read_render_target_failed");
		FUeAgentInterfaceLogger::Log(TEXT("CmdAnimSequenceScreenshot failed asset=%s error=%s"), *Sequence->GetOutermost()->GetName(), *OutError);
		return false;
	}
	for (FColor& Pixel : Pixels)
	{
		if (Pixel.A < 255)
		{
			const float Alpha = static_cast<float>(Pixel.A) / 255.0f;
			Pixel.R = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Pixel.R) * Alpha + static_cast<float>(BackgroundColor.R) * (1.0f - Alpha)), 0, 255));
			Pixel.G = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Pixel.G) * Alpha + static_cast<float>(BackgroundColor.G) * (1.0f - Alpha)), 0, 255));
			Pixel.B = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Pixel.B) * Alpha + static_cast<float>(BackgroundColor.B) * (1.0f - Alpha)), 0, 255));
		}
		Pixel.A = 255;
	}
	const FString CaptureMode = TEXT("scene_capture_temp_actor");

	TArray<FColor> ResizedPixels;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
	{
		return false;
	}

	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
	{
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
	const int32 MaxFrameIndex = FMath::Max(0, Sequence->GetNumberOfSampledKeys() - 1);

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetStringField(TEXT("target"), Target);
	OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	OutData->SetNumberField(TEXT("frame_index"), FrameIndex);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("num_frames"), MaxFrameIndex);
	OutData->SetNumberField(TEXT("sampling_frame_rate_numerator"), SamplingFrameRate.Numerator);
	OutData->SetNumberField(TEXT("sampling_frame_rate_denominator"), SamplingFrameRate.Denominator);
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), EffectivePreviewMeshPath);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetSettings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	int32 SequenceNumFrames = 0;
	UAnimationBlueprintLibrary::GetNumFrames(Sequence, SequenceNumFrames);

	double RateScaleValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("rate_scale"), RateScaleValue))
	{
		UAnimationBlueprintLibrary::SetRateScale(Sequence, static_cast<float>(RateScaleValue));
	}

	bool bBoolValue = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("enable_root_motion"), bBoolValue))
	{
		UAnimationBlueprintLibrary::SetRootMotionEnabled(Sequence, bBoolValue);
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("force_root_lock"), bBoolValue))
	{
		UAnimationBlueprintLibrary::SetIsRootMotionLockForced(Sequence, bBoolValue);
	}

	FString AdditiveAnimationTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("additive_animation_type"), AdditiveAnimationTypeText) && !AdditiveAnimationTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAdditiveAnimationType AdditiveAnimationType = AAT_None;
		if (!UeAgentAnimationAssetOps::ParseAdditiveAnimationType(AdditiveAnimationTypeText, AdditiveAnimationType))
		{
			OutError = TEXT("invalid_additive_animation_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAdditiveAnimationType(Sequence, AdditiveAnimationType);
	}

	FString AdditiveBasePoseTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("additive_base_pose_type"), AdditiveBasePoseTypeText) && !AdditiveBasePoseTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAdditiveBasePoseType AdditiveBasePoseType = ABPT_None;
		if (!UeAgentAnimationAssetOps::ParseAdditiveBasePoseType(AdditiveBasePoseTypeText, AdditiveBasePoseType))
		{
			OutError = TEXT("invalid_additive_base_pose_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAdditiveBasePoseType(Sequence, AdditiveBasePoseType);
	}

	FString RootMotionLockTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("root_motion_lock_type"), RootMotionLockTypeText) && !RootMotionLockTypeText.TrimStartAndEnd().IsEmpty())
	{
		ERootMotionRootLock::Type RootMotionLockType = ERootMotionRootLock::RefPose;
		if (!UeAgentAnimationAssetOps::ParseRootMotionLockType(RootMotionLockTypeText, RootMotionLockType))
		{
			OutError = TEXT("invalid_root_motion_lock_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetRootMotionLockType(Sequence, RootMotionLockType);
	}

	FString InterpolationTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("interpolation_type"), InterpolationTypeText) && !InterpolationTypeText.TrimStartAndEnd().IsEmpty())
	{
		EAnimInterpolationType InterpolationType = EAnimInterpolationType::Linear;
		if (!UeAgentAnimationAssetOps::ParseInterpolationType(InterpolationTypeText, InterpolationType))
		{
			OutError = TEXT("invalid_interpolation_type");
			return false;
		}
		UAnimationBlueprintLibrary::SetAnimationInterpolationType(Sequence, InterpolationType);
	}

	FString BasePoseSequencePath;
	if (JsonTryGetString(Ctx.Params, TEXT("base_pose_sequence_path"), BasePoseSequencePath) && !BasePoseSequencePath.TrimStartAndEnd().IsEmpty())
	{
		UAnimSequence* BasePoseSequence = UeAgentAnimationAssetOps::LoadAnimSequence(BasePoseSequencePath);
		if (!BasePoseSequence)
		{
			OutError = TEXT("base_pose_sequence_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && BasePoseSequence->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(BasePoseSequence->GetSkeleton()))
		{
			OutError = TEXT("base_pose_sequence_skeleton_incompatible");
			return false;
		}
		Sequence->RefPoseSeq = BasePoseSequence;
		Sequence->MarkPackageDirty();
	}

	bool bClearBasePoseSequence = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("clear_base_pose_sequence"), bClearBasePoseSequence) && bClearBasePoseSequence)
	{
		Sequence->RefPoseSeq = nullptr;
		Sequence->MarkPackageDirty();
	}

	double BasePoseFrameIndexValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("base_pose_frame_index"), BasePoseFrameIndexValue))
	{
		const int32 BasePoseFrameIndex = static_cast<int32>(BasePoseFrameIndexValue);
		if (BasePoseFrameIndex < 0 || BasePoseFrameIndex >= SequenceNumFrames)
		{
			OutError = TEXT("invalid_base_pose_frame_index");
			return false;
		}
		Sequence->RefFrameIndex = BasePoseFrameIndex;
		Sequence->MarkPackageDirty();
	}

	if (Ctx.Params->HasField(TEXT("retarget_source")))
	{
		FString RetargetSourceText;
		JsonTryGetString(Ctx.Params, TEXT("retarget_source"), RetargetSourceText);
		Sequence->RetargetSource = RetargetSourceText.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*RetargetSourceText.TrimStartAndEnd());
		Sequence->MarkPackageDirty();
	}

	bool bClearRetargetSourceAsset = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("clear_retarget_source_asset"), bClearRetargetSourceAsset) && bClearRetargetSourceAsset)
	{
		Sequence->ClearRetargetSourceAsset();
		Sequence->MarkPackageDirty();
	}

	FString RetargetSourceAssetPath;
	if (JsonTryGetString(Ctx.Params, TEXT("retarget_source_asset_path"), RetargetSourceAssetPath) && !RetargetSourceAssetPath.TrimStartAndEnd().IsEmpty())
	{
		USkeletalMesh* RetargetSourceAsset = UeAgentAnimationAssetOps::LoadSkeletalMesh(RetargetSourceAssetPath);
		if (!RetargetSourceAsset)
		{
			OutError = TEXT("retarget_source_asset_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && RetargetSourceAsset->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(RetargetSourceAsset->GetSkeleton()))
		{
			OutError = TEXT("retarget_source_asset_skeleton_incompatible");
			return false;
		}
		Sequence->SetRetargetSourceAsset(RetargetSourceAsset);
		Sequence->MarkPackageDirty();
	}

	FString BoneCompressionSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("bone_compression_settings_path"), BoneCompressionSettingsPath) && !BoneCompressionSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UAnimBoneCompressionSettings* BoneCompressionSettings = UeAgentAnimationAssetOps::LoadBoneCompressionSettings(BoneCompressionSettingsPath);
		if (!BoneCompressionSettings)
		{
			OutError = TEXT("bone_compression_settings_not_found");
			return false;
		}
		if (!BoneCompressionSettings->AreSettingsValid())
		{
			OutError = TEXT("invalid_bone_compression_settings");
			return false;
		}
		UAnimationBlueprintLibrary::SetBoneCompressionSettings(Sequence, BoneCompressionSettings);
		Sequence->MarkPackageDirty();
	}

	FString CurveCompressionSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("curve_compression_settings_path"), CurveCompressionSettingsPath) && !CurveCompressionSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UAnimCurveCompressionSettings* CurveCompressionSettings = UeAgentAnimationAssetOps::LoadCurveCompressionSettings(CurveCompressionSettingsPath);
		if (!CurveCompressionSettings)
		{
			OutError = TEXT("curve_compression_settings_not_found");
			return false;
		}
		if (!CurveCompressionSettings->AreSettingsValid())
		{
			OutError = TEXT("invalid_curve_compression_settings");
			return false;
		}
		UAnimationBlueprintLibrary::SetCurveCompressionSettings(Sequence, CurveCompressionSettings);
		Sequence->MarkPackageDirty();
	}

	FString VariableFrameStrippingSettingsPath;
	if (JsonTryGetString(Ctx.Params, TEXT("variable_frame_stripping_settings_path"), VariableFrameStrippingSettingsPath) && !VariableFrameStrippingSettingsPath.TrimStartAndEnd().IsEmpty())
	{
		UVariableFrameStrippingSettings* VariableFrameStrippingSettings = UeAgentAnimationAssetOps::LoadVariableFrameStrippingSettings(VariableFrameStrippingSettingsPath);
		if (!VariableFrameStrippingSettings)
		{
			OutError = TEXT("variable_frame_stripping_settings_not_found");
			return false;
		}
		UAnimationBlueprintLibrary::SetVariableFrameStrippingSettings(Sequence, VariableFrameStrippingSettings);
		Sequence->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	float SequenceLength = 0.0f;
	float RateScale = 0.0f;
	TEnumAsByte<EAdditiveAnimationType> AdditiveAnimationType = AAT_None;
	TEnumAsByte<EAdditiveBasePoseType> AdditiveBasePoseType = ABPT_None;
	TEnumAsByte<ERootMotionRootLock::Type> RootMotionLockType = ERootMotionRootLock::RefPose;
	EAnimInterpolationType InterpolationType = EAnimInterpolationType::Linear;
	UAnimBoneCompressionSettings* BoneCompressionSettings = nullptr;
	UAnimCurveCompressionSettings* CurveCompressionSettings = nullptr;
	UVariableFrameStrippingSettings* VariableFrameStrippingSettings = nullptr;
	UAnimationBlueprintLibrary::GetSequenceLength(Sequence, SequenceLength);
	UAnimationBlueprintLibrary::GetRateScale(Sequence, RateScale);
	UAnimationBlueprintLibrary::GetAdditiveAnimationType(Sequence, AdditiveAnimationType);
	UAnimationBlueprintLibrary::GetAdditiveBasePoseType(Sequence, AdditiveBasePoseType);
	UAnimationBlueprintLibrary::GetRootMotionLockType(Sequence, RootMotionLockType);
	UAnimationBlueprintLibrary::GetAnimationInterpolationType(Sequence, InterpolationType);
	UAnimationBlueprintLibrary::GetBoneCompressionSettings(Sequence, BoneCompressionSettings);
	UAnimationBlueprintLibrary::GetCurveCompressionSettings(Sequence, CurveCompressionSettings);
	UAnimationBlueprintLibrary::GetVariableFrameStrippingSettings(Sequence, VariableFrameStrippingSettings);

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetNumberField(TEXT("sequence_length"), SequenceLength);
	OutData->SetNumberField(TEXT("rate_scale"), RateScale);
	OutData->SetStringField(TEXT("interpolation_type"), UeAgentAnimationAssetOps::InterpolationTypeToString(InterpolationType));
	OutData->SetStringField(TEXT("additive_animation_type"), UeAgentAnimationAssetOps::AdditiveAnimationTypeToString(AdditiveAnimationType));
	OutData->SetStringField(TEXT("additive_base_pose_type"), UeAgentAnimationAssetOps::AdditiveBasePoseTypeToString(AdditiveBasePoseType));
	OutData->SetStringField(TEXT("additive_base_pose_sequence"), Sequence->RefPoseSeq ? Sequence->RefPoseSeq->GetPathName() : TEXT(""));
	OutData->SetNumberField(TEXT("additive_base_pose_frame_index"), Sequence->RefFrameIndex);
	OutData->SetBoolField(TEXT("root_motion_enabled"), UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence));
	OutData->SetBoolField(TEXT("force_root_lock"), UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence));
	OutData->SetStringField(TEXT("root_motion_lock_type"), UeAgentAnimationAssetOps::RootMotionLockTypeToString(RootMotionLockType));
	OutData->SetStringField(TEXT("retarget_source"), Sequence->RetargetSource.ToString());
	OutData->SetStringField(TEXT("retarget_source_asset"), Sequence->GetRetargetSourceAsset().ToSoftObjectPath().ToString());
	OutData->SetStringField(TEXT("bone_compression_settings"), BoneCompressionSettings ? BoneCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("curve_compression_settings"), CurveCompressionSettings ? CurveCompressionSettings->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("variable_frame_stripping_settings"), VariableFrameStrippingSettings ? VariableFrameStrippingSettings->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);

	USkeletalMesh* PreviewMesh = nullptr;
	if (!bClearPreviewMesh)
	{
		if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		PreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && PreviewMesh->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_mesh_incompatible_with_sequence_skeleton");
			return false;
		}
	}

	Sequence->Modify();
	Sequence->SetPreviewMesh(PreviewMesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Sequence->GetPreviewMesh() ? Sequence->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetCurve(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString CurveNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("curve_name"), CurveNameText) || CurveNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_curve_name");
		return false;
	}
	const FName CurveName(*CurveNameText.TrimStartAndEnd());

	FString CurveTypeText = TEXT("float");
	JsonTryGetString(Ctx.Params, TEXT("curve_type"), CurveTypeText);
	ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
	if (!UeAgentAnimationAssetOps::ParseCurveType(CurveTypeText, CurveType))
	{
		OutError = TEXT("invalid_curve_type");
		return false;
	}

	UAnimSequence* Sequence = UeAgentAnimationAssetOps::LoadAnimSequence(AssetPath);
	if (!Sequence)
	{
		OutError = TEXT("anim_sequence_not_found");
		return false;
	}

	bool bRemove = false;
	bool bMetaDataCurve = false;
	bool bClearExistingKeys = false;
	bool bRemoveNameFromSkeleton = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);
	JsonTryGetBool(Ctx.Params, TEXT("meta_data_curve"), bMetaDataCurve);
	JsonTryGetBool(Ctx.Params, TEXT("clear_existing_keys"), bClearExistingKeys);
	JsonTryGetBool(Ctx.Params, TEXT("remove_name_from_skeleton"), bRemoveNameFromSkeleton);

	Sequence->Modify();

	if (bRemove)
	{
		UAnimationBlueprintLibrary::RemoveCurve(Sequence, CurveName, bRemoveNameFromSkeleton);
	}
	else
	{
		if (bClearExistingKeys && UAnimationBlueprintLibrary::DoesCurveExist(Sequence, CurveName, CurveType))
		{
			UAnimationBlueprintLibrary::RemoveCurve(Sequence, CurveName, false);
		}

		if (!UAnimationBlueprintLibrary::DoesCurveExist(Sequence, CurveName, CurveType))
		{
			UAnimationBlueprintLibrary::AddCurve(Sequence, CurveName, CurveType, bMetaDataCurve);
		}

		auto AddSingleCurveKey = [&](const TSharedPtr<FJsonObject>& CurveObject) -> bool
		{
			if (!CurveObject.IsValid())
			{
				return true;
			}

			double TimeSeconds = 0.0;
			if (!CurveObject->TryGetNumberField(TEXT("time_seconds"), TimeSeconds) || TimeSeconds < 0.0 || TimeSeconds > Sequence->GetPlayLength() + KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("invalid_curve_time_seconds");
				return false;
			}

			switch (CurveType)
			{
			case ERawCurveTrackTypes::RCT_Float:
			default:
			{
				double FloatValue = 0.0;
				if (!CurveObject->TryGetNumberField(TEXT("value"), FloatValue))
				{
					OutError = TEXT("missing_float_curve_value");
					return false;
				}
				UAnimationBlueprintLibrary::AddFloatCurveKey(Sequence, CurveName, static_cast<float>(TimeSeconds), static_cast<float>(FloatValue));
				return true;
			}
			}
		};

		if (Ctx.Params->HasTypedField<EJson::Array>(TEXT("keys")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
			if (Ctx.Params->TryGetArrayField(TEXT("keys"), Keys) && Keys)
			{
				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObject = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject || !KeyObject->IsValid())
					{
						continue;
					}
					if (!AddSingleCurveKey(*KeyObject))
					{
						return false;
					}
				}
			}
		}
		else if (Ctx.Params->HasField(TEXT("time_seconds")))
		{
			if (!AddSingleCurveKey(Ctx.Params))
			{
				return false;
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

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("curve_name"), CurveName.ToString());
	OutData->SetStringField(TEXT("curve_type"), UeAgentAnimationAssetOps::CurveTypeToString(CurveType));
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("curve_exists"), !bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	if (!bRemove)
	{
		OutData->SetObjectField(TEXT("curve"), UeAgentAnimationAssetOps::BuildCurveJson(Sequence, CurveName, CurveType, true));
	}
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSetBones(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	bool bRemoveAllBoneAnimation = false;
	bool bIncludeChildren = false;
	bool bExcludeChildrenRecursively = false;
	bool bFinalizeAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("remove_all_bone_animation"), bRemoveAllBoneAnimation);
	JsonTryGetBool(Ctx.Params, TEXT("include_children"), bIncludeChildren);
	JsonTryGetBool(Ctx.Params, TEXT("exclude_children_recursively"), bExcludeChildrenRecursively);
	JsonTryGetBool(Ctx.Params, TEXT("finalize_after_set"), bFinalizeAfterSet);

	Sequence->Modify();

	int32 RemovedBoneRequestCount = 0;
	int32 RemovedVirtualBoneRequestCount = 0;

	if (bRemoveAllBoneAnimation)
	{
		UAnimationBlueprintLibrary::RemoveAllBoneAnimation(Sequence);
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* RemoveBoneNames = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ChildrenExcluded = nullptr;
		Ctx.Params->TryGetArrayField(TEXT("remove_bone_names"), RemoveBoneNames);
		Ctx.Params->TryGetArrayField(TEXT("children_excluded"), ChildrenExcluded);

		TArray<FName> ExcludedChildrenNames;
		if (ChildrenExcluded)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ChildrenExcluded)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString BoneNameText = Value->AsString().TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					ExcludedChildrenNames.Add(FName(*BoneNameText));
				}
			}
		}

		if (RemoveBoneNames)
		{
			TArray<FName> BoneNames;
			for (const TSharedPtr<FJsonValue>& Value : *RemoveBoneNames)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString BoneNameText = Value->AsString().TrimStartAndEnd();
				if (!BoneNameText.IsEmpty())
				{
					BoneNames.Add(FName(*BoneNameText));
				}
			}

			RemovedBoneRequestCount = BoneNames.Num();
			for (int32 Index = 0; Index < BoneNames.Num(); ++Index)
			{
				const bool bFinalizeThisRemoval = bFinalizeAfterSet && (Index == BoneNames.Num() - 1);
				if (ExcludedChildrenNames.Num() > 0)
				{
					UAnimationBlueprintLibrary::RemoveBoneSelectiveAnimation(Sequence, BoneNames[Index], ExcludedChildrenNames, bIncludeChildren, bExcludeChildrenRecursively, bFinalizeThisRemoval);
				}
				else
				{
					UAnimationBlueprintLibrary::RemoveBoneAnimation(Sequence, BoneNames[Index], bIncludeChildren, bFinalizeThisRemoval);
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveVirtualBoneNames = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_virtual_bone_names"), RemoveVirtualBoneNames) && RemoveVirtualBoneNames)
	{
		TArray<FName> VirtualBoneNames;
		for (const TSharedPtr<FJsonValue>& Value : *RemoveVirtualBoneNames)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString BoneNameText = Value->AsString().TrimStartAndEnd();
			if (!BoneNameText.IsEmpty())
			{
				VirtualBoneNames.Add(FName(*BoneNameText));
			}
		}

		if (VirtualBoneNames.Num() > 0)
		{
			UAnimationBlueprintLibrary::RemoveVirtualBones(Sequence, VirtualBoneNames);
			RemovedVirtualBoneRequestCount = VirtualBoneNames.Num();
		}
	}

	Sequence->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	TArray<FName> TrackNames;
	UAnimationBlueprintLibrary::GetAnimationTrackNames(Sequence, TrackNames);
	TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
	for (const FName TrackName : TrackNames)
	{
		TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackName.ToString()));
	}

	OutData->SetStringField(TEXT("asset_path"), Sequence->GetPathName());
	OutData->SetBoolField(TEXT("removed_all_bone_animation"), bRemoveAllBoneAnimation);
	OutData->SetNumberField(TEXT("removed_bone_request_count"), RemovedBoneRequestCount);
	OutData->SetNumberField(TEXT("removed_virtual_bone_request_count"), RemovedVirtualBoneRequestCount);
	OutData->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
	OutData->SetArrayField(TEXT("track_names"), TrackNamesJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

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

bool FUeAgentHttpServer::CmdSkeletonGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<TSharedPtr<FJsonValue>> SlotGroups;
	for (const FAnimSlotGroup& SlotGroup : Skeleton->GetSlotGroups())
	{
		TSharedPtr<FJsonObject> GroupObject = MakeShared<FJsonObject>();
		GroupObject->SetStringField(TEXT("group_name"), SlotGroup.GroupName.ToString());

		TArray<TSharedPtr<FJsonValue>> SlotNames;
		for (const FName SlotName : SlotGroup.SlotNames)
		{
			SlotNames.Add(MakeShared<FJsonValueString>(SlotName.ToString()));
		}
		GroupObject->SetArrayField(TEXT("slot_names"), SlotNames);
		SlotGroups.Add(MakeShared<FJsonValueObject>(GroupObject));
	}

	TArray<TSharedPtr<FJsonValue>> Sockets;
	for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (Socket)
		{
			Sockets.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSocketJson(Socket)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> VirtualBones;
	for (const FVirtualBone& VirtualBone : Skeleton->GetVirtualBones())
	{
		VirtualBones.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildVirtualBoneJson(VirtualBone)));
	}

	TArray<TSharedPtr<FJsonValue>> CompatibleSkeletons;
	for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : Skeleton->GetCompatibleSkeletons())
	{
		CompatibleSkeletons.Add(MakeShared<FJsonValueString>(CompatibleSkeleton.ToSoftObjectPath().ToString()));
	}

#if WITH_EDITOR
	TArray<FName> AnimationNotifies;
	Skeleton->CollectAnimationNotifies(AnimationNotifies);
	TArray<TSharedPtr<FJsonValue>> AnimationNotifyNames;
	for (const FName NotifyName : AnimationNotifies)
	{
		AnimationNotifyNames.Add(MakeShared<FJsonValueString>(NotifyName.ToString()));
	}
#endif

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Skeleton->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Skeleton->GetPreviewMesh() ? Skeleton->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("root_bone_name"), ReferenceSkeleton.GetNum() > 0 ? ReferenceSkeleton.GetBoneName(0).ToString() : TEXT(""));
	OutData->SetNumberField(TEXT("bone_count"), ReferenceSkeleton.GetNum());
	OutData->SetNumberField(TEXT("socket_count"), Sockets.Num());
	OutData->SetNumberField(TEXT("virtual_bone_count"), VirtualBones.Num());
	OutData->SetNumberField(TEXT("slot_group_count"), SlotGroups.Num());
	OutData->SetNumberField(TEXT("compatible_skeleton_count"), CompatibleSkeletons.Num());
	OutData->SetArrayField(TEXT("slot_groups"), SlotGroups);
	OutData->SetArrayField(TEXT("sockets"), Sockets);
	OutData->SetArrayField(TEXT("virtual_bones"), VirtualBones);
	OutData->SetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons);
	OutData->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
#if WITH_EDITOR
	OutData->SetNumberField(TEXT("animation_notify_name_count"), AnimationNotifyNames.Num());
	OutData->SetArrayField(TEXT("animation_notify_names"), AnimationNotifyNames);
#endif
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonListBones(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
	TArray<TSharedPtr<FJsonValue>> Bones;
	Bones.Reserve(ReferenceSkeleton.GetNum());

	for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
	{
		const int32 ParentIndex = ReferenceSkeleton.GetParentIndex(BoneIndex);
		TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
		BoneObject->SetNumberField(TEXT("bone_index"), BoneIndex);
		BoneObject->SetStringField(TEXT("bone_name"), ReferenceSkeleton.GetBoneName(BoneIndex).ToString());
		BoneObject->SetNumberField(TEXT("parent_index"), ParentIndex);
		BoneObject->SetStringField(TEXT("parent_bone_name"), ParentIndex != INDEX_NONE ? ReferenceSkeleton.GetBoneName(ParentIndex).ToString() : TEXT(""));
		Bones.Add(MakeShared<FJsonValueObject>(BoneObject));
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetNumberField(TEXT("bone_count"), Bones.Num());
	OutData->SetArrayField(TEXT("bones"), Bones);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetCompatibleSkeletons(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	auto ApplySkeletonList = [&](const TArray<TSharedPtr<FJsonValue>>* Paths, const bool bAdd) -> bool
	{
		if (!Paths)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Paths)
		{
			if (!Value.IsValid())
			{
				continue;
			}

			const FString CompatibleSkeletonPath = Value->AsString().TrimStartAndEnd();
			if (CompatibleSkeletonPath.IsEmpty())
			{
				continue;
			}

			USkeleton* CompatibleSkeleton = UeAgentAnimationAssetOps::LoadSkeleton(CompatibleSkeletonPath);
			if (!CompatibleSkeleton)
			{
				OutError = TEXT("compatible_skeleton_not_found");
				return false;
			}
			if (CompatibleSkeleton == Skeleton)
			{
				OutError = TEXT("compatible_skeleton_same_as_asset");
				return false;
			}

			if (bAdd)
			{
				Skeleton->AddCompatibleSkeleton(CompatibleSkeleton);
			}
			else
			{
				Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
			}
		}
		return true;
	};

	Skeleton->Modify();

	bool bClearAll = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_all"), bClearAll);
	if (bClearAll)
	{
		TArray<TSoftObjectPtr<USkeleton>> ExistingCompatibleSkeletons = Skeleton->GetCompatibleSkeletons();
		for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : ExistingCompatibleSkeletons)
		{
			Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SetCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("set_compatible_skeleton_paths"), SetCompatibleSkeletonPaths) && SetCompatibleSkeletonPaths)
	{
		TArray<TSoftObjectPtr<USkeleton>> ExistingCompatibleSkeletons = Skeleton->GetCompatibleSkeletons();
		for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : ExistingCompatibleSkeletons)
		{
			Skeleton->RemoveCompatibleSkeleton(CompatibleSkeleton);
		}
		if (!ApplySkeletonList(SetCompatibleSkeletonPaths, true))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* AddCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("add_compatible_skeleton_paths"), AddCompatibleSkeletonPaths) && AddCompatibleSkeletonPaths)
	{
		if (!ApplySkeletonList(AddCompatibleSkeletonPaths, true))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemoveCompatibleSkeletonPaths = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("remove_compatible_skeleton_paths"), RemoveCompatibleSkeletonPaths) && RemoveCompatibleSkeletonPaths)
	{
		if (!ApplySkeletonList(RemoveCompatibleSkeletonPaths, false))
		{
			return false;
		}
	}

	if (Ctx.Params->HasField(TEXT("use_retarget_modes_from_compatible_skeleton")))
	{
		bool bUseRetargetModesFromCompatibleSkeleton = false;
		JsonTryGetBool(Ctx.Params, TEXT("use_retarget_modes_from_compatible_skeleton"), bUseRetargetModesFromCompatibleSkeleton);
		Skeleton->SetUseRetargetModesFromCompatibleSkeleton(bUseRetargetModesFromCompatibleSkeleton);
	}

	Skeleton->MarkPackageDirty();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CompatibleSkeletons;
	for (const TSoftObjectPtr<USkeleton>& CompatibleSkeleton : Skeleton->GetCompatibleSkeletons())
	{
		CompatibleSkeletons.Add(MakeShared<FJsonValueString>(CompatibleSkeleton.ToSoftObjectPath().ToString()));
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetNumberField(TEXT("compatible_skeleton_count"), CompatibleSkeletons.Num());
	OutData->SetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons);
	OutData->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"), Skeleton->GetUseRetargetModesFromCompatibleSkeleton());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetPreviewMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	bool bClearPreviewMesh = false;
	JsonTryGetBool(Ctx.Params, TEXT("clear_preview_mesh"), bClearPreviewMesh);

	FString PreviewMeshPath;
	JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), PreviewMeshPath);

	USkeletalMesh* PreviewMesh = nullptr;
	if (!bClearPreviewMesh)
	{
		if (PreviewMeshPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_skeletal_mesh_path");
			return false;
		}

		PreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("skeletal_mesh_not_found");
			return false;
		}
	}

	Skeleton->Modify();
	Skeleton->SetPreviewMesh(PreviewMesh, true);

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Skeleton->GetPreviewMesh() ? Skeleton->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetSocket(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	FString SocketNameText;
	if (!JsonTryGetString(Ctx.Params, TEXT("socket_name"), SocketNameText) || SocketNameText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_socket_name");
		return false;
	}
	const FName SocketName(*SocketNameText.TrimStartAndEnd());

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	USkeletalMeshSocket* Socket = UeAgentAnimationAssetOps::FindSocketByName(Skeleton, SocketName);
	Skeleton->Modify();

	if (bRemove)
	{
		if (!Socket)
		{
			OutError = TEXT("socket_not_found");
			return false;
		}
		Skeleton->Sockets.Remove(Socket);
		Skeleton->MarkPackageDirty();
	}
	else
	{
		FString BoneNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("bone_name"), BoneNameText) && !Socket)
		{
			OutError = TEXT("missing_bone_name");
			return false;
		}

		if (!Socket)
		{
			Socket = NewObject<USkeletalMeshSocket>(Skeleton, NAME_None, RF_Transactional);
			if (!Socket)
			{
				OutError = TEXT("create_socket_failed");
				return false;
			}
			Skeleton->Sockets.Add(Socket);
		}

		if (!BoneNameText.TrimStartAndEnd().IsEmpty())
		{
			Socket->BoneName = FName(*BoneNameText.TrimStartAndEnd());
		}
		Socket->SocketName = SocketName;

		FVector RelativeLocation = Socket->RelativeLocation;
		FRotator RelativeRotation = Socket->RelativeRotation;
		FVector RelativeScale = Socket->RelativeScale;
		JsonTryGetVector(Ctx.Params, TEXT("relative_location"), RelativeLocation);
		JsonTryGetRotator(Ctx.Params, TEXT("relative_rotation"), RelativeRotation);
		JsonTryGetVector(Ctx.Params, TEXT("relative_scale"), RelativeScale);
		Socket->RelativeLocation = RelativeLocation;
		Socket->RelativeRotation = RelativeRotation;
		Socket->RelativeScale = RelativeScale;
		Skeleton->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("socket_name"), SocketName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	if (!bRemove && Socket)
	{
		OutData->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());
		OutData->SetObjectField(TEXT("relative_location"), UeAgentAnimationAssetOps::BuildVectorJson(Socket->RelativeLocation));
		OutData->SetObjectField(TEXT("relative_rotation"), UeAgentAnimationAssetOps::BuildRotatorJson(Socket->RelativeRotation));
		OutData->SetObjectField(TEXT("relative_scale"), UeAgentAnimationAssetOps::BuildVectorJson(Socket->RelativeScale));
	}
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdSkeletonSetVirtualBone(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(AssetPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	bool bRemove = false;
	JsonTryGetBool(Ctx.Params, TEXT("remove"), bRemove);

	Skeleton->Modify();
	FName VirtualBoneName = NAME_None;

	if (bRemove)
	{
		FString VirtualBoneNameText;
		if (!JsonTryGetString(Ctx.Params, TEXT("virtual_bone_name"), VirtualBoneNameText) || VirtualBoneNameText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_virtual_bone_name");
			return false;
		}

		VirtualBoneName = FName(*VirtualBoneNameText.TrimStartAndEnd());
		if (!UeAgentAnimationAssetOps::FindVirtualBoneByName(Skeleton, VirtualBoneName))
		{
			OutError = TEXT("virtual_bone_not_found");
			return false;
		}

		TArray<FName> BonesToRemove;
		BonesToRemove.Add(VirtualBoneName);
		Skeleton->RemoveVirtualBones(BonesToRemove);
		Skeleton->MarkPackageDirty();
	}
	else
	{
		FString SourceBoneNameText;
		FString TargetBoneNameText;
		JsonTryGetString(Ctx.Params, TEXT("source_bone_name"), SourceBoneNameText);
		JsonTryGetString(Ctx.Params, TEXT("target_bone_name"), TargetBoneNameText);
		SourceBoneNameText = SourceBoneNameText.TrimStartAndEnd();
		TargetBoneNameText = TargetBoneNameText.TrimStartAndEnd();
		if (SourceBoneNameText.IsEmpty() || TargetBoneNameText.IsEmpty())
		{
			OutError = TEXT("missing_source_or_target_bone_name");
			return false;
		}

		FString VirtualBoneNameText;
		JsonTryGetString(Ctx.Params, TEXT("virtual_bone_name"), VirtualBoneNameText);
		VirtualBoneNameText = VirtualBoneNameText.TrimStartAndEnd();

		if (VirtualBoneNameText.IsEmpty())
		{
			if (!Skeleton->AddNewVirtualBone(FName(*SourceBoneNameText), FName(*TargetBoneNameText), VirtualBoneName))
			{
				OutError = TEXT("add_virtual_bone_failed");
				return false;
			}
		}
		else
		{
			VirtualBoneName = FName(*VirtualBoneNameText);
			if (!Skeleton->AddNewNamedVirtualBone(FName(*SourceBoneNameText), FName(*TargetBoneNameText), VirtualBoneName))
			{
				OutError = TEXT("add_virtual_bone_failed");
				return false;
			}
		}
		Skeleton->MarkPackageDirty();
	}

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentAnimationAssetOps::SaveAssetPackage(Skeleton, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
	OutData->SetStringField(TEXT("virtual_bone_name"), VirtualBoneName.ToString());
	OutData->SetBoolField(TEXT("removed"), bRemove);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}
