#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/CriticalSection.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::EditorProjectSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = TEXT(""))
        { OutError = MoveTemp(Diagnostic); if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject; return false; }

		FString NativePointerPart(const FString& Text)
		{
			return Text.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		FString NativeKindName(EUeAgentValueKind Kind)
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
			}
			return FString::Printf(TEXT("unsupported EUeAgentValueKind (%d)"), static_cast<int32>(Kind));
		}

		TSharedPtr<FJsonValue> NativeKind(const FUeAgentCanonicalValue* Value)
		{
			return MakeShared<FJsonValueString>(Value ? NativeKindName(Value->GetKind()) : TEXT("missing"));
		}

		TSharedPtr<FJsonValue> NativeStringOrKind(const FUeAgentCanonicalValue* Value)
		{
			return Value && Value->GetKind() == EUeAgentValueKind::String
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value->GetText())) : NativeKind(Value);
		}

		FExecutionMutationError NativeValueFailure(const FString& Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = FExecutionMutationError::Failure(Code, Reason, TEXT("native_execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		FExecutionMutationError NativeCondition(const FString& Code, const FString& Reason,
			const FString& Path, bool Actual = false, bool Expected = true)
		{
			return NativeValueFailure(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected));
		}

		FString NativeTargetKind(const FString& Profile)
		{
			if (Profile == TEXT("config_section")) return TEXT("configuration section");
			if (Profile == TEXT("device_profile")) return TEXT("device profile");
			if (Profile == TEXT("localization_target")) return TEXT("localization target");
			if (Profile == TEXT("platform_config")) return TEXT("platform configuration");
			if (Profile == TEXT("scalability_group")) return TEXT("scalability group");
			if (Profile == TEXT("project_settings")) return TEXT("project settings");
			if (Profile == TEXT("string_table")) return TEXT("string table");
			return TEXT("unrecognized target kind");
		}

		void NativeSupplement(FExecutionMutationError& Error, const FString& Target, const FString& Path = FString())
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
			if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Path;
			if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
		}

		FExecutionMutationError NativeUnresolvedTarget(
			const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target, const FString& Ref)
		{
			auto Error = Target.GetValidationDiagnostic();
			if (Error.IsEmpty())
			{
				Error = NativeCondition(TEXT("editor_project_target_unresolved"),
					TEXT("The target resolver did not return a usable editor project target."), TEXT("/target_resolved"));
			}
			NativeSupplement(Error, Ref);
			return Error;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field)
		{
			return Object.IsValid()
				? FindObjectField(*Object, Field)
				: nullptr;
		}

		bool TryReadString(
			const FUeAgentCanonicalValuePtr& Object,
			const FString& Field,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			if (!Value
				|| Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return true;
		}

		struct FTypedChange
		{
			FString Path;
			FString FieldId;
			FString FieldBindingId;
			FString Presence;
			FString TypeId;
			FUeAgentCanonicalValuePtr Value;
			bool bHasValue = false;
		};

		bool TryReadTypedChanges(
			const FUeAgentCanonicalValuePtr& After,
			TArray<FTypedChange>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			const FUeAgentCanonicalValue* Changes = FindObjectField(After, TEXT("typed_changes"));
			const FString Path = TEXT("/after/typed_changes");
			if (!Changes || Changes->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, NativeValueFailure(TEXT("editor_project_native_typed_changes_missing"),
					TEXT("The native change requires an array of typed field changes."), Path,
					NativeKind(Changes), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			for (int32 Index = 0; Index < Changes->GetArrayItems().Num(); ++Index)
			{
				const auto& Unit = Changes->GetArrayItems()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_native_change_invalid"),
						TEXT("A typed field change must be an object."), ItemPath,
						NativeKind(&Unit.Get()), MakeShared<FJsonValueString>(TEXT("object"))));
				}
				FTypedChange Change;
				Change.Path = ItemPath;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item : Unit->GetObjectItems())
				{
					if (Item.Key == TEXT("field_id") && Item.Value->GetKind() == EUeAgentValueKind::String)
						Change.FieldId = Item.Value->GetText();
					else if (Item.Key == TEXT("field_binding_id") && Item.Value->GetKind() == EUeAgentValueKind::String)
						Change.FieldBindingId = Item.Value->GetText();
					else if (Item.Key == TEXT("presence") && Item.Value->GetKind() == EUeAgentValueKind::String)
						Change.Presence = Item.Value->GetText();
					else if (Item.Key == TEXT("value") && Item.Value->GetKind() == EUeAgentValueKind::Object)
					{
						for (const TPair<FString, FUeAgentCanonicalValueRef>& ValueItem : Item.Value->GetObjectItems())
						{
							if (ValueItem.Key == TEXT("type_id") && ValueItem.Value->GetKind() == EUeAgentValueKind::String)
								Change.TypeId = ValueItem.Value->GetText();
							else if (ValueItem.Key == TEXT("canonical_value"))
							{
								Change.Value = FUeAgentCanonicalValuePtr(ValueItem.Value);
								Change.bHasValue = true;
							}
						}
					}
				}
				if (Change.FieldId.IsEmpty() || Change.FieldBindingId.IsEmpty() || Change.Presence.IsEmpty())
				{
					const FString Field = Change.FieldId.IsEmpty() ? TEXT("field_id")
						: Change.FieldBindingId.IsEmpty() ? TEXT("field_binding_id") : TEXT("presence");
					const auto* Actual = FindObjectField(*Unit, Field);
					auto Error = NativeValueFailure(TEXT("editor_project_native_change_invalid"),
						TEXT("A typed field change is missing a nonempty string field: ") + Field + TEXT("."),
						ItemPath + TEXT("/") + Field, NativeStringOrKind(Actual),
						MakeShared<FJsonValueString>(TEXT("nonempty string")));
					Error.CandidateName = Change.FieldId;
					return Fail(OutError, MoveTemp(Error));
				}
				OutChanges.Add(MoveTemp(Change));
			}
			return !OutChanges.IsEmpty() || Fail(OutError, NativeValueFailure(
				TEXT("editor_project_native_typed_changes_missing"),
				TEXT("The native change contains no typed field changes."), Path,
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change"))));
		}

		bool TryReadStringMap(
			const FUeAgentCanonicalValue& Value, TMap<FString, FString>& OutValues,
			const FString& Path, FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, NativeValueFailure(TEXT("editor_project_config_entry_invalid"),
					TEXT("Configuration entries must be a map of string values."), Path,
					NativeKind(&Value), MakeShared<FJsonValueString>(TEXT("object"))));
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item : Value.GetObjectItems())
			{
				if (Item.Value->GetKind() != EUeAgentValueKind::String)
				{
					auto Error = NativeValueFailure(TEXT("editor_project_config_entry_invalid"),
						TEXT("The configuration entry must contain a string value."),
						Path + TEXT("/") + NativePointerPart(Item.Key), NativeKind(&Item.Value.Get()),
						MakeShared<FJsonValueString>(TEXT("string")));
					Error.CandidateName = Item.Key;
					return Fail(OutError, MoveTemp(Error));
				}
				OutValues.Add(Item.Key, Item.Value->GetText());
			}
			return true;
		}

		bool TryReadStringArray(
			const FUeAgentCanonicalValue& Value, TArray<FString>& OutValues,
			const FString& Path, FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, NativeValueFailure(TEXT("editor_project_culture_set_invalid"),
					TEXT("Cultures must be an array of strings."), Path, NativeKind(&Value),
					MakeShared<FJsonValueString>(TEXT("array"))));
			}
			for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
			{
				const auto& Item = Value.GetArrayItems()[Index];
				if (Item->GetKind() != EUeAgentValueKind::String)
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_culture_set_invalid"),
						TEXT("The culture entry must be a string."), Path + TEXT("/") + FString::FromInt(Index),
						NativeKind(&Item.Get()), MakeShared<FJsonValueString>(TEXT("string"))));
				}
				OutValues.Add(Item->GetText());
			}
			return true;
		}

		struct FSettingsChange
		{
			FString Path;
			FString FieldRef;
			FString ExpectedRevision;
			FString Presence;
			FString TypeId;
			FProperty* Property = nullptr;
			FUeAgentCanonicalValuePtr Value;
			bool bHasValue = false;
			FString BeforeText;
		};

		bool TryReadSettingsChanges(
			const FUeAgentCanonicalValue& Value, UObject& SettingsObject,
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
			TArray<FSettingsChange>& OutChanges, const FString& Path,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, NativeValueFailure(TEXT("editor_project_settings_changes_invalid"),
					TEXT("Settings changes must be an array."), Path, NativeKind(&Value),
					MakeShared<FJsonValueString>(TEXT("array"))));
			}
			TArray<FUeAgentEditorProjectSettingsFieldObservation> Observations;
			if (!Resolver.TryReadSettingsFields(SettingsObject, Observations, OutError))
			{
				if (OutError.IsEmpty()) OutError = NativeCondition(TEXT("editor_project_settings_fields_unavailable"),
					TEXT("The settings resolver could not enumerate the current fields."), Path + TEXT("/fields_available"));
				NativeSupplement(OutError, FString(), Path);
				return false;
			}
			for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
			{
				const auto& Unit = Value.GetArrayItems()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("A settings change must be an object."), ItemPath,
						NativeKind(&Unit.Get()), MakeShared<FJsonValueString>(TEXT("object"))));
				}
				FSettingsChange Change;
				Change.Path = ItemPath;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item : Unit->GetObjectItems())
				{
					if (Item.Value->GetKind() == EUeAgentValueKind::String)
					{
						if (Item.Key == TEXT("field_ref")) Change.FieldRef = Item.Value->GetText();
						else if (Item.Key == TEXT("presence")) Change.Presence = Item.Value->GetText();
						else if (Item.Key == TEXT("type_id")) Change.TypeId = Item.Value->GetText();
					}
					if (Item.Key == TEXT("value"))
					{
						Change.Value = FUeAgentCanonicalValuePtr(Item.Value);
						Change.bHasValue = true;
					}
				}
				const auto* Observation = Observations.FindByPredicate(
					[&Change](const FUeAgentEditorProjectSettingsFieldObservation& Candidate)
					{ return Candidate.FieldRef == Change.FieldRef; });
				if (!Observation)
				{
					auto Error = NativeValueFailure(TEXT("editor_project_settings_field_unresolved"),
						TEXT("The requested settings field is absent from the current settings snapshot."),
						ItemPath + TEXT("/field_ref"), MakeShared<FJsonValueString>(Change.FieldRef),
						MakeShared<FJsonValueString>(TEXT("an observed settings field")));
					Error.CandidateName = Change.FieldRef;
					return Fail(OutError, MoveTemp(Error), Change.FieldRef);
				}
				Change.ExpectedRevision = Observation->FieldRevision;
				if (Change.FieldRef.IsEmpty() || Change.Presence.IsEmpty() || Change.TypeId.IsEmpty())
				{
					const FString Field = Change.FieldRef.IsEmpty() ? TEXT("field_ref")
						: Change.Presence.IsEmpty() ? TEXT("presence") : TEXT("type_id");
					auto Error = NativeValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("The settings change requires a nonempty string field: ") + Field + TEXT("."),
						ItemPath + TEXT("/") + Field, NativeStringOrKind(FindObjectField(*Unit, Field)),
						MakeShared<FJsonValueString>(TEXT("nonempty string")));
					Error.CandidateName = Change.FieldRef;
					return Fail(OutError, MoveTemp(Error), Change.FieldRef);
				}
				if ((Change.Presence == TEXT("set")) != Change.bHasValue)
				{
					auto Error = NativeValueFailure(TEXT("editor_project_settings_change_invalid"),
						TEXT("A set operation requires a value; other settings operations omit it."),
						ItemPath + TEXT("/value"), MakeShared<FJsonValueBoolean>(Change.bHasValue),
						MakeShared<FJsonValueBoolean>(Change.Presence == TEXT("set")));
					Error.CandidateName = Change.FieldRef;
					return Fail(OutError, MoveTemp(Error), Change.FieldRef);
				}
				FString CurrentRevision;
				if (!Resolver.TryResolveSettingsField(SettingsObject, Change.FieldRef, Change.ExpectedRevision,
					Change.Property, CurrentRevision, OutError))
				{
					if (OutError.IsEmpty()) OutError = NativeCondition(TEXT("editor_project_settings_change_invalid"),
						TEXT("The settings resolver could not resolve the field at its captured revision."),
						ItemPath + TEXT("/field_resolved"));
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Change.FieldRef;
					NativeSupplement(OutError, FString(), ItemPath);
					return false;
				}
				if (!Change.Property || CurrentRevision != Change.ExpectedRevision)
				{
					auto Error = NativeCondition(TEXT("editor_project_settings_field_revision_stale"),
						!Change.Property ? TEXT("The resolved settings field has no native property.")
							: TEXT("The settings field changed after its revision was captured."),
						ItemPath + (!Change.Property ? TEXT("/property_available") : TEXT("/revision_matches")));
					Error.CandidateName = Change.FieldRef;
					Error.ActualEvidence = MakeShared<FJsonValueString>(CurrentRevision);
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Change.ExpectedRevision);
					return Fail(OutError, MoveTemp(Error), Change.FieldRef);
				}
				const void* Address = Change.Property->ContainerPtrToValuePtr<void>(&SettingsObject);
				Change.Property->ExportText_Direct(Change.BeforeText, Address, Address, &SettingsObject, PPF_None);
				OutChanges.Add(MoveTemp(Change));
			}
			return !OutChanges.IsEmpty() || Fail(OutError, NativeValueFailure(
				TEXT("editor_project_settings_changes_invalid"), TEXT("The settings change array is empty."),
				Path, MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one change"))));
		}

		struct FStringTableRow
		{
			FString Key;
			FString Source;
			TMap<FName, FString> Metadata;
		};

		bool TryReadStringTableRows(
			const FUeAgentCanonicalValue& Value, TArray<FStringTableRow>& OutRows,
			const FString& Path, FExecutionMutationError& OutError)
		{
			OutRows.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(OutError, NativeValueFailure(TEXT("editor_project_string_table_entries_invalid"),
					TEXT("String table entries must be an array."), Path, NativeKind(&Value),
					MakeShared<FJsonValueString>(TEXT("array"))));
			}
			for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
			{
				const auto& Unit = Value.GetArrayItems()[Index];
				const FString ItemPath = Path + TEXT("/") + FString::FromInt(Index);
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_string_table_entries_invalid"),
						TEXT("A string table entry must be an object."), ItemPath,
						NativeKind(&Unit.Get()), MakeShared<FJsonValueString>(TEXT("object"))));
				}
				FStringTableRow Row;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item : Unit->GetObjectItems())
				{
					if (Item.Key == TEXT("key") && Item.Value->GetKind() == EUeAgentValueKind::String)
						Row.Key = Item.Value->GetText();
					else if (Item.Key == TEXT("source") && Item.Value->GetKind() == EUeAgentValueKind::String)
						Row.Source = Item.Value->GetText();
					else if (Item.Key == TEXT("metadata") && Item.Value->GetKind() == EUeAgentValueKind::Object)
					{
						for (const TPair<FString, FUeAgentCanonicalValueRef>& Metadata : Item.Value->GetObjectItems())
						{
							if (Metadata.Value->GetKind() != EUeAgentValueKind::String)
							{
								auto Error = NativeValueFailure(TEXT("editor_project_string_table_entries_invalid"),
									TEXT("String table metadata values must be strings."),
									ItemPath + TEXT("/metadata/") + NativePointerPart(Metadata.Key),
									NativeKind(&Metadata.Value.Get()), MakeShared<FJsonValueString>(TEXT("string")));
								Error.CandidateName = Metadata.Key;
								return Fail(OutError, MoveTemp(Error));
							}
							Row.Metadata.Add(FName(*Metadata.Key), Metadata.Value->GetText());
						}
					}
				}
				if (Row.Key.IsEmpty())
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_string_table_entries_invalid"),
						TEXT("The string table entry requires a nonempty string key."), ItemPath + TEXT("/key"),
						NativeStringOrKind(FindObjectField(*Unit, TEXT("key"))),
						MakeShared<FJsonValueString>(TEXT("nonempty string"))));
				}
				OutRows.Add(MoveTemp(Row));
			}
			return true;
		}

		struct FStringTableSnapshot
		{
			FString Namespace;
			TMap<FString, FString> Sources;
			TMap<FString, TMap<FName, FString>> Metadata;
		};

		void CaptureStringTable(
			const FStringTable& Table,
			FStringTableSnapshot& OutSnapshot)
		{
			OutSnapshot.Namespace = Table.GetNamespace();
			Table.EnumerateSourceStrings(
				[&](const FString& Key, const FString& Source)
				{
					OutSnapshot.Sources.Add(Key, Source);
					TMap<FName, FString>& Metadata =
						OutSnapshot.Metadata.FindOrAdd(Key);
					Table.EnumerateMetaData(
						Key,
						[&](
							const FName MetadataKey,
							const FString& MetadataValue)
						{
							Metadata.Add(
								MetadataKey,
								MetadataValue);
							return true;
						});
					return true;
				});
		}

		void RestoreStringTable(
			FStringTable& Table,
			const FStringTableSnapshot& Snapshot)
		{
			Table.ClearMetaData();
			Table.ClearSourceStrings();
			Table.SetNamespace(Snapshot.Namespace);
			for (const TPair<FString, FString>& Source
				: Snapshot.Sources)
			{
				Table.SetSourceString(Source.Key, Source.Value);
				if (const TMap<FName, FString>* Metadata =
					Snapshot.Metadata.Find(Source.Key))
				{
					for (const TPair<FName, FString>& Item
						: *Metadata)
					{
						Table.SetMetaData(
							Source.Key,
							Item.Key,
							Item.Value);
					}
				}
			}
		}

		struct FAction
		{
			FString CanonicalKey;
			FString ProfileId;
			FString TargetRef;
			FString TargetRevision;
			FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
			TArray<FTypedChange> Changes;
			TArray<FSettingsChange> SettingsChanges;
			TUniquePtr<FConfigFile> BeforeConfig;
			TSet<FString> BeforeLocalizationDirectories;
			FStringTableSnapshot BeforeStringTable;
			bool bPackageWasDirty = false;
			bool bApplied = false;
		};

		bool ValidateFieldBinding(
			const FString& NativeBindingId, const FTypedChange& Change,
			FExecutionMutationError& OutError)
		{
			FString ExpectedBindingId;
			FString ExpectedTypeId;
			EUeAgentValueKind ExpectedKind = EUeAgentValueKind::Null;
			TArray<FString> AllowedPresences;
			UE::UAI::Contract::FDiagnostic BindingError;
			if (!TryResolveEditorProjectSimpleEffectFieldContract(NativeBindingId, Change.FieldId,
				ExpectedBindingId, ExpectedTypeId, ExpectedKind, AllowedPresences, BindingError))
			{
				OutError = MoveTemp(BindingError);
				NativeSupplement(OutError, FString(), Change.Path + TEXT("/field_id"));
				return false;
			}
			if (ExpectedBindingId != Change.FieldBindingId)
			{
				auto Error = NativeCondition(TEXT("editor_project_native_field_binding_mismatch"),
					TEXT("The field binding does not match the registered binding for this field."),
					Change.Path + TEXT("/field_binding_matches"));
				Error.CandidateName = Change.FieldId;
				Error.ActualEvidence = MakeShared<FJsonValueString>(Change.FieldBindingId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedBindingId);
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			if (!AllowedPresences.Contains(Change.Presence))
			{
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const FString& Presence : AllowedPresences) Choices.Add(MakeShared<FJsonValueString>(Presence));
				auto Error = NativeValueFailure(TEXT("editor_project_native_field_binding_mismatch"),
					TEXT("The requested operation is not supported by this field."),
					Change.Path + TEXT("/presence"), MakeShared<FJsonValueString>(Change.Presence),
					MakeShared<FJsonValueArray>(MoveTemp(Choices)));
				Error.CandidateName = Change.FieldId;
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			if (Change.bHasValue && ExpectedTypeId != Change.TypeId)
			{
				auto Error = NativeValueFailure(TEXT("editor_project_native_field_binding_mismatch"),
					TEXT("The field value type does not match its registered type."),
					Change.Path + TEXT("/value/type_id"), MakeShared<FJsonValueString>(Change.TypeId),
					MakeShared<FJsonValueString>(ExpectedTypeId));
				Error.CandidateName = Change.FieldId;
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			if (Change.bHasValue && (!Change.Value.IsValid() || Change.Value->GetKind() != ExpectedKind))
			{
				auto Error = NativeValueFailure(TEXT("editor_project_native_field_binding_mismatch"),
					TEXT("The field value has a different canonical kind from its registered type."),
					Change.Path + TEXT("/value/canonical_value"), NativeKind(Change.Value.Get()),
					MakeShared<FJsonValueString>(NativeKindName(ExpectedKind)));
				Error.CandidateName = Change.FieldId;
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			if (!Change.bHasValue && !Change.TypeId.IsEmpty())
			{
				auto Error = NativeValueFailure(TEXT("editor_project_native_field_binding_mismatch"),
					TEXT("A field change without a value must omit the value type."),
					Change.Path + TEXT("/value/type_id"), MakeShared<FJsonValueString>(Change.TypeId),
					MakeShared<FJsonValueString>(TEXT("")));
				Error.CandidateName = Change.FieldId;
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			return true;
		}

		bool PrepareAction(
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
			const FPlannedChange& Change, FAction& OutAction, FExecutionMutationError& OutError)
		{
			OutAction = FAction();
			OutAction.CanonicalKey = Change.GetIdentity().GetCanonicalKey();
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty()) NativeSupplement(OutError, OutAction.TargetRef);
			};
			FString NativeOperationType;
			FString NativeBindingId;
			FString NativeMechanicId;
			for (const auto& Field : {
				TPair<const TCHAR*, FString*>(TEXT("native_operation_type"), &NativeOperationType),
				TPair<const TCHAR*, FString*>(TEXT("native_binding_id"), &NativeBindingId),
				TPair<const TCHAR*, FString*>(TEXT("native_mechanic_id"), &NativeMechanicId),
				TPair<const TCHAR*, FString*>(TEXT("profile_id"), &OutAction.ProfileId),
				TPair<const TCHAR*, FString*>(TEXT("target_ref"), &OutAction.TargetRef),
				TPair<const TCHAR*, FString*>(TEXT("target_revision"), &OutAction.TargetRevision)})
			{
				if (!TryReadString(Change.GetAfter(), Field.Key, *Field.Value))
				{
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_native_change_invalid"),
						TEXT("The native change requires a string field: ") + FString(Field.Key) + TEXT("."),
						TEXT("/after/") + FString(Field.Key), NativeKind(FindObjectField(Change.GetAfter(), Field.Key)),
						MakeShared<FJsonValueString>(TEXT("string"))));
				}
			}
			if (!TryReadTypedChanges(Change.GetAfter(), OutAction.Changes, OutError)) return false;

			FString BoundOperationType;
			FString BoundProfileId;
			FString BoundMechanicId;
			UE::UAI::Contract::FDiagnostic BindingError;
			if (!TryResolveEditorProjectSimpleEffectNativeBinding(NativeBindingId,
				BoundOperationType, BoundProfileId, BoundMechanicId, BindingError))
			{
				return Fail(OutError, MoveTemp(BindingError), NativeBindingId);
			}
			if (BoundOperationType != NativeOperationType
				|| BoundOperationType != Change.GetIdentity().GetTargetKind()
				|| BoundProfileId != OutAction.ProfileId || BoundMechanicId != NativeMechanicId)
			{
				const FString Field = BoundOperationType != NativeOperationType ? TEXT("operation_type_matches")
					: BoundOperationType != Change.GetIdentity().GetTargetKind() ? TEXT("target_kind_matches")
					: BoundProfileId != OutAction.ProfileId ? TEXT("profile_matches") : TEXT("mechanic_matches");
				auto Error = NativeCondition(TEXT("editor_project_native_binding_mismatch"),
					TEXT("The native change disagrees with its registered binding: ") + Field + TEXT("."),
					TEXT("/after/") + Field);
				const auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("operation_type"), NativeOperationType);
				Actual->SetStringField(TEXT("target_kind"), Change.GetIdentity().GetTargetKind());
				Actual->SetStringField(TEXT("profile"), OutAction.ProfileId);
				Actual->SetStringField(TEXT("mechanic"), NativeMechanicId);
				const auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("operation_type"), BoundOperationType);
				Expected->SetStringField(TEXT("profile"), BoundProfileId);
				Expected->SetStringField(TEXT("mechanic"), BoundMechanicId);
				Error.ActualEvidence = MakeShared<FJsonValueObject>(Actual);
				Error.ExpectedEvidence = MakeShared<FJsonValueObject>(Expected);
				return Fail(OutError, MoveTemp(Error), NativeBindingId);
			}
			for (const FTypedChange& TypedChange : OutAction.Changes)
			{
				if (!ValidateFieldBinding(NativeBindingId, TypedChange, OutError)) return false;
			}
			if (!Resolver.TryResolveTarget(OutAction.TargetRef, OutAction.Target, OutError)
				|| !OutAction.Target.IsValid())
			{
				if (OutError.IsEmpty()) OutError = NativeUnresolvedTarget(OutAction.Target, OutAction.TargetRef);
				return false;
			}
			if (OutAction.Target.GetRevision() != OutAction.TargetRevision)
			{
				auto Error = NativeCondition(TEXT("editor_project_target_revision_stale"),
					TEXT("The editor project target changed after its revision was captured."),
					TEXT("/after/target_revision_matches"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(OutAction.Target.GetRevision());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(OutAction.TargetRevision);
				return Fail(OutError, MoveTemp(Error), OutAction.TargetRef);
			}
			if (OutAction.Target.GetProfileId() != OutAction.ProfileId)
			{
				auto Error = NativeValueFailure(TEXT("editor_project_target_profile_mismatch"),
					TEXT("The resolved target kind differs from the requested target kind."),
					TEXT("/after/profile_id"), MakeShared<FJsonValueString>(NativeTargetKind(OutAction.Target.GetProfileId())),
					MakeShared<FJsonValueString>(NativeTargetKind(OutAction.ProfileId)));
				Error.ActualEvidence = MakeShared<FJsonValueString>(OutAction.Target.GetProfileId());
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(OutAction.ProfileId);
				return Fail(OutError, MoveTemp(Error), OutAction.TargetRef);
			}

			if (FConfigFile* Config = OutAction.Target.GetConfigDocument())
				OutAction.BeforeConfig = MakeUnique<FConfigFile>(*Config);
			if (TSet<FString>* Directories = OutAction.Target.GetLocalizationDirectories())
				OutAction.BeforeLocalizationDirectories = *Directories;
			if (UObject* Settings = OutAction.Target.GetSettingsObject())
				OutAction.bPackageWasDirty = Settings->GetOutermost()->IsDirty();
			if (UStringTable* StringTable = OutAction.Target.GetStringTable())
			{
				OutAction.bPackageWasDirty = StringTable->GetOutermost()->IsDirty();
				CaptureStringTable(*StringTable->GetStringTable(), OutAction.BeforeStringTable);
			}
			for (const FTypedChange& TypedChange : OutAction.Changes)
			{
				const FString ValuePath = TypedChange.Path + TEXT("/value/canonical_value");
				if (OutAction.ProfileId == TEXT("project_settings") && TypedChange.FieldId == TEXT("properties"))
				{
					if (!TypedChange.Value.IsValid())
						return Fail(OutError, NativeValueFailure(TEXT("editor_project_settings_changes_invalid"),
							TEXT("The settings properties change has no value."), ValuePath,
							NativeKind(nullptr), MakeShared<FJsonValueString>(TEXT("array"))));
					if (!TryReadSettingsChanges(*TypedChange.Value, *OutAction.Target.GetSettingsObject(),
						Resolver, OutAction.SettingsChanges, ValuePath, OutError)) return false;
				}
				if (OutAction.ProfileId == TEXT("device_profile")
					&& TypedChange.FieldId == TEXT("base_profile_ref") && TypedChange.Presence == TEXT("set"))
				{
					if (!TypedChange.Value.IsValid() || TypedChange.Value->GetKind() != EUeAgentValueKind::String)
						return Fail(OutError, NativeValueFailure(TEXT("editor_project_device_profile_base_unresolved"),
							TEXT("The base device profile reference must be a string."), ValuePath,
							NativeKind(TypedChange.Value.Get()), MakeShared<FJsonValueString>(TEXT("string"))));
					const FString& BaseRef = TypedChange.Value->GetText();
					if (BaseRef == OutAction.TargetRef)
						return Fail(OutError, NativeValueFailure(TEXT("editor_project_device_profile_base_unresolved"),
							TEXT("A device profile cannot use itself as its base profile."), ValuePath,
							MakeShared<FJsonValueString>(BaseRef),
							MakeShared<FJsonValueString>(TEXT("a different device profile"))));
					FUeAgentEditorProjectSimpleEffectResolvedTarget BaseTarget;
					if (!Resolver.TryResolveTarget(BaseRef, BaseTarget, OutError) || !BaseTarget.IsValid())
					{
						if (OutError.IsEmpty()) OutError = NativeUnresolvedTarget(BaseTarget, BaseRef);
						NativeSupplement(OutError, BaseRef, ValuePath);
						return false;
					}
					if (BaseTarget.GetProfileId() != TEXT("device_profile"))
					{
						auto Error = NativeValueFailure(TEXT("editor_project_device_profile_base_unresolved"),
							TEXT("The requested base reference resolves to a different target kind."),
							ValuePath, MakeShared<FJsonValueString>(NativeTargetKind(BaseTarget.GetProfileId())),
							MakeShared<FJsonValueString>(TEXT("device profile")));
						Error.CandidateName = BaseRef;
						Error.ActualEvidence = MakeShared<FJsonValueString>(BaseTarget.GetProfileId());
						return Fail(OutError, MoveTemp(Error));
					}
				}
			}
			return true;
		}

		void ApplyStringMap(
			FConfigFile& Config,
			const FString& Section,
			const FString& Presence,
			const TMap<FString, FString>& Values)
		{
			if (Presence == TEXT("replace")
				|| Presence == TEXT("clear"))
			{
				Config.Remove(Section);
			}
			if (Presence == TEXT("remove"))
			{
				for (const TPair<FString, FString>& Item : Values)
				{
					Config.RemoveKeyFromSection(
						*Section,
						FName(*Item.Key));
				}
				return;
			}
			if (Presence == TEXT("clear"))
			{
				return;
			}
			for (const TPair<FString, FString>& Item : Values)
			{
				Config.SetString(
					*Section,
					*Item.Key,
					*Item.Value);
			}
		}

		TMap<FString, FString> ReadCVarMap(
			const FConfigFile& Config,
			const FString& Section)
		{
			TMap<FString, FString> Result;
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
				if (Value.GetValue().Split(
						TEXT("="),
						&Key,
						&Data))
				{
					Result.Add(Key, Data);
				}
			}
			return Result;
		}

		void WriteCVarMap(
			FConfigFile& Config,
			const FString& Section,
			const TMap<FString, FString>& Values)
		{
			Config.RemoveKeyFromSection(
				*Section,
				FName(TEXT("CVars")));
			TArray<FString> Keys;
			Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				Config.AddToSection(
					*Section,
					FName(TEXT("CVars")),
					Key + TEXT("=") + Values.FindChecked(Key));
			}
		}

		bool ApplyPropertyValue(
			UObject& Object, const FSettingsChange& Change, FExecutionMutationError& OutError)
		{
			if (!Change.Property)
			{
				auto Error = NativeCondition(TEXT("editor_project_settings_field_unresolved"),
					TEXT("The settings field has no native property."), Change.Path + TEXT("/property_available"));
				Error.CandidateName = Change.FieldRef;
				return Fail(OutError, MoveTemp(Error), Change.FieldRef);
			}
			void* Address = Change.Property->ContainerPtrToValuePtr<void>(&Object);
			Object.PreEditChange(Change.Property);
			bool bApplied = true;
			FString ExpectedKind;
			FString FailureReason;
			TSharedPtr<FJsonValue> Actual = NativeKind(Change.Value.Get());
			if (Change.Presence != TEXT("set"))
			{
				Change.Property->ClearValue(Address);
			}
			else if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Change.Property))
			{
				bApplied = Change.Value.IsValid() && Change.Value->GetKind() == EUeAgentValueKind::Boolean;
				ExpectedKind = TEXT("boolean");
				FailureReason = TEXT("The settings boolean field requires a boolean value.");
				if (bApplied) BoolProperty->SetPropertyValue(Address, Change.Value->GetBoolean());
			}
			else if (const FStrProperty* StringProperty = CastField<FStrProperty>(Change.Property))
			{
				bApplied = Change.Value.IsValid() && Change.Value->GetKind() == EUeAgentValueKind::String;
				ExpectedKind = TEXT("string");
				FailureReason = TEXT("The settings string field requires a string value.");
				if (bApplied) StringProperty->SetPropertyValue(Address, Change.Value->GetText());
			}
			else if (const FIntProperty* IntProperty = CastField<FIntProperty>(Change.Property))
			{
				int64 Parsed = 0;
				const bool bNumeric = Change.Value.IsValid()
					&& (Change.Value->GetKind() == EUeAgentValueKind::Integer
						|| Change.Value->GetKind() == EUeAgentValueKind::Decimal);
				const bool bParsed = bNumeric && TryParseInt64Number(Change.Value->GetLexeme(), Parsed);
				bApplied = bParsed && Parsed >= MIN_int32 && Parsed <= MAX_int32;
				ExpectedKind = TEXT("integral number in [-2147483648, 2147483647]");
				FailureReason = !bNumeric ? TEXT("The settings int32 field requires a numeric value.")
					: !bParsed ? TEXT("The settings value cannot be parsed as an integral int64 number.")
					: TEXT("The settings value is outside the int32 range.");
				if (bNumeric) Actual = MakeShared<FJsonValueString>(Change.Value->GetLexeme());
				if (bApplied) IntProperty->SetPropertyValue(Address, static_cast<int32>(Parsed));
			}
			else if (const FInt64Property* Int64Property = CastField<FInt64Property>(Change.Property))
			{
				int64 Parsed = 0;
				const bool bNumeric = Change.Value.IsValid()
					&& (Change.Value->GetKind() == EUeAgentValueKind::Integer
						|| Change.Value->GetKind() == EUeAgentValueKind::Decimal);
				bApplied = bNumeric && TryParseInt64Number(Change.Value->GetLexeme(), Parsed);
				ExpectedKind = TEXT("integral number in [-9223372036854775808, 9223372036854775807]");
				FailureReason = !bNumeric ? TEXT("The settings int64 field requires a numeric value.")
					: TEXT("The settings value cannot be parsed as an integral int64 number.");
				if (bNumeric) Actual = MakeShared<FJsonValueString>(Change.Value->GetLexeme());
				if (bApplied) Int64Property->SetPropertyValue(Address, Parsed);
			}
			else
			{
				bApplied = false;
				Actual = MakeShared<FJsonValueString>(Change.Property->GetCPPType());
				ExpectedKind = TEXT("bool, FString, int32 or int64 native property");
				FailureReason = TEXT("The settings native property type is not supported for assignment.");
			}
			FPropertyChangedEvent ChangedEvent(Change.Property, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(ChangedEvent);
			if (bApplied) return true;
			auto Error = NativeValueFailure(TEXT("editor_project_settings_property_type_unsupported"),
				FailureReason, Change.Path + TEXT("/value"), Actual,
				MakeShared<FJsonValueString>(ExpectedKind));
			Error.CandidateName = Change.FieldRef;
			Error.SubjectId = Object.GetPathName();
			return Fail(OutError, MoveTemp(Error), Change.FieldRef);
		}

		bool RestorePropertyValue(
			UObject& Object, const FSettingsChange& Change, FExecutionMutationError& OutError)
		{
			if (!Change.Property)
			{
				auto Error = NativeCondition(TEXT("editor_project_settings_restore_property_unavailable"),
					TEXT("The settings rollback has no native property for the captured field."),
					Change.Path + TEXT("/property_available"));
				Error.CandidateName = Change.FieldRef;
				return Fail(OutError, MoveTemp(Error), Change.FieldRef);
			}
			void* Address = Change.Property->ContainerPtrToValuePtr<void>(&Object);
			Object.PreEditChange(Change.Property);
			const TCHAR* End = Change.Property->ImportText_Direct(
				*Change.BeforeText, Address, &Object, PPF_None);
			FPropertyChangedEvent ChangedEvent(Change.Property, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(ChangedEvent);
			if (End) return true;
			auto Error = NativeCondition(TEXT("editor_project_settings_restore_failed"),
				TEXT("The native property importer rejected the captured settings value during rollback."),
				Change.Path + TEXT("/restore_succeeded"));
			Error.CandidateName = Change.FieldRef;
			Error.ActualEvidence = MakeShared<FJsonValueString>(Change.BeforeText);
			Error.SubjectId = Object.GetPathName();
			return Fail(OutError, MoveTemp(Error), Change.FieldRef);
		}

		bool ApplyConfigChange(
			FAction& Action,
			const FTypedChange& Change,
			FExecutionMutationError& OutError)
		{
			FConfigFile* Config =
				Action.Target.GetConfigDocument();
			if (!Config)
			{
				return Fail(OutError, NativeCondition(TEXT("editor_project_config_target_invalid"),
						TEXT("The configuration target has no document available for the update."),
						TEXT("/config_document_available")), Action.TargetRef);
			}
			const FString& Section =
				Action.Target.GetConfigSection();
			if (Action.ProfileId == TEXT("device_profile")
				&& Change.FieldId == TEXT("device_type"))
			{
				if (Change.Presence == TEXT("clear"))
				{
					Config->RemoveKeyFromSection(
						*Section,
						FName(TEXT("DeviceType")));
				}
				else
				{
					Config->SetString(
						*Section,
						TEXT("DeviceType"),
						*Change.Value->GetText());
				}
				return true;
			}
			if (Action.ProfileId == TEXT("device_profile")
				&& Change.FieldId == TEXT("base_profile_ref"))
			{
				if (Change.Presence == TEXT("clear")
					|| Change.Presence == TEXT("null"))
				{
					Config->RemoveKeyFromSection(
						*Section,
						FName(TEXT("BaseProfileName")));
				}
				else
				{
					Config->SetString(
						*Section,
						TEXT("BaseProfileName"),
						*Change.Value->GetText());
				}
				return true;
			}
			if (Action.ProfileId == TEXT("device_profile")
				&& Change.FieldId == TEXT("cvars"))
			{
				TMap<FString, FString> Values;
				TMap<FString, FString> Current =
					ReadCVarMap(*Config, Section);
				if (Change.Presence != TEXT("clear"))
				{
					const FString Path = Change.Path + TEXT("/value/canonical_value");
					if (!Change.Value.IsValid())
						return Fail(OutError, NativeValueFailure(TEXT("editor_project_config_entry_invalid"),
							TEXT("The configuration entries change has no value."), Path, NativeKind(nullptr),
							MakeShared<FJsonValueString>(TEXT("object"))));
					if (!TryReadStringMap(*Change.Value, Values, Path, OutError)) return false;
				}
				if (Change.Presence == TEXT("replace")
					|| Change.Presence == TEXT("clear"))
				{
					Current.Reset();
				}
				if (Change.Presence == TEXT("remove"))
				{
					for (const TPair<FString, FString>& Item
						: Values)
					{
						Current.Remove(Item.Key);
					}
				}
				else
				{
					Current.Append(Values);
				}
				WriteCVarMap(*Config, Section, Current);
				return true;
			}
			if (Change.FieldId == TEXT("entries"))
			{
				TMap<FString, FString> Values;
				if (Change.Presence != TEXT("clear"))
				{
					const FString Path = Change.Path + TEXT("/value/canonical_value");
					if (!Change.Value.IsValid())
						return Fail(OutError, NativeValueFailure(TEXT("editor_project_config_entry_invalid"),
							TEXT("The configuration entries change has no value."), Path, NativeKind(nullptr),
							MakeShared<FJsonValueString>(TEXT("object"))));
					if (!TryReadStringMap(*Change.Value, Values, Path, OutError)) return false;
				}
				ApplyStringMap(
					*Config,
					Section,
					Change.Presence,
					Values);
				return true;
			}
			if (Action.ProfileId == TEXT("localization_target"))
			{
				if (Change.FieldId == TEXT("native_culture"))
				{
					Config->SetString(
						*Section,
						TEXT("NativeCulture"),
						*Change.Value->GetText());
					return true;
				}
				if (Change.FieldId == TEXT("cultures"))
				{
					TArray<FString> Requested;
					if (Change.Presence != TEXT("clear"))
					{
						const FString Path = Change.Path + TEXT("/value/canonical_value");
						if (!Change.Value.IsValid())
							return Fail(OutError, NativeValueFailure(TEXT("editor_project_culture_set_invalid"),
								TEXT("The culture change has no value."), Path, NativeKind(nullptr),
								MakeShared<FJsonValueString>(TEXT("array"))));
						if (!TryReadStringArray(*Change.Value, Requested, Path, OutError)) return false;
					}
					TSet<FString> Cultures;
					if (Change.Presence == TEXT("set")
						|| Change.Presence == TEXT("remove"))
					{
						const FConfigSection* Found =
							Config->FindSection(Section);
						if (Found)
						{
							TArray<FConfigValue> Existing;
							Found->MultiFind(
								FName(
									TEXT(
										"CulturesToGenerate")),
								Existing,
								true);
							for (const FConfigValue& Value
								: Existing)
							{
								Cultures.Add(Value.GetValue());
							}
						}
					}
					for (const FString& Culture : Requested)
					{
						if (Change.Presence == TEXT("remove"))
						{
							Cultures.Remove(Culture);
						}
						else
						{
							Cultures.Add(Culture);
						}
					}
					Config->RemoveKeyFromSection(
						*Section,
						FName(TEXT("CulturesToGenerate")));
					TArray<FString> Sorted = Cultures.Array();
					Sorted.Sort();
					for (const FString& Culture : Sorted)
					{
						Config->AddToSection(
							*Section,
							FName(
								TEXT("CulturesToGenerate")),
							Culture);
					}
					if (TSet<FString>* Directories =
						Action.Target
							.GetLocalizationDirectories())
					{
						*Directories = Cultures;
					}
					return true;
				}
			}
			auto Error = NativeValueFailure(TEXT("editor_project_native_change_unsupported"),
				TEXT("The field is not supported by the resolved ") + NativeTargetKind(Action.ProfileId) + TEXT(" target."),
				Change.Path + TEXT("/field_id"), MakeShared<FJsonValueString>(Change.FieldId),
				MakeShared<FJsonValueString>(TEXT("a registered field for this target kind")));
			Error.CandidateName = Change.FieldId;
			Error.ActualEvidence = MakeShared<FJsonValueString>(Action.ProfileId);
			return Fail(OutError, MoveTemp(Error), Change.FieldId);
		}

		bool ApplyStringTableChange(
			FAction& Action,
			const FTypedChange& Change,
			FExecutionMutationError& OutError)
		{
			UStringTable* Object =
				Action.Target.GetStringTable();
			if (!Object)
			{
				return Fail(OutError, NativeCondition(TEXT("editor_project_string_table_target_invalid"),
						TEXT("The string table target is no longer available for the update."),
						TEXT("/string_table_available")));
			}
			FStringTable& Table =
				*Object->GetMutableStringTable();
			if (Change.FieldId == TEXT("namespace"))
			{
				Table.SetNamespace(
					Change.Presence == TEXT("clear")
						? FString()
						: Change.Value->GetText());
				return true;
			}
			if (Change.FieldId != TEXT("entries"))
			{
				auto Error = NativeValueFailure(TEXT("editor_project_native_change_unsupported"),
					TEXT("The string table field is unsupported."), Change.Path + TEXT("/field_id"),
					MakeShared<FJsonValueString>(Change.FieldId), MakeShared<FJsonValueString>(TEXT("namespace or entries")));
				Error.CandidateName = Change.FieldId;
				return Fail(OutError, MoveTemp(Error), Change.FieldId);
			}
			TArray<FStringTableRow> Rows;
			if (Change.Presence != TEXT("clear"))
			{
				const FString Path = Change.Path + TEXT("/value/canonical_value");
				if (!Change.Value.IsValid())
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_string_table_entries_invalid"),
						TEXT("The string table entries change has no value."), Path, NativeKind(nullptr),
						MakeShared<FJsonValueString>(TEXT("array"))));
				if (!TryReadStringTableRows(*Change.Value, Rows, Path, OutError)) return false;
			}
			if (Change.Presence == TEXT("replace")
				|| Change.Presence == TEXT("clear"))
			{
				Table.ClearMetaData();
				Table.ClearSourceStrings();
			}
			for (const FStringTableRow& Row : Rows)
			{
				if (Change.Presence == TEXT("remove"))
				{
					Table.ClearMetaData(Row.Key);
					Table.RemoveSourceString(Row.Key);
					continue;
				}
				Table.SetSourceString(Row.Key, Row.Source);
				Table.ClearMetaData(Row.Key);
				for (const TPair<FName, FString>& Metadata
					: Row.Metadata)
				{
					Table.SetMetaData(
						Row.Key,
						Metadata.Key,
						Metadata.Value);
				}
			}
			return true;
		}

		void NativeCollectFailure(FExecutionMutationError& OutError, FExecutionMutationError Error)
		{
			if (OutError.IsEmpty()) OutError = MoveTemp(Error);
			else OutError.Diagnostics.Add(MoveTemp(Error));
		}

		void NativeAttachRollback(FExecutionMutationError& Failure, bool bConfirmed,
			FExecutionMutationError RecoveryError)
		{
			const bool bEarlierConfirmed = !Failure.RollbackSucceeded.IsSet() || Failure.RollbackSucceeded.GetValue();
			Failure.RollbackAttempted = true;
			Failure.RollbackSucceeded = bEarlierConfirmed && bConfirmed;
			Failure.MutationMayHaveStarted = true;
			if (!RecoveryError.IsEmpty()) Failure.Diagnostics.Add(MoveTemp(RecoveryError));
		}

		bool RollbackAction(FAction& Action, FExecutionMutationError& OutError)
		{
			OutError = FExecutionMutationError();
			bool bConfirmed = true;
			auto MissingTarget = [&](const FString& Reason, const FString& Field)
			{
				auto Error = NativeCondition(TEXT("editor_project_rollback_target_unavailable"), Reason,
					TEXT("/rollback/") + Field);
				NativeSupplement(Error, Action.TargetRef);
				NativeCollectFailure(OutError, MoveTemp(Error));
				bConfirmed = false;
			};
			if (Action.BeforeConfig.IsValid())
			{
				if (FConfigFile* Config = Action.Target.GetConfigDocument()) *Config = *Action.BeforeConfig;
				else MissingTarget(TEXT("The captured configuration cannot be restored because its document is unavailable."),
					TEXT("config_document_available"));
			}
			if (TSet<FString>* Directories = Action.Target.GetLocalizationDirectories())
				*Directories = Action.BeforeLocalizationDirectories;
			if (UObject* Settings = Action.Target.GetSettingsObject())
			{
				for (int32 Index = Action.SettingsChanges.Num() - 1; Index >= 0; --Index)
				{
					FExecutionMutationError RestoreError;
					if (!RestorePropertyValue(*Settings, Action.SettingsChanges[Index], RestoreError))
					{
						NativeSupplement(RestoreError, Action.TargetRef);
						NativeCollectFailure(OutError, MoveTemp(RestoreError));
						bConfirmed = false;
					}
				}
				Settings->GetOutermost()->SetDirtyFlag(Action.bPackageWasDirty);
			}
			else if (Action.ProfileId == TEXT("project_settings"))
				MissingTarget(TEXT("The captured settings cannot be restored because the settings object is no longer alive."),
					TEXT("settings_object_available"));
			if (UStringTable* StringTable = Action.Target.GetStringTable())
			{
				RestoreStringTable(*StringTable->GetMutableStringTable(), Action.BeforeStringTable);
				StringTable->GetOutermost()->SetDirtyFlag(Action.bPackageWasDirty);
			}
			else if (Action.ProfileId == TEXT("string_table"))
				MissingTarget(TEXT("The captured string table cannot be restored because the string table is no longer alive."),
					TEXT("string_table_available"));
			if (bConfirmed) Action.bApplied = false;
			if (!OutError.IsEmpty())
			{
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
				OutError.MutationMayHaveStarted = true;
			}
			return bConfirmed;
		}

		bool ApplyAction(FAction& Action, FExecutionMutationError& OutError)
		{
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty()) NativeSupplement(OutError, Action.TargetRef);
			};
			if (Action.ProfileId == TEXT("project_settings"))
			{
				UObject* Settings = Action.Target.GetSettingsObject();
				if (!Settings)
					return Fail(OutError, NativeCondition(TEXT("editor_project_settings_target_invalid"),
						TEXT("The settings object is no longer available for the update."),
						TEXT("/settings_object_available")));
				for (const FSettingsChange& Change : Action.SettingsChanges)
				{
					if (!ApplyPropertyValue(*Settings, Change, OutError))
					{
						FExecutionMutationError RecoveryError;
						const bool bConfirmed = RollbackAction(Action, RecoveryError);
						NativeAttachRollback(OutError, bConfirmed, MoveTemp(RecoveryError));
						return false;
					}
				}
				Settings->GetOutermost()->MarkPackageDirty();
				Action.bApplied = true;
				return true;
			}
			for (const FTypedChange& Change : Action.Changes)
			{
				const bool bChanged = Action.ProfileId == TEXT("string_table")
					? ApplyStringTableChange(Action, Change, OutError)
					: ApplyConfigChange(Action, Change, OutError);
				if (!bChanged)
				{
					FExecutionMutationError RecoveryError;
					const bool bConfirmed = RollbackAction(Action, RecoveryError);
					NativeAttachRollback(OutError, bConfirmed, MoveTemp(RecoveryError));
					return false;
				}
			}
			if (UStringTable* StringTable = Action.Target.GetStringTable())
				StringTable->GetOutermost()->MarkPackageDirty();
			Action.bApplied = true;
			return true;
		}

		bool RollbackAll(TArray<FAction>& Actions, FExecutionMutationError& OutError)
		{
			OutError = FExecutionMutationError();
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1; Index >= 0; --Index)
			{
				FExecutionMutationError ActionError;
				if (!RollbackAction(Actions[Index], ActionError))
				{
					NativeCollectFailure(OutError, MoveTemp(ActionError));
					bConfirmed = false;
				}
			}
			return bConfirmed;
		}

		class FEditorProjectSimpleEffectNativeHook final
			: public IUeAgentEditorProjectSimpleEffectNativeHook
		{
		public:
			explicit FEditorProjectSimpleEffectNativeHook(
				TSharedRef<
					IUeAgentEditorProjectSimpleEffectTargetResolver,
					ESPMode::ThreadSafe> InResolver)
				: Resolver(MoveTemp(InResolver))
			{
			}

			bool TryApply(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				const TArray<FUeAgentGenericEffectBinding>& Bindings,
				FUeAgentResourceTracker& Resources,
				FUeAgentGenericEffectApplyResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
					return Fail(OutError, NativeCondition(TEXT("editor_project_native_request_invalid"),
						TEXT("Editor project updates must execute on the game thread."), TEXT("/game_thread")));
				if (!Request.IsValid())
				{
					OutError = Request.GetValidationDiagnostic();
					NativeSupplement(OutError, FString());
					return false;
				}
				if (!Lease.IsValid())
				{
					OutError = Lease.GetValidationDiagnostic();
					NativeSupplement(OutError, FString());
					return false;
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetNumberField(TEXT("changes"), Changes.Num());
					Actual->SetNumberField(TEXT("bindings"), Bindings.Num());
					return Fail(OutError, NativeValueFailure(TEXT("editor_project_change_binding_mismatch"),
						Changes.IsEmpty() ? TEXT("The native execution request contains no changes.")
							: TEXT("The native binding count differs from the planned change count."),
						TEXT("/changes"), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("a nonempty change list with one binding per change"))));
				}
				for (int32 Index = 0; Index < Bindings.Num(); ++Index)
				{
					if (!Bindings[Index].IsValid())
					{
						OutError = Bindings[Index].GetValidationDiagnostic();
						NativeSupplement(OutError, FString(), TEXT("/bindings/") + FString::FromInt(Index));
						return false;
					}
				}

				TArray<FAction> Actions;
				Actions.Reserve(Changes.Num());
				for (const FPlannedChange& Change : Changes)
				{
					FAction Action;
					if (!PrepareAction(
							*Resolver,
							Change,
							Action,
							OutError))
					{
						return false;
					}
					Actions.Add(MoveTemp(Action));
				}

				TArray<FAction> AppliedActions;
				TArray<FString> MutatedKeys;
				for (FAction& Action : Actions)
				{
					FUeAgentEditorProjectSimpleEffectResolvedTarget
						CurrentTarget;
					FExecutionMutationError ResolveError;
					if (!Resolver->TryResolveTarget(
							Action.TargetRef,
							CurrentTarget,
							ResolveError)
						|| !CurrentTarget.IsValid()
						|| CurrentTarget.GetCanonicalRef()
							!= Action.TargetRef
						|| CurrentTarget.GetProfileId()
							!= Action.ProfileId
						|| CurrentTarget.GetRevision()
							!= Action.TargetRevision)
					{
						if (!ResolveError.IsEmpty()) OutError = MoveTemp(ResolveError);
						else if (!CurrentTarget.IsValid()) OutError = NativeUnresolvedTarget(CurrentTarget, Action.TargetRef);
						else if (CurrentTarget.GetCanonicalRef() != Action.TargetRef)
							OutError = NativeValueFailure(TEXT("editor_project_target_revision_stale"),
								TEXT("The target resolver returned a different editor project target."),
								TEXT("/target_ref"), MakeShared<FJsonValueString>(CurrentTarget.GetCanonicalRef()),
								MakeShared<FJsonValueString>(Action.TargetRef));
						else if (CurrentTarget.GetProfileId() != Action.ProfileId)
						{
							OutError = NativeValueFailure(TEXT("editor_project_target_revision_stale"),
								TEXT("The editor project target kind changed before the update."),
								TEXT("/target_kind"), MakeShared<FJsonValueString>(NativeTargetKind(CurrentTarget.GetProfileId())),
								MakeShared<FJsonValueString>(NativeTargetKind(Action.ProfileId)));
							OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentTarget.GetProfileId());
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Action.ProfileId);
						}
						else
						{
							OutError = NativeCondition(TEXT("editor_project_target_revision_stale"),
								TEXT("The editor project target changed after the update was prepared."),
								TEXT("/target_revision_matches"));
							OutError.ActualEvidence = MakeShared<FJsonValueString>(CurrentTarget.GetRevision());
							OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Action.TargetRevision);
						}
						NativeSupplement(OutError, Action.TargetRef);
						if (!AppliedActions.IsEmpty())
						{
							FExecutionMutationError RecoveryError;
							const bool bConfirmed = RollbackAll(AppliedActions, RecoveryError);
							NativeAttachRollback(OutError, bConfirmed, MoveTemp(RecoveryError));
						}
						return false;
					}
					OutResult = FUeAgentGenericEffectApplyResult();
					if (!ApplyAction(Action, OutError))
					{
						if (!AppliedActions.IsEmpty())
						{
							FExecutionMutationError RecoveryError;
							const bool bConfirmed = RollbackAll(AppliedActions, RecoveryError);
							NativeAttachRollback(OutError, bConfirmed, MoveTemp(RecoveryError));
						}
						return false;
					}
					MutatedKeys.Add(Action.CanonicalKey);
					AppliedActions.Add(MoveTemp(Action));
				}

				const FString Token = FString::Printf(
					TEXT("editor-project:%s:%lld"),
					*Request.GetOperationId(),
					Lease.GetFencingToken());
				{
					FScopeLock Lock(&StateMutex);
					RollbackByToken.Add(Token, MoveTemp(AppliedActions));
					TokenByOperation.Add(Request.GetOperationId(), Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("editor_project.authoring.memory")},
					true,
					Token,
					true);
				if (OutResult.IsValid()) return true;
				OutError = OutResult.GetValidationDiagnostic();
				NativeSupplement(OutError, FString());
				return false;
			}

			bool TryRollback(
				const FExecutionMutationRequest& Request,
				const FUeAgentGenericEffectApplyResult& Applied,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				bOutConfirmed = false;
				OutError = FExecutionMutationError();
				if (!IsInGameThread())
					return Fail(OutError, NativeCondition(TEXT("editor_project_rollback_request_invalid"),
						TEXT("Editor project rollback must execute on the game thread."), TEXT("/game_thread")));
				if (!Request.IsValid())
				{
					OutError = Request.GetValidationDiagnostic();
					NativeSupplement(OutError, FString());
					return false;
				}
				if (!Applied.IsValid())
				{
					OutError = Applied.GetValidationDiagnostic();
					NativeSupplement(OutError, FString());
					return false;
				}
				FScopeLock Lock(&StateMutex);
				TArray<FAction>* Actions =
					RollbackByToken.Find(
						Applied.GetRollbackToken());
				if (!Actions)
				{
					auto Error = NativeCondition(TEXT("editor_project_rollback_token_unknown"),
						TEXT("No captured editor project updates are registered for this rollback token."),
						TEXT("/rollback_state_available"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(Applied.GetRollbackToken());
					Error.RollbackAttempted = false;
					return Fail(OutError, MoveTemp(Error));
				}
				FExecutionMutationError RecoveryError;
				bOutConfirmed = RollbackAll(*Actions, RecoveryError);
				if (bOutConfirmed)
				{
					RollbackByToken.Remove(
						Applied.GetRollbackToken());
					TokenByOperation.Remove(
						Request.GetOperationId());
				}
				if (bOutConfirmed) return true;
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("editor_project_rollback_unconfirmed"),
					RecoveryError, TEXT("native_execution"), TEXT("The captured editor project updates could not all be restored."));
				OutError.RollbackAttempted = true;
				OutError.RollbackSucceeded = false;
				OutError.MutationMayHaveStarted = true;
				return false;
			}

			void ReleaseVerifiedOperation(
				const FString& OperationId) override
			{
				FScopeLock Lock(&StateMutex);
				if (const FString* Token =
					TokenByOperation.Find(OperationId))
				{
					RollbackByToken.Remove(*Token);
				}
				TokenByOperation.Remove(OperationId);
			}

			bool TryWaitForSettle(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError();
				if (!Request.IsValid())
				{
					auto Error = NativeCondition(TEXT("editor_project_barrier_request_invalid"),
						TEXT("The settle request has a negative subject generation."), TEXT("/subject_generation_valid"));
					Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
					return Fail(OutError, MoveTemp(Error));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("editor-project:%lld"),
						Request.GetSubjectGeneration()),
					true);
				if (OutResult.IsValid()) return true;
				OutError = OutResult.GetValidationDiagnostic();
				NativeSupplement(OutError, FString());
				return false;
			}

		private:
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> Resolver;
			FCriticalSection StateMutex;
			TMap<FString, TArray<FAction>> RollbackByToken;
			TMap<FString, FString> TokenByOperation;
		};
	}

	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		FUeAgentEditorProjectSimpleEffectResolvedTarget(
			FString InCanonicalRef,
			FString InProfileId,
			FString InRevision,
			FConfigFile* InConfigDocument,
			FString InConfigSection,
			UObject* InSettingsObject,
			UStringTable* InStringTable,
			TSet<FString>* InLocalizationDirectories)
		: CanonicalRef(MoveTemp(InCanonicalRef))
		, ProfileId(MoveTemp(InProfileId))
		, Revision(MoveTemp(InRevision))
		, ConfigDocument(InConfigDocument)
		, ConfigSection(MoveTemp(InConfigSection))
		, SettingsObject(InSettingsObject)
		, StringTable(InStringTable)
		, LocalizationDirectories(InLocalizationDirectories)
	{
		const bool bConfigTarget =
			ProfileId == TEXT("config_section")
			|| ProfileId == TEXT("device_profile")
			|| ProfileId == TEXT("localization_target")
			|| ProfileId == TEXT("platform_config")
			|| ProfileId == TEXT("scalability_group");
		bValid = !CanonicalRef.IsEmpty()
			&& !ProfileId.IsEmpty()
			&& !Revision.IsEmpty()
			&& ((bConfigTarget
					&& ConfigDocument
					&& !ConfigSection.IsEmpty()
					&& (ProfileId
							!= TEXT("localization_target")
						|| LocalizationDirectories))
				|| (ProfileId == TEXT("project_settings")
					&& SettingsObject.IsValid())
				|| (ProfileId == TEXT("string_table")
					&& StringTable.IsValid()));
		if (!bValid)
		{
			FString Field;
			FString Reason;
			if (CanonicalRef.IsEmpty()) { Field = TEXT("target_ref_present"); Reason = TEXT("The resolved target has no canonical reference."); }
			else if (ProfileId.IsEmpty()) { Field = TEXT("target_kind_present"); Reason = TEXT("The resolved target has no target kind."); }
			else if (Revision.IsEmpty()) { Field = TEXT("revision_present"); Reason = TEXT("The resolved target has no captured revision."); }
			else if (bConfigTarget && !ConfigDocument) { Field = TEXT("config_document_available"); Reason = TEXT("The resolved configuration target has no configuration document."); }
			else if (bConfigTarget && ConfigSection.IsEmpty()) { Field = TEXT("config_section_present"); Reason = TEXT("The resolved configuration target has no section name."); }
			else if (bConfigTarget && ProfileId == TEXT("localization_target") && !LocalizationDirectories) { Field = TEXT("localization_directories_available"); Reason = TEXT("The resolved localization target has no culture directory state."); }
			else if (ProfileId == TEXT("project_settings")) { Field = TEXT("settings_object_available"); Reason = TEXT("The resolved settings target has no live settings object."); }
			else if (ProfileId == TEXT("string_table")) { Field = TEXT("string_table_available"); Reason = TEXT("The resolved string table target has no live string table."); }
			else { Field = TEXT("target_kind_supported"); Reason = TEXT("The resolved target kind is unsupported by editor project native execution."); }
			auto Error = NativeCondition(TEXT("editor_project_resolved_target_invalid"), Reason,
				TEXT("/resolved_target/") + Field);
			Error.ActualEvidence = MakeShared<FJsonValueString>(ProfileId);
			Error.SubjectId = CanonicalRef;
			ValidationDiagnostic = MakeShared<const FExecutionMutationError>(MoveTemp(Error));
		}
	}

	const FString&
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetCanonicalRef() const
	{
		return CanonicalRef;
	}

	const FString&
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetProfileId() const
	{
		return ProfileId;
	}

	const FString&
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetRevision() const
	{
		return Revision;
	}

	FConfigFile*
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetConfigDocument() const
	{
		return ConfigDocument;
	}

	const FString&
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetConfigSection() const
	{
		return ConfigSection;
	}

	UObject*
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetSettingsObject() const
	{
		return SettingsObject.Get();
	}

	UStringTable*
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetStringTable() const
	{
		return StringTable.Get();
	}

	TSet<FString>*
	FUeAgentEditorProjectSimpleEffectResolvedTarget::
		GetLocalizationDirectories() const
	{
		return LocalizationDirectories;
	}

	bool FUeAgentEditorProjectSimpleEffectResolvedTarget::IsValid() const
	{
		return bValid;
	}

	const FExecutionMutationError&
	FUeAgentEditorProjectSimpleEffectResolvedTarget::GetValidationDiagnostic() const
	{
		static const FExecutionMutationError EmptyDiagnostic;
		return ValidationDiagnostic.IsValid() ? *ValidationDiagnostic : EmptyDiagnostic;
	}

	bool FUeAgentEditorProjectSimpleEffectNativeHookFactory::TryCreate(
		TSharedRef<
			IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> TargetResolver,
		TSharedPtr<
			IUeAgentEditorProjectSimpleEffectNativeHook>& OutHook,
		FExecutionMutationError& OutError)
	{
		OutHook =
			MakeShared<FEditorProjectSimpleEffectNativeHook>(
				MoveTemp(TargetResolver));
		OutError = FExecutionMutationError();
		return OutHook.IsValid()
			|| Fail(OutError, NativeCondition(TEXT("editor_project_native_hook_create_failed"),
				TEXT("The editor project native hook could not be allocated."), TEXT("/native_hook_available")));
	}
}
