// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Blueprint.h"

#include "Commands/UeAgentGraphViewUtils.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "EditorViewportClient.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "GraphEditorSettings.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "SEditorViewport.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/SceneViewport.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/GarbageCollection.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"
#include "InputAction.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"

namespace UeAgentBlueprintOps
{
	enum class ECompilerMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static UBlueprint* LoadBlueprintAsset(const FString& InPath)
	{
		return Cast<UBlueprint>(LoadAssetObject(InPath));
	}

	static UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassPath = ClassPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	static FString BlueprintStatusToString(const EBlueprintStatus InStatus)
	{
		switch (InStatus)
		{
		case BS_Unknown: return TEXT("Unknown");
		case BS_Dirty: return TEXT("Dirty");
		case BS_Error: return TEXT("Error");
		case BS_UpToDate: return TEXT("UpToDate");
		case BS_BeingCreated: return TEXT("BeingCreated");
		case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
		default: return TEXT("Unknown");
		}
	}

	static FString CompilerSeverityToString(const EMessageSeverity::Type InSeverity)
	{
		switch (InSeverity)
		{
		case EMessageSeverity::Error:
			return TEXT("Error");
		case EMessageSeverity::Warning:
			return TEXT("Warning");
		case EMessageSeverity::PerformanceWarning:
			return TEXT("PerformanceWarning");
		case EMessageSeverity::Info:
			return TEXT("Info");
		default:
			return TEXT("Unknown");
		}
	}

