	static UClass* ResolveClassByPath(const FString& InClassPath);
	static FString ScriptUsageToString(const ENiagaraScriptUsage Usage);
	static FString ScriptCompileStatusToString(const ENiagaraScriptCompileStatus Status);
	static FString MakeStageKey(const ENiagaraScriptUsage Usage, const FGuid& UsageId);
	static void GetStageModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModules);
	static FString GetModuleScriptPath(UNiagaraNodeFunctionCall& ModuleNode);
	static UEdGraphPin* FindOverridePinByAliasedInputName(UNiagaraGraph* Graph, const FName AliasedInputName);
	static bool EnsureGraphNodeGuid(UEdGraphNode* Node, TSet<FGuid>* ExistingGuids = nullptr);
	static int32 NormalizeGraphNodeGuids(UEdGraph* Graph);
	static FVector2D ResolveOffscreenDrawSize(const TSharedRef<SWidget>& Widget, const FString& Target, const TSharedPtr<FJsonObject>& Params, const int32 MaxSize);
	static bool ScreenshotSlateWidgetOffscreen(const TSharedRef<SWidget>& Widget, const FVector2D& DrawSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);

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

	struct FNiagaraCompileAfterSetResult
	{
		bool bCompileAfterSet = false;
		bool bForceCompile = true;
		bool bWaitForComplete = true;
		bool bCompilationRequested = false;
		bool bCompilationComplete = false;
		bool bHasOutstandingCompilationRequests = false;
	};

	static bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static FNiagaraCompileAfterSetResult ReadCompileAfterSetParams(const TSharedPtr<FJsonObject>& Params, const bool bDefaultCompileAfterSet)
	{
		FNiagaraCompileAfterSetResult Result;
		Result.bCompileAfterSet = bDefaultCompileAfterSet;
		TryGetBoolField(Params, TEXT("compile_after_set"), Result.bCompileAfterSet);
		TryGetBoolField(Params, TEXT("force_compile"), Result.bForceCompile);
		TryGetBoolField(Params, TEXT("wait_for_complete"), Result.bWaitForComplete);
		return Result;
	}

	static FNiagaraCompileAfterSetResult CompileSystemAfterSet(UNiagaraSystem* NiagaraSystem, const TSharedPtr<FJsonObject>& Params, const bool bDefaultCompileAfterSet, const bool bShouldCompile = true)
	{
		FNiagaraCompileAfterSetResult Result = ReadCompileAfterSetParams(Params, bDefaultCompileAfterSet);
		if (!Result.bCompileAfterSet || !bShouldCompile || !NiagaraSystem)
		{
			return Result;
		}

		Result.bCompilationRequested = NiagaraSystem->RequestCompile(Result.bForceCompile);
		if (Result.bWaitForComplete)
		{
			if (NiagaraSystem->HasOutstandingCompilationRequests(true))
			{
				NiagaraSystem->WaitForCompilationComplete(true, false);
			}
			NiagaraSystem->PollForCompilationComplete(true);
			Result.bHasOutstandingCompilationRequests = NiagaraSystem->HasOutstandingCompilationRequests(true);
			Result.bCompilationComplete = !Result.bHasOutstandingCompilationRequests;
			if (Result.bCompilationComplete)
			{
				NiagaraSystem->CacheFromCompiledData();
			}
		}
		else
		{
			Result.bHasOutstandingCompilationRequests = NiagaraSystem->HasOutstandingCompilationRequests(true);
		}
		return Result;
	}

	static FNiagaraCompileAfterSetResult CompileEmitterScriptsAfterSet(FVersionedNiagaraEmitterData* EmitterData, const TSharedPtr<FJsonObject>& Params, const bool bDefaultCompileAfterSet, const bool bShouldCompile = true)
	{
		FNiagaraCompileAfterSetResult Result = ReadCompileAfterSetParams(Params, bDefaultCompileAfterSet);
		if (!Result.bCompileAfterSet || !bShouldCompile || !EmitterData)
		{
			return Result;
		}

		TArray<UNiagaraScript*> EmitterScripts;
		EmitterData->GetScripts(EmitterScripts, false, false);
		for (UNiagaraScript* Script : EmitterScripts)
		{
			if (!Script)
			{
				continue;
			}
			Script->RequestCompile(Script->GetExposedVersion().VersionGuid, Result.bForceCompile);
			Result.bCompilationRequested = true;
		}
		Result.bCompilationComplete = true;
		return Result;
	}

	static void AppendCompileAfterSetFields(const TSharedPtr<FJsonObject>& OutData, const FNiagaraCompileAfterSetResult& Result)
	{
		if (!OutData.IsValid())
		{
			return;
		}
		OutData->SetBoolField(TEXT("compile_after_set"), Result.bCompileAfterSet);
		OutData->SetBoolField(TEXT("force_compile"), Result.bForceCompile);
		OutData->SetBoolField(TEXT("wait_for_complete"), Result.bWaitForComplete);
		OutData->SetBoolField(TEXT("compilation_requested"), Result.bCompilationRequested);
		OutData->SetBoolField(TEXT("compilation_complete"), Result.bCompilationComplete);
		OutData->SetBoolField(TEXT("has_outstanding_compilation_requests"), Result.bHasOutstandingCompilationRequests);
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
		if (Existing->IsRooted())
		{
			Existing->RemoveFromRoot();
		}
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

		FSlateApplication::Get().PumpMessages();
		const float WindowScale = FSlateApplication::Get().GetApplicationScale() * Window->GetDPIScaleFactor();
		Window->SlatePrepass(WindowScale);
		FSlateApplication::Get().InvalidateAllViewports();
		OutRedrawSkipReason = TEXT("legacy_backbuffer_redraw_disabled");
		return false;
	}

	static bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		const FVector2D DrawSize = ResolveOffscreenDrawSize(Widget, TEXT("window"), nullptr, 8192);
		return ScreenshotSlateWidgetOffscreen(Widget, DrawSize, OutPixels, OutSize, OutError);
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

	struct FNiagaraPreviewSceneCaptureContext
	{
		UWorld* World = nullptr;
		ASceneCapture2D* CaptureActor = nullptr;
		UTextureRenderTarget2D* RenderTarget = nullptr;
		FIntPoint CaptureSize = FIntPoint::ZeroValue;

		~FNiagaraPreviewSceneCaptureContext()
		{
			if (CaptureActor && IsValid(CaptureActor))
			{
				CaptureActor->Destroy();
			}
			if (RenderTarget)
			{
				RenderTarget->RemoveFromRoot();
			}
		}
	};

	static int32 GetNiagaraShaderCompileJobCount()
	{
		return GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
	}

	static bool IsNiagaraShaderCompiling()
	{
		return GShaderCompilingManager ? GShaderCompilingManager->IsCompiling() : false;
	}

	static TSharedPtr<FJsonObject> WaitForNiagaraPreviewRenderDependencies(UWorld* PreviewWorld, const TSharedPtr<FJsonObject>& Params)
	{
		bool bWaitForRenderDependencies = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("wait_for_render_dependencies"), bWaitForRenderDependencies) ||
				Params->TryGetBoolField(TEXT("wait_for_asset_compilation"), bWaitForRenderDependencies) ||
				Params->TryGetBoolField(TEXT("wait_for_shader_compilation"), bWaitForRenderDependencies);
		}

		TSharedPtr<FJsonObject> WaitInfo = MakeShared<FJsonObject>();
		WaitInfo->SetBoolField(TEXT("requested"), bWaitForRenderDependencies);
		WaitInfo->SetNumberField(TEXT("asset_compiling_before"), FAssetCompilingManager::Get().GetNumRemainingAssets());
		WaitInfo->SetNumberField(TEXT("shader_jobs_before"), GetNiagaraShaderCompileJobCount());
		WaitInfo->SetBoolField(TEXT("shader_compiling_before"), IsNiagaraShaderCompiling());
		WaitInfo->SetBoolField(TEXT("submitted_world_material_jobs"), false);
		WaitInfo->SetBoolField(TEXT("finished_asset_compilation"), false);
		WaitInfo->SetBoolField(TEXT("finished_shader_compilation"), false);

		if (bWaitForRenderDependencies)
		{
			if (PreviewWorld)
			{
				UMaterialInterface::SubmitRemainingJobsForWorld(PreviewWorld);
				WaitInfo->SetBoolField(TEXT("submitted_world_material_jobs"), true);
			}
			WaitInfo->SetNumberField(TEXT("asset_compiling_after_submit"), FAssetCompilingManager::Get().GetNumRemainingAssets());
			WaitInfo->SetNumberField(TEXT("shader_jobs_after_submit"), GetNiagaraShaderCompileJobCount());

			FAssetCompilingManager::Get().FinishAllCompilation();
			WaitInfo->SetBoolField(TEXT("finished_asset_compilation"), true);

			if (GShaderCompilingManager)
			{
				GShaderCompilingManager->FinishAllCompilation();
				WaitInfo->SetBoolField(TEXT("finished_shader_compilation"), true);
			}
			FlushRenderingCommands();
		}

		WaitInfo->SetNumberField(TEXT("asset_compiling_after"), FAssetCompilingManager::Get().GetNumRemainingAssets());
		WaitInfo->SetNumberField(TEXT("shader_jobs_after"), GetNiagaraShaderCompileJobCount());
		WaitInfo->SetBoolField(TEXT("shader_compiling_after"), IsNiagaraShaderCompiling());
		return WaitInfo;
	}

	static bool WarmupNiagaraPreviewSceneCapture(
		FNiagaraPreviewSceneCaptureContext& Context,
		UNiagaraComponent* PreviewComponent,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutWarmupInfo,
		FString& OutError)
	{
		if (!Context.CaptureActor || !IsValid(Context.CaptureActor))
		{
			OutError = TEXT("niagara_scene_capture_not_initialized");
			return false;
		}
		USceneCaptureComponent2D* CaptureComp = Context.CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = TEXT("niagara_scene_capture_component_missing");
			return false;
		}

		double WarmupCaptureValue = 1.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("scene_capture_warmup_captures"), WarmupCaptureValue) ||
				Params->TryGetNumberField(TEXT("render_warmup_captures"), WarmupCaptureValue);
		}
		const int32 WarmupCaptureCount = FMath::Clamp(FMath::RoundToInt(WarmupCaptureValue), 0, 8);

		OutWarmupInfo = MakeShared<FJsonObject>();
		OutWarmupInfo->SetNumberField(TEXT("warmup_capture_count"), WarmupCaptureCount);
		OutWarmupInfo->SetObjectField(TEXT("pre_wait"), WaitForNiagaraPreviewRenderDependencies(Context.World, Params));

		TArray<TSharedPtr<FJsonValue>> WarmupWaits;
		for (int32 WarmupIndex = 0; WarmupIndex < WarmupCaptureCount; ++WarmupIndex)
		{
			if (PreviewComponent)
			{
				PreviewComponent->MarkRenderStateDirty();
				if (UWorld* PreviewWorld = PreviewComponent->GetWorld())
				{
					PreviewWorld->SendAllEndOfFrameUpdates();
					if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(PreviewWorld))
					{
						WorldManager->FlushComputeAndDeferredQueues(false);
					}
				}
			}
			CaptureComp->CaptureScene();
			FlushRenderingCommands();
			WarmupWaits.Add(MakeShared<FJsonValueObject>(WaitForNiagaraPreviewRenderDependencies(Context.World, Params)));
		}
		OutWarmupInfo->SetArrayField(TEXT("post_warmup_waits"), WarmupWaits);
		return true;
	}

	static FIntPoint ResolveNiagaraPreviewSceneCaptureSize(const FVector2D& PreferredSize, const TSharedPtr<FJsonObject>& Params, const int32 MaxSize)
	{
		double RequestedWidth = 0.0;
		double RequestedHeight = 0.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("width"), RequestedWidth) || Params->TryGetNumberField(TEXT("capture_width"), RequestedWidth) || Params->TryGetNumberField(TEXT("offscreen_width"), RequestedWidth);
			Params->TryGetNumberField(TEXT("height"), RequestedHeight) || Params->TryGetNumberField(TEXT("capture_height"), RequestedHeight) || Params->TryGetNumberField(TEXT("offscreen_height"), RequestedHeight);
		}

		FVector2D CaptureSize = PreferredSize;
		if (RequestedWidth > 0.0 && RequestedHeight > 0.0)
		{
			CaptureSize = FVector2D(RequestedWidth, RequestedHeight);
		}
		if (CaptureSize.X <= 0.0 || CaptureSize.Y <= 0.0)
		{
			CaptureSize = FVector2D(1280.0, 720.0);
		}

		const double MaxDimension = static_cast<double>(FMath::Clamp(MaxSize, 64, 8192));
		if (CaptureSize.X > MaxDimension || CaptureSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / CaptureSize.X, MaxDimension / CaptureSize.Y);
			CaptureSize *= Scale;
		}
		return FIntPoint(
			FMath::Clamp(FMath::RoundToInt(CaptureSize.X), 64, FMath::Clamp(MaxSize, 64, 8192)),
			FMath::Clamp(FMath::RoundToInt(CaptureSize.Y), 64, FMath::Clamp(MaxSize, 64, 8192)));
	}

	static TSharedPtr<FJsonObject> ResolveNiagaraPreviewSceneCaptureCamera(
		UNiagaraComponent* PreviewComponent,
		const TSharedPtr<FJsonObject>& Params,
		FVector& OutLocation,
		FRotator& OutRotation,
		float& OutFov)
	{
		TSharedPtr<FJsonObject> CameraInfo = MakeShared<FJsonObject>();

		double FovValue = 45.0;
		double DistanceValue = 0.0;
		double DistanceScaleValue = 2.8;
		double TargetZOffsetValue = 0.0;
		double HeightOffsetValue = 0.0;
		double SideOffsetValue = 0.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("camera_fov"), FovValue) || Params->TryGetNumberField(TEXT("capture_fov"), FovValue);
			Params->TryGetNumberField(TEXT("camera_distance"), DistanceValue) || Params->TryGetNumberField(TEXT("capture_distance"), DistanceValue);
			Params->TryGetNumberField(TEXT("camera_distance_scale"), DistanceScaleValue) || Params->TryGetNumberField(TEXT("capture_distance_scale"), DistanceScaleValue);
			Params->TryGetNumberField(TEXT("camera_target_z_offset"), TargetZOffsetValue);
			Params->TryGetNumberField(TEXT("camera_height_offset"), HeightOffsetValue);
			Params->TryGetNumberField(TEXT("camera_side_offset"), SideOffsetValue);
		}

		OutFov = FMath::Clamp(static_cast<float>(FovValue), 5.0f, 170.0f);
		FVector BoundsOrigin = PreviewComponent ? PreviewComponent->GetComponentLocation() : FVector::ZeroVector;
		double BoundsRadius = 180.0;
		bool bUsedComponentBounds = false;
		if (PreviewComponent)
		{
			PreviewComponent->UpdateComponentToWorld();
			PreviewComponent->UpdateBounds();
			const FBoxSphereBounds Bounds = PreviewComponent->Bounds;
			if (Bounds.SphereRadius > KINDA_SMALL_NUMBER && Bounds.Origin.ContainsNaN() == false)
			{
				BoundsOrigin = Bounds.Origin;
				BoundsRadius = FMath::Max(static_cast<double>(Bounds.SphereRadius), 180.0);
				bUsedComponentBounds = true;
			}
		}

		const double Distance = DistanceValue > 0.0 ? DistanceValue : FMath::Max(BoundsRadius * FMath::Max(DistanceScaleValue, 0.1), 320.0);
		const FVector Target = BoundsOrigin + FVector(0.0, 0.0, TargetZOffsetValue);
		const double HeightOffset = HeightOffsetValue != 0.0 ? HeightOffsetValue : BoundsRadius * 0.2;
		const double SideOffset = SideOffsetValue != 0.0 ? SideOffsetValue : BoundsRadius * 0.12;
		OutLocation = Target + FVector(-Distance, SideOffset, HeightOffset);
		const FVector ViewDirection = (Target - OutLocation).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		OutRotation = FRotationMatrix::MakeFromX(ViewDirection).Rotator();

		CameraInfo->SetBoolField(TEXT("used_component_bounds"), bUsedComponentBounds);
		CameraInfo->SetNumberField(TEXT("bounds_origin_x"), BoundsOrigin.X);
		CameraInfo->SetNumberField(TEXT("bounds_origin_y"), BoundsOrigin.Y);
		CameraInfo->SetNumberField(TEXT("bounds_origin_z"), BoundsOrigin.Z);
		CameraInfo->SetNumberField(TEXT("bounds_radius"), BoundsRadius);
		CameraInfo->SetNumberField(TEXT("camera_location_x"), OutLocation.X);
		CameraInfo->SetNumberField(TEXT("camera_location_y"), OutLocation.Y);
		CameraInfo->SetNumberField(TEXT("camera_location_z"), OutLocation.Z);
		CameraInfo->SetNumberField(TEXT("camera_rotation_pitch"), OutRotation.Pitch);
		CameraInfo->SetNumberField(TEXT("camera_rotation_yaw"), OutRotation.Yaw);
		CameraInfo->SetNumberField(TEXT("camera_rotation_roll"), OutRotation.Roll);
		CameraInfo->SetNumberField(TEXT("camera_fov"), OutFov);
		CameraInfo->SetNumberField(TEXT("camera_distance"), Distance);
		return CameraInfo;
	}

	static bool InitializeNiagaraPreviewSceneCapture(
		UNiagaraComponent* PreviewComponent,
		const FIntPoint& CaptureSize,
		const TSharedPtr<FJsonObject>& Params,
		FNiagaraPreviewSceneCaptureContext& InOutContext,
		TSharedPtr<FJsonObject>& OutCameraInfo,
		FString& OutError)
	{
		if (!PreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return false;
		}
		UWorld* PreviewWorld = PreviewComponent->GetWorld();
		if (!PreviewWorld)
		{
			OutError = TEXT("niagara_preview_world_not_found");
			return false;
		}
		if (CaptureSize.X <= 0 || CaptureSize.Y <= 0)
		{
			OutError = TEXT("niagara_scene_capture_size_invalid");
			return false;
		}

		InOutContext.World = PreviewWorld;
		InOutContext.CaptureSize = CaptureSize;
		InOutContext.RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!InOutContext.RenderTarget)
		{
			OutError = TEXT("niagara_scene_capture_render_target_alloc_failed");
			return false;
		}
		InOutContext.RenderTarget->AddToRoot();
		InOutContext.RenderTarget->ClearColor = FLinearColor::Black;
		InOutContext.RenderTarget->InitCustomFormat(CaptureSize.X, CaptureSize.Y, PF_B8G8R8A8, false);
		InOutContext.RenderTarget->UpdateResourceImmediate(true);

		FVector CaptureLocation = FVector::ZeroVector;
		FRotator CaptureRotation = FRotator::ZeroRotator;
		float CaptureFov = 45.0f;
		OutCameraInfo = ResolveNiagaraPreviewSceneCaptureCamera(PreviewComponent, Params, CaptureLocation, CaptureRotation, CaptureFov);

		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(PreviewWorld, ASceneCapture2D::StaticClass(), TEXT("UeAgentNiagaraFrameCapture"));
		SpawnParams.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.bTemporaryEditorActor = true;
		SpawnParams.bHideFromSceneOutliner = true;
		InOutContext.CaptureActor = PreviewWorld->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), CaptureLocation, CaptureRotation, SpawnParams);
		if (!InOutContext.CaptureActor)
		{
			OutError = TEXT("niagara_scene_capture_spawn_failed");
			return false;
		}
		InOutContext.CaptureActor->SetActorEnableCollision(false);
		InOutContext.CaptureActor->SetActorHiddenInGame(true);
		InOutContext.CaptureActor->SetIsTemporarilyHiddenInEditor(true);

		USceneCaptureComponent2D* CaptureComp = InOutContext.CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = TEXT("niagara_scene_capture_component_missing");
			return false;
		}
		CaptureComp->TextureTarget = InOutContext.RenderTarget;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->CaptureSource = SCS_FinalColorLDR;
		CaptureComp->FOVAngle = CaptureFov;
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		CaptureComp->ClearShowOnlyComponents();
		CaptureComp->ShowOnlyComponent(PreviewComponent);
		CaptureComp->ShowFlags.SetMotionBlur(false);
		CaptureComp->ShowFlags.SetParticles(true);
		CaptureComp->ShowFlags.SetNiagara(true);
		return true;
	}

	static bool CaptureNiagaraPreviewSceneFrame(FNiagaraPreviewSceneCaptureContext& Context, UNiagaraComponent* PreviewComponent, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
	{
		if (!Context.CaptureActor || !IsValid(Context.CaptureActor) || !Context.RenderTarget)
		{
			OutError = TEXT("niagara_scene_capture_not_initialized");
			return false;
		}
		if (PreviewComponent)
		{
			PreviewComponent->MarkRenderStateDirty();
			if (UWorld* PreviewWorld = PreviewComponent->GetWorld())
			{
				PreviewWorld->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(PreviewWorld))
				{
					WorldManager->FlushComputeAndDeferredQueues(false);
				}
			}
		}

		USceneCaptureComponent2D* CaptureComp = Context.CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = TEXT("niagara_scene_capture_component_missing");
			return false;
		}
		CaptureComp->CaptureScene();
		FlushRenderingCommands();

		FRenderTarget* RenderTarget = Context.RenderTarget->GameThread_GetRenderTargetResource();
		if (!RenderTarget)
		{
			OutError = TEXT("niagara_scene_capture_render_target_resource_missing");
			return false;
		}
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(false);
		if (!RenderTarget->ReadPixels(OutPixels, ReadFlags) || OutPixels.Num() <= 0)
		{
			OutError = TEXT("niagara_scene_capture_read_pixels_failed");
			return false;
		}
		for (FColor& Pixel : OutPixels)
		{
			Pixel.A = 255;
		}
		OutSize = Context.CaptureSize;
		return true;
	}

	static uint64 ComputePixelSignature(const TArray<FColor>& Pixels)
	{
		uint64 Hash = 1469598103934665603ull;
		for (const FColor& Pixel : Pixels)
		{
			Hash ^= static_cast<uint64>(Pixel.R);
			Hash *= 1099511628211ull;
			Hash ^= static_cast<uint64>(Pixel.G);
			Hash *= 1099511628211ull;
			Hash ^= static_cast<uint64>(Pixel.B);
			Hash *= 1099511628211ull;
			Hash ^= static_cast<uint64>(Pixel.A);
			Hash *= 1099511628211ull;
		}
		return Hash;
	}

	static int32 CountPixelDifferences(const TArray<FColor>& A, const TArray<FColor>& B)
	{
		if (A.Num() != B.Num())
		{
			return INDEX_NONE;
		}
		int32 DifferenceCount = 0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index] != B[Index])
			{
				++DifferenceCount;
			}
		}
		return DifferenceCount;
	}

	static TSharedPtr<FJsonObject> BuildPixelSignalStats(const TArray<FColor>& Pixels)
	{
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		const int32 PixelCount = Pixels.Num();
		int32 NonBlackPixelCount = 0;
		int32 BrightPixelCount = 0;
		int32 ColoredPixelCount = 0;
		int32 FireLikePixelCount = 0;
		double LumaSum = 0.0;

		for (const FColor& Pixel : Pixels)
		{
			const int32 R = Pixel.R;
			const int32 G = Pixel.G;
			const int32 B = Pixel.B;
			const int32 MaxChannel = FMath::Max3(R, G, B);
			const int32 MinChannel = FMath::Min3(R, G, B);
			if (MaxChannel > 8)
			{
				++NonBlackPixelCount;
			}
			if (MaxChannel > 220)
			{
				++BrightPixelCount;
			}
			if (MaxChannel - MinChannel > 35)
			{
				++ColoredPixelCount;
			}
			if (R > 160 && G > 45 && B < 140 && R > B + 40)
			{
				++FireLikePixelCount;
			}
			LumaSum += 0.2126 * static_cast<double>(R) + 0.7152 * static_cast<double>(G) + 0.0722 * static_cast<double>(B);
		}

		const uint64 Signature = ComputePixelSignature(Pixels);
		Stats->SetStringField(TEXT("pixel_signature"), FString::Printf(TEXT("%016llX"), static_cast<unsigned long long>(Signature)));
		Stats->SetNumberField(TEXT("pixel_count"), PixelCount);
		Stats->SetNumberField(TEXT("non_black_pixel_count"), NonBlackPixelCount);
		Stats->SetNumberField(TEXT("bright_pixel_count"), BrightPixelCount);
		Stats->SetNumberField(TEXT("colored_pixel_count"), ColoredPixelCount);
		Stats->SetNumberField(TEXT("fire_like_pixel_count"), FireLikePixelCount);
		Stats->SetNumberField(TEXT("average_luma"), PixelCount > 0 ? LumaSum / static_cast<double>(PixelCount) : 0.0);
		return Stats;
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

	static constexpr const TCHAR* UeAgentNiagaraPreviewStateTagPrefix = TEXT("UAI_NiagaraPreviewState=");

	static bool ResolveNiagaraSystemPreviewComponent(UNiagaraSystem* NiagaraSystem, const bool bOpenEditorIfNeeded, UNiagaraComponent*& OutPreviewComponent, TSharedPtr<FNiagaraSystemViewModel>& OutSystemViewModel, FString& OutError)
	{
		OutPreviewComponent = nullptr;
		OutSystemViewModel.Reset();

		if (!NiagaraSystem)
		{
			OutError = TEXT("asset_is_not_niagara_system");
			return false;
		}

		UObject* Asset = NiagaraSystem;
		IAssetEditorInstance* AssetEditor = GetNiagaraAssetEditor(Asset, bOpenEditorIfNeeded, OutError);
		if (!AssetEditor)
		{
			return false;
		}

		FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
		if (!NiagaraEditorModule)
		{
			OutError = TEXT("niagara_editor_module_not_loaded");
			return false;
		}

		OutSystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
		if (!OutSystemViewModel.IsValid())
		{
			OutError = TEXT("niagara_system_view_model_not_found");
			return false;
		}

		OutPreviewComponent = OutSystemViewModel->GetPreviewComponent();
		if (!OutPreviewComponent)
		{
			OutError = TEXT("niagara_preview_component_not_found");
			return false;
		}
		return true;
	}

	static void SetNiagaraPreviewStateToken(UNiagaraComponent* PreviewComponent, const FString& Token)
	{
		if (!PreviewComponent)
		{
			return;
		}

		for (int32 Index = PreviewComponent->ComponentTags.Num() - 1; Index >= 0; --Index)
		{
			if (PreviewComponent->ComponentTags[Index].ToString().StartsWith(UeAgentNiagaraPreviewStateTagPrefix))
			{
				PreviewComponent->ComponentTags.RemoveAt(Index);
			}
		}

		if (!Token.IsEmpty())
		{
			PreviewComponent->ComponentTags.AddUnique(FName(*(FString(UeAgentNiagaraPreviewStateTagPrefix) + Token)));
		}
	}

	static FString GetNiagaraPreviewStateToken(const UNiagaraComponent* PreviewComponent)
	{
		if (!PreviewComponent)
		{
			return FString();
		}

		for (const FName& TagName : PreviewComponent->ComponentTags)
		{
			const FString Tag = TagName.ToString();
			if (Tag.StartsWith(UeAgentNiagaraPreviewStateTagPrefix))
			{
				return Tag.RightChop(FCString::Strlen(UeAgentNiagaraPreviewStateTagPrefix));
			}
		}
		return FString();
	}

	static FString MakeNiagaraPreviewStateToken(const UNiagaraSystem* NiagaraSystem, const UNiagaraComponent* PreviewComponent, const int32 FrameIndex, const float TickDeltaSeconds, const FString& AdvanceMode)
	{
		const FString AssetPath = NiagaraSystem ? NiagaraSystem->GetOutermost()->GetName() : FString();
		const FString ComponentPath = PreviewComponent ? PreviewComponent->GetPathName() : FString();
		const FString WorldPath = (PreviewComponent && PreviewComponent->GetWorld()) ? PreviewComponent->GetWorld()->GetPathName() : FString();
		return FString::Printf(
			TEXT("asset=%s;component=%s;world=%s;frame=%d;dt=%.9f;mode=%s;semantics=continuous_from_zero;nonce=%s"),
			*AssetPath,
			*ComponentPath,
			*WorldPath,
			FrameIndex,
			TickDeltaSeconds,
			*AdvanceMode,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	static bool GetNiagaraComponentRuntimePosition(UNiagaraComponent* PreviewComponent, const float TickDeltaSeconds, double& OutSystemAge, int32& OutSystemTickCount, int32& OutFrameEstimate)
	{
		OutSystemAge = -1.0;
		OutSystemTickCount = INDEX_NONE;
		OutFrameEstimate = INDEX_NONE;
		if (!PreviewComponent)
		{
			return false;
		}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
		FNiagaraSystemInstance* SystemInstance = PreviewComponent->GetSystemInstance();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!SystemInstance)
		{
			return false;
		}

		OutSystemAge = SystemInstance->GetAge();
		OutSystemTickCount = SystemInstance->GetTickCount();
		OutFrameEstimate = TickDeltaSeconds > SMALL_NUMBER ? FMath::RoundToInt(static_cast<float>(OutSystemAge) / TickDeltaSeconds) : INDEX_NONE;
		return true;
	}

	static bool ValidateNiagaraPreviewStateRequest(const TSharedPtr<FJsonObject>& Params, UNiagaraComponent* PreviewComponent, const float TickDeltaSeconds, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}

		const FString CurrentToken = GetNiagaraPreviewStateToken(PreviewComponent);
		OutData->SetStringField(TEXT("preview_state_token"), CurrentToken);

		FString ExpectedToken;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("expected_preview_state_token"), ExpectedToken) && !ExpectedToken.IsEmpty())
		{
			const bool bTokenMatched = CurrentToken == ExpectedToken;
			OutData->SetStringField(TEXT("expected_preview_state_token"), ExpectedToken);
			OutData->SetBoolField(TEXT("preview_state_token_matched"), bTokenMatched);
			if (!bTokenMatched)
			{
				OutError = TEXT("preview_state_token_mismatch");
				return false;
			}
		}

		double SystemAge = -1.0;
		int32 SystemTickCount = INDEX_NONE;
		int32 FrameEstimate = INDEX_NONE;
		const bool bHasRuntimePosition = GetNiagaraComponentRuntimePosition(PreviewComponent, TickDeltaSeconds, SystemAge, SystemTickCount, FrameEstimate);
		OutData->SetBoolField(TEXT("has_runtime_position"), bHasRuntimePosition);
		OutData->SetNumberField(TEXT("current_system_age"), SystemAge);
		OutData->SetNumberField(TEXT("current_system_tick_count"), SystemTickCount);
		OutData->SetNumberField(TEXT("current_frame_estimate"), FrameEstimate);
		OutData->SetNumberField(TEXT("frame_tick_delta_seconds"), TickDeltaSeconds);
		OutData->SetBoolField(TEXT("component_paused"), PreviewComponent ? PreviewComponent->IsPaused() : false);

		bool bRequirePaused = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("require_paused"), bRequirePaused);
		}
		OutData->SetBoolField(TEXT("require_paused"), bRequirePaused);
		if (bRequirePaused && PreviewComponent && !PreviewComponent->IsPaused())
		{
			OutError = TEXT("preview_component_not_paused");
			return false;
		}

		double ExpectedFrameNumber = 0.0;
		if (Params.IsValid() && (Params->TryGetNumberField(TEXT("expected_frame"), ExpectedFrameNumber) || Params->TryGetNumberField(TEXT("target_frame"), ExpectedFrameNumber)))
		{
			const int32 ExpectedFrame = FMath::RoundToInt(ExpectedFrameNumber);
			OutData->SetNumberField(TEXT("expected_frame"), ExpectedFrame);
			OutData->SetBoolField(TEXT("expected_frame_matched"), FrameEstimate == ExpectedFrame);
			if (!bHasRuntimePosition || FrameEstimate != ExpectedFrame)
			{
				OutError = TEXT("preview_frame_mismatch");
				return false;
			}
		}

		return true;
	}

	static bool TryGetBoolAnyField(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& Keys, bool& InOutValue)
	{
		if (!Params.IsValid())
		{
			return false;
		}

		for (const FString& Key : Keys)
		{
			bool Candidate = false;
			if (Params->TryGetBoolField(Key, Candidate))
			{
				InOutValue = Candidate;
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> RestoreNiagaraPreviewForInteractiveEditing(UNiagaraSystem* NiagaraSystem, TSharedPtr<FNiagaraSystemViewModel> SystemViewModel)
	{
		TSharedPtr<FJsonObject> Restore = MakeShared<FJsonObject>();
		Restore->SetBoolField(TEXT("requested"), true);
		Restore->SetBoolField(TEXT("restored"), false);

		if (!NiagaraSystem)
		{
			Restore->SetStringField(TEXT("error"), TEXT("asset_is_not_niagara_system"));
			return Restore;
		}

		if (!SystemViewModel.IsValid())
		{
			if (FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor")))
			{
				SystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
			}
		}

		if (!SystemViewModel.IsValid())
		{
			Restore->SetStringField(TEXT("error"), TEXT("niagara_system_view_model_not_found"));
			return Restore;
		}

		UNiagaraComponent* PreviewComponent = SystemViewModel->GetPreviewComponent();
		if (!PreviewComponent)
		{
			Restore->SetStringField(TEXT("error"), TEXT("niagara_preview_component_not_found"));
			return Restore;
		}

		const bool bWasPaused = PreviewComponent->IsPaused();
		const bool bWasActive = PreviewComponent->IsActive();
		const ENiagaraAgeUpdateMode PreviousAgeUpdateMode = PreviewComponent->GetAgeUpdateMode();
		const float PreviousSeekDelta = PreviewComponent->GetSeekDelta();
		const bool bPreviousLockDesiredAgeDeltaTimeToSeekDelta = PreviewComponent->GetLockDesiredAgeDeltaTimeToSeekDelta();
		const FString PreviousToken = GetNiagaraPreviewStateToken(PreviewComponent);
		const float PlaybackInterval = static_cast<float>(SystemViewModel->GetEditorData().GetPlaybackFrameRate().AsInterval());
		const bool bLockPlaybackFrameRate = SystemViewModel->GetEditorData().GetLockPlaybackFrameRate();

		PreviewComponent->SetAsset(NiagaraSystem, false);
		PreviewComponent->SetForceSolo(true);
		// Leave the editor preview in live playback mode; DesiredAge keeps it in seek mode and can look frozen.
		PreviewComponent->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
		PreviewComponent->SetSeekDelta(PlaybackInterval > SMALL_NUMBER ? PlaybackInterval : (1.0f / 60.0f));
		PreviewComponent->SetLockDesiredAgeDeltaTimeToSeekDelta(bLockPlaybackFrameRate);
		PreviewComponent->SetCanRenderWhileSeeking(false);
		PreviewComponent->SetAllowScalability(false);
		PreviewComponent->SetRenderingEnabled(true);
		SetNiagaraPreviewStateToken(PreviewComponent, FString());
		PreviewComponent->SetPaused(false);
		PreviewComponent->Activate(true);
		PreviewComponent->MarkRenderStateDirty();
		FlushNiagaraPreviewWorld(PreviewComponent);
		FEditorSupportDelegates::RedrawAllViewports.Broadcast();

		Restore->SetBoolField(TEXT("restored"), true);
		Restore->SetBoolField(TEXT("was_paused"), bWasPaused);
		Restore->SetBoolField(TEXT("is_paused"), PreviewComponent->IsPaused());
		Restore->SetBoolField(TEXT("was_active"), bWasActive);
		Restore->SetBoolField(TEXT("is_active"), PreviewComponent->IsActive());
		Restore->SetStringField(TEXT("previous_age_update_mode"), NiagaraEnumToString(PreviousAgeUpdateMode));
		Restore->SetStringField(TEXT("age_update_mode"), NiagaraEnumToString(PreviewComponent->GetAgeUpdateMode()));
		Restore->SetStringField(TEXT("restore_policy"), TEXT("live_tick_delta_time"));
		Restore->SetNumberField(TEXT("previous_seek_delta"), PreviousSeekDelta);
		Restore->SetNumberField(TEXT("seek_delta"), PreviewComponent->GetSeekDelta());
		Restore->SetBoolField(TEXT("previous_lock_desired_age_delta_time_to_seek_delta"), bPreviousLockDesiredAgeDeltaTimeToSeekDelta);
		Restore->SetBoolField(TEXT("lock_desired_age_delta_time_to_seek_delta"), PreviewComponent->GetLockDesiredAgeDeltaTimeToSeekDelta());
		Restore->SetBoolField(TEXT("previous_preview_state_token_present"), !PreviousToken.IsEmpty());
		Restore->SetStringField(TEXT("preview_state_token"), GetNiagaraPreviewStateToken(PreviewComponent));
		return Restore;
	}

	struct FNiagaraScreenshotFrameRequest
	{
		int32 FrameIndex = 0;
	};

	static bool TryGetNiagaraNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Key : Keys)
		{
			if (Obj->TryGetNumberField(Key, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool ResolveNiagaraScreenshotFrameSequence(const TSharedPtr<FJsonObject>& Params, TArray<FNiagaraScreenshotFrameRequest>& OutFrames, bool& bOutFrameSequenceRequested, FString& OutError)
	{
		OutFrames.Reset();
		bOutFrameSequenceRequested = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const bool bHasFrameSequenceSignals =
			Params->HasField(TEXT("frames")) ||
			Params->HasField(TEXT("count")) ||
			Params->HasField(TEXT("frame_count")) ||
			Params->HasField(TEXT("sample_count")) ||
			Params->HasField(TEXT("start_frame")) ||
			Params->HasField(TEXT("start_frame_index")) ||
			Params->HasField(TEXT("first_frame_index")) ||
			Params->HasField(TEXT("frame_interval")) ||
			Params->HasField(TEXT("frame_stride")) ||
			Params->HasField(TEXT("target_frame")) ||
			Params->HasField(TEXT("frame_index")) ||
			Params->HasField(TEXT("frame"));
		if (!bHasFrameSequenceSignals)
		{
			return true;
		}
		bOutFrameSequenceRequested = true;

		const TArray<TSharedPtr<FJsonValue>>* ExplicitFrames = nullptr;
		if (Params->TryGetArrayField(TEXT("frames"), ExplicitFrames) && ExplicitFrames)
		{
			if (ExplicitFrames->Num() < 1 || ExplicitFrames->Num() > 120)
			{
				OutError = TEXT("invalid_frame_count");
				return false;
			}

			int32 PreviousFrame = INDEX_NONE;
			for (int32 Index = 0; Index < ExplicitFrames->Num(); ++Index)
			{
				double FrameValue = 0.0;
				if ((*ExplicitFrames)[Index]->Type == EJson::Number)
				{
					FrameValue = (*ExplicitFrames)[Index]->AsNumber();
				}
				else if ((*ExplicitFrames)[Index]->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> FrameObject = (*ExplicitFrames)[Index]->AsObject();
					if (!TryGetNiagaraNumberAnyField(FrameObject, { TEXT("target_frame"), TEXT("frame_index"), TEXT("frame") }, FrameValue))
					{
						OutError = TEXT("invalid_frame_entry");
						return false;
					}
				}
				else
				{
					OutError = TEXT("invalid_frame_entry");
					return false;
				}

				const int32 FrameIndex = FMath::RoundToInt(FrameValue);
				if (FrameIndex < 0 || FrameIndex > 14400)
				{
					OutError = TEXT("invalid_frame_index");
					return false;
				}
				if (PreviousFrame != INDEX_NONE && FrameIndex < PreviousFrame)
				{
					OutError = TEXT("niagara_frame_sequence_must_be_non_decreasing");
					return false;
				}
				PreviousFrame = FrameIndex;

				FNiagaraScreenshotFrameRequest Request;
				Request.FrameIndex = FrameIndex;
				OutFrames.Add(Request);
			}
			return true;
		}

		double CountValue = 1.0;
		TryGetNiagaraNumberAnyField(Params, { TEXT("count"), TEXT("frame_count"), TEXT("sample_count") }, CountValue);
		const int32 RequestedCount = FMath::RoundToInt(CountValue);
		if (RequestedCount < 1 || RequestedCount > 120)
		{
			OutError = TEXT("invalid_frame_count");
			return false;
		}

		double StartFrameValue = 0.0;
		if (!TryGetNiagaraNumberAnyField(Params, { TEXT("start_frame"), TEXT("start_frame_index"), TEXT("first_frame_index"), TEXT("target_frame"), TEXT("frame_index"), TEXT("frame") }, StartFrameValue))
		{
			StartFrameValue = 0.0;
		}

		double FrameIntervalValue = 1.0;
		TryGetNiagaraNumberAnyField(Params, { TEXT("frame_interval"), TEXT("frame_stride") }, FrameIntervalValue);
		if (FrameIntervalValue < 0.0)
		{
			OutError = TEXT("invalid_frame_interval");
			return false;
		}

		const int32 StartFrame = FMath::RoundToInt(StartFrameValue);
		const int32 FrameInterval = FMath::Max(0, FMath::RoundToInt(FrameIntervalValue));
		for (int32 Index = 0; Index < RequestedCount; ++Index)
		{
			const int32 FrameIndex = StartFrame + FrameInterval * Index;
			if (FrameIndex < 0 || FrameIndex > 14400)
			{
				OutError = TEXT("invalid_frame_index");
				return false;
			}
			FNiagaraScreenshotFrameRequest Request;
			Request.FrameIndex = FrameIndex;
			OutFrames.Add(Request);
		}
		return true;
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
