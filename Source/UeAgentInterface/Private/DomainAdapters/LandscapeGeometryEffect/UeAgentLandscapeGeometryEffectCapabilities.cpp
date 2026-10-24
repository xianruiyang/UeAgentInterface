#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::LandscapeGeometryEffect
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		constexpr const TCHAR* ResultSchema =
			TEXT(R"json({"type":"object","properties":{"operation_id":{"type":"string","minLength":1},"outcome":{"type":"string","enum":["complete","partial","rolled_back","state_unknown"]},"target_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"evidence_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["operation_id","outcome","target_refs"],"additionalProperties":false})json");

		constexpr const TCHAR* CreateInputSchema =
			TEXT(R"json({"type":"object","properties":{"target_ref":{"type":"string","minLength":1},"topology":{"type":"object","properties":{"quads_per_section":{"type":"number","enum":[7,15,31,63,127,255]},"sections_per_component":{"type":"number","enum":[1,2]},"component_count":{"type":"array","items":{"type":"number","minimum":1},"minItems":2,"maxItems":2}},"required":["quads_per_section","sections_per_component","component_count"],"additionalProperties":false},"placement":{"type":"object","properties":{"subject_point":{"const":"sample_origin"},"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotation_quaternion_xyzw":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"scale":{"type":"array","items":{"type":"number","exclusiveMinimum":0},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["subject_point","frame_id","unit","translation","rotation_quaternion_xyzw","scale","basis_evidence"],"additionalProperties":false},"sample_region":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"min_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"max_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","min_inclusive","max_inclusive","basis_evidence"],"additionalProperties":false},"initial_height":{"type":"object","properties":{"encoding":{"const":"landscape_local_height"},"value":{"type":"number"}},"required":["encoding","value"],"additionalProperties":false}},"required":["target_ref","topology","placement","sample_region","initial_height"],"additionalProperties":false})json");

		constexpr const TCHAR* HeightDeltaInputSchema =
			TEXT(R"json({"type":"object","properties":{"target_ref":{"type":"string","minLength":1},"space_contract":{"type":"object","properties":{"semantic_frame_id":{"type":"string","minLength":1},"write_frame_id":{"const":"ue.landscape.local_sample_grid"},"read_frame_id":{"const":"ue.landscape.local_sample_grid"},"verify_frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"expected_scale":{"type":"array","items":{"type":"number","exclusiveMinimum":0},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1},"conversion_chain":{"type":"string","minLength":1}},"required":["semantic_frame_id","write_frame_id","read_frame_id","verify_frame_id","unit","expected_scale","basis_evidence","conversion_chain"],"additionalProperties":false},"region":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"min_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"max_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2}},"required":["frame_id","min_inclusive","max_inclusive"],"additionalProperties":false},"delta":{"type":"object","properties":{"encoding":{"const":"landscape_local_height_delta"},"row_stride":{"type":"number","minimum":1},"values":{"type":"array","items":{"type":"number"},"minItems":1}},"required":["encoding","row_stride","values"],"additionalProperties":false}},"required":["target_ref","space_contract","region","delta"],"additionalProperties":false})json");

		struct FDescriptorSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* NativeTargetKind;
			const TCHAR* InputSchema;
		};

		const FDescriptorSpec DescriptorSpecs[] = {
			{
				TEXT("uai.operation.landscape.create"),
				TEXT("landscape.create"),
				TEXT("landscape.geometry.create"),
				CreateInputSchema,
			},
			{
				TEXT("uai.operation.landscape.height_delta"),
				TEXT("landscape.height_delta"),
				TEXT("landscape.geometry.height_delta"),
				HeightDeltaInputSchema,
			},
		};

		bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		struct FValueContext
		{
			FString Code;
			FString Prefix;
			FString Target;
			FString SourceKind = TEXT("command_input");
			FString Path(const FString& Field) const { return Prefix + TEXT("/") + Field; }
		};

		FString JsonKindName(EJson Kind)
		{
			switch (Kind)
			{
			case EJson::Null: return TEXT("null");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}

		FExecutionMutationError ValueFailure(const FValueContext& Context, FString Reason, FString Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FExecutionMutationError::Failure(Context.Code, MoveTemp(Reason), TEXT("execution"));
			Error.SourceKind = Context.SourceKind;
			Error.TargetPath = Context.Target;
			Error.PropertyPath = MoveTemp(Path);
			if (Context.SourceKind == TEXT("command_input")) Error.Source.JsonPath = Error.PropertyPath;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		TSharedPtr<FJsonValue> JsonField(const TSharedPtr<FJsonObject>& Object, const FString& Field)
		{
			const auto* Value = Object.IsValid() ? Object->Values.Find(Field) : nullptr;
			return Value ? *Value : nullptr;
		}

		TSharedPtr<FJsonValue> ScalarFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			if (Value->Type == EJson::Object || Value->Type == EJson::Array)
				return MakeShared<FJsonValueString>(JsonKindName(Value->Type));
			if (Value->Type == EJson::Number && !FMath::IsFinite(Value->AsNumber()))
			{
				FString Lexeme;
				if (!CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)) Lexeme = FString::Printf(TEXT("%.17g"), Value->AsNumber());
				return MakeShared<FJsonValueString>(Lexeme);
			}
			return Value;
		}

		FExecutionMutationError FieldFailure(const FValueContext& Context, const TSharedPtr<FJsonObject>& Object,
			const FString& Field, const FString& Expected)
		{
			const auto Value = JsonField(Object, Field);
			return ValueFailure(Context,
				!Object.IsValid() ? TEXT("The required Landscape object is missing.")
					: !Value.IsValid() ? TEXT("A required Landscape field is missing.")
						: TEXT("The Landscape field has an incompatible value type."),
				!Object.IsValid() ? Context.Prefix : Context.Path(Field),
				Value.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(JsonKindName(Value->Type))) : nullptr,
				MakeShared<FJsonValueString>(!Object.IsValid() ? FString(TEXT("object")) : Expected));
		}

		bool ReadStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& OutValue,
			FExecutionMutationError& OutError, const FValueContext& Context)
		{
			OutValue.Reset();
			if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue))
				return Fail(OutError, FieldFailure(Context, Object, Field, TEXT("non-empty string")));
			return !OutValue.IsEmpty() || Fail(OutError, ValueFailure(Context, TEXT("The Landscape string field must not be empty."),
				Context.Path(Field), MakeShared<FJsonValueString>(OutValue), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
		}

		bool ReadObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TSharedPtr<FJsonObject>*& OutValue,
			FExecutionMutationError& OutError, const FValueContext& Context)
		{
			OutValue = nullptr;
			return (Object.IsValid() && Object->TryGetObjectField(Field, OutValue) && OutValue)
				|| Fail(OutError, FieldFailure(Context, Object, Field, TEXT("object")));
		}

		bool ReadArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TArray<TSharedPtr<FJsonValue>>*& OutValue,
			FExecutionMutationError& OutError, const FValueContext& Context)
		{
			OutValue = nullptr;
			return (Object.IsValid() && Object->TryGetArrayField(Field, OutValue) && OutValue)
				|| Fail(OutError, FieldFailure(Context, Object, Field, TEXT("array")));
		}

		bool ReadNumberValue(const TSharedPtr<FJsonValue>& Value, double& OutNumber,
			FExecutionMutationError& OutError, const FValueContext& Context, const FString& Path)
		{
			OutNumber = 0.0;
			if (!Value.IsValid() || !Value->TryGetNumber(OutNumber))
				return Fail(OutError, ValueFailure(Context, TEXT("The Landscape value must be numeric."), Path,
					ScalarFact(Value), MakeShared<FJsonValueString>(TEXT("number"))));
			return FMath::IsFinite(OutNumber) || Fail(OutError, ValueFailure(Context, TEXT("The Landscape number must be finite."), Path,
				ScalarFact(Value), MakeShared<FJsonValueString>(TEXT("finite number"))));
		}

		bool ReadNumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& OutNumber,
			FExecutionMutationError& OutError, const FValueContext& Context)
		{
			return ReadNumberValue(JsonField(Object, Field), OutNumber, OutError, Context, Context.Path(Field));
		}

		bool TryCanonicalizeSchema(const TCHAR* Source, FString& OutCanonical, UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Object;
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Json::TryParseObject(FString(Source), Object, ParseError))
			{
				ParseError.SourceKind = TEXT("native_result");
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("landscape_geometry_schema_parse_failed"), ParseError, TEXT("registration"));
				return false;
			}
			if (!Json::TryCanonicalize(Object, OutCanonical, OutError))
			{
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		bool ParsePayload(const FUeAgentOperationRequest& Request, TSharedPtr<FJsonObject>& OutPayload,
			FExecutionMutationError& OutError)
		{
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Json::TryParseObject(Request.GetPayload().GetJsonValue(), OutPayload, ParseError))
			{
				if (ParseError.SourceKind.IsEmpty()) ParseError.SourceKind = TEXT("command_input");
				return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_payload_parse_failed"), ParseError, TEXT("execution")));
			}
			return true;
		}

		TSharedPtr<FJsonValue> NumberArray(
			const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Values.Num());
			for (const double Value : Values)
			{
				Items.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		bool ReadNumberArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			const int32 ExpectedCount, TArray<double>& OutValues, FExecutionMutationError& OutError, const FValueContext& Context)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!ReadArrayField(Object, Field, Values, OutError, Context)) return false;
			if (Values->Num() != ExpectedCount)
				return Fail(OutError, ValueFailure(Context, TEXT("The Landscape array has an unexpected number of elements."),
					Context.Path(Field) + TEXT("/count"), MakeShared<FJsonValueNumber>(Values->Num()), MakeShared<FJsonValueNumber>(ExpectedCount)));
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				double Number = 0.0;
				if (!ReadNumberValue((*Values)[Index], Number, OutError, Context, Context.Path(Field) + TEXT("/") + FString::FromInt(Index))) return false;
				OutValues.Add(Number);
			}
			return true;
		}

		bool ToInteger(const double Value, int32& OutValue, FExecutionMutationError& OutError,
			const FValueContext& Context, const FString& Path)
		{
			if (!FMath::IsFinite(Value) || Value < static_cast<double>(MIN_int32) || Value > static_cast<double>(MAX_int32))
				return Fail(OutError, ValueFailure(Context, TEXT("The Landscape value must be a finite integer in the signed 32-bit range."), Path,
					FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(::LexToString(Value))),
					MakeShared<FJsonValueString>(TEXT("integer in [-2147483648, 2147483647]"))));
			const int32 Rounded = FMath::RoundToInt(Value);
			if (!FMath::IsNearlyEqual(Value, static_cast<double>(Rounded), UE_DOUBLE_SMALL_NUMBER))
				return Fail(OutError, ValueFailure(Context, TEXT("The Landscape value must be an integer."), Path,
					MakeShared<FJsonValueNumber>(Value), MakeShared<FJsonValueString>(TEXT("integer"))));
			OutValue = Rounded;
			return true;
		}

		bool ReadIntegerField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& OutValue,
			FExecutionMutationError& OutError, const FValueContext& Context)
		{
			double Number = 0.0;
			return ReadNumberField(Object, Field, Number, OutError, Context) && ToInteger(Number, OutValue, OutError, Context, Context.Path(Field));
		}

		bool ReadIntegerArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const int32 ExpectedCount,
			TArray<int32>& OutValues, FExecutionMutationError& OutError, const FValueContext& Context)
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(Object, Field, ExpectedCount, Numbers, OutError, Context)) return false;
			OutValues.Reset();
			for (int32 Index = 0; Index < Numbers.Num(); ++Index)
			{
				int32 Integer = 0;
				if (!ToInteger(Numbers[Index], Integer, OutError, Context, Context.Path(Field) + TEXT("/") + FString::FromInt(Index))) return false;
				OutValues.Add(Integer);
			}
			return true;
		}

		FUeAgentCanonicalValuePtr FindCanonicalField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<
				FString,
				FUeAgentCanonicalValueRef>& Item
					: Object->GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return FUeAgentCanonicalValuePtr(Item.Value);
				}
			}
			return nullptr;
		}

		FString CanonicalKindName(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}

		bool ReadCanonicalNumberArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const int32 ExpectedCount,
			TArray<double>& OutValues, FExecutionMutationError& OutError, const FValueContext& Context)
		{
			OutValues.Reset();
			if (!Object.IsValid() || Object->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, ValueFailure(Context, TEXT("The captured Landscape state must provide an object."), Context.Prefix,
					Object.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CanonicalKindName(Object->GetKind()))) : nullptr,
					MakeShared<FJsonValueString>(TEXT("object"))));
			const auto Value = FindCanonicalField(Object, Field);
			if (!Value.IsValid() || Value->GetKind() != EUeAgentValueKind::Array)
				return Fail(OutError, ValueFailure(Context, Value.IsValid() ? TEXT("The captured Landscape state field must be an array.")
					: TEXT("A required captured Landscape state field is missing."), Context.Path(Field),
					Value.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CanonicalKindName(Value->GetKind()))) : nullptr,
					MakeShared<FJsonValueString>(TEXT("array"))));
			if (Value->GetArrayItems().Num() != ExpectedCount)
				return Fail(OutError, ValueFailure(Context, TEXT("The captured Landscape state array has an unexpected number of elements."),
					Context.Path(Field) + TEXT("/count"), MakeShared<FJsonValueNumber>(Value->GetArrayItems().Num()), MakeShared<FJsonValueNumber>(ExpectedCount)));
			for (int32 Index = 0; Index < Value->GetArrayItems().Num(); ++Index)
			{
				const auto& Item = Value->GetArrayItems()[Index];
				const FString Path = Context.Path(Field) + TEXT("/") + FString::FromInt(Index);
				if (Item->GetKind() != EUeAgentValueKind::Decimal && Item->GetKind() != EUeAgentValueKind::Integer)
					return Fail(OutError, ValueFailure(Context, TEXT("The captured Landscape state component must be numeric."), Path,
						MakeShared<FJsonValueString>(CanonicalKindName(Item->GetKind())), MakeShared<FJsonValueString>(TEXT("decimal or integer"))));
				const double Number = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
					return Fail(OutError, ValueFailure(Context, TEXT("The captured Landscape state component must be finite."), Path,
						MakeShared<FJsonValueString>(Item->GetLexeme()), MakeShared<FJsonValueString>(TEXT("finite number"))));
				OutValues.Add(Number);
			}
			return true;
		}

		bool ReadCanonicalIntegerArray(const FUeAgentCanonicalValuePtr& Object, const FString& Field, const int32 ExpectedCount,
			TArray<int32>& OutValues, FExecutionMutationError& OutError, const FValueContext& Context)
		{
			TArray<double> Numbers;
			if (!ReadCanonicalNumberArray(Object, Field, ExpectedCount, Numbers, OutError, Context)) return false;
			OutValues.Reset();
			for (int32 Index = 0; Index < Numbers.Num(); ++Index)
			{
				int32 Integer = 0;
				if (!ToInteger(Numbers[Index], Integer, OutError, Context, Context.Path(Field) + TEXT("/") + FString::FromInt(Index))) return false;
				OutValues.Add(Integer);
			}
			return true;
		}

		bool TryFindCurrentEntry(const FUeAgentCanonicalSnapshotRef& Snapshot, const FString& NativeTargetKind,
			const FString& Selector, const FString& WorldIdentity, const FUeAgentStateEntry*& OutMatch, FExecutionMutationError& OutError)
		{
			OutMatch = nullptr;
			int32 MatchCount = 0;
			for (const auto& Entry : Snapshot->GetEntries())
			{
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetTargetKind() != NativeTargetKind || Identity.GetSelector() != Selector || Identity.GetOwnerId() != WorldIdentity) continue;
				++MatchCount;
				if (MatchCount == 1) OutMatch = &Entry;
			}
			if (MatchCount == 1) return true;
			OutMatch = nullptr;
			const FValueContext Context{MatchCount == 0 ? TEXT("landscape_height_delta_current_state_missing")
				: TEXT("landscape_height_delta_current_state_ambiguous"), TEXT("/current_state"), Selector, TEXT("native_result")};
			OutError = ValueFailure(Context, MatchCount == 0 ? TEXT("No captured Landscape state entry matches this target in the selected world.")
				: TEXT("More than one captured Landscape state entry matches this target in the selected world."),
				Context.Path(TEXT("matching_entries")), MakeShared<FJsonValueNumber>(MatchCount), MakeShared<FJsonValueNumber>(1));
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(WorldIdentity);
			return false;
		}

		bool AddDraftChange(
			const FDescriptorSpec& Spec,
			const FString& TargetRef,
			const EChangeIntent Intent,
			const TSharedPtr<FJsonObject>& InternalAfter,
			const FUeAgentCanonicalValuePtr& ExpectedBefore,
			const FUeAgentCanonicalIdentity* ExistingIdentity,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr After;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(InternalAfter),
					Resources,
					After,
					OutError))
			{
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			}
			const FUeAgentCanonicalIdentity Identity =
				ExistingIdentity
					? *ExistingIdentity
					: FUeAgentCanonicalIdentity(
						TEXT("uai.landscape.geometry:")
							+ TargetRef,
						Spec.NativeTargetKind,
						Context.GetWorldIdentity(),
						TargetRef,
						TEXT("landscape_geometry"),
						Context.GetContextRevision());
			OutChanges.Emplace(
				Identity,
				Intent,
				ExpectedBefore,
				After,
				GetLandscapeGeometryEffectAuthorityScopes(),
				TEXT(
					"landscape-geometry:canonical-typed-draft"),
				TArray<FUeAgentCanonicalIdentity>{});
			if (OutChanges.Last().IsValid()) return true;
			auto Cause = OutChanges.Last().GetValidationDiagnostic();
			if (Cause.SourceKind.IsEmpty()) Cause.SourceKind = TEXT("native_result");
			OutError = FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_change_draft_invalid"), Cause);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
			return false;
		}

		bool BuildCreate(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* Topology = nullptr;
			const TSharedPtr<FJsonObject>* Placement = nullptr;
			const TSharedPtr<FJsonObject>* Region = nullptr;
			const TSharedPtr<FJsonObject>* InitialHeight = nullptr;
			FString TargetRef;
			FValueContext PayloadContext{TEXT("landscape_create_payload_invalid"), TEXT(""), TEXT("")};
			if (!ReadStringField(Payload, TEXT("target_ref"), TargetRef, OutError, PayloadContext)) return false;
			PayloadContext.Target = TargetRef;
			if (!ReadObjectField(Payload, TEXT("topology"), Topology, OutError, PayloadContext)
				|| !ReadObjectField(Payload, TEXT("placement"), Placement, OutError, PayloadContext)
				|| !ReadObjectField(Payload, TEXT("sample_region"), Region, OutError, PayloadContext)
				|| !ReadObjectField(Payload, TEXT("initial_height"), InitialHeight, OutError, PayloadContext))
				return false;
			const FValueContext TopologyContext{TEXT("landscape_create_topology_invalid"), TEXT("/topology"), TargetRef};
			const FValueContext RegionTopologyContext{TEXT("landscape_create_topology_invalid"), TEXT("/sample_region"), TargetRef};
			int32 QuadsPerSection = 0, SectionsPerComponent = 0;
			TArray<int32> ComponentCount, MinInclusive, MaxInclusive;
			if (!ReadIntegerField(*Topology, TEXT("quads_per_section"), QuadsPerSection, OutError, TopologyContext)
				|| !ReadIntegerField(*Topology, TEXT("sections_per_component"), SectionsPerComponent, OutError, TopologyContext)
				|| !ReadIntegerArray(*Topology, TEXT("component_count"), 2, ComponentCount, OutError, TopologyContext)
				|| !ReadIntegerArray(*Region, TEXT("min_inclusive"), 2, MinInclusive, OutError, RegionTopologyContext)
				|| !ReadIntegerArray(*Region, TEXT("max_inclusive"), 2, MaxInclusive, OutError, RegionTopologyContext))
				return false;
			if (QuadsPerSection <= 0)
				return Fail(OutError, ValueFailure(TopologyContext, TEXT("Landscape quads per section must be positive."),
					TopologyContext.Path(TEXT("quads_per_section")), MakeShared<FJsonValueNumber>(QuadsPerSection), MakeShared<FJsonValueString>(TEXT("> 0"))));
			if (SectionsPerComponent != 1 && SectionsPerComponent != 2)
				return Fail(OutError, ValueFailure(TopologyContext, TEXT("Landscape sections per component must be 1 or 2."),
					TopologyContext.Path(TEXT("sections_per_component")), MakeShared<FJsonValueNumber>(SectionsPerComponent), MakeShared<FJsonValueString>(TEXT("1 or 2"))));
			for (int32 Index = 0; Index < 2; ++Index)
				if (ComponentCount[Index] <= 0)
					return Fail(OutError, ValueFailure(TopologyContext, TEXT("Landscape component counts must be positive."),
						TopologyContext.Path(TEXT("component_count")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(ComponentCount[Index]), MakeShared<FJsonValueString>(TEXT("> 0"))));
			const int64 QuadsX = static_cast<int64>(QuadsPerSection) * SectionsPerComponent * ComponentCount[0];
			const int64 QuadsY = static_cast<int64>(QuadsPerSection) * SectionsPerComponent * ComponentCount[1];
			const int64 Quads[] = {QuadsX, QuadsY};
			const FValueContext RegionContext{TEXT("landscape_create_region_topology_mismatch"), TEXT("/sample_region"), TargetRef};
			for (int32 Index = 0; Index < 2; ++Index)
				if (MaxInclusive[Index] < MinInclusive[Index])
					return Fail(OutError, ValueFailure(RegionContext, TEXT("The sample region maximum must not be below its inclusive minimum."),
						RegionContext.Path(TEXT("max_inclusive")) + TEXT("/") + FString::FromInt(Index), MakeShared<FJsonValueNumber>(MaxInclusive[Index]),
						MakeShared<FJsonValueString>(TEXT(">= ") + ::LexToString(MinInclusive[Index]))));
			for (int32 Index = 0; Index < 2; ++Index)
				if (static_cast<int64>(MaxInclusive[Index]) - MinInclusive[Index] != Quads[Index])
					return Fail(OutError, ValueFailure(RegionContext, TEXT("The sample region size does not match the requested Landscape topology."),
						RegionContext.Path(TEXT("max_inclusive")) + TEXT("/") + FString::FromInt(Index), MakeShared<FJsonValueNumber>(MaxInclusive[Index]),
						MakeShared<FJsonValueString>(::LexToString(static_cast<int64>(MinInclusive[Index]) + Quads[Index]))));
			FString FrameId, Unit, BasisEvidence, SubjectPoint;
			(*Placement)->TryGetStringField(TEXT("unit"), Unit);
			const FValueContext PlacementContext{TEXT("landscape_create_space_contract_invalid"), TEXT("/placement"), TargetRef};
			if (!ReadStringField(*Placement, TEXT("frame_id"), FrameId, OutError, PlacementContext)) return false;
			if (FrameId != TEXT("ue.world"))
				return Fail(OutError, ValueFailure(PlacementContext, TEXT("Landscape placement must use the UE world frame."),
					PlacementContext.Path(TEXT("frame_id")), MakeShared<FJsonValueString>(FrameId), MakeShared<FJsonValueString>(TEXT("ue.world"))));
			if (!ReadStringField(*Placement, TEXT("subject_point"), SubjectPoint, OutError, PlacementContext)) return false;
			if (SubjectPoint != TEXT("sample_origin"))
				return Fail(OutError, ValueFailure(PlacementContext, TEXT("Landscape placement must identify the sample origin."),
					PlacementContext.Path(TEXT("subject_point")), MakeShared<FJsonValueString>(SubjectPoint), MakeShared<FJsonValueString>(TEXT("sample_origin"))));
			if (!ReadStringField(*Placement, TEXT("basis_evidence"), BasisEvidence, OutError, PlacementContext)) return false;
			TArray<double> Translation, Rotation, Scale;
			if (!ReadNumberArray(*Placement, TEXT("translation"), 3, Translation, OutError, PlacementContext)
				|| !ReadNumberArray(*Placement, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError, PlacementContext)
				|| !ReadNumberArray(*Placement, TEXT("scale"), 3, Scale, OutError, PlacementContext))
				return false;
			for (int32 Index = 0; Index < Scale.Num(); ++Index)
				if (Scale[Index] <= 0.0)
					return Fail(OutError, ValueFailure(PlacementContext, TEXT("Landscape scale components must be positive."),
						PlacementContext.Path(TEXT("scale")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(Scale[Index]), MakeShared<FJsonValueString>(TEXT("> 0"))));
			const FQuat Quat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]);
			if (!Quat.IsNormalized())
				return Fail(OutError, ValueFailure({TEXT("landscape_create_rotation_invalid"), TEXT("/placement"), TargetRef},
					TEXT("The Landscape rotation quaternion must be normalized."), PlacementContext.Path(TEXT("rotation_quaternion_xyzw")),
					NumberArray(Rotation), MakeShared<FJsonValueString>(TEXT("normalized quaternion"))));
			const double UnitScale = Unit == TEXT("meter") ? 100.0 : Unit == TEXT("centimeter") ? 1.0 : 0.0;
			if (UnitScale == 0.0)
				return Fail(OutError, ValueFailure({TEXT("landscape_create_unit_invalid"), TEXT("/placement"), TargetRef},
					TEXT("Landscape placement translation must use centimeters or meters."), PlacementContext.Path(TEXT("unit")),
					ScalarFact(JsonField(*Placement, TEXT("unit"))), MakeShared<FJsonValueString>(TEXT("centimeter or meter"))));
			for (int32 Index = 0; Index < Translation.Num(); ++Index)
			{
				const double Original = Translation[Index];
				Translation[Index] *= UnitScale;
				if (!FMath::IsFinite(Translation[Index]))
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("input_value"), Original);
					Actual->SetStringField(TEXT("unit"), Unit);
					Actual->SetStringField(TEXT("converted_centimeters"), ::LexToString(Translation[Index]));
					return Fail(OutError, ValueFailure(PlacementContext, TEXT("Converting this placement translation to centimeters exceeds the finite native number range."),
						PlacementContext.Path(TEXT("translation")) + TEXT("/") + FString::FromInt(Index), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("finite centimeters"))));
				}
			}
			double InitialLocalHeight = 0.0;
			const FValueContext HeightContext{TEXT("landscape_create_initial_height_invalid"), TEXT("/initial_height"), TargetRef};
			if (!ReadNumberField(*InitialHeight, TEXT("value"), InitialLocalHeight, OutError, HeightContext)) return false;
			if (InitialLocalHeight < -256.0 || InitialLocalHeight > 255.9921875)
				return Fail(OutError, ValueFailure(HeightContext, TEXT("The initial Landscape height is outside the native representable range."),
					HeightContext.Path(TEXT("value")), MakeShared<FJsonValueNumber>(InitialLocalHeight), NumberArray({-256.0, 255.9921875})));

			const TSharedPtr<FJsonObject> After =
				MakeShared<FJsonObject>();
			After->SetStringField(TEXT("target_ref"), TargetRef);
			After->SetStringField(
				TEXT("write_frame_id"),
				TEXT(
					"ue.landscape.actor_and_local_sample_grid"));
			After->SetStringField(
				TEXT("conversion_evidence"),
				BasisEvidence);
			After->SetNumberField(
				TEXT("quads_per_section"),
				QuadsPerSection);
			After->SetNumberField(
				TEXT("sections_per_component"),
				SectionsPerComponent);
			After->SetField(
				TEXT("component_count"),
				NumberArray({
					static_cast<double>(ComponentCount[0]),
					static_cast<double>(ComponentCount[1])}));
			After->SetField(
				TEXT("sample_min_inclusive"),
				NumberArray({
					static_cast<double>(MinInclusive[0]),
					static_cast<double>(MinInclusive[1])}));
			After->SetField(
				TEXT("sample_max_inclusive"),
				NumberArray({
					static_cast<double>(MaxInclusive[0]),
					static_cast<double>(MaxInclusive[1])}));
			After->SetField(
				TEXT("translation_cm"),
				NumberArray(Translation));
			After->SetField(
				TEXT("rotation_quaternion_xyzw"),
				NumberArray(Rotation));
			After->SetField(TEXT("scale"), NumberArray(Scale));
			After->SetNumberField(
				TEXT("initial_local_height"),
				InitialLocalHeight);
			return AddDraftChange(
				Spec,
				TargetRef,
				EChangeIntent::Create,
				After,
				nullptr,
				nullptr,
				Context,
				Resources,
				OutChanges,
				OutError);
		}

		bool BuildHeightDelta(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
			const TSharedPtr<FJsonObject>* Region = nullptr;
			const TSharedPtr<FJsonObject>* Delta = nullptr;
			FString TargetRef;
			FValueContext PayloadContext{TEXT("landscape_height_delta_payload_invalid"), TEXT(""), TEXT("")};
			if (!ReadStringField(Payload, TEXT("target_ref"), TargetRef, OutError, PayloadContext)) return false;
			PayloadContext.Target = TargetRef;
			if (!ReadObjectField(Payload, TEXT("space_contract"), SpaceContract, OutError, PayloadContext)
				|| !ReadObjectField(Payload, TEXT("region"), Region, OutError, PayloadContext)
				|| !ReadObjectField(Payload, TEXT("delta"), Delta, OutError, PayloadContext))
				return false;
			const FUeAgentStateEntry* Current = nullptr;
			if (!TryFindCurrentEntry(CurrentSnapshot, Spec.NativeTargetKind, TargetRef, Context.GetWorldIdentity(), Current, OutError)) return false;
			const FUeAgentCanonicalValuePtr CurrentValue(Current->GetValue());
			TArray<double> CurrentScale;
			const auto Extent = FindCanonicalField(CurrentValue, TEXT("extent"));
			TArray<int32> ExtentMin, ExtentMax;
			const FValueContext CurrentContext{TEXT("landscape_height_delta_current_state_invalid"), TEXT("/current_state"), TargetRef, TEXT("native_result")};
			const FValueContext ExtentContext{CurrentContext.Code, TEXT("/current_state/extent"), TargetRef, TEXT("native_result")};
			if (!ReadCanonicalNumberArray(CurrentValue, TEXT("scale"), 3, CurrentScale, OutError, CurrentContext)
				|| !ReadCanonicalIntegerArray(Extent, TEXT("min_inclusive"), 2, ExtentMin, OutError, ExtentContext)
				|| !ReadCanonicalIntegerArray(Extent, TEXT("max_inclusive"), 2, ExtentMax, OutError, ExtentContext))
				return false;
			const FValueContext SpaceContext{TEXT("landscape_height_delta_space_contract_invalid"), TEXT("/space_contract"), TargetRef};
			TArray<double> ExpectedScale;
			if (!ReadNumberArray(*SpaceContract, TEXT("expected_scale"), 3, ExpectedScale, OutError, SpaceContext)) return false;
			for (int32 Index = 0; Index < 3; ++Index)
				if (!FMath::IsNearlyEqual(CurrentScale[Index], ExpectedScale[Index], UE_DOUBLE_KINDA_SMALL_NUMBER))
					return Fail(OutError, ValueFailure({TEXT("landscape_height_delta_scale_stale"), TEXT("/space_contract"), TargetRef},
						TEXT("The captured Landscape scale differs from the expected scale beyond the native tolerance."),
						SpaceContext.Path(TEXT("expected_scale")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(CurrentScale[Index]), MakeShared<FJsonValueNumber>(ExpectedScale[Index])));
			FString SemanticFrame, VerifyFrame, WriteFrame, ReadFrame, Unit, BasisEvidence, ConversionChain;
			if (!ReadStringField(*SpaceContract, TEXT("semantic_frame_id"), SemanticFrame, OutError, SpaceContext)
				|| !ReadStringField(*SpaceContract, TEXT("verify_frame_id"), VerifyFrame, OutError, SpaceContext))
				return false;
			if (VerifyFrame != SemanticFrame)
				return Fail(OutError, ValueFailure(SpaceContext, TEXT("The verification frame must match the declared semantic frame."),
					SpaceContext.Path(TEXT("verify_frame_id")), MakeShared<FJsonValueString>(VerifyFrame), MakeShared<FJsonValueString>(SemanticFrame)));
			if (!ReadStringField(*SpaceContract, TEXT("write_frame_id"), WriteFrame, OutError, SpaceContext)) return false;
			if (WriteFrame != TEXT("ue.landscape.local_sample_grid"))
				return Fail(OutError, ValueFailure(SpaceContext, TEXT("Landscape height writes must use the local sample grid."),
					SpaceContext.Path(TEXT("write_frame_id")), MakeShared<FJsonValueString>(WriteFrame), MakeShared<FJsonValueString>(TEXT("ue.landscape.local_sample_grid"))));
			if (!ReadStringField(*SpaceContract, TEXT("read_frame_id"), ReadFrame, OutError, SpaceContext)) return false;
			if (ReadFrame != WriteFrame)
				return Fail(OutError, ValueFailure(SpaceContext, TEXT("The height read frame must match its write frame."),
					SpaceContext.Path(TEXT("read_frame_id")), MakeShared<FJsonValueString>(ReadFrame), MakeShared<FJsonValueString>(WriteFrame)));
			if (!ReadStringField(*SpaceContract, TEXT("unit"), Unit, OutError, SpaceContext)) return false;
			if (Unit != TEXT("centimeter") && Unit != TEXT("meter"))
				return Fail(OutError, ValueFailure(SpaceContext, TEXT("The height space contract must declare centimeters or meters."),
					SpaceContext.Path(TEXT("unit")), MakeShared<FJsonValueString>(Unit), MakeShared<FJsonValueString>(TEXT("centimeter or meter"))));
			if (!ReadStringField(*SpaceContract, TEXT("basis_evidence"), BasisEvidence, OutError, SpaceContext)
				|| !ReadStringField(*SpaceContract, TEXT("conversion_chain"), ConversionChain, OutError, SpaceContext))
				return false;
			const FValueContext RegionContext{TEXT("landscape_height_delta_region_out_of_bounds"), TEXT("/region"), TargetRef};
			TArray<int32> RegionMin, RegionMax;
			if (!ReadIntegerArray(*Region, TEXT("min_inclusive"), 2, RegionMin, OutError, RegionContext)
				|| !ReadIntegerArray(*Region, TEXT("max_inclusive"), 2, RegionMax, OutError, RegionContext))
				return false;
			for (int32 Index = 0; Index < 2; ++Index)
				if (RegionMin[Index] > RegionMax[Index])
					return Fail(OutError, ValueFailure(RegionContext, TEXT("The height region minimum must not exceed its inclusive maximum."),
						RegionContext.Path(TEXT("min_inclusive")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(RegionMin[Index]), MakeShared<FJsonValueString>(TEXT("<= ") + ::LexToString(RegionMax[Index]))));
			for (int32 Index = 0; Index < 2; ++Index)
				if (RegionMin[Index] < ExtentMin[Index])
					return Fail(OutError, ValueFailure(RegionContext, TEXT("The height region minimum is below the captured Landscape extent."),
						RegionContext.Path(TEXT("min_inclusive")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(RegionMin[Index]), MakeShared<FJsonValueString>(TEXT(">= ") + ::LexToString(ExtentMin[Index]))));
			for (int32 Index = 0; Index < 2; ++Index)
				if (RegionMax[Index] > ExtentMax[Index])
					return Fail(OutError, ValueFailure(RegionContext, TEXT("The height region maximum exceeds the captured Landscape extent."),
						RegionContext.Path(TEXT("max_inclusive")) + TEXT("/") + FString::FromInt(Index),
						MakeShared<FJsonValueNumber>(RegionMax[Index]), MakeShared<FJsonValueString>(TEXT("<= ") + ::LexToString(ExtentMax[Index]))));
			const int64 Width = static_cast<int64>(RegionMax[0]) - RegionMin[0] + 1;
			const int64 Height = static_cast<int64>(RegionMax[1]) - RegionMin[1] + 1;
			int32 RowStride = 0;
			const TArray<TSharedPtr<FJsonValue>>* DeltaValues = nullptr;
			const FValueContext CountContext{TEXT("landscape_height_delta_sample_count_mismatch"), TEXT("/delta"), TargetRef};
			if (!ReadIntegerField(*Delta, TEXT("row_stride"), RowStride, OutError, CountContext)
				|| !ReadArrayField(*Delta, TEXT("values"), DeltaValues, OutError, CountContext))
				return false;
			if (RowStride != Width)
				return Fail(OutError, ValueFailure(CountContext, TEXT("The height delta row stride must equal the region width."),
					CountContext.Path(TEXT("row_stride")), MakeShared<FJsonValueNumber>(RowStride), MakeShared<FJsonValueString>(::LexToString(Width))));
			const int64 RequiredSamples = Width * Height;
			if (static_cast<int64>(DeltaValues->Num()) != RequiredSamples)
				return Fail(OutError, ValueFailure(CountContext, TEXT("The height delta array must contain one value for every region sample."),
					CountContext.Path(TEXT("values")) + TEXT("/count"), MakeShared<FJsonValueNumber>(DeltaValues->Num()),
					MakeShared<FJsonValueString>(::LexToString(RequiredSamples))));
			TArray<double> Values;
			Values.Reserve(DeltaValues->Num());
			const FValueContext ValueContext{TEXT("landscape_height_delta_value_invalid"), TEXT("/delta"), TargetRef};
			for (int32 Index = 0; Index < DeltaValues->Num(); ++Index)
			{
				double Number = 0.0;
				if (!ReadNumberValue((*DeltaValues)[Index], Number, OutError, ValueContext,
					ValueContext.Path(TEXT("values")) + TEXT("/") + FString::FromInt(Index))) return false;
				Values.Add(Number);
			}

			const TSharedPtr<FJsonObject> After =
				MakeShared<FJsonObject>();
			After->SetStringField(TEXT("target_ref"), TargetRef);
			After->SetStringField(
				TEXT("write_frame_id"),
				WriteFrame);
			After->SetStringField(
				TEXT("conversion_evidence"),
				BasisEvidence + TEXT(" | ") + ConversionChain);
			After->SetField(
				TEXT("expected_scale"),
				NumberArray(ExpectedScale));
			After->SetField(
				TEXT("frozen_extent_min_inclusive"),
				NumberArray({
					static_cast<double>(ExtentMin[0]),
					static_cast<double>(ExtentMin[1])}));
			After->SetField(
				TEXT("frozen_extent_max_inclusive"),
				NumberArray({
					static_cast<double>(ExtentMax[0]),
					static_cast<double>(ExtentMax[1])}));
			After->SetField(
				TEXT("region_min_inclusive"),
				NumberArray({
					static_cast<double>(RegionMin[0]),
					static_cast<double>(RegionMin[1])}));
			After->SetField(
				TEXT("region_max_inclusive"),
				NumberArray({
					static_cast<double>(RegionMax[0]),
					static_cast<double>(RegionMax[1])}));
			After->SetNumberField(
				TEXT("row_stride"),
				RowStride);
			After->SetField(
				TEXT("delta_local_heights"),
				NumberArray(Values));
			return AddDraftChange(
				Spec,
				TargetRef,
				EChangeIntent::Update,
				After,
				FUeAgentCanonicalValuePtr(
					Current->GetValue()),
				&Current->GetIdentity(),
				Context,
				Resources,
				OutChanges,
				OutError);
		}
	}

	TArray<FString> GetLandscapeGeometryEffectAuthorityScopes()
	{
		return {TEXT("uai:landscape:write")};
	}

	bool TryCreateLandscapeGeometryEffectDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		FString CanonicalResult;
		if (!TryCanonicalizeSchema(
				ResultSchema,
				CanonicalResult,
				OutError))
		{
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TEXT("landscape operation result schema");
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(UE_ARRAY_COUNT(DescriptorSpecs));
		for (const FDescriptorSpec& Spec : DescriptorSpecs)
		{
			FString CanonicalInput;
			if (!TryCanonicalizeSchema(
					Spec.InputSchema,
					CanonicalInput,
					OutError))
			{
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Spec.DescriptorId;
				OutRegistry.Reset();
				return false;
			}
			auto BuildDescriptor = [&](const FString& DescriptorHash)
			{
				return FUeAgentOperationDescriptor(
					FUeAgentDescriptorIdentity(
						Spec.DescriptorId,
						1,
						DescriptorHash),
					Spec.CapabilityId,
					TEXT("landscape"),
					EUeAgentDescriptorOperationKind::Operation,
					FUeAgentExecutionContextPolicy(
						{EUeAgentStatePlane::AuthoringMemory},
						{
							EUeAgentExecutionContextFacet::Level,
							EUeAgentExecutionContextFacet::World,
						},
						EUeAgentContextResolutionPolicy::ExplicitOnly,
						EUeAgentTransientContextPolicy::
							TemporaryMutationAllowed),
					FUeAgentContractValue(CanonicalInput),
					FUeAgentContractValue(CanonicalResult));
			};
			FString DescriptorHash;
			if (!TryComputeOperationDescriptorHash(
				BuildDescriptor(FString()), DescriptorHash, OutError))
			{
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Spec.DescriptorId;
				return false;
			}
			Descriptors.Add(BuildDescriptor(DescriptorHash));
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryBuildLandscapeGeometryEffectChangeSetDraft(
		const FUeAgentOperationRequest& Request,
		const FUeAgentExecutionContextRef& Context,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		OutDraft.Reset();
		OutError = FExecutionMutationError();
		if (!Request.IsValid())
		{
			auto Cause = Request.GetValidationDiagnostic();
			if (Cause.SourceKind.IsEmpty()) Cause.SourceKind = TEXT("native_result");
			return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_draft_request_invalid"), Cause));
		}
		const FValueContext RequestContext{TEXT("landscape_geometry_draft_request_invalid"), TEXT("/execution_context"), TEXT(""), TEXT("native_result")};
		const FString MissingContextField = Context.GetWorldIdentity().IsEmpty() ? TEXT("world_identity_present")
			: Context.GetLevelIdentity().IsEmpty() ? TEXT("level_identity_present")
				: Context.GetContextRevision().IsEmpty() ? TEXT("context_revision_present") : FString();
		if (!MissingContextField.IsEmpty())
			return Fail(OutError, ValueFailure(RequestContext,
				FString::Printf(TEXT("The Landscape draft execution context has no %s."), *MissingContextField.LeftChop(8)),
				RequestContext.Path(MissingContextField), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (CurrentSnapshot->GetStatePlane() != EUeAgentStatePlane::AuthoringMemory)
			return Fail(OutError, ValueFailure(RequestContext, TEXT("Landscape drafts require an authoring-memory snapshot."),
				TEXT("/current_state/state_plane"), MakeShared<FJsonValueString>(LexToString(CurrentSnapshot->GetStatePlane())),
				MakeShared<FJsonValueString>(TEXT("authoring_memory"))));

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		UE::UAI::Contract::FDiagnostic RegistryError;
		if (!TryCreateLandscapeGeometryEffectDescriptorRegistry(Registry, RegistryError))
		{
			OutError = MoveTemp(RegistryError); return false;
		}
		const auto Compatibility = Registry->CheckCompatibility(Request.GetDescriptorIdentity());
		if (Compatibility != EUeAgentDescriptorCompatibility::Exact)
		{
			const auto& Identity = Request.GetDescriptorIdentity();
			const FValueContext IdentityContext{TEXT("landscape_geometry_descriptor_not_exact"), TEXT("/descriptor_identity"), TEXT(""), TEXT("native_result")};
			switch (Compatibility)
			{
			case EUeAgentDescriptorCompatibility::NotFound:
			{
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Candidate : DescriptorSpecs) Choices.Add(MakeShared<FJsonValueString>(Candidate.DescriptorId));
				OutError = ValueFailure(IdentityContext, TEXT("The native Landscape descriptor registry does not contain this descriptor."),
					IdentityContext.Path(TEXT("descriptor_id")), MakeShared<FJsonValueString>(Identity.GetDescriptorId()), MakeShared<FJsonValueArray>(Choices));
				break;
			}
			case EUeAgentDescriptorCompatibility::VersionIncompatible:
				OutError = ValueFailure(IdentityContext, TEXT("The Landscape descriptor version differs from its registered version."),
					IdentityContext.Path(TEXT("version")), MakeShared<FJsonValueNumber>(Identity.GetVersion()), MakeShared<FJsonValueNumber>(1));
				break;
			case EUeAgentDescriptorCompatibility::HashIncompatible:
				OutError = ValueFailure(IdentityContext, TEXT("The Landscape descriptor hash does not match its registered definition."),
					IdentityContext.Path(TEXT("descriptor_hash_matches_registered")), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetDescriptorHash());
				break;
			default:
				OutError = ValueFailure(IdentityContext, TEXT("The native registry returned an unsupported descriptor compatibility result."),
					IdentityContext.Path(TEXT("compatibility")), MakeShared<FJsonValueNumber>(static_cast<int32>(Compatibility)),
					MakeShared<FJsonValueString>(TEXT("EUeAgentDescriptorCompatibility::Exact")));
				break;
			}
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Identity.GetDescriptorId();
			return false;
		}

		const FDescriptorSpec* Spec = nullptr;
		for (const FDescriptorSpec& Candidate
			: DescriptorSpecs)
		{
			const FUeAgentDescriptorIdentity& Identity =
				Request.GetDescriptorIdentity();
			if (Identity.GetDescriptorId()
					== Candidate.DescriptorId)
			{
				Spec = &Candidate;
				break;
			}
		}
		if (!Spec)
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const auto& Candidate : DescriptorSpecs) Choices.Add(MakeShared<FJsonValueString>(Candidate.DescriptorId));
			return Fail(OutError, ValueFailure({TEXT("landscape_geometry_descriptor_not_exact"), TEXT(""), TEXT(""), TEXT("native_result")},
				TEXT("The registered descriptor has no Landscape geometry specification."), TEXT("/registered_descriptor_id"),
				MakeShared<FJsonValueString>(Request.GetDescriptorIdentity().GetDescriptorId()), MakeShared<FJsonValueArray>(Choices)));
		}

		TSharedPtr<FJsonObject> Payload;
		if (!ParsePayload(Request, Payload, OutError))
		{
			return false;
		}
		TArray<FChangeDraft> Changes;
		bool bBuilt = false;
		if (Request.GetDescriptorIdentity().GetDescriptorId()
			== TEXT("uai.operation.landscape.create"))
		{
			bBuilt = BuildCreate(
				Payload,
				*Spec,
				Context,
				Resources,
				Changes,
				OutError);
		}
		else
		{
			bBuilt = BuildHeightDelta(
				Payload,
				*Spec,
				CurrentSnapshot,
				Context,
				Resources,
				Changes,
				OutError);
		}
		if (!bBuilt)
		{
			return false;
		}

		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			MoveTemp(Changes));
		if (!Draft.IsValid())
		{
			auto Cause = Draft.GetValidationDiagnostic();
			if (Cause.SourceKind.IsEmpty()) Cause.SourceKind = TEXT("native_result");
			return Fail(OutError, FExecutionMutationError::WrapFailure(TEXT("landscape_geometry_change_set_draft_invalid"), Cause));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}
}
