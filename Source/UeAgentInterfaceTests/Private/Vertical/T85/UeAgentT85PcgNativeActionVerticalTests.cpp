#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringProjection.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Elements/PCGCreatePointsGrid.h"
#include "Elements/PCGCreatePoints.h"
#include "PackageTools.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringRegistration.h"
#include "HAL/FileManager.h"
#include "Internationalization/Text.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "ObjectTools.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* ExpectedSettingsType =
		TEXT("/Script/PCG.PCGCreatePointsGridSettings");

	class FNeverCancelled final
		: public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	CommandEdge::FUeAgentCommandResult SubmitPublicCommand(
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& ParamsJson)
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		if (!Contract || !Dispatch.IsValid())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-pcg"), TEXT("req-t85-pcg"), CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("pcg_public_import_contract_missing"),
				TEXT("the exact public import contract is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-pcg\",")
			TEXT("\"request_id\":\"req-t85-pcg-%s\",")
			TEXT("\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			*CommandId, *Contract->GetContractHash(), *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		if (!CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-pcg"), TEXT("req-t85-pcg"), CommandId,
				Contract->GetContractHash(),
				TEXT("pcg_public_import_request_invalid"),
				TEXT("the exact public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-pcg"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t85-pcg")),
			MakeShared<FNeverCancelled>());
	}
	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool SaveGraph(
		UPackage& Package,
		UPCGGraph& Graph,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Graph, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildPcgDirectoryImportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), WorkspacePath);

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetObjectField(TEXT("source"), Source);
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildPcgExportParams(
		const FString& Target,
		const FString& WorkspacePath,
		FString& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("workspace_path"), WorkspacePath);
		Params->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryAddVisibleNode(
		const FString& WorkspacePath,
		const FString& CandidateName,
		UE::UAI::Contract::FDiagnostic& OutError,
		const TMap<FString, FString>& Select = {})
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		Files.Sort();
		FString GraphPath;
		TSharedPtr<FJsonObject> Document;
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> CandidateDocument;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, CandidateDocument, OutError)
				&& CandidateDocument.IsValid()
				&& CandidateDocument->HasTypedField<EJson::Object>(TEXT("nodes"))
				&& CandidateDocument->HasTypedField<EJson::Array>(TEXT("connections")))
			{
				GraphPath = File;
				Document = MoveTemp(CandidateDocument);
				break;
			}
		}
		if (!Document.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_visible_graph_missing"));
			return false;
		}
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes)
			|| !Nodes || !(*Nodes).IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_visible_nodes_missing"));
			return false;
		}
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), CandidateName);
		if (!Select.IsEmpty())
		{
			TSharedRef<FJsonObject> Selection = MakeShared<FJsonObject>();
			for (const auto& Field : Select) Selection->SetStringField(Field.Key, Field.Value);
			Candidate->SetObjectField(TEXT("select"), Selection);
		}
		Node->SetObjectField(TEXT("candidate"), Candidate);
		Node->SetArrayField(TEXT("position"), {
			MakeShared<FJsonValueNumber>(160.0),
			MakeShared<FJsonValueNumber>(96.0),
		});
		(*Nodes)->SetObjectField(TEXT("T85Native"), Node);
		FString UpdatedText;
		if (!CommandContract::Json::TryCanonicalize(
			Document, UpdatedText, OutError)
			|| !FFileHelper::SaveStringToFile(
				UpdatedText, *GraphPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			if (OutError.IsEmpty()) OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_visible_hjson_write_failed"));
			return false;
		}
		return true;
	}

	bool TryReadVisibleNode(
		const FString& WorkspacePath)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *WorkspacePath, TEXT("*.hjson"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			UE::UAI::Contract::FDiagnostic Error;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, Document, Error)
				|| !Document.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!Document->TryGetObjectField(TEXT("nodes"), Nodes)
				|| !Nodes || !Nodes->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if ((*Nodes)->TryGetObjectField(TEXT("T85Native1"), Node)
				&& Node && Node->IsValid())
			{
				return true;
			}
		}
		return false;
	}
}

