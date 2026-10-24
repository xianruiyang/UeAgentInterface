#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Dom/JsonObject.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableCurveTableDomainMechanics.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringState.h"
#include "DomainAdapters/DataTableCurveTable/UeAgentDataTableAuthoringApply.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestTypes.h"
#include "Vertical/T208/UeAgentT208DataTableTestTypes.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace
{
	using namespace UE::UAI;

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& RequestId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Command = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		FString ParamsJson;
		FString JsonError;
		if (!Command || !Dispatch.IsValid()
			|| !CommandContract::Json::TryCanonicalize(
				Params, ParamsJson, JsonError))
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-datatable"),
				RequestId,
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("datatable_public_command_unavailable"),
				TEXT("the public command or request is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t208-datatable\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*Command->GetContractHash(),
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t208-datatable"),
				RequestId,
				CommandId,
				Command->GetContractHash(),
				TEXT("datatable_public_request_invalid"),
				TEXT("the public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t208-datatable"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t208-datatable")),
			MakeShared<FNeverCancelled>());
	}

	bool EditExportedRows(const FString& Workspace, FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *Workspace, TEXT("*.hjson"), true, false);
		Files.Sort();
		bool bRowsEdited = false;
		bool bRootEdited = false;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/"))
				|| File.Contains(TEXT("\\.uai\\")))
			{
				continue;
			}
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *File)) continue;
			if (Text.Contains(TEXT("t208-root-before")))
			{
				bRootEdited = Text.ReplaceInline(TEXT("t208-root-before"), TEXT("t208-root-after")) == 1
					&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			if (Text.Contains(TEXT("Retained")) && Text.Contains(TEXT("Removed"))
				&& Text.ReplaceInline(TEXT("Removed"), TEXT("Added")) == 1
				&& Text.ReplaceInline(TEXT("37"), TEXT("73")) == 1
				&& Text.ReplaceInline(TEXT("19"), TEXT("29")) == 1
				&& FFileHelper::SaveStringToFile(
					Text,
					*File,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				bRowsEdited = true;
			}
		}
		if (bRowsEdited && bRootEdited) return true;
		OutError = TEXT("datatable_exported_row_key_missing");
		return false;
	}

	bool ReadRows(
		const CommandEdge::FUeAgentCommandResult& Result,
		TArray<TSharedPtr<FJsonValue>>& OutRows)
	{
		OutRows.Reset();
		TSharedPtr<FJsonObject> Payload;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result.IsOk() || !FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(Result.GetPayloadJson()), Payload)
			|| !Payload.IsValid() || !Payload->TryGetArrayField(TEXT("entries"), Rows)) return false;
		for (const auto& Value : *Rows)
		{
			if (Value.IsValid() && Value->Type == EJson::Object
				&& Value->AsObject()->HasField(TEXT("name"))
				&& Value->AsObject()->HasField(TEXT("properties"))) OutRows.Add(Value);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208DataTableTypedCaptureTest,
	"UAI.Root.T208.DataTable.TypedRowCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTableTypedCaptureTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
	FString Error;
	if (!TestTrue(TEXT("DataTable state registration"), RegisterDataTableAuthoringState(Error)))
	{
		AddError(Error);
		return false;
	}
	UDataTable* Table = NewObject<UDataTable>(GetTransientPackage());
	Table->RowStruct = FUeAgentAssetDataSimpleEffectTableRow::StaticStruct();
	FUeAgentAssetDataSimpleEffectTableRow Row;
	Row.Count = 37;
	Row.Label = TEXT("Typed row");
	Table->AddRow(TEXT("First"), Row);
	Row.Count = 19;
	Table->AddRow(TEXT("Second"), Row);
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	if (!TestTrue(TEXT("row requests use the shared struct reader"),
		BuildDataTableAuthoringReadRequests(*Table, TEXT("/Game/UAI/Transient/TypedCapture"), Requests, Error)))
	{
		AddError(Error);
		return false;
	}
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!TestTrue(TEXT("typed state capture"), UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)))
	{
		AddError(Capture.Diagnostic.Code + TEXT(":") + Capture.Diagnostic.InternalContext);
		return false;
	}
	int32 CountFields = 0;
	for (const UeAgentAuthoringState::FStateEntry& Entry : Capture.Snapshot.CopyEntries())
	{
		if (Entry.SemanticId != TEXT("data_table.rows")) continue;
		const TSharedPtr<FJsonObject> Rows = Entry.Value->AsObject()->GetObjectField(TEXT("rows"));
		for (const auto& Pair : Rows->Values)
		{
			const TSharedPtr<FJsonObject> Count = Pair.Value->AsObject()->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Count"));
			++CountFields;
			TestFalse(TEXT("count has the shared readable type"), Count->GetStringField(TEXT("type")).IsEmpty());
			TestEqual(TEXT("row identity preserves the right reflected field value"),
				Count->GetNumberField(TEXT("value")), Pair.Key == TEXT("First") ? 37.0 : 19.0);
		}
	}
	return TestEqual(TEXT("each row has its own Count field identity"), CountFields, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208DataTableCommonSourceRejectionTest,
	"UAI.Root.T208.DataTable.CommonSourceRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTableCommonSourceRejectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Target = TEXT("/Game/UAI/Transient/T208DataTable/DT_Source.DT_Source");
	const TSharedRef<FJsonObject> Inline = MakeShared<FJsonObject>();
	Inline->SetStringField(TEXT("kind"), TEXT("inline"));
	Inline->SetStringField(
		TEXT("document"),
		TEXT("{schema:'uai.datatable_curvetable.edit/1',root:{")
		TEXT("type:'/Script/Engine.DataTable',row_struct_ref:")
		TEXT("'/Script/Engine.TableRowBase',rows:[]}}"));
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	HjsonWorkspace::FWorkspaceError Error;
	if (!TestTrue(
			TEXT("public inline source resolves before internal dispatch"),
			HjsonWorkspace::TryLoadContentGraphImportSource(
				Inline, Target, Resources, Resolved, Error))
		|| !Resolved.IsValid())
	{
		return false;
	}
	const TSharedRef<FJsonObject> Tampered =
		Resolved->MakeNormalizedSourceObject();
	const TArray<TSharedPtr<FJsonValue>>& Documents =
		Tampered->GetArrayField(TEXT("documents"));
	Documents[0]->AsObject()->SetStringField(
		TEXT("content_hash"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), Target);
	Params->SetStringField(TEXT("expected_revision"), TEXT("missing"));
	Params->SetObjectField(TEXT("source"), Tampered);
	FString ParamsJson;
	FString JsonError;
	if (!TestTrue(
			TEXT("tampered normalized request canonicalizes"),
			CommandContract::Json::TryCanonicalize(
				Params, ParamsJson, JsonError)))
	{
		AddError(JsonError);
		return false;
	}
	FString Payload;
	bool bReadOnly = true;
	TestFalse(
		TEXT("DataTable adapter delegates normalized source integrity to common owner"),
		DomainAdapters::DataTableCurveTable::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			Target,
			TEXT("/Script/Engine.DataTable"),
			ParamsJson,
			Payload,
			bReadOnly,
			Error));
	return TestEqual(
		TEXT("common source owner supplies the rejection code"),
		Error.GetCode(),
		FString(TEXT("content_graph_normalized_document_digest_mismatch")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208DataTableOwnedLifecycleTest,
	"UAI.Root.T208.DataTable.OwnedRowLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTableOwnedLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
	using namespace UeAgentAuthoringOperationApply;
	const FString PackageName = TEXT("/Game/UAI/Transient/T208DataTable/DT_Owned_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
	const FString Path = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		if (UPackage* Remaining = FindPackage(nullptr, *PackageName))
		{
			Remaining->SetDirtyFlag(false);
			FText Error;
			UPackageTools::UnloadPackages({Remaining}, Error, true);
		}
		IFileManager::Get().Delete(*Filename, false, true);
	};
	FString Error;
	if (!RegisterDataTableAuthoringState(Error)) { AddError(Error); return false; }
	UPackage* Package = CreatePackage(*PackageName);
	UDataTable* Table = NewObject<UDataTable>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	Table->RowStruct = FUeAgentT208DataTableOwnedRow::StaticStruct();
	Table->ImportKeyField = TEXT("disk-key");
	FUeAgentT208DataTableOwnedRow Initial;
	Initial.Count = 10;
	Initial.ReadOnly = 99;
	Initial.Detail = NewObject<UUeAgentT208DataTableOwnedValue>(Table, TEXT("OriginalDetail"), RF_Transactional);
	Initial.Detail->Amount = 10;
	Table->AddRow(TEXT("Keep"), Initial);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("owned fixture baseline saves"), UPackage::SavePackage(Package, Table, *Filename, SaveArgs))) return false;
	TArray<uint8> OriginalDisk;
	FFileHelper::LoadFileToArray(OriginalDisk, *Filename);
	auto* Live = Table->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"));
	Live->Count = 21;
	Live->Detail->Amount = 11;
	Table->ImportKeyField = TEXT("dirty-key");
	Table->MarkPackageDirty();
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!BuildDataTableAuthoringReadRequests(*Table, Path, Requests, Error)
		|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
	{
		AddError(Error + Capture.Diagnostic.Code + Capture.Diagnostic.InternalContext);
		return false;
	}
	TSharedPtr<FJsonObject> Desired;
	for (const auto& Entry : Capture.Snapshot.CopyEntries())
		if (Entry.SemanticId == TEXT("data_table.rows")) Desired = Entry.Value->AsObject();
	if (!TestTrue(TEXT("owned row typed capture exists"), Desired.IsValid())) return false;
	Capture.Snapshot.ReleaseNativeValues();
	Requests.Reset();
	const auto Properties = Desired->GetObjectField(TEXT("rows"))->GetObjectField(TEXT("Keep"))->GetObjectField(TEXT("properties"));
	Properties->GetObjectField(TEXT("Count"))->SetNumberField(TEXT("value"), 73);
	const auto Owned = Properties->GetObjectField(TEXT("Detail"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("owned_object"));
	for (const auto& Property : Owned->GetArrayField(TEXT("properties")))
		if (Property->AsObject()->GetStringField(TEXT("property_path")) == TEXT("Amount"))
			Property->AsObject()->SetNumberField(TEXT("value"), 42);
	const auto Added = MakeShared<FJsonObject>();
	Added->SetObjectField(TEXT("properties"), Properties);
	Desired->GetObjectField(TEXT("rows"))->SetObjectField(TEXT("Added"), Added);
	FString DesiredJson;
	CommandContract::Json::TryCanonicalize(Desired, DesiredJson, Error);
	TSharedPtr<FJsonObject> Invalid;
	CommandContract::Json::TryParseObject(DesiredJson, Invalid, Error);
	const auto BadProperties = MakeShared<FJsonObject>();
	const auto BadCount = MakeShared<FJsonObject>();
	BadCount->SetStringField(TEXT("value"), TEXT("not-an-integer"));
	BadProperties->SetObjectField(TEXT("Count"), BadCount);
	const auto BadRow = MakeShared<FJsonObject>();
	BadRow->SetObjectField(TEXT("properties"), BadProperties);
	Invalid->GetObjectField(TEXT("rows"))->SetObjectField(TEXT("Rejected"), BadRow);
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Rejected;
	const auto RootProperty = MakeShared<FJsonObject>();
	RootProperty->SetStringField(TEXT("property_path"), TEXT("ImportKeyField"));
	RootProperty->SetStringField(TEXT("value"), TEXT("applied-key"));
	const TArray<TSharedPtr<FJsonValue>> RootProperties{MakeShared<FJsonValueObject>(RootProperty)};
	TestFalse(TEXT("later invalid row rejects preparation after legal owned edits in scratch"),
		ApplyDataTableAuthoring(Path, TEXT("t208-owned-prepare"), Invalid, Rejected, nullptr, RootProperties));
	TestEqual(TEXT("preflight leaves dirty root property unchanged"), Table->ImportKeyField, FString(TEXT("dirty-key")));
	TestEqual(TEXT("preflight leaves original owned value unchanged"), Live->Detail->Amount, 11);
	TestEqual(TEXT("preflight leaves original row value unchanged"), Live->Count, 21);
	TestTrue(TEXT("preflight retains real outer and dirty memory"), Live->Detail->GetOuter() == Table && Package->IsDirty());

	class FRejectChangedStage final : public IDurablePackageStager
	{
	public:
		explicit FRejectChangedStage(FString InPath) : Path(MoveTemp(InPath)), Inner(CreateDefaultDurablePackageStager()) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			UDataTable* Actual = LoadObject<UDataTable>(nullptr, *Path);
			const auto* Row = Actual ? Actual->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208")) : nullptr;
			const auto* NewRow = Actual ? Actual->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Added"), TEXT("T208"), false) : nullptr;
			if (Row && Row->Count == 73 && Row->Detail && Row->Detail->Amount == 42 && NewRow && NewRow->Detail
				&& Actual->ImportKeyField == TEXT("applied-key"))
			{
				bObservedRealWrite = Row->Detail->GetOuter() == Actual && NewRow->Detail->GetOuter() == Actual;
				Failure.Diagnostic.Code = TEXT("t208_data_table_stage_rejected_after_write");
				Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
				return false;
			}
			return Inner->StagePackages(Plans, Artifacts, Failure);
		}
		bool bObservedRealWrite = false;
	private:
		FString Path;
		TSharedRef<IDurablePackageStager> Inner;
	};
	const auto Stager = MakeShared<FRejectChangedStage>(Path);
	FBackendOptions Options;
	Options.PackageStager = Stager;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	Early.FinalReadback = [](FOperationFailure&) { return true; };
	TestFalse(TEXT("post-mutation staging failure rolls back"),
		ApplyDataTableAuthoring(Path, TEXT("t208-owned-early"), Desired, Early, &Options, RootProperties));
	if (!TestTrue(TEXT("failure follows real existing and newly owned writes with real outers"), Stager->bObservedRealWrite))
	{
		AddError(Early.Diagnostic.Code + TEXT(":") + Early.Diagnostic.PublicMessage);
		return false;
	}
	Live = Table->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"));
	TestEqual(TEXT("early rollback restores dirty row memory"), Live->Count, 21);
	TestEqual(TEXT("early rollback restores dirty root property"), Table->ImportKeyField, FString(TEXT("dirty-key")));
	TestEqual(TEXT("early rollback restores original owned state"), Live->Detail->Amount, 11);
	TestEqual(TEXT("early rollback preserves read-only row storage"), Live->ReadOnly, 99);
	TestTrue(TEXT("early rollback retains original owner and removes new row"), Live->Detail->GetOuter() == Table
		&& Table->GetRowMap().Num() == 1 && Package->IsDirty());
	TArray<uint8> RestoredDisk;
	FFileHelper::LoadFileToArray(RestoredDisk, *Filename);
	TestTrue(TEXT("early rollback retains independent disk baseline"), OriginalDisk == RestoredDisk);
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Success;
	Success.FinalReadback = [&Path](FOperationFailure& Failure)
	{
		const UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Path);
		if (Reloaded && Reloaded->GetRowMap().Num() == 2 && Reloaded->ImportKeyField == TEXT("applied-key"))
		{
			for (const auto& Pair : Reloaded->GetRowMap())
			{
				const auto* Row = reinterpret_cast<const FUeAgentT208DataTableOwnedRow*>(Pair.Value);
				if (!Row->Detail || Row->Count != 73 || Row->Detail->Amount != 42 || Row->Detail->GetOuter() != Reloaded)
				{
					Failure.Diagnostic.Code = TEXT("t208_owned_readback_mismatch");
					return false;
				}
			}
			return true;
		}
		Failure.Diagnostic.Code = TEXT("t208_owned_readback_missing");
		return false;
	};
	Initial.Detail = nullptr;
	Table = nullptr;
	Package = nullptr;
	Live = nullptr;
	if (!TestTrue(TEXT("owned rows save, unload, reload and verify real outers"),
		ApplyDataTableAuthoring(Path, TEXT("t208-owned-success"), Desired, Success, nullptr, RootProperties)))
	{
		AddError(Success.Diagnostic.Code + TEXT(":") + Success.Diagnostic.PublicMessage);
		return false;
	}
	// Observe the actual in-memory row allocations before persistent unload: a property-only
	// edit must not replace rows or their instanced objects.
	class FObserveUnchangedRowsStage final : public IDurablePackageStager
	{
	public:
		explicit FObserveUnchangedRowsStage(UDataTable& Table)
			: Path(Table.GetPathName()), Row(Table.FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"))),
			  Detail(Row->Detail), Inner(CreateDefaultDurablePackageStager()) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			const UDataTable* Actual = LoadObject<UDataTable>(nullptr, *Path);
			if (Actual && Actual->ImportKeyField == TEXT("property-only-key"))
			{
				const auto* Current = Actual->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"));
				bPreserved = Current == Row && Current->Detail == Detail && Current->Count == 73
					&& Current->Detail->Amount == 42 && Actual->GetRowMap().Num() == 2;
				if (!bPreserved) { Failure.Diagnostic.Code = TEXT("t208_property_only_replaced_rows"); return false; }
			}
			return Inner->StagePackages(Plans, Artifacts, Failure);
		}
		bool bPreserved = false;
	private:
		FString Path;
		const FUeAgentT208DataTableOwnedRow* Row;
		const UUeAgentT208DataTableOwnedValue* Detail;
		TSharedRef<IDurablePackageStager> Inner;
	};
	Table = LoadObject<UDataTable>(nullptr, *Path);
	const auto Observer = MakeShared<FObserveUnchangedRowsStage>(*Table);
	FBackendOptions PropertyOnlyOptions;
	PropertyOnlyOptions.PackageStager = Observer;
	RootProperty->SetStringField(TEXT("value"), TEXT("property-only-key"));
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome PropertyOnly;
	PropertyOnly.FinalReadback = [&Path](FOperationFailure& Failure)
	{
		const UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Path);
		if (Reloaded && Reloaded->ImportKeyField == TEXT("property-only-key")) return true;
		Failure.Diagnostic.Code = TEXT("t208_property_only_readback_failed");
		return false;
	};
	Table = nullptr;
	if (!TestTrue(TEXT("property-only edit persists through the shared reflection wrapper"),
		ApplyDataTableAuthoring(Path, TEXT("t208-property-only"), nullptr, PropertyOnly, &PropertyOnlyOptions, RootProperties)))
	{
		AddError(PropertyOnly.Diagnostic.Code + TEXT(":") + PropertyOnly.Diagnostic.PublicMessage);
		return false;
	}
	TestTrue(TEXT("property-only commit keeps real row and owned allocations"), Observer->bPreserved);
	TArray<uint8> SuccessfulDisk;
	FFileHelper::LoadFileToArray(SuccessfulDisk, *Filename);
	Table = LoadObject<UDataTable>(nullptr, *Path);
	Live = Table->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"));
	Live->Count = 74;
	Live->Detail->Amount = 43;
	Table->ImportKeyField = TEXT("late-dirty-key");
	RootProperty->SetStringField(TEXT("value"), TEXT("late-applied-key"));
	Table->MarkPackageDirty();
	for (const auto& Property : Owned->GetArrayField(TEXT("properties")))
		if (Property->AsObject()->GetStringField(TEXT("property_path")) == TEXT("Amount"))
			Property->AsObject()->SetNumberField(TEXT("value"), 54);
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&Path, &bLateObserved](FOperationFailure& Failure)
	{
		const UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Path);
		const auto* Row = Reloaded ? Reloaded->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208")) : nullptr;
		bLateObserved = Row && Row->Detail && Row->Detail->Amount == 54 && Row->Detail->GetOuter() == Reloaded
			&& Reloaded->ImportKeyField == TEXT("late-applied-key");
		Failure.Diagnostic.Code = TEXT("t208_data_table_late_rejected_after_readback");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Table = nullptr;
	Live = nullptr;
	TestFalse(TEXT("late failure restores checkpoint instead of committing"),
		ApplyDataTableAuthoring(Path, TEXT("t208-owned-late"), Desired, Late, nullptr, RootProperties));
	if (!TestTrue(TEXT("late rejection occurs after independent owned readback"), bLateObserved))
	{
		AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage);
		return false;
	}
	Table = LoadObject<UDataTable>(nullptr, *Path);
	Live = Table->FindRow<FUeAgentT208DataTableOwnedRow>(TEXT("Keep"), TEXT("T208"));
	TestEqual(TEXT("late restore recovers dirty row memory"), Live->Count, 74);
	TestEqual(TEXT("late restore recovers dirty root property"), Table->ImportKeyField, FString(TEXT("late-dirty-key")));
	TestEqual(TEXT("late restore recovers dirty owned memory"), Live->Detail->Amount, 43);
	TestTrue(TEXT("late restore has real owner and original dirty flag"), Live->Detail->GetOuter() == Table && Table->GetOutermost()->IsDirty());
	FFileHelper::LoadFileToArray(RestoredDisk, *Filename);
	TestTrue(TEXT("late restore recovers original disk independently from dirty memory"), SuccessfulDisk == RestoredDisk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT208DataTablePublicSourceVerticalTest,
	"UAI.Root.T208.DataTable.PublicDirectorySourceVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTablePublicSourceVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T208DataTable/DT_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir()
		/ TEXT("UeAssetFolders/T208DataTable/edit");
	ON_SCOPE_EXIT
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			Package->SetDirtyFlag(false);
			FText Error;
			UPackageTools::UnloadPackages({Package}, Error, true);
		}
		IFileManager::Get().Delete(*Filename, false, true);
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};

	UPackage* Package = CreatePackage(*PackageName);
	UDataTable* Table = Package
		? NewObject<UDataTable>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("persistent DataTable fixture is created"), Table))
	{
		return false;
	}
	Table->RowStruct = FUeAgentAssetDataSimpleEffectTableRow::StaticStruct();
	FUeAgentAssetDataSimpleEffectTableRow Initial;
	Table->ImportKeyField = TEXT("t208-root-before");
	Initial.Count = 37;
	Table->AddRow(TEXT("Retained"), Initial);
	Initial.Count = 19;
	Table->AddRow(TEXT("Removed"), Initial);
	FAssetRegistryModule::AssetCreated(Table);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(
			TEXT("initial DataTable fixture saves"),
			UPackage::SavePackage(Package, Table, *Filename, SaveArgs)))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the formal process active composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	TSharedRef<FJsonObject> Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), ObjectPath);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const CommandEdge::FUeAgentCommandResult ExportResult = SubmitPublicCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.export_workspace"),
		TEXT("req-t208-datatable-export"),
		Export);
	if (!TestTrue(
			TEXT("public DataTable directory export succeeds"),
			ExportResult.IsOk()))
	{
		AddError(ExportResult.GetErrorJson());
		return false;
	}
	if (!TestTrue(
			TEXT("one readable edit changes an existing field, creates an assigned row and removes a row"),
			EditExportedRows(Workspace, Error)))
	{
		AddError(Error);
		return false;
	}

	TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	TSharedRef<FJsonObject> Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), ObjectPath);
	Import->SetObjectField(TEXT("source"), Source);
	// The shared rows contract declares this CurveTable field, but DataTable has no captured source for it.
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	TArray<FString> EditFiles;
	IFileManager::Get().FindFilesRecursive(EditFiles, *Workspace, TEXT("*.hjson"), true, false);
	FString RowsFile;
	TArray<uint8> RowsFileBefore;
	for (const FString& File : EditFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) continue;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError))
		{ AddError(ParseError.GetCode()); return false; }
		if (!Document->GetRoot()->Get(TEXT("rows"))) continue;
		if (!TestTrue(TEXT("DataTable exports no CurveTable mode"), !Document->GetRoot()->Get(TEXT("table_curve_mode")))) return false;
		if (!FFileHelper::LoadFileToArray(RowsFileBefore, *File)) return false;
		FTCHARToUTF8 Utf8(*Text);
		const int32 RootStart = static_cast<int32>(Document->GetRoot()->GetProvenance().GetByteStart());
		if (RootStart < 0 || RootStart >= Utf8.Length()) return false;
		const int32 InsertAt = RootStart + (Utf8.Get()[RootStart] == '{' ? 1 : 0);
		FTCHARToUTF8 Field(TEXT("\ntable_curve_mode: \"simple\"\n"));
		TArray<uint8> Edited;
		Edited.Append(reinterpret_cast<const uint8*>(Utf8.Get()), InsertAt);
		Edited.Append(reinterpret_cast<const uint8*>(Field.Get()), Field.Length());
		Edited.Append(reinterpret_cast<const uint8*>(Utf8.Get() + InsertAt), Utf8.Length() - InsertAt);
		if (!FFileHelper::SaveArrayToFile(Edited, *File)) return false;
		RowsFile = File;
		break;
	}
	if (!TestFalse(TEXT("ordinary rows document was found"), RowsFile.IsEmpty())) return false;
	const auto Rejected = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t208-datatable-uncaptured-mode"), Import);
	TSharedPtr<FJsonObject> Rejection;
	FString RejectionCode;
	if (!TestFalse(TEXT("DataTable rejects the uncaptured editable root field"), Rejected.IsOk())
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Rejected.GetErrorJson()), Rejection)
		|| !Rejection->TryGetStringField(TEXT("code"), RejectionCode)
		|| !TestEqual(TEXT("shared resolver supplies the exact rejection"), RejectionCode, FString(TEXT("keyed_item_root_field_not_captured"))))
	{ AddError(Rejected.GetErrorJson()); return false; }
	FUeAgentAssetDataSimpleEffectTableRow BeforeRetained;
	BeforeRetained.Count = 37;
	FUeAgentAssetDataSimpleEffectTableRow BeforeRemoved;
	BeforeRemoved.Count = 19;
	const auto* Retained = Table->FindRow<FUeAgentAssetDataSimpleEffectTableRow>(TEXT("Retained"), TEXT("mode rejection"));
	const auto* Removed = Table->FindRow<FUeAgentAssetDataSimpleEffectTableRow>(TEXT("Removed"), TEXT("mode rejection"));
	TArray<uint8> DiskAfter;
	if (!TestTrue(TEXT("rejection leaves the original native table and disk unchanged"),
		LoadObject<UDataTable>(nullptr, *ObjectPath) == Table && !Table->GetOutermost()->IsDirty()
		&& Table->ImportKeyField == TEXT("t208-root-before")
		&& Table->GetRowStruct() == FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()
		&& Table->GetRowMap().Num() == 2 && Retained && Removed
		&& Table->GetRowStruct()->CompareScriptStruct(Retained, &BeforeRetained, 0)
		&& Table->GetRowStruct()->CompareScriptStruct(Removed, &BeforeRemoved, 0)
		&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
	if (!FFileHelper::SaveArrayToFile(RowsFileBefore, *RowsFile)) return false;
	const CommandEdge::FUeAgentCommandResult ImportResult = SubmitPublicCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.import_workspace"),
		TEXT("req-t208-datatable-import"),
		Import);
	if (!TestTrue(
			TEXT("public directory import reaches DataTable row mechanics"),
			ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson());
		return false;
	}
	Table = LoadObject<UDataTable>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("root property persists beside the row edits"), Table
		&& Table->ImportKeyField == TEXT("t208-root-after")
		&& Table->GetRowStruct() == FUeAgentAssetDataSimpleEffectTableRow::StaticStruct())) return false;

	TSharedRef<FJsonObject> Structure = MakeShared<FJsonObject>();
	Structure->SetStringField(TEXT("target"), ObjectPath);
	const CommandEdge::FUeAgentCommandResult StructureResult = SubmitPublicCommand(
		Registry,
		Dispatch,
		TEXT("content_graph.structure"),
		TEXT("req-t208-datatable-readback"),
		Structure);
	if (!TestTrue(
			TEXT("independent public DataTable readback succeeds"),
			StructureResult.IsOk()))
	{
		AddError(StructureResult.GetErrorJson());
		return false;
	}
	TSharedPtr<FJsonObject> StructurePayload;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StructureResult.GetPayloadJson()), StructurePayload)) return false;
	const TArray<TSharedPtr<FJsonValue>>* SchemaEntries = nullptr;
	if (!StructurePayload->TryGetArrayField(TEXT("entries"), SchemaEntries)
		|| !TestEqual(TEXT("structure exposes one row schema"), SchemaEntries->Num(), 1)) return false;
	const auto Schema = (*SchemaEntries)[0]->AsObject();
	if (!TestTrue(TEXT("structure exposes the public default row fields"), Schema.IsValid()
		&& Schema->GetObjectField(TEXT("properties"))->HasField(TEXT("Count")))) return false;
	TArray<TSharedPtr<FJsonValue>> Rows;
	const auto ItemsResult = SubmitPublicCommand(Registry, Dispatch,
		TEXT("content_graph.items"), TEXT("req-t208-datatable-row-readback"), Structure);
	if (!TestTrue(TEXT("readback exposes DataTable rows"),
			ReadRows(ItemsResult, Rows)))
	{
		AddError(ItemsResult.GetErrorJson());
		return false;
	}
	if (!TestEqual(TEXT("saved readback has exactly the retained and new rows"), Rows.Num(), 2)) return false;
	for (const auto& Row : Rows)
	{
		const FString Name = Row->AsObject()->GetStringField(TEXT("name"));
		TestTrue(TEXT("removed row is absent"), Name == TEXT("Retained") || Name == TEXT("Added"));
		TestEqual(TEXT("typed edits and newly assigned values persist"),
			Row->AsObject()->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Count"))->GetNumberField(TEXT("value")),
			Name == TEXT("Retained") ? 73.0 : 29.0);
	}
	Table = nullptr;
	Package = nullptr;
	const FString Reexport = Workspace + TEXT("-readback");
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Reexport, false, true); };
	Export->SetStringField(TEXT("workspace_path"), Reexport);
	const auto ReexportResult = SubmitPublicCommand(Registry, Dispatch,
		TEXT("content_graph.export_workspace"), TEXT("req-t208-datatable-reexport"), Export);
	if (!TestTrue(TEXT("persisted table re-exports through the public composition"), ReexportResult.IsOk()))
	{
		AddError(ReexportResult.GetErrorJson());
		return false;
	}
	TArray<FString> ReexportFiles;
	IFileManager::Get().FindFilesRecursive(ReexportFiles, *Reexport, TEXT("*.hjson"), true, false);
	bool bVerifiedRows = false;
	bool bVerifiedRoot = false;
	for (const FString& File : ReexportFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) continue;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError))
		{
			AddError(ParseError.GetCode());
			return false;
		}
		if (const auto* Categories = Document->GetRoot()->Get(TEXT("categories")))
		{
			for (const auto& Category : Categories->GetEntries())
			{
				if (const auto* Property = Category.Value->Get(TEXT("ImportKeyField")))
				{
					const auto* Value = Property->Get(TEXT("value"));
					bVerifiedRoot = Value && Value->GetText() == TEXT("t208-root-after");
				}
				if (!TestTrue(TEXT("RowStruct remains absent from the editable root properties"),
					Category.Value->Get(TEXT("RowStruct")) == nullptr)) return false;
			}
		}
		const auto* RowMap = Document->GetRoot()->Get(TEXT("rows"));
		if (!RowMap) continue;
		if (RowMap->GetEntries().Num() != 2 || RowMap->Get(TEXT("Removed"))) return false;
		const auto* RowSchema = Document->GetRoot()->Get(TEXT("row_schema"));
		const auto* Defaults = RowSchema ? RowSchema->Get(TEXT("properties")) : nullptr;
		const auto* DefaultCount = Defaults ? Defaults->Get(TEXT("Count")) : nullptr;
		const auto* ExpectedType = DefaultCount ? DefaultCount->Get(TEXT("type")) : nullptr;
		if (!TestTrue(TEXT("re-export defines the Count field type"), ExpectedType != nullptr)) return false;
		for (const FString& Name : {FString(TEXT("Retained")), FString(TEXT("Added"))})
		{
			const auto* Row = RowMap->Get(Name);
			const auto* Properties = Row ? Row->Get(TEXT("properties")) : nullptr;
			const auto* Count = Properties ? Properties->Get(TEXT("Count")) : nullptr;
			const auto* Type = Count ? Count->Get(TEXT("type")) : nullptr;
			const auto* Value = Count ? Count->Get(TEXT("value")) : nullptr;
			if (!Value || !Type)
			{
				AddError(FString::Printf(TEXT("re-export row %s is missing row=%d properties=%d Count=%d type=%d value=%d"),
					*Name, Row != nullptr, Properties != nullptr, Count != nullptr, Type != nullptr, Value != nullptr));
				return false;
			}
			if (!TestEqual(TEXT("re-export retains the declared Count type"), Type->GetText(), ExpectedType->GetText())) return false;
			if (!TestTrue(FString::Printf(TEXT("re-export numeric value kind %d"), static_cast<int32>(Value->GetKind())),
				Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::Integer
				|| Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::Decimal)) return false;
			if (!TestEqual(TEXT("parsed re-export row value"), FCString::Atod(*Value->GetLexeme()),
				Name == TEXT("Retained") ? 73.0 : 29.0)) return false;
		}
		bVerifiedRows = true;
	}
	return TestTrue(TEXT("re-export contains the typed modified and newly assigned rows"), bVerifiedRows)
		&& TestTrue(TEXT("re-export contains the persisted root property"), bVerifiedRoot);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208DataTablePublicInitializationTest,
	"UAI.Root.T208.DataTable.PublicInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTablePublicInitializationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString PackageName = TEXT("/Game/UAI/Transient/T208DataTable/DT_Init_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T208DataTable/initial-readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Created = FindObject<UObject>(nullptr, *Path))
		{
			const TWeakObjectPtr<UObject> Witness(Created);
			TestEqual(TEXT("owned initialization fixture is deleted through UE"), ObjectTools::ForceDeleteObjects({Created}, false), 1);
			TestFalse(TEXT("owned initialization object is gone"), Witness.IsValid(true, true));
		}
		TestFalse(TEXT("owned initialization asset file is gone"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
	};
	if (!TestFalse(TEXT("initialization target starts absent"), FindObject<UObject>(nullptr, *Path) != nullptr
		|| IFileManager::Get().FileExists(*Filename))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{ AddError(Error); return false; }
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("inline"));
	Source->SetStringField(TEXT("document"),
		TEXT("schema: \"uai.datatable_curvetable.edit/1\"\nroot: {\n")
		TEXT(" type: \"/Script/Engine.DataTable\"\n row_struct_ref: \"")
		+ FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()->GetPathName()
		+ TEXT("\"\n categories: { ImportOptions: { ImportKeyField: { type: \"string\", value: \"initial-key\" } } }\n")
		TEXT(" rows: { First: { properties: { Count: { type: \"int32\", value: 73 }, Label: { type: \"string\", value: \"Created row\" } } } }\n}\n"));
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), Path);
	Import->SetObjectField(TEXT("source"), Source);
	const auto Result = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("t208-datatable-public-initialization"), Import);
	if (!TestTrue(TEXT("public typed rows and categories initialize an absent DataTable"), Result.IsOk()))
	{ AddError(Result.GetErrorJson()); return false; }
	UDataTable* Reloaded = LoadObject<UDataTable>(nullptr, *Path);
	if (!TestTrue(TEXT("created table has persisted native struct and root property"), Reloaded
		&& Reloaded->GetRowStruct() == FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()
		&& Reloaded->ImportKeyField.Equals(TEXT("initial-key"), ESearchCase::CaseSensitive)
		&& IFileManager::Get().FileExists(*Filename) && !Reloaded->GetOutermost()->IsDirty())) return false;
	const auto* Row = Reloaded->FindRow<FUeAgentAssetDataSimpleEffectTableRow>(TEXT("First"), TEXT("T208"));
	if (!TestTrue(TEXT("native row storage contains the typed initialized fields"), Row && Row->Count == 73
		&& Row->Label.Equals(TEXT("Created row"), ESearchCase::CaseSensitive) && Reloaded->GetRowMap().Num() == 1)) return false;
	Reloaded = nullptr;
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), Path);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("t208-datatable-initial-reexport"), Export);
	if (!TestTrue(TEXT("initialized table exports a public semantic workspace"), Exported.IsOk()))
	{ AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool bRows = false, bRoot = false;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) continue;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Document;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Document, ParseError))
		{ AddError(ParseError.GetCode()); return false; }
		if (const auto* Rows = Document->GetRoot()->Get(TEXT("rows")))
		{
			const auto* First = Rows->Get(TEXT("First"));
			const auto* Properties = First ? First->Get(TEXT("properties")) : nullptr;
			const auto* Count = Properties ? Properties->Get(TEXT("Count")) : nullptr;
			const auto* Value = Count ? Count->Get(TEXT("value")) : nullptr;
			const auto* Type = Count ? Count->Get(TEXT("type")) : nullptr;
			bRows = Rows->GetEntries().Num() == 1 && Type
				&& Type->GetKind() == HjsonWorkspace::EHjsonNodeKind::String && Type->GetText() == TEXT("int32")
				&& Value && (Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::Integer
					|| Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::Decimal)
				&& FCString::Atod(*Value->GetLexeme()) == 73.0;
		}
		if (const auto* Categories = Document->GetRoot()->Get(TEXT("categories")))
			for (const auto& Category : Categories->GetEntries())
				if (const auto* Property = Category.Value->Get(TEXT("ImportKeyField")))
					if (const auto* Value = Property->Get(TEXT("value")))
						bRoot = Value->GetKind() == HjsonWorkspace::EHjsonNodeKind::String
							&& Value->GetText().Equals(TEXT("initial-key"), ESearchCase::CaseSensitive);
	}
	return TestTrue(TEXT("ordinary re-export independently preserves initialized rows and root properties"), bRows && bRoot);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208DataTableInitializationLifecycleTest,
	"UAI.Root.T208.DataTable.InitializationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataTableInitializationLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainAdapters::DataTableCurveTable;
	using namespace UeAgentAuthoringOperationApply;
	const FString PackageName = TEXT("/Game/UAI/Transient/T208DataTable/DT_CreateLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	auto IsAbsent = [&]()
	{
		return !FindObject<UObject>(nullptr, *Path) && !IFileManager::Get().FileExists(*Filename)
			&& !FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
	};
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = FindObject<UObject>(nullptr, *Path))
			TestEqual(TEXT("remaining owned initialization fixture deletes through UE"), ObjectTools::ForceDeleteObjects({Remaining}, false), 1);
		TestTrue(TEXT("initialization lifecycle fixture leaves no object, file or registry entry"), IsAbsent());
	};
	FString Error;
	if (!RegisterDataTableAuthoringState(Error)) { AddError(Error); return false; }
	if (!TestTrue(TEXT("create lifecycle begins with independently observed absence"), IsAbsent())) return false;
	const auto Count = MakeShared<FJsonObject>();
	Count->SetStringField(TEXT("type"), TEXT("int32"));
	Count->SetNumberField(TEXT("value"), 73);
	const auto Properties = MakeShared<FJsonObject>();
	Properties->SetObjectField(TEXT("Count"), Count);
	const auto Row = MakeShared<FJsonObject>();
	Row->SetObjectField(TEXT("properties"), Properties);
	const auto Rows = MakeShared<FJsonObject>();
	Rows->SetObjectField(TEXT("First"), Row);
	const auto Desired = MakeShared<FJsonObject>();
	Desired->SetStringField(TEXT("row_struct_ref"), FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()->GetPathName());
	Desired->SetObjectField(TEXT("rows"), Rows);
	const auto Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("property_path"), TEXT("ImportKeyField"));
	Root->SetStringField(TEXT("value"), TEXT("create-lifecycle-key"));
	const TArray<TSharedPtr<FJsonValue>> RootProperties{MakeShared<FJsonValueObject>(Root)};
	Count->SetStringField(TEXT("value"), TEXT("not-an-integer"));
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Invalid;
	if (!TestFalse(TEXT("invalid typed row is rejected before creating any target"),
		ApplyDataTableAuthoring(Path, TEXT("t208-create-invalid"), Desired, Invalid, nullptr, RootProperties, true))
		|| !TestTrue(TEXT("prepare failure preserves original absence"), IsAbsent())) return false;
	Count->SetNumberField(TEXT("value"), 73);
	class FRejectCreatedStage final : public IDurablePackageStager
	{
	public:
		FRejectCreatedStage(FString InPath, FString InFilename) : Path(MoveTemp(InPath)), Filename(MoveTemp(InFilename)), Inner(CreateDefaultDurablePackageStager()) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
			const UDataTable* Actual = FindObject<UDataTable>(nullptr, *Path);
			const auto* Row = Actual ? Actual->FindRow<FUeAgentAssetDataSimpleEffectTableRow>(TEXT("First"), TEXT("T208"), false) : nullptr;
			if (!Actual) return true;
			bObserved = Row && Row->Count == 73 && Actual->ImportKeyField == TEXT("create-lifecycle-key")
				&& !IFileManager::Get().FileExists(*Filename);
			Failure.Diagnostic.Code = TEXT("t208_create_stage_rejected_after_real_staging");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		}
		bool bObserved = false;
	private:
		FString Path, Filename;
		TSharedRef<IDurablePackageStager> Inner;
	};
	const auto Stager = MakeShared<FRejectCreatedStage>(Path, Filename);
	FBackendOptions Options;
	Options.PackageStager = Stager;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Early;
	Early.FinalReadback = [](FOperationFailure&) { return true; };
	const bool bEarly = ApplyDataTableAuthoring(Path, TEXT("t208-create-early"), Desired, Early, &Options, RootProperties, true);
	if (!TestFalse(TEXT("early create staging failure is returned"), bEarly)
		|| !TestTrue(TEXT("early failure follows real rows/root writes and staging before publication"), Stager->bObserved)
		|| !TestTrue(TEXT("early rollback restores object, disk and registry absence"), IsAbsent()))
	{ AddError(Early.Diagnostic.Code + TEXT(":") + Early.Diagnostic.PublicMessage); return false; }
	bool bLateObserved = false;
	HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome Late;
	Late.FinalReadback = [&](FOperationFailure& Failure)
	{
		const UDataTable* Actual = LoadObject<UDataTable>(nullptr, *Path);
		const auto* Row = Actual ? Actual->FindRow<FUeAgentAssetDataSimpleEffectTableRow>(TEXT("First"), TEXT("T208"), false) : nullptr;
		bLateObserved = Row && Row->Count == 73 && Actual->ImportKeyField == TEXT("create-lifecycle-key")
			&& IFileManager::Get().FileExists(*Filename) && !Actual->GetOutermost()->IsDirty()
			&& FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(Path)).IsValid();
		Failure.Diagnostic.Code = TEXT("t208_create_late_rejected_after_reloaded_readback");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	const bool bLate = ApplyDataTableAuthoring(Path, TEXT("t208-create-late"), Desired, Late, nullptr, RootProperties, true);
	if (!TestFalse(TEXT("late create failure is returned"), bLate)
		|| !TestTrue(TEXT("late rejection observes reloaded typed rows/root, registry and final file"), bLateObserved)
		|| !TestTrue(TEXT("late rollback restores independently observed absence"), IsAbsent()))
	{ AddError(Late.Diagnostic.Code + TEXT(":") + Late.Diagnostic.PublicMessage); return false; }
	FPlatformProcess::Sleep(0.15f);
	FAssetRegistryModule::TickAssetRegistry(0.15f);
	return TestTrue(TEXT("restored absence survives pending native registry processing"), IsAbsent());
}
