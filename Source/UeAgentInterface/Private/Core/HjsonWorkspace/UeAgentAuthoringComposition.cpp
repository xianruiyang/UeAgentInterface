#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"

#include "Containers/StringConv.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	bool FCompositionSnapshotBinding::IsValid() const
	{
		return !SnapshotId.TrimStartAndEnd().IsEmpty()
			&& Revision.IsValid()
			&& !EvidenceId.TrimStartAndEnd().IsEmpty();
	}

	namespace
	{
		const FString CompositionSchema =
			TEXT("uai.authoring.target_composition/1");
		const FString ManifestPath =
			TEXT(".uai/workspace.hjson");

		struct FUnitDraft
		{
			FString UnitId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::PropertySet;
			FString OwnerRef;
			FString SemanticTypeRef;
			FString Role;
			FString FieldSetRef;
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			TArray<FString> ItemRefs;
			TArray<FString> RelationRefs;
			FString OrderingRef;
			FString LayoutRef;
			TArray<FString> DependencyRefs;
			TArray<FString> ProfileVisibility{
				TEXT("state"),
				TEXT("seed"),
				TEXT("edit_bundle")};
			FString SourceProvenance;
			FString TargetIdentityKey;
		};

		struct FEntryRoute
		{
			FString SurfaceId;
			ESemanticUnitKind UnitKind =
				ESemanticUnitKind::PropertySet;
			bool bTargetValues = false;
			bool bGraph = false;
			bool bItem = false;
			bool bProperty = false;
			bool bRelation = false;
			bool bDocument = false;
		};

		struct FCompositionIdentityView
		{
			FString CanonicalKey;
			TMap<FString, FString> Selectors;

			FString GetSelector(const FString& Name) const
			{
				const FString* Found = Selectors.Find(Name);
				return Found ? *Found : FString();
			}
		};

		struct FCompositionEntryView
		{
			FString SemanticId;
			FCompositionIdentityView TargetIdentity;
			FUeAgentCanonicalValuePtr Value;
			bool bAvailable = true;
			FString Provenance;
		};

		struct FCompositionInput
		{
			FString TargetKind;
			FString ProfileId;
			FString BaseRevision;
			FString SourceProvenance;
			TArray<FCompositionEntryView> Entries;
		};

		struct FDocumentVisit
		{
			FUeAgentCanonicalValuePtr Value;
			FString Path;
			FString RoleHint;
			FString OwnerId;
			FString SurfaceId;
			int32 SourceIndex = INDEX_NONE;
		};

		struct FRelationAggregate
		{
			FString OwnerRef;
			FString SurfaceId;
			TMap<
				FString,
				TMap<FString, FUeAgentCanonicalValueRef>> Groups;
		};

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256(Bytes);
		}

		FString StableId(
			const FString& Prefix,
			const FString& Identity)
		{
			return Prefix + TEXT(":") + Sha256Text(Identity).Right(24);
		}

		bool SurfaceEquals(
			const FCompositionSurfaceContract& Left,
			const FCompositionSurfaceContract& Right)
		{
			return Left.SurfaceId == Right.SurfaceId
				&& Left.SurfaceKind == Right.SurfaceKind
				&& Left.UnitKind == Right.UnitKind;
		}

		bool ContractEquals(
			const FTargetCompositionContract& Left,
			const FTargetCompositionContract& Right)
		{
			if (Left.SchemaId != Right.SchemaId
				|| Left.TargetKind != Right.TargetKind
				|| Left.Surfaces.Num() != Right.Surfaces.Num()
				|| Left.SemanticRoutes.Num() != Right.SemanticRoutes.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < Left.Surfaces.Num(); ++Index)
			{
				if (!SurfaceEquals(
					Left.Surfaces[Index],
					Right.Surfaces[Index]))
				{
					return false;
				}
			}
			for (int32 Index = 0; Index < Left.SemanticRoutes.Num(); ++Index)
			{
				const FCompositionSemanticRouteContract& LeftRoute =
					Left.SemanticRoutes[Index];
				const FCompositionSemanticRouteContract& RightRoute =
					Right.SemanticRoutes[Index];
				if (LeftRoute.Match != RightRoute.Match
					|| LeftRoute.Pattern != RightRoute.Pattern
					|| LeftRoute.SurfaceId != RightRoute.SurfaceId
					|| LeftRoute.UnitKind != RightRoute.UnitKind
					|| LeftRoute.Role != RightRoute.Role
					|| LeftRoute.Priority != RightRoute.Priority)
				{
					return false;
				}
			}
			return true;
		}

		FCompositionSurfaceContract Surface(
			const TCHAR* Id,
			EAuthoringSurfaceKind Kind,
			ESemanticUnitKind UnitKind)
		{
			FCompositionSurfaceContract Result;
			Result.SurfaceId = Id;
			Result.SurfaceKind = Kind;
			Result.UnitKind = UnitKind;
			return Result;
		}

		FTargetCompositionContract Contract(
			const TCHAR* TargetKind,
			TArray<FCompositionSurfaceContract> Surfaces)
		{
			FTargetCompositionContract Result;
			Result.SchemaId = CompositionSchema;
			Result.TargetKind = TargetKind;
			Result.Surfaces = MoveTemp(Surfaces);
			return Result;
		}

		bool TryCanonicalizeJson(
			const TSharedPtr<FJsonValue>& Root,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			OutValue.Reset();
			if (!Root.IsValid())
			{
				OutError.Set(TEXT("composition_json_value_invalid"));
				return false;
			}
			struct FVisit
			{
				TSharedPtr<FJsonValue> Value;
				bool bExpanded = false;
			};
			TArray<FVisit> Stack;
			TArray<TSharedPtr<FJsonValue>> Order;
			TSet<const FJsonValue*> Expanded;
			Stack.Add({Root, false});
			while (!Stack.IsEmpty())
			{
				FVisit Visit = Stack.Pop(EAllowShrinking::No);
				if (!Visit.Value.IsValid())
				{
					OutError.Set(TEXT("composition_json_value_invalid"));
					return false;
				}
				const FJsonValue* Key = Visit.Value.Get();
				if (Visit.bExpanded)
				{
					Order.Add(Visit.Value);
					continue;
				}
				if (Expanded.Contains(Key))
				{
					continue;
				}
				Expanded.Add(Key);
				Stack.Add({Visit.Value, true});
				if (Visit.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items =
						Visit.Value->AsArray();
					for (int32 Index = Items.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add({Items[Index], false});
					}
				}
				else if (Visit.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> Object =
						Visit.Value->AsObject();
					if (!Object.IsValid())
					{
						OutError.Set(
							TEXT("composition_json_object_invalid"));
						return false;
					}
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					for (int32 Index = Keys.Num() - 1;
						Index >= 0;
						--Index)
					{
						Stack.Add({
							Object->Values.FindChecked(Keys[Index]),
							false});
					}
				}
			}

			TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
			for (const TSharedPtr<FJsonValue>& Value : Order)
			{
				FUeAgentCanonicalValuePtr Canonical;
				FUeAgentStateSemanticError SemanticError;
				bool bBuilt = false;
				switch (Value->Type)
				{
				case EJson::Null:
				case EJson::None:
					bBuilt = FUeAgentCanonicalValue::TryNull(
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::Boolean:
					bBuilt = FUeAgentCanonicalValue::TryBoolean(
						Value->AsBool(),
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::String:
					bBuilt = FUeAgentCanonicalValue::TryString(
						Value->AsString(),
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::Number:
					bBuilt = FUeAgentCanonicalValue::TryDecimal(
						FString::SanitizeFloat(Value->AsNumber()),
						Resources,
						Canonical,
						SemanticError);
					break;
				case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: Value->AsArray())
					{
						const FUeAgentCanonicalValuePtr* Found =
							Built.Find(Item.Get());
						if (!Found || !Found->IsValid())
						{
							OutError.Set(
								TEXT(
									"composition_json_child_missing"));
							return false;
						}
						Items.Add(Found->ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryArray(
						Items,
						Resources,
						Canonical,
						SemanticError);
					break;
				}
				case EJson::Object:
				{
					const TSharedPtr<FJsonObject> Object =
						Value->AsObject();
					TArray<FString> Keys;
					Object->Values.GetKeys(Keys);
					Keys.Sort();
					TArray<TPair<
						FString,
						FUeAgentCanonicalValueRef>> Items;
					for (const FString& ChildKey : Keys)
					{
						const TSharedPtr<FJsonValue>& Child =
							Object->Values.FindChecked(ChildKey);
						const FUeAgentCanonicalValuePtr* Found =
							Built.Find(Child.Get());
						if (!Found || !Found->IsValid())
						{
							OutError.Set(
								TEXT(
									"composition_json_child_missing"));
							return false;
						}
						Items.Emplace(
							ChildKey,
							Found->ToSharedRef());
					}
					bBuilt = FUeAgentCanonicalValue::TryObject(
						Items,
						Resources,
						Canonical,
						SemanticError);
					break;
				}
				default:
					break;
				}
				if (!bBuilt || !Canonical.IsValid())
				{
					OutError.Set(
						SemanticError.Code.IsEmpty()
							? TEXT("composition_canonical_value_failed")
							: SemanticError.Code,
						SemanticError.Path);
					return false;
				}
				Built.Add(Value.Get(), Canonical);
			}
			const FUeAgentCanonicalValuePtr* Result =
				Built.Find(Root.Get());
			if (!Result || !Result->IsValid())
			{
				OutError.Set(TEXT("composition_canonical_root_missing"));
				return false;
			}
			OutValue = *Result;
			return true;
		}

		bool TryBuildValues(
			const TSharedPtr<FJsonValue>& Value,
			const TSet<FString>& Excluded,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid())
				{
					OutError.Set(TEXT("composition_json_object_invalid"));
					return false;
				}
				TArray<FString> Keys;
				Object->Values.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					if (Excluded.Contains(Key))
					{
						continue;
					}
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryCanonicalizeJson(
						Object->Values.FindChecked(Key),
						Resources,
						Canonical,
						OutError))
					{
						return false;
					}
					OutValues.Add(Key, Canonical.ToSharedRef());
				}
				return true;
			}
			FUeAgentCanonicalValuePtr Canonical;
			if (!TryCanonicalizeJson(
				Value,
				Resources,
				Canonical,
				OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("value"), Canonical.ToSharedRef());
			return true;
		}

		bool TryBuildValues(
			const FUeAgentCanonicalValuePtr& Value,
			const TSet<FString>& Excluded,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->GetKind() == EUeAgentValueKind::Object)
			{
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
					: Value->GetObjectItems())
				{
					if (!Excluded.Contains(Field.Key))
					{
						OutValues.Add(Field.Key, Field.Value);
					}
				}
				return true;
			}
			OutValues.Add(TEXT("value"), Value.ToSharedRef());
			return true;
		}

		bool TryStringArrayValue(
			const TArray<FString>& Values,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FString& Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError SemanticError;
				if (!FUeAgentCanonicalValue::TryString(
					Value,
					Resources,
					Item,
					SemanticError))
				{
					OutError.Set(SemanticError.Code, SemanticError.Path);
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryArray(
				Items,
				Resources,
				OutValue,
				SemanticError))
			{
				OutError.Set(SemanticError.Code, SemanticError.Path);
				return false;
			}
			return true;
		}

		FString Selector(
			const FCompositionEntryView& Entry,
			const TCHAR* Name)
		{
			return Entry.TargetIdentity.GetSelector(Name);
		}

		FString GraphSelectorKey(
			const FCompositionEntryView& Entry)
		{
			const FString Asset = Selector(Entry, TEXT("asset_path"));
			FString Graph = Selector(Entry, TEXT("graph_guid"));
			if (Graph.IsEmpty())
			{
				Graph = Selector(Entry, TEXT("graph_name"));
			}
			if (Graph.IsEmpty())
			{
				const FString Usage =
					Selector(Entry, TEXT("usage"));
				const FString UsageId =
					Selector(Entry, TEXT("usage_id"));
				if (!Usage.IsEmpty() || !UsageId.IsEmpty())
				{
					Graph = TEXT("stage=") + Usage
						+ TEXT("@") + UsageId;
				}
			}
			return Asset + TEXT("|") + Graph;
		}

		FString ItemSelectorKey(
			const FCompositionEntryView& Entry)
		{
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString OwnerActorGuid =
				Selector(Entry, TEXT("owner_actor_guid"));
			const FString OwnerManagedId =
				Selector(Entry, TEXT("owner_managed_id"));
			const FString ComponentName =
				Selector(Entry, TEXT("component_name"));
			if (!ComponentName.IsEmpty()
				&& (!OwnerActorGuid.IsEmpty() || !OwnerManagedId.IsEmpty()))
			{
				return Scope
					+ (!OwnerActorGuid.IsEmpty()
						? TEXT("|owner_actor_guid=") + OwnerActorGuid
						: TEXT("|owner_managed_id=") + OwnerManagedId)
					+ TEXT("|component_name=") + ComponentName;
			}
			for (const TCHAR* Name : {
				TEXT("dynamic_input_guid"),
				TEXT("input_name"),
				TEXT("node_guid"),
				TEXT("node_id"),
				TEXT("expression_guid"),
				TEXT("component_guid"),
				TEXT("component_name"),
				TEXT("widget_name"),
				TEXT("member_guid"),
				TEXT("member_name"),
				TEXT("renderer_name"),
				TEXT("script_usage_id"),
				TEXT("module_guid"),
				TEXT("usage_id"),
				TEXT("emitter_handle_id"),
				TEXT("emitter_name"),
				TEXT("actor_guid"),
				TEXT("managed_id"),
				TEXT("managed_actor_id")})
			{
				const FString Value = Selector(Entry, Name);
				if (!Value.IsEmpty())
				{
					return Scope + TEXT("|") + Name
						+ TEXT("=") + Value;
				}
			}
			return Entry.TargetIdentity.CanonicalKey;
		}

		bool IsTargetSemantic(const FString& SemanticId)
		{
			return SemanticId.Contains(TEXT(".export.target"))
					|| SemanticId.Contains(TEXT(".export.function_target"));
		}

		bool TryResolveDeclaredSemanticRoute(
			const FString& TargetKind,
			const FString& SemanticId,
			FEntryRoute& OutRoute)
		{
			FCompositionSemanticRouteContract Selected;
			if (!TryResolveTargetCompositionSemanticRoute(
				TargetKind,
				SemanticId,
				Selected))
			{
				return false;
			}
			OutRoute = {};
			OutRoute.SurfaceId = Selected.SurfaceId;
			OutRoute.UnitKind = Selected.UnitKind;
			switch (Selected.Role)
			{
			case ECompositionSemanticRouteRole::TargetValues:
				OutRoute.bTargetValues = true;
				break;
			case ECompositionSemanticRouteRole::Graph:
				OutRoute.bGraph = true;
				break;
			case ECompositionSemanticRouteRole::Item:
				OutRoute.bItem = true;
				break;
			case ECompositionSemanticRouteRole::Property:
				OutRoute.bProperty = true;
				break;
			case ECompositionSemanticRouteRole::Relation:
				OutRoute.bRelation = true;
				break;
			case ECompositionSemanticRouteRole::Document:
				OutRoute.bDocument = true;
				break;
			default:
				return false;
			}
			return true;
		}

		FEntryRoute RouteEntry(
			const FString& TargetKind,
			const FString& SemanticId)
		{
			FEntryRoute Route;
			if (SemanticId.StartsWith(TEXT("content_graph.summary:")))
			{
				return Route;
			}
			if (IsTargetSemantic(SemanticId))
			{
				Route.SurfaceId = TEXT("target_settings");
				Route.UnitKind = ESemanticUnitKind::TargetState;
				Route.bTargetValues = true;
				return Route;
			}

			if (SemanticId.EndsWith(TEXT(".document"))
				|| SemanticId.EndsWith(TEXT(".profile_state"))
				|| SemanticId.EndsWith(TEXT(".seed")))
			{
				Route.bDocument = true;
				return Route;
			}

			if (TryResolveDeclaredSemanticRoute(
				TargetKind,
				SemanticId,
				Route))
			{
				return Route;
			}

			if (TargetKind.StartsWith(TEXT("ue.niagara_")))
			{
				if (SemanticId == TEXT("uai.semantic.niagara.asset")
					|| SemanticId == TEXT("uai.semantic.niagara.system")
					|| SemanticId == TEXT("uai.semantic.niagara.emitter")
					|| SemanticId == TEXT("uai.semantic.niagara.script"))
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::TargetState;
					Route.bTargetValues = true;
				}
				else if (SemanticId.EndsWith(TEXT(".stack_entry")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.EndsWith(TEXT(".module")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".stack_order")))
				{
					Route.SurfaceId = TEXT("stack_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else if (SemanticId.EndsWith(TEXT(".dynamic_input")))
				{
					Route.SurfaceId = TEXT("dynamic_input_tree");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".emitter_owner"))
					|| SemanticId.EndsWith(TEXT(".renderer"))
					|| SemanticId.EndsWith(TEXT(".event_handler")))
				{
					Route.SurfaceId = TEXT("emitter_hierarchy");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".emitter_property"))
					|| SemanticId.EndsWith(TEXT(".module_property")))
				{
					Route.SurfaceId = TEXT("module_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.level.asset"))
			{
				if (SemanticId.EndsWith(TEXT(".world.target")))
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::TargetState;
					Route.bTargetValues = true;
				}
				else if (SemanticId.EndsWith(TEXT(".actor.target"))
					|| SemanticId.EndsWith(TEXT(".component.target")))
				{
					Route.SurfaceId =
						TEXT("actor_component_hierarchy");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".actor"))
					|| SemanticId.EndsWith(TEXT(".component"))
					|| SemanticId.EndsWith(TEXT(".property")))
				{
					Route.SurfaceId =
						TEXT("actor_component_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.blueprint.asset")
				|| TargetKind == TEXT("ue.anim_blueprint.asset"))
			{
				if (SemanticId.Contains(TEXT(".class_default.property")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("class_defaults")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".interface")))
				{
					Route.SurfaceId = TEXT("member_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".member.")))
				{
					Route.SurfaceId = TEXT("member_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".component.property")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("component_tree")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".component")))
				{
					Route.SurfaceId =
						TargetKind == TEXT("ue.blueprint.asset")
							? TEXT("component_tree")
							: TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".graph")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.Contains(TEXT(".node.property"))
					|| SemanticId.Contains(TEXT(".pin_default")))
				{
					Route.SurfaceId = TEXT("node_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".node")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".edge")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.widget_blueprint.asset"))
			{
				if (SemanticId.EndsWith(TEXT(".widget_tree")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::StructureState;
					Route.bGraph = true;
				}
				else if (SemanticId.EndsWith(TEXT(".widget")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.Contains(TEXT(".widget.property"))
					|| SemanticId.Contains(TEXT(".slot.property")))
				{
					Route.SurfaceId = TEXT("widget_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".slot"))
					|| SemanticId.EndsWith(TEXT(".binding")))
				{
					Route.SurfaceId = TEXT("widget_tree");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else if (SemanticId.EndsWith(TEXT(".animation")))
				{
					Route.SurfaceId = TEXT("animation_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else
				{
					return RouteEntry(
						TEXT("ue.blueprint.asset"),
						SemanticId);
				}
				return Route;
			}

			if (TargetKind.StartsWith(TEXT("ue.material")))
			{
				if (SemanticId.Contains(TEXT("expression_property")))
				{
					Route.SurfaceId = TEXT("expression_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".expression")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".edge"))
					|| SemanticId.EndsWith(TEXT(".root_input")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else if (SemanticId.Contains(TEXT("parameter")))
				{
					Route.SurfaceId = TEXT("parameter_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			if (TargetKind == TEXT("ue.pcg_graph.asset"))
			{
				if (SemanticId.Contains(TEXT("node_property")))
				{
					Route.SurfaceId = TEXT("node_properties");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				else if (SemanticId.EndsWith(TEXT(".node"))
					|| SemanticId.EndsWith(TEXT(".pin")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::ItemState;
					Route.bItem = true;
				}
				else if (SemanticId.EndsWith(TEXT(".edge")))
				{
					Route.SurfaceId = TEXT("graph_collection");
					Route.UnitKind = ESemanticUnitKind::RelationSet;
					Route.bRelation = true;
				}
				else
				{
					Route.SurfaceId = TEXT("target_settings");
					Route.UnitKind = ESemanticUnitKind::PropertySet;
					Route.bProperty = true;
				}
				return Route;
			}

			Route.SurfaceId = TEXT("target_settings");
			Route.UnitKind = ESemanticUnitKind::PropertySet;
			Route.bProperty = true;
			return Route;
		}

		FUeAgentCanonicalValuePtr FindLayoutValue(
			const FCompositionEntryView& Entry)
		{
			if (!Entry.Value.IsValid()
				|| Entry.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Entry.Value->GetObjectItems())
			{
				if (Field.Key == TEXT("pos")
					|| Field.Key == TEXT("position")
					|| Field.Key == TEXT("editor_position"))
				{
					return FUeAgentCanonicalValuePtr(Field.Value);
				}
			}
			return nullptr;
		}

		FString FindGraphOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& GraphUnits,
			const FString& Fallback)
		{
			const FString Key = GraphSelectorKey(Entry);
			if (const FString* Found = GraphUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const FString& Fallback)
		{
			const FString Key = ItemSelectorKey(Entry);
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwnerBySelector(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const TCHAR* SelectorName,
			const FString& Fallback)
		{
			const FString SelectorValue =
				Selector(Entry, SelectorName);
			if (SelectorValue.IsEmpty())
			{
				return Fallback;
			}
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString Key = Scope
				+ TEXT("|") + SelectorName + TEXT("=")
				+ SelectorValue;
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindItemOwnerByAliasedSelector(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& ItemUnits,
			const TCHAR* SourceSelectorName,
			const TCHAR* OwnerSelectorName,
			const FString& Fallback)
		{
			const FString SelectorValue =
				Selector(Entry, SourceSelectorName);
			if (SelectorValue.IsEmpty())
			{
				return Fallback;
			}
			FString Scope = Selector(Entry, TEXT("asset_path"));
			if (Scope.IsEmpty())
			{
				Scope = Selector(Entry, TEXT("world_path"));
			}
			const FString Key = Scope + TEXT("|") + OwnerSelectorName
				+ TEXT("=") + SelectorValue;
			if (const FString* Found = ItemUnits.Find(Key))
			{
				return *Found;
			}
			return Fallback;
		}

		FString FindDeclaredItemOwner(
			const FCompositionEntryView& Entry,
			const TMap<FString, FString>& SourceItemUnits)
		{
			const FString OwnerItemId =
				Selector(Entry, TEXT("owner_item_id"));
			if (OwnerItemId.IsEmpty())
			{
				return FString();
			}
			const FString Key =
				Selector(Entry, TEXT("asset_path"))
				+ TEXT("|item_id=") + OwnerItemId;
			const FString* Found = SourceItemUnits.Find(Key);
			return Found ? *Found : FString();
		}

		FString NormalizeRoleToken(FString Value)
		{
			Value.ToLowerInline();
			Value.ReplaceInline(TEXT("-"), TEXT("_"));
			Value.ReplaceInline(TEXT(" "), TEXT("_"));
			if (Value.EndsWith(TEXT("ies")))
			{
				Value.LeftChopInline(3);
				Value += TEXT("y");
			}
			else if (Value.EndsWith(TEXT("s"))
				&& !Value.EndsWith(TEXT("ss")))
			{
				Value.LeftChopInline(1);
			}
			return Value;
		}

		FString ChooseDocumentSurface(
			const FTargetCompositionContract& CompositionContract,
			const FString& RootRole,
			const bool bStructured)
		{
			const FString Role = NormalizeRoleToken(RootRole);
			const auto HasAny =
				[&Role](std::initializer_list<const TCHAR*> Tokens)
				{
					for (const TCHAR* Token : Tokens)
					{
						if (Role.Contains(Token))
						{
							return true;
						}
					}
					return false;
				};
			const EAuthoringSurfaceKind PreferredKind =
				HasAny({TEXT("movie_scene"), TEXT("timeline"),
					TEXT("track"), TEXT("section"), TEXT("key"),
					TEXT("curve")})
					? EAuthoringSurfaceKind::Timeline
				: HasAny({TEXT("hierarchy"), TEXT("element"),
					TEXT("bone"), TEXT("goal"), TEXT("chain"),
					TEXT("actor"), TEXT("component")})
					? EAuthoringSurfaceKind::Hierarchy
				: HasAny({TEXT("graph"), TEXT("node"),
					TEXT("edge"), TEXT("link")})
					? EAuthoringSurfaceKind::Structure
				: HasAny({TEXT("payload"), TEXT("source"),
					TEXT("import")})
					? EAuthoringSurfaceKind::ExternalPayload
					: EAuthoringSurfaceKind::Items;

			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				const FString SurfaceRole =
					NormalizeRoleToken(SurfaceContract.SurfaceId);
				if (SurfaceRole.Contains(Role)
					|| (!Role.IsEmpty()
						&& Role.Contains(SurfaceRole)))
				{
					return SurfaceContract.SurfaceId;
				}
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind == PreferredKind)
				{
					return SurfaceContract.SurfaceId;
				}
			}
			if (!bStructured)
			{
				return TEXT("target_settings");
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind
						== EAuthoringSurfaceKind::Properties
					&& SurfaceContract.SurfaceId
						!= TEXT("target_settings"))
				{
					return SurfaceContract.SurfaceId;
				}
			}
			for (const FCompositionSurfaceContract& SurfaceContract
				: CompositionContract.Surfaces)
			{
				if (SurfaceContract.SurfaceKind
					!= EAuthoringSurfaceKind::Properties)
				{
					return SurfaceContract.SurfaceId;
				}
			}
			return TEXT("target_settings");
		}

		bool IsRelationDocumentObject(
			const FString& RoleHint,
			const FUeAgentCanonicalValuePtr& Object)
		{
			if (!Object.IsValid()
				|| Object->GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			const FString Role = NormalizeRoleToken(RoleHint);
			const bool bRelationRole =
				Role.Contains(TEXT("edge"))
				|| Role.Contains(TEXT("link"))
				|| Role.Contains(TEXT("relation"));
			bool bFromNode = false;
			bool bToNode = false;
			bool bSource = false;
			bool bTarget = false;
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Object->GetObjectItems())
			{
				bFromNode |= Field.Key == TEXT("from_node");
				bToNode |= Field.Key == TEXT("to_node");
				bSource |= Field.Key == TEXT("source");
				bTarget |= Field.Key == TEXT("target");
			}
			return bRelationRole
				|| (bFromNode && bToNode)
				|| (bSource && bTarget);
		}

		bool IsStructuredValue(const FUeAgentCanonicalValueRef& Value)
		{
			return Value->GetKind() == EUeAgentValueKind::Object
				|| Value->GetKind() == EUeAgentValueKind::Array;
		}

		bool IsScalarCollection(const FUeAgentCanonicalValueRef& Value)
		{
			if (Value->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				if (IsStructuredValue(Item))
				{
					return false;
				}
			}
			return true;
		}

		const FUeAgentCanonicalValueRef* FindObjectField(
			const FUeAgentCanonicalValueRef& Object,
			const FString& Name)
		{
			if (Object->GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Object->GetObjectItems())
			{
				if (Field.Key == Name)
				{
					return &Field.Value;
				}
			}
			return nullptr;
		}

		void AddOwnedReference(
			FUnitDraft& Owner,
			const FUnitDraft& Child)
		{
			switch (Child.UnitKind)
			{
			case ESemanticUnitKind::StructureState:
			case ESemanticUnitKind::ItemState:
			case ESemanticUnitKind::CollectionState:
			case ESemanticUnitKind::EntityState:
				Owner.ItemRefs.AddUnique(Child.UnitId);
				break;
			case ESemanticUnitKind::RelationSet:
				Owner.RelationRefs.AddUnique(Child.UnitId);
				break;
			case ESemanticUnitKind::LayoutState:
				Owner.LayoutRef = Child.UnitId;
				break;
			case ESemanticUnitKind::DependencyState:
				Owner.DependencyRefs.AddUnique(Child.UnitId);
				break;
			default:
				break;
			}
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		bool SerializeJson(
			const TSharedRef<FJsonObject>& Object,
			TArray<uint8>& OutBytes)
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer =
					TJsonWriterFactory<
						TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			if (!FJsonSerializer::Serialize(Object, Writer))
			{
				return false;
			}
			Text.AppendChar(TEXT('\n'));
			const FTCHARToUTF8 Utf8(*Text);
			OutBytes.Reset(Utf8.Length());
			OutBytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return true;
		}

		int64 EstimatedTokens(const TArray<FExportFilePlan>& Files)
		{
			int64 Bytes = 0;
			for (const FExportFilePlan& File : Files)
			{
				Bytes += File.GetCanonicalUtf8().Num();
			}
			return (Bytes + 3) / 4;
		}

		bool RegisterOne(
			FTargetCompositionContract Value,
			FString& OutError)
		{
			return FTargetCompositionRegistry::Get().RegisterContract(
				MoveTemp(Value),
				OutError);
		}
	}

	FTargetCompositionRegistry&
	FTargetCompositionRegistry::Get()
	{
		static FTargetCompositionRegistry Registry;
		return Registry;
	}

	bool FTargetCompositionRegistry::RegisterContract(
		FTargetCompositionContract ContractValue,
		FString& OutError)
	{
		return RegisterContracts(
			{MoveTemp(ContractValue)},
			OutError);
	}

	bool FTargetCompositionRegistry::RegisterContracts(
		TArray<FTargetCompositionContract> InContracts,
		FString& OutError)
	{
		OutError.Reset();
		TMap<FString, FTargetCompositionContract> Pending;
		for (FTargetCompositionContract& ContractValue : InContracts)
		{
			if (!ValidateTargetCompositionContract(
				ContractValue,
				OutError))
			{
				return false;
			}
			if (const FTargetCompositionContract* ExistingPending =
				Pending.Find(ContractValue.TargetKind))
			{
				if (!ContractEquals(*ExistingPending, ContractValue))
				{
					OutError = TEXT("target_composition_contract_conflict:")
						+ ContractValue.TargetKind;
					return false;
				}
				continue;
			}
			Pending.Add(
				ContractValue.TargetKind,
				MoveTemp(ContractValue));
		}
		FWriteScopeLock Lock(RegistryLock);
		for (const TPair<FString, FTargetCompositionContract>& Pair : Pending)
		{
			if (const FTargetCompositionContract* Existing =
				Contracts.Find(Pair.Key))
			{
				if (!ContractEquals(*Existing, Pair.Value))
				{
					OutError = TEXT("target_composition_contract_conflict:")
						+ Pair.Key;
					return false;
				}
			}
		}
		for (TPair<FString, FTargetCompositionContract>& Pair : Pending)
		{
			if (!Contracts.Contains(Pair.Key))
			{
				Contracts.Add(Pair.Key, MoveTemp(Pair.Value));
			}
		}
		return true;
	}

	bool FTargetCompositionRegistry::FindContract(
		const FString& TargetKind,
		FTargetCompositionContract& OutContract) const
	{
		FReadScopeLock Lock(RegistryLock);
		const FTargetCompositionContract* Found =
			Contracts.Find(Normalize(TargetKind));
		if (!Found)
		{
			return false;
		}
		OutContract = *Found;
		return true;
	}

	TArray<FTargetCompositionContract>
	FTargetCompositionRegistry::ListContracts() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FTargetCompositionContract> Result;
		Contracts.GenerateValueArray(Result);
		Result.Sort(
			[](const FTargetCompositionContract& Left,
				const FTargetCompositionContract& Right)
			{
				return Left.TargetKind < Right.TargetKind;
			});
		return Result;
	}

	FString GetTargetCompositionContractSchemaId()
	{
		return CompositionSchema;
	}

	bool TryResolveTargetCompositionSemanticRoute(
		const FString& TargetKind,
		const FString& SemanticId,
		FCompositionSemanticRouteContract& OutRoute)
	{
		OutRoute = {};
		FTargetCompositionContract ContractValue;
		if (!FTargetCompositionRegistry::Get().FindContract(
				TargetKind,
				ContractValue)
			|| ContractValue.SemanticRoutes.IsEmpty())
		{
			return false;
		}
		const FCompositionSemanticRouteContract* Selected = nullptr;
		for (const FCompositionSemanticRouteContract& Candidate :
			ContractValue.SemanticRoutes)
		{
			bool bMatches = false;
			switch (Candidate.Match)
			{
			case ECompositionSemanticRouteMatch::Exact:
				bMatches = SemanticId == Candidate.Pattern;
				break;
			case ECompositionSemanticRouteMatch::Suffix:
				bMatches = SemanticId.EndsWith(Candidate.Pattern);
				break;
			case ECompositionSemanticRouteMatch::Contains:
				bMatches = SemanticId.Contains(Candidate.Pattern);
				break;
			case ECompositionSemanticRouteMatch::Default:
				bMatches = true;
				break;
			default:
				break;
			}
			if (bMatches
				&& (!Selected || Candidate.Priority > Selected->Priority))
			{
				Selected = &Candidate;
			}
		}
		if (!Selected)
		{
			return false;
		}
		OutRoute = *Selected;
		return true;
	}

	bool ValidateTargetCompositionContract(
		const FTargetCompositionContract& ContractValue,
		FString& OutError)
	{
		OutError.Reset();
		if (Normalize(ContractValue.SchemaId) != CompositionSchema
			|| Normalize(ContractValue.TargetKind).IsEmpty()
			|| ContractValue.Surfaces.IsEmpty())
		{
			OutError = TEXT("target_composition_contract_invalid");
			return false;
		}
		TSet<FString> SurfaceIds;
		bool bHasProperties = false;
		bool bHasCompound = false;
		for (const FCompositionSurfaceContract& Item
			: ContractValue.Surfaces)
		{
			const FString SurfaceId = Normalize(Item.SurfaceId);
			if (SurfaceId.IsEmpty()
				|| SurfaceIds.Contains(SurfaceId)
				|| Item.UnitKind
					== ESemanticUnitKind::OpaqueExtension)
			{
				OutError =
					TEXT("target_composition_surface_invalid:")
					+ SurfaceId;
				return false;
			}
			SurfaceIds.Add(SurfaceId);
			bHasProperties |= Item.SurfaceKind
				== EAuthoringSurfaceKind::Properties;
			bHasCompound |= Item.SurfaceKind
				!= EAuthoringSurfaceKind::Properties;
		}
		if (!SurfaceIds.Contains(TEXT("target_settings"))
			|| !bHasProperties
			|| !bHasCompound)
		{
			OutError =
				TEXT("target_composition_surface_coverage_invalid");
			return false;
		}
		TSet<int32> Priorities;
		int32 DefaultCount = 0;
		int32 DefaultPriority = MAX_int32;
		int32 LowestSpecificPriority = MAX_int32;
		for (const FCompositionSemanticRouteContract& Route :
			ContractValue.SemanticRoutes)
		{
			const FString SurfaceId = Normalize(Route.SurfaceId);
			const FString Pattern = Normalize(Route.Pattern);
			if (!SurfaceIds.Contains(SurfaceId))
			{
				OutError =
					TEXT("target_composition_semantic_route_surface_missing:")
					+ SurfaceId;
				return false;
			}
			if (Route.Role == ECompositionSemanticRouteRole::Undeclared
				|| Route.UnitKind == ESemanticUnitKind::OpaqueExtension)
			{
				OutError = TEXT("target_composition_semantic_route_invalid:")
					+ SurfaceId;
				return false;
			}
			if (Priorities.Contains(Route.Priority))
			{
				OutError =
					TEXT("target_composition_semantic_route_priority_duplicate:")
					+ FString::FromInt(Route.Priority);
				return false;
			}
			Priorities.Add(Route.Priority);
			if (Route.Match == ECompositionSemanticRouteMatch::Default)
			{
				if (!Pattern.IsEmpty())
				{
					OutError = TEXT("target_composition_semantic_route_default_pattern_invalid");
					return false;
				}
				++DefaultCount;
				DefaultPriority = Route.Priority;
			}
			else
			{
				if (Pattern.IsEmpty())
				{
					OutError = TEXT("target_composition_semantic_route_pattern_missing");
					return false;
				}
				LowestSpecificPriority = FMath::Min(
					LowestSpecificPriority,
					Route.Priority);
			}
		}
		if (!ContractValue.SemanticRoutes.IsEmpty()
			&& (DefaultCount != 1
				|| (LowestSpecificPriority != MAX_int32
					&& DefaultPriority >= LowestSpecificPriority)))
		{
			OutError = TEXT("target_composition_semantic_route_default_invalid");
			return false;
		}
		return true;
	}

	bool RegisterGraphTreeTargetCompositionContracts(
		FString& OutError)
	{
		const TArray<FCompositionSurfaceContract> Blueprint{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("class_defaults"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("component_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("timeline_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Widget{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("widget_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("animation_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("widget_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> AnimBlueprint{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("member_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Material{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("parameter_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("expression_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Pcg{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("graph_collection"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("node_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		return RegisterOne(
				Contract(TEXT("ue.blueprint.asset"), Blueprint),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.widget_blueprint.asset"), Widget),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.anim_blueprint.asset"),
					AnimBlueprint),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.material.asset"), Material),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.material_function.asset"),
					Material),
				OutError)
			&& RegisterOne(
				Contract(
					TEXT("ue.material_instance.asset"),
					Material),
				OutError)
			&& RegisterOne(
				Contract(TEXT("ue.pcg_graph.asset"), Pcg),
				OutError);
	}

	bool RegisterOtherTargetCompositionContracts(
		FString& OutError)
	{
		const TArray<FCompositionSurfaceContract> Generic{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("owned_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> Niagara{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("emitter_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("stack_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("dynamic_input_tree"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("module_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Timeline{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("timeline_collection"),
				EAuthoringSurfaceKind::Timeline,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> Level{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("actor_component_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("actor_component_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Rig{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("rig_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
			Surface(
				TEXT("rig_hierarchy"),
				EAuthoringSurfaceKind::Hierarchy,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("rigvm_graph"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("hierarchy_item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Behavior{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("behavior_tree"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Audio{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("audio_graph"),
				EAuthoringSurfaceKind::Structure,
				ESemanticUnitKind::StructureState),
			Surface(
				TEXT("item_properties"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
		};
		const TArray<FCompositionSurfaceContract> Table{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("table_collection"),
				EAuthoringSurfaceKind::Collection,
				ESemanticUnitKind::CollectionState),
		};
		const TArray<FCompositionSurfaceContract> External{
			Surface(
				TEXT("target_settings"),
				EAuthoringSurfaceKind::Properties,
				ESemanticUnitKind::PropertySet),
			Surface(
				TEXT("source_payload"),
				EAuthoringSurfaceKind::ExternalPayload,
				ESemanticUnitKind::ExternalPayloadState),
		};
		const TArray<TPair<FString, const TArray<FCompositionSurfaceContract>*>>
			Registrations{
				{TEXT("ue.generic.object"), &Generic},
				{TEXT("ue.niagara_system.asset"), &Niagara},
				{TEXT("ue.niagara_emitter.asset"), &Niagara},
				{TEXT("ue.niagara_script.asset"), &Niagara},
				{TEXT("ue.level_sequence.asset"), &Timeline},
				{TEXT("ue.animation_sequence.asset"), &Timeline},
				{TEXT("ue.curve.asset"), &Timeline},
				{TEXT("ue.level.asset"), &Level},
				{TEXT("ue.control_rig.asset"), &Rig},
				{TEXT("ue.ik_rig.asset"), &Rig},
				{TEXT("ue.ik_retargeter.asset"), &Rig},
				{TEXT("ue.skeleton.asset"), &Rig},
				{TEXT("ue.behavior_tree.asset"), &Behavior},
				{TEXT("ue.metasound.asset"), &Audio},
				{TEXT("ue.sound_cue.asset"), &Audio},
				{TEXT("ue.sound_submix.asset"), &Audio},
				{TEXT("ue.data_table.asset"), &Table},
				{TEXT("ue.imported_texture.asset"), &External},
				{TEXT("ue.imported_mesh.asset"), &External},
				{TEXT("ue.imported_audio.asset"), &External},
				{TEXT("ue.imported_csv.asset"), &External},
			};
		for (const TPair<
			FString,
			const TArray<FCompositionSurfaceContract>*>& Registration
			: Registrations)
		{
			if (!RegisterOne(
				Contract(
					*Registration.Key,
					*Registration.Value),
				OutError))
			{
				return false;
			}
		}
		return true;
	}

	FString ResolveTargetCompositionKind(
		const FString& SourceTargetKind,
		const FString& SourceTypeId)
	{
		const FString Target = SourceTargetKind.ToLower();
		const FString Type = SourceTypeId.ToLower();
		if (Target.StartsWith(TEXT("ue.")))
		{
			return Target;
		}
		if (Target.StartsWith(TEXT("niagara"))
			|| Type.StartsWith(TEXT("niagara")))
		{
			const FString Kind = Type.IsEmpty() ? Target : Type;
			if (Kind.Contains(TEXT("emitter")))
			{
				return TEXT("ue.niagara_emitter.asset");
			}
			if (Kind.Contains(TEXT("script")))
			{
				return TEXT("ue.niagara_script.asset");
			}
			return TEXT("ue.niagara_system.asset");
		}
		if (Target == TEXT("animation.skeleton")
			|| Type == TEXT("skeleton"))
		{
			return TEXT("ue.skeleton.asset");
		}
		if (Target == TEXT("animation.skeletal_mesh")
			|| Type == TEXT("skeletal_mesh")
			|| Type == TEXT("static_mesh"))
		{
			return TEXT("ue.imported_mesh.asset");
		}
		if (Target == TEXT("rigging.control_rig")
			|| Type == TEXT("control_rig"))
		{
			return TEXT("ue.control_rig.asset");
		}
		if (Target == TEXT("rigging.ik_rig")
			|| Type == TEXT("ik_rig"))
		{
			return TEXT("ue.ik_rig.asset");
		}
		if (Target == TEXT("rigging.ik_retargeter")
			|| Type == TEXT("ik_retargeter"))
		{
			return TEXT("ue.ik_retargeter.asset");
		}
		if (Target == TEXT("audio.metasound")
			|| Type == TEXT("metasound"))
		{
			return TEXT("ue.metasound.asset");
		}
		if (Target == TEXT("audio.sound_cue")
			|| Type == TEXT("sound_cue"))
		{
			return TEXT("ue.sound_cue.asset");
		}
		if (Target == TEXT("audio.sound_submix")
			|| Type == TEXT("sound_submix"))
		{
			return TEXT("ue.sound_submix.asset");
		}
		if (Type == TEXT("sequence"))
		{
			return TEXT("ue.level_sequence.asset");
		}
		if (Type == TEXT("behavior_tree"))
		{
			return TEXT("ue.behavior_tree.asset");
		}
		if (Type.Contains(TEXT("curve")))
		{
			return TEXT("ue.curve.asset");
		}
		if (Type == TEXT("data_table"))
		{
			return TEXT("ue.data_table.asset");
		}
		if (Type.Contains(TEXT("texture"))
			|| Type == TEXT("paper_sprite")
			|| Type == TEXT("paper_flipbook")
			|| Type == TEXT("paper_tilemap")
			|| Type == TEXT("paper_tileset"))
		{
			return TEXT("ue.imported_texture.asset");
		}
		if (Type == TEXT("material_instance"))
		{
			return TEXT("ue.material_instance.asset");
		}
		return TEXT("ue.generic.object");
	}

	static bool TryComposeEntryViews(
		const FCompositionInput& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		OutError.Reset();
		FTargetCompositionContract CompositionContract;
		if (!FTargetCompositionRegistry::Get().FindContract(
			Request.TargetKind,
			CompositionContract))
		{
			OutError.Set(
				TEXT("target_composition_contract_missing"),
				Request.TargetKind);
			return false;
		}
		if (Normalize(Request.ProfileId).IsEmpty()
			|| Normalize(Request.BaseRevision).IsEmpty())
		{
			OutError.Set(TEXT("composition_request_invalid"));
			return false;
		}

		TArray<FCompositionEntryView> Entries =
			Request.Entries;
		Entries.Sort(
			[](const FCompositionEntryView& Left,
				const FCompositionEntryView& Right)
			{
				if (Left.SemanticId != Right.SemanticId)
				{
					return Left.SemanticId < Right.SemanticId;
				}
				return Left.TargetIdentity.CanonicalKey
					< Right.TargetIdentity.CanonicalKey;
			});

		FString AssetPath;
		TMap<FString, FRelationAggregate> RelationAggregates;
		for (const FCompositionEntryView& Entry : Entries)
		{
			AssetPath = Selector(Entry, TEXT("asset_path"));
			if (!AssetPath.IsEmpty())
			{
				break;
			}
		}
		if (AssetPath.IsEmpty())
		{
			AssetPath = Request.TargetKind;
		}
		const FString TargetId = StableId(
			TEXT("target"),
			Request.TargetKind + TEXT("|") + AssetPath);
		const FString Provenance =
			Normalize(Request.SourceProvenance).IsEmpty()
				? TEXT("authoring_state.capture")
				: Normalize(Request.SourceProvenance);

		TArray<FUnitDraft> Drafts;
		TMap<FString, int32> DraftIndex;
		auto AddDraft =
			[&Drafts, &DraftIndex](FUnitDraft Draft) -> FUnitDraft&
			{
				const int32 Index = Drafts.Add(MoveTemp(Draft));
				DraftIndex.Add(Drafts[Index].UnitId, Index);
				return Drafts[Index];
			};
		FUnitDraft Target;
		Target.UnitId = TargetId;
		Target.UnitKind = ESemanticUnitKind::TargetState;
		Target.SemanticTypeRef = Request.TargetKind;
		Target.Role = TEXT("authoring_target");
		Target.SourceProvenance = Provenance;
		AddDraft(MoveTemp(Target));

		TMap<FString, FString> SurfaceUnits;
		for (const FCompositionSurfaceContract& SurfaceContract
			: CompositionContract.Surfaces)
		{
			FUnitDraft Root;
			Root.UnitId = StableId(
				TEXT("surface"),
				Request.TargetKind + TEXT("|")
					+ SurfaceContract.SurfaceId);
			Root.UnitKind = SurfaceContract.UnitKind;
			Root.OwnerRef = TargetId;
			Root.SemanticTypeRef =
				TEXT("uai.authoring.surface.")
				+ SurfaceContract.SurfaceId;
			Root.Role = SurfaceContract.SurfaceId;
			Root.FieldSetRef =
				SurfaceContract.SurfaceKind
						== EAuthoringSurfaceKind::Properties
					? SurfaceContract.SurfaceId
					: FString();
			Root.SourceProvenance = Provenance;
			SurfaceUnits.Add(
				SurfaceContract.SurfaceId,
				Root.UnitId);
			AddDraft(MoveTemp(Root));
		}

		TMap<FString, FString> GraphUnits;
		TMap<FString, FString> ItemUnits;
		TMap<FString, FString> SourceItemUnits;
		TMap<FString, TArray<FString>> ExplicitOrder;
		TMap<FString, const FCompositionEntryView*>
			EntryByUnit;

		auto BindingForOwner =
			[&Drafts, &DraftIndex](
				const FString& OwnerId,
				const FCompositionEntryView& Entry)
				-> FString
			{
				FString CurrentId = OwnerId;
				TSet<FString> Seen;
				while (!CurrentId.IsEmpty()
					&& !Seen.Contains(CurrentId))
				{
					Seen.Add(CurrentId);
					const int32* Index = DraftIndex.Find(CurrentId);
					if (!Index)
					{
						break;
					}
					const FUnitDraft& Owner = Drafts[*Index];
					if (!Owner.TargetIdentityKey.IsEmpty())
					{
						return FString();
					}
					CurrentId = Owner.OwnerRef;
				}
				return Entry.TargetIdentity.CanonicalKey;
			};

		auto AddDocumentProperty =
			[&](
				const FCompositionEntryView& Entry,
				const FString& OwnerId,
				const FString& SurfaceId,
				const FString& Path,
				const FUeAgentCanonicalValuePtr& Value) -> bool
			{
				FUnitDraft Property;
				Property.UnitId = StableId(
					TEXT("property"),
					Request.TargetKind + TEXT("|")
						+ Entry.SemanticId + TEXT("|")
						+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
						+ Path);
				Property.UnitKind = ESemanticUnitKind::PropertySet;
				Property.OwnerRef = OwnerId;
				Property.SemanticTypeRef =
					Entry.SemanticId + TEXT("#") + Path;
				Property.Role = SurfaceId;
				Property.FieldSetRef = Path;
				Property.TargetIdentityKey =
					BindingForOwner(OwnerId, Entry);
				Property.SourceProvenance =
					Entry.Provenance.IsEmpty()
						? Provenance
						: Entry.Provenance;
				if (!Value.IsValid())
				{
					OutError.Set(
						TEXT("composition_document_value_invalid"),
						Path);
					return false;
				}
				Property.Values.Add(TEXT("value"), Value.ToSharedRef());
				AddDraft(MoveTemp(Property));
				return true;
			};

		auto DecomposeDocument =
			[&](const FCompositionEntryView& Entry) -> bool
			{
				if (!Entry.Value.IsValid())
				{
					return true;
				}
				TArray<FDocumentVisit> Visits;
				if (Entry.Value->GetKind() == EUeAgentValueKind::Object)
				{
					const auto& Fields = Entry.Value->GetObjectItems();
					for (int32 Index = Fields.Num() - 1;
						Index >= 0;
						--Index)
					{
						const FString& Key = Fields[Index].Key;
						const FUeAgentCanonicalValuePtr Value(
							Fields[Index].Value);
						const bool bStructured =
							IsStructuredValue(Fields[Index].Value);
						const FString SurfaceId =
							ChooseDocumentSurface(
								CompositionContract,
								Key,
								bStructured);
						const FString* SurfaceOwner =
							SurfaceUnits.Find(SurfaceId);
						if (!SurfaceOwner)
						{
							continue;
						}
						if (!bStructured)
						{
							if (!AddDocumentProperty(
								Entry,
								*SurfaceOwner,
								SurfaceId,
								Key,
								Value))
							{
								return false;
							}
							continue;
						}
						Visits.Add({
							Value,
							Key,
							Key,
							*SurfaceOwner,
							SurfaceId,
							INDEX_NONE});
					}
				}
				else
				{
					const FString SurfaceId =
						ChooseDocumentSurface(
							CompositionContract,
							TEXT("document"),
							Entry.Value->GetKind()
								== EUeAgentValueKind::Array);
					const FString* SurfaceOwner =
						SurfaceUnits.Find(SurfaceId);
					if (!SurfaceOwner)
					{
						return true;
					}
					if (Entry.Value->GetKind()
						!= EUeAgentValueKind::Array)
					{
						return AddDocumentProperty(
							Entry,
							*SurfaceOwner,
							SurfaceId,
							TEXT("document"),
							Entry.Value);
					}
					Visits.Add({
						Entry.Value,
						TEXT("document"),
						TEXT("document"),
						*SurfaceOwner,
						SurfaceId,
						INDEX_NONE});
				}

				while (!Visits.IsEmpty())
				{
					const FDocumentVisit Visit =
						Visits.Pop(EAllowShrinking::No);
					if (!Visit.Value.IsValid())
					{
						OutError.Set(
							TEXT("composition_document_value_invalid"),
							Visit.Path);
						return false;
					}
					if (Visit.Value->GetKind()
						== EUeAgentValueKind::Array)
					{
						const FString CollectionId = StableId(
							TEXT("collection"),
							Request.TargetKind + TEXT("|")
								+ Entry.SemanticId + TEXT("|")
								+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
								+ Visit.Path);
						FUnitDraft Collection;
						Collection.UnitId = CollectionId;
						Collection.UnitKind =
							ESemanticUnitKind::CollectionState;
						Collection.OwnerRef = Visit.OwnerId;
						Collection.SemanticTypeRef =
							Entry.SemanticId + TEXT("#")
								+ Visit.Path;
						Collection.Role = Visit.SurfaceId;
						Collection.TargetIdentityKey =
							BindingForOwner(Visit.OwnerId, Entry);
						Collection.SourceProvenance =
							Entry.Provenance.IsEmpty()
								? Provenance
								: Entry.Provenance;
						const FString CollectionProvenance =
							Collection.SourceProvenance;
						AddDraft(MoveTemp(Collection));
						ExplicitOrder.FindOrAdd(
							Visit.OwnerId).Add(CollectionId);
						const TArray<FUeAgentCanonicalValueRef>& Items =
							Visit.Value->GetArrayItems();
						for (int32 Index = Items.Num() - 1;
							Index >= 0;
							--Index)
						{
							const FString ItemPath =
								FString::Printf(
									TEXT("%s/%08d"),
									*Visit.Path,
									Index);
							if (IsStructuredValue(Items[Index]))
							{
								Visits.Add({
									FUeAgentCanonicalValuePtr(Items[Index]),
									ItemPath,
									Visit.RoleHint,
									CollectionId,
									Visit.SurfaceId,
									Index});
							}
							else
							{
								FUnitDraft Item;
								Item.UnitId = StableId(
									TEXT("item"),
									Request.TargetKind + TEXT("|")
										+ Entry.SemanticId + TEXT("|")
										+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
										+ ItemPath);
								Item.UnitKind =
									ESemanticUnitKind::ItemState;
								Item.OwnerRef = CollectionId;
								Item.SemanticTypeRef =
									Entry.SemanticId + TEXT("#")
										+ ItemPath;
								Item.Role = Visit.SurfaceId;
								Item.TargetIdentityKey =
									BindingForOwner(CollectionId, Entry);
								Item.SourceProvenance =
									CollectionProvenance;
								if (!TryBuildValues(
									FUeAgentCanonicalValuePtr(Items[Index]),
									{},
									Resources,
									Item.Values,
									OutError))
								{
									return false;
								}
								const FString ItemId = Item.UnitId;
								AddDraft(MoveTemp(Item));
								ExplicitOrder.FindOrAdd(
									CollectionId).Insert(ItemId, 0);
							}
						}
						continue;
					}

					if (Visit.Value->GetKind()
						!= EUeAgentValueKind::Object)
					{
						if (!AddDocumentProperty(
							Entry,
							Visit.OwnerId,
							Visit.SurfaceId,
							Visit.Path,
							Visit.Value))
						{
							return false;
						}
						continue;
					}
					const bool bRelation =
						IsRelationDocumentObject(
							Visit.RoleHint,
							Visit.Value);
					FUnitDraft Unit;
					Unit.UnitId = StableId(
						bRelation ? TEXT("relation") : TEXT("item"),
						Request.TargetKind + TEXT("|")
							+ Entry.SemanticId + TEXT("|")
							+ Entry.TargetIdentity.CanonicalKey + TEXT("|")
							+ Visit.Path);
					Unit.UnitKind = bRelation
						? ESemanticUnitKind::RelationSet
						: ESemanticUnitKind::ItemState;
					Unit.OwnerRef = Visit.OwnerId;
					Unit.SemanticTypeRef =
						Entry.SemanticId + TEXT("#") + Visit.Path;
					Unit.Role = Visit.SurfaceId;
					Unit.TargetIdentityKey =
						BindingForOwner(Visit.OwnerId, Entry);
					Unit.SourceProvenance =
						Entry.Provenance.IsEmpty()
							? Provenance
							: Entry.Provenance;
					FUeAgentCanonicalValuePtr LayoutValue;
					const auto& Fields = Visit.Value->GetObjectItems();
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
						: Fields)
					{
						if (Field.Key == TEXT("pos")
							|| Field.Key == TEXT("position")
							|| Field.Key == TEXT("editor_position"))
						{
							LayoutValue =
								FUeAgentCanonicalValuePtr(Field.Value);
							continue;
						}
						if (!IsStructuredValue(Field.Value)
							|| IsScalarCollection(Field.Value))
						{
							Unit.Values.Add(Field.Key, Field.Value);
						}
					}
					const FString UnitId = Unit.UnitId;
					AddDraft(MoveTemp(Unit));
					if (!bRelation)
					{
						ExplicitOrder.FindOrAdd(
							Visit.OwnerId).Add(UnitId);
					}
					if (LayoutValue.IsValid())
					{
						FUnitDraft Layout;
						Layout.UnitId = StableId(
							TEXT("layout"),
							UnitId);
						Layout.UnitKind =
							ESemanticUnitKind::LayoutState;
						Layout.OwnerRef = UnitId;
						Layout.SemanticTypeRef =
							TEXT("uai.authoring.layout/1");
						Layout.Role = TEXT("editor_layout");
						Layout.SourceProvenance =
							Entry.Provenance.IsEmpty()
								? Provenance
								: Entry.Provenance;
						if (!TryBuildValues(
							LayoutValue,
							{},
							Resources,
							Layout.Values,
							OutError))
						{
							return false;
						}
						AddDraft(MoveTemp(Layout));
					}
					for (int32 Index = Fields.Num() - 1;
						Index >= 0;
						--Index)
					{
						const FString& Key = Fields[Index].Key;
						if (Key == TEXT("pos")
							|| Key == TEXT("position")
							|| Key == TEXT("editor_position"))
						{
							continue;
						}
						const FUeAgentCanonicalValueRef& Child =
							Fields[Index].Value;
						if (!IsStructuredValue(Child))
						{
							continue;
						}
						if (IsScalarCollection(Child))
						{
							continue;
						}
						Visits.Add({
							FUeAgentCanonicalValuePtr(Child),
							Visit.Path + TEXT("/") + Key,
							Key,
							UnitId,
							Visit.SurfaceId,
							INDEX_NONE});
					}
				}
				return true;
			};

		for (const FCompositionEntryView& Entry : Entries)
		{
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId);
			if (Entry.bAvailable
				&& Route.bDocument
				&& !DecomposeDocument(Entry))
			{
				return false;
			}
		}

		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAvailable || !Entry.Value.IsValid())
			{
				continue;
			}
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId);
			if (Route.bDocument)
			{
				continue;
			}
			if (Route.bTargetValues)
			{
				FUnitDraft& TargetDraft =
					Drafts[DraftIndex.FindChecked(TargetId)];
				if (TargetDraft.TargetIdentityKey.IsEmpty())
				{
					TargetDraft.TargetIdentityKey =
						Entry.TargetIdentity.CanonicalKey;
				}
				else if (TargetDraft.TargetIdentityKey
					!= Entry.TargetIdentity.CanonicalKey)
				{
					OutError.Set(
						TEXT("composition_target_identity_conflict"),
						Entry.SemanticId);
					return false;
				}
				if (!TryBuildValues(
					Entry.Value,
					{},
					Resources,
					TargetDraft.Values,
					OutError))
				{
					return false;
				}
				continue;
			}
			if (!Route.bGraph && !Route.bItem)
			{
				continue;
			}
			const FString* SurfaceUnit =
				SurfaceUnits.Find(Route.SurfaceId);
			if (!SurfaceUnit)
			{
				continue;
			}
			FUnitDraft Draft;
			Draft.UnitId = StableId(
				Route.bGraph ? TEXT("structure") : TEXT("item"),
				Request.TargetKind + TEXT("|")
					+ Entry.SemanticId + TEXT("|")
					+ Entry.TargetIdentity.CanonicalKey);
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = Route.bGraph
				? *SurfaceUnit
				: FindGraphOwner(
					Entry,
					GraphUnits,
					*SurfaceUnit);
			Draft.SemanticTypeRef = Entry.SemanticId;
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.CanonicalKey;
			Draft.Role = Route.SurfaceId;
			Draft.SourceProvenance =
				Entry.Provenance.IsEmpty()
					? Provenance
					: Entry.Provenance;
			if (!TryBuildValues(
				Entry.Value,
				{
					TEXT("pos"),
					TEXT("position"),
					TEXT("editor_position")},
				Resources,
				Draft.Values,
				OutError))
			{
				return false;
			}
			const FString DraftId = Draft.UnitId;
			AddDraft(MoveTemp(Draft));
			EntryByUnit.Add(DraftId, &Entry);
			if (Route.bGraph)
			{
				GraphUnits.Add(GraphSelectorKey(Entry), DraftId);
			}
			else
			{
				ItemUnits.Add(ItemSelectorKey(Entry), DraftId);
				const FString SourceItemId =
					Selector(Entry, TEXT("item_id"));
				if (!SourceItemId.IsEmpty())
				{
					SourceItemUnits.Add(
						Selector(Entry, TEXT("asset_path"))
							+ TEXT("|item_id=") + SourceItemId,
						DraftId);
				}
			}
		}

		for (const TPair<FString, const FCompositionEntryView*>& Pair
			: EntryByUnit)
		{
			FUnitDraft& Draft =
				Drafts[DraftIndex.FindChecked(Pair.Key)];
			const FCompositionEntryView& Entry = *Pair.Value;
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId);
			if (Route.bItem)
			{
				Draft.OwnerRef = FindGraphOwner(
					Entry,
					GraphUnits,
					Draft.OwnerRef);
				const FString DeclaredOwner =
					FindDeclaredItemOwner(Entry, SourceItemUnits);
				if (!DeclaredOwner.IsEmpty())
				{
					Draft.OwnerRef = DeclaredOwner;
				}
				else if (Entry.SemanticId.EndsWith(TEXT(".pin")))
				{
					Draft.OwnerRef = FindItemOwner(
						Entry,
						ItemUnits,
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
					TEXT(".dynamic_input")))
				{
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("module_guid"),
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
						TEXT(".renderer"))
					|| Entry.SemanticId.EndsWith(
						TEXT(".event_handler")))
				{
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("emitter_handle_id"),
						Draft.OwnerRef);
					Draft.OwnerRef = FindItemOwnerBySelector(
						Entry,
						ItemUnits,
						TEXT("emitter_name"),
						Draft.OwnerRef);
				}
				else if (Entry.SemanticId.EndsWith(
					TEXT(".component.target")))
				{
					Draft.OwnerRef = FindItemOwnerByAliasedSelector(
						Entry,
						ItemUnits,
						TEXT("owner_actor_guid"),
						TEXT("actor_guid"),
						Draft.OwnerRef);
					Draft.OwnerRef = FindItemOwnerByAliasedSelector(
						Entry,
						ItemUnits,
						TEXT("owner_managed_id"),
						TEXT("managed_id"),
						Draft.OwnerRef);
				}
			}
			const FUeAgentCanonicalValuePtr Layout =
				FindLayoutValue(Entry);
			if (Layout.IsValid())
			{
				FUnitDraft LayoutDraft;
				LayoutDraft.UnitId = StableId(
					TEXT("layout"),
					Draft.UnitId);
				LayoutDraft.UnitKind =
					ESemanticUnitKind::LayoutState;
				LayoutDraft.OwnerRef = Draft.UnitId;
				LayoutDraft.SemanticTypeRef =
					TEXT("uai.authoring.layout/1");
				LayoutDraft.Role = TEXT("editor_layout");
				LayoutDraft.SourceProvenance =
					Draft.SourceProvenance;
				if (!TryBuildValues(
					Layout,
					{},
					Resources,
					LayoutDraft.Values,
					OutError))
				{
					return false;
				}
				AddDraft(MoveTemp(LayoutDraft));
			}
		}

		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.bAvailable || !Entry.Value.IsValid())
			{
				continue;
			}
			const FEntryRoute Route =
				RouteEntry(Request.TargetKind, Entry.SemanticId);
			if (!Route.bProperty && !Route.bRelation)
			{
				continue;
			}
			const FString* SurfaceUnit =
				SurfaceUnits.Find(Route.SurfaceId);
			if (!SurfaceUnit)
			{
				continue;
			}
			const FString DeclaredOwner = Route.bProperty
				? FindDeclaredItemOwner(Entry, SourceItemUnits)
				: FString();
			const FString OwnerRef = !DeclaredOwner.IsEmpty()
				? DeclaredOwner
				: Route.bProperty
					? FindItemOwner(Entry, ItemUnits, *SurfaceUnit)
					: FindGraphOwner(Entry, GraphUnits, *SurfaceUnit);
			if (Route.bRelation)
			{
				const FString AggregateKey =
					OwnerRef + TEXT("|") + Route.SurfaceId;
				FRelationAggregate& Aggregate =
					RelationAggregates.FindOrAdd(AggregateKey);
				Aggregate.OwnerRef = OwnerRef;
				Aggregate.SurfaceId = Route.SurfaceId;
				TMap<FString, FUeAgentCanonicalValueRef>& Group =
					Aggregate.Groups.FindOrAdd(Entry.SemanticId);
				if (Group.Contains(Entry.TargetIdentity.CanonicalKey))
				{
					OutError.Set(
						TEXT("composition_relation_identity_conflict"),
						Entry.TargetIdentity.CanonicalKey);
					return false;
				}
				Group.Add(
					Entry.TargetIdentity.CanonicalKey,
					Entry.Value.ToSharedRef());

				if (Entry.Value->GetKind() == EUeAgentValueKind::Object)
				{
					for (const TCHAR* Field : {
						TEXT("from_node"),
						TEXT("to_node")})
					{
						const FUeAgentCanonicalValueRef* NodeValue =
							FindObjectField(
								Entry.Value.ToSharedRef(),
								Field);
						if (!NodeValue
							|| (*NodeValue)->GetKind()
								!= EUeAgentValueKind::String
							|| !(*NodeValue)->GetText().StartsWith(
								TEXT("builtin:")))
						{
							continue;
						}
						const FString& NodeRef = (*NodeValue)->GetText();
						const FString BuiltinId = StableId(
							TEXT("item"),
							Request.TargetKind + TEXT("|")
								+ OwnerRef + TEXT("|") + NodeRef);
						if (DraftIndex.Contains(BuiltinId))
						{
							continue;
						}
						FUnitDraft Builtin;
						Builtin.UnitId = BuiltinId;
						Builtin.UnitKind =
							ESemanticUnitKind::ItemState;
						Builtin.OwnerRef = OwnerRef;
						Builtin.SemanticTypeRef =
							TEXT("ue.blueprint.node.builtin");
						Builtin.Role = TEXT("graph_collection");
						Builtin.SourceProvenance = Provenance;
						FUeAgentCanonicalValuePtr BuiltinValue;
						FUeAgentStateSemanticError SemanticError;
						if (!FUeAgentCanonicalValue::TryString(
							NodeRef,
							Resources,
							BuiltinValue,
							SemanticError))
						{
							OutError.Set(
								SemanticError.Code,
								SemanticError.Path);
							return false;
						}
						Builtin.Values.Add(
							TEXT("builtin_id"),
							BuiltinValue.ToSharedRef());
						AddDraft(MoveTemp(Builtin));
					}
				}
				continue;
			}
			FUnitDraft Draft;
			Draft.UnitId = StableId(
				TEXT("property"),
				Request.TargetKind + TEXT("|")
					+ Entry.SemanticId + TEXT("|")
					+ Entry.TargetIdentity.CanonicalKey);
			Draft.UnitKind = Route.UnitKind;
			Draft.OwnerRef = OwnerRef;
			Draft.SemanticTypeRef = Entry.SemanticId;
			Draft.TargetIdentityKey =
				Entry.TargetIdentity.CanonicalKey;
			Draft.Role = Route.SurfaceId;
			Draft.FieldSetRef = Route.bProperty
				? Normalize(Selector(Entry, TEXT("property_path")))
				: FString();
			if (Draft.FieldSetRef.IsEmpty() && Route.bProperty)
			{
				FString FieldSetBase =
					Normalize(Selector(Entry, TEXT("selector")));
				if (FieldSetBase.IsEmpty())
				{
					FieldSetBase = Route.SurfaceId;
				}
				Draft.FieldSetRef = FieldSetBase + TEXT("-")
					+ StableId(
						TEXT("binding"),
						Entry.SemanticId + TEXT("|")
							+ Entry.TargetIdentity.CanonicalKey);
			}
			Draft.SourceProvenance =
				Entry.Provenance.IsEmpty()
					? Provenance
					: Entry.Provenance;
			if (!TryBuildValues(
				Entry.Value,
				{},
				Resources,
				Draft.Values,
				OutError))
			{
				return false;
			}
			AddDraft(MoveTemp(Draft));
		}

		TArray<FString> RelationAggregateKeys;
		RelationAggregates.GetKeys(RelationAggregateKeys);
		RelationAggregateKeys.Sort();
		for (const FString& AggregateKey : RelationAggregateKeys)
		{
			const FRelationAggregate& Aggregate =
				RelationAggregates.FindChecked(AggregateKey);
			TArray<FString> SemanticIds;
			Aggregate.Groups.GetKeys(SemanticIds);
			SemanticIds.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> GroupValues;
			for (const FString& SemanticId : SemanticIds)
			{
				const TMap<FString, FUeAgentCanonicalValueRef>& Records =
					Aggregate.Groups.FindChecked(SemanticId);
				TArray<FString> RecordIds;
				Records.GetKeys(RecordIds);
				RecordIds.Sort();
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> RecordValues;
				for (const FString& RecordId : RecordIds)
				{
					RecordValues.Emplace(
						RecordId,
						Records.FindChecked(RecordId));
				}
				FUeAgentCanonicalValuePtr GroupValue;
				FUeAgentStateSemanticError SemanticError;
				if (!FUeAgentCanonicalValue::TryObject(
					RecordValues,
					Resources,
					GroupValue,
					SemanticError))
				{
					OutError.Set(
						SemanticError.Code,
						SemanticError.Path);
					return false;
				}
				GroupValues.Emplace(
					SemanticId,
					GroupValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr RelationsValue;
			FUeAgentStateSemanticError SemanticError;
			if (!FUeAgentCanonicalValue::TryObject(
				GroupValues,
				Resources,
				RelationsValue,
				SemanticError))
			{
				OutError.Set(
					SemanticError.Code,
					SemanticError.Path);
				return false;
			}
			FUnitDraft RelationSet;
			RelationSet.UnitId = StableId(
				TEXT("relation"),
				Request.TargetKind + TEXT("|") + AggregateKey);
			RelationSet.UnitKind = ESemanticUnitKind::RelationSet;
			RelationSet.OwnerRef = Aggregate.OwnerRef;
			RelationSet.SemanticTypeRef =
				TEXT("uai.authoring.relations/1");
			RelationSet.Role = Aggregate.SurfaceId;
			RelationSet.SourceProvenance = Provenance;
			RelationSet.Values.Add(
				TEXT("relations"),
				RelationsValue.ToSharedRef());
			AddDraft(MoveTemp(RelationSet));
		}

		TArray<FString> DependencyPaths;
		for (const FCompositionEntryView& Entry : Entries)
		{
			if (!Entry.Value.IsValid()
				|| Entry.Value->GetKind() != EUeAgentValueKind::Object)
			{
				continue;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Field
				: Entry.Value->GetObjectItems())
			{
				if (Field.Key == TEXT("asset_path")
					|| !Field.Key.EndsWith(TEXT("_path"))
					|| Field.Value->GetKind()
						!= EUeAgentValueKind::String)
				{
					continue;
				}
				const FString& Path = Field.Value->GetText();
				if (Path.StartsWith(TEXT("/")))
				{
					DependencyPaths.AddUnique(Path);
				}
			}
		}
		DependencyPaths.Sort();
		if (!DependencyPaths.IsEmpty())
		{
			FUnitDraft Dependencies;
			Dependencies.UnitId = StableId(
				TEXT("dependencies"),
				TargetId);
			Dependencies.UnitKind =
				ESemanticUnitKind::DependencyState;
			Dependencies.OwnerRef = TargetId;
			Dependencies.SemanticTypeRef =
				TEXT("uai.authoring.dependencies/1");
			Dependencies.Role = TEXT("asset_dependencies");
			Dependencies.SourceProvenance = Provenance;
			FUeAgentCanonicalValuePtr PathsValue;
			if (!TryStringArrayValue(
				DependencyPaths,
				Resources,
				PathsValue,
				OutError))
			{
				return false;
			}
			Dependencies.Values.Add(
				TEXT("asset_paths"),
				PathsValue.ToSharedRef());
			AddDraft(MoveTemp(Dependencies));
		}

		for (const FUnitDraft& Child : Drafts)
		{
			if (Child.OwnerRef.IsEmpty())
			{
				continue;
			}
			if (int32* OwnerIndex = DraftIndex.Find(Child.OwnerRef))
			{
				AddOwnedReference(Drafts[*OwnerIndex], Child);
			}
		}

		const int32 ContainerCount = Drafts.Num();
		for (int32 Index = 0; Index < ContainerCount; ++Index)
		{
			FUnitDraft& Container = Drafts[Index];
			if (Container.ItemRefs.IsEmpty())
			{
				continue;
			}
			if (const TArray<FString>* Ordered =
				ExplicitOrder.Find(Container.UnitId))
			{
				TArray<FString> StableOrder;
				for (const FString& ItemId : *Ordered)
				{
					if (Container.ItemRefs.Contains(ItemId))
					{
						StableOrder.AddUnique(ItemId);
					}
				}
				TArray<FString> Remaining = Container.ItemRefs;
				Remaining.RemoveAll(
					[&StableOrder](const FString& ItemId)
					{
						return StableOrder.Contains(ItemId);
					});
				Remaining.Sort();
				StableOrder.Append(Remaining);
				Container.ItemRefs = MoveTemp(StableOrder);
			}
			else
			{
				Container.ItemRefs.Sort();
			}
			FUnitDraft Ordering;
			Ordering.UnitId = StableId(
				TEXT("ordering"),
				Container.UnitId);
			Ordering.UnitKind =
				ESemanticUnitKind::OrderingState;
			Ordering.OwnerRef = Container.UnitId;
			Ordering.SemanticTypeRef =
				TEXT("uai.authoring.ordering/1");
			Ordering.Role = TEXT("stable_item_order");
			Ordering.SourceProvenance = Provenance;
			FUeAgentCanonicalValuePtr OrderValue;
			if (!TryStringArrayValue(
				Container.ItemRefs,
				Resources,
				OrderValue,
				OutError))
			{
				return false;
			}
			Ordering.Values.Add(
				TEXT("item_refs"),
				OrderValue.ToSharedRef());
			Container.OrderingRef = Ordering.UnitId;
			AddDraft(MoveTemp(Ordering));
		}

		TArray<FSemanticUnit> Units;
		Units.Reserve(Drafts.Num());
		for (FUnitDraft& Draft : Drafts)
		{
			Draft.ItemRefs.Sort();
			Draft.RelationRefs.Sort();
			Draft.DependencyRefs.Sort();
			Units.Emplace(
				MoveTemp(Draft.UnitId),
				Draft.UnitKind,
				MoveTemp(Draft.OwnerRef),
				MoveTemp(Draft.SemanticTypeRef),
				MoveTemp(Draft.Role),
				MoveTemp(Draft.FieldSetRef),
				MoveTemp(Draft.Values),
				MoveTemp(Draft.ItemRefs),
				MoveTemp(Draft.RelationRefs),
				MoveTemp(Draft.OrderingRef),
				MoveTemp(Draft.LayoutRef),
				MoveTemp(Draft.DependencyRefs),
				MoveTemp(Draft.ProfileVisibility),
				MoveTemp(Draft.SourceProvenance),
				MoveTemp(Draft.TargetIdentityKey));
		}
		return TryCompileAuthoringModel(
			CompositionSchema,
			1,
			Request.ProfileId,
			Request.BaseRevision,
			MoveTemp(Units),
			{TargetId},
			Resources,
			OutModel,
			OutError);
	}

	bool TryComposeAuthoringModel(
		const FCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		FCompositionInput Input;
		Input.TargetKind = Request.TargetKind;
		Input.ProfileId = Request.ProfileId;
		Input.BaseRevision = Request.BaseRevision;
		Input.SourceProvenance = Request.SourceProvenance;
		Input.Entries.Reserve(Request.Entries.Num());
		for (const UeAgentAuthoringState::FStateEntry& Entry
			: Request.Entries)
		{
			FCompositionEntryView View;
			View.SemanticId = Entry.SemanticId;
			View.TargetIdentity.CanonicalKey =
				Entry.TargetIdentity.CanonicalKey;
			View.TargetIdentity.Selectors =
				Entry.TargetIdentity.Selectors;
			View.bAvailable = Entry.bAvailable;
			View.Provenance = Entry.Provenance;
			if (Entry.Value.IsValid()
				&& !TryCanonicalizeJson(
					Entry.Value,
					Resources,
					View.Value,
					OutError))
			{
				return false;
			}
			Input.Entries.Add(MoveTemp(View));
		}
		return TryComposeEntryViews(
			Input,
			Resources,
			OutModel,
			OutError);
	}

	bool TryComposeCanonicalAuthoringModel(
		const FCanonicalCompositionRequest& Request,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		FCompositionInput Input;
		Input.TargetKind = Request.TargetKind;
		Input.ProfileId = Request.ProfileId;
		Input.BaseRevision = Request.BaseRevision;
		Input.SourceProvenance = Request.SourceProvenance;
		Input.Entries.Reserve(Request.Entries.Num());
		for (const FUeAgentStateEntry& Entry : Request.Entries)
		{
			FCompositionEntryView View;
			View.SemanticId = Entry.GetIdentity().GetSemanticId();
			View.TargetIdentity.CanonicalKey =
				Entry.GetIdentity().GetCanonicalKey();
			View.TargetIdentity.Selectors =
				Entry.GetIdentity().GetSelectors();
			if (View.TargetIdentity.Selectors.IsEmpty()
				&& !Entry.GetIdentity().GetSelector().IsEmpty())
			{
				View.TargetIdentity.Selectors.Add(
					TEXT("selector"),
					Entry.GetIdentity().GetSelector());
			}
			View.Value = FUeAgentCanonicalValuePtr(Entry.GetValue());
			View.bAvailable =
				Entry.GetDetails().GetAvailability()
					!= TEXT("unavailable");
			View.Provenance = Request.SourceProvenance;
			Input.Entries.Add(MoveTemp(View));
		}
		return TryComposeEntryViews(
			Input,
			Resources,
			OutModel,
			OutError);
	}

	bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		const FUeAgentStateRevision Revision(
			TEXT("authoring.composition"),
			Model->GetBaseRevision(),
			0);
		const FString EvidenceId =
			TEXT("composition:") + Model->GetSemanticFingerprint();
		return TryBuildCompositionExportPlan(
			Model,
			{
				EvidenceId,
				Revision,
				EvidenceId,
			},
			MoveTemp(Telemetry),
			Resources,
			OutPlan,
			OutError);
	}

	bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		OutError.Reset();
		if (!SnapshotBinding.IsValid())
		{
			OutError.Set(
				TEXT("composition_snapshot_binding_invalid"));
			return false;
		}
		FWorkspaceLayoutPlanPtr Layout;
		if (!TryPlanWorkspaceLayout(
			Model,
			2,
			EWorkspaceSourceMode::Directory,
			MoveTemp(Telemetry),
			Resources,
			Layout,
			OutError)
			|| !Layout.IsValid())
		{
			return false;
		}

		TArray<FExportFilePlan> Files;
		TArray<TSharedRef<FJsonObject>> InventoryRows;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			TArray<uint8> RowBytes;
			TArray<uint8> FileBytes;
			if (!TrySerializeSemanticUnitRow(Unit, RowBytes)
				|| !TrySerializeSemanticUnitDocument(
					Unit,
					FileBytes))
			{
				OutError.Set(
					TEXT("composition_unit_serialize_failed"),
					Unit.GetUnitId());
				return false;
			}
			const FString RelativePath =
				Layout->GetLogicalPlacements().FindChecked(
					Unit.GetUnitId());
			Files.Emplace(
				RelativePath,
				FileBytes,
				Sha256(FileBytes));
			TSharedRef<FJsonObject> Inventory =
				MakeShared<FJsonObject>();
			Inventory->SetStringField(
				TEXT("unit_id"),
				Unit.GetUnitId());
			Inventory->SetStringField(
				TEXT("unit_kind"),
				LexToString(Unit.GetUnitKind()));
			Inventory->SetStringField(
				TEXT("logical_path"),
				RelativePath);
			Inventory->SetStringField(
				TEXT("content_hash"),
				Sha256(RowBytes));
			InventoryRows.Add(Inventory);
		}

		const FUeAgentStateRevision& Revision =
			SnapshotBinding.Revision;
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(
			TEXT("workspace_schema"),
			GetSemanticWorkspaceSchemaId());
		Manifest->SetStringField(TEXT("import_mode"), TEXT("state"));
		Manifest->SetStringField(
			TEXT("contract_id"),
			Model->GetContractId());
		Manifest->SetNumberField(
			TEXT("contract_version"),
			Model->GetContractVersion());
		Manifest->SetNumberField(
			TEXT("layout_version"),
			Layout->GetLayoutVersion());
		Manifest->SetStringField(
			TEXT("profile_id"),
			Model->GetProfileId());
		Manifest->SetStringField(
			TEXT("base_revision"),
			Model->GetBaseRevision());
		TSharedRef<FJsonObject> RevisionObject =
			MakeShared<FJsonObject>();
		RevisionObject->SetStringField(
			TEXT("source_id"),
			Revision.GetSourceId());
		RevisionObject->SetStringField(
			TEXT("generation"),
			Revision.GetGeneration());
		RevisionObject->SetNumberField(
			TEXT("sequence"),
			Revision.GetSequence());
		Manifest->SetObjectField(
			TEXT("base_state_revision"),
			RevisionObject);
		Manifest->SetStringField(
			TEXT("semantic_fingerprint"),
			Model->GetSemanticFingerprint());
		Manifest->SetArrayField(
			TEXT("target_unit_ids"),
			StringArray(Model->GetTargetUnitIds()));
		TArray<TSharedPtr<FJsonValue>> InventoryValues;
		for (const TSharedRef<FJsonObject>& Inventory : InventoryRows)
		{
			InventoryValues.Add(
				MakeShared<FJsonValueObject>(Inventory));
		}
		Manifest->SetArrayField(
			TEXT("unit_inventory"),
			MoveTemp(InventoryValues));
		Manifest->SetArrayField(
			TEXT("structural_edits"),
			TArray<TSharedPtr<FJsonValue>>{});

		TArray<uint8> ManifestBytes;
		if (!SerializeJson(Manifest, ManifestBytes))
		{
			OutError.Set(
				TEXT("composition_manifest_serialize_failed"));
			return false;
		}
		Files.Emplace(
			ManifestPath,
			ManifestBytes,
			Sha256(ManifestBytes));
		Files.Sort(
			[](const FExportFilePlan& Left,
				const FExportFilePlan& Right)
			{
				return Left.GetRelativePath()
					< Right.GetRelativePath();
			});

		int64 OutputBytes = 0;
		FString PlanMaterial =
			TEXT("directory|") + ManifestPath + TEXT("|")
			+ Model->GetSemanticFingerprint()
			+ TEXT("|") + SnapshotBinding.SnapshotId
			+ TEXT("|") + Revision.GetSourceId()
			+ TEXT("|") + Revision.GetGeneration()
			+ TEXT("|")
			+ FString::Printf(TEXT("%lld"), Revision.GetSequence())
			+ TEXT("|") + SnapshotBinding.EvidenceId;
		for (const FExportFilePlan& File : Files)
		{
			OutputBytes += File.GetCanonicalUtf8().Num();
			PlanMaterial += TEXT("|") + File.GetRelativePath()
				+ TEXT("|") + File.GetContentHash();
		}
		FUeAgentResourceBudgetExceeded ResourceError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::OutputBytes, OutputBytes},
				{EUeAgentResourceDimension::Files, Files.Num()},
				{EUeAgentResourceDimension::WorkUnits,
					Files.Num() + Model->GetUnits().Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					OutputBytes * 2},
			},
			TEXT("authoring_composition.materialize"),
			ResourceError))
		{
			OutError.Set(
				ResourceError.Code,
				ResourceError.Path,
				TEXT("/"),
				ResourceError.Observed,
				ResourceError.bStoppedEarly);
			return false;
		}
		const FString Fingerprint = Sha256Text(PlanMaterial);
		const int64 ObservedTokens = EstimatedTokens(Files);
		OutPlan = MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::Directory,
			ManifestPath,
			SnapshotBinding.SnapshotId,
			Revision,
			SnapshotBinding.EvidenceId,
			MoveTemp(Files),
			Fingerprint,
			ObservedTokens);
		return true;
	}

	bool TryExportCompositionWorkspace(
		const FString& TargetKind,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const FString& WorkspaceRoot,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError)
	{
		OutResult =
			UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();
		FString ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError = TEXT("target_composition_contract_registration_failed:")
				+ ContractError;
			return false;
		}
		const TMap<EUeAgentResourceDimension, int64> Limits{
			{EUeAgentResourceDimension::InputBytes, 1ll << 40},
			{EUeAgentResourceDimension::OutputBytes, 1ll << 40},
			{EUeAgentResourceDimension::StringBytes, 1ll << 40},
			{EUeAgentResourceDimension::Nodes, 1ll << 32},
			{EUeAgentResourceDimension::Items, 1ll << 32},
			{EUeAgentResourceDimension::Files, 1ll << 28},
			{EUeAgentResourceDimension::ReferenceEdges, 1ll << 32},
			{EUeAgentResourceDimension::WorkUnits, 1ll << 34},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 1ll << 42},
		};
		FUeAgentResourceTracker Resources{
			FUeAgentResourceBudget(Limits)};
		const TArray<UeAgentAuthoringState::FStateEntry> Entries =
			Snapshot.CopyEntries();
		FString RevisionMaterial = TargetKind;
		for (const UeAgentAuthoringState::FStateEntry& Entry : Entries)
		{
			RevisionMaterial += TEXT("|") + Entry.SemanticId
				+ TEXT("|") + Entry.TargetIdentity.CanonicalKey;
		}
		FCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = ProfileId;
		Request.BaseRevision =
			TEXT("capture:") + Sha256Text(RevisionMaterial).Right(24);
		Request.SourceProvenance = TEXT("authoring_state.capture");
		Request.Entries = Entries;
		FAuthoringModelPtr Model;
		FWorkspaceError WorkspaceError;
		if (!TryComposeAuthoringModel(
			Request,
			Resources,
			Model,
			WorkspaceError)
			|| !Model.IsValid())
		{
			OutError = WorkspaceError.GetCode() + TEXT(":")
				+ WorkspaceError.GetSourceUnit();
			return false;
		}
		FExportWorkspacePlanPtr Plan;
		if (!TryBuildCompositionExportPlan(
			Model.ToSharedRef(),
			{},
			Resources,
			Plan,
			WorkspaceError)
			|| !Plan.IsValid())
		{
			OutError = WorkspaceError.GetCode() + TEXT(":")
				+ WorkspaceError.GetSourceUnit();
			return false;
		}
		FArtifactPublishReceipt Receipt;
		FArtifactPublishError PublishError;
		const FString OwnerId =
			TEXT("uai.authoring.composition.")
			+ TargetKind.Replace(TEXT("."), TEXT("_"));
		if (!TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(),
				WorkspaceRoot,
				OwnerId),
			Resources,
			Receipt,
			PublishError))
		{
			OutResult.ErrorCode = PublishError.GetCode();
			OutResult.ErrorDetail = PublishError.GetDetail();
			OutError = OutResult.ErrorCode + TEXT(":")
				+ OutResult.ErrorDetail;
			return false;
		}

		OutResult.Status = TEXT("written");
		OutResult.ContractId = Model->GetContractId();
		OutResult.ContractVersion =
			FString::FromInt(Model->GetContractVersion());
		OutResult.ProfileId = ProfileId;
		OutResult.WorkspaceRoot = Receipt.GetFinalPath();
		OutResult.EntryFile = Receipt.GetEntryFile();
		OutResult.TokenBudget = 0;
		for (const FExportFilePlan& File : Plan->GetFiles())
		{
			UeAgentExportWorkspaceWriter::FPlannedFile Planned;
			Planned.FileId = File.GetRelativePath();
			Planned.RelativePath = File.GetRelativePath();
			Planned.ResolvedPath = FPaths::Combine(
				Receipt.GetFinalPath(),
				File.GetRelativePath());
			Planned.Role =
				File.GetRelativePath() == ManifestPath
					? TEXT("machine_manifest")
					: TEXT("semantic_unit");
			Planned.Metrics.Utf8Bytes =
				File.GetCanonicalUtf8().Num();
			Planned.Metrics.CharacterCount =
				File.GetCanonicalUtf8().Num();
			Planned.Metrics.TokenProxy =
				(File.GetCanonicalUtf8().Num() + 3) / 4;
			OutResult.Metrics.Add(Planned.Metrics);
			OutResult.Files.Add(MoveTemp(Planned));
		}
		OutResult.WrittenRelativeFiles =
			Receipt.GetIncludedFiles();
		for (const FString& Relative : Receipt.GetIncludedFiles())
		{
			OutResult.WrittenFiles.Add(
				FPaths::Combine(
					Receipt.GetFinalPath(),
					Relative));
		}
		TSharedRef<FJsonObject> ReceiptJson =
			MakeShared<FJsonObject>();
		ReceiptJson->SetStringField(
			TEXT("publisher_owner_id"),
			Receipt.GetPublisherOwnerId());
		ReceiptJson->SetStringField(
			TEXT("transaction_id"),
			Receipt.GetTransactionId());
		ReceiptJson->SetStringField(
			TEXT("plan_fingerprint"),
			Receipt.GetPlanFingerprint());
		ReceiptJson->SetStringField(
			TEXT("inventory_hash"),
			Receipt.GetInventoryHash());
		OutResult.PublisherReceipt = ReceiptJson;
		return true;
	}
}
