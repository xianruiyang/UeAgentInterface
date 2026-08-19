#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"

#include "AssetRegistry/AssetData.h"
#include "Commands/OriginResolve/UeAgentOriginResolveTypes.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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
				UObject* Asset = nullptr;
				if (!ResolveAsset(Target, Asset, OutError))
				{
					return false;
				}
				TArray<FUeAgentStateEntry> Entries;
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
					== TEXT("asset_project.read.asset_properties"))
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
					Input->TryGetStringField(
						TEXT("direction"),
						Direction);
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
				}
				else if (ProviderBinding
					== TEXT("asset_project.read.asset_validation"))
				{
					if (!AddBoolean(
							TEXT("asset.validation:valid"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("is_valid"),
							IsValid(Asset),
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
					Input->TryGetStringField(
						TEXT("collection_kind"),
						CollectionKind);
					if (!AddString(
							TEXT("asset.collection:kind"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("collection_kind"),
							CollectionKind,
							Context,
							Entries,
							OutError)
						|| !AddRequestedProperties(
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
					== TEXT("asset_project.read.asset_sample"))
				{
					FString SampleKind;
					Input->TryGetStringField(
						TEXT("sample_kind"),
						SampleKind);
					if (!AddString(
							TEXT("asset.sample:kind"),
							TEXT("asset"),
							Asset->GetPathName(),
							TEXT("sample_kind"),
							SampleKind,
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
				TArray<UPackage*> Packages;
				if (!Environment->TryResolveEditorResources(
						Context.GetExecutionContext(),
						Packages,
						OutError))
				{
					return false;
				}
				for (UPackage* Package : Packages)
				{
					if (!Package)
					{
						continue;
					}
					const FString OwnerId = Package->GetName();
					if (!AddString(
							TEXT("editor.resource:path:")
								+ OwnerId,
							TEXT("package"),
							OwnerId,
							TEXT("package_name"),
							OwnerId,
							Context,
							Entries,
							OutError)
						|| !AddBoolean(
							TEXT("editor.resource:dirty:")
								+ OwnerId,
							TEXT("package"),
							OwnerId,
							TEXT("package_dirty"),
							Package->IsDirty(),
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
					== TEXT("asset_project.read.project_settings")
					|| ProviderBinding
						== TEXT(
							"asset_project.read.project_platform"))
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
					if (!AddBoolean(
							TEXT("project.validation:valid"),
							TEXT("project"),
							ProjectObject->GetPathName(),
							TEXT("is_valid"),
							IsValid(ProjectObject),
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
