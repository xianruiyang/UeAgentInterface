bool FUeAgentHttpServer::CmdRenderTargetCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString Type = TEXT("2d");
	JsonTryGetStringAny(Ctx.Params, { TEXT("target_type"), TEXT("type") }, Type);
	Type = Type.ToLower();
	int32 Width = 256;
	int32 Height = 256;
	int32 Depth = 1;
	int32 Slices = 1;
	GetOptionalInt(Ctx.Params, TEXT("width"), Width, OutError);
	GetOptionalInt(Ctx.Params, TEXT("height"), Height, OutError);
	GetOptionalInt(Ctx.Params, TEXT("depth"), Depth, OutError);
	GetOptionalInt(Ctx.Params, TEXT("depth_or_slices"), Depth, OutError);
	GetOptionalInt(Ctx.Params, TEXT("slices"), Slices, OutError);
	GetOptionalInt(Ctx.Params, TEXT("depth_or_slices"), Slices, OutError);
	bool bAutoGenerateMips = false;
	GetOptionalBool(Ctx.Params, TEXT("auto_generate_mips"), bAutoGenerateMips, OutError);
	Width = FMath::Clamp(Width, 1, 8192);
	Height = FMath::Clamp(Height, 1, 8192);
	Depth = FMath::Clamp(Depth, 1, 2048);
	Slices = FMath::Clamp(Slices, 1, 2048);

	FString FormatText = TEXT("RTF_RGBA8");
	JsonTryGetString(Ctx.Params, TEXT("format"), FormatText);
	ETextureRenderTargetFormat RTFormat = RTF_RGBA8;
	if (!ParseRenderTargetFormat(FormatText, RTFormat))
	{
		OutError = TEXT("invalid_render_target_format");
		return false;
	}
	bool bReplaceExisting = false;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError);
	FLinearColor ClearColor = FLinearColor::Transparent;
	if (!ParseColor(Ctx.Params, TEXT("clear_color"), ClearColor, OutError))
	{
		return false;
	}

	UClass* Class = UTextureRenderTarget2D::StaticClass();
	if (Type == TEXT("canvas") || Type == TEXT("canvas2d"))
	{
		Class = UCanvasRenderTarget2D::StaticClass();
	}
	else if (Type == TEXT("cube"))
	{
		Class = UTextureRenderTargetCube::StaticClass();
	}
	else if (Type == TEXT("2d_array") || Type == TEXT("array"))
	{
		Class = UTextureRenderTarget2DArray::StaticClass();
	}
	else if (Type == TEXT("volume"))
	{
		Class = UTextureRenderTargetVolume::StaticClass();
	}

	UObject* Asset = CreateAssetObject(Class, AssetPath, bReplaceExisting, OutError);
	if (!Asset)
	{
		return false;
	}
	if (UTextureRenderTarget2D* RT2D = Cast<UTextureRenderTarget2D>(Asset))
	{
		RT2D->RenderTargetFormat = RTFormat;
		RT2D->ClearColor = ClearColor;
		RT2D->bAutoGenerateMips = bAutoGenerateMips;
		RT2D->InitCustomFormat(Width, Height, PixelFormatFromRTFormat(RTFormat), RTFormat != RTF_RGBA8 && RTFormat != RTF_R8);
		RT2D->UpdateResourceImmediate(true);
	}
	else if (UTextureRenderTargetCube* Cube = Cast<UTextureRenderTargetCube>(Asset))
	{
		Cube->Init(Width, PixelFormatFromRTFormat(RTFormat));
		Cube->UpdateResourceImmediate(true);
	}
	else if (UTextureRenderTarget2DArray* Array = Cast<UTextureRenderTarget2DArray>(Asset))
	{
		Array->Init(Width, Height, Slices, PixelFormatFromRTFormat(RTFormat));
		Array->UpdateResourceImmediate(true);
	}
	else if (UTextureRenderTargetVolume* Volume = Cast<UTextureRenderTargetVolume>(Asset))
	{
		Volume->Init(Width, Height, Depth, PixelFormatFromRTFormat(RTFormat));
		Volume->UpdateResourceImmediate(true);
	}
	Asset->PostEditChange();
	Asset->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildRenderTargetInfo(Cast<UTextureRenderTarget>(Asset));
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("auto_generate_mips"), bAutoGenerateMips);
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget* RenderTarget = LoadAsset<UTextureRenderTarget>(AssetPath);
	if (!RenderTarget)
	{
		OutError = TEXT("render_target_not_found");
		return false;
	}
	OutData = BuildRenderTargetInfo(RenderTarget);
	bool bIncludePixelStats = false;
	bool bIncludeResourceState = false;
	GetOptionalBool(Ctx.Params, TEXT("include_pixel_stats"), bIncludePixelStats, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_resource_state"), bIncludeResourceState, OutError);
	OutData->SetBoolField(TEXT("include_resource_state"), bIncludeResourceState);
	if (bIncludePixelStats)
	{
		if (UTextureRenderTarget2D* RT2D = Cast<UTextureRenderTarget2D>(RenderTarget))
		{
			TArray<FColor> Pixels;
			FIntPoint Size;
			FString PixelError;
			if (ReadRenderTarget2DPixels(RT2D, Pixels, Size, PixelError))
			{
				OutData->SetObjectField(TEXT("stats"), BuildPixelStatsFromColors(Pixels, Size));
			}
			else
			{
				OutData->SetStringField(TEXT("pixel_stats_error"), PixelError);
			}
		}
		else
		{
			OutData->SetStringField(TEXT("pixel_stats_error"), TEXT("render_target_pixel_stats_only_supports_2d"));
		}
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	UTextureRenderTarget* RenderTarget = LoadAsset<UTextureRenderTarget>(AssetPath);
	if (!RenderTarget)
	{
		OutError = TEXT("render_target_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(RenderTarget, TEXT("ue_agent_interface.render_target.v1"), true);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_raw_properties"), true, OutError);
	EchoOptionalBool(Ctx.Params, Root, TEXT("include_validation"), true, OutError);
	Root->SetObjectField(TEXT("render_target_info"), BuildRenderTargetInfo(RenderTarget));
	FString Saved;
	if (!SaveJsonFile(OutputFile, Root, Saved, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), Saved);
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString Profile;
	Root->TryGetStringField(TEXT("profile"), Profile);
	if (Profile != TEXT("ue_agent_interface.render_target.v1"))
	{
		AddIssue(Issues, TEXT("unexpected_profile"), TEXT("warning"), TEXT("JSON profile is not ue_agent_interface.render_target.v1."));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), Resolved);
	OutData->SetBoolField(TEXT("validation_passed"), true);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(TEXT("ue_agent_interface.render_target.v1")));
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString Resolved;
	if (!LoadJsonFile(JsonFile, Root, Resolved, OutError))
	{
		return false;
	}
	FString AssetPath;
	Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	UTextureRenderTarget* RenderTarget = LoadAsset<UTextureRenderTarget>(AssetPath);
	if (!RenderTarget)
	{
		OutError = TEXT("render_target_not_found");
		return false;
	}
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowResize = false;
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("allow_resize"), bAllowResize, OutError);
	if (bDryRun || bValidateOnly)
	{
		OutData = BuildRenderTargetInfo(RenderTarget);
		OutData->SetStringField(TEXT("json_file"), Resolved);
		OutData->SetBoolField(TEXT("dry_run"), bDryRun);
		OutData->SetBoolField(TEXT("validate_only"), bValidateOnly);
		OutData->SetBoolField(TEXT("allow_resize"), bAllowResize);
		OutData->SetArrayField(TEXT("property_results"), {});
		OutData->SetObjectField(TEXT("resize_result"), MakeShared<FJsonObject>());
		return true;
	}
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!ApplyGenericProperties(RenderTarget, Root, Results, OutError))
	{
		return false;
	}
	RenderTarget->PostEditChange();
	RenderTarget->UpdateResource();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ RenderTarget }, OutError))
	{
		return false;
	}
	OutData = BuildRenderTargetInfo(RenderTarget);
	OutData->SetStringField(TEXT("json_file"), Resolved);
	OutData->SetBoolField(TEXT("allow_resize"), bAllowResize);
	OutData->SetArrayField(TEXT("property_results"), Results);
	OutData->SetObjectField(TEXT("resize_result"), MakeShared<FJsonObject>());
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetResize(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	bool bAllowResize = false;
	GetOptionalBool(Ctx.Params, TEXT("allow_resize"), bAllowResize, OutError);
	if (!bAllowResize)
	{
		OutError = TEXT("resize_requires_allow_resize");
		return false;
	}
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	const int32 OldWidth = RT2D->SizeX;
	const int32 OldHeight = RT2D->SizeY;
	int32 Width = RT2D->SizeX;
	int32 Height = RT2D->SizeY;
	GetOptionalInt(Ctx.Params, TEXT("width"), Width, OutError);
	GetOptionalInt(Ctx.Params, TEXT("height"), Height, OutError);
	int32 DepthOrSlices = 1;
	GetOptionalInt(Ctx.Params, TEXT("depth_or_slices"), DepthOrSlices, OutError);
	bool bPreserveContents = false;
	GetOptionalBool(Ctx.Params, TEXT("preserve_contents"), bPreserveContents, OutError);
	RT2D->ResizeTarget(FMath::Clamp(Width, 1, 8192), FMath::Clamp(Height, 1, 8192));
	RT2D->UpdateResourceImmediate(true);
	RT2D->PostEditChange();
	RT2D->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_resize"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ RT2D }, OutError))
	{
		return false;
	}
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetBoolField(TEXT("resized"), true);
	TSharedPtr<FJsonObject> OldSize = MakeShared<FJsonObject>();
	OldSize->SetNumberField(TEXT("width"), OldWidth);
	OldSize->SetNumberField(TEXT("height"), OldHeight);
	OutData->SetObjectField(TEXT("old_size"), OldSize);
	TSharedPtr<FJsonObject> NewSize = MakeShared<FJsonObject>();
	NewSize->SetNumberField(TEXT("width"), RT2D->SizeX);
	NewSize->SetNumberField(TEXT("height"), RT2D->SizeY);
	NewSize->SetNumberField(TEXT("depth_or_slices"), DepthOrSlices);
	OutData->SetObjectField(TEXT("new_size"), NewSize);
	if (bPreserveContents)
	{
		OutData->SetStringField(TEXT("preserve_contents_note"), TEXT("best_effort_resize_recreates_render_resource"));
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetClear(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	FLinearColor ClearColor = FLinearColor::Black;
	if (!ParseColorAny(Ctx.Params, { TEXT("clear_color"), TEXT("color") }, ClearColor, OutError))
	{
		return false;
	}
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UKismetRenderingLibrary::ClearRenderTarget2D(World, RT2D, ClearColor);
	FlushRenderingCommands();
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetObjectField(TEXT("cleared_color"), MakeColorJson(ClearColor));
	OutData->SetBoolField(TEXT("cleared"), true);
	TArray<FColor> Pixels;
	FIntPoint Size;
	FString PixelError;
	if (ReadRenderTarget2DPixels(RT2D, Pixels, Size, PixelError))
	{
		OutData->SetObjectField(TEXT("stats_after"), BuildPixelStatsFromColors(Pixels, Size));
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetDrawMaterial(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString MaterialPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("material"), MaterialPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	UMaterialInterface* Material = LoadAsset<UMaterialInterface>(MaterialPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	UMaterialInterface* MaterialToDraw = Material;
	TSharedPtr<FJsonObject> AppliedParameters = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* Parameters = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters && Parameters->IsValid())
	{
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());
		if (MID)
		{
			for (const auto& Pair : (*Parameters)->Values)
			{
				if (!Pair.Value.IsValid())
				{
					continue;
				}
				if (Pair.Value->Type == EJson::Number)
				{
					MID->SetScalarParameterValue(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber()));
					AppliedParameters->SetNumberField(Pair.Key, Pair.Value->AsNumber());
				}
				else if (Pair.Value->Type == EJson::String)
				{
					if (UTexture* ParamTexture = LoadAsset<UTexture>(Pair.Value->AsString()))
					{
						MID->SetTextureParameterValue(FName(*Pair.Key), ParamTexture);
						AppliedParameters->SetStringField(Pair.Key, NormalizeAssetPath(ParamTexture->GetOutermost()->GetName()));
					}
				}
				else if (Pair.Value->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> ColorObj = Pair.Value->AsObject();
					if (ColorObj.IsValid())
					{
						double R = 0, G = 0, B = 0, A = 1;
						ColorObj->TryGetNumberField(TEXT("r"), R);
						ColorObj->TryGetNumberField(TEXT("g"), G);
						ColorObj->TryGetNumberField(TEXT("b"), B);
						ColorObj->TryGetNumberField(TEXT("a"), A);
						MID->SetVectorParameterValue(FName(*Pair.Key), FLinearColor(R, G, B, A));
						AppliedParameters->SetObjectField(Pair.Key, MakeColorJson(FLinearColor(R, G, B, A)));
					}
				}
			}
			MaterialToDraw = MID;
		}
	}
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT2D, MaterialToDraw);
	FlushRenderingCommands();
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetStringField(TEXT("material"), NormalizeAssetPath(Material->GetOutermost()->GetName()));
	FString FeatureLevel;
	JsonTryGetString(Ctx.Params, TEXT("feature_level"), FeatureLevel);
	OutData->SetStringField(TEXT("feature_level"), FeatureLevel);
	OutData->SetObjectField(TEXT("parameters"), AppliedParameters);
	OutData->SetBoolField(TEXT("drawn"), true);
	TArray<FColor> Pixels;
	FIntPoint Size;
	FString PixelError;
	if (ReadRenderTarget2DPixels(RT2D, Pixels, Size, PixelError))
	{
		OutData->SetObjectField(TEXT("stats_after"), BuildPixelStatsFromColors(Pixels, Size));
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetCopyTexture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString TexturePath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("target_render_target"), TEXT("render_target") }, AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("source_texture"), TexturePath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	UTexture* SourceTexture = LoadAsset<UTexture>(TexturePath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	if (!SourceTexture)
	{
		OutError = TEXT("source_texture_not_found");
		return false;
	}
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	if (!SourceTexture->GetResource())
	{
		SourceTexture->UpdateResource();
		FlushRenderingCommands();
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FTextureRenderTargetResource* Resource = RT2D->GameThread_GetRenderTargetResource();
	if (!Resource || !SourceTexture->GetResource())
	{
		OutError = TEXT("render_resource_unavailable");
		return false;
	}
	FCanvas Canvas(Resource, nullptr, World, World->GetFeatureLevel());
	FCanvasTileItem Tile(FVector2D(0, 0), SourceTexture->GetResource(), FVector2D(RT2D->SizeX, RT2D->SizeY), FLinearColor::White);
	Tile.BlendMode = SE_BLEND_Opaque;
	Canvas.DrawItem(Tile);
	Canvas.Flush_GameThread();
	FlushRenderingCommands();
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetStringField(TEXT("source_texture"), NormalizeAssetPath(SourceTexture->GetOutermost()->GetName()));
	OutData->SetBoolField(TEXT("copied"), true);
	TArray<FColor> Pixels;
	FIntPoint Size;
	FString PixelError;
	if (ReadRenderTarget2DPixels(RT2D, Pixels, Size, PixelError))
	{
		OutData->SetObjectField(TEXT("stats_after"), BuildPixelStatsFromColors(Pixels, Size));
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetCaptureScene(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	FVector Location(0, -300, 200);
	FRotator Rotation(-20, 0, 0);
	float FOV = 90.f;
	const TSharedPtr<FJsonObject>* CameraObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("camera"), CameraObj) && CameraObj && CameraObj->IsValid())
	{
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		if ((*CameraObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && LocationObj->IsValid())
		{
			double X = Location.X, Y = Location.Y, Z = Location.Z;
			(*LocationObj)->TryGetNumberField(TEXT("x"), X);
			(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
			(*LocationObj)->TryGetNumberField(TEXT("z"), Z);
			Location = FVector(X, Y, Z);
		}
		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		if ((*CameraObj)->TryGetObjectField(TEXT("rotation"), RotationObj) && RotationObj && RotationObj->IsValid())
		{
			double Pitch = Rotation.Pitch, Yaw = Rotation.Yaw, Roll = Rotation.Roll;
			(*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*RotationObj)->TryGetNumberField(TEXT("roll"), Roll);
			Rotation = FRotator(Pitch, Yaw, Roll);
		}
		double FovValue = FOV;
		if ((*CameraObj)->TryGetNumberField(TEXT("fov"), FovValue))
		{
			FOV = static_cast<float>(FovValue);
		}
	}
	AActor* CaptureActor = World->SpawnActor<ASceneCapture2D>(Location, Rotation);
	if (!CaptureActor)
	{
		OutError = TEXT("spawn_scene_capture_failed");
		return false;
	}
	ASceneCapture2D* Capture = Cast<ASceneCapture2D>(CaptureActor);
	USceneCaptureComponent2D* CaptureComp = Capture ? Capture->GetCaptureComponent2D() : nullptr;
	if (!CaptureComp)
	{
		CaptureActor->Destroy();
		OutError = TEXT("scene_capture_component_missing");
		return false;
	}
	CaptureComp->TextureTarget = RT2D;
	CaptureComp->CaptureSource = SCS_FinalColorLDR;
	FString CaptureSourceText;
	if (JsonTryGetString(Ctx.Params, TEXT("capture_source"), CaptureSourceText) && !CaptureSourceText.TrimStartAndEnd().IsEmpty())
	{
		if (UEnum* CaptureSourceEnum = StaticEnum<ESceneCaptureSource>())
		{
			const int64 Value = CaptureSourceEnum->GetValueByNameString(CaptureSourceText);
			if (Value != INDEX_NONE)
			{
				CaptureComp->CaptureSource = static_cast<ESceneCaptureSource>(Value);
			}
		}
	}
	CaptureComp->FOVAngle = FOV;
	auto FindActorForCapture = [World](const FString& ActorId) -> AActor*
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetPathName() == ActorId || Actor->GetName() == ActorId || Actor->GetActorLabel() == ActorId))
			{
				return Actor;
			}
		}
		return nullptr;
	};
	TArray<FString> ActorsOnly;
	if (ReadStringArray(Ctx.Params, TEXT("actors_only"), ActorsOnly) && ActorsOnly.Num() > 0)
	{
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		for (const FString& ActorId : ActorsOnly)
		{
			if (AActor* Actor = FindActorForCapture(ActorId))
			{
				CaptureComp->ShowOnlyActors.Add(Actor);
			}
		}
	}
	TArray<FString> HiddenActors;
	if (ReadStringArray(Ctx.Params, TEXT("hidden_actors"), HiddenActors))
	{
		for (const FString& ActorId : HiddenActors)
		{
			if (AActor* Actor = FindActorForCapture(ActorId))
			{
				CaptureComp->HiddenActors.Add(Actor);
			}
		}
	}
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->CaptureScene();
	FlushRenderingCommands();
	CaptureActor->Destroy();
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetBoolField(TEXT("captured"), true);
	TSharedPtr<FJsonObject> CameraResult = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> LocationResult = MakeShared<FJsonObject>();
	LocationResult->SetNumberField(TEXT("x"), Location.X);
	LocationResult->SetNumberField(TEXT("y"), Location.Y);
	LocationResult->SetNumberField(TEXT("z"), Location.Z);
	CameraResult->SetObjectField(TEXT("location"), LocationResult);
	CameraResult->SetNumberField(TEXT("fov"), FOV);
	OutData->SetObjectField(TEXT("camera"), CameraResult);
	OutData->SetStringField(TEXT("capture_source"), StaticEnum<ESceneCaptureSource>()->GetNameStringByValue(static_cast<int64>(CaptureComp->CaptureSource)));
	OutData->SetArrayField(TEXT("actors_only"), {});
	OutData->SetArrayField(TEXT("hidden_actors"), {});
	TArray<FColor> Pixels;
	FIntPoint Size;
	FString PixelError;
	if (ReadRenderTarget2DPixels(RT2D, Pixels, Size, PixelError))
	{
		OutData->SetObjectField(TEXT("stats_after"), BuildPixelStatsFromColors(Pixels, Size));
	}
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetReadPixels(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	TArray<FColor> Pixels;
	FIntPoint Size;
	if (!ReadRenderTarget2DPixels(RT2D, Pixels, Size, OutError))
	{
		return false;
	}
	FIntRect Rect(0, 0, Size.X, Size.Y);
	const TSharedPtr<FJsonObject>* RectObj = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("rect"), RectObj) && RectObj && RectObj->IsValid())
	{
		double X = 0;
		double Y = 0;
		double W = Size.X;
		double H = Size.Y;
		(*RectObj)->TryGetNumberField(TEXT("x"), X);
		(*RectObj)->TryGetNumberField(TEXT("y"), Y);
		(*RectObj)->TryGetNumberField(TEXT("w"), W);
		(*RectObj)->TryGetNumberField(TEXT("width"), W);
		(*RectObj)->TryGetNumberField(TEXT("h"), H);
		(*RectObj)->TryGetNumberField(TEXT("height"), H);
		Rect.Min.X = FMath::Clamp(static_cast<int32>(X), 0, Size.X);
		Rect.Min.Y = FMath::Clamp(static_cast<int32>(Y), 0, Size.Y);
		Rect.Max.X = FMath::Clamp(Rect.Min.X + FMath::Max(1, static_cast<int32>(W)), Rect.Min.X, Size.X);
		Rect.Max.Y = FMath::Clamp(Rect.Min.Y + FMath::Max(1, static_cast<int32>(H)), Rect.Min.Y, Size.Y);
		TArray<FColor> Cropped;
		Cropped.Reserve(Rect.Width() * Rect.Height());
		for (int32 Row = Rect.Min.Y; Row < Rect.Max.Y; ++Row)
		{
			for (int32 Col = Rect.Min.X; Col < Rect.Max.X; ++Col)
			{
				Cropped.Add(Pixels[Row * Size.X + Col]);
			}
		}
		Pixels = MoveTemp(Cropped);
		Size = FIntPoint(Rect.Width(), Rect.Height());
	}
	int32 MaxPixels = 0;
	GetOptionalInt(Ctx.Params, TEXT("max_pixels"), MaxPixels, OutError);
	FString ReadFormat = TEXT("rgba8");
	JsonTryGetString(Ctx.Params, TEXT("format"), ReadFormat);
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetObjectField(TEXT("stats"), BuildPixelStatsFromColors(Pixels, Size));
	OutData->SetStringField(TEXT("read_path"), TEXT("FRenderTarget::ReadPixels"));
	OutData->SetStringField(TEXT("format"), ReadFormat);
	TSharedPtr<FJsonObject> RectJson = MakeShared<FJsonObject>();
	RectJson->SetNumberField(TEXT("x"), Rect.Min.X);
	RectJson->SetNumberField(TEXT("y"), Rect.Min.Y);
	RectJson->SetNumberField(TEXT("width"), Size.X);
	RectJson->SetNumberField(TEXT("height"), Size.Y);
	OutData->SetObjectField(TEXT("rect"), RectJson);
	TArray<TSharedPtr<FJsonValue>> Samples;
	const int32 SampleCount = MaxPixels > 0 ? FMath::Min(MaxPixels, Pixels.Num()) : 0;
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetNumberField(TEXT("index"), Index);
		Sample->SetObjectField(TEXT("color"), MakeColorJson(Pixels[Index].ReinterpretAsLinear()));
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
	}
	OutData->SetArrayField(TEXT("samples"), Samples);
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetSampleUv(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	float U = 0.5f;
	float V = 0.5f;
	GetOptionalFloat(Ctx.Params, TEXT("u"), U, OutError);
	GetOptionalFloat(Ctx.Params, TEXT("v"), V, OutError);
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TPair<float, float>> Uvs;
	const TArray<TSharedPtr<FJsonValue>>* UvValues = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("uvs"), UvValues) && UvValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *UvValues)
		{
			const TSharedPtr<FJsonObject>* UvObj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(UvObj) && UvObj && UvObj->IsValid())
			{
				double ItemU = U;
				double ItemV = V;
				(*UvObj)->TryGetNumberField(TEXT("u"), ItemU);
				(*UvObj)->TryGetNumberField(TEXT("v"), ItemV);
				Uvs.Add(TPair<float, float>(static_cast<float>(ItemU), static_cast<float>(ItemV)));
			}
		}
	}
	if (Uvs.Num() == 0)
	{
		Uvs.Add(TPair<float, float>(U, V));
	}
	OutData = BuildRenderTargetInfo(RT2D);
	bool bLinear = false;
	GetOptionalBool(Ctx.Params, TEXT("linear"), bLinear, OutError);
	OutData->SetBoolField(TEXT("linear"), bLinear);
	TArray<TSharedPtr<FJsonValue>> Samples;
	for (int32 Index = 0; Index < Uvs.Num(); ++Index)
	{
		const FColor Pixel = UKismetRenderingLibrary::ReadRenderTargetUV(World, RT2D, Uvs[Index].Key, Uvs[Index].Value);
		TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetNumberField(TEXT("index"), Index);
		Sample->SetNumberField(TEXT("u"), Uvs[Index].Key);
		Sample->SetNumberField(TEXT("v"), Uvs[Index].Value);
		Sample->SetObjectField(TEXT("color"), MakeColorJson(Pixel.ReinterpretAsLinear()));
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
		if (Index == 0)
		{
			OutData->SetNumberField(TEXT("u"), Uvs[Index].Key);
			OutData->SetNumberField(TEXT("v"), Uvs[Index].Value);
			OutData->SetObjectField(TEXT("color"), MakeColorJson(Pixel.ReinterpretAsLinear()));
		}
	}
	OutData->SetArrayField(TEXT("samples"), Samples);
	OutData->SetBoolField(TEXT("sampled"), true);
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetExportImage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.ToLower();
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (OutputFile.TrimStartAndEnd().IsEmpty())
	{
		OutputFile = MakeShotPath(Format);
	}
	else
	{
		OutputFile = ResolveFilePath(OutputFile);
	}
	TArray<FColor> Pixels;
	FIntPoint Size;
	if (!ReadRenderTarget2DPixels(RT2D, Pixels, Size, OutError))
	{
		return false;
	}
	if (!WriteCompressedImage(Pixels, Size, Format, 90, OutputFile, OutError))
	{
		return false;
	}
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetStringField(TEXT("file_path"), OutputFile);
	OutData->SetStringField(TEXT("format"), Format);
	int32 MipIndex = 0;
	GetOptionalInt(Ctx.Params, TEXT("mip_index"), MipIndex, OutError);
	OutData->SetNumberField(TEXT("mip_index"), MipIndex);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(IFileManager::Get().FileSize(*OutputFile)));
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetCreateStaticTexture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	FString DestinationAssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("asset_path"), TEXT("render_target") }, AssetPath, OutError))
	{
		return false;
	}
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("destination_asset_path") }, DestinationAssetPath))
	{
		FString DestinationPath;
		FString DestinationName;
		if (!GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError) || !GetRequiredString(Ctx, TEXT("destination_name"), DestinationName, OutError))
		{
			return false;
		}
		DestinationAssetPath = FPaths::Combine(DestinationPath, DestinationName);
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	FString DestPath;
	FString DestName;
	if (!ValidateAssetPathForCreate(DestinationAssetPath, DestPath, DestName, OutError))
	{
		return false;
	}
	if (LoadAssetObject(DestPath))
	{
		OutError = TEXT("destination_asset_already_exists");
		return false;
	}
	if (!RequireRenderResourceAccess(OutError))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*DestPath);
	RT2D->UpdateResourceImmediate(false);
	FlushRenderingCommands();
	UTexture2D* Texture = RT2D->ConstructTexture2D(Package, DestName, RF_Public | RF_Standalone | RF_Transactional, CTF_Default, nullptr);
	if (!Texture)
	{
		TArray<FColor> Pixels;
		FIntPoint Size;
		if (!ReadRenderTarget2DPixels(RT2D, Pixels, Size, OutError))
		{
			return false;
		}
		if (Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != Size.X * Size.Y)
		{
			OutError = TEXT("render_target_pixel_data_invalid");
			return false;
		}
		Texture = NewObject<UTexture2D>(Package, *DestName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Texture)
		{
			OutError = TEXT("texture2d_create_failed");
			return false;
		}
		Texture->Source.Init(Size.X, Size.Y, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = true;
		Texture->CompressionSettings = TC_Default;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->UpdateResource();
	}
	FString CompressionSettings;
	if (JsonTryGetString(Ctx.Params, TEXT("compression_settings"), CompressionSettings) && !CompressionSettings.TrimStartAndEnd().IsEmpty())
	{
		FString Readback;
		FString ImportError;
		ImportPropertyText(Texture, TEXT("CompressionSettings"), CompressionSettings.TrimStartAndEnd(), Readback, ImportError);
	}
	FString MipSettings;
	if (JsonTryGetStringAny(Ctx.Params, { TEXT("mip_settings"), TEXT("mip_gen_settings") }, MipSettings) && !MipSettings.TrimStartAndEnd().IsEmpty())
	{
		FString Readback;
		FString ImportError;
		ImportPropertyText(Texture, TEXT("MipGenSettings"), MipSettings.TrimStartAndEnd(), Readback, ImportError);
	}
	FAssetRegistryModule::AssetCreated(Texture);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	if (bSave && !SavePackagesForObjects({ Texture }, OutError))
	{
		return false;
	}
	OutData = BuildTextureInfo(Texture, true, true);
	OutData->SetStringField(TEXT("source_render_target"), NormalizeAssetPath(RT2D->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("texture_asset_path"), NormalizeAssetPath(Texture->GetOutermost()->GetName()));
	return true;
}

bool FUeAgentHttpServer::CmdRenderTargetProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	if (!CmdRenderTargetReadPixels(Ctx, OutData, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("operation"), TEXT("read_pixels"));
	Step->SetBoolField(TEXT("passed"), true);
	if (OutData.IsValid() && OutData->HasTypedField<EJson::Object>(TEXT("stats")))
	{
		Step->SetObjectField(TEXT("stats"), OutData->GetObjectField(TEXT("stats")));
	}
	Steps.Add(MakeShared<FJsonValueObject>(Step));
	bool bProbePassed = true;
	bool bExpectedChanged = false;
	UeAgentTextureOps::GetOptionalBool(Ctx.Params, TEXT("expected_changed"), bExpectedChanged, OutError);
	const TSharedPtr<FJsonObject>* ExpectedStats = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_stats"), ExpectedStats) && ExpectedStats && ExpectedStats->IsValid() && OutData->HasTypedField<EJson::Object>(TEXT("stats")))
	{
		double MinNonBlack = 0;
		if ((*ExpectedStats)->TryGetNumberField(TEXT("min_non_black_pixel_count"), MinNonBlack) && OutData->GetObjectField(TEXT("stats"))->GetNumberField(TEXT("non_black_pixel_count")) < MinNonBlack)
		{
			bProbePassed = false;
		}
		OutData->SetObjectField(TEXT("expected_stats"), *ExpectedStats);
	}
	bool bCleanup = false;
	GetOptionalBool(Ctx.Params, TEXT("cleanup"), bCleanup, OutError);
	OutData->SetBoolField(TEXT("cleanup"), bCleanup);
	OutData->SetArrayField(TEXT("steps"), Steps);
	OutData->SetBoolField(TEXT("probe_passed"), bProbePassed);
	return true;
}

bool FUeAgentHttpServer::CmdSceneCaptureValidateRenderTarget(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentTextureOps;
	FString AssetPath;
	if (!GetRequiredStringAny(Ctx, { TEXT("expected_render_target"), TEXT("render_target"), TEXT("asset_path") }, AssetPath, OutError))
	{
		return false;
	}
	UTextureRenderTarget2D* RT2D = LoadAsset<UTextureRenderTarget2D>(AssetPath);
	if (!RT2D)
	{
		OutError = TEXT("render_target_2d_not_found");
		return false;
	}
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> CaptureComponents;
	FString ExpectedCaptureSource;
	JsonTryGetString(Ctx.Params, TEXT("expected_capture_source"), ExpectedCaptureSource);
	TArray<TSharedPtr<FJsonValue>> Issues;
	for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It)
	{
		USceneCaptureComponent2D* Comp = *It;
		if (Comp && Comp->GetWorld() == World && Comp->TextureTarget == RT2D)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("component"), Comp->GetPathName());
			Obj->SetStringField(TEXT("owner"), Comp->GetOwner() ? Comp->GetOwner()->GetActorLabel() : FString());
			const FString CaptureSourceName = StaticEnum<ESceneCaptureSource>()->GetNameStringByValue(static_cast<int64>(Comp->CaptureSource));
			Obj->SetStringField(TEXT("capture_source"), CaptureSourceName);
			if (!ExpectedCaptureSource.IsEmpty() && !CaptureSourceName.Equals(ExpectedCaptureSource, ESearchCase::IgnoreCase))
			{
				AddIssue(Issues, TEXT("capture_source_mismatch"), TEXT("warning"), TEXT("SceneCapture capture source does not match expected_capture_source."));
			}
			CaptureComponents.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}
	OutData = BuildRenderTargetInfo(RT2D);
	OutData->SetArrayField(TEXT("scene_capture_components"), CaptureComponents);
	OutData->SetBoolField(TEXT("validation_passed"), CaptureComponents.Num() > 0 && Issues.Num() == 0);
	OutData->SetStringField(TEXT("status"), CaptureComponents.Num() > 0 && Issues.Num() == 0 ? TEXT("ok") : TEXT("no_bound_scene_capture_component_or_mismatch"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("component_count"), CaptureComponents.Num());
	Summary->SetStringField(TEXT("expected_render_target"), AssetPath);
	Summary->SetStringField(TEXT("expected_capture_source"), ExpectedCaptureSource);
	OutData->SetObjectField(TEXT("component_summary"), Summary);
	return true;
}

