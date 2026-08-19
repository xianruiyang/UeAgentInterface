#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialIkRetargetNative.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigBodyMoverSolver.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "Rig/Solvers/IKRigPoleSolver.h"
#include "Rig/Solvers/IKRigSetTransform.h"
#include "RigEditor/IKRigController.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			FString Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(Subject);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool TrySaveProjectAsset(
			UObject& Object,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UPackage* Package = Object.GetOutermost();
			if (!Package || !Package->GetName().StartsWith(TEXT("/Game/"))
				|| !Package->IsDirty())
			{
				return true;
			}
			if (!UEditorLoadingAndSavingUtils::SavePackages({Package}, false))
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_persistence_failed"),
					Object.GetPathName(), Package->GetName());
			}
			return true;
		}

		bool IsCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("rig.ik.definition.update")
				|| CapabilityId == TEXT("rig.retarget.definition.update")
				|| CapabilityId == TEXT("rig.retarget.pose.update")
				|| CapabilityId == TEXT("rig.retarget.pose.auto_align")
				|| CapabilityId == TEXT("rig.retarget.assets.generate");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Request->GetFamilyId() != TEXT("ik_retarget")
				|| Request->GetTypeId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				Fail(OutError,
					TEXT("rig_simulation_spatial_ik_retarget_typed_request_invalid"),
					Request->GetTypeId());
				return nullptr;
			}
			return static_cast<const FUeAgentRigSimulationSpatialRequest*>(&Request.Get());
		}

		TOptional<FUeAgentCandidateBinding> FirstTarget(
			const TArray<FUeAgentOperationTargetBinding>& Targets)
		{
			return Targets.IsEmpty() ? TOptional<FUeAgentCandidateBinding>()
				: TOptional<FUeAgentCandidateBinding>(Targets[0].GetBinding());
		}

		bool CheckRevision(
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			const FString& ExplicitRef,
			const FRigSimulationSpatialRevision& Expected,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FRigSimulationSpatialRevision Actual;
			if (!Environment.TryReadRevision(ExplicitRef, Actual, OutError)) return false;
			return Actual == Expected ? true : Fail(OutError,
				TEXT("rig_simulation_spatial_revision_mismatch"), ExplicitRef);
		}

		bool TryNativeTransform(
			const FUeAgentSpatialValue& Value,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const TArray<FString>& Components = Value.GetComponents();
			if (!Value.IsValid()
				|| Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Transform
				|| Components.Num() != 16)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_native_transform_invalid"),
					Value.GetSubject());
			}
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Component = 0.0;
					if (!LexTryParseString(Component,
							*Components[Row * 4 + Column])
						|| !FMath::IsFinite(Component))
					{
						return Fail(OutError,
							TEXT("rig_simulation_spatial_native_transform_numeric_invalid"),
							Value.GetSubject());
					}
					Matrix.M[Column][Row] = Component;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			return OutTransform.IsValid();
		}

		bool TryStringValue(
			const FString& Text,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
				Text, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(OutError,
					TEXT("rig_simulation_spatial_ik_retarget_string_failed"),
					Error.Path, Error.Code);
		}

		bool TrySnapshot(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FString& PublicRef,
			const FString& State,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentCanonicalValuePtr RefValue;
			FUeAgentCanonicalValuePtr CapabilityValue;
			FUeAgentCanonicalValuePtr StateValue;
			FUeAgentCanonicalValuePtr ObjectValue;
			FUeAgentStateSemanticError Error;
			if (!TryStringValue(PublicRef, Context, RefValue, OutError)
				|| !TryStringValue(Contract.GetCapabilityId(), Context,
					CapabilityValue, OutError)
				|| !TryStringValue(State, Context, StateValue, OutError)
				|| !FUeAgentCanonicalValue::TryObject({
						{TEXT("public_ref"), RefValue.ToSharedRef()},
						{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
						{TEXT("observed_state"), StateValue.ToSharedRef()}},
					Context.GetResourceTracker(), ObjectValue, Error))
			{
				return OutError.IsValid() ? false : Fail(OutError,
					TEXT("rig_simulation_spatial_ik_retarget_object_failed"),
					Error.Path, Error.Code);
			}
			const uint64 Upper = FCrc::StrCrc32(*State);
			const uint64 Lower = FCrc::StrCrc32(*(Contract.GetReadbackId() + State));
			const FUeAgentStateRevision Revision(Contract.GetReadbackId(),
				TEXT("native-observation-v1"),
				static_cast<int64>(((Upper << 32) | Lower) & MAX_int64));
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					TEXT("rig-simulation-spatial:") + Contract.GetCapabilityId(),
					TEXT("rig_simulation_spatial_target"), PublicRef, PublicRef,
					TEXT("result"), TEXT("native-observation-v1")),
				FUeAgentCanonicalType(Contract.GetTypedResultId(),
					EUeAgentValueKind::Object, false, TEXT("object")),
				ObjectValue.ToSharedRef(),
				FUeAgentDefaultValue(EUeAgentDefaultSourceKind::DomainNative,
					Contract.GetReadbackId(), TEXT("independent-native-readback"),
					ObjectValue.ToSharedRef()),
				FUeAgentStateDetails(TEXT("result"),
					TEXT("independent IK/Retarget native observation"),
					{TEXT("RigSimulationSpatial"), TEXT("IkRetarget")}, false,
					TEXT("available"), {}), Revision);
			return FUeAgentCanonicalSnapshot::TryCreate(
				FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
					*Contract.GetCapabilityId(), static_cast<int32>(Phase),
					*Context.GetRequestId()),
				Context.GetExecutionContext().GetStatePlane(), Revision,
				FUeAgentTemporalPoint(TEXT("editor-native"),
					FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
				FUeAgentStateProvenance(TEXT("independent_readback"),
					Contract.GetReadbackId(),
					TEXT("native-state:") + Contract.GetCapabilityId()),
				{Entry}, Context.GetResourceTracker(), OutSnapshot, Error)
				? true
				: Fail(OutError,
					TEXT("rig_simulation_spatial_ik_retarget_snapshot_failed"),
					Error.Path, Error.Code);
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Snapshot.GetEntries()[0].GetValue()->GetObjectItems())
			{
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			}
			return FString();
		}

		const FName* ResolveName(
			const TMap<FString, FName>& Bindings,
			const FString& PublicRef)
		{
			return Bindings.Find(PublicRef);
		}

		const FBoneChain* FindChain(
			const UIKRigController& Controller,
			const FName ChainName)
		{
			return Controller.GetRetargetChains().FindByPredicate(
				[ChainName](const FBoneChain& Chain)
				{
					return Chain.ChainName == ChainName;
				});
		}

		UScriptStruct* SolverStruct(const FString& Kind)
		{
			if (Kind == TEXT("full_body_ik")) return FIKRigFullBodyIKSolver::StaticStruct();
			if (Kind == TEXT("pole")) return FIKRigPoleSolver::StaticStruct();
			if (Kind == TEXT("set_transform")) return FIKRigSetTransform::StaticStruct();
			if (Kind == TEXT("body_mover")) return FIKRigBodyMoverSolver::StaticStruct();
			return nullptr;
		}

		int32 FindSolverIndex(UIKRigController& Controller, UScriptStruct* Type)
		{
			for (int32 Index = 0; Index < Controller.GetNumSolvers(); ++Index)
			{
				const FInstancedStruct* Solver = Controller.GetSolverStructAtIndex(Index);
				if (Solver && Solver->GetScriptStruct() == Type) return Index;
			}
			return INDEX_NONE;
		}

		bool ApplyIkDefinition(
			UIKRigDefinition& Rig,
			const FUeAgentRigSimulationSpatialIkRigBinding& Binding,
			const FRigIkDefinitionUpdateInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			UIKRigController* Controller = UIKRigController::GetController(&Rig);
			if (!Controller) return Fail(OutError,
				TEXT("rig_simulation_spatial_ik_controller_unavailable"), Binding.Identity);
			for (const FRigSimulationSpatialIkDefinitionChange& Change
				: Input.DefinitionChanges)
			{
				const bool bSet = Change.Operation == TEXT("set");
				if (!bSet && Change.Operation != TEXT("remove"))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_definition_operation_invalid"),
						Change.PrimaryRef);
				if (Change.Kind == TEXT("preview_mesh"))
				{
					const FString MeshRef = !Input.PreviewMeshRef.IsEmpty()
						? Input.PreviewMeshRef : Change.PrimaryRef;
					USkeletalMesh* Mesh = bSet ? Cast<USkeletalMesh>(
						Environment.ResolveExplicitObject(MeshRef)) : nullptr;
					if (bSet && !Mesh) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_preview_mesh_unavailable"), MeshRef);
					if (Controller->GetSkeletalMesh() != Mesh
						&& !Controller->SetSkeletalMesh(Mesh)) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_preview_mesh_apply_failed"), MeshRef);
				}
				else if (Change.Kind == TEXT("retarget_root"))
				{
					const FName* Root = ResolveName(Binding.BoneBindings, Change.PrimaryRef);
					if (!Root) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_bone_binding_unavailable"),
						Change.PrimaryRef);
					const FName Desired = bSet ? *Root : NAME_None;
					if (Controller->GetRetargetRoot() != Desired
						&& !Controller->SetRetargetRoot(Desired)) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_retarget_root_apply_failed"),
						Change.PrimaryRef);
				}
				else if (Change.Kind == TEXT("goal"))
				{
					const FName* Goal = ResolveName(Binding.GoalBindings, Change.PrimaryRef);
					if (!Goal) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_goal_binding_unavailable"),
						Change.PrimaryRef);
					if (bSet)
					{
						const FName* Bone = ResolveName(
							Binding.BoneBindings, Change.SecondaryRef);
						if (!Bone) return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_bone_binding_unavailable"),
							Change.SecondaryRef);
						if (Controller->GetGoal(*Goal))
						{
							if (Controller->GetBoneForGoal(*Goal) != *Bone
								&& !Controller->SetGoalBone(*Goal, *Bone)) return Fail(OutError,
								TEXT("rig_simulation_spatial_ik_goal_apply_failed"),
								Change.PrimaryRef);
						}
						else if (Controller->AddNewGoal(*Goal, *Bone).IsNone())
							return Fail(OutError,
								TEXT("rig_simulation_spatial_ik_goal_apply_failed"),
								Change.PrimaryRef);
					}
					else if (!Controller->GetGoal(*Goal) || !Controller->RemoveGoal(*Goal))
						return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_goal_remove_failed"),
							Change.PrimaryRef);
				}
				else if (Change.Kind == TEXT("retarget_chain"))
				{
					const FName* Chain = ResolveName(Binding.ChainBindings, Change.PrimaryRef);
					if (!Chain) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_chain_binding_unavailable"),
						Change.PrimaryRef);
					if (bSet)
					{
						const FName* Start = ResolveName(Binding.BoneBindings, Change.SecondaryRef);
						const FName* End = ResolveName(Binding.BoneBindings, Change.TertiaryRef);
						if (!Start || !End) return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_chain_bone_binding_unavailable"),
							Change.PrimaryRef);
						if (FindChain(*Controller, *Chain)) Controller->RemoveRetargetChain(*Chain);
						if (Controller->AddRetargetChain(*Chain, *Start, *End, NAME_None).IsNone())
							return Fail(OutError,
								TEXT("rig_simulation_spatial_ik_chain_apply_failed"),
								Change.PrimaryRef);
					}
					else if (!FindChain(*Controller, *Chain)
						|| !Controller->RemoveRetargetChain(*Chain)) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_chain_remove_failed"),
						Change.PrimaryRef);
				}
				else if (Change.Kind == TEXT("solver"))
				{
					UScriptStruct* Type = SolverStruct(Change.SolverKind);
					if (!Type) return Fail(OutError,
						TEXT("rig_simulation_spatial_ik_solver_kind_invalid"),
						Change.SolverKind);
					const int32 Existing = FindSolverIndex(*Controller, Type);
					if (bSet && Existing == INDEX_NONE && Controller->AddSolver(Type) == INDEX_NONE)
						return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_solver_apply_failed"),
							Change.PrimaryRef);
					if (!bSet && (Existing == INDEX_NONE || !Controller->RemoveSolver(Existing)))
						return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_solver_remove_failed"),
							Change.PrimaryRef);
				}
				else return Fail(OutError,
					TEXT("rig_simulation_spatial_ik_definition_kind_invalid"), Change.Kind);
			}
			return true;
		}

		bool ApplyRetargetDefinition(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& Binding,
			const FRigRetargetDefinitionUpdateInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Input.SourceRigRef == Input.TargetRigRef
				|| Input.SourceRigRef != Binding.SourceRigIdentity
				|| Input.TargetRigRef != Binding.TargetRigIdentity)
				return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_subject_binding_mismatch"),
					Binding.Identity);
			const auto* SourceBinding = Environment.FindIkRigBinding(Input.SourceRigRef);
			const auto* TargetBinding = Environment.FindIkRigBinding(Input.TargetRigRef);
			UIKRigDefinition* SourceRig = SourceBinding ? SourceBinding->Rig.Get() : nullptr;
			UIKRigDefinition* TargetRig = TargetBinding ? TargetBinding->Rig.Get() : nullptr;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			if (!SourceRig || !TargetRig || SourceRig == TargetRig || !Controller)
				return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_subject_unavailable"), Binding.Identity);
			Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
			Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
			Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source,
				Cast<USkeletalMesh>(Environment.ResolveExplicitObject(
					SourceBinding->PreviewMeshIdentity)));
			Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target,
				Cast<USkeletalMesh>(Environment.ResolveExplicitObject(
					TargetBinding->PreviewMeshIdentity)));
			if (!Controller->GetChainMapping(NAME_None)) Controller->AddDefaultOps();
			UIKRigController* TargetController = UIKRigController::GetController(TargetRig);
			if (!TargetController) return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_target_controller_unavailable"),
				Input.TargetRigRef);
			for (const FBoneChain& Chain : TargetController->GetRetargetChains())
				Controller->SetSourceChain(NAME_None, Chain.ChainName);
			for (const FRigSimulationSpatialChainMapping& Mapping : Input.ChainMappings)
			{
				const FName* SourceChain = ResolveName(
					SourceBinding->ChainBindings, Mapping.SourceChainRef);
				const FName* TargetChain = ResolveName(
					TargetBinding->ChainBindings, Mapping.TargetChainRef);
				if (!SourceChain || !TargetChain
					|| !FindChain(*UIKRigController::GetController(SourceRig), *SourceChain))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_chain_binding_unavailable"),
						Mapping.TargetChainRef);
				const FBoneChain* TargetChainValue = FindChain(*TargetController, *TargetChain);
				if (!TargetChainValue) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_chain_binding_unavailable"),
					Mapping.TargetChainRef);
				if (!Mapping.GoalRef.IsEmpty())
				{
					const FName* Goal = ResolveName(TargetBinding->GoalBindings, Mapping.GoalRef);
					if (!Goal || TargetChainValue->IKGoalName != *Goal) return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_goal_binding_mismatch"),
						Mapping.GoalRef);
				}
				if (!Controller->SetSourceChain(*SourceChain, *TargetChain)) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_mapping_apply_failed"),
					Mapping.TargetChainRef);
			}
			return true;
		}

		bool SideFacts(
			const FString& SubjectSide,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			ERetargetSourceOrTarget& OutSide,
			const FUeAgentRigSimulationSpatialIkRigBinding*& OutRigBinding,
			FString& OutNativeFrame,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (SubjectSide == TEXT("source"))
			{
				OutSide = ERetargetSourceOrTarget::Source;
				OutRigBinding = Environment.FindIkRigBinding(
					RetargeterBinding.SourceRigIdentity);
				OutNativeFrame = RetargeterBinding.SourceNativePoseFrameId;
			}
			else if (SubjectSide == TEXT("target"))
			{
				OutSide = ERetargetSourceOrTarget::Target;
				OutRigBinding = Environment.FindIkRigBinding(
					RetargeterBinding.TargetRigIdentity);
				OutNativeFrame = RetargeterBinding.TargetNativePoseFrameId;
			}
			else return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_subject_side_invalid"), SubjectSide);
			return OutRigBinding && !OutNativeFrame.IsEmpty();
		}

		bool DesiredPoseTransform(
			const FRigSimulationSpatialPoseItem& Item,
			const FUeAgentRigSimulationSpatialRequest& Request,
			const FString& NativeFrame,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Item.BoneRef != Item.Transform.Subject) return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_pose_subject_mismatch"), Item.BoneRef);
			const FUeAgentSpatialValuePtr* Value = Request.GetFormalPortRequest()
				.GetSpatialValues().FindByPredicate(
					[&Item](const FUeAgentSpatialValuePtr& Candidate)
					{
						return Candidate.IsValid()
							&& Candidate->GetSubject() == Item.Transform.Subject
							&& Candidate->GetQuantityKind()
								== EUeAgentSpatialQuantityKind::Transform;
					});
			if (!Value) return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_pose_formal_value_missing"),
				Item.BoneRef);
			FUeAgentSpatialValuePtr Native;
			return Environment.TryConvertSpatialValueToFrame(
				*Value->ToSharedRef(), NativeFrame, Resources, Native, OutError)
				&& Native.IsValid() && TryNativeTransform(*Native, OutTransform, OutError);
		}

		bool ApplyRetargetPose(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			const FRigRetargetPoseUpdateInput& Input,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			ERetargetSourceOrTarget Side;
			const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
			FString NativeFrame;
			if (!SideFacts(Input.SubjectSide, RetargeterBinding, Environment,
				Side, RigBinding, NativeFrame, OutError)) return false;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			UIKRigController* RigController = RigBinding
				? UIKRigController::GetController(RigBinding->Rig.Get()) : nullptr;
			if (!Controller || !RigController) return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
				Input.RetargeterRef);
			const FName RootBone = RigController->GetRetargetRoot();
			for (const FRigSimulationSpatialPoseItem& Item : Input.Pose)
			{
				const FName* Bone = ResolveName(RigBinding->BoneBindings, Item.BoneRef);
				FTransform Transform;
				if (!Bone || !DesiredPoseTransform(Item, Request, NativeFrame,
					Environment, Resources, Transform, OutError)) return false;
				if (!Transform.GetScale3D().Equals(FVector::OneVector, 0.001))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_pose_scale_unsupported"),
						Item.BoneRef);
				if (*Bone == RootBone)
				{
					const FVector Translation = Transform.GetTranslation();
					if (!FMath::IsNearlyZero(Translation.X, 0.001)
						|| !FMath::IsNearlyZero(Translation.Y, 0.001))
						return Fail(OutError,
							TEXT("rig_simulation_spatial_retarget_pose_horizontal_root_translation_unsupported"),
							Item.BoneRef);
					Controller->GetCurrentRetargetPose(Side)
						.SetRootTranslationDelta(Translation);
				}
				else if (!Transform.GetTranslation().IsNearlyZero(0.001))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_pose_non_root_translation_unsupported"),
						Item.BoneRef);
				Controller->SetRotationOffsetForRetargetPoseBone(
					*Bone, Transform.GetRotation(), Side);
			}
			return true;
		}

		bool ApplyAutoAlign(
			UIKRetargeter& Retargeter,
			const FUeAgentRigSimulationSpatialRetargeterBinding& RetargeterBinding,
			const FRigRetargetPoseAutoAlignInput& Input,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			ERetargetSourceOrTarget Side;
			const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
			FString NativeFrame;
			if (!SideFacts(Input.SubjectSide, RetargeterBinding, Environment,
				Side, RigBinding, NativeFrame, OutError)) return false;
			UIKRetargeterController* Controller =
				UIKRetargeterController::GetController(&Retargeter);
			if (!Controller) return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_pose_controller_unavailable"),
				Input.RetargeterRef);
			TArray<FName> Bones;
			for (const FString& BoneRef : Input.BoneRefs)
			{
				const FName* Bone = ResolveName(RigBinding->BoneBindings, BoneRef);
				if (!Bone) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_pose_bone_binding_unavailable"),
					BoneRef);
				Bones.Add(*Bone);
			}
			if (Input.AlignmentPolicy == TEXT("root"))
			{
				if (Bones.Num() != 1) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_root_alignment_bone_required"),
					Input.RetargeterRef);
				Controller->SnapBoneToGround(Bones[0], Side);
			}
			else if (Input.AlignmentPolicy == TEXT("chain"))
			{
				if (Bones.IsEmpty()) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_chain_alignment_bones_required"),
					Input.RetargeterRef);
				Controller->AutoAlignBones(Bones,
					ERetargetAutoAlignMethod::ChainToChain, Side);
			}
			else if (Input.AlignmentPolicy == TEXT("full_pose"))
			{
				if (!Bones.IsEmpty()) return Fail(OutError,
					TEXT("rig_simulation_spatial_retarget_full_pose_alignment_bones_forbidden"),
					Input.RetargeterRef);
				Controller->AutoAlignAllBones(Side,
					ERetargetAutoAlignMethod::ChainToChain);
			}
			else return Fail(OutError,
				TEXT("rig_simulation_spatial_retarget_alignment_policy_invalid"),
				Input.AlignmentPolicy);
			return true;
		}

		FString AssetObjectPath(const FString& Root, const FString& Name)
		{
			return Root / Name + TEXT(".") + Name;
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Contract.GetCapabilityId() == TEXT("rig.ik.definition.update"))
			{
				const auto& Input = Request.GetPayload().Get<FRigIkDefinitionUpdateInput>();
				const auto* Binding = Environment.FindIkRigBinding(Input.IkRigRef);
				UIKRigController* Controller = Binding
					? UIKRigController::GetController(Binding->Rig.Get()) : nullptr;
				if (!Binding || !Controller) return FString();
				bool bMatches = true;
				for (const auto& Change : Input.DefinitionChanges)
				{
					const bool bSet = Change.Operation == TEXT("set");
					if (Change.Kind == TEXT("preview_mesh"))
					{
						UObject* Mesh = Environment.ResolveExplicitObject(
							!Input.PreviewMeshRef.IsEmpty()
								? Input.PreviewMeshRef : Change.PrimaryRef);
						bMatches &= bSet ? Controller->GetSkeletalMesh() == Mesh
							: Controller->GetSkeletalMesh() == nullptr;
					}
					else if (Change.Kind == TEXT("retarget_root"))
					{
						const FName* Name = ResolveName(Binding->BoneBindings,
							Change.PrimaryRef);
						bMatches &= Name && Controller->GetRetargetRoot()
							== (bSet ? *Name : NAME_None);
					}
					else if (Change.Kind == TEXT("goal"))
					{
						const FName* Goal = ResolveName(Binding->GoalBindings,
							Change.PrimaryRef);
						const FName* Bone = ResolveName(Binding->BoneBindings,
							Change.SecondaryRef);
						bMatches &= Goal && (bSet
							? Bone && Controller->GetGoal(*Goal)
								&& Controller->GetBoneForGoal(*Goal) == *Bone
							: !Controller->GetGoal(*Goal));
					}
					else if (Change.Kind == TEXT("retarget_chain"))
					{
						const FName* Chain = ResolveName(Binding->ChainBindings,
							Change.PrimaryRef);
						bMatches &= Chain && (bSet
							? FindChain(*Controller, *Chain) != nullptr
							: FindChain(*Controller, *Chain) == nullptr);
					}
					else if (Change.Kind == TEXT("solver"))
					{
						const int32 Index = FindSolverIndex(*Controller,
							SolverStruct(Change.SolverKind));
						bMatches &= bSet ? Index != INDEX_NONE : Index == INDEX_NONE;
					}
				}
				return FString::Printf(TEXT("ik=%s|matches=%d|root=%s|solvers=%d"),
					*Input.IkRigRef, bMatches ? 1 : 0,
					*Controller->GetRetargetRoot().ToString(), Controller->GetNumSolvers());
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.definition.update"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetDefinitionUpdateInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				UIKRetargeterController* Controller = Binding
					? UIKRetargeterController::GetController(Binding->Retargeter.Get()) : nullptr;
				UIKRigDefinition* Source = Cast<UIKRigDefinition>(
					Environment.ResolveExplicitObject(Input.SourceRigRef));
				UIKRigDefinition* Target = Cast<UIKRigDefinition>(
					Environment.ResolveExplicitObject(Input.TargetRigRef));
				bool bMatches = Controller && Source && Target && Source != Target
					&& Controller->GetIKRig(ERetargetSourceOrTarget::Source) == Source
					&& Controller->GetIKRig(ERetargetSourceOrTarget::Target) == Target;
				if (Controller)
				{
					const auto* SourceBinding = Environment.FindIkRigBinding(Input.SourceRigRef);
					const auto* TargetBinding = Environment.FindIkRigBinding(Input.TargetRigRef);
					for (const auto& Mapping : Input.ChainMappings)
					{
						const FName* SourceName = SourceBinding
							? ResolveName(SourceBinding->ChainBindings,
								Mapping.SourceChainRef) : nullptr;
						const FName* TargetName = TargetBinding
							? ResolveName(TargetBinding->ChainBindings,
								Mapping.TargetChainRef) : nullptr;
						bMatches &= SourceName && TargetName
							&& Controller->GetSourceChain(*TargetName) == *SourceName;
					}
				}
				return FString::Printf(
					TEXT("retargeter=%s|source=%s|target=%s|matches=%d"),
					*Input.RetargeterRef, *Input.SourceRigRef, *Input.TargetRigRef,
					bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.update"))
			{
				const auto& Input = Request.GetPayload().Get<FRigRetargetPoseUpdateInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				ERetargetSourceOrTarget Side;
				const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
				FString NativeFrame;
				if (!Binding || !SideFacts(Input.SubjectSide, *Binding, Environment,
					Side, RigBinding, NativeFrame, OutError)) return FString();
				UIKRetargeterController* Controller =
					UIKRetargeterController::GetController(Binding->Retargeter.Get());
				UIKRigController* RigController = RigBinding
					? UIKRigController::GetController(RigBinding->Rig.Get()) : nullptr;
				if (!Controller || !RigController) return FString();
				bool bMatches = true;
				for (const auto& Item : Input.Pose)
				{
					const FName* Bone = ResolveName(RigBinding->BoneBindings, Item.BoneRef);
					FTransform Desired;
					if (!Bone || !DesiredPoseTransform(Item, Request, NativeFrame,
						Environment, Resources, Desired, OutError)) return FString();
					bMatches &= Controller->GetRotationOffsetForRetargetPoseBone(*Bone, Side)
						.Equals(Desired.GetRotation(), 0.001);
					if (*Bone == RigController->GetRetargetRoot())
						bMatches &= Controller->GetRootOffsetInRetargetPose(Side)
							.Equals(Desired.GetTranslation(), 0.001);
				}
				const FVector Root = Controller->GetRootOffsetInRetargetPose(Side);
				return FString::Printf(TEXT("retargeter=%s|side=%s|root=%.9g,%.9g,%.9g|matches=%d"),
					*Input.RetargeterRef, *Input.SubjectSide, Root.X, Root.Y, Root.Z,
					bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.auto_align"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetPoseAutoAlignInput>();
				const auto* Binding = Environment.FindRetargeterBinding(Input.RetargeterRef);
				ERetargetSourceOrTarget Side;
				const FUeAgentRigSimulationSpatialIkRigBinding* RigBinding = nullptr;
				FString NativeFrame;
				if (!Binding || !SideFacts(Input.SubjectSide, *Binding, Environment,
					Side, RigBinding, NativeFrame, OutError)) return FString();
				UIKRetargeterController* Controller =
					UIKRetargeterController::GetController(Binding->Retargeter.Get());
				if (!Controller) return FString();
				const FVector Root = Controller->GetRootOffsetInRetargetPose(Side);
				return FString::Printf(TEXT("retargeter=%s|side=%s|root=%.9g,%.9g,%.9g|matches=1"),
					*Input.RetargeterRef, *Input.SubjectSide, Root.X, Root.Y, Root.Z);
			}
			if (Contract.GetCapabilityId() == TEXT("rig.retarget.assets.generate"))
			{
				const auto& Input = Request.GetPayload()
					.Get<FRigRetargetAssetsGenerateInput>();
				USkeletalMesh* Target = Cast<USkeletalMesh>(
					Environment.ResolveExplicitObject(Input.TargetSubjectRef));
				bool bMatches = Target != nullptr;
				int32 GeneratedCount = 0;
				for (const auto& Item : Input.AssetRequests)
				{
					const FString Path = AssetObjectPath(Input.DestinationRoot, Item.Name);
					UAnimSequence* Animation = FindObject<UAnimSequence>(nullptr, *Path);
					bMatches &= Item.AssetKind == TEXT("animation_sequence")
						&& Animation && Animation->GetSkeleton() == Target->GetSkeleton()
						&& Animation->GetDataModel()
						&& Animation->GetDataModel()->GetNumberOfKeys() > 0;
					GeneratedCount += Animation ? 1 : 0;
				}
				return FString::Printf(TEXT("destination=%s|generated=%d|matches=%d"),
					*Input.DestinationRoot, GeneratedCount, bMatches ? 1 : 0);
			}
			Fail(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				Contract.GetCapabilityId());
			return FString();
		}

		class FReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			explicit FReadback(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef))
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_mismatch"), PublicRef);
				const FString State = ReadState(Contract, *Typed, *Environment,
					Context.GetResourceTracker(), OutError);
				return !State.IsEmpty() && TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			FMutation(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment,
				TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> InAssetEffectPort)
				: Environment(MoveTemp(InEnvironment))
				, AssetEffectPort(MoveTemp(InAssetEffectPort)) {}

			bool TryMutate(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialMutationResult& OutResult,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutResult = {};
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				const FUeAgentRigSimulationSpatialRequest* Typed =
					ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet()
					|| Target->GetPublicRef() != Typed->GetPrimaryTargetRef())
					return Fail(OutError,
						TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						Typed->GetPrimaryTargetRef());

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId() == TEXT("rig.ik.definition.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FRigIkDefinitionUpdateInput>();
					const auto* Binding = Environment->FindIkRigBinding(Input.IkRigRef);
					UIKRigDefinition* Rig = Binding ? Binding->Rig.Get() : nullptr;
					if (!Binding || !Rig
						|| Environment->ResolveExplicitObject(Input.IkRigRef) != Rig)
						return Fail(OutError,
							TEXT("rig_simulation_spatial_ik_binding_unavailable"), Input.IkRigRef);
					if (!CheckRevision(*Environment, Input.IkRigRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRigDefinition* Probe = DuplicateObject<UIKRigDefinition>(Rig,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRigDefinition::StaticClass(), TEXT("UAI_IK_Probe")));
					if (!Probe || !ApplyIkDefinition(*Probe, *Binding, Input,
						*Environment, OutError)) return false;
					Rig->Modify();
					if (!ApplyIkDefinition(*Rig, *Binding, Input, *Environment, OutError))
						return false;
					Rig->MarkPackageDirty();
					MutatedObject = Rig;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.retarget.definition.update"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetDefinitionUpdateInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter
						|| Environment->ResolveExplicitObject(Input.RetargeterRef) != Retargeter)
						return Fail(OutError,
							TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
							Input.RetargeterRef);
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Retarget_Probe")));
					if (!Probe || !ApplyRetargetDefinition(*Probe, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->Modify();
					if (!ApplyRetargetDefinition(*Retargeter, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else if (Contract.GetCapabilityId() == TEXT("rig.retarget.pose.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FRigRetargetPoseUpdateInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter) return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
						Input.RetargeterRef);
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Pose_Probe")));
					if (!Probe || !ApplyRetargetPose(*Probe, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Retargeter->Modify();
					if (!ApplyRetargetPose(*Retargeter, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("rig.retarget.pose.auto_align"))
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetPoseAutoAlignInput>();
					const auto* Binding = Environment->FindRetargeterBinding(Input.RetargeterRef);
					UIKRetargeter* Retargeter = Binding ? Binding->Retargeter.Get() : nullptr;
					if (!Binding || !Retargeter) return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_binding_unavailable"),
						Input.RetargeterRef);
					if (!CheckRevision(*Environment, Input.RetargeterRef,
						Input.ExpectedRevision, OutError)) return false;
					UIKRetargeter* Probe = DuplicateObject<UIKRetargeter>(Retargeter,
						GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
							UIKRetargeter::StaticClass(), TEXT("UAI_Align_Probe")));
					if (!Probe || !ApplyAutoAlign(*Probe, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->Modify();
					if (!ApplyAutoAlign(*Retargeter, *Binding, Input,
						*Environment, OutError)) return false;
					Retargeter->MarkPackageDirty();
					MutatedObject = Retargeter;
				}
				else
				{
					const auto& Input = Typed->GetPayload()
						.Get<FRigRetargetAssetsGenerateInput>();
					if (!AssetEffectPort.IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_asset_effect_port_unavailable"),
						Input.DestinationRoot);
					UObject* Source = Environment->ResolveExplicitObject(Input.SourceSubjectRef);
					UObject* TargetSubject =
						Environment->ResolveExplicitObject(Input.TargetSubjectRef);
					UIKRetargeter* Retargeter = Cast<UIKRetargeter>(
						Environment->ResolveExplicitObject(Input.RetargeterRef));
					FUeAgentRigSimulationSpatialRetargetAssetEffectRequest EffectRequest;
					EffectRequest.SourceSubject = Source;
					EffectRequest.TargetSubject = TargetSubject;
					EffectRequest.Retargeter = Retargeter;
					EffectRequest.DestinationRoot = Input.DestinationRoot;
					EffectRequest.AssetRequests = Input.AssetRequests;
					if (!EffectRequest.IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_retarget_asset_effect_request_invalid"),
						Input.DestinationRoot);
					FUeAgentRigSimulationSpatialRetargetAssetEffectResult EffectResult;
					if (!AssetEffectPort->TryApplyRetargetAssets(
						EffectRequest, Context, EffectResult, OutError)
						|| !EffectResult.IsValid()) return false;
					MutatedObject = EffectResult.GeneratedAssets[0].Get();
				}

				if (!IsValid(MutatedObject)) return Fail(OutError,
					TEXT("rig_simulation_spatial_ik_retarget_mutation_target_unavailable"),
					Typed->GetPrimaryTargetRef());
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, MutatedObject);
				OutResult = FUeAgentRigSimulationSpatialMutationResult(OperationId,
					TEXT("native-mutation:") + MutatedObject->GetPathName());
				return OutResult.IsValid();
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
			TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort;
		};

		class FSettle final : public IUeAgentRigSimulationSpatialSettlePort
		{
		public:
			explicit FSettle(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment)
				: Environment(MoveTemp(InEnvironment)) {}

			bool TrySettle(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("rig_simulation_spatial_settle_target_unavailable"),
					Mutation.GetOperationId());
				if (UAnimSequence* Animation = Cast<UAnimSequence>(Object))
					Animation->RefreshCacheData();
				Object->PostEditChange();
				return TrySaveProjectAsset(*Object, OutError);
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
		};

		class FComparator final : public IUeAgentRigSimulationSpatialResultComparator
		{
		public:
			bool TryCompare(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutError = {};
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				if (!ProductionRequest(Request, OutError)) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				bool bMatches = !BeforeState.IsEmpty() && !AfterState.IsEmpty()
					&& AfterState.Contains(TEXT("|matches=1"));
				if (Contract.GetCapabilityId()
					== TEXT("rig.retarget.pose.auto_align"))
					bMatches &= BeforeState != AfterState;
				OutOutcome = FVerificationOutcome(
					TEXT("rig-simulation-spatial-verify:") + Contract.GetCapabilityId(),
					bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
					bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
					bMatches ? FString()
						: TEXT("rig_simulation_spatial_ik_retarget_state_mismatch"),
					false, {}, {TEXT("independent-state:") + After.GetSnapshotId()}, {});
				return true;
			}
		};
	}

	bool FUeAgentRigSimulationSpatialIkRigBinding::IsValid() const
	{
		if (Identity.IsEmpty() || !Rig.IsValid() || PreviewMeshIdentity.IsEmpty()
			|| BoneBindings.IsEmpty()) return false;
		auto ValidNames = [](const TMap<FString, FName>& Values)
		{
			for (const auto& Pair : Values)
				if (Pair.Key.IsEmpty() || Pair.Value.IsNone()) return false;
			return true;
		};
		return ValidNames(BoneBindings) && ValidNames(GoalBindings)
			&& ValidNames(ChainBindings);
	}

	bool FUeAgentRigSimulationSpatialRetargeterBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Retargeter.IsValid()
			&& !SourceRigIdentity.IsEmpty() && !TargetRigIdentity.IsEmpty()
			&& SourceRigIdentity != TargetRigIdentity
			&& !SourceNativePoseFrameId.IsEmpty()
			&& !TargetNativePoseFrameId.IsEmpty()
			&& SourceNativePoseFrameId != TargetNativePoseFrameId;
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectRequest::IsValid() const
	{
		if (!SourceSubject.IsValid() || !TargetSubject.IsValid()
			|| SourceSubject == TargetSubject || !Retargeter.IsValid()
			|| AssetRequests.IsEmpty()
			|| !FPackageName::IsValidLongPackageName(DestinationRoot)) return false;
		TSet<FString> Names;
		for (const FRigSimulationSpatialAssetRequest& Request : AssetRequests)
		{
			if (Request.AssetKind.IsEmpty() || Request.Name.IsEmpty()
				|| (Request.OverwritePolicy != TEXT("fail")
					&& Request.OverwritePolicy != TEXT("replace"))
				|| Names.Contains(Request.Name)
				|| !FPackageName::IsValidObjectPath(
					AssetObjectPath(DestinationRoot, Request.Name))) return false;
			Names.Add(Request.Name);
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialRetargetAssetEffectResult::IsValid() const
	{
		if (GeneratedAssets.IsEmpty()
			|| GeneratedAssets.Num() != GeneratedAssetRefs.Num()) return false;
		TSet<FString> Refs;
		for (int32 Index = 0; Index < GeneratedAssets.Num(); ++Index)
		{
			if (!GeneratedAssets[Index].IsValid()
				|| GeneratedAssetRefs[Index].IsEmpty()
				|| GeneratedAssets[Index]->GetPathName() != GeneratedAssetRefs[Index]
				|| Refs.Contains(GeneratedAssetRefs[Index])) return false;
			Refs.Add(GeneratedAssetRefs[Index]);
		}
		return true;
	}

	bool TryCreateRigSimulationSpatialIkRetargetNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("rig.ik.definition.update"),
			TEXT("rig.retarget.definition.update"),
			TEXT("rig.retarget.pose.update"),
			TEXT("rig.retarget.pose.auto_align"),
			TEXT("rig.retarget.assets.generate"),
		};
		OutPorts.Readback = MakeShared<FReadback>(Environment);
		OutPorts.Mutation = MakeShared<FMutation>(Environment, MoveTemp(AssetEffectPort));
		OutPorts.Settle = MakeShared<FSettle>(Environment);
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_ik_retarget_ports_unavailable"));
	}
}
