#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "Components/StateTreeAIComponentSchema.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "HAL/FileManager.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Kismet2/KismetEditorUtilities.h"
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
#include "StructUtils/PropertyBag.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85StateTreeTextLifecycleTest,
	"UAI.Root.T85.StateTree.TextLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85StateTreeTextLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error))
	{ AddError(TEXT("authoring registration: ") + Error.ToPublicDetail()); return false; }
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RootPath = TEXT("/Game/UAI/Transient/T85StateTreeText/");
	const FString Name = TEXT("ST_") + Suffix, BlueprintName = TEXT("BP_") + Suffix;
	const FString PackageName = RootPath + Name, BlueprintPackageName = RootPath + BlueprintName;
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString BlueprintPath = BlueprintPackageName + TEXT(".") + BlueprintName;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString BlueprintFilename = FPackageName::LongPackageNameToFilename(BlueprintPackageName, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		if (UObject* Asset = LoadObject<UObject>(nullptr, *BlueprintPath)) ObjectTools::DeleteObjectsUnchecked({Asset});
		TestFalse(TEXT("owned tree disk removed"), IFileManager::Get().FileExists(*Filename));
		TestFalse(TEXT("owned Blueprint disk removed"), IFileManager::Get().FileExists(*BlueprintFilename));
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	auto* BlueprintPackage = CreatePackage(*BlueprintPackageName);
	auto* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = UStateTreeTaskBlueprintBase::StaticClass();
	auto* Blueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(), BlueprintPackage,
		*BlueprintName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!TestNotNull(TEXT("native task Blueprint"), Blueprint)) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("task Blueprint compiled"), Blueprint->GeneratedClass
		&& (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings))) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	if (!TestTrue(TEXT("task Blueprint saved"), UPackage::SavePackage(BlueprintPackage, Blueprint, *BlueprintFilename, Save))) return false;
	auto* Package = CreatePackage(*PackageName);
	auto* Tree = NewObject<UStateTree>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Tree);
	auto* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
	Tree->EditorData = Data;
	const FString RootParametersId = Data->GetRootParametersGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const auto* BagProperty = FindFProperty<FStructProperty>(Data->GetClass(), TEXT("RootParameterPropertyBag"));
	if (!TestNotNull(TEXT("native root parameter property"), BagProperty)) return false;
	const auto ParameterBag = [&](UStateTree* Current) -> FInstancedPropertyBag* {
		auto* CurrentData = Current ? Cast<UStateTreeEditorData>(Current->EditorData) : nullptr;
		return CurrentData ? BagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(CurrentData) : nullptr;
	};
	const auto ParameterText = [&](UStateTree* Current) -> FText {
		const auto* Bag = ParameterBag(Current);
		if (!Bag) return FText();
		const auto Value = Bag->GetValueText(TEXT("Caption"));
		return Value.IsValid() ? Value.GetValue() : FText();
	};
	Data->Schema = NewObject<UStateTreeAIComponentSchema>(Data, NAME_None, RF_Transactional);
	auto& Root = Data->AddSubTree(TEXT("TextRoot"));
	auto& Task = Root.Tasks.AddDefaulted_GetRef();
	Task.ID = FGuid::NewGuid();
	Task.Node.InitializeAs<FStateTreeBlueprintTaskWrapper>();
	Task.Node.GetMutable<FStateTreeBlueprintTaskWrapper>().TaskClass = Blueprint->GeneratedClass;
	Task.InstanceObject = NewObject<UObject>(&Root, Blueprint->GeneratedClass, NAME_None, RF_Transactional);
	const FString TaskId = Task.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
	const auto* TextProperty = FindFProperty<FTextProperty>(Blueprint->GeneratedClass, TEXT("Description"));
	if (!TestNotNull(TEXT("native editable description"), TextProperty)) return false;
	if (!TestTrue(TEXT("native seed text edit"), TextNamespaceUtil::EditTextProperty(Task.InstanceObject, TextProperty,
		TextNamespaceUtil::ETextEditAction::SourceString, TEXT("Original description")))) return false;
	FStateTreeCompilerLog CompileLog;
	if (!TestTrue(TEXT("native tree compiles"), UStateTreeEditingSubsystem::CompileStateTree(Tree, CompileLog))
		|| !TestTrue(TEXT("native tree saves"), UPackage::SavePackage(Package, Tree, *Filename, Save))) return false;
	const auto Instance = [&]() -> UObject* {
		auto* Current = LoadObject<UStateTree>(nullptr, *ObjectPath);
		auto* CurrentData = Current ? Cast<UStateTreeEditorData>(Current->EditorData) : nullptr;
		return CurrentData && CurrentData->SubTrees.Num() == 1 && CurrentData->SubTrees[0]->Tasks.Num() == 1
			? CurrentData->SubTrees[0]->Tasks[0].InstanceObject.Get() : nullptr;
	};
	const auto BuildRequest = [&](const FString& Source, UeAgentReflectionAuthoring::FAssetApplyRequest& Request) {
		TSharedPtr<FJsonValue> Document;
		auto* Current = LoadObject<UStateTree>(nullptr, *ObjectPath);
		if (!Current || !UeAgentStateTreeAuthoring::ReadTreeDocument(*Current, Document, Error))
		{ AddError(TEXT("text request capture: ") + Error.ToPublicDetail()); return false; }
		const auto Row = Document->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(TaskId);
		const auto& Properties = Row->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"));
		const auto* Description = Properties.FindByPredicate([](const auto& Value) {
			return Value->AsObject()->GetStringField(TEXT("property_path")) == TEXT("instance_object.Description");
		});
		if (!TestNotNull(TEXT("captured task description descriptor"), Description)) return false;
		(*Description)->AsObject()->SetStringField(TEXT("value"), Source);
		const auto& ParameterProperties = Document->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(RootParametersId)
			->GetObjectField(TEXT("properties"))->GetArrayField(TEXT("properties"));
		const auto* Parameter = ParameterProperties.FindByPredicate([](const auto& Value) {
			return Value->AsObject()->GetStringField(TEXT("property_path")) == TEXT("parameters.RootParameterPropertyBag");
		});
		if (!TestNotNull(TEXT("captured root parameter descriptor"), Parameter)) return false;
		const auto Bag = (*Parameter)->AsObject()->GetObjectField(TEXT("value"));
		if (Bag->GetArrayField(TEXT("schema")).IsEmpty()) {
			auto CaptionSchema = MakeShared<FJsonObject>();
			CaptionSchema->SetStringField(TEXT("name"), TEXT("Caption")); CaptionSchema->SetStringField(TEXT("kind"), TEXT("text"));
			Bag->SetArrayField(TEXT("schema"), {MakeShared<FJsonValueObject>(CaptionSchema)});
		}
		Bag->GetObjectField(TEXT("fields"))->SetStringField(TEXT("Caption"), Source);
		Request.TypeId = TEXT("state_tree"); Request.AssetPath = PackageName;
		Request.bSave = true; Request.bCompactValues = true;
		Request.ProviderExtension = MakeShared<FJsonObject>();
		Request.ProviderExtension->SetObjectField(TEXT("state_tree"), Document->AsObject());
		return true;
	};
	UeAgentReflectionAuthoring::FAssetApplyRequest SuccessRequest;
	if (!BuildRequest(TEXT("Persisted description"), SuccessRequest)) return false;
	TWeakObjectPtr<UStateTree> BeforeSuccess(Tree);
	SuccessRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Actual = LoadObject<UStateTree>(nullptr, *ObjectPath);
		UObject* ActualInstance = Instance();
		if (Actual && ActualInstance && !BeforeSuccess.IsValid(true, true) && !Actual->GetOutermost()->IsDirty()
			&& TextProperty->GetPropertyValue_InContainer(ActualInstance).ToString() == TEXT("Persisted description")
			&& ParameterText(Actual).ToString() == TEXT("Persisted description")) return true;
		Failure.Diagnostic.Code = TEXT("t208_state_tree_text_success_readback_failed"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	UeAgentReflectionAuthoring::FAssetApplyResult SuccessResult;
	if (!UeAgentReflectionAuthoring::ApplyAsset(SuccessRequest, SuccessResult, TEXT("state-tree-text-success-") + Suffix))
	{ AddError(SuccessResult.Diagnostic.Code + TEXT(":") + SuccessResult.Diagnostic.PublicMessage); return false; }
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	UObject* CurrentInstance = Instance();
	if (!TestTrue(TEXT("success unloaded old tree and persisted the new text"), !BeforeSuccess.IsValid(true, true)
		&& Tree && CurrentInstance && !Tree->GetOutermost()->IsDirty()
		&& TextProperty->GetPropertyValue_InContainer(CurrentInstance).ToString() == TEXT("Persisted description"))) return false;
	const FText PublishedText = TextProperty->GetPropertyValue_InContainer(CurrentInstance);
	TestTrue(TEXT("published text has a native localization identity"),
		!PublishedText.IsCultureInvariant() && !FTextInspector::GetTextId(PublishedText).GetKey().IsEmpty());
	const FText PublishedParameter = ParameterText(Tree);
	TestTrue(TEXT("new text parameter persists a native localization identity"),
		PublishedParameter.ToString() == TEXT("Persisted description") && !PublishedParameter.IsCultureInvariant()
		&& !FTextInspector::GetTextId(PublishedParameter).GetKey().IsEmpty());
	TArray<uint8> DiskBefore, BlueprintDiskBefore;
	if (!TestTrue(TEXT("saved tree bytes readable"), FFileHelper::LoadFileToArray(DiskBefore, *Filename))
		|| !TestTrue(TEXT("saved Blueprint bytes readable"), FFileHelper::LoadFileToArray(BlueprintDiskBefore, *BlueprintFilename))) return false;
	CurrentInstance->Modify();
	if (!TestTrue(TEXT("native dirty description edit"), TextNamespaceUtil::EditTextProperty(CurrentInstance, TextProperty,
		TextNamespaceUtil::ETextEditAction::SourceString, TEXT("Dirty description before rejection")))) return false;
	const FText DirtyParameter = FText::ChangeKey(FTextInspector::GetTextId(PublishedParameter).GetNamespace().ToString(),
		FTextInspector::GetTextId(PublishedParameter).GetKey().ToString(), FText::FromString(TEXT("Dirty parameter before rejection")));
	if (!TestTrue(TEXT("native dirty parameter edit"), ParameterBag(Tree)->SetValueText(TEXT("Caption"), DirtyParameter) == EPropertyBagResult::Success)) return false;
	Tree->MarkPackageDirty();
	const FText DirtyText = TextProperty->GetPropertyValue_InContainer(CurrentInstance);
	const auto MetadataBefore = Tree->GetOutermost()->GetMetaData().RootMetaDataMap;
	const auto BlueprintMetadataBefore = BlueprintPackage->GetMetaData().RootMetaDataMap;
	TSharedPtr<FJsonValue> DirtyDocument;
	FString DirtyDocumentText;
	if (!UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, DirtyDocument, Error)
		|| !CommandContract::Json::TryCanonicalize(DirtyDocument->AsObject(), DirtyDocumentText, Error))
	{ AddError(TEXT("dirty text capture: ") + Error.ToPublicDetail()); return false; }
	UeAgentReflectionAuthoring::FAssetApplyRequest LateRequest;
	if (!BuildRequest(TEXT("Rejected description"), LateRequest)) return false;
	const TWeakObjectPtr<UStateTree> BeforeLate(Tree);
	const TWeakObjectPtr<UObject> BeforeInstance(CurrentInstance);
	TWeakObjectPtr<UStateTree> RejectedTree;
	bool LateObserved = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* ActualTree = LoadObject<UStateTree>(nullptr, *ObjectPath);
		UObject* ActualInstance = Instance();
		RejectedTree = ActualTree;
		TArray<uint8> ActualDisk;
		LateObserved = ActualTree && ActualInstance && !BeforeLate.IsValid(true, true) && !BeforeInstance.IsValid(true, true)
			&& !ActualTree->GetOutermost()->IsDirty()
			&& TextProperty->GetPropertyValue_InContainer(ActualInstance).ToString() == TEXT("Rejected description")
			&& !FTextInspector::GetTextId(TextProperty->GetPropertyValue_InContainer(ActualInstance)).GetKey().IsEmpty()
			&& ParameterText(ActualTree).ToString() == TEXT("Rejected description")
			&& !FTextInspector::GetTextId(ParameterText(ActualTree)).GetKey().IsEmpty()
			&& FFileHelper::LoadFileToArray(ActualDisk, *Filename) && ActualDisk != DiskBefore;
		Failure.Diagnostic.Code = TEXT("t208_state_tree_text_late_rejected"); Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
		return false;
	};
	UeAgentReflectionAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("late text edit rejected before commit"),
		UeAgentReflectionAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("state-tree-text-late-") + Suffix));
	if (!TestTrue(TEXT("rejection follows real persistence and independent reload"), LateObserved)
		|| !TestEqual(TEXT("original rejection retained"), LateResult.Diagnostic.Code, FString(TEXT("t208_state_tree_text_late_rejected"))))
	{ AddError(LateResult.Phase + TEXT(":") + LateResult.Diagnostic.PublicMessage); return false; }
	TestTrue(TEXT("checkpoint recovery confirmed"), LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded);
	TestFalse(TEXT("rejected tree removed during recovery"), RejectedTree.IsValid(true, true));
	Tree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	CurrentInstance = Instance();
	if (!TestTrue(TEXT("restored tree and Blueprint instance exist"), Tree && CurrentInstance)) return false;
	const FText RestoredText = TextProperty->GetPropertyValue_InContainer(CurrentInstance);
	TestTrue(TEXT("dirty text source and exact native identity restored"), RestoredText.ToString() == DirtyText.ToString()
		&& FTextInspector::GetNamespace(RestoredText) == FTextInspector::GetNamespace(DirtyText)
		&& FTextInspector::GetKey(RestoredText) == FTextInspector::GetKey(DirtyText));
	const FText RestoredParameter = ParameterText(Tree);
	TestTrue(TEXT("dirty parameter source and exact identity restored"), RestoredParameter.ToString() == DirtyParameter.ToString()
		&& FTextInspector::GetNamespace(RestoredParameter) == FTextInspector::GetNamespace(DirtyParameter)
		&& FTextInspector::GetKey(RestoredParameter) == FTextInspector::GetKey(DirtyParameter));
	TestTrue(TEXT("target package metadata restored"), Tree->GetOutermost()->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(MetadataBefore));
	TestTrue(TEXT("referenced Blueprint package metadata unchanged"), BlueprintPackage->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(BlueprintMetadataBefore));
	TSharedPtr<FJsonValue> RestoredDocument;
	FString RestoredDocumentText;
	TestTrue(TEXT("complete dirty tree document restored"), Tree->GetOutermost()->IsDirty()
		&& UeAgentStateTreeAuthoring::ReadTreeDocument(*Tree, RestoredDocument, Error)
		&& CommandContract::Json::TryCanonicalize(RestoredDocument->AsObject(), RestoredDocumentText, Error)
		&& RestoredDocumentText == DirtyDocumentText);
	TArray<uint8> DiskAfter, BlueprintDiskAfter;
	TestTrue(TEXT("original tree disk restored independently of dirty memory"),
		FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	TestTrue(TEXT("referenced Blueprint disk unchanged"),
		FFileHelper::LoadFileToArray(BlueprintDiskAfter, *BlueprintFilename) && BlueprintDiskAfter == BlueprintDiskBefore);
	return !HasAnyErrors();
}
