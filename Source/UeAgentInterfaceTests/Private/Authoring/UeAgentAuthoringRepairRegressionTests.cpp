#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Rigging/Authoring/UeAgentRigVMNodeAuthoring.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "UObject/Package.h"
#include "Materials/Material.h"
#include "OperationOwners/Material/UeAgentMaterialDomainService.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiDiagnosticTransferRepairTest,
    "UAI.Authoring.Repair.DiagnosticTransfer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiDiagnosticTransferRepairTest::RunTest(const FString&)
{
    using namespace UeAgentAuthoringDiagnostics;
    auto Failure = FDiagnostic::Failure(TEXT("semantic_snapshots_different"), TEXT("Expected and observed state differ."), TEXT("final_readback"));
    Failure.TransactionStatus = TEXT("rolled_back");
    Failure.RollbackAttempted = true;
    Failure.RollbackSucceeded = false;
    Failure.AssetApplied = false;
    Failure.RetryImport = false;
    Failure.RecoveryAction = TEXT("inspect_state");
    Failure.ExpectedValue = MakeShared<FJsonValueNull>();
    Failure.ActualValue = MakeShared<FJsonValueBoolean>(false);
    Failure.SemanticPointer = TEXT("/graphs/g/nodes/n/pins/0/type");
    Failure.SourceKind = TEXT("related_input");
    auto Related = MakeShared<FJsonObject>();
    Related->SetStringField(TEXT("source_file"), TEXT("graphs/eventgraph.hjson"));
    Related->SetStringField(TEXT("json_pointer"), TEXT("/nodes/MakeArray"));
    Related->SetNumberField(TEXT("line"), 7);
    Related->SetNumberField(TEXT("column"), 3);
    Failure.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
    auto Difference = FDiagnostic::Failure(TEXT("value_mismatch"), TEXT("Expected and observed class names differ."));
    Difference.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Pawn"));
    Difference.ActualValue = MakeShared<FJsonValueString>(TEXT("Actor"));
    Failure.Diagnostics.Add(Difference);
    UE::UAI::HjsonWorkspace::FWorkspaceError WorkspaceError;
    WorkspaceError.SetFailure(Failure, TEXT("final_readback"));
    const auto Public = WorkspaceError.GetDiagnostic().ToPublicJson();
    TestEqual(TEXT("transaction survives every layer"), Public->GetStringField(TEXT("transaction_status")), FString(TEXT("rolled_back")));
    TestFalse(TEXT("explicit false rollback preserved"), Public->GetBoolField(TEXT("rollback_succeeded")));
    TestFalse(TEXT("explicit false apply preserved"), Public->GetBoolField(TEXT("asset_applied")));
    TestTrue(TEXT("expected null is present"), Public->HasField(TEXT("expected_value")) && Public->TryGetField(TEXT("expected_value"))->IsNull());
    TestFalse(TEXT("actual false is present"), Public->GetBoolField(TEXT("actual_value")));
    TestEqual(TEXT("related source preserved"), Public->GetArrayField(TEXT("related_sources")).Num(), 1);
    TestEqual(TEXT("nested comparison value preserved"), Public->GetArrayField(TEXT("diagnostics"))[0]->AsObject()->GetStringField(TEXT("actual_value")), FString(TEXT("Actor")));
    FDiagnostic Empty;
    Empty.Code = TEXT("no_outcome_reported");
    TestFalse(TEXT("unknown rollback is not fabricated false"), Empty.ToPublicJson()->HasField(TEXT("rollback_succeeded")));
    auto NativeIssue = FDiagnostic::Failure(TEXT("curve_kind_mismatch"), TEXT("Curve kind does not match."), TEXT("curve_decode"));
    NativeIssue.PropertyPath = TEXT("curve.curve_kind");
    NativeIssue.ExpectedValue = MakeShared<FJsonValueString>(TEXT("float"));
    NativeIssue.ActualValue = MakeShared<FJsonValueString>(TEXT("vector"));
    const auto CurveFailure = MakeIssuesFailure(TEXT("curve_profile_state_invalid"),
        {NativeIssue}, TEXT("profile_state_preflight")).ToPublicJson();
    TestEqual(TEXT("native issue keeps expected fact"), CurveFailure->GetStringField(TEXT("expected_value")), FString(TEXT("float")));
    TestEqual(TEXT("native issue keeps actual fact"), CurveFailure->GetStringField(TEXT("actual_value")), FString(TEXT("vector")));
    NativeIssue.ExpectedValue = MakeShared<FJsonValueNull>();
    NativeIssue.ActualValue = MakeShared<FJsonValueBoolean>(false);
    const auto TypedIssue = NativeIssue.ToPublicJson();
    TestTrue(TEXT("native explicit null survives projection"), TypedIssue->HasField(TEXT("expected_value")) && TypedIssue->TryGetField(TEXT("expected_value"))->IsNull());
    TestFalse(TEXT("native explicit false survives projection"), TypedIssue->GetBoolField(TEXT("actual_value")));
    using namespace UE::UAI::StateSemantic;
    FUeAgentResourceTracker Resources;
    FUeAgentStateSemanticError EvidenceError;
    const FUeAgentCanonicalIdentity Identity(TEXT("private-semantic"), TEXT("actor"), TEXT("private-owner"),
        TMap<FString, FString>{{TEXT("actor_label"), TEXT("Door")}}, TEXT("target"), TEXT("private-generation"));
    FUeAgentCanonicalValuePtr PublicIdentity, InternalIdentity;
    if (!TestTrue(TEXT("typed public identity constructs"), TryCanonicalizeDifferenceEvidence(Identity, Resources, PublicIdentity, EvidenceError, EDifferenceEvidenceAudience::Public))
        || !TestTrue(TEXT("typed internal identity constructs"), TryCanonicalizeDifferenceEvidence(Identity, Resources, InternalIdentity, EvidenceError))) return false;
    FDiagnostic IdentityFailure = FDiagnostic::Failure(TEXT("identity_changed"), TEXT("The object identity changed."));
    TestTrue(TEXT("public evidence encodes"), CanonicalJson::TryToJsonValue(*PublicIdentity, IdentityFailure.ActualValue, EvidenceError));
    TestTrue(TEXT("internal evidence encodes"), CanonicalJson::TryToJsonValue(*InternalIdentity, IdentityFailure.ActualEvidence, EvidenceError));
    const auto PublicEvidence = IdentityFailure.ToPublicJson()->GetObjectField(TEXT("actual_value"));
    TestEqual(TEXT("public selector survives"), PublicEvidence->GetObjectField(TEXT("selectors"))->GetStringField(TEXT("actor_label")), FString(TEXT("Door")));
    TestFalse(TEXT("public identity does not carry machine owner"), PublicEvidence->HasField(TEXT("owner_id")));
    TestTrue(TEXT("internal identity remains available"), IdentityFailure.ToJson()->GetObjectField(TEXT("actual_evidence"))->HasField(TEXT("owner_id")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiBlueprintSelfTypeRepairTest,
    "UAI.Authoring.Repair.BlueprintSelfType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiBlueprintSelfTypeRepairTest::RunTest(const FString&)
{
    using namespace UeAgentBlueprintAssetLifecycle;
    using namespace UeAgentBlueprintAuthoringOps;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UBlueprint* Original = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
        CreatePackage(*(TEXT("/Temp/UAIRepair/Original_") + Suffix)), FName(TEXT("BP_Original")),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("original blueprint"), Original)) return false;
    UBlueprint* Working = DuplicateBlueprint(Original, CreatePackage(*(TEXT("/Temp/UAIRepair/Working_") + Suffix)), FName(TEXT("BP_Working")));
    if (!TestNotNull(TEXT("working blueprint"), Working)) return false;
    const FString Target = TEXT("/Game/UAI/NotCreated/BP_SelfTypeRepair");
    SetAuthoringContext(*Working, Target, Original);
    const FString Expected = Target + TEXT(".BP_SelfTypeRepair_C");
    for (UClass* SelfClass : {Original->GeneratedClass.Get(), Original->SkeletonGeneratedClass.Get(), Working->GeneratedClass.Get(), Working->SkeletonGeneratedClass.Get()})
    {
        if (!TestNotNull(TEXT("self class phase"), SelfClass)) return false;
        for (EPinContainerType Container : {EPinContainerType::None, EPinContainerType::Array, EPinContainerType::Set, EPinContainerType::Map})
        {
            FEdGraphPinType Pin;
            Pin.PinCategory = UEdGraphSchema_K2::PC_Object;
            Pin.PinSubCategoryObject = SelfClass;
            Pin.ContainerType = Container;
            Pin.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Object;
            Pin.PinValueType.TerminalSubCategoryObject = SelfClass;
            const auto Encoded = BuildPinTypeJsonObject(Pin, Working);
            TestEqual(TEXT("all self phases have the same public class"), Encoded->GetStringField(TEXT("pin_subcategory_object")), Expected);
            FEdGraphPinType Decoded;
            FString Error;
            if (!TestTrue(TEXT("public self type decodes in working context: ") + Error, BuildPinTypeFromJsonObject(Encoded, Decoded, Error, Working))) return false;
            TestTrue(TEXT("decoded type belongs to working blueprint"), Decoded.PinSubCategoryObject.Get() == Working->SkeletonGeneratedClass.Get());
            if (Container == EPinContainerType::Map)
            {
                TestEqual(TEXT("map value uses same public class"), Encoded->GetObjectField(TEXT("value_type"))->GetStringField(TEXT("pin_subcategory_object")), Expected);
                TestTrue(TEXT("map value resolves to working class"), Decoded.PinValueType.TerminalSubCategoryObject.Get() == Working->SkeletonGeneratedClass.Get());
            }
        }
    }
    TestEqual(TEXT("external actor class is not reclassified self"), GetAuthoringTypeObjectPath(Working, *AActor::StaticClass()), AActor::StaticClass()->GetPathName());
    UClass* Resolved = nullptr;
    TestFalse(TEXT("external class is not a self alias"), TryResolveAuthoringSelfClass(*Working, AActor::StaticClass()->GetPathName(), Resolved));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiRigDefinitionRepairTest,
    "UAI.Authoring.Repair.RigDefinitions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiRigDefinitionRepairTest::RunTest(const FString&)
{
    using namespace UeAgentRigVMNodeAuthoring;
    using namespace UE::UAI::QueryOwners::AnimationRigGraph;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>();
    Factory->ParentClass = UControlRig::StaticClass();
    auto* BP = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(UControlRigBlueprint::StaticClass(),
        CreatePackage(*(TEXT("/Temp/UAIRepair/Rig_") + Suffix)), FName(TEXT("CR_Repair")), RF_Transient, nullptr, GWarn));
    if (!TestNotNull(TEXT("rig fixture"), BP)) return false;
    BP->InitializeModelIfRequired(false);
    auto* Controller = BP->GetController(BP->GetDefaultModel());
    if (!TestNotNull(TEXT("native graph controller"), Controller)) return false;
    TestEqual(TEXT("variable registered with the RigVM host"), BP->AddMemberVariable(FName(TEXT("Count")), TEXT("int32"), false, false, TEXT("3")), FName(TEXT("Count")));
    auto* LibraryController = BP->GetController(BP->GetLocalFunctionLibrary());
    if (!TestNotNull(TEXT("native function library controller"), LibraryController)) return false;
    if (!TestNotNull(TEXT("local function"), LibraryController->AddFunctionToLibrary(FName(TEXT("LocalRepair")), true, FVector2D::ZeroVector, false, false))) return false;
    const TArray<FString> Definitions = {
        TEXT(R"({"node_type":"unit","script_struct":"/Script/ControlRig.RigUnit_BeginExecution","method":"Execute"})"),
        TEXT(R"({"node_type":"variable","variable_name":"Count","cpp_type":"int32","cpp_type_object_path":"","is_getter":true,"default_value":"3"})"),
        TEXT(R"({"node_type":"variable","variable_name":"Count","cpp_type":"int32","cpp_type_object_path":"","is_getter":false,"default_value":"4"})"),
        TEXT(R"({"node_type":"enum","enum_path":"/Script/ControlRig.ERigElementType"})"),
        TEXT(R"({"node_type":"function_reference","function_host":"self","function_name":"LocalRepair","variable_map":{}})"),
        TEXT(R"({"node_type":"invoke_entry","entry_name":"Forwards Solve"})")
    };
    int32 Index = 0;
    for (const FString& Text : Definitions)
    {
        TSharedPtr<FJsonObject> Definition;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Definition);
        FString Error;
        auto* Node = CreateNode(*BP, *Controller, *Definition, FString::Printf(TEXT("Node%d"), Index++), FVector2D::ZeroVector, Error);
        if (!TestNotNull(TEXT("native definition creation: ") + Error, Node)) return false;
        auto Captured = MakeShared<FJsonObject>();
        TestTrue(TEXT("created family captures"), CaptureDefinition(*BP, *Node, *Captured));
        if (auto* Variable = Cast<URigVMVariableNode>(Node))
            TestEqual(TEXT("variable getter/setter retained"), Variable->IsGetter(), Definition->GetBoolField(TEXT("is_getter")));
        const FString Name = Node->GetName();
        TestTrue(TEXT("remove before recreation"), Controller->RemoveNode(Node, false, false));
        auto* Recreated = CreateNode(*BP, *Controller, *Captured, Name, FVector2D::ZeroVector, Error);
        if (!TestNotNull(TEXT("captured definition recreates: ") + Error, Recreated)) return false;
        TestTrue(TEXT("captured definition remains stable: ") + Error, ValidateExistingDefinition(*BP, *Recreated, *Captured, Error));
    }
    FAnimationRigGraphCandidateQueryResult Candidates;
    FAnimationRigGraphCandidateQueryError QueryError;
    if (!TestTrue(TEXT("native IKTwoBone candidate search"), TryQueryAnimationRigGraphCandidates(*BP, TEXT("IKTwoBone"), Candidates, QueryError))) { AddError(QueryError.Code); return false; }
    const auto* Candidate = Candidates.Candidates.FindByPredicate([](const auto& Item) { return Item.Name == TEXT("IKTwoBone"); });
    if (!TestNotNull(TEXT("original failing candidate is present"), Candidate)) return false;
    AddInfo(TEXT("IKTwoBone native family: ") + Candidate->ActionKind);
    TestTrue(TEXT("original failing candidate now has persistence support"), Candidate->bSupported);
    URigVMNode* Created = nullptr;
    FAnimationRigGraphCandidate Resolved;
    if (!TestTrue(TEXT("original candidate creates natively"), TryPerformAnimationRigGraphCandidate(*BP, Candidate->Name, FVector2f::ZeroVector, Created, Resolved, QueryError))) { AddError(QueryError.Code + TEXT(":") + QueryError.PublicMessage); return false; }
    auto Captured = MakeShared<FJsonObject>();
    TestTrue(TEXT("original candidate produces a complete definition"), CaptureDefinition(*BP, *Created, *Captured));
    const FString Name = Created->GetName();
    Controller->RemoveNode(Created, false, false);
    FString Error;
    TestNotNull(TEXT("original candidate persisted definition recreates: ") + Error, CreateNode(*BP, *Controller, *Captured, Name, FVector2D::ZeroVector, Error));
    auto* Setter = Controller->AddVariableNodeFromObjectPath(FName(TEXT("Count")), TEXT("int32"), FString(), false,
        TEXT("4"), FVector2D::ZeroVector, TEXT("DefaultRepair"), false, false);
    if (!TestNotNull(TEXT("default edit fixture"), Setter)) return false;
    auto Definition = MakeShared<FJsonObject>();
    auto Defaults = MakeShared<FJsonObject>();
    Definition->SetObjectField(TEXT("pin_defaults"), Defaults);
    Definition->SetStringField(TEXT("default_value"), TEXT("11"));
    Defaults->SetStringField(TEXT("Value"), TEXT("4"));
    TArray<UE::UAI::Contract::FDiagnostic> Issues;
    TestTrue(TEXT("legacy unchanged duplicate cannot undo top-level edit"), ApplyPinDefaults(*Controller, *Setter, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("top-level edit survives"), Setter->FindPin(TEXT("Value"))->GetDefaultValue(), FString(TEXT("11")));
    Defaults->SetStringField(TEXT("Value"), TEXT("12"));
    TestTrue(TEXT("changed pin wins over unchanged legacy definition"), ApplyPinDefaults(*Controller, *Setter, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("pin edit survives"), Setter->FindPin(TEXT("Value"))->GetDefaultValue(), FString(TEXT("12")));
    Definition->SetStringField(TEXT("default_value"), TEXT("13"));
    Defaults->SetStringField(TEXT("Value"), TEXT("14"));
    TestFalse(TEXT("two conflicting changed aliases are diagnosed"), ApplyPinDefaults(*Controller, *Setter, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("conflict does not partially write"), Setter->FindPin(TEXT("Value"))->GetDefaultValue(), FString(TEXT("12")));
    Issues.Reset();
    Definition->SetStringField(TEXT("pin_defaults"), TEXT("invalid"));
    TestFalse(TEXT("invalid default carrier is not a successful no-op"), ApplyPinDefaults(*Controller, *Setter, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("invalid carrier has field provenance"), Issues[0]->AsObject()->GetStringField(TEXT("path")), FString(TEXT("node.pin_defaults")));
    Definition->SetObjectField(TEXT("pin_defaults"), Defaults);
    Issues.Reset();

    BP->AddMemberVariable(FName(TEXT("Offset")), TEXT("/Script/CoreUObject.Vector"), false, false, TEXT("(X=1,Y=2,Z=3)"));
    auto* Vector = Controller->AddVariableNodeFromObjectPath(FName(TEXT("Offset")), TEXT("FVector"), TEXT("/Script/CoreUObject.Vector"), false,
        TEXT("(X=1,Y=2,Z=3)"), FVector2D::ZeroVector, TEXT("VectorRepair"), false, false);
    if (!TestNotNull(TEXT("struct default fixture"), Vector)) return false;
    Definition->RemoveField(TEXT("default_value"));
    Defaults->Values.Reset();
    Defaults->SetStringField(TEXT("Value.X"), Vector->FindPin(TEXT("Value.X"))->GetDefaultValue());
    Defaults->SetStringField(TEXT("Value"), TEXT("(X=10,Y=20,Z=30)"));
    TestTrue(TEXT("root struct edit ignores stale child copy"), ApplyPinDefaults(*Controller, *Vector, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("root struct X retained"), FCString::Atod(*Vector->FindPin(TEXT("Value.X"))->GetDefaultValue()), 10.0);
    Defaults->SetStringField(TEXT("Value"), Vector->FindPin(TEXT("Value"))->GetDefaultValue());
    Defaults->SetStringField(TEXT("Value.X"), TEXT("15"));
    TestTrue(TEXT("child edit ignores stale parent copy"), ApplyPinDefaults(*Controller, *Vector, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("child X retained"), FCString::Atod(*Vector->FindPin(TEXT("Value.X"))->GetDefaultValue()), 15.0);

    BP->AddMemberVariable(FName(TEXT("Values")), TEXT("TArray<int32>"), false, false, TEXT("()"));
    auto* Array = Controller->AddVariableNodeFromObjectPath(FName(TEXT("Values")), TEXT("TArray<int32>"), FString(), false,
        TEXT("()"), FVector2D::ZeroVector, TEXT("ArrayRepair"), false, false);
    if (!TestNotNull(TEXT("array default fixture"), Array)) return false;
    Defaults->Values.Reset();
    Defaults->SetStringField(TEXT("Value"), TEXT("(2,4,6)"));
    TestTrue(TEXT("root array edit grows native pins"), ApplyPinDefaults(*Controller, *Array, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("array has three native elements"), Array->FindPin(TEXT("Value"))->GetSubPins().Num(), 3);
    Defaults->SetStringField(TEXT("Value"), TEXT("()"));
    TestTrue(TEXT("explicit empty array clears native pins"), ApplyPinDefaults(*Controller, *Array, *Definition, false, TEXT("node"), Issues));
    TestEqual(TEXT("array is empty"), Array->FindPin(TEXT("Value"))->GetSubPins().Num(), 0);

    TSharedPtr<FJsonObject> Document;
    using namespace UeAgentRiggingAuthoring;
    if (!TestTrue(TEXT("ordinary Rig capture"), BuildDocument(EAssetKind::ControlRig, *BP, Document, Error))) return false;
    bool bFoundSetter = false;
    const auto Graphs = Document->GetObjectField(TEXT("graphs"))->GetObjectField(TEXT("graphs"));
    for (const auto& Graph : Graphs->Values)
        for (const auto& Item : Graph.Value->AsObject()->GetObjectField(TEXT("nodes"))->Values)
        {
            const auto Node = Item.Value->AsObject();
            TestFalse(TEXT("definition no longer repeats mutable default"), Node->HasField(TEXT("default_value")));
            FString VariableName;
            bool bGetter = true;
            Node->TryGetStringField(TEXT("variable_name"), VariableName);
            Node->TryGetBoolField(TEXT("is_getter"), bGetter);
            if (VariableName == TEXT("Offset")) TestEqual(TEXT("struct exports one editable root"), Node->GetObjectField(TEXT("pin_defaults"))->Values.Num(), 1);
            if (VariableName == TEXT("Values")) TestEqual(TEXT("empty array remains editable"), Node->GetObjectField(TEXT("pin_defaults"))->Values.Num(), 1);
            if (VariableName == TEXT("Count") && !bGetter && Node->GetObjectField(TEXT("pin_defaults"))->GetStringField(TEXT("Value")) == TEXT("12"))
            {
                Node->SetStringField(TEXT("default_value"), TEXT("23"));
                bFoundSetter = true;
            }
        }
    if (!TestTrue(TEXT("ordinary setter located"), bFoundSetter)) return false;
    FMutationStats Stats;
    TestTrue(TEXT("ordinary adapter preserves local legacy edit: ") + Error, ApplyDocument(EAssetKind::ControlRig, *BP, Document, Stats, Error));
    TestEqual(TEXT("ordinary apply reaches intended native value"), Setter->FindPin(TEXT("Value"))->GetDefaultValue(), FString(TEXT("23")));
    FAnimationRigGraphCandidateQueryResult VariableCandidates;
    if (!TestTrue(TEXT("query native variable candidates"), TryQueryAnimationRigGraphCandidates(*BP, TEXT("Count"), VariableCandidates, QueryError))) return false;
    const auto* VariableCandidate = VariableCandidates.Candidates.FindByPredicate([](const auto& Item) {
        return Item.ActionKind.Contains(TEXT("VariableNodeSpawner"));
    });
    if (!TestNotNull(TEXT("native variable candidate available"), VariableCandidate)) return false;
    if (!TestTrue(TEXT("refresh ordinary document before adding candidate"), BuildDocument(EAssetKind::ControlRig, *BP, Document, Error))) return false;
    TSet<const URigVMNode*> ExistingNodes;
    for (const auto* Node : BP->GetDefaultModel()->GetNodes()) ExistingNodes.Add(Node);
    auto CandidateNode = MakeShared<FJsonObject>();
    auto CandidateSelector = MakeShared<FJsonObject>();
    CandidateSelector->SetStringField(TEXT("name"), VariableCandidate->Name);
    CandidateNode->SetObjectField(TEXT("candidate"), CandidateSelector);
    CandidateNode->SetStringField(TEXT("default_value"), TEXT("71"));
    Document->GetObjectField(TEXT("graphs"))->GetObjectField(TEXT("graphs"))
        ->GetObjectField(BP->GetDefaultModel()->GetNodePath())->GetObjectField(TEXT("nodes"))
        ->SetObjectField(TEXT("CandidateDefault"), CandidateNode);
    if (!TestTrue(TEXT("candidate creation retains authored fields: ") + Error, ApplyDocument(EAssetKind::ControlRig, *BP, Document, Stats, Error))) { AddError(Error); return false; }
    bool bCandidateValueApplied = false;
    for (const auto* Node : BP->GetDefaultModel()->GetNodes())
        if (!ExistingNodes.Contains(Node))
        {
            const auto* Value = Node->FindPin(TEXT("Value"));
            bCandidateValueApplied |= Value && Value->GetDefaultValue() == TEXT("71");
        }
    TestTrue(TEXT("candidate native value is authored value, not action default"), bCandidateValueApplied);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiMaterialCandidateContextRepairTest,
    "UAI.Authoring.Repair.MaterialCandidateContext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiMaterialCandidateContextRepairTest::RunTest(const FString&)
{
    using namespace UE::UAI::OperationOwners::Material;
    auto* Material = NewObject<UMaterial>(CreatePackage(*(TEXT("/Temp/UAIRepair/Material_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))),
        FName(TEXT("M_Repair")), RF_Public | RF_Standalone);
    TSharedPtr<IUeAgentMaterialCandidateCommandService> Service;
    FString Error;
    if (!TestTrue(TEXT("material public service"), TryCreateProductionMaterialCandidateCommandService(Service, Error))) return false;
    FUeAgentMaterialCandidateCommandRequest Request;
    Request.CommandId = TEXT("material.expression.candidates");
    FUeAgentMaterialCandidateCommandResponse Response;
    FUeAgentMaterialCandidateCommandError Failure;
    Request.CanonicalParamsJson = FString::Printf(TEXT("{\"target\":\"%s\",\"describe\":{\"name\":\"Multiply\"}}"), *Material->GetPathName());
    if (!TestTrue(TEXT("background description resolves"), Service->Execute(Request, Response, Failure))) { AddError(Failure.Code + Failure.PublicMessage); return false; }
    TestTrue(TEXT("description declares workspace creation support"), Response.PayloadJson.Contains(TEXT("creation")));
    Request.CanonicalParamsJson = FString::Printf(TEXT("{\"target\":\"%s\",\"pin_name\":\"MissingRepairPin\",\"describe\":{\"name\":\"Multiply\"}}"), *Material->GetPathName());
    TestFalse(TEXT("description cannot silently fall back from invalid pin context"), Service->Execute(Request, Response, Failure));
    TestTrue(TEXT("description reports pin context failure"), Failure.Code.Contains(TEXT("pin")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiDomainIssueCollectionRepairTest,
    "UAI.Authoring.Repair.DomainIssueCollection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUaiDomainIssueCollectionRepairTest::RunTest(const FString&)
{
    using namespace UeAgentRiggingAuthoring;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UeAgentAuthoringDiagnostics::FDiagnostic Error;
    UObject* Rig = CreateFactorySeed(EAssetKind::ControlRig,
        CreatePackage(*(TEXT("/Temp/UAIRepair/Issues_") + Suffix)), FName(TEXT("CR_Issues")), RF_Transient, Error);
    if (!TestNotNull(TEXT("native Control Rig issue fixture"), Rig)) return false;
    auto Document = MakeShared<FJsonObject>();
    Document->SetArrayField(TEXT("shape_libraries"),
        {MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueBoolean>(false)});
    FMutationStats Stats;
    TestFalse(TEXT("both invalid native library entries reject apply"),
        ApplyDocument(EAssetKind::ControlRig, *Rig, Document, Stats, Error));
    const auto& Failure = Error;
    TestEqual(TEXT("all native causes survive the domain"), Failure.Diagnostics.Num(), 2);
    if (Failure.Diagnostics.Num() == 2)
    {
        TestTrue(TEXT("first entry index survives"), Failure.Diagnostics[0].PropertyPath.EndsWith(TEXT("[0]")));
        TestTrue(TEXT("second entry index survives"), Failure.Diagnostics[1].PropertyPath.EndsWith(TEXT("[1]")));
    }
    return true;
}
