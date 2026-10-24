#include "Animation/WidgetAnimation.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Domains/UMG/Authoring/UeAgentUmgAnimationState.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneScalarChannel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/UmgWidget/UeAgentUmgWidgetMechanics.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringRegistration.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringRegistration.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringExport.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UeAgentT85FixtureLifecycle.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "KeyPropertyParams.h"
#include "PreviewScene.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Serialization/ObjectWriter.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	class FUmgInlineNeverCancelled final : public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	TSharedRef<FJsonObject> InlineLifecycleOptions()
	{
		auto Extension = MakeShared<FJsonObject>();
		Extension->SetBoolField(TEXT("compile"), true);
		Extension->SetBoolField(TEXT("save"), true);
		return Extension;
	}

	TMap<FString, FString> CaptureUmgNativeContents(UWidgetBlueprint& Blueprint)
	{
		TMap<FString, FString> State;
		State.Add(TEXT("category"), Blueprint.BlueprintCategory);
		TArray<UWidget*> Widgets;
		Blueprint.WidgetTree->GetAllWidgets(Widgets);
		for (const UWidget* Widget : Widgets)
		{
			State.Add(TEXT("widget:") + Widget->GetName(), Widget->GetClass()->GetPathName()
				+ TEXT(":") + GetNameSafe(Widget->GetParent()) + TEXT(":") + LexToString(Widget->bIsVariable));
		}
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			State.Add(TEXT("graph:") + Graph->GetName(), Graph->GetClass()->GetPathName());
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const FString Key = TEXT("node:") + Node->NodeGuid.ToString();
				State.Add(Key, Graph->GetName() + TEXT(":") + Node->GetClass()->GetPathName()
					+ FString::Printf(TEXT(":%d:%d"), Node->NodePosX, Node->NodePosY));
				if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node)) State.Add(Key + TEXT(":event"), Event->CustomFunctionName.ToString());
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					TArray<FString> Links;
					for (const UEdGraphPin* Link : Pin->LinkedTo) Links.Add(Link->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + Link->PinName.ToString());
					Links.Sort();
					State.Add(Key + TEXT(":pin:") + Pin->PinName.ToString(), Pin->DefaultValue + TEXT(":")
						+ LexToString(int32(Pin->Direction)) + TEXT(":") + FString::Join(Links, TEXT(",")));
				}
			}
		}
		return State;
	}

	bool RunUmgInlineAtomic(FAutomationTestBase& Test, const bool bCreate, const bool bGraphEdit = false, const bool bMemberEdit = false)
	{
		using namespace UeAgentUmgAuthoring;
		using namespace UE::UAI::DomainAdapters::AssetDestructiveImport;
		UE::UAI::Contract::FDiagnostic Error;
		if (!Test.TestTrue(TEXT("Blueprint authoring dependencies register"), UeAgentBlueprintAuthoring::RegisterAuthoring(Error))) return false;
		if (!Test.TestTrue(TEXT("UMG authoring contracts register"), RegisterAuthoring(Error))) return false;
		const FString Root = TEXT("/Game/UAI/Transient/T85UmgInline_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = Root + TEXT("/Widget");
		const FString ObjectPath = PackageName + TEXT(".Widget");
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		TSharedPtr<IUeAgentAssetDeleteOwner> DeleteOwner;
		UE::UAI::Contract::FDiagnostic DeleteOwnerError;
		if (!TryCreateAssetDeleteOwner(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/UmgInlineDelete"), TEXT("umg-inline"), DeleteOwner, DeleteOwnerError)) return false;
		ON_SCOPE_EXIT
		{
			if (UObject* Remaining = FindObject<UObject>(nullptr, *ObjectPath))
			{
				Remaining->GetOutermost()->SetDirtyFlag(false);
				const auto Deleted = DeleteOwner->Delete({TEXT("umg-inline-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FUmgInlineNeverCancelled>());
				Test.TestTrue(TEXT("owned Widget fixture deletes through the formal owner"), Deleted.IsSuccess());
			}
			UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
		};
		UWidgetBlueprint* Blueprint = nullptr;
		TWeakObjectPtr<UEdGraphNode> OriginalNode;
		TArray<uint8> DiskBefore;
		TMap<FString, FString> NativeBefore;
		FGuid PrintGuid;
		bool InitialRoot = false;
		if (!bCreate)
		{
			UPackage* Package = CreatePackage(*PackageName);
			Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package, TEXT("Widget"), BPTYPE_Normal,
				UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None));
			if (!Test.TestNotNull(TEXT("the native WidgetBlueprint fixture exists"), Blueprint)) return false;
			FAssetRegistryModule::AssetCreated(Blueprint);
			UCanvasPanel* Canvas = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			Blueprint->WidgetTree->RootWidget = Canvas;
			UButton* Button = Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("PreservedButton"));
			Canvas->AddChild(Button);
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("PreservedGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
			FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
			UK2Node_CustomEvent* Event = Creator.CreateNode();
			Event->CustomFunctionName = TEXT("PreservedEvent");
			Creator.Finalize();
			Event->NodePosX = 120;
			Event->NodePosY = 80;
			UK2Node_CallFunction* Print = nullptr;
			if (bGraphEdit)
			{
				FGraphNodeCreator<UK2Node_CallFunction> PrintCreator(*Graph);
				Print = PrintCreator.CreateNode();
				Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
				PrintCreator.Finalize();
				Print->NodePosX = 500; Print->NodePosY = 80;
				Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("DiskGraphBefore");
				PrintGuid = Print->NodeGuid;
				if (!Graph->GetSchema()->TryCreateConnection(Event->FindPinChecked(UEdGraphSchema_K2::PN_Then), Print->GetExecPin())) return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			if (bGraphEdit) CastChecked<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.625f);
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			if (!Test.TestTrue(TEXT("the existing native baseline saves"), UPackage::SavePackage(Package, Blueprint, *Filename, Args))) return false;
			Package->SetDirtyFlag(false);
			if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
			InitialRoot = Blueprint->bRunConstructionScriptOnDrag;
			Blueprint->BlueprintCategory = TEXT("DirtyInlineCategory");
			Button->bIsVariable = !Button->bIsVariable;
			Event->NodePosX = 240;
			if (Print) { Print->NodePosX = 600; Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("DirtyGraphBefore"); }
			if (bGraphEdit) CastChecked<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.375f);
			Blueprint->MarkPackageDirty();
			OriginalNode = Event;
			NativeBefore = CaptureUmgNativeContents(*Blueprint);
		}
		TWeakObjectPtr<UWidgetBlueprint> SavedOriginal;
		bool SawPreSave = false;
		const FDelegateHandle SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda(
			[&](UObject* Object, FObjectPreSaveContext)
			{
				if (Object && Object->GetPathName() == ObjectPath)
				{
					SavedOriginal = Cast<UWidgetBlueprint>(Object);
					SawPreSave = SavedOriginal.IsValid();
				}
			});
		ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle); };
		FAssetApplyRequest Request;
		Request.AssetPath = PackageName;
		Request.Operation = bCreate ? UeAgentApplyInputContract::EOperation::Create : UeAgentApplyInputContract::EOperation::Update;
		Request.ProviderExtension = InlineLifecycleOptions();
		if (bMemberEdit)
		{
			auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("int"));
			auto Default = MakeShared<FJsonObject>(); Default->SetStringField(TEXT("property_path"), TEXT("LateCount")); Default->SetNumberField(TEXT("value"), 31);
			auto Variable = MakeShared<FJsonObject>(); Variable->SetObjectField(TEXT("type"), Type); Variable->SetObjectField(TEXT("default_property"), Default);
			auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(TEXT("LateCount"), Variable);
			auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables);
			Request.ProviderExtension->SetObjectField(TEXT("members"), Members);
		}
		TMap<FString, FString> PublishedExpected = NativeBefore;
		if (bGraphEdit)
		{
			auto Pin = MakeShared<FJsonObject>();
			Pin->SetStringField(TEXT("pin_name"), TEXT("InString"));
			Pin->SetNumberField(TEXT("pin_occurrence"), 0);
			Pin->SetStringField(TEXT("default_value"), TEXT("PublishedGraphAfter"));
			auto Pins = MakeShared<FJsonObject>(); Pins->SetObjectField(TEXT("pin:InString:0"), Pin);
			auto Position = MakeShared<FJsonObject>(); Position->SetNumberField(TEXT("x"), 780); Position->SetNumberField(TEXT("y"), 160);
			auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("operation"), TEXT("update"));
			Node->SetStringField(TEXT("node_guid"), PrintGuid.ToString()); Node->SetObjectField(TEXT("pos"), Position); Node->SetObjectField(TEXT("pin_defaults"), Pins);
			auto Nodes = MakeShared<FJsonObject>(); Nodes->SetObjectField(PrintGuid.ToString(), Node);
			auto Graph = MakeShared<FJsonObject>(); Graph->SetStringField(TEXT("name"), TEXT("PreservedGraph"));
			Graph->SetStringField(TEXT("graph_kind"), TEXT("event_graph")); Graph->SetStringField(TEXT("operation"), TEXT("update")); Graph->SetObjectField(TEXT("nodes"), Nodes);
			auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(TEXT("event_graph:PreservedGraph"), Graph);
			Request.ProviderExtension->SetObjectField(TEXT("graphs"), Graphs);
			const FString Key = TEXT("node:") + PrintGuid.ToString();
			PublishedExpected[Key] = PublishedExpected[Key].Replace(TEXT(":600:80"), TEXT(":780:160"));
			PublishedExpected[Key + TEXT(":pin:InString")] = PublishedExpected[Key + TEXT(":pin:InString")].Replace(TEXT("DirtyGraphBefore"), TEXT("PublishedGraphAfter"));
		}
		auto Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("property_path"), bCreate ? TEXT("BlueprintCategory") : TEXT("bRunConstructionScriptOnDrag"));
		if (bCreate) Property->SetStringField(TEXT("value"), TEXT("CreatedInlineCategory"));
		else Property->SetBoolField(TEXT("value"), !InitialRoot);
		Request.Properties.Add(MakeShared<FJsonValueObject>(Property));
		bool ObservedPublished = false;
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			UWidgetBlueprint* Published = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
			TArray<uint8> PublishedBytes;
			ObservedPublished = SawPreSave && !SavedOriginal.IsValid() && Published && Published->GeneratedClass && Published->WidgetTree
				&& !Published->GetOutermost()->IsDirty() && (Published->Status == BS_UpToDate || Published->Status == BS_UpToDateWithWarnings)
				&& FFileHelper::LoadFileToArray(PublishedBytes, *Filename) && !PublishedBytes.IsEmpty()
				&& (bCreate ? Published->BlueprintCategory == TEXT("CreatedInlineCategory")
					: !OriginalNode.IsValid() && Published->bRunConstructionScriptOnDrag == !InitialRoot
						&& PublishedExpected.OrderIndependentCompareEqual(CaptureUmgNativeContents(*Published)) && PublishedBytes != DiskBefore);
			ObservedPublished &= !bGraphEdit || (Published && CastChecked<UUserWidget>(Published->GeneratedClass->GetDefaultObject())->GetRenderOpacity() == 0.375f);
			if (bMemberEdit)
			{
				const FIntProperty* Member = Published && Published->GeneratedClass ? FindFProperty<FIntProperty>(Published->GeneratedClass, TEXT("LateCount")) : nullptr;
				ObservedPublished &= Member && Member->GetPropertyValue_InContainer(Published->GeneratedClass->GetDefaultObject()) == 31;
			}
			Failure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_umg_inline_late_rejection"));
			return false;
		};
		Blueprint = nullptr;
		FAssetApplyResult Result;
		Test.TestFalse(TEXT("late rejection fails inside the UMG transaction"), ApplyAsset(Request, Result, TEXT("umg-inline-atomic")));
		Test.AddInfo(FString::Printf(TEXT("UMG transaction result phase=%s code=%s detail=%s"), *Result.Phase, *Result.Diagnostic.Code, *Result.Diagnostic.PublicMessage));
		Test.TestEqual(TEXT("the requested late failure survives rollback"), Result.Diagnostic.Code, FString(TEXT("test_umg_inline_late_rejection")));
		Test.TestTrue(TEXT("final readback sees changed, compiled, saved and reloaded native state"), ObservedPublished);
		Test.TestTrue(TEXT("the public checkpoint confirms recovery"), Result.bRollbackAttempted && Result.bRollbackSucceeded);
		if (bCreate)
		{
			Test.TestNull(TEXT("absent checkpoint removes the created object"), FindObject<UObject>(nullptr, *ObjectPath));
			Test.TestNull(TEXT("absent checkpoint removes the created package"), FindPackage(nullptr, *PackageName));
			Test.TestFalse(TEXT("absent checkpoint removes the created asset file"), IFileManager::Get().FileExists(*Filename));
			Test.TestFalse(TEXT("absent checkpoint removes the created registry entry"), FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid());
		}
		else
		{
			UWidgetBlueprint* Restored = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
			TArray<uint8> DiskAfter;
			if (bMemberEdit) Test.TestTrue(TEXT("late rollback removes newly published member"), Restored
				&& FBlueprintEditorUtils::FindNewVariableIndex(Restored, TEXT("LateCount")) == INDEX_NONE
				&& !FindFProperty<FProperty>(Restored->GeneratedClass, TEXT("LateCount")));
			Test.TestTrue(TEXT("existing checkpoint preserves dirty widget tree and graph independently of disk"), Restored && Restored->GetOutermost()->IsDirty()
				&& Restored->bRunConstructionScriptOnDrag == InitialRoot && NativeBefore.OrderIndependentCompareEqual(CaptureUmgNativeContents(*Restored))
				&& (!bGraphEdit || CastChecked<UUserWidget>(Restored->GeneratedClass->GetDefaultObject())->GetRenderOpacity() == 0.375f)
				&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
		}
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgInlineCreateAtomicTest, "UAI.Root.T208.UMG.InlineCreateAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgInlineCreateAtomicTest::RunTest(const FString&) { return RunUmgInlineAtomic(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgInlineUpdateAtomicTest, "UAI.Root.T208.UMG.InlineUpdateAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgInlineUpdateAtomicTest::RunTest(const FString&) { return RunUmgInlineAtomic(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgGraphAtomicTest, "UAI.Root.T208.UMG.GraphAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgGraphAtomicTest::RunTest(const FString&) { return RunUmgInlineAtomic(*this, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgMemberAtomicTest, "UAI.Root.T208.UMG.MemberAtomic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgMemberAtomicTest::RunTest(const FString&) { return RunUmgInlineAtomic(*this, false, true, true); }

static bool RunUmgWorkspace(FAutomationTestBase& Test, const bool bGraphEdit, const bool bMemberEdit = false, const bool bBindingEdit = false)
{
	using namespace UE::UAI;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentBlueprintAuthoring::RegisterAuthoring(Error) || !UeAgentUmgAuthoring::RegisterAuthoring(Error)) { Test.AddError(Error.ToPublicDetail()); return false; }
	const FString Leaf = TEXT("T85UmgRoot_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/") + Leaf + TEXT("/Widget");
	const FString ObjectPath = PackageName + TEXT(".Widget");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-umg-inline-atomic/root-workspace/native") / Leaf);
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/UmgRootDelete"), TEXT("umg-root"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			Remaining->GetOutermost()->SetDirtyFlag(false);
			Test.TestTrue(TEXT("root fixture formal cleanup"), DeleteOwner->Delete({TEXT("umg-root-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FUmgInlineNeverCancelled>()).IsSuccess());
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(Test, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package, TEXT("Widget"), BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None));
	if (!Test.TestNotNull(TEXT("native root fixture exists"), Blueprint)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->BlueprintCategory = TEXT("RootBefore");
	const bool BeforeFlag = Blueprint->bRunConstructionScriptOnDrag;
	UCanvasPanel* Canvas = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("PreservedCanvas"));
	Blueprint->WidgetTree->RootWidget = Canvas;
	Canvas->AddChild(Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("PreservedButton")));
	if (bBindingEdit)
	{
		Canvas->AddChild(Blueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DisplayText")));
		FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Text;
		for (const TCHAR* Name : {TEXT("SourceA"), TEXT("SourceB"), TEXT("SourceC")})
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(Name), Type)) return false;
	}
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("PreservedGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
	UK2Node_CustomEvent* Event = Creator.CreateNode(); Event->CustomFunctionName = TEXT("PreservedEvent"); Creator.Finalize(); Event->NodePosX = 216; Event->NodePosY = 92;
	FGraphNodeCreator<UK2Node_CallFunction> PrintCreator(*Graph);
	UK2Node_CallFunction* Print = PrintCreator.CreateNode();
	Print->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
	PrintCreator.Finalize();
	Print->NodePosX = 500; Print->NodePosY = 92;
	Print->FindPinChecked(TEXT("InString"))->DefaultValue = TEXT("GraphBefore");
	const FGuid PrintGuid = Print->NodeGuid;
	if (!Graph->GetSchema()->TryCreateConnection(Event->FindPinChecked(UEdGraphSchema_K2::PN_Then), Print->GetExecPin())) return false;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	UUserWidget* CDO = CastChecked<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject());
	CDO->SetRenderOpacity(0.625f);
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args)) return false;
	Package->SetDirtyFlag(false);
	auto BeforeNative = CaptureUmgNativeContents(*Blueprint); BeforeNative.Remove(TEXT("category"));
	TArray<FGuid> BindingMemberIds;
	if (bBindingEdit) for (const auto& Member : Blueprint->NewVariables) BindingMemberIds.Add(Member.VarGuid);
	TArray<UeAgentAuthoringState::FReadRequest> Reads;
	if (!Test.TestEqual(TEXT("native print pin remains non-default before export"), Print->FindPinChecked(TEXT("InString"))->DefaultValue, FString(TEXT("GraphBefore")))) return false;
	if (!UeAgentUmgAuthoring::BuildAssetExportSnapshotReadRequests(*Blueprint, Reads, Error)) { Test.AddError(Error.ToPublicDetail()); return false; }
	int32 CdoOpacityReads = 0;
	for (const auto& Read : Reads) if (Read.RootObject == CDO && Read.TargetIdentity.GetSelector(TEXT("property_path")) == TEXT("RenderOpacity")) ++CdoOpacityReads;
	const FProperty* OpacityProperty = FindFProperty<FProperty>(CDO->GetClass(), TEXT("RenderOpacity"));
	Test.AddInfo(FString::Printf(TEXT("CDO probe native_opacity=%f property_flags=%llu snapshot_requests=%d"), CDO->GetRenderOpacity(), OpacityProperty ? static_cast<uint64>(OpacityProperty->GetPropertyFlags()) : 0, CdoOpacityReads));
	Reads.Reset();
	UeAgentExportWorkspaceWriter::FWorkspaceResult Exported;
	if (!UeAgentUmgAuthoring::ExportAssetWorkspace(*Blueprint, Workspace, TEXT("llm_authoring"), Exported)) { Test.AddError(Exported.Diagnostic.ToPublicDetail()); return false; }
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	int32 Categories = 0, Flags = 0, CdoExports = 0, CdoEdits = 0, PinEdits = 0, LayoutEdits = 0, MemberEdits = 0, BindingEdits = 0;
	TFunction<void(const TSharedPtr<FJsonValue>&)> EditValue;
	EditValue = [&](const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return;
		if (Value->Type == EJson::Array) { for (const auto& Child : Value->AsArray()) EditValue(Child); return; }
		if (Value->Type != EJson::Object) return;
		if (bMemberEdit && Value->AsObject()->HasField(TEXT("variables")))
		{
			auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("pin_category"), TEXT("int"));
			auto Default = MakeShared<FJsonObject>(); Default->SetStringField(TEXT("property_path"), TEXT("SdkCount")); Default->SetNumberField(TEXT("value"), 17);
			auto Variable = MakeShared<FJsonObject>(); Variable->SetObjectField(TEXT("type"), Type); Variable->SetBoolField(TEXT("instance_editable"), true); Variable->SetObjectField(TEXT("default_property"), Default);
			Value->AsObject()->GetObjectField(TEXT("variables"))->SetObjectField(TEXT("SdkCount"), Variable);
			++MemberEdits;
		}
		if (bBindingEdit && Value->AsObject()->HasField(TEXT("bindings")))
		{
			auto Binding = MakeShared<FJsonObject>();
			Binding->SetStringField(TEXT("widget_name"), TEXT("DisplayText"));
			Binding->SetStringField(TEXT("property_name"), TEXT("Text"));
			Binding->SetStringField(TEXT("binding_kind"), TEXT("property_variable"));
			Binding->SetStringField(TEXT("source_variable_name"), TEXT("SourceA"));
			Value->AsObject()->GetObjectField(TEXT("bindings"))->SetObjectField(TEXT("DisplayText_Text"), Binding);
			++BindingEdits;
		}
		if (bGraphEdit && !bMemberEdit && !bBindingEdit)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FString FunctionName;
			if (Object->TryGetStringField(TEXT("function_name"), FunctionName) && FunctionName == TEXT("PrintString"))
			{
				const TSharedPtr<FJsonObject>* Position = nullptr;
				if (Object->TryGetObjectField(TEXT("pos"), Position)) { (*Position)->SetNumberField(TEXT("x"), 760); (*Position)->SetNumberField(TEXT("y"), 180); ++LayoutEdits; }
			}
			for (auto& Field : Object->Values)
			{
				if (Field.Value->Type == EJson::String && Field.Value->AsString() == TEXT("GraphBefore"))
				{ Field.Value = MakeShared<FJsonValueString>(TEXT("GraphAfter")); ++PinEdits; }
			}
		}
		for (const auto& Field : Value->AsObject()->Values)
		{
			if (Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value")))
			{
				if (!bGraphEdit && Field.Key == TEXT("BlueprintCategory")) { Field.Value->AsObject()->SetStringField(TEXT("value"), TEXT("RootAfter")); ++Categories; }
				if (!bGraphEdit && Field.Key == TEXT("bRunConstructionScriptOnDrag")) { Field.Value->AsObject()->SetBoolField(TEXT("value"), !BeforeFlag); ++Flags; }
				if (Field.Key == TEXT("RenderOpacity"))
				{
					++CdoExports;
					double ValueBefore = 0;
					if (!bGraphEdit && Field.Value->AsObject()->TryGetNumberField(TEXT("value"), ValueBefore) && ValueBefore == 0.625)
					{ Field.Value->AsObject()->SetNumberField(TEXT("value"), 0.375); ++CdoEdits; }
				}
			}
			EditValue(Field.Value);
		}
	};
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) { Test.AddError(Error.ToPublicDetail()); return false; }
		const int32 PreviousEdits = Categories + Flags + CdoEdits + PinEdits + LayoutEdits + MemberEdits + BindingEdits;
		EditValue(MakeShared<FJsonValueObject>(Document));
		if (Categories + Flags + CdoEdits + PinEdits + LayoutEdits + MemberEdits + BindingEdits != PreviousEdits && (!CommandContract::Json::TryCanonicalize(Document, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	}
	Test.AddInfo(FString::Printf(TEXT("ordinary RenderOpacity exported occurrences=%d (includes widget instances)"), CdoExports));
	if (bBindingEdit)
	{
		if (!Test.TestEqual(TEXT("one ordinary binding collection edits"), BindingEdits, 1)) return false;
	}
	else if (bMemberEdit)
	{
		if (!Test.TestEqual(TEXT("one ordinary member collection edits"), MemberEdits, 1)) return false;
	}
	else if (bGraphEdit)
	{
		if (!Test.TestEqual(TEXT("one existing node pin default edits"), PinEdits, 1) || !Test.TestEqual(TEXT("one existing node layout edits"), LayoutEdits, 1)) return false;
	}
	else
	{
	if (!Test.TestEqual(TEXT("one editable Blueprint category exports"), Categories, 1) || !Test.TestEqual(TEXT("one editable Blueprint flag exports"), Flags, 1)) return false;
	if (!Test.TestEqual(TEXT("the non-default CDO property is captured"), CdoOpacityReads, 1)
		|| !Test.TestEqual(TEXT("the non-default CDO property exports independently of widget instances"), CdoEdits, 1)) return false;
	}
	auto Directory = MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"), TEXT("directory")); Directory->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Source;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	FString Revision, Authorization; HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Directory, PackageName, Resources, Source, Revision, Authorization, WorkspaceError)) { Test.AddError(WorkspaceError.GetCode()); return false; }
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), PackageName); Params->SetStringField(TEXT("expected_revision"), Revision); Params->SetObjectField(TEXT("source"), Source->MakeNormalizedSourceObject());
	FString ParamsText; if (!CommandContract::Json::TryCanonicalize(Params, ParamsText, Error)) return false;
	Blueprint = nullptr; CDO = nullptr; Canvas = nullptr; Graph = nullptr; Event = nullptr; Print = nullptr;
	FString Payload; bool ReadOnly = true;
	if (!DomainAdapters::UmgWidget::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName, ParamsText, Payload, ReadOnly, WorkspaceError))
	{ Test.AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit() + TEXT(":") + WorkspaceError.GetJsonPointer()); return false; }
	Package = FindPackage(nullptr, *PackageName);
	if (!Package || !UPackageTools::UnloadPackages({Package})) return false;
	UWidgetBlueprint* Reloaded = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!Test.TestNotNull(TEXT("root result reloads independently"), Reloaded)) return false;
	auto AfterNative = CaptureUmgNativeContents(*Reloaded); AfterNative.Remove(TEXT("category"));
	if (bGraphEdit && !bMemberEdit && !bBindingEdit)
	{
		const FString NodeKey = TEXT("node:") + PrintGuid.ToString();
		BeforeNative[NodeKey] = BeforeNative[NodeKey].Replace(TEXT(":500:92"), TEXT(":760:180"));
		BeforeNative[NodeKey + TEXT(":pin:InString")] = BeforeNative[NodeKey + TEXT(":pin:InString")].Replace(TEXT("GraphBefore"), TEXT("GraphAfter"));
	}
	Test.TestTrue(TEXT("ordinary changes persist and non-target widgets/graphs remain"), Reloaded->BlueprintCategory == (bGraphEdit ? TEXT("RootBefore") : TEXT("RootAfter"))
		&& Reloaded->bRunConstructionScriptOnDrag == (bGraphEdit ? BeforeFlag : !BeforeFlag) && !Reloaded->GetOutermost()->IsDirty() && BeforeNative.OrderIndependentCompareEqual(AfterNative));
	Test.TestEqual(TEXT("class default survives compile, save and independent reload"), CastChecked<UUserWidget>(Reloaded->GeneratedClass->GetDefaultObject())->GetRenderOpacity(), bGraphEdit ? 0.625f : 0.375f);
	if (bMemberEdit)
	{
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Reloaded, TEXT("SdkCount"));
		const FIntProperty* Property = FindFProperty<FIntProperty>(Reloaded->GeneratedClass, TEXT("SdkCount"));
		Test.TestTrue(TEXT("native variable type, identity and default persist"), Index != INDEX_NONE && Reloaded->NewVariables[Index].VarGuid.IsValid()
			&& Property && Property->GetPropertyValue_InContainer(Reloaded->GeneratedClass->GetDefaultObject()) == 17);
	}
	UeAgentExportWorkspaceWriter::FWorkspaceResult Readback;
	if (!UeAgentUmgAuthoring::ExportAssetWorkspace(*Reloaded, Workspace + TEXT("-readback"), TEXT("llm_authoring"), Readback)) { Test.AddError(Readback.Diagnostic.ToPublicDetail()); return false; }
	if (bBindingEdit)
	{
		auto HasBinding = [](UWidgetBlueprint& Asset, const FName SourceName)
		{
			const auto* Generated = Cast<UWidgetBlueprintGeneratedClass>(Asset.GeneratedClass);
			return Generated && Generated->Bindings.Num() == 1
				&& Generated->Bindings[0].ObjectName == TEXT("DisplayText") && Generated->Bindings[0].PropertyName == TEXT("Text")
				&& Generated->Bindings[0].Kind == EBindingKind::Property && Generated->Bindings[0].FunctionName == SourceName
				&& Asset.Bindings.Num() == 1 && Asset.Bindings[0].ObjectName == TEXT("DisplayText")
				&& Asset.Bindings[0].PropertyName == TEXT("Text") && Asset.Bindings[0].Kind == EBindingKind::Property
				&& Asset.Bindings[0].SourceProperty == SourceName;
		};
		TArray<FGuid> AfterMemberIds; for (const auto& Member : Reloaded->NewVariables) AfterMemberIds.Add(Member.VarGuid);
		auto PreservesNonTargets = [&](UWidgetBlueprint& Asset)
		{
			auto Contents = CaptureUmgNativeContents(Asset); Contents.Remove(TEXT("category"));
			TArray<FGuid> Ids; for (const auto& Member : Asset.NewVariables) Ids.Add(Member.VarGuid);
			return BeforeNative.OrderIndependentCompareEqual(Contents) && Ids == BindingMemberIds
				&& Asset.BlueprintCategory == TEXT("RootBefore") && Asset.bRunConstructionScriptOnDrag == BeforeFlag;
		};
		if (!Test.TestTrue(TEXT("binding created and persisted with members preserved"), HasBinding(*Reloaded, TEXT("SourceA")) && BindingMemberIds == AfterMemberIds)) return false;
		TArray<uint8> DiskBefore;
		if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
		FProperty* DirtySource = FindFProperty<FProperty>(Reloaded->SkeletonGeneratedClass, TEXT("SourceB"));
		if (!DirtySource) return false;
		Reloaded->Bindings[0].SourceProperty = TEXT("SourceB");
		Reloaded->Bindings[0].FunctionName = TEXT("SourceB");
		Reloaded->Bindings[0].SourcePath = FEditorPropertyPath(TArray<FFieldVariant>{FFieldVariant(DirtySource)});
		FKismetEditorUtilities::CompileBlueprint(Reloaded);
		CastChecked<UUserWidget>(Reloaded->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.375f);
		Reloaded->MarkPackageDirty();
		TWeakObjectPtr<UWidgetBlueprint> Original = Reloaded;
		UeAgentUmgAuthoring::FAssetApplyRequest Request;
		Request.AssetPath = PackageName; Request.Operation = UeAgentApplyInputContract::EOperation::Update;
		Request.ProviderExtension = InlineLifecycleOptions();
		auto Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("operation"), TEXT("update"));
		Binding->SetStringField(TEXT("widget_name"), TEXT("DisplayText"));
		Binding->SetStringField(TEXT("property_name"), TEXT("Text"));
		Binding->SetStringField(TEXT("binding_kind"), TEXT("property_variable"));
		Binding->SetStringField(TEXT("source_variable_name"), TEXT("SourceC"));
		auto Bindings = MakeShared<FJsonObject>(); Bindings->SetObjectField(TEXT("DisplayText_Text"), Binding);
		Request.ProviderExtension->SetObjectField(TEXT("bindings"), Bindings);
		bool ObservedPublished = false;
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
		{
			UWidgetBlueprint* Published = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
			TArray<uint8> PublishedBytes;
			ObservedPublished = !Original.IsValid() && Published && !Published->GetOutermost()->IsDirty()
				&& HasBinding(*Published, TEXT("SourceC")) && Published->Status == BS_UpToDate && PreservesNonTargets(*Published)
				&& FFileHelper::LoadFileToArray(PublishedBytes, *Filename) && PublishedBytes != DiskBefore
				&& CastChecked<UUserWidget>(Published->GeneratedClass->GetDefaultObject())->GetRenderOpacity() == 0.375f;
			Failure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_umg_binding_late_rejection")); return false;
		};
		Reloaded = nullptr; DirtySource = nullptr;
		UeAgentUmgAuthoring::FAssetApplyResult Result;
		Test.TestFalse(TEXT("binding final readback rejects"), UeAgentUmgAuthoring::ApplyAsset(Request, Result));
		Test.TestTrue(TEXT("late rejection observes published reloaded binding"), ObservedPublished);
		Test.TestTrue(TEXT("binding rollback succeeds"), Result.bRollbackAttempted && Result.bRollbackSucceeded && Result.Diagnostic.Code == TEXT("test_umg_binding_late_rejection"));
		UWidgetBlueprint* Restored = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		TArray<uint8> DiskAfter;
		Test.TestTrue(TEXT("binding dirty memory and original disk independently restored"), Restored && Restored->GetOutermost()->IsDirty()
			&& HasBinding(*Restored, TEXT("SourceB")) && PreservesNonTargets(*Restored) && CastChecked<UUserWidget>(Restored->GeneratedClass->GetDefaultObject())->GetRenderOpacity() == 0.375f
			&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	}
	return !Test.HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgRootWorkspaceTest, "UAI.Root.T208.UMG.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgRootWorkspaceTest::RunTest(const FString&) { return RunUmgWorkspace(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgGraphWorkspaceTest, "UAI.Root.T208.UMG.GraphWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgGraphWorkspaceTest::RunTest(const FString&) { return RunUmgWorkspace(*this, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgMemberWorkspaceTest, "UAI.Root.T208.UMG.MemberWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgMemberWorkspaceTest::RunTest(const FString&) { return RunUmgWorkspace(*this, true, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgBindingWorkspaceTest, "UAI.Root.T208.UMG.BindingWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgBindingWorkspaceTest::RunTest(const FString&) { return RunUmgWorkspace(*this, true, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208BlueprintPreparedIdentityTest, "UAI.Root.T208.Blueprint.PreparedCreationIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208BlueprintPreparedIdentityTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentBlueprintAuthoring::RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	const FString PackageName = TEXT("/Game/UAI/Transient/T85BlueprintIdentity_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Blueprint");
	const FString ObjectPath = PackageName + TEXT(".Blueprint");
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/BlueprintIdentityDelete"), TEXT("blueprint-identity"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		if (UObject* Remaining = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			Remaining->GetOutermost()->SetDirtyFlag(false);
			TestTrue(TEXT("identity fixture formal cleanup"), DeleteOwner->Delete({TEXT("identity-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FUmgInlineNeverCancelled>()).IsSuccess());
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("Blueprint"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	if (!TestNotNull(TEXT("native Blueprint exists"), Blueprint)) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("IdentityGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
	UK2Node_CustomEvent* Event = Creator.CreateNode(); Event->CustomFunctionName = TEXT("PreservedEvent"); Creator.Finalize();
	Event->NodePosX = 100; Event->NodePosY = 40;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	const FGuid PreservedNodeId = Event->NodeGuid;
	const FGuid PreservedPinId = Event->FindPinChecked(UEdGraphSchema_K2::PN_Then)->PinId;
	auto Candidate = MakeShared<FJsonObject>(); Candidate->SetStringField(TEXT("name"), TEXT("Print String"));
	auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("operation"), TEXT("create")); Node->SetObjectField(TEXT("candidate"), Candidate);
	auto Nodes = MakeShared<FJsonObject>(); Nodes->SetObjectField(TEXT("node:IdentityPrint"), Node);
	auto GraphSpec = MakeShared<FJsonObject>(); GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
	auto Graphs = MakeShared<FJsonObject>(); Graphs->SetObjectField(TEXT("event_graph:IdentityGraph"), GraphSpec);
	auto Extension = MakeShared<FJsonObject>(); Extension->SetObjectField(TEXT("graphs"), Graphs);
	auto VariableType = MakeShared<FJsonObject>(); VariableType->SetStringField(TEXT("pin_category"), TEXT("int"));
	auto Variable = MakeShared<FJsonObject>(); Variable->SetObjectField(TEXT("type"), VariableType);
	auto Variables = MakeShared<FJsonObject>(); Variables->SetObjectField(TEXT("IdentityCount"), Variable);
	auto Members = MakeShared<FJsonObject>(); Members->SetObjectField(TEXT("variables"), Variables); Extension->SetObjectField(TEXT("members"), Members);
	HjsonWorkspace::FWorkspaceError ProjectionError;
	if (!DomainAdapters::Blueprint::Surfaces::TryNormalizeBlueprintCandidateBindings(ObjectPath, Extension, ProjectionError)) { AddError(ProjectionError.GetCode()); return false; }
	UPackage* ScratchPackage = CreatePackage(*(TEXT("/Temp/UAI/BlueprintIdentity_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ScratchPackage->SetFlags(RF_Transient);
	TStrongObjectPtr<UBlueprint> Scratch(UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Blueprint, ScratchPackage));
	if (!TestNotNull(TEXT("validation Blueprint duplicate exists"), Scratch.Get())) return false;
	Scratch->ClearFlags(RF_Public | RF_Standalone); Scratch->SetFlags(RF_Transient);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Prepared;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(*Scratch, Extension, Prepared, Counters, Error)) { AddError(Error.ToPublicDetail()); return false; }
	auto IdentityState = [](UBlueprint& Asset)
	{
		TMap<FString, FString> State;
		for (const FBPVariableDescription& Member : Asset.NewVariables)
			State.Add(TEXT("member:") + Member.VarName.ToString(), Member.VarGuid.ToString());
		TArray<UEdGraph*> AllGraphs; Asset.GetAllGraphs(AllGraphs);
		for (const UEdGraph* Current : AllGraphs)
		{
			State.Add(TEXT("graph:") + Current->GetName(), Current->GraphGuid.ToString());
			for (const UEdGraphNode* CurrentNode : Current->Nodes)
			{
				const FString Key = CurrentNode->NodeGuid.ToString();
				State.Add(Key, CurrentNode->GetClass()->GetPathName());
				for (const UEdGraphPin* Pin : CurrentNode->Pins)
					State.Add(Key + TEXT(":") + Pin->PinName.ToString(), Pin->PinId.ToString() + TEXT(":") + Pin->PersistentGuid.ToString() + TEXT(":") + Pin->DefaultValue);
			}
		}
		return State;
	};
	const auto Expected = IdentityState(*Scratch);
	if (!Prepared->ApplyTo(*Blueprint, Counters, Error)) { AddError(Error.ToPublicDetail()); return false; }
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!Prepared->FinalizeAfterCompile(*Blueprint, Counters, Error)) { AddError(Error.ToPublicDetail()); return false; }
	TestTrue(TEXT("native Blueprint replay preserves all prepared graph/node/pin identities"), Expected.OrderIndependentCompareEqual(IdentityState(*Blueprint)));
	TestTrue(TEXT("existing node and pin identities remain unchanged"), Event->NodeGuid == PreservedNodeId && Event->FindPinChecked(UEdGraphSchema_K2::PN_Then)->PinId == PreservedPinId);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args)) return false;
	Package->SetDirtyFlag(false);
	Prepared.Reset(); Scratch.Reset(); Blueprint = nullptr; Graph = nullptr; Event = nullptr;
	if (!UPackageTools::UnloadPackages({Package})) return false;
	UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	TestTrue(TEXT("prepared creation identities persist after independent reload"), Reloaded && Expected.OrderIndependentCompareEqual(IdentityState(*Reloaded)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgAnimationWorkspaceTest, "UAI.Root.T208.UMG.AnimationWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208UmgAnimationWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	using namespace UeAgentUmgAuthoring;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentBlueprintAuthoring::RegisterAuthoring(Error) || !RegisterAuthoring(Error)) return false;
	const FString PackageName = TEXT("/Game/UAI/Transient/T85UmgAnimation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Widget");
	const FString ObjectPath = PackageName + TEXT(".Widget");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Workspace = FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/UmgAnimation") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TSharedPtr<DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(Workspace, TEXT("umg-animation"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			Asset->GetOutermost()->SetDirtyFlag(false);
			TestTrue(TEXT("animation fixture formal cleanup"), DeleteOwner->Delete({TEXT("umg-animation-cleanup"), {ObjectPath}, false, TEXT("t208")}, MakeShared<FUmgInlineNeverCancelled>()).IsSuccess());
		}
		T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	UPackage* Package = CreatePackage(*PackageName);
	auto* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package, TEXT("Widget"), BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None));
	if (!Blueprint) return false;
	FAssetRegistryModule::AssetCreated(Blueprint);
	auto* Canvas = Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
	Blueprint->WidgetTree->RootWidget = Canvas;
	auto* Button = Blueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Button")); Canvas->AddChild(Button);
	FEdGraphPinType Type; Type.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("PreservedCount"), Type, TEXT("19"))) return false;
	auto* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("PreservedGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph); auto* Event = Creator.CreateNode(); Event->CustomFunctionName = TEXT("PreservedEvent"); Creator.Finalize();
	auto* Animation = NewObject<UWidgetAnimation>(Blueprint, TEXT("Fade"), RF_Transactional);
	Animation->MovieScene = NewObject<UMovieScene>(Animation, TEXT("Fade"), RF_Transactional);
	Blueprint->Animations.Add(Animation); Blueprint->OnVariableAdded(Animation->GetFName());
	UMovieScene* Scene = Animation->GetMovieScene();
	const FGuid BindingId = Scene->AddPossessable(Button->GetName(), Button->GetClass());
	FWidgetAnimationBinding Binding; Binding.WidgetName = Button->GetFName(); Binding.AnimationGuid = BindingId;
	Animation->AnimationBindings.Add(Binding);
	Scene->SetTickResolutionDirectly(FFrameRate(24000, 1)); Scene->SetDisplayRate(FFrameRate(24, 1)); Scene->SetPlaybackRange(0, 24000);
	UMovieSceneFloatTrack* Track = Scene->AddTrack<UMovieSceneFloatTrack>(BindingId);
	if (!Track) return false;
	Track->SetPropertyNameAndPath(TEXT("RenderOpacity"), TEXT("RenderOpacity"));
	auto* First = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection()); Track->AddSection(*First);
	First->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(12000)));
	FMovieSceneFloatValue Initial(0.25f); Initial.InterpMode = RCIM_Constant;
	First->GetChannel().GetData().AddKey(FFrameNumber(0), Initial);
	First->GetChannel().GetData().AddKey(FFrameNumber(6000), FMovieSceneFloatValue(0.5f)); First->GetChannel().SetDefault(0.125f);
	auto* Second = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection()); Track->AddSection(*Second);
	Second->SetRange(TRange<FFrameNumber>(FFrameNumber(12000), FFrameNumber(24000)));
	FMovieSceneFloatValue Untouched(0.875f); Untouched.InterpMode = RCIM_Cubic; Untouched.Tangent.ArriveTangent = 0.125f;
	Second->GetChannel().GetData().AddKey(FFrameNumber(18000), Untouched); Second->GetChannel().SetDefault(0.625f);
	const FName TrackName = Track->GetFName(), FirstName = First->GetFName(), SecondName = Second->GetFName();
	const FGuid MemberId = Blueprint->NewVariables[0].VarGuid;
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	CastChecked<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.625f);
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args)) return false; Package->SetDirtyFlag(false);
	const auto NonTargets = CaptureUmgNativeContents(*Blueprint);
	auto NativeCheck = [&](UWidgetBlueprint& Asset, float Value, float Default, ERichCurveInterpMode Mode, float CdoOpacity, bool bSelectFirst = false,
		int32 DisplayRate = 48, int32 PlaybackStart = 4800, int32 PlaybackEnd = 36000, int32 AnimationCount = 1) {
		if (Asset.Animations.Num() != AnimationCount || Asset.NewVariables.Num() != 1 || Asset.NewVariables[0].VarGuid != MemberId
			|| !NonTargets.OrderIndependentCompareEqual(CaptureUmgNativeContents(Asset))
			|| CastChecked<UUserWidget>(Asset.GeneratedClass->GetDefaultObject())->GetRenderOpacity() != CdoOpacity) return false;
		UWidgetAnimation* A = nullptr;
		for (auto Candidate : Asset.Animations) if (Candidate && Candidate->GetFName() == TEXT("Fade")) A = Candidate;
		if (!A) return false;
		auto* M = A->GetMovieScene();
		if (M->GetDisplayRate() != FFrameRate(DisplayRate, 1) || M->GetTickResolution() != FFrameRate(24000, 1)
			|| M->GetPlaybackRange() != TRange<FFrameNumber>(FFrameNumber(PlaybackStart), FFrameNumber(PlaybackEnd))) return false;
		if (A->AnimationBindings.Num() != 1 || A->AnimationBindings[0].AnimationGuid != BindingId || A->AnimationBindings[0].WidgetName != TEXT("Button")) return false;
		auto* T = M->FindTrack<UMovieSceneFloatTrack>(BindingId, TEXT("RenderOpacity"));
		if (!T || T->GetFName() != TrackName || T->GetAllSections().Num() != 2) return false;
		auto* S0 = CastChecked<UMovieSceneFloatSection>(T->GetAllSections()[0]); auto* S1 = CastChecked<UMovieSceneFloatSection>(T->GetAllSections()[1]);
		const auto D0 = S0->GetChannel().GetData(), D1 = S1->GetChannel().GetData();
		return T->GetSectionToKey() == (bSelectFirst ? S0 : S1)
			&& S0->GetFName() == FirstName && S1->GetFName() == SecondName && D0.GetValues().Num() == 2 && D1.GetValues().Num() == 1
			&& D0.GetValues()[0].Value == Value && D0.GetValues()[0].InterpMode == Mode && D0.GetValues()[1].Value == 0.5f
			&& D0.GetTimes()[0] == FFrameNumber(0) && D0.GetTimes()[1] == FFrameNumber(6000)
			&& S0->GetChannel().GetDefault().Get(0) == Default && S1->GetChannel().GetDefault().Get(0) == 0.625f
			&& D1.GetValues()[0] == Untouched && D1.GetTimes()[0] == FFrameNumber(18000)
			&& S0->GetRange() == TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(12000))
			&& S1->GetRange() == TRange<FFrameNumber>(FFrameNumber(12000), FFrameNumber(24000));
	};
	UeAgentExportWorkspaceWriter::FWorkspaceResult Exported;
	if (!ExportAssetWorkspace(*Blueprint, Workspace, TEXT("llm_authoring"), Exported)) { AddError(Exported.Diagnostic.ToPublicDetail()); return false; }
	TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Workspace, TEXT("*.hjson"), true, false);
	int32 EditedKeys = 0, EditedDefaults = 0, EditedSettings = 0;
	TFunction<void(const TSharedPtr<FJsonValue>&)> Edit;
	Edit = [&](const TSharedPtr<FJsonValue>& V) {
		if (!V) return;
		if (V->Type == EJson::Array) { for (const auto& Child : V->AsArray()) Edit(Child); return; }
		if (V->Type != EJson::Object) return;
		auto O = V->AsObject(); FString Lexeme;
		if (O->HasField(TEXT("display_rate")) && O->HasField(TEXT("playback_range")))
		{
			O->GetObjectField(TEXT("display_rate"))->SetStringField(TEXT("numerator"), TEXT("48"));
			auto Range = O->GetObjectField(TEXT("playback_range"));
			TestFalse(TEXT("ordinary playback range hides clock identity"), Range->HasField(TEXT("clock_id")));
			Range->GetObjectField(TEXT("lower"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("4800"));
			Range->GetObjectField(TEXT("upper"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("36000"));
			++EditedSettings;
		}
		if (O->TryGetStringField(TEXT("value_lexeme"), Lexeme) && Lexeme == TEXT("0.25"))
		{
			O->SetStringField(TEXT("value_lexeme"), TEXT("0.75"));
			FMovieSceneFloatValue Desired(0.75f); Desired.InterpMode = RCIM_Linear;
			O->SetField(TEXT("properties"), Authoring::Timeline::ScalarChannel::CaptureProperties(Desired, First)); ++EditedKeys;
		}
		if (O->TryGetStringField(TEXT("value_lexeme"), Lexeme) && Lexeme == TEXT("0.125") && O->HasField(TEXT("is_set")))
		{ O->SetStringField(TEXT("value_lexeme"), TEXT("0.375")); ++EditedDefaults; }
		for (const auto& Field : O->Values) Edit(Field.Value);
	};
	for (const FString& File : Files)
	{
		if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
		FString Text; TSharedPtr<FJsonObject> Document;
		if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Document, Error)) return false;
		const int32 Before = EditedKeys + EditedDefaults + EditedSettings; Edit(MakeShared<FJsonValueObject>(Document));
		if (Before != EditedKeys + EditedDefaults + EditedSettings && (!CommandContract::Json::TryCanonicalize(Document, Text, Error)
			|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
	}
	if (!TestEqual(TEXT("one ordinary exact key edited"), EditedKeys, 1) || !TestEqual(TEXT("one ordinary channel default edited"), EditedDefaults, 1)) return false;
	if (!TestEqual(TEXT("one ordinary animation display rate and playback range edited"), EditedSettings, 1)) return false;
	auto Directory = MakeShared<FJsonObject>(); Directory->SetStringField(TEXT("kind"), TEXT("directory")); Directory->SetStringField(TEXT("path"), Workspace);
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Source; auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	FString Revision, Authorization; HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Directory, PackageName, Resources, Source, Revision, Authorization, WorkspaceError)) { AddError(WorkspaceError.GetCode()); return false; }
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("target"), PackageName); Params->SetStringField(TEXT("expected_revision"), Revision); Params->SetObjectField(TEXT("source"), Source->MakeNormalizedSourceObject());
	FString ParamsText; if (!CommandContract::Json::TryCanonicalize(Params, ParamsText, Error)) return false;
	Blueprint = nullptr; Animation = nullptr; Scene = nullptr; Track = nullptr; First = nullptr; Second = nullptr; Canvas = nullptr; Button = nullptr; Event = nullptr; Graph = nullptr;
	FString Payload; bool ReadOnly = true;
	if (!DomainAdapters::UmgWidget::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName, ParamsText, Payload, ReadOnly, WorkspaceError))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit()); return false; }
	Package = FindPackage(nullptr, *PackageName); if (!Package || !UPackageTools::UnloadPackages({Package})) return false;
	auto* Reloaded = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("ordinary exact animation edit persists with all other sections and native identities"), Reloaded && NativeCheck(*Reloaded, 0.75f, 0.375f, RCIM_Linear, 0.625f))) return false;
	if (!ExportAssetWorkspace(*Reloaded, Workspace + TEXT("-readback"), TEXT("llm_authoring"), Exported)) return false;
	TArray<uint8> DiskBefore; if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	TSharedPtr<FJsonObject> InvalidSettings;
	if (!TryReadAnimationState(*Reloaded->Animations[0], PackageName, InvalidSettings, Error)) return false;
	InvalidSettings->GetObjectField(TEXT("playback_range"))->GetObjectField(TEXT("upper"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("100"));
	auto InvalidAnimations = MakeShared<FJsonObject>(); InvalidAnimations->SetObjectField(TEXT("Fade"), InvalidSettings);
	FAssetApplyRequest InvalidRequest; InvalidRequest.AssetPath = PackageName; InvalidRequest.Operation = UeAgentApplyInputContract::EOperation::Update;
	InvalidRequest.ProviderExtension = InlineLifecycleOptions(); InvalidRequest.ProviderExtension->SetObjectField(TEXT("animations"), InvalidAnimations);
	bool InvalidPublished = false;
	InvalidRequest.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure&) { InvalidPublished = true; return true; };
	FAssetApplyResult InvalidResult;
	TestFalse(TEXT("inverted exact playback range rejected before publication"), ApplyAsset(InvalidRequest, InvalidResult));
	TArray<uint8> InvalidDisk;
	if (!TestTrue(TEXT("invalid range preserves native values and disk without reaching final callback"), !InvalidPublished
		&& NativeCheck(*Reloaded, 0.75f, 0.375f, RCIM_Linear, 0.625f)
		&& FFileHelper::LoadFileToArray(InvalidDisk, *Filename) && InvalidDisk == DiskBefore)) return false;
	Reloaded->Animations[0]->GetMovieScene()->SetDisplayRate(FFrameRate(60, 1));
	Reloaded->Animations[0]->GetMovieScene()->SetPlaybackRange(2400, 39600);
	auto* DirtyTrack = Reloaded->Animations[0]->GetMovieScene()->FindTrack<UMovieSceneFloatTrack>(BindingId, TEXT("RenderOpacity"));
	auto* DirtySection = CastChecked<UMovieSceneFloatSection>(DirtyTrack->GetAllSections()[0]); DirtySection->GetChannel().GetData().GetValues()[0].Value = 0.5f;
	DirtyTrack->SetSectionToKey(DirtySection);
	CastChecked<UUserWidget>(Reloaded->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.25f); Reloaded->MarkPackageDirty();
	TSharedPtr<FJsonObject> Desired; if (!TryReadAnimationState(*Reloaded->Animations[0], PackageName, Desired, Error)) return false;
	Desired->GetObjectField(TEXT("display_rate"))->SetStringField(TEXT("numerator"), TEXT("30"));
	Desired->GetObjectField(TEXT("playback_range"))->GetObjectField(TEXT("lower"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("1200"));
	Desired->GetObjectField(TEXT("playback_range"))->GetObjectField(TEXT("upper"))->GetObjectField(TEXT("point"))->SetStringField(TEXT("tick"), TEXT("48000"));
	auto Key = Desired->GetArrayField(TEXT("bindings"))[0]->AsObject()->GetArrayField(TEXT("tracks"))[0]->AsObject()->GetArrayField(TEXT("sections"))[0]->AsObject()->GetArrayField(TEXT("channels"))[0]->AsObject()->GetArrayField(TEXT("keys"))[0]->AsObject();
	Key->SetStringField(TEXT("value_lexeme"), TEXT("0.875"));
	auto Animations = MakeShared<FJsonObject>(); Animations->SetObjectField(TEXT("Fade"), Desired);
	FAssetApplyRequest Request; Request.AssetPath = PackageName; Request.Operation = UeAgentApplyInputContract::EOperation::Update; Request.ProviderExtension = InlineLifecycleOptions(); Request.ProviderExtension->SetObjectField(TEXT("animations"), Animations);
	TWeakObjectPtr<UWidgetBlueprint> Original = Reloaded; bool Published = false;
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Asset = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath); TArray<uint8> Bytes;
		Published = !Original.IsValid() && Asset && !Asset->GetOutermost()->IsDirty() && Asset->Status == BS_UpToDate
			&& NativeCheck(*Asset, 0.875f, 0.375f, RCIM_Linear, 0.25f, true, 30, 1200, 48000) && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != DiskBefore;
		Failure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_umg_animation_late_rejection")); return false;
	};
	Reloaded = nullptr; DirtyTrack = nullptr; DirtySection = nullptr;
	FAssetApplyResult Result; TestFalse(TEXT("animation late rejection"), ApplyAsset(Request, Result));
	TestTrue(TEXT("late callback observes changed saved and unloaded animation"), Published);
	TestTrue(TEXT("animation rollback succeeds"), Result.bRollbackAttempted && Result.bRollbackSucceeded && Result.Diagnostic.Code == TEXT("test_umg_animation_late_rejection"));
	auto* Restored = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath); TArray<uint8> DiskAfter;
	TestTrue(TEXT("animation dirty memory including settings and native section selection and original disk independently restored"), Restored && Restored->GetOutermost()->IsDirty() && NativeCheck(*Restored, 0.5f, 0.375f, RCIM_Linear, 0.25f, true, 60, 2400, 42000)
		&& FFileHelper::LoadFileToArray(DiskAfter, *Filename) && DiskAfter == DiskBefore);
	if (HasAnyErrors()) return false;
	auto FindAdded = [](UWidgetBlueprint& Asset) -> UWidgetAnimation* {
		for (auto A : Asset.Animations) if (A && A->GetFName() == TEXT("Added")) return A;
		return nullptr;
	};
	auto CollectionImport = [&](bool Add, const FString& Suffix) {
		auto* Asset = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		const FString Folder = Workspace + Suffix;
		if (!Asset || !ExportAssetWorkspace(*Asset, Folder, TEXT("llm_authoring"), Exported)) return false;
		TArray<FString> Paths; IFileManager::Get().FindFilesRecursive(Paths, *Folder, TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const FString& Path : Paths)
		{
			if (Path.Contains(TEXT("/.uai/")) || Path.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Doc;
			if (!FFileHelper::LoadFileToString(Text, *Path) || !FUeAgentStructuredHjsonParser::ParseObject(Text, Path, Doc, Error)) return false;
			TFunction<void(const TSharedPtr<FJsonValue>&)> Visit;
			Visit = [&](const TSharedPtr<FJsonValue>& V) {
				if (!V || V->Type != EJson::Object) return;
				const auto O = V->AsObject(); const TSharedPtr<FJsonObject>* Map = nullptr;
				if (O->TryGetObjectField(TEXT("animations"), Map) && Map && (*Map)->HasField(TEXT("Fade")))
				{
					if (Add) { auto New = MakeShared<FJsonObject>(); New->SetStringField(TEXT("display_label"), TEXT("Added animation")); (*Map)->SetObjectField(TEXT("Added"), New); }
					else (*Map)->RemoveField(TEXT("Added"));
					++Count; return;
				}
				for (const auto& Field : O->Values) Visit(Field.Value);
			};
			const int32 BeforeCount = Count; Visit(MakeShared<FJsonValueObject>(Doc));
			if (Count != BeforeCount && (!CommandContract::Json::TryCanonicalize(Doc, Text, Error)
				|| !FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
		}
		if (Count != 1) return false;
		auto Dir = MakeShared<FJsonObject>(); Dir->SetStringField(TEXT("kind"), TEXT("directory")); Dir->SetStringField(TEXT("path"), Folder);
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Input; auto Tracker = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
		FString Rev, Auth;
		if (!HjsonWorkspace::TryRecoverContentGraphImportMachineContext(Dir, PackageName, Tracker, Input, Rev, Auth, WorkspaceError)) return false;
		auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("target"), PackageName); P->SetStringField(TEXT("expected_revision"), Rev); P->SetObjectField(TEXT("source"), Input->MakeNormalizedSourceObject());
		FString Encoded; if (!CommandContract::Json::TryCanonicalize(P, Encoded, Error)) return false;
		Asset = nullptr;
		return DomainAdapters::UmgWidget::TryExecuteContentGraph(TEXT("content_graph.import_workspace"), PackageName, Encoded, Payload, ReadOnly, WorkspaceError);
	};
	auto LateCollection = [&](bool Add) {
		auto* BeforeAsset = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		TArray<uint8> BeforeDisk; if (!BeforeAsset || !FFileHelper::LoadFileToArray(BeforeDisk, *Filename)) return false;
		if (!Add) CastChecked<UUserWidget>(BeforeAsset->GeneratedClass->GetDefaultObject())->SetRenderOpacity(0.375f);
		BeforeAsset->MarkPackageDirty();
		TWeakObjectPtr<UWidgetBlueprint> Weak = BeforeAsset;
		auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("operation"), Add ? TEXT("create") : TEXT("remove"));
		Record->SetStringField(TEXT("display_label"), TEXT("Added animation"));
		auto Map = MakeShared<FJsonObject>(); Map->SetObjectField(TEXT("Added"), Record);
		FAssetApplyRequest R; R.AssetPath = PackageName; R.Operation = UeAgentApplyInputContract::EOperation::Update;
		R.ProviderExtension = InlineLifecycleOptions(); R.ProviderExtension->SetObjectField(TEXT("animations"), Map);
		bool SawPublished = false;
		R.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
			auto* A = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath); TArray<uint8> Bytes;
			SawPublished = !Weak.IsValid() && A && !A->GetOutermost()->IsDirty() && A->Status == BS_UpToDate
				&& (FindAdded(*A) != nullptr) == Add && NativeCheck(*A, 0.5f, 0.375f, RCIM_Linear, Add ? 0.25f : 0.375f, true, 60, 2400, 42000, Add ? 2 : 1)
				&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != BeforeDisk;
			Failure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("test_umg_animation_collection_late_rejection")); return false;
		};
		BeforeAsset = nullptr; FAssetApplyResult RResult;
		TestFalse(TEXT("collection late rejection"), ApplyAsset(R, RResult));
		auto* A = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath); TArray<uint8> Bytes;
		return TestTrue(TEXT("collection publication observed before rejection and dirty memory/original disk restored"), SawPublished
			&& RResult.bRollbackSucceeded && A && A->GetOutermost()->IsDirty() && (FindAdded(*A) != nullptr) != Add
			&& NativeCheck(*A, 0.5f, 0.375f, RCIM_Linear, Add ? 0.25f : 0.375f, true, 60, 2400, 42000, Add ? 1 : 2)
			&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes == BeforeDisk);
	};
	Restored = nullptr;
	if (!LateCollection(true)) return false;
	if (!CollectionImport(true, TEXT("-add"))) { AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit()); return false; }
	auto* AddedAsset = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath); auto* Added = AddedAsset ? FindAdded(*AddedAsset) : nullptr;
	auto* NativeDefault = NewObject<UMovieScene>();
	if (!TestTrue(TEXT("ordinary new animation preserves native empty MovieScene defaults and existing nonempty animation"), Added
		&& Added->GetMovieScene()->GetDisplayRate() == NativeDefault->GetDisplayRate()
		&& Added->GetMovieScene()->GetTickResolution() == NativeDefault->GetTickResolution()
		&& Added->GetMovieScene()->GetPlaybackRange() == NativeDefault->GetPlaybackRange()
		&& Added->GetMovieScene()->GetPlaybackRange().GetLowerBound().IsInclusive()
		&& Added->GetMovieScene()->GetPlaybackRange().GetUpperBound().IsExclusive()
		&& Added->GetMovieScene()->GetBindings().IsEmpty() && Added->GetMovieScene()->GetTracks().IsEmpty()
		&& NativeCheck(*AddedAsset, 0.5f, 0.375f, RCIM_Linear, 0.25f, true, 60, 2400, 42000, 2))) return false;
	if (!ExportAssetWorkspace(*AddedAsset, Workspace + TEXT("-added-readback"), TEXT("llm_authoring"), Exported)) return false;
	Added = nullptr; AddedAsset = nullptr; NativeDefault->MarkAsGarbage(); NativeDefault = nullptr;
	if (!LateCollection(false)) return false;
	if (!CollectionImport(false, TEXT("-remove"))) { AddError(WorkspaceError.GetCode()); return false; }
	auto* Final = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	TestTrue(TEXT("ordinary animation omission removes only the target animation"), Final && !FindAdded(*Final)
		&& NativeCheck(*Final, 0.5f, 0.375f, RCIM_Linear, 0.375f, true, 60, 2400, 42000));
	if (Final && !ExportAssetWorkspace(*Final, Workspace + TEXT("-removed-readback"), TEXT("llm_authoring"), Exported)) return false;
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208UmgPropertyKeyIsolationTest,
	"UeAgentInterface.T208.UMG.PropertyKeyIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208UmgPropertyKeyIsolationTest::RunTest(const FString&)
{
	// This probe intentionally exercises the real registered Sequencer editors and normal Close.
	// It establishes only the owned automation-worker context, not an arbitrary open editor.
	FString LogFilename;
	if (!TestTrue(TEXT("formal runner absolute log path supplied"), FParse::Value(FCommandLine::Get(), TEXT("abslog="), LogFilename)
		&& !FPaths::IsRelative(LogFilename))) return false;
	const FString EvidenceRoot = FPaths::GetPath(LogFilename) / FPaths::GetBaseFilename(LogFilename) / TEXT("ini-constructor");
	IFileManager::Get().MakeDirectory(*EvidenceRoot, true);
	const FString PackageName = TEXT("/Game/UAI/Transient/T85UmgKeyProbe_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Widget");
	const FString ObjectPath = PackageName + TEXT(".Widget");
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UE::UAI::Contract::FDiagnostic Error;
	TSharedPtr<UE::UAI::DomainAdapters::AssetDestructiveImport::IUeAgentAssetDeleteOwner> DeleteOwner;
	UE::UAI::Contract::FDiagnostic DeleteOwnerError;
	if (!UE::UAI::DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(
		FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T208/UmgKeyIsolation"), TEXT("umg-key-isolation"), DeleteOwner, DeleteOwnerError)) return false;
	ON_SCOPE_EXIT
	{
		if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
		{
			Asset->GetOutermost()->SetDirtyFlag(false);
			TestTrue(TEXT("key probe fixture formal cleanup"), DeleteOwner->Delete({TEXT("umg-key-cleanup"), {ObjectPath}, false, TEXT("t208")},
				MakeShared<FUmgInlineNeverCancelled>()).IsSuccess());
		}
		UE::UAI::T85::FixtureLifecycle::TryRemoveEmptyRootForPackage(*this, PackageName);
	};
	auto* Package = CreatePackage(*PackageName);
	auto* Original = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package, TEXT("Widget"), BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None));
	if (!TestNotNull(TEXT("original fixture"), Original)) return false;
	FAssetRegistryModule::AssetCreated(Original);
	auto* Canvas = Original->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
	Original->WidgetTree->RootWidget = Canvas;
	auto* Button = Original->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Button"));
	Button->bIsVariable = true; Button->SetRenderOpacity(0.375f); Canvas->AddChild(Button);
	Original->OnVariableAdded(Canvas->GetFName());
	Original->OnVariableAdded(Button->GetFName());
	auto* Animation = NewObject<UWidgetAnimation>(Original, TEXT("Fade"), RF_Transactional);
	Animation->MovieScene = NewObject<UMovieScene>(Animation, TEXT("Fade"), RF_Transactional);
	Animation->MovieScene->SetPlaybackRange(0, 60000);
	Original->Animations.Add(Animation); Original->OnVariableAdded(Animation->GetFName());
	FKismetEditorUtilities::CompileBlueprint(Original);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("probe original saves"), UPackage::SavePackage(Package, Original, *Filename, Save))) return false;
	Package->SetDirtyFlag(false);
	TArray<uint8> DiskBefore;
	if (!FFileHelper::LoadFileToArray(DiskBefore, *Filename)) return false;
	const auto OriginalBefore = CaptureUmgNativeContents(*Original);
	TStrongObjectPtr<UWidgetBlueprint> Scratch(Cast<UWidgetBlueprint>(UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Original,
		CreatePackage(*FString::Printf(TEXT("/Temp/UAI/UmgKeyProbe_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))))));
	if (!TestNotNull(TEXT("owned prepared duplicate"), Scratch.Get())) return false;
	ON_SCOPE_EXIT { Scratch->ClearFlags(RF_Public | RF_Standalone); Scratch->MarkAsGarbage(); };
	FPreviewScene PreviewScene{FPreviewScene::ConstructionValues()};
	TStrongObjectPtr<UUserWidget> Preview(FWidgetBlueprintEditorUtils::CreateUserWidgetFromBlueprint(PreviewScene.GetWorld(), Scratch.Get(),
		{EWidgetDesignFlags::Designing, nullptr}));
	if (!TestNotNull(TEXT("public owned design-time context"), Preview.Get())) return false;
	ON_SCOPE_EXIT { FWidgetBlueprintEditorUtils::DestroyUserWidget(Preview.Get()); };
	auto* PreviewButton = Preview->WidgetTree->FindWidget(TEXT("Button"));
	if (!TestTrue(TEXT("preview and prepared ownership is isolated"), PreviewButton && PreviewButton != Button
		&& PreviewButton->IsIn(Preview.Get()) && !PreviewButton->IsIn(Original)
		&& Preview->GetClass()->ClassGeneratedBy == Scratch.Get()
		&& Scratch->Animations.Num() == 1 && Scratch->Animations[0] != Animation)) return false;
	UWidgetAnimation* PreparedAnimation = Scratch->Animations[0].Get();
	UMovieScene* PreparedScene = PreparedAnimation->GetMovieScene();
	TSharedPtr<ISequencer> Sequencer;
	ON_SCOPE_EXIT { if (Sequencer) { Sequencer->Close(); Sequencer.Reset(); } };
	// Compare full cached INI contents and complete bytes of their files, plus every project
	// generated INI (including newly created files), instead of selecting known settings keys.
	auto CaptureIni = [&](const TCHAR* Stage)
	{
		TMap<FString, FString> Result;
		const FString StageRoot = EvidenceRoot / Stage;
		IFileManager::Get().MakeDirectory(*StageRoot, true);
		FString Manifest;
		int32 Index = 0;
		TArray<FString> Files; GConfig->GetConfigFilenames(Files);
		Files.Sort();
		for (const FString& File : Files)
		{
			if (FConfigFile* Config = GConfig->FindConfigFile(File))
			{
				FString Text; Config->WriteToString(Text); Result.Add(TEXT("cache:") + File, MoveTemp(Text));
				const FString Leaf = FString::Printf(TEXT("%04d-cache.ini"), Index++);
				TestTrue(TEXT("complete cached INI evidence writes"), FFileHelper::SaveStringToFile(*Result[TEXT("cache:") + File], *(StageRoot / Leaf), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
				Manifest += Leaf + TEXT("\tcache\t") + File + TEXT("\n");
			}
		}
		TArray<FString> Generated;
		IFileManager::Get().FindFilesRecursive(Generated, *FPaths::GeneratedConfigDir(), TEXT("*.ini"), true, false);
		for (const FString& File : Generated) Files.AddUnique(File);
		for (const FString& File : Files)
		{
			TArray<uint8> Bytes;
			const bool Exists = IFileManager::Get().FileExists(*File);
			if (Exists)
			{
				TestTrue(TEXT("complete disk INI evidence reads"), FFileHelper::LoadFileToArray(Bytes, *File));
				const FString Leaf = FString::Printf(TEXT("%04d-disk.ini"), Index++);
				TestTrue(TEXT("complete disk INI evidence writes"), FFileHelper::SaveArrayToFile(Bytes, *(StageRoot / Leaf)));
				Manifest += Leaf + TEXT("\tdisk\t") + File + TEXT("\n");
			}
			else Manifest += TEXT("<absent>\tdisk\t") + File + TEXT("\n");
			Result.Add(TEXT("disk:") + File, Exists ? BytesToHex(Bytes.GetData(), Bytes.Num()) : TEXT("<absent>"));
		}
		TestTrue(TEXT("exact filename and stage manifest writes"), FFileHelper::SaveStringToFile(*Manifest, *(StageRoot / TEXT("manifest.tsv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		return Result;
	};
	auto CaptureWorld = []()
	{
		TMap<FString, TArray<uint8>> Result;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UObject*> Objects; Objects.Add(*It); GetObjectsWithOuter(*It, Objects, true);
			for (UObject* Object : Objects)
			{
				TArray<uint8> Bytes; FObjectWriter Writer(Object, Bytes, false, false, false, PPF_DuplicateVerbatim);
				Result.Add(Object->GetPathName(), MoveTemp(Bytes));
			}
		}
		return Result;
	};
	const auto IniBefore = CaptureIni(TEXT("before-create"));
	const auto WorldBefore = CaptureWorld();
	UWorld* EditorWorldBefore = GEditor->GetEditorWorldContext().World();
	const bool WorldDirtyBefore = EditorWorldBefore->GetOutermost()->IsDirty();
	auto CheckIsolation = [&](const TCHAR* Stage)
	{
		TArray<uint8> Disk;
		bool Good = TestTrue(FString(Stage) + TEXT(": original asset unchanged"), OriginalBefore.OrderIndependentCompareEqual(CaptureUmgNativeContents(*Original))
			&& !Package->IsDirty() && Animation->AnimationBindings.IsEmpty() && Animation->MovieScene->GetBindings().IsEmpty()
			&& Animation->MovieScene->GetTracks().IsEmpty() && Button->GetRenderOpacity() == 0.375f
			&& FFileHelper::LoadFileToArray(Disk, *Filename) && Disk == DiskBefore);
		Good &= TestTrue(FString(Stage) + TEXT(": non-target editor world unchanged"), GEditor->GetEditorWorldContext().World() == EditorWorldBefore
			&& EditorWorldBefore->GetOutermost()->IsDirty() == WorldDirtyBefore && WorldBefore.OrderIndependentCompareEqual(CaptureWorld()));
		const auto CurrentIni = CaptureIni(Stage);
		for (const auto& Pair : CurrentIni) if (!IniBefore.Contains(Pair.Key) || IniBefore[Pair.Key] != Pair.Value) AddInfo(FString(Stage) + TEXT(": changed INI ") + Pair.Key);
		Good &= TestTrue(FString(Stage) + TEXT(": complete INI snapshots unchanged"), IniBefore.OrderIndependentCompareEqual(CurrentIni));
		return Good;
	};
	FSequencerInitParams Init;
	Init.RootSequence = PreparedAnimation;
	Init.ViewParams = FSequencerViewParams(TEXT("UAIUmgPropertyKeyIsolation"));
	Init.HostCapabilities.bSupportsCurveEditor = false;
	Init.PlaybackContext = TAttribute<UObject*>(Preview.Get());
	Init.EventContexts = TAttribute<TArray<UObject*>>(TArray<UObject*>{Preview.Get()});
	Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>(TEXT("Sequencer")).CreateSequencer(Init);
	CheckIsolation(TEXT("after-create"));
	TestTrue(TEXT("constructor creates no prepared bindings or tracks"), PreparedScene->GetBindings().IsEmpty() && PreparedScene->GetTracks().IsEmpty());
	Sequencer->Close(); Sequencer.Reset();
	CheckIsolation(TEXT("after-close"));
	AddInfo(TEXT("UMG isolated worker: constructor and normal Close observed; CanKeyProperty and KeyProperty intentionally not run"));
	return !HasAnyErrors();
}