namespace
{
	bool ReadOrEditPointSteepness(const FString& Directory, double Expected, bool bEdit, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("properties.hjson"), true, false);
		int32 Matches = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, OutError)) return false;
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
			for (const auto& Category : (*Categories)->Values)
			{
				const auto Fields = Category.Value->AsObject();
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (!Fields.IsValid() || !Fields->TryGetObjectField(TEXT("PointSteepness"), Property)) continue;
				double Value = 0;
				if (!(*Property)->TryGetNumberField(TEXT("value"), Value) || Value != Expected) continue;
				++Matches;
				if (bEdit)
				{
					(*Property)->SetNumberField(TEXT("value"), 0.75);
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, OutError)
						|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
		}
		if (Matches != 1) OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_fixture_invalid"), FString::Printf(TEXT("pcg_point_steepness_matches:%d"), Matches), TEXT("test_fixture"));
		return Matches == 1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85PcgCompactWorkspaceTest,
	"UAI.Root.T85.PCG.CompactWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PcgCompactWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85PcgCompact/PG_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/PcgCompact") / Token;
	const FString Readback = Workspace + TEXT("-readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("owned compact fixture UE deletion"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		TestFalse(TEXT("owned compact fixture disk absent"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	UPCGSettings* Base = nullptr;
	UPCGNode* Node = Graph->AddNodeOfType(UPCGCreatePointsSettings::StaticClass(), Base);
	UPCGCreatePointsSettings* Settings = Cast<UPCGCreatePointsSettings>(Base);
	if (!TestNotNull(TEXT("native points settings"), Settings)) return false;
	Node->SetNodePosition(123, 234);
	Settings->CoordinateSpace = EPCGCoordinateSpace::LocalComponent;
	FPCGPoint Original(FTransform(FRotator(10,20,30), FVector(12,34,56), FVector(2,3,4)), 0.25f, 731);
	Original.Steepness = 0.8f;
	Original.MetadataEntry = 951;
	Settings->PointsToCreate = {Original};
	Graph->bLandscapeUsesMetadata = true;
	FAssetRegistryModule::AssetCreated(Graph);
	if (!SaveGraph(*Package, *Graph, Filename)) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	UE::UAI::Contract::FDiagnostic Error; FString Params;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error.ToPublicDetail()); return false; }
	if (!TryBuildPcgExportParams(PackageName, Workspace, Params, Error)) return false;
	const auto Export = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), Params);
	if (!TestTrue(TEXT("compact public export"), Export.IsOk())) { AddError(Export.GetErrorJson()); return false; }
	auto EditOrRead = [&](const FString& Root, bool bEdit) -> bool
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("properties.hjson"), true, false);
		int32 Matches = 0, RootMatches = 0;
		for (const FString& File : Files)
		{
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
			for (const auto& Category : (*Categories)->Values)
			{
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (Category.Value->AsObject()->TryGetObjectField(TEXT("bLandscapeUsesMetadata"), Property))
				{
					++RootMatches;
					if (bEdit)
					{
						(*Property)->SetBoolField(TEXT("value"), false);
						if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
							|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
					}
					else if ((*Property)->GetBoolField(TEXT("value"))) return false;
				}
				if (!Category.Value->AsObject()->TryGetObjectField(TEXT("PointsToCreate"), Property)) continue;
				++Matches;
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (!(*Property)->TryGetArrayField(TEXT("value"), Items) || Items->Num() != (bEdit ? 1 : 2))
				{ Error = TEXT("ordinary points array shape/count invalid:") + Text.Left(3000); return false; }
				if (bEdit)
				{
					FString CopyText; TSharedPtr<FJsonObject> Copy;
					if (!CommandContract::Json::TryCanonicalize((*Items)[0]->AsObject(), CopyText, Error)
						|| !FUeAgentStructuredHjsonParser::ParseObject(CopyText, File, Copy, Error)) return false;
					const auto Fields = Copy->GetObjectField(TEXT("fields"));
					if (Fields->HasField(TEXT("MetadataEntry"))) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_fixture_invalid"), TEXT("readonly member leaked into ordinary value"), TEXT("test_fixture")); return false; }
					Fields->SetNumberField(TEXT("Density"), 0.75);
					Fields->RemoveField(TEXT("Seed")); Fields->RemoveField(TEXT("Steepness"));
					auto Updated = *Items; Updated.Add(MakeShared<FJsonValueObject>(Copy)); (*Property)->SetArrayField(TEXT("value"), Updated);
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
						|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
		}
		return Matches == 1 && RootMatches == 1;
	};
	if (!TestTrue(TEXT("ordinary recursive points append"), EditOrRead(Workspace, true))) { AddError(Error.ToPublicDetail()); return false; }
	if (!TryBuildPcgDirectoryImportParams(PackageName, Workspace, Params, Error)) return false;
	Node = nullptr; Settings = nullptr; Base = nullptr; Graph = nullptr; Package = nullptr;
	const auto Import = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Params);
	if (!TestTrue(TEXT("compact public import"), Import.IsOk())) { AddError(Import.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError; if (!TestTrue(TEXT("independent compact unload"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) return false;
	}
	Graph = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent compact reload"), Graph)) return false;
	for (UPCGNode* Candidate : Graph->GetNodes()) if (auto* Points = Cast<UPCGCreatePointsSettings>(Candidate->GetSettings())) { Node = Candidate; Settings = Points; }
	if (!TestNotNull(TEXT("persisted native points"), Settings) || !TestEqual(TEXT("persisted array count"), Settings->PointsToCreate.Num(), 2)) return false;
	FPCGPoint Expected = Original; Expected.Density = 0.75f; Expected.Seed = 0; Expected.Steepness = 0.5f; Expected.MetadataEntry = -1;
	TestTrue(TEXT("old point complete native value preserved"), FPCGPoint::StaticStruct()->CompareScriptStruct(&Original, &Settings->PointsToCreate[0], PPF_None));
	TestTrue(TEXT("new recursive point and native omitted defaults"), FPCGPoint::StaticStruct()->CompareScriptStruct(&Expected, &Settings->PointsToCreate[1], PPF_None));
	TestTrue(TEXT("neighbor settings and position preserved"), Settings->CoordinateSpace == EPCGCoordinateSpace::LocalComponent && Node->PositionX == 123 && Node->PositionY == 234);
	TestFalse(TEXT("root metadata option persisted"), Graph->bLandscapeUsesMetadata);
	TestFalse(TEXT("persisted compact graph clean"), Graph->GetOutermost()->IsDirty());
	if (!TryBuildPcgExportParams(PackageName, Readback, Params, Error)) return false;
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), Params);
	if (!TestTrue(TEXT("compact public reexport"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("ordinary recursive array reexport"), EditOrRead(Readback, false))) AddError(Error.ToPublicDetail());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85PcgAtomicLifecycleTest,
	"UAI.Root.T85.PCG.AtomicLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool RunPcgAtomicLifecycle(FAutomationTestBase& Test, bool bPreserveExistingNodes)
{
	using namespace UE::UAI;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85PcgNativeAction/PG_Atomic_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT {
		DeleteAssetIfPresent(ObjectPath);
		Test.TestFalse(TEXT("owned PCG asset removed by UE"), IFileManager::Get().FileExists(*Filename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentPCGAuthoring::RegisterAuthoring(Error)) { Test.AddError(Error.ToPublicDetail()); return false; }
	UPackage* Package = CreatePackage(*PackageName);
	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Graph);
	UPCGCreatePointsGridSettings* Grid = nullptr;
	UPCGNode* RemovedNode = Graph->AddNodeOfType(Grid);
	if (!Test.TestNotNull(TEXT("native grid node created"), RemovedNode) || !Grid) return false;
	Grid->GridExtents = FVector(200,300,40); Grid->CellSize = FVector(20,30,10);
	RemovedNode->PositionX = 10; RemovedNode->PositionY = 20;
	UPCGNode* Output = Graph->GetOutputNode();
	if (!Test.TestTrue(TEXT("native pins exist"), !RemovedNode->GetOutputPins().IsEmpty() && Output && !Output->GetInputPins().IsEmpty())) return false;
	if (!Test.TestNotNull(TEXT("native connected graph established"), Graph->AddEdge(RemovedNode,
		RemovedNode->GetOutputPins()[0]->Properties.Label, Output, Output->GetInputPins()[0]->Properties.Label))) return false;
	if (!SaveGraph(*Package, *Graph, Filename)) return false;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	Grid->GridExtents = FVector(420,530,60); Grid->CellSize = FVector(42,53,6);
	RemovedNode->PositionX = 71; RemovedNode->PositionY = 82;
	UPCGCreatePointsGridSettings* AddedGrid = nullptr;
	UPCGNode* DirtyNode = Graph->AddNodeOfType(AddedGrid);
	if (!DirtyNode || !AddedGrid) return false;
	DirtyNode->PositionX = 171; DirtyNode->PositionY = 182;
	AddedGrid->GridExtents = FVector(120,230,30); AddedGrid->CellSize = FVector(12,23,3);
	Graph->MarkPackageDirty();
	const FString RemovedName = RemovedNode->GetName();
	auto CaptureSettings = [](const UPCGGraph& Value) {
		TMap<FString, FString> State;
		for (const UPCGNode* Node : Value.GetNodes()) {
			const UPCGSettings* Settings = Node->GetSettings();
			if (!Settings) continue;
			for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It) {
				if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
				FString Text;
				It->ExportText_InContainer(0, Text, Settings, nullptr, const_cast<UPCGSettings*>(Settings), PPF_None);
				State.Add(Node->GetName() + TEXT("/") + It->GetName(), MoveTemp(Text));
			}
		}
		return State;
	};
	const TMap<FString, FString> SettingsBefore = CaptureSettings(*Graph);
	TSharedPtr<FJsonObject> BeforeDocument;
	if (!UeAgentPCGAuthoring::BuildGraphDocument(*Graph, BeforeDocument, Error)) return false;
	FString BeforeJson;
	if (!CommandContract::Json::TryCanonicalize(BeforeDocument, BeforeJson, Error)) return false;
	const auto NodesById = UeAgentPCG::BuildExportNodeMapById(Graph);
	FString RemovedId, DirtyId;
	for (const auto& Pair : NodesById) {
		if (Pair.Value == RemovedNode) RemovedId = Pair.Key;
		if (Pair.Value == DirtyNode) DirtyId = Pair.Key;
	}
	if (!Test.TestFalse(TEXT("removed node has native export identity"), RemovedId.IsEmpty())) return false;
	TSharedPtr<FJsonObject> Desired;
	if (!CommandContract::Json::TryParseObject(BeforeJson, Desired, Error)) return false;
	Desired->GetObjectField(TEXT("nodes"))->RemoveField(RemovedId);
	const auto Edges = Desired->GetObjectField(TEXT("edges"));
	TArray<FString> EdgeKeys; Edges->Values.GetKeys(EdgeKeys);
	for (const FString& Key : EdgeKeys) {
		const auto Edge = Edges->GetObjectField(Key);
		if (Edge->GetObjectField(TEXT("from"))->GetStringField(TEXT("node")) == RemovedId
			|| Edge->GetObjectField(TEXT("to"))->GetStringField(TEXT("node")) == RemovedId) Edges->RemoveField(Key);
	}
	UeAgentPCGAuthoring::FAssetApplyRequest Request;
	Request.AssetPath = PackageName; Request.Graph = Desired; Request.bCompile = true; Request.bSave = true;
	Request.bPreserveExistingNodes = bPreserveExistingNodes;
	if (bPreserveExistingNodes)
	{
		const auto Patch = MakeShared<FJsonObject>();
		Patch->SetStringField(TEXT("property_path"), TEXT("PointSteepness"));
		Patch->SetNumberField(TEXT("value"), 0.75);
		Desired->GetObjectField(TEXT("nodes"))->GetObjectField(DirtyId)->SetArrayField(
			TEXT("properties"), {MakeShared<FJsonValueObject>(Patch)});
	}
	bool bObserved = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		UPCGGraph* Current = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
		TArray<uint8> Bytes;
		const auto CurrentNodes = UeAgentPCG::BuildExportNodeMapById(Current);
		UPCGNode* Retained = CurrentNodes.FindRef(DirtyId);
		const auto* Settings = Retained ? Cast<UPCGCreatePointsGridSettings>(Retained->GetSettings()) : nullptr;
		const bool bClean = Current && !Current->GetPackage()->IsDirty();
		const bool bStructure = Current && Current->GetNodes().Num() == 1 && Retained && !CurrentNodes.Contains(RemovedId)
			&& Current->GetOutputNode()->GetInputPins()[0]->Edges.IsEmpty();
		const bool bValues = Settings && Retained->PositionX == 171 && Retained->PositionY == 182
			&& Settings->GridExtents == (bPreserveExistingNodes ? FVector(120,230,30) : GetDefault<UPCGCreatePointsGridSettings>()->GridExtents)
			&& Settings->CellSize == (bPreserveExistingNodes ? FVector(12,23,3) : GetDefault<UPCGCreatePointsGridSettings>()->CellSize)
			&& (!bPreserveExistingNodes || Settings->PointSteepness == 0.75f);
		const bool bPublished = FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore;
		Test.AddInfo(FString::Printf(TEXT("pcg late native clean=%d structure=%d values=%d published=%d"), bClean, bStructure, bValues, bPublished));
		bObserved = bClean && bStructure && bValues && bPublished;
		Test.TestTrue(TEXT("late readback observes removed native node and edge after clean publication"), bObserved);
		Failure.Diagnostic.Code = TEXT("test_pcg_late_rejection"); return false;
	};
	UeAgentPCGAuthoring::FAssetApplyResult Result;
	const bool bApplied = UeAgentPCGAuthoring::ApplyAsset(Request, Result, TEXT("pcg-atomic-late"));
	Test.AddInfo(FString::Printf(TEXT("pcg late phase=%s code=%s detail=%s rollback=%d/%d observed=%d"),
		*Result.Phase, *Result.Diagnostic.Code, *Result.Diagnostic.InternalContext, Result.bRollbackAttempted, Result.bRollbackSucceeded, bObserved));
	if (!Test.TestFalse(TEXT("late refusal rejects transaction"), bApplied)
		|| !Test.TestTrue(TEXT("checkpoint restoration is confirmed"), bObserved && Result.bRollbackAttempted && Result.bRollbackSucceeded)) return false;
	UPCGGraph* Restored = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("restored graph resolves"), Restored)) return false;
	TSharedPtr<FJsonObject> ActualDocument; FString ActualJson;
	if (!UeAgentPCGAuthoring::BuildGraphDocument(*Restored, ActualDocument, Error)
		|| !CommandContract::Json::TryCanonicalize(ActualDocument, ActualJson, Error)) return false;
	Test.TestTrue(TEXT("all native nodes, positions, pin definitions, edges and parameters restore"), ActualJson.Equals(BeforeJson, ESearchCase::CaseSensitive));
	Test.TestTrue(TEXT("dirty native state remains distinct from original disk"), Restored->GetPackage()->IsDirty());
	const TMap<FString, FString> SettingsAfter = CaptureSettings(*Restored);
	Test.TestEqual(TEXT("native persistent settings field inventory restores"), SettingsAfter.Num(), SettingsBefore.Num());
	for (const auto& Pair : SettingsBefore) {
		const FString* Actual = SettingsAfter.Find(Pair.Key);
		Test.TestTrue(TEXT("native persistent settings field value restores"), Actual && Actual->Equals(Pair.Value, ESearchCase::CaseSensitive));
	}
	TArray<uint8> ActualDisk;
	Test.TestTrue(TEXT("original independent disk bytes restore"), FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk == DiskBefore);
	for (UPCGNode* Node : Restored->GetNodes()) {
		const auto* Settings = Cast<UPCGCreatePointsGridSettings>(Node->GetSettings());
		if (!Test.TestNotNull(TEXT("restored native settings class"), Settings)) return false;
		const bool bRemoved = Node->GetName() == RemovedName;
		Test.TestTrue(TEXT("both native settings values restore"), Settings->GridExtents == (bRemoved ? FVector(420,530,60) : FVector(120,230,30))
			&& Settings->CellSize == (bRemoved ? FVector(42,53,6) : FVector(12,23,3)));
	}
	return !Test.HasAnyErrors();
}