	static bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter)
	{
		FString Normalized = InFilterText.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		Normalized.ReplaceInline(TEXT(" "), TEXT(""));

		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ECompilerMessageSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Error;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Warning;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("note") || Normalized == TEXT("notes"))
		{
			OutFilter = ECompilerMessageSeverityFilter::Info;
			return true;
		}
		if (Normalized == TEXT("warningorerror") || Normalized == TEXT("warnorerror") || Normalized == TEXT("errorsandwarnings"))
		{
			OutFilter = ECompilerMessageSeverityFilter::WarningOrError;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompilerMessage(const EMessageSeverity::Type InSeverity, const ECompilerMessageSeverityFilter InFilter)
	{
		switch (InFilter)
		{
		case ECompilerMessageSeverityFilter::All:
			return true;
		case ECompilerMessageSeverityFilter::Error:
			return InSeverity == EMessageSeverity::Error;
		case ECompilerMessageSeverityFilter::Warning:
			return InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		case ECompilerMessageSeverityFilter::Info:
			return InSeverity == EMessageSeverity::Info;
		case ECompilerMessageSeverityFilter::WarningOrError:
			return InSeverity == EMessageSeverity::Error || InSeverity == EMessageSeverity::Warning || InSeverity == EMessageSeverity::PerformanceWarning;
		default:
			return true;
		}
	}

	static void BuildCompilerLogJson(
		const FCompilerResultsLog& InCompilerLog,
		const ECompilerMessageSeverityFilter InFilter,
		const int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutFilteredErrorCount,
		int32& OutFilteredWarningCount,
		int32& OutFilteredInfoCount)
	{
		OutFilteredErrorCount = 0;
		OutFilteredWarningCount = 0;
		OutFilteredInfoCount = 0;
		OutMessages.Reset();

		const int32 MaxMessages = FMath::Clamp(InMaxMessages, 1, 2000);
		for (const TSharedRef<FTokenizedMessage>& Message : InCompilerLog.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (!ShouldIncludeCompilerMessage(Severity, InFilter))
			{
				continue;
			}

			if (Severity == EMessageSeverity::Error)
			{
				++OutFilteredErrorCount;
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				++OutFilteredWarningCount;
			}
			else if (Severity == EMessageSeverity::Info)
			{
				++OutFilteredInfoCount;
			}

			if (OutMessages.Num() >= MaxMessages)
			{
				continue;
			}

			TSharedPtr<FJsonObject> MessageObj = MakeShared<FJsonObject>();
			MessageObj->SetStringField(TEXT("severity"), CompilerSeverityToString(Severity));
			MessageObj->SetStringField(TEXT("message"), Message->ToText().ToString());
			MessageObj->SetStringField(TEXT("identifier"), Message->GetIdentifier().ToString());
			OutMessages.Add(MakeShared<FJsonValueObject>(MessageObj));
		}
	}

	static UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& InGraphName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		FString GraphName = InGraphName;
		GraphName.TrimStartAndEndInline();
		if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
		{
			return UBlueprintEditorLibrary::FindEventGraph(Blueprint);
		}

		auto MatchesGraph = [&GraphName](UEdGraph* Graph) -> bool
		{
			return Graph
				&& (Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)
					|| Graph->GetPathName().Equals(GraphName, ESearchCase::IgnoreCase));
		};

		const bool bIsLikelyGraphPath = GraphName.Contains(TEXT("."));
		if (!bIsLikelyGraphPath)
		{
			if (UEdGraph* Graph = UBlueprintEditorLibrary::FindGraph(Blueprint, FName(*GraphName)))
			{
				return Graph;
			}
		}

		auto FindFromList = [&MatchesGraph](const TArray<UEdGraph*>& InGraphs) -> UEdGraph*
		{
			for (UEdGraph* Graph : InGraphs)
			{
				if (MatchesGraph(Graph))
				{
					return Graph;
				}
			}
			return nullptr;
		};

		if (UEdGraph* Graph = FindFromList(Blueprint->UbergraphPages))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->FunctionGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->MacroGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->DelegateSignatureGraphs))
		{
			return Graph;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (MatchesGraph(Graph))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	static bool IsUserConstructionScriptGraph(const UEdGraph* Graph)
	{
		return Graph && Graph->GetName().Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase);
	}

	static bool IsUnsafeConstructionScriptPcgCall(const FString& FunctionOwnerClass, const FString& FunctionName)
	{
		const bool bOwnerIsPcgComponent =
			FunctionOwnerClass.Contains(TEXT("PCGComponent"), ESearchCase::IgnoreCase)
			&& FunctionOwnerClass.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
		if (bOwnerIsPcgComponent)
		{
			return FunctionName.Equals(TEXT("SetGraph"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("SetGraphInterface"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("Generate"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("GenerateGetTaskId"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("GenerateLocal"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("GenerateLocalGetTaskId"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("Cleanup"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("CleanupLocal"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("Refresh"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("RefreshAfterGraphChanged"), ESearchCase::IgnoreCase)
				|| FunctionName.Equals(TEXT("NotifyPropertiesChangedFromBlueprint"), ESearchCase::IgnoreCase);
		}

		const bool bOwnerIsPcgGraphParametersHelpers =
			FunctionOwnerClass.Contains(TEXT("PCGGraphParametersHelpers"), ESearchCase::IgnoreCase);
		return bOwnerIsPcgGraphParametersHelpers && FunctionName.StartsWith(TEXT("Set"), ESearchCase::IgnoreCase);
	}

	static bool RequiresExplicitParentOrEventNode(const FString& FunctionName)
	{
		return FunctionName.Equals(TEXT("ReceiveBeginPlay"), ESearchCase::IgnoreCase);
	}

	static bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_not_found");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Blueprint->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool PersistBlueprintGraphDocumentViewState(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FVector2f& ViewLocation,
		float ZoomAmount,
		int32* OutRemovedExistingEntryCount = nullptr)
	{
		if (OutRemovedExistingEntryCount)
		{
			*OutRemovedExistingEntryCount = 0;
		}
		if (!Blueprint || !Graph)
		{
			return false;
		}

		const FSoftObjectPath GraphPath(Graph);
		const int32 BeforeCount = Blueprint->LastEditedDocuments.Num();
		Blueprint->LastEditedDocuments.RemoveAll(
			[&GraphPath](const FEditedDocumentInfo& Info)
			{
				return Info.EditedObjectPath == GraphPath;
			});
		if (OutRemovedExistingEntryCount)
		{
			*OutRemovedExistingEntryCount = BeforeCount - Blueprint->LastEditedDocuments.Num();
		}

		const FVector2f SnappedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(ViewLocation, ZoomAmount);
		Blueprint->LastEditedDocuments.Add(FEditedDocumentInfo(Graph, SnappedViewLocation, ZoomAmount));
		return true;
	}

	static bool TryGetBlueprintGraphDocumentViewState(
		const UBlueprint* Blueprint,
		const UEdGraph* Graph,
		FVector2f& OutViewLocation,
		float& OutZoomAmount)
	{
		if (!Blueprint || !Graph)
		{
			return false;
		}

		const FSoftObjectPath GraphPath(Graph);
		for (int32 Index = Blueprint->LastEditedDocuments.Num() - 1; Index >= 0; --Index)
		{
			const FEditedDocumentInfo& Info = Blueprint->LastEditedDocuments[Index];
			if (Info.EditedObjectPath == GraphPath)
			{
				OutViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(Info.SavedViewOffset, Info.SavedZoomAmount);
				OutZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(Info.SavedZoomAmount);
				return true;
			}
		}
		return false;
	}

	static bool RestoreBlueprintGraphEditorToDocumentViewState(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedPtr<SGraphEditor>& GraphEditor,
		FVector2f* OutRestoredViewLocation = nullptr,
		float* OutRestoredZoomAmount = nullptr)
	{
		if (!Blueprint || !Graph || !GraphEditor.IsValid())
		{
			return false;
		}

		FVector2f RestoreViewLocation(0.0f, 0.0f);
		float RestoreZoomAmount = 1.0f;
		if (!TryGetBlueprintGraphDocumentViewState(Blueprint, Graph, RestoreViewLocation, RestoreZoomAmount))
		{
			GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(RestoreZoomAmount);
			RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(RestoreViewLocation, RestoreZoomAmount);
		}

		GraphEditor->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(RestoreZoomAmount);
		if (!UeAgentGraphViewUtils::IsViewLocationPixelAligned(RestoreViewLocation, RestoreZoomAmount))
		{
			RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(RestoreViewLocation, RestoreZoomAmount);
			GraphEditor->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		}
		if (OutRestoredViewLocation)
		{
			*OutRestoredViewLocation = RestoreViewLocation;
		}
		if (OutRestoredZoomAmount)
		{
			*OutRestoredZoomAmount = RestoreZoomAmount;
		}
		return true;
	}

	static TSharedPtr<IBlueprintEditor> GetBlueprintEditor(UBlueprint* Blueprint, bool bOpenIfNeeded, FString& OutError)
	{
		if (!Blueprint)
		{
			OutError = TEXT("blueprint_not_found");
			return nullptr;
		}

		TSharedPtr<IBlueprintEditor> BlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, bOpenIfNeeded);
		if (!BlueprintEditor.IsValid())
		{
			OutError = bOpenIfNeeded ? TEXT("blueprint_editor_open_failed") : TEXT("blueprint_editor_not_opened");
			return nullptr;
		}
		return BlueprintEditor;
	}

	static TSharedPtr<SGraphEditor> CreateOffscreenGraphEditorForCapture(UEdGraph* Graph, const FString& CornerText, FString& OutError)
	{
		if (!Graph)
		{
			OutError = TEXT("graph_not_found");
			return nullptr;
		}

		FGraphAppearanceInfo AppearanceInfo;
		AppearanceInfo.CornerText = FText::FromString(CornerText);
		TSharedPtr<SGraphEditor> GraphEditor = SNew(SGraphEditor)
			.IsEditable(false)
			.DisplayAsReadOnly(true)
			.ShowGraphStateOverlay(false)
			.Appearance(AppearanceInfo)
			.GraphToEdit(Graph);
		if (!GraphEditor.IsValid())
		{
			OutError = TEXT("offscreen_graph_editor_create_failed");
			return nullptr;
		}
		return GraphEditor;
	}

	static bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget)
	{
		if (RootWidget->GetType() == TypeName)
		{
			OutWidget = RootWidget;
			return true;
		}

		FChildren* Children = RootWidget->GetChildren();
		if (!Children)
		{
			return false;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			const TSharedRef<SWidget> ChildWidget = Children->GetChildAt(ChildIndex);
			if (FindWidgetByTypeRecursive(ChildWidget, TypeName, OutWidget))
			{
				return true;
			}
		}
		return false;
	}

	static void FindWidgetsByTypeRecursiveAll(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		if (RootWidget->GetType() == TypeName)
		{
			OutWidgets.Add(RootWidget);
		}

		FChildren* Children = RootWidget->GetChildren();
		if (!Children)
		{
			return;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			FindWidgetsByTypeRecursiveAll(Children->GetChildAt(ChildIndex), TypeName, OutWidgets);
		}
	}

	static TSharedPtr<SDockTab> ResolveBlueprintTab(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, const FName& TabId, FString& OutError)
	{
		if (!BlueprintEditor.IsValid())
		{
			OutError = TEXT("blueprint_editor_not_opened");
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("blueprint_tab_manager_not_found");
			return nullptr;
		}

		TSharedPtr<SDockTab> Tab = TabManager->FindExistingLiveTab(FTabId(TabId));
		if (!Tab.IsValid())
		{
			Tab = TabManager->TryInvokeTab(FTabId(TabId), false);
		}
		if (!Tab.IsValid())
		{
			OutError = TEXT("blueprint_tab_not_found");
			return nullptr;
		}
		Tab->ActivateInParent(ETabActivationCause::SetDirectly);
		return Tab;
	}

	static TSharedPtr<SWindow> ResolveBlueprintEditorWindow(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, FString& OutError)
	{
		if (!BlueprintEditor.IsValid())
		{
			OutError = TEXT("blueprint_editor_not_opened");
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("blueprint_tab_manager_not_found");
			return nullptr;
		}

		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = TEXT("blueprint_owner_tab_not_found");
			return nullptr;
		}

		const TSharedPtr<SWindow> ParentWindow = OwnerTab->GetParentWindow();
		if (!ParentWindow.IsValid())
		{
			OutError = TEXT("blueprint_window_not_found");
			return nullptr;
		}
		return ParentWindow;
	}

	static TSharedPtr<SEditorViewport> ResolveBlueprintViewportWidget(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, FString& OutError)
	{
		const TArray<FName> CandidateTabIds = {
			FBlueprintEditorTabs::SCSViewportID,
			FName(TEXT("Viewport")),
			FName(TEXT("PreviewViewport")),
			FName(TEXT("AnimBlueprintPreviewEditor"))
		};

		auto ResolveViewportFromTab = [](const TSharedPtr<SDockTab>& ViewportTab) -> TSharedPtr<SEditorViewport>
		{
			if (!ViewportTab.IsValid())
			{
				return nullptr;
			}

			const TSharedPtr<SWidget> RootWidget = ViewportTab->GetContent();
			if (!RootWidget.IsValid())
			{
				return nullptr;
			}

			TSharedPtr<SWidget> FoundWidget;
			if (!FindWidgetByTypeRecursive(RootWidget.ToSharedRef(), FName(TEXT("SSCSEditorViewport")), FoundWidget) &&
				!FindWidgetByTypeRecursive(RootWidget.ToSharedRef(), FName(TEXT("SAnimationEditorViewport")), FoundWidget) &&
				!FindWidgetByTypeRecursive(RootWidget.ToSharedRef(), FName(TEXT("SEditorViewport")), FoundWidget))
			{
				return nullptr;
			}

			const TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(FoundWidget);
			if (ViewportWidget.IsValid() && ViewportWidget->GetViewportClient().IsValid())
			{
				return ViewportWidget;
			}
			return nullptr;
		};

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor.IsValid() ? BlueprintEditor->GetTabManager() : nullptr;
		if (TabManager.IsValid())
		{
			for (const FName& TabId : CandidateTabIds)
			{
				const TSharedPtr<SDockTab> ViewportTab = TabManager->FindExistingLiveTab(FTabId(TabId));
				const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(ViewportTab);
				if (ViewportWidget.IsValid())
				{
					return ViewportWidget;
				}
			}
		}

		for (const FName& TabId : CandidateTabIds)
		{
			FString LocalError;
			const TSharedPtr<SDockTab> ViewportTab = ResolveBlueprintTab(BlueprintEditor, TabId, LocalError);
			const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(ViewportTab);
			if (ViewportWidget.IsValid())
			{
				return ViewportWidget;
			}
		}

		OutError = TEXT("blueprint_viewport_widget_not_found");
		return nullptr;
	}

	static TSharedPtr<SWidget> ResolveBlueprintViewportSceneWidget(const TSharedRef<SEditorViewport>& ViewportWidget)
	{
		TArray<TSharedRef<SWidget>> ViewportChildren;
		FindWidgetsByTypeRecursiveAll(ViewportWidget, FName(TEXT("SViewport")), ViewportChildren);
		if (ViewportChildren.Num() <= 0)
		{
			return nullptr;
		}

		TSharedRef<SWidget> BestWidget = ViewportChildren[0];
		float BestArea = 0.0f;
		for (const TSharedRef<SWidget>& Candidate : ViewportChildren)
		{
			const FVector2D Size = Candidate->GetTickSpaceGeometry().GetAbsoluteSize();
			const float Area = Size.X * Size.Y;
			if (Area > BestArea)
			{
				BestArea = Area;
				BestWidget = Candidate;
			}
		}
		return BestWidget;
	}

	static void PrepareBlueprintViewportForCapture(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, UBlueprint* Blueprint, const TSharedPtr<SEditorViewport>& ViewportWidget)
	{
		if (BlueprintEditor.IsValid())
		{
			const TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor = StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor);
			if (ConcreteBlueprintEditor.IsValid() && Blueprint)
			{
				ConcreteBlueprintEditor->UpdatePreviewActor(Blueprint, true);

				AActor* PreviewActor = ConcreteBlueprintEditor->GetPreviewActor();
				if (PreviewActor)
				{
					const FBox Bounds = PreviewActor->GetComponentsBoundingBox(true);
					FUeAgentInterfaceLogger::Log(
						TEXT("PrepareBlueprintViewportForCapture preview_actor=%s bounds_min=(%.1f,%.1f,%.1f) bounds_max=(%.1f,%.1f,%.1f)"),
						*PreviewActor->GetName(),
						Bounds.Min.X,
						Bounds.Min.Y,
						Bounds.Min.Z,
						Bounds.Max.X,
						Bounds.Max.Y,
						Bounds.Max.Z);
				}
				else
				{
					FUeAgentInterfaceLogger::Log(TEXT("PrepareBlueprintViewportForCapture preview_actor=null"));
				}
			}
		}

		if (!ViewportWidget.IsValid())
		{
			return;
		}

		if (const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			const bool bNeedsInitialFocus =
				ViewportClient->GetViewLocation().IsNearlyZero(0.01f) &&
				ViewportClient->GetViewRotation().Equals(FRotator::ZeroRotator, 0.01f);
			if (bNeedsInitialFocus)
			{
				const TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor = BlueprintEditor.IsValid() ? StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor) : nullptr;
				if (ConcreteBlueprintEditor.IsValid() && ConcreteBlueprintEditor->GetPreviewActor())
				{
					const FBox PreviewBounds = ConcreteBlueprintEditor->GetPreviewActor()->GetComponentsBoundingBox(true);
					FUeAgentInterfaceLogger::Log(
						TEXT("PrepareBlueprintViewportForCapture viewport camera looks uninitialized -> FocusViewportOnBox bounds_min=(%.1f,%.1f,%.1f) bounds_max=(%.1f,%.1f,%.1f)"),
						PreviewBounds.Min.X,
						PreviewBounds.Min.Y,
						PreviewBounds.Min.Z,
						PreviewBounds.Max.X,
						PreviewBounds.Max.Y,
						PreviewBounds.Max.Z);
					ViewportClient->FocusViewportOnBox(PreviewBounds, true);
				}
			}

			ViewportClient->Invalidate();
			FUeAgentInterfaceLogger::Log(
				TEXT("PrepareBlueprintViewportForCapture viewport_client realtime=%s view_loc=(%.1f,%.1f,%.1f) view_rot=(%.1f,%.1f,%.1f)"),
				ViewportClient->IsRealtime() ? TEXT("true") : TEXT("false"),
				ViewportClient->GetViewLocation().X,
				ViewportClient->GetViewLocation().Y,
				ViewportClient->GetViewLocation().Z,
				ViewportClient->GetViewRotation().Pitch,
				ViewportClient->GetViewRotation().Yaw,
				ViewportClient->GetViewRotation().Roll);
		}

		if (const TSharedPtr<FSceneViewport> SceneViewport = ViewportWidget->GetSceneViewport())
		{
			SceneViewport->InvalidateDisplay();
			SceneViewport->DeferInvalidateHitProxy();
			FUeAgentInterfaceLogger::Log(
				TEXT("PrepareBlueprintViewportForCapture scene_viewport size=(%d,%d)"),
				SceneViewport->GetSizeXY().X,
				SceneViewport->GetSizeXY().Y);
		}
	}

	static bool EnsureSlateWindowRenderableForCapture(
		const TSharedPtr<SWindow>& Window,
		FString& OutError,
		const bool bAllowWindowMutation = true,
		bool* OutWindowMutated = nullptr)
	{
		bool bWindowMutated = false;
		auto Finish = [&]()
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bWindowMutated;
			}
		};

		if (!Window.IsValid() || !FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_window_not_available");
			Finish();
			return false;
		}

		if (Window->IsWindowMinimized())
		{
			if (!bAllowWindowMutation)
			{
				OutError = TEXT("slate_window_minimized_ui_mutation_disabled");
				Finish();
				return false;
			}
			Window->Restore();
			bWindowMutated = true;
			FSlateApplication::Get().PumpMessages();
		}
		if (!Window->IsVisible())
		{
			if (!bAllowWindowMutation)
			{
				OutError = TEXT("slate_window_hidden_ui_mutation_disabled");
				Finish();
				return false;
			}
			Window->ShowWindow();
			bWindowMutated = true;
		}
		if (bAllowWindowMutation)
		{
			Window->BringToFront(true);
			bWindowMutated = true;
			FSlateApplication::Get().PumpMessages();
		}

		if (!Window->IsVisible() || Window->IsWindowMinimized())
		{
			OutError = TEXT("slate_window_not_renderable");
			Finish();
			return false;
		}
		Finish();
		return true;
	}

	static void PumpAndTickSlateForCapture()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FSlateApplication& SlateApp = FSlateApplication::Get();
		SlateApp.PumpMessages();
		if (!SlateApp.IsTicking())
		{
			SlateApp.Tick(ESlateTickType::All);
		}
	}

	static bool PrepareSlateWindowForCapture(
		const TSharedPtr<SWindow>& Window,
		FString& OutError,
		const bool bAllowWindowMutation = true,
		bool* OutWindowMutated = nullptr)
	{
		bool bWindowMutated = false;
		if (!EnsureSlateWindowRenderableForCapture(Window, OutError, bAllowWindowMutation, &bWindowMutated))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bWindowMutated;
			}
			return false;
		}
		if (!bAllowWindowMutation)
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bWindowMutated;
			}
			return true;
		}
		for (int32 FrameIndex = 0; FrameIndex < 3; ++FrameIndex)
		{
			PumpAndTickSlateForCapture();
			const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
			Window->SlatePrepass(WindowScale);
			FSlateApplication::Get().ForceRedrawWindow(Window.ToSharedRef());
		}
		PumpAndTickSlateForCapture();
		if (OutWindowMutated)
		{
			*OutWindowMutated = true;
		}
		return true;
	}

	static bool TryResolveOffscreenWidgetDrawSize(
		const TSharedRef<SWidget>& Widget,
		const bool bAllowFallbackSize,
		FVector2D& OutDrawSize,
		FString& OutError)
	{
		FVector2D DrawSize = Widget->GetTickSpaceGeometry().GetLocalSize();
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetTickSpaceGeometry().GetAbsoluteSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetCachedGeometry().GetLocalSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetDesiredSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			if (!bAllowFallbackSize)
			{
				OutError = TEXT("widget_geometry_unavailable_for_noninvasive_screenshot");
				return false;
			}
			DrawSize = FVector2D(1440.0, 900.0);
		}

		const double MaxDimension = 8192.0;
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 64.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 64.0, MaxDimension);
		OutDrawSize = DrawSize;
		return true;
	}

	static FVector2D ResolveOffscreenWidgetDrawSize(const TSharedRef<SWidget>& Widget)
	{
		FVector2D DrawSize(1440.0, 900.0);
		FString Error;
		TryResolveOffscreenWidgetDrawSize(Widget, true, DrawSize, Error);
		return DrawSize;
	}

	static bool ScreenshotSlateWidgetWithDrawSize(
		const TSharedRef<SWidget>& Widget,
		const FVector2D& DrawSize,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		FWidgetRenderer* WidgetRenderer = new FWidgetRenderer(true, true);
		if (!WidgetRenderer)
		{
			OutError = TEXT("widget_renderer_create_failed");
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
		if (!RenderTarget)
		{
			BeginCleanup(WidgetRenderer);
			OutError = TEXT("offscreen_render_target_create_failed");
			return false;
		}

		WidgetRenderer->DrawWidget(RenderTarget, Widget, 1.0f, DrawSize, 0.0f, false);
		FlushRenderingCommands();
		BeginCleanup(WidgetRenderer);

		FRenderTarget* RenderTargetResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTargetResource)
		{
			OutError = TEXT("offscreen_render_target_resource_missing");
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		OutPixels.Reset();
		if (!RenderTargetResource->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("offscreen_read_pixels_failed");
			return false;
		}

		OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
		if (OutSize.X <= 0 || OutSize.Y <= 0 || OutPixels.Num() < OutSize.X * OutSize.Y)
		{
			OutError = TEXT("offscreen_screenshot_empty");
			return false;
		}
		return true;
	}

	static bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		FSlateApplication::Get().PumpMessages();
		const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
		Widget->SlatePrepass(WidgetScale);

		const FVector2D DrawSize = ResolveOffscreenWidgetDrawSize(Widget);
		return ScreenshotSlateWidgetWithDrawSize(Widget, DrawSize, OutPixels, OutSize, OutError);
	}

	static bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		FVector2D DrawSize(0.0, 0.0);
		if (!TryResolveOffscreenWidgetDrawSize(Widget, false, DrawSize, OutError))
		{
			OutError.Reset();
			if (!TryResolveOffscreenWidgetDrawSize(Widget, true, DrawSize, OutError))
			{
				return false;
			}
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotSlateWidgetNonInvasive using fallback draw size for widget=%s size=(%.0f,%.0f)"),
				*Widget->GetTypeAsString(),
				DrawSize.X,
				DrawSize.Y);
		}
		return ScreenshotSlateWidgetWithDrawSize(Widget, DrawSize, OutPixels, OutSize, OutError);
	}

	static bool ScreenshotSlateWidgetNonInvasive(
		const TSharedRef<SWidget>& Widget,
		const FVector2D& RequestedDrawSize,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		const double MaxDimension = 8192.0;
		FVector2D DrawSize(
			FMath::Clamp(FMath::RoundToDouble(RequestedDrawSize.X), 64.0, MaxDimension),
			FMath::Clamp(FMath::RoundToDouble(RequestedDrawSize.Y), 64.0, MaxDimension));
		return ScreenshotSlateWidgetWithDrawSize(Widget, DrawSize, OutPixels, OutSize, OutError);
	}

	static void PrepareOffscreenGraphEditorForCapture(
		const TSharedPtr<SGraphEditor>& GraphEditor,
		const FVector2f& ViewLocation,
		const float ZoomAmount,
		const int32 FrameCount = 2)
	{
		if (!GraphEditor.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		const float SafeZoom = UeAgentGraphViewUtils::SafeGraphZoomAmount(ZoomAmount);
		const FVector2f SnappedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(ViewLocation, SafeZoom);
		GraphEditor->SetViewLocation(SnappedViewLocation, SafeZoom);
		SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
		for (int32 FrameIndex = 0; FrameIndex < FMath::Max(1, FrameCount); ++FrameIndex)
		{
			if (GraphPanel)
			{
				GraphPanel->Update();
			}
			PumpAndTickSlateForCapture();
			const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
			GraphEditor->SlatePrepass(WidgetScale);
			if (GraphPanel)
			{
				GraphPanel->SlatePrepass(WidgetScale);
			}
		}
	}

	static bool ScreenshotEditorViewportPixels(const TSharedRef<SEditorViewport>& ViewportWidget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (!ViewportClient.IsValid())
		{
			OutError = TEXT("blueprint_viewport_client_not_found");
			return false;
		}

		FViewport* RawViewport = ViewportClient->Viewport;
		if (!RawViewport)
		{
			OutError = TEXT("blueprint_viewport_not_found");
			const FGeometry& WidgetGeometry = ViewportWidget->GetTickSpaceGeometry();
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotEditorViewportPixels missing raw viewport widget_local=(%.1f,%.1f) widget_abs=(%.1f,%.1f) widget_pos=(%.1f,%.1f)"),
				WidgetGeometry.GetLocalSize().X,
				WidgetGeometry.GetLocalSize().Y,
				WidgetGeometry.GetAbsoluteSize().X,
				WidgetGeometry.GetAbsoluteSize().Y,
				WidgetGeometry.GetAbsolutePosition().X,
				WidgetGeometry.GetAbsolutePosition().Y);
			return false;
		}

		const FIntPoint ViewportSize = RawViewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = TEXT("blueprint_scene_viewport_zero_size");
			const FGeometry& WidgetGeometry = ViewportWidget->GetTickSpaceGeometry();
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotEditorViewportPixels zero-size raw_viewport=(%d,%d) widget_local=(%.1f,%.1f) widget_abs=(%.1f,%.1f) widget_pos=(%.1f,%.1f)"),
				ViewportSize.X,
				ViewportSize.Y,
				WidgetGeometry.GetLocalSize().X,
				WidgetGeometry.GetLocalSize().Y,
				WidgetGeometry.GetAbsoluteSize().X,
				WidgetGeometry.GetAbsoluteSize().Y,
				WidgetGeometry.GetAbsolutePosition().X,
				WidgetGeometry.GetAbsolutePosition().Y);
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(true);
		OutPixels.Reset();
		if (!RawViewport->ReadPixels(OutPixels, ReadFlags))
		{
			OutError = TEXT("blueprint_scene_viewport_read_pixels_failed");
			return false;
		}

		if (OutPixels.Num() <= 0)
		{
			OutError = TEXT("blueprint_scene_viewport_empty_pixels");
			return false;
		}

		OutSize = ViewportSize;
		return true;
	}

	static bool ScreenshotBlueprintWindow(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError,
		const bool bAllowWindowMutation = true,
		bool* OutWindowMutated = nullptr)
	{
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
		if (!BlueprintWindow.IsValid())
		{
			return false;
		}
		if (!PrepareSlateWindowForCapture(BlueprintWindow, OutError, bAllowWindowMutation, OutWindowMutated))
		{
			return false;
		}
		return ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), OutPixels, OutSize, OutError);
	}

	static bool BuildWidgetCropRectInWindowPixels(
		const TSharedRef<SWindow>& Window,
		const TSharedRef<SWidget>& Widget,
		const FIntPoint& WindowShotSize,
		FIntRect& OutRect,
		FString& OutError)
	{
		const FGeometry& WindowGeometry = Window->GetTickSpaceGeometry();
		const FGeometry& WidgetGeometry = Widget->GetTickSpaceGeometry();

		const FVector2D WindowLocalSize = WindowGeometry.GetLocalSize();
		const FVector2D WidgetAbsPos = WidgetGeometry.GetAbsolutePosition();
		const FVector2D WidgetAbsSize = WidgetGeometry.GetAbsoluteSize();
		const FVector2D LocalTopLeft = WindowGeometry.AbsoluteToLocal(WidgetAbsPos);
		const FVector2D LocalBottomRight = WindowGeometry.AbsoluteToLocal(WidgetAbsPos + WidgetAbsSize);

		if (WindowLocalSize.X <= 1.0 || WindowLocalSize.Y <= 1.0)
		{
			OutError = TEXT("blueprint_window_geometry_invalid");
			return false;
		}
		if (WidgetAbsSize.X <= 1.0 || WidgetAbsSize.Y <= 1.0)
		{
			OutError = TEXT("blueprint_widget_geometry_invalid");
			return false;
		}

		const double ScaleX = (double)WindowShotSize.X / (double)WindowLocalSize.X;
		const double ScaleY = (double)WindowShotSize.Y / (double)WindowLocalSize.Y;
		int32 Left = FMath::FloorToInt(LocalTopLeft.X * ScaleX);
		int32 Top = FMath::FloorToInt(LocalTopLeft.Y * ScaleY);
		int32 Right = FMath::CeilToInt(LocalBottomRight.X * ScaleX);
		int32 Bottom = FMath::CeilToInt(LocalBottomRight.Y * ScaleY);

		Left = FMath::Clamp(Left, 0, WindowShotSize.X - 1);
		Top = FMath::Clamp(Top, 0, WindowShotSize.Y - 1);
		Right = FMath::Clamp(Right, Left + 1, WindowShotSize.X);
		Bottom = FMath::Clamp(Bottom, Top + 1, WindowShotSize.Y);

		if (Right <= Left || Bottom <= Top)
		{
			OutError = TEXT("blueprint_widget_crop_invalid");
			return false;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("BuildWidgetCropRectInWindowPixels window_local=(%.1f,%.1f) window_shot=(%d,%d) widget_abs_pos=(%.1f,%.1f) widget_abs_size=(%.1f,%.1f) crop=(%d,%d)-(%d,%d)"),
			WindowLocalSize.X,
			WindowLocalSize.Y,
			WindowShotSize.X,
			WindowShotSize.Y,
			WidgetAbsPos.X,
			WidgetAbsPos.Y,
			WidgetAbsSize.X,
			WidgetAbsSize.Y,
			Left,
			Top,
			Right,
			Bottom);

		OutRect = FIntRect(Left, Top, Right, Bottom);
		return true;
	}

	static bool CropPixelsRect(
		const TArray<FColor>& InPixels,
		const FIntPoint& InSize,
		const FIntRect& CropRect,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != InSize.X * InSize.Y)
		{
			OutError = TEXT("invalid_source_pixels");
			return false;
		}

		const int32 CropWidth = CropRect.Width();
		const int32 CropHeight = CropRect.Height();
		if (CropWidth <= 0 || CropHeight <= 0)
		{
			OutError = TEXT("invalid_crop_rect");
			return false;
		}

		OutPixels.Reset();
		OutPixels.AddUninitialized(CropWidth * CropHeight);
		for (int32 Row = 0; Row < CropHeight; ++Row)
		{
			const int32 SourceIndex = (CropRect.Min.Y + Row) * InSize.X + CropRect.Min.X;
			const int32 DestIndex = Row * CropWidth;
			FMemory::Memcpy(
				OutPixels.GetData() + DestIndex,
				InPixels.GetData() + SourceIndex,
				sizeof(FColor) * CropWidth);
		}

		OutSize = FIntPoint(CropWidth, CropHeight);
		return true;
	}

	static bool ScreenshotBlueprintWindowCropWidget(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		const TSharedRef<SWidget>& Widget,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError,
		const bool bAllowWindowMutation = true,
		bool* OutWindowMutated = nullptr)
	{
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
		if (!BlueprintWindow.IsValid())
		{
			return false;
		}

		bool bAnyWindowMutated = false;
		bool bWindowMutatedThisPass = false;
		if (!PrepareSlateWindowForCapture(BlueprintWindow, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
			}
			return false;
		}
		bAnyWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;

		TArray<FColor> WindowPixels;
		FIntPoint WindowSize(0, 0);
		if (!ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("ScreenshotBlueprintWindowCropWidget warmup window_shot=(%d,%d)"),
			WindowSize.X,
			WindowSize.Y);

		bWindowMutatedThisPass = false;
		if (!PrepareSlateWindowForCapture(BlueprintWindow, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
			}
			return false;
		}
		bAnyWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
		if (!ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		FIntRect CropRect;
		if (!BuildWidgetCropRectInWindowPixels(BlueprintWindow.ToSharedRef(), Widget, WindowSize, CropRect, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		if (OutWindowMutated)
		{
			*OutWindowMutated = bAnyWindowMutated;
		}
		return CropPixelsRect(WindowPixels, WindowSize, CropRect, OutPixels, OutSize, OutError);
	}
}


#include "UeAgentHttpServer_Blueprint_GraphLayout.inl"
#include "UeAgentHttpServer_Blueprint_AssetAndViewport.inl"
#include "UeAgentHttpServer_Blueprint_Components.inl"
#include "UeAgentHttpServer_Blueprint_GraphEditing.inl"
#include "UeAgentHttpServer_Blueprint_FolderFormat.inl"
