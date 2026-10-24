#include "Misc/AutomationTest.h"

#include "AIGraphNode.h"
#include "AIGraphTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "OperationOwners/AiBehavior/UeAgentAiBehaviorDomainService.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85EqsWorkspaceTest,
	"UAI.Root.T85.EQS.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85EqsWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("EQS_Workspace_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85EqsWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	auto* Query = NewObject<UEnvQuery>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Query);
	auto* Graph = NewObject<UEnvironmentQueryGraph>(Query, TEXT("EnvironmentQueryGraph"), RF_Transactional);
	Query->EdGraph = Graph;
	Graph->Initialize();
	auto* Root = NewObject<UEnvironmentQueryGraphNode_Root>(Graph);
	Root->CreateNewGuid();
	Root->PostPlacedNewNode();
	Root->AllocateDefaultPins();
	Graph->AddNode(Root, false, false);
	auto* OptionNode = NewObject<UEnvironmentQueryGraphNode_Option>(Graph);
	OptionNode->ClassData = FGraphNodeClassData(UEnvQueryGenerator_ActorsOfClass::StaticClass(), FString());
	OptionNode->CreateNewGuid();
	OptionNode->PostPlacedNewNode();
	OptionNode->AllocateDefaultPins();
	OptionNode->NodePosX = 320;
	Graph->AddNode(OptionNode, false, false);
	if (!Root->GetOutputPin() || !OptionNode->GetInputPin()) return false;
	Root->GetOutputPin()->MakeLinkTo(OptionNode->GetInputPin());
	auto* Option = Cast<UEnvQueryOption>(OptionNode->NodeInstance);
	if (!TestTrue(TEXT("native option creation owns the generator in the query asset"),
		Option && Option->GetOuter() == Query && Option->Generator && Option->Generator->GetOuter() == Query)) return false;
	UClass* TestClass = FindObject<UClass>(nullptr, TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Test"));
	FBoolProperty* Enabled = TestClass ? FindFProperty<FBoolProperty>(TestClass, TEXT("bTestEnabled")) : nullptr;
	if (!TestNotNull(TEXT("native test graph class is registered"), TestClass)
		|| !TestNotNull(TEXT("native test enable field exists"), Enabled)) return false;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Node = NewObject<UAIGraphNode>(Graph, TestClass);
		auto* Test = NewObject<UEnvQueryTest_Distance>(Query, NAME_None, RF_Transactional);
		Test->TestComment = Index == 0 ? TEXT("EqsEnabledMarker") : TEXT("EqsDisabledMarker");
		Test->FloatValueMin.DefaultValue = Index == 0 ? 125.f : 375.f;
		Node->NodeInstance = Test;
		OptionNode->AddSubNode(Node, Graph);
		Enabled->SetPropertyValue_InContainer(Node, Index == 0);
	}
	Graph->UpdateAsset();
	if (!TestTrue(TEXT("native update retains two editor tests but executes only the enabled one"),
		OptionNode->SubNodes.Num() == 2 && Query->GetOptions().Num() == 1 && Option->Tests.Num() == 1
		&& Option->Tests[0]->TestComment == TEXT("EqsEnabledMarker"))) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Query, *Filename, Save)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/eqs-common-r1/workspace") / Name);
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName);
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	FString Params, Error, Payload;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	bool bReadOnly = false;
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)) return false;
	if (!TestTrue(TEXT("ordinary export accepts a native EQS editor graph"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/AIModule.EnvQuery"), Params, Payload, bReadOnly, WorkspaceError)))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *(Workspace / TEXT("graphs")), TEXT("*.hjson"), true, false);
	FString Visible;
	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		Visible += Text;
	}
	TestTrue(TEXT("public graph includes enabled and disabled test properties"),
		Visible.Contains(TEXT("EqsEnabledMarker")) && Visible.Contains(TEXT("EqsDisabledMarker")));
	TestTrue(TEXT("ordinary export is read-only and preserves native state"), bReadOnly
		&& OptionNode->SubNodes.Num() == 2 && Option->Tests.Num() == 1 && !Package->IsDirty());
	// Exercise the shared property boundary independently of graph mutation/persistence.
	auto* Test = Cast<UEnvQueryTest_Distance>(OptionNode->SubNodes[1]->NodeInstance);
	if (!Test) return false;
	const auto Change = MakeShared<FJsonObject>();
	Change->SetStringField(TEXT("property_path"), TEXT("TestComment"));
	Change->SetStringField(TEXT("value"), TEXT("EqsEditedMarker"));
	const TArray<TSharedPtr<FJsonValue>> Changes{MakeShared<FJsonValueObject>(Change)};
	UeAgentAuthoringObjectApply::FOptions Options;
	Options.bCompactValues = true;
	UeAgentAuthoringDirectApply::FResult ApplyResult;
	TestFalse(TEXT("default instance property context remains restricted"),
		UeAgentAuthoringDirectApply::ApplyObjectPatch(Test, Changes, Options, ApplyResult));
	TestEqual(TEXT("rejected default-context edit preserves the value"), Test->TestComment, FString(TEXT("EqsDisabledMarker")));
	Options.bShowDefaultsOnly = true;
	TestTrue(TEXT("native Details context writes EditDefaultsOnly through common property apply"),
		UeAgentAuthoringDirectApply::ApplyObjectPatch(Test, Changes, Options, ApplyResult));
	TestEqual(TEXT("common apply changed the actual disabled test"), Test->TestComment, FString(TEXT("EqsEditedMarker")));
	Change->SetStringField(TEXT("property_path"), TEXT("TestOrder"));
	Change->SetNumberField(TEXT("value"), 99);
	TestFalse(TEXT("Details defaults visibility does not authorize non-editable runtime fields"),
		UeAgentAuthoringDirectApply::ApplyObjectPatch(Test, Changes, Options, ApplyResult));
	Change->SetStringField(TEXT("property_path"), TEXT("TestComment"));
	Change->SetStringField(TEXT("value"), TEXT("EqsDisabledMarker"));
	if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(Test, Changes, Options, ApplyResult)
		|| !UPackage::SavePackage(Package, Query, *Filename, Save)) return false;
	bool bEditedProperties = false, bEnabledTest = false;
	for (const FString& File : Files)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		bool bChanged = false;
		if (Text.Contains(TEXT("EqsDisabledMarker")))
		{
			Text.ReplaceInline(TEXT("EqsDisabledMarker"), TEXT("EqsImportedMarker"));
			bEditedProperties = bChanged = true;
		}
		if (Text.Contains(TEXT("enabled: false")))
		{
			Text.ReplaceInline(TEXT("enabled: false"), TEXT("enabled: true"));
			bEnabledTest = bChanged = true;
		}
		if (bChanged && !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestTrue(TEXT("ordinary workspace edits the disabled test property and enable flag"), bEditedProperties && bEnabledTest)) return false;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	bool bAddedTest = false;
	for (const FString& File : Files)
	{
		if (File.EndsWith(TEXT(".interfaces.hjson"))) continue;
		FString Text;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, WorkspaceError)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, WorkspaceError)) return false;
		if (Json->Type != EJson::Object || !Json->AsObject()->HasField(TEXT("nodes"))) continue;
		const auto Rows = Json->AsObject()->GetObjectField(TEXT("nodes"));
		FString Parent;
		for (const auto& Row : Rows->Values)
			if (Row.Value->AsObject()->TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty()) break;
		if (!TestFalse(TEXT("export supplies a readable EQS option reference"), Parent.IsEmpty())) return false;
		using namespace OperationOwners::AiBehavior;
		TSharedPtr<IUeAgentAiBehaviorCandidateCommandService> Service;
		if (!TryCreateProductionAiBehaviorCandidateCommandService(Service, Error)) return false;
		const auto CandidateParams = MakeShared<FJsonObject>();
		CandidateParams->SetStringField(TEXT("target"), ObjectPath);
		CandidateParams->SetStringField(TEXT("parent"), Parent);
		CandidateParams->SetStringField(TEXT("attachment_kind"), TEXT("test"));
		CandidateParams->SetStringField(TEXT("query"), TEXT("Distance"));
		FAiBehaviorCandidateCommandRequest Request;
		Request.CommandId = TEXT("ai.behavior.candidates");
		FAiBehaviorCandidateCommandResponse Response;
		FAiBehaviorCandidateCommandError CandidateError;
		if (!CommandContract::Json::TryCanonicalize(CandidateParams, Request.CanonicalParamsJson, Error)
			|| !Service->Execute(Request, Response, CandidateError))
		{ AddError(CandidateError.Code); return false; }
		TSharedPtr<FJsonObject> Result;
		if (!CommandContract::Json::TryParseObject(Response.PayloadJson, Result, Error)) return false;
		const auto& Names = Result->GetArrayField(TEXT("names"));
		if (!TestTrue(TEXT("public EQS parent query finds a unique native Distance action without mutation"),
			Response.bReadOnly && Names.Num() == 1 && !Query->GetOutermost()->IsDirty())) return false;
		const auto NewTest = MakeShared<FJsonObject>();
		const auto Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), Names[0]->AsString());
		NewTest->SetObjectField(TEXT("candidate"), Candidate);
		NewTest->SetStringField(TEXT("parent"), Parent);
		NewTest->SetStringField(TEXT("attachment_kind"), TEXT("test"));
		NewTest->SetBoolField(TEXT("enabled"), true);
		NewTest->SetNumberField(TEXT("order"), 2);
		Rows->SetObjectField(TEXT("AddedDistance"), NewTest);
		const FString DistanceCandidateName = Names[0]->AsString();
		CandidateParams->RemoveField(TEXT("parent"));
		CandidateParams->RemoveField(TEXT("attachment_kind"));
		CandidateParams->SetStringField(TEXT("query"), TEXT("ActorsOfClass"));
		if (!CommandContract::Json::TryCanonicalize(CandidateParams, Request.CanonicalParamsJson, Error)
			|| !Service->Execute(Request, Response, CandidateError))
		{ AddError(CandidateError.Code); return false; }
		if (!CommandContract::Json::TryParseObject(Response.PayloadJson, Result, Error)) return false;
		const auto& GeneratorNames = Result->GetArrayField(TEXT("names"));
		if (!TestTrue(TEXT("public EQS background query exposes the native generator action"),
			Response.bReadOnly && GeneratorNames.Num() == 1)) return false;
		const auto NewOption = MakeShared<FJsonObject>();
		const auto GeneratorCandidate = MakeShared<FJsonObject>();
		GeneratorCandidate->SetStringField(TEXT("name"), GeneratorNames[0]->AsString());
		NewOption->SetObjectField(TEXT("candidate"), GeneratorCandidate);
		const auto Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 640);
		Position->SetNumberField(TEXT("y"), 0);
		NewOption->SetObjectField(TEXT("pos"), Position);
		Rows->SetObjectField(TEXT("AddedGenerator"), NewOption);
		const auto NewChild = MakeShared<FJsonObject>();
		const auto ChildCandidate = MakeShared<FJsonObject>();
		ChildCandidate->SetStringField(TEXT("name"), DistanceCandidateName);
		NewChild->SetObjectField(TEXT("candidate"), ChildCandidate);
		NewChild->SetStringField(TEXT("parent"), TEXT("AddedGenerator"));
		NewChild->SetStringField(TEXT("attachment_kind"), TEXT("test"));
		NewChild->SetBoolField(TEXT("enabled"), true);
		NewChild->SetNumberField(TEXT("order"), 0);
		Rows->SetObjectField(TEXT("NewParentDistance"), NewChild);
		auto Connections = Json->AsObject()->GetArrayField(TEXT("connections"));
		if (!TestEqual(TEXT("fixture exports its native root-option connection"), Connections.Num(), 1)) return false;
		const auto ExistingConnection = Connections[0]->AsObject();
		const FString ExistingTarget = ExistingConnection->GetStringField(TEXT("to"));
		int32 PinSeparator = INDEX_NONE;
		if (!ExistingTarget.FindLastChar(TEXT('.'), PinSeparator)) return false;
		const auto NewConnection = MakeShared<FJsonObject>();
		NewConnection->SetStringField(TEXT("from"), ExistingConnection->GetStringField(TEXT("from")));
		NewConnection->SetStringField(TEXT("to"), TEXT("AddedGenerator") + ExistingTarget.Mid(PinSeparator));
		Connections.Add(MakeShared<FJsonValueObject>(NewConnection));
		Json->AsObject()->SetArrayField(TEXT("connections"), Connections);
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		bAddedTest = true;
	}
	if (!TestTrue(TEXT("ordinary HJSON uses the returned test name unchanged"), bAddedTest)) return false;
	const auto Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("directory"));
	Source->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources,
		Resolved, Revision, Authorization, WorkspaceError)) return false;
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName);
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UEnvQuery> BeforeImport(Query);
	Query = nullptr; Graph = nullptr; Option = nullptr; OptionNode = nullptr; Root = nullptr; Test = nullptr; Package = nullptr;
	if (!TestTrue(TEXT("ordinary EQS graph and properties import persists through common Reflection"),
		DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, TEXT("/Script/AIModule.EnvQuery"), Params, Payload, bReadOnly, WorkspaceError)))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	Query = LoadObject<UEnvQuery>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("import reloads the saved EQS asset"), Query && !BeforeImport.IsValid() && !Query->GetOutermost()->IsDirty())) return false;
	Graph = Cast<UEnvironmentQueryGraph>(Query->EdGraph);
	if (!TestTrue(TEXT("runtime options retain the original tests and include the new connected generator"), Graph && Query->GetOptions().Num() == 2 && Query->GetOptions()[0]->Tests.Num() == 3)) return false;
	Option = Query->GetOptions()[0];
	TestEqual(TEXT("enabled test property persists"), Option->Tests[0]->TestComment, FString(TEXT("EqsEnabledMarker")));
	TestEqual(TEXT("previously disabled test edit persists"), Option->Tests[1]->TestComment, FString(TEXT("EqsImportedMarker")));
	TestEqual(TEXT("untouched nested provider value persists"), Option->Tests[1]->FloatValueMin.DefaultValue, 375.f);
	TestTrue(TEXT("native named test creation persists with correct asset ownership"),
		Option->Tests[2]->IsA<UEnvQueryTest_Distance>() && Option->Tests[2]->GetOuter() == Query);
	const auto* AddedOption = Query->GetOptions()[1];
	TestTrue(TEXT("same-import native generator and test creation persists with correct ownership"),
		AddedOption->GetOuter() == Query && AddedOption->Generator
		&& AddedOption->Generator->IsA<UEnvQueryGenerator_ActorsOfClass>() && AddedOption->Generator->GetOuter() == Query
		&& AddedOption->Tests.Num() == 1 && AddedOption->Tests[0]->IsA<UEnvQueryTest_Distance>()
		&& AddedOption->Tests[0]->GetOuter() == Query);
	// Exercise the migrated specialized consumer against the same nonempty graph.
	using namespace Composition::GraphCompound;
	using namespace Contract;
	FGraphCompoundRuntimeConfig RuntimeConfig;

	FGraphCompoundPcgAiProductionConfig ProductionConfig;
	ProductionConfig.DurableJournalRoot = FPaths::GetPath(Workspace) / TEXT("graph-compound-journals");
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TryCreatePcgAiGraphProductionRuntime(RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
	{ AddError(RuntimeError.Code + TEXT(":") + RuntimeError.Detail); return false; }
	const FString NewId = TEXT("node:") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const auto Definition = MakeShared<FJsonObject>();
	Definition->SetStringField(TEXT("carrier_type_ref"), UEnvironmentQueryGraphNode_Option::StaticClass()->GetPathName());
	Definition->SetStringField(TEXT("payload_type_ref"), UEnvQueryGenerator_ActorsOfClass::StaticClass()->GetPathName());
	const auto Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("property_path"), TEXT("NodePosX"));
	Property->SetStringField(TEXT("value_type"), TEXT("int32"));
	Property->SetNumberField(TEXT("value"), 960);
	auto MakeEdit = [](const FString& Id, const FString& Kind, const TSharedPtr<FJsonObject>& Value,
		const TArray<TSharedPtr<FJsonValue>>& Dependencies) {
		const auto Edit = MakeShared<FJsonObject>();
		Edit->SetStringField(TEXT("identity"), Id);
		Edit->SetStringField(TEXT("element_kind"), Kind);
		Edit->SetStringField(TEXT("intent"), TEXT("create"));
		Edit->SetArrayField(TEXT("depends_on"), Dependencies);
		Edit->SetObjectField(TEXT("value"), Value);
		return MakeShared<FJsonValueObject>(Edit);
	};
	const auto Specialized = MakeShared<FJsonObject>();
	Specialized->SetStringField(TEXT("target_ref"), ObjectPath);
	Specialized->SetArrayField(TEXT("edits"), {
		MakeEdit(NewId, TEXT("item"), Definition, {}),
		MakeEdit(TEXT("property:") + NewId + TEXT(":NodePosX"), TEXT("property"), Property, {MakeShared<FJsonValueString>(NewId)})});
	if (!CommandContract::Json::TryCanonicalize(Specialized, Params, Error)) return false;
	class FEqsCancellation final : public IUeAgentCancellationSignal
	{ public: bool IsCancelled() const override { return false; } };
	const auto* Route = FindPcgAnimationGraphRouteProfile(TEXT("ai.eqs.graph.apply"));
	if (!Route) return false;
	TWeakObjectPtr<UEnvQuery> BeforeSpecialized(Query);
	Query = nullptr; Graph = nullptr; Option = nullptr;
	const auto SpecializedResult = Runtime->GetOperationPort()->Submit(
		FUeAgentOperationRequest(Route->GetExecutionContract().GetDescriptorIdentity(),
			TEXT("eqs-specialized:") + Name, TEXT("eqs-specialized:") + Name, FUeAgentContractValue(Params)),
		FUeAgentCallerContext(TEXT("test.agent"), {TEXT("pcg_ai_node_graph.read"), TEXT("pcg_ai_node_graph.write")}, TEXT("t208-eqs")),
		MakeShared<FEqsCancellation>());
	TSharedPtr<FJsonObject> SpecializedResponse;
	if (!SpecializedResult.IsValid() || !CommandContract::Json::TryParseObject(SpecializedResult.GetValue().GetJsonValue(), SpecializedResponse, Error)) return false;
	const TSharedPtr<FJsonObject>* OperationError = nullptr;
	if (SpecializedResponse->TryGetObjectField(TEXT("operation_error"), OperationError))
	{ AddError((*OperationError)->GetStringField(TEXT("code")) + TEXT(":") + (*OperationError)->GetStringField(TEXT("detail"))); return false; }
	Query = LoadObject<UEnvQuery>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("specialized EQS mutation uses common independent reload"),
		Query && !BeforeSpecialized.IsValid() && !Query->GetOutermost()->IsDirty() && Query->GetOptions().Num() == 3)) return false;
	TestTrue(TEXT("specialized mutation preserves existing test graph and properties"),
		Query->GetOptions()[0]->Tests.Num() == 3 && Query->GetOptions()[1]->Tests.Num() == 1
		&& Query->GetOptions()[0]->Tests[1]->TestComment == TEXT("EqsImportedMarker")
		&& Query->GetOptions()[0]->Tests[1]->FloatValueMin.DefaultValue == 375.f);
	TestTrue(TEXT("specialized new generator has native query ownership"),
		Query->GetOptions()[2]->Generator && Query->GetOptions()[2]->Generator->GetOuter() == Query);
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	UEnvQueryTest* DirtyTest = Query->GetOptions()[0]->Tests[1];
	DirtyTest->Modify();
	DirtyTest->TestComment = TEXT("EqsDirtyBeforeFailure");
	Graph = CastChecked<UEnvironmentQueryGraph>(Query->EdGraph);
	FString RemovedId;
	for (UEdGraphNode* GraphNode : Graph->Nodes)
		if (auto* NativeOption = Cast<UEnvironmentQueryGraphNode_Option>(GraphNode);
			NativeOption && NativeOption->NodeInstance == Query->GetOptions()[0])
		{
			NativeOption->Modify();
			NativeOption->NodePosY += 96;
			RemovedId = TEXT("node:") + NativeOption->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		}
	if (RemovedId.IsEmpty()) return false;
	Query->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentEqsAuthoring::ReadGraphDocument(*Query, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const auto DesiredNodes = Desired->AsObject()->GetObjectField(TEXT("nodes"));
	TArray<FString> RemovedIds{RemovedId};
	for (const auto& Pair : DesiredNodes->Values)
		if (Pair.Value->AsObject()->GetStringField(TEXT("parent")) == RemovedId) RemovedIds.Add(Pair.Key);
	for (const auto& Id : RemovedIds) DesiredNodes->RemoveField(Id);
	auto DesiredConnections = Desired->AsObject()->GetArrayField(TEXT("connections"));
	DesiredConnections.RemoveAll([&](const auto& Edge) {
		return RemovedIds.Contains(Edge->AsObject()->GetObjectField(TEXT("from"))->GetStringField(TEXT("node")))
			|| RemovedIds.Contains(Edge->AsObject()->GetObjectField(TEXT("to"))->GetStringField(TEXT("node")));
	});
	Desired->AsObject()->SetArrayField(TEXT("connections"), DesiredConnections);
	TWeakObjectPtr<UEnvQuery> OriginalQuery(Query), RejectedQuery;
	TWeakObjectPtr<UEnvQueryTest> OriginalTest(DirtyTest);
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("eqs");
	LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true;
	LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("graph"), Desired->AsObject());
	bool bLateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Actual = LoadObject<UEnvQuery>(nullptr, *ObjectPath);
		RejectedQuery = Actual;
		TArray<uint8> ActualDisk;
		bLateObserved = !OriginalQuery.IsValid(true, true) && !OriginalTest.IsValid(true, true)
			&& Actual && !Actual->GetOutermost()->IsDirty() && Actual->GetOptions().Num() == 2
			&& Actual->GetOptions()[0]->Tests.Num() == 1
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_eqs_late_rejected");
		Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	Query = nullptr; Graph = nullptr; DirtyTest = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late EQS deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("eqs-late-") + Name));
	if (!TestTrue(TEXT("the option and its tests were deleted and persisted before rejection"), bLateObserved)
		|| !TestEqual(TEXT("late EQS rejection retains its own diagnostic"), LateResult.Diagnostic.Code, FString(TEXT("t208_eqs_late_rejected")))
		|| !TestTrue(TEXT("EQS checkpoint recovery is confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	TestFalse(TEXT("rejected EQS state exits during recovery"), RejectedQuery.IsValid(true, true));
	Query = LoadObject<UEnvQuery>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("complete EQS graph and dirty property state are restored"), Query
		&& UeAgentEqsAuthoring::ReadGraphDocument(*Query, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error)
		&& RestoredText == DirtyBefore && Query->GetOutermost()->IsDirty());
	TestTrue(TEXT("EQS original disk is restored independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	return !HasAnyErrors();
}
