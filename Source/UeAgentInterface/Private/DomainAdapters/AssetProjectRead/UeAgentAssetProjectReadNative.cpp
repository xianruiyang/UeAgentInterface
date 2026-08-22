#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"

#include "AssetRegistry/AssetData.h"
#include "Commands/OriginResolve/UeAgentOriginResolveTypes.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentAssetProjectReadError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool ParseInput(
			const FUeAgentContractValue& Input,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentAssetProjectReadError& OutError)
		{
			OutObject.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Input.GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_input_invalid"),
					FString(),
					TEXT("canonical input is not a JSON object"));
			}
			return true;
		}

		FUeAgentTemporalPoint ObservedAt(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentTemporalPoint(
				TEXT("ue.operation_context:")
					+ Context.GetExecutionContext()
						.GetContextRevision(),
				TEXT("1"));
		}

		FUeAgentStateRevision Revision(
			const FUeAgentOperationContext& Context)
		{
			return FUeAgentStateRevision(
				TEXT("asset-project-read"),
				Context.GetExecutionContext().GetContextRevision(),
				1);
		}

		FUeAgentStateDetails Details(const FString& DisplayName)
		{
			return FUeAgentStateDetails(
				DisplayName,
				TEXT(
					"Fact captured from an explicit "
					"AssetProjectRead environment owner"),
				{TEXT("AssetProjectRead")},
				false,
				TEXT("available"),
				{});
		}

		bool AddValueEntry(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& TypeId,
			EUeAgentValueKind Kind,
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_value_invalid"),
					OwnerId,
					Selector);
			}
			const FUeAgentStateRevision StateRevision =
				Revision(Context);
			const FUeAgentStateEntry Entry(
				FUeAgentCanonicalIdentity(
					SemanticId,
					TargetKind,
					OwnerId,
					Selector,
					TEXT("state"),
					StateRevision.GetGeneration()),
				FUeAgentCanonicalType(
					TypeId,
					Kind,
					false,
					Kind == EUeAgentValueKind::Object
						? TEXT("object")
						: Kind == EUeAgentValueKind::Array
							? TEXT("array")
							: TEXT("scalar")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("ue-native"),
					TEXT("asset-project-read"),
					Value.ToSharedRef()),
				Details(Selector),
				StateRevision);
			if (!Entry.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_entry_invalid"),
					OwnerId,
					Selector);
			}
			OutEntries.Add(Entry);
			return true;
		}

		bool AddString(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const FString& Text,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					Text,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.string"),
				EUeAgentValueKind::String,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddBoolean(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			bool bValue,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.boolean"),
				EUeAgentValueKind::Boolean,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddInteger(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			int64 Number,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryInteger(
					LexToString(Number),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.integer"),
				EUeAgentValueKind::Integer,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddDecimal(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Selector,
			const double Number,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Number, 0),
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TargetKind,
				OwnerId,
				Selector,
				TEXT("core.decimal"),
				EUeAgentValueKind::Decimal,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddStringArray(
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& Selector,
			const TArray<FString>& Strings,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FString& String : Strings)
			{
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError StateError;
				if (!FUeAgentCanonicalValue::TryString(
						String,
						Context.GetResourceTracker(),
						Item,
						StateError))
				{
					return Fail(
						OutError,
						StateError.Code,
						StateError.Path,
						StateError.Detail);
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return AddValueEntry(
				SemanticId,
				TEXT("reference_origin"),
				OwnerId,
				Selector,
				TEXT("core.string_array"),
				EUeAgentValueKind::Array,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool AddCodePaths(
			const FString& OwnerId,
			const TArray<UeAgentOriginResolve::FCodePath>& CodePaths,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			FUeAgentAssetProjectReadError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const UeAgentOriginResolve::FCodePath& CodePath : CodePaths)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				auto AddStringField = [&](const TCHAR* Name, const FString& Text)
				{
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryString(
							Text,
							Context.GetResourceTracker(),
							Value,
							StateError))
					{
						return Fail(OutError, StateError.Code, StateError.Path, StateError.Detail);
					}
					Fields.Add({Name, Value.ToSharedRef()});
					return true;
				};
				auto AddIntegerField = [&](const TCHAR* Name, int32 Number)
				{
					FUeAgentCanonicalValuePtr Value;
					FUeAgentStateSemanticError StateError;
					if (!FUeAgentCanonicalValue::TryInteger(
							LexToString(Number),
							Context.GetResourceTracker(),
							Value,
							StateError))
					{
						return Fail(OutError, StateError.Code, StateError.Path, StateError.Detail);
					}
					Fields.Add({Name, Value.ToSharedRef()});
					return true;
				};
				FUeAgentCanonicalValuePtr Item;
				FUeAgentStateSemanticError StateError;
				if (!AddStringField(TEXT("path"), CodePath.Path)
					|| !AddIntegerField(TEXT("line"), CodePath.Line)
					|| !AddIntegerField(TEXT("column"), CodePath.Column)
					|| !AddStringField(TEXT("symbol"), CodePath.Symbol)
					|| !AddStringField(TEXT("module"), CodePath.Module)
					|| !AddStringField(TEXT("status"), CodePath.Status)
					|| !FUeAgentCanonicalValue::TryObject(
						Fields,
						Context.GetResourceTracker(),
						Item,
						StateError))
				{
					if (OutError.Code.IsEmpty())
					{
						return Fail(OutError, StateError.Code, StateError.Path, StateError.Detail);
					}
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Value;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryArray(
					Items,
					Context.GetResourceTracker(),
					Value,
					StateError))
			{
				return Fail(OutError, StateError.Code, StateError.Path, StateError.Detail);
			}
			return AddValueEntry(
				TEXT("reference.origin:code_paths"),
				TEXT("reference_origin"),
				OwnerId,
				TEXT("origin.code_paths"),
				TEXT("reference.origin.code_path_array"),
				EUeAgentValueKind::Array,
				Value,
				Context,
				OutEntries,
				OutError);
		}

		bool BuildSnapshot(
			const FString& ProviderBinding,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry> Entries,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentAssetProjectReadError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					FString::Printf(
						TEXT("%s:%s"),
						*ProviderBinding,
						*Context.GetRequestId()),
					Context.GetExecutionContext().GetStatePlane(),
					Revision(Context),
					ObservedAt(Context),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						ProviderBinding,
						Context.GetExecutionContext()
							.GetProjectInstanceId()),
					Entries,
					Context.GetResourceTracker(),
					OutSnapshot,
					StateError))
			{
				return Fail(
					OutError,
					StateError.Code,
					StateError.Path,
					StateError.Detail);
			}
			return true;
		}

		TArray<FString> ReadStringArray(
			const TSharedPtr<FJsonObject>& Input,
			const TCHAR* Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Input.IsValid()
				&& Input->TryGetArrayField(Field, Values)
				&& Values)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					if (Value.IsValid()
						&& Value->Type == EJson::String)
					{
						Result.Add(Value->AsString());
					}
				}
			}
			return Result;
		}

		bool AddObjectIdentity(
			UObject* Object,
			const FString& SemanticPrefix,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const FString OwnerId = Object->GetPathName();
			return AddString(
					SemanticPrefix + TEXT(":path"),
					TEXT("asset"),
					OwnerId,
					TEXT("path"),
					OwnerId,
					Context,
					Entries,
					OutError)
				&& AddString(
					SemanticPrefix + TEXT(":class"),
					TEXT("asset"),
					OwnerId,
					TEXT("class_path"),
					Object->GetClass()->GetPathName(),
					Context,
					Entries,
					OutError)
				&& AddString(
					SemanticPrefix + TEXT(":package"),
					TEXT("asset"),
					OwnerId,
					TEXT("package_name"),
					Object->GetOutermost()->GetName(),
					Context,
					Entries,
					OutError)
				&& AddBoolean(
					SemanticPrefix + TEXT(":dirty"),
					TEXT("asset"),
					OwnerId,
					TEXT("package_dirty"),
					Object->GetOutermost()->IsDirty(),
					Context,
					Entries,
					OutError);
		}

		bool AddRequestedProperties(
			UObject* Object,
			const TArray<FString>& FieldIds,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			if (FieldIds.IsEmpty())
			{
				int64 PropertyCount = 0;
				for (TFieldIterator<FProperty> It(Object->GetClass());
					It;
					++It)
				{
					++PropertyCount;
				}
				return AddInteger(
					TEXT("asset.properties:count"),
					TEXT("asset"),
					Object->GetPathName(),
					TEXT("property_count"),
					PropertyCount,
					Context,
					Entries,
					OutError);
			}

			for (const FString& FieldId : FieldIds)
			{
				const FProperty* Property =
					FindFProperty<FProperty>(
						Object->GetClass(),
						*FieldId);
				if (!Property)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_project_read_property_not_found"),
						Object->GetPathName(),
						FieldId);
				}
				FString Text;
				Property->ExportText_InContainer(
					0,
					Text,
					Object,
					Object,
					Object,
					PPF_None);
				if (!AddString(
						TEXT("asset.properties:value:")
							+ FieldId,
						TEXT("asset"),
						Object->GetPathName(),
						FieldId,
						Text,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool MatchesCollectionKey(
			const FString& Key,
			const TArray<FString>& RequestedKeys,
			const FString& KeyPrefix)
		{
			return (RequestedKeys.IsEmpty() || RequestedKeys.Contains(Key))
				&& (KeyPrefix.IsEmpty() || Key.StartsWith(KeyPrefix));
		}

		bool AddCollectionCount(
			UObject* Asset,
			const FString& CollectionKind,
			const int64 Count,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			return AddString(
				TEXT("asset.collection:kind"),
				TEXT("asset"),
				Asset->GetPathName(),
				TEXT("collection_kind"),
				CollectionKind,
				Context,
				Entries,
				OutError)
				&& AddInteger(
					TEXT("asset.collection:count"),
					TEXT("asset"),
					Asset->GetPathName(),
					TEXT("collection_count"),
					Count,
					Context,
					Entries,
					OutError);
		}

		bool TryAddDataTableRows(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const UDataTable* Table = Cast<UDataTable>(Asset);
			if (!Table || !Table->GetRowStruct())
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_collection_kind_unsupported"),
					Asset->GetPathName(),
					TEXT("rows requires a DataTable with a row struct"));
			}
			const TArray<FString> RequestedKeys =
				ReadStringArray(Input, TEXT("keys"));
			const TArray<FString> FieldIds =
				ReadStringArray(Input, TEXT("field_ids"));
			FString KeyPrefix;
			Input->TryGetStringField(TEXT("key_prefix"), KeyPrefix);
			TSet<FString> MatchedKeys;
			int64 Count = 0;
			TArray<FName> RowNames = Table->GetRowNames();
			RowNames.Sort(FNameLexicalLess());
			for (const FName RowName : RowNames)
			{
				const FString Key = RowName.ToString();
				if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
				{
					continue;
				}
				uint8* const* RowData = Table->GetRowMap().Find(RowName);
				if (!RowData || !*RowData)
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_collection_row_unavailable"),
						Asset->GetPathName(),
						Key);
				}
				MatchedKeys.Add(Key);
				++Count;
				const FString RowOwner = Asset->GetPathName()
					+ TEXT("#row:") + Key;
				if (!AddString(
						TEXT("asset.collection:row:") + Key,
						TEXT("data_table_row"),
						RowOwner,
						TEXT("row_key"),
						Key,
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				for (const FString& FieldId : FieldIds)
				{
					FProperty* Property = FindFProperty<FProperty>(
						Table->GetRowStruct(), *FieldId);
					if (!Property)
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_collection_field_not_found"),
							RowOwner,
							FieldId);
					}
					FString ValueText;
					const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(
						*RowData);
					Property->ExportTextItem_Direct(
						ValueText, ValuePtr, nullptr, Asset, PPF_None);
					const FString FieldOwner = RowOwner
						+ TEXT("#field:") + FieldId;
					if (!AddString(
							TEXT("asset.collection:row_field:")
								+ Key + TEXT(":") + FieldId,
							TEXT("data_table_field"),
							FieldOwner,
							TEXT("field_id"),
							FieldId,
							Context,
							Entries,
							OutError)
						|| !AddString(
							TEXT("asset.collection:row_value:")
								+ Key + TEXT(":") + FieldId,
							TEXT("data_table_field"),
							FieldOwner,
							TEXT("field_value_text"),
							ValueText,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
			}
			for (const FString& RequestedKey : RequestedKeys)
			{
				if (!MatchedKeys.Contains(RequestedKey))
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_collection_key_not_found"),
						Asset->GetPathName(),
						RequestedKey);
				}
			}
			return AddCollectionCount(
				Asset, TEXT("rows"), Count, Context, Entries, OutError);
		}

		bool TryAddReflectedCollection(
			UObject* Asset,
			const FString& CollectionKind,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			const TArray<FString> RequestedKeys =
				ReadStringArray(Input, TEXT("keys"));
			const TArray<FString> FieldIds =
				ReadStringArray(Input, TEXT("field_ids"));
			FString KeyPrefix;
			Input->TryGetStringField(TEXT("key_prefix"), KeyPrefix);
			TArray<FProperty*> Properties;
			if (FieldIds.IsEmpty())
			{
				for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
				{
					FProperty* Property = *It;
					if ((CollectionKind == TEXT("items")
							&& (Property->IsA<FArrayProperty>()
								|| Property->IsA<FSetProperty>()))
						|| (CollectionKind == TEXT("entries")
							&& Property->IsA<FMapProperty>()))
					{
						Properties.Add(Property);
					}
				}
				Properties.Sort(
					[](const FProperty& Left, const FProperty& Right)
					{
						return Left.GetName() < Right.GetName();
					});
			}
			else
			{
				for (const FString& FieldId : FieldIds)
				{
					FProperty* Property = FindFProperty<FProperty>(
						Asset->GetClass(), *FieldId);
					const bool bCompatible = Property
						&& ((CollectionKind == TEXT("items")
								&& (Property->IsA<FArrayProperty>()
									|| Property->IsA<FSetProperty>()))
							|| (CollectionKind == TEXT("entries")
								&& Property->IsA<FMapProperty>()));
					if (!bCompatible)
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_collection_field_kind_mismatch"),
							Asset->GetPathName(),
							FieldId);
					}
					Properties.Add(Property);
				}
			}

			int64 Count = 0;
			for (FProperty* Property : Properties)
			{
				void* Container = Property->ContainerPtrToValuePtr<void>(Asset);
				if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
				{
					FScriptArrayHelper Helper(Array, Container);
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						const FString Key = LexToString(Index);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						FString ValueText;
						Array->Inner->ExportTextItem_Direct(
							ValueText, Helper.GetRawPtr(Index), nullptr,
							Asset, PPF_None);
						const FString Owner = Asset->GetPathName()
							+ TEXT("#") + Property->GetName()
							+ TEXT("[") + Key + TEXT("]");
						if (!AddString(
								TEXT("asset.collection:item_field:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("collection_field_id"), Property->GetName(),
								Context, Entries, OutError)
							|| !AddInteger(
								TEXT("asset.collection:item_index:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("item_index"), Index,
								Context, Entries, OutError)
							|| !AddString(
								TEXT("asset.collection:item_value:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("item_value_text"), ValueText,
								Context, Entries, OutError))
						{
							return false;
						}
						++Count;
					}
				}
				else if (FSetProperty* Set = CastField<FSetProperty>(Property))
				{
					FScriptSetHelper Helper(Set, Container);
					int32 ItemIndex = 0;
					for (int32 InternalIndex = 0;
						InternalIndex < Helper.GetMaxIndex(); ++InternalIndex)
					{
						if (!Helper.IsValidIndex(InternalIndex))
						{
							continue;
						}
						FString ValueText;
						Set->ElementProp->ExportTextItem_Direct(
							ValueText, Helper.GetElementPtr(InternalIndex),
							nullptr, Asset, PPF_None);
						const FString Key = LexToString(ItemIndex++);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						const FString Owner = Asset->GetPathName()
							+ TEXT("#") + Property->GetName()
							+ TEXT("{") + Key + TEXT("}");
						if (!AddString(
								TEXT("asset.collection:set_field:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("collection_field_id"), Property->GetName(),
								Context, Entries, OutError)
							|| !AddInteger(
								TEXT("asset.collection:set_index:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("item_index"), FCString::Atoi(*Key),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("asset.collection:set_value:") + Owner,
								TEXT("collection_item"), Owner,
								TEXT("item_value_text"), ValueText,
								Context, Entries, OutError))
						{
							return false;
						}
						++Count;
					}
				}
				else if (FMapProperty* Map = CastField<FMapProperty>(Property))
				{
					FScriptMapHelper Helper(Map, Container);
					for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
					{
						if (!Helper.IsValidIndex(Index))
						{
							continue;
						}
						FString Key;
						Map->KeyProp->ExportTextItem_Direct(
							Key, Helper.GetKeyPtr(Index), nullptr,
							Asset, PPF_None);
						if (!MatchesCollectionKey(Key, RequestedKeys, KeyPrefix))
						{
							continue;
						}
						FString ValueText;
						Map->ValueProp->ExportTextItem_Direct(
							ValueText, Helper.GetValuePtr(Index), nullptr,
							Asset, PPF_None);
						const FString Owner = Asset->GetPathName()
							+ TEXT("#") + Property->GetName()
							+ TEXT("{") + Key + TEXT("}");
						if (!AddString(
								TEXT("asset.collection:entry_field:") + Owner,
								TEXT("collection_entry"), Owner,
								TEXT("collection_field_id"), Property->GetName(),
								Context, Entries, OutError)
							|| !AddString(
								TEXT("asset.collection:entry_key:") + Owner,
								TEXT("collection_entry"), Owner,
								TEXT("entry_key"), Key,
								Context, Entries, OutError)
							|| !AddString(
								TEXT("asset.collection:entry_value:") + Owner,
								TEXT("collection_entry"), Owner,
								TEXT("entry_value_text"), ValueText,
								Context, Entries, OutError))
						{
							return false;
						}
						++Count;
					}
				}
			}
			return AddCollectionCount(
				Asset, CollectionKind, Count, Context, Entries, OutError);
		}

		bool TryAddAssetSample(
			UObject* Asset,
			const TSharedPtr<FJsonObject>& Input,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& Entries,
			FUeAgentAssetProjectReadError& OutError)
		{
			FString SampleKind;
			if (!Input->TryGetStringField(TEXT("sample_kind"), SampleKind))
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_sample_kind_missing"),
					Asset->GetPathName());
			}
			if (SampleKind == TEXT("bounds"))
			{
				if (Input->HasField(TEXT("coordinates"))
					|| Input->HasField(TEXT("index"))
					|| Input->HasField(TEXT("channel")))
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_sample_selector_unsupported"),
						Asset->GetPathName(),
						SampleKind);
				}
				FBoxSphereBounds Bounds;
				if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
				{
					Bounds = StaticMesh->GetBounds();
				}
				else if (const USkeletalMesh* SkeletalMesh =
					Cast<USkeletalMesh>(Asset))
				{
					Bounds = SkeletalMesh->GetBounds();
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_sample_target_unsupported"),
						Asset->GetPathName(),
						SampleKind);
				}
				const FString Owner = Asset->GetPathName();
				return AddString(
						TEXT("asset.sample:kind"), TEXT("asset"), Owner,
						TEXT("sample_kind"), SampleKind,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_origin_x"), TEXT("asset"), Owner,
						TEXT("bounds_origin_x_cm"), Bounds.Origin.X,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_origin_y"), TEXT("asset"), Owner,
						TEXT("bounds_origin_y_cm"), Bounds.Origin.Y,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_origin_z"), TEXT("asset"), Owner,
						TEXT("bounds_origin_z_cm"), Bounds.Origin.Z,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_extent_x"), TEXT("asset"), Owner,
						TEXT("bounds_extent_x_cm"), Bounds.BoxExtent.X,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_extent_y"), TEXT("asset"), Owner,
						TEXT("bounds_extent_y_cm"), Bounds.BoxExtent.Y,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_extent_z"), TEXT("asset"), Owner,
						TEXT("bounds_extent_z_cm"), Bounds.BoxExtent.Z,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:bounds_radius"), TEXT("asset"), Owner,
						TEXT("bounds_sphere_radius_cm"), Bounds.SphereRadius,
						Context, Entries, OutError);
			}
			if (SampleKind == TEXT("vertex"))
			{
				if (!Input->HasField(TEXT("index"))
					|| Input->HasField(TEXT("coordinates"))
					|| Input->HasField(TEXT("channel")))
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_sample_selector_unsupported"),
						Asset->GetPathName(),
						SampleKind);
				}
				const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
				const FMeshDescription* Description = StaticMesh
					? StaticMesh->GetMeshDescription(0) : nullptr;
				if (!Description)
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_sample_target_unsupported"),
						Asset->GetPathName(),
						SampleKind);
				}
				const int32 Index = static_cast<int32>(
					Input->GetIntegerField(TEXT("index")));
				const FVertexID VertexId(Index);
				if (!Description->IsVertexValid(VertexId))
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_sample_index_out_of_range"),
						Asset->GetPathName(),
						LexToString(Index));
				}
				const FStaticMeshConstAttributes Attributes(*Description);
				const TVertexAttributesConstRef<FVector3f> Positions =
					Attributes.GetVertexPositions();
				const FVector3f Position = Positions[VertexId];
				const FString Owner = Asset->GetPathName();
				return AddString(
						TEXT("asset.sample:kind"), TEXT("asset"), Owner,
						TEXT("sample_kind"), SampleKind,
						Context, Entries, OutError)
					&& AddInteger(
						TEXT("asset.sample:vertex_index"), TEXT("asset"), Owner,
						TEXT("vertex_index"), Index,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:vertex_x"), TEXT("asset"), Owner,
						TEXT("vertex_x_cm"), Position.X,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:vertex_y"), TEXT("asset"), Owner,
						TEXT("vertex_y_cm"), Position.Y,
						Context, Entries, OutError)
					&& AddDecimal(
						TEXT("asset.sample:vertex_z"), TEXT("asset"), Owner,
						TEXT("vertex_z_cm"), Position.Z,
						Context, Entries, OutError);
			}
			return Fail(
				OutError,
				TEXT("asset_project_read_sample_kind_unsupported"),
				Asset->GetPathName(),
				SampleKind);
		}

		class FHookBase : public IUeAgentAssetProjectReadHook
		{
		public:
			FHookBase(
				FString InProviderBinding,
				TSharedRef<IUeAgentAssetProjectReadEnvironment>
					InEnvironment)
				: ProviderBinding(MoveTemp(InProviderBinding))
				, Environment(MoveTemp(InEnvironment))
			{
			}

		protected:
			bool ResolveAsset(
				const TOptional<FUeAgentCandidateBinding>& Target,
				UObject*& OutAsset,
				FUeAgentAssetProjectReadError& OutError) const
			{
				OutAsset = nullptr;
				if (!Target.IsSet())
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_target_required"),
						ProviderBinding);
				}
				if (!Environment->TryResolveAsset(
						Target.GetValue(),
						OutAsset,
						OutError))
				{
					return false;
				}
				return OutAsset
					? true
					: Fail(
						OutError,
						TEXT("asset_project_read_asset_unresolved"),
						Target.GetValue().GetBindingId());
			}

			const FString ProviderBinding;
			const TSharedRef<IUeAgentAssetProjectReadEnvironment>
				Environment;
		};

		class FReferenceOriginHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (Target.IsSet())
				{
					return Fail(
						OutError,
						TEXT("reference_origin_public_target_forbidden"),
						ProviderBinding,
						TEXT("reference.origin.resolve accepts only input.target"));
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				const TSharedPtr<FJsonObject>* TargetObject = nullptr;
				FString ExplicitSource;
				if (!Input->TryGetObjectField(TEXT("target"), TargetObject)
					|| !TargetObject
					|| !TargetObject->IsValid()
					|| !(*TargetObject)->TryGetStringField(TEXT("source"), ExplicitSource)
					|| ExplicitSource.TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("reference_origin_target_invalid"),
						ProviderBinding,
						TEXT("input.target.source is required"));
				}
				UeAgentOriginResolve::FResolveParams Params;
				FString ResolveError;
				if (!UeAgentOriginResolve::ParseResolveParams(
						Input,
						Params,
						ResolveError)
					|| Params.TargetSource.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("reference_origin_target_invalid"),
						Params.TargetSource,
						ResolveError.IsEmpty()
							? FString(TEXT("target.source is required"))
							: ResolveError);
				}
				UeAgentOriginResolve::FResolveResult Result;
				if (!UeAgentOriginResolve::ResolveOrigin(
						Params,
						Result,
						ResolveError))
				{
					return Fail(
						OutError,
						TEXT("reference_origin_resolve_failed"),
						Params.TargetSource,
						ResolveError);
				}

				const FString OwnerId = TEXT("reference-origin:")
					+ Params.TargetSource;
				TArray<FUeAgentStateEntry> Entries;
				auto AddTargetString = [&](const TCHAR* Selector, const FString& Value)
				{
					return Value.IsEmpty()
						|| AddString(
							TEXT("reference.origin.target:") + FString(Selector),
							TEXT("reference_origin"),
							OwnerId,
							TEXT("target.") + FString(Selector),
							Value,
							Context,
							Entries,
							OutError);
				};
				if (!AddTargetString(TEXT("source"), Params.TargetSource)
					|| !AddTargetString(TEXT("candidate_id"), Params.CandidateId)
					|| !AddTargetString(TEXT("json_file"), Params.JsonFile)
					|| !AddTargetString(TEXT("json_pointer"), Params.JsonPointer)
					|| !AddTargetString(TEXT("kind"), Result.Kind.IsEmpty() ? Params.Kind : Result.Kind)
					|| !AddTargetString(TEXT("name"), Result.Name.IsEmpty() ? Params.Name : Result.Name)
					|| !AddTargetString(TEXT("full_name"), Result.FullName.IsEmpty() ? Params.FullName : Result.FullName)
					|| !AddTargetString(TEXT("asset_path"), Params.AssetPath)
					|| !AddTargetString(TEXT("graph_name"), Params.GraphName)
					|| !AddTargetString(TEXT("node_guid"), Params.NodeGuid)
					|| !AddStringArray(TEXT("reference.origin:asset_paths"), OwnerId, TEXT("origin.asset_paths"), Result.AssetPaths, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:package_paths"), OwnerId, TEXT("origin.package_paths"), Result.PackagePaths, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:package_files"), OwnerId, TEXT("origin.package_files"), Result.PackageFiles, Context, Entries, OutError))
				{
					return false;
				}

				const FString Status = Result.Status.IsEmpty()
					? FString(TEXT("resolved"))
					: Result.Status;
				if (!AddCodePaths(OwnerId, Result.CodePaths, Context, Entries, OutError)
					|| !AddBoolean(TEXT("reference.origin:resolved"), TEXT("reference_origin"), OwnerId, TEXT("resolved"), Result.bResolved, Context, Entries, OutError)
					|| !AddString(TEXT("reference.origin:status"), TEXT("reference_origin"), OwnerId, TEXT("status"), Status, Context, Entries, OutError)
					|| !AddStringArray(TEXT("reference.origin:warnings"), OwnerId, TEXT("warnings"), Result.Warnings, Context, Entries, OutError))
				{
					return false;
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FAssetHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				UObject* Asset = nullptr;
				if (!ResolveAsset(Target, Asset, OutError))
				{
					if (ProviderBinding
							!= TEXT("asset_project.read.reference_asset")
						|| OutError.Code
							!= TEXT("asset_project_read_asset_unresolved"))
					{
						return false;
					}
					OutError = {};
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (!AddObjectIdentity(
						Asset,
						TEXT("asset"),
						Context,
						Entries,
						OutError))
				{
					return false;
				}

				if (ProviderBinding
						== TEXT("asset_project.read.asset_properties")
					|| ProviderBinding
						== TEXT("asset_project.read.reference_asset"))
				{
					if (!AddRequestedProperties(
							Asset,
							ReadStringArray(
								Input,
								TEXT("field_ids")),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_relations"))
				{
					FString Direction;
					if (!Input->TryGetStringField(
							TEXT("direction"), Direction))
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_relation_direction_missing"),
							Asset->GetPathName());
					}
					FAssetData Record;
					if (!Target.IsSet()
						|| !Environment->TryResolveRegistryRecord(
							Target.GetValue(), Record, OutError)
						|| !Record.IsValid())
					{
						return OutError.Code.IsEmpty()
							? Fail(
								OutError,
								TEXT("asset_project_read_registry_unresolved"),
								Asset->GetPathName())
							: false;
					}
					TArray<FString> Dependencies;
					TArray<FString> Referencers;
					if (!Environment->TryResolveRegistryRelations(
							Record,
							Direction,
							ReadStringArray(Input, TEXT("relation_kinds")),
							Dependencies,
							Referencers,
							OutError))
					{
						return false;
					}
					if (!AddString(
							TEXT("asset.relations:direction"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("direction"),
							Direction,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					for (const FString& Dependency : Dependencies)
					{
						if (!AddString(
								TEXT("asset.relations:dependency:") + Dependency,
								TEXT("asset"),
								Asset->GetPathName(),
								TEXT("dependency"),
								Dependency,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					for (const FString& Referencer : Referencers)
					{
						if (!AddString(
								TEXT("asset.relations:referencer:") + Referencer,
								TEXT("asset"),
								Asset->GetPathName(),
								TEXT("referencer"),
								Referencer,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_validation"))
				{
					TArray<FString> RuleIds =
						ReadStringArray(Input, TEXT("rule_ids"));
					if (RuleIds.IsEmpty())
					{
						RuleIds = {TEXT("object.valid")};
					}
					bool bIncludePassed = false;
					Input->TryGetBoolField(
						TEXT("include_passed"), bIncludePassed);
					bool bOverallValid = true;
					for (const FString& RuleId : RuleIds)
					{
						bool bPassed = false;
						if (RuleId == TEXT("object.valid"))
						{
							bPassed = IsValid(Asset);
						}
						else if (RuleId == TEXT("package.present"))
						{
							bPassed = IsValid(Asset->GetOutermost());
						}
						else if (RuleId == TEXT("package.not_dirty"))
						{
							bPassed = IsValid(Asset->GetOutermost())
								&& !Asset->GetOutermost()->IsDirty();
						}
						else if (RuleId == TEXT("registry_record.present"))
						{
							FAssetData Record;
							FUeAgentAssetProjectReadError IgnoredError;
							bPassed = Target.IsSet()
								&& Environment->TryResolveRegistryRecord(
									Target.GetValue(), Record, IgnoredError)
								&& Record.IsValid();
						}
						else
						{
							return Fail(
								OutError,
								TEXT("asset_project_read_validation_rule_unsupported"),
								Asset->GetPathName(),
								RuleId);
						}
						bOverallValid = bOverallValid && bPassed;
						if (!bPassed || bIncludePassed)
						{
							const FString RuleOwner = Asset->GetPathName()
								+ TEXT("#validation:") + RuleId;
							if (!AddString(
									TEXT("asset.validation:rule:") + RuleId,
									TEXT("validation_rule"),
									RuleOwner,
									TEXT("validation_rule_id"),
									RuleId,
									Context,
									Entries,
									OutError)
								|| !AddBoolean(
									TEXT("asset.validation:passed:") + RuleId,
									TEXT("validation_rule"),
									RuleOwner,
									TEXT("validation_rule_passed"),
									bPassed,
									Context,
									Entries,
									OutError))
							{
								return false;
							}
						}
					}
					if (!AddBoolean(
							TEXT("asset.validation:valid"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("is_valid"),
							bOverallValid,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_collection"))
				{
					FString CollectionKind;
					if (!Input->TryGetStringField(
							TEXT("collection_kind"), CollectionKind))
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_collection_kind_missing"),
							Asset->GetPathName());
					}
					const bool bCaptured = CollectionKind == TEXT("rows")
						? TryAddDataTableRows(
							Asset, Input, Context, Entries, OutError)
						: (CollectionKind == TEXT("items")
								|| CollectionKind == TEXT("entries"))
							? TryAddReflectedCollection(
								Asset, CollectionKind, Input,
								Context, Entries, OutError)
							: Fail(
								OutError,
								TEXT("asset_project_read_collection_kind_unsupported"),
								Asset->GetPathName(),
								CollectionKind);
					if (!bCaptured)
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_sample"))
				{
					if (!TryAddAssetSample(
							Asset, Input, Context, Entries, OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FRegistryHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!Target.IsSet())
				{
					return Fail(
						OutError,
						TEXT("asset_project_read_target_required"),
						ProviderBinding);
				}
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				FAssetData Record;
				if (!Environment->TryResolveRegistryRecord(
						Target.GetValue(),
						Record,
						OutError)
					|| !Record.IsValid())
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"asset_project_read_registry_unresolved"),
							Target.GetValue().GetBindingId())
						: false;
				}
				const FString OwnerId = Record.GetObjectPathString();
				TArray<FUeAgentStateEntry> Entries;
				if (!AddString(
						TEXT("asset.registry:object_path"),
						TEXT("asset"),
						OwnerId,
						TEXT("object_path"),
						OwnerId,
						Context,
						Entries,
						OutError)
					|| !AddString(
						TEXT("asset.registry:class"),
						TEXT("asset"),
						OwnerId,
						TEXT("class_path"),
						Record.AssetClassPath.ToString(),
						Context,
						Entries,
						OutError)
					|| !AddString(
						TEXT("asset.registry:package"),
						TEXT("asset"),
						OwnerId,
						TEXT("package_name"),
						Record.PackageName.ToString(),
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (ProviderBinding
					== TEXT(
						"asset_project.read.registry_relations"))
				{
					FString Direction;
					if (!Input->TryGetStringField(
						TEXT("direction"),
						Direction))
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_relation_direction_missing"),
							OwnerId);
					}
					TArray<FString> Dependencies;
					TArray<FString> Referencers;
					if (!Environment->TryResolveRegistryRelations(
							Record,
							Direction,
							ReadStringArray(Input, TEXT("relation_kinds")),
							Dependencies,
							Referencers,
							OutError))
					{
						return false;
					}
					if (!AddString(
							TEXT("asset.registry:direction"),
							TEXT("asset"),
							OwnerId,
							TEXT("direction"),
							Direction,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					for (const FString& Dependency : Dependencies)
					{
						if (!AddString(
								TEXT("asset.registry:dependency:") + Dependency,
								TEXT("asset"),
								OwnerId,
								TEXT("dependency"),
								Dependency,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
					for (const FString& Referencer : Referencers)
					{
						if (!AddString(
								TEXT("asset.registry:referencer:") + Referencer,
								TEXT("asset"),
								OwnerId,
								TEXT("referencer"),
								Referencer,
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		bool TryCaptureEditorProjectCapability(
			const TSharedPtr<FJsonObject>& Input,
			const FString& ProviderBinding,
			const FUeAgentOperationContext& Context,
			TArray<FUeAgentStateEntry>& OutEntries,
			bool& bOutHandled,
			FUeAgentAssetProjectReadError& OutError)
		{
			bOutHandled = false;
			FString Action;
			if (!Input.IsValid()
				|| !Input->TryGetStringField(
					TEXT("editor_project_capability"), Action))
			{
				return true;
			}
			bOutHandled = true;
			if (!EditorProject::IsEditorProjectQueryRoute(
					ProviderBinding, Action))
			{
				return Fail(
					OutError,
					TEXT("editor_project_query_route_mismatch"),
					ProviderBinding,
					Action);
			}

			TSharedPtr<FJsonObject> Parameters = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* SuppliedParameters = nullptr;
			if (Input->TryGetObjectField(
					TEXT("parameters"), SuppliedParameters)
				&& SuppliedParameters
				&& SuppliedParameters->IsValid())
			{
				Parameters = *SuppliedParameters;
			}

			static const TSharedRef<const EditorProject::
				IEditorProjectQueryMechanicsPort> Port =
				EditorProject::CreateEditorProjectQueryMechanicsPort();
			TSharedPtr<FJsonObject> Observation;
			FString NativeError;
			if (!Port->Execute(
					Action,
					{Context.GetRequestId(), Parameters},
					Observation,
					NativeError)
				|| !Observation.IsValid())
			{
				return Fail(
					OutError,
					TEXT("editor_project_query_mechanics_failed"),
					Action,
					NativeError);
			}

			FString ObservationJson;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&ObservationJson);
			if (!FJsonSerializer::Serialize(
					Observation.ToSharedRef(), Writer))
			{
				return Fail(
					OutError,
					TEXT("editor_project_query_observation_invalid"),
					Action);
			}
			return AddString(
				TEXT("editor_project.capability:") + Action,
				TEXT("editor_project"),
				Context.GetExecutionContext().GetProjectInstanceId(),
				TEXT("observation_json"),
				ObservationJson,
				Context,
				OutEntries,
				OutError);
		}

		class FEditorHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				TArray<FUeAgentEditorResourceObservation> Resources;
				bool bIncludeEditorIdentity = false;
				Input->TryGetBoolField(
					TEXT("include_editor_identity"),
					bIncludeEditorIdentity);
				const bool bResolved = ProviderBinding
						== TEXT("asset_project.read.editor_open_assets")
					? Environment->TryResolveOpenAssets(
						Context.GetExecutionContext(),
						bIncludeEditorIdentity,
						Resources,
						OutError)
					: Environment->TryResolveDirtyResources(
						Context.GetExecutionContext(),
						ReadStringArray(Input, TEXT("resource_kinds")),
						Resources,
						OutError);
				if (!bResolved)
				{
					return false;
				}
				for (const FUeAgentEditorResourceObservation& Resource : Resources)
				{
					if (Resource.ResourceId.IsEmpty()
						|| Resource.ResourceKind.IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("asset_project_read_editor_resource_invalid"),
							ProviderBinding);
					}
					const FString& OwnerId = Resource.ResourceId;
					if (!AddString(
							TEXT("editor.resource:ref:")
								+ OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_ref"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddString(
							TEXT("editor.resource:kind:") + OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_kind"),
							Resource.ResourceKind,
							Context,
							Entries,
							OutError)
						|| !AddBoolean(
							TEXT("editor.resource:dirty:")
								+ OwnerId,
							TEXT("editor_resource"),
							OwnerId,
							TEXT("resource_dirty"),
							Resource.bDirty,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
					auto AddOptional = [&](const TCHAR* Semantic,
						const TCHAR* Selector, const FString& Value)
					{
						return Value.IsEmpty()
							|| AddString(
								FString(Semantic) + OwnerId,
								TEXT("editor_resource"),
								OwnerId,
								Selector,
								Value,
								Context,
								Entries,
								OutError);
					};
					if (!AddOptional(
							TEXT("editor.resource:package:"),
							TEXT("package_name"),
							Resource.PackageName)
						|| !AddOptional(
							TEXT("editor.resource:object:"),
							TEXT("object_path"),
							Resource.ObjectPath)
						|| !AddOptional(
							TEXT("editor.resource:class:"),
							TEXT("class_path"),
							Resource.ClassPath)
						|| !AddOptional(
							TEXT("editor.resource:editor:"),
							TEXT("editor_identity"),
							Resource.EditorIdentity))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FProjectHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				if (ProviderBinding
					== TEXT("asset_project.read.project_platform"))
				{
					const TArray<FString> PlatformIds =
						ReadStringArray(Input, TEXT("platform_ids"));
					const TArray<FString> FieldIds =
						ReadStringArray(Input, TEXT("field_ids"));
					static const TArray<FString> SupportedFields = {
						TEXT("platform_name"),
						TEXT("ini_platform_name"),
						TEXT("display_name"),
						TEXT("running_platform"),
						TEXT("sdk_installed"),
						TEXT("sdk_documentation_path"),
					};
					for (const FString& FieldId : FieldIds)
					{
						if (!SupportedFields.Contains(FieldId))
						{
							return Fail(
								OutError,
								TEXT("asset_project_read_platform_field_unsupported"),
								FieldId);
						}
					}
					TArray<FUeAgentProjectPlatformObservation> Platforms;
					if (!Environment->TryResolveProjectPlatforms(
							Context.GetExecutionContext(),
							PlatformIds,
							Platforms,
							OutError))
					{
						return false;
					}
					bool bResolveInheritance = false;
					Input->TryGetBoolField(
						TEXT("resolve_inheritance"), bResolveInheritance);
					auto Includes = [&FieldIds](const TCHAR* Field)
					{
						return FieldIds.IsEmpty()
							|| FieldIds.Contains(Field);
					};
					for (const FUeAgentProjectPlatformObservation& Platform
						: Platforms)
					{
						if (Platform.PlatformName.IsEmpty())
						{
							return Fail(
								OutError,
								TEXT("asset_project_read_platform_observation_invalid"),
								Context.GetExecutionContext()
									.GetProjectInstanceId());
						}
						const FString Owner = TEXT("platform:")
							+ Platform.PlatformName;
						if (!AddBoolean(
								TEXT("project.platform:resolve_inheritance:") + Owner,
								TEXT("target_platform"), Owner,
								TEXT("resolve_inheritance"), bResolveInheritance,
								Context, Entries, OutError))
						{
							return false;
						}
						auto AddSelectedString = [&](const TCHAR* Field,
							const FString& Value)
						{
							return !Includes(Field)
								|| AddString(
									FString(TEXT("project.platform:"))
										+ Field + TEXT(":") + Owner,
									TEXT("target_platform"), Owner,
									Field, Value,
									Context, Entries, OutError);
						};
						auto AddSelectedBoolean = [&](const TCHAR* Field,
							const bool Value)
						{
							return !Includes(Field)
								|| AddBoolean(
									FString(TEXT("project.platform:"))
										+ Field + TEXT(":") + Owner,
									TEXT("target_platform"), Owner,
									Field, Value,
									Context, Entries, OutError);
						};
						if (!AddSelectedString(
								TEXT("platform_name"), Platform.PlatformName)
							|| !AddSelectedString(
								TEXT("ini_platform_name"), Platform.IniPlatformName)
							|| !AddSelectedString(
								TEXT("display_name"), Platform.DisplayName)
							|| !AddSelectedBoolean(
								TEXT("running_platform"), Platform.bRunningPlatform)
							|| !AddSelectedBoolean(
								TEXT("sdk_installed"), Platform.bSdkInstalled)
							|| !AddSelectedString(
								TEXT("sdk_documentation_path"),
								Platform.SdkDocumentationPath))
						{
							return false;
						}
					}
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				UObject* ProjectObject = nullptr;
				if (!Environment->TryResolveProjectObject(
						Context.GetExecutionContext(),
						TEXT("project"),
						ProjectObject,
						OutError)
					|| !ProjectObject)
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"asset_project_read_project_unresolved"),
							Context.GetExecutionContext()
								.GetProjectInstanceId())
						: false;
				}
				if (!AddObjectIdentity(
						ProjectObject,
						TEXT("project"),
						Context,
						Entries,
						OutError))
				{
					return false;
				}
				if (ProviderBinding
					== TEXT("asset_project.read.project_settings"))
				{
					if (!AddRequestedProperties(
							ProjectObject,
							ReadStringArray(
								Input,
								TEXT("field_ids")),
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.project_validation"))
				{
					const TArray<FString> SectionIds =
						ReadStringArray(Input, TEXT("section_ids"));
					for (const FString& SectionId : SectionIds)
					{
						if (SectionId != TEXT("project"))
						{
							return Fail(
								OutError,
								TEXT("asset_project_read_validation_section_unsupported"),
								SectionId);
						}
					}
					TArray<FString> RuleIds =
						ReadStringArray(Input, TEXT("rule_ids"));
					if (RuleIds.IsEmpty())
					{
						RuleIds = {TEXT("project_object.valid")};
					}
					bool bIncludePassed = false;
					Input->TryGetBoolField(
						TEXT("include_passed"), bIncludePassed);
					bool bOverallValid = true;
					for (const FString& RuleId : RuleIds)
					{
						if (RuleId != TEXT("project_object.valid"))
						{
							return Fail(
								OutError,
								TEXT("asset_project_read_validation_rule_unsupported"),
								ProjectObject->GetPathName(),
								RuleId);
						}
						const bool bPassed = IsValid(ProjectObject);
						bOverallValid = bOverallValid && bPassed;
						if (!bPassed || bIncludePassed)
						{
							const FString RuleOwner = ProjectObject->GetPathName()
								+ TEXT("#validation:") + RuleId;
							if (!AddString(
									TEXT("project.validation:rule:") + RuleId,
									TEXT("validation_rule"), RuleOwner,
									TEXT("validation_rule_id"), RuleId,
									Context, Entries, OutError)
								|| !AddBoolean(
									TEXT("project.validation:passed:") + RuleId,
									TEXT("validation_rule"), RuleOwner,
									TEXT("validation_rule_passed"), bPassed,
									Context, Entries, OutError))
							{
								return false;
							}
						}
					}
					if (!AddBoolean(
							TEXT("project.validation:valid"),
							TEXT("project"),
							ProjectObject->GetPathName(),
							TEXT("is_valid"),
							bOverallValid,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		class FConfigHook final : public FHookBase
		{
		public:
			using FHookBase::FHookBase;

			bool TryCapture(
				const FUeAgentContractValue& CanonicalInput,
				const TOptional<FUeAgentCandidateBinding>& Target,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentAssetProjectReadError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				TSharedPtr<FJsonObject> Input;
				if (!ParseInput(CanonicalInput, Input, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
				bool bHandled = false;
				if (!TryCaptureEditorProjectCapability(
						Input,
						ProviderBinding,
						Context,
						Entries,
						bHandled,
						OutError))
				{
					return false;
				}
				if (bHandled)
				{
					return BuildSnapshot(
						ProviderBinding,
						Context,
						MoveTemp(Entries),
						OutSnapshot,
						OutError);
				}
				FString ConfigScope;
				FString SectionId;
				if (!Input->TryGetStringField(
						TEXT("config_scope"),
						ConfigScope)
					|| !Input->TryGetStringField(
						TEXT("section_id"),
						SectionId))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_project_read_config_input_missing"),
						ProviderBinding);
				}
				const FConfigFile* Config = nullptr;
				if (!Environment->TryResolveConfig(
						Context.GetExecutionContext(),
						ConfigScope,
						Config,
						OutError)
					|| !Config)
				{
					return OutError.Code.IsEmpty()
						? Fail(
							OutError,
							TEXT(
								"asset_project_read_config_unresolved"),
							ConfigScope)
						: false;
				}
				const FString OwnerId =
					Context.GetExecutionContext().GetProjectInstanceId()
					+ TEXT(":config:")
					+ ConfigScope
					+ TEXT(":")
					+ SectionId;
				if (!AddString(
						TEXT("config.section:id"),
						TEXT("config"),
						OwnerId,
						TEXT("section_id"),
						SectionId,
						Context,
						Entries,
						OutError))
				{
					return false;
				}

				if (ProviderBinding
					== TEXT(
						"asset_project.read.config_resolved_value"))
				{
					FString Key;
					if (!Input->TryGetStringField(TEXT("key"), Key))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_project_read_config_key_missing"),
							SectionId);
					}
					FString Value;
					if (!Config->GetString(
							*SectionId,
							*Key,
							Value))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_project_read_config_value_missing"),
							SectionId,
							Key);
					}
					if (!AddString(
							TEXT("config.value:") + Key,
							TEXT("config"),
							OwnerId,
							Key,
							Value,
							Context,
							Entries,
							OutError))
					{
						return false;
					}
				}
				else
				{
					const FConfigSection* Section =
						Config->FindSection(SectionId);
					if (!Section)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_project_read_config_section_missing"),
							SectionId);
					}
					TSet<FString> RequestedKeys;
					for (const FString& Key
						: ReadStringArray(Input, TEXT("keys")))
					{
						RequestedKeys.Add(Key);
					}
					for (FConfigSection::TConstIterator It(*Section);
						It;
						++It)
					{
						const FString Key = It.Key().ToString();
						if (!RequestedKeys.IsEmpty()
							&& !RequestedKeys.Contains(Key))
						{
							continue;
						}
						if (!AddString(
								TEXT("config.value:") + Key,
								TEXT("config"),
								OwnerId,
								Key,
								It.Value().GetSavedValue(),
								Context,
								Entries,
								OutError))
						{
							return false;
						}
					}
				}
				return BuildSnapshot(
					ProviderBinding,
					Context,
					MoveTemp(Entries),
					OutSnapshot,
					OutError);
			}
		};

		bool IsOneOf(
			const FString& Value,
			std::initializer_list<const TCHAR*> Candidates)
		{
			for (const TCHAR* Candidate : Candidates)
			{
				if (Value == Candidate)
				{
					return true;
				}
			}
			return false;
		}
	}

	bool FUeAgentAssetProjectReadNativeHookFactory::TryCreate(
		const FString& ProviderBinding,
		TSharedRef<IUeAgentAssetProjectReadEnvironment> Environment,
		TSharedPtr<IUeAgentAssetProjectReadHook>& OutHook,
		FUeAgentAssetProjectReadError& OutError)
	{
		OutHook.Reset();
		OutError = {};
		if (IsOneOf(
				ProviderBinding,
				{
					TEXT("asset_project.read.reference_origin"),
				}))
		{
			OutHook = MakeShared<FReferenceOriginHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
				ProviderBinding,
				{
					TEXT("asset_project.read.asset_summary"),
					TEXT("asset_project.read.asset_properties"),
					TEXT("asset_project.read.reference_asset"),
					TEXT("asset_project.read.asset_relations"),
					TEXT("asset_project.read.asset_validation"),
					TEXT("asset_project.read.asset_collection"),
					TEXT("asset_project.read.asset_sample"),
				}))
		{
			OutHook = MakeShared<FAssetHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.registry_record"),
					TEXT("asset_project.read.registry_relations"),
				}))
		{
			OutHook = MakeShared<FRegistryHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.editor_open_assets"),
					TEXT("asset_project.read.editor_dirty_resources"),
				}))
		{
			OutHook = MakeShared<FEditorHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.project_summary"),
					TEXT("asset_project.read.project_settings"),
					TEXT("asset_project.read.project_platform"),
					TEXT("asset_project.read.project_validation"),
				}))
		{
			OutHook = MakeShared<FProjectHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else if (IsOneOf(
			ProviderBinding,
				{
					TEXT("asset_project.read.config_section"),
					TEXT(
						"asset_project.read.config_resolved_value"),
				}))
		{
			OutHook = MakeShared<FConfigHook>(
				ProviderBinding,
				MoveTemp(Environment));
		}
		else
		{
			return Fail(
				OutError,
				TEXT("asset_project_read_provider_unknown"),
				ProviderBinding);
		}
		return true;
	}
}