bool FUeAgentHttpServer::CmdSceneCaptureRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString RenderTargetPath;
	if (!UeAgentTextureOps::GetRequiredStringAny(Ctx, { TEXT("render_target"), TEXT("asset_path") }, RenderTargetPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		for (const auto& Pair : Ctx.Params->Values)
		{
			Params->SetField(Pair.Key, Pair.Value);
		}
	}
	Params->SetStringField(TEXT("asset_path"), RenderTargetPath);
	FUeAgentRequestContext CaptureCtx = Ctx;
	CaptureCtx.Params = Params;
	TSharedPtr<FJsonObject> BeforeData;
	FString BeforeError;
	CmdRenderTargetReadPixels(CaptureCtx, BeforeData, BeforeError);
	if (!CmdRenderTargetCaptureScene(CaptureCtx, OutData, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AfterData;
	FString AfterError;
	if (CmdRenderTargetReadPixels(CaptureCtx, AfterData, AfterError))
	{
		if (BeforeData.IsValid() && BeforeData->HasTypedField<EJson::Object>(TEXT("stats")))
		{
			OutData->SetObjectField(TEXT("before_stats"), BeforeData->GetObjectField(TEXT("stats")));
		}
		if (AfterData.IsValid() && AfterData->HasTypedField<EJson::Object>(TEXT("stats")))
		{
			OutData->SetObjectField(TEXT("after_stats"), AfterData->GetObjectField(TEXT("stats")));
		}
	}
	bool bProbePassed = true;
	bool bExpectedChanged = false;
	UeAgentTextureOps::GetOptionalBool(Ctx.Params, TEXT("expected_changed"), bExpectedChanged, OutError);
	const TSharedPtr<FJsonObject>* ExpectedStats = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("expected_stats"), ExpectedStats) && ExpectedStats && ExpectedStats->IsValid() && OutData->HasTypedField<EJson::Object>(TEXT("after_stats")))
	{
		double MinNonBlack = 0;
		if ((*ExpectedStats)->TryGetNumberField(TEXT("min_non_black_pixel_count"), MinNonBlack) && OutData->GetObjectField(TEXT("after_stats"))->GetNumberField(TEXT("non_black_pixel_count")) < MinNonBlack)
		{
			bProbePassed = false;
		}
		OutData->SetObjectField(TEXT("expected_stats"), *ExpectedStats);
	}
	OutData->SetBoolField(TEXT("expected_changed"), bExpectedChanged);
	if (bExpectedChanged && OutData->HasTypedField<EJson::Object>(TEXT("before_stats")) && OutData->HasTypedField<EJson::Object>(TEXT("after_stats")))
	{
		const double BeforeNonBlack = OutData->GetObjectField(TEXT("before_stats"))->GetNumberField(TEXT("non_black_pixel_count"));
		const double AfterNonBlack = OutData->GetObjectField(TEXT("after_stats"))->GetNumberField(TEXT("non_black_pixel_count"));
		if (FMath::IsNearlyEqual(BeforeNonBlack, AfterNonBlack))
		{
			bProbePassed = false;
		}
	}
	OutData->SetBoolField(TEXT("probe_passed"), bProbePassed);
	return true;
}
