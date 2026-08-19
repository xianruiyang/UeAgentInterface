#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableCurveTableDomainMechanics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StructOnScope.h"
#include "Utilities/UeAgentCurveJson.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString SchemaId = TEXT("uai.datatable_curvetable.edit/1");
		const FString DataTableType = TEXT("/Script/Engine.DataTable");
		const FString CurveTableType = TEXT("/Script/Engine.CurveTable");

		class FNeverCancelled final
			: public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("datatable_curvetable_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_json_canonicalization_failed"),
					Error);
			}
			return true;
		}

		bool ParseDocument(
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutRoot,
			FWorkspaceError& OutError)
		{
			OutRoot.Reset();
			FString DocumentText;
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_resolved_source_required"));
			}
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_resolved_source_invalid"));
			}
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject()
					: nullptr;
				FString RelativePath;
				FString Text;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(
						TEXT("document"), Text))
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					DocumentText = MoveTemp(Text);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || DocumentText.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_resolved_source_invalid"),
					EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:datatable-curvetable-domain"),
					Resources,
					Document,
					OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Value =
				JsonValueFromHjson(*Document->GetRoot(), OutError);
			const TSharedPtr<FJsonObject> Parsed =
				Value.IsValid() ? Value->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			if (!Parsed.IsValid()
				|| !Parsed->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !Parsed->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid())
			{
				return Fail(
					OutError, TEXT("datatable_curvetable_document_invalid"));
			}
			OutRoot = *Root;
			return true;
		}

		bool SplitTarget(
			const FString& Target,
			FString& OutPackage,
			FString& OutObject,
			FWorkspaceError& OutError)
		{
			if (!Target.Split(TEXT("."), &OutPackage, &OutObject)
				|| !FPackageName::IsValidLongPackageName(OutPackage)
				|| OutObject.IsEmpty()
				|| OutObject != FPackageName::GetLongPackageAssetName(OutPackage))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_target_identity_invalid"),
					Target);
			}
			return true;
		}

		FString PackageRevision(
			const FString& Target,
			FWorkspaceError& OutError)
		{
			FString PackageName;
			FString ObjectName;
			if (!SplitTarget(Target, PackageName, ObjectName, OutError))
			{
				return FString();
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetAssetPackageExtension());
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.IsEmpty())
			{
				Fail(
					OutError,
					TEXT("datatable_curvetable_persisted_package_missing"),
					PackageName);
				return FString();
			}
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Sha256Text(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			return Sha256Bytes(Bytes);
		}

		UObject* LoadTarget(
			const FString& Target,
			const FString& RootType,
			FWorkspaceError& OutError)
		{
			UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Target);
			const bool bTypeMatches =
				(RootType == DataTableType && Cast<UDataTable>(Object))
				|| (RootType == CurveTableType && Cast<UCurveTable>(Object));
			if (!Object || !bTypeMatches)
			{
				Fail(
					OutError,
					Object
						? FString(TEXT("datatable_curvetable_target_type_mismatch"))
						: FString(TEXT("datatable_curvetable_target_missing")),
					Target);
				return nullptr;
			}
			return Object;
		}

		bool SaveAndReload(
			UObject& Asset,
			const FString& Target,
			UObject*& OutReloaded,
			FWorkspaceError& OutError)
		{
			UPackage* Package = Asset.GetOutermost();
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, &Asset, *Filename, Args))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_save_failed"),
					Filename);
			}
			FText ReloadError;
			if (!UPackageTools::ReloadPackages(
					{Package},
					ReloadError,
					EReloadPackagesInteractionMode::AssumeNegative))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_reload_failed"),
					ReloadError.ToString());
			}
			OutReloaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Target);
			return OutReloaded != nullptr
				|| Fail(
					OutError,
					TEXT("datatable_curvetable_reload_readback_missing"),
					Target);
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("datatable-curvetable-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId,
				*RequestId,
				*CommandId,
				*ContractHash,
				*ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_internal_request_invalid"),
					Diagnostic.Code,
					Diagnostic.Message);
			}
			return true;
		}

		bool SubmitDataTableReplace(
			UDataTable& Table,
			const FString& Target,
			const TSharedPtr<FJsonObject>& Root,
			FWorkspaceError& OutError)
		{
			FString RowStructRef;
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Root->TryGetStringField(TEXT("row_struct_ref"), RowStructRef)
				|| !Root->TryGetArrayField(TEXT("rows"), Rows)
				|| !Rows)
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_data_table_definition_invalid"));
			}
			UScriptStruct* RowStruct =
				LoadObject<UScriptStruct>(nullptr, *RowStructRef);
			if (!RowStruct || !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_row_struct_unsupported"),
					RowStructRef);
			}
			if (!Table.GetRowStruct())
			{
				Table.RowStruct = RowStruct;
			}

			TArray<TSharedPtr<FJsonValue>> CanonicalRows;
			TSet<FString> Keys;
			for (int32 Index = 0; Index < Rows->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Row = (*Rows)[Index]->AsObject();
				FString Key;
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (!Row.IsValid()
					|| !Row->TryGetStringField(TEXT("key"), Key)
					|| Key.IsEmpty()
					|| Keys.Contains(Key)
					|| !Row->TryGetObjectField(TEXT("values"), Values)
					|| !Values || !Values->IsValid())
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_row_invalid"),
						FString::FromInt(Index));
				}
				Keys.Add(Key);
				const TSharedPtr<FJsonObject> CanonicalRow = MakeShared<FJsonObject>();
				CanonicalRow->SetStringField(TEXT("name"), Key);
				CanonicalRow->SetObjectField(TEXT("values"), *Values);
				CanonicalRows.Add(MakeShared<FJsonValueObject>(CanonicalRow));
			}

			const auto MakeChange = [](
				const FString& Field,
				const FString& Type,
				const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Typed = MakeShared<FJsonObject>();
				Typed->SetStringField(TEXT("type_id"), Type);
				Typed->SetField(TEXT("canonical_value"), Value);
				const TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("field_id"), Field);
				Change->SetStringField(TEXT("presence"), TEXT("replace"));
				Change->SetObjectField(TEXT("value"), Typed);
				return MakeShared<FJsonValueObject>(Change);
			};
			TArray<TSharedPtr<FJsonValue>> Changes;
			Changes.Add(MakeChange(
				TEXT("row_struct_ref"),
				TEXT("uai.ref.struct.v1"),
				MakeShared<FJsonValueString>(RowStructRef)));
			Changes.Add(MakeChange(
				TEXT("rows"),
				TEXT("uai.asset.data_table.rows.v1"),
				MakeShared<FJsonValueArray>(CanonicalRows)));
			const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target_ref"), Target);
			Params->SetStringField(
				TEXT("expected_revision"),
				FString::Printf(
					TEXT("asset-crc32:%08x"),
					FArchiveObjectCrc32().Crc32(&Table)));
			Params->SetArrayField(TEXT("changes"), Changes);

			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry;
			TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
			FString CompositionError;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, Dispatch, CompositionError)
				|| !Registry.IsValid())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_command_registry_unavailable"),
					CompositionError);
			}
			const auto* FrozenContract =
				Registry->FindExact(TEXT("asset.data_table.replace"));
			if (!FrozenContract)
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_data_table_mechanics_missing"));
			}
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
			TOptional<FUeAgentCommandRequest> Request;
			if (!DecodeRequest(
					TEXT("asset.data_table.replace"),
					FrozenContract->GetContractHash(),
					ParamsJson,
					Request,
					OutError))
			{
				return false;
			}
			TArray<FString> Scopes = AssetDataSimpleEffect::
				GetAssetDataSimpleEffectAuthorityScopes();
			Scopes.AddUnique(TEXT("uai:operate"));
			Scopes.AddUnique(TEXT("uai:query"));
			const FUeAgentCommandResult Result =
				OperationOwners::AssetSimple::SubmitProductionAssetSimpleOperation(
					Request.GetValue(),
					Contract::FUeAgentCallerContext(
						TEXT("datatable-curvetable-domain-provider"),
						Scopes,
						Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			return Result.IsOk()
				|| Fail(
					OutError,
					TEXT("datatable_curvetable_data_table_mechanics_failed"),
					Result.GetErrorJson());
		}

		bool BuildDataTableState(
			UDataTable& Table,
			TSharedPtr<FJsonObject>& OutState,
			FWorkspaceError& OutError)
		{
			const UScriptStruct* RowStruct = Table.GetRowStruct();
			if (!RowStruct)
			{
				return Fail(
					OutError, TEXT("datatable_curvetable_row_struct_missing"));
			}
			TArray<FName> Names;
			Table.GetRowMap().GetKeys(Names);
			Names.Sort([](const FName Left, const FName Right)
			{
				return Left.LexicalLess(Right);
			});
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FName Name : Names)
			{
				const uint8* const* Data = Table.GetRowMap().Find(Name);
				const TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
				if (!Data || !*Data
					|| !FJsonObjectConverter::UStructToJsonObject(
						RowStruct, *Data, Values.ToSharedRef(), 0, 0))
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_row_readback_failed"),
						Name.ToString());
				}
				const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Name.ToString());
				Row->SetObjectField(TEXT("values"), Values);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			OutState = MakeShared<FJsonObject>();
			OutState->SetStringField(TEXT("kind"), TEXT("data_table"));
			OutState->SetStringField(TEXT("row_struct_ref"), RowStruct->GetPathName());
			OutState->SetArrayField(TEXT("rows"), Rows);
			OutState->SetNumberField(TEXT("row_count"), Rows.Num());
			return true;
		}

		bool BuildCurveTableState(
			UCurveTable& Table,
			TSharedPtr<FJsonObject>& OutState,
			FWorkspaceError& OutError)
		{
			OutState = UeAgentCurveJson::BuildCurveAssetJson(&Table);
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!OutState.IsValid()
				|| !OutState->TryGetArrayField(TEXT("rows"), Rows)
				|| !Rows)
			{
				return Fail(
					OutError, TEXT("datatable_curvetable_curve_readback_failed"));
			}
			TArray<TSharedPtr<FJsonValue>> Sorted = *Rows;
			Sorted.Sort([](
				const TSharedPtr<FJsonValue>& Left,
				const TSharedPtr<FJsonValue>& Right)
			{
				FString A;
				FString B;
				const TSharedPtr<FJsonObject> L = Left->AsObject();
				const TSharedPtr<FJsonObject> R = Right->AsObject();
				if (L.IsValid()) L->TryGetStringField(TEXT("row_name"), A);
				if (R.IsValid()) R->TryGetStringField(TEXT("row_name"), B);
				return A < B;
			});
			OutState->SetArrayField(TEXT("rows"), Sorted);
			return true;
		}

		bool BuildState(
			UObject& Asset,
			const FString& RootType,
			TSharedPtr<FJsonObject>& OutState,
			FWorkspaceError& OutError)
		{
			return RootType == DataTableType
				? BuildDataTableState(*CastChecked<UDataTable>(&Asset), OutState, OutError)
				: BuildCurveTableState(*CastChecked<UCurveTable>(&Asset), OutState, OutError);
		}

		bool ImportDataTable(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			UObject*& OutReloaded,
			FWorkspaceError& OutError)
		{
			FString RowStructRef;
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Root->TryGetStringField(TEXT("row_struct_ref"), RowStructRef)
				|| !Root->TryGetArrayField(TEXT("rows"), Rows)
				|| !Rows)
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_data_table_definition_invalid"));
			}
			UScriptStruct* RowStruct =
				LoadObject<UScriptStruct>(nullptr, *RowStructRef);
			if (!RowStruct || !RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_row_struct_unsupported"),
					RowStructRef);
			}
			TSet<FString> ValidatedKeys;
			for (int32 Index = 0; Index < Rows->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Row = (*Rows)[Index]->AsObject();
				FString Key;
				const TSharedPtr<FJsonObject>* Values = nullptr;
				if (!Row.IsValid()
					|| !Row->TryGetStringField(TEXT("key"), Key)
					|| Key.IsEmpty()
					|| ValidatedKeys.Contains(Key)
					|| !Row->TryGetObjectField(TEXT("values"), Values)
					|| !Values || !Values->IsValid())
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_row_invalid"),
						FString::FromInt(Index));
				}
				ValidatedKeys.Add(Key);
				FStructOnScope Probe(RowStruct);
				if (!Probe.IsValid()
					|| !FJsonObjectConverter::JsonObjectToUStruct(
						(*Values).ToSharedRef(),
						RowStruct,
						Probe.GetStructMemory(),
						0,
						0))
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_row_values_invalid"),
						Key);
				}
			}
			UDataTable* Table = Cast<UDataTable>(
				StaticLoadObject(UDataTable::StaticClass(), nullptr, *Target));
			const bool bCreate = ExpectedRevision == TEXT("missing");
			if (bCreate == (Table != nullptr))
			{
				return Fail(
					OutError,
					bCreate
						? FString(TEXT("datatable_curvetable_target_exists"))
						: FString(TEXT("datatable_curvetable_target_missing")),
					Target);
			}
			if (!bCreate)
			{
				const FString Current = PackageRevision(Target, OutError);
				if (Current.IsEmpty()) return false;
				if (Current != ExpectedRevision)
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_stale_revision"),
						Target,
						Current);
				}
			}
			if (bCreate)
			{
				FString PackageName;
				FString ObjectName;
				if (!SplitTarget(Target, PackageName, ObjectName, OutError)) return false;
				UPackage* Package = CreatePackage(*PackageName);
				Table = NewObject<UDataTable>(
					Package,
					*ObjectName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (!Table)
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_data_table_create_failed"),
						Target);
				}
				FAssetRegistryModule::AssetCreated(Table);
				Package->MarkPackageDirty();
			}
			if (!SubmitDataTableReplace(*Table, Target, Root, OutError))
			{
				if (bCreate)
				{
					FAssetRegistryModule::AssetDeleted(Table);
					Table->ClearFlags(RF_Public | RF_Standalone);
					Table->MarkAsGarbage();
					Table->GetOutermost()->SetDirtyFlag(false);
				}
				return false;
			}
			return SaveAndReload(*Table, Target, OutReloaded, OutError);
		}

		bool ImportCurveTable(
			const FString& Target,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Root,
			UObject*& OutReloaded,
			FWorkspaceError& OutError)
		{
			UCurveTable* Table = Cast<UCurveTable>(
				StaticLoadObject(UCurveTable::StaticClass(), nullptr, *Target));
			const bool bCreate = ExpectedRevision == TEXT("missing");
			if (bCreate == (Table != nullptr))
			{
				return Fail(
					OutError,
					bCreate
						? FString(TEXT("datatable_curvetable_target_exists"))
						: FString(TEXT("datatable_curvetable_target_missing")),
					Target);
			}
			if (!bCreate)
			{
				const FString Current = PackageRevision(Target, OutError);
				if (Current.IsEmpty()) return false;
				if (Current != ExpectedRevision)
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_stale_revision"),
						Target,
						Current);
				}
			}

			UCurveTable* Probe = NewObject<UCurveTable>(GetTransientPackage());
			TArray<TSharedPtr<FJsonValue>> Issues;
			if (!Probe
				|| !UeAgentCurveJson::ApplyCurveAssetJson(
					Probe, Root, TEXT("$.root"), Issues)
				|| UeAgentCurveJson::HasError(Issues))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_curve_definition_invalid"));
			}
			if (bCreate)
			{
				FString CreateError;
				Table = Cast<UCurveTable>(UeAgentCurveJson::CreateCurveAsset(
					Target, TEXT("curve_table"), CreateError));
				if (!Table)
				{
					return Fail(
						OutError,
						TEXT("datatable_curvetable_curve_table_create_failed"),
						CreateError.IsEmpty() ? Target : CreateError);
				}
			}
			Table->Modify();
			Table->EmptyTable();
			Issues.Reset();
			if (!UeAgentCurveJson::ApplyCurveAssetJson(
					Table, Root, TEXT("$.root"), Issues)
				|| UeAgentCurveJson::HasError(Issues))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_curve_apply_failed"));
			}
			Table->GetOutermost()->MarkPackageDirty();
			return SaveAndReload(*Table, Target, OutReloaded, OutError);
		}

		bool BuildWriteResponse(
			const FString& Target,
			const FString& RootType,
			UObject& Asset,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TSharedPtr<FJsonObject> State;
			if (!BuildState(Asset, RootType, State, OutError)) return false;
			const FString Revision = PackageRevision(Target, OutError);
			if (Revision.IsEmpty()) return false;
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(
				TEXT("provider_id"), TEXT("datatable-curvetable-domain-provider"));
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetBoolField(TEXT("reloaded"), true);
			Response->SetObjectField(TEXT("independent_readback"), State);
			return CanonicalObject(Response, OutPayload, OutError);
		}

		class FVerifiedDataTableCurveTableExportSource final
			: public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedDataTableCurveTableExportSource(
				FString InTarget,
				FString InRootType,
				FString InCanonicalDocument)
				: Target(MoveTemp(InTarget))
				, RootType(MoveTemp(InRootType))
				, CanonicalDocument(MoveTemp(InCanonicalDocument))
			{
			}

			bool TryFreezeTarget(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

			bool TryCaptureWorkspace(
				const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				FTCHARToUTF8 Utf8(*CanonicalDocument);
				TArray<uint8> Bytes;
				Bytes.Append(
					reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				const FString FileHash = Sha256Text(CanonicalDocument);
				OutSemantic = Signature(FileHash);
				OutPlan = MakeShared<FExportWorkspacePlan>(
					EWorkspaceKind::Directory,
					TEXT("table.hjson"),
					Target,
					FrozenRevision,
					Sha256Text(Target + TEXT("|") + FileHash),
					TArray<FExportFilePlan>{
						FExportFilePlan(TEXT("table.hjson"), Bytes, FileHash),
					},
					Sha256Text(FileHash + TEXT("|")
						+ FrozenRevision.GetGeneration()),
					FMath::Max<int64>(1, Bytes.Num() / 4));
				return true;
			}

			bool TryRebuildStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				TArray<uint8> Bytes;
				const FString Path = StagedWorkspacePath / EntryFile;
				if (!FFileHelper::LoadFileToArray(Bytes, *Path))
				{
					OutError.Set(
						TEXT("datatable_curvetable_export_rebuild_failed"), Path);
					return false;
				}
				OutSemantic = Signature(Sha256Bytes(Bytes));
				return true;
			}

			bool TryReadCurrentTargetRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

		private:
			FExportSemanticSignature Signature(const FString& FileHash) const
			{
				return FExportSemanticSignature(
					Sha256Text(TEXT("table.hjson")),
					Sha256Text(Target + TEXT("|") + RootType),
					FileHash,
					Sha256Text(SchemaId),
					Sha256Text(RootType + TEXT("|") + FileHash));
			}

			bool ReadRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) const
			{
				FWorkspaceError WorkspaceError;
				const FString Revision = PackageRevision(Target, WorkspaceError);
				if (Revision.IsEmpty())
				{
					OutError.Set(
						WorkspaceError.GetCode().IsEmpty()
							? FString(TEXT("datatable_curvetable_export_target_missing"))
							: WorkspaceError.GetCode(),
						Target,
						WorkspaceError.GetSourceUnit());
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					TEXT("datatable-curvetable-export"), Revision, 1);
				return true;
			}

			FString Target;
			FString RootType;
			FString CanonicalDocument;
		};

		bool ExportWorkspace(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Target,
			const FString& RootType,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FString WorkspacePath;
			if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
				|| WorkspacePath.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_export_path_required"));
			}
			UObject* Asset = LoadTarget(Target, RootType, OutError);
			if (!Asset) return false;
			TSharedPtr<FJsonObject> State;
			if (!BuildState(*Asset, RootType, State, OutError)) return false;
			State->SetStringField(TEXT("type"), RootType);
			const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("schema"), SchemaId);
			Document->SetStringField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("root"), State);
			FString CanonicalDocument;
			if (!CanonicalObject(Document, CanonicalDocument, OutError)) return false;

			FUeAgentResourceTracker Resources = MakeResources();
			FArtifactPublishReceipt Receipt;
			FArtifactPublishError PublishError;
			if (!TryExportVerifiedWorkspace(
					FVerifiedExportRequest(
						MakeShared<FVerifiedDataTableCurveTableExportSource>(
							Target, RootType, CanonicalDocument),
						WorkspacePath,
						TEXT("datatable-curvetable-domain-provider")),
					Resources,
					Receipt,
					PublishError))
			{
				return Fail(
					OutError,
					PublishError.GetCode(),
					PublishError.GetPath(),
					PublishError.GetDetail());
			}
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("status"), TEXT("published"));
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			Response->SetStringField(
				TEXT("workspace_path"), Receipt.GetFinalPath());
			Response->SetStringField(TEXT("entry_file"), Receipt.GetEntryFile());
			Response->SetStringField(
				TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
			Response->SetStringField(
				TEXT("inventory_hash"), Receipt.GetInventoryHash());
			const TSharedPtr<FJsonObject> Semantic = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Field
				: Receipt.GetSemanticVerificationFields())
			{
				Semantic->SetStringField(Field.Key, Field.Value);
			}
			Response->SetObjectField(TEXT("semantic_verification"), Semantic);
			return CanonicalObject(Response, OutPayload, OutError);
		}
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(
				OutError, TEXT("datatable_curvetable_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(
				OutError,
				TEXT("datatable_curvetable_target_identity_mismatch"),
				TargetIdentity);
		}
		if (RootTypeIdentity != DataTableType
			&& RootTypeIdentity != CurveTableType)
		{
			return Fail(
				OutError,
				TEXT("datatable_curvetable_root_type_unsupported"),
				RootTypeIdentity);
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_expected_revision_required"));
			}
			TSharedPtr<FJsonObject> Root;
			if (!ParseDocument(Params, Root, OutError)) return false;
			FString DocumentType;
			if (!Root->TryGetStringField(TEXT("type"), DocumentType)
				|| DocumentType != RootTypeIdentity)
			{
				return Fail(
					OutError,
					TEXT("datatable_curvetable_document_type_mismatch"),
					DocumentType);
			}
			UObject* Reloaded = nullptr;
			const bool bImported = RootTypeIdentity == DataTableType
				? ImportDataTable(
					Target, ExpectedRevision, Root, Reloaded, OutError)
				: ImportCurveTable(
					Target, ExpectedRevision, Root, Reloaded, OutError);
			return bImported && Reloaded
				&& BuildWriteResponse(
					Target,
					RootTypeIdentity,
					*Reloaded,
					OutPayloadJson,
					OutError);
		}
		if (CommandId == TEXT("content_graph.export_workspace"))
		{
			bOutReadOnly = true;
			return ExportWorkspace(
				Params,
				Target,
				RootTypeIdentity,
				OutPayloadJson,
				OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			UObject* Asset = LoadTarget(Target, RootTypeIdentity, OutError);
			if (!Asset) return false;
			TSharedPtr<FJsonObject> State;
			if (!BuildState(*Asset, RootTypeIdentity, State, OutError)) return false;
			const FString Revision = PackageRevision(Target, OutError);
			if (Revision.IsEmpty()) return false;
			const TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootTypeIdentity);
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), State);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		return Fail(
			OutError,
			TEXT("datatable_curvetable_command_not_supported"),
			CommandId);
	}
}
