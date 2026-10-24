#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoring.h"
#include "Domains/Deformer/Authoring/UeAgentDeformerAuthoringApply.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"
#include "OptimusNodeLink.h"
#include "OptimusBindingTypes.h"
#include "OptimusDataTypeRegistry.h"
#include "OptimusShaderText.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

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
		const TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			Registry,
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const FString& RequestId,
		const TSharedRef<FJsonObject>& Params)
	{
		const auto* Command = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
		FString ParamsJson;
		FString JsonError;
		if (!Command || !Dispatch.IsValid()
			|| !CommandContract::Json::TryCanonicalize(
				Params, ParamsJson, JsonError))
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				TEXT("corr-t85-deformer"), RequestId, CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				TEXT("deformer_public_command_unavailable"),
				TEXT("the public command or request is unavailable"));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t85-deformer\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId, *CommandId, *Command->GetContractHash(), *ParamsJson);
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
				TEXT("corr-t85-deformer"), RequestId, CommandId,
				Command->GetContractHash(),
				TEXT("deformer_public_request_invalid"),
				TEXT("the public request could not be decoded"));
		}
		return Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("t85-deformer"),
				{TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")},
				TEXT("corr-t85-deformer")),
			MakeShared<FNeverCancelled>());
	}

	bool TryLoadReadableGraph(
		const FString& Root,
		FString& OutPath,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files, *Root, TEXT("*.hjson"), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FString Normalized = File;
			FPaths::NormalizeFilename(Normalized);
			if (Normalized.Contains(TEXT("/.uai/")) || File.EndsWith(TEXT(".interfaces.hjson"))) continue;
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (FFileHelper::LoadFileToString(Text, *File)
				&& FUeAgentStructuredHjsonParser::ParseObject(
					Text, File, Document, OutError)
				&& Document.IsValid()
				&& Document->HasTypedField<EJson::Object>(TEXT("nodes"))
				&& Document->HasTypedField<EJson::Array>(TEXT("connections")))
			{
				OutPath = File;
				OutDocument = MoveTemp(Document);
				return true;
			}
		}
		OutError = TEXT("deformer_visible_graph_missing");
		return false;
	}

	bool EditReadableWorkspace(const FString& Root, const FString& CandidateName, FString& OutError)
	{
		FString Path;
		TSharedPtr<FJsonObject> Document;
		if (!TryLoadReadableGraph(Root, Path, Document, OutError)) return false;
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes)
			|| !Nodes || !Nodes->IsValid())
		{
			OutError = TEXT("deformer_visible_nodes_missing");
			return false;
		}
		if ((*Nodes)->Values.Num() != 1) { OutError = TEXT("deformer_initial_node_count_invalid"); return false; }
		for (const auto& Existing : (*Nodes)->Values)
		{
			const auto Spec = Existing.Value->AsObject();
			const TSharedPtr<FJsonObject>* Position = nullptr;
			if (!Spec->TryGetObjectField(TEXT("pos"), Position) || !Position
				|| (*Position)->GetNumberField(TEXT("x")) != 80 || (*Position)->GetNumberField(TEXT("y")) != 48)
			{ OutError = TEXT("deformer_existing_layout_not_visible"); return false; }
			const auto ChangedPosition = MakeShared<FJsonObject>();
			ChangedPosition->SetNumberField(TEXT("x"), 320);
			ChangedPosition->SetNumberField(TEXT("y"), 96);
			Spec->SetObjectField(TEXT("pos"), ChangedPosition);
		}
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("name"), CandidateName);
		(*Nodes)->SetObjectField(TEXT("T85Imported"), Node);
		FString Updated;
		return CommandContract::Json::TryCanonicalize(Document, Updated, OutError)
			&& FFileHelper::SaveStringToFile(Updated, *Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// Independent native values, including graph members, node/pin subobjects and binding settings.
	// Transient compile/action state is not package state; names and native property values are retained.
	struct FNativeDeformerState
	{
		TMap<FString, FString> Fields;
		TMap<FString, FText> Texts;
	};

	FNativeDeformerState NativeDeformerState(UOptimusDeformer& Asset)
	{
		TArray<UObject*> Objects;
		FReferenceFinder References(Objects, &Asset, false, true, true, true);
		References.FindReferences(&Asset);
		Objects.Add(&Asset);
		FNativeDeformerState State;
		for (UObject* Object : Objects)
		{
			if (!IsValid(Object) || Object->HasAnyFlags(RF_Transient)) continue;
			const FString Path = Object->GetPathName(&Asset);
			State.Fields.Add(Path + TEXT("/@class"), Object->GetClass()->GetPathName());
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)) continue;
				if (const FTextProperty* TextProperty = CastField<FTextProperty>(*It))
				{
					State.Texts.Add(Path + TEXT("/") + It->GetName(), TextProperty->GetPropertyValue_InContainer(Object));
					continue;
				}
				FString Value;
				It->ExportText_InContainer(0, Value, Object, nullptr, Object, PPF_None);
				State.Fields.Add(Path + TEXT("/") + It->GetName(), MoveTemp(Value));
			}
		}
		return State;
	}

	bool EqualNativeDeformerState(const FNativeDeformerState& A, const FNativeDeformerState& B)
	{
		if (A.Fields.Num() != B.Fields.Num() || A.Texts.Num() != B.Texts.Num()) return false;
		for (const auto& Item : A.Fields)
		{
			const FString* Other = B.Fields.Find(Item.Key);
			if (!Other || !Item.Value.Equals(*Other, ESearchCase::CaseSensitive)) return false;
		}
		for (const auto& Item : A.Texts)
		{
			const FText* Other = B.Texts.Find(Item.Key);
			if (!Other || !FTextProperty::Identical_Implementation(Item.Value, *Other, PPF_None)) return false;
		}
		return true;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85DeformerWorkspaceVerticalTest,
	"UAI.Root.T85.Deformer.WorkspaceVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85DeformerWorkspaceVerticalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DeformerWorkspace/DG_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerWorkspace/edit");
	const FString Readback = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerWorkspace/readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			TestEqual(TEXT("owned Deformer is deleted through UE"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		}
		TestFalse(TEXT("owned Deformer package file is absent after UE deletion"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Deformer = Package
		? NewObject<UOptimusDeformer>(Package, *AssetName,
			RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (!TestNotNull(TEXT("persistent Optimus fixture is created"), Deformer)
		|| !TestNotNull(TEXT("skeletal mesh fixture exists"), Mesh)) return false;
	Deformer->Mesh = Mesh;
	UClass* CandidateClass = nullptr;
	for (UClass* Class : UOptimusNode::GetAllNodeClasses())
		if (Class->GetName() == TEXT("OptimusNode_CustomComputeKernel")) CandidateClass = Class;
	if (!TestNotNull(TEXT("native basic-node enumeration exposes Custom Compute Kernel"), CandidateClass)) return false;
	const FString CandidateName = CandidateClass->GetDefaultObject<UOptimusNode>()->GetDisplayName().ToString();
	UOptimusNode* Preserved = Deformer->GetUpdateGraph()->AddNode(CandidateClass, FVector2D(80, 48));
	if (!TestNotNull(TEXT("existing basic-node state is created"), Preserved)) return false;
	const FName PreservedName = Preserved->GetFName();
	if (!TestTrue(TEXT("primary binding is created"), Deformer->AddComponentBinding(
		nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName()) != nullptr)
		|| !TestTrue(TEXT("initial Optimus graph compiles"), Deformer->Compile())) return false;
	FAssetRegistryModule::AssetCreated(Deformer);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("initial Optimus fixture saves"),
		UPackage::SavePackage(Package, Deformer, *Filename, SaveArgs))) return false;
	Package->SetDirtyFlag(false);

	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the formal process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)))
	{
		AddError(Error);
		return false;
	}

	TSharedRef<FJsonObject> Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const CommandEdge::FUeAgentCommandResult ExportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t85-deformer-export"), Export);
	if (!TestTrue(TEXT("public Deformer workspace export succeeds"),
		ExportResult.IsOk()))
	{
		AddError(ExportResult.GetErrorJson()); return false;
	}
	FString VisibleGraphPath;
	TSharedPtr<FJsonObject> VisibleGraph;
	FString VisibleGraphText;
	TestTrue(TEXT("export exposes readable graph collections without machine identity"),
		TryLoadReadableGraph(
			Workspace, VisibleGraphPath, VisibleGraph, Error)
		&& CommandContract::Json::TryCanonicalize(
			VisibleGraph, VisibleGraphText, Error)
		&& VisibleGraph->HasTypedField<EJson::Object>(TEXT("nodes"))
		&& VisibleGraph->HasTypedField<EJson::Array>(TEXT("connections"))
		&& !VisibleGraphText.Contains(TEXT("guid"), ESearchCase::IgnoreCase)
		&& !VisibleGraphText.Contains(TEXT("md5:"), ESearchCase::IgnoreCase));
	if (!TestTrue(TEXT("workspace accepts a readable node edit"),
		EditReadableWorkspace(Workspace, CandidateName, Error))) { AddError(Error); return false; }

	TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	TSharedRef<FJsonObject> Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName);
	Import->SetObjectField(TEXT("source"), Source);
	Preserved = nullptr;
	Deformer = nullptr;
	Package = nullptr;
	const CommandEdge::FUeAgentCommandResult ImportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.import_workspace"),
		TEXT("req-t85-deformer-import"), Import);
	if (!TestTrue(TEXT("public Deformer workspace import reaches atomic native basic-node apply"),
		ImportResult.IsOk()))
	{
		AddError(ImportResult.GetErrorJson()); return false;
	}
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!TestTrue(TEXT("independent published package unload succeeds"),
			UPackageTools::UnloadPackages({Published}, UnloadError, true)))
		{ AddError(UnloadError.ToString()); return false; }
	}
	Deformer = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("published Deformer reloads"), Deformer)) return false;
	TestFalse(TEXT("published Deformer is clean"), Deformer->GetOutermost()->IsDirty());
	TestTrue(TEXT("native skeletal mesh binding is preserved"), Deformer->Mesh == Mesh && Deformer->GetComponentBindings().Num() == 1);
	int32 ExistingCount = 0, AddedCount = 0;
	for (UOptimusNode* Node : Deformer->GetUpdateGraph()->GetAllNodes())
	{
		if (Node->GetFName() == PreservedName && Node->GetClass() == CandidateClass
			&& Node->GetGraphPosition() == FVector2D(320, 96)) ++ExistingCount;
		else if (Node->GetClass() == CandidateClass && Node->GetGraphPosition() == FVector2D::ZeroVector) ++AddedCount;
	}
	TestEqual(TEXT("existing native node persists the ordinary position edit"), ExistingCount, 1);
	TestEqual(TEXT("new native node has the actual requested basic class and position"), AddedCount, 1);
	TestEqual(TEXT("graph contains exactly the original and new basic nodes"), Deformer->GetUpdateGraph()->GetAllNodes().Num(), 2);
	TSharedRef<FJsonObject> ReExport = MakeShared<FJsonObject>();
	ReExport->SetStringField(TEXT("target"), PackageName);
	ReExport->SetStringField(TEXT("workspace_path"), Readback);
	ReExport->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const CommandEdge::FUeAgentCommandResult ReExportResult = SubmitPublicCommand(
		Registry, Dispatch, TEXT("content_graph.export_workspace"),
		TEXT("req-t85-deformer-readback"), ReExport);
	if (!TestTrue(TEXT("independent Deformer readback export succeeds"),
		ReExportResult.IsOk()))
	{
		AddError(ReExportResult.GetErrorJson()); return false;
	}
	FString ReadbackGraphPath;
	TSharedPtr<FJsonObject> ReadbackGraph;
	if (!TestTrue(TEXT("saved and reloaded readback exposes an ordinary graph"),
		TryLoadReadableGraph(Readback, ReadbackGraphPath, ReadbackGraph, Error))) return false;
	const auto ReadbackNodes = ReadbackGraph->GetObjectField(TEXT("nodes"));
	TestEqual(TEXT("readback retains both basic nodes"), ReadbackNodes->Values.Num(), 2);
	for (const auto& Entry : ReadbackNodes->Values)
	{
		if (!TestTrue(TEXT("readback node is a structured ordinary value"), Entry.Value->Type == EJson::Object)) return false;
		TestEqual(TEXT("readback has the readable native class without a class path"),
			Entry.Value->AsObject()->GetStringField(TEXT("node_type")), CandidateClass->GetName());
		const auto Position = Entry.Value->AsObject()->GetObjectField(TEXT("pos"));
		TestTrue(TEXT("reexport exposes the actual edited or default position"),
			(Position->GetNumberField(TEXT("x")) == 320 && Position->GetNumberField(TEXT("y")) == 96)
			|| (Position->GetNumberField(TEXT("x")) == 0 && Position->GetNumberField(TEXT("y")) == 0));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85DeformerEarlyCheckpointTest,
	"UAI.Root.T85.Deformer.EarlyCheckpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	bool VerifyInputPins(UOptimusNode& Node, int32 ExpectedCount)
	{
		int32 Existing = 0, Added = 0;
		const auto FloatType = FOptimusDataTypeRegistry::Get().FindType(*FFloatProperty::StaticClass());
		for (UOptimusNodePin* RootPin : Node.GetPins())
		{
			for (UOptimusNodePin* Pin : RootPin->GetSubPinsRecursively(true))
			{
				if (Pin->GetDirection() != EOptimusNodePinDirection::Input) continue;
				if (Pin->GetFName() != TEXT("Existing") && Pin->GetFName() != TEXT("Added")) continue;
				if (Pin->GetDataType() != FloatType || !Pin->GetDataDomain().IsSingleton()) return false;
				if (Pin->GetFName() == TEXT("Existing")) ++Existing; else ++Added;
			}
		}
		return Existing == 1 && Added == ExpectedCount - 1;
	}

	FNativeDeformerState ReadBindingNeighbors(UOptimusNode& Node)
	{
		FNativeDeformerState State;
		for (TFieldIterator<FProperty> It(Node.GetClass()); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated)
				|| It->GetFName() == TEXT("InputBindingArray") || It->GetFName() == TEXT("ShaderSource")) continue;
			FString Value;
			It->ExportText_InContainer(0, Value, &Node, nullptr, &Node, PPF_None);
			State.Fields.Add(It->GetName(), MoveTemp(Value));
		}
		return State;
	}

	bool ReadOrEditInputBindings(const FString& Root, int32 ExpectedCount, bool bEdit, FString& Error,
		const TCHAR* FirstName = TEXT("Existing"), const TCHAR* AddedName = TEXT("Added"), bool bSkipEmpty = false)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("properties.hjson"), true, false);
		int32 Matches = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
			for (const auto& Category : (*Categories)->Values)
			{
				const auto Fields = Category.Value->AsObject();
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (!Fields.IsValid() || !Fields->TryGetObjectField(TEXT("InputBindingArray"), Property)) continue;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				const TSharedPtr<FJsonObject>* Members = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (!(*Property)->TryGetObjectField(TEXT("value"), Value)
					|| !(*Value)->TryGetObjectField(TEXT("fields"), Members)
					|| !(*Members)->TryGetArrayField(TEXT("InnerArray"), Items))
				{
					FString Actual, JsonError;
					CommandContract::Json::TryCanonicalize(*Property, Actual, JsonError);
					Error = TEXT("deformer_binding_shape_or_count_mismatch:") + Actual;
					return false;
				}
				if (bSkipEmpty && Items->IsEmpty()) continue;
				if (Items->Num() != ExpectedCount) { Error = TEXT("deformer_binding_count_mismatch"); return false; }
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject>* ItemFields = nullptr;
					const TSharedPtr<FJsonObject>* Name = nullptr;
					const TSharedPtr<FJsonObject>* NameFields = nullptr;
					FString ActualName;
					if ((*Items)[Index]->Type != EJson::Object
						|| !(*Items)[Index]->AsObject()->TryGetObjectField(TEXT("fields"), ItemFields)
						|| !(*ItemFields)->TryGetObjectField(TEXT("Name"), Name)
						|| !(*Name)->TryGetObjectField(TEXT("fields"), NameFields)
						|| !(*NameFields)->TryGetStringField(TEXT("Name"), ActualName)
						|| !ActualName.Equals(Index == 0 ? FirstName : AddedName, ESearchCase::CaseSensitive))
					{ Error = TEXT("deformer_binding_name_shape_or_value_mismatch"); return false; }
				}
				++Matches;
				if (bEdit)
				{
					FString CopyText;
					TSharedPtr<FJsonObject> Copy;
					if (!CommandContract::Json::TryCanonicalize((*Items)[0]->AsObject(), CopyText, Error)
						|| !FUeAgentStructuredHjsonParser::ParseObject(CopyText, File, Copy, Error)) return false;
					Copy->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Name"))->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Name"), AddedName);
					TArray<TSharedPtr<FJsonValue>> Updated = *Items;
					Updated.Add(MakeShared<FJsonValueObject>(Copy));
					(*Members)->SetArrayField(TEXT("InnerArray"), Updated);
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
						|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
		}
		if (Matches != 1) Error = FString::Printf(TEXT("deformer_binding_matches:%d"), Matches);
		return Matches == 1;
	}

	bool ReadOrEditGroupSize(const FString& Root, int32 ExpectedX, bool bEdit, FString& Error)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("properties.hjson"), true, false);
		int32 Matches = 0, MeshMatches = 0;
		for (const FString& File : Files)
		{
			FString Text;
			TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File)
				|| !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Categories = nullptr;
			if (!Document->TryGetObjectField(TEXT("categories"), Categories)) continue;
			for (const auto& Category : (*Categories)->Values)
			{
				const auto Fields = Category.Value->AsObject();
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (Fields.IsValid() && Fields->TryGetObjectField(TEXT("Mesh"), Property))
				{
					++MeshMatches;
					if (bEdit)
					{
						if ((*Property)->HasTypedField<EJson::Null>(TEXT("value"))) { Error = TEXT("seed Mesh unexpectedly null"); return false; }
						(*Property)->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
						if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
							|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
					}
					else if (!(*Property)->HasTypedField<EJson::Null>(TEXT("value"))) return false;
				}
				if (!Fields.IsValid() || !Fields->TryGetObjectField(TEXT("GroupSize"), Property)) continue;
				const TSharedPtr<FJsonObject>* Value = nullptr;
				const TSharedPtr<FJsonObject>* Components = nullptr;
				if (!(*Property)->TryGetObjectField(TEXT("value"), Value)
					|| !(*Value)->TryGetObjectField(TEXT("fields"), Components)
					|| !(*Components)->HasTypedField<EJson::Number>(TEXT("X"))
					|| (*Components)->GetNumberField(TEXT("X")) != ExpectedX
					|| (*Components)->GetNumberField(TEXT("Y")) != 1 || (*Components)->GetNumberField(TEXT("Z")) != 1)
				{
					FString Actual, JsonError;
					CommandContract::Json::TryCanonicalize(*Property, Actual, JsonError);
					Error = TEXT("deformer_group_size_shape_or_value_mismatch:") + Actual;
					return false;
				}
				++Matches;
				if (bEdit)
				{
					(*Components)->SetNumberField(TEXT("X"), 32);
					if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
						|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				}
			}
		}
		if (Matches != 1 || MeshMatches != 1) Error = FString::Printf(TEXT("deformer_group_size_matches:%d mesh:%d"), Matches, MeshMatches);
		return Matches == 1 && MeshMatches == 1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85DeformerPropertyWorkspaceTest,
	"UAI.Root.T85.Deformer.PropertyWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85DeformerPropertyWorkspaceTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DeformerProperties/DG_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerProperties/edit");
	const FString Readback = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerProperties/readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("owned asset deleted through UE"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		TestFalse(TEXT("owned disk absent"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Asset = NewObject<UOptimusDeformer>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Asset->Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UClass* NodeClass = nullptr;
	for (UClass* Class : UOptimusNode::GetAllNodeClasses())
		if (Class->GetName() == TEXT("OptimusNode_CustomComputeKernel")) NodeClass = Class;
	if (!TestNotNull(TEXT("public basic node class exists"), NodeClass)) return false;
	UOptimusNode* Node = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(96, 48));
	if (!TestNotNull(TEXT("native node created"), Node)
		|| !TestNotNull(TEXT("primary binding created"), Asset->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName()))
		|| !TestTrue(TEXT("seed compiles"), Asset->Compile())) return false;
	const FName NodeName = Node->GetFName();
	const FStructProperty* Group = FindFProperty<FStructProperty>(NodeClass, TEXT("GroupSize"));
	if (!TestNotNull(TEXT("native reflected GroupSize exists"), Group)) return false;
	TestTrue(TEXT("native seed group size"), *Group->ContainerPtrToValuePtr<FIntVector>(Node) == FIntVector(64, 1, 1));
	const FString GroupKey = Node->GetPathName(Asset) + TEXT("/GroupSize");
	const FString MeshKey = Asset->GetPathName(Asset) + TEXT("/Mesh");
	if (!TestNotNull(TEXT("original skeletal mesh assigned"), Asset->Mesh.Get())) return false;
	FAssetRegistryModule::AssetCreated(Asset);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("seed saves"), UPackage::SavePackage(Package, Asset, *Filename, Save))) return false;
	Package->SetDirtyFlag(false);
	auto Before = NativeDeformerState(*Asset);
	Before.Fields.Remove(GroupKey);
	Before.Fields.Remove(MeshKey);
	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-deformer-property-export"), Export);
	if (!TestTrue(TEXT("public property export"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("ordinary recursive GroupSize edit"), ReadOrEditGroupSize(Workspace, 64, true, Error))) { AddError(Error); return false; }
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto Import = MakeShared<FJsonObject>(); Import->SetStringField(TEXT("target"), PackageName); Import->SetObjectField(TEXT("source"), Source);
	Node = nullptr; Asset = nullptr; Package = nullptr;
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-deformer-property-import"), Import);
	if (!TestTrue(TEXT("public property import"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!TestTrue(TEXT("independent disk unload"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) { AddError(UnloadError.ToString()); return false; }
	}
	Asset = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent disk reload"), Asset)) return false;
	for (UOptimusNode* Existing : Asset->GetUpdateGraph()->GetAllNodes()) if (Existing->GetFName() == NodeName) Node = Existing;
	if (!TestNotNull(TEXT("original node retained"), Node)) return false;
	TestTrue(TEXT("native full recursive GroupSize persisted"), *Group->ContainerPtrToValuePtr<FIntVector>(Node) == FIntVector(32, 1, 1));
	TestNull(TEXT("native root Mesh explicitly cleared"), Asset->Mesh.Get());
	TestFalse(TEXT("published graph clean"), Asset->GetOutermost()->IsDirty());
	auto After = NativeDeformerState(*Asset); After.Fields.Remove(GroupKey);
	After.Fields.Remove(MeshKey);
	TestTrue(TEXT("all other persistent native state retained"), EqualNativeDeformerState(Before, After));
	Export->SetStringField(TEXT("workspace_path"), Readback);
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-deformer-property-reexport"), Export);
	if (!TestTrue(TEXT("independent ordinary reexport"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("ordinary GroupSize readback"), ReadOrEditGroupSize(Readback, 32, false, Error))) AddError(Error);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85DeformerBindingWorkspaceTest,
	"UAI.Root.T85.Deformer.BindingWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85DeformerBindingWorkspaceTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DeformerBindings/DG_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerBindings/edit");
	const FString Readback = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerBindings/readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("owned asset deleted through UE"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		TestFalse(TEXT("owned disk absent"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Asset = NewObject<UOptimusDeformer>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Asset->Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UClass* NodeClass = nullptr;
	for (UClass* Class : UOptimusNode::GetAllNodeClasses())
		if (Class->GetName() == TEXT("OptimusNode_CustomComputeKernel")) NodeClass = Class;
	if (!TestNotNull(TEXT("public basic node class exists"), NodeClass)) return false;
	UOptimusNode* Node = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(96, 48));
	if (!TestNotNull(TEXT("native node created"), Node)
		|| !TestNotNull(TEXT("primary binding created"), Asset->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName()))
		|| !TestTrue(TEXT("seed compiles"), Asset->Compile())) return false;
	const FName NodeName = Node->GetFName();
	const FStructProperty* Group = FindFProperty<FStructProperty>(NodeClass, TEXT("GroupSize"));
	if (!TestNotNull(TEXT("native reflected GroupSize exists"), Group)) return false;
	TestTrue(TEXT("native seed group size"), *Group->ContainerPtrToValuePtr<FIntVector>(Node) == FIntVector(64, 1, 1));
	const FStructProperty* BindingProperty = FindFProperty<FStructProperty>(NodeClass, TEXT("InputBindingArray"));
	const FStructProperty* ShaderProperty = FindFProperty<FStructProperty>(NodeClass, TEXT("ShaderSource"));
	if (!TestNotNull(TEXT("native binding struct exists"), BindingProperty) || !TestNotNull(TEXT("native shader source exists"), ShaderProperty)) return false;
	FOptimusParameterBinding Binding;
	Binding.Name = FName(TEXT("Existing"));
	Binding.DataType.Set(FOptimusDataTypeRegistry::Get().FindType(*FFloatProperty::StaticClass()));
	if (!TestTrue(TEXT("native float binding valid"), Binding.IsValid())) return false;
	auto* Bindings = BindingProperty->ContainerPtrToValuePtr<FOptimusParameterBindingArray>(Node);
	Bindings->InnerArray.Add(Binding);
	FProperty* InnerArray = FindFProperty<FArrayProperty>(BindingProperty->Struct, TEXT("InnerArray"));
	if (!TestNotNull(TEXT("native binding array field exists"), InnerArray)) return false;
	// Seed through the native whole-array paste event before the public export under test.
	FPropertyChangedEvent SeedEvent(InnerArray, EPropertyChangeType::ValueSet);
	SeedEvent.SetActiveMemberProperty(const_cast<FStructProperty*>(BindingProperty));
	Node->PostEditChangeProperty(SeedEvent);
	if (!TestTrue(TEXT("seed native binding pin exists"), VerifyInputPins(*Node, 1))) return false;
	if (!TestTrue(TEXT("seed binding graph compiles"), Asset->Compile())) return false;
	const auto Neighbors = ReadBindingNeighbors(*Node);
	const FString ShaderText = ShaderProperty->ContainerPtrToValuePtr<FOptimusShaderText>(Node)->ShaderText;
	FAssetRegistryModule::AssetCreated(Asset);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("seed saves"), UPackage::SavePackage(Package, Asset, *Filename, Save))) return false;
	Package->SetDirtyFlag(false);

	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-deformer-binding-export"), Export);
	if (!TestTrue(TEXT("public property export"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("ordinary binding array edit"), ReadOrEditInputBindings(Workspace, 1, true, Error))) { AddError(Error); return false; }
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto Import = MakeShared<FJsonObject>(); Import->SetStringField(TEXT("target"), PackageName); Import->SetObjectField(TEXT("source"), Source);
	Node = nullptr; Asset = nullptr; Package = nullptr;
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("req-deformer-binding-import"), Import);
	if (!TestTrue(TEXT("public property import"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError;
		if (!TestTrue(TEXT("independent disk unload"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) { AddError(UnloadError.ToString()); return false; }
	}
	Asset = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent disk reload"), Asset)) return false;
	for (UOptimusNode* Existing : Asset->GetUpdateGraph()->GetAllNodes()) if (Existing->GetFName() == NodeName) Node = Existing;
	if (!TestNotNull(TEXT("original node retained"), Node)) return false;
	TestTrue(TEXT("native original GroupSize retained"), *Group->ContainerPtrToValuePtr<FIntVector>(Node) == FIntVector(64, 1, 1));
	Bindings = BindingProperty->ContainerPtrToValuePtr<FOptimusParameterBindingArray>(Node);
	if (!TestEqual(TEXT("native binding membership persisted"), Bindings->Num(), 2)) return false;
	TestTrue(TEXT("native binding names persisted"), (*Bindings)[0].Name == TEXT("Existing") && (*Bindings)[1].Name == TEXT("Added"));
	for (const auto& Actual : *Bindings)
		TestTrue(TEXT("native binding type/domain/flags preserved"), Actual.DataType == Binding.DataType && Actual.DataDomain == Binding.DataDomain
			&& Actual.bSupportRead == Binding.bSupportRead && Actual.bSupportAtomicIfCompatibleDataType == Binding.bSupportAtomicIfCompatibleDataType);
	TestTrue(TEXT("both native input pins reconstructed and persisted"), VerifyInputPins(*Node, 2));
	TestTrue(TEXT("native position retained"), Node->GetGraphPosition() == FVector2D(96,48));
	TestTrue(TEXT("native mesh and primary binding retained"), Asset->Mesh != nullptr && Asset->GetComponentBindings().Num() == 1);
	const auto* Shader = ShaderProperty->ContainerPtrToValuePtr<FOptimusShaderText>(Node);
	TestTrue(TEXT("native shader body retained"), Shader->ShaderText.Equals(ShaderText, ESearchCase::CaseSensitive));
	TestTrue(TEXT("native declarations reflect newly rebuilt input"), Shader->Declarations.Contains(TEXT("Added")));
	TestFalse(TEXT("published graph clean"), Asset->GetOutermost()->IsDirty());
	TestTrue(TEXT("other editable native settings retained"), EqualNativeDeformerState(Neighbors, ReadBindingNeighbors(*Node)));
	Export->SetStringField(TEXT("workspace_path"), Readback);
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("req-deformer-binding-reexport"), Export);
	if (!TestTrue(TEXT("independent ordinary reexport"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	if (!TestTrue(TEXT("ordinary binding array readback"), ReadOrEditInputBindings(Readback, 2, false, Error))) AddError(Error);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85DeformerConnectionWorkspaceTest,
	"UAI.Root.T85.Deformer.ConnectionWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85DeformerConnectionWorkspaceTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DeformerConnections/DG_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerConnections/edit");
	const FString Readback = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerConnections/readback");
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			TestEqual(TEXT("owned connection fixture deleted through UE"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		TestFalse(TEXT("owned disk absent"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		IFileManager::Get().DeleteDirectory(*Readback, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Asset = NewObject<UOptimusDeformer>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Asset->Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	UClass* NodeClass = nullptr;
	for (UClass* Class : UOptimusNode::GetAllNodeClasses()) if (Class->GetName() == TEXT("OptimusNode_CustomComputeKernel")) NodeClass = Class;
	if (!TestNotNull(TEXT("basic node class"), NodeClass) || !TestNotNull(TEXT("mesh"), Asset->Mesh.Get())) return false;
	UOptimusNode* SourceNode = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(96,48));
	UOptimusNode* TargetNode = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(384,48));
	if (!SourceNode || !TargetNode || !Asset->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName())) return false;
	auto Seed = [&](UOptimusNode* Node, const TCHAR* PropertyName, const TCHAR* PinName) -> UOptimusNodePin*
	{
		FStructProperty* Property = FindFProperty<FStructProperty>(NodeClass, PropertyName);
		if (!Property) return nullptr;
		FOptimusParameterBinding Binding; Binding.Name = FName(PinName);
		Binding.DataType.Set(FOptimusDataTypeRegistry::Get().FindType(*FFloatProperty::StaticClass()));
		Property->ContainerPtrToValuePtr<FOptimusParameterBindingArray>(Node)->InnerArray.Add(Binding);
		FPropertyChangedEvent Event(FindFProperty<FArrayProperty>(Property->Struct, TEXT("InnerArray")), EPropertyChangeType::ValueSet);
		Event.SetActiveMemberProperty(Property); Node->PostEditChangeProperty(Event);
		for (UOptimusNodePin* RootPin : Node->GetPins())
			for (UOptimusNodePin* Pin : RootPin->GetSubPinsRecursively(true)) if (Pin->GetFName() == PinName) return Pin;
		return nullptr;
	};
	UOptimusNodePin* FromPin = Seed(SourceNode, TEXT("OutputBindingArray"), TEXT("Produced"));
	UOptimusNodePin* ToPin = Seed(TargetNode, TEXT("InputBindingArray"), TEXT("Consumed"));
	FString Error;
	if (!TestTrue(TEXT("native typed data domains allow the intended connection"), FromPin && ToPin && FromPin->CanCannect(ToPin, &Error))) { AddError(Error); return false; }
	const FName SourceName = SourceNode->GetFName(), TargetName = TargetNode->GetFName();
	const FString FromPath = FromPin->GetUniqueName().ToString(), ToPath = ToPin->GetUniqueName().ToString();
	const auto SourceSettings = ReadBindingNeighbors(*SourceNode), TargetSettings = ReadBindingNeighbors(*TargetNode);
	if (!TestTrue(TEXT("seed compiles"), Asset->Compile())) return false;
	FAssetRegistryModule::AssetCreated(Asset);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("seed saves"), UPackage::SavePackage(Package, Asset, *Filename, Save))) return false;
	Package->SetDirtyFlag(false);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Export = MakeShared<FJsonObject>(); Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace); Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Exported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("deformer-connection-export"), Export);
	if (!TestTrue(TEXT("public connection export"), Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	FString GraphPath; TSharedPtr<FJsonObject> Graph;
	if (!TryLoadReadableGraph(Workspace, GraphPath, Graph, Error)) { AddError(Error); return false; }
	TArray<FString> Interfaces; IFileManager::Get().FindFilesRecursive(Interfaces, *Workspace, TEXT("*.interfaces.hjson"), true, false);
	FString FromEndpoint, ToEndpoint;
	for (const FString& File : Interfaces)
	{
		FString Text; TSharedPtr<FJsonObject> Interface;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Interface, Error)) return false;
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Interface->TryGetObjectField(TEXT("nodes"), Nodes)) continue;
		for (const auto& Pair : (*Nodes)->Values)
		{
			for (const auto& Value : Pair.Value->AsObject()->GetArrayField(TEXT("pins")))
			{
				const auto Pin = Value->AsObject();
				const FString Direction = Pin->GetStringField(TEXT("direction"));
				FString& Endpoint = Direction == TEXT("output") ? FromEndpoint : ToEndpoint;
				if (!Endpoint.IsEmpty()) { AddError(TEXT("ambiguous ordinary test pin")); return false; }
				HjsonWorkspace::FWorkspaceError EndpointError;
				if (!HjsonWorkspace::TryFormatGraphAuthoringEndpoint(Pair.Key, Pin->GetStringField(TEXT("alias")), 0,
					Direction == TEXT("output") ? HjsonWorkspace::EGraphAuthoringEndpointDirection::Output : HjsonWorkspace::EGraphAuthoringEndpointDirection::Input,
					Endpoint, EndpointError)) { AddError(EndpointError.GetCode()); return false; }
			}
		}
	}
	if (!TestTrue(TEXT("ordinary interfaces expose source and nested target pins"), !FromEndpoint.IsEmpty() && !ToEndpoint.IsEmpty())) return false;
	auto Connection = MakeShared<FJsonObject>(); Connection->SetStringField(TEXT("from"), FromEndpoint); Connection->SetStringField(TEXT("to"), ToEndpoint);
	Graph->SetArrayField(TEXT("connections"), {MakeShared<FJsonValueObject>(Connection)});
	FString Updated;
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Graph.ToSharedRef(), Updated, Error)
		|| !FFileHelper::SaveStringToFile(Updated, *GraphPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	auto Import = MakeShared<FJsonObject>(); Import->SetStringField(TEXT("target"), PackageName); Import->SetObjectField(TEXT("source"), Source);
	SourceNode = nullptr; TargetNode = nullptr; FromPin = nullptr; ToPin = nullptr; Asset = nullptr; Package = nullptr;
	const auto Imported = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("deformer-connection-import"), Import);
	if (!TestTrue(TEXT("public ordinary connection import"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError; if (!TestTrue(TEXT("independent disk unload"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) { AddError(UnloadError.ToString()); return false; }
	}
	Asset = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent disk reload"), Asset)) return false;
	for (UOptimusNode* Node : Asset->GetUpdateGraph()->GetAllNodes())
	{ if (Node->GetFName() == SourceName) SourceNode = Node; if (Node->GetFName() == TargetName) TargetNode = Node; }
	if (!SourceNode || !TargetNode) return false;
	FromPin = SourceNode->FindPin(FromPath); ToPin = TargetNode->FindPin(ToPath);
	if (!TestEqual(TEXT("one native persisted link"), Asset->GetUpdateGraph()->GetAllLinks().Num(), 1)) return false;
	const auto* Link = Asset->GetUpdateGraph()->GetAllLinks()[0];
	TestTrue(TEXT("native complete endpoints match intended pins"), Link->GetNodeOutputPin() == FromPin && Link->GetNodeInputPin() == ToPin);
	TestTrue(TEXT("node settings retained"), EqualNativeDeformerState(SourceSettings, ReadBindingNeighbors(*SourceNode)) && EqualNativeDeformerState(TargetSettings, ReadBindingNeighbors(*TargetNode)));
	TestFalse(TEXT("published graph clean"), Asset->GetOutermost()->IsDirty());
	Export->SetStringField(TEXT("workspace_path"), Readback);
	const auto Reexport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("deformer-connection-reexport"), Export);
	if (!TestTrue(TEXT("public persisted reexport"), Reexport.IsOk())) { AddError(Reexport.GetErrorJson()); return false; }
	if (!TryLoadReadableGraph(Readback, GraphPath, Graph, Error)) return false;
	const auto& Connections = Graph->GetArrayField(TEXT("connections"));
	TestTrue(TEXT("ordinary connection endpoints survive reexport"), Connections.Num() == 1
		&& Connections[0]->AsObject()->GetStringField(TEXT("from")) == FromEndpoint
		&& Connections[0]->AsObject()->GetStringField(TEXT("to")) == ToEndpoint);
	if (HasAnyErrors()) return false;
	// Retain the ordinary connection while a real binding-array edit rebuilds its target pin.
	if (!TestTrue(TEXT("ordinary connected binding append"), ReadOrEditInputBindings(Readback, 1, true, Error, TEXT("Consumed"), TEXT("Added"), true)))
	{ AddError(Error); return false; }
	FString TargetAlias, ExistingPinAlias, AddedEndpoint;
	HjsonWorkspace::FWorkspaceError EndpointError;
	if (!HjsonWorkspace::TryParseGraphAuthoringEndpoint(ToEndpoint, TargetAlias, ExistingPinAlias, EndpointError)) return false;
	if (!TestTrue(TEXT("exported input alias identifies copied binding"), ExistingPinAlias.EndsWith(TEXT("Consumed"), ESearchCase::CaseSensitive))) return false;
	const FString AddedPinAlias = ExistingPinAlias.LeftChop(FCString::Strlen(TEXT("Consumed"))) + TEXT("Added");
	if (!HjsonWorkspace::TryFormatGraphAuthoringEndpoint(TargetAlias, AddedPinAlias, 0,
		HjsonWorkspace::EGraphAuthoringEndpointDirection::Input, AddedEndpoint, EndpointError)) return false;
	auto AddedConnection = MakeShared<FJsonObject>();
	AddedConnection->SetStringField(TEXT("from"), FromEndpoint); AddedConnection->SetStringField(TEXT("to"), AddedEndpoint);
	auto DesiredConnections = Graph->GetArrayField(TEXT("connections"));
	DesiredConnections.Add(MakeShared<FJsonValueObject>(AddedConnection));
	Graph->SetArrayField(TEXT("connections"), DesiredConnections);
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Graph.ToSharedRef(), Updated, Error)
		|| !FFileHelper::SaveStringToFile(Updated, *GraphPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Source->SetStringField(TEXT("path"), Readback);
	SourceNode = nullptr; TargetNode = nullptr; FromPin = nullptr; ToPin = nullptr; Asset = nullptr;
	const auto Rebound = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), TEXT("deformer-connection-binding-import"), Import);
	if (!TestTrue(TEXT("public binding update retains desired connection"), Rebound.IsOk())) { AddError(Rebound.GetErrorJson()); return false; }
	if (UPackage* Published = FindPackage(nullptr, *PackageName))
	{
		FText UnloadError; if (!TestTrue(TEXT("independent binding disk unload"), UPackageTools::UnloadPackages({Published}, UnloadError, true))) { AddError(UnloadError.ToString()); return false; }
	}
	Asset = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent binding disk reload"), Asset)) return false;
	for (UOptimusNode* Node : Asset->GetUpdateGraph()->GetAllNodes())
	{ if (Node->GetFName() == SourceName) SourceNode = Node; if (Node->GetFName() == TargetName) TargetNode = Node; }
	if (!SourceNode || !TargetNode) return false;
	FromPin = SourceNode->FindPin(FromPath); ToPin = TargetNode->FindPin(ToPath);
	int32 AddedPins = 0;
	UOptimusNodePin* AddedPin = nullptr;
	for (UOptimusNodePin* RootPin : TargetNode->GetPins())
		for (UOptimusNodePin* Pin : RootPin->GetSubPinsRecursively(true))
			if (Pin->GetFName() == TEXT("Added") && Pin->GetDirection() == EOptimusNodePinDirection::Input) { ++AddedPins; AddedPin = Pin; }
	TestEqual(TEXT("new binding really rebuilt native pins"), AddedPins, 1);
	if (!TestEqual(TEXT("two native links survive pin rebuilding and persistence"), Asset->GetUpdateGraph()->GetAllLinks().Num(), 2)) return false;
	for (UOptimusNodePin* ExpectedInput : {ToPin, AddedPin})
		TestTrue(TEXT("connection resolves current rebuilt and newly added pins"), Asset->GetUpdateGraph()->GetAllLinks().ContainsByPredicate([&](const UOptimusNodeLink* ActualLink)
			{ return ActualLink->GetNodeOutputPin() == FromPin && ActualLink->GetNodeInputPin() == ExpectedInput; }));
	TestTrue(TEXT("neighboring settings preserved after binding rebuild"), EqualNativeDeformerState(SourceSettings, ReadBindingNeighbors(*SourceNode)) && EqualNativeDeformerState(TargetSettings, ReadBindingNeighbors(*TargetNode)));
	TestFalse(TEXT("rebound persisted graph clean"), Asset->GetOutermost()->IsDirty());
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	const auto FinalExport = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("deformer-connection-binding-reexport"), Export);
	if (!TestTrue(TEXT("rebound graph ordinary reexport"), FinalExport.IsOk())) { AddError(FinalExport.GetErrorJson()); return false; }
	if (!TryLoadReadableGraph(Workspace, GraphPath, Graph, Error)) return false;
	const auto& FinalConnections = Graph->GetArrayField(TEXT("connections"));
	TestEqual(TEXT("both ordinary connections reexport"), FinalConnections.Num(), 2);
	for (const FString& ExpectedTo : {ToEndpoint, AddedEndpoint})
		TestTrue(TEXT("ordinary retained and added endpoints reexport"), FinalConnections.ContainsByPredicate([&](const TSharedPtr<FJsonValue>& Value)
			{ return Value->AsObject()->GetStringField(TEXT("from")) == FromEndpoint && Value->AsObject()->GetStringField(TEXT("to")) == ExpectedTo; }));
	if (!ReadOrEditInputBindings(Workspace, 2, false, Error, TEXT("Consumed"), TEXT("Added"), true)) AddError(Error);
	return !HasAnyErrors();
}

static bool RunDeformerCheckpoint(FAutomationTestBase& Test, const bool bLate)
{
	using namespace HjsonWorkspace;
	using namespace UeAgentAuthoringOperationApply;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DeformerEarly/DG_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("UeAssetFolders/T85DeformerEarly") / Token;
	ON_SCOPE_EXIT
	{
		if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
			Test.TestEqual(TEXT("owned early fixture is deleted through UE"), ObjectTools::ForceDeleteObjects({Owned}, false), 1);
		Test.TestFalse(TEXT("owned early fixture disk is absent"), IFileManager::Get().FileExists(*Filename));
		IFileManager::Get().DeleteDirectory(*Workspace, false, true);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};
	UClass* NodeClass = nullptr;
	for (UClass* Class : UOptimusNode::GetAllNodeClasses())
		if (Class->GetName() == TEXT("OptimusNode_CustomComputeKernel")) NodeClass = Class;
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!Test.TestNotNull(TEXT("basic native node class exists"), NodeClass) || !Test.TestNotNull(TEXT("mesh exists"), Mesh)) return false;
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Asset = NewObject<UOptimusDeformer>(Package,
		*FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	Asset->Mesh = Mesh;
	UOptimusNode* Retained = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(64, 32));
	if (!Retained || !Asset->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName())
		|| !Asset->Compile()) { Test.AddError(TEXT("early fixture native setup failed")); return false; }
	FAssetRegistryModule::AssetCreated(Asset);
	FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs)) { Test.AddError(TEXT("disk seed save failed")); return false; }
	Package->SetDirtyFlag(false);
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	const auto DiskNative = NativeDeformerState(*Asset);
	Asset->Modify();
	if (!Retained->SetGraphPosition(FVector2D(256, 128))) return false;
	UOptimusNode* Removed = Asset->GetUpdateGraph()->AddNode(NodeClass, FVector2D(480, 224));
	if (!Removed) { Test.AddError(TEXT("dirty additional node creation failed")); return false; }
	const FName RemovedName = Removed->GetFName();
	const FName RetainedName = Retained->GetFName();
	Package->SetDirtyFlag(true);
	const auto DirtyBefore = NativeDeformerState(*Asset);
	if (!Test.TestFalse(TEXT("dirty full native state differs from saved state"), EqualNativeDeformerState(DiskNative, DirtyBefore))) return false;

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{ Test.AddError(Error); return false; }
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	Export->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto ExportResult = SubmitPublicCommand(Registry, Dispatch, TEXT("content_graph.export_workspace"), TEXT("deformer-early-export-") + Token, Export);
	if (!ExportResult.IsOk()) { Test.AddError(ExportResult.GetErrorJson()); return false; }
	FString VisiblePath;
	TSharedPtr<FJsonObject> Visible;
	if (!TryLoadReadableGraph(Workspace, VisiblePath, Visible, Error)) { Test.AddError(Error); return false; }
	const FString Evidence = FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-graph-common-foundation/deformer-lifecycle-r1/readable");
	IFileManager::Get().MakeDirectory(*Evidence, true);
	TArray<FString> LayoutFiles;
	IFileManager::Get().FindFilesRecursive(LayoutFiles, *Workspace, TEXT("layout.hjson"), true, false);
	LayoutFiles.Add(VisiblePath);
	for (int32 Index = 0; Index < LayoutFiles.Num(); ++Index)
	{
		FString Normalized = LayoutFiles[Index]; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString ReadableText;
		if (!FFileHelper::LoadFileToString(ReadableText, *LayoutFiles[Index])) return false;
		FString Relative = LayoutFiles[Index]; FPaths::MakePathRelativeTo(Relative, *Workspace);
		Test.AddInfo(TEXT("Deformer ordinary source ") + Relative + TEXT("\n") + ReadableText.Left(4096));
		FFileHelper::SaveStringToFile(Relative + TEXT("\n") + ReadableText,
			*(Evidence / FString::Printf(TEXT("source-%d.txt"), Index)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	const auto Nodes = Visible->GetObjectField(TEXT("nodes"));
	FString RemovedKey;
	for (const auto& Item : Nodes->Values)
	{
		const auto Node = Item.Value->AsObject();
		const TSharedPtr<FJsonObject>* Position = nullptr;
		if (Node->TryGetObjectField(TEXT("pos"), Position) && Position && (*Position)->GetNumberField(TEXT("x")) == 480)
			RemovedKey = Item.Key;
	}
	if (!Test.TestFalse(TEXT("ordinary layout identifies the dirty node to remove"), RemovedKey.IsEmpty())) return false;
	Nodes->RemoveField(RemovedKey);
	FString Text;
	if (!CommandContract::Json::TryCanonicalize(Visible, Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *VisiblePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;

	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!UeAgentDeformerAuthoring::BuildExportSnapshotReadRequests(*Asset, Requests, Error)
		|| !UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) { Test.AddError(Error + Capture.Diagnostic.InternalContext); return false; }
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	StateSemantic::FUeAgentCanonicalSnapshotPtr Snapshot;
	FAuthoringModelPtr Model;
	FWorkspaceError WorkspaceError;
	if (!TryBuildCanonicalAuthoringCapture(UeAgentDeformerAuthoring::CompositionKind, TEXT("llm_authoring"),
		Capture.Snapshot, Resources, Snapshot, Model, WorkspaceError)) { Test.AddError(WorkspaceError.GetCode()); return false; }
	Capture.Snapshot.ReleaseNativeValues();
	Requests.Reset();
	Composition::HjsonImport::FPreparedAuthoringImportPtr Prepared;
	Composition::HjsonImport::FSemanticWorkspaceImportError PrepareError;
	const FString RequestId = TEXT("deformer-early-") + Token;
	const Contract::FUeAgentCallerContext Caller(TEXT("t85-deformer"),
		{TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write"), TEXT("authoring.structure")}, RequestId);
	if (!Composition::HjsonImport::TryPrepareSemanticWorkspaceImport(FWorkspaceSource::Directory(Workspace),
		ESemanticWorkspaceMode::State, Model.ToSharedRef(), Snapshot.ToSharedRef(),
		Contract::FUeAgentDescriptorIdentity(TEXT("deformer.semantic_workspace"), 1, Model->GetSemanticFingerprint()),
		RequestId, RequestId, Caller, false, {}, {}, Resources, Prepared, PrepareError) || !Prepared.IsValid())
	{ Test.AddError(PrepareError.Code + TEXT(":") + PrepareError.Detail); return false; }

	class FRejectDeletedNodeStage final : public IDurablePackageStager
	{
	public:
		TWeakObjectPtr<UOptimusDeformer> Original;
		FName RemovedNode;
		FString Path;
		TArray<uint8> Disk;
		bool bObserved = false;
		int32 BeforeStages = 0;
		TSharedRef<IDurablePackageStager> Inner = CreateDefaultDurablePackageStager();
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{ return Inner->BuildArtifactInventory(Plans, Artifacts, Failure); }
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
		{
			if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
			UOptimusDeformer* Actual = Original.Get();
			if (!Actual) { Failure.Diagnostic.Code = TEXT("deformer_early_released_before_stage"); return false; }
			for (const UOptimusNode* Node : Actual->GetUpdateGraph()->GetAllNodes())
				if (Node->GetFName() == RemovedNode) { ++BeforeStages; return true; }
			TArray<uint8> Current;
			bObserved = BeforeStages == 1 && Actual->GetUpdateGraph()->GetAllNodes().Num() == 1
				&& Actual->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Current, *Path) && Current == Disk;
			bool bAnyStaged = false;
			for (const auto& Artifact : Artifacts)
			{
				bObserved &= IFileManager::Get().FileExists(*Artifact.StagingPath) == Artifact.bStagedExists;
				bAnyStaged |= Artifact.bStagedExists;
			}
			bObserved &= bAnyStaged;
			Failure.Diagnostic.Code = TEXT("t85_deformer_early_stage_rejected");
			Failure.Diagnostic.PublicMessage = TEXT("Default target staging finished after native node removal and before release/publish");
			return false;
		}
	};
	const auto Stager = MakeShared<FRejectDeletedNodeStage>();
	Stager->Original = Asset; Stager->RemovedNode = RemovedName; Stager->Path = Filename; Stager->Disk = DiskBefore;
	FBackendOptions Options;
	bool bLateObserved = false;
	if (!bLate) Options.PackageStager = Stager;
	else Options.FinalReadback = [&](FOperationFailure& Failure)
	{
		UOptimusDeformer* Published = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
		TArray<uint8> Bytes;
		bLateObserved = Published && !Stager->Original.IsValid() && !Published->GetOutermost()->IsDirty()
			&& Published->Mesh == Mesh && Published->GetComponentBindings().Num() == 1
			&& Published->GetUpdateGraph()->GetAllNodes().Num() == 1
			&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore
			&& FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
		if (bLateObserved)
		{
			const UOptimusNode* Node = Published->GetUpdateGraph()->GetAllNodes()[0];
			bLateObserved = Node->GetFName() == RetainedName && Node->GetClass() == NodeClass
				&& Node->GetGraphPosition().Equals(FVector2D(256, 128));
		}
		Failure.Diagnostic.Code = TEXT("t85_deformer_late_final_readback_rejected");
		Failure.Diagnostic.PublicMessage = TEXT("Observed clean reloaded graph after real deletion and final disk publication");
		return false;
	};
	FApplyWorkspaceResult Result;
	Composition::HjsonImport::FSemanticWorkspaceChangeProjection Projection;
	Asset = nullptr; Package = nullptr; Retained = nullptr; Removed = nullptr;
	const bool bApplied = UeAgentDeformerAuthoring::ApplySemanticWorkspace(ObjectPath, RequestId, *Prepared, Projection, Options, Result, Error);
	if (!Test.TestFalse(TEXT("requested phase rejection fails the update"), bApplied)
		|| !Test.TestTrue(TEXT("rejection observes the real native deletion at its requested persistence phase"), (bLate ? bLateObserved : Stager->bObserved)))
	{ Test.AddError(Error); return false; }
	Test.TestEqual(TEXT("failure phase is preserved"), Result.Execution.Phase, FString(bLate ? TEXT("final_readback") : TEXT("save")));
	Test.TestEqual(TEXT("save failure retains cause"), Result.Execution.Diagnostic.CauseCode, FString(bLate ? TEXT("verification_failed") : TEXT("save_failed")));
	Test.TestEqual(TEXT("diagnostic identifies deliberate post-mutation rejection"), Result.Execution.Diagnostic.Code, FString(bLate ? TEXT("t85_deformer_late_final_readback_rejected") : TEXT("t85_deformer_early_stage_rejected")));
	Test.TestTrue(TEXT("one native mutation was applied before checkpoint rollback"), Result.Execution.MutationsApplied == 1 && Result.Execution.bRollbackAttempted && Result.Execution.bRollbackSucceeded);
	UOptimusDeformer* Restored = LoadObject<UOptimusDeformer>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("checkpoint restored actual Deformer"), Restored)) return false;
	const auto RestoredNative = NativeDeformerState(*Restored);
	if (!EqualNativeDeformerState(DirtyBefore, RestoredNative))
	{
		int32 Differences = 0;
		for (const auto& Item : DirtyBefore.Fields)
		{
			const FString* Actual = RestoredNative.Fields.Find(Item.Key);
			if (!Actual || !Actual->Equals(Item.Value, ESearchCase::CaseSensitive))
			{
				if (++Differences <= 16) Test.AddInfo(FString::Printf(TEXT("Native restore difference %s before=%s after=%s"),
					*Item.Key, *Item.Value.Left(512), Actual ? *Actual->Left(512) : TEXT("<missing>")));
			}
		}
		for (const auto& Item : RestoredNative.Fields)
			if (!DirtyBefore.Fields.Contains(Item.Key) && ++Differences <= 16)
				Test.AddInfo(TEXT("Native restore unexpected field ") + Item.Key + TEXT("=") + Item.Value.Left(512));
		for (const auto& Item : DirtyBefore.Texts)
		{
			const FText* Actual = RestoredNative.Texts.Find(Item.Key);
			if ((!Actual || !FTextProperty::Identical_Implementation(Item.Value, *Actual, PPF_None)) && ++Differences <= 16)
				Test.AddInfo(TEXT("Native restore text difference ") + Item.Key);
		}
		for (const auto& Item : RestoredNative.Texts)
			if (!DirtyBefore.Texts.Contains(Item.Key) && ++Differences <= 16)
				Test.AddInfo(TEXT("Native restore unexpected text field ") + Item.Key);
		Test.AddInfo(FString::Printf(TEXT("Native restore comparison before_fields=%d after_fields=%d differences=%d"),
			DirtyBefore.Fields.Num() + DirtyBefore.Texts.Num(), RestoredNative.Fields.Num() + RestoredNative.Texts.Num(), Differences));
	}
	Test.TestTrue(TEXT("complete original dirty graph members and native settings restore"), EqualNativeDeformerState(DirtyBefore, RestoredNative));
	Test.TestTrue(TEXT("restored package retains original dirty state"), Restored->GetOutermost()->IsDirty());
	Test.TestEqual(TEXT("deleted dirty node is restored in the full two-node graph"), Restored->GetUpdateGraph()->GetAllNodes().Num(), 2);
	TArray<uint8> DiskAfter;
	Test.TestTrue(TEXT("original saved package bytes restore separately from dirty memory"), FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	return !Test.HasAnyErrors();
}

bool FUeAgentT85DeformerEarlyCheckpointTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunDeformerCheckpoint(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85DeformerLateCheckpointTest,
	"UAI.Root.T85.Deformer.LateCheckpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85DeformerLateCheckpointTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunDeformerCheckpoint(*this, true);
}
