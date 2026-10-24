#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"

#define UAI_TIMELINE_ANIMATION_TYPED_PORT_V1 1

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	enum class EUeAgentTimelineAnimationPortFamily : uint8
	{
		TemporalRead,
		AnimationKeyedData,
		TimelineStructure,
		SubjectAnimationState,
		TimelineRigCandidate,
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationPortRequest final
	{
	public:
		FUeAgentTimelineAnimationPortRequest(
			EUeAgentTimelineAnimationPortFamily InFamily,
			TArray<StateSemantic::FUeAgentTemporalPoint> InTemporalPoints,
			TArray<StateSemantic::FUeAgentTemporalRange> InTemporalRanges,
			TArray<StateSemantic::FUeAgentSpatialValuePtr> InSpatialValues,
			TSet<FString> InFrozenContextFacets,
			TArray<StateSemantic::FUeAgentTemporalConversion>
				InTemporalConversions = {});

		EUeAgentTimelineAnimationPortFamily GetFamily() const;
		const TArray<StateSemantic::FUeAgentTemporalPoint>&
		GetTemporalPoints() const;
		const TArray<StateSemantic::FUeAgentTemporalRange>&
		GetTemporalRanges() const;
		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialValues() const;
		const TSet<FString>& GetFrozenContextFacets() const;
		const TArray<StateSemantic::FUeAgentTemporalConversion>&
		GetTemporalConversions() const;

	private:
		EUeAgentTimelineAnimationPortFamily Family;
		TArray<StateSemantic::FUeAgentTemporalPoint> TemporalPoints;
		TArray<StateSemantic::FUeAgentTemporalRange> TemporalRanges;
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialValues;
		TSet<FString> FrozenContextFacets;
		TArray<StateSemantic::FUeAgentTemporalConversion>
			TemporalConversions;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationPortObservation final
	{
	public:
		FUeAgentTimelineAnimationPortObservation() = default;
		FUeAgentTimelineAnimationPortObservation(
			TArray<StateSemantic::FUeAgentTemporalPoint>
				InTemporalObservations,
			TArray<StateSemantic::FUeAgentTemporalRange>
				InTemporalRangeObservations,
			TArray<StateSemantic::FUeAgentSpatialValuePtr>
				InSpatialObservations,
			TArray<FString> InReadbackEvidence);

		const TArray<StateSemantic::FUeAgentTemporalPoint>&
		GetTemporalObservations() const;
		const TArray<StateSemantic::FUeAgentTemporalRange>&
		GetTemporalRangeObservations() const;
		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialObservations() const;
		const TArray<FString>& GetReadbackEvidence() const;

	private:
		TArray<StateSemantic::FUeAgentTemporalPoint> TemporalObservations;
		TArray<StateSemantic::FUeAgentTemporalRange>
			TemporalRangeObservations;
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialObservations;
		TArray<FString> ReadbackEvidence;
	};

	using FUeAgentTimelineAnimationPortError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationNativePort
	{
	public:
		virtual ~IUeAgentTimelineAnimationNativePort() = default;

		virtual bool TryExecute(
			const FUeAgentTimelineAnimationPortRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentTimelineAnimationPortObservation& OutObservation,
			FUeAgentTimelineAnimationPortError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryExecuteTimelineAnimationPort(
		const FUeAgentTimelineAnimationPortRequest& Request,
		IUeAgentTimelineAnimationNativePort& NativePort,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentTimelineAnimationPortObservation& OutObservation,
		FUeAgentTimelineAnimationPortError& OutError);
}
