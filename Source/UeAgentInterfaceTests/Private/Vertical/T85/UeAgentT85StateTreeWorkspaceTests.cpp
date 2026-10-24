#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "GameFramework/Character.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorTypes.h"
#include "StateTreeState.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeWorkspaceTest,
	"UAI.Root.T85.StateTree.Workspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeWorkspaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("StateTree_Workspace_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85StateTreeWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	auto* Tree = NewObject<UStateTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	auto* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
	Tree->EditorData = Data;
	Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data, NAME_None, RF_Transactional);
	auto& Root = Data->AddSubTree(TEXT("WorkspaceRoot"));
	FStateTreeEditorColor Accent;
	Accent.DisplayName = TEXT("Workspace Accent");
	Accent.Color = FLinearColor(0.2f, 0.4f, 0.6f, 1.f);
	const FStateTreeEditorColorRef AccentRef = Accent.ColorRef;
	Data->Colors.Add(Accent);
	Root.ColorRef = AccentRef;
	auto& Child = Root.AddChildState(TEXT("UntouchedChild"));
	Child.Description = TEXT("StateTreeUntouchedMarker");
	Child.Parameters.Parameters.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	Child.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 245.125f);
	const FGuid SpeedId = Child.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID;
	Child.Parameters.PropertyOverrides.Add(SpeedId);
	UScriptStruct* Delay = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
	if (!TestNotNull(TEXT("native delay task"), Delay)) return false;
	auto& Task = Root.Tasks.AddDefaulted_GetRef();
	Task.ID = FGuid::NewGuid(); Task.Node.InitializeAs(Delay);
	Task.Instance.InitializeAs(CastChecked<UScriptStruct>(Task.Node.Get<FStateTreeNodeBase>().GetInstanceDataType()));
	FFloatProperty* Duration = FindFProperty<FFloatProperty>(Task.Instance.GetScriptStruct(), TEXT("Duration"));
	if (!TestNotNull(TEXT("native delay duration"), Duration)) return false;
	Duration->SetPropertyValue_InContainer(Task.Instance.GetMutableMemory(), 7.25f);
	const auto* RootBagProperty = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParameterPropertyBag"));
	if (!TestNotNull(TEXT("root parameter bag"), RootBagProperty)) return false;
	auto* RootBag = RootBagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Data);
	RootBag->AddProperty(TEXT("Threshold"), EPropertyBagPropertyType::Float);
	RootBag->AddProperty(TEXT("Alternative"), EPropertyBagPropertyType::Float);
	RootBag->AddProperty(TEXT("WrongType"), EPropertyBagPropertyType::String);
	RootBag->SetValueFloat(TEXT("Threshold"), 2.f);
	RootBag->SetValueFloat(TEXT("Alternative"), 41.375f);
	RootBag->SetValueString(TEXT("WrongType"), TEXT("not a numeric binding"));
	const FGuid AlternativeId = RootBag->FindPropertyDescByName(TEXT("Alternative"))->ID;
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Threshold")),
		FPropertyBindingPath(Task.ID, TEXT("Duration")));
	Root.bHasRequiredEventToEnter = true;
	Root.RequiredEventToEnter.PayloadStruct = Task.Instance.GetScriptStruct();
	FPropertyBindingPath EventPath(Root.GetEventID());
	FPropertyBindingDataView EventView;
	FString EventError;
	if (!TestTrue(TEXT("native event payload path resolves against its declared instance type"),
		Data->GetBindingDataViewByID(Root.GetEventID(), EventView) && EventPath.FromString(TEXT("Payload.Duration"))
		&& EventPath.UpdateSegmentsFromValue(EventView, &EventError))) return false;
	Data->AddPropertyBinding(EventPath, FPropertyBindingPath(Task.ID, TEXT("RandomDeviation")));
	FStateTreeCompilerLog CompileLog;
	if (!TestTrue(TEXT("native fixture compiles before authoring"), UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog))) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Tree, *Filename, Save)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/state-tree-common-r1/workspace") / Name);
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName); Export->SetStringField(TEXT("workspace_path"), Workspace);
	FString Params, Error, Payload;
	HjsonWorkspace::FWorkspaceError Failure;
	bool ReadOnly = false;
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)) return false;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
		PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	TestTrue(TEXT("ordinary export preserves clean native fixture"), ReadOnly && !Package->IsDirty());
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult Candidates;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("delay"), Candidates, CandidateError))
	{ AddError(CandidateError.Code); return false; }
	const auto* DelayCandidate = Candidates.Candidates.FindByPredicate([&](const auto& Candidate) { return Candidate.PayloadType == Delay->GetPathName(); });
	if (!TestNotNull(TEXT("native schema candidate exposes delay creation"), DelayCandidate)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult BindingCandidates;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("binding"), BindingCandidates, CandidateError)) return false;
	const auto* BindingCandidate = BindingCandidates.Candidates.FindByPredicate([](const auto& Candidate) { return Candidate.Kind == TEXT("state_tree_binding"); });
	if (!TestNotNull(TEXT("native binding candidate is discoverable"), BindingCandidate)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult ColorCandidates;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("Color"), ColorCandidates, CandidateError)) return false;
	const auto* ColorCandidate = ColorCandidates.Candidates.FindByPredicate([](const auto& Candidate) { return Candidate.Kind == TEXT("state_tree_color"); });
	if (!TestNotNull(TEXT("native palette creation is discoverable"), ColorCandidate)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult FunctionCandidates;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("add"), FunctionCandidates, CandidateError)) return false;
	const auto* FunctionCandidate = FunctionCandidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.PayloadType == TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction");
	});
	if (!TestNotNull(TEXT("native float-add function candidate is discoverable"), FunctionCandidate)) return false;
	const auto NewTask = MakeShared<FJsonObject>();
	NewTask->SetStringField(TEXT("name"), DelayCandidate->Name);
	NewTask->SetStringField(TEXT("parent"), TEXT("AddedState"));
	NewTask->SetStringField(TEXT("placement"), TEXT("task"));
	const auto InitialDuration = MakeShared<FJsonObject>();
	InitialDuration->SetStringField(TEXT("type"), TEXT("float"));
	InitialDuration->SetStringField(TEXT("value"), TEXT("InvalidInitialDuration"));
	const auto InitialParameter = MakeShared<FJsonObject>();
	InitialParameter->SetObjectField(TEXT("instance.Duration"), InitialDuration);
	const auto InitialCategories = MakeShared<FJsonObject>();
	InitialCategories->SetObjectField(TEXT("Parameter"), InitialParameter);
	NewTask->SetObjectField(TEXT("categories"), InitialCategories);
	FString NewTaskText;
	if (!CommandContract::Json::TryCanonicalize(NewTask, NewTaskText, Error)) return false;
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool EditedName = false, EditedDuration = false, AddedItems = false;
	bool EditedRootParameter = false, EditedStateParameter = false;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		bool Changed = false;
		if (Text.Contains(TEXT("WorkspaceRoot: {")))
		{ Text.ReplaceInline(TEXT("WorkspaceRoot: {"), TEXT("WorkspaceRoot: { label: ImportedRoot,")); EditedName = Changed = true; }
		if (Text.Contains(TEXT("7.25")))
		{ Text.ReplaceInline(TEXT("7.25"), TEXT("9.75")); EditedDuration = Changed = true; }
		if (Text.ReplaceInline(TEXT("41.375"), TEXT("44.625")) > 0) EditedRootParameter = Changed = true;
		if (Text.ReplaceInline(TEXT("245.125"), TEXT("310.625")) > 0) EditedStateParameter = Changed = true;
		if (Text.Contains(TEXT("tree_items: {")))
		{
			Text.ReplaceInline(TEXT("tree_items: {"), *(FString(TEXT("tree_items: {\nAddedState: { name: State, parent: WorkspaceRoot, index: 1 }\nAddedDelay: ")) + NewTaskText
				+ TEXT("\nAddedTransition: { name: Transition, parent: AddedState, target: UntouchedChild, transition_type: GotoState }\n")));
			AddedItems = Changed = true;
		}
		if (Changed && !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	}
	if (!TestTrue(TEXT("ordinary files expose edits and new item collection"), EditedName && EditedDuration && AddedItems)) return false;
	if (!TestTrue(TEXT("ordinary files expose root and state parameter values"), EditedRootParameter && EditedStateParameter)) return false;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	const auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName); Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UStateTree> BeforeImport(Tree);
	TSharedPtr<FJsonValue> InitialBefore, InitialAfter;
	FString InitialBeforeText, InitialAfterText;
	TArray<uint8> InitialDiskBefore, InitialDiskAfter;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, InitialBefore, Error)
		|| !CommandContract::Json::TryCanonicalize(InitialBefore->AsObject(), InitialBeforeText, Error)
		|| !FFileHelper::LoadFileToArray(InitialDiskBefore, *Filename)) return false;
	if (!TestFalse(TEXT("invalid new-task initial value is rejected"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
	AddInfo(TEXT("Initial value rejection: ") + Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer());
	if (!TestTrue(TEXT("invalid initial value preserves original live object"), BeforeImport.IsValid())) return false;
	if (!TestTrue(TEXT("invalid initial value preserves live tree and disk"), BeforeImport.IsValid()
		&& !Tree->GetOutermost()->IsDirty() && UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, InitialAfter, Error)
		&& CommandContract::Json::TryCanonicalize(InitialAfter->AsObject(), InitialAfterText, Error)
		&& InitialAfterText == InitialBeforeText && FFileHelper::LoadFileToArray(InitialDiskAfter, *Filename)
		&& InitialDiskBefore == InitialDiskAfter)) return false;
	bool CorrectedInitialValue = false;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		if (Text.ReplaceInline(TEXT("\"InvalidInitialDuration\""), TEXT("6.5")) > 0)
		{
			if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			CorrectedInitialValue = true;
		}
	}
	if (!TestTrue(TEXT("correct only the initial value in ordinary HJSON"), CorrectedInitialValue)) return false;
	FString ExistingItemFile, ExistingItemText;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		if (!Text.Contains(TEXT("WorkspaceRoot: { label: ImportedRoot,"))) continue;
		ExistingItemFile = File; ExistingItemText = Text;
		Text.ReplaceInline(TEXT("WorkspaceRoot: { label: ImportedRoot,"), TEXT("WorkspaceRoot: { categories: {}, label: ImportedRoot,"));
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		break;
	}
	if (!TestFalse(TEXT("existing structure row found for initial-property boundary"), ExistingItemFile.IsEmpty())) return false;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	if (!TestFalse(TEXT("existing item categories are rejected rather than ignored"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
	if (!TestEqual(TEXT("existing item categories have an explicit error"), Failure.GetCode(), FString(TEXT("state_tree_initial_categories_require_create")))
		|| !TestTrue(TEXT("existing item categories preserve original tree"), BeforeImport.IsValid() && !Tree->GetOutermost()->IsDirty())) return false;
	if (!FFileHelper::SaveStringToFile(ExistingItemText, *ExistingItemFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	FString BindingFile, ValidBindingText;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		const int32 Changes = Text.ReplaceInline(TEXT("source_path: Threshold"), TEXT("source_path: MissingBindingField"))
			+ Text.ReplaceInline(TEXT("source_path: \"Threshold\""), TEXT("source_path: MissingBindingField"));
		if (!Changes) continue;
		BindingFile = File; ValidBindingText = Text.Replace(TEXT("source_path: MissingBindingField"), TEXT("source_path: Alternative"));
		const int32 TargetChanges = ValidBindingText.ReplaceInline(TEXT("target_path: Duration"), TEXT("target_path: RandomDeviation"))
			+ ValidBindingText.ReplaceInline(TEXT("target_path: \"Duration\""), TEXT("target_path: RandomDeviation"));
		if (!TestEqual(TEXT("ordinary target binding path found"), TargetChanges, 1)) return false;
		HjsonWorkspace::FHjsonDocumentPtr BindingParsed;
		TSharedPtr<FJsonValue> BindingJson;
		if (!HjsonWorkspace::TryParseHjsonDocument(ValidBindingText, File, Resources, BindingParsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*BindingParsed->GetRoot(), Resources, BindingJson, Failure)) return false;
		bool Redirected = false;
		for (const auto& Row : BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->Values)
		{
			FString Path;
			if (!Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) || Path != TEXT("Alternative")) continue;
			const auto BindingRow = Row.Value->AsObject();
			const FString OriginalTaskAlias = BindingRow->GetStringField(TEXT("target"));
			BindingRow->SetStringField(TEXT("target"), TEXT("AddedDelay"));
			const auto AddedBinding = MakeShared<FJsonObject>();
			AddedBinding->SetStringField(TEXT("name"), BindingCandidate->Name);
			AddedBinding->SetStringField(TEXT("source"), TEXT("AddedSum"));
			AddedBinding->SetStringField(TEXT("source_path"), TEXT("Result"));
			AddedBinding->SetStringField(TEXT("target"), TEXT("AddedDelay"));
			AddedBinding->SetStringField(TEXT("target_path"), TEXT("Duration"));
			BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->SetObjectField(TEXT("AddedBinding"), AddedBinding);
			const auto FunctionRow = MakeShared<FJsonObject>();
			FunctionRow->SetStringField(TEXT("name"), FunctionCandidate->Name);
			FunctionRow->SetStringField(TEXT("parent"), TEXT("AddedDelay"));
			const auto FunctionParameters = MakeShared<FJsonObject>();
			for (const auto& Input : TArray<TPair<FString, double>>{{TEXT("instance.Left"), 2.0}, {TEXT("instance.Right"), 3.0}})
			{
				const auto Property = MakeShared<FJsonObject>();
				Property->SetStringField(TEXT("type"), TEXT("float")); Property->SetNumberField(TEXT("value"), Input.Value);
				FunctionParameters->SetObjectField(Input.Key, Property);
			}
			const auto FunctionCategories = MakeShared<FJsonObject>(); FunctionCategories->SetObjectField(TEXT("Parameter"), FunctionParameters);
			FunctionRow->SetObjectField(TEXT("categories"), FunctionCategories);
			BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->SetObjectField(TEXT("AddedSum"), FunctionRow);
			const auto InputBinding = MakeShared<FJsonObject>();
			InputBinding->SetStringField(TEXT("name"), BindingCandidate->Name);
			InputBinding->SetStringField(TEXT("source"), OriginalTaskAlias);
			InputBinding->SetStringField(TEXT("source_path"), TEXT("Duration"));
			InputBinding->SetStringField(TEXT("target"), TEXT("AddedSum"));
			InputBinding->SetStringField(TEXT("target_path"), TEXT("Left"));
			BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->SetObjectField(TEXT("FunctionInput"), InputBinding);
			const auto NestedFunction = MakeShared<FJsonObject>();
			NestedFunction->SetStringField(TEXT("name"), FunctionCandidate->Name);
			NestedFunction->SetStringField(TEXT("parent"), TEXT("AddedSum"));
			BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->SetObjectField(TEXT("NestedSum"), NestedFunction);
			const auto NestedBinding = MakeShared<FJsonObject>();
			NestedBinding->SetStringField(TEXT("name"), BindingCandidate->Name);
			NestedBinding->SetStringField(TEXT("source"), TEXT("NestedSum"));
			NestedBinding->SetStringField(TEXT("source_path"), TEXT("Result"));
			NestedBinding->SetStringField(TEXT("target"), TEXT("AddedSum"));
			NestedBinding->SetStringField(TEXT("target_path"), TEXT("Right"));
			BindingJson->AsObject()->GetObjectField(TEXT("tree_items"))->SetObjectField(TEXT("NestedOutput"), NestedBinding);
			if (!CommandContract::Json::TryCanonicalize(BindingJson->AsObject(), ValidBindingText, Error)) return false;
			BindingRow->SetStringField(TEXT("target"), TEXT("UntouchedChild"));
			if (!CommandContract::Json::TryCanonicalize(BindingJson->AsObject(), Text, Error)) return false;
			Redirected = true; break;
		}
		if (!TestTrue(TEXT("redirect existing binding to a same-import new task"), Redirected)) return false;
		if (!FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		break;
	}
	if (!TestFalse(TEXT("ordinary binding source path is editable"), BindingFile.IsEmpty())) return false;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	if (!TestFalse(TEXT("non-bindable state endpoint is rejected before target mutation"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
	if (!TestEqual(TEXT("invalid binding path error"), Failure.GetCode(), FString(TEXT("state_tree_binding_path_invalid")))
		|| !TestTrue(TEXT("invalid binding path preserves original tree and disk"), BeforeImport.IsValid()
			&& !Tree->GetOutermost()->IsDirty() && UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, InitialAfter, Error)
			&& CommandContract::Json::TryCanonicalize(InitialAfter->AsObject(), InitialAfterText, Error)
			&& InitialAfterText == InitialBeforeText && FFileHelper::LoadFileToArray(InitialDiskAfter, *Filename)
			&& InitialDiskBefore == InitialDiskAfter)) return false;
	HjsonWorkspace::FHjsonDocumentPtr TypeParsed;
	TSharedPtr<FJsonValue> TypeJson;
	if (!HjsonWorkspace::TryParseHjsonDocument(ValidBindingText, BindingFile, Resources, TypeParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*TypeParsed->GetRoot(), Resources, TypeJson, Failure)) return false;
	auto VerifyRejectedFunction = [&](const TCHAR* ExpectedCode) {
		FString RejectedText;
		if (!CommandContract::Json::TryCanonicalize(TypeJson->AsObject(), RejectedText, Error)
			|| !FFileHelper::SaveStringToFile(RejectedText, *BindingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
		Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		Import->RemoveField(TEXT("authorization_id"));
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
		if (!TestFalse(ExpectedCode, DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
		return TestEqual(TEXT("rejection identifies the violated contract"), Failure.GetCode(), FString(ExpectedCode))
			&& TestTrue(TEXT("rejected input preserves original live tree and disk"), BeforeImport.IsValid()
				&& !Tree->GetOutermost()->IsDirty() && UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, InitialAfter, Error)
				&& CommandContract::Json::TryCanonicalize(InitialAfter->AsObject(), InitialAfterText, Error)
				&& InitialAfterText == InitialBeforeText && FFileHelper::LoadFileToArray(InitialDiskAfter, *Filename)
				&& InitialDiskBefore == InitialDiskAfter);
	};
	const auto FunctionRows = TypeJson->AsObject()->GetObjectField(TEXT("tree_items"));
	TSharedPtr<FJsonObject> EventBindingRow;
	for (const auto& Row : FunctionRows->Values)
	{
		FString Path;
		if (Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) && Path == TEXT("Payload.Duration"))
			EventBindingRow = Row.Value->AsObject();
	}
	if (!TestTrue(TEXT("event binding has an ordinary readable payload path"), EventBindingRow.IsValid())) return false;
	EventBindingRow->SetStringField(TEXT("source_path"), TEXT("Payload.MissingEventField"));
	if (!VerifyRejectedFunction(TEXT("state_tree_binding_path_invalid"))) return false;
	EventBindingRow->SetStringField(TEXT("source_path"), TEXT("Payload.Duration"));
	const auto FunctionOutputBinding = FunctionRows->GetObjectField(TEXT("AddedBinding"));
	FunctionRows->RemoveField(TEXT("AddedBinding"));
	if (!VerifyRejectedFunction(TEXT("state_tree_function_binding_missing"))) return false;
	FunctionRows->SetObjectField(TEXT("AddedBinding"), FunctionOutputBinding);
	const auto OuterFunctionRow = FunctionRows->GetObjectField(TEXT("AddedSum"));
	OuterFunctionRow->SetStringField(TEXT("parent"), TEXT("NestedSum"));
	FunctionOutputBinding->SetStringField(TEXT("target"), TEXT("NestedSum"));
	FunctionOutputBinding->SetStringField(TEXT("target_path"), TEXT("Left"));
	if (!VerifyRejectedFunction(TEXT("state_tree_function_binding_cycle"))) return false;
	OuterFunctionRow->SetStringField(TEXT("parent"), TEXT("AddedDelay"));
	FunctionOutputBinding->SetStringField(TEXT("target"), TEXT("AddedDelay"));
	FunctionOutputBinding->SetStringField(TEXT("target_path"), TEXT("Duration"));
	const auto InnerFunctionRow = FunctionRows->GetObjectField(TEXT("NestedSum"));
	const auto InnerOutputBinding = FunctionRows->GetObjectField(TEXT("NestedOutput"));
	InnerFunctionRow->SetStringField(TEXT("parent"), TEXT("NestedSum"));
	InnerOutputBinding->SetStringField(TEXT("target"), TEXT("NestedSum"));
	if (!VerifyRejectedFunction(TEXT("state_tree_function_binding_cycle"))) return false;
	InnerFunctionRow->SetStringField(TEXT("parent"), TEXT("AddedSum"));
	InnerOutputBinding->SetStringField(TEXT("target"), TEXT("AddedSum"));
	TSharedPtr<FJsonObject> ParameterBindingRow;
	for (const auto& Row : FunctionRows->Values)
	{
		FString Path;
		if (Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) && Path == TEXT("Alternative"))
			ParameterBindingRow = Row.Value->AsObject();
	}
	if (!ParameterBindingRow.IsValid()) return false;
	const FString ParameterSource = ParameterBindingRow->GetStringField(TEXT("source"));
	ParameterBindingRow->SetStringField(TEXT("source"), TEXT("AddedSum"));
	ParameterBindingRow->SetStringField(TEXT("source_path"), TEXT("Result"));
	if (!VerifyRejectedFunction(TEXT("state_tree_function_binding_owner_invalid"))) return false;
	ParameterBindingRow->SetStringField(TEXT("source"), ParameterSource);
	ParameterBindingRow->SetStringField(TEXT("source_path"), TEXT("Alternative"));
	const auto FunctionInitialCategories = FunctionRows->GetObjectField(TEXT("AddedSum"))->GetObjectField(TEXT("categories"));
	const auto OutputInitial = MakeShared<FJsonObject>();
	OutputInitial->SetStringField(TEXT("type"), TEXT("float")); OutputInitial->SetNumberField(TEXT("value"), 12.0);
	const auto OutputCategory = MakeShared<FJsonObject>(); OutputCategory->SetObjectField(TEXT("instance.Result"), OutputInitial);
	FunctionInitialCategories->SetObjectField(TEXT("Output"), OutputCategory);
	if (!VerifyRejectedFunction(TEXT("hjson_initial_property_unknown"))) return false;
	FunctionInitialCategories->RemoveField(TEXT("Output"));
	const auto AddedBindingRow = TypeJson->AsObject()->GetObjectField(TEXT("tree_items"))->GetObjectField(TEXT("AddedBinding"));
	AddedBindingRow->SetStringField(TEXT("target_path"), TEXT("RandomDeviation"));
	FString DuplicateBindingText;
	if (!CommandContract::Json::TryCanonicalize(TypeJson->AsObject(), DuplicateBindingText, Error)
		|| !FFileHelper::SaveStringToFile(DuplicateBindingText, *BindingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	if (!TestFalse(TEXT("two bindings cannot silently compete for one target"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
	if (!TestEqual(TEXT("duplicate binding has an explicit error"), Failure.GetCode(), FString(TEXT("state_tree_document_binding_target_duplicate")))
		|| !TestTrue(TEXT("duplicate binding keeps original object clean"), BeforeImport.IsValid() && !Tree->GetOutermost()->IsDirty())) return false;
	AddedBindingRow->SetStringField(TEXT("target_path"), TEXT("Duration"));
	for (const auto& Row : TypeJson->AsObject()->GetObjectField(TEXT("tree_items"))->Values)
	{
		FString Path;
		if (Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) && Path == TEXT("Alternative"))
			Row.Value->AsObject()->SetStringField(TEXT("source_path"), TEXT("WrongType"));
	}
	FString InvalidTypeText;
	if (!CommandContract::Json::TryCanonicalize(TypeJson->AsObject(), InvalidTypeText, Error)
		|| !FFileHelper::SaveStringToFile(InvalidTypeText, *BindingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	AddExpectedError(TEXT("Failed to compile 'StateTree"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("properties are incompatible"), EAutomationExpectedErrorFlags::Contains, 1);
	if (!TestFalse(TEXT("native compiler rejects string to float binding"), DomainAdapters::AiBehavior::TryExecuteContentGraph(
		TEXT("content_graph.import_workspace"), PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))) return false;
	if (!TestEqual(TEXT("binding type failure is reported"), Failure.GetCode(), FString(TEXT("state_tree_binding_compile_failed")))
		|| !TestTrue(TEXT("binding failure identifies fields and native reason"), Failure.GetJsonPointer().Contains(TEXT("WrongType"))
			&& Failure.GetJsonPointer().Contains(TEXT("RandomDeviation")) && Failure.GetJsonPointer().Contains(TEXT("properties are incompatible")))
		|| !TestTrue(TEXT("incompatible binding preserves original tree and disk"), BeforeImport.IsValid()
			&& !Tree->GetOutermost()->IsDirty() && UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, InitialAfter, Error)
			&& CommandContract::Json::TryCanonicalize(InitialAfter->AsObject(), InitialAfterText, Error)
			&& InitialAfterText == InitialBeforeText && FFileHelper::LoadFileToArray(InitialDiskAfter, *Filename)
			&& InitialDiskBefore == InitialDiskAfter)) return false;
	HjsonWorkspace::FHjsonDocumentPtr ColorParsed;
	TSharedPtr<FJsonValue> ColorJson;
	if (!HjsonWorkspace::TryParseHjsonDocument(ValidBindingText, BindingFile, Resources, ColorParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*ColorParsed->GetRoot(), Resources, ColorJson, Failure)) return false;
	const auto ColorRows = ColorJson->AsObject()->GetObjectField(TEXT("tree_items"));
	bool EditedEventPath = false;
	for (const auto& Row : ColorRows->Values)
	{
		FString Path;
		if (Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) && Path == TEXT("Payload.Duration"))
		{
			Row.Value->AsObject()->SetStringField(TEXT("source_path"), TEXT("Payload.RandomDeviation"));
			EditedEventPath = true;
		}
	}
	if (!TestTrue(TEXT("ordinary event payload field is edited"), EditedEventPath)) return false;
	const auto NewColor = MakeShared<FJsonObject>();
	NewColor->SetStringField(TEXT("name"), ColorCandidate->Name);
	const auto NewColorValue = MakeShared<FJsonObject>();
	NewColorValue->SetStringField(TEXT("type"), TEXT("linear_color"));
	NewColorValue->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0.1), MakeShared<FJsonValueNumber>(0.6),
		MakeShared<FJsonValueNumber>(0.9), MakeShared<FJsonValueNumber>(1.0)});
	const auto NewColorTheme = MakeShared<FJsonObject>(); NewColorTheme->SetObjectField(TEXT("color.Color"), NewColorValue);
	const auto NewColorCategories = MakeShared<FJsonObject>(); NewColorCategories->SetObjectField(TEXT("Theme"), NewColorTheme);
	NewColor->SetObjectField(TEXT("categories"), NewColorCategories);
	ColorRows->SetObjectField(TEXT("AddedColor"), NewColor);
	ColorRows->GetObjectField(TEXT("AddedState"))->SetStringField(TEXT("color_ref"), TEXT("AddedColor"));
	FString AccentKey;
	for (const auto& Row : ColorRows->Values)
	{
		FString Kind, Label;
		Row.Value->AsObject()->TryGetStringField(TEXT("kind"), Kind);
		Row.Value->AsObject()->TryGetStringField(TEXT("label"), Label);
		if (Label.IsEmpty()) Label = Row.Key;
		if (Kind == TEXT("color") && Label == TEXT("Workspace Accent"))
		{
			AccentKey = Row.Key;
			Row.Value->AsObject()->SetStringField(TEXT("label"), TEXT("Edited Accent"));
		}
	}
	if (!TestFalse(TEXT("palette entry has a readable collection key"), AccentKey.IsEmpty())) return false;
	ColorRows->GetObjectField(TEXT("UntouchedChild"))->SetStringField(TEXT("color_ref"), AccentKey);
	if (!CommandContract::Json::TryCanonicalize(ColorJson->AsObject(), ValidBindingText, Error)
		|| !FFileHelper::SaveStringToFile(ValidBindingText, *BindingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	bool EditedColor = false;
	for (const FString& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		if (!TestFalse(TEXT("old palette and raw color identity properties exit ordinary authoring"),
			Text.Contains(TEXT("editor.Colors")) || Text.Contains(TEXT("state.ColorRef")))) return false;
		if (!Text.Contains(TEXT("color.Color"))) continue;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		bool Changed = false;
		for (const auto& Category : Json->AsObject()->GetObjectField(TEXT("categories"))->Values)
		{
			const TSharedPtr<FJsonObject>* Property = nullptr;
			if (!Category.Value->AsObject()->TryGetObjectField(TEXT("color.Color"), Property)) continue;
			const auto& Components = (*Property)->GetArrayField(TEXT("value"));
			if (Components.Num() != 4 || !FMath::IsNearlyEqual(Components[0]->AsNumber(), 0.2)) continue;
			(*Property)->SetArrayField(TEXT("value"), {MakeShared<FJsonValueNumber>(0.8), MakeShared<FJsonValueNumber>(0.3),
				MakeShared<FJsonValueNumber>(0.1), MakeShared<FJsonValueNumber>(1.0)});
			Changed = EditedColor = true;
		}
		if (Changed && (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	}
	if (!TestTrue(TEXT("palette color is edited through common property values"), EditedColor)) return false;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	Tree = nullptr; Data = nullptr; Package = nullptr;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("ordinary import persists and reloads StateTree"), Tree && !BeforeImport.IsValid() && !Tree->GetOutermost()->IsDirty())) return false;
	Data = Cast<UStateTreeEditorData>(Tree->EditorData);
	RootBag = RootBagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Data);
	const auto* RestoredAlternative = RootBag->FindPropertyDescByName(TEXT("Alternative"));
	const UStateTreeState* ParameterChild = Data->SubTrees[0]->Children[0];
	const auto* RestoredSpeed = ParameterChild->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	if (!TestTrue(TEXT("root parameter edit persists with original descriptor identity"), RestoredAlternative
		&& RestoredAlternative->ID == AlternativeId && RootBag->GetValueFloat(TEXT("Alternative")).GetValue() == 44.625f)
		|| !TestTrue(TEXT("state parameter edit persists with original identity and effective override"), RestoredSpeed
			&& RestoredSpeed->ID == SpeedId && ParameterChild->IsParametersPropertyOverridden(SpeedId)
			&& ParameterChild->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue() == 310.625f)) return false;
	TestEqual(TEXT("parameter, event, function input and both function outputs retained after reload"), Data->EditorBindings.GetNumBindings(), 5);
	bool SawParameterBinding = false, SawNodeBinding = false, SawFunctionInput = false, SawEventBinding = false;
	FGuid FunctionId, FunctionInputTarget, NestedFunctionId, NestedOutputTarget;
	Data->EditorBindings.ForEachBinding([&](const FPropertyBindingBinding& Binding) {
		if (Binding.GetTargetPath().ToString() == TEXT("RandomDeviation"))
			SawParameterBinding |= Binding.GetSourcePath().ToString() == TEXT("Alternative");
		if (Binding.GetSourcePath().ToString() == TEXT("Payload.RandomDeviation"))
			SawEventBinding = Binding.GetSourcePath().GetStructID() == Data->SubTrees[0]->GetEventID()
				&& Binding.GetSourcePath().GetSegments().Num() == 2
				&& Binding.GetSourcePath().GetSegments()[0].GetInstanceStruct() == Data->SubTrees[0]->Tasks[0].Instance.GetScriptStruct()
				&& Binding.GetTargetPath().GetStructID() == Data->SubTrees[0]->Tasks[0].ID
				&& Binding.GetTargetPath().ToString() == TEXT("RandomDeviation");
		if (Binding.GetTargetPath().ToString() == TEXT("Duration"))
		{
			const auto* Function = Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>();
			SawNodeBinding = Function && Binding.GetSourcePath().ToString() == TEXT("Result")
				&& Binding.GetSourcePath().GetStructID() == Function->ID
				&& Function->Node.GetScriptStruct()->GetPathName() == TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction");
			if (Function)
			{
				FunctionId = Function->ID;
				const auto* Left = FindFProperty<FFloatProperty>(Function->Instance.GetScriptStruct(), TEXT("Left"));
				const auto* Right = FindFProperty<FFloatProperty>(Function->Instance.GetScriptStruct(), TEXT("Right"));
				TestTrue(TEXT("function inputs persist through common initial properties"), Left && Right
					&& Left->GetPropertyValue_InContainer(Function->Instance.GetMemory()) == 2.f
					&& Right->GetPropertyValue_InContainer(Function->Instance.GetMemory()) == 3.f);
			}
		}
		if (Binding.GetTargetPath().ToString() == TEXT("Left"))
		{
			FunctionInputTarget = Binding.GetTargetPath().GetStructID();
			SawFunctionInput = Binding.GetSourcePath().ToString() == TEXT("Duration")
				&& Binding.GetSourcePath().GetStructID() == Data->SubTrees[0]->Tasks[0].ID;
		}
		if (Binding.GetTargetPath().ToString() == TEXT("Right"))
		{
			const auto* Nested = Binding.GetPropertyFunctionNode().GetPtr<const FStateTreeEditorNode>();
			if (Nested && Binding.GetSourcePath().ToString() == TEXT("Result") && Binding.GetSourcePath().GetStructID() == Nested->ID
				&& Nested->Node.GetScriptStruct()->GetPathName() == TEXT("/Script/StateTreeModule.StateTreeAddFloatPropertyFunction"))
			{
				NestedFunctionId = Nested->ID;
				NestedOutputTarget = Binding.GetTargetPath().GetStructID();
			}
		}
	});
	TestTrue(TEXT("parameter, function and function-input bindings persist"), SawParameterBinding && SawNodeBinding
		&& SawFunctionInput && FunctionId.IsValid() && FunctionInputTarget == FunctionId);
	TestTrue(TEXT("edited event binding retains native instance type and endpoint identity after reload"), SawEventBinding);
	TestTrue(TEXT("nested function output binds to its containing function after reload"), NestedFunctionId.IsValid()
		&& NestedFunctionId != FunctionId && NestedOutputTarget == FunctionId);
	if (!TestTrue(TEXT("native hierarchy retained after reload"), Data && Data->SubTrees.Num() == 1
		&& Data->SubTrees[0]->Children.Num() == 2 && Data->SubTrees[0]->Tasks.Num() == 1)) return false;
	TestEqual(TEXT("state label edit persisted"), Data->SubTrees[0]->Name, FName(TEXT("ImportedRoot")));
	const auto* ReloadedAccent = Data->FindColor(Data->SubTrees[0]->ColorRef);
	if (!TestTrue(TEXT("palette edits and readable state selection preserve native identity after reload"),
		Data->SubTrees[0]->ColorRef == AccentRef && ReloadedAccent
		&& Data->SubTrees[0]->Children[0]->ColorRef == AccentRef
		&& ReloadedAccent->DisplayName == TEXT("Edited Accent")
		&& ReloadedAccent->Color.Equals(FLinearColor(0.8f, 0.3f, 0.1f, 1.f)))) return false;
	TestEqual(TEXT("task instance edit persisted"), Duration->GetPropertyValue_InContainer(Data->SubTrees[0]->Tasks[0].Instance.GetMemory()), 9.75f);
	TestEqual(TEXT("untouched child property persisted"), Data->SubTrees[0]->Children[0]->Description, FString(TEXT("StateTreeUntouchedMarker")));
	UStateTreeState* AddedState = Data->SubTrees[0]->Children[1];
	const auto* AddedColor = Data->FindColor(AddedState->ColorRef);
	if (!TestTrue(TEXT("queried color creation and same-import reference persist with native initial values"),
		Data->Colors.Num() == 3 && AddedColor && AddedState->ColorRef.ID.IsValid() && AddedState->ColorRef != AccentRef
		&& AddedColor->DisplayName == TEXT("AddedColor") && AddedColor->Color.Equals(FLinearColor(0.1f, 0.6f, 0.9f, 1.f)))) return false;
	if (!TestTrue(TEXT("new state and queried task persisted with native ownership"), AddedState->Name == TEXT("AddedState")
		&& AddedState->GetOuter() == Data->SubTrees[0] && AddedState->Parent == Data->SubTrees[0]
		&& AddedState->Tasks.Num() == 1 && AddedState->Tasks[0].Node.GetScriptStruct() == Delay)) return false;
	TestEqual(TEXT("new task initial value persists in the same creation"), Duration->GetPropertyValue_InContainer(AddedState->Tasks[0].Instance.GetMemory()), 6.5f);
	const auto* RandomDeviation = FindFProperty<FFloatProperty>(AddedState->Tasks[0].Instance.GetScriptStruct(), TEXT("RandomDeviation"));
	TestTrue(TEXT("unprovided task initial value retains its native default"), RandomDeviation
		&& RandomDeviation->GetPropertyValue_InContainer(AddedState->Tasks[0].Instance.GetMemory()) == 0.f);
	Data->EditorBindings.ForEachBinding([&](const FPropertyBindingBinding& Binding) {
		const FGuid ExpectedTarget = Binding.GetSourcePath().GetStructID() == Data->SubTrees[0]->GetEventID()
			? Data->SubTrees[0]->Tasks[0].ID
			: ((Binding.GetTargetPath().ToString() == TEXT("Left") || Binding.GetTargetPath().ToString() == TEXT("Right"))
				? FunctionId : AddedState->Tasks[0].ID);
		TestEqual(TEXT("binding endpoint resolves its intended task or function after reload"),
			Binding.GetTargetPath().GetStructID(), ExpectedTarget);
	});
	TestTrue(TEXT("untouched node keeps its optional name while new node has its instance label"),
		Data->SubTrees[0]->Tasks[0].Node.Get<FStateTreeNodeBase>().Name.IsNone()
		&& AddedState->Tasks[0].Node.Get<FStateTreeNodeBase>().Name == TEXT("AddedDelay"));
	if (!TestTrue(TEXT("new transition resolves existing state from new parent"), AddedState->Transitions.Num() == 1
		&& AddedState->Transitions[0].State.ID == Data->SubTrees[0]->Children[0]->ID
		&& AddedState->Transitions[0].State.LinkType == EStateTreeTransitionType::GotoState
		&& AddedState->Transitions[0].Trigger == EStateTreeTransitionTrigger::OnStateCompleted)) return false;
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture ReadCapture;
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
	if (!DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Tree, ReadCapture, ReadError))
	{ AddError(ReadError.Code + TEXT(":") + ReadError.Detail); return false; }
	TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot> ReadSnapshot;
	const auto ReadResources = HjsonWorkspace::MakeSemanticWorkspaceResourceTrackerOwner();
	if (!DomainAdapters::ContentGraphRead::TryBuildContentGraphAuthoringSnapshot(ReadCapture, ReadResources, ReadSnapshot, ReadError))
	{ AddError(ReadError.Code + TEXT(":") + ReadError.Detail); return false; }
	const auto* ReadTask = ReadCapture.Items.FindByPredicate([](const auto& Item) {
		return Item.SemanticIdentity.Selectors.FindRef(TEXT("public_name")) == TEXT("AddedDelay");
	});
	const auto* ReadTransition = ReadCapture.Items.FindByPredicate([](const auto& Item) { return Item.ItemKind == TEXT("state_tree_transition"); });
	if (!TestTrue(TEXT("content query captures named nodes and transition state"), ReadTask && ReadTask->State.IsValid()
		&& ReadTransition && ReadTransition->State.IsValid() && ReadCapture.CanonicalRevision.IsSet())) return false;
	bool ReadDuration = false, ReadUntouched = false;
	const FString OriginalTaskId = Data->SubTrees[0]->Tasks[0].ID.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString UntouchedStateId = Data->SubTrees[0]->Children[0]->ID.ToString(EGuidFormats::DigitsWithHyphensLower);
	for (const auto& Property : ReadCapture.Properties)
	{
		TSharedPtr<FJsonValue> Value;
		if (!Property.State.Value || !StateSemantic::CanonicalJson::TryToJsonValue(*Property.State.Value, Value, Error)) return false;
		// Scoped properties retain the shared PropertyValueCodec value envelope.
		const auto Literal = Value->Type == EJson::Object ? Value->AsObject()->TryGetField(TEXT("literal")) : nullptr;
		ReadDuration |= Property.OwnerId == OriginalTaskId && Property.FieldId == TEXT("instance.Duration")
			&& Literal && Literal->Type == EJson::Number && Literal->AsNumber() == 9.75;
		ReadUntouched |= Property.OwnerId == UntouchedStateId && Property.FieldId == TEXT("state.Description")
			&& Literal && Literal->Type == EJson::String && Literal->AsString() == TEXT("StateTreeUntouchedMarker");
	}
	TestTrue(TEXT("content query reads edited original task duration"), ReadDuration);
	TestTrue(TEXT("content query reads untouched child description"), ReadUntouched);
	TestFalse(TEXT("content query relations no longer have empty state"), ReadCapture.Relations.ContainsByPredicate([](const auto& Relation) { return !Relation.State.IsValid(); }));
	const FString DeleteWorkspace = Workspace + TEXT("-delete");
	TArray<UeAgentAuthoringState::FReadRequest> FunctionRequests;
	UeAgentAuthoringState::FCaptureResult FunctionSnapshot;
	if (!UeAgentStateTreeAuthoring::AppendStructuralReadRequests(*Data, PackageName, FunctionRequests, Error)
		|| !UeAgentAuthoringState::CaptureSnapshot(FunctionRequests, FunctionSnapshot)) return false;
	bool CapturedFunctionOutput = false;
	for (const auto& Entry : FunctionSnapshot.Snapshot.CopyEntries())
	{
		if (Entry.TargetIdentity.Selectors.FindRef(TEXT("node_id")) != FunctionId.ToString(EGuidFormats::DigitsWithHyphensLower)
			|| Entry.TargetIdentity.Selectors.FindRef(TEXT("property_path")) != TEXT("instance.Result")) continue;
		CapturedFunctionOutput = Entry.Value.IsValid() && Entry.NativeValue.IsValid() && !Entry.bAuthoringVisible;
	}
	if (!TestTrue(TEXT("function output stays in complete capture but is not an authoring field"), CapturedFunctionOutput)) return false;
	FunctionSnapshot.Snapshot.ReleaseNativeValues();
	FunctionRequests.Reset();
	Export->SetStringField(TEXT("workspace_path"), DeleteWorkspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode()); return false; }
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *DeleteWorkspace, TEXT("*.hjson"), true, false);
	bool RemovedTask = false, SawFunctionInputProperties = false;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (Json->Type == EJson::Object && Json->AsObject()->TryGetObjectField(TEXT("categories"), Categories))
			for (const auto& Category : (*Categories)->Values)
			{
				if (Category.Value->Type != EJson::Object) continue;
				if (!TestFalse(TEXT("ordinary property files exclude calculated function output"), Category.Value->AsObject()->HasField(TEXT("instance.Result")))) return false;
				SawFunctionInputProperties |= Category.Value->AsObject()->HasField(TEXT("instance.Left"))
					&& Category.Value->AsObject()->HasField(TEXT("instance.Right"));
			}
		if (!Normalized.EndsWith(TEXT("/structure.hjson"))) continue;
		const TSharedPtr<FJsonObject>* EmptyUnit = nullptr;
		const TSharedPtr<FJsonObject>* EmptyValues = nullptr;
		if (Json->Type == EJson::Object && Json->AsObject()->TryGetObjectField(TEXT("unit"), EmptyUnit)
			&& (*EmptyUnit)->TryGetObjectField(TEXT("values"), EmptyValues))
			if (!TestFalse(TEXT("ordinary structures exclude empty internal surface containers"), (*EmptyValues)->Values.IsEmpty())) return false;
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		if (Json->Type != EJson::Object || !Json->AsObject()->TryGetObjectField(TEXT("tree_items"), Rows)) continue;
		if (!(*Rows)->HasField(TEXT("AddedDelay"))) continue;
		(*Rows)->RemoveField(TEXT("AddedDelay"));
		if (!TestTrue(TEXT("function item is present in ordinary readback"), (*Rows)->HasField(TEXT("AddedSum")))) return false;
		(*Rows)->RemoveField(TEXT("AddedSum"));
		if (!TestTrue(TEXT("nested function appears in ordinary readback"), (*Rows)->HasField(TEXT("NestedSum")))) return false;
		(*Rows)->RemoveField(TEXT("NestedSum"));
		TArray<FString> DependentBindings;
		for (const auto& Row : (*Rows)->Values)
		{
			FString Target, Path;
			if (Row.Value->AsObject()->TryGetStringField(TEXT("target"), Target) && (Target == TEXT("AddedDelay") || Target == TEXT("AddedSum"))
				&& Row.Value->AsObject()->TryGetStringField(TEXT("target_path"), Path)) DependentBindings.Add(Row.Key);
		}
		if (!TestEqual(TEXT("all four bindings depend on the deleted task and functions"), DependentBindings.Num(), 4)) return false;
		for (const auto& Key : DependentBindings) (*Rows)->RemoveField(Key);
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		RemovedTask = true;
	}
	if (!TestTrue(TEXT("ordinary function inputs remain editable"), SawFunctionInputProperties)
		|| !TestTrue(TEXT("ordinary delete removes the selected task and its binding"), RemovedTask)) return false;
	Source->SetStringField(TEXT("path"), DeleteWorkspace);
	Resolved.Reset(); Revision.Reset(); Authorization.Reset();
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)) return false;
	TWeakObjectPtr<UStateTree> BeforeDelete(Tree);
	Tree = nullptr; Data = nullptr; AddedState = nullptr;
	if (!DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
		PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetSourceUnit() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	Data = Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestTrue(TEXT("ordinary task deletion persists and reloads without deleting other items"), Data && !BeforeDelete.IsValid()
		&& !Tree->GetOutermost()->IsDirty() && Data->SubTrees.Num() == 1 && Data->SubTrees[0]->Children.Num() == 2
		&& Data->SubTrees[0]->Tasks.Num() == 1 && Data->SubTrees[0]->Children[1]->Tasks.IsEmpty()
		&& Data->EditorBindings.GetNumBindings() == 1
		&& Data->SubTrees[0]->Children[1]->Transitions.Num() == 1)) return false;
	AddedState = Data->SubTrees[0]->Children[1];
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	UStateTreeState* DirtyChild = Data->SubTrees[0]->Children[0];
	DirtyChild->Modify();
	DirtyChild->Description = TEXT("StateTreeDirtyBeforeFailure");
	Tree->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const FString RemovedState = DirtyChild->ID.ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemovedParameters = DirtyChild->Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
	const auto Items = Desired->AsObject()->GetObjectField(TEXT("items"));
	Items->RemoveField(RemovedState);
	Items->RemoveField(RemovedParameters);
	Items->RemoveField(AddedState->Transitions[0].ID.ToString(EGuidFormats::DigitsWithHyphensLower));
	TWeakObjectPtr<UStateTree> OriginalTree(Tree), RejectedTree;
	TWeakObjectPtr<UStateTreeState> OriginalChild(DirtyChild);
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("state_tree");
	LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true;
	LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("state_tree"), Desired->AsObject());
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<UStateTree>(nullptr, *ObjectPath);
		auto* ActualData = Actual ? Cast<UStateTreeEditorData>(Actual->EditorData) : nullptr;
		RejectedTree = Actual;
		TArray<uint8> ActualDisk;
		LateObserved = !OriginalTree.IsValid(true, true) && !OriginalChild.IsValid(true, true)
			&& ActualData && ActualData->SubTrees.Num() == 1 && ActualData->SubTrees[0]->Children.Num() == 1
			&& ActualData->SubTrees[0]->Children[0]->Name == TEXT("AddedState")
			&& ActualData->SubTrees[0]->Children[0]->Transitions.IsEmpty()
			&& ActualData->SubTrees[0]->Tasks.Num() == 1 && !Actual->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Rejection.Diagnostic.Code = TEXT("t208_state_tree_late_rejected");
		Rejection.Diagnostic.PublicMessage = Rejection.Diagnostic.Code;
		return false;
	};
	Tree = nullptr; Data = nullptr; DirtyChild = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late StateTree deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-late-") + Name));
	if (!TestTrue(TEXT("child deletion persisted before the injected rejection"), LateObserved)
		|| !TestEqual(TEXT("late rejection retains original diagnostic"), LateResult.Diagnostic.Code, FString(TEXT("t208_state_tree_late_rejected")))
		|| !TestTrue(TEXT("StateTree checkpoint recovery is confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	TestFalse(TEXT("rejected StateTree exits during recovery"), RejectedTree.IsValid(true, true));
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("complete StateTree and dirty child state are restored"), Tree
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error)
		&& RestoredText == DirtyBefore && Tree->GetOutermost()->IsDirty());
	TestTrue(TEXT("StateTree original disk is restored independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeGraphCompoundTest,
	"UAI.Root.T85.StateTree.GraphCompound", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeGraphCompoundTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace Composition::GraphCompound;
	using namespace Contract;
	class FCancellation final : public IUeAgentCancellationSignal { public: bool IsCancelled() const override { return false; } };
	const FString Name = TEXT("StateTree_GraphCompound_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85StateTreeWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto* Package = CreatePackage(*PackageName);
	auto* Tree = NewObject<UStateTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	auto* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
	Tree->EditorData = Data;
	Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data, NAME_None, RF_Transactional);
	auto& Root = Data->AddSubTree(TEXT("Root"));
	auto& Untouched = Root.AddChildState(TEXT("Untouched"));
	Untouched.Description = TEXT("GraphCompoundPreserve");
	auto& Defaults = Data->AddSubTree(TEXT("Defaults"));
	Defaults.Type = EStateTreeStateType::Subtree;
	Defaults.Parameters.Parameters.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	Defaults.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 29.5f);
	Untouched.Type = EStateTreeStateType::Linked;
	FStateTreeStateLink Link;
	Link.ID = Defaults.ID; Link.Name = Defaults.Name; Link.LinkType = EStateTreeTransitionType::GotoState;
	Untouched.SetLinkedState(Link);
	const auto* Speed = Untouched.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	if (!TestNotNull(TEXT("untouched native linked parameter"), Speed)) return false;
	const FGuid SpeedId = Speed->ID, ParametersId = Untouched.Parameters.ID, DefaultsId = Defaults.ID;
	Untouched.SetParametersPropertyOverridden(SpeedId, true);
	Untouched.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 52.25f);
	const auto* BagProperty = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParameterPropertyBag"));
	if (!TestNotNull(TEXT("root binding source bag"), BagProperty)) return false;
	auto* Bag = BagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Data);
	Bag->AddProperty(TEXT("Input"), EPropertyBagPropertyType::Float);
	Bag->SetValueFloat(TEXT("Input"), 90.f);
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(ParametersId, TEXT("Speed")));
	const auto Id = [](const FGuid& Guid) { return Guid.ToString(EGuidFormats::DigitsWithHyphensLower); };
	const FString RootId = TEXT("state:") + Id(Root.ID), UntouchedId = TEXT("state:") + Id(Untouched.ID);
	const FGuid NewStateGuid = FGuid::NewGuid(), NewNodeGuid = FGuid::NewGuid();
	const FString StateId = TEXT("state:") + Id(NewStateGuid);
	FStateTreeCompilerLog CompileLog;
	if (!UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog)) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Tree, *Filename, Save)) return false;
	TArray<TSharedPtr<FJsonValue>> Edits;
	auto AddEdit = [&](const FString& Identity, const FString& Kind, const TArray<FString>& Depends, const TSharedRef<FJsonObject>& Value) {
		const auto Edit = MakeShared<FJsonObject>();
		Edit->SetStringField(TEXT("identity"), Identity); Edit->SetStringField(TEXT("element_kind"), Kind);
		Edit->SetStringField(TEXT("intent"), Kind == TEXT("order") ? TEXT("update") : TEXT("create")); Edit->SetObjectField(TEXT("value"), Value);
		TArray<TSharedPtr<FJsonValue>> Dependencies;
		for (const auto& Dependency : Depends) Dependencies.Add(MakeShared<FJsonValueString>(Dependency));
		Edit->SetArrayField(TEXT("depends_on"), Dependencies); Edits.Add(MakeShared<FJsonValueObject>(Edit));
	};
	const auto State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("native_guid"), Id(NewStateGuid)); State->SetStringField(TEXT("parent_identity"), RootId);
	State->SetStringField(TEXT("payload_type_ref"), UStateTreeState::StaticClass()->GetPathName());
	AddEdit(StateId, TEXT("hierarchy"), {}, State);
	const auto Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("owner_state_ref"), StateId); Node->SetStringField(TEXT("placement"), TEXT("state_task"));
	Node->SetStringField(TEXT("node_class_ref"), TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
	AddEdit(TEXT("node:") + Id(NewNodeGuid), TEXT("item"), {StateId}, Node);
	const auto Label = MakeShared<FJsonObject>();
	Label->SetStringField(TEXT("property_path"), TEXT("Name")); Label->SetStringField(TEXT("value_type"), TEXT("name"));
	Label->SetStringField(TEXT("value"), TEXT("CreatedThroughGraphCompound"));
	AddEdit(TEXT("property:") + StateId + TEXT(":Name"), TEXT("property"), {StateId}, Label);
	const auto Order = MakeShared<FJsonObject>();
	Order->SetStringField(TEXT("owner_identity"), RootId);
	Order->SetArrayField(TEXT("ordered_identities"), {MakeShared<FJsonValueString>(StateId), MakeShared<FJsonValueString>(UntouchedId)});
	AddEdit(TEXT("order:") + RootId + TEXT(":children"), TEXT("order"), {StateId}, Order);
	const auto Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target_ref"), ObjectPath); Input->SetArrayField(TEXT("edits"), Edits);
	FString Payload, Error;
	if (!CommandContract::Json::TryCanonicalize(Input, Payload, Error)) return false;
	FGraphCompoundRuntimeConfig RuntimeConfig;

	FGraphCompoundPcgAiProductionConfig ProductionConfig;
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError RuntimeError;
	if (!TryCreatePcgAiGraphProductionRuntime(RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
	{ AddError(RuntimeError.Code); return false; }
	const auto* Profile = FindPcgAnimationGraphRouteProfile(TEXT("ai.state_tree.hierarchy.apply"));
	if (!TestNotNull(TEXT("StateTree production route"), Profile)) return false;
	TWeakObjectPtr<UStateTree> Before = Tree;
	const auto Result = Runtime->GetOperationPort()->Submit(FUeAgentOperationRequest(
		Profile->GetExecutionContract().GetDescriptorIdentity(), TEXT("state-tree:") + Name, TEXT("state-tree:") + Name,
		FUeAgentContractValue(Payload)), FUeAgentCallerContext(TEXT("test.agent"),
		{TEXT("pcg_ai_node_graph.read"), TEXT("pcg_ai_node_graph.write")}, TEXT("t208.state_tree")), MakeShared<FCancellation>());
	TSharedPtr<FJsonObject> Response;
	if (!Result.IsValid() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.GetValue().GetJsonValue()), Response)) return false;
	if (Response->HasField(TEXT("operation_error")))
	{ AddError(Result.GetValue().GetJsonValue()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	Data = Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestTrue(TEXT("GraphCompound uses persistent reload"), !Before.IsValid() && Tree && !Tree->GetOutermost()->IsDirty()
		&& Data && Data->SubTrees.Num() == 2 && Data->SubTrees[0]->Children.Num() == 2)) return false;
	const UStateTreeState* Created = Data->SubTrees[0]->Children[0];
	TestTrue(TEXT("created state, name, order and task survive shared transaction"), Created->ID == NewStateGuid
		&& Created->Name == TEXT("CreatedThroughGraphCompound") && Created->GetOuter() == Data->SubTrees[0]
		&& Created->Tasks.Num() == 1 && Created->Tasks[0].ID == NewNodeGuid);
	TestEqual(TEXT("untouched child preserved"), Data->SubTrees[0]->Children[1]->Description, FString(TEXT("GraphCompoundPreserve")));
	const UStateTreeState* Preserved = Data->SubTrees[0]->Children[1];
	const auto* PreservedSpeed = Preserved->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	TestTrue(TEXT("untouched linked parameter and native field identities survive compound edits"),
		Preserved->Parameters.ID == ParametersId && PreservedSpeed && PreservedSpeed->ID == SpeedId
		&& Preserved->Type == EStateTreeStateType::Linked && Preserved->LinkedSubtree.ID == DefaultsId);
	const auto PreservedValue = Preserved->Parameters.Parameters.GetValueFloat(TEXT("Speed"));
	TestTrue(TEXT("untouched linked override value survives compound edits"), PreservedValue.IsValid()
		&& PreservedValue.GetValue() == 52.25f && Preserved->Parameters.PropertyOverrides.Num() == 1
		&& Preserved->Parameters.PropertyOverrides.Contains(SpeedId));
	const auto* BindingSource = Data->EditorBindings.GetBindingSource(FPropertyBindingPath(ParametersId, TEXT("Speed")));
	TestTrue(TEXT("untouched binding retains original source and target"), Data->EditorBindings.GetNumBindings() == 1
		&& BindingSource && *BindingSource == FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")));
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture ReadCapture;
	DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError ReadError;
	if (!DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(*Tree, ReadCapture, ReadError))
	{ AddError(ReadError.Code + TEXT(":") + ReadError.Detail); return false; }
	const FString ParameterItemId = Id(ParametersId);
	const auto* ReadParameters = ReadCapture.Items.FindByPredicate([&](const auto& Item) { return Item.ItemId == ParameterItemId; });
	TestTrue(TEXT("content query exposes preserved parameters through canonical named item"), ReadParameters
		&& ReadParameters->State.IsValid() && !ReadParameters->SemanticIdentity.Selectors.FindRef(TEXT("public_name")).IsEmpty()
		&& ReadCapture.CanonicalRevision.IsSet());
	bool ReadSpeed = false;
	for (const auto& Property : ReadCapture.Properties)
	{
		if (Property.OwnerId != ParameterItemId || Property.FieldId != TEXT("parameters.Parameters")) continue;
		TSharedPtr<FJsonValue> Value;
		if (!Property.State.Value || !StateSemantic::CanonicalJson::TryToJsonValue(*Property.State.Value, Value, Error)) return false;
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		const TSharedPtr<FJsonObject>* Field = nullptr;
		if (Value->Type == EJson::Object && Value->AsObject()->TryGetObjectField(TEXT("fields"), Fields)
			&& (*Fields)->TryGetObjectField(TEXT("Speed"), Field))
		{
			double Literal = 0;
			ReadSpeed = (*Field)->TryGetNumberField(TEXT("literal"), Literal) && Literal == 52.25;
		}
	}
	TestTrue(TEXT("content query reads retained linked override from shared property codec"), ReadSpeed);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeSchemaSettingsTest,
	"UAI.Root.T85.StateTree.SchemaSettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeSchemaSettingsTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("StateTree_Schema_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85StateTreeWorkspace/") + Name;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto* Package = CreatePackage(*PackageName);
	auto* Tree = NewObject<UStateTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	auto* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
	Tree->EditorData = Data;
	Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data, NAME_None, RF_Transactional);
	auto& Root = Data->AddSubTree(TEXT("Root"));
	UScriptStruct* Delay = FindObject<UScriptStruct>(nullptr, TEXT("/Script/StateTreeModule.StateTreeDelayTask"));
	if (!TestNotNull(TEXT("schema binding target task type"), Delay)) return false;
	auto& Task = Root.Tasks.AddDefaulted_GetRef();
	Task.ID = FGuid::NewGuid(); Task.Node.InitializeAs(Delay);
	Task.Node.GetMutable<FStateTreeNodeBase>().Name = TEXT("ContextTarget");
	Task.Instance.InitializeAs(CastChecked<UScriptStruct>(Task.Node.Get<FStateTreeNodeBase>().GetInstanceDataType()));
	const FGuid TaskId = Task.ID, ActorId = Data->Schema->GetContextDataDescs()[0].ID;
	auto* ContextProperty = FindFProperty<FObjectPropertyBase>(Data->Schema->GetClass(), TEXT("ContextActorClass"));
	if (!TestNotNull(TEXT("editable native schema context class"), ContextProperty)) return false;
	{
		auto* Native = NewObject<UStateTreeAIComponentSchema>();
		ContextProperty->SetObjectPropertyValue_InContainer(Native, ACharacter::StaticClass());
		FEditPropertyChain Chain; Chain.AddTail(ContextProperty);
		FPropertyChangedEvent Event(ContextProperty, EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(Chain, Event);
		Native->PostEditChangeChainProperty(ChainEvent);
		if (!TestTrue(TEXT("native schema edit updates bindable context type"),
			static_cast<const UStateTreeSchema*>(Native)->GetContextDataDescs()[0].Struct == ACharacter::StaticClass())) return false;
	}
	FStateTreeCompilerLog CompileLog;
	if (!TestTrue(TEXT("schema fixture compiles"), UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog))) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Tree, *Filename, Save)) return false;
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/state-tree-common-r1/workspace") / Name);
	FString Params, Payload, Error;
	HjsonWorkspace::FWorkspaceError Failure;
	bool ReadOnly = false;
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName); Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(TEXT("schema export: ") + Failure.GetCode()); return false; }
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool Edited = false;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (!Json->AsObject()->TryGetObjectField(TEXT("categories"), Categories)) continue;
		for (const auto& Category : (*Categories)->Values)
		{
			const TSharedPtr<FJsonObject>* Field = nullptr;
			if (!Category.Value->AsObject()->TryGetObjectField(TEXT("schema.ContextActorClass"), Field)) continue;
			(*Field)->SetStringField(TEXT("value"), ACharacter::StaticClass()->GetPathName());
			Edited = true;
		}
		if (!Edited) continue;
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		break;
	}
	if (!TestTrue(TEXT("schema class is an ordinary editable property"), Edited)) return false;
	const auto Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("kind"), TEXT("directory")); Input->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	FString Revision, Authorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Input, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	const auto Import = MakeShared<FJsonObject>();
	Import->SetStringField(TEXT("target"), PackageName); Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	TWeakObjectPtr<UStateTree> Before = Tree;
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(TEXT("schema import: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	Data = Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestTrue(TEXT("schema change saves and independently reloads"), !Before.IsValid() && Data
		&& Data->Schema && !Tree->GetOutermost()->IsDirty())) return false;
	TestTrue(TEXT("schema class property and bindable context agree after reload"),
		ContextProperty->GetObjectPropertyValue_InContainer(Data->Schema) == ACharacter::StaticClass()
		&& Data->Schema->GetContextDataDescs()[0].Struct == ACharacter::StaticClass());
	if (HasAnyErrors()) return false;
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	ContextProperty->SetObjectPropertyValue_InContainer(Data->Schema, APawn::StaticClass());
	FEditPropertyChain Chain; Chain.AddTail(ContextProperty);
	FPropertyChangedEvent Event(ContextProperty, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Data->Schema->PostEditChangeChainProperty(ChainEvent);
	Data->SubTrees[0]->Description = TEXT("Dirty schema restoration marker");
	Tree->MarkPackageDirty();
	TSharedPtr<FJsonValue> DirtyDocument;
	FString DirtyBefore;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, DirtyDocument, Error)
		|| !CommandContract::Json::TryCanonicalize(DirtyDocument->AsObject(), DirtyBefore, Error)) return false;
	const auto Desired = UeAgentAuthoringState::CloneCanonicalValue(DirtyDocument);
	const auto& Properties = Desired->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(TEXT("schema_settings"))
		->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"));
	const auto* ContextDescriptor = Properties.FindByPredicate([](const auto& Value) {
		return Value->AsObject()->GetStringField(TEXT("property_path")) == TEXT("schema.ContextActorClass");
	});
	if (!TestNotNull(TEXT("schema class descriptor for late change"), ContextDescriptor)) return false;
	(*ContextDescriptor)->AsObject()->SetStringField(TEXT("value"), ACharacter::StaticClass()->GetPathName());
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("state_tree"); LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("state_tree"), Desired->AsObject());
	TWeakObjectPtr<UStateTree> BeforeLate = Tree, Rejected;
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& LateFailure) {
		auto* Actual = LoadObject<UStateTree>(nullptr, *ObjectPath);
		auto* ActualData = Actual ? Cast<UStateTreeEditorData>(Actual->EditorData) : nullptr;
		TArray<uint8> ChangedDisk;
		LateObserved = !BeforeLate.IsValid() && ActualData && ActualData->Schema && !Actual->GetPackage()->IsDirty()
			&& ContextProperty->GetObjectPropertyValue_InContainer(ActualData->Schema) == ACharacter::StaticClass()
			&& ActualData->Schema->GetContextDataDescs()[0].Struct == ACharacter::StaticClass()
			&& FFileHelper::LoadFileToArray(ChangedDisk, *Filename) && ChangedDisk != DiskBefore;
		Rejected = Actual;
		LateFailure.Diagnostic.Code = TEXT("t208_schema_late_rejected"); LateFailure.Diagnostic.PublicMessage = LateFailure.Diagnostic.Code;
		return false;
	};
	Tree = nullptr; Data = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late schema change rejected"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-schema-late-") + Name));
	if (!TestTrue(TEXT("new schema context observed after save and reload"), LateObserved)
		|| !TestEqual(TEXT("late schema diagnostic retained"), LateResult.Diagnostic.Code, FString(TEXT("t208_schema_late_rejected")))
		|| !TestTrue(TEXT("schema checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	if (!TestTrue(TEXT("complete dirty schema document restored"), Tree && Tree->GetPackage()->IsDirty()
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error) && RestoredText == DirtyBefore)) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	TestTrue(TEXT("dirty schema property and derived context both restored"),
		ContextProperty->GetObjectPropertyValue_InContainer(Data->Schema) == APawn::StaticClass()
		&& Data->Schema->GetContextDataDescs()[0].Struct == APawn::StaticClass());
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("original Character schema disk restored independently of dirty Pawn state"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskBefore == DiskAfter);
	TestFalse(TEXT("rejected schema target released"), Rejected.IsValid(true, true));
	if (HasAnyErrors()) return false;
	{
		TStrongObjectPtr<UStateTree> Native(DuplicateObject<UStateTree>(Tree, GetTransientPackage()));
		auto* NativeData = CastChecked<UStateTreeEditorData>(Native->EditorData);
		FPropertyBindingDataView PawnView;
		FPropertyBindingPath CharacterPath(ActorId);
		if (!NativeData->GetBindingDataViewByID(ActorId, PawnView) || !CharacterPath.FromString(TEXT("CrouchedEyeHeight"))) return false;
		FString NativeError;
		if (!TestFalse(TEXT("Character-only source field is absent from Pawn context"), CharacterPath.UpdateSegmentsFromValue(PawnView, &NativeError))) return false;
		ContextProperty->SetObjectPropertyValue_InContainer(NativeData->Schema, ACharacter::StaticClass());
		FEditPropertyChain NativeChain; NativeChain.AddTail(ContextProperty);
		FPropertyChangedEvent NativeEvent(ContextProperty, EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent NativeChainEvent(NativeChain, NativeEvent);
		NativeData->Schema->PostEditChangeChainProperty(NativeChainEvent);
		FPropertyBindingDataView CharacterView;
		if (!NativeData->GetBindingDataViewByID(ActorId, CharacterView)
			|| !CharacterPath.UpdateSegmentsFromValue(CharacterView, &NativeError)) return false;
		NativeData->AddPropertyBinding(CharacterPath,
			FPropertyBindingPath(NativeData->SubTrees[0]->Tasks[0].ID, TEXT("Duration")));
		FStateTreeCompilerLog NativeLog;
		if (!TestTrue(TEXT("native schema change and Character Float binding compile together"),
			UStateTreeEditingSubsystem::CompileStateTree(Native.Get(), NativeLog))) return false;
	}
	const FString BindingWorkspace = Workspace + TEXT("-binding");
	Export->SetStringField(TEXT("workspace_path"), BindingWorkspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryResult Candidates;
	QueryOwners::AiBehavior::FAiBehaviorCandidateQueryError CandidateError;
	if (!QueryOwners::AiBehavior::TryQueryAiBehaviorCandidates(*Tree, TEXT("binding"), Candidates, CandidateError))
	{ AddError(CandidateError.Code); return false; }
	const auto* BindingCandidate = Candidates.Candidates.FindByPredicate([](const auto& Candidate) {
		return Candidate.Kind == TEXT("state_tree_binding");
	});
	if (!TestNotNull(TEXT("schema binding uses formal creation candidate"), BindingCandidate)) return false;
	Files.Reset();
	IFileManager::Get().FindFilesRecursive(Files, *BindingWorkspace, TEXT("*.hjson"), true, false);
	bool ChangedClass = false, AddedBinding = false;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		bool ChangedFile = false;
		const TSharedPtr<FJsonObject>* Categories = nullptr;
		if (Json->AsObject()->TryGetObjectField(TEXT("categories"), Categories))
			for (const auto& Category : (*Categories)->Values)
			{
				const TSharedPtr<FJsonObject>* Field = nullptr;
				if (!Category.Value->AsObject()->TryGetObjectField(TEXT("schema.ContextActorClass"), Field)) continue;
				(*Field)->SetStringField(TEXT("value"), ACharacter::StaticClass()->GetPathName());
				ChangedClass = ChangedFile = true;
			}
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		if (Json->AsObject()->TryGetObjectField(TEXT("tree_items"), Rows)
			&& (*Rows)->HasField(TEXT("Actor")) && (*Rows)->HasField(TEXT("ContextTarget")))
		{
			const auto Binding = MakeShared<FJsonObject>();
			Binding->SetStringField(TEXT("name"), BindingCandidate->Name);
			Binding->SetStringField(TEXT("source"), TEXT("Actor"));
			Binding->SetStringField(TEXT("source_path"), TEXT("CrouchedEyeHeight"));
			Binding->SetStringField(TEXT("target"), TEXT("ContextTarget"));
			Binding->SetStringField(TEXT("target_path"), TEXT("Duration"));
			(*Rows)->SetObjectField(TEXT("ContextHeightBinding"), Binding);
			AddedBinding = ChangedFile = true;
		}
		if (ChangedFile && (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	}
	if (!TestTrue(TEXT("same workspace changes schema and adds named context binding"), ChangedClass && AddedBinding)) return false;
	Input->SetStringField(TEXT("path"), BindingWorkspace);
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Input, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
	Import->SetStringField(TEXT("expected_revision"), Revision);
	Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
	Import->RemoveField(TEXT("authorization_id"));
	if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
	Before = Tree;
	if (!CommandContract::Json::TryCanonicalize(Import, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(TEXT("schema and context binding import: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	Data = Tree ? Cast<UStateTreeEditorData>(Tree->EditorData) : nullptr;
	if (!TestTrue(TEXT("schema and binding save and reload together"), !Before.IsValid() && Data
		&& Data->Schema && !Tree->GetPackage()->IsDirty())) return false;
	const auto* SourcePath = Data->EditorBindings.GetBindingSource(FPropertyBindingPath(TaskId, TEXT("Duration")));
	FPropertyBindingDataView ContextView;
	FPropertyBindingPath ResolvedPath(ActorId);
	TestTrue(TEXT("persisted binding resolves against new Character context"), Data->EditorBindings.GetNumBindings() == 1
		&& SourcePath && *SourcePath == FPropertyBindingPath(ActorId, TEXT("CrouchedEyeHeight"))
		&& Data->Schema->GetContextDataDescs()[0].Struct == ACharacter::StaticClass()
		&& Data->GetBindingDataViewByID(ActorId, ContextView) && ResolvedPath.FromString(TEXT("CrouchedEyeHeight"))
		&& ResolvedPath.UpdateSegmentsFromValue(ContextView, &Error));
	return !HasAnyErrors();
}
