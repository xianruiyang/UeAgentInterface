bool FUeAgentHttpServer::ExecuteScreenshotViewport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	// If called on the game thread, fall back to the synchronous implementation.
	// (Waiting for Slate redraw on the game thread would deadlock.)
	if (IsInGameThread())
	{
		return CmdScreenshotViewport(Ctx, OutData, OutArtifacts, OutError);
	}

	// Parse params on the calling thread.
	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 1024;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QD = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QD))
	{
		Quality = FMath::Clamp((int32)QD, 1, 100);
	}
	double MSD = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MSD))
	{
		MaxSize = FMath::Clamp((int32)MSD, 64, 8192);
	}

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	struct FShotJob
	{
		FString Format;
		int32 Quality = 85;
		int32 MaxSize = 1024;

		FViewport* Viewport = nullptr;
		FDelegateHandle PostTickHandle;
		FEvent* DoneEvent = nullptr;

		std::atomic_bool bFinished{ false };

		bool bOk = false;
		FString Error;
		FString FilePath;
		int32 Width = 0;
		int32 Height = 0;
		int64 Bytes = 0;
	};

	TSharedRef<FShotJob, ESPMode::ThreadSafe> Job = MakeShared<FShotJob, ESPMode::ThreadSafe>();
	Job->Format = Format;
	Job->Quality = Quality;
	Job->MaxSize = MaxSize;
	Job->DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);

	// Schedule capture on the game thread, but wait on this thread.
	AsyncTask(ENamedThreads::GameThread, [Job]()
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		if (!Settings || !Settings->bEnableScreenshots)
		{
			Job->Error = TEXT("screenshots_disabled");
			if (!Job->bFinished.exchange(true))
			{
				Job->DoneEvent->Trigger();
			}
			return;
		}

		FLevelEditorViewportClient* LVC = GCurrentLevelEditingViewportClient;
		if (!LVC || !LVC->Viewport)
		{
			Job->Error = TEXT("no_level_editor_viewport");
			if (!Job->bFinished.exchange(true))
			{
				Job->DoneEvent->Trigger();
			}
			return;
		}

		Job->Viewport = LVC->Viewport;

		// Request a redraw; we'll capture on the next Slate post-tick (after DrawWindows).
		LVC->Invalidate();
		Job->Viewport->Invalidate();

		if (!FSlateApplication::IsInitialized())
		{
			// Fallback: capture immediately (may be stale if the editor hasn't redrawn yet).
			TArray<FColor> Pixels;
			FIntPoint Size(0, 0);
			FString Err;
			if (!TakeActiveViewportScreenshot(Pixels, Size, Err))
			{
				Job->Error = Err;
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			TArray<FColor> Resized;
			FIntPoint ResizedSize(0, 0);
			if (!ResizePixelsMaxSize(Pixels, Size, Job->MaxSize, Resized, ResizedSize, Err))
			{
				Job->Error = Err;
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			const FString OutPath = MakeShotPath(Job->Format);
			if (!WriteCompressedImage(Resized, ResizedSize, Job->Format, Job->Quality, OutPath, Err))
			{
				Job->Error = Err;
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			Job->FilePath = OutPath;
			Job->Width = ResizedSize.X;
			Job->Height = ResizedSize.Y;
			Job->Bytes = IFileManager::Get().FileSize(*OutPath);
			Job->bOk = true;
			if (!Job->bFinished.exchange(true))
			{
				Job->DoneEvent->Trigger();
			}
			return;
		}

		Job->PostTickHandle = FSlateApplication::Get().OnPostTick().AddLambda([Job](float)
		{
			// One-shot capture.
			if (FSlateApplication::IsInitialized() && Job->PostTickHandle.IsValid())
			{
				FSlateApplication::Get().OnPostTick().Remove(Job->PostTickHandle);
			}

			// If the job already finished (e.g. timed out/canceled), do nothing.
			if (Job->bFinished.load())
			{
				return;
			}

			if (!Job->Viewport)
			{
				Job->Error = TEXT("no_level_editor_viewport");
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			// Ensure the render thread has completed the draw before we read back pixels.
			FlushRenderingCommands();

			const FIntPoint Size = Job->Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
			{
				Job->Error = TEXT("invalid_viewport_size");
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			TArray<FColor> Pixels;
			Pixels.AddUninitialized(Size.X * Size.Y);
			if (!Job->Viewport->ReadPixels(Pixels))
			{
				Job->Error = TEXT("read_pixels_failed");
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			TArray<FColor> Resized;
			FIntPoint ResizedSize(0, 0);
			FString Err;
			if (!ResizePixelsMaxSize(Pixels, Size, Job->MaxSize, Resized, ResizedSize, Err))
			{
				Job->Error = Err;
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			const FString OutPath = MakeShotPath(Job->Format);
			if (!WriteCompressedImage(Resized, ResizedSize, Job->Format, Job->Quality, OutPath, Err))
			{
				Job->Error = Err;
				if (!Job->bFinished.exchange(true))
				{
					Job->DoneEvent->Trigger();
				}
				return;
			}

			Job->FilePath = OutPath;
			Job->Width = ResizedSize.X;
			Job->Height = ResizedSize.Y;
			Job->Bytes = IFileManager::Get().FileSize(*OutPath);
			Job->bOk = true;
			if (!Job->bFinished.exchange(true))
			{
				Job->DoneEvent->Trigger();
			}
		});
	});

	bool bDone = Job->DoneEvent->Wait(FTimespan::FromSeconds(5));
	if (!bDone)
	{
		// Mark finished to prevent any later post-tick delegate from trying to signal a recycled event.
		Job->Error = TEXT("screenshot_timeout");
		Job->bFinished.exchange(true);

		// Best-effort cleanup (no blocking wait here).
		AsyncTask(ENamedThreads::GameThread, [Job]()
		{
			if (FSlateApplication::IsInitialized() && Job->PostTickHandle.IsValid())
			{
				FSlateApplication::Get().OnPostTick().Remove(Job->PostTickHandle);
			}
		});
	}

	FPlatformProcess::ReturnSynchEventToPool(Job->DoneEvent);
	Job->DoneEvent = nullptr;

	if (!Job->bOk)
	{
		OutError = Job->Error.IsEmpty() ? TEXT("screenshot_failed") : Job->Error;
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("file_path"), Job->FilePath);
	OutData->SetNumberField(TEXT("width"), Job->Width);
	OutData->SetNumberField(TEXT("height"), Job->Height);
	OutData->SetNumberField(TEXT("bytes"), (double)Job->Bytes);
	OutData->SetStringField(TEXT("format"), Job->Format);

	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
	Artifact->SetStringField(TEXT("file_path"), Job->FilePath);
	Artifact->SetNumberField(TEXT("bytes"), (double)Job->Bytes);
	Artifact->SetStringField(TEXT("mime"), Job->Format == TEXT("png") ? TEXT("image/png") : (Job->Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), Job->Width);
	Artifact->SetNumberField(TEXT("height"), Job->Height);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	return true;
}

bool FUeAgentHttpServer::CmdScreenshotViewport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 1024;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QD = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QD))
	{
		Quality = FMath::Clamp((int32)QD, 1, 100);
	}
	double MSD = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MSD))
	{
		MaxSize = FMath::Clamp((int32)MSD, 64, 8192);
	}

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s format=%s"), *Ctx.RequestId, *OutError, *Format);
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s begin format=%s quality=%d max_size=%d"), *Ctx.RequestId, *Format, Quality, MaxSize);

	TArray<FColor> Pixels;
	FIntPoint Size(0, 0);
	if (!TakeActiveViewportScreenshot(Pixels, Size, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	TArray<FColor> Resized;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, Size, MaxSize, Resized, ResizedSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Resized, ResizedSize, Format, Quality, OutPath, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);

	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	OutData->SetStringField(TEXT("format"), Format);

	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
	Artifact->SetStringField(TEXT("file_path"), OutPath);
	Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
	Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), ResizedSize.X);
	Artifact->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s ok path=%s size=%dx%d bytes=%lld"),
		*Ctx.RequestId,
		*OutPath,
		ResizedSize.X,
		ResizedSize.Y,
		(long long)Bytes);

	return true;
}

