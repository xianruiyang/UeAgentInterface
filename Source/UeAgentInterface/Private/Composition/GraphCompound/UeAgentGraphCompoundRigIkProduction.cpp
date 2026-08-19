#include "Composition/GraphCompound/UeAgentGraphCompoundRigIkProduction.h"

#include "Animation/Skeleton.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigGizmoLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Factories/Factory.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/FKChainsOp.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace StateSemantic;

	namespace
	{
		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString Subject = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(Subject);
			OutError.Detail = MoveTemp(Detail);
		}

		EAnimationRigGraphMutationIntent NativeIntent(const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EAnimationRigGraphMutationIntent::Create;
			case EChangeIntent::Update:
				return EAnimationRigGraphMutationIntent::Update;
			case EChangeIntent::Replace:
				return EAnimationRigGraphMutationIntent::Replace;
			case EChangeIntent::Delete:
				return EAnimationRigGraphMutationIntent::Delete;
			}
			return EAnimationRigGraphMutationIntent::Update;
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Object.GetObjectItems())
			{
				if (Pair.Key == Name)
				{
					return &Pair.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryStringField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value || Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return true;
		}

		bool TryIntegerField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value)
			{
				return false;
			}
			if (Value->GetKind() == EUeAgentValueKind::Integer)
			{
				return LexTryParseString(OutValue, *Value->GetLexeme());
			}
			if (Value->GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			double Decimal = 0.0;
			if (!LexTryParseString(Decimal, *Value->GetLexeme())
				|| !FMath::IsNearlyEqual(Decimal, FMath::RoundToDouble(Decimal))
				|| Decimal < static_cast<double>(MIN_int32)
				|| Decimal > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(FMath::RoundToDouble(Decimal));
			return true;
		}

		bool TryScalarText(const FUeAgentCanonicalValue& Value, FString& OutValue)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::String:
				OutValue = Value.GetText();
				return true;
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				OutValue = Value.GetLexeme();
				return true;
			case EUeAgentValueKind::Boolean:
				OutValue = Value.GetBoolean() ? TEXT("true") : TEXT("false");
				return true;
			default:
				return false;
			}
		}

		FString ObjectPath(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? TargetRef : TargetRef + TEXT(".")
					+ FPackageName::GetLongPackageAssetName(TargetRef);
		}

		FString AssetName(const FString& TargetRef)
		{
			return TargetRef.Contains(TEXT("."))
				? FPackageName::ObjectPathToObjectName(TargetRef)
				: FPackageName::GetLongPackageAssetName(TargetRef);
		}

		FString ControlTypeText(const ERigControlType Type)
		{
			const UEnum* Enum = StaticEnum<ERigControlType>();
			return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Type)) : FString();
		}

		TSharedRef<FJsonObject> Observation(
			const FString& Identity,
			const bool bPresent)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("identity"), Identity);
			Value->SetBoolField(TEXT("present"), bPresent);
			return Value;
		}

		TSharedPtr<FJsonValue> JsonScalar(const FString& Value)
		{
			return MakeShared<FJsonValueString>(Value);
		}
	}

	bool IsRigIkCreateProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Capability = Profile.GetCapabilityId();
		return Capability == TEXT("animation.control_rig.asset.create")
			|| Capability == TEXT("animation.control_rig.shape_library.asset.create")
			|| Capability == TEXT("animation.ik_rig.asset.create")
			|| Capability == TEXT("animation.ik_retargeter.asset.create");
	}

	bool IsRigIkMutationProfile(const FPcgAnimationGraphRouteProfile& Profile)
	{
		const FString& Capability = Profile.GetCapabilityId();
		return Capability == TEXT("animation.control_rig.hierarchy_graph.apply")
			|| Capability == TEXT("animation.control_rig.shape_library.apply")
			|| Capability == TEXT("animation.ik_rig.definition.apply")
			|| Capability == TEXT("animation.ik_rig.definition.auto_generate")
			|| Capability == TEXT("animation.ik_retargeter.definition.apply")
			|| Capability == TEXT("animation.ik_retargeter.settings.apply");
	}

	bool TryCaptureRigIkValues(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		TArray<FRigIkCapturedValue>& OutValues,
		FGraphCompoundRuntimeError& OutError)
	{
		OutValues.Reset();
		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.control_rig.hierarchy_graph.apply"))
		{
			UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Target);
			URigHierarchy* Hierarchy = Blueprint ? Blueprint->GetHierarchy() : nullptr;
			if (!Hierarchy)
			{
				SetError(OutError, TEXT("graph_compound_animation_target_class_mismatch"),
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
				const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
				const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
				if (!Control)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("native_key"),
					TEXT("Control:") + Key.Name.ToString());
				Value->SetStringField(TEXT("parent_native_key"),
					Parent.IsValid() ? TEXT("Bone:") + Parent.Name.ToString() : FString());
				Value->SetStringField(TEXT("control_type_ref"),
					ControlTypeText(Control->Settings.ControlType));
				OutValues.Add({TEXT("rig_element:control:") + Key.Name.ToString(), Value});
			}
			return true;
		}
		if (Capability == TEXT("animation.control_rig.shape_library.apply"))
		{
			UControlRigShapeLibrary* Library = Cast<UControlRigShapeLibrary>(&Target);
			if (!Library)
			{
				SetError(OutError, TEXT("graph_compound_animation_target_class_mismatch"),
					Target.GetPathName());
				return false;
			}
			for (int32 Index = 0; Index < Library->Shapes.Num(); ++Index)
			{
				const FControlRigShapeDefinition& Shape = Library->Shapes[Index];
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("mesh_ref"),
					Shape.StaticMesh.ToSoftObjectPath().ToString());
				Value->SetStringField(TEXT("material_ref"),
					Library->DefaultMaterial.ToSoftObjectPath().ToString());
				Value->SetNumberField(TEXT("order_index"), Index);
				OutValues.Add({TEXT("item:shape:") + Shape.ShapeName.ToString(), Value});
			}
			return true;
		}
		if (Capability == TEXT("animation.ik_rig.definition.apply")
			|| Capability == TEXT("animation.ik_rig.definition.auto_generate"))
		{
			UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&Target);
			UIKRigController* Controller = Rig
				? UIKRigController::GetController(Rig) : nullptr;
			if (!Controller)
			{
				SetError(OutError, TEXT("graph_compound_animation_target_class_mismatch"),
					Target.GetPathName());
				return false;
			}
			if (Capability == TEXT("animation.ik_rig.definition.auto_generate"))
			{
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("skeletal_mesh_ref"),
					Controller->GetSkeletalMesh()
						? Controller->GetSkeletalMesh()->GetPathName() : FString());
				// The preset is an operation input, not persisted UE state. Leaving the
				// observed value empty prevents read-before-plan from inventing a preset
				// and incorrectly collapsing an explicit auto-generate request to no-op.
				Value->SetStringField(TEXT("preset"), FString());
				OutValues.Add({TEXT("property:definition:auto_generate"), Value});
				return true;
			}
			OutValues.Add({TEXT("property:retarget_root"),
				MakeShared<FJsonObject>()});
			OutValues.Last().Value->SetStringField(
				TEXT("value"), Controller->GetRetargetRoot().ToString());
			for (UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
			{
				if (!Goal)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("bone_name"),
					Controller->GetBoneForGoal(Goal->GoalName).ToString());
				OutValues.Add({TEXT("item:ik_goal:") + Goal->GoalName.ToString(), Value});
			}
			for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index)
			{
				const FInstancedStruct* Solver = Controller->GetSolverStructAtIndex(Index);
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("solver_class_ref"),
					Solver && Solver->GetScriptStruct()
						? Solver->GetScriptStruct()->GetPathName() : FString());
				Value->SetNumberField(TEXT("order_index"), Index);
				OutValues.Add({FString::Printf(TEXT("order:ik_solver:solver:%d"), Index),
					Value});
			}
			for (const FBoneChain& Chain : Controller->GetRetargetChains())
			{
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
				Value->SetStringField(TEXT("end_bone"), Chain.EndBone.BoneName.ToString());
				Value->SetStringField(TEXT("goal_name"), Chain.IKGoalName.ToString());
				OutValues.Add({TEXT("hierarchy:ik_chain:") + Chain.ChainName.ToString(),
					Value});
			}
			return true;
		}

		UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&Target);
		UIKRetargeterController* Controller = Retargeter
			? UIKRetargeterController::GetController(Retargeter) : nullptr;
		if (!Controller)
		{
			SetError(OutError, TEXT("graph_compound_animation_target_class_mismatch"),
				Target.GetPathName());
			return false;
		}
		if (Capability == TEXT("animation.ik_retargeter.definition.apply"))
		{
			const UIKRigDefinition* Source =
				Controller->GetIKRig(ERetargetSourceOrTarget::Source);
			const UIKRigDefinition* TargetRig =
				Controller->GetIKRig(ERetargetSourceOrTarget::Target);
			const TSharedRef<FJsonObject> SourceValue = MakeShared<FJsonObject>();
			SourceValue->SetStringField(TEXT("value"), Source ? Source->GetPathName() : FString());
			OutValues.Add({TEXT("binding:source_ik_rig"), SourceValue});
			const TSharedRef<FJsonObject> TargetValue = MakeShared<FJsonObject>();
			TargetValue->SetStringField(TEXT("value"), TargetRig ? TargetRig->GetPathName() : FString());
			OutValues.Add({TEXT("binding:target_ik_rig"), TargetValue});
			const FRetargetChainMapping* Mapping = Controller->GetChainMapping(NAME_None);
			if (Mapping && Mapping->IsReady())
			{
				for (const FName& TargetChain : Mapping->GetChainNames(
					ERetargetSourceOrTarget::Target))
				{
					const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("source_chain"), Mapping->GetChainMappedTo(
						TargetChain, ERetargetSourceOrTarget::Target).ToString());
					Value->SetStringField(TEXT("operation_ref"),
						TEXT("/Script/IKRig.IKRetargetFKChainsOp"));
					Value->SetNumberField(TEXT("order_index"), 0);
					OutValues.Add({TEXT("binding:chain_mapping:")
						+ TargetChain.ToString(), Value});
				}
			}
			return true;
		}
		UIKRetargetFKChainsController* OpController = Cast<UIKRetargetFKChainsController>(
			Controller->GetOpController(0));
		if (!OpController)
		{
			return true;
		}
		const FIKRetargetFKChainsOpSettings Settings = OpController->GetSettings();
		for (const FRetargetFKChainSettings& Chain : Settings.ChainsToRetarget)
		{
			auto AddSetting = [&](const FString& Name, const FString& Value)
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("value"), Value);
				OutValues.Add({TEXT("property:setting:chain:")
					+ Chain.TargetChainName.ToString() + TEXT(":") + Name, Object});
			};
			AddSetting(TEXT("RotationMode"),
				StaticEnum<EFKChainRotationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain.RotationMode)));
			AddSetting(TEXT("TranslationMode"),
				StaticEnum<EFKChainTranslationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain.TranslationMode)));
			AddSetting(TEXT("RotationAlpha"), FString::SanitizeFloat(Chain.RotationAlpha));
			AddSetting(TEXT("TranslationAlpha"), FString::SanitizeFloat(Chain.TranslationAlpha));
			AddSetting(TEXT("EnableFK"), Chain.EnableFK ? TEXT("true") : TEXT("false"));
		}
		return true;
	}

	bool TryParseRigIkReadbackSpec(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		FRigIkReadbackSpec& OutSpec,
		FGraphCompoundRuntimeError& OutError)
	{
		OutSpec = {};
		OutSpec.TargetKind = Request.GetTargetKind();
		const FString& Capability = Profile.GetCapabilityId();
		const TArray<FGraphCompoundCanonicalEdit>& Edits = Request.GetPayload().GetEdits();
		if (Edits.IsEmpty())
		{
			SetError(OutError, TEXT("graph_compound_animation_plan_invalid"),
				Request.GetTargetRef());
			return false;
		}
		if (IsRigIkCreateProfile(Profile))
		{
			const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
			if (!Value.IsValid())
			{
				SetError(OutError, TEXT("graph_compound_animation_create_value_missing"));
				return false;
			}
			if (Capability == TEXT("animation.control_rig.asset.create"))
			{
				OutSpec.Kind = ERigIkReadbackKind::ControlRigCreate;
				OutSpec.NativeClassPath = UControlRigBlueprint::StaticClass()->GetPathName();
				if (!TryStringField(*Value, TEXT("skeleton_ref"), OutSpec.SkeletonRef))
				{
					SetError(OutError, TEXT("graph_compound_animation_create_reference_missing"));
					return false;
				}
			}
			else if (Capability
				== TEXT("animation.control_rig.shape_library.asset.create"))
			{
				OutSpec.Kind = ERigIkReadbackKind::ShapeLibraryCreate;
				OutSpec.NativeClassPath = UControlRigShapeLibrary::StaticClass()->GetPathName();
			}
			else if (Capability == TEXT("animation.ik_rig.asset.create"))
			{
				OutSpec.Kind = ERigIkReadbackKind::IkRigCreate;
				OutSpec.NativeClassPath = UIKRigDefinition::StaticClass()->GetPathName();
				if (!TryStringField(*Value, TEXT("skeletal_mesh_ref"),
					OutSpec.SkeletalMeshRef))
				{
					SetError(OutError, TEXT("graph_compound_animation_create_reference_missing"));
					return false;
				}
			}
			else
			{
				OutSpec.Kind = ERigIkReadbackKind::IkRetargeterCreate;
				OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
				if (!TryStringField(*Value, TEXT("source_ik_rig_ref"), OutSpec.SourceRigRef)
					|| !TryStringField(*Value, TEXT("target_ik_rig_ref"),
						OutSpec.TargetRigRef))
				{
					SetError(OutError, TEXT("graph_compound_animation_create_reference_missing"));
					return false;
				}
			}
			return true;
		}

		if (Capability == TEXT("animation.control_rig.hierarchy_graph.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::ControlRigDefinition;
			OutSpec.NativeClassPath = UControlRigBlueprint::StaticClass()->GetPathName();
			const FString Prefix = TEXT("rig_element:control:");
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				if (!Edit.GetIdentity().GetSemanticId().StartsWith(Prefix))
				{
					SetError(OutError, TEXT("graph_compound_control_rig_identity_unsupported"),
						Edit.GetIdentity().GetSemanticId());
					return false;
				}
				FAnimationRigGraphControlMutation Mutation;
				Mutation.Name = *Edit.GetIdentity().GetSemanticId().Mid(Prefix.Len());
				Mutation.Intent = NativeIntent(Edit.GetIntent());
				if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					FString ParentKey;
					if (!Edit.GetProposedAfter().IsValid()
						|| !TryStringField(*Edit.GetProposedAfter(), TEXT("parent_native_key"),
							ParentKey)
						|| !ParentKey.StartsWith(TEXT("Bone:"))
						|| !TryStringField(*Edit.GetProposedAfter(), TEXT("control_type_ref"),
							Mutation.ControlTypeRef))
					{
						SetError(OutError, TEXT("graph_compound_control_rig_edit_invalid"),
							Mutation.Name.ToString());
						return false;
					}
					Mutation.ParentBone = *ParentKey.Mid(5);
				}
				OutSpec.Controls.Add(MoveTemp(Mutation));
			}
			return true;
		}
		if (Capability == TEXT("animation.control_rig.shape_library.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::ShapeLibraryDefinition;
			OutSpec.NativeClassPath = UControlRigShapeLibrary::StaticClass()->GetPathName();
			const FString Prefix = TEXT("item:shape:");
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				FAnimationRigGraphShapeMutation Mutation;
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (!SemanticId.StartsWith(Prefix))
				{
					SetError(OutError, TEXT("graph_compound_shape_identity_invalid"), SemanticId);
					return false;
				}
				Mutation.Name = *SemanticId.Mid(Prefix.Len());
				Mutation.Intent = NativeIntent(Edit.GetIntent());
				if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					FString MeshRef;
					FString MaterialRef;
					if (!Edit.GetProposedAfter().IsValid())
					{
						SetError(OutError, TEXT("graph_compound_shape_value_missing"),
							Mutation.Name.ToString());
						return false;
					}
					if (!TryStringField(*Edit.GetProposedAfter(), TEXT("mesh_ref"), MeshRef))
					{
						SetError(OutError, TEXT("graph_compound_shape_mesh_field_invalid"),
							Mutation.Name.ToString());
						return false;
					}
					if (!TryStringField(*Edit.GetProposedAfter(), TEXT("material_ref"),
						MaterialRef))
					{
						SetError(OutError, TEXT("graph_compound_shape_material_field_invalid"),
							Mutation.Name.ToString());
						return false;
					}
					if (!TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"),
						Mutation.OrderIndex))
					{
						SetError(OutError, TEXT("graph_compound_shape_order_field_invalid"),
							Mutation.Name.ToString());
						return false;
					}
					Mutation.Mesh = LoadObject<UStaticMesh>(nullptr, *MeshRef);
					Mutation.Material = LoadObject<UMaterial>(nullptr, *MaterialRef);
					if (!Mutation.Mesh || !Mutation.Material)
					{
						SetError(OutError, TEXT("graph_compound_shape_reference_invalid"),
							Mutation.Name.ToString());
						return false;
					}
				}
				OutSpec.Shapes.Add(MoveTemp(Mutation));
			}
			return true;
		}
		if (Capability == TEXT("animation.ik_rig.definition.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::IkRigDefinition;
			OutSpec.NativeClassPath = UIKRigDefinition::StaticClass()->GetPathName();
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (SemanticId == TEXT("property:retarget_root"))
				{
					if (!Edit.GetProposedAfter().IsValid()
						|| Edit.GetProposedAfter()->GetKind() != EUeAgentValueKind::String)
					{
						SetError(OutError, TEXT("graph_compound_ik_root_invalid"));
						return false;
					}
					OutSpec.RetargetRoot = *Edit.GetProposedAfter()->GetText();
				}
				else if (SemanticId.StartsWith(TEXT("item:ik_goal:")))
				{
					FAnimationRigGraphIkGoalMutation Goal;
					Goal.Name = *SemanticId.Mid(13);
					Goal.Intent = NativeIntent(Edit.GetIntent());
					FString Bone;
					if (Goal.Intent != EAnimationRigGraphMutationIntent::Delete
						&& (!Edit.GetProposedAfter().IsValid()
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("bone_name"), Bone)))
					{
						SetError(OutError, TEXT("graph_compound_ik_goal_invalid"));
						return false;
					}
					Goal.BoneName = *Bone;
					OutSpec.Goals.Add(MoveTemp(Goal));
				}
				else if (SemanticId.StartsWith(TEXT("order:ik_solver:")))
				{
					FAnimationRigGraphIkSolverMutation Solver;
					Solver.Intent = NativeIntent(Edit.GetIntent());
					if (Solver.Intent != EAnimationRigGraphMutationIntent::Delete
						&& (!Edit.GetProposedAfter().IsValid()
							|| !TryStringField(*Edit.GetProposedAfter(),
								TEXT("solver_class_ref"), Solver.SolverClassRef)
							|| !TryIntegerField(*Edit.GetProposedAfter(),
								TEXT("order_index"), Solver.OrderIndex)))
					{
						SetError(OutError, TEXT("graph_compound_ik_solver_invalid"));
						return false;
					}
					OutSpec.Solvers.Add(MoveTemp(Solver));
				}
				else if (SemanticId.StartsWith(TEXT("hierarchy:ik_chain:")))
				{
					FAnimationRigGraphIkChainMutation Chain;
					Chain.Name = *SemanticId.Mid(19);
					Chain.Intent = NativeIntent(Edit.GetIntent());
					FString Start;
					FString End;
					FString Goal;
					if (Chain.Intent != EAnimationRigGraphMutationIntent::Delete
						&& (!Edit.GetProposedAfter().IsValid()
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("start_bone"), Start)
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("end_bone"), End)
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("goal_name"), Goal)))
					{
						SetError(OutError, TEXT("graph_compound_ik_chain_invalid"));
						return false;
					}
					Chain.StartBone = *Start;
					Chain.EndBone = *End;
					Chain.GoalName = *Goal;
					OutSpec.Chains.Add(MoveTemp(Chain));
				}
			}
			return !OutSpec.RetargetRoot.IsNone();
		}
		if (Capability == TEXT("animation.ik_rig.definition.auto_generate"))
		{
			OutSpec.Kind = ERigIkReadbackKind::IkRigAutoGenerate;
			OutSpec.NativeClassPath = UIKRigDefinition::StaticClass()->GetPathName();
			const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
			if (!Value.IsValid()
				|| !TryStringField(*Value, TEXT("skeletal_mesh_ref"), OutSpec.SkeletalMeshRef)
				|| !TryStringField(*Value, TEXT("preset"), OutSpec.Preset))
			{
				SetError(OutError, TEXT("graph_compound_ik_auto_generate_invalid"));
				return false;
			}
			return true;
		}
		if (Capability == TEXT("animation.ik_retargeter.definition.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::IkRetargeterDefinition;
			OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (SemanticId == TEXT("binding:source_ik_rig")
					|| SemanticId == TEXT("binding:target_ik_rig"))
				{
					if (!Edit.GetProposedAfter().IsValid()
						|| Edit.GetProposedAfter()->GetKind() != EUeAgentValueKind::String)
					{
						SetError(OutError, TEXT("graph_compound_retarget_rig_ref_invalid"));
						return false;
					}
					FString& Ref = SemanticId.Contains(TEXT("source"))
						? OutSpec.SourceRigRef : OutSpec.TargetRigRef;
					Ref = Edit.GetProposedAfter()->GetText();
				}
				else if (SemanticId.StartsWith(TEXT("binding:chain_mapping:")))
				{
					FAnimationRigGraphRetargetMappingMutation Mapping;
					Mapping.TargetChain = *SemanticId.Mid(22);
					Mapping.Intent = NativeIntent(Edit.GetIntent());
					FString Source;
					if (Mapping.Intent != EAnimationRigGraphMutationIntent::Delete
						&& (!Edit.GetProposedAfter().IsValid()
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("source_chain"), Source)
							|| !TryStringField(*Edit.GetProposedAfter(), TEXT("operation_ref"),
								Mapping.OperationRef)
							|| !TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"),
								Mapping.OrderIndex)))
					{
						SetError(OutError, TEXT("graph_compound_retarget_mapping_invalid"));
						return false;
					}
					Mapping.SourceChain = *Source;
					OutSpec.Mappings.Add(MoveTemp(Mapping));
				}
			}
			return !OutSpec.SourceRigRef.IsEmpty() && !OutSpec.TargetRigRef.IsEmpty()
				&& !OutSpec.Mappings.IsEmpty();
		}

		OutSpec.Kind = ERigIkReadbackKind::IkRetargeterSettings;
		OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
		const FString Prefix = TEXT("property:setting:");
		for (const FGraphCompoundCanonicalEdit& Edit : Edits)
		{
			const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
			TArray<FString> Parts;
			SemanticId.Mid(Prefix.Len()).ParseIntoArray(Parts, TEXT(":"), false);
			FAnimationRigGraphRetargetSettingMutation Setting;
			if (!SemanticId.StartsWith(Prefix) || Parts.Num() != 3
				|| !Edit.GetProposedAfter().IsValid()
				|| !TryScalarText(*Edit.GetProposedAfter(), Setting.Value))
			{
				SetError(OutError, TEXT("graph_compound_retarget_setting_invalid"), SemanticId);
				return false;
			}
			Setting.Scope = Parts[0];
			Setting.ChainName = *Parts[1];
			Setting.SettingName = Parts[2];
			OutSpec.Settings.Add(MoveTemp(Setting));
		}
		return !OutSpec.Settings.IsEmpty();
	}

	TSharedRef<FJsonObject> RigIkExpectedJson(const FRigIkReadbackSpec& Spec)
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		Root->SetStringField(TEXT("native_class"), Spec.NativeClassPath);
		switch (Spec.Kind)
		{
		case ERigIkReadbackKind::ControlRigCreate:
			Root->SetStringField(TEXT("skeleton_ref"), Spec.SkeletonRef);
			break;
		case ERigIkReadbackKind::ControlRigDefinition:
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphControlMutation& Control : Spec.Controls)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("rig_element:control:") + Control.Name.ToString(),
					Control.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Control.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("native_key"),
						TEXT("Control:") + Control.Name.ToString());
					Value->SetStringField(TEXT("parent_native_key"),
						TEXT("Bone:") + Control.ParentBone.ToString());
					Value->SetStringField(TEXT("control_type_ref"), Control.ControlTypeRef);
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("controls"), MoveTemp(Values));
			break;
		}
		case ERigIkReadbackKind::ShapeLibraryCreate:
			break;
		case ERigIkReadbackKind::ShapeLibraryDefinition:
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphShapeMutation& Shape : Spec.Shapes)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("item:shape:") + Shape.Name.ToString(),
					Shape.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Shape.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("mesh_ref"), Shape.Mesh->GetPathName());
					Value->SetStringField(TEXT("material_ref"), Shape.Material->GetPathName());
					Value->SetNumberField(TEXT("order_index"), Shape.OrderIndex);
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("shapes"), MoveTemp(Values));
			break;
		}
		case ERigIkReadbackKind::IkRigCreate:
			Root->SetStringField(TEXT("skeletal_mesh_ref"), Spec.SkeletalMeshRef);
			break;
		case ERigIkReadbackKind::IkRigDefinition:
		{
			Root->SetStringField(TEXT("retarget_root"), Spec.RetargetRoot.ToString());
			TArray<TSharedPtr<FJsonValue>> Goals;
			for (const FAnimationRigGraphIkGoalMutation& Goal : Spec.Goals)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("item:ik_goal:") + Goal.Name.ToString(),
					Goal.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Goal.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("bone_name"), Goal.BoneName.ToString());
				}
				Goals.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("goals"), MoveTemp(Goals));
			TArray<TSharedPtr<FJsonValue>> Solvers;
			for (const FAnimationRigGraphIkSolverMutation& Solver : Spec.Solvers)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					FString::Printf(TEXT("solver:%d"), Solver.OrderIndex),
					Solver.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Solver.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("solver_class_ref"), Solver.SolverClassRef);
					Value->SetNumberField(TEXT("order_index"), Solver.OrderIndex);
				}
				Solvers.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("solvers"), MoveTemp(Solvers));
			TArray<TSharedPtr<FJsonValue>> Chains;
			for (const FAnimationRigGraphIkChainMutation& Chain : Spec.Chains)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("hierarchy:ik_chain:") + Chain.Name.ToString(),
					Chain.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Chain.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("start_bone"), Chain.StartBone.ToString());
					Value->SetStringField(TEXT("end_bone"), Chain.EndBone.ToString());
					Value->SetStringField(TEXT("goal_name"), Chain.GoalName.ToString());
				}
				Chains.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("chains"), MoveTemp(Chains));
			break;
		}
		case ERigIkReadbackKind::IkRigAutoGenerate:
			Root->SetStringField(TEXT("skeletal_mesh_ref"), Spec.SkeletalMeshRef);
			break;
		case ERigIkReadbackKind::IkRetargeterCreate:
			Root->SetStringField(TEXT("source_ik_rig_ref"), Spec.SourceRigRef);
			Root->SetStringField(TEXT("target_ik_rig_ref"), Spec.TargetRigRef);
			break;
		case ERigIkReadbackKind::IkRetargeterDefinition:
		{
			Root->SetStringField(TEXT("source_ik_rig_ref"), Spec.SourceRigRef);
			Root->SetStringField(TEXT("target_ik_rig_ref"), Spec.TargetRigRef);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphRetargetMappingMutation& Mapping : Spec.Mappings)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("binding:chain_mapping:") + Mapping.TargetChain.ToString(),
					Mapping.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Mapping.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(TEXT("source_chain"), Mapping.SourceChain.ToString());
					Value->SetStringField(TEXT("operation_ref"), Mapping.OperationRef);
					Value->SetNumberField(TEXT("order_index"), Mapping.OrderIndex);
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("chain_mappings"), MoveTemp(Values));
			break;
		}
		case ERigIkReadbackKind::IkRetargeterSettings:
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphRetargetSettingMutation& Setting : Spec.Settings)
			{
				const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("scope"), Setting.Scope);
				Value->SetStringField(TEXT("chain_name"), Setting.ChainName.ToString());
				Value->SetStringField(TEXT("setting_name"), Setting.SettingName);
				Value->SetStringField(TEXT("value"), Setting.Value);
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("settings"), MoveTemp(Values));
			break;
		}
		}
		return Root;
	}

	bool TryBuildRigIkActualJson(
		const FString& TargetRef,
		const FRigIkReadbackSpec& Spec,
		TSharedRef<FJsonObject>& OutRoot,
		FString& OutError)
	{
		OutError.Reset();
		OutRoot = MakeShared<FJsonObject>();
		UObject* const Target = LoadObject<UObject>(nullptr, *ObjectPath(TargetRef));
		if (!Target || Target->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			OutError = TEXT("animation_readback_target_class_mismatch");
			return false;
		}
		OutRoot->SetStringField(TEXT("target_kind"), Spec.TargetKind);
		OutRoot->SetStringField(TEXT("native_class"), Target->GetClass()->GetPathName());
		if (Spec.Kind == ERigIkReadbackKind::ControlRigCreate
			|| Spec.Kind == ERigIkReadbackKind::ControlRigDefinition)
		{
			UControlRigBlueprint* Blueprint = CastChecked<UControlRigBlueprint>(Target);
			if (Spec.Kind == ERigIkReadbackKind::ControlRigCreate)
			{
				const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(
					UControlRigBlueprint::StaticClass(), TEXT("SourceHierarchyImport"));
				const FSoftObjectPtr* Source = Property
					? Property->ContainerPtrToValuePtr<FSoftObjectPtr>(Blueprint) : nullptr;
				OutRoot->SetStringField(TEXT("skeleton_ref"), Source
					? Source->ToSoftObjectPath().ToString() : FString());
				return true;
			}
			URigHierarchy* Hierarchy = Blueprint->GetHierarchy();
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphControlMutation& Requested : Spec.Controls)
			{
				const FRigElementKey Key(Requested.Name, ERigElementType::Control);
				const FRigControlElement* Control = Hierarchy
					? Hierarchy->Find<FRigControlElement>(Key) : nullptr;
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("rig_element:control:") + Requested.Name.ToString(), Control != nullptr);
				if (Control)
				{
					const FRigElementKey Parent = Hierarchy->GetFirstParent(Key);
					Value->SetStringField(TEXT("native_key"),
						TEXT("Control:") + Requested.Name.ToString());
					Value->SetStringField(TEXT("parent_native_key"),
						Parent.IsValid() ? TEXT("Bone:") + Parent.Name.ToString() : FString());
					Value->SetStringField(TEXT("control_type_ref"),
						ControlTypeText(Control->Settings.ControlType));
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("controls"), MoveTemp(Values));
			return true;
		}
		if (Spec.Kind == ERigIkReadbackKind::ShapeLibraryCreate
			|| Spec.Kind == ERigIkReadbackKind::ShapeLibraryDefinition)
		{
			UControlRigShapeLibrary* Library = CastChecked<UControlRigShapeLibrary>(Target);
			if (Spec.Kind == ERigIkReadbackKind::ShapeLibraryCreate)
			{
				return true;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphShapeMutation& Requested : Spec.Shapes)
			{
				const int32 Index = Library->Shapes.IndexOfByPredicate(
					[&Requested](const FControlRigShapeDefinition& Shape)
					{
						return Shape.ShapeName == Requested.Name;
					});
				const bool bPresent = Index != INDEX_NONE;
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("item:shape:") + Requested.Name.ToString(), bPresent);
				if (bPresent)
				{
					Value->SetStringField(TEXT("mesh_ref"),
						Library->Shapes[Index].StaticMesh.ToSoftObjectPath().ToString());
					Value->SetStringField(TEXT("material_ref"),
						Library->DefaultMaterial.ToSoftObjectPath().ToString());
					Value->SetNumberField(TEXT("order_index"), Index);
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("shapes"), MoveTemp(Values));
			return true;
		}
		if (Spec.Kind == ERigIkReadbackKind::IkRigCreate
			|| Spec.Kind == ERigIkReadbackKind::IkRigDefinition
			|| Spec.Kind == ERigIkReadbackKind::IkRigAutoGenerate)
		{
			UIKRigDefinition* Rig = CastChecked<UIKRigDefinition>(Target);
			UIKRigController* Controller = UIKRigController::GetController(Rig);
			if (!Controller)
			{
				OutError = TEXT("animation_readback_ik_controller_unavailable");
				return false;
			}
			if (Spec.Kind == ERigIkReadbackKind::IkRigCreate
				|| Spec.Kind == ERigIkReadbackKind::IkRigAutoGenerate)
			{
				OutRoot->SetStringField(TEXT("skeletal_mesh_ref"),
					Controller->GetSkeletalMesh()
						? Controller->GetSkeletalMesh()->GetPathName() : FString());
				return true;
			}
			OutRoot->SetStringField(TEXT("retarget_root"),
				Controller->GetRetargetRoot().ToString());
			TArray<TSharedPtr<FJsonValue>> Goals;
			for (const FAnimationRigGraphIkGoalMutation& Requested : Spec.Goals)
			{
				const bool bPresent = Controller->GetGoal(Requested.Name) != nullptr;
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("item:ik_goal:") + Requested.Name.ToString(), bPresent);
				if (bPresent)
				{
					Value->SetStringField(TEXT("bone_name"),
						Controller->GetBoneForGoal(Requested.Name).ToString());
				}
				Goals.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("goals"), MoveTemp(Goals));
			TArray<TSharedPtr<FJsonValue>> Solvers;
			for (const FAnimationRigGraphIkSolverMutation& Requested : Spec.Solvers)
			{
				const FInstancedStruct* Solver = Controller->GetSolverStructAtIndex(
					Requested.OrderIndex);
				const bool bPresent = Solver && Solver->GetScriptStruct();
				const TSharedRef<FJsonObject> Value = Observation(
					FString::Printf(TEXT("solver:%d"), Requested.OrderIndex), bPresent);
				if (bPresent)
				{
					Value->SetStringField(TEXT("solver_class_ref"),
						Solver->GetScriptStruct()->GetPathName());
					Value->SetNumberField(TEXT("order_index"), Requested.OrderIndex);
				}
				Solvers.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("solvers"), MoveTemp(Solvers));
			TArray<TSharedPtr<FJsonValue>> Chains;
			for (const FAnimationRigGraphIkChainMutation& Requested : Spec.Chains)
			{
				const FBoneChain* Chain = Controller->GetRetargetChains().FindByPredicate(
					[&Requested](const FBoneChain& Existing)
					{
						return Existing.ChainName == Requested.Name;
					});
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("hierarchy:ik_chain:") + Requested.Name.ToString(), Chain != nullptr);
				if (Chain)
				{
					Value->SetStringField(TEXT("start_bone"), Chain->StartBone.BoneName.ToString());
					Value->SetStringField(TEXT("end_bone"), Chain->EndBone.BoneName.ToString());
					Value->SetStringField(TEXT("goal_name"), Chain->IKGoalName.ToString());
				}
				Chains.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("chains"), MoveTemp(Chains));
			return true;
		}

		UIKRetargeter* Retargeter = CastChecked<UIKRetargeter>(Target);
		UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
		if (!Controller)
		{
			OutError = TEXT("animation_readback_retarget_controller_unavailable");
			return false;
		}
		const UIKRigDefinition* Source = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
		const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
		if (Spec.Kind == ERigIkReadbackKind::IkRetargeterCreate
			|| Spec.Kind == ERigIkReadbackKind::IkRetargeterDefinition)
		{
			OutRoot->SetStringField(TEXT("source_ik_rig_ref"),
				Source ? Source->GetPathName() : FString());
			OutRoot->SetStringField(TEXT("target_ik_rig_ref"),
				TargetRig ? TargetRig->GetPathName() : FString());
			if (Spec.Kind == ERigIkReadbackKind::IkRetargeterCreate)
			{
				return true;
			}
			const FRetargetChainMapping* NativeMapping = Controller->GetChainMapping(NAME_None);
			FInstancedStruct* NativeOp = Controller->GetRetargetOpStructAtIndex(0);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FAnimationRigGraphRetargetMappingMutation& Requested : Spec.Mappings)
			{
				const bool bPresent = NativeMapping && NativeMapping->IsReady()
					&& NativeMapping->HasChain(Requested.TargetChain,
						ERetargetSourceOrTarget::Target)
					&& !NativeMapping->GetChainMappedTo(Requested.TargetChain,
						ERetargetSourceOrTarget::Target).IsNone();
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("binding:chain_mapping:") + Requested.TargetChain.ToString(),
					bPresent);
				if (bPresent)
				{
					Value->SetStringField(TEXT("source_chain"),
						NativeMapping->GetChainMappedTo(Requested.TargetChain,
							ERetargetSourceOrTarget::Target).ToString());
					Value->SetStringField(TEXT("operation_ref"),
						NativeOp && NativeOp->GetScriptStruct()
							? NativeOp->GetScriptStruct()->GetPathName() : FString());
					Value->SetNumberField(TEXT("order_index"), 0);
				}
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("chain_mappings"), MoveTemp(Values));
			return true;
		}

		UIKRetargetFKChainsController* OpController = Cast<UIKRetargetFKChainsController>(
			Controller->GetOpController(0));
		const FIKRetargetFKChainsOpSettings NativeSettings = OpController
			? OpController->GetSettings() : FIKRetargetFKChainsOpSettings();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FAnimationRigGraphRetargetSettingMutation& Requested : Spec.Settings)
		{
			const FRetargetFKChainSettings* Chain = NativeSettings.ChainsToRetarget.FindByPredicate(
				[&Requested](const FRetargetFKChainSettings& Existing)
				{
					return Existing.TargetChainName == Requested.ChainName;
				});
			if (!Chain)
			{
				OutError = TEXT("animation_readback_retarget_setting_missing");
				return false;
			}
			FString Actual;
			if (Requested.SettingName == TEXT("RotationMode"))
			{
				Actual = StaticEnum<EFKChainRotationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain->RotationMode));
			}
			else if (Requested.SettingName == TEXT("TranslationMode"))
			{
				Actual = StaticEnum<EFKChainTranslationMode>()->GetNameStringByValue(
					static_cast<int64>(Chain->TranslationMode));
			}
			else if (Requested.SettingName == TEXT("RotationAlpha"))
			{
				Actual = FString::SanitizeFloat(Chain->RotationAlpha);
			}
			else if (Requested.SettingName == TEXT("TranslationAlpha"))
			{
				Actual = FString::SanitizeFloat(Chain->TranslationAlpha);
			}
			else if (Requested.SettingName == TEXT("EnableFK"))
			{
				Actual = Chain->EnableFK ? TEXT("true") : TEXT("false");
			}
			else
			{
				OutError = TEXT("animation_readback_retarget_setting_unsupported");
				return false;
			}
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("scope"), Requested.Scope);
			Value->SetStringField(TEXT("chain_name"), Requested.ChainName.ToString());
			Value->SetStringField(TEXT("setting_name"), Requested.SettingName);
			Value->SetStringField(TEXT("value"), Actual);
			Values.Add(MakeShared<FJsonValueObject>(Value));
		}
		OutRoot->SetArrayField(TEXT("settings"), MoveTemp(Values));
		return true;
	}

	UObject* TryCreateRigIkAsset(
		const FPcgAnimationGraphRouteProfile& Profile,
		const FGraphCompoundAnimationOperationRequest& Request,
		const FRigIkReadbackSpec& Spec,
		UPackage& Package,
		FGraphCompoundRuntimeError& OutError)
	{
		const FName Name(*AssetName(Request.GetTargetRef()));
		const EObjectFlags Flags =
			RF_Public | RF_Standalone | RF_Transactional;
		UObject* Created = nullptr;
		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.control_rig.asset.create"))
		{
			USkeleton* const Skeleton = LoadObject<USkeleton>(nullptr, *Spec.SkeletonRef);
			if (!Skeleton)
			{
				SetError(OutError, TEXT("graph_compound_animation_skeleton_ref_invalid"),
					Spec.SkeletonRef);
				return nullptr;
			}
			UControlRigBlueprintFactory* const Factory =
				NewObject<UControlRigBlueprintFactory>();
			Factory->ParentClass = UControlRig::StaticClass();
			UControlRigBlueprint* const Blueprint = Cast<UControlRigBlueprint>(
				Factory->FactoryCreateNew(UControlRigBlueprint::StaticClass(), &Package,
					Name, Flags, nullptr, GWarn));
			URigHierarchyController* const Controller = Blueprint
				? Blueprint->GetHierarchyController() : nullptr;
			if (!Blueprint || !Controller
				|| Controller->ImportBones(Skeleton->GetReferenceSkeleton(), NAME_None,
					false, false, false, false).IsEmpty())
			{
				SetError(OutError, TEXT("graph_compound_control_rig_factory_failed"),
					Request.GetTargetRef());
				return nullptr;
			}
			const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(
				UControlRigBlueprint::StaticClass(), TEXT("SourceHierarchyImport"));
			FSoftObjectPtr* Source = Property
				? Property->ContainerPtrToValuePtr<FSoftObjectPtr>(Blueprint) : nullptr;
			if (!Source)
			{
				SetError(OutError, TEXT("graph_compound_control_rig_source_property_missing"));
				return nullptr;
			}
			*Source = FSoftObjectPtr(Skeleton);
			Blueprint->PropagateHierarchyFromBPToInstances();
			Blueprint->RecompileVM();
			Created = Blueprint;
		}
		else if (Capability
			== TEXT("animation.control_rig.shape_library.asset.create"))
		{
			Created = NewObject<UControlRigShapeLibrary>(&Package, Name, Flags);
		}
		else if (Capability == TEXT("animation.ik_rig.asset.create"))
		{
			USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
				nullptr, *Spec.SkeletalMeshRef);
			UIKRigDefinitionFactory* const Factory =
				NewObject<UIKRigDefinitionFactory>();
			UIKRigDefinition* const Rig = Mesh
				? Cast<UIKRigDefinition>(Factory->FactoryCreateNew(
					UIKRigDefinition::StaticClass(), &Package, Name, Flags, nullptr, GWarn))
				: nullptr;
			UIKRigController* const Controller = Rig
				? UIKRigController::GetController(Rig) : nullptr;
			if (!Rig || !Controller || !Controller->SetSkeletalMesh(Mesh))
			{
				SetError(OutError, TEXT("graph_compound_ik_rig_factory_failed"),
					Request.GetTargetRef());
				return nullptr;
			}
			Created = Rig;
		}
		else
		{
			UIKRigDefinition* const Source = LoadObject<UIKRigDefinition>(
				nullptr, *Spec.SourceRigRef);
			UIKRigDefinition* const Target = LoadObject<UIKRigDefinition>(
				nullptr, *Spec.TargetRigRef);
			UIKRetargetFactory* const Factory = NewObject<UIKRetargetFactory>();
			UIKRetargeter* const Retargeter = Source && Target
				? Cast<UIKRetargeter>(Factory->FactoryCreateNew(
					UIKRetargeter::StaticClass(), &Package, Name, Flags, nullptr, GWarn))
				: nullptr;
			UIKRetargeterController* const Controller = Retargeter
				? UIKRetargeterController::GetController(Retargeter) : nullptr;
			if (!Retargeter || !Controller)
			{
				SetError(OutError, TEXT("graph_compound_ik_retargeter_factory_failed"),
					Request.GetTargetRef());
				return nullptr;
			}
			Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
			Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
			Controller->CleanAsset();
			Created = Retargeter;
		}
		if (!Created || Created->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			SetError(OutError, TEXT("graph_compound_animation_factory_failed"),
				Request.GetTargetRef(), Spec.NativeClassPath);
			return nullptr;
		}
		return Created;
	}

	bool TryApplyRigIkMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		const FRigIkReadbackSpec& Spec,
		FAnimationRigGraphNativeHook& Hook,
		FString& OutRevision,
		FGraphCompoundRuntimeError& OutError)
	{
		FAnimationRigGraphNativeMutationReceipt Apply;
		FAnimationRigGraphBarrierReceipt Barrier;
		FAnimationRigGraphTopology Topology;
		FString Evidence;
		FAnimationRigGraphAdapterError NativeError;
		bool bApplied = false;
		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.control_rig.hierarchy_graph.apply"))
		{
			if (UControlRigBlueprint* const Blueprint = Cast<UControlRigBlueprint>(&Target))
			{
				bApplied = Hook.TryApplyAndCaptureControlRigDefinition(
					*Blueprint, Spec.Controls, Apply, Barrier, Topology,
					OutRevision, Evidence, NativeError);
			}
		}
		else if (Capability == TEXT("animation.control_rig.shape_library.apply"))
		{
			if (UControlRigShapeLibrary* const Library =
				Cast<UControlRigShapeLibrary>(&Target))
			{
				bApplied = Hook.TryApplyAndCaptureControlRigShapeLibrary(
					*Library, Spec.Shapes, Apply, Barrier, Topology,
					OutRevision, Evidence, NativeError);
			}
		}
		else if (Capability == TEXT("animation.ik_rig.definition.apply"))
		{
			if (UIKRigDefinition* const Rig = Cast<UIKRigDefinition>(&Target))
			{
				bApplied = Hook.TryApplyAndCaptureIKRigDefinition(
					*Rig, Spec.RetargetRoot, Spec.Goals, Spec.Solvers, Spec.Chains,
					Apply, Barrier, Topology, OutRevision, Evidence, NativeError);
			}
		}
		else if (Capability == TEXT("animation.ik_rig.definition.auto_generate"))
		{
			UIKRigDefinition* const Rig = Cast<UIKRigDefinition>(&Target);
			USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
				nullptr, *Spec.SkeletalMeshRef);
			if (Rig && Mesh)
			{
				bApplied = Hook.TryApplyAndCaptureIKRigAutoGenerate(
					*Rig, *Mesh, Spec.Preset, Apply, Barrier, Topology,
					OutRevision, Evidence, NativeError);
			}
		}
		else if (Capability == TEXT("animation.ik_retargeter.definition.apply"))
		{
			UIKRetargeter* const Retargeter = Cast<UIKRetargeter>(&Target);
			UIKRigDefinition* const Source = LoadObject<UIKRigDefinition>(
				nullptr, *Spec.SourceRigRef);
			UIKRigDefinition* const TargetRig = LoadObject<UIKRigDefinition>(
				nullptr, *Spec.TargetRigRef);
			if (Retargeter && Source && TargetRig)
			{
				bApplied = Hook.TryApplyAndCaptureIKRetargeterDefinition(
					*Retargeter, *Source, *TargetRig, Spec.Mappings,
					Apply, Barrier, Topology, OutRevision, Evidence, NativeError);
			}
		}
		else if (UIKRetargeter* const Retargeter = Cast<UIKRetargeter>(&Target))
		{
			bApplied = Hook.TryApplyAndCaptureIKRetargeterSettings(
				*Retargeter, Spec.Settings, Apply, Barrier, Topology,
				OutRevision, Evidence, NativeError);
		}
		if (!bApplied || !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
		{
			SetError(OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("graph_compound_animation_native_failed")
					: NativeError.GetCode(),
				Target.GetPathName(), NativeError.GetDetail());
			return false;
		}
		return true;
	}

	void AddRigIkDependencyAnchors(
		const FRigIkReadbackSpec& Spec,
		TArray<TStrongObjectPtr<UObject>>& OutAnchors)
	{
		auto Add = [&OutAnchors](const FString& Ref)
		{
			if (!Ref.IsEmpty())
			{
				if (UObject* const Object = LoadObject<UObject>(nullptr, *Ref))
				{
					OutAnchors.Emplace(Object);
				}
			}
		};
		Add(Spec.SkeletonRef);
		Add(Spec.SkeletalMeshRef);
		Add(Spec.SourceRigRef);
		Add(Spec.TargetRigRef);
		for (const FAnimationRigGraphShapeMutation& Shape : Spec.Shapes)
		{
			if (Shape.Mesh)
			{
				OutAnchors.Emplace(Shape.Mesh);
			}
			if (Shape.Material)
			{
				OutAnchors.Emplace(Shape.Material);
			}
		}
	}
}
