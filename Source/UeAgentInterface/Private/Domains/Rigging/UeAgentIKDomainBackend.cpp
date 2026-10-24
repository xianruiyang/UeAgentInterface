// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/UeAgentIKDomainBackend.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Utilities/UeAgentAuthoringJson.h"
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
#include "Engine/DirectionalLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
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
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentIKAssetOps
{
	namespace
	{
		const TCHAR* IKDiagnosticSourceKind(const UE::UAI::Contract::FSourceLocation& Source)
		{
			return Source.SourceFile.IsEmpty() && Source.JsonPath.IsEmpty() && Source.WorkspaceSource.IsEmpty()
				&& Source.SourceUnit.IsEmpty() && Source.InternalJsonPath.IsEmpty() ? TEXT("native_result") : TEXT("related_input");
		}
	}

	bool ApplyRetargetRoot(UIKRigController& Controller, const FName RequestedRoot,
		UE::UAI::Contract::FDiagnostic& OutError, const UE::UAI::Contract::FSourceLocation& Source, const FString& TargetPath)
	{
		if (!Controller.SetRetargetRoot(RequestedRoot))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_rig_retarget_root_apply_failed"),
				FString::Printf(TEXT("The IK Rig controller rejected retarget root bone '%s'."), *RequestedRoot.ToString()), TEXT("native_apply"));
			OutError.TargetPath = TargetPath;
			OutError.PropertyPath = TEXT("retarget_root");
			OutError.Source = Source;
			OutError.SourceKind = IKDiagnosticSourceKind(Source);
			OutError.ActualValue = MakeShared<FJsonValueString>(RequestedRoot.ToString());
			return false;
		}
		const FName ActualRoot = Controller.GetRetargetRoot();
		if (ActualRoot != RequestedRoot)
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("ik_rig_retarget_root_readback_mismatch");
			Diagnostic.Stage = TEXT("native_apply");
			Diagnostic.PropertyPath = TEXT("retarget_root");
			Diagnostic.Source = Source;
			Diagnostic.TargetPath = TargetPath;
			Diagnostic.SourceKind = IKDiagnosticSourceKind(Source);
			Diagnostic.PublicMessage = TEXT("The native retarget root does not match the requested bone.");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(RequestedRoot.ToString());
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(ActualRoot.ToString());
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Diagnostic);
		}
		return true;
	}

	bool ApplyRetargetChainGoal(UIKRigController& Controller, const FName ChainName,
		const FName RequestedGoal, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FSourceLocation& Source, const FString& TargetPath)
	{
		if (!Controller.SetRetargetChainGoal(ChainName, RequestedGoal))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_rig_chain_goal_apply_failed"),
				FString::Printf(TEXT("The IK Rig controller rejected goal '%s' for retarget chain '%s'."),
					*RequestedGoal.ToString(), *ChainName.ToString()), TEXT("native_apply"));
			OutError.TargetPath = TargetPath;
			OutError.PropertyPath = TEXT("goal");
			OutError.Source = Source;
			OutError.SourceKind = IKDiagnosticSourceKind(Source);
			OutError.ActualValue = MakeShared<FJsonValueString>(RequestedGoal.ToString());
			return false;
		}
		const FName ActualGoal = Controller.GetRetargetChainGoal(ChainName);
		if (ActualGoal != RequestedGoal)
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("ik_rig_chain_goal_readback_mismatch");
			Diagnostic.Stage = TEXT("native_apply");
			Diagnostic.PropertyPath = TEXT("goal");
			Diagnostic.Source = Source;
			Diagnostic.TargetPath = TargetPath;
			Diagnostic.SourceKind = IKDiagnosticSourceKind(Source);
			Diagnostic.PublicMessage = FString::Printf(TEXT("The native goal for retarget chain '%s' does not match the requested goal."), *ChainName.ToString());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(RequestedGoal.ToString());
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(ActualGoal.ToString());
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Diagnostic);
		}
		return true;
	}

	bool ApplySolverBone(UIKRigController& Controller, const int32 SolverIndex,
		const FName RequestedBone, const bool bEndBone, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FIKRigSolverBase* Solver = Controller.GetSolverAtIndex(SolverIndex);
		const FString Field = bEndBone ? TEXT("end_bone") : TEXT("start_bone");
		if (!Solver)
		{
			auto Actual = MakeShared<FJsonObject>();
            Actual->SetNumberField(TEXT("solver_index"), SolverIndex);
            Actual->SetBoolField(TEXT("solver_available"), false);
            OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("ik_rig_solver_unavailable"), Field,
                MakeShared<FJsonValueString>(TEXT("available native solver")), MakeShared<FJsonValueObject>(Actual),
                TEXT("native_apply"), TEXT("The native IK Rig solver is unavailable for the requested bone edit."));
            OutError.SourceKind = TEXT("native_result");
            OutError.Source = {};
			return false;
		}
		const auto Read = [&]() { return bEndBone
			? Controller.GetEndBone(SolverIndex) : Controller.GetStartBone(SolverIndex); };
		if (RequestedBone.IsNone() && Read().IsNone()) return true;
		bool bApplied = true;
		if (RequestedBone.IsNone())
		{
			// Controller bone picking requires a skeleton bone. Clearing is a native
			// solver operation and has no bone to pick; use the same virtual owner.
			Controller.GetAsset()->Modify();
			if (bEndBone) Solver->SetEndBone(NAME_None);
			else Solver->SetStartBone(NAME_None);
			Controller.BroadcastNeedsReinitialized();
		}
		else
		{
			bApplied = bEndBone ? Controller.SetEndBone(RequestedBone, SolverIndex)
				: Controller.SetStartBone(RequestedBone, SolverIndex);
		}
		const FName ActualBone = Read();
		if (!bApplied || ActualBone != RequestedBone)
		{
			UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
			Diagnostic.Code = bApplied ? TEXT("ik_rig_solver_bone_readback_mismatch")
				: TEXT("ik_rig_solver_bone_apply_failed");
			Diagnostic.Stage = TEXT("native_apply");
			Diagnostic.PropertyPath = Field;
			Diagnostic.SourceKind = TEXT("native_result");
            Diagnostic.PublicMessage = TEXT("The native solver bone does not match the requested bone.");
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(RequestedBone.ToString());
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(ActualBone.ToString());
			if (!bApplied) { OutError = Diagnostic; return false; }
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Diagnostic);
		}
		return true;
	}

	namespace
	{
		bool RetargetValueFailure(const TCHAR* Code, const FString& Reason, const FString& NativePath,
			const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual,
			const UE::UAI::Contract::FSourceLocation& Source, const FString& TargetPath,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_apply"));
			OutError.PropertyPath = NativePath;
			OutError.Source = Source;
			OutError.SourceKind = IKDiagnosticSourceKind(Source);
			OutError.TargetPath = TargetPath;
			OutError.ExpectedValue = Expected;
			OutError.ActualValue = Actual;
			return false;
		}

		FString RetargetPointerToken(FString Value)
		{
			Value.ReplaceInline(TEXT("~"), TEXT("~0"));
			Value.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Value;
		}

		FString RetargetPosePath(const ERetargetSourceOrTarget Side, const FName PoseName)
		{
			return FString(TEXT("/poses/")) + SourceOrTargetToString(Side)
				+ TEXT("/poses/") + RetargetPointerToken(PoseName.ToString());
		}

		template<typename T>
		TSharedPtr<FJsonValue> EncodeRetargetValue(const T& Value)
		{
			return UeAgentPropertyValueCodec::EncodeStructValue(
				TBaseStructure<T>::Get(), &Value, nullptr, UeAgentPropertyValueCodec::FOptions());
		}
	}

	bool VerifyRetargetChainMapping(UIKRetargeterController& Controller,
		const FName SourceChain, const FName TargetChain, const FName OpName, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetMappingDiagnosticSource& Source)
	{
		bool bFound = false;
		for (int32 Index = 0; Index < Controller.GetNumRetargetOps(); ++Index)
		{
			FIKRetargetOpBase* Op = Controller.GetRetargetOpByIndex(Index);
			if (!Op || (!OpName.IsNone() && Op->GetName() != OpName)) continue;
			const FRetargetChainMapping* Mapping = Op->GetChainMapping();
			if (!Mapping || !Mapping->HasChain(TargetChain, ERetargetSourceOrTarget::Target)) continue;
			bFound = true;
			const FName Actual = Mapping->GetChainMappedTo(TargetChain, ERetargetSourceOrTarget::Target);
			if (Actual != SourceChain)
				return RetargetValueFailure(TEXT("ik_retargeter_mapping_readback_mismatch"),
					FString::Printf(TEXT("Retarget operation '%s' maps target chain '%s' to a different source chain than requested."),
						*Op->GetName().ToString(), *TargetChain.ToString()),
					TEXT("/mappings/") + RetargetPointerToken(Op->GetName().ToString() + TEXT(":")
						+ TargetChain.ToString()) + TEXT("/source_chain"),
					MakeShared<FJsonValueString>(SourceChain.ToString()),
					MakeShared<FJsonValueString>(Actual.ToString()), Source.SourceChain, Source.TargetPath, OutError);
		}
		if (!bFound)
			return RetargetValueFailure(TEXT("ik_retargeter_mapping_target_unavailable"),
				FString::Printf(TEXT("No selected retarget operation provides target chain '%s' for mapping."), *TargetChain.ToString()),
				TEXT("/mappings/") + RetargetPointerToken(OpName.ToString() + TEXT(":")
					+ TargetChain.ToString()), MakeShared<FJsonValueString>(TargetChain.ToString()),
				MakeShared<FJsonValueNull>(), Source.TargetChain, Source.TargetPath, OutError);
		return true;
	}

	bool ApplyRetargetChainMapping(UIKRetargeterController& Controller,
		const FName SourceChain, const FName TargetChain, const FName OpName, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetMappingDiagnosticSource& Source)
	{
		// HasChain(Source) searches existing mappings, not the rig's declared
		// chains. A valid newly mapped source must be checked against the rig.
		const UIKRigDefinition* SourceRig = Controller.GetIKRig(ERetargetSourceOrTarget::Source);
		const bool bSourceExists = !SourceRig || SourceChain.IsNone()
			|| SourceRig->GetRetargetChainNames().Contains(SourceChain);
		for (int32 Index = 0; Index < Controller.GetNumRetargetOps(); ++Index)
		{
			FIKRetargetOpBase* Op = Controller.GetRetargetOpByIndex(Index);
			if (!Op || (!OpName.IsNone() && Op->GetName() != OpName)) continue;
			FRetargetChainMapping* Mapping = Op->GetChainMapping();
			if (!Mapping || !Mapping->HasChain(TargetChain, ERetargetSourceOrTarget::Target)) continue;
			// Native cleanup deliberately retains unresolved mappings until both
			// rigs exist. Preserve that configuration lifecycle as well.
			if (!bSourceExists && Mapping->GetIKRig(ERetargetSourceOrTarget::Target))
				return RetargetValueFailure(TEXT("ik_retargeter_mapping_source_unavailable"),
					FString::Printf(TEXT("Source chain '%s' is not declared by the source IK Rig for target chain '%s'."),
						*SourceChain.ToString(), *TargetChain.ToString()),
					TEXT("/mappings/") + RetargetPointerToken(Op->GetName().ToString() + TEXT(":")
						+ TargetChain.ToString()) + TEXT("/source_chain"),
					MakeShared<FJsonValueString>(SourceChain.ToString()), MakeShared<FJsonValueNull>(), Source.SourceChain, Source.TargetPath, OutError);
		}
		if (!Controller.SetSourceChain(SourceChain, TargetChain, OpName))
			return RetargetValueFailure(TEXT("ik_retargeter_mapping_apply_failed"),
				FString::Printf(TEXT("The retargeter controller rejected mapping source chain '%s' to target chain '%s'."),
					*SourceChain.ToString(), *TargetChain.ToString()),
				TEXT("/mappings/") + RetargetPointerToken(OpName.ToString() + TEXT(":")
					+ TargetChain.ToString()), MakeShared<FJsonValueString>(SourceChain.ToString()),
				MakeShared<FJsonValueNull>(), Source.Mapping, Source.TargetPath, OutError);
		return VerifyRetargetChainMapping(Controller, SourceChain, TargetChain, OpName, OutError, Source);
	}

	bool ApplyRetargetPoseRotation(UIKRetargeterController& Controller,
		const ERetargetSourceOrTarget Side, const FName Bone, const FQuat& Rotation, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source)
	{
		const FString Path = RetargetPosePath(Side, Controller.GetCurrentRetargetPoseName(Side))
			+ TEXT("/bone_rotations/") + RetargetPointerToken(Bone.ToString());
		const UIKRigDefinition* Rig = Controller.GetIKRig(Side);
		// The native setter dereferences the rig; CleanPoseList removes unknown bones.
		if (!Rig || !Rig->GetSkeleton().BoneNames.Contains(Bone))
			return RetargetValueFailure(!Rig ? TEXT("ik_retargeter_pose_rig_unavailable")
				: TEXT("ik_retargeter_pose_bone_unavailable"),
				!Rig ? FString::Printf(TEXT("The %s IK Rig is unavailable while applying the pose rotation for bone '%s'."),
					SourceOrTargetToString(Side), *Bone.ToString())
					: FString::Printf(TEXT("Bone '%s' does not exist in the %s IK Rig skeleton."), *Bone.ToString(), SourceOrTargetToString(Side)), Path,
				MakeShared<FJsonValueString>(Bone.ToString()), MakeShared<FJsonValueNull>(),
				Source.BoneRotations.FindRef(Bone), Source.TargetPath, OutError);
		Controller.SetRotationOffsetForRetargetPoseBone(Bone, Rotation, Side);
		const FQuat Actual = Controller.GetRotationOffsetForRetargetPoseBone(Bone, Side);
		if (!Actual.Equals(Rotation, 0.0))
			return RetargetValueFailure(TEXT("ik_retargeter_pose_rotation_readback_mismatch"),
				FString::Printf(TEXT("The %s pose rotation read back for bone '%s' differs from the requested rotation."),
					SourceOrTargetToString(Side), *Bone.ToString()), Path,
				EncodeRetargetValue(Rotation), EncodeRetargetValue(Actual), Source.BoneRotations.FindRef(Bone), Source.TargetPath, OutError);
		return true;
	}

	bool ApplyRetargetPoseRootTranslation(UIKRetargeterController& Controller,
		const ERetargetSourceOrTarget Side, const FVector& Translation, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source)
	{
		const FString Path = RetargetPosePath(Side, Controller.GetCurrentRetargetPoseName(Side))
			+ TEXT("/root_translation");
		if (Translation.X != 0.0 || Translation.Y != 0.0)
			return RetargetValueFailure(TEXT("ik_retargeter_pose_horizontal_root_translation_unsupported"),
				TEXT("Retarget pose root translation supports only the Z component; X and Y must be zero."),
				Path, EncodeRetargetValue(FVector(0.0, 0.0, Translation.Z)), EncodeRetargetValue(Translation),
				Source.RootTranslation, Source.TargetPath, OutError);
		// Native SetRootOffsetInRetargetPose adds a delta; this edit supplies the total.
		Controller.GetCurrentRetargetPose(Side).SetRootTranslationDelta(Translation);
		const FVector Actual = Controller.GetRootOffsetInRetargetPose(Side);
		if (Actual != Translation)
			return RetargetValueFailure(TEXT("ik_retargeter_pose_root_readback_mismatch"),
				FString::Printf(TEXT("The %s retarget pose root translation read back differs from the requested translation."), SourceOrTargetToString(Side)),
				Path, EncodeRetargetValue(Translation), EncodeRetargetValue(Actual), Source.RootTranslation, Source.TargetPath, OutError);
		return true;
	}

	bool VerifyRetargetPose(UIKRetargeterController& Controller,
		const ERetargetSourceOrTarget Side, const FName PoseName, const FVector& RootTranslation,
		const TMap<FName, FQuat>& BoneRotations, UE::UAI::Contract::FDiagnostic& OutError,
		const FRetargetPoseDiagnosticSource& Source)
	{
		const FString Path = RetargetPosePath(Side, PoseName);
		const FIKRetargetPose* Pose = Controller.GetRetargetPoses(Side).Find(PoseName);
		if (!Pose)
			return RetargetValueFailure(TEXT("ik_retargeter_pose_readback_missing"),
				FString::Printf(TEXT("Requested %s retarget pose '%s' is missing after native cleanup."),
					SourceOrTargetToString(Side), *PoseName.ToString()), Path,
				MakeShared<FJsonValueString>(PoseName.ToString()), MakeShared<FJsonValueNull>(), Source.Pose, Source.TargetPath, OutError);
		if (Pose->GetRootTranslationDelta() != RootTranslation)
			return RetargetValueFailure(TEXT("ik_retargeter_pose_root_readback_mismatch"),
				FString::Printf(TEXT("The root translation of %s retarget pose '%s' changed during native cleanup."),
					SourceOrTargetToString(Side), *PoseName.ToString()),
				Path + TEXT("/root_translation"), EncodeRetargetValue(RootTranslation),
				EncodeRetargetValue(Pose->GetRootTranslationDelta()), Source.RootTranslation, Source.TargetPath, OutError);
		const TMap<FName, FQuat>& ActualRotations = Pose->GetAllDeltaRotations();
		for (const auto& Pair : BoneRotations)
		{
			const FQuat* Actual = ActualRotations.Find(Pair.Key);
			if (!Actual || !Actual->Equals(Pair.Value, 0.0))
				return RetargetValueFailure(TEXT("ik_retargeter_pose_rotation_readback_mismatch"),
					FString::Printf(TEXT("Bone '%s' in %s retarget pose '%s' has a missing or changed rotation after native cleanup."),
						*Pair.Key.ToString(), SourceOrTargetToString(Side), *PoseName.ToString()),
					Path + TEXT("/bone_rotations/") + RetargetPointerToken(Pair.Key.ToString()),
					EncodeRetargetValue(Pair.Value), Actual ? EncodeRetargetValue(*Actual)
						: MakeShared<FJsonValueNull>(), Source.BoneRotations.FindRef(Pair.Key), Source.TargetPath, OutError);
		}
		if (ActualRotations.Num() != BoneRotations.Num())
		{
			const auto Unexpected = MakeShared<FJsonObject>();
			for (const auto& Pair : ActualRotations)
				if (!BoneRotations.Contains(Pair.Key)) Unexpected->SetField(Pair.Key.ToString(), EncodeRetargetValue(Pair.Value));
			return RetargetValueFailure(TEXT("ik_retargeter_pose_rotation_count_mismatch"),
				FString::Printf(TEXT("The %s retarget pose '%s' contains unexpected bone rotations after native cleanup (%d entries, expected %d)."),
					SourceOrTargetToString(Side), *PoseName.ToString(), ActualRotations.Num(), BoneRotations.Num()),
				Path + TEXT("/bone_rotations"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()),
				MakeShared<FJsonValueObject>(Unexpected), Source.Rotations, Source.TargetPath, OutError);
		}
		return true;
	}

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

	FString ToObjectPath(const FString& InPath)
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

	UIKRigDefinition* LoadIKRig(const FString& InPath)
	{
		return Cast<UIKRigDefinition>(LoadAssetObject(InPath));
	}

	UIKRetargeter* LoadIKRetargeter(const FString& InPath)
	{
		return Cast<UIKRetargeter>(LoadAssetObject(InPath));
	}

	USkeletalMesh* LoadSkeletalMesh(const FString& InPath)
	{
		return Cast<USkeletalMesh>(LoadAssetObject(InPath));
	}

	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Asset)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_not_found"),
				TEXT("The asset cannot be saved because its native object is unavailable."), TEXT("asset_save"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("asset_available");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_asset_failed"), OutError, TEXT(""));
			return false;
		}
		return true;
	}

	FString SanitizeArtifactSegment(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TCHAR('-') || Ch == TCHAR('_'))
			{
				Out.AppendChar(Ch);
			}
			else
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("probe") : Out;
	}

	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix)
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
		const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
		const FString Hostname = SanitizeArtifactSegment(FPlatformProcess::ComputerName());
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafeRequestId = SanitizeArtifactSegment(RequestId);
		const FString SafeSuffix = SanitizeArtifactSegment(Suffix);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(
			ShotsDir,
			FString::Printf(TEXT("probe_%s_%s_%s_%s_%s.%s"), *Hostname, *Timestamp, *SafeRequestId, *SafeSuffix, *Guid, *Ext));
	}

	TSharedPtr<FJsonObject> BuildLinearColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), Color.R);
		Obj->SetNumberField(TEXT("g"), Color.G);
		Obj->SetNumberField(TEXT("b"), Color.B);
		Obj->SetNumberField(TEXT("a"), Color.A);
		return Obj;
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

	TSharedPtr<FJsonObject> BuildTransformJson(const FTransform& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), BuildVectorJson(Value.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), BuildRotatorJson(Value.Rotator()));
		Obj->SetObjectField(TEXT("scale"), BuildVectorJson(Value.GetScale3D()));
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> TextArrayToJson(const TArray<FText>& Texts)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FText& Text : Texts)
		{
			Values.Add(MakeShared<FJsonValueString>(Text.ToString()));
		}
		return Values;
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	bool ApplyVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		if (!FieldValue)
		{
			return true;
		}

		FVector Decoded = OutValue;
		if (!UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, Decoded, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + Key);
			return false;
		}
		OutValue = Decoded;
		return true;
	}

	bool ApplyRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		if (!FieldValue)
		{
			return true;
		}

		FRotator Decoded = OutValue;
		if (!UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, Decoded, OutError))
		{
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + Key);
			return false;
		}
		OutValue = Decoded;
		return true;
	}

	const TCHAR* SourceOrTargetToString(const ERetargetSourceOrTarget Value)
	{
		return Value == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
	}

	bool ParseSourceOrTarget(const FString& InValue, ERetargetSourceOrTarget& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized == TEXT("source") || Normalized == TEXT("src"))
		{
			OutValue = ERetargetSourceOrTarget::Source;
			return true;
		}
		if (Normalized == TEXT("target") || Normalized == TEXT("dst"))
		{
			OutValue = ERetargetSourceOrTarget::Target;
			return true;
		}
		return false;
	}

	bool ParseAutoMapType(const FString& InValue, EAutoMapChainType& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("exact"))
		{
			OutValue = EAutoMapChainType::Exact;
			return true;
		}
		if (Normalized == TEXT("fuzzy"))
		{
			OutValue = EAutoMapChainType::Fuzzy;
			return true;
		}
		if (Normalized == TEXT("clear"))
		{
			OutValue = EAutoMapChainType::Clear;
			return true;
		}
		return false;
	}

	bool ParseAutoAlignMethod(const FString& InValue, ERetargetAutoAlignMethod& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("chaintochain") || Normalized == TEXT("chain"))
		{
			OutValue = ERetargetAutoAlignMethod::ChainToChain;
			return true;
		}
		if (Normalized == TEXT("meshtomesh") || Normalized == TEXT("mesh"))
		{
			OutValue = ERetargetAutoAlignMethod::MeshToMesh;
			return true;
		}
		if (Normalized == TEXT("localrotationaxes") || Normalized == TEXT("localaxes"))
		{
			OutValue = ERetargetAutoAlignMethod::LocalRotationAxes;
			return true;
		}
		if (Normalized == TEXT("globalrotationaxes") || Normalized == TEXT("globalaxes"))
		{
			OutValue = ERetargetAutoAlignMethod::GlobalRotationAxes;
			return true;
		}
		return false;
	}

	bool ParseIKRigGoalSpace(const FString& InValue, EIKRigGoalSpace& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("additive"))
		{
			OutValue = EIKRigGoalSpace::Additive;
			return true;
		}
		if (Normalized == TEXT("component") || Normalized == TEXT("componentspace"))
		{
			OutValue = EIKRigGoalSpace::Component;
			return true;
		}
		if (Normalized == TEXT("world") || Normalized == TEXT("worldspace"))
		{
			OutValue = EIKRigGoalSpace::World;
			return true;
		}
		return false;
	}

	FString IKRigGoalSpaceToString(const EIKRigGoalSpace Value)
	{
		switch (Value)
		{
		case EIKRigGoalSpace::Component:
			return TEXT("component");
		case EIKRigGoalSpace::World:
			return TEXT("world");
		case EIKRigGoalSpace::Additive:
		default:
			return TEXT("additive");
		}
	}

	FString AutoAlignMethodToString(const ERetargetAutoAlignMethod InValue)
	{
		switch (InValue)
		{
		case ERetargetAutoAlignMethod::MeshToMesh:
			return TEXT("mesh_to_mesh");
		case ERetargetAutoAlignMethod::LocalRotationAxes:
			return TEXT("local_rotation_axes");
		case ERetargetAutoAlignMethod::GlobalRotationAxes:
			return TEXT("global_rotation_axes");
		case ERetargetAutoAlignMethod::ChainToChain:
		default:
			return TEXT("chain_to_chain");
		}
	}

	TSharedPtr<FJsonObject> BuildGoalJson(const UIKRigEffectorGoal* Goal)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Goal ? Goal->GoalName.ToString() : TEXT(""));
		Item->SetStringField(TEXT("bone_name"), Goal ? Goal->BoneName.ToString() : TEXT(""));
		Item->SetNumberField(TEXT("position_alpha"), Goal ? Goal->PositionAlpha : 0.0);
		Item->SetNumberField(TEXT("rotation_alpha"), Goal ? Goal->RotationAlpha : 0.0);
		const FTransform GoalTransform = Goal ? Goal->CurrentTransform : FTransform::Identity;
		Item->SetObjectField(TEXT("current_location"), BuildVectorJson(GoalTransform.GetLocation()));
		Item->SetObjectField(TEXT("current_rotation"), BuildRotatorJson(GoalTransform.Rotator()));
		Item->SetObjectField(TEXT("current_scale"), BuildVectorJson(GoalTransform.GetScale3D()));
		TSharedPtr<FJsonObject> TransformObject = MakeShared<FJsonObject>();
		TransformObject->SetObjectField(TEXT("location"), BuildVectorJson(GoalTransform.GetLocation()));
		TransformObject->SetObjectField(TEXT("rotation"), BuildRotatorJson(GoalTransform.Rotator()));
		TransformObject->SetObjectField(TEXT("scale"), BuildVectorJson(GoalTransform.GetScale3D()));
		Item->SetObjectField(TEXT("current_transform"), TransformObject);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("goal_name"), Goal ? Goal->GoalName.ToString() : TEXT(""), Goal ? Goal->GoalName.ToString() : TEXT("IK Goal"), TEXT("IK Rig/Goals"), TEXT("goal"));
		return Item;
	}

	FString BasicAxisToString(const EBasicAxis Axis)
	{
		switch (Axis)
		{
		case EBasicAxis::X:
			return TEXT("x");
		case EBasicAxis::Y:
			return TEXT("y");
		case EBasicAxis::Z:
			return TEXT("z");
		case EBasicAxis::NegX:
			return TEXT("-x");
		case EBasicAxis::NegY:
			return TEXT("-y");
		case EBasicAxis::NegZ:
			return TEXT("-z");
		default:
			return TEXT("y");
		}
	}

	bool ParseBasicAxis(const FString& InValue, EBasicAxis& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("y") || Normalized == TEXT("+y") || Normalized == TEXT("forward"))
		{
			OutValue = EBasicAxis::Y;
			return true;
		}
		if (Normalized == TEXT("x") || Normalized == TEXT("+x"))
		{
			OutValue = EBasicAxis::X;
			return true;
		}
		if (Normalized == TEXT("z") || Normalized == TEXT("+z"))
		{
			OutValue = EBasicAxis::Z;
			return true;
		}
		if (Normalized == TEXT("-x") || Normalized == TEXT("negx"))
		{
			OutValue = EBasicAxis::NegX;
			return true;
		}
		if (Normalized == TEXT("-y") || Normalized == TEXT("negy"))
		{
			OutValue = EBasicAxis::NegY;
			return true;
		}
		if (Normalized == TEXT("-z") || Normalized == TEXT("negz"))
		{
			OutValue = EBasicAxis::NegZ;
			return true;
		}
		return false;
	}

	FString WarpingDirectionSourceToString(const EWarpingDirectionSource Source)
	{
		switch (Source)
		{
		case EWarpingDirectionSource::Chain:
			return TEXT("chain");
		case EWarpingDirectionSource::RootBone:
			return TEXT("root_bone");
		case EWarpingDirectionSource::Goals:
		default:
			return TEXT("goals");
		}
	}

	bool ParseWarpingDirectionSource(const FString& InValue, EWarpingDirectionSource& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("goals") || Normalized == TEXT("goal"))
		{
			OutValue = EWarpingDirectionSource::Goals;
			return true;
		}
		if (Normalized == TEXT("chain"))
		{
			OutValue = EWarpingDirectionSource::Chain;
			return true;
		}
		if (Normalized == TEXT("rootbone") || Normalized == TEXT("root"))
		{
			OutValue = EWarpingDirectionSource::RootBone;
			return true;
		}
		return false;
	}

	FString RetargetTranslationModeToString(const ERetargetTranslationMode Value)
	{
		switch (Value)
		{
		case ERetargetTranslationMode::GloballyScaled:
			return TEXT("globally_scaled");
		case ERetargetTranslationMode::Absolute:
			return TEXT("absolute");
		case ERetargetTranslationMode::StretchBoneLengthUniformly:
			return TEXT("stretch_bone_length_uniformly");
		case ERetargetTranslationMode::StretchBoneLengthNonUniformly:
			return TEXT("stretch_bone_length_non_uniformly");
		case ERetargetTranslationMode::None:
		default:
			return TEXT("none");
		}
	}

	bool ParseRetargetTranslationMode(const FString& InValue, ERetargetTranslationMode& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("none"))
		{
			OutValue = ERetargetTranslationMode::None;
			return true;
		}
		if (Normalized == TEXT("globallyscaled"))
		{
			OutValue = ERetargetTranslationMode::GloballyScaled;
			return true;
		}
		if (Normalized == TEXT("absolute"))
		{
			OutValue = ERetargetTranslationMode::Absolute;
			return true;
		}
		if (Normalized == TEXT("stretchbonelengthuniformly"))
		{
			OutValue = ERetargetTranslationMode::StretchBoneLengthUniformly;
			return true;
		}
		if (Normalized == TEXT("stretchbonelengthnonuniformly"))
		{
			OutValue = ERetargetTranslationMode::StretchBoneLengthNonUniformly;
			return true;
		}
		return false;
	}

	FString RetargetRotationModeToString(const ERetargetRotationMode Value)
	{
		switch (Value)
		{
		case ERetargetRotationMode::OneToOne:
			return TEXT("one_to_one");
		case ERetargetRotationMode::OneToOneReversed:
			return TEXT("one_to_one_reversed");
		case ERetargetRotationMode::MatchChain:
			return TEXT("match_chain");
		case ERetargetRotationMode::MatchScaledChain:
			return TEXT("match_scaled_chain");
		case ERetargetRotationMode::None:
			return TEXT("none");
		case ERetargetRotationMode::Interpolated:
		default:
			return TEXT("interpolated");
		}
	}

	bool ParseRetargetRotationMode(const FString& InValue, ERetargetRotationMode& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("interpolated"))
		{
			OutValue = ERetargetRotationMode::Interpolated;
			return true;
		}
		if (Normalized == TEXT("onetoone"))
		{
			OutValue = ERetargetRotationMode::OneToOne;
			return true;
		}
		if (Normalized == TEXT("onetoonereversed"))
		{
			OutValue = ERetargetRotationMode::OneToOneReversed;
			return true;
		}
		if (Normalized == TEXT("matchchain"))
		{
			OutValue = ERetargetRotationMode::MatchChain;
			return true;
		}
		if (Normalized == TEXT("matchscaledchain"))
		{
			OutValue = ERetargetRotationMode::MatchScaledChain;
			return true;
		}
		if (Normalized == TEXT("none"))
		{
			OutValue = ERetargetRotationMode::None;
			return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> BuildChainJson(const FBoneChain& Chain)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("chain_name"), Chain.ChainName.ToString());
		Item->SetStringField(TEXT("start_bone_name"), Chain.StartBone.BoneName.ToString());
		Item->SetStringField(TEXT("end_bone_name"), Chain.EndBone.BoneName.ToString());
		Item->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("chain_name"), Chain.ChainName.ToString(), Chain.ChainName.ToString(), TEXT("IK Rig/Retarget Chains"), TEXT("chain"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildChainPairJson(const FRetargetChainPair& Pair)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), Pair.TargetChainName.ToString());
		Item->SetStringField(TEXT("source_chain_name"), Pair.SourceChainName.ToString());
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetPoseJson(const FIKRetargetPose& Pose)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("root_offset"), BuildVectorJson(Pose.GetRootTranslationDelta()));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("pose"), TEXT("current_pose"), TEXT("Retarget Pose"), TEXT("IK Retargeter/Poses"), TEXT("pose"));

		TArray<TSharedPtr<FJsonValue>> BoneRotationOffsets;
		for (const TPair<FName, FQuat>& Pair : Pose.GetAllDeltaRotations())
		{
			TSharedPtr<FJsonObject> BoneObject = MakeShared<FJsonObject>();
			BoneObject->SetStringField(TEXT("bone_name"), Pair.Key.ToString());
			BoneObject->SetObjectField(TEXT("rotation_offset"), BuildRotatorJson(Pair.Value.Rotator()));
			UeAgentAuthoringJson::ApplyAuthoringMetadata(BoneObject, TEXT("bone_name"), Pair.Key.ToString(), Pair.Key.ToString(), TEXT("IK Retargeter/Pose Bone Offsets"), TEXT("rotator"));
			BoneRotationOffsets.Add(MakeShared<FJsonValueObject>(BoneObject));
		}
		Item->SetNumberField(TEXT("bone_rotation_offset_count"), BoneRotationOffsets.Num());
		Item->SetArrayField(TEXT("bone_rotation_offsets"), BoneRotationOffsets);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildBodyMoverSolverSettingsJson(const FIKRigBodyMoverSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("start_bone"), Settings.StartBone.ToString());
		Item->SetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		Item->SetNumberField(TEXT("position_positive_x"), Settings.PositionPositiveX);
		Item->SetNumberField(TEXT("position_negative_x"), Settings.PositionNegativeX);
		Item->SetNumberField(TEXT("position_positive_y"), Settings.PositionPositiveY);
		Item->SetNumberField(TEXT("position_negative_y"), Settings.PositionNegativeY);
		Item->SetNumberField(TEXT("position_positive_z"), Settings.PositionPositiveZ);
		Item->SetNumberField(TEXT("position_negative_z"), Settings.PositionNegativeZ);
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("rotate_x_alpha"), Settings.RotateXAlpha);
		Item->SetNumberField(TEXT("rotate_y_alpha"), Settings.RotateYAlpha);
		Item->SetNumberField(TEXT("rotate_z_alpha"), Settings.RotateZAlpha);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildBodyMoverGoalSettingsJson(const FIKRigBodyMoverGoalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Settings.Goal.ToString());
		Item->SetStringField(TEXT("bone_name"), Settings.BoneName.ToString());
		Item->SetNumberField(TEXT("influence_multiplier"), Settings.InfluenceMultiplier);
		return Item;
	}

	FString PBIKRootBehaviorToString(const EPBIKRootBehavior Value)
	{
		switch (Value)
		{
		case EPBIKRootBehavior::PinToInput:
			return TEXT("pin_to_input");
		case EPBIKRootBehavior::Free:
			return TEXT("free");
		case EPBIKRootBehavior::PrePull:
		default:
			return TEXT("pre_pull");
		}
	}

	bool ParsePBIKRootBehavior(const FString& InValue, EPBIKRootBehavior& OutValue)
	{
		FString Normalized = InValue.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));
		if (Normalized.IsEmpty() || Normalized == TEXT("prepull"))
		{
			OutValue = EPBIKRootBehavior::PrePull;
			return true;
		}
		if (Normalized == TEXT("pintoinput") || Normalized == TEXT("pin"))
		{
			OutValue = EPBIKRootBehavior::PinToInput;
			return true;
		}
		if (Normalized == TEXT("free"))
		{
			OutValue = EPBIKRootBehavior::Free;
			return true;
		}
		return false;
	}

	TSharedPtr<FJsonObject> BuildPBIKPrePullSettingsJson(const FRootPrePullSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("rotation_alpha_x"), Settings.RotationAlphaX);
		Item->SetNumberField(TEXT("rotation_alpha_y"), Settings.RotationAlphaY);
		Item->SetNumberField(TEXT("rotation_alpha_z"), Settings.RotationAlphaZ);
		Item->SetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		Item->SetNumberField(TEXT("position_alpha_x"), Settings.PositionAlphaX);
		Item->SetNumberField(TEXT("position_alpha_y"), Settings.PositionAlphaY);
		Item->SetNumberField(TEXT("position_alpha_z"), Settings.PositionAlphaZ);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKSolverSettingsJson(const FIKRigFBIKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("root_bone"), Settings.RootBone.ToString());
		Item->SetNumberField(TEXT("iterations"), Settings.Iterations);
		Item->SetNumberField(TEXT("sub_iterations"), Settings.SubIterations);
		Item->SetNumberField(TEXT("mass_multiplier"), Settings.MassMultiplier);
		Item->SetBoolField(TEXT("allow_stretch"), Settings.bAllowStretch);
		Item->SetStringField(TEXT("root_behavior"), PBIKRootBehaviorToString(Settings.RootBehavior));
		Item->SetObjectField(TEXT("pre_pull_root_settings"), BuildPBIKPrePullSettingsJson(Settings.PrePullRootSettings));
		Item->SetNumberField(TEXT("global_pull_chain_alpha"), Settings.GlobalPullChainAlpha);
		Item->SetNumberField(TEXT("max_angle"), Settings.MaxAngle);
		Item->SetNumberField(TEXT("over_relaxation"), Settings.OverRelaxation);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKGoalSettingsJson(const FIKRigFBIKGoalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("goal_name"), Settings.Goal.ToString());
		Item->SetStringField(TEXT("bone_name"), Settings.BoneName.ToString());
		Item->SetNumberField(TEXT("chain_depth"), Settings.ChainDepth);
		Item->SetNumberField(TEXT("strength_alpha"), Settings.StrengthAlpha);
		Item->SetNumberField(TEXT("pull_chain_alpha"), Settings.PullChainAlpha);
		Item->SetNumberField(TEXT("pin_rotation"), Settings.PinRotation);
		return Item;
	}

	TSharedPtr<FJsonObject> BuildFBIKBoneSettingsJson(const FIKRigFBIKBoneSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bone_name"), Settings.Bone.ToString());
		Item->SetNumberField(TEXT("rotation_stiffness"), Settings.RotationStiffness);
		Item->SetNumberField(TEXT("position_stiffness"), Settings.PositionStiffness);
		Item->SetBoolField(TEXT("use_preferred_angles"), Settings.bUsePreferredAngles);
		Item->SetObjectField(TEXT("preferred_angles"), BuildVectorJson(Settings.PreferredAngles));
		return Item;
	}

	bool ApplyBodyMoverSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigBodyMoverSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		SettingsObject->TryGetNumberField(TEXT("position_positive_x"), Settings.PositionPositiveX);
		SettingsObject->TryGetNumberField(TEXT("position_negative_x"), Settings.PositionNegativeX);
		SettingsObject->TryGetNumberField(TEXT("position_positive_y"), Settings.PositionPositiveY);
		SettingsObject->TryGetNumberField(TEXT("position_negative_y"), Settings.PositionNegativeY);
		SettingsObject->TryGetNumberField(TEXT("position_positive_z"), Settings.PositionPositiveZ);
		SettingsObject->TryGetNumberField(TEXT("position_negative_z"), Settings.PositionNegativeZ);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_x_alpha"), Settings.RotateXAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_y_alpha"), Settings.RotateYAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotate_z_alpha"), Settings.RotateZAlpha);
		return true;
	}

	bool ApplyPBIKPrePullSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRootPrePullSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_x"), Settings.RotationAlphaX);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_y"), Settings.RotationAlphaY);
		SettingsObject->TryGetNumberField(TEXT("rotation_alpha_z"), Settings.RotationAlphaZ);
		SettingsObject->TryGetNumberField(TEXT("position_alpha"), Settings.PositionAlpha);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_x"), Settings.PositionAlphaX);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_y"), Settings.PositionAlphaY);
		SettingsObject->TryGetNumberField(TEXT("position_alpha_z"), Settings.PositionAlphaZ);
		return true;
	}

	bool ApplyFBIKSolverSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FIKRigFBIKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		double DoubleValue = 0.0;
		if (SettingsObject->TryGetNumberField(TEXT("iterations"), DoubleValue))
		{
			Settings.Iterations = static_cast<int32>(DoubleValue);
		}
		if (SettingsObject->TryGetNumberField(TEXT("sub_iterations"), DoubleValue))
		{
			Settings.SubIterations = static_cast<int32>(DoubleValue);
		}
		SettingsObject->TryGetNumberField(TEXT("mass_multiplier"), Settings.MassMultiplier);
		SettingsObject->TryGetBoolField(TEXT("allow_stretch"), Settings.bAllowStretch);
		SettingsObject->TryGetNumberField(TEXT("global_pull_chain_alpha"), Settings.GlobalPullChainAlpha);
		SettingsObject->TryGetNumberField(TEXT("max_angle"), Settings.MaxAngle);
		SettingsObject->TryGetNumberField(TEXT("over_relaxation"), Settings.OverRelaxation);

		FString RootBehaviorText;
		if (SettingsObject->TryGetStringField(TEXT("root_behavior"), RootBehaviorText))
		{
			if (!ParsePBIKRootBehavior(RootBehaviorText, Settings.RootBehavior))
			{
				TArray<TSharedPtr<FJsonValue>> Allowed;
				for (const TCHAR* Name : {TEXT(""), TEXT("pre_pull"), TEXT("pin_to_input"), TEXT("pin"), TEXT("free")})
					Allowed.Add(MakeShared<FJsonValueString>(Name));
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_pbik_root_behavior"),
					TEXT("root_behavior"), MakeShared<FJsonValueArray>(MoveTemp(Allowed)),
					MakeShared<FJsonValueString>(RootBehaviorText), TEXT("input_decode"),
					TEXT("Root behavior does not select a supported PBIK mode after ignoring case, spaces, underscores and hyphens."));
				OutError.SourceKind = TEXT("authored_value");
				OutError.Source.InternalJsonPath = TEXT("/root_behavior");
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SettingsObject, TEXT("root_behavior"));
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* PrePullSettingsObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("pre_pull_root_settings"), PrePullSettingsObject) && PrePullSettingsObject && PrePullSettingsObject->IsValid())
		{
			if (!ApplyPBIKPrePullSettingsPatch(*PrePullSettingsObject, Settings.PrePullRootSettings, OutError))
			{
				return false;
			}
		}

		return true;
	}

	TSharedPtr<FJsonObject> BuildRetargetRootSettingsJson(const FTargetRootSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		Item->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		Item->SetObjectField(TEXT("blend_to_source_weights"), BuildVectorJson(Settings.BlendToSourceWeights));
		Item->SetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
		Item->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		Item->SetObjectField(TEXT("translation_offset"), BuildVectorJson(Settings.TranslationOffset));
		Item->SetObjectField(TEXT("rotation_offset"), BuildRotatorJson(Settings.RotationOffset));
		Item->SetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
		Item->SetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("settings"), TEXT("root_settings"), TEXT("Root Settings"), TEXT("IK Retargeter/Root Settings"), TEXT("settings"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetGlobalSettingsJson(const FRetargetGlobalSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_root"), Settings.bEnableRoot);
		Item->SetBoolField(TEXT("enable_fk"), Settings.bEnableFK);
		Item->SetBoolField(TEXT("enable_ik"), Settings.bEnableIK);
		Item->SetBoolField(TEXT("enable_post"), Settings.bEnablePost);
		Item->SetBoolField(TEXT("copy_base_pose"), Settings.bCopyBasePose);
		Item->SetStringField(TEXT("copy_base_pose_root"), Settings.CopyBasePoseRoot.ToString());
		Item->SetNumberField(TEXT("source_scale_factor"), Settings.SourceScaleFactor);
		Item->SetBoolField(TEXT("warping"), Settings.bWarping);
		Item->SetStringField(TEXT("direction_source"), WarpingDirectionSourceToString(Settings.DirectionSource));
		Item->SetStringField(TEXT("forward_direction"), BasicAxisToString(Settings.ForwardDirection));
		Item->SetStringField(TEXT("direction_chain"), Settings.DirectionChain.ToString());
		Item->SetNumberField(TEXT("warp_forwards"), Settings.WarpForwards);
		Item->SetNumberField(TEXT("sideways_offset"), Settings.SidewaysOffset);
		Item->SetNumberField(TEXT("warp_splay"), Settings.WarpSplay);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("settings"), TEXT("global_settings"), TEXT("Global Settings"), TEXT("IK Retargeter/Global Settings"), TEXT("settings"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainFKSettingsJson(const FTargetChainFKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_fk"), Settings.EnableFK);
		Item->SetStringField(TEXT("rotation_mode"), RetargetRotationModeToString(Settings.RotationMode));
		Item->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		Item->SetStringField(TEXT("translation_mode"), RetargetTranslationModeToString(Settings.TranslationMode));
		Item->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		Item->SetNumberField(TEXT("pole_vector_matching"), Settings.PoleVectorMatching);
		Item->SetBoolField(TEXT("pole_vector_maintain_offset"), Settings.PoleVectorMaintainOffset);
		Item->SetNumberField(TEXT("pole_vector_offset"), Settings.PoleVectorOffset);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("settings"), TEXT("fk"), TEXT("FK Settings"), TEXT("IK Retargeter/Chain Settings"), TEXT("settings"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainIKSettingsJson(const FTargetChainIKSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_ik"), Settings.EnableIK);
		Item->SetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		Item->SetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
		Item->SetNumberField(TEXT("blend_to_source_rotation"), Settings.BlendToSourceRotation);
		Item->SetObjectField(TEXT("blend_to_source_weights"), BuildVectorJson(Settings.BlendToSourceWeights));
		Item->SetObjectField(TEXT("static_offset"), BuildVectorJson(Settings.StaticOffset));
		Item->SetObjectField(TEXT("static_local_offset"), BuildVectorJson(Settings.StaticLocalOffset));
		Item->SetObjectField(TEXT("static_rotation_offset"), BuildRotatorJson(Settings.StaticRotationOffset));
		Item->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		Item->SetNumberField(TEXT("extension"), Settings.Extension);
		Item->SetBoolField(TEXT("affected_by_ik_warping"), Settings.bAffectedByIKWarping);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("settings"), TEXT("ik"), TEXT("IK Settings"), TEXT("IK Retargeter/Chain Settings"), TEXT("settings"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainSpeedPlantSettingsJson(const FTargetChainSpeedPlantSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("enable_speed_planting"), Settings.EnableSpeedPlanting);
		Item->SetStringField(TEXT("speed_curve_name"), Settings.SpeedCurveName.ToString());
		Item->SetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
		Item->SetNumberField(TEXT("unplant_stiffness"), Settings.UnplantStiffness);
		Item->SetNumberField(TEXT("unplant_critical_damping"), Settings.UnplantCriticalDamping);
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("settings"), TEXT("speed_planting"), TEXT("Speed Planting"), TEXT("IK Retargeter/Chain Settings"), TEXT("settings"));
		return Item;
	}

	TSharedPtr<FJsonObject> BuildRetargetChainSettingsJson(const FTargetChainSettings& Settings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetObjectField(TEXT("fk"), BuildRetargetChainFKSettingsJson(Settings.FK));
		Item->SetObjectField(TEXT("ik"), BuildRetargetChainIKSettingsJson(Settings.IK));
		Item->SetObjectField(TEXT("speed_planting"), BuildRetargetChainSpeedPlantSettingsJson(Settings.SpeedPlanting));
		return Item;
	}

	bool ApplyRetargetRootSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetRootSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		if (!ApplyVectorField(SettingsObject, TEXT("blend_to_source_weights"), Settings.BlendToSourceWeights, OutError)) return false;
		SettingsObject->TryGetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
		SettingsObject->TryGetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		if (!ApplyVectorField(SettingsObject, TEXT("translation_offset"), Settings.TranslationOffset, OutError)
			|| !ApplyRotatorField(SettingsObject, TEXT("rotation_offset"), Settings.RotationOffset, OutError)) return false;
		SettingsObject->TryGetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
		SettingsObject->TryGetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
		return true;
	}

	bool ApplyRetargetGlobalSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FRetargetGlobalSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_root"), Settings.bEnableRoot);
		SettingsObject->TryGetBoolField(TEXT("enable_fk"), Settings.bEnableFK);
		SettingsObject->TryGetBoolField(TEXT("enable_ik"), Settings.bEnableIK);
		SettingsObject->TryGetBoolField(TEXT("enable_post"), Settings.bEnablePost);
		SettingsObject->TryGetBoolField(TEXT("copy_base_pose"), Settings.bCopyBasePose);

		FString CopyBasePoseRootText;
		if (SettingsObject->TryGetStringField(TEXT("copy_base_pose_root"), CopyBasePoseRootText))
		{
			Settings.CopyBasePoseRoot = FName(*CopyBasePoseRootText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("source_scale_factor"), Settings.SourceScaleFactor);
		SettingsObject->TryGetBoolField(TEXT("warping"), Settings.bWarping);

		FString DirectionSourceText;
		if (SettingsObject->TryGetStringField(TEXT("direction_source"), DirectionSourceText))
		{
			if (!ParseWarpingDirectionSource(DirectionSourceText, Settings.DirectionSource))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_warping_direction_source"), TEXT("/direction_source"),
                    MakeShared<FJsonValueString>(TEXT("goals, chain, root_bone (aliases: goal, root)")), MakeShared<FJsonValueString>(DirectionSourceText),
                    TEXT("input_decode"), TEXT("Unsupported direction_source value."));
                OutError.SourceKind = TEXT("authored_value");
                OutError.Source.InternalJsonPath = TEXT("/direction_source");
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SettingsObject, TEXT("direction_source"));
				return false;
			}
		}

		FString ForwardDirectionText;
		if (SettingsObject->TryGetStringField(TEXT("forward_direction"), ForwardDirectionText))
		{
			if (!ParseBasicAxis(ForwardDirectionText, Settings.ForwardDirection))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_forward_direction"), TEXT("/forward_direction"),
                    MakeShared<FJsonValueString>(TEXT("x, y, z, -x, -y, -z (aliases: +x, +y, +z, forward, negx, negy, negz)")), MakeShared<FJsonValueString>(ForwardDirectionText),
                    TEXT("input_decode"), TEXT("Unsupported forward_direction value."));
                OutError.SourceKind = TEXT("authored_value");
                OutError.Source.InternalJsonPath = TEXT("/forward_direction");
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SettingsObject, TEXT("forward_direction"));
				return false;
			}
		}

		FString DirectionChainText;
		if (SettingsObject->TryGetStringField(TEXT("direction_chain"), DirectionChainText))
		{
			Settings.DirectionChain = FName(*DirectionChainText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("warp_forwards"), Settings.WarpForwards);
		SettingsObject->TryGetNumberField(TEXT("sideways_offset"), Settings.SidewaysOffset);
		SettingsObject->TryGetNumberField(TEXT("warp_splay"), Settings.WarpSplay);
		return true;
	}

	bool ApplyRetargetChainFKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainFKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_fk"), Settings.EnableFK);

		FString RotationModeText;
		if (SettingsObject->TryGetStringField(TEXT("rotation_mode"), RotationModeText))
		{
			if (!ParseRetargetRotationMode(RotationModeText, Settings.RotationMode))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_retarget_rotation_mode"), TEXT("/rotation_mode"),
                    MakeShared<FJsonValueString>(TEXT("interpolated, one_to_one, one_to_one_reversed, match_chain, match_scaled_chain, none")), MakeShared<FJsonValueString>(RotationModeText),
                    TEXT("input_decode"), TEXT("Unsupported rotation_mode value."));
                OutError.SourceKind = TEXT("authored_value");
                OutError.Source.InternalJsonPath = TEXT("/rotation_mode");
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SettingsObject, TEXT("rotation_mode"));
				return false;
			}
		}

		FString TranslationModeText;
		if (SettingsObject->TryGetStringField(TEXT("translation_mode"), TranslationModeText))
		{
			if (!ParseRetargetTranslationMode(TranslationModeText, Settings.TranslationMode))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_retarget_translation_mode"), TEXT("/translation_mode"),
                    MakeShared<FJsonValueString>(TEXT("none, globally_scaled, absolute, stretch_bone_length_uniformly, stretch_bone_length_non_uniformly")), MakeShared<FJsonValueString>(TranslationModeText),
                    TEXT("input_decode"), TEXT("Unsupported translation_mode value."));
                OutError.SourceKind = TEXT("authored_value");
                OutError.Source.InternalJsonPath = TEXT("/translation_mode");
                UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *SettingsObject, TEXT("translation_mode"));
				return false;
			}
		}

		SettingsObject->TryGetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
		SettingsObject->TryGetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
		SettingsObject->TryGetNumberField(TEXT("pole_vector_matching"), Settings.PoleVectorMatching);
		SettingsObject->TryGetBoolField(TEXT("pole_vector_maintain_offset"), Settings.PoleVectorMaintainOffset);
		SettingsObject->TryGetNumberField(TEXT("pole_vector_offset"), Settings.PoleVectorOffset);
		return true;
	}

	bool ApplyRetargetChainIKSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainIKSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_ik"), Settings.EnableIK);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source"), Settings.BlendToSource);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
		SettingsObject->TryGetNumberField(TEXT("blend_to_source_rotation"), Settings.BlendToSourceRotation);
		if (!ApplyVectorField(SettingsObject, TEXT("blend_to_source_weights"), Settings.BlendToSourceWeights, OutError)
			|| !ApplyVectorField(SettingsObject, TEXT("static_offset"), Settings.StaticOffset, OutError)
			|| !ApplyVectorField(SettingsObject, TEXT("static_local_offset"), Settings.StaticLocalOffset, OutError)
			|| !ApplyRotatorField(SettingsObject, TEXT("static_rotation_offset"), Settings.StaticRotationOffset, OutError)) return false;
		SettingsObject->TryGetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
		SettingsObject->TryGetNumberField(TEXT("extension"), Settings.Extension);
		SettingsObject->TryGetBoolField(TEXT("affected_by_ik_warping"), Settings.bAffectedByIKWarping);
		return true;
	}

	bool ApplyRetargetChainSpeedPlantSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSpeedPlantSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		SettingsObject->TryGetBoolField(TEXT("enable_speed_planting"), Settings.EnableSpeedPlanting);

		FString SpeedCurveNameText;
		if (SettingsObject->TryGetStringField(TEXT("speed_curve_name"), SpeedCurveNameText))
		{
			Settings.SpeedCurveName = FName(*SpeedCurveNameText.TrimStartAndEnd());
		}

		SettingsObject->TryGetNumberField(TEXT("speed_threshold"), Settings.SpeedThreshold);
		SettingsObject->TryGetNumberField(TEXT("unplant_stiffness"), Settings.UnplantStiffness);
		SettingsObject->TryGetNumberField(TEXT("unplant_critical_damping"), Settings.UnplantCriticalDamping);
		return true;
	}

	bool ApplyRetargetChainSettingsPatch(const TSharedPtr<FJsonObject>& SettingsObject, FTargetChainSettings& Settings, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!SettingsObject.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* FKObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("fk"), FKObject) && FKObject && FKObject->IsValid())
		{
			if (!ApplyRetargetChainFKSettingsPatch(*FKObject, Settings.FK, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* IKObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("ik"), IKObject) && IKObject && IKObject->IsValid())
		{
			if (!ApplyRetargetChainIKSettingsPatch(*IKObject, Settings.IK, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* SpeedPlantingObject = nullptr;
		if (SettingsObject->TryGetObjectField(TEXT("speed_planting"), SpeedPlantingObject) && SpeedPlantingObject && SpeedPlantingObject->IsValid())
		{
			if (!ApplyRetargetChainSpeedPlantSettingsPatch(*SpeedPlantingObject, Settings.SpeedPlanting, OutError))
			{
				return false;
			}
		}

		return true;
	}

	TSharedPtr<FJsonObject> BuildChainSettingsEntryJson(const URetargetChainSettings* ChainSettings)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("target_chain_name"), ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT(""));
		Item->SetStringField(TEXT("source_chain_name"), ChainSettings ? ChainSettings->SourceChain.ToString() : TEXT(""));
		Item->SetObjectField(TEXT("settings"), BuildRetargetChainSettingsJson(ChainSettings ? ChainSettings->Settings : FTargetChainSettings()));
		UeAgentAuthoringJson::ApplyAuthoringMetadata(Item, TEXT("target_chain_name"), ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT(""), ChainSettings ? ChainSettings->TargetChain.ToString() : TEXT("Chain Settings"), TEXT("IK Retargeter/Chain Settings"), TEXT("settings"));
		return Item;
	}

	bool ResolveChainMappingOpName(UIKRetargeterController* Controller, const FName TargetChainName, FName& OutOpName)
	{
		if (!Controller)
		{
			return false;
		}

		for (int32 OpIndex = 0; OpIndex < Controller->GetNumRetargetOps(); ++OpIndex)
		{
			const FName CandidateOpName = Controller->GetOpName(OpIndex);
			const FRetargetChainMapping* Mapping = Controller->GetChainMapping(CandidateOpName);
			if (Mapping && Mapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target))
			{
				OutOpName = CandidateOpName;
				return true;
			}
		}

		const FRetargetChainMapping* DefaultMapping = Controller->GetChainMapping(NAME_None);
		if (DefaultMapping && DefaultMapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target))
		{
			OutOpName = NAME_None;
			return true;
		}

		return false;
	}

	FAssetData GetAssetDataForObject(UObject* Object)
	{
		if (!Object)
		{
			return FAssetData();
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(Object->GetPathName()));
	}

	void GatherAssetsUnderPath(const FString& PackagePath, TMap<FString, FAssetData>& OutAssetsByObjectPath)
	{
		OutAssetsByObjectPath.Reset();
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		AssetRegistryModule.Get().GetAssetsByPath(FName(*PackagePath), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			OutAssetsByObjectPath.Add(Asset.GetObjectPathString(), Asset);
		}
	}
}
