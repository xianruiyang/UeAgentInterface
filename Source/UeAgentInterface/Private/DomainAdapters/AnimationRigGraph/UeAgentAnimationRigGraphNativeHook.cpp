#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"

#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/MeshDeformerCollection.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigGizmoLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "ReferenceSkeleton.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigFullBodyIK.h"
#include "RigEditor/IKRigController.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "Units/RigUnit.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	namespace
	{
		FString NewEvidence(const TCHAR* Stage, const UObject& Subject)
		{
			return FString::Printf(
				TEXT("native:%s:%s:%s"),
				Stage,
				*Subject.GetPathName(),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString ClassPath(const UObject& Object)
		{
			return Object.GetClass()->GetPathName();
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.ToString(EGuidFormats::DigitsWithHyphens);
		}

		FString NativeComposite(const UObject& Object, const FVector& Position)
		{
			return FString::Printf(
				TEXT("%s@%.17g,%.17g,%.17g"),
				*Object.GetPathName(), Position.X, Position.Y, Position.Z);
		}

		FString ItemRevision(const FAnimationRigGraphTopology& Topology)
		{
			uint64 Hash = 1469598103934665603ull;
			auto Mix = [&Hash](const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				for (int32 Index = 0; Index < Utf8.Length(); ++Index)
				{
					Hash ^= static_cast<uint8>(Utf8.Get()[Index]);
					Hash *= 1099511628211ull;
				}
				Hash ^= 0xff;
				Hash *= 1099511628211ull;
			};
			TArray<FString> Facts;
			for (const FAnimationRigGraphItemIdentity& Item : Topology.GetItems())
			{
				Facts.Add(Item.GetItemKey() + TEXT("|") + Item.GetNativeIdentity()
					+ TEXT("|") + Item.GetNativeTypeIdentity());
			}
			for (const FAnimationRigGraphPinIdentity& Pin : Topology.GetPins())
			{
				Facts.Add(Pin.GetPinKey() + TEXT("|") + Pin.GetOwnerItemKey());
			}
			for (const FAnimationRigGraphRelationIdentity& Relation : Topology.GetRelations())
			{
				Facts.Add(Relation.GetRelationKey() + TEXT("|")
					+ Relation.GetFromIdentity() + TEXT("|") + Relation.GetToIdentity());
			}
			Facts.Sort();
			for (const FString& Fact : Facts)
			{
				Mix(Fact);
			}
			return FString::Printf(TEXT("native-revision:%016llx"), Hash);
		}

		FAnimationRigGraphItemIdentity ObjectItem(
			const FString& Key,
			const UObject& Object,
			const FString& SemanticRole)
		{
			return FAnimationRigGraphItemIdentity(
				Key, TEXT("native_object_path"), Object.GetPathName(), FString(),
				ClassPath(Object), ClassPath(Object), SemanticRole);
		}

		bool TryControlType(const FString& Ref, ERigControlType& OutType)
		{
			const TMap<FString, ERigControlType> Types = {
				{TEXT("Bool"), ERigControlType::Bool},
				{TEXT("Float"), ERigControlType::Float},
				{TEXT("ScaleFloat"), ERigControlType::ScaleFloat},
				{TEXT("Integer"), ERigControlType::Integer},
				{TEXT("Vector2D"), ERigControlType::Vector2D},
				{TEXT("Position"), ERigControlType::Position},
				{TEXT("Scale"), ERigControlType::Scale},
				{TEXT("Rotator"), ERigControlType::Rotator},
				{TEXT("Transform"), ERigControlType::Transform},
				{TEXT("TransformNoScale"), ERigControlType::TransformNoScale},
				{TEXT("EulerTransform"), ERigControlType::EulerTransform}};
			const ERigControlType* Found = Types.Find(Ref);
			if (!Found)
			{
				return false;
			}
			OutType = *Found;
			return true;
		}

		FRigControlValue InitialControlValue(const ERigControlType Type)
		{
			switch (Type)
			{
			case ERigControlType::Bool:
				return FRigControlValue::Make<bool>(false);
			case ERigControlType::Float:
				return FRigControlValue::Make<float>(0.0f);
			case ERigControlType::ScaleFloat:
				return FRigControlValue::Make<float>(1.0f);
			case ERigControlType::Integer:
				return FRigControlValue::Make<int32>(0);
			case ERigControlType::Vector2D:
				return FRigControlValue::Make<FVector2D>(FVector2D::ZeroVector);
			case ERigControlType::Position:
				return FRigControlValue::Make<FVector>(FVector::ZeroVector);
			case ERigControlType::Scale:
				return FRigControlValue::Make<FVector>(FVector::OneVector);
			case ERigControlType::Rotator:
				return FRigControlValue::Make<FRotator>(FRotator::ZeroRotator);
			case ERigControlType::Transform:
				return FRigControlValue::Make<FTransform>(FTransform::Identity);
			case ERigControlType::TransformNoScale:
			{
				const FTransformNoScale Identity = FTransform::Identity;
				return FRigControlValue::Make<FTransformNoScale>(Identity);
			}
			case ERigControlType::EulerTransform:
				return FRigControlValue::Make<FEulerTransform>(FEulerTransform::Identity);
			default:
				return FRigControlValue::Make<float>(0.0f);
			}
		}
	}

	void FAnimationRigGraphNativeHook::SetReceipts(
		const UObject& Subject,
		const EAnimationRigGraphBarrierKind BarrierKind,
		const bool bBarrierComplete,
		const int32 ErrorCount,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt)
	{
		OutApplyReceipt.SetEvidenceId(NewEvidence(TEXT("apply"), Subject));
		OutBarrierReceipt.BarrierKind = BarrierKind;
		OutBarrierReceipt.bComplete = bBarrierComplete;
		OutBarrierReceipt.ErrorCount = ErrorCount;
		OutBarrierReceipt.BarrierEvidenceId = NewEvidence(TEXT("barrier"), Subject);
		OutBarrierReceipt.SettleEvidenceId = NewEvidence(TEXT("settle"), Subject);
	}

	bool FAnimationRigGraphNativeHook::FinalizeTopologyCapture(
		const UObject& Subject,
		const FAnimationRigGraphTopology& Topology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError)
	{
		if (!TryValidateAnimationRigGraphTopology(Topology, OutError))
		{
			return false;
		}
		OutRevision = ItemRevision(Topology);
		OutEvidenceId = NewEvidence(TEXT("independent-readback"), Subject);
		return true;
	}

	bool FAnimationRigGraphNativeHook::TryCreateAndCaptureOwnedAssetObject(
		UObject& Outer,
		const FString& AssetKind,
		UClass& AssetClass,
		const FName ObjectName,
		UObject*& OutObject,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphAssetIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		OutObject = nullptr;
		if (AssetKind.IsEmpty() || ObjectName.IsNone()
			|| AssetClass.HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			OutError.Set(TEXT("animation_rig_graph_asset_create_invalid"));
			return false;
		}
		OutObject = NewObject<UObject>(&Outer, &AssetClass, ObjectName, RF_Transactional);
		if (!OutObject)
		{
			OutError.Set(TEXT("animation_rig_graph_asset_create_failed"));
			return false;
		}
		return TryCaptureOwnedAssetObject(
			Outer, AssetKind, AssetClass, ObjectName, *OutObject,
			OutApplyReceipt, OutBarrierReceipt, OutIdentity,
			OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryCaptureOwnedAssetObject(
		UObject& Outer,
		const FString& AssetKind,
		UClass& AssetClass,
		const FName ObjectName,
		UObject& Object,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphAssetIdentity& OutIdentity,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (AssetKind.IsEmpty() || ObjectName.IsNone()
			|| Object.GetOuter() != &Outer || Object.GetFName() != ObjectName
			|| Object.GetClass() != &AssetClass)
		{
			OutError.Set(TEXT("animation_rig_graph_asset_identity_mismatch"));
			return false;
		}
		Object.Modify();
		OutIdentity = FAnimationRigGraphAssetIdentity(
			AssetKind, Object.GetPathName(), AssetClass.GetPathName());
		if (!TryValidateAnimationRigGraphAssetIdentity(OutIdentity, OutError))
		{
			return false;
		}
		SetReceipts(Object, EAnimationRigGraphBarrierKind::Refresh, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		OutRevision = FString::Printf(TEXT("native-revision:%s:%s"),
			*Object.GetPathName(), *AssetClass.GetPathName());
		OutEvidenceId = NewEvidence(TEXT("independent-readback"), Object);
		return true;
	}

	bool FAnimationRigGraphNativeHook::TryEnsureCompileAndCaptureAnimBlueprint(
		UAnimBlueprint& Blueprint,
		USkeleton& Skeleton,
		const FName GraphName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (GraphName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_graph_name_missing"));
			return false;
		}
		Blueprint.Modify();
		Blueprint.TargetSkeleton = &Skeleton;
		Blueprint.ParentClass = UAnimInstance::StaticClass();
		UEdGraph* Graph = nullptr;
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (UEdGraph* Candidate : Graphs)
		{
			if (Candidate && Candidate->GetFName() == GraphName)
			{
				Graph = Candidate;
				break;
			}
		}
		if (!Graph)
		{
			Graph = FBlueprintEditorUtils::CreateNewGraph(
				&Blueprint, GraphName,
				UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
			if (!Graph)
			{
				OutError.Set(TEXT("animation_rig_graph_anim_graph_create_failed"));
				return false;
			}
			FBlueprintEditorUtils::AddDomainSpecificGraph(&Blueprint, Graph);
		}
		TArray<UAnimGraphNode_Root*> Roots;
		Graph->GetNodesOfClass(Roots);
		if (Roots.IsEmpty() && Graph->GetSchema())
		{
			Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
			Graph->GetNodesOfClass(Roots);
		}
		if (Roots.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_anim_root_create_failed"));
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		const bool bCompileComplete = CompilerLog.NumErrors == 0 && Blueprint.Status != BS_Error;
		SetReceipts(Blueprint, EAnimationRigGraphBarrierKind::Compile,
			bCompileComplete, CompilerLog.NumErrors, OutApplyReceipt, OutBarrierReceipt);
		if (!bCompileComplete)
		{
			OutError.Set(TEXT("animation_rig_graph_compile_incomplete"), LexToString(CompilerLog.NumErrors));
			return false;
		}

		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		const FString GraphKey = TEXT("anim_graph:") + GuidText(Graph->GraphGuid);
		Items.Emplace(GraphKey, TEXT("native_guid"), GuidText(Graph->GraphGuid), FString(),
			ClassPath(*Graph), UAnimationGraph::StaticClass()->GetPathName(), TEXT("graph"));
		for (int32 Index = 0; Index < Roots.Num(); ++Index)
		{
			UAnimGraphNode_Root* Root = Roots[Index];
			if (!Root || !Root->NodeGuid.IsValid())
			{
				continue;
			}
			const FString NodeKey = TEXT("anim_node:") + GuidText(Root->NodeGuid);
			Items.Emplace(NodeKey, TEXT("native_guid"), GuidText(Root->NodeGuid), FString(),
				ClassPath(*Root), TEXT("/Script/Engine.AnimNode_Root"), TEXT("runtime_node"));
			Relations.Emplace(TEXT("graph_member:") + GuidText(Root->NodeGuid),
				TEXT("graph_member"), FString(), GraphKey, NodeKey, Index);
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Blueprint, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureValidateAndCaptureBlendSpace(
		UBlendSpace& BlendSpace,
		USkeleton& Skeleton,
		UAnimSequence& Sequence,
		const FName AxisName,
		const FVector& SamplePosition,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (AxisName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_blend_axis_missing"));
			return false;
		}
		if (Sequence.GetSkeleton() != &Skeleton)
		{
			OutError.Set(TEXT("animation_rig_graph_blend_sample_skeleton_mismatch"));
			return false;
		}
		BlendSpace.Modify();
		BlendSpace.SetSkeleton(&Skeleton);
		FBlendParameter& Axis = const_cast<FBlendParameter&>(BlendSpace.GetBlendParameter(0));
		Axis.DisplayName = AxisName.ToString();
		Axis.Min = FMath::Min(0.0, SamplePosition.X);
		Axis.Max = FMath::Max(1.0, SamplePosition.X);
		const int32 SampleIndex = BlendSpace.AddSample(&Sequence, SamplePosition);
		if (SampleIndex == INDEX_NONE)
		{
			OutError.Set(TEXT("animation_rig_graph_blend_sample_add_failed"));
			return false;
		}
		BlendSpace.ValidateSampleData();
		BlendSpace.ResampleData();
		SetReceipts(BlendSpace, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);

		const FString AxisKey = TEXT("blend_axis:0:") + Axis.DisplayName;
		const FString SampleKey = FString::Printf(TEXT("blend_sample:%d"), SampleIndex);
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Emplace(AxisKey, TEXT("native_composite"),
			TEXT("axis:0:") + Axis.DisplayName, FString(), ClassPath(BlendSpace),
			TEXT("/Script/Engine.BlendParameter"), TEXT("blend_axis"));
		Items.Emplace(SampleKey, TEXT("native_composite"),
			NativeComposite(Sequence, SamplePosition), FString(), ClassPath(BlendSpace),
			TEXT("/Script/Engine.BlendSample"), TEXT("blend_sample"));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("sample_axis:") + LexToString(SampleIndex),
			TEXT("sample_coordinate"), FString(), SampleKey, AxisKey, 0);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(BlendSpace, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureBlendSpaceDefinition(
		UBlendSpace& BlendSpace,
		USkeleton& Skeleton,
		const TArray<FAnimationRigGraphBlendAxisMutation>& Axes,
		const TArray<FAnimationRigGraphBlendSampleMutation>& Samples,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (Axes.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_blend_axes_missing"));
			return false;
		}
		BlendSpace.Modify();
		BlendSpace.SetSkeleton(&Skeleton);
		TSet<int32> AxisIndices;
		for (const FAnimationRigGraphBlendAxisMutation& AxisMutation : Axes)
		{
			if (AxisMutation.AxisIndex < 0 || AxisMutation.AxisIndex > 1
				|| AxisMutation.AxisName.IsNone()
				|| !FMath::IsFinite(AxisMutation.Minimum)
				|| !FMath::IsFinite(AxisMutation.Maximum)
				|| AxisMutation.Maximum <= AxisMutation.Minimum
				|| AxisMutation.GridDivisions < 1
				|| AxisIndices.Contains(AxisMutation.AxisIndex))
			{
				OutError.Set(TEXT("animation_rig_graph_blend_axis_invalid"),
					LexToString(AxisMutation.AxisIndex));
				return false;
			}
			AxisIndices.Add(AxisMutation.AxisIndex);
			FBlendParameter& Axis = const_cast<FBlendParameter&>(
				BlendSpace.GetBlendParameter(AxisMutation.AxisIndex));
			Axis.DisplayName = AxisMutation.AxisName.ToString();
			Axis.Min = AxisMutation.Minimum;
			Axis.Max = AxisMutation.Maximum;
			Axis.GridNum = AxisMutation.GridDivisions;
		}

		for (const FAnimationRigGraphBlendSampleMutation& SampleMutation : Samples)
		{
			if (!SampleMutation.Animation
				|| SampleMutation.Animation->GetSkeleton() != &Skeleton)
			{
				OutError.Set(TEXT(
					"animation_rig_graph_blend_sample_skeleton_mismatch"));
				return false;
			}
			int32 ExistingIndex = INDEX_NONE;
			const TArray<FBlendSample>& ExistingSamples = BlendSpace.GetBlendSamples();
			for (int32 Index = 0; Index < ExistingSamples.Num(); ++Index)
			{
				if (ExistingSamples[Index].Animation == SampleMutation.Animation
					&& ExistingSamples[Index].SampleValue.Equals(
						SampleMutation.Coordinates, UE_DOUBLE_SMALL_NUMBER))
				{
					ExistingIndex = Index;
					break;
				}
			}
			switch (SampleMutation.Intent)
			{
			case EAnimationRigGraphMutationIntent::Create:
				if (ExistingIndex != INDEX_NONE
					|| BlendSpace.AddSample(
						SampleMutation.Animation,
						SampleMutation.Coordinates) == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_blend_sample_create_failed"));
					return false;
				}
				break;
			case EAnimationRigGraphMutationIntent::Update:
				if (ExistingIndex == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_blend_sample_missing"));
					return false;
				}
				break;
			case EAnimationRigGraphMutationIntent::Replace:
				if (ExistingIndex == INDEX_NONE
					&& BlendSpace.AddSample(
						SampleMutation.Animation,
						SampleMutation.Coordinates) == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_blend_sample_replace_failed"));
					return false;
				}
				break;
			case EAnimationRigGraphMutationIntent::Delete:
				if (ExistingIndex == INDEX_NONE
					|| !BlendSpace.DeleteSample(ExistingIndex))
				{
					OutError.Set(TEXT("animation_rig_graph_blend_sample_delete_failed"));
					return false;
				}
				break;
			}
		}
		BlendSpace.ValidateSampleData();
		BlendSpace.ResampleData();
		SetReceipts(BlendSpace, EAnimationRigGraphBarrierKind::Validate,
			true, 0, OutApplyReceipt, OutBarrierReceipt);

		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
		{
			const FBlendParameter& Axis = BlendSpace.GetBlendParameter(AxisIndex);
			const FString AxisKey = FString::Printf(
				TEXT("blend_axis:%d"), AxisIndex);
			Items.Emplace(AxisKey, TEXT("native_composite"),
				FString::Printf(TEXT("axis:%d:%s"), AxisIndex, *Axis.DisplayName),
				FString(), ClassPath(BlendSpace),
				TEXT("/Script/Engine.BlendParameter"), TEXT("blend_axis"));
		}
		const TArray<FBlendSample>& CurrentSamples = BlendSpace.GetBlendSamples();
		for (int32 SampleIndex = 0; SampleIndex < CurrentSamples.Num(); ++SampleIndex)
		{
			const FBlendSample& Sample = CurrentSamples[SampleIndex];
			if (!Sample.Animation)
			{
				continue;
			}
			const FString SampleKey = FString::Printf(
				TEXT("blend_sample:%d"), SampleIndex);
			Items.Emplace(SampleKey, TEXT("native_composite"),
				NativeComposite(*Sample.Animation, Sample.SampleValue), FString(),
				ClassPath(BlendSpace), TEXT("/Script/Engine.BlendSample"),
				TEXT("blend_sample"));
			Relations.Emplace(TEXT("sample_axis:") + LexToString(SampleIndex),
				TEXT("sample_coordinate"), FString(), SampleKey,
				TEXT("blend_axis:0"), 0);
		}
		OutTopology = FAnimationRigGraphTopology(
			MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(
			BlendSpace, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureSkeletonDefinition(
		USkeleton& Skeleton,
		const TArray<FAnimationRigGraphSkeletonBoneMutation>& Mutations,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (Mutations.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_skeleton_definition_missing"));
			return false;
		}
		Skeleton.Modify();
		FReferenceSkeletonModifier Modifier(&Skeleton);
		for (const FAnimationRigGraphSkeletonBoneMutation& Mutation : Mutations)
		{
			if (Mutation.BoneName.IsNone())
			{
				OutError.Set(TEXT("animation_rig_graph_skeleton_bone_missing"));
				return false;
			}
			const int32 BoneIndex = Modifier.FindBoneIndex(Mutation.BoneName);
			const int32 ParentIndex = Mutation.ParentBone.IsNone()
				? INDEX_NONE : Modifier.FindBoneIndex(Mutation.ParentBone);
			if (!Mutation.ParentBone.IsNone() && ParentIndex == INDEX_NONE)
			{
				OutError.Set(TEXT("animation_rig_graph_skeleton_parent_missing"),
					Mutation.ParentBone.ToString());
				return false;
			}
			switch (Mutation.Intent)
			{
			case EAnimationRigGraphMutationIntent::Create:
				if (BoneIndex != INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_skeleton_bone_exists"),
						Mutation.BoneName.ToString());
					return false;
				}
				Modifier.Add(FMeshBoneInfo(
					Mutation.BoneName, Mutation.BoneName.ToString(), ParentIndex),
					FTransform::Identity, Mutation.ParentBone.IsNone());
				break;
			case EAnimationRigGraphMutationIntent::Delete:
				if (BoneIndex == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_skeleton_bone_missing"),
						Mutation.BoneName.ToString());
					return false;
				}
				Modifier.Remove(Mutation.BoneName, false);
				break;
			case EAnimationRigGraphMutationIntent::Update:
			case EAnimationRigGraphMutationIntent::Replace:
				if (BoneIndex == INDEX_NONE
					|| Skeleton.GetReferenceSkeleton().GetParentIndex(BoneIndex)
						!= ParentIndex)
				{
					OutError.Set(TEXT("animation_rig_graph_skeleton_reparent_unsupported"),
						Mutation.BoneName.ToString());
					return false;
				}
				break;
			}
		}
		SetReceipts(Skeleton, EAnimationRigGraphBarrierKind::Refresh,
			true, 0, OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		const FReferenceSkeleton& Ref = Skeleton.GetReferenceSkeleton();
		for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
		{
			const FName BoneName = Ref.GetBoneName(Index);
			const FString BoneKey = TEXT("bone:") + BoneName.ToString();
			Items.Emplace(BoneKey, TEXT("native_key"), BoneName.ToString(),
				FString(), ClassPath(Skeleton),
				TEXT("/Script/Engine.ReferenceSkeleton"), TEXT("bone"));
			const int32 ParentIndex = Ref.GetParentIndex(Index);
			if (ParentIndex != INDEX_NONE)
			{
				Relations.Emplace(TEXT("hierarchy:") + BoneName.ToString(),
					TEXT("hierarchy_child"), BoneName.ToString(),
					TEXT("bone:") + Ref.GetBoneName(ParentIndex).ToString(),
					BoneKey, Index);
			}
		}
		OutTopology = FAnimationRigGraphTopology(
			MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(
			Skeleton, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureSkeletonCompatibility(
		USkeleton& Skeleton,
		const TArray<FAnimationRigGraphSkeletonCompatibilityMutation>& Mutations,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (Mutations.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_compatibility_missing"));
			return false;
		}
		Skeleton.Modify();
		for (const FAnimationRigGraphSkeletonCompatibilityMutation& Mutation
			: Mutations)
		{
			if (!Mutation.CompatibleSkeleton
				|| Mutation.CompatibleSkeleton == &Skeleton)
			{
				OutError.Set(TEXT("animation_rig_graph_compatible_skeleton_invalid"));
				return false;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				Skeleton.RemoveCompatibleSkeleton(Mutation.CompatibleSkeleton);
			}
			else
			{
				Skeleton.AddCompatibleSkeleton(Mutation.CompatibleSkeleton);
			}
		}
		SetReceipts(Skeleton, EAnimationRigGraphBarrierKind::Refresh,
			true, 0, OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		for (const TSoftObjectPtr<USkeleton>& Compatible
			: Skeleton.GetCompatibleSkeletons())
		{
			const FString Ref = Compatible.ToSoftObjectPath().ToString();
			Items.Emplace(TEXT("compatible_skeleton:") + Ref,
				TEXT("native_object_path"), Ref, FString(), ClassPath(Skeleton),
				USkeleton::StaticClass()->GetPathName(), TEXT("compatible_skeleton"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Skeleton, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureSkeletonVirtualBone(
		USkeleton& Skeleton,
		const FAnimationRigGraphSkeletonVirtualBoneMutation& Mutation,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (Mutation.Name.IsNone()
			|| Mutation.SourceBone.IsNone() || Mutation.TargetBone.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_virtual_bone_invalid"));
			return false;
		}
		Skeleton.Modify();
		const bool bExists = Skeleton.GetVirtualBones().ContainsByPredicate(
			[&Mutation](const FVirtualBone& Bone)
			{
				return Bone.VirtualBoneName == Mutation.Name;
			});
		if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
		{
			if (!bExists)
			{
				OutError.Set(TEXT("animation_rig_graph_virtual_bone_missing"));
				return false;
			}
			Skeleton.RemoveVirtualBones({Mutation.Name});
		}
		else if (!bExists
			&& !Skeleton.AddNewNamedVirtualBone(
				Mutation.SourceBone, Mutation.TargetBone, Mutation.Name))
		{
			OutError.Set(TEXT("animation_rig_graph_virtual_bone_add_failed"));
			return false;
		}
		SetReceipts(Skeleton, EAnimationRigGraphBarrierKind::Refresh,
			true, 0, OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		for (const FVirtualBone& Bone : Skeleton.GetVirtualBones())
		{
			Items.Emplace(TEXT("virtual_bone:") + Bone.VirtualBoneName.ToString(),
				TEXT("native_key"), Bone.VirtualBoneName.ToString(), FString(),
				ClassPath(Skeleton), TEXT("/Script/Engine.VirtualBone"),
				TEXT("virtual_bone"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Skeleton, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureRefreshAndCaptureSkeleton(
		USkeleton& Skeleton,
		USkeleton& CompatibleSkeleton,
		const FName SourceBone,
		const FName TargetBone,
		const FName VirtualBoneName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (SourceBone.IsNone() || TargetBone.IsNone() || VirtualBoneName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_skeleton_key_missing"));
			return false;
		}
		Skeleton.Modify();
		{
			FReferenceSkeletonModifier Modifier(&Skeleton);
			if (Modifier.FindBoneIndex(SourceBone) == INDEX_NONE)
			{
				Modifier.Add(FMeshBoneInfo(SourceBone, SourceBone.ToString(), INDEX_NONE),
					FTransform::Identity, false);
			}
			if (Modifier.FindBoneIndex(TargetBone) == INDEX_NONE)
			{
				Modifier.Add(FMeshBoneInfo(TargetBone, TargetBone.ToString(),
					Modifier.FindBoneIndex(SourceBone)), FTransform::Identity, false);
			}
		}
		Skeleton.AddCompatibleSkeleton(&CompatibleSkeleton);
		const bool bVirtualBoneExists = Skeleton.GetVirtualBones().ContainsByPredicate(
			[VirtualBoneName](const FVirtualBone& Bone) { return Bone.VirtualBoneName == VirtualBoneName; });
		if (!bVirtualBoneExists
			&& !Skeleton.AddNewNamedVirtualBone(SourceBone, TargetBone, VirtualBoneName))
		{
			OutError.Set(TEXT("animation_rig_graph_virtual_bone_add_failed"));
			return false;
		}
		SetReceipts(Skeleton, EAnimationRigGraphBarrierKind::Refresh, true, 0,
			OutApplyReceipt, OutBarrierReceipt);

		const FString SourceKey = TEXT("bone:") + SourceBone.ToString();
		const FString TargetKey = TEXT("bone:") + TargetBone.ToString();
		const FString VirtualKey = TEXT("virtual_bone:") + VirtualBoneName.ToString();
		const FString CompatibleKey = TEXT("compatible_skeleton:") + CompatibleSkeleton.GetPathName();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Emplace(SourceKey, TEXT("native_key"), SourceBone.ToString(), FString(),
			ClassPath(Skeleton), TEXT("/Script/Engine.ReferenceSkeleton"), TEXT("bone"));
		Items.Emplace(TargetKey, TEXT("native_key"), TargetBone.ToString(), FString(),
			ClassPath(Skeleton), TEXT("/Script/Engine.ReferenceSkeleton"), TEXT("bone"));
		Items.Emplace(VirtualKey, TEXT("native_key"), VirtualBoneName.ToString(), FString(),
			ClassPath(Skeleton), TEXT("/Script/Engine.VirtualBone"), TEXT("virtual_bone"));
		Items.Add(ObjectItem(CompatibleKey, CompatibleSkeleton, TEXT("compatible_skeleton")));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("hierarchy:") + TargetBone.ToString(), TEXT("hierarchy_child"),
			TargetBone.ToString(), SourceKey, TargetKey, 0);
		Relations.Emplace(TEXT("virtual_source:") + VirtualBoneName.ToString(),
			TEXT("virtual_bone_source"), VirtualBoneName.ToString(), SourceKey, VirtualKey, INDEX_NONE);
		Relations.Emplace(TEXT("compatible:") + CompatibleSkeleton.GetPathName(),
			TEXT("compatible_skeleton"), CompatibleSkeleton.GetPathName(), SourceKey, CompatibleKey, 0);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Skeleton, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryEnsureCompileAndCaptureControlRig(
		UControlRigBlueprint& Blueprint,
		const FName ParentBoneName,
		const FName ControlName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		URigHierarchy* Hierarchy = Blueprint.GetHierarchy();
		URigHierarchyController* Controller = Blueprint.GetHierarchyController();
		if (!Hierarchy || !Controller || ParentBoneName.IsNone() || ControlName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_control_rig_controller_unavailable"));
			return false;
		}
		Blueprint.Modify();
		Blueprint.ParentClass = UControlRig::StaticClass();
		const FRigElementKey BoneKey(ParentBoneName, ERigElementType::Bone);
		if (!Hierarchy->Contains(BoneKey))
		{
			Controller->AddBone(ParentBoneName, FRigElementKey(), FTransform::Identity,
				true, ERigBoneType::User, false, false);
		}
		const FRigElementKey ControlKey(ControlName, ERigElementType::Control);
		if (!Hierarchy->Contains(ControlKey))
		{
			FRigControlSettings Settings;
			Settings.ControlType = ERigControlType::Float;
			Settings.SetupLimitArrayForType();
			Controller->AddControl(ControlName, BoneKey, Settings,
				FRigControlValue::Make<float>(0.0f), FTransform::Identity,
				FTransform::Identity, false, false);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		const bool bCompileComplete = CompilerLog.NumErrors == 0 && Blueprint.Status != BS_Error;
		SetReceipts(Blueprint, EAnimationRigGraphBarrierKind::Compile,
			bCompileComplete, CompilerLog.NumErrors, OutApplyReceipt, OutBarrierReceipt);
		if (!bCompileComplete)
		{
			OutError.Set(TEXT("animation_rig_graph_compile_incomplete"), LexToString(CompilerLog.NumErrors));
			return false;
		}
		const FString BoneItemKey = TEXT("rig_element:bone:") + ParentBoneName.ToString();
		const FString ControlItemKey = TEXT("rig_element:control:") + ControlName.ToString();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Emplace(BoneItemKey, TEXT("native_key"),
			TEXT("Bone:") + ParentBoneName.ToString(), FString(),
			ClassPath(*Hierarchy), TEXT("/Script/ControlRig.RigBoneElement"), TEXT("bone"));
		Items.Emplace(ControlItemKey, TEXT("native_key"),
			TEXT("Control:") + ControlName.ToString(), FString(),
			ClassPath(*Hierarchy), TEXT("/Script/ControlRig.RigControlElement"), TEXT("control"));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("hierarchy:") + ControlName.ToString(), TEXT("hierarchy_child"),
			ControlName.ToString(), BoneItemKey, ControlItemKey, 0);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Blueprint, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureControlRigDefinition(
		UControlRigBlueprint& Blueprint,
		const TArray<FAnimationRigGraphControlMutation>& Mutations,
		const TArray<FAnimationRigGraphUnitMutation>& UnitMutations,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		URigHierarchy* Hierarchy = Blueprint.GetHierarchy();
		URigHierarchyController* Controller = Blueprint.GetHierarchyController();
		if (!Hierarchy || !Controller
			|| (Mutations.IsEmpty() && UnitMutations.IsEmpty()))
		{
			OutError.Set(TEXT("animation_rig_graph_control_rig_controller_unavailable"));
			return false;
		}
		Blueprint.Modify();
		Blueprint.ParentClass = UControlRig::StaticClass();
		for (const FAnimationRigGraphControlMutation& Mutation : Mutations)
		{
			const FRigElementKey ControlKey(Mutation.Name, ERigElementType::Control);
			const bool bExists = Hierarchy->Contains(ControlKey);
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveElement(ControlKey, false, false))
				{
					OutError.Set(TEXT("animation_rig_graph_control_remove_failed"),
						Mutation.Name.ToString());
					return false;
				}
				continue;
			}
			ERigControlType ControlType = ERigControlType::Float;
			const FRigElementKey ParentKey(Mutation.ParentBone, ERigElementType::Bone);
			if (Mutation.Name.IsNone() || Mutation.ParentBone.IsNone()
				|| !Hierarchy->Contains(ParentKey)
				|| !TryControlType(Mutation.ControlTypeRef, ControlType))
			{
				OutError.Set(TEXT("animation_rig_graph_control_definition_invalid"),
					Mutation.Name.ToString());
				return false;
			}
			if (bExists
				&& (Mutation.Intent == EAnimationRigGraphMutationIntent::Update
					|| Mutation.Intent == EAnimationRigGraphMutationIntent::Replace)
				&& !Controller->RemoveElement(ControlKey, false, false))
			{
				OutError.Set(TEXT("animation_rig_graph_control_replace_failed"),
					Mutation.Name.ToString());
				return false;
			}
			FRigControlSettings Settings;
			Settings.ControlType = ControlType;
			Settings.SetupLimitArrayForType();
			const FRigElementKey Added = Controller->AddControl(
				Mutation.Name, ParentKey, Settings, InitialControlValue(ControlType),
				FTransform::Identity, FTransform::Identity, false, false);
			if (Added != ControlKey)
			{
				OutError.Set(TEXT("animation_rig_graph_control_add_failed"),
					Mutation.Name.ToString());
				return false;
			}
		}

		URigVMGraph* const RigGraph = Blueprint.GetDefaultModel();
		URigVMController* const RigController = RigGraph
			? Blueprint.GetController(RigGraph) : nullptr;
		if (!UnitMutations.IsEmpty() && (!RigGraph || !RigController))
		{
			OutError.Set(TEXT("animation_rig_graph_rigvm_controller_unavailable"));
			return false;
		}
		auto FindRigNode = [RigGraph](const FName Name) -> URigVMNode*
		{
			if (!RigGraph) return nullptr;
			for (URigVMNode* Node : RigGraph->GetNodes())
			{
				if (Node && Node->GetFName() == Name)
				{
					return Node;
				}
			}
			return nullptr;
		};
		for (const FAnimationRigGraphUnitMutation& Mutation : UnitMutations)
		{
			if (Mutation.NodeName.IsNone())
			{
				OutError.Set(TEXT("animation_rig_graph_rigvm_unit_name_invalid"));
				return false;
			}
			URigVMNode* Existing = FindRigNode(Mutation.NodeName);
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!Existing
					|| !RigController->RemoveNode(Existing, true, false))
				{
					OutError.Set(
						TEXT("animation_rig_graph_rigvm_unit_remove_failed"),
						Mutation.NodeName.ToString());
					return false;
				}
				continue;
			}
			UScriptStruct* const UnitStruct = LoadObject<UScriptStruct>(
				nullptr, *Mutation.ScriptStructRef);
			if (!UnitStruct
				|| UnitStruct == FRigUnit::StaticStruct()
				|| !UnitStruct->IsChildOf(FRigUnit::StaticStruct())
				|| UnitStruct->HasMetaData(TEXT("Abstract"))
				|| Mutation.MethodName.IsNone()
				|| !FMath::IsFinite(Mutation.Position.X)
				|| !FMath::IsFinite(Mutation.Position.Y))
			{
				OutError.Set(
					TEXT("animation_rig_graph_rigvm_unit_definition_invalid"),
					Mutation.NodeName.ToString());
				return false;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Create
				&& Existing)
			{
				OutError.Set(
					TEXT("animation_rig_graph_rigvm_unit_already_exists"),
					Mutation.NodeName.ToString());
				return false;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Replace
				&& Existing)
			{
				if (!RigController->RemoveNode(Existing, true, false))
				{
					OutError.Set(
						TEXT("animation_rig_graph_rigvm_unit_replace_failed"),
						Mutation.NodeName.ToString());
					return false;
				}
				Existing = nullptr;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Update)
			{
				const URigVMUnitNode* const ExistingUnit =
					Cast<URigVMUnitNode>(Existing);
				if (!ExistingUnit
					|| !ExistingUnit->GetScriptStruct()
					|| ExistingUnit->GetScriptStruct()->GetPathName()
						!= Mutation.ScriptStructRef
					|| ExistingUnit->GetMethodName() != Mutation.MethodName
					|| !RigController->SetNodePosition(
						Existing, Mutation.Position, true, false, false))
				{
					OutError.Set(
						TEXT("animation_rig_graph_rigvm_unit_update_failed"),
						Mutation.NodeName.ToString());
					return false;
				}
				continue;
			}
			URigVMNode* const Added = RigController->AddUnitNodeFromStructPath(
				Mutation.ScriptStructRef,
				Mutation.MethodName,
				Mutation.Position,
				Mutation.NodeName.ToString(),
				true,
				false);
			if (!Added || Added->GetFName() != Mutation.NodeName)
			{
				OutError.Set(
					TEXT("animation_rig_graph_rigvm_unit_add_failed"),
					Mutation.NodeName.ToString());
				return false;
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		FCompilerResultsLog CompilerLog;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
		const bool bCompileComplete = CompilerLog.NumErrors == 0
			&& Blueprint.Status != BS_Error;
		SetReceipts(Blueprint, EAnimationRigGraphBarrierKind::Compile,
			bCompileComplete, CompilerLog.NumErrors, OutApplyReceipt, OutBarrierReceipt);
		if (!bCompileComplete)
		{
			OutError.Set(TEXT("animation_rig_graph_compile_incomplete"),
				LexToString(CompilerLog.NumErrors));
			return false;
		}
		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		for (const FAnimationRigGraphControlMutation& Mutation : Mutations)
		{
			const FRigElementKey Key(Mutation.Name, ERigElementType::Control);
			if (!Hierarchy->Contains(Key))
			{
				continue;
			}
			const FString ParentItemKey =
				TEXT("rig_element:bone:") + Mutation.ParentBone.ToString();
			if (!Items.ContainsByPredicate(
				[&ParentItemKey](const FAnimationRigGraphItemIdentity& Item)
				{
					return Item.GetItemKey() == ParentItemKey;
				}))
			{
				Items.Emplace(ParentItemKey, TEXT("native_key"),
					TEXT("Bone:") + Mutation.ParentBone.ToString(), FString(),
					ClassPath(*Hierarchy), TEXT("/Script/ControlRig.RigBoneElement"),
					TEXT("bone"));
			}
			const FString ItemKey = TEXT("rig_element:control:") + Mutation.Name.ToString();
			Items.Emplace(ItemKey, TEXT("native_key"),
				TEXT("Control:") + Mutation.Name.ToString(), FString(),
				ClassPath(*Hierarchy), TEXT("/Script/ControlRig.RigControlElement"),
				TEXT("control"));
			Relations.Emplace(TEXT("hierarchy:") + Mutation.Name.ToString(),
				TEXT("hierarchy_child"), Mutation.Name.ToString(),
				ParentItemKey,
				ItemKey, INDEX_NONE);
		}
		for (const FAnimationRigGraphUnitMutation& Mutation : UnitMutations)
		{
			URigVMNode* const Node = FindRigNode(Mutation.NodeName);
			const URigVMUnitNode* const Unit = Cast<URigVMUnitNode>(Node);
			if (!Unit || !Unit->GetScriptStruct())
			{
				continue;
			}
			Items.Emplace(
				TEXT("rigvm_node:") + Mutation.NodeName.ToString(),
				TEXT("native_object_path"),
				Unit->GetPathName(),
				FString(),
				Unit->GetClass()->GetPathName(),
				Unit->GetScriptStruct()->GetPathName(),
				TEXT("rig_unit"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(
			Blueprint, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureRefreshAndCaptureControlRigShapeLibrary(
		UControlRigShapeLibrary& Library,
		const FName ShapeName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (ShapeName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_shape_name_missing"));
			return false;
		}
		Library.Modify();
		if (!Library.Shapes.ContainsByPredicate(
			[ShapeName](const FControlRigShapeDefinition& Shape) { return Shape.ShapeName == ShapeName; }))
		{
			FControlRigShapeDefinition Shape;
			Shape.ShapeName = ShapeName;
			Library.Shapes.Add(Shape);
		}
		SetReceipts(Library, EAnimationRigGraphBarrierKind::Refresh, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		for (int32 Index = 0; Index < Library.Shapes.Num(); ++Index)
		{
			const FControlRigShapeDefinition& Shape = Library.Shapes[Index];
			Items.Emplace(TEXT("shape:") + Shape.ShapeName.ToString(), TEXT("native_key"),
				Shape.ShapeName.ToString(), FString(), ClassPath(Library),
				TEXT("/Script/ControlRig.ControlRigShapeDefinition"), TEXT("shape"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(Library, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureControlRigShapeLibrary(
		UControlRigShapeLibrary& Library,
		const TArray<FAnimationRigGraphShapeMutation>& Mutations,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (Mutations.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_shape_mutations_empty"));
			return false;
		}
		UMaterial* SharedMaterial = nullptr;
		for (const FAnimationRigGraphShapeMutation& Mutation : Mutations)
		{
			if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
			{
				if (!Mutation.Mesh)
				{
					OutError.Set(TEXT("animation_rig_graph_shape_mesh_invalid"),
						Mutation.Name.ToString());
					return false;
				}
				if (!Mutation.Material)
				{
					OutError.Set(TEXT("animation_rig_graph_shape_material_invalid"),
						Mutation.Name.ToString());
					return false;
				}
				if (Mutation.OrderIndex < 0)
				{
					OutError.Set(TEXT("animation_rig_graph_shape_order_invalid"),
						Mutation.Name.ToString());
					return false;
				}
				if (SharedMaterial && SharedMaterial != Mutation.Material)
				{
					OutError.Set(TEXT("animation_rig_graph_shape_material_conflict"),
						Mutation.Name.ToString());
					return false;
				}
				SharedMaterial = Mutation.Material;
			}
		}
		Library.Modify();
		if (SharedMaterial)
		{
			Library.DefaultMaterial = SharedMaterial;
		}
		for (const FAnimationRigGraphShapeMutation& Mutation : Mutations)
		{
			const int32 ExistingIndex = Library.Shapes.IndexOfByPredicate(
				[&Mutation](const FControlRigShapeDefinition& Shape)
				{
					return Shape.ShapeName == Mutation.Name;
				});
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (ExistingIndex == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_shape_remove_failed"),
						Mutation.Name.ToString());
					return false;
				}
				Library.Shapes.RemoveAt(ExistingIndex);
				continue;
			}
			if (ExistingIndex != INDEX_NONE)
			{
				Library.Shapes.RemoveAt(ExistingIndex);
			}
			FControlRigShapeDefinition Shape;
			Shape.ShapeName = Mutation.Name;
			Shape.StaticMesh = Mutation.Mesh;
			Library.Shapes.Insert(
				MoveTemp(Shape), FMath::Clamp(Mutation.OrderIndex, 0, Library.Shapes.Num()));
		}
		SetReceipts(Library, EAnimationRigGraphBarrierKind::Refresh, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		for (int32 Index = 0; Index < Library.Shapes.Num(); ++Index)
		{
			const FControlRigShapeDefinition& Shape = Library.Shapes[Index];
			Items.Emplace(TEXT("shape:") + Shape.ShapeName.ToString(), TEXT("native_key"),
				Shape.ShapeName.ToString(), FString(), ClassPath(Library),
				TEXT("/Script/ControlRig.ControlRigShapeDefinition"), TEXT("shape"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Library, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryEnsureCompileAndCaptureDeformerGraph(
		UOptimusDeformer& Deformer,
		const FName CommentNodeName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UOptimusNodeGraph* Graph = Deformer.GetUpdateGraph();
		if (!Graph || CommentNodeName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_optimus_graph_unavailable"));
			return false;
		}
		Deformer.Modify();
		UOptimusNode* Comment = Graph->GetAllNodes().ContainsByPredicate(
			[CommentNodeName](const UOptimusNode* Node) { return Node && Node->GetFName() == CommentNodeName; })
			? *Graph->GetAllNodes().FindByPredicate(
				[CommentNodeName](const UOptimusNode* Node) { return Node && Node->GetFName() == CommentNodeName; })
			: nullptr;
		if (!Comment)
		{
			Comment = Graph->AddCommentNode(FVector2D(100.0, 100.0),
				FVector2D(300.0, 160.0), FLinearColor::White);
			if (Comment)
			{
				Comment->Rename(*CommentNodeName.ToString(), Graph, REN_DontCreateRedirectors);
			}
		}
		if (!Comment)
		{
			OutError.Set(TEXT("animation_rig_graph_optimus_comment_add_failed"));
			return false;
		}
		const bool bCompileComplete = Deformer.Compile();
		SetReceipts(Deformer, EAnimationRigGraphBarrierKind::Compile,
			bCompileComplete, bCompileComplete ? 0 : 1, OutApplyReceipt, OutBarrierReceipt);
		if (!bCompileComplete)
		{
			OutError.Set(TEXT("animation_rig_graph_compile_incomplete"));
			return false;
		}
		const FString GraphKey = TEXT("optimus_graph:") + Graph->GetPathName();
		const FString NodeKey = TEXT("optimus_node:") + Comment->GetPathName();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(GraphKey, *Graph, TEXT("graph")));
		Items.Add(ObjectItem(NodeKey, *Comment, TEXT("node")));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("graph_member:") + Comment->GetPathName(),
			TEXT("graph_member"), FString(), GraphKey, NodeKey, 0);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Deformer, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureValidateAndCaptureMeshDeformerCollection(
		UMeshDeformerCollection& Collection,
		UOptimusDeformer& Deformer,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		Collection.Modify();
		if (!Collection.MeshDeformers.Contains(&Deformer))
		{
			Collection.MeshDeformers.Add(&Deformer);
		}
		SetReceipts(Collection, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		const FString CollectionKey = TEXT("collection:") + Collection.GetPathName();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(CollectionKey, Collection, TEXT("collection")));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		for (int32 Index = 0; Index < Collection.MeshDeformers.Num(); ++Index)
		{
			const UMeshDeformer* Entry = Collection.MeshDeformers[Index].Get();
			if (!Entry)
			{
				continue;
			}
			const FString EntryKey = TEXT("collection_entry:") + Entry->GetPathName();
			Items.Add(ObjectItem(EntryKey, *Entry, TEXT("collection_entry")));
			Relations.Emplace(TEXT("collection_member:") + LexToString(Index),
				TEXT("collection_member"), Entry->GetPathName(), CollectionKey, EntryKey, Index);
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Collection, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureValidateAndCaptureIKRig(
		UIKRigDefinition& Rig,
		const FName GoalName,
		const FName GoalBone,
		const FName ChainName,
		const FName StartBone,
		const FName EndBone,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		if (!Controller || GoalName.IsNone() || GoalBone.IsNone()
			|| ChainName.IsNone() || StartBone.IsNone() || EndBone.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_ik_rig_controller_unavailable"));
			return false;
		}
		Rig.Modify();
		if (!Controller->GetGoal(GoalName)
			&& Controller->AddNewGoal(GoalName, GoalBone) != GoalName)
		{
			OutError.Set(TEXT("animation_rig_graph_ik_goal_add_failed"));
			return false;
		}
		const bool bChainExists = Controller->GetRetargetChains().ContainsByPredicate(
			[ChainName](const FBoneChain& Chain) { return Chain.ChainName == ChainName; });
		if (!bChainExists
			&& Controller->AddRetargetChain(ChainName, StartBone, EndBone, GoalName) != ChainName)
		{
			OutError.Set(TEXT("animation_rig_graph_ik_chain_add_failed"));
			return false;
		}
		if (Controller->GetNumSolvers() == 0
			&& Controller->AddSolver(FIKRigFullBodyIKSolver::StaticStruct()) == INDEX_NONE)
		{
			OutError.Set(TEXT("animation_rig_graph_ik_solver_add_failed"));
			return false;
		}
		Controller->BroadcastNeedsReinitialized();
		SetReceipts(Rig, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		const FString GoalKey = TEXT("ik_goal:") + GoalName.ToString();
		const FString ChainKey = TEXT("ik_chain:") + ChainName.ToString();
		const FInstancedStruct* Solver = Controller->GetSolverStructAtIndex(0);
		const FString SolverType = Solver && Solver->GetScriptStruct()
			? Solver->GetScriptStruct()->GetPathName() : TEXT("/Script/IKRig.IKRigSolverBase");
		const FString SolverKey = TEXT("ik_solver:0:") + SolverType;
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Emplace(GoalKey, TEXT("native_key"), GoalName.ToString(), FString(),
			ClassPath(Rig), TEXT("/Script/IKRig.IKRigEffectorGoal"), TEXT("ik_goal"));
		Items.Emplace(ChainKey, TEXT("native_key"), ChainName.ToString(), FString(),
			ClassPath(Rig), TEXT("/Script/IKRig.BoneChain"), TEXT("retarget_chain"));
		Items.Emplace(SolverKey, TEXT("native_composite"),
			TEXT("solver:0:") + SolverType, FString(), ClassPath(Rig), SolverType, TEXT("solver"));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("goal_binding:") + ChainName.ToString(), TEXT("chain_goal"),
			GoalName.ToString(), ChainKey, GoalKey, INDEX_NONE);
		Relations.Emplace(TEXT("solver_order:0"), TEXT("solver_order"), FString(),
			ChainKey, SolverKey, 0);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Rig, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureIKRigDefinition(
		UIKRigDefinition& Rig,
		const FName RetargetRoot,
		const TArray<FAnimationRigGraphIkGoalMutation>& Goals,
		const TArray<FAnimationRigGraphIkSolverMutation>& Solvers,
		const TArray<FAnimationRigGraphIkChainMutation>& Chains,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		if (!Controller || RetargetRoot.IsNone()
			|| !Controller->SetRetargetRoot(RetargetRoot))
		{
			OutError.Set(TEXT("animation_rig_graph_ik_rig_controller_unavailable"));
			return false;
		}
		Rig.Modify();
		for (const FAnimationRigGraphIkGoalMutation& Goal : Goals)
		{
			const bool bExists = Controller->GetGoal(Goal.Name) != nullptr;
			if (Goal.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveGoal(Goal.Name))
				{
					OutError.Set(TEXT("animation_rig_graph_ik_goal_remove_failed"),
						Goal.Name.ToString());
					return false;
				}
			}
			else if (bExists)
			{
				if (!Controller->SetGoalBone(Goal.Name, Goal.BoneName))
				{
					OutError.Set(TEXT("animation_rig_graph_ik_goal_update_failed"),
						Goal.Name.ToString());
					return false;
				}
			}
			else if (Controller->AddNewGoal(Goal.Name, Goal.BoneName) != Goal.Name)
			{
				OutError.Set(TEXT("animation_rig_graph_ik_goal_add_failed"),
					Goal.Name.ToString());
				return false;
			}
		}
		for (const FAnimationRigGraphIkSolverMutation& Solver : Solvers)
		{
			if (Solver.OrderIndex < 0)
			{
				OutError.Set(TEXT("animation_rig_graph_ik_solver_order_invalid"));
				return false;
			}
			if (Solver.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (Solver.OrderIndex >= Controller->GetNumSolvers()
					|| !Controller->RemoveSolver(Solver.OrderIndex))
				{
					OutError.Set(TEXT("animation_rig_graph_ik_solver_remove_failed"));
					return false;
				}
				continue;
			}
			UScriptStruct* const SolverType = LoadObject<UScriptStruct>(
				nullptr, *Solver.SolverClassRef);
			if (!SolverType)
			{
				OutError.Set(TEXT("animation_rig_graph_ik_solver_type_invalid"),
					Solver.SolverClassRef);
				return false;
			}
			if (Solver.OrderIndex < Controller->GetNumSolvers()
				&& (Solver.Intent == EAnimationRigGraphMutationIntent::Update
					|| Solver.Intent == EAnimationRigGraphMutationIntent::Replace)
				&& !Controller->RemoveSolver(Solver.OrderIndex))
			{
				OutError.Set(TEXT("animation_rig_graph_ik_solver_replace_failed"));
				return false;
			}
			const int32 AddedIndex = Controller->AddSolver(SolverType);
			const int32 TargetIndex = FMath::Min(Solver.OrderIndex, AddedIndex);
			if (AddedIndex == INDEX_NONE
				|| (AddedIndex != TargetIndex
					&& !Controller->MoveSolverInStack(AddedIndex, TargetIndex)))
			{
				OutError.Set(TEXT("animation_rig_graph_ik_solver_add_failed"));
				return false;
			}
		}
		for (const FAnimationRigGraphIkChainMutation& Chain : Chains)
		{
			const bool bExists = Controller->GetRetargetChains().ContainsByPredicate(
				[&Chain](const FBoneChain& Existing)
				{
					return Existing.ChainName == Chain.Name;
				});
			if (Chain.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveRetargetChain(Chain.Name))
				{
					OutError.Set(TEXT("animation_rig_graph_ik_chain_remove_failed"),
						Chain.Name.ToString());
					return false;
				}
				continue;
			}
			if (bExists)
			{
				if (!Controller->SetRetargetChainStartBone(Chain.Name, Chain.StartBone)
					|| !Controller->SetRetargetChainEndBone(Chain.Name, Chain.EndBone)
					|| !Controller->SetRetargetChainGoal(Chain.Name, Chain.GoalName))
				{
					OutError.Set(TEXT("animation_rig_graph_ik_chain_update_failed"),
						Chain.Name.ToString());
					return false;
				}
			}
			else if (Controller->AddRetargetChain(
				Chain.Name, Chain.StartBone, Chain.EndBone, Chain.GoalName) != Chain.Name)
			{
				OutError.Set(TEXT("animation_rig_graph_ik_chain_add_failed"),
					Chain.Name.ToString());
				return false;
			}
		}
		Controller->BroadcastNeedsReinitialized();
		SetReceipts(Rig, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		for (UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
		{
			if (!Goal)
			{
				continue;
			}
			Items.Emplace(TEXT("ik_goal:") + Goal->GoalName.ToString(), TEXT("native_key"),
				Goal->GoalName.ToString(), FString(), ClassPath(Rig),
				TEXT("/Script/IKRig.IKRigEffectorGoal"), TEXT("ik_goal"));
		}
		for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index)
		{
			const FInstancedStruct* Solver = Controller->GetSolverStructAtIndex(Index);
			const FString Type = Solver && Solver->GetScriptStruct()
				? Solver->GetScriptStruct()->GetPathName() : FString();
			Items.Emplace(FString::Printf(TEXT("ik_solver:%d"), Index),
				TEXT("native_composite"), FString::Printf(TEXT("solver:%d:%s"),
					Index, *Type), FString(), ClassPath(Rig), Type, TEXT("solver"));
		}
		for (const FBoneChain& Chain : Controller->GetRetargetChains())
		{
			const FString ChainKey = TEXT("ik_chain:") + Chain.ChainName.ToString();
			Items.Emplace(ChainKey, TEXT("native_key"), Chain.ChainName.ToString(),
				FString(), ClassPath(Rig), TEXT("/Script/IKRig.BoneChain"),
				TEXT("retarget_chain"));
			if (!Chain.IKGoalName.IsNone())
			{
				Relations.Emplace(TEXT("goal_binding:") + Chain.ChainName.ToString(),
					TEXT("chain_goal"), Chain.IKGoalName.ToString(), ChainKey,
					TEXT("ik_goal:") + Chain.IKGoalName.ToString(), INDEX_NONE);
			}
		}
		OutTopology = FAnimationRigGraphTopology(
			MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(
			Rig, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureIKRigAutoGenerate(
		UIKRigDefinition& Rig,
		USkeletalMesh& SkeletalMesh,
		const FString& Preset,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		if (!Controller
			|| (Controller->GetSkeletalMesh() != &SkeletalMesh
				&& !Controller->SetSkeletalMesh(&SkeletalMesh)))
		{
			OutError.Set(TEXT("animation_rig_graph_ik_rig_mesh_invalid"));
			return false;
		}
		if (Preset == TEXT("humanoid"))
		{
			if (!Controller->ApplyAutoGeneratedRetargetDefinition()
				|| !Controller->ApplyAutoFBIK())
			{
				OutError.Set(TEXT("animation_rig_graph_ik_auto_generate_failed"), Preset);
				return false;
			}
		}
		else if (Preset != TEXT("custom"))
		{
			OutError.Set(TEXT("animation_rig_graph_ik_auto_preset_unsupported"), Preset);
			return false;
		}
		Controller->BroadcastNeedsReinitialized();
		SetReceipts(Rig, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(TEXT("skeletal_mesh:") + SkeletalMesh.GetPathName(),
			SkeletalMesh, TEXT("skeletal_mesh")));
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Rig, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryConfigureValidateAndCaptureIKRetargeter(
		UIKRetargeter& Retargeter,
		UIKRigDefinition& SourceRig,
		UIKRigDefinition& TargetRig,
		const FName TargetChainName,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
		if (!Controller || TargetChainName.IsNone())
		{
			OutError.Set(TEXT("animation_rig_graph_retargeter_controller_unavailable"));
			return false;
		}
		Retargeter.Modify();
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, &SourceRig);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, &TargetRig);
		if (!Controller->GetChainMapping(NAME_None)
			&& Controller->AddRetargetOp(FIKRetargetFKChainsOp::StaticStruct()) == INDEX_NONE)
		{
			OutError.Set(TEXT("animation_rig_graph_retarget_mapping_op_add_failed"));
			return false;
		}
		Controller->CleanAsset();
		Controller->AutoMapChains(EAutoMapChainType::Exact, true);
		const FRetargetChainMapping* ChainMapping = Controller->GetChainMapping(NAME_None);
		if (Controller->GetIKRig(ERetargetSourceOrTarget::Source) != &SourceRig
			|| Controller->GetIKRig(ERetargetSourceOrTarget::Target) != &TargetRig
			|| !ChainMapping
			|| !ChainMapping->IsReady()
			|| !ChainMapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target)
			|| ChainMapping->GetChainMappedTo(
				TargetChainName, ERetargetSourceOrTarget::Target) != TargetChainName)
		{
			OutError.Set(TEXT("animation_rig_graph_retarget_mapping_missing"));
			return false;
		}
		SetReceipts(Retargeter, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		const FString SourceKey = TEXT("source_rig:") + SourceRig.GetPathName();
		const FString TargetKey = TEXT("target_rig:") + TargetRig.GetPathName();
		const FString MappingKey = TEXT("retarget_mapping:") + TargetChainName.ToString();
		const FString SettingsKey = TEXT("retarget_setting:chain:") + TargetChainName.ToString();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(SourceKey, SourceRig, TEXT("source_ik_rig")));
		Items.Add(ObjectItem(TargetKey, TargetRig, TEXT("target_ik_rig")));
		Items.Emplace(MappingKey, TEXT("native_key"), TargetChainName.ToString(), FString(),
			ClassPath(Retargeter), TEXT("/Script/IKRig.RetargetChainSettings"), TEXT("chain_mapping"));
		Items.Emplace(SettingsKey, TEXT("native_composite"),
			TEXT("chain:") + TargetChainName.ToString(), FString(), ClassPath(Retargeter),
			TEXT("/Script/IKRig.TargetChainSettings"), TEXT("setting_scope"));
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("source_rig_binding"), TEXT("source_rig"), SourceRig.GetPathName(),
			MappingKey, SourceKey, INDEX_NONE);
		Relations.Emplace(TEXT("target_rig_binding"), TEXT("target_rig"), TargetRig.GetPathName(),
			MappingKey, TargetKey, INDEX_NONE);
		Relations.Emplace(TEXT("setting_scope:") + TargetChainName.ToString(), TEXT("setting_scope"),
			TargetChainName.ToString(), MappingKey, SettingsKey, INDEX_NONE);
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, MoveTemp(Relations));
		return FinalizeTopologyCapture(Retargeter, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureIKRetargeterDefinition(
		UIKRetargeter& Retargeter,
		UIKRigDefinition& SourceRig,
		UIKRigDefinition& TargetRig,
		const TArray<FAnimationRigGraphRetargetMappingMutation>& Mappings,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
		if (!Controller || Mappings.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_retargeter_controller_unavailable"));
			return false;
		}
		Retargeter.Modify();
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, &SourceRig);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, &TargetRig);
		Controller->CleanAsset();
		for (const FAnimationRigGraphRetargetMappingMutation& Mapping : Mappings)
		{
			if (Mapping.OperationRef != TEXT("/Script/IKRig.IKRetargetFKChainsOp")
				|| Mapping.OrderIndex != 0)
			{
				OutError.Set(TEXT("animation_rig_graph_retarget_operation_unsupported"),
					Mapping.OperationRef);
				return false;
			}
			if (!Controller->GetChainMapping(NAME_None)
				&& Controller->AddRetargetOp(FIKRetargetFKChainsOp::StaticStruct()) == INDEX_NONE)
			{
				OutError.Set(TEXT("animation_rig_graph_retarget_mapping_op_add_failed"));
				return false;
			}
			const FName SourceChain =
				Mapping.Intent == EAnimationRigGraphMutationIntent::Delete
					? NAME_None : Mapping.SourceChain;
			if (!Controller->SetSourceChain(SourceChain, Mapping.TargetChain, NAME_None))
			{
				OutError.Set(TEXT("animation_rig_graph_retarget_mapping_update_failed"),
					Mapping.TargetChain.ToString());
				return false;
			}
		}
		Controller->CleanAsset();
		const FRetargetChainMapping* ChainMapping = Controller->GetChainMapping(NAME_None);
		if (Controller->GetIKRig(ERetargetSourceOrTarget::Source) != &SourceRig
			|| Controller->GetIKRig(ERetargetSourceOrTarget::Target) != &TargetRig
			|| !ChainMapping || !ChainMapping->IsReady())
		{
			OutError.Set(TEXT("animation_rig_graph_retarget_mapping_missing"));
			return false;
		}
		SetReceipts(Retargeter, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(TEXT("source_rig:") + SourceRig.GetPathName(),
			SourceRig, TEXT("source_ik_rig")));
		Items.Add(ObjectItem(TEXT("target_rig:") + TargetRig.GetPathName(),
			TargetRig, TEXT("target_ik_rig")));
		for (const FAnimationRigGraphRetargetMappingMutation& Mapping : Mappings)
		{
			Items.Emplace(TEXT("retarget_mapping:") + Mapping.TargetChain.ToString(),
				TEXT("native_key"), Mapping.TargetChain.ToString(), FString(),
				ClassPath(Retargeter), TEXT("/Script/IKRig.RetargetChainSettings"),
				TEXT("chain_mapping"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Retargeter, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryApplyAndCaptureIKRetargeterSettings(
		UIKRetargeter& Retargeter,
		const TArray<FAnimationRigGraphRetargetSettingMutation>& Mutations,
		FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
		FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
		FAnimationRigGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutError.Reset();
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
		UIKRetargetFKChainsController* OpController = Controller
			? Cast<UIKRetargetFKChainsController>(Controller->GetOpController(0)) : nullptr;
		if (!Controller || !OpController || Mutations.IsEmpty())
		{
			OutError.Set(TEXT("animation_rig_graph_retarget_settings_unavailable"));
			return false;
		}
		Retargeter.Modify();
		FIKRetargetFKChainsOpSettings Settings = OpController->GetSettings();
		for (const FAnimationRigGraphRetargetSettingMutation& Mutation : Mutations)
		{
			if (Mutation.Scope != TEXT("chain") || Mutation.ChainName.IsNone())
			{
				OutError.Set(TEXT("animation_rig_graph_retarget_setting_scope_unsupported"),
					Mutation.Scope);
				return false;
			}
			FRetargetFKChainSettings* Chain = Settings.ChainsToRetarget.FindByPredicate(
				[&Mutation](const FRetargetFKChainSettings& Existing)
				{
					return Existing.TargetChainName == Mutation.ChainName;
				});
			if (!Chain)
			{
				Settings.ChainsToRetarget.Emplace(Mutation.ChainName);
				Chain = &Settings.ChainsToRetarget.Last();
			}
			if (Mutation.SettingName == TEXT("RotationMode"))
			{
				const int64 Value = StaticEnum<EFKChainRotationMode>()->GetValueByNameString(
					Mutation.Value);
				if (Value == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						Mutation.Value);
					return false;
				}
				Chain->RotationMode = static_cast<EFKChainRotationMode>(Value);
			}
			else if (Mutation.SettingName == TEXT("TranslationMode"))
			{
				const int64 Value = StaticEnum<EFKChainTranslationMode>()->GetValueByNameString(
					Mutation.Value);
				if (Value == INDEX_NONE)
				{
					OutError.Set(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						Mutation.Value);
					return false;
				}
				Chain->TranslationMode = static_cast<EFKChainTranslationMode>(Value);
			}
			else if (Mutation.SettingName == TEXT("RotationAlpha"))
			{
				if (!LexTryParseString(Chain->RotationAlpha, *Mutation.Value))
				{
					OutError.Set(TEXT("animation_rig_graph_retarget_setting_value_invalid"));
					return false;
				}
			}
			else if (Mutation.SettingName == TEXT("TranslationAlpha"))
			{
				if (!LexTryParseString(Chain->TranslationAlpha, *Mutation.Value))
				{
					OutError.Set(TEXT("animation_rig_graph_retarget_setting_value_invalid"));
					return false;
				}
			}
			else if (Mutation.SettingName == TEXT("EnableFK"))
			{
				if (!LexTryParseString(Chain->EnableFK, *Mutation.Value))
				{
					OutError.Set(TEXT("animation_rig_graph_retarget_setting_value_invalid"));
					return false;
				}
			}
			else
			{
				OutError.Set(TEXT("animation_rig_graph_retarget_setting_unsupported"),
					Mutation.SettingName);
				return false;
			}
		}
		OpController->SetSettings(Settings);
		Controller->CleanAsset();
		SetReceipts(Retargeter, EAnimationRigGraphBarrierKind::Validate, true, 0,
			OutApplyReceipt, OutBarrierReceipt);
		TArray<FAnimationRigGraphItemIdentity> Items;
		for (const FAnimationRigGraphRetargetSettingMutation& Mutation : Mutations)
		{
			Items.Emplace(TEXT("retarget_setting:") + Mutation.Scope + TEXT(":")
				+ Mutation.ChainName.ToString() + TEXT(":") + Mutation.SettingName,
				TEXT("native_composite"), Mutation.Value, FString(),
				ClassPath(Retargeter), TEXT("/Script/IKRig.RetargetFKChainSettings"),
				TEXT("setting"));
		}
		OutTopology = FAnimationRigGraphTopology(MoveTemp(Items), {}, {});
		return FinalizeTopologyCapture(
			Retargeter, OutTopology, OutRevision, OutEvidenceId, OutError);
	}

	bool FAnimationRigGraphNativeHook::TryResolveConcreteTypeSupport(
		const FString& FamilyId,
		const FString& NativeClassPath,
		FAnimationRigGraphConcreteTypeSupport& OutSupport,
		FAnimationRigGraphAdapterError& OutError) const
	{
		OutSupport = {};
		OutError.Reset();
		if (FamilyId != TEXT("deformer_source_binding_mutation")
			&& FamilyId != TEXT("ml_deformer_configuration_mutation"))
		{
			OutError.Set(TEXT("animation_rig_graph_concrete_type_required"), FamilyId);
			return false;
		}
		OutSupport.FamilyId = FamilyId;
		OutSupport.NativeClassPath = NativeClassPath;
		OutError.Set(TEXT("animation_rig_graph_concrete_type_unavailable"), NativeClassPath);
		return false;
	}
}
