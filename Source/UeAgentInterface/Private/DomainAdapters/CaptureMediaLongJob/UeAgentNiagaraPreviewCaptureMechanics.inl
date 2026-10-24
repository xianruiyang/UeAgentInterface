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
		int32 ClosedEditorCount = 0;
		UE::UAI::Contract::FDiagnostic Error;
		if (!::UeAgentNiagaraOps::PrepareAssetForStructuralNiagaraEdit(Asset, ClosedEditorCount, Error))
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Niagara structural edit editor isolation failed: asset=%s error=%s"),
				*Asset.GetPathName(),
				*Error);
		}
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
			if (!UE::UAI::AssetPreview::OpenAssetEditor(Asset))
			{
				OutError = TEXT("niagara_editor_open_failed");
				return nullptr;
			}
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
			UE::UAI::EditorCapture::WidgetTree::FindAllByType(Window, TypeName, CandidateWidgets);
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
		UE::UAI::EditorCapture::WidgetTree::FindAllByType(ViewportWidget, FName(TEXT("SViewport")), ViewportChildren);
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
		const FVector2D DrawSize = ResolveOffscreenDrawSize(Widget, TEXT("window"), nullptr, MAX_int32);
		return ScreenshotSlateWidgetOffscreen(Widget, DrawSize, OutPixels, OutSize, OutError);
	}

	static FVector2D ResolveOffscreenDrawSize(const TSharedRef<SWidget>& Widget, const FString& Target, const TSharedPtr<FJsonObject>& Params, const int32 MaxSize)
	{
		const FVector2D CachedSize = Widget->GetCachedGeometry().GetLocalSize();
		if (Target == TEXT("viewport") && CachedSize.X >= 1.0 && CachedSize.Y >= 1.0)
		{
			const double UpperBound = MaxSize > 0
				? static_cast<double>(MaxSize)
				: static_cast<double>(MAX_int32);
			return FVector2D(
				FMath::Clamp(FMath::RoundToDouble(CachedSize.X), 1.0, UpperBound),
				FMath::Clamp(FMath::RoundToDouble(CachedSize.Y), 1.0, UpperBound));
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

		const double MaxDimension = MaxSize > 0
			? static_cast<double>(MaxSize)
			: static_cast<double>(MAX_int32);
		if (DrawSize.X > MaxDimension || DrawSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / DrawSize.X, MaxDimension / DrawSize.Y);
			DrawSize *= Scale;
		}
		DrawSize.X = FMath::Clamp(FMath::RoundToDouble(DrawSize.X), 1.0, MaxDimension);
		DrawSize.Y = FMath::Clamp(FMath::RoundToDouble(DrawSize.Y), 1.0, MaxDimension);
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
		if (!FMath::IsFinite(WarmupCaptureValue) || WarmupCaptureValue < 0.0
            || WarmupCaptureValue > static_cast<double>(MAX_int32))
        {
            OutError = TEXT("scene_capture_warmup_captures_invalid");
            return false;
        }
        const int32 WarmupCaptureCount = FMath::RoundToInt(WarmupCaptureValue);

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

		const double MaxDimension = MaxSize > 0
			? static_cast<double>(MaxSize)
			: static_cast<double>(MAX_int32);
		if (CaptureSize.X > MaxDimension || CaptureSize.Y > MaxDimension)
		{
			const double Scale = FMath::Min(MaxDimension / CaptureSize.X, MaxDimension / CaptureSize.Y);
			CaptureSize *= Scale;
		}
		return FIntPoint(
			FMath::Clamp(FMath::RoundToInt(CaptureSize.X), 1, static_cast<int32>(MaxDimension)),
			FMath::Clamp(FMath::RoundToInt(CaptureSize.Y), 1, static_cast<int32>(MaxDimension)));
	}

	static TSharedPtr<FJsonObject> ResolveNiagaraPreviewSceneCaptureCamera(
		UNiagaraComponent* PreviewComponent,
		const TSharedPtr<FJsonObject>& Params,
		FVector& OutLocation,
		FRotator& OutRotation,
		float& OutFov)
	{
		TSharedPtr<FJsonObject> CameraInfo = MakeShared<FJsonObject>();
		auto TryReadTriple = [](const TSharedPtr<FJsonObject>& Object, const FString& Field, FVector& OutValue) -> bool
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->Num() != 3)
			{
				return false;
			}
			double Components[3] = { 0.0, 0.0, 0.0 };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(Components[Index]) || !FMath::IsFinite(Components[Index]))
				{
					return false;
				}
			}
			OutValue = FVector(Components[0], Components[1], Components[2]);
			return true;
		};

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
		const TSharedPtr<FJsonObject>* ExplicitCamera = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("camera"), ExplicitCamera) && ExplicitCamera && ExplicitCamera->IsValid())
		{
			FVector Translation;
			FVector RotationComponents;
			const bool bHasTranslation = TryReadTriple(*ExplicitCamera, TEXT("translation"), Translation)
				|| TryReadTriple(*ExplicitCamera, TEXT("location"), Translation);
			const bool bHasRotation = TryReadTriple(*ExplicitCamera, TEXT("rotation"), RotationComponents);
			if (bHasTranslation && bHasRotation)
			{
				(*ExplicitCamera)->TryGetNumberField(TEXT("fov"), FovValue);
				OutLocation = Translation;
				OutRotation = FRotator(RotationComponents.X, RotationComponents.Y, RotationComponents.Z);
				OutFov = FMath::Clamp(static_cast<float>(FovValue), 5.0f, 170.0f);
				CameraInfo->SetStringField(TEXT("source"), TEXT("explicit_world_camera"));
				CameraInfo->SetNumberField(TEXT("camera_location_x"), OutLocation.X);
				CameraInfo->SetNumberField(TEXT("camera_location_y"), OutLocation.Y);
				CameraInfo->SetNumberField(TEXT("camera_location_z"), OutLocation.Z);
				CameraInfo->SetNumberField(TEXT("camera_rotation_pitch"), OutRotation.Pitch);
				CameraInfo->SetNumberField(TEXT("camera_rotation_yaw"), OutRotation.Yaw);
				CameraInfo->SetNumberField(TEXT("camera_rotation_roll"), OutRotation.Roll);
				CameraInfo->SetNumberField(TEXT("camera_fov"), OutFov);
				return CameraInfo;
			}
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
		CameraInfo->SetStringField(TEXT("source"), TEXT("component_bounds_derived"));
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
			return UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::MakeShotPath(Format);
		}
		if (FPaths::IsRelative(OutPath))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutPath);
		}
		return OutPath;
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

	static bool ResolveNiagaraScreenshotFrameSequence(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FNiagaraScreenshotFrameRequest>& OutFrames,
		bool& bOutFrameSequenceRequested,
		FString& OutError)
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
			if (ExplicitFrames->Num() < 1)
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

				if (!FMath::IsFinite(FrameValue) || FrameValue < 0.0 || FrameValue > static_cast<double>(MAX_int32))
				{
					OutError = TEXT("invalid_frame_index");
					return false;
				}
				const int64 FrameIndexValue = FMath::RoundToInt64(FrameValue);
				const int32 FrameIndex = static_cast<int32>(FrameIndexValue);
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
		if (!FMath::IsFinite(CountValue) || CountValue < 1.0 || CountValue > static_cast<double>(MAX_int32))
		{
			OutError = TEXT("invalid_frame_count");
			return false;
		}
		const int64 RequestedCountValue = FMath::RoundToInt64(CountValue);
		const int32 RequestedCount = static_cast<int32>(RequestedCountValue);

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

		if (!FMath::IsFinite(StartFrameValue)
			|| StartFrameValue < 0.0
			|| StartFrameValue > static_cast<double>(MAX_int32)
			|| !FMath::IsFinite(FrameIntervalValue)
			|| FrameIntervalValue > static_cast<double>(MAX_int32))
		{
			OutError = TEXT("invalid_frame_index");
			return false;
		}
		const int64 StartFrame = FMath::RoundToInt64(StartFrameValue);
		const int64 FrameInterval = FMath::RoundToInt64(FrameIntervalValue);
		for (int32 Index = 0; Index < RequestedCount; ++Index)
		{
			const int64 FrameIndexValue = StartFrame + FrameInterval * Index;
			if (FrameIndexValue < 0
				|| FrameIndexValue > MAX_int32)
			{
				OutError = TEXT("invalid_frame_index");
				return false;
			}
			const int32 FrameIndex = static_cast<int32>(FrameIndexValue);
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
	static TSharedPtr<FJsonObject> CollectNiagaraComponentStats(
		UNiagaraSystem* NiagaraSystem,
		UNiagaraComponent* PreviewComponent,
		FString& OutError,
		const UeAgentNiagaraRuntimeParticleCapture::FOptions* ParticleCaptureOptions = nullptr)
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
			if (ParticleCaptureOptions && ParticleCaptureOptions->IsEnabled())
			{
				EmitterObj->SetObjectField(
					TEXT("particle_attribute_capture"),
					UeAgentNiagaraRuntimeParticleCapture::Capture(
						EmitterInstance, *ParticleCaptureOptions));
			}
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
