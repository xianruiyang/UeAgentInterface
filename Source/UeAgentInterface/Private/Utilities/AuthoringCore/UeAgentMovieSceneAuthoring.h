#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"
#include "Misc/FrameRate.h"

class UMovieScene;
class UMovieSceneSequence;
class UMovieSceneTrack;
class UMovieSceneSection;

namespace UE::UAI::Authoring::Timeline
{
	UEAGENTINTERFACE_API bool TryQueryMovieSceneTrackCandidates(const FString& Target, const FString& Query,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryQueryMovieSceneTrackCandidates(const UMovieScene& Scene, const FString& Query,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError, const UObject* PropertyObject = nullptr, const FString& PropertyPath = {});
	struct FMovieSceneBindingIdentity
	{
		FString Kind;
		FString ObjectRef;
		TSharedPtr<FJsonObject> Properties;
		TSharedPtr<FJsonObject> PropertyDefinitions;
		TSharedPtr<FJsonObject> TemplateProperties;
		TSharedPtr<FJsonObject> TemplateDefinitions;
	};
	struct FMovieSceneObjectBinding
	{
		UObject* Object = nullptr;
		UObject* Context = nullptr;
		// Domains may install native binding content while sharing binding identity/track handling.
		UClass* BoundClass = nullptr;
		TFunction<bool(UMovieSceneSequence&, const FGuid&, UE::UAI::Contract::FDiagnostic&)> Install;
	};
	using FMovieSceneBindingEdit = TFunction<bool(UE::UAI::Contract::FDiagnostic&)>;
	using FMovieSceneBindingEditPreparer = TFunction<bool(const FTimelineAnimationBinding&, FMovieSceneBindingEdit&, UE::UAI::Contract::FDiagnostic&)>;
	using FMovieSceneBindingResolver = TFunction<bool(const FTimelineAnimationBinding&, FMovieSceneObjectBinding&, UE::UAI::Contract::FDiagnostic&)>;
	// Owned by one prepared operation for one MovieScene; never serialized into authoring input.
	struct FMovieSceneCreationState
	{
		TMap<FString, FGuid> BindingIds;
		TMap<FString, FGuid> ActorTemplateIds;
		TMap<FString, FName> ObjectNames;
		UEAGENTINTERFACE_API bool StabilizeObjectName(UObject& Object, const FString& LogicalKey);
	};
	struct UEAGENTINTERFACE_API FPreparedMovieSceneSettings
	{
		TOptional<FFrameRate> DisplayRate;
		TOptional<TRange<FFrameNumber>> PlaybackRange;
		bool HasChanges() const { return DisplayRate.IsSet() || PlaybackRange.IsSet(); }
		void Apply(UMovieScene& MovieScene) const;
	};
	UEAGENTINTERFACE_API bool TryPrepareMovieSceneSettings(const UMovieScene& MovieScene, const FString& ClockId,
		const TOptional<FTimelineAnimationRate>& DisplayRate, const TOptional<FTimelineAnimationRange>& PlaybackRange,
		FPreparedMovieSceneSettings& OutSettings, FUeAgentTimelineAnimationExecutionError& OutError);
	UEAGENTINTERFACE_API bool TryRate(const FTimelineAnimationRate& Rate, FFrameRate& OutRate,
		FUeAgentTimelineAnimationExecutionError& OutError);
	UEAGENTINTERFACE_API bool TryPointTick(const FTimelineAnimationPoint& Point,
		const FString& ClockId, const FFrameRate& Rate, int64& OutTick,
		FUeAgentTimelineAnimationExecutionError& OutError);
	UEAGENTINTERFACE_API bool TrySectionRange(const FTimelineAnimationRange& Value, const FString& ClockId,
		const FFrameRate& Rate, TRange<FFrameNumber>& OutRange,
		FUeAgentTimelineAnimationExecutionError& OutError);

	// Explicit authoring preserves editor lock state while executing the requested native range.
	UEAGENTINTERFACE_API bool TrySetMovieSceneSectionRange(UMovieSceneSection& Section,
		const TRange<FFrameNumber>& Range, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryReadMovieSceneStructure(const UMovieScene& MovieScene, const FString& ClockId,
		const TMap<FGuid, FMovieSceneBindingIdentity>& Identities, TSharedPtr<FJsonObject>& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError, bool bIncludeSequenceCollections = false);
	UEAGENTINTERFACE_API bool TryUpdateMovieSceneValues(UMovieScene& MovieScene,
		const FTimelineAnimationStructure& Current, const FTimelineAnimationStructure& Desired,
		FUeAgentTimelineAnimationExecutionError& OutError, UMovieSceneSequence* OwnerSequence = nullptr,
		const FMovieSceneBindingResolver& ResolveBinding = {}, FMovieSceneCreationState* CreationState = nullptr,
		const FMovieSceneBindingEditPreparer& PrepareBindingEdit = {});
}
