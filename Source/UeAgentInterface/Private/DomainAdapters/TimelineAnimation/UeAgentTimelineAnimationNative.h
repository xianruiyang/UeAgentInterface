#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UControlRig;
class ULevelSequence;
class FJsonObject;
class UPoseableMeshComponent;
class USkeletalMeshComponent;
namespace UE::UAI::Authoring::Timeline { struct FMovieSceneCreationState; }

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
		bool TryValidate(Contract::FDiagnostic& OutError) const;
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
		bool TryValidate(Contract::FDiagnostic& OutError) const;
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

	UEAGENTINTERFACE_API bool TryApplyLevelSequenceStructure(
		ULevelSequence& Sequence,
		const FTimelineAnimationStructure& Structure,
		const FString& AuthoredObjectPath,
		FUeAgentTimelineAnimationExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryReadLevelSequenceStructure(
		const ULevelSequence& Sequence,
		const FString& AuthoredObjectPath,
		TSharedPtr<FJsonObject>& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError);
	UEAGENTINTERFACE_API bool TryUpdateLevelSequenceValues(
		ULevelSequence& Sequence,
		const FTimelineAnimationStructure& Structure,
		const FString& AuthoredObjectPath,
		FUeAgentTimelineAnimationExecutionError& OutError,
		Authoring::Timeline::FMovieSceneCreationState* CreationState = nullptr);
	UEAGENTINTERFACE_API bool TryQueryLevelSequenceTrackCandidates(
		const FString& Target, const FString& Query, const FString& ObjectRef,
		const FString& PropertyPath, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError);

	// Read-only resolver summary, independent of the editable binding kind.
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> ReadLevelSequenceBindingResolverSummary(
		const ULevelSequence& Sequence);

	// Apply-only verification of metadata outside the ordinary authoring structure.
	UEAGENTINTERFACE_API bool TryReadLevelSequenceBindingMetadataState(
		const ULevelSequence& Sequence, TSharedPtr<FJsonObject>& OutState,
		FUeAgentTimelineAnimationExecutionError& OutError);
	UEAGENTINTERFACE_API bool TryReadLevelSequencePersistentState(
		const ULevelSequence& Sequence,
		TSharedPtr<FJsonObject>& OutState,
		FUeAgentTimelineAnimationExecutionError& OutError);
}
