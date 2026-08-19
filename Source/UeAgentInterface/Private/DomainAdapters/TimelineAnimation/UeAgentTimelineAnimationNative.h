#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UControlRig;
class ULevelSequence;
class UPoseableMeshComponent;
class USkeletalMeshComponent;

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationSubjectBinding
	{
		FString Identity;
		TWeakObjectPtr<UPoseableMeshComponent> PoseComponent;
		TWeakObjectPtr<USkeletalMeshComponent> MorphComponent;
		FString NativeFrameId;
		FString EvaluationClockId;
		FString EvaluationRateNumerator;
		FString EvaluationRateDenominator;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationRigCandidateBinding
	{
		FString Identity;
		TWeakObjectPtr<ULevelSequence> Sequence;
		TWeakObjectPtr<UControlRig> Rig;
		FString RigIdentity;
		FName ControlName;
		FString NativeFrameId;
		FString AuthoredClockId;
		FString AuthoredRateNumerator;
		FString AuthoredRateDenominator;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationRevisionSource
	{
	public:
		virtual ~IUeAgentTimelineAnimationRevisionSource() = default;
		virtual bool TryReadRevision(
			const FString& ExplicitRef,
			FTimelineAnimationRevision& OutRevision,
			FUeAgentTimelineAnimationExecutionError& OutError) const = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationEnvironment
	{
	public:
		virtual ~IUeAgentTimelineAnimationEnvironment() = default;
		virtual void GetExplicitSubjectIdentities(TArray<FString>& OutIds) const = 0;
		virtual void GetExplicitRigCandidateIdentities(TArray<FString>& OutIds) const = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationNativeDependencies
	{
		TSharedPtr<IUeAgentTimelineAnimationEnvironment> Environment;
		TSharedPtr<IUeAgentTimelineAnimationRevisionSource> RevisionSource;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationNativePorts
	{
		TSharedPtr<IUeAgentTimelineAnimationReadbackPort> Readback;
		TSharedPtr<IUeAgentTimelineAnimationMutationPort> Mutation;
		TSharedPtr<IUeAgentTimelineAnimationSettlePort> Settle;
		TSharedPtr<IUeAgentTimelineAnimationResultComparator> Comparator;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationProductionDependencies(
		const FString& RuntimeGeneration,
		const FString& ServerSessionId,
		const TArray<FUeAgentTimelineAnimationSubjectBinding>& SubjectBindings,
		const TArray<FUeAgentTimelineAnimationRigCandidateBinding>& RigBindings,
		FUeAgentTimelineAnimationNativeDependencies& OutDependencies,
		FUeAgentTimelineAnimationExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationNativePorts(
		const FUeAgentTimelineAnimationNativeDependencies& Dependencies,
		FUeAgentTimelineAnimationNativePorts& OutPorts,
		FUeAgentTimelineAnimationExecutionError& OutError);
}
