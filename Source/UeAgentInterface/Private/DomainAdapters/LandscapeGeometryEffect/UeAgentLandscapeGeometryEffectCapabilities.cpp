#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
			TEXT(R"json({"type":"object","properties":{"target_ref":{"type":"string","minLength":1},"topology":{"type":"object","properties":{"quads_per_section":{"type":"number","enum":[7,15,31,63,127,255]},"sections_per_component":{"type":"number","enum":[1,2]},"component_count":{"type":"array","items":{"type":"number","minimum":1,"maximum":64},"minItems":2,"maxItems":2}},"required":["quads_per_section","sections_per_component","component_count"],"additionalProperties":false},"placement":{"type":"object","properties":{"subject_point":{"const":"sample_origin"},"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotation_quaternion_xyzw":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"scale":{"type":"array","items":{"type":"number","exclusiveMinimum":0},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["subject_point","frame_id","unit","translation","rotation_quaternion_xyzw","scale","basis_evidence"],"additionalProperties":false},"sample_region":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"min_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"max_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","min_inclusive","max_inclusive","basis_evidence"],"additionalProperties":false},"initial_height":{"type":"object","properties":{"encoding":{"const":"landscape_local_height"},"value":{"type":"number"}},"required":["encoding","value"],"additionalProperties":false}},"required":["target_ref","topology","placement","sample_region","initial_height"],"additionalProperties":false})json");

		constexpr const TCHAR* HeightDeltaInputSchema =
			TEXT(R"json({"type":"object","properties":{"target_ref":{"type":"string","minLength":1},"space_contract":{"type":"object","properties":{"semantic_frame_id":{"type":"string","minLength":1},"write_frame_id":{"const":"ue.landscape.local_sample_grid"},"read_frame_id":{"const":"ue.landscape.local_sample_grid"},"verify_frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"expected_scale":{"type":"array","items":{"type":"number","exclusiveMinimum":0},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1},"conversion_chain":{"type":"string","minLength":1}},"required":["semantic_frame_id","write_frame_id","read_frame_id","verify_frame_id","unit","expected_scale","basis_evidence","conversion_chain"],"additionalProperties":false},"region":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"min_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"max_inclusive":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2}},"required":["frame_id","min_inclusive","max_inclusive"],"additionalProperties":false},"delta":{"type":"object","properties":{"encoding":{"const":"landscape_local_height_delta"},"row_stride":{"type":"number","minimum":1},"values":{"type":"array","items":{"type":"number"},"minItems":1}},"required":["encoding","row_stride","values"],"additionalProperties":false}},"required":["target_ref","space_contract","region","delta"],"additionalProperties":false})json");

		struct FDescriptorSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* DescriptorHash;
			const TCHAR* CapabilityId;
			const TCHAR* NativeTargetKind;
			const TCHAR* InputSchema;
		};

		const FDescriptorSpec DescriptorSpecs[] = {
			{
				TEXT("uai.operation.landscape.create"),
				TEXT("sha256:237659fc6faffd0b1d9b475624939f0140ff45417ef2654601a8618be58fd7eb"),
				TEXT("landscape.create"),
				TEXT("landscape.geometry.create"),
				CreateInputSchema,
			},
			{
				TEXT("uai.operation.landscape.height_delta"),
				TEXT("sha256:cfca3d633f0fde98f4299fd6c1cf6f0a974f1a72448ba9e63938da8c66d6acff"),
				TEXT("landscape.height_delta"),
				TEXT("landscape.geometry.height_delta"),
				HeightDeltaInputSchema,
			},
		};

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		bool TryCanonicalizeSchema(
			const TCHAR* Source,
			FString& OutCanonical,
			FString& OutError)
		{
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Source);
			if (!FJsonSerializer::Deserialize(Reader, Object)
				|| !Object.IsValid())
			{
				OutError =
					TEXT("landscape_geometry_schema_parse_failed");
				return false;
			}
			return Json::TryCanonicalize(
				Object,
				OutCanonical,
				OutError);
		}

		bool ParsePayload(
			const FUeAgentOperationRequest& Request,
			TSharedPtr<FJsonObject>& OutPayload,
			FExecutionMutationError& OutError)
		{
			OutPayload.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Request.GetPayload().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutPayload)
				|| !OutPayload.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_payload_parse_failed"));
			}
			return true;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_internal_value_missing"));
			}
			switch (JsonValue->Type)
			{
			case EJson::Null:
				if (FUeAgentCanonicalValue::TryNull(
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Boolean:
				if (FUeAgentCanonicalValue::TryBoolean(
						JsonValue->AsBool(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::String:
				if (FUeAgentCanonicalValue::TryString(
						JsonValue->AsString(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Number:
				if (FUeAgentCanonicalValue::TryDecimal(
						FString::SanitizeFloat(
							JsonValue->AsNumber()),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Item,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
							: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			default:
				return Fail(
					OutError,
					TEXT(
						"landscape_geometry_internal_value_kind"));
			}
			return Fail(
				OutError,
				Error.Code.IsEmpty()
					? TEXT(
						"landscape_geometry_internal_value_failed")
					: Error.Code,
				Error.Detail);
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

		bool ReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values
				|| Values->Num() != ExpectedCount)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				double Number = 0.0;
				if (!Value.IsValid()
					|| !Value->TryGetNumber(Number)
					|| !FMath::IsFinite(Number))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool ToInteger(
			const double Value,
			int32& OutValue)
		{
			if (!FMath::IsFinite(Value)
				|| Value < static_cast<double>(MIN_int32)
				|| Value > static_cast<double>(MAX_int32))
			{
				return false;
			}
			const int32 Rounded = FMath::RoundToInt(Value);
			if (!FMath::IsNearlyEqual(
					Value,
					static_cast<double>(Rounded),
					UE_DOUBLE_SMALL_NUMBER))
			{
				return false;
			}
			OutValue = Rounded;
			return true;
		}

		bool ReadIntegerField(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			int32& OutValue)
		{
			double Number = 0.0;
			return Object.IsValid()
				&& Object->TryGetNumberField(Field, Number)
				&& ToInteger(Number, OutValue);
		}

		bool ReadIntegerArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 ExpectedCount,
			TArray<int32>& OutValues)
		{
			TArray<double> Numbers;
			if (!ReadNumberArray(
					Object,
					Field,
					ExpectedCount,
					Numbers))
			{
				return false;
			}
			OutValues.Reset();
			for (const double Number : Numbers)
			{
				int32 Integer = 0;
				if (!ToInteger(Number, Integer))
				{
					return false;
				}
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

		bool ReadCanonicalNumberArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const FUeAgentCanonicalValuePtr Value =
				FindCanonicalField(Object, Field);
			if (!Value.IsValid()
				|| Value->GetKind() != EUeAgentValueKind::Array
				|| Value->GetArrayItems().Num()
					!= ExpectedCount)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Decimal
					&& Item->GetKind()
						!= EUeAgentValueKind::Integer)
				{
					return false;
				}
				const double Number =
					FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return false;
				}
				OutValues.Add(Number);
			}
			return true;
		}

		bool ReadCanonicalIntegerArray(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			const int32 ExpectedCount,
			TArray<int32>& OutValues)
		{
			TArray<double> Numbers;
			if (!ReadCanonicalNumberArray(
					Object,
					Field,
					ExpectedCount,
					Numbers))
			{
				return false;
			}
			OutValues.Reset();
			for (const double Number : Numbers)
			{
				int32 Integer = 0;
				if (!ToInteger(Number, Integer))
				{
					return false;
				}
				OutValues.Add(Integer);
			}
			return true;
		}

		const FUeAgentStateEntry* FindCurrentEntry(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& NativeTargetKind,
			const FString& Selector,
			const FString& WorldIdentity)
		{
			const FUeAgentStateEntry* Match = nullptr;
			for (const FUeAgentStateEntry& Entry
				: Snapshot->GetEntries())
			{
				const FUeAgentCanonicalIdentity& Identity =
					Entry.GetIdentity();
				if (Identity.GetTargetKind() != NativeTargetKind
					|| Identity.GetSelector() != Selector
					|| Identity.GetOwnerId() != WorldIdentity)
				{
					continue;
				}
				if (Match)
				{
					return nullptr;
				}
				Match = &Entry;
			}
			return Match;
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
			if (!TryCanonicalValue(
					MakeShared<FJsonValueObject>(InternalAfter),
					Resources,
					After,
					OutError))
			{
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
			return OutChanges.Last().IsValid()
				|| Fail(
					OutError,
					TEXT(
						"landscape_geometry_change_draft_invalid"),
					TargetRef);
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
			if (!Payload->TryGetStringField(
					TEXT("target_ref"),
					TargetRef)
				|| TargetRef.IsEmpty()
				|| !Payload->TryGetObjectField(
					TEXT("topology"),
					Topology)
				|| !Payload->TryGetObjectField(
					TEXT("placement"),
					Placement)
				|| !Payload->TryGetObjectField(
					TEXT("sample_region"),
					Region)
				|| !Payload->TryGetObjectField(
					TEXT("initial_height"),
					InitialHeight)
				|| !Topology
				|| !Placement
				|| !Region
				|| !InitialHeight)
			{
				return Fail(
					OutError,
					TEXT("landscape_create_payload_invalid"));
			}

			int32 QuadsPerSection = 0;
			int32 SectionsPerComponent = 0;
			TArray<int32> ComponentCount;
			TArray<int32> MinInclusive;
			TArray<int32> MaxInclusive;
			if (!ReadIntegerField(
					*Topology,
					TEXT("quads_per_section"),
					QuadsPerSection)
				|| !ReadIntegerField(
					*Topology,
					TEXT("sections_per_component"),
					SectionsPerComponent)
				|| !ReadIntegerArray(
					*Topology,
					TEXT("component_count"),
					2,
					ComponentCount)
				|| !ReadIntegerArray(
					*Region,
					TEXT("min_inclusive"),
					2,
					MinInclusive)
				|| !ReadIntegerArray(
					*Region,
					TEXT("max_inclusive"),
					2,
					MaxInclusive)
				|| QuadsPerSection <= 0
				|| (SectionsPerComponent != 1
					&& SectionsPerComponent != 2)
				|| ComponentCount[0] <= 0
				|| ComponentCount[1] <= 0)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_create_topology_invalid"));
			}
			const int64 QuadsX =
				static_cast<int64>(QuadsPerSection)
				* SectionsPerComponent * ComponentCount[0];
			const int64 QuadsY =
				static_cast<int64>(QuadsPerSection)
				* SectionsPerComponent * ComponentCount[1];
			if (MaxInclusive[0] < MinInclusive[0]
				|| MaxInclusive[1] < MinInclusive[1]
				|| static_cast<int64>(
					MaxInclusive[0] - MinInclusive[0])
					!= QuadsX
				|| static_cast<int64>(
					MaxInclusive[1] - MinInclusive[1])
					!= QuadsY)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_create_region_topology_mismatch"));
			}

			FString FrameId;
			FString Unit;
			FString BasisEvidence;
			FString SubjectPoint;
			(*Placement)->TryGetStringField(
				TEXT("frame_id"),
				FrameId);
			(*Placement)->TryGetStringField(TEXT("unit"), Unit);
			(*Placement)->TryGetStringField(
				TEXT("basis_evidence"),
				BasisEvidence);
			(*Placement)->TryGetStringField(
				TEXT("subject_point"),
				SubjectPoint);
			TArray<double> Translation;
			TArray<double> Rotation;
			TArray<double> Scale;
			if (FrameId != TEXT("ue.world")
				|| SubjectPoint != TEXT("sample_origin")
				|| BasisEvidence.IsEmpty()
				|| !ReadNumberArray(
					*Placement,
					TEXT("translation"),
					3,
					Translation)
				|| !ReadNumberArray(
					*Placement,
					TEXT("rotation_quaternion_xyzw"),
					4,
					Rotation)
				|| !ReadNumberArray(
					*Placement,
					TEXT("scale"),
					3,
					Scale)
				|| Scale.ContainsByPredicate(
					[](const double Value)
					{
						return Value <= 0.0;
					}))
			{
				return Fail(
					OutError,
					TEXT("landscape_create_space_contract_invalid"));
			}
			const FQuat Quat(
				Rotation[0],
				Rotation[1],
				Rotation[2],
				Rotation[3]);
			if (!Quat.IsNormalized())
			{
				return Fail(
					OutError,
					TEXT("landscape_create_rotation_invalid"));
			}
			const double UnitScale =
				Unit == TEXT("meter")
					? 100.0
					: Unit == TEXT("centimeter") ? 1.0 : 0.0;
			if (UnitScale == 0.0)
			{
				return Fail(
					OutError,
					TEXT("landscape_create_unit_invalid"));
			}
			for (double& Value : Translation)
			{
				Value *= UnitScale;
			}

			double InitialLocalHeight = 0.0;
			if (!(*InitialHeight)->TryGetNumberField(
					TEXT("value"),
					InitialLocalHeight)
				|| !FMath::IsFinite(InitialLocalHeight)
				|| InitialLocalHeight < -256.0
				|| InitialLocalHeight > 255.9921875)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_create_initial_height_invalid"));
			}

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
			if (!Payload->TryGetStringField(
					TEXT("target_ref"),
					TargetRef)
				|| TargetRef.IsEmpty()
				|| !Payload->TryGetObjectField(
					TEXT("space_contract"),
					SpaceContract)
				|| !Payload->TryGetObjectField(
					TEXT("region"),
					Region)
				|| !Payload->TryGetObjectField(
					TEXT("delta"),
					Delta)
				|| !SpaceContract
				|| !Region
				|| !Delta)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_payload_invalid"));
			}

			const FUeAgentStateEntry* Current =
				FindCurrentEntry(
					CurrentSnapshot,
					Spec.NativeTargetKind,
					TargetRef,
					Context.GetWorldIdentity());
			if (!Current)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_current_state_missing"),
					TargetRef);
			}
			const FUeAgentCanonicalValuePtr CurrentValue(
				Current->GetValue());
			TArray<double> CurrentScale;
			const FUeAgentCanonicalValuePtr Extent =
				FindCanonicalField(
					CurrentValue,
					TEXT("extent"));
			TArray<int32> ExtentMin;
			TArray<int32> ExtentMax;
			if (!ReadCanonicalNumberArray(
					CurrentValue,
					TEXT("scale"),
					3,
					CurrentScale)
				|| !ReadCanonicalIntegerArray(
					Extent,
					TEXT("min_inclusive"),
					2,
					ExtentMin)
				|| !ReadCanonicalIntegerArray(
					Extent,
					TEXT("max_inclusive"),
					2,
					ExtentMax))
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_current_state_invalid"));
			}

			TArray<double> ExpectedScale;
			if (!ReadNumberArray(
					*SpaceContract,
					TEXT("expected_scale"),
					3,
					ExpectedScale))
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_space_contract_invalid"));
			}
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!FMath::IsNearlyEqual(
						CurrentScale[Index],
						ExpectedScale[Index],
						UE_DOUBLE_KINDA_SMALL_NUMBER))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_height_delta_scale_stale"));
				}
			}
			FString SemanticFrame;
			FString VerifyFrame;
			FString WriteFrame;
			FString ReadFrame;
			FString Unit;
			FString BasisEvidence;
			FString ConversionChain;
			(*SpaceContract)->TryGetStringField(
				TEXT("semantic_frame_id"),
				SemanticFrame);
			(*SpaceContract)->TryGetStringField(
				TEXT("verify_frame_id"),
				VerifyFrame);
			(*SpaceContract)->TryGetStringField(
				TEXT("write_frame_id"),
				WriteFrame);
			(*SpaceContract)->TryGetStringField(
				TEXT("read_frame_id"),
				ReadFrame);
			(*SpaceContract)->TryGetStringField(
				TEXT("unit"),
				Unit);
			(*SpaceContract)->TryGetStringField(
				TEXT("basis_evidence"),
				BasisEvidence);
			(*SpaceContract)->TryGetStringField(
				TEXT("conversion_chain"),
				ConversionChain);
			if (SemanticFrame.IsEmpty()
				|| VerifyFrame != SemanticFrame
				|| WriteFrame
					!= TEXT(
						"ue.landscape.local_sample_grid")
				|| ReadFrame != WriteFrame
				|| (Unit != TEXT("centimeter")
					&& Unit != TEXT("meter"))
				|| BasisEvidence.IsEmpty()
				|| ConversionChain.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_space_contract_invalid"));
			}

			TArray<int32> RegionMin;
			TArray<int32> RegionMax;
			if (!ReadIntegerArray(
					*Region,
					TEXT("min_inclusive"),
					2,
					RegionMin)
				|| !ReadIntegerArray(
					*Region,
					TEXT("max_inclusive"),
					2,
					RegionMax)
				|| RegionMin[0] > RegionMax[0]
				|| RegionMin[1] > RegionMax[1]
				|| RegionMin[0] < ExtentMin[0]
				|| RegionMin[1] < ExtentMin[1]
				|| RegionMax[0] > ExtentMax[0]
				|| RegionMax[1] > ExtentMax[1])
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_region_out_of_bounds"));
			}
			const int32 Width =
				RegionMax[0] - RegionMin[0] + 1;
			const int32 Height =
				RegionMax[1] - RegionMin[1] + 1;
			int32 RowStride = 0;
			const TArray<TSharedPtr<FJsonValue>>* DeltaValues =
				nullptr;
			if (!ReadIntegerField(
					*Delta,
					TEXT("row_stride"),
					RowStride)
				|| !(*Delta)->TryGetArrayField(
					TEXT("values"),
					DeltaValues)
				|| !DeltaValues
				|| RowStride != Width
				|| static_cast<int64>(DeltaValues->Num())
					!= static_cast<int64>(Width) * Height)
			{
				return Fail(
					OutError,
					TEXT(
						"landscape_height_delta_sample_count_mismatch"));
			}
			TArray<double> Values;
			Values.Reserve(DeltaValues->Num());
			for (const TSharedPtr<FJsonValue>& Value
				: *DeltaValues)
			{
				double Number = 0.0;
				if (!Value.IsValid()
					|| !Value->TryGetNumber(Number)
					|| !FMath::IsFinite(Number))
				{
					return Fail(
						OutError,
						TEXT(
							"landscape_height_delta_value_invalid"));
				}
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
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		FString CanonicalResult;
		if (!TryCanonicalizeSchema(
				ResultSchema,
				CanonicalResult,
				OutError))
		{
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
				OutRegistry.Reset();
				return false;
			}
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(
					Spec.DescriptorId,
					1,
					Spec.DescriptorHash),
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
				FUeAgentContractValue(MoveTemp(CanonicalInput)),
				FUeAgentContractValue(CanonicalResult));
		}
		if (Descriptors.Num() != 2)
		{
			OutError =
				TEXT(
					"landscape_geometry_descriptor_count_invalid");
			return false;
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
		if (!Request.IsValid()
			|| Context.GetWorldIdentity().IsEmpty()
			|| Context.GetLevelIdentity().IsEmpty()
			|| Context.GetContextRevision().IsEmpty()
			|| CurrentSnapshot->GetStatePlane()
				!= EUeAgentStatePlane::AuthoringMemory)
		{
			return Fail(
				OutError,
				TEXT(
					"landscape_geometry_draft_request_invalid"));
		}

		const FDescriptorSpec* Spec = nullptr;
		for (const FDescriptorSpec& Candidate
			: DescriptorSpecs)
		{
			const FUeAgentDescriptorIdentity& Identity =
				Request.GetDescriptorIdentity();
			if (Identity.GetDescriptorId()
					== Candidate.DescriptorId
				&& Identity.GetVersion() == 1
				&& Identity.GetDescriptorHash()
					== Candidate.DescriptorHash)
			{
				Spec = &Candidate;
				break;
			}
		}
		if (!Spec)
		{
			return Fail(
				OutError,
				TEXT(
					"landscape_geometry_descriptor_not_exact"));
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
			return Fail(
				OutError,
				TEXT(
					"landscape_geometry_change_set_draft_invalid"));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}
}
