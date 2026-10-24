#pragma once
#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"
class UAnimSequence;
namespace UE::UAI::Authoring::Timeline {
	struct UEAGENTINTERFACE_API FTimelineAnimationCurveKey
	{
		FTimelineAnimationPoint Time;
		double Value = 0.0;
		FString Interpolation;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationCurveChange
	{
		FString CurveRef;
		FString Operation;
		TArray<FTimelineAnimationCurveKey> Keys;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationMetadataChange
	{
		FString MetadataRef;
		FString Operation;
		FString ValueType;
		FString ValueLexeme;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
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

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

UEAGENTINTERFACE_API bool TryPointSeconds(const FTimelineAnimationPoint& Point, double& Seconds, FUeAgentTimelineAnimationExecutionError& Error);
struct FSequenceChanges {
 TArray<FTimelineAnimationCurveChange> CurveChanges;
 TArray<FTimelineAnimationMetadataChange> MetadataChanges;
 TArray<FTimelineAnimationEventChange> EventChanges;
 TSharedPtr<FJsonObject> SourceRecord;
};
UEAGENTINTERFACE_API bool ParseCurveChange(const TSharedPtr<FJsonObject>& Root, FTimelineAnimationCurveChange& Out, FUeAgentTimelineAnimationExecutionError& Error);
UEAGENTINTERFACE_API bool ParseMetadataChange(const TSharedPtr<FJsonObject>& Root, FTimelineAnimationMetadataChange& Out, FUeAgentTimelineAnimationExecutionError& Error);
UEAGENTINTERFACE_API bool ParseEventChange(const TSharedPtr<FJsonObject>& Root, FTimelineAnimationEventChange& Out, FUeAgentTimelineAnimationExecutionError& Error);
UEAGENTINTERFACE_API bool ParseSequenceChanges(const TSharedPtr<FJsonObject>& Root, FSequenceChanges& Out, FUeAgentTimelineAnimationExecutionError& Error);
UEAGENTINTERFACE_API bool ApplySequenceCurves(UAnimSequence& Sequence, const FSequenceChanges& Data, FUeAgentTimelineAnimationExecutionError& Error, const FString& CanonicalObjectPath = FString());
UEAGENTINTERFACE_API bool ApplySequenceMetadata(UAnimSequence& Sequence, const FSequenceChanges& Data, FUeAgentTimelineAnimationExecutionError& Error, const FString& CanonicalObjectPath = FString());
}
