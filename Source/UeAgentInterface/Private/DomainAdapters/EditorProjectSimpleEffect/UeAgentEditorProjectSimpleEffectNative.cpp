#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "HAL/CriticalSection.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
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
			const FUeAgentCanonicalValue* Changes =
				FindObjectField(After, TEXT("typed_changes"));
			if (!Changes
				|| Changes->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_native_typed_changes_missing"));
			}
			for (const FUeAgentCanonicalValueRef& Unit
				: Changes->GetArrayItems())
			{
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(
						OutError,
						TEXT("editor_project_native_change_invalid"));
				}
				FTypedChange Change;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
					: Unit->GetObjectItems())
				{
					if (Item.Key == TEXT("field_id")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.FieldId = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("field_binding_id")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.FieldBindingId =
							Item.Value->GetText();
					}
					else if (Item.Key == TEXT("presence")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Change.Presence = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("value")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::Object)
					{
						for (const TPair<
							FString,
							FUeAgentCanonicalValueRef>& ValueItem
								: Item.Value->GetObjectItems())
						{
							if (ValueItem.Key == TEXT("type_id")
								&& ValueItem.Value->GetKind()
									== EUeAgentValueKind::String)
							{
								Change.TypeId =
									ValueItem.Value->GetText();
							}
							else if (ValueItem.Key
								== TEXT("canonical_value"))
							{
								Change.Value =
									FUeAgentCanonicalValuePtr(
										ValueItem.Value);
								Change.bHasValue = true;
							}
						}
					}
				}
				if (Change.FieldId.IsEmpty()
					|| Change.FieldBindingId.IsEmpty()
					|| Change.Presence.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("editor_project_native_change_invalid"));
				}
				OutChanges.Add(MoveTemp(Change));
			}
			return !OutChanges.IsEmpty()
				|| Fail(
					OutError,
					TEXT(
						"editor_project_native_typed_changes_missing"));
		}

		bool TryReadStringMap(
			const FUeAgentCanonicalValue& Value,
			TMap<FString, FString>& OutValues)
		{
			OutValues.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value.GetObjectItems())
			{
				if (Item.Value->GetKind()
					!= EUeAgentValueKind::String)
				{
					return false;
				}
				OutValues.Add(Item.Key, Item.Value->GetText());
			}
			return true;
		}

		bool TryReadStringArray(
			const FUeAgentCanonicalValue& Value,
			TArray<FString>& OutValues)
		{
			OutValues.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::String)
				{
					return false;
				}
				OutValues.Add(Item->GetText());
			}
			return true;
		}

		struct FSettingsChange
		{
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
			const FUeAgentCanonicalValue& Value,
			UObject& SettingsObject,
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
			TArray<FSettingsChange>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_settings_changes_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& Unit
				: Value.GetArrayItems())
			{
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_change_invalid"));
				}
				FSettingsChange Change;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
					: Unit->GetObjectItems())
				{
					if (Item.Value->GetKind()
						== EUeAgentValueKind::String)
					{
						if (Item.Key == TEXT("field_ref"))
						{
							Change.FieldRef = Item.Value->GetText();
						}
						else if (Item.Key
							== TEXT("expected_revision"))
						{
							Change.ExpectedRevision =
								Item.Value->GetText();
						}
						else if (Item.Key == TEXT("presence"))
						{
							Change.Presence =
								Item.Value->GetText();
						}
						else if (Item.Key == TEXT("type_id"))
						{
							Change.TypeId = Item.Value->GetText();
						}
					}
					if (Item.Key == TEXT("value"))
					{
						Change.Value =
							FUeAgentCanonicalValuePtr(Item.Value);
						Change.bHasValue = true;
					}
				}
				FString CurrentRevision;
				if (Change.FieldRef.IsEmpty()
					|| Change.ExpectedRevision.IsEmpty()
					|| Change.Presence.IsEmpty()
					|| Change.TypeId.IsEmpty()
					|| (Change.Presence == TEXT("set"))
						!= Change.bHasValue
					|| !Resolver.TryResolveSettingsField(
						SettingsObject,
						Change.FieldRef,
						Change.ExpectedRevision,
						Change.Property,
						CurrentRevision,
						OutError))
				{
					return OutError.IsPresent()
						? false
						: Fail(
							OutError,
							TEXT(
								"editor_project_settings_change_invalid"),
							Change.FieldRef);
				}
				if (!Change.Property
					|| CurrentRevision != Change.ExpectedRevision)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_field_revision_stale"),
						Change.FieldRef);
				}
				const void* Address =
					Change.Property->ContainerPtrToValuePtr<void>(
						&SettingsObject);
				Change.Property->ExportText_Direct(
					Change.BeforeText,
					Address,
					Address,
					&SettingsObject,
					PPF_None);
				OutChanges.Add(MoveTemp(Change));
			}
			return !OutChanges.IsEmpty()
				|| Fail(
					OutError,
					TEXT(
						"editor_project_settings_changes_invalid"));
		}

		struct FStringTableRow
		{
			FString Key;
			FString Source;
			TMap<FName, FString> Metadata;
		};

		bool TryReadStringTableRows(
			const FUeAgentCanonicalValue& Value,
			TArray<FStringTableRow>& OutRows)
		{
			OutRows.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			for (const FUeAgentCanonicalValueRef& Unit
				: Value.GetArrayItems())
			{
				if (Unit->GetKind() != EUeAgentValueKind::Object)
				{
					return false;
				}
				FStringTableRow Row;
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
					: Unit->GetObjectItems())
				{
					if (Item.Key == TEXT("key")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Row.Key = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("source")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::String)
					{
						Row.Source = Item.Value->GetText();
					}
					else if (Item.Key == TEXT("metadata")
						&& Item.Value->GetKind()
							== EUeAgentValueKind::Object)
					{
						for (const TPair<
							FString,
							FUeAgentCanonicalValueRef>& Metadata
								: Item.Value->GetObjectItems())
						{
							if (Metadata.Value->GetKind()
								!= EUeAgentValueKind::String)
							{
								return false;
							}
							Row.Metadata.Add(
								FName(*Metadata.Key),
								Metadata.Value->GetText());
						}
					}
				}
				if (Row.Key.IsEmpty())
				{
					return false;
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
			const FString& NativeBindingId,
			const FTypedChange& Change,
			FExecutionMutationError& OutError)
		{
			FString ExpectedBindingId;
			FString ExpectedTypeId;
			EUeAgentValueKind ExpectedKind =
				EUeAgentValueKind::Null;
			TArray<FString> AllowedPresences;
			FString BindingError;
			if (!TryResolveEditorProjectSimpleEffectFieldContract(
					NativeBindingId,
					Change.FieldId,
					ExpectedBindingId,
					ExpectedTypeId,
					ExpectedKind,
					AllowedPresences,
					BindingError)
				|| ExpectedBindingId != Change.FieldBindingId
				|| !AllowedPresences.Contains(Change.Presence)
				|| (Change.bHasValue
					&& (ExpectedTypeId != Change.TypeId
						|| !Change.Value.IsValid()
						|| Change.Value->GetKind() != ExpectedKind))
				|| (!Change.bHasValue
					&& !Change.TypeId.IsEmpty()))
			{
				return Fail(
					OutError,
					BindingError.IsEmpty()
						? TEXT(
							"editor_project_native_field_binding_mismatch")
						: MoveTemp(BindingError),
					Change.FieldId);
			}
			return true;
		}

		bool PrepareAction(
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
			const FPlannedChange& Change,
			FAction& OutAction,
			FExecutionMutationError& OutError)
		{
			OutAction = FAction();
			OutAction.CanonicalKey =
				Change.GetIdentity().GetCanonicalKey();
			FString NativeOperationType;
			FString NativeBindingId;
			FString NativeMechanicId;
			if (!TryReadString(
					Change.GetAfter(),
					TEXT("native_operation_type"),
					NativeOperationType)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("native_binding_id"),
					NativeBindingId)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("native_mechanic_id"),
					NativeMechanicId)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("profile_id"),
					OutAction.ProfileId)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("target_ref"),
					OutAction.TargetRef)
				|| !TryReadString(
					Change.GetAfter(),
					TEXT("target_revision"),
					OutAction.TargetRevision)
				|| !TryReadTypedChanges(
					Change.GetAfter(),
					OutAction.Changes,
					OutError))
			{
				return OutError.IsPresent()
					? false
					: Fail(
						OutError,
						TEXT(
							"editor_project_native_change_invalid"));
			}

			FString BoundOperationType;
			FString BoundProfileId;
			FString BoundMechanicId;
			FString BindingError;
			if (!TryResolveEditorProjectSimpleEffectNativeBinding(
					NativeBindingId,
					BoundOperationType,
					BoundProfileId,
					BoundMechanicId,
					BindingError)
				|| BoundOperationType != NativeOperationType
				|| BoundOperationType
					!= Change.GetIdentity().GetTargetKind()
				|| BoundProfileId != OutAction.ProfileId
				|| BoundMechanicId != NativeMechanicId)
			{
				return Fail(
					OutError,
					BindingError.IsEmpty()
						? TEXT(
							"editor_project_native_binding_mismatch")
						: MoveTemp(BindingError),
					NativeBindingId);
			}
			for (const FTypedChange& TypedChange
				: OutAction.Changes)
			{
				if (!ValidateFieldBinding(
						NativeBindingId,
						TypedChange,
						OutError))
				{
					return false;
				}
			}

			if (!Resolver.TryResolveTarget(
					OutAction.TargetRef,
					OutAction.Target,
					OutError)
				|| !OutAction.Target.IsValid())
			{
				return false;
			}
			if (OutAction.Target.GetRevision()
				!= OutAction.TargetRevision)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_target_revision_stale"),
					OutAction.TargetRef);
			}
			if (OutAction.Target.GetProfileId()
				!= OutAction.ProfileId)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_target_profile_mismatch"),
					OutAction.TargetRef);
			}

			if (FConfigFile* Config =
				OutAction.Target.GetConfigDocument())
			{
				OutAction.BeforeConfig =
					MakeUnique<FConfigFile>(*Config);
			}
			if (TSet<FString>* Directories =
				OutAction.Target.GetLocalizationDirectories())
			{
				OutAction.BeforeLocalizationDirectories =
					*Directories;
			}
			if (UObject* Settings =
				OutAction.Target.GetSettingsObject())
			{
				OutAction.bPackageWasDirty =
					Settings->GetOutermost()->IsDirty();
			}
			if (UStringTable* StringTable =
				OutAction.Target.GetStringTable())
			{
				OutAction.bPackageWasDirty =
					StringTable->GetOutermost()->IsDirty();
				CaptureStringTable(
					*StringTable->GetStringTable(),
					OutAction.BeforeStringTable);
			}

			for (const FTypedChange& TypedChange
				: OutAction.Changes)
			{
				if (OutAction.ProfileId == TEXT("project_settings")
					&& TypedChange.FieldId == TEXT("properties"))
				{
					if (!TypedChange.Value.IsValid()
						|| !TryReadSettingsChanges(
							*TypedChange.Value,
							*OutAction.Target.GetSettingsObject(),
							Resolver,
							OutAction.SettingsChanges,
							OutError))
					{
						return false;
					}
				}
				if (OutAction.ProfileId == TEXT("device_profile")
					&& TypedChange.FieldId
						== TEXT("base_profile_ref")
					&& TypedChange.Presence == TEXT("set"))
				{
					FUeAgentEditorProjectSimpleEffectResolvedTarget
						BaseTarget;
					if (!TypedChange.Value.IsValid()
						|| TypedChange.Value->GetKind()
							!= EUeAgentValueKind::String
						|| TypedChange.Value->GetText()
							== OutAction.TargetRef
						|| !Resolver.TryResolveTarget(
							TypedChange.Value->GetText(),
							BaseTarget,
							OutError)
						|| !BaseTarget.IsValid()
						|| BaseTarget.GetProfileId()
							!= TEXT("device_profile"))
					{
						return OutError.IsPresent()
							? false
							: Fail(
								OutError,
								TEXT(
									"editor_project_device_profile_base_unresolved"));
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
			UObject& Object,
			const FSettingsChange& Change,
			FExecutionMutationError& OutError)
		{
			if (!Change.Property)
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_settings_field_unresolved"),
					Change.FieldRef);
			}
			void* Address =
				Change.Property->ContainerPtrToValuePtr<void>(&Object);
			Object.PreEditChange(Change.Property);
			bool bApplied = true;
			if (Change.Presence != TEXT("set"))
			{
				Change.Property->ClearValue(Address);
			}
			else if (const FBoolProperty* BoolProperty =
				CastField<FBoolProperty>(Change.Property))
			{
				bApplied = Change.Value.IsValid()
					&& Change.Value->GetKind()
						== EUeAgentValueKind::Boolean;
				if (bApplied)
				{
					BoolProperty->SetPropertyValue(
						Address,
						Change.Value->GetBoolean());
				}
			}
			else if (const FStrProperty* StringProperty =
				CastField<FStrProperty>(Change.Property))
			{
				bApplied = Change.Value.IsValid()
					&& Change.Value->GetKind()
						== EUeAgentValueKind::String;
				if (bApplied)
				{
					StringProperty->SetPropertyValue(
						Address,
						Change.Value->GetText());
				}
			}
			else if (const FIntProperty* IntProperty =
				CastField<FIntProperty>(Change.Property))
			{
				int32 Parsed = 0;
				bApplied = Change.Value.IsValid()
					&& Change.Value->GetKind()
						== EUeAgentValueKind::Integer
					&& LexTryParseString(
						Parsed,
						*Change.Value->GetLexeme());
				if (bApplied)
				{
					IntProperty->SetPropertyValue(Address, Parsed);
				}
			}
			else if (const FInt64Property* Int64Property =
				CastField<FInt64Property>(Change.Property))
			{
				int64 Parsed = 0;
				bApplied = Change.Value.IsValid()
					&& Change.Value->GetKind()
						== EUeAgentValueKind::Integer
					&& LexTryParseString(
						Parsed,
						*Change.Value->GetLexeme());
				if (bApplied)
				{
					Int64Property->SetPropertyValue(
						Address,
						Parsed);
				}
			}
			else
			{
				bApplied = false;
			}
			FPropertyChangedEvent ChangedEvent(
				Change.Property,
				EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(ChangedEvent);
			return bApplied
				|| Fail(
					OutError,
					TEXT(
						"editor_project_settings_property_type_unsupported"),
					Change.FieldRef);
		}

		bool RestorePropertyValue(
			UObject& Object,
			const FSettingsChange& Change)
		{
			if (!Change.Property)
			{
				return false;
			}
			void* Address =
				Change.Property->ContainerPtrToValuePtr<void>(&Object);
			Object.PreEditChange(Change.Property);
			const TCHAR* End =
				Change.Property->ImportText_Direct(
					*Change.BeforeText,
					Address,
					&Object,
					PPF_None);
			FPropertyChangedEvent ChangedEvent(
				Change.Property,
				EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(ChangedEvent);
			return End != nullptr;
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
				return Fail(
					OutError,
					TEXT(
						"editor_project_config_target_invalid"),
					Action.TargetRef);
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
				if (Change.Presence != TEXT("clear")
					&& (!Change.Value.IsValid()
						|| !TryReadStringMap(
							*Change.Value,
							Values)))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_config_entry_invalid"));
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
				if (Change.Presence != TEXT("clear")
					&& (!Change.Value.IsValid()
						|| !TryReadStringMap(
							*Change.Value,
							Values)))
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_config_entry_invalid"));
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
					if (Change.Presence != TEXT("clear")
						&& (!Change.Value.IsValid()
							|| !TryReadStringArray(
								*Change.Value,
								Requested)))
					{
						return Fail(
							OutError,
							TEXT(
								"editor_project_culture_set_invalid"));
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
			return Fail(
				OutError,
				TEXT(
					"editor_project_native_change_unsupported"),
				Change.FieldId);
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
				return Fail(
					OutError,
					TEXT(
						"editor_project_string_table_target_invalid"));
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
				return Fail(
					OutError,
					TEXT(
						"editor_project_native_change_unsupported"),
					Change.FieldId);
			}
			TArray<FStringTableRow> Rows;
			if (Change.Presence != TEXT("clear")
				&& (!Change.Value.IsValid()
					|| !TryReadStringTableRows(
						*Change.Value,
						Rows)))
			{
				return Fail(
					OutError,
					TEXT(
						"editor_project_string_table_entries_invalid"));
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

		bool RollbackAction(FAction& Action)
		{
			bool bConfirmed = true;
			if (Action.BeforeConfig.IsValid()
				&& Action.Target.GetConfigDocument())
			{
				*Action.Target.GetConfigDocument() =
					*Action.BeforeConfig;
			}
			if (TSet<FString>* Directories =
				Action.Target.GetLocalizationDirectories())
			{
				*Directories =
					Action.BeforeLocalizationDirectories;
			}
			if (UObject* Settings =
				Action.Target.GetSettingsObject())
			{
				for (int32 Index =
						Action.SettingsChanges.Num() - 1;
					Index >= 0;
					--Index)
				{
					bConfirmed =
						RestorePropertyValue(
							*Settings,
							Action.SettingsChanges[Index])
						&& bConfirmed;
				}
				Settings->GetOutermost()->SetDirtyFlag(
					Action.bPackageWasDirty);
			}
			if (UStringTable* StringTable =
				Action.Target.GetStringTable())
			{
				RestoreStringTable(
					*StringTable->GetMutableStringTable(),
					Action.BeforeStringTable);
				StringTable->GetOutermost()->SetDirtyFlag(
					Action.bPackageWasDirty);
			}
			Action.bApplied = false;
			return bConfirmed;
		}

		bool ApplyAction(
			FAction& Action,
			FExecutionMutationError& OutError)
		{
			if (Action.ProfileId == TEXT("project_settings"))
			{
				UObject* Settings =
					Action.Target.GetSettingsObject();
				if (!Settings)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_settings_target_invalid"));
				}
				for (const FSettingsChange& Change
					: Action.SettingsChanges)
				{
					if (!ApplyPropertyValue(
							*Settings,
							Change,
							OutError))
					{
						RollbackAction(Action);
						return false;
					}
				}
				Settings->GetOutermost()->MarkPackageDirty();
				Action.bApplied = true;
				return true;
			}
			for (const FTypedChange& Change : Action.Changes)
			{
				const bool bChanged =
					Action.ProfileId == TEXT("string_table")
						? ApplyStringTableChange(
							Action,
							Change,
							OutError)
						: ApplyConfigChange(
							Action,
							Change,
							OutError);
				if (!bChanged)
				{
					RollbackAction(Action);
					return false;
				}
			}
			if (UStringTable* StringTable =
				Action.Target.GetStringTable())
			{
				StringTable->GetOutermost()->MarkPackageDirty();
			}
			Action.bApplied = true;
			return true;
		}

		bool RollbackAll(TArray<FAction>& Actions)
		{
			bool bConfirmed = true;
			for (int32 Index = Actions.Num() - 1;
				Index >= 0;
				--Index)
			{
				bConfirmed =
					RollbackAction(Actions[Index])
					&& bConfirmed;
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
				OutResult = FUeAgentGenericEffectApplyResult();
				OutError = FExecutionMutationError();
				if (!IsInGameThread()
					|| !Request.IsValid()
					|| !Lease.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_native_request_invalid"));
				}
				const TArray<FPlannedChange>& Changes =
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges();
				if (Changes.IsEmpty()
					|| Bindings.Num() != Changes.Num())
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_change_binding_mismatch"));
				}
				for (const FUeAgentGenericEffectBinding& Binding
					: Bindings)
				{
					if (!Binding.IsValid())
					{
						return Fail(
							OutError,
							TEXT(
								"editor_project_change_binding_mismatch"));
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
					if (!ApplyAction(Action, OutError))
					{
						RollbackAll(AppliedActions);
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
					RollbackByToken.Add(
						Token,
						MoveTemp(AppliedActions));
					TokenByOperation.Add(
						Request.GetOperationId(),
						Token);
				}
				OutResult = FUeAgentGenericEffectApplyResult(
					Lease.GetFencingToken(),
					MoveTemp(MutatedKeys),
					{TEXT("editor_project.authoring.memory")},
					true,
					Token,
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"editor_project_apply_result_invalid"));
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
				if (!IsInGameThread()
					|| !Request.IsValid()
					|| !Applied.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_rollback_request_invalid"));
				}
				FScopeLock Lock(&StateMutex);
				TArray<FAction>* Actions =
					RollbackByToken.Find(
						Applied.GetRollbackToken());
				if (!Actions)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_project_rollback_token_unknown"));
				}
				bOutConfirmed = RollbackAll(*Actions);
				if (bOutConfirmed)
				{
					RollbackByToken.Remove(
						Applied.GetRollbackToken());
					TokenByOperation.Remove(
						Request.GetOperationId());
				}
				return bOutConfirmed
					|| Fail(
						OutError,
						TEXT(
							"editor_project_rollback_unconfirmed"));
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
					return Fail(
						OutError,
						TEXT(
							"editor_project_barrier_request_invalid"));
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("editor-project:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return OutResult.IsValid()
					|| Fail(
						OutError,
						TEXT(
							"editor_project_barrier_result_invalid"));
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
			|| Fail(
				OutError,
				TEXT(
					"editor_project_native_hook_create_failed"));
	}
}
