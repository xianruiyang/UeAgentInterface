#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableCurveTableDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringState.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringApply.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringState.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentCurveTableAuthoringApply.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DataTableEditorUtils.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::DataTableCurveTable
{
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString SchemaId = TEXT("uai.datatable_curvetable.edit/1");
		const FString DataTableType = TEXT("/Script/Engine.DataTable");
		const FString CurveTableType = TEXT("/Script/Engine.CurveTable");

		Contract::FDiagnostic TableDomainFact(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			bool bNative = true)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual,
				TEXT("authoring_apply"), Reason);
			Error.TargetPath = Target;
			if (bNative)
			{
				Error.SourceKind = TEXT("native_result");
				Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset();
			}
			return Error;
		}

		Contract::FDiagnostic TableDomainCondition(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Path, bool Actual, bool Expected, bool bNative = true)
		{
			return TableDomainFact(Code, Reason, Target, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), bNative);
		}

		Contract::FDiagnostic TableCommandFact(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = TableDomainFact(Code, Reason, Target, Path, Actual, Expected, false);
			Error.SourceKind = TEXT("command_input");
			Error.Stage = TEXT("command_validation");
			return Error;
		}

		Contract::FDiagnostic UnsupportedTableStructuralEdits(const TArray<FStructuralEdit>& Edits,
			const FString& Prefix, const FString& Target)
		{
			Contract::FDiagnostic Result;
			for (const FStructuralEdit& Edit : Edits)
			{
				auto Error = Edit.IsValid() ? TableDomainFact(Prefix + TEXT("_change_surface_unsupported"),
					TEXT("Table imports support row values and root properties; this structural edit is unsupported."),
					Target, TEXT("/structural_edits"), MakeShared<FJsonValueString>(HjsonWorkspace::LexToString(Edit.GetKind())),
					MakeShared<FJsonValueString>(TEXT("row values or root properties")), false) : Edit.GetValidationDiagnostic();
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
				if (Edit.GetSource().IsSet())
				{
					FWorkspaceError Located;
					Located.SetFailure(Error);
					Located.Locate(Edit.GetSource().GetValue());
					Error = Located.GetDiagnostic();
				}
				Result.Append(Error);
			}
			return Result;
		}

		Contract::FDiagnostic UnsupportedTableChange(const FString& Prefix, const FString& Target,
			const FString& SemanticId, const FString& Owner, const FString& Property)
		{
			const bool bProperty = SemanticId == TEXT("core.property");
			auto Error = !bProperty ? TableDomainCondition(Prefix + TEXT("_change_surface_unsupported"),
				TEXT("The change is neither a table-row change nor a root-property change."), Target,
				TEXT("/supported_change_kind"), false, true)
				: TableDomainFact(Prefix + TEXT("_change_surface_unsupported"), Owner != Target
					? TEXT("The root-property change belongs to a different target.") : TEXT("The root-property change has no property path."),
					Target, Owner != Target ? TEXT("/owner_path") : TEXT("/property_path"),
					MakeShared<FJsonValueString>(Owner != Target ? Owner : Property),
					MakeShared<FJsonValueString>(Owner != Target ? Target : TEXT("non-empty property path")));
			Error.ActualEvidence = MakeShared<FJsonValueString>(SemanticId);
			return Error;
		}

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Source = FString(), const FString& Pointer = TEXT("/"))
        {
            OutError.SetFailure(MoveTemp(Diagnostic), Source, Pointer);
            return false;
        }

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError, Error.IsValid() ? Error : TableDomainCondition(TEXT("datatable_curvetable_json_canonicalization_failed"),
					TEXT("The table result has no JSON object to encode."), FString(), TEXT("/result_object_present"), false, true));
			}
			return true;
		}

		bool SplitTarget(
			const FString& Target,
			FString& OutPackage,
			FString& OutObject,
			FWorkspaceError& OutError)
		{
			const FString ObjectPath = DomainAuthoring::ObjectPathFromPackageIdentity(Target);
			if (!ObjectPath.Split(TEXT("."), &OutPackage, &OutObject)
				|| !FPackageName::IsValidLongPackageName(OutPackage)
				|| OutObject.IsEmpty()
				|| OutObject != FPackageName::GetLongPackageAssetName(OutPackage))
			{
				auto Error = TableDomainFact(TEXT("datatable_curvetable_target_identity_invalid"),
					TEXT("The table target must be a valid asset package path or its matching object path."), Target,
					TEXT("/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("valid package or matching package.asset path")), false);
				Error.SourceKind = TEXT("command_input"); Error.Source.JsonPath.Reset();
				return Fail(OutError, MoveTemp(Error));
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
			const bool bRead = FFileHelper::LoadFileToArray(Bytes, *Filename);
			if (!bRead || Bytes.IsEmpty())
			{
				auto Error = TableDomainFact(TEXT("datatable_curvetable_persisted_package_missing"),
					bRead ? TEXT("The saved table package file is empty.") : TEXT("The saved table package file could not be read."),
					PackageName, bRead ? TEXT("/package_byte_count") : TEXT("/package_read_succeeded"),
					bRead ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Bytes.Num())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					bRead ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("one or more package bytes"))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
				Error.Source.SourceFile = Filename; Error.Stage = TEXT("verification");
				Fail(OutError, MoveTemp(Error));
				return FString();
			}
			uint8 Digest[SHA256_DIGEST_LENGTH];
			if (!SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest))
			{
				auto Error = TableDomainCondition(TEXT("datatable_curvetable_revision_hash_failed"),
					TEXT("The saved table package SHA256 revision could not be calculated."), PackageName, TEXT("/revision_hash_succeeded"), false, true);
				Error.Source.SourceFile = Filename; Error.Stage = TEXT("verification");
				Fail(OutError, MoveTemp(Error));
				return FString();
			}
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
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
				Fail(OutError, TableDomainFact(Object ? TEXT("datatable_curvetable_target_type_mismatch") : TEXT("datatable_curvetable_target_missing"),
					Object ? TEXT("The loaded table target has a different native asset class.") : TEXT("The requested table target could not be loaded."),
					Target, TEXT("/target_class"), Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()), MakeShared<FJsonValueString>(RootType)));
				return nullptr;
			}
			return Object;
		}

		bool BuildDataTableState(
			UDataTable& Table,
			TSharedPtr<FJsonObject>& OutState,
			FWorkspaceError& OutError)
		{
			const UScriptStruct* RowStruct = Table.GetRowStruct();
			if (!RowStruct)
			{
				return Fail(OutError, TableDomainCondition(TEXT("datatable_curvetable_row_struct_missing"),
					TEXT("The DataTable has no row struct for readback."), Table.GetPathName(), TEXT("/row_struct_present"), false, true));
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
					auto Error = TableDomainCondition(TEXT("datatable_curvetable_row_readback_failed"),
						!Data || !*Data ? TEXT("The native DataTable row has no readable value storage.") : TEXT("The native DataTable row could not be converted to JSON for readback."),
						Table.GetPathName(), TEXT("/rows/") + Name.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), false, true);
					Error.CandidateName = Name.ToString();
					return Fail(OutError, MoveTemp(Error));
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
				return Fail(OutError, TableDomainFact(TEXT("datatable_curvetable_curve_readback_failed"),
					TEXT("The native CurveTable readback returned no rows array."), Table.GetPathName(), TEXT("/rows"),
					MakeShared<FJsonValueString>(OutState.IsValid() ? UeAgentJsonDiagnostics::JsonValueTypeToString(OutState->TryGetField(TEXT("rows"))) : TEXT("missing result")),
					MakeShared<FJsonValueString>(TEXT("array"))));
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

		bool BuildWriteResponse(const FString& Target, const FString& RootType,
			UObject& Asset, FString& OutPayload, FWorkspaceError& OutError);

		bool ProjectInitialTableFields(UObject& NativeDefaults, const TSharedPtr<FJsonObject>& Schema,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document, const FString& Prefix,
			TSharedPtr<FJsonValue>& RowsJson, TArray<TSharedPtr<FJsonValue>>& Properties, FWorkspaceError& OutError)
		{
			const FHjsonNode& Root = *Document.AstRoot;
			UE::UAI::Contract::FDiagnostic Error;
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalValuePtr CanonicalSchema, EmptyRows, DesiredRows;
			FUeAgentStateSemanticError StateError;
			if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Schema), Resources, CanonicalSchema, Error)
				|| !FUeAgentCanonicalValue::TryObject({}, Resources, EmptyRows, StateError))
			{
				OutError.SetFailure(Error.IsEmpty() ? StateError : Error, Document.SourceUnit);
				return false;
			}
			DesiredRows = EmptyRows;
			if (const FHjsonNode* Rows = Root.Get(TEXT("rows")))
			{
				FKeyedItemProjectionContract Contract;
				FUeAgentCanonicalValuePtr VisibleRows;
				bool bChanged = false;
				if (!TryGetKeyedItemProjectionContract(TEXT("rows"), Contract))
					return Fail(OutError, TableDomainCondition(Prefix + TEXT("_rows_contract_missing"),
						TEXT("The registered keyed-item projection contract for table rows is missing."), FString(), TEXT("/rows_projection_contract_registered"), false, true));
				if (!TryProjectHjsonCanonicalValue(*Rows, Resources, VisibleRows, OutError)
					|| !TryResolveKeyedItemDesiredState(*VisibleRows, *EmptyRows, Document.SourceUnit,
						Contract, CanonicalSchema.Get(), Resources, DesiredRows, bChanged, OutError, nullptr,
						EKeyedItemInputKind::VisibleDesiredState, nullptr, Rows)) return false;
			}
			if (const FHjsonNode* Categories = Root.Get(TEXT("categories")))
			{
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UeAgentAuthoringState::FCaptureResult Capture;
				Composition::HjsonImport::FSemanticWorkspaceImportError ProjectionError;
				if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(&NativeDefaults,
					UeAgentAuthoringState::FEditablePropertyReadOptions(), Requests, Error)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(Error.IsEmpty() ? Capture.Diagnostic : Error, Document.SourceUnit);
					return false;
				}
				if (!Composition::HjsonImport::TryProjectInitialRootObjectProperties(Capture.Snapshot,
					NativeDefaults.GetPathName(), *Categories, Resources, Properties, ProjectionError))
				{
					OutError.SetFailure(ProjectionError, Document.SourceUnit);
					return false;
				}
			}
			if (!CanonicalJson::TryToJsonValue(*DesiredRows, RowsJson, Error))
			{
				OutError.SetFailure(Error, Document.SourceUnit);
				return false;
			}
			return true;
		}

		bool CreateDataTable(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			const FString Path = ObjectPathFromPackageIdentity(Target);
			if (Document.ExpectedRevision != TEXT("missing") && Document.ExpectedRevision != TEXT("absent"))
			{
				auto Error = TableDomainCondition(TEXT("data_table_semantic_workspace_required"),
					TEXT("Updating an existing DataTable requires an exported semantic workspace."), Target, TEXT("/semantic_workspace_present"), false, true);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!Document.AstRoot)
				return Fail(OutError, TableDomainCondition(TEXT("data_table_document_missing"), TEXT("The resolved DataTable document has no HJSON root."), Target, TEXT("/document_root_present"), false, true));
			if (!IsDataTableTargetAbsent(Path))
				return Fail(OutError, TableDomainCondition(TEXT("data_table_target_exists"), TEXT("The create target already exists in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
			const FHjsonNode& Root = *Document.AstRoot;
			for (const auto& Field : Root.GetEntries())
				if (!Field.Key.Equals(TEXT("type"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("row_struct_ref"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("rows"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("categories"), ESearchCase::CaseSensitive))
					return Fail(OutError, HjsonFieldFailure(TEXT("data_table_initial_field_unsupported"), TEXT("This field is not supported when creating a DataTable; allowed fields are type, row_struct_ref, rows and categories."), TEXT("authoring_apply"), Root, Field.Key));
			const FHjsonNode* StructNode = Root.Get(TEXT("row_struct_ref"));
			if (!StructNode || StructNode->GetKind() != EHjsonNodeKind::String)
				return Fail(OutError, HjsonFieldFailure(TEXT("data_table_row_struct_required"), TEXT("DataTable creation requires a string row_struct_ref."), TEXT("authoring_apply"), Root, TEXT("row_struct_ref"), MakeShared<FJsonValueString>(TEXT("UScriptStruct object path"))));
			UScriptStruct* RowStruct = LoadObject<UScriptStruct>(nullptr, *StructNode->GetText());
			if (!RowStruct || !FDataTableEditorUtils::IsValidTableStruct(RowStruct))
				return Fail(OutError, HjsonNodeFailure(TEXT("datatable_curvetable_row_struct_unsupported"), RowStruct ? TEXT("The requested UScriptStruct is not a valid DataTable row structure.") : TEXT("row_struct_ref could not be loaded as a UScriptStruct."), TEXT("authoring_apply"), *StructNode, MakeShared<FJsonValueString>(TEXT("loadable valid DataTable row structure"))));
			TStrongObjectPtr<UDataTable> NativeDefaults(NewObject<UDataTable>(GetTransientPackage()));
			NativeDefaults->RowStruct = RowStruct;
			UE::UAI::Contract::FDiagnostic Error;
			TSharedPtr<FJsonObject> Schema;
			if (!RegisterDataTableAuthoringState(Error)
				|| !BuildDataTableRowSchema(*NativeDefaults, Schema, Error))
				return Fail(OutError, Error.IsValid() ? Error : TableDomainCondition(TEXT("data_table_initial_schema_failed"), TEXT("The DataTable authoring state or row schema could not be registered or built."), Target, TEXT("/row_schema_available"), false, true));
			TSharedPtr<FJsonValue> RowsJson;
			TArray<TSharedPtr<FJsonValue>> Properties;
			if (!ProjectInitialTableFields(*NativeDefaults, Schema, Document, TEXT("data_table"), RowsJson, Properties, OutError)) return false;
			const auto Desired = MakeShared<FJsonObject>();
			Desired->SetStringField(TEXT("row_struct_ref"), RowStruct->GetPathName());
			Desired->SetField(TEXT("rows"), RowsJson);
			NativeDefaults.Reset();
			FRootPropertyApplyOutcome Outcome;
			Outcome.FinalReadback = [&Path, &OutPayload, &OutError](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Path);
				if (Reloaded && BuildWriteResponse(Path, DataTableType, *Reloaded, OutPayload, OutError)) return true;
				Failure.Diagnostic = Reloaded && !OutError.GetDiagnostic().IsEmpty() ? OutError.GetDiagnostic()
					: TableDomainCondition(TEXT("data_table_initial_readback_failed"), Reloaded ? TEXT("The reloaded DataTable produced no complete write response.") : TEXT("The newly created DataTable could not be reloaded."), Path, Reloaded ? TEXT("/write_response_complete") : TEXT("/readback_object_present"), false, true);
				return false;
			};
			if (!ApplyDataTableAuthoring(Path, TEXT("data-table-create-") + FGuid::NewGuid().ToString(),
				Desired, Outcome, nullptr, Properties, true))
				return Fail(OutError, Outcome.Diagnostic);
			return true;
		}
		bool ImportDataTableWorkspace(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			UDataTable* Table = LoadObject<UDataTable>(nullptr, *Target);
			UE::UAI::Contract::FDiagnostic Error;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Table) return Fail(OutError, TableDomainCondition(TEXT("data_table_target_missing"), TEXT("The import target could not be loaded as a DataTable."), Target, TEXT("/target_object_present"), false, true));
			if (!RegisterDataTableAuthoringState(Error)
				|| !BuildDataTableAuthoringReadRequests(*Table, Target, Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(Error.IsEmpty() ? Capture.Diagnostic : Error, Target);
					return false;
				}
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.data_table.asset"), TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, OutError)) return false;
			Capture.Snapshot.ReleaseNativeValues();
			Table = nullptr;
			return TryCoordinateSemanticWorkspaceImport(Document.SemanticWorkspace.ToSharedRef(),
				Model.ToSharedRef(), Snapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(TEXT("data_table.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
				Target, Resources,
				{TEXT("datatable-curvetable-domain-provider"), Target, Target, TEXT("data-table-semantic"),
				 TEXT("data_table_prepare_failed"), TEXT("data_table_apply_failed"), TEXT("data_table_readback_invalid"), TEXT("data_table_schema_invalid"),
				 [Target](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error) {
					 UDataTable* Native = LoadObject<UDataTable>(nullptr, *Target);
					 if (!Native) { Error = TableDomainCondition(TEXT("data_table_capture_target_missing"), TEXT("The DataTable could not be reloaded while capturing the import state."), Target, TEXT("/capture_target_present"), false, true); return false; }
					 return BuildDataTableAuthoringReadRequests(*Native, Target, Requests, Error);
				 }},
				[&Target](const FString& RequestId, const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection, FRootPropertyApplyOutcome& Outcome)
				{
					if (!Projection.StructuralEdits.IsEmpty())
					{
						Outcome.Diagnostic = UnsupportedTableStructuralEdits(Projection.StructuralEdits, TEXT("data_table"), Target);
						return false;
					}
					// ChangedUnitIds also contains related units sharing the asset identity. The actual draft defines writes.
					bool bRowsChanged = false;
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
					{
						const FString& SemanticId = Change.GetIdentity().GetSemanticId();
						if (SemanticId == TEXT("data_table.rows")) bRowsChanged = true;
						else if (SemanticId != TEXT("core.property")
							|| Change.GetIdentity().GetSelector(TEXT("owner_path")) != Target
							|| Change.GetIdentity().GetSelector(TEXT("property_path")).IsEmpty())
						{
							Outcome.Diagnostic = UnsupportedTableChange(TEXT("data_table"), Target, SemanticId,
								Change.GetIdentity().GetSelector(TEXT("owner_path")), Change.GetIdentity().GetSelector(TEXT("property_path")));
							return false;
						}
					}
					// PropertyChangesByUnitId is another projection of these same root changes;
					// the shared root projection above has already validated their intent and owner.
					if (!bRowsChanged)
						return ApplyDataTableAuthoring(Target, RequestId, nullptr, Outcome, nullptr,
							Projection.RootObjectProperties);
					for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
					{
						if (Unit.GetSemanticTypeRef() != TEXT("data_table.rows")) continue;
						const auto Desired = MakeShared<FJsonObject>();
						for (const auto& Field : Unit.GetValues())
						{
							TSharedPtr<FJsonValue> Value;
							UE::UAI::Contract::FDiagnostic Error;
							if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error))
							{
								Outcome.Diagnostic = Error;
								return false;
							}
							Desired->SetField(Field.Key, Value);
						}
						return ApplyDataTableAuthoring(Target, RequestId, Desired, Outcome, nullptr,
							Projection.RootObjectProperties);
					}
					Outcome.Diagnostic = TableDomainCondition(TEXT("data_table_rows_missing"), TEXT("The change draft modifies table rows, but the desired model has no corresponding rows unit."), Target, TEXT("/desired_rows_unit_present"), false, true);
					Outcome.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("data_table.rows"));
					return false;
				},
				[&Target](FString& Payload, FString& Revision, FWorkspaceError& ReadError)
				{
					UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Target);
					TSharedPtr<FJsonObject> State;
					if (!Reloaded) return Fail(ReadError, TableDomainCondition(TEXT("data_table_readback_target_missing"), TEXT("The DataTable could not be reloaded for final import readback."), Target, TEXT("/readback_target_present"), false, true));
					if (!BuildDataTableState(*Reloaded, State, ReadError)) return false;
					Revision = PackageRevision(Target, ReadError);
					return !Revision.IsEmpty() && CanonicalObject(State, Payload, ReadError);
				}, OutPayload, OutError);
		}

		bool ImportCurveTableWorkspace(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			UCurveTable* Table = LoadObject<UCurveTable>(nullptr, *Target);
			UE::UAI::Contract::FDiagnostic Error;
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!Table) return Fail(OutError, TableDomainCondition(TEXT("curve_table_target_missing"), TEXT("The import target could not be loaded as a CurveTable."), Target, TEXT("/target_object_present"), false, true));
			if (!RegisterCurveTableAuthoringState(Error)
				|| !BuildCurveTableAuthoringReadRequests(*Table, Target, Requests, Error)
				|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(Error.IsEmpty() ? Capture.Diagnostic : Error, Target);
					return false;
				}
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FUeAgentCanonicalSnapshotPtr Snapshot;
			FAuthoringModelPtr Model;
			if (!TryBuildCanonicalAuthoringCapture(TEXT("ue.curve_table.asset"), TEXT("llm_authoring"),
				Capture.Snapshot, Resources, Snapshot, Model, OutError)) return false;
			Capture.Snapshot.ReleaseNativeValues();
			Table = nullptr;
			return TryCoordinateSemanticWorkspaceImport(Document.SemanticWorkspace.ToSharedRef(),
				Model.ToSharedRef(), Snapshot.ToSharedRef(),
				Contract::FUeAgentDescriptorIdentity(TEXT("curve_table.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
				Target, Resources,
				{TEXT("datatable-curvetable-domain-provider"), Target, Target, TEXT("curve-table-semantic"),
				 TEXT("curve_table_prepare_failed"), TEXT("curve_table_apply_failed"), TEXT("curve_table_readback_invalid"), TEXT("curve_table_schema_invalid"),
				 [Target](TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error) {
					 UCurveTable* Native = LoadObject<UCurveTable>(nullptr, *Target);
					 if (!Native) { Error = TableDomainCondition(TEXT("curve_table_capture_target_missing"), TEXT("The CurveTable could not be reloaded while capturing the import state."), Target, TEXT("/capture_target_present"), false, true); return false; }
					 return BuildCurveTableAuthoringReadRequests(*Native, Target, Requests, Error);
				 }},
				[&Target](const FString& RequestId, const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
					Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection, FRootPropertyApplyOutcome& Outcome)
				{
					if (!Projection.StructuralEdits.IsEmpty())
					{
						Outcome.Diagnostic = UnsupportedTableStructuralEdits(Projection.StructuralEdits, TEXT("curve_table"), Target);
						return false;
					}
					// ChangedUnitIds also contains related units sharing the asset identity. The actual draft defines writes.
					bool bRowsChanged = false;
					for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
					{
						const FString& SemanticId = Change.GetIdentity().GetSemanticId();
						if (SemanticId == TEXT("curve_table.rows")) bRowsChanged = true;
						else if (SemanticId != TEXT("core.property")
							|| Change.GetIdentity().GetSelector(TEXT("owner_path")) != Target
							|| Change.GetIdentity().GetSelector(TEXT("property_path")).IsEmpty())
						{
							Outcome.Diagnostic = UnsupportedTableChange(TEXT("curve_table"), Target, SemanticId,
								Change.GetIdentity().GetSelector(TEXT("owner_path")), Change.GetIdentity().GetSelector(TEXT("property_path")));
							return false;
						}
					}
					// PropertyChangesByUnitId is another projection of these same root changes;
					// the shared root projection above has already validated their intent and owner.
					if (!bRowsChanged)
						return ApplyCurveTableAuthoring(Target, RequestId, nullptr, Outcome, nullptr,
							Projection.RootObjectProperties);
					for (const auto& Unit : Prepared.GetWorkspace()->GetModel()->GetUnits())
					{
						if (Unit.GetSemanticTypeRef() != TEXT("curve_table.rows")) continue;
						const auto Desired = MakeShared<FJsonObject>();
						for (const auto& Field : Unit.GetValues())
						{
							TSharedPtr<FJsonValue> Value;
							UE::UAI::Contract::FDiagnostic Error;
							if (!CanonicalJson::TryToJsonValue(Field.Value.Get(), Value, Error))
							{
								Outcome.Diagnostic = Error;
								return false;
							}
							Desired->SetField(Field.Key, Value);
						}
						return ApplyCurveTableAuthoring(Target, RequestId, Desired, Outcome, nullptr,
							Projection.RootObjectProperties);
					}
					Outcome.Diagnostic = TableDomainCondition(TEXT("curve_table_rows_missing"), TEXT("The change draft modifies table rows, but the desired model has no corresponding rows unit."), Target, TEXT("/desired_rows_unit_present"), false, true);
					Outcome.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("curve_table.rows"));
					return false;
				},
				[&Target](FString& Payload, FString& Revision, FWorkspaceError& ReadError)
				{
					UCurveTable* Reloaded = LoadObject<UCurveTable>(nullptr, *Target);
					TSharedPtr<FJsonObject> State;
					if (!Reloaded) return Fail(ReadError, TableDomainCondition(TEXT("curve_table_readback_target_missing"), TEXT("The CurveTable could not be reloaded for final import readback."), Target, TEXT("/readback_target_present"), false, true));
					if (!BuildCurveTableState(*Reloaded, State, ReadError)) return false;
					Revision = PackageRevision(Target, ReadError);
					return !Revision.IsEmpty() && CanonicalObject(State, Payload, ReadError);
				}, OutPayload, OutError);
		}

		bool CreateCurveTable(const FString& Target,
			const DomainAuthoring::FResolvedDomainHjsonDocument& Document,
			FString& OutPayload, FWorkspaceError& OutError)
		{
			using namespace DomainAuthoring;
			const FString Path = ObjectPathFromPackageIdentity(Target);
			if (Document.ExpectedRevision != TEXT("missing") && Document.ExpectedRevision != TEXT("absent"))
			{
				auto Error = TableDomainCondition(TEXT("curve_table_semantic_workspace_required"),
					TEXT("Updating an existing CurveTable requires an exported semantic workspace."), Target, TEXT("/semantic_workspace_present"), false, true);
				Error.ActualEvidence = MakeShared<FJsonValueString>(Document.ExpectedRevision);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!Document.AstRoot)
				return Fail(OutError, TableDomainCondition(TEXT("curve_table_document_missing"), TEXT("The resolved CurveTable document has no HJSON root."), Target, TEXT("/document_root_present"), false, true));
			if (!IsCurveTableTargetAbsent(Path))
				return Fail(OutError, TableDomainCondition(TEXT("curve_table_target_exists"), TEXT("The create target already exists in memory, on disk or in the asset registry."), Path, TEXT("/target_absent"), false, true));
			const FHjsonNode& Root = *Document.AstRoot;
			for (const auto& Field : Root.GetEntries())
				if (!Field.Key.Equals(TEXT("type"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("table_curve_mode"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("rows"), ESearchCase::CaseSensitive)
					&& !Field.Key.Equals(TEXT("categories"), ESearchCase::CaseSensitive))
					return Fail(OutError, HjsonFieldFailure(TEXT("curve_table_initial_field_unsupported"), TEXT("This field is not supported when creating a CurveTable; allowed fields are type, table_curve_mode, rows and categories."), TEXT("authoring_apply"), Root, Field.Key));
			FString Mode = TEXT("empty");
			if (const FHjsonNode* ModeNode = Root.Get(TEXT("table_curve_mode")))
			{
				if (ModeNode->GetKind() != EHjsonNodeKind::String)
					return Fail(OutError, HjsonNodeFailure(TEXT("curve_table_mode_invalid"), TEXT("table_curve_mode must be a string."), TEXT("authoring_apply"), *ModeNode, MakeShared<FJsonValueString>(TEXT("empty, simple or rich"))));
				Mode = ModeNode->GetText();
			}
			if (Mode != TEXT("empty") && Mode != TEXT("simple") && Mode != TEXT("rich"))
				return Fail(OutError, HjsonFieldFailure(TEXT("curve_table_mode_invalid"), TEXT("The CurveTable mode is not supported."), TEXT("authoring_apply"), Root, TEXT("table_curve_mode"), MakeShared<FJsonValueString>(TEXT("empty, simple or rich"))));
			TStrongObjectPtr<UCurveTable> NativeDefaults(NewObject<UCurveTable>(GetTransientPackage()));
			UE::UAI::Contract::FDiagnostic Error;
			if (!RegisterCurveTableAuthoringState(Error)) return Fail(OutError, Error.IsValid() ? Error : TableDomainCondition(TEXT("curve_table_initial_schema_failed"), TEXT("The CurveTable authoring state could not be registered."), Target, TEXT("/authoring_state_registered"), false, true));
			const auto Schema = Mode == TEXT("empty") ? UeAgentCurveJson::BuildEmptyCurveTableRowSchema()
				: UeAgentCurveJson::BuildCurveTableRowSchema(Mode == TEXT("simple"));
			TSharedPtr<FJsonValue> RowsJson;
			TArray<TSharedPtr<FJsonValue>> Properties;
			if (!ProjectInitialTableFields(*NativeDefaults, Schema, Document, TEXT("curve_table"), RowsJson, Properties, OutError)) return false;
			const auto Desired = MakeShared<FJsonObject>();
			Desired->SetStringField(TEXT("table_curve_mode"), Mode);
			Desired->SetField(TEXT("rows"), RowsJson);
			NativeDefaults.Reset();
			FRootPropertyApplyOutcome Outcome;
			Outcome.FinalReadback = [&Path, &OutPayload, &OutError](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				UCurveTable* Reloaded = LoadObject<UCurveTable>(nullptr, *Path);
				if (Reloaded && BuildWriteResponse(Path, CurveTableType, *Reloaded, OutPayload, OutError)) return true;
				Failure.Diagnostic = Reloaded && !OutError.GetDiagnostic().IsEmpty() ? OutError.GetDiagnostic()
					: TableDomainCondition(TEXT("curve_table_initial_readback_failed"), Reloaded ? TEXT("The reloaded CurveTable produced no complete write response.") : TEXT("The newly created CurveTable could not be reloaded."), Path, Reloaded ? TEXT("/write_response_complete") : TEXT("/readback_object_present"), false, true);
				return false;
			};
			if (!ApplyCurveTableAuthoring(Path, TEXT("curve-table-create-") + FGuid::NewGuid().ToString(),
				Desired, Outcome, nullptr, Properties, true))
				return Fail(OutError, Outcome.Diagnostic);
			return true;
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
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"), RootType);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetBoolField(TEXT("reloaded"), true);
			Response->SetObjectField(TEXT("independent_readback"), State);
			return CanonicalObject(Response, OutPayload, OutError);
		}

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
				return Fail(OutError, TableCommandFact(TEXT("datatable_curvetable_export_path_required"), TEXT("workspace_path must be a non-empty string for table export."), Target, TEXT("/params/workspace_path"), Params->Values.FindRef(TEXT("workspace_path")), MakeShared<FJsonValueString>(TEXT("non-empty workspace path"))));
			}
			UObject* Asset = LoadTarget(Target, RootType, OutError);
			if (!Asset) return false;
			if (UDataTable* Table = Cast<UDataTable>(Asset))
			{
				UE::UAI::Contract::FDiagnostic Error;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!RegisterDataTableAuthoringState(Error)
					|| !BuildDataTableAuthoringReadRequests(*Table, Target, Requests, Error)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
					{
						OutError.SetFailure(Error.IsEmpty() ? Capture.Diagnostic : Error, Target);
						return false;
					}
				FString Profile = TEXT("llm_authoring");
				Params->TryGetStringField(TEXT("profile_id"), Profile);
				UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
				if (!TryExportCompositionWorkspace(TEXT("ue.data_table.asset"), Profile, Capture.Snapshot, WorkspacePath, Result))
				{
					OutError.SetFailure(Result.Diagnostic, Target);
					return false;
				}
				return CanonicalObject(Result.ToPublicJsonObject(), OutPayload, OutError);
			}
			if (UCurveTable* Table = Cast<UCurveTable>(Asset))
			{
				UE::UAI::Contract::FDiagnostic Error;
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UeAgentAuthoringState::FCaptureResult Capture;
				if (!RegisterCurveTableAuthoringState(Error)
					|| !BuildCurveTableAuthoringReadRequests(*Table, Target, Requests, Error)
					|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
					{
						OutError.SetFailure(Error.IsEmpty() ? Capture.Diagnostic : Error, Target);
						return false;
					}
				FString Profile = TEXT("llm_authoring");
				Params->TryGetStringField(TEXT("profile_id"), Profile);
				UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
				if (!TryExportCompositionWorkspace(TEXT("ue.curve_table.asset"), Profile, Capture.Snapshot, WorkspacePath, Result))
				{
					OutError.SetFailure(Result.Diagnostic, Target);
					return false;
				}
				return CanonicalObject(Result.ToPublicJsonObject(), OutPayload, OutError);
			}
			return Fail(OutError, TableDomainFact(TEXT("datatable_curvetable_target_type_invalid"), TEXT("The export target is neither a DataTable nor a CurveTable."), Target, TEXT("/root_class"), MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("DataTable or CurveTable"))));
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError, JsonError.IsValid() ? JsonError : TableCommandFact(TEXT("datatable_curvetable_params_invalid"), TEXT("The table command parameters did not decode to a JSON object."), TargetIdentity, TEXT("/params"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, TableCommandFact(TEXT("datatable_curvetable_target_identity_mismatch"), TEXT("The command target must be a string matching the routed target."), TargetIdentity, TEXT("/params/target"), Params->Values.FindRef(TEXT("target")), MakeShared<FJsonValueString>(TargetIdentity)));
		}
		if (RootTypeIdentity != DataTableType
			&& RootTypeIdentity != CurveTableType)
		{
			return Fail(OutError, TableCommandFact(TEXT("datatable_curvetable_root_type_unsupported"), TEXT("The routed table root type is unsupported."), TargetIdentity, TEXT("/root_type"), MakeShared<FJsonValueString>(RootTypeIdentity), MakeShared<FJsonValueString>(TEXT("/Script/Engine.DataTable or /Script/Engine.CurveTable"))));
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			if (RootTypeIdentity == DataTableType)
			{
				DomainAuthoring::FResolvedDomainHjsonDocument Document;
				if (!DomainAuthoring::TryDecodeResolvedDocument(Target, CanonicalParamsJson,
					{TEXT("data_table"), SchemaId, DataTableType}, Document, OutError,
					DomainAuthoring::EDomainHjsonDocumentProjection::Ast)) return false;
				DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
				if (Document.SemanticWorkspace.IsValid())
					return ImportDataTableWorkspace(Target, Document, OutPayloadJson, OutError);
				return CreateDataTable(Target, Document, OutPayloadJson, OutError);
			}
			DomainAuthoring::FResolvedDomainHjsonDocument Document;
			if (!DomainAuthoring::TryDecodeResolvedDocument(Target, CanonicalParamsJson,
				{TEXT("curve_table"), SchemaId, CurveTableType}, Document, OutError,
				DomainAuthoring::EDomainHjsonDocumentProjection::Ast)) return false;
			DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.SemanticWorkspace.IsValid())
				return ImportCurveTableWorkspace(Target, Document, OutPayloadJson, OutError);
			return CreateCurveTable(Target, Document, OutPayloadJson, OutError);
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
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), State);
			bOutReadOnly = true;
			return CanonicalObject(Response, OutPayloadJson, OutError);
		}
		return Fail(OutError, TableCommandFact(TEXT("datatable_curvetable_command_not_supported"), TEXT("This table adapter requires a content_graph command."), TargetIdentity, TEXT("/command_id"), MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.*"))));
	}
}
