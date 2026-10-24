#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialPort.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(FUeAgentWorldSpatialPortError& OutError, FUeAgentWorldSpatialPortError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FUeAgentWorldSpatialPortError NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		void PrefixValueContext(FUeAgentWorldSpatialPortError& Error, const FString& Path)
		{
			if (Error.IsEmpty()) return;
			Error.PropertyPath = Path + (Error.PropertyPath.IsEmpty() || Error.PropertyPath.StartsWith(TEXT("/")) ? TEXT("") : TEXT("/")) + Error.PropertyPath;
			if (Error.SourceKind.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) PrefixValueContext(Child, Path);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<FUeAgentWorldSpatialPortError>(*Error.PublicCause);
				PrefixValueContext(*Cause, Path);
				Error.PublicCause = MoveTemp(Cause);
			}
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
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(
					{{EUeAgentResourceDimension::Items, Items},
				 {EUeAgentResourceDimension::WorkUnits, Items + 1}},
					Path,
					ChargeError))
			{
				return true;
			}
			return Fail(OutError, MoveTemp(ChargeError));
		}

		bool ValidateSpatialValues(
			const TArray<FUeAgentSpatialValuePtr>& Values,
			const TArray<int32>& AllowedDimensions,
			const FString& PathPrefix,
			FUeAgentWorldSpatialPortError& OutError,
			const FString& DimensionMismatchCode)
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FUeAgentSpatialValuePtr& Value = Values[Index];
				const FString Path = PathPrefix + TEXT("/") + FString::FromInt(Index);
				if (!Value.IsValid())
					return Fail(OutError, NativeFact(TEXT("world_spatial_value_invalid"),
						TEXT("Spatial port value is missing."), Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				if (!Value->IsValid())
				{
					OutError = Value->GetValidationDiagnostic();
					PrefixValueContext(OutError, Path);
					return false;
				}
				if (!AllowedDimensions.Contains(Value->GetFrame().GetDimension()))
				{
					TArray<TSharedPtr<FJsonValue>> Expected;
					for (const int32 Dimension : AllowedDimensions) Expected.Add(MakeShared<FJsonValueNumber>(Dimension));
					return Fail(OutError, NativeFact(DimensionMismatchCode,
						TEXT("Spatial value frame dimension is not supported by this port family."), Path + TEXT("/frame/dimension"),
						MakeShared<FJsonValueNumber>(Value->GetFrame().GetDimension()), MakeShared<FJsonValueArray>(MoveTemp(Expected))));
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
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const auto& Family : GetWorldSpatialPortFamilyContracts()) Expected.Add(MakeShared<FJsonValueString>(Family.GetFamilyId()));
			return Fail(OutError, NativeFact(TEXT("world_spatial_port_family_invalid"),
				TEXT("Spatial port family is not registered."), TEXT("/family"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())), MakeShared<FJsonValueArray>(MoveTemp(Expected))));
		}

		for (const FString& Facet
			: Contract->GetRequiredContextFacets())
		{
			if (!Request.GetFrozenContextFacets().Contains(Facet))
			{
				FString EscapedFacet = Facet;
				EscapedFacet.ReplaceInline(TEXT("~"), TEXT("~0"));
				EscapedFacet.ReplaceInline(TEXT("/"), TEXT("~1"));
				return Fail(OutError, NativeFact(TEXT("world_spatial_context_facet_missing"),
					TEXT("Spatial port family ") + Contract->GetFamilyId() + TEXT(" requires a frozen ") + Facet + TEXT(" context facet."),
					TEXT("/context/") + EscapedFacet, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
		}

		if (!ValidateSpatialValues(
				Request.GetSpatialInputs(),
				Contract->GetDimensions(),
				TEXT("/input"),
				OutError, TEXT("world_spatial_input_dimension_mismatch"))
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
				OutError = NativeFact(TEXT("world_spatial_native_operation_failed"),
					TEXT("Native spatial port failed without returning a diagnostic."), TEXT("/native_port/diagnostic"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			}
			return false;
		}

		if (!ValidateSpatialValues(
				NativeObservation.GetSpatialObservations(),
				Contract->GetDimensions(),
				TEXT("/observation"),
				OutError, TEXT("world_spatial_observation_dimension_mismatch")))
		{
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
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("family"), FamilyId(Request.GetFamily()));
				Actual->SetBoolField(TEXT("bridge_evidence_present"), !NativeObservation.GetDimensionBridgeEvidence().IsEmpty());
				Actual->SetBoolField(TEXT("camera_deprojection_kind"), NativeObservation.GetDimensionBridgeEvidence().StartsWith(TEXT("camera_deprojection:")));
				const auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("family"), TEXT("viewport_projection"));
				Expected->SetBoolField(TEXT("camera_deprojection_kind"), true);
				auto Error = NativeFact(TEXT("world_spatial_dimension_bridge_evidence_required"),
					TEXT("A 2D-to-3D spatial result requires viewport projection and camera-deprojection bridge evidence."),
					TEXT("/observation/dimension_bridge_evidence"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
				Error.ActualEvidence = MakeShared<FJsonValueString>(NativeObservation.GetDimensionBridgeEvidence());
				return Fail(OutError, MoveTemp(Error));
			}
		}
		else if (!NativeObservation.GetDimensionBridgeEvidence().IsEmpty()
			&& Request.GetFamily()
				!= EUeAgentWorldSpatialPortFamily::ViewportProjection)
		{
			auto Error = NativeFact(TEXT("world_spatial_dimension_bridge_not_allowed"),
				FString(TEXT("Spatial port family ")) + FamilyId(Request.GetFamily()) + TEXT(" does not allow dimension-bridge evidence."),
				TEXT("/observation/dimension_bridge_evidence"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			Error.ActualEvidence = MakeShared<FJsonValueString>(NativeObservation.GetDimensionBridgeEvidence());
			return Fail(OutError, MoveTemp(Error));
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