bool FUeAgentHttpServer::CmdScreenshotViewportBuffer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC || !LVC->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	FString Buffer = TEXT("SceneDepth");
	JsonTryGetString(Ctx.Params, TEXT("buffer"), Buffer);
	Buffer.TrimStartAndEndInline();
	if (Buffer.IsEmpty())
	{
		OutError = TEXT("missing_buffer");
		return false;
	}

	const FString BufferLower = Buffer.ToLower();
	if (BufferLower == TEXT("scene_depth") || BufferLower == TEXT("scene-depth") || BufferLower == TEXT("scenedepth") || BufferLower == TEXT("depth"))
	{
		Buffer = TEXT("SceneDepth");
	}

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 1024;
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QD = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QD))
	{
		Quality = FMath::Clamp((int32)QD, 1, 100);
	}
	double MSD = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MSD))
	{
		MaxSize = FMath::Clamp((int32)MSD, 64, 8192);
	}

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s format=%s"), *Ctx.RequestId, *OutError, *Format);
		return false;
	}

	// Prefer a capture-path that doesn't rely on editor debug viewmode shaders, so we can always produce a usable depth/normal/basecolor buffer.
	enum class ECaptureBufferKind : uint8
	{
		None,
		SceneDepth,
		DeviceDepth,
		BaseColor,
		WorldNormal,
	};

	ECaptureBufferKind CaptureKind = ECaptureBufferKind::None;
	ESceneCaptureSource CaptureSource = SCS_MAX;

	const FString BufferLower2 = Buffer.ToLower();
	if (BufferLower2 == TEXT("scene_depth") || BufferLower2 == TEXT("scene-depth") || BufferLower2 == TEXT("scenedepth") || BufferLower2 == TEXT("scenedepthworldunits") || BufferLower2 == TEXT("scene_depth_world_units") || BufferLower2 == TEXT("scene-depth-world-units") || BufferLower2 == TEXT("depth"))
	{
		CaptureKind = ECaptureBufferKind::SceneDepth;
		CaptureSource = SCS_SceneDepth;
		Buffer = TEXT("SceneDepth");
	}
	else if (BufferLower2 == TEXT("device_depth") || BufferLower2 == TEXT("device-depth") || BufferLower2 == TEXT("devicedepth"))
	{
		CaptureKind = ECaptureBufferKind::DeviceDepth;
		CaptureSource = SCS_DeviceDepth;
		Buffer = TEXT("DeviceDepth");
	}
	else if (BufferLower2 == TEXT("basecolor") || BufferLower2 == TEXT("base_color") || BufferLower2 == TEXT("base-color"))
	{
		CaptureKind = ECaptureBufferKind::BaseColor;
		CaptureSource = SCS_BaseColor;
		Buffer = TEXT("BaseColor");
	}
	else if (BufferLower2 == TEXT("worldnormal") || BufferLower2 == TEXT("world_normal") || BufferLower2 == TEXT("world-normal") || BufferLower2 == TEXT("normal"))
	{
		CaptureKind = ECaptureBufferKind::WorldNormal;
		CaptureSource = SCS_Normal;
		Buffer = TEXT("WorldNormal");
	}

	auto TryCaptureViaSceneCapture = [&](TArray<FColor>& OutPixels, FIntPoint& OutSize) -> bool
	{
		if (CaptureKind == ECaptureBufferKind::None || CaptureSource == SCS_MAX)
		{
			OutError = TEXT("unsupported_buffer");
			return false;
		}
		if (!GEditor)
		{
			OutError = TEXT("no_geditor");
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = TEXT("no_editor_world");
			return false;
		}

		const FIntPoint ViewportSize = LVC->Viewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		const int32 MaxDim = FMath::Max(ViewportSize.X, ViewportSize.Y);
		const float Scale = (MaxDim > MaxSize) ? ((float)MaxSize / (float)MaxDim) : 1.0f;
		const int32 TargetX = FMath::Max(1, (int32)FMath::RoundToInt((float)ViewportSize.X * Scale));
		const int32 TargetY = FMath::Max(1, (int32)FMath::RoundToInt((float)ViewportSize.Y * Scale));
		OutSize = FIntPoint(TargetX, TargetY);

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!RenderTarget)
		{
			OutError = TEXT("render_target_alloc_failed");
			return false;
		}
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->InitCustomFormat(TargetX, TargetY, PF_FloatRGBA, /*bForceLinearGamma*/ true);
		RenderTarget->UpdateResourceImmediate(true);

		const FVector CamLocation = LVC->GetViewLocation();
		const FRotator CamRotation = LVC->GetViewRotation();

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags = RF_Transient;

		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), CamLocation, CamRotation, SpawnParams);
		if (!CaptureActor)
		{
			OutError = TEXT("spawn_scene_capture_failed");
			return false;
		}

		struct FCaptureActorGuard
		{
			ASceneCapture2D* Actor = nullptr;
			~FCaptureActorGuard()
			{
				if (Actor && IsValid(Actor) && !Actor->IsActorBeingDestroyed())
				{
					Actor->Destroy();
				}
			}
		};

		FCaptureActorGuard Guard;
		Guard.Actor = CaptureActor;

		CaptureActor->SetFlags(RF_Transient);
		CaptureActor->SetActorHiddenInGame(true);
		CaptureActor->SetIsTemporarilyHiddenInEditor(true);

		USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = TEXT("scene_capture_component_missing");
			return false;
		}

		CaptureComp->TextureTarget = RenderTarget;
		CaptureComp->CaptureSource = CaptureSource;
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->FOVAngle = LVC->ViewFOV;
		CaptureComp->CaptureScene();
		FlushRenderingCommands();

		FRenderTarget* Target = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Target)
		{
			OutError = TEXT("render_target_resource_missing");
			return false;
		}

		TArray<FLinearColor> LinearPixels;
		LinearPixels.AddUninitialized(TargetX * TargetY);
		if (!Target->ReadLinearColorPixels(LinearPixels))
		{
			OutError = TEXT("read_render_target_failed");
			return false;
		}

		OutPixels.Reset();
		OutPixels.AddUninitialized(TargetX * TargetY);

		if (CaptureKind == ECaptureBufferKind::SceneDepth || CaptureKind == ECaptureBufferKind::DeviceDepth)
		{
			// NOTE:
			// - `SCS_SceneDepth` returns linear scene depth in world units (cm) in R.
			// - Using a fixed `depth_far_cm` can easily produce near-black images when the viewport camera is far away.
			//   So we default to an auto-range mapping (percentiles) unless the user explicitly provides `depth_far_cm`.
			bool bInvert = true;
			JsonTryGetBool(Ctx.Params, TEXT("invert"), bInvert);

			float DepthNearCm = 0.0f;
			float DepthFarCm = 10000.0f;
			FString DepthMode;

			double DepthNearCmD = 0.0;
			double DepthFarCmD = 0.0;
			const bool bHasDepthNear = JsonTryGetNumber(Ctx.Params, TEXT("depth_near_cm"), DepthNearCmD);
			const bool bHasDepthFar = (CaptureKind == ECaptureBufferKind::SceneDepth) ? JsonTryGetNumber(Ctx.Params, TEXT("depth_far_cm"), DepthFarCmD) : false;
			JsonTryGetString(Ctx.Params, TEXT("depth_mode"), DepthMode);
			DepthMode.TrimStartAndEndInline();
			DepthMode = DepthMode.ToLower();

			const auto ApplyFixedDepthRange = [&]()
			{
				DepthNearCm = bHasDepthNear ? FMath::Max(0.0f, (float)DepthNearCmD) : 0.0f;
				const float FixedFar = bHasDepthFar ? (float)DepthFarCmD : 10000.0f;
				DepthFarCm = FMath::Max(DepthNearCm + 1.0f, FixedFar);
				OutData->SetStringField(TEXT("debug_depth_mode"), TEXT("fixed"));
				OutData->SetNumberField(TEXT("debug_depth_near_cm"), (double)DepthNearCm);
				OutData->SetNumberField(TEXT("debug_depth_far_cm"), (double)DepthFarCm);
			};

			const auto TryApplyAutoDepthRange = [&]() -> bool
			{
				// DeviceDepth is already 0..1; auto-range only makes sense for SceneDepth (cm).
				if (CaptureKind != ECaptureBufferKind::SceneDepth)
				{
					return false;
				}

				double PctLowD = 2.0;
				double PctHighD = 98.0;
				JsonTryGetNumber(Ctx.Params, TEXT("depth_auto_pct_low"), PctLowD);
				JsonTryGetNumber(Ctx.Params, TEXT("depth_auto_pct_high"), PctHighD);
				const float PctLow = FMath::Clamp((float)PctLowD, 0.0f, 49.0f);
				const float PctHigh = FMath::Clamp((float)PctHighD, 51.0f, 100.0f);

				const int32 NumPixels = LinearPixels.Num();
				const int32 MaxSamples = 50000;
				const int32 Step = FMath::Max(1, NumPixels / MaxSamples);

				TArray<float> Samples;
				Samples.Reserve(FMath::Min(MaxSamples, NumPixels));

				int32 SkippedSaturated = 0;
				for (int32 i = 0; i < NumPixels; i += Step)
				{
					const float D = LinearPixels[i].R;
					if (!FMath::IsFinite(D) || D <= 0.0f)
					{
						continue;
					}
					// `PF_FloatRGBA` is typically half-float; `SCS_SceneDepth` can saturate at ~65504cm in large/empty views.
					// Treat saturated values as "invalid/sky/far" so auto-range is driven by real geometry depths.
					if (D >= 65000.0f)
					{
						SkippedSaturated++;
						continue;
					}
					// Filter out extreme outliers (e.g., sky/invalid depth) to keep auto-range useful.
					if (D > 10000000.0f) // 100 km in cm
					{
						continue;
					}
					Samples.Add(D);
				}

				if (Samples.Num() < 16)
				{
					return false;
				}

				Samples.Sort();
				const int32 Last = Samples.Num() - 1;
				const int32 IdxLow = FMath::Clamp((int32)FMath::RoundToInt((PctLow / 100.0f) * (float)Last), 0, Last);
				const int32 IdxHigh = FMath::Clamp((int32)FMath::RoundToInt((PctHigh / 100.0f) * (float)Last), 0, Last);

				DepthNearCm = bHasDepthNear ? FMath::Max(0.0f, (float)DepthNearCmD) : Samples[IdxLow];
				DepthFarCm = Samples[IdxHigh];
				DepthFarCm = FMath::Max(DepthNearCm + 1.0f, DepthFarCm);

				OutData->SetStringField(TEXT("debug_depth_mode"), TEXT("auto_percentile"));
				OutData->SetNumberField(TEXT("debug_depth_near_cm"), (double)DepthNearCm);
				OutData->SetNumberField(TEXT("debug_depth_far_cm"), (double)DepthFarCm);
				OutData->SetNumberField(TEXT("debug_depth_samples"), (double)Samples.Num());
				OutData->SetNumberField(TEXT("debug_depth_auto_skipped_saturated"), (double)SkippedSaturated);
				OutData->SetNumberField(TEXT("debug_depth_auto_pct_low"), (double)PctLow);
				OutData->SetNumberField(TEXT("debug_depth_auto_pct_high"), (double)PctHigh);
				return true;
			};

			// Decide mapping mode:
			// - If user provides `depth_far_cm`, keep the old fixed-far behavior (reproducible).
			// - Otherwise default to auto-percentile unless explicitly requested `fixed`.
			bool bAuto = (CaptureKind == ECaptureBufferKind::SceneDepth) && !bHasDepthFar;
			if (DepthMode == TEXT("fixed") || DepthMode == TEXT("fixed_far") || DepthMode == TEXT("far"))
			{
				bAuto = false;
			}
			else if (DepthMode == TEXT("auto") || DepthMode == TEXT("auto_percentile") || DepthMode == TEXT("percentile"))
			{
				bAuto = true;
			}

			if (!bAuto || !TryApplyAutoDepthRange())
			{
				ApplyFixedDepthRange();
			}

			uint8 MinG = 255;
			uint8 MaxG = 0;
			int32 CountNearBlack = 0;
			int32 CountNearWhite = 0;
			const int32 NumOutPixels = LinearPixels.Num();

			for (int32 i = 0; i < LinearPixels.Num(); ++i)
			{
				const FLinearColor P = LinearPixels[i];
				const float DepthValue = (CaptureKind == ECaptureBufferKind::SceneDepth) ? P.R : P.R; // DeviceDepth provides depth in RGB; use R.
				float T = 0.0f;
				if (CaptureKind == ECaptureBufferKind::DeviceDepth)
				{
					T = FMath::Clamp(DepthValue, 0.0f, 1.0f);
				}
				else
				{
					const float Range = FMath::Max(1.0f, DepthFarCm - DepthNearCm);
					T = FMath::Clamp((DepthValue - DepthNearCm) / Range, 0.0f, 1.0f);
				}
				if (bInvert)
				{
					T = 1.0f - T;
				}
				const uint8 G = (uint8)FMath::Clamp((int32)FMath::RoundToInt(T * 255.0f), 0, 255);
				OutPixels[i] = FColor(G, G, G, 255);
				MinG = FMath::Min(MinG, G);
				MaxG = FMath::Max(MaxG, G);
				if (G <= 1)
				{
					CountNearBlack++;
				}
				else if (G >= 254)
				{
					CountNearWhite++;
				}
			}

			OutData->SetNumberField(TEXT("debug_depth_min_u8"), (double)MinG);
			OutData->SetNumberField(TEXT("debug_depth_max_u8"), (double)MaxG);
			OutData->SetNumberField(TEXT("debug_depth_pct_near_black"), NumOutPixels > 0 ? ((double)CountNearBlack / (double)NumOutPixels) : 1.0);
			OutData->SetNumberField(TEXT("debug_depth_pct_near_white"), NumOutPixels > 0 ? ((double)CountNearWhite / (double)NumOutPixels) : 0.0);
			if ((int32)MaxG - (int32)MinG <= 2)
			{
				OutData->SetBoolField(TEXT("warning_low_contrast"), true);
			}
			if (NumOutPixels > 0 && (double)CountNearBlack / (double)NumOutPixels >= 0.98)
			{
				OutData->SetStringField(TEXT("warning_depth_distribution"), TEXT("mostly_black"));
			}
			else if (NumOutPixels > 0 && (double)CountNearWhite / (double)NumOutPixels >= 0.98)
			{
				OutData->SetStringField(TEXT("warning_depth_distribution"), TEXT("mostly_white"));
			}
		}
		else
		{
			for (int32 i = 0; i < LinearPixels.Num(); ++i)
			{
				OutPixels[i] = LinearPixels[i].ToFColorSRGB();
			}
		}

		return true;
	};

	if (CaptureKind != ECaptureBufferKind::None)
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s begin(scene_capture) buffer=%s format=%s quality=%d max_size=%d"),
			*Ctx.RequestId,
			*Buffer,
			*Format,
			Quality,
			MaxSize);

		TArray<FColor> Captured;
		FIntPoint CapturedSize(0, 0);
		if (!TryCaptureViaSceneCapture(Captured, CapturedSize))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed(scene_capture): %s"), *Ctx.RequestId, *OutError);
			return false;
		}

		const FString OutPath = MakeShotPath(Format);
		if (!WriteCompressedImage(Captured, CapturedSize, Format, Quality, OutPath, OutError))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
			return false;
		}

		const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
		OutData->SetStringField(TEXT("file_path"), OutPath);
		OutData->SetNumberField(TEXT("width"), CapturedSize.X);
		OutData->SetNumberField(TEXT("height"), CapturedSize.Y);
		OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
		OutData->SetStringField(TEXT("format"), Format);
		OutData->SetStringField(TEXT("buffer"), Buffer);
		OutData->SetStringField(TEXT("method"), TEXT("scene_capture"));

		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
		Artifact->SetStringField(TEXT("file_path"), OutPath);
		Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
		Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
		Artifact->SetNumberField(TEXT("width"), CapturedSize.X);
		Artifact->SetNumberField(TEXT("height"), CapturedSize.Y);
		Artifact->SetStringField(TEXT("buffer"), Buffer);
		OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s ok(scene_capture) path=%s buffer=%s size=%dx%d bytes=%lld"),
			*Ctx.RequestId,
			*OutPath,
			*Buffer,
			CapturedSize.X,
			CapturedSize.Y,
			(long long)Bytes);

		return true;
	}

	const EViewModeIndex PrevViewMode = LVC->GetViewMode();
	const FName PrevBufferMode = LVC->CurrentBufferVisualizationMode;

	struct FRestoreViewportDebugView
	{
		FLevelEditorViewportClient* Client = nullptr;
		EViewModeIndex PreviousViewMode = VMI_Lit;
		FName PreviousBufferMode = NAME_None;

		~FRestoreViewportDebugView()
		{
			if (Client)
			{
				// NOTE: `ChangeBufferVisualizationMode` will force `VMI_VisualizeBuffer`.
				// When restoring a non-buffer view mode (Lit/Unlit/Wireframe/...), we must not re-enter buffer mode.
				if (PreviousViewMode == VMI_VisualizeBuffer)
				{
					Client->ChangeBufferVisualizationMode(PreviousBufferMode);
				}
				else
				{
					Client->SetViewMode(PreviousViewMode);
					Client->CurrentBufferVisualizationMode = PreviousBufferMode;
				}
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}
			}
		}
	};

	FRestoreViewportDebugView Restore;
	Restore.Client = LVC;
	Restore.PreviousViewMode = PrevViewMode;
	Restore.PreviousBufferMode = PrevBufferMode;

	LVC->SetViewMode(VMI_VisualizeBuffer);
	LVC->ChangeBufferVisualizationMode(FName(*Buffer));

	OutData->SetStringField(TEXT("debug_prev_view_mode"), FString::Printf(TEXT("%d"), (int32)PrevViewMode));
	OutData->SetStringField(TEXT("debug_prev_buffer_mode"), PrevBufferMode.ToString());
	OutData->SetStringField(TEXT("debug_applied_view_mode"), FString::Printf(TEXT("%d"), (int32)LVC->GetViewMode()));
	OutData->SetStringField(TEXT("debug_applied_buffer_mode"), LVC->CurrentBufferVisualizationMode.ToString());
	LVC->Invalidate();
	LVC->Viewport->Invalidate();

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s begin buffer=%s format=%s quality=%d max_size=%d"),
		*Ctx.RequestId,
		*Buffer,
		*Format,
		Quality,
		MaxSize);

	TArray<FColor> Pixels;
	FIntPoint Size(0, 0);
	{
		FViewport* Viewport = LVC->Viewport;
		const FText ScreenshotRealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "ScreenshotRealtimeOverrideSystem", "UeAgentInterfaceScreenshot");
		const bool bNeedTemporaryRealtime = !LVC->IsRealtime();
		if (bNeedTemporaryRealtime)
		{
			if (LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
			{
				LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
			}
			LVC->AddRealtimeOverride(true, ScreenshotRealtimeOverrideSystem);
		}

		Viewport->Invalidate();
		Viewport->Draw(/*bShouldPresent*/ true);
		FlushRenderingCommands();

		if (bNeedTemporaryRealtime && LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
		{
			LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
		}

		const FIntPoint LocalSize = Viewport->GetSizeXY();
		if (LocalSize.X <= 0 || LocalSize.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
			return false;
		}

		Pixels.AddUninitialized(LocalSize.X * LocalSize.Y);
		if (!Viewport->ReadPixels(Pixels))
		{
			OutError = TEXT("read_pixels_failed");
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
			return false;
		}

		Size = LocalSize;
	}

	TArray<FColor> Resized;
	FIntPoint ResizedSize(0, 0);
	if (!ResizePixelsMaxSize(Pixels, Size, MaxSize, Resized, ResizedSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Resized, ResizedSize, Format, Quality, OutPath, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError);
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);

	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("buffer"), Buffer);
	OutData->SetStringField(TEXT("method"), TEXT("buffer_visualization"));

	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
	Artifact->SetStringField(TEXT("file_path"), OutPath);
	Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
	Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), ResizedSize.X);
	Artifact->SetNumberField(TEXT("height"), ResizedSize.Y);
	Artifact->SetStringField(TEXT("buffer"), Buffer);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s ok path=%s buffer=%s size=%dx%d bytes=%lld"),
		*Ctx.RequestId,
		*OutPath,
		*Buffer,
		ResizedSize.X,
		ResizedSize.Y,
		(long long)Bytes);

	return true;
}

