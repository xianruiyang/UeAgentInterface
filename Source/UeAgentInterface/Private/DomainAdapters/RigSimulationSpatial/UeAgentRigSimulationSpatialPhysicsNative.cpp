#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsAssetUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "UObject/UnrealType.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
    bool AuthorizesSimpleCollisionShapeResize(const FProperty* Property)
    {
        return Property && Property->GetOwnerStruct() == FKAggregateGeom::StaticStruct()
            && (Property->GetFName() == GET_MEMBER_NAME_CHECKED(FKAggregateGeom, SphereElems)
                || Property->GetFName() == GET_MEMBER_NAME_CHECKED(FKAggregateGeom, BoxElems)
                || Property->GetFName() == GET_MEMBER_NAME_CHECKED(FKAggregateGeom, SphylElems));
    }
	void RefreshPhysicsAsset(UPhysicsAsset& Asset)
	{
		Asset.UpdateBodySetupIndexMap();
		Asset.UpdateBoundsBodiesArray();
		Asset.InvalidateAllPhysicsMeshes();
		Asset.RefreshPhysicsAssetChange();
	}
	bool DeletePhysicsAssetBody(UPhysicsAsset& Asset, int32 BodyIndex, UE::UAI::Contract::FDiagnostic& Error,
		TMap<FName, FName>* ReconnectedNames)
	{
		if (!Asset.SkeletalBodySetups.IsValidIndex(BodyIndex))
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("physics_body_not_found"),
				TEXT("The body index is outside the physics asset body array."), TEXT("native_execution"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Asset.GetPathName();
			Error.PropertyPath = TEXT("/skeletal_body_setups");
			Error.ActualValuePresent = true;
			Error.ActualValue = MakeShared<FJsonValueNumber>(BodyIndex);
			Error.ExpectedValuePresent = true;
			Error.ExpectedValue = MakeShared<FJsonValueString>(
				FString::Printf(TEXT("0 <= body index < %d"), Asset.SkeletalBodySetups.Num()));
			return false;
		}
		Asset.Modify();
		auto* Body = Asset.SkeletalBodySetups[BodyIndex].Get();
		Body->Modify();
		// PhAT's editor wrapper is private. Reuse the same public utilities and
		// native frame owner; do not synthesize constraint transforms in UAI.
		if (USkeletalMesh* Mesh = Asset.GetPreviewMesh())
		{
			TArray<int32> Constraints, Children;
			Asset.BodyFindConstraints(BodyIndex, Constraints);
			Asset.GetNearestBodyIndicesBelow(Children, Body->BoneName, Mesh);
			const int32 Bone = Mesh->GetRefSkeleton().FindBoneIndex(Body->BoneName);
			const int32 Parent = Bone != INDEX_NONE ? Asset.FindParentBodyIndex(Mesh, Bone) : INDEX_NONE;
			if (Asset.SkeletalBodySetups.IsValidIndex(Parent))
				for (const int32 ConstraintIndex : Constraints)
				{
					auto* Constraint = Asset.ConstraintSetup[ConstraintIndex].Get();
					Constraint->Modify();
					for (const int32 Child : Children)
						if (Constraint->DefaultInstance.ConstraintBone1 == Asset.SkeletalBodySetups[Child]->BoneName)
						{
							const int32 NewIndex = FPhysicsAssetUtils::CreateNewConstraint(&Asset,
								Asset.SkeletalBodySetups[Child]->BoneName, Constraint);
							if (!Asset.ConstraintSetup.IsValidIndex(NewIndex))
							{
								Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("physics_native_constraint_reconnect_failed"),
									TEXT("Native constraint creation returned an index outside the physics asset constraint array."),
									TEXT("native_execution"));
								Error.SourceKind = TEXT("native_result");
								Error.TargetPath = Asset.GetPathName();
								Error.PropertyPath = TEXT("/constraint_setup");
								const auto Actual = MakeShared<FJsonObject>();
								Actual->SetNumberField(TEXT("returned_index"), NewIndex);
								Actual->SetNumberField(TEXT("constraint_count"), Asset.ConstraintSetup.Num());
								Actual->SetNumberField(TEXT("original_constraint_index"), ConstraintIndex);
								Actual->SetStringField(TEXT("child_bone"), Asset.SkeletalBodySetups[Child]->BoneName.ToString());
								Actual->SetStringField(TEXT("parent_bone"), Asset.SkeletalBodySetups[Parent]->BoneName.ToString());
								Error.ActualValuePresent = true;
								Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
								Error.ExpectedValuePresent = true;
								Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("0 <= returned_index < constraint_count"));
								return false;
							}
							auto* Reconnected = Asset.ConstraintSetup[NewIndex].Get();
							if (ReconnectedNames && Reconnected != Constraint) {
								const FName OldName = Constraint->DefaultInstance.JointName;
								const FName NewName = Reconnected->DefaultInstance.JointName;
								for (auto& Pair : *ReconnectedNames) if (Pair.Value == OldName) Pair.Value = NewName;
								ReconnectedNames->Add(OldName, NewName);
							}
							Reconnected->Modify(false);
							Reconnected->DefaultInstance.ConstraintBone1 = Asset.SkeletalBodySetups[Child]->BoneName;
							Reconnected->DefaultInstance.ConstraintBone2 = Asset.SkeletalBodySetups[Parent]->BoneName;
							Reconnected->DefaultInstance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, &Asset);
							Reconnected->SetDefaultProfile(Reconnected->DefaultInstance);
							Asset.DisableCollision(Child, Parent);
							}
				}
		}
		FPhysicsAssetUtils::DestroyBody(&Asset, BodyIndex);
		return true;
	}
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Discovery;
	using namespace StateSemantic;
	using namespace Verification;

	FUeAgentRigSimulationSpatialPhysicsProbeVector::
		FUeAgentRigSimulationSpatialPhysicsProbeVector(
			FString InSubjectRef,
			UPrimitiveComponent* InComponent,
			FUeAgentTemporalPoint InTime,
			FString InNativeFrameId,
			FVector InValue)
		: SubjectRef(MoveTemp(InSubjectRef))
		, Component(InComponent)
		, Time(MoveTemp(InTime))
		, NativeFrameId(MoveTemp(InNativeFrameId))
		, Value(InValue)
	{
	}

	namespace
	{
		using Contract::FDiagnostic;

		bool ProbeFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.TargetPath = Target;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValuePresent = Expected.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		void NativeContext(FDiagnostic& Error, const FString& Path = FString(), bool bCanonicalValue = false)
		{
			Error.SourceKind = TEXT("native_result");
			if (!Path.IsEmpty() || bCanonicalValue)
			{
				auto Locate = [&Path, bCanonicalValue](FString Pointer)
				{
					// Canonical constructors locate their value parameter at /value.
					if (bCanonicalValue && (Pointer == TEXT("/value") || Pointer.StartsWith(TEXT("/value/"))))
						Pointer.RightChopInline(6);
					if (Pointer.IsEmpty() || Pointer == TEXT("/")) return Path;
					if (!Pointer.StartsWith(TEXT("/")))
						Pointer = TEXT("/") + Pointer.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					return Path + Pointer;
				};
				if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.Source.JsonPath;
				Error.Source.JsonPath = Locate(Error.Source.JsonPath);
				Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Error.Source.JsonPath : Locate(Error.PropertyPath);
				if (!Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Locate(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) NativeContext(Child, Path, bCanonicalValue);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
				NativeContext(*Cause, Path, bCanonicalValue);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool NativeFailureAt(FDiagnostic& OutError, FDiagnostic Error, const FString& Path, const FString& Target = FString())
		{
			NativeContext(Error, Path);
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			OutError = MoveTemp(Error);
			return false;
		}

		bool FiniteVector(FDiagnostic& OutError, const FVector& Value, const FString& Path, const FString& Target)
		{
			const double Components[] = {Value.X, Value.Y, Value.Z};
			const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z")};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!FMath::IsFinite(Components[Index]))
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"),
						TEXT("A physics vector component is not finite."), Path + TEXT("/") + Axes[Index],
						MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Components[Index])),
						MakeShared<FJsonValueString>(TEXT("finite number")), Target);
			return true;
		}

		bool InputFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			ProbeFailure(OutError, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			OutError.SourceKind = TEXT("command_input");
			return false;
		}

		bool InputFailureAt(FDiagnostic& OutError, const FString& Path, const FString& Target = FString())
		{
			if (OutError.Source.InternalJsonPath.IsEmpty()) OutError.Source.InternalJsonPath = OutError.Source.JsonPath;
			OutError.Source.JsonPath = Path + OutError.Source.JsonPath;
			OutError.PropertyPath = Path + OutError.PropertyPath;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target;
			return false;
		}

		bool EnumFailure(FDiagnostic& OutError, const FString& Path, const FString& Actual, const TArray<FString>& Allowed)
		{
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const FString& Value : Allowed) Expected.Add(MakeShared<FJsonValueString>(Value));
			return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_enum_invalid"),
				TEXT("The physics setting is not one of the supported values."), Path,
				MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueArray>(MoveTemp(Expected)));
		}

		bool CapabilityFailure(FDiagnostic& OutError, const FString& Capability)
		{
			return ProbeFailure(OutError, TEXT("rig_simulation_spatial_family_port_unavailable"),
				TEXT("The physics native port does not support this capability."), TEXT("/capability_id"),
				MakeShared<FJsonValueString>(Capability), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(TEXT("physics.material.update")),
					MakeShared<FJsonValueString>(TEXT("physics.asset.structure.replace")),
					MakeShared<FJsonValueString>(TEXT("physics.constraint.update")),
					MakeShared<FJsonValueString>(TEXT("physics.simulation.probe"))}));
		}

		bool MissingNative(FDiagnostic& OutError, const FString& Code, const FString& Reason,
			const FString& Path, const FString& Target)
		{
			return ProbeFailure(OutError, Code, Reason, Path, MakeShared<FJsonValueBoolean>(false),
				MakeShared<FJsonValueBoolean>(true), Target);
		}

		bool IsCapability(const FString& CapabilityId)
		{
			return CapabilityId == TEXT("physics.material.update")
				|| CapabilityId == TEXT("physics.asset.structure.replace")
				|| CapabilityId == TEXT("physics.constraint.update")
				|| CapabilityId == TEXT("physics.simulation.probe");
		}

		const FUeAgentRigSimulationSpatialRequest* ProductionRequest(
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FString Path;
			FString Actual;
			FString Expected;
			if (Request->GetFamilyId() != TEXT("simulation_definition") && Request->GetFamilyId() != TEXT("simulation_observation"))
			{ Path = TEXT("/family_id"); Actual = Request->GetFamilyId(); Expected = TEXT("simulation_definition or simulation_observation"); }
			else if (Request->GetTypeId().IsEmpty())
			{ Path = TEXT("/type_id"); Actual = Request->GetTypeId(); Expected = TEXT("nonempty type identifier"); }
			else if (Request->GetRequestKindId().IsEmpty())
			{ Path = TEXT("/request_kind_id"); Actual = Request->GetRequestKindId(); Expected = TEXT("nonempty request kind identifier"); }
			if (!Path.IsEmpty())
			{
				ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_typed_request_invalid"),
					TEXT("The typed request does not satisfy the physics native port contract."), Path,
					MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected));
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
			if (Actual == Expected) return true;
			const FString Field = Actual.SourceId != Expected.SourceId ? TEXT("source_id")
				: Actual.Generation != Expected.Generation ? TEXT("generation") : TEXT("sequence");
			ProbeFailure(OutError, TEXT("rig_simulation_spatial_revision_mismatch"),
				TEXT("The current asset revision differs from the expected revision."), TEXT("/expected_revision/") + Field,
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ExplicitRef);
			if (Field == TEXT("sequence"))
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(::LexToString(Actual.Sequence));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(::LexToString(Expected.Sequence));
			}
			else
			{
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Field == TEXT("source_id") ? Actual.SourceId : Actual.Generation);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Field == TEXT("source_id") ? Expected.SourceId : Expected.Generation);
			}
			return false;
		}

		bool TryNumber(
			const FString& Text,
			double Minimum,
			double Maximum,
			double& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Path)
		{
			if (!LexTryParseString(OutValue, *Text) || !FMath::IsFinite(OutValue)
				|| OutValue < Minimum || OutValue > Maximum)
			{
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetNumberField(TEXT("minimum"), Minimum);
				Expected->SetNumberField(TEXT("maximum"), Maximum);
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_numeric_invalid"),
					TEXT("The physics numeric value must be finite and within the inclusive bounds."), Path,
					MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueObject>(Expected));
			}
			return true;
		}

		bool TryOptionalNumber(
			const FString& Text,
			double Minimum,
			double Maximum,
			double& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Path)
		{
			return Text.IsEmpty() || TryNumber(Text, Minimum, Maximum,
				OutValue, OutError, Path);
		}

		bool TryCollision(const FString& Text, ECollisionEnabled::Type& Out)
		{
			if (Text == TEXT("no_collision")) Out = ECollisionEnabled::NoCollision;
			else if (Text == TEXT("query_only")) Out = ECollisionEnabled::QueryOnly;
			else if (Text == TEXT("physics_only")) Out = ECollisionEnabled::PhysicsOnly;
			else if (Text == TEXT("query_and_physics")) Out = ECollisionEnabled::QueryAndPhysics;
			else if (Text == TEXT("probe_only")) Out = ECollisionEnabled::ProbeOnly;
			else if (Text == TEXT("query_and_probe")) Out = ECollisionEnabled::QueryAndProbe;
			else return false;
			return true;
		}

		bool TryPhysicsType(const FString& Text, EPhysicsType& Out)
		{
			if (Text == TEXT("default")) Out = PhysType_Default;
			else if (Text == TEXT("kinematic")) Out = PhysType_Kinematic;
			else if (Text == TEXT("simulated")) Out = PhysType_Simulated;
			else return false;
			return true;
		}

		bool TryLinearMotion(const FString& Text, ELinearConstraintMotion& Out)
		{
			if (Text == TEXT("free")) Out = LCM_Free;
			else if (Text == TEXT("limited")) Out = LCM_Limited;
			else if (Text == TEXT("locked")) Out = LCM_Locked;
			else return false;
			return true;
		}

		bool TryAngularMotion(const FString& Text, EAngularConstraintMotion& Out)
		{
			if (Text == TEXT("free")) Out = ACM_Free;
			else if (Text == TEXT("limited")) Out = ACM_Limited;
			else if (Text == TEXT("locked")) Out = ACM_Locked;
			else return false;
			return true;
		}

		bool TryCombineMode(const FString& Text, EFrictionCombineMode::Type& Out)
		{
			if (Text == TEXT("average")) Out = EFrictionCombineMode::Average;
			else if (Text == TEXT("min")) Out = EFrictionCombineMode::Min;
			else if (Text == TEXT("multiply")) Out = EFrictionCombineMode::Multiply;
			else if (Text == TEXT("max")) Out = EFrictionCombineMode::Max;
			else return false;
			return true;
		}

		bool ApplyMaterialState(
			UPhysicalMaterial& Material,
			const FRigSimulationSpatialMaterialState& State,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			double Value = 0.0;
			if (!TryOptionalNumber(State.Friction, 0.0, MAX_flt, Value,
				OutError, TEXT("/friction"))) return false;
			if (!State.Friction.IsEmpty()) Material.Friction = Value;
			if (!TryOptionalNumber(State.StaticFriction, 0.0, MAX_flt, Value,
				OutError, TEXT("/static_friction"))) return false;
			if (!State.StaticFriction.IsEmpty()) Material.StaticFriction = Value;
			if (!TryOptionalNumber(State.Restitution, 0.0, 1.0, Value,
				OutError, TEXT("/restitution"))) return false;
			if (!State.Restitution.IsEmpty()) Material.Restitution = Value;
			if (!TryOptionalNumber(State.Density, 0.0, MAX_flt, Value,
				OutError, TEXT("/density"))) return false;
			if (!State.Density.IsEmpty()) Material.Density = Value;
			if (!TryOptionalNumber(State.RaiseMassToPower, 0.1, 1.0, Value,
				OutError, TEXT("/raise_mass_to_power"))) return false;
			if (!State.RaiseMassToPower.IsEmpty()) Material.RaiseMassToPower = Value;
			if (!State.FrictionCombineMode.IsEmpty())
			{
				EFrictionCombineMode::Type Mode;
				if (!TryCombineMode(State.FrictionCombineMode, Mode)) return EnumFailure(OutError,
					TEXT("/friction_combine_mode"), State.FrictionCombineMode, {TEXT("average"), TEXT("min"), TEXT("multiply"), TEXT("max")});
				Material.FrictionCombineMode = Mode;
				Material.bOverrideFrictionCombineMode = true;
			}
			if (!State.RestitutionCombineMode.IsEmpty())
			{
				EFrictionCombineMode::Type Mode;
				if (!TryCombineMode(State.RestitutionCombineMode, Mode)) return EnumFailure(OutError,
					TEXT("/restitution_combine_mode"), State.RestitutionCombineMode, {TEXT("average"), TEXT("min"), TEXT("multiply"), TEXT("max")});
				Material.RestitutionCombineMode = Mode;
				Material.bOverrideRestitutionCombineMode = true;
			}
			return true;
		}

		bool ApplyBodyState(
			USkeletalBodySetup& Body,
			const FRigSimulationSpatialPhysicsBodyState& State,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (!State.CollisionEnabled.IsEmpty())
			{
				ECollisionEnabled::Type Collision;
				if (!TryCollision(State.CollisionEnabled, Collision)) return EnumFailure(OutError,
					TEXT("/collision_enabled"), State.CollisionEnabled, {TEXT("no_collision"), TEXT("query_only"), TEXT("physics_only"), TEXT("query_and_physics"), TEXT("probe_only"), TEXT("query_and_probe")});
				Body.DefaultInstance.SetCollisionEnabled(Collision, false);
			}
			if (!State.PhysicsType.IsEmpty())
			{
				EPhysicsType PhysicsType;
				if (!TryPhysicsType(State.PhysicsType, PhysicsType)) return EnumFailure(OutError,
					TEXT("/physics_type"), State.PhysicsType, {TEXT("default"), TEXT("kinematic"), TEXT("simulated")});
				Body.PhysicsType = PhysicsType;
			}
			if (!State.MassKg.IsEmpty())
			{
				double Mass = 0.0;
				if (!TryNumber(State.MassKg, UE_SMALL_NUMBER, MAX_flt, Mass,
					OutError, TEXT("/mass_kg"))) return false;
				Body.DefaultInstance.SetMassOverride(Mass, true);
			}
			if (State.ConsiderForBounds.IsSet())
				Body.bConsiderForBounds = State.ConsiderForBounds.GetValue();
			return true;
		}

		bool ApplyConstraintState(
			FConstraintInstance& Constraint,
			const FRigSimulationSpatialConstraintState& State,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			ELinearConstraintMotion X = Constraint.GetLinearXMotion();
			ELinearConstraintMotion Y = Constraint.GetLinearYMotion();
			ELinearConstraintMotion Z = Constraint.GetLinearZMotion();
			double LinearLimit = Constraint.GetLinearLimit();
			if (!State.LinearXMotion.IsEmpty() && !TryLinearMotion(State.LinearXMotion, X))
				return EnumFailure(OutError, TEXT("/linear_x_motion"), State.LinearXMotion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!State.LinearYMotion.IsEmpty() && !TryLinearMotion(State.LinearYMotion, Y))
				return EnumFailure(OutError, TEXT("/linear_y_motion"), State.LinearYMotion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!State.LinearZMotion.IsEmpty() && !TryLinearMotion(State.LinearZMotion, Z))
				return EnumFailure(OutError, TEXT("/linear_z_motion"), State.LinearZMotion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!TryOptionalNumber(State.LinearLimitCm, 0.0, MAX_flt, LinearLimit,
				OutError, TEXT("/linear_limit_cm"))) return false;
			Constraint.SetLinearLimits(X, Y, Z, LinearLimit);

			EAngularConstraintMotion Twist = Constraint.GetAngularTwistMotion();
			EAngularConstraintMotion Swing1 = Constraint.GetAngularSwing1Motion();
			EAngularConstraintMotion Swing2 = Constraint.GetAngularSwing2Motion();
			double TwistLimit = Constraint.GetAngularTwistLimit();
			double Swing1Limit = Constraint.GetAngularSwing1Limit();
			double Swing2Limit = Constraint.GetAngularSwing2Limit();
			if (!State.TwistMotion.IsEmpty() && !TryAngularMotion(State.TwistMotion, Twist))
				return EnumFailure(OutError, TEXT("/twist_motion"), State.TwistMotion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!State.Swing1Motion.IsEmpty() && !TryAngularMotion(State.Swing1Motion, Swing1))
				return EnumFailure(OutError, TEXT("/swing1_motion"), State.Swing1Motion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!State.Swing2Motion.IsEmpty() && !TryAngularMotion(State.Swing2Motion, Swing2))
				return EnumFailure(OutError, TEXT("/swing2_motion"), State.Swing2Motion, {TEXT("free"), TEXT("limited"), TEXT("locked")});
			if (!TryOptionalNumber(State.TwistLimitDegrees, 0.0, 180.0,
				TwistLimit, OutError, TEXT("/twist_limit_degrees"))
				|| !TryOptionalNumber(State.Swing1LimitDegrees, 0.0, 180.0,
					Swing1Limit, OutError, TEXT("/swing1_limit_degrees"))
				|| !TryOptionalNumber(State.Swing2LimitDegrees, 0.0, 180.0,
					Swing2Limit, OutError, TEXT("/swing2_limit_degrees"))) return false;
			Constraint.SetAngularTwistLimit(Twist, TwistLimit);
			Constraint.SetAngularSwing1Limit(Swing1, Swing1Limit);
			Constraint.SetAngularSwing2Limit(Swing2, Swing2Limit);
			if (State.DisableCollision.IsSet())
				Constraint.SetDisableCollision(State.DisableCollision.GetValue());
			return true;
		}

		const FUeAgentSpatialValue* FindFormalValue(
			const FUeAgentRigSimulationSpatialRequest& Request,
			const FString& Subject,
			EUeAgentSpatialQuantityKind Kind,
			int32 Ordinal = 0)
		{
			for (const FUeAgentSpatialValuePtr& Value
				: Request.GetFormalPortRequest().GetSpatialValues())
			{
				if (Value.IsValid() && Value->GetSubject() == Subject
					&& Value->GetQuantityKind() == Kind)
				{
					if (Ordinal-- == 0) return Value.Get();
				}
			}
			return nullptr;
		}

		bool TryNativeTransform(
			const FUeAgentRigSimulationSpatialRequest& Request,
			const FString& Subject,
			const FString& NativeFrame,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FTransform& OutTransform,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const FUeAgentSpatialValue* Source = FindFormalValue(
				Request, Subject, EUeAgentSpatialQuantityKind::Transform);
			FUeAgentSpatialValuePtr Converted;
			if (!Source) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_transform_missing"),
				TEXT("The formal request has no transform for this subject."), TEXT("/spatial_values"),
				MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a subject transform")), Subject);
			if (!Environment.TryConvertSpatialValueToFrame(*Source, NativeFrame, Resources, Converted, OutError)) return false;
			if (!Converted.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_transform_missing"),
				TEXT("Spatial conversion returned no native transform."), TEXT("/converted_transform"),
				MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a converted native transform")), Subject);
			if (Converted->GetComponents().Num() != 16)
				return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_transform_invalid"),
					TEXT("The converted transform requires sixteen matrix components."), TEXT("/converted_transform/components"),
					MakeShared<FJsonValueNumber>(Converted->GetComponents().Num()), MakeShared<FJsonValueNumber>(16), Subject);
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Value = 0.0;
					if (!TryNumber(Converted->GetComponents()[Row * 4 + Column],
						-MAX_flt, MAX_flt, Value, OutError, FString()))
						return NativeFailureAt(OutError, OutError, TEXT("/converted_transform/components/") + FString::FromInt(Row * 4 + Column), Subject);
					Matrix.M[Column][Row] = Value;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			if (OutTransform.IsValid()) return true;
			const FQuat Rotation = OutTransform.GetRotation();
			const double Components[] = {Rotation.X, Rotation.Y, Rotation.Z, Rotation.W};
			const TCHAR* Axes[] = {TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")};
			for (int32 Index = 0; Index < 4; ++Index)
				if (!FMath::IsFinite(Components[Index]))
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_transform_invalid"),
						TEXT("The native transform quaternion contains a non-finite component."), TEXT("/converted_transform/rotation/") + FString(Axes[Index]),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Components[Index])), MakeShared<FJsonValueString>(TEXT("finite number")), Subject);
			if (!FiniteVector(OutError, OutTransform.GetTranslation(), TEXT("/converted_transform/translation"), Subject)
				|| !FiniteVector(OutError, OutTransform.GetScale3D(), TEXT("/converted_transform/scale"), Subject)) return false;
			return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_transform_invalid"),
				TEXT("The converted transform quaternion is not normalized."), TEXT("/converted_transform/rotation"),
				MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Rotation.X), MakeShared<FJsonValueNumber>(Rotation.Y),
					MakeShared<FJsonValueNumber>(Rotation.Z), MakeShared<FJsonValueNumber>(Rotation.W)}),
				MakeShared<FJsonValueString>(TEXT("a normalized quaternion")), Subject);
		}

		const FUeAgentRigSimulationSpatialPhysicsBodyBinding* FindBodyBinding(
			const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding,
			const FString& Identity)
		{
			return Binding.BodyBindings.FindByPredicate(
				[&Identity](const auto& Item){ return Item.Identity == Identity; });
		}

		const FUeAgentRigSimulationSpatialPhysicsShapeBinding* FindShapeBinding(
			const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding,
			const FString& Identity)
		{
			return Binding.ShapeBindings.FindByPredicate(
				[&Identity](const auto& Item){ return Item.Identity == Identity; });
		}

		const FUeAgentRigSimulationSpatialPhysicsConstraintBinding* FindConstraintBinding(
			const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding,
			const FString& Identity)
		{
			return Binding.ConstraintBindings.FindByPredicate(
				[&Identity](const auto& Item){ return Item.Identity == Identity; });
		}

		USkeletalBodySetup* FindBody(UPhysicsAsset& Asset, FName BoneName)
		{
			const TObjectPtr<USkeletalBodySetup>* Found = Asset.SkeletalBodySetups.FindByPredicate(
				[BoneName](const TObjectPtr<USkeletalBodySetup>& Body)
				{
					return Body && Body->BoneName == BoneName;
				});
			return Found ? Found->Get() : nullptr;
		}

		UPhysicsConstraintTemplate* FindConstraint(UPhysicsAsset& Asset, FName JointName)
		{
			const TObjectPtr<UPhysicsConstraintTemplate>* Found = Asset.ConstraintSetup.FindByPredicate(
				[JointName](const TObjectPtr<UPhysicsConstraintTemplate>& Constraint)
				{
					return Constraint && Constraint->DefaultInstance.JointName == JointName;
				});
			return Found ? Found->Get() : nullptr;
		}

		bool AddShape(
			USkeletalBodySetup& Body,
			const FUeAgentRigSimulationSpatialPhysicsShapeBinding& Shape,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Shape.ShapeKind == TEXT("box"))
			{
				FKBoxElem& Elem = Body.AggGeom.BoxElems.AddDefaulted_GetRef();
				Elem.SetName(Shape.NativeName);
				Elem.X = Shape.Dimensions.X;
				Elem.Y = Shape.Dimensions.Y;
				Elem.Z = Shape.Dimensions.Z;
				Elem.SetTransform(Shape.LocalTransform);
				return true;
			}
			if (Shape.ShapeKind == TEXT("sphere"))
			{
				FKSphereElem& Elem = Body.AggGeom.SphereElems.AddDefaulted_GetRef();
				Elem.SetName(Shape.NativeName);
				Elem.Radius = Shape.Dimensions.X;
				Elem.SetTransform(Shape.LocalTransform);
				return true;
			}
			if (Shape.ShapeKind == TEXT("capsule"))
			{
				FKSphylElem& Elem = Body.AggGeom.SphylElems.AddDefaulted_GetRef();
				Elem.SetName(Shape.NativeName);
				Elem.Radius = Shape.Dimensions.X;
				Elem.Length = Shape.Dimensions.Y;
				Elem.SetTransform(Shape.LocalTransform);
				return true;
			}
			return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_shape_kind_invalid"),
				TEXT("The registered shape kind is not supported."), TEXT("/shape_kind"),
				MakeShared<FJsonValueString>(Shape.ShapeKind), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(TEXT("box")), MakeShared<FJsonValueString>(TEXT("sphere")), MakeShared<FJsonValueString>(TEXT("capsule"))}), Shape.Identity);
		}

		bool ApplyAssetStructure(
			UPhysicsAsset& Asset,
			const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding,
			const FPhysicsAssetStructureReplaceInput& Input,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Input.Bodies.Num() != Binding.BodyBindings.Num())
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_structure_not_complete"), TEXT("The body list does not cover the registered physics bodies."), TEXT("/bodies"), MakeShared<FJsonValueNumber>(Input.Bodies.Num()), MakeShared<FJsonValueNumber>(Binding.BodyBindings.Num()), Input.PhysicsAssetRef);
			if (Input.Constraints.Num() != Binding.ConstraintBindings.Num())
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_structure_not_complete"), TEXT("The constraint list does not cover the registered physics constraints."), TEXT("/constraints"), MakeShared<FJsonValueNumber>(Input.Constraints.Num()), MakeShared<FJsonValueNumber>(Binding.ConstraintBindings.Num()), Input.PhysicsAssetRef);
			Asset.SkeletalBodySetups.Reset();
			Asset.ConstraintSetup.Reset();
			Asset.PhysicalAnimationProfiles.Reset();
			Asset.ConstraintProfiles.Reset();
			TSet<FString> SeenBodies;
			TSet<FString> SeenShapes;
			for (int32 BodyIndex = 0; BodyIndex < Input.Bodies.Num(); ++BodyIndex)
			{
				const auto& BodyInput = Input.Bodies[BodyIndex];
				const FString BodyPath = TEXT("/bodies/") + FString::FromInt(BodyIndex);
				const auto* BodyBinding = FindBodyBinding(Binding, BodyInput.BodyRef);
				if (!BodyBinding) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_body_binding_invalid"), TEXT("The body reference has no registered physics binding."), BodyPath + TEXT("/body_ref"), MakeShared<FJsonValueString>(BodyInput.BodyRef), MakeShared<FJsonValueString>(TEXT("a registered body reference")), Input.PhysicsAssetRef);
				if (BodyBinding->BoneIdentity != BodyInput.BoneRef)
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_body_binding_invalid"), TEXT("The requested bone does not match the body binding."), BodyPath + TEXT("/bone_ref"), MakeShared<FJsonValueString>(BodyInput.BoneRef), MakeShared<FJsonValueString>(BodyBinding->BoneIdentity), BodyInput.BodyRef);
				if (SeenBodies.Contains(BodyInput.BodyRef))
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_body_binding_invalid"), TEXT("The body reference occurs more than once."), BodyPath + TEXT("/body_ref"), MakeShared<FJsonValueString>(BodyInput.BodyRef), MakeShared<FJsonValueString>(TEXT("a unique body reference")), Input.PhysicsAssetRef);
				SeenBodies.Add(BodyInput.BodyRef);
				USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(&Asset,
					MakeUniqueObjectName(&Asset, USkeletalBodySetup::StaticClass(),
						BodyBinding->BoneName), RF_Transactional);
				if (!Body) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_body_create_failed"),
					TEXT("Creating the physics body failed."), BodyPath + TEXT("/native_body"), BodyInput.BodyRef);
				Body->BoneName = BodyBinding->BoneName;
				if (!ApplyBodyState(*Body, BodyInput.State, OutError)) return InputFailureAt(OutError, BodyPath + TEXT("/state"), BodyInput.BodyRef);
				for (int32 ShapeIndex = 0; ShapeIndex < BodyInput.ShapeRefs.Num(); ++ShapeIndex)
				{
					const FString& ShapeRef = BodyInput.ShapeRefs[ShapeIndex];
					const FString ShapePath = BodyPath + TEXT("/shape_refs/") + FString::FromInt(ShapeIndex);
					const auto* Shape = FindShapeBinding(Binding, ShapeRef);
					if (!Shape) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_shape_binding_invalid"), TEXT("The shape reference has no registered shape binding."), ShapePath, MakeShared<FJsonValueString>(ShapeRef), MakeShared<FJsonValueString>(TEXT("a registered shape reference")), BodyInput.BodyRef);
					if (Shape->BodyIdentity != BodyInput.BodyRef)
						return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_shape_binding_invalid"), TEXT("The shape is bound to a different physics body."), ShapePath, MakeShared<FJsonValueString>(Shape->BodyIdentity), MakeShared<FJsonValueString>(BodyInput.BodyRef), ShapeRef);
					if (SeenShapes.Contains(ShapeRef))
						return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_shape_binding_invalid"), TEXT("The shape reference occurs more than once."), ShapePath, MakeShared<FJsonValueString>(ShapeRef), MakeShared<FJsonValueString>(TEXT("a unique shape reference")), BodyInput.BodyRef);
					if (!AddShape(*Body, *Shape, OutError)) return NativeFailureAt(OutError, OutError, ShapePath + TEXT("/binding"), ShapeRef);
					SeenShapes.Add(ShapeRef);
				}
				Asset.SkeletalBodySetups.Add(Body);
			}
			if (SeenShapes.Num() != Binding.ShapeBindings.Num())
			{
				TArray<TSharedPtr<FJsonValue>> Missing;
				for (const auto& Shape : Binding.ShapeBindings)
					if (!SeenShapes.Contains(Shape.Identity)) Missing.Add(MakeShared<FJsonValueString>(Shape.Identity));
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_shape_set_not_complete"),
					TEXT("Some registered shapes are not referenced by any requested body."), TEXT("/bodies"),
					MakeShared<FJsonValueArray>(MoveTemp(Missing)), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{}), Input.PhysicsAssetRef);
			}

			TSet<FString> SeenConstraints;
			for (int32 ConstraintIndex = 0; ConstraintIndex < Input.Constraints.Num(); ++ConstraintIndex)
			{
				const auto& ConstraintInput = Input.Constraints[ConstraintIndex];
				const FString ConstraintPath = TEXT("/constraints/") + FString::FromInt(ConstraintIndex);
				const auto* ConstraintBinding = FindConstraintBinding(
					Binding, ConstraintInput.ConstraintRef);
				const auto* BodyA = ConstraintBinding
					? FindBodyBinding(Binding, ConstraintBinding->BodyAIdentity) : nullptr;
				const auto* BodyB = ConstraintBinding
					? FindBodyBinding(Binding, ConstraintBinding->BodyBIdentity) : nullptr;
				if (!ConstraintBinding) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The constraint reference has no registered binding."), ConstraintPath + TEXT("/constraint_ref"), MakeShared<FJsonValueString>(ConstraintInput.ConstraintRef), MakeShared<FJsonValueString>(TEXT("a registered constraint reference")), Input.PhysicsAssetRef);
				if (!BodyA) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"),
					TEXT("The registered first constraint body has no body binding."), ConstraintPath + TEXT("/binding/body_a"), ConstraintInput.ConstraintRef);
				if (!BodyB) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"),
					TEXT("The registered second constraint body has no body binding."), ConstraintPath + TEXT("/binding/body_b"), ConstraintInput.ConstraintRef);
				if (ConstraintInput.EndpointA.BodyRef != ConstraintBinding->BodyAIdentity)
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The first endpoint does not match the registered constraint body."), ConstraintPath + TEXT("/endpoint_a/body_ref"), MakeShared<FJsonValueString>(ConstraintInput.EndpointA.BodyRef), MakeShared<FJsonValueString>(ConstraintBinding->BodyAIdentity), ConstraintInput.ConstraintRef);
				if (ConstraintInput.EndpointB.BodyRef != ConstraintBinding->BodyBIdentity)
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The second endpoint does not match the registered constraint body."), ConstraintPath + TEXT("/endpoint_b/body_ref"), MakeShared<FJsonValueString>(ConstraintInput.EndpointB.BodyRef), MakeShared<FJsonValueString>(ConstraintBinding->BodyBIdentity), ConstraintInput.ConstraintRef);
				if (SeenConstraints.Contains(ConstraintInput.ConstraintRef))
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The constraint reference occurs more than once."), ConstraintPath + TEXT("/constraint_ref"), MakeShared<FJsonValueString>(ConstraintInput.ConstraintRef), MakeShared<FJsonValueString>(TEXT("a unique constraint reference")), Input.PhysicsAssetRef);
				SeenConstraints.Add(ConstraintInput.ConstraintRef);
				FTransform FrameA;
				FTransform FrameB;
				if (!TryNativeTransform(Request, ConstraintInput.EndpointA.BodyRef,
						ConstraintBinding->NativeFrameAId, Environment, Resources,
						FrameA, OutError)
					|| !TryNativeTransform(Request, ConstraintInput.EndpointB.BodyRef,
						ConstraintBinding->NativeFrameBId, Environment, Resources,
						FrameB, OutError)) return false;
				UPhysicsConstraintTemplate* Constraint =
					NewObject<UPhysicsConstraintTemplate>(&Asset,
						MakeUniqueObjectName(&Asset,
							UPhysicsConstraintTemplate::StaticClass(),
							ConstraintBinding->NativeName), RF_Transactional);
				if (!Constraint) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_constraint_create_failed"),
					TEXT("Creating the native physics constraint failed."), ConstraintPath + TEXT("/native_constraint"), ConstraintInput.ConstraintRef);
				Constraint->DefaultInstance.JointName = ConstraintBinding->NativeName;
				Constraint->DefaultInstance.ConstraintBone1 = BodyA->BoneName;
				Constraint->DefaultInstance.ConstraintBone2 = BodyB->BoneName;
				Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FrameA);
				Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame2, FrameB);
				if (!ApplyConstraintState(Constraint->DefaultInstance, ConstraintInput.State, OutError))
					return InputFailureAt(OutError, ConstraintPath + TEXT("/state"), ConstraintInput.ConstraintRef);
				Asset.ConstraintSetup.Add(Constraint);
			}
			TSet<FName> ProfileNames;
			for (int32 ProfileIndex = 0; ProfileIndex < Input.Profiles.Num(); ++ProfileIndex)
			{
				const auto& Profile = Input.Profiles[ProfileIndex];
				const FString ProfilePath = TEXT("/profiles/") + FString::FromInt(ProfileIndex);
				const FName ProfileName(*Profile.ProfileName);
				if (ProfileName.IsNone() || ProfileNames.Contains(ProfileName))
					return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_profile_invalid"),
						ProfileName.IsNone() ? TEXT("The profile name resolves to None.") : TEXT("The profile name occurs more than once."),
						ProfilePath + TEXT("/profile_name"), MakeShared<FJsonValueString>(Profile.ProfileName),
						MakeShared<FJsonValueString>(TEXT("a unique non-None profile name")), Input.PhysicsAssetRef);
				ProfileNames.Add(ProfileName);
				Asset.PhysicalAnimationProfiles.Add(ProfileName);
				Asset.ConstraintProfiles.Add(ProfileName);
				for (int32 ReferenceIndex = 0; ReferenceIndex < Profile.BodyRefs.Num(); ++ReferenceIndex)
				{
					const FString& BodyRef = Profile.BodyRefs[ReferenceIndex];
					const auto* BodyBinding = FindBodyBinding(Binding, BodyRef);
					USkeletalBodySetup* Body = BodyBinding
						? FindBody(Asset, BodyBinding->BoneName) : nullptr;
					if (!Body) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_profile_body_invalid"), TEXT("The profile body reference does not resolve to an available physics body."), ProfilePath + TEXT("/body_refs/") + FString::FromInt(ReferenceIndex), MakeShared<FJsonValueString>(BodyRef), MakeShared<FJsonValueString>(TEXT("an available physics body reference")), Input.PhysicsAssetRef);
					Body->AddPhysicalAnimationProfile(ProfileName);
				}
				for (int32 ReferenceIndex = 0; ReferenceIndex < Profile.ConstraintRefs.Num(); ++ReferenceIndex)
				{
					const FString& ConstraintRef = Profile.ConstraintRefs[ReferenceIndex];
					const auto* ConstraintBinding = FindConstraintBinding(Binding, ConstraintRef);
					UPhysicsConstraintTemplate* Constraint = ConstraintBinding
						? FindConstraint(Asset, ConstraintBinding->NativeName) : nullptr;
					if (!Constraint) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_profile_constraint_invalid"), TEXT("The profile constraint reference does not resolve to an available physics constraint."), ProfilePath + TEXT("/constraint_refs/") + FString::FromInt(ReferenceIndex), MakeShared<FJsonValueString>(ConstraintRef), MakeShared<FJsonValueString>(TEXT("an available physics constraint reference")), Input.PhysicsAssetRef);
					Constraint->AddConstraintProfile(ProfileName);
				}
			}
			Asset.UpdateBodySetupIndexMap();
			Asset.UpdateBoundsBodiesArray();
			return true;
		}

		bool ApplyConstraintUpdate(
			UPhysicsAsset& Asset,
			const FUeAgentRigSimulationSpatialPhysicsAssetBinding& Binding,
			const FPhysicsConstraintUpdateInput& Input,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			const auto* ConstraintBinding = FindConstraintBinding(Binding, Input.ConstraintRef);
			if (!ConstraintBinding) return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The constraint reference has no registered binding."), TEXT("/constraint_ref"), MakeShared<FJsonValueString>(Input.ConstraintRef), MakeShared<FJsonValueString>(TEXT("a registered constraint reference")), Input.PhysicsAssetRef);
			if (Input.EndpointA.BodyRef != ConstraintBinding->BodyAIdentity)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The first endpoint does not match the registered constraint body."), TEXT("/endpoint_a/body_ref"), MakeShared<FJsonValueString>(Input.EndpointA.BodyRef), MakeShared<FJsonValueString>(ConstraintBinding->BodyAIdentity), Input.ConstraintRef);
			if (Input.EndpointB.BodyRef != ConstraintBinding->BodyBIdentity)
				return InputFailure(OutError, TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"), TEXT("The second endpoint does not match the registered constraint body."), TEXT("/endpoint_b/body_ref"), MakeShared<FJsonValueString>(Input.EndpointB.BodyRef), MakeShared<FJsonValueString>(ConstraintBinding->BodyBIdentity), Input.ConstraintRef);
			UPhysicsConstraintTemplate* Constraint =
				FindConstraint(Asset, ConstraintBinding->NativeName);
			if (!Constraint) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_constraint_unavailable"),
				TEXT("The bound native physics constraint is unavailable."), TEXT("/constraint_ref"), Input.ConstraintRef);
			FTransform FrameA;
			FTransform FrameB;
			if (!TryNativeTransform(Request, Input.EndpointA.BodyRef,
					ConstraintBinding->NativeFrameAId, Environment, Resources,
					FrameA, OutError)
				|| !TryNativeTransform(Request, Input.EndpointB.BodyRef,
					ConstraintBinding->NativeFrameBId, Environment, Resources,
					FrameB, OutError)) return false;
			Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FrameA);
			Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame2, FrameB);
			if (!ApplyConstraintState(Constraint->DefaultInstance, Input.ConstraintState, OutError))
				return InputFailureAt(OutError, TEXT("/constraint_state"), Input.ConstraintRef);
			return true;
		}

		bool TryVector(
			const FUeAgentSpatialValue& Value,
			FVector& OutVector,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Direction)
			{
				const auto Kind = Value.GetQuantityKind();
				const FString Actual = Kind == EUeAgentSpatialQuantityKind::Point ? TEXT("point")
					: Kind == EUeAgentSpatialQuantityKind::Normal ? TEXT("normal")
					: Kind == EUeAgentSpatialQuantityKind::Transform ? TEXT("transform") : TEXT("unknown");
				return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"),
					TEXT("A physics vector requires a direction quantity."), TEXT("/quantity_kind"),
					MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(TEXT("direction")), Value.GetSubject());
			}
			if (Value.GetComponents().Num() != 3)
				return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"),
					TEXT("A physics vector requires exactly three components."), TEXT("/components"),
					MakeShared<FJsonValueNumber>(Value.GetComponents().Num()), MakeShared<FJsonValueNumber>(3), Value.GetSubject());
			double Components[3] = {};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!TryNumber(Value.GetComponents()[Index], -MAX_flt, MAX_flt, Components[Index], OutError, FString()))
					return NativeFailureAt(OutError, OutError, TEXT("/components/") + FString::FromInt(Index));
			OutVector = FVector(Components[0], Components[1], Components[2]);
			return true;
		}

		bool BuildProbeRequest(
			const FPhysicsSimulationProbeInput& Input,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			OutRequest = {};
			OutRequest.Session = Input.SimulationSession;
			FString NativeClock;
			for (int32 Index = 0; Index < Input.SubjectRefs.Num(); ++Index)
			{
				const FString& SubjectRef = Input.SubjectRefs[Index];
				const FString Path = TEXT("/subject_refs/") + FString::FromInt(Index);
				const auto* Binding = Environment.FindPhysicsSubjectBinding(SubjectRef);
				if (!Binding) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_binding_unavailable"),
					TEXT("No physics subject binding is available for this reference."), Path,
					MakeShared<FJsonValueString>(SubjectRef), MakeShared<FJsonValueString>(TEXT("a registered physics subject reference")), SubjectRef);
				if (!Binding->TryValidate(OutError)) return NativeFailureAt(OutError, OutError, Path + TEXT("/binding"), SubjectRef);
				if (Environment.ResolveExplicitObject(SubjectRef) != Binding->Component.Get())
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_binding_unavailable"),
						TEXT("The subject reference resolves to a different object than the bound physics component."), Path,
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
				if (NativeClock.IsEmpty()) NativeClock = Binding->NativeSimulationClockId;
				if (NativeClock != Binding->NativeSimulationClockId)
				{
					ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_clock_mismatch"),
						TEXT("All physics probe subjects must use the same native simulation clock."), Path + TEXT("/binding/native_simulation_clock_id"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Binding->NativeSimulationClockId);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(NativeClock);
					return false;
				}
				OutRequest.Subjects.Add({SubjectRef, Binding->Component, Binding->NativeSimulationFrameId});
			}
			const TArray<FUeAgentTemporalRange>& Ranges = Request.GetFormalPortRequest().GetTemporalRanges();
			if (Ranges.Num() != 1) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_range_invalid"),
				TEXT("The formal physics probe request requires exactly one simulation range."), TEXT("/temporal_ranges"),
				MakeShared<FJsonValueNumber>(Ranges.Num()), MakeShared<FJsonValueNumber>(1), Input.SimulationSession.OwnerRef);
			TOptional<FUeAgentTemporalRange> NativeRange;
			if (!Environment.TryConvertTemporalRangeToClock(Ranges[0], NativeClock, Resources, NativeRange, OutError)) return false;
			if (!NativeRange.IsSet()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_range_invalid"),
				TEXT("Temporal conversion returned no native simulation range."), TEXT("/simulation_range"),
				MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a converted native simulation range")), Input.SimulationSession.OwnerRef);
			OutRequest.SimulationRange = NativeRange.GetValue();

			TArray<const FUeAgentSpatialValue*> Directions;
			for (const FUeAgentSpatialValuePtr& Value : Request.GetFormalPortRequest().GetSpatialValues())
				if (Value.IsValid() && Value->GetQuantityKind() == EUeAgentSpatialQuantityKind::Direction)
					Directions.Add(Value.Get());
			if (Directions.Num() != Input.Impulses.Num() + Input.Velocities.Num())
				return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_vector_count_mismatch"),
					TEXT("The formal direction count does not match the impulse and velocity sample count."), TEXT("/spatial_values"),
					MakeShared<FJsonValueNumber>(Directions.Num()), MakeShared<FJsonValueNumber>(Input.Impulses.Num() + Input.Velocities.Num()), Input.SimulationSession.OwnerRef);
			int32 DirectionIndex = 0;
			auto ConvertSamples = [&](const TArray<FRigSimulationSpatialVectorSample>& Samples,
				TArray<FUeAgentRigSimulationSpatialPhysicsProbeVector>& Output, const FString& Path) -> bool
			{
				for (int32 Index = 0; Index < Samples.Num(); ++Index)
				{
					const auto& Sample = Samples[Index];
					const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
					const auto* Binding = Environment.FindPhysicsSubjectBinding(Sample.SubjectRef);
					const FUeAgentSpatialValue* Direction = Directions[DirectionIndex++];
					if (!Binding) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_binding_unavailable"),
						TEXT("The vector sample has no physics subject binding."), ItemPath + TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(Sample.SubjectRef), MakeShared<FJsonValueString>(TEXT("a registered physics subject reference")), Sample.SubjectRef);
					if (Direction->GetSubject() != Sample.SubjectRef)
						return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_vector_subject_mismatch"),
							TEXT("The formal direction subject does not match its vector sample."), ItemPath + TEXT("/subject_ref"),
							MakeShared<FJsonValueString>(Direction->GetSubject()), MakeShared<FJsonValueString>(Sample.SubjectRef), Sample.SubjectRef);
					FUeAgentSpatialValuePtr NativeDirection;
					if (!Environment.TryConvertSpatialValueToFrame(*Direction, Binding->NativeSimulationFrameId, Resources, NativeDirection, OutError)) return false;
					if (!NativeDirection.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_vector_missing"),
						TEXT("Spatial conversion returned no native direction."), ItemPath + TEXT("/components"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a converted native direction")), Sample.SubjectRef);
					TOptional<FUeAgentTemporalPoint> NativeTime;
					if (!Environment.TryConvertTemporalPointToClock(Sample.Time.ToTemporalPoint(), Binding->NativeSimulationClockId, Resources, NativeTime, OutError)) return false;
					if (!NativeTime.IsSet()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_probe_time_missing"),
						TEXT("Temporal conversion returned no native sample time."), ItemPath + TEXT("/time"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a converted native sample time")), Sample.SubjectRef);
					FVector Vector;
					if (!TryVector(*NativeDirection, Vector, OutError)) return NativeFailureAt(OutError, OutError, ItemPath, Sample.SubjectRef);
					Output.Add({Sample.SubjectRef, Binding->Component.Get(), NativeTime.GetValue(), Binding->NativeSimulationFrameId, Vector});
				}
				return true;
			};
			return ConvertSamples(Input.Impulses, OutRequest.Impulses, TEXT("/impulses"))
				&& ConvertSamples(Input.Velocities, OutRequest.Velocities, TEXT("/velocities"))
				&& OutRequest.TryValidate(OutError);
		}

		bool TryStringValue(
			const FString& Text,
			const FString& Path,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (FUeAgentCanonicalValue::TryString(Text, Context.GetResourceTracker(), OutValue, Error)) return true;
			OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_physics_string_failed"), Error, TEXT("native_execution"));
			NativeContext(OutError, Path, true);
			return false;
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
			if (!TryStringValue(PublicRef, TEXT("/public_ref"), Context, RefValue, OutError)
				|| !TryStringValue(Contract.GetCapabilityId(), TEXT("/capability_id"), Context, CapabilityValue, OutError)
				|| !TryStringValue(State, TEXT("/observed_state"), Context, StateValue, OutError)) return false;
			if (!FUeAgentCanonicalValue::TryObject({
					{TEXT("public_ref"), RefValue.ToSharedRef()},
					{TEXT("capability_id"), CapabilityValue.ToSharedRef()},
					{TEXT("observed_state"), StateValue.ToSharedRef()}},
				Context.GetResourceTracker(), ObjectValue, Error))
			{
				OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_physics_object_failed"), Error, TEXT("native_execution"));
				NativeContext(OutError);
				return false;
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
					TEXT("independent Physics native observation"),
					{TEXT("RigSimulationSpatial"), TEXT("Physics")}, false,
					TEXT("available"), {}), Revision);
			if (FUeAgentCanonicalSnapshot::TryCreate(
				FString::Printf(TEXT("rig-simulation-spatial:%s:%d:%s"),
					*Contract.GetCapabilityId(), static_cast<int32>(Phase),
					*Context.GetRequestId()),
				Context.GetExecutionContext().GetStatePlane(), Revision,
				FUeAgentTemporalPoint(TEXT("editor-native"),
					FString::Printf(TEXT("%llu"), FPlatformTime::Cycles64())),
				FUeAgentStateProvenance(TEXT("independent_readback"),
					Contract.GetReadbackId(),
					TEXT("native-state:") + Contract.GetCapabilityId()),
				{Entry}, Context.GetResourceTracker(), OutSnapshot, Error)) return true;
			OutError = FDiagnostic::WrapFailure(TEXT("rig_simulation_spatial_physics_snapshot_failed"), Error, TEXT("native_execution"));
			NativeContext(OutError);
			return false;
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Snapshot.GetEntries()[0].GetValue()->GetObjectItems())
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			return FString();
		}

		TSharedRef<FJsonValue> NumberFact(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value));
		}

		TSharedRef<FJsonValue> VectorFact(const FVector& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z)});
		}

		TSharedRef<FJsonValue> RotationFact(const FQuat& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				NumberFact(Value.X), NumberFact(Value.Y), NumberFact(Value.Z), NumberFact(Value.W)});
		}

		void AppendDifference(FDiagnostic& OutDifference, FDiagnostic Difference)
		{
			if (OutDifference.IsEmpty()) OutDifference = MoveTemp(Difference);
			else OutDifference.Diagnostics.Add(MoveTemp(Difference));
		}

		bool MatchFact(bool bMatches, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Reason, const FString& Target, FDiagnostic& OutDifference)
		{
			if (bMatches) return true;
			FDiagnostic Difference;
			ProbeFailure(Difference, TEXT("rig_simulation_spatial_physics_state_mismatch"), Reason, Path,
				MoveTemp(Actual), MoveTemp(Expected), Target);
			Difference.Stage = TEXT("verification");
			AppendDifference(OutDifference, MoveTemp(Difference));
			return false;
		}

		bool ExpectationFailure(FDiagnostic Error, const FString& Path, const FString& Target, FDiagnostic& OutDifference)
		{
			if (Error.IsEmpty()) return MatchFact(false, Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				TEXT("Native expected-state evaluation failed without reporting a diagnostic."), Target, OutDifference);
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			AppendDifference(OutDifference, MoveTemp(Error));
			return false;
		}

		bool MatchTransform(const FTransform& Actual, const FTransform& Expected,
			const FString& Path, const FString& Target, FDiagnostic& OutDifference)
		{
			// Same order, tolerance and quaternion sign equivalence as FTransform::Equals.
			return MatchFact(Actual.TranslationEquals(Expected, 0.001), Path + TEXT("/translation"),
				VectorFact(Actual.GetTranslation()), VectorFact(Expected.GetTranslation()),
				TEXT("The observed translation differs from the requested native translation beyond tolerance 0.001."), Target, OutDifference)
				&& MatchFact(Actual.RotationEquals(Expected, 0.001), Path + TEXT("/rotation"),
					RotationFact(Actual.GetRotation()), RotationFact(Expected.GetRotation()),
					TEXT("The observed quaternion differs from both signs of the requested native quaternion beyond tolerance 0.001."), Target, OutDifference)
				&& MatchFact(Actual.Scale3DEquals(Expected, 0.001), Path + TEXT("/scale"),
					VectorFact(Actual.GetScale3D()), VectorFact(Expected.GetScale3D()),
					TEXT("The observed scale differs from the requested native scale beyond tolerance 0.001."), Target, OutDifference);
		}

		FString CombineModeName(EFrictionCombineMode::Type Value)
		{
			switch (Value)
			{
			case EFrictionCombineMode::Average: return TEXT("average");
			case EFrictionCombineMode::Min: return TEXT("min");
			case EFrictionCombineMode::Multiply: return TEXT("multiply");
			case EFrictionCombineMode::Max: return TEXT("max");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Value));
			}
		}

		FString CollisionName(ECollisionEnabled::Type Value)
		{
			switch (Value)
			{
			case ECollisionEnabled::NoCollision: return TEXT("no_collision");
			case ECollisionEnabled::QueryOnly: return TEXT("query_only");
			case ECollisionEnabled::PhysicsOnly: return TEXT("physics_only");
			case ECollisionEnabled::QueryAndPhysics: return TEXT("query_and_physics");
			case ECollisionEnabled::ProbeOnly: return TEXT("probe_only");
			case ECollisionEnabled::QueryAndProbe: return TEXT("query_and_probe");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Value));
			}
		}

		FString PhysicsTypeName(EPhysicsType Value)
		{
			switch (Value)
			{
			case PhysType_Default: return TEXT("default");
			case PhysType_Kinematic: return TEXT("kinematic");
			case PhysType_Simulated: return TEXT("simulated");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Value));
			}
		}

		FString LinearMotionName(ELinearConstraintMotion Value)
		{
			switch (Value)
			{
			case LCM_Free: return TEXT("free");
			case LCM_Limited: return TEXT("limited");
			case LCM_Locked: return TEXT("locked");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Value));
			}
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			IUeAgentRigSimulationSpatialPhysicsSessionPort* SessionPort,
			const FUeAgentOperationContext& Context,
			FDiagnostic& OutDifference,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			OutDifference.Reset();
			if (Contract.GetCapabilityId() == TEXT("physics.material.update"))
			{
				const auto& Input = Request.GetPayload().Get<FPhysicsMaterialUpdateInput>();
				const UPhysicalMaterial* Material = Cast<UPhysicalMaterial>(
					Environment.ResolveExplicitObject(Input.PhysicalMaterialRef));
				if (!Material)
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_physics_material_unavailable"), TEXT("The physical material reference does not resolve to an available physical material."), TEXT("/physical_material_ref"), Input.PhysicalMaterialRef);
					return FString();
				}
				UPhysicalMaterial* Probe = DuplicateObject<UPhysicalMaterial>(
					Material, GetTransientPackage());
				FDiagnostic ExpectedError;
				bool bMatches = MatchFact(Probe != nullptr, TEXT("/material_state/preview_clone"),
					MakeShared<FJsonValueBoolean>(Probe != nullptr), MakeShared<FJsonValueBoolean>(true),
					TEXT("The expected material state could not be cloned for comparison."), Input.PhysicalMaterialRef, OutDifference);
				if (bMatches && !ApplyMaterialState(*Probe, Input.MaterialState, ExpectedError))
				{
					InputFailureAt(ExpectedError, TEXT("/material_state"), Input.PhysicalMaterialRef);
					bMatches = ExpectationFailure(MoveTemp(ExpectedError), TEXT("/material_state"), Input.PhysicalMaterialRef, OutDifference);
				}
				if (bMatches)
				{
					bMatches = MatchFact(FMath::IsNearlyEqual(Probe->Friction, Material->Friction), TEXT("/material_state/friction"), NumberFact(Material->Friction), NumberFact(Probe->Friction),
						TEXT("The observed physical material value differs from the requested value."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(FMath::IsNearlyEqual(Probe->StaticFriction, Material->StaticFriction), TEXT("/material_state/static_friction"), NumberFact(Material->StaticFriction), NumberFact(Probe->StaticFriction),
						TEXT("The observed physical material value differs from the requested value."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(FMath::IsNearlyEqual(Probe->Restitution, Material->Restitution), TEXT("/material_state/restitution"), NumberFact(Material->Restitution), NumberFact(Probe->Restitution),
						TEXT("The observed physical material value differs from the requested value."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(FMath::IsNearlyEqual(Probe->Density, Material->Density), TEXT("/material_state/density"), NumberFact(Material->Density), NumberFact(Probe->Density),
						TEXT("The observed physical material value differs from the requested value."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(FMath::IsNearlyEqual(Probe->RaiseMassToPower, Material->RaiseMassToPower), TEXT("/material_state/raise_mass_to_power"), NumberFact(Material->RaiseMassToPower), NumberFact(Probe->RaiseMassToPower),
						TEXT("The observed physical material value differs from the requested value."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(Probe->FrictionCombineMode == Material->FrictionCombineMode, TEXT("/material_state/friction_combine_mode"), MakeShared<FJsonValueString>(CombineModeName(Material->FrictionCombineMode)), MakeShared<FJsonValueString>(CombineModeName(Probe->FrictionCombineMode)),
						TEXT("The observed physical material combine mode differs from the requested mode."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(Probe->RestitutionCombineMode == Material->RestitutionCombineMode, TEXT("/material_state/restitution_combine_mode"), MakeShared<FJsonValueString>(CombineModeName(Material->RestitutionCombineMode)), MakeShared<FJsonValueString>(CombineModeName(Probe->RestitutionCombineMode)),
						TEXT("The observed physical material combine mode differs from the requested mode."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(Probe->bOverrideFrictionCombineMode == Material->bOverrideFrictionCombineMode, TEXT("/material_state/override_friction_combine_mode"), MakeShared<FJsonValueBoolean>(static_cast<bool>(Material->bOverrideFrictionCombineMode)), MakeShared<FJsonValueBoolean>(static_cast<bool>(Probe->bOverrideFrictionCombineMode)),
						TEXT("The observed physical material override setting differs from the requested setting."), Input.PhysicalMaterialRef, OutDifference)
						&& MatchFact(Probe->bOverrideRestitutionCombineMode == Material->bOverrideRestitutionCombineMode, TEXT("/material_state/override_restitution_combine_mode"), MakeShared<FJsonValueBoolean>(static_cast<bool>(Material->bOverrideRestitutionCombineMode)), MakeShared<FJsonValueBoolean>(static_cast<bool>(Probe->bOverrideRestitutionCombineMode)),
						TEXT("The observed physical material override setting differs from the requested setting."), Input.PhysicalMaterialRef, OutDifference);
				}
				return FString::Printf(TEXT("material=%s|friction=%g|matches=%d"),
					*Input.PhysicalMaterialRef, Material->Friction, bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("physics.asset.structure.replace"))
			{
				const auto& Input = Request.GetPayload().Get<FPhysicsAssetStructureReplaceInput>();
				const auto* Binding = Environment.FindPhysicsAssetBinding(Input.PhysicsAssetRef);
				UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
				if (!Binding || !Asset)
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
						Binding ? TEXT("The registered physics asset is unavailable.") : TEXT("The physics asset reference has no registered binding."),
						TEXT("/physics_asset_ref"), Input.PhysicsAssetRef);
					return FString();
				}
				bool bMatches = MatchFact(Asset->SkeletalBodySetups.Num() == Input.Bodies.Num(), TEXT("/bodies"),
					NumberFact(Asset->SkeletalBodySetups.Num()), NumberFact(Input.Bodies.Num()),
					TEXT("The native body count differs from the requested body count."), Input.PhysicsAssetRef, OutDifference)
					&& MatchFact(Asset->ConstraintSetup.Num() == Input.Constraints.Num(), TEXT("/constraints"),
						NumberFact(Asset->ConstraintSetup.Num()), NumberFact(Input.Constraints.Num()),
						TEXT("The native constraint count differs from the requested constraint count."), Input.PhysicsAssetRef, OutDifference);
				for (int32 BodyIndex = 0; BodyIndex < Input.Bodies.Num(); ++BodyIndex)
				{
					const auto& BodyInput = Input.Bodies[BodyIndex];
					const FString BodyPath = TEXT("/bodies/") + FString::FromInt(BodyIndex);
					const auto* BodyBinding = FindBodyBinding(*Binding, BodyInput.BodyRef);
					USkeletalBodySetup* Body = BodyBinding ? FindBody(*Asset, BodyBinding->BoneName) : nullptr;
					bMatches &= MatchFact(Body != nullptr, BodyPath + TEXT("/native_body"),
						MakeShared<FJsonValueBoolean>(Body != nullptr), MakeShared<FJsonValueBoolean>(true),
						TEXT("The requested body has no available native physics body."), BodyInput.BodyRef, OutDifference)
						&& MatchFact(BodyInput.ShapeRefs.Num() == Body->AggGeom.GetElementCount(), BodyPath + TEXT("/shape_refs"),
							NumberFact(Body->AggGeom.GetElementCount()), NumberFact(BodyInput.ShapeRefs.Num()),
							TEXT("The native shape count differs from the requested shape count."), BodyInput.BodyRef, OutDifference);
					if (Body)
					{
						ECollisionEnabled::Type Collision;
						EPhysicsType PhysicsType;
						double Mass = 0.0;
						FDiagnostic ExpectedError;
						bool bStateMatches = true;
						if (!TryCollision(BodyInput.State.CollisionEnabled, Collision))
						{
							EnumFailure(ExpectedError, TEXT("/collision_enabled"), BodyInput.State.CollisionEnabled,
								{TEXT("no_collision"), TEXT("query_only"), TEXT("physics_only"), TEXT("query_and_physics"), TEXT("probe_only"), TEXT("query_and_probe")});
							bStateMatches = false;
						}
						else if (!TryPhysicsType(BodyInput.State.PhysicsType, PhysicsType))
						{
							EnumFailure(ExpectedError, TEXT("/physics_type"), BodyInput.State.PhysicsType,
								{TEXT("default"), TEXT("kinematic"), TEXT("simulated")});
							bStateMatches = false;
						}
						else if (!TryNumber(BodyInput.State.MassKg, UE_SMALL_NUMBER, MAX_flt, Mass, ExpectedError, TEXT("/mass_kg")))
							bStateMatches = false;
						if (!bStateMatches)
						{
							InputFailureAt(ExpectedError, BodyPath + TEXT("/state"), BodyInput.BodyRef);
							ExpectationFailure(MoveTemp(ExpectedError), BodyPath + TEXT("/state"), BodyInput.BodyRef, OutDifference);
						}
						else
						{
							bStateMatches = MatchFact(Body->DefaultInstance.GetCollisionEnabled(false) == Collision, BodyPath + TEXT("/state/collision_enabled"),
								MakeShared<FJsonValueString>(CollisionName(Body->DefaultInstance.GetCollisionEnabled(false))), MakeShared<FJsonValueString>(CollisionName(Collision)),
								TEXT("The observed collision mode differs from the requested mode."), BodyInput.BodyRef, OutDifference)
								&& MatchFact(Body->PhysicsType == PhysicsType, BodyPath + TEXT("/state/physics_type"),
									MakeShared<FJsonValueString>(PhysicsTypeName(Body->PhysicsType)), MakeShared<FJsonValueString>(PhysicsTypeName(PhysicsType)),
									TEXT("The observed physics type differs from the requested type."), BodyInput.BodyRef, OutDifference)
								&& MatchFact(FMath::IsNearlyEqual(Body->DefaultInstance.GetMassOverride(), static_cast<float>(Mass)), BodyPath + TEXT("/state/mass_kg"),
									NumberFact(Body->DefaultInstance.GetMassOverride()), NumberFact(static_cast<float>(Mass)),
									TEXT("The observed mass differs from the requested mass."), BodyInput.BodyRef, OutDifference)
								&& (!BodyInput.State.ConsiderForBounds.IsSet()
									|| MatchFact(Body->bConsiderForBounds == BodyInput.State.ConsiderForBounds.GetValue(), BodyPath + TEXT("/state/consider_for_bounds"),
										MakeShared<FJsonValueBoolean>(static_cast<bool>(Body->bConsiderForBounds)), MakeShared<FJsonValueBoolean>(BodyInput.State.ConsiderForBounds.GetValue()),
										TEXT("The observed bounds setting differs from the requested setting."), BodyInput.BodyRef, OutDifference));
						}
						bMatches &= bStateMatches;
					}
				}
				for (int32 ConstraintIndex = 0; ConstraintIndex < Input.Constraints.Num(); ++ConstraintIndex)
				{
					const auto& ConstraintInput = Input.Constraints[ConstraintIndex];
					const FString ConstraintPath = TEXT("/constraints/") + FString::FromInt(ConstraintIndex);
					const auto* ConstraintBinding = FindConstraintBinding(*Binding, ConstraintInput.ConstraintRef);
					UPhysicsConstraintTemplate* Constraint = ConstraintBinding ? FindConstraint(*Asset, ConstraintBinding->NativeName) : nullptr;
					FTransform ExpectedA;
					FTransform ExpectedB;
					FDiagnostic ExpectedError;
					bool bConstraintMatches = MatchFact(Constraint != nullptr, ConstraintPath + TEXT("/native_constraint"),
						MakeShared<FJsonValueBoolean>(Constraint != nullptr), MakeShared<FJsonValueBoolean>(true),
						TEXT("The requested constraint has no available native physics constraint."), ConstraintInput.ConstraintRef, OutDifference);
					if (bConstraintMatches && (!TryNativeTransform(Request, ConstraintInput.EndpointA.BodyRef, ConstraintBinding->NativeFrameAId,
						Environment, Context.GetResourceTracker(), ExpectedA, ExpectedError)
						|| !TryNativeTransform(Request, ConstraintInput.EndpointB.BodyRef, ConstraintBinding->NativeFrameBId,
							Environment, Context.GetResourceTracker(), ExpectedB, ExpectedError)))
						bConstraintMatches = ExpectationFailure(MoveTemp(ExpectedError), ConstraintPath, ConstraintInput.ConstraintRef, OutDifference);
					if (bConstraintMatches)
						bConstraintMatches = MatchTransform(Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1), ExpectedA,
							ConstraintPath + TEXT("/endpoint_a/frame"), ConstraintInput.ConstraintRef, OutDifference)
							&& MatchTransform(Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2), ExpectedB,
								ConstraintPath + TEXT("/endpoint_b/frame"), ConstraintInput.ConstraintRef, OutDifference);
					bMatches &= bConstraintMatches;
				}
				for (int32 ProfileIndex = 0; ProfileIndex < Input.Profiles.Num(); ++ProfileIndex)
				{
					const auto& Profile = Input.Profiles[ProfileIndex];
					const FString ProfilePath = TEXT("/profiles/") + FString::FromInt(ProfileIndex);
					const bool bPhysicalProfilePresent = Asset->PhysicalAnimationProfiles.Contains(FName(*Profile.ProfileName));
					bMatches &= MatchFact(bPhysicalProfilePresent, ProfilePath + TEXT("/physical_animation_profile"),
						MakeShared<FJsonValueBoolean>(bPhysicalProfilePresent), MakeShared<FJsonValueBoolean>(true),
						TEXT("The requested physical animation profile is absent."), Profile.ProfileName, OutDifference)
						&& MatchFact(Asset->ConstraintProfiles.Contains(FName(*Profile.ProfileName)), ProfilePath + TEXT("/constraint_profile"),
							MakeShared<FJsonValueBoolean>(Asset->ConstraintProfiles.Contains(FName(*Profile.ProfileName))), MakeShared<FJsonValueBoolean>(true),
							TEXT("The requested constraint profile is absent."), Profile.ProfileName, OutDifference);
				}
				return FString::Printf(TEXT("asset=%s|bodies=%d|constraints=%d|matches=%d"),
					*Input.PhysicsAssetRef, Asset->SkeletalBodySetups.Num(),
					Asset->ConstraintSetup.Num(), bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("physics.constraint.update"))
			{
				const auto& Input = Request.GetPayload().Get<FPhysicsConstraintUpdateInput>();
				const auto* Binding = Environment.FindPhysicsAssetBinding(Input.PhysicsAssetRef);
				UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
				const auto* ConstraintBinding = Binding
					? FindConstraintBinding(*Binding, Input.ConstraintRef) : nullptr;
				UPhysicsConstraintTemplate* Constraint = Asset && ConstraintBinding
					? FindConstraint(*Asset, ConstraintBinding->NativeName) : nullptr;
				if (!ConstraintBinding || !Constraint)
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_physics_constraint_unavailable"),
						!Binding ? TEXT("The physics asset reference has no registered binding.") : !Asset ? TEXT("The registered physics asset is unavailable.")
							: !ConstraintBinding ? TEXT("The constraint reference has no registered binding.") : TEXT("The registered native constraint is unavailable."),
						!Binding || !Asset ? TEXT("/physics_asset_ref") : TEXT("/constraint_ref"), !Binding || !Asset ? Input.PhysicsAssetRef : Input.ConstraintRef);
					return FString();
				}
				UPhysicsAsset* ProbeAsset = DuplicateObject<UPhysicsAsset>(
					Asset, GetTransientPackage());
				FDiagnostic ExpectedError;
				bool bMatches = MatchFact(ProbeAsset != nullptr, TEXT("/preview_clone"),
					MakeShared<FJsonValueBoolean>(ProbeAsset != nullptr), MakeShared<FJsonValueBoolean>(true),
					TEXT("The expected physics asset state could not be cloned for comparison."), Input.PhysicsAssetRef, OutDifference);
				if (bMatches && !ApplyConstraintUpdate(*ProbeAsset, *Binding, Input, Request, Environment, Context.GetResourceTracker(), ExpectedError))
					bMatches = ExpectationFailure(MoveTemp(ExpectedError), TEXT("/constraint_state"), Input.ConstraintRef, OutDifference);
				if (bMatches)
				{
					UPhysicsConstraintTemplate* Desired = FindConstraint(*ProbeAsset, ConstraintBinding->NativeName);
					FTransform ExpectedA;
					FTransform ExpectedB;
					bMatches = MatchFact(Desired != nullptr, TEXT("/expected_constraint"),
						MakeShared<FJsonValueBoolean>(Desired != nullptr), MakeShared<FJsonValueBoolean>(true),
						TEXT("The expected native constraint is unavailable after applying the requested update."), Input.ConstraintRef, OutDifference);
					if (bMatches && (!TryNativeTransform(Request, Input.EndpointA.BodyRef, ConstraintBinding->NativeFrameAId,
						Environment, Context.GetResourceTracker(), ExpectedA, ExpectedError)
						|| !TryNativeTransform(Request, Input.EndpointB.BodyRef, ConstraintBinding->NativeFrameBId,
							Environment, Context.GetResourceTracker(), ExpectedB, ExpectedError)))
						bMatches = ExpectationFailure(MoveTemp(ExpectedError), TEXT("/constraint_state"), Input.ConstraintRef, OutDifference);
					if (bMatches)
					{
						bMatches = MatchTransform(Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1), ExpectedA,
							TEXT("/endpoint_a/frame"), Input.ConstraintRef, OutDifference)
							&& MatchTransform(Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2), ExpectedB,
								TEXT("/endpoint_b/frame"), Input.ConstraintRef, OutDifference)
							&& MatchFact(Constraint->DefaultInstance.GetLinearXMotion() == Desired->DefaultInstance.GetLinearXMotion(), TEXT("/constraint_state/linear_x_motion"),
								MakeShared<FJsonValueString>(LinearMotionName(Constraint->DefaultInstance.GetLinearXMotion())),
								MakeShared<FJsonValueString>(LinearMotionName(Desired->DefaultInstance.GetLinearXMotion())),
								TEXT("The observed linear motion differs from the requested motion."), Input.ConstraintRef, OutDifference)
							&& MatchFact(FMath::IsNearlyEqual(Constraint->DefaultInstance.GetLinearLimit(), Desired->DefaultInstance.GetLinearLimit()), TEXT("/constraint_state/linear_limit_cm"),
								NumberFact(Constraint->DefaultInstance.GetLinearLimit()), NumberFact(Desired->DefaultInstance.GetLinearLimit()),
								TEXT("The observed linear limit differs from the requested limit."), Input.ConstraintRef, OutDifference)
							&& MatchFact(FMath::IsNearlyEqual(Constraint->DefaultInstance.GetAngularTwistLimit(), Desired->DefaultInstance.GetAngularTwistLimit()), TEXT("/constraint_state/twist_limit_degrees"),
								NumberFact(Constraint->DefaultInstance.GetAngularTwistLimit()), NumberFact(Desired->DefaultInstance.GetAngularTwistLimit()),
								TEXT("The observed twist limit differs from the requested limit."), Input.ConstraintRef, OutDifference)
							&& MatchFact(Constraint->DefaultInstance.IsCollisionDisabled() == Desired->DefaultInstance.IsCollisionDisabled(), TEXT("/constraint_state/disable_collision"),
								MakeShared<FJsonValueBoolean>(Constraint->DefaultInstance.IsCollisionDisabled()), MakeShared<FJsonValueBoolean>(Desired->DefaultInstance.IsCollisionDisabled()),
								TEXT("The observed collision-disable setting differs from the requested setting."), Input.ConstraintRef, OutDifference);
					}
				}
				return FString::Printf(TEXT("constraint=%s|matches=%d"),
					*Input.ConstraintRef, bMatches ? 1 : 0);
			}
			const auto& Input = Request.GetPayload().Get<FPhysicsSimulationProbeInput>();
			if (!SessionPort)
			{
				ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_port_unavailable"),
					TEXT("The native environment has no physics session port."), TEXT("/session_port"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.SimulationSession.OwnerRef);
				return FString();
			}
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult Result;
			if (!SessionPort->TryReadPhysicsProbe(Input.SimulationSession, Input.SubjectRefs, Context, Result, OutError)
				|| !Result.TryValidate(OutError)) return FString();
			const bool bMatches = MatchFact(Result.bCompleted, TEXT("/completed"),
				MakeShared<FJsonValueBoolean>(Result.bCompleted), MakeShared<FJsonValueBoolean>(true),
				TEXT("The observed physics probe session is not completed."), Input.SimulationSession.OwnerRef, OutDifference)
				&& MatchFact(Result.bRestored, TEXT("/restored"),
					MakeShared<FJsonValueBoolean>(Result.bRestored), MakeShared<FJsonValueBoolean>(true),
					TEXT("The observed physics probe session is not restored."), Input.SimulationSession.OwnerRef, OutDifference)
				&& MatchFact(!Result.Samples.IsEmpty(), TEXT("/samples"), NumberFact(Result.Samples.Num()),
					MakeShared<FJsonValueString>(TEXT("one or more samples")),
					TEXT("The observed physics probe session contains no samples."), Input.SimulationSession.OwnerRef, OutDifference);
			return FString::Printf(
				TEXT("session=%s|completed=%d|restored=%d|samples=%d|restoration=%s|matches=%d"),
				*Result.Session.SessionId, Result.bCompleted ? 1 : 0,
				Result.bRestored ? 1 : 0, Result.Samples.Num(),
				*Result.RestorationEvidenceId, bMatches ? 1 : 0);
		}

		class FReadback final : public IUeAgentRigSimulationSpatialReadbackPort
		{
		public:
			FReadback(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment,
				TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> InSessionPort)
				: Environment(MoveTemp(InEnvironment)), SessionPort(MoveTemp(InSessionPort)) {}

			bool TryRead(
				const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
				const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentRigSimulationSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutReadback.Reset();
				TSharedPtr<const FUeAgentCanonicalSnapshot> OutSnapshot;
				FDiagnostic Difference;
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return CapabilityFailure(OutError, Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty() || (Target.IsSet() && Target->GetPublicRef() != PublicRef))
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						PublicRef.IsEmpty() ? TEXT("The typed request has no primary target reference.")
							: TEXT("The readback target differs from the request target."), TEXT("/target_ref"),
						MakeShared<FJsonValueString>(Target.IsSet() ? Target->GetPublicRef() : PublicRef),
						MakeShared<FJsonValueString>(PublicRef.IsEmpty() ? TEXT("a nonempty target reference") : PublicRef), PublicRef);
				const FString State = ReadState(Contract, *Typed, *Environment,
					SessionPort.Get(), Context, Difference, OutError);
				if (State.IsEmpty() || !TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError)) return false;
				OutReadback.Emplace(OutSnapshot.ToSharedRef(), MoveTemp(Difference));
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
			TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> SessionPort;
		};

		class FMutation final : public IUeAgentRigSimulationSpatialMutationPort
		{
		public:
			FMutation(
				TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> InEnvironment,
				TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> InSessionPort)
				: Environment(MoveTemp(InEnvironment)), SessionPort(MoveTemp(InSessionPort)) {}

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
				if (!IsCapability(Contract.GetCapabilityId())) return CapabilityFailure(OutError, Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet() || Target->GetPublicRef() != Typed->GetPrimaryTargetRef())
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_explicit_target_mismatch"),
						Target.IsSet() ? TEXT("The mutation target differs from the request target.") : TEXT("The mutation has no target binding."),
						TEXT("/target_ref"), Target.IsSet() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Target->GetPublicRef())) : MakeShared<FJsonValueNull>(),
						MakeShared<FJsonValueString>(Typed->GetPrimaryTargetRef()), Typed->GetPrimaryTargetRef());

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId() == TEXT("physics.material.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsMaterialUpdateInput>();
					UPhysicalMaterial* Material = Cast<UPhysicalMaterial>(
						Environment->ResolveExplicitObject(Input.PhysicalMaterialRef));
					if (!Material) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_material_unavailable"),
						TEXT("The physical material reference does not resolve to an available physical material."), TEXT("/physical_material_ref"), Input.PhysicalMaterialRef);
					if (!CheckRevision(*Environment, Input.PhysicalMaterialRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicalMaterial* Probe = DuplicateObject<UPhysicalMaterial>(
						Material, GetTransientPackage());
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_preview_clone_failed"),
						TEXT("Cloning the physical material for preflight failed."), TEXT("/material_state/preview_clone"), Input.PhysicalMaterialRef);
					if (!ApplyMaterialState(*Probe, Input.MaterialState, OutError)) return InputFailureAt(OutError, TEXT("/material_state"), Input.PhysicalMaterialRef);
					Material->Modify();
					if (!ApplyMaterialState(*Material, Input.MaterialState, OutError)) return InputFailureAt(OutError, TEXT("/material_state"), Input.PhysicalMaterialRef);
					Material->MarkPackageDirty();
					MutatedObject = Material;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("physics.asset.structure.replace"))
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsAssetStructureReplaceInput>();
					const auto* Binding = Environment->FindPhysicsAssetBinding(Input.PhysicsAssetRef);
					UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
					if (!Binding) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
						TEXT("The physics asset reference has no registered binding."), TEXT("/physics_asset_ref"), Input.PhysicsAssetRef);
					if (!Asset) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
						TEXT("The registered physics asset is unavailable."), TEXT("/physics_asset_ref"), Input.PhysicsAssetRef);
					if (Environment->ResolveExplicitObject(Input.PhysicsAssetRef) != Asset)
						return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
							TEXT("The physics asset reference resolves to a different object than its binding."), TEXT("/physics_asset_ref"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.PhysicsAssetRef);
					if (!CheckRevision(*Environment, Input.PhysicsAssetRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicsAsset* Probe = DuplicateObject<UPhysicsAsset>(
						Asset, GetTransientPackage());
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_preview_clone_failed"),
						TEXT("Cloning the physics asset for preflight failed."), TEXT("/preview_clone"), Input.PhysicsAssetRef);
					if (!ApplyAssetStructure(*Probe, *Binding, Input, *Typed, *Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->Modify();
					if (!ApplyAssetStructure(*Asset, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->MarkPackageDirty();
					MutatedObject = Asset;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("physics.constraint.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsConstraintUpdateInput>();
					const auto* Binding = Environment->FindPhysicsAssetBinding(Input.PhysicsAssetRef);
					UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
					if (!Binding || !Asset) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
						Binding ? TEXT("The registered physics asset is unavailable.") : TEXT("The physics asset reference has no registered binding."),
						TEXT("/physics_asset_ref"), Input.PhysicsAssetRef);
					if (!CheckRevision(*Environment, Input.PhysicsAssetRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicsAsset* Probe = DuplicateObject<UPhysicsAsset>(
						Asset, GetTransientPackage());
					if (!Probe) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_preview_clone_failed"),
						TEXT("Cloning the physics asset for preflight failed."), TEXT("/preview_clone"), Input.PhysicsAssetRef);
					if (!ApplyConstraintUpdate(*Probe, *Binding, Input, *Typed, *Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->Modify();
					if (!ApplyConstraintUpdate(*Asset, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->MarkPackageDirty();
					MutatedObject = Asset;
				}
				else
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsSimulationProbeInput>();
					if (!SessionPort.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_port_unavailable"),
						TEXT("The native environment has no physics session port."), TEXT("/session_port"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.SimulationSession.OwnerRef);
					FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest SessionRequest;
					if (!BuildProbeRequest(Input, *Typed, *Environment,
						Context.GetResourceTracker(), SessionRequest, OutError)) return false;
					FUeAgentRigSimulationSpatialPhysicsProbeSessionResult SessionResult;
					if (!SessionPort->TryRunPhysicsProbe(SessionRequest, Context, SessionResult, OutError)
						|| !SessionResult.TryValidate(OutError)) return false;
					if (!SessionResult.bCompleted) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_incomplete"),
						TEXT("The physics session returned without completing the probe."), TEXT("/completed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input.SimulationSession.OwnerRef);
					MutatedObject = SessionResult.OwnerObject.Get();
				}
				if (!IsValid(MutatedObject)) return MissingNative(OutError, TEXT("rig_simulation_spatial_physics_mutation_target_unavailable"),
					TEXT("The native mutation did not return an available target object."), TEXT("/mutation_target"), Typed->GetPrimaryTargetRef());
				const FString OperationId = Context.GetRequestId() + TEXT(":")
					+ Contract.GetCapabilityId();
				Environment->RecordMutation(OperationId, MutatedObject);
				OutResult = FUeAgentRigSimulationSpatialMutationResult(OperationId,
					TEXT("native-mutation:") + MutatedObject->GetPathName());
				// Both constructor inputs have nonempty fixed prefixes.
				return true;
			}

		private:
			TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment;
			TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> SessionPort;
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
				if (!IsCapability(Contract.GetCapabilityId())) return CapabilityFailure(OutError, Contract.GetCapabilityId());
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object))
				{
					MissingNative(OutError, TEXT("rig_simulation_spatial_settle_target_unavailable"),
						TEXT("The recorded mutation target is no longer available for settling."), TEXT("/mutation_target"), FString());
					OutError.SubjectId = Mutation.GetOperationId();
					return false;
				}
				if (UPhysicsAsset* Asset = Cast<UPhysicsAsset>(Object))
				{
					RefreshPhysicsAsset(*Asset);
					Asset->PostEditChange();
				}
				else if (UPhysicalMaterial* Material = Cast<UPhysicalMaterial>(Object))
				{
					Material->PostEditChange();
				}
				return true;
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
				const FUeAgentRigSimulationSpatialReadbackResult& AfterReadback,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				const auto& After = AfterReadback.Snapshot.Get();
				OutError = {};
				if (!IsCapability(Contract.GetCapabilityId())) return CapabilityFailure(OutError, Contract.GetCapabilityId());
				if (!ProductionRequest(Request, OutError)) return false;
				const FString BeforeState = SnapshotState(Before);
				const FString AfterState = SnapshotState(After);
				FDiagnostic Difference;
				bool bMatches = MatchFact(!BeforeState.IsEmpty(), TEXT("/before/observed_state"),
					MakeShared<FJsonValueBoolean>(!BeforeState.IsEmpty()), MakeShared<FJsonValueBoolean>(true),
					TEXT("The preflight snapshot has no observed physics state."), FString(), Difference)
					&& MatchFact(!AfterState.IsEmpty(), TEXT("/after/observed_state"),
						MakeShared<FJsonValueBoolean>(!AfterState.IsEmpty()), MakeShared<FJsonValueBoolean>(true),
						TEXT("The verification snapshot has no observed physics state."), FString(), Difference);
				if (!AfterReadback.Difference.IsEmpty())
				{
					AppendDifference(Difference, AfterReadback.Difference);
					bMatches = false;
				}
				if (Contract.GetCapabilityId() == TEXT("physics.simulation.probe"))
				{
					const bool bChanged = BeforeState != AfterState;
					if (!bChanged)
					{
						FDiagnostic ObservationDifference;
						MatchFact(false, TEXT("/observation_changed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
							TEXT("The physics probe returned the same native observation as before execution."), FString(), ObservationDifference);
						ObservationDifference.ActualEvidence = MakeShared<FJsonValueString>(AfterState);
						ObservationDifference.ExpectedEvidence = MakeShared<FJsonValueString>(BeforeState);
						AppendDifference(Difference, MoveTemp(ObservationDifference));
					}
					bMatches &= bChanged;
				}
				OutOutcome = FVerificationOutcome(TEXT("rig-simulation-spatial-verify:") + Contract.GetCapabilityId(),
				bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
				bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
				MoveTemp(Difference),
				false,
				{},
				{TEXT("independent-state:") + After.GetSnapshotId()},
				{});
				return true;
			}
		};
	}

	namespace
	{
		bool PhysicsBindingFailure(Contract::FDiagnostic& OutError, const TCHAR* Code,
			const TCHAR* Reason, const FString& Path, const FString& Identity,
			TSharedPtr<FJsonValue> Actual, const TCHAR* Expected)
		{
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_binding"));
			OutError.PropertyPath = Path;
			OutError.TargetPath = Identity;
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		bool RequirePhysicsText(Contract::FDiagnostic& OutError, const TCHAR* Code,
			const TCHAR* Reason, const TCHAR* Path, const FString& Identity, const FString& Value)
		{
			return !Value.IsEmpty() || PhysicsBindingFailure(OutError, Code, Reason, Path, Identity,
				MakeShared<FJsonValueString>(Value), TEXT("non-empty reference"));
		}

		TSharedPtr<FJsonValue> PhysicsNumericValue(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FMath::IsNaN(Value) ? TEXT("NaN") : Value < 0.0 ? TEXT("-Infinity") : TEXT("Infinity"));
		}

		void PrefixPhysicsBinding(Contract::FDiagnostic& Error, const TCHAR* Collection, int32 Index)
		{
			const FString Path = FString::Printf(TEXT("%s[%d]"), Collection, Index);
			Error.PropertyPath = Error.PropertyPath.IsEmpty() ? Path : Path + TEXT(".") + Error.PropertyPath;
		}
	}

	bool FUeAgentRigSimulationSpatialPhysicsBodyBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const TCHAR* Code = TEXT("physics_body_binding_invalid");
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics body binding requires a body reference."), TEXT("identity"), Identity, Identity)
			|| !RequirePhysicsText(OutError, Code, TEXT("The physics body binding requires a bone reference."), TEXT("bone_identity"), Identity, BoneIdentity)) return false;
		if (BoneName.IsNone()) return PhysicsBindingFailure(OutError, Code, TEXT("The physics body bone name must not be None."), TEXT("bone_name"), Identity,
			MakeShared<FJsonValueString>(BoneName.ToString()), TEXT("non-None bone name"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsBodyBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsShapeBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const TCHAR* Code = TEXT("physics_shape_binding_invalid");
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics shape requires a shape reference."), TEXT("identity"), Identity, Identity)
			|| !RequirePhysicsText(OutError, Code, TEXT("The physics shape requires a body reference."), TEXT("body_identity"), Identity, BodyIdentity)) return false;
		if (NativeName.IsNone()) return PhysicsBindingFailure(OutError, Code, TEXT("The physics shape name must not be None."), TEXT("native_name"), Identity,
			MakeShared<FJsonValueString>(NativeName.ToString()), TEXT("non-None shape name"));
		if (ShapeKind != TEXT("box") && ShapeKind != TEXT("sphere") && ShapeKind != TEXT("capsule"))
			return PhysicsBindingFailure(OutError, Code, TEXT("The physics shape kind is unsupported."), TEXT("shape_kind"), Identity,
				MakeShared<FJsonValueString>(ShapeKind), TEXT("box, sphere or capsule"));
		const double Values[] = {Dimensions.X, Dimensions.Y, Dimensions.Z};
		const TCHAR* Fields[] = {TEXT("dimensions.x"), TEXT("dimensions.y"), TEXT("dimensions.z")};
		for (int32 Index = 0; Index < 3; ++Index)
			if (!FMath::IsFinite(Values[Index])) return PhysicsBindingFailure(OutError, Code,
				TEXT("The physics shape dimension must be finite."), Fields[Index], Identity, PhysicsNumericValue(Values[Index]), TEXT("finite number"));
		const int32 RequiredPositive = ShapeKind == TEXT("sphere") ? 1 : ShapeKind == TEXT("capsule") ? 2 : 3;
		for (int32 Index = 0; Index < RequiredPositive; ++Index)
			if (Values[Index] <= 0.0) return PhysicsBindingFailure(OutError, Code,
				TEXT("This physics shape dimension must be greater than zero."), Fields[Index], Identity, PhysicsNumericValue(Values[Index]), TEXT("number greater than zero"));
		if (!LocalTransform.IsValid())
		{
			if (LocalTransform.ContainsNaN())
			{
				// Ordinary component getters can repair non-finite values in diagnostic builds.
				const TCHAR* Field = VectorContainsNaNOrInfinite(LocalTransform.GetTranslationRegister()) ? TEXT("local_transform.translation")
					: VectorContainsNaNOrInfinite(LocalTransform.GetRotationRegister()) ? TEXT("local_transform.rotation") : TEXT("local_transform.scale_3d");
				PhysicsBindingFailure(OutError, Code, TEXT("This local transform component contains NaN or infinity."), Field, Identity,
					nullptr, TEXT("finite components"));
				OutError.ActualValuePresent = true;
				return false;
			}
			const FQuat Rotation = LocalTransform.GetRotation();
			TArray<TSharedPtr<FJsonValue>> Actual = {PhysicsNumericValue(Rotation.X), PhysicsNumericValue(Rotation.Y),
				PhysicsNumericValue(Rotation.Z), PhysicsNumericValue(Rotation.W)};
			return PhysicsBindingFailure(OutError, Code, TEXT("The local transform rotation quaternion is not normalized."), TEXT("local_transform.rotation"), Identity,
				MakeShared<FJsonValueArray>(MoveTemp(Actual)), TEXT("normalized quaternion [x, y, z, w]"));
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsShapeBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsConstraintBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const TCHAR* Code = TEXT("physics_constraint_binding_invalid");
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics constraint requires a constraint reference."), TEXT("identity"), Identity, Identity)) return false;
		if (NativeName.IsNone()) return PhysicsBindingFailure(OutError, Code, TEXT("The physics constraint name must not be None."), TEXT("native_name"), Identity,
			MakeShared<FJsonValueString>(NativeName.ToString()), TEXT("non-None constraint name"));
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics constraint requires body A."), TEXT("body_a_identity"), Identity, BodyAIdentity)
			|| !RequirePhysicsText(OutError, Code, TEXT("The physics constraint requires body B."), TEXT("body_b_identity"), Identity, BodyBIdentity)) return false;
		if (BodyAIdentity == BodyBIdentity) return PhysicsBindingFailure(OutError, Code, TEXT("A physics constraint must connect distinct bodies."), TEXT("body_b_identity"), Identity,
			MakeShared<FJsonValueString>(BodyBIdentity), TEXT("body reference different from body A"));
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics constraint requires frame A."), TEXT("native_frame_a_id"), Identity, NativeFrameAId)
			|| !RequirePhysicsText(OutError, Code, TEXT("The physics constraint requires frame B."), TEXT("native_frame_b_id"), Identity, NativeFrameBId)) return false;
		if (NativeFrameAId == NativeFrameBId) return PhysicsBindingFailure(OutError, Code, TEXT("The physics constraint requires distinct native frames."), TEXT("native_frame_b_id"), Identity,
			MakeShared<FJsonValueString>(NativeFrameBId), TEXT("frame reference different from frame A"));
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsConstraintBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsAssetBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const TCHAR* Code = TEXT("physics_asset_binding_invalid");
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics asset binding requires an asset reference."), TEXT("identity"), Identity, Identity)) return false;
		if (!PhysicsAsset.IsValid()) return PhysicsBindingFailure(OutError, Code, TEXT("The bound physics asset is unavailable."), TEXT("physics_asset"), Identity, nullptr, TEXT("valid physics asset"));
		if (BodyBindings.IsEmpty()) return PhysicsBindingFailure(OutError, Code, TEXT("The physics asset binding requires at least one body."), TEXT("body_bindings"), Identity,
			MakeShared<FJsonValueNumber>(0), TEXT("at least one body binding"));
		TSet<FString> Bodies;
		for (int32 Index = 0; Index < BodyBindings.Num(); ++Index)
		{
			const auto& Body = BodyBindings[Index];
			if (!Body.TryValidate(OutError)) { PrefixPhysicsBinding(OutError, TEXT("body_bindings"), Index); return false; }
			if (Bodies.Contains(Body.Identity)) return PhysicsBindingFailure(OutError, Code, TEXT("The body reference is registered more than once."),
				FString::Printf(TEXT("body_bindings[%d].identity"), Index), Identity, MakeShared<FJsonValueString>(Body.Identity), TEXT("unique body reference"));
			Bodies.Add(Body.Identity);
		}
		TSet<FString> Shapes;
		for (int32 Index = 0; Index < ShapeBindings.Num(); ++Index)
		{
			const auto& Shape = ShapeBindings[Index];
			if (!Shape.TryValidate(OutError)) { PrefixPhysicsBinding(OutError, TEXT("shape_bindings"), Index); return false; }
			if (Shapes.Contains(Shape.Identity)) return PhysicsBindingFailure(OutError, Code, TEXT("The shape reference is registered more than once."),
				FString::Printf(TEXT("shape_bindings[%d].identity"), Index), Identity, MakeShared<FJsonValueString>(Shape.Identity), TEXT("unique shape reference"));
			if (!Bodies.Contains(Shape.BodyIdentity)) return PhysicsBindingFailure(OutError, Code, TEXT("The shape refers to an unregistered body."),
				FString::Printf(TEXT("shape_bindings[%d].body_identity"), Index), Identity, MakeShared<FJsonValueString>(Shape.BodyIdentity), TEXT("registered body reference"));
			Shapes.Add(Shape.Identity);
		}
		TSet<FString> Constraints;
		for (int32 Index = 0; Index < ConstraintBindings.Num(); ++Index)
		{
			const auto& Constraint = ConstraintBindings[Index];
			if (!Constraint.TryValidate(OutError)) { PrefixPhysicsBinding(OutError, TEXT("constraint_bindings"), Index); return false; }
			if (Constraints.Contains(Constraint.Identity)) return PhysicsBindingFailure(OutError, Code, TEXT("The constraint reference is registered more than once."),
				FString::Printf(TEXT("constraint_bindings[%d].identity"), Index), Identity, MakeShared<FJsonValueString>(Constraint.Identity), TEXT("unique constraint reference"));
			if (!Bodies.Contains(Constraint.BodyAIdentity)) return PhysicsBindingFailure(OutError, Code, TEXT("Constraint body A is not registered."),
				FString::Printf(TEXT("constraint_bindings[%d].body_a_identity"), Index), Identity, MakeShared<FJsonValueString>(Constraint.BodyAIdentity), TEXT("registered body reference"));
			if (!Bodies.Contains(Constraint.BodyBIdentity)) return PhysicsBindingFailure(OutError, Code, TEXT("Constraint body B is not registered."),
				FString::Printf(TEXT("constraint_bindings[%d].body_b_identity"), Index), Identity, MakeShared<FJsonValueString>(Constraint.BodyBIdentity), TEXT("registered body reference"));
			Constraints.Add(Constraint.Identity);
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsAssetBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsSubjectBinding::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const TCHAR* Code = TEXT("physics_subject_binding_invalid");
		if (!RequirePhysicsText(OutError, Code, TEXT("The physics subject binding requires a subject reference."), TEXT("identity"), Identity, Identity)) return false;
		if (!Component.IsValid()) return PhysicsBindingFailure(OutError, Code, TEXT("The physics subject component is unavailable."), TEXT("component"), Identity, nullptr, TEXT("valid primitive component"));
		return RequirePhysicsText(OutError, Code, TEXT("The physics subject requires a simulation frame reference."), TEXT("native_simulation_frame_id"), Identity, NativeSimulationFrameId)
			&& RequirePhysicsText(OutError, Code, TEXT("The physics subject requires a simulation clock reference."), TEXT("native_simulation_clock_id"), Identity, NativeSimulationClockId);
	}

	bool FUeAgentRigSimulationSpatialPhysicsSubjectBinding::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSubject::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (SubjectRef.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_invalid"), TEXT("The physics probe subject has no reference."), TEXT("/subject_ref"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (!Component.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_invalid"), TEXT("The physics probe subject component is unavailable."), TEXT("/component"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (NativeSimulationFrameId.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_subject_invalid"), TEXT("The physics probe subject has no native simulation frame."), TEXT("/native_simulation_frame_id"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSubject::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeVector::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (SubjectRef.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"), TEXT("The physics vector has no subject reference."), TEXT("/subject_ref"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (!Component.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"), TEXT("The physics vector subject component is unavailable."), TEXT("/component"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (!Time.IsValid()) return NativeFailureAt(OutError, Time.GetValidationDiagnostic(), TEXT("/time"), SubjectRef);
		if (NativeFrameId.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_vector_invalid"), TEXT("The physics vector has no native frame."), TEXT("/native_frame_id"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		return FiniteVector(OutError, Value, TEXT("/value"), SubjectRef);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeVector::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSample::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (SubjectRef.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_sample_invalid"), TEXT("The physics sample has no subject reference."), TEXT("/subject_ref"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (!Time.IsValid()) return NativeFailureAt(OutError, Time.GetValidationDiagnostic(), TEXT("/time"), SubjectRef);
		if (FrameId.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_sample_invalid"), TEXT("The physics sample has no frame."), TEXT("/frame_id"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		if (!FiniteVector(OutError, Position, TEXT("/position"), SubjectRef)
			|| !FiniteVector(OutError, Velocity, TEXT("/velocity"), SubjectRef)) return false;
		if (EvidenceId.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_sample_invalid"), TEXT("The physics sample has no observation evidence."), TEXT("/evidence_id"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SubjectRef);
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSample::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (!Session.TryValidate(OutError)) return NativeFailureAt(OutError, OutError, TEXT("/simulation_session"));
		if (Subjects.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_request_invalid"),
			TEXT("A physics probe session requires at least one subject."), TEXT("/subjects"),
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more subjects")), Session.OwnerRef);
		if (!SimulationRange.IsValid()) return NativeFailureAt(OutError, SimulationRange.GetValidationDiagnostic(), TEXT("/simulation_range"), Session.OwnerRef);
		TSet<FString> SubjectIds;
		for (int32 Index = 0; Index < Subjects.Num(); ++Index)
		{
			const auto& Subject = Subjects[Index];
			const FString Path = TEXT("/subjects/") + FString::FromInt(Index);
			if (!Subject.TryValidate(OutError)) return NativeFailureAt(OutError, OutError, Path);
			if (SubjectIds.Contains(Subject.SubjectRef))
				return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_request_invalid"),
					TEXT("The physics probe session contains a duplicate subject reference."), Path + TEXT("/subject_ref"),
					MakeShared<FJsonValueString>(Subject.SubjectRef), MakeShared<FJsonValueString>(TEXT("unique subject reference")), Subject.SubjectRef);
			SubjectIds.Add(Subject.SubjectRef);
		}
		auto ValidateVectors = [&](const TArray<FUeAgentRigSimulationSpatialPhysicsProbeVector>& Values, const FString& Path)
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const auto& Value = Values[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				if (!Value.TryValidate(OutError)) return NativeFailureAt(OutError, OutError, ItemPath);
				if (!SubjectIds.Contains(Value.SubjectRef))
					return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_request_invalid"),
						TEXT("The physics vector refers to a subject outside this probe session."), ItemPath + TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(Value.SubjectRef), MakeShared<FJsonValueString>(TEXT("a session subject reference")), Value.SubjectRef);
			}
			return true;
		};
		return ValidateVectors(Impulses, TEXT("/impulses")) && ValidateVectors(Velocities, TEXT("/velocities"));
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	FUeAgentRigSimulationSpatialPhysicsProbeSessionResult::
	FUeAgentRigSimulationSpatialPhysicsProbeSessionResult(
		FRigSimulationSpatialSimulationSession InSession,
		const bool bInCompleted,
		const bool bInRestored,
		TArray<FUeAgentRigSimulationSpatialPhysicsProbeSample> InSamples,
		FString InRestorationEvidenceId,
		UObject* InOwnerObject)
		: Session(MoveTemp(InSession))
		, bCompleted(bInCompleted)
		, bRestored(bInRestored)
		, Samples(MoveTemp(InSamples))
		, RestorationEvidenceId(MoveTemp(InRestorationEvidenceId))
		, OwnerObject(InOwnerObject)
	{
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionResult::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (!Session.TryValidate(OutError)) return NativeFailureAt(OutError, OutError, TEXT("/simulation_session"));
		if (!OwnerObject.IsValid()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_result_invalid"), TEXT("The physics session owner object is unavailable."), TEXT("/owner_object"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Session.OwnerRef);
		if (!bRestored)
		{
			if (!RestorationDiagnostic.IsEmpty()) return NativeFailureAt(OutError, RestorationDiagnostic, FString(), Session.OwnerRef);
			return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_result_invalid"), TEXT("The physics session did not restore the simulated subjects."), TEXT("/restored"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Session.OwnerRef);
		}
		if (RestorationEvidenceId.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_result_invalid"), TEXT("The physics session has no restoration evidence."), TEXT("/restoration_evidence_id"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Session.OwnerRef);
		if (bCompleted && Samples.IsEmpty()) return ProbeFailure(OutError, TEXT("rig_simulation_spatial_physics_session_result_invalid"),
			TEXT("A completed physics probe session contains no samples."), TEXT("/samples"),
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more samples for a completed session")), Session.OwnerRef);
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
			if (!Samples[Index].TryValidate(OutError))
				return NativeFailureAt(OutError, OutError, TEXT("/samples/") + FString::FromInt(Index));
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionResult::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool TryCreateRigSimulationSpatialPhysicsNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> PhysicsSessionPort,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError)
	{
		OutPorts = {};
		OutError = {};
		OutPorts.SupportedCapabilities = {
			TEXT("physics.material.update"),
			TEXT("physics.asset.structure.replace"),
			TEXT("physics.constraint.update"),
			TEXT("physics.simulation.probe"),
		};
		OutPorts.Readback = MakeShared<FReadback>(Environment, PhysicsSessionPort);
		OutPorts.Mutation = MakeShared<FMutation>(Environment, PhysicsSessionPort);
		OutPorts.Settle = MakeShared<FSettle>(Environment);
		OutPorts.Comparator = MakeShared<FComparator>();
		return OutPorts.TryValidate(OutError);
	}
}
