#include "Misc/AutomationTest.h"

// GC-009 rebuilds this source before and after production implementation.
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/MeshDeformerCollection.h"
#include "Animation/Skeleton.h"
#include "ControlRigBlueprint.h"
#include "ControlRigGizmoLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAnimationRigGraphNativeHookTest,
	"UeAgentInterface.DomainAdapters.AnimationRigGraph.E2.Native."
		"OwnedDomainMutationBarrierAndIndependentReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAnimationRigGraphNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::AnimationRigGraph;

	FAnimationRigGraphNativeHook Hook;
	FAnimationRigGraphAdapterError Error;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/AnimationRigGraph/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	UObject* CreatedObject = nullptr;
	FAnimationRigGraphNativeMutationReceipt AssetApply;
	FAnimationRigGraphBarrierReceipt AssetBarrier;
	FAnimationRigGraphAssetIdentity AssetIdentity;
	FString AssetRevision;
	FString AssetEvidence;
	TestTrue(TEXT("asset lifecycle refreshes and independently captures"),
		Hook.TryCreateAndCaptureOwnedAssetObject(
			*Package, TEXT("anim_blueprint"),
			*UAnimBlueprint::StaticClass(), TEXT("ABP_GC008_Created"),
			CreatedObject, AssetApply, AssetBarrier, AssetIdentity,
			AssetRevision, AssetEvidence, Error));
	TestNotNull(TEXT("created asset object exists"), CreatedObject);
	TestEqual(TEXT("asset lifecycle owns refresh barrier"),
		AssetBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Refresh);
	TestTrue(TEXT("asset identity validates"),
		TryValidateAnimationRigGraphAssetIdentity(AssetIdentity, Error));

	USkeleton* SkeletonTemplate = LoadObject<USkeleton>(nullptr,
		TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton."
			"SkeletalCube_Skeleton"));
	if (!TestNotNull(TEXT("real Skeleton template exists"), SkeletonTemplate))
	{
		Package->MarkAsGarbage();
		return false;
	}
	TStrongObjectPtr<USkeleton> Skeleton(DuplicateObject<USkeleton>(
		SkeletonTemplate, Package, TEXT("SK_GC008")));
	TStrongObjectPtr<USkeleton> CompatibleSkeleton(DuplicateObject<USkeleton>(
		SkeletonTemplate, Package, TEXT("SK_GC008_Compatible")));
	TStrongObjectPtr<UAnimSequence> Sequence(NewObject<UAnimSequence>(
		Package, TEXT("AS_GC008"), RF_Transactional));
	TStrongObjectPtr<UAnimSequence> MismatchedSequence(NewObject<UAnimSequence>(
		Package, TEXT("AS_GC008_Mismatched"), RF_Transactional));
	TStrongObjectPtr<UBlendSpace> BlendSpace(NewObject<UBlendSpace>(
		Package, TEXT("BS_GC008"), RF_Transactional));
	TStrongObjectPtr<UAnimBlueprint> AnimBlueprint(NewObject<UAnimBlueprint>(
		Package, TEXT("ABP_GC008"), RF_Transactional));
	if (!TestNotNull(TEXT("owned Skeleton exists"), Skeleton.Get())
		|| !TestNotNull(TEXT("owned compatible Skeleton exists"),
			CompatibleSkeleton.Get())
		|| !TestNotNull(TEXT("owned AnimSequence exists"), Sequence.Get())
		|| !TestNotNull(TEXT("owned mismatched AnimSequence exists"),
			MismatchedSequence.Get())
		|| !TestNotNull(TEXT("owned BlendSpace exists"), BlendSpace.Get())
		|| !TestNotNull(TEXT("owned AnimBlueprint exists"), AnimBlueprint.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}
	const FName SourceSkeletonBone =
		Skeleton->GetReferenceSkeleton().GetBoneName(0);
	if (!TestFalse(TEXT("owned Skeleton exposes a real root bone"),
		SourceSkeletonBone.IsNone()))
	{
		Package->MarkAsGarbage();
		return false;
	}
	Sequence->SetSkeleton(Skeleton.Get());
	IAnimationDataController& SequenceController = Sequence->GetController();
	SequenceController.InitializeModel();
	SequenceController.SetFrameRate(FFrameRate(30, 1), false);
	SequenceController.SetNumberOfFrames(FFrameNumber(1), false);
	SequenceController.NotifyPopulated();
	MismatchedSequence->SetSkeleton(CompatibleSkeleton.Get());
	IAnimationDataController& MismatchedController =
		MismatchedSequence->GetController();
	MismatchedController.InitializeModel();
	MismatchedController.SetFrameRate(FFrameRate(30, 1), false);
	MismatchedController.SetNumberOfFrames(FFrameNumber(1), false);
	MismatchedController.NotifyPopulated();

	FAnimationRigGraphNativeMutationReceipt AnimApply;
	FAnimationRigGraphBarrierReceipt AnimBarrier;
	FAnimationRigGraphTopology AnimTopology;
	FString AnimRevision;
	FString AnimEvidence;
	TestTrue(TEXT("AnimBlueprint graph mutates, compiles and captures"),
		Hook.TryEnsureCompileAndCaptureAnimBlueprint(
			*AnimBlueprint, *Skeleton, TEXT("AnimGraph_GC008"),
			AnimApply, AnimBarrier, AnimTopology,
			AnimRevision, AnimEvidence, Error));
	TestEqual(TEXT("AnimBlueprint owns compile barrier"),
		AnimBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Compile);
	TestTrue(TEXT("AnimBlueprint topology validates"),
		TryValidateAnimationRigGraphTopology(AnimTopology, Error));
	TestTrue(TEXT("AnimBlueprint captures a native graph node"),
		AnimTopology.GetItems().ContainsByPredicate(
			[](const FAnimationRigGraphItemIdentity& Item)
			{
				return Item.GetIdentitySource() == TEXT("native_guid")
					&& Item.GetSemanticRole() == TEXT("runtime_node")
					&& !Item.GetNativeCarrierTypeIdentity().IsEmpty()
					&& !Item.GetNativeTypeIdentity().IsEmpty();
			}));

	FAnimationRigGraphNativeMutationReceipt BlendApply;
	FAnimationRigGraphBarrierReceipt BlendBarrier;
	FAnimationRigGraphTopology BlendTopology;
	FString BlendRevision;
	FString BlendEvidence;
	FAnimationRigGraphNativeMutationReceipt MismatchApply;
	FAnimationRigGraphBarrierReceipt MismatchBarrier;
	FAnimationRigGraphTopology MismatchTopology;
	FString MismatchRevision;
	FString MismatchEvidence;
	TestFalse(TEXT("BlendSpace rejects a referenced sequence with another skeleton"),
		Hook.TryConfigureValidateAndCaptureBlendSpace(
			*BlendSpace, *Skeleton, *MismatchedSequence, TEXT("Speed"),
			FVector(300.0, 0.0, 0.0), MismatchApply, MismatchBarrier,
			MismatchTopology, MismatchRevision, MismatchEvidence, Error));
	TestEqual(TEXT("BlendSpace mismatch uses exact preflight error"),
		Error.GetCode(),
		FString(TEXT("animation_rig_graph_blend_sample_skeleton_mismatch")));
	TestTrue(TEXT("BlendSpace validates native axis/sample identity"),
		Hook.TryConfigureValidateAndCaptureBlendSpace(
			*BlendSpace, *Skeleton, *Sequence, TEXT("Speed"),
			FVector(600.0, 0.0, 0.0), BlendApply, BlendBarrier,
			BlendTopology, BlendRevision, BlendEvidence, Error));
	TestEqual(TEXT("BlendSpace owns validate barrier"),
		BlendBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Validate);
	TestTrue(TEXT("BlendSpace topology validates"),
		TryValidateAnimationRigGraphTopology(BlendTopology, Error));
	TestTrue(TEXT("BlendSpace sample declares native composite identity"),
		BlendTopology.GetItems().ContainsByPredicate(
			[](const FAnimationRigGraphItemIdentity& Item)
			{
				return Item.GetSemanticRole() == TEXT("blend_sample")
					&& Item.GetIdentitySource() == TEXT("native_composite");
			}));

	FAnimationRigGraphNativeMutationReceipt SkeletonApply;
	FAnimationRigGraphBarrierReceipt SkeletonBarrier;
	FAnimationRigGraphTopology SkeletonTopology;
	FString SkeletonRevision;
	FString SkeletonEvidence;
	TestTrue(TEXT("Skeleton refreshes bones compatibility and virtual bone"),
		Hook.TryConfigureRefreshAndCaptureSkeleton(
			*Skeleton, *CompatibleSkeleton, SourceSkeletonBone,
			TEXT("GC008_Foot"), TEXT("VB GC008_Root_Foot"),
			SkeletonApply, SkeletonBarrier,
			SkeletonTopology, SkeletonRevision, SkeletonEvidence, Error));
	TestEqual(TEXT("Skeleton owns refresh barrier"),
		SkeletonBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Refresh);
	TestTrue(TEXT("Skeleton topology validates"),
		TryValidateAnimationRigGraphTopology(SkeletonTopology, Error));

	TStrongObjectPtr<UControlRigBlueprint> ControlRig(
		NewObject<UControlRigBlueprint>(
			Package, TEXT("CR_GC008"), RF_Transactional));
	TStrongObjectPtr<UControlRigShapeLibrary> ShapeLibrary(
		NewObject<UControlRigShapeLibrary>(
			Package, TEXT("CRSL_GC008"), RF_Transactional));
	if (!TestNotNull(TEXT("owned ControlRig exists"), ControlRig.Get())
		|| !TestNotNull(TEXT("owned shape library exists"), ShapeLibrary.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}
	FAnimationRigGraphNativeMutationReceipt RigApply;
	FAnimationRigGraphBarrierReceipt RigBarrier;
	FAnimationRigGraphTopology RigTopology;
	FString RigRevision;
	FString RigEvidence;
	TestTrue(TEXT("ControlRig hierarchy/RigVM compiles and captures"),
		Hook.TryEnsureCompileAndCaptureControlRig(
			*ControlRig, TEXT("root"), TEXT("FootIK"),
			RigApply, RigBarrier, RigTopology,
			RigRevision, RigEvidence, Error));
	TestEqual(TEXT("ControlRig owns compile barrier"),
		RigBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Compile);
	TestTrue(TEXT("ControlRig topology validates"),
		TryValidateAnimationRigGraphTopology(RigTopology, Error));
	TestTrue(TEXT("ControlRig captures native RigElementKey"),
		RigTopology.GetItems().ContainsByPredicate(
			[](const FAnimationRigGraphItemIdentity& Item)
			{
				return Item.GetIdentitySource() == TEXT("native_key")
					&& Item.GetSemanticRole() == TEXT("control");
			}));

	FAnimationRigGraphNativeMutationReceipt ShapeApply;
	FAnimationRigGraphBarrierReceipt ShapeBarrier;
	FAnimationRigGraphTopology ShapeTopology;
	FString ShapeRevision;
	FString ShapeEvidence;
	TestTrue(TEXT("shape library refreshes ordered shape identity"),
		Hook.TryConfigureRefreshAndCaptureControlRigShapeLibrary(
			*ShapeLibrary, TEXT("Cube_GC008"), ShapeApply, ShapeBarrier,
			ShapeTopology, ShapeRevision, ShapeEvidence, Error));
	TestEqual(TEXT("shape library owns refresh barrier"),
		ShapeBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Refresh);
	TestTrue(TEXT("shape topology validates"),
		TryValidateAnimationRigGraphTopology(ShapeTopology, Error));

	TStrongObjectPtr<UOptimusDeformer> Deformer(NewObject<UOptimusDeformer>(
		Package, TEXT("DG_GC008"), RF_Transactional));
	TStrongObjectPtr<UMeshDeformerCollection> Collection(
		NewObject<UMeshDeformerCollection>(
			Package, TEXT("MDC_GC008"), RF_Transactional));
	if (!TestNotNull(TEXT("owned Optimus Deformer exists"), Deformer.Get())
		|| !TestNotNull(TEXT("owned MeshDeformerCollection exists"),
			Collection.Get()))
	{
		Package->MarkAsGarbage();
		return false;
	}
	if (!TestNotNull(TEXT("Optimus fixture owns its native primary binding"),
		Deformer->AddComponentBinding(
			nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName())))
	{
		Package->MarkAsGarbage();
		return false;
	}
	FAnimationRigGraphNativeMutationReceipt DeformerApply;
	FAnimationRigGraphBarrierReceipt DeformerBarrier;
	FAnimationRigGraphTopology DeformerTopology;
	FString DeformerRevision;
	FString DeformerEvidence;
	const bool bDeformerSucceeded = Hook.TryEnsureCompileAndCaptureDeformerGraph(
		*Deformer, TEXT("Comment_GC008"), DeformerApply,
		DeformerBarrier, DeformerTopology,
		DeformerRevision, DeformerEvidence, Error);
	TestTrue(TEXT("Optimus graph mutates, compiles and captures"),
		bDeformerSucceeded);
	if (!bDeformerSucceeded)
	{
		AddError(TEXT("Optimus exact failure: ") + Error.GetCode());
	}
	TestEqual(TEXT("Optimus graph owns compile barrier"),
		DeformerBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Compile);
	TestTrue(TEXT("Optimus topology validates"),
		TryValidateAnimationRigGraphTopology(DeformerTopology, Error));
	TestTrue(TEXT("Optimus node identity is native UObject path"),
		DeformerTopology.GetItems().ContainsByPredicate(
			[](const FAnimationRigGraphItemIdentity& Item)
			{
				return Item.GetSemanticRole() == TEXT("node")
					&& Item.GetIdentitySource()
						== TEXT("native_object_path");
			}));

	FAnimationRigGraphNativeMutationReceipt CollectionApply;
	FAnimationRigGraphBarrierReceipt CollectionBarrier;
	FAnimationRigGraphTopology CollectionTopology;
	FString CollectionRevision;
	FString CollectionEvidence;
	TestTrue(TEXT("MeshDeformerCollection validates ordered native entry"),
		Hook.TryConfigureValidateAndCaptureMeshDeformerCollection(
			*Collection, *Deformer, CollectionApply, CollectionBarrier,
			CollectionTopology, CollectionRevision,
			CollectionEvidence, Error));
	TestEqual(TEXT("MeshDeformerCollection owns validate barrier"),
		CollectionBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Validate);
	TestTrue(TEXT("MeshDeformerCollection topology validates"),
		TryValidateAnimationRigGraphTopology(CollectionTopology, Error));

	TStrongObjectPtr<UIKRigDefinition> SourceRig(NewObject<UIKRigDefinition>(
		Package, TEXT("IKR_Source_GC008"), RF_Transactional));
	TStrongObjectPtr<UIKRigDefinition> TargetRig(NewObject<UIKRigDefinition>(
		Package, TEXT("IKR_Target_GC008"), RF_Transactional));
	TStrongObjectPtr<UIKRetargeter> Retargeter(NewObject<UIKRetargeter>(
		Package, TEXT("RTG_GC008"), RF_Transactional));
	USkeletalMesh* IKRigMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("owned source IK Rig exists"), SourceRig.Get())
		|| !TestNotNull(TEXT("owned target IK Rig exists"), TargetRig.Get())
		|| !TestNotNull(TEXT("owned IK Retargeter exists"), Retargeter.Get())
		|| !TestNotNull(TEXT("real IK Rig mesh exists"), IKRigMesh))
	{
		Package->MarkAsGarbage();
		return false;
	}
	const FReferenceSkeleton& IKRigReferenceSkeleton = IKRigMesh->GetRefSkeleton();
	if (!TestTrue(TEXT("real IK Rig mesh exposes a native bone chain"),
		IKRigReferenceSkeleton.GetNum() >= 2))
	{
		Package->MarkAsGarbage();
		return false;
	}
	const FName IKChainStartBone = IKRigReferenceSkeleton.GetBoneName(0);
	const FName IKChainEndBone =
		IKRigReferenceSkeleton.GetBoneName(IKRigReferenceSkeleton.GetNum() - 1);
	UIKRigController* SourceRigController =
		UIKRigController::GetController(SourceRig.Get());
	UIKRigController* TargetRigController =
		UIKRigController::GetController(TargetRig.Get());
	UIKRetargeterController* RetargeterController =
		UIKRetargeterController::GetController(Retargeter.Get());
	if (!TestNotNull(TEXT("source IK Rig controller exists"), SourceRigController)
		|| !TestNotNull(TEXT("target IK Rig controller exists"), TargetRigController)
		|| !TestNotNull(TEXT("IK Retargeter controller exists"), RetargeterController)
		|| !TestTrue(TEXT("source IK Rig loads the native mesh hierarchy"),
			SourceRigController->SetSkeletalMesh(IKRigMesh))
		|| !TestTrue(TEXT("target IK Rig loads the native mesh hierarchy"),
			TargetRigController->SetSkeletalMesh(IKRigMesh)))
	{
		Package->MarkAsGarbage();
		return false;
	}
	TestNull(TEXT("new IK Retargeter starts without a chain-mapping op"),
		RetargeterController->GetChainMapping(NAME_None));
	FAnimationRigGraphNativeMutationReceipt SourceRigApply;
	FAnimationRigGraphBarrierReceipt SourceRigBarrier;
	FAnimationRigGraphTopology SourceRigTopology;
	FString SourceRigRevision;
	FString SourceRigEvidence;
	TestTrue(TEXT("IK Rig validates goal/chain/solver-order identity"),
		Hook.TryConfigureValidateAndCaptureIKRig(
			*SourceRig, TEXT("GC008_Goal"), IKChainEndBone,
			TEXT("GC008_Chain"), IKChainStartBone, IKChainEndBone, SourceRigApply,
			SourceRigBarrier, SourceRigTopology,
			SourceRigRevision, SourceRigEvidence, Error));
	TestEqual(TEXT("IK Rig owns validate barrier"),
		SourceRigBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Validate);
	TestTrue(TEXT("IK Rig topology validates"),
		TryValidateAnimationRigGraphTopology(SourceRigTopology, Error));

	FAnimationRigGraphNativeMutationReceipt TargetRigApply;
	FAnimationRigGraphBarrierReceipt TargetRigBarrier;
	FAnimationRigGraphTopology TargetRigTopology;
	FString TargetRigRevision;
	FString TargetRigEvidence;
	TestTrue(TEXT("target IK Rig has matching native chain"),
		Hook.TryConfigureValidateAndCaptureIKRig(
			*TargetRig, TEXT("GC008_Goal"), IKChainEndBone,
			TEXT("GC008_Chain"), IKChainStartBone, IKChainEndBone, TargetRigApply,
			TargetRigBarrier, TargetRigTopology,
			TargetRigRevision, TargetRigEvidence, Error));

	FAnimationRigGraphNativeMutationReceipt RetargetApply;
	FAnimationRigGraphBarrierReceipt RetargetBarrier;
	FAnimationRigGraphTopology RetargetTopology;
	FString RetargetRevision;
	FString RetargetEvidence;
	TestTrue(TEXT("IK Retargeter validates rigs/mapping/settings identity"),
		Hook.TryConfigureValidateAndCaptureIKRetargeter(
			*Retargeter, *SourceRig, *TargetRig, TEXT("GC008_Chain"),
			RetargetApply, RetargetBarrier, RetargetTopology,
			RetargetRevision, RetargetEvidence, Error));
	TestEqual(TEXT("IK Retargeter owns validate barrier"),
		RetargetBarrier.GetBarrierKind(),
		EAnimationRigGraphBarrierKind::Validate);
	TestTrue(TEXT("IK Retargeter topology validates"),
		TryValidateAnimationRigGraphTopology(RetargetTopology, Error));
	const FRetargetChainMapping* NativeChainMapping =
		RetargeterController->GetChainMapping(NAME_None);
	TestTrue(TEXT("IK Retargeter native mapping is independently readable"),
		NativeChainMapping
			&& NativeChainMapping->IsReady()
			&& NativeChainMapping->GetChainMappedTo(
				TEXT("GC008_Chain"), ERetargetSourceOrTarget::Target)
				== TEXT("GC008_Chain"));

	FAnimationRigGraphConcreteTypeSupport UnsupportedSupport;
	TestFalse(TEXT("unknown Deformer Source class has no reflection fallback"),
		Hook.TryResolveConcreteTypeSupport(
			TEXT("deformer_source_binding_mutation"),
			TEXT("/Script/UAI.DoesNotExist"),
			UnsupportedSupport, Error));
	TestEqual(TEXT("unknown concrete class exact error"), Error.GetCode(),
		FString(TEXT("animation_rig_graph_concrete_type_unavailable")));
	TestFalse(TEXT("unsupported type is not marked available"),
		UnsupportedSupport.IsAvailable());

	TestFalse(TEXT("apply evidence differs from independent readback"),
		AnimApply.GetEvidenceId() == AnimEvidence);
	TestFalse(TEXT("barrier evidence differs from readback"),
		AnimBarrier.GetBarrierEvidenceId() == AnimEvidence);
	TestFalse(TEXT("revision and evidence are non-empty"),
		AnimRevision.IsEmpty() || AnimEvidence.IsEmpty());

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
