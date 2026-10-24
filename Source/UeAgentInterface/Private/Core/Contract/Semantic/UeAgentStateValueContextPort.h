#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/ExecutionContext/UeAgentFrozenExecutionContext.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"

namespace UE::UAI::SemanticContract
{
	using FUeAgentStateValueContextError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentStateValueContextPort
	{
	public:
		virtual ~IUeAgentStateValueContextPort() = default;

		virtual bool ValidateEntry(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentStateEntry& Entry,
			FUeAgentStateValueContextError& OutError) const = 0;

		virtual bool TryCreateSnapshot(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const FString& SnapshotId,
			const StateSemantic::FUeAgentStateRevision& Revision,
			const StateSemantic::FUeAgentTemporalPoint& CapturedAt,
			const StateSemantic::FUeAgentStateProvenance& Provenance,
			const TArray<StateSemantic::FUeAgentStateEntry>& Entries,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentStateValueContextError& OutError) const = 0;

		virtual bool TryCompareSnapshots(
			const StateSemantic::FUeAgentCanonicalSnapshotRef& Left,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& Right,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentSemanticComparison& OutComparison,
			FUeAgentStateValueContextError& OutError) const = 0;

		virtual bool TryConvertTemporalValue(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentTemporalPoint& Source,
			const StateSemantic::FUeAgentTemporalConversion& Conversion,
			StateSemantic::EUeAgentTemporalRoundingPolicy Rounding,
			ContextResource::FUeAgentResourceTracker& Resources,
			TOptional<StateSemantic::FUeAgentTemporalPoint>& OutPoint,
			FUeAgentStateValueContextError& OutError) const = 0;

		virtual bool TryConvertSpatialValue(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentSpatialValue& Value,
			const StateSemantic::FUeAgentSpatialFrame& TargetFrame,
			const StateSemantic::FUeAgentSpatialConversion& Conversion,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentSpatialValuePtr& OutValue,
			FUeAgentStateValueContextError& OutError) const = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentStateValueContextPort final
		: public IUeAgentStateValueContextPort
	{
	public:
		virtual bool ValidateEntry(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentStateEntry& Entry,
			FUeAgentStateValueContextError& OutError) const override;

		virtual bool TryCreateSnapshot(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const FString& SnapshotId,
			const StateSemantic::FUeAgentStateRevision& Revision,
			const StateSemantic::FUeAgentTemporalPoint& CapturedAt,
			const StateSemantic::FUeAgentStateProvenance& Provenance,
			const TArray<StateSemantic::FUeAgentStateEntry>& Entries,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FUeAgentStateValueContextError& OutError) const override;

		virtual bool TryCompareSnapshots(
			const StateSemantic::FUeAgentCanonicalSnapshotRef& Left,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& Right,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentSemanticComparison& OutComparison,
			FUeAgentStateValueContextError& OutError) const override;

		virtual bool TryConvertTemporalValue(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentTemporalPoint& Source,
			const StateSemantic::FUeAgentTemporalConversion& Conversion,
			StateSemantic::EUeAgentTemporalRoundingPolicy Rounding,
			ContextResource::FUeAgentResourceTracker& Resources,
			TOptional<StateSemantic::FUeAgentTemporalPoint>& OutPoint,
			FUeAgentStateValueContextError& OutError) const override;

		virtual bool TryConvertSpatialValue(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			const StateSemantic::FUeAgentSpatialValue& Value,
			const StateSemantic::FUeAgentSpatialFrame& TargetFrame,
			const StateSemantic::FUeAgentSpatialConversion& Conversion,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentSpatialValuePtr& OutValue,
			FUeAgentStateValueContextError& OutError) const override;
	};
}
