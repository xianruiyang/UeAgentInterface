// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"
#include "Domains/Animation/UeAgentAnimationEditorDomainBackend.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
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

	bool SaveAuthoringFile(const FString& FilePathInput, const TSharedPtr<FJsonObject>& Obj, FString& OutResolvedPath, FString& OutError)
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

	bool ApplyBlendParameterFromJson(UBlendSpace* BlendSpace, const int32 AxisIndex, const TSharedPtr<FJsonObject>& AxisObj, TArray<TSharedPtr<FJsonValue>>& Issues)
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

	bool ResolveAnimFrameRate(const TSharedPtr<FJsonObject>& Params, const FFrameRate& Fallback, FFrameRate& OutFrameRate, FString& OutError)
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

	bool ParseAnimSequenceBoneTrackInput(
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

	bool ResolvePreviewFrameSequence(UAnimSequence* Sequence, const TSharedPtr<FJsonObject>& Params, TArray<FPreviewFrameRequest>& OutFrames, FString& OutError)
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
