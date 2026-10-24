#include "Core/StateSemantic/UeAgentSemanticSurface.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentContractJson.h"

#include "Containers/StringConv.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <initializer_list>

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

		bool Fail(Contract::FDiagnostic& OutError, const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Expected = nullptr, TSharedPtr<FJsonValue> Actual = nullptr) {
			OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("semantic_surface"));
			OutError.Source.JsonPath = Path; OutError.SemanticPointer = Path;
			OutError.ExpectedValue = MoveTemp(Expected); OutError.ActualValue = MoveTemp(Actual);
			return false;
		}
		FString PointerChild(const FString& Parent, const FString& Child) {
			return (Parent == TEXT("/") ? FString() : Parent) + TEXT("/") + Child.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}
		void PrefixFailure(Contract::FDiagnostic& Error, const FString& Prefix) {
			if (Error.Source.SourceFile.IsEmpty()) {
				auto PrefixPath = [&](FString& Path) { Path = Prefix + (Path == TEXT("/") ? FString() : Path); };
				PrefixPath(Error.Source.JsonPath);
				if (!Error.Source.InternalJsonPath.IsEmpty()) PrefixPath(Error.Source.InternalJsonPath);
				if (!Error.SemanticPointer.IsEmpty()) PrefixPath(Error.SemanticPointer);
			}
			for (auto& Child : Error.Diagnostics) PrefixFailure(Child, Prefix);
			if (Error.PublicCause) { auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause); PrefixFailure(*Cause, Prefix); Error.PublicCause = MoveTemp(Cause); }
		}
		bool PrivateReferenceFailure(Contract::FDiagnostic& Error, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, const FString& Reference) {
			Fail(Error, Code, Reason, Path, MakeShared<FJsonValueString>(TEXT("valid unambiguous reference")));
			Error.ActualValuePresent = !Reference.IsEmpty(); Error.ActualEvidence = MakeShared<FJsonValueString>(Reference); return false;
		}
		bool RequireNormalizedText(FString& Value, const TCHAR* Code, const FString& Path, bool bPrivate, Contract::FDiagnostic& Error) {
			FString Normalized = Normalize(Value);
			if (Normalized.IsEmpty()) {
				Fail(Error, Code, TEXT("The semantic surface field requires non-empty text."), Path, MakeShared<FJsonValueString>(TEXT("non-empty string")));
				if (bPrivate) Error.ActualEvidence = MakeShared<FJsonValueString>(Value); else Error.ActualValue = MakeShared<FJsonValueString>(Value);
				return false;
			}
			Value = MoveTemp(Normalized); return true;
		}
		FString JsonType(const TSharedPtr<FJsonValue>& Value) {
			if (!Value) return TEXT("missing");
			switch (Value->Type) {
			case EJson::Null: return TEXT("null");
			case EJson::String: return TEXT("string");
			case EJson::Number: return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array: return TEXT("array");
			case EJson::Object: return TEXT("object");
			default: return TEXT("none");
			}
		}
		bool FieldTypeFailure(Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Path,
			const TCHAR* ExpectedType, const TSharedPtr<FJsonValue>& Actual) {
			Fail(Error, Code, TEXT("The semantic surface field is missing or has an incompatible JSON type."), Path,
				MakeShared<FJsonValueString>(ExpectedType), MakeShared<FJsonValueString>(JsonType(Actual)));
			Error.ExpectedValuePresent = true; Error.ActualValuePresent = Actual.IsValid(); return false;
		}
		bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& OutValue,
			const TCHAR* Code, const FString& Parent, Contract::FDiagnostic& Error) {
			const TSharedPtr<FJsonValue> Value = Object ? Object->TryGetField(Field) : nullptr;
			if (!Value || Value->Type != EJson::String)
				return FieldTypeFailure(Error, Code, PointerChild(Parent, Field), TEXT("string"), Value);
			OutValue = Value->AsString(); return true;
		}
		bool ReadOrder(const TSharedPtr<FJsonObject>& Object, int64& OutOrder, const TCHAR* Code,
			const FString& Parent, Contract::FDiagnostic& Error) {
			FString Text;
			if (!ReadString(Object, TEXT("order"), Text, Code, Parent, Error)) return false;
			if (!LexTryParseString(OutOrder, *Text))
				return Fail(Error, Code, TEXT("The semantic surface order is not a valid signed 64-bit integer string."),
					PointerChild(Parent, TEXT("order")), MakeShared<FJsonValueString>(TEXT("int64 string")), MakeShared<FJsonValueString>(Text));
			return true;
		}
		bool ReadArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			const TArray<TSharedPtr<FJsonValue>>*& OutValues, Contract::FDiagnostic& Error) {
			if (Object && Object->TryGetArrayField(Field, OutValues) && OutValues) return true;
			return FieldTypeFailure(Error, TEXT("semantic_surface_collections_missing"), PointerChild(TEXT("/"), Field),
				TEXT("array"), Object ? Object->TryGetField(Field) : nullptr);
		}
		template<typename EnumType>
		bool EnumFailure(Contract::FDiagnostic& Error, const FString& Path, const FString& Actual,
			std::initializer_list<EnumType> Allowed) {
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const EnumType Value : Allowed) Expected.Add(MakeShared<FJsonValueString>(LexToString(Value)));
			return Fail(Error, TEXT("semantic_surface_enum_invalid"), TEXT("The semantic surface enum value is not supported."), Path,
				MakeShared<FJsonValueArray>(MoveTemp(Expected)), MakeShared<FJsonValueString>(Actual));
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

		bool TryValidateOwnerCycles(
			const TArray<FUeAgentSemanticSurfaceItem>& Items,
			const FString& SurfaceId, Contract::FDiagnostic& Error)
		{
			TMap<FString, FString> Owners;
			for (const FUeAgentSemanticSurfaceItem& Item : Items)
			{
				Owners.Add(Item.Identity, Item.OwnerIdentity);
			}
			for (int32 StartIndex = 0; StartIndex < Items.Num(); ++StartIndex)
			{
				const auto& Start = Items[StartIndex];
				TSet<FString> Seen;
				FString Current = Start.Identity;
				while (Current != SurfaceId)
				{
					if (Seen.Contains(Current))
					{
						return PrivateReferenceFailure(Error, TEXT("semantic_surface_tree_cycle"), TEXT("The item's owner chain forms a cycle."),
							FString::Printf(TEXT("/items/%d/owner_identity"), StartIndex), Current);
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
			return true;
		}

		TSharedRef<FJsonObject> ValuesToJson(
			const TMap<FString, FUeAgentCanonicalValueRef>& Values,
			UE::UAI::Contract::FDiagnostic& OutError)
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
					PrefixFailure(OutError, PointerChild(TEXT("/values"), Key));
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValues.Reset();
			const TSharedPtr<FJsonObject>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(TEXT("values"), Values)
				|| Values == nullptr
				|| !Values->IsValid())
			{
				return FieldTypeFailure(OutError, TEXT("semantic_surface_values_missing"), TEXT("/values"), TEXT("object"),
					Object ? Object->TryGetField(TEXT("values")) : nullptr);
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
					PrefixFailure(OutError, PointerChild(TEXT("/values"), Key)); return false;
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
				OutError = FUeAgentStateSemanticError::Failure(TEXT("semantic_surface_snapshot_entry_invalid"), TEXT("The semantic surface snapshot entry is invalid."), TEXT("state_semantic"));
				OutError.Source.JsonPath = Identity;
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

	bool FUeAgentSemanticSurfaceBuilder::TryAddItem(FUeAgentSemanticSurfaceItem Item, Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString Path = TEXT("/items/") + FString::FromInt(Items.Num());
		if (!RequireNormalizedText(Item.Identity, TEXT("semantic_surface_item_invalid"), Path + TEXT("/identity"), true, OutError)
			|| !RequireNormalizedText(Item.SemanticType, TEXT("semantic_surface_item_invalid"), Path + TEXT("/semantic_type"), false, OutError)
			|| !RequireNormalizedText(Item.OwnerIdentity, TEXT("semantic_surface_item_invalid"), Path + TEXT("/owner_identity"), true, OutError)
			|| !RequireNormalizedText(Item.StableKey, TEXT("semantic_surface_item_invalid"), Path + TEXT("/stable_key"), true, OutError)
			|| !RequireNormalizedText(Item.Slot, TEXT("semantic_surface_item_invalid"), Path + TEXT("/slot"), false, OutError)) return false;
		if (Item.Order < 0) return Fail(OutError, TEXT("semantic_surface_item_invalid"), TEXT("The semantic surface order must be non-negative."), Path + TEXT("/order"),
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Item.Order)));
		for (const auto& Pair : Item.Values)
			if (Normalize(Pair.Key).IsEmpty()) return Fail(OutError, TEXT("semantic_surface_item_value_key_invalid"),
				TEXT("A semantic value member key must contain non-whitespace text."), Path + TEXT("/values"),
				MakeShared<FJsonValueString>(TEXT("non-empty value key")), MakeShared<FJsonValueString>(Pair.Key));
		if (Items.ContainsByPredicate([&](const auto& Existing) { return Existing.Identity == Item.Identity; }))
			return PrivateReferenceFailure(OutError, TEXT("semantic_surface_item_duplicate"), TEXT("The same item identity occurs more than once."), Path + TEXT("/identity"), Item.Identity);
		Items.Add(MoveTemp(Item)); return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryAddEndpoint(FUeAgentSemanticSurfaceEndpoint Endpoint, Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString Path = TEXT("/endpoints/") + FString::FromInt(Endpoints.Num());
		if (!RequireNormalizedText(Endpoint.Identity, TEXT("semantic_surface_endpoint_invalid"), Path + TEXT("/identity"), true, OutError)
			|| !RequireNormalizedText(Endpoint.ItemIdentity, TEXT("semantic_surface_endpoint_invalid"), Path + TEXT("/item_identity"), true, OutError)
			|| !RequireNormalizedText(Endpoint.Role, TEXT("semantic_surface_endpoint_invalid"), Path + TEXT("/role"), false, OutError)) return false;
		if (Endpoint.Order < 0) return Fail(OutError, TEXT("semantic_surface_endpoint_invalid"), TEXT("The semantic surface order must be non-negative."), Path + TEXT("/order"),
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Endpoint.Order)));
		if (Endpoints.ContainsByPredicate([&](const auto& Existing) { return Existing.Identity == Endpoint.Identity; }))
			return PrivateReferenceFailure(OutError, TEXT("semantic_surface_endpoint_duplicate"), TEXT("The same endpoint identity occurs more than once."), Path + TEXT("/identity"), Endpoint.Identity);
		Endpoints.Add(MoveTemp(Endpoint)); return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryAddRelation(FUeAgentSemanticSurfaceRelation Relation, Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FString Path = TEXT("/relations/") + FString::FromInt(Relations.Num());
		if (!RequireNormalizedText(Relation.Identity, TEXT("semantic_surface_relation_invalid"), Path + TEXT("/identity"), true, OutError)
			|| !RequireNormalizedText(Relation.FromEndpointIdentity, TEXT("semantic_surface_relation_invalid"), Path + TEXT("/from_endpoint_identity"), true, OutError)
			|| !RequireNormalizedText(Relation.ToEndpointIdentity, TEXT("semantic_surface_relation_invalid"), Path + TEXT("/to_endpoint_identity"), true, OutError)
			|| !RequireNormalizedText(Relation.RelationType, TEXT("semantic_surface_relation_invalid"), Path + TEXT("/relation_type"), false, OutError)) return false;
		if (Relation.Order < 0) return Fail(OutError, TEXT("semantic_surface_relation_invalid"), TEXT("The semantic surface order must be non-negative."), Path + TEXT("/order"),
			MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Relation.Order)));
		if (Relations.ContainsByPredicate([&](const auto& Existing) { return Existing.Identity == Relation.Identity; }))
			return PrivateReferenceFailure(OutError, TEXT("semantic_surface_relation_duplicate"), TEXT("The same relation identity occurs more than once."), Path + TEXT("/identity"), Relation.Identity);
		Relations.Add(MoveTemp(Relation)); return true;
	}

	bool FUeAgentSemanticSurfaceBuilder::TryBuild(
		FUeAgentResourceTracker& Resources,
		FUeAgentSemanticSurfacePtr& OutSurface,
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutSurface.Reset();
		OutError.Reset();
		if (SurfaceId.IsEmpty() || SourceProvenance.IsEmpty())
		{
			return PrivateReferenceFailure(OutError, TEXT("semantic_surface_identity_invalid"), SurfaceId.IsEmpty() ? TEXT("The semantic surface identifier must be non-empty.") : TEXT("The semantic surface source provenance must be non-empty."), SurfaceId.IsEmpty() ? TEXT("/surface_id") : TEXT("/source_provenance"), SurfaceId.IsEmpty() ? SurfaceId : SourceProvenance);
		}
		if (Completeness == EUeAgentSemanticSurfaceCompleteness::Complete
			&& Items.IsEmpty())
		{
			return Fail(OutError, TEXT("semantic_surface_complete_empty"), TEXT("A complete semantic surface must contain at least one item."), TEXT("/items"), MakeShared<FJsonValueString>(TEXT("at least one item")), MakeShared<FJsonValueNumber>(Items.Num()));
		}

		TSet<FString> ItemIds;
		TSet<FString> StableKeys;
		for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
		{
			const auto& Item = Items[ItemIndex];
			ItemIds.Add(Item.Identity);
			if (StableKeys.Contains(Item.StableKey))
			{
				return PrivateReferenceFailure(OutError, TEXT("semantic_surface_stable_key_duplicate"), TEXT("The same stable key is assigned to more than one semantic item."),
					FString::Printf(TEXT("/items/%d/stable_key"), ItemIndex), Item.StableKey);
			}
			StableKeys.Add(Item.StableKey);
		}
		for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
		{
			const auto& Item = Items[ItemIndex];
			if (Item.OwnerIdentity != SurfaceId
				&& !ItemIds.Contains(Item.OwnerIdentity))
			{
				return PrivateReferenceFailure(OutError, Kind == EUeAgentSemanticSurfaceKind::Tree ? TEXT("semantic_surface_tree_parent_missing") : TEXT("semantic_surface_item_owner_missing"),
					TEXT("The item owner is neither the surface nor an existing item."), FString::Printf(TEXT("/items/%d/owner_identity"), ItemIndex), Item.OwnerIdentity);
			}
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Tree && !TryValidateOwnerCycles(Items, SurfaceId, OutError)) return false;

		TSet<FString> EndpointIds;
		for (int32 Index = 0; Index < Endpoints.Num(); ++Index)
		{
			const auto& Endpoint = Endpoints[Index];
			if (!ItemIds.Contains(Endpoint.ItemIdentity))
			{
				return PrivateReferenceFailure(OutError, TEXT("semantic_surface_endpoint_item_missing"), TEXT("The endpoint refers to an item that is absent from the surface."),
					FString::Printf(TEXT("/endpoints/%d/item_identity"), Index), Endpoint.ItemIdentity);
			}
			EndpointIds.Add(Endpoint.Identity);
		}
		for (int32 Index = 0; Index < Relations.Num(); ++Index)
		{
			const auto& Relation = Relations[Index];
			if (!EndpointIds.Contains(Relation.FromEndpointIdentity)
				|| !EndpointIds.Contains(Relation.ToEndpointIdentity))
			{
				const bool bFrom = !EndpointIds.Contains(Relation.FromEndpointIdentity);
				return PrivateReferenceFailure(OutError, TEXT("semantic_surface_relation_endpoint_missing"), TEXT("The relation refers to an endpoint that is absent from the surface."),
					FString::Printf(TEXT("/relations/%d/%s"), Index, bFrom ? TEXT("from_endpoint_identity") : TEXT("to_endpoint_identity")), bFrom ? Relation.FromEndpointIdentity : Relation.ToEndpointIdentity);
			}
		}
		if (Kind != EUeAgentSemanticSurfaceKind::Graph
			&& (!Endpoints.IsEmpty() || !Relations.IsEmpty()))
		{
			return Fail(OutError, TEXT("semantic_surface_relations_not_supported"), TEXT("Only graph surfaces support endpoints and relations."), TEXT("/kind"),
				MakeShared<FJsonValueString>(LexToString(EUeAgentSemanticSurfaceKind::Graph)), MakeShared<FJsonValueString>(LexToString(Kind)));
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Timeline)
		{
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				const auto& Item = Items[ItemIndex];
				if (!HasValueKind(Item, EUeAgentValueKind::Temporal))
				{
					Fail(OutError, TEXT("semantic_surface_timeline_time_missing"), TEXT("This semantic item requires a temporal value."), FString::Printf(TEXT("/items/%d/values"), ItemIndex),
						MakeShared<FJsonValueString>(TEXT("temporal value")));
					OutError.CandidateName = Item.SemanticType; OutError.ActualValuePresent = false; return false;
				}
			}
		}
		if (Kind == EUeAgentSemanticSurfaceKind::Spatial)
		{
			for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
			{
				const auto& Item = Items[ItemIndex];
				if (!HasValueKind(Item, EUeAgentValueKind::Spatial))
				{
					Fail(OutError, TEXT("semantic_surface_spatial_value_missing"), TEXT("This semantic item requires a spatial value."), FString::Printf(TEXT("/items/%d/values"), ItemIndex),
						MakeShared<FJsonValueString>(TEXT("spatial value")));
					OutError.CandidateName = Item.SemanticType; OutError.ActualValuePresent = false; return false;
				}
			}
		}

		FUeAgentResourceChargeError ChargeError;
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
				ChargeError))
		{
			OutError = Contract::FDiagnostic::WrapFailure(TEXT("semantic_surface_resource_accounting_failed"), ChargeError, TEXT("semantic_surface")); return false;
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		for (int32 ItemIndex = 0; ItemIndex < Surface->GetItems().Num(); ++ItemIndex)
		{
			const FUeAgentSemanticSurfaceItem& Item = Surface->GetItems()[ItemIndex];
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
				PrefixFailure(OutError, FString::Printf(TEXT("/items/%d"), ItemIndex));
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
			return Fail(OutError, TEXT("semantic_surface_serialize_failed"), TEXT("The semantic surface JSON writer could not serialize the constructed document."), TEXT("/"));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSurface.Reset();
		OutError.Reset();
		if (CanonicalUtf8.IsEmpty())
		{
			return Fail(OutError, TEXT("semantic_surface_document_empty"), TEXT("The semantic surface document contains no UTF-8 bytes."), TEXT("/"), MakeShared<FJsonValueString>(TEXT("non-empty document")), MakeShared<FJsonValueNumber>(CanonicalUtf8.Num()));
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(CanonicalUtf8.GetData()),
			CanonicalUtf8.Num());
		const FString Text(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Root;
		UE::UAI::Contract::FDiagnostic ParseError;
		if (!Contract::Json::TryParseObject(Text, Root, ParseError))
		{
			OutError = Contract::FDiagnostic::WrapFailure(TEXT("semantic_surface_document_invalid"), ParseError, TEXT("semantic_surface")); return false;
		}
		FString Schema;
		FString SurfaceId;
		FString KindText;
		FString IntentText;
		FString CompletenessText;
		FString SourceProvenance;
		const TCHAR* HeaderCode = TEXT("semantic_surface_header_invalid");
		if (!ReadString(Root, TEXT("schema"), Schema, HeaderCode, TEXT("/"), OutError)) return false;
		if (Schema != SurfaceSchema)
			return Fail(OutError, HeaderCode, TEXT("The semantic surface schema version is not supported."), TEXT("/schema"),
				MakeShared<FJsonValueString>(SurfaceSchema), MakeShared<FJsonValueString>(Schema));
		if (!ReadString(Root, TEXT("surface_id"), SurfaceId, HeaderCode, TEXT("/"), OutError)
			|| !ReadString(Root, TEXT("kind"), KindText, HeaderCode, TEXT("/"), OutError)
			|| !ReadString(Root, TEXT("intent"), IntentText, HeaderCode, TEXT("/"), OutError)
			|| !ReadString(Root, TEXT("completeness"), CompletenessText, HeaderCode, TEXT("/"), OutError)
			|| !ReadString(Root, TEXT("source_provenance"), SourceProvenance, HeaderCode, TEXT("/"), OutError)) return false;
		EUeAgentSemanticSurfaceKind Kind;
		EUeAgentSemanticSurfaceIntent Intent;
		EUeAgentSemanticSurfaceCompleteness Completeness;
		if (!ParseKind(KindText, Kind))
			return EnumFailure(OutError, TEXT("/kind"), KindText, {
				EUeAgentSemanticSurfaceKind::PropertyCollection, EUeAgentSemanticSurfaceKind::Graph,
				EUeAgentSemanticSurfaceKind::Tree, EUeAgentSemanticSurfaceKind::Timeline,
				EUeAgentSemanticSurfaceKind::KeyedTable, EUeAgentSemanticSurfaceKind::Spatial,
				EUeAgentSemanticSurfaceKind::ExternalProvenance});
		if (!ParseIntent(IntentText, Intent))
			return EnumFailure(OutError, TEXT("/intent"), IntentText, {
				EUeAgentSemanticSurfaceIntent::FullState, EUeAgentSemanticSurfaceIntent::MinimalCreate, EUeAgentSemanticSurfaceIntent::LocalEdit});
		if (!ParseCompleteness(CompletenessText, Completeness))
			return EnumFailure(OutError, TEXT("/completeness"), CompletenessText, {
				EUeAgentSemanticSurfaceCompleteness::Complete, EUeAgentSemanticSurfaceCompleteness::Partial});
		FUeAgentSemanticSurfaceBuilder Builder(
			SurfaceId,
			Kind,
			Intent,
			Completeness,
			SourceProvenance);

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Endpoints = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Relations = nullptr;
		if (!ReadArray(Root, TEXT("items"), Items, OutError)
			|| !ReadArray(Root, TEXT("endpoints"), Endpoints, OutError)
			|| !ReadArray(Root, TEXT("relations"), Relations, OutError)) return false;
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Items)[Index];
			const FString Path = FString::Printf(TEXT("/items/%d"), Index);
			const TCHAR* Code = TEXT("semantic_surface_item_document_invalid");
			const TSharedPtr<FJsonObject> Object =
				Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Object) return FieldTypeFailure(OutError, Code, Path, TEXT("object"), Value);
			FUeAgentSemanticSurfaceItem Item;
			if (!ReadString(Object, TEXT("identity"), Item.Identity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("semantic_type"), Item.SemanticType, Code, Path, OutError)
				|| !ReadString(Object, TEXT("owner_identity"), Item.OwnerIdentity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("stable_key"), Item.StableKey, Code, Path, OutError)
				|| !ReadString(Object, TEXT("slot"), Item.Slot, Code, Path, OutError)
				|| !ReadOrder(Object, Item.Order, Code, Path, OutError)) return false;
			if (!TryReadValues(Object, Resources, Item.Values, OutError)) { PrefixFailure(OutError, Path); return false; }
			if (!Builder.TryAddItem(MoveTemp(Item), OutError)) return false;
		}
		for (int32 Index = 0; Index < Endpoints->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Endpoints)[Index];
			const FString Path = FString::Printf(TEXT("/endpoints/%d"), Index);
			const TCHAR* Code = TEXT("semantic_surface_endpoint_document_invalid");
			const TSharedPtr<FJsonObject> Object =
				Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Object) return FieldTypeFailure(OutError, Code, Path, TEXT("object"), Value);
			FUeAgentSemanticSurfaceEndpoint Endpoint;
			if (!ReadString(Object, TEXT("identity"), Endpoint.Identity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("item_identity"), Endpoint.ItemIdentity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("role"), Endpoint.Role, Code, Path, OutError)
				|| !ReadOrder(Object, Endpoint.Order, Code, Path, OutError)
				|| !Builder.TryAddEndpoint(MoveTemp(Endpoint), OutError)) return false;
		}
		for (int32 Index = 0; Index < Relations->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Relations)[Index];
			const FString Path = FString::Printf(TEXT("/relations/%d"), Index);
			const TCHAR* Code = TEXT("semantic_surface_relation_document_invalid");
			const TSharedPtr<FJsonObject> Object =
				Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Object) return FieldTypeFailure(OutError, Code, Path, TEXT("object"), Value);
			FUeAgentSemanticSurfaceRelation Relation;
			if (!ReadString(Object, TEXT("identity"), Relation.Identity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("from_endpoint_identity"), Relation.FromEndpointIdentity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("to_endpoint_identity"), Relation.ToEndpointIdentity, Code, Path, OutError)
				|| !ReadString(Object, TEXT("relation_type"), Relation.RelationType, Code, Path, OutError)
				|| !ReadOrder(Object, Relation.Order, Code, Path, OutError)
				|| !Builder.TryAddRelation(MoveTemp(Relation), OutError)) return false;
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
