#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

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

	bool SaveWidgetBlueprint(
		UPackage& Package,
		UWidgetBlueprint& Blueprint,
		const FString& Filename)
	{
		Package.MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(
			&Package, &Blueprint, *Filename, Args);
		if (bSaved)
		{
			Package.SetDirtyFlag(false);
		}
		return bSaved;
	}

	bool TryBuildResolvedUmgImportParams(
		const FString& Target,
		const FString& ExpectedRevision,
		const FString& CandidateName,
		FString& OutParams,
		FString& OutError,
		const TMap<FString, FString>& Select = {})
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), CandidateName);
		if (!Select.IsEmpty())
		{
			auto Selection = MakeShared<FJsonObject>();
			for (const auto& Field : Select) Selection->SetStringField(Field.Key, Field.Value);
			Candidate->SetObjectField(TEXT("select"), Selection);
		}
		const TSharedRef<FJsonObject> Button = MakeShared<FJsonObject>();
		Button->SetStringField(TEXT("operation"), TEXT("create"));
		Button->SetObjectField(TEXT("candidate"), Candidate);
		Button->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
		Button->SetNumberField(TEXT("index"), 0);
		const TSharedRef<FJsonObject> Widgets = MakeShared<FJsonObject>();
		Widgets->SetObjectField(TEXT("widget:ActionButton"), Button);
		const TSharedRef<FJsonObject> WidgetTree = MakeShared<FJsonObject>();
		WidgetTree->SetObjectField(TEXT("widgets"), Widgets);
		const TSharedRef<FJsonObject> ProviderExtension = MakeShared<FJsonObject>();
		ProviderExtension->SetObjectField(TEXT("widget_tree"), WidgetTree);
		const TSharedRef<FJsonObject> DocumentTarget = MakeShared<FJsonObject>();
		DocumentTarget->SetStringField(TEXT("asset_path"), Target);
		const TSharedRef<FJsonObject> AuthoringDocument =
			MakeShared<FJsonObject>();
		AuthoringDocument->SetStringField(TEXT("operation"), TEXT("update"));
		AuthoringDocument->SetObjectField(TEXT("target"), DocumentTarget);
		AuthoringDocument->SetObjectField(
			TEXT("provider_extension"), ProviderExtension);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("type"), TEXT("/Script/UMGEditor.WidgetBlueprint"));
		Root->SetObjectField(TEXT("document"), AuthoringDocument);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("uai.umg.edit/1"));
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
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr ResolvedSource;
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		auto Resources =
			HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		if (!HjsonWorkspace::TryLoadContentGraphImportSource(
				Source,
				Target,
				Resources,
				ResolvedSource,
				WorkspaceError)
			|| !ResolvedSource.IsValid())
		{
			OutError = WorkspaceError.GetCode() + TEXT(":")
				+ WorkspaceError.GetSourceUnit();
			return false;
		}

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(
			TEXT("source"), ResolvedSource->MakeNormalizedSourceObject());
		return CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85UmgWidgetCandidateVerticalTest,
	"UAI.Root.T85.UMG.WidgetCandidateVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85UmgWidgetCandidateVerticalTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	using namespace UE::UAI::QueryOwners::UmgWidget;

	const FString PackageName =
		TEXT("/Game/UAI/Transient/T85UmgWidget/WBP_Candidate_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(ObjectPath);
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(ObjectPath);
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
			*this, PackageName);
	};

	UPackage* const Package = CreatePackage(*PackageName);
	UWidgetBlueprint* const Blueprint = Package
		? Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			Package,
			*AssetName,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			NAME_None))
		: nullptr;
	if (!TestNotNull(TEXT("a persistent WidgetBlueprint fixture is created"),
		Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	if (!TestNotNull(TEXT("the fixture owns one WidgetTree"),
		Blueprint->WidgetTree.Get()))
	{
		return false;
	}
	UCanvasPanel* const RootCanvas =
		Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
			UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	if (!TestNotNull(TEXT("the fixture root CanvasPanel is created"), RootCanvas))
	{
		return false;
	}
	Blueprint->WidgetTree->RootWidget = RootCanvas;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!TestTrue(TEXT("the initial WidgetBlueprint fixture is saved"),
		SaveWidgetBlueprint(*Package, *Blueprint, Filename)))
	{
		return false;
	}

	FUmgWidgetCandidateQueryResult CandidateResult;
	FUmgWidgetCandidateQueryError CandidateError;
	if (!TestTrue(TEXT("the partial Widget catalog searches Button"),
		TryQueryUmgWidgetCandidates(
			*Blueprint,
			TEXT("Button"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FUmgWidgetCandidate* const Selected =
		CandidateResult.Candidates.FindByPredicate(
			[](const FUmgWidgetCandidate& Candidate)
			{
				return Candidate.ClassPath
					== UButton::StaticClass()->GetPathName()
					&& Candidate.bSupported;
			});
	if (!TestNotNull(TEXT("one writable native Button candidate is available"),
		Selected))
	{
		return false;
	}
	TestFalse(TEXT("the selected Button exposes a non-empty name"),
		Selected->Name.IsEmpty());
	TestFalse(TEXT("candidate search leaves the saved package clean"),
		Package->IsDirty());
	TestEqual(TEXT("the returned native name has no synthetic scope"),
		Selected->Name, FString(TEXT("Button")));
	TSet<FString> Names;
	for (const auto& Candidate : CandidateResult.Candidates)
	{
		TestFalse(TEXT("ordinary query names are unique before paging"), Names.Contains(Candidate.Name));
		Names.Add(Candidate.Name);
	}

	FString CanonicalParams;
	FString ParamsError;
	TArray<uint8> BeforeBytes;
	if (!TestTrue(TEXT("the saved baseline can be read"), FFileHelper::LoadFileToArray(BeforeBytes, *Filename))
		|| !TestTrue(TEXT("invalid selection input canonicalizes"),
			TryBuildResolvedUmgImportParams(PackageName, CandidateResult.TargetRevision,
				Selected->Name, CanonicalParams, ParamsError,
				{{TEXT("form"), TEXT("missing widget form")}}))) return false;
	FString RejectedPayload;
	bool bRejectedReadOnly = true;
	HjsonWorkspace::FWorkspaceError RejectedError;
	TestFalse(TEXT("invalid selection is rejected before native mutation"),
		DomainAdapters::UmgWidget::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"), PackageName, CanonicalParams,
			RejectedPayload, bRejectedReadOnly, RejectedError));
	TestEqual(TEXT("common selection code survives preflight"),
		RejectedError.GetCode(), FString(TEXT("candidate_selector_invalid")));
	TSharedPtr<FJsonObject> Recovery;
	FString RecoveryError;
	TestTrue(TEXT("common recovery retains the original candidate name"),
		CommandContract::Json::TryParseObject(RejectedError.GetJsonPointer(), Recovery, RecoveryError)
			&& Recovery->GetStringField(TEXT("name")) == TEXT("Button"));
	TArray<uint8> AfterBytes;
	TestTrue(TEXT("failed selection preserves saved bytes"),
		FFileHelper::LoadFileToArray(AfterBytes, *Filename) && BeforeBytes == AfterBytes);
	TestTrue(TEXT("failed selection preserves the existing root instance"),
		Blueprint->WidgetTree->RootWidget == RootCanvas);
	TestNull(TEXT("failed selection creates no Button"),
		Blueprint->WidgetTree->FindWidget(TEXT("ActionButton")));
	TestFalse(TEXT("failed selection leaves the original package clean"), Package->IsDirty());
	if (HasAnyErrors()) return false;
	if (!TestTrue(TEXT("name/select Widget HJSON canonicalizes"),
		TryBuildResolvedUmgImportParams(
			PackageName,
			CandidateResult.TargetRevision,
			Selected->Name,
			CanonicalParams,
			ParamsError,
			{{TEXT("form"), TEXT("native widget")}})))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the model input contains no widget class binding"),
		CanonicalParams.Contains(TEXT("\"class\"")));

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("name/select HJSON creates and persists the Button"),
		DomainAdapters::UmgWidget::TryExecuteContentGraph(
			TEXT("content_graph.import_workspace"),
			PackageName,
			CanonicalParams,
			ImportPayloadJson,
			bReadOnly,
			ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":")
			+ ImportError.GetSourceUnit() + TEXT(":")
			+ ImportError.GetJsonPointer());
		return false;
	}
	TestFalse(TEXT("the UMG import is a mutation"), bReadOnly);

	TSharedPtr<FJsonObject> ImportPayload;
	FString PayloadError;
	if (!TestTrue(TEXT("the UMG import payload is canonical JSON"),
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

	UPackage* const PersistedPackage = FindPackage(nullptr, *PackageName);
	if (!TestNotNull(TEXT("the imported package is available for independent unload"), PersistedPackage)
		|| !TestTrue(TEXT("independent unload succeeds"),
			UPackageTools::UnloadPackages({PersistedPackage}))) return false;
	UWidgetBlueprint* const Reloaded =
		LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the persisted WidgetBlueprint reloads"), Reloaded)
		|| !TestNotNull(TEXT("the reloaded asset owns a WidgetTree"),
			Reloaded ? Reloaded->WidgetTree.Get() : nullptr))
	{
		return false;
	}
	UWidget* const ReloadedRoot = Reloaded->WidgetTree->FindWidget(
		FName(TEXT("RootCanvas")));
	UWidget* const ReloadedButton = Reloaded->WidgetTree->FindWidget(
		FName(TEXT("ActionButton")));
	TestTrue(TEXT("the original root survives reload"),
		ReloadedRoot == Reloaded->WidgetTree->RootWidget
			&& ReloadedRoot->GetClass() == UCanvasPanel::StaticClass());
	TestNotNull(TEXT("the candidate-created Button survives reload"),
		ReloadedButton);
	if (ReloadedButton)
	{
		TestEqual(TEXT("the exact candidate class survives reload"),
			ReloadedButton->GetClass(), UButton::StaticClass());
		UCanvasPanelSlot* const Slot = Cast<UCanvasPanelSlot>(ReloadedButton->Slot);
		TestNotNull(TEXT("the explicit parent creates a CanvasPanelSlot"), Slot);
		if (Slot)
		{
			TestEqual(TEXT("the slot retains the exact parent"),
				Slot->Parent.Get(), Cast<UPanelWidget>(ReloadedRoot));
		}
	}
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(),
		TEXT("tmp/TROOT-UAI-208-graph-common-foundation/umg-name-selection-r1/test-workspaces"), AssetName));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*WorkspacePath, false, true); };
	UeAgentExportWorkspaceWriter::FWorkspaceResult ExportResult;
	FString ExportError;
	if (!TestTrue(TEXT("ordinary Widget workspace exports"),
		UeAgentUmgAuthoring::ExportAssetWorkspace(*Reloaded, WorkspacePath, TEXT("llm_authoring"), ExportResult)))
	{
		AddError(ExportError); return false;
	}
	FString WidgetFile;
	FString WidgetText;
	TSharedPtr<FJsonObject> WidgetDocument;
	TArray<FString> StructureFiles;
	IFileManager::Get().FindFilesRecursive(StructureFiles,
		*FPaths::Combine(WorkspacePath, TEXT("structures")), TEXT("*.hjson"), true, false);
	for (const FString& File : StructureFiles)
	{
		TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(WidgetText, *File)
			|| !FUeAgentStructuredHjsonParser::ParseObject(WidgetText, File, Document, ExportError))
		{
			AddError(ExportError); return false;
		}
		if (!Document->HasTypedField<EJson::Object>(TEXT("widgets"))) continue;
		if (!TestTrue(TEXT("one ordinary widgets document exists"), WidgetFile.IsEmpty())) return false;
		WidgetFile = File;
		WidgetDocument = Document;
	}
	if (!TestTrue(TEXT("ordinary widgets document is located by its public field"), WidgetDocument.IsValid())) return false;
	const TSharedPtr<FJsonObject>* VisibleWidgets = nullptr;
	if (!TestTrue(TEXT("the shared keyed projection exposes widgets"),
		WidgetDocument->TryGetObjectField(TEXT("widgets"), VisibleWidgets) && VisibleWidgets)) return false;
	TestTrue(TEXT("existing widgets have readable keys"),
		(*VisibleWidgets)->HasField(TEXT("RootCanvas")) && (*VisibleWidgets)->HasField(TEXT("ActionButton")));
	auto AddedButton = MakeShared<FJsonObject>();
	AddedButton->SetStringField(TEXT("name"), TEXT("Button"));
	AddedButton->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
	AddedButton->SetNumberField(TEXT("index"), 1);
	(*VisibleWidgets)->SetObjectField(TEXT("SecondButton"), AddedButton);
	if (!TestTrue(TEXT("ordinary name-only edit serializes"),
		CommandContract::Json::TryCanonicalize(WidgetDocument, WidgetText, ExportError)
			&& FFileHelper::SaveStringToFile(WidgetText, *WidgetFile,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	auto Directory = MakeShared<FJsonObject>();
	Directory->SetStringField(TEXT("kind"), TEXT("directory"));
	Directory->SetStringField(TEXT("path"), WorkspacePath);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr DirectorySource;
	auto DirectoryResources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	FString DirectoryRevision, DirectoryAuthorization;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Directory, PackageName, DirectoryResources,
		DirectorySource, DirectoryRevision, DirectoryAuthorization, ImportError) || !DirectorySource.IsValid())
	{
		AddError(ImportError.GetCode()); return false;
	}
	auto DirectoryParams = MakeShared<FJsonObject>();
	DirectoryParams->SetStringField(TEXT("target"), PackageName);
	DirectoryParams->SetStringField(TEXT("expected_revision"), DirectoryRevision);
	DirectoryParams->SetObjectField(TEXT("source"), DirectorySource->MakeNormalizedSourceObject());
	if (!CommandContract::Json::TryCanonicalize(DirectoryParams, CanonicalParams, ExportError)) return false;
	if (!TestTrue(TEXT("ordinary directory creates another Button"),
		DomainAdapters::UmgWidget::TryExecuteContentGraph(TEXT("content_graph.import_workspace"),
			PackageName, CanonicalParams, ImportPayloadJson, bReadOnly, ImportError)))
	{
		AddError(ImportError.GetCode() + TEXT(":") + ImportError.GetSourceUnit() + TEXT(":") + ImportError.GetJsonPointer());
		return false;
	}
	UPackage* DirectoryPackage = FindPackage(nullptr, *PackageName);
	if (!DirectoryPackage || !UPackageTools::UnloadPackages({DirectoryPackage})) return false;
	UWidgetBlueprint* DirectoryReloaded = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("directory result reloads independently"), DirectoryReloaded)) return false;
	const UButton* SecondButton = Cast<UButton>(DirectoryReloaded->WidgetTree->FindWidget(TEXT("SecondButton")));
	TestNotNull(TEXT("the ordinary name resolves to native Button"), SecondButton);
	TestNotNull(TEXT("the original Button survives the ordinary edit"),
		DirectoryReloaded->WidgetTree->FindWidget(TEXT("ActionButton")));
	TestTrue(TEXT("the ordinary result is clean"), !DirectoryReloaded->GetOutermost()->IsDirty());

	TArray<uint8> BeforeLateBytes;
	if (!FFileHelper::LoadFileToArray(BeforeLateBytes, *Filename)) return false;
	UWidget* DirtyButton = DirectoryReloaded->WidgetTree->FindWidget(TEXT("ActionButton"));
	if (!TestNotNull(TEXT("the accepted Button supplies a dirty-memory baseline"), DirtyButton)) return false;
	const bool bSavedVariable = DirtyButton->bIsVariable;
	DirtyButton->bIsVariable = !bSavedVariable;
	DirectoryReloaded->MarkPackageDirty();
	TWeakObjectPtr<UWidgetBlueprint> BeforeNative = DirectoryReloaded;
	auto LateButton = MakeShared<FJsonObject>();
	LateButton->SetStringField(TEXT("operation"), TEXT("create"));
	auto LateCandidate = MakeShared<FJsonObject>();
	LateCandidate->SetStringField(TEXT("name"), TEXT("Button"));
	LateButton->SetObjectField(TEXT("candidate"), LateCandidate);
	LateButton->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
	LateButton->SetNumberField(TEXT("index"), 2);
	auto LateWidgets = MakeShared<FJsonObject>();
	LateWidgets->SetObjectField(TEXT("widget:LateButton"), LateButton);
	auto LateTree = MakeShared<FJsonObject>();
	LateTree->SetObjectField(TEXT("widgets"), LateWidgets);
	UeAgentUmgAuthoring::FAssetApplyRequest LateRequest;
	LateRequest.AssetPath = PackageName;
	LateRequest.ProviderExtension = MakeShared<FJsonObject>();
	LateRequest.ProviderExtension->SetObjectField(TEXT("widget_tree"), LateTree);
	LateRequest.ProviderExtension->SetBoolField(TEXT("compile"), true);
	LateRequest.ProviderExtension->SetBoolField(TEXT("save"), true);
	bool bObservedPublished = false;
	LateRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
	{
		UWidgetBlueprint* Current = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		TArray<uint8> PublishedBytes;
		UWidget* CurrentButton = Current && Current->WidgetTree
			? Current->WidgetTree->FindWidget(TEXT("ActionButton")) : nullptr;
		bObservedPublished = !BeforeNative.IsValid() && Current && Current->WidgetTree
			&& Cast<UButton>(Current->WidgetTree->FindWidget(TEXT("LateButton")))
			&& CurrentButton && CurrentButton->bIsVariable == !bSavedVariable
			&& !Current->GetOutermost()->IsDirty()
			&& FFileHelper::LoadFileToArray(PublishedBytes, *Filename)
			&& PublishedBytes != BeforeLateBytes;
		TestTrue(TEXT("late failure occurs after native creation and clean disk publication"), bObservedPublished);
		Failure.Diagnostic.Code = TEXT("test_umg_late_rejection");
		return false;
	};
	UeAgentUmgAuthoring::FAssetApplyResult LateResult;
	TestFalse(TEXT("final readback rejection fails the transaction"),
		UeAgentUmgAuthoring::ApplyAsset(LateRequest, LateResult, TEXT("umg-atomic-late")));
	TestEqual(TEXT("the original late failure survives recovery"), LateResult.Diagnostic.Code, FString(TEXT("test_umg_late_rejection")));
	TestTrue(TEXT("the common checkpoint restores after unload"),
		bObservedPublished && LateResult.bRollbackAttempted && LateResult.bRollbackSucceeded);
	UWidgetBlueprint* Restored = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!TestNotNull(TEXT("the restored WidgetBlueprint resolves"), Restored)) return false;
	TestNull(TEXT("the rejected Button is absent after recovery"), Restored->WidgetTree->FindWidget(TEXT("LateButton")));
	UCanvasPanel* RestoredRoot = Cast<UCanvasPanel>(Restored->WidgetTree->RootWidget);
	TestTrue(TEXT("both accepted Buttons and their parent survive recovery"), RestoredRoot
		&& RestoredRoot->GetChildrenCount() == 2
		&& Cast<UButton>(Restored->WidgetTree->FindWidget(TEXT("ActionButton")))
		&& Cast<UButton>(Restored->WidgetTree->FindWidget(TEXT("SecondButton"))));
	TArray<uint8> RestoredBytes;
	TestTrue(TEXT("late rejection restores original disk bytes"),
		FFileHelper::LoadFileToArray(RestoredBytes, *Filename) && RestoredBytes == BeforeLateBytes);
	TestTrue(TEXT("late rejection restores dirty memory independently of original disk"),
		Restored->GetOutermost()->IsDirty()
		&& Restored->WidgetTree->FindWidget(TEXT("ActionButton"))->bIsVariable == !bSavedVariable);

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Widget fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
