#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::Authoring::Timeline
{
	using FUeAgentTimelineAnimationExecutionError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FTimelineAnimationRate
	{
		FString Numerator;
		FString Denominator;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationPoint
	{
		FString ClockId;
		FString Tick;
		FTimelineAnimationRate Rate;
		FString State;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
		UE::UAI::StateSemantic::FUeAgentTemporalPoint ToTemporalPoint() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBound
	{
		FString Kind;
		TOptional<FTimelineAnimationPoint> Point;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationRange
	{
		FString ClockId;
		FString State;
		FTimelineAnimationBound Lower;
		FTimelineAnimationBound Upper;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationFrame3d
	{
		FString FrameId;
		FString BasisFrameId;
		int32 Dimension = 0;
		FString Unit;
		FString BasisEvidenceId;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationTransform3d
	{
		FString Subject;
		FString Quantity;
		FTimelineAnimationFrame3d Frame;
		TArray<FString> Components;
		TArray<FString> ConversionEvidenceIds;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
		bool TryToNativeTransform(
			const FString& NativeFrameId,
			FTransform& OutTransform,
			FUeAgentTimelineAnimationExecutionError& OutError) const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationKey
	{
		FString KeyRef;
		FTimelineAnimationPoint Time;
		FString ValueType;
		FString ValueLexeme;
		bool bHasValueLexeme = false;
		TSharedPtr<FJsonValue> Value;
		TOptional<FTimelineAnimationTransform3d> Transform;
		TSharedPtr<FJsonValue> Properties;
		TSharedPtr<FJsonObject> SourceRecord;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationChannel
	{
		FString ChannelRef;
		FString Name;
		FString ChannelKind;
		TArray<FTimelineAnimationKey> Keys;
		TOptional<bool> DefaultIsSet;
		FString DefaultValueLexeme;
		TSharedPtr<FJsonValue> DefaultValue;
		TSharedPtr<FJsonValue> Properties;
		TSharedPtr<FJsonObject> SourceRecord;
		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationSection
	{
		FString SectionRef;
		FString SectionKind;
		FTimelineAnimationRange Range;
		FString SourceRef;
		TArray<FTimelineAnimationKey> Keys;
		TArray<FTimelineAnimationChannel> Channels;
		TSharedPtr<FJsonValue> NativeChannels;
		TSharedPtr<FJsonObject> Properties;
		TSharedPtr<FJsonObject> SourceRecord;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationTrack
	{
		// Validated source retained only for complete deletion-conflict qualification.
		TSharedPtr<FJsonObject> SourceRecord;
		FString TrackRef;
		FString DisplayName;
		FString TrackKind;
		FString PropertyPath;
		TArray<FTimelineAnimationSection> Sections;
		TSharedPtr<FJsonObject> Properties;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FTimelineAnimationBinding
	{
		TSharedPtr<FJsonObject> SourceRecord;
		TSharedPtr<FJsonObject> Properties;
		TSharedPtr<FJsonObject> TemplateProperties;
		FString BindingRef;
		FString BindingKind;
		FString ObjectRef;
		// Absent preserves local membership; an explicit empty set clears it.
		TOptional<TArray<FString>> Tags;
		TArray<FTimelineAnimationTrack> Tracks;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	struct FTimelineAnimationItemReference
	{
		FString Key;
		bool bNew = false;
		bool operator==(const FTimelineAnimationItemReference& Other) const { return Key == Other.Key && bNew == Other.bNew; }
	};
	struct FTimelineAnimationFolder
	{
		FTimelineAnimationItemReference Ref, Parent;
		FString Name;
		TArray<FTimelineAnimationItemReference> Bindings, RootTracks;
		TSharedPtr<FJsonObject> SourceRecord;
	};
	struct UEAGENTINTERFACE_API FTimelineAnimationStructure
	{
		FTimelineAnimationRate DisplayRate;
		FTimelineAnimationRate TickResolution;
		FTimelineAnimationRange PlaybackRange;
		TArray<FTimelineAnimationBinding> Bindings;
		TArray<FTimelineAnimationTrack> RootTracks;
		TOptional<TArray<FTimelineAnimationFolder>> Folders;

		bool TryValidate(FUeAgentTimelineAnimationExecutionError& OutError) const;
		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool ParseRate(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationRate& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParsePoint(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationPoint& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseBound(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationBound& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseRange(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationRange& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseFrame(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationFrame3d& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTransform(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationTransform3d& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTimelineKey(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationKey& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTimelineChannel(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationChannel& Out, FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTimelineSection(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationSection& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTimelineTrack(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationTrack& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseTimelineBinding(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationBinding& Out,
		FUeAgentTimelineAnimationExecutionError& Error);

	UEAGENTINTERFACE_API bool ParseStructure(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationStructure& Out,
		FUeAgentTimelineAnimationExecutionError& Error);
}
