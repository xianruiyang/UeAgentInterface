#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringStateMechanics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject::Private
{
	namespace
	{
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::DomainAdapters::
			EditorProjectSimpleEffect;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::StateSemantic;

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

		TSharedPtr<FJsonValue> CloneJsonValue(
			const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			switch (Value->Type)
			{
			case EJson::Null:
				return MakeShared<FJsonValueNull>();
			case EJson::Boolean:
				return MakeShared<FJsonValueBoolean>(Value->AsBool());
			case EJson::String:
				return MakeShared<FJsonValueString>(Value->AsString());
			case EJson::Number:
				return MakeShared<FJsonValueNumber>(Value->AsNumber());
			case EJson::Array:
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: Value->AsArray())
					{
						Items.Add(CloneJsonValue(Item));
					}
					return MakeShared<FJsonValueArray>(MoveTemp(Items));
				}
			case EJson::Object:
				{
					const TSharedPtr<FJsonObject> Object =
						MakeShared<FJsonObject>();
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
						: Value->AsObject()->Values)
					{
						Object->SetField(
							Pair.Key,
							CloneJsonValue(Pair.Value));
					}
					return MakeShared<FJsonValueObject>(Object);
				}
			default:
				return MakeShared<FJsonValueNull>();
			}
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
					TEXT("editor_project_state_value_missing"));
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
				{
					const double Number = JsonValue->AsNumber();
					const bool bInteger =
						FMath::IsNearlyEqual(
							Number,
							FMath::FloorToDouble(Number))
						&& Number >= static_cast<double>(MIN_int64)
						&& Number <= static_cast<double>(MAX_int64);
					const bool bBuilt = bInteger
						? FUeAgentCanonicalValue::TryInteger(
							FString::Printf(
								TEXT("%lld"),
								static_cast<long long>(Number)),
							Resources,
							OutValue,
							Error)
						: FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(Number),
							Resources,
							OutValue,
							Error);
					if (bBuilt)
					{
						return true;
					}
					break;
				}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
								Item,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Add(Canonical.ToSharedRef());
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
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
						: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr Canonical;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								Canonical,
								OutError))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							Canonical.ToSharedRef());
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
				break;
			}
			return Fail(
				OutError,
				Error.Code.IsEmpty()
					? TEXT("editor_project_state_value_invalid")
					: Error.Code,
				Error.Detail);
		}

		TSharedPtr<FJsonValue> CanonicalToJson(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
			case EUeAgentValueKind::String:
				return MakeShared<FJsonValueString>(Value.GetText());
			case EUeAgentValueKind::Integer:
				{
					int64 Parsed = 0;
					if (!LexTryParseString(Parsed, *Value.GetLexeme()))
					{
						return nullptr;
					}
					return MakeShared<FJsonValueNumber>(
						static_cast<double>(Parsed));
				}
			case EUeAgentValueKind::Decimal:
				{
					double Parsed = 0.0;
					if (!LexTryParseString(Parsed, *Value.GetLexeme()))
					{
						return nullptr;
					}
					return MakeShared<FJsonValueNumber>(Parsed);
				}
			case EUeAgentValueKind::Array:
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FUeAgentCanonicalValueRef& Item
						: Value.GetArrayItems())
					{
						TSharedPtr<FJsonValue> Json = CanonicalToJson(*Item);
						if (!Json.IsValid())
						{
							return nullptr;
						}
						Items.Add(MoveTemp(Json));
					}
					return MakeShared<FJsonValueArray>(MoveTemp(Items));
				}
			case EUeAgentValueKind::Object:
				{
					const TSharedPtr<FJsonObject> Object =
						MakeShared<FJsonObject>();
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Pair
						: Value.GetObjectItems())
					{
						TSharedPtr<FJsonValue> Json = CanonicalToJson(*Pair.Value);
						if (!Json.IsValid())
						{
							return nullptr;
						}
						Object->SetField(Pair.Key, MoveTemp(Json));
					}
					return MakeShared<FJsonValueObject>(Object);
				}
			default:
				return nullptr;
			}
		}

		TSharedPtr<FJsonObject> ReadConfigMap(
			const FConfigFile& Config,
			const FString& Section,
			const TSet<FName>& ExcludedKeys = {})
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			const FConfigSection* Found = Config.FindSection(Section);
			if (!Found)
			{
				return Result;
			}
			for (const TPair<FName, FConfigValue>& Pair : *Found)
			{
				if (!ExcludedKeys.Contains(Pair.Key))
				{
					Result->SetStringField(
						Pair.Key.ToString(),
						Pair.Value.GetValue());
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> ReadCVarMap(
			const FConfigFile& Config,
			const FString& Section)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			const FConfigSection* Found = Config.FindSection(Section);
			if (!Found)
			{
				return Result;
			}
			TArray<FConfigValue> Values;
			Found->MultiFind(FName(TEXT("CVars")), Values, true);
			for (const FConfigValue& Value : Values)
			{
				FString Key;
				FString Data;
				if (Value.GetValue().Split(TEXT("="), &Key, &Data)
					&& !Key.IsEmpty())
				{
					Result->SetStringField(Key, Data);
				}
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ReadCultures(
			const FConfigFile& Config,
			const FString& Section)
		{
			TSet<FString> Unique;
			if (const FConfigSection* Found = Config.FindSection(Section))
			{
				TArray<FConfigValue> Values;
				Found->MultiFind(
					FName(TEXT("CulturesToGenerate")),
					Values,
					true);
				for (const FConfigValue& Value : Values)
				{
					Unique.Add(Value.GetValue());
				}
			}
			TArray<FString> Sorted = Unique.Array();
			Sorted.Sort();
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Culture : Sorted)
			{
				Result.Add(MakeShared<FJsonValueString>(Culture));
			}
			return Result;
		}

		TSharedPtr<FJsonValue> ReadPropertyValue(
			UObject& Object,
			FProperty& Property,
			FExecutionMutationError& OutError)
		{
			const void* Address =
				Property.ContainerPtrToValuePtr<void>(&Object);
			if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
			{
				return MakeShared<FJsonValueBoolean>(
					Bool->GetPropertyValue(Address));
			}
			if (const FStrProperty* String = CastField<FStrProperty>(&Property))
			{
				return MakeShared<FJsonValueString>(
					String->GetPropertyValue(Address));
			}
			if (const FIntProperty* Integer = CastField<FIntProperty>(&Property))
			{
				return MakeShared<FJsonValueNumber>(
					Integer->GetPropertyValue(Address));
			}
			if (const FInt64Property* Integer = CastField<FInt64Property>(&Property))
			{
				return MakeShared<FJsonValueNumber>(
					static_cast<double>(Integer->GetPropertyValue(Address)));
			}
			Fail(
				OutError,
				TEXT("editor_project_settings_property_type_unsupported"),
				Property.GetName());
			return nullptr;
		}

		TSharedPtr<FJsonValue> DefaultPropertyValue(FProperty& Property)
		{
			if (CastField<FBoolProperty>(&Property))
			{
				return MakeShared<FJsonValueBoolean>(false);
			}
			if (CastField<FStrProperty>(&Property))
			{
				return MakeShared<FJsonValueString>(FString());
			}
			if (CastField<FIntProperty>(&Property)
				|| CastField<FInt64Property>(&Property))
			{
				return MakeShared<FJsonValueNumber>(0.0);
			}
			return nullptr;
		}

		bool TryReadSettingsState(
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
			const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
			const TSharedPtr<FJsonObject>& RequestPayload,
			TSharedPtr<FJsonObject>& OutProperties,
			TSharedPtr<FJsonObject>& OutDefaults,
			TArray<FEditorProjectDependencyState>& OutDependencies,
			FExecutionMutationError& OutError)
		{
			OutProperties = MakeShared<FJsonObject>();
			OutDefaults = MakeShared<FJsonObject>();
			UObject* Settings = Target.GetSettingsObject();
			const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
			if (!Settings
				|| !RequestPayload.IsValid()
				|| !RequestPayload->TryGetArrayField(TEXT("changes"), Changes)
				|| !Changes)
			{
				return Fail(
					OutError,
					TEXT("editor_project_settings_state_request_invalid"),
					Target.GetCanonicalRef());
			}
			const TArray<TSharedPtr<FJsonValue>>* Nested = nullptr;
			for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
			{
				const TSharedPtr<FJsonObject> Change =
					ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
				FString FieldId;
				const TSharedPtr<FJsonObject>* Typed = nullptr;
				if (Change.IsValid()
					&& Change->TryGetStringField(TEXT("field_id"), FieldId)
					&& FieldId == TEXT("properties")
					&& Change->TryGetObjectField(TEXT("value"), Typed)
					&& Typed && Typed->IsValid())
				{
					const TSharedPtr<FJsonValue> Canonical =
						(*Typed)->TryGetField(TEXT("canonical_value"));
					if (Canonical.IsValid()
						&& Canonical->Type == EJson::Array)
					{
						Nested = &Canonical->AsArray();
					}
					break;
				}
			}
			if (!Nested)
			{
				return Fail(
					OutError,
					TEXT("editor_project_settings_changes_missing"));
			}
			for (const TSharedPtr<FJsonValue>& UnitValue : *Nested)
			{
				const TSharedPtr<FJsonObject> Unit =
					UnitValue.IsValid() ? UnitValue->AsObject() : nullptr;
				FString FieldRef;
				FString ExpectedRevision;
				FString TypeId;
				FProperty* Property = nullptr;
				FString CurrentRevision;
				if (!Unit.IsValid()
					|| !Unit->TryGetStringField(TEXT("field_ref"), FieldRef)
					|| !Unit->TryGetStringField(
						TEXT("expected_revision"),
						ExpectedRevision)
					|| !Unit->TryGetStringField(TEXT("type_id"), TypeId)
					|| !Resolver.TryResolveSettingsField(
						*Settings,
						FieldRef,
						ExpectedRevision,
						Property,
						CurrentRevision,
						OutError)
					|| !Property
					|| CurrentRevision != ExpectedRevision)
				{
					return OutError.IsPresent()
						? false
						: Fail(
							OutError,
							TEXT("editor_project_settings_field_unresolved"),
							FieldRef);
				}
				TSharedPtr<FJsonValue> Current =
					ReadPropertyValue(*Settings, *Property, OutError);
				TSharedPtr<FJsonValue> Default =
					DefaultPropertyValue(*Property);
				if (!Current.IsValid() || !Default.IsValid())
				{
					return false;
				}
				const TSharedPtr<FJsonObject> CurrentRecord =
					MakeShared<FJsonObject>();
				CurrentRecord->SetStringField(
					TEXT("target_revision"),
					CurrentRevision);
				CurrentRecord->SetStringField(TEXT("type_id"), TypeId);
				CurrentRecord->SetField(TEXT("value"), Current);
				OutProperties->SetObjectField(FieldRef, CurrentRecord);
				const TSharedPtr<FJsonObject> DefaultRecord =
					MakeShared<FJsonObject>();
				DefaultRecord->SetStringField(
					TEXT("target_revision"),
					CurrentRevision);
				DefaultRecord->SetStringField(TEXT("type_id"), TypeId);
				DefaultRecord->SetField(TEXT("value"), Default);
				OutDefaults->SetObjectField(FieldRef, DefaultRecord);
				OutDependencies.Add({
					FieldRef,
					CurrentRevision,
					TEXT("project_settings_field"),
					TypeId,
				});
			}
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> ReadStringTableRows(
			const FStringTable& Table)
		{
			TArray<TSharedPtr<FJsonObject>> Rows;
			Table.EnumerateSourceStrings(
				[&Rows, &Table](
					const FString& Key,
					const FString& Source)
				{
					const TSharedPtr<FJsonObject> Metadata =
						MakeShared<FJsonObject>();
					Table.EnumerateMetaData(
						Key,
						[&Metadata](
							const FName MetadataKey,
							const FString& MetadataValue)
						{
							Metadata->SetStringField(
								MetadataKey.ToString(),
								MetadataValue);
							return true;
						});
					const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("key"), Key);
					Row->SetStringField(TEXT("source"), Source);
					Row->SetObjectField(TEXT("metadata"), Metadata);
					Rows.Add(Row);
					return true;
				});
			Rows.Sort(
				[](const TSharedPtr<FJsonObject>& Left,
					const TSharedPtr<FJsonObject>& Right)
				{
					return Left->GetStringField(TEXT("key"))
						< Right->GetStringField(TEXT("key"));
				});
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const TSharedPtr<FJsonObject>& Row : Rows)
			{
				Result.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Result;
		}

		const TSharedPtr<FJsonObject>* GetObjectField(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field)
		{
			const TSharedPtr<FJsonObject>* Result = nullptr;
			return Object.IsValid()
				&& Object->TryGetObjectField(Field, Result)
				&& Result && Result->IsValid()
				? Result
				: nullptr;
		}

		TSharedPtr<FJsonValue> GetCanonicalChangeValue(
			const TSharedPtr<FJsonObject>& Change)
		{
			const TSharedPtr<FJsonObject>* Value =
				GetObjectField(Change, TEXT("value"));
			return Value ? (*Value)->TryGetField(TEXT("canonical_value"))
				: nullptr;
		}

		bool ApplyMapChange(
			FJsonObject& FieldState,
			const FString& FieldId,
			const FString& Presence,
			const TSharedPtr<FJsonValue>& Value)
		{
			TSharedPtr<FJsonObject> Current = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (FieldState.TryGetObjectField(FieldId, Existing)
				&& Existing && Existing->IsValid())
			{
				Current = CloneJsonValue(
					MakeShared<FJsonValueObject>(*Existing))->AsObject();
			}
			if (Presence == TEXT("replace") || Presence == TEXT("clear"))
			{
				Current = MakeShared<FJsonObject>();
			}
			if (Presence != TEXT("clear"))
			{
				const TSharedPtr<FJsonObject> Values =
					Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Values.IsValid())
				{
					return false;
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
					: Values->Values)
				{
					if (Presence == TEXT("remove"))
					{
						Current->RemoveField(Pair.Key);
					}
					else
					{
						Current->SetField(
							Pair.Key,
							CloneJsonValue(Pair.Value));
					}
				}
			}
			FieldState.SetObjectField(FieldId, Current);
			return true;
		}

		bool ApplyStringSetChange(
			FJsonObject& FieldState,
			const FString& FieldId,
			const FString& Presence,
			const TSharedPtr<FJsonValue>& Value)
		{
			TSet<FString> Current;
			if (Presence == TEXT("set") || Presence == TEXT("remove"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
				if (FieldState.TryGetArrayField(FieldId, Existing) && Existing)
				{
					for (const TSharedPtr<FJsonValue>& Item : *Existing)
					{
						if (Item.IsValid() && Item->Type == EJson::String)
						{
							Current.Add(Item->AsString());
						}
					}
				}
			}
			if (Presence != TEXT("clear"))
			{
				if (!Value.IsValid() || Value->Type != EJson::Array)
				{
					return false;
				}
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					if (!Item.IsValid() || Item->Type != EJson::String)
					{
						return false;
					}
					if (Presence == TEXT("remove"))
					{
						Current.Remove(Item->AsString());
					}
					else
					{
						Current.Add(Item->AsString());
					}
				}
			}
			TArray<FString> Sorted = Current.Array();
			Sorted.Sort();
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Item : Sorted)
			{
				Items.Add(MakeShared<FJsonValueString>(Item));
			}
			FieldState.SetArrayField(FieldId, MoveTemp(Items));
			return true;
		}

		bool ApplyStringTableRowsChange(
			FJsonObject& FieldState,
			const FString& Presence,
			const TSharedPtr<FJsonValue>& Value)
		{
			TMap<FString, TSharedPtr<FJsonObject>> Rows;
			if (Presence == TEXT("set") || Presence == TEXT("remove"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
				if (FieldState.TryGetArrayField(TEXT("entries"), Existing)
					&& Existing)
				{
					for (const TSharedPtr<FJsonValue>& Item : *Existing)
					{
						const TSharedPtr<FJsonObject> Row =
							Item.IsValid() ? Item->AsObject() : nullptr;
						FString Key;
						if (Row.IsValid()
							&& Row->TryGetStringField(TEXT("key"), Key))
						{
							Rows.Add(Key, Row);
						}
					}
				}
			}
			if (Presence != TEXT("clear"))
			{
				if (!Value.IsValid() || Value->Type != EJson::Array)
				{
					return false;
				}
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					const TSharedPtr<FJsonObject> Row =
						Item.IsValid() ? Item->AsObject() : nullptr;
					FString Key;
					if (!Row.IsValid()
						|| !Row->TryGetStringField(TEXT("key"), Key))
					{
						return false;
					}
					if (Presence == TEXT("remove"))
					{
						Rows.Remove(Key);
					}
					else
					{
						Rows.Add(Key, Row);
					}
				}
			}
			TArray<FString> Keys;
			Rows.GetKeys(Keys);
			Keys.Sort();
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Key : Keys)
			{
				Result.Add(CloneJsonValue(
					MakeShared<FJsonValueObject>(Rows.FindChecked(Key))));
			}
			FieldState.SetArrayField(TEXT("entries"), MoveTemp(Result));
			return true;
		}

		bool ApplySettingsChange(
			FJsonObject& FieldState,
			const FJsonObject& Defaults,
			const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject>* CurrentProperties = nullptr;
			const TSharedPtr<FJsonObject>* DefaultProperties = nullptr;
			if (!FieldState.TryGetObjectField(
					TEXT("properties"),
					CurrentProperties)
				|| !CurrentProperties || !CurrentProperties->IsValid()
				|| !Defaults.TryGetObjectField(
					TEXT("properties"),
					DefaultProperties)
				|| !DefaultProperties || !DefaultProperties->IsValid()
				|| !Value.IsValid() || Value->Type != EJson::Array)
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Properties =
				CloneJsonValue(
					MakeShared<FJsonValueObject>(*CurrentProperties))
						->AsObject();
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				const TSharedPtr<FJsonObject> Change =
					Item.IsValid() ? Item->AsObject() : nullptr;
				FString FieldRef;
				FString Presence;
				const TSharedPtr<FJsonObject>* CurrentRecord = nullptr;
				const TSharedPtr<FJsonObject>* DefaultRecord = nullptr;
				if (!Change.IsValid()
					|| !Change->TryGetStringField(TEXT("field_ref"), FieldRef)
					|| !Change->TryGetStringField(TEXT("presence"), Presence)
					|| !Properties->TryGetObjectField(
						FieldRef,
						CurrentRecord)
					|| !CurrentRecord || !CurrentRecord->IsValid()
					|| !(*DefaultProperties)->TryGetObjectField(
						FieldRef,
						DefaultRecord)
					|| !DefaultRecord || !DefaultRecord->IsValid())
				{
					return false;
				}
				const TSharedPtr<FJsonObject> ResultRecord =
					CloneJsonValue(
						MakeShared<FJsonValueObject>(*CurrentRecord))
							->AsObject();
				if (Presence == TEXT("set"))
				{
					const TSharedPtr<FJsonValue> NewValue =
						Change->TryGetField(TEXT("value"));
					if (!NewValue.IsValid())
					{
						return false;
					}
					ResultRecord->SetField(
						TEXT("value"),
						CloneJsonValue(NewValue));
				}
				else if (Presence == TEXT("clear")
					|| Presence == TEXT("null"))
				{
					const TSharedPtr<FJsonValue> DefaultValue =
						(*DefaultRecord)->TryGetField(TEXT("value"));
					if (!DefaultValue.IsValid())
					{
						return false;
					}
					ResultRecord->SetField(
						TEXT("value"),
						CloneJsonValue(DefaultValue));
				}
				else
				{
					return false;
				}
				Properties->SetObjectField(FieldRef, ResultRecord);
			}
			FieldState.SetObjectField(TEXT("properties"), Properties);
			return true;
		}
	}

	bool TryBuildEditorProjectTargetState(
		IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
		const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
		const FString& ProfileId,
		const TSharedPtr<FJsonObject>& RequestPayload,
		const bool bIncludePlanningDependencies,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutState,
		TArray<FEditorProjectDependencyState>& OutDependencies,
		FExecutionMutationError& OutError)
	{
		OutState.Reset();
		OutDependencies.Reset();
		OutError = FExecutionMutationError();
		if (!Target.IsValid()
			|| Target.GetProfileId() != ProfileId
			|| !RequestPayload.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_project_state_target_invalid"),
				Target.GetCanonicalRef());
		}
		const TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
		if (ProfileId == TEXT("project_settings"))
		{
			TSharedPtr<FJsonObject> Properties;
			TSharedPtr<FJsonObject> PropertyDefaults;
			if (!TryReadSettingsState(
					Resolver,
					Target,
					RequestPayload,
					Properties,
					PropertyDefaults,
					OutDependencies,
					OutError))
			{
				return false;
			}
			Fields->SetObjectField(TEXT("properties"), Properties);
			Defaults->SetObjectField(
				TEXT("properties"),
				PropertyDefaults);
		}
		else if (ProfileId == TEXT("string_table"))
		{
			UStringTable* Object = Target.GetStringTable();
			if (!Object)
			{
				return Fail(
					OutError,
					TEXT("editor_project_string_table_target_invalid"),
					Target.GetCanonicalRef());
			}
			Fields->SetStringField(
				TEXT("namespace"),
				Object->GetStringTable()->GetNamespace());
			Fields->SetArrayField(
				TEXT("entries"),
				ReadStringTableRows(*Object->GetStringTable()));
			Defaults->SetStringField(TEXT("namespace"), FString());
			Defaults->SetArrayField(TEXT("entries"), {});
		}
		else
		{
			FConfigFile* Config = Target.GetConfigDocument();
			const FString& Section = Target.GetConfigSection();
			if (!Config || Section.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("editor_project_config_target_invalid"),
					Target.GetCanonicalRef());
			}
			if (ProfileId == TEXT("device_profile"))
			{
				FString DeviceType;
				FString BaseProfile;
				Config->GetString(
					*Section,
					TEXT("DeviceType"),
					DeviceType);
				Config->GetString(
					*Section,
					TEXT("BaseProfileName"),
					BaseProfile);
				Fields->SetStringField(TEXT("device_type"), DeviceType);
				Fields->SetField(
					TEXT("base_profile_ref"),
					BaseProfile.IsEmpty()
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
						: MakeShared<FJsonValueString>(BaseProfile));
				Fields->SetObjectField(
					TEXT("cvars"),
					ReadCVarMap(*Config, Section));
				Defaults->SetStringField(TEXT("device_type"), FString());
				Defaults->SetField(
					TEXT("base_profile_ref"),
					MakeShared<FJsonValueNull>());
				Defaults->SetObjectField(
					TEXT("cvars"),
					MakeShared<FJsonObject>());
				if (bIncludePlanningDependencies)
				{
					const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
					if (RequestPayload->TryGetArrayField(
							TEXT("changes"),
							Changes)
						&& Changes)
					{
						for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
						{
							const TSharedPtr<FJsonObject> Change =
								ChangeValue.IsValid()
									? ChangeValue->AsObject()
									: nullptr;
							FString FieldId;
							FString Presence;
							TSharedPtr<FJsonValue> Value =
								GetCanonicalChangeValue(Change);
							if (!Change.IsValid()
								|| !Change->TryGetStringField(
									TEXT("field_id"),
									FieldId)
								|| FieldId != TEXT("base_profile_ref")
								|| !Change->TryGetStringField(
									TEXT("presence"),
									Presence)
								|| Presence != TEXT("set")
								|| !Value.IsValid()
								|| Value->Type != EJson::String)
							{
								continue;
							}
							FUeAgentEditorProjectSimpleEffectResolvedTarget BaseTarget;
							if (!Resolver.TryResolveTarget(
									Value->AsString(),
									BaseTarget,
									OutError)
								|| !BaseTarget.IsValid()
								|| BaseTarget.GetProfileId()
									!= TEXT("device_profile"))
							{
								return false;
							}
							OutDependencies.Add({
								BaseTarget.GetCanonicalRef(),
								BaseTarget.GetRevision(),
								TEXT("device_profile"),
								TEXT("editor_project_device_profile_state"),
							});
						}
					}
				}
			}
			else if (ProfileId == TEXT("localization_target"))
			{
				FString NativeCulture;
				Config->GetString(
					*Section,
					TEXT("NativeCulture"),
					NativeCulture);
				Fields->SetStringField(
					TEXT("native_culture"),
					NativeCulture);
				Fields->SetArrayField(
					TEXT("cultures"),
					ReadCultures(*Config, Section));
				Defaults->SetStringField(
					TEXT("native_culture"),
					FString());
				Defaults->SetArrayField(TEXT("cultures"), {});
			}
			else
			{
				Fields->SetObjectField(
					TEXT("entries"),
					ReadConfigMap(*Config, Section));
				Defaults->SetObjectField(
					TEXT("entries"),
					MakeShared<FJsonObject>());
			}
		}

		if (!bIncludePlanningDependencies)
		{
			OutDependencies.Reset();
		}
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("target_ref"), Target.GetCanonicalRef());
		if (bIncludePlanningDependencies)
		{
			State->SetStringField(
				TEXT("target_revision"),
				Target.GetRevision());
		}
		State->SetStringField(TEXT("profile_id"), ProfileId);
		State->SetObjectField(TEXT("field_state"), Fields);
		State->SetObjectField(TEXT("field_defaults"), Defaults);
		return TryCanonicalValue(
			MakeShared<FJsonValueObject>(State),
			Resources,
			OutState,
			OutError);
	}

	bool TryProjectEditorProjectExpectedState(
		const FUeAgentCanonicalValue& Before,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Reset();
		OutError = FExecutionMutationError();
		TSharedPtr<FJsonValue> BeforeValue = CanonicalToJson(Before);
		TSharedPtr<FJsonValue> AfterValue = CanonicalToJson(ProposedAfter);
		const TSharedPtr<FJsonObject> State =
			BeforeValue.IsValid() ? BeforeValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject> After =
			AfterValue.IsValid() ? AfterValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* FieldsPtr = nullptr;
		const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		FString ProfileId;
		if (!State.IsValid() || !After.IsValid()
			|| !After->TryGetStringField(TEXT("profile_id"), ProfileId)
			|| !State->TryGetObjectField(TEXT("field_state"), FieldsPtr)
			|| !FieldsPtr || !FieldsPtr->IsValid()
			|| !State->TryGetObjectField(TEXT("field_defaults"), DefaultsPtr)
			|| !DefaultsPtr || !DefaultsPtr->IsValid()
			|| !After->TryGetArrayField(TEXT("typed_changes"), Changes)
			|| !Changes)
		{
			return Fail(
				OutError,
				TEXT("editor_project_expected_state_invalid"));
		}
		const TSharedPtr<FJsonObject> Fields =
			CloneJsonValue(MakeShared<FJsonValueObject>(*FieldsPtr))
				->AsObject();
		for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
		{
			const TSharedPtr<FJsonObject> Change =
				ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
			FString FieldId;
			FString Presence;
			if (!Change.IsValid()
				|| !Change->TryGetStringField(TEXT("field_id"), FieldId)
				|| !Change->TryGetStringField(TEXT("presence"), Presence))
			{
				return Fail(
					OutError,
					TEXT("editor_project_expected_change_invalid"));
			}
			const TSharedPtr<FJsonValue> Value =
				GetCanonicalChangeValue(Change);
			bool bApplied = false;
			if ((FieldId == TEXT("entries")
					&& ProfileId != TEXT("string_table"))
				|| FieldId == TEXT("cvars"))
			{
				bApplied = ApplyMapChange(
					*Fields,
					FieldId,
					Presence,
					Value);
			}
			else if (ProfileId == TEXT("project_settings")
				&& FieldId == TEXT("properties"))
			{
				bApplied = ApplySettingsChange(
					*Fields,
					**DefaultsPtr,
					Value);
			}
			else if (ProfileId == TEXT("localization_target")
				&& FieldId == TEXT("cultures"))
			{
				bApplied = ApplyStringSetChange(
					*Fields,
					FieldId,
					Presence,
					Value);
			}
			else if (ProfileId == TEXT("string_table")
				&& FieldId == TEXT("entries"))
			{
				bApplied = ApplyStringTableRowsChange(
					*Fields,
					Presence,
					Value);
			}
			else if (Presence == TEXT("set"))
			{
				bApplied = Value.IsValid();
				if (bApplied)
				{
					Fields->SetField(FieldId, CloneJsonValue(Value));
				}
			}
			else if (Presence == TEXT("clear")
				|| Presence == TEXT("null"))
			{
				const TSharedPtr<FJsonValue> Default =
					(*DefaultsPtr)->TryGetField(FieldId);
				bApplied = Default.IsValid();
				if (bApplied)
				{
					Fields->SetField(FieldId, CloneJsonValue(Default));
				}
			}
			if (!bApplied)
			{
				return Fail(
					OutError,
					TEXT("editor_project_expected_change_unsupported"),
					ProfileId + TEXT(":") + FieldId);
			}
		}
		State->SetObjectField(TEXT("field_state"), Fields);
		State->RemoveField(TEXT("target_revision"));
		return TryCanonicalValue(
			MakeShared<FJsonValueObject>(State),
			Resources,
			OutState,
			OutError);
	}

	bool TryWriteEditorProjectStagedConfig(
		const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
		const FString& StagedPath,
		FExecutionMutationError& OutError)
	{
		OutError = FExecutionMutationError();
		const FString FullPath =
			FPaths::ConvertRelativePathToFull(StagedPath);
		if (!Target.IsValid() || FullPath.IsEmpty()
			|| !IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(FullPath),
				true))
		{
			return Fail(
				OutError,
				TEXT("editor_project_config_stage_path_invalid"),
				FullPath);
		}
		IFileManager::Get().Delete(*FullPath, false, true);
		if (FConfigFile* Config = Target.GetConfigDocument())
		{
			if (!Config->Write(FullPath))
			{
				return Fail(
					OutError,
					TEXT("editor_project_config_stage_write_failed"),
					Target.GetCanonicalRef());
			}
			return true;
		}
		if (UObject* Settings = Target.GetSettingsObject())
		{
			Settings->SaveConfig(CPF_Config, *FullPath);
			if (!IFileManager::Get().FileExists(*FullPath))
			{
				return Fail(
					OutError,
					TEXT("editor_project_config_stage_write_failed"),
					Target.GetCanonicalRef());
			}
			return true;
		}
		return Fail(
			OutError,
			TEXT("editor_project_config_stage_target_invalid"),
			Target.GetCanonicalRef());
	}
}
