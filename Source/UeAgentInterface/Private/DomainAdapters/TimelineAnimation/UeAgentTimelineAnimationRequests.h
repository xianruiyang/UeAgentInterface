#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "Templates/SharedPointer.h"

#define UAI_TIMELINE_ANIMATION_TYPED_REQUESTS_V1 1

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	struct UEAGENTINTERFACE_API FTimelineAnimationRevision
	{
		FString SourceId;
		FString Generation;
		int64 Sequence = -1;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationRate
	{
		FString Numerator;
		FString Denominator;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationPoint
	{
		FString ClockId;
		FString Tick;
		FTimelineAnimationRate Rate;
		FString State;

		bool IsValid() const;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint ToTemporalPoint() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBound
	{
		FString Kind;
		TOptional<FTimelineAnimationPoint> Point;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationRange
	{
		FString ClockId;
		FString State;
		FTimelineAnimationBound Lower;
		FTimelineAnimationBound Upper;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationFrame3d
	{
		FString FrameId;
		FString BasisFrameId;
		int32 Dimension = 0;
		FString Unit;
		FString BasisEvidenceId;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationTransform3d
	{
		FString Subject;
		FString Quantity;
		FTimelineAnimationFrame3d Frame;
		TArray<FString> Components;
		TArray<FString> ConversionEvidenceIds;

		bool IsValid() const;
		bool TryToNativeTransform(
			const FString& NativeFrameId,
			FTransform& OutTransform,
			FUeAgentTimelineAnimationExecutionError& OutError) const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBoneKey
	{
		FTimelineAnimationPoint Time;
		FTimelineAnimationTransform3d Transform;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBoneTrack
	{
		FString BoneRef;
		TArray<FTimelineAnimationBoneKey> Keys;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationCurveKey
	{
		FTimelineAnimationPoint Time;
		double Value = 0.0;
		FString Interpolation;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationCurveChange
	{
		FString CurveRef;
		FString Operation;
		TArray<FTimelineAnimationCurveKey> Keys;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationMetadataChange
	{
		FString MetadataRef;
		FString Operation;
		FString ValueType;
		FString ValueLexeme;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationEventChange
	{
		FString EventRef;
		FString EventKind;
		FString Operation;
		TOptional<FTimelineAnimationPoint> Time;
		TOptional<FTimelineAnimationRange> Range;
		FString PayloadClassRef;
		FString TrackRef;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBlendParameter
	{
		FString AxisRef;
		double Value = 0.0;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationKey
	{
		FString KeyRef;
		FTimelineAnimationPoint Time;
		FString ValueType;
		FString ValueLexeme;
		TOptional<FTimelineAnimationTransform3d> Transform;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationSection
	{
		FString SectionRef;
		FString SectionKind;
		FTimelineAnimationRange Range;
		FString SourceRef;
		TArray<FTimelineAnimationKey> Keys;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationTrack
	{
		FString TrackRef;
		FString TrackKind;
		FString PropertyPath;
		TArray<FTimelineAnimationSection> Sections;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBinding
	{
		FString BindingRef;
		FString BindingKind;
		FString ObjectRef;
		TArray<FTimelineAnimationTrack> Tracks;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationStructure
	{
		FTimelineAnimationRate DisplayRate;
		FTimelineAnimationRate TickResolution;
		FTimelineAnimationRange PlaybackRange;
		TArray<FTimelineAnimationBinding> Bindings;
		TArray<FTimelineAnimationTrack> RootTracks;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationPoseItem
	{
		FString BoneRef;
		FTimelineAnimationTransform3d Transform;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationMorphWeight
	{
		FString MorphRef;
		double Weight = 0.0;

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
		LevelSequenceStructureReplace,
		WidgetAnimationRead,
		SubjectPoseUpdate,
		SubjectMorphWeightsUpdate,
		IkProbeCandidateApply,
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationRequestData
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
		TArray<FTimelineAnimationCurveChange> CurveChanges;
		TArray<FTimelineAnimationMetadataChange> MetadataChanges;
		TArray<FTimelineAnimationEventChange> EventChanges;
		TArray<FTimelineAnimationPoseItem> Pose;
		TArray<FTimelineAnimationMorphWeight> MorphWeights;
		FString RootBoneRef;
		FString DestinationRef;
		FString SkeletonRef;
		FString SourceAnimationRef;
		FString RigRef;
		FString GoalRef;

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
