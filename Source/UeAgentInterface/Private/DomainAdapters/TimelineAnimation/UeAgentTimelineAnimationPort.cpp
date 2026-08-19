#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationPort.h"

#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationContract.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentTimelineAnimationPortError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Path = MoveTemp(Path);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		const TCHAR* FamilyId(EUeAgentTimelineAnimationPortFamily Family)
		{
			switch (Family)
			{
			case EUeAgentTimelineAnimationPortFamily::TemporalRead:
				return TEXT("temporal_read");
			case EUeAgentTimelineAnimationPortFamily::AnimationKeyedData:
				return TEXT("animation_keyed_data");
			case EUeAgentTimelineAnimationPortFamily::TimelineStructure:
				return TEXT("timeline_structure");
			case EUeAgentTimelineAnimationPortFamily::SubjectAnimationState:
				return TEXT("subject_animation_state");
			case EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate:
				return TEXT("timeline_rig_candidate");
			default:
				return TEXT("");
			}
		}

		const FUeAgentTimelineAnimationPortFamilyContract* FindContract(
			EUeAgentTimelineAnimationPortFamily Family)
		{
			const FString Wanted = FamilyId(Family);
			for (const FUeAgentTimelineAnimationPortFamilyContract& Contract
				: GetTimelineAnimationPortFamilyContracts())
			{
				if (Contract.GetFamilyId() == Wanted)
				{
					return &Contract;
				}
			}
			return nullptr;
		}

		bool TryCharge(
			FUeAgentResourceTracker& Resources,
			int64 Items,
			const FString& Path,
			FUeAgentTimelineAnimationPortError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(
					{{EUeAgentResourceDimension::Items, Items},
				 {EUeAgentResourceDimension::WorkUnits, Items + 1}},
					Path,
					BudgetError))
			{
				return true;
			}
			return Fail(
				OutError,
				BudgetError.Code.IsEmpty()
					? TEXT("timeline_animation_resource_budget_exceeded")
					: BudgetError.Code,
				BudgetError.Path,
				LexToString(BudgetError.Dimension));
		}

		bool ValidateTemporalInputs(
			const FUeAgentTimelineAnimationPortRequest& Request,
			FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0;
				Index < Request.GetTemporalPoints().Num();
				++Index)
			{
				const FUeAgentTemporalPoint& Point =
					Request.GetTemporalPoints()[Index];
				const FString Path = FString::Printf(
					TEXT("temporal_points[%d]"), Index);
				if (!Point.IsValid())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_temporal_point_invalid"),
						Path);
				}
				if (Point.GetState() != TEXT("authored"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_authored_time_required"),
						Path + TEXT(".state"),
						Point.GetState());
				}
			}
			for (int32 Index = 0;
				Index < Request.GetTemporalRanges().Num();
				++Index)
			{
				const FUeAgentTemporalRange& Range =
					Request.GetTemporalRanges()[Index];
				const FString Path = FString::Printf(
					TEXT("temporal_ranges[%d]"), Index);
				if (!Range.IsValid())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_temporal_range_invalid"),
						Path);
				}
				if (Range.GetState() != TEXT("authored"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_authored_time_required"),
						Path + TEXT(".state"),
						Range.GetState());
				}
			}
			for (int32 Index = 0;
				Index < Request.GetTemporalConversions().Num();
				++Index)
			{
				if (!Request.GetTemporalConversions()[Index].IsValid())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_temporal_conversion_invalid"),
						FString::Printf(
							TEXT("temporal_conversions[%d]"), Index));
				}
			}
			return true;
		}

		bool ValidateSpatialValues(
			const TArray<FUeAgentSpatialValuePtr>& Values,
			EUeAgentTimelineAnimationPortFamily Family,
			const FString& PathPrefix,
			FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FUeAgentSpatialValuePtr& Value = Values[Index];
				const FString Path = FString::Printf(
					TEXT("%s[%d]"), *PathPrefix, Index);
				if (!Value.IsValid() || !Value->IsValid())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_spatial_value_invalid"),
						Path);
				}
				if (Family
						== EUeAgentTimelineAnimationPortFamily::
							TimelineRigCandidate
					&& (Value->GetFrame().GetDimension() != 3
						|| Value->GetQuantityKind()
							!= EUeAgentSpatialQuantityKind::Transform))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_rig_candidate_space_invalid"),
						Path);
				}
			}
			return true;
		}

		bool ValidateObservation(
			const FUeAgentTimelineAnimationPortObservation& Observation,
			EUeAgentTimelineAnimationPortFamily Family,
			FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0;
				Index < Observation.GetTemporalObservations().Num();
				++Index)
			{
				const FUeAgentTemporalPoint& Point =
					Observation.GetTemporalObservations()[Index];
				if (!Point.IsValid() || Point.GetState() != TEXT("authored"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_observation_time_invalid"),
						FString::Printf(
							TEXT("observation.temporal[%d]"), Index));
				}
			}
			for (int32 Index = 0;
				Index < Observation.GetTemporalRangeObservations().Num();
				++Index)
			{
				const FUeAgentTemporalRange& Range =
					Observation.GetTemporalRangeObservations()[Index];
				if (!Range.IsValid() || Range.GetState() != TEXT("authored"))
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_observation_range_invalid"),
						FString::Printf(
							TEXT("observation.ranges[%d]"), Index));
				}
			}
			if (!ValidateSpatialValues(
					Observation.GetSpatialObservations(),
					Family,
					TEXT("observation.spatial"),
					OutError))
			{
				return false;
			}
			if (Observation.GetReadbackEvidence().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("timeline_animation_readback_evidence_required"),
					TEXT("observation.readback_evidence"));
			}
			for (int32 Index = 0;
				Index < Observation.GetReadbackEvidence().Num();
				++Index)
			{
				if (Observation.GetReadbackEvidence()[Index]
						.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("timeline_animation_readback_evidence_invalid"),
						FString::Printf(
							TEXT("observation.readback_evidence[%d]"),
							Index));
				}
			}
			return true;
		}
	}

	FUeAgentTimelineAnimationPortRequest::
	FUeAgentTimelineAnimationPortRequest(
		EUeAgentTimelineAnimationPortFamily InFamily,
		TArray<FUeAgentTemporalPoint> InTemporalPoints,
		TArray<FUeAgentTemporalRange> InTemporalRanges,
		TArray<FUeAgentSpatialValuePtr> InSpatialValues,
		TSet<FString> InFrozenContextFacets,
		TArray<FUeAgentTemporalConversion> InTemporalConversions)
		: Family(InFamily)
		, TemporalPoints(MoveTemp(InTemporalPoints))
		, TemporalRanges(MoveTemp(InTemporalRanges))
		, SpatialValues(MoveTemp(InSpatialValues))
		, FrozenContextFacets(MoveTemp(InFrozenContextFacets))
		, TemporalConversions(MoveTemp(InTemporalConversions))
	{
	}

	EUeAgentTimelineAnimationPortFamily
	FUeAgentTimelineAnimationPortRequest::GetFamily() const { return Family; }
	const TArray<FUeAgentTemporalPoint>& FUeAgentTimelineAnimationPortRequest::GetTemporalPoints() const { return TemporalPoints; }
	const TArray<FUeAgentTemporalRange>& FUeAgentTimelineAnimationPortRequest::GetTemporalRanges() const { return TemporalRanges; }
	const TArray<FUeAgentSpatialValuePtr>& FUeAgentTimelineAnimationPortRequest::GetSpatialValues() const { return SpatialValues; }
	const TSet<FString>& FUeAgentTimelineAnimationPortRequest::GetFrozenContextFacets() const { return FrozenContextFacets; }
	const TArray<FUeAgentTemporalConversion>& FUeAgentTimelineAnimationPortRequest::GetTemporalConversions() const { return TemporalConversions; }

	FUeAgentTimelineAnimationPortObservation::
	FUeAgentTimelineAnimationPortObservation(
		TArray<FUeAgentTemporalPoint> InTemporalObservations,
		TArray<FUeAgentTemporalRange> InTemporalRangeObservations,
		TArray<FUeAgentSpatialValuePtr> InSpatialObservations,
		TArray<FString> InReadbackEvidence)
		: TemporalObservations(MoveTemp(InTemporalObservations))
		, TemporalRangeObservations(MoveTemp(InTemporalRangeObservations))
		, SpatialObservations(MoveTemp(InSpatialObservations))
		, ReadbackEvidence(MoveTemp(InReadbackEvidence))
	{
	}

	const TArray<FUeAgentTemporalPoint>& FUeAgentTimelineAnimationPortObservation::GetTemporalObservations() const { return TemporalObservations; }
	const TArray<FUeAgentTemporalRange>& FUeAgentTimelineAnimationPortObservation::GetTemporalRangeObservations() const { return TemporalRangeObservations; }
	const TArray<FUeAgentSpatialValuePtr>& FUeAgentTimelineAnimationPortObservation::GetSpatialObservations() const { return SpatialObservations; }
	const TArray<FString>& FUeAgentTimelineAnimationPortObservation::GetReadbackEvidence() const { return ReadbackEvidence; }

	bool TryExecuteTimelineAnimationPort(
		const FUeAgentTimelineAnimationPortRequest& Request,
		IUeAgentTimelineAnimationNativePort& NativePort,
		FUeAgentResourceTracker& Resources,
		FUeAgentTimelineAnimationPortObservation& OutObservation,
		FUeAgentTimelineAnimationPortError& OutError)
	{
		OutObservation = FUeAgentTimelineAnimationPortObservation();
		OutError = FUeAgentTimelineAnimationPortError();
		const FUeAgentTimelineAnimationPortFamilyContract* Contract =
			FindContract(Request.GetFamily());
		if (Contract == nullptr)
		{
			return Fail(
				OutError,
				TEXT("timeline_animation_port_family_invalid"),
				TEXT("family"));
		}
		for (const FString& Facet : Contract->GetRequiredContextFacets())
		{
			if (!Request.GetFrozenContextFacets().Contains(Facet))
			{
				return Fail(
					OutError,
					TEXT("timeline_animation_context_facet_missing"),
					TEXT("context.") + Facet,
					Contract->GetFamilyId());
			}
		}
		if (!ValidateTemporalInputs(Request, OutError)
			|| !ValidateSpatialValues(
				Request.GetSpatialValues(),
				Request.GetFamily(),
				TEXT("spatial_values"),
				OutError))
		{
			return false;
		}
		if (Request.GetFamily()
				== EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate
			&& (Request.GetTemporalPoints().IsEmpty()
				|| Request.GetSpatialValues().IsEmpty()))
		{
			return Fail(
				OutError,
				TEXT("timeline_animation_rig_candidate_input_required"));
		}
		const int64 InputItems = Request.GetTemporalPoints().Num()
			+ Request.GetTemporalRanges().Num()
			+ Request.GetSpatialValues().Num()
			+ Request.GetTemporalConversions().Num();
		if (!TryCharge(
				Resources,
				InputItems,
				TEXT("timeline_animation.preflight"),
				OutError))
		{
			return false;
		}

		FUeAgentTimelineAnimationPortObservation NativeObservation;
		FUeAgentTimelineAnimationPortError NativeError;
		if (!NativePort.TryExecute(
				Request,
				Resources,
				NativeObservation,
				NativeError))
		{
			OutError = MoveTemp(NativeError);
			if (OutError.Code.IsEmpty())
			{
				OutError.Code =
					TEXT("timeline_animation_native_operation_failed");
			}
			return false;
		}
		if (!ValidateObservation(
				NativeObservation,
				Request.GetFamily(),
				OutError))
		{
			return false;
		}
		const int64 ObservationItems =
			NativeObservation.GetTemporalObservations().Num()
			+ NativeObservation.GetTemporalRangeObservations().Num()
			+ NativeObservation.GetSpatialObservations().Num()
			+ NativeObservation.GetReadbackEvidence().Num();
		if (!TryCharge(
				Resources,
				ObservationItems,
				TEXT("timeline_animation.observation"),
				OutError))
		{
			return false;
		}
		OutObservation = MoveTemp(NativeObservation);
		return true;
	}
}
