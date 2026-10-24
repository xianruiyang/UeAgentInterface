// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"
#include "Domains/Animation/UeAgentAnimationEditorDomainBackend.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureSequenceFrameOptions.h"
#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"

#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

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
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Animation/Skeleton.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "AnimationBlueprintLibrary.h"
#include "AnimPose.h"
#include "AnimPreviewInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Dom/JsonValue.h"
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
#include "TextureResource.h"
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
#include "Subsystems/AssetEditorSubsystem.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UeAgentAnimationAssetOps
{
	FString NormalizeAssetPath(const FString& InPath)
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

	UObject* LoadAssetObject(const FString& InPath)
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

	UAnimSequence* LoadAnimSequence(const FString& InPath)
	{
		return Cast<UAnimSequence>(LoadAssetObject(InPath));
	}


	static UAnimationAsset* LoadAnimationAsset(const FString& InPath)
	{
		return Cast<UAnimationAsset>(LoadAssetObject(InPath));
	}

	UBlendSpace* LoadBlendSpace(const FString& InPath)
	{
		return Cast<UBlendSpace>(LoadAssetObject(InPath));
	}

	USkeleton* LoadSkeleton(const FString& InPath)
	{
		return Cast<USkeleton>(LoadAssetObject(InPath));
	}

	USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	bool SaveAuthoringFile(const FString& FilePathInput, const TSharedPtr<FJsonObject>& Obj, FString& OutResolvedPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("animation_asset_json_export");
		Options.bExternalizeInlineDetails = false;
		return UeAgentAuthoringFileIO::SaveObject(FilePathInput, Obj, Options, OutResolvedPath, OutError);
	}

	UClass* ResolveBlendSpaceClass(const FString& Kind)
	{
		if (Kind.Equals(TEXT("blendspace_1d"), ESearchCase::IgnoreCase))
		{
			return UBlendSpace1D::StaticClass();
		}
		if (Kind.Equals(TEXT("aim_offset"), ESearchCase::IgnoreCase))
		{
			return UAimOffsetBlendSpace::StaticClass();
		}
		if (Kind.Equals(TEXT("aim_offset_1d"), ESearchCase::IgnoreCase))
		{
			return UAimOffsetBlendSpace1D::StaticClass();
		}
		return UBlendSpace::StaticClass();
	}

	FString GetBlendSpaceKind(const UBlendSpace* BlendSpace)
	{
		if (!BlendSpace)
		{
			return TEXT("");
		}
		if (BlendSpace->IsA<UAimOffsetBlendSpace1D>())
		{
			return TEXT("aim_offset_1d");
		}
		if (BlendSpace->IsA<UAimOffsetBlendSpace>())
		{
			return TEXT("aim_offset");
		}
		if (BlendSpace->IsA<UBlendSpace1D>())
		{
			return TEXT("blendspace_1d");
		}
		return TEXT("blendspace_2d");
	}

	static TSharedPtr<FJsonObject> BlendParameterToJson(const FBlendParameter& Parameter, const int32 AxisIndex)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("axis_index"), AxisIndex);
		Obj->SetStringField(TEXT("name"), Parameter.DisplayName);
		Obj->SetNumberField(TEXT("min"), Parameter.Min);
		Obj->SetNumberField(TEXT("max"), Parameter.Max);
		Obj->SetNumberField(TEXT("grid_num"), Parameter.GridNum);
		Obj->SetBoolField(TEXT("snap_to_grid"), Parameter.bSnapToGrid);
		Obj->SetBoolField(TEXT("wrap_input"), Parameter.bWrapInput);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildVectorJsonForBlendSpace(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	bool ApplyBlendParameterFromJson(UBlendSpace* BlendSpace, const int32 AxisIndex, const TSharedPtr<FJsonObject>& AxisObj, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		if (!BlendSpace || !AxisObj.IsValid() || AxisIndex < 0 || AxisIndex > 2)
		{
			return false;
		}
		FStructProperty* BlendParametersProperty = FindFProperty<FStructProperty>(BlendSpace->GetClass(), TEXT("BlendParameters"));
		if (!BlendParametersProperty || BlendParametersProperty->Struct != FBlendParameter::StaticStruct() || BlendParametersProperty->ArrayDim < AxisIndex + 1)
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("blendspace_axis_property_not_found"), TEXT("axes"), TEXT("BlendParameters property is not accessible."));
			return false;
		}
		FBlendParameter* Parameter = BlendParametersProperty->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, AxisIndex);
		if (!Parameter)
		{
			return false;
		}
		FString Name;
		if (AxisObj->TryGetStringField(TEXT("name"), Name))
		{
			Parameter->DisplayName = Name;
		}
		double NumberValue = 0.0;
		if (AxisObj->TryGetNumberField(TEXT("min"), NumberValue))
		{
			Parameter->Min = static_cast<float>(NumberValue);
		}
		if (AxisObj->TryGetNumberField(TEXT("max"), NumberValue))
		{
			Parameter->Max = static_cast<float>(NumberValue);
		}
		if (AxisObj->TryGetNumberField(TEXT("grid_num"), NumberValue))
		{
			Parameter->GridNum = FMath::Max(1, static_cast<int32>(NumberValue));
		}
		bool bBoolValue = false;
		if (AxisObj->TryGetBoolField(TEXT("snap_to_grid"), bBoolValue))
		{
			Parameter->bSnapToGrid = bBoolValue;
		}
		if (AxisObj->TryGetBoolField(TEXT("wrap_input"), bBoolValue))
		{
			Parameter->bWrapInput = bBoolValue;
		}
		return true;
	}

	TArray<FBlendSample>* GetMutableBlendSamples(UBlendSpace* BlendSpace)
	{
		FArrayProperty* SampleDataProperty = BlendSpace ? FindFProperty<FArrayProperty>(BlendSpace->GetClass(), TEXT("SampleData")) : nullptr;
		return SampleDataProperty ? SampleDataProperty->ContainerPtrToValuePtr<TArray<FBlendSample>>(BlendSpace) : nullptr;
	}

	TSharedPtr<FJsonObject> BuildBlendSpaceJson(UBlendSpace* BlendSpace)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.blendspace.v1"));
		Obj->SetStringField(TEXT("asset_path"), BlendSpace ? NormalizeAssetPath(BlendSpace->GetOutermost()->GetName()) : TEXT(""));
		Obj->SetStringField(TEXT("object_path"), BlendSpace ? BlendSpace->GetPathName() : TEXT(""));
		Obj->SetStringField(TEXT("blendspace_kind"), GetBlendSpaceKind(BlendSpace));
		Obj->SetStringField(TEXT("skeleton"), BlendSpace && BlendSpace->GetSkeleton() ? BlendSpace->GetSkeleton()->GetPathName() : TEXT(""));
		TArray<TSharedPtr<FJsonValue>> Axes;
		if (BlendSpace)
		{
			const int32 AxisCount = BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2;
			for (int32 AxisIndex = 0; AxisIndex < AxisCount; ++AxisIndex)
			{
				Axes.Add(MakeShared<FJsonValueObject>(BlendParameterToJson(BlendSpace->GetBlendParameter(AxisIndex), AxisIndex)));
			}
		}
		Obj->SetArrayField(TEXT("axes"), Axes);

		TArray<TSharedPtr<FJsonValue>> Samples;
		if (BlendSpace)
		{
			const TArray<FBlendSample>& BlendSamples = BlendSpace->GetBlendSamples();
			for (int32 SampleIndex = 0; SampleIndex < BlendSamples.Num(); ++SampleIndex)
			{
				const FBlendSample& Sample = BlendSamples[SampleIndex];
				TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
				SampleObj->SetNumberField(TEXT("sample_index"), SampleIndex);
				SampleObj->SetStringField(TEXT("animation"), Sample.Animation ? Sample.Animation->GetPathName() : TEXT(""));
				SampleObj->SetObjectField(TEXT("position"), BuildVectorJsonForBlendSpace(Sample.SampleValue));
				SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
				Samples.Add(MakeShared<FJsonValueObject>(SampleObj));
			}
		}
		Obj->SetArrayField(TEXT("samples"), Samples);
		return Obj;
	}

	UAnimBoneCompressionSettings* LoadBoneCompressionSettings(const FString& InPath)
	{
		return Cast<UAnimBoneCompressionSettings>(LoadAssetObject(InPath));
	}

	UAnimCurveCompressionSettings* LoadCurveCompressionSettings(const FString& InPath)
	{
		return Cast<UAnimCurveCompressionSettings>(LoadAssetObject(InPath));
	}

	UVariableFrameStrippingSettings* LoadVariableFrameStrippingSettings(const FString& InPath)
	{
		return Cast<UVariableFrameStrippingSettings>(LoadAssetObject(InPath));
	}

	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Asset)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_not_found"),
				TEXT("There is no animation asset object to save."), TEXT("animation_save"));
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("animation asset object"));
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Asset->GetOutermost()->GetName();
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError,
				TEXT("animation_save"), OutError.PublicMessage.IsEmpty()
					? TEXT("The editor did not save the animation asset package.") : FString());
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildQuatJson(const FQuat& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		Obj->SetNumberField(TEXT("w"), Value.W);
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

	bool TryParseLinearColorField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FLinearColor& OutColor)
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

	const TCHAR* InterpolationTypeToString(const EAnimInterpolationType Type)
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

	bool ParseInterpolationType(const FString& InValue, EAnimInterpolationType& OutType)
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

	const TCHAR* CurveTypeToString(const ERawCurveTrackTypes Type)
	{
		switch (Type)
		{
		case ERawCurveTrackTypes::RCT_Float:
		default:
			return TEXT("float");
		}
	}

	bool ParseCurveType(const FString& InValue, ERawCurveTrackTypes& OutType)
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

	const TCHAR* AdditiveAnimationTypeToString(const EAdditiveAnimationType Type)
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

	const TCHAR* AdditiveBasePoseTypeToString(const EAdditiveBasePoseType Type)
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

	bool ParseAdditiveAnimationType(const FString& InValue, EAdditiveAnimationType& OutType)
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

	bool ParseAdditiveBasePoseType(const FString& InValue, EAdditiveBasePoseType& OutType)
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

	const TCHAR* RootMotionLockTypeToString(const ERootMotionRootLock::Type Type)
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

	bool ParseRootMotionLockType(const FString& InValue, ERootMotionRootLock::Type& OutType)
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

	TSharedPtr<FJsonObject> BuildNotifyTrackJson(UAnimSequenceBase* Sequence, const FAnimNotifyTrack& Track, const int32 TrackIndex)
	{
		TArray<FAnimNotifyEvent> NotifyEvents;
		UAnimationBlueprintLibrary::GetAnimationNotifyEventsForTrack(Sequence, Track.TrackName, NotifyEvents);

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("track_index"), TrackIndex);
		Item->SetStringField(TEXT("track_name"), Track.TrackName.ToString());
		Item->SetStringField(TEXT("track_color"), Track.TrackColor.ToString());
		Item->SetObjectField(TEXT("track_color_linear"), BuildLinearColorJson(Track.TrackColor));
		Item->SetNumberField(TEXT("notify_count"), NotifyEvents.Num());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("track_name"), Track.TrackName.ToString(), Track.TrackName.ToString(), TEXT("Animation/Notify Tracks"), TEXT("track"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildNotifyJson(const UAnimSequenceBase* Sequence, const FAnimNotifyEvent& Notify, const int32 NotifyIndex)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("notify_index"), FString::FromInt(NotifyIndex), Notify.NotifyName.ToString(), TEXT("Animation/Notifies"), TEXT("timeline_event"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildSyncMarkerJson(const UAnimSequence* Sequence, const FAnimSyncMarker& Marker, const int32 MarkerIndex)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("marker_index"), FString::FromInt(MarkerIndex), Marker.MarkerName.ToString(), TEXT("Animation/Sync Markers"), TEXT("timeline_marker"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), BuildVectorJson(Value.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), BuildRotatorJson(Value.Rotator()));
		Obj->SetObjectField(TEXT("scale"), BuildVectorJson(Value.GetScale3D()));
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildTransformJsonWithQuaternion(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = BuildTransformJson(Value);
		Obj->SetObjectField(TEXT("quaternion"), BuildQuatJson(Value.GetRotation().GetNormalized()));
		return Obj;
	}

	bool ParseAnimDataEvalType(const FString& InText, EAnimDataEvalType& OutType)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("raw"), ESearchCase::IgnoreCase))
		{
			OutType = EAnimDataEvalType::Raw;
			return true;
		}
		if (Text.Equals(TEXT("source"), ESearchCase::IgnoreCase))
		{
			OutType = EAnimDataEvalType::Source;
			return true;
		}
		if (Text.Equals(TEXT("compressed"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("runtime"), ESearchCase::IgnoreCase))
		{
			OutType = EAnimDataEvalType::Compressed;
			return true;
		}
		return false;
	}

	FString AnimDataEvalTypeToString(const EAnimDataEvalType Type)
	{
		switch (Type)
		{
		case EAnimDataEvalType::Source:
			return TEXT("source");
		case EAnimDataEvalType::Compressed:
			return TEXT("compressed");
		case EAnimDataEvalType::Raw:
		default:
			return TEXT("raw");
		}
	}

	bool ParseAnimPoseSpace(const FString& InText, EAnimPoseSpaces& OutSpace)
	{
		const FString Text = InText.TrimStartAndEnd();
		if (Text.IsEmpty() || Text.Equals(TEXT("local"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("bone"), ESearchCase::IgnoreCase))
		{
			OutSpace = EAnimPoseSpaces::Local;
			return true;
		}
		if (Text.Equals(TEXT("world"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("component"), ESearchCase::IgnoreCase))
		{
			OutSpace = EAnimPoseSpaces::World;
			return true;
		}
		return false;
	}

	FString AnimPoseSpaceToString(const EAnimPoseSpaces Space)
	{
		return Space == EAnimPoseSpaces::World ? TEXT("world") : TEXT("local");
	}

	TSharedPtr<FJsonObject> BuildSimpleKeyJson(const double TimeSeconds, const double Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("time_seconds"), TimeSeconds);
		Obj->SetNumberField(TEXT("value"), Value);
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildCurveJson(UAnimSequenceBase* Sequence, const FName CurveName, const ERawCurveTrackTypes CurveType, const bool bIncludeKeys)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("curve_name"), CurveName.ToString());
		Item->SetStringField(TEXT("curve_type"), CurveTypeToString(CurveType));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("curve_name"), CurveName.ToString(), CurveName.ToString(), TEXT("Animation/Curves"), TEXT("curve"));

		TArray<TSharedPtr<FJsonValue>> Keys;
		switch (CurveType)
		{
		case ERawCurveTrackTypes::RCT_Float:
		default:
		{
			TArray<float> Times;
			TArray<float> Values;
			UAnimationBlueprintLibrary::GetFloatKeys(Sequence, CurveName, Times, Values);
			TArray<FRichCurveKey> RichKeys;
			for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
			{
				FRichCurveKey RichKey(Times[Index], Values[Index]);
				RichKey.InterpMode = RCIM_Linear;
				RichKeys.Add(RichKey);
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
			if (bIncludeKeys)
			{
				FRichCurve RichCurve;
				RichCurve.SetKeys(RichKeys);
				Item->SetObjectField(TEXT("curve_json"), UeAgentCurveJson::BuildFloatCurveJson(RichCurve, TEXT("animation_raw_curve"), TEXT("FRichCurve")));
			}
			break;
		}
		}

		if (bIncludeKeys)
		{
			Item->SetArrayField(TEXT("keys"), Keys);
		}
		return Item;
	}

	TSharedPtr<FJsonObject> BuildMetadataJson(const UAnimMetaData* MetaDataObject)
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

	FProperty* FindEditablePropertyByFlexibleName(UClass* Class, const FString& InName)
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


	TSharedPtr<FJsonObject> BuildSocketJson(const USkeletalMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("socket_name"), Socket ? Socket->SocketName.ToString() : TEXT(""));
		Item->SetStringField(TEXT("bone_name"), Socket ? Socket->BoneName.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("relative_location"), BuildVectorJson(Socket ? Socket->RelativeLocation : FVector::ZeroVector));
		Item->SetObjectField(TEXT("relative_rotation"), BuildRotatorJson(Socket ? Socket->RelativeRotation : FRotator::ZeroRotator));
		Item->SetObjectField(TEXT("relative_scale"), BuildVectorJson(Socket ? Socket->RelativeScale : FVector::OneVector));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("socket_name"), Socket ? Socket->SocketName.ToString() : TEXT(""), Socket ? Socket->SocketName.ToString() : TEXT("Socket"), TEXT("Skeleton/Sockets"), TEXT("socket"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildVirtualBoneJson(const FVirtualBone& VirtualBone)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source_bone_name"), VirtualBone.SourceBoneName.ToString());
		Item->SetStringField(TEXT("target_bone_name"), VirtualBone.TargetBoneName.ToString());
		Item->SetStringField(TEXT("virtual_bone_name"), VirtualBone.VirtualBoneName.ToString());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("virtual_bone_name"), VirtualBone.VirtualBoneName.ToString(), VirtualBone.VirtualBoneName.ToString(), TEXT("Skeleton/Virtual Bones"), TEXT("bone_link"));
		return Item;
	}

	USkeletalMeshSocket* FindSocketByName(USkeleton* Skeleton, const FName SocketName)
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

	const FVirtualBone* FindVirtualBoneByName(const USkeleton* Skeleton, const FName VirtualBoneName)
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

	bool EnsureNotifyTrack(UAnimSequenceBase* Sequence, const FName TrackName, int32& OutTrackIndex)
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

	UClass* LoadClassByPath(const FString& InPath)
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

	static bool JsonTryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}


	bool JsonTryGetNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
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

	bool JsonTryGetStringAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, FString& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Key : Keys)
		{
			if (Obj->TryGetStringField(Key, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool JsonTryGetFieldAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, TSharedPtr<FJsonValue>& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Key : Keys)
		{
			if (const TSharedPtr<FJsonValue>* FoundValue = Obj->Values.Find(Key))
			{
				if (FoundValue && FoundValue->IsValid())
				{
					OutValue = *FoundValue;
					return true;
				}
			}
		}
		return false;
	}

	static bool JsonTryGetObjectAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, TSharedPtr<FJsonObject>& OutObject)
	{
		TSharedPtr<FJsonValue> Value;
		if (!JsonTryGetFieldAny(Obj, Keys, Value) || !Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		OutObject = Value->AsObject();
		return OutObject.IsValid();
	}

	bool JsonTryGetArrayAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		TSharedPtr<FJsonValue> Value;
		if (!JsonTryGetFieldAny(Obj, Keys, Value) || !Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}
		OutArray = &Value->AsArray();
		return true;
	}

}

namespace UeAgentAnimationViewportOps
{
	FLevelEditorViewportClient* GetPreferredLevelViewportClient()
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
