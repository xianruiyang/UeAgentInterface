#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPort.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialContract.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentRigSimulationSpatialPortError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Path = MoveTemp(Path);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		const TCHAR* FamilyId(EUeAgentRigSimulationSpatialPortFamily Family)
		{
			switch (Family)
			{
			case EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment:
				return TEXT("skeletal_attachment");
			case EUeAgentRigSimulationSpatialPortFamily::RigBake:
				return TEXT("rig_bake");
			case EUeAgentRigSimulationSpatialPortFamily::IkRetarget:
				return TEXT("ik_retarget");
			case EUeAgentRigSimulationSpatialPortFamily::SimulationDefinition:
				return TEXT("simulation_definition");
			case EUeAgentRigSimulationSpatialPortFamily::SimulationObservation:
				return TEXT("simulation_observation");
			default:
				return TEXT("");
			}
		}

		const FUeAgentRigSimulationSpatialPortFamilyContract* FindContract(
			EUeAgentRigSimulationSpatialPortFamily Family)
		{
			const FString Wanted = FamilyId(Family);
			for (const FUeAgentRigSimulationSpatialPortFamilyContract& Contract
				: GetRigSimulationSpatialPortFamilyContracts())
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
			FUeAgentRigSimulationSpatialPortError& OutError)
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
					? TEXT("rig_simulation_spatial_resource_budget_exceeded")
					: BudgetError.Code,
				BudgetError.Path,
				LexToString(BudgetError.Dimension));
		}

		bool ValidateTemporalState(
			const FString& State,
			const FUeAgentRigSimulationSpatialPortFamilyContract& Contract,
			const FString& Path,
			FUeAgentRigSimulationSpatialPortError& OutError)
		{
			if (Contract.GetAcceptedTimeStates().Contains(State))
			{
				return true;
			}
			if (Contract.GetAcceptedTimeStates()
					== TArray<FString>({TEXT("authored")}))
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_authored_time_required"),
					Path,
					State);
			}
			return Fail(
				OutError,
				TEXT("rig_simulation_spatial_temporal_state_not_allowed"),
				Path,
				State);
		}

		bool ValidateTemporalInputs(
			const FUeAgentRigSimulationSpatialPortRequest& Request,
			const FUeAgentRigSimulationSpatialPortFamilyContract& Contract,
			FUeAgentRigSimulationSpatialPortError& OutError)
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
						TEXT("rig_simulation_spatial_temporal_point_invalid"),
						Path);
				}
				if (!ValidateTemporalState(
						Point.GetState(), Contract, Path + TEXT(".state"),
						OutError))
				{
					return false;
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
						TEXT("rig_simulation_spatial_temporal_range_invalid"),
						Path);
				}
				if (!ValidateTemporalState(
						Range.GetState(), Contract, Path + TEXT(".state"),
						OutError))
				{
					return false;
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
						TEXT("rig_simulation_spatial_temporal_conversion_invalid"),
						FString::Printf(
							TEXT("temporal_conversions[%d]"), Index));
				}
			}
			return true;
		}

		bool ValidateSpatialValues(
			const TArray<FUeAgentSpatialValuePtr>& Values,
			const TArray<int32>& AllowedDimensions,
			const FString& PathPrefix,
			FUeAgentRigSimulationSpatialPortError& OutError)
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
						TEXT("rig_simulation_spatial_value_invalid"),
						Path);
				}
				if (!AllowedDimensions.Contains(
						Value->GetFrame().GetDimension()))
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_dimension_mismatch"),
						Path,
						FString::FromInt(
							Value->GetFrame().GetDimension()));
				}
				if (Value->GetSubject().TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_subject_required"),
						Path + TEXT(".subject"));
				}
			}
			return true;
		}

		bool ValidateSpatialConversions(
			const FUeAgentRigSimulationSpatialPortRequest& Request,
			const FUeAgentRigSimulationSpatialPortFamilyContract& Contract,
			FUeAgentRigSimulationSpatialPortError& OutError)
		{
			if (!Request.GetSpatialValues().IsEmpty()
				&& Request.GetSpatialConversions().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_conversion_required"),
					TEXT("spatial_conversions"));
			}
			for (int32 Index = 0;
				Index < Request.GetSpatialConversions().Num();
				++Index)
			{
				const FUeAgentSpatialConversion& Conversion =
					Request.GetSpatialConversions()[Index];
				const FString Path = FString::Printf(
					TEXT("spatial_conversions[%d]"), Index);
				if (!Conversion.IsValid())
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_conversion_invalid"),
						Path);
				}
				if (!Contract.GetDimensions().Contains(
						Conversion.GetSourceDimension())
					|| !Contract.GetDimensions().Contains(
						Conversion.GetTargetDimension()))
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_conversion_dimension_mismatch"),
						Path);
				}
			}
			for (int32 ValueIndex = 0;
				ValueIndex < Request.GetSpatialValues().Num();
				++ValueIndex)
			{
				const FUeAgentSpatialValuePtr& Value =
					Request.GetSpatialValues()[ValueIndex];
				if (!Value.IsValid())
				{
					continue;
				}
				const FString& FrameId = Value->GetFrame().GetFrameId();
				bool bFrameReferenced = false;
				for (const FUeAgentSpatialConversion& Conversion
					: Request.GetSpatialConversions())
				{
					if (Conversion.GetFromFrame() == FrameId
						|| Conversion.GetToFrame() == FrameId)
					{
						bFrameReferenced = true;
						break;
					}
				}
				if (!bFrameReferenced)
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_frame_chain_mismatch"),
						FString::Printf(
							TEXT("spatial_values[%d].frame"), ValueIndex),
						FrameId);
				}
			}
			return true;
		}

		bool HasTemporalConversionPath(
			const FString& FromClock,
			const FString& ToClock,
			const TArray<FUeAgentTemporalConversion>& Conversions)
		{
			if (FromClock == ToClock)
			{
				return true;
			}
			TArray<FString> Pending = {FromClock};
			TSet<FString> Visited;
			for (int32 PendingIndex = 0;
				PendingIndex < Pending.Num();
				++PendingIndex)
			{
				const FString Current = Pending[PendingIndex];
				if (Visited.Contains(Current))
				{
					continue;
				}
				Visited.Add(Current);
				for (const FUeAgentTemporalConversion& Conversion
					: Conversions)
				{
					if (Conversion.GetSourceClockId() != Current)
					{
						continue;
					}
					if (Conversion.GetTargetClockId() == ToClock)
					{
						return true;
					}
					Pending.AddUnique(Conversion.GetTargetClockId());
				}
			}
			return false;
		}

		bool HasSpatialConversionPath(
			const FString& FromFrame,
			const FString& ToFrame,
			const TArray<FUeAgentSpatialConversion>& Conversions)
		{
			if (FromFrame == ToFrame)
			{
				return true;
			}
			TArray<FString> Pending = {FromFrame};
			TSet<FString> Visited;
			for (int32 PendingIndex = 0;
				PendingIndex < Pending.Num();
				++PendingIndex)
			{
				const FString Current = Pending[PendingIndex];
				if (Visited.Contains(Current))
				{
					continue;
				}
				Visited.Add(Current);
				for (const FUeAgentSpatialConversion& Conversion
					: Conversions)
				{
					if (Conversion.GetFromFrame() != Current)
					{
						continue;
					}
					if (Conversion.GetToFrame() == ToFrame)
					{
						return true;
					}
					Pending.AddUnique(Conversion.GetToFrame());
				}
			}
			return false;
		}

		bool ValidateObservation(
			const FUeAgentRigSimulationSpatialPortRequest& Request,
			const FUeAgentRigSimulationSpatialPortFamilyContract& Contract,
			const FUeAgentRigSimulationSpatialPortObservation& Observation,
			FUeAgentRigSimulationSpatialPortError& OutError)
		{
			if (Observation.GetReadbackEvidence().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_readback_evidence_required"),
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
						TEXT("rig_simulation_spatial_readback_evidence_invalid"),
						FString::Printf(
							TEXT("observation.readback_evidence[%d]"),
							Index));
				}
			}
			if ((!Request.GetTemporalPoints().IsEmpty()
					|| !Request.GetTemporalRanges().IsEmpty())
				&& Observation.GetTemporalObservations().IsEmpty()
				&& Observation.GetTemporalRangeObservations().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_observation_time_required"),
					TEXT("observation.temporal"));
			}
			for (int32 Index = 0;
				Index < Observation.GetTemporalObservations().Num();
				++Index)
			{
				const FUeAgentTemporalPoint& Point =
					Observation.GetTemporalObservations()[Index];
				const FString Path = FString::Printf(
					TEXT("observation.temporal[%d]"), Index);
				if (!Point.IsValid())
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_observation_time_invalid"),
						Path);
				}
				if (!ValidateTemporalState(
						Point.GetState(), Contract, Path + TEXT(".state"),
						OutError))
				{
					return false;
				}
				bool bClockConnected = Request.GetTemporalPoints().IsEmpty();
				for (const FUeAgentTemporalPoint& Input
					: Request.GetTemporalPoints())
				{
					bClockConnected = bClockConnected
						|| HasTemporalConversionPath(
							Input.GetClockId(), Point.GetClockId(),
							Request.GetTemporalConversions());
				}
				if (!bClockConnected)
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_observation_clock_mismatch"),
						Path + TEXT(".clock_id"),
						Point.GetClockId());
				}
			}
			for (int32 Index = 0;
				Index < Observation.GetTemporalRangeObservations().Num();
				++Index)
			{
				const FUeAgentTemporalRange& Range =
					Observation.GetTemporalRangeObservations()[Index];
				const FString Path = FString::Printf(
					TEXT("observation.temporal_ranges[%d]"), Index);
				if (!Range.IsValid())
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_observation_range_invalid"),
						Path);
				}
				if (!ValidateTemporalState(
						Range.GetState(), Contract, Path + TEXT(".state"),
						OutError))
				{
					return false;
				}
			}
			if (!ValidateSpatialValues(
					Observation.GetSpatialObservations(),
					Contract.GetDimensions(),
					TEXT("observation.spatial"),
					OutError))
			{
				return false;
			}
			if (!Request.GetSpatialValues().IsEmpty()
				&& Observation.GetSpatialObservations().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_observation_space_required"),
					TEXT("observation.spatial"));
			}
			for (int32 ObservationIndex = 0;
				ObservationIndex < Observation.GetSpatialObservations().Num();
				++ObservationIndex)
			{
				const FUeAgentSpatialValuePtr& Observed =
					Observation.GetSpatialObservations()[ObservationIndex];
				bool bRelated = Request.GetSpatialValues().IsEmpty();
				for (const FUeAgentSpatialValuePtr& Input
					: Request.GetSpatialValues())
				{
					if (!Input.IsValid())
					{
						continue;
					}
					if (Input->GetSubject() == Observed->GetSubject()
						&& HasSpatialConversionPath(
							Input->GetFrame().GetFrameId(),
							Observed->GetFrame().GetFrameId(),
							Request.GetSpatialConversions()))
					{
						bRelated = true;
						break;
					}
				}
				if (!bRelated && Observed->GetConversionEvidence().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("rig_simulation_spatial_observation_subject_frame_mismatch"),
						FString::Printf(
							TEXT("observation.spatial[%d]"),
							ObservationIndex),
						Observed->GetSubject());
				}
			}
			return true;
		}
	}

	FUeAgentRigSimulationSpatialPortRequest::
	FUeAgentRigSimulationSpatialPortRequest(
		EUeAgentRigSimulationSpatialPortFamily InFamily,
		TArray<FUeAgentTemporalPoint> InTemporalPoints,
		TArray<FUeAgentTemporalRange> InTemporalRanges,
		TArray<FUeAgentSpatialValuePtr> InSpatialValues,
		TArray<FUeAgentSpatialConversion> InSpatialConversions,
		TSet<FString> InFrozenContextFacets,
		TArray<FUeAgentTemporalConversion> InTemporalConversions)
		: Family(InFamily)
		, TemporalPoints(MoveTemp(InTemporalPoints))
		, TemporalRanges(MoveTemp(InTemporalRanges))
		, SpatialValues(MoveTemp(InSpatialValues))
		, SpatialConversions(MoveTemp(InSpatialConversions))
		, FrozenContextFacets(MoveTemp(InFrozenContextFacets))
		, TemporalConversions(MoveTemp(InTemporalConversions))
	{
	}

	EUeAgentRigSimulationSpatialPortFamily
	FUeAgentRigSimulationSpatialPortRequest::GetFamily() const
	{
		return Family;
	}

	const TArray<FUeAgentTemporalPoint>&
	FUeAgentRigSimulationSpatialPortRequest::GetTemporalPoints() const
	{
		return TemporalPoints;
	}

	const TArray<FUeAgentTemporalRange>&
	FUeAgentRigSimulationSpatialPortRequest::GetTemporalRanges() const
	{
		return TemporalRanges;
	}

	const TArray<FUeAgentSpatialValuePtr>&
	FUeAgentRigSimulationSpatialPortRequest::GetSpatialValues() const
	{
		return SpatialValues;
	}

	const TArray<FUeAgentSpatialConversion>&
	FUeAgentRigSimulationSpatialPortRequest::GetSpatialConversions() const
	{
		return SpatialConversions;
	}

	const TSet<FString>&
	FUeAgentRigSimulationSpatialPortRequest::GetFrozenContextFacets() const
	{
		return FrozenContextFacets;
	}

	const TArray<FUeAgentTemporalConversion>&
	FUeAgentRigSimulationSpatialPortRequest::GetTemporalConversions() const
	{
		return TemporalConversions;
	}

	FUeAgentRigSimulationSpatialPortObservation::
	FUeAgentRigSimulationSpatialPortObservation(
		TArray<FUeAgentTemporalPoint> InTemporalObservations,
		TArray<FUeAgentSpatialValuePtr> InSpatialObservations,
		TArray<FString> InReadbackEvidence,
		TArray<FUeAgentTemporalRange> InTemporalRangeObservations)
		: TemporalObservations(MoveTemp(InTemporalObservations))
		, TemporalRangeObservations(MoveTemp(InTemporalRangeObservations))
		, SpatialObservations(MoveTemp(InSpatialObservations))
		, ReadbackEvidence(MoveTemp(InReadbackEvidence))
	{
	}

	const TArray<FUeAgentTemporalPoint>&
	FUeAgentRigSimulationSpatialPortObservation::
	GetTemporalObservations() const
	{
		return TemporalObservations;
	}

	const TArray<FUeAgentTemporalRange>&
	FUeAgentRigSimulationSpatialPortObservation::
	GetTemporalRangeObservations() const
	{
		return TemporalRangeObservations;
	}

	const TArray<FUeAgentSpatialValuePtr>&
	FUeAgentRigSimulationSpatialPortObservation::
	GetSpatialObservations() const
	{
		return SpatialObservations;
	}

	const TArray<FString>&
	FUeAgentRigSimulationSpatialPortObservation::GetReadbackEvidence() const
	{
		return ReadbackEvidence;
	}

	bool TryExecuteRigSimulationSpatialPort(
		const FUeAgentRigSimulationSpatialPortRequest& Request,
		IUeAgentRigSimulationSpatialNativePort& NativePort,
		FUeAgentResourceTracker& Resources,
		FUeAgentRigSimulationSpatialPortObservation& OutObservation,
		FUeAgentRigSimulationSpatialPortError& OutError)
	{
		OutObservation = FUeAgentRigSimulationSpatialPortObservation();
		OutError = FUeAgentRigSimulationSpatialPortError();

		const FUeAgentRigSimulationSpatialPortFamilyContract* Contract =
			FindContract(Request.GetFamily());
		if (Contract == nullptr)
		{
			return Fail(
				OutError,
				TEXT("rig_simulation_spatial_port_family_invalid"),
				TEXT("family"));
		}
		for (const FString& Facet : Contract->GetRequiredContextFacets())
		{
			if (!Request.GetFrozenContextFacets().Contains(Facet))
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_context_facet_missing"),
					TEXT("context.") + Facet,
					Contract->GetFamilyId());
			}
		}
		for (const FString& Facet : Contract->GetForbiddenImplicitFacets())
		{
			if (Request.GetFrozenContextFacets().Contains(Facet))
			{
				return Fail(
					OutError,
					TEXT("rig_simulation_spatial_implicit_context_forbidden"),
					TEXT("context.") + Facet,
					Contract->GetFamilyId());
			}
		}
		if (!ValidateTemporalInputs(Request, *Contract, OutError)
			|| !ValidateSpatialValues(
				Request.GetSpatialValues(),
				Contract->GetDimensions(),
				TEXT("spatial_values"),
				OutError)
			|| !ValidateSpatialConversions(Request, *Contract, OutError))
		{
			return false;
		}
		const int64 InputItems = Request.GetTemporalPoints().Num()
			+ Request.GetTemporalRanges().Num()
			+ Request.GetSpatialValues().Num()
			+ Request.GetSpatialConversions().Num()
			+ Request.GetTemporalConversions().Num()
			+ Request.GetFrozenContextFacets().Num();
		if (!TryCharge(
				Resources,
				InputItems,
				TEXT("rig_simulation_spatial.preflight"),
				OutError))
		{
			return false;
		}

		FUeAgentRigSimulationSpatialPortObservation NativeObservation;
		FUeAgentRigSimulationSpatialPortError NativeError;
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
					TEXT("rig_simulation_spatial_native_operation_failed");
			}
			return false;
		}
		if (!ValidateObservation(
				Request, *Contract, NativeObservation, OutError))
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
				TEXT("rig_simulation_spatial.observation"),
				OutError))
		{
			return false;
		}
		OutObservation = MoveTemp(NativeObservation);
		return true;
	}
}
