#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/UMG/Authoring/UeAgentUmgMutationModel.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"

namespace
{
    UEdGraphNode* IntentNode(UEdGraph& Graph)
    {
        auto* Node = NewObject<UEdGraphNode>(&Graph);
        Node->CreateNewGuid();
        Graph.AddNode(Node, false, false);
        return Node;
    }

    TSharedRef<FJsonObject> IntentEnd(const UEdGraphNode& Node, const TCHAR* Pin)
    {
        auto End = MakeShared<FJsonObject>();
        End->SetStringField(TEXT("node"), Node.NodeGuid.ToString());
        End->SetStringField(TEXT("pin"), Pin);
        return End;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentGraphFrozenLinkIntentTest,
    "UeAgentInterface.Core.HjsonWorkspace.FrozenLinkIntent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphFrozenLinkIntentTest::RunTest(const FString&)
{
    FString Error;
    TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));
    Graph->GraphGuid = FGuid::NewGuid();
    auto* From = IntentNode(*Graph);
    auto* Bridge = IntentNode(*Graph);
    auto* To = IntentNode(*Graph);
    auto* Out = From->CreatePin(EGPD_Output, TEXT("exec"), TEXT("Out"));
    auto* In = To->CreatePin(EGPD_Input, TEXT("exec"), TEXT("In"));
    auto* BridgeIn = Bridge->CreatePin(EGPD_Input, TEXT("exec"), TEXT("In"));
    auto* BridgeOut = Bridge->CreatePin(EGPD_Output, TEXT("exec"), TEXT("Out"));
    Out->MakeLinkTo(BridgeIn);
    BridgeOut->MakeLinkTo(In);
    UeAgentAuthoringGraphLinks::FIntent Intent;
    if (!TestTrue(TEXT("schema conversion path retains original endpoints"),
            Intent.ExpectConnected(Out, In, {Bridge}, TEXT("/edges/converted"), Error))
        || !TestTrue(TEXT("freeze native creation bindings"), Intent.Freeze(*Graph, Error))) return false;
    UPackage* CopyPackage = CreatePackage(*(TEXT("/Temp/UAI/FrozenLink_") + FGuid::NewGuid().ToString()));
    TStrongObjectPtr<UEdGraph> Copy(DuplicateObject<UEdGraph>(Graph.Get(), CopyPackage));
    if (!TestTrue(TEXT("frozen conversion path rebinds to an independent owner"), Intent.Verify(*Copy, Error))) return false;
    TArray<UEdGraph*> SameReferences{Copy.Get(), Copy.Get()};
    TestTrue(TEXT("repeated graph references remain one native identity"), Intent.Verify(*Copy, SameReferences, Error));
    TArray<UEdGraph*> ForeignReferences{Graph.Get()};
    TestFalse(TEXT("an old owner's graph cannot attest the current owner"), Intent.Verify(*Copy, ForeignReferences, Error));
    Copy->Rename(TEXT("RenamedGraph"), CopyPackage, REN_DontCreateRedirectors | REN_DoNotDirty);
    TestTrue(TEXT("graph name normalization retains native identity"), Intent.Verify(*Copy, Error));
    UEdGraphNode* CopiedFrom = nullptr;
    for (UEdGraphNode* Node : Copy->Nodes) if (Node->NodeGuid == From->NodeGuid) CopiedFrom = Node;
    if (!CopiedFrom) return false;
    CopiedFrom->FindPinById(Out->PinId)->BreakAllPinLinks();
    TestFalse(TEXT("copy's missing requested edge cannot become a new expected value"), Intent.Verify(*Copy, Error));
    TestTrue(TEXT("failure keeps the original requested source"), Error.Contains(TEXT("/edges/converted")));
    TestTrue(TEXT("original owner's correct edge cannot hide copy failure"), Intent.Verify(*Graph, Error));

