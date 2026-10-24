#include "Misc/AutomationTest.h"

#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintFieldNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/MemberReference.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_CustomEvent.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "TestSupport/UeAgentFixtureDirectoryLifecycle.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintDelegateBindingTest,
	"UAI.Root.T208.Blueprint.DelegateBindingWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

static bool RunDelegateBinding(FAutomationTestBase& Test, const bool bBackground)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208BlueprintDelegate/BP_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString WorkspaceRoot = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/Delegate") / Token;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
	{
		Test.AddError(Error);
		return false;
	}
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-blueprint-delegate"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	ON_SCOPE_EXIT
	{
		if (FindObject<UBlueprint>(nullptr, *ObjectPath) || FPackageName::DoesPackageExist(PackageName))
		{
			auto Args = MakeShared<FJsonObject>();
			Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
			Args->SetBoolField(TEXT("recursive"), false);
			for (bool bConfirm : {false, true})
			{
				Args->SetBoolField(TEXT("confirm"), bConfirm);
				const auto Result = Submit(TEXT("asset.delete"), Args);
				if (!Test.TestTrue(TEXT("owned delegate fixture publicly deletes"), Result.IsOk()))
				{
					Test.AddError(Result.GetErrorJson());
					break;
				}
			}
		}
		Test.TestFalse(TEXT("owned delegate package absent after cleanup"), FPackageName::DoesPackageExist(PackageName));
		FString CleanupError;
		if (!TestSupport::TryRemoveEmptyFixturePackageDirectory(TEXT("/Game/UAI/Transient"),
			TEXT("/Game/UAI/Transient/T208BlueprintDelegate"), CleanupError)) Test.AddError(CleanupError);
		IFileManager::Get().DeleteDirectory(*WorkspaceRoot, false, true);
	};

	// Initialization is separate from the candidate-based ordinary imports below.
	const FString Seed = FString::Printf(TEXT(R"JSON({
 "schema":"uai.blueprint.edit/1", "root":{"type":"/Script/Engine.Blueprint", "document":{
 "operation":"create", "target":{"asset_path":"%s","asset_kind":"actor_blueprint"},
 "provider_extension":{"parent_class":"/Script/Engine.Actor",
 "members":{"delegates":{"StatusChanged":{"operation":"create","signature":{"inputs":[{"name":"Enabled","type":{"pin_category":"bool"}},{"name":"Count","type":{"pin_category":"int"}}],"outputs":[]}}},
 "variables":{"KeepCount":{"type":{"pin_category":"int"},"default_property":{"property_path":"KeepCount","value":23}}}},
 "graphs":{"event_graph:EventGraph":{"operation":"update","graph":{"name":"EventGraph","graph_kind":"event_graph"},
 "nodes":{"custom_event:HandleStatus":{"operation":"create","node_type":"custom_event","event_name":"HandleStatus","pos":{"x":0,"y":320},
 "signature":{"inputs":[{"name":"Enabled","type":{"pin_category":"bool"}},{"name":"Count","type":{"pin_category":"int"}}],"outputs":[]}}}}}
 }}}})JSON"), *ObjectPath);
	auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("inline"));
	Source->SetStringField(TEXT("document"), Seed);
	auto ImportArgs = MakeShared<FJsonObject>();
	ImportArgs->SetStringField(TEXT("target"), ObjectPath);
	ImportArgs->SetObjectField(TEXT("source"), Source);
	const auto Created = Submit(TEXT("content_graph.import_workspace"), ImportArgs);
	if (!Test.TestTrue(TEXT("public inline dispatcher and matching event fixture"), Created.IsOk()))
	{
		Test.AddError(Created.GetErrorJson());
		return false;
	}

	auto StableState = [](UBlueprint& Blueprint)
	{
		TMap<FString, FString> State;
		for (const FBPVariableDescription& Member : Blueprint.NewVariables)
			State.Add(TEXT("member/") + Member.VarName.ToString(), Member.VarGuid.ToString());
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			State.Add(TEXT("graph/") + Graph->GetName(), Graph->GraphGuid.ToString());
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node->IsA<UK2Node_AddDelegate>() || Node->IsA<UK2Node_RemoveDelegate>()) continue;
				const FString Key = Graph->GetName() + TEXT("/") + Node->NodeGuid.ToString();
				State.Add(Key, Node->GetClass()->GetPathName() + FString::Printf(TEXT(":%d,%d"), Node->NodePosX, Node->NodePosY));
				for (UEdGraphPin* Pin : Node->Pins)
					State.Add(Key + TEXT("/") + Pin->PinName.ToString(), Pin->PinId.ToString() + TEXT(":")
						+ Pin->PinType.PinCategory.ToString() + TEXT(":") + Pin->DefaultValue);
			}
		}
		return State;
	};
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("persisted seed loads"), Blueprint)) return false;
	const auto Baseline = StableState(*Blueprint);
	{
		// Match the no-Pin ContextMenu used by the public palette owner exactly.
		FBlueprintActionContext Context;
		Context.Blueprints.Add(Blueprint);
		UEdGraph* EventGraph = nullptr;
		for (UEdGraph* Graph : Blueprint->UbergraphPages) if (Graph->GetName() == TEXT("EventGraph")) EventGraph = Graph;
		if (!Test.TestNotNull(TEXT("native background context has the same EventGraph"), EventGraph)) return false;
		Context.Graphs.Add(EventGraph);
		const uint32 TargetMask = EContextTargetFlags::TARGET_Blueprint
			| EContextTargetFlags::TARGET_SubComponents | EContextTargetFlags::TARGET_NodeTarget
			| EContextTargetFlags::TARGET_PinObject | EContextTargetFlags::TARGET_SiblingPinObjects
			| EContextTargetFlags::TARGET_BlueprintLibraries;
		FBlueprintActionMenuBuilder Builder;
		FBlueprintActionMenuUtils::MakeContextMenu(Context, true, TargetMask, Builder);
		while (Builder.ProcessPendingActions()) {}
		bool bNativeAdd = false, bNativeRemove = false;
		for (int32 Index = 0; Index < Builder.GetNumActions(); ++Index)
		{
			const auto Action = Builder.GetSchemaAction(Index);
			if (!Action.IsValid() || Action->GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()) continue;
			const UBlueprintNodeSpawner* Spawner = StaticCastSharedPtr<FBlueprintActionMenuItem>(Action)->GetRawAction();
			const UBlueprintFieldNodeSpawner* FieldSpawner = Cast<UBlueprintFieldNodeSpawner>(Spawner);
			const FMulticastDelegateProperty* Property = FieldSpawner
				? CastField<FMulticastDelegateProperty>(FieldSpawner->GetField().ToField()) : nullptr;
			if (!Property || Property->GetFName() != TEXT("StatusChanged")
				|| !Property->GetOwnerClass() || Property->GetOwnerClass()->ClassGeneratedBy != Blueprint) continue;
			bNativeAdd |= Spawner->NodeClass == UK2Node_AddDelegate::StaticClass();
			bNativeRemove |= Spawner->NodeClass == UK2Node_RemoveDelegate::StaticClass();
		}
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Args->SetStringField(TEXT("query"), TEXT("StatusChanged"));
		const auto Background = Submit(TEXT("blueprint.node.candidates"), Args);
		if (!Test.TestTrue(TEXT("public background control query succeeds"), Background.IsOk())) { Test.AddError(Background.GetErrorJson()); return false; }
		TSharedPtr<FJsonObject> Payload;
		if (!CommandContract::Json::TryParseObject(Background.GetPayloadJson(), Payload, Error)) { Test.AddError(Error); return false; }
		const auto& Names = Payload->GetArrayField(TEXT("names"));
		Test.AddInfo(FString::Printf(TEXT("DelegateBackgroundControl native_add=%d native_remove=%d returned_name_groups=%d"),
			bNativeAdd, bNativeRemove, Names.Num()));
		if (bNativeAdd && bNativeRemove)
			Test.TestTrue(TEXT("native local delegate actions are not all lost by public background discovery"), Names.Num() > 0);
	}
	FGuid AddGuid;
	TMap<FName, FGuid> AddPinIds;

	for (const bool bRemove : {false, true})
	{
		const FString Stage = bRemove ? TEXT("remove") : TEXT("bind");
		auto QueryArgs = MakeShared<FJsonObject>();
		QueryArgs->SetStringField(TEXT("target"), ObjectPath);
		QueryArgs->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		if (!bBackground)
		{
			QueryArgs->SetStringField(TEXT("node_name"), TEXT("HandleStatus"));
			QueryArgs->SetStringField(TEXT("pin_name"), TEXT("OutputDelegate"));
		}
		QueryArgs->SetStringField(TEXT("query"), TEXT("StatusChanged"));
		const auto Queried = Submit(TEXT("blueprint.node.candidates"), QueryArgs);
		if (!Test.TestTrue(TEXT("public delegate pin candidate query"), Queried.IsOk())) { Test.AddError(Queried.GetErrorJson()); return false; }
		TSharedPtr<FJsonObject> CandidatePayload;
		if (!CommandContract::Json::TryParseObject(Queried.GetPayloadJson(), CandidatePayload, Error)) { Test.AddError(Error); return false; }
		FString CandidateName;
		for (const auto& Value : CandidatePayload->GetArrayField(TEXT("names")))
		{
			const FString Name = Value->AsString();
			if (Name == TEXT("Status Changed")) CandidateName = Name;
		}
		if (!Test.TestFalse(TEXT("native delegate action has a public name"), CandidateName.IsEmpty())) return false;
		const FString Workspace = WorkspaceRoot / Stage;
		auto ExportArgs = MakeShared<FJsonObject>();
		ExportArgs->SetStringField(TEXT("target"), ObjectPath);
		ExportArgs->SetStringField(TEXT("workspace_path"), Workspace);
		ExportArgs->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Exported = Submit(TEXT("content_graph.export_workspace"), ExportArgs);
		if (!Test.TestTrue(TEXT("public ordinary delegate baseline export"), Exported.IsOk())) { Test.AddError(Exported.GetErrorJson()); return false; }
		const FString GraphFile = Workspace / TEXT("graphs/eventgraph.hjson");
		FString Text;
		TSharedPtr<FJsonObject> GraphDocument;
		if (!FFileHelper::LoadFileToString(Text, *GraphFile)
			|| !FUeAgentStructuredHjsonParser::ParseObject(Text, GraphFile, GraphDocument, Error)) { Test.AddError(Error); return false; }
		auto NewNode = MakeShared<FJsonObject>();
		NewNode->SetStringField(TEXT("name"), CandidateName);
		auto Select = MakeShared<FJsonObject>();
		Select->SetStringField(TEXT("node_class"), bRemove
			? UK2Node_RemoveDelegate::StaticClass()->GetName() : UK2Node_AddDelegate::StaticClass()->GetName());
		NewNode->SetObjectField(TEXT("select"), Select);
		auto Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("node"), TEXT("HandleStatus1"));
		Context->SetStringField(TEXT("pin"), TEXT("OutputDelegate"));
		if (!bBackground) NewNode->SetObjectField(TEXT("candidate_context"), Context);
		auto Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 512);
		Position->SetNumberField(TEXT("y"), bRemove ? 640 : 320);
		NewNode->SetObjectField(TEXT("pos"), Position);
		GraphDocument->GetObjectField(TEXT("nodes"))->SetObjectField(bRemove ? TEXT("RemoveStatus") : TEXT("BindStatus"), NewNode);
		if (bRemove || bBackground)
		{
			auto Connections = GraphDocument->GetArrayField(TEXT("connections"));
			auto Edge = MakeShared<FJsonObject>();
			Edge->SetStringField(TEXT("from"), bRemove ? TEXT("ScriptBlueprintGraphK2NodeAddDelegate1.then") : TEXT("HandleStatus1.then"));
			Edge->SetStringField(TEXT("to"), bRemove ? TEXT("RemoveStatus.execute") : TEXT("BindStatus.execute"));
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
			if (bBackground)
			{
				auto DelegateEdge = MakeShared<FJsonObject>();
				DelegateEdge->SetStringField(TEXT("from"), TEXT("HandleStatus1.OutputDelegate"));
				DelegateEdge->SetStringField(TEXT("to"), bRemove ? TEXT("RemoveStatus.Delegate") : TEXT("BindStatus.Delegate"));
				Connections.Add(MakeShared<FJsonValueObject>(DelegateEdge));
			}
			GraphDocument->SetArrayField(TEXT("connections"), Connections);
		}
		if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(GraphDocument.ToSharedRef(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *GraphFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Test.AddError(Error); return false; }
		Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory"));
		Source->SetStringField(TEXT("path"), Workspace);
		ImportArgs->SetObjectField(TEXT("source"), Source);
		const FWeakObjectPtr BeforeImport(Blueprint);
		Blueprint = nullptr;
		const auto Imported = Submit(TEXT("content_graph.import_workspace"), ImportArgs);
		if (!Test.TestTrue(TEXT("public ordinary delegate action import"), Imported.IsOk())) { Test.AddError(Imported.GetErrorJson()); return false; }
		Test.TestFalse(TEXT("public import unloaded original Blueprint"), BeforeImport.IsValid());
		if (!Test.TestTrue(TEXT("independent package unload"), T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error))) { Test.AddError(Error); return false; }
		Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (!Test.TestNotNull(TEXT("delegate result independently reloads from disk"), Blueprint)) return false;
		Test.TestTrue(TEXT("compiled persisted Blueprint is clean"), Blueprint->GeneratedClass && !Blueprint->GetOutermost()->IsDirty()
			&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings));
		Test.TestTrue(TEXT("non-target native members graphs nodes and pins retain identity and values"), Baseline.OrderIndependentCompareEqual(StableState(*Blueprint)));
		const FIntProperty* Keep = FindFProperty<FIntProperty>(Blueprint->GeneratedClass, TEXT("KeepCount"));
		Test.TestTrue(TEXT("non-target CDO value remains 23"), Keep && Keep->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()) == 23);
		UK2Node_AddDelegate* Add = nullptr;
		UK2Node_RemoveDelegate* Remove = nullptr;
		UK2Node_CustomEvent* Handler = nullptr;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (auto* Found = Cast<UK2Node_AddDelegate>(Node)) { Test.TestNull(TEXT("only one Bind node"), Add); Add = Found; }
				if (auto* Found = Cast<UK2Node_RemoveDelegate>(Node)) { Test.TestNull(TEXT("only one Remove node"), Remove); Remove = Found; }
				if (auto* Found = Cast<UK2Node_CustomEvent>(Node)) if (Found->CustomFunctionName == TEXT("HandleStatus")) Handler = Found;
			}
		if (!Test.TestNotNull(TEXT("native Bind node persisted"), Add) || !Test.TestNotNull(TEXT("matching Custom Event persisted"), Handler)) return false;
		if (bRemove && !Test.TestNotNull(TEXT("native Remove node persisted"), Remove)) return false;
		for (UK2Node_BaseMCDelegate* Node : {static_cast<UK2Node_BaseMCDelegate*>(Add), static_cast<UK2Node_BaseMCDelegate*>(Remove)})
		{
			if (!Node) continue;
			const auto* Property = CastField<FMulticastDelegateProperty>(Node->GetProperty());
			Test.TestTrue(TEXT("native delegate reference resolves the local StatusChanged property"), Property
				&& Node->GetPropertyName() == TEXT("StatusChanged")
				&& Property->GetOwnerClass()->ClassGeneratedBy == Blueprint);
			const FStructProperty* RefProperty = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("DelegateReference"));
			const FMemberReference* Ref = RefProperty ? RefProperty->ContainerPtrToValuePtr<FMemberReference>(Node) : nullptr;
			// UBlueprintDelegateNodeSpawner sets an explicit owner reference (bSelfContext=false).
			const UClass* ReferenceOwner = Ref ? Ref->GetMemberParentClass(Blueprint->GeneratedClass) : nullptr;
			Test.TestTrue(TEXT("native StatusChanged reference retains the spawner's explicit local owner"), Ref && !Ref->IsSelfContext()
				&& Ref->GetMemberName() == TEXT("StatusChanged") && ReferenceOwner && ReferenceOwner->ClassGeneratedBy == Blueprint);
			const FBPVariableDescription* Member = Blueprint->NewVariables.FindByPredicate([](const FBPVariableDescription& Value) { return Value.VarName == TEXT("StatusChanged"); });
			Test.TestTrue(TEXT("native delegate reference retains the declared member GUID"), Ref && Member && Ref->GetMemberGuid() == Member->VarGuid);
			UFunction* Signature = Node->GetDelegateSignature(true);
			Test.TestTrue(TEXT("delegate has exact bool/int signature"), Signature && FindFProperty<FBoolProperty>(Signature, TEXT("Enabled")) && FindFProperty<FIntProperty>(Signature, TEXT("Count")));
			int32 ParameterCount = 0;
			if (Signature) for (TFieldIterator<FProperty> It(Signature); It; ++It) if (It->HasAnyPropertyFlags(CPF_Parm)) ++ParameterCount;
			Test.TestEqual(TEXT("delegate signature has exactly two parameters"), ParameterCount, 2);
			UEdGraphPin* DelegatePin = Node->GetDelegatePin();
			Test.TestTrue(TEXT("native delegate pin links the matching event output"), DelegatePin && DelegatePin->LinkedTo.Num() == 1
				&& DelegatePin->LinkedTo[0] == Handler->FindPin(TEXT("OutputDelegate")));
		}
		Test.TestTrue(TEXT("event execution remains connected to Bind"), Handler->FindPin(TEXT("then"))->LinkedTo.Contains(Add->FindPin(TEXT("execute"))));
		if (!bRemove)
		{
			AddGuid = Add->NodeGuid;
			for (UEdGraphPin* Pin : Add->Pins) AddPinIds.Add(Pin->PinName, Pin->PinId);
		}
		else
		{
			Test.TestEqual(TEXT("Remove creation preserves Bind identity"), Add->NodeGuid, AddGuid);
			for (UEdGraphPin* Pin : Add->Pins) Test.TestEqual(TEXT("Remove creation preserves Bind pin identity"), Pin->PinId, AddPinIds.FindRef(Pin->PinName));
			Test.TestTrue(TEXT("Bind then executes Remove"), Add->FindPin(TEXT("then"))->LinkedTo.Contains(Remove->FindPin(TEXT("execute"))));
		}
		ExportArgs->SetStringField(TEXT("workspace_path"), WorkspaceRoot / (Stage + TEXT("-readback")));
		const auto Readback = Submit(TEXT("content_graph.export_workspace"), ExportArgs);
		if (!Test.TestTrue(TEXT("independent ordinary delegate reexport"), Readback.IsOk())) { Test.AddError(Readback.GetErrorJson()); return false; }
	}
	Blueprint = nullptr;
	return !Test.HasAnyErrors();
}

bool FUeAgentT208BlueprintDelegateBindingTest::RunTest(const FString&)
{
	return RunDelegateBinding(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintBackgroundDelegateBindingTest,
	"UAI.Root.T208.Blueprint.BackgroundDelegateBindingWorkspace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208BlueprintBackgroundDelegateBindingTest::RunTest(const FString&)
{
	return RunDelegateBinding(*this, true);
}
