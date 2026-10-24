#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeLinkedAssetTest,
	"UAI.Root.T85.StateTree.LinkedAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeLinkedAssetTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("StateTree_LinkedAsset_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RootPath = TEXT("/Game/UAI/Transient/T85StateTreeLinkedAsset/");
	const FString SourcePackage = RootPath + Name + TEXT("_Source"), TargetPackage = RootPath + Name + TEXT("_Target");
	const FString SourcePath = SourcePackage + TEXT(".") + Name + TEXT("_Source");
	const FString TargetPath = TargetPackage + TEXT(".") + Name + TEXT("_Target");
	ON_SCOPE_EXIT
	{
		for (const auto& Path : {TargetPath, SourcePath})
			if (UObject* Asset = LoadObject<UObject>(nullptr, *Path)) ObjectTools::DeleteObjectsUnchecked({Asset});
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, TargetPackage);
	};
	auto CreateTree = [](const FString& PackagePath) {
		auto* Tree = NewObject<UStateTree>(CreatePackage(*PackagePath), *FPackageName::GetShortName(PackagePath),
			RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Tree);
		auto* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
		Tree->EditorData = Data;
		Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data, NAME_None, RF_Transactional);
		Data->AddSubTree(TEXT("Root"));
		return Tree;
	};
	auto RootBag = [](UStateTreeEditorData& Data) {
		const auto* Property = FindFProperty<FStructProperty>(Data.GetClass(), TEXT("RootParameterPropertyBag"));
		return Property ? Property->ContainerPtrToValuePtr<FInstancedPropertyBag>(&Data) : nullptr;
	};
	auto CompileAndSave = [](UStateTree& Tree) {
		FStateTreeCompilerLog Log;
		if (!UStateTreeEditingSubsystem::CompileStateTree(&Tree, Log)) return false;
		const FString Filename = FPackageName::LongPackageNameToFilename(Tree.GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Tree.GetPackage(), &Tree, *Filename, Save);
	};
	auto* Source = CreateTree(SourcePackage);
	auto* SourceData = CastChecked<UStateTreeEditorData>(Source->EditorData);
	auto* SourceBag = RootBag(*SourceData);
	if (!TestNotNull(TEXT("source root bag"), SourceBag)) return false;
	SourceBag->AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	SourceBag->SetValueFloat(TEXT("Speed"), 22.5f);
	if (!TestTrue(TEXT("linked source compiled and saved before use"), CompileAndSave(*Source))) return false;
	const auto* SourceField = Source->GetDefaultParameters().FindPropertyDescByName(TEXT("Speed"));
	if (!TestNotNull(TEXT("compiled source exposes parameter"), SourceField)) return false;
	const FGuid FieldId = SourceField->ID;
	auto* Target = CreateTree(TargetPackage);
	auto* Data = CastChecked<UStateTreeEditorData>(Target->EditorData);
	auto& Linked = Data->SubTrees[0]->AddChildState(TEXT("External"));
	Linked.Type = EStateTreeStateType::LinkedAsset;
	Linked.SetLinkedStateAsset(Source);
	Linked.SetParametersPropertyOverridden(FieldId, true);
	Linked.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 47.5f);
	const FGuid StateId = Linked.ID;
	auto* TargetBag = RootBag(*Data);
	if (!TestNotNull(TEXT("target root bag"), TargetBag)) return false;
	TargetBag->AddProperty(TEXT("Input"), EPropertyBagPropertyType::Float);
	TargetBag->SetValueFloat(TEXT("Input"), 90.f);
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(Linked.Parameters.ID, TEXT("Speed")));
	if (!TestTrue(TEXT("target compiled and saved"), CompileAndSave(*Target))) return false;
	UE::UAI::Contract::FDiagnostic Error, Params, Payload, SourceBefore;
	TSharedPtr<FJsonValue> SourceDocument;
	if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)
		|| !UeAgentStateTreeAuthoring::ReadTreeDocument(*Source, SourceDocument, Error)
		|| !CommandContract::Json::TryCanonicalize(SourceDocument->AsObject(), SourceBefore, Error)) return false;
	const FString SourceFilename = FPackageName::LongPackageNameToFilename(SourcePackage, FPackageName::GetAssetPackageExtension());
	TArray<uint8> SourceDiskBefore;
	if (!FFileHelper::LoadFileToArray(SourceDiskBefore, *SourceFilename)) return false;
	FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/state-tree-common-r1/workspace") / Name);
	HjsonWorkspace::FWorkspaceError Failure;
	bool ReadOnly = false;
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), TargetPackage); Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			TargetPackage, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(TEXT("linked asset export: ") + Failure.GetCode()); return false; }
	const FString File = Workspace / TEXT("structures/state_tree/items/external-parameters/properties.hjson");
	FString Text;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	HjsonWorkspace::FHjsonDocumentPtr Parsed;
	TSharedPtr<FJsonValue> Json;
	if (!FFileHelper::LoadFileToString(Text, *File)
		|| !HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
	Json->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"))
		->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Speed"), 61.25);
	if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
		|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto ImportWorkspace = [&]() {
		const auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("kind"), TEXT("directory")); Input->SetStringField(TEXT("path"), Workspace);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
		FString Revision, Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Input, TargetPackage, Resources, Resolved, Revision, Authorization, Failure)) return false;
		const auto Import = MakeShared<FJsonObject>();
		Import->SetStringField(TEXT("target"), TargetPackage); Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		return CommandContract::Json::TryCanonicalize(Import, Params, Error)
			&& DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				TargetPackage, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure);
	};
	TWeakObjectPtr<UStateTree> Before = Target;
	if (!ImportWorkspace())
	{ AddError(TEXT("linked asset import: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Target = LoadObject<UStateTree>(nullptr, *TargetPath);
	if (!TestTrue(TEXT("linked asset target saved and reloaded"), Target && !Before.IsValid() && !Target->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Target->EditorData);
	const auto* Actual = Data->GetStateByID(StateId);
	Source = LoadObject<UStateTree>(nullptr, *SourcePath);
	if (!TestTrue(TEXT("linked external asset reference retained"), Actual && Source && Actual->LinkedAsset == Source)) return false;
	const auto* ActualField = Actual->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	TestTrue(TEXT("external parameter identity retained"), ActualField && ActualField->ID == FieldId);
	TestEqual(TEXT("external overridden value persisted"), Actual->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 61.25f);
	TestTrue(TEXT("external override retained"), Actual->Parameters.PropertyOverrides.Num() == 1 && Actual->Parameters.PropertyOverrides.Contains(FieldId));
	TestEqual(TEXT("external binding retained"), Data->EditorBindings.GetNumBindings(), 1);
	if (HasAnyErrors()) return false;
	Workspace += TEXT("-cancel");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			TargetPackage, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	bool Cancelled = false;
	for (const auto& Candidate : Files)
	{
		FString Normalized = Candidate; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		if (!FFileHelper::LoadFileToString(Text, *Candidate)
			|| !HjsonWorkspace::TryParseHjsonDocument(Text, Candidate, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (!Json->AsObject()->TryGetObjectField(TEXT("tree_items"), Rows)
			|| !(*Rows)->TryGetObjectField(TEXT("External"), Row)) continue;
		if (!TestEqual(TEXT("external override appears in ordinary row"), (*Row)->GetArrayField(TEXT("parameter_overrides")).Num(), 1)) return false;
		(*Row)->SetArrayField(TEXT("parameter_overrides"), {});
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *Candidate, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		Cancelled = true;
	}
	if (!TestTrue(TEXT("external override cancelled in ordinary HJSON"), Cancelled)) return false;
	Before = Target;
	if (!ImportWorkspace())
	{ AddError(TEXT("linked asset cancellation: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Target = LoadObject<UStateTree>(nullptr, *TargetPath);
	if (!TestTrue(TEXT("external cancellation saved and reloaded"), Target && !Before.IsValid() && !Target->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Target->EditorData);
	Actual = Data->GetStateByID(StateId);
	Source = LoadObject<UStateTree>(nullptr, *SourcePath);
	if (!TestTrue(TEXT("external reference survives cancellation"), Actual && Source && Actual->LinkedAsset == Source)) return false;
	ActualField = Actual->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	TestTrue(TEXT("external field identity survives cancellation"), ActualField && ActualField->ID == FieldId);
	TestEqual(TEXT("cancellation restores compiled external default"), Actual->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 22.5f);
	TestTrue(TEXT("external override inventory cleared"), Actual->Parameters.PropertyOverrides.IsEmpty());
	TestEqual(TEXT("cancelled external binding removed"), Data->EditorBindings.GetNumBindings(), 0);
	if (HasAnyErrors()) return false;
	const FString TargetFilename = FPackageName::LongPackageNameToFilename(TargetPackage, FPackageName::GetAssetPackageExtension());
	TArray<uint8> TargetDiskBefore;
	if (!FFileHelper::LoadFileToArray(TargetDiskBefore, *TargetFilename)) return false;
	auto* DirtyLinked = Data->GetMutableStateByID(StateId);
	DirtyLinked->SetParametersPropertyOverridden(FieldId, true);
	DirtyLinked->Parameters.Parameters.SetValueFloat(TEXT("Speed"), 76.25f);
	DirtyLinked->Description = TEXT("Dirty external reference before late rejection");
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(DirtyLinked->Parameters.ID, TEXT("Speed")));
	Target->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Target, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const FString StateKey = StateId.ToString(EGuidFormats::DigitsWithHyphensLower);
	Desired->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(StateKey)->SetArrayField(TEXT("parameter_overrides"), {});
	const auto Resets = MakeShared<FJsonObject>();
	Resets->SetArrayField(StateKey, {MakeShared<FJsonValueString>(TEXT("Speed"))});
	Desired->AsObject()->SetObjectField(TEXT("authoring_parameter_resets"), Resets);
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("state_tree"); LateRequest.AssetPath = TargetPackage;
	LateRequest.bSave = true; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("state_tree"), Desired->AsObject());
	TWeakObjectPtr<UStateTree> Original(Target), Rejected;
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Reloaded = LoadObject<UStateTree>(nullptr, *TargetPath);
		auto* ReloadedData = Reloaded ? Cast<UStateTreeEditorData>(Reloaded->EditorData) : nullptr;
		const auto* ReloadedState = ReloadedData ? ReloadedData->GetStateByID(StateId) : nullptr;
		auto* External = LoadObject<UStateTree>(nullptr, *SourcePath);
		Rejected = Reloaded;
		TArray<uint8> SavedDisk;
		LateObserved = !Original.IsValid(true, true) && ReloadedState && External
			&& ReloadedState->LinkedAsset == External && !Reloaded->GetPackage()->IsDirty()
			&& ReloadedState->Parameters.PropertyOverrides.IsEmpty() && ReloadedData->EditorBindings.GetNumBindings() == 0
			&& ReloadedState->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue() == 22.5f
			&& FFileHelper::LoadFileToArray(SavedDisk, *TargetFilename) && SavedDisk != TargetDiskBefore;
		Rejection.ErrorCode = TEXT("t208_linked_asset_late_rejected"); Rejection.ErrorDetail = Rejection.ErrorCode;
		return false;
	};
	Target = nullptr; Data = nullptr; DirtyLinked = nullptr; Actual = nullptr; ActualField = nullptr; Source = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late external cancellation rejected"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-linked-asset-late-") + Name));
	if (!TestTrue(TEXT("external cancellation observed after save and reload"), LateObserved)
		|| !TestEqual(TEXT("external late diagnostic retained"), LateResult.ErrorCode, FString(TEXT("t208_linked_asset_late_rejected")))
		|| !TestTrue(TEXT("external checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.ErrorDetail); return false; }
	Target = LoadObject<UStateTree>(nullptr, *TargetPath);
	Source = LoadObject<UStateTree>(nullptr, *SourcePath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TestTrue(TEXT("complete external dirty state restored"), Target && Target->GetPackage()->IsDirty()
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Target, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error) && RestoredText == DirtyBefore);
	if (HasAnyErrors() || !TestNotNull(TEXT("source survives external recovery"), Source)) return false;
	Data = CastChecked<UStateTreeEditorData>(Target->EditorData);
	Actual = Data->GetStateByID(StateId);
	if (!TestTrue(TEXT("external reference restored"), Actual && Actual->LinkedAsset == Source)) return false;
	ActualField = Actual->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	TestTrue(TEXT("native external field and override identity restored"), ActualField && ActualField->ID == FieldId
		&& Actual->Parameters.PropertyOverrides.Contains(FieldId));
	TArray<uint8> TargetDiskAfter;
	TestTrue(TEXT("original target disk restored independently"), FFileHelper::LoadFileToArray(TargetDiskAfter, *TargetFilename)
		&& TargetDiskBefore == TargetDiskAfter);
	TestFalse(TEXT("rejected external target released"), Rejected.IsValid(true, true));
	FString SourceAfter;
	TArray<uint8> SourceDiskAfter;
	TestTrue(TEXT("source asset authoring remains unchanged"), !Source->GetPackage()->IsDirty()
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Source, SourceDocument, Error)
		&& CommandContract::Json::TryCanonicalize(SourceDocument->AsObject(), SourceAfter, Error) && SourceBefore == SourceAfter);
	TestTrue(TEXT("source asset disk remains unchanged"), FFileHelper::LoadFileToArray(SourceDiskAfter, *SourceFilename) && SourceDiskBefore == SourceDiskAfter);
	TestEqual(TEXT("source compiled default remains unchanged"), Source->GetDefaultParameters().GetValueFloat(TEXT("Speed")).GetValue(), 22.5f);
	return !HasAnyErrors();
}