    UeAgentAuthoringGraphLinks::FIntent Removed;
    Out->BreakLinkTo(BridgeIn);
    Removed.Expect(Out, BridgeIn, false, TEXT("/edges/removed"));
    // Replayed creation identities change before freezing, without changing intent.
    Out->PinId = FGuid::NewGuid();
    if (!TestTrue(TEXT("creation replay binds current pin identity once"), Removed.Freeze(*Graph, Error))) return false;
    Out->MakeLinkTo(BridgeIn);
    TestFalse(TEXT("restored forbidden edge is detected"), Removed.Verify(*Graph, Error));
    Out->BreakLinkTo(BridgeIn);
    Graph->RemoveNode(Bridge);
    TestTrue(TEXT("native endpoint deletion satisfies requested disconnection"), Removed.Verify(*Graph, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentBlueprintFrozenLinkIntentTest,
    "UeAgentInterface.Core.HjsonWorkspace.BlueprintLinkIntentLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintFrozenLinkIntentTest::RunTest(const FString&)
{
    using namespace UeAgentBlueprintAuthoring;
    using namespace UeAgentBlueprintAssetLifecycle;
    FString Error;
    const FString PackagePath = TEXT("/Game/UAI/Transient/HjsonIntent_BP_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
    const FString ObjectPath = PackagePath + TEXT(".") + Name;
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        CreatePackage(*PackagePath), *Name, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    ON_SCOPE_EXIT
    {
        Blueprint.Reset();
        if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            TestEqual(TEXT("owned Blueprint fixture removed"), ObjectTools::DeleteObjects({Owned}, false), 1);
        }
    };
    if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) return false;
    FAssetRegistryModule::AssetCreated(Blueprint.Get());
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
    auto* Event = EventCreator.CreateNode(); Event->CustomFunctionName = TEXT("RequestedStart"); EventCreator.Finalize();
    FGraphNodeCreator<UK2Node_ExecutionSequence> SequenceCreator(*Graph);
    auto* Sequence = SequenceCreator.CreateNode(); SequenceCreator.Finalize();
    const FGuid FromId = Event->NodeGuid, ToId = Sequence->NodeGuid;
    auto Edge = MakeShared<FJsonObject>();
    Edge->SetObjectField(TEXT("from"), IntentEnd(*Event, TEXT("then")));
    Edge->SetObjectField(TEXT("to"), IntentEnd(*Sequence, TEXT("execute")));
    auto Edges = MakeShared<FJsonObject>(); Edges->SetObjectField(TEXT("requested"), Edge);
    auto Spec = MakeShared<FJsonObject>(); Spec->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
    Spec->SetStringField(TEXT("graph_kind"), TEXT("event_graph")); Spec->SetObjectField(TEXT("edges"), Edges);
    auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(Graph->GraphGuid.ToString(), Spec);
    auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs);
    TStrongObjectPtr<UBlueprint> Scratch(DuplicateBlueprint(Blueprint.Get(), CreatePackage(*(TEXT("/Temp/UAI/IntentScratch_") + Name))));
    TUniquePtr<FPreparedBlueprintMutation> Plan;
    FMutationCounters Counters;
    if (!Scratch || !FPreparedBlueprintMutation::Prepare(*Scratch, Extension, Plan, Counters, Error)
        || !Plan->ApplyTo(*Blueprint, Counters, Error) || !CompileBlueprint(Blueprint.Get(), Error)
        || !Plan->FinalizeAfterCompile(*Blueprint, Counters, Error)) { AddError(Error); return false; }
    TestTrue(TEXT("original requested edge retained through native compile"), Plan->VerifyIntent(*Blueprint, Error));
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint.Get(), *Filename, SaveArgs)) return false;
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    UPackage* Package = Blueprint->GetOutermost(); Blueprint.Reset(); Scratch.Reset();
    FText UnloadError;
    if (!UPackageTools::UnloadPackages({Package}, UnloadError, true)) { AddError(UnloadError.ToString()); return false; }
    Blueprint.Reset(LoadObject<UBlueprint>(nullptr, *ObjectPath));
    if (!Blueprint || !TestTrue(TEXT("original edge intent survives actual disk reload"), Plan->VerifyIntent(*Blueprint, Error)))
        { AddError(Error); return false; }
    UEdGraphNode* From = nullptr;
    UEdGraphNode* To = nullptr;
    TArray<UEdGraph*> ReloadedGraphs; Blueprint->GetAllGraphs(ReloadedGraphs);
    for (auto* ReloadedGraph : ReloadedGraphs) for (UEdGraphNode* Node : ReloadedGraph->Nodes)
    {
        if (Node->NodeGuid == FromId) From = Node;
        if (Node->NodeGuid == ToId) To = Node;
    }
    if (!From || !To) return false;
    From->FindPin(TEXT("then"))->BreakLinkTo(To->FindPin(TEXT("execute")));
    TestFalse(TEXT("late native edge removal does not overwrite original expectation"), Plan->VerifyIntent(*Blueprint, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMaterialFrozenGraphIntentTest,
    "UeAgentInterface.Core.HjsonWorkspace.MaterialGraphIntentLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentMaterialFrozenGraphIntentTest::RunTest(const FString&)
{
    FString Error, Detail;
    TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage()));
    auto Property = MakeShared<FJsonObject>(); Property->SetStringField(TEXT("property_path"), TEXT("R"));
    Property->SetNumberField(TEXT("value"), 0.1);
    auto Expression = MakeShared<FJsonObject>(); Expression->SetStringField(TEXT("id"), TEXT("source"));
    Expression->SetStringField(TEXT("expression_class"), UMaterialExpressionConstant::StaticClass()->GetPathName());
    Expression->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueObject>(Property)});
    auto RootFrom = MakeShared<FJsonObject>(); RootFrom->SetStringField(TEXT("node"), TEXT("source")); RootFrom->SetStringField(TEXT("output"), TEXT(""));
    auto Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("material_property"), TEXT("Roughness")); Root->SetObjectField(TEXT("from"), RootFrom);
    auto Document = MakeShared<FJsonObject>(); Document->SetArrayField(TEXT("expressions"), {MakeShared<FJsonValueObject>(Expression)});
    Document->SetArrayField(TEXT("root_inputs"), {MakeShared<FJsonValueObject>(Root)});
    UeAgentMaterialAuthoring::FPreparedGraph Plan;
    if (!Plan.Prepare(*Material, Document, TEXT(""), TEXT("MaterialGraph"), Error, Detail)) { AddError(Error + Detail); return false; }
    if (!TestTrue(TEXT("native float normalization is accepted"), Plan.VerifyIntent(*Material, Error))) { AddError(Error); return false; }
    TStrongObjectPtr<UMaterial> Copy(DuplicateObject<UMaterial>(Material.Get(), CreatePackage(*(TEXT("/Temp/UAI/MaterialIntent_") + FGuid::NewGuid().ToString()))));
    Plan.ReleaseForPersistentReadback();
    if (!TestTrue(TEXT("frozen property/relation intent rebinds without old property storage"), Plan.VerifyIntent(*Copy, Error))) { AddError(Error); return false; }
    auto* Constant = Cast<UMaterialExpressionConstant>(Copy->GetExpressions()[0]);
    Constant->R = 0.75f;
    TestFalse(TEXT("later property corruption cannot be recaptured as expected"), Plan.VerifyIntent(*Copy, Error));
    Constant->R = 0.1f;
    TestTrue(TEXT("restoring the authored value retains semantic float equality"), Plan.VerifyIntent(*Copy, Error));
    Copy->GetExpressionInputForProperty(MP_Roughness)->Expression = nullptr;
    TestFalse(TEXT("later root relationship corruption is detected"), Plan.VerifyIntent(*Copy, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentUmgFrozenLinkIntentTest,
    "UeAgentInterface.Core.HjsonWorkspace.UmgLinkIntentLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentUmgFrozenLinkIntentTest::RunTest(const FString&)
{
    FString Error;
    const FString PackagePath = TEXT("/Game/UAI/Transient/HjsonIntent_UMG_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
    const FString ObjectPath = PackagePath + TEXT(".") + Name;
    TStrongObjectPtr<UWidgetBlueprint> Blueprint(Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        UUserWidget::StaticClass(), CreatePackage(*PackagePath), *Name, BPTYPE_Normal,
        UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass())));
    ON_SCOPE_EXIT
    {
        Blueprint.Reset();
        if (UObject* Owned = LoadObject<UObject>(nullptr, *ObjectPath))
        {
            Owned->GetOutermost()->SetDirtyFlag(false);
            TestEqual(TEXT("owned UMG fixture removed"), ObjectTools::DeleteObjects({Owned}, false), 1);
        }
    };
    if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) return false;
    FAssetRegistryModule::AssetCreated(Blueprint.Get());
    UEdGraph* Graph = Blueprint->UbergraphPages[0];
    FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
    auto* Event = EventCreator.CreateNode(); Event->CustomFunctionName = TEXT("RequestedStart"); EventCreator.Finalize();
    FGraphNodeCreator<UK2Node_ExecutionSequence> SequenceCreator(*Graph);
    auto* Sequence = SequenceCreator.CreateNode(); SequenceCreator.Finalize();
    auto Edge = MakeShared<FJsonObject>();
    Edge->SetObjectField(TEXT("from"), IntentEnd(*Event, TEXT("then")));
    Edge->SetObjectField(TEXT("to"), IntentEnd(*Sequence, TEXT("execute")));
    auto Edges = MakeShared<FJsonObject>(); Edges->SetObjectField(TEXT("requested"), Edge);
    auto Spec = MakeShared<FJsonObject>(); Spec->SetObjectField(TEXT("edges"), Edges);
    auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(Graph->GraphGuid.ToString(), Spec);
    auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs);
    Extension->SetObjectField(TEXT("bindings"), MakeShared<FJsonObject>());
    TStrongObjectPtr<UWidgetBlueprint> Scratch(Cast<UWidgetBlueprint>(UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(
        Blueprint.Get(), CreatePackage(*(TEXT("/Temp/UAI/UmgIntentScratch_") + Name)))));
    TUniquePtr<UeAgentUmgAuthoring::FPreparedUmgMutation> Plan;
    UeAgentUmgAuthoring::FMutationCounters Counters;
    bool bSecondCompile = false;
    if (!Scratch || !UeAgentUmgAuthoring::FPreparedUmgMutation::Prepare(*Scratch, Extension, Plan, Counters, Error, PackagePath)
        || !Plan->ApplyTo(*Blueprint, Counters, Error)
        || !UeAgentBlueprintAssetLifecycle::CompileBlueprint(Blueprint.Get(), Error)
        || !Plan->FinalizeAfterCompile(*Blueprint, Counters, bSecondCompile, Error)
        || !TestTrue(TEXT("binding finalization requests its second native compile"), bSecondCompile)
        || !UeAgentBlueprintAssetLifecycle::CompileBlueprint(Blueprint.Get(), Error)
        || !TestTrue(TEXT("UMG reuses frozen Blueprint intent after second compile"), Plan->VerifyIntent(*Blueprint, Error)))
        { AddError(Error); return false; }
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Blueprint->GetOutermost(), Blueprint.Get(), *Filename, SaveArgs)) return false;
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    UPackage* Package = Blueprint->GetOutermost(); Blueprint.Reset(); Scratch.Reset();
    FText UnloadError;
    if (!UPackageTools::UnloadPackages({Package}, UnloadError, true)) { AddError(UnloadError.ToString()); return false; }
    Blueprint.Reset(LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath));
    if (!Blueprint || !TestTrue(TEXT("UMG wrapper retains intent through actual disk reload"), Plan->VerifyIntent(*Blueprint, Error)))
        { AddError(Error); return false; }
    for (UEdGraphNode* Node : Blueprint->UbergraphPages[0]->Nodes)
        if (auto* Rebound = Cast<UK2Node_CustomEvent>(Node); Rebound && Rebound->CustomFunctionName == TEXT("RequestedStart"))
            Rebound->FindPin(TEXT("then"))->BreakAllPinLinks();
    TestFalse(TEXT("UMG's empty own property set cannot hide late Blueprint edge loss"), Plan->VerifyIntent(*Blueprint, Error));
    return true;
}