bool FUeAgentHttpServer::CmdSaveCurrentLevel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	const bool bOnlyIfDirty = [&]()
	{
		bool b = true;
		JsonTryGetBool(Ctx.Params, TEXT("only_if_dirty"), b);
		return b;
	}();

	UPackage* Pkg = World->GetOutermost();
	const bool bDirty = Pkg ? Pkg->IsDirty() : false;
	if (bOnlyIfDirty && !bDirty)
	{
		OutData->SetBoolField(TEXT("saved"), false);
		OutData->SetStringField(TEXT("reason"), TEXT("not_dirty"));
		return true;
	}

	const bool bSaved = UEditorLoadingAndSavingUtils::SaveCurrentLevel();
	OutData->SetBoolField(TEXT("saved"), bSaved);
	OutData->SetBoolField(TEXT("was_dirty"), bDirty);
	if (!bSaved)
	{
		OutError = TEXT("save_failed");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdBeginTransaction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError)
{
	FScopeLock Lock(&TransactionMutex);
	if (ActiveTransactionIndex != INDEX_NONE)
	{
		OutError = TEXT("transaction_already_active");
		return false;
	}

	FString Label = TEXT("UeAgentInterface Transaction");
	JsonTryGetString(Ctx.Params, TEXT("label"), Label);
	ActiveTransactionIndex = GEditor->BeginTransaction(FText::FromString(Label));
	OutData->SetNumberField(TEXT("transaction_index"), ActiveTransactionIndex);
	OutData->SetStringField(TEXT("label"), Label);
	return true;
}

