#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/FileHelper.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "UObject/TextProperty.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace
{
	using namespace UE::UAI;

	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
				UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool TryBuildPublicBlueprintCreateParams(
		const FString& Target,
		TSharedPtr<FJsonObject>& OutParams,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedRef<FJsonObject> DocumentTarget = MakeShared<FJsonObject>();
		DocumentTarget->SetStringField(TEXT("asset_path"), Target);
		DocumentTarget->SetStringField(TEXT("asset_kind"), TEXT("actor_blueprint"));
		const TSharedRef<FJsonObject> AuthoringDocument = MakeShared<FJsonObject>();
		AuthoringDocument->SetStringField(TEXT("operation"), TEXT("create"));
		AuthoringDocument->SetObjectField(TEXT("target"), DocumentTarget);
		AuthoringDocument->SetObjectField(
			TEXT("provider_extension"), MakeShared<FJsonObject>());
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("/Script/Engine.Blueprint"));
		Root->SetObjectField(TEXT("document"), AuthoringDocument);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.blueprint.edit/1"));
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
		OutParams = MakeShared<FJsonObject>();
		OutParams->SetStringField(TEXT("target"), Target);
		OutParams->SetObjectField(TEXT("source"), Source);
		return true;
	}
}

static bool RunBlueprintCreateRecovery(FAutomationTestBase& Test, const bool bEarly)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	using namespace UeAgentAuthoringOperationApply;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85BlueprintCreateRecovery/BP_") + Token;
	const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + Name;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	auto IsAbsent = [&]()
	{
		return !FindObject<UObject>(nullptr, *ObjectPath)
			&& !IFileManager::Get().FileExists(*Filename)
			&& !FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
				FSoftObjectPath(ObjectPath)).IsValid();
	};
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath))
		{
			Test.TestEqual(TEXT("remaining owned create recovery fixture deletes through UE"),
				ObjectTools::ForceDeleteObjects({Remaining}, false), 1);
		}
		Test.TestTrue(TEXT("create recovery fixture leaves object disk and registry absent"), IsAbsent());
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error))
	{
		Test.AddError(Error.ToPublicDetail());
		return false;
	}
	if (!Test.TestTrue(TEXT("create recovery begins with independently observed absence"), IsAbsent()))
	{
		return false;
	}

	FAssetApplyRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Create;
	Request.AssetPath = PackageName;
	Request.AssetKind = TEXT("actor_blueprint");
	Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>();
	Request.ProviderExtension->SetBoolField(TEXT("compile"), true);
	Request.ProviderExtension->SetBoolField(TEXT("save"), true);

	class FRejectCreatedBlueprintStage final : public IDurablePackageStager
	{
	public:
		FRejectCreatedBlueprintStage(FString InObjectPath, FString InFilename, const bool bInReject)
			: ObjectPath(MoveTemp(InObjectPath)), Filename(MoveTemp(InFilename)),
			  Inner(CreateDefaultDurablePackageStager()), bReject(bInReject) {}
		bool BuildArtifactInventory(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			return Inner->BuildArtifactInventory(Plans, Artifacts, Failure);
		}
		bool StagePackages(const TArray<FPackageDurablePlan>& Plans,
			TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,
			UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override
		{
			if (!Inner->StagePackages(Plans, Artifacts, Failure)) return false;
			const UBlueprint* Actual = FindObject<UBlueprint>(nullptr, *ObjectPath);
			if (!Actual) return true;
			StagedWitness = Actual;
			bObserved = Actual && Actual->ParentClass == AActor::StaticClass()
				&& Actual->GeneratedClass && Actual->GeneratedClass->IsChildOf(AActor::StaticClass())
				&& !IFileManager::Get().FileExists(*Filename)
				&& FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
					FSoftObjectPath(ObjectPath)).IsValid();
			bool bAnyStaged = false;
			for (const auto& Artifact : Artifacts)
			{
				bObserved = bObserved
					&& IFileManager::Get().FileExists(*Artifact.StagingPath) == Artifact.bStagedExists;
				bAnyStaged |= Artifact.bStagedExists;
			}
			bObserved &= bAnyStaged;
			if (!bReject) return true;
			Failure.Diagnostic.Code = TEXT("t85_blueprint_create_stage_rejected");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		}
		bool bObserved = false;
		FWeakObjectPtr StagedWitness;
	private:
		FString ObjectPath;
		FString Filename;
		TSharedRef<IDurablePackageStager> Inner;
		bool bReject = false;
	};

	TSharedPtr<FRejectCreatedBlueprintStage> Stager;
	bool bLateObserved = false;
	FWeakObjectPtr PublishedWitness;
	if (bEarly)
	{
		Stager = MakeShared<FRejectCreatedBlueprintStage>(ObjectPath, Filename, true);
		Request.PackageStager = Stager;
	}
	else
	{
		Stager = MakeShared<FRejectCreatedBlueprintStage>(ObjectPath, Filename, false);
		Request.PackageStager = Stager;
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			const UBlueprint* Actual = LoadObject<UBlueprint>(nullptr, *ObjectPath);
			PublishedWitness = Actual;
			bLateObserved = Stager->bObserved && !Stager->StagedWitness.IsValid(true, true)
				&& Actual && Actual->ParentClass == AActor::StaticClass()
				&& Actual->GeneratedClass && Actual->GeneratedClass->IsChildOf(AActor::StaticClass())
				&& Actual->Status == BS_UpToDate && !Actual->GetOutermost()->IsDirty()
				&& IFileManager::Get().FileExists(*Filename)
				&& FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
					FSoftObjectPath(ObjectPath)).IsValid();
			Failure.Diagnostic.Code = TEXT("t85_blueprint_create_late_rejected");
			Failure.Diagnostic.PublicMessage = Failure.Diagnostic.Code;
			return false;
		};
	}

	FAssetApplyResult Result;
	Test.TestFalse(TEXT("Blueprint create rejects at requested lifecycle phase"),
		ApplyAsset(Request, Result,
			(bEarly ? TEXT("t85-blueprint-create-early-") : TEXT("t85-blueprint-create-late-")) + Token));
	Test.TestEqual(TEXT("requested create diagnostic retained"), Result.Diagnostic.Code,
		FString(bEarly ? TEXT("t85_blueprint_create_stage_rejected")
			: TEXT("t85_blueprint_create_late_rejected")));
	if (!Test.TestTrue(TEXT("real requested create mutation phase observed"),
			bEarly ? Stager->bObserved : bLateObserved)
		|| !Test.TestTrue(TEXT("create checkpoint recovery confirmed"),
			Result.bRollbackAttempted && Result.bRollbackSucceeded)
		|| (!bEarly && !Test.TestFalse(TEXT("published Blueprint generation is released during restore"),
			PublishedWitness.IsValid(true, true)))
		|| !Test.TestTrue(TEXT("create rollback restores object disk and registry absence"), IsAbsent()))
	{
		Test.AddError(Result.Phase + TEXT(":") + Result.Diagnostic.Code + TEXT(":") + Result.Diagnostic.PublicMessage);
		return false;
	}
	FAssetRegistryModule::TickAssetRegistry(0.15f);
	return Test.TestTrue(TEXT("restored Blueprint absence survives native registry tick"), IsAbsent());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlueprintCreateEarlyRecoveryTest,
	"UAI.Root.T85.Blueprint.CreateEarlyRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintCreateEarlyRecoveryTest::RunTest(const FString&)
{
	return RunBlueprintCreateRecovery(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlueprintCreateLateRecoveryTest,
	"UAI.Root.T85.Blueprint.CreateLateRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintCreateLateRecoveryTest::RunTest(const FString&)
{
	return RunBlueprintCreateRecovery(*this, false);
}

static bool RunBlueprintRootPropertyRecovery(FAutomationTestBase& Test, const bool bEarly)
{
	using namespace UE::UAI;
	using namespace UeAgentBlueprintAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!RegisterAuthoring(Error)) { Test.AddError(Error.ToPublicDetail()); return false; }
	const FString Token=FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName=TEXT("/Game/UAI/Transient/T85BlueprintRecovery/BP_")+Token;
	const FString Name=FPackageName::GetLongPackageAssetName(PackageName), ObjectPath=PackageName+TEXT(".")+Name;
	const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); Test.TestFalse(TEXT("owned recovery Blueprint disk absent"),IFileManager::Get().FileExists(*Filename)); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test,PackageName); };
	UPackage* Package=CreatePackage(*PackageName); auto* Factory=NewObject<UBlueprintFactory>(); Factory->ParentClass=AActor::StaticClass();
	UBlueprint* Blueprint=Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(),Package,*Name,RF_Public|RF_Standalone|RF_Transactional,nullptr,GWarn));
	if(!Test.TestNotNull(TEXT("owned recovery Blueprint"),Blueprint)) return false;
	Blueprint->bRunConstructionScriptOnDrag=false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if(!Test.TestTrue(TEXT("seed compile settled"),Blueprint->Status==BS_UpToDate || Blueprint->Status==BS_UpToDateWithWarnings)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint); FSavePackageArgs Save; Save.TopLevelFlags=RF_Public|RF_Standalone;
	if(!Test.TestTrue(TEXT("original disk saved"),UPackage::SavePackage(Package,Blueprint,*Filename,Save))) return false;
	Package->SetDirtyFlag(false);
	TArray<uint8> DiskBefore; if(!Test.TestTrue(TEXT("original bytes read independently"),FFileHelper::LoadFileToArray(DiskBefore,*Filename))) return false;
	struct FNativeState { TMap<FString,FString> Fields; TMap<FString,FText> Texts; };
	auto CaptureNative=[](UBlueprint& Root) {
		FNativeState State; TArray<UObject*> Objects;
		FReferenceFinder References(Objects,&Root,false,true,true,true); References.FindReferences(&Root); Objects.Add(&Root);
		for(UObject* Object:Objects) {
			if(!IsValid(Object)||Object->HasAnyFlags(RF_Transient)) continue;
			const FString Path=Object->GetPathName(&Root); State.Fields.Add(Path+TEXT("/@class"),Object->GetClass()->GetPathName());
			for(TFieldIterator<FProperty> It(Object->GetClass());It;++It) {
				if(It->HasAnyPropertyFlags(CPF_Transient)) continue;
				if(const FTextProperty* Text=CastField<FTextProperty>(*It)) { State.Texts.Add(Path+TEXT("/")+It->GetName(),Text->GetPropertyValue_InContainer(Object)); continue; }
				FString Value; It->ExportText_InContainer(0,Value,Object,nullptr,Object,PPF_None); State.Fields.Add(Path+TEXT("/")+It->GetName(),MoveTemp(Value));
			}
			if(const UEdGraphNode* Node=Cast<UEdGraphNode>(Object)) {
				TArray<FString> PinOrder;
				for(const UEdGraphPin* Pin:Node->Pins) {
					const FString PinKey=Path+TEXT("/@pin/")+Pin->PinId.ToString(); PinOrder.Add(Pin->PinId.ToString());
					FString Type; FEdGraphPinType::StaticStruct()->ExportText(Type,&Pin->PinType,nullptr,Object,PPF_None,nullptr);
					State.Fields.Add(PinKey+TEXT("/type"),MoveTemp(Type));
					State.Fields.Add(PinKey+TEXT("/name"),Pin->PinName.ToString());
					State.Fields.Add(PinKey+TEXT("/direction"),LexToString(static_cast<int32>(Pin->Direction.GetValue())));
					State.Fields.Add(PinKey+TEXT("/default"),Pin->DefaultValue);
					State.Fields.Add(PinKey+TEXT("/autodefault"),Pin->AutogeneratedDefaultValue);
					State.Fields.Add(PinKey+TEXT("/object"),GetPathNameSafe(Pin->DefaultObject.Get()));
					State.Texts.Add(PinKey+TEXT("/text"),Pin->DefaultTextValue);
					State.Fields.Add(PinKey+TEXT("/parent"),Pin->ParentPin?Pin->ParentPin->PinId.ToString():FString());
					TArray<FString> Links,SubPins;
					for(const UEdGraphPin* Link:Pin->LinkedTo) Links.Add(Link->GetOwningNode()->GetPathName(&Root)+TEXT("/")+Link->PinId.ToString());
					for(const UEdGraphPin* Child:Pin->SubPins) SubPins.Add(Child->PinId.ToString());
					Links.Sort(); State.Fields.Add(PinKey+TEXT("/links"),FString::Join(Links,TEXT(","))); State.Fields.Add(PinKey+TEXT("/subpins"),FString::Join(SubPins,TEXT(",")));
				}
				State.Fields.Add(Path+TEXT("/@pins"),FString::Join(PinOrder,TEXT(",")));
			}
		}
		return State;
	};
	TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs); UEdGraphNode* ChangedNode=nullptr;
	for(UEdGraph* Graph:Graphs) if(!Graph->Nodes.IsEmpty()) { ChangedNode=Graph->Nodes[0]; break; }
	if(!Test.TestNotNull(TEXT("real graph node before mutation"),ChangedNode)) return false;
	ChangedNode->Modify(); ChangedNode->NodePosX=357; ChangedNode->NodePosY=246;
	const FGuid NodeGuid=ChangedNode->NodeGuid;
	Blueprint->bRunConstructionScriptOnDrag=true; Blueprint->BlueprintCategory=TEXT("DirtyRecoveryCategory"); Blueprint->MarkPackageDirty();
	const FNativeState Before=CaptureNative(*Blueprint);
	const TWeakObjectPtr<UBlueprint> OldBlueprint=Blueprint;
	const TWeakObjectPtr<UEdGraphNode> OldNode=ChangedNode;
	FAssetApplyRequest Request; Request.AssetPath=PackageName; Request.Operation=UeAgentApplyInputContract::EOperation::Update; Request.bCompactValues=true;
	auto Property=MakeShared<FJsonObject>(); Property->SetStringField(TEXT("property_path"),TEXT("bRunConstructionScriptOnDrag")); Property->SetBoolField(TEXT("value"),false);
	Request.Properties.Add(MakeShared<FJsonValueObject>(Property)); Request.ProviderExtension=MakeShared<FJsonObject>(); Request.ProviderExtension->SetBoolField(TEXT("compile"),true); Request.ProviderExtension->SetBoolField(TEXT("save"),true);
	bool bLateObserved=false, bFinalReadbackCalled=false, bEarlyObserved=false;
	class FRejectBlueprintStage final : public UeAgentAuthoringOperationApply::IDurablePackageStager {
	public:
		TWeakObjectPtr<UBlueprint> Original;
		TFunction<bool(const TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>&)> Observe;
		TSharedRef<IDurablePackageStager> Inner=UeAgentAuthoringOperationApply::CreateDefaultDurablePackageStager();
		bool BuildArtifactInventory(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override { return Inner->BuildArtifactInventory(Plans,Artifacts,Failure); }
		bool StagePackages(const TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts,UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) override {
			if(!Inner->StagePackages(Plans,Artifacts,Failure)) return false;
			if(Original.IsValid() && Original->bRunConstructionScriptOnDrag) return true;
			Observe(Artifacts);
			Failure.Diagnostic.Code=TEXT("t85_blueprint_early_stage_rejected"); Failure.Diagnostic.PublicMessage=TEXT("real mutated target staged before release and final readback"); return false;
		}
	};
	if(bEarly) {
		auto Stager=MakeShared<FRejectBlueprintStage>(); Stager->Original=Blueprint;
		Stager->Observe=[&](const TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact>& Artifacts) {
			TArray<uint8> Bytes;
			bEarlyObserved=OldBlueprint.IsValid() && OldNode.IsValid() && !OldBlueprint->bRunConstructionScriptOnDrag
				&& OldBlueprint->GetOutermost()->IsDirty() && OldNode->NodePosX==357 && OldNode->NodePosY==246
				&& !bFinalReadbackCalled && FFileHelper::LoadFileToArray(Bytes,*Filename) && Bytes==DiskBefore;
			bool bAny=false; for(const auto& Artifact:Artifacts) { bEarlyObserved &= IFileManager::Get().FileExists(*Artifact.StagingPath)==Artifact.bStagedExists; bAny |= Artifact.bStagedExists; }
			bEarlyObserved &= bAny; return bEarlyObserved;
		};
		Request.PackageStager=Stager;
	}
	Request.FinalReadback=[&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		bFinalReadbackCalled=true;
		UBlueprint* Published=LoadObject<UBlueprint>(nullptr,*ObjectPath); UEdGraphNode* Node=nullptr;
		if(Published) { TArray<UEdGraph*> PublishedGraphs; Published->GetAllGraphs(PublishedGraphs); for(UEdGraph* Graph:PublishedGraphs) for(UEdGraphNode* Item:Graph->Nodes) if(Item->NodeGuid==NodeGuid) Node=Item; }
		TArray<uint8> Bytes;
		bLateObserved=Published && Node && !OldBlueprint.IsValid() && !OldNode.IsValid() && !Published->bRunConstructionScriptOnDrag
			&& Node->NodePosX==357 && Node->NodePosY==246 && Published->BlueprintCategory==TEXT("DirtyRecoveryCategory")
			&& !Published->GetOutermost()->IsDirty() && (Published->Status==BS_UpToDate || Published->Status==BS_UpToDateWithWarnings)
			&& FFileHelper::LoadFileToArray(Bytes,*Filename) && Bytes!=DiskBefore;
		Failure.Diagnostic.Code=TEXT("t85_blueprint_late_readback_rejected"); Failure.Diagnostic.PublicMessage=TEXT("published clean Blueprint root and dirty graph state observed before rejection"); return false;
	};
	Graphs.Reset(); ChangedNode=nullptr; Blueprint=nullptr; Package=nullptr;
	FAssetApplyResult Result;
	Test.TestFalse(TEXT("Blueprint update rejects at requested phase"),ApplyAsset(Request,Result,(bEarly?TEXT("blueprint-early-"):TEXT("blueprint-late-"))+Token));
	Test.AddInfo(TEXT("Blueprint rejection:")+Result.Phase+TEXT(":")+Result.Diagnostic.Code);
	Test.TestEqual(TEXT("requested diagnostic retained"),Result.Diagnostic.Code,FString(bEarly?TEXT("t85_blueprint_early_stage_rejected"):TEXT("t85_blueprint_late_readback_rejected")));
	if(!Test.TestTrue(TEXT("real requested mutation phase observed"),bEarly?(bEarlyObserved&&!bFinalReadbackCalled):bLateObserved) || !Test.TestTrue(TEXT("checkpoint recovery confirmed"),Result.bRollbackAttempted&&Result.bRollbackSucceeded)) { Test.AddError(Result.Phase+TEXT(":")+Result.Diagnostic.Code+TEXT(":")+Result.Diagnostic.PublicMessage); return false; }
	Blueprint=LoadObject<UBlueprint>(nullptr,*ObjectPath); if(!Test.TestNotNull(TEXT("restored Blueprint"),Blueprint)) return false;
	const FNativeState Restored=CaptureNative(*Blueprint); bool bEqual=Before.Fields.OrderIndependentCompareEqual(Restored.Fields)&&Before.Texts.Num()==Restored.Texts.Num();
	for(const auto& Entry:Before.Texts) { const FText* Value=Restored.Texts.Find(Entry.Key); bEqual=bEqual&&Value&&FTextProperty::Identical_Implementation(Entry.Value,*Value,PPF_None); }
	if(!bEqual) { int32 Count=0; for(const auto& Entry:Before.Fields) { const FString* Value=Restored.Fields.Find(Entry.Key); if((!Value||*Value!=Entry.Value)&&Count++<12) Test.AddInfo(TEXT("Native difference ")+Entry.Key+TEXT(" before=")+Entry.Value+TEXT(" after=")+(Value?*Value:TEXT("<missing>"))); } for(const auto& Entry:Restored.Fields) if(!Before.Fields.Contains(Entry.Key)&&Count++<12) Test.AddInfo(TEXT("Native extra ")+Entry.Key); }
	Test.TestTrue(TEXT("captured persistent members and native graph pin structure restored"),bEqual);
	Test.TestTrue(TEXT("dirty native root and flag restored"),Blueprint->bRunConstructionScriptOnDrag&&Blueprint->GetOutermost()->IsDirty());
	TArray<uint8> DiskAfter; Test.TestTrue(TEXT("original disk bytes separately restored"),FFileHelper::LoadFileToArray(DiskAfter,*Filename)&&DiskAfter==DiskBefore);
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlueprintRootPropertyRecoveryTest,
	"UAI.Root.T85.Blueprint.RootPropertyRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintRootPropertyRecoveryTest::RunTest(const FString&)
{
	return RunBlueprintRootPropertyRecovery(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlueprintRootPropertyEarlyRecoveryTest,
	"UAI.Root.T85.Blueprint.RootPropertyEarlyRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintRootPropertyEarlyRecoveryTest::RunTest(const FString&)
{
	return RunBlueprintRootPropertyRecovery(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85BlueprintRootPropertyWorkspaceTest,
	"UAI.Root.T85.Blueprint.RootPropertyWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintRootPropertyWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token=FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName=TEXT("/Game/UAI/Transient/T85BlueprintRoot/BP_")+Token;
	const FString Name=FPackageName::GetLongPackageAssetName(PackageName), ObjectPath=PackageName+TEXT(".")+Name;
	const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
	const FString Workspace=FPaths::ProjectSavedDir()/TEXT("Automation/UAI/T85/BlueprintRoot")/Token, Readback=Workspace+TEXT("-readback");
	ON_SCOPE_EXIT { DeleteAssetIfPresent(ObjectPath); TestFalse(TEXT("owned blueprint disk absent"),IFileManager::Get().FileExists(*Filename)); T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this,PackageName); IFileManager::Get().DeleteDirectory(*Workspace,false,true); IFileManager::Get().DeleteDirectory(*Readback,false,true); };
	UPackage* Package=CreatePackage(*PackageName); auto* Factory=NewObject<UBlueprintFactory>(); Factory->ParentClass=AActor::StaticClass();
	UBlueprint* Blueprint=Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(),Package,*Name,RF_Public|RF_Standalone|RF_Transactional,nullptr,GWarn));
	if(!TestNotNull(TEXT("owned native Actor Blueprint"),Blueprint)) return false;
	Blueprint->bRunConstructionScriptOnDrag=true; FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if(!TestTrue(TEXT("seed generated class and compile valid"),Blueprint->GeneratedClass && (Blueprint->Status==BS_UpToDate || Blueprint->Status==BS_UpToDateWithWarnings))) return false;
	const FString GeneratedPath=Blueprint->GeneratedClass->GetPathName();
	auto GraphState=[](UBlueprint* Asset) {
		TMap<FString,FString> State; TArray<UEdGraph*> Graphs; Asset->GetAllGraphs(Graphs);
		for(UEdGraph* Graph:Graphs) {
			State.Add(Graph->GetName()+TEXT("/@class"),Graph->GetClass()->GetPathName());
			for(UEdGraphNode* Node:Graph->Nodes) {
				const FString NodeKey=Graph->GetName()+TEXT("/")+Node->NodeGuid.ToString();
				State.Add(NodeKey,Node->GetClass()->GetPathName()+FString::Printf(TEXT(":%d,%d"),Node->NodePosX,Node->NodePosY));
				for(UEdGraphPin* Pin:Node->Pins) {
					TArray<FString> Links; for(UEdGraphPin* Link:Pin->LinkedTo) Links.Add(Link->GetOwningNode()->NodeGuid.ToString()+TEXT("/")+Link->PinId.ToString()); Links.Sort();
					State.Add(NodeKey+TEXT("/")+Pin->PinId.ToString(),Pin->PinName.ToString()+TEXT(":")+Pin->DefaultValue+TEXT(":")+FString::Join(Links,TEXT(",")));
				}
			}
		}
		return State;
	};
	const auto Before=GraphState(Blueprint); if(!TestFalse(TEXT("seed graph structure exists"),Before.IsEmpty())) return false;
	FAssetRegistryModule::AssetCreated(Blueprint); FSavePackageArgs Save; Save.TopLevelFlags=RF_Public|RF_Standalone;
	if(!TestTrue(TEXT("seed saved"),UPackage::SavePackage(Package,Blueprint,*Filename,Save))) return false; Package->SetDirtyFlag(false);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry; TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch; UE::UAI::Contract::FDiagnostic Error;
	if(!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry,Dispatch,Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto ExportTo=[&](const FString& Path) {
		auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"),ObjectPath); Args->SetStringField(TEXT("workspace_path"),Path); Args->SetStringField(TEXT("profile_id"),TEXT("llm_authoring"));
		auto Result=Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry,Dispatch,TEXT("content_graph.export_workspace"),Args,TEXT("t85-blueprint-root-export"),{TEXT("uai:query"),TEXT("uai:export"),TEXT("uai:operate")}); if(!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk();
	};
	auto EditOrRead=[&](const FString& Path,bool bEdit) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files,*Path,TEXT("properties.hjson"),true,false); int32 Matches=0;
		for(const FString& File:Files) {
			FString Text; TSharedPtr<FJsonObject> Document; if(!FFileHelper::LoadFileToString(Text,*File) || !FUeAgentStructuredHjsonParser::ParseObject(Text,File,Document,Error)) return false;
			const TSharedPtr<FJsonObject>* Categories=nullptr; if(!Document->TryGetObjectField(TEXT("categories"),Categories)) continue;
			for(const auto& Category:(*Categories)->Values) {
				const TSharedPtr<FJsonObject>* Property=nullptr; if(!Category.Value->AsObject()->TryGetObjectField(TEXT("bRunConstructionScriptOnDrag"),Property)) continue;
				++Matches; if((*Property)->GetBoolField(TEXT("value"))!=bEdit) return false;
				if(bEdit) { (*Property)->SetBoolField(TEXT("value"),false); if(!FUeAgentStructuredHjsonFormatter::TryWriteObject(Document.ToSharedRef(),Text,Error) || !FFileHelper::SaveStringToFile(Text,*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false; }
			}
		}
		return Matches==1;
	};
	if(!TestTrue(TEXT("public root export"),ExportTo(Workspace))) return false;
	auto Source=MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"),TEXT("directory")); Source->SetStringField(TEXT("path"),Workspace);
	auto Args=MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"),ObjectPath); Args->SetObjectField(TEXT("source"),Source);
	const auto NoChange=Tests::PublicCommand::SubmitActiveCommand(Registry,Dispatch,TEXT("content_graph.import_workspace"),Args,TEXT("t85-blueprint-root-nochange"));
	if(!TestTrue(TEXT("unedited public workspace retains common no-change rejection"),!NoChange.IsOk() && NoChange.GetErrorJson().Contains(TEXT("hjson_import_no_explicit_changes")))) { AddError(NoChange.GetErrorJson()); return false; }
	TestTrue(TEXT("no-change preserves native root and graph"),Blueprint->bRunConstructionScriptOnDrag && Before.OrderIndependentCompareEqual(GraphState(Blueprint)) && !Blueprint->GetOutermost()->IsDirty());
	if(!TestTrue(TEXT("ordinary root property edit"),EditOrRead(Workspace,true))) { AddError(Error.ToPublicDetail()); return false; }
	Blueprint=nullptr; Package=nullptr;
	auto Imported=Tests::PublicCommand::SubmitActiveCommand(Registry,Dispatch,TEXT("content_graph.import_workspace"),Args,TEXT("t85-blueprint-root-import"));
	if(!TestTrue(TEXT("public root import"),Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	if(!TestTrue(TEXT("independent blueprint unload"),T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName,Error))) { AddError(Error.ToPublicDetail()); return false; }
	Blueprint=LoadObject<UBlueprint>(nullptr,*ObjectPath); if(!TestNotNull(TEXT("independent Blueprint disk reload"),Blueprint)) return false;
	TestFalse(TEXT("root drag option persisted"),Blueprint->bRunConstructionScriptOnDrag);
	TestTrue(TEXT("parent generated class and compile retained"),Blueprint->ParentClass==AActor::StaticClass() && Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetPathName()==GeneratedPath && (Blueprint->Status==BS_UpToDate || Blueprint->Status==BS_UpToDateWithWarnings));
	TestTrue(TEXT("unedited native graph structure retained"),Before.OrderIndependentCompareEqual(GraphState(Blueprint)));
	TestFalse(TEXT("persisted Blueprint clean"),Blueprint->GetOutermost()->IsDirty());
	if(!TestTrue(TEXT("independent public root reexport"),ExportTo(Readback)) || !TestTrue(TEXT("ordinary root false readback"),EditOrRead(Readback,false))) AddError(Error.ToPublicDetail());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85BlueprintAssetWorkspaceVerticalTest,
	"UAI.Root.T85.Blueprint.AssetWorkspaceVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85BlueprintAssetWorkspaceVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85BlueprintAssetWorkspace/BP_Actor_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		TestFalse(TEXT("created fixture registry absent after cleanup"), FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid());
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString ParamsError;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry,Dispatch,ParamsError)) { AddError(ParamsError); return false; }
	TSharedPtr<FJsonObject> Params;
	if (!TryBuildPublicBlueprintCreateParams(ObjectPath,Params,ParamsError)) { AddError(ParamsError); return false; }
	const auto Imported=Tests::PublicCommand::SubmitActiveCommand(Registry,Dispatch,TEXT("content_graph.import_workspace"),Params.ToSharedRef(),TEXT("t85-blueprint-public-create"));
	if (!TestTrue(TEXT("public inline Blueprint creation"),Imported.IsOk())) { AddError(Imported.GetErrorJson()); return false; }
	const FString ImportPayloadJson=Imported.GetPayloadJson();

	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(TEXT("the Blueprint import payload is canonical JSON"),
		CommandContract::Json::TryParseObject(
			ImportPayloadJson, ImportPayload, PayloadError)))
	{
		AddError(PayloadError);
		return false;
	}
	TestTrue(TEXT("the import exposes only its compact success status"),
		ImportPayload.IsValid()
			&& ImportPayload->Values.Num() == 1
			&& ImportPayload->GetStringField(TEXT("status")) == TEXT("succeeded"));

	if (!TestTrue(TEXT("independent created Blueprint unload"),T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName,ParamsError))) { AddError(ParamsError); return false; }
	UBlueprint* const Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted Blueprint independently reloads"), Reloaded))
	{
		return false;
	}
	TestEqual(TEXT("the created asset is an ordinary Blueprint"),
		Reloaded->GetClass(), UBlueprint::StaticClass());
	TestTrue(TEXT("created Blueprint registry entry present"), FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid());
	TestEqual(TEXT("the created Blueprint has the Actor parent"),
		Reloaded->ParentClass.Get(), AActor::StaticClass());
	TestTrue(TEXT("the created Blueprint has a generated Actor class"),
		Reloaded->GeneratedClass
			&& Reloaded->GeneratedClass->IsChildOf(AActor::StaticClass()));
	TestEqual(TEXT("the reloaded Blueprint compile status is valid"),
		Reloaded->Status, BS_UpToDate);

	TestFalse(TEXT("created Blueprint clean"),Reloaded->GetOutermost()->IsDirty());
	const FString Workspace=FPaths::ProjectSavedDir()/TEXT("Automation/UAI/T85/BlueprintCreate")/AssetName;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Workspace,false,true); };
	auto ExportArgs=MakeShared<FJsonObject>(); ExportArgs->SetStringField(TEXT("target"),ObjectPath); ExportArgs->SetStringField(TEXT("workspace_path"),Workspace); ExportArgs->SetStringField(TEXT("profile_id"),TEXT("llm_authoring"));
	const auto Exported=Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry,Dispatch,TEXT("content_graph.export_workspace"),ExportArgs,TEXT("t85-blueprint-created-reexport"),{TEXT("uai:query"),TEXT("uai:export"),TEXT("uai:operate")});
	if (!TestTrue(TEXT("public created Blueprint reexport"),Exported.IsOk())) { AddError(Exported.GetErrorJson()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files,*Workspace,TEXT("properties.hjson"),true,false); bool bReadDefault=false;
	for (const FString& File:Files) {
		FString Text; TSharedPtr<FJsonObject> Value; if (!FFileHelper::LoadFileToString(Text,*File)||!FUeAgentStructuredHjsonParser::ParseObject(Text,File,Value,ParamsError)) { AddError(ParamsError); return false; }
		const TSharedPtr<FJsonObject>* Categories=nullptr; if (!Value->TryGetObjectField(TEXT("categories"),Categories)) continue;
		for (const auto& Category:(*Categories)->Values) { const TSharedPtr<FJsonObject>* Property=nullptr; if (Category.Value->AsObject()->TryGetObjectField(TEXT("bRunConstructionScriptOnDrag"),Property)) bReadDefault=(*Property)->GetBoolField(TEXT("value"))==Reloaded->bRunConstructionScriptOnDrag; }
	}
	TestTrue(TEXT("ordinary created root matches native default"),bReadDefault);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Blueprint fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
