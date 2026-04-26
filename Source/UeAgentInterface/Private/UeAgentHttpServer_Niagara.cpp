// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Niagara.h"
#include "UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorModule.h"
#include "NiagaraGraph.h"
#include "NiagaraMessageStore.h"
#include "NiagaraMessages.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptExecutionContext.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraTypes.h"
#include "NiagaraWorldManager.h"
#include "NiagaraParameterMapHistory.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/GarbageCollection.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackObject.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"
#include "UObject/UnrealType.h"
#include "UeAgentInterfaceSettings.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

#include "EdGraphSchema_Niagara.h"

namespace UeAgentNiagaraOps
{
	static UClass* ResolveClassByPath(const FString& InClassPath);
	static FString ScriptUsageToString(const ENiagaraScriptUsage Usage);
	static FString ScriptCompileStatusToString(const ENiagaraScriptCompileStatus Status);
	static FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId);
	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules);
	static FString GetModuleScriptPath(UNiagaraNodeFunctionCall& ModuleNode);
	static UEdGraphPin* FindOverridePinByAliasedInputName(UNiagaraGraph* Graph, const FName AliasedInputName);
	static bool EnsureGraphNodeGuid(UEdGraphNode* Node, TSet<FGuid>* ExistingGuids = nullptr);
	static int32 NormalizeGraphNodeGuids(UEdGraph* Graph);

	template<typename TEnum>
	static FString NiagaraEnumToString(const TEnum Value)
	{
		if (const UEnum* Enum = StaticEnum<TEnum>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::Printf(TEXT("%d"), static_cast<int32>(Value));
	}

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
			const bool bIsSubobjectPath = ObjectPath.Contains(TEXT(":"));
			return (IsValid(Existing) && (Existing->HasAnyFlags(RF_Public) || bIsSubobjectPath)) ? Existing : nullptr;
		}
		UObject* LoadedObject = LoadObject<UObject>(nullptr, *ObjectPath);
		return IsValid(LoadedObject) ? LoadedObject : nullptr;
	}

	static bool DetachStaleObjectAtPath(const FString& ObjectPath, bool* bOutFoundStaleObject = nullptr)
	{
		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = false;
		}
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Existing)
		{
			return false;
		}
		if (IsValid(Existing) && Existing->HasAnyFlags(RF_Public | RF_Standalone))
		{
			return false;
		}

		if (bOutFoundStaleObject)
		{
			*bOutFoundStaleObject = true;
		}
		if (!IsValid(Existing))
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			return false;
		}

		Existing->ClearFlags(RF_Public | RF_Standalone);
		const ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_ForceNoResetLoaders;
		const FName TrashName = MakeUniqueObjectName(GetTransientPackage(), Existing->GetClass(), Existing->GetFName());
		const bool bRenamed = Existing->Rename(*TrashName.ToString(), GetTransientPackage(), RenameFlags);
		Existing->MarkAsGarbage();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return bRenamed;
	}

	static bool FindWidgetsByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		bool bFoundAny = false;
		if (RootWidget->GetType() == TypeName)
		{
			OutWidgets.Add(RootWidget);
			bFoundAny = true;
		}

		FChildren* Children = RootWidget->GetChildren();
		if (!Children)
		{
			return bFoundAny;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			bFoundAny |= FindWidgetsByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidgets);
		}
		return bFoundAny;
	}

	static TSharedPtr<SWidget> ChooseLargestWidget(const TArray<TSharedRef<SWidget>>& Widgets)
	{
		TSharedPtr<SWidget> BestWidget;
		float BestArea = -1.0f;
		for (const TSharedRef<SWidget>& Candidate : Widgets)
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

	static int32 CountNiagaraScriptOutputNodes(const UNiagaraScript* Script)
	{
		if (!Script)
		{
			return INDEX_NONE;
		}

		const UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		const UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (!Graph)
		{
			return INDEX_NONE;
		}

		int32 OutputNodeCount = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<UNiagaraNodeOutput>(Node))
			{
				++OutputNodeCount;
			}
		}
		return OutputNodeCount;
	}

	static bool CanSafelyOpenNiagaraSystemEditor(UNiagaraSystem& NiagaraSystem, FString& OutError)
	{
		const int32 SystemSpawnOutputCount = CountNiagaraScriptOutputNodes(NiagaraSystem.GetSystemSpawnScript());
		const int32 SystemUpdateOutputCount = CountNiagaraScriptOutputNodes(NiagaraSystem.GetSystemUpdateScript());
		if (SystemSpawnOutputCount <= 0 || SystemUpdateOutputCount <= 0)
		{
			OutError = FString::Printf(
				TEXT("unsafe_niagara_system_graph_missing_output_nodes:system_spawn=%d system_update=%d"),
				SystemSpawnOutputCount,
				SystemUpdateOutputCount);
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara editor open blocked: system=%s reason=%s"),
				*NiagaraSystem.GetPathName(),
				*OutError);
			return false;
		}
		return true;
	}

	static int32 CloseAssetEditorsForStructuralNiagaraEdit(UObject& Asset)
	{
		if (!GEditor)
		{
			return 0;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem || !AssetEditorSubsystem->FindEditorForAsset(&Asset, false))
		{
			return 0;
		}

		const int32 ClosedEditorCount = AssetEditorSubsystem->CloseAllEditorsForAsset(&Asset);
		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara structural edit closed open asset editors: asset=%s closed=%d"),
			*Asset.GetPathName(),
			ClosedEditorCount);
		return ClosedEditorCount;
	}

	static IAssetEditorInstance* GetNiagaraAssetEditor(UObject* Asset, const bool bOpenIfNeeded, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return nullptr;
		}
		if (!GEditor)
		{
			OutError = TEXT("editor_unavailable");
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			OutError = TEXT("asset_editor_subsystem_unavailable");
			return nullptr;
		}

		if (bOpenIfNeeded)
		{
			if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset))
			{
				if (!CanSafelyOpenNiagaraSystemEditor(*NiagaraSystem, OutError))
				{
					return nullptr;
				}
			}
			AssetEditorSubsystem->OpenEditorForAsset(Asset);
		}

		IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
		if (!AssetEditor)
		{
			OutError = bOpenIfNeeded ? TEXT("niagara_editor_open_failed") : TEXT("niagara_editor_not_opened");
			return nullptr;
		}

		return AssetEditor;
	}

	static TSharedPtr<SWindow> ResolveNiagaraEditorWindow(IAssetEditorInstance* AssetEditor, FString& OutError)
	{
		if (!AssetEditor)
		{
			OutError = TEXT("niagara_editor_not_opened");
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
		if (!TabManager.IsValid())
		{
			OutError = TEXT("niagara_tab_manager_not_found");
			return nullptr;
		}

		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = TEXT("niagara_owner_tab_not_found");
			return nullptr;
		}

		const TSharedPtr<SWindow> ParentWindow = OwnerTab->GetParentWindow();
		if (!ParentWindow.IsValid())
		{
			OutError = TEXT("niagara_window_not_found");
			return nullptr;
		}
		return ParentWindow;
	}

	static TSharedPtr<SWidget> ResolveNiagaraViewportWidget(const TSharedRef<SWindow>& Window, FString& OutError)
	{
		TArray<TSharedRef<SWidget>> CandidateWidgets;
		const FName CandidateTypes[] =
		{
			FName(TEXT("SNiagaraSystemViewport")),
			FName(TEXT("SNiagaraAssetBrowserPreview")),
			FName(TEXT("SNiagaraSimCacheViewport")),
			FName(TEXT("SEditorViewport"))
		};

		for (const FName& TypeName : CandidateTypes)
		{
			FindWidgetsByTypeRecursive(Window, TypeName, CandidateWidgets);
		}

		if (CandidateWidgets.Num() <= 0)
		{
			OutError = TEXT("niagara_viewport_widget_not_found");
			return nullptr;
		}

		return ChooseLargestWidget(CandidateWidgets);
	}

	static TSharedPtr<SWidget> ResolveNiagaraViewportSceneWidget(const TSharedRef<SWidget>& ViewportWidget)
	{
		TArray<TSharedRef<SWidget>> ViewportChildren;
		FindWidgetsByTypeRecursive(ViewportWidget, FName(TEXT("SViewport")), ViewportChildren);
		return ChooseLargestWidget(ViewportChildren);
	}

	static bool IsSlateWindowSafeForBackbufferCapture(const TSharedPtr<SWindow>& Window, FString& OutUnsafeReason)
	{
		OutUnsafeReason.Reset();
		if (!Window.IsValid())
		{
			OutUnsafeReason = TEXT("window_invalid");
			return false;
		}
		if (Window->IsWindowMinimized())
		{
			OutUnsafeReason = TEXT("window_minimized");
			return false;
		}
		if (!Window->IsVisible())
		{
			OutUnsafeReason = TEXT("window_not_visible");
			return false;
		}

		const FVector2D ClientSize = Window->GetClientSizeInScreen();
		if (ClientSize.X < 64.0 || ClientSize.Y < 64.0)
		{
			OutUnsafeReason = FString::Printf(TEXT("window_too_small:%.0fx%.0f"), ClientSize.X, ClientSize.Y);
			return false;
		}
		return true;
	}

	static bool PrepareSlateWindowForCapture(const TSharedPtr<SWindow>& Window, FString& OutRedrawSkipReason)
	{
		if (!Window.IsValid() || !FSlateApplication::IsInitialized())
		{
			OutRedrawSkipReason = TEXT("window_or_slate_invalid");
			return false;
		}

		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();

		FString UnsafeReason;
		if (!IsSlateWindowSafeForBackbufferCapture(Window, UnsafeReason))
		{
			OutRedrawSkipReason = UnsafeReason;
			return false;
		}

		FSlateApplication::Get().ForceRedrawWindow(Window.ToSharedRef());
		OutRedrawSkipReason.Reset();
		return true;
	}

	static bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}

		FIntVector ShotSize(0, 0, 0);
		if (!FSlateApplication::Get().TakeScreenshot(Widget, OutPixels, ShotSize))
		{
			OutError = TEXT("widget_screenshot_failed");
			return false;
		}
		if (ShotSize.X <= 0 || ShotSize.Y <= 0 || OutPixels.Num() <= 0)
		{
			OutError = TEXT("widget_screenshot_empty");
			return false;
		}

		OutSize = FIntPoint(ShotSize.X, ShotSize.Y);
		return true;
	}

	static FVector2D ResolveOffscreenDrawSize(const TSharedRef<SWidget>& Widget, const FString& Target, const TSharedPtr<FJsonObject>& Params, const int32 MaxSize)
	{
		const FVector2D CachedSize = Widget->GetCachedGeometry().GetLocalSize();
		if (Target == TEXT("viewport") && CachedSize.X >= 64.0 && CachedSize.Y >= 64.0)
		{
			return FVector2D(
				FMath::Clamp(FMath::RoundToDouble(CachedSize.X), 64.0, static_cast<double>(FMath::Clamp(MaxSize, 64, 8192))),
				FMath::Clamp(FMath::RoundToDouble(CachedSize.Y), 64.0, static_cast<double>(FMath::Clamp(MaxSize, 64, 8192))));
		}

		double RequestedWidth = 0.0;
		double RequestedHeight = 0.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("capture_width"), RequestedWidth);
			Params->TryGetNumberField(TEXT("capture_height"), RequestedHeight);
			if (RequestedWidth <= 0.0)
			{
				Params->TryGetNumberField(TEXT("offscreen_width"), RequestedWidth);
			}
			if (RequestedHeight <= 0.0)
			{
				Params->TryGetNumberField(TEXT("offscreen_height"), RequestedHeight);
			}
		}

		FVector2D DrawSize(RequestedWidth, RequestedHeight);
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			DrawSize = CachedSize;
		}
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			DrawSize = Widget->GetDesiredSize();
		}
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			DrawSize = Target == TEXT("viewport") ? FVector2D(1280.0, 720.0) : FVector2D(1440.0, 900.0);
		}

		const double MaxDimension = static_cast<double>(FMath::Clamp(MaxSize, 64, 8192));
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 64.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 64.0, MaxDimension);
		return DrawSize;
	}

	static bool ScreenshotSlateWidgetOffscreen(const TSharedRef<SWidget>& Widget, const FVector2D& DrawSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
			return false;
		}
		if (!FApp::CanEverRender())
		{
			OutError = TEXT("rendering_not_available");
			return false;
		}
		if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
		{
			OutError = TEXT("offscreen_draw_size_invalid");
			return false;
		}

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

	static FString ResolveOutputScreenshotPath(const TSharedPtr<FJsonObject>& Params, const FString& Format)
	{
		FString OutPath;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("file_path"), OutPath) || Params->TryGetStringField(TEXT("out_file_path"), OutPath);
		}
		OutPath.TrimStartAndEndInline();
		if (OutPath.IsEmpty())
		{
			return FUeAgentHttpServer::MakeShotPath(Format);
		}
		if (FPaths::IsRelative(OutPath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutPath);
		}
		return OutPath;
	}

	static void LinkGraphPins(UEdGraphPin* SourcePin, UEdGraphPin* TargetPin)
	{
		if (!SourcePin || !TargetPin)
		{
			return;
		}
		SourcePin->MakeLinkTo(TargetPin);
		if (UEdGraphNode* SourceNode = SourcePin->GetOwningNode())
		{
			SourceNode->PinConnectionListChanged(SourcePin);
		}
		if (UEdGraphNode* TargetNode = TargetPin->GetOwningNode())
		{
			TargetNode->PinConnectionListChanged(TargetPin);
		}
	}

	static UEdGraphPin* FindParameterMapPin(UNiagaraNode& Node, const EEdGraphPinDirection Direction)
	{
		const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
		if (!NiagaraSchema)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin &&
				Pin->Direction == Direction &&
				NiagaraSchema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UClass* GetNiagaraNodeEmitterClass()
	{
		return StaticLoadClass(
			UNiagaraNode::StaticClass(),
			nullptr,
			TEXT("/Script/NiagaraEditor.NiagaraNodeEmitter"));
	}

	static bool SetObjectPropertyByName(UObject& Object, const FName PropertyName, UObject* Value)
	{
		if (FObjectProperty* ObjectProperty = FindFProperty<FObjectProperty>(Object.GetClass(), PropertyName))
		{
			ObjectProperty->SetObjectPropertyValue_InContainer(&Object, Value);
			return true;
		}
		return false;
	}

	static bool SetGuidPropertyByName(UObject& Object, const FName PropertyName, const FGuid& Value)
	{
		FStructProperty* StructProperty = FindFProperty<FStructProperty>(Object.GetClass(), PropertyName);
		if (!StructProperty || StructProperty->Struct != TBaseStructure<FGuid>::Get())
		{
			return false;
		}

		if (FGuid* PropertyValue = StructProperty->ContainerPtrToValuePtr<FGuid>(&Object))
		{
			*PropertyValue = Value;
			return true;
		}
		return false;
	}

	static bool SetNiagaraScriptUsagePropertyByName(UObject& Object, const FName PropertyName, const ENiagaraScriptUsage Value)
	{
		FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
		if (!Property)
		{
			return false;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (void* PropertyValue = EnumProperty->ContainerPtrToValuePtr<void>(&Object))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(PropertyValue, static_cast<int64>(Value));
				return true;
			}
		}
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			ByteProperty->SetPropertyValue_InContainer(&Object, static_cast<uint8>(Value));
			return true;
		}
		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (void* PropertyValue = NumericProperty->ContainerPtrToValuePtr<void>(&Object))
			{
				NumericProperty->SetIntPropertyValue(PropertyValue, static_cast<int64>(Value));
				return true;
			}
		}
		return false;
	}

	static UNiagaraNodeOutput* FindSystemOutputNodeByUsage(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		Graph.GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (OutputNode && OutputNode->GetUsage() == Usage)
			{
				return OutputNode;
			}
		}
		return nullptr;
	}

	static bool RebuildSystemEmitterNodesForCompile(UNiagaraSystem& NiagaraSystem, int32& OutEmitterNodeCount)
	{
		OutEmitterNodeCount = 0;

		UNiagaraScript* SystemSpawnScript = NiagaraSystem.GetSystemSpawnScript();
		UNiagaraScriptSource* SystemScriptSource = SystemSpawnScript ? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* SystemGraph = SystemScriptSource ? SystemScriptSource->NodeGraph : nullptr;
		if (!SystemGraph)
		{
			return false;
		}
		UClass* NodeEmitterClass = GetNiagaraNodeEmitterClass();
		if (!NodeEmitterClass)
		{
			return false;
		}

		SystemGraph->Modify();

		TArray<UNiagaraNode*> ExistingNodes;
		SystemGraph->GetNodesOfClass<UNiagaraNode>(ExistingNodes);
		for (UNiagaraNode* ExistingEmitterNode : ExistingNodes)
		{
			if (!ExistingEmitterNode || !ExistingEmitterNode->IsA(NodeEmitterClass))
			{
				continue;
			}

			ExistingEmitterNode->Modify();
			UEdGraphPin* InputPin = FindParameterMapPin(*ExistingEmitterNode, EGPD_Input);
			UEdGraphPin* OutputPin = FindParameterMapPin(*ExistingEmitterNode, EGPD_Output);
			UEdGraphPin* PreviousPin = InputPin && InputPin->LinkedTo.Num() == 1 ? InputPin->LinkedTo[0] : nullptr;
			UEdGraphPin* NextPin = OutputPin && OutputPin->LinkedTo.Num() == 1 ? OutputPin->LinkedTo[0] : nullptr;
			ExistingEmitterNode->DestroyNode();
			if (PreviousPin && NextPin)
			{
				LinkGraphPins(PreviousPin, NextPin);
			}
		}

		TArray<UNiagaraNodeInput*> AllInputNodes;
		SystemGraph->GetNodesOfClass<UNiagaraNodeInput>(AllInputNodes);
		TArray<UNiagaraNodeInput*> ParameterMapInputNodes;
		for (UNiagaraNodeInput* InputNode : AllInputNodes)
		{
			if (InputNode && FindParameterMapPin(*InputNode, EGPD_Output))
			{
				ParameterMapInputNodes.Add(InputNode);
			}
		}

		FNiagaraVariable SharedInputVar(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		for (int32 StageIndex = 0; StageIndex < 2; ++StageIndex)
		{
			const ENiagaraScriptUsage SystemUsage = static_cast<ENiagaraScriptUsage>(static_cast<int32>(ENiagaraScriptUsage::SystemSpawnScript) + StageIndex);
			UNiagaraNodeOutput* OutputNode = FindSystemOutputNodeByUsage(*SystemGraph, SystemUsage);
			if (!OutputNode)
			{
				FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(*SystemGraph);
				OutputNode = OutputNodeCreator.CreateNode();
				OutputNode->SetUsage(SystemUsage);
				OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
				OutputNode->NodePosX = 0;
				OutputNode->NodePosY = StageIndex * 120;
				OutputNodeCreator.Finalize();
			}

			UEdGraphPin* OutputInputPin = FindParameterMapPin(*OutputNode, EGPD_Input);
			if (!OutputInputPin)
			{
				OutputNode->AllocateDefaultPins();
				OutputInputPin = FindParameterMapPin(*OutputNode, EGPD_Input);
			}
			if (!OutputInputPin)
			{
				continue;
			}

			UEdGraphPin* PreviousOutputPin = nullptr;
			if (OutputInputPin->LinkedTo.Num() == 1)
			{
				PreviousOutputPin = OutputInputPin->LinkedTo[0];
			}
			if (!PreviousOutputPin)
			{
				UNiagaraNodeInput* InputNode = ParameterMapInputNodes.IsValidIndex(StageIndex) ? ParameterMapInputNodes[StageIndex] : nullptr;

				if (!InputNode)
				{
					FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*SystemGraph);
					InputNode = InputNodeCreator.CreateNode();
					InputNode->Input = SharedInputVar;
					InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
					InputNode->NodePosX = -350;
					InputNode->NodePosY = StageIndex * 120;
					InputNodeCreator.Finalize();
					ParameterMapInputNodes.Add(InputNode);
				}

				PreviousOutputPin = FindParameterMapPin(*InputNode, EGPD_Output);
				LinkGraphPins(PreviousOutputPin, OutputInputPin);
			}

			for (const FNiagaraEmitterHandle& EmitterHandle : NiagaraSystem.GetEmitterHandles())
			{
				if (!EmitterHandle.GetInstance().Emitter)
				{
					continue;
				}

				UNiagaraNode* EmitterNode = NewObject<UNiagaraNode>(SystemGraph, NodeEmitterClass, NAME_None, RF_Transactional);
				if (!EmitterNode)
				{
					continue;
				}
				EmitterNode->CreateNewGuid();
				SetObjectPropertyByName(*EmitterNode, TEXT("OwnerSystem"), &NiagaraSystem);
				SetGuidPropertyByName(*EmitterNode, TEXT("EmitterHandleId"), EmitterHandle.GetId());
				SetNiagaraScriptUsagePropertyByName(
					*EmitterNode,
					TEXT("ScriptType"),
					static_cast<ENiagaraScriptUsage>(static_cast<int32>(ENiagaraScriptUsage::EmitterSpawnScript) + StageIndex));
				EmitterNode->SetEnabledState(EmitterHandle.GetIsEnabled() ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled, false);
				EmitterNode->NodePosX = -220 + (OutEmitterNodeCount * 20);
				EmitterNode->NodePosY = StageIndex * 120;
				EmitterNode->AllocateDefaultPins();
				SystemGraph->AddNode(EmitterNode, true, false);

				UEdGraphPin* EmitterInputPin = FindParameterMapPin(*EmitterNode, EGPD_Input);
				UEdGraphPin* EmitterOutputPin = FindParameterMapPin(*EmitterNode, EGPD_Output);
				if (!EmitterInputPin || !EmitterOutputPin || !PreviousOutputPin)
				{
					continue;
				}

				OutputInputPin->BreakAllPinLinks(true);
				LinkGraphPins(PreviousOutputPin, EmitterInputPin);
				LinkGraphPins(EmitterOutputPin, OutputInputPin);
				PreviousOutputPin = EmitterOutputPin;
				++OutEmitterNodeCount;
			}
		}

		SystemGraph->NotifyGraphChanged();
		return true;
	}

	static bool SynchronizeSystemEditorData(UNiagaraSystem& NiagaraSystem, int32& OutOverviewNodeCountBefore, int32& OutOverviewNodeCountAfter)
	{
		OutOverviewNodeCountBefore = INDEX_NONE;
		OutOverviewNodeCountAfter = INDEX_NONE;

		UNiagaraSystemEditorData* SystemEditorData = Cast<UNiagaraSystemEditorData>(NiagaraSystem.GetEditorData());
		if (!SystemEditorData)
		{
			return false;
		}

		UEdGraph* OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
		OutOverviewNodeCountBefore = OverviewGraph ? OverviewGraph->Nodes.Num() : 0;

		NiagaraSystem.Modify();
		SystemEditorData->Modify();
		if (OverviewGraph)
		{
			OverviewGraph->Modify();
		}

		SystemEditorData->SynchronizeOverviewGraphWithSystem(NiagaraSystem);
		NiagaraSystem.ComputeEmittersExecutionOrder();
		NiagaraSystem.InvalidateCachedData();

		OverviewGraph = SystemEditorData->GetSystemOverviewGraph();
		OutOverviewNodeCountAfter = OverviewGraph ? OverviewGraph->Nodes.Num() : 0;
		if (OutOverviewNodeCountAfter != OutOverviewNodeCountBefore)
		{
			NiagaraSystem.MarkPackageDirty();
		}
		return true;
	}

	struct FNiagaraSystemRefreshResult
	{
		bool bPerformed = false;
		bool bSynchronizedOverviewGraph = false;
		bool bRebuiltEmitterNodes = false;
		bool bComputedExecutionOrder = false;
		bool bInvalidatedCachedData = false;
		bool bBroadcastPostEditChange = false;
		bool bCreatedDataProcessingViewModel = false;
		bool bHadExistingSystemViewModel = false;
		bool bMarkedDirty = false;
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		int32 EmitterCount = 0;
	};

	static FNiagaraSystemRefreshResult RefreshNiagaraSystemAfterStructuralEdit(
		UNiagaraSystem& NiagaraSystem,
		const bool bBroadcastPostEditChange,
		const bool bMarkDirty)
	{
		FNiagaraSystemRefreshResult Result;
		Result.bPerformed = true;
		Result.EmitterCount = NiagaraSystem.GetEmitterHandles().Num();

		NiagaraSystem.Modify();
		int32 RebuiltEmitterNodeCount = 0;
		Result.bRebuiltEmitterNodes = RebuildSystemEmitterNodesForCompile(NiagaraSystem, RebuiltEmitterNodeCount);
		Result.bSynchronizedOverviewGraph = SynchronizeSystemEditorData(
			NiagaraSystem,
			Result.OverviewNodeCountBefore,
			Result.OverviewNodeCountAfter);
		Result.bComputedExecutionOrder = Result.bSynchronizedOverviewGraph;
		Result.bInvalidatedCachedData = Result.bSynchronizedOverviewGraph;

		if (bBroadcastPostEditChange)
		{
			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				const TSharedPtr<FNiagaraSystemViewModel> ExistingViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(&NiagaraSystem);
				Result.bHadExistingSystemViewModel = ExistingViewModel.IsValid();
				if (!ExistingViewModel.IsValid())
				{
					TSharedPtr<FNiagaraSystemViewModel> TemporaryViewModel = MakeShared<FNiagaraSystemViewModel>();
					FNiagaraSystemViewModelOptions Options;
					Options.bCanModifyEmittersFromTimeline = false;
					Options.bCanAutoCompile = false;
					Options.bCanSimulate = false;
					Options.bIsForDataProcessingOnly = true;
					Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
					Options.MessageLogGuid = NiagaraSystem.GetAssetGuid();
					TemporaryViewModel->Initialize(NiagaraSystem, Options);
					Result.bCreatedDataProcessingViewModel = TemporaryViewModel.IsValid();
				}
			}
			NiagaraSystem.OnSystemPostEditChange().Broadcast(&NiagaraSystem);
			Result.bBroadcastPostEditChange = true;
		}

		if (bMarkDirty)
		{
			NiagaraSystem.MarkPackageDirty();
			Result.bMarkedDirty = true;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("Niagara RefreshSystemAfterStructuralEdit: system=%s emitters=%d overview_before=%d overview_after=%d sync=%s post_edit=%s dirty=%s"),
			*NiagaraSystem.GetPathName(),
			Result.EmitterCount,
			Result.OverviewNodeCountBefore,
			Result.OverviewNodeCountAfter,
			Result.bSynchronizedOverviewGraph ? TEXT("true") : TEXT("false"),
			Result.bBroadcastPostEditChange ? TEXT("true") : TEXT("false"),
			Result.bMarkedDirty ? TEXT("true") : TEXT("false"));

		return Result;
	}

	static TSharedPtr<FJsonObject> BuildNiagaraSystemRefreshJson(const FNiagaraSystemRefreshResult& Result)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("performed"), Result.bPerformed);
		Obj->SetBoolField(TEXT("synchronized_overview_graph"), Result.bSynchronizedOverviewGraph);
		Obj->SetBoolField(TEXT("rebuilt_emitter_nodes"), Result.bRebuiltEmitterNodes);
		Obj->SetBoolField(TEXT("computed_execution_order"), Result.bComputedExecutionOrder);
		Obj->SetBoolField(TEXT("invalidated_cached_data"), Result.bInvalidatedCachedData);
		Obj->SetBoolField(TEXT("broadcast_post_edit_change"), Result.bBroadcastPostEditChange);
		Obj->SetBoolField(TEXT("had_existing_system_view_model"), Result.bHadExistingSystemViewModel);
		Obj->SetBoolField(TEXT("created_data_processing_view_model"), Result.bCreatedDataProcessingViewModel);
		Obj->SetBoolField(TEXT("marked_dirty"), Result.bMarkedDirty);
		Obj->SetNumberField(TEXT("overview_node_count_before"), Result.OverviewNodeCountBefore);
		Obj->SetNumberField(TEXT("overview_node_count_after"), Result.OverviewNodeCountAfter);
		Obj->SetNumberField(TEXT("emitter_count"), Result.EmitterCount);
		return Obj;
	}

	static void FlushNiagaraPreviewWorld(UNiagaraComponent* PreviewComponent)
	{
		if (!PreviewComponent)
		{
			return;
		}

		if (UWorld* PreviewWorld = PreviewComponent->GetWorld())
		{
			PreviewWorld->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(PreviewWorld))
			{
				WorldManager->FlushComputeAndDeferredQueues(false);
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildNiagaraScriptRuntimeStats(UNiagaraScript* Script, const ENiagaraSimTarget SimTarget)
	{
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetBoolField(TEXT("has_script"), Script != nullptr);
		if (!Script)
		{
			return Stats;
		}

		const bool bGpuScript = SimTarget == ENiagaraSimTarget::GPUComputeSim || Script->IsGPUScript();
		const FNiagaraVMExecutableData& ExecutableData = Script->GetVMExecutableData();
		Stats->SetStringField(TEXT("script_name"), Script->GetName());
		Stats->SetStringField(TEXT("script_path"), Script->GetPathName());
		Stats->SetStringField(TEXT("script_usage"), NiagaraEnumToString(Script->GetUsage()));
		Stats->SetStringField(TEXT("script_usage_id"), Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Stats->SetBoolField(TEXT("ready_to_run"), Script->IsReadyToRun(SimTarget));
		Stats->SetBoolField(TEXT("compile_succeeded"), Script->DidScriptCompilationSucceed(bGpuScript));
#if WITH_EDITORONLY_DATA
		Stats->SetStringField(TEXT("last_compile_status"), NiagaraEnumToString(Script->GetLastCompileStatus()));
		Stats->SetNumberField(TEXT("last_compile_event_count"), ExecutableData.LastCompileEvents.Num());
		Stats->SetNumberField(TEXT("attributes_written_count"), ExecutableData.AttributesWritten.Num());
#else
		Stats->SetStringField(TEXT("last_compile_status"), TEXT(""));
		Stats->SetNumberField(TEXT("last_compile_event_count"), 0);
		Stats->SetNumberField(TEXT("attributes_written_count"), 0);
#endif
		Stats->SetBoolField(TEXT("has_bytecode"), ExecutableData.HasByteCode());
		Stats->SetBoolField(TEXT("data_usage_reads_attribute_data"), ExecutableData.DataUsage.bReadsAttributeData);
#if WITH_EDITORONLY_DATA
		Stats->SetBoolField(TEXT("vm_reads_attribute_data"), ExecutableData.bReadsAttributeData);
#else
		Stats->SetBoolField(TEXT("vm_reads_attribute_data"), false);
#endif
		Stats->SetNumberField(TEXT("vm_attribute_count"), ExecutableData.Attributes.Num());
		Stats->SetNumberField(TEXT("data_interface_info_count"), ExecutableData.DataInterfaceInfo.Num());
		Stats->SetNumberField(TEXT("called_vm_external_function_count"), ExecutableData.CalledVMExternalFunctions.Num());
		Stats->SetNumberField(TEXT("read_dataset_count"), ExecutableData.ReadDataSets.Num());
		Stats->SetNumberField(TEXT("write_dataset_count"), ExecutableData.WriteDataSets.Num());
		Stats->SetNumberField(TEXT("stat_scope_count"), ExecutableData.StatScopes.Num());
		return Stats;
	}

	static TSharedPtr<FJsonObject> BuildScriptExecutionContextStats(const FNiagaraScriptExecutionContextBase& Context, const ENiagaraSimTarget SimTarget)
	{
		TSharedPtr<FJsonObject> Stats = BuildNiagaraScriptRuntimeStats(Context.Script, SimTarget);
		Stats->SetNumberField(TEXT("bound_data_interface_count"), Context.GetDataInterfaces().Num());
		Stats->SetNumberField(TEXT("function_table_count"), Context.FunctionTable.Num());
		Stats->SetNumberField(TEXT("user_ptr_count"), Context.UserPtrTable.Num());
		Stats->SetNumberField(TEXT("dataset_info_count"), Context.DataSetInfo.Num());
		Stats->SetNumberField(TEXT("dataset_meta_count"), Context.DataSetMetaTable.Num());
		Stats->SetBoolField(TEXT("has_vector_vm_state"), Context.VectorVMState != nullptr);
		Stats->SetBoolField(TEXT("has_interpolation_parameters"), Context.HasInterpolationParameters != 0);
		return Stats;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildSpawnInfoArray(const TArray<FNiagaraSpawnInfo>& SpawnInfos, int32& OutTotalSpawnCount)
	{
		TArray<TSharedPtr<FJsonValue>> SpawnInfoJson;
		OutTotalSpawnCount = 0;
		SpawnInfoJson.Reserve(SpawnInfos.Num());
		for (const FNiagaraSpawnInfo& SpawnInfo : SpawnInfos)
		{
			OutTotalSpawnCount += SpawnInfo.Count;

			TSharedPtr<FJsonObject> SpawnInfoObj = MakeShared<FJsonObject>();
			SpawnInfoObj->SetNumberField(TEXT("count"), SpawnInfo.Count);
			SpawnInfoObj->SetNumberField(TEXT("interp_start_dt"), SpawnInfo.InterpStartDt);
			SpawnInfoObj->SetNumberField(TEXT("interval_dt"), SpawnInfo.IntervalDt);
			SpawnInfoObj->SetNumberField(TEXT("spawn_group"), SpawnInfo.SpawnGroup);
			SpawnInfoJson.Add(MakeShared<FJsonValueObject>(SpawnInfoObj));
		}
		return SpawnInfoJson;
	}

	struct FNiagaraStackIssueCounts
	{
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		int32 InfoCount = 0;
		int32 NoneCount = 0;

		int32 GetTotal() const
		{
			return ErrorCount + WarningCount + InfoCount + NoneCount;
		}

		void Add(const EStackIssueSeverity Severity)
		{
			switch (Severity)
			{
			case EStackIssueSeverity::Error:
				++ErrorCount;
				break;
			case EStackIssueSeverity::Warning:
				++WarningCount;
				break;
			case EStackIssueSeverity::Info:
				++InfoCount;
				break;
			case EStackIssueSeverity::None:
			default:
				++NoneCount;
				break;
			}
		}

		void AppendToJson(TSharedPtr<FJsonObject> Obj, const FString& Prefix = FString()) const
		{
			if (!Obj.IsValid())
			{
				return;
			}
			Obj->SetNumberField(Prefix + TEXT("issue_count"), GetTotal());
			Obj->SetNumberField(Prefix + TEXT("error_count"), ErrorCount);
			Obj->SetNumberField(Prefix + TEXT("warning_count"), WarningCount);
			Obj->SetNumberField(Prefix + TEXT("info_count"), InfoCount);
			Obj->SetNumberField(Prefix + TEXT("none_count"), NoneCount);
		}
	};

	static FString StackIssueSeverityToString(const EStackIssueSeverity Severity)
	{
		switch (Severity)
		{
		case EStackIssueSeverity::Error:
			return TEXT("error");
		case EStackIssueSeverity::Warning:
			return TEXT("warning");
		case EStackIssueSeverity::Info:
			return TEXT("info");
		case EStackIssueSeverity::None:
		default:
			return TEXT("none");
		}
	}

	static FString StackIssueFixStyleToString(const UNiagaraStackEntry::EStackIssueFixStyle Style)
	{
		switch (Style)
		{
		case UNiagaraStackEntry::EStackIssueFixStyle::Fix:
			return TEXT("fix");
		case UNiagaraStackEntry::EStackIssueFixStyle::Link:
			return TEXT("link");
		default:
			return TEXT("unknown");
		}
	}

	static bool ShouldIncludeStackIssueSeverity(const EStackIssueSeverity Severity, const FString& SeverityFilter)
	{
		const FString NormalizedFilter = SeverityFilter.TrimStartAndEnd().ToLower();
		if (NormalizedFilter.IsEmpty() || NormalizedFilter == TEXT("warning_or_error") || NormalizedFilter == TEXT("warnings_or_errors"))
		{
			return Severity == EStackIssueSeverity::Error || Severity == EStackIssueSeverity::Warning;
		}
		if (NormalizedFilter == TEXT("all"))
		{
			return true;
		}
		if (NormalizedFilter == TEXT("error") || NormalizedFilter == TEXT("errors"))
		{
			return Severity == EStackIssueSeverity::Error;
		}
		if (NormalizedFilter == TEXT("warning") || NormalizedFilter == TEXT("warnings"))
		{
			return Severity == EStackIssueSeverity::Warning;
		}
		if (NormalizedFilter == TEXT("info"))
		{
			return Severity == EStackIssueSeverity::Info;
		}
		if (NormalizedFilter == TEXT("none"))
		{
			return Severity == EStackIssueSeverity::None;
		}
		return Severity == EStackIssueSeverity::Error || Severity == EStackIssueSeverity::Warning;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildStackIssueFixArray(const TArray<UNiagaraStackEntry::FStackIssueFix>& Fixes)
	{
		TArray<TSharedPtr<FJsonValue>> FixesJson;
		FixesJson.Reserve(Fixes.Num());
		for (const UNiagaraStackEntry::FStackIssueFix& Fix : Fixes)
		{
			TSharedPtr<FJsonObject> FixObj = MakeShared<FJsonObject>();
			FixObj->SetStringField(TEXT("description"), Fix.GetDescription().ToString());
			FixObj->SetStringField(TEXT("style"), StackIssueFixStyleToString(Fix.GetStyle()));
			FixObj->SetStringField(TEXT("unique_identifier"), Fix.GetUniqueIdentifier());
			FixObj->SetBoolField(TEXT("has_fix_delegate"), Fix.GetFixDelegate().IsBound());
			FixObj->SetBoolField(TEXT("is_valid"), Fix.IsValid());
			FixesJson.Add(MakeShared<FJsonValueObject>(FixObj));
		}
		return FixesJson;
	}

	static void AppendStackEntryModuleFields(UNiagaraStackEntry& Entry, TSharedPtr<FJsonObject> IssueObj)
	{
		if (!IssueObj.IsValid())
		{
			return;
		}

		UNiagaraStackModuleItem* ModuleItem = Cast<UNiagaraStackModuleItem>(&Entry);
		if (!ModuleItem)
		{
			IssueObj->SetBoolField(TEXT("is_module"), false);
			return;
		}

		IssueObj->SetBoolField(TEXT("is_module"), true);
		IssueObj->SetNumberField(TEXT("module_index"), ModuleItem->GetModuleIndex());
		IssueObj->SetBoolField(TEXT("module_enabled"), ModuleItem->GetIsEnabled());

		UNiagaraNodeFunctionCall& ModuleNode = ModuleItem->GetModuleNode();
		IssueObj->SetStringField(TEXT("module_node_guid"), ModuleNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		IssueObj->SetStringField(TEXT("module_name"), ModuleNode.GetFunctionName());
		IssueObj->SetStringField(TEXT("module_script_asset_path"), GetModuleScriptPath(ModuleNode));

		if (UNiagaraNodeOutput* OutputNode = ModuleItem->GetOutputNode())
		{
			const ENiagaraScriptUsage Usage = OutputNode->GetUsage();
			const FGuid UsageId = OutputNode->GetUsageId();
			IssueObj->SetStringField(TEXT("stage_key"), MakeStageKey(Usage, UsageId));
			IssueObj->SetStringField(TEXT("script_usage"), ScriptUsageToString(Usage));
			IssueObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			IssueObj->SetStringField(TEXT("output_node_guid"), OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		}
	}

	static TSharedPtr<FJsonObject> BuildStackIssueJson(
		UNiagaraStackEntry& Entry,
		const UNiagaraStackEntry::FStackIssue& Issue,
		const FString& ScopeType,
		const FString& ScopeName,
		const FGuid& EmitterHandleId,
		const FString& EntryPath)
	{
		TSharedPtr<FJsonObject> IssueObj = MakeShared<FJsonObject>();
		IssueObj->SetStringField(TEXT("scope_type"), ScopeType);
		IssueObj->SetStringField(TEXT("scope_name"), ScopeName);
		IssueObj->SetStringField(TEXT("emitter_handle_id"), EmitterHandleId.IsValid() ? EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
		IssueObj->SetStringField(TEXT("entry_path"), EntryPath);
		IssueObj->SetStringField(TEXT("entry_display_name"), Entry.GetDisplayName().ToString());
		IssueObj->SetStringField(TEXT("entry_class"), Entry.GetClass() ? Entry.GetClass()->GetName() : FString());
		IssueObj->SetStringField(TEXT("stack_editor_data_key"), Entry.GetStackEditorDataKey());
		IssueObj->SetStringField(TEXT("execution_category"), Entry.GetExecutionCategoryName().ToString());
		IssueObj->SetStringField(TEXT("execution_subcategory"), Entry.GetExecutionSubcategoryName().ToString());
		IssueObj->SetStringField(TEXT("severity"), StackIssueSeverityToString(Issue.GetSeverity()));
		IssueObj->SetStringField(TEXT("short_description"), Issue.GetShortDescription().ToString());
		IssueObj->SetStringField(TEXT("long_description"), Issue.GetLongDescription().ToString());
		IssueObj->SetStringField(TEXT("unique_identifier"), Issue.GetUniqueIdentifier());
		IssueObj->SetBoolField(TEXT("can_be_dismissed"), Issue.GetCanBeDismissed());
		IssueObj->SetBoolField(TEXT("expanded_by_default"), Issue.GetIsExpandedByDefault());
		IssueObj->SetArrayField(TEXT("fixes"), BuildStackIssueFixArray(Issue.GetFixes()));
		IssueObj->SetNumberField(TEXT("fix_count"), Issue.GetFixes().Num());
		AppendStackEntryModuleFields(Entry, IssueObj);
		return IssueObj;
	}

	static FString GetStackEntryPathSegment(UNiagaraStackEntry& Entry)
	{
		FString DisplayName = Entry.GetDisplayName().ToString();
		DisplayName.TrimStartAndEndInline();
		if (!DisplayName.IsEmpty())
		{
			return DisplayName;
		}
		return Entry.GetClass() ? Entry.GetClass()->GetName() : TEXT("StackEntry");
	}

	static FString BuildStackIssueEntryPath(UNiagaraStackViewModel* StackViewModel, UNiagaraStackEntry& Entry)
	{
		TArray<FString> EntryPathSegments;
		if (StackViewModel)
		{
			TArray<UNiagaraStackEntry*> EntryPath;
			StackViewModel->GetPathForEntry(&Entry, EntryPath);
			EntryPath.Add(&Entry);
			for (UNiagaraStackEntry* PathEntry : EntryPath)
			{
				if (!PathEntry)
				{
					continue;
				}
				const FString Segment = GetStackEntryPathSegment(*PathEntry);
				if (!Segment.IsEmpty())
				{
					EntryPathSegments.Add(Segment);
				}
			}
		}

		if (EntryPathSegments.Num() == 0)
		{
			EntryPathSegments.Add(GetStackEntryPathSegment(Entry));
		}
		return FString::Join(EntryPathSegments, TEXT(" / "));
	}

	static int32 CountNiagaraStackEntries(UNiagaraStackEntry* Entry)
	{
		if (!Entry)
		{
			return 0;
		}

		int32 Count = 1;
		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			Count += CountNiagaraStackEntries(Child);
		}
		return Count;
	}

	static UNiagaraStackEntry* FindNiagaraStackEntryForObject(UNiagaraStackEntry* Entry, const UObject* SourceObject)
	{
		if (!Entry || !SourceObject)
		{
			return nullptr;
		}

		if (Entry->GetDisplayedObject() == SourceObject)
		{
			return Entry;
		}
		if (const UNiagaraStackObject* StackObject = Cast<UNiagaraStackObject>(Entry))
		{
			if (StackObject->GetObject() == SourceObject)
			{
				return Entry;
			}
		}

		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			if (UNiagaraStackEntry* MatchingEntry = FindNiagaraStackEntryForObject(Child, SourceObject))
			{
				return MatchingEntry;
			}
		}
		return nullptr;
	}

	static EStackIssueSeverity TokenizedSeverityToStackIssueSeverity(const EMessageSeverity::Type MessageSeverity)
	{
		switch (MessageSeverity)
		{
		case EMessageSeverity::Error:
			return EStackIssueSeverity::Error;
		case EMessageSeverity::PerformanceWarning:
		case EMessageSeverity::Warning:
			return EStackIssueSeverity::Warning;
		case EMessageSeverity::Info:
			return EStackIssueSeverity::Info;
		default:
			return EStackIssueSeverity::Info;
		}
	}

	static UNiagaraStackEntry::FStackIssue BuildStackIssueFromNiagaraMessage(
		const TSharedRef<const INiagaraMessage>& Message,
		const FString& StackEditorDataKey)
	{
		FText ShortDescription = Message->GenerateMessageTitle();
		if (ShortDescription.IsEmptyOrWhitespace())
		{
			ShortDescription = FText::FromString(TEXT("Message"));
		}

		TArray<UNiagaraStackEntry::FStackIssueFix> FixLinks;
		TArray<FText> LinkMessages;
		TArray<FSimpleDelegate> LinkNavigateActions;
		Message->GenerateLinks(LinkMessages, LinkNavigateActions);
		for (int32 LinkIndex = 0; LinkIndex < LinkMessages.Num(); ++LinkIndex)
		{
			const FText& LinkMessage = LinkMessages[LinkIndex];
			const FSimpleDelegate& LinkNavigateAction = LinkNavigateActions.IsValidIndex(LinkIndex)
				? LinkNavigateActions[LinkIndex]
				: FSimpleDelegate();
			FixLinks.Add(UNiagaraStackEntry::FStackIssueFix(
				LinkMessage,
				UNiagaraStackEntry::FStackIssueFixDelegate::CreateLambda([LinkNavigateAction]()
				{
					if (LinkNavigateAction.IsBound())
					{
						LinkNavigateAction.Execute();
					}
				}),
				UNiagaraStackEntry::EStackIssueFixStyle::Link));
		}

		const EStackIssueSeverity StackIssueSeverity = TokenizedSeverityToStackIssueSeverity(Message->GenerateTokenizedMessage()->GetSeverity());
		return UNiagaraStackEntry::FStackIssue(
			StackIssueSeverity,
			ShortDescription,
			Message->GenerateMessageText(),
			StackEditorDataKey.IsEmpty() ? FString(TEXT("MessageStore")) : StackEditorDataKey,
			Message->GetDismissHandler().IsBound(),
			FixLinks,
			Message->GetDismissHandler());
	}

	static void CollectNiagaraStackIssuesFromIssueEntry(
		UNiagaraStackViewModel* StackViewModel,
		UNiagaraStackEntry* Entry,
		const FString& ScopeType,
		const FString& ScopeName,
		const FGuid& EmitterHandleId,
		const FString& SeverityFilter,
		TArray<TSharedPtr<FJsonValue>>& OutIssues,
		FNiagaraStackIssueCounts& OutAllCounts,
		FNiagaraStackIssueCounts& OutReturnedCounts,
		TSet<FString>* OutUniqueIssueIds = nullptr)
	{
		if (!Entry)
		{
			return;
		}

		const FString EntryPath = BuildStackIssueEntryPath(StackViewModel, *Entry);
		for (const UNiagaraStackEntry::FStackIssue& Issue : Entry->GetIssues())
		{
			const EStackIssueSeverity Severity = Issue.GetSeverity();
			OutAllCounts.Add(Severity);
			if (OutUniqueIssueIds)
			{
				OutUniqueIssueIds->Add(Issue.GetUniqueIdentifier());
			}
			if (ShouldIncludeStackIssueSeverity(Severity, SeverityFilter))
			{
				OutReturnedCounts.Add(Severity);
				OutIssues.Add(MakeShared<FJsonValueObject>(BuildStackIssueJson(*Entry, Issue, ScopeType, ScopeName, EmitterHandleId, EntryPath)));
			}
		}
	}

	static void CollectNiagaraMessageStoreIssues(
		UNiagaraStackViewModel* StackViewModel,
		UNiagaraStackEntry* RootEntry,
		const TArray<FNiagaraMessageSourceAndStore>& MessageStores,
		const FString& ScopeType,
		const FString& ScopeName,
		const FGuid& EmitterHandleId,
		const FString& SeverityFilter,
		TArray<TSharedPtr<FJsonValue>>& OutIssues,
		FNiagaraStackIssueCounts& OutAllCounts,
		FNiagaraStackIssueCounts& OutReturnedCounts,
		TSet<FString>& InOutUniqueIssueIds,
		int32& OutCandidateMessageCount,
		int32& OutAddedIssueCount,
		int32& OutDuplicateIssueCount,
		int32& OutDismissedMessageCount,
		int32& OutUnsupportedMessageCount)
	{
		OutCandidateMessageCount = 0;
		OutAddedIssueCount = 0;
		OutDuplicateIssueCount = 0;
		OutDismissedMessageCount = 0;
		OutUnsupportedMessageCount = 0;

		if (!RootEntry)
		{
			return;
		}

		for (const FNiagaraMessageSourceAndStore& MessageSourceAndStore : MessageStores)
		{
			FNiagaraMessageStore* MessageStore = MessageSourceAndStore.GetStore();
			UObject* MessageSource = MessageSourceAndStore.GetSource();
			if (!MessageStore || !MessageSource)
			{
				continue;
			}

			for (auto MessageIt = MessageStore->GetMessages().CreateConstIterator(); MessageIt; ++MessageIt)
			{
				++OutCandidateMessageCount;
				const FGuid& MessageKey = MessageIt.Key();
				if (MessageStore->IsMessageDismissed(MessageKey))
				{
					++OutDismissedMessageCount;
					continue;
				}

				const UNiagaraMessageData* MessageData = Cast<UNiagaraMessageData>(MessageIt.Value());
				if (!MessageData)
				{
					++OutUnsupportedMessageCount;
					continue;
				}

				FGenerateNiagaraMessageInfo GenerateInfo;
				GenerateInfo.SetAssociatedObjectKeys({ FObjectKey(MessageSource) });
				if (MessageData->GetAllowDismissal())
				{
					GenerateInfo.SetDismissHandler(FSimpleDelegate::CreateLambda([]() {}));
				}

				TSharedRef<const INiagaraMessage> Message = MessageData->GenerateNiagaraMessage(GenerateInfo);
				UNiagaraStackEntry* IssueEntry = FindNiagaraStackEntryForObject(RootEntry, MessageSource);
				if (!IssueEntry)
				{
					IssueEntry = RootEntry;
				}

				const FString StackEditorDataKey = IssueEntry->GetStackEditorDataKey();
				UNiagaraStackEntry::FStackIssue Issue = BuildStackIssueFromNiagaraMessage(Message, StackEditorDataKey);
				if (!Issue.IsValid())
				{
					++OutUnsupportedMessageCount;
					continue;
				}

				const FString UniqueIdentifier = Issue.GetUniqueIdentifier();
				if (InOutUniqueIssueIds.Contains(UniqueIdentifier))
				{
					++OutDuplicateIssueCount;
					continue;
				}
				InOutUniqueIssueIds.Add(UniqueIdentifier);

				const EStackIssueSeverity Severity = Issue.GetSeverity();
				OutAllCounts.Add(Severity);
				if (ShouldIncludeStackIssueSeverity(Severity, SeverityFilter))
				{
					OutReturnedCounts.Add(Severity);
					const FString EntryPath = BuildStackIssueEntryPath(StackViewModel, *IssueEntry);
					TSharedPtr<FJsonObject> IssueObj = BuildStackIssueJson(*IssueEntry, Issue, ScopeType, ScopeName, EmitterHandleId, EntryPath);
					IssueObj->SetStringField(TEXT("issue_origin"), TEXT("message_store"));
					IssueObj->SetStringField(TEXT("message_key"), MessageKey.ToString(EGuidFormats::DigitsWithHyphensLower));
					IssueObj->SetStringField(TEXT("message_topic"), Message->GetMessageTopic().ToString());
					IssueObj->SetStringField(TEXT("message_source_class"), MessageSource->GetClass() ? MessageSource->GetClass()->GetName() : FString());
					IssueObj->SetStringField(TEXT("message_source_name"), MessageSource->GetName());
					IssueObj->SetStringField(TEXT("message_source_path"), MessageSource->GetPathName());
					IssueObj->SetBoolField(TEXT("message_should_only_log"), Message->ShouldOnlyLog());
					OutIssues.Add(MakeShared<FJsonValueObject>(IssueObj));
				}
				++OutAddedIssueCount;
			}
		}
	}

	static void CollectNiagaraStackIssuesFromEntry(
		UNiagaraStackEntry* Entry,
		const FString& ScopeType,
		const FString& ScopeName,
		const FGuid& EmitterHandleId,
		const FString& SeverityFilter,
		TArray<FString>& EntryPathSegments,
		TArray<TSharedPtr<FJsonValue>>& OutIssues,
		FNiagaraStackIssueCounts& OutAllCounts,
		FNiagaraStackIssueCounts& OutReturnedCounts,
		int32& OutVisitedEntryCount)
	{
		if (!Entry)
		{
			return;
		}

		++OutVisitedEntryCount;
		EntryPathSegments.Add(GetStackEntryPathSegment(*Entry));
		const FString EntryPath = FString::Join(EntryPathSegments, TEXT(" / "));

		for (const UNiagaraStackEntry::FStackIssue& Issue : Entry->GetIssues())
		{
			const EStackIssueSeverity Severity = Issue.GetSeverity();
			OutAllCounts.Add(Severity);
			if (ShouldIncludeStackIssueSeverity(Severity, SeverityFilter))
			{
				OutReturnedCounts.Add(Severity);
				OutIssues.Add(MakeShared<FJsonValueObject>(BuildStackIssueJson(*Entry, Issue, ScopeType, ScopeName, EmitterHandleId, EntryPath)));
			}
		}

		TArray<UNiagaraStackEntry*> Children;
		Entry->GetUnfilteredChildren(Children);
		for (UNiagaraStackEntry* Child : Children)
		{
			CollectNiagaraStackIssuesFromEntry(Child, ScopeType, ScopeName, EmitterHandleId, SeverityFilter, EntryPathSegments, OutIssues, OutAllCounts, OutReturnedCounts, OutVisitedEntryCount);
		}

		EntryPathSegments.Pop(EAllowShrinking::No);
	}

	static FString FormatStackIssueCountSummary(const int32 Count, const TCHAR* Singular, const TCHAR* Plural)
	{
		if (Count <= 0)
		{
			return FString();
		}
		return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : Plural);
	}

	static FString BuildStackIssueTooltipSummary(const int32 ErrorCount, const int32 WarningCount, const int32 InfoCount)
	{
		TArray<FString> Parts;
		const FString ErrorPart = FormatStackIssueCountSummary(ErrorCount, TEXT("error"), TEXT("errors"));
		const FString WarningPart = FormatStackIssueCountSummary(WarningCount, TEXT("warning"), TEXT("warnings"));
		const FString InfoPart = FormatStackIssueCountSummary(InfoCount, TEXT("info"), TEXT("infos"));
		if (!ErrorPart.IsEmpty())
		{
			Parts.Add(ErrorPart);
		}
		if (!WarningPart.IsEmpty())
		{
			Parts.Add(WarningPart);
		}
		if (!InfoPart.IsEmpty())
		{
			Parts.Add(InfoPart);
		}
		if (Parts.Num() == 0)
		{
			return FString();
		}
		if (Parts.Num() == 1)
		{
			return Parts[0];
		}
		if (Parts.Num() == 2)
		{
			return FString::Printf(TEXT("%s and %s"), *Parts[0], *Parts[1]);
		}
		return FString::Printf(TEXT("%s, %s, and %s"), *Parts[0], *Parts[1], *Parts[2]);
	}

	static FString GetStackIssueIconKind(const int32 ErrorCount, const int32 WarningCount, const int32 InfoCount)
	{
		if (ErrorCount > 0)
		{
			return TEXT("error");
		}
		if (WarningCount > 0)
		{
			return TEXT("warning");
		}
		if (InfoCount > 0)
		{
			return TEXT("info");
		}
		return TEXT("none");
	}

	static TSharedPtr<FJsonObject> CollectNiagaraStackIssuesFromViewModel(
		UNiagaraStackViewModel* StackViewModel,
		const FString& ScopeType,
		const FString& ScopeName,
		const FGuid& EmitterHandleId,
		const FString& SeverityFilter,
		const TArray<FNiagaraMessageSourceAndStore>* MessageStores,
		TArray<TSharedPtr<FJsonValue>>& OutIssues,
		FNiagaraStackIssueCounts& OutAllCounts,
		FNiagaraStackIssueCounts& OutReturnedCounts)
	{
		TSharedPtr<FJsonObject> ScopeObj = MakeShared<FJsonObject>();
		ScopeObj->SetStringField(TEXT("scope_type"), ScopeType);
		ScopeObj->SetStringField(TEXT("scope_name"), ScopeName);
		ScopeObj->SetStringField(TEXT("emitter_handle_id"), EmitterHandleId.IsValid() ? EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
		ScopeObj->SetBoolField(TEXT("has_stack_view_model"), StackViewModel != nullptr);

		if (!StackViewModel)
		{
			return ScopeObj;
		}

		UNiagaraStackEntry* RootEntry = StackViewModel->GetRootEntry();
		ScopeObj->SetBoolField(TEXT("has_root_entry"), RootEntry != nullptr);
		if (!RootEntry)
		{
			return ScopeObj;
		}

		RootEntry->RefreshChildren();
		RootEntry->RefreshFilteredChildren();
		const int32 RootErrorCount = RootEntry->GetTotalNumberOfErrorIssues();
		const int32 RootWarningCount = RootEntry->GetTotalNumberOfWarningIssues();
		const int32 RootInfoCount = RootEntry->GetTotalNumberOfInfoIssues();
		ScopeObj->SetStringField(TEXT("root_entry_class"), RootEntry->GetClass() ? RootEntry->GetClass()->GetName() : FString());
		ScopeObj->SetStringField(TEXT("root_entry_display_name"), RootEntry->GetDisplayName().ToString());
		ScopeObj->SetBoolField(TEXT("root_has_issues"), RootEntry->HasIssuesOrAnyChildHasIssues());
		ScopeObj->SetNumberField(TEXT("root_total_error_count"), RootErrorCount);
		ScopeObj->SetNumberField(TEXT("root_total_warning_count"), RootWarningCount);
		ScopeObj->SetNumberField(TEXT("root_total_info_count"), RootInfoCount);

		FNiagaraStackIssueCounts ScopeAllCounts;
		FNiagaraStackIssueCounts ScopeReturnedCounts;
		TSet<FString> ScopeUniqueIssueIds;
		ScopeObj->SetStringField(TEXT("issue_collection_source"), TEXT("stack_issue_icon_entries"));
		ScopeObj->SetNumberField(TEXT("visited_entry_count"), CountNiagaraStackEntries(RootEntry));

		TArray<UNiagaraStackEntry*> EntriesToCheck;
		EntriesToCheck.Add(RootEntry);
		EntriesToCheck.Append(RootEntry->GetAllChildrenWithIssues());
		TSet<UNiagaraStackEntry*> SeenEntries;
		int32 IssueEntryCount = 0;
		for (UNiagaraStackEntry* EntryToCheck : EntriesToCheck)
		{
			if (!EntryToCheck || SeenEntries.Contains(EntryToCheck))
			{
				continue;
			}
			SeenEntries.Add(EntryToCheck);
			++IssueEntryCount;
			CollectNiagaraStackIssuesFromIssueEntry(StackViewModel, EntryToCheck, ScopeType, ScopeName, EmitterHandleId, SeverityFilter, OutIssues, ScopeAllCounts, ScopeReturnedCounts, &ScopeUniqueIssueIds);
		}
		ScopeObj->SetNumberField(TEXT("issue_entry_count"), IssueEntryCount);

		int32 MessageStoreCandidateCount = 0;
		int32 MessageStoreAddedIssueCount = 0;
		int32 MessageStoreDuplicateIssueCount = 0;
		int32 MessageStoreDismissedMessageCount = 0;
		int32 MessageStoreUnsupportedMessageCount = 0;
		if (MessageStores)
		{
			CollectNiagaraMessageStoreIssues(
				StackViewModel,
				RootEntry,
				*MessageStores,
				ScopeType,
				ScopeName,
				EmitterHandleId,
				SeverityFilter,
				OutIssues,
				ScopeAllCounts,
				ScopeReturnedCounts,
				ScopeUniqueIssueIds,
				MessageStoreCandidateCount,
				MessageStoreAddedIssueCount,
				MessageStoreDuplicateIssueCount,
				MessageStoreDismissedMessageCount,
				MessageStoreUnsupportedMessageCount);
		}
		ScopeObj->SetNumberField(TEXT("message_store_source_count"), MessageStores ? MessageStores->Num() : 0);
		ScopeObj->SetNumberField(TEXT("message_store_candidate_message_count"), MessageStoreCandidateCount);
		ScopeObj->SetNumberField(TEXT("message_store_added_issue_count"), MessageStoreAddedIssueCount);
		ScopeObj->SetNumberField(TEXT("message_store_duplicate_issue_count"), MessageStoreDuplicateIssueCount);
		ScopeObj->SetNumberField(TEXT("message_store_dismissed_message_count"), MessageStoreDismissedMessageCount);
		ScopeObj->SetNumberField(TEXT("message_store_unsupported_message_count"), MessageStoreUnsupportedMessageCount);
		ScopeObj->SetStringField(TEXT("stack_issue_icon_kind"), GetStackIssueIconKind(ScopeAllCounts.ErrorCount, ScopeAllCounts.WarningCount, ScopeAllCounts.InfoCount));
		ScopeObj->SetStringField(TEXT("stack_issue_tooltip_summary"), BuildStackIssueTooltipSummary(ScopeAllCounts.ErrorCount, ScopeAllCounts.WarningCount, ScopeAllCounts.InfoCount));
		ScopeAllCounts.AppendToJson(ScopeObj, TEXT("total_"));
		ScopeReturnedCounts.AppendToJson(ScopeObj, TEXT("returned_"));
		ScopeObj->SetBoolField(
			TEXT("root_collected_issue_count_mismatch"),
			ScopeAllCounts.ErrorCount != RootEntry->GetTotalNumberOfErrorIssues() ||
			ScopeAllCounts.WarningCount != RootEntry->GetTotalNumberOfWarningIssues() ||
			ScopeAllCounts.InfoCount != RootEntry->GetTotalNumberOfInfoIssues());

		OutAllCounts.ErrorCount += ScopeAllCounts.ErrorCount;
		OutAllCounts.WarningCount += ScopeAllCounts.WarningCount;
		OutAllCounts.InfoCount += ScopeAllCounts.InfoCount;
		OutAllCounts.NoneCount += ScopeAllCounts.NoneCount;
		OutReturnedCounts.ErrorCount += ScopeReturnedCounts.ErrorCount;
		OutReturnedCounts.WarningCount += ScopeReturnedCounts.WarningCount;
		OutReturnedCounts.InfoCount += ScopeReturnedCounts.InfoCount;
		OutReturnedCounts.NoneCount += ScopeReturnedCounts.NoneCount;
		return ScopeObj;
	}

	static ENiagaraScriptCompileStatus UnionNiagaraCompileStatus(const ENiagaraScriptCompileStatus StatusA, const ENiagaraScriptCompileStatus StatusB)
	{
		if (StatusA == StatusB)
		{
			return StatusA;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_Unknown || StatusB == ENiagaraScriptCompileStatus::NCS_Unknown)
		{
			return ENiagaraScriptCompileStatus::NCS_Unknown;
		}
		if (StatusA >= ENiagaraScriptCompileStatus::NCS_MAX || StatusB >= ENiagaraScriptCompileStatus::NCS_MAX)
		{
			return ENiagaraScriptCompileStatus::NCS_MAX;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_Dirty || StatusB == ENiagaraScriptCompileStatus::NCS_Dirty)
		{
			return ENiagaraScriptCompileStatus::NCS_Dirty;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_Error || StatusB == ENiagaraScriptCompileStatus::NCS_Error)
		{
			return ENiagaraScriptCompileStatus::NCS_Error;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings ||
			StatusB == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings ||
			StatusA == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings ||
			StatusB == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings)
		{
			return ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_BeingCreated || StatusB == ENiagaraScriptCompileStatus::NCS_BeingCreated)
		{
			return ENiagaraScriptCompileStatus::NCS_BeingCreated;
		}
		if (StatusA == ENiagaraScriptCompileStatus::NCS_UpToDate || StatusB == ENiagaraScriptCompileStatus::NCS_UpToDate)
		{
			return ENiagaraScriptCompileStatus::NCS_UpToDate;
		}
		return ENiagaraScriptCompileStatus::NCS_Unknown;
	}

	static ENiagaraScriptCompileStatus GetScriptCompileStatusFromAsset(
		const UNiagaraScript* Script,
		const bool bTreatUnsynchronizedAsDirty,
		const bool bTreatIndeterminateAsDirty)
	{
		if (!Script)
		{
			return ENiagaraScriptCompileStatus::NCS_Unknown;
		}
		if (bTreatUnsynchronizedAsDirty && !Script->AreScriptAndSourceSynchronized())
		{
			return ENiagaraScriptCompileStatus::NCS_Dirty;
		}

		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		if (bTreatIndeterminateAsDirty &&
			(Status == ENiagaraScriptCompileStatus::NCS_Unknown ||
				Status == ENiagaraScriptCompileStatus::NCS_Dirty ||
				Status == ENiagaraScriptCompileStatus::NCS_BeingCreated))
		{
			return ENiagaraScriptCompileStatus::NCS_Dirty;
		}
		return Status;
	}

	static ENiagaraScriptCompileStatus AggregateScriptCompileStatusFromAsset(
		const TArray<UNiagaraScript*>& Scripts,
		const bool bTreatUnsynchronizedAsDirty,
		const bool bTreatIndeterminateAsDirty)
	{
		if (Scripts.Num() == 0)
		{
			return ENiagaraScriptCompileStatus::NCS_Unknown;
		}

		ENiagaraScriptCompileStatus AggregateStatus = ENiagaraScriptCompileStatus::NCS_UpToDate;
		for (const UNiagaraScript* Script : Scripts)
		{
			AggregateStatus = UnionNiagaraCompileStatus(
				AggregateStatus,
				GetScriptCompileStatusFromAsset(Script, bTreatUnsynchronizedAsDirty, bTreatIndeterminateAsDirty));
		}
		return AggregateStatus;
	}

	static ENiagaraScriptCompileStatus GetSystemCompileStatusFromAsset(UNiagaraSystem& NiagaraSystem)
	{
		TArray<UNiagaraScript*> Scripts;
		Scripts.Add(NiagaraSystem.GetSystemSpawnScript());
		Scripts.Add(NiagaraSystem.GetSystemUpdateScript());

		for (const FNiagaraEmitterHandle& EmitterHandle : NiagaraSystem.GetEmitterHandles())
		{
			if (EmitterHandle.GetIsEnabled())
			{
				if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
				{
					EmitterData->GetScripts(Scripts, true);
				}
			}
		}
		return AggregateScriptCompileStatusFromAsset(Scripts, true, true);
	}

	static ENiagaraScriptCompileStatus GetEmitterHandleCompileStatusFromAsset(FNiagaraEmitterHandle* EmitterHandle)
	{
		if (!EmitterHandle)
		{
			return ENiagaraScriptCompileStatus::NCS_Unknown;
		}
		FVersionedNiagaraEmitterData* EmitterData = EmitterHandle->GetEmitterData();
		if (!EmitterData)
		{
			return ENiagaraScriptCompileStatus::NCS_Unknown;
		}

		TArray<UNiagaraScript*> Scripts;
		EmitterData->GetScripts(Scripts, true);
		ENiagaraScriptCompileStatus AggregateStatus = AggregateScriptCompileStatusFromAsset(Scripts, true, false);
		if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim && AggregateStatus != ENiagaraScriptCompileStatus::NCS_Dirty)
		{
			if (const UNiagaraScript* GPUScript = EmitterData->GetGPUComputeScript())
			{
				if (!GPUScript->AreScriptAndSourceSynchronized())
				{
					AggregateStatus = ENiagaraScriptCompileStatus::NCS_Dirty;
				}
			}
		}
		return AggregateStatus;
	}

	static FString EmitterHandleErrorTextFromCompileStatus(const ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Unknown:
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			return TEXT("Needs compilation & refresh.");
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			return TEXT("Compiled");
		default:
			return TEXT("Error! Needs compilation & refresh.");
		}
	}

	static FString EmitterHandleErrorColorFromCompileStatus(const ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Unknown:
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			return TEXT("yellow");
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			return TEXT("green");
		default:
			return TEXT("red");
		}
	}

	static void AppendNiagaraSystemCompileStatus(TSharedPtr<FJsonObject> ScopeObj, UNiagaraSystem& NiagaraSystem)
	{
		if (!ScopeObj.IsValid())
		{
			return;
		}

		const ENiagaraScriptCompileStatus LatestCompileStatus = GetSystemCompileStatusFromAsset(NiagaraSystem);
		ScopeObj->SetStringField(TEXT("system_compile_status_source"), TEXT("asset_script_status"));
		ScopeObj->SetStringField(TEXT("system_latest_compile_status"), ScriptCompileStatusToString(LatestCompileStatus));
		ScopeObj->SetBoolField(TEXT("system_compile_status_up_to_date"), LatestCompileStatus == ENiagaraScriptCompileStatus::NCS_UpToDate);
	}

	static void AppendNiagaraEmitterHandleStatus(
		TSharedPtr<FJsonObject> ScopeObj,
		const TSharedRef<FNiagaraEmitterHandleViewModel>& EmitterHandleViewModel,
		const int32 EmitterIndex)
	{
		if (!ScopeObj.IsValid())
		{
			return;
		}

		const ENiagaraScriptCompileStatus LatestCompileStatus = GetEmitterHandleCompileStatusFromAsset(EmitterHandleViewModel->GetEmitterHandle());
		const bool bErrorTextVisible = LatestCompileStatus != ENiagaraScriptCompileStatus::NCS_UpToDate;
		ScopeObj->SetNumberField(TEXT("emitter_index"), EmitterIndex);
		ScopeObj->SetStringField(TEXT("emitter_compile_status_source"), TEXT("asset_script_status"));
		ScopeObj->SetStringField(TEXT("emitter_latest_compile_status"), ScriptCompileStatusToString(LatestCompileStatus));
		ScopeObj->SetBoolField(TEXT("emitter_compile_status_up_to_date"), LatestCompileStatus == ENiagaraScriptCompileStatus::NCS_UpToDate);
		ScopeObj->SetStringField(TEXT("emitter_handle_error_text"), EmitterHandleErrorTextFromCompileStatus(LatestCompileStatus));
		ScopeObj->SetStringField(TEXT("emitter_handle_error_color"), EmitterHandleErrorColorFromCompileStatus(LatestCompileStatus));
		ScopeObj->SetStringField(TEXT("emitter_handle_error_visibility"), bErrorTextVisible ? TEXT("visible") : TEXT("collapsed"));
		ScopeObj->SetBoolField(TEXT("emitter_handle_error_visible"), bErrorTextVisible);
	}

	static TSharedPtr<FNiagaraSystemViewModel> CreateDataProcessingSystemViewModel(UNiagaraSystem& NiagaraSystem)
	{
		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = MakeShared<FNiagaraSystemViewModel>();
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCanAutoCompile = false;
		Options.bCanSimulate = false;
		Options.bIsForDataProcessingOnly = true;
		Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
		Options.MessageLogGuid = NiagaraSystem.GetAssetGuid();
		SystemViewModel->Initialize(NiagaraSystem, Options);
		return SystemViewModel;
	}

	static TSharedPtr<FNiagaraSystemViewModel> CreateDataProcessingEmitterViewModel(UNiagaraEmitter& NiagaraEmitter, UNiagaraSystem*& OutTransientSystem)
	{
		OutTransientSystem = NewObject<UNiagaraSystem>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
		if (!OutTransientSystem)
		{
			return nullptr;
		}

		UNiagaraSystemFactoryNew::InitializeSystem(OutTransientSystem, true);
		OutTransientSystem->EnsureFullyLoaded();
		NiagaraEmitter.UpdateEmitterAfterLoad();

		const FGuid VersionGuid = NiagaraEmitter.IsVersioningEnabled() && NiagaraEmitter.VersionToOpenInEditor.IsValid()
			? NiagaraEmitter.VersionToOpenInEditor
			: NiagaraEmitter.GetExposedVersion().VersionGuid;
		const FVersionedNiagaraEmitter VersionedEmitter(&NiagaraEmitter, VersionGuid);

		TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = MakeShared<FNiagaraSystemViewModel>();
		FNiagaraSystemViewModelOptions Options;
		Options.bCanModifyEmittersFromTimeline = false;
		Options.bCanAutoCompile = false;
		Options.bCanSimulate = false;
		Options.bIsForDataProcessingOnly = true;
		Options.EditMode = ENiagaraSystemViewModelEditMode::EmitterAsset;
		Options.MessageLogGuid = OutTransientSystem->GetAssetGuid();
		SystemViewModel->Initialize(*OutTransientSystem, Options);
		SystemViewModel->GetEditorData().SetOwningSystemIsPlaceholder(true, *OutTransientSystem);
		SystemViewModel->AddEmitter(VersionedEmitter);
		return SystemViewModel;
	}

	static bool PrepareNiagaraPreviewForCapture(UObject* Asset, const bool bResetPreview, const double AdvanceSeconds, const double TickDeltaSeconds, const FString& AdvanceMode, const bool bPauseAfterAdvance, FString& OutError)
	{
		OutError.Reset();

		UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		if (!NiagaraSystem)
		{
			OutError = TEXT("preview_prepare_only_supported_for_system_assets");
			return false;
		}

		if (NiagaraSystem->HasOutstandingCompilationRequests(true))
		{
			NiagaraSystem->WaitForCompilationComplete(true, false);
			NiagaraSystem->PollForCompilationComplete(true);
		}
		NiagaraSystem->CacheFromCompiledData();

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return false;
		}

		const TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return false;
		}

		UNiagaraComponent* PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return false;
		}

		const float SafeTickDelta = FMath::Clamp(static_cast<float>(TickDeltaSeconds), 1.0f / 240.0f, 1.0f / 5.0f);
		const FString NormalizedAdvanceMode = AdvanceMode.TrimStartAndEnd().ToLower();
		const bool bUseAdvanceSimulation =
			NormalizedAdvanceMode == TEXT("advance") ||
			NormalizedAdvanceMode == TEXT("advance_simulation") ||
			NormalizedAdvanceMode == TEXT("advance_simulation_by_time");
		const bool bUseTickDeltaAdvance =
			NormalizedAdvanceMode == TEXT("tick") ||
			NormalizedAdvanceMode == TEXT("tick_delta") ||
			NormalizedAdvanceMode == TEXT("tick_delta_time") ||
			NormalizedAdvanceMode == TEXT("simulation");
		auto ConfigurePreviewComponent = [SafeTickDelta, bUseTickDeltaAdvance, bUseAdvanceSimulation](UNiagaraComponent* Component)
		{
			if (!Component)
			{
				return;
			}
			Component->SetAgeUpdateMode((bUseTickDeltaAdvance || bUseAdvanceSimulation) ? ENiagaraAgeUpdateMode::TickDeltaTime : ENiagaraAgeUpdateMode::DesiredAge);
			Component->SetSeekDelta(SafeTickDelta);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->SetCanRenderWhileSeeking(true);
			Component->SetAllowScalability(false);
			Component->SetRenderingEnabled(true);
		};

		PreviewComponent->SetAsset(NiagaraSystem, false);
		PreviewComponent->SetForceSolo(true);
		ConfigurePreviewComponent(PreviewComponent);

		if (bResetPreview)
		{
			PreviewComponent->SetDesiredAge(0.0f);
			PreviewComponent->ReinitializeSystem();
			PreviewComponent = SystemViewModel->GetPreviewComponent();
			if (!PreviewComponent)
			{
				OutError = TEXT("niagara_preview_component_missing_after_reset");
				return false;
			}
			PreviewComponent->SetAsset(NiagaraSystem, false);
			PreviewComponent->SetForceSolo(true);
			ConfigurePreviewComponent(PreviewComponent);
		}

		PreviewComponent->SetPaused(false);
		PreviewComponent->Activate(true);

		if (bUseAdvanceSimulation)
		{
			if (AdvanceSeconds > 0.0)
			{
				PreviewComponent->AdvanceSimulationByTime(static_cast<float>(AdvanceSeconds), SafeTickDelta);
			}
			else
			{
				PreviewComponent->AdvanceSimulation(1, SafeTickDelta);
			}
			FlushNiagaraPreviewWorld(PreviewComponent);
		}
		else if (bUseTickDeltaAdvance)
		{
			const int32 TickCount = AdvanceSeconds > 0.0
				? FMath::Max(1, FMath::CeilToInt(static_cast<float>(AdvanceSeconds) / SafeTickDelta))
				: 1;
			for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
			{
				PreviewComponent->TickComponent(SafeTickDelta, ELevelTick::LEVELTICK_All, nullptr);
				FlushNiagaraPreviewWorld(PreviewComponent);
			}
		}
		else if (AdvanceSeconds > 0.0)
		{
			PreviewComponent->SeekToDesiredAge(static_cast<float>(AdvanceSeconds));
			PreviewComponent->TickComponent(SafeTickDelta, ELevelTick::LEVELTICK_All, nullptr);
			FlushNiagaraPreviewWorld(PreviewComponent);
		}
		else
		{
			PreviewComponent->SetDesiredAge(0.0f);
			PreviewComponent->TickComponent(SafeTickDelta, ELevelTick::LEVELTICK_All, nullptr);
			FlushNiagaraPreviewWorld(PreviewComponent);
		}

		if (bPauseAfterAdvance)
		{
			PreviewComponent->SetPaused(true);
		}
		PreviewComponent->MarkRenderStateDirty();
		return true;
	}

	static TSharedPtr<FJsonObject> CollectNiagaraComponentStats(UNiagaraSystem* NiagaraSystem, UNiagaraComponent* PreviewComponent, FString& OutError)
	{
		OutError.Reset();
		if (!NiagaraSystem)
		{
			OutError = TEXT("preview_stats_only_supported_for_system_assets");
			return nullptr;
		}
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return nullptr;
		}

		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetBoolField(TEXT("system_ready_to_run"), NiagaraSystem->IsReadyToRun());
		Stats->SetBoolField(TEXT("system_has_outstanding_compilation_requests"), NiagaraSystem->HasOutstandingCompilationRequests(true));
		Stats->SetObjectField(TEXT("system_spawn_script"), BuildNiagaraScriptRuntimeStats(NiagaraSystem->GetSystemSpawnScript(), ENiagaraSimTarget::CPUSim));
		Stats->SetObjectField(TEXT("system_update_script"), BuildNiagaraScriptRuntimeStats(NiagaraSystem->GetSystemUpdateScript(), ENiagaraSimTarget::CPUSim));
		Stats->SetBoolField(TEXT("component_registered"), PreviewComponent->IsRegistered());
		Stats->SetBoolField(TEXT("component_active"), PreviewComponent->IsActive());
		Stats->SetBoolField(TEXT("component_paused"), PreviewComponent->IsPaused());
		Stats->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
		Stats->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
		Stats->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
		Stats->SetBoolField(TEXT("component_has_attach_parent"), PreviewComponent->GetAttachParent() != nullptr);
		Stats->SetStringField(TEXT("component_requested_execution_state"), NiagaraEnumToString(PreviewComponent->GetRequestedExecutionState()));
		Stats->SetStringField(TEXT("component_execution_state"), NiagaraEnumToString(PreviewComponent->GetExecutionState()));
		Stats->SetBoolField(TEXT("component_complete"), PreviewComponent->IsComplete());
		Stats->SetStringField(TEXT("age_update_mode"), NiagaraEnumToString(PreviewComponent->GetAgeUpdateMode()));
		Stats->SetNumberField(TEXT("desired_age"), PreviewComponent->GetDesiredAge());