bool FUeAgentHttpServer::CmdEndTransaction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError)
{
	FScopeLock Lock(&TransactionMutex);
	if (ActiveTransactionIndex == INDEX_NONE)
	{
		OutError = TEXT("no_active_transaction");
		return false;
	}

	bool bCommit = true;
	JsonTryGetBool(Ctx.Params, TEXT("commit"), bCommit);

	if (bCommit)
	{
		GEditor->EndTransaction();
		OutData->SetBoolField(TEXT("committed"), true);
	}
	else
	{
		GEditor->CancelTransaction(ActiveTransactionIndex);
		OutData->SetBoolField(TEXT("committed"), false);
	}

	OutData->SetNumberField(TEXT("transaction_index"), ActiveTransactionIndex);
	ActiveTransactionIndex = INDEX_NONE;
	return true;
}

bool FUeAgentHttpServer::CmdUndo(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return false;
	}
	if (!GEditor->UndoTransaction())
	{
		OutError = TEXT("undo_failed");
		return false;
	}
	OutData->SetBoolField(TEXT("ok"), true);
	return true;
}

bool FUeAgentHttpServer::CmdRedo(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return false;
	}
	if (!GEditor->RedoTransaction())
	{
		OutError = TEXT("redo_failed");
		return false;
	}
	OutData->SetBoolField(TEXT("ok"), true);
	return true;
}

