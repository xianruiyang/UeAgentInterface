#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialRequests.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class ULevelSequence;
class UIKRigDefinition;
class UIKRetargeter;
class USkeleton;
class USkeletalMeshComponent;
class UWorld;

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialSkeletonBinding
	{
		FString Identity;
		TWeakObjectPtr<USkeleton> Skeleton;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialSocketBinding
	{
		FString SkeletonIdentity;
		FString SocketIdentity;
		FName SocketName;
		FString ParentBoneIdentity;
		FName ParentBoneName;
		FString NativeParentFrameId;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialRigBakeBinding
	{
		FString Identity;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ULevelSequence> Sequence;
		TWeakObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;
		FGuid BindingGuid;
		FString NativeAuthoredClockId;
		FString NativeControlFrameId;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialNativeEnvironment
		: public IUeAgentRigSimulationSpatialRequestEnvironment
	{
	public:
		virtual UObject* ResolveExplicitObject(const FString& Identity) const = 0;
		virtual const FUeAgentRigSimulationSpatialSkeletonBinding*
			FindSkeletonBinding(const FString& Identity) const = 0;
		virtual const FUeAgentRigSimulationSpatialSocketBinding*
			FindSocketBinding(
				const FString& SkeletonIdentity,
				const FString& SocketIdentity) const = 0;
		virtual const FUeAgentRigSimulationSpatialRigBakeBinding*
			FindRigBakeBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialIkRigBinding*
			FindIkRigBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialRetargeterBinding*
			FindRetargeterBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialPhysicsAssetBinding*
			FindPhysicsAssetBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialPhysicsSubjectBinding*
			FindPhysicsSubjectBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialDeformerGeometryBinding*
			FindDeformerGeometryBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialDeformerTrainingInputBinding*
			FindDeformerTrainingInputBinding(const FString& Identity) const = 0;
		virtual const struct FUeAgentRigSimulationSpatialDeformerEvaluationBinding*
			FindDeformerEvaluationBinding(const FString& Identity) const = 0;
		virtual bool TryConvertSpatialValueToFrame(
			const UE::UAI::StateSemantic::FUeAgentSpatialValue& Value,
			const FString& TargetFrameId,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			UE::UAI::StateSemantic::FUeAgentSpatialValuePtr& OutValue,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
		virtual bool TryConvertTemporalRangeToClock(
			const UE::UAI::StateSemantic::FUeAgentTemporalRange& Range,
			const FString& TargetClockId,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TOptional<UE::UAI::StateSemantic::FUeAgentTemporalRange>& OutRange,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
		virtual bool TryConvertTemporalPointToClock(
			const UE::UAI::StateSemantic::FUeAgentTemporalPoint& Point,
			const FString& TargetClockId,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			TOptional<UE::UAI::StateSemantic::FUeAgentTemporalPoint>& OutPoint,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
		virtual void RecordMutation(const FString& OperationId, UObject* Object) = 0;
		virtual UObject* FindMutation(const FString& OperationId) const = 0;
		virtual void CountFamilyDispatch() = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialNativePorts
	{
		TSet<FString> SupportedCapabilities;
		TSharedPtr<IUeAgentRigSimulationSpatialReadbackPort> Readback;
		TSharedPtr<IUeAgentRigSimulationSpatialMutationPort> Mutation;
		TSharedPtr<IUeAgentRigSimulationSpatialSettlePort> Settle;
		TSharedPtr<IUeAgentRigSimulationSpatialResultComparator> Comparator;

		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
		bool Supports(const FString& CapabilityId) const;
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialSkeletalRigBakeNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryComposeRigSimulationSpatialNativePorts(
		TArray<FUeAgentRigSimulationSpatialNativePorts> FamilyPorts,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}
