#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphStructureCapture.h"

#include "ControlRigBlueprint.h"
#include "ControlRigGizmoLibrary.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "Rigs/RigHierarchy.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	namespace
	{
		FString ControlTypeText(const ERigControlType Type)
		{
			const UEnum* Enum = StaticEnum<ERigControlType>();
			return Enum
				? Enum->GetNameStringByValue(static_cast<int64>(Type))
				: FString();
		}

		TSharedRef<FJsonObject> PositionJson(const FVector2D& Position)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("x"), Position.X);
			Value->SetNumberField(TEXT("y"), Position.Y);
			return Value;
		}

		void AddValue(
			TArray<FAnimationRigGraphStructureValue>& OutValues,
			FString SemanticId,
			FString ItemKind,
			FString ClassPath,
			TSharedRef<FJsonObject> Value)
		{
			OutValues.Add({
				MoveTemp(SemanticId),
				MoveTemp(ItemKind),
				MoveTemp(ClassPath),
				MoveTemp(Value),
			});
		}
	}

	bool TryCaptureAnimationRigGraphStructure(
		UObject& Target,
		TArray<FAnimationRigGraphStructureValue>& OutValues,
		FAnimationRigGraphAdapterError& OutError)
	{
		OutValues.Reset();
		OutError.Reset();
		if (UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Target))
		{
			URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
			if (!Hierarchy)
			{
				OutError.Set(
					TEXT("animation_rig_graph_structure_hierarchy_unavailable"),
					Target.GetPathName());
				return false;
			}
			for (int32 Index = 0; Index < Hierarchy->Num(); ++Index)
			{
				const FRigElementKey Key = Hierarchy->GetKey(Index);
				if (Key.Type != ERigElementType::Control)
				{
					continue;
				}
				const FRigControlElement* Control =
					Hierarchy->Find<FRigControlElement>(Key);
				if (!Control)
				{
					continue;
				}
				const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("native_key"), TEXT("Control:") + Key.Name.ToString());
				Value->SetStringField(
					TEXT("parent_native_key"),
					Parent.IsValid()
						? TEXT("Bone:") + Parent.Name.ToString()
						: FString());
				Value->SetStringField(
					TEXT("control_type_ref"),
					ControlTypeText(Control->Settings.ControlType));
				AddValue(
					OutValues,
					TEXT("rig_element:control:") + Key.Name.ToString(),
					TEXT("control"),
					TEXT("/Script/ControlRig.RigControlElement"),
					Value);
			}
			if (const URigVMGraph* Graph = Blueprint->GetDefaultModel())
			{
				for (const URigVMNode* Node : Graph->GetNodes())
				{
					const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
					if (!Unit || !Unit->GetScriptStruct())
					{
						continue;
					}
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(
						TEXT("script_struct_ref"),
						Unit->GetScriptStruct()->GetPathName());
					Value->SetStringField(
						TEXT("method"), Unit->GetMethodName().ToString());
					Value->SetObjectField(
						TEXT("position"), PositionJson(Unit->GetPosition()));
					AddValue(
						OutValues,
						TEXT("rigvm_node:") + Unit->GetName(),
						TEXT("rig_unit"),
						Unit->GetScriptStruct()->GetPathName(),
						Value);
				}
			}
			return true;
		}

		if (UControlRigShapeLibrary* Library =
			Cast<UControlRigShapeLibrary>(&Target))
		{
			for (int32 Index = 0; Index < Library->Shapes.Num(); ++Index)
			{
				const FControlRigShapeDefinition& Shape = Library->Shapes[Index];
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("mesh_ref"),
					Shape.StaticMesh.ToSoftObjectPath().ToString());
				Value->SetStringField(
					TEXT("material_ref"),
					Library->DefaultMaterial.ToSoftObjectPath().ToString());
				Value->SetNumberField(TEXT("order_index"), Index);
				AddValue(
					OutValues,
					TEXT("item:shape:") + Shape.ShapeName.ToString(),
					TEXT("shape"),
					TEXT("/Script/ControlRig.ControlRigShapeDefinition"),
					Value);
			}
			return true;
		}

		if (UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Target))
		{
			UIKRigController* Controller = UIKRigController::GetController(Rig);
			if (!Controller)
			{
				OutError.Set(
					TEXT("animation_rig_graph_structure_ik_rig_unavailable"),
					Target.GetPathName());
				return false;
			}
			const TSharedRef<FJsonObject> Mesh = MakeShared<FJsonObject>();
			Mesh->SetStringField(
				TEXT("skeletal_mesh_ref"),
				Controller->GetSkeletalMesh()
					? Controller->GetSkeletalMesh()->GetPathName()
					: FString());
			AddValue(
				OutValues,
				TEXT("binding:skeletal_mesh"),
				TEXT("binding"),
				TEXT("/Script/Engine.SkeletalMesh"),
				Mesh);

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(
				TEXT("value"), Controller->GetRetargetRoot().ToString());
			AddValue(
				OutValues,
				TEXT("property:retarget_root"),
				TEXT("property"),
				TEXT("/Script/IKRig.IKRigDefinition"),
				Root);
			for (UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
			{
				if (!Goal)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("bone_name"),
					Controller->GetBoneForGoal(Goal->GoalName).ToString());
				AddValue(
					OutValues,
					TEXT("item:ik_goal:") + Goal->GoalName.ToString(),
					TEXT("ik_goal"),
					TEXT("/Script/IKRig.IKRigEffectorGoal"),
					Value);
			}
			for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index)
			{
				const FInstancedStruct* Solver =
					Controller->GetSolverStructAtIndex(Index);
				const FString SolverClass = Solver && Solver->GetScriptStruct()
					? Solver->GetScriptStruct()->GetPathName()
					: FString();
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("solver_class_ref"), SolverClass);
				Value->SetNumberField(TEXT("order_index"), Index);
				AddValue(
					OutValues,
					FString::Printf(TEXT("order:ik_solver:solver:%d"), Index),
					TEXT("ik_solver"),
					SolverClass,
					Value);
			}
			for (const FBoneChain& Chain : Controller->GetRetargetChains())
			{
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
				Value->SetStringField(
					TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
				Value->SetStringField(
					TEXT("goal_name"), Chain.IKGoalName.ToString());
				AddValue(
					OutValues,
					TEXT("hierarchy:ik_chain:") + Chain.ChainName.ToString(),
					TEXT("retarget_chain"),
					TEXT("/Script/IKRig.BoneChain"),
					Value);
			}
			return true;
		}

		UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Target);
		UIKRetargeterController* Controller = Retargeter
			? UIKRetargeterController::GetController(Retargeter)
			: nullptr;
		if (!Controller)
		{
			OutError.Set(
				TEXT("animation_rig_graph_structure_target_unsupported"),
				Target.GetClass()->GetPathName());
			return false;
		}

		const UIKRigDefinition* Source =
			Controller->GetIKRig(ERetargetSourceOrTarget::Source);
		const UIKRigDefinition* TargetRig =
			Controller->GetIKRig(ERetargetSourceOrTarget::Target);
		const TSharedRef<FJsonObject> SourceValue = MakeShared<FJsonObject>();
		SourceValue->SetStringField(
			TEXT("value"), Source ? Source->GetPathName() : FString());
		AddValue(
			OutValues,
			TEXT("binding:source_ik_rig"),
			TEXT("binding"),
			TEXT("/Script/IKRig.IKRigDefinition"),
			SourceValue);
		const TSharedRef<FJsonObject> TargetValue = MakeShared<FJsonObject>();
		TargetValue->SetStringField(
			TEXT("value"), TargetRig ? TargetRig->GetPathName() : FString());
		AddValue(
			OutValues,
			TEXT("binding:target_ik_rig"),
			TEXT("binding"),
			TEXT("/Script/IKRig.IKRigDefinition"),
			TargetValue);

		const FRetargetChainMapping* Mapping =
			Controller->GetChainMapping(NAME_None);
		if (Mapping && Mapping->IsReady())
		{
			for (const FName& TargetChain : Mapping->GetChainNames(
				ERetargetSourceOrTarget::Target))
			{
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("source_chain"),
					Mapping->GetChainMappedTo(
						TargetChain,
						ERetargetSourceOrTarget::Target).ToString());
				Value->SetStringField(
					TEXT("operation_ref"),
					TEXT("/Script/IKRig.IKRetargetFKChainsOp"));
				Value->SetNumberField(TEXT("order_index"), 0);
				AddValue(
					OutValues,
					TEXT("binding:chain_mapping:") + TargetChain.ToString(),
					TEXT("chain_mapping"),
					TEXT("/Script/IKRig.RetargetChainSettings"),
					Value);
			}
		}

		UIKRetargetFKChainsController* OpController =
			Cast<UIKRetargetFKChainsController>(Controller->GetOpController(0));
		if (!OpController)
		{
			return true;
		}
		const FIKRetargetFKChainsOpSettings Settings = OpController->GetSettings();
		for (const FRetargetFKChainSettings& Chain : Settings.ChainsToRetarget)
		{
			auto AddSetting =
				[&OutValues, &Chain](const FString& Name, const FString& Value)
				{
					const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
					Object->SetStringField(TEXT("value"), Value);
					AddValue(
						OutValues,
						TEXT("property:setting:chain:")
							+ Chain.TargetChainName.ToString()
							+ TEXT(":") + Name,
						TEXT("setting"),
						TEXT("/Script/IKRig.RetargetFKChainSettings"),
						Object);
				};
			AddSetting(
				TEXT("RotationMode"),
				StaticEnum<EFKChainRotationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain.RotationMode)));
			AddSetting(
				TEXT("TranslationMode"),
				StaticEnum<EFKChainTranslationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain.TranslationMode)));
			AddSetting(
				TEXT("RotationAlpha"),
				FString::SanitizeFloat(Chain.RotationAlpha));
			AddSetting(
				TEXT("TranslationAlpha"),
				FString::SanitizeFloat(Chain.TranslationAlpha));
			AddSetting(
				TEXT("EnableFK"), Chain.EnableFK ? TEXT("true") : TEXT("false"));
		}
		return true;
	}
}
