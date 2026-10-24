#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringStateMechanics.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject::Private
{
	namespace
	{
		using namespace UE::UAI::ContextResource;
		using namespace UE::UAI::DomainAdapters::
			EditorProjectSimpleEffect;
		using namespace UE::UAI::ExecutionMutation;
		using namespace UE::UAI::StateSemantic;

		using UE::UAI::Contract::FDiagnostic;

		FDiagnostic StateFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("execution"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			return Error;
		}

		FDiagnostic JsonValueFailure(const FString& Path, const TSharedPtr<FJsonValue>& Actual, const FString& Expected, const bool bInternal = false)
		{
			auto Error = StateFailure(TEXT("editor_project_state_field_invalid"),
				TEXT("The EditorProject state field does not satisfy the required value."), Path);
			Error.ActualValuePresent = Actual.IsValid();
			if (bInternal) Error.ActualEvidence = Actual;
			else if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				else if (Actual->AsObject().IsValid()) Summary->SetNumberField(TEXT("count"), Actual->AsObject()->Values.Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		FDiagnostic JsonFieldFailure(const TSharedPtr<FJsonObject>& Object, const FString& Field,
			const FString& Expected, const FString& Path = FString(), const bool bInternal = false)
		{
			return JsonValueFailure(Path.IsEmpty() ? Field : Path,
				Object.IsValid() ? Object->Values.FindRef(Field) : nullptr, Expected, bInternal);
		}

		FString PathKey(const FString& Value)
		{
			return Value.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}


		TSharedPtr<FJsonValue> CloneJsonValue(
			const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			return UeAgentAuthoringState::CloneCanonicalValue(Value);
		}

		bool TryCanonicalValue(const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue, FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				OutError = JsonValueFailure(TEXT("state"), JsonValue, TEXT("JSON value"));
				return false;
			}
			if (!CanonicalJson::TryFromJsonValue(JsonValue, Resources, OutValue, OutError))
			{
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		bool TryCanonicalToJson(const FUeAgentCanonicalValue& Value, TSharedPtr<FJsonValue>& OutValue,
			FDiagnostic& OutError, const FString& Path)
		{
			if (CanonicalJson::TryToJsonValue(Value, OutValue, OutError)) return true;
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = OutError.PropertyPath.IsEmpty() ? Path
				: Path + (OutError.PropertyPath.StartsWith(TEXT("/")) ? OutError.PropertyPath : TEXT("/") + OutError.PropertyPath);
			return false;
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
			OutError = StateFailure(TEXT("editor_project_settings_property_type_unsupported"),
				TEXT("The EditorProject settings reader does not support this property type."), Property.GetName());
			OutError.ActualValue = MakeShared<FJsonValueString>(Property.GetCPPType());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("bool, string, int32 or int64"));
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
			if (!Settings)
			{
				OutError = StateFailure(TEXT("editor_project_settings_state_request_invalid"),
					TEXT("The EditorProject target has no settings object to read."), TEXT("settings_object"));
				OutError.TargetPath = Target.GetCanonicalRef();
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			if (!RequestPayload.IsValid() || !RequestPayload->TryGetArrayField(TEXT("changes"), Changes) || !Changes)
			{
				OutError = !RequestPayload.IsValid() ? JsonValueFailure(TEXT("request"), nullptr, TEXT("object"))
					: JsonFieldFailure(RequestPayload, TEXT("changes"), TEXT("array"));
				OutError.TargetPath = Target.GetCanonicalRef();
				return false;
			}
			TArray<FUeAgentEditorProjectSettingsFieldObservation> Observations;
			FExecutionMutationError ObservationError;
			if (!Resolver.TryReadSettingsFields(
					*Settings,
					Observations,
					ObservationError))
			{
				if ((!ObservationError.IsEmpty()))
				{
					OutError = MoveTemp(ObservationError);
					return false;
				}
				OutError = StateFailure(TEXT("editor_project_settings_fields_unavailable"),
					TEXT("The settings resolver could not enumerate fields and returned no diagnostic."), TEXT("settings_fields"));
				OutError.TargetPath = Target.GetCanonicalRef();
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Nested = nullptr;
			FDiagnostic NestedError;
			FString NestedPath;
			for (int32 ChangeIndex = 0; ChangeIndex < Changes->Num(); ++ChangeIndex)
			{
				const auto& ChangeValue = (*Changes)[ChangeIndex];
				const auto Change = ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
				FString FieldId;
				const TSharedPtr<FJsonObject>* Typed = nullptr;
				if (!Change.IsValid() || !Change->TryGetStringField(TEXT("field_id"), FieldId) || FieldId != TEXT("properties")) continue;
				const FString ChangePath = TEXT("changes/") + FString::FromInt(ChangeIndex);
				if (!Change->TryGetObjectField(TEXT("value"), Typed) || !Typed || !Typed->IsValid())
				{
					if (NestedError.IsEmpty()) NestedError = JsonFieldFailure(Change, TEXT("value"), TEXT("object"), ChangePath + TEXT("/value"));
					continue;
				}
				const auto Canonical = (*Typed)->TryGetField(TEXT("canonical_value"));
				NestedPath = ChangePath + TEXT("/value/canonical_value");
				if (Canonical.IsValid() && Canonical->Type == EJson::Array) Nested = &Canonical->AsArray();
				else NestedError = JsonValueFailure(NestedPath, Canonical, TEXT("array of property changes"));
				break;
			}
			if (!Nested)
			{
				OutError = NestedError;
				if (OutError.IsEmpty())
				{
					OutError = StateFailure(TEXT("editor_project_settings_changes_missing"),
						TEXT("The settings request contains no properties change with an array of canonical values."), TEXT("changes"));
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Changes->Num());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a properties change whose value.canonical_value is an array"));
				}
				OutError.TargetPath = Target.GetCanonicalRef();
				return false;
			}
			for (int32 UnitIndex = 0; UnitIndex < Nested->Num(); ++UnitIndex)
			{
				const auto& UnitValue = (*Nested)[UnitIndex];
				const FString UnitPath = NestedPath + TEXT("/") + FString::FromInt(UnitIndex);
				const TSharedPtr<FJsonObject> Unit =
					UnitValue.IsValid() ? UnitValue->AsObject() : nullptr;
				FString FieldRef;
				FString TypeId;
				FProperty* Property = nullptr;
				FString CurrentRevision;
				const FUeAgentEditorProjectSettingsFieldObservation* Observation = nullptr;
				if (!Unit.IsValid())
				{
					OutError = JsonValueFailure(UnitPath, UnitValue, TEXT("object"));
					return false;
				}
				if (!Unit->TryGetStringField(TEXT("field_ref"), FieldRef))
				{
					OutError = JsonFieldFailure(Unit, TEXT("field_ref"), TEXT("string"), UnitPath + TEXT("/field_ref"));
					return false;
				}
				if (!Unit->TryGetStringField(TEXT("type_id"), TypeId))
				{
					OutError = JsonFieldFailure(Unit, TEXT("type_id"), TEXT("string"), UnitPath + TEXT("/type_id"));
					OutError.TargetPath = FieldRef;
					return false;
				}
				Observation = Observations.FindByPredicate(
						[&FieldRef](const FUeAgentEditorProjectSettingsFieldObservation& Candidate)
						{
							return Candidate.FieldRef == FieldRef;
						});
				if (!Observation)
				{
					OutError = StateFailure(TEXT("editor_project_settings_field_unresolved"),
						TEXT("The requested settings field was not found in the resolver observations."), UnitPath + TEXT("/field_ref"));
					OutError.TargetPath = FieldRef;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(FieldRef);
					return false;
				}
				const bool bResolved = Resolver.TryResolveSettingsField(*Settings, FieldRef, Observation->FieldRevision,
					Property, CurrentRevision, OutError);
				if (!bResolved)
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = FieldRef;
					if (OutError.Source.JsonPath.IsEmpty()) OutError.Source.JsonPath = TEXT("/") + UnitPath + TEXT("/field_ref");
					return false;
				}
				TSharedPtr<FJsonValue> Current =
					ReadPropertyValue(*Settings, *Property, OutError);
				TSharedPtr<FJsonValue> Default =
					DefaultPropertyValue(*Property);
				if (!Current.IsValid())
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = FieldRef;
					if (OutError.Source.JsonPath.IsEmpty()) OutError.Source.JsonPath = TEXT("/") + UnitPath + TEXT("/field_ref");
					return false;
				}
				const TSharedPtr<FJsonObject> CurrentRecord =
					MakeShared<FJsonObject>();
				CurrentRecord->SetStringField(TEXT("type_id"), TypeId);
				CurrentRecord->SetField(TEXT("value"), Current);
				OutProperties->SetObjectField(FieldRef, CurrentRecord);
				const TSharedPtr<FJsonObject> DefaultRecord =
					MakeShared<FJsonObject>();
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

		TSharedPtr<FJsonValue> GetCanonicalChangeValue(const TSharedPtr<FJsonObject>& Change,
			FDiagnostic& OutError, const FString& Path)
		{
			const TSharedPtr<FJsonObject>* Value = GetObjectField(Change, TEXT("value"));
			if (!Value)
			{
				OutError = JsonFieldFailure(Change, TEXT("value"), TEXT("object"), Path + TEXT("/value"));
				return nullptr;
			}
			const auto Canonical = (*Value)->TryGetField(TEXT("canonical_value"));
			if (!Canonical.IsValid())
				OutError = JsonFieldFailure(*Value, TEXT("canonical_value"), TEXT("JSON value"), Path + TEXT("/value/canonical_value"));
			return Canonical;
		}

		bool ApplyMapChange(
			FJsonObject& FieldState,
			const FString& FieldId,
			const FString& Presence,
			const TSharedPtr<FJsonValue>& Value,
			FDiagnostic& OutError,
			const FString& Path,
			const FDiagnostic& ValueError)
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
					OutError = !Value.IsValid() ? ValueError : JsonValueFailure(Path, Value, TEXT("object"));
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
			const TSharedPtr<FJsonValue>& Value,
			FDiagnostic& OutError,
			const FString& Path,
			const FDiagnostic& ValueError)
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
					OutError = !Value.IsValid() ? ValueError : JsonValueFailure(Path, Value, TEXT("array of strings"));
					return false;
				}
				const auto& Values = Value->AsArray();
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					const auto& Item = Values[Index];
					if (!Item.IsValid() || Item->Type != EJson::String)
					{
						OutError = JsonValueFailure(Path + TEXT("/") + FString::FromInt(Index), Item, TEXT("string"));
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
			const TSharedPtr<FJsonValue>& Value,
			FDiagnostic& OutError,
			const FString& Path,
			const FDiagnostic& ValueError)
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
					OutError = !Value.IsValid() ? ValueError : JsonValueFailure(Path, Value, TEXT("array of row objects"));
					return false;
				}
				const auto& Values = Value->AsArray();
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					const auto& Item = Values[Index];
					const TSharedPtr<FJsonObject> Row =
						Item.IsValid() ? Item->AsObject() : nullptr;
					FString Key;
					if (!Row.IsValid())
					{
						OutError = JsonValueFailure(Path + TEXT("/") + FString::FromInt(Index), Item, TEXT("object"));
						return false;
					}
					if (!Row->TryGetStringField(TEXT("key"), Key))
					{
						OutError = JsonFieldFailure(Row, TEXT("key"), TEXT("string"), Path + TEXT("/") + FString::FromInt(Index) + TEXT("/key"));
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
			const TSharedPtr<FJsonValue>& Value,
			FDiagnostic& OutError,
			const FString& Path,
			const FDiagnostic& ValueError)
		{
			const TSharedPtr<FJsonObject>* CurrentProperties = nullptr;
			const TSharedPtr<FJsonObject>* DefaultProperties = nullptr;
			if (!FieldState.TryGetObjectField(TEXT("properties"), CurrentProperties) || !CurrentProperties || !CurrentProperties->IsValid())
			{
				OutError = JsonValueFailure(TEXT("before/field_state/properties"), FieldState.Values.FindRef(TEXT("properties")), TEXT("object"));
				return false;
			}
			if (!Defaults.TryGetObjectField(TEXT("properties"), DefaultProperties) || !DefaultProperties || !DefaultProperties->IsValid())
			{
				OutError = JsonValueFailure(TEXT("before/field_defaults/properties"), Defaults.Values.FindRef(TEXT("properties")), TEXT("object"));
				return false;
			}
			if (!Value.IsValid() || Value->Type != EJson::Array)
			{
				OutError = !Value.IsValid() ? ValueError : JsonValueFailure(Path, Value, TEXT("array of property changes"));
				return false;
			}
			const TSharedPtr<FJsonObject> Properties =
				CloneJsonValue(
					MakeShared<FJsonValueObject>(*CurrentProperties))
						->AsObject();
			const auto& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const auto& Item = Values[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				const TSharedPtr<FJsonObject> Change =
					Item.IsValid() ? Item->AsObject() : nullptr;
				FString FieldRef;
				FString Presence;
				const TSharedPtr<FJsonObject>* CurrentRecord = nullptr;
				const TSharedPtr<FJsonObject>* DefaultRecord = nullptr;
				if (!Change.IsValid())
				{
					OutError = JsonValueFailure(ItemPath, Item, TEXT("object"));
					return false;
				}
				if (!Change->TryGetStringField(TEXT("field_ref"), FieldRef))
				{
					OutError = JsonFieldFailure(Change, TEXT("field_ref"), TEXT("string"), ItemPath + TEXT("/field_ref"));
					return false;
				}
				if (!Change->TryGetStringField(TEXT("presence"), Presence))
				{
					OutError = JsonFieldFailure(Change, TEXT("presence"), TEXT("string"), ItemPath + TEXT("/presence"));
					return false;
				}
				if (!Properties->TryGetObjectField(FieldRef, CurrentRecord) || !CurrentRecord || !CurrentRecord->IsValid())
				{
					OutError = JsonFieldFailure(Properties, FieldRef, TEXT("property record object"), TEXT("before/field_state/properties/") + PathKey(FieldRef));
					OutError.TargetPath = FieldRef;
					return false;
				}
				if (!(*DefaultProperties)->TryGetObjectField(FieldRef, DefaultRecord) || !DefaultRecord || !DefaultRecord->IsValid())
				{
					OutError = JsonFieldFailure(*DefaultProperties, FieldRef, TEXT("default property record object"), TEXT("before/field_defaults/properties/") + PathKey(FieldRef));
					OutError.TargetPath = FieldRef;
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
						OutError = JsonFieldFailure(Change, TEXT("value"), TEXT("JSON value"), ItemPath + TEXT("/value"));
						OutError.TargetPath = FieldRef;
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
						OutError = JsonFieldFailure(*DefaultRecord, TEXT("value"), TEXT("JSON value"),
							TEXT("before/field_defaults/properties/") + PathKey(FieldRef) + TEXT("/value"));
						OutError.TargetPath = FieldRef;
						return false;
					}
					ResultRecord->SetField(
						TEXT("value"),
						CloneJsonValue(DefaultValue));
				}
				else
				{
					OutError = JsonFieldFailure(Change, TEXT("presence"), TEXT("set, clear or null"), ItemPath + TEXT("/presence"));
					OutError.TargetPath = FieldRef;
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
		if (!Target.IsValid())
		{
			OutError = Target.GetValidationDiagnostic();
			return false;
		}
		if (Target.GetProfileId() != ProfileId)
		{
			OutError = StateFailure(TEXT("editor_project_state_target_invalid"),
				TEXT("The EditorProject target profile differs from the requested state profile."), TEXT("profile_id"));
			OutError.TargetPath = Target.GetCanonicalRef();
			OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetProfileId());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ProfileId);
			return false;
		}
		if (!RequestPayload.IsValid())
		{
			OutError = JsonValueFailure(TEXT("request"), nullptr, TEXT("object"));
			OutError.TargetPath = Target.GetCanonicalRef();
			return false;
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
				OutError = StateFailure(TEXT("editor_project_string_table_target_invalid"),
					TEXT("The EditorProject target has no StringTable object."), TEXT("string_table"));
				OutError.TargetPath = Target.GetCanonicalRef();
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
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
				OutError = StateFailure(TEXT("editor_project_config_target_invalid"),
					!Config ? TEXT("The EditorProject target has no configuration document.") : TEXT("The EditorProject target has an empty configuration section."),
					!Config ? TEXT("config_document") : TEXT("config_section"));
				OutError.TargetPath = Target.GetCanonicalRef();
				if (!Config)
				{
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
				}
				else
				{
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Section);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty section name"));
				}
				return false;
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
						for (int32 ChangeIndex = 0; ChangeIndex < Changes->Num(); ++ChangeIndex)
						{
							const auto& ChangeValue = (*Changes)[ChangeIndex];
							const FString ChangePath = TEXT("changes/") + FString::FromInt(ChangeIndex);
							const TSharedPtr<FJsonObject> Change =
								ChangeValue.IsValid()
									? ChangeValue->AsObject()
									: nullptr;
							FString FieldId;
							FString Presence;
							FDiagnostic ProbeError;
							TSharedPtr<FJsonValue> Value = GetCanonicalChangeValue(Change, ProbeError, ChangePath);
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
							const bool bResolved = Resolver.TryResolveTarget(Value->AsString(), BaseTarget, OutError);
							if (!bResolved || !BaseTarget.IsValid() || BaseTarget.GetProfileId() != TEXT("device_profile"))
							{
								if (OutError.IsEmpty())
								{
									if (bResolved && !BaseTarget.IsValid()) OutError = BaseTarget.GetValidationDiagnostic();
									else
									{
										OutError = StateFailure(TEXT("editor_project_base_profile_unresolved"),
											!bResolved ? TEXT("The base-profile resolver failed without returning a diagnostic.")
												: TEXT("The resolved base target is not a device profile."), ChangePath + TEXT("/value/canonical_value"));
										if (bResolved)
										{
											OutError.ActualValue = MakeShared<FJsonValueString>(BaseTarget.GetProfileId());
											OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("device_profile"));
										}
									}
								}
								if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Value->AsString();
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
		TSharedPtr<FJsonValue> BeforeValue;
		TSharedPtr<FJsonValue> AfterValue;
		FDiagnostic BeforeError;
		FDiagnostic AfterError;
		const bool bBeforeConverted = TryCanonicalToJson(Before, BeforeValue, BeforeError, TEXT("before"));
		const bool bAfterConverted = TryCanonicalToJson(ProposedAfter, AfterValue, AfterError, TEXT("proposed_after"));
		if (!bBeforeConverted || !bAfterConverted)
		{
			OutError = !bBeforeConverted ? MoveTemp(BeforeError) : MoveTemp(AfterError);
			return false;
		}
		const TSharedPtr<FJsonObject> State =
			BeforeValue.IsValid() ? BeforeValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject> After =
			AfterValue.IsValid() ? AfterValue->AsObject() : nullptr;
		const TSharedPtr<FJsonObject>* FieldsPtr = nullptr;
		const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		FString ProfileId;
		if (!State.IsValid())
		{
			OutError = JsonValueFailure(TEXT("before"), BeforeValue, TEXT("object"));
			return false;
		}
		if (!After.IsValid())
		{
			OutError = JsonValueFailure(TEXT("proposed_after"), AfterValue, TEXT("object"));
			return false;
		}
		if (!After->TryGetStringField(TEXT("profile_id"), ProfileId))
		{
			OutError = JsonFieldFailure(After, TEXT("profile_id"), TEXT("string"), TEXT("proposed_after/profile_id"));
			return false;
		}
		if (!State->TryGetObjectField(TEXT("field_state"), FieldsPtr) || !FieldsPtr || !FieldsPtr->IsValid())
		{
			OutError = JsonFieldFailure(State, TEXT("field_state"), TEXT("object"), TEXT("before/field_state"));
			return false;
		}
		if (!State->TryGetObjectField(TEXT("field_defaults"), DefaultsPtr) || !DefaultsPtr || !DefaultsPtr->IsValid())
		{
			OutError = JsonFieldFailure(State, TEXT("field_defaults"), TEXT("object"), TEXT("before/field_defaults"));
			return false;
		}
		if (!After->TryGetArrayField(TEXT("typed_changes"), Changes) || !Changes)
		{
			OutError = JsonFieldFailure(After, TEXT("typed_changes"), TEXT("array"), TEXT("proposed_after/typed_changes"));
			return false;
		}
		const TSharedPtr<FJsonObject> Fields =
			CloneJsonValue(MakeShared<FJsonValueObject>(*FieldsPtr))
				->AsObject();
		for (int32 ChangeIndex = 0; ChangeIndex < Changes->Num(); ++ChangeIndex)
		{
			const auto& ChangeValue = (*Changes)[ChangeIndex];
			const FString ChangePath = TEXT("proposed_after/typed_changes/") + FString::FromInt(ChangeIndex);
			const TSharedPtr<FJsonObject> Change =
				ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
			FString FieldId;
			FString Presence;
			if (!Change.IsValid())
			{
				OutError = JsonValueFailure(ChangePath, ChangeValue, TEXT("object"));
				return false;
			}
			if (!Change->TryGetStringField(TEXT("field_id"), FieldId))
			{
				OutError = JsonFieldFailure(Change, TEXT("field_id"), TEXT("string"), ChangePath + TEXT("/field_id"));
				return false;
			}
			if (!Change->TryGetStringField(TEXT("presence"), Presence))
			{
				OutError = JsonFieldFailure(Change, TEXT("presence"), TEXT("string"), ChangePath + TEXT("/presence"));
				return false;
			}
			FDiagnostic ValueError;
			const auto Value = GetCanonicalChangeValue(Change, ValueError, ChangePath);
			const FString ValuePath = ChangePath + TEXT("/value/canonical_value");
			bool bApplied = false;
			if ((FieldId == TEXT("entries")
					&& ProfileId != TEXT("string_table"))
				|| FieldId == TEXT("cvars"))
			{
				bApplied = ApplyMapChange(*Fields, FieldId, Presence, Value, OutError, ValuePath, ValueError);
			}
			else if (ProfileId == TEXT("project_settings")
				&& FieldId == TEXT("properties"))
			{
				bApplied = ApplySettingsChange(*Fields, **DefaultsPtr, Value, OutError, ValuePath, ValueError);
			}
			else if (ProfileId == TEXT("localization_target")
				&& FieldId == TEXT("cultures"))
			{
				bApplied = ApplyStringSetChange(*Fields, FieldId, Presence, Value, OutError, ValuePath, ValueError);
			}
			else if (ProfileId == TEXT("string_table")
				&& FieldId == TEXT("entries"))
			{
				bApplied = ApplyStringTableRowsChange(*Fields, Presence, Value, OutError, ValuePath, ValueError);
			}
			else if (Presence == TEXT("set"))
			{
				bApplied = Value.IsValid();
				if (bApplied)
				{
					Fields->SetField(FieldId, CloneJsonValue(Value));
				}
				else OutError = MoveTemp(ValueError);
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
				else OutError = JsonFieldFailure(*DefaultsPtr, FieldId, TEXT("default JSON value"),
					TEXT("before/field_defaults/") + PathKey(FieldId));
			}
			else
			{
				OutError = JsonFieldFailure(Change, TEXT("presence"), TEXT("set, clear or null"), ChangePath + TEXT("/presence"));
			}
			if (!bApplied) return false;
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
		if (!Target.IsValid())
		{
			OutError = Target.GetValidationDiagnostic();
			return false;
		}
		if (FullPath.IsEmpty())
		{
			OutError = StateFailure(TEXT("editor_project_config_stage_path_invalid"),
				TEXT("The configuration stage path resolves to an empty path."), TEXT("staged_path"));
			OutError.TargetPath = Target.GetCanonicalRef();
			OutError.ActualEvidence = MakeShared<FJsonValueString>(StagedPath);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty file path"));
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), true))
		{
			OutError = StateFailure(TEXT("editor_project_config_stage_path_invalid"),
				TEXT("The configuration stage directory could not be created."), TEXT("staged_directory"));
			OutError.TargetPath = Target.GetCanonicalRef();
			OutError.ActualEvidence = MakeShared<FJsonValueString>(FPaths::GetPath(FullPath));
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		IFileManager::Get().Delete(*FullPath, false, true);
		if (FConfigFile* Config = Target.GetConfigDocument())
		{
			if (!Config->Write(FullPath))
			{
				OutError = StateFailure(TEXT("editor_project_config_stage_write_failed"),
					TEXT("The configuration document writer reported failure for the staged file."), TEXT("staged_file/write_succeeded"));
				OutError.TargetPath = Target.GetCanonicalRef();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FullPath);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			return true;
		}
		if (UObject* Settings = Target.GetSettingsObject())
		{
			Settings->SaveConfig(CPF_Config, *FullPath);
			if (!IFileManager::Get().FileExists(*FullPath))
			{
				OutError = StateFailure(TEXT("editor_project_config_stage_write_failed"),
					TEXT("The settings save returned without producing the staged configuration file."), TEXT("staged_file"));
				OutError.TargetPath = Target.GetCanonicalRef();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FullPath);
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			return true;
		}
		OutError = StateFailure(TEXT("editor_project_config_stage_target_invalid"),
			TEXT("The EditorProject target provides neither a configuration document nor a settings object for staging."), TEXT("target"));
		OutError.TargetPath = Target.GetCanonicalRef();
		OutError.ActualValue = MakeShared<FJsonValueString>(Target.GetProfileId());
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("configuration document or settings object"));
		return false;
	}
}
