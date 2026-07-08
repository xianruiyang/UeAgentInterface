// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "UeAgentHttpServer_AnimationAssetOps.h"

#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentFolderDetailsExport.h"
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

	UAnimSequence* CreateAnimSequenceAsset(const FString& AssetPathInput, USkeleton* Skeleton, USkeletalMesh* PreviewMesh, FString& OutError)
	{
		const FString NormalizedPath = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(NormalizedPath))
		{
			OutError = TEXT("invalid_anim_sequence_path");
			return nullptr;
		}
		if (!Skeleton)
		{
			OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedPath);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_anim_sequence_path");
			return nullptr;
		}

		UPackage* Package = CreatePackage(*NormalizedPath);
		if (!Package)
		{
			OutError = TEXT("create_anim_sequence_package_failed");
			return nullptr;
		}
		Package->FullyLoad();

		if (UObject* ExistingObject = FindObject<UObject>(Package, *AssetName))
		{
			OutError = ExistingObject->IsA<UAnimSequence>() ? TEXT("anim_sequence_already_exists") : TEXT("asset_exists_with_different_class");
			return nullptr;
		}

		UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Sequence)
		{
			OutError = TEXT("create_anim_sequence_failed");
			return nullptr;
		}

		Sequence->SetSkeleton(Skeleton);
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.InitializeModel();
		Controller.SetNumberOfFrames(FFrameNumber(1), false);
		if (PreviewMesh)
		{
			Sequence->SetPreviewMesh(PreviewMesh, true);
		}
		Controller.NotifyPopulated();
		FAssetRegistryModule::AssetCreated(Sequence);
		Package->MarkPackageDirty();
		return Sequence;
	}

	static UAnimationAsset* LoadAnimationAsset(const FString& InPath)
	{
		return Cast<UAnimationAsset>(LoadAssetObject(InPath));
	}

	static UBlendSpace* LoadBlendSpace(const FString& InPath)
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

	static bool SaveJsonFile(const FString& FilePathInput, const TSharedPtr<FJsonObject>& Obj, FString& OutResolvedPath, FString& OutError)
	{
		OutResolvedPath.Reset();
		if (!Obj.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}
		FString FilePath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePathInput);
		if (FilePath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		FString JsonText;
		UeAgentJsonDiagnostics::SerializeJsonObjectCompact(Obj, JsonText);
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_failed");
			return false;
		}
		OutResolvedPath = FilePath;
		return true;
	}

	static UClass* ResolveBlendSpaceClass(const FString& Kind)
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

	static FString GetBlendSpaceKind(const UBlendSpace* BlendSpace)
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

	static bool ApplyBlendParameterFromJson(UBlendSpace* BlendSpace, const int32 AxisIndex, const TSharedPtr<FJsonObject>& AxisObj, TArray<TSharedPtr<FJsonValue>>& Issues)
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

	static TArray<FBlendSample>* GetMutableBlendSamples(UBlendSpace* BlendSpace)
	{
		FArrayProperty* SampleDataProperty = BlendSpace ? FindFProperty<FArrayProperty>(BlendSpace->GetClass(), TEXT("SampleData")) : nullptr;
		return SampleDataProperty ? SampleDataProperty->ContainerPtrToValuePtr<TArray<FBlendSample>>(BlendSpace) : nullptr;
	}

	static TSharedPtr<FJsonObject> BuildBlendSpaceJson(UBlendSpace* BlendSpace)
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

	bool SaveAssetPackage(UObject* Asset, FString& OutError)
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

	bool ApplyNotifyCommonSettings(const TSharedPtr<FJsonObject>& Params, FAnimNotifyEvent& Notify, FString& OutError)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("track_name"),
			Track.TrackName.ToString(),
			Track.TrackName.ToString(),
			TEXT("Animation/Notify Tracks"),
			TEXT("notify_track"),
			TEXT("track"),
			{ TEXT("set"), TEXT("remove") });
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("notify_index"),
			FString::FromInt(NotifyIndex),
			Notify.NotifyName.ToString(),
			TEXT("Animation/Notifies"),
			TEXT("anim_notify"),
			TEXT("timeline_event"),
			{ TEXT("set"), TEXT("remove") });
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("marker_index"),
			FString::FromInt(MarkerIndex),
			Marker.MarkerName.ToString(),
			TEXT("Animation/Sync Markers"),
			TEXT("sync_marker"),
			TEXT("timeline_marker"),
			{ TEXT("set"), TEXT("remove") });
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

	static TSharedPtr<FJsonObject> BuildTransformJsonWithQuaternion(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = BuildTransformJson(Value);
		Obj->SetObjectField(TEXT("quaternion"), BuildQuatJson(Value.GetRotation().GetNormalized()));
		return Obj;
	}

	static bool ParseAnimDataEvalType(const FString& InText, EAnimDataEvalType& OutType)
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

	static FString AnimDataEvalTypeToString(const EAnimDataEvalType Type)
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

	static bool ParseAnimPoseSpace(const FString& InText, EAnimPoseSpaces& OutSpace)
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

	static FString AnimPoseSpaceToString(const EAnimPoseSpaces Space)
	{
		return Space == EAnimPoseSpaces::World ? TEXT("world") : TEXT("local");
	}

	static TSharedPtr<FJsonObject> BuildSimpleKeyJson(const double TimeSeconds, const double Value)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("curve_name"),
			CurveName.ToString(),
			CurveName.ToString(),
			TEXT("Animation/Curves"),
			TEXT("animation_curve"),
			TEXT("curve"),
			{ TEXT("set_keys"), TEXT("remove") });

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

	bool ApplySimpleJsonValueToProperty(UObject* TargetObject, FProperty* Property, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
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

	TSharedPtr<FJsonObject> BuildSocketJson(const USkeletalMeshSocket* Socket)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("socket_name"), Socket ? Socket->SocketName.ToString() : TEXT(""));
		Item->SetStringField(TEXT("bone_name"), Socket ? Socket->BoneName.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("relative_location"), BuildVectorJson(Socket ? Socket->RelativeLocation : FVector::ZeroVector));
		Item->SetObjectField(TEXT("relative_rotation"), BuildRotatorJson(Socket ? Socket->RelativeRotation : FRotator::ZeroRotator));
		Item->SetObjectField(TEXT("relative_scale"), BuildVectorJson(Socket ? Socket->RelativeScale : FVector::OneVector));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("socket_name"),
			Socket ? Socket->SocketName.ToString() : TEXT(""),
			Socket ? Socket->SocketName.ToString() : TEXT("Socket"),
			TEXT("Skeleton/Sockets"),
			TEXT("skeleton_socket"),
			TEXT("socket"),
			{ TEXT("set"), TEXT("remove") });
		return Item;
	}

	TSharedPtr<FJsonObject> BuildVirtualBoneJson(const FVirtualBone& VirtualBone)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("source_bone_name"), VirtualBone.SourceBoneName.ToString());
		Item->SetStringField(TEXT("target_bone_name"), VirtualBone.TargetBoneName.ToString());
		Item->SetStringField(TEXT("virtual_bone_name"), VirtualBone.VirtualBoneName.ToString());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(
			Item,
			TEXT("virtual_bone_name"),
			VirtualBone.VirtualBoneName.ToString(),
			VirtualBone.VirtualBoneName.ToString(),
			TEXT("Skeleton/Virtual Bones"),
			TEXT("virtual_bone"),
			TEXT("bone_link"),
			{ TEXT("set"), TEXT("remove") });
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

		FSlateApplication::Get().PumpMessages();
		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();
	}

	static FVector2D ResolveOffscreenWidgetDrawSize(const TSharedRef<SWidget>& Widget)
	{
		FVector2D DrawSize = Widget->GetTickSpaceGeometry().GetLocalSize();
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetTickSpaceGeometry().GetAbsoluteSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetCachedGeometry().GetLocalSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetDesiredSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = FVector2D(1440.0, 900.0);
		}

		const double MaxDimension = 8192.0;
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 64.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 64.0, MaxDimension);
		return DrawSize;
	}

	static bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		FSlateApplication::Get().PumpMessages();
		const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
		Widget->SlatePrepass(WidgetScale);
		FSlateApplication::Get().InvalidateAllViewports();

		const FVector2D DrawSize = ResolveOffscreenWidgetDrawSize(Widget);
		FWidgetRenderer* WidgetRenderer = new FWidgetRenderer(true, true);
		if (!WidgetRenderer)
		{
			OutError = TEXT("widget_renderer_create_failed");
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
		if (!RenderTarget)
		{
			BeginCleanup(WidgetRenderer);
			OutError = TEXT("offscreen_render_target_create_failed");
			return false;
		}

		WidgetRenderer->DrawWidget(RenderTarget, Widget, 1.0f, DrawSize, 0.0f, false);
		FlushRenderingCommands();
		BeginCleanup(WidgetRenderer);

		FRenderTarget* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource)
		{
			OutError = TEXT("offscreen_render_target_resource_missing");
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		OutPixels.Reset();
		if (!RenderTargetResource->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("offscreen_read_pixels_failed");
			return false;
		}

		OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
		if (OutSize.X <= 0 || OutSize.Y <= 0 || OutPixels.Num() < OutSize.X * OutSize.Y)
		{
			OutError = TEXT("offscreen_screenshot_empty");
			return false;
		}
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

	struct FPreviewFrameRequest
	{
		int32 FrameIndex = 0;
		float TimeSeconds = 0.0f;
	};

	static bool JsonTryGetNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
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

	static bool JsonTryGetStringAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, FString& OutValue)
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

	static bool JsonTryGetArrayAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, const TArray<TSharedPtr<FJsonValue>>*& OutArray)
	{
		TSharedPtr<FJsonValue> Value;
		if (!JsonTryGetFieldAny(Obj, Keys, Value) || !Value.IsValid() || Value->Type != EJson::Array)
		{
			return false;
		}
		OutArray = &Value->AsArray();
		return true;
	}

	static bool TryReadVectorJsonValue(const TSharedPtr<FJsonValue>& Value, FVector& OutVector, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("invalid_vector_value");
			return false;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			if (Values.Num() != 3)
			{
				OutError = TEXT("vector_array_requires_3_numbers");
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!Values[0].IsValid() || !Values[0]->TryGetNumber(X) ||
				!Values[1].IsValid() || !Values[1]->TryGetNumber(Y) ||
				!Values[2].IsValid() || !Values[2]->TryGetNumber(Z))
			{
				OutError = TEXT("vector_array_has_non_number");
				return false;
			}
			OutVector = FVector(X, Y, Z);
			return true;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			const bool bHasX = JsonTryGetNumberAnyField(Obj, { TEXT("x"), TEXT("X") }, X);
			const bool bHasY = JsonTryGetNumberAnyField(Obj, { TEXT("y"), TEXT("Y") }, Y);
			const bool bHasZ = JsonTryGetNumberAnyField(Obj, { TEXT("z"), TEXT("Z") }, Z);
			if (!bHasX || !bHasY || !bHasZ)
			{
				OutError = TEXT("vector_object_requires_xyz");
				return false;
			}
			OutVector = FVector(X, Y, Z);
			return true;
		}

		OutError = TEXT("vector_value_must_be_object_or_array");
		return false;
	}

	static bool TryReadRotationJsonValue(const TSharedPtr<FJsonValue>& Value, FQuat& OutRotation, FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("invalid_rotation_value");
			return false;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			if (Values.Num() != 3 && Values.Num() != 4)
			{
				OutError = TEXT("rotation_array_requires_3_or_4_numbers");
				return false;
			}

			double A = 0.0;
			double B = 0.0;
			double C = 0.0;
			if (!Values[0].IsValid() || !Values[0]->TryGetNumber(A) ||
				!Values[1].IsValid() || !Values[1]->TryGetNumber(B) ||
				!Values[2].IsValid() || !Values[2]->TryGetNumber(C))
			{
				OutError = TEXT("rotation_array_has_non_number");
				return false;
			}
			if (Values.Num() == 3)
			{
				OutRotation = FRotator(A, B, C).Quaternion();
				return true;
			}

			double W = 1.0;
			if (!Values[3].IsValid() || !Values[3]->TryGetNumber(W))
			{
				OutError = TEXT("rotation_array_has_non_number");
				return false;
			}
			OutRotation = FQuat(A, B, C, W).GetNormalized();
			return true;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			double W = 0.0;
			if (JsonTryGetNumberAnyField(Obj, { TEXT("w"), TEXT("W") }, W))
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if (!JsonTryGetNumberAnyField(Obj, { TEXT("x"), TEXT("X") }, X) ||
					!JsonTryGetNumberAnyField(Obj, { TEXT("y"), TEXT("Y") }, Y) ||
					!JsonTryGetNumberAnyField(Obj, { TEXT("z"), TEXT("Z") }, Z))
				{
					OutError = TEXT("quat_object_requires_xyzw");
					return false;
				}
				OutRotation = FQuat(X, Y, Z, W).GetNormalized();
				return true;
			}

			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			const bool bHasPitch = JsonTryGetNumberAnyField(Obj, { TEXT("pitch"), TEXT("Pitch") }, Pitch);
			const bool bHasYaw = JsonTryGetNumberAnyField(Obj, { TEXT("yaw"), TEXT("Yaw") }, Yaw);
			const bool bHasRoll = JsonTryGetNumberAnyField(Obj, { TEXT("roll"), TEXT("Roll") }, Roll);
			if (!bHasPitch || !bHasYaw || !bHasRoll)
			{
				OutError = TEXT("rotator_object_requires_pitch_yaw_roll");
				return false;
			}
			OutRotation = FRotator(Pitch, Yaw, Roll).Quaternion();
			return true;
		}

		OutError = TEXT("rotation_value_must_be_object_or_array");
		return false;
	}

	static bool TryReadTransformKeyJson(const TSharedPtr<FJsonObject>& KeyObj, const FTransform& ReferenceTransform, FVector& OutPosition, FQuat& OutRotation, FVector& OutScale, FString& OutError)
	{
		if (!KeyObj.IsValid())
		{
			OutError = TEXT("invalid_transform_key");
			return false;
		}

		OutPosition = ReferenceTransform.GetTranslation();
		OutRotation = ReferenceTransform.GetRotation();
		OutScale = ReferenceTransform.GetScale3D();

		TSharedPtr<FJsonValue> FieldValue;
		if (JsonTryGetFieldAny(KeyObj, { TEXT("location"), TEXT("translation"), TEXT("position") }, FieldValue))
		{
			if (!TryReadVectorJsonValue(FieldValue, OutPosition, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_location:%s"), *OutError);
				return false;
			}
		}

		if (JsonTryGetFieldAny(KeyObj, { TEXT("rotation"), TEXT("rotator"), TEXT("quat"), TEXT("quaternion") }, FieldValue))
		{
			if (!TryReadRotationJsonValue(FieldValue, OutRotation, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_rotation:%s"), *OutError);
				return false;
			}
		}
		else if (JsonTryGetFieldAny(KeyObj, { TEXT("rotation_delta"), TEXT("rotator_delta"), TEXT("quat_delta"), TEXT("quaternion_delta"), TEXT("additive_rotation") }, FieldValue))
		{
			FQuat RotationDelta = FQuat::Identity;
			if (!TryReadRotationJsonValue(FieldValue, RotationDelta, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_rotation_delta:%s"), *OutError);
				return false;
			}
			OutRotation = (ReferenceTransform.GetRotation() * RotationDelta).GetNormalized();
		}
		else if (JsonTryGetFieldAny(KeyObj, { TEXT("pre_rotation_delta"), TEXT("pre_rotator_delta"), TEXT("pre_quat_delta"), TEXT("pre_quaternion_delta") }, FieldValue))
		{
			FQuat RotationDelta = FQuat::Identity;
			if (!TryReadRotationJsonValue(FieldValue, RotationDelta, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_pre_rotation_delta:%s"), *OutError);
				return false;
			}
			OutRotation = (RotationDelta * ReferenceTransform.GetRotation()).GetNormalized();
		}

		if (JsonTryGetFieldAny(KeyObj, { TEXT("scale"), TEXT("scale3d") }, FieldValue))
		{
			if (!TryReadVectorJsonValue(FieldValue, OutScale, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_scale:%s"), *OutError);
				return false;
			}
		}

		return true;
	}

	static bool ResolveAnimFrameRate(const TSharedPtr<FJsonObject>& Params, const FFrameRate& Fallback, FFrameRate& OutFrameRate, FString& OutError)
	{
		int32 Numerator = Fallback.Numerator > 0 ? Fallback.Numerator : 30;
		int32 Denominator = Fallback.Denominator > 0 ? Fallback.Denominator : 1;

		TSharedPtr<FJsonObject> FrameRateObj;
		if (JsonTryGetObjectAnyField(Params, { TEXT("frame_rate"), TEXT("sampling_frame_rate") }, FrameRateObj))
		{
			double NumValue = 0.0;
			double DenValue = 1.0;
			if (!JsonTryGetNumberAnyField(FrameRateObj, { TEXT("numerator"), TEXT("num") }, NumValue))
			{
				OutError = TEXT("frame_rate_missing_numerator");
				return false;
			}
			JsonTryGetNumberAnyField(FrameRateObj, { TEXT("denominator"), TEXT("den") }, DenValue);
			Numerator = FMath::RoundToInt(NumValue);
			Denominator = FMath::RoundToInt(DenValue);
		}
		else
		{
			double FpsValue = 0.0;
			if (JsonTryGetNumberAnyField(Params, { TEXT("fps"), TEXT("frame_rate") }, FpsValue))
			{
				Numerator = FMath::RoundToInt(FpsValue);
				Denominator = 1;
			}

			double NumValue = 0.0;
			double DenValue = 0.0;
			if (JsonTryGetNumberAnyField(Params, { TEXT("frame_rate_numerator"), TEXT("sampling_frame_rate_numerator") }, NumValue))
			{
				Numerator = FMath::RoundToInt(NumValue);
			}
			if (JsonTryGetNumberAnyField(Params, { TEXT("frame_rate_denominator"), TEXT("sampling_frame_rate_denominator") }, DenValue))
			{
				Denominator = FMath::RoundToInt(DenValue);
			}
		}

		if (Numerator <= 0 || Denominator <= 0)
		{
			OutError = TEXT("invalid_frame_rate");
			return false;
		}

		OutFrameRate = FFrameRate(Numerator, Denominator);
		return true;
	}

	struct FAnimSequenceBoneTrackInput
	{
		FName BoneName;
		TArray<FVector> Positions;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
		int32 ProvidedKeyCount = 0;
		bool bUsedExplicitFrames = false;
	};

	static bool TryResolveBoneKeyFrameIndex(const TSharedPtr<FJsonObject>& KeyObj, const FFrameRate& FrameRate, const int32 RequiredKeyCount, int32& OutFrameIndex, bool& bOutHasExplicitFrame, FString& OutError)
	{
		bOutHasExplicitFrame = false;
		double FrameValue = 0.0;
		if (JsonTryGetNumberAnyField(KeyObj, { TEXT("frame"), TEXT("frame_index"), TEXT("key_index") }, FrameValue))
		{
			bOutHasExplicitFrame = true;
			OutFrameIndex = FMath::RoundToInt(FrameValue);
		}
		else
		{
			double TimeSeconds = 0.0;
			if (!JsonTryGetNumberAnyField(KeyObj, { TEXT("time_seconds"), TEXT("time") }, TimeSeconds))
			{
				return true;
			}
			bOutHasExplicitFrame = true;
			OutFrameIndex = FrameRate.AsFrameTime(TimeSeconds).RoundToFrame().Value;
		}

		if (OutFrameIndex < 0 || OutFrameIndex >= RequiredKeyCount)
		{
			OutError = TEXT("bone_track_key_frame_out_of_range");
			return false;
		}
		return true;
	}

	static bool ParseAnimSequenceBoneTrackInput(
		const TSharedPtr<FJsonObject>& TrackObj,
		const FReferenceSkeleton& RefSkeleton,
		const int32 RequiredKeyCount,
		const FFrameRate& FrameRate,
		FAnimSequenceBoneTrackInput& OutTrack,
		FString& OutError)
	{
		if (!TrackObj.IsValid())
		{
			OutError = TEXT("invalid_bone_track_object");
			return false;
		}

		FString BoneNameText;
		if (!JsonTryGetStringAnyField(TrackObj, { TEXT("bone"), TEXT("bone_name"), TEXT("track_name") }, BoneNameText) || BoneNameText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("bone_track_missing_bone_name");
			return false;
		}
		BoneNameText = BoneNameText.TrimStartAndEnd();
		const FName BoneName(*BoneNameText);
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("bone_not_found:%s"), *BoneNameText);
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!JsonTryGetArrayAnyField(TrackObj, { TEXT("keys"), TEXT("transforms") }, Keys) || !Keys || Keys->Num() == 0)
		{
			OutError = FString::Printf(TEXT("bone_track_missing_keys:%s"), *BoneNameText);
			return false;
		}

		const FTransform ReferenceTransform = RefSkeleton.GetRefBonePose()[BoneIndex];
		OutTrack.BoneName = BoneName;
		OutTrack.Positions.Init(ReferenceTransform.GetTranslation(), RequiredKeyCount);
		OutTrack.Rotations.Init(ReferenceTransform.GetRotation(), RequiredKeyCount);
		OutTrack.Scales.Init(ReferenceTransform.GetScale3D(), RequiredKeyCount);
		OutTrack.ProvidedKeyCount = Keys->Num();
		OutTrack.bUsedExplicitFrames = false;

		bool bAnyExplicitFrames = false;
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			if (!KeyValue.IsValid() || KeyValue->Type != EJson::Object)
			{
				OutError = FString::Printf(TEXT("bone_track_key_must_be_object:%s"), *BoneNameText);
				return false;
			}
			const TSharedPtr<FJsonObject> KeyObj = KeyValue->AsObject();
			int32 FrameIndex = 0;
			bool bHasExplicitFrame = false;
			if (!TryResolveBoneKeyFrameIndex(KeyObj, FrameRate, RequiredKeyCount, FrameIndex, bHasExplicitFrame, OutError))
			{
				OutError = FString::Printf(TEXT("%s:%s"), *OutError, *BoneNameText);
				return false;
			}
			bAnyExplicitFrames = bAnyExplicitFrames || bHasExplicitFrame;
		}

		if (!bAnyExplicitFrames && Keys->Num() != RequiredKeyCount)
		{
			OutError = FString::Printf(TEXT("bone_track_key_count_mismatch:%s:expected_%d_got_%d"), *BoneNameText, RequiredKeyCount, Keys->Num());
			return false;
		}
		OutTrack.bUsedExplicitFrames = bAnyExplicitFrames;

		for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
		{
			const TSharedPtr<FJsonObject> KeyObj = (*Keys)[KeyIndex]->AsObject();
			int32 FrameIndex = KeyIndex;
			bool bHasExplicitFrame = false;
			if (!TryResolveBoneKeyFrameIndex(KeyObj, FrameRate, RequiredKeyCount, FrameIndex, bHasExplicitFrame, OutError))
			{
				OutError = FString::Printf(TEXT("%s:%s"), *OutError, *BoneNameText);
				return false;
			}
			if (!bAnyExplicitFrames)
			{
				FrameIndex = KeyIndex;
			}

			FVector Position;
			FQuat Rotation;
			FVector Scale;
			if (!TryReadTransformKeyJson(KeyObj, ReferenceTransform, Position, Rotation, Scale, OutError))
			{
				OutError = FString::Printf(TEXT("invalid_bone_track_key:%s:%s"), *BoneNameText, *OutError);
				return false;
			}
			OutTrack.Positions[FrameIndex] = Position;
			OutTrack.Rotations[FrameIndex] = Rotation.GetNormalized();
			OutTrack.Scales[FrameIndex] = Scale;
		}
		return true;
	}

	static bool ResolvePreviewFrameSequence(UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Params, TArray<FPreviewFrameRequest>& OutFrames, FString& OutError)
	{
		OutFrames.Reset();
		if (!Sequence || !Params.IsValid())
		{
			OutError = TEXT("invalid_anim_sequence");
			return false;
		}

		const int32 MaxFrameIndex = FMath::Max(0, Sequence->GetNumberOfSampledKeys() - 1);
		const float SequenceLength = Sequence->GetPlayLength();
		const FFrameRate SamplingFrameRate = Sequence->GetSamplingFrameRate();
		const double SecondsPerFrame =
			SamplingFrameRate.IsValid() && SamplingFrameRate.AsDecimal() > KINDA_SMALL_NUMBER
				? 1.0 / SamplingFrameRate.AsDecimal()
				: 1.0 / 30.0;

		double CountValue = 1.0;
		JsonTryGetNumberAnyField(Params, { TEXT("count"), TEXT("frame_count"), TEXT("sample_count") }, CountValue);
		const int32 RequestedCount = FMath::RoundToInt(CountValue);
		if (RequestedCount < 1 || RequestedCount > 120)
		{
			OutError = TEXT("invalid_frame_count");
			return false;
		}

		auto AddFrameFromFrameIndex = [&](const int32 InFrameIndex) -> bool
		{
			if (InFrameIndex < 0 || InFrameIndex > MaxFrameIndex)
			{
				OutError = TEXT("preview_frame_range_exceeds_sequence");
				return false;
			}
			FPreviewFrameRequest Request;
			Request.FrameIndex = InFrameIndex;
			Request.TimeSeconds = static_cast<float>(SamplingFrameRate.AsSeconds(FFrameNumber(Request.FrameIndex)));
			OutFrames.Add(Request);
			return true;
		};

		auto AddFrameFromTime = [&](const double InTimeSeconds) -> bool
		{
			if (InTimeSeconds < -KINDA_SMALL_NUMBER || InTimeSeconds > static_cast<double>(SequenceLength) + KINDA_SMALL_NUMBER)
			{
				OutError = TEXT("preview_time_range_exceeds_sequence");
				return false;
			}
			const float ClampedTime = FMath::Clamp(static_cast<float>(InTimeSeconds), 0.0f, SequenceLength);
			const int32 FrameIndex = FMath::Clamp(SamplingFrameRate.AsFrameTime(ClampedTime).RoundToFrame().Value, 0, MaxFrameIndex);
			return AddFrameFromFrameIndex(FrameIndex);
		};

		double StartTimeSeconds = 0.0;
		const bool bHasStartTime = JsonTryGetNumberAnyField(Params, { TEXT("start_time_seconds"), TEXT("first_time_seconds"), TEXT("time_seconds") }, StartTimeSeconds);

		double IntervalSeconds = SecondsPerFrame;
		const bool bHasIntervalSeconds = JsonTryGetNumberAnyField(Params, { TEXT("interval_seconds"), TEXT("time_interval_seconds") }, IntervalSeconds);

		double StartFrameValue = 0.0;
		const bool bHasStartFrame = JsonTryGetNumberAnyField(Params, { TEXT("start_frame_index"), TEXT("first_frame_index"), TEXT("frame_index"), TEXT("frame") }, StartFrameValue);

		double FrameIntervalValue = 1.0;
		const bool bHasFrameInterval = JsonTryGetNumberAnyField(Params, { TEXT("frame_interval"), TEXT("frame_stride") }, FrameIntervalValue);

		if (bHasStartTime || bHasIntervalSeconds)
		{
			if (!bHasIntervalSeconds && bHasFrameInterval)
			{
				IntervalSeconds = FrameIntervalValue * SecondsPerFrame;
			}
			if (IntervalSeconds < 0.0)
			{
				OutError = TEXT("invalid_interval_seconds");
				return false;
			}
			for (int32 Index = 0; Index < RequestedCount; ++Index)
			{
				if (!AddFrameFromTime(StartTimeSeconds + IntervalSeconds * static_cast<double>(Index)))
				{
					return false;
				}
			}
			return true;
		}

		if (FrameIntervalValue < 0.0)
		{
			OutError = TEXT("invalid_frame_interval");
			return false;
		}

		const int32 StartFrameIndex = static_cast<int32>(StartFrameValue);
		const int32 FrameInterval = FMath::Max(0, FMath::RoundToInt(FrameIntervalValue));
		for (int32 Index = 0; Index < RequestedCount; ++Index)
		{
			const int32 FrameIndex = StartFrameIndex + FrameInterval * Index;
			if (!AddFrameFromFrameIndex(FrameIndex))
			{
				if (RequestedCount == 1)
				{
					OutError = TEXT("invalid_frame_index");
				}
				return false;
			}
		}
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
	if (CommandLower == TEXT("anim_sequence_analyze_root_motion")) return CmdAnimSequenceAnalyzeRootMotion(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_sample_pose")) return CmdAnimSequenceSamplePose(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_screenshot")) return CmdAnimSequenceScreenshot(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_settings")) return CmdAnimSequenceSetSettings(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_preview_mesh")) return CmdAnimSequenceSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_curve")) return CmdAnimSequenceSetCurve(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_bones")) return CmdAnimSequenceSetBones(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_apply_bone_tracks")) return CmdAnimSequenceApplyBoneTracks(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_metadata")) return CmdAnimSequenceSetMetadata(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_notify")) return CmdAnimSequenceSetNotify(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_notify_track")) return CmdAnimSequenceSetNotifyTrack(Ctx, OutData, OutError);
	if (CommandLower == TEXT("anim_sequence_set_sync_markers")) return CmdAnimSequenceSetSyncMarkers(Ctx, OutData, OutError);

	OutError = TEXT("unknown_anim_sequence_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteBlendSpaceCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("blendspace_create")) return CmdBlendSpaceCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blendspace_get_info")) return CmdBlendSpaceGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blendspace_export_json")) return CmdBlendSpaceExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blendspace_validate_json")) return CmdBlendSpaceValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blendspace_apply_json")) return CmdBlendSpaceApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("blendspace_preview_sample")) return CmdBlendSpacePreviewSample(Ctx, OutData, OutError);

	OutError = TEXT("unknown_blendspace_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteSkeletonCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("skeleton_get_info")) return CmdSkeletonGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_list_bones")) return CmdSkeletonListBones(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_export_folder")) return CmdSkeletonExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_apply_folder")) return CmdSkeletonApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_validate_folder")) return CmdSkeletonValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_compatible_skeletons")) return CmdSkeletonSetCompatibleSkeletons(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_preview_mesh")) return CmdSkeletonSetPreviewMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_socket")) return CmdSkeletonSetSocket(Ctx, OutData, OutError);
	if (CommandLower == TEXT("skeleton_set_virtual_bone")) return CmdSkeletonSetVirtualBone(Ctx, OutData, OutError);

	OutError = TEXT("unknown_skeleton_command");
	return false;
}