UWorld* FUeAgentHttpServer::GetEditorWorld(FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("no_geditor");
		return nullptr;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("no_editor_world");
		return nullptr;
	}
	return World;
}

AActor* FUeAgentHttpServer::FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A)
		{
			continue;
		}
		if (A->GetName() == NameOrLabel || A->GetActorLabel() == NameOrLabel)
		{
			return A;
		}
	}
	return nullptr;
}

bool FUeAgentHttpServer::TakeActiveViewportScreenshot(TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC || !LVC->Viewport)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FViewport* Viewport = LVC->Viewport;
	const FText ScreenshotRealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "ScreenshotRealtimeOverrideSystem", "UeAgentInterfaceScreenshot");
	const bool bNeedTemporaryRealtime = !LVC->IsRealtime();
	if (bNeedTemporaryRealtime)
	{
		if (LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
		{
			LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
		}
		LVC->AddRealtimeOverride(true, ScreenshotRealtimeOverrideSystem);
	}

	// Ensure the viewport has a chance to redraw with the latest camera/scene state before we read pixels.
	Viewport->Invalidate();
	Viewport->Draw(/*bShouldPresent*/ true);
	FlushRenderingCommands();

	if (bNeedTemporaryRealtime && LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
	{
		LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
	}

	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		OutError = TEXT("invalid_viewport_size");
		return false;
	}

	TArray<FColor> Pixels;
	Pixels.AddUninitialized(Size.X * Size.Y);
	if (!Viewport->ReadPixels(Pixels))
	{
		OutError = TEXT("read_pixels_failed");
		return false;
	}

	OutPixels = MoveTemp(Pixels);
	OutSize = Size;
	return true;
}

