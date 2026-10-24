#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphNativeHook.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentSkeletonVirtualBones.h"

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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Misc/ScopeExit.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodeLink.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
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
#include "Engine/SkeletalMesh.h"
#include "UObject/UObjectGlobals.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	namespace
	{
		void LocateRigInput(FAnimationRigGraphAdapterError& Error, const FString& InputPointer,
			const FString& RelativePointer = FString())
		{
			if (!Error.IsValid() || InputPointer.IsEmpty() || Error.SourceKind == TEXT("native_result")
				|| Error.SourceKind == TEXT("command_input") || !Error.Source.SourceFile.IsEmpty()
				|| !Error.Source.JsonPath.IsEmpty() || !Error.SemanticPointer.IsEmpty()) return;
			Error.Source.JsonPath = RelativePointer.IsEmpty() ? InputPointer
				: InputPointer == TEXT("/") ? RelativePointer : InputPointer + RelativePointer;
			Error.SourceKind = TEXT("related_input");
		}

		FAnimationRigGraphAdapterError RigNativeFailure(const TCHAR* Code, const FString& Reason,
			const UObject& Target, const FString& SubjectId = FString(),
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr,
			const FString& Field = FString(), const FString& InputPointer = FString())
		{
			auto Error = FAnimationRigGraphAdapterError::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = InputPointer.IsEmpty() ? TEXT("native_result") : TEXT("related_input");
			const UBlueprint* Blueprint = Cast<UBlueprint>(&Target);
			Error.TargetPath = Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : Target.GetPathName();
			Error.SubjectId = SubjectId;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.PropertyPath = Field;
			// Only an explicit authored mapping allows the producer's field to locate input.
			LocateRigInput(Error, InputPointer, Field);
			return Error;
		}

		UeAgentIKAssetOps::FRetargetMappingDiagnosticSource RetargetMappingSource(
			const FAnimationRigGraphRetargetMappingMutation& Mutation, const UIKRetargeter& Target)
		{
			UeAgentIKAssetOps::FRetargetMappingDiagnosticSource Source;
			Source.TargetPath = Target.GetPathName();
			Source.Mapping.JsonPath = Mutation.InputPointer;
			Source.TargetChain = Source.Mapping;
			Source.SourceChain = Source.Mapping;
			if (!Mutation.InputPointer.IsEmpty() && Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
				Source.SourceChain.JsonPath = Mutation.InputPointer == TEXT("/")
					? TEXT("/source_chain") : Mutation.InputPointer + TEXT("/source_chain");
			return Source;
		}

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

		bool TryControlType(const FString& Ref, ERigControlType& OutType,
			const UControlRigBlueprint& Blueprint, const FName ControlName, const FString& InputPointer,
			FAnimationRigGraphAdapterError& OutError)
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
				TArray<FString> Names;
				Types.GetKeys(Names);
				Names.Sort();
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const FString& Name : Names) Choices.Add(MakeShared<FJsonValueString>(Name));
				OutError = RigNativeFailure(TEXT("animation_rig_graph_control_definition_invalid"),
					FString::Printf(TEXT("Control '%s' uses an unsupported control type."), *ControlName.ToString()), Blueprint,
					TEXT("rig_element:control:") + ControlName.ToString(), MakeShared<FJsonValueString>(Ref),
					MakeShared<FJsonValueArray>(MoveTemp(Choices)), TEXT("/control_type_ref"), InputPointer);
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
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Subject.GetPathName();
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
            const bool bKindMissing = AssetKind.IsEmpty();
            const bool bNameMissing = ObjectName.IsNone();
            OutError = RigNativeFailure(TEXT("animation_rig_graph_asset_create_invalid"),
                bKindMissing ? TEXT("Asset creation requires a non-empty asset kind.")
                    : bNameMissing ? TEXT("Asset creation requires an object name.")
                    : AssetClass.HasAnyClassFlags(CLASS_Abstract) ? TEXT("The requested asset class is abstract.")
                    : TEXT("The requested asset class is deprecated."),
                Outer, ObjectName.ToString(),
                MakeShared<FJsonValueString>(bKindMissing ? AssetKind : bNameMissing ? ObjectName.ToString() : AssetClass.GetPathName()),
                MakeShared<FJsonValueString>(bKindMissing ? TEXT("non-empty asset kind")
                    : bNameMissing ? TEXT("non-None object name") : TEXT("concrete, non-deprecated class")),
                bKindMissing ? TEXT("asset_kind") : bNameMissing ? TEXT("object_name") : TEXT("class"));
			return false;
		}
		OutObject = NewObject<UObject>(&Outer, &AssetClass, ObjectName, RF_Transactional);
		if (!OutObject)
		{
            OutError = RigNativeFailure(TEXT("animation_rig_graph_asset_create_failed"),
                TEXT("Unreal did not create the requested owned asset object."), Outer, ObjectName.ToString(),
                nullptr, MakeShared<FJsonValueString>(AssetClass.GetPathName()), TEXT("created_object"));
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_asset_identity_mismatch"),
				AssetKind.IsEmpty() ? TEXT("The captured animation asset kind is empty.")
					: ObjectName.IsNone() ? TEXT("The expected animation asset name is empty.")
					: Object.GetOuter() != &Outer ? TEXT("The captured animation asset belongs to a different outer object than requested.")
					: Object.GetFName() != ObjectName ? TEXT("The captured animation asset has a different name than requested.")
					: TEXT("The captured animation asset has a different native class than requested."), Object);
			if (AssetKind.IsEmpty() || ObjectName.IsNone())
			{
				OutError.PropertyPath = AssetKind.IsEmpty() ? TEXT("/asset_kind") : TEXT("/object_name");
				OutError.ActualValue = MakeShared<FJsonValueString>(FString());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty value"));
			}
			else if (Object.GetOuter() != &Outer)
			{
				OutError.PropertyPath = TEXT("/outer");
				OutError.ActualValuePresent = Object.GetOuter() != nullptr;
				if (Object.GetOuter()) OutError.ActualValue = MakeShared<FJsonValueString>(Object.GetOuter()->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Outer.GetPathName());
			}
			else
			{
				const bool bNameMismatch = Object.GetFName() != ObjectName;
				OutError.PropertyPath = bNameMismatch ? TEXT("/object_name") : TEXT("/native_class");
				OutError.ActualValue = MakeShared<FJsonValueString>(bNameMismatch ? Object.GetName() : Object.GetClass()->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(bNameMismatch ? ObjectName.ToString() : AssetClass.GetPathName());
			}
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
		FAnimationRigGraphAdapterError& OutError,
		const FString& GraphNameInputPointer) const
	{
		OutError.Reset();
		if (GraphName.IsNone())
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_graph_name_missing"),
				TEXT("The animation graph name must not resolve to None."), Blueprint, FString(),
				MakeShared<FJsonValueString>(GraphName.ToString()), MakeShared<FJsonValueString>(TEXT("a non-None graph name")),
				FString(), GraphNameInputPointer);
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
				OutError = RigNativeFailure(TEXT("animation_rig_graph_anim_graph_create_failed"),
					FString::Printf(TEXT("Unreal could not create animation graph '%s'."), *GraphName.ToString()), Blueprint,
					FString(), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(UAnimationGraph::StaticClass()->GetPathName()),
					FString(), GraphNameInputPointer);
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_anim_root_create_failed"),
				FString::Printf(TEXT("Animation graph '%s' has no root node after default node creation."), *GraphName.ToString()),
				Blueprint, FString(), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one animation root node")), FString(), GraphNameInputPointer);
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
			OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("animation_rig_graph_compile_incomplete"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), CompilerLog, &Blueprint);
			OutError.SourceKind = TEXT("native_result");
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
            OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_axis_missing"),
                TEXT("A BlendSpace axis requires a non-None name."), BlendSpace, TEXT("blend_axis:0"),
                MakeShared<FJsonValueString>(AxisName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None name")), TEXT("axis_name"));
			return false;
		}
		if (Sequence.GetSkeleton() != &Skeleton)
		{
            OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_sample_skeleton_mismatch"),
                TEXT("The sample sequence uses a different skeleton than the requested BlendSpace skeleton."),
                BlendSpace, Sequence.GetPathName(),
                Sequence.GetSkeleton() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Sequence.GetSkeleton()->GetPathName())) : nullptr,
                MakeShared<FJsonValueString>(Skeleton.GetPathName()), TEXT("sample.skeleton"));
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
            OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_sample_add_failed"),
                FString::Printf(TEXT("Unreal rejected sample sequence '%s' at position (%.17g, %.17g, %.17g)."),
                    *Sequence.GetPathName(), SamplePosition.X, SamplePosition.Y, SamplePosition.Z), BlendSpace, Sequence.GetPathName(),
                MakeShared<FJsonValueNumber>(SampleIndex), MakeShared<FJsonValueString>(TEXT("valid sample index")), TEXT("sample_index"));
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_axes_missing"),
				TEXT("The BlendSpace mutation contains no axes."), BlendSpace, FString(),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one axis")));
			return false;
		}
		BlendSpace.Modify();
		BlendSpace.SetSkeleton(&Skeleton);
		TSet<int32> AxisIndices;
		for (const FAnimationRigGraphBlendAxisMutation& AxisMutation : Axes)
		{
			auto InvalidAxis = [&](const TCHAR* Field, const FString& Reason,
				const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_axis_invalid"), Reason, BlendSpace,
					FString(), Actual, MakeShared<FJsonValueString>(Expected), Field, AxisMutation.InputPointer);
				return false;
			};
			if (AxisMutation.AxisIndex < 0 || AxisMutation.AxisIndex > 1)
				return InvalidAxis(TEXT("/axis_index"), TEXT("The BlendSpace axis index must be 0 or 1."),
					MakeShared<FJsonValueNumber>(AxisMutation.AxisIndex), TEXT("0 or 1"));
			if (AxisMutation.AxisName.IsNone())
				return InvalidAxis(TEXT("/axis_name"), TEXT("The BlendSpace axis name must not resolve to None."),
					MakeShared<FJsonValueString>(AxisMutation.AxisName.ToString()), TEXT("a non-None axis name"));
			if (!FMath::IsFinite(AxisMutation.Minimum))
				return InvalidAxis(TEXT("/minimum"), TEXT("The BlendSpace axis minimum must be finite."),
					MakeShared<FJsonValueString>(LexToString(AxisMutation.Minimum)), TEXT("a finite number"));
			if (!FMath::IsFinite(AxisMutation.Maximum))
				return InvalidAxis(TEXT("/maximum"), TEXT("The BlendSpace axis maximum must be finite."),
					MakeShared<FJsonValueString>(LexToString(AxisMutation.Maximum)), TEXT("a finite number"));
			if (AxisMutation.Maximum <= AxisMutation.Minimum)
				return InvalidAxis(TEXT("/maximum"), TEXT("The BlendSpace axis maximum must exceed its minimum."),
					MakeShared<FJsonValueNumber>(AxisMutation.Maximum),
					TEXT("greater than ") + LexToString(AxisMutation.Minimum));
			if (AxisMutation.GridDivisions < 1)
				return InvalidAxis(TEXT("/grid_divisions"), TEXT("The BlendSpace axis must have at least one grid division."),
					MakeShared<FJsonValueNumber>(AxisMutation.GridDivisions), TEXT("an integer greater than or equal to 1"));
			if (AxisIndices.Contains(AxisMutation.AxisIndex))
				return InvalidAxis(TEXT("/axis_index"), TEXT("The BlendSpace mutation specifies the same axis more than once."),
					MakeShared<FJsonValueNumber>(AxisMutation.AxisIndex), TEXT("a distinct axis index"));
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
			const FString AnimationRef = SampleMutation.Animation ? SampleMutation.Animation->GetPathName() : SampleMutation.AnimationRef;
			if (!SampleMutation.Animation
				|| SampleMutation.Animation->GetSkeleton() != &Skeleton)
			{
				TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueNull>();
				if (!SampleMutation.Animation) Actual = MakeShared<FJsonValueString>(AnimationRef);
				else if (SampleMutation.Animation->GetSkeleton()) Actual = MakeShared<FJsonValueString>(SampleMutation.Animation->GetSkeleton()->GetPathName());
				OutError = RigNativeFailure(TEXT("animation_rig_graph_blend_sample_skeleton_mismatch"),
					!SampleMutation.Animation ? FString::Printf(TEXT("BlendSpace sample animation '%s' could not be loaded as an AnimSequence."), *AnimationRef)
						: FString::Printf(TEXT("BlendSpace sample animation '%s' uses a different skeleton."), *AnimationRef),
					BlendSpace, FString(), Actual,
					MakeShared<FJsonValueString>(!SampleMutation.Animation ? UAnimSequence::StaticClass()->GetPathName() : Skeleton.GetPathName()),
					TEXT("/animation_ref"), SampleMutation.InputPointer);
				return false;
			}
			auto SampleFailure = [&](const TCHAR* Code, const FString& Reason)
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("animation_ref"), AnimationRef);
				TArray<TSharedPtr<FJsonValue>> Coordinates;
				for (int32 Index = 0; Index < 3; ++Index) Coordinates.Add(MakeShared<FJsonValueNumber>(SampleMutation.Coordinates[Index]));
				Actual->SetArrayField(TEXT("coordinates"), Coordinates);
				OutError = RigNativeFailure(Code, Reason, BlendSpace, FString(), MakeShared<FJsonValueObject>(Actual),
					nullptr, FString(), SampleMutation.InputPointer);
				return false;
			};
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
					return SampleFailure(TEXT("animation_rig_graph_blend_sample_create_failed"), ExistingIndex != INDEX_NONE
						? TEXT("A BlendSpace sample with this animation and coordinates already exists.")
						: TEXT("Unreal rejected adding the BlendSpace sample with this animation and coordinates."));
				}
				break;
			case EAnimationRigGraphMutationIntent::Update:
				if (ExistingIndex == INDEX_NONE)
				{
					return SampleFailure(TEXT("animation_rig_graph_blend_sample_missing"),
						TEXT("No BlendSpace sample with this animation and coordinates exists to update."));
				}
				break;
			case EAnimationRigGraphMutationIntent::Replace:
				if (ExistingIndex == INDEX_NONE
					&& BlendSpace.AddSample(
						SampleMutation.Animation,
						SampleMutation.Coordinates) == INDEX_NONE)
				{
					return SampleFailure(TEXT("animation_rig_graph_blend_sample_replace_failed"),
						TEXT("No matching BlendSpace sample exists, and Unreal rejected adding its replacement."));
				}
				break;
			case EAnimationRigGraphMutationIntent::Delete:
				if (ExistingIndex == INDEX_NONE
					|| !BlendSpace.DeleteSample(ExistingIndex))
				{
					return SampleFailure(TEXT("animation_rig_graph_blend_sample_delete_failed"), ExistingIndex == INDEX_NONE
						? TEXT("No BlendSpace sample with this animation and coordinates exists to delete.")
						: TEXT("Unreal rejected deleting the matching BlendSpace sample."));
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_definition_missing"),
				TEXT("Skeleton definition apply requires at least one bone mutation."), Skeleton, FString(),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one bone mutation")));
			return false;
		}
		Skeleton.Modify();
		FReferenceSkeletonModifier Modifier(&Skeleton);
		for (const FAnimationRigGraphSkeletonBoneMutation& Mutation : Mutations)
		{
			if (Mutation.BoneName.IsNone())
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_bone_missing"),
					TEXT("The bone name resolves to None."), Skeleton, FString(),
					MakeShared<FJsonValueString>(Mutation.BoneName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None bone name")),
					TEXT("/bone_name"), Mutation.InputPointer);
				return false;
			}
			const int32 BoneIndex = Modifier.FindBoneIndex(Mutation.BoneName);
			const int32 ParentIndex = Mutation.ParentBone.IsNone()
				? INDEX_NONE : Modifier.FindBoneIndex(Mutation.ParentBone);
			if (!Mutation.ParentBone.IsNone() && ParentIndex == INDEX_NONE)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_parent_missing"),
					FString::Printf(TEXT("Parent bone '%s' for bone '%s' does not exist in the reference skeleton."),
						*Mutation.ParentBone.ToString(), *Mutation.BoneName.ToString()), Skeleton, FString(),
					MakeShared<FJsonValueString>(Mutation.ParentBone.ToString()), MakeShared<FJsonValueString>(TEXT("existing parent bone")),
					Mutation.bHasParentBoneInput ? TEXT("/parent_bone") : FString(), Mutation.InputPointer);
				return false;
			}
			switch (Mutation.Intent)
			{
			case EAnimationRigGraphMutationIntent::Create:
				if (BoneIndex != INDEX_NONE)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_bone_exists"),
						FString::Printf(TEXT("Cannot create bone '%s' because it already exists."), *Mutation.BoneName.ToString()), Skeleton, FString(),
						MakeShared<FJsonValueString>(Mutation.BoneName.ToString()), MakeShared<FJsonValueString>(TEXT("new bone name")),
						TEXT("/bone_name"), Mutation.InputPointer);
					return false;
				}
				Modifier.Add(FMeshBoneInfo(
					Mutation.BoneName, Mutation.BoneName.ToString(), ParentIndex),
					FTransform::Identity, Mutation.ParentBone.IsNone());
				break;
			case EAnimationRigGraphMutationIntent::Delete:
				if (BoneIndex == INDEX_NONE)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_bone_missing"),
						FString::Printf(TEXT("Cannot delete bone '%s' because it does not exist."), *Mutation.BoneName.ToString()), Skeleton, FString(),
						MakeShared<FJsonValueString>(Mutation.BoneName.ToString()), MakeShared<FJsonValueString>(TEXT("existing bone name")),
						TEXT("/bone_name"), Mutation.InputPointer);
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
					if (BoneIndex == INDEX_NONE)
					{
						OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_reparent_unsupported"),
							FString::Printf(TEXT("Cannot update bone '%s' because it does not exist."), *Mutation.BoneName.ToString()), Skeleton, FString(),
							MakeShared<FJsonValueString>(Mutation.BoneName.ToString()), MakeShared<FJsonValueString>(TEXT("existing bone name")),
							TEXT("/bone_name"), Mutation.InputPointer);
					}
					else
					{
						const int32 ExistingParent = Skeleton.GetReferenceSkeleton().GetParentIndex(BoneIndex);
						const FString ExistingParentName = ExistingParent == INDEX_NONE ? FString()
							: Skeleton.GetReferenceSkeleton().GetBoneName(ExistingParent).ToString();
						OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_reparent_unsupported"),
							FString::Printf(TEXT("Changing the parent of bone '%s' is not supported; preserve its current parent."), *Mutation.BoneName.ToString()), Skeleton, FString(),
							MakeShared<FJsonValueString>(Mutation.ParentBone.IsNone() ? FString() : Mutation.ParentBone.ToString()),
							MakeShared<FJsonValueString>(ExistingParentName),
							Mutation.bHasParentBoneInput ? TEXT("/parent_bone") : FString(), Mutation.InputPointer);
					}
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_compatibility_missing"),
				TEXT("Skeleton compatibility apply requires at least one mutation."), Skeleton, FString(),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one compatibility mutation")));
			return false;
		}
		Skeleton.Modify();
		for (const FAnimationRigGraphSkeletonCompatibilityMutation& Mutation
			: Mutations)
		{
			if (!Mutation.CompatibleSkeleton
				|| Mutation.CompatibleSkeleton == &Skeleton)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_compatible_skeleton_invalid"),
					!Mutation.CompatibleSkeleton ? TEXT("The compatible skeleton object is unavailable.")
						: TEXT("A skeleton cannot list itself as a compatible skeleton."), Skeleton, FString(), nullptr,
					MakeShared<FJsonValueString>(TEXT("a different, loaded Skeleton asset")), FString(), Mutation.InputPointer);
				if (Mutation.CompatibleSkeleton) OutError.ActualValue = MakeShared<FJsonValueString>(Mutation.CompatibleSkeleton->GetPathName());
				else OutError.ActualValue = MakeShared<FJsonValueNull>();
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
			const FString Field = Mutation.Name.IsNone() ? FString()
				: Mutation.SourceBone.IsNone() ? TEXT("/source_bone") : TEXT("/target_bone");
			const FName InvalidName = Mutation.Name.IsNone() ? Mutation.Name
				: Mutation.SourceBone.IsNone() ? Mutation.SourceBone : Mutation.TargetBone;
			OutError = RigNativeFailure(TEXT("animation_rig_graph_virtual_bone_invalid"),
				Mutation.Name.IsNone() ? TEXT("The virtual bone name resolves to None.")
					: Mutation.SourceBone.IsNone() ? TEXT("The virtual bone source bone resolves to None.")
					: TEXT("The virtual bone target bone resolves to None."), Skeleton, FString(),
				MakeShared<FJsonValueString>(InvalidName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None bone name")),
				Field, Mutation.InputPointer);
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
				OutError = RigNativeFailure(TEXT("animation_rig_graph_virtual_bone_missing"),
					FString::Printf(TEXT("Cannot delete virtual bone '%s' because it does not exist."), *Mutation.Name.ToString()), Skeleton, FString(),
					MakeShared<FJsonValueString>(Mutation.Name.ToString()), MakeShared<FJsonValueString>(TEXT("existing virtual bone name")),
					FString(), Mutation.InputPointer);
				return false;
			}
			Skeleton.RemoveVirtualBones({Mutation.Name});
		}
		else if (!bExists)
		{
			FName Name = Mutation.Name;
			UE::UAI::Contract::FDiagnostic Error; FString Field;
			UeAgentSkeletonVirtualBones::FDiagnosticSource DiagnosticSource;
			DiagnosticSource.TargetPath = Skeleton.GetPathName();
			DiagnosticSource.Operation.JsonPath = Mutation.InputPointer;
			DiagnosticSource.Name = DiagnosticSource.Operation;
			if (!Mutation.InputPointer.IsEmpty())
			{
				const FString Prefix = Mutation.InputPointer == TEXT("/") ? FString() : Mutation.InputPointer;
				DiagnosticSource.TargetBone.JsonPath = Prefix + TEXT("/target_bone");
			}
			if (!UeAgentSkeletonVirtualBones::TryAdd(Skeleton, Mutation.SourceBone, Mutation.TargetBone, Name, Error, Field, DiagnosticSource))
			{
				OutError = Error;
				return false;
			}
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
            const TCHAR* Field = SourceBone.IsNone() ? TEXT("source_bone")
                : TargetBone.IsNone() ? TEXT("target_bone") : TEXT("virtual_bone_name");
            OutError = RigNativeFailure(TEXT("animation_rig_graph_skeleton_key_missing"),
                FString::Printf(TEXT("Skeleton configuration requires a non-None %s."), Field), Skeleton, VirtualBoneName.ToString(),
                MakeShared<FJsonValueString>(FName().ToString()), MakeShared<FJsonValueString>(TEXT("non-None bone name")), Field);
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
		if (!bVirtualBoneExists)
		{
			FName Name = VirtualBoneName;
			UE::UAI::Contract::FDiagnostic Error; FString Field;
			if (!UeAgentSkeletonVirtualBones::TryAdd(Skeleton, SourceBone, TargetBone, Name, Error, Field))
			{
				OutError = Error;
				return false;
			}
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
            const bool bNativeMissing = !Hierarchy || !Controller;
            const TCHAR* Field = !Hierarchy ? TEXT("hierarchy_available") : !Controller ? TEXT("controller_available")
                : ParentBoneName.IsNone() ? TEXT("parent_bone_name") : TEXT("control_name");
            OutError = RigNativeFailure(TEXT("animation_rig_graph_control_rig_controller_unavailable"),
                bNativeMissing ? TEXT("The Control Rig hierarchy or controller required for compilation is unavailable.")
                    : FString::Printf(TEXT("Control Rig setup requires a non-None %s."), Field),
                Blueprint, ControlName.ToString(), nullptr, nullptr, Field);
            if (bNativeMissing)
            {
                OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
                OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            }
            else
            {
                OutError.ActualValue = MakeShared<FJsonValueString>(FName().ToString());
                OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None name"));
            }
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
			OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("animation_rig_graph_compile_incomplete"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), CompilerLog, &Blueprint);
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
		TArray<FAnimationRigGraphUnitMutation>& UnitMutations,
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_control_rig_controller_unavailable"),
				!Hierarchy ? TEXT("The Control Rig hierarchy is unavailable.")
					: !Controller ? TEXT("The Control Rig hierarchy controller is unavailable.")
					: TEXT("No control or RigVM node mutations were supplied."), Blueprint);
			return false;
		}
		Blueprint.Modify();
		Blueprint.ParentClass = UControlRig::StaticClass();
		for (const FAnimationRigGraphControlMutation& Mutation : Mutations)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Mutation.InputPointer); };
			const FString Subject = TEXT("rig_element:control:") + Mutation.Name.ToString();
			const FRigElementKey ControlKey(Mutation.Name, ERigElementType::Control);
			const bool bExists = Hierarchy->Contains(ControlKey);
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveElement(ControlKey, false, false))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_control_remove_failed"),
						FString::Printf(TEXT("Control '%s' %s."), *Mutation.Name.ToString(), bExists
							? TEXT("could not be removed by the hierarchy controller") : TEXT("does not exist and cannot be removed")), Blueprint, Subject);
					return false;
				}
				continue;
			}
			ERigControlType ControlType = ERigControlType::Float;
			const FRigElementKey ParentKey(Mutation.ParentBone, ERigElementType::Bone);
			if (Mutation.Name.IsNone() || Mutation.ParentBone.IsNone()
				|| !Hierarchy->Contains(ParentKey))
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_control_definition_invalid"),
					Mutation.Name.IsNone() ? TEXT("A control mutation has no control name.")
						: Mutation.ParentBone.IsNone() ? FString::Printf(TEXT("Control '%s' has no parent bone name."), *Mutation.Name.ToString())
						: FString::Printf(TEXT("Parent bone '%s' for control '%s' does not exist in the hierarchy."),
							*Mutation.ParentBone.ToString(), *Mutation.Name.ToString()), Blueprint, Subject,
					MakeShared<FJsonValueString>(Mutation.Name.IsNone() ? Mutation.Name.ToString() : Mutation.ParentBone.ToString()),
					MakeShared<FJsonValueString>(Mutation.Name.IsNone() ? TEXT("non-empty control name") : TEXT("existing parent bone")),
					Mutation.Name.IsNone() ? FString() : TEXT("/parent_native_key"), Mutation.InputPointer);
				return false;
			}
			if (!TryControlType(Mutation.ControlTypeRef, ControlType, Blueprint, Mutation.Name, Mutation.InputPointer, OutError)) return false;
			if (bExists
				&& (Mutation.Intent == EAnimationRigGraphMutationIntent::Update
					|| Mutation.Intent == EAnimationRigGraphMutationIntent::Replace)
				&& !Controller->RemoveElement(ControlKey, false, false))
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_control_replace_failed"),
					FString::Printf(TEXT("The hierarchy controller could not remove control '%s' before replacing it."), *Mutation.Name.ToString()), Blueprint, Subject);
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
				const auto ActualKey = MakeShared<FJsonObject>();
				ActualKey->SetStringField(TEXT("name"), Added.Name.ToString());
				ActualKey->SetNumberField(TEXT("element_type"), static_cast<int32>(Added.Type));
				const auto ExpectedKey = MakeShared<FJsonObject>();
				ExpectedKey->SetStringField(TEXT("name"), ControlKey.Name.ToString());
				ExpectedKey->SetNumberField(TEXT("element_type"), static_cast<int32>(ControlKey.Type));
				OutError = RigNativeFailure(TEXT("animation_rig_graph_control_add_failed"),
					TEXT("The hierarchy controller returned a different element identity when adding the control."), Blueprint, Subject,
					MakeShared<FJsonValueObject>(ActualKey), MakeShared<FJsonValueObject>(ExpectedKey));
				return false;
			}
		}

		URigVMGraph* const RigGraph = Blueprint.GetDefaultModel();
		URigVMController* const RigController = RigGraph
			? Blueprint.GetController(RigGraph) : nullptr;
		if (!UnitMutations.IsEmpty() && (!RigGraph || !RigController))
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_controller_unavailable"),
				!RigGraph ? TEXT("The Control Rig has no default RigVM graph for the requested node mutations.")
					: TEXT("The Control Rig default graph has no RigVM controller."), Blueprint);
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
		for (FAnimationRigGraphUnitMutation& Mutation : UnitMutations)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Mutation.InputPointer); };
			const FString Subject = TEXT("rigvm_node:") + Mutation.NodeName.ToString();
			if (Mutation.NodeName.IsNone())
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_name_invalid"),
					TEXT("A RigVM node mutation has no node name."), Blueprint, Subject,
					MakeShared<FJsonValueString>(Mutation.NodeName.ToString()), MakeShared<FJsonValueString>(TEXT("non-empty node name")), TEXT("name"));
				return false;
			}
			URigVMNode* Existing = FindRigNode(Mutation.NodeName);
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!Existing
					|| !RigController->RemoveNode(Existing, true, false))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_remove_failed"),
						FString::Printf(TEXT("RigVM node '%s' %s."), *Mutation.NodeName.ToString(), Existing
							? TEXT("could not be removed by the RigVM controller") : TEXT("does not exist and cannot be removed")), Blueprint, Subject);
					return false;
				}
				continue;
			}
			if (!FMath::IsFinite(Mutation.Position.X)
				|| !FMath::IsFinite(Mutation.Position.Y))
			{
				const bool bInvalidX = !FMath::IsFinite(Mutation.Position.X);
				OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_definition_invalid"),
					FString::Printf(TEXT("RigVM node '%s' has a non-finite %s position."), *Mutation.NodeName.ToString(), bInvalidX ? TEXT("x") : TEXT("y")),
					Blueprint, Subject, MakeShared<FJsonValueString>(LexToString(bInvalidX ? Mutation.Position.X : Mutation.Position.Y)),
					MakeShared<FJsonValueString>(TEXT("finite coordinate")), bInvalidX ? TEXT("/position/x") : TEXT("/position/y"), Mutation.InputPointer);
				return false;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Create
				&& Existing)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_already_exists"),
					FString::Printf(TEXT("RigVM node '%s' already exists and cannot be created again."), *Mutation.NodeName.ToString()), Blueprint, Subject);
				return false;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Replace
				&& Existing)
			{
				if (!RigController->RemoveNode(Existing, true, false))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_replace_failed"),
						FString::Printf(TEXT("The RigVM controller could not remove node '%s' before replacing it."), *Mutation.NodeName.ToString()), Blueprint, Subject);
					return false;
				}
				Existing = nullptr;
			}
			if (Mutation.Intent == EAnimationRigGraphMutationIntent::Update)
			{
				UScriptStruct* const UnitStruct = LoadObject<UScriptStruct>(
					nullptr, *Mutation.ScriptStructRef);
				const URigVMUnitNode* const ExistingUnit =
					Cast<URigVMUnitNode>(Existing);
				if (!UnitStruct
					|| UnitStruct == FRigUnit::StaticStruct()
					|| !UnitStruct->IsChildOf(FRigUnit::StaticStruct())
					|| UnitStruct->HasMetaData(TEXT("Abstract"))
					|| Mutation.MethodName.IsNone()
					|| !ExistingUnit
					|| !ExistingUnit->GetScriptStruct()
					|| ExistingUnit->GetScriptStruct()->GetPathName()
						!= Mutation.ScriptStructRef
					|| ExistingUnit->GetMethodName() != Mutation.MethodName
					|| !RigController->SetNodePosition(
						Existing, Mutation.Position, true, false, false))
				{
					const FString Reason = !UnitStruct ? TEXT("the requested script struct could not be loaded")
						: UnitStruct == FRigUnit::StaticStruct() ? TEXT("the base RigUnit struct cannot be used as a concrete unit")
						: !UnitStruct->IsChildOf(FRigUnit::StaticStruct()) ? TEXT("the requested script struct is not a RigUnit")
						: UnitStruct->HasMetaData(TEXT("Abstract")) ? TEXT("the requested script struct is abstract")
						: Mutation.MethodName.IsNone() ? TEXT("the requested method name is empty")
						: !ExistingUnit ? (Existing ? TEXT("the existing node is not a RigVM unit") : TEXT("the node does not exist"))
						: !ExistingUnit->GetScriptStruct() ? TEXT("the existing unit has no script struct")
						: ExistingUnit->GetScriptStruct()->GetPathName() != Mutation.ScriptStructRef ? TEXT("the existing unit uses a different script struct")
						: ExistingUnit->GetMethodName() != Mutation.MethodName ? TEXT("the existing unit uses a different method")
						: TEXT("the RigVM controller rejected the position update");
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("node_exists"), Existing != nullptr);
					if (Existing) Actual->SetStringField(TEXT("node_class"), Existing->GetClass()->GetPathName());
					if (ExistingUnit)
					{
						Actual->SetStringField(TEXT("script_struct_ref"), ExistingUnit->GetScriptStruct() ? ExistingUnit->GetScriptStruct()->GetPathName() : FString());
						Actual->SetStringField(TEXT("method"), ExistingUnit->GetMethodName().ToString());
					}
					const auto Expected = MakeShared<FJsonObject>();
					Expected->SetStringField(TEXT("script_struct_ref"), Mutation.ScriptStructRef);
					Expected->SetStringField(TEXT("method"), Mutation.MethodName.ToString());
					Expected->SetNumberField(TEXT("position_x"), Mutation.Position.X);
					Expected->SetNumberField(TEXT("position_y"), Mutation.Position.Y);
					const FString InputField = !UnitStruct || UnitStruct == FRigUnit::StaticStruct()
						|| !UnitStruct->IsChildOf(FRigUnit::StaticStruct()) || UnitStruct->HasMetaData(TEXT("Abstract"))
						? TEXT("/script_struct_ref") : Mutation.MethodName.IsNone() ? (Mutation.bHasMethodInput ? TEXT("/method") : FString())
						: !ExistingUnit || !ExistingUnit->GetScriptStruct() ? FString()
						: ExistingUnit->GetScriptStruct()->GetPathName() != Mutation.ScriptStructRef ? TEXT("/script_struct_ref")
						: ExistingUnit->GetMethodName() != Mutation.MethodName ? (Mutation.bHasMethodInput ? TEXT("/method") : FString()) : TEXT("/position");
					OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_update_failed"),
						FString::Printf(TEXT("RigVM node '%s' could not be updated: %s."), *Mutation.NodeName.ToString(), *Reason), Blueprint, Subject,
						MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), InputField, Mutation.InputPointer);
					return false;
				}
				continue;
			}
			if (Mutation.CandidateName.IsEmpty())
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_candidate_selector_required"),
					FString::Printf(TEXT("RigVM node '%s' requires a candidate name for creation."), *Mutation.NodeName.ToString()), Blueprint, Subject,
					MakeShared<FJsonValueString>(Mutation.CandidateName), MakeShared<FJsonValueString>(TEXT("non-empty candidate name")), TEXT("/candidate/name"), Mutation.InputPointer);
				return false;
			}

			const FString ExpectedStruct = Mutation.ScriptStructRef;
			const FName ExpectedMethod = Mutation.MethodName;
			URigVMUnitNode* Added = nullptr;
			QueryOwners::AnimationRigGraph::FAnimationRigGraphCandidate
				ResolvedCandidate;
			QueryOwners::AnimationRigGraph::
				FAnimationRigGraphCandidateQueryError CandidateError;
			const bool bCandidateAdded = QueryOwners::AnimationRigGraph::
					TryPerformAnimationRigGraphCandidate(
						Blueprint,
						Mutation.CandidateName,
						FVector2f(Mutation.Position),
						Added,
						ResolvedCandidate,
						CandidateError, Mutation.CandidateSelect);
			if (!bCandidateAdded || !Added || !Added->GetScriptStruct()
				|| Added->GetMethodName().IsNone())
			{
				OutError = CandidateError.IsValid() ? CandidateError
					: RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_add_failed"),
						!bCandidateAdded ? TEXT("The native candidate action failed to create the RigVM unit.")
							: !Added ? TEXT("The native candidate action reported success without returning a RigVM unit.")
							: !Added->GetScriptStruct() ? TEXT("The created RigVM unit has no script struct.")
							: TEXT("The created RigVM unit has no method name."), Blueprint, Subject,
						MakeShared<FJsonValueString>(Mutation.CandidateName), MakeShared<FJsonValueString>(TEXT("candidate producing a valid RigVM unit")));
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
				if (!Mutation.InputPointer.IsEmpty())
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
						(Mutation.InputPointer == TEXT("/") ? FString() : Mutation.InputPointer)
							+ (OutError.PropertyPath == TEXT("x") || OutError.PropertyPath == TEXT("y") ? TEXT("/position") : TEXT("/candidate")));
				return false;
			}
			if (Added->GetFName() != Mutation.NodeName
				&& !RigController->RenameNode(
					Added, Mutation.NodeName, true, false))
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_rigvm_unit_rename_failed"),
					TEXT("The RigVM controller could not rename the created unit to the requested node name."), Blueprint, Subject,
					MakeShared<FJsonValueString>(Added->GetName()), MakeShared<FJsonValueString>(Mutation.NodeName.ToString()), TEXT("name"));
				return false;
			}
			if ((Mutation.bHasScriptStructAssertion
					&& Added->GetScriptStruct()->GetPathName()
						!= ExpectedStruct)
				|| (Mutation.bHasMethodAssertion
					&& Added->GetMethodName() != ExpectedMethod))
			{
				const bool bStructMismatch = Mutation.bHasScriptStructAssertion && Added->GetScriptStruct()->GetPathName() != ExpectedStruct;
				OutError = RigNativeFailure(TEXT("animation_rig_graph_candidate_assertion_mismatch"),
					bStructMismatch ? TEXT("The candidate created a RigVM unit with a different script struct than requested.")
						: TEXT("The candidate created a RigVM unit with a different method than requested."), Blueprint, Subject,
					MakeShared<FJsonValueString>(bStructMismatch ? Added->GetScriptStruct()->GetPathName() : Added->GetMethodName().ToString()),
					MakeShared<FJsonValueString>(bStructMismatch ? ExpectedStruct : ExpectedMethod.ToString()),
					bStructMismatch ? TEXT("/script_struct_ref") : TEXT("/method"), Mutation.InputPointer);
				return false;
			}
			Mutation.CandidateName = ResolvedCandidate.Name;
			Mutation.ScriptStructRef =
				Added->GetScriptStruct()->GetPathName();
			Mutation.MethodName = Added->GetMethodName();
			Mutation.Position = Added->GetPosition();
			Mutation.bHasScriptStructAssertion = true;
			Mutation.bHasMethodAssertion = true;
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
			OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("animation_rig_graph_compile_incomplete"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), CompilerLog, &Blueprint);
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
            OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_name_missing"),
                TEXT("A Control Rig shape requires a non-None name."), Library, ShapeName.ToString(),
                MakeShared<FJsonValueString>(ShapeName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None shape name")), TEXT("shape_name"));
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_mutations_empty"),
				TEXT("At least one shape mutation is required."), Library, FString(),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one mutation")));
			return false;
		}
		UMaterial* SharedMaterial = nullptr;
		for (const FAnimationRigGraphShapeMutation& Mutation : Mutations)
		{
			if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
			{
				if (!Mutation.Mesh)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_mesh_invalid"),
						FString::Printf(TEXT("Shape '%s' has no loaded static mesh."), *Mutation.Name.ToString()),
						Library, TEXT("item:shape:") + Mutation.Name.ToString(), nullptr,
						MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName()), TEXT("/mesh_ref"), Mutation.InputPointer);
					OutError.ActualValuePresent = false;
					return false;
				}
				if (!Mutation.Material)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_material_invalid"),
						FString::Printf(TEXT("Shape '%s' has no loaded material."), *Mutation.Name.ToString()),
						Library, TEXT("item:shape:") + Mutation.Name.ToString(), nullptr,
						MakeShared<FJsonValueString>(UMaterial::StaticClass()->GetPathName()), TEXT("/material_ref"), Mutation.InputPointer);
					OutError.ActualValuePresent = false;
					return false;
				}
				if (Mutation.OrderIndex < 0)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_order_invalid"),
						FString::Printf(TEXT("Shape '%s' order_index must be nonnegative."), *Mutation.Name.ToString()),
						Library, TEXT("item:shape:") + Mutation.Name.ToString(), MakeShared<FJsonValueNumber>(Mutation.OrderIndex),
						MakeShared<FJsonValueString>(TEXT("integer greater than or equal to zero")), TEXT("/order_index"), Mutation.InputPointer);
					return false;
				}
				if (SharedMaterial && SharedMaterial != Mutation.Material)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_material_conflict"),
						FString::Printf(TEXT("Shape '%s' uses a different material; all shapes in this library mutation must share one material."),
							*Mutation.Name.ToString()), Library, TEXT("item:shape:") + Mutation.Name.ToString(),
						MakeShared<FJsonValueString>(Mutation.Material->GetPathName()),
						MakeShared<FJsonValueString>(SharedMaterial->GetPathName()), TEXT("/material_ref"), Mutation.InputPointer);
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
					OutError = RigNativeFailure(TEXT("animation_rig_graph_shape_remove_failed"),
						FString::Printf(TEXT("Shape '%s' cannot be removed because it does not exist in the library."), *Mutation.Name.ToString()),
						Library, TEXT("item:shape:") + Mutation.Name.ToString(), MakeShared<FJsonValueString>(Mutation.Name.ToString()),
						nullptr, FString(), Mutation.InputPointer);
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
		FAnimationRigGraphAdapterError& OutError,
		const FString& CommentNodeInputPointer) const
	{
		OutError.Reset();
		UOptimusNodeGraph* Graph = Deformer.GetUpdateGraph();
		if (!Graph || CommentNodeName.IsNone())
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_optimus_graph_unavailable"),
				!Graph ? TEXT("The Deformer update graph is unavailable.")
					: TEXT("A non-None comment node name is required to capture Deformer topology."), Deformer);
			if (Graph)
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(CommentNodeName.ToString());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None node name"));
				LocateRigInput(OutError, CommentNodeInputPointer);
			}
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_optimus_comment_add_failed"),
				FString::Printf(TEXT("Comment node '%s' could not be created in the Deformer update graph."), *CommentNodeName.ToString()), Deformer);
			LocateRigInput(OutError, CommentNodeInputPointer);
			return false;
		}
		const bool bCompileComplete = Deformer.Compile();
		SetReceipts(Deformer, EAnimationRigGraphBarrierKind::Compile,
			bCompileComplete, bCompileComplete ? 0 : 1, OutApplyReceipt, OutBarrierReceipt);
		if (!bCompileComplete)
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_compile_incomplete"),
				TEXT("The Deformer failed to compile after topology editing."), Deformer);
			return false;
		}
		const FString GraphKey = TEXT("optimus_graph:") + Graph->GetPathName();
		const FString NodeKey = TEXT("optimus_node:") + Comment->GetPathName();
		TArray<FAnimationRigGraphItemIdentity> Items;
		Items.Add(ObjectItem(GraphKey, *Graph, TEXT("graph")));
		Items.Add(ObjectItem(NodeKey, *Comment, TEXT("node")));
		TArray<FAnimationRigGraphPinIdentity> Pins;
		TMap<const UOptimusNode*, FString> NodeKeys;
		for (const UOptimusNode* Node : Graph->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			const FString StableNodeKey =
				TEXT("optimus_node:") + Node->GetPathName();
			NodeKeys.Add(Node, StableNodeKey);
			if (Node != Comment)
			{
				Items.Add(ObjectItem(StableNodeKey, *Node, TEXT("node")));
			}
			for (const UOptimusNodePin* Pin : Node->GetPins())
			{
				if (!Pin || Pin->IsGroupingPin())
				{
					continue;
				}
				const FString PinKey = TEXT("optimus_pin:") + Pin->GetPinPath();
				const EOptimusNodePinDirection Direction = Pin->GetDirection();
				if (Direction != EOptimusNodePinDirection::Input
					&& Direction != EOptimusNodePinDirection::Output)
				{
					continue;
				}
				const FOptimusDataTypeHandle DataType = Pin->GetDataType();
				const FString TypeName = DataType.IsValid()
					? DataType->TypeName.ToString() : FString();
				Pins.Add(FAnimationRigGraphPinIdentity(
					PinKey,
					StableNodeKey,
					Pin->GetUniqueName().ToString(),
					Direction == EOptimusNodePinDirection::Input
						? EAnimationRigGraphPinDirection::Input
						: EAnimationRigGraphPinDirection::Output,
					TypeName));
			}
		}
		TArray<FAnimationRigGraphRelationIdentity> Relations;
		Relations.Emplace(TEXT("graph_member:") + Comment->GetPathName(),
			TEXT("graph_member"), FString(), GraphKey, NodeKey, 0);
		for (const UOptimusNodeLink* Link : Graph->GetAllLinks())
		{
			if (!Link || !Link->GetNodeOutputPin() || !Link->GetNodeInputPin())
			{
				continue;
			}
			const UOptimusNode* OutputNode =
				Link->GetNodeOutputPin()->GetOwningNode();
			const UOptimusNode* InputNode =
				Link->GetNodeInputPin()->GetOwningNode();
			if (!OutputNode || !InputNode
				|| !NodeKeys.Contains(OutputNode) || !NodeKeys.Contains(InputNode))
			{
				continue;
			}
			Relations.Emplace(
				TEXT("optimus_link:") + Link->GetNodeOutputPin()->GetPinPath()
					+ TEXT("->") + Link->GetNodeInputPin()->GetPinPath(),
				TEXT("connection"), FString(),
				TEXT("optimus_pin:") + Link->GetNodeOutputPin()->GetPinPath(),
				TEXT("optimus_pin:") + Link->GetNodeInputPin()->GetPinPath(),
				INDEX_NONE);
		}
		OutTopology = FAnimationRigGraphTopology(
			MoveTemp(Items), MoveTemp(Pins), MoveTemp(Relations));
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
            const TCHAR* Field = !Controller ? TEXT("controller_available") : GoalName.IsNone() ? TEXT("goal_name")
                : GoalBone.IsNone() ? TEXT("goal_bone") : ChainName.IsNone() ? TEXT("chain_name")
                : StartBone.IsNone() ? TEXT("start_bone") : TEXT("end_bone");
            OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_rig_controller_unavailable"),
                !Controller ? TEXT("The IK Rig controller is unavailable.")
                    : FString::Printf(TEXT("IK Rig setup requires a non-None %s."), Field), Rig, GoalName.ToString(), nullptr, nullptr, Field);
            if (!Controller)
            {
                OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
                OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            }
            else
            {
                OutError.ActualValue = MakeShared<FJsonValueString>(FName().ToString());
                OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None name"));
            }
			return false;
		}
		Rig.Modify();
        if (!Controller->GetGoal(GoalName))
        {
            const FName AddedGoal = Controller->AddNewGoal(GoalName, GoalBone);
            if (AddedGoal != GoalName)
            {
                OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_goal_add_failed"),
                    FString::Printf(TEXT("Unreal did not create an IK goal on bone '%s' with the requested name."),
                        *GoalBone.ToString()), Rig, GoalName.ToString(),
                    MakeShared<FJsonValueString>(AddedGoal.ToString()), MakeShared<FJsonValueString>(GoalName.ToString()), TEXT("goal_name"));
                return false;
            }
        }
		const bool bChainExists = Controller->GetRetargetChains().ContainsByPredicate(
			[ChainName](const FBoneChain& Chain) { return Chain.ChainName == ChainName; });
        if (!bChainExists)
        {
            const FName AddedChain = Controller->AddRetargetChain(ChainName, StartBone, EndBone, GoalName);
            if (AddedChain != ChainName)
            {
                OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_chain_add_failed"),
                    FString::Printf(TEXT("Unreal did not create a retarget chain from '%s' to '%s' with goal '%s' and the requested name."),
                        *StartBone.ToString(), *EndBone.ToString(), *GoalName.ToString()), Rig, ChainName.ToString(),
                    MakeShared<FJsonValueString>(AddedChain.ToString()), MakeShared<FJsonValueString>(ChainName.ToString()), TEXT("chain_name"));
                return false;
            }
        }
        if (Controller->GetNumSolvers() == 0)
        {
            const int32 SolverIndex = Controller->AddSolver(FIKRigFullBodyIKSolver::StaticStruct());
            if (SolverIndex == INDEX_NONE)
            {
                OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_add_failed"),
                    TEXT("Unreal failed to add the Full Body IK solver."), Rig, FIKRigFullBodyIKSolver::StaticStruct()->GetPathName(),
                    MakeShared<FJsonValueNumber>(SolverIndex), MakeShared<FJsonValueString>(TEXT("valid solver index")), TEXT("solver_index"));
                return false;
            }
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
		FAnimationRigGraphAdapterError& OutError, const FString& RootInputPointer) const
	{
		OutError.Reset();
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		if (!Controller || RetargetRoot.IsNone())
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_rig_controller_unavailable"),
				!Controller ? TEXT("The IK Rig controller is unavailable.") : TEXT("The IK Rig definition has no retarget root bone."), Rig);
			if (Controller)
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(RetargetRoot.ToString());
				LocateRigInput(OutError, RootInputPointer);
			}
			return false;
		}
		UE::UAI::Contract::FDiagnostic IntentError;
		UE::UAI::Contract::FSourceLocation RootSource;
		RootSource.JsonPath = RootInputPointer;
		if (!UeAgentIKAssetOps::ApplyRetargetRoot(*Controller, RetargetRoot, IntentError, RootSource, Rig.GetPathName()))
		{
			OutError = IntentError;
			return false;
		}
		Rig.Modify();
		for (const FAnimationRigGraphIkGoalMutation& Goal : Goals)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Goal.InputPointer); };
			const bool bExists = Controller->GetGoal(Goal.Name) != nullptr;
			if (Goal.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveGoal(Goal.Name))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_goal_remove_failed"),
						FString::Printf(TEXT("IK goal '%s' %s."), *Goal.Name.ToString(), bExists
							? TEXT("could not be removed by the controller") : TEXT("does not exist and cannot be removed")), Rig);
					return false;
				}
			}
			else if (bExists)
			{
				if (!Controller->SetGoalBone(Goal.Name, Goal.BoneName))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_goal_update_failed"),
						FString::Printf(TEXT("The IK Rig controller rejected the bone assignment for goal '%s'."), *Goal.Name.ToString()),
						Rig, FString(), MakeShared<FJsonValueString>(Goal.BoneName.ToString()), nullptr, TEXT("/bone_name"), Goal.InputPointer);
					return false;
				}
			}
			else
			{
				const FName AddedName = Controller->AddNewGoal(Goal.Name, Goal.BoneName);
				if (AddedName != Goal.Name)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_goal_add_failed"),
						TEXT("The IK Rig controller returned a different goal name when adding the requested goal."), Rig, FString(),
						MakeShared<FJsonValueString>(AddedName.ToString()), MakeShared<FJsonValueString>(Goal.Name.ToString()));
					OutError.ProposedValue = MakeShared<FJsonValueString>(Goal.BoneName.ToString());
					return false;
				}
			}
		}
		for (const FAnimationRigGraphIkSolverMutation& Solver : Solvers)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Solver.InputPointer); };
			if (Solver.OrderIndex < 0)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_order_invalid"),
					TEXT("The IK solver order index must be nonnegative."), Rig, FString(),
					MakeShared<FJsonValueNumber>(Solver.OrderIndex), MakeShared<FJsonValueString>(TEXT("integer greater than or equal to zero")),
					Solver.Intent == EAnimationRigGraphMutationIntent::Delete ? FString() : TEXT("/order_index"), Solver.InputPointer);
				return false;
			}
			if (Solver.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				const int32 SolverCount = Controller->GetNumSolvers();
				if (Solver.OrderIndex >= SolverCount
					|| !Controller->RemoveSolver(Solver.OrderIndex))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_remove_failed"),
						Solver.OrderIndex >= SolverCount ? TEXT("The requested IK solver index is outside the current solver stack.")
							: TEXT("The IK Rig controller could not remove the requested solver."), Rig, FString(),
						MakeShared<FJsonValueNumber>(Solver.OrderIndex), MakeShared<FJsonValueString>(FString::Printf(TEXT("existing index below %d"), SolverCount)));
					return false;
				}
				continue;
			}
			UScriptStruct* const SolverType = LoadObject<UScriptStruct>(
				nullptr, *Solver.SolverClassRef);
			if (!SolverType)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_type_invalid"),
					TEXT("The IK solver type could not be loaded as a script struct."), Rig, FString(),
					MakeShared<FJsonValueString>(Solver.SolverClassRef), MakeShared<FJsonValueString>(UScriptStruct::StaticClass()->GetPathName()),
					TEXT("/solver_class_ref"), Solver.InputPointer);
				return false;
			}
			if (Solver.OrderIndex < Controller->GetNumSolvers()
				&& (Solver.Intent == EAnimationRigGraphMutationIntent::Update
					|| Solver.Intent == EAnimationRigGraphMutationIntent::Replace)
				&& !Controller->RemoveSolver(Solver.OrderIndex))
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_replace_failed"),
					TEXT("The IK Rig controller could not remove the existing solver before replacement."), Rig, FString(),
					MakeShared<FJsonValueNumber>(Solver.OrderIndex), nullptr, TEXT("/order_index"), Solver.InputPointer);
				return false;
			}
			const int32 AddedIndex = Controller->AddSolver(SolverType);
			const int32 TargetIndex = FMath::Min(Solver.OrderIndex, AddedIndex);
			if (AddedIndex == INDEX_NONE
				|| (AddedIndex != TargetIndex
					&& !Controller->MoveSolverInStack(AddedIndex, TargetIndex)))
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_solver_add_failed"),
					AddedIndex == INDEX_NONE ? TEXT("The IK Rig controller could not add the requested solver type.")
						: TEXT("The IK Rig controller added the solver but could not move it to the requested stack position."), Rig, FString(),
					MakeShared<FJsonValueNumber>(AddedIndex), nullptr,
					AddedIndex == INDEX_NONE ? TEXT("/solver_class_ref") : TEXT("/order_index"), Solver.InputPointer);
				if (AddedIndex == INDEX_NONE) OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("valid solver index"));
				else OutError.ExpectedValue = MakeShared<FJsonValueNumber>(TargetIndex);
				const auto Proposed = MakeShared<FJsonObject>();
				Proposed->SetStringField(TEXT("solver_class_ref"), Solver.SolverClassRef);
				Proposed->SetNumberField(TEXT("order_index"), Solver.OrderIndex);
				OutError.ProposedValue = MakeShared<FJsonValueObject>(Proposed);
				return false;
			}
		}
		for (const FAnimationRigGraphIkChainMutation& Chain : Chains)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Chain.InputPointer); };
			const bool bExists = Controller->GetRetargetChains().ContainsByPredicate(
				[&Chain](const FBoneChain& Existing)
				{
					return Existing.ChainName == Chain.Name;
				});
			if (Chain.Intent == EAnimationRigGraphMutationIntent::Delete)
			{
				if (!bExists || !Controller->RemoveRetargetChain(Chain.Name))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_chain_remove_failed"),
						FString::Printf(TEXT("Retarget chain '%s' %s."), *Chain.Name.ToString(), bExists
							? TEXT("could not be removed by the IK Rig controller") : TEXT("does not exist and cannot be removed")), Rig);
					return false;
				}
				continue;
			}
			if (bExists)
			{
				const bool bStartApplied = Controller->SetRetargetChainStartBone(Chain.Name, Chain.StartBone);
				if (!bStartApplied || !Controller->SetRetargetChainEndBone(Chain.Name, Chain.EndBone))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_chain_update_failed"),
						FString::Printf(TEXT("The IK Rig controller rejected the %s bone for retarget chain '%s'."),
							bStartApplied ? TEXT("end") : TEXT("start"), *Chain.Name.ToString()), Rig, FString(),
						MakeShared<FJsonValueString>((bStartApplied ? Chain.EndBone : Chain.StartBone).ToString()), nullptr,
						bStartApplied ? TEXT("/end_bone") : TEXT("/start_bone"), Chain.InputPointer);
					return false;
				}
			}
			else
			{
				const FName AddedName = Controller->AddRetargetChain(Chain.Name, Chain.StartBone, Chain.EndBone, Chain.GoalName);
				if (AddedName != Chain.Name)
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_chain_add_failed"),
						TEXT("The IK Rig controller returned a different name when adding the requested retarget chain."), Rig, FString(),
						MakeShared<FJsonValueString>(AddedName.ToString()), MakeShared<FJsonValueString>(Chain.Name.ToString()));
					const auto Proposed = MakeShared<FJsonObject>();
					Proposed->SetStringField(TEXT("start_bone"), Chain.StartBone.ToString());
					Proposed->SetStringField(TEXT("end_bone"), Chain.EndBone.ToString());
					Proposed->SetStringField(TEXT("goal_name"), Chain.GoalName.ToString());
					OutError.ProposedValue = MakeShared<FJsonValueObject>(Proposed);
					return false;
				}
			}
			UE::UAI::Contract::FSourceLocation GoalSource;
			if (!Chain.InputPointer.IsEmpty()) GoalSource.JsonPath = Chain.InputPointer == TEXT("/")
				? TEXT("/goal_name") : Chain.InputPointer + TEXT("/goal_name");
			if (!UeAgentIKAssetOps::ApplyRetargetChainGoal(*Controller, Chain.Name,
				Chain.GoalName, IntentError, GoalSource, Rig.GetPathName()))
			{
				OutError = IntentError;
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
		FAnimationRigGraphAdapterError& OutError, const FString& InputPointer) const
	{
		OutError.Reset();
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		if (!Controller
			|| (Controller->GetSkeletalMesh() != &SkeletalMesh
				&& !Controller->SetSkeletalMesh(&SkeletalMesh)))
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_rig_mesh_invalid"),
				!Controller ? TEXT("The IK Rig controller is unavailable for automatic generation.")
					: TEXT("The IK Rig controller rejected the skeletal mesh for automatic generation."), Rig);
			if (Controller)
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(SkeletalMesh.GetPathName());
				OutError.PropertyPath = TEXT("/skeletal_mesh_ref");
				LocateRigInput(OutError, InputPointer, TEXT("/skeletal_mesh_ref"));
			}
			return false;
		}
		if (Preset == TEXT("humanoid"))
		{
			const bool bRetargetApplied = Controller->ApplyAutoGeneratedRetargetDefinition();
			if (!bRetargetApplied || !Controller->ApplyAutoFBIK())
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_auto_generate_failed"),
					!bRetargetApplied ? TEXT("The IK Rig controller could not generate the humanoid retarget definition.")
						: TEXT("The IK Rig controller generated the humanoid retarget definition but could not generate the full-body IK setup."), Rig,
					FString(), MakeShared<FJsonValueString>(Preset), nullptr, TEXT("/preset"), InputPointer);
				return false;
			}
		}
		else if (Preset != TEXT("custom"))
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_ik_auto_preset_unsupported"),
				TEXT("The requested IK automatic-generation preset is unsupported."), Rig, FString(),
				MakeShared<FJsonValueString>(Preset), MakeShared<FJsonValueString>(TEXT("humanoid or custom")), TEXT("/preset"), InputPointer);
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
            OutError = RigNativeFailure(TEXT("animation_rig_graph_retargeter_controller_unavailable"),
                !Controller ? TEXT("The IK Retargeter controller is unavailable.") : TEXT("Retargeting requires a non-None target chain name."),
                Retargeter, TargetChainName.ToString(), nullptr, nullptr, !Controller ? TEXT("controller_available") : TEXT("target_chain_name"));
            if (!Controller)
            {
                OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
                OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            }
            else
            {
                OutError.ActualValue = MakeShared<FJsonValueString>(TargetChainName.ToString());
                OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-None chain name"));
            }
			return false;
		}
		Retargeter.Modify();
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, &SourceRig);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, &TargetRig);
        if (!Controller->GetChainMapping(NAME_None))
        {
            const int32 OpIndex = Controller->AddRetargetOp(FIKRetargetFKChainsOp::StaticStruct());
            if (OpIndex == INDEX_NONE)
            {
                OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_op_add_failed"),
                    TEXT("Unreal failed to add the FK Chains retarget operation."), Retargeter,
                    FIKRetargetFKChainsOp::StaticStruct()->GetPathName(),
                    MakeShared<FJsonValueNumber>(OpIndex), MakeShared<FJsonValueString>(TEXT("valid operation index")), TEXT("operation_index"));
                return false;
            }
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
            const bool bSourceMatches = Controller->GetIKRig(ERetargetSourceOrTarget::Source) == &SourceRig;
            const bool bTargetMatches = Controller->GetIKRig(ERetargetSourceOrTarget::Target) == &TargetRig;
            if (!bSourceMatches || !bTargetMatches)
            {
                const UIKRigDefinition* ActualRig = Controller->GetIKRig(!bSourceMatches ? ERetargetSourceOrTarget::Source : ERetargetSourceOrTarget::Target);
                OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_missing"),
                    TEXT("The retargeter did not retain the requested IK Rig assignment."), Retargeter, TargetChainName.ToString(),
                    ActualRig ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ActualRig->GetPathName())) : nullptr,
                    MakeShared<FJsonValueString>(!bSourceMatches ? SourceRig.GetPathName() : TargetRig.GetPathName()),
                    !bSourceMatches ? TEXT("source_rig") : TEXT("target_rig"));
            }
            else if (!ChainMapping || !ChainMapping->IsReady()
                || !ChainMapping->HasChain(TargetChainName, ERetargetSourceOrTarget::Target))
            {
                const TCHAR* Field = !ChainMapping ? TEXT("chain_mapping_available")
                    : !ChainMapping->IsReady() ? TEXT("chain_mapping_ready") : TEXT("target_chain_available");
                OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_missing"),
                    !ChainMapping ? TEXT("No retarget chain mapping was created.")
                        : !ChainMapping->IsReady() ? TEXT("The retarget chain mapping is not ready.")
                        : FString::Printf(TEXT("Target chain '%s' is absent from the retarget mapping."), *TargetChainName.ToString()),
                    Retargeter, TargetChainName.ToString(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Field);
            }
            else
            {
                OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_missing"),
                    TEXT("Exact automatic mapping did not map the target chain to the same-named source chain."),
                    Retargeter, TargetChainName.ToString(),
                    MakeShared<FJsonValueString>(ChainMapping->GetChainMappedTo(TargetChainName, ERetargetSourceOrTarget::Target).ToString()),
                    MakeShared<FJsonValueString>(TargetChainName.ToString()), TEXT("source_chain"));
            }
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
		FAnimationRigGraphAdapterError& OutError, const FString& SourceRigInputPointer, const FString& TargetRigInputPointer) const
	{
		OutError.Reset();
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter);
		if (!Controller || Mappings.IsEmpty())
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_retargeter_controller_unavailable"),
				!Controller ? TEXT("The IK Retargeter controller is unavailable.") : TEXT("At least one retarget chain mapping mutation is required."), Retargeter);
			return false;
		}
		Retargeter.Modify();
		Controller->SetIKRig(ERetargetSourceOrTarget::Source, &SourceRig);
		Controller->SetIKRig(ERetargetSourceOrTarget::Target, &TargetRig);
		Controller->CleanAsset();
		for (const FAnimationRigGraphRetargetMappingMutation& Mapping : Mappings)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Mapping.InputPointer); };
			if (Mapping.OperationRef != TEXT("/Script/IKRig.IKRetargetFKChainsOp")
				|| Mapping.OrderIndex != 0)
			{
				const bool bTypeInvalid = Mapping.OperationRef != TEXT("/Script/IKRig.IKRetargetFKChainsOp");
				OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_operation_unsupported"),
					bTypeInvalid ? TEXT("The retarget mapping requires an FK chains operation.")
						: TEXT("The retarget mapping supports only operation stack index zero."), Retargeter);
				if (bTypeInvalid)
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(Mapping.OperationRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("/Script/IKRig.IKRetargetFKChainsOp"));
				}
				else
				{
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Mapping.OrderIndex);
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(0);
				}
				if (Mapping.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					OutError.PropertyPath = bTypeInvalid ? TEXT("/operation_ref") : TEXT("/order_index");
					LocateRigInput(OutError, Mapping.InputPointer, OutError.PropertyPath);
				}
				return false;
			}
			if (!Controller->GetChainMapping(NAME_None)
				&& Controller->AddRetargetOp(FIKRetargetFKChainsOp::StaticStruct()) == INDEX_NONE)
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_op_add_failed"),
					TEXT("The IK Retargeter controller could not add the FK chains operation required for the mapping."), Retargeter,
					FString(), MakeShared<FJsonValueString>(Mapping.OperationRef));
				return false;
			}
			const FName SourceChain =
				Mapping.Intent == EAnimationRigGraphMutationIntent::Delete
					? NAME_None : Mapping.SourceChain;
			UE::UAI::Contract::FDiagnostic MappingError;
			if (!UeAgentIKAssetOps::ApplyRetargetChainMapping(*Controller,
				SourceChain, Mapping.TargetChain, NAME_None, MappingError, RetargetMappingSource(Mapping, Retargeter)))
			{
				OutError = MappingError;
				return false;
			}
		}
		Controller->CleanAsset();
		for (const FAnimationRigGraphRetargetMappingMutation& Mapping : Mappings)
		{
			const FName SourceChain = Mapping.Intent == EAnimationRigGraphMutationIntent::Delete
				? NAME_None : Mapping.SourceChain;
			UE::UAI::Contract::FDiagnostic MappingError;
			if (!UeAgentIKAssetOps::VerifyRetargetChainMapping(*Controller,
				SourceChain, Mapping.TargetChain, NAME_None, MappingError, RetargetMappingSource(Mapping, Retargeter)))
			{
				OutError = MappingError;
				return false;
			}
		}
		const FRetargetChainMapping* ChainMapping = Controller->GetChainMapping(NAME_None);
		const UIKRigDefinition* ActualSource = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
		const UIKRigDefinition* ActualTarget = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
		if (ActualSource != &SourceRig || ActualTarget != &TargetRig
			|| !ChainMapping || !ChainMapping->IsReady())
		{
			OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_mapping_missing"),
				ActualSource != &SourceRig ? TEXT("The retargeter did not retain the requested source IK Rig.")
					: ActualTarget != &TargetRig ? TEXT("The retargeter did not retain the requested target IK Rig.")
					: !ChainMapping ? TEXT("The retargeter has no chain mapping after applying the definition.")
					: TEXT("The retargeter chain mapping is not ready after applying the definition."), Retargeter);
			if (ActualSource != &SourceRig || ActualTarget != &TargetRig)
			{
				const bool bSourceMismatch = ActualSource != &SourceRig;
				const UIKRigDefinition* ActualRig = bSourceMismatch ? ActualSource : ActualTarget;
				OutError.ActualValuePresent = ActualRig != nullptr;
				if (ActualRig) OutError.ActualValue = MakeShared<FJsonValueString>(ActualRig->GetPathName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(bSourceMismatch ? SourceRig.GetPathName() : TargetRig.GetPathName());
				LocateRigInput(OutError, bSourceMismatch ? SourceRigInputPointer : TargetRigInputPointer);
			}
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
			OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_settings_unavailable"),
				!Controller ? TEXT("The IK Retargeter controller is unavailable for settings changes.")
					: !OpController ? TEXT("The first retarget operation has no FK chains controller for settings changes.")
					: TEXT("At least one retarget setting mutation is required."), Retargeter);
			return false;
		}
		Retargeter.Modify();
		FIKRetargetFKChainsOpSettings Settings = OpController->GetSettings();
		for (const FAnimationRigGraphRetargetSettingMutation& Mutation : Mutations)
		{
			ON_SCOPE_EXIT { LocateRigInput(OutError, Mutation.InputPointer); };
			if (Mutation.Scope != TEXT("chain") || Mutation.ChainName.IsNone())
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_scope_unsupported"),
					Mutation.Scope != TEXT("chain") ? TEXT("Retarget settings support only chain scope.")
						: TEXT("The retarget setting has no chain name."), Retargeter, FString(),
					MakeShared<FJsonValueString>(Mutation.Scope != TEXT("chain") ? Mutation.Scope : Mutation.ChainName.ToString()),
					MakeShared<FJsonValueString>(Mutation.Scope != TEXT("chain") ? TEXT("chain") : TEXT("nonempty chain name")));
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
				int64 Value = 0;
				if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(StaticEnum<EFKChainRotationMode>(), Mutation.Value, Value))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						FString::Printf(TEXT("RotationMode for retarget chain '%s' is not a declared rotation-mode value."), *Mutation.ChainName.ToString()),
						Retargeter, FString(), MakeShared<FJsonValueString>(Mutation.Value), MakeShared<FJsonValueString>(StaticEnum<EFKChainRotationMode>()->GetPathName()));
					if (const auto Choices = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(StaticEnum<EFKChainRotationMode>()))
						OutError.CandidateChoices = MakeShared<FJsonValueObject>(Choices);
					return false;
				}
				Chain->RotationMode = static_cast<EFKChainRotationMode>(Value);
			}
			else if (Mutation.SettingName == TEXT("TranslationMode"))
			{
				int64 Value = 0;
				if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(StaticEnum<EFKChainTranslationMode>(), Mutation.Value, Value))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						FString::Printf(TEXT("TranslationMode for retarget chain '%s' is not a declared translation-mode value."), *Mutation.ChainName.ToString()),
						Retargeter, FString(), MakeShared<FJsonValueString>(Mutation.Value), MakeShared<FJsonValueString>(StaticEnum<EFKChainTranslationMode>()->GetPathName()));
					if (const auto Choices = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(StaticEnum<EFKChainTranslationMode>()))
						OutError.CandidateChoices = MakeShared<FJsonValueObject>(Choices);
					return false;
				}
				Chain->TranslationMode = static_cast<EFKChainTranslationMode>(Value);
			}
			else if (Mutation.SettingName == TEXT("RotationAlpha"))
			{
				if (!LexTryParseString(Chain->RotationAlpha, *Mutation.Value))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						FString::Printf(TEXT("RotationAlpha for retarget chain '%s' could not be parsed as a floating-point value."), *Mutation.ChainName.ToString()),
						Retargeter, FString(), MakeShared<FJsonValueString>(Mutation.Value), MakeShared<FJsonValueString>(TEXT("floating-point value")));
					return false;
				}
			}
			else if (Mutation.SettingName == TEXT("TranslationAlpha"))
			{
				if (!LexTryParseString(Chain->TranslationAlpha, *Mutation.Value))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						FString::Printf(TEXT("TranslationAlpha for retarget chain '%s' could not be parsed as a floating-point value."), *Mutation.ChainName.ToString()),
						Retargeter, FString(), MakeShared<FJsonValueString>(Mutation.Value), MakeShared<FJsonValueString>(TEXT("floating-point value")));
					return false;
				}
			}
			else if (Mutation.SettingName == TEXT("EnableFK"))
			{
				if (!LexTryParseString(Chain->EnableFK, *Mutation.Value))
				{
					OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_value_invalid"),
						FString::Printf(TEXT("EnableFK for retarget chain '%s' could not be parsed as a boolean."), *Mutation.ChainName.ToString()),
						Retargeter, FString(), MakeShared<FJsonValueString>(Mutation.Value), MakeShared<FJsonValueString>(TEXT("boolean")));
					return false;
				}
			}
			else
			{
				OutError = RigNativeFailure(TEXT("animation_rig_graph_retarget_setting_unsupported"),
					FString::Printf(TEXT("The requested setting is unsupported for retarget chain '%s'."), *Mutation.ChainName.ToString()), Retargeter,
					FString(), MakeShared<FJsonValueString>(Mutation.SettingName),
					MakeShared<FJsonValueString>(TEXT("RotationMode, TranslationMode, RotationAlpha, TranslationAlpha or EnableFK")));
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
            OutError = FAnimationRigGraphAdapterError::Failure(TEXT("animation_rig_graph_concrete_type_required"),
                TEXT("Concrete type support is only defined for the deformer source binding and ML deformer configuration mutation families."),
                TEXT("native_graph"));
            OutError.SourceKind = TEXT("native_result");
            OutError.PropertyPath = TEXT("family");
            OutError.ActualValue = MakeShared<FJsonValueString>(FamilyId);
            OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("deformer_source_binding_mutation or ml_deformer_configuration_mutation"));
			return false;
		}
		OutSupport.FamilyId = FamilyId;
		OutSupport.NativeClassPath = NativeClassPath;
        OutError = FAnimationRigGraphAdapterError::Failure(TEXT("animation_rig_graph_concrete_type_unavailable"),
            TEXT("No native mutation hook is available for the requested concrete type in this family."), TEXT("native_graph"));
        OutError.SourceKind = TEXT("native_result");
        OutError.SubjectId = FamilyId;
        OutError.PropertyPath = TEXT("native_class");
        OutError.ActualValue = MakeShared<FJsonValueString>(NativeClassPath);
        OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("concrete type with a supported native mutation hook"));
		return false;
	}
}
