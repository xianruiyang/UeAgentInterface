#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AiBehavior/UeAgentAiBehaviorDomainMechanics.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeDocument.h"
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
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeLinkedParametersTest,
	"UAI.Root.T85.StateTree.LinkedParameters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeLinkedParametersTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	const FString Name = TEXT("StateTree_LinkedParameters_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85StateTreeLinkedParameters/") + Name;
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
	auto& Root = Data->AddSubTree(TEXT("Root"));
	auto& Subtree = Data->AddSubTree(TEXT("Defaults"));
	Subtree.Type = EStateTreeStateType::Subtree;
	Subtree.Parameters.Parameters.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	Subtree.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 12.5f);
	auto& Linked = Root.AddChildState(TEXT("Linked"));
	Linked.Type = EStateTreeStateType::Linked;
	FStateTreeStateLink Link;
	Link.ID = Subtree.ID; Link.Name = Subtree.Name; Link.LinkType = EStateTreeTransitionType::GotoState;
	Linked.SetLinkedState(Link);
	const FGuid LinkedId = Linked.ID;
	auto& Inherited = Root.AddChildState(TEXT("Inherited"));
	Inherited.Type = EStateTreeStateType::Linked;
	Inherited.SetLinkedState(Link);
	const FGuid InheritedId = Inherited.ID;
	const auto* Speed = Linked.Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
	if (!TestNotNull(TEXT("native linked schema is inherited"), Speed)) return false;
	const FGuid SpeedId = Speed->ID;
	Linked.SetParametersPropertyOverridden(SpeedId, true);
	Linked.Parameters.Parameters.SetValueFloat(TEXT("Speed"), 36.25f);
	const auto* BagProperty = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParameterPropertyBag"));
	if (!TestNotNull(TEXT("root source bag"), BagProperty)) return false;
	auto* Bag = BagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Data);
	Bag->AddProperty(TEXT("Input"), EPropertyBagPropertyType::Float);
	Bag->AddProperty(TEXT("Alternative"), EPropertyBagPropertyType::Float);
	Bag->SetValueFloat(TEXT("Input"), 90.f);
	Bag->SetValueFloat(TEXT("Alternative"), 91.f);
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(Linked.Parameters.ID, TEXT("Speed")));
	FStateTreeCompilerLog CompileLog;
	if (!TestTrue(TEXT("linked override and binding fixture compiles"), UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog))) return false;
	if (!TestEqual(TEXT("compile preserves valid override binding"), Data->EditorBindings.GetNumBindings(), 1)) return false;
	{
		TSharedPtr<FJsonValue> Document, RoundTrip;
		UE::UAI::Contract::FDiagnostic Detail, BeforeText, AfterText;
		if (!UeAgentReflectionAuthoring::RegisterAuthoring(Detail)
			|| !UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Document, Detail)
			|| !CommandContract::Json::TryCanonicalize(Document->AsObject(), BeforeText, Detail))
		{ AddError(TEXT("exact replay input capture: ") + Detail.ToPublicDetail()); return false; }
		Document = UeAgentAuthoringState::CloneCanonicalValue(Document);
		// Exact reconstruction must preserve state regardless of property-group order.
		const FString ParameterKey = Linked.Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
		Document->AsObject()->GetObjectField(TEXT("items"))->Values.KeySort([&](const FString& A, const FString& B) {
			if (A == ParameterKey || B == ParameterKey) return A == ParameterKey && B != ParameterKey;
			return A < B;
		});
		TStrongObjectPtr<UStateTree> Replay(NewObject<UStateTree>(GetTransientPackage()));
		if (!UeAgentStateTreeAuthoring::ApplyTreeDocument(*Replay, Document, Detail))
		{ AddError(TEXT("parameter-before-state exact replay: ") + Detail.ToPublicDetail()); return false; }
		if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Replay, RoundTrip, Detail)
			|| !CommandContract::Json::TryCanonicalize(RoundTrip->AsObject(), AfterText, Detail))
		{ AddError(TEXT("exact replay readback: ") + Detail.ToPublicDetail()); return false; }
		if (!TestTrue(TEXT("exact replay retains complete linked parameters and bindings before native compilation"), BeforeText == AfterText)) return false;
	}
	{
		TStrongObjectPtr<UStateTree> Native(DuplicateObject<UStateTree>(Tree, GetTransientPackage()));
		auto* NativeData = CastChecked<UStateTreeEditorData>(Native->EditorData);
		UStateTreeState* NativeLinked = nullptr;
		NativeData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
			if (State.Name == TEXT("Linked")) NativeLinked = &State;
			return EStateTreeVisitor::Continue;
		});
		if (!TestNotNull(TEXT("native comparison linked state"), NativeLinked)) return false;
		const auto* NativeSpeed = NativeLinked->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"));
		if (!TestNotNull(TEXT("native comparison parameter"), NativeSpeed)
			|| !TestEqual(TEXT("native comparison retains initial binding"), NativeData->EditorBindings.GetNumBindings(), 1)) return false;
		NativeLinked->SetParametersPropertyOverridden(NativeSpeed->ID, false);
		if (!TestEqual(TEXT("native cancel override removes the valid binding"), NativeData->EditorBindings.GetNumBindings(), 0)
			|| !TestEqual(TEXT("native cancel override restores subtree default"),
				NativeLinked->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 12.5f)) return false;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Tree, *Filename, Save)) return false;
	FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
		/ TEXT("tmp/TROOT-UAI-208-graph-common-foundation/state-tree-common-r1/workspace") / Name);
	FString Params, Error, Payload;
	HjsonWorkspace::FWorkspaceError Failure;
	bool ReadOnly = false;
	const auto Export = MakeShared<FJsonObject>();
	Export->SetStringField(TEXT("target"), PackageName); Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(Failure.GetCode()); return false; }
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
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		if (!Json->AsObject()->TryGetObjectField(TEXT("tree_items"), Rows)) continue;
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (!(*Rows)->TryGetObjectField(TEXT("Linked"), Row)) continue;
		if (!TestEqual(TEXT("ordinary linked override is readable"), (*Row)->GetArrayField(TEXT("parameter_overrides")).Num(), 1)) return false;
		(*Row)->SetArrayField(TEXT("parameter_overrides"), {});
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		Edited = true;
	}
	if (!TestTrue(TEXT("ordinary linked override row edited"), Edited)) return false;
	const auto ImportWorkspace = [&]() {
		const auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Workspace);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
		FString Revision, Authorization;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Source, PackageName, Resources, Resolved, Revision, Authorization, Failure)) return false;
		const auto Import = MakeShared<FJsonObject>();
		Import->SetStringField(TEXT("target"), PackageName); Import->SetStringField(TEXT("expected_revision"), Revision);
		Import->SetObjectField(TEXT("source"), Resolved->MakeNormalizedSourceObject());
		if (!Authorization.IsEmpty()) Import->SetStringField(TEXT("authorization_id"), Authorization);
		return CommandContract::Json::TryCanonicalize(Import, Params, Error)
			&& DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
				PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure);
	};
	// An explicit binding edit cannot silently disappear behind the reset side effect.
	FString BindingFile, BindingText;
	for (const auto& File : Files)
	{
		FString Normalized = File; FPaths::NormalizeFilename(Normalized);
		if (Normalized.Contains(TEXT("/.uai/"))) continue;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) return false;
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!HjsonWorkspace::TryParseHjsonDocument(Text, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		const TSharedPtr<FJsonObject>* Rows = nullptr;
		if (!Json->AsObject()->TryGetObjectField(TEXT("tree_items"), Rows)) continue;
		bool Changed = false;
		for (const auto& Row : (*Rows)->Values)
		{
			FString Path;
			if (Row.Value->AsObject()->TryGetStringField(TEXT("source_path"), Path) && Path == TEXT("Input"))
			{ Row.Value->AsObject()->SetStringField(TEXT("source_path"), TEXT("Alternative")); Changed = true; }
		}
		if (!Changed) continue;
		BindingFile = File; BindingText = Text;
		if (!CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
		break;
	}
	if (!TestFalse(TEXT("ordinary binding found for conflict probe"), BindingFile.IsEmpty())) return false;
	if (!TestFalse(TEXT("explicit binding edit conflicting with override reset is rejected"), ImportWorkspace())
		|| !TestEqual(TEXT("conflict has native authoring diagnostic"), Failure.GetCode(), FString(TEXT("state_tree_binding_rejected_by_native_authoring")))
		|| !TestTrue(TEXT("conflict leaves original binding and override unchanged"), !Package->IsDirty()
			&& Data->EditorBindings.GetNumBindings() == 1 && Linked.Parameters.PropertyOverrides.Num() == 1)) return false;
	if (!FFileHelper::SaveStringToFile(BindingText, *BindingFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	TWeakObjectPtr<UStateTree> Before(Tree);
	if (!ImportWorkspace())
	{ AddError(Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("independent persisted readback"), Tree)) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	auto* Reloaded = Data->GetStateByID(LinkedId);
	if (!TestNotNull(TEXT("linked identity survives reload"), Reloaded)) return false;
	TestFalse(TEXT("successful import unloads original object"), Before.IsValid());
	TestFalse(TEXT("persisted tree is clean"), Tree->GetOutermost()->IsDirty());
	TestEqual(TEXT("ordinary cancel override removes binding like native editor"), Data->EditorBindings.GetNumBindings(), 0);
	TestEqual(TEXT("ordinary cancel override restores subtree default"), Reloaded->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 12.5f);
	TestEqual(TEXT("ordinary cancel override clears native override inventory"), Reloaded->Parameters.PropertyOverrides.Num(), 0);
	if (HasAnyErrors()) return false;

	// The original on-disk default state differs from the dirty override state to recover.
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	auto* DirtyLinked = Data->GetMutableStateByID(LinkedId);
	DirtyLinked->Modify();
	DirtyLinked->SetParametersPropertyOverridden(SpeedId, true);
	DirtyLinked->Parameters.Parameters.SetValueFloat(TEXT("Speed"), 49.75f);
	DirtyLinked->Description = TEXT("Dirty linked override before rejection");
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(DirtyLinked->Parameters.ID, TEXT("Speed")));
	Tree->MarkPackageDirty();
	TSharedPtr<FJsonValue> Desired;
	FString DirtyBefore;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const FString StateId = LinkedId.ToString(EGuidFormats::DigitsWithHyphensLower);
	Desired->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(StateId)->SetArrayField(TEXT("parameter_overrides"), {});
	const auto Resets = MakeShared<FJsonObject>();
	Resets->SetArrayField(StateId, {MakeShared<FJsonValueString>(TEXT("Speed"))});
	Desired->AsObject()->SetObjectField(TEXT("authoring_parameter_resets"), Resets);
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.TypeId = TEXT("state_tree"); LateRequest.AssetPath = PackageName;
	LateRequest.bSave = true; LateRequest.bCompactValues = true;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("state_tree"), Desired->AsObject());
	TWeakObjectPtr<UStateTree> Original(Tree), Rejected;
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<UStateTree>(nullptr, *ObjectPath);
		auto* ActualData = Actual ? Cast<UStateTreeEditorData>(Actual->EditorData) : nullptr;
		const auto* ActualLinked = ActualData ? ActualData->GetStateByID(LinkedId) : nullptr;
		Rejected = Actual;
		TArray<uint8> SavedDisk;
		LateObserved = !Original.IsValid(true, true) && ActualLinked && !Actual->GetPackage()->IsDirty()
			&& ActualLinked->Parameters.PropertyOverrides.IsEmpty() && ActualData->EditorBindings.GetNumBindings() == 0
			&& ActualLinked->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue() == 12.5f
			&& FFileHelper::LoadFileToArray(SavedDisk, *Filename) && SavedDisk != DiskBefore;
		Rejection.ErrorCode = TEXT("t208_linked_parameter_late_rejected"); Rejection.ErrorDetail = Rejection.ErrorCode;
		return false;
	};
	Tree = nullptr; Data = nullptr; DirtyLinked = nullptr;
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late linked override reset is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-linked-late-") + Name));
	if (!TestTrue(TEXT("native override reset was saved and reloaded before rejection"), LateObserved)
		|| !TestEqual(TEXT("late diagnostic retained"), LateResult.ErrorCode, FString(TEXT("t208_linked_parameter_late_rejected")))
		|| !TestTrue(TEXT("checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.ErrorDetail); return false; }
	TestFalse(TEXT("rejected object leaves recovery"), Rejected.IsValid(true, true));
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	TSharedPtr<FJsonValue> Restored;
	FString RestoredText;
	TArray<uint8> DiskAfter;
	TestTrue(TEXT("complete dirty override, value and binding are restored without applying reset again"), Tree
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error)
		&& RestoredText == DirtyBefore && Tree->GetPackage()->IsDirty());
	TestTrue(TEXT("original disk restored independently"), FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	if (HasAnyErrors()) return false;

	// Change only the source subtree default. The unoverridden linked state must
	// inherit it while the dirty linked override and its binding remain effective.
	Workspace += TEXT("-defaults");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure))
	{ AddError(TEXT("defaults export: ") + Failure.GetCode()); return false; }
	const FString DefaultsFile = Workspace / TEXT("structures/state_tree/items/defaults-parameters/properties.hjson");
	FString DefaultsText;
	HjsonWorkspace::FHjsonDocumentPtr DefaultsParsed;
	TSharedPtr<FJsonValue> DefaultsJson;
	if (!FFileHelper::LoadFileToString(DefaultsText, *DefaultsFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(DefaultsText, DefaultsFile, Resources, DefaultsParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*DefaultsParsed->GetRoot(), Resources, DefaultsJson, Failure))
	{ AddError(TEXT("defaults property input: ") + Failure.GetCode()); return false; }
	const auto Fields = DefaultsJson->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"))->GetObjectField(TEXT("fields"));
	if (!TestEqual(TEXT("ordinary subtree default before edit"), Fields->GetNumberField(TEXT("Speed")), 12.5)) return false;
	Fields->SetNumberField(TEXT("Speed"), 27.5);
	if (!CommandContract::Json::TryCanonicalize(DefaultsJson->AsObject(), DefaultsText, Error)
		|| !FFileHelper::SaveStringToFile(DefaultsText, *DefaultsFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	const auto EditParameter = [&](const FString& Alias, const double Value, FString& OriginalText) {
		const FString File = Workspace / TEXT("structures/state_tree/items") / Alias / TEXT("properties.hjson");
		HjsonWorkspace::FHjsonDocumentPtr Parsed;
		TSharedPtr<FJsonValue> Json;
		if (!FFileHelper::LoadFileToString(OriginalText, *File)
			|| !HjsonWorkspace::TryParseHjsonDocument(OriginalText, File, Resources, Parsed, Failure)
			|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*Parsed->GetRoot(), Resources, Json, Failure)) return false;
		Json->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
			->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"))
			->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Speed"), Value);
		FString Text;
		return CommandContract::Json::TryCanonicalize(Json->AsObject(), Text, Error)
			&& FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};
	FString InheritedText;
	if (!EditParameter(TEXT("inherited-parameters"), 88.0, InheritedText)) return false;
	if (!TestFalse(TEXT("explicit non-overridden value cannot be silently discarded"), ImportWorkspace())
		|| !TestEqual(TEXT("discarded parameter edit has native diagnostic"), Failure.GetCode(),
			FString(TEXT("state_tree_parameter_edit_rejected_by_native_authoring")))) return false;
	TSharedPtr<FJsonValue> Unchanged;
	FString UnchangedText;
	// The transaction can unload/rebind even when the proposed application fails.
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	TestTrue(TEXT("rejected inherited value preserves dirty tree and original disk"),
		Tree && UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Unchanged, Error)
		&& CommandContract::Json::TryCanonicalize(Unchanged->AsObject(), UnchangedText, Error)
		&& UnchangedText == DirtyBefore && Tree->GetPackage()->IsDirty()
		&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	if (HasAnyErrors() || !FFileHelper::SaveStringToFile(InheritedText,
		*(Workspace / TEXT("structures/state_tree/items/inherited-parameters/properties.hjson")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	HjsonWorkspace::FHjsonDocumentPtr FixedParsed;
	TSharedPtr<FJsonValue> FixedJson;
	if (!HjsonWorkspace::TryParseHjsonDocument(InheritedText, TEXT("inherited parameters"), Resources, FixedParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*FixedParsed->GetRoot(), Resources, FixedJson, Failure)) return false;
	const auto FixedBag = FixedJson->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"));
	FixedBag->SetArrayField(TEXT("schema"), {});
	FixedBag->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
	FString FixedText;
	const FString InheritedFile = Workspace / TEXT("structures/state_tree/items/inherited-parameters/properties.hjson");
	if (!CommandContract::Json::TryCanonicalize(FixedJson->AsObject(), FixedText, Error)
		|| !FFileHelper::SaveStringToFile(FixedText, *InheritedFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	if (!TestFalse(TEXT("linked parameter schema cannot be edited independently"), ImportWorkspace())
		|| !TestEqual(TEXT("fixed schema diagnostic"), Failure.GetCode(), FString(TEXT("state_tree_linked_parameter_schema_fixed")))) return false;
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("fixed schema rejection preserves original dirty tree and disk"), Tree
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Unchanged, Error)
		&& CommandContract::Json::TryCanonicalize(Unchanged->AsObject(), UnchangedText, Error)
		&& UnchangedText == DirtyBefore && Tree->GetPackage()->IsDirty()
		&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore)) return false;
	if (!FFileHelper::SaveStringToFile(InheritedText, *InheritedFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Before = Tree;
	if (!ImportWorkspace())
	{ AddError(TEXT("subtree default import: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("subtree default import persists and reloads"), Tree && !Before.IsValid() && !Tree->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	const auto* InheritedAfter = Data->GetStateByID(InheritedId);
	const auto* OverriddenAfter = Data->GetStateByID(LinkedId);
	if (!TestNotNull(TEXT("inherited linked state survives"), InheritedAfter)
		|| !TestNotNull(TEXT("overridden linked state survives"), OverriddenAfter)) return false;
	TestEqual(TEXT("unoverridden linked parameter inherits changed subtree default"),
		InheritedAfter->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 27.5f);
	TestEqual(TEXT("overridden linked parameter retains its value"),
		OverriddenAfter->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 49.75f);
	TestEqual(TEXT("overridden linked binding remains"), Data->EditorBindings.GetNumBindings(), 1);
	if (HasAnyErrors()) return false;
	Workspace += TEXT("-override");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	FString OriginalParameterText;
	if (!EditParameter(TEXT("defaults-parameters"), 41.5, OriginalParameterText)
		|| !EditParameter(TEXT("linked-parameters"), 63.25, OriginalParameterText)) return false;
	if (!ImportWorkspace())
	{ AddError(TEXT("default and override import: ") + Failure.GetCode()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("simultaneous edit persisted tree"), Tree)) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	TestEqual(TEXT("simultaneous source change reaches inherited parameter"),
		Data->GetStateByID(InheritedId)->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 41.5f);
	TestEqual(TEXT("explicit overridden value survives native propagation"),
		Data->GetStateByID(LinkedId)->Parameters.Parameters.GetValueFloat(TEXT("Speed")).GetValue(), 63.25f);
	TestEqual(TEXT("simultaneous edit retains binding"), Data->EditorBindings.GetNumBindings(), 1);
	if (HasAnyErrors()) return false;
	{
		TStrongObjectPtr<UStateTree> Native(DuplicateObject<UStateTree>(Tree, GetTransientPackage()));
		auto* NativeData = CastChecked<UStateTreeEditorData>(Native->EditorData);
		UStateTreeState* NativeDefaults = nullptr;
		NativeData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
			if (State.Name == TEXT("Defaults")) NativeDefaults = &State;
			return EStateTreeVisitor::Continue;
		});
		if (!TestNotNull(TEXT("native source subtree"), NativeDefaults)) return false;
		NativeDefaults->Parameters.Parameters.RemovePropertyByName(TEXT("Speed"));
		// Native EditorMode reacts to the source parameter notification before compile.
		NativeData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
			if (State.Type == EStateTreeStateType::Linked && State.LinkedSubtree.ID == NativeDefaults->ID)
				State.UpdateParametersFromLinkedSubtree();
			return EStateTreeVisitor::Continue;
		});
		FStateTreeCompilerLog NativeLog;
		if (!TestTrue(TEXT("native source schema deletion compiles"), UStateTreeEditingSubsystem::CompileStateTree(Native.Get(), NativeLog))
			|| !TestEqual(TEXT("native compilation removes the invalid linked binding"), NativeData->EditorBindings.GetNumBindings(), 0)) return false;
	}
	Workspace += TEXT("-schema");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	const FString SchemaFile = Workspace / TEXT("structures/state_tree/items/defaults-parameters/properties.hjson");
	FString SchemaText;
	HjsonWorkspace::FHjsonDocumentPtr SchemaParsed;
	TSharedPtr<FJsonValue> SchemaJson;
	if (!FFileHelper::LoadFileToString(SchemaText, *SchemaFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(SchemaText, SchemaFile, Resources, SchemaParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*SchemaParsed->GetRoot(), Resources, SchemaJson, Failure)) return false;
	const auto SourceBag = SchemaJson->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"));
	SourceBag->SetArrayField(TEXT("schema"), {});
	SourceBag->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
	if (!CommandContract::Json::TryCanonicalize(SchemaJson->AsObject(), SchemaText, Error)
		|| !FFileHelper::SaveStringToFile(SchemaText, *SchemaFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Before = Tree;
	if (!ImportWorkspace())
	{ AddError(TEXT("source schema deletion: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("source schema deletion persists and reloads"), Tree && !Before.IsValid() && !Tree->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	for (const auto& LinkedStateId : {InheritedId, LinkedId})
	{
		const auto* State = Data->GetStateByID(LinkedStateId);
		if (!TestNotNull(TEXT("linked state survives source schema removal"), State)) return false;
		TestNull(TEXT("removed source field leaves linked schema"), State->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed")));
		TestTrue(TEXT("obsolete override leaves linked inventory"), State->Parameters.PropertyOverrides.IsEmpty());
	}
	TestEqual(TEXT("invalid linked binding leaves persisted tree"), Data->EditorBindings.GetNumBindings(), 0);
	if (HasAnyErrors() || !FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	UStateTreeState* SchemaSource = nullptr;
	Data->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
		if (State.Name == TEXT("Defaults")) SchemaSource = &State;
		return EStateTreeVisitor::Continue;
	});
	if (!TestNotNull(TEXT("dirty schema source"), SchemaSource)) return false;
	SchemaSource->Parameters.Parameters.AddProperty(TEXT("Speed"), EPropertyBagPropertyType::Float);
	SchemaSource->Parameters.Parameters.SetValueFloat(TEXT("Speed"), 55.25f);
	const FGuid SchemaSpeedId = SchemaSource->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))->ID;
	Data->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
		if (State.Type == EStateTreeStateType::Linked) State.UpdateParametersFromLinkedSubtree();
		return EStateTreeVisitor::Continue;
	});
	auto* SchemaLinked = Data->GetMutableStateByID(LinkedId);
	SchemaLinked->SetParametersPropertyOverridden(SchemaSpeedId, true);
	SchemaLinked->Parameters.Parameters.SetValueFloat(TEXT("Speed"), 77.25f);
	SchemaLinked->Description = TEXT("Dirty schema before deletion rejection");
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Input")),
		FPropertyBindingPath(SchemaLinked->Parameters.ID, TEXT("Speed")));
	Tree->MarkPackageDirty();
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Desired, Error)
		|| !CommandContract::Json::TryCanonicalize(Desired->AsObject(), DirtyBefore, Error)) return false;
	const auto SourceProperties = Desired->AsObject()->GetObjectField(TEXT("items"))
		->GetObjectField(SchemaSource->Parameters.ID.ToString(EGuidFormats::DigitsWithHyphensLower))->GetObjectField(TEXT("properties"));
	bool DeletedSourceSchema = false;
	for (const auto& Property : SourceProperties->GetArrayField(TEXT("properties")))
		if (Property->AsObject()->GetStringField(TEXT("property_path")) == TEXT("parameters.Parameters"))
		{
			const auto BagValue = Property->AsObject()->GetObjectField(TEXT("value"));
			BagValue->SetArrayField(TEXT("schema"), {});
			BagValue->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
			DeletedSourceSchema = true;
		}
	if (!TestTrue(TEXT("late source schema request changed the intended bag"), DeletedSourceSchema)) return false;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("state_tree"), Desired->AsObject());
	Original = Tree;
	LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Rejection) {
		auto* Actual = LoadObject<UStateTree>(nullptr, *ObjectPath);
		auto* ActualData = Actual ? Cast<UStateTreeEditorData>(Actual->EditorData) : nullptr;
		const auto* ActualLinked = ActualData ? ActualData->GetStateByID(LinkedId) : nullptr;
		Rejected = Actual;
		TArray<uint8> SavedDisk;
		LateObserved = !Original.IsValid(true, true) && ActualLinked && !Actual->GetPackage()->IsDirty()
			&& !ActualLinked->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed"))
			&& ActualLinked->Parameters.PropertyOverrides.IsEmpty() && ActualData->EditorBindings.GetNumBindings() == 0
			&& FFileHelper::LoadFileToArray(SavedDisk, *Filename) && SavedDisk != DiskBefore;
		Rejection.ErrorCode = TEXT("t208_linked_schema_late_rejected"); Rejection.ErrorDetail = Rejection.ErrorCode;
		return false;
	};
	Tree = nullptr; Data = nullptr; SchemaSource = nullptr; SchemaLinked = nullptr;
	TestFalse(TEXT("late schema deletion is rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-schema-late-") + Name));
	if (!TestTrue(TEXT("schema and binding deletion saved and reloaded before rejection"), LateObserved)
		|| !TestEqual(TEXT("late schema diagnostic retained"), LateResult.ErrorCode, FString(TEXT("t208_linked_schema_late_rejected")))
		|| !TestTrue(TEXT("schema checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.ErrorDetail); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	TestTrue(TEXT("schema failure restores exact dirty document independently"), Tree
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, Restored, Error)
		&& CommandContract::Json::TryCanonicalize(Restored->AsObject(), RestoredText, Error)
		&& RestoredText == DirtyBefore && Tree->GetPackage()->IsDirty());
	if (HasAnyErrors()) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	const auto* RestoredSchemaLinked = Data->GetStateByID(LinkedId);
	const auto* RestoredSchemaField = RestoredSchemaLinked
		? RestoredSchemaLinked->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed")) : nullptr;
	TestTrue(TEXT("schema recovery preserves original native field and override identity"),
		RestoredSchemaField && RestoredSchemaField->ID == SchemaSpeedId
		&& RestoredSchemaLinked->Parameters.PropertyOverrides.Contains(SchemaSpeedId));
	TestFalse(TEXT("rejected schema object leaves recovery"), Rejected.IsValid(true, true));
	TestTrue(TEXT("schema failure restores original disk independently"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	if (HasAnyErrors()) return false;
	// A non-empty migration must retire only the removed field's relationships.
	UStateTreeState* PartialSource = nullptr;
	Data->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
		if (State.Name == TEXT("Defaults")) PartialSource = &State;
		return EStateTreeVisitor::Continue;
	});
	if (!TestNotNull(TEXT("partial schema source"), PartialSource)) return false;
	PartialSource->Parameters.Parameters.AddProperty(TEXT("Keep"), EPropertyBagPropertyType::Float);
	PartialSource->Parameters.Parameters.SetValueFloat(TEXT("Keep"), 21.5f);
	const FGuid KeepId = PartialSource->Parameters.Parameters.FindPropertyDescByName(TEXT("Keep"))->ID;
	Data->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
		if (State.Type == EStateTreeStateType::Linked) State.UpdateParametersFromLinkedSubtree();
		return EStateTreeVisitor::Continue;
	});
	auto* PartialLinked = Data->GetMutableStateByID(LinkedId);
	PartialLinked->SetParametersPropertyOverridden(KeepId, true);
	PartialLinked->Parameters.Parameters.SetValueFloat(TEXT("Keep"), 84.5f);
	Data->AddPropertyBinding(FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Alternative")),
		FPropertyBindingPath(PartialLinked->Parameters.ID, TEXT("Keep")));
	Tree->MarkPackageDirty();
	if (!TestEqual(TEXT("partial schema starts with two bindings"), Data->EditorBindings.GetNumBindings(), 2)) return false;
	Workspace += TEXT("-partial");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	const FString PartialFile = Workspace / TEXT("structures/state_tree/items/defaults-parameters/properties.hjson");
	if (!FFileHelper::LoadFileToString(SchemaText, *PartialFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(SchemaText, PartialFile, Resources, SchemaParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*SchemaParsed->GetRoot(), Resources, SchemaJson, Failure)) return false;
	const auto PartialBag = SchemaJson->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"));
	auto PartialSchema = PartialBag->GetArrayField(TEXT("schema"));
	const int32 RemovedSchemaFields = PartialSchema.RemoveAll([](const TSharedPtr<FJsonValue>& Field) {
		return Field->AsObject()->GetStringField(TEXT("name")) == TEXT("Speed");
	});
	if (!TestEqual(TEXT("partial edit removes precisely Speed"), RemovedSchemaFields, 1)) return false;
	PartialBag->SetArrayField(TEXT("schema"), PartialSchema);
	PartialBag->GetObjectField(TEXT("fields"))->RemoveField(TEXT("Speed"));
	if (!CommandContract::Json::TryCanonicalize(SchemaJson->AsObject(), SchemaText, Error)
		|| !FFileHelper::SaveStringToFile(SchemaText, *PartialFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Before = Tree;
	PartialSource = nullptr; PartialLinked = nullptr;
	if (!ImportWorkspace())
	{ AddError(TEXT("partial source schema deletion: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("partial schema persisted and reloaded"), Tree && !Before.IsValid() && !Tree->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	for (const auto& PartialStateId : {InheritedId, LinkedId})
	{
		const auto* State = Data->GetStateByID(PartialStateId);
		if (!TestNotNull(TEXT("partial linked state survives"), State)) return false;
		TestNull(TEXT("partial deletion removes Speed"), State->Parameters.Parameters.FindPropertyDescByName(TEXT("Speed")));
		const auto* Kept = State->Parameters.Parameters.FindPropertyDescByName(TEXT("Keep"));
		if (!TestTrue(TEXT("surviving linked field retains native identity"), Kept && Kept->ID == KeepId)) return false;
		const bool IsOverridden = PartialStateId == LinkedId;
		TestEqual(TEXT("surviving field retains inherited or overridden value"),
			State->Parameters.Parameters.GetValueFloat(TEXT("Keep")).GetValue(), IsOverridden ? 84.5f : 21.5f);
		TestEqual(TEXT("only surviving override remains"), State->Parameters.PropertyOverrides.Num(), IsOverridden ? 1 : 0);
		if (IsOverridden) TestTrue(TEXT("surviving override retains identity"), State->Parameters.PropertyOverrides.Contains(KeepId));
	}
	TestEqual(TEXT("only surviving target binding remains"), Data->EditorBindings.GetNumBindings(), 1);
	const auto* RemainingBinding = Data->EditorBindings.GetBindingSource(
		FPropertyBindingPath(Data->GetStateByID(LinkedId)->Parameters.ID, TEXT("Keep")));
	TestTrue(TEXT("surviving binding keeps its original source"), RemainingBinding
		&& *RemainingBinding == FPropertyBindingPath(Data->GetRootParametersGuid(), TEXT("Alternative")));
	if (HasAnyErrors()) return false;
	{
		TStrongObjectPtr<UStateTree> Native(DuplicateObject<UStateTree>(Tree, GetTransientPackage()));
		auto* NativeData = CastChecked<UStateTreeEditorData>(Native->EditorData);
		UStateTreeState* NativeSource = nullptr;
		NativeData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
			if (State.Name == TEXT("Defaults")) NativeSource = &State;
			return EStateTreeVisitor::Continue;
		});
		if (!TestNotNull(TEXT("native type change source"), NativeSource)) return false;
		NativeSource->Parameters.Parameters.AddProperty(TEXT("Keep"), EPropertyBagPropertyType::Double, nullptr, true);
		NativeSource->Parameters.Parameters.SetValueDouble(TEXT("Keep"), 32.25);
		TestTrue(TEXT("native type change retains field identity"), NativeSource->Parameters.Parameters.FindPropertyDescByName(TEXT("Keep"))->ID == KeepId);
		NativeData->VisitHierarchy([&](UStateTreeState& State, UStateTreeState*) {
			if (State.Type == EStateTreeStateType::Linked)
			{
				State.UpdateParametersFromLinkedSubtree();
				TestEqual(TEXT("native numeric migration retains override or new default"),
					State.Parameters.Parameters.GetValueDouble(TEXT("Keep")).GetValue(), State.Name == TEXT("Linked") ? 84.5 : 32.25);
			}
			return EStateTreeVisitor::Continue;
		});
		FStateTreeCompilerLog NativeLog;
		if (!TestTrue(TEXT("native numeric type change compiles"), UStateTreeEditingSubsystem::CompileStateTree(Native.Get(), NativeLog))
			|| !TestEqual(TEXT("native compatible numeric binding survives"), NativeData->EditorBindings.GetNumBindings(), 1)
			|| HasAnyErrors()) return false;
	}
	Workspace += TEXT("-type");
	Export->SetStringField(TEXT("workspace_path"), Workspace);
	if (!CommandContract::Json::TryCanonicalize(Export, Params, Error)
		|| !DomainAdapters::AiBehavior::TryExecuteContentGraph(TEXT("content_graph.export_workspace"),
			PackageName, TEXT("/Script/StateTreeModule.StateTree"), Params, Payload, ReadOnly, Failure)) return false;
	const FString TypeFile = Workspace / TEXT("structures/state_tree/items/defaults-parameters/properties.hjson");
	if (!FFileHelper::LoadFileToString(SchemaText, *TypeFile)
		|| !HjsonWorkspace::TryParseHjsonDocument(SchemaText, TypeFile, Resources, SchemaParsed, Failure)
		|| !HjsonWorkspace::TryProjectHjsonNodeToJsonValue(*SchemaParsed->GetRoot(), Resources, SchemaJson, Failure)) return false;
	const auto TypeBag = SchemaJson->AsObject()->GetObjectField(TEXT("categories"))->GetObjectField(TEXT("Parameters"))
		->GetObjectField(TEXT("parameters.Parameters"))->GetObjectField(TEXT("value"));
	if (!TestEqual(TEXT("type edit has one source field"), TypeBag->GetArrayField(TEXT("schema")).Num(), 1)) return false;
	TypeBag->GetArrayField(TEXT("schema"))[0]->AsObject()->SetStringField(TEXT("kind"), TEXT("double"));
	TypeBag->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Keep"), 32.25);
	if (!CommandContract::Json::TryCanonicalize(SchemaJson->AsObject(), SchemaText, Error)
		|| !FFileHelper::SaveStringToFile(SchemaText, *TypeFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
	Before = Tree;
	if (!ImportWorkspace())
	{ AddError(TEXT("source numeric type change: ") + Failure.GetCode() + TEXT(":") + Failure.GetJsonPointer()); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("numeric type change persisted and reloaded"), Tree && !Before.IsValid() && !Tree->GetPackage()->IsDirty())) return false;
	Data = CastChecked<UStateTreeEditorData>(Tree->EditorData);
	for (const auto& TypedStateId : {InheritedId, LinkedId})
	{
		const auto* State = Data->GetStateByID(TypedStateId);
		if (!TestNotNull(TEXT("typed linked state"), State)) return false;
		const auto* Field = State->Parameters.Parameters.FindPropertyDescByName(TEXT("Keep"));
		if (!TestTrue(TEXT("linked numeric type and identity migrated"), Field && Field->ID == KeepId
			&& Field->ValueType == EPropertyBagPropertyType::Double)) return false;
		const bool IsOverridden = TypedStateId == LinkedId;
		TestEqual(TEXT("typed value matches native numeric migration"), State->Parameters.Parameters.GetValueDouble(TEXT("Keep")).GetValue(),
			IsOverridden ? 84.5 : 32.25);
		TestEqual(TEXT("typed override inventory retained"), State->Parameters.PropertyOverrides.Num(), IsOverridden ? 1 : 0);
		if (IsOverridden) TestTrue(TEXT("typed override identity retained"), State->Parameters.PropertyOverrides.Contains(KeepId));
	}
	TestEqual(TEXT("compatible binding remains after numeric type change"), Data->EditorBindings.GetNumBindings(), 1);
	return !HasAnyErrors();
}
