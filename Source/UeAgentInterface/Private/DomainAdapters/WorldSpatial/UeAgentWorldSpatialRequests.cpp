#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <initializer_list>

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceChargeError;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentContractValue;

	namespace
	{
		bool ValueFailure(FUeAgentWorldSpatialExecutionError& Error, const TCHAR* Code,
			const TCHAR* Reason, const FString& Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, bool bPublicValue = true)
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.PropertyPath = Path;
			Error.SourceKind = TEXT("world_spatial_value");
			Error.Source.JsonPath = Path;
			Error.ActualValuePresent = Actual.IsValid();
			if (bPublicValue) Error.ActualValue = Actual;
			else Error.ActualEvidence = Actual;
			Error.ExpectedValue = Expected;
			return false;
		}

		bool ValueText(const FString& Value, const TCHAR* Path, const TCHAR* Code,
			FUeAgentWorldSpatialExecutionError& Error, bool bPublicValue = true)
		{
			return !Value.IsEmpty() || ValueFailure(Error, Code, TEXT("The value must be a non-empty string."), Path,
				MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicValue);
		}

		bool ValueChoice(const FString& Value, std::initializer_list<const TCHAR*> Choices,
			const TCHAR* Path, const TCHAR* Code, FUeAgentWorldSpatialExecutionError& Error)
		{
			for (const TCHAR* Choice : Choices) if (Value == Choice) return true;
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const TCHAR* Choice : Choices) Expected.Add(MakeShared<FJsonValueString>(Choice));
			return ValueFailure(Error, Code, TEXT("The value is not one of the supported choices."), Path,
				MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueArray>(MoveTemp(Expected)));
		}

		TSharedPtr<FJsonValue> DiagnosticNumber(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(FMath::IsNaN(Value) ? TEXT("NaN") : Value < 0.0 ? TEXT("-Infinity") : TEXT("Infinity"));
		}

		bool ValueNumber(bool bAccepted, double Value, const TCHAR* Constraint,
			const TCHAR* Path, const TCHAR* Code, FUeAgentWorldSpatialExecutionError& Error)
		{
			if (bAccepted) return true;
			return ValueFailure(Error, Code, TEXT("The value does not satisfy the required numeric constraint."), Path,
				DiagnosticNumber(Value), MakeShared<FJsonValueString>(Constraint));
		}

		bool ValuePresent(bool bPresent, const TCHAR* Path, const TCHAR* Code,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return bPresent || ValueFailure(Error, Code, TEXT("This value is required by the selected operation."), Path,
				nullptr, MakeShared<FJsonValueString>(TEXT("present value")));
		}

		template <typename ValueType>
		bool ValidateChild(const ValueType& Value, const TCHAR* Path, FUeAgentWorldSpatialExecutionError& Error)
		{
			if (Value.TryValidate(Error)) return true;
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Path);
			return false;
		}

		template <typename ValueType>
		bool ValidateParsedValue(const TSharedPtr<FJsonObject>& Object, const ValueType& Value,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (Value.TryValidate(Error)) return true;
			// The compact spatial schema places frame members alongside subject and components.
			TArray<FString> Segments;
			Error.Source.JsonPath.ParseIntoArray(Segments, TEXT("/"), true);
			TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(Object);
			FString InputPath;
			for (const FString& Segment : Segments)
			{
				const TSharedPtr<FJsonObject> CurrentObject = Current.IsValid() && Current->Type == EJson::Object ? Current->AsObject() : nullptr;
				if (Segment == TEXT("frame") && CurrentObject.IsValid() && CurrentObject->HasField(TEXT("frame_id"))) continue;
				InputPath += TEXT("/") + Segment;
				if (CurrentObject.IsValid()) Current = CurrentObject->TryGetField(Segment);
				else if (Current.IsValid() && Current->Type == EJson::Array)
				{
					int32 Index = INDEX_NONE;
					const auto& Items = Current->AsArray();
					Current = LexTryParseString(Index, *Segment) && Items.IsValidIndex(Index) ? Items[Index] : nullptr;
				}
				else Current.Reset();
			}
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = InputPath.IsEmpty() ? TEXT("/") : InputPath;
			return false;
		}

		template <typename ValueType, typename ParserType>
		bool ParseAt(const TSharedPtr<FJsonObject>& Object, ValueType& Value, ParserType Parser,
			FUeAgentWorldSpatialExecutionError& Error, const TCHAR* Path)
		{
			if (Parser(Object, Value, Error)) return true;
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Path);
			return false;
		}

		bool Fail(
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Detail, TEXT("native_execution"));
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
			OutError.SubjectId = SubjectId;
			return false;
		}

		bool InvalidField(FUeAgentWorldSpatialExecutionError& Error, const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field, const TCHAR* Expected, bool bPublicValue = true,
			const TCHAR* Code = TEXT("world_spatial_typed_field_invalid"))
		{
			const TSharedPtr<FJsonValue> Actual = Object ? Object->TryGetField(Field) : nullptr;
			Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code,
				TEXT("/") + FString(Field), MakeShared<FJsonValueString>(Expected), bPublicValue ? Actual : nullptr);
			if (!bPublicValue) Error.ActualEvidence = Actual;
			Error.ActualValuePresent = Object && Object->HasField(Field);
			Error.PublicMessage = TEXT("The request field is missing or has an incompatible value.");
			Error.SourceKind = TEXT("command_input");
			return false;
		}

		bool RequestValueFailure(FUeAgentWorldSpatialExecutionError& Error, const TCHAR* Code,
			const TCHAR* Reason, const FString& Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			ValueFailure(Error, Code, Reason, Path, Actual, Expected);
			Error.SourceKind = TEXT("command_input");
			return false;
		}

		bool SelectorFailure(FUeAgentWorldSpatialExecutionError& Error, const TCHAR* Code,
			std::initializer_list<TPair<FString, bool>> Selectors)
		{
			auto Actual = MakeShared<FJsonObject>();
			for (const auto& Selector : Selectors) Actual->SetBoolField(Selector.Key, Selector.Value);
			return RequestValueFailure(Error, Code, TEXT("Exactly one selector must be active."), TEXT("/"),
				MakeShared<FJsonValueObject>(MoveTemp(Actual)), MakeShared<FJsonValueString>(TEXT("exactly one active selector")));
		}

		bool ChargeDecode(
			FUeAgentResourceTracker& Resources,
			const FString& CapabilityId,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentResourceChargeError Error;
			if (!Resources.TryCharge(
					EUeAgentResourceDimension::WorkUnits,
					1,
					TEXT("world_spatial.request.decode/") + CapabilityId,
					Error))
			{
				OutError = MoveTemp(Error);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = CapabilityId;
				return false;
			}
			return true;
		}

		bool TryParseRoot(
			const FUeAgentContractValue& CanonicalInput,
			TSharedPtr<FJsonObject>& OutRoot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Contract::Json::TryParseObject(CanonicalInput.GetJsonValue(), OutRoot, ParseError))
			{
				OutError = MoveTemp(ParseError);
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("canonical_document");
				return false;
			}
			return true;
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& Out,
			FUeAgentWorldSpatialExecutionError& Error, bool bPublicValue = true)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, Out)
				|| Out.IsEmpty())
			{
				return InvalidField(Error, Object, Field, TEXT("non-empty string"), bPublicValue);
			}
			return true;
		}

		bool RequiredNumber(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			double& Out,
			FUeAgentWorldSpatialExecutionError& Error, bool bPublicValue = true)
		{
			if (!Object.IsValid() || !Object->TryGetNumberField(Field, Out))
			{
				return InvalidField(Error, Object, Field, TEXT("number"), bPublicValue);
			}
			return true;
		}

		bool RequiredBool(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			bool& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Object.IsValid() || !Object->TryGetBoolField(Field, Out))
			{
				return InvalidField(Error, Object, Field, TEXT("boolean"));
			}
			return true;
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(Field, Found)
				|| !Found
				|| !Found->IsValid())
			{
				return InvalidField(Error, Object, Field, TEXT("object"), false);
			}
			Out = *Found;
			return true;
		}

		bool OptionalColdStartRenderSize(const TSharedPtr<FJsonObject>& Root,
			TOptional<FIntPoint>& Out, FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!Root->HasField(TEXT("cold_start_render_size"))) return true;
			TSharedPtr<FJsonObject> Size;
			if (!RequiredObject(Root, TEXT("cold_start_render_size"), Size, Error)) return false;
			int32 Width = 0, Height = 0;
			if (!Size->TryGetNumberField(TEXT("width"), Width) || !Size->TryGetNumberField(TEXT("height"), Height)
				|| Width < 1 || Width > 4096 || Height < 1 || Height > 4096)
				return InvalidField(Error, Root, TEXT("cold_start_render_size"), TEXT("integer width/height in 1..4096"), false);
			Out = FIntPoint(Width, Height);
			return true;
		}

		bool RequiredArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const TArray<TSharedPtr<FJsonValue>>*& Out,
			FUeAgentWorldSpatialExecutionError& Error, bool bPublicValue = false)
		{
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Out)
				|| !Out)
			{
				return InvalidField(Error, Object, Field, TEXT("array"), bPublicValue);
			}
			return true;
		}

		bool StringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& Out,
			FUeAgentWorldSpatialExecutionError& Error, bool bPublicValue = true)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!RequiredArray(Object, Field, Values, Error, bPublicValue))
			{
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid()
					|| !Value->TryGetString(Text)
					|| Text.IsEmpty())
				{
					Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_spatial_typed_array_item_invalid"),
						TEXT("/") + FString(Field) + TEXT("/") + LexToString(Index), MakeShared<FJsonValueString>(TEXT("non-empty string")), bPublicValue ? Value : nullptr);
					if (!bPublicValue) Error.ActualEvidence = Value;
					Error.ActualValuePresent = Value.IsValid();
					Error.SourceKind = TEXT("command_input");
					Error.PublicMessage = TEXT("This array element must be a non-empty string.");
					return false;
				}
				Out.Add(MoveTemp(Text));
			}
			return true;
		}

		bool ParseRevision(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialRevision& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			double Sequence = -1.0;
			if (!RequiredString(Object, TEXT("source_id"), Out.SourceId, Error, false)
				|| !RequiredString(Object, TEXT("generation"), Out.Generation, Error, false)
				|| !RequiredNumber(Object, TEXT("sequence"), Sequence, Error, false))
			{
				return false;
			}
			Out.Sequence = static_cast<int64>(Sequence);
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseTemporalPoint(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialTemporalPoint& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredString(Object, TEXT("clock_id"), Out.ClockId, Error)
				|| !RequiredString(Object, TEXT("tick"), Out.Tick, Error)
				|| !RequiredString(Object, TEXT("rate_numerator"), Out.RateNumerator, Error)
				|| !RequiredString(Object, TEXT("rate_denominator"), Out.RateDenominator, Error)
				|| !RequiredString(Object, TEXT("state"), Out.State, Error))
			{
				return false;
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseFrame(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialFrame& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			double Dimension = 0.0;
			TSharedPtr<FJsonObject> ObservedAt;
			if (!Object->HasField(TEXT("basis_frame_id"))
				&& !Object->HasField(TEXT("basis_evidence_id"))
				&& !Object->HasField(TEXT("observed_at")))
			{
				Out.bAuthored = true;
				if (!RequiredString(Object, TEXT("frame_id"), Out.FrameId, Error)
					|| !RequiredNumber(Object, TEXT("dimension"), Dimension, Error)
					|| !RequiredString(Object, TEXT("unit"), Out.Unit, Error)) return false;
				Out.Dimension = static_cast<int32>(Dimension);
				return ValidateParsedValue(Object, Out, Error);
			}
			if (!RequiredString(Object, TEXT("frame_id"), Out.FrameId, Error)
				|| !RequiredString(Object, TEXT("basis_frame_id"), Out.BasisFrameId, Error)
				|| !RequiredNumber(Object, TEXT("dimension"), Dimension, Error)
				|| !RequiredString(Object, TEXT("unit"), Out.Unit, Error)
				|| !RequiredString(Object, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error, false)
				|| !RequiredObject(Object, TEXT("observed_at"), ObservedAt, Error))
			{
				return false;
			}
			if (!ParseTemporalPoint(ObservedAt, Out.ObservedAt, Error))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/observed_at"));
				return false;
			}
			Out.Dimension = static_cast<int32>(Dimension);
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseSpatialValue(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialValue& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Frame;
			if (Object->HasField(TEXT("frame_id")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
				if (!RequiredString(Object, TEXT("subject"), Out.Subject, Error)
					|| !RequiredString(Object, TEXT("quantity"), Out.Quantity, Error)
					|| !ParseFrame(Object, Out.Frame, Error)
					|| !RequiredArray(Object, TEXT("components"), Components, Error)) return false;
				for (int32 Index = 0; Index < Components->Num(); ++Index)
				{
					const auto& Component = (*Components)[Index];
					double Number = 0.0;
					FString Lexeme;
					if (Component.IsValid() && Component->Type == EJson::Number && Component->TryGetNumber(Number)
						&& FMath::IsFinite(Number))
					{
						Lexeme = FString::Printf(TEXT("%.17g"), Number);
					}
					else if (!Component.IsValid() || !Component->TryGetString(Lexeme) || Lexeme.IsEmpty())
					{
						Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_spatial_typed_array_item_invalid"),
							TEXT("/components/") + LexToString(Index), MakeShared<FJsonValueString>(TEXT("finite number or non-empty string")), Component);
						Error.PublicMessage = TEXT("A component must be a finite number or a non-empty string.");
						Error.ActualValuePresent = Component.IsValid();
						Error.SourceKind = TEXT("command_input");
						return false;
					}
					Out.Components.Add(MoveTemp(Lexeme));
				}
				return ValidateParsedValue(Object, Out, Error);
			}
			if (!RequiredString(Object, TEXT("subject"), Out.Subject, Error)
				|| !RequiredString(Object, TEXT("quantity"), Out.Quantity, Error)
				|| !RequiredObject(Object, TEXT("frame"), Frame, Error)) return false;
			if (!ParseFrame(Frame, Out.Frame, Error))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/frame"));
				return false;
			}
			if (!StringArray(Object, TEXT("components"), Out.Components, Error)
				|| !StringArray(Object, TEXT("conversion_evidence_ids"), Out.ConversionEvidenceIds, Error, false))
			{
				return false;
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseExpectedRevisionEntry(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialExpectedRevisionEntry& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Revision;
			if (!RequiredString(Object, TEXT("target_ref"), Out.TargetRef, Error)
				|| !RequiredObject(Object, TEXT("revision"), Revision, Error)) return false;
			if (!ParseRevision(Revision, Out.Revision, Error))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/revision"));
				return false;
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		template <typename TValue, typename FParser>
		bool ObjectArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<TValue>& Out,
			FParser&& Parser,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!RequiredArray(Object, Field, Values, Error))
			{
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				const TSharedPtr<FJsonObject> Item =
					Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				TValue Parsed;
				if (!Item.IsValid() || !Parser(Item, Parsed, Error))
				{
					if (!Item || Error.IsEmpty())
					{
						Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("world_spatial_typed_array_item_invalid"),
							TEXT("/"), MakeShared<FJsonValueString>(TEXT("valid request object")), nullptr);
						Error.ActualEvidence = Value;
						Error.ActualValuePresent = Value.IsValid();
						Error.SourceKind = TEXT("command_input");
						Error.PublicMessage = !Item ? TEXT("This array element must be an object.")
							: TEXT("The array item parser failed without supplying a diagnostic.");
					}
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + FString(Field) + TEXT("/") + LexToString(Index));
					return false;
				}
				Out.Add(MoveTemp(Parsed));
			}
			return true;
		}

		bool ParseGraphViewState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialGraphViewState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Location;
			return RequiredObject(Object, TEXT("location"), Location, Error)
				&& ParseAt(Location, Out.Location, ParseSpatialValue, Error, TEXT("/location"))
				&& RequiredNumber(Object, TEXT("zoom"), Out.Zoom, Error)
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseNodePlacement(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialNodePlacement& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Position;
			return RequiredString(Object, TEXT("node_ref"), Out.NodeRef, Error)
				&& RequiredObject(Object, TEXT("position"), Position, Error)
				&& ParseAt(Position, Out.Position, ParseSpatialValue, Error, TEXT("/position"))
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseLayoutRelation(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialLayoutRelation& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredString(Object, TEXT("source_node_ref"), Out.SourceNodeRef, Error)
				|| !RequiredString(Object, TEXT("target_node_ref"), Out.TargetNodeRef, Error)
				|| !RequiredString(Object, TEXT("relation"), Out.Relation, Error))
			{
				return false;
			}
			if (Object->HasField(TEXT("gap")))
			{
				double Gap = 0.0;
				if (!RequiredNumber(Object, TEXT("gap"), Gap, Error))
				{
					return false;
				}
				Out.Gap = Gap;
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseAlignment(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialAlignment& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> VerifyFrame;
			if (!RequiredString(Object, TEXT("kind"), Out.Kind, Error)
				|| !RequiredObject(Object, TEXT("verify_frame"), VerifyFrame, Error)
				|| !ParseAt(VerifyFrame, Out.VerifyFrame, ParseFrame, Error, TEXT("/verify_frame")))
			{
				return false;
			}
			Object->TryGetStringField(TEXT("rotation_policy"), Out.RotationPolicy);
			Object->TryGetStringField(TEXT("axis"), Out.Axis);
			Object->TryGetStringField(TEXT("source_anchor"), Out.SubjectBoundsAnchor);
			Object->TryGetStringField(TEXT("target_anchor"), Out.ReferenceBoundsAnchor);
			Object->TryGetStringField(TEXT("source_face"), Out.SubjectFace);
			Object->TryGetStringField(TEXT("target_face"), Out.ReferenceFace);
			Object->TryGetNumberField(TEXT("offset_cm"), Out.OffsetCm);
			double Index = 0.0;
			if (Object->TryGetNumberField(TEXT("source_vertex_index"), Index))
			{
				Out.SubjectVertexIndex = static_cast<int32>(Index);
			}
			if (Object->TryGetNumberField(TEXT("target_vertex_index"), Index))
			{
				Out.ReferenceVertexIndex = static_cast<int32>(Index);
			}
			const TSharedPtr<FJsonObject>* AnchorObject = nullptr;
			if (Object->TryGetObjectField(TEXT("subject_anchor"), AnchorObject)
				&& AnchorObject && AnchorObject->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseAt(*AnchorObject, Value, ParseSpatialValue, Error, TEXT("/subject_anchor")))
				{
					return false;
				}
				Out.SubjectAnchor = MoveTemp(Value);
			}
			AnchorObject = nullptr;
			if (Object->TryGetObjectField(TEXT("reference_anchor"), AnchorObject)
				&& AnchorObject && AnchorObject->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseAt(*AnchorObject, Value, ParseSpatialValue, Error, TEXT("/reference_anchor")))
				{
					return false;
				}
				Out.ReferenceAnchor = MoveTemp(Value);
			}
			const TSharedPtr<FJsonObject>* Optional = nullptr;
			if (Object->TryGetObjectField(TEXT("subject_normal"), Optional)
				&& Optional && Optional->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseAt(*Optional, Value, ParseSpatialValue, Error, TEXT("/subject_normal")))
				{
					return false;
				}
				Out.SubjectNormal = MoveTemp(Value);
			}
			Optional = nullptr;
			if (Object->TryGetObjectField(TEXT("reference_normal"), Optional)
				&& Optional && Optional->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseAt(*Optional, Value, ParseSpatialValue, Error, TEXT("/reference_normal")))
				{
					return false;
				}
				Out.ReferenceNormal = MoveTemp(Value);
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseSurfaceQuery(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialSurfaceQuery& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Origin;
			TSharedPtr<FJsonObject> Direction;
			return RequiredObject(Object, TEXT("origin"), Origin, Error)
				&& ParseAt(Origin, Out.Origin, ParseSpatialValue, Error, TEXT("/origin"))
				&& RequiredObject(Object, TEXT("direction"), Direction, Error)
				&& ParseAt(Direction, Out.Direction, ParseSpatialValue, Error, TEXT("/direction"))
				&& RequiredNumber(Object, TEXT("max_distance_cm"), Out.MaxDistanceCm, Error)
				&& RequiredString(Object, TEXT("trace_channel"), Out.TraceChannel, Error)
				&& RequiredBool(Object, TEXT("trace_complex"), Out.bTraceComplex, Error)
				&& StringArray(Object, TEXT("ignore_target_refs"), Out.IgnoreTargetRefs, Error)
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParsePlacementPolicy(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialPlacementPolicy& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredNumber(Object, TEXT("clearance_cm"), Out.ClearanceCm, Error)
				&& RequiredBool(Object, TEXT("align_to_surface_normal"), Out.bAlignToSurfaceNormal, Error)
				&& RequiredBool(Object, TEXT("preserve_yaw"), Out.bPreserveYaw, Error)
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseTraceQuery(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialTraceQuery& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredNumber(Object, TEXT("max_distance_cm"), Out.MaxDistanceCm, Error)
				&& RequiredString(Object, TEXT("trace_channel"), Out.TraceChannel, Error)
				&& RequiredBool(Object, TEXT("trace_complex"), Out.bTraceComplex, Error)
				&& StringArray(Object, TEXT("ignore_target_refs"), Out.IgnoreTargetRefs, Error)
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseFramePolicy(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialFramePolicy& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			if (!RequiredNumber(Object, TEXT("padding_ratio"), Out.PaddingRatio, Error)
				|| !RequiredNumber(Object, TEXT("minimum_distance_cm"), Out.MinimumDistanceCm, Error)
				|| !RequiredNumber(Object, TEXT("maximum_distance_cm"), Out.MaximumDistanceCm, Error)
				|| !RequiredBool(Object, TEXT("preserve_rotation"), Out.bPreserveRotation, Error))
			{
				return false;
			}
			Object->TryGetBoolField(TEXT("collision_aware"), Out.bCollisionAware);
			Object->TryGetBoolField(TEXT("look_at"), Out.bLookAt);
			Object->TryGetBoolField(TEXT("auto_fallback"), Out.bAutoFallback);
			return ValidateParsedValue(Object, Out, Error);
		}

		bool ParseViewportInteractionState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportInteractionState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			return RequiredBool(Object, TEXT("mode_widgets"), Out.bModeWidgets, Error)
				&& RequiredBool(Object, TEXT("selection"), Out.bSelection, Error)
				&& RequiredBool(Object, TEXT("pivot"), Out.bPivot, Error)
				&& RequiredBool(Object, TEXT("editor_primitives"), Out.bEditorPrimitives, Error)
				&& RequiredBool(Object, TEXT("game_primitives"), Out.bGamePrimitives, Error)
				&& RequiredString(Object, TEXT("widget_mode"), Out.WidgetMode, Error)
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseViewportCameraState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportCameraState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			TSharedPtr<FJsonObject> ObservedAt;
			return RequiredObject(Object, TEXT("camera_transform"), Transform, Error)
				&& ParseAt(Transform, Out.CameraTransform, ParseSpatialValue, Error, TEXT("/camera_transform"))
				&& RequiredNumber(Object, TEXT("fov_degrees"), Out.FovDegrees, Error)
				&& RequiredString(Object, TEXT("view_mode"), Out.ViewMode, Error)
				&& (!Object->HasField(TEXT("observed_at"))
					|| (RequiredObject(Object, TEXT("observed_at"), ObservedAt, Error)
						&& ParseAt(ObservedAt, Out.ObservedAt, ParseTemporalPoint, Error, TEXT("/observed_at"))))
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseViewportState(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportState& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Camera;
			TSharedPtr<FJsonObject> Interaction;
			TSharedPtr<FJsonObject> Revision;
			return RequiredString(Object, TEXT("viewport_ref"), Out.ViewportRef, Error)
				&& RequiredObject(Object, TEXT("camera"), Camera, Error)
				&& ParseAt(Camera, Out.Camera, ParseViewportCameraState, Error, TEXT("/camera"))
				&& RequiredBool(Object, Object->HasField(TEXT("realtime_configured"))
					? TEXT("realtime_configured") : TEXT("realtime"), Out.bRealtime, Error)
				&& RequiredBool(Object, TEXT("game_view"), Out.bGameView, Error)
				&& StringArray(Object, TEXT("show_flags"), Out.ShowFlags, Error)
				&& RequiredObject(Object, TEXT("interaction"), Interaction, Error)
				&& ParseAt(Interaction, Out.Interaction, ParseViewportInteractionState, Error, TEXT("/interaction"))
				&& (!Object->HasField(TEXT("observed_revision"))
					|| (RequiredObject(Object, TEXT("observed_revision"), Revision, Error)
						&& ParseAt(Revision, Out.ObservedRevision, ParseRevision, Error, TEXT("/observed_revision"))))
				&& (ValidateParsedValue(Object, Out, Error));
		}

		bool ParseViewportStatePatch(
			const TSharedPtr<FJsonObject>& Object,
			FWorldSpatialViewportStatePatch& Out,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			if (Object->TryGetObjectField(TEXT("camera_transform"), Transform)
				&& Transform && Transform->IsValid())
			{
				FWorldSpatialValue Value;
				if (!ParseAt(*Transform, Value, ParseSpatialValue, Error, TEXT("/camera_transform")))
				{
					return false;
				}
				Out.CameraTransform = MoveTemp(Value);
			}
			FString CameraTransformMode;
			if (Object->TryGetStringField(
					TEXT("camera_transform_mode"), CameraTransformMode))
			{
				Out.CameraTransformMode = MoveTemp(CameraTransformMode);
			}
			double Number = 0.0;
			if (Object->TryGetNumberField(TEXT("fov_degrees"), Number))
			{
				Out.FovDegrees = Number;
			}
			FString Text;
			if (Object->TryGetStringField(TEXT("view_mode"), Text))
			{
				Out.ViewMode = MoveTemp(Text);
			}
			bool Bool = false;
			if (Object->TryGetBoolField(TEXT("realtime"), Bool))
			{
				Out.Realtime = Bool;
			}
			if (Object->TryGetBoolField(TEXT("game_view"), Bool))
			{
				Out.GameView = Bool;
			}
			if (Object->HasField(TEXT("show_flags")))
			{
				TArray<FString> Flags;
				if (!StringArray(Object, TEXT("show_flags"), Flags, Error))
				{
					return false;
				}
				Out.ShowFlags = MoveTemp(Flags);
			}
			const TSharedPtr<FJsonObject>* Interaction = nullptr;
			if (Object->TryGetObjectField(TEXT("interaction"), Interaction)
				&& Interaction && Interaction->IsValid())
			{
				FWorldSpatialViewportInteractionState Value;
				if (!ParseAt(*Interaction, Value, ParseViewportInteractionState, Error, TEXT("/interaction")))
				{
					return false;
				}
				Out.Interaction = MoveTemp(Value);
			}
			return ValidateParsedValue(Object, Out, Error);
		}

		template <typename TPayload, typename FDecode>
		bool DecodeAs(
			const TSharedPtr<FJsonObject>& Root,
			FUeAgentWorldSpatialRequestPayload& OutPayload,
			FDecode&& Decode,
			FUeAgentWorldSpatialExecutionError& Error)
		{
			TPayload Payload;
			if (!Decode(Payload))
			{
				return false;
			}
			OutPayload.Set<TPayload>(MoveTemp(Payload));
			return true;
		}

		using FPayloadDecoder = TFunction<bool(
			const TSharedPtr<FJsonObject>&,
			FUeAgentWorldSpatialRequestPayload&,
			FUeAgentWorldSpatialExecutionError&)>;

		struct FRequestSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* TypeId;
			const TCHAR* FamilyId;
			const TCHAR* RequestKindId;
			bool bOperation;
			FPayloadDecoder Decode;
		};

		const TArray<FRequestSpec>& RequestSpecs()
		{
			static const TArray<FRequestSpec> Specs = {
				{TEXT("graph.view.read"), TEXT("world_spatial.graph_view.read.request.v1"), TEXT("graph_view"), TEXT("graph_view_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphViewReadRequestInput>(Root, Out, [&](FGraphViewReadRequestInput& P)
						{
							return !Root->HasField(TEXT("include_document_state"))
								|| Root->TryGetBoolField(
									TEXT("include_document_state"),
									P.bIncludeDocumentState)
								|| InvalidField(Error, Root, TEXT("include_document_state"), TEXT("boolean"), true,
									TEXT("world_spatial_graph_document_state_flag_invalid"));
						}, Error);
					}},
				{TEXT("graph.view.update"), TEXT("world_spatial.graph_view.update.request.v1"), TEXT("graph_view"), TEXT("graph_view_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphViewUpdateRequestInput>(Root, Out, [&](FGraphViewUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> ViewState;
							if (!RequiredObject(Root, TEXT("view_state"), ViewState, Error)
								|| !ParseAt(ViewState, P.ViewState, ParseGraphViewState, Error, TEXT("/view_state")))
							{
								return false;
							}
							if (Root->HasField(TEXT("update_document_state"))
								&& !Root->TryGetBoolField(
									TEXT("update_document_state"),
									P.bUpdateDocumentState))
							{
								return InvalidField(Error, Root, TEXT("update_document_state"), TEXT("boolean"), true,
									TEXT("world_spatial_graph_document_state_flag_invalid"));
							}
							if (Root->HasField(TEXT("save_after_update"))
								&& !Root->TryGetBoolField(
									TEXT("save_after_update"), P.bSaveAfterUpdate))
							{
								return InvalidField(Error, Root, TEXT("save_after_update"), TEXT("boolean"), true,
									TEXT("world_spatial_graph_document_state_flag_invalid"));
							}
							return !P.bSaveAfterUpdate || P.bUpdateDocumentState
								? true
								: RequestValueFailure(Error, TEXT("world_spatial_graph_save_requires_document_state"),
									TEXT("Saving after an update requires updating the document state."), TEXT("/update_document_state"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						}, Error);
					}},
				{TEXT("graph.layout.apply"), TEXT("world_spatial.graph_layout.apply.request.v1"), TEXT("graph_layout"), TEXT("graph_layout_apply"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FGraphLayoutApplyRequestInput>(Root, Out, [&](FGraphLayoutApplyRequestInput& P)
						{
							if (!RequiredString(Root, TEXT("graph_ref"), P.GraphRef, Error)
								|| !ObjectArray(Root, TEXT("node_placements"), P.NodePlacements, ParseNodePlacement, Error)
								)
							{
								return false;
							}
							return !Root->HasField(TEXT("layout_relations"))
								|| ObjectArray(Root, TEXT("layout_relations"), P.LayoutRelations, ParseLayoutRelation, Error);
						}, Error);
					}},
				{TEXT("world.object_transform.read"), TEXT("world_spatial.object_transform.read.request.v1"), TEXT("object_transform"), TEXT("object_transform_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FObjectTransformReadRequestInput>(Root, Out, [&](FObjectTransformReadRequestInput& P)
						{
							TSharedPtr<FJsonObject> Frame;
							return RequiredObject(Root, TEXT("verify_frame"), Frame, Error)
								&& ParseAt(Frame, P.VerifyFrame, ParseFrame, Error, TEXT("/verify_frame"));
						}, Error);
					}},
				{TEXT("world.object_transform.update"), TEXT("world_spatial.object_transform.update.request.v1"), TEXT("object_transform"), TEXT("object_transform_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectTransformUpdateRequestInput>(Root, Out, [&](FWorldObjectTransformUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Transform;
							return RequiredObject(Root, TEXT("transform"), Transform, Error)
								&& ParseAt(Transform, P.Transform, ParseSpatialValue, Error, TEXT("/transform"));
						}, Error);
					}},
				{TEXT("world.object_align"), TEXT("world_spatial.object_transform.align.request.v1"), TEXT("object_transform"), TEXT("object_align"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectAlignRequestInput>(Root, Out, [&](FWorldObjectAlignRequestInput& P)
						{
							TSharedPtr<FJsonObject> Alignment;
							if (!StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error)
								|| !RequiredObject(Root, TEXT("alignment"), Alignment, Error)
								|| !ParseAt(Alignment, P.Alignment, ParseAlignment, Error, TEXT("/alignment"))
								)
							{
								return false;
							}
							FString Anchor;
							if (Root->TryGetStringField(TEXT("anchor_ref"), Anchor))
							{
								P.AnchorRef = MoveTemp(Anchor);
							}
							return true;
						}, Error);
					}},
				{TEXT("world.object_snap_to_surface"), TEXT("world_spatial.object_transform.snap_to_surface.request.v1"), TEXT("object_transform"), TEXT("object_snap_to_surface"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FWorldObjectSnapToSurfaceRequestInput>(Root, Out, [&](FWorldObjectSnapToSurfaceRequestInput& P)
						{
							TSharedPtr<FJsonObject> Surface;
							TSharedPtr<FJsonObject> Policy;
							return StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error)
								&& RequiredObject(Root, TEXT("surface_query"), Surface, Error)
								&& ParseAt(Surface, P.SurfaceQuery, ParseSurfaceQuery, Error, TEXT("/surface_query"))
								&& RequiredObject(Root, TEXT("placement_policy"), Policy, Error)
								&& ParseAt(Policy, P.PlacementPolicy, ParsePlacementPolicy, Error, TEXT("/placement_policy"))
								;
						}, Error);
					}},
				{TEXT("mesh.transform.bake"), TEXT("world_spatial.object_transform.bake_mesh.request.v1"), TEXT("object_transform"), TEXT("mesh_transform_bake"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshTransformBakeRequestInput>(Root, Out, [&](FMeshTransformBakeRequestInput& P)
						{
							TSharedPtr<FJsonObject> Transform;
							if (!RequiredObject(Root, TEXT("transform"), Transform, Error)
								|| !ParseAt(Transform, P.Transform, ParseSpatialValue, Error, TEXT("/transform")))
							{
								return false;
							}
							bool Preserve = false;
							if (Root->TryGetBoolField(TEXT("preserve_placement"), Preserve))
							{
								P.PreservePlacement = Preserve;
							}
							return true;
						}, Error);
					}},
				{TEXT("mesh.pivot.update"), TEXT("world_spatial.object_transform.update_pivot.request.v1"), TEXT("object_transform"), TEXT("mesh_pivot_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshPivotUpdateRequestInput>(Root, Out, [&](FMeshPivotUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Pivot;
							if (!RequiredObject(Root, TEXT("pivot"), Pivot, Error)
								|| !ParseAt(Pivot, P.Pivot, ParseSpatialValue, Error, TEXT("/pivot")))
							{
								return false;
							}
							bool Preserve = false;
							if (Root->TryGetBoolField(TEXT("preserve_world_geometry"), Preserve))
							{
								P.PreserveWorldGeometry = Preserve;
							}
							return true;
						}, Error);
					}},
				{TEXT("viewport.ray.deproject"), TEXT("world_spatial.viewport_projection.deproject.request.v1"), TEXT("viewport_projection"), TEXT("viewport_ray_deproject"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportRayDeprojectRequestInput>(Root, Out, [&](FViewportRayDeprojectRequestInput& P)
						{
							TSharedPtr<FJsonObject> Value;
							return RequiredObject(Root, TEXT("screen_point"), Value, Error)
								&& ParseAt(Value, P.ScreenPoint, ParseSpatialValue, Error, TEXT("/screen_point"))
								&& OptionalColdStartRenderSize(Root, P.ColdStartRenderSize, Error);
						}, Error);
					}},
				{TEXT("viewport.ray.trace"), TEXT("world_spatial.viewport_projection.trace.request.v1"), TEXT("viewport_projection"), TEXT("viewport_ray_trace"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportRayTraceRequestInput>(Root, Out, [&](FViewportRayTraceRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							TSharedPtr<FJsonObject> Query;
							return RequiredObject(Root, TEXT("screen_point"), Point, Error)
								&& ParseAt(Point, P.ScreenPoint, ParseSpatialValue, Error, TEXT("/screen_point"))
								&& RequiredObject(Root, TEXT("trace_query"), Query, Error)
								&& ParseAt(Query, P.TraceQuery, ParseTraceQuery, Error, TEXT("/trace_query"))
								&& OptionalColdStartRenderSize(Root, P.ColdStartRenderSize, Error);
						}, Error);
					}},
				{TEXT("viewport.actor.pick"), TEXT("world_spatial.viewport_projection.pick_actor.request.v1"), TEXT("viewport_projection"), TEXT("viewport_actor_pick"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportActorPickRequestInput>(Root, Out, [&](FViewportActorPickRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							TSharedPtr<FJsonObject> Query;
							Root->TryGetBoolField(
								TEXT("allow_no_hit"), P.bAllowNoHit);
							return RequiredObject(Root, TEXT("screen_point"), Point, Error)
								&& ParseAt(Point, P.ScreenPoint, ParseSpatialValue, Error, TEXT("/screen_point"))
								&& RequiredObject(Root, TEXT("trace_query"), Query, Error)
								&& ParseAt(Query, P.TraceQuery, ParseTraceQuery, Error, TEXT("/trace_query"));
						}, Error);
					}},
				{TEXT("viewport.actor.select"), TEXT("world_spatial.viewport_projection.select_actor.request.v1"), TEXT("viewport_projection"), TEXT("viewport_actor_select"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportActorSelectRequestInput>(Root, Out, [&](FViewportActorSelectRequestInput& P)
						{
							const TSharedPtr<FJsonObject>* Point = nullptr;
							if (Root->TryGetObjectField(TEXT("screen_point"), Point)
								&& Point && Point->IsValid())
							{
								FWorldSpatialValue ParsedPoint;
								if (!ParseAt(*Point, ParsedPoint, ParseSpatialValue, Error, TEXT("/screen_point")))
								{
									return false;
								}
								P.ScreenPoint = MoveTemp(ParsedPoint);
							}
							if (Root->HasField(TEXT("actor_refs"))
								&& !StringArray(Root, TEXT("actor_refs"), P.ActorRefs, Error))
							{
								return false;
							}
							if (!RequiredString(Root, TEXT("selection_mode"), P.SelectionMode, Error)
								|| !P.HasExactlyOneSelector())
							{
								return Error.IsValid() ? false : SelectorFailure(Error,
									TEXT("world_spatial_viewport_actor_selection_selector_invalid"),
									{{TEXT("screen_point"), P.ScreenPoint.IsSet()}, {TEXT("actor_refs"), !P.ActorRefs.IsEmpty()}});
							}
							const TSharedPtr<FJsonObject>* Query = nullptr;
							if (Root->TryGetObjectField(TEXT("trace_query"), Query)
								&& Query && Query->IsValid())
							{
								FWorldSpatialTraceQuery Parsed;
								if (!ParseAt(*Query, Parsed, ParseTraceQuery, Error, TEXT("/trace_query")))
								{
									return false;
								}
								P.TraceQuery = MoveTemp(Parsed);
							}
							if (P.TraceQuery.IsSet() && !P.ScreenPoint.IsSet())
							{
								return RequestValueFailure(Error, TEXT("world_spatial_viewport_actor_selection_trace_without_screen"),
									TEXT("A trace query requires a screen point selector."), TEXT("/screen_point"), nullptr,
									MakeShared<FJsonValueString>(TEXT("screen point")));
							}
							return true;
						}, Error);
					}},
				{TEXT("mesh.element.select_by_ray"), TEXT("world_spatial.viewport_projection.select_mesh_element.request.v1"), TEXT("viewport_projection"), TEXT("mesh_element_select_by_ray"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FMeshElementSelectByRayRequestInput>(Root, Out, [&](FMeshElementSelectByRayRequestInput& P)
						{
							TSharedPtr<FJsonObject> Point;
							if (!RequiredObject(Root, TEXT("screen_point"), Point, Error)
								|| !ParseAt(Point, P.ScreenPoint, ParseSpatialValue, Error, TEXT("/screen_point"))
								|| !RequiredString(Root, TEXT("element_kind"), P.ElementKind, Error)
								|| !RequiredString(Root, TEXT("selection_mode"), P.SelectionMode, Error))
							{
								return false;
							}
							const TSharedPtr<FJsonObject>* Query = nullptr;
							if (Root->TryGetObjectField(TEXT("trace_query"), Query)
								&& Query && Query->IsValid())
							{
								FWorldSpatialTraceQuery Parsed;
								if (!ParseAt(*Query, Parsed, ParseTraceQuery, Error, TEXT("/trace_query")))
								{
									return false;
								}
								P.TraceQuery = MoveTemp(Parsed);
							}
							return true;
						}, Error);
					}},
				{TEXT("viewport.pixel_scale.read"), TEXT("world_spatial.viewport_projection.read_pixel_scale.request.v1"), TEXT("viewport_projection"), TEXT("viewport_pixel_scale_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportPixelScaleReadRequestInput>(Root, Out, [&](FViewportPixelScaleReadRequestInput& P)
						{
							TSharedPtr<FJsonObject> Value;
							return RequiredObject(Root, TEXT("sample_point"), Value, Error)
								&& ParseAt(Value, P.SamplePoint, ParseSpatialValue, Error, TEXT("/sample_point"));
						}, Error);
					}},
				{TEXT("viewport.camera.frame"), TEXT("world_spatial.viewport_session.frame_camera.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_camera_frame"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportCameraFrameRequestInput>(Root, Out, [&](FViewportCameraFrameRequestInput& P)
						{
							TSharedPtr<FJsonObject> Policy;
							if (Root->HasField(TEXT("subject_refs"))
								&& !StringArray(Root, TEXT("subject_refs"), P.SubjectRefs, Error))
							{
								return false;
							}
							FString Folder;
							if (Root->TryGetStringField(TEXT("folder_path"), Folder))
							{
								P.FolderPath = MoveTemp(Folder);
							}
							Root->TryGetBoolField(TEXT("include_child_folders"), P.bIncludeChildFolders);
							Root->TryGetBoolField(TEXT("use_current_selection"), P.bUseCurrentSelection);
							const TSharedPtr<FJsonObject>* WorldPoint = nullptr;
							if (Root->TryGetObjectField(TEXT("world_point"), WorldPoint)
								&& WorldPoint && WorldPoint->IsValid())
							{
								FWorldSpatialValue Point;
								if (!ParseAt(*WorldPoint, Point, ParseSpatialValue, Error, TEXT("/world_point")))
								{
									return false;
								}
								P.WorldPoint = MoveTemp(Point);
							}
							const TSharedPtr<FJsonObject>* WorldBounds = nullptr;
							if (Root->TryGetObjectField(TEXT("world_bounds"), WorldBounds)
								&& WorldBounds && WorldBounds->IsValid())
							{
								TSharedPtr<FJsonObject> Min;
								TSharedPtr<FJsonObject> Max;
								FWorldSpatialValue MinValue;
								FWorldSpatialValue MaxValue;
								if (!RequiredObject(*WorldBounds, TEXT("min"), Min, Error)
									|| !RequiredObject(*WorldBounds, TEXT("max"), Max, Error)
									|| !ParseAt(Min, MinValue, ParseSpatialValue, Error, TEXT("/min"))
									|| !ParseAt(Max, MaxValue, ParseSpatialValue, Error, TEXT("/max")))
								{
									UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/world_bounds"));
									return false;
								}
								P.WorldBoundsMin = MoveTemp(MinValue);
								P.WorldBoundsMax = MoveTemp(MaxValue);
							}
							return RequiredObject(Root, TEXT("frame_policy"), Policy, Error)
								&& ParseAt(Policy, P.FramePolicy, ParseFramePolicy, Error, TEXT("/frame_policy"))
								&& (P.HasExactlyOneSelector()
									|| SelectorFailure(Error, TEXT("world_spatial_viewport_frame_selector_invalid"),
										{{TEXT("subject_refs"), !P.SubjectRefs.IsEmpty()}, {TEXT("folder_path"), P.FolderPath.IsSet() && !P.FolderPath->IsEmpty()},
										 {TEXT("use_current_selection"), P.bUseCurrentSelection}, {TEXT("world_point"), P.WorldPoint.IsSet()},
										 {TEXT("world_bounds"), P.WorldBoundsMin.IsSet() && P.WorldBoundsMax.IsSet()}}));
						}, Error);
					}},
				{TEXT("viewport.state.read"), TEXT("world_spatial.viewport_session.read_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_read"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateReadRequestInput>(Root, Out, [](FViewportStateReadRequestInput&) { return true; }, Error);
					}},
				{TEXT("viewport.state.restore"), TEXT("world_spatial.viewport_session.restore_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_restore"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateRestoreRequestInput>(Root, Out, [&](FViewportStateRestoreRequestInput& P)
						{
							TSharedPtr<FJsonObject> Snapshot;
							return RequiredObject(Root, TEXT("snapshot"), Snapshot, Error)
								&& ParseAt(Snapshot, P.Snapshot, ParseViewportState, Error, TEXT("/snapshot"));
						}, Error);
					}},
				{TEXT("viewport.warning.list"), TEXT("world_spatial.viewport_session.list_warnings.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_warning_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportWarningListRequestInput>(Root, Out, [&](FViewportWarningListRequestInput& P)
						{
							FString Ref;
							if (Root->TryGetStringField(TEXT("viewport_ref"), Ref))
							{
								P.ViewportRef = MoveTemp(Ref);
							}
							Root->TryGetBoolField(TEXT("include_suppressed"), P.bIncludeSuppressed);
							return true;
						}, Error);
					}},
				{TEXT("viewport.view_mode.list"), TEXT("world_spatial.viewport_session.list_view_modes.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_view_mode_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportViewModeListRequestInput>(Root, Out, [&](FViewportViewModeListRequestInput& P)
						{
							Root->TryGetStringField(TEXT("query"), P.Query);
							P.Query.TrimStartAndEndInline();
							return true;
						}, Error);
					}},
				{TEXT("viewport.instance.list"), TEXT("world_spatial.viewport_session.list_instances.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_instance_list"), false,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportInstanceListRequestInput>(Root, Out, [](FViewportInstanceListRequestInput&) { return true; }, Error);
					}},
				{TEXT("viewport.state.update"), TEXT("world_spatial.viewport_session.update_state.request.v1"), TEXT("viewport_session_state"), TEXT("viewport_state_update"), true,
					[](const TSharedPtr<FJsonObject>& Root, FUeAgentWorldSpatialRequestPayload& Out, FUeAgentWorldSpatialExecutionError& Error)
					{
						return DecodeAs<FViewportStateUpdateRequestInput>(Root, Out, [&](FViewportStateUpdateRequestInput& P)
						{
							TSharedPtr<FJsonObject> Patch;
							return RequiredObject(Root, TEXT("state_patch"), Patch, Error)
								&& ParseAt(Patch, P.StatePatch, ParseViewportStatePatch, Error, TEXT("/state_patch"));
						}, Error);
					}},
			};
			return Specs;
		}

		void BindCompactInputSubjects(const TSharedPtr<FJsonObject>& Object, const FString& BoundSubject)
		{
			FString Subject = BoundSubject;
			// Node placement already names its owner separately from coordinates.
			Object->TryGetStringField(TEXT("node_ref"), Subject);
			if (Object->HasField(TEXT("quantity")) && Object->HasField(TEXT("frame_id"))
				&& Object->HasField(TEXT("components")))
			{
				Object->SetStringField(TEXT("subject"), Subject);
				return;
			}
			for (const auto& Entry : Object->Values)
			{
				if (Entry.Value->Type == EJson::Object)
				{
					BindCompactInputSubjects(Entry.Value->AsObject(), Subject);
				}
				else if (Entry.Value->Type == EJson::Array)
				{
					for (const auto& Item : Entry.Value->AsArray())
					{
						if (Item->Type == EJson::Object) BindCompactInputSubjects(Item->AsObject(), Subject);
					}
				}
			}
		}

		class FWorldSpatialRequestDecoder final
			: public IUeAgentWorldSpatialRequestDecoder
		{
		public:
			bool TryDecode(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentContractValue& CanonicalInput,
				FUeAgentResourceTracker& Resources,
				FUeAgentWorldSpatialTypedRequestPtr& OutRequest,
				FUeAgentWorldSpatialExecutionError& OutError,
				const FString& InputSubject) override
			{
				OutRequest.Reset();
				OutError = {};
				const FRequestSpec* Spec = RequestSpecs().FindByPredicate(
					[&Contract](const FRequestSpec& Candidate)
					{
						return Contract.GetCapabilityId() == Candidate.CapabilityId;
					});
				if (!Spec
					|| Contract.GetTypedRequestId() != Spec->TypeId
					|| Contract.GetFamilyId() != Spec->FamilyId
					|| Contract.RequiresNativeMutation() != Spec->bOperation)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_request_contract_mismatch"),
						Contract.GetCapabilityId(), TEXT("The request execution contract does not match the registered capability."));
				}
				if (!ChargeDecode(Resources, Contract.GetCapabilityId(), OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> Root;
				if (!TryParseRoot(CanonicalInput, Root, OutError))
				{
					return false;
				}
				FUeAgentWorldSpatialRequestPayload Payload;
				BindCompactInputSubjects(Root, InputSubject);
				if (!Spec->Decode(Root, Payload, OutError))
				{
					if (!OutError.IsValid())
					{
						Fail(
							OutError,
							TEXT("world_spatial_request_decode_failed"),
							Contract.GetCapabilityId(), TEXT("The request decoder failed without supplying a diagnostic."));
					}
					return false;
				}
				OutRequest = MakeShared<FUeAgentWorldSpatialRequest>(
					Spec->TypeId,
					Spec->FamilyId,
					Spec->RequestKindId,
					Spec->bOperation,
					MoveTemp(Payload));
				return OutRequest.IsValid() || Fail(OutError, TEXT("world_spatial_typed_request_unavailable"),
					Contract.GetCapabilityId(), TEXT("The decoder could not create the typed request."));
			}
		};
	}

	bool FWorldSpatialRevision::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_revision_invalid");
		if (!ValueText(SourceId, TEXT("/source_id"), Code, Error, false)
			|| !ValueText(Generation, TEXT("/generation"), Code, Error, false)) return false;
		return Sequence >= 0 || ValueFailure(Error, Code, TEXT("The revision sequence must be non-negative."), TEXT("/sequence"),
			MakeShared<FJsonValueString>(::LexToString(Sequence)), MakeShared<FJsonValueString>(TEXT("non-negative integer")), false);
	}

	bool FWorldSpatialTemporalPoint::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_temporal_point_invalid");
		return ValueText(ClockId, TEXT("/clock_id"), Code, Error)
			&& ValueText(Tick, TEXT("/tick"), Code, Error)
			&& ValueText(RateNumerator, TEXT("/rate_numerator"), Code, Error)
			&& ValueText(RateDenominator, TEXT("/rate_denominator"), Code, Error)
			&& ValueChoice(State, {TEXT("captured"), TEXT("frozen")}, TEXT("/state"), Code, Error);
	}

	bool FWorldSpatialFrame::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_frame_invalid");
		return ValueText(FrameId, TEXT("/frame_id"), Code, Error)
			&& ValueNumber(Dimension == 2 || Dimension == 3, Dimension, TEXT("2 or 3"), TEXT("/dimension"), Code, Error)
			&& ValueText(Unit, TEXT("/unit"), Code, Error)
			&& (bAuthored || (ValueText(BasisFrameId, TEXT("/basis_frame_id"), Code, Error)
				&& ValueText(BasisEvidenceId, TEXT("/basis_evidence_id"), Code, Error, false)
				&& ValidateChild(ObservedAt, TEXT("/observed_at"), Error)));
	}

	bool FWorldSpatialValue::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_value_invalid");
		const int32 ExpectedComponents = Quantity == TEXT("transform") ? 10 : Frame.Dimension;
		if (!ValueText(Subject, TEXT("/subject"), Code, Error)
			|| !ValueChoice(Quantity, {TEXT("point"), TEXT("direction"), TEXT("normal"), TEXT("transform")}, TEXT("/quantity"), Code, Error)
			|| !ValidateChild(Frame, TEXT("/frame"), Error)) return false;
		if (Components.Num() != ExpectedComponents)
			return ValueFailure(Error, Code, TEXT("The component count does not match the spatial quantity and frame dimension."), TEXT("/components"),
				MakeShared<FJsonValueNumber>(Components.Num()), MakeShared<FJsonValueNumber>(ExpectedComponents));
		for (int32 Index = 0; Index < Components.Num(); ++Index)
			if (!ValueText(Components[Index], *FString::Printf(TEXT("/components/%d"), Index), Code, Error)) return false;
		return true;
	}

	bool FWorldSpatialExpectedRevisionEntry::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		return ValueText(TargetRef, TEXT("/target_ref"), TEXT("world_spatial_expected_revision_invalid"), Error)
			&& ValidateChild(Revision, TEXT("/revision"), Error);
	}

	bool FWorldSpatialGraphViewState::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_graph_view_state_invalid");
		return ValidateChild(Location, TEXT("/location"), Error)
			&& ValueNumber(Location.Frame.Dimension == 2, Location.Frame.Dimension, TEXT("2"), TEXT("/location/frame/dimension"), Code, Error)
			&& ValueNumber(Zoom > 0.0, Zoom, TEXT("greater than zero"), TEXT("/zoom"), Code, Error);
	}

	bool FWorldSpatialNodePlacement::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_node_placement_invalid");
		return ValueText(NodeRef, TEXT("/node_ref"), Code, Error)
			&& ValidateChild(Position, TEXT("/position"), Error)
			&& ValueNumber(Position.Frame.Dimension == 2, Position.Frame.Dimension, TEXT("2"), TEXT("/position/frame/dimension"), Code, Error);
	}

	bool FWorldSpatialLayoutRelation::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_layout_relation_invalid");
		return ValueText(SourceNodeRef, TEXT("/source_node_ref"), Code, Error)
			&& ValueText(TargetNodeRef, TEXT("/target_node_ref"), Code, Error)
			&& ValueText(Relation, TEXT("/relation"), Code, Error)
			&& (!Gap.IsSet() || ValueNumber(Gap.GetValue() >= 0.0, Gap.GetValue(), TEXT("non-negative"), TEXT("/gap"), Code, Error));
	}

	bool FWorldSpatialAlignment::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_alignment_invalid");
		if (!ValidateChild(VerifyFrame, TEXT("/verify_frame"), Error)
			|| !ValueNumber(VerifyFrame.Dimension == 3, VerifyFrame.Dimension, TEXT("3"), TEXT("/verify_frame/dimension"), Code, Error)
			|| !ValueChoice(RotationPolicy, {TEXT("preserve"), TEXT("align_normals"), TEXT("align_frames")}, TEXT("/rotation_policy"), Code, Error)
			|| (SubjectNormal.IsSet() && !ValidateChild(SubjectNormal.GetValue(), TEXT("/subject_normal"), Error))
			|| (ReferenceNormal.IsSet() && !ValidateChild(ReferenceNormal.GetValue(), TEXT("/reference_normal"), Error))) return false;
		if (Kind == TEXT("anchors"))
			return ValuePresent(SubjectAnchor.IsSet(), TEXT("/subject_anchor"), Code, Error)
				&& ValidateChild(SubjectAnchor.GetValue(), TEXT("/subject_anchor"), Error)
				&& ValuePresent(ReferenceAnchor.IsSet(), TEXT("/reference_anchor"), Code, Error)
				&& ValidateChild(ReferenceAnchor.GetValue(), TEXT("/reference_anchor"), Error);
		if (Kind == TEXT("bounds"))
			return ValueChoice(Axis, {TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("/axis"), Code, Error)
				&& ValueChoice(SubjectBoundsAnchor, {TEXT("min"), TEXT("center"), TEXT("max")}, TEXT("/source_anchor"), Code, Error)
				&& ValueChoice(ReferenceBoundsAnchor, {TEXT("min"), TEXT("center"), TEXT("max")}, TEXT("/target_anchor"), Code, Error);
		if (Kind == TEXT("face"))
			return ValueText(SubjectFace, TEXT("/source_face"), Code, Error)
				&& ValueText(ReferenceFace, TEXT("/target_face"), Code, Error)
				&& ValueChoice(Axis, {TEXT(""), TEXT("x"), TEXT("y"), TEXT("z")}, TEXT("/axis"), Code, Error);
		if (Kind == TEXT("vertex"))
			return (!SubjectVertexIndex.IsSet() || ValueNumber(SubjectVertexIndex.GetValue() >= 0, SubjectVertexIndex.GetValue(), TEXT("non-negative integer"), TEXT("/source_vertex_index"), Code, Error))
				&& (!ReferenceVertexIndex.IsSet() || ValueNumber(ReferenceVertexIndex.GetValue() >= 0, ReferenceVertexIndex.GetValue(), TEXT("non-negative integer"), TEXT("/target_vertex_index"), Code, Error));
		return ValueChoice(Kind, {TEXT("anchors"), TEXT("bounds"), TEXT("face"), TEXT("vertex")}, TEXT("/kind"), Code, Error);
	}

	bool FWorldSpatialSurfaceQuery::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_surface_query_invalid");
		return ValidateChild(Origin, TEXT("/origin"), Error)
			&& ValidateChild(Direction, TEXT("/direction"), Error)
			&& ValueNumber(Origin.Frame.Dimension == 3, Origin.Frame.Dimension, TEXT("3"), TEXT("/origin/frame/dimension"), Code, Error)
			&& ValueNumber(Direction.Frame.Dimension == 3, Direction.Frame.Dimension, TEXT("3"), TEXT("/direction/frame/dimension"), Code, Error)
			&& ValueNumber(MaxDistanceCm > 0.0, MaxDistanceCm, TEXT("greater than zero"), TEXT("/max_distance_cm"), Code, Error)
			&& ValueText(TraceChannel, TEXT("/trace_channel"), Code, Error);
	}

	bool FWorldSpatialPlacementPolicy::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		return ValueNumber(ClearanceCm >= 0.0, ClearanceCm, TEXT("non-negative"), TEXT("/clearance_cm"), TEXT("world_spatial_placement_policy_invalid"), Error);
	}

	bool FWorldSpatialTraceQuery::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_trace_query_invalid");
		return ValueNumber(MaxDistanceCm > 0.0, MaxDistanceCm, TEXT("greater than zero"), TEXT("/max_distance_cm"), Code, Error)
			&& ValueText(TraceChannel, TEXT("/trace_channel"), Code, Error);
	}

	bool FWorldSpatialFramePolicy::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_frame_policy_invalid");
		if (!ValueNumber(PaddingRatio >= 0.0, PaddingRatio, TEXT("non-negative"), TEXT("/padding_ratio"), Code, Error)
			|| !ValueNumber(MinimumDistanceCm >= 0.0, MinimumDistanceCm, TEXT("non-negative"), TEXT("/minimum_distance_cm"), Code, Error)
			|| !ValueNumber(MaximumDistanceCm > 0.0, MaximumDistanceCm, TEXT("greater than zero"), TEXT("/maximum_distance_cm"), Code, Error)) return false;
		return MaximumDistanceCm >= MinimumDistanceCm || ValueFailure(Error, Code,
			TEXT("The maximum distance must be at least the minimum distance."), TEXT("/maximum_distance_cm"),
			DiagnosticNumber(MaximumDistanceCm), DiagnosticNumber(MinimumDistanceCm));
	}

	bool FWorldSpatialViewportInteractionState::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		return ValueText(WidgetMode, TEXT("/widget_mode"), TEXT("world_spatial_viewport_interaction_state_invalid"), Error);
	}

	bool FWorldSpatialViewportCameraState::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_viewport_camera_state_invalid");
		return ValidateChild(CameraTransform, TEXT("/camera_transform"), Error)
			&& ValueNumber(CameraTransform.Frame.Dimension == 3, CameraTransform.Frame.Dimension, TEXT("3"), TEXT("/camera_transform/frame/dimension"), Code, Error)
			&& ValueNumber(FovDegrees > 0.0 && FovDegrees <= 179.0, FovDegrees, TEXT("greater than zero and at most 179 degrees"), TEXT("/fov_degrees"), Code, Error)
			&& ValueText(ViewMode, TEXT("/view_mode"), Code, Error)
			&& (CameraTransform.Frame.bAuthored || ValidateChild(ObservedAt, TEXT("/observed_at"), Error));
	}

	bool FWorldSpatialViewportState::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		return ValueText(ViewportRef, TEXT("/viewport_ref"), TEXT("world_spatial_viewport_state_invalid"), Error)
			&& ValidateChild(Camera, TEXT("/camera"), Error)
			&& ValidateChild(Interaction, TEXT("/interaction"), Error);
	}

	bool FWorldSpatialViewportStatePatch::TryValidate(FUeAgentWorldSpatialExecutionError& Error) const
	{
		Error.Reset();
		const TCHAR* Code = TEXT("world_spatial_viewport_state_patch_invalid");
		if (CameraTransform.IsSet()
			&& (!ValidateChild(CameraTransform.GetValue(), TEXT("/camera_transform"), Error)
				|| !ValuePresent(CameraTransformMode.IsSet(), TEXT("/camera_transform_mode"), Code, Error)
				|| !ValueChoice(CameraTransformMode.GetValue(), {TEXT("absolute"), TEXT("delta")}, TEXT("/camera_transform_mode"), Code, Error))) return false;
		return (!CameraTransformMode.IsSet() || ValuePresent(CameraTransform.IsSet(), TEXT("/camera_transform"), Code, Error))
			&& (!FovDegrees.IsSet() || ValueNumber(FovDegrees.GetValue() > 0.0 && FovDegrees.GetValue() <= 179.0, FovDegrees.GetValue(), TEXT("greater than zero and at most 179 degrees"), TEXT("/fov_degrees"), Code, Error))
			&& (!ViewMode.IsSet() || ValueText(ViewMode.GetValue(), TEXT("/view_mode"), Code, Error))
			&& (!Interaction.IsSet() || ValidateChild(Interaction.GetValue(), TEXT("/interaction"), Error));
	}

	bool FWorldSpatialRevision::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialTemporalPoint::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialFrame::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialValue::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialExpectedRevisionEntry::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialGraphViewState::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialNodePlacement::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialLayoutRelation::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialAlignment::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialSurfaceQuery::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialPlacementPolicy::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialTraceQuery::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialFramePolicy::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialViewportInteractionState::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialViewportCameraState::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialViewportState::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FWorldSpatialViewportStatePatch::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool FViewportActorSelectRequestInput::HasExactlyOneSelector() const
	{
		return static_cast<int32>(ScreenPoint.IsSet())
			+ static_cast<int32>(ActorRefs.Num() > 0) == 1;
	}

	bool FViewportCameraFrameRequestInput::HasExactlyOneSelector() const
	{
		const bool bHasBounds = WorldBoundsMin.IsSet() && WorldBoundsMax.IsSet();
		return static_cast<int32>(SubjectRefs.Num() > 0)
			+ static_cast<int32>(FolderPath.IsSet() && !FolderPath->IsEmpty())
			+ static_cast<int32>(bUseCurrentSelection)
			+ static_cast<int32>(WorldPoint.IsSet())
			+ static_cast<int32>(bHasBounds) == 1
			&& (WorldPoint.IsSet() ? WorldPoint->IsValid() : true)
			&& (bHasBounds
				? WorldBoundsMin->IsValid() && WorldBoundsMax->IsValid()
				: !WorldBoundsMin.IsSet() && !WorldBoundsMax.IsSet());
	}

	namespace
	{
		using UE::UAI::StateSemantic::FUeAgentCanonicalValue;
		using UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr;
		using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;
		using UE::UAI::StateSemantic::FUeAgentStateSemanticError;

		class FTypedIntentBuilder final
		{
		public:
			FTypedIntentBuilder(
				FUeAgentResourceTracker& InResources,
				FUeAgentWorldSpatialExecutionError& InError,
				const FString& InRequestKind)
				: Resources(InResources)
				, Error(InError)
				, RequestKind(InRequestKind)
			{
			}

			bool String(const FString& Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						Value,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Boolean(const bool Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryBoolean(
						Value,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Decimal(const double Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryDecimal(
						FString::SanitizeFloat(Value, 0),
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Integer(const int64 Value, FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryInteger(
						FString::Printf(TEXT("%lld"), Value),
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Object(
				const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool Array(
				const TArray<FUeAgentCanonicalValueRef>& Items,
				FUeAgentCanonicalValuePtr& Out)
			{
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						Out,
						StateError))
				{
					return FailBuild(StateError.Code);
				}
				return true;
			}

			bool StringArray(
				const TArray<FString>& Values,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const FString& Value : Values)
				{
					FUeAgentCanonicalValuePtr Item;
					if (!String(Value, Item))
					{
						return false;
					}
					Items.Add(Item.ToSharedRef());
				}
				return Array(Items, Out);
			}

			bool Temporal(
				const FWorldSpatialTemporalPoint& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				return AddString(Items, TEXT("clock_id"), Value.ClockId)
					&& AddString(Items, TEXT("tick"), Value.Tick)
					&& AddString(Items, TEXT("rate_numerator"), Value.RateNumerator)
					&& AddString(Items, TEXT("rate_denominator"), Value.RateDenominator)
					&& AddString(Items, TEXT("state"), Value.State)
					&& Object(Items, Out);
			}

			bool Frame(
				const FWorldSpatialFrame& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				if (Value.bAuthored)
				{
					return AddString(Items, TEXT("frame_id"), Value.FrameId)
						&& AddInteger(Items, TEXT("dimension"), Value.Dimension)
						&& AddString(Items, TEXT("unit"), Value.Unit)
						&& Object(Items, Out);
				}
				FUeAgentCanonicalValuePtr ObservedAt;
				return Temporal(Value.ObservedAt, ObservedAt)
					&& AddString(Items, TEXT("frame_id"), Value.FrameId)
					&& AddString(Items, TEXT("basis_frame_id"), Value.BasisFrameId)
					&& AddInteger(Items, TEXT("dimension"), Value.Dimension)
					&& AddString(Items, TEXT("unit"), Value.Unit)
					&& AddString(Items, TEXT("basis_evidence_id"), Value.BasisEvidenceId)
					&& Add(Items, TEXT("observed_at"), ObservedAt)
					&& Object(Items, Out);
			}

			bool SpatialValue(
				const FWorldSpatialValue& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				FUeAgentCanonicalValuePtr FrameValue;
				FUeAgentCanonicalValuePtr Components;
				FUeAgentCanonicalValuePtr Evidence;
				return Frame(Value.Frame, FrameValue)
					&& StringArray(Value.Components, Components)
					&& StringArray(Value.ConversionEvidenceIds, Evidence)
					&& AddString(Items, TEXT("subject"), Value.Subject)
					&& AddString(Items, TEXT("quantity"), Value.Quantity)
					&& Add(Items, TEXT("frame"), FrameValue)
					&& Add(Items, TEXT("components"), Components)
					&& Add(Items, TEXT("conversion_evidence_ids"), Evidence)
					&& Object(Items, Out);
			}

			bool Revision(
				const FWorldSpatialRevision& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				return AddString(Items, TEXT("source_id"), Value.SourceId)
					&& AddString(Items, TEXT("generation"), Value.Generation)
					&& AddInteger(Items, TEXT("sequence"), Value.Sequence)
					&& Object(Items, Out);
			}

			bool TraceQuery(
				const FWorldSpatialTraceQuery& Value,
				FUeAgentCanonicalValuePtr& Out)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				FUeAgentCanonicalValuePtr Ignore;
				return StringArray(Value.IgnoreTargetRefs, Ignore)
					&& AddDecimal(Items, TEXT("max_distance_cm"), Value.MaxDistanceCm)
					&& AddString(Items, TEXT("trace_channel"), Value.TraceChannel)
					&& AddBoolean(Items, TEXT("trace_complex"), Value.bTraceComplex)
					&& Add(Items, TEXT("ignore_target_refs"), Ignore)
					&& Object(Items, Out);
			}

			bool Add(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const FUeAgentCanonicalValuePtr& Value)
			{
				if (!Value.IsValid())
				{
					return FailBuild(TEXT("typed_intent_value_missing"));
				}
				Items.Emplace(Name, Value.ToSharedRef());
				return true;
			}

			bool AddString(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const FString& Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return String(Value, Built) && Add(Items, Name, Built);
			}

			bool AddBoolean(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const bool Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Boolean(Value, Built) && Add(Items, Name, Built);
			}

			bool AddDecimal(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const double Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Decimal(Value, Built) && Add(Items, Name, Built);
			}

			bool AddInteger(
				TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
				const FString& Name,
				const int64 Value)
			{
				FUeAgentCanonicalValuePtr Built;
				return Integer(Value, Built) && Add(Items, Name, Built);
			}

		private:
			bool FailBuild(const FString& Detail)
			{
				Error.Code = TEXT("world_spatial_typed_intent_build_failed");
				Error.SubjectId = RequestKind;
				Error.PublicMessage = Detail;
				return false;
			}

			FUeAgentResourceTracker& Resources;
			FUeAgentWorldSpatialExecutionError& Error;
			FString RequestKind;
		};

		bool TryBuildRevisionEntries(
			FTypedIntentBuilder& Builder,
			const TArray<FWorldSpatialExpectedRevisionEntry>& Values,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FWorldSpatialExpectedRevisionEntry& Value : Values)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				FUeAgentCanonicalValuePtr Revision;
				FUeAgentCanonicalValuePtr Item;
				if (!Builder.Revision(Value.Revision, Revision)
					|| !Builder.AddString(Fields, TEXT("target_ref"), Value.TargetRef)
					|| !Builder.Add(Fields, TEXT("revision"), Revision)
					|| !Builder.Object(Fields, Item))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return Builder.Array(Items, Out);
		}

		bool TryBuildViewportState(
			FTypedIntentBuilder& Builder,
			const FWorldSpatialViewportState& Value,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CameraFields;
			FUeAgentCanonicalValuePtr CameraTransform;
			FUeAgentCanonicalValuePtr CameraObservedAt;
			FUeAgentCanonicalValuePtr Camera;
			if (!Builder.SpatialValue(Value.Camera.CameraTransform, CameraTransform)
				|| !Builder.Add(CameraFields, TEXT("camera_transform"), CameraTransform)
				|| !Builder.AddDecimal(CameraFields, TEXT("fov_degrees"), Value.Camera.FovDegrees)
				|| !Builder.AddString(CameraFields, TEXT("view_mode"), Value.Camera.ViewMode)
				|| (Value.Camera.ObservedAt.IsValid()
					&& (!Builder.Temporal(Value.Camera.ObservedAt, CameraObservedAt)
						|| !Builder.Add(CameraFields, TEXT("observed_at"), CameraObservedAt)))
				|| !Builder.Object(CameraFields, Camera))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> InteractionFields;
			FUeAgentCanonicalValuePtr Interaction;
			if (!Builder.AddBoolean(InteractionFields, TEXT("mode_widgets"), Value.Interaction.bModeWidgets)
				|| !Builder.AddBoolean(InteractionFields, TEXT("selection"), Value.Interaction.bSelection)
				|| !Builder.AddBoolean(InteractionFields, TEXT("pivot"), Value.Interaction.bPivot)
				|| !Builder.AddBoolean(InteractionFields, TEXT("editor_primitives"), Value.Interaction.bEditorPrimitives)
				|| !Builder.AddBoolean(InteractionFields, TEXT("game_primitives"), Value.Interaction.bGamePrimitives)
				|| !Builder.AddString(InteractionFields, TEXT("widget_mode"), Value.Interaction.WidgetMode)
				|| !Builder.Object(InteractionFields, Interaction))
			{
				return false;
			}
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			FUeAgentCanonicalValuePtr ShowFlags;
			return Builder.StringArray(Value.ShowFlags, ShowFlags)
				&& Builder.AddString(Fields, TEXT("viewport_ref"), Value.ViewportRef)
				&& Builder.Add(Fields, TEXT("camera"), Camera)
				&& Builder.AddBoolean(Fields, TEXT("realtime"), Value.bRealtime)
				&& Builder.AddBoolean(Fields, TEXT("game_view"), Value.bGameView)
				&& Builder.Add(Fields, TEXT("show_flags"), ShowFlags)
				&& Builder.Add(Fields, TEXT("interaction"), Interaction)
				&& Builder.Object(Fields, Out);
		}

		bool TryBuildTypedPayload(
			const FUeAgentWorldSpatialRequestPayload& Payload,
			FTypedIntentBuilder& Builder,
			FUeAgentCanonicalValuePtr& Out)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			if (Payload.IsType<FGraphViewUpdateRequestInput>())
			{
				const FGraphViewUpdateRequestInput& Input =
					Payload.Get<FGraphViewUpdateRequestInput>();
				const FWorldSpatialGraphViewState& Value = Input.ViewState;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> StateFields;
				FUeAgentCanonicalValuePtr Location;
				FUeAgentCanonicalValuePtr State;
				return Builder.SpatialValue(Value.Location, Location)
					&& Builder.Add(StateFields, TEXT("location"), Location)
					&& Builder.AddDecimal(StateFields, TEXT("zoom"), Value.Zoom)
					&& Builder.Object(StateFields, State)
					&& Builder.Add(Fields, TEXT("view_state"), State)
					&& Builder.AddBoolean(
						Fields, TEXT("update_document_state"),
						Input.bUpdateDocumentState)
					&& Builder.AddBoolean(
						Fields, TEXT("save_after_update"),
						Input.bSaveAfterUpdate)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FGraphLayoutApplyRequestInput>())
			{
				const FGraphLayoutApplyRequestInput& Value =
					Payload.Get<FGraphLayoutApplyRequestInput>();
				TArray<FUeAgentCanonicalValueRef> Placements;
				for (const FWorldSpatialNodePlacement& Placement : Value.NodePlacements)
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> ItemFields;
					FUeAgentCanonicalValuePtr Position;
					FUeAgentCanonicalValuePtr Item;
					if (!Builder.SpatialValue(Placement.Position, Position)
						|| !Builder.AddString(ItemFields, TEXT("node_ref"), Placement.NodeRef)
						|| !Builder.Add(ItemFields, TEXT("position"), Position)
						|| !Builder.Object(ItemFields, Item))
					{
						return false;
					}
					Placements.Add(Item.ToSharedRef());
				}
				TArray<FUeAgentCanonicalValueRef> Relations;
				for (const FWorldSpatialLayoutRelation& Relation : Value.LayoutRelations)
				{
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> ItemFields;
					FUeAgentCanonicalValuePtr Item;
					if (!Builder.AddString(ItemFields, TEXT("source_node_ref"), Relation.SourceNodeRef)
						|| !Builder.AddString(ItemFields, TEXT("target_node_ref"), Relation.TargetNodeRef)
						|| !Builder.AddString(ItemFields, TEXT("relation"), Relation.Relation)
						|| (Relation.Gap.IsSet()
							&& !Builder.AddDecimal(ItemFields, TEXT("gap"), Relation.Gap.GetValue()))
						|| !Builder.Object(ItemFields, Item))
					{
						return false;
					}
					Relations.Add(Item.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr PlacementArray;
				FUeAgentCanonicalValuePtr RelationArray;
				return Builder.Array(Placements, PlacementArray)
					&& Builder.Array(Relations, RelationArray)
					&& Builder.AddString(Fields, TEXT("graph_ref"), Value.GraphRef)
					&& Builder.Add(Fields, TEXT("node_placements"), PlacementArray)
					&& Builder.Add(Fields, TEXT("layout_relations"), RelationArray)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectTransformUpdateRequestInput>())
			{
				const FWorldObjectTransformUpdateRequestInput& Value =
					Payload.Get<FWorldObjectTransformUpdateRequestInput>();
				FUeAgentCanonicalValuePtr Transform;
				return Builder.SpatialValue(Value.Transform, Transform)
					&& Builder.Add(Fields, TEXT("transform"), Transform)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectAlignRequestInput>())
			{
				const FWorldObjectAlignRequestInput& Value =
					Payload.Get<FWorldObjectAlignRequestInput>();
				const FWorldSpatialAlignment& Alignment = Value.Alignment;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> AlignmentFields;
				FUeAgentCanonicalValuePtr VerifyFrame;
				FUeAgentCanonicalValuePtr AlignmentValue;
				FUeAgentCanonicalValuePtr Subjects;
				if (!Builder.Frame(Alignment.VerifyFrame, VerifyFrame)
					|| !Builder.AddString(AlignmentFields, TEXT("kind"), Alignment.Kind)
					|| !Builder.Add(AlignmentFields, TEXT("verify_frame"), VerifyFrame))
				{
					return false;
				}
				if (Alignment.SubjectAnchor.IsSet())
				{
					FUeAgentCanonicalValuePtr Anchor;
					if (!Builder.SpatialValue(Alignment.SubjectAnchor.GetValue(), Anchor)
						|| !Builder.Add(AlignmentFields, TEXT("subject_anchor"), Anchor))
					{
						return false;
					}
				}
				if (Alignment.ReferenceAnchor.IsSet())
				{
					FUeAgentCanonicalValuePtr Anchor;
					if (!Builder.SpatialValue(Alignment.ReferenceAnchor.GetValue(), Anchor)
						|| !Builder.Add(AlignmentFields, TEXT("reference_anchor"), Anchor))
					{
						return false;
					}
				}
				if (Alignment.SubjectNormal.IsSet())
				{
					FUeAgentCanonicalValuePtr Normal;
					if (!Builder.SpatialValue(Alignment.SubjectNormal.GetValue(), Normal)
						|| !Builder.Add(AlignmentFields, TEXT("subject_normal"), Normal))
					{
						return false;
					}
				}
				if (Alignment.ReferenceNormal.IsSet())
				{
					FUeAgentCanonicalValuePtr Normal;
					if (!Builder.SpatialValue(Alignment.ReferenceNormal.GetValue(), Normal)
						|| !Builder.Add(AlignmentFields, TEXT("reference_normal"), Normal))
					{
						return false;
					}
				}
				return (Alignment.Axis.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("axis"), Alignment.Axis))
					&& (Alignment.SubjectBoundsAnchor.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("source_anchor"), Alignment.SubjectBoundsAnchor))
					&& (Alignment.ReferenceBoundsAnchor.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("target_anchor"), Alignment.ReferenceBoundsAnchor))
					&& (Alignment.SubjectFace.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("source_face"), Alignment.SubjectFace))
					&& (Alignment.ReferenceFace.IsEmpty()
						|| Builder.AddString(AlignmentFields, TEXT("target_face"), Alignment.ReferenceFace))
					&& Builder.AddDecimal(AlignmentFields, TEXT("offset_cm"), Alignment.OffsetCm)
					&& (!Alignment.SubjectVertexIndex.IsSet()
						|| Builder.AddInteger(AlignmentFields, TEXT("source_vertex_index"), Alignment.SubjectVertexIndex.GetValue()))
					&& (!Alignment.ReferenceVertexIndex.IsSet()
						|| Builder.AddInteger(AlignmentFields, TEXT("target_vertex_index"), Alignment.ReferenceVertexIndex.GetValue()))
					&& Builder.AddString(AlignmentFields, TEXT("rotation_policy"), Alignment.RotationPolicy)
					&& Builder.Object(AlignmentFields, AlignmentValue)
					&& Builder.StringArray(Value.SubjectRefs, Subjects)
					&& Builder.Add(Fields, TEXT("subject_refs"), Subjects)
					&& Builder.Add(Fields, TEXT("alignment"), AlignmentValue)
					&& (!Value.AnchorRef.IsSet()
						|| Builder.AddString(Fields, TEXT("anchor_ref"), Value.AnchorRef.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FWorldObjectSnapToSurfaceRequestInput>())
			{
				const FWorldObjectSnapToSurfaceRequestInput& Value =
					Payload.Get<FWorldObjectSnapToSurfaceRequestInput>();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> QueryFields;
				FUeAgentCanonicalValuePtr Origin;
				FUeAgentCanonicalValuePtr Direction;
				FUeAgentCanonicalValuePtr Ignore;
				FUeAgentCanonicalValuePtr Query;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PolicyFields;
				FUeAgentCanonicalValuePtr Policy;
				FUeAgentCanonicalValuePtr Subjects;
				return Builder.SpatialValue(Value.SurfaceQuery.Origin, Origin)
					&& Builder.SpatialValue(Value.SurfaceQuery.Direction, Direction)
					&& Builder.StringArray(Value.SurfaceQuery.IgnoreTargetRefs, Ignore)
					&& Builder.Add(QueryFields, TEXT("origin"), Origin)
					&& Builder.Add(QueryFields, TEXT("direction"), Direction)
					&& Builder.AddDecimal(QueryFields, TEXT("max_distance_cm"), Value.SurfaceQuery.MaxDistanceCm)
					&& Builder.AddString(QueryFields, TEXT("trace_channel"), Value.SurfaceQuery.TraceChannel)
					&& Builder.AddBoolean(QueryFields, TEXT("trace_complex"), Value.SurfaceQuery.bTraceComplex)
					&& Builder.Add(QueryFields, TEXT("ignore_target_refs"), Ignore)
					&& Builder.Object(QueryFields, Query)
					&& Builder.AddDecimal(PolicyFields, TEXT("clearance_cm"), Value.PlacementPolicy.ClearanceCm)
					&& Builder.AddBoolean(PolicyFields, TEXT("align_to_surface_normal"), Value.PlacementPolicy.bAlignToSurfaceNormal)
					&& Builder.AddBoolean(PolicyFields, TEXT("preserve_yaw"), Value.PlacementPolicy.bPreserveYaw)
					&& Builder.Object(PolicyFields, Policy)
					&& Builder.StringArray(Value.SubjectRefs, Subjects)
					&& Builder.Add(Fields, TEXT("subject_refs"), Subjects)
					&& Builder.Add(Fields, TEXT("surface_query"), Query)
					&& Builder.Add(Fields, TEXT("placement_policy"), Policy)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshTransformBakeRequestInput>())
			{
				const FMeshTransformBakeRequestInput& Value =
					Payload.Get<FMeshTransformBakeRequestInput>();
				FUeAgentCanonicalValuePtr Transform;
				return Builder.SpatialValue(Value.Transform, Transform)
					&& Builder.Add(Fields, TEXT("transform"), Transform)
					&& (!Value.PreservePlacement.IsSet()
						|| Builder.AddBoolean(Fields, TEXT("preserve_placement"), Value.PreservePlacement.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshPivotUpdateRequestInput>())
			{
				const FMeshPivotUpdateRequestInput& Value =
					Payload.Get<FMeshPivotUpdateRequestInput>();
				FUeAgentCanonicalValuePtr Pivot;
				return Builder.SpatialValue(Value.Pivot, Pivot)
					&& Builder.Add(Fields, TEXT("pivot"), Pivot)
					&& (!Value.PreserveWorldGeometry.IsSet()
						|| Builder.AddBoolean(Fields, TEXT("preserve_world_geometry"), Value.PreserveWorldGeometry.GetValue()))
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportActorSelectRequestInput>())
			{
				const FViewportActorSelectRequestInput& Value =
					Payload.Get<FViewportActorSelectRequestInput>();
				if (Value.ScreenPoint.IsSet())
				{
					FUeAgentCanonicalValuePtr ScreenPoint;
					if (!Builder.SpatialValue(Value.ScreenPoint.GetValue(), ScreenPoint)
						|| !Builder.Add(Fields, TEXT("screen_point"), ScreenPoint))
					{
						return false;
					}
				}
				if (!Value.ActorRefs.IsEmpty())
				{
					FUeAgentCanonicalValuePtr Actors;
					if (!Builder.StringArray(Value.ActorRefs, Actors)
						|| !Builder.Add(Fields, TEXT("actor_refs"), Actors))
					{
						return false;
					}
				}
				if (!Builder.AddString(Fields, TEXT("selection_mode"), Value.SelectionMode))
				{
					return false;
				}
				if (Value.TraceQuery.IsSet())
				{
					FUeAgentCanonicalValuePtr Query;
					if (!Builder.TraceQuery(Value.TraceQuery.GetValue(), Query)
						|| !Builder.Add(Fields, TEXT("trace_query"), Query))
					{
						return false;
					}
				}
				return Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FMeshElementSelectByRayRequestInput>())
			{
				const FMeshElementSelectByRayRequestInput& Value =
					Payload.Get<FMeshElementSelectByRayRequestInput>();
				FUeAgentCanonicalValuePtr ScreenPoint;
				if (!Builder.SpatialValue(Value.ScreenPoint, ScreenPoint)
					|| !Builder.Add(Fields, TEXT("screen_point"), ScreenPoint)
					|| !Builder.AddString(Fields, TEXT("element_kind"), Value.ElementKind)
					|| !Builder.AddString(Fields, TEXT("selection_mode"), Value.SelectionMode))
				{
					return false;
				}
				if (Value.TraceQuery.IsSet())
				{
					FUeAgentCanonicalValuePtr Query;
					if (!Builder.TraceQuery(Value.TraceQuery.GetValue(), Query)
						|| !Builder.Add(Fields, TEXT("trace_query"), Query))
					{
						return false;
					}
				}
				return Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportCameraFrameRequestInput>())
			{
				const FViewportCameraFrameRequestInput& Value =
					Payload.Get<FViewportCameraFrameRequestInput>();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PolicyFields;
				FUeAgentCanonicalValuePtr Policy;
				if (!Builder.AddDecimal(PolicyFields, TEXT("padding_ratio"), Value.FramePolicy.PaddingRatio)
					|| !Builder.AddDecimal(PolicyFields, TEXT("minimum_distance_cm"), Value.FramePolicy.MinimumDistanceCm)
					|| !Builder.AddDecimal(PolicyFields, TEXT("maximum_distance_cm"), Value.FramePolicy.MaximumDistanceCm)
					|| !Builder.AddBoolean(PolicyFields, TEXT("preserve_rotation"), Value.FramePolicy.bPreserveRotation)
					|| !Builder.AddBoolean(PolicyFields, TEXT("collision_aware"), Value.FramePolicy.bCollisionAware)
					|| !Builder.AddBoolean(PolicyFields, TEXT("look_at"), Value.FramePolicy.bLookAt)
					|| !Builder.AddBoolean(PolicyFields, TEXT("auto_fallback"), Value.FramePolicy.bAutoFallback)
					|| !Builder.Object(PolicyFields, Policy))
				{
					return false;
				}
				if (!Value.SubjectRefs.IsEmpty())
				{
					FUeAgentCanonicalValuePtr Subjects;
					if (!Builder.StringArray(Value.SubjectRefs, Subjects)
						|| !Builder.Add(Fields, TEXT("subject_refs"), Subjects))
					{
						return false;
					}
				}
				if (Value.FolderPath.IsSet()
					&& !Builder.AddString(Fields, TEXT("folder_path"), Value.FolderPath.GetValue()))
				{
					return false;
				}
				if (Value.FolderPath.IsSet()
					&& !Builder.AddBoolean(Fields, TEXT("include_child_folders"), Value.bIncludeChildFolders))
				{
					return false;
				}
				if (Value.bUseCurrentSelection
					&& !Builder.AddBoolean(Fields, TEXT("use_current_selection"), true))
				{
					return false;
				}
				return Builder.Add(Fields, TEXT("frame_policy"), Policy)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportStateRestoreRequestInput>())
			{
				FUeAgentCanonicalValuePtr Snapshot;
				return TryBuildViewportState(
						Builder,
						Payload.Get<FViewportStateRestoreRequestInput>().Snapshot,
						Snapshot)
					&& Builder.Add(Fields, TEXT("snapshot"), Snapshot)
					&& Builder.Object(Fields, Out);
			}
			if (Payload.IsType<FViewportStateUpdateRequestInput>())
			{
				const FWorldSpatialViewportStatePatch& Value =
					Payload.Get<FViewportStateUpdateRequestInput>().StatePatch;
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> PatchFields;
				if (Value.CameraTransform.IsSet())
				{
					FUeAgentCanonicalValuePtr Camera;
					if (!Builder.SpatialValue(Value.CameraTransform.GetValue(), Camera)
						|| !Builder.Add(PatchFields, TEXT("camera_transform"), Camera))
					{
						return false;
					}
				}
				if (Value.FovDegrees.IsSet()
					&& !Builder.AddDecimal(PatchFields, TEXT("fov_degrees"), Value.FovDegrees.GetValue()))
				{
					return false;
				}
				if (Value.CameraTransformMode.IsSet()
					&& !Builder.AddString(PatchFields, TEXT("camera_transform_mode"), Value.CameraTransformMode.GetValue()))
				{
					return false;
				}
				if (Value.ViewMode.IsSet()
					&& !Builder.AddString(PatchFields, TEXT("view_mode"), Value.ViewMode.GetValue()))
				{
					return false;
				}
				if (Value.Realtime.IsSet()
					&& !Builder.AddBoolean(PatchFields, TEXT("realtime"), Value.Realtime.GetValue()))
				{
					return false;
				}
				if (Value.GameView.IsSet()
					&& !Builder.AddBoolean(PatchFields, TEXT("game_view"), Value.GameView.GetValue()))
				{
					return false;
				}
				if (Value.ShowFlags.IsSet())
				{
					FUeAgentCanonicalValuePtr ShowFlags;
					if (!Builder.StringArray(Value.ShowFlags.GetValue(), ShowFlags)
						|| !Builder.Add(PatchFields, TEXT("show_flags"), ShowFlags))
					{
						return false;
					}
				}
				if (Value.Interaction.IsSet())
				{
					const FWorldSpatialViewportInteractionState& InteractionValue =
						Value.Interaction.GetValue();
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> InteractionFields;
					FUeAgentCanonicalValuePtr Interaction;
					if (!Builder.AddBoolean(InteractionFields, TEXT("mode_widgets"), InteractionValue.bModeWidgets)
						|| !Builder.AddBoolean(InteractionFields, TEXT("selection"), InteractionValue.bSelection)
						|| !Builder.AddBoolean(InteractionFields, TEXT("pivot"), InteractionValue.bPivot)
						|| !Builder.AddBoolean(InteractionFields, TEXT("editor_primitives"), InteractionValue.bEditorPrimitives)
						|| !Builder.AddBoolean(InteractionFields, TEXT("game_primitives"), InteractionValue.bGamePrimitives)
						|| !Builder.AddString(InteractionFields, TEXT("widget_mode"), InteractionValue.WidgetMode)
						|| !Builder.Object(InteractionFields, Interaction)
						|| !Builder.Add(PatchFields, TEXT("interaction"), Interaction))
					{
						return false;
					}
				}
				FUeAgentCanonicalValuePtr Patch;
				return Builder.Object(PatchFields, Patch)
					&& Builder.Add(Fields, TEXT("state_patch"), Patch)
					&& Builder.Object(Fields, Out);
			}
			return false;
		}
	}

	FUeAgentWorldSpatialRequest::FUeAgentWorldSpatialRequest(
		FString InTypeId,
		FString InFamilyId,
		FString InRequestKindId,
		const bool bInOperation,
		FUeAgentWorldSpatialRequestPayload InPayload)
		: TypeId(MoveTemp(InTypeId))
		, FamilyId(MoveTemp(InFamilyId))
		, RequestKindId(MoveTemp(InRequestKindId))
		, bOperation(bInOperation)
		, Payload(MoveTemp(InPayload))
	{
	}

	const FString& FUeAgentWorldSpatialRequest::GetTypeId() const { return TypeId; }
	const FString& FUeAgentWorldSpatialRequest::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentWorldSpatialRequest::GetRequestKindId() const { return RequestKindId; }
	const FUeAgentWorldSpatialRequestPayload& FUeAgentWorldSpatialRequest::GetPayload() const { return Payload; }
	bool FUeAgentWorldSpatialRequest::IsOperation() const { return bOperation; }

	bool FUeAgentWorldSpatialRequest::TryBuildChangeDraft(
		const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
		const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
		TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
		FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutDraft.Reset();
		OutError = {};
		if (!bOperation)
		{
			return Fail(
				OutError,
				TEXT("world_spatial_query_has_no_change_draft"),
				RequestKindId,
				TEXT("query requests do not create C6 changes"));
		}
		if (Snapshot.GetEntries().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_preflight_snapshot_empty"),
				RequestKindId, TEXT("The preflight snapshot contains no state entry to update."));
		}

		FTypedIntentBuilder Builder(
			Context.GetResourceTracker(),
			OutError,
			RequestKindId);
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr TypedPayload;
		if (!TryBuildTypedPayload(Payload, Builder, TypedPayload)
			|| !TypedPayload.IsValid())
		{
			if (!OutError.IsValid())
			{
				Fail(
					OutError,
					TEXT("world_spatial_typed_intent_unsupported"),
					RequestKindId, TEXT("The request payload could not be converted to a typed mutation intent."));
			}
			return false;
		}

		TArray<TPair<
			FString,
			UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>> IntentFields;
		if (!Builder.AddString(IntentFields, TEXT("request_kind"), RequestKindId)
			|| !Builder.AddString(IntentFields, TEXT("family_id"), FamilyId)
			|| !Builder.Add(IntentFields, TEXT("typed_payload"), TypedPayload))
		{
			return false;
		}
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Proposed;
		if (!Builder.Object(IntentFields, Proposed) || !Proposed.IsValid())
		{
			return false;
		}

		const UE::UAI::StateSemantic::FUeAgentStateEntry& Primary =
			Snapshot.GetEntries()[0];
		TArray<UE::UAI::StateSemantic::FUeAgentCanonicalIdentity> ImpactTargets;
		for (const UE::UAI::StateSemantic::FUeAgentStateEntry& Entry
			: Snapshot.GetEntries())
		{
			ImpactTargets.Add(Entry.GetIdentity());
		}
		OutDraft.Emplace(
			Primary.GetIdentity(),
			UE::UAI::PlanningPreflight::EChangeIntent::Update,
			Primary.GetValue(),
			Proposed,
			TArray<FString>{TEXT("world-spatial:write")},
			TEXT("world-spatial-typed-intent:") + RequestKindId,
			MoveTemp(ImpactTargets));
		if (OutDraft->IsValid()) return true;
		OutError = OutDraft->GetValidationDiagnostic();
		if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RequestKindId;
		return false;
	}

	bool TryCreateWorldSpatialRequestDecoder(
		TSharedPtr<IUeAgentWorldSpatialRequestDecoder>& OutDecoder,
		FUeAgentWorldSpatialExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		TSet<FString> Capabilities;
		TSet<FString> Types;
		TSet<FString> Kinds;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId)
				|| Kinds.Contains(Spec.RequestKindId)
				|| !Spec.Decode)
			{
				const bool bCapabilityDuplicate = Capabilities.Contains(Spec.CapabilityId);
				const bool bTypeDuplicate = Types.Contains(Spec.TypeId);
				const bool bKindDuplicate = Kinds.Contains(Spec.RequestKindId);
				const bool bDuplicate = bCapabilityDuplicate || bTypeDuplicate || bKindDuplicate;
				OutError = UE::UAI::Contract::FDiagnostic::Failure(bDuplicate ? TEXT("world_spatial_request_spec_duplicate") : TEXT("world_spatial_request_decoder_missing"),
					bDuplicate ? TEXT("The request specifications contain a duplicate identifier.") : TEXT("The request specification has no payload decoder."), TEXT("native_execution"));
				OutError.SubjectId = Spec.CapabilityId;
				OutError.PropertyPath = bCapabilityDuplicate ? TEXT("capability_id") : bTypeDuplicate ? TEXT("type_id") : bKindDuplicate ? TEXT("request_kind_id") : TEXT("decoder");
				if (bDuplicate) OutError.ActualValue = MakeShared<FJsonValueString>(bCapabilityDuplicate ? Spec.CapabilityId : bTypeDuplicate ? Spec.TypeId : Spec.RequestKindId);
				OutError.ActualValuePresent = bDuplicate;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(bDuplicate ? TEXT("unique request specification identifier") : TEXT("payload decoder"));
				return false;
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
			Kinds.Add(Spec.RequestKindId);
		}
		OutDecoder = MakeShared<FWorldSpatialRequestDecoder>();
		return true;
	}
}
