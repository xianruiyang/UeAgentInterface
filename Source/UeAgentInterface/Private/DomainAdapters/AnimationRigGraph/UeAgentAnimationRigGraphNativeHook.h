#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphAdapter.h"

class UAnimBlueprint;
class UAnimSequence;
class UBlendSpace;
class UControlRigBlueprint;
class UControlRigShapeLibrary;
class UIKRigDefinition;
class UIKRetargeter;
class UMeshDeformerCollection;
class UMaterial;
class UOptimusDeformer;
class USkeleton;
class UStaticMesh;

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	enum class EAnimationRigGraphMutationIntent : uint8
	{
		Create,
		Update,
		Replace,
		Delete,
	};

	struct FAnimationRigGraphBlendAxisMutation final
	{
		int32 AxisIndex = INDEX_NONE;
		FName AxisName;
		double Minimum = 0.0;
		double Maximum = 0.0;
		int32 GridDivisions = 0;
	};

	struct FAnimationRigGraphBlendSampleMutation final
	{
		UAnimSequence* Animation = nullptr;
		FVector Coordinates = FVector::ZeroVector;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphSkeletonBoneMutation final
	{
		FName BoneName;
		FName ParentBone;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphSkeletonCompatibilityMutation final
	{
		USkeleton* CompatibleSkeleton = nullptr;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphSkeletonVirtualBoneMutation final
	{
		FName Name;
		FName SourceBone;
		FName TargetBone;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphControlMutation final
	{
		FName Name;
		FName ParentBone;
		FString ControlTypeRef;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphShapeMutation final
	{
		FName Name;
		UStaticMesh* Mesh = nullptr;
		UMaterial* Material = nullptr;
		int32 OrderIndex = INDEX_NONE;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphIkGoalMutation final
	{
		FName Name;
		FName BoneName;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphIkSolverMutation final
	{
		FString SolverClassRef;
		int32 OrderIndex = INDEX_NONE;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphIkChainMutation final
	{
		FName Name;
		FName StartBone;
		FName EndBone;
		FName GoalName;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphRetargetMappingMutation final
	{
		FName SourceChain;
		FName TargetChain;
		FString OperationRef;
		int32 OrderIndex = INDEX_NONE;
		EAnimationRigGraphMutationIntent Intent =
			EAnimationRigGraphMutationIntent::Create;
	};

	struct FAnimationRigGraphRetargetSettingMutation final
	{
		FString Scope;
		FName ChainName;
		FString SettingName;
		FString Value;
	};

	class FAnimationRigGraphNativeMutationReceipt final
	{
	public:
		const FString& GetEvidenceId() const { return EvidenceId; }
		void SetEvidenceId(FString InEvidenceId) { EvidenceId = MoveTemp(InEvidenceId); }

	private:
		FString EvidenceId;
	};

	class FAnimationRigGraphBarrierReceipt final
	{
	public:
		EAnimationRigGraphBarrierKind GetBarrierKind() const { return BarrierKind; }
		bool IsComplete() const { return bComplete; }
		int32 GetErrorCount() const { return ErrorCount; }
		const FString& GetBarrierEvidenceId() const { return BarrierEvidenceId; }
		const FString& GetSettleEvidenceId() const { return SettleEvidenceId; }

	private:
		friend class FAnimationRigGraphNativeHook;
		EAnimationRigGraphBarrierKind BarrierKind = EAnimationRigGraphBarrierKind::Compile;
		bool bComplete = false;
		int32 ErrorCount = 0;
		FString BarrierEvidenceId;
		FString SettleEvidenceId;
	};

	class FAnimationRigGraphConcreteTypeSupport final
	{
	public:
		bool IsAvailable() const { return bAvailable; }
		const FString& GetFamilyId() const { return FamilyId; }
		const FString& GetNativeClassPath() const { return NativeClassPath; }

	private:
		friend class FAnimationRigGraphNativeHook;
		bool bAvailable = false;
		FString FamilyId;
		FString NativeClassPath;
	};

	class UEAGENTINTERFACE_API FAnimationRigGraphNativeHook final
	{
	public:
		bool TryCaptureOwnedAssetObject(
			UObject& Outer,
			const FString& AssetKind,
			UClass& AssetClass,
			FName ObjectName,
			UObject& Object,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphAssetIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryCreateAndCaptureOwnedAssetObject(
			UObject& Outer,
			const FString& AssetKind,
			UClass& AssetClass,
			FName ObjectName,
			UObject*& OutObject,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphAssetIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryEnsureCompileAndCaptureAnimBlueprint(
			UAnimBlueprint& Blueprint,
			USkeleton& Skeleton,
			FName GraphName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureValidateAndCaptureBlendSpace(
			UBlendSpace& BlendSpace,
			USkeleton& Skeleton,
			UAnimSequence& Sequence,
			FName AxisName,
			const FVector& SamplePosition,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureBlendSpaceDefinition(
			UBlendSpace& BlendSpace,
			USkeleton& Skeleton,
			const TArray<FAnimationRigGraphBlendAxisMutation>& Axes,
			const TArray<FAnimationRigGraphBlendSampleMutation>& Samples,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureSkeletonDefinition(
			USkeleton& Skeleton,
			const TArray<FAnimationRigGraphSkeletonBoneMutation>& Mutations,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureSkeletonCompatibility(
			USkeleton& Skeleton,
			const TArray<FAnimationRigGraphSkeletonCompatibilityMutation>& Mutations,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureSkeletonVirtualBone(
			USkeleton& Skeleton,
			const FAnimationRigGraphSkeletonVirtualBoneMutation& Mutation,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureRefreshAndCaptureSkeleton(
			USkeleton& Skeleton,
			USkeleton& CompatibleSkeleton,
			FName SourceBone,
			FName TargetBone,
			FName VirtualBoneName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryEnsureCompileAndCaptureControlRig(
			UControlRigBlueprint& Blueprint,
			FName ParentBoneName,
			FName ControlName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureControlRigDefinition(
			UControlRigBlueprint& Blueprint,
			const TArray<FAnimationRigGraphControlMutation>& Mutations,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureRefreshAndCaptureControlRigShapeLibrary(
			UControlRigShapeLibrary& Library,
			FName ShapeName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureControlRigShapeLibrary(
			UControlRigShapeLibrary& Library,
			const TArray<FAnimationRigGraphShapeMutation>& Mutations,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryEnsureCompileAndCaptureDeformerGraph(
			UOptimusDeformer& Deformer,
			FName CommentNodeName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureValidateAndCaptureMeshDeformerCollection(
			UMeshDeformerCollection& Collection,
			UOptimusDeformer& Deformer,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureValidateAndCaptureIKRig(
			UIKRigDefinition& Rig,
			FName GoalName,
			FName GoalBone,
			FName ChainName,
			FName StartBone,
			FName EndBone,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureIKRigDefinition(
			UIKRigDefinition& Rig,
			FName RetargetRoot,
			const TArray<FAnimationRigGraphIkGoalMutation>& Goals,
			const TArray<FAnimationRigGraphIkSolverMutation>& Solvers,
			const TArray<FAnimationRigGraphIkChainMutation>& Chains,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureIKRigAutoGenerate(
			UIKRigDefinition& Rig,
			USkeletalMesh& SkeletalMesh,
			const FString& Preset,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryConfigureValidateAndCaptureIKRetargeter(
			UIKRetargeter& Retargeter,
			UIKRigDefinition& SourceRig,
			UIKRigDefinition& TargetRig,
			FName TargetChainName,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureIKRetargeterDefinition(
			UIKRetargeter& Retargeter,
			UIKRigDefinition& SourceRig,
			UIKRigDefinition& TargetRig,
			const TArray<FAnimationRigGraphRetargetMappingMutation>& Mappings,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryApplyAndCaptureIKRetargeterSettings(
			UIKRetargeter& Retargeter,
			const TArray<FAnimationRigGraphRetargetSettingMutation>& Settings,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt,
			FAnimationRigGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError) const;

		bool TryResolveConcreteTypeSupport(
			const FString& FamilyId,
			const FString& NativeClassPath,
			FAnimationRigGraphConcreteTypeSupport& OutSupport,
			FAnimationRigGraphAdapterError& OutError) const;

	private:
		static void SetReceipts(
			const UObject& Subject,
			EAnimationRigGraphBarrierKind BarrierKind,
			bool bBarrierComplete,
			int32 ErrorCount,
			FAnimationRigGraphNativeMutationReceipt& OutApplyReceipt,
			FAnimationRigGraphBarrierReceipt& OutBarrierReceipt);
		static bool FinalizeTopologyCapture(
			const UObject& Subject,
			const FAnimationRigGraphTopology& Topology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FAnimationRigGraphAdapterError& OutError);
	};
}
