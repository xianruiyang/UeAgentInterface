#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#define UAI_TEMPORAL_CONTRACT_V1 1

namespace UE::UAI::StateSemantic
{
	enum class EUeAgentTemporalRoundingPolicy : uint8
	{
		Exact,
		Floor,
		Ceiling,
		NearestTiesToEven,
	};

	enum class EUeAgentTemporalBoundKind : uint8
	{
		Open,
		Inclusive,
		Exclusive,
	};

	enum class EUeAgentTemporalComparison : uint8
	{
		Before,
		Equivalent,
		After,
	};

	class UEAGENTINTERFACE_API FUeAgentTemporalRate final
	{
	public:
		FUeAgentTemporalRate(FString InNumerator, FString InDenominator);

		const FString& GetNumerator() const;
		const FString& GetDenominator() const;
		bool IsValid() const;
		bool SemanticallyEquals(const FUeAgentTemporalRate& Other) const;

	private:
		FString Numerator;
		FString Denominator;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentTemporalConversion final
	{
	public:
		FUeAgentTemporalConversion(
			FString InConversionId,
			FString InSourceClockId,
			FString InTargetClockId,
			FUeAgentTemporalRate InSourceRate,
			FUeAgentTemporalRate InTargetRate,
			FString InScaleNumerator,
			FString InScaleDenominator,
			FString InOffsetTargetTicksNumerator,
			FString InOffsetTargetTicksDenominator,
			FString InEvidence);

		const FString& GetConversionId() const;
		const FString& GetSourceClockId() const;
		const FString& GetTargetClockId() const;
		const FUeAgentTemporalRate& GetSourceRate() const;
		const FUeAgentTemporalRate& GetTargetRate() const;
		const FString& GetScaleNumerator() const;
		const FString& GetScaleDenominator() const;
		const FString& GetOffsetTargetTicksNumerator() const;
		const FString& GetOffsetTargetTicksDenominator() const;
		const FString& GetEvidence() const;
		bool IsReverse() const;
		bool IsValid() const;

	private:
		FString ConversionId;
		FString SourceClockId;
		FString TargetClockId;
		FUeAgentTemporalRate SourceRate;
		FUeAgentTemporalRate TargetRate;
		FString ScaleNumerator;
		FString ScaleDenominator;
		FString OffsetTargetTicksNumerator;
		FString OffsetTargetTicksDenominator;
		FString Evidence;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FUeAgentTemporalBound final
	{
	public:
		static FUeAgentTemporalBound Open();
		static FUeAgentTemporalBound Inclusive(FUeAgentTemporalPoint Point);
		static FUeAgentTemporalBound Exclusive(FUeAgentTemporalPoint Point);

		EUeAgentTemporalBoundKind GetKind() const;
		const TOptional<FUeAgentTemporalPoint>& GetPoint() const;
		bool IsValid() const;

	private:
		FUeAgentTemporalBound(
			EUeAgentTemporalBoundKind InKind,
			TOptional<FUeAgentTemporalPoint> InPoint);

		EUeAgentTemporalBoundKind Kind = EUeAgentTemporalBoundKind::Open;
		TOptional<FUeAgentTemporalPoint> Point;
	};

	class UEAGENTINTERFACE_API FUeAgentTemporalRange final
	{
	public:
		FUeAgentTemporalRange(
			FString InClockId,
			FString InState,
			FUeAgentTemporalBound InLowerBound,
			FUeAgentTemporalBound InUpperBound);

		const FString& GetClockId() const;
		const FString& GetState() const;
		const FUeAgentTemporalBound& GetLowerBound() const;
		const FUeAgentTemporalBound& GetUpperBound() const;
		bool IsValid() const;
		bool IsEmpty() const;

	private:
		FString ClockId;
		FString State;
		FUeAgentTemporalBound LowerBound;
		FUeAgentTemporalBound UpperBound;
		bool bValid = false;
		bool bEmpty = false;
	};

	UEAGENTINTERFACE_API bool TryCompareTemporalPoints(
		const FUeAgentTemporalPoint& Left,
		const FUeAgentTemporalPoint& Right,
		ContextResource::FUeAgentResourceTracker& Resources,
		EUeAgentTemporalComparison& OutComparison,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryConvertTemporalPoint(
		const FUeAgentTemporalPoint& Source,
		const FUeAgentTemporalConversion& Conversion,
		EUeAgentTemporalRoundingPolicy Rounding,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<FUeAgentTemporalPoint>& OutPoint,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryConvertTemporalRange(
		const FUeAgentTemporalRange& Source,
		const FUeAgentTemporalConversion& Conversion,
		EUeAgentTemporalRoundingPolicy LowerRounding,
		EUeAgentTemporalRoundingPolicy UpperRounding,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<FUeAgentTemporalRange>& OutRange,
		FUeAgentStateSemanticError& OutError);
}
