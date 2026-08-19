#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"

#include "Components/PrimitiveComponent.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
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
			if ((Request->GetFamilyId() != TEXT("simulation_definition")
					&& Request->GetFamilyId() != TEXT("simulation_observation"))
				|| Request->GetTypeId().IsEmpty()
				|| Request->GetRequestKindId().IsEmpty())
			{
				Fail(OutError,
					TEXT("rig_simulation_spatial_physics_typed_request_invalid"),
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

		bool TryNumber(
			const FString& Text,
			double Minimum,
			double Maximum,
			double& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject)
		{
			if (!LexTryParseString(OutValue, *Text) || !FMath::IsFinite(OutValue)
				|| OutValue < Minimum || OutValue > Maximum)
			{
				return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_numeric_invalid"), Subject, Text);
			}
			return true;
		}

		bool TryOptionalNumber(
			const FString& Text,
			double Minimum,
			double Maximum,
			double& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError,
			const FString& Subject)
		{
			return Text.IsEmpty() || TryNumber(Text, Minimum, Maximum,
				OutValue, OutError, Subject);
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
				OutError, TEXT("friction"))) return false;
			if (!State.Friction.IsEmpty()) Material.Friction = Value;
			if (!TryOptionalNumber(State.StaticFriction, 0.0, MAX_flt, Value,
				OutError, TEXT("static_friction"))) return false;
			if (!State.StaticFriction.IsEmpty()) Material.StaticFriction = Value;
			if (!TryOptionalNumber(State.Restitution, 0.0, 1.0, Value,
				OutError, TEXT("restitution"))) return false;
			if (!State.Restitution.IsEmpty()) Material.Restitution = Value;
			if (!TryOptionalNumber(State.Density, 0.0, MAX_flt, Value,
				OutError, TEXT("density"))) return false;
			if (!State.Density.IsEmpty()) Material.Density = Value;
			if (!TryOptionalNumber(State.RaiseMassToPower, 0.1, 1.0, Value,
				OutError, TEXT("raise_mass_to_power"))) return false;
			if (!State.RaiseMassToPower.IsEmpty()) Material.RaiseMassToPower = Value;
			if (!State.FrictionCombineMode.IsEmpty())
			{
				EFrictionCombineMode::Type Mode;
				if (!TryCombineMode(State.FrictionCombineMode, Mode)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("friction_combine_mode"), State.FrictionCombineMode);
				Material.FrictionCombineMode = Mode;
				Material.bOverrideFrictionCombineMode = true;
			}
			if (!State.RestitutionCombineMode.IsEmpty())
			{
				EFrictionCombineMode::Type Mode;
				if (!TryCombineMode(State.RestitutionCombineMode, Mode)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("restitution_combine_mode"), State.RestitutionCombineMode);
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
				if (!TryCollision(State.CollisionEnabled, Collision)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("collision_enabled"), State.CollisionEnabled);
				Body.DefaultInstance.SetCollisionEnabled(Collision, false);
			}
			if (!State.PhysicsType.IsEmpty())
			{
				EPhysicsType PhysicsType;
				if (!TryPhysicsType(State.PhysicsType, PhysicsType)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("physics_type"), State.PhysicsType);
				Body.PhysicsType = PhysicsType;
			}
			if (!State.MassKg.IsEmpty())
			{
				double Mass = 0.0;
				if (!TryNumber(State.MassKg, UE_SMALL_NUMBER, MAX_flt, Mass,
					OutError, TEXT("mass_kg"))) return false;
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
			if ((!State.LinearXMotion.IsEmpty()
					&& !TryLinearMotion(State.LinearXMotion, X))
				|| (!State.LinearYMotion.IsEmpty()
					&& !TryLinearMotion(State.LinearYMotion, Y))
				|| (!State.LinearZMotion.IsEmpty()
					&& !TryLinearMotion(State.LinearZMotion, Z)))
				return Fail(OutError, TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("linear_motion"));
			if (!TryOptionalNumber(State.LinearLimitCm, 0.0, MAX_flt, LinearLimit,
				OutError, TEXT("linear_limit_cm"))) return false;
			Constraint.SetLinearLimits(X, Y, Z, LinearLimit);

			EAngularConstraintMotion Twist = Constraint.GetAngularTwistMotion();
			EAngularConstraintMotion Swing1 = Constraint.GetAngularSwing1Motion();
			EAngularConstraintMotion Swing2 = Constraint.GetAngularSwing2Motion();
			double TwistLimit = Constraint.GetAngularTwistLimit();
			double Swing1Limit = Constraint.GetAngularSwing1Limit();
			double Swing2Limit = Constraint.GetAngularSwing2Limit();
			if ((!State.TwistMotion.IsEmpty()
					&& !TryAngularMotion(State.TwistMotion, Twist))
				|| (!State.Swing1Motion.IsEmpty()
					&& !TryAngularMotion(State.Swing1Motion, Swing1))
				|| (!State.Swing2Motion.IsEmpty()
					&& !TryAngularMotion(State.Swing2Motion, Swing2)))
				return Fail(OutError, TEXT("rig_simulation_spatial_physics_enum_invalid"),
					TEXT("angular_motion"));
			if (!TryOptionalNumber(State.TwistLimitDegrees, 0.0, 180.0,
				TwistLimit, OutError, TEXT("twist_limit_degrees"))
				|| !TryOptionalNumber(State.Swing1LimitDegrees, 0.0, 180.0,
					Swing1Limit, OutError, TEXT("swing1_limit_degrees"))
				|| !TryOptionalNumber(State.Swing2LimitDegrees, 0.0, 180.0,
					Swing2Limit, OutError, TEXT("swing2_limit_degrees"))) return false;
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
			if (!Source || !Environment.TryConvertSpatialValueToFrame(
				*Source, NativeFrame, Resources, Converted, OutError)
				|| !Converted.IsValid() || Converted->GetComponents().Num() != 16)
				return Source ? false : Fail(OutError,
					TEXT("rig_simulation_spatial_physics_transform_missing"), Subject);
			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					double Value = 0.0;
					if (!TryNumber(Converted->GetComponents()[Row * 4 + Column],
						-MAX_flt, MAX_flt, Value, OutError, Subject)) return false;
					Matrix.M[Column][Row] = Value;
				}
			}
			OutTransform = FTransform(Matrix);
			OutTransform.NormalizeRotation();
			return OutTransform.IsValid();
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
			return Fail(OutError, TEXT("rig_simulation_spatial_physics_shape_kind_invalid"),
				Shape.Identity, Shape.ShapeKind);
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
			if (Input.Bodies.Num() != Binding.BodyBindings.Num()
				|| Input.Constraints.Num() != Binding.ConstraintBindings.Num())
				return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_structure_not_complete"), Input.PhysicsAssetRef);
			Asset.SkeletalBodySetups.Reset();
			Asset.ConstraintSetup.Reset();
			Asset.PhysicalAnimationProfiles.Reset();
			Asset.ConstraintProfiles.Reset();
			TSet<FString> SeenBodies;
			TSet<FString> SeenShapes;
			for (const FRigSimulationSpatialPhysicsBody& BodyInput : Input.Bodies)
			{
				const auto* BodyBinding = FindBodyBinding(Binding, BodyInput.BodyRef);
				if (!BodyBinding || BodyBinding->BoneIdentity != BodyInput.BoneRef
					|| SeenBodies.Contains(BodyInput.BodyRef)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_body_binding_invalid"), BodyInput.BodyRef);
				SeenBodies.Add(BodyInput.BodyRef);
				USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(&Asset,
					MakeUniqueObjectName(&Asset, USkeletalBodySetup::StaticClass(),
						BodyBinding->BoneName), RF_Transactional);
				if (!Body) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_body_create_failed"), BodyInput.BodyRef);
				Body->BoneName = BodyBinding->BoneName;
				if (!ApplyBodyState(*Body, BodyInput.State, OutError)) return false;
				for (const FString& ShapeRef : BodyInput.ShapeRefs)
				{
					const auto* Shape = FindShapeBinding(Binding, ShapeRef);
					if (!Shape || Shape->BodyIdentity != BodyInput.BodyRef
						|| SeenShapes.Contains(ShapeRef)
						|| !AddShape(*Body, *Shape, OutError)) return Shape ? false : Fail(OutError,
						TEXT("rig_simulation_spatial_physics_shape_binding_invalid"), ShapeRef);
					SeenShapes.Add(ShapeRef);
				}
				Asset.SkeletalBodySetups.Add(Body);
			}
			if (SeenShapes.Num() != Binding.ShapeBindings.Num()) return Fail(OutError,
				TEXT("rig_simulation_spatial_physics_shape_set_not_complete"), Input.PhysicsAssetRef);

			TSet<FString> SeenConstraints;
			for (const FRigSimulationSpatialPhysicsConstraint& ConstraintInput
				: Input.Constraints)
			{
				const auto* ConstraintBinding = FindConstraintBinding(
					Binding, ConstraintInput.ConstraintRef);
				const auto* BodyA = ConstraintBinding
					? FindBodyBinding(Binding, ConstraintBinding->BodyAIdentity) : nullptr;
				const auto* BodyB = ConstraintBinding
					? FindBodyBinding(Binding, ConstraintBinding->BodyBIdentity) : nullptr;
				if (!ConstraintBinding || !BodyA || !BodyB
					|| ConstraintInput.EndpointA.BodyRef != ConstraintBinding->BodyAIdentity
					|| ConstraintInput.EndpointB.BodyRef != ConstraintBinding->BodyBIdentity
					|| SeenConstraints.Contains(ConstraintInput.ConstraintRef)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"),
					ConstraintInput.ConstraintRef);
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
				if (!Constraint) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_constraint_create_failed"),
					ConstraintInput.ConstraintRef);
				Constraint->DefaultInstance.JointName = ConstraintBinding->NativeName;
				Constraint->DefaultInstance.ConstraintBone1 = BodyA->BoneName;
				Constraint->DefaultInstance.ConstraintBone2 = BodyB->BoneName;
				Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame1, FrameA);
				Constraint->DefaultInstance.SetRefFrame(EConstraintFrame::Frame2, FrameB);
				if (!ApplyConstraintState(Constraint->DefaultInstance,
					ConstraintInput.State, OutError)) return false;
				Asset.ConstraintSetup.Add(Constraint);
			}
			TSet<FName> ProfileNames;
			for (const FRigSimulationSpatialPhysicsProfile& Profile : Input.Profiles)
			{
				const FName ProfileName(*Profile.ProfileName);
				if (ProfileName.IsNone() || ProfileNames.Contains(ProfileName)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_profile_invalid"), Profile.ProfileName);
				ProfileNames.Add(ProfileName);
				Asset.PhysicalAnimationProfiles.Add(ProfileName);
				Asset.ConstraintProfiles.Add(ProfileName);
				for (const FString& BodyRef : Profile.BodyRefs)
				{
					const auto* BodyBinding = FindBodyBinding(Binding, BodyRef);
					USkeletalBodySetup* Body = BodyBinding
						? FindBody(Asset, BodyBinding->BoneName) : nullptr;
					if (!Body) return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_profile_body_invalid"), BodyRef);
					Body->AddPhysicalAnimationProfile(ProfileName);
				}
				for (const FString& ConstraintRef : Profile.ConstraintRefs)
				{
					const auto* ConstraintBinding = FindConstraintBinding(Binding, ConstraintRef);
					UPhysicsConstraintTemplate* Constraint = ConstraintBinding
						? FindConstraint(Asset, ConstraintBinding->NativeName) : nullptr;
					if (!Constraint) return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_profile_constraint_invalid"),
						ConstraintRef);
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
			if (!ConstraintBinding
				|| Input.EndpointA.BodyRef != ConstraintBinding->BodyAIdentity
				|| Input.EndpointB.BodyRef != ConstraintBinding->BodyBIdentity)
				return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_constraint_binding_invalid"),
					Input.ConstraintRef);
			UPhysicsConstraintTemplate* Constraint =
				FindConstraint(Asset, ConstraintBinding->NativeName);
			if (!Constraint) return Fail(OutError,
				TEXT("rig_simulation_spatial_physics_constraint_unavailable"),
				Input.ConstraintRef);
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
			return ApplyConstraintState(
				Constraint->DefaultInstance, Input.ConstraintState, OutError);
		}

		bool TryVector(
			const FUeAgentSpatialValue& Value,
			FVector& OutVector,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Value.GetQuantityKind() != EUeAgentSpatialQuantityKind::Direction
				|| Value.GetComponents().Num() != 3) return Fail(OutError,
				TEXT("rig_simulation_spatial_physics_vector_invalid"), Value.GetSubject());
			double Components[3] = {};
			for (int32 Index = 0; Index < 3; ++Index)
				if (!TryNumber(Value.GetComponents()[Index], -MAX_flt, MAX_flt,
					Components[Index], OutError, Value.GetSubject())) return false;
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
			for (const FString& SubjectRef : Input.SubjectRefs)
			{
				const auto* Binding = Environment.FindPhysicsSubjectBinding(SubjectRef);
				if (!Binding || !Binding->IsValid()
					|| Environment.ResolveExplicitObject(SubjectRef) != Binding->Component.Get())
					return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_subject_binding_unavailable"),
						SubjectRef);
				if (NativeClock.IsEmpty()) NativeClock = Binding->NativeSimulationClockId;
				if (NativeClock != Binding->NativeSimulationClockId) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_subject_clock_mismatch"), SubjectRef);
				OutRequest.Subjects.Add({SubjectRef, Binding->Component,
					Binding->NativeSimulationFrameId});
			}
			const TArray<FUeAgentTemporalRange>& Ranges =
				Request.GetFormalPortRequest().GetTemporalRanges();
			TOptional<FUeAgentTemporalRange> NativeRange;
			if (Ranges.Num() != 1 || !Environment.TryConvertTemporalRangeToClock(
				Ranges[0], NativeClock, Resources, NativeRange, OutError)
				|| !NativeRange.IsSet()) return Ranges.Num() == 1 ? false : Fail(OutError,
				TEXT("rig_simulation_spatial_physics_probe_range_invalid"));
			OutRequest.SimulationRange = NativeRange.GetValue();

			TArray<const FUeAgentSpatialValue*> Directions;
			for (const FUeAgentSpatialValuePtr& Value
				: Request.GetFormalPortRequest().GetSpatialValues())
				if (Value.IsValid()
					&& Value->GetQuantityKind() == EUeAgentSpatialQuantityKind::Direction)
					Directions.Add(Value.Get());
			if (Directions.Num() != Input.Impulses.Num() + Input.Velocities.Num())
				return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_probe_vector_count_mismatch"));
			int32 DirectionIndex = 0;
			auto ConvertSamples = [&](const TArray<FRigSimulationSpatialVectorSample>& Samples,
				TArray<FUeAgentRigSimulationSpatialPhysicsProbeVector>& Output) -> bool
			{
				for (const FRigSimulationSpatialVectorSample& Sample : Samples)
				{
					const auto* Binding = Environment.FindPhysicsSubjectBinding(Sample.SubjectRef);
					const FUeAgentSpatialValue* Direction = Directions[DirectionIndex++];
					FUeAgentSpatialValuePtr NativeDirection;
					if (!Binding || Direction->GetSubject() != Sample.SubjectRef
						|| !Environment.TryConvertSpatialValueToFrame(*Direction,
							Binding->NativeSimulationFrameId, Resources,
							NativeDirection, OutError) || !NativeDirection.IsValid()) return false;
					TOptional<FUeAgentTemporalPoint> NativeTime;
					if (!Environment.TryConvertTemporalPointToClock(
						Sample.Time.ToTemporalPoint(), Binding->NativeSimulationClockId,
						Resources, NativeTime, OutError) || !NativeTime.IsSet()) return false;
					FVector Vector;
					if (!TryVector(*NativeDirection, Vector, OutError)) return false;
					Output.Add({Sample.SubjectRef, Binding->Component.Get(), NativeTime.GetValue(),
						Binding->NativeSimulationFrameId, Vector});
				}
				return true;
			};
			return ConvertSamples(Input.Impulses, OutRequest.Impulses)
				&& ConvertSamples(Input.Velocities, OutRequest.Velocities)
				&& OutRequest.IsValid();
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
				? true : Fail(OutError,
					TEXT("rig_simulation_spatial_physics_string_failed"),
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
				return OutError.IsValid() ? false : Fail(OutError,
					TEXT("rig_simulation_spatial_physics_object_failed"),
					Error.Path, Error.Code);
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
				? true : Fail(OutError,
					TEXT("rig_simulation_spatial_physics_snapshot_failed"),
					Error.Path, Error.Code);
		}

		FString SnapshotState(const FUeAgentCanonicalSnapshot& Snapshot)
		{
			if (Snapshot.GetEntries().IsEmpty()) return FString();
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Snapshot.GetEntries()[0].GetValue()->GetObjectItems())
				if (Pair.Key == TEXT("observed_state")) return Pair.Value->GetLexeme();
			return FString();
		}

		FString ReadState(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialRequest& Request,
			IUeAgentRigSimulationSpatialNativeEnvironment& Environment,
			IUeAgentRigSimulationSpatialPhysicsSessionPort* SessionPort,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialExecutionError& OutError)
		{
			if (Contract.GetCapabilityId() == TEXT("physics.material.update"))
			{
				const auto& Input = Request.GetPayload().Get<FPhysicsMaterialUpdateInput>();
				const UPhysicalMaterial* Material = Cast<UPhysicalMaterial>(
					Environment.ResolveExplicitObject(Input.PhysicalMaterialRef));
				if (!Material) { Fail(OutError,
					TEXT("rig_simulation_spatial_physics_material_unavailable"),
					Input.PhysicalMaterialRef); return FString(); }
				UPhysicalMaterial* Probe = DuplicateObject<UPhysicalMaterial>(
					Material, GetTransientPackage());
				bool bMatches = Probe && ApplyMaterialState(*Probe, Input.MaterialState, OutError)
					&& FMath::IsNearlyEqual(Probe->Friction, Material->Friction)
					&& FMath::IsNearlyEqual(Probe->StaticFriction, Material->StaticFriction)
					&& FMath::IsNearlyEqual(Probe->Restitution, Material->Restitution)
					&& FMath::IsNearlyEqual(Probe->Density, Material->Density)
					&& FMath::IsNearlyEqual(Probe->RaiseMassToPower, Material->RaiseMassToPower)
					&& Probe->FrictionCombineMode == Material->FrictionCombineMode
					&& Probe->RestitutionCombineMode == Material->RestitutionCombineMode
					&& Probe->bOverrideFrictionCombineMode == Material->bOverrideFrictionCombineMode
					&& Probe->bOverrideRestitutionCombineMode == Material->bOverrideRestitutionCombineMode;
				return FString::Printf(TEXT("material=%s|friction=%g|matches=%d"),
					*Input.PhysicalMaterialRef, Material->Friction, bMatches ? 1 : 0);
			}
			if (Contract.GetCapabilityId() == TEXT("physics.asset.structure.replace"))
			{
				const auto& Input = Request.GetPayload().Get<FPhysicsAssetStructureReplaceInput>();
				const auto* Binding = Environment.FindPhysicsAssetBinding(Input.PhysicsAssetRef);
				UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
				if (!Binding || !Asset) { Fail(OutError,
					TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
					Input.PhysicsAssetRef); return FString(); }
				bool bMatches = Asset->SkeletalBodySetups.Num() == Input.Bodies.Num()
					&& Asset->ConstraintSetup.Num() == Input.Constraints.Num();
				for (const auto& BodyInput : Input.Bodies)
				{
					const auto* BodyBinding = FindBodyBinding(*Binding, BodyInput.BodyRef);
					USkeletalBodySetup* Body = BodyBinding
						? FindBody(*Asset, BodyBinding->BoneName) : nullptr;
					bMatches &= Body != nullptr && BodyInput.ShapeRefs.Num()
						== (Body ? Body->AggGeom.GetElementCount() : 0);
					if (Body)
					{
						ECollisionEnabled::Type Collision;
						EPhysicsType PhysicsType;
						double Mass = 0.0;
						bMatches &= TryCollision(BodyInput.State.CollisionEnabled, Collision)
							&& TryPhysicsType(BodyInput.State.PhysicsType, PhysicsType)
							&& TryNumber(BodyInput.State.MassKg, UE_SMALL_NUMBER, MAX_flt,
								Mass, OutError, TEXT("mass_kg"))
							&& Body->DefaultInstance.GetCollisionEnabled(false) == Collision
							&& Body->PhysicsType == PhysicsType
							&& FMath::IsNearlyEqual(Body->DefaultInstance.GetMassOverride(),
								static_cast<float>(Mass))
							&& (!BodyInput.State.ConsiderForBounds.IsSet()
								|| Body->bConsiderForBounds
									== BodyInput.State.ConsiderForBounds.GetValue());
					}
				}
				for (const auto& ConstraintInput : Input.Constraints)
				{
					const auto* ConstraintBinding = FindConstraintBinding(
						*Binding, ConstraintInput.ConstraintRef);
					UPhysicsConstraintTemplate* Constraint = ConstraintBinding
						? FindConstraint(*Asset, ConstraintBinding->NativeName) : nullptr;
					FTransform ExpectedA;
					FTransform ExpectedB;
					bMatches &= Constraint && TryNativeTransform(Request,
						ConstraintInput.EndpointA.BodyRef, ConstraintBinding->NativeFrameAId,
						Environment, Context.GetResourceTracker(), ExpectedA, OutError)
						&& TryNativeTransform(Request, ConstraintInput.EndpointB.BodyRef,
							ConstraintBinding->NativeFrameBId, Environment,
							Context.GetResourceTracker(), ExpectedB, OutError)
						&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1)
							.Equals(ExpectedA, 0.001)
						&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2)
							.Equals(ExpectedB, 0.001);
				}
				for (const auto& Profile : Input.Profiles)
					bMatches &= Asset->PhysicalAnimationProfiles.Contains(FName(*Profile.ProfileName))
						&& Asset->ConstraintProfiles.Contains(FName(*Profile.ProfileName));
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
				if (!ConstraintBinding || !Constraint) { Fail(OutError,
					TEXT("rig_simulation_spatial_physics_constraint_unavailable"),
					Input.ConstraintRef); return FString(); }
				UPhysicsAsset* ProbeAsset = DuplicateObject<UPhysicsAsset>(
					Asset, GetTransientPackage());
				bool bMatches = ProbeAsset && ApplyConstraintUpdate(*ProbeAsset, *Binding,
					Input, Request, Environment, Context.GetResourceTracker(), OutError);
				if (bMatches)
				{
					UPhysicsConstraintTemplate* Desired = FindConstraint(
						*ProbeAsset, ConstraintBinding->NativeName);
					FTransform ExpectedA;
					FTransform ExpectedB;
					bMatches = Desired && TryNativeTransform(Request, Input.EndpointA.BodyRef,
						ConstraintBinding->NativeFrameAId, Environment,
						Context.GetResourceTracker(), ExpectedA, OutError)
						&& TryNativeTransform(Request, Input.EndpointB.BodyRef,
							ConstraintBinding->NativeFrameBId, Environment,
							Context.GetResourceTracker(), ExpectedB, OutError)
						&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame1)
							.Equals(ExpectedA, 0.001)
						&& Constraint->DefaultInstance.GetRefFrame(EConstraintFrame::Frame2)
							.Equals(ExpectedB, 0.001)
						&& Constraint->DefaultInstance.GetLinearXMotion()
							== Desired->DefaultInstance.GetLinearXMotion()
						&& FMath::IsNearlyEqual(Constraint->DefaultInstance.GetLinearLimit(),
							Desired->DefaultInstance.GetLinearLimit())
						&& FMath::IsNearlyEqual(
							Constraint->DefaultInstance.GetAngularTwistLimit(),
							Desired->DefaultInstance.GetAngularTwistLimit())
						&& Constraint->DefaultInstance.IsCollisionDisabled()
							== Desired->DefaultInstance.IsCollisionDisabled();
				}
				return FString::Printf(TEXT("constraint=%s|matches=%d"),
					*Input.ConstraintRef, bMatches ? 1 : 0);
			}
			const auto& Input = Request.GetPayload().Get<FPhysicsSimulationProbeInput>();
			if (!SessionPort) { Fail(OutError,
				TEXT("rig_simulation_spatial_physics_session_port_unavailable"),
				Input.SimulationSession.OwnerRef); return FString(); }
			FUeAgentRigSimulationSpatialPhysicsProbeSessionResult Result;
			if (!SessionPort->TryReadPhysicsProbe(Input.SimulationSession,
				Input.SubjectRefs, Context, Result, OutError) || !Result.IsValid())
				return FString();
			const bool bMatches = Result.bCompleted && Result.bRestored
				&& !Result.Samples.IsEmpty();
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
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentRigSimulationSpatialExecutionError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				Environment->CountFamilyDispatch();
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const FString PublicRef = Typed->GetPrimaryTargetRef();
				if (PublicRef.IsEmpty()
					|| (Target.IsSet() && Target->GetPublicRef() != PublicRef)) return Fail(OutError,
					TEXT("rig_simulation_spatial_explicit_target_mismatch"), PublicRef);
				const FString State = ReadState(Contract, *Typed, *Environment,
					SessionPort.Get(), Context, OutError);
				return !State.IsEmpty() && TrySnapshot(Contract, Phase, PublicRef,
					State, Context, OutSnapshot, OutError);
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
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				const auto* Typed = ProductionRequest(Request, OutError);
				if (!Typed) return false;
				const TOptional<FUeAgentCandidateBinding> Target = FirstTarget(Targets);
				if (!Target.IsSet()
					|| Target->GetPublicRef() != Typed->GetPrimaryTargetRef()) return Fail(OutError,
					TEXT("rig_simulation_spatial_explicit_target_mismatch"),
					Typed->GetPrimaryTargetRef());

				UObject* MutatedObject = nullptr;
				if (Contract.GetCapabilityId() == TEXT("physics.material.update"))
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsMaterialUpdateInput>();
					UPhysicalMaterial* Material = Cast<UPhysicalMaterial>(
						Environment->ResolveExplicitObject(Input.PhysicalMaterialRef));
					if (!Material) return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_material_unavailable"),
						Input.PhysicalMaterialRef);
					if (!CheckRevision(*Environment, Input.PhysicalMaterialRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicalMaterial* Probe = DuplicateObject<UPhysicalMaterial>(
						Material, GetTransientPackage());
					if (!Probe || !ApplyMaterialState(*Probe, Input.MaterialState, OutError))
						return false;
					Material->Modify();
					if (!ApplyMaterialState(*Material, Input.MaterialState, OutError)) return false;
					Material->MarkPackageDirty();
					MutatedObject = Material;
				}
				else if (Contract.GetCapabilityId()
					== TEXT("physics.asset.structure.replace"))
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsAssetStructureReplaceInput>();
					const auto* Binding = Environment->FindPhysicsAssetBinding(Input.PhysicsAssetRef);
					UPhysicsAsset* Asset = Binding ? Binding->PhysicsAsset.Get() : nullptr;
					if (!Binding || !Asset
						|| Environment->ResolveExplicitObject(Input.PhysicsAssetRef) != Asset)
						return Fail(OutError,
							TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
							Input.PhysicsAssetRef);
					if (!CheckRevision(*Environment, Input.PhysicsAssetRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicsAsset* Probe = DuplicateObject<UPhysicsAsset>(
						Asset, GetTransientPackage());
					if (!Probe || !ApplyAssetStructure(*Probe, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
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
					if (!Binding || !Asset) return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_asset_binding_unavailable"),
						Input.PhysicsAssetRef);
					if (!CheckRevision(*Environment, Input.PhysicsAssetRef,
						Input.ExpectedRevision, OutError)) return false;
					UPhysicsAsset* Probe = DuplicateObject<UPhysicsAsset>(
						Asset, GetTransientPackage());
					if (!Probe || !ApplyConstraintUpdate(*Probe, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->Modify();
					if (!ApplyConstraintUpdate(*Asset, *Binding, Input, *Typed,
						*Environment, Context.GetResourceTracker(), OutError)) return false;
					Asset->MarkPackageDirty();
					MutatedObject = Asset;
				}
				else
				{
					const auto& Input = Typed->GetPayload().Get<FPhysicsSimulationProbeInput>();
					if (!SessionPort.IsValid()) return Fail(OutError,
						TEXT("rig_simulation_spatial_physics_session_port_unavailable"),
						Input.SimulationSession.OwnerRef);
					FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest SessionRequest;
					if (!BuildProbeRequest(Input, *Typed, *Environment,
						Context.GetResourceTracker(), SessionRequest, OutError)) return false;
					FUeAgentRigSimulationSpatialPhysicsProbeSessionResult SessionResult;
					if (!SessionPort->TryRunPhysicsProbe(SessionRequest, Context,
						SessionResult, OutError) || !SessionResult.IsValid()
						|| !SessionResult.bCompleted || !SessionResult.bRestored) return false;
					MutatedObject = SessionResult.OwnerObject.Get();
				}
				if (!IsValid(MutatedObject)) return Fail(OutError,
					TEXT("rig_simulation_spatial_physics_mutation_target_unavailable"),
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
				if (!IsCapability(Contract.GetCapabilityId())) return Fail(OutError,
					TEXT("rig_simulation_spatial_family_port_unavailable"),
					Contract.GetCapabilityId());
				UObject* Object = Environment->FindMutation(Mutation.GetOperationId());
				if (!IsValid(Object)) return Fail(OutError,
					TEXT("rig_simulation_spatial_settle_target_unavailable"),
					Mutation.GetOperationId());
				if (UPhysicsAsset* Asset = Cast<UPhysicsAsset>(Object))
				{
					Asset->UpdateBodySetupIndexMap();
					Asset->UpdateBoundsBodiesArray();
					Asset->InvalidateAllPhysicsMeshes();
					Asset->RefreshPhysicsAssetChange();
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
				if (Contract.GetCapabilityId() == TEXT("physics.simulation.probe"))
					bMatches &= BeforeState != AfterState;
				OutOutcome = FVerificationOutcome(
					TEXT("rig-simulation-spatial-verify:") + Contract.GetCapabilityId(),
					bMatches ? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch,
					bMatches ? EEffectOutcome::Complete : EEffectOutcome::Partial,
					bMatches ? FString()
						: TEXT("rig_simulation_spatial_physics_state_mismatch"),
					false, {}, {TEXT("independent-state:") + After.GetSnapshotId()}, {});
				return true;
			}
		};
	}

	bool FUeAgentRigSimulationSpatialPhysicsBodyBinding::IsValid() const
	{
		return !Identity.IsEmpty() && !BoneIdentity.IsEmpty() && !BoneName.IsNone();
	}

	bool FUeAgentRigSimulationSpatialPhysicsShapeBinding::IsValid() const
	{
		return !Identity.IsEmpty() && !BodyIdentity.IsEmpty() && !NativeName.IsNone()
			&& (ShapeKind == TEXT("box") || ShapeKind == TEXT("sphere")
				|| ShapeKind == TEXT("capsule"))
			&& FMath::IsFinite(Dimensions.X) && FMath::IsFinite(Dimensions.Y)
			&& FMath::IsFinite(Dimensions.Z) && Dimensions.X > 0.0
			&& (ShapeKind == TEXT("sphere")
				|| (Dimensions.Y > 0.0 && (ShapeKind == TEXT("capsule")
					|| Dimensions.Z > 0.0)))
			&& LocalTransform.IsValid();
	}

	bool FUeAgentRigSimulationSpatialPhysicsConstraintBinding::IsValid() const
	{
		return !Identity.IsEmpty() && !NativeName.IsNone()
			&& !BodyAIdentity.IsEmpty() && !BodyBIdentity.IsEmpty()
			&& BodyAIdentity != BodyBIdentity && !NativeFrameAId.IsEmpty()
			&& !NativeFrameBId.IsEmpty() && NativeFrameAId != NativeFrameBId;
	}

	bool FUeAgentRigSimulationSpatialPhysicsAssetBinding::IsValid() const
	{
		if (Identity.IsEmpty() || !PhysicsAsset.IsValid() || BodyBindings.IsEmpty())
			return false;
		TSet<FString> Bodies;
		for (const auto& Body : BodyBindings)
		{
			if (!Body.IsValid() || Bodies.Contains(Body.Identity)) return false;
			Bodies.Add(Body.Identity);
		}
		TSet<FString> Shapes;
		for (const auto& Shape : ShapeBindings)
		{
			if (!Shape.IsValid() || Shapes.Contains(Shape.Identity)
				|| !Bodies.Contains(Shape.BodyIdentity)) return false;
			Shapes.Add(Shape.Identity);
		}
		TSet<FString> Constraints;
		for (const auto& Constraint : ConstraintBindings)
		{
			if (!Constraint.IsValid() || Constraints.Contains(Constraint.Identity)
				|| !Bodies.Contains(Constraint.BodyAIdentity)
				|| !Bodies.Contains(Constraint.BodyBIdentity)) return false;
			Constraints.Add(Constraint.Identity);
		}
		return true;
	}

	bool FUeAgentRigSimulationSpatialPhysicsSubjectBinding::IsValid() const
	{
		return !Identity.IsEmpty() && Component.IsValid()
			&& !NativeSimulationFrameId.IsEmpty()
			&& !NativeSimulationClockId.IsEmpty();
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSubject::IsValid() const
	{
		return !SubjectRef.IsEmpty() && Component.IsValid()
			&& !NativeSimulationFrameId.IsEmpty();
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeVector::IsValid() const
	{
		return !SubjectRef.IsEmpty() && Component.IsValid() && Time.IsValid()
			&& !NativeFrameId.IsEmpty() && FMath::IsFinite(Value.X)
			&& FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSample::IsValid() const
	{
		return !SubjectRef.IsEmpty() && Time.IsValid() && !FrameId.IsEmpty()
			&& FMath::IsFinite(Position.X) && FMath::IsFinite(Position.Y)
			&& FMath::IsFinite(Position.Z) && FMath::IsFinite(Velocity.X)
			&& FMath::IsFinite(Velocity.Y) && FMath::IsFinite(Velocity.Z)
			&& !EvidenceId.IsEmpty();
	}

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionRequest::IsValid() const
	{
		if (!Session.IsValid() || Subjects.IsEmpty() || !SimulationRange.IsValid())
			return false;
		TSet<FString> SubjectIds;
		for (const auto& Subject : Subjects)
		{
			if (!Subject.IsValid() || SubjectIds.Contains(Subject.SubjectRef)) return false;
			SubjectIds.Add(Subject.SubjectRef);
		}
		for (const auto& Value : Impulses)
			if (!Value.IsValid() || !SubjectIds.Contains(Value.SubjectRef)) return false;
		for (const auto& Value : Velocities)
			if (!Value.IsValid() || !SubjectIds.Contains(Value.SubjectRef)) return false;
		return true;
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

	bool FUeAgentRigSimulationSpatialPhysicsProbeSessionResult::IsValid() const
	{
		if (!Session.IsValid() || !OwnerObject.IsValid()
			|| !bRestored || RestorationEvidenceId.IsEmpty()
			|| (bCompleted && Samples.IsEmpty())) return false;
		for (const auto& Sample : Samples)
			if (!Sample.IsValid()) return false;
		return true;
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
		return OutPorts.IsValid() ? true : Fail(OutError,
			TEXT("rig_simulation_spatial_physics_ports_unavailable"));
	}
}
