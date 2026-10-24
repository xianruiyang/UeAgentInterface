#include "Composition/GraphCompound/UeAgentGraphCompoundRigIkProduction.h"
#include "Misc/ScopeExit.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Animation/Skeleton.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigGizmoLibrary.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphStructureCapture.h"
#include "Dom/JsonValue.h"
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
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace StateSemantic;

	namespace
	{
		void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		Contract::FDiagnostic RigOperationFailure(const TCHAR* Code, const FString& Reason, const FString& Target,
			const TSharedPtr<FJsonValue>& Actual = nullptr, const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_graph"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			return Error;
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

		FString PointerToken(FString Token)
		{
			Token.ReplaceInline(TEXT("~"), TEXT("~0"));
			Token.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Token;
		}

		FString JoinInputPointer(const FString& Base, const FString& Relative)
		{
			if (Relative.IsEmpty()) return Base;
			return Base == TEXT("/") ? Relative : Base + Relative;
		}

		Contract::FDiagnostic RigInputFailure(const FGraphCompoundAnimationOperationRequest& Request,
			const FGraphCompoundCanonicalEdit& Edit, const TCHAR* Code, const FString& Reason,
			const FString& RelativePointer, const FUeAgentCanonicalValue* Actual,
			const FString& Expected)
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_compound"));
			Error.TargetPath = Request.GetTargetRef();
			Error.SourceKind = TEXT("command_input");
			Error.PropertyPath = RelativePointer;
			Error.SemanticId = Edit.GetIdentity().GetSemanticId();
			Error.ActualValuePresent = Actual != nullptr;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			if (Actual)
			{
				Contract::FDiagnostic ProjectionError;
				if (!CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
					Error.Append(ProjectionError);
			}
			if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edit.GetIdentity().GetSemanticId()))
			{
				Error.Source.JsonPath = JoinInputPointer(*Pointer, RelativePointer);
				Error.SourceKind = TEXT("related_input");
			}
			return Error;
		}

		bool RequireStringField(const FGraphCompoundAnimationOperationRequest& Request,
			const FGraphCompoundCanonicalEdit& Edit, const FUeAgentCanonicalValue* Object,
			const FString& Field, FString& OutValue, const TCHAR* Code,
			FGraphCompoundRuntimeError& OutError, const FString& ParentPointer = FString())
		{
			const bool bObject = Object && Object->GetKind() == EUeAgentValueKind::Object;
			const FUeAgentCanonicalValue* Value = bObject ? FindField(*Object, Field) : Object;
			if (!bObject || !Value || Value->GetKind() != EUeAgentValueKind::String)
			{
				OutError = RigInputFailure(Request, Edit, Code,
					!bObject ? TEXT("The proposed edit value must be an object.")
						: FString::Printf(TEXT("Field '%s' %s."), *Field,
							Value ? TEXT("must be a string") : TEXT("is missing")),
					bObject ? ParentPointer + TEXT("/") + PointerToken(Field) : ParentPointer,
					Value, bObject ? TEXT("string") : TEXT("object"));
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
			if (Value->GetKind() != EUeAgentValueKind::Integer && Value->GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			int64 Integer = 0;
			if (!StateSemantic::TryParseInt64Number(Value->GetLexeme(), Integer)
				|| Integer < MIN_int32 || Integer > MAX_int32)
			{
				return false;
			}
			OutValue = static_cast<int32>(Integer);
			return true;
		}

		bool TryNumberField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			double& OutValue)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			if (!Value
				|| (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal)
				|| !StateSemantic::TryParseFiniteDecimal(Value->GetLexeme(), OutValue))
			{
				return false;
			}
			return true;
		}

		bool RequirePositionField(
			const FGraphCompoundAnimationOperationRequest& Request,
			const FGraphCompoundCanonicalEdit& Edit,
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FVector2D& OutPosition,
			FGraphCompoundRuntimeError& OutError)
		{
			const FUeAgentCanonicalValue* Value = FindField(Object, Name);
			double X = 0.0;
			double Y = 0.0;
			const FString Base = TEXT("/") + PointerToken(Name);
			if (!Value || Value->GetKind() != EUeAgentValueKind::Object)
			{
				OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_control_rig_unit_edit_invalid"),
					TEXT("The RigVM node position must be an object with finite x and y coordinates."), Base, Value, TEXT("object"));
				return false;
			}
			for (const auto& Axis : {TPair<const TCHAR*, double*>(TEXT("x"), &X), TPair<const TCHAR*, double*>(TEXT("y"), &Y)})
			{
				if (TryNumberField(*Value, Axis.Key, *Axis.Value)) continue;
				OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_control_rig_unit_edit_invalid"),
					FString::Printf(TEXT("The RigVM node position '%s' coordinate must be a finite number."), Axis.Key),
					Base + TEXT("/") + Axis.Key, FindField(*Value, Axis.Key), TEXT("finite number"));
				return false;
			}
			OutPosition = FVector2D(X, Y);
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

		TSharedRef<FJsonObject> PositionJson(const FVector2D& Position)
		{
			const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("x"), Position.X);
			Value->SetNumberField(TEXT("y"), Position.Y);
			return Value;
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
		TArray<FAnimationRigGraphStructureValue> CapturedValues;
		FAnimationRigGraphAdapterError CaptureError;
		if (!TryCaptureAnimationRigGraphStructure(
				Target, CapturedValues, CaptureError))
		{
			OutError = CaptureError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Target.GetPathName();
			return false;
		}

		const FString& Capability = Profile.GetCapabilityId();
		if (Capability == TEXT("animation.ik_rig.definition.auto_generate"))
		{
			const FAnimationRigGraphStructureValue* Mesh =
				CapturedValues.FindByPredicate(
					[](const FAnimationRigGraphStructureValue& Value)
					{
						return Value.SemanticId
							== TEXT("binding:skeletal_mesh");
					});
			if (!Mesh)
			{
				OutError = RigOperationFailure(TEXT("animation_rig_graph_structure_mesh_binding_missing"),
					TEXT("The captured IK Rig structure did not provide the skeletal mesh binding required for automatic generation."),
					Target.GetPathName(), nullptr, MakeShared<FJsonValueString>(TEXT("skeletal mesh binding")));
				OutError.ActualValuePresent = false;
				return false;
			}
			const TSharedRef<FJsonObject> Value =
				MakeShared<FJsonObject>(*Mesh->Value);
			// The preset is an operation input, not persisted UE state. Leaving the
			// observed value empty prevents read-before-plan from inventing a preset
			// and incorrectly collapsing an explicit auto-generate request to no-op.
			Value->SetStringField(TEXT("preset"), FString());
			OutValues.Add({
				TEXT("property:definition:auto_generate"),
				Value,
			});
			return true;
		}

		auto IsApplicable =
			[&Capability](const FString& SemanticId)
			{
				if (Capability
					== TEXT("animation.control_rig.hierarchy_graph.apply"))
				{
					return SemanticId.StartsWith(TEXT("rig_element:control:"))
						|| SemanticId.StartsWith(TEXT("rigvm_node:"));
				}
				if (Capability
					== TEXT("animation.control_rig.shape_library.apply"))
				{
					return SemanticId.StartsWith(TEXT("item:shape:"));
				}
				if (Capability == TEXT("animation.ik_rig.definition.apply"))
				{
					return SemanticId == TEXT("property:retarget_root")
						|| SemanticId.StartsWith(TEXT("item:ik_goal:"))
						|| SemanticId.StartsWith(TEXT("order:ik_solver:"))
						|| SemanticId.StartsWith(TEXT("hierarchy:ik_chain:"));
				}
				if (Capability
					== TEXT("animation.ik_retargeter.definition.apply"))
				{
					return SemanticId == TEXT("binding:source_ik_rig")
						|| SemanticId == TEXT("binding:target_ik_rig")
						|| SemanticId.StartsWith(
							TEXT("binding:chain_mapping:"));
				}
				return Capability
						== TEXT("animation.ik_retargeter.settings.apply")
					&& SemanticId.StartsWith(
						TEXT("property:setting:"));
			};
		for (const FAnimationRigGraphStructureValue& Captured : CapturedValues)
		{
			if (IsApplicable(Captured.SemanticId))
			{
				OutValues.Add({Captured.SemanticId, Captured.Value});
			}
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
			OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_animation_plan_invalid"),
				TEXT("The animation operation has no edits to apply or verify."), TEXT("graph_compound"));
			OutError.TargetPath = Request.GetTargetRef();
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one edit"));
			return false;
		}
		if (IsRigIkCreateProfile(Profile))
		{
			const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
			if (!Value.IsValid())
			{
				OutError = RigInputFailure(Request, Edits[0], TEXT("graph_compound_animation_create_value_missing"),
					TEXT("The asset creation edit has no proposed value."), FString(), nullptr, TEXT("asset creation object"));
				return false;
			}
			if (Capability == TEXT("animation.control_rig.asset.create"))
			{
				OutSpec.Kind = ERigIkReadbackKind::ControlRigCreate;
				OutSpec.NativeClassPath = UControlRigBlueprint::StaticClass()->GetPathName();
				if (!RequireStringField(Request, Edits[0], Value.Get(), TEXT("skeleton_ref"),
					OutSpec.SkeletonRef, TEXT("graph_compound_animation_create_reference_missing"), OutError)) return false;
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
				if (!RequireStringField(Request, Edits[0], Value.Get(), TEXT("skeletal_mesh_ref"),
					OutSpec.SkeletalMeshRef, TEXT("graph_compound_animation_create_reference_missing"), OutError)) return false;
			}
			else
			{
				OutSpec.Kind = ERigIkReadbackKind::IkRetargeterCreate;
				OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
				if (!RequireStringField(Request, Edits[0], Value.Get(), TEXT("source_ik_rig_ref"), OutSpec.SourceRigRef,
						TEXT("graph_compound_animation_create_reference_missing"), OutError)
					|| !RequireStringField(Request, Edits[0], Value.Get(), TEXT("target_ik_rig_ref"), OutSpec.TargetRigRef,
						TEXT("graph_compound_animation_create_reference_missing"), OutError))
				{
					return false;
				}
			}
			// Resolve before expected readback, dependency anchors and native loading
			// consume these references. Search aliases are owned by ContentDiscovery.
			const TPair<const TCHAR*, FString*> References[] = {
				{TEXT("skeleton_ref"), &OutSpec.SkeletonRef}, {TEXT("skeletal_mesh_ref"), &OutSpec.SkeletalMeshRef},
				{TEXT("source_ik_rig_ref"), &OutSpec.SourceRigRef}, {TEXT("target_ik_rig_ref"), &OutSpec.TargetRigRef}};
			for (const auto& Reference : References)
			{
				FString* Ref = Reference.Value;
				if (Ref->IsEmpty() || Ref->StartsWith(TEXT("/"))) continue;
				ON_SCOPE_EXIT
				{
					if (OutError.IsValid() && OutError.Source.SourceFile.IsEmpty()
						&& OutError.Source.JsonPath.IsEmpty() && OutError.SemanticPointer.IsEmpty())
					{
						if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
						{
							OutError.Source.JsonPath = JoinInputPointer(*Pointer, TEXT("/") + FString(Reference.Key));
							OutError.SourceKind = TEXT("related_input");
						}
					}
				};
				DomainCapability::FDomainCapabilityAuthorities Authorities;
				UE::UAI::Contract::FDiagnostic AuthorityError;
				if (!DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(Authorities, AuthorityError)
					|| !Authorities.GetContentDiscoveryService())
				{
					OutError = AuthorityError.IsEmpty()
						? RigInputFailure(Request, Edits[0], TEXT("content_search_authority_unavailable"),
							TEXT("The content discovery service is unavailable, so this asset reference cannot be resolved."),
							TEXT("/") + FString(Reference.Key), FindField(*Value, Reference.Key), TEXT("resolvable asset reference"))
						: AuthorityError;
					return false;
				}
				ContentDiscovery::FContentDiscoveryItem Item;
				ContentDiscovery::FContentDiscoveryError Error;
				if (!Authorities.GetContentDiscoveryService()->TryResolveReadableName(*Ref, Item, Error))
				{
					SetError(OutError, Error, Error.SubjectId);
					return false;
				}
				if (Item.ContentKind != TEXT("asset"))
				{
					OutError = RigInputFailure(Request, Edits[0], TEXT("graph_compound_animation_asset_reference_required"),
						FString::Printf(TEXT("Reference '%s' resolves to content kind '%s'; an asset is required."), **Ref, *Item.ContentKind),
						TEXT("/") + FString(Reference.Key), FindField(*Value, Reference.Key), TEXT("asset reference"));
					return false;
				}
				*Ref = Item.Identity;
			}
			return true;
		}

		if (Capability == TEXT("animation.control_rig.hierarchy_graph.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::ControlRigDefinition;
			OutSpec.NativeClassPath = UControlRigBlueprint::StaticClass()->GetPathName();
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (SemanticId.StartsWith(TEXT("rig_element:control:")))
				{
					FAnimationRigGraphControlMutation Mutation;
					Mutation.Name = *SemanticId.Mid(20);
					Mutation.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId))
						Mutation.InputPointer = *Pointer;
					if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
					{
						FString ParentKey;
						if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(),
							TEXT("parent_native_key"), ParentKey, TEXT("graph_compound_control_rig_edit_invalid"), OutError)) return false;
						if (!ParentKey.StartsWith(TEXT("Bone:")))
						{
							OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_control_rig_edit_invalid"),
								TEXT("A control parent must identify a bone using the Bone: prefix."), TEXT("/parent_native_key"),
								FindField(*Edit.GetProposedAfter(), TEXT("parent_native_key")), TEXT("Bone:<bone name>"));
							return false;
						}
						if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(),
							TEXT("control_type_ref"), Mutation.ControlTypeRef, TEXT("graph_compound_control_rig_edit_invalid"), OutError)) return false;
						Mutation.ParentBone = *ParentKey.Mid(5);
					}
					OutSpec.Controls.Add(MoveTemp(Mutation));
					continue;
				}
				if (SemanticId.StartsWith(TEXT("rigvm_node:")))
				{
					FAnimationRigGraphUnitMutation Mutation;
					Mutation.NodeName = *SemanticId.Mid(11);
					Mutation.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId))
						Mutation.InputPointer = *Pointer;
					if (Mutation.Intent == EAnimationRigGraphMutationIntent::Create
						|| Mutation.Intent
							== EAnimationRigGraphMutationIntent::Replace)
					{
						const FUeAgentCanonicalValue* Candidate =
							Edit.GetProposedAfter().IsValid()
								? FindField(
									*Edit.GetProposedAfter(), TEXT("candidate"))
								: nullptr;
						FString Method;
						if (!Edit.GetProposedAfter().IsValid()
							|| !Candidate
							|| Candidate->GetKind() != EUeAgentValueKind::Object)
						{
							OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_control_rig_unit_edit_invalid"),
								TEXT("The RigVM node edit requires a candidate object."), TEXT("/candidate"), Candidate, TEXT("object"));
							return false;
						}
						if (!RequirePositionField(Request, Edit, *Edit.GetProposedAfter(), TEXT("position"), Mutation.Position, OutError)
							|| !RequireStringField(Request, Edit, Candidate, TEXT("name"), Mutation.CandidateName,
								TEXT("graph_compound_control_rig_unit_edit_invalid"), OutError, TEXT("/candidate"))) return false;
						if (const FUeAgentCanonicalValue* Selection = FindField(*Candidate, TEXT("select")))
						{
							if (Selection->GetKind() != EUeAgentValueKind::Object)
							{
								OutError = RigInputFailure(Request, Edit, TEXT("candidate_selector_invalid"),
									TEXT("Candidate select must be an object of semantic string fields."),
									TEXT("/candidate/select"), Selection, TEXT("object"));
								OutError.Stage = TEXT("candidate_resolve");
								return false;
							}
							for (const auto& Field : Selection->GetObjectItems())
							{
								if (Field.Value->GetKind() != EUeAgentValueKind::String)
								{
									OutError = RigInputFailure(Request, Edit, TEXT("candidate_selector_invalid"),
										FString::Printf(TEXT("Candidate select field '%s' must be a string."), *Field.Key),
										TEXT("/candidate/select/") + PointerToken(Field.Key), &Field.Value.Get(), TEXT("string"));
									OutError.Stage = TEXT("candidate_resolve");
									return false;
								}
								Mutation.CandidateSelect.Add(Field.Key, Field.Value->GetText());
							}
						}
						Mutation.bHasScriptStructAssertion = TryStringField(
							*Edit.GetProposedAfter(),
							TEXT("script_struct_ref"),
							Mutation.ScriptStructRef);
						if (TryStringField(
								*Edit.GetProposedAfter(), TEXT("method"), Method))
						{
							Mutation.MethodName = *Method;
							Mutation.bHasMethodAssertion = true;
							Mutation.bHasMethodInput = true;
						}
					}
					else if (Mutation.Intent
						== EAnimationRigGraphMutationIntent::Update)
					{
						FString Method;
						if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("script_struct_ref"),
							Mutation.ScriptStructRef, TEXT("graph_compound_control_rig_unit_edit_invalid"), OutError)
							|| !RequirePositionField(Request, Edit, *Edit.GetProposedAfter(), TEXT("position"), Mutation.Position, OutError)) return false;
						Mutation.bHasScriptStructAssertion = true;
						if (TryStringField(
								*Edit.GetProposedAfter(), TEXT("method"), Method))
						{
							Mutation.MethodName = *Method;
							Mutation.bHasMethodInput = true;
						}
						Mutation.bHasMethodAssertion = true;
					}
					OutSpec.Units.Add(MoveTemp(Mutation));
					continue;
				}
				OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_control_rig_identity_unsupported"),
					TEXT("This edit does not identify a Control Rig control or RigVM node."), FString(), nullptr,
					TEXT("control or RigVM node edit"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(SemanticId);
				OutError.ActualValuePresent.Reset();
				return false;
			}
			if (OutSpec.Controls.IsEmpty() && OutSpec.Units.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_control_rig_edits_required"),
					TEXT("At least one control or RigVM node edit is required."), TEXT("graph_compound"));
				OutError.TargetPath = Request.GetTargetRef();
				return false;
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
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				FAnimationRigGraphShapeMutation Mutation;
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (!SemanticId.StartsWith(Prefix))
				{
					OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_shape_identity_invalid"),
						TEXT("The edit identity does not identify a shape in the shape library."), FString(), nullptr,
						TEXT("shape item identity"));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(SemanticId);
					OutError.ActualValuePresent.Reset();
					return false;
				}
				Mutation.Name = *SemanticId.Mid(Prefix.Len());
				Mutation.Intent = NativeIntent(Edit.GetIntent());
				if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId))
					Mutation.InputPointer = *Pointer;
				if (Mutation.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					FString MeshRef;
					FString MaterialRef;
					if (!Edit.GetProposedAfter().IsValid())
					{
						OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_shape_value_missing"),
							FString::Printf(TEXT("Shape '%s' requires a proposed value."), *Mutation.Name.ToString()),
							FString(), nullptr, TEXT("object"));
						return false;
					}
					if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("mesh_ref"), MeshRef,
						TEXT("graph_compound_shape_mesh_field_invalid"), OutError))
					{
						return false;
					}
					if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("material_ref"), MaterialRef,
						TEXT("graph_compound_shape_material_field_invalid"), OutError))
					{
						return false;
					}
					if (!TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"),
						Mutation.OrderIndex))
					{
						OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_shape_order_field_invalid"),
							TEXT("Shape field 'order_index' must be an integer within the signed 32-bit range."),
							TEXT("/order_index"), FindField(*Edit.GetProposedAfter(), TEXT("order_index")), TEXT("signed 32-bit integer"));
						return false;
					}
					Mutation.Mesh = LoadObject<UStaticMesh>(nullptr, *MeshRef);
					Mutation.Material = LoadObject<UMaterial>(nullptr, *MaterialRef);
					if (!Mutation.Mesh || !Mutation.Material)
					{
						const TCHAR* Field = !Mutation.Mesh ? TEXT("mesh_ref") : TEXT("material_ref");
						OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_shape_reference_invalid"),
							FString::Printf(TEXT("Shape '%s' field '%s' could not be loaded as the required asset class."),
								*Mutation.Name.ToString(), Field), TEXT("/") + FString(Field), FindField(*Edit.GetProposedAfter(), Field),
							!Mutation.Mesh ? UStaticMesh::StaticClass()->GetPathName() : UMaterial::StaticClass()->GetPathName());
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
			const FGraphCompoundCanonicalEdit* RootEdit = nullptr;
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (SemanticId == TEXT("property:retarget_root"))
				{
					RootEdit = &Edit;
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId))
						OutSpec.RetargetRootInputPointer = *Pointer;
					if (!Edit.GetProposedAfter().IsValid()
						|| Edit.GetProposedAfter()->GetKind() != EUeAgentValueKind::String)
					{
						OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_ik_root_invalid"),
							TEXT("The retarget root edit value must be a bone-name string."), FString(), Edit.GetProposedAfter().Get(), TEXT("string"));
						return false;
					}
					OutSpec.RetargetRoot = *Edit.GetProposedAfter()->GetText();
				}
				else if (SemanticId.StartsWith(TEXT("item:ik_goal:")))
				{
					FAnimationRigGraphIkGoalMutation Goal;
					Goal.Name = *SemanticId.Mid(13);
					Goal.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Goal.InputPointer = *Pointer;
					FString Bone;
					if (Goal.Intent != EAnimationRigGraphMutationIntent::Delete
						&& !RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("bone_name"), Bone,
							TEXT("graph_compound_ik_goal_invalid"), OutError))
					{
						return false;
					}
					Goal.BoneName = *Bone;
					OutSpec.Goals.Add(MoveTemp(Goal));
				}
				else if (SemanticId.StartsWith(TEXT("order:ik_solver:")))
				{
					FAnimationRigGraphIkSolverMutation Solver;
					Solver.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Solver.InputPointer = *Pointer;
					if (Solver.Intent != EAnimationRigGraphMutationIntent::Delete)
					{
						if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("solver_class_ref"),
							Solver.SolverClassRef, TEXT("graph_compound_ik_solver_invalid"), OutError)) return false;
						if (!TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"), Solver.OrderIndex))
						{
							OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_ik_solver_invalid"),
								TEXT("IK solver field 'order_index' must be an integer within the signed 32-bit range."),
								TEXT("/order_index"), FindField(*Edit.GetProposedAfter(), TEXT("order_index")), TEXT("signed 32-bit integer"));
							return false;
						}
					}
					OutSpec.Solvers.Add(MoveTemp(Solver));
				}
				else if (SemanticId.StartsWith(TEXT("hierarchy:ik_chain:")))
				{
					FAnimationRigGraphIkChainMutation Chain;
					Chain.Name = *SemanticId.Mid(19);
					Chain.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Chain.InputPointer = *Pointer;
					FString Start;
					FString End;
					FString Goal;
					if (Chain.Intent != EAnimationRigGraphMutationIntent::Delete
						&& (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("start_bone"), Start, TEXT("graph_compound_ik_chain_invalid"), OutError)
							|| !RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("end_bone"), End, TEXT("graph_compound_ik_chain_invalid"), OutError)
							|| !RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("goal_name"), Goal, TEXT("graph_compound_ik_chain_invalid"), OutError)))
					{
						return false;
					}
					Chain.StartBone = *Start;
					Chain.EndBone = *End;
					Chain.GoalName = *Goal;
					OutSpec.Chains.Add(MoveTemp(Chain));
				}
			}
			if (OutSpec.RetargetRoot.IsNone())
			{
				if (RootEdit)
				{
					OutError = RigInputFailure(Request, *RootEdit, TEXT("graph_compound_ik_root_invalid"),
						TEXT("An IK Rig definition requires a nonempty retarget root bone."), FString(), RootEdit->GetProposedAfter().Get(),
						TEXT("nonempty retarget root bone name"));
					return false;
				}
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_ik_root_invalid"),
					TEXT("An IK Rig definition requires a nonempty retarget root bone."), TEXT("graph_compound"));
				OutError.TargetPath = Request.GetTargetRef();
				OutError.SourceKind = TEXT("command_input");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty retarget root bone name"));
				OutError.ActualValuePresent = false;
				return false;
			}
			return true;
		}
		if (Capability == TEXT("animation.ik_rig.definition.auto_generate"))
		{
			OutSpec.Kind = ERigIkReadbackKind::IkRigAutoGenerate;
			OutSpec.NativeClassPath = UIKRigDefinition::StaticClass()->GetPathName();
			const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
			if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
				OutSpec.AutoGenerateInputPointer = *Pointer;
			if (!RequireStringField(Request, Edits[0], Value.Get(), TEXT("skeletal_mesh_ref"), OutSpec.SkeletalMeshRef,
					TEXT("graph_compound_ik_auto_generate_invalid"), OutError)
				|| !RequireStringField(Request, Edits[0], Value.Get(), TEXT("preset"), OutSpec.Preset,
					TEXT("graph_compound_ik_auto_generate_invalid"), OutError))
			{
				return false;
			}
			return true;
		}
		if (Capability == TEXT("animation.ik_retargeter.definition.apply"))
		{
			OutSpec.Kind = ERigIkReadbackKind::IkRetargeterDefinition;
			OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
			const FGraphCompoundCanonicalEdit* SourceRigEdit = nullptr;
			const FGraphCompoundCanonicalEdit* TargetRigEdit = nullptr;
			for (const FGraphCompoundCanonicalEdit& Edit : Edits)
			{
				ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
				const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
				if (SemanticId == TEXT("binding:source_ik_rig")
					|| SemanticId == TEXT("binding:target_ik_rig"))
				{
					if (!Edit.GetProposedAfter().IsValid()
						|| Edit.GetProposedAfter()->GetKind() != EUeAgentValueKind::String)
					{
						OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_retarget_rig_ref_invalid"),
							TEXT("The retargeter rig binding edit value must be an IK Rig reference string."),
							FString(), Edit.GetProposedAfter().Get(), TEXT("IK Rig reference string"));
						return false;
					}
					FString& Ref = SemanticId.Contains(TEXT("source"))
						? OutSpec.SourceRigRef : OutSpec.TargetRigRef;
					Ref = Edit.GetProposedAfter()->GetText();
					const bool bSource = SemanticId == TEXT("binding:source_ik_rig");
					(bSource ? SourceRigEdit : TargetRigEdit) = &Edit;
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId))
						(bSource ? OutSpec.SourceRigInputPointer : OutSpec.TargetRigInputPointer) = *Pointer;
				}
				else if (SemanticId.StartsWith(TEXT("binding:chain_mapping:")))
				{
					FAnimationRigGraphRetargetMappingMutation Mapping;
					Mapping.TargetChain = *SemanticId.Mid(22);
					Mapping.Intent = NativeIntent(Edit.GetIntent());
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Mapping.InputPointer = *Pointer;
					FString Source;
					if (Mapping.Intent != EAnimationRigGraphMutationIntent::Delete)
					{
						if (!RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("source_chain"), Source,
								TEXT("graph_compound_retarget_mapping_invalid"), OutError)
							|| !RequireStringField(Request, Edit, Edit.GetProposedAfter().Get(), TEXT("operation_ref"), Mapping.OperationRef,
								TEXT("graph_compound_retarget_mapping_invalid"), OutError)) return false;
						if (!TryIntegerField(*Edit.GetProposedAfter(), TEXT("order_index"), Mapping.OrderIndex))
						{
							OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_retarget_mapping_invalid"),
								TEXT("Retarget mapping field 'order_index' must be an integer within the signed 32-bit range."),
								TEXT("/order_index"), FindField(*Edit.GetProposedAfter(), TEXT("order_index")), TEXT("signed 32-bit integer"));
							return false;
						}
					}
					Mapping.SourceChain = *Source;
					OutSpec.Mappings.Add(MoveTemp(Mapping));
				}
			}
			if (OutSpec.SourceRigRef.IsEmpty() || OutSpec.TargetRigRef.IsEmpty())
			{
				const bool bSource = OutSpec.SourceRigRef.IsEmpty();
				const FGraphCompoundCanonicalEdit* MissingEdit = bSource ? SourceRigEdit : TargetRigEdit;
				const FString Reason = bSource ? TEXT("The retargeter definition requires a nonempty source IK Rig binding.")
					: TEXT("The retargeter definition requires a nonempty target IK Rig binding.");
				OutError = MissingEdit ? RigInputFailure(Request, *MissingEdit, TEXT("graph_compound_retarget_rig_ref_required"), Reason,
					FString(), MissingEdit->GetProposedAfter().Get(), TEXT("nonempty IK Rig reference"))
					: Contract::FDiagnostic::Failure(TEXT("graph_compound_retarget_rig_ref_required"), Reason, TEXT("graph_compound"));
				OutError.TargetPath = Request.GetTargetRef();
				if (!MissingEdit)
				{
					OutError.SourceKind = TEXT("command_input");
					OutError.ActualValuePresent = false;
				}
				return false;
			}
			if (OutSpec.Mappings.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_retarget_mapping_required"),
					TEXT("The retargeter definition requires at least one chain mapping edit."), TEXT("graph_compound"));
				OutError.TargetPath = Request.GetTargetRef();
				OutError.SourceKind = TEXT("command_input");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
				return false;
			}
			return true;
		}

		OutSpec.Kind = ERigIkReadbackKind::IkRetargeterSettings;
		OutSpec.NativeClassPath = UIKRetargeter::StaticClass()->GetPathName();
		const FString Prefix = TEXT("property:setting:");
		for (const FGraphCompoundCanonicalEdit& Edit : Edits)
		{
			ON_SCOPE_EXIT { Request.GetPayload().LocateDiagnostic(OutError, Edit.GetIdentity().GetSemanticId()); };
			const FString& SemanticId = Edit.GetIdentity().GetSemanticId();
			TArray<FString> Parts;
			SemanticId.Mid(Prefix.Len()).ParseIntoArray(Parts, TEXT(":"), false);
			FAnimationRigGraphRetargetSettingMutation Setting;
			if (!SemanticId.StartsWith(Prefix) || Parts.Num() != 3
				|| !Edit.GetProposedAfter().IsValid()
				|| !TryScalarText(*Edit.GetProposedAfter(), Setting.Value))
			{
				const bool bIdentityInvalid = !SemanticId.StartsWith(Prefix) || Parts.Num() != 3;
				OutError = RigInputFailure(Request, Edit, TEXT("graph_compound_retarget_setting_invalid"),
					bIdentityInvalid ? TEXT("The retarget setting edit identity must identify a scope, chain and setting.")
						: TEXT("The retarget setting edit value must be a string, number or boolean."), FString(),
					bIdentityInvalid ? nullptr : Edit.GetProposedAfter().Get(), bIdentityInvalid ? TEXT("retarget setting identity") : TEXT("string, number or boolean"));
				if (bIdentityInvalid)
				{
					OutError.ActualEvidence = MakeShared<FJsonValueString>(SemanticId);
					OutError.ActualValuePresent.Reset();
				}
				return false;
			}
			Setting.Scope = Parts[0];
			Setting.ChainName = *Parts[1];
			Setting.SettingName = Parts[2];
			if (const FString* Pointer = Request.GetPayload().FindInputPointer(SemanticId)) Setting.InputPointer = *Pointer;
			OutSpec.Settings.Add(MoveTemp(Setting));
		}
		if (OutSpec.Settings.IsEmpty())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("graph_compound_retarget_setting_required"),
				TEXT("At least one retarget setting edit is required."), TEXT("graph_compound"));
			OutError.TargetPath = Request.GetTargetRef();
			return false;
		}
		return true;
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
			TArray<TSharedPtr<FJsonValue>> Units;
			for (const FAnimationRigGraphUnitMutation& Unit : Spec.Units)
			{
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("rigvm_node:") + Unit.NodeName.ToString(),
					Unit.Intent != EAnimationRigGraphMutationIntent::Delete);
				if (Unit.Intent != EAnimationRigGraphMutationIntent::Delete)
				{
					Value->SetStringField(
						TEXT("script_struct_ref"), Unit.ScriptStructRef);
					Value->SetStringField(
						TEXT("method"), Unit.MethodName.ToString());
					Value->SetObjectField(
						TEXT("position"), PositionJson(Unit.Position));
				}
				Units.Add(MakeShared<FJsonValueObject>(Value));
			}
			Root->SetArrayField(TEXT("units"), MoveTemp(Units));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		OutRoot = MakeShared<FJsonObject>();
		UObject* const Target = LoadObject<UObject>(nullptr, *ObjectPath(TargetRef));
		if (!Target || Target->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			OutError = RigOperationFailure(TEXT("animation_readback_target_class_mismatch"),
				!Target ? TEXT("The animation readback target could not be loaded.")
					: TEXT("The animation readback target has a different native class than expected."), TargetRef,
				nullptr,
				MakeShared<FJsonValueString>(Spec.NativeClassPath));
			if (Target) OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetClass()->GetPathName());
			OutError.ActualValuePresent = Target != nullptr;
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
			const URigVMGraph* const Graph = Blueprint->GetDefaultModel();
			TArray<TSharedPtr<FJsonValue>> Units;
			for (const FAnimationRigGraphUnitMutation& Requested : Spec.Units)
			{
				const URigVMUnitNode* Unit = nullptr;
				if (Graph)
				{
					for (const URigVMNode* Node : Graph->GetNodes())
					{
						if (Node && Node->GetFName() == Requested.NodeName)
						{
							Unit = Cast<URigVMUnitNode>(Node);
							break;
						}
					}
				}
				const TSharedRef<FJsonObject> Value = Observation(
					TEXT("rigvm_node:") + Requested.NodeName.ToString(),
					Unit != nullptr);
				if (Unit && Unit->GetScriptStruct())
				{
					Value->SetStringField(
						TEXT("script_struct_ref"),
						Unit->GetScriptStruct()->GetPathName());
					Value->SetStringField(
						TEXT("method"), Unit->GetMethodName().ToString());
					Value->SetObjectField(
						TEXT("position"), PositionJson(Unit->GetPosition()));
				}
				Units.Add(MakeShared<FJsonValueObject>(Value));
			}
			OutRoot->SetArrayField(TEXT("units"), MoveTemp(Units));
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
				OutError = RigOperationFailure(TEXT("animation_readback_ik_controller_unavailable"),
					TEXT("The IK Rig controller is unavailable for readback."), TargetRef);
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
			OutError = RigOperationFailure(TEXT("animation_readback_retarget_controller_unavailable"),
				TEXT("The IK Retargeter controller is unavailable for readback."), TargetRef);
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
				OutError = RigOperationFailure(TEXT("animation_readback_retarget_setting_missing"),
					!OpController ? TEXT("The first retarget operation has no FK chains controller for settings readback.")
						: FString::Printf(TEXT("Retarget chain '%s' has no settings during readback of '%s'."),
							*Requested.ChainName.ToString(), *Requested.SettingName), TargetRef);
				OutError.ActualValuePresent = false;
				if (OpController && !Requested.InputPointer.IsEmpty())
				{
					OutError.Source.JsonPath = Requested.InputPointer;
					OutError.SourceKind = TEXT("related_input");
				}
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
				OutError = RigOperationFailure(TEXT("animation_readback_retarget_setting_unsupported"),
					FString::Printf(TEXT("The requested setting is unsupported during readback for retarget chain '%s'."), *Requested.ChainName.ToString()),
					TargetRef, MakeShared<FJsonValueString>(Requested.SettingName),
					MakeShared<FJsonValueString>(TEXT("RotationMode, TranslationMode, RotationAlpha, TranslationAlpha or EnableFK")));
				if (!Requested.InputPointer.IsEmpty())
				{
					OutError.Source.JsonPath = Requested.InputPointer;
					OutError.SourceKind = TEXT("related_input");
				}
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
				OutError = RigOperationFailure(TEXT("graph_compound_animation_skeleton_ref_invalid"),
					TEXT("The Control Rig creation skeleton could not be loaded as a Skeleton asset."), Request.GetTargetRef(),
					MakeShared<FJsonValueString>(Spec.SkeletonRef), MakeShared<FJsonValueString>(USkeleton::StaticClass()->GetPathName()));
				OutError.PropertyPath = TEXT("skeleton_ref");
				const auto& Edits = Request.GetPayload().GetEdits();
				if (!Edits.IsEmpty())
				{
					if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
					{
						OutError.Source.JsonPath = JoinInputPointer(*Pointer, TEXT("/skeleton_ref"));
						OutError.SourceKind = TEXT("related_input");
					}
				}
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
				OutError = RigOperationFailure(TEXT("graph_compound_control_rig_factory_failed"),
					!Blueprint ? TEXT("The Control Rig factory did not create a ControlRigBlueprint.")
						: !Controller ? TEXT("The created Control Rig has no hierarchy controller.")
						: TEXT("The hierarchy controller imported no bones from the selected skeleton."), Request.GetTargetRef());
				if (Blueprint && Controller)
				{
					OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one imported bone"));
				}
				return nullptr;
			}
			const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(
				UControlRigBlueprint::StaticClass(), TEXT("SourceHierarchyImport"));
			FSoftObjectPtr* Source = Property
				? Property->ContainerPtrToValuePtr<FSoftObjectPtr>(Blueprint) : nullptr;
			if (!Source)
			{
				OutError = RigOperationFailure(TEXT("graph_compound_control_rig_source_property_missing"),
					!Property ? TEXT("ControlRigBlueprint has no SourceHierarchyImport soft-object property.")
						: TEXT("The created Control Rig SourceHierarchyImport property could not be accessed."), Request.GetTargetRef());
				OutError.PropertyPath = TEXT("SourceHierarchyImport");
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
				OutError = RigOperationFailure(TEXT("graph_compound_ik_rig_factory_failed"),
					!Mesh ? TEXT("The IK Rig creation skeletal mesh could not be loaded.")
						: !Rig ? TEXT("The IK Rig factory did not create an IK Rig asset.")
						: !Controller ? TEXT("The newly created IK Rig has no controller.")
						: TEXT("The IK Rig controller rejected the creation skeletal mesh."), Request.GetTargetRef());
				if (!Mesh || (Rig && Controller))
				{
					OutError.ActualValue = MakeShared<FJsonValueString>(Spec.SkeletalMeshRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName());
					OutError.PropertyPath = TEXT("/skeletal_mesh_ref");
					const auto& Edits = Request.GetPayload().GetEdits();
					if (!Edits.IsEmpty())
					{
						if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
						{
							OutError.Source.JsonPath = JoinInputPointer(*Pointer, TEXT("/skeletal_mesh_ref"));
							OutError.SourceKind = TEXT("related_input");
						}
					}
				}
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
				OutError = RigOperationFailure(TEXT("graph_compound_ik_retargeter_factory_failed"),
					!Source ? TEXT("The source IK Rig for retargeter creation could not be loaded.")
						: !Target ? TEXT("The target IK Rig for retargeter creation could not be loaded.")
						: !Retargeter ? TEXT("The IK Retargeter factory did not create an asset.")
						: TEXT("The newly created IK Retargeter has no controller."), Request.GetTargetRef());
				if (!Source || !Target)
				{
					const FString Field = !Source ? TEXT("/source_ik_rig_ref") : TEXT("/target_ik_rig_ref");
					OutError.ActualValue = MakeShared<FJsonValueString>(!Source ? Spec.SourceRigRef : Spec.TargetRigRef);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UIKRigDefinition::StaticClass()->GetPathName());
					OutError.PropertyPath = Field;
					const auto& Edits = Request.GetPayload().GetEdits();
					if (!Edits.IsEmpty())
					{
						if (const FString* Pointer = Request.GetPayload().FindInputPointer(Edits[0].GetIdentity().GetSemanticId()))
						{
							OutError.Source.JsonPath = JoinInputPointer(*Pointer, Field);
							OutError.SourceKind = TEXT("related_input");
						}
					}
				}
				return nullptr;
			}
			Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
			Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
			Controller->CleanAsset();
			Created = Retargeter;
		}
		if (!Created || Created->GetClass()->GetPathName() != Spec.NativeClassPath)
		{
			OutError = RigOperationFailure(TEXT("graph_compound_animation_factory_failed"),
				!Created ? TEXT("The animation asset factory returned no asset.")
					: TEXT("The animation asset factory created an asset with a different native class than requested."), Request.GetTargetRef(),
				nullptr,
				MakeShared<FJsonValueString>(Spec.NativeClassPath));
			if (Created) OutError.ActualValue = MakeShared<FJsonValueString>(Created->GetClass()->GetPathName());
			OutError.ActualValuePresent = Created != nullptr;
			return nullptr;
		}
		return Created;
	}

	bool TryApplyRigIkMutation(
		const FPcgAnimationGraphRouteProfile& Profile,
		UObject& Target,
		FRigIkReadbackSpec& Spec,
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
					*Blueprint, Spec.Controls, Spec.Units,
					Apply, Barrier, Topology,
					OutRevision, Evidence, NativeError);
			}
			else
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("The hierarchy graph mutation requires a ControlRigBlueprint target."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(UControlRigBlueprint::StaticClass()->GetPathName()));
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
			else
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("The shape library mutation requires a ControlRigShapeLibrary target."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(UControlRigShapeLibrary::StaticClass()->GetPathName()));
			}
		}
		else if (Capability == TEXT("animation.ik_rig.definition.apply"))
		{
			if (UIKRigDefinition* const Rig = Cast<UIKRigDefinition>(&Target))
			{
				bApplied = Hook.TryApplyAndCaptureIKRigDefinition(
					*Rig, Spec.RetargetRoot, Spec.Goals, Spec.Solvers, Spec.Chains,
					Apply, Barrier, Topology, OutRevision, Evidence, NativeError, Spec.RetargetRootInputPointer);
			}
			else
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("The IK definition mutation requires an IKRigDefinition target."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(UIKRigDefinition::StaticClass()->GetPathName()));
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
					OutRevision, Evidence, NativeError, Spec.AutoGenerateInputPointer);
			}
			else if (!Rig)
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("IK automatic generation requires an IKRigDefinition target."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(UIKRigDefinition::StaticClass()->GetPathName()));
			}
			else
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("The skeletal mesh for IK automatic generation could not be loaded."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Spec.SkeletalMeshRef), MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName()));
				NativeError.PropertyPath = TEXT("/skeletal_mesh_ref");
				if (!Spec.AutoGenerateInputPointer.IsEmpty())
				{
					NativeError.Source.JsonPath = JoinInputPointer(Spec.AutoGenerateInputPointer, TEXT("/skeletal_mesh_ref"));
					NativeError.SourceKind = TEXT("related_input");
				}
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
					Apply, Barrier, Topology, OutRevision, Evidence, NativeError, Spec.SourceRigInputPointer, Spec.TargetRigInputPointer);
			}
			else if (!Retargeter)
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					TEXT("The retarget definition mutation requires an IKRetargeter target."), Target.GetPathName(),
					MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()), MakeShared<FJsonValueString>(UIKRetargeter::StaticClass()->GetPathName()));
			}
			else
			{
				NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					!Source ? TEXT("The retarget definition source IK Rig could not be loaded.") : TEXT("The retarget definition target IK Rig could not be loaded."),
					Target.GetPathName(), MakeShared<FJsonValueString>(!Source ? Spec.SourceRigRef : Spec.TargetRigRef),
					MakeShared<FJsonValueString>(UIKRigDefinition::StaticClass()->GetPathName()));
				const FString& Pointer = !Source ? Spec.SourceRigInputPointer : Spec.TargetRigInputPointer;
				if (!Pointer.IsEmpty())
				{
					NativeError.Source.JsonPath = Pointer;
					NativeError.SourceKind = TEXT("related_input");
				}
			}
		}
		else if (UIKRetargeter* const Retargeter = Cast<UIKRetargeter>(&Target))
		{
			bApplied = Hook.TryApplyAndCaptureIKRetargeterSettings(
				*Retargeter, Spec.Settings, Apply, Barrier, Topology,
				OutRevision, Evidence, NativeError);
		}
		else
		{
			NativeError = RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
				TEXT("The retarget settings mutation requires an IKRetargeter target."), Target.GetPathName(),
				MakeShared<FJsonValueString>(Target.GetClass()->GetPathName()), MakeShared<FJsonValueString>(UIKRetargeter::StaticClass()->GetPathName()));
		}
		if (!bApplied || !Barrier.IsComplete() || Barrier.GetErrorCount() != 0)
		{
			OutError = NativeError.IsValid() ? NativeError
				: RigOperationFailure(TEXT("graph_compound_animation_native_failed"),
					!bApplied ? TEXT("The native animation mutation could not be applied.")
						: !Barrier.IsComplete() ? TEXT("The native animation mutation did not complete its compile or validation barrier.")
						: TEXT("The native animation mutation completed with compile or validation errors."), Target.GetPathName());
			if (!NativeError.IsValid())
			{
				const auto Facts = MakeShared<FJsonObject>();
				Facts->SetBoolField(TEXT("applied"), bApplied);
				Facts->SetBoolField(TEXT("barrier_complete"), Barrier.IsComplete());
				Facts->SetNumberField(TEXT("barrier_error_count"), Barrier.GetErrorCount());
				OutError.ActualValue = MakeShared<FJsonValueObject>(Facts);
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPathName();
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