bool FUeAgentHttpServer::CmdBlendSpaceCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString SkeletonPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("skeleton"), SkeletonPath) || SkeletonPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_skeleton");
		return false;
	}
	USkeleton* Skeleton = UeAgentAnimationAssetOps::LoadSkeleton(SkeletonPath);
	if (!Skeleton)
	{
		OutError = TEXT("skeleton_not_found");
		return false;
	}

	const FString PackageName = UeAgentAnimationAssetOps::NormalizeAssetPath(AssetPath);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	if (UeAgentAnimationAssetOps::LoadBlendSpace(PackageName))
	{
		OutError = TEXT("blendspace_already_exists");
		return false;
	}

	FString Kind = TEXT("blendspace_2d");
	JsonTryGetString(Ctx.Params, TEXT("blendspace_kind"), Kind);
	UClass* BlendSpaceClass = UeAgentAnimationAssetOps::ResolveBlendSpaceClass(Kind);
	UPackage* Package = CreatePackage(*PackageName);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UBlendSpace* BlendSpace = NewObject<UBlendSpace>(Package, BlendSpaceClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!BlendSpace)
	{
		OutError = TEXT("create_blendspace_failed");
		return false;
	}
	BlendSpace->SetSkeleton(Skeleton);
	BlendSpace->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(BlendSpace);

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		JsonObj->SetField(Pair.Key, Pair.Value);
	}
	JsonObj->SetStringField(TEXT("asset_path"), PackageName);
	JsonObj->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	JsonObj->SetStringField(TEXT("blendspace_kind"), UeAgentAnimationAssetOps::GetBlendSpaceKind(BlendSpace));
	TSharedPtr<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetObjectField(TEXT("json"), JsonObj);
	ApplyParams->SetBoolField(TEXT("save_after_apply"), false);
	TSharedPtr<FJsonObject> ApplyData = MakeShared<FJsonObject>();
	FString ApplyError;
	CmdBlendSpaceApplyJson(FUeAgentRequestContext{ TEXT("blendspace_create_apply"), TEXT("blendspace_apply_json"), ApplyParams }, ApplyData, ApplyError);

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentAnimationAssetOps::SaveAssetPackage(BlendSpace, OutError))
	{
		return false;
	}
	OutData = UeAgentAnimationAssetOps::BuildBlendSpaceJson(BlendSpace);
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	OutData->SetObjectField(TEXT("apply_result"), ApplyData);
	return true;
}

