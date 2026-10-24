#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeGraphDocument.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* BehaviorTreeType =
		TEXT("/Script/AIModule.BehaviorTree");
	constexpr const TCHAR* CompositeGuidText =
		TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	constexpr const TCHAR* TaskGuidText =
		TEXT("11111111-2222-3333-4444-555555555555");
	constexpr const TCHAR* FixtureFolder =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction");

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	void DeleteOwnedFixtureAssets()
	{
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.ScanPathsSynchronous({FixtureFolder}, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(FixtureFolder), Assets);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* const Object = Asset.GetAsset())
			{
				Objects.Add(Object);
			}
		}
		if (!Objects.IsEmpty())
		{
			ObjectTools::DeleteObjectsUnchecked(Objects);
		}
	}

	bool SaveBehaviorTree(
		UPackage& Package,
		UBehaviorTree& BehaviorTree,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &BehaviorTree, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	TSharedRef<FJsonObject> CreateEdit(
		const FString& SemanticId,
		const FString& ElementKind,
		const TArray<FString>& Dependencies,
		const TSharedRef<FJsonObject>& Value)
	{
		TArray<TSharedPtr<FJsonValue>> DependencyValues;
		for (const FString& Dependency : Dependencies)
		{
			DependencyValues.Add(MakeShared<FJsonValueString>(Dependency));
		}
		const TSharedRef<FJsonObject> Edit = MakeShared<FJsonObject>();
		Edit->SetArrayField(TEXT("depends_on"), MoveTemp(DependencyValues));
		Edit->SetStringField(TEXT("element_kind"), ElementKind);
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetStringField(TEXT("identity"), SemanticId);
		Edit->SetObjectField(TEXT("value"), Value);
		return Edit;
	}

	bool TryBuildResolvedBehaviorTreeImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CompositeCandidateName,
		const FString& TaskCandidateName,
		const bool bRootDirectTask,
		FString& OutParams,
		FString& OutError,
		const TMap<FString, FString>& TaskSelect = {})
	{
		const FString CompositeId =
			TEXT("node:") + FString(CompositeGuidText);
		const FString TaskId = TEXT("node:") + FString(TaskGuidText);
		const FString RootTargetId =
			bRootDirectTask ? TaskId : CompositeId;
		const FString RootRelationId = FString::Printf(
			TEXT("relation:node:root:out:0->%s:in:0"), *RootTargetId);
		const FString TaskRelationId = FString::Printf(
			TEXT("relation:%s:out:0->%s:in:0"), *CompositeId, *TaskId);

		const TSharedRef<FJsonObject> RootValue = MakeShared<FJsonObject>();
		RootValue->SetStringField(
			TEXT("carrier_type_ref"),
			TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"));

		const TSharedRef<FJsonObject> CompositeCandidate =
			MakeShared<FJsonObject>();
		CompositeCandidate->SetStringField(
			TEXT("name"), CompositeCandidateName);
		const TSharedRef<FJsonObject> CompositeValue = MakeShared<FJsonObject>();
		CompositeValue->SetObjectField(
			TEXT("candidate"), CompositeCandidate);
		CompositeValue->SetArrayField(
			TEXT("position"),
			{MakeShared<FJsonValueNumber>(384.0),
				MakeShared<FJsonValueNumber>(192.0)});

		const TSharedRef<FJsonObject> TaskCandidate = MakeShared<FJsonObject>();
		TaskCandidate->SetStringField(TEXT("name"), TaskCandidateName);
		if (!TaskSelect.IsEmpty())
		{
			const auto Select = MakeShared<FJsonObject>();
			for (const auto& Field : TaskSelect) Select->SetStringField(Field.Key, Field.Value);
			TaskCandidate->SetObjectField(TEXT("select"), Select);
		}
		const TSharedRef<FJsonObject> TaskValue = MakeShared<FJsonObject>();
		TaskValue->SetObjectField(TEXT("candidate"), TaskCandidate);
		TaskValue->SetArrayField(
			TEXT("position"),
			{MakeShared<FJsonValueNumber>(bRootDirectTask ? 384.0 : 640.0),
				MakeShared<FJsonValueNumber>(192.0)});

		const TSharedRef<FJsonObject> RootRelationValue =
			MakeShared<FJsonObject>();
		RootRelationValue->SetStringField(
			TEXT("from_item_ref"), TEXT("node:root"));
		RootRelationValue->SetNumberField(TEXT("from_output_index"), 0);
		RootRelationValue->SetStringField(
			TEXT("to_item_ref"), RootTargetId);
		RootRelationValue->SetNumberField(TEXT("to_input_index"), 0);

		const TSharedRef<FJsonObject> TaskRelationValue =
			MakeShared<FJsonObject>();
		TaskRelationValue->SetStringField(
			TEXT("from_item_ref"), CompositeId);
		TaskRelationValue->SetNumberField(TEXT("from_output_index"), 0);
		TaskRelationValue->SetStringField(TEXT("to_item_ref"), TaskId);
		TaskRelationValue->SetNumberField(TEXT("to_input_index"), 0);

		TArray<TSharedPtr<FJsonValue>> Edits;
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			TEXT("node:root"), TEXT("item"), {}, RootValue)));
		if (!bRootDirectTask)
		{
			Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
				CompositeId, TEXT("item"), {}, CompositeValue)));
		}
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			TaskId, TEXT("item"), {}, TaskValue)));
		Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
			RootRelationId,
			TEXT("relation"),
			{TEXT("node:root"), RootTargetId},
			RootRelationValue)));
		if (!bRootDirectTask)
		{
			Edits.Add(MakeShared<FJsonValueObject>(CreateEdit(
				TaskRelationId,
				TEXT("relation"),
				{CompositeId, TaskId},
				TaskRelationValue)));
		}

		const TSharedRef<FJsonObject> NativeParams = MakeShared<FJsonObject>();
		NativeParams->SetArrayField(TEXT("edits"), MoveTemp(Edits));
		const TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(
			TEXT("command_id"), TEXT("ai.behavior_tree.graph.apply"));
		Operation->SetObjectField(TEXT("params"), NativeParams);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), BehaviorTreeType);
		Root->SetObjectField(TEXT("operation"), Operation);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.ai_behavior.edit/1"));
		Document->SetObjectField(TEXT("root"), Root);
		FString DocumentText;
		if (!CommandContract::Json::TryCanonicalize(
				Document, DocumentText, OutError))
		{
			return false;
		}

		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("inline"));
		Source->SetStringField(TEXT("document"), DocumentText);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedSource;
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!HjsonWorkspace::TryLoadContentGraphImportSource(
				Source, Target, Resources, ResolvedSource, WorkspaceError)
			|| !ResolvedSource.IsValid())
		{
			OutError = WorkspaceError.GetCode() + TEXT(":")
				+ WorkspaceError.GetSourceUnit();
			return false;
		}

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(
			TEXT("source"), ResolvedSource->MakeNormalizedSourceObject());
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	bool TryBuildOrdinaryBehaviorTreeCreateParams(const FString& Target,
		const FString& SelectorName, const FString& WaitName, FString& OutParams, FString& OutError,
		const TMap<FString, FString>& WaitSelect = {}, const FString& ServiceName = FString(),
		const FString& DecoratorName = FString())
	{
		const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
			TEXT("tmp/TROOT-UAI-208-graph-common-foundation/behavior-tree-common-r1"),
			TEXT("create-workspace-") + FPackageName::GetLongPackageAssetName(Target)));
		const auto Export = MakeShared<FJsonObject>();
		Export->SetStringField(TEXT("target"), Target);
		Export->SetStringField(TEXT("workspace_path"), Workspace);
		FString Params, Payload;
		bool bReadOnly = false;
		HjsonWorkspace::FWorkspaceError Error;
		if (!CommandContract::Json::TryCanonicalize(Export, Params, OutError)
			|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
				Target, BehaviorTreeType, Params, Payload, bReadOnly, Error))
		{
			OutError += Error.GetCode() + TEXT(":") + Error.GetJsonPointer();
			return false;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
		FString GraphPath;
		for (const FString& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *File) && Text.Contains(TEXT("connections:")) && Text.Contains(TEXT("nodes:")))
			{
				if (!GraphPath.IsEmpty()) { OutError = TEXT("test_behavior_graph_ambiguous"); return false; }
				GraphPath = File;
			}
		}
		if (GraphPath.IsEmpty()) { OutError = TEXT("test_behavior_graph_missing"); return false; }
		const auto Document = MakeShared<FJsonObject>();
		const auto Graph = MakeShared<FJsonObject>();
		Graph->SetStringField(TEXT("graph_kind"), TEXT("behavior_tree"));
		Graph->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(Target));
		Document->SetObjectField(TEXT("graph"), Graph);
		Document->SetField(TEXT("blackboard"), MakeShared<FJsonValueNull>());
		const auto Nodes = MakeShared<FJsonObject>();
		auto AddNode = [&](const TCHAR* Alias, const FString& Name, int32 X, int32 Y, bool bRoot = false) {
			const auto Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("name"), Name);
			if (bRoot) Node->SetStringField(TEXT("role"), TEXT("root"));
			const auto Pos = MakeShared<FJsonObject>();
			Pos->SetNumberField(TEXT("x"), X);
			Pos->SetNumberField(TEXT("y"), Y);
			Node->SetObjectField(TEXT("pos"), Pos);
			Nodes->SetObjectField(Alias, Node);
		};
		AddNode(TEXT("Root1"), TEXT("根"), 0, 0, true);
		AddNode(TEXT("Selector1"), SelectorName, 384, 192);
		AddNode(TEXT("Wait1"), WaitName, 640, 192);
		if (!ServiceName.IsEmpty())
		{
			const auto Service = MakeShared<FJsonObject>();
			Service->SetStringField(TEXT("name"), ServiceName);
			Service->SetStringField(TEXT("parent"), TEXT("Wait1"));
			Service->SetStringField(TEXT("attachment_kind"), TEXT("service"));
			Service->SetNumberField(TEXT("order"), 0);
			Nodes->SetObjectField(TEXT("Focus1"), Service);
		}
		if (!WaitSelect.IsEmpty())
		{
			const auto Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), WaitName);
			const auto Select = MakeShared<FJsonObject>();
			for (const auto& Field : WaitSelect) Select->SetStringField(Field.Key, Field.Value);
			Candidate->SetObjectField(TEXT("select"), Select);
			const auto Wait = Nodes->GetObjectField(TEXT("Wait1"));
			Wait->RemoveField(TEXT("name"));
			Wait->SetObjectField(TEXT("candidate"), Candidate);
		}
		if (!DecoratorName.IsEmpty())
		{
			const auto Decorator = MakeShared<FJsonObject>();
			Decorator->SetStringField(TEXT("name"), DecoratorName);
			Decorator->SetStringField(TEXT("parent"), TEXT("Wait1"));
			Decorator->SetStringField(TEXT("attachment_kind"), TEXT("decorator"));
			Decorator->SetNumberField(TEXT("order"), 0);
			Nodes->SetObjectField(TEXT("Gate1"), Decorator);
		}
		Document->SetObjectField(TEXT("nodes"), Nodes);
		TArray<TSharedPtr<FJsonValue>> Connections;
		auto Connect = [&](const TCHAR* From, const TCHAR* To) {
			const auto Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("from"), From);
			Edge->SetStringField(TEXT("to"), To);
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
		};
		Connect(TEXT("Root1.In"), TEXT("Selector1.In"));
		Connect(TEXT("Selector1.Out"), TEXT("Wait1.In"));
		Document->SetArrayField(TEXT("connections"), Connections);
		FString Text;
		if (!CommandContract::Json::TryCanonicalize(Document, Text, OutError)
			|| !FFileHelper::SaveStringToFile(Text, *GraphPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		const auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Workspace);
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
		FString Revision, Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, Target, Resources, Resolved, Revision, Authorization, Error))
		{
			OutError = Error.GetCode() + TEXT(":") + Error.GetJsonPointer();
			return false;
		}
		const auto Import = MakeShared<FJsonObject>();
		Import->SetStringField(TEXT("target"), Target);
		Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		return CommandContract::Json::TryCanonicalize(Import, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeServiceContextTest,
	"UAI.Root.T85.BehaviorTree.ServiceContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeServiceContextTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace OperationOwners::AiBehavior;
	TArray<FString> CreatedAssets;
	ON_SCOPE_EXIT
	{
		for (const FString& Path : CreatedAssets) DeleteAssetIfPresent(Path);
		for (const FString& Path : CreatedAssets) T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, FPackageName::ObjectPathToPackageName(Path));
	};
	FString ServiceName, Error;
	for (const bool bWithService : {false, true})
	{
		const FString Name = TEXT("BT_Service_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = FString(FixtureFolder) / Name;
		const FString ObjectPath = PackageName + TEXT(".") + Name;
		UPackage* Package = CreatePackage(*PackageName);
		UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		CreatedAssets.Add(ObjectPath);
		FAssetRegistryModule::AssetCreated(Tree);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!SaveBehaviorTree(*Package, *Tree, Filename)) return false;
		FString Params, Payload;
		if (!TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, TEXT("Selector"), TEXT("Wait"), Params, Error, {}, ServiceName))
		{ AddError(Error); return false; }
		TWeakObjectPtr<UBehaviorTree> Before(Tree);
		Tree = nullptr;
		Package = nullptr;
		bool bReadOnly = true;
		HjsonWorkspace::FWorkspaceError ImportError;
		if (!TestTrue(TEXT("ordinary creation resolves parent before its named service"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError)))
		{ AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetJsonPointer()); return false; }
		TestFalse(TEXT("the service creation path releases the original tree"), Before.IsValid());
		Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
		auto* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
		if (!TestTrue(TEXT("the tree is independently reloaded and clean"), Graph && !Tree->GetOutermost()->IsDirty())) return false;
		UBehaviorTreeGraphNode_Task* Task = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes) if (auto* Match = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Match;
		if (!TestNotNull(TEXT("the parent task exists"), Task)) return false;
		if (bWithService)
		{
			if (!TestTrue(TEXT("native and compiled service ownership persist"), Task->Services.Num() == 1
				&& Task->SubNodes.Num() == 1 && Task->Services[0]->NodeInstance->IsA<UBTService_DefaultFocus>()
				&& Tree->RootNode->Children[0].ChildTask->Services.Num() == 1
				&& Tree->RootNode->Children[0].ChildTask->Services[0]->IsA<UBTService_DefaultFocus>())) return false;
			continue;
		}
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService> Service;
		if (!TryCreateProductionAiBehaviorCandidateCommandService(Service, Error)) return false;
		const auto Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("target"), ObjectPath);
		Query->SetStringField(TEXT("query"), TEXT("Default Focus"));
		Query->SetStringField(TEXT("parent"), TEXT("Wait1"));
		Query->SetStringField(TEXT("attachment_kind"), TEXT("service"));
		FAiBehaviorCandidateCommandRequest Request;
		Request.CommandId = TEXT("ai.behavior.candidates");
		FAiBehaviorCandidateCommandResponse Response;
		FAiBehaviorCandidateCommandError QueryError;
		CommandContract::Json::TryCanonicalize(Query, Request.CanonicalParamsJson, Error);
		if (!TestTrue(TEXT("the public candidate owner resolves the exported parent alias"), Service->Execute(Request, Response, QueryError)))
		{ AddError(QueryError.Code + TEXT(":") + QueryError.PublicMessage); return false; }
		TSharedPtr<FJsonObject> Result;
		if (!CommandContract::Json::TryParseObject(Response.PayloadJson, Result, Error)) return false;
		const auto Names = Result->GetArrayField(TEXT("names"));
		if (!TestEqual(TEXT("the native service name is discoverable"), Names.Num(), 1)) return false;
		ServiceName = Names[0]->AsString();
		Query->SetStringField(TEXT("attachment_kind"), TEXT("invalid"));
		CommandContract::Json::TryCanonicalize(Query, Request.CanonicalParamsJson, Error);
		TestFalse(TEXT("invalid attachment context is rejected"), Service->Execute(Request, Response, QueryError));
		TestEqual(TEXT("invalid context has a specific error"), QueryError.Code, FString(TEXT("ai_behavior_candidate_attachment_context_invalid")));
		TestTrue(TEXT("candidate queries do not create attachments or dirty the tree"), Task->SubNodes.IsEmpty() && !Tree->GetOutermost()->IsDirty());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeSameNameSelectionTest,
	"UAI.Root.T85.BehaviorTree.SameNameSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeSameNameSelectionTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;
	TArray<FString> CreatedAssets;
	ON_SCOPE_EXIT
	{
		TMap<FString, FString> FixtureRoots;
		for (int32 Index = CreatedAssets.Num() - 1; Index >= 0; --Index)
		{
			DeleteAssetIfPresent(CreatedAssets[Index]);
			const FString PackageName = FPackageName::ObjectPathToPackageName(CreatedAssets[Index]);
			FixtureRoots.Add(FPackageName::GetLongPackagePath(PackageName), PackageName);
		}
		for (const auto& Root : FixtureRoots)
			UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, Root.Value);
	};
	TArray<FString> TaskClasses;
	for (const TCHAR* Folder : {TEXT("T85BehaviorTreeSelectLeft"), TEXT("T85BehaviorTreeSelectRight")})
	{
		const FString PackageName = FString(TEXT("/Game/UAI/Transient/")) + Folder + TEXT("/BTTask_SelectTwin");
		const FString ObjectPath = PackageName + TEXT(".BTTask_SelectTwin");
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!TestFalse(TEXT("same-name fixture does not overwrite an existing asset"),
			FindPackage(nullptr, *PackageName) != nullptr || IFileManager::Get().FileExists(*Filename))) return false;
		UPackage* Package = CreatePackage(*PackageName);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(UBTTask_BlueprintBase::StaticClass(),
			Package, TEXT("BTTask_SelectTwin"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!TestNotNull(TEXT("a native BTTask Blueprint fixture is created"), Blueprint)) return false;
		CreatedAssets.Add(ObjectPath);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestTrue(TEXT("the BTTask Blueprint compiles"), Blueprint->GeneratedClass && Blueprint->Status != BS_Error)) return false;
		FAssetRegistryModule::AssetCreated(Blueprint);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("the same-name Blueprint is persisted and registered"),
			UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs))) return false;
		Package->SetDirtyFlag(false);
		TaskClasses.Add(Blueprint->GeneratedClass->GetPathName());
	}
	// The two ingress modes share creation/lifecycle mechanics but independently carry the selector.
	for (const bool bIncremental : {false, true})
	{
		const FString AssetName = FString(bIncremental ? TEXT("BT_SelectIncremental_") : TEXT("BT_SelectOrdinary_"))
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = FString(FixtureFolder) / AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		UPackage* Package = CreatePackage(*PackageName);
		UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		CreatedAssets.Add(ObjectPath);
		FAssetRegistryModule::AssetCreated(Tree);
		if (!TestTrue(TEXT("same-name target starts saved and empty"), SaveBehaviorTree(*Package, *Tree, Filename))) return false;
		FAiBehaviorCandidateQueryResult Catalog;
		FAiBehaviorCandidateQueryError Error;
		if (!TestTrue(TEXT("the native menu includes both registered Blueprint assets"),
			TryQueryAiBehaviorCandidates(*Tree, TEXT("SelectTwin"), Catalog, Error)))
		{
			AddError(Error.Code + TEXT(":") + Error.PublicMessage);
			return false;
		}
		TArray<FAiBehaviorCandidate> Twins;
		for (const auto& Candidate : Catalog.Candidates)
			if (TaskClasses.Contains(Candidate.PayloadType)) Twins.Add(Candidate);
		if (!TestEqual(TEXT("both native candidates survive catalog identity deduplication"), Twins.Num(), 2)) return false;
		TestEqual(TEXT("discovery preserves the same original name"), Twins[0].Name, Twins[1].Name);
		FAiBehaviorCandidate Resolved;
		TestFalse(TEXT("a same-name native menu selection fails closed"),
			TryResolveAiBehaviorCandidate(*Tree, Twins[0].Name, FString(), Resolved, Error));
		if (!TestEqual(TEXT("ambiguity uses the shared error identity"), Error.Code, FString(TEXT("candidate_ambiguous"))))
		{
			AddError(Error.PublicMessage);
			return false;
		}
		if (!TestTrue(TEXT("native ambiguity carries readable correction choices"),
			Error.CandidateChoices.IsValid() && Error.CandidateChoices->Type == EJson::Object)) return false;
		const TSharedPtr<FJsonObject> Choose = Error.CandidateChoices->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (!TestTrue(TEXT("only the differing readable source is needed"),
			Choose.IsValid() && Choose->Values.Num() == 1 && Choose->TryGetArrayField(TEXT("source"), Choices)
			&& Choices && Choices->Num() == 2)) return false;
		const FString SelectedSource = (*Choices)[bIncremental ? 1 : 0]->AsString();
		const TMap<FString, FString> Select{{TEXT("source"), SelectedSource}};
		if (!TestTrue(TEXT("returned source choice replays without an identity mapping"),
			TryResolveAiBehaviorCandidate(*Tree, Twins[0].Name, FString(), Resolved, Error, Select))) return false;
		const FString ExpectedClass = Resolved.PayloadType;
		if (!TestTrue(TEXT("the replay selects one of the two actual Blueprint classes"), TaskClasses.Contains(ExpectedClass))) return false;
		FAiBehaviorCandidate Selector;
		if (!TestTrue(TEXT("the native Selector remains name-only"),
			TryResolveAiBehaviorCandidate(*Tree, TEXT("Selector"), FString(), Selector, Error))) return false;
		FString Params, BuildError, Payload;
		bool bReadOnly = true;
		HjsonWorkspace::FWorkspaceError ImportError;
		if (!bIncremental)
		{
			TArray<uint8> Before, After;
			if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
			if (!TestTrue(TEXT("ordinary ambiguous input is well formed"),
				TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, Selector.Name, Twins[0].Name, Params, BuildError))) return false;
			TestFalse(TEXT("ordinary import rejects real ambiguity before mutation"),
				DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
					PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError));
			TestEqual(TEXT("ordinary import preserves ambiguity code"), ImportError.GetCode(), FString(TEXT("candidate_ambiguous")));
			TestTrue(TEXT("ordinary import preserves the same correction payload"), ImportError.GetJsonPointer().Contains(SelectedSource));
			TestTrue(TEXT("ambiguity preserves disk and empty clean graph"),
				FFileHelper::LoadFileToArray(After, *Filename) && After == Before && !Package->IsDirty()
				&& !Tree->RootNode && (!Tree->BTGraph || Tree->BTGraph->Nodes.IsEmpty()));
		}
		const bool bBuilt = bIncremental
			? TryBuildResolvedBehaviorTreeImportParams(PackageName, Catalog.TargetRevision, Selector.Name,
				Twins[0].Name, false, Params, BuildError, Select)
			: TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, Selector.Name, Twins[0].Name, Params, BuildError, Select);
		if (!TestTrue(TEXT("the returned selector enters the requested authoring ingress"), bBuilt))
		{
			AddError(BuildError);
			return false;
		}
		TWeakObjectPtr<UBehaviorTree> Before(Tree);
		Tree = nullptr;
		Package = nullptr;
		if (!TestTrue(TEXT("same-name selection reaches native action and persistent readback"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError)))
		{
			AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetJsonPointer());
			return false;
		}
		TestFalse(TEXT("selected creation releases the original tree"), Before.IsValid());
		Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
		if (!TestTrue(TEXT("selected Blueprint payload survives reload under the native Selector"), Tree
			&& Tree->RootNode && Tree->RootNode->Children.Num() == 1 && Tree->RootNode->Children[0].ChildTask
			&& Tree->RootNode->Children[0].ChildTask->GetClass()->GetPathName() == ExpectedClass
			&& !Tree->GetOutermost()->IsDirty())) return false;
	}
	return !HasAnyErrors();
}

