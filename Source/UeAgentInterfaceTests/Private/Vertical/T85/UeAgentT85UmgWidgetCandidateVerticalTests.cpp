#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
	using namespace UE::UAI;

	constexpr const TCHAR* Fingerprint =
		TEXT("sha256:8888888888888888888888888888888888888888888888888888888888888888");

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
		const FString& CandidateId,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("id"), CandidateId);
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

		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("index.hjson"));
		Entry->SetStringField(TEXT("document"), DocumentText);
		Entry->SetStringField(TEXT("content_hash"), Fingerprint);
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("carrier"), TEXT("inline"));
		Source->SetStringField(
			TEXT("source_unit"), TEXT("t85-umg-widget-candidate-vertical"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("base_revision"), TEXT(""));
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		Source->SetStringField(TEXT("source_fingerprint"), Fingerprint);
		Source->SetStringField(TEXT("semantic_fingerprint"), Fingerprint);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});

		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), ExpectedRevision);
		Params->SetObjectField(TEXT("source"), Source);
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
			CandidateError,
			64,
			0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
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
	TestTrue(TEXT("the selected Button exposes a compact id"),
		Selected->Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("the partial catalog is not reported as the full Palette"),
		CandidateResult.bCoverageComplete);
	TestFalse(TEXT("candidate search leaves the saved package clean"),
		Package->IsDirty());

	FString CanonicalParams;
	FString ParamsError;
	if (!TestTrue(TEXT("id-only Widget HJSON canonicalizes"),
		TryBuildResolvedUmgImportParams(
			PackageName,
			CandidateResult.TargetRevision,
			Selected->Id,
			CanonicalParams,
			ParamsError)))
	{
		AddError(ParamsError);
		return false;
	}
	TestFalse(TEXT("the model input contains no widget class binding"),
		CanonicalParams.Contains(TEXT("\"class\"")));

	FString ImportPayloadJson;
	bool bReadOnly = true;
	HjsonWorkspace::FWorkspaceError ImportError;
	if (!TestTrue(TEXT("id-only HJSON creates and persists the Button"),
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
	FString TerminalStatus;
	bool bSaved = false;
	bool bReloaded = false;
	TestTrue(TEXT("the import reports the persisted reload lifecycle"),
		ImportPayload.IsValid()
			&& ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus)
			&& TerminalStatus == TEXT("succeeded")
			&& ImportPayload->TryGetBoolField(TEXT("saved"), bSaved)
			&& bSaved
			&& ImportPayload->TryGetBoolField(TEXT("reloaded"), bReloaded)
			&& bReloaded);

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

	DeleteAssetIfPresent(ObjectPath);
	UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(
		*this, PackageName);
	TestFalse(TEXT("the task-owned Widget fixture leaves no asset file"),
		IFileManager::Get().FileExists(*Filename));
	return !HasAnyErrors();
}