bool FUeAgentHttpServer::ResizePixelsMaxSize(const TArray<FColor>& InPixels, const FIntPoint& InSize, int32 MaxSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError)
{
	if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != InSize.X * InSize.Y)
	{
		OutError = TEXT("invalid_pixels");
		return false;
	}

	MaxSize = FMath::Clamp(MaxSize, 64, 8192);
	if (InSize.X <= MaxSize && InSize.Y <= MaxSize)
	{
		OutPixels = InPixels;
		OutSize = InSize;
		return true;
	}

	const float Scale = (float)MaxSize / (float)FMath::Max(InSize.X, InSize.Y);
	const int32 NewX = FMath::Max(1, (int32)FMath::RoundToInt((float)InSize.X * Scale));
	const int32 NewY = FMath::Max(1, (int32)FMath::RoundToInt((float)InSize.Y * Scale));

	OutPixels.Reset();
	OutPixels.AddUninitialized(NewX * NewY);
	FImageUtils::ImageResize(InSize.X, InSize.Y, InPixels, NewX, NewY, OutPixels, /*bLinearSpace*/ true);
	OutSize = FIntPoint(NewX, NewY);
	return true;
}

bool FUeAgentHttpServer::WriteCompressedImage(const TArray<FColor>& Pixels, const FIntPoint& Size, const FString& Format, int32 Quality, const FString& OutFilePath, FString& OutError)
{
	TArray64<uint8> Compressed;
	const FImageView Img((void*)Pixels.GetData(), Size.X, Size.Y, ERawImageFormat::BGRA8);
	if (!FImageUtils::CompressImage(Compressed, *Format, Img, Quality))
	{
		OutError = TEXT("compress_failed");
		return false;
	}

	TArray<uint8> Bytes;
	Bytes.Append(Compressed.GetData(), (int32)Compressed.Num());

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilePath), true);
	if (!FFileHelper::SaveArrayToFile(Bytes, *OutFilePath))
	{
		OutError = TEXT("write_file_failed");
		return false;
	}
	return true;
}

FString FUeAgentHttpServer::MakeShotPath(const FString& Ext)
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	const FString Hostname = FPlatformProcess::ComputerName();
	const FString Ts = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s"));

	const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
	const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
	return FPaths::Combine(ShotsDir, FString::Printf(TEXT("shot_%s_%s.%s"), *Hostname, *Ts, *Ext));
}