bool FUeAgentT85PcgAtomicLifecycleTest::RunTest(const FString&)
{
	return RunPcgAtomicLifecycle(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85PcgSemanticAtomicLifecycleTest,
	"UAI.Root.T85.PCG.SemanticAtomicLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PcgSemanticAtomicLifecycleTest::RunTest(const FString&)
{
	return RunPcgAtomicLifecycle(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85PcgNativeActionVerticalTest,
	"UAI.Root.T85.PCG.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85PcgNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::Pcg;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85PcgNativeAction/PG_NativeAction_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UPCGGraph* const Graph = Package
		? NewObject<UPCGGraph>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("a persistent PCG Graph fixture is created"), Graph))
	{
		return false;
	}
		FAssetRegistryModule::AssetCreated(Graph);
	UPCGSettings* ExistingSettingsBase = nullptr;
	UPCGNode* ExistingNode = Graph->AddNodeOfType(UPCGCreatePointsGridSettings::StaticClass(), ExistingSettingsBase);
	UPCGCreatePointsGridSettings* ExistingSettings = Cast<UPCGCreatePointsGridSettings>(ExistingSettingsBase);
	if (!TestNotNull(TEXT("the existing native settings fixture is available"), ExistingSettings)) return false;
	ExistingNode->NodeTitle = TEXT("T85Existing");
	ExistingNode->SetNodePosition(31, 47);
	ExistingSettings->GridExtents = FVector(420, 530, 60);
	ExistingSettings->CellSize = FVector(42, 53, 6);
	ExistingSettings->PointSteepness = 0.25f;
	if (!TestTrue(TEXT("existing edge has native endpoints"), !ExistingNode->GetOutputPins().IsEmpty()
		&& !Graph->GetOutputNode()->GetInputPins().IsEmpty())) return false;
	Graph->AddLabeledEdge(ExistingNode, ExistingNode->GetOutputPins()[0]->Properties.Label,
		Graph->GetOutputNode(), Graph->GetOutputNode()->GetInputPins()[0]->Properties.Label);
	auto ReadSettings = [](UPCGSettings* Settings)
	{
		TMap<FString, FString> Values;
		for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient)) continue;
			FString Value;
			It->ExportText_InContainer(0, Value, Settings, nullptr, Settings, PPF_None);
			Values.Add(It->GetName(), MoveTemp(Value));
		}
		return Values;
	};
	const TMap<FString, FString> SettingsBefore = ReadSettings(ExistingSettings);
	if (!TestTrue(
		TEXT("the initial PCG Graph fixture is saved"),
		SaveGraph(*Package, *Graph, Filename)))
	{
		return false;
	}

	UClass* const ExpectedClass = LoadObject<UClass>(
		nullptr, ExpectedSettingsType);
	const UPCGSettings* const ExpectedDefaults = ExpectedClass
		? Cast<UPCGSettings>(ExpectedClass->GetDefaultObject())
		: nullptr;
	if (!TestNotNull(
		TEXT("the representative native PCG settings class resolves"),
		ExpectedDefaults))
	{
		return false;
	}
	const FString ExpectedMenuName =
		ExpectedDefaults->GetDefaultNodeTitle().ToString();
	FPcgNodeCandidateQueryResult CandidateResult;
	FPcgNodeCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("search enumerates the native PCG graph context menu"),
		TryQueryPcgNodeCandidates(
			*Graph,
			ExpectedMenuName,
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FPcgNodeCandidate* const Selected =
		CandidateResult.Candidates.FindByPredicate(
			[&ExpectedMenuName](const FPcgNodeCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.Label.Equals(
						ExpectedMenuName, ESearchCase::CaseSensitive);
			});
	if (!TestNotNull(
		TEXT("the exact native Create Points Grid action is searchable"),
		Selected))
	{
		return false;
	}
	TestFalse(TEXT("the public PCG selector is a readable name"),
		Selected->Name.IsEmpty()
			|| Selected->Name.StartsWith(TEXT("/Script/")));
	TestFalse(TEXT("the public PCG name is not a settings class path"),
		Selected->Name.StartsWith(TEXT("/Script/")));
	const FText NativeMenuTitle = ExpectedDefaults->GetDefaultNodeTitle();
	const FString* OriginalMenuName = FTextInspector::GetSourceString(NativeMenuTitle);
	TestTrue(TEXT("query keeps the original native source name"),
		OriginalMenuName && Selected->Name == *OriginalMenuName);
	TestFalse(TEXT("native PCG search leaves the asset package clean"),
		Package->IsDirty());

	UE::UAI::Contract::FDiagnostic ParamsError;

	TSharedPtr<OperationOwners::UObjectDataAsset::
		IUeAgentUObjectDataAssetCommandService> Service;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString RegistrationError;
	if (!TestTrue(
		TEXT("the production public import service composes"),
		OperationOwners::UObjectDataAsset::
			TryCreateProductionUObjectDataAssetCommandService(
				Service, RegistrationError))
		|| !Service.IsValid()
		|| !TestTrue(
			TEXT("production registration creates exact public dispatch"),
			Module::CommandRegistration::UObjectDataAsset::
				TryCreateUObjectDataAssetRegistration(
					Service.ToSharedRef(), Registry, Dispatch,
					RegistrationError)))
	{
		AddError(RegistrationError);
		return false;
	}
	UE::UAI::Contract::FDiagnostic PcgRegistrationError;
	if (!TestTrue(
			TEXT("PCG authoring export/import owners register"),
			UeAgentPCGAuthoring::RegisterAuthoring(PcgRegistrationError)))
	{
		AddError(PcgRegistrationError.ToPublicDetail());
		return false;
	}
	UeAgentExportContract::FContract PcgExportContract;
	FString ExportContractError;
	TestTrue(
		TEXT("the PCG export contract is registered"),
		UeAgentPCGAuthoring::GetExportContract(
			PcgExportContract,
			ExportContractError)
			&& PcgExportContract.IsValid());
	if (!ExportContractError.IsEmpty())
	{
		AddError(ExportContractError);
	}

	FString CompositionRegistrationError;
	if (!TestTrue(
			TEXT("shared target compositions register for PCG"),
			HjsonWorkspace::RegisterGraphTreeTargetCompositionContracts(
				CompositionRegistrationError)))
	{
		AddError(CompositionRegistrationError);
		return false;
	}
	HjsonWorkspace::FTargetCompositionContract PcgTargetComposition;
	TestTrue(
		TEXT("the PCG target composition is registered"),
		HjsonWorkspace::FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.pcg_graph.asset"),
			PcgTargetComposition));

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/PcgExport_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ReadbackWorkspacePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T85/PcgReadback_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true);
		IFileManager::Get().DeleteDirectory(*ReadbackWorkspacePath, false, true);
	};
	FString ExportParams;
	if (!TestTrue(
		TEXT("the PCG export request uses public workspace fields"),
		TryBuildPcgExportParams(
			PackageName, WorkspacePath, ExportParams, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail());
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ExportResult =
		SubmitPublicCommand(
			Registry, Dispatch, TEXT("content_graph.export_workspace"), ExportParams);
	if (!TestTrue(
		TEXT("formal PCG export publishes the shared graph workspace"),
		ExportResult.IsOk()))
	{
		AddError(ExportResult.GetErrorJson());
		return false;
	}
	if (!TestTrue(
		TEXT("the exported PCG workspace edits only readable node and candidate fields"),
		TryAddVisibleNode(WorkspacePath, Selected->Name, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail());
		return false;
	}
	FString ImportParams;
	if (!TestTrue(
		TEXT("the PCG directory import request uses public workspace fields"),
		TryBuildPcgDirectoryImportParams(
			PackageName, WorkspacePath, ImportParams, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail());
		return false;
	}
	TArray<uint8> DiskBeforeInvalid;
	if (!TestTrue(TEXT("capture saved bytes before invalid selection"),
		FFileHelper::LoadFileToArray(DiskBeforeInvalid, *Filename))) return false;
	if (!TestTrue(TEXT("ordinary HJSON accepts the public select shape"),
		TryAddVisibleNode(WorkspacePath, Selected->Name, ParamsError,
			{{TEXT("settings"), TEXT("Missing settings")}}))) return false;
	const CommandEdge::FUeAgentCommandResult InvalidSelection = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams);
	TestFalse(TEXT("a stale semantic selection is rejected"), InvalidSelection.IsOk());
	TestTrue(TEXT("the public import preserves the shared selection error and original name"),
		InvalidSelection.GetErrorJson().Contains(TEXT("candidate_selector_invalid"))
			&& InvalidSelection.GetErrorJson().Contains(Selected->Name));
	TArray<uint8> DiskAfterInvalid;
	TestTrue(TEXT("invalid selection preserves the original disk bytes"),
		FFileHelper::LoadFileToArray(DiskAfterInvalid, *Filename) && DiskAfterInvalid == DiskBeforeInvalid);
	TestTrue(TEXT("invalid selection leaves the original native node and settings intact"),
		Graph->GetNodes().Num() == 1 && Graph->GetNodes()[0]->GetSettings() == ExistingSettings
			&& ReadSettings(ExistingSettings).OrderIndependentCompareEqual(SettingsBefore)
			&& !Package->IsDirty());
	if (!TestTrue(TEXT("the native settings semantic selection reaches actual creation"),
		TryAddVisibleNode(WorkspacePath, Selected->Name, ParamsError,
			{{TEXT("settings"), TEXT("Create Points Grid")}}))) return false;
	const CommandEdge::FUeAgentCommandResult ImportResult =
		SubmitPublicCommand(
			Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams);
	if (!TestTrue(
		TEXT("formal PCG import consumes the shared semantic graph workspace"),
		ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson());
		return false;
	}
	const FString ImportPayloadJson = ImportResult.GetPayloadJson();
	FString ReadbackExportParams;
	if (!TestTrue(
		TEXT("the independent PCG readback export uses public workspace fields"),
		TryBuildPcgExportParams(
			PackageName, ReadbackWorkspacePath,
			ReadbackExportParams, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail());
		return false;
	}
	const CommandEdge::FUeAgentCommandResult ReadbackExportResult =
		SubmitPublicCommand(
			Registry, Dispatch, TEXT("content_graph.export_workspace"),
			ReadbackExportParams);
	if (!TestTrue(
		TEXT("the independent PCG readback export succeeds"),
		ReadbackExportResult.IsOk()))
	{
		AddError(ReadbackExportResult.GetErrorJson());
		return false;
	}
	TestTrue(
		TEXT("the independent readback contains the readable node alias"),
		TryReadVisibleNode(ReadbackWorkspacePath));
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
		TEXT("the PCG import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	FString ProviderId;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!TestTrue(TEXT("independent unload before persistence verification"),
			UPackageTools::UnloadPackages({Published}, UnloadError, true))) return false;
	}
	UPCGGraph* const Reloaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
	if (!TestNotNull(
		TEXT("the persisted PCG Graph independently reloads"), Reloaded))
	{
		return false;
	}
	TArray<UPCGNode*> MatchingNodes;
	UPCGNode* Preserved = nullptr;
	for (UPCGNode* Node : Reloaded->GetNodes())
	{
		if (Node && Node->GetAuthoredTitleName() == FName(TEXT("T85Existing"))) Preserved = Node;
		if (Node && Node->HasAuthoredTitle()
			&& Node->GetAuthoredTitleName() == FName(TEXT("T85Native")))
		{
			MatchingNodes.Add(Node);
		}
	}
	if (!TestNotNull(TEXT("the existing node survives semantic membership update"), Preserved)) return false;
	TestFalse(TEXT("the independently reloaded semantic graph is clean"), Reloaded->GetOutermost()->IsDirty());
	const TMap<FString, FString> SettingsAfter = ReadSettings(Preserved->GetSettings());
	TestEqual(TEXT("all native settings fields remain present"), SettingsAfter.Num(), SettingsBefore.Num());
	for (const auto& Pair : SettingsBefore)
	{
		const FString* Actual = SettingsAfter.Find(Pair.Key);
		TestTrue(TEXT("unedited native setting is preserved: ") + Pair.Key,
			Actual && Actual->Equals(Pair.Value, ESearchCase::CaseSensitive));
	}
	TestEqual(TEXT("existing placement X is preserved"), Preserved->PositionX, 31);
	TestEqual(TEXT("existing placement Y is preserved"), Preserved->PositionY, 47);
	auto HasPreservedEdge = [](UPCGGraph* Current, UPCGNode* Node)
	{
		const UPCGPin* Sink = Current->GetOutputNode()->GetInputPins()[0];
		return Sink->Edges.Num() == 1 && Sink->Edges[0]
			&& Sink->Edges[0]->GetOtherPin(Sink) == Node->GetOutputPins()[0];
	};
	if (!TestTrue(TEXT("existing connection survives semantic desired edges"), HasPreservedEdge(Reloaded, Preserved))) return false;
	TestEqual(TEXT("one native action-created node survives reload"),
		MatchingNodes.Num(), 1);
	if (MatchingNodes.Num() == 1)
	{
		UPCGNode* const Created = MatchingNodes[0];
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture ReadCapture;
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
		if (!TestTrue(TEXT("independent PCG capture consumes the ordinary export composition"),
			DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*Reloaded, ReadCapture, ReadError)))
		{
			AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
			return false;
		}
		const auto* Captured = ReadCapture.Items.FindByPredicate([Created](const auto& Item)
			{ return Item.ItemId == Created->GetPathName(); });
		TSharedPtr<FJsonValue> ReadValue;
		FString ReadJsonError;
		if (!TestTrue(TEXT("PCG node read state is an object"), Captured && Captured->State.IsValid()
			&& StateSemantic::CanonicalJson::TryToJsonValue(*Captured->State, ReadValue, ReadJsonError)
			&& ReadValue.IsValid() && ReadValue->Type == EJson::Object)) return false;
		TestEqual(TEXT("readback uses the exact fresh HJSON node alias"),
			ReadValue->AsObject()->GetStringField(TEXT("name")), FString(TEXT("T85Native1")));
		TestEqual(TEXT("readback identifies the actual native settings"),
			ReadValue->AsObject()->GetStringField(TEXT("node_type")), FString(TEXT("Create Points Grid")));
		TestTrue(TEXT("explicit pin interface is available without machine identity"),
			ReadValue->AsObject()->HasTypedField<EJson::Object>(TEXT("pin_interface"))
				&& !ReadValue->AsObject()->HasField(TEXT("id"))
				&& !ReadValue->AsObject()->HasField(TEXT("settings_class")));
		TestFalse(TEXT("PCG read capture leaves the persisted graph clean"), Reloaded->GetOutermost()->IsDirty());
		TestNotNull(TEXT("the persisted node owns native settings"),
			Created->GetSettings());
		if (Created->GetSettings())
		{
			TestEqual(TEXT("the searched action created the intended settings type"),
				Created->GetSettings()->GetClass()->GetPathName(),
				FString(ExpectedSettingsType));
		}
		TestEqual(TEXT("placement remains outside candidate and persists on X"),
			Created->PositionX, 160);
		TestEqual(TEXT("placement remains outside candidate and persists on Y"),
			Created->PositionY, 96);
	}

	if (!TestTrue(TEXT("ordinary property readback retains the nondefault value"),
		ReadOrEditPointSteepness(ReadbackWorkspacePath, 0.25, true, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail()); return false;
	}
	if (!TryBuildPcgDirectoryImportParams(PackageName, ReadbackWorkspacePath, ImportParams, ParamsError))
	{
		AddError(ParamsError.ToPublicDetail()); return false;
	}
	const auto PropertyImport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), ImportParams);
	if (!TestTrue(TEXT("public node property edit uses the shared descriptor projection"), PropertyImport.IsOk()))
	{
		AddError(PropertyImport.GetErrorJson()); return false;
	}
	UPCGGraph* PropertyReloaded = LoadObject<UPCGGraph>(nullptr, *ObjectPath);
	UPCGNode* PropertyNode = UeAgentPCG::BuildExportNodeMapById(PropertyReloaded).FindRef(TEXT("T85Existing"));
	if (!TestNotNull(TEXT("property-edited node persists"), PropertyNode)) return false;
	const auto PropertySettings = Cast<UPCGCreatePointsGridSettings>(PropertyNode->GetSettings());
	if (!TestTrue(TEXT("changed property and neighboring native values persist clean"), PropertySettings
		&& PropertySettings->PointSteepness == 0.75f && PropertySettings->GridExtents == FVector(420, 530, 60)
		&& PropertySettings->CellSize == FVector(42, 53, 6) && !PropertyReloaded->GetOutermost()->IsDirty()
		&& HasPreservedEdge(PropertyReloaded, PropertyNode))) return false;
	const auto PropertyState = ReadSettings(PropertySettings);
	for (const auto& Pair : SettingsBefore)
	{
		if (Pair.Key == TEXT("PointSteepness")) continue;
		const FString* Actual = PropertyState.Find(Pair.Key);
		TestTrue(TEXT("neighboring native field remains unchanged: ") + Pair.Key,
			Actual && Actual->Equals(Pair.Value, ESearchCase::CaseSensitive));
	}
	const FString PropertyReadback = ReadbackWorkspacePath + TEXT("_property");
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*PropertyReadback, false, true); };
	if (!TryBuildPcgExportParams(PackageName, PropertyReadback, ReadbackExportParams, ParamsError)) return false;
	const auto PropertyExport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), ReadbackExportParams);
	if (!TestTrue(TEXT("property update independently reexports"), PropertyExport.IsOk())
		|| !TestTrue(TEXT("ordinary reexport contains the edited native property"),
			ReadOrEditPointSteepness(PropertyReadback, 0.75, false, ParamsError)))
	{
		AddError(ParamsError.ToPublicDetail()); return false;
	}

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned PCG fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
