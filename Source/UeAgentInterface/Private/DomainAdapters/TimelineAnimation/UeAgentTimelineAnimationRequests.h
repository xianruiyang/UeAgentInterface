#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"
#include "Utilities/AuthoringCore/UeAgentAnimationSequenceChanges.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "Templates/SharedPointer.h"

class FJsonValue;

#define UAI_TIMELINE_ANIMATION_TYPED_REQUESTS_V1 1

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	struct UEAGENTINTERFACE_API FTimelineAnimationRevision
	{
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	using Authoring::Timeline::FTimelineAnimationRate;

	using Authoring::Timeline::FTimelineAnimationPoint;

	using Authoring::Timeline::FTimelineAnimationBound;

	using Authoring::Timeline::FTimelineAnimationRange;

	using Authoring::Timeline::FTimelineAnimationFrame3d;

	using Authoring::Timeline::FTimelineAnimationTransform3d;

	struct UEAGENTINTERFACE_API FTimelineAnimationBoneKey
	{
		FTimelineAnimationPoint Time;
		FTimelineAnimationTransform3d Transform;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBoneTrack
	{
		FString BoneRef;
		TArray<FTimelineAnimationBoneKey> Keys;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	using Authoring::Timeline::FTimelineAnimationCurveKey;
	using Authoring::Timeline::FTimelineAnimationCurveChange;
	using Authoring::Timeline::FTimelineAnimationMetadataChange;
	using Authoring::Timeline::FTimelineAnimationEventChange;

	struct UEAGENTINTERFACE_API FTimelineAnimationBlendParameter
	{
		FString AxisRef;
		double Value = 0.0;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	using Authoring::Timeline::FTimelineAnimationKey;

	using Authoring::Timeline::FTimelineAnimationChannel;

	using Authoring::Timeline::FTimelineAnimationSection;

	using Authoring::Timeline::FTimelineAnimationTrack;

	using Authoring::Timeline::FTimelineAnimationBinding;

	using Authoring::Timeline::FTimelineAnimationStructure;

	UEAGENTINTERFACE_API bool TryParseTimelineAnimationStructure(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationStructure& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError);

	struct UEAGENTINTERFACE_API FTimelineAnimationPoseItem
	{
		FString BoneRef;
		FTimelineAnimationTransform3d Transform;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationMorphWeight
	{
		FString MorphRef;
		double Weight = 0.0;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	enum class EUeAgentTimelineAnimationRequestKind : uint8
	{
		SequenceRead,
		SequenceRootMotionAnalyze,
		SequencePoseSample,
		BlendSpaceSample,
		SequenceBoneTracksReplace,
		SequenceCurvesUpdate,
		SequenceMetadataUpdate,
		SequenceEventsUpdate,
		MontageRead,
		MontageCreate,
		MontageStructureReplace,
		SkeletonSlotsRead,
		LevelSequenceRead,
		LevelSequenceCreate,
		LevelSequenceSettingsUpdate,
		LevelSequenceStructureReplace,
		WidgetAnimationRead,
		SubjectPoseUpdate,
		SubjectMorphWeightsUpdate,
		IkProbeCandidateApply,
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationRequestData : Authoring::Timeline::FSequenceChanges
	{
		TOptional<FTimelineAnimationRange> TemporalRange;
		TOptional<FTimelineAnimationRate> SampleRate;
		TOptional<FTimelineAnimationPoint> TimePoint;
		TOptional<FTimelineAnimationPoint> EvaluationPoint;
		TOptional<FTimelineAnimationPoint> AuthoredTime;
		TOptional<FTimelineAnimationFrame3d> OutputFrame;
		TOptional<FTimelineAnimationFrame3d> PoseFrame;
		TOptional<FTimelineAnimationTransform3d> Candidate;
		TOptional<FTimelineAnimationStructure> Structure;
		TOptional<FTimelineAnimationRevision> ExpectedRevision;
		TOptional<FTimelineAnimationRate> DisplayRate;
		TOptional<FTimelineAnimationRate> TickResolution;
		TOptional<double> Weight;
		TOptional<bool> IncludeTracks;
		TOptional<bool> IncludeKeys;
		TOptional<bool> IncludeSections;
		TOptional<bool> IncludeSlots;
		TOptional<bool> IncludeBindings;
		TArray<FString> BoneRefs;
		TArray<FTimelineAnimationBlendParameter> BlendParameters;
		TArray<FTimelineAnimationBoneTrack> BoneTracks;
		TArray<FTimelineAnimationPoseItem> Pose;
		TArray<FTimelineAnimationMorphWeight> MorphWeights;
		FString RootBoneRef;
		FString DestinationRef;
		FString SkeletonRef;
		FString SourceAnimationRef;
		FString RigRef;
		FString GoalRef;

		bool TryValidateFor(EUeAgentTimelineAnimationRequestKind Kind, FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValidFor(EUeAgentTimelineAnimationRequestKind Kind) const;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationRequest final
		: public IUeAgentTimelineAnimationTypedRequest
	{
	public:
		FUeAgentTimelineAnimationRequest(
			FString InTypeId,
			FString InFamilyId,
			FString InRequestKindId,
			EUeAgentTimelineAnimationRequestKind InKind,
			bool bInOperation,
			FTimelineAnimationRequestData InData,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef InIntentPayload);

		const FString& GetTypeId() const override;
		const FString& GetFamilyId() const override;
		const FString& GetRequestKindId() const override;
		EUeAgentTimelineAnimationRequestKind GetKind() const;
		bool IsOperation() const;
		const FTimelineAnimationRequestData& GetData() const;
		const UE::UAI::StateSemantic::FUeAgentCanonicalValueRef&
			GetIntentPayload() const;
		bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentTimelineAnimationExecutionError& OutError) const override;

	private:
		FString TypeId;
		FString FamilyId;
		FString RequestKindId;
		EUeAgentTimelineAnimationRequestKind Kind;
		bool bOperation = false;
		FTimelineAnimationRequestData Data;
		UE::UAI::StateSemantic::FUeAgentCanonicalValueRef IntentPayload;
	};

	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationRequestDecoder(
		TSharedPtr<IUeAgentTimelineAnimationRequestDecoder>& OutDecoder,
		FUeAgentTimelineAnimationExecutionError& OutError);
}