bool FUeAgentHttpServer::CmdBlendSpaceGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlendSpace* BlendSpace = UeAgentAnimationAssetOps::LoadBlendSpace(AssetPath);
	if (!BlendSpace)
	{
		OutError = TEXT("blendspace_not_found");
		return false;
	}
	OutData = UeAgentAnimationAssetOps::BuildBlendSpaceJson(BlendSpace);
	OutData->SetBoolField(TEXT("dirty"), BlendSpace->GetOutermost()->IsDirty());
	return true;
}

bool FUeAgentHttpServer::CmdBlendSpaceExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdBlendSpaceGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString OutputFile;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString ResolvedPath;
		if (!UeAgentAnimationAssetOps::SaveJsonFile(OutputFile, OutData, ResolvedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), ResolvedPath);
	}
	return true;
}

bool FUeAgentHttpServer::CmdBlendSpaceValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
	{
		Params->SetField(Pair.Key, Pair.Value);
	}
	Params->SetBoolField(TEXT("validate_only"), true);
	Params->SetBoolField(TEXT("dry_run"), true);
	FUeAgentRequestContext SubCtx;
	SubCtx.RequestId = TEXT("blendspace_json_validate");
	SubCtx.Command = TEXT("blendspace_apply_json");
	SubCtx.Params = Params;
	return CmdBlendSpaceApplyJson(SubCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdBlendSpaceApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (Ctx.Params.IsValid())
	{
		const TSharedPtr<FJsonObject>* InlineJson = nullptr;
		if (Ctx.Params->TryGetObjectField(TEXT("json"), InlineJson) && InlineJson && InlineJson->IsValid())
		{
			JsonObj = *InlineJson;
		}
	}
	if (!JsonObj.IsValid())
	{
		FString JsonFile;
		if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_json_or_json_file");
			return false;
		}
		if (!UeAgentJsonDiagnostics::LoadJsonObjectFile(JsonFile, JsonObj, &Issues, OutError, false))
		{
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			return false;
		}
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(JsonObj, TEXT("blendspace"), { TEXT("schema"), TEXT("asset_path"), TEXT("object_path"), TEXT("blendspace_kind"), TEXT("skeleton"), TEXT("preview_skeletal_mesh"), TEXT("axes"), TEXT("samples"), TEXT("settings") }, Issues);

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	UBlendSpace* BlendSpace = UeAgentAnimationAssetOps::LoadBlendSpace(AssetPath);
	if (!BlendSpace)
	{
		OutError = TEXT("blendspace_not_found");
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);

	int32 AppliedAxes = 0;
	int32 AppliedSamples = 0;
	const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
	if (JsonObj->TryGetArrayField(TEXT("axes"), Axes) && Axes)
	{
		for (int32 Index = 0; Index < Axes->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> AxisObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Axes)[Index], FString::Printf(TEXT("axes[%d]"), Index), AxisObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(AxisObj, FString::Printf(TEXT("axes[%d]"), Index), { TEXT("axis_index"), TEXT("name"), TEXT("min"), TEXT("max"), TEXT("grid_num"), TEXT("snap_to_grid"), TEXT("wrap_input") }, Issues);
			double AxisIndexValue = Index;
			AxisObj->TryGetNumberField(TEXT("axis_index"), AxisIndexValue);
			if (!bDryRun && !bValidateOnly && UeAgentAnimationAssetOps::ApplyBlendParameterFromJson(BlendSpace, static_cast<int32>(AxisIndexValue), AxisObj, Issues))
			{
				++AppliedAxes;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
	if (JsonObj->TryGetArrayField(TEXT("samples"), Samples) && Samples)
	{
		TArray<TSharedPtr<FJsonValue>> SampleIssues;
		if (!bDryRun && !bValidateOnly)
		{
			BlendSpace->Modify();
			while (BlendSpace->GetNumberOfBlendSamples() > 0)
			{
				BlendSpace->DeleteSample(0);
			}
		}
		for (int32 Index = 0; Index < Samples->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> SampleObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Samples)[Index], FString::Printf(TEXT("samples[%d]"), Index), SampleObj, Issues))
			{
				continue;
			}
			UeAgentJsonDiagnostics::WarnUnknownFields(SampleObj, FString::Printf(TEXT("samples[%d]"), Index), { TEXT("sample_index"), TEXT("animation"), TEXT("position"), TEXT("rate_scale") }, Issues);
			FString AnimPath;
			if (!SampleObj->TryGetStringField(TEXT("animation"), AnimPath) || AnimPath.TrimStartAndEnd().IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), FString::Printf(TEXT("samples[%d].animation"), Index), TEXT("animation is required."));
				continue;
			}
			UAnimSequence* AnimSequence = UeAgentAnimationAssetOps::LoadAnimSequence(AnimPath);
			if (!AnimSequence)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("anim_sequence_not_found"), FString::Printf(TEXT("samples[%d].animation"), Index), AnimPath);
				continue;
			}
			if (!BlendSpace->IsAnimationCompatibleWithSkeleton(AnimSequence))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("blendspace_sample_skeleton_incompatible"), FString::Printf(TEXT("samples[%d].animation"), Index), AnimPath);
				continue;
			}
			FVector Position = FVector::ZeroVector;
			UeAgentJsonDiagnostics::ReadVectorField(SampleObj, TEXT("position"), FString::Printf(TEXT("samples[%d].position"), Index), Position, Issues, true);
			if (!bDryRun && !bValidateOnly)
			{
				const int32 NewSampleIndex = BlendSpace->AddSample(AnimSequence, Position);
				if (TArray<FBlendSample>* MutableSamples = UeAgentAnimationAssetOps::GetMutableBlendSamples(BlendSpace))
				{
					if (MutableSamples->IsValidIndex(NewSampleIndex))
					{
						double RateScale = (*MutableSamples)[NewSampleIndex].RateScale;
						if (SampleObj->TryGetNumberField(TEXT("rate_scale"), RateScale))
						{
							(*MutableSamples)[NewSampleIndex].RateScale = FMath::Max(0.01f, static_cast<float>(RateScale));
						}
					}
				}
				++AppliedSamples;
			}
		}
		if (!bDryRun && !bValidateOnly)
		{
			BlendSpace->ValidateSampleData();
			BlendSpace->ResampleData();
		}
	}

	if (!bDryRun && !bValidateOnly)
	{
		BlendSpace->MarkPackageDirty();
		bool bSaveAfterApply = false;
		JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
		if (bSaveAfterApply && !UeAgentAnimationAssetOps::SaveAssetPackage(BlendSpace, OutError))
		{
			return false;
		}
	}

	OutData = UeAgentAnimationAssetOps::BuildBlendSpaceJson(BlendSpace);
	OutData->SetBoolField(TEXT("applied"), !(bDryRun || bValidateOnly));
	OutData->SetNumberField(TEXT("applied_axes"), AppliedAxes);
	OutData->SetNumberField(TEXT("applied_samples"), AppliedSamples);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdBlendSpacePreviewSample(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UBlendSpace* BlendSpace = UeAgentAnimationAssetOps::LoadBlendSpace(AssetPath);
	if (!BlendSpace)
	{
		OutError = TEXT("blendspace_not_found");
		return false;
	}
	FVector Position = FVector::ZeroVector;
	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentJsonDiagnostics::ReadVectorField(Ctx.Params, TEXT("position"), TEXT("position"), Position, Issues, false);
	JsonTryGetVector(Ctx.Params, TEXT("position"), Position);
	TArray<FBlendSampleData> SampleData;
	int32 CachedIndex = INDEX_NONE;
	const bool bHasSamples = BlendSpace->GetSamplesFromBlendInput(Position, SampleData, CachedIndex, true);
	TArray<TSharedPtr<FJsonValue>> Samples;
	for (const FBlendSampleData& Sample : SampleData)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("sample_data_index"), Sample.SampleDataIndex);
		Item->SetNumberField(TEXT("total_weight"), Sample.TotalWeight);
		Samples.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("asset_path"), BlendSpace->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("position"), UeAgentAnimationAssetOps::BuildVectorJson(Position));
	OutData->SetBoolField(TEXT("has_samples"), bHasSamples);
	OutData->SetArrayField(TEXT("sample_weights"), Samples);
	return true;
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

