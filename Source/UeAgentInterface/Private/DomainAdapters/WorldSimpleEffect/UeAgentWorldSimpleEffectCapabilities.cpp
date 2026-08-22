#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
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
				TEXT("uai.operation.world.actor.create"),
				TEXT("sha256:7ee82512ce0c69025653e38cac976c1aa55d529e8e053c81da8a6c6f0fdc8619"),
				TEXT("world.actor.create"),
				TEXT("world.entity.create"),
				TEXT(R"json({"type":"object","properties":{"actor_class_ref":{"type":"string","minLength":1},"placement":{"type":"object","properties":{"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotation_quaternion_xyzw":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","unit","translation","rotation_quaternion_xyzw","scale","basis_evidence"],"additionalProperties":false},"label":{"type":"string","minLength":1},"folder_path":{"type":"string"},"tags":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["actor_class_ref","placement","label"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.wall.create_with_opening"),
				TEXT("sha256:afae257cfa65ee52931f7a03480eb812e104b80ff326f0423e0bbb639770d4d2"),
				TEXT("world.wall.create_with_opening"),
				TEXT("world.entity.create"),
				TEXT(R"json({"type":"object","properties":{"plane":{"type":"object","properties":{"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"center":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"normal":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"up":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","unit","center","normal","up","basis_evidence"],"additionalProperties":false},"wall":{"type":"object","properties":{"thickness":{"type":"number","exclusiveMinimum":0},"width":{"type":"number","exclusiveMinimum":0},"height":{"type":"number","exclusiveMinimum":0}},"required":["thickness","width","height"],"additionalProperties":false},"opening":{"type":"object","properties":{"center":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"size":{"type":"array","items":{"type":"number","exclusiveMinimum":0},"minItems":2,"maxItems":2},"padding":{"type":"number","minimum":0},"out_of_bounds":{"type":"string","enum":["reject","clamp"]},"minimum_segment_size":{"type":"number","minimum":0}},"required":["center","size"],"additionalProperties":false},"segment_template":{"type":"object","properties":{"actor_class_ref":{"type":"string","minLength":1},"mesh_ref":{"type":"string","minLength":1},"folder_path":{"type":"string"},"label_prefix":{"type":"string","minLength":1}},"required":["actor_class_ref","mesh_ref"],"additionalProperties":false}},"required":["plane","wall","opening","segment_template"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.data_layer_instance.create"),
				TEXT("sha256:adfcd0f822359bfb044f5b3946287bfca5f9746118057d871b5524d69d673178"),
				TEXT("world.data_layer_instance.create"),
				TEXT("world.entity.create"),
				TEXT(R"json({"oneOf":[{"type":"object","properties":{"asset_ref":{"type":"string","minLength":1},"parent_ref":{"type":"string","minLength":1},"private":{"const":false},"initial_state":{"type":"object","properties":{"initial_runtime_state":{"type":"string","enum":["unloaded","loaded","activated"]},"initially_visible":{"type":"boolean"},"loaded_in_editor":{"type":"boolean"},"visible":{"type":"boolean"}},"minProperties":1,"additionalProperties":false}},"required":["asset_ref"],"additionalProperties":false},{"type":"object","properties":{"parent_ref":{"type":"string","minLength":1},"private":{"const":true},"short_name":{"type":"string","minLength":1},"initial_state":{"type":"object","properties":{"initial_runtime_state":{"type":"string","enum":["unloaded","loaded","activated"]},"initially_visible":{"type":"boolean"},"loaded_in_editor":{"type":"boolean"},"visible":{"type":"boolean"}},"minProperties":1,"additionalProperties":false}},"required":["private","short_name"],"additionalProperties":false}]})json"),
			},
			{
				TEXT("uai.operation.world.data_layer_relation.update"),
				TEXT("sha256:d6572ec8ab59510bcfe91db0f545465a604567a4715101b3c45115db7d7adf9c"),
				TEXT("world.data_layer_relation.update"),
				TEXT("world.membership.update"),
				TEXT(R"json({"oneOf":[{"type":"object","properties":{"relation_kind":{"const":"actor_membership"},"subject_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"collection_refs":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"mode":{"type":"string","enum":["add","remove","replace","clear"]}},"required":["relation_kind","subject_refs","collection_refs","mode"],"additionalProperties":false},{"type":"object","properties":{"relation_kind":{"const":"parent"},"subject_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"collection_refs":{"type":"array","items":{"type":"string","minLength":1},"maxItems":1,"uniqueItems":true},"mode":{"type":"string","enum":["replace","clear"]}},"required":["relation_kind","subject_refs","collection_refs","mode"],"additionalProperties":false}]})json"),
			},
			{
				TEXT("uai.operation.world.data_layer_state.update"),
				TEXT("sha256:d75a283bac3d696d2e8c7e141302957644de688eaa10704127527e22266ad404"),
				TEXT("world.data_layer_state.update"),
				TEXT("world.state.update"),
				TEXT(R"json({"type":"object","properties":{"target_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"changes":{"type":"object","properties":{"initial_runtime_state":{"type":"string","enum":["unloaded","loaded","activated"]},"initially_visible":{"type":"boolean"},"loaded_in_editor":{"type":"boolean"},"visible":{"type":"boolean"},"short_name":{"type":"string","minLength":1}},"minProperties":1,"additionalProperties":false}},"required":["target_refs","changes"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.actor_hlod_layer.update"),
				TEXT("sha256:3a4945c7b3ab00c58cde8a475610f855f16fb53cdc3e656bac310e3d7a3c76a0"),
				TEXT("world.actor_hlod_layer.update"),
				TEXT("world.membership.update"),
				TEXT(R"json({"oneOf":[{"type":"object","properties":{"subject_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"mode":{"const":"replace"},"collection_ref":{"type":"string","minLength":1}},"required":["subject_refs","mode","collection_ref"],"additionalProperties":false},{"type":"object","properties":{"subject_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"mode":{"const":"clear"}},"required":["subject_refs","mode"],"additionalProperties":false}]})json"),
			},
			{
				TEXT("uai.operation.world.actor_streaming_policy.update"),
				TEXT("sha256:127c8f4013c746cc0786d82e7ca5217591923b8225fd5e42ef90723b7f501373"),
				TEXT("world.actor_streaming_policy.update"),
				TEXT("world.state.update"),
				TEXT(R"json({"type":"object","properties":{"target_refs":{"type":"array","items":{"type":"string","minLength":1},"minItems":1,"uniqueItems":true},"changes":{"type":"object","properties":{"runtime_grid":{"oneOf":[{"type":"object","properties":{"intent":{"const":"set"},"value":{"type":"string","minLength":1}},"required":["intent","value"],"additionalProperties":false},{"type":"object","properties":{"intent":{"const":"clear"}},"required":["intent"],"additionalProperties":false}]},"is_spatially_loaded":{"type":"boolean"}},"minProperties":1,"additionalProperties":false}},"required":["target_refs","changes"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.streaming_relation.create"),
				TEXT("sha256:a320e65a408b02338329c76279e08952761a08cf1491acc7d820ec224599903f"),
				TEXT("world.streaming_relation.create"),
				TEXT("world.entity.create"),
				TEXT(R"json({"type":"object","properties":{"level_asset_ref":{"type":"string","minLength":1},"streaming_class_ref":{"type":"string","minLength":1},"placement":{"type":"object","properties":{"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotation_quaternion_xyzw":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","unit","translation","rotation_quaternion_xyzw","scale","basis_evidence"],"additionalProperties":false}},"required":["level_asset_ref","streaming_class_ref"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.streaming_relation.state.update"),
				TEXT("sha256:b2bec3d2ba22b96ed3ce490fffed6868944db0e44bd6bfb3990fa1d530ee3b40"),
				TEXT("world.streaming_relation.state.update"),
				TEXT("world.state.update"),
				TEXT(R"json({"type":"object","properties":{"target_ref":{"type":"string","minLength":1},"changes":{"type":"object","properties":{"loaded":{"type":"boolean"},"visible":{"type":"boolean"},"placement":{"type":"object","properties":{"frame_id":{"type":"string","minLength":1},"unit":{"type":"string","enum":["centimeter","meter"]},"translation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"rotation_quaternion_xyzw":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","unit","translation","rotation_quaternion_xyzw","scale","basis_evidence"],"additionalProperties":false}},"minProperties":1,"additionalProperties":false}},"required":["target_ref","changes"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.world_partition_streaming.update"),
				TEXT("sha256:5419fbf11af5d86ee907d1e3e8163506b225783df56f646c68f394c3cba9294c"),
				TEXT("world.world_partition_streaming.update"),
				TEXT("world.state.update"),
				TEXT(R"json({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"],"additionalProperties":false})json"),
			},
			{
				TEXT("uai.operation.world.level_game_mode.update"),
				TEXT("sha256:49c9cf01c69ba7ada6d488bbcb69c1fcc32a09a4360f1076e83942f116fc4774"),
				TEXT("world.level_game_mode.update"),
				TEXT("world.state.update"),
				TEXT(R"json({"type":"object","properties":{"game_mode_class_ref":{"type":"string","minLength":1}},"required":["game_mode_class_ref"],"additionalProperties":false})json"),
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
				OutError = TEXT("world_simple_schema_parse_failed");
				return false;
			}
			return Json::TryCanonicalize(
				Object,
				OutCanonical,
				OutError);
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
					TEXT("world_simple_internal_value_missing"));
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
					TArray<TPair<
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
					TEXT("world_simple_internal_value_kind_invalid"));
			}
			return Fail(
				OutError,
				Error.Code.IsEmpty()
					? TEXT("world_simple_internal_value_failed")
					: Error.Code,
				Error.Detail);
		}

		TSharedPtr<FJsonValue> CloneField(
			const TSharedPtr<FJsonObject>& Source,
			const TCHAR* Field)
		{
			if (!Source.IsValid())
			{
				return nullptr;
			}
			return Source->TryGetField(Field);
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
					TEXT("world_simple_payload_parse_failed"));
			}
			return true;
		}

		TSharedPtr<FJsonValue> NumberArray(
			const TArray<double>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const double Value : Values)
			{
				Items.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		bool ReadVector(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 Count,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values
				|| Values->Num() != Count)
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

		bool AddDraftChange(
			const FString& NativeTargetKind,
			const FString& Selector,
			const FString& Role,
			const int32 Index,
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
						FString::Printf(
							TEXT("uai.world.simple:%s:%d"),
							*Role,
							Index),
						NativeTargetKind,
						Context.GetWorldIdentity(),
						Selector,
						Role,
						Context.GetContextRevision());
			OutChanges.Emplace(
				Identity,
				Intent,
				ExpectedBefore,
				After,
				GetWorldSimpleEffectAuthorityScopes(),
				TEXT("world-simple:canonical-draft"),
				TArray<FUeAgentCanonicalIdentity>{});
			return OutChanges.Last().IsValid()
				|| Fail(
					OutError,
					TEXT("world_simple_change_draft_invalid"),
					Selector);
		}

		const FUeAgentStateEntry* FindCurrentEntry(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& NativeTargetKind,
			const FString& Selector)
		{
			const FUeAgentStateEntry* KindMatch = nullptr;
			for (const FUeAgentStateEntry& Entry
				: Snapshot->GetEntries())
			{
				if (Entry.GetIdentity().GetTargetKind()
					!= NativeTargetKind)
				{
					continue;
				}
				if (Entry.GetIdentity().GetSelector() == Selector)
				{
					return &Entry;
				}
				if (KindMatch)
				{
					KindMatch = nullptr;
					break;
				}
				KindMatch = &Entry;
			}
			return KindMatch;
		}

		bool BuildWall(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const TSharedPtr<FJsonObject>* Plane = nullptr;
			const TSharedPtr<FJsonObject>* Wall = nullptr;
			const TSharedPtr<FJsonObject>* Opening = nullptr;
			const TSharedPtr<FJsonObject>* Segment = nullptr;
			if (!Payload->TryGetObjectField(TEXT("plane"), Plane)
				|| !Payload->TryGetObjectField(TEXT("wall"), Wall)
				|| !Payload->TryGetObjectField(TEXT("opening"), Opening)
				|| !Payload->TryGetObjectField(
					TEXT("segment_template"),
					Segment)
				|| !Plane
				|| !Wall
				|| !Opening
				|| !Segment)
			{
				return Fail(
					OutError,
					TEXT("world_simple_wall_payload_invalid"));
			}

			TArray<double> Center;
			TArray<double> Normal;
			TArray<double> Up;
			TArray<double> OpeningCenter;
			TArray<double> OpeningSize;
			if (!ReadVector(*Plane, TEXT("center"), 3, Center)
				|| !ReadVector(*Plane, TEXT("normal"), 3, Normal)
				|| !ReadVector(*Plane, TEXT("up"), 3, Up)
				|| !ReadVector(
					*Opening,
					TEXT("center"),
					2,
					OpeningCenter)
				|| !ReadVector(
					*Opening,
					TEXT("size"),
					2,
					OpeningSize))
			{
				return Fail(
					OutError,
					TEXT("world_simple_wall_spatial_value_invalid"));
			}

			FString Unit;
			(*Plane)->TryGetStringField(TEXT("unit"), Unit);
			const double UnitScale =
				Unit == TEXT("meter") ? 100.0 : 1.0;
			for (double& Value : Center)
			{
				Value *= UnitScale;
			}
			const FVector PlaneCenter(
				Center[0],
				Center[1],
				Center[2]);
			const FVector PlaneNormal(
				Normal[0],
				Normal[1],
				Normal[2]);
			const FVector PlaneUp(Up[0], Up[1], Up[2]);
			const FVector N = PlaneNormal.GetSafeNormal();
			const FVector U = PlaneUp.GetSafeNormal();
			const FVector R = FVector::CrossProduct(U, N).GetSafeNormal();
			if (N.IsNearlyZero()
				|| U.IsNearlyZero()
				|| R.IsNearlyZero())
			{
				return Fail(
					OutError,
					TEXT("world_simple_wall_basis_invalid"));
			}

			const double Width =
				(*Wall)->GetNumberField(TEXT("width")) * UnitScale;
			const double Height =
				(*Wall)->GetNumberField(TEXT("height")) * UnitScale;
			const double Thickness =
				(*Wall)->GetNumberField(TEXT("thickness"))
				* UnitScale;
			const double OpeningX = OpeningCenter[0] * UnitScale;
			const double OpeningZ = OpeningCenter[1] * UnitScale;
			const double OpeningWidth =
				OpeningSize[0] * UnitScale;
			const double OpeningHeight =
				OpeningSize[1] * UnitScale;
			if (OpeningWidth > Width
				|| OpeningHeight > Height
				|| FMath::Abs(OpeningX) + OpeningWidth * 0.5
					> Width * 0.5
				|| FMath::Abs(OpeningZ) + OpeningHeight * 0.5
					> Height * 0.5)
			{
				return Fail(
					OutError,
					TEXT("world_simple_wall_opening_out_of_bounds"));
			}

			struct FPart
			{
				double HorizontalCenter;
				double VerticalCenter;
				double PartWidth;
				double PartHeight;
			};
			const double LeftEdge = OpeningX - OpeningWidth * 0.5;
			const double RightEdge = OpeningX + OpeningWidth * 0.5;
			const double BottomEdge = OpeningZ - OpeningHeight * 0.5;
			const double TopEdge = OpeningZ + OpeningHeight * 0.5;
			const FPart Parts[] = {
				{
					(-Width * 0.5 + LeftEdge) * 0.5,
					0.0,
					LeftEdge + Width * 0.5,
					Height,
				},
				{
					(RightEdge + Width * 0.5) * 0.5,
					0.0,
					Width * 0.5 - RightEdge,
					Height,
				},
				{
					OpeningX,
					(TopEdge + Height * 0.5) * 0.5,
					OpeningWidth,
					Height * 0.5 - TopEdge,
				},
				{
					OpeningX,
					(-Height * 0.5 + BottomEdge) * 0.5,
					OpeningWidth,
					BottomEdge + Height * 0.5,
				},
			};
			const FString ActorClassRef =
				(*Segment)->GetStringField(TEXT("actor_class_ref"));
			const FString MeshRef =
				(*Segment)->GetStringField(TEXT("mesh_ref"));
			FString LabelPrefix = TEXT("WorldSimpleWall_");
			(*Segment)->TryGetStringField(
				TEXT("label_prefix"),
				LabelPrefix);
			FString FolderPath;
			(*Segment)->TryGetStringField(
				TEXT("folder_path"),
				FolderPath);
			for (int32 Index = 0; Index < 4; ++Index)
			{
				const FPart& Part = Parts[Index];
				if (Part.PartWidth <= 0.0
					|| Part.PartHeight <= 0.0)
				{
					return Fail(
						OutError,
						TEXT("world_simple_wall_segment_degenerate"));
				}
				const FVector Location =
					PlaneCenter
					+ R * Part.HorizontalCenter
					+ U * Part.VerticalCenter;
				const TSharedPtr<FJsonObject> After =
					MakeShared<FJsonObject>();
				After->SetStringField(
					TEXT("entity_kind"),
					TEXT("wall_segment"));
				After->SetStringField(
					TEXT("actor_class_ref"),
					ActorClassRef);
				After->SetStringField(TEXT("mesh_ref"), MeshRef);
				After->SetStringField(
					TEXT("label"),
					FString::Printf(
						TEXT("%s%d"),
						*LabelPrefix,
						Index));
				After->SetStringField(
					TEXT("folder_path"),
					FolderPath);
				After->SetField(
					TEXT("position"),
					NumberArray({
						Location.X,
						Location.Y,
						Location.Z}));
				After->SetField(
					TEXT("basis_normal"),
					NumberArray({N.X, N.Y, N.Z}));
				After->SetField(
					TEXT("basis_up"),
					NumberArray({U.X, U.Y, U.Z}));
				After->SetField(
					TEXT("dimensions_cm"),
					NumberArray({
						Thickness,
						Part.PartWidth,
						Part.PartHeight}));
				if (!AddDraftChange(
						Spec.NativeTargetKind,
						FString::Printf(
							TEXT("wall-segment:%d"),
							Index),
						TEXT("wall_segment"),
						Index,
						EChangeIntent::Create,
						After,
						nullptr,
						nullptr,
						Context,
						Resources,
						OutChanges,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		void CopyOptionalField(
			const TSharedPtr<FJsonObject>& Source,
			const TCHAR* SourceField,
			const TSharedPtr<FJsonObject>& Target,
			const TCHAR* TargetField)
		{
			if (const TSharedPtr<FJsonValue> Value =
				CloneField(Source, SourceField))
			{
				Target->SetField(TargetField, Value);
			}
		}

		bool BuildEntityCreate(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const FString DescriptorId(Spec.DescriptorId);
			const TSharedPtr<FJsonObject> After =
				MakeShared<FJsonObject>();
			FString Selector;
			if (DescriptorId == TEXT("uai.operation.world.actor.create"))
			{
				After->SetStringField(TEXT("entity_kind"), TEXT("actor"));
				for (const TCHAR* Field : {
					TEXT("actor_class_ref"),
					TEXT("label"),
					TEXT("folder_path"),
					TEXT("tags")})
				{
					CopyOptionalField(Payload, Field, After, Field);
				}
				const TSharedPtr<FJsonObject>* Placement = nullptr;
				if (!Payload->TryGetObjectField(TEXT("placement"), Placement)
					|| !Placement || !Placement->IsValid())
				{
					return Fail(OutError, TEXT("world_simple_actor_placement_missing"));
				}
				for (const TCHAR* Field : {
					TEXT("translation"),
					TEXT("rotation_quaternion_xyzw"),
					TEXT("scale"),
					TEXT("unit")})
				{
					CopyOptionalField(*Placement, Field, After, Field);
				}
				Selector = TEXT("actor-label:")
					+ Payload->GetStringField(TEXT("label"));
			}
			else if (DescriptorId
				== TEXT(
					"uai.operation.world."
					"data_layer_instance.create"))
			{
				bool bPrivate = false;
				Payload->TryGetBoolField(
					TEXT("private"),
					bPrivate);
				After->SetStringField(
					TEXT("entity_kind"),
					TEXT("data_layer_instance"));
				After->SetBoolField(
					TEXT("is_private"),
					bPrivate);
				CopyOptionalField(
					Payload,
					TEXT("parent_ref"),
					After,
					TEXT("parent_ref"));
				if (bPrivate)
				{
					const FString ShortName =
						Payload->GetStringField(
							TEXT("short_name"));
					After->SetStringField(
						TEXT("short_name"),
						ShortName);
					Selector =
						TEXT("private-data-layer:")
						+ ShortName;
				}
				else
				{
					const FString AssetRef =
						Payload->GetStringField(
							TEXT("asset_ref"));
					After->SetStringField(
						TEXT("asset_ref"),
						AssetRef);
					Selector = AssetRef;
				}
				const TSharedPtr<FJsonObject>* Initial = nullptr;
				if (Payload->TryGetObjectField(
						TEXT("initial_state"),
						Initial)
					&& Initial)
				{
					for (const TCHAR* Field : {
						TEXT("initial_runtime_state"),
						TEXT("initially_visible"),
						TEXT("loaded_in_editor"),
						TEXT("visible")})
					{
						CopyOptionalField(
							*Initial,
							Field,
						After,
						Field);
					}
				}
			}
			else
			{
				After->SetStringField(
					TEXT("entity_kind"),
					TEXT("streaming_relation"));
				CopyOptionalField(
					Payload,
					TEXT("level_asset_ref"),
					After,
					TEXT("level_asset_ref"));
				CopyOptionalField(
					Payload,
					TEXT("streaming_class_ref"),
					After,
					TEXT("streaming_class_ref"));
				const TSharedPtr<FJsonObject>* Placement = nullptr;
				if (Payload->TryGetObjectField(
						TEXT("placement"),
						Placement)
					&& Placement)
				{
					After->SetBoolField(
						TEXT("has_transform"),
						true);
					for (const TCHAR* Field : {
						TEXT("translation"),
						TEXT("rotation_quaternion_xyzw"),
						TEXT("scale"),
						TEXT("unit")})
					{
						CopyOptionalField(
							*Placement,
							Field,
							After,
							Field);
					}
				}
				else
				{
					After->SetBoolField(
						TEXT("has_transform"),
						false);
				}
				Selector = Payload->GetStringField(
					TEXT("level_asset_ref"));
			}
			return AddDraftChange(
				Spec.NativeTargetKind,
				Selector,
				After->GetStringField(TEXT("entity_kind")),
				0,
				EChangeIntent::Create,
				After,
				nullptr,
				nullptr,
				Context,
				Resources,
				OutChanges,
				OutError);
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values)
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid()
					&& Value->TryGetString(Text))
				{
					Result.Add(Text);
				}
			}
			return Result;
		}

		bool BuildMembership(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const TArray<FString> Subjects =
				ReadStringArray(Payload, TEXT("subject_refs"));
			const FString DescriptorId(Spec.DescriptorId);
			for (int32 Index = 0; Index < Subjects.Num(); ++Index)
			{
				const FUeAgentStateEntry* Current =
					FindCurrentEntry(
						CurrentSnapshot,
						Spec.NativeTargetKind,
						Subjects[Index]);
				if (!Current)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_current_target_missing"),
						Subjects[Index]);
				}
				const TSharedPtr<FJsonObject> After =
					MakeShared<FJsonObject>();
				After->SetStringField(
					TEXT("subject_ref"),
					Subjects[Index]);
				CopyOptionalField(
					Payload,
					TEXT("mode"),
					After,
					TEXT("mode"));
				if (DescriptorId
					== TEXT(
						"uai.operation.world."
						"data_layer_relation.update"))
				{
					After->SetStringField(
						TEXT("membership_kind"),
						TEXT("data_layer_relation"));
					CopyOptionalField(
						Payload,
						TEXT("relation_kind"),
						After,
						TEXT("relation_kind"));
					const TArray<TSharedPtr<FJsonValue>>* Refs =
						nullptr;
					if (Payload->TryGetArrayField(
							TEXT("collection_refs"),
							Refs)
						&& Refs)
					{
						After->SetArrayField(
							TEXT("member_refs"),
							*Refs);
					}
				}
				else
				{
					After->SetStringField(
						TEXT("membership_kind"),
						TEXT("actor_hlod_layer"));
					CopyOptionalField(
						Payload,
						TEXT("collection_ref"),
						After,
						TEXT("member_ref"));
				}
				if (!AddDraftChange(
						Spec.NativeTargetKind,
						Subjects[Index],
						After->GetStringField(
							TEXT("membership_kind")),
						Index,
						EChangeIntent::Update,
						After,
						FUeAgentCanonicalValuePtr(
							Current->GetValue()),
						&Current->GetIdentity(),
						Context,
						Resources,
						OutChanges,
						OutError))
				{
					return false;
				}
			}
			return !OutChanges.IsEmpty()
				|| Fail(
					OutError,
					TEXT("world_simple_membership_subject_missing"));
		}

		bool BuildState(
			const TSharedPtr<FJsonObject>& Payload,
			const FDescriptorSpec& Spec,
			const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FChangeDraft>& OutChanges,
			FExecutionMutationError& OutError)
		{
			const FString DescriptorId(Spec.DescriptorId);
			TArray<FString> Targets;
			const TSharedPtr<FJsonObject>* Changes = nullptr;
			FString StateKind;
			if (DescriptorId
				== TEXT(
					"uai.operation.world.data_layer_state.update"))
			{
				Targets = ReadStringArray(
					Payload,
					TEXT("target_refs"));
				Payload->TryGetObjectField(
					TEXT("changes"),
					Changes);
				StateKind = TEXT("data_layer");
			}
			else if (DescriptorId
				== TEXT(
					"uai.operation.world."
					"actor_streaming_policy.update"))
			{
				Targets = ReadStringArray(
					Payload,
					TEXT("target_refs"));
				Payload->TryGetObjectField(
					TEXT("changes"),
					Changes);
				StateKind = TEXT("actor_streaming_policy");
			}
			else if (DescriptorId
				== TEXT(
					"uai.operation.world."
					"streaming_relation.state.update"))
			{
				Targets.Add(Payload->GetStringField(
					TEXT("target_ref")));
				Payload->TryGetObjectField(
					TEXT("changes"),
					Changes);
				StateKind = TEXT("streaming_relation");
			}
			else if (DescriptorId
				== TEXT("uai.operation.world.level_game_mode.update"))
			{
				Targets.Add(Context.GetWorldIdentity());
				StateKind = TEXT("level_game_mode");
			}
			else
			{
				Targets.Add(Context.GetWorldIdentity());
				StateKind = TEXT("world_partition_streaming");
			}
			for (int32 Index = 0; Index < Targets.Num(); ++Index)
			{
				const FUeAgentStateEntry* Current =
					FindCurrentEntry(
						CurrentSnapshot,
						Spec.NativeTargetKind,
						Targets[Index]);
				if (!Current)
				{
					return Fail(
						OutError,
						TEXT(
							"world_simple_current_target_missing"),
						Targets[Index]);
				}
				const TSharedPtr<FJsonObject> After =
					MakeShared<FJsonObject>();
				After->SetStringField(
					TEXT("state_kind"),
					StateKind);
				if (StateKind
					!= TEXT("world_partition_streaming")
					&& StateKind != TEXT("level_game_mode"))
				{
					After->SetStringField(
						TEXT("target_ref"),
						Targets[Index]);
				}
				if (StateKind == TEXT("data_layer"))
				{
					for (const TCHAR* Field : {
						TEXT("initial_runtime_state"),
						TEXT("initially_visible"),
						TEXT("loaded_in_editor"),
						TEXT("visible"),
						TEXT("short_name")})
					{
						CopyOptionalField(
							Changes ? *Changes : nullptr,
							Field,
							After,
							Field);
					}
				}
				else if (
					StateKind
					== TEXT("actor_streaming_policy"))
				{
					CopyOptionalField(
						Changes ? *Changes : nullptr,
						TEXT("is_spatially_loaded"),
						After,
						TEXT("is_spatially_loaded"));
					const TSharedPtr<FJsonObject>* Grid = nullptr;
					if (Changes
						&& (*Changes)->TryGetObjectField(
							TEXT("runtime_grid"),
							Grid)
						&& Grid)
					{
						CopyOptionalField(
							*Grid,
							TEXT("intent"),
							After,
							TEXT("runtime_grid_intent"));
						CopyOptionalField(
							*Grid,
							TEXT("value"),
							After,
							TEXT("runtime_grid_value"));
					}
				}
				else if (StateKind == TEXT("streaming_relation"))
				{
					CopyOptionalField(
						Changes ? *Changes : nullptr,
						TEXT("loaded"),
						After,
						TEXT("loaded"));
					CopyOptionalField(
						Changes ? *Changes : nullptr,
						TEXT("visible"),
						After,
						TEXT("visible"));
					const TSharedPtr<FJsonObject>* Placement = nullptr;
					if (Changes
						&& (*Changes)->TryGetObjectField(
							TEXT("placement"),
							Placement)
						&& Placement)
					{
						After->SetBoolField(
							TEXT("has_transform"),
							true);
						for (const TCHAR* Field : {
							TEXT("translation"),
							TEXT("rotation_quaternion_xyzw"),
							TEXT("scale"),
							TEXT("unit")})
						{
							CopyOptionalField(
								*Placement,
								Field,
								After,
								Field);
						}
					}
				}
				else if (StateKind == TEXT("level_game_mode"))
				{
					CopyOptionalField(
						Payload,
						TEXT("game_mode_class_ref"),
						After,
						TEXT("game_mode_class_ref"));
				}
				else
				{
					CopyOptionalField(
						Payload,
						TEXT("enabled"),
						After,
						TEXT("enabled"));
				}
				if (!AddDraftChange(
						Spec.NativeTargetKind,
						Targets[Index],
						StateKind,
						Index,
						EChangeIntent::Update,
						After,
						FUeAgentCanonicalValuePtr(
							Current->GetValue()),
						&Current->GetIdentity(),
						Context,
						Resources,
						OutChanges,
						OutError))
				{
					return false;
				}
			}
			return !OutChanges.IsEmpty()
				|| Fail(
					OutError,
					TEXT("world_simple_state_target_missing"));
		}
	}

	TArray<FString> GetWorldSimpleEffectAuthorityScopes()
	{
		return {TEXT("uai:world:write")};
	}

	bool TryCreateWorldSimpleEffectDescriptorRegistry(
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
				TEXT("world"),
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
		if (Descriptors.Num() != 11)
		{
			OutError =
				TEXT("world_simple_descriptor_count_invalid");
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryBuildWorldSimpleEffectChangeSetDraft(
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
				TEXT("world_simple_draft_request_invalid"));
		}

		const FDescriptorSpec* Spec = nullptr;
		for (const FDescriptorSpec& Candidate : DescriptorSpecs)
		{
			if (Request.GetDescriptorIdentity().GetDescriptorId()
					== Candidate.DescriptorId
				&& Request.GetDescriptorIdentity().GetVersion() == 1
				&& Request.GetDescriptorIdentity()
						.GetDescriptorHash()
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
				TEXT("world_simple_descriptor_not_exact"));
		}

		TSharedPtr<FJsonObject> Payload;
		if (!ParsePayload(Request, Payload, OutError))
		{
			return false;
		}
		TArray<FChangeDraft> Changes;
		const FString NativeTargetKind(Spec->NativeTargetKind);
		bool bBuilt = false;
		if (Request.GetDescriptorIdentity().GetDescriptorId()
			== TEXT(
				"uai.operation.world.wall.create_with_opening"))
		{
			bBuilt = BuildWall(
				Payload,
				*Spec,
				Context,
				Resources,
				Changes,
				OutError);
		}
		else if (NativeTargetKind == TEXT("world.entity.create"))
		{
			bBuilt = BuildEntityCreate(
				Payload,
				*Spec,
				Context,
				Resources,
				Changes,
				OutError);
		}
		else if (NativeTargetKind
			== TEXT("world.membership.update"))
		{
			if (CurrentSnapshot->GetEntries().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("world_simple_current_state_missing"));
			}
			bBuilt = BuildMembership(
				Payload,
				*Spec,
				CurrentSnapshot,
				Context,
				Resources,
				Changes,
				OutError);
		}
		else
		{
			if (CurrentSnapshot->GetEntries().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("world_simple_current_state_missing"));
			}
			bBuilt = BuildState(
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
				TEXT("world_simple_change_set_draft_invalid"));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}
}
