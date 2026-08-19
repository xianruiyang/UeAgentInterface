#include "Core/StateSemantic/UeAgentSemanticSurface.h"

#include "Containers/StringConv.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::StateSemantic
{
	using namespace UE::UAI::ContextResource;

	namespace
	{
		const FString SurfaceSchema = TEXT("uai.semantic_surface/1");

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool Fail(FString& OutError, const TCHAR* Code)
		{
			OutError = Code;
			return false;
		}

		bool ParseKind(
			const FString& Text,
			EUeAgentSemanticSurfaceKind& OutKind)
		{
			for (const EUeAgentSemanticSurfaceKind Candidate : {
				EUeAgentSemanticSurfaceKind::PropertyCollection,
				EUeAgentSemanticSurfaceKind::Graph,
				EUeAgentSemanticSurfaceKind::Tree,
				EUeAgentSemanticSurfaceKind::Timeline,
				EUeAgentSemanticSurfaceKind::KeyedTable,
				EUeAgentSemanticSurfaceKind::Spatial,
				EUeAgentSemanticSurfaceKind::ExternalProvenance})
			{
				if (LexToString(Candidate) == Text)
				{
					OutKind = Candidate;
					return true;
				}
			}
			return false;
		}

		bool ParseIntent(
			const FString& Text,
			EUeAgentSemanticSurfaceIntent& OutIntent)
		{
			for (const EUeAgentSemanticSurfaceIntent Candidate : {
				EUeAgentSemanticSurfaceIntent::FullState,
				EUeAgentSemanticSurfaceIntent::MinimalCreate,
				EUeAgentSemanticSurfaceIntent::LocalEdit})
			{
				if (LexToString(Candidate) == Text)
				{
					OutIntent = Candidate;
					return true;
				}
			}
			return false;
		}

		bool ParseCompleteness(
			const FString& Text,
			EUeAgentSemanticSurfaceCompleteness& OutCompleteness)
		{
			for (const EUeAgentSemanticSurfaceCompleteness Candidate : {
				EUeAgentSemanticSurfaceCompleteness::Complete,
				EUeAgentSemanticSurfaceCompleteness::Partial})
			{
				if (LexToString(Candidate) == Text)
				{
					OutCompleteness = Candidate;
					return true;
				}
			}
			return false;
		}

		bool HasValueKind(
			const FUeAgentSemanticSurfaceItem& Item,
			EUeAgentValueKind Kind)
		{
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
				: Item.Values)
			{
				if (Pair.Value->GetKind() == Kind)
				{
					return true;
				}
			}
			return false;
		}

		bool HasOwnerCycle(
			const TArray<FUeAgentSemanticSurfaceItem>& Items,
			const FString& SurfaceId)
		{
			TMap<FString, FString> Owners;
			for (const FUeAgentSemanticSurfaceItem& Item : Items)
			{
				Owners.Add(Item.Identity, Item.OwnerIdentity);
			}
			for (const FUeAgentSemanticSurfaceItem& Start : Items)
			{
				TSet<FString> Seen;
				FString Current = Start.Identity;
				while (Current != SurfaceId)
				{
					if (Seen.Contains(Current))
					{
						return true;
					}
					Seen.Add(Current);
					const FString* Owner = Owners.Find(Current);
					if (Owner == nullptr)
					{
						break;
					}
					Current = *Owner;
				}
			}
			return false;
		}

		TSharedRef<FJsonObject> ValuesToJson(
			const TMap<FString, FUeAgentCanonicalValueRef>& Values,
			FString& OutError)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				TSharedPtr<FJsonValue> JsonValue;
				if (!CanonicalJson::TryToJsonValue(
						*Values.FindChecked(Key),
						JsonValue,
						OutError)
					|| !JsonValue.IsValid())
				{
					return MakeShared<FJsonObject>();
				}
				Result->SetField(Key, JsonValue);
			}
			return Result;
		}

		bool TryReadValues(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FString& OutError)
		{
			OutValues.Reset();
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(TEXT("values"), Values)
				|| Values == nullptr
				|| !Values->IsValid())
			{
				return Fail(OutError, TEXT("semantic_surface_values_missing"));
			}
			TArray<FString> Keys;
			(*Values)->Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!CanonicalJson::TryFromJsonValue(
						(*Values)->Values.FindChecked(Key),
						Resources,
						Canonical,
						OutError)
					|| !Canonical.IsValid())
				{
					return false;
				}
				OutValues.Add(Key, Canonical.ToSharedRef());
			}
			return true;
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					OutError)
				&& OutValue.IsValid();
		}

		bool TryIntegerValue(
			int64 Number,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			OutValue.Reset();
			return FUeAgentCanonicalValue::TryInteger(
					FString::Printf(TEXT("%lld"), Number),
					Resources,
					OutValue,
					OutError)
				&& OutValue.IsValid();
		}

		bool TryObjectValue(
			const TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentStateSemanticError& OutError)
		{
			TArray<FString> Keys;
			Fields.GetKeys(Keys);
			Keys.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const FString& Key : Keys)
			{
				Items.Emplace(Key, Fields.FindChecked(Key));
			}
			OutValue.Reset();
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					OutError)
				&& OutValue.IsValid();
		}

		bool TryAddStringField(
			TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			const FString& Name,
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr Canonical;
			if (!TryStringValue(Value, Resources, Canonical, OutError))
			{
				return false;
			}
			Fields.Add(Name, Canonical.ToSharedRef());
			return true;
		}

		bool TryAddIntegerField(
			TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			const FString& Name,
			int64 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr Canonical;
			if (!TryIntegerValue(Value, Resources, Canonical, OutError))
			{
				return false;
			}
			Fields.Add(Name, Canonical.ToSharedRef());
			return true;
		}

		bool TryAddSnapshotEntry(
			const FUeAgentSemanticSurface& Surface,
			const FString& Identity,
			const FString& ObjectRole,
			const FString& StableKey,
			const FString& SemanticType,
			const TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			const FUeAgentStateRevision& Revision,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentStateSemanticError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			if (!TryObjectValue(Fields, Resources, Value, OutError))
			{
				return false;
			}
			TMap<FString, FString> Selectors{
				{TEXT("surface_id"), Surface.GetSurfaceId()},
				{TEXT("object_id"), Identity},
			};
			if (!StableKey.IsEmpty())
			{
				Selectors.Add(TEXT("stable_key"), StableKey);
			}
			OutEntries.Emplace(
				FUeAgentCanonicalIdentity(
					SemanticType,
					LexToString(Surface.GetKind()),
					Surface.GetSurfaceId(),
					MoveTemp(Selectors),
					ObjectRole,
					Revision.GetGeneration()),
				FUeAgentCanonicalType(
					TEXT("uai.semantic_surface.object/1"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					Surface.GetSurfaceId(),
					Surface.GetSourceProvenance(),
					Value.ToSharedRef()),
				FUeAgentStateDetails(
					Identity,
					TEXT("Canonical semantic surface readback"),
					{TEXT("UAI"), TEXT("SemanticSurface")},
					false,
					TEXT("available"),
					{}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::None);
			if (!OutEntries.Last().IsValid())
			{
				OutError.Code = TEXT("semantic_surface_snapshot_entry_invalid");
				OutError.Path = Identity;
				return false;
			}
			return true;
		}

	}

	FString LexToString(EUeAgentSemanticSurfaceKind Kind)
	{
		switch (Kind)
		{
		case EUeAgentSemanticSurfaceKind::PropertyCollection:
			return TEXT("property_collection");
		case EUeAgentSemanticSurfaceKind::Graph:
			return TEXT("graph");
		case EUeAgentSemanticSurfaceKind::Tree:
			return TEXT("tree");
		case EUeAgentSemanticSurfaceKind::Timeline:
			return TEXT("timeline");
		case EUeAgentSemanticSurfaceKind::KeyedTable:
			return TEXT("keyed_table");
		case EUeAgentSemanticSurfaceKind::Spatial:
			return TEXT("spatial");
		case EUeAgentSemanticSurfaceKind::ExternalProvenance:
			return TEXT("external_provenance");
		default:
			return FString();
		}
	}

	FString LexToString(EUeAgentSemanticSurfaceIntent Intent)
	{
		switch (Intent)
		{
		case EUeAgentSemanticSurfaceIntent::FullState:
			return TEXT("full_state");
		case EUeAgentSemanticSurfaceIntent::MinimalCreate:
			return TEXT("minimal_create");
		case EUeAgentSemanticSurfaceIntent::LocalEdit:
			return TEXT("local_edit");
		default:
			return FString();
		}
	}

	FString LexToString(EUeAgentSemanticSurfaceCompleteness Completeness)
	{
		return Completeness == EUeAgentSemanticSurfaceCompleteness::Complete
			? TEXT("complete")
			: TEXT("partial");
	}

	FUeAgentSemanticSurface::FUeAgentSemanticSurface(
		FString InSurfaceId,
		EUeAgentSemanticSurfaceKind InKind,
		EUeAgentSemanticSurfaceIntent InIntent,
		EUeAgentSemanticSurfaceCompleteness InCompleteness,
		FString InSourceProvenance,
		TArray<FUeAgentSemanticSurfaceItem> InItems,
		TArray<FUeAgentSemanticSurfaceEndpoint> InEndpoints,
		TArray<FUeAgentSemanticSurfaceRelation> InRelations)
		: SurfaceId(MoveTemp(InSurfaceId))
		, Kind(InKind)
		, Intent(InIntent)
		, Completeness(InCompleteness)
		, SourceProvenance(MoveTemp(InSourceProvenance))
		, Items(MoveTemp(InItems))
		, Endpoints(MoveTemp(InEndpoints))
		, Relations(MoveTemp(InRelations))
	{
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			ItemIndex.Add(Items[Index].Identity, Index);
		}
	}

	const FString& FUeAgentSemanticSurface::GetSurfaceId() const
	{
		return SurfaceId;
	}

	EUeAgentSemanticSurfaceKind FUeAgentSemanticSurface::GetKind() const
	{
		return Kind;
	}

	EUeAgentSemanticSurfaceIntent FUeAgentSemanticSurface::GetIntent() const
	{
		return Intent;
	}

	EUeAgentSemanticSurfaceCompleteness
	FUeAgentSemanticSurface::GetCompleteness() const
	{
		return Completeness;
	}

	const FString& FUeAgentSemanticSurface::GetSourceProvenance() const
	{
		return SourceProvenance;
	}

	const TArray<FUeAgentSemanticSurfaceItem>&
	FUeAgentSemanticSurface::GetItems() const
	{
		return Items;
	}

	const TArray<FUeAgentSemanticSurfaceEndpoint>&
	FUeAgentSemanticSurface::GetEndpoints() const
	{
		return Endpoints;
	}

	const TArray<FUeAgentSemanticSurfaceRelation>&
	FUeAgentSemanticSurface::GetRelations() const
	{
		return Relations;
	}

	const FUeAgentSemanticSurfaceItem* FUeAgentSemanticSurface::FindItem(
		const FString& Identity) const
	{
		const int32* Index = ItemIndex.Find(Identity);
		return Index == nullptr ? nullptr : &Items[*Index];
	}

	FUeAgentSemanticSurfaceBuilder::FUeAgentSemanticSurfaceBuilder(
		FString InSurfaceId,
		EUeAgentSemanticSurfaceKind InKind,
		EUeAgentSemanticSurfaceIntent InIntent,
		EUeAgentSemanticSurfaceCompleteness InCompleteness,
		FString InSourceProvenance)
		: SurfaceId(Normalize(MoveTemp(InSurfaceId)))
		, Kind(InKind)
		, Intent(InIntent)
		, Completeness(InCompleteness)
		, SourceProvenance(Normalize(MoveTemp(InSourceProvenance)))
	{
	}

	bool FUeAgentSemanticSurfaceBuilder::TryAddItem(
		FUeAgentSemanticSurfaceItem Item,
		FString& OutError)
	{
		OutError.Reset();
		Item.Identity = Normalize(MoveTemp(Item.Identity));
		Item.SemanticType = Normalize(MoveTemp(Item.SemanticType));
		Item.OwnerIdentity = Normalize(MoveTemp(Item.OwnerIdentity));
		Item.StableKey = Normalize(MoveTemp(Item.StableKey));
		Item.Slot = Normalize(MoveTemp(Item.Slot));
		if (Item.Identity.IsEmpty()
			|| Item.SemanticType.IsEmpty()
			|| Item.OwnerIdentity.IsEmpty()
			|| Item.StableKey.IsEmpty()
			|| Item.Slot.IsEmpty()
			|| Item.Order < 0)
		{
			return Fail(OutError, TEXT("semantic_surface_item_invalid"));
		}
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
			: Item.Values)
		{
			if (Normalize(Pair.Key).IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("semantic_surface_item_value_key_invalid"));
			}
		}
		if (Items.ContainsByPredicate(
			[&Item](const FUeAgentSemanticSurfaceItem& Existing)
			{
				return Existing.Identity == Item.Identity;
			}))
		{
			return Fail(OutError, TEXT("semantic_surface_item_duplicate"));
		}
		Items.Add(MoveTemp(Item));
		return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryAddEndpoint(
		FUeAgentSemanticSurfaceEndpoint Endpoint,
		FString& OutError)
	{
		OutError.Reset();
		Endpoint.Identity = Normalize(MoveTemp(Endpoint.Identity));
		Endpoint.ItemIdentity = Normalize(MoveTemp(Endpoint.ItemIdentity));
		Endpoint.Role = Normalize(MoveTemp(Endpoint.Role));
		if (Endpoint.Identity.IsEmpty()
			|| Endpoint.ItemIdentity.IsEmpty()
			|| Endpoint.Role.IsEmpty()
			|| Endpoint.Order < 0)
		{
			return Fail(OutError, TEXT("semantic_surface_endpoint_invalid"));
		}
		if (Endpoints.ContainsByPredicate(
			[&Endpoint](const FUeAgentSemanticSurfaceEndpoint& Existing)
			{
				return Existing.Identity == Endpoint.Identity;
			}))
		{
			return Fail(
				OutError,
				TEXT("semantic_surface_endpoint_duplicate"));
		}
		Endpoints.Add(MoveTemp(Endpoint));
		return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryAddRelation(
		FUeAgentSemanticSurfaceRelation Relation,
		FString& OutError)
	{
		OutError.Reset();
		Relation.Identity = Normalize(MoveTemp(Relation.Identity));
		Relation.FromEndpointIdentity =
			Normalize(MoveTemp(Relation.FromEndpointIdentity));
		Relation.ToEndpointIdentity =
			Normalize(MoveTemp(Relation.ToEndpointIdentity));
		Relation.RelationType = Normalize(MoveTemp(Relation.RelationType));
		if (Relation.Identity.IsEmpty()
			|| Relation.FromEndpointIdentity.IsEmpty()
			|| Relation.ToEndpointIdentity.IsEmpty()
			|| Relation.RelationType.IsEmpty()
			|| Relation.Order < 0)
		{
			return Fail(OutError, TEXT("semantic_surface_relation_invalid"));
		}
		if (Relations.ContainsByPredicate(
			[&Relation](const FUeAgentSemanticSurfaceRelation& Existing)
			{
				return Existing.Identity == Relation.Identity;
			}))
		{
			return Fail(
				OutError,
				TEXT("semantic_surface_relation_duplicate"));
		}
		Relations.Add(MoveTemp(Relation));
		return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryBuild(
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticSurfacePtr& OutSurface,
		FString& OutError) const
	{
		OutSurface.Reset();
		OutError.Reset();
		if (SurfaceId.IsEmpty() || SourceProvenance.IsEmpty())
		{
			return Fail(OutError, TEXT("semantic_surface_identity_invalid"));
		}
		if (Completeness == EUeAgentSemanticSurfaceCompleteness::Complete
			&& Items.IsEmpty())
		{
			return Fail(OutError, TEXT("semantic_surface_complete_empty"));
		}

		TSet<FString> ItemIds;
		TSet<FString> StableKeys;
		for (const FUeAgentSemanticSurfaceItem& Item : Items)
		{
			ItemIds.Add(Item.Identity);
			if (StableKeys.Contains(Item.StableKey))
			{
				return Fail(
					OutError,
					TEXT("semantic_surface_stable_key_duplicate"));
			}
			StableKeys.Add(Item.StableKey);
		}
		for (const FUeAgentSemanticSurfaceItem& Item : Items)
		{
			if (Item.OwnerIdentity != SurfaceId
				&& !ItemIds.Contains(Item.OwnerIdentity))
			{
				return Fail(
					OutError,
					Kind == EUeAgentSemanticSurfaceKind::Tree
						? TEXT("semantic_surface_tree_parent_missing")
						: TEXT("semantic_surface_item_owner_missing"));
			}
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Tree
			&& HasOwnerCycle(Items, SurfaceId))
		{
			return Fail(OutError, TEXT("semantic_surface_tree_cycle"));
		}

		TSet<FString> EndpointIds;
		for (const FUeAgentSemanticSurfaceEndpoint& Endpoint : Endpoints)
		{
			if (!ItemIds.Contains(Endpoint.ItemIdentity))
			{
				return Fail(
					OutError,
					TEXT("semantic_surface_endpoint_item_missing"));
			}
			EndpointIds.Add(Endpoint.Identity);
		}
		for (const FUeAgentSemanticSurfaceRelation& Relation : Relations)
		{
			if (!EndpointIds.Contains(Relation.FromEndpointIdentity)
				|| !EndpointIds.Contains(Relation.ToEndpointIdentity))
			{
				return Fail(
					OutError,
					TEXT("semantic_surface_relation_endpoint_missing"));
			}
		}
		if (Kind != EUeAgentSemanticSurfaceKind::Graph
			&& (!Endpoints.IsEmpty() || !Relations.IsEmpty()))
		{
			return Fail(
				OutError,
				TEXT("semantic_surface_relations_not_supported"));
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Timeline)
		{
			for (const FUeAgentSemanticSurfaceItem& Item : Items)
			{
				if (!HasValueKind(Item, EUeAgentValueKind::Temporal))
				{
					return Fail(
						OutError,
						TEXT("semantic_surface_timeline_time_missing"));
				}
			}
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Spatial)
		{
			for (const FUeAgentSemanticSurfaceItem& Item : Items)
			{
				if (!HasValueKind(Item, EUeAgentValueKind::Spatial))
				{
					return Fail(
						OutError,
						TEXT("semantic_surface_spatial_value_missing"));
				}
			}
		}

		FUeAgentResourceBudgetExceeded BudgetError;
		if (!Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Items,
						Items.Num() + Endpoints.Num() + Relations.Num() + 1},
					{EUeAgentResourceDimension::ReferenceEdges,
						Endpoints.Num() + Relations.Num() * 2},
					{EUeAgentResourceDimension::WorkUnits,
						Items.Num() * 2 + Endpoints.Num() + Relations.Num() * 2 + 1},
				},
				TEXT("semantic_surface/build"),
				BudgetError))
		{
			return Fail(
				OutError,
				TEXT("semantic_surface_resource_budget_exceeded"));
		}

		TArray<FUeAgentSemanticSurfaceItem> SortedItems = Items;
		SortedItems.Sort([](
			const FUeAgentSemanticSurfaceItem& Left,
			const FUeAgentSemanticSurfaceItem& Right)
		{
			return Left.Order == Right.Order
				? Left.Identity < Right.Identity
				: Left.Order < Right.Order;
		});
		TArray<FUeAgentSemanticSurfaceEndpoint> SortedEndpoints = Endpoints;
		SortedEndpoints.Sort([](
			const FUeAgentSemanticSurfaceEndpoint& Left,
			const FUeAgentSemanticSurfaceEndpoint& Right)
		{
			return Left.Order == Right.Order
				? Left.Identity < Right.Identity
				: Left.Order < Right.Order;
		});
		TArray<FUeAgentSemanticSurfaceRelation> SortedRelations = Relations;
		SortedRelations.Sort([](
			const FUeAgentSemanticSurfaceRelation& Left,
			const FUeAgentSemanticSurfaceRelation& Right)
		{
			return Left.Order == Right.Order
				? Left.Identity < Right.Identity
				: Left.Order < Right.Order;
		});
		OutSurface = MakeShareable(new FUeAgentSemanticSurface(
			SurfaceId,
			Kind,
			Intent,
			Completeness,
			SourceProvenance,
			MoveTemp(SortedItems),
			MoveTemp(SortedEndpoints),
			MoveTemp(SortedRelations)));
		return true;
	}

	bool TrySerializeSemanticSurface(
		FUeAgentSemanticSurfaceRef Surface,
		TArray<uint8>& OutCanonicalUtf8,
		FString& OutError)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SurfaceSchema);
		Root->SetStringField(TEXT("surface_id"), Surface->GetSurfaceId());
		Root->SetStringField(TEXT("kind"), LexToString(Surface->GetKind()));
		Root->SetStringField(TEXT("intent"), LexToString(Surface->GetIntent()));
		Root->SetStringField(
			TEXT("completeness"),
			LexToString(Surface->GetCompleteness()));
		Root->SetStringField(
			TEXT("source_provenance"),
			Surface->GetSourceProvenance());

		TArray<TSharedPtr<FJsonValue>> ItemValues;
		for (const FUeAgentSemanticSurfaceItem& Item : Surface->GetItems())
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("identity"), Item.Identity);
			Object->SetStringField(TEXT("semantic_type"), Item.SemanticType);
			Object->SetStringField(TEXT("owner_identity"), Item.OwnerIdentity);
			Object->SetStringField(TEXT("stable_key"), Item.StableKey);
			Object->SetStringField(TEXT("slot"), Item.Slot);
			Object->SetStringField(
				TEXT("order"),
				FString::Printf(TEXT("%lld"), Item.Order));
			const TSharedRef<FJsonObject> JsonValues =
				ValuesToJson(Item.Values, OutError);
			if (!OutError.IsEmpty())
			{
				return false;
			}
			Object->SetObjectField(TEXT("values"), JsonValues);
			ItemValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("items"), MoveTemp(ItemValues));

		TArray<TSharedPtr<FJsonValue>> EndpointValues;
		for (const FUeAgentSemanticSurfaceEndpoint& Endpoint
			: Surface->GetEndpoints())
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("identity"), Endpoint.Identity);
			Object->SetStringField(TEXT("item_identity"), Endpoint.ItemIdentity);
			Object->SetStringField(TEXT("role"), Endpoint.Role);
			Object->SetStringField(
				TEXT("order"),
				FString::Printf(TEXT("%lld"), Endpoint.Order));
			EndpointValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("endpoints"), MoveTemp(EndpointValues));

		TArray<TSharedPtr<FJsonValue>> RelationValues;
		for (const FUeAgentSemanticSurfaceRelation& Relation
			: Surface->GetRelations())
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("identity"), Relation.Identity);
			Object->SetStringField(
				TEXT("from_endpoint_identity"),
				Relation.FromEndpointIdentity);
			Object->SetStringField(
				TEXT("to_endpoint_identity"),
				Relation.ToEndpointIdentity);
			Object->SetStringField(TEXT("relation_type"), Relation.RelationType);
			Object->SetStringField(
				TEXT("order"),
				FString::Printf(TEXT("%lld"), Relation.Order));
			RelationValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Root->SetArrayField(TEXT("relations"), MoveTemp(RelationValues));

		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			Writer = TJsonWriterFactory<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			return Fail(OutError, TEXT("semantic_surface_serialize_failed"));
		}
		const FTCHARToUTF8 Utf8(*Text);
		OutCanonicalUtf8.Append(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length());
		return true;
	}

	bool TryParseSemanticSurface(
		const TArray<uint8>& CanonicalUtf8,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticSurfacePtr& OutSurface,
		FString& OutError)
	{
		OutSurface.Reset();
		OutError.Reset();
		if (CanonicalUtf8.IsEmpty())
		{
			return Fail(OutError, TEXT("semantic_surface_document_empty"));
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(CanonicalUtf8.GetData()),
			CanonicalUtf8.Num());
		const FString Text(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return Fail(OutError, TEXT("semantic_surface_document_invalid"));
		}
		FString Schema;
		FString SurfaceId;
		FString KindText;
		FString IntentText;
		FString CompletenessText;
		FString SourceProvenance;
		if (!Root->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != SurfaceSchema
			|| !Root->TryGetStringField(TEXT("surface_id"), SurfaceId)
			|| !Root->TryGetStringField(TEXT("kind"), KindText)
			|| !Root->TryGetStringField(TEXT("intent"), IntentText)
			|| !Root->TryGetStringField(TEXT("completeness"), CompletenessText)
			|| !Root->TryGetStringField(
				TEXT("source_provenance"),
				SourceProvenance))
		{
			return Fail(OutError, TEXT("semantic_surface_header_invalid"));
		}
		EUeAgentSemanticSurfaceKind Kind;
		EUeAgentSemanticSurfaceIntent Intent;
		EUeAgentSemanticSurfaceCompleteness Completeness;
		if (!ParseKind(KindText, Kind)
			|| !ParseIntent(IntentText, Intent)
			|| !ParseCompleteness(CompletenessText, Completeness))
		{
			return Fail(OutError, TEXT("semantic_surface_enum_invalid"));
		}
		FUeAgentSemanticSurfaceBuilder Builder(
			SurfaceId,
			Kind,
			Intent,
			Completeness,
			SourceProvenance);

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Endpoints = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Relations = nullptr;
		if (!Root->TryGetArrayField(TEXT("items"), Items)
			|| !Root->TryGetArrayField(TEXT("endpoints"), Endpoints)
			|| !Root->TryGetArrayField(TEXT("relations"), Relations)
			|| Items == nullptr
			|| Endpoints == nullptr
			|| Relations == nullptr)
		{
			return Fail(OutError, TEXT("semantic_surface_collections_missing"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FUeAgentSemanticSurfaceItem Item;
			FString Order;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(TEXT("identity"), Item.Identity)
				|| !Object->TryGetStringField(
					TEXT("semantic_type"),
					Item.SemanticType)
				|| !Object->TryGetStringField(
					TEXT("owner_identity"),
					Item.OwnerIdentity)
				|| !Object->TryGetStringField(TEXT("stable_key"), Item.StableKey)
				|| !Object->TryGetStringField(TEXT("slot"), Item.Slot)
				|| !Object->TryGetStringField(TEXT("order"), Order)
				|| !LexTryParseString(Item.Order, *Order)
				|| !TryReadValues(Object, Resources, Item.Values, OutError)
				|| !Builder.TryAddItem(MoveTemp(Item), OutError))
			{
				return OutError.IsEmpty()
					? Fail(OutError, TEXT("semantic_surface_item_document_invalid"))
					: false;
			}
		}
		for (const TSharedPtr<FJsonValue>& Value : *Endpoints)
		{
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FUeAgentSemanticSurfaceEndpoint Endpoint;
			FString Order;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(TEXT("identity"), Endpoint.Identity)
				|| !Object->TryGetStringField(
					TEXT("item_identity"),
					Endpoint.ItemIdentity)
				|| !Object->TryGetStringField(TEXT("role"), Endpoint.Role)
				|| !Object->TryGetStringField(TEXT("order"), Order)
				|| !LexTryParseString(Endpoint.Order, *Order)
				|| !Builder.TryAddEndpoint(MoveTemp(Endpoint), OutError))
			{
				return OutError.IsEmpty()
					? Fail(OutError, TEXT("semantic_surface_endpoint_document_invalid"))
					: false;
			}
		}
		for (const TSharedPtr<FJsonValue>& Value : *Relations)
		{
			const TSharedPtr<FJsonObject> Object =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FUeAgentSemanticSurfaceRelation Relation;
			FString Order;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(TEXT("identity"), Relation.Identity)
				|| !Object->TryGetStringField(
					TEXT("from_endpoint_identity"),
					Relation.FromEndpointIdentity)
				|| !Object->TryGetStringField(
					TEXT("to_endpoint_identity"),
					Relation.ToEndpointIdentity)
				|| !Object->TryGetStringField(
					TEXT("relation_type"),
					Relation.RelationType)
				|| !Object->TryGetStringField(TEXT("order"), Order)
				|| !LexTryParseString(Relation.Order, *Order)
				|| !Builder.TryAddRelation(MoveTemp(Relation), OutError))
			{
				return OutError.IsEmpty()
					? Fail(OutError, TEXT("semantic_surface_relation_document_invalid"))
					: false;
			}
		}
		return Builder.TryBuild(Resources, OutSurface, OutError);
	}

	bool TryBuildSemanticSurfaceSnapshot(
		FUeAgentSemanticSurfaceRef Surface,
		const FString& SnapshotId,
		Contract::EUeAgentStatePlane StatePlane,
		const FUeAgentStateRevision& Revision,
		const FUeAgentTemporalPoint& CapturedAt,
		const FUeAgentStateProvenance& Provenance,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FUeAgentStateSemanticError& OutError)
	{
		OutSnapshot.Reset();
		OutError = {};
		TArray<FUeAgentStateEntry> Entries;
		TMap<FString, FUeAgentCanonicalValueRef> Fields;
		if (!TryAddStringField(
				Fields,
				TEXT("kind"),
				LexToString(Surface->GetKind()),
				Resources,
				OutError)
			|| !TryAddStringField(
				Fields,
				TEXT("intent"),
				LexToString(Surface->GetIntent()),
				Resources,
				OutError)
			|| !TryAddStringField(
				Fields,
				TEXT("completeness"),
				LexToString(Surface->GetCompleteness()),
				Resources,
				OutError)
			|| !TryAddStringField(
				Fields,
				TEXT("source_provenance"),
				Surface->GetSourceProvenance(),
				Resources,
				OutError)
			|| !TryAddSnapshotEntry(
				*Surface,
				Surface->GetSurfaceId(),
				TEXT("surface"),
				Surface->GetSurfaceId(),
				TEXT("semantic_surface.meta"),
				Fields,
				Revision,
				Resources,
				Entries,
				OutError))
		{
			return false;
		}
		for (const FUeAgentSemanticSurfaceItem& Item : Surface->GetItems())
		{
			Fields = Item.Values;
			FUeAgentCanonicalValuePtr NestedValues;
			if (!TryObjectValue(Item.Values, Resources, NestedValues, OutError))
			{
				return false;
			}
			Fields.Reset();
			Fields.Add(TEXT("values"), NestedValues.ToSharedRef());
			if (!TryAddStringField(Fields, TEXT("identity"), Item.Identity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("semantic_type"), Item.SemanticType, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("owner_identity"), Item.OwnerIdentity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("stable_key"), Item.StableKey, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("slot"), Item.Slot, Resources, OutError)
				|| !TryAddIntegerField(Fields, TEXT("order"), Item.Order, Resources, OutError)
				|| !TryAddSnapshotEntry(
					*Surface,
					Item.Identity,
					TEXT("item"),
					Item.StableKey,
					TEXT("semantic_surface.item"),
					Fields,
					Revision,
					Resources,
					Entries,
					OutError))
			{
				return false;
			}
		}
		for (const FUeAgentSemanticSurfaceEndpoint& Endpoint
			: Surface->GetEndpoints())
		{
			Fields.Reset();
			if (!TryAddStringField(Fields, TEXT("identity"), Endpoint.Identity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("item_identity"), Endpoint.ItemIdentity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("role"), Endpoint.Role, Resources, OutError)
				|| !TryAddIntegerField(Fields, TEXT("order"), Endpoint.Order, Resources, OutError)
				|| !TryAddSnapshotEntry(
					*Surface,
					Endpoint.Identity,
					TEXT("endpoint"),
					Endpoint.Identity,
					TEXT("semantic_surface.endpoint"),
					Fields,
					Revision,
					Resources,
					Entries,
					OutError))
			{
				return false;
			}
		}
		for (const FUeAgentSemanticSurfaceRelation& Relation
			: Surface->GetRelations())
		{
			Fields.Reset();
			if (!TryAddStringField(Fields, TEXT("identity"), Relation.Identity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("from_endpoint_identity"), Relation.FromEndpointIdentity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("to_endpoint_identity"), Relation.ToEndpointIdentity, Resources, OutError)
				|| !TryAddStringField(Fields, TEXT("relation_type"), Relation.RelationType, Resources, OutError)
				|| !TryAddIntegerField(Fields, TEXT("order"), Relation.Order, Resources, OutError)
				|| !TryAddSnapshotEntry(
					*Surface,
					Relation.Identity,
					TEXT("relation"),
					Relation.Identity,
					TEXT("semantic_surface.relation"),
					Fields,
					Revision,
					Resources,
					Entries,
					OutError))
			{
				return false;
			}
		}
		return FUeAgentCanonicalSnapshot::TryCreate(
			SnapshotId,
			StatePlane,
			Revision,
			CapturedAt,
			Provenance,
			Entries,
			Resources,
			OutSnapshot,
			OutError);
	}

	bool TryCompareSemanticSurfaces(
		FUeAgentSemanticSurfaceRef Left,
		FUeAgentSemanticSurfaceRef Right,
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticComparison& OutComparison,
		FUeAgentStateSemanticError& OutError)
	{
		const FUeAgentStateRevision Revision(
			TEXT("semantic-surface-comparator"),
			TEXT("semantic-surface-v1"),
			0);
		const FUeAgentTemporalPoint CapturedAt(
			TEXT("semantic-surface-comparator"),
			TEXT("0"));
		const FUeAgentStateProvenance Provenance(
			TEXT("independent_readback"),
			TEXT("semantic-surface-comparator"),
			TEXT("semantic-surface-comparator-v1"));
		FUeAgentCanonicalSnapshotPtr LeftSnapshot;
		FUeAgentCanonicalSnapshotPtr RightSnapshot;
		if (!TryBuildSemanticSurfaceSnapshot(
				Left,
				TEXT("semantic-surface-comparison"),
				Contract::EUeAgentStatePlane::AuthoringMemory,
				Revision,
				CapturedAt,
				Provenance,
				Resources,
				LeftSnapshot,
				OutError)
			|| !TryBuildSemanticSurfaceSnapshot(
				Right,
				TEXT("semantic-surface-comparison"),
				Contract::EUeAgentStatePlane::AuthoringMemory,
				Revision,
				CapturedAt,
				Provenance,
				Resources,
				RightSnapshot,
				OutError))
		{
			return false;
		}
		return TryCompareSnapshots(
			LeftSnapshot.ToSharedRef(),
			RightSnapshot.ToSharedRef(),
			Resources,
			OutComparison,
			OutError);
	}

}