bool FUeAgentHttpServer::CmdAnimSequenceAnalyzeRootMotion(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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

	float SequenceLengthFloat = 0.0f;
	int32 NumFrames = 0;
	UAnimationBlueprintLibrary::GetSequenceLength(Sequence, SequenceLengthFloat);
	UAnimationBlueprintLibrary::GetNumFrames(Sequence, NumFrames);
	const double SequenceLength = static_cast<double>(SequenceLengthFloat);
	if (SequenceLength <= 0.0)
	{
		OutError = TEXT("invalid_sequence_length");
		return false;
	}

	double SampleCountNumber = 17.0;
	JsonTryGetNumber(Ctx.Params, TEXT("sample_count"), SampleCountNumber);
	const int32 SampleCount = FMath::Clamp(static_cast<int32>(FMath::RoundToInt(SampleCountNumber)), 2, 121);

	bool bIncludeSamples = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_samples"), bIncludeSamples);
	bool bIncludeSuggestedKeys = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_suggested_keys"), bIncludeSuggestedKeys);

	double MeaningfulYawThresholdDegrees = 15.0;
	JsonTryGetNumber(Ctx.Params, TEXT("meaningful_yaw_threshold_degrees"), MeaningfulYawThresholdDegrees);
	MeaningfulYawThresholdDegrees = FMath::Max(0.0, MeaningfulYawThresholdDegrees);

	double MeaningfulTranslationThresholdCm = 2.0;
	JsonTryGetNumber(Ctx.Params, TEXT("meaningful_translation_threshold_cm"), MeaningfulTranslationThresholdCm);
	MeaningfulTranslationThresholdCm = FMath::Max(0.0, MeaningfulTranslationThresholdCm);

	const USkeleton* Skeleton = Sequence->GetSkeleton();
	FString RootBoneName;
	int32 OrientationBoneIndex = INDEX_NONE;
	FString OrientationBoneName;
	if (Skeleton)
	{
		const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
		if (RefSkeleton.GetNum() > 0)
		{
			RootBoneName = RefSkeleton.GetBoneName(0).ToString();
		}

		FString RequestedOrientationBoneName;
		JsonTryGetString(Ctx.Params, TEXT("orientation_bone_name"), RequestedOrientationBoneName);
		RequestedOrientationBoneName.TrimStartAndEndInline();
		if (!RequestedOrientationBoneName.IsEmpty())
		{
			OrientationBoneIndex = RefSkeleton.FindBoneIndex(FName(*RequestedOrientationBoneName));
		}
		else
		{
			const FString CandidateNames[] = { TEXT("hips"), TEXT("pelvis"), TEXT("spine"), TEXT("root") };
			for (const FString& CandidateName : CandidateNames)
			{
				OrientationBoneIndex = RefSkeleton.FindBoneIndex(FName(*CandidateName));
				if (OrientationBoneIndex != INDEX_NONE)
				{
					break;
				}
			}
		}
		if (OrientationBoneIndex != INDEX_NONE)
		{
			OrientationBoneName = RefSkeleton.GetBoneName(OrientationBoneIndex).ToString();
		}
	}

	TArray<FName> TrackNames;
	UAnimationBlueprintLibrary::GetAnimationTrackNames(Sequence, TrackNames);
	bool bHasRootBoneTrack = false;
	TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
	for (const FName TrackName : TrackNames)
	{
		const FString TrackNameText = TrackName.ToString();
		TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackNameText));
		if (!RootBoneName.IsEmpty() && TrackNameText.Equals(RootBoneName, ESearchCase::IgnoreCase))
		{
			bHasRootBoneTrack = true;
		}
	}

	const FTransform TotalRootMotion = Sequence->ExtractRootMotionFromRange(0.0, SequenceLength, FAnimExtractContext());
	const FVector TotalLocation = TotalRootMotion.GetTranslation();
	const double TotalHorizontalDistance = FVector(TotalLocation.X, TotalLocation.Y, 0.0).Size();
	const double TotalYaw = TotalRootMotion.Rotator().Yaw;

	FTransform InitialRawRoot = FTransform::Identity;
	if (Skeleton && !RootBoneName.IsEmpty())
	{
		Sequence->GetBoneTransform(InitialRawRoot, FSkeletonPoseBoneIndex(0), FAnimExtractContext(0.0), false);
	}
	FTransform InitialOrientationBone = FTransform::Identity;
	FTransform TotalOrientationBoneDelta = FTransform::Identity;
	if (Skeleton && OrientationBoneIndex != INDEX_NONE)
	{
		Sequence->GetBoneTransform(InitialOrientationBone, FSkeletonPoseBoneIndex(OrientationBoneIndex), FAnimExtractContext(0.0), false);
		FTransform FinalOrientationBone = FTransform::Identity;
		Sequence->GetBoneTransform(FinalOrientationBone, FSkeletonPoseBoneIndex(OrientationBoneIndex), FAnimExtractContext(SequenceLength), false);
		TotalOrientationBoneDelta = FinalOrientationBone.GetRelativeTransform(InitialOrientationBone);
	}
	const double TotalOrientationBoneDeltaYaw = TotalOrientationBoneDelta.Rotator().Yaw;

	TArray<TSharedPtr<FJsonValue>> Samples;
	TArray<TSharedPtr<FJsonValue>> SuggestedRootMotionYawKeys;
	TArray<TSharedPtr<FJsonValue>> SuggestedOrientationYawKeys;
	TArray<TSharedPtr<FJsonValue>> SuggestedDistanceKeys;
	double MaxAbsRootMotionYaw = 0.0;
	double MaxRootMotionHorizontalDistance = 0.0;
	double MaxAbsRawRootDeltaYaw = 0.0;
	double MaxRawRootDeltaHorizontalDistance = 0.0;
	double MaxAbsOrientationBoneDeltaYaw = 0.0;
	double RootMotionPathDistance = 0.0;
	FVector PreviousRootMotionLocation = FVector::ZeroVector;
	double PivotCommitTime = SequenceLength * 0.5;
	bool bFoundPivotCommit = false;
	const double PivotYawThreshold = FMath::Abs(TotalYaw) > KINDA_SMALL_NUMBER ? FMath::Abs(TotalYaw) * 0.5 : MeaningfulYawThresholdDegrees;
	double OrientationPivotCommitTime = SequenceLength * 0.5;
	bool bFoundOrientationPivotCommit = false;
	const double OrientationPivotYawThreshold = FMath::Abs(TotalOrientationBoneDeltaYaw) > KINDA_SMALL_NUMBER ? FMath::Abs(TotalOrientationBoneDeltaYaw) * 0.5 : MeaningfulYawThresholdDegrees;

	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const double Alpha = SampleCount > 1 ? static_cast<double>(SampleIndex) / static_cast<double>(SampleCount - 1) : 0.0;
		const double SampleTime = SequenceLength * Alpha;
		const FTransform RootMotionAtTime = Sequence->ExtractRootMotionFromRange(0.0, SampleTime, FAnimExtractContext());
		const FVector RootMotionLocation = RootMotionAtTime.GetTranslation();
		const double RootMotionYaw = RootMotionAtTime.Rotator().Yaw;
		const double RootMotionHorizontalDistance = FVector(RootMotionLocation.X, RootMotionLocation.Y, 0.0).Size();
		if (SampleIndex > 0)
		{
			const FVector CurrentHorizontalLocation(RootMotionLocation.X, RootMotionLocation.Y, 0.0);
			const FVector PreviousHorizontalLocation(PreviousRootMotionLocation.X, PreviousRootMotionLocation.Y, 0.0);
			RootMotionPathDistance += FVector::Distance(PreviousHorizontalLocation, CurrentHorizontalLocation);
		}
		PreviousRootMotionLocation = RootMotionLocation;

		FTransform RawRootAtTime = FTransform::Identity;
		FTransform RawRootDelta = FTransform::Identity;
		if (Skeleton && !RootBoneName.IsEmpty())
		{
			Sequence->GetBoneTransform(RawRootAtTime, FSkeletonPoseBoneIndex(0), FAnimExtractContext(SampleTime), false);
			RawRootDelta = RawRootAtTime.GetRelativeTransform(InitialRawRoot);
		}
		const FVector RawRootDeltaLocation = RawRootDelta.GetTranslation();
		const double RawRootDeltaYaw = RawRootDelta.Rotator().Yaw;
		const double RawRootDeltaHorizontalDistance = FVector(RawRootDeltaLocation.X, RawRootDeltaLocation.Y, 0.0).Size();

		FTransform OrientationBoneAtTime = FTransform::Identity;
		FTransform OrientationBoneDelta = FTransform::Identity;
		if (Skeleton && OrientationBoneIndex != INDEX_NONE)
		{
			Sequence->GetBoneTransform(OrientationBoneAtTime, FSkeletonPoseBoneIndex(OrientationBoneIndex), FAnimExtractContext(SampleTime), false);
			OrientationBoneDelta = OrientationBoneAtTime.GetRelativeTransform(InitialOrientationBone);
		}
		const double OrientationBoneDeltaYaw = OrientationBoneDelta.Rotator().Yaw;

		MaxAbsRootMotionYaw = FMath::Max(MaxAbsRootMotionYaw, FMath::Abs(RootMotionYaw));
		MaxRootMotionHorizontalDistance = FMath::Max(MaxRootMotionHorizontalDistance, RootMotionHorizontalDistance);
		MaxAbsRawRootDeltaYaw = FMath::Max(MaxAbsRawRootDeltaYaw, FMath::Abs(RawRootDeltaYaw));
		MaxRawRootDeltaHorizontalDistance = FMath::Max(MaxRawRootDeltaHorizontalDistance, RawRootDeltaHorizontalDistance);
		MaxAbsOrientationBoneDeltaYaw = FMath::Max(MaxAbsOrientationBoneDeltaYaw, FMath::Abs(OrientationBoneDeltaYaw));

		if (!bFoundPivotCommit && PivotYawThreshold > KINDA_SMALL_NUMBER && FMath::Abs(RootMotionYaw) >= PivotYawThreshold)
		{
			PivotCommitTime = SampleTime;
			bFoundPivotCommit = true;
		}
		if (!bFoundOrientationPivotCommit && OrientationPivotYawThreshold > KINDA_SMALL_NUMBER && FMath::Abs(OrientationBoneDeltaYaw) >= OrientationPivotYawThreshold)
		{
			OrientationPivotCommitTime = SampleTime;
			bFoundOrientationPivotCommit = true;
		}

		if (bIncludeSamples)
		{
			TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
			Sample->SetNumberField(TEXT("sample_index"), SampleIndex);
			Sample->SetNumberField(TEXT("time_seconds"), SampleTime);
			Sample->SetNumberField(TEXT("normalized_time"), Alpha);
			Sample->SetObjectField(TEXT("root_motion_transform"), UeAgentAnimationAssetOps::BuildTransformJson(RootMotionAtTime));
			Sample->SetNumberField(TEXT("root_motion_yaw_degrees"), RootMotionYaw);
			Sample->SetNumberField(TEXT("root_motion_horizontal_distance_cm"), RootMotionHorizontalDistance);
			Sample->SetNumberField(TEXT("root_motion_path_distance_cm"), RootMotionPathDistance);
			Sample->SetObjectField(TEXT("raw_root_transform"), UeAgentAnimationAssetOps::BuildTransformJson(RawRootAtTime));
			Sample->SetObjectField(TEXT("raw_root_delta_transform"), UeAgentAnimationAssetOps::BuildTransformJson(RawRootDelta));
			Sample->SetNumberField(TEXT("raw_root_delta_yaw_degrees"), RawRootDeltaYaw);
			Sample->SetNumberField(TEXT("raw_root_delta_horizontal_distance_cm"), RawRootDeltaHorizontalDistance);
			Sample->SetObjectField(TEXT("orientation_bone_transform"), UeAgentAnimationAssetOps::BuildTransformJson(OrientationBoneAtTime));
			Sample->SetObjectField(TEXT("orientation_bone_delta_transform"), UeAgentAnimationAssetOps::BuildTransformJson(OrientationBoneDelta));
			Sample->SetNumberField(TEXT("orientation_bone_delta_yaw_degrees"), OrientationBoneDeltaYaw);
			Samples.Add(MakeShared<FJsonValueObject>(Sample));
		}

		if (bIncludeSuggestedKeys)
		{
			SuggestedRootMotionYawKeys.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSimpleKeyJson(SampleTime, RootMotionYaw)));
			SuggestedOrientationYawKeys.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSimpleKeyJson(SampleTime, OrientationBoneDeltaYaw)));
			SuggestedDistanceKeys.Add(MakeShared<FJsonValueObject>(UeAgentAnimationAssetOps::BuildSimpleKeyJson(SampleTime, RootMotionPathDistance)));
		}
	}

	const bool bHasMeaningfulRootYaw = FMath::Max(FMath::Abs(TotalYaw), MaxAbsRootMotionYaw) >= MeaningfulYawThresholdDegrees;
	const bool bHasMeaningfulRawRootYaw = MaxAbsRawRootDeltaYaw >= MeaningfulYawThresholdDegrees;
	const bool bHasMeaningfulOrientationBoneYaw = FMath::Max(FMath::Abs(TotalOrientationBoneDeltaYaw), MaxAbsOrientationBoneDeltaYaw) >= MeaningfulYawThresholdDegrees;
	const bool bHasMeaningfulRootTranslation = FMath::Max(TotalHorizontalDistance, MaxRootMotionHorizontalDistance) >= MeaningfulTranslationThresholdCm;
	const bool bHasMeaningfulRawRootTranslation = MaxRawRootDeltaHorizontalDistance >= MeaningfulTranslationThresholdCm;

	FString TurnClassification = TEXT("none");
	FString TurnClassificationSource = TEXT("none");
	double ClassificationYaw = 0.0;
	if (bHasMeaningfulRootYaw)
	{
		ClassificationYaw = FMath::Abs(TotalYaw) >= MeaningfulYawThresholdDegrees ? TotalYaw : (TotalYaw >= 0.0 ? MaxAbsRootMotionYaw : -MaxAbsRootMotionYaw);
		TurnClassificationSource = TEXT("root_motion");
	}
	else if (bHasMeaningfulOrientationBoneYaw)
	{
		ClassificationYaw = FMath::Abs(TotalOrientationBoneDeltaYaw) >= MeaningfulYawThresholdDegrees ? TotalOrientationBoneDeltaYaw : (TotalOrientationBoneDeltaYaw >= 0.0 ? MaxAbsOrientationBoneDeltaYaw : -MaxAbsOrientationBoneDeltaYaw);
		TurnClassificationSource = TEXT("orientation_bone");
	}
	if (FMath::Abs(ClassificationYaw) >= 135.0)
	{
		TurnClassification = ClassificationYaw >= 0.0 ? TEXT("turn_positive_180") : TEXT("turn_negative_180");
	}
	else if (FMath::Abs(ClassificationYaw) >= 45.0)
	{
		TurnClassification = ClassificationYaw >= 0.0 ? TEXT("turn_positive_90") : TEXT("turn_negative_90");
	}
	else if (FMath::Abs(ClassificationYaw) >= MeaningfulYawThresholdDegrees)
	{
		TurnClassification = ClassificationYaw >= 0.0 ? TEXT("turn_positive_partial") : TEXT("turn_negative_partial");
	}

	const FString SuggestedTurnYawSource = bHasMeaningfulRootYaw ? TEXT("root_motion") : (bHasMeaningfulOrientationBoneYaw ? TEXT("orientation_bone") : TEXT("root_motion"));
	const double SuggestedPivotCommitTime = bHasMeaningfulRootYaw ? PivotCommitTime : (bHasMeaningfulOrientationBoneYaw ? OrientationPivotCommitTime : SequenceLength * 0.5);
	const TArray<TSharedPtr<FJsonValue>>& SuggestedTurnYawKeys = bHasMeaningfulRootYaw ? SuggestedRootMotionYawKeys : (bHasMeaningfulOrientationBoneYaw ? SuggestedOrientationYawKeys : SuggestedRootMotionYawKeys);

	TArray<TSharedPtr<FJsonValue>> SuggestedMarkers;
	{
		TSharedPtr<FJsonObject> StartMarker = MakeShared<FJsonObject>();
		StartMarker->SetStringField(TEXT("marker_name"), TEXT("TurnStart"));
		StartMarker->SetNumberField(TEXT("time_seconds"), 0.0);
		StartMarker->SetStringField(TEXT("notify_track_name"), TEXT("Turn"));
		SuggestedMarkers.Add(MakeShared<FJsonValueObject>(StartMarker));

		TSharedPtr<FJsonObject> PivotMarker = MakeShared<FJsonObject>();
		PivotMarker->SetStringField(TEXT("marker_name"), TEXT("PivotCommit"));
		PivotMarker->SetNumberField(TEXT("time_seconds"), FMath::Clamp(SuggestedPivotCommitTime, 0.0, SequenceLength));
		PivotMarker->SetStringField(TEXT("notify_track_name"), TEXT("Turn"));
		SuggestedMarkers.Add(MakeShared<FJsonValueObject>(PivotMarker));

		TSharedPtr<FJsonObject> EndMarker = MakeShared<FJsonObject>();
		EndMarker->SetStringField(TEXT("marker_name"), TEXT("TurnEnd"));
		EndMarker->SetNumberField(TEXT("time_seconds"), SequenceLength);
		EndMarker->SetStringField(TEXT("notify_track_name"), TEXT("Turn"));
		SuggestedMarkers.Add(MakeShared<FJsonValueObject>(EndMarker));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("root_bone_name"), RootBoneName);
	OutData->SetStringField(TEXT("orientation_bone_name"), OrientationBoneName);
	OutData->SetNumberField(TEXT("orientation_bone_index"), OrientationBoneIndex);
	OutData->SetNumberField(TEXT("sequence_length"), SequenceLength);
	OutData->SetNumberField(TEXT("num_frames"), NumFrames);
	OutData->SetNumberField(TEXT("sample_count"), SampleCount);
	OutData->SetBoolField(TEXT("root_motion_enabled"), UAnimationBlueprintLibrary::IsRootMotionEnabled(Sequence));
	OutData->SetBoolField(TEXT("force_root_lock"), UAnimationBlueprintLibrary::IsRootMotionLockForced(Sequence));
	OutData->SetBoolField(TEXT("has_root_bone_track"), bHasRootBoneTrack);
	OutData->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
	OutData->SetArrayField(TEXT("track_names"), TrackNamesJson);
	OutData->SetObjectField(TEXT("total_root_motion_transform"), UeAgentAnimationAssetOps::BuildTransformJson(TotalRootMotion));
	OutData->SetNumberField(TEXT("total_yaw_degrees"), TotalYaw);
	OutData->SetNumberField(TEXT("total_horizontal_distance_cm"), TotalHorizontalDistance);
	OutData->SetNumberField(TEXT("total_root_motion_path_distance_cm"), RootMotionPathDistance);
	OutData->SetNumberField(TEXT("max_abs_root_motion_yaw_degrees"), MaxAbsRootMotionYaw);
	OutData->SetNumberField(TEXT("max_root_motion_horizontal_distance_cm"), MaxRootMotionHorizontalDistance);
	OutData->SetNumberField(TEXT("max_abs_raw_root_delta_yaw_degrees"), MaxAbsRawRootDeltaYaw);
	OutData->SetNumberField(TEXT("max_raw_root_delta_horizontal_distance_cm"), MaxRawRootDeltaHorizontalDistance);
	OutData->SetNumberField(TEXT("total_orientation_bone_delta_yaw_degrees"), TotalOrientationBoneDeltaYaw);
	OutData->SetNumberField(TEXT("max_abs_orientation_bone_delta_yaw_degrees"), MaxAbsOrientationBoneDeltaYaw);
	OutData->SetBoolField(TEXT("has_meaningful_root_yaw"), bHasMeaningfulRootYaw);
	OutData->SetBoolField(TEXT("has_meaningful_raw_root_yaw"), bHasMeaningfulRawRootYaw);
	OutData->SetBoolField(TEXT("has_meaningful_orientation_bone_yaw"), bHasMeaningfulOrientationBoneYaw);
	OutData->SetBoolField(TEXT("has_meaningful_root_translation"), bHasMeaningfulRootTranslation);
	OutData->SetBoolField(TEXT("has_meaningful_raw_root_translation"), bHasMeaningfulRawRootTranslation);
	OutData->SetStringField(TEXT("turn_classification"), TurnClassification);
	OutData->SetStringField(TEXT("turn_classification_source"), TurnClassificationSource);
	OutData->SetNumberField(TEXT("meaningful_yaw_threshold_degrees"), MeaningfulYawThresholdDegrees);
	OutData->SetNumberField(TEXT("meaningful_translation_threshold_cm"), MeaningfulTranslationThresholdCm);
	OutData->SetArrayField(TEXT("suggested_markers"), SuggestedMarkers);
	if (bIncludeSuggestedKeys)
	{
		OutData->SetStringField(TEXT("suggested_turn_yaw_source"), SuggestedTurnYawSource);
		OutData->SetArrayField(TEXT("suggested_root_motion_yaw_keys"), SuggestedRootMotionYawKeys);
		OutData->SetArrayField(TEXT("suggested_orientation_yaw_keys"), SuggestedOrientationYawKeys);
		OutData->SetArrayField(TEXT("suggested_turn_yaw_keys"), SuggestedTurnYawKeys);
		OutData->SetArrayField(TEXT("suggested_distance_keys"), SuggestedDistanceKeys);
	}
	if (bIncludeSamples)
	{
		OutData->SetArrayField(TEXT("samples"), Samples);
	}
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceSamplePose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
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

	const int32 MaxFrameIndex = FMath::Max(0, Sequence->GetNumberOfSampledKeys() - 1);
	float SequenceLengthFloat = 0.0f;
	int32 NumFrames = 0;
	UAnimationBlueprintLibrary::GetSequenceLength(Sequence, SequenceLengthFloat);
	UAnimationBlueprintLibrary::GetNumFrames(Sequence, NumFrames);
	const double SequenceLength = static_cast<double>(SequenceLengthFloat);
	FFrameRate SamplingFrameRate = Sequence->GetSamplingFrameRate();
	if (!SamplingFrameRate.IsValid() || SamplingFrameRate.AsDecimal() <= KINDA_SMALL_NUMBER)
	{
		SamplingFrameRate = FFrameRate(30, 1);
	}

	FString SpaceText = TEXT("local");
	JsonTryGetString(Ctx.Params, TEXT("space"), SpaceText);
	EAnimPoseSpaces PoseSpace = EAnimPoseSpaces::Local;
	if (!UeAgentAnimationAssetOps::ParseAnimPoseSpace(SpaceText, PoseSpace))
	{
		OutError = TEXT("invalid_pose_space");
		return false;
	}

	FString EvaluationTypeText = TEXT("raw");
	UeAgentAnimationEditorOps::JsonTryGetStringAnyField(Ctx.Params, { TEXT("evaluation_type"), TEXT("eval_type") }, EvaluationTypeText);
	EAnimDataEvalType EvaluationType = EAnimDataEvalType::Raw;
	if (!UeAgentAnimationAssetOps::ParseAnimDataEvalType(EvaluationTypeText, EvaluationType))
	{
		OutError = TEXT("invalid_evaluation_type");
		return false;
	}

	FAnimPoseEvaluationOptions EvaluationOptions;
	EvaluationOptions.EvaluationType = EvaluationType;
	EvaluationOptions.bShouldRetarget = false;
	EvaluationOptions.bExtractRootMotion = false;
	EvaluationOptions.bIncorporateRootMotionIntoPose = true;
	EvaluationOptions.bRetrieveAdditiveAsFullPose = true;
	EvaluationOptions.bEvaluateCurves = false;

	bool bBoolOption = false;
	if (JsonTryGetBool(Ctx.Params, TEXT("should_retarget"), bBoolOption))
	{
		EvaluationOptions.bShouldRetarget = bBoolOption;
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("extract_root_motion"), bBoolOption))
	{
		EvaluationOptions.bExtractRootMotion = bBoolOption;
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("incorporate_root_motion_into_pose"), bBoolOption))
	{
		EvaluationOptions.bIncorporateRootMotionIntoPose = bBoolOption;
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("retrieve_additive_as_full_pose"), bBoolOption))
	{
		EvaluationOptions.bRetrieveAdditiveAsFullPose = bBoolOption;
	}
	if (JsonTryGetBool(Ctx.Params, TEXT("evaluate_curves"), bBoolOption))
	{
		EvaluationOptions.bEvaluateCurves = bBoolOption;
	}

	FString PreviewMeshPath;
	UeAgentAnimationEditorOps::JsonTryGetStringAnyField(Ctx.Params, { TEXT("skeletal_mesh_path"), TEXT("preview_skeletal_mesh") }, PreviewMeshPath);
	USkeletalMesh* PreviewMesh = nullptr;
	if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		PreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
		if (Sequence->GetSkeleton() && PreviewMesh->GetSkeleton() && !Sequence->GetSkeleton()->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
		{
			OutError = TEXT("preview_skeletal_mesh_not_compatible_with_sequence_skeleton");
			return false;
		}
		EvaluationOptions.OptionalSkeletalMesh = PreviewMesh;
	}

	struct FPoseSampleRequest
	{
		bool bUseFrameIndex = false;
		int32 FrameIndex = 0;
		double TimeSeconds = 0.0;
	};

	TArray<FPoseSampleRequest> SampleRequests;
	auto AddFrameRequest = [&](const int32 RequestedFrameIndex) -> bool
	{
		if (RequestedFrameIndex < 0 || RequestedFrameIndex > MaxFrameIndex)
		{
			OutError = TEXT("pose_sample_frame_out_of_range");
			return false;
		}
		FPoseSampleRequest Request;
		Request.bUseFrameIndex = true;
		Request.FrameIndex = RequestedFrameIndex;
		Request.TimeSeconds = SamplingFrameRate.AsSeconds(FFrameNumber(RequestedFrameIndex));
		SampleRequests.Add(Request);
		return true;
	};

	auto AddTimeRequest = [&](const double RequestedTimeSeconds) -> bool
	{
		if (RequestedTimeSeconds < 0.0 || RequestedTimeSeconds > SequenceLength + KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("pose_sample_time_out_of_range");
			return false;
		}
		FPoseSampleRequest Request;
		Request.bUseFrameIndex = false;
		Request.TimeSeconds = FMath::Clamp(RequestedTimeSeconds, 0.0, SequenceLength);
		Request.FrameIndex = FMath::Clamp(SamplingFrameRate.AsFrameTime(Request.TimeSeconds).RoundToFrame().Value, 0, MaxFrameIndex);
		SampleRequests.Add(Request);
		return true;
	};

	const TArray<TSharedPtr<FJsonValue>>* FrameValues = nullptr;
	if (UeAgentAnimationEditorOps::JsonTryGetArrayAnyField(Ctx.Params, { TEXT("frame_indices"), TEXT("frames") }, FrameValues) && FrameValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FrameValues)
		{
			double FrameValue = 0.0;
			if (!Value.IsValid() || !Value->TryGetNumber(FrameValue))
			{
				OutError = TEXT("pose_sample_frame_must_be_number");
				return false;
			}
			if (!AddFrameRequest(FMath::RoundToInt(FrameValue)))
			{
				return false;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TimeValues = nullptr;
	if (UeAgentAnimationEditorOps::JsonTryGetArrayAnyField(Ctx.Params, { TEXT("time_seconds_list"), TEXT("times_seconds"), TEXT("times") }, TimeValues) && TimeValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *TimeValues)
		{
			double TimeValue = 0.0;
			if (!Value.IsValid() || !Value->TryGetNumber(TimeValue))
			{
				OutError = TEXT("pose_sample_time_must_be_number");
				return false;
			}
			if (!AddTimeRequest(TimeValue))
			{
				return false;
			}
		}
	}

	if (SampleRequests.Num() == 0)
	{
		double FrameIndexValue = 0.0;
		if (UeAgentAnimationEditorOps::JsonTryGetNumberAnyField(Ctx.Params, { TEXT("frame_index"), TEXT("frame") }, FrameIndexValue))
		{
			if (!AddFrameRequest(FMath::RoundToInt(FrameIndexValue)))
			{
				return false;
			}
		}
		else
		{
			double TimeValue = 0.0;
			if (UeAgentAnimationEditorOps::JsonTryGetNumberAnyField(Ctx.Params, { TEXT("time_seconds"), TEXT("time") }, TimeValue))
			{
				if (!AddTimeRequest(TimeValue))
				{
					return false;
				}
			}
			else if (!AddFrameRequest(0))
			{
				return false;
			}
		}
	}

	TArray<FName> RequestedBoneNames;
	const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
	if (UeAgentAnimationEditorOps::JsonTryGetArrayAnyField(Ctx.Params, { TEXT("bone_names"), TEXT("bones"), TEXT("sample_bones") }, BoneValues) && BoneValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *BoneValues)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			const FString BoneNameText = Value->AsString().TrimStartAndEnd();
			if (!BoneNameText.IsEmpty())
			{
				RequestedBoneNames.Add(FName(*BoneNameText));
			}
		}
	}
	else
	{
		FString BoneNameText;
		if (UeAgentAnimationEditorOps::JsonTryGetStringAnyField(Ctx.Params, { TEXT("bone_name"), TEXT("bone") }, BoneNameText) && !BoneNameText.TrimStartAndEnd().IsEmpty())
		{
			RequestedBoneNames.Add(FName(*BoneNameText.TrimStartAndEnd()));
		}
	}

	bool bIncludeRefPose = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_ref_pose"), bIncludeRefPose);

	double MaxBonesValue = 512.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_bones"), MaxBonesValue);
	const int32 MaxBones = FMath::Clamp(FMath::RoundToInt(MaxBonesValue), 1, 4096);

	TArray<TSharedPtr<FJsonValue>> Samples;
	TArray<TSharedPtr<FJsonValue>> MissingBonesSummary;
	TArray<FString> MissingBoneNamesSeen;
	bool bTruncatedBones = false;

	for (int32 SampleIndex = 0; SampleIndex < SampleRequests.Num(); ++SampleIndex)
	{
		const FPoseSampleRequest& Request = SampleRequests[SampleIndex];

		FAnimPose Pose;
		if (Request.bUseFrameIndex)
		{
			UAnimPoseExtensions::GetAnimPoseAtFrame(Sequence, Request.FrameIndex, EvaluationOptions, Pose);
		}
		else
		{
			UAnimPoseExtensions::GetAnimPoseAtTime(Sequence, Request.TimeSeconds, EvaluationOptions, Pose);
		}

		if (!UAnimPoseExtensions::IsValid(Pose))
		{
			OutError = TEXT("anim_pose_evaluation_failed");
			return false;
		}

		TArray<FName> PoseBoneNames;
		UAnimPoseExtensions::GetBoneNames(Pose, PoseBoneNames);

		TArray<FName> OutputBoneNames;
		if (RequestedBoneNames.Num() > 0)
		{
			OutputBoneNames = RequestedBoneNames;
		}
		else
		{
			const int32 OutputCount = FMath::Min(PoseBoneNames.Num(), MaxBones);
			OutputBoneNames.Reserve(OutputCount);
			for (int32 BoneIndex = 0; BoneIndex < OutputCount; ++BoneIndex)
			{
				OutputBoneNames.Add(PoseBoneNames[BoneIndex]);
			}
			bTruncatedBones = PoseBoneNames.Num() > OutputCount;
		}

		TArray<TSharedPtr<FJsonValue>> BonesJson;
		TArray<TSharedPtr<FJsonValue>> MissingBonesForSample;
		for (const FName BoneName : OutputBoneNames)
		{
			const int32 PoseBoneIndex = PoseBoneNames.IndexOfByKey(BoneName);
			if (PoseBoneIndex == INDEX_NONE)
			{
				const FString BoneNameText = BoneName.ToString();
				MissingBonesForSample.Add(MakeShared<FJsonValueString>(BoneNameText));
				if (!MissingBoneNamesSeen.Contains(BoneNameText))
				{
					MissingBoneNamesSeen.Add(BoneNameText);
					MissingBonesSummary.Add(MakeShared<FJsonValueString>(BoneNameText));
				}
				continue;
			}

			const FTransform& BoneTransform = UAnimPoseExtensions::GetBonePose(Pose, BoneName, PoseSpace);
			TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
			BoneObj->SetStringField(TEXT("bone_name"), BoneName.ToString());
			BoneObj->SetNumberField(TEXT("bone_index"), PoseBoneIndex);
			BoneObj->SetObjectField(TEXT("transform"), UeAgentAnimationAssetOps::BuildTransformJsonWithQuaternion(BoneTransform));
			if (bIncludeRefPose)
			{
				const FTransform& RefBoneTransform = UAnimPoseExtensions::GetRefBonePose(Pose, BoneName, PoseSpace);
				BoneObj->SetObjectField(TEXT("ref_transform"), UeAgentAnimationAssetOps::BuildTransformJsonWithQuaternion(RefBoneTransform));
			}
			BonesJson.Add(MakeShared<FJsonValueObject>(BoneObj));
		}

		TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
		SampleObj->SetNumberField(TEXT("sample_index"), SampleIndex);
		SampleObj->SetNumberField(TEXT("frame_index"), Request.FrameIndex);
		SampleObj->SetNumberField(TEXT("time_seconds"), Request.TimeSeconds);
		SampleObj->SetNumberField(TEXT("normalized_time"), SequenceLength > KINDA_SMALL_NUMBER ? Request.TimeSeconds / SequenceLength : 0.0);
		SampleObj->SetBoolField(TEXT("used_frame_index"), Request.bUseFrameIndex);
		SampleObj->SetNumberField(TEXT("bone_count"), BonesJson.Num());
		SampleObj->SetArrayField(TEXT("bones"), BonesJson);
		SampleObj->SetArrayField(TEXT("missing_bones"), MissingBonesForSample);
		Samples.Add(MakeShared<FJsonValueObject>(SampleObj));
	}

	TSharedPtr<FJsonObject> FrameRateJson = MakeShared<FJsonObject>();
	FrameRateJson->SetNumberField(TEXT("numerator"), SamplingFrameRate.Numerator);
	FrameRateJson->SetNumberField(TEXT("denominator"), SamplingFrameRate.Denominator);
	FrameRateJson->SetNumberField(TEXT("decimal"), SamplingFrameRate.AsDecimal());

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("skeleton"), Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh ? PreviewMesh->GetPathName() : (Sequence->GetPreviewMesh() ? Sequence->GetPreviewMesh()->GetPathName() : TEXT("")));
	OutData->SetStringField(TEXT("space"), UeAgentAnimationAssetOps::AnimPoseSpaceToString(PoseSpace));
	OutData->SetStringField(TEXT("evaluation_type"), UeAgentAnimationAssetOps::AnimDataEvalTypeToString(EvaluationType));
	OutData->SetBoolField(TEXT("should_retarget"), EvaluationOptions.bShouldRetarget);
	OutData->SetBoolField(TEXT("extract_root_motion"), EvaluationOptions.bExtractRootMotion);
	OutData->SetBoolField(TEXT("evaluate_curves"), EvaluationOptions.bEvaluateCurves);
	OutData->SetNumberField(TEXT("sequence_length"), SequenceLength);
	OutData->SetNumberField(TEXT("num_frames"), NumFrames);
	OutData->SetNumberField(TEXT("max_frame_index"), MaxFrameIndex);
	OutData->SetObjectField(TEXT("sampling_frame_rate"), FrameRateJson);
	OutData->SetNumberField(TEXT("sample_count"), Samples.Num());
	OutData->SetBoolField(TEXT("requested_all_bones"), RequestedBoneNames.Num() == 0);
	OutData->SetNumberField(TEXT("requested_bone_count"), RequestedBoneNames.Num());
	OutData->SetNumberField(TEXT("max_bones"), MaxBones);
	OutData->SetBoolField(TEXT("truncated_bones"), bTruncatedBones);
	OutData->SetArrayField(TEXT("missing_bones"), MissingBonesSummary);
	OutData->SetArrayField(TEXT("samples"), Samples);
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

	UeAgentEditorViewportSequenceFrameOptions::FUnifiedSequenceFrameOptionsReport UnifiedOptionsReport;
	UeAgentEditorViewportSequenceFrameOptions::ApplyUnifiedSequenceFrameOptionsToParams(
		Ctx.Params,
		UeAgentEditorViewportSequenceFrameOptions::ESequenceFrameCommandKind::AnimSequence,
		UnifiedOptionsReport);

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

	TArray<UeAgentAnimationEditorOps::FPreviewFrameRequest> FrameRequests;
	if (!UeAgentAnimationEditorOps::ResolvePreviewFrameSequence(Sequence, Ctx.Params, FrameRequests, OutError))
	{
		return false;
	}
	if (FrameRequests.Num() <= 0)
	{
		OutError = TEXT("preview_frame_sequence_empty");
		return false;
	}
	const int32 FrameIndex = FrameRequests[0].FrameIndex;
	float TimeSeconds = FrameRequests[0].TimeSeconds;

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
	const FString CaptureMode = TEXT("scene_capture_temp_actor");
	TArray<TSharedPtr<FJsonValue>> CapturedFrames;
	TSharedPtr<FJsonObject> FirstFrameObject;
	TArray<TArray<FColor>> CapturedFramePixelsForSheet;
	TArray<FString> CapturedFrameLabelsForSheet;
	FIntPoint FrameSheetTileSize(0, 0);

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

	auto CaptureRequestedFrame = [&](const UeAgentAnimationEditorOps::FPreviewFrameRequest& Request, const int32 SequenceIndex, TSharedPtr<FJsonObject>& OutFrameObject) -> bool
	{
		PreviewMeshComponent->bPauseAnims = false;
		PreviewMeshComponent->SetComponentTickEnabled(true);
		PreviewMeshComponent->SetPosition(Request.TimeSeconds, false);
		PreviewMeshComponent->TickAnimation(0.0f, false);
		PreviewMeshComponent->RefreshBoneTransforms(nullptr);
		PreviewMeshComponent->RefreshFollowerComponents();
		Cleanup.PreviewActor->SetActorLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
		PreviewMeshComponent->SetRelativeLocation(FVector::ZeroVector);
		PreviewMeshComponent->SetRelativeRotation(FRotator::ZeroRotator);
		PreviewMeshComponent->UpdateComponentToWorld();
		World->Tick(LEVELTICK_All, TickDeltaSeconds);
		PreviewMeshComponent->bPauseAnims = true;
		PreviewMeshComponent->SetComponentTickEnabled(false);
		PreviewMeshComponent->MarkRenderStateDirty();
		FlushRenderingCommands();

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
	const int32 MaxFrameIndex = FMath::Max(0, Sequence->GetNumberOfSampledKeys() - 1);

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

	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
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
	OutData->SetStringField(TEXT("target"), Target);
	OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetNumberField(TEXT("frame_index"), FrameIndex);
	OutData->SetNumberField(TEXT("time_seconds"), TimeSeconds);
	OutData->SetNumberField(TEXT("num_frames"), MaxFrameIndex);
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
	OutData->SetNumberField(TEXT("sampling_frame_rate_numerator"), SamplingFrameRate.Numerator);
	OutData->SetNumberField(TEXT("sampling_frame_rate_denominator"), SamplingFrameRate.Denominator);
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), EffectivePreviewMeshPath);
	UeAgentEditorViewportSequenceFrameOptions::AttachUnifiedSequenceFrameOptionsReport(OutData, UnifiedOptionsReport);
	return true;
}

