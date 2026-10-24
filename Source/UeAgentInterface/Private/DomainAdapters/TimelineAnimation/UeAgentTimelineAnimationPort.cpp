#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationPort.h"

#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationContract.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		using UE::UAI::Contract::FDiagnostic;

		bool Fail(FUeAgentTimelineAnimationPortError& OutError, FDiagnostic Error)
		{
			OutError = MoveTemp(Error);
			return false;
		}

		FDiagnostic TimelinePortFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic PrefixTimelinePortValue(FDiagnostic Error, const FString& Prefix)
		{
			if (Error.IsEmpty()) return Error;
			Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() || Error.PropertyPath == TEXT("/") ? FString()
				: Error.PropertyPath.StartsWith(TEXT("/")) ? Error.PropertyPath : TEXT("/") + Error.PropertyPath);
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) Child = PrefixTimelinePortValue(MoveTemp(Child), Prefix);
			if (Error.PublicCause) Error.PublicCause = MakeShared<FDiagnostic>(PrefixTimelinePortValue(*Error.PublicCause, Prefix));
			return Error;
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

		bool TryCharge(FUeAgentResourceTracker& Resources, int64 Items, const FString& Path, FUeAgentTimelineAnimationPortError& OutError)
		{
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany({{EUeAgentResourceDimension::Items, Items}, {EUeAgentResourceDimension::WorkUnits, Items + 1}}, Path, ChargeError))
				return true;
			return Fail(OutError, MoveTemp(ChargeError));
		}

		bool ValidateTemporalInputs(const FUeAgentTimelineAnimationPortRequest& Request, FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0; Index < Request.GetTemporalPoints().Num(); ++Index)
			{
				const auto& Point = Request.GetTemporalPoints()[Index];
				const FString Path = TEXT("/temporal_points/") + FString::FromInt(Index);
				if (!Point.IsValid()) return Fail(OutError, PrefixTimelinePortValue(Point.GetValidationDiagnostic(), Path));
				if (Point.GetState() != TEXT("authored"))
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_authored_time_required"),
						TEXT("Timeline input time must be in authored state."), Path + TEXT("/state"),
						MakeShared<FJsonValueString>(Point.GetState()), MakeShared<FJsonValueString>(TEXT("authored"))));
			}
			for (int32 Index = 0; Index < Request.GetTemporalRanges().Num(); ++Index)
			{
				const auto& Range = Request.GetTemporalRanges()[Index];
				const FString Path = TEXT("/temporal_ranges/") + FString::FromInt(Index);
				if (!Range.IsValid()) return Fail(OutError, PrefixTimelinePortValue(Range.GetValidationDiagnostic(), Path));
				if (Range.GetState() != TEXT("authored"))
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_authored_time_required"),
						TEXT("Timeline input range must be in authored state."), Path + TEXT("/state"),
						MakeShared<FJsonValueString>(Range.GetState()), MakeShared<FJsonValueString>(TEXT("authored"))));
			}
			for (int32 Index = 0; Index < Request.GetTemporalConversions().Num(); ++Index)
				if (!Request.GetTemporalConversions()[Index].IsValid())
					return Fail(OutError, PrefixTimelinePortValue(Request.GetTemporalConversions()[Index].GetValidationDiagnostic(),
						TEXT("/temporal_conversions/") + FString::FromInt(Index)));
			return true;
		}

		bool ValidateSpatialValues(const TArray<FUeAgentSpatialValuePtr>& Values, EUeAgentTimelineAnimationPortFamily Family,
			const FString& PathPrefix, FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const auto& Value = Values[Index];
				const FString Path = PathPrefix + TEXT("/") + FString::FromInt(Index);
				if (!Value.IsValid())
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_spatial_value_invalid"),
						TEXT("The timeline spatial value is missing."), Path, MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("spatial value"))));
				if (!Value->IsValid()) return Fail(OutError, PrefixTimelinePortValue(Value->GetValidationDiagnostic(), Path));
				if (Family == EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate)
				{
					if (Value->GetFrame().GetDimension() != 3)
						return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_rig_candidate_space_invalid"),
							TEXT("A rig candidate requires a three-dimensional spatial value."), Path + TEXT("/frame/dimension"),
							MakeShared<FJsonValueNumber>(Value->GetFrame().GetDimension()), MakeShared<FJsonValueNumber>(3)));
					if (Value->GetQuantityKind() != EUeAgentSpatialQuantityKind::Transform)
						return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_rig_candidate_space_invalid"),
							TEXT("A rig candidate spatial value must have Transform quantity kind."), Path + TEXT("/quantity_kind"),
							MakeShared<FJsonValueNumber>(static_cast<int32>(Value->GetQuantityKind())),
							MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentSpatialQuantityKind::Transform))));
				}
			}
			return true;
		}

		bool ValidateObservation(const FUeAgentTimelineAnimationPortObservation& Observation, EUeAgentTimelineAnimationPortFamily Family,
			FUeAgentTimelineAnimationPortError& OutError)
		{
			for (int32 Index = 0; Index < Observation.GetTemporalObservations().Num(); ++Index)
			{
				const auto& Point = Observation.GetTemporalObservations()[Index];
				const FString Path = TEXT("/observation/temporal/") + FString::FromInt(Index);
				if (!Point.IsValid()) return Fail(OutError, PrefixTimelinePortValue(Point.GetValidationDiagnostic(), Path));
				if (Point.GetState() != TEXT("authored"))
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_observation_time_invalid"),
						TEXT("Observed timeline time must be in authored state."), Path + TEXT("/state"),
						MakeShared<FJsonValueString>(Point.GetState()), MakeShared<FJsonValueString>(TEXT("authored"))));
			}
			for (int32 Index = 0; Index < Observation.GetTemporalRangeObservations().Num(); ++Index)
			{
				const auto& Range = Observation.GetTemporalRangeObservations()[Index];
				const FString Path = TEXT("/observation/ranges/") + FString::FromInt(Index);
				if (!Range.IsValid()) return Fail(OutError, PrefixTimelinePortValue(Range.GetValidationDiagnostic(), Path));
				if (Range.GetState() != TEXT("authored"))
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_observation_range_invalid"),
						TEXT("Observed timeline range must be in authored state."), Path + TEXT("/state"),
						MakeShared<FJsonValueString>(Range.GetState()), MakeShared<FJsonValueString>(TEXT("authored"))));
			}
			if (!ValidateSpatialValues(Observation.GetSpatialObservations(), Family, TEXT("/observation/spatial"), OutError)) return false;
			if (Observation.GetReadbackEvidence().IsEmpty())
				return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_readback_evidence_required"),
					TEXT("Timeline observation requires at least one readback evidence entry."), TEXT("/observation/readback_evidence/count"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one"))));
			for (int32 Index = 0; Index < Observation.GetReadbackEvidence().Num(); ++Index)
				if (Observation.GetReadbackEvidence()[Index].TrimStartAndEnd().IsEmpty())
					return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_readback_evidence_invalid"),
						TEXT("A readback evidence entry is empty or whitespace."), TEXT("/observation/readback_evidence/") + FString::FromInt(Index),
						MakeShared<FJsonValueString>(Observation.GetReadbackEvidence()[Index]), MakeShared<FJsonValueString>(TEXT("non-empty evidence"))));
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
			return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_port_family_invalid"),
				TEXT("The timeline port family has no registered contract."), TEXT("/family"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())),
				MakeShared<FJsonValueString>(TEXT("TemporalRead, AnimationKeyedData, TimelineStructure, SubjectAnimationState, or TimelineRigCandidate"))));
		}
		for (const FString& Facet : Contract->GetRequiredContextFacets())
		{
			if (!Request.GetFrozenContextFacets().Contains(Facet))
			{
				return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_context_facet_missing"),
					FString(TEXT("Timeline port family ")) + Contract->GetFamilyId() + TEXT(" requires the frozen context facet."),
					TEXT("/context/facets/") + Facet.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
		}
		if (!ValidateTemporalInputs(Request, OutError)
			|| !ValidateSpatialValues(
				Request.GetSpatialValues(),
				Request.GetFamily(),
				TEXT("/spatial_values"),
				OutError))
		{
			return false;
		}
		if (Request.GetFamily()
				== EUeAgentTimelineAnimationPortFamily::TimelineRigCandidate
			&& (Request.GetTemporalPoints().IsEmpty()
				|| Request.GetSpatialValues().IsEmpty()))
		{
			const bool bPointMissing = Request.GetTemporalPoints().IsEmpty();
			return Fail(OutError, TimelinePortFact(TEXT("timeline_animation_rig_candidate_input_required"),
				bPointMissing ? TEXT("A rig candidate requires at least one temporal point.") : TEXT("A rig candidate requires at least one spatial value."),
				bPointMissing ? TEXT("/temporal_points/count") : TEXT("/spatial_values/count"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one"))));
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
			OutError = !NativeError.IsEmpty() ? MoveTemp(NativeError) : TimelinePortFact(TEXT("timeline_animation_native_operation_failed"),
				TEXT("The native timeline port failed without returning a diagnostic."), TEXT("/native_port/succeeded"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
