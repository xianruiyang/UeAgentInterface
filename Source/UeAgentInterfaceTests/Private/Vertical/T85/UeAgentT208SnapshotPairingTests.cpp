#include "Blueprint/UserWidget.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208SnapshotPairingTest,
    "UAI.Root.T208.SemanticSnapshot.Pairing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208SnapshotPairingTest::RunTest(const FString&)
{
    using namespace UeAgentAuthoringState;
    using namespace UeAgentAuthoringSemanticCompare;
    UE::UAI::Contract::FDiagnostic Error;
    if (!UeAgentBlueprintAuthoring::RegisterAuthoring(Error)
        || !UeAgentUmgAuthoring::RegisterAuthoring(Error))
    { AddError(Error.ToPublicDetail()); return false; }
    TStrongObjectPtr<UWidgetBlueprint> Blueprint(UeAgentUmgAuthoring::CreateFactorySeed(
        UUserWidget::StaticClass(), GetTransientPackage(), NAME_None, RF_Transient, Error));
    if (!TestNotNull(TEXT("native widget fixture exists"), Blueprint.Get())) return false;
    Blueprint->ClearFlags(RF_Public | RF_Standalone);
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("PairingGraph"),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(), Graph);
    FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
    UK2Node_CustomEvent* Event = Creator.CreateNode();
    Event->CustomFunctionName = TEXT("PairingEvent");
    Creator.Finalize();
    TArray<FReadRequest> All;
    if (!UeAgentUmgAuthoring::BuildAssetSnapshotReadRequests(Blueprint.Get(),
        Blueprint->GetOutermost()->GetName(), false, All, Error))
    { AddError(Error.ToPublicDetail()); return false; }
    TArray<FReadRequest> Both, AssetOnly;
    for (const FReadRequest& Read : All)
    {
        if (Read.SemanticId == UeAgentUmgAuthoring::AssetSemanticId)
        { Both.Add(Read); AssetOnly.Add(Read); }
        else if (Read.SemanticId == UeAgentBlueprintAuthoring::NodeSemanticId)
        { Both.Add(Read); }
    }
    if (!TestEqual(TEXT("one asset and one node captured"), Both.Num(), 2)
        || !TestEqual(TEXT("shared native asset selected"), AssetOnly.Num(), 1)) return false;
    FCaptureResult Full, Shared;
    if (!CaptureSnapshot(Both, Full) || !CaptureSnapshot(AssetOnly, Shared))
    { AddError(TEXT("native snapshot capture failed")); return false; }
    FComparisonReport Equal;
    if (!CompareSnapshots(Full.Snapshot, Full.Snapshot, {}, Equal)) return false;
    TestTrue(TEXT("equal native snapshots remain equivalent"), Equal.bEquivalent);
    auto Check = [&](const FStateSnapshot& Left, const FStateSnapshot& Right,
        const FString& Missing, bool CommonEquivalent)
    {
        FComparisonReport Report;
        if (!CompareSnapshots(Left, Right, {}, Report)) { AddError(Report.Diagnostic.Code); return; }
        TestFalse(TEXT("real missing entry remains different"), Report.bEquivalent);
        TestEqual(TEXT("one missing and one paired entry only"), Report.Entries.Num(), 2);
        int32 MissingCount = 0, CommonCount = 0;
        for (const FEntryComparison& Entry : Report.Entries)
        {
            if (Entry.SemanticId == UeAgentBlueprintAuthoring::NodeSemanticId)
            { ++MissingCount; TestEqual(TEXT("missing side is exact"), Entry.Diagnostic.Code, Missing); }
            if (Entry.SemanticId == UeAgentUmgAuthoring::AssetSemanticId)
            {
                ++CommonCount;
                TestFalse(TEXT("shared asset is never misreported missing"), Entry.Diagnostic.Code.StartsWith(TEXT("semantic_missing_")));
                TestEqual(TEXT("shared asset value comparison preserved"), Entry.bEquivalent, CommonEquivalent);
            }
        }
        TestEqual(TEXT("one actual missing node"), MissingCount, 1);
        TestEqual(TEXT("one shared asset pair"), CommonCount, 1);
    };
    Check(Full.Snapshot, Shared.Snapshot, TEXT("semantic_missing_right"), true);
    Check(Shared.Snapshot, Full.Snapshot, TEXT("semantic_missing_left"), true);
    Blueprint->WidgetTree->RootWidget = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(), TEXT("ChangedRoot"));
    FCaptureResult Changed;
    if (!CaptureSnapshot(AssetOnly, Changed)) return false;
    Check(Full.Snapshot, Changed.Snapshot, TEXT("semantic_missing_right"), false);
    return !HasAnyErrors();
}