bool FUeAgentHttpServer::CmdAnimSequenceApplyBoneTracks(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* ExistingObject = UeAgentAnimationAssetOps::LoadAssetObject(AssetPath);
	UAnimSequence* Sequence = Cast<UAnimSequence>(ExistingObject);
	if (ExistingObject && !Sequence)
	{
		OutError = TEXT("asset_exists_with_different_class");
		return false;
	}

	FString SkeletonPath;
	UeAgentAnimationEditorOps::JsonTryGetStringAnyField(Ctx.Params, { TEXT("target_skeleton"), TEXT("skeleton") }, SkeletonPath);
	USkeleton* RequestedSkeleton = nullptr;
	if (!SkeletonPath.TrimStartAndEnd().IsEmpty())
	{
		RequestedSkeleton = UeAgentAnimationAssetOps::LoadSkeleton(SkeletonPath);
		if (!RequestedSkeleton)
		{
			OutError = TEXT("target_skeleton_not_found");
			return false;
		}
	}

	FString PreviewMeshPath;
	UeAgentAnimationEditorOps::JsonTryGetStringAnyField(Ctx.Params, { TEXT("preview_skeletal_mesh"), TEXT("skeletal_mesh_path") }, PreviewMeshPath);
	USkeletalMesh* PreviewMesh = nullptr;
	if (!PreviewMeshPath.TrimStartAndEnd().IsEmpty())
	{
		PreviewMesh = UeAgentAnimationAssetOps::LoadSkeletalMesh(PreviewMeshPath);
		if (!PreviewMesh)
		{
			OutError = TEXT("preview_skeletal_mesh_not_found");
			return false;
		}
	}

	USkeleton* TargetSkeleton = RequestedSkeleton;
	if (!TargetSkeleton && Sequence)
	{
		TargetSkeleton = Sequence->GetSkeleton();
	}
	if (!TargetSkeleton && PreviewMesh)
	{
		TargetSkeleton = PreviewMesh->GetSkeleton();
	}
	if (Sequence && Sequence->GetSkeleton() && RequestedSkeleton && !Sequence->GetSkeleton()->IsCompatibleForEditor(RequestedSkeleton))
	{
		OutError = TEXT("target_skeleton_not_compatible_with_existing_sequence");
		return false;
	}
	if (TargetSkeleton && PreviewMesh && PreviewMesh->GetSkeleton() && !TargetSkeleton->IsCompatibleForEditor(PreviewMesh->GetSkeleton()))
	{
		OutError = TEXT("preview_skeletal_mesh_not_compatible_with_target_skeleton");
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	bool bCreated = false;
	if (!Sequence)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("anim_sequence_not_found");
			return false;
		}
		if (!TargetSkeleton)
		{
			OutError = TEXT("target_skeleton_required_for_new_anim_sequence");
			return false;
		}
		FString CreateError;
		Sequence = UeAgentAnimationAssetOps::CreateAnimSequenceAsset(AssetPath, TargetSkeleton, PreviewMesh, CreateError);
		if (!Sequence)
		{
			OutError = CreateError.IsEmpty() ? TEXT("create_anim_sequence_failed") : CreateError;
			return false;
		}
		bCreated = true;
	}
	else
	{
		if (RequestedSkeleton && !Sequence->GetSkeleton())
		{
			Sequence->SetSkeleton(RequestedSkeleton);
		}
		if (PreviewMesh)
		{
			Sequence->SetPreviewMesh(PreviewMesh, true);
		}
	}

	if (!Sequence->GetSkeleton())
	{
		OutError = TEXT("anim_sequence_missing_skeleton");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
	if (!UeAgentAnimationEditorOps::JsonTryGetArrayAnyField(Ctx.Params, { TEXT("tracks"), TEXT("bone_tracks") }, TracksArray) || !TracksArray || TracksArray->Num() == 0)
	{
		OutError = TEXT("missing_bone_tracks");
		return false;
	}

	IAnimationDataModel* DataModel = Sequence->GetDataModel();
	if (!DataModel)
	{
		OutError = TEXT("anim_sequence_data_model_not_found");
		return false;
	}

	FFrameRate FallbackFrameRate = Sequence->GetSamplingFrameRate();
	if (!FallbackFrameRate.IsValid() || FallbackFrameRate.AsDecimal() <= KINDA_SMALL_NUMBER)
	{
		FallbackFrameRate = FFrameRate(30, 1);
	}
	if (DataModel->GetFrameRate().IsValid() && DataModel->GetFrameRate().AsDecimal() > KINDA_SMALL_NUMBER)
	{
		FallbackFrameRate = DataModel->GetFrameRate();
	}

	FFrameRate FrameRate;
	if (!UeAgentAnimationEditorOps::ResolveAnimFrameRate(Ctx.Params, FallbackFrameRate, FrameRate, OutError))
	{
		return false;
	}

	int32 NumFrames = FMath::Max(1, DataModel->GetNumberOfFrames());
	double NumFramesValue = 0.0;
	if (JsonTryGetNumber(Ctx.Params, TEXT("num_frames"), NumFramesValue))
	{
		NumFrames = FMath::RoundToInt(NumFramesValue);
	}
	else if (JsonTryGetNumber(Ctx.Params, TEXT("frame_count"), NumFramesValue))
	{
		NumFrames = FMath::RoundToInt(NumFramesValue);
	}
	if (NumFrames <= 0)
	{
		OutError = TEXT("invalid_num_frames");
		return false;
	}
	const int32 RequiredKeyCount = NumFrames + 1;

	const FReferenceSkeleton& RefSkeleton = Sequence->GetSkeleton()->GetReferenceSkeleton();
	if (RefSkeleton.GetNum() <= 0)
	{
		OutError = TEXT("target_skeleton_has_no_bones");
		return false;
	}

	TArray<UeAgentAnimationEditorOps::FAnimSequenceBoneTrackInput> ParsedTracks;
	ParsedTracks.Reserve(TracksArray->Num());
	for (const TSharedPtr<FJsonValue>& TrackValue : *TracksArray)
	{
		if (!TrackValue.IsValid() || TrackValue->Type != EJson::Object)
		{
			OutError = TEXT("bone_track_must_be_object");
			return false;
		}
		UeAgentAnimationEditorOps::FAnimSequenceBoneTrackInput ParsedTrack;
		if (!UeAgentAnimationEditorOps::ParseAnimSequenceBoneTrackInput(TrackValue->AsObject(), RefSkeleton, RequiredKeyCount, FrameRate, ParsedTrack, OutError))
		{
			return false;
		}
		ParsedTracks.Add(MoveTemp(ParsedTrack));
	}

	bool bReplaceExistingTracks = false;
	JsonTryGetBool(Ctx.Params, TEXT("replace_existing_tracks"), bReplaceExistingTracks);

	Sequence->Modify();
	IAnimationDataController& Controller = Sequence->GetController();
	{
		IAnimationDataController::FScopedBracket ScopedBracket(Controller, FText::FromString(TEXT("UAI Apply AnimSequence Bone Tracks")), false);
		Controller.SetFrameRate(FrameRate, false);
		Controller.SetNumberOfFrames(FFrameNumber(NumFrames), false);
		if (bReplaceExistingTracks)
		{
			Controller.RemoveAllBoneTracks(false);
		}

		for (const UeAgentAnimationEditorOps::FAnimSequenceBoneTrackInput& ParsedTrack : ParsedTracks)
		{
			TArray<FName> CurrentTrackNames;
			DataModel->GetBoneTrackNames(CurrentTrackNames);
			if (!CurrentTrackNames.Contains(ParsedTrack.BoneName))
			{
				Controller.AddBoneCurve(ParsedTrack.BoneName, false);
				CurrentTrackNames.Reset();
				DataModel->GetBoneTrackNames(CurrentTrackNames);
				if (!CurrentTrackNames.Contains(ParsedTrack.BoneName))
				{
					OutError = FString::Printf(TEXT("add_bone_track_failed:%s"), *ParsedTrack.BoneName.ToString());
					return false;
				}
			}
			if (!Controller.SetBoneTrackKeys(ParsedTrack.BoneName, ParsedTrack.Positions, ParsedTrack.Rotations, ParsedTrack.Scales, false))
			{
				OutError = FString::Printf(TEXT("set_bone_track_keys_failed:%s"), *ParsedTrack.BoneName.ToString());
				return false;
			}
		}
		Controller.NotifyPopulated();
	}

	Sequence->RefreshCacheData();
	Sequence->MarkPackageDirty();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentAnimationAssetOps::SaveAssetPackage(Sequence, OutError))
	{
		return false;
	}

	TArray<FName> TrackNames;
	DataModel->GetBoneTrackNames(TrackNames);
	TArray<TSharedPtr<FJsonValue>> TrackNamesJson;
	for (const FName TrackName : TrackNames)
	{
		TrackNamesJson.Add(MakeShared<FJsonValueString>(TrackName.ToString()));
	}

	TArray<TSharedPtr<FJsonValue>> AppliedTracksJson;
	for (const UeAgentAnimationEditorOps::FAnimSequenceBoneTrackInput& ParsedTrack : ParsedTracks)
	{
		TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
		TrackObj->SetStringField(TEXT("bone"), ParsedTrack.BoneName.ToString());
		TrackObj->SetNumberField(TEXT("provided_key_count"), ParsedTrack.ProvidedKeyCount);
		TrackObj->SetNumberField(TEXT("written_key_count"), ParsedTrack.Positions.Num());
		TrackObj->SetBoolField(TEXT("used_explicit_frames"), ParsedTrack.bUsedExplicitFrames);
		AppliedTracksJson.Add(MakeShared<FJsonValueObject>(TrackObj));
	}

	TSharedPtr<FJsonObject> FrameRateJson = MakeShared<FJsonObject>();
	FrameRateJson->SetNumberField(TEXT("numerator"), FrameRate.Numerator);
	FrameRateJson->SetNumberField(TEXT("denominator"), FrameRate.Denominator);
	FrameRateJson->SetNumberField(TEXT("decimal"), FrameRate.AsDecimal());

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), UeAgentAnimationAssetOps::NormalizeAssetPath(Sequence->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), Sequence->GetPathName());
	OutData->SetStringField(TEXT("skeleton"), Sequence->GetSkeleton() ? Sequence->GetSkeleton()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("preview_skeletal_mesh"), Sequence->GetPreviewMesh() ? Sequence->GetPreviewMesh()->GetPathName() : TEXT(""));
	OutData->SetObjectField(TEXT("frame_rate"), FrameRateJson);
	OutData->SetNumberField(TEXT("num_frames"), NumFrames);
	OutData->SetNumberField(TEXT("sample_key_count"), RequiredKeyCount);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("replace_existing_tracks"), bReplaceExistingTracks);
	OutData->SetNumberField(TEXT("applied_track_count"), ParsedTracks.Num());
	OutData->SetArrayField(TEXT("applied_tracks"), AppliedTracksJson);
	OutData->SetNumberField(TEXT("track_count"), TrackNamesJson.Num());
	OutData->SetArrayField(TEXT("track_names"), TrackNamesJson);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return true;
}