PRAGMA_DISABLE_DEPRECATION_WARNINGS
		FNiagaraSystemInstance* SystemInstance = PreviewComponent->GetSystemInstance();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!SystemInstance)
		{
			Stats->SetBoolField(TEXT("has_system_instance"), false);
			return Stats;
		}

		Stats->SetBoolField(TEXT("has_system_instance"), true);
		Stats->SetStringField(TEXT("system_instance_state"), NiagaraEnumToString(SystemInstance->SystemInstanceState));
		Stats->SetBoolField(TEXT("system_instance_pending_spawn"), SystemInstance->IsPendingSpawn());
		Stats->SetBoolField(TEXT("system_instance_disabled"), SystemInstance->IsDisabled());
		Stats->SetStringField(TEXT("system_attach_component"), SystemInstance->GetAttachComponent() ? SystemInstance->GetAttachComponent()->GetPathName() : FString());
		Stats->SetNumberField(TEXT("system_age"), SystemInstance->GetAge());
		Stats->SetNumberField(TEXT("system_tick_count"), SystemInstance->GetTickCount());
		Stats->SetBoolField(TEXT("system_instance_ready_to_run"), SystemInstance->IsReadyToRun());
		Stats->SetBoolField(TEXT("system_complete"), SystemInstance->IsComplete());
		Stats->SetStringField(TEXT("system_execution_state"), NiagaraEnumToString(SystemInstance->GetActualExecutionState()));
		Stats->SetStringField(TEXT("system_requested_execution_state"), NiagaraEnumToString(SystemInstance->GetRequestedExecutionState()));

		int32 TotalParticles = 0;
		int32 TotalSpawnedParticles = 0;
		TArray<TSharedPtr<FJsonValue>> EmitterStats;
		for (const FNiagaraEmitterInstanceRef& EmitterRef : SystemInstance->GetEmitters())
		{
			FNiagaraEmitterInstance& EmitterInstance = EmitterRef.Get();
			const int32 NumParticles = EmitterInstance.GetNumParticles();
			const int32 SpawnedParticles = EmitterInstance.GetTotalSpawnedParticles();
			TotalParticles += NumParticles;
			TotalSpawnedParticles += SpawnedParticles;

			TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
			EmitterObj->SetStringField(TEXT("name"), EmitterInstance.GetEmitterHandle().GetName().ToString());
			EmitterObj->SetStringField(TEXT("execution_state"), NiagaraEnumToString(EmitterInstance.GetExecutionState()));
			EmitterObj->SetBoolField(TEXT("active"), EmitterInstance.IsActive());
			EmitterObj->SetBoolField(TEXT("disabled"), EmitterInstance.IsDisabled());
			EmitterObj->SetBoolField(TEXT("inactive"), EmitterInstance.IsInactive());
			EmitterObj->SetBoolField(TEXT("complete"), EmitterInstance.IsComplete());
			EmitterObj->SetBoolField(TEXT("should_tick"), EmitterInstance.ShouldTick());
			EmitterObj->SetStringField(TEXT("sim_target"), NiagaraEnumToString(EmitterInstance.GetSimTarget()));
			EmitterObj->SetBoolField(TEXT("is_stateful"), EmitterInstance.AsStateful() != nullptr);
			EmitterObj->SetBoolField(TEXT("is_stateless"), EmitterInstance.AsStateless() != nullptr);
#if WITH_EDITORONLY_DATA
			EmitterObj->SetBoolField(TEXT("disabled_from_isolation"), EmitterInstance.IsDisabledFromIsolation());
#else
			EmitterObj->SetBoolField(TEXT("disabled_from_isolation"), false);
#endif
			EmitterObj->SetNumberField(TEXT("particle_count"), NumParticles);
			EmitterObj->SetNumberField(TEXT("total_spawned_particles"), SpawnedParticles);
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			EmitterObj->SetBoolField(TEXT("ready_to_run"), EmitterInstance.IsReadyToRun());
			EmitterObj->SetBoolField(TEXT("has_ticked"), EmitterInstance.HasTicked());
			if (EmitterInstance.AsStateful() != nullptr)
			{
				int32 SpawnInfoTotalCount = 0;
				TArray<TSharedPtr<FJsonValue>> SpawnInfoJson = BuildSpawnInfoArray(EmitterInstance.GetSpawnInfo(), SpawnInfoTotalCount);
				EmitterObj->SetNumberField(TEXT("spawn_info_count"), SpawnInfoJson.Num());
				EmitterObj->SetNumberField(TEXT("spawn_info_total_count"), SpawnInfoTotalCount);
				EmitterObj->SetArrayField(TEXT("spawn_infos"), SpawnInfoJson);
				EmitterObj->SetObjectField(TEXT("spawn_execution_context"), BuildScriptExecutionContextStats(EmitterInstance.GetSpawnExecutionContext(), EmitterInstance.GetSimTarget()));
				EmitterObj->SetObjectField(TEXT("update_execution_context"), BuildScriptExecutionContextStats(EmitterInstance.GetUpdateExecutionContext(), EmitterInstance.GetSimTarget()));
			}
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			EmitterStats.Add(MakeShared<FJsonValueObject>(EmitterObj));
		}

		Stats->SetNumberField(TEXT("emitter_instance_count"), EmitterStats.Num());
		Stats->SetNumberField(TEXT("total_particle_count"), TotalParticles);
		Stats->SetNumberField(TEXT("total_spawned_particles"), TotalSpawnedParticles);
		Stats->SetArrayField(TEXT("emitters"), EmitterStats);
		return Stats;
	}

	static TSharedPtr<FJsonObject> CollectNiagaraPreviewStats(UObject* Asset, FString& OutError)
	{
		OutError.Reset();

		UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Asset);
		if (!NiagaraSystem)
		{
			OutError = TEXT("preview_stats_only_supported_for_system_assets");
			return nullptr;
		}

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return nullptr;
		}

		const TSharedPtr<FNiagaraSystemViewModel> SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!SystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return nullptr;
		}

		UNiagaraComponent* PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return nullptr;
		}

		return CollectNiagaraComponentStats(NiagaraSystem, PreviewComponent, OutError);
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}

		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return false;
	}

	static bool ResolveStructPropertyPath(UStruct* RootStruct, void* RootContainer, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!RootStruct || !RootContainer || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = RootStruct;
		void* CurrentContainer = RootContainer;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}

			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}

		return false;
	}

	static bool SetStructPropertyText(UStruct* RootStruct, void* RootContainer, UObject* OwnerForImport, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveStructPropertyPath(RootStruct, RootContainer, PropertyPath, Property, ValuePtr))
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		if (OutCppType)
		{
			*OutCppType = Property ? Property->GetCPPType() : TEXT("");
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				FString GuidText = ValueText.TrimStartAndEnd();
				if (GuidText.StartsWith(TEXT("\"")) && GuidText.EndsWith(TEXT("\"")) && GuidText.Len() >= 2)
				{
					GuidText = GuidText.Mid(1, GuidText.Len() - 2);
				}

				FGuid ParsedGuid;
				if (FGuid::Parse(GuidText, ParsedGuid))
				{
					*reinterpret_cast<FGuid*>(ValuePtr) = ParsedGuid;
					if (OutAppliedValueText)
					{
						Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, OwnerForImport, PPF_None);
					}
					return true;
				}
			}
		}

		if (!Property || Property->ImportText_Direct(*ValueText, ValuePtr, OwnerForImport, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, OwnerForImport, PPF_None);
		}
		return true;
	}

	static bool GetStructPropertyText(UStruct* RootStruct, void* RootContainer, UObject* OwnerForExport, const FString& PropertyPath, FString& OutValueText, FString& OutCppType)
	{
		OutValueText.Reset();
		OutCppType.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveStructPropertyPath(RootStruct, RootContainer, PropertyPath, Property, ValuePtr))
		{
			return false;
		}

		if (!Property)
		{
			return false;
		}

		OutCppType = Property->GetCPPType();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, OwnerForExport, PPF_None);
		return true;
	}

	static FString NormalizeEventHandlerPropertyName(const FString& InPropertyName)
	{
		const FString Property = InPropertyName.TrimStartAndEnd();
		if (Property.IsEmpty())
		{
			return Property;
		}

		if (Property.Equals(TEXT("source_event_name"), ESearchCase::IgnoreCase)) return TEXT("SourceEventName");
		if (Property.Equals(TEXT("source_emitter_id"), ESearchCase::IgnoreCase)) return TEXT("SourceEmitterID");
		if (Property.Equals(TEXT("execution_mode"), ESearchCase::IgnoreCase)) return TEXT("ExecutionMode");
		if (Property.Equals(TEXT("spawn_number"), ESearchCase::IgnoreCase)) return TEXT("SpawnNumber");
		if (Property.Equals(TEXT("min_spawn_number"), ESearchCase::IgnoreCase)) return TEXT("MinSpawnNumber");
		if (Property.Equals(TEXT("random_spawn_number"), ESearchCase::IgnoreCase)) return TEXT("bRandomSpawnNumber");
		if (Property.Equals(TEXT("max_events_per_frame"), ESearchCase::IgnoreCase)) return TEXT("MaxEventsPerFrame");
		if (Property.Equals(TEXT("update_attribute_initial_values"), ESearchCase::IgnoreCase)) return TEXT("bUpdateAttributeInitialValues");
		return Property;
	}

	struct FEmitterPropertySideEffectResult
	{
		bool bHandled = false;
		bool bUpdatedSpawnScriptUsage = false;
		bool bMarkedGraphSourceUnsynchronized = false;
		ENiagaraScriptUsage OldSpawnScriptUsage = ENiagaraScriptUsage::ParticleSpawnScript;
		ENiagaraScriptUsage NewSpawnScriptUsage = ENiagaraScriptUsage::ParticleSpawnScript;
	};

	static FEmitterPropertySideEffectResult ApplyEmitterPropertySideEffects(FVersionedNiagaraEmitterData* EmitterData, const FString& PropertyName)
	{
		FEmitterPropertySideEffectResult Result;
		if (!EmitterData)
		{
			return Result;
		}

		if (PropertyName.Equals(TEXT("InterpolatedSpawnMode"), ESearchCase::IgnoreCase))
		{
			Result.bHandled = true;
			const ENiagaraScriptUsage DesiredUsage = EmitterData->UsesInterpolatedSpawning()
				? ENiagaraScriptUsage::ParticleSpawnScriptInterpolated
				: ENiagaraScriptUsage::ParticleSpawnScript;

			if (UNiagaraScript* SpawnScript = EmitterData->SpawnScriptProps.Script)
			{
				Result.OldSpawnScriptUsage = SpawnScript->GetUsage();
				Result.NewSpawnScriptUsage = DesiredUsage;
				if (Result.OldSpawnScriptUsage != DesiredUsage)
				{
					SpawnScript->Modify();
					SpawnScript->SetUsage(DesiredUsage);
					Result.bUpdatedSpawnScriptUsage = true;
				}
			}

			if (EmitterData->GraphSource)
			{
				EmitterData->GraphSource->MarkNotSynchronized(TEXT("Emitter interpolated spawn changed"));
				Result.bMarkedGraphSourceUnsynchronized = true;
			}
		}

		return Result;
	}

	static bool SetObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText = nullptr, FString* OutCppType = nullptr, FString* OutImportError = nullptr)
	{
		if (OutImportError)
		{
			OutImportError->Reset();
		}

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr))
		{
			if (OutImportError)
			{
				*OutImportError = TEXT("property_not_found");
			}
			return false;
		}

		Target->Modify();
		if (OutCppType)
		{
			*OutCppType = Property ? Property->GetCPPType() : TEXT("");
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FGuid>::Get())
			{
				FString GuidText = ValueText.TrimStartAndEnd();
				if (GuidText.StartsWith(TEXT("\"")) && GuidText.EndsWith(TEXT("\"")) && GuidText.Len() >= 2)
				{
					GuidText = GuidText.Mid(1, GuidText.Len() - 2);
				}

				FGuid ParsedGuid;
				if (FGuid::Parse(GuidText, ParsedGuid))
				{
					*reinterpret_cast<FGuid*>(ValuePtr) = ParsedGuid;
					if (OutAppliedValueText)
					{
						Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
					}
					return true;
				}
			}
		}
		if (!Property || Property->ImportText_Direct(*ValueText, ValuePtr, Target, PPF_None, GLog) == nullptr)
		{
			if (OutImportError)
			{
				*OutImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyPath, *ValueText);
			}
			return false;
		}
		if (OutAppliedValueText)
		{
			Property->ExportTextItem_Direct(*OutAppliedValueText, ValuePtr, nullptr, Target, PPF_None);
		}
		return true;
	}

	static bool GetObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText)
	{
		OutValueText.Reset();

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Target, PropertyPath, Property, ValuePtr))
		{
			return false;
		}

		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Target, PPF_None);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildPropertyValueJson(UObject& Target, FProperty& Property, void* ValuePtr)
	{
		TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
		PropertyObj->SetStringField(TEXT("property_name"), Property.GetName());
		PropertyObj->SetStringField(TEXT("authored_name"), Property.GetAuthoredName());
		PropertyObj->SetStringField(TEXT("cpp_type"), Property.GetCPPType());

		FString ValueText;
		Property.ExportTextItem_Direct(ValueText, ValuePtr, nullptr, &Target, PPF_None);
		PropertyObj->SetStringField(TEXT("value_text"), ValueText);
		return PropertyObj;
	}

	static FString StripUserNamespace(const FString& InName)
	{
		if (InName.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			return InName.Mid(5);
		}
		return InName;
	}

	static FString GetVariableNamespace(const FString& InName)
	{
		int32 DotIndex = INDEX_NONE;
		if (InName.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			return InName.Left(DotIndex);
		}
		return FString();
	}

	static FString GetVariableLeafName(const FString& InName)
	{
		int32 DotIndex = INDEX_NONE;
		if (InName.FindLastChar(TEXT('.'), DotIndex) && DotIndex >= 0 && DotIndex + 1 < InName.Len())
		{
			return InName.Mid(DotIndex + 1);
		}
		return InName;
	}

	static bool ParseBoolText(const FString& InText, bool& OutValue)
	{
		const FString Normalized = InText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("1") || Normalized == TEXT("true") || Normalized == TEXT("yes") || Normalized == TEXT("on"))
		{
			OutValue = true;
			return true;
		}
		if (Normalized == TEXT("0") || Normalized == TEXT("false") || Normalized == TEXT("no") || Normalized == TEXT("off"))
		{
			OutValue = false;
			return true;
		}
		return false;
	}

	static bool ParseFloatComponents(const FString& InText, const int32 ExpectedNum, TArray<float>& OutValues)
	{
		OutValues.Reset();
		if (ExpectedNum <= 0)
		{
			return false;
		}

		FString Clean = InText;
		Clean.ReplaceInline(TEXT("("), TEXT(" "));
		Clean.ReplaceInline(TEXT(")"), TEXT(" "));
		Clean.ReplaceInline(TEXT("{"), TEXT(" "));
		Clean.ReplaceInline(TEXT("}"), TEXT(" "));
		Clean.ReplaceInline(TEXT("["), TEXT(" "));
		Clean.ReplaceInline(TEXT("]"), TEXT(" "));
		Clean.ReplaceInline(TEXT(","), TEXT(" "));
		Clean.ReplaceInline(TEXT(";"), TEXT(" "));

		const TCHAR* PrefixTokens[] =
		{
			TEXT("X="), TEXT("Y="), TEXT("Z="), TEXT("W="),
			TEXT("R="), TEXT("G="), TEXT("B="), TEXT("A="),
			TEXT("x="), TEXT("y="), TEXT("z="), TEXT("w="),
			TEXT("r="), TEXT("g="), TEXT("b="), TEXT("a=")
		};
		for (const TCHAR* Token : PrefixTokens)
		{
			Clean.ReplaceInline(Token, TEXT(" "));
		}

		TArray<FString> Parts;
		Clean.ParseIntoArrayWS(Parts);
		if (Parts.Num() != ExpectedNum)
		{
			return false;
		}

		OutValues.Reserve(ExpectedNum);
		for (const FString& Part : Parts)
		{
			float Value = 0.0f;
			if (!LexTryParseString(Value, *Part) || !FMath::IsFinite(Value))
			{
				return false;
			}
			OutValues.Add(Value);
		}
		return true;
	}

	static bool TryResolveNiagaraEnumValueFromText(const UEnum& EnumType, const FString& InValueText, int64& OutEnumValue)
	{
		auto IsUserSettableEnumValue = [&EnumType](int64 EnumValue) -> bool
		{
			if (!EnumType.IsValidEnumValue(EnumValue))
			{
				return false;
			}

			const int32 EnumIndex = EnumType.GetIndexByValue(EnumValue);
			if (EnumIndex == INDEX_NONE)
			{
				return false;
			}
			if (EnumType.HasMetaData(TEXT("Hidden"), EnumIndex) ||
				EnumType.HasMetaData(TEXT("HiddenByDefault"), EnumIndex) ||
				EnumType.HasMetaData(TEXT("Spacer"), EnumIndex))
			{
				return false;
			}

			const FString EnumName = EnumType.GetNameStringByValue(EnumValue);
			const FString EnumNameUpper = EnumName.ToUpper();
			if (EnumNameUpper.Equals(TEXT("MAX")) ||
				EnumNameUpper.EndsWith(TEXT("_MAX")) ||
				EnumNameUpper.Contains(TEXT("INVALID")))
			{
				return false;
			}

			const FString FullEnumName = EnumType.GetNameByValue(EnumValue).ToString().ToUpper();
			if (FullEnumName.EndsWith(TEXT("_MAX")))
			{
				return false;
			}
			return true;
		};

		FString ValueText = InValueText.TrimStartAndEnd();
		if (ValueText.IsEmpty())
		{
			return false;
		}

		if (ValueText.StartsWith(TEXT("\"")) && ValueText.EndsWith(TEXT("\"")) && ValueText.Len() > 1)
		{
			ValueText = ValueText.Mid(1, ValueText.Len() - 2);
		}

		int64 ParsedIntValue = 0;
		if (LexTryParseString(ParsedIntValue, *ValueText))
		{
			if (IsUserSettableEnumValue(ParsedIntValue))
			{
				OutEnumValue = ParsedIntValue;
				return true;
			}
		}

		int64 ParsedEnumValue = EnumType.GetValueByNameString(ValueText, EGetByNameFlags::CheckAuthoredName);
		if (ParsedEnumValue == INDEX_NONE)
		{
			ParsedEnumValue = EnumType.GetValueByNameString(ValueText, EGetByNameFlags::None);
		}
		if (ParsedEnumValue == INDEX_NONE && ValueText.StartsWith(TEXT("NewEnumerator"), ESearchCase::IgnoreCase))
		{
			const FString EnumIndexText = ValueText.Mid(13).TrimStartAndEnd();
			int32 EnumIndex = INDEX_NONE;
			if (LexTryParseString(EnumIndex, *EnumIndexText))
			{
				if (EnumIndex >= 0 && EnumIndex < EnumType.NumEnums())
				{
					ParsedEnumValue = EnumType.GetValueByIndex(EnumIndex);
				}
				else if (EnumType.IsValidEnumValue(EnumIndex))
				{
					ParsedEnumValue = EnumIndex;
				}
			}
		}
		if (ParsedEnumValue == INDEX_NONE)
		{
			int32 ScopePos = INDEX_NONE;
			if (ValueText.FindLastChar(TEXT(':'), ScopePos) && ScopePos + 1 < ValueText.Len())
			{
				const FString ShortName = ValueText.Mid(ScopePos + 1).TrimStartAndEnd();
				if (!ShortName.IsEmpty())
				{
					ParsedEnumValue = EnumType.GetValueByNameString(ShortName, EGetByNameFlags::CheckAuthoredName);
					if (ParsedEnumValue == INDEX_NONE)
					{
						ParsedEnumValue = EnumType.GetValueByNameString(ShortName, EGetByNameFlags::None);
					}
				}
			}
		}
		if (ParsedEnumValue == INDEX_NONE || !EnumType.IsValidEnumValue(ParsedEnumValue))
		{
			return false;
		}
		if (!IsUserSettableEnumValue(ParsedEnumValue))
		{
			return false;
		}

		OutEnumValue = ParsedEnumValue;
		return true;
	}

	static bool ResolveNiagaraTypeDefinition(const FString& InTypeText, FNiagaraTypeDefinition& OutTypeDefinition)
	{
		FString TypeText = InTypeText;
		TypeText.TrimStartAndEndInline();
		if (TypeText.IsEmpty())
		{
			return false;
		}

		FString TypeToken = TypeText;
		TypeToken.ReplaceInline(TEXT(" "), TEXT(""));
		TypeToken.ReplaceInline(TEXT("_"), TEXT(""));
		TypeToken.ReplaceInline(TEXT("-"), TEXT(""));
		TypeToken.ToLowerInline();

		if (TypeToken == TEXT("bool") || TypeToken == TEXT("布尔") || TypeToken.EndsWith(TEXT("niagarabool")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (TypeToken == TEXT("int") || TypeToken == TEXT("int32") || TypeToken == TEXT("整数") || TypeToken.EndsWith(TEXT("niagaraint")) || TypeToken.EndsWith(TEXT("niagaraint32")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (TypeToken == TEXT("float") || TypeToken == TEXT("float32") || TypeToken == TEXT("浮点") || TypeToken.EndsWith(TEXT("niagarafloat")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (TypeToken == TEXT("double") || TypeToken == TEXT("float64") || TypeToken.EndsWith(TEXT("niagaradouble")))
		{
			OutTypeDefinition = FNiagaraTypeHelper::GetDoubleDef();
			return true;
		}
		if (TypeToken == TEXT("vec2") || TypeToken == TEXT("vector2") || TypeToken == TEXT("vector2d") || TypeToken == TEXT("fvector2f") || TypeToken.EndsWith(TEXT("niagaravector2")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec2Def();
			return true;
		}
		if (TypeToken == TEXT("vec3") || TypeToken == TEXT("vector") || TypeToken == TEXT("vector3") || TypeToken == TEXT("fvector3f") || TypeToken == TEXT("fvector") || TypeToken == TEXT("向量") || TypeToken == TEXT("矢量") || TypeToken.EndsWith(TEXT("niagaravector3")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec3Def();
			return true;
		}
		if (TypeToken == TEXT("position") || TypeToken == TEXT("niagaraposition"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}
		if (TypeToken == TEXT("vec4") || TypeToken == TEXT("vector4") || TypeToken == TEXT("fvector4f") || TypeToken.EndsWith(TEXT("niagaravector4")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetVec4Def();
			return true;
		}
		if (TypeToken == TEXT("quat") || TypeToken == TEXT("quaternion") || TypeToken == TEXT("fquat") || TypeToken == TEXT("fquat4f"))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetQuatDef();
			return true;
		}
		if (TypeToken == TEXT("color") || TypeToken == TEXT("linearcolor") || TypeToken == TEXT("flinearcolor") || TypeToken == TEXT("颜色") || TypeToken.EndsWith(TEXT("niagaracolor")))
		{
			OutTypeDefinition = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}

		if (UClass* TypeClass = ResolveClassByPath(TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeClass);
			return true;
		}
		if (UScriptStruct* TypeStruct = FindObject<UScriptStruct>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeStruct);
			return true;
		}
		if (UScriptStruct* LoadedTypeStruct = LoadObject<UScriptStruct>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(LoadedTypeStruct);
			return true;
		}
		if (UEnum* TypeEnum = FindObject<UEnum>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(TypeEnum);
			return true;
		}
		if (UEnum* LoadedTypeEnum = LoadObject<UEnum>(nullptr, *TypeText))
		{
			OutTypeDefinition = FNiagaraTypeDefinition(LoadedTypeEnum);
			return true;
		}

		return false;
	}

	static bool BuildNiagaraValueBytesFromText(const FNiagaraTypeDefinition& TypeDefinition, const FString& ValueText, TArray<uint8>& OutBytes, FString& OutError)
	{
		OutBytes.Reset();
		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			bool bValue = false;
			if (!ParseBoolText(ValueText, bValue))
			{
				OutError = TEXT("invalid_bool_value_text");
				return false;
			}
			FNiagaraBool NiagaraBool;
			NiagaraBool.SetValue(bValue);
			OutBytes.SetNumUninitialized(sizeof(FNiagaraBool));
			FMemory::Memcpy(OutBytes.GetData(), &NiagaraBool, sizeof(FNiagaraBool));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 IntValue = 0;
			if (!LexTryParseString(IntValue, *ValueText))
			{
				OutError = TEXT("invalid_int_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &IntValue, sizeof(int32));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef())
		{
			float FloatValue = 0.0f;
			if (!LexTryParseString(FloatValue, *ValueText))
			{
				OutError = TEXT("invalid_float_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(float));
			FMemory::Memcpy(OutBytes.GetData(), &FloatValue, sizeof(float));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			double DoubleValue = 0.0;
			if (!LexTryParseString(DoubleValue, *ValueText))
			{
				OutError = TEXT("invalid_double_value_text");
				return false;
			}
			OutBytes.SetNumUninitialized(sizeof(double));
			FMemory::Memcpy(OutBytes.GetData(), &DoubleValue, sizeof(double));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 2, Components))
			{
				OutError = TEXT("invalid_vector2_value_text");
				return false;
			}
			const FVector2f VecValue(Components[0], Components[1]);
			OutBytes.SetNumUninitialized(sizeof(FVector2f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector2f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TEXT("invalid_vector3_value_text");
				return false;
			}
			const FVector3f VecValue(Components[0], Components[1], Components[2]);
			OutBytes.SetNumUninitialized(sizeof(FVector3f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector3f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TEXT("invalid_position_value_text");
				return false;
			}
			const FNiagaraPosition PositionValue(Components[0], Components[1], Components[2]);
			OutBytes.SetNumUninitialized(sizeof(FNiagaraPosition));
			FMemory::Memcpy(OutBytes.GetData(), &PositionValue, sizeof(FNiagaraPosition));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_vector4_value_text");
				return false;
			}
			const FVector4f VecValue(Components[0], Components[1], Components[2], Components[3]);
			OutBytes.SetNumUninitialized(sizeof(FVector4f));
			FMemory::Memcpy(OutBytes.GetData(), &VecValue, sizeof(FVector4f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_quat_value_text");
				return false;
			}
			const FQuat4f QuatValue(Components[0], Components[1], Components[2], Components[3]);
			OutBytes.SetNumUninitialized(sizeof(FQuat4f));
			FMemory::Memcpy(OutBytes.GetData(), &QuatValue, sizeof(FQuat4f));
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor ColorValue;
			const FString Trimmed = ValueText.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("#")))
			{
				ColorValue = FLinearColor(FColor::FromHex(Trimmed));
			}
			else
			{
				TArray<float> Components;
				if (ParseFloatComponents(Trimmed, 3, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], 1.0f);
				}
				else if (ParseFloatComponents(Trimmed, 4, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
				}
				else if (!ColorValue.InitFromString(Trimmed))
				{
					OutError = TEXT("invalid_color_value_text");
					return false;
				}
			}

			OutBytes.SetNumUninitialized(sizeof(FLinearColor));
			FMemory::Memcpy(OutBytes.GetData(), &ColorValue, sizeof(FLinearColor));
			return true;
		}
		if (TypeDefinition.IsEnum())
		{
			UEnum* EnumType = TypeDefinition.GetEnum();
			if (!EnumType)
			{
				OutError = TEXT("enum_type_not_found");
				return false;
			}

			int64 EnumValue = 0;
			if (!TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
			{
				OutError = TEXT("invalid_enum_value_text");
				return false;
			}

			const int32 EnumIntValue = static_cast<int32>(EnumValue);
			OutBytes.SetNumUninitialized(sizeof(int32));
			FMemory::Memcpy(OutBytes.GetData(), &EnumIntValue, sizeof(int32));
			return true;
		}

		OutError = TEXT("unsupported_parameter_type_for_value_text");
		return false;
	}

	static FString FormatNiagaraFloatDefaultValue(const float Value)
	{
		return FString::SanitizeFloat(Value);
	}

	static FString FormatNiagaraVectorDefaultValue(const TArray<float>& Components, const TArray<const TCHAR*>& ComponentNames)
	{
		TArray<FString> Parts;
		const int32 ComponentCount = FMath::Min(Components.Num(), ComponentNames.Num());
		Parts.Reserve(ComponentCount);
		for (int32 Index = 0; Index < ComponentCount; ++Index)
		{
			Parts.Add(FString::Printf(
				TEXT("%s=%s"),
				ComponentNames[Index],
				*FormatNiagaraFloatDefaultValue(Components[Index])));
		}
		return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
	}

	static bool NormalizeNiagaraValueTextForPinDefault(const FNiagaraTypeDefinition& TypeDefinition, const FString& ValueText, FString& OutValueText, FString& OutError)
	{
		OutValueText = ValueText;
		OutError.Empty();

		if (TypeDefinition == FNiagaraTypeDefinition::GetBoolDef())
		{
			bool bValue = false;
			if (!ParseBoolText(ValueText, bValue))
			{
				OutError = TEXT("invalid_bool_value_text");
				return false;
			}
			OutValueText = bValue ? TEXT("true") : TEXT("false");
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetIntDef())
		{
			int32 IntValue = 0;
			if (!LexTryParseString(IntValue, *ValueText))
			{
				OutError = TEXT("invalid_int_value_text");
				return false;
			}
			OutValueText = LexToString(IntValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetFloatDef())
		{
			float FloatValue = 0.0f;
			if (!LexTryParseString(FloatValue, *ValueText) || !FMath::IsFinite(FloatValue))
			{
				OutError = TEXT("invalid_float_value_text");
				return false;
			}
			OutValueText = FormatNiagaraFloatDefaultValue(FloatValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeHelper::GetDoubleDef())
		{
			double DoubleValue = 0.0;
			if (!LexTryParseString(DoubleValue, *ValueText) || !FMath::IsFinite(DoubleValue))
			{
				OutError = TEXT("invalid_double_value_text");
				return false;
			}
			OutValueText = FString::SanitizeFloat(DoubleValue);
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 2, Components))
			{
				OutError = TEXT("invalid_vector2_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() || TypeDefinition == FNiagaraTypeDefinition::GetPositionDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 3, Components))
			{
				OutError = TypeDefinition == FNiagaraTypeDefinition::GetPositionDef() ? TEXT("invalid_position_value_text") : TEXT("invalid_vector3_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_vector4_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetQuatDef())
		{
			TArray<float> Components;
			if (!ParseFloatComponents(ValueText, 4, Components))
			{
				OutError = TEXT("invalid_quat_value_text");
				return false;
			}
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") });
			return true;
		}
		if (TypeDefinition == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor ColorValue;
			const FString Trimmed = ValueText.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("#")))
			{
				ColorValue = FLinearColor(FColor::FromHex(Trimmed));
			}
			else
			{
				TArray<float> Components;
				if (ParseFloatComponents(Trimmed, 4, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], Components[3]);
				}
				else if (ParseFloatComponents(Trimmed, 3, Components))
				{
					ColorValue = FLinearColor(Components[0], Components[1], Components[2], 1.0f);
				}
				else if (!ColorValue.InitFromString(Trimmed))
				{
					OutError = TEXT("invalid_color_value_text");
					return false;
				}
			}

			if (!FMath::IsFinite(ColorValue.R) || !FMath::IsFinite(ColorValue.G) || !FMath::IsFinite(ColorValue.B) || !FMath::IsFinite(ColorValue.A))
			{
				OutError = TEXT("invalid_color_value_text");
				return false;
			}
			TArray<float> Components = { ColorValue.R, ColorValue.G, ColorValue.B, ColorValue.A };
			OutValueText = FormatNiagaraVectorDefaultValue(Components, { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
			return true;
		}
		if (TypeDefinition.IsEnum())
		{
			UEnum* EnumType = TypeDefinition.GetEnum();
			if (!EnumType)
			{
				OutError = TEXT("enum_type_not_found");
				return false;
			}

			int64 EnumValue = 0;
			if (!TryResolveNiagaraEnumValueFromText(*EnumType, ValueText, EnumValue))
			{
				OutError = TEXT("invalid_enum_value_text");
				return false;
			}

			OutValueText = EnumType->GetNameStringByValue(EnumValue);
			if (OutValueText.IsEmpty())
			{
				OutValueText = LexToString(static_cast<int32>(EnumValue));
			}
			return true;
		}

		return true;
	}

	static bool IsNiagaraStrictStructPinDefaultType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		return TypeDefinition == FNiagaraTypeDefinition::GetVec2Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetVec3Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetVec4Def() ||
			TypeDefinition == FNiagaraTypeDefinition::GetPositionDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetQuatDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetColorDef();
	}

	static bool IsNiagaraValidatedPinDefaultType(const FNiagaraTypeDefinition& TypeDefinition)
	{
		return TypeDefinition == FNiagaraTypeDefinition::GetBoolDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetIntDef() ||
			TypeDefinition == FNiagaraTypeDefinition::GetFloatDef() ||
			TypeDefinition == FNiagaraTypeHelper::GetDoubleDef() ||
			IsNiagaraStrictStructPinDefaultType(TypeDefinition) ||
			TypeDefinition.IsEnum();
	}

	static bool ValidateNiagaraPinDefaultValueAfterSet(
		const FNiagaraTypeDefinition& TypeDefinition,
		const FString& AppliedValueText,
		const FString& StoredDefaultValue,
		FString& OutStatus,
		FString& OutError)
	{
		OutStatus.Empty();
		OutError.Empty();

		if (!IsNiagaraValidatedPinDefaultType(TypeDefinition))
		{
			OutStatus = TEXT("not_validated_unsupported_type");
			return true;
		}

		const FString AppliedTrimmed = AppliedValueText.TrimStartAndEnd();
		const FString StoredTrimmed = StoredDefaultValue.TrimStartAndEnd();
		if (StoredTrimmed.Equals(AppliedTrimmed, ESearchCase::CaseSensitive))
		{
			OutStatus = TEXT("stored_default_exact_match");
			return true;
		}

		FString NormalizedStoredValue;
		FString NormalizeStoredError;
		if (!NormalizeNiagaraValueTextForPinDefault(TypeDefinition, StoredTrimmed, NormalizedStoredValue, NormalizeStoredError))
		{
			OutStatus = TEXT("stored_default_parse_failed");
			OutError = NormalizeStoredError;
			return false;
		}

		// UE Niagara struct defaults are safest when the stored text is the native import
		// form, e.g. (X=...,Y=...,Z=...) or (R=...,G=...,B=...,A=...).
		// A bare vector like "0 0 1" can normalize semantically here but still appear
		// as a zero vector or black color in the Niagara stack UI.
		if (IsNiagaraStrictStructPinDefaultType(TypeDefinition))
		{
			OutStatus = TEXT("stored_struct_default_text_mismatch");
			OutError = FString::Printf(
				TEXT("stored_default_value_does_not_match_applied_import_text; stored=%s; applied=%s; normalized_stored=%s"),
				*StoredTrimmed,
				*AppliedTrimmed,
				*NormalizedStoredValue);
			return false;
		}

		if (NormalizedStoredValue.Equals(AppliedTrimmed, ESearchCase::CaseSensitive))
		{
			OutStatus = TEXT("stored_default_semantic_match");
			return true;
		}

		OutStatus = TEXT("stored_default_value_mismatch");
		OutError = FString::Printf(
			TEXT("stored_default_value_does_not_match_applied_value; stored=%s; applied=%s; normalized_stored=%s"),
			*StoredTrimmed,
			*AppliedTrimmed,
			*NormalizedStoredValue);
		return false;
	}

	static bool FindUserParameterByName(const FNiagaraUserRedirectionParameterStore& Store, const FString& InParameterName, FNiagaraVariable& OutParameter)
	{
		const FString SearchName = InParameterName.TrimStartAndEnd();
		if (SearchName.IsEmpty())
		{
			return false;
		}

		const FString SearchNameWithoutUser = StripUserNamespace(SearchName);
		TArray<FNiagaraVariable> Parameters;
		Store.GetUserParameters(Parameters);
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			const FString FullName = Parameter.GetName().ToString();
			const FString ShortName = StripUserNamespace(FullName);
			if (FullName.Equals(SearchName, ESearchCase::IgnoreCase) ||
				ShortName.Equals(SearchName, ESearchCase::IgnoreCase) ||
				ShortName.Equals(SearchNameWithoutUser, ESearchCase::IgnoreCase))
			{
				OutParameter = Parameter;
				return true;
			}
		}
		return false;
	}

	static bool SetUserParameterValueFromText(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, const FString& ValueText, FString& OutError)
	{
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();
		if (TypeDefinition.IsDataInterface())
		{
			UObject* ObjectValue = nullptr;
			const FString ObjectPath = ValueText.TrimStartAndEnd();
			if (!ObjectPath.IsEmpty() && !ObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectValue = LoadAssetObject(ObjectPath);
				if (!ObjectValue)
				{
					OutError = TEXT("data_interface_object_not_found");
					return false;
				}
			}

			UNiagaraDataInterface* DataInterface = Cast<UNiagaraDataInterface>(ObjectValue);
			if (ObjectValue && !DataInterface)
			{
				OutError = TEXT("object_is_not_data_interface");
				return false;
			}
			Store.SetDataInterface(DataInterface, Parameter);
			return true;
		}

		if (TypeDefinition.IsUObject())
		{
			UObject* ObjectValue = nullptr;
			const FString ObjectPath = ValueText.TrimStartAndEnd();
			if (!ObjectPath.IsEmpty() && !ObjectPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectValue = LoadAssetObject(ObjectPath);
				if (!ObjectValue)
				{
					OutError = TEXT("uobject_parameter_value_not_found");
					return false;
				}
			}

			if (ObjectValue)
			{
				if (UClass* ExpectedClass = TypeDefinition.GetClass())
				{
					if (!ObjectValue->IsA(ExpectedClass))
					{
						OutError = TEXT("uobject_parameter_value_type_mismatch");
						return false;
					}
				}
			}

			Store.SetUObject(ObjectValue, Parameter);
			return true;
		}

		TArray<uint8> ValueBytes;
		if (!BuildNiagaraValueBytesFromText(TypeDefinition, ValueText, ValueBytes, OutError))
		{
			return false;
		}
		if (!Store.SetParameterData(ValueBytes.GetData(), Parameter, false))
		{
			OutError = TEXT("set_parameter_data_failed");
			return false;
		}
		return true;
	}

	static bool GetUserParameterValueAsText(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, FString& OutValueText, FString& OutObjectPath)
	{
		OutValueText.Reset();
		OutObjectPath.Reset();
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		if (TypeDefinition.IsDataInterface())
		{
			if (UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Parameter))
			{
				OutObjectPath = DataInterface->GetPathName();
				return true;
			}
			return false;
		}
		if (TypeDefinition.IsUObject())
		{
			if (UObject* ObjectValue = Store.GetUObject(Parameter))
			{
				OutObjectPath = ObjectValue->GetPathName();
				return true;
			}
			return false;
		}

		const uint8* DataPtr = Store.GetParameterData(Parameter);
		if (!DataPtr)
		{
			return false;
		}

		OutValueText = TypeDefinition.ToString(DataPtr);
		return true;
	}

	static TSharedPtr<FJsonObject> BuildUserParameterJson(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter, const bool bIncludeValue)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		const FString FullName = Parameter.GetName().ToString();
		const FString ShortName = StripUserNamespace(FullName);
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		ParameterObj->SetStringField(TEXT("name"), FullName);
		ParameterObj->SetStringField(TEXT("short_name"), ShortName);
		ParameterObj->SetStringField(TEXT("namespace"), GetVariableNamespace(FullName));
		ParameterObj->SetStringField(TEXT("leaf_name"), GetVariableLeafName(FullName));
		ParameterObj->SetStringField(TEXT("type"), TypeDefinition.GetNameText().ToString());
		ParameterObj->SetStringField(TEXT("type_internal"), TypeDefinition.GetName());
		ParameterObj->SetStringField(TEXT("type_path"), TypeDefinition.GetStruct() ? TypeDefinition.GetStruct()->GetPathName() : TEXT(""));

		if (bIncludeValue)
		{
			FString ValueText;
			FString ObjectPath;
			const bool bHasValue = GetUserParameterValueAsText(Store, Parameter, ValueText, ObjectPath);
			ParameterObj->SetBoolField(TEXT("has_value"), bHasValue);
			ParameterObj->SetStringField(TEXT("value_text"), ValueText);
			ParameterObj->SetStringField(TEXT("value_object_path"), ObjectPath);
		}
		return ParameterObj;
	}

	static bool FindGraphParameterByName(UNiagaraGraph& Graph, const FString& InParameterName, FNiagaraVariable& OutParameter, UNiagaraScriptVariable*& OutScriptVariable)
	{
		OutScriptVariable = nullptr;
		const FString SearchName = InParameterName.TrimStartAndEnd();
		if (SearchName.IsEmpty())
		{
			return false;
		}

		const UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();

		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
		{
			if (Pair.Key.GetName().ToString().Equals(SearchName, ESearchCase::IgnoreCase))
			{
				OutParameter = Pair.Key;
				OutScriptVariable = Pair.Value;
				return true;
			}
		}

		const FString SearchLeaf = GetVariableLeafName(SearchName);
		FNiagaraVariable Candidate;
		UNiagaraScriptVariable* CandidateVar = nullptr;
		int32 CandidateCount = 0;
		for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : ScriptVariableMap)
		{
			const FString Leaf = GetVariableLeafName(Pair.Key.GetName().ToString());
			if (Leaf.Equals(SearchLeaf, ESearchCase::IgnoreCase))
			{
				++CandidateCount;
				Candidate = Pair.Key;
				CandidateVar = Pair.Value;
			}
		}

		if (CandidateCount == 1 && CandidateVar)
		{
			OutParameter = Candidate;
			OutScriptVariable = CandidateVar;
			return true;
		}

		return false;
	}

	static UNiagaraScriptVariable* AddGraphParameterMetadata(UNiagaraGraph& Graph, const FNiagaraVariable& Parameter, const bool bIsStaticSwitch)
	{
		UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();
		if (TObjectPtr<UNiagaraScriptVariable>* ExistingScriptVariable = ScriptVariableMap.Find(Parameter))
		{
			return ExistingScriptVariable->Get();
		}

		UNiagaraScriptVariable* NewScriptVariable = NewObject<UNiagaraScriptVariable>(&Graph, NAME_None, RF_Transactional);
		if (!NewScriptVariable)
		{
			return nullptr;
		}

		NewScriptVariable->Init(Parameter, FNiagaraVariableMetaData());
		NewScriptVariable->SetIsStaticSwitch(bIsStaticSwitch);
		NewScriptVariable->DefaultMode = ENiagaraDefaultMode::FailIfPreviouslyNotSet;

		const int32 ValueSizeBytes = NewScriptVariable->Variable.GetSizeInBytes();
		if (ValueSizeBytes > 0)
		{
			TArray<uint8> ZeroValueBytes;
			ZeroValueBytes.SetNumZeroed(ValueSizeBytes);
			NewScriptVariable->SetDefaultValueData(ZeroValueBytes.GetData());
		}

		ScriptVariableMap.Add(Parameter, NewScriptVariable);
		return NewScriptVariable;
	}

	static bool RemoveGraphParameterMetadata(UNiagaraGraph& Graph, const FNiagaraVariable& Parameter)
	{
		UNiagaraGraph::FScriptVariableMap& ScriptVariableMap = Graph.GetAllMetaData();
		return ScriptVariableMap.Remove(Parameter) > 0;
	}

	static bool SetScriptVariableDefaultValueFromText(UNiagaraScriptVariable& ScriptVariable, const FString& ValueText, FString& OutError)
	{
		const FNiagaraTypeDefinition& TypeDefinition = ScriptVariable.Variable.GetType();
		if (TypeDefinition.IsUObject() || TypeDefinition.IsDataInterface())
		{
			OutError = TEXT("script_variable_object_default_not_supported");
			return false;
		}

		TArray<uint8> ValueBytes;
		if (!BuildNiagaraValueBytesFromText(TypeDefinition, ValueText, ValueBytes, OutError))
		{
			return false;
		}

		ScriptVariable.Modify();
		ScriptVariable.SetDefaultValueData(ValueBytes.GetData());
		ScriptVariable.UpdateChangeId();
		return true;
	}

	static TSharedPtr<FJsonObject> BuildGraphParameterJson(const FNiagaraVariable& Parameter, const UNiagaraScriptVariable* ScriptVariable, const bool bIncludeValue)
	{
		TSharedPtr<FJsonObject> ParameterObj = MakeShared<FJsonObject>();
		const FString FullName = Parameter.GetName().ToString();
		const FNiagaraTypeDefinition& TypeDefinition = Parameter.GetType();

		ParameterObj->SetStringField(TEXT("name"), FullName);
		ParameterObj->SetStringField(TEXT("namespace"), GetVariableNamespace(FullName));
		ParameterObj->SetStringField(TEXT("leaf_name"), GetVariableLeafName(FullName));
		ParameterObj->SetStringField(TEXT("type"), TypeDefinition.GetNameText().ToString());
		ParameterObj->SetStringField(TEXT("type_internal"), TypeDefinition.GetName());
		ParameterObj->SetStringField(TEXT("type_path"), TypeDefinition.GetStruct() ? TypeDefinition.GetStruct()->GetPathName() : TEXT(""));

		if (ScriptVariable)
		{
			ParameterObj->SetBoolField(TEXT("is_static_switch"), ScriptVariable->GetIsStaticSwitch());
			ParameterObj->SetNumberField(TEXT("default_mode"), static_cast<int32>(ScriptVariable->DefaultMode));
		}

		if (bIncludeValue && ScriptVariable)
		{
			const uint8* DefaultValueData = ScriptVariable->GetDefaultValueData();
			const bool bHasDefaultValue = DefaultValueData != nullptr;
			ParameterObj->SetBoolField(TEXT("has_default_value"), bHasDefaultValue);
			if (bHasDefaultValue && !TypeDefinition.IsUObject() && !TypeDefinition.IsDataInterface())
			{
				ParameterObj->SetStringField(TEXT("default_value_text"), TypeDefinition.ToString(DefaultValueData));
			}
		}
		return ParameterObj;
	}

	static FString GetNiagaraAssetKind(UObject* Asset)
	{
		if (Cast<UNiagaraSystem>(Asset))
		{
			return TEXT("system");
		}
		if (Cast<UNiagaraEmitter>(Asset))
		{
			return TEXT("emitter");
		}
		if (Cast<UNiagaraScript>(Asset))
		{
			return TEXT("script");
		}
		return TEXT("unknown");
	}

	enum class ENiagaraCompileLogSeverityFilter : uint8
	{
		All,
		ErrorOnly,
		WarningOnly,
		WarningOrError,
		InfoOnly,
	};

	static FString ScriptCompileStatusToString(const ENiagaraScriptCompileStatus Status)
	{
		if (const UEnum* StatusEnum = StaticEnum<ENiagaraScriptCompileStatus>())
		{
			return StatusEnum->GetNameStringByValue(static_cast<int64>(Status));
		}
		return TEXT("NCS_Unknown");
	}

	static FString CompileEventSeverityToString(const FNiagaraCompileEventSeverity Severity)
	{
		switch (Severity)
		{
		case FNiagaraCompileEventSeverity::Log:
			return TEXT("Log");
		case FNiagaraCompileEventSeverity::Display:
			return TEXT("Display");
		case FNiagaraCompileEventSeverity::Warning:
			return TEXT("Warning");
		case FNiagaraCompileEventSeverity::Error:
			return TEXT("Error");
		default:
			return TEXT("Display");
		}
	}

	static FString CompileEventSourceToString(const FNiagaraCompileEventSource Source)
	{
		switch (Source)
		{
		case FNiagaraCompileEventSource::Unset:
			return TEXT("Unset");
		case FNiagaraCompileEventSource::ScriptDependency:
			return TEXT("ScriptDependency");
		default:
			return TEXT("Unset");
		}
	}

	static bool IsCompileStatusWarning(const ENiagaraScriptCompileStatus Status)
	{
		return Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
	}

	static bool ParseCompileLogSeverityFilter(const FString& SeverityFilterText, ENiagaraCompileLogSeverityFilter& OutFilter)
	{
		FString Normalized = SeverityFilterText.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("all"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::All;
			return true;
		}
		if (Normalized == TEXT("error") || Normalized == TEXT("errors"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::ErrorOnly;
			return true;
		}
		if (Normalized == TEXT("warning") || Normalized == TEXT("warnings") || Normalized == TEXT("warn"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::WarningOnly;
			return true;
		}
		if (Normalized == TEXT("warning_or_error") || Normalized == TEXT("warn_or_error") || Normalized == TEXT("errors_and_warnings"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::WarningOrError;
			return true;
		}
		if (Normalized == TEXT("info") || Normalized == TEXT("display") || Normalized == TEXT("log"))
		{
			OutFilter = ENiagaraCompileLogSeverityFilter::InfoOnly;
			return true;
		}
		return false;
	}

	static bool ShouldIncludeCompileEvent(const FNiagaraCompileEvent& CompileEvent, const ENiagaraCompileLogSeverityFilter SeverityFilter)
	{
		switch (SeverityFilter)
		{
		case ENiagaraCompileLogSeverityFilter::All:
			return true;
		case ENiagaraCompileLogSeverityFilter::ErrorOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Error;
		case ENiagaraCompileLogSeverityFilter::WarningOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning;
		case ENiagaraCompileLogSeverityFilter::WarningOrError:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Warning || CompileEvent.Severity == FNiagaraCompileEventSeverity::Error;
		case ENiagaraCompileLogSeverityFilter::InfoOnly:
			return CompileEvent.Severity == FNiagaraCompileEventSeverity::Log || CompileEvent.Severity == FNiagaraCompileEventSeverity::Display;
		default:
			return true;
		}
	}

	static TSharedPtr<FJsonObject> BuildCompileEventJson(const FNiagaraCompileEvent& CompileEvent, const bool bIncludeStackGuids)
	{
		TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
		EventObj->SetStringField(TEXT("severity"), CompileEventSeverityToString(CompileEvent.Severity));
		EventObj->SetStringField(TEXT("message"), CompileEvent.Message);
		EventObj->SetStringField(TEXT("short_description"), CompileEvent.ShortDescription);
		EventObj->SetStringField(TEXT("node_guid"), CompileEvent.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("pin_guid"), CompileEvent.PinGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("source"), CompileEventSourceToString(CompileEvent.Source));

		if (bIncludeStackGuids)
		{
			TArray<TSharedPtr<FJsonValue>> StackGuidValues;
			StackGuidValues.Reserve(CompileEvent.StackGuids.Num());
			for (const FGuid& StackGuid : CompileEvent.StackGuids)
			{
				StackGuidValues.Add(MakeShared<FJsonValueString>(StackGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
			}
			EventObj->SetArrayField(TEXT("stack_guids"), StackGuidValues);
		}

		return EventObj;
	}

	struct FCollectedNiagaraScriptInfo
	{
		UNiagaraScript* Script = nullptr;
		FString OwnerType;
		FString OwnerName;
		FString OwnerPath;
		FString EmitterHandleId;
		FString EmitterVersion;
		bool bEmitterEnabled = true;
	};

	static void AppendUniqueCollectedScript(TArray<FCollectedNiagaraScriptInfo>& OutScripts, TSet<const UNiagaraScript*>& SeenScripts, UNiagaraScript* Script, const FString& OwnerType, const FString& OwnerName, const FString& OwnerPath, const FString& EmitterHandleId, const FString& EmitterVersion, const bool bEmitterEnabled)
	{
		if (!Script || SeenScripts.Contains(Script))
		{
			return;
		}

		SeenScripts.Add(Script);

		FCollectedNiagaraScriptInfo& Item = OutScripts.AddDefaulted_GetRef();
		Item.Script = Script;
		Item.OwnerType = OwnerType;
		Item.OwnerName = OwnerName;
		Item.OwnerPath = OwnerPath;
		Item.EmitterHandleId = EmitterHandleId;
		Item.EmitterVersion = EmitterVersion;
		Item.bEmitterEnabled = bEmitterEnabled;
	}

	static void CollectSystemScriptsForCompileLog(UNiagaraSystem& NiagaraSystem, const bool bIncludeDisabledEmitters, TArray<FCollectedNiagaraScriptInfo>& OutScripts)
	{
		OutScripts.Reset();

		TSet<const UNiagaraScript*> SeenScripts;
		AppendUniqueCollectedScript(
			OutScripts,
			SeenScripts,
			NiagaraSystem.GetSystemSpawnScript(),
			TEXT("system_script"),
			TEXT("SystemSpawn"),
			NiagaraSystem.GetPathName(),
			FString(),
			FString(),
			true);

		AppendUniqueCollectedScript(
			OutScripts,
			SeenScripts,
			NiagaraSystem.GetSystemUpdateScript(),
			TEXT("system_script"),
			TEXT("SystemUpdate"),
			NiagaraSystem.GetPathName(),
			FString(),
			FString(),
			true);

		const TArray<FNiagaraEmitterHandle>& EmitterHandles = NiagaraSystem.GetEmitterHandles();
		for (const FNiagaraEmitterHandle& Handle : EmitterHandles)
		{
			if (!bIncludeDisabledEmitters && !Handle.GetIsEnabled())
			{
				continue;
			}

			FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
			const FVersionedNiagaraEmitterData* EmitterData = VersionedEmitter.GetEmitterData();
			if (!EmitterData && VersionedEmitter.Emitter)
			{
				EmitterData = VersionedEmitter.Emitter->GetLatestEmitterData();
			}
			if (!EmitterData)
			{
				continue;
			}

			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, false, false);
			const FString OwnerName = Handle.GetUniqueInstanceName();
			const FString OwnerPath = VersionedEmitter.Emitter ? VersionedEmitter.Emitter->GetPathName() : FString();
			const FString EmitterVersion = VersionedEmitter.Version.ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString HandleId = Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AppendUniqueCollectedScript(
					OutScripts,
					SeenScripts,
					Script,
					TEXT("emitter_in_system"),
					OwnerName,
					OwnerPath,
					HandleId,
					EmitterVersion,
					Handle.GetIsEnabled());
			}
		}
	}

	struct FNiagaraEmitterEditContext
	{
		UNiagaraEmitter* Emitter = nullptr;
		FGuid VersionGuid;
		FVersionedNiagaraEmitterData* EmitterData = nullptr;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
	};

	struct FNiagaraSystemEditContext
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraScriptSource* ScriptSource = nullptr;
		UNiagaraGraph* Graph = nullptr;
	};

	struct FResolvedNiagaraStage
	{
		UNiagaraNodeOutput* OutputNode = nullptr;
		ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
		FGuid UsageId;
		FString StageKey;
	};

	static FString ScriptUsageToString(const ENiagaraScriptUsage Usage)
	{
		if (const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			return UsageEnum->GetNameStringByValue((int64)Usage);
		}
		return TEXT("Unknown");
	}

	static bool TryParseScriptUsage(const FString& InUsageText, ENiagaraScriptUsage& OutUsage)
	{
		const FString UsageText = InUsageText.TrimStartAndEnd();
		if (UsageText.IsEmpty())
		{
			return false;
		}

		if (const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>())
		{
			int64 ParsedValue = UsageEnum->GetValueByNameString(UsageText, EGetByNameFlags::CheckAuthoredName);
			if (ParsedValue == INDEX_NONE)
			{
				const FString QualifiedName = FString::Printf(TEXT("ENiagaraScriptUsage::%s"), *UsageText);
				ParsedValue = UsageEnum->GetValueByNameString(QualifiedName, EGetByNameFlags::CheckAuthoredName);
			}
			if (ParsedValue == INDEX_NONE)
			{
				ParsedValue = UsageEnum->GetValueByNameString(UsageText, EGetByNameFlags::None);
			}
			if (ParsedValue != INDEX_NONE)
			{
				OutUsage = (ENiagaraScriptUsage)ParsedValue;
				return true;
			}
		}
		return false;
	}

	static FString ScriptLibraryVisibilityToString(const ENiagaraScriptLibraryVisibility Visibility)
	{
		if (const UEnum* VisibilityEnum = StaticEnum<ENiagaraScriptLibraryVisibility>())
		{
			return VisibilityEnum->GetNameStringByValue((int64)Visibility);
		}
		return TEXT("Unknown");
	}

	static TArray<TSharedPtr<FJsonValue>> BuildScriptUsageJsonArray(const TArray<ENiagaraScriptUsage>& Usages)
	{
		TArray<TSharedPtr<FJsonValue>> UsageJson;
		UsageJson.Reserve(Usages.Num());
		for (const ENiagaraScriptUsage Usage : Usages)
		{
			UsageJson.Add(MakeShared<FJsonValueString>(ScriptUsageToString(Usage)));
		}
		return UsageJson;
	}

	static FString MakeRelativePackagePath(const FString& PackagePath, const FString& RootPath)
	{
		FString RelativePath = PackagePath;
		if (RelativePath.RemoveFromStart(RootPath))
		{
			RelativePath.RemoveFromStart(TEXT("/"));
		}
		return RelativePath;
	}

	static FString MakeModuleCategory(const FString& RelativePackagePath)
	{
		if (RelativePackagePath.IsEmpty())
		{
			return TEXT("Root");
		}

		FString Category;
		FString Remainder;
		if (RelativePackagePath.Split(TEXT("/"), &Category, &Remainder))
		{
			return Category;
		}
		return RelativePackagePath;
	}

	static FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		return FString::Printf(TEXT("%s|%s"), *ScriptUsageToString(Usage), *UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	static bool TryParseStageKey(const FString& StageKey, ENiagaraScriptUsage& OutUsage, FGuid& OutUsageId)
	{
		FString StageKeyText = StageKey.TrimStartAndEnd();
		if (StageKeyText.IsEmpty())
		{
			return false;
		}

		FString UsageText;
		FString UsageIdText;
		if (!StageKeyText.Split(TEXT("|"), &UsageText, &UsageIdText))
		{
			return false;
		}

		if (!TryParseScriptUsage(UsageText, OutUsage))
		{
			return false;
		}
		return FGuid::Parse(UsageIdText, OutUsageId);
	}

	static bool GetSystemEditContext(const TSharedPtr<FJsonObject>& Params, FNiagaraSystemEditContext& OutCtx, FString& OutError, const bool bRequireGraph)
	{
		FString SystemAssetPathInput;
		if (!Params.IsValid() ||
			(!Params->TryGetStringField(TEXT("system_asset_path"), SystemAssetPathInput) &&
				!Params->TryGetStringField(TEXT("asset_path"), SystemAssetPathInput)) ||
			SystemAssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_system_asset_path");
			return false;
		}

		UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadAssetObject(SystemAssetPathInput));
		if (!System)
		{
			OutError = TEXT("system_asset_not_found");
			return false;
		}

		UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
		UNiagaraScriptSource* ScriptSource = SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr;
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (bRequireGraph && (!ScriptSource || !Graph))
		{
			OutError = TEXT("missing_system_graph_source");
			return false;
		}

		OutCtx.System = System;
		OutCtx.ScriptSource = ScriptSource;
		OutCtx.Graph = Graph;
		return true;
	}

	static bool IsSystemStageUsage(const ENiagaraScriptUsage Usage)
	{
		return Usage == ENiagaraScriptUsage::SystemSpawnScript ||
			Usage == ENiagaraScriptUsage::SystemUpdateScript;
	}

	static bool GetEmitterEditContext(const TSharedPtr<FJsonObject>& Params, FNiagaraEmitterEditContext& OutCtx, FString& OutError, const bool bRequireGraph)
	{
		FString EmitterAssetPathInput;
		if (!Params.IsValid() ||
			(!Params->TryGetStringField(TEXT("emitter_asset_path"), EmitterAssetPathInput) &&
				!Params->TryGetStringField(TEXT("asset_path"), EmitterAssetPathInput)) ||
			EmitterAssetPathInput.IsEmpty())
		{
			OutError = TEXT("missing_emitter_asset_path");
			return false;
		}

		UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(LoadAssetObject(EmitterAssetPathInput));
		if (!Emitter)
		{
			OutError = TEXT("emitter_asset_not_found");
			return false;
		}

		FGuid VersionGuid = Emitter->GetExposedVersion().VersionGuid;
		FString VersionText;
		if (Params->TryGetStringField(TEXT("emitter_version"), VersionText) && !VersionText.IsEmpty())
		{
			FGuid ParsedVersionGuid;
			if (!FGuid::Parse(VersionText, ParsedVersionGuid))
			{
				OutError = TEXT("invalid_emitter_version");
				return false;
			}
			VersionGuid = ParsedVersionGuid;
		}

		FVersionedNiagaraEmitterData* EmitterData = Emitter->GetEmitterData(VersionGuid);
		if (!EmitterData)
		{
			EmitterData = Emitter->GetLatestEmitterData();
			if (EmitterData)
			{
				VersionGuid = EmitterData->Version.VersionGuid;
			}
		}
		if (!EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
		if (bRequireGraph && (!ScriptSource || !Graph))
		{
			OutError = TEXT("missing_emitter_graph_source");
			return false;
		}

		OutCtx.Emitter = Emitter;
		OutCtx.VersionGuid = VersionGuid;
		OutCtx.EmitterData = EmitterData;
		OutCtx.ScriptSource = ScriptSource;
		OutCtx.Graph = Graph;
		return true;
	}

	static UNiagaraNodeFunctionCall* FindModuleNodeByGuid(UNiagaraGraph* Graph, const FGuid& ModuleNodeGuid)
	{
		if (!Graph || !ModuleNodeGuid.IsValid())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(Node);
			if (ModuleNode && ModuleNode->NodeGuid == ModuleNodeGuid)
			{
				return ModuleNode;
			}
		}
		return nullptr;
	}

	static FString GetModuleScriptPath(UNiagaraNodeFunctionCall& ModuleNode)
	{
		if (ModuleNode.FunctionScript)
		{
			return ModuleNode.FunctionScript->GetPathName();
		}
		return ModuleNode.FunctionScriptAssetObjectPath.ToString();
	}

	static void GetOutputNodesFromGraph(UNiagaraGraph& Graph, TArray<UNiagaraNodeOutput*>& OutOutputNodes)
	{
		OutOutputNodes.Reset();
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
			{
				OutOutputNodes.Add(OutputNode);
			}
		}
	}

	static UEdGraphPin* FindFirstInputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindFirstOutputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	struct FLocalNiagaraStackNodeGroup
	{
		TArray<UNiagaraNode*> StartNodes;
		UNiagaraNode* EndNode = nullptr;
	};

	static bool IsParameterMapPin(const UEdGraphPin* Pin)
	{
		return Pin != nullptr && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef();
	}

	static UEdGraphPin* FindParameterMapInputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && IsParameterMapPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* FindParameterMapOutputPin(UEdGraphNode& Node)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && IsParameterMapPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool TryGetOrCreateStackFunctionInputOverridePin(
		UNiagaraNodeFunctionCall& ModuleNode,
		const FNiagaraParameterHandle& AliasedInputHandle,
		const FNiagaraTypeDefinition& InputType,
		const FGuid& InputScriptVariableGuid,
		UEdGraphPin*& OutOverridePin,
		FString& OutError)
	{
		OutOverridePin = FindOverridePinByAliasedInputName(
			Cast<UNiagaraGraph>(ModuleNode.GetGraph()),
			AliasedInputHandle.GetParameterHandleString());
		if (OutOverridePin)
		{
			return true;
		}

		UNiagaraGraph* Graph = Cast<UNiagaraGraph>(ModuleNode.GetGraph());
		if (!Graph)
		{
			OutError = TEXT("module_graph_missing");
			return false;
		}

		bool bModuleIsInOutputStack = false;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node);
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
			GetStageModuleNodes(*OutputNode, ModuleNodes);
			if (ModuleNodes.Contains(&ModuleNode))
			{
				bModuleIsInOutputStack = true;
				break;
			}
		}
		if (!bModuleIsInOutputStack)
		{
			OutError = TEXT("module_not_in_output_stack");
			return false;
		}

		UEdGraphPin* ParameterMapInputPin = FindParameterMapInputPin(ModuleNode);
		if (!ParameterMapInputPin)
		{
			OutError = TEXT("module_parameter_map_input_missing");
			return false;
		}
		if (ParameterMapInputPin->LinkedTo.Num() != 1)
		{
			OutError = FString::Printf(TEXT("module_parameter_map_input_link_count:%d"), ParameterMapInputPin->LinkedTo.Num());
			return false;
		}

		UEdGraphPin* PreviousStackPin = ParameterMapInputPin->LinkedTo[0];
		if (!PreviousStackPin || !IsParameterMapPin(PreviousStackPin) || PreviousStackPin->Direction != EGPD_Output)
		{
			OutError = TEXT("module_parameter_map_input_invalid_link");
			return false;
		}

		OutOverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			ModuleNode,
			AliasedInputHandle,
			InputType,
			InputScriptVariableGuid,
			FGuid());
		return OutOverridePin != nullptr;
	}

	static UNiagaraNodeInput* FindStageParameterMapInputNode(UNiagaraNodeOutput& OutputNode)
	{
		UEdGraphNode* CurrentNode = &OutputNode;
		TSet<UEdGraphNode*> VisitedNodes;

		while (CurrentNode && !VisitedNodes.Contains(CurrentNode))
		{
			VisitedNodes.Add(CurrentNode);

			UEdGraphPin* InputPin = FindParameterMapInputPin(*CurrentNode);
			if (!InputPin || InputPin->LinkedTo.Num() == 0)
			{
				return nullptr;
			}

			UEdGraphPin* LinkedPin = nullptr;
			for (UEdGraphPin* CandidatePin : InputPin->LinkedTo)
			{
				if (IsParameterMapPin(CandidatePin))
				{
					LinkedPin = CandidatePin;
					break;
				}
			}
			if (!LinkedPin)
			{
				return nullptr;
			}

			UEdGraphNode* PreviousNode = LinkedPin->GetOwningNode();
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(PreviousNode))
			{
				return InputNode;
			}

			CurrentNode = PreviousNode;
		}

		return nullptr;
	}

	static void GetLocalStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FLocalNiagaraStackNodeGroup>& OutStackNodeGroups)
	{
		OutStackNodeGroups.Reset();

		UNiagaraNodeInput* InputNode = FindStageParameterMapInputNode(OutputNode);
		if (!InputNode)
		{
			return;
		}

		FLocalNiagaraStackNodeGroup InputGroup;
		InputGroup.StartNodes.Add(InputNode);
		InputGroup.EndNode = InputNode;
		OutStackNodeGroups.Add(InputGroup);

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		GetStageModuleNodes(OutputNode, ModuleNodes);
		for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
		{
			if (!ModuleNode)
			{
				continue;
			}

			FLocalNiagaraStackNodeGroup ModuleGroup;
			UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
			if (!PreviousOutputPin)
			{
				return;
			}
			for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
			{
				if (LinkedPin)
				{
					if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
					{
						ModuleGroup.StartNodes.Add(LinkedNode);
					}
				}
			}
			ModuleGroup.EndNode = ModuleNode;
			OutStackNodeGroups.Add(ModuleGroup);
		}

		FLocalNiagaraStackNodeGroup OutputGroup;
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*OutStackNodeGroups.Last().EndNode);
		if (!PreviousOutputPin)
		{
			return;
		}
		for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
		{
			if (LinkedPin)
			{
				if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
				{
					OutputGroup.StartNodes.Add(LinkedNode);
				}
			}
		}
		OutputGroup.EndNode = &OutputNode;
		OutStackNodeGroups.Add(OutputGroup);
	}

	static void DisconnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& DisconnectGroup, const FLocalNiagaraStackNodeGroup& PreviousGroup, const FLocalNiagaraStackNodeGroup& NextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*PreviousGroup.EndNode);
		UEdGraphPin* DisconnectOutputPin = FindParameterMapOutputPin(*DisconnectGroup.EndNode);
		if (!PreviousOutputPin || !DisconnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		DisconnectOutputPin->BreakAllPinLinks(true);

		for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				PreviousOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static void ConnectLocalStackNodeGroup(const FLocalNiagaraStackNodeGroup& ConnectGroup, const FLocalNiagaraStackNodeGroup& NewPreviousGroup, const FLocalNiagaraStackNodeGroup& NewNextGroup)
	{
		UEdGraphPin* PreviousOutputPin = FindParameterMapOutputPin(*NewPreviousGroup.EndNode);
		UEdGraphPin* ConnectOutputPin = FindParameterMapOutputPin(*ConnectGroup.EndNode);
		if (!PreviousOutputPin || !ConnectOutputPin)
		{
			return;
		}

		PreviousOutputPin->BreakAllPinLinks(true);
		for (UNiagaraNode* ConnectStartNode : ConnectGroup.StartNodes)
		{
			if (!ConnectStartNode)
			{
				continue;
			}
			if (UEdGraphPin* ConnectInputPin = FindParameterMapInputPin(*ConnectStartNode))
			{
				PreviousOutputPin->MakeLinkTo(ConnectInputPin);
			}
		}

		for (UNiagaraNode* NextStartNode : NewNextGroup.StartNodes)
		{
			if (!NextStartNode)
			{
				continue;
			}
			if (UEdGraphPin* NextInputPin = FindParameterMapInputPin(*NextStartNode))
			{
				ConnectOutputPin->MakeLinkTo(NextInputPin);
			}
		}
	}

	static UNiagaraNodeOutput* EnsureStageInputOutputBridge(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		Graph.Modify();
		NormalizeGraphNodeGuids(&Graph);

		UNiagaraNodeOutput* OutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> ExistingOutputs;
		GetOutputNodesFromGraph(Graph, ExistingOutputs);
		for (UNiagaraNodeOutput* ExistingOutput : ExistingOutputs)
		{
			if (ExistingOutput && ExistingOutput->GetUsage() == Usage && ExistingOutput->GetUsageId() == UsageId)
			{
				OutputNode = ExistingOutput;
				break;
			}
		}

		UEdGraphPin* OutputNodeInputPin = OutputNode ? FindFirstInputPin(*OutputNode) : nullptr;
		if (OutputNode && !OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: remove invalid output node usage=%s usage_id=%s reason=no_input_pin"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			Graph.RemoveNode(OutputNode);
			OutputNode = nullptr;
		}

		if (!OutputNode)
		{
			OutputNode = NewObject<UNiagaraNodeOutput>(&Graph, NAME_None, RF_Transactional);
			if (!OutputNode)
			{
				return nullptr;
			}
			OutputNode->CreateNewGuid();
			OutputNode->SetUsage(Usage);
			OutputNode->SetUsageId(UsageId);
			OutputNode->Outputs.Empty();
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			OutputNode->AllocateDefaultPins();
			Graph.AddNode(OutputNode, true, false);
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}
		else
		{
			OutputNode->Modify();
			OutputNodeInputPin = FindFirstInputPin(*OutputNode);
		}

		if (!OutputNodeInputPin)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=output_input_pin_missing"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			return nullptr;
		}

		if (OutputNodeInputPin->LinkedTo.Num() > 0 && FindStageParameterMapInputNode(*OutputNode) == nullptr)
		{
			FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: reset invalid stack usage=%s usage_id=%s reason=input_node_unreachable"),
				*ScriptUsageToString(Usage),
				*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			OutputNodeInputPin->BreakAllPinLinks();
		}

		if (OutputNodeInputPin->LinkedTo.Num() == 0)
		{
			UNiagaraNodeInput* InputNode = NewObject<UNiagaraNodeInput>(&Graph, NAME_None, RF_Transactional);
			if (!InputNode)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=create_input_node_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
			InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			InputNode->CreateNewGuid();
			InputNode->AllocateDefaultPins();
			Graph.AddNode(InputNode, true, false);

			UEdGraphPin* InputNodeOutputPin = FindFirstOutputPin(*InputNode);
			if (!InputNodeOutputPin)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=input_output_pin_missing"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}

			bool bLinked = false;
			if (const UEdGraphSchema* Schema = Graph.GetSchema())
			{
				bLinked = Schema->TryCreateConnection(OutputNodeInputPin, InputNodeOutputPin);
			}
			if (!bLinked)
			{
				OutputNodeInputPin->MakeLinkTo(InputNodeOutputPin);
				bLinked = OutputNodeInputPin->LinkedTo.Contains(InputNodeOutputPin) || InputNodeOutputPin->LinkedTo.Contains(OutputNodeInputPin);
			}
			if (!bLinked)
			{
				FUeAgentInterfaceLogger::Log(TEXT("Niagara EnsureStageBridge: failed usage=%s usage_id=%s reason=link_failed"),
					*ScriptUsageToString(Usage),
					*UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				return nullptr;
			}
		}

		return OutputNode;
	}

	static void GatherUpstreamNodesRecursive(UEdGraphNode* Node, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphNode*>& OutNodes)
	{
		if (!Node || VisitedNodes.Contains(Node))
		{
			return;
		}

		VisitedNodes.Add(Node);
		OutNodes.Add(Node);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin)
				{
					GatherUpstreamNodesRecursive(LinkedPin->GetOwningNode(), VisitedNodes, OutNodes);
				}
			}
		}
	}

	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules)
	{
		OutModules.Reset();

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(&OutputNode, VisitedNodes, StageNodes);

		TArray<UNiagaraNodeFunctionCall*> ModulesFromOutputToInput;
		for (UEdGraphNode* StageNode : StageNodes)
		{
			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(StageNode);
			if (!ModuleNode)
			{
				continue;
			}

			const ENiagaraScriptUsage CalledUsage = ModuleNode->GetCalledUsage();
			if (CalledUsage == ENiagaraScriptUsage::Module || CalledUsage == ENiagaraScriptUsage::Function)
			{
				ModulesFromOutputToInput.Add(ModuleNode);
			}
		}

		for (int32 Index = ModulesFromOutputToInput.Num() - 1; Index >= 0; --Index)
		{
			OutModules.Add(ModulesFromOutputToInput[Index]);
		}
	}

	static bool ResolveStageFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, FString& OutError)
	{
		if (!Params.IsValid())
		{
			OutError = TEXT("missing_params");
			return false;
		}

		ENiagaraScriptUsage TargetUsage = ENiagaraScriptUsage::Function;
		FGuid TargetUsageId;
		bool bHasUsage = false;
		bool bHasUsageId = false;

		FString StageKey;
		if (Params->TryGetStringField(TEXT("stage_key"), StageKey) && !StageKey.IsEmpty())
		{
			if (!TryParseStageKey(StageKey, TargetUsage, TargetUsageId))
			{
				OutError = TEXT("invalid_stage_key");
				return false;
			}
			bHasUsage = true;
			bHasUsageId = true;
		}
		else
		{
			FString UsageText;
			if (Params->TryGetStringField(TEXT("script_usage"), UsageText) && !UsageText.IsEmpty())
			{
				if (!TryParseScriptUsage(UsageText, TargetUsage))
				{
					OutError = TEXT("invalid_script_usage");
					return false;
				}
				bHasUsage = true;
			}

			FString UsageIdText;
			if (Params->TryGetStringField(TEXT("script_usage_id"), UsageIdText) && !UsageIdText.IsEmpty())
			{
				if (!FGuid::Parse(UsageIdText, TargetUsageId))
				{
					OutError = TEXT("invalid_script_usage_id");
					return false;
				}
				bHasUsageId = true;
			}
		}

		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		if (OutputNodes.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_output_nodes");
			return false;
		}

		TArray<UNiagaraNodeOutput*> Matches;
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}
			if (bHasUsage && OutputNode->GetUsage() != TargetUsage)
			{
				continue;
			}
			if (bHasUsageId && OutputNode->GetUsageId() != TargetUsageId)
			{
				continue;
			}
			Matches.Add(OutputNode);
		}

		if (Matches.Num() == 0)
		{
			OutError = TEXT("stage_not_found");
			return false;
		}
		if (Matches.Num() > 1 && !bHasUsageId)
		{
			OutError = TEXT("stage_is_ambiguous_require_script_usage_id");
			return false;
		}

		OutStage.OutputNode = Matches[0];
		OutStage.Usage = Matches[0]->GetUsage();
		OutStage.UsageId = Matches[0]->GetUsageId();
		OutStage.StageKey = MakeStageKey(OutStage.Usage, OutStage.UsageId);
		return true;
	}

	static void GetModuleInputPins(UNiagaraNodeFunctionCall& ModuleNode, TArray<UEdGraphPin*>& OutInputPins)
	{
		OutInputPins.Reset();
		for (UEdGraphPin* Pin : ModuleNode.Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->ParentPin != nullptr)
			{
				continue;
			}

			const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				continue;
			}

			OutInputPins.Add(Pin);
		}
	}

	struct FResolvedNiagaraModuleInput
	{
		FNiagaraVariable InputVariable;
		FString FullInputName;
		FString ShortInputName;
		FGuid InputScriptVariableGuid;
		UEdGraphPin* VisiblePin = nullptr;
	};

	static FString MakeLowerTrimmed(const FString& InText)
	{
		FString OutText = InText.TrimStartAndEnd();
		OutText.ToLowerInline();
		return OutText;
	}

	static UEdGraphPin* FindOverridePinByAliasedInputName(UNiagaraGraph* Graph, const FName AliasedInputName)
	{
		if (!Graph || AliasedInputName.IsNone())
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				if (Pin->Direction != EGPD_Input)
				{
					continue;
				}
				if (Pin->PinName == AliasedInputName)
				{
					return Pin;
				}
			}
		}

		return nullptr;
	}

	static void BuildResolvedModuleInputs(UNiagaraNodeFunctionCall& ModuleNode, TArray<FResolvedNiagaraModuleInput>& OutInputs)
	{
		OutInputs.Reset();

		TMap<FString, int32> IndexByLowerFullName;

		TArray<UEdGraphPin*> VisiblePins;
		GetModuleInputPins(ModuleNode, VisiblePins);
		for (UEdGraphPin* InputPin : VisiblePins)
		{
			if (!InputPin)
			{
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.FullInputName = InputPin->PinName.ToString();
			InputData.ShortInputName = InputData.FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}
			InputData.InputVariable = UEdGraphSchema_Niagara::PinToNiagaraVariable(InputPin);
			if (!InputData.InputVariable.IsValid())
			{
				InputData.InputVariable = FNiagaraVariable(UEdGraphSchema_Niagara::PinToTypeDefinition(InputPin), InputPin->PinName);
			}
			InputData.VisiblePin = InputPin;

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(MakeLowerTrimmed(OutInputs[NewIndex].FullInputName), NewIndex);
		}

		TArray<FNiagaraVariable> StackInputVariables;
		TSet<FNiagaraVariable> HiddenInputVariables;
		FCompileConstantResolver ConstantResolver;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			ModuleNode,
			StackInputVariables,
			HiddenInputVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
			false);

		for (const FNiagaraVariable& InputVariable : StackInputVariables)
		{
			if (!InputVariable.IsValid())
			{
				continue;
			}

			const FString FullInputName = InputVariable.GetName().ToString();
			const FString LowerFullInputName = MakeLowerTrimmed(FullInputName);
			int32* ExistingIndex = IndexByLowerFullName.Find(LowerFullInputName);
			if (ExistingIndex)
			{
				FResolvedNiagaraModuleInput& ExistingInput = OutInputs[*ExistingIndex];
				if (!ExistingInput.InputVariable.IsValid())
				{
					ExistingInput.InputVariable = InputVariable;
				}
				continue;
			}

			FResolvedNiagaraModuleInput InputData;
			InputData.InputVariable = InputVariable;
			InputData.FullInputName = FullInputName;
			InputData.ShortInputName = FullInputName;
			int32 DotIndex = INDEX_NONE;
			if (InputData.ShortInputName.FindLastChar(TEXT('.'), DotIndex))
			{
				InputData.ShortInputName = InputData.ShortInputName.Mid(DotIndex + 1);
			}

			const int32 NewIndex = OutInputs.Add(MoveTemp(InputData));
			IndexByLowerFullName.Add(LowerFullInputName, NewIndex);
		}
	}

	static const FResolvedNiagaraModuleInput* ResolveModuleInputByName(const TArray<FResolvedNiagaraModuleInput>& ModuleInputs, const FString& InputName, FString& OutResolvedInputName)
	{
		const FString InputNameNormalized = InputName.TrimStartAndEnd();
		if (InputNameNormalized.IsEmpty())
		{
			return nullptr;
		}

		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (InputData.FullInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				OutResolvedInputName = InputData.FullInputName;
				return &InputData;
			}
		}

		const FResolvedNiagaraModuleInput* ShortMatch = nullptr;
		for (const FResolvedNiagaraModuleInput& InputData : ModuleInputs)
		{
			if (!InputData.ShortInputName.Equals(InputNameNormalized, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ShortMatch != nullptr)
			{
				return nullptr;
			}
			ShortMatch = &InputData;
		}

		if (ShortMatch)
		{
			OutResolvedInputName = ShortMatch->FullInputName;
		}
		return ShortMatch;
	}

	static TSharedPtr<FJsonObject> BuildModuleInputJson(UNiagaraNodeFunctionCall& ModuleNode, const FResolvedNiagaraModuleInput& InputData)
	{
		TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
		const FNiagaraTypeDefinition InputType = InputData.InputVariable.GetType();
		bool bHasLinks = false;
		bool bHasDefaultOverride = false;
		FString OverrideDefaultValue;
		FString AutogeneratedDefaultValue;

		if (InputData.VisiblePin)
		{
			bHasLinks = InputData.VisiblePin->LinkedTo.Num() > 0;
			bHasDefaultOverride = InputData.VisiblePin->DefaultValue != InputData.VisiblePin->AutogeneratedDefaultValue;
			OverrideDefaultValue = InputData.VisiblePin->DefaultValue;
			AutogeneratedDefaultValue = InputData.VisiblePin->AutogeneratedDefaultValue;
		}
		else if (InputData.InputVariable.IsValid())
		{
			const FNiagaraParameterHandle InputHandle(InputData.InputVariable.GetName());
			const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &ModuleNode);
			UNiagaraGraph* Graph = Cast<UNiagaraGraph>(ModuleNode.GetGraph());
			if (UEdGraphPin* OverridePin = FindOverridePinByAliasedInputName(Graph, AliasedHandle.GetParameterHandleString()))
			{
				bHasLinks = OverridePin->LinkedTo.Num() > 0;
				bHasDefaultOverride = !OverridePin->DefaultValue.IsEmpty() ||
					OverridePin->DefaultValue != OverridePin->AutogeneratedDefaultValue;
				OverrideDefaultValue = OverridePin->DefaultValue;
				AutogeneratedDefaultValue = OverridePin->AutogeneratedDefaultValue;
			}
		}

		InputObj->SetStringField(TEXT("input_name"), InputData.FullInputName);
		InputObj->SetStringField(TEXT("input_short_name"), InputData.ShortInputName);
		InputObj->SetStringField(TEXT("input_type"), InputType.GetNameText().ToString());
		InputObj->SetBoolField(TEXT("has_override"), bHasLinks || bHasDefaultOverride);
		InputObj->SetBoolField(TEXT("has_links"), bHasLinks);
		InputObj->SetStringField(TEXT("override_default_value"), OverrideDefaultValue);
		InputObj->SetStringField(TEXT("autogenerated_default_value"), AutogeneratedDefaultValue);
		InputObj->SetBoolField(TEXT("has_visible_pin"), InputData.VisiblePin != nullptr);
		return InputObj;
	}

	static TSharedPtr<FJsonObject> BuildModuleNodeJson(UNiagaraNodeFunctionCall& ModuleNode, const int32 ModuleIndex, const bool bIncludeInputs)
	{
		TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
		ModuleObj->SetNumberField(TEXT("module_index"), ModuleIndex);
		ModuleObj->SetStringField(TEXT("module_node_guid"), ModuleNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		ModuleObj->SetStringField(TEXT("module_name"), ModuleNode.GetFunctionName());
		ModuleObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(ModuleNode));
		ModuleObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(&ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
		ModuleObj->SetBoolField(TEXT("module_has_enabled_state"), true);
		ModuleObj->SetBoolField(TEXT("module_enabled"), ModuleNode.IsNodeEnabled());

		if (bIncludeInputs)
		{
			TArray<FResolvedNiagaraModuleInput> Inputs;
			BuildResolvedModuleInputs(ModuleNode, Inputs);
			TArray<TSharedPtr<FJsonValue>> InputsJson;
			InputsJson.Reserve(Inputs.Num());
			for (const FResolvedNiagaraModuleInput& InputData : Inputs)
			{
				InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(ModuleNode, InputData)));
			}
			ModuleObj->SetArrayField(TEXT("inputs"), InputsJson);
			ModuleObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
		}

		return ModuleObj;
	}

	static TSharedPtr<FJsonObject> BuildStageNodeJson(UEdGraphNode& StageNode, const int32 StageNodeIndex, const bool bIncludeProperties, const bool bIncludeModuleInputs)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetNumberField(TEXT("stage_node_index"), StageNodeIndex);
		NodeObj->SetStringField(TEXT("node_guid"), StageNode.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		NodeObj->SetStringField(TEXT("node_class"), StageNode.GetClass()->GetPathName());
		NodeObj->SetStringField(TEXT("node_title"), StageNode.GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObj->SetNumberField(TEXT("node_pos_x"), StageNode.NodePosX);
		NodeObj->SetNumberField(TEXT("node_pos_y"), StageNode.NodePosY);
		NodeObj->SetBoolField(TEXT("is_output_node"), Cast<UNiagaraNodeOutput>(&StageNode) != nullptr);
		NodeObj->SetBoolField(TEXT("is_module_node"), Cast<UNiagaraNodeFunctionCall>(&StageNode) != nullptr);

		if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(&StageNode))
		{
			NodeObj->SetStringField(TEXT("module_name"), ModuleNode->GetFunctionName());
			NodeObj->SetStringField(TEXT("module_script_path"), GetModuleScriptPath(*ModuleNode));
			NodeObj->SetStringField(TEXT("module_kind"), Cast<UNiagaraNodeAssignment>(ModuleNode) ? TEXT("assignment") : TEXT("script_module"));
			NodeObj->SetBoolField(TEXT("module_has_enabled_state"), true);
			NodeObj->SetBoolField(TEXT("module_enabled"), ModuleNode->IsNodeEnabled());

			if (bIncludeModuleInputs)
			{
				TArray<FResolvedNiagaraModuleInput> Inputs;
				BuildResolvedModuleInputs(*ModuleNode, Inputs);

				TArray<TSharedPtr<FJsonValue>> InputsJson;
				InputsJson.Reserve(Inputs.Num());
				for (const FResolvedNiagaraModuleInput& InputData : Inputs)
				{
					InputsJson.Add(MakeShared<FJsonValueObject>(BuildModuleInputJson(*ModuleNode, InputData)));
				}
				NodeObj->SetArrayField(TEXT("inputs"), InputsJson);
				NodeObj->SetNumberField(TEXT("input_count"), InputsJson.Num());
			}
		}

		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertiesJson;
			for (TFieldIterator<FProperty> It(StageNode.GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}

				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(&StageNode);
				PropertiesJson.Add(MakeShared<FJsonValueObject>(BuildPropertyValueJson(StageNode, *Property, ValuePtr)));
			}
			NodeObj->SetArrayField(TEXT("properties"), PropertiesJson);
			NodeObj->SetNumberField(TEXT("property_count"), PropertiesJson.Num());
		}

		return NodeObj;
	}

	static TSharedPtr<FJsonObject> BuildRendererJson(UNiagaraRendererProperties& Renderer, const int32 RendererIndex)
	{
		TSharedPtr<FJsonObject> RendererObj = MakeShared<FJsonObject>();
		RendererObj->SetNumberField(TEXT("renderer_index"), RendererIndex);
		RendererObj->SetStringField(TEXT("renderer_name"), Renderer.GetFName().ToString());
		RendererObj->SetStringField(TEXT("renderer_class"), Renderer.GetClass()->GetPathName());
		RendererObj->SetStringField(TEXT("renderer_object_path"), Renderer.GetPathName());
		return RendererObj;
	}

	static UClass* ResolveRendererClass(const FString& RendererClassOrAlias)
	{
		FString ClassText = RendererClassOrAlias.TrimStartAndEnd();
		if (ClassText.IsEmpty())
		{
			return nullptr;
		}

		const FString AliasLower = ClassText.ToLower();
		if (AliasLower == TEXT("sprite"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
		}
		else if (AliasLower == TEXT("ribbon"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
		}
		else if (AliasLower == TEXT("mesh"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraMeshRendererProperties");
		}
		else if (AliasLower == TEXT("light"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraLightRendererProperties");
		}
		else if (AliasLower == TEXT("decal"))
		{
			ClassText = TEXT("/Script/Niagara.NiagaraDecalRendererProperties");
		}

		UClass* RendererClass = ResolveClassByPath(ClassText);
		if (!RendererClass || !RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()) || RendererClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}

		return RendererClass;
	}

	static bool ResolveRendererFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutRendererIndex, UNiagaraRendererProperties*& OutRenderer, FString& OutError)
	{
		OutRendererIndex = INDEX_NONE;
		OutRenderer = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		const TArray<UNiagaraRendererProperties*>& Renderers = EditContext.EmitterData->GetRenderers();
		if (Renderers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_renderers");
			return false;
		}

		double RendererIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("renderer_index"), RendererIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(RendererIndexNumber);
			if (!Renderers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_renderer_index");
				return false;
			}

			OutRendererIndex = Index;
			OutRenderer = Renderers[Index];
			return OutRenderer != nullptr;
		}

		FString RendererName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_name"), RendererName) && !RendererName.IsEmpty())
		{
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (Renderer && Renderer->GetFName().ToString().Equals(RendererName, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		FString RendererClass;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("renderer_class"), RendererClass) && !RendererClass.IsEmpty())
		{
			UClass* ResolvedClass = ResolveRendererClass(RendererClass);
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				UNiagaraRendererProperties* Renderer = Renderers[Index];
				if (!Renderer)
				{
					continue;
				}
				if (ResolvedClass)
				{
					if (Renderer->GetClass() == ResolvedClass)
					{
						OutRendererIndex = Index;
						OutRenderer = Renderer;
						return true;
					}
				}
				else if (Renderer->GetClass()->GetPathName().Equals(RendererClass, ESearchCase::IgnoreCase))
				{
					OutRendererIndex = Index;
					OutRenderer = Renderer;
					return true;
				}
			}
		}

		OutError = TEXT("renderer_not_found");
		return false;
	}

	static FString ExecutionModeToString(const EScriptExecutionMode ExecutionMode)
	{
		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			return Enum->GetNameStringByValue((int64)ExecutionMode);
		}
		return TEXT("");
	}

	static bool ParseExecutionMode(const FString& ExecutionModeText, EScriptExecutionMode& OutExecutionMode)
	{
		const FString Input = ExecutionModeText.TrimStartAndEnd();
		if (Input.IsEmpty())
		{
			return false;
		}

		if (const UEnum* Enum = StaticEnum<EScriptExecutionMode>())
		{
			int64 Value = Enum->GetValueByNameString(Input, EGetByNameFlags::CheckAuthoredName);
			if (Value == INDEX_NONE)
			{
				const FString QualifiedName = FString::Printf(TEXT("EScriptExecutionMode::%s"), *Input);
				Value = Enum->GetValueByNameString(QualifiedName, EGetByNameFlags::CheckAuthoredName);
			}
			if (Value == INDEX_NONE)
			{
				Value = Enum->GetValueByNameString(Input, EGetByNameFlags::None);
			}
			if (Value != INDEX_NONE)
			{
				OutExecutionMode = static_cast<EScriptExecutionMode>(Value);
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildEventHandlerJson(FNiagaraEventScriptProperties& EventHandler, const int32 EventIndex)
	{
		TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
		const FGuid UsageId = EventHandler.Script ? EventHandler.Script->GetUsageId() : FGuid();
		EventObj->SetNumberField(TEXT("event_index"), EventIndex);
		EventObj->SetStringField(TEXT("script_usage_id"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		EventObj->SetStringField(TEXT("stage_key"), MakeStageKey(ENiagaraScriptUsage::ParticleEventScript, UsageId));
		EventObj->SetStringField(TEXT("execution_mode"), ExecutionModeToString(EventHandler.ExecutionMode));
		EventObj->SetNumberField(TEXT("spawn_number"), EventHandler.SpawnNumber);
		EventObj->SetNumberField(TEXT("min_spawn_number"), EventHandler.MinSpawnNumber);
		EventObj->SetBoolField(TEXT("random_spawn_number"), EventHandler.bRandomSpawnNumber);
		EventObj->SetNumberField(TEXT("max_events_per_frame"), EventHandler.MaxEventsPerFrame);
		EventObj->SetBoolField(TEXT("update_attribute_initial_values"), EventHandler.UpdateAttributeInitialValues);
		EventObj->SetStringField(TEXT("source_event_name"), EventHandler.SourceEventName.ToString());
		EventObj->SetStringField(TEXT("source_emitter_id"), EventHandler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
		if (EventHandler.Script)
		{
			EventObj->SetStringField(TEXT("script_object_path"), EventHandler.Script->GetPathName());
		}
		return EventObj;
	}

	static bool ResolveEventHandlerFromParams(FNiagaraEmitterEditContext& EditContext, const TSharedPtr<FJsonObject>& Params, int32& OutEventIndex, FNiagaraEventScriptProperties*& OutEventHandler, FString& OutError)
	{
		OutEventIndex = INDEX_NONE;
		OutEventHandler = nullptr;

		if (!EditContext.EmitterData)
		{
			OutError = TEXT("missing_emitter_version_data");
			return false;
		}

		TArray<FNiagaraEventScriptProperties>& EventHandlers = EditContext.EmitterData->EventHandlerScriptProps;
		if (EventHandlers.Num() == 0)
		{
			OutError = TEXT("emitter_has_no_event_handlers");
			return false;
		}

		double EventIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("event_index"), EventIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(EventIndexNumber);
			if (!EventHandlers.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_event_index");
				return false;
			}

			OutEventIndex = Index;
			OutEventHandler = &EventHandlers[Index];
			return true;
		}

		FString ScriptUsageIdText;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("script_usage_id"), ScriptUsageIdText) && !ScriptUsageIdText.IsEmpty())
		{
			FGuid ScriptUsageId;
			if (!FGuid::Parse(ScriptUsageIdText, ScriptUsageId))
			{
				OutError = TEXT("invalid_script_usage_id");
				return false;
			}

			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == ScriptUsageId)
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}

			OutError = TEXT("event_handler_not_found");
			return false;
		}

		FString SourceEventName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("source_event_name"), SourceEventName) && !SourceEventName.IsEmpty())
		{
			for (int32 Index = 0; Index < EventHandlers.Num(); ++Index)
			{
				FNiagaraEventScriptProperties& EventHandler = EventHandlers[Index];
				if (EventHandler.SourceEventName.ToString().Equals(SourceEventName, ESearchCase::IgnoreCase))
				{
					OutEventIndex = Index;
					OutEventHandler = &EventHandler;
					return true;
				}
			}
		}

		OutError = TEXT("event_handler_not_found");
		return false;
	}

	static UEdGraphNode* FindGraphNodeByGuid(UNiagaraGraph& Graph, const FGuid& NodeGuid)
	{
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool EnsureGraphNodeGuid(UEdGraphNode* Node, TSet<FGuid>* ExistingGuids)
	{
		if (!Node)
		{
			return false;
		}

		if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
		{
			if (ExistingGuids)
			{
				ExistingGuids->Add(Node->NodeGuid);
			}
			return false;
		}

		Node->Modify();
		for (int32 Attempt = 0; Attempt < 16; ++Attempt)
		{
			Node->CreateNewGuid();
			if (Node->NodeGuid.IsValid() && (!ExistingGuids || !ExistingGuids->Contains(Node->NodeGuid)))
			{
				if (ExistingGuids)
				{
					ExistingGuids->Add(Node->NodeGuid);
				}
				return true;
			}
		}
		return false;
	}

	static int32 NormalizeGraphNodeGuids(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return 0;
		}

		TSet<FGuid> ExistingGuids;
		int32 FixedCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (EnsureGraphNodeGuid(Node, &ExistingGuids))
			{
				++FixedCount;
			}
		}
		return FixedCount;
	}

	static UEdGraphPin* ResolvePinByName(UEdGraphNode& Node, const FString& PinName)
	{
		const FString Wanted = PinName.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinName.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static bool ResolveStageNodeFromParams(UNiagaraGraph& Graph, const TSharedPtr<FJsonObject>& Params, FResolvedNiagaraStage& OutStage, UEdGraphNode*& OutStageNode, FString& OutError)
	{
		OutStageNode = nullptr;

		if (!ResolveStageFromParams(Graph, Params, OutStage, OutError))
		{
			return false;
		}

		FString NodeGuidText;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("node_guid"), NodeGuidText) || NodeGuidText.IsEmpty())
		{
			OutError = TEXT("missing_node_guid");
			return false;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			OutError = TEXT("invalid_node_guid");
			return false;
		}

		TSet<UEdGraphNode*> VisitedNodes;
		TArray<UEdGraphNode*> StageNodes;
		GatherUpstreamNodesRecursive(OutStage.OutputNode, VisitedNodes, StageNodes);

		for (UEdGraphNode* StageNode : StageNodes)
		{
			if (StageNode && StageNode->NodeGuid == NodeGuid)
			{
				OutStageNode = StageNode;
				break;
			}
		}

		if (!OutStageNode)
		{
			OutError = TEXT("node_not_found_in_stage");
			return false;
		}

		return true;
	}

	static bool FindOwningOutputNodeForModule(UNiagaraGraph& Graph, UNiagaraNodeFunctionCall& ModuleNode, UNiagaraNodeOutput*& OutOutputNode)
	{
		OutOutputNode = nullptr;
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetStageModuleNodes(*OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* CandidateModule : Modules)
			{
				if (CandidateModule == &ModuleNode)
				{
					OutOutputNode = OutputNode;
					return true;
				}
			}
		}
		return false;
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

	static void DestroyGraphNodesForStage(UNiagaraGraph& Graph, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		TArray<UNiagaraNodeOutput*> OutputNodes;
		GetOutputNodesFromGraph(Graph, OutputNodes);
		for (UNiagaraNodeOutput* OutputNode : OutputNodes)
		{
			if (!OutputNode || OutputNode->GetUsage() != Usage || OutputNode->GetUsageId() != UsageId)
			{
				continue;
			}

			TSet<UEdGraphNode*> VisitedNodes;
			TArray<UEdGraphNode*> StageNodes;
			GatherUpstreamNodesRecursive(OutputNode, VisitedNodes, StageNodes);
			for (UEdGraphNode* StageNode : StageNodes)
			{
				if (!StageNode)
				{
					continue;
				}
				StageNode->Modify();
				StageNode->DestroyNode();
			}
			return;
		}
	}

	static UNiagaraScript* ResolveOwningScriptForStage(FVersionedNiagaraEmitterData& EmitterData, const ENiagaraScriptUsage Usage, const FGuid& UsageId)
	{
		if (Usage == ENiagaraScriptUsage::EmitterSpawnScript)
		{
			return EmitterData.EmitterSpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::EmitterUpdateScript)
		{
			return EmitterData.EmitterUpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
		{
			return EmitterData.SpawnScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleUpdateScript)
		{
			return EmitterData.UpdateScriptProps.Script;
		}
		if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			for (FNiagaraEventScriptProperties& EventHandler : EmitterData.EventHandlerScriptProps)
			{
				if (EventHandler.Script && EventHandler.Script->GetUsageId() == UsageId)
				{
					return EventHandler.Script;
				}
			}
			return nullptr;
		}
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			if (UNiagaraSimulationStageBase* SimulationStage = EmitterData.GetSimulationStageById(UsageId))
			{
				return SimulationStage->Script;
			}
			return nullptr;
		}
		return nullptr;
	}

	static FNiagaraSystemRefreshResult MarkEmitterGraphEdited(FNiagaraEmitterEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.Emitter)
		{
			Context.Emitter->MarkPackageDirty();

			if (UNiagaraSystem* OwningSystem = Context.Emitter->GetTypedOuter<UNiagaraSystem>())
			{
				RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*OwningSystem, true, true);
				FUeAgentInterfaceLogger::Log(
					TEXT("Niagara MarkEmitterGraphEdited: refreshed system=%s emitter=%s"),
					*OwningSystem->GetPathName(),
					*Context.Emitter->GetPathName());
			}
		}
		return RefreshResult;
	}

	static FNiagaraSystemRefreshResult MarkSystemGraphEdited(FNiagaraSystemEditContext& Context)
	{
		FNiagaraSystemRefreshResult RefreshResult;
		if (Context.Graph)
		{
			Context.Graph->NotifyGraphChanged();
		}
		if (Context.System)
		{
			Context.System->MarkPackageDirty();
			RefreshResult = RefreshNiagaraSystemAfterStructuralEdit(*Context.System, true, true);
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara MarkSystemGraphEdited: refreshed system=%s"),
				*Context.System->GetPathName());
		}
		return RefreshResult;
	}

	static int32 RepairSystemInvalidEmitterVersions(UNiagaraSystem& NiagaraSystem)
	{
		int32 RepairedCount = 0;
		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		for (FNiagaraEmitterHandle& Handle : Handles)
		{
			FVersionedNiagaraEmitter Instance = Handle.GetInstance();
			if (!Instance.Emitter)
			{
				continue;
			}

			const bool bVersionGuidInvalid = !Instance.Version.IsValid();
			const bool bMissingVersionData = Instance.GetEmitterData() == nullptr;
			if (!bVersionGuidInvalid && !bMissingVersionData)
			{
				continue;
			}

			const FGuid ExposedVersionGuid = Instance.Emitter->GetExposedVersion().VersionGuid;
			if (!ExposedVersionGuid.IsValid())
			{
				continue;
			}

			const bool bChangedByApi = NiagaraSystem.ChangeEmitterVersion(Instance, ExposedVersionGuid);
			if (!bChangedByApi)
			{
				FVersionedNiagaraEmitter& MutableInstance = Handle.GetInstance();
				MutableInstance.Version = ExposedVersionGuid;
			}

			++RepairedCount;
		}
		return RepairedCount;
	}

	static bool ResolveEmitterHandleFromParams(UNiagaraSystem& NiagaraSystem, const TSharedPtr<FJsonObject>& Params, int32& OutEmitterIndex, FNiagaraEmitterHandle*& OutEmitterHandle, FString& OutError)
	{
		OutEmitterIndex = INDEX_NONE;
		OutEmitterHandle = nullptr;

		TArray<FNiagaraEmitterHandle>& Handles = NiagaraSystem.GetEmitterHandles();
		if (Handles.Num() == 0)
		{
			OutError = TEXT("system_has_no_emitters");
			return false;
		}

		double HandleIndexNumber = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(TEXT("emitter_index"), HandleIndexNumber))
		{
			const int32 Index = FMath::RoundToInt(HandleIndexNumber);
			if (!Handles.IsValidIndex(Index))
			{
				OutError = TEXT("invalid_emitter_index");
				return false;
			}

			OutEmitterIndex = Index;
			OutEmitterHandle = &Handles[Index];
			return true;
		}

		FString EmitterIdString;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_id"), EmitterIdString) && !EmitterIdString.IsEmpty())
		{
			FGuid EmitterGuid;
			if (!FGuid::Parse(EmitterIdString, EmitterGuid))
			{
				OutError = TEXT("invalid_emitter_id");
				return false;
			}

			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetId() == EmitterGuid)
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}
			OutError = TEXT("emitter_not_found");
			return false;
		}

		FString EmitterName;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("emitter_name"), EmitterName) && !EmitterName.IsEmpty())
		{
			for (int32 Index = 0; Index < Handles.Num(); ++Index)
			{
				if (Handles[Index].GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
				{
					OutEmitterIndex = Index;
					OutEmitterHandle = &Handles[Index];
					return true;
				}
			}

			OutError = TEXT("emitter_not_found");
			return false;
		}

		OutError = TEXT("missing_emitter_identifier");
		return false;
	}
}


#include "UeAgentHttpServer_Niagara_Dispatch.inl"
#include "UeAgentHttpServer_Niagara_AssetsAndSystem.inl"
#include "UeAgentHttpServer_Niagara_UserAndEmitterParams.inl"
#include "UeAgentHttpServer_Niagara_RenderersAndEvents.inl"
#include "UeAgentHttpServer_Niagara_StageGraph.inl"
#include "UeAgentHttpServer_Niagara_FolderFormat.inl"