class FBehaviorTreeInjectedWorkspaceTestBase : public FAutomationTestBase
{
public:
	FBehaviorTreeInjectedWorkspaceTestBase(const FString& Name, bool bComplex) : FAutomationTestBase(Name, bComplex) {}
	bool RunInjectedWorkspace(bool bCompositeSource);
};

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeInjectedWorkspaceTest,
	FBehaviorTreeInjectedWorkspaceTestBase,
	"UAI.Root.T85.BehaviorTree.InjectedWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeInjectedWorkspaceTest::RunTest(const FString& Parameters)
{
	return RunInjectedWorkspace(false);
}

IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeInjectedCompositeWorkspaceTest,
	FBehaviorTreeInjectedWorkspaceTestBase,
	"UAI.Root.T85.BehaviorTree.InjectedCompositeWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeInjectedCompositeWorkspaceTest::RunTest(const FString& Parameters)
{
	return RunInjectedWorkspace(true);
}

bool FBehaviorTreeInjectedWorkspaceTestBase::RunInjectedWorkspace(bool bCompositeSource)
{
	using namespace UE::UAI;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ChildPackage = FString(FixtureFolder) / (TEXT("BT_InjectedChild_") + Suffix);
	const FString ParentPackage = FString(FixtureFolder) / (TEXT("BT_InjectedParent_") + Suffix);
	const FString ChildPath = ChildPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(ChildPackage);
	const FString ParentPath = ParentPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(ParentPackage);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ParentPath);
		DeleteAssetIfPresent(ChildPath);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, ParentPackage);
	};
	auto CreateFixture = [&](const FString& PackageName) -> UBehaviorTree*
	{
		UPackage* Package = CreatePackage(*PackageName);
		const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
		auto* Tree = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Tree);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (!SaveBehaviorTree(*Package, *Tree, Filename)) return nullptr;
		FString Params, Error, Payload;
		bool bReadOnly = true;
		HjsonWorkspace::FWorkspaceError ImportError;
		if (!TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, TEXT("Selector"), TEXT("Wait"), Params, Error)
			|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError))
		{ AddError(Error + ImportError.GetCode()); return nullptr; }
		return LoadObject<UBehaviorTree>(nullptr, *(PackageName + TEXT(".") + Name));
	};
	UBehaviorTree* Child = CreateFixture(ChildPackage);
	if (!TestNotNull(TEXT("child fixture exists"), Child)) return false;
	auto* ChildGraph = Cast<UBehaviorTreeGraph>(Child->BTGraph);
	UBehaviorTreeGraphNode_Composite* ChildRoot = nullptr;
	for (UEdGraphNode* Node : ChildGraph->Nodes)
		if (auto* Composite = Cast<UBehaviorTreeGraphNode_Composite>(Node)) ChildRoot = Composite;
	if (!TestNotNull(TEXT("child root composite exists"), ChildRoot)) return false;
	auto* Cooldown = NewObject<UBTDecorator_Cooldown>(Child, NAME_None, RF_Transactional);
	Cooldown->CoolDownTime = 7.25f;
	if (bCompositeSource)
	{
		auto* SourceNode = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>(ChildGraph);
		ChildRoot->AddSubNode(SourceNode, ChildGraph);
		auto* Second = NewObject<UBTDecorator_Cooldown>(Child, NAME_None, RF_Transactional);
		Second->CoolDownTime = 9.5f;
		const TArray<UBTDecorator*> Instances{Cooldown, Second};
		const TArray<FBTDecoratorLogic> Operations{
			FBTDecoratorLogic(EBTDecoratorLogic::And, 2),
			FBTDecoratorLogic(EBTDecoratorLogic::Test, 0), FBTDecoratorLogic(EBTDecoratorLogic::Test, 1)};
		if (!TestEqual(TEXT("source composite owns a native And expression"), SourceNode->SpawnMissingNodes(Instances, Operations, 0), 3)) return false;
	}
	else
	{
		auto* SourceNode = NewObject<UBehaviorTreeGraphNode_Decorator>(ChildGraph);
		SourceNode->NodeInstance = Cooldown;
		ChildRoot->AddSubNode(SourceNode, ChildGraph);
	}
	ChildGraph->UpdateAsset();
	if (!TestEqual(TEXT("child supplies the native root decorators"), Child->RootDecorators.Num(), bCompositeSource ? 2 : 1)) return false;
	if (!SaveBehaviorTree(*Child->GetOutermost(), *Child,
		FPackageName::LongPackageNameToFilename(ChildPackage, FPackageName::GetAssetPackageExtension()))) return false;
	UBehaviorTree* Parent = CreateFixture(ParentPackage);
	if (!TestNotNull(TEXT("parent fixture exists"), Parent)) return false;
	// Parent import may unload unrelated references; resolve the saved source again.
	Child = LoadObject<UBehaviorTree>(nullptr, *ChildPath);
	auto* ParentGraph = Cast<UBehaviorTreeGraph>(Parent->BTGraph);
	const FObjectPropertyBase* AssetProperty = FindFProperty<FObjectPropertyBase>(UBTTask_RunBehavior::StaticClass(), TEXT("BehaviorAsset"));
	if (!TestNotNull(TEXT("native subtree asset property exists"), AssetProperty)) return false;
	TArray<UBehaviorTreeGraphNode_SubtreeTask*> Tasks;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Node = NewObject<UBehaviorTreeGraphNode_SubtreeTask>(ParentGraph);
		auto* Task = NewObject<UBTTask_RunBehavior>(Parent, NAME_None, RF_Transactional);
		AssetProperty->SetObjectPropertyValue_InContainer(Task, Child);
		Node->NodeInstance = Task;
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		ParentGraph->AddNode(Node, false, false);
		Tasks.Add(Node);
	}
	if (!TestTrue(TEXT("native graph generates both subtree previews"), ParentGraph->UpdateInjectedNodes())) return false;
	if (!TestTrue(TEXT("each subtree task owns one distinct read-only injected copy"),
		Tasks[0]->Decorators.Num() == 1 && Tasks[1]->Decorators.Num() == 1
		&& Tasks[0]->Decorators[0] != Tasks[1]->Decorators[0]
		&& Tasks[0]->Decorators[0]->bInjectedNode && Tasks[1]->Decorators[0]->bInjectedNode
		&& Tasks[0]->Decorators[0]->bIsReadOnly && Tasks[1]->Decorators[0]->bIsReadOnly)) return false;
	TestEqual(TEXT("native copies preserve the source node Guid across owners"),
		Tasks[0]->Decorators[0]->NodeGuid, Tasks[1]->Decorators[0]->NodeGuid);
	if (bCompositeSource)
	{
		for (const auto* Task : Tasks)
		{
			const auto* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Task->Decorators[0]);
			if (!TestTrue(TEXT("native injected composite retains a read-only graph owned by the copied node"),
				Composite && Composite->BoundGraph && Composite->BoundGraph->GetOuter() == Composite
				&& !Composite->BoundGraph->bEditable && Composite->BoundGraph->Nodes.Num() == 4)) return false;
			int32 Registrations = 0;
			for (const auto& SubGraph : ParentGraph->SubGraphs) Registrations += SubGraph == Composite->BoundGraph;
			AddInfo(FString::Printf(TEXT("Native injected BoundGraph registrations: %d"), Registrations));
		}
	}
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("tmp/TROOT-UAI-208-graph-common-foundation/behavior-tree-common-r1/injected-workspace"), Suffix));
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), ParentPackage);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	FString Params, Error, Payload;
	bool bReadOnly = false;
	HjsonWorkspace::FWorkspaceError ExportError;
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)) return false;
	const bool bExported = DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
		ParentPackage, BehaviorTreeType, Params, Payload, bReadOnly, ExportError);
	if (!TestTrue(TEXT("ordinary export supports native repeated subtree references"), bExported))
	{ AddError(ExportError.GetCode() + TEXT(":") + ExportError.GetJsonPointer()); return false; }
	TArray<FString> GraphFiles;
	IFileManager::Get().FindFiles(GraphFiles, *(Workspace / TEXT("graphs/*.hjson")), true, false);
	FString GraphFile, Text;
	TSharedPtr<FJsonObject> Editable;
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> InjectedGraphs;
	for (const FString& File : GraphFiles)
	{
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		const FString CandidateFile = Workspace / TEXT("graphs") / File;
		FString CandidateText;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(CandidateText, *CandidateFile)
			|| !HjsonWorkspace::TryParseHjsonDocument(CandidateText, CandidateFile, Resources, Parsed, ExportError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, ExportError)) return false;
		if (Json->AsObject()->HasField(TEXT("owner")))
		{
			InjectedGraphs.Emplace(CandidateFile, Json->AsObject());
			continue;
		}
		GraphFile = CandidateFile;
		Text = CandidateText;
		Editable = Json->AsObject();
	}
	if (!TestTrue(TEXT("repeated source copies receive separate public rows"), Editable.IsValid()
		&& Editable->GetObjectField(TEXT("nodes"))->Values.Num() == 7)) return false;
	TestFalse(TEXT("source Guid is not part of public authoring"), Text.Contains(Tasks[0]->Decorators[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TSharedPtr<FJsonObject> InjectedRow, WaitRow;
	int32 ReadOnlyRows = 0;
	for (const auto& Pair : Editable->GetObjectField(TEXT("nodes"))->Values)
	{
		const auto Row = Pair.Value->AsObject();
		bool bInjected = false, bReadOnlyRow = false;
		if (Row->TryGetBoolField(TEXT("injected"), bInjected) && bInjected)
		{
			TestTrue(TEXT("injected row exposes the native read-only boundary"), Row->TryGetBoolField(TEXT("read_only"), bReadOnlyRow) && bReadOnlyRow);
			InjectedRow = Row;
			++ReadOnlyRows;
		}
		if (Pair.Key == TEXT("Wait1")) WaitRow = Row;
	}
	if (!TestTrue(TEXT("both injected rows and editable task are present"), ReadOnlyRows == 2 && WaitRow.IsValid())) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), ParentPackage);
	auto ImportEdited = [&]() {
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
		FString Revision, Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, ParentPackage, Resources,
			Resolved, Revision, Authorization, ExportError)) return false;
		Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		return CommandContract::Json::TryCanonicalize(Import, Params, Error)
			&& DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				ParentPackage, BehaviorTreeType, Params, Payload, bReadOnly, ExportError);
	};
	TSharedPtr<FJsonValue> Before, After;
	if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, Before, Error)) return false;
	FString BeforeJson, AfterJson;
	CommandContract::Json::TryCanonicalize(Before->AsObject(), BeforeJson, Error);
	if (bCompositeSource)
	{
		if (!TestEqual(TEXT("two source references expose separate owned graphs"), InjectedGraphs.Num(), 2)) return false;
		for (const auto& Owned : InjectedGraphs)
		{
			bool bGraphReadOnly = false;
			TestTrue(TEXT("injected owned graph exposes its read-only boundary"),
				Owned.Value->TryGetBoolField(TEXT("read_only"), bGraphReadOnly) && bGraphReadOnly);
			TestEqual(TEXT("each injected graph retains the whole native expression"), Owned.Value->GetObjectField(TEXT("nodes"))->Values.Num(), 4);
			for (const auto& Pair : Owned.Value->GetObjectField(TEXT("nodes"))->Values)
			{
				const auto Row = Pair.Value->AsObject();
				bool bRowReadOnly = false;
				TestTrue(TEXT("derived inner node is read-only"), Row->TryGetBoolField(TEXT("read_only"), bRowReadOnly) && bRowReadOnly);
				FString Logic;
				if (!Row->TryGetStringField(TEXT("logic"), Logic) || Logic != TEXT("And")) continue;
				const auto InnerPosition = Row->GetObjectField(TEXT("pos"));
				const double X = InnerPosition->GetNumberField(TEXT("x"));
				InnerPosition->SetNumberField(TEXT("x"), X + 40);
				FString Modified;
				if (!CommandContract::Json::TryCanonicalize(Owned.Value, Modified, Error)
					|| !FFileHelper::SaveStringToFile(Modified, *Owned.Key, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
				TestFalse(TEXT("independent injected inner-graph editing is rejected"), ImportEdited());
				TestEqual(TEXT("inner-graph edit reports the read-only boundary"), ExportError.GetCode(), FString(TEXT("behavior_tree_semantic_injected_read_only")));
				InnerPosition->SetNumberField(TEXT("x"), X);
				if (!CommandContract::Json::TryCanonicalize(Owned.Value, Modified, Error)
					|| !FFileHelper::SaveStringToFile(Modified, *Owned.Key, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		}
		using namespace QueryOwners::AiBehavior;
		FAiBehaviorCandidate Candidate;
		FAiBehaviorCandidateQueryError CandidateError;
		const auto* Composite = CastChecked<UBehaviorTreeGraphNode_CompositeDecorator>(Tasks[0]->Decorators[0]);
		TestFalse(TEXT("native candidate entry refuses creation in the derived graph"),
			TryResolveAiBehaviorCandidate(*Parent, TEXT("And"), FString(), Candidate, CandidateError, {}, {nullptr, FString(), Composite->BoundGraph}));
		TestEqual(TEXT("candidate refusal reports a read-only graph"), CandidateError.Code, FString(TEXT("ai_behavior_candidate_graph_read_only")));
		if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, After, Error)
			|| !CommandContract::Json::TryCanonicalize(After->AsObject(), AfterJson, Error)) return false;
		TestEqual(TEXT("inner-graph refusals preserve all native state"), AfterJson, BeforeJson);
		if (HasAnyErrors()) return false;
	}
	const auto Position = InjectedRow->GetObjectField(TEXT("pos"));
	const double OriginalX = Position->GetNumberField(TEXT("x"));
	Position->SetNumberField(TEXT("x"), OriginalX + 40);
	if (!CommandContract::Json::TryCanonicalize(Editable, Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TestFalse(TEXT("ordinary import rejects independent injected-node editing"), ImportEdited());
	TestEqual(TEXT("read-only rejection has a specific error"), ExportError.GetCode(), FString(TEXT("behavior_tree_semantic_injected_read_only")));
	if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, After, Error)) return false;
	CommandContract::Json::TryCanonicalize(After->AsObject(), AfterJson, Error);
	TestEqual(TEXT("rejection preserves the full native graph"), AfterJson, BeforeJson);
	Position->SetNumberField(TEXT("x"), OriginalX);
	WaitRow->GetObjectField(TEXT("pos"))->SetNumberField(TEXT("x"), 704);
	if (!CommandContract::Json::TryCanonicalize(Editable, Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const TWeakObjectPtr<UBehaviorTree> Previous = Parent;
	Parent = nullptr;
	Tasks.Reset();
	if (!TestTrue(TEXT("ordinary task edit retains native injected copies"), ImportEdited()))
	{ AddError(ExportError.GetCode() + TEXT(":") + ExportError.GetJsonPointer()); return false; }
	TestFalse(TEXT("successful task edit releases the previous asset"), Previous.IsValid());
	Parent = LoadObject<UBehaviorTree>(nullptr, *ParentPath);
	if (!Parent || !UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, After, Error)) return false;
	for (const auto& Pair : Before->AsObject()->GetObjectField(TEXT("nodes"))->Values)
		if (Pair.Value->AsObject()->GetStringField(TEXT("payload_class")) == UBTTask_Wait::StaticClass()->GetPathName())
			Pair.Value->AsObject()->GetObjectField(TEXT("pos"))->SetNumberField(TEXT("x"), 704);
	CommandContract::Json::TryCanonicalize(Before->AsObject(), BeforeJson, Error);
	CommandContract::Json::TryCanonicalize(After->AsObject(), AfterJson, Error);
	TestEqual(TEXT("clean persisted graph changes only the selected task position"), AfterJson, BeforeJson);
	TestFalse(TEXT("independent reloaded parent is clean"), Parent->GetOutermost()->IsDirty());
	const FString RebindWorkspace = Workspace + TEXT("-rebind");
	Export->SetStringField(TEXT("workspace_path"), RebindWorkspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			ParentPackage, BehaviorTreeType, Params, Payload, bReadOnly, ExportError)) return false;
	TArray<FString> RebindFiles;
	IFileManager::Get().FindFilesRecursive(RebindFiles, *RebindWorkspace, TEXT("*.hjson"), true, false);
	bool bAddedTask = false, bClearedSource = false;
	for (const FString& File : RebindFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		if (!Text.Contains(TEXT("nodes:")) && (!Text.Contains(TEXT("BehaviorAsset")) || bClearedSource)) continue;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, ExportError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, ExportError)) return false;
		const auto Root = Json->AsObject();
		bool bChanged = false;
		if (Root->HasField(TEXT("nodes")) && !Root->HasField(TEXT("owner")))
		{
			const auto NewTask = MakeShared<FJsonObject>();
			const auto Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), TEXT("Run Behavior"));
			NewTask->SetObjectField(TEXT("candidate"), Candidate);
			Root->GetObjectField(TEXT("nodes"))->SetObjectField(TEXT("AddedRun"), NewTask);
			bAddedTask = bChanged = true;
		}
		else if (Root->HasField(TEXT("categories")))
		{
			for (const auto& Category : Root->GetObjectField(TEXT("categories"))->Values)
			{
				const TSharedPtr<FJsonObject>* Property = nullptr;
				if (!Category.Value->AsObject()->TryGetObjectField(TEXT("BehaviorAsset"), Property)) continue;
				FString Value;
				if (!(*Property)->TryGetStringField(TEXT("value"), Value) || Value != ChildPath) continue;
				(*Property)->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
				bClearedSource = bChanged = true;
				break;
			}
		}
		if (bChanged && (!CommandContract::Json::TryCanonicalize(Root, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	}
	if (!TestTrue(TEXT("ordinary workspace expresses new RunBehavior and one source clear"), bAddedTask && bClearedSource)) return false;
	Source->SetStringField(TEXT("path"), RebindWorkspace);
	const TWeakObjectPtr<UBehaviorTree> BeforeRebind = Parent;
	Parent = nullptr;
	if (!TestTrue(TEXT("native subtree candidate and source refresh share the ordinary transaction"), ImportEdited()))
	{ AddError(ExportError.GetCode() + TEXT(":") + ExportError.GetJsonPointer()); return false; }
	TestFalse(TEXT("source rebind releases the previous tree"), BeforeRebind.IsValid());
	Parent = LoadObject<UBehaviorTree>(nullptr, *ParentPath);
	if (!Parent || !Parent->BTGraph) return false;
	int32 SubtreeTasks = 0, BoundSources = 0, InjectedCount = 0;
	for (UEdGraphNode* Node : Parent->BTGraph->Nodes)
	{
		if (Node->IsA<UBehaviorTreeGraphNode_Root>())
			TestFalse(TEXT("root retains its independent native deletion restriction"), Node->CanUserDeleteNode());
		const auto* SubtreeTask = Cast<UBehaviorTreeGraphNode_SubtreeTask>(Node);
		if (!SubtreeTask) continue;
		++SubtreeTasks;
		const auto* Task = Cast<UBTTask_RunBehavior>(SubtreeTask->NodeInstance);
		if (!TestNotNull(TEXT("native subtree carrier owns RunBehavior payload"), Task)) return false;
		BoundSources += Task->GetSubtreeAsset() != nullptr;
		for (const auto& Decorator : SubtreeTask->Decorators) InjectedCount += Decorator && Decorator->bInjectedNode;
	}
	TestEqual(TEXT("new candidate persists as native SubtreeTask"), SubtreeTasks, 3);
	TestEqual(TEXT("exactly one native subtree source remains bound"), BoundSources, 1);
	TestEqual(TEXT("source clear removes only its derived preview"), InjectedCount, 1);
	TestFalse(TEXT("source refresh persists cleanly"), Parent->GetOutermost()->IsDirty());
	if (HasAnyErrors()) return false;
	const FString ParentFilename = FPackageName::LongPackageNameToFilename(ParentPackage, FPackageName::GetAssetPackageExtension());
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *ParentFilename)) return false;
	FString RemovedParent;
	UeAgentBehaviorTreeAuthoring::FGraphCapture BeforeDelete;
	if (!UeAgentBehaviorTreeAuthoring::CaptureGraph(*Parent, BeforeDelete, Error)) return false;
	for (const auto& Node : BeforeDelete.Nodes)
	{
		const auto* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node.NativeNode);
		const auto* Task = BehaviorNode ? Cast<UBTTask_RunBehavior>(BehaviorNode->NodeInstance) : nullptr;
		if (Task && Task->GetSubtreeAsset()) RemovedParent = Node.Id;
		if (BehaviorNode && Cast<UBTTask_Wait>(BehaviorNode->NodeInstance))
		{
			auto* Dirty = const_cast<UEdGraphNode*>(Node.NativeNode);
			Dirty->Modify();
			Dirty->NodePosX += 96;
		}
	}
	if (!TestFalse(TEXT("late deletion selects the remaining bound parent"), RemovedParent.IsEmpty())) return false;
	Parent->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const auto DesiredNodes = Desired->AsObject()->GetObjectField(TEXT("nodes"));
	for (const auto& Node : BeforeDelete.Nodes)
		if (Node.Id == RemovedParent || Node.ParentId == RemovedParent) DesiredNodes->RemoveField(Node.Id);
	for (const auto& Owned : BeforeDelete.OwnedGraphs)
		if (!DesiredNodes->HasField(Owned.OwnerId)) Desired->AsObject()->GetObjectField(TEXT("owned_graphs"))->RemoveField(Owned.Id);
	const TWeakObjectPtr<UBehaviorTree> OriginalParent = Parent;
	TWeakObjectPtr<UBehaviorTree> RejectedParent;
	UeAgentReflectionAuthoring::FAssetApplyRequest Request;
	Request.TypeId = TEXT("behavior_tree");
	Request.AssetPath = ParentPackage;
	Request.bSave = true;
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetObjectField(TEXT("graph"), Desired->AsObject());
	bool bLateObserved = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		UBehaviorTree* Actual = LoadObject<UBehaviorTree>(nullptr, *ParentPath);
		RejectedParent = Actual;
		UeAgentBehaviorTreeAuthoring::FGraphCapture Observed;
		TArray<uint8> ActualDisk;
		FString CaptureError;
		bLateObserved = !OriginalParent.IsValid(true, true) && Actual && !Actual->GetOutermost()->IsDirty()
			&& UeAgentBehaviorTreeAuthoring::CaptureGraph(*Actual, Observed, CaptureError)
			&& Observed.Nodes.Num() == BeforeDelete.Nodes.Num() - 2
			&& Observed.OwnedGraphs.IsEmpty()
			&& !Observed.Nodes.ContainsByPredicate([&](const auto& Node) { return Node.Id == RemovedParent || Node.ParentId == RemovedParent; })
			&& FFileHelper::LoadFileToArray(ActualDisk, *ParentFilename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_injected_parent_late_rejected");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Parent = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult Result;
	TestFalse(TEXT("late parent deletion rejects after persistence"),
		UeAgentReflectionAuthoring::ApplyAsset(Request, Result, TEXT("injected-parent-late-") + Suffix));
	if (!TestTrue(TEXT("parent and preview deletion reached clean persisted state before rejection"), bLateObserved)
		|| !TestEqual(TEXT("the original late rejection is retained"), Result.Diagnostic.Code, FString(TEXT("t208_injected_parent_late_rejected")))
		|| !TestTrue(TEXT("injected graph checkpoint recovery is confirmed"), Result.bRollbackAttempted && Result.bRollbackSucceeded))
	{ AddError(Result.Phase + TEXT(":") + Result.Diagnostic.PublicMessage); return false; }
	TestFalse(TEXT("rejected deletion state leaves memory"), RejectedParent.IsValid(true, true));
	Parent = LoadObject<UBehaviorTree>(nullptr, *ParentPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("full dirty graph including source binding and injected identity is restored"), Parent
		&& UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Parent, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error)
		&& RestoredText == DirtyBefore && Parent->GetOutermost()->IsDirty());
	TestTrue(TEXT("original disk is restored independently of dirty injected graph"),
		FFileHelper::LoadFileToArray(DiskAfter, *ParentFilename) && DiskAfter == DiskBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeOwnedGraphWorkspaceTest,
	"UAI.Root.T85.BehaviorTree.OwnedGraphWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeOwnedGraphWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("BT_OwnedGraph_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString(FixtureFolder) / Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	if (!SaveBehaviorTree(*Package, *Tree, Filename)) return false;
	FString Params, Error, Payload;
	if (!TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, TEXT("Selector"), TEXT("Wait"), Params, Error,
		{}, FString(), TEXT("Composite Decorator")))
	{ AddError(Error); return false; }
	Tree = nullptr;
	Package = nullptr;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, BehaviorTreeType, Params, Payload, bReadOnly, WorkspaceError))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	auto* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	if (!TestNotNull(TEXT("ordinary parent graph reloads"), Graph)) return false;
	UBehaviorTreeGraphNode_Task* Task = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
		if (auto* Candidate = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Candidate;
	if (!TestNotNull(TEXT("native task owns the composite decorator"), Task)) return false;
	auto* Composite = Task->Decorators.Num() == 1 ? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Task->Decorators[0]) : nullptr;
	if (!TestTrue(TEXT("ordinary creation persists the composite and its native default graph"), Composite
		&& Composite->BoundGraph && Composite->BoundGraph->Nodes.Num() == 1 && !Tree->GetOutermost()->IsDirty())) return false;
	Composite->CompositeName = TEXT("GateLogic");
	auto* First = NewObject<UBTDecorator_Cooldown>(Tree, NAME_None, RF_Transactional);
	auto* Second = NewObject<UBTDecorator_Cooldown>(Tree, NAME_None, RF_Transactional);
	First->CoolDownTime = 4.25f;
	Second->CoolDownTime = 9.5f;
	// This native reconstruction API only arranges the fixture. Production capture
	// must read these editor nodes, not regenerate them from the compiled logic.
	const TArray<UBTDecorator*> Instances{First, Second};
	const TArray<FBTDecoratorLogic> Operations{
		FBTDecoratorLogic(EBTDecoratorLogic::And, 2),
		FBTDecoratorLogic(EBTDecoratorLogic::Test, 0), FBTDecoratorLogic(EBTDecoratorLogic::Test, 1)};
	if (!TestEqual(TEXT("native fixture creates the complete expression"), Composite->SpawnMissingNodes(Instances, Operations, 0), 3)) return false;
	Graph->UpdateAsset();
	Tree->MarkPackageDirty();
	TArray<UBTDecorator*> CompiledInstances;
	TArray<FBTDecoratorLogic> CompiledOperations;
	Composite->CollectDecoratorData(CompiledInstances, CompiledOperations);
	if (!TestTrue(TEXT("fixture has the intended native And expression"), CompiledInstances == Instances
		&& CompiledOperations.Num() == 3 && CompiledOperations[0].Operation == EBTDecoratorLogic::And
		&& CompiledOperations[0].Number == 2)) return false;
	UeAgentBehaviorTreeAuthoring::FGraphCapture Capture;
	if (!TestTrue(TEXT("capture traverses the actual owned editor graph"),
		UeAgentBehaviorTreeAuthoring::CaptureGraph(*Tree, Capture, Error))) { AddError(Error); return false; }
	if (!TestTrue(TEXT("capture separates main and owned node scopes"), Capture.Nodes.Num() == 4
		&& Capture.OwnedGraphs.Num() == 1 && Capture.OwnedGraphs[0].Nodes.Num() == 4
		&& Capture.OwnedGraphs[0].Connections.Num() == 3
		&& Capture.OwnedGraphs[0].NativeGraph == Composite->BoundGraph)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("tmp/TROOT-UAI-208-graph-common-foundation/behavior-tree-common-r1/owned-graph-workspace"), Name));
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !TestTrue(TEXT("public export consumes the independent owned graph"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
				PackageName, BehaviorTreeType, Params, Payload, bReadOnly, WorkspaceError)))
	{ AddError(Error + WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	TestTrue(TEXT("export leaves the native dirty tree in place"), bReadOnly && Tree->GetOutermost()->IsDirty()
		&& Composite->BoundGraph == Capture.OwnedGraphs[0].NativeGraph);
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	int32 GraphCount = 0;
	bool bFoundOwner = false;
	FString Visible;
	FString OwnedGraphFile, PropertyFile;
	TSharedPtr<FJsonObject> OwnedEditable;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		Visible += Text;
		if (Text.Contains(TEXT("CoolDownTime")) && Text.Contains(TEXT("4.25"))) PropertyFile = File;
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, WorkspaceError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, WorkspaceError)) return false;
		if (!Json.IsValid() || Json->Type != EJson::Object || !Json->AsObject()->HasField(TEXT("nodes"))) continue;
		++GraphCount;
		const auto Row = Json->AsObject();
		TestEqual(TEXT("each graph has its own node set"), Row->GetObjectField(TEXT("nodes"))->Values.Num(), 4);
		FString Owner;
		if (Row->TryGetStringField(TEXT("owner"), Owner))
		{
			OwnedGraphFile = File;
			OwnedEditable = Row;
			bFoundOwner = Owner.StartsWith(TEXT("item:"))
				&& !HjsonWorkspace::MakeReadableSemanticAlias(Owner.Mid(5)).IsEmpty();
			bool bFoundAnd = false;
			for (const auto& Pair : Row->GetObjectField(TEXT("nodes"))->Values)
			{
				FString Logic;
				bFoundAnd |= Pair.Value->AsObject()->TryGetStringField(TEXT("logic"), Logic) && Logic == TEXT("And");
			}
			TestTrue(TEXT("owned graph includes the native And operation"), bFoundAnd);
		}
	}
	TestEqual(TEXT("public export contains two distinct graph files"), GraphCount, 2);
	TestTrue(TEXT("owned graph uses the common readable owner reference"), bFoundOwner);
	TestTrue(TEXT("both native test properties reach the common property workspace"),
		Visible.Contains(TEXT("CoolDownTime")) && Visible.Contains(TEXT("4.25")) && Visible.Contains(TEXT("9.5")));
	for (const auto& Node : Capture.OwnedGraphs[0].Nodes)
		TestFalse(TEXT("owned native identities remain hidden"), Visible.Contains(Node.NativeNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	if (HasAnyErrors() || !OwnedEditable || PropertyFile.IsEmpty()) return false;
	FString ChangedId;
	int32 ExpectedX = 0;
	for (const auto& Node : Capture.OwnedGraphs[0].Nodes)
	{
		FString Logic;
		if (Node.State->TryGetStringField(TEXT("logic"), Logic) && Logic == TEXT("And"))
		{
			ChangedId = Node.Id;
			ExpectedX = Node.NativeNode->NodePosX + 96;
		}
	}
	for (const auto& Pair : OwnedEditable->GetObjectField(TEXT("nodes"))->Values)
	{
		FString Logic;
		if (Pair.Value->AsObject()->TryGetStringField(TEXT("logic"), Logic) && Logic == TEXT("And"))
		{
			Pair.Value->AsObject()->SetStringField(TEXT("logic"), TEXT("Or"));
			Pair.Value->AsObject()->GetObjectField(TEXT("pos"))->SetNumberField(TEXT("x"), ExpectedX);
		}
	}
	FString Text;
	if (!CommandContract::Json::TryCanonicalize(OwnedEditable, Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *OwnedGraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !FFileHelper::LoadFileToString(Text, *PropertyFile)) return false;
	if (!TestEqual(TEXT("one public property value changes"), Text.ReplaceInline(TEXT("4.25"), TEXT("6.75")), 1)
		|| !FFileHelper::SaveStringToFile(Text, *PropertyFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
		Resolved, Revision, Authorization, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName);
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UBehaviorTree> Before(Tree);
	Tree = nullptr;
	Graph = nullptr;
	Task = nullptr;
	Composite = nullptr;
	First = nullptr;
	Second = nullptr;
	CompiledInstances.Reset();
	if (!TestTrue(TEXT("ordinary owned graph edits reach persistent readback"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, WorkspaceError)))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	TestFalse(TEXT("the original tree leaves memory before independent readback"), Before.IsValid());
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	UeAgentBehaviorTreeAuthoring::FGraphCapture After;
	if (!TestTrue(TEXT("independently reloaded owned graph is clean and captured"), Tree && !Tree->GetOutermost()->IsDirty()
		&& UeAgentBehaviorTreeAuthoring::CaptureGraph(*Tree, After, Error))) { AddError(Error); return false; }
	if (!TestTrue(TEXT("both graph scopes and ownership survive"), After.Nodes.Num() == Capture.Nodes.Num()
		&& After.Connections.Num() == Capture.Connections.Num() && After.OwnedGraphs.Num() == 1
		&& After.OwnedGraphs[0].Id == Capture.OwnedGraphs[0].Id && After.OwnedGraphs[0].Nodes.Num() == 4
		&& After.OwnedGraphs[0].Connections.Num() == 3)) return false;
	for (const auto& Original : Capture.Nodes)
	{
		const auto* Current = After.Nodes.FindByPredicate([&](const auto& Node) { return Node.Id == Original.Id; });
		FString BeforeState, AfterState;
		if (!TestTrue(TEXT("every main graph node retains its captured state"), Current && Current->ParentId == Original.ParentId
			&& CommandContract::Json::TryCanonicalize(Original.State, BeforeState, Error)
			&& CommandContract::Json::TryCanonicalize(Current->State, AfterState, Error) && BeforeState == AfterState)) return false;
	}
	for (int32 Index = 0; Index < Capture.Connections.Num(); ++Index)
		TestEqual(TEXT("main graph topology is unchanged"), After.Connections[Index].Id, Capture.Connections[Index].Id);
	const auto* Changed = After.OwnedGraphs[0].Nodes.FindByPredicate([&](const auto& Node) { return Node.Id == ChangedId; });
	TestTrue(TEXT("the edited native logical node keeps identity and new position"), Changed
		&& Changed->NativeNode->NodePosX == ExpectedX && Changed->State->GetStringField(TEXT("logic")) == TEXT("Or"));
	Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
	for (UEdGraphNode* Node : Graph->Nodes)
		if (auto* Match = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Match;
	Composite = Task && Task->Decorators.Num() == 1 ? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Task->Decorators[0]) : nullptr;
	if (!TestNotNull(TEXT("reloaded task retains its native composite owner"), Composite)) return false;
	CompiledOperations.Reset();
	Composite->CollectDecoratorData(CompiledInstances, CompiledOperations);
	if (!TestTrue(TEXT("native compiled expression reads the new operation"), CompiledInstances.Num() == 2
		&& CompiledOperations.Num() == 3 && CompiledOperations[0].Operation == EBTDecoratorLogic::Or)) return false;
	TestEqual(TEXT("the intended test payload changed"), CastChecked<UBTDecorator_Cooldown>(CompiledInstances[0])->CoolDownTime.GetValue(
		static_cast<const UBlackboardComponent*>(nullptr)), 6.75f);
	TestEqual(TEXT("the adjacent test payload remains unchanged"), CastChecked<UBTDecorator_Cooldown>(CompiledInstances[1])->CoolDownTime.GetValue(
		static_cast<const UBlackboardComponent*>(nullptr)), 9.5f);
	if (HasAnyErrors()) return false;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	auto* DirtyDecorator = CastChecked<UBTDecorator_Cooldown>(CompiledInstances[0]);
	DirtyDecorator->Modify();
	DirtyDecorator->CoolDownTime = 8.25f;
	UEdGraphNode* DirtyLogic = const_cast<UEdGraphNode*>(Changed->NativeNode);
	DirtyLogic->Modify();
	DirtyLogic->NodePosX += 128;
	Tree->MarkPackageDirty();
	FString RemovedId;
	for (const auto& Node : After.OwnedGraphs[0].Nodes)
	{
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Node.NativeNode->GetClass(), TEXT("NodeInstance"));
		if (Property && Property->GetObjectPropertyValue_InContainer(Node.NativeNode) == DirtyDecorator) RemovedId = Node.Id;
	}
	if (!TestFalse(TEXT("the removed inner node is bound to the real native payload"), RemovedId.IsEmpty())) return false;
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Tree, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const auto DesiredGraph = Desired->AsObject()->GetObjectField(TEXT("owned_graphs"))->GetObjectField(After.OwnedGraphs[0].Id);
	DesiredGraph->GetObjectField(TEXT("nodes"))->RemoveField(RemovedId);
	TArray<TSharedPtr<FJsonValue>> KeptConnections;
	for (const auto& Edge : DesiredGraph->GetArrayField(TEXT("connections")))
		if (Edge->AsObject()->GetObjectField(TEXT("from"))->GetStringField(TEXT("node")) != RemovedId
			&& Edge->AsObject()->GetObjectField(TEXT("to"))->GetStringField(TEXT("node")) != RemovedId) KeptConnections.Add(Edge);
	DesiredGraph->SetArrayField(TEXT("connections"), KeptConnections);
	TWeakObjectPtr<UBehaviorTree> OriginalTree(Tree), RejectedTree;
	TWeakObjectPtr<UBTDecorator_Cooldown> OriginalDecorator(DirtyDecorator);
	UeAgentReflectionAuthoring::FAssetApplyRequest Request;
	Request.TypeId = TEXT("behavior_tree");
	Request.AssetPath = PackageName;
	Request.bSave = true;
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetObjectField(TEXT("graph"), Desired->AsObject());
	bool bLateObserved = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		UBehaviorTree* Actual = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
		RejectedTree = Actual;
		UeAgentBehaviorTreeAuthoring::FGraphCapture ActualCapture;
		TArray<uint8> ActualDisk;
		FString CaptureError;
		bLateObserved = !OriginalTree.IsValid(true, true) && !OriginalDecorator.IsValid(true, true)
			&& Actual && !Actual->GetOutermost()->IsDirty() && Actual->RootNode
			&& Actual->RootNode->Children.Num() == 1 && Actual->RootNode->Children[0].Decorators.Num() == 1
			&& UeAgentBehaviorTreeAuthoring::CaptureGraph(*Actual, ActualCapture, CaptureError)
			&& ActualCapture.OwnedGraphs.Num() == 1 && ActualCapture.OwnedGraphs[0].Nodes.Num() == 3
			&& ActualCapture.OwnedGraphs[0].Connections.Num() == 2
			&& !ActualCapture.OwnedGraphs[0].Nodes.ContainsByPredicate([&](const auto& Node) { return Node.Id == RemovedId; })
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_owned_graph_late_rejected");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Tree = nullptr;
	Graph = nullptr;
	Task = nullptr;
	Composite = nullptr;
	DirtyDecorator = nullptr;
	DirtyLogic = nullptr;
	CompiledInstances.Reset();
	UeAgentReflectionAuthoring::FAssetApplyResult ApplyResult;
	TestFalse(TEXT("late owned graph deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(Request, ApplyResult, TEXT("owned-graph-late-") + Name));
	if (!TestTrue(TEXT("owned node deletion was persisted before rejection"), bLateObserved)
		|| !TestEqual(TEXT("late rejection retains its own diagnostic"), ApplyResult.Diagnostic.Code, FString(TEXT("t208_owned_graph_late_rejected")))
		|| !TestTrue(TEXT("owned graph checkpoint recovery is confirmed"), ApplyResult.bRollbackAttempted && ApplyResult.bRollbackSucceeded))
	{ AddError(ApplyResult.Phase + TEXT(":") + ApplyResult.Diagnostic.PublicMessage); return false; }
	TestFalse(TEXT("rejected owned graph state leaves memory during recovery"), RejectedTree.IsValid(true, true));
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("complete dirty graph ownership, identity, logic, layout and payload are restored"), Tree
		&& UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error) && RestoredText == DirtyBefore
		&& Tree->GetOutermost()->IsDirty());
	TestTrue(TEXT("the old disk is restored independently of dirty graph memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	if (HasAnyErrors()) return false;
	// New nodes use the native inner schema catalog, then the ordinary workspace ingress.
	const FString CreateWorkspace = Workspace + TEXT("-create");
	Export->SetStringField(TEXT("workspace_path"), CreateWorkspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, WorkspaceError)) return false;
	UeAgentBehaviorTreeAuthoring::FGraphCapture BeforeCreate;
	if (!UeAgentBehaviorTreeAuthoring::CaptureGraph(*Tree, BeforeCreate, Error)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateContext Context;
	Context.Graph = const_cast<UEdGraph*>(BeforeCreate.OwnedGraphs[0].NativeGraph);
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult CandidateResult;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("NOT"), CandidateResult, CandidateError, FString(), Context))
	{ AddError(CandidateError.Code); return false; }
	const auto* LogicCandidate = CandidateResult.Candidates.FindByPredicate([](const auto& Candidate) { return Candidate.LogicMode == TEXT("Not"); });
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult TestCandidates;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("Cooldown"), TestCandidates, CandidateError, FString(), Context))
	{ AddError(CandidateError.Code); return false; }
	const auto* TestCandidate = TestCandidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.PayloadType == UBTDecorator_Cooldown::StaticClass()->GetPathName(); });
	if (!TestTrue(TEXT("native inner menu exposes logic and test candidates"), LogicCandidate && TestCandidate)) return false;
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *CreateWorkspace, TEXT("*.hjson"), true, false);
	bool bEdited = false;
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, WorkspaceError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, WorkspaceError)) return false;
		if (Json->Type != EJson::Object || !Json->AsObject()->HasField(TEXT("owner")) || !Json->AsObject()->HasField(TEXT("nodes"))) continue;
		using namespace OperationOwners::AiBehavior;
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService> Service;
		if (!TryCreateProductionAiBehaviorCandidateCommandService(Service, Error)) return false;
		const auto Query = MakeShared<FJsonObject>();
		Query->SetStringField(TEXT("target"), ObjectPath);
		Query->SetStringField(TEXT("graph_owner"), Json->AsObject()->GetStringField(TEXT("owner")));
		FAiBehaviorCandidateCommandRequest QueryRequest;
		QueryRequest.CommandId = TEXT("ai.behavior.candidates");
		FAiBehaviorCandidateCommandResponse QueryResponse;
		FAiBehaviorCandidateCommandError QueryError;
		const auto Rows = Json->AsObject()->GetObjectField(TEXT("nodes"));
		FString ExpandedAlias;
		for (const auto& Existing : Rows->Values)
		{
			FString Logic;
			if (Existing.Value->AsObject()->TryGetStringField(TEXT("logic"), Logic) && Logic == TEXT("Or"))
			{
				ExpandedAlias = Existing.Key;
				const auto Groups = Existing.Value->AsObject()->GetObjectField(TEXT("pin_groups"));
				if (!TestEqual(TEXT("ordinary graph exports the actual variable input count"), Groups->GetIntegerField(TEXT("inputs")), 2)) return false;
				Groups->SetNumberField(TEXT("inputs"), 3);
			}
		}
		if (!TestFalse(TEXT("the expanded logic is identified from the public graph"), ExpandedAlias.IsEmpty())) return false;
		for (const auto* Candidate : {LogicCandidate, TestCandidate})
		{
			Query->SetStringField(TEXT("query"), Candidate->Name);
			if (!CommandContract::Json::TryCanonicalize(Query, QueryRequest.CanonicalParamsJson, Error)
				|| !Service->Execute(QueryRequest, QueryResponse, QueryError))
			{ AddError(QueryError.Code); return false; }
			TSharedPtr<FJsonObject> Result;
			if (!CommandContract::Json::TryParseObject(QueryResponse.PayloadJson, Result, Error)) return false;
			const auto& Names = Result->GetArrayField(TEXT("names"));
			if (!TestTrue(TEXT("exported graph owner resolves through the public candidate service"), QueryResponse.bReadOnly
				&& Names.ContainsByPredicate([&](const auto& Value) { return Value->AsString() == Candidate->Name; }))) return false;
			const auto Row = MakeShared<FJsonObject>();
			const auto Selector = MakeShared<FJsonObject>();
			Selector->SetStringField(TEXT("name"), Candidate->Name);
			Row->SetObjectField(TEXT("candidate"), Selector);
			const auto Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), 640);
			Position->SetNumberField(TEXT("y"), Candidate == LogicCandidate ? 320 : 480);
			Row->SetObjectField(TEXT("pos"), Position);
			Rows->SetObjectField(Candidate == LogicCandidate ? TEXT("NewLogic") : TEXT("NewTest"), Row);
		}
		const auto NewConnection = MakeShared<FJsonObject>();
		NewConnection->SetStringField(TEXT("from"), TEXT("NewTest.Out"));
		NewConnection->SetStringField(TEXT("to"), ExpandedAlias + TEXT(".In3"));
		auto Connections = Json->AsObject()->GetArrayField(TEXT("connections"));
		Connections.Add(MakeShared<FJsonValueObject>(NewConnection));
		Json->AsObject()->SetArrayField(TEXT("connections"), Connections);
		Query->SetStringField(TEXT("graph_owner"), TEXT("item:missing"));
		CommandContract::Json::TryCanonicalize(Query, QueryRequest.CanonicalParamsJson, Error);
		TestFalse(TEXT("unresolved owned graph context is rejected"), Service->Execute(QueryRequest, QueryResponse, QueryError));
		TestEqual(TEXT("graph context has a specific public error"), QueryError.Code, FString(TEXT("behavior_tree_candidate_graph_owner_unresolved")));
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bEdited = true;
	}
	if (!TestTrue(TEXT("new inner nodes are authored in the exported owned graph"), bEdited)) return false;
	Source->SetStringField(TEXT("path"), CreateWorkspace);
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
		Resolved, Revision, Authorization, WorkspaceError)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	Before = Tree;
	Tree = nullptr;
	Context.Graph = nullptr;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, BehaviorTreeType, Params, Payload, bReadOnly, WorkspaceError))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	TestFalse(TEXT("named inner creation releases the previous tree"), Before.IsValid());
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	UeAgentBehaviorTreeAuthoring::FGraphCapture Created;
	if (!Tree || !UeAgentBehaviorTreeAuthoring::CaptureGraph(*Tree, Created, Error)) return false;
	TestTrue(TEXT("both named inner nodes persist in the existing owned graph"), !Tree->GetOutermost()->IsDirty()
		&& Created.OwnedGraphs.Num() == 1 && Created.OwnedGraphs[0].Nodes.Num() == 6
		&& Created.OwnedGraphs[0].Connections.Num() == BeforeCreate.OwnedGraphs[0].Connections.Num() + 1);
	const auto* Expanded = Created.OwnedGraphs[0].Nodes.FindByPredicate([&](const auto& Node) { return Node.Id == ChangedId; });
	int32 InputCount = 0;
	if (Expanded) for (const UEdGraphPin* Pin : Expanded->NativeNode->Pins) InputCount += Pin && Pin->Direction == EGPD_Input;
	TestEqual(TEXT("ordinary variable group growth persists three native inputs"), InputCount, 3);
	int32 Added = 0;
	for (const auto& Node : Created.OwnedGraphs[0].Nodes)
	{
		if (BeforeCreate.OwnedGraphs[0].Nodes.ContainsByPredicate([&](const auto& Old) { return Old.Id == Node.Id; })) continue;
		++Added;
		FString Logic;
		const bool bLogic = Node.State->TryGetStringField(TEXT("logic"), Logic);
		TestTrue(TEXT("new node uses the requested native logic or payload and position"), Node.NativeNode->NodePosX == 640
			&& (bLogic ? Logic == TEXT("Not") && Node.NativeNode->NodePosY == 320
				: Node.PayloadClass == UBTDecorator_Cooldown::StaticClass()->GetPathName() && Node.NativeNode->NodePosY == 480));
	}
	TestEqual(TEXT("exactly two native inner nodes were added"), Added, 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeAttachmentWorkspaceTest,
	"UAI.Root.T85.BehaviorTree.AttachmentWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeAttachmentWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("BT_Attachment_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString(FixtureFolder) / Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	if (!TestTrue(TEXT("attachment fixture starts saved"), SaveBehaviorTree(*Package, *Tree, Filename))) return false;
	FString Params, Error, Payload;
	if (!TestTrue(TEXT("attachment fixture uses ordinary graph creation"),
		TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, TEXT("Selector"), TEXT("Wait"), Params, Error))) return false;
	Tree = nullptr;
	Package = nullptr;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("attachment fixture creates its native graph"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError))) return false;
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	UBehaviorTreeGraph* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	if (!TestNotNull(TEXT("attachment fixture graph reloads"), Graph)) return false;
	UBehaviorTreeGraphNode_Task* Task = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
		if (auto* Match = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Match;
	if (!TestNotNull(TEXT("attachment owner is the existing Wait task"), Task)) return false;
	for (float Duration : {4.0f, 9.0f, 15.0f, 22.0f})
	{
		auto* Node = NewObject<UBehaviorTreeGraphNode_Decorator>(Graph);
		auto* Decorator = NewObject<UBTDecorator_Cooldown>(Tree, NAME_None, RF_Transactional);
		Decorator->CoolDownTime = Duration;
		Node->NodeInstance = Decorator;
		Task->AddSubNode(Node, Graph);
	}
	// Native UI storage and execution order need not agree.
	Task->SubNodes.Swap(0, 1);
	Graph->UpdateAsset();
	if (!TestEqual(TEXT("native decorator ownership is established"), Task->Decorators.Num(), 4)) return false;
	TestEqual(TEXT("compiled relation contains all native decorators"), Tree->RootNode->Children[0].Decorators.Num(), 4);
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("tmp/TROOT-UAI-208-graph-common-foundation/behavior-tree-common-r1/attachment-workspace"), Name));
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)) return false;
	HjsonWorkspace::FWorkspaceError ExportError;
	if (!TestTrue(TEXT("ordinary export handles native decorator attachments"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ExportError)))
	{
		AddError(ExportError.GetCode() + TEXT(":") + ExportError.GetJsonPointer());
		return false;
	}
	TestTrue(TEXT("attachment export preserves dirty native state"), bReadOnly && Tree->GetOutermost()->IsDirty());
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Workspace / TEXT("graphs/*.hjson")), true, false);
	FString GraphFile;
	TSharedPtr<FJsonObject> Editable;
	for (const FString& File : Files)
	{
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		GraphFile = Workspace / TEXT("graphs") / File;
		FString Text;
		auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(Text, *GraphFile)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, GraphFile, Resources, Parsed, ExportError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, ExportError)) return false;
		if (Json->AsObject()->HasField(TEXT("nodes"))) { Editable = Json->AsObject(); break; }
	}
	if (!TestTrue(TEXT("the public graph includes all four owned attachments"), Editable.IsValid()
		&& Editable->GetObjectField(TEXT("nodes"))->Values.Num() == 7)) return false;
	const auto Rows = Editable->GetObjectField(TEXT("nodes"));
	FString RemovedAlias;
	int32 Attachments = 0;
	for (const auto& Pair : Rows->Values)
	{
		const auto Spec = Pair.Value->AsObject();
		FString Parent;
		if (!Spec->TryGetStringField(TEXT("parent"), Parent)) continue;
		++Attachments;
		TestTrue(TEXT("parent uses an existing public graph alias"), Rows->HasField(Parent)
			&& Rows->GetObjectField(Parent)->GetStringField(TEXT("name")) == TEXT("Wait"));
		TestEqual(TEXT("attachment classification is visible"), Spec->GetStringField(TEXT("attachment_kind")), FString(TEXT("decorator")));
		const int32 Order = Spec->GetIntegerField(TEXT("order"));
		if (Order == 3) RemovedAlias = Pair.Key;
		else Spec->SetNumberField(TEXT("order"), 2 - Order);
	}
	if (!TestEqual(TEXT("every attachment has an editable parent relationship"), Attachments, 4)
		|| !TestFalse(TEXT("the removed attachment is identified through its public order"), RemovedAlias.IsEmpty())) return false;
	Rows->RemoveField(RemovedAlias);
	FString Text;
	if (!CommandContract::Json::TryCanonicalize(Editable, Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
		Resolved, Revision, Authorization, ImportError)) { AddError(ImportError.GetCode()); return false; }
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName);
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UBehaviorTree> Before(Tree);
	Tree = nullptr;
	Graph = nullptr;
	Task = nullptr;
	if (!TestTrue(TEXT("ordinary attachment deletion and reorder reach persistent readback"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError)))
	{ AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetJsonPointer()); return false; }
	TestFalse(TEXT("attachment edits release the original tree before readback"), Before.IsValid());
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
	if (!TestTrue(TEXT("attachment result is independently reloaded and clean"), Graph && !Tree->GetOutermost()->IsDirty())) return false;
	for (UEdGraphNode* Node : Graph->Nodes)
		if (auto* Match = Cast<UBehaviorTreeGraphNode_Task>(Node)) Task = Match;
	if (!TestTrue(TEXT("removed decorator exits both native collections"), Task && Task->Decorators.Num() == 3
		&& Task->SubNodes.Num() == 3 && Tree->RootNode->Children[0].Decorators.Num() == 3)) return false;
	const float ExpectedDecorators[] = {15.0f, 9.0f, 4.0f};
	const float ExpectedSubNodes[] = {9.0f, 4.0f, 15.0f};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestEqual(TEXT("native execution order follows the edited typed order"),
			CastChecked<UBTDecorator_Cooldown>(Task->Decorators[Index]->NodeInstance)->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), ExpectedDecorators[Index]);
		TestEqual(TEXT("compiled child uses that same decorator order"),
			CastChecked<UBTDecorator_Cooldown>(Tree->RootNode->Children[0].Decorators[Index])->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), ExpectedDecorators[Index]);
		TestEqual(TEXT("generic ownership storage retains its independent order"),
			CastChecked<UBTDecorator_Cooldown>(Task->SubNodes[Index]->NodeInstance)->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), ExpectedSubNodes[Index]);
	}
	// Exercise the same provider's late checkpoint with a dirty state distinct from disk.
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	auto* ChangedDecorator = CastChecked<UBTDecorator_Cooldown>(Task->Decorators[0]->NodeInstance);
	ChangedDecorator->Modify();
	ChangedDecorator->CoolDownTime = 18.5f;
	Task->Modify();
	Task->SubNodes.Swap(1, 2);
	Tree->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Tree, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const FString RemovedId = TEXT("node:") + Task->Decorators[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	Desired->AsObject()->GetObjectField(TEXT("nodes"))->RemoveField(RemovedId);
	TWeakObjectPtr<UBehaviorTree> OriginalTree(Tree), RejectedTree;
	TWeakObjectPtr<UBTDecorator_Cooldown> OriginalDecorator(ChangedDecorator);
	UeAgentReflectionAuthoring::FAssetApplyRequest Request;
	Request.TypeId = TEXT("behavior_tree");
	Request.AssetPath = PackageName;
	Request.bSave = true;
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetObjectField(TEXT("graph"), Desired->AsObject());
	bool bLateObserved = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		UBehaviorTree* Actual = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
		RejectedTree = Actual;
		TArray<uint8> ActualDisk;
		bLateObserved = !OriginalTree.IsValid(true, true) && !OriginalDecorator.IsValid(true, true)
			&& Actual && !Actual->GetOutermost()->IsDirty() && Actual->RootNode
			&& Actual->RootNode->Children.Num() == 1 && Actual->RootNode->Children[0].Decorators.Num() == 2
			&& CastChecked<UBTDecorator_Cooldown>(Actual->RootNode->Children[0].Decorators[0])->CoolDownTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)) == 9.0f
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_attachment_late_rejected");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Tree = nullptr;
	Graph = nullptr;
	Task = nullptr;
	ChangedDecorator = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult ApplyResult;
	TestFalse(TEXT("late attachment deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(Request, ApplyResult, TEXT("attachment-late-") + Name));
	if (!TestTrue(TEXT("the attachment deletion was persisted before the rejection"), bLateObserved)
		|| !TestEqual(TEXT("late rejection retains its diagnostic"), ApplyResult.Diagnostic.Code, FString(TEXT("t208_attachment_late_rejected")))
		|| !TestTrue(TEXT("attachment checkpoint recovery is confirmed"), ApplyResult.bRollbackAttempted && ApplyResult.bRollbackSucceeded))
	{ AddError(ApplyResult.Phase + TEXT(":") + ApplyResult.Diagnostic.PublicMessage); return false; }
	TestFalse(TEXT("rejected attachment state exits during recovery"), RejectedTree.IsValid(true, true));
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("dirty attachment payload, identity and both collection orders are restored"), Tree
		&& UeAgentBehaviorTreeAuthoring::ReadGraphDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error) && RestoredText == DirtyBefore
		&& Tree->GetOutermost()->IsDirty());
	TestTrue(TEXT("recovery restores original disk independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeRootTargetRejectedTest,
	"UAI.Root.T85.BehaviorTree.RootTargetRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeRootTargetRejectedTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;

	DeleteOwnedFixtureAssets();
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction/BT_InvalidRootTarget_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		DeleteOwnedFixtureAssets();
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UBehaviorTree* const BehaviorTree = Package
		? NewObject<UBehaviorTree>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
			TEXT("an invalid-topology fixture is created"), BehaviorTree))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(BehaviorTree);
	if (!TestTrue(
			TEXT("the invalid-topology fixture is initially saved"),
			SaveBehaviorTree(*Package, *BehaviorTree, Filename)))
	{
		return false;
	}

	FAiBehaviorCandidateQueryResult CandidateResult;
	FAiBehaviorCandidateQueryError CandidateError;
	if (!TestTrue(
			TEXT("the native Wait action is available for the negative case"),
			TryQueryAiBehaviorCandidates(
				*BehaviorTree,
				TEXT("Wait"),
				CandidateResult,
				CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const TArray<FAiBehaviorCandidate> WaitCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTTask_Wait::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the negative case resolves one native Wait action"),
		WaitCandidates.Num(), 1))
	{
		return false;
	}

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(
			TEXT("the root-to-task HJSON canonicalizes before topology preflight"),
			TryBuildResolvedBehaviorTreeImportParams(
				PackageName,
				CandidateResult.TargetRevision,
				FString(),
				WaitCandidates[0].Name,
				true,
				CanonicalParams,
				ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	TestFalse(
		TEXT("root-to-task is rejected before the BehaviorTree writer"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			BehaviorTreeType,
			CanonicalParams,
			ImportPayloadJson,
			bReadOnly,
			ImportError));
	TestEqual(
		TEXT("the invalid root target has a typed error"),
		ImportError.GetCode(),
		FString(TEXT("graph_compound_behavior_tree_root_target_invalid")));
	TestFalse(TEXT("the rejected topology leaves the package clean"),
		Package->IsDirty());
#if WITH_EDITORONLY_DATA
	TestNull(TEXT("the rejected topology creates no BehaviorTree graph"),
		BehaviorTree->BTGraph);
#endif

	DeleteAssetIfPresent(ObjectPath);
	DeleteOwnedFixtureAssets();
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the negative fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeIncrementalSharedLifecycleTest,
	"UAI.Root.T85.BehaviorTree.IncrementalSharedLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeIncrementalSharedLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction/BT_Incremental_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	if (!TestTrue(TEXT("incremental baseline is saved"), SaveBehaviorTree(*Package, *Tree, Filename))) return false;
	TWeakObjectPtr<UBehaviorTree> Before(Tree);
	FAiBehaviorCandidateQueryResult Candidates;
	FAiBehaviorCandidateQueryError QueryError;
	if (!TestTrue(TEXT("incremental candidates use the native query"),
		TryQueryAiBehaviorCandidates(*Tree, FString(), Candidates, QueryError))) return false;
	FString Selector, Wait;
	for (const auto& Candidate : Candidates.Candidates)
	{
		if (!Candidate.bSupported) continue;
		if (Candidate.PayloadType == UBTComposite_Selector::StaticClass()->GetPathName()) Selector = Candidate.Name;
		if (Candidate.PayloadType == UBTTask_Wait::StaticClass()->GetPathName()) Wait = Candidate.Name;
	}
	if (!TestFalse(TEXT("both native creation names exist"), Selector.IsEmpty() || Wait.IsEmpty())) return false;
	FString Params, Error;
	if (!TestTrue(TEXT("existing incremental input resolves"), TryBuildResolvedBehaviorTreeImportParams(
		PackageName, Candidates.TargetRevision, Selector, Wait, false, Params, Error,
		{{TEXT("class"), UBTTask_Wait::StaticClass()->GetName()}})))
	{
		AddError(Error);
		return false;
	}
	Tree = nullptr;
	Package = nullptr;
	FString Payload;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("incremental input uses the shared saved lifecycle"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetSourceUnit());
		return false;
	}
	TestFalse(TEXT("shared persistent readback releases the original asset"), Before.IsValid());
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the saved tree reloads"), Tree)) return false;
	const auto* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
	if (!TestNotNull(TEXT("the saved native graph exists"), Graph)) return false;
	TestEqual(TEXT("the increment creates exactly root, composite and task"), Graph->Nodes.Num(), 3);
	TestTrue(TEXT("the persisted execution graph is Selector to Wait"),
		Tree->RootNode && Tree->RootNode->GetClass() == UBTComposite_Selector::StaticClass()
			&& Tree->RootNode->Children.Num() == 1
			&& Cast<UBTTask_Wait>(Tree->RootNode->Children[0].ChildTask));
	TestFalse(TEXT("the reloaded incremental result is clean"), Tree->GetOutermost()->IsDirty());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeLateRecoveryTest,
	"UAI.Root.T85.BehaviorTree.LateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeLateRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;
	using namespace UeAgentBehaviorTreeAuthoring;
	using namespace UeAgentReflectionAuthoring;
	const FString Name = TEXT("BT_LateRecovery_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = FString(FixtureFolder) / Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	if (!TestTrue(TEXT("recovery fixture begins as a saved empty tree"), SaveBehaviorTree(*Package, *Tree, Filename))) return false;
	FAiBehaviorCandidateQueryResult Candidates;
	FAiBehaviorCandidateQueryError QueryError;
	if (!TestTrue(TEXT("recovery fixture queries native creation names"),
		TryQueryAiBehaviorCandidates(*Tree, FString(), Candidates, QueryError))) return false;
	FString SelectorName, WaitName;
	for (const auto& Candidate : Candidates.Candidates)
	{
		if (!Candidate.bSupported) continue;
		if (Candidate.PayloadType == UBTComposite_Selector::StaticClass()->GetPathName()) SelectorName = Candidate.Name;
		if (Candidate.PayloadType == UBTTask_Wait::StaticClass()->GetPathName()) WaitName = Candidate.Name;
	}
	if (!TestFalse(TEXT("recovery fixture has both candidate names"), SelectorName.IsEmpty() || WaitName.IsEmpty())) return false;
	FString Params, Error, Payload;
	if (!TestTrue(TEXT("recovery fixture uses ordinary HJSON creation"),
		TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, SelectorName, WaitName, Params, Error))) return false;
	Tree = nullptr;
	Package = nullptr;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("ordinary fixture creation completes"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, BehaviorTreeType, Params, Payload, bReadOnly, ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetSourceUnit());
		return false;
	}
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("recovery fixture reloads"), Tree)) return false;
	FGraphCapture Captured;
	if (!CaptureGraph(*Tree, Captured, Error)) return false;
	UBehaviorTreeGraphNode_Task* TaskNode = nullptr;
	FString TaskId;
	for (const auto& Node : Captured.Nodes)
		if (Node.PayloadClass == UBTTask_Wait::StaticClass()->GetPathName())
		{
			TaskNode = const_cast<UBehaviorTreeGraphNode_Task*>(Cast<UBehaviorTreeGraphNode_Task>(Node.NativeNode));
			TaskId = Node.Id;
		}
	if (!TestNotNull(TEXT("recovery fixture contains Wait"), TaskNode)) return false;
	UBTTask_Wait* Wait = CastChecked<UBTTask_Wait>(TaskNode->NodeInstance);
	Wait->WaitTime = 5.0f;
	if (!SaveBehaviorTree(*Tree->GetOutermost(), *Tree, Filename)) return false;
	TArray<uint8> DiskBefore;
	if (!TestTrue(TEXT("original disk bytes captured"), FFileHelper::LoadFileToArray(DiskBefore, *Filename))) return false;
	Wait->Modify();
	Wait->WaitTime = 7.25f;
	TaskNode->Modify();
	TaskNode->NodePosX = 704;
	Tree->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	if (!ReadGraphDocument(*Tree, Desired, Error)) return false;
	FString DirtyBefore;
	if (!CommandContract::Json::TryCanonicalize(Desired->AsObject().ToSharedRef(), DirtyBefore, Error)) return false;
	const TWeakObjectPtr<UBehaviorTree> OriginalTree(Tree);
	const TWeakObjectPtr<UBTTask_Wait> OriginalTask(Wait);
	Desired->AsObject()->GetObjectField(TEXT("nodes"))->RemoveField(TaskId);
	TArray<TSharedPtr<FJsonValue>> Connections = Desired->AsObject()->GetArrayField(TEXT("connections"));
	Connections.RemoveAll([&](const TSharedPtr<FJsonValue>& Edge) {
		return Edge->AsObject()->GetObjectField(TEXT("from"))->GetStringField(TEXT("node")) == TaskId
			|| Edge->AsObject()->GetObjectField(TEXT("to"))->GetStringField(TEXT("node")) == TaskId;
	});
	Desired->AsObject()->SetArrayField(TEXT("connections"), MoveTemp(Connections));
	FAssetApplyRequest Request;
	Request.TypeId = TEXT("behavior_tree");
	Request.AssetPath = PackageName;
	Request.bSave = true;
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetObjectField(TEXT("graph"), Desired->AsObject());
	bool bLateObserved = false;
	TWeakObjectPtr<UBehaviorTree> RejectedTree;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		UBehaviorTree* Actual = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
		RejectedTree = Actual;
		FGraphCapture ActualGraph;
		TArray<uint8> ActualDisk;
		FString CaptureError;
		bLateObserved = !OriginalTree.IsValid(true, true) && !OriginalTask.IsValid(true, true)
			&& Actual && !Actual->GetOutermost()->IsDirty()
			&& CaptureGraph(*Actual, ActualGraph, CaptureError) && ActualGraph.Nodes.Num() == 2
			&& Actual->RootNode && Actual->RootNode->Children.IsEmpty()
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t85_behavior_tree_late_rejected");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Tree = nullptr;
	TaskNode = nullptr;
	Wait = nullptr;
	Captured = {};
	FAssetApplyResult Result;
	TestFalse(TEXT("late graph deletion is rejected before commit"), ApplyAsset(Request, Result, TEXT("behavior-tree-late-") + Name));
	TestEqual(TEXT("late failure diagnostic is preserved"), Result.Diagnostic.Code, FString(TEXT("t85_behavior_tree_late_rejected")));
	if (!TestTrue(TEXT("deletion was independently persisted before rejection"), bLateObserved)
		|| !TestTrue(TEXT("checkpoint rollback is confirmed"), Result.bRollbackAttempted && Result.bRollbackSucceeded))
	{
		AddError(Result.Phase + TEXT(":") + Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.PublicMessage);
		return false;
	}
	TestFalse(TEXT("the rejected persisted object exits during recovery"), RejectedTree.IsValid(true, true));
	Tree = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("memory checkpoint reloads"), Tree)) return false;
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TestTrue(TEXT("full dirty native graph including removed task and properties is restored"),
		ReadGraphDocument(*Tree, Restored, Error)
			&& CommandContract::Json::TryCanonicalize(Restored->AsObject().ToSharedRef(), RestoredText, Error)
			&& RestoredText == DirtyBefore);
	TestTrue(TEXT("restored memory remains dirty"), Tree->GetOutermost()->IsDirty());
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("original disk restored independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BehaviorTreeNativeActionVerticalTest,
	"UAI.Root.T85.BehaviorTree.NativeActionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BehaviorTreeNativeActionVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::AiBehavior;

	DeleteOwnedFixtureAssets();
	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85BehaviorTreeNativeAction/BT_NativeAction_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		DeleteOwnedFixtureAssets();
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UBehaviorTree* const BehaviorTree = Package
		? NewObject<UBehaviorTree>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(
			TEXT("a persistent Behavior Tree fixture is created"), BehaviorTree))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(BehaviorTree);
	if (!TestTrue(
			TEXT("the initial Behavior Tree fixture is saved"),
			SaveBehaviorTree(*Package, *BehaviorTree, Filename)))
	{
		return false;
	}

	FAiBehaviorCandidateQueryResult CandidateResult;
	FAiBehaviorCandidateQueryError CandidateError;
	if (!TestTrue(
			TEXT("search enumerates the native Behavior Tree blank menu"),
			TryQueryAiBehaviorCandidates(
				*BehaviorTree,
				FString(),
				CandidateResult,
				CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const TArray<FAiBehaviorCandidate> WaitCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTTask_Wait::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the exact native Wait action is unique"),
		WaitCandidates.Num(), 1))
	{
		return false;
	}
	const TArray<FAiBehaviorCandidate> SelectorCandidates =
		CandidateResult.Candidates.FilterByPredicate(
			[](const FAiBehaviorCandidate& Candidate)
			{
				return Candidate.bSupported
					&& Candidate.PayloadType
						== UBTComposite_Selector::StaticClass()->GetPathName();
			});
	if (!TestEqual(TEXT("the exact native Selector action is unique"),
		SelectorCandidates.Num(), 1))
	{
		return false;
	}
	const FAiBehaviorCandidate& WaitCandidate = WaitCandidates[0];
	const FAiBehaviorCandidate& SelectorCandidate = SelectorCandidates[0];
	TestFalse(TEXT("the public Behavior Tree selectors have names"),
		WaitCandidate.Name.IsEmpty()
			|| SelectorCandidate.Name.IsEmpty());
	TestFalse(TEXT("native Behavior Tree search leaves the package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	TArray<uint8> DiskBeforeInvalid;
	if (!TestTrue(TEXT("capture baseline bytes before invalid select"),
		FFileHelper::LoadFileToArray(DiskBeforeInvalid, *Filename))) return false;
	if (!TestTrue(TEXT("ordinary HJSON carries a semantic select"),
		TryBuildOrdinaryBehaviorTreeCreateParams(PackageName, SelectorCandidate.Name, WaitCandidate.Name,
			CanonicalParams, ParamsError, {{TEXT("class"), TEXT("Missing task class")}})))
	{
		AddError(ParamsError);
		return false;
	}
	FString RejectedPayload;
	bool bRejectedReadOnly = true;
	HjsonWorkspace::FWorkspaceError SelectionError;
	TestFalse(TEXT("invalid select is rejected before creation"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, BehaviorTreeType, CanonicalParams, RejectedPayload, bRejectedReadOnly, SelectionError));
	if (!TestTrue(TEXT("the shared candidate selector error survives ordinary import"),
		SelectionError.GetCode() == TEXT("candidate_selector_invalid")))
	{
		AddError(SelectionError.GetCode() + TEXT(":") + SelectionError.GetSourceUnit());
	}
	TestTrue(TEXT("invalid select returns the original readable name for correction"),
		SelectionError.GetJsonPointer().Contains(WaitCandidate.Name));
	TArray<uint8> DiskAfterInvalid;
	TestTrue(TEXT("invalid select preserves disk bytes"),
		FFileHelper::LoadFileToArray(DiskAfterInvalid, *Filename) && DiskAfterInvalid == DiskBeforeInvalid);
	TestTrue(TEXT("invalid select leaves the empty tree clean"), !BehaviorTree->RootNode
		&& (!BehaviorTree->BTGraph || BehaviorTree->BTGraph->Nodes.IsEmpty()) && !Package->IsDirty());
	if (!TestTrue(
			TEXT("name and native class selection canonicalize"),
			TryBuildOrdinaryBehaviorTreeCreateParams(
				PackageName,
				SelectorCandidate.Name,
				WaitCandidate.Name,
				CanonicalParams,
				ParamsError, {{TEXT("class"), UBTTask_Wait::StaticClass()->GetName()}})))
	{
		AddError(ParamsError);
		return false;
	}

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(
			TEXT("name and select HJSON replay the native Selector and Wait actions"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"),
				PackageName,
				BehaviorTreeType,
				CanonicalParams,
				ImportPayloadJson,
				bReadOnly,
				ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":")
			+ ImportError.GetSourceUnit() + TEXT(":")
			+ ImportError.GetJsonPointer());
		return false;
	}
	TestFalse(TEXT("the Behavior Tree import is a mutation"), bReadOnly);
	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(
			TEXT("the Behavior Tree import payload is canonical JSON"),
			CommandContract::Json::TryParseObject(
				ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	UBehaviorTree* const Reloaded =
		LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	if (!TestNotNull(
			TEXT("the persisted Behavior Tree independently reloads"), Reloaded))
	{
		return false;
	}
	UBehaviorTreeGraph* const Graph =
		Cast<UBehaviorTreeGraph>(Reloaded->BTGraph);
	if (!TestNotNull(TEXT("the reloaded Behavior Tree owns its graph"), Graph))
	{
		return false;
	}
	FGuid TaskGuid;
	FGuid CompositeGuid;
	UBehaviorTreeGraphNode_Task* TaskNode = nullptr;
	UBehaviorTreeGraphNode_Composite* CompositeNode = nullptr;
	UBehaviorTreeGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UBehaviorTreeGraphNode_Task* Task = Cast<UBehaviorTreeGraphNode_Task>(Node))
		{
			TaskNode = Task;
			TaskGuid = Node->NodeGuid;
		}
		if (UBehaviorTreeGraphNode_Composite* Composite = Cast<UBehaviorTreeGraphNode_Composite>(Node))
		{
			CompositeNode = Composite;
			CompositeGuid = Node->NodeGuid;
		}
		if (UBehaviorTreeGraphNode_Root* const CandidateRoot =
			Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			RootNode = CandidateRoot;
		}
	}
	if (!TestNotNull(TEXT("ordinary creation produces a native composite"),
			CompositeNode)
		|| !TestNotNull(TEXT("ordinary creation produces a native task"), TaskNode)
		|| !TestNotNull(TEXT("the root node survives reload"), RootNode))
	{
		return false;
	}
	TestTrue(TEXT("the task owns an exact Wait runtime instance"),
		TaskNode->NodeInstance
			&& TaskNode->NodeInstance->GetClass() == UBTTask_Wait::StaticClass());
	TestTrue(TEXT("the composite owns an exact Selector runtime instance"),
		CompositeNode->NodeInstance
			&& CompositeNode->NodeInstance->GetClass()
				== UBTComposite_Selector::StaticClass());
	TestTrue(TEXT("the persisted Selector retains pins"),
		!CompositeNode->Pins.IsEmpty());
	TestTrue(TEXT("the persisted Wait task retains pins"),
		!TaskNode->Pins.IsEmpty());
	TestEqual(TEXT("the persisted composite retains X placement"),
		CompositeNode->NodePosX, 384);
	TestEqual(TEXT("the persisted composite retains Y placement"),
		CompositeNode->NodePosY, 192);
	TestEqual(TEXT("the persisted task retains X placement"),
		TaskNode->NodePosX, 640);
	TestEqual(TEXT("the persisted task retains Y placement"),
		TaskNode->NodePosY, 192);
	UEdGraphPin* const RootOutput = RootNode->GetOutputPin();
	UEdGraphPin* const CompositeInput = CompositeNode->GetInputPin();
	UEdGraphPin* const CompositeOutput = CompositeNode->GetOutputPin();
	UEdGraphPin* const TaskInput = TaskNode->GetInputPin();
	TestTrue(TEXT("the root-to-composite relation survives reload"),
		RootOutput && CompositeInput
			&& RootOutput->LinkedTo.Contains(CompositeInput)
			&& CompositeInput->LinkedTo.Contains(RootOutput));
	TestTrue(TEXT("the composite-to-task relation survives reload"),
		CompositeOutput && TaskInput
			&& CompositeOutput->LinkedTo.Contains(TaskInput)
			&& TaskInput->LinkedTo.Contains(CompositeOutput));

	UeAgentBehaviorTreeAuthoring::FGraphCapture Captured;
	FString CaptureError;
	if (!TestTrue(TEXT("the domain capture reads the native graph without reconstruction"),
		UeAgentBehaviorTreeAuthoring::CaptureGraph(*Reloaded, Captured, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	TestEqual(TEXT("capture retains root, Selector and Wait"), Captured.Nodes.Num(), 3);
	TestEqual(TEXT("capture retains both native connections"), Captured.Connections.Num(), 2);
	const auto* CapturedTask = Captured.Nodes.FindByPredicate([&](const auto& Node) {
		return Node.Id == FString(TEXT("node:")) + TaskGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	});
	if (!TestNotNull(TEXT("capture retains the existing Wait identity"), CapturedTask)) return false;
	TestEqual(TEXT("capture reads the actual Wait position"),
		CapturedTask->State->GetObjectField(TEXT("pos"))->GetIntegerField(TEXT("x")), TaskNode->NodePosX);
	TArray<TSharedPtr<FJsonValue>> PropertyDescriptors;
	if (!TestTrue(TEXT("payload properties use the common property document"),
		UeAgentDetailsAuthoringService::DecodePropertiesDocument(
			CapturedTask->State->GetObjectField(TEXT("properties")), PropertyDescriptors, CaptureError)))
	{
		AddError(CaptureError);
		return false;
	}
	TestTrue(TEXT("the common capture includes Wait's editable duration"),
		PropertyDescriptors.ContainsByPredicate([](const auto& Value) {
			FString Path;
			return Value.IsValid() && Value->Type == EJson::Object
				&& Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)
				&& Path == TEXT("WaitTime");
		}));

	// A non-default native value makes loss in compact property projection observable.
	UBTTask_Wait* Wait = CastChecked<UBTTask_Wait>(TaskNode->NodeInstance);
	Wait->Modify();
	Wait->WaitTime = 7.25f;
	Reloaded->MarkPackageDirty();
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectDir(), TEXT("tmp/TROOT-UAI-208-graph-common-foundation/behavior-tree-common-r1"),
		TEXT("native-workspace-") + AssetName));
	const auto ExportParams = MakeShared<FJsonObject>();
	ExportParams->SetStringField(TEXT("target"), PackageName);
	ExportParams->SetStringField(TEXT("workspace_path"), WorkspacePath);
	FString ExportParamsText;
	FString ExportPayload;
	HjsonWorkspace::FWorkspaceError ExportError;
	if (!CommandContract::Json::TryCanonicalize(ExportParams, ExportParamsText, CaptureError)
		|| !TestTrue(TEXT("ordinary workspace export consumes the real BehaviorTree graph"),
			DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
				PackageName, BehaviorTreeType, ExportParamsText, ExportPayload, bReadOnly, ExportError)))
	{
		AddError(CaptureError + ExportError.GetCode() + TEXT(":") + ExportError.GetJsonPointer());
		return false;
	}
	TestTrue(TEXT("ordinary export remains read-only and preserves dirty state"), bReadOnly && Reloaded->GetOutermost()->IsDirty());
	TArray<FString> ExportFiles;
	IFileManager::Get().FindFilesRecursive(ExportFiles, *WorkspacePath, TEXT("*.hjson"), true, false);
	FString VisibleText;
	FString WaitPropertiesFile;
	FString GraphFile;
	for (const FString& File : ExportFiles)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text;
		if (!TestTrue(TEXT("the public authoring file is readable"), FFileHelper::LoadFileToString(Text, *File))) return false;
		VisibleText += Text;
		if (Text.Contains(TEXT("WaitTime")) && Text.Contains(TEXT("7.25"))) WaitPropertiesFile = File;
		if (Text.Contains(TEXT("connections:")) && Text.Contains(TEXT("Selector1"))) GraphFile = File;
	}
	TestTrue(TEXT("ordinary graph export contains Selector and Wait"), VisibleText.Contains(TEXT("Selector")) && VisibleText.Contains(TEXT("Wait")));
	TestTrue(TEXT("ordinary export retains the non-default editable payload value"), VisibleText.Contains(TEXT("7.25")));
	TestFalse(TEXT("ordinary authoring never requires the native task GUID"), VisibleText.Contains(TaskGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestFalse(TEXT("ordinary authoring never requires the native composite GUID"), VisibleText.Contains(CompositeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	if (!TestFalse(TEXT("the Wait property edit has a public file"), WaitPropertiesFile.IsEmpty())
		|| !TestFalse(TEXT("the placement edit has a public graph file"), GraphFile.IsEmpty())) return false;
	FString PropertyText, GraphText;
	if (!FFileHelper::LoadFileToString(PropertyText, *WaitPropertiesFile)
		|| !FFileHelper::LoadFileToString(GraphText, *GraphFile)) return false;
	if (!TestEqual(TEXT("the fixture changes one duration value"), PropertyText.ReplaceInline(TEXT("7.25"), TEXT("8.5")), 1)
		|| !TestEqual(TEXT("the fixture changes one task position"), GraphText.ReplaceInline(TEXT("x: 640.0"), TEXT("x: 704.0")), 1)) return false;
	if (!FFileHelper::SaveStringToFile(PropertyText, *WaitPropertiesFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !FFileHelper::SaveStringToFile(GraphText, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto WorkspaceSource = MakeShared<FJsonObject>();
	WorkspaceSource->SetStringField(TEXT("kind"), TEXT("directory"));
	WorkspaceSource->SetStringField(TEXT("path"), WorkspacePath);
	auto ImportResources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedWorkspace;
	FString WorkspaceRevision, WorkspaceAuthorization;
	if (!TestTrue(TEXT("ordinary directory import recovers its own baseline"),
		HjsonWorkspace::TryRecoverContentGraphImportMachineContext(WorkspaceSource, PackageName, ImportResources,
			ResolvedWorkspace, WorkspaceRevision, WorkspaceAuthorization, ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetJsonPointer());
		return false;
	}
	const auto WorkspaceParams = MakeShared<FJsonObject>();
	WorkspaceParams->SetStringField(TEXT("target"), PackageName);
	WorkspaceParams->SetStringField(TEXT("expected_revision"), WorkspaceRevision);
	WorkspaceParams->SetObjectField(TEXT("source"), ResolvedWorkspace->MakeNormalizedSourceObject());
	if (!WorkspaceAuthorization.IsEmpty()) WorkspaceParams->SetStringField(TEXT("authorization_id"), WorkspaceAuthorization);
	FString WorkspaceParamsText;
	if (!CommandContract::Json::TryCanonicalize(WorkspaceParams, WorkspaceParamsText, CaptureError)) return false;
	if (!TestTrue(TEXT("ordinary workspace updates existing payload and placement through the shared transaction"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName,
			BehaviorTreeType, WorkspaceParamsText, ImportPayloadJson, bReadOnly, ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetJsonPointer());
		return false;
	}
	UBehaviorTree* Updated = LoadObject<UBehaviorTree>(nullptr, *ObjectPath);
	UBehaviorTreeGraph* UpdatedGraph = Updated ? Cast<UBehaviorTreeGraph>(Updated->BTGraph) : nullptr;
	if (!TestNotNull(TEXT("ordinary import retains the native graph after its lifecycle"), UpdatedGraph)) return false;
	UBehaviorTreeGraphNode_Task* UpdatedTask = nullptr;
	for (UEdGraphNode* Node : UpdatedGraph->Nodes)
		if (Node && Node->NodeGuid == TaskGuid) UpdatedTask = Cast<UBehaviorTreeGraphNode_Task>(Node);
	if (!TestNotNull(TEXT("ordinary import preserves the existing task identity"), UpdatedTask)) return false;
	const UBTTask_Wait* UpdatedWait = Cast<UBTTask_Wait>(UpdatedTask->NodeInstance);
	if (!TestNotNull(TEXT("ordinary import preserves the Wait payload type"), UpdatedWait)) return false;
	TestEqual(TEXT("ordinary import applies the non-default Wait duration"),
		UpdatedWait->WaitTime.GetValue(static_cast<const UBlackboardComponent*>(nullptr)), 8.5f);
	TestEqual(TEXT("ordinary import applies task placement"), UpdatedTask->NodePosX, 704);
	TestEqual(TEXT("ordinary import preserves all three nodes"), UpdatedGraph->Nodes.Num(), 3);
	TestTrue(TEXT("ordinary import retains the compiled Selector-to-Wait execution relation"),
		Updated->RootNode && Updated->RootNode->GetClass() == UBTComposite_Selector::StaticClass()
			&& Updated->RootNode->Children.Num() == 1 && Updated->RootNode->Children[0].ChildTask == UpdatedWait);
	TestFalse(TEXT("ordinary saved import leaves the package clean"), Updated->GetOutermost()->IsDirty());

	DeleteAssetIfPresent(ObjectPath);
	DeleteOwnedFixtureAssets();
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Behavior Tree fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
