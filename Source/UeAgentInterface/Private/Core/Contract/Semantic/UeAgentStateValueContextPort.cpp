#include "Core/Contract/Semantic/UeAgentStateValueContextPort.h"

namespace UE::UAI::SemanticContract
{
	namespace
	{
		void Reset(FUeAgentStateValueContextError& OutError)
		{
			OutError = {};
		}

		void CopyError(
			const StateSemantic::FUeAgentStateSemanticError& Source,
			FUeAgentStateValueContextError& OutError)
		{
			OutError.Code = Source.Code;
			OutError.Detail = Source.Detail;
		}

		bool ValidateContext(
			const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
			FUeAgentStateValueContextError& OutError)
		{
			if (Context.IsValid())
			{
				return true;
			}
			OutError.Code = TEXT("state_value_context_invalid");
			OutError.Detail = TEXT("the request execution context is not frozen");
			return false;
		}
	}

	bool FUeAgentStateValueContextPort::ValidateEntry(
		const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
		const StateSemantic::FUeAgentStateEntry& Entry,
		FUeAgentStateValueContextError& OutError) const
	{
		Reset(OutError);
		if (!ValidateContext(Context, OutError))
		{
			return false;
		}
		if (!Entry.IsValid())
		{
			OutError.Code = TEXT("state_value_entry_invalid");
			OutError.Detail =
				TEXT("identity, type, value, default, presence and override must agree");
			return false;
		}
		return true;
	}

	bool FUeAgentStateValueContextPort::TryCreateSnapshot(
		const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
		const FString& SnapshotId,
		const StateSemantic::FUeAgentStateRevision& Revision,
		const StateSemantic::FUeAgentTemporalPoint& CapturedAt,
		const StateSemantic::FUeAgentStateProvenance& Provenance,
		const TArray<StateSemantic::FUeAgentStateEntry>& Entries,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateValueContextError& OutError) const
	{
		Reset(OutError);
		OutSnapshot.Reset();
		if (!ValidateContext(Context, OutError))
		{
			return false;
		}
		for (const StateSemantic::FUeAgentStateEntry& Entry : Entries)
		{
			if (!ValidateEntry(Context, Entry, OutError))
			{
				return false;
			}
		}

		StateSemantic::FUeAgentStateSemanticError SemanticError;
		if (!StateSemantic::FUeAgentCanonicalSnapshot::TryCreate(
				SnapshotId,
				Context.GetStatePlane(),
				Revision,
				CapturedAt,
				Provenance,
				Entries,
				Resources,
				OutSnapshot,
				SemanticError))
		{
			CopyError(SemanticError, OutError);
			return false;
		}
		return true;
	}

	bool FUeAgentStateValueContextPort::TryCompareSnapshots(
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Left,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Right,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateValueContextError& OutError) const
	{
		Reset(OutError);
		StateSemantic::FUeAgentStateSemanticError SemanticError;
		if (!StateSemantic::TryCompareSnapshots(
				Left,
				Right,
				Resources,
				OutComparison,
				SemanticError))
		{
			CopyError(SemanticError, OutError);
			return false;
		}
		return true;
	}

	bool FUeAgentStateValueContextPort::TryConvertTemporalValue(
		const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
		const StateSemantic::FUeAgentTemporalPoint& Source,
		const StateSemantic::FUeAgentTemporalConversion& Conversion,
		const StateSemantic::EUeAgentTemporalRoundingPolicy Rounding,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<StateSemantic::FUeAgentTemporalPoint>& OutPoint,
		FUeAgentStateValueContextError& OutError) const
	{
		Reset(OutError);
		OutPoint.Reset();
		if (!ValidateContext(Context, OutError))
		{
			return false;
		}
		StateSemantic::FUeAgentStateSemanticError SemanticError;
		if (!StateSemantic::TryConvertTemporalPoint(
				Source,
				Conversion,
				Rounding,
				Resources,
				OutPoint,
				SemanticError))
		{
			CopyError(SemanticError, OutError);
			return false;
		}
		return true;
	}

	bool FUeAgentStateValueContextPort::TryConvertSpatialValue(
		const ExecutionContext::FUeAgentFrozenExecutionContext& Context,
		const StateSemantic::FUeAgentSpatialValue& Value,
		const StateSemantic::FUeAgentSpatialFrame& TargetFrame,
		const StateSemantic::FUeAgentSpatialConversion& Conversion,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentSpatialValuePtr& OutValue,
		FUeAgentStateValueContextError& OutError) const
	{
		Reset(OutError);
		OutValue.Reset();
		if (!ValidateContext(Context, OutError))
		{
			return false;
		}
		StateSemantic::FUeAgentStateSemanticError SemanticError;
		if (!StateSemantic::TryConvertSpatialValue(
				Value,
				TargetFrame,
				Conversion,
				Resources,
				OutValue,
				SemanticError))
		{
			CopyError(SemanticError, OutError);
			return false;
		}
		return true;
	}
}
