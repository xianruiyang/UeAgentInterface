#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialPort.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialContract.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentWorldSpatialPortError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Path = MoveTemp(Path);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		const TCHAR* FamilyId(EUeAgentWorldSpatialPortFamily Family)
		{
			switch (Family)
			{
			case EUeAgentWorldSpatialPortFamily::GraphView:
				return TEXT("graph_view");
			case EUeAgentWorldSpatialPortFamily::GraphLayout:
				return TEXT("graph_layout");
			case EUeAgentWorldSpatialPortFamily::ObjectTransform:
				return TEXT("object_transform");
			case EUeAgentWorldSpatialPortFamily::ViewportProjection:
				return TEXT("viewport_projection");
			case EUeAgentWorldSpatialPortFamily::ViewportSessionState:
				return TEXT("viewport_session_state");
			default:
				return TEXT("");
			}
		}

		const FUeAgentWorldSpatialPortFamilyContract* FindContract(
			EUeAgentWorldSpatialPortFamily Family)
		{
			const FString Wanted = FamilyId(Family);
			for (const FUeAgentWorldSpatialPortFamilyContract& Contract
				: GetWorldSpatialPortFamilyContracts())
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
			FUeAgentWorldSpatialPortError& OutError)
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
					? TEXT("world_spatial_resource_budget_exceeded")
					: BudgetError.Code,
				BudgetError.Path,
				LexToString(BudgetError.Dimension));
		}

		bool ValidateSpatialValues(
			const TArray<FUeAgentSpatialValuePtr>& Values,
			const TArray<int32>& AllowedDimensions,
			const FString& PathPrefix,
			FUeAgentWorldSpatialPortError& OutError)
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
						TEXT("world_spatial_value_invalid"),
						Path);
				}
				if (!AllowedDimensions.Contains(
						Value->GetFrame().GetDimension()))
				{
					return Fail(
						OutError,
						TEXT("world_spatial_input_dimension_mismatch"),
						Path,
						FString::FromInt(
							Value->GetFrame().GetDimension()));
				}
			}
			return true;
		}

		bool HasDimension(
			const TArray<FUeAgentSpatialValuePtr>& Values,
			int32 Dimension)
		{
			for (const FUeAgentSpatialValuePtr& Value : Values)
			{
				if (Value.IsValid()
					&& Value->GetFrame().GetDimension() == Dimension)
				{
					return true;
				}
			}
			return false;
		}
	}

	FUeAgentWorldSpatialPortRequest::FUeAgentWorldSpatialPortRequest(
		EUeAgentWorldSpatialPortFamily InFamily,
		TArray<FUeAgentSpatialValuePtr> InSpatialInputs,
		TSet<FString> InFrozenContextFacets)
		: Family(InFamily)
		, SpatialInputs(MoveTemp(InSpatialInputs))
		, FrozenContextFacets(MoveTemp(InFrozenContextFacets))
	{
	}

	EUeAgentWorldSpatialPortFamily
	FUeAgentWorldSpatialPortRequest::GetFamily() const
	{
		return Family;
	}

	const TArray<FUeAgentSpatialValuePtr>&
	FUeAgentWorldSpatialPortRequest::GetSpatialInputs() const
	{
		return SpatialInputs;
	}

	const TSet<FString>&
	FUeAgentWorldSpatialPortRequest::GetFrozenContextFacets() const
	{
		return FrozenContextFacets;
	}

	FUeAgentWorldSpatialPortObservation::
	FUeAgentWorldSpatialPortObservation(
		TArray<FUeAgentSpatialValuePtr> InSpatialObservations,
		FString InDimensionBridgeEvidence)
		: SpatialObservations(MoveTemp(InSpatialObservations))
		, DimensionBridgeEvidence(MoveTemp(InDimensionBridgeEvidence))
	{
	}

	const TArray<FUeAgentSpatialValuePtr>&
	FUeAgentWorldSpatialPortObservation::GetSpatialObservations() const
	{
		return SpatialObservations;
	}

	const FString& FUeAgentWorldSpatialPortObservation::
	GetDimensionBridgeEvidence() const
	{
		return DimensionBridgeEvidence;
	}

	bool TryExecuteWorldSpatialPort(
		const FUeAgentWorldSpatialPortRequest& Request,
		IUeAgentWorldSpatialNativePort& NativePort,
		FUeAgentResourceTracker& Resources,
		FUeAgentWorldSpatialPortObservation& OutObservation,
		FUeAgentWorldSpatialPortError& OutError)
	{
		OutObservation = FUeAgentWorldSpatialPortObservation();
		OutError = FUeAgentWorldSpatialPortError();

		const FUeAgentWorldSpatialPortFamilyContract* Contract =
			FindContract(Request.GetFamily());
		if (Contract == nullptr)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_port_family_invalid"),
				TEXT("family"));
		}

		for (const FString& Facet
			: Contract->GetRequiredContextFacets())
		{
			if (!Request.GetFrozenContextFacets().Contains(Facet))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_context_facet_missing"),
					TEXT("context.") + Facet,
					Contract->GetFamilyId());
			}
		}

		if (!ValidateSpatialValues(
				Request.GetSpatialInputs(),
				Contract->GetDimensions(),
				TEXT("input"),
				OutError)
			|| !TryCharge(
				Resources,
				Request.GetSpatialInputs().Num(),
				TEXT("world_spatial.preflight"),
				OutError))
		{
			return false;
		}

		FUeAgentWorldSpatialPortObservation NativeObservation;
		FUeAgentWorldSpatialPortError NativeError;
		if (!NativePort.TryExecute(
				Request,
				Resources,
				NativeObservation,
				NativeError))
		{
			OutError = MoveTemp(NativeError);
			if (OutError.Code.IsEmpty())
			{
				OutError.Code = TEXT("world_spatial_native_operation_failed");
			}
			return false;
		}

		if (!ValidateSpatialValues(
				NativeObservation.GetSpatialObservations(),
				Contract->GetDimensions(),
				TEXT("observation"),
				OutError))
		{
			if (OutError.Code
				== TEXT("world_spatial_input_dimension_mismatch"))
			{
				OutError.Code =
					TEXT("world_spatial_observation_dimension_mismatch");
			}
			return false;
		}

		const bool bCrossesTwoToThree =
			HasDimension(Request.GetSpatialInputs(), 2)
			&& HasDimension(
				NativeObservation.GetSpatialObservations(), 3);
		if (bCrossesTwoToThree)
		{
			if (Request.GetFamily()
					!= EUeAgentWorldSpatialPortFamily::ViewportProjection
				|| !NativeObservation.GetDimensionBridgeEvidence().StartsWith(
					TEXT("camera_deprojection:")))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_dimension_bridge_evidence_required"),
					TEXT("observation.dimension_bridge_evidence"),
					TEXT("camera_deprojection"));
			}
		}
		else if (!NativeObservation.GetDimensionBridgeEvidence().IsEmpty()
			&& Request.GetFamily()
				!= EUeAgentWorldSpatialPortFamily::ViewportProjection)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_dimension_bridge_not_allowed"),
				TEXT("observation.dimension_bridge_evidence"));
		}

		if (!TryCharge(
				Resources,
				NativeObservation.GetSpatialObservations().Num(),
				TEXT("world_spatial.observation"),
				OutError))
		{
			return false;
		}

		OutObservation = MoveTemp(NativeObservation);
		return true;
	}
}
