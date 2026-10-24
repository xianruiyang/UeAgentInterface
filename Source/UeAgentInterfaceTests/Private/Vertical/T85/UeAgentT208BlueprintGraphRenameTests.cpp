#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintGraphSignature.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	TMap<FString, FString> CaptureIdentity(UBlueprint& Blueprint, const FGuid& RenamedGraph)
	{
		TMap<FString, FString> State;
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			const FString GraphKey = Graph->GraphGuid.ToString();
			State.Add(GraphKey, Graph->GetClass()->GetPathName() + (Graph->GraphGuid == RenamedGraph ? FString() : TEXT(":") + Graph->GetName()));
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const FString NodeKey = GraphKey + TEXT("/") + Node->NodeGuid.ToString();
				State.Add(NodeKey, Node->GetClass()->GetPathName() + FString::Printf(TEXT(":%d,%d"), Node->NodePosX, Node->NodePosY));
				for (UEdGraphPin* Pin : Node->Pins)
				{
					TArray<FString> Links;
					for (UEdGraphPin* Link : Pin->LinkedTo) Links.Add(Link->GetOwningNode()->NodeGuid.ToString() + TEXT("/") + Link->PinId.ToString());
					Links.Sort();
					State.Add(NodeKey + TEXT("/") + Pin->PinId.ToString(), Pin->PinName.ToString()
						+ TEXT(":") + Pin->PinType.PinCategory.ToString() + FString::FromInt(Pin->Direction)
						+ TEXT(":") + Pin->DefaultValue + TEXT(":") + FString::Join(Links, TEXT(",")));
				}
			}
		}
		return State;
	}

	UEdGraph* FindFunction(UBlueprint& Blueprint, const FString& Name)
	{
		for (UEdGraph* Graph : Blueprint.FunctionGraphs) if (Graph && Graph->GetName() == Name) return Graph;
		return nullptr;
	}

	TSharedRef<FJsonObject> RenameExtension(const FGuid& Guid, const FString& Name, const FString& Kind = TEXT("function_graph"))
	{
		auto Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("operation"), TEXT("update"));
		Spec->SetStringField(TEXT("name"), Name);
		Spec->SetStringField(TEXT("graph_guid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
		Spec->SetStringField(TEXT("graph_kind"), Kind);
		auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(Guid.ToString(EGuidFormats::DigitsWithHyphens), Spec);
		auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs);
		Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
		return Extension;
	}

	bool ReadNames(UBlueprint& Blueprint, const FString& Name, const FGuid& Guid, const FGuid& CallerId, const FGuid& LocalId)
	{
		UEdGraph* Function = FindFunction(Blueprint, Name);
		if (!Function || Function->GraphGuid != Guid || !Blueprint.GeneratedClass || Blueprint.Status == BS_Error) return false;
		bool Caller = false, Local = false;
		TArray<UEdGraph*> Graphs; Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (auto* Call = Cast<UK2Node_CallFunction>(Node); Call && Node->NodeGuid == CallerId)
				Caller = Call->FunctionReference.GetMemberName() == FName(*Name)
					&& Call->GetTargetFunction() && Call->GetTargetFunction()->GetName() == Name;
			if (auto* Variable = Cast<UK2Node_VariableGet>(Node); Variable && Node->NodeGuid == LocalId)
				Local = Variable->VariableReference.GetMemberScopeName() == Name;
		}
		return Caller && Local;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintGraphRenameTest,
	"UeAgentInterface.T208.Blueprint.GraphRename", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintGraphRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	const bool MVVMWasLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("ModelViewViewModelEditor"));
	const bool AdapterWasLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("UeAgentInterfaceMVVM"));
	AddInfo(FString::Printf(TEXT("optional modules before fixture: mvvm_editor=%d mvvm_blueprint=%d adapter=%d"), MVVMWasLoaded,
		FModuleManager::Get().IsModuleLoaded(TEXT("ModelViewViewModelBlueprint")), AdapterWasLoaded));
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85GraphRename_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-graph-identity-rename/native") / Token);
	ON_SCOPE_EXIT
	{
		if (!MVVMWasLoaded && !AdapterWasLoaded)
			TestFalse(TEXT("ordinary rename does not load optional MVVM adapter"), FModuleManager::Get().IsModuleLoaded(TEXT("UeAgentInterfaceMVVM")));
	};
	TArray<FString> OwnedObjects{ObjectPath};
	ON_SCOPE_EXIT
	{
		for (int32 Index = OwnedObjects.Num() - 1; Index >= 0; --Index)
			if (UObject* Object = LoadObject<UObject>(nullptr, *OwnedObjects[Index]))
			{
				Object->GetOutermost()->SetDirtyFlag(false);
				TestEqual(TEXT("owned rename fixture deleted through native UE lifecycle"), ObjectTools::DeleteObjects({Object}, false), 1);
			}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto NewBlueprint = [&](const FString& Path, UClass* Parent) {
		auto* Created = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Path),
			*FPackageName::GetLongPackageAssetName(Path), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Created) FAssetRegistryModule::AssetCreated(Created);
		return Created;
	};
	UBlueprint* Blueprint = NewBlueprint(PackageName, AActor::StaticClass());
	if (!TestNotNull(TEXT("native Actor Blueprint fixture"), Blueprint)) return false;
	UEdGraph* Function = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("ComputeStatus"));
	UEdGraph* Untouched = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Function || !Untouched || Blueprint->UbergraphPages.IsEmpty()) return false;
	auto Pin = [](const TCHAR* Name) { auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("name"), Name);
		auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("bool")); Value->SetObjectField(TEXT("type"), Type); return MakeShared<FJsonValueObject>(Value); };
	auto Signature = MakeShared<FJsonObject>(); Signature->SetArrayField(TEXT("inputs"), {Pin(TEXT("Enabled"))}); Signature->SetArrayField(TEXT("outputs"), {Pin(TEXT("Succeeded"))});
	if (!ApplyFunctionGraphSignature(*Function, Signature, Error)) { AddError(Error.ToPublicDetail()); return false; }
	TArray<UK2Node_FunctionEntry*> Entries; Function->GetNodesOfClass(Entries);
	TArray<UK2Node_FunctionResult*> Results; Function->GetNodesOfClass(Results);
	if (Entries.Num() != 1 || Results.Num() != 1) return false;
	FBPVariableDescription Local; Local.VarName = TEXT("ScratchValue"); Local.VarGuid = FGuid::NewGuid(); Local.VarType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	Entries[0]->LocalVariables.Add(Local);
	FGraphNodeCreator<UK2Node_VariableGet> LocalCreator(*Function); auto* LocalNode = LocalCreator.CreateNode();
	LocalNode->VariableReference.SetLocalMember(Local.VarName, Function->GetName(), Local.VarGuid); LocalCreator.Finalize();
	const UEdGraphSchema* Schema = Function->GetSchema();
	if (!Schema->TryCreateConnection(Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then), Results[0]->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Entries[0]->FindPin(TEXT("Enabled")), Results[0]->FindPin(TEXT("Succeeded")))) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	// Local scope is first materialized by compilation; allocate its real pin
	// before freezing the fixture, rather than letting first load complete it.
	LocalNode->ReconstructNode();
	if (!TestNotNull(TEXT("fixture local variable has its native value pin"), LocalNode->FindPin(Local.VarName))) return false;
	FGraphNodeCreator<UK2Node_CallFunction> CallerCreator(*Blueprint->UbergraphPages[0]); auto* Caller = CallerCreator.CreateNode();
	Caller->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("ComputeStatus"))); CallerCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Blueprint->UbergraphPages[0]); auto* Event = EventCreator.CreateNode();
	Event->CustomFunctionName = TEXT("InvokeStatus"); EventCreator.Finalize();
	if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Caller->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FGuid GraphId = Function->GraphGuid, CallerId = Caller->NodeGuid, LocalId = LocalNode->NodeGuid;
	const auto Before = CaptureIdentity(*Blueprint, GraphId);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Save)) return false;
	Blueprint->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	if (!TestTrue(TEXT("fixture has real caller and local function scope"), ReadNames(*Blueprint, TEXT("ComputeStatus"), GraphId, CallerId, LocalId))) return false;

	auto RejectBeforeWrite = [&](const TSharedRef<FJsonObject>& Extension, const FString& Code) {
		int32 Modified = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			if (Object && OwnedObjects.ContainsByPredicate([&](const FString& Path) { return Object->GetOutermost()->GetName() == FPackageName::ObjectPathToPackageName(Path); })) ++Modified;
		});
		FAssetApplyRequest Request; Request.Operation = UeAgentApplyInputContract::EOperation::Update;
		Request.AssetPath = PackageName; Request.ProviderExtension = Extension;
		FAssetApplyResult Result; const bool Applied = ApplyAsset(Request, Result, TEXT("graph-rename-prewrite"));
		FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		TArray<uint8> Disk;
		return TestTrue(TEXT("rename failure rejects before first modification without relying on rollback"), !Applied && Modified == 0
			&& Result.Diagnostic.PublicMessage.Contains(Code) && !Result.bRollbackAttempted
			&& ReadNames(*Blueprint, TEXT("ComputeStatus"), GraphId, CallerId, LocalId)
			&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, GraphId))
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk && !Blueprint->GetOutermost()->IsDirty());
	};
	if (!RejectBeforeWrite(RenameExtension(GraphId, TEXT("Untouched")), TEXT("identity_conflict"))) return false;
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	UBlueprint* External = NewBlueprint(ExternalPackage, AActor::StaticClass());
	if (!TestNotNull(TEXT("external dependency fixture"), External)) return false;
	OwnedObjects.Add(ExternalPackage + TEXT(".BP_External"));
	FEdGraphPinType RefType; RefType.PinCategory = UEdGraphSchema_K2::PC_Object; RefType.PinSubCategoryObject = Blueprint->GeneratedClass;
	FBlueprintEditorUtils::AddMemberVariable(External, TEXT("UnrelatedDependency"), RefType);
	FKismetEditorUtilities::CompileBlueprint(External); External->GetOutermost()->SetDirtyFlag(false);
	if (!TestTrue(TEXT("an unrelated dependency is not an external rename write"), FPreparedBlueprintMutation::ValidateGraphNames(*Blueprint, RenameExtension(GraphId, TEXT("EvaluateStatus")), Error))) { AddError(Error.ToPublicDetail()); return false; }
	FGraphNodeCreator<UK2Node_CallFunction> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("ComputeStatus"))); ExternalCreator.Finalize();
	ExternalCall->FunctionReference.SetExternalMember(TEXT("ComputeStatus"), Blueprint->GeneratedClass);
	External->GetOutermost()->SetDirtyFlag(false);
	if (!RejectBeforeWrite(RenameExtension(GraphId, TEXT("EvaluateStatus")), TEXT("rename_external_impact"))) return false;
	TestEqual(TEXT("external caller is unchanged at rejection"), ExternalCall->FunctionReference.GetMemberName(), FName(TEXT("ComputeStatus")));
	TestFalse(TEXT("external caller package remains clean"), External->GetOutermost()->IsDirty());
	if (ObjectTools::DeleteObjects({External}, false) != 1) return false;
	OwnedObjects.Pop(); External = nullptr; ExternalCall = nullptr;
	const FString ChildPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_Child");
	UBlueprint* Child = NewBlueprint(ChildPackage, Blueprint->GeneratedClass); OwnedObjects.Add(ChildPackage + TEXT(".BP_Child"));
	if (!TestNotNull(TEXT("child collision fixture"), Child)) return false;
	FEdGraphPinType BoolType; BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Child, TEXT("EvaluateStatus"), BoolType); Child->GetOutermost()->SetDirtyFlag(false);
	if (!RejectBeforeWrite(RenameExtension(GraphId, TEXT("EvaluateStatus")), TEXT("rename_external_impact"))) return false;
	TestEqual(TEXT("child collision variable not natively renamed before rejection"), Child->NewVariables[0].VarName, FName(TEXT("EvaluateStatus")));
	if (ObjectTools::DeleteObjects({Child}, false) != 1) return false;
	OwnedObjects.Pop(); Child = nullptr;

	// The late oracle first observes the published/reloaded new graph and caller.
	// Original dirty memory differs from disk, so each recovery layer is independently checked.
	const bool DiskWarning = Blueprint->bRunConstructionScriptOnDrag;
	Blueprint->bRunConstructionScriptOnDrag = !DiskWarning; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> BeforeReload = Blueprint;
	bool Published = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update;
	Late.AssetPath = PackageName; Late.ProviderExtension = RenameExtension(GraphId, TEXT("EvaluateStatus"));
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		const bool Replaced = !BeforeReload.IsValid(), Clean = Current && !Current->GetOutermost()->IsDirty();
		const bool Names = Current && ReadNames(*Current, TEXT("EvaluateStatus"), GraphId, CallerId, LocalId);
		const auto After = Current ? CaptureIdentity(*Current, GraphId) : TMap<FString, FString>();
		const bool Identities = Before.OrderIndependentCompareEqual(After);
		const bool Written = FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("late observation: replaced=%d clean=%d names=%d identities=%d written=%d"), Replaced, Clean, Names, Identities, Written));
		if (!Identities)
		{
			int32 Shown = 0;
			for (const auto& Pair : Before) if (!After.Contains(Pair.Key) || After[Pair.Key] != Pair.Value)
			{
				AddInfo(TEXT("identity state: ") + Pair.Value + TEXT(" -> ") + (After.Contains(Pair.Key) ? After[Pair.Key] : TEXT("missing")));
				if (++Shown == 8) break;
			}
			AddInfo(FString::Printf(TEXT("identity record counts before=%d after=%d"), Before.Num(), After.Num()));
			for (const auto& Pair : After) if (!Before.Contains(Pair.Key))
			{
				AddInfo(TEXT("identity added: ") + Pair.Value);
				if (++Shown == 8) break;
			}
		}
		if (Current && !Names)
		{
			TArray<UEdGraph*> ActualGraphs; Current->GetAllGraphs(ActualGraphs);
			for (UEdGraph* ActualGraph : ActualGraphs) for (UEdGraphNode* Node : ActualGraph->Nodes)
			{
				if (auto* Call = Cast<UK2Node_CallFunction>(Node); Call && Node->NodeGuid == CallerId)
					AddInfo(TEXT("caller name=") + Call->FunctionReference.GetMemberName().ToString() + TEXT(" target=") + GetNameSafe(Call->GetTargetFunction()));
				if (auto* Variable = Cast<UK2Node_VariableGet>(Node); Variable && Node->NodeGuid == LocalId)
					AddInfo(TEXT("local scope=") + Variable->VariableReference.GetMemberScopeName());
			}
		}
		Published = Replaced && Clean && Names && Identities && Written;
		TestTrue(TEXT("late rejection first observes saved reloaded rename and caller identities"), Published);
		Failure.Diagnostic.Code = TEXT("test_graph_rename_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Function = nullptr; Untouched = nullptr; Caller = nullptr; LocalNode = nullptr; Event = nullptr; Entries.Reset(); Results.Reset(); Blueprint = nullptr;
	FAssetApplyResult LateResult;
	TestFalse(TEXT("late rejection returns failure"), ApplyAsset(Late, LateResult, TEXT("graph-rename-late")));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RestoredDisk;
	if (!TestTrue(TEXT("late recovery restores original dirty memory and original disk with caller references"), Published
		&& LateResult.Diagnostic.Code == TEXT("test_graph_rename_late_rejection") && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded
		&& Blueprint && Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskWarning
		&& ReadNames(*Blueprint, TEXT("ComputeStatus"), GraphId, CallerId, LocalId)
		&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, GraphId))
		&& FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk)) { AddError(LateResult.Diagnostic.Code + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskWarning; Blueprint->GetOutermost()->SetDirtyFlag(false);

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("graph-rename-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Declaration = [&](const FString& Path, const bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (!(*Declarations)->TryGetObjectField(Edit ? TEXT("ComputeStatus") : TEXT("EvaluateStatus"), Row)) return false;
			++Matches; if ((*Row)->HasField(TEXT("name"))) return false;
			if (Edit) { (*Row)->SetStringField(TEXT("name"), TEXT("EvaluateStatus"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false; }
		}
		return Matches == 1;
	};
	if (!Export(Workspace / TEXT("edit")) || !TestTrue(TEXT("only visible declaration optional name is edited"), Declaration(Workspace / TEXT("edit"), true))) { AddError(Error.ToPublicDetail()); return false; }
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
	auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
	BeforeReload = Blueprint; Blueprint = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("graph-rename-import"));
	if (!TestTrue(TEXT("public rename import succeeds"), Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("public rename compiles saves reloads all identities signature links local scope and caller"), !BeforeReload.IsValid()
		&& Blueprint && !Blueprint->GetOutermost()->IsDirty() && ReadNames(*Blueprint, TEXT("EvaluateStatus"), GraphId, CallerId, LocalId)
		&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, GraphId)))) return false;
	TestTrue(TEXT("independent public export emits new declaration key without duplicate name"), Export(Workspace / TEXT("readback")) && Declaration(Workspace / TEXT("readback"), false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintCustomEventRenameNativeTest,
	"UeAgentInterface.T208.Blueprint.CustomEventRenameNative", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintCustomEventRenameNativeTest::RunTest(const FString&)
{
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85CustomEventRename_") + Token + TEXT("/BP_Owner");
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("native CustomEvent fixture"), Blueprint)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	UBlueprint* External = nullptr;
	ON_SCOPE_EXIT {
		if (External) { External->GetOutermost()->SetDirtyFlag(false); TestEqual(TEXT("owned external CustomEvent fixture cleanup"), ObjectTools::DeleteObjects({External}, false), 1); }
		Blueprint->GetOutermost()->SetDirtyFlag(false);
		TestEqual(TEXT("owned CustomEvent fixture cleanup"), ObjectTools::DeleteObjects({Blueprint}, false), 1);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph); auto* Event = EventCreator.CreateNode();
	Event->CustomFunctionName = TEXT("HandleStatus"); EventCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph); auto* Call = CallCreator.CreateNode();
	Call->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("HandleStatus"))); CallCreator.Finalize();
	FGraphNodeCreator<UK2Node_CreateDelegate> BindCreator(*Graph); auto* Bind = BindCreator.CreateNode(); BindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> TimerCreator(*Graph); auto* Timer = TimerCreator.CreateNode();
	Timer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate"))); TimerCreator.Finalize();
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Bind->GetDelegateOutPin(), Timer->FindPin(TEXT("Delegate")))) return false;
	Bind->SetFunction(TEXT("HandleStatus"));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
	AddInfo(FString::Printf(TEXT("native before: call_guid=%d bind_guid=%d"), Call->FunctionReference.GetMemberGuid() == Event->NodeGuid, Bind->SelectedFunctionGuid == Event->NodeGuid));
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	FAssetRegistryModule::AssetCreated(External);
	FGraphNodeCreator<UK2Node_CallFunction> ExternalCallCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCallCreator.CreateNode();
	ExternalCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("HandleStatus"))); ExternalCallCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	FGraphNodeCreator<UK2Node_CreateDelegate> ExternalBindCreator(*External->UbergraphPages[0]); auto* ExternalBind = ExternalBindCreator.CreateNode(); ExternalBindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> ExternalTimerCreator(*External->UbergraphPages[0]); auto* ExternalTimer = ExternalTimerCreator.CreateNode();
	ExternalTimer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate"))); ExternalTimerCreator.Finalize();
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(ExternalBind->GetDelegateOutPin(), ExternalTimer->FindPin(TEXT("Delegate")))) return false;
	FEdGraphPinType ExternalType; ExternalType.PinCategory = UEdGraphSchema_K2::PC_Object; ExternalType.PinSubCategoryObject = Blueprint->GeneratedClass;
	FBlueprintEditorUtils::AddMemberVariable(External, TEXT("SourceObject"), ExternalType);
	FGraphNodeCreator<UK2Node_VariableGet> ExternalObjectCreator(*External->UbergraphPages[0]); auto* ExternalObject = ExternalObjectCreator.CreateNode();
	ExternalObject->VariableReference.SetSelfMember(TEXT("SourceObject")); ExternalObjectCreator.Finalize(); ExternalObject->ReconstructNode();
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(ExternalObject->FindPin(TEXT("SourceObject")), ExternalBind->GetObjectInPin())) return false;
	ExternalBind->SetFunction(TEXT("HandleStatus"));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External); FKismetEditorUtilities::CompileBlueprint(External);
	if (!TestTrue(TEXT("native external binding resolves the source event identity"), ExternalBind->SelectedFunctionGuid == Event->NodeGuid
		&& UBlueprint::GetBlueprintFromClass(ExternalBind->GetScopeClass()) == Blueprint)) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> ExternalCallBefore, ExternalBindBefore;
	FObjectWriter(ExternalCall, ExternalCallBefore); FObjectWriter(ExternalBind, ExternalBindBefore);
	int32 ExternalModifications = 0;
	const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) { if (Object && Object->GetOutermost() == External->GetOutermost()) ++ExternalModifications; });
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	Event->Modify(); Event->OnRenameNode(TEXT("ReceiveStatus"));
	AddInfo(FString::Printf(TEXT("native skeleton: call=%s bind=%s"), *Call->FunctionReference.GetMemberName().ToString(), *Bind->GetFunctionName().ToString()));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
	TArray<uint8> ExternalCallAfter, ExternalBindAfter;
	FObjectWriter(ExternalCall, ExternalCallAfter); FObjectWriter(ExternalBind, ExternalBindAfter);
	AddInfo(FString::Printf(TEXT("native external full: call=%s bind=%s modifications=%d dirty=%d call_changed=%d bind_changed=%d"),
		*ExternalCall->FunctionReference.GetMemberName().ToString(), *ExternalBind->GetFunctionName().ToString(), ExternalModifications, External->GetOutermost()->IsDirty(),
		ExternalCallBefore != ExternalCallAfter, ExternalBindBefore != ExternalBindAfter));
	AddInfo(FString::Printf(TEXT("native full: call=%s bind=%s status=%d identities=%d"), *Call->FunctionReference.GetMemberName().ToString(), *Bind->GetFunctionName().ToString(), Blueprint->Status,
		Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))));
	TestTrue(TEXT("native CustomEvent rename resolves actual call and binding"), Call->FunctionReference.GetMemberName() == TEXT("ReceiveStatus") && Bind->GetFunctionName() == TEXT("ReceiveStatus") && Blueprint->Status != BS_Error);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintCustomEventRemovalTest,
	"UeAgentInterface.T208.Blueprint.CustomEventRemovalWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintCustomEventRemovalTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85CustomEventRemoval_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-custom-event-removal/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		for (const FString& Path : Owned) if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) {
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned event removal cleanup"), ObjectTools::ForceDeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto Save = [&]() {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool Saved = Blueprint->Status != BS_Error && UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args);
		if (Saved) Blueprint->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph); auto* Event = EventCreator.CreateNode();
	Event->CustomFunctionName = TEXT("DoomedEvent"); EventCreator.Finalize();
	// Force the producer removal key before its callers: kinds must be resolved before any removal.
	Event->NodeGuid = FGuid(1, 0, 0, 0);
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save()) return false;
	FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph); auto* Call = CallCreator.CreateNode();
	Call->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("DoomedEvent"))); CallCreator.Finalize(); Call->NodeGuid = FGuid(2, 0, 0, 0);
	FGraphNodeCreator<UK2Node_CreateDelegate> BindCreator(*Graph); auto* Bind = BindCreator.CreateNode(); BindCreator.Finalize(); Bind->NodeGuid = FGuid(3, 0, 0, 0);
	FGraphNodeCreator<UK2Node_CallFunction> TimerCreator(*Graph); auto* Timer = TimerCreator.CreateNode();
	Timer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate"))); TimerCreator.Finalize(); Timer->NodeGuid = FGuid(4, 0, 0, 0);
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema->TryCreateConnection(Bind->GetDelegateOutPin(), Timer->FindPin(TEXT("Delegate")))
		|| !Schema->TryCreateConnection(Call->FindPin(UEdGraphSchema_K2::PN_Then), Timer->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
	Bind->SetFunction(TEXT("DoomedEvent"));
	Schema->TrySetDefaultValue(*Timer->FindPin(TEXT("Time")), TEXT("0.5"));
	if (!Save()) return false;
	const FGuid GraphId = Graph->GraphGuid, EventId = Event->NodeGuid, CallId = Call->NodeGuid, BindId = Bind->NodeGuid, TimerId = Timer->NodeGuid;
	const TSet<FGuid> Removed{EventId, CallId, BindId, TimerId};
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	auto Expected = Before;
	for (auto It = Expected.CreateIterator(); It; ++It)
		for (const FGuid& Id : Removed) if (It.Key().StartsWith(GraphId.ToString() + TEXT("/") + Id.ToString())) { It.RemoveCurrent(); break; }
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto References = [&](UBlueprint* BP) {
		if (!BP || BP->Status == BS_Error || !Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		bool Definition = false, Caller = false, Binding = false;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes) {
			if (auto* E = Cast<UK2Node_CustomEvent>(N); E && E->NodeGuid == EventId) Definition = E->CustomFunctionName == TEXT("DoomedEvent");
			if (auto* C = Cast<UK2Node_CallFunction>(N); C && C->NodeGuid == CallId) {
				FMemberReference Ref = C->FunctionReference; const UFunction* F = Ref.ResolveMember<UFunction>(C->GetBlueprintClassFromNode());
				Caller = F && F->GetFName() == TEXT("DoomedEvent") && UBlueprint::GetBlueprintFromClass(F->GetOwnerClass()) == BP && Ref.GetMemberGuid() == EventId;
			}
			if (auto* B = Cast<UK2Node_CreateDelegate>(N); B && B->NodeGuid == BindId) Binding = B->GetFunctionName() == TEXT("DoomedEvent") && B->SelectedFunctionGuid == EventId;
		}
		return Definition && Caller && Binding;
	};
	auto Absent = [&](UBlueprint* BP) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass || BP->GeneratedClass->FindFunctionByName(TEXT("DoomedEvent"))) return false;
		return Expected.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()));
	};
	if (!TestTrue(TEXT("native event call binding fixture"), References(Blueprint))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("event-removal-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("event-removal-import"));
	};
	// Only visible semantic fields select nodes. Hidden node identity stays with the workspace owner.
	auto Omit = [&](const FString& Path, const int32 Mode) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			if (!Doc->HasField(TEXT("interface_ref"))) continue;
			const auto Nodes = Doc->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
			for (const auto& Pair : Nodes->Values) {
				const auto Node = Pair.Value->AsObject(); FString Name, Kind, Class;
				Node->TryGetStringField(TEXT("node_type"), Kind);
				Node->TryGetStringField(TEXT("node_class"), Class);
				const bool Definition = Node->TryGetStringField(TEXT("event_name"), Name) && Name == TEXT("DoomedEvent");
				const bool Caller = Node->TryGetStringField(TEXT("custom_event"), Name) && Name == TEXT("DoomedEvent");
				if (Definition || (Mode >= 1 && Caller) || (Mode == 2 && (Class == UK2Node_CreateDelegate::StaticClass()->GetPathName() || Kind == TEXT("call_function")))) Deleted.Add(Pair.Key);
			}
			Count += Deleted.Num(); for (const FString& Key : Deleted) Nodes->RemoveField(Key);
			if (Deleted.IsEmpty()) continue;
			auto Connections = Doc->GetArrayField(TEXT("connections"));
			Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) { const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); }); });
			Doc->SetArrayField(TEXT("connections"), Connections);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		return Count == (Mode == 0 ? 1 : Mode == 1 ? 2 : 4);
	};
	for (int32 Mode = 0; Mode != 2; ++Mode) {
		const FString Path = Workspace / FString::Printf(TEXT("reject-%d"), Mode);
		if (!Export(Path) || !Omit(Path, Mode)) { AddError(Error.ToPublicDetail()); return false; }
		TArray<uint8> RawBefore, RawAfter; FObjectWriter(Blueprint, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle); FObjectWriter(Blueprint, RawAfter);
		TArray<uint8> Disk; const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		const bool Typed = Result.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"));
		AddInfo(FString::Printf(TEXT("event removal retained %s: rejected=%d typed=%d modifications=%d unchanged=%d"), Mode == 0 ? TEXT("call") : TEXT("binding"), !Result.IsOk(), Typed, Modified, Unchanged));
		if (!TestTrue(TEXT("retained consumer prewrite rejection"), !Result.IsOk() && Typed && Modified == 0 && Unchanged)) { AddError(Result.GetErrorJson()); return false; }
	}
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	const FString ExternalPath = ExternalPackage + TEXT(".BP_External"); Owned.Insert(ExternalPath, 0);
	const FString ExternalFile = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	UBlueprint* External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!External) return false;
	FAssetRegistryModule::AssetCreated(External);
	FGraphNodeCreator<UK2Node_CallFunction> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("DoomedEvent"))); ExternalCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External); FKismetEditorUtilities::CompileBlueprint(External);
	FSavePackageArgs ExternalArgs; ExternalArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (External->Status == BS_Error || !UPackage::SavePackage(External->GetOutermost(), External, *ExternalFile, ExternalArgs)) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFile)) return false;
	auto RejectExternal = [&](const FString& Label, const FString& Code) {
		const FString Path = Workspace / Label; if (!Export(Path) || !Omit(Path, 2)) return false;
		TArray<uint8> RawBefore, RawAfter; if (ExternalCall) FObjectWriter(ExternalCall, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) {
			if (O && (O->GetOutermost() == Blueprint->GetOutermost() || O->GetOutermost()->GetName() == ExternalPackage)) ++Modified;
		});
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (Result.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("external removal unexpectedly accepted")); return false; }
		if (ExternalCall) FObjectWriter(ExternalCall, RawAfter);
		TArray<uint8> Disk, OutsideDisk;
		const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter
			&& (!External || !External->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk
			&& FFileHelper::LoadFileToArray(OutsideDisk, *ExternalFile) && OutsideDisk == ExternalDisk;
		const bool Typed = Result.GetErrorJson().Contains(Code);
		AddInfo(FString::Printf(TEXT("event removal %s: typed=%d modifications=%d source_external_unchanged=%d"), *Label, Typed, Modified, Unchanged));
		if (!Typed) AddError(Result.GetErrorJson()); return Typed && Modified == 0 && Unchanged;
	};
	if (!RejectExternal(TEXT("external-loaded"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({ExternalFile}, true);
	UPackage* ExternalToUnload = External->GetOutermost(); External = nullptr; ExternalCall = nullptr;
	if (!UPackageTools::UnloadPackages({ExternalToUnload}) || FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;
	if (!RejectExternal(TEXT("external-unloaded"), TEXT("blueprint_graph_remove_references_unresolved"))
		|| !TestNull(TEXT("unresolved query did not load external"), FindObject<UBlueprint>(nullptr, *ExternalPath))) return false;
	auto LoadArgs = MakeShared<FJsonObject>(); LoadArgs->SetStringField(TEXT("target"), ExternalPath);
	LoadArgs->SetStringField(TEXT("workspace_path"), Workspace / TEXT("explicit-external-load")); LoadArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Loaded = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), LoadArgs,
		TEXT("event-removal-explicit-load"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Loaded.IsOk()) { AddError(Loaded.GetErrorJson()); return false; }
	External = FindObject<UBlueprint>(nullptr, *ExternalPath);
	if (!External) return false;
	for (UEdGraphNode* N : External->UbergraphPages[0]->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N); C && C->GetFunctionName() == TEXT("DoomedEvent")) ExternalCall = C;
	if (!ExternalCall || !RejectExternal(TEXT("external-loaded-retry"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	ExternalCall = nullptr; External->GetOutermost()->SetDirtyFlag(false);
	if (ObjectTools::ForceDeleteObjects({External}, false) != 1) return false;
	External = nullptr;
	auto NodeSpecs = MakeShared<FJsonObject>();
	for (const FGuid& Id : Removed) { auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("operation"), TEXT("remove")); NodeSpecs->SetObjectField(Id.ToString(EGuidFormats::DigitsWithHyphens), Spec); }
	auto GraphSpec = MakeShared<FJsonObject>(); GraphSpec->SetStringField(TEXT("operation"), TEXT("update")); GraphSpec->SetObjectField(TEXT("nodes"), NodeSpecs);
	auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(GraphId.ToString(EGuidFormats::DigitsWithHyphens), GraphSpec);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs); Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Old = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Witness = !Old.IsValid() && Absent(Current) && !Current->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("event removal late witness: replaced_saved_deleted_non_targets=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_custom_event_removal_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Event = nullptr; Call = nullptr; Bind = nullptr; Timer = nullptr; Graph = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("event-removal-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	const bool Recovered = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && References(Blueprint)
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("event removal recovery: original_memory_disk_dirty_pin_references=%d"), Recovered));
	if (!TestTrue(TEXT("late deletion restores native identities references memory disk dirty"), Recovered)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString SuccessPath = Workspace / TEXT("success");
	if (!Export(SuccessPath) || !Omit(SuccessPath, 2)) { AddError(Error.ToPublicDetail()); return false; }
	Old = Blueprint; Blueprint = nullptr; const auto Result = Import(SuccessPath);
	if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const FString ReadbackPath = Workspace / TEXT("readback");
	bool ExportAbsent = Export(ReadbackPath);
	TArray<FString> ReadbackFiles; IFileManager::Get().FindFilesRecursive(ReadbackFiles, *ReadbackPath, TEXT("*.hjson"), true, false);
	for (const FString& File : ReadbackFiles) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		ExportAbsent &= !Text.Contains(TEXT("DoomedEvent")) && !Text.Contains(TEXT("K2_SetTimerDelegate")) && !Text.Contains(TEXT("K2Node_CreateDelegate"));
	}
	const bool Success = !Old.IsValid() && Absent(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && ExportAbsent;
	AddInfo(FString::Printf(TEXT("event removal public: replaced_clean_deleted_non_targets_independent_export=%d"), Success));
	return TestTrue(TEXT("public atomic event and explicit consumer omission"), Success);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintCustomEventRenameTest,
	"UeAgentInterface.T208.Blueprint.CustomEventRenameWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintCustomEventRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = TEXT("/Game/UAI/Transient/T85CustomEventRename_") + Token;
	const FString PackageName = Root / TEXT("BP_Owner"), ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-custom-event-rename/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		TArray<UObject*> Objects;
		for (const FString& Path : Owned) if (auto* Object = LoadObject<UObject>(nullptr, *Path)) { Object->GetOutermost()->SetDirtyFlag(false); Objects.Add(Object); }
		TestEqual(TEXT("owned CustomEvent fixtures cleanup"), ObjectTools::ForceDeleteObjects(Objects, false), Objects.Num());
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto Create = [&](const FString& Name) {
		auto* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*(Root / Name)), *Name,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) FAssetRegistryModule::AssetCreated(BP); return BP;
	};
	auto Save = [&](UBlueprint* BP) {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP); FKismetEditorUtilities::CompileBlueprint(BP);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		const bool Saved = BP->Status != BS_Error && UPackage::SavePackage(BP->GetOutermost(), BP, *File, Args);
		if (Saved) BP->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	UBlueprint* Blueprint = Create(TEXT("BP_Owner"));
	if (!Blueprint) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph); auto* Event = EventCreator.CreateNode();
	Event->CustomFunctionName = TEXT("HandleStatus"); EventCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*Graph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("InvokeStatus"); InvokeCreator.Finalize();
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save(Blueprint)) return false;
	FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph); auto* Call = CallCreator.CreateNode();
	Call->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("HandleStatus"))); CallCreator.Finalize();
	FGraphNodeCreator<UK2Node_CreateDelegate> BindCreator(*Graph); auto* Bind = BindCreator.CreateNode(); BindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> TimerCreator(*Graph); auto* Timer = TimerCreator.CreateNode();
	Timer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate"))); TimerCreator.Finalize();
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema->TryCreateConnection(Bind->GetDelegateOutPin(), Timer->FindPin(TEXT("Delegate")))
		|| !Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Call->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Call->FindPin(UEdGraphSchema_K2::PN_Then), Timer->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
	Bind->SetFunction(TEXT("HandleStatus"));
	Schema->TrySetDefaultValue(*Timer->FindPin(TEXT("Time")), TEXT("0.5"));
	if (!Save(Blueprint)) return false;
	const FGuid EventId = Event->NodeGuid, CallId = Call->NodeGuid, BindId = Bind->NodeGuid, GraphId = Graph->GraphGuid;
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	auto OriginalSignature = MakeShared<FJsonObject>(); FString SignatureText;
	if (!CaptureCustomEventSignature(*Event, OriginalSignature, Error) || !CommandContract::Json::TryCanonicalize(OriginalSignature, SignatureText, Error)) return false;
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Read = [&](UBlueprint* BP, const FName Name) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass || !Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		bool Definition = false, Caller = false, Binding = false;
		TArray<UEdGraph*> Graphs; BP->GetAllGraphs(Graphs);
		for (UEdGraph* G : Graphs) for (UEdGraphNode* Node : G->Nodes)
		{
			if (auto* E = Cast<UK2Node_CustomEvent>(Node); E && E->NodeGuid == EventId) {
				auto S = MakeShared<FJsonObject>(); FString Text;
				Definition = E->CustomFunctionName == Name && CaptureCustomEventSignature(*E, S, Error)
					&& CommandContract::Json::TryCanonicalize(S, Text, Error) && Text == SignatureText;
			}
			if (auto* C = Cast<UK2Node_CallFunction>(Node); C && C->NodeGuid == CallId) {
				FMemberReference Reference = C->FunctionReference;
				const UFunction* Function = Reference.ResolveMember<UFunction>(C->GetBlueprintClassFromNode());
				Caller = C->FunctionReference.GetMemberName() == Name && C->FunctionReference.GetMemberGuid() == EventId
					&& Function && Function->GetFName() == Name && UBlueprint::GetBlueprintFromClass(Function->GetOwnerClass()) == BP;
			}
			if (auto* B = Cast<UK2Node_CreateDelegate>(Node); B && B->NodeGuid == BindId)
				Binding = B->GetFunctionName() == Name && B->SelectedFunctionGuid == EventId && B->GetScopeClass()
					&& B->GetScopeClass()->FindFunctionByName(Name) && UBlueprint::GetBlueprintFromClass(B->GetScopeClass()) == BP;
		}
		return Definition && Caller && Binding;
	};
	if (!TestTrue(TEXT("fixture native function identities signature and real references"), Read(Blueprint, TEXT("HandleStatus")))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("event-rename-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto NameInWorkspace = [&](const FString& Path, const FString& Name, const bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			bool Changed = false;
			TFunction<void(const TSharedPtr<FJsonObject>&)> Visit = [&](const TSharedPtr<FJsonObject>& O) {
				FString Current;
				if (O->TryGetStringField(TEXT("event_name"), Current) && Current == (Edit ? TEXT("HandleStatus") : Name)) {
					++Matches;
					if (Edit) { O->SetStringField(TEXT("event_name"), Name); O->RemoveField(TEXT("signature")); Changed = true; }
				}
				for (const auto& Pair : O->Values) if (Pair.Value->Type == EJson::Object) Visit(Pair.Value->AsObject());
			};
			Visit(Doc);
			if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		return Matches == 1;
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("event-rename-import"));
	};
	auto Reject = [&](const FString& Label, const FString& Name, const FString& Code, UBlueprint* External) {
		const FString Path = Workspace / Label;
		if (!Export(Path) || !NameInWorkspace(Path, Name, true)) { AddError(Error.ToPublicDetail()); return false; }
		TArray<uint8> ExternalBefore, ExternalAfter; if (External) FObjectWriter(External, ExternalBefore);
		TArray<uint8> ExternalDiskBefore, ExternalDiskAfter;
		const FString ExternalFile = External ? FPackageName::LongPackageNameToFilename(External->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()) : FString();
		if (External && !FFileHelper::LoadFileToArray(ExternalDiskBefore, *ExternalFile)) return false;
		TArray<TArray<uint8>> NodesBefore, NodesAfter;
		if (External) for (UEdGraphNode* N : External->UbergraphPages[0]->Nodes) { TArray<uint8> Bytes; FObjectWriter(N, Bytes); NodesBefore.Add(Bytes); }
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && (O->GetOutermost() == Blueprint->GetOutermost() || (External && O->GetOutermost() == External->GetOutermost()))) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (External) { FObjectWriter(External, ExternalAfter); for (UEdGraphNode* N : External->UbergraphPages[0]->Nodes) { TArray<uint8> Bytes; FObjectWriter(N, Bytes); NodesAfter.Add(Bytes); } }
		TArray<uint8> Disk;
		const bool Unchanged = Read(Blueprint, TEXT("HandleStatus")) && !Blueprint->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk && ExternalBefore == ExternalAfter && NodesBefore == NodesAfter
			&& (!External || (!External->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(ExternalDiskAfter, *ExternalFile) && ExternalDiskBefore == ExternalDiskAfter));
		AddInfo(FString::Printf(TEXT("event prewrite %s: rejected=%d typed=%d modifications=%d unchanged=%d"), *Label, !Result.IsOk(), Result.GetErrorJson().Contains(Code), Modified, Unchanged));
		if (Result.IsOk() || !Result.GetErrorJson().Contains(Code)) AddError(Result.GetErrorJson());
		return !Result.IsOk() && Result.GetErrorJson().Contains(Code) && Modified == 0 && Unchanged;
	};
	if (!Reject(TEXT("conflict"), TEXT("Untouched"), TEXT("blueprint_custom_event_rename_name_invalid"), nullptr)) return false;
	if (!Reject(TEXT("inherited-native-name"), TEXT("ReceiveTick"), TEXT("blueprint_custom_event_rename_name_invalid"), nullptr)) return false;
	UBlueprint* External = Create(TEXT("BP_External")); Owned.Add(External->GetPathName());
	FGraphNodeCreator<UK2Node_CallFunction> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("HandleStatus"))); ExternalCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	if (!Save(External) || !Reject(TEXT("external"), TEXT("ReceiveStatus"), TEXT("blueprint_custom_event_rename_external_impact"), External)) return false;
	ExternalCall->DestroyNode(); ExternalCall = nullptr;
	FGraphNodeCreator<UK2Node_CreateDelegate> ExternalBindCreator(*External->UbergraphPages[0]); auto* ExternalBind = ExternalBindCreator.CreateNode(); ExternalBindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> ExternalTimerCreator(*External->UbergraphPages[0]); auto* ExternalTimer = ExternalTimerCreator.CreateNode();
	ExternalTimer->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate"))); ExternalTimerCreator.Finalize();
	if (!Schema->TryCreateConnection(ExternalBind->GetDelegateOutPin(), ExternalTimer->FindPin(TEXT("Delegate")))) return false;
	FEdGraphPinType ExternalType; ExternalType.PinCategory = UEdGraphSchema_K2::PC_Object; ExternalType.PinSubCategoryObject = Blueprint->GeneratedClass;
	FBlueprintEditorUtils::AddMemberVariable(External, TEXT("SourceObject"), ExternalType);
	FGraphNodeCreator<UK2Node_VariableGet> ExternalObjectCreator(*External->UbergraphPages[0]); auto* ExternalObject = ExternalObjectCreator.CreateNode();
	ExternalObject->VariableReference.SetSelfMember(TEXT("SourceObject")); ExternalObjectCreator.Finalize(); ExternalObject->ReconstructNode();
	if (!Schema->TryCreateConnection(ExternalObject->FindPin(TEXT("SourceObject")), ExternalBind->GetObjectInPin())) return false;
	ExternalBind->SetFunction(TEXT("HandleStatus"));
	if (!Save(External) || !TestTrue(TEXT("external binding uses native linked source scope and event GUID"), ExternalBind->SelectedFunctionGuid == EventId
		&& UBlueprint::GetBlueprintFromClass(ExternalBind->GetScopeClass()) == Blueprint)) return false;
	if (!Reject(TEXT("external-binding-persistent-reload"), TEXT("ReceiveStatus"), TEXT("blueprint_custom_event_rename_external_impact"), External)) return false;
	ExternalBind->DestroyNode(); ExternalTimer->DestroyNode(); ExternalBind = nullptr; ExternalTimer = nullptr;
	if (!Save(External)) return false;
	const FString ExternalPath = External->GetPathName();
	const FString ExternalFile = FPackageName::LongPackageNameToFilename(External->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFile)) return false;
	const auto ExternalIdentity = CaptureIdentity(*External, FGuid());
	const bool ExternalDiskFlag = External->bRunConstructionScriptOnDrag;
	External->bRunConstructionScriptOnDrag = !ExternalDiskFlag; External->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> OriginalExternal = External;
	TWeakObjectPtr<UK2Node_VariableGet> OriginalDependencyNode = ExternalObject;
	auto SerializeExternal = [&]() {
		// Package replacement remaps native object pointers. Serialize their paths,
		// as persistent asset references, rather than process-specific addresses.
		TArray<TArray<uint8>> Result;
		TArray<UObject*> Objects{External};
		TArray<UEdGraph*> Graphs; External->GetAllGraphs(Graphs);
		for (UEdGraph* G : Graphs) { Objects.Add(G); for (UEdGraphNode* N : G->Nodes) Objects.Add(N); }
		for (UObject* Object : Objects) {
			TArray<uint8> Bytes; FMemoryWriter Writer(Bytes, true); FObjectAndNameAsStringProxyArchive Archive(Writer, false);
			Object->Serialize(Archive); Result.Add(MoveTemp(Bytes));
		}
		return Result;
	};
	const auto ExternalSerialized = SerializeExternal();
	auto ReadExternal = [&](UBlueprint* Source, const FName EventName) {
		if (!OriginalExternal.IsValid() || !OriginalDependencyNode.IsValid()) { AddInfo(TEXT("external observation: original objects invalid")); return false; }
		const bool SameObject = FindObject<UBlueprint>(nullptr, *ExternalPath) == External;
		const bool Dirty = External->GetOutermost()->IsDirty(), Flag = External->bRunConstructionScriptOnDrag == !ExternalDiskFlag;
		const bool Identities = ExternalIdentity.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid()));
		TArray<uint8> BeforeQuery, AfterQuery, Disk; FObjectWriter(ExternalObject, BeforeQuery);
		FMemberReference Reference = ExternalObject->VariableReference;
		const FObjectProperty* Property = CastField<FObjectProperty>(Reference.ResolveMember<FProperty>(External->SkeletonGeneratedClass));
		const bool Target = Property && UBlueprint::GetBlueprintFromClass(Property->PropertyClass) == Source;
		FObjectWriter(ExternalObject, AfterQuery);
		const bool QueryUnchanged = BeforeQuery == AfterQuery, DiskUnchanged = FFileHelper::LoadFileToArray(Disk, *ExternalFile) && Disk == ExternalDisk;
		const bool Serialized = ExternalSerialized == SerializeExternal();
		AddInfo(FString::Printf(TEXT("unrelated external dependency %s: object=%d dirty=%d flag=%d identities=%d target=%d query=%d disk=%d serialized=%d"),
			*EventName.ToString(), SameObject, Dirty, Flag, Identities, Target, QueryUnchanged, DiskUnchanged, Serialized));
		return SameObject && Dirty && Flag && Identities && Target && QueryUnchanged && DiskUnchanged && Serialized;
	};
	auto Extension = [&]() {
		auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("operation"), TEXT("update")); Node->SetStringField(TEXT("node_type"), TEXT("custom_event"));
		Node->SetStringField(TEXT("node_guid"), EventId.ToString()); Node->SetStringField(TEXT("event_name"), TEXT("ReceiveStatus"));
		auto Nodes = MakeShared<FJsonObject>(); Nodes->SetObjectField(EventId.ToString(), Node);
		auto G = MakeShared<FJsonObject>(); G->SetStringField(TEXT("graph_guid"), GraphId.ToString()); G->SetStringField(TEXT("graph_kind"), TEXT("event_graph")); G->SetObjectField(TEXT("nodes"), Nodes);
		auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(GraphId.ToString(), G);
		auto Result = MakeShared<FJsonObject>(); Result->SetObjectField(TEXT("graphs"), Graphs); Result->SetBoolField(TEXT("compile"), true); Result->SetBoolField(TEXT("save"), true); return Result;
	};
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> OldObject = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension();
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		const bool Replaced = !OldObject.IsValid(), Clean = Current && !Current->GetOutermost()->IsDirty(), Source = Read(Current, TEXT("ReceiveStatus"));
		const bool Written = FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk, BindingPreserved = ReadExternal(Current, TEXT("ReceiveStatus"));
		AddInfo(FString::Printf(TEXT("late observation: replaced=%d clean=%d source=%d written=%d external=%d"), Replaced, Clean, Source, Written, BindingPreserved));
		Witness = Replaced && Clean && Source && Written && BindingPreserved;
		AddInfo(FString::Printf(TEXT("event late saved-reloaded name call binding identity witness=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_custom_event_rename_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Graph = nullptr; Event = nullptr; Invoke = nullptr; Call = nullptr; Bind = nullptr; Timer = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("event-rename-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RestoredDisk;
	const bool ExternalRestored = ReadExternal(Blueprint, TEXT("HandleStatus"));
	AddInfo(FString::Printf(TEXT("restore observation: attempted=%d succeeded=%d source=%d external=%d code=%s"), LateResult.bRollbackAttempted, LateResult.bRollbackSucceeded,
		Read(Blueprint, TEXT("HandleStatus")), ExternalRestored, *LateResult.Diagnostic.Code));
	const bool Restored = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded
		&& Blueprint && Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& Read(Blueprint, TEXT("HandleStatus")) && FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk
		&& ExternalRestored;
	AddInfo(FString::Printf(TEXT("event late original memory disk dirty references restored=%d"), Restored));
	if (!TestTrue(TEXT("event late observed published state before independent restoration"), Restored)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString EditPath = Workspace / TEXT("edit");
	if (!Export(EditPath) || !NameInWorkspace(EditPath, TEXT("ReceiveStatus"), true)) return false;
	OldObject = Blueprint; Blueprint = nullptr;
	const auto Imported = Import(EditPath);
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	const bool Readback = !OldObject.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && Read(Blueprint, TEXT("ReceiveStatus"))
		&& Export(Workspace / TEXT("readback")) && NameInWorkspace(Workspace / TEXT("readback"), TEXT("ReceiveStatus"), false)
		&& ReadExternal(Blueprint, TEXT("ReceiveStatus"));
	AddInfo(FString::Printf(TEXT("event public omitted signature rename saved-reloaded independent export identity references=%d"), Readback));
	AddInfo(FString::Printf(TEXT("event loaded unrelated dependency object pins native owner original dirty serialization and disk preserved=%d"), ReadExternal(Blueprint, TEXT("ReceiveStatus"))));
	return TestTrue(TEXT("public event_name-only rename preserves all native identities signature links and references"), Readback) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintDelegateRenameTest,
	"UeAgentInterface.T208.Blueprint.DelegateRenameWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintDelegateRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DelegateRename_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-delegate-rename/native") / Token);
	TArray<FString> Owned{ObjectPath};
	const bool MVVMWasLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("ModelViewViewModelEditor"));
	ON_SCOPE_EXIT
	{
		TArray<UObject*> Cleanup;
		for (int32 Index = Owned.Num() - 1; Index >= 0; --Index) if (UObject* Object = LoadObject<UObject>(nullptr, *Owned[Index]))
		{
			Object->GetOutermost()->SetDirtyFlag(false);
			Cleanup.Add(Object);
		}
		// Interface classes are still referenced by this test's restored/scratch
		// Blueprint instances. Delete the complete, random fixture-owned selection.
		TestEqual(TEXT("delegate rename fixture cleanup"), ObjectTools::ForceDeleteObjects(Cleanup, false), Cleanup.Num());
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
		if (!MVVMWasLoaded) TestFalse(TEXT("ordinary delegate rename leaves optional MVVM unloaded"), FModuleManager::Get().IsModuleLoaded(TEXT("UeAgentInterfaceMVVM")));
	};
	auto NewBlueprint = [](const FString& Path) {
		auto* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Path),
			*FPackageName::GetLongPackageAssetName(Path), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) FAssetRegistryModule::AssetCreated(BP); return BP;
	};
	UBlueprint* Blueprint = NewBlueprint(PackageName);
	if (!TestTrue(TEXT("delegate fixture has native Actor event graph"), Blueprint && !Blueprint->UbergraphPages.IsEmpty())) return false;
	TArray<FString> InterfaceClassPaths;
	for (const FString& InterfaceName : {FString(TEXT("BPI_Keep")), FString(TEXT("BPI_Additional"))})
	{
		const FString InterfacePackage = FPackageName::GetLongPackagePath(PackageName) / InterfaceName;
		auto* Interface = FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), CreatePackage(*InterfacePackage),
			*InterfaceName, BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!TestNotNull(TEXT("mixed member fixture interface"), Interface)) return false;
		FAssetRegistryModule::AssetCreated(Interface); FKismetEditorUtilities::CompileBlueprint(Interface);
		Owned.Insert(InterfacePackage + TEXT(".") + InterfaceName, 0);
		InterfaceClassPaths.Add(Interface->GeneratedClass->GetPathName());
		FSavePackageArgs InterfaceSave; InterfaceSave.TopLevelFlags = RF_Public | RF_Standalone;
		const FString InterfaceFile = FPackageName::LongPackageNameToFilename(InterfacePackage, FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Interface->GetOutermost(), Interface, *InterfaceFile, InterfaceSave)) return false;
		Interface->GetOutermost()->SetDirtyFlag(false);
	}
	UClass* KeepInterface = LoadObject<UClass>(nullptr, *InterfaceClassPaths[0]);
	if (!TestTrue(TEXT("mixed fixture implements original interface"), FBlueprintEditorUtils::ImplementNewInterface(Blueprint, KeepInterface->GetClassPathName()))) return false;
	FEdGraphPinType Integer; Integer.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("mixed fixture non-target variable"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Integer, TEXT("23")))) return false;
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, TEXT("KeepCount"), true);
	FEdGraphPinType DelegateType; DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!TestTrue(TEXT("native delegate member added"), FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("StatusChanged"), DelegateType))) return false;
	UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("StatusChanged"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!TestNotNull(TEXT("native delegate signature graph"), SignatureGraph)) return false;
	SignatureGraph->bEditable = false;
	const auto* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
	K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
	K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
	Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);
	auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("name"), TEXT("Enabled"));
	auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("bool")); Input->SetObjectField(TEXT("type"), Type);
	auto Signature = MakeShared<FJsonObject>(); Signature->SetArrayField(TEXT("inputs"), {MakeShared<FJsonValueObject>(Input)});
	Signature->SetArrayField(TEXT("outputs"), {});
	if (!ApplyDelegateSignatureGraph(*SignatureGraph, Signature, Error)) { AddError(Error.ToPublicDetail()); return false; }
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(Blueprint->GeneratedClass, TEXT("StatusChanged"));
	if (!TestNotNull(TEXT("compiled multicast delegate property"), Property)) return false;
	UEdGraph* EventGraph = Blueprint->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_AddDelegate> BindCreator(*EventGraph); auto* Bind = BindCreator.CreateNode();
	Bind->SetFromProperty(Property, true, Blueprint->GeneratedClass); BindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallDelegate> CallCreator(*EventGraph); auto* Call = CallCreator.CreateNode();
	Call->SetFromProperty(Property, true, Blueprint->GeneratedClass); CallCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*EventGraph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("InvokeStatus"); InvokeCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> HandlerCreator(*EventGraph); auto* Handler = HandlerCreator.CreateNode();
	Handler->CustomFunctionName = TEXT("HandleStatus"); HandlerCreator.Finalize();
	if (!ApplyCustomEventSignature(*Handler, Signature, Error)) { AddError(Error.ToPublicDetail()); return false; }
	const auto* Schema = EventGraph->GetSchema();
	if (!TestTrue(TEXT("real delegate exec and event binding connections"), Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Bind->FindPin(UEdGraphSchema_K2::PN_Execute))
		&& Schema->TryCreateConnection(Bind->FindPin(UEdGraphSchema_K2::PN_Then), Call->FindPin(UEdGraphSchema_K2::PN_Execute))
		&& Schema->TryCreateConnection(Handler->FindPin(UK2Node_CustomEvent::DelegateOutputName), Bind->GetDelegatePin()))) return false;
	Schema->TrySetDefaultValue(*Call->FindPin(TEXT("Enabled")), TEXT("true"));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FGuid MemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("StatusChanged"))].VarGuid;
	const FGuid KeepId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
	bool ExpectedEditable = false;
	int32 ExpectedInterfaces = 1;
	const FGuid GraphId = SignatureGraph->GraphGuid, BindId = Bind->NodeGuid, CallId = Call->NodeGuid;
	const auto Before = CaptureIdentity(*Blueprint, GraphId);
	TSharedRef<FJsonObject> OriginalSignature = MakeShared<FJsonObject>();
	if (!CaptureDelegateSignatureGraph(*SignatureGraph, OriginalSignature, Error)) return false;
	auto Read = [&](UBlueprint* BP, const FName Name) {
		if (!BP || BP->Status == BS_Error) return false;
		const int32 KeepIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("KeepCount"));
		if (KeepIndex == INDEX_NONE || BP->NewVariables[KeepIndex].VarGuid != KeepId
			|| ((BP->NewVariables[KeepIndex].PropertyFlags & CPF_DisableEditOnInstance) == 0) != ExpectedEditable
			|| BP->ImplementedInterfaces.Num() != ExpectedInterfaces) return false;
		const auto* KeepProperty = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("KeepCount"));
		if (!KeepProperty || KeepProperty->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) != 23) return false;
		for (int32 InterfaceIndex = 0; InterfaceIndex < ExpectedInterfaces; ++InterfaceIndex)
			if (!BP->ImplementedInterfaces.ContainsByPredicate([&](const auto& Interface) {
				return Interface.Interface && Interface.Interface->GetPathName() == InterfaceClassPaths[InterfaceIndex]; })) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, Name);
		UEdGraph* Graph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, Name);
		if (Index == INDEX_NONE || BP->NewVariables[Index].VarGuid != MemberId || !Graph || Graph->GraphGuid != GraphId) return false;
		TSharedRef<FJsonObject> ActualSignature = MakeShared<FJsonObject>();
		FString ActualText, ExpectedText;
		if (!CaptureDelegateSignatureGraph(*Graph, ActualSignature, Error)
			|| !CommandContract::Json::TryCanonicalize(ActualSignature, ActualText, Error)
			|| !CommandContract::Json::TryCanonicalize(OriginalSignature, ExpectedText, Error)
			|| ActualText != ExpectedText) return false;
		int32 Consumers = 0;
		TArray<UK2Node_BaseMCDelegate*> Nodes; FBlueprintEditorUtils::GetAllNodesOfClass(BP, Nodes);
		for (const auto* Node : Nodes) if (Node->NodeGuid == BindId || Node->NodeGuid == CallId)
		{
			FMemberReference Reference = Node->DelegateReference;
			const auto* Resolved = Reference.ResolveMember<FMulticastDelegateProperty>(BP->GeneratedClass);
			if (Reference.GetMemberName() != Name || !Resolved || Resolved->GetOwnerClass() != BP->GeneratedClass) return false;
			++Consumers;
		}
		return Consumers == 2 && Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, GraphId));
	};
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Save)) return false;
	Blueprint->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	if (!TestTrue(TEXT("native dispatcher has true Bind/Call references and signature topology"), Read(Blueprint, TEXT("StatusChanged")))) return false;

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("delegate-rename-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Declaration = [&](const FString& Path, const FString& Key, const FString& NewName) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* UnitRow = nullptr;
			const TSharedPtr<FJsonObject>* UnitValues = nullptr;
			if (Document->TryGetObjectField(TEXT("unit"), UnitRow) && (*UnitRow)->TryGetObjectField(TEXT("values"), UnitValues)
				&& (*UnitValues)->HasField(TEXT("guid")) && (*UnitValues)->HasField(TEXT("signature"))) return false;
			const TSharedPtr<FJsonObject>* Map = nullptr;
			if (!Document->TryGetObjectField(TEXT("delegates"), Map)) continue;
			if (!Document->HasTypedField<EJson::Object>(TEXT("variables")) || !Document->HasTypedField<EJson::Object>(TEXT("interfaces"))) return false;
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (!(*Map)->TryGetObjectField(Key, Row) || (*Row)->HasField(TEXT("name")) || (*Row)->HasField(TEXT("guid")) || (*Row)->HasField(TEXT("new_name"))) return false;
			++Count;
			if (!NewName.IsEmpty())
			{
				(*Row)->SetStringField(TEXT("name"), NewName);
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		}
		return Count == 1;
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("delegate-rename-import"));
	};
	auto EditSiblingCollection = [&](const FString& Tag, const bool EditVariable, const bool ClearInterfaces = false) {
		const FString Path = Workspace / Tag;
		if (!Export(Path) || !Declaration(Path, TEXT("StatusChanged"), FString())) return false;
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false);
		int32 Edited = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			if (!Document->HasField(TEXT("delegates"))) continue;
			auto Variables = Document->GetObjectField(TEXT("variables")); auto Interfaces = Document->GetObjectField(TEXT("interfaces"));
			if (!Variables->HasField(TEXT("KeepCount")) || Interfaces->Values.Num() != ExpectedInterfaces) return false;
			if (EditVariable) Variables->GetObjectField(TEXT("KeepCount"))->SetBoolField(TEXT("instance_editable"), !ExpectedEditable);
			else if (ClearInterfaces) Document->SetObjectField(TEXT("interfaces"), MakeShared<FJsonObject>());
			else { auto Added = MakeShared<FJsonObject>(); Added->SetStringField(TEXT("class_path"), InterfaceClassPaths[1]); Interfaces->SetObjectField(TEXT("Additional"), Added); }
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			++Edited;
		}
		if (Edited != 1) return false;
		TWeakObjectPtr<UBlueprint> Old = Blueprint; Blueprint = nullptr;
		const auto Result = Import(Path);
		if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
		Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (EditVariable) ExpectedEditable = !ExpectedEditable; else ExpectedInterfaces = ClearInterfaces ? 0 : 2;
		const bool Preserved = !Old.IsValid() && Read(Blueprint, TEXT("StatusChanged")) && !Blueprint->GetOutermost()->IsDirty();
		AddInfo(FString::Printf(TEXT("mixed member %s: saved_reloaded=%d sibling_collections_preserved=%d"), *Tag, !Old.IsValid(), Preserved));
		return Preserved && Export(Workspace / (Tag + TEXT("-readback")))
			&& Declaration(Workspace / (Tag + TEXT("-readback")), TEXT("StatusChanged"), FString());
	};
	const FString MissingPath = Workspace / TEXT("missing-collection");
	if (!Export(MissingPath)) return false;
	TArray<FString> MissingFiles; IFileManager::Get().FindFilesRecursive(MissingFiles, *MissingPath, TEXT("*.hjson"), true, false);
	int32 MissingEdits = 0;
	for (const FString& MembersFile : MissingFiles)
	{
		if (MembersFile.Contains(TEXT("/.uai/")) || MembersFile.Contains(TEXT("\\.uai\\"))) continue;
		FString MissingText; TSharedPtr<FJsonObject> MissingDocument;
		if (!FFileHelper::LoadFileToString(MissingText, *MembersFile)
			|| !FUeAgentStructuredHjsonParser::ParseObject(MissingText, MembersFile, MissingDocument, Error)) return false;
		if (!MissingDocument->HasField(TEXT("delegates"))) continue;
		MissingDocument->RemoveField(TEXT("interfaces"));
		MissingDocument->GetObjectField(TEXT("variables"))->GetObjectField(TEXT("KeepCount"))->SetBoolField(TEXT("instance_editable"), true);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(MissingDocument.ToSharedRef(), MissingText, Error)
			|| !FFileHelper::SaveStringToFile(MissingText, *MembersFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		++MissingEdits;
	}
	if (!TestEqual(TEXT("missing collection edits the actual exported member document"), MissingEdits, 1)) return false;
	TWeakObjectPtr<UBlueprint> BeforeMissing = Blueprint;
	Blueprint = nullptr; SignatureGraph = nullptr; EventGraph = nullptr; Bind = nullptr; Call = nullptr; Handler = nullptr; Invoke = nullptr; Property = nullptr;
	const auto MissingResult = Import(MissingPath);
	if (!MissingResult.IsOk()) { AddError(MissingResult.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	ExpectedEditable = true;
	const bool OmittedPreserved = !BeforeMissing.IsValid() && Read(Blueprint, TEXT("StatusChanged")) && !Blueprint->GetOutermost()->IsDirty();
	AddInfo(FString::Printf(TEXT("mixed member omitted interfaces: saved_reloaded=%d omitted_and_siblings_preserved=%d"), !BeforeMissing.IsValid(), OmittedPreserved));
	if (!TestTrue(TEXT("omitted interface collection remains while explicit variable edit saves and reloads"), OmittedPreserved
		&& Export(Workspace / TEXT("omission-readback")) && Declaration(Workspace / TEXT("omission-readback"), TEXT("StatusChanged"), FString()))) return false;
	if (!TestTrue(TEXT("mixed variable edit preserves interface and dispatcher collections"), EditSiblingCollection(TEXT("variable-edit"), true))
		|| !TestTrue(TEXT("mixed interface edit preserves variable and dispatcher collections"), EditSiblingCollection(TEXT("interface-edit"), false))
		|| !TestTrue(TEXT("explicit empty interfaces removes interfaces and preserves siblings"), EditSiblingCollection(TEXT("interface-empty"), false, true))) return false;
	if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Reject = [&](const FString& Tag, const FString& Name, const FString& Code) {
		const FString Path = Workspace / Tag;
		if (!Export(Path) || !Declaration(Path, TEXT("StatusChanged"), Name)) return false;
		int32 Modified = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			if (Object && Owned.ContainsByPredicate([&](const FString& OwnedPath) { return Object->GetOutermost()->GetName() == FPackageName::ObjectPathToPackageName(OwnedPath); })) ++Modified;
		});
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		TArray<uint8> Disk;
		const bool Unchanged = Read(Blueprint, TEXT("StatusChanged")) && !Blueprint->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		AddInfo(FString::Printf(TEXT("delegate prewrite %s: rejected=%d typed=%d modifications=%d unchanged=%d"), *Tag, !Result.IsOk(), Result.GetErrorJson().Contains(Code), Modified, Unchanged));
		if (!Result.GetErrorJson().Contains(Code)) AddInfo(TEXT("delegate rejection detail: ") + Result.GetErrorJson());
		return TestTrue(TEXT("public delegate rename rejected before source or external modification"), !Result.IsOk() && Result.GetErrorJson().Contains(Code) && Modified == 0 && Unchanged);
	};
	if (!Reject(TEXT("conflict"), TEXT("Untouched"), TEXT("rename_name_invalid"))) return false;
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	UBlueprint* External = NewBlueprint(ExternalPackage); if (!External) return false;
	Owned.Add(ExternalPackage + TEXT(".BP_External"));
	FEdGraphPinType RefType; RefType.PinCategory = UEdGraphSchema_K2::PC_Object; RefType.PinSubCategoryObject = Blueprint->GeneratedClass;
	if (!TestTrue(TEXT("external dependency member added"), FBlueprintEditorUtils::AddMemberVariable(External, TEXT("SourceOwnerRef"), RefType))) return false;
	FKismetEditorUtilities::CompileBlueprint(External);
	if (!TestTrue(TEXT("external dependency Blueprint compiles"), External->Status != BS_Error)) return false;
	FGraphNodeCreator<UK2Node_CallFunction> UnrelatedCreator(*External->UbergraphPages[0]); auto* UnrelatedCall = UnrelatedCreator.CreateNode();
	UnrelatedCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("Untouched"))); UnrelatedCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External);
	TArray<UBlueprint*> UnrelatedDependents; FBlueprintEditorUtils::FindDependentBlueprints(Blueprint, UnrelatedDependents);
	if (!TestTrue(TEXT("unrelated fixture is a native dependency"), UnrelatedDependents.Contains(External))) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("name"), TEXT("StatusUpdated"));
	auto Delegates = MakeShared<FJsonObject>(); Delegates->SetObjectField(MemberId.ToString(EGuidFormats::DigitsWithHyphens), Spec);
	auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("delegates"), Delegates);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members); Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
	if (!TestTrue(TEXT("unrelated dependency is not a native rename write"), FPreparedBlueprintMutation::ValidateMemberNames(*Blueprint, Extension, Error))) { AddError(Error.ToPublicDetail()); return false; }
	FGraphNodeCreator<UK2Node_VariableGet> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalGet = ExternalCreator.CreateNode();
	ExternalGet->VariableReference.SetExternalMember(TEXT("StatusChanged"), Blueprint->GeneratedClass); ExternalCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External);
	TArray<UBlueprint*> NativeDependents; FBlueprintEditorUtils::FindDependentBlueprints(Blueprint, NativeDependents);
	if (!TestTrue(TEXT("external native consumer is in actual rename traversal"), NativeDependents.Contains(External)
		&& ExternalGet->ReferencesVariable(TEXT("StatusChanged"), nullptr))) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	if (!Reject(TEXT("external"), TEXT("StatusUpdated"), TEXT("delegate_rename_external_impact"))) return false;
	if (!TestEqual(TEXT("external fixture removed before accepted rename"), ObjectTools::DeleteObjects({External}, false), 1)) return false;
	Owned.Pop(); External = nullptr; ExternalGet = nullptr;
	auto LegacySpec = MakeShared<FJsonObject>(); LegacySpec->SetStringField(TEXT("name"), TEXT("StatusChanged"));
	LegacySpec->SetStringField(TEXT("new_name"), TEXT("StatusUpdated"));
	auto LegacyDelegates = MakeShared<FJsonObject>(); LegacyDelegates->SetObjectField(MemberId.ToString(EGuidFormats::DigitsWithHyphens), LegacySpec);
	auto LegacyMembers = MakeShared<FJsonObject>(); LegacyMembers->SetObjectField(TEXT("delegates"), LegacyDelegates);
	auto LegacyExtension = MakeShared<FJsonObject>(); LegacyExtension->SetObjectField(TEXT("members"), LegacyMembers);
	int32 LegacyModified = 0;
	const auto LegacyHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && Object->GetOutermost()->GetName() == PackageName) ++LegacyModified;
	});
	FAssetApplyRequest Legacy; Legacy.Operation = UeAgentApplyInputContract::EOperation::Update;
	Legacy.AssetPath = PackageName; Legacy.ProviderExtension = LegacyExtension;
	FAssetApplyResult LegacyResult; const bool LegacyApplied = ApplyAsset(Legacy, LegacyResult, TEXT("delegate-legacy-name"));
	FCoreUObjectDelegates::OnObjectModified.Remove(LegacyHandle);
	AddInfo(FString::Printf(TEXT("delegate retired new_name: rejected=%d typed=%d modifications=%d rollback=%d"), !LegacyApplied,
		LegacyResult.Diagnostic.PublicMessage.Contains(TEXT("blueprint_delegate_rename_legacy_field_unsupported")), LegacyModified, LegacyResult.bRollbackAttempted));
	if (!TestTrue(TEXT("retired delegate new_name fails explicitly before writing"), !LegacyApplied && LegacyModified == 0 && !LegacyResult.bRollbackAttempted
		&& LegacyResult.Diagnostic.PublicMessage.Contains(TEXT("blueprint_delegate_rename_legacy_field_unsupported")) && Read(Blueprint, TEXT("StatusChanged")))) return false;

	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag;
	Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Previous = Blueprint;
	bool Published = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Published = !Previous.IsValid() && Current && !Current->GetOutermost()->IsDirty() && Read(Current, TEXT("StatusUpdated"))
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("delegate late saved-reloaded member signature Bind Call identity witness=%d"), Published));
		Failure.Diagnostic.Code = TEXT("test_delegate_rename_late"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; SignatureGraph = nullptr; EventGraph = nullptr; Bind = nullptr; Call = nullptr; Handler = nullptr; Invoke = nullptr; Property = nullptr;
	FAssetApplyResult LateResult; TestFalse(TEXT("delegate late rejection"), ApplyAsset(Late, LateResult, TEXT("delegate-rename-late")));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RestoredDisk;
	const bool Restored = Published && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded
		&& Read(Blueprint, TEXT("StatusChanged")) && Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("delegate late original memory disk dirty complete references restored=%d"), Restored));
	if (!TestTrue(TEXT("delegate late rollback preserves original dirty memory and original disk"), Restored)) { AddError(LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString Edit = Workspace / TEXT("edit");
	if (!Export(Edit) || !Declaration(Edit, TEXT("StatusChanged"), TEXT("StatusUpdated"))) { AddError(Error.ToPublicDetail()); return false; }
	Previous = Blueprint; Blueprint = nullptr;
	const auto Imported = Import(Edit);
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	TestTrue(TEXT("public delegate name-only import preserves member graph node pin signature links and consumers"), !Previous.IsValid() && Read(Blueprint, TEXT("StatusUpdated")) && !Blueprint->GetOutermost()->IsDirty());
	TestTrue(TEXT("independent export emits renamed delegate without duplicate name or machine identity"), Export(Workspace / TEXT("readback")) && Declaration(Workspace / TEXT("readback"), TEXT("StatusUpdated"), FString()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintMacroTopologyTest,
	"UeAgentInterface.T208.Blueprint.MacroTopologyWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintMacroTopologyTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85MacroTopology_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	FString CurrentObjectPath = ObjectPath;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-graph-identity-rename/macro") / Token);
	TArray<FString> MacroOwned{ObjectPath};
	ON_SCOPE_EXIT
	{
		for (int32 Index = MacroOwned.Num() - 1; Index >= 0; --Index) if (UObject* Object = LoadObject<UObject>(nullptr, *MacroOwned[Index]))
		{
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned macro fixture deleted"), ObjectTools::DeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName),
		TEXT("BP_Owner"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("SelectStatus"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Blueprint, Macro, true, nullptr);
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	auto Pin = [](const TCHAR* Name, const TCHAR* Category) {
		auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("name"), Name);
		auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), Category); Value->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Value);
	};
	auto Signature = MakeShared<FJsonObject>();
	Signature->SetArrayField(TEXT("inputs"), {Pin(TEXT("Enabled"), TEXT("bool"))});
	Signature->SetArrayField(TEXT("outputs"), {Pin(TEXT("Succeeded"), TEXT("bool"))});
	if (!ApplyMacroGraphSignature(*Macro, Signature, Error)) { AddError(Error.ToPublicDetail()); return false; }
	UK2Node_Tunnel* Entry = nullptr; UK2Node_Tunnel* Result = nullptr;
	for (UEdGraphNode* Node : Macro->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node))
	{
		if (Tunnel->bCanHaveOutputs) Entry = Tunnel;
		if (Tunnel->bCanHaveInputs) Result = Tunnel;
	}
	if (!Entry || !Result) return false;
	FEdGraphPinType ExecType; ExecType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	Entry->CreateUserDefinedPin(TEXT("In"), ExecType, EGPD_Output, false);
	Result->CreateUserDefinedPin(TEXT("Out"), ExecType, EGPD_Input, false);
	Entry->NodePosX = -320; Result->NodePosX = 320;
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema->TryCreateConnection(Entry->FindPin(TEXT("In")), Result->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(Entry->FindPin(TEXT("Enabled")), Result->FindPin(TEXT("Succeeded")))) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> CallerCreator(*Blueprint->UbergraphPages[0]);
	UK2Node_MacroInstance* Caller = CallerCreator.CreateNode(); Caller->SetMacroGraph(Macro); CallerCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Blueprint->UbergraphPages[0]);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode(); Event->CustomFunctionName = TEXT("InvokeStatus"); EventCreator.Finalize();
	if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Caller->FindPin(TEXT("In")))) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error) return false;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Save)) return false;
	const FGuid MacroId = Macro->GraphGuid, CallerId = Caller->NodeGuid;
	auto Expected = CaptureIdentity(*Blueprint, FGuid());
	Expected[MacroId.ToString() + TEXT("/") + Entry->NodeGuid.ToString()] = Entry->GetClass()->GetPathName() + FString::Printf(TEXT(":%d,%d"), -224, Entry->NodePosY);
	auto BeforeSignature = MakeShared<FJsonObject>();
	if (!CaptureMacroGraphSignature(*Macro, BeforeSignature, Error)) return false;
	FString BeforeSignatureText;
	if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(BeforeSignature, BeforeSignatureText, Error)) return false;
	TArray<uint8> BeforeDisk; FFileHelper::LoadFileToArray(BeforeDisk, *Filename);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), CurrentObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Outcome = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("macro-topology-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Outcome.IsOk()) AddError(Outcome.GetErrorJson()); return Outcome.IsOk();
	};
	auto Body = [&](const FString& Path, bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Header = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph"), Header) || (*Header)->GetStringField(TEXT("name")) != TEXT("SelectStatus")) continue;
			if (!Document->HasField(TEXT("interface_ref"))) continue;
			const auto Nodes = Document->GetObjectField(TEXT("nodes"));
			for (const auto& Pair : Nodes->Values)
			{
				const auto Node = Pair.Value->AsObject(); const auto Position = Node->GetObjectField(TEXT("pos"));
				if (Node->GetStringField(TEXT("node_class")) != TEXT("/Script/BlueprintGraph.K2Node_Tunnel")
					|| Position->GetNumberField(TEXT("x")) != (Edit ? -320 : -224)) continue;
				++Matches; if (Edit) Position->SetNumberField(TEXT("x"), -224);
			}
			if (Document->GetArrayField(TEXT("connections")).Num() != 2) return false;
			if (Edit && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		return Matches == 1;
	};
	if (!Export(Workspace / TEXT("edit")) || !Body(Workspace / TEXT("edit"), true)) { AddError(Error.ToPublicDetail()); return false; }
	TWeakObjectPtr<UBlueprint> BeforeReload = Blueprint;
	Blueprint = nullptr; Macro = nullptr; Entry = nullptr; Result = nullptr; Caller = nullptr; Event = nullptr;
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
	auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), CurrentObjectPath); Args->SetObjectField(TEXT("source"), Source);
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("macro-topology-import"));
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!Blueprint) return false;
	bool CallerMatches = false;
	for (UEdGraph* Graph : Blueprint->MacroGraphs) if (Graph->GraphGuid == MacroId) Macro = Graph;
	for (UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes) if (auto* Instance = Cast<UK2Node_MacroInstance>(Node); Instance && Instance->NodeGuid == CallerId)
		CallerMatches = Instance->GetMacroGraph() == Macro;
	auto AfterSignature = MakeShared<FJsonObject>(); FString AfterSignatureText;
	const bool SignatureMatches = Macro && CaptureMacroGraphSignature(*Macro, AfterSignature, Error)
		&& FUeAgentStructuredHjsonFormatter::TryWriteObject(AfterSignature, AfterSignatureText, Error) && BeforeSignatureText == AfterSignatureText;
	TArray<uint8> AfterDisk;
	const bool Saved = FFileHelper::LoadFileToArray(AfterDisk, *Filename) && BeforeDisk != AfterDisk;
	const bool Identities = Expected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()));
	const bool Reexport = Export(Workspace / TEXT("readback")) && Body(Workspace / TEXT("readback"), false);
	AddInfo(FString::Printf(TEXT("macro topology witness: replaced=%d clean=%d compiled=%d signature=%d identities=%d caller=%d written=%d reexport=%d"),
		!BeforeReload.IsValid(), !Blueprint->GetOutermost()->IsDirty(), Blueprint->Status != BS_Error, SignatureMatches, Identities, CallerMatches, Saved, Reexport));
	if (!TestTrue(TEXT("public macro body edit preserves tunnel topology signature caller and non-target identities after saved reload and independent export"),
		!BeforeReload.IsValid() && !Blueprint->GetOutermost()->IsDirty() && Blueprint->Status != BS_Error
		&& SignatureMatches && Identities && CallerMatches && Saved && Reexport)) return false;

	auto Omit = [&](const FString& Path, bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			if (Edit && (*Declarations)->HasField(TEXT("SelectStatus")))
			{
				++Matches; (*Declarations)->RemoveField(TEXT("SelectStatus"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			else if (!Edit && !(*Declarations)->HasField(TEXT("SelectStatus"))) ++Matches;
		}
		return Matches == 1;
	};
	auto ImportRemoval = [&](const FString& Path) {
		auto Directory = MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"), TEXT("directory")); Directory->SetStringField(TEXT("path"), Path);
		auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("target"), CurrentObjectPath); Input->SetObjectField(TEXT("source"), Directory);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Input, TEXT("macro-removal-import"));
	};
	auto SaveAsset = [&](UBlueprint* Asset) {
		FKismetEditorUtilities::CompileBlueprint(Asset);
		FSavePackageArgs Options; Options.TopLevelFlags = RF_Public | RF_Standalone;
		return Asset->Status != BS_Error && UPackage::SavePackage(Asset->GetOutermost(), Asset,
			*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Options);
	};
	auto RejectRemoval = [&](const FString& Label, UBlueprint* Other) {
		const FString Path = Workspace / Label;
		if (!Export(Path) || !Omit(Path, true)) return false;
		const auto Before = CaptureIdentity(*Blueprint, FGuid());
		const auto Outside = Other ? CaptureIdentity(*Other, FGuid()) : TMap<FString, FString>();
		const FString RejectedFilename = FPackageName::LongPackageNameToFilename(Blueprint->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		TArray<uint8> DiskBefore, DiskAfter; FFileHelper::LoadFileToArray(DiskBefore, *RejectedFilename);
		int32 Modifications = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			if (Object && (Object->GetOutermost() == Blueprint->GetOutermost() || (Other && Object->GetOutermost() == Other->GetOutermost()))) ++Modifications;
		});
		const auto Outcome = ImportRemoval(Path);
		FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (Outcome.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("macro removal unexpectedly succeeded")); return false; }
		const bool Unchanged = Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
			&& (!Other || Outside.OrderIndependentCompareEqual(CaptureIdentity(*Other, FGuid())))
			&& !Blueprint->GetOutermost()->IsDirty() && (!Other || !Other->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(DiskAfter, *RejectedFilename) && DiskBefore == DiskAfter;
		AddInfo(FString::Printf(TEXT("macro removal %s: rejected=1 modifications=%d unchanged=%d"), *Label, Modifications, Unchanged));
		return TestTrue(TEXT("macro instance rejected before source or consumer write"),
			Outcome.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported")) && Modifications == 0 && Unchanged);
	};
	if (!RejectRemoval(TEXT("same-asset-removal"), nullptr)) return false;
	for (UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes) if (Node->NodeGuid == CallerId) Caller = Cast<UK2Node_MacroInstance>(Node);
	if (!Caller) return false;
	const auto* ReferenceProperty = FindFProperty<FStructProperty>(UK2Node_MacroInstance::StaticClass(), TEXT("MacroGraphReference"));
	const auto* CacheProperty = FindFProperty<FObjectPropertyBase>(FGraphReference::StaticStruct(), TEXT("MacroGraph"));
	if (!ReferenceProperty || ReferenceProperty->Struct != FGraphReference::StaticStruct() || !CacheProperty) return false;
	FGraphReference* Reference = ReferenceProperty->ContainerPtrToValuePtr<FGraphReference>(Caller);
	CacheProperty->SetObjectPropertyValue_InContainer(Reference, nullptr);
	TArray<uint8> EmptyBefore, EmptyAfter; FObjectWriter EmptyWriter(Caller, EmptyBefore);
	auto RemoveSpec = MakeShared<FJsonObject>(); RemoveSpec->SetStringField(TEXT("operation"), TEXT("remove"));
	RemoveSpec->SetStringField(TEXT("graph_kind"), TEXT("macro_graph")); RemoveSpec->SetStringField(TEXT("name"), TEXT("SelectStatus"));
	auto GraphSpecs = MakeShared<FJsonObject>(); GraphSpecs->SetObjectField(MacroId.ToString(EGuidFormats::DigitsWithHyphens), RemoveSpec);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), GraphSpecs);
	int32 EmptyModifications = 0;
	const auto EmptyHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && Object->GetOutermost() == Blueprint->GetOutermost()) ++EmptyModifications;
	});
	const bool EmptyRejected = !FPreparedBlueprintMutation::ValidateRemovals(*Blueprint, Extension, Error);
	FCoreUObjectDelegates::OnObjectModified.Remove(EmptyHandle);
	FObjectWriter EmptyAfterWriter(Caller, EmptyAfter);
	const bool CacheUnchanged = EmptyBefore == EmptyAfter && !CacheProperty->GetObjectPropertyValue_InContainer(Reference);
	AddInfo(FString::Printf(TEXT("macro empty cache: rejected=%d unchanged=%d modifications=%d"), EmptyRejected, CacheUnchanged, EmptyModifications));
	if (!TestTrue(TEXT("native reference copy resolves empty cache without changing the original"), EmptyRejected && CacheUnchanged && EmptyModifications == 0
		&& Error.Code == TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	Caller->SetMacroGraph(Macro); Caller->DestroyNode(); Caller = nullptr;
	if (!SaveAsset(Blueprint)) return false;
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	UBlueprint* External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!External) return false;
	FAssetRegistryModule::AssetCreated(External); MacroOwned.Add(External->GetPathName());
	const FString LibraryPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_Library");
	UBlueprint* Library = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*LibraryPackage), TEXT("BP_Library"),
		BPTYPE_MacroLibrary, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Library) return false;
	FAssetRegistryModule::AssetCreated(Library); MacroOwned.Add(Library->GetPathName());
	UEdGraph* LibraryMacro = FBlueprintEditorUtils::CreateNewGraph(Library, TEXT("SelectStatus"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Library, LibraryMacro, true, nullptr);
	if (!ApplyMacroGraphSignature(*LibraryMacro, Signature, Error) || !SaveAsset(Library)) return false;
	if (!TestTrue(TEXT("native MacroLibrary makes its cross-asset graph public"), LibraryMacro->HasAnyFlags(RF_Public))) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> ExternalCreator(*External->UbergraphPages[0]);
	auto* ExternalCaller = ExternalCreator.CreateNode(); ExternalCaller->SetMacroGraph(LibraryMacro); ExternalCreator.Finalize();
	if (!SaveAsset(External)) return false;
	UBlueprint* OrdinaryOwner = Blueprint;
	Blueprint = Library; CurrentObjectPath = Library->GetPathName();
	const bool ExternalRejected = RejectRemoval(TEXT("external-removal"), External);
	Blueprint = OrdinaryOwner; CurrentObjectPath = ObjectPath;
	if (!ExternalRejected) return false;
	ExternalCaller->DestroyNode(); ExternalCaller = nullptr;
	if (!SaveAsset(External)) return false;
	const FString CopyPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_Copy");
	UBlueprint* Copy = DuplicateObject<UBlueprint>(Blueprint, CreatePackage(*CopyPackage), TEXT("BP_Copy"));
	if (!Copy) return false;
	FAssetRegistryModule::AssetCreated(Copy); MacroOwned.Add(Copy->GetPathName());
	UEdGraph* CopiedMacro = nullptr;
	for (UEdGraph* Graph : Copy->MacroGraphs) if (Graph->GraphGuid == MacroId) CopiedMacro = Graph;
	if (!TestTrue(TEXT("native copied macro retains GUID but has different graph identity"), CopiedMacro && CopiedMacro != Macro)) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> CopyCreator(*Copy->UbergraphPages[0]);
	auto* CopyCaller = CopyCreator.CreateNode(); CopyCaller->SetMacroGraph(CopiedMacro); CopyCreator.Finalize();
	if (!SaveAsset(Copy)) return false;
	const auto CopyBefore = CaptureIdentity(*Copy, FGuid());
	const auto OutsideBefore = CaptureIdentity(*External, FGuid());
	TArray<uint8> CopyBytesBefore, CopyBytesAfter; FObjectWriter CopyWriter(CopyCaller, CopyBytesBefore);
	auto Remaining = CaptureIdentity(*Blueprint, FGuid());
	for (auto It = Remaining.CreateIterator(); It; ++It) if (It.Key() == MacroId.ToString() || It.Key().StartsWith(MacroId.ToString() + TEXT("/"))) It.RemoveCurrent();
	if (!Export(Workspace / TEXT("remove-success")) || !Omit(Workspace / TEXT("remove-success"), true)) return false;
	BeforeReload = Blueprint; Blueprint = nullptr; Macro = nullptr;
	const auto Removed = ImportRemoval(Workspace / TEXT("remove-success"));
	if (!Removed.IsOk()) { AddError(Removed.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	FObjectWriter CopyAfterWriter(CopyCaller, CopyBytesAfter);
	const bool RemovalPreserved = Blueprint && Remaining.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
		&& CopyBefore.OrderIndependentCompareEqual(CaptureIdentity(*Copy, FGuid())) && OutsideBefore.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid()))
		&& CopyBytesBefore == CopyBytesAfter && CopyCaller->GetMacroGraph() == CopiedMacro;
	const bool RemovalExport = Export(Workspace / TEXT("removed-readback")) && Omit(Workspace / TEXT("removed-readback"), false);
	AddInfo(FString::Printf(TEXT("macro removal success: replaced=%d clean=%d absent=%d identities=%d copied_reference=%d reexport=%d"),
		!BeforeReload.IsValid(), Blueprint && !Blueprint->GetOutermost()->IsDirty(), Blueprint && Blueprint->MacroGraphs.IsEmpty(), RemovalPreserved, CopyBytesBefore == CopyBytesAfter, RemovalExport));
	if (!TestTrue(TEXT("macro removal preserves copied macro signature topology caller and non-targets after saved reload"),
		!BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && Blueprint->MacroGraphs.IsEmpty() && RemovalPreserved && RemovalExport)) return false;
	UEdGraph* AtomicMacro = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("SelectStatus"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Blueprint, AtomicMacro, true, nullptr);
	if (!ApplyMacroGraphSignature(*AtomicMacro, Signature, Error)) return false;
	UK2Node_Tunnel* AtomicEntry = nullptr; UK2Node_Tunnel* AtomicResult = nullptr;
	for (UEdGraphNode* Node : AtomicMacro->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node))
	{
		if (Tunnel->bCanHaveOutputs) AtomicEntry = Tunnel;
		if (Tunnel->bCanHaveInputs) AtomicResult = Tunnel;
	}
	if (!AtomicEntry || !AtomicResult) return false;
	AtomicEntry->CreateUserDefinedPin(TEXT("In"), ExecType, EGPD_Output, false);
	AtomicResult->CreateUserDefinedPin(TEXT("Out"), ExecType, EGPD_Input, false);
	if (!Schema->TryCreateConnection(AtomicEntry->FindPin(TEXT("In")), AtomicResult->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(AtomicEntry->FindPin(TEXT("Enabled")), AtomicResult->FindPin(TEXT("Succeeded")))) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> AtomicCreator(*Blueprint->UbergraphPages[0]);
	auto* AtomicCaller = AtomicCreator.CreateNode(); AtomicCaller->SetMacroGraph(AtomicMacro); AtomicCreator.Finalize();
	UK2Node_CustomEvent* ExistingEvent = nullptr;
	for (UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes) if (auto* Custom = Cast<UK2Node_CustomEvent>(Node)) ExistingEvent = Custom;
	if (!ExistingEvent || !Schema->TryCreateConnection(ExistingEvent->FindPin(UEdGraphSchema_K2::PN_Then), AtomicCaller->FindPin(TEXT("In")))
		|| !SaveAsset(Blueprint)) return false;
	const FGuid AtomicMacroId = AtomicMacro->GraphGuid, AtomicCallerId = AtomicCaller->NodeGuid;
	const FString CallerKey = Blueprint->UbergraphPages[0]->GraphGuid.ToString() + TEXT("/") + AtomicCallerId.ToString();
	const FString CallerLink = AtomicCallerId.ToString() + TEXT("/") + AtomicCaller->FindPin(TEXT("In"))->PinId.ToString();
	auto AtomicExpected = CaptureIdentity(*Blueprint, FGuid());
	for (auto It = AtomicExpected.CreateIterator(); It; ++It)
	{
		if (It.Key() == AtomicMacroId.ToString() || It.Key().StartsWith(AtomicMacroId.ToString() + TEXT("/"))
			|| It.Key() == CallerKey || It.Key().StartsWith(CallerKey + TEXT("/"))) It.RemoveCurrent();
		else It.Value() = It.Value().Replace(*CallerLink, TEXT(""));
	}
	const FString AtomicWorkspace = Workspace / TEXT("atomic-macro");
	if (!Export(AtomicWorkspace) || !Omit(AtomicWorkspace, true)) return false;
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *AtomicWorkspace, TEXT("*.hjson"), true, false); int32 Edited = 0;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		if (!Document->HasField(TEXT("interface_ref"))) continue;
		const auto Nodes = Document->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
		for (const auto& Pair : Nodes->Values)
		{
			const auto Node = Pair.Value->AsObject(); FString Kind, Name;
			if (Node->TryGetStringField(TEXT("node_type"), Kind) && Kind == TEXT("macro_instance")
				&& Node->TryGetStringField(TEXT("name"), Name) && Name == TEXT("SelectStatus")) Deleted.Add(Pair.Key);
		}
		if (Deleted.IsEmpty()) continue;
		for (const FString& Key : Deleted) { Nodes->RemoveField(Key); ++Edited; }
		TArray<TSharedPtr<FJsonValue>> Connections = Document->GetArrayField(TEXT("connections"));
		Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) {
			const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); });
		});
		Document->SetArrayField(TEXT("connections"), Connections);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestEqual(TEXT("ordinary workspace explicitly omits one macro instance"), Edited, 1)) return false;
	BeforeReload = Blueprint; Blueprint = nullptr; AtomicMacro = nullptr; AtomicCaller = nullptr; AtomicEntry = nullptr; AtomicResult = nullptr; ExistingEvent = nullptr;
	const auto AtomicImported = ImportRemoval(AtomicWorkspace);
	if (!AtomicImported.IsOk()) { AddError(AtomicImported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const bool AtomicReadback = Export(Workspace / TEXT("atomic-macro-readback")) && Omit(Workspace / TEXT("atomic-macro-readback"), false);
	const bool AtomicSuccess = !BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && Blueprint->MacroGraphs.IsEmpty()
		&& AtomicExpected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid())) && AtomicReadback;
	AddInfo(FString::Printf(TEXT("atomic public macro deletion: replaced_clean_both_absent_non_targets_reexport=%d"), AtomicSuccess));
	if (!TestTrue(TEXT("one public import removes explicit macro instance before producer and independently reads back"), AtomicSuccess)) return false;
	auto CreateSetMacro = [&](const TCHAR* Name, UK2Node_Tunnel*& In, UK2Node_Tunnel*& Out) {
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
		if (!ApplyMacroGraphSignature(*Graph, Signature, Error)) return static_cast<UEdGraph*>(nullptr);
		for (UEdGraphNode* Node : Graph->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node))
		{
			if (Tunnel->bCanHaveOutputs) In = Tunnel;
			if (Tunnel->bCanHaveInputs) Out = Tunnel;
		}
		if (!In || !Out) return static_cast<UEdGraph*>(nullptr);
		In->CreateUserDefinedPin(TEXT("In"), ExecType, EGPD_Output, false);
		Out->CreateUserDefinedPin(TEXT("Out"), ExecType, EGPD_Input, false);
		return Graph;
	};
	UK2Node_Tunnel* AIn = nullptr; UK2Node_Tunnel* AOut = nullptr; UK2Node_Tunnel* BIn = nullptr; UK2Node_Tunnel* BOut = nullptr;
	UEdGraph* MacroA = CreateSetMacro(TEXT("MacroSetA"), AIn, AOut);
	UEdGraph* MacroB = CreateSetMacro(TEXT("MacroSetB"), BIn, BOut);
	if (!MacroA || !MacroB) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> NestedCreator(*MacroA);
	auto* Nested = NestedCreator.CreateNode(); Nested->SetMacroGraph(MacroB); NestedCreator.Finalize();
	if (!Schema->TryCreateConnection(BIn->FindPin(TEXT("In")), BOut->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(BIn->FindPin(TEXT("Enabled")), BOut->FindPin(TEXT("Succeeded")))
		|| !Schema->TryCreateConnection(AIn->FindPin(TEXT("In")), Nested->FindPin(TEXT("In")))
		|| !Schema->TryCreateConnection(Nested->FindPin(TEXT("Out")), AOut->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(AIn->FindPin(TEXT("Enabled")), Nested->FindPin(TEXT("Enabled")))
		|| !Schema->TryCreateConnection(Nested->FindPin(TEXT("Succeeded")), AOut->FindPin(TEXT("Succeeded")))) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> TopCreator(*Blueprint->UbergraphPages[0]);
	auto* Top = TopCreator.CreateNode(); Top->SetMacroGraph(MacroA); TopCreator.Finalize();
	for (UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes) if (auto* Custom = Cast<UK2Node_CustomEvent>(Node)) ExistingEvent = Custom;
	if (!ExistingEvent || !Schema->TryCreateConnection(ExistingEvent->FindPin(UEdGraphSchema_K2::PN_Then), Top->FindPin(TEXT("In"))) || !SaveAsset(Blueprint)) return false;
	Top->DestroyNode(); Top = nullptr;
	if (!SaveAsset(Blueprint)) return false;
	const FGuid AId = MacroA->GraphGuid, BId = MacroB->GraphGuid;
	auto SetExpected = CaptureIdentity(*Blueprint, FGuid());
	for (auto It = SetExpected.CreateIterator(); It; ++It)
		if (It.Key() == AId.ToString() || It.Key().StartsWith(AId.ToString() + TEXT("/"))
			|| It.Key() == BId.ToString() || It.Key().StartsWith(BId.ToString() + TEXT("/"))) It.RemoveCurrent();
	auto OmitMacroSet = [&](const FString& Path, bool Edit) {
		TArray<FString> Paths; IFileManager::Get().FindFilesRecursive(Paths, *Path, TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const FString& File : Paths)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			if (Edit)
			{
				if (!(*Declarations)->HasField(TEXT("MacroSetA")) || !(*Declarations)->HasField(TEXT("MacroSetB"))) return false;
				(*Declarations)->RemoveField(TEXT("MacroSetA")); (*Declarations)->RemoveField(TEXT("MacroSetB"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			else if ((*Declarations)->HasField(TEXT("MacroSetA")) || (*Declarations)->HasField(TEXT("MacroSetB"))) return false;
			++Count;
		}
		return Count == 1;
	};
	if (!Export(Workspace / TEXT("macro-set")) || !OmitMacroSet(Workspace / TEXT("macro-set"), true)) return false;
	BeforeReload = Blueprint; Blueprint = nullptr; MacroA = nullptr; MacroB = nullptr; Nested = nullptr; AIn = nullptr; AOut = nullptr; BIn = nullptr; BOut = nullptr; ExistingEvent = nullptr;
	const auto SetImported = ImportRemoval(Workspace / TEXT("macro-set"));
	if (!SetImported.IsOk()) { AddError(SetImported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const bool SetReadback = Export(Workspace / TEXT("macro-set-readback")) && OmitMacroSet(Workspace / TEXT("macro-set-readback"), false);
	const bool SetSuccess = !BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && Blueprint->MacroGraphs.IsEmpty()
		&& SetExpected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid())) && SetReadback;
	AddInfo(FString::Printf(TEXT("public macro deletion set: legal_A_to_B_saved_reloaded_absent_non_targets_reexport=%d"), SetSuccess));
	return TestTrue(TEXT("legal acyclic macro owning graph set is removed in one public import"), SetSuccess) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintFunctionRemovalTest,
	"UeAgentInterface.T208.Blueprint.FunctionRemovalWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintFunctionRemovalTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	UE::UAI::Contract::FDiagnostic Error;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Root = TEXT("/Game/UAI/Transient/T85FunctionRemoval_") + Token;
	const FString PackageName = Root + TEXT("/BP_Owner"), ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-function-removal/native") / Token);
	TArray<FString> Owned;
	ON_SCOPE_EXIT
	{
		for (int32 Index = Owned.Num() - 1; Index >= 0; --Index) if (UObject* Object = LoadObject<UObject>(nullptr, *Owned[Index]))
		{
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned removal fixture deleted"), ObjectTools::DeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto Create = [&](const TCHAR* Name) {
		const FString Package = Root / Name;
		auto* Created = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Package), Name,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Created) { FAssetRegistryModule::AssetCreated(Created); Owned.Add(Created->GetPathName()); }
		return Created;
	};
	auto Save = [&](UBlueprint* Asset) {
		FKismetEditorUtilities::CompileBlueprint(Asset);
		FSavePackageArgs Options; Options.TopLevelFlags = RF_Public | RF_Standalone;
		return Asset->Status != BS_Error && UPackage::SavePackage(Asset->GetOutermost(), Asset,
			*FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Options);
	};
	UBlueprint* Blueprint = Create(TEXT("BP_Owner"));
	if (!Blueprint) return false;
	UEdGraph* Removed = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Doomed"));
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save(Blueprint)) return false;
	auto AddCall = [&](UEdGraph* Graph, UFunction* Function) {
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		auto* Call = Creator.CreateNode(); Call->SetFromFunction(Function); Creator.Finalize(); return Call;
	};
	UK2Node_CallFunction* Local = AddCall(Blueprint->UbergraphPages[0], Blueprint->GeneratedClass->FindFunctionByName(TEXT("Doomed")));
	AddCall(Removed, Blueprint->GeneratedClass->FindFunctionByName(TEXT("Doomed")));
	if (!Save(Blueprint)) return false;
	const FGuid RemovedId = Removed->GraphGuid;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Outcome = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("function-removal-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Outcome.IsOk()) AddError(Outcome.GetErrorJson()); return Outcome.IsOk();
	};
	auto Omit = [&](const FString& Path, bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			if (Edit && (*Declarations)->HasField(TEXT("Doomed")))
			{
				++Matches; (*Declarations)->RemoveField(TEXT("Doomed"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			else if (!Edit && !(*Declarations)->HasField(TEXT("Doomed"))) ++Matches;
		}
		return Matches == 1;
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("function-removal-import"));
	};
	auto Reject = [&](const FString& Label, UBlueprint* External, const TCHAR* Code = TEXT("blueprint_graph_remove_live_consumer_plan_unsupported")) {
		const FString Path = Workspace / Label;
		if (!Export(Path) || !Omit(Path, true)) { AddError(Error.ToPublicDetail()); return false; }
		const auto Before = CaptureIdentity(*Blueprint, FGuid());
		const auto Outside = External ? CaptureIdentity(*External, FGuid()) : TMap<FString, FString>();
		TArray<uint8> DiskBefore, DiskAfter; FFileHelper::LoadFileToArray(DiskBefore, *Filename);
		int32 Modifications = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
			if (Object && (Object->GetOutermost() == Blueprint->GetOutermost() || (External && Object->GetOutermost() == External->GetOutermost()))) ++Modifications;
		});
		const auto Outcome = Import(Path);
		FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		// A wrongly accepted import can replace the asset. Never dereference the
		// old native fixture pointer before inspecting that outcome.
		if (Outcome.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(Label + TEXT(": removal unexpectedly succeeded")); return false; }
		const bool Unchanged = Before.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
			&& (!External || Outside.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid())))
			&& !Blueprint->GetOutermost()->IsDirty() && (!External || !External->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskBefore == DiskAfter;
		AddInfo(FString::Printf(TEXT("function removal %s: rejected=%d modifications=%d unchanged=%d"), *Label, !Outcome.IsOk(), Modifications, Unchanged));
		if (Outcome.IsOk() || !Outcome.GetErrorJson().Contains(Code)) { AddError(Outcome.GetErrorJson()); return false; }
		return TestTrue(TEXT("live consumer refused before any source or consumer write"), Modifications == 0 && Unchanged);
	};
	if (!Reject(TEXT("same-asset"), nullptr)) return false;
	Local->DestroyNode(); Local = nullptr;
	if (!Save(Blueprint)) return false;
	FGraphNodeCreator<UK2Node_CreateDelegate> DelegateCreator(*Blueprint->UbergraphPages[0]);
	UK2Node_CreateDelegate* Delegate = DelegateCreator.CreateNode(); DelegateCreator.Finalize();
	UK2Node_CallFunction* Timer = AddCall(Blueprint->UbergraphPages[0], UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate")));
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Delegate->GetDelegateOutPin(), Timer->FindPin(TEXT("Delegate")))) return false;
	Delegate->SetFunction(TEXT("Doomed"));
	// The real delegate sink keeps native validation from clearing the selection.
	if (!Save(Blueprint) || !Reject(TEXT("create-delegate"), nullptr)) return false;
	Delegate->DestroyNode(); Delegate = nullptr; Timer->DestroyNode(); Timer = nullptr;
	if (!Save(Blueprint)) return false;
	UBlueprint* External = Create(TEXT("BP_External"));
	UK2Node_CallFunction* ExternalCall = AddCall(External->UbergraphPages[0], Blueprint->GeneratedClass->FindFunctionByName(TEXT("Doomed")));
	// The unconnected external instance call remains a real persisted reference.
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	if (!Save(External) || !Reject(TEXT("external"), External)) return false;
	const FString ExternalPath = External->GetPathName();
	const FString ExternalPackage = External->GetOutermost()->GetName();
	const FString ExternalFilename = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({ExternalFilename}, true);
	UPackage* ToUnload = External->GetOutermost();
	External = nullptr; ExternalCall = nullptr;
	if (!TestTrue(TEXT("external hard referencer is genuinely unloaded"), UPackageTools::UnloadPackages({ToUnload}) && !FindObject<UBlueprint>(nullptr, *ExternalPath))) return false;
	if (!Reject(TEXT("unloaded-reference"), nullptr, TEXT("blueprint_graph_remove_references_unresolved"))) return false;
	if (!TestNull(TEXT("preflight does not load unresolved external candidate"), FindObject<UBlueprint>(nullptr, *ExternalPath))) return false;
	auto LoadArgs = MakeShared<FJsonObject>(); LoadArgs->SetStringField(TEXT("target"), ExternalPath);
	LoadArgs->SetStringField(TEXT("workspace_path"), Workspace / TEXT("explicit-load")); LoadArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Loaded = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), LoadArgs,
		TEXT("function-removal-explicit-load"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Loaded.IsOk()) { AddError(Loaded.GetErrorJson()); return false; }
	External = FindObject<UBlueprint>(nullptr, *ExternalPath);
	if (!External || !Reject(TEXT("loaded-reference-retry"), External)) return false;
	for (UEdGraphNode* Node : External->UbergraphPages[0]->Nodes) if (auto* Call = Cast<UK2Node_CallFunction>(Node); Call && Call->GetFunctionName() == TEXT("Doomed")) ExternalCall = Call;
	if (!ExternalCall) return false;
	ExternalCall->DestroyNode(); ExternalCall = nullptr;
	UBlueprintEditorLibrary::AddFunctionGraph(External, TEXT("Doomed"));
	if (!Save(External)) return false;
	AddCall(External->UbergraphPages[0], External->GeneratedClass->FindFunctionByName(TEXT("Doomed")));
	if (!Save(External)) return false;
	const auto OutsideBefore = CaptureIdentity(*External, FGuid());
	const FString CopyPackage = Root / TEXT("BP_Copy");
	UBlueprint* Copy = DuplicateObject<UBlueprint>(Blueprint, CreatePackage(*CopyPackage), TEXT("BP_Copy"));
	if (!Copy) return false;
	FAssetRegistryModule::AssetCreated(Copy); Owned.Add(Copy->GetPathName());
	if (!Save(Copy)) return false;
	UFunction* CopyFunction = Copy->GeneratedClass->FindFunctionByName(TEXT("Doomed"));
	UK2Node_CallFunction* CopyCall = AddCall(Copy->UbergraphPages[0], CopyFunction);
	FGraphNodeCreator<UK2Node_CreateDelegate> CopyDelegateCreator(*Copy->UbergraphPages[0]);
	UK2Node_CreateDelegate* CopyDelegate = CopyDelegateCreator.CreateNode(); CopyDelegateCreator.Finalize();
	UK2Node_CallFunction* CopyTimer = AddCall(Copy->UbergraphPages[0], UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("K2_SetTimerDelegate")));
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(CopyDelegate->GetDelegateOutPin(), CopyTimer->FindPin(TEXT("Delegate")))) return false;
	CopyDelegate->SetFunction(TEXT("Doomed"));
	if (!Save(Copy)) return false;
	FGuid SourceFunctionGuid, CopyFunctionGuid;
	UBlueprint::GetFunctionGuidFromClassByFieldName(Blueprint->SkeletonGeneratedClass, TEXT("Doomed"), SourceFunctionGuid);
	UBlueprint::GetFunctionGuidFromClassByFieldName(Copy->SkeletonGeneratedClass, TEXT("Doomed"), CopyFunctionGuid);
	const bool Collision = SourceFunctionGuid.IsValid() && SourceFunctionGuid == CopyFunctionGuid
		&& FindFunction(*Copy, TEXT("Doomed"))->GraphGuid == RemovedId;
	const bool DistinctOwner = CopyCall->GetTargetFunction()
		&& UBlueprint::GetBlueprintFromClass(CopyCall->GetTargetFunction()->GetOwnerClass()) == Copy
		&& UBlueprint::GetBlueprintFromClass(CopyDelegate->GetScopeClass()->FindFunctionByName(TEXT("Doomed"))->GetOwnerClass()) == Copy;
	AddInfo(FString::Printf(TEXT("native copied function identity: shared_guid=%d distinct_owner=%d"), Collision, DistinctOwner));
	if (!TestTrue(TEXT("native duplicate retains graph/function GUID but callers resolve to the separate copied owner"), Collision && DistinctOwner)) return false;
	const auto CopyBefore = CaptureIdentity(*Copy, FGuid());
	TArray<uint8> CopyCallBefore, CopyDelegateBefore;
	FObjectWriter CopyCallWriter(CopyCall, CopyCallBefore);
	FObjectWriter CopyDelegateWriter(CopyDelegate, CopyDelegateBefore);
	auto Expected = CaptureIdentity(*Blueprint, FGuid());
	for (auto It = Expected.CreateIterator(); It; ++It) if (It.Key() == RemovedId.ToString() || It.Key().StartsWith(RemovedId.ToString() + TEXT("/"))) It.RemoveCurrent();
	if (!Export(Workspace / TEXT("success")) || !Omit(Workspace / TEXT("success"), true)) return false;
	TWeakObjectPtr<UBlueprint> BeforeReload = Blueprint; Blueprint = nullptr; Removed = nullptr;
	int32 CopyModifications = 0;
	const auto CopyHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && Object->GetOutermost() == Copy->GetOutermost()) ++CopyModifications;
	});
	const auto Outcome = Import(Workspace / TEXT("success"));
	FCoreUObjectDelegates::OnObjectModified.Remove(CopyHandle);
	if (!Outcome.IsOk()) { AddError(Outcome.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	TArray<uint8> CopyCallAfter, CopyDelegateAfter;
	FObjectWriter CopyCallAfterWriter(CopyCall, CopyCallAfter);
	FObjectWriter CopyDelegateAfterWriter(CopyDelegate, CopyDelegateAfter);
	const bool Preserved = Blueprint && Expected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
		&& OutsideBefore.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid()))
		&& CopyBefore.OrderIndependentCompareEqual(CaptureIdentity(*Copy, FGuid())) && !Copy->GetOutermost()->IsDirty() && CopyModifications == 0
		&& CopyCallBefore == CopyCallAfter && CopyDelegateBefore == CopyDelegateAfter;
	AddInfo(FString::Printf(TEXT("copied function consumer unchanged: modifications=%d serialized_call=%d serialized_delegate=%d"),
		CopyModifications, CopyCallBefore == CopyCallAfter, CopyDelegateBefore == CopyDelegateAfter));
	const bool Reexport = Export(Workspace / TEXT("readback")) && Omit(Workspace / TEXT("readback"), false);
	AddInfo(FString::Printf(TEXT("function removal success: replaced=%d clean=%d absent=%d identities=%d reexport=%d"),
		!BeforeReload.IsValid(), Blueprint && !Blueprint->GetOutermost()->IsDirty(), Blueprint && !FindFunction(*Blueprint, TEXT("Doomed")), Preserved, Reexport));
	if (!TestTrue(TEXT("unreferenced function and recursive own call removed, unrelated same-name caller and all non-target identities preserved"),
		!BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && !FindFunction(*Blueprint, TEXT("Doomed")) && Preserved && Reexport)) return false;

	UEdGraph* AtomicFunction = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Doomed"));
	if (!Save(Blueprint)) return false;
	UEdGraph* AtomicEventGraph = Blueprint->UbergraphPages[0];
	UK2Node_CallFunction* AtomicCaller = AddCall(AtomicEventGraph, Blueprint->GeneratedClass->FindFunctionByName(TEXT("Doomed")));
	FGraphNodeCreator<UK2Node_CustomEvent> AtomicEventCreator(*AtomicEventGraph);
	UK2Node_CustomEvent* AtomicEvent = AtomicEventCreator.CreateNode(); AtomicEvent->CustomFunctionName = TEXT("InvokeAtomic"); AtomicEventCreator.Finalize();
	if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(AtomicEvent->FindPin(UEdGraphSchema_K2::PN_Then), AtomicCaller->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Save(Blueprint)) return false;
	const FGuid AtomicGraphId = AtomicFunction->GraphGuid, AtomicCallerId = AtomicCaller->NodeGuid;
	const FGuid EventGraphId = AtomicEventGraph->GraphGuid, AtomicEventId = AtomicEvent->NodeGuid;
	const FString CallerKey = EventGraphId.ToString() + TEXT("/") + AtomicCallerId.ToString();
	const FString CallerLink = AtomicCallerId.ToString() + TEXT("/") + AtomicCaller->FindPin(UEdGraphSchema_K2::PN_Execute)->PinId.ToString();
	const auto AtomicBefore = CaptureIdentity(*Blueprint, FGuid());
	auto AtomicExpected = AtomicBefore;
	for (auto It = AtomicExpected.CreateIterator(); It; ++It)
	{
		if (It.Key() == AtomicGraphId.ToString() || It.Key().StartsWith(AtomicGraphId.ToString() + TEXT("/"))
			|| It.Key() == CallerKey || It.Key().StartsWith(CallerKey + TEXT("/"))) It.RemoveCurrent();
		else It.Value() = It.Value().Replace(*CallerLink, TEXT(""));
	}
	TArray<uint8> AtomicDiskBefore; if (!FFileHelper::LoadFileToArray(AtomicDiskBefore, *Filename)) return false;
	auto BothAbsent = [&](UBlueprint& Asset) {
		if (FindFunction(Asset, TEXT("Doomed"))) return false;
		for (UEdGraph* Graph : Asset.UbergraphPages) for (UEdGraphNode* Node : Graph->Nodes) if (Node->NodeGuid == AtomicCallerId) return false;
		return true;
	};
	auto GraphRemove = MakeShared<FJsonObject>(); GraphRemove->SetStringField(TEXT("operation"), TEXT("remove"));
	GraphRemove->SetStringField(TEXT("name"), TEXT("Doomed")); GraphRemove->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
	auto CallerRemove = MakeShared<FJsonObject>(); CallerRemove->SetStringField(TEXT("operation"), TEXT("remove"));
	auto CallerSpecs = MakeShared<FJsonObject>(); CallerSpecs->SetObjectField(AtomicCallerId.ToString(EGuidFormats::DigitsWithHyphens), CallerRemove);
	auto Endpoint = [](const FGuid& Node, const TCHAR* Pin) { auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("node"), Node.ToString(EGuidFormats::DigitsWithHyphens)); Value->SetStringField(TEXT("pin"), Pin); return Value; };
	auto EdgeRemove = MakeShared<FJsonObject>(); EdgeRemove->SetStringField(TEXT("operation"), TEXT("remove"));
	EdgeRemove->SetObjectField(TEXT("from"), Endpoint(AtomicEventId, TEXT("then")));
	EdgeRemove->SetObjectField(TEXT("to"), Endpoint(AtomicCallerId, TEXT("execute")));
	auto EdgeSpecs = MakeShared<FJsonObject>(); EdgeSpecs->SetObjectField(TEXT("atomic-call-edge"), EdgeRemove);
	auto EventSpec = MakeShared<FJsonObject>(); EventSpec->SetStringField(TEXT("operation"), TEXT("update"));
	EventSpec->SetStringField(TEXT("name"), TEXT("EventGraph")); EventSpec->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	EventSpec->SetObjectField(TEXT("nodes"), CallerSpecs); EventSpec->SetObjectField(TEXT("edges"), EdgeSpecs);
	auto AtomicGraphs = MakeShared<FJsonObject>(); AtomicGraphs->SetObjectField(AtomicGraphId.ToString(EGuidFormats::DigitsWithHyphens), GraphRemove);
	AtomicGraphs->SetObjectField(EventGraphId.ToString(EGuidFormats::DigitsWithHyphens), EventSpec);
	auto AtomicExtension = MakeShared<FJsonObject>(); AtomicExtension->SetObjectField(TEXT("graphs"), AtomicGraphs);
	AtomicExtension->SetBoolField(TEXT("compile"), true); AtomicExtension->SetBoolField(TEXT("save"), true);
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag;
	Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	BeforeReload = Blueprint;
	bool Published = false;
	UeAgentBlueprintAuthoring::FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update;
	Late.AssetPath = PackageName; Late.ProviderExtension = AtomicExtension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Published = !BeforeReload.IsValid() && Current && !Current->GetOutermost()->IsDirty() && BothAbsent(*Current)
			&& AtomicExpected.OrderIndependentCompareEqual(CaptureIdentity(*Current, FGuid()))
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != AtomicDiskBefore;
		AddInfo(FString::Printf(TEXT("atomic deletion late witness: published_reloaded_both_absent=%d"), Published));
		Failure.Diagnostic.Code = TEXT("test_atomic_graph_removal_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; AtomicFunction = nullptr; AtomicCaller = nullptr; AtomicEvent = nullptr; AtomicEventGraph = nullptr;
	UeAgentBlueprintAuthoring::FAssetApplyResult LateResult;
	const bool LateApplied = UeAgentBlueprintAuthoring::ApplyAsset(Late, LateResult, TEXT("atomic-graph-removal-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	const bool Recovered = !LateApplied && Published && LateResult.Diagnostic.Code == TEXT("test_atomic_graph_removal_late_rejection")
		&& LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && Blueprint && Blueprint->GetOutermost()->IsDirty()
		&& Blueprint->bRunConstructionScriptOnDrag == !DiskFlag && AtomicBefore.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
		&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == AtomicDiskBefore;
	AddInfo(FString::Printf(TEXT("atomic deletion late recovery: memory_disk_dirty_references=%d"), Recovered));
	if (!TestTrue(TEXT("late rejection restores original function caller connection dirty memory and disk"), Recovered)) { AddError(LateResult.Diagnostic.Code + TEXT(":") + LateResult.Diagnostic.InternalContext); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString AtomicWorkspace = Workspace / TEXT("atomic-success");
	if (!Export(AtomicWorkspace) || !Omit(AtomicWorkspace, true)) return false;
	TArray<FString> AtomicFiles; IFileManager::Get().FindFilesRecursive(AtomicFiles, *AtomicWorkspace, TEXT("*.hjson"), true, false);
	int32 EditedCallers = 0;
	for (const FString& File : AtomicFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		if (!Document->HasField(TEXT("interface_ref"))) continue;
		const auto Nodes = Document->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
		for (const auto& Pair : Nodes->Values)
		{
			FString FunctionName; if (Pair.Value->AsObject()->TryGetStringField(TEXT("function_graph"), FunctionName) && FunctionName == TEXT("Doomed")) Deleted.Add(Pair.Key);
		}
		if (Deleted.IsEmpty()) continue;
		for (const FString& Key : Deleted) { Nodes->RemoveField(Key); ++EditedCallers; }
		TArray<TSharedPtr<FJsonValue>> Connections = Document->GetArrayField(TEXT("connections"));
		Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) {
			const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); });
		});
		Document->SetArrayField(TEXT("connections"), Connections);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestEqual(TEXT("ordinary workspace omits exactly one caller"), EditedCallers, 1)) return false;
	BeforeReload = Blueprint; Blueprint = nullptr;
	const auto AtomicImported = Import(AtomicWorkspace);
	if (!AtomicImported.IsOk()) { AddError(AtomicImported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const bool AtomicReadback = Export(Workspace / TEXT("atomic-readback")) && Omit(Workspace / TEXT("atomic-readback"), false);
	const bool AtomicSuccess = !BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && BothAbsent(*Blueprint)
		&& AtomicExpected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid())) && AtomicReadback;
	AddInfo(FString::Printf(TEXT("atomic public deletion: replaced_clean_both_absent_non_targets_reexport=%d"), AtomicSuccess));
	if (!TestTrue(TEXT("one public import removes explicit caller and function then saves reloads and independently exports"), AtomicSuccess)) return false;
	UEdGraph* SetA = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("SetA"));
	UEdGraph* SetB = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("SetB"));
	if (!Save(Blueprint)) return false;
	UK2Node_CallFunction* SetCaller = AddCall(SetA, Blueprint->GeneratedClass->FindFunctionByName(TEXT("SetB")));
	TArray<UK2Node_FunctionEntry*> SetEntries; SetA->GetNodesOfClass(SetEntries);
	if (SetEntries.Num() != 1 || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(
		SetEntries[0]->FindPin(UEdGraphSchema_K2::PN_Then), SetCaller->FindPin(UEdGraphSchema_K2::PN_Execute)) || !Save(Blueprint)) return false;
	const FGuid SetAId = SetA->GraphGuid, SetBId = SetB->GraphGuid;
	auto OmitSet = [&](const FString& Path, const bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			if (Edit)
			{
				if (!(*Declarations)->HasField(TEXT("SetA")) || !(*Declarations)->HasField(TEXT("SetB"))) return false;
				(*Declarations)->RemoveField(TEXT("SetA")); (*Declarations)->RemoveField(TEXT("SetB"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			else if ((*Declarations)->HasField(TEXT("SetA")) || (*Declarations)->HasField(TEXT("SetB"))) return false;
			++Count;
		}
		return Count == 1;
	};
	UK2Node_CallFunction* OutsideSet = AddCall(Blueprint->UbergraphPages[0], Blueprint->GeneratedClass->FindFunctionByName(TEXT("SetB")));
	if (!Save(Blueprint) || !Export(Workspace / TEXT("set-outside")) || !OmitSet(Workspace / TEXT("set-outside"), true)) return false;
	const auto OutsideSetBefore = CaptureIdentity(*Blueprint, FGuid());
	TArray<uint8> SetRejectDiskBefore, SetRejectDiskAfter; FFileHelper::LoadFileToArray(SetRejectDiskBefore, *Filename);
	int32 SetWrites = 0;
	const auto SetHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && Object->GetOutermost() == Blueprint->GetOutermost()) ++SetWrites;
	});
	const auto RejectedSet = Import(Workspace / TEXT("set-outside"));
	FCoreUObjectDelegates::OnObjectModified.Remove(SetHandle);
	if (RejectedSet.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("set outside caller unexpectedly accepted")); return false; }
	const bool SetUnchanged = OutsideSetBefore.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
		&& !Blueprint->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(SetRejectDiskAfter, *Filename) && SetRejectDiskBefore == SetRejectDiskAfter;
	AddInfo(FString::Printf(TEXT("graph deletion set outside caller: writes=%d unchanged=%d"), SetWrites, SetUnchanged));
	if (!TestTrue(TEXT("outside-set caller remains a prewrite rejection"), SetWrites == 0 && SetUnchanged
		&& RejectedSet.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported")))) return false;
	OutsideSet->DestroyNode(); OutsideSet = nullptr;
	if (!Save(Blueprint)) return false;
	const auto SetBefore = CaptureIdentity(*Blueprint, FGuid());
	auto SetExpected = SetBefore;
	for (auto It = SetExpected.CreateIterator(); It; ++It)
		if (It.Key() == SetAId.ToString() || It.Key().StartsWith(SetAId.ToString() + TEXT("/"))
			|| It.Key() == SetBId.ToString() || It.Key().StartsWith(SetBId.ToString() + TEXT("/"))) It.RemoveCurrent();
	TArray<uint8> SetDiskBefore; if (!FFileHelper::LoadFileToArray(SetDiskBefore, *Filename)) return false;
	auto SetAbsent = [&](UBlueprint& Asset) { return !FindFunction(Asset, TEXT("SetA")) && !FindFunction(Asset, TEXT("SetB")); };
	auto SetGraphs = MakeShared<FJsonObject>();
	for (UEdGraph* Graph : {SetA, SetB})
	{
		auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("operation"), TEXT("remove"));
		Spec->SetStringField(TEXT("name"), Graph->GetName()); Spec->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
		SetGraphs->SetObjectField(Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens), Spec);
	}
	auto SetExtension = MakeShared<FJsonObject>(); SetExtension->SetObjectField(TEXT("graphs"), SetGraphs);
	SetExtension->SetBoolField(TEXT("compile"), true); SetExtension->SetBoolField(TEXT("save"), true);
	const bool SetDiskFlag = Blueprint->bRunConstructionScriptOnDrag;
	Blueprint->bRunConstructionScriptOnDrag = !SetDiskFlag; Blueprint->MarkPackageDirty(); BeforeReload = Blueprint;
	bool SetPublished = false;
	UeAgentBlueprintAuthoring::FAssetApplyRequest SetLate; SetLate.Operation = UeAgentApplyInputContract::EOperation::Update;
	SetLate.AssetPath = PackageName; SetLate.ProviderExtension = SetExtension;
	SetLate.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		SetPublished = !BeforeReload.IsValid() && Current && !Current->GetOutermost()->IsDirty() && SetAbsent(*Current)
			&& SetExpected.OrderIndependentCompareEqual(CaptureIdentity(*Current, FGuid()))
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != SetDiskBefore;
		AddInfo(FString::Printf(TEXT("graph deletion set late witness: saved_reloaded_both_absent=%d"), SetPublished));
		Failure.Diagnostic.Code = TEXT("test_graph_deletion_set_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; SetA = nullptr; SetB = nullptr; SetCaller = nullptr; SetEntries.Reset();
	UeAgentBlueprintAuthoring::FAssetApplyResult SetLateResult;
	const bool SetApplied = UeAgentBlueprintAuthoring::ApplyAsset(SetLate, SetLateResult, TEXT("graph-deletion-set-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> SetRestoredDisk;
	const bool SetRestored = !SetApplied && SetPublished && SetLateResult.Diagnostic.Code == TEXT("test_graph_deletion_set_late_rejection")
		&& SetLateResult.bRollbackAttempted && SetLateResult.bRollbackSucceeded && Blueprint && Blueprint->GetOutermost()->IsDirty()
		&& Blueprint->bRunConstructionScriptOnDrag == !SetDiskFlag && SetBefore.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid()))
		&& FFileHelper::LoadFileToArray(SetRestoredDisk, *Filename) && SetRestoredDisk == SetDiskBefore;
	AddInfo(FString::Printf(TEXT("graph deletion set late recovery: original_graphs_references_memory_disk_dirty=%d"), SetRestored));
	if (!TestTrue(TEXT("set late rejection restores original graphs and their internal call"), SetRestored)) { AddError(SetLateResult.Diagnostic.Code + TEXT(":") + SetLateResult.Diagnostic.InternalContext); return false; }
	Blueprint->bRunConstructionScriptOnDrag = SetDiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	if (!Export(Workspace / TEXT("set-success")) || !OmitSet(Workspace / TEXT("set-success"), true)) return false;
	BeforeReload = Blueprint; Blueprint = nullptr;
	const auto SetImported = Import(Workspace / TEXT("set-success"));
	if (!SetImported.IsOk()) { AddError(SetImported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const bool SetReexport = Export(Workspace / TEXT("set-readback")) && OmitSet(Workspace / TEXT("set-readback"), false);
	const bool SetSuccess = !BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty() && SetAbsent(*Blueprint)
		&& SetExpected.OrderIndependentCompareEqual(CaptureIdentity(*Blueprint, FGuid())) && SetReexport;
	AddInfo(FString::Printf(TEXT("public graph deletion set: saved_reloaded_both_absent_non_targets_reexport=%d"), SetSuccess));
	return TestTrue(TEXT("one public import removes both declarations including the internal A to B call"), SetSuccess) && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintMacroRenameTest,
	"UeAgentInterface.T208.Blueprint.MacroRenameWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintMacroRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85MacroRename_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	FString CurrentObjectPath = ObjectPath;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-macro-rename/native") / Token);
	TArray<FString> MacroOwned{ObjectPath};
	ON_SCOPE_EXIT
	{
		for (int32 Index = MacroOwned.Num() - 1; Index >= 0; --Index) if (UObject* Object = LoadObject<UObject>(nullptr, *MacroOwned[Index]))
		{
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned macro fixture deleted"), ObjectTools::DeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName),
		TEXT("BP_Owner"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("SelectStatus"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Blueprint, Macro, true, nullptr);
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	auto Pin = [](const TCHAR* Name, const TCHAR* Category) {
		auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("name"), Name);
		auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), Category); Value->SetObjectField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(Value);
	};
	auto Signature = MakeShared<FJsonObject>();
	Signature->SetArrayField(TEXT("inputs"), {Pin(TEXT("Enabled"), TEXT("bool"))});
	Signature->SetArrayField(TEXT("outputs"), {Pin(TEXT("Succeeded"), TEXT("bool"))});
	if (!ApplyMacroGraphSignature(*Macro, Signature, Error)) { AddError(Error.ToPublicDetail()); return false; }
	UK2Node_Tunnel* Entry = nullptr; UK2Node_Tunnel* Result = nullptr;
	for (UEdGraphNode* Node : Macro->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node))
	{
		if (Tunnel->bCanHaveOutputs) Entry = Tunnel;
		if (Tunnel->bCanHaveInputs) Result = Tunnel;
	}
	if (!Entry || !Result) return false;
	FEdGraphPinType ExecType; ExecType.PinCategory = UEdGraphSchema_K2::PC_Exec;
	Entry->CreateUserDefinedPin(TEXT("In"), ExecType, EGPD_Output, false);
	Result->CreateUserDefinedPin(TEXT("Out"), ExecType, EGPD_Input, false);
	Entry->NodePosX = -320; Result->NodePosX = 320;
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema->TryCreateConnection(Entry->FindPin(TEXT("In")), Result->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(Entry->FindPin(TEXT("Enabled")), Result->FindPin(TEXT("Succeeded")))) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> CallerCreator(*Blueprint->UbergraphPages[0]);
	UK2Node_MacroInstance* Caller = CallerCreator.CreateNode(); Caller->SetMacroGraph(Macro); CallerCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Blueprint->UbergraphPages[0]);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode(); Event->CustomFunctionName = TEXT("InvokeStatus"); EventCreator.Finalize();
	if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Caller->FindPin(TEXT("In")))) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error) return false;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Save)) return false;
	const FGuid MacroId = Macro->GraphGuid, CallerId = Caller->NodeGuid;
	const auto Before = CaptureIdentity(*Blueprint, MacroId);
	auto BeforeSignature = MakeShared<FJsonObject>(); FString SignatureText;
	if (!CaptureMacroGraphSignature(*Macro, BeforeSignature, Error)
		|| !FUeAgentStructuredHjsonFormatter::TryWriteObject(BeforeSignature, SignatureText, Error)) return false;
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Read = [&](UBlueprint* Current, const FString& Name) {
		if (!Current || Current->Status == BS_Error) return false;
		UEdGraph* Target = nullptr;
		for (UEdGraph* Graph : Current->MacroGraphs) if (Graph->GraphGuid == MacroId && Graph->GetName() == Name) Target = Graph;
		if (!Target) return false;
		bool CallerMatches = false;
		for (UEdGraphNode* Node : Current->UbergraphPages[0]->Nodes)
			if (auto* Instance = Cast<UK2Node_MacroInstance>(Node); Instance && Instance->NodeGuid == CallerId)
				CallerMatches = Instance->GetMacroGraph() == Target && Instance->GetMacroGraph()->GetName() == Name;
		auto ActualSignature = MakeShared<FJsonObject>(); FString ActualSignatureText;
		return CallerMatches && Before.OrderIndependentCompareEqual(CaptureIdentity(*Current, MacroId))
			&& CaptureMacroGraphSignature(*Target, ActualSignature, Error)
			&& FUeAgentStructuredHjsonFormatter::TryWriteObject(ActualSignature, ActualSignatureText, Error)
			&& SignatureText == ActualSignatureText;
	};
	const bool DiskWarning = Blueprint->bRunConstructionScriptOnDrag;
	Blueprint->bRunConstructionScriptOnDrag = !DiskWarning; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> BeforeReload = Blueprint; bool Published = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update;
	Late.AssetPath = PackageName; Late.ProviderExtension = RenameExtension(MacroId, TEXT("EvaluateStatus"), TEXT("macro_graph"));
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Published = !BeforeReload.IsValid() && Current && !Current->GetOutermost()->IsDirty()
			&& Read(Current, TEXT("EvaluateStatus")) && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("macro rename late witness: saved_reloaded_name_caller_signature_identity_links=%d"), Published));
		Failure.Diagnostic.Code = TEXT("test_macro_rename_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Macro = nullptr; Entry = nullptr; Result = nullptr; Caller = nullptr; Event = nullptr;
	FAssetApplyResult LateResult;
	const bool LateApplied = ApplyAsset(Late, LateResult, TEXT("macro-rename-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RestoredDisk;
	const bool Restored = !LateApplied && Published && LateResult.bRollbackSucceeded && Blueprint
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskWarning
		&& Read(Blueprint, TEXT("SelectStatus")) && FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("macro rename late recovery: original_name_caller_identity_signature_memory_disk_dirty=%d"), Restored));
	if (!TestTrue(TEXT("macro late recovery"), Restored)) { AddError(LateResult.Diagnostic.Code + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskWarning; Blueprint->GetOutermost()->SetDirtyFlag(false);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Outcome = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("macro-rename-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Outcome.IsOk()) AddError(Outcome.GetErrorJson()); return Outcome.IsOk();
	};
	auto Declaration = [&](const FString& Path, bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Document;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr;
			if (!Document->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (!(*Declarations)->TryGetObjectField(Edit ? TEXT("SelectStatus") : TEXT("EvaluateStatus"), Row) || (*Row)->HasField(TEXT("name"))) return false;
			++Matches;
			if (Edit) { (*Row)->SetStringField(TEXT("name"), TEXT("EvaluateStatus"));
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false; }
		}
		return Matches == 1;
	};
	if (!Export(Workspace / TEXT("edit")) || !Declaration(Workspace / TEXT("edit"), true)) return false;
	auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace / TEXT("edit"));
	auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
	BeforeReload = Blueprint; Blueprint = nullptr;
	const auto Imported = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("macro-rename-import"));
	if (!Imported.IsOk()) { AddError(Imported.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const bool Success = !BeforeReload.IsValid() && Blueprint && !Blueprint->GetOutermost()->IsDirty()
		&& Read(Blueprint, TEXT("EvaluateStatus")) && Export(Workspace / TEXT("readback")) && Declaration(Workspace / TEXT("readback"), false);
	AddInfo(FString::Printf(TEXT("public macro rename: saved_reloaded_name_caller_signature_identity_links_non_targets_reexport=%d"), Success));
	TestTrue(TEXT("public macro rename full readback"), Success);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintMacroLibraryRenameTest,
	"UeAgentInterface.T208.Blueprint.MacroLibraryRenameWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintMacroLibraryRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI; using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error; if (!RegisterAuthoring(Error)) return false;
	const FString Root = TEXT("/Game/UAI/Transient/T85MacroLibraryRename_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = Root / TEXT("BP_Library"), ObjectPath = PackageName + TEXT(".BP_Library");
	const FString ExternalPackage = Root / TEXT("BP_External"), ExternalPath = ExternalPackage + TEXT(".BP_External");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString ExternalFilename = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-macro-rename/library") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT {
		for (const FString& Path : {ExternalPath, ObjectPath}) if (auto* Object = LoadObject<UBlueprint>(nullptr, *Path))
		{
			TArray<UObject*> OwnedSelection{Object};
			// Native name reuse leaves standalone redirectors outside the asset's
			// outer. Include only this fixture's derived references in its cleanup.
			for (TObjectIterator<UObjectRedirector> It; It; ++It)
				if (It->GetOuter() == GetTransientPackage() && It->DestinationObject
					&& It->DestinationObject->GetTypedOuter<UBlueprint>() == Object)
					OwnedSelection.Add(*It);
			Object->GetOutermost()->SetDirtyFlag(false);
			const int32 Deleted = ObjectTools::ForceDeleteObjects(OwnedSelection, false);
			const FString File = FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(Path), FPackageName::GetAssetPackageExtension());
			TestTrue(TEXT("library rename owned fixture and derived redirectors cleaned"), Deleted > 0 && !IFileManager::Get().FileExists(*File));
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto Create = [&](const FString& Path, EBlueprintType Type) {
		auto* BP = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), Type, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) FAssetRegistryModule::AssetCreated(BP); return BP;
	};
	auto Save = [&](UBlueprint* BP) {
		FKismetEditorUtilities::CompileBlueprint(BP); if (BP->Status == BS_Error) return false;
		FSavePackageArgs Options; Options.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(BP->GetOutermost(), BP, *FPackageName::LongPackageNameToFilename(BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Options);
	};
	UBlueprint* Library = Create(PackageName, BPTYPE_MacroLibrary); if (!Library) return false;
	UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(Library, TEXT("SelectStatus"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Library, Macro, true, nullptr);
	UEdGraph* Untouched = FBlueprintEditorUtils::CreateNewGraph(Library, TEXT("Untouched"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddMacroGraph(Library, Untouched, true, nullptr);
	UK2Node_Tunnel* Entry = nullptr; UK2Node_Tunnel* Exit = nullptr;
	for (UEdGraphNode* Node : Macro->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node)) { if (Tunnel->bCanHaveOutputs) Entry = Tunnel; if (Tunnel->bCanHaveInputs) Exit = Tunnel; }
	if (!Entry || !Exit) return false;
	FEdGraphPinType Exec; Exec.PinCategory = UEdGraphSchema_K2::PC_Exec;
	FEdGraphPinType Bool; Bool.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	Entry->CreateUserDefinedPin(TEXT("In"), Exec, EGPD_Output, false); Exit->CreateUserDefinedPin(TEXT("Out"), Exec, EGPD_Input, false);
	Entry->CreateUserDefinedPin(TEXT("Enabled"), Bool, EGPD_Output, false); Exit->CreateUserDefinedPin(TEXT("Succeeded"), Bool, EGPD_Input, false);
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Schema->TryCreateConnection(Entry->FindPin(TEXT("In")), Exit->FindPin(TEXT("Out"))) || !Schema->TryCreateConnection(Entry->FindPin(TEXT("Enabled")), Exit->FindPin(TEXT("Succeeded")))) return false;
	UK2Node_Tunnel* OtherEntry = nullptr; UK2Node_Tunnel* OtherExit = nullptr;
	for (UEdGraphNode* Node : Untouched->Nodes) if (auto* Tunnel = Cast<UK2Node_Tunnel>(Node)) { if (Tunnel->bCanHaveOutputs) OtherEntry = Tunnel; if (Tunnel->bCanHaveInputs) OtherExit = Tunnel; }
	if (!OtherEntry || !OtherExit) return false;
	OtherEntry->CreateUserDefinedPin(TEXT("In"), Exec, EGPD_Output, false); OtherExit->CreateUserDefinedPin(TEXT("Out"), Exec, EGPD_Input, false);
	OtherEntry->CreateUserDefinedPin(TEXT("Enabled"), Bool, EGPD_Output, false); OtherExit->CreateUserDefinedPin(TEXT("Succeeded"), Bool, EGPD_Input, false);
	if (!Schema->TryCreateConnection(OtherEntry->FindPin(TEXT("In")), OtherExit->FindPin(TEXT("Out")))
		|| !Schema->TryCreateConnection(OtherEntry->FindPin(TEXT("Enabled")), OtherExit->FindPin(TEXT("Succeeded"))) || !Save(Library)) return false;
	const FGuid OtherMacroId = Untouched->GraphGuid;
	const FGuid MacroId = Macro->GraphGuid; const auto Before = CaptureIdentity(*Library, MacroId);
	UBlueprint* External = Create(ExternalPackage, BPTYPE_Normal); if (!External) return false;
	FGraphNodeCreator<UK2Node_MacroInstance> Creator(*External->UbergraphPages[0]); auto* Caller = Creator.CreateNode(); Caller->SetMacroGraph(Macro); Creator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*External->UbergraphPages[0]); auto* Event = EventCreator.CreateNode(); Event->CustomFunctionName = TEXT("InvokeStatus"); EventCreator.Finalize();
	if (!Schema->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Caller->FindPin(TEXT("In"))) || !Save(External)) return false;
	const FGuid CallerId = Caller->NodeGuid; const auto ExternalBefore = CaptureIdentity(*External, FGuid());
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFilename)) return false;
	TArray<uint8> NodeBefore, NodeAfter; FObjectWriter BeforeWriter(Caller, NodeBefore); int32 Writes = 0;
	const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) { if (Object && Object->GetOutermost() == External->GetOutermost()) ++Writes; });
	const bool Allowed = FPreparedBlueprintMutation::ValidateGraphNames(*Library, RenameExtension(MacroId, TEXT("EvaluateStatus"), TEXT("macro_graph")), Error);
	FCoreUObjectDelegates::OnObjectModified.Remove(Handle); FObjectWriter AfterWriter(Caller, NodeAfter);
	if (!TestTrue(TEXT("loaded external macro reference is read-only and allowed"), Allowed && Writes == 0 && NodeBefore == NodeAfter && !External->GetOutermost()->IsDirty())) { AddError(Error.ToPublicDetail()); return false; }
	UPackage* Unload = External->GetOutermost(); External = nullptr; Caller = nullptr; Event = nullptr;
	if (!UPackageTools::UnloadPackages({Unload}) || FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry; TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		auto Outcome = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("macro-library-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Outcome.IsOk()) AddError(Outcome.GetErrorJson()); return Outcome.IsOk();
	};
	auto Declaration = [&](const FString& Path, const FString& OldName, const FString& NewName) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr; if (!Doc->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			const TSharedPtr<FJsonObject>* Row = nullptr; if (!(*Declarations)->TryGetObjectField(OldName, Row) || (*Row)->HasField(TEXT("name"))) return false;
			++Matches; if (!NewName.IsEmpty()) { (*Row)->SetStringField(TEXT("name"), NewName);
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false; }
		} return Matches == 1;
	};
	auto PublicRename = [&](const FString& Folder, const FString& OldName, const FString& NewName) {
		if (!Export(Workspace / Folder) || !Declaration(Workspace / Folder, OldName, NewName)) return false;
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace / Folder);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		TWeakObjectPtr<UBlueprint> Prior = Library; Library = nullptr; Macro = nullptr; Entry = nullptr; Exit = nullptr; Untouched = nullptr;
		auto Outcome = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("macro-library-rename"));
		if (!Outcome.IsOk()) { AddError(Outcome.GetErrorJson()); return false; }
		Library = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		return !Prior.IsValid() && Library && !Library->GetOutermost()->IsDirty() && Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId))
			&& Export(Workspace / (Folder + TEXT("-readback"))) && Declaration(Workspace / (Folder + TEXT("-readback")), NewName, FString());
	};
	auto CheckExternal = [&](const FString& Name) {
		if (FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;
		External = LoadObject<UBlueprint>(nullptr, *ExternalPath); if (!External) return false;
		bool Matches = false; for (UEdGraphNode* Node : External->UbergraphPages[0]->Nodes)
			if (auto* Instance = Cast<UK2Node_MacroInstance>(Node); Instance && Instance->NodeGuid == CallerId)
			{ auto* Graph = Instance->GetMacroGraph(); Matches = Graph && Graph->GraphGuid == MacroId && Graph->GetName() == Name && Graph->GetTypedOuter<UBlueprint>() == Library; }
		FKismetEditorUtilities::CompileBlueprint(External); TArray<uint8> Disk;
		const bool Valid = Matches && External->Status != BS_Error && ExternalBefore.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid()))
			&& FFileHelper::LoadFileToArray(Disk, *ExternalFilename) && Disk == ExternalDisk;
		External->GetOutermost()->SetDirtyFlag(false); Unload = External->GetOutermost(); External = nullptr;
		return Valid && UPackageTools::UnloadPackages({Unload});
	};
	if (!PublicRename(TEXT("first"), TEXT("SelectStatus"), TEXT("EvaluateStatus"))) return false;
	auto* Redirector = FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus"));
	const bool First = Redirector && Redirector->DestinationObject && Redirector->DestinationObject->GetName() == TEXT("EvaluateStatus") && CheckExternal(TEXT("EvaluateStatus"));
	AddInfo(FString::Printf(TEXT("macro library rename: saved_redirector_external_old_import_load_compile_unchanged=%d"), First));
	if (!TestTrue(TEXT("macro library persisted old-name redirector"), First)) return false;
	TArray<uint8> BeforeDisk; FFileHelper::LoadFileToArray(BeforeDisk, *Filename);
	const bool DiskFlag = Library->bRunConstructionScriptOnDrag; Library->bRunConstructionScriptOnDrag = !DiskFlag; Library->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Prior = Library; bool Published = false;
	TWeakObjectPtr<UBlueprint> PublishedLibrary;
	TWeakObjectPtr<UObjectRedirector> ReusedRedirector = Redirector;
	const FDelegateHandle SaveObserver = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext) {
		if (!Object || Object->GetPathName() != ObjectPath) return;
		TArray<UObject*> Children; GetObjectsWithOuter(Object, Children, false);
		for (UObject* Child : Children) if (Child->IsA<UEdGraph>() || Child->IsA<UObjectRedirector>())
		{
			const auto* Redirect = Cast<UObjectRedirector>(Child);
			AddInfo(FString::Printf(TEXT("macro library presave child: name=%s class=%s public=%d standalone=%d transient=%d destination=%s"),
				*Child->GetName(), *Child->GetClass()->GetName(), Child->HasAnyFlags(RF_Public), Child->HasAnyFlags(RF_Standalone), Child->HasAnyFlags(RF_Transient),
				Redirect ? *GetNameSafe(Redirect->DestinationObject) : TEXT("none")));
		}
	});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveObserver); };
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName;
	Late.ProviderExtension = RenameExtension(MacroId, TEXT("SelectStatus"), TEXT("macro_graph"));
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		PublishedLibrary = Library;
		auto* Reverse = Library ? FindObjectFast<UObjectRedirector>(Library, TEXT("EvaluateStatus")) : nullptr;
		const bool DiskRead = FFileHelper::LoadFileToArray(Disk, *Filename);
		if (auto* Reused = ReusedRedirector.Get())
			AddInfo(FString::Printf(TEXT("macro library moved redirector: outer_transient=%d rooted=%d standalone=%d public=%d transient=%d destination=%s keeps_prior_owner=%d"),
				Reused->GetOuter() == GetTransientPackage(), Reused->IsRooted(), Reused->HasAnyFlags(RF_Standalone), Reused->HasAnyFlags(RF_Public), Reused->HasAnyFlags(RF_Transient),
				*GetNameSafe(Reused->DestinationObject), Reused->DestinationObject && Reused->DestinationObject->GetTypedOuter<UBlueprint>() == Prior.Get()));
		AddInfo(FString::Printf(TEXT("macro library reuse late detail: replaced=%d loaded=%d clean=%d reverse=%d destination=%s identity=%d disk_read=%d disk_changed=%d"),
			!Prior.IsValid(), Library != nullptr, Library && !Library->GetOutermost()->IsDirty(), Reverse != nullptr,
			Reverse ? *GetNameSafe(Reverse->DestinationObject) : TEXT("none"),
			Library && Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId)),
			DiskRead, Disk != BeforeDisk));
		if (Library) for (UEdGraph* Graph : Library->MacroGraphs)
			AddInfo(FString::Printf(TEXT("macro library reuse late graph: name=%s target_identity=%d"), *Graph->GetName(), Graph->GraphGuid == MacroId));
		Published = !Prior.IsValid() && Library && !Library->GetOutermost()->IsDirty() && Reverse
			&& Reverse->DestinationObject && Reverse->DestinationObject->GetName() == TEXT("SelectStatus")
			&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId)) && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != BeforeDisk;
		AddInfo(FString::Printf(TEXT("macro library reuse late witness: saved_reloaded_graph_reverse_redirector=%d"), Published));
		Failure.Diagnostic.Code = TEXT("test_macro_library_reuse_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Library = nullptr; Redirector = nullptr; FAssetApplyResult LateResult;
	const bool Applied = ApplyAsset(Late, LateResult, TEXT("macro-library-reuse-late"));
	Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RestoredDisk;
	Redirector = Library ? FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus")) : nullptr;
	const bool RestoredDiskRead = FFileHelper::LoadFileToArray(RestoredDisk, *Filename);
	AddInfo(FString::Printf(TEXT("macro library recovery detail: applied=%d rollback_attempted=%d rollback_succeeded=%d loaded=%d dirty=%d memory_flag=%d redirector=%d destination=%s identity=%d disk_read=%d disk_original=%d"),
		Applied, LateResult.bRollbackAttempted, LateResult.bRollbackSucceeded, Library != nullptr,
		Library && Library->GetOutermost()->IsDirty(), Library && Library->bRunConstructionScriptOnDrag == !DiskFlag,
		Redirector != nullptr, Redirector ? *GetNameSafe(Redirector->DestinationObject) : TEXT("none"),
		Library && Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId)),
		RestoredDiskRead, RestoredDisk == BeforeDisk));
	const bool Restored = !Applied && Published && LateResult.bRollbackSucceeded && !PublishedLibrary.IsValid() && Library && Library->GetOutermost()->IsDirty()
		&& Library->bRunConstructionScriptOnDrag == !DiskFlag && Redirector && Redirector->DestinationObject && Redirector->DestinationObject->GetName() == TEXT("EvaluateStatus")
		&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId)) && FFileHelper::LoadFileToArray(RestoredDisk, *Filename) && RestoredDisk == BeforeDisk;
	AddInfo(FString::Printf(TEXT("macro library reuse late recovery: original_graph_redirector_memory_disk_dirty=%d"), Restored));
	if (!TestTrue(TEXT("macro library redirector restored by public transaction"), Restored)) { AddError(LateResult.Diagnostic.Code + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	Library->bRunConstructionScriptOnDrag = DiskFlag; Library->GetOutermost()->SetDirtyFlag(false);
	if (!TestTrue(TEXT("external old-name import independently loads after checkpoint restoration"), CheckExternal(TEXT("EvaluateStatus")))) return false;
	if (!PublicRename(TEXT("reuse"), TEXT("EvaluateStatus"), TEXT("SelectStatus"))) return false;
	const bool Reused = CheckExternal(TEXT("SelectStatus"));
	AddInfo(FString::Printf(TEXT("macro library public old name reuse: external_original_import_unchanged=%d"), Reused));
	if (!TestTrue(TEXT("macro library old name reuse"), Reused)) return false;

	// Keep the original external UObject and its unsaved user edits resident.
	External = LoadObject<UBlueprint>(nullptr, *ExternalPath); if (!External) return false;
	Caller = nullptr;
	for (UEdGraphNode* Node : External->UbergraphPages[0]->Nodes)
		if (auto* Instance = Cast<UK2Node_MacroInstance>(Node); Instance && Instance->NodeGuid == CallerId) Caller = Instance;
	if (!Caller) return false;
	const bool ExternalFlag = External->bRunConstructionScriptOnDrag;
	const int32 ExternalX = Caller->NodePosX;
	External->bRunConstructionScriptOnDrag = !ExternalFlag; Caller->NodePosX += 73; External->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> ResidentExternal = External; TWeakObjectPtr<UK2Node_MacroInstance> ResidentCaller = Caller;
	const auto DirtyExternalIdentity = CaptureIdentity(*External, FGuid());
	auto CheckLoaded = [&](const FString& Name, const TCHAR* Stage) {
		TArray<uint8> Disk;
		const bool SameObject = External && External == ResidentExternal.Get() && FindObject<UBlueprint>(nullptr, *ExternalPath) == External;
		const bool SameCaller = Caller && Caller == ResidentCaller.Get();
		UEdGraph* Target = SameCaller ? Caller->GetMacroGraph() : nullptr;
		const bool TargetMatches = Target && Target->GetTypedOuter<UBlueprint>() == Library && Target->GraphGuid == MacroId && Target->GetName() == Name;
		const bool MemoryMatches = SameObject && External->GetOutermost()->IsDirty() && External->bRunConstructionScriptOnDrag == !ExternalFlag
			&& DirtyExternalIdentity.OrderIndependentCompareEqual(CaptureIdentity(*External, FGuid()));
		const bool DiskMatches = FFileHelper::LoadFileToArray(Disk, *ExternalFilename) && Disk == ExternalDisk;
		AddInfo(FString::Printf(TEXT("macro library loaded external %s: same_object=%d same_caller=%d target_owner_guid_name=%d dirty_memory_identity_links=%d disk_original=%d"),
			Stage, SameObject, SameCaller, TargetMatches, MemoryMatches, DiskMatches));
		return SameObject && SameCaller && TargetMatches && MemoryMatches && DiskMatches;
	};
	if (!CheckLoaded(TEXT("SelectStatus"), TEXT("before")) || !PublicRename(TEXT("loaded"), TEXT("SelectStatus"), TEXT("EvaluateStatus"))) return false;
	if (!TestTrue(TEXT("loaded external dirty memory survives source rename without unloading"), CheckLoaded(TEXT("EvaluateStatus"), TEXT("after_rename")))) return false;
	TArray<uint8> LoadedSourceDisk; if (!FFileHelper::LoadFileToArray(LoadedSourceDisk, *Filename)) return false;
	Library->bRunConstructionScriptOnDrag = !DiskFlag; Library->MarkPackageDirty();
	Prior = Library; bool LoadedPublished = false;
	FAssetApplyRequest LoadedLate; LoadedLate.Operation = UeAgentApplyInputContract::EOperation::Update; LoadedLate.AssetPath = PackageName;
	LoadedLate.ProviderExtension = RenameExtension(MacroId, TEXT("SelectStatus"), TEXT("macro_graph"));
	LoadedLate.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); PublishedLibrary = Library; TArray<uint8> Disk;
		auto* Reverse = Library ? FindObjectFast<UObjectRedirector>(Library, TEXT("EvaluateStatus")) : nullptr;
		const bool ExternalLate = CheckLoaded(TEXT("SelectStatus"), TEXT("late_saved"));
		LoadedPublished = !Prior.IsValid() && Library && !Library->GetOutermost()->IsDirty() && Reverse
			&& Reverse->DestinationObject && Reverse->DestinationObject->GetName() == TEXT("SelectStatus")
			&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId))
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != LoadedSourceDisk
			&& ExternalLate;
		AddInfo(FString::Printf(TEXT("macro library loaded late witness: source_replaced_saved_external_preserved=%d"), LoadedPublished));
		Failure.Diagnostic.Code = TEXT("test_macro_library_loaded_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Library = nullptr; FAssetApplyResult LoadedLateResult;
	const bool LoadedApplied = ApplyAsset(LoadedLate, LoadedLateResult, TEXT("macro-library-loaded-late"));
	Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> LoadedRestoredDisk;
	Redirector = Library ? FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus")) : nullptr;
	const bool ExternalRestored = CheckLoaded(TEXT("EvaluateStatus"), TEXT("after_restore"));
	const bool LoadedRestored = !LoadedApplied && LoadedPublished && LoadedLateResult.bRollbackSucceeded && !PublishedLibrary.IsValid()
		&& Library && Library->GetOutermost()->IsDirty() && Library->bRunConstructionScriptOnDrag == !DiskFlag
		&& Redirector && Redirector->DestinationObject && Redirector->DestinationObject->GetName() == TEXT("EvaluateStatus")
		&& Before.OrderIndependentCompareEqual(CaptureIdentity(*Library, MacroId))
		&& FFileHelper::LoadFileToArray(LoadedRestoredDisk, *Filename) && LoadedRestoredDisk == LoadedSourceDisk
		&& ExternalRestored;
	AddInfo(FString::Printf(TEXT("macro library loaded late recovery: source_and_external_memory_disk_dirty=%d"), LoadedRestored));
	if (!TestTrue(TEXT("loaded external dirty memory survives checkpoint restoration without unloading"), LoadedRestored)) { AddError(LoadedLateResult.Diagnostic.Code + TEXT(":") + LoadedLateResult.Diagnostic.PublicMessage); return false; }
	Library->bRunConstructionScriptOnDrag = DiskFlag; Library->GetOutermost()->SetDirtyFlag(false);
	External->bRunConstructionScriptOnDrag = ExternalFlag; Caller->NodePosX = ExternalX; External->GetOutermost()->SetDirtyFlag(false);
	Unload = External->GetOutermost(); External = nullptr; Caller = nullptr;
	if (!UPackageTools::UnloadPackages({Unload}) || FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;

	// The redirector still owns the old import name for the original macro.
	// A distinct macro must be rejected before the first source modification.
	if (!Export(Workspace / TEXT("different-owner")) || !Declaration(Workspace / TEXT("different-owner"), TEXT("Untouched"), TEXT("SelectStatus"))) return false;
	auto CrossSource = MakeShared<FJsonObject>(); CrossSource->SetStringField(TEXT("kind"), TEXT("directory")); CrossSource->SetStringField(TEXT("path"), Workspace / TEXT("different-owner"));
	auto CrossArgs = MakeShared<FJsonObject>(); CrossArgs->SetStringField(TEXT("target"), ObjectPath); CrossArgs->SetObjectField(TEXT("source"), CrossSource);
	Prior = Library;
	const auto CrossBefore = CaptureIdentity(*Library, FGuid());
	UObjectRedirector* CrossRedirector = FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus"));
	if (!CrossRedirector) return false;
	TArray<uint8> CrossSourceDisk, RedirectorBefore, RedirectorAfter;
	if (!FFileHelper::LoadFileToArray(CrossSourceDisk, *Filename)) return false;
	FObjectWriter RedirectorWriter(CrossRedirector, RedirectorBefore);
	int32 CrossWrites = 0;
	const FDelegateHandle CrossHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && (Object->GetOutermost()->GetName() == PackageName || Object->GetOutermost()->GetName() == ExternalPackage)) ++CrossWrites;
	});
	const auto Cross = Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), CrossArgs, TEXT("macro-library-different-owner"));
	FCoreUObjectDelegates::OnObjectModified.Remove(CrossHandle);
	const bool CrossRejected = !Cross.IsOk() && Cross.GetErrorJson().Contains(TEXT("blueprint_graph_name_identity_conflict"));
	Library = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	bool SourceValid = Library && Prior.Get() == Library && !Library->GetOutermost()->IsDirty()
		&& CrossBefore.OrderIndependentCompareEqual(CaptureIdentity(*Library, FGuid()));
	UEdGraph* OriginalMacro = nullptr; UEdGraph* NewOccupant = nullptr;
	if (Library) for (UEdGraph* Graph : Library->MacroGraphs) { if (Graph->GraphGuid == MacroId) OriginalMacro = Graph; if (Graph->GraphGuid == OtherMacroId) NewOccupant = Graph; }
	SourceValid = SourceValid && OriginalMacro && OriginalMacro->GetName() == TEXT("EvaluateStatus") && NewOccupant && NewOccupant->GetName() == TEXT("Untouched");
	UObjectRedirector* RedirectorAfterReject = Library ? FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus")) : nullptr;
	if (RedirectorAfterReject) { FObjectWriter RedirectorAfterWriter(RedirectorAfterReject, RedirectorAfter); }
	TArray<uint8> SourceDiskAfter;
	const bool RedirectorPreserved = RedirectorAfterReject == CrossRedirector && RedirectorBefore == RedirectorAfter
		&& RedirectorAfterReject && RedirectorAfterReject->DestinationObject == OriginalMacro;
	const bool SourceDiskPreserved = FFileHelper::LoadFileToArray(SourceDiskAfter, *Filename) && SourceDiskAfter == CrossSourceDisk;
	External = LoadObject<UBlueprint>(nullptr, *ExternalPath); if (!External) return false;
	UEdGraph* ActualTarget = nullptr;
	for (UEdGraphNode* Node : External->UbergraphPages[0]->Nodes)
		if (auto* Instance = Cast<UK2Node_MacroInstance>(Node); Instance && Instance->NodeGuid == CallerId) ActualTarget = Instance->GetMacroGraph();
	TArray<uint8> CrossDisk; const bool CrossDiskUnchanged = FFileHelper::LoadFileToArray(CrossDisk, *ExternalFilename) && CrossDisk == ExternalDisk;
	AddInfo(FString::Printf(TEXT("macro library old-name identity rejection: typed=%d modifications=%d source_identity=%d redirector=%d source_disk=%d external_original_target=%d external_disk=%d"),
		CrossRejected, CrossWrites, SourceValid, RedirectorPreserved, SourceDiskPreserved, ActualTarget == OriginalMacro, CrossDiskUnchanged));
	TestTrue(TEXT("different macro claiming old name rejects before write and preserves original imports"), CrossRejected && CrossWrites == 0
		&& SourceValid && RedirectorPreserved && SourceDiskPreserved && ActualTarget == OriginalMacro && CrossDiskUnchanged);
	if (HasAnyErrors()) return false;
	auto NewDeclaration = [&](const FString& Path, const FString& Name, bool Edit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Matches = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			const TSharedPtr<FJsonObject>* Declarations = nullptr; if (!Doc->TryGetObjectField(TEXT("graph_declarations"), Declarations)) continue;
			++Matches;
			if (Edit) {
				if ((*Declarations)->HasField(Name)) return false;
				auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("graph_kind"), TEXT("macro_graph")); (*Declarations)->SetObjectField(Name, Row);
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			} else if (!(*Declarations)->HasTypedField<EJson::Object>(Name)) return false;
		} return Matches == 1;
	};
	auto ImportNewGraph = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("macro-library-create-name"));
	};
	if (!Export(Workspace / TEXT("create-old-name")) || !NewDeclaration(Workspace / TEXT("create-old-name"), TEXT("SelectStatus"), true)) return false;
	CrossWrites = 0;
	const auto CreateHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) {
		if (Object && (Object->GetOutermost()->GetName() == PackageName || Object->GetOutermost()->GetName() == ExternalPackage)) ++CrossWrites;
	});
	const auto CreateConflict = ImportNewGraph(Workspace / TEXT("create-old-name"));
	FCoreUObjectDelegates::OnObjectModified.Remove(CreateHandle);
	const bool CreateRejected = !CreateConflict.IsOk() && CreateConflict.GetErrorJson().Contains(TEXT("blueprint_graph_name_identity_conflict"));
	if (!CreateRejected) { AddError(CreateConflict.IsOk() ? TEXT("old-name create unexpectedly succeeded") : CreateConflict.GetErrorJson()); return false; }
	Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); if (!Library) return false;
	const auto* CreateRedirector = FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus"));
	if (CreateRedirector != CrossRedirector) { AddError(TEXT("old-name create replaced original redirector")); return false; }
	RedirectorAfter.Reset(); FObjectWriter AfterCreateRedirectorWriter(CrossRedirector, RedirectorAfter);
	const bool CreateUnchanged = CrossWrites == 0 && Prior.Get() == Library && !Library->GetOutermost()->IsDirty()
		&& CrossBefore.OrderIndependentCompareEqual(CaptureIdentity(*Library, FGuid())) && RedirectorBefore == RedirectorAfter
		&& FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus")) == CrossRedirector && CrossRedirector->DestinationObject == OriginalMacro
		&& FFileHelper::LoadFileToArray(SourceDiskAfter, *Filename) && SourceDiskAfter == CrossSourceDisk
		&& FFileHelper::LoadFileToArray(CrossDisk, *ExternalFilename) && CrossDisk == ExternalDisk;
	Unload = External->GetOutermost(); External = nullptr;
	if (!UPackageTools::UnloadPackages({Unload}) || !CheckExternal(TEXT("EvaluateStatus"))) return false;
	AddInfo(FString::Printf(TEXT("macro library create old-name rejection: typed=%d modifications=%d source_redirector_disk_unchanged=%d independent_external_original=1"), CreateRejected, CrossWrites, CreateUnchanged));
	if (!TestTrue(TEXT("new macro cannot take redirector-owned name before writing"), CreateRejected && CreateUnchanged)) return false;
	if (!Export(Workspace / TEXT("create-free-name")) || !NewDeclaration(Workspace / TEXT("create-free-name"), TEXT("FreshMacro"), true)) return false;
	Prior = Library; Library = nullptr; CrossRedirector = nullptr; OriginalMacro = nullptr; NewOccupant = nullptr;
	const auto Created = ImportNewGraph(Workspace / TEXT("create-free-name"));
	if (!Created.IsOk()) { AddError(Created.GetErrorJson()); return false; }
	Library = LoadObject<UBlueprint>(nullptr, *ObjectPath); if (!Library) return false;
	UEdGraph* FreshMacro = nullptr; for (UEdGraph* Graph : Library->MacroGraphs) if (Graph->GetName() == TEXT("FreshMacro")) FreshMacro = Graph;
	if (!FreshMacro) return false;
	auto AfterCreateIdentity = CaptureIdentity(*Library, FGuid()); const FString FreshKey = FreshMacro->GraphGuid.ToString();
	for (auto It = AfterCreateIdentity.CreateIterator(); It; ++It) if (It.Key() == FreshKey || It.Key().StartsWith(FreshKey + TEXT("/"))) It.RemoveCurrent();
	const auto* PreservedRedirector = FindObjectFast<UObjectRedirector>(Library, TEXT("SelectStatus"));
	const auto* PreservedMacro = PreservedRedirector ? Cast<UEdGraph>(PreservedRedirector->DestinationObject) : nullptr;
	const bool CreatedValid = !Prior.IsValid() && !Library->GetOutermost()->IsDirty() && Library->Status != BS_Error
		&& CrossBefore.OrderIndependentCompareEqual(AfterCreateIdentity) && PreservedMacro && PreservedMacro->GraphGuid == MacroId
		&& CheckExternal(TEXT("EvaluateStatus")) && Export(Workspace / TEXT("create-readback"))
		&& NewDeclaration(Workspace / TEXT("create-readback"), TEXT("FreshMacro"), false);
	AddInfo(FString::Printf(TEXT("macro library free-name create: saved_reloaded_non_targets_redirector_external_reexport=%d"), CreatedValid));
	TestTrue(TEXT("unoccupied macro name creates through public workspace"), CreatedValid);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintDelegateRemovalTest,
	"UeAgentInterface.T208.Blueprint.DelegateRemovalWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintDelegateRemovalTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85DelegateRemoval_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-delegate-removal/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		for (const FString& Path : Owned) if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) {
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned delegate removal cleanup"), ObjectTools::ForceDeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto Save = [&]() {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool Saved = Blueprint->Status != BS_Error && UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args);
		if (Saved) Blueprint->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	FEdGraphPinType Integer; Integer.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Integer, TEXT("23"))) return false;
	FEdGraphPinType DelegateType; DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("StatusChanged"), DelegateType)) return false;
	UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("StatusChanged"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	SignatureGraph->bEditable = false;
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	Schema->CreateDefaultNodesForGraph(*SignatureGraph);
	Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
	Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
	Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);
	auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("name"), TEXT("Enabled"));
	auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("bool")); Input->SetObjectField(TEXT("type"), Type);
	auto Signature = MakeShared<FJsonObject>(); Signature->SetArrayField(TEXT("inputs"), {MakeShared<FJsonValueObject>(Input)}); Signature->SetArrayField(TEXT("outputs"), {});
	if (!ApplyDelegateSignatureGraph(*SignatureGraph, Signature, Error)) return false;
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save()) return false;
	const auto* Property = FindFProperty<FMulticastDelegateProperty>(Blueprint->GeneratedClass, TEXT("StatusChanged"));
	if (!Property) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_AddDelegate> BindCreator(*Graph); auto* Bind = BindCreator.CreateNode();
	Bind->SetFromProperty(Property, true, Blueprint->GeneratedClass); BindCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallDelegate> CallCreator(*Graph); auto* Call = CallCreator.CreateNode();
	Call->SetFromProperty(Property, true, Blueprint->GeneratedClass); CallCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*Graph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("InvokeStatus"); InvokeCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> HandlerCreator(*Graph); auto* Handler = HandlerCreator.CreateNode();
	Handler->CustomFunctionName = TEXT("HandleStatus"); HandlerCreator.Finalize();
	if (!ApplyCustomEventSignature(*Handler, Signature, Error)
		|| !Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Bind->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Bind->FindPin(UEdGraphSchema_K2::PN_Then), Call->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Handler->FindPin(UK2Node_CustomEvent::DelegateOutputName), Bind->GetDelegatePin())) return false;
	Schema->TrySetDefaultValue(*Call->FindPin(TEXT("Enabled")), TEXT("true"));
	// Keep a real, connected non-target branch throughout deletion and restoration.
	FGraphNodeCreator<UK2Node_CustomEvent> KeepEventCreator(*Graph); auto* KeepEvent = KeepEventCreator.CreateNode();
	KeepEvent->CustomFunctionName = TEXT("KeepInvoke"); KeepEventCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> KeepCallCreator(*Graph); auto* KeepCall = KeepCallCreator.CreateNode();
	KeepCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("Untouched"))); KeepCallCreator.Finalize();
	if (!Schema->TryCreateConnection(KeepEvent->FindPin(UEdGraphSchema_K2::PN_Then), KeepCall->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
	if (!Save()) return false;
	const FGuid GraphId = Graph->GraphGuid, SignatureId = SignatureGraph->GraphGuid, CallId = Call->NodeGuid, BindId = Bind->NodeGuid;
	const FGuid MemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("StatusChanged"))].VarGuid;
	const FGuid KeepId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
	const TSet<FGuid> Removed{CallId, BindId, Invoke->NodeGuid, Handler->NodeGuid};
	TSharedRef<FJsonObject> OriginalSignature = MakeShared<FJsonObject>();
	if (!CaptureDelegateSignatureGraph(*SignatureGraph, OriginalSignature, Error)) return false;
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	auto Expected = Before;
	for (auto It = Expected.CreateIterator(); It; ++It) {
		bool Remove = It.Key() == SignatureId.ToString() || It.Key().StartsWith(SignatureId.ToString() + TEXT("/"));
		for (const FGuid& Id : Removed) Remove |= It.Key().StartsWith(GraphId.ToString() + TEXT("/") + Id.ToString());
		if (Remove) It.RemoveCurrent();
	}
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Keep = [&](UBlueprint* BP) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("KeepCount"));
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("KeepCount"));
		return Index != INDEX_NONE && BP->NewVariables[Index].VarGuid == KeepId && Value
			&& Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) == 23;
	};
	auto References = [&](UBlueprint* BP) {
		if (!Keep(BP) || !Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("StatusChanged"));
		UEdGraph* Sig = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, TEXT("StatusChanged"));
		if (Index == INDEX_NONE || BP->NewVariables[Index].VarGuid != MemberId || !Sig || Sig->GraphGuid != SignatureId) return false;
		TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>(); FString A, B;
		if (!CaptureDelegateSignatureGraph(*Sig, Actual, Error) || !CommandContract::Json::TryCanonicalize(Actual, A, Error)
			|| !CommandContract::Json::TryCanonicalize(OriginalSignature, B, Error) || A != B) return false;
		int32 Consumers = 0;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* D = Cast<UK2Node_BaseMCDelegate>(N); D && (N->NodeGuid == BindId || N->NodeGuid == CallId)) {
				FMemberReference Ref = D->DelegateReference;
				const auto* P = Ref.ResolveMember<FMulticastDelegateProperty>(BP->GeneratedClass);
				if (!P || P->GetFName() != TEXT("StatusChanged") || UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) != BP) return false;
				++Consumers;
			}
		return Consumers == 2;
	};
	auto Absent = [&](UBlueprint* BP) {
		return Keep(BP) && FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("StatusChanged")) == INDEX_NONE
			&& !FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, TEXT("StatusChanged"))
			&& !FindFProperty<FMulticastDelegateProperty>(BP->GeneratedClass, TEXT("StatusChanged"))
			&& Expected.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()));
	};
	if (!TestTrue(TEXT("native dispatcher bind call signature fixture"), References(Blueprint))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("delegate-removal-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("delegate-removal-import"));
	};
	// Only visible semantic fields select nodes. Hidden node identity stays with the workspace owner.
	auto Omit = [&](const FString& Path, const int32 Mode) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0, MemberCount = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			if (Doc->HasTypedField<EJson::Object>(TEXT("delegates"))) {
				auto Members = Doc->GetObjectField(TEXT("delegates"));
				if (Members->HasField(TEXT("StatusChanged"))) { ++MemberCount; Members->RemoveField(TEXT("StatusChanged")); }
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			if (!Doc->HasField(TEXT("interface_ref"))) continue;
			const auto Nodes = Doc->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
			for (const auto& Pair : Nodes->Values) {
				const auto Node = Pair.Value->AsObject(); FString Name, Kind, Class;
				Node->TryGetStringField(TEXT("node_type"), Kind);
				Node->TryGetStringField(TEXT("node_class"), Class);
				Node->TryGetStringField(TEXT("event_name"), Name);
				const bool Caller = Kind == TEXT("call_delegate");
				const bool Consumer = Class == UK2Node_AddDelegate::StaticClass()->GetPathName() || Caller;
				if ((Mode == 0 && Class == UK2Node_AddDelegate::StaticClass()->GetPathName())
					|| (Mode == 1 && Caller) || (Mode == 2 && (Consumer || Name == TEXT("InvokeStatus") || Name == TEXT("HandleStatus")))) Deleted.Add(Pair.Key);
			}
			Count += Deleted.Num(); for (const FString& Key : Deleted) Nodes->RemoveField(Key);
			if (Deleted.IsEmpty()) continue;
			auto Connections = Doc->GetArrayField(TEXT("connections"));
			Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) { const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); }); });
			Doc->SetArrayField(TEXT("connections"), Connections);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		return MemberCount == 1 && Count == (Mode == 2 ? 4 : 1);
	};
	for (int32 Mode = 0; Mode != 2; ++Mode) {
		const FString Path = Workspace / FString::Printf(TEXT("reject-%d"), Mode);
		if (!Export(Path) || !Omit(Path, Mode)) { AddError(Error.ToPublicDetail()); return false; }
		TArray<uint8> RawBefore, RawAfter; FObjectWriter(Blueprint, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle); FObjectWriter(Blueprint, RawAfter);
		TArray<uint8> Disk; const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		const bool Typed = Result.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"));
		AddInfo(FString::Printf(TEXT("delegate removal retained %s: rejected=%d typed=%d modifications=%d unchanged=%d"), Mode == 0 ? TEXT("call") : TEXT("binding"), !Result.IsOk(), Typed, Modified, Unchanged));
		if (!TestTrue(TEXT("retained consumer prewrite rejection"), !Result.IsOk() && Typed && Modified == 0 && Unchanged)) { AddError(Result.GetErrorJson()); return false; }
	}
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	const FString ExternalPath = ExternalPackage + TEXT(".BP_External"); Owned.Insert(ExternalPath, 0);
	const FString ExternalFile = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	UBlueprint* External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!External) return false;
	FAssetRegistryModule::AssetCreated(External);
	Property = FindFProperty<FMulticastDelegateProperty>(Blueprint->GeneratedClass, TEXT("StatusChanged"));
	if (!Property) return false;
	FGraphNodeCreator<UK2Node_CallDelegate> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->SetFromProperty(Property, false, Blueprint->GeneratedClass); ExternalCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External); FKismetEditorUtilities::CompileBlueprint(External);
	FSavePackageArgs ExternalArgs; ExternalArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (External->Status == BS_Error || !UPackage::SavePackage(External->GetOutermost(), External, *ExternalFile, ExternalArgs)) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFile)) return false;
	auto RejectExternal = [&](const FString& Label, const FString& Code) {
		const FString Path = Workspace / Label; if (!Export(Path) || !Omit(Path, 2)) return false;
		TArray<uint8> RawBefore, RawAfter; if (ExternalCall) FObjectWriter(ExternalCall, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) {
			if (O && (O->GetOutermost() == Blueprint->GetOutermost() || O->GetOutermost()->GetName() == ExternalPackage)) ++Modified;
		});
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (Result.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("external removal unexpectedly accepted")); return false; }
		if (ExternalCall) FObjectWriter(ExternalCall, RawAfter);
		TArray<uint8> Disk, OutsideDisk;
		const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter
			&& (!External || !External->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk
			&& FFileHelper::LoadFileToArray(OutsideDisk, *ExternalFile) && OutsideDisk == ExternalDisk;
		const bool Typed = Result.GetErrorJson().Contains(Code);
		AddInfo(FString::Printf(TEXT("delegate removal %s: typed=%d modifications=%d source_external_unchanged=%d"), *Label, Typed, Modified, Unchanged));
		if (!Typed) AddError(Result.GetErrorJson()); return Typed && Modified == 0 && Unchanged;
	};
	if (!RejectExternal(TEXT("external-loaded"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({ExternalFile}, true);
	UPackage* ExternalToUnload = External->GetOutermost(); External = nullptr; ExternalCall = nullptr;
	if (!UPackageTools::UnloadPackages({ExternalToUnload}) || FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;
	if (!RejectExternal(TEXT("external-unloaded"), TEXT("blueprint_graph_remove_references_unresolved"))
		|| !TestNull(TEXT("unresolved query did not load external"), FindObject<UBlueprint>(nullptr, *ExternalPath))) return false;
	auto LoadArgs = MakeShared<FJsonObject>(); LoadArgs->SetStringField(TEXT("target"), ExternalPath);
	LoadArgs->SetStringField(TEXT("workspace_path"), Workspace / TEXT("explicit-external-load")); LoadArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Loaded = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), LoadArgs,
		TEXT("delegate-removal-explicit-load"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Loaded.IsOk()) { AddError(Loaded.GetErrorJson()); return false; }
	External = FindObject<UBlueprint>(nullptr, *ExternalPath);
	if (!External) return false;
	for (UEdGraphNode* N : External->UbergraphPages[0]->Nodes) if (auto* C = Cast<UK2Node_CallDelegate>(N); C && C->GetPropertyName() == TEXT("StatusChanged")) ExternalCall = C;
	if (!ExternalCall || !RejectExternal(TEXT("external-loaded-retry"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	ExternalCall = nullptr; External->GetOutermost()->SetDirtyFlag(false);
	if (ObjectTools::ForceDeleteObjects({External}, false) != 1) return false;
	External = nullptr;
	auto NodeSpecs = MakeShared<FJsonObject>();
	for (const FGuid& Id : Removed) { auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("operation"), TEXT("remove")); NodeSpecs->SetObjectField(Id.ToString(EGuidFormats::DigitsWithHyphens), Spec); }
	auto GraphSpec = MakeShared<FJsonObject>(); GraphSpec->SetStringField(TEXT("operation"), TEXT("update")); GraphSpec->SetObjectField(TEXT("nodes"), NodeSpecs);
	auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(GraphId.ToString(EGuidFormats::DigitsWithHyphens), GraphSpec);
	auto DelegateSpec = MakeShared<FJsonObject>(); DelegateSpec->SetStringField(TEXT("operation"), TEXT("remove"));
	auto Delegates = MakeShared<FJsonObject>(); Delegates->SetObjectField(MemberId.ToString(EGuidFormats::DigitsWithHyphens), DelegateSpec);
	auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("delegates"), Delegates);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members); Extension->SetObjectField(TEXT("graphs"), Graphs); Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Old = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Witness = !Old.IsValid() && Absent(Current) && !Current->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("delegate removal late witness: replaced_saved_deleted_non_targets=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_delegate_removal_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Call = nullptr; Bind = nullptr; Handler = nullptr; Invoke = nullptr; Graph = nullptr; SignatureGraph = nullptr; Property = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("delegate-removal-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	const bool Recovered = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && References(Blueprint)
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("delegate removal recovery: original_memory_disk_dirty_pin_references=%d"), Recovered));
	if (!TestTrue(TEXT("late deletion restores native identities references memory disk dirty"), Recovered)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString SuccessPath = Workspace / TEXT("success");
	if (!Export(SuccessPath) || !Omit(SuccessPath, 2)) { AddError(Error.ToPublicDetail()); return false; }
	Old = Blueprint; Blueprint = nullptr; const auto Result = Import(SuccessPath);
	if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const FString ReadbackPath = Workspace / TEXT("readback");
	bool ExportAbsent = Export(ReadbackPath);
	TArray<FString> ReadbackFiles; IFileManager::Get().FindFilesRecursive(ReadbackFiles, *ReadbackPath, TEXT("*.hjson"), true, false);
	for (const FString& File : ReadbackFiles) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		ExportAbsent &= !Text.Contains(TEXT("StatusChanged")) && !Text.Contains(TEXT("InvokeStatus")) && !Text.Contains(TEXT("HandleStatus"));
	}
	const bool Success = !Old.IsValid() && Absent(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && ExportAbsent;
	AddInfo(FString::Printf(TEXT("delegate removal public: replaced_clean_deleted_non_targets_independent_export=%d"), Success));
	return TestTrue(TEXT("public atomic delegate and explicit consumer omission"), Success);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintVariableRemovalTest,
	"UeAgentInterface.T208.Blueprint.VariableRemovalWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintVariableRemovalTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85VariableRemoval_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-variable-removal/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		for (const FString& Path : Owned) if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) {
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned variable removal cleanup"), ObjectTools::ForceDeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto Save = [&]() {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool Saved = Blueprint->Status != BS_Error && UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args);
		if (Saved) Blueprint->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	FEdGraphPinType Integer; Integer.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Integer, TEXT("23"))
		|| !FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Counter"), Integer, TEXT("17"))) return false;
	FEdGraphPinType ObjectType; ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object; ObjectType.PinSubCategoryObject = AActor::StaticClass();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CallTarget"), ObjectType)) return false;
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save()) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	FGraphNodeCreator<UK2Node_CallFunctionOnMember> MemberCallCreator(*Graph); auto* MemberCall = MemberCallCreator.CreateNode();
	MemberCall->MemberVariableToCallOn.SetSelfMember(TEXT("CallTarget"));
	MemberCall->SetFromFunction(AActor::StaticClass()->FindFunctionByName(TEXT("K2_GetActorLocation"))); MemberCallCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Graph); auto* Get = GetCreator.CreateNode();
	Get->VariableReference.SetSelfMember(TEXT("Counter")); GetCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Graph); auto* Set = SetCreator.CreateNode();
	Set->VariableReference.SetSelfMember(TEXT("Counter")); SetCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*Graph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("SetCounter"); InvokeCreator.Finalize();
	if (!Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Set->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Get->FindPin(TEXT("Counter")), Set->FindPin(TEXT("Counter")))) return false;
	FGraphNodeCreator<UK2Node_CustomEvent> KeepEventCreator(*Graph); auto* KeepEvent = KeepEventCreator.CreateNode();
	KeepEvent->CustomFunctionName = TEXT("KeepInvoke"); KeepEventCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> KeepCallCreator(*Graph); auto* KeepCall = KeepCallCreator.CreateNode();
	KeepCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("Untouched"))); KeepCallCreator.Finalize();
	if (!Schema->TryCreateConnection(KeepEvent->FindPin(UEdGraphSchema_K2::PN_Then), KeepCall->FindPin(UEdGraphSchema_K2::PN_Execute)) || !Save()) return false;
	const FGuid GraphId = Graph->GraphGuid, GetId = Get->NodeGuid, SetId = Set->NodeGuid, MemberCallId = MemberCall->NodeGuid;
	const FGuid MemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Counter"))].VarGuid;
	const FGuid KeepId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
	const FGuid ObjectMemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("CallTarget"))].VarGuid;
	const TSet<FGuid> Removed{GetId, SetId, Invoke->NodeGuid};
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	auto Expected = Before;
	for (auto It = Expected.CreateIterator(); It; ++It) {
		bool Remove = false;
		for (const FGuid& Id : Removed) Remove |= It.Key().StartsWith(GraphId.ToString() + TEXT("/") + Id.ToString());
		if (Remove) It.RemoveCurrent();
	}
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Keep = [&](UBlueprint* BP) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass) return false;
		const int32 ObjectIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("CallTarget"));
		if (ObjectIndex == INDEX_NONE || BP->NewVariables[ObjectIndex].VarGuid != ObjectMemberId) return false;
		bool MemberCallPreserved = false;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* C = Cast<UK2Node_CallFunctionOnMember>(N); C && C->NodeGuid == MemberCallId) {
				FMemberReference Ref = C->MemberVariableToCallOn;
				const auto* P = Ref.ResolveMember<FProperty>(BP->GeneratedClass);
				MemberCallPreserved = P && P->GetFName() == TEXT("CallTarget") && UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) == BP;
			}
		if (!MemberCallPreserved) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("KeepCount"));
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("KeepCount"));
		return Index != INDEX_NONE && BP->NewVariables[Index].VarGuid == KeepId && Value
			&& Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) == 23;
	};
	auto References = [&](UBlueprint* BP) {
		if (!Keep(BP) || !Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Counter"));
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Counter"));
		if (Index == INDEX_NONE || BP->NewVariables[Index].VarGuid != MemberId || !Value
			|| Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) != 17) return false;
		int32 Consumers = 0;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* V = Cast<UK2Node_Variable>(N); V && (N->NodeGuid == GetId || N->NodeGuid == SetId)) {
				FMemberReference Ref = V->VariableReference;
				const auto* P = Ref.ResolveMember<FProperty>(BP->GeneratedClass);
				if (!P || P->GetFName() != TEXT("Counter") || UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) != BP || Ref.GetMemberGuid() != MemberId) return false;
				++Consumers;
			}
		return Consumers == 2;
	};
	auto Absent = [&](UBlueprint* BP) {
		return Keep(BP) && FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Counter")) == INDEX_NONE
			&& !FindFProperty<FProperty>(BP->GeneratedClass, TEXT("Counter"))
			&& Expected.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()));
	};
	if (!TestTrue(TEXT("native variable Get Set fixture"), References(Blueprint))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("variable-removal-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("variable-removal-import"));
	};
	// Only visible semantic fields select nodes. Hidden node identity stays with the workspace owner.
	auto Omit = [&](const FString& Path, const int32 Mode, const FString& MemberName = TEXT("Counter")) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0, MemberCount = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			if (Doc->HasTypedField<EJson::Object>(TEXT("variables"))) {
				auto Members = Doc->GetObjectField(TEXT("variables"));
				if (Members->HasField(MemberName)) { ++MemberCount; Members->RemoveField(MemberName); }
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			if (!Doc->HasField(TEXT("interface_ref"))) continue;
			const auto Nodes = Doc->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
			for (const auto& Pair : Nodes->Values) {
				const auto Node = Pair.Value->AsObject(); FString Name, Kind, Class;
				Node->TryGetStringField(TEXT("node_type"), Kind);
				Node->TryGetStringField(TEXT("node_class"), Class);
				Node->TryGetStringField(TEXT("event_name"), Name);
				FString Variable, Access;
				Node->TryGetStringField(TEXT("variable_name"), Variable); Node->TryGetStringField(TEXT("access"), Access);
				const bool Getter = Kind == TEXT("variable_node") && Variable == TEXT("Counter") && Access == TEXT("get");
				const bool Setter = Kind == TEXT("variable_node") && Variable == TEXT("Counter") && Access == TEXT("set");
				if ((Mode == 0 && Setter) || (Mode == 1 && Getter) || (Mode == 2 && (Getter || Setter || Name == TEXT("SetCounter")))) Deleted.Add(Pair.Key);
			}
			Count += Deleted.Num(); for (const FString& Key : Deleted) Nodes->RemoveField(Key);
			if (Deleted.IsEmpty()) continue;
			auto Connections = Doc->GetArrayField(TEXT("connections"));
			Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) { const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); }); });
			Doc->SetArrayField(TEXT("connections"), Connections);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		return MemberCount == 1 && Count == (Mode == 2 ? 3 : Mode == -1 ? 0 : 1);
	};
	{
		const FString Path = Workspace / TEXT("reject-member-call");
		if (!Export(Path) || !Omit(Path, -1, TEXT("CallTarget"))) return false;
		TArray<uint8> RawBefore, RawAfter; FObjectWriter(MemberCall, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle); FObjectWriter(MemberCall, RawAfter);
		TArray<uint8> Disk; const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		const bool Typed = Result.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"));
		AddInfo(FString::Printf(TEXT("variable removal member-call: rejected=%d typed=%d modifications=%d unchanged=%d"), !Result.IsOk(), Typed, Modified, Unchanged));
		if (!TestTrue(TEXT("expanded member-call source reference rejects before writing"), !Result.IsOk() && Typed && Modified == 0 && Unchanged)) { AddError(Result.GetErrorJson()); return false; }
	}
	for (int32 Mode = 0; Mode != 2; ++Mode) {
		const FString Path = Workspace / FString::Printf(TEXT("reject-%d"), Mode);
		if (!Export(Path) || !Omit(Path, Mode)) { AddError(Error.ToPublicDetail()); return false; }
		TArray<uint8> RawBefore, RawAfter; FObjectWriter(Blueprint, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle); FObjectWriter(Blueprint, RawAfter);
		TArray<uint8> Disk; const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		const bool Typed = Result.GetErrorJson().Contains(TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"));
		AddInfo(FString::Printf(TEXT("variable removal retained %s: rejected=%d typed=%d modifications=%d unchanged=%d"), Mode == 0 ? TEXT("get") : TEXT("set"), !Result.IsOk(), Typed, Modified, Unchanged));
		if (!TestTrue(TEXT("retained consumer prewrite rejection"), !Result.IsOk() && Typed && Modified == 0 && Unchanged)) { AddError(Result.GetErrorJson()); return false; }
	}
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	const FString ExternalPath = ExternalPackage + TEXT(".BP_External"); Owned.Insert(ExternalPath, 0);
	const FString ExternalFile = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	UBlueprint* External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!External) return false;
	FAssetRegistryModule::AssetCreated(External);
	FGraphNodeCreator<UK2Node_VariableGet> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->VariableReference.SetExternalMember(TEXT("Counter"), Blueprint->GeneratedClass); ExternalCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External); FKismetEditorUtilities::CompileBlueprint(External);
	FSavePackageArgs ExternalArgs; ExternalArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (External->Status == BS_Error || !UPackage::SavePackage(External->GetOutermost(), External, *ExternalFile, ExternalArgs)) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFile)) return false;
	auto RejectExternal = [&](const FString& Label, const FString& Code) {
		const FString Path = Workspace / Label; if (!Export(Path) || !Omit(Path, 2)) return false;
		TArray<uint8> RawBefore, RawAfter; if (ExternalCall) FObjectWriter(ExternalCall, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) {
			if (O && (O->GetOutermost() == Blueprint->GetOutermost() || O->GetOutermost()->GetName() == ExternalPackage)) ++Modified;
		});
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (Result.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("external removal unexpectedly accepted")); return false; }
		if (ExternalCall) FObjectWriter(ExternalCall, RawAfter);
		TArray<uint8> Disk, OutsideDisk;
		const bool Unchanged = References(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter
			&& (!External || !External->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk
			&& FFileHelper::LoadFileToArray(OutsideDisk, *ExternalFile) && OutsideDisk == ExternalDisk;
		const bool Typed = Result.GetErrorJson().Contains(Code);
		AddInfo(FString::Printf(TEXT("variable removal %s: typed=%d modifications=%d source_external_unchanged=%d"), *Label, Typed, Modified, Unchanged));
		if (!Typed) AddError(Result.GetErrorJson()); return Typed && Modified == 0 && Unchanged;
	};
	if (!RejectExternal(TEXT("external-loaded"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanFilesSynchronous({ExternalFile}, true);
	UPackage* ExternalToUnload = External->GetOutermost(); External = nullptr; ExternalCall = nullptr;
	if (!UPackageTools::UnloadPackages({ExternalToUnload}) || FindObject<UBlueprint>(nullptr, *ExternalPath)) return false;
	if (!RejectExternal(TEXT("external-unloaded"), TEXT("blueprint_graph_remove_references_unresolved"))
		|| !TestNull(TEXT("unresolved query did not load external"), FindObject<UBlueprint>(nullptr, *ExternalPath))) return false;
	auto LoadArgs = MakeShared<FJsonObject>(); LoadArgs->SetStringField(TEXT("target"), ExternalPath);
	LoadArgs->SetStringField(TEXT("workspace_path"), Workspace / TEXT("explicit-external-load")); LoadArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
	const auto Loaded = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), LoadArgs,
		TEXT("variable-removal-explicit-load"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	if (!Loaded.IsOk()) { AddError(Loaded.GetErrorJson()); return false; }
	External = FindObject<UBlueprint>(nullptr, *ExternalPath);
	if (!External) return false;
	for (UEdGraphNode* N : External->UbergraphPages[0]->Nodes) if (auto* C = Cast<UK2Node_VariableGet>(N); C && C->GetVarName() == TEXT("Counter")) ExternalCall = C;
	if (!ExternalCall || !RejectExternal(TEXT("external-loaded-retry"), TEXT("blueprint_graph_remove_live_consumer_plan_unsupported"))) return false;
	ExternalCall = nullptr; External->GetOutermost()->SetDirtyFlag(false);
	if (ObjectTools::ForceDeleteObjects({External}, false) != 1) return false;
	External = nullptr;
	auto NodeSpecs = MakeShared<FJsonObject>();
	for (const FGuid& Id : Removed) { auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("operation"), TEXT("remove")); NodeSpecs->SetObjectField(Id.ToString(EGuidFormats::DigitsWithHyphens), Spec); }
	auto GraphSpec = MakeShared<FJsonObject>(); GraphSpec->SetStringField(TEXT("operation"), TEXT("update")); GraphSpec->SetObjectField(TEXT("nodes"), NodeSpecs);
	auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(GraphId.ToString(EGuidFormats::DigitsWithHyphens), GraphSpec);
	auto MemberSpec = MakeShared<FJsonObject>(); MemberSpec->SetStringField(TEXT("operation"), TEXT("remove"));
	auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(MemberId.ToString(EGuidFormats::DigitsWithHyphens), MemberSpec);
	auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members); Extension->SetObjectField(TEXT("graphs"), Graphs); Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Old = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Witness = !Old.IsValid() && Absent(Current) && !Current->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("variable removal late witness: replaced_saved_deleted_non_targets=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_variable_removal_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Get = nullptr; Set = nullptr; Invoke = nullptr; Graph = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("variable-removal-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	const bool Recovered = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && References(Blueprint)
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("variable removal recovery: original_memory_disk_dirty_pin_references=%d"), Recovered));
	if (!TestTrue(TEXT("late deletion restores native identities references memory disk dirty"), Recovered)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString SuccessPath = Workspace / TEXT("success");
	if (!Export(SuccessPath) || !Omit(SuccessPath, 2)) { AddError(Error.ToPublicDetail()); return false; }
	Old = Blueprint; Blueprint = nullptr; const auto Result = Import(SuccessPath);
	if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const FString ReadbackPath = Workspace / TEXT("readback");
	bool ExportAbsent = Export(ReadbackPath);
	TArray<FString> ReadbackFiles; IFileManager::Get().FindFilesRecursive(ReadbackFiles, *ReadbackPath, TEXT("*.hjson"), true, false);
	for (const FString& File : ReadbackFiles) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		ExportAbsent &= !Text.Contains(TEXT("Counter")) && !Text.Contains(TEXT("SetCounter"));
	}
	const bool Success = !Old.IsValid() && Absent(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && ExportAbsent;
	AddInfo(FString::Printf(TEXT("variable removal public: replaced_clean_deleted_non_targets_independent_export=%d"), Success));
	return TestTrue(TEXT("public atomic variable and explicit consumer omission"), Success);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintVariableRenameTest,
	"UeAgentInterface.T208.Blueprint.VariableRenameWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintVariableRenameTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85VariableRename_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-variable-rename/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		for (const FString& Path : Owned) if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) {
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned variable rename cleanup"), ObjectTools::ForceDeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto Save = [&]() {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool Saved = Blueprint->Status != BS_Error && UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args);
		if (Saved) Blueprint->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	FEdGraphPinType Integer; Integer.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Integer, TEXT("23"))
		|| !FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Counter"), Integer, TEXT("17"))) return false;
	FEdGraphPinType ObjectType; ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object; ObjectType.PinSubCategoryObject = AActor::StaticClass();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CallTarget"), ObjectType)) return false;
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save()) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	FGraphNodeCreator<UK2Node_CallFunctionOnMember> MemberCallCreator(*Graph); auto* MemberCall = MemberCallCreator.CreateNode();
	MemberCall->MemberVariableToCallOn.SetSelfMember(TEXT("CallTarget"));
	MemberCall->SetFromFunction(AActor::StaticClass()->FindFunctionByName(TEXT("K2_GetActorLocation"))); MemberCallCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Graph); auto* Get = GetCreator.CreateNode();
	Get->VariableReference.SetSelfMember(TEXT("Counter")); GetCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Graph); auto* Set = SetCreator.CreateNode();
	Set->VariableReference.SetSelfMember(TEXT("Counter")); SetCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*Graph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("SetCounter"); InvokeCreator.Finalize();
	if (!Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Set->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Get->FindPin(TEXT("Counter")), Set->FindPin(TEXT("Counter")))) return false;
	FGraphNodeCreator<UK2Node_CustomEvent> KeepEventCreator(*Graph); auto* KeepEvent = KeepEventCreator.CreateNode();
	KeepEvent->CustomFunctionName = TEXT("KeepInvoke"); KeepEventCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> KeepCallCreator(*Graph); auto* KeepCall = KeepCallCreator.CreateNode();
	KeepCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("Untouched"))); KeepCallCreator.Finalize();
	if (!Schema->TryCreateConnection(KeepEvent->FindPin(UEdGraphSchema_K2::PN_Then), KeepCall->FindPin(UEdGraphSchema_K2::PN_Execute)) || !Save()) return false;
	const FGuid GraphId = Graph->GraphGuid, GetId = Get->NodeGuid, SetId = Set->NodeGuid, MemberCallId = MemberCall->NodeGuid;
	const FGuid MemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Counter"))].VarGuid;
	const FGuid KeepId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
	const FGuid ObjectMemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("CallTarget"))].VarGuid;
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto References = [&](UBlueprint* BP, const FString& Name) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, *Name);
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, *Name);
		const int32 KeepIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("KeepCount"));
		const auto* KeepValue = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("KeepCount"));
		if (Index == INDEX_NONE || BP->NewVariables[Index].VarGuid != MemberId || !Value
			|| Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) != 17
			|| KeepIndex == INDEX_NONE || BP->NewVariables[KeepIndex].VarGuid != KeepId || !KeepValue
			|| KeepValue->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) != 23) return false;
		auto Expected = Before;
		for (auto& Pair : Expected) if (Pair.Value.StartsWith(TEXT("Counter:"))) Pair.Value = Name + Pair.Value.Mid(7);
		if (!Expected.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		int32 Consumers = 0;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* V = Cast<UK2Node_Variable>(N); V && (N->NodeGuid == GetId || N->NodeGuid == SetId)) {
				FMemberReference Ref = V->VariableReference;
				const auto* P = Ref.ResolveMember<FProperty>(BP->GeneratedClass);
				if (V->VariableReference.GetMemberName() != *Name || !P || P->GetFName() != *Name
					|| UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) != BP || Ref.GetMemberGuid() != MemberId) return false;
				++Consumers;
			}
		return Consumers == 2;
	};
	if (!TestTrue(TEXT("native variable rename fixture"), References(Blueprint, TEXT("Counter")))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("variable-rename-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("variable-rename-import"));
	};
	auto EditName = [&](const FString& Path, const FString& Name, bool Omit = false) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0;
			for (const FString& File : Files) {
				if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
				FString Text; TSharedPtr<FJsonObject> Doc;
				if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
				if (!Doc->HasTypedField<EJson::Object>(TEXT("variables"))) continue;
				const auto Variables = Doc->GetObjectField(TEXT("variables"));
				if (!Variables->HasField(TEXT("Counter"))) continue;
				const auto Spec = Variables->GetObjectField(TEXT("Counter"));
				if (Spec->HasField(TEXT("guid")) || Spec->HasField(TEXT("new_name"))) return false;
				if (Omit) { Spec->RemoveField(TEXT("name")); Spec->SetBoolField(TEXT("instance_editable"), true); }
				else Spec->SetStringField(TEXT("name"), Name);
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				++Count;
			}
		return Count == 1;
	};
	auto ExtensionFor = [&](const FString& Name, bool Legacy = false) {
		auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("operation"), TEXT("update"));
		Spec->SetStringField(Legacy ? TEXT("new_name") : TEXT("name"), Name);
		auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(MemberId.ToString(), Spec);
		auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables);
		auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members);
		Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true); return Extension;
	};
	{
		FAssetApplyRequest Request; Request.Operation = UeAgentApplyInputContract::EOperation::Update; Request.AssetPath = PackageName;
		Request.ProviderExtension = ExtensionFor(TEXT("Total"), true);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		FAssetApplyResult Result; const bool Applied = ApplyAsset(Request, Result, TEXT("variable-rename-legacy")); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		const bool Rejected = !Applied && Modified == 0 && !Result.bRollbackAttempted && Result.Diagnostic.PublicMessage.Contains(TEXT("blueprint_variable_rename_legacy_field_unsupported")) && References(Blueprint, TEXT("Counter"));
		AddInfo(FString::Printf(TEXT("variable rename legacy: typed_zero_write=%d"), Rejected)); if (!TestTrue(TEXT("old new_name retired"), Rejected)) { AddError(Result.Diagnostic.Code + Result.Diagnostic.PublicMessage); return false; }
	}
	auto RejectPublic = [&](const FString& Label, const FString& Name, const FString& Code) {
		const FString Path = Workspace / Label; if (!Export(Path) || !EditName(Path, Name)) return false;
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && !O->HasAnyFlags(RF_Transient) && O->GetOutermost()->GetName().StartsWith(PackageName.LeftChop(8))) ++Modified; });
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		TArray<uint8> Disk; const bool Unchanged = References(Blueprint, TEXT("Counter")) && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		const bool Rejected = !Result.IsOk() && Result.GetErrorJson().Contains(Code) && Modified == 0 && Unchanged;
		AddInfo(FString::Printf(TEXT("variable rename %s: typed_zero_write_unchanged=%d"), *Label, Rejected)); if (!Rejected) AddError(Result.GetErrorJson()); return Rejected;
	};
	if (!RejectPublic(TEXT("invalid-name"), TEXT("KeepCount"), TEXT("blueprint_member_identity_conflict"))) return false;
	const int32 RepIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Counter"));
	Blueprint->NewVariables[RepIndex].RepNotifyFunc = TEXT("OnRep_Counter");
	if (!RejectPublic(TEXT("repnotify"), TEXT("Total"), TEXT("blueprint_variable_rename_forbidden"))) return false;
	Blueprint->NewVariables[RepIndex].RepNotifyFunc = NAME_None;
	const FString ExternalPackage = FPackageName::GetLongPackagePath(PackageName) / TEXT("BP_External");
	const FString ExternalPath = ExternalPackage + TEXT(".BP_External"); Owned.Insert(ExternalPath, 0);
	const FString ExternalFile = FPackageName::LongPackageNameToFilename(ExternalPackage, FPackageName::GetAssetPackageExtension());
	UBlueprint* External = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*ExternalPackage), TEXT("BP_External"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!External) return false;
	FAssetRegistryModule::AssetCreated(External);
	FGraphNodeCreator<UK2Node_VariableGet> ExternalCreator(*External->UbergraphPages[0]); auto* ExternalCall = ExternalCreator.CreateNode();
	ExternalCall->VariableReference.SetExternalMember(TEXT("Counter"), Blueprint->GeneratedClass); ExternalCreator.Finalize();
	ExternalCall->FindPin(UEdGraphSchema_K2::PN_Self)->DefaultObject = Blueprint->GeneratedClass->GetDefaultObject();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(External); FKismetEditorUtilities::CompileBlueprint(External);
	FSavePackageArgs ExternalArgs; ExternalArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (External->Status == BS_Error || !UPackage::SavePackage(External->GetOutermost(), External, *ExternalFile, ExternalArgs)) return false;
	External->GetOutermost()->SetDirtyFlag(false);
	TArray<uint8> ExternalDisk; if (!FFileHelper::LoadFileToArray(ExternalDisk, *ExternalFile)) return false;
	auto RejectExternal = [&](const FString& Label, const FString& Code) {
		const FString Path = Workspace / Label; if (!Export(Path) || !EditName(Path, TEXT("Total"))) return false;
		TArray<uint8> RawBefore, RawAfter; if (ExternalCall) FObjectWriter(ExternalCall, RawBefore);
		int32 Modified = 0; const auto Handle = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) {
			if (O && (O->GetOutermost() == Blueprint->GetOutermost() || O->GetOutermost()->GetName() == ExternalPackage)) ++Modified;
		});
		const auto Result = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(Handle);
		if (Result.IsOk()) { Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); AddError(TEXT("external rename unexpectedly accepted")); return false; }
		if (ExternalCall) FObjectWriter(ExternalCall, RawAfter);
		TArray<uint8> Disk, OutsideDisk;
		const bool Unchanged = References(Blueprint, TEXT("Counter")) && !Blueprint->GetOutermost()->IsDirty() && RawBefore == RawAfter
			&& (!External || !External->GetOutermost()->IsDirty())
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk
			&& FFileHelper::LoadFileToArray(OutsideDisk, *ExternalFile) && OutsideDisk == ExternalDisk;
		const bool Typed = Result.GetErrorJson().Contains(Code);
		AddInfo(FString::Printf(TEXT("variable rename %s: typed=%d modifications=%d source_external_unchanged=%d"), *Label, Typed, Modified, Unchanged));
		if (!Typed) AddError(Result.GetErrorJson()); return Typed && Modified == 0 && Unchanged;
	};
	if (!RejectExternal(TEXT("external-loaded"), TEXT("blueprint_variable_rename_external_impact"))) return false;
	ExternalCall = nullptr; External->GetOutermost()->SetDirtyFlag(false);
	if (ObjectTools::ForceDeleteObjects({External}, false) != 1) return false; External = nullptr;
	const FString ChildPackage = PackageName.LeftChop(8) + TEXT("BP_Child");
	auto* Child = FKismetEditorUtilities::CreateBlueprint(Blueprint->GeneratedClass, CreatePackage(*ChildPackage), TEXT("BP_Child"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Child) return false; FAssetRegistryModule::AssetCreated(Child); Owned.Add(Child->GetPathName());
	if (!FBlueprintEditorUtils::AddMemberVariable(Child, TEXT("Total"), Integer, TEXT("39"))) return false;
	if (!RejectPublic(TEXT("child-conflict"), TEXT("Total"), TEXT("blueprint_variable_rename_external_impact"))) return false;
	if (ObjectTools::ForceDeleteObjects({Child}, false) != 1) return false; Child = nullptr;
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Old = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = ExtensionFor(TEXT("Total"));
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		Witness = !Old.IsValid() && References(Current, TEXT("Total")) && !Current->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("variable rename late witness: replaced_saved_name_identity_default_links=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_variable_rename_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Get = nullptr; Set = nullptr; Invoke = nullptr; Graph = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("variable-rename-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	const bool Recovered = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && References(Blueprint, TEXT("Counter"))
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
			&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("variable rename recovery: original_memory_disk_dirty_guid_getset=%d"), Recovered));
	if (!TestTrue(TEXT("late rename restores original state"), Recovered)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString Omission = Workspace / TEXT("omit-name");
	if (!Export(Omission) || !EditName(Omission, FString(), true)) return false;
	const auto Omitted = Import(Omission); if (!Omitted.IsOk()) { AddError(Omitted.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("omitted name retains captured variable identity"), References(Blueprint, TEXT("Counter")))) return false;
	const FString SuccessPath = Workspace / TEXT("success");
	if (!Export(SuccessPath) || !EditName(SuccessPath, TEXT("Total"))) { AddError(Error.ToPublicDetail()); return false; }
	Old = Blueprint; Blueprint = nullptr; const auto Result = Import(SuccessPath);
	if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const FString ReadbackPath = Workspace / TEXT("readback"); bool Exported = Export(ReadbackPath), HasMember = false, HasReference = false;
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *ReadbackPath, TEXT("*.hjson"), true, false);
	for (const FString& File : Files) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Doc; if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
		if (Doc->HasField(TEXT("variables"))) { const auto Vars = Doc->GetObjectField(TEXT("variables")); HasMember |= Vars->HasField(TEXT("Total")) && !Vars->HasField(TEXT("Counter")); }
		HasReference |= Text.Contains(TEXT("variable_name: Total"));
	}
	const bool Success = !Old.IsValid() && References(Blueprint, TEXT("Total")) && !Blueprint->GetOutermost()->IsDirty() && Exported && HasMember && HasReference;
	AddInfo(FString::Printf(TEXT("variable rename public: replaced_clean_guid_default_getset_independent_export=%d"), Success));
	return TestTrue(TEXT("public variable name rename"), Success);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintVariableTypeTest,
	"UeAgentInterface.T208.Blueprint.VariableTypeWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintVariableTypeTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85VariableType_") + Token + TEXT("/BP_Owner");
	const FString ObjectPath = PackageName + TEXT(".BP_Owner");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-blueprint-referenced-type/native") / Token);
	TArray<FString> Owned{ObjectPath};
	ON_SCOPE_EXIT {
		for (const FString& Path : Owned) if (UObject* Object = LoadObject<UObject>(nullptr, *Path)) {
			Object->GetOutermost()->SetDirtyFlag(false);
			TestEqual(TEXT("owned variable type cleanup"), ObjectTools::ForceDeleteObjects({Object}, false), 1);
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*PackageName), TEXT("BP_Owner"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto Save = [&]() {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool Saved = Blueprint->Status != BS_Error && UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint, *Filename, Args);
		if (Saved) Blueprint->GetOutermost()->SetDirtyFlag(false); return Saved;
	};
	FEdGraphPinType Integer; Integer.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("KeepCount"), Integer, TEXT("23"))
		|| !FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("Counter"), Integer, TEXT("17"))) return false;
	FEdGraphPinType ObjectType; ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object; ObjectType.PinSubCategoryObject = AActor::StaticClass();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CallTarget"), ObjectType)) return false;
	UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("Untouched"));
	if (!Save()) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	FGraphNodeCreator<UK2Node_CallFunctionOnMember> MemberCallCreator(*Graph); auto* MemberCall = MemberCallCreator.CreateNode();
	MemberCall->MemberVariableToCallOn.SetSelfMember(TEXT("CallTarget"));
	MemberCall->SetFromFunction(AActor::StaticClass()->FindFunctionByName(TEXT("K2_GetActorLocation"))); MemberCallCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Graph); auto* Get = GetCreator.CreateNode();
	Get->VariableReference.SetSelfMember(TEXT("Counter")); GetCreator.Finalize();
	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Graph); auto* Set = SetCreator.CreateNode();
	Set->VariableReference.SetSelfMember(TEXT("Counter")); SetCreator.Finalize();
	FGraphNodeCreator<UK2Node_CustomEvent> InvokeCreator(*Graph); auto* Invoke = InvokeCreator.CreateNode();
	Invoke->CustomFunctionName = TEXT("SetCounter"); InvokeCreator.Finalize();
	if (!Schema->TryCreateConnection(Invoke->FindPin(UEdGraphSchema_K2::PN_Then), Set->FindPin(UEdGraphSchema_K2::PN_Execute))
		|| !Schema->TryCreateConnection(Get->FindPin(TEXT("Counter")), Set->FindPin(TEXT("Counter")))) return false;
	FGraphNodeCreator<UK2Node_CustomEvent> KeepEventCreator(*Graph); auto* KeepEvent = KeepEventCreator.CreateNode();
	KeepEvent->CustomFunctionName = TEXT("KeepInvoke"); KeepEventCreator.Finalize();
	FGraphNodeCreator<UK2Node_CallFunction> KeepCallCreator(*Graph); auto* KeepCall = KeepCallCreator.CreateNode();
	KeepCall->SetFromFunction(Blueprint->GeneratedClass->FindFunctionByName(TEXT("Untouched"))); KeepCallCreator.Finalize();
	if (!Schema->TryCreateConnection(KeepEvent->FindPin(UEdGraphSchema_K2::PN_Then), KeepCall->FindPin(UEdGraphSchema_K2::PN_Execute)) || !Save()) return false;
	const FGuid GraphId = Graph->GraphGuid, GetId = Get->NodeGuid, SetId = Set->NodeGuid, MemberCallId = MemberCall->NodeGuid;
	const FGuid MemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("Counter"))].VarGuid;
	const FGuid KeepId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("KeepCount"))].VarGuid;
	const FGuid ObjectMemberId = Blueprint->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, TEXT("CallTarget"))].VarGuid;
	const auto Before = CaptureIdentity(*Blueprint, FGuid());
	auto Expected = Before;
	for (auto It = Expected.CreateIterator(); It; ++It) {
		for (const FGuid& Id : {GetId, SetId})
			if (It.Key().StartsWith(GraphId.ToString() + TEXT("/") + Id.ToString() + TEXT("/"))) {
				It.Value().ReplaceInline(TEXT(":int"), TEXT(":real"));
				// Native K2 formats the migrated integer zero default as double zero.
				It.Value().ReplaceInline(TEXT(":0:"), TEXT(":0.0:"));
			}
	}
	TArray<uint8> OriginalDisk; if (!FFileHelper::LoadFileToArray(OriginalDisk, *Filename)) return false;
	auto Keep = [&](UBlueprint* BP) {
		if (!BP || BP->Status == BS_Error || !BP->GeneratedClass) return false;
		const int32 ObjectIndex = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("CallTarget"));
		if (ObjectIndex == INDEX_NONE || BP->NewVariables[ObjectIndex].VarGuid != ObjectMemberId) return false;
		bool MemberCallPreserved = false;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* C = Cast<UK2Node_CallFunctionOnMember>(N); C && C->NodeGuid == MemberCallId) {
				FMemberReference Ref = C->MemberVariableToCallOn;
				const auto* P = Ref.ResolveMember<FProperty>(BP->GeneratedClass);
				MemberCallPreserved = P && P->GetFName() == TEXT("CallTarget") && UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) == BP;
			}
		if (!MemberCallPreserved) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("KeepCount"));
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("KeepCount"));
		return Index != INDEX_NONE && BP->NewVariables[Index].VarGuid == KeepId && Value
			&& Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) == 23;
	};
	auto References = [&](UBlueprint* BP) {
		if (!Keep(BP) || !Before.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()))) return false;
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Counter"));
		const auto* Value = FindFProperty<FIntProperty>(BP->GeneratedClass, TEXT("Counter"));
		if (Index == INDEX_NONE || BP->NewVariables[Index].VarGuid != MemberId || !Value
			|| Value->GetPropertyValue_InContainer(BP->GeneratedClass->GetDefaultObject()) != 17) return false;
		int32 Consumers = 0;
		for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* V = Cast<UK2Node_Variable>(N); V && (N->NodeGuid == GetId || N->NodeGuid == SetId)) {
				FMemberReference Ref = V->VariableReference;
				const auto* P = Ref.ResolveMember<FProperty>(BP->GeneratedClass);
				if (!P || P->GetFName() != TEXT("Counter") || UBlueprint::GetBlueprintFromClass(P->GetOwnerClass()) != BP || Ref.GetMemberGuid() != MemberId) return false;
				++Consumers;
			}
		return Consumers == 2;
	};
	auto ChangedType = [&](UBlueprint* BP) {
		const int32 Index = BP ? FBlueprintEditorUtils::FindNewVariableIndex(BP, TEXT("Counter")) : INDEX_NONE;
		if (BP) {
			const auto Actual = CaptureIdentity(*BP, FGuid());
			for (const auto& Pair : Expected) if (!Actual.Contains(Pair.Key) || Actual[Pair.Key] != Pair.Value)
				AddInfo(TEXT("type identity difference expected=") + Pair.Value + TEXT(" actual=") + Actual.FindRef(Pair.Key));
			AddInfo(FString::Printf(TEXT("type member readback: keep=%d index=%d double=%d"), Keep(BP), Index, FindFProperty<FDoubleProperty>(BP->GeneratedClass, TEXT("Counter")) != nullptr));
		}
		int32 NativePins = 0;
		if (BP) for (UEdGraph* G : BP->UbergraphPages) for (UEdGraphNode* N : G->Nodes)
			if (const auto* V = Cast<UK2Node_Variable>(N); V && (N->NodeGuid == GetId || N->NodeGuid == SetId)) {
				FMemberReference Ref = V->VariableReference;
				const auto* Property = Ref.ResolveMember<FDoubleProperty>(BP->GeneratedClass);
				if (!Property || UBlueprint::GetBlueprintFromClass(Property->GetOwnerClass()) != BP || Ref.GetMemberGuid() != MemberId) {
					AddInfo(TEXT("type consumer member identity mismatch")); return false;
				}
				for (const UEdGraphPin* Pin : V->Pins) {
					if (Pin->bOrphanedPin) return false;
					if (Pin->PinName == TEXT("Counter")) {
						if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Real || Pin->PinType.PinSubCategory != UEdGraphSchema_K2::PC_Double) return false;
						++NativePins;
					}
				}
			}
		AddInfo(FString::Printf(TEXT("type native pins=%d"), NativePins));
		return Keep(BP) && Index != INDEX_NONE && BP->NewVariables[Index].VarGuid == MemberId
			&& BP->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_Real
			&& BP->NewVariables[Index].VarType.PinSubCategory == UEdGraphSchema_K2::PC_Double
			&& FindFProperty<FDoubleProperty>(BP->GeneratedClass, TEXT("Counter"))
			&& NativePins == 2 && Expected.OrderIndependentCompareEqual(CaptureIdentity(*BP, FGuid()));
	};
	if (!TestTrue(TEXT("native variable Get Set fixture"), References(Blueprint))) return false;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) return false;
	auto Export = [&](const FString& Path) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Path); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, TEXT("content_graph.export_workspace"), Args, TEXT("variable-type-export"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
		if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto Import = [&](const FString& Path) {
		auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Path);
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Tests::PublicCommand::SubmitActiveCommand(Registry, Dispatch, TEXT("content_graph.import_workspace"), Args, TEXT("variable-type-import"));
	};
	// Only visible semantic fields select nodes. Hidden node identity stays with the workspace owner.
	auto Omit = [&](const FString& Path, const int32 Mode, const FString& MemberName = TEXT("Counter"), const FString& Category = TEXT("real")) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Path, TEXT("*.hjson"), true, false); int32 Count = 0, MemberCount = 0;
		for (const FString& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
			if (Doc->HasTypedField<EJson::Object>(TEXT("variables"))) {
				auto Members = Doc->GetObjectField(TEXT("variables"));
				if (Members->HasField(MemberName)) { ++MemberCount; auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), Category);
					if (Category == TEXT("real")) Type->SetStringField(TEXT("pin_subcategory"), TEXT("double"));
					if (Category == TEXT("object")) Type->SetStringField(TEXT("pin_subcategory_object"), TEXT("/Script/CoreUObject.Vector"));
					if (Category == TEXT("pawn")) { Type->SetStringField(TEXT("pin_category"), TEXT("object")); Type->SetStringField(TEXT("pin_subcategory_object"), TEXT("/Script/Engine.Pawn")); }
					if (Category == TEXT("set")) { Type->SetStringField(TEXT("pin_category"), TEXT("int")); Type->SetStringField(TEXT("container_type"), TEXT("set")); }
					Members->GetObjectField(MemberName)->SetObjectField(TEXT("type"), Type); }
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
			if (!Doc->HasField(TEXT("interface_ref"))) continue;
			const auto Nodes = Doc->GetObjectField(TEXT("nodes")); TArray<FString> Deleted;
			for (const auto& Pair : Nodes->Values) {
				const auto Node = Pair.Value->AsObject(); FString Name, Kind, Class;
				Node->TryGetStringField(TEXT("node_type"), Kind);
				Node->TryGetStringField(TEXT("node_class"), Class);
				Node->TryGetStringField(TEXT("event_name"), Name);
				FString Variable, Access;
				Node->TryGetStringField(TEXT("variable_name"), Variable); Node->TryGetStringField(TEXT("access"), Access);
				const bool Getter = Kind == TEXT("variable_node") && Variable == TEXT("Counter") && Access == TEXT("get");
				const bool Setter = Kind == TEXT("variable_node") && Variable == TEXT("Counter") && Access == TEXT("set");
				if ((Mode == 0 && Setter) || (Mode == 1 && Getter) || (Mode == 2 && (Getter || Setter || Name == TEXT("SetCounter")))) Deleted.Add(Pair.Key);
			}
			Count += Deleted.Num(); for (const FString& Key : Deleted) Nodes->RemoveField(Key);
			if (Deleted.IsEmpty()) continue;
			auto Connections = Doc->GetArrayField(TEXT("connections"));
			Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Value) { const auto Edge = Value->AsObject(); return Deleted.ContainsByPredicate([&](const FString& Key) {
				return Edge->GetStringField(TEXT("from")).StartsWith(Key + TEXT(".")) || Edge->GetStringField(TEXT("to")).StartsWith(Key + TEXT(".")); }); });
			Doc->SetArrayField(TEXT("connections"), Connections);
			if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		}
		return MemberCount == 1 && Count == (Mode == 2 ? 3 : Mode == -1 ? 0 : 1);
	};
	for (const FString Category : {FString(TEXT("object")), FString(TEXT("interface"))}) {
		const FString Path = Workspace / (TEXT("invalid-") + Category); if (!Export(Path) || !Omit(Path, -1, TEXT("Counter"), Category)) return false;
		int32 Modified = 0; const auto H = FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* O) { if (O && O->GetOutermost() == Blueprint->GetOutermost()) ++Modified; });
		const auto R = Import(Path); FCoreUObjectDelegates::OnObjectModified.Remove(H);
		const FString Code = Category == TEXT("object") ? TEXT("invalid_class_pin_subcategory_object") : TEXT("missing_interface_pin_subcategory_object");
		TArray<uint8> Disk; const bool Rejected = !R.IsOk() && R.GetErrorJson().Contains(Code) && Modified == 0 && References(Blueprint)
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == OriginalDisk;
		if (!TestTrue(TEXT("invalid referenced pin type rejects before source write"), Rejected)) { AddError(R.GetErrorJson()); return false; }
	}

	auto MemberSpec = MakeShared<FJsonObject>(); MemberSpec->SetStringField(TEXT("operation"), TEXT("update"));
	auto NewType = MakeShared<FJsonObject>(); NewType->SetStringField(TEXT("pin_category"), TEXT("real")); NewType->SetStringField(TEXT("pin_subcategory"), TEXT("double")); MemberSpec->SetObjectField(TEXT("type"), NewType); MemberSpec->SetStringField(TEXT("name"), TEXT("Counter"));
	auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(MemberId.ToString(EGuidFormats::DigitsWithHyphens), MemberSpec);
	auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("members"), Members); Extension->SetBoolField(TEXT("compile"), true); Extension->SetBoolField(TEXT("save"), true);
	const bool DiskFlag = Blueprint->bRunConstructionScriptOnDrag; Blueprint->bRunConstructionScriptOnDrag = !DiskFlag; Blueprint->MarkPackageDirty();
	TWeakObjectPtr<UBlueprint> Old = Blueprint; bool Witness = false;
	FAssetApplyRequest Late; Late.Operation = UeAgentApplyInputContract::EOperation::Update; Late.AssetPath = PackageName; Late.ProviderExtension = Extension;
	Late.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> Disk;
		const bool NativeChanged = ChangedType(Current);
		Witness = !Old.IsValid() && NativeChanged && !Current->GetOutermost()->IsDirty() && FFileHelper::LoadFileToArray(Disk, *Filename) && Disk != OriginalDisk;
		AddInfo(FString::Printf(TEXT("type late stage: replaced=%d native=%d clean=%d"), !Old.IsValid(), NativeChanged, Current && !Current->GetOutermost()->IsDirty()));
		AddInfo(FString::Printf(TEXT("variable type late witness: replaced_saved_retained_native_pins_links_non_targets=%d"), Witness));
		Failure.Diagnostic.Code = TEXT("test_variable_type_late_rejection"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code; return false;
	};
	Blueprint = nullptr; Get = nullptr; Set = nullptr; Invoke = nullptr; Graph = nullptr;
	FAssetApplyResult LateResult; const bool Applied = ApplyAsset(Late, LateResult, TEXT("variable-type-late"));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); TArray<uint8> RecoveredDisk;
	AddInfo(FString::Printf(TEXT("type recovery stage: attempted=%d restored=%d references=%d dirty=%d"), LateResult.bRollbackAttempted, LateResult.bRollbackSucceeded, References(Blueprint), Blueprint && Blueprint->GetOutermost()->IsDirty()));
	const bool Recovered = !Applied && Witness && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded && References(Blueprint)
		&& Blueprint->GetOutermost()->IsDirty() && Blueprint->bRunConstructionScriptOnDrag == !DiskFlag
		&& FFileHelper::LoadFileToArray(RecoveredDisk, *Filename) && RecoveredDisk == OriginalDisk;
	AddInfo(FString::Printf(TEXT("variable type recovery: original_memory_disk_dirty_pin_references=%d"), Recovered));
	if (!TestTrue(TEXT("late type rejection restores native identities references memory disk dirty"), Recovered)) { AddError(LateResult.Diagnostic.Code + LateResult.Diagnostic.PublicMessage); return false; }
	Blueprint->bRunConstructionScriptOnDrag = DiskFlag; Blueprint->GetOutermost()->SetDirtyFlag(false);
	const FString SuccessPath = Workspace / TEXT("success");
	if (!Export(SuccessPath) || !Omit(SuccessPath, -1)) { AddError(Error.ToPublicDetail()); return false; }
	Old = Blueprint; Blueprint = nullptr; const auto Result = Import(SuccessPath);
	if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const FString ReadbackPath = Workspace / TEXT("readback");
	bool ExportReadback = Export(ReadbackPath);
	TArray<FString> ReadbackFiles; IFileManager::Get().FindFilesRecursive(ReadbackFiles, *ReadbackPath, TEXT("*.hjson"), true, false);
	bool FoundConsumer = false, FoundType = false;
	for (const FString& File : ReadbackFiles) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Doc;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
		if (Doc->HasTypedField<EJson::Object>(TEXT("variables"))) {
			const auto VariablesReadback = Doc->GetObjectField(TEXT("variables"));
			if (VariablesReadback->HasTypedField<EJson::Object>(TEXT("Counter"))) {
				const auto Type = VariablesReadback->GetObjectField(TEXT("Counter"))->GetObjectField(TEXT("type"));
				FoundType |= Type->GetStringField(TEXT("pin_category")) == TEXT("real") && Type->GetStringField(TEXT("pin_subcategory")) == TEXT("double");
			}
		}
		if (Doc->HasField(TEXT("interface_ref"))) for (const auto& Pair : Doc->GetObjectField(TEXT("nodes"))->Values) {
			FString Variable; Pair.Value->AsObject()->TryGetStringField(TEXT("variable_name"), Variable);
			FoundConsumer |= Variable == TEXT("Counter");
		}
	}
	const bool Success = !Old.IsValid() && ChangedType(Blueprint) && !Blueprint->GetOutermost()->IsDirty() && ExportReadback && FoundConsumer && FoundType;
	AddInfo(FString::Printf(TEXT("type export readback: export=%d consumer=%d type=%d clean=%d"), ExportReadback, FoundConsumer, FoundType, !Blueprint->GetOutermost()->IsDirty()));
	AddInfo(FString::Printf(TEXT("variable type public: replaced_clean_retained_native_pins_links_non_targets_independent_export=%d"), Success));
	if (!TestTrue(TEXT("public type change preserves referenced Get Set nodes and connections"), Success)) return false;
	const FString MemberCallPath = Workspace / TEXT("member-call-compatible");
	if (!Export(MemberCallPath) || !Omit(MemberCallPath, -1, TEXT("CallTarget"), TEXT("pawn"))) return false;
	Old = Blueprint; Blueprint = nullptr; const auto MemberCallResult = Import(MemberCallPath);
	if (!MemberCallResult.IsOk()) { AddError(MemberCallResult.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	const auto* ObjectProperty = Blueprint && Blueprint->GeneratedClass ? FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("CallTarget")) : nullptr;
	const bool MemberCallSuccess = !Old.IsValid() && ChangedType(Blueprint) && ObjectProperty
		&& ObjectProperty->PropertyClass->GetPathName() == TEXT("/Script/Engine.Pawn") && !Blueprint->GetOutermost()->IsDirty()
		&& Export(Workspace / TEXT("member-call-readback"));
	AddInfo(FString::Printf(TEXT("variable type member call: compatible_class_retained_identity_saved=%d"), MemberCallSuccess));
	if (!TestTrue(TEXT("compatible member-object call migrates without cross-asset writes"), MemberCallSuccess)) return false;
	Graph = Blueprint->UbergraphPages[0];
	for (UEdGraphNode* Node : Graph->Nodes) if (Node->NodeGuid == GetId) Get = Cast<UK2Node_VariableGet>(Node);
	FGraphNodeCreator<UK2Node_CallFunction> FixedCreator(*Graph); auto* Fixed = FixedCreator.CreateNode();
	Fixed->SetFromFunction(UKismetStringLibrary::StaticClass()->FindFunctionByName(TEXT("Conv_DoubleToString"))); FixedCreator.Finalize();
	if (!Get || !Schema->TryCreateConnection(Get->FindPin(TEXT("Counter")), Fixed->FindPin(TEXT("InDouble"))) || !Save()) return false;
	const FGuid FixedId = Fixed->NodeGuid;
	const FString IncompatiblePath = Workspace / TEXT("explicit-link-removal");
	if (!Export(IncompatiblePath) || !Omit(IncompatiblePath, -1, TEXT("Counter"), TEXT("string"))) return false;
	TArray<FString> EditFiles; IFileManager::Get().FindFilesRecursive(EditFiles, *IncompatiblePath, TEXT("*.hjson"), true, false);
	int32 ExplicitlyRemoved = 0;
	for (const FString& File : EditFiles) {
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Doc;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Doc, Error)) return false;
		if (!Doc->HasField(TEXT("interface_ref"))) continue;
		auto Connections = Doc->GetArrayField(TEXT("connections"));
		ExplicitlyRemoved += Connections.RemoveAll([](const TSharedPtr<FJsonValue>& Value) { return Value->AsObject()->GetStringField(TEXT("to")).EndsWith(TEXT(".InDouble")); });
		Doc->SetArrayField(TEXT("connections"), Connections);
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Doc.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestEqual(TEXT("only explicitly incompatible connection omitted"), ExplicitlyRemoved, 1)) return false;
	Old = Blueprint; Blueprint = nullptr; Get = nullptr; Fixed = nullptr; Graph = nullptr;
	const auto Disconnected = Import(IncompatiblePath);
	if (!Disconnected.IsOk()) { AddError(Disconnected.GetErrorJson()); return false; }
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	Get = nullptr; Set = nullptr;
	for (UEdGraph* G : Blueprint->UbergraphPages) for (UEdGraphNode* Node : G->Nodes) {
		if (Node->NodeGuid == GetId) Get = Cast<UK2Node_VariableGet>(Node);
		if (Node->NodeGuid == SetId) Set = Cast<UK2Node_VariableSet>(Node);
		if (Node->NodeGuid == FixedId) Fixed = Cast<UK2Node_CallFunction>(Node);
	}
	const bool ExplicitSuccess = !Old.IsValid() && Keep(Blueprint) && Blueprint->Status != BS_Error && !Blueprint->GetOutermost()->IsDirty()
		&& FindFProperty<FStrProperty>(Blueprint->GeneratedClass, TEXT("Counter")) && Get && Set && Fixed
		&& Get->FindPin(TEXT("Counter"))->PinType.PinCategory == UEdGraphSchema_K2::PC_String
		&& Get->FindPin(TEXT("Counter"))->LinkedTo.Contains(Set->FindPin(TEXT("Counter"))) && Fixed->FindPin(TEXT("InDouble"))->LinkedTo.IsEmpty()
		&& Export(Workspace / TEXT("explicit-link-readback"));
	AddInfo(FString::Printf(TEXT("variable type explicit link removal: saved_retained_nodes=%d"), ExplicitSuccess));
	return TestTrue(TEXT("explicit connection removal is preserved during native type migration"), ExplicitSuccess);
}

