#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"
#include "Misc/TVariant.h"

#define UAI_RIG_SIMULATION_SPATIAL_TYPED_REQUESTS_V1 1

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	struct UEAGENTINTERFACE_API FRigSimulationSpatialRevision
	{
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;

		bool IsValid() const;
		bool operator==(const FRigSimulationSpatialRevision& Other) const;
	};

	struct UEAGENTINTERFACE_API FRigSimulationSpatialRate
	{
		FString Numerator;
		FString Denominator;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FRigSimulationSpatialPoint
	{
		FString ClockId;
		FString Tick;
		FRigSimulationSpatialRate Rate;
		FString State;
		bool IsValid() const;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint ToTemporalPoint() const;
	};

	struct UEAGENTINTERFACE_API FRigSimulationSpatialRange
	{
		FString ClockId;
		FString LowerTick;
		FString UpperTick;
		FRigSimulationSpatialRate Rate;
		FString State;
		bool IsValid() const;
		UE::UAI::StateSemantic::FUeAgentTemporalRange ToTemporalRange() const;
	};

	struct UEAGENTINTERFACE_API FRigSimulationSpatialFrame3d
	{
		FString FrameId;
		FString BasisFrameId;
		int32 Dimension = 0;
		FString Unit;
		FString BasisEvidenceId;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FRigSimulationSpatialTransform3d
	{
		FString Subject;
		FString Quantity;
		FRigSimulationSpatialFrame3d Frame;
		TArray<FString> Components;
		TArray<FString> ConversionEvidenceIds;
		bool IsValid() const;
	};

	struct FRigSimulationSpatialIkDefinitionChange
	{
		FString Kind;
		FString Operation;
		FString PrimaryRef;
		FString SecondaryRef;
		FString TertiaryRef;
		FString SolverKind;
	};

	struct FRigSimulationSpatialPoseItem
	{
		FString BoneRef;
		FRigSimulationSpatialTransform3d Transform;
	};

	struct FRigSimulationSpatialChainMapping
	{
		FString SourceChainRef;
		FString TargetChainRef;
		FString GoalRef;
	};

	struct FRigSimulationSpatialAssetRequest
	{
		FString AssetKind;
		FString Name;
		FString OverwritePolicy;
	};

	struct FRigSimulationSpatialMaterialState
	{
		FString Friction;
		FString StaticFriction;
		FString Restitution;
		FString Density;
		FString RaiseMassToPower;
		FString FrictionCombineMode;
		FString RestitutionCombineMode;
	};

	struct FRigSimulationSpatialPhysicsBodyState
	{
		FString CollisionEnabled;
		FString PhysicsType;
		FString MassKg;
		TOptional<bool> ConsiderForBounds;
	};

	struct FRigSimulationSpatialConstraintState
	{
		FString LinearXMotion;
		FString LinearYMotion;
		FString LinearZMotion;
		FString LinearLimitCm;
		FString TwistMotion;
		FString Swing1Motion;
		FString Swing2Motion;
		FString TwistLimitDegrees;
		FString Swing1LimitDegrees;
		FString Swing2LimitDegrees;
		TOptional<bool> DisableCollision;
	};

	struct FRigSimulationSpatialPhysicsBody
	{
		FString BodyRef;
		FString BoneRef;
		TArray<FString> ShapeRefs;
		FRigSimulationSpatialPhysicsBodyState State;
	};

	struct FRigSimulationSpatialConstraintEndpoint
	{
		FString BodyRef;
		FRigSimulationSpatialTransform3d Frame;
	};

	struct FRigSimulationSpatialPhysicsConstraint
	{
		FString ConstraintRef;
		FRigSimulationSpatialConstraintEndpoint EndpointA;
		FRigSimulationSpatialConstraintEndpoint EndpointB;
		FRigSimulationSpatialConstraintState State;
	};

	struct FRigSimulationSpatialPhysicsProfile
	{
		FString ProfileName;
		TArray<FString> BodyRefs;
		TArray<FString> ConstraintRefs;
	};

	struct FRigSimulationSpatialSimulationSession
	{
		FString SessionId;
		FString OwnerRef;
		FString Generation;

		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const
		{
			Contract::FDiagnostic Error;
			return TryValidate(Error);
		}
	};

	struct FRigSimulationSpatialVectorSample
	{
		FString SubjectRef;
		FRigSimulationSpatialPoint Time;
		FRigSimulationSpatialFrame3d Frame;
		TArray<FString> Components;
		TArray<FString> EvidenceIds;
	};

	struct FRigSimulationSpatialSelector
	{
		FString Mode;
		TArray<FString> Refs;
		TOptional<int32> Start;
		TOptional<int32> Count;
	};

	struct FRigSimulationSpatialFrameMapping
	{
		FRigSimulationSpatialFrame3d SourceFrame;
		FRigSimulationSpatialFrame3d TargetFrame;
		TArray<FString> ConversionEvidenceIds;
	};

	struct FRigSkeletonPreviewMeshBindInput
	{
		FString SkeletonRef;
		FString PreviewMeshRef;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigSkeletonSocketUpdateInput
	{
		FString SkeletonRef;
		FString Operation;
		FString SocketRef;
		FString ParentBoneRef;
		FRigSimulationSpatialTransform3d SocketTransform;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigControlBakeContextInput
	{
		FString SequenceRef;
		FString BindingName;
		FString RigRef;
		FString TargetAnimationRef;
	};
	struct FRigControlBakeAnimationInput
	{
		FString SourceSequenceRef;
		FString TargetAnimationRef;
		FString BindingRef;
		FString RigRef;
		FRigSimulationSpatialRange AuthoredRange;
		FRigSimulationSpatialRate SampleRate;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigControlBakeControlsInput
	{
		FString TargetSequenceRef;
		FString BindingRef;
		FString RigRef;
		FRigSimulationSpatialRange AuthoredRange;
		FRigSimulationSpatialRate SampleRate;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigIkDefinitionUpdateInput
	{
		FString IkRigRef;
		TArray<FRigSimulationSpatialIkDefinitionChange> DefinitionChanges;
		FRigSimulationSpatialRevision ExpectedRevision;
		FString PreviewMeshRef;
	};
	struct FRigRetargetDefinitionUpdateInput
	{
		FString RetargeterRef;
		FString SourceRigRef;
		FString TargetRigRef;
		TArray<FRigSimulationSpatialChainMapping> ChainMappings;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigRetargetPoseUpdateInput
	{
		FString RetargeterRef;
		FString SubjectSide;
		TArray<FRigSimulationSpatialPoseItem> Pose;
		FRigSimulationSpatialFrame3d PoseFrame;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FRigRetargetPoseAutoAlignInput
	{
		FString RetargeterRef;
		FString SubjectSide;
		FString AlignmentPolicy;
		FRigSimulationSpatialRevision ExpectedRevision;
		TArray<FString> BoneRefs;
	};
	struct FRigRetargetAssetsGenerateInput
	{
		FString SourceSubjectRef;
		FString TargetSubjectRef;
		FString RetargeterRef;
		FString DestinationRoot;
		TArray<FRigSimulationSpatialAssetRequest> AssetRequests;
	};
	struct FPhysicsMaterialUpdateInput
	{
		FString PhysicalMaterialRef;
		FRigSimulationSpatialMaterialState MaterialState;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FPhysicsAssetStructureReplaceInput
	{
		FString PhysicsAssetRef;
		TArray<FRigSimulationSpatialPhysicsBody> Bodies;
		TArray<FRigSimulationSpatialPhysicsConstraint> Constraints;
		TArray<FRigSimulationSpatialPhysicsProfile> Profiles;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FPhysicsConstraintUpdateInput
	{
		FString PhysicsAssetRef;
		FString ConstraintRef;
		FRigSimulationSpatialConstraintEndpoint EndpointA;
		FRigSimulationSpatialConstraintEndpoint EndpointB;
		FRigSimulationSpatialConstraintState ConstraintState;
		FRigSimulationSpatialRevision ExpectedRevision;
	};
	struct FPhysicsSimulationProbeInput
	{
		TArray<FString> SubjectRefs;
		FRigSimulationSpatialSimulationSession SimulationSession;
		FRigSimulationSpatialRange SimulationRange;
		TArray<FRigSimulationSpatialVectorSample> Impulses;
		TArray<FRigSimulationSpatialVectorSample> Velocities;
	};
	struct FDeformerGeometrySampleInput
	{
		FString GeometryCacheRef;
		FRigSimulationSpatialPoint SampleTime;
		FRigSimulationSpatialFrame3d OutputFrame;
		TOptional<FRigSimulationSpatialSelector> VertexSelector;
	};
	struct FDeformerTrainingInputsValidateInput
	{
		TArray<FString> TrainingInputRefs;
		TArray<FRigSimulationSpatialRange> TemporalRanges;
		TArray<FRigSimulationSpatialFrameMapping> FrameMappings;
	};
	struct FDeformerEvaluationSampleInput
	{
		FString DeformerRef;
		FString SubjectRef;
		FRigSimulationSpatialPoint SampleTime;
		FRigSimulationSpatialFrame3d OutputFrame;
		TOptional<FRigSimulationSpatialSelector> VertexSelector;
	};
	using FUeAgentRigSimulationSpatialRequestPayload = TVariant<
		FRigSkeletonPreviewMeshBindInput,
		FRigSkeletonSocketUpdateInput,
		FRigControlBakeContextInput,
		FRigControlBakeAnimationInput,
		FRigControlBakeControlsInput,
		FRigIkDefinitionUpdateInput,
		FRigRetargetDefinitionUpdateInput,
		FRigRetargetPoseUpdateInput,
		FRigRetargetPoseAutoAlignInput,
		FRigRetargetAssetsGenerateInput,
		FPhysicsMaterialUpdateInput,
		FPhysicsAssetStructureReplaceInput,
		FPhysicsConstraintUpdateInput,
		FPhysicsSimulationProbeInput,
		FDeformerGeometrySampleInput,
		FDeformerTrainingInputsValidateInput,
		FDeformerEvaluationSampleInput>;

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialRequestEnvironment
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialRequestEnvironment() = default;
		virtual const FString& GetInputRootPointer() const = 0;
		virtual bool HasExplicitObject(const FString& Identity) const = 0;
		virtual bool HasTemporalPath(const FString& ClockId) const = 0;
		virtual bool HasSpatialPath(const FString& FrameId) const = 0;
		virtual bool TryResolveFrame(
			const FRigSimulationSpatialFrame3d& WireFrame,
			UE::UAI::StateSemantic::FUeAgentSpatialFrame& OutFrame,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
		virtual bool TryReadRevision(
			const FString& ExplicitRef,
			FRigSimulationSpatialRevision& OutRevision,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
		virtual const TArray<UE::UAI::StateSemantic::FUeAgentSpatialConversion>&
			GetSpatialConversions() const = 0;
		virtual const TArray<UE::UAI::StateSemantic::FUeAgentTemporalConversion>&
			GetTemporalConversions() const = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialRequest final
		: public IUeAgentRigSimulationSpatialTypedRequest
	{
	public:
		FUeAgentRigSimulationSpatialRequest(
			FString InTypeId,
			FString InFamilyId,
			FString InRequestKindId,
			bool bInOperation,
			FString InPrimaryTargetRef,
			TOptional<FRigSimulationSpatialRevision> InExpectedRevision,
			FUeAgentRigSimulationSpatialRequestPayload InPayload,
			FUeAgentRigSimulationSpatialPortRequest InFormalPortRequest,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef InIntentPayload);

		const FString& GetTypeId() const override;
		const FString& GetFamilyId() const override;
		const FString& GetRequestKindId() const override;
		const FString& GetPrimaryTargetRef() const;
		const FUeAgentRigSimulationSpatialRequestPayload& GetPayload() const;
		const FUeAgentRigSimulationSpatialPortRequest& GetFormalPortRequest() const override;
		bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const override;

	private:
		FString TypeId;
		FString FamilyId;
		FString RequestKindId;
		bool bOperation = false;
		FString PrimaryTargetRef;
		TOptional<FRigSimulationSpatialRevision> ExpectedRevision;
		FUeAgentRigSimulationSpatialRequestPayload Payload;
		FUeAgentRigSimulationSpatialPortRequest FormalPortRequest;
		UE::UAI::StateSemantic::FUeAgentCanonicalValueRef IntentPayload;
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialRequestDecoder(
		TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialRequestDecoder>& OutDecoder,
		FUeAgentRigSimulationSpatialExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryGetRigSimulationSpatialIdentityFields(
		const FString& CapabilityId,
		TArray<FString>& OutFields,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}
