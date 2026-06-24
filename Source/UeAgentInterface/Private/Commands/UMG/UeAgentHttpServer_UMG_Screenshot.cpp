// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_UMG.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "RenderDeferredCleanup.h"
#include "Slate/WidgetRenderer.h"
#include "UObject/StrongObjectPtr.h"
#include "UeAgentInterfaceSettings.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgScreenshotOps
{
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

	static UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UWidgetBlueprint* Existing = FindObject<UWidgetBlueprint>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
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

	static bool TryGetNumberAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
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

	static bool TryGetIntAny(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, int32& OutValue)
	{
		double NumberValue = 0.0;
		if (!TryGetNumberAny(Obj, Keys, NumberValue))
		{
			return false;
		}
		OutValue = FMath::RoundToInt(NumberValue);
		return true;
	}

	static TSharedPtr<FJsonObject> RectToJson(const FIntRect& Rect)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Rect.Min.X);
		Obj->SetNumberField(TEXT("y"), Rect.Min.Y);
		Obj->SetNumberField(TEXT("width"), Rect.Width());
		Obj->SetNumberField(TEXT("height"), Rect.Height());
		Obj->SetNumberField(TEXT("right"), Rect.Max.X);
		Obj->SetNumberField(TEXT("bottom"), Rect.Max.Y);
		return Obj;
	}

	static TSharedPtr<FJsonObject> SizeToJson(const FIntPoint& Size)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("width"), Size.X);
		Obj->SetNumberField(TEXT("height"), Size.Y);
		return Obj;
	}

	static bool ScreenshotWidgetWithDrawSize(
		const TSharedRef<SWidget>& Widget,
		const FVector2D& DrawSize,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = TEXT("slate_not_initialized");
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

		FSlateApplication::Get().PumpMessages();
		Widget->SlatePrepass(1.0f);
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
		if (OutSize.X <= 0 || OutSize.Y <= 0 || OutPixels.Num() != OutSize.X * OutSize.Y)
		{
			OutError = TEXT("offscreen_screenshot_empty");
			return false;
		}
		return true;
	}

	static bool FindAlphaBounds(
		const TArray<FColor>& Pixels,
		const FIntPoint& Size,
		const int32 AlphaThreshold,
		FIntRect& OutBounds)
	{
		if (Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != Size.X * Size.Y)
		{
			return false;
		}

		int32 MinX = Size.X;
		int32 MinY = Size.Y;
		int32 MaxX = -1;
		int32 MaxY = -1;
		const int32 Threshold = FMath::Clamp(AlphaThreshold, 0, 255);
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			const int32 RowOffset = Y * Size.X;
			for (int32 X = 0; X < Size.X; ++X)
			{
				if (Pixels[RowOffset + X].A <= Threshold)
				{
					continue;
				}
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
			}
		}

		if (MaxX < MinX || MaxY < MinY)
		{
			return false;
		}
		OutBounds = FIntRect(MinX, MinY, MaxX + 1, MaxY + 1);
		return true;
	}

	static FIntRect ClampRectToSize(const FIntRect& Rect, const FIntPoint& Size)
	{
		const int32 MinX = FMath::Clamp(Rect.Min.X, 0, Size.X);
		const int32 MinY = FMath::Clamp(Rect.Min.Y, 0, Size.Y);
		const int32 MaxX = FMath::Clamp(Rect.Max.X, 0, Size.X);
		const int32 MaxY = FMath::Clamp(Rect.Max.Y, 0, Size.Y);
		return FIntRect(MinX, MinY, FMath::Max(MinX, MaxX), FMath::Max(MinY, MaxY));
	}

	static FIntRect PadRectToSize(const FIntRect& Rect, const int32 Padding, const FIntPoint& Size)
	{
		const int32 SafePadding = FMath::Clamp(Padding, 0, 1024);
		return ClampRectToSize(
			FIntRect(
				Rect.Min.X - SafePadding,
				Rect.Min.Y - SafePadding,
				Rect.Max.X + SafePadding,
				Rect.Max.Y + SafePadding),
			Size);
	}

	static bool CropPixels(
		const TArray<FColor>& InPixels,
		const FIntPoint& InSize,
		const FIntRect& CropRect,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != InSize.X * InSize.Y)
		{
			OutError = TEXT("invalid_pixels");
			return false;
		}

		const FIntRect SafeRect = ClampRectToSize(CropRect, InSize);
		if (SafeRect.Width() <= 0 || SafeRect.Height() <= 0)
		{
			OutError = TEXT("empty_crop_region");
			return false;
		}

		OutSize = FIntPoint(SafeRect.Width(), SafeRect.Height());
		OutPixels.SetNumUninitialized(OutSize.X * OutSize.Y);
		for (int32 Y = 0; Y < OutSize.Y; ++Y)
		{
			const FColor* SourceRow = InPixels.GetData() + (SafeRect.Min.Y + Y) * InSize.X + SafeRect.Min.X;
			FColor* DestRow = OutPixels.GetData() + Y * OutSize.X;
			FMemory::Memcpy(DestRow, SourceRow, sizeof(FColor) * OutSize.X);
		}
		return true;
	}

	static bool TryParseManualRegion(const TSharedPtr<FJsonObject>& Params, FIntRect& OutRegion, FString& OutError)
	{
		TSharedPtr<FJsonObject> RegionObject;
		if (Params.IsValid())
		{
			const TSharedPtr<FJsonObject>* FoundRegion = nullptr;
			if (Params->TryGetObjectField(TEXT("region"), FoundRegion) && FoundRegion)
			{
				RegionObject = *FoundRegion;
			}
			else if (Params->TryGetObjectField(TEXT("crop"), FoundRegion) && FoundRegion)
			{
				RegionObject = *FoundRegion;
			}
		}

		const TSharedPtr<FJsonObject>& Source = RegionObject.IsValid() ? RegionObject : Params;
		int32 X = 0;
		int32 Y = 0;
		int32 Width = 0;
		int32 Height = 0;
		TryGetIntAny(Source, { TEXT("crop_x"), TEXT("x"), TEXT("left") }, X);
		TryGetIntAny(Source, { TEXT("crop_y"), TEXT("y"), TEXT("top") }, Y);
		const bool bHasWidth = TryGetIntAny(Source, { TEXT("crop_width"), TEXT("width"), TEXT("w") }, Width);
		const bool bHasHeight = TryGetIntAny(Source, { TEXT("crop_height"), TEXT("height"), TEXT("h") }, Height);
		if (!bHasWidth || !bHasHeight)
		{
			OutError = TEXT("missing_crop_width_or_height");
			return false;
		}
		if (Width <= 0 || Height <= 0)
		{
			OutError = TEXT("invalid_crop_size");
			return false;
		}
		OutRegion = FIntRect(X, Y, X + Width, Y + Height);
		return true;
	}

	static bool HasManualRegionInput(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		if (Params->HasTypedField<EJson::Object>(TEXT("region")) || Params->HasTypedField<EJson::Object>(TEXT("crop")))
		{
			return true;
		}
		static const TCHAR* RegionKeys[] = {
			TEXT("crop_x"), TEXT("crop_y"), TEXT("crop_width"), TEXT("crop_height"),
			TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height"),
			TEXT("left"), TEXT("top"), TEXT("w"), TEXT("h")
		};
		for (const TCHAR* Key : RegionKeys)
		{
			if (Params->HasField(Key))
			{
				return true;
			}
		}
		return false;
	}

	static bool ResolveOutputPath(const TSharedPtr<FJsonObject>& Params, const FString& Format, const FString& DefaultPath, FString& OutPath)
	{
		FString OutputFile;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("output_file"), OutputFile);
		}
		OutputFile.TrimStartAndEndInline();
		if (OutputFile.IsEmpty())
		{
			OutPath = DefaultPath;
			return true;
		}

		if (FPaths::IsRelative(OutputFile))
		{
			OutputFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputFile);
		}
		if (FPaths::GetExtension(OutputFile).IsEmpty())
		{
			OutputFile += TEXT(".") + Format;
		}
		OutPath = OutputFile;
		return true;
	}
}

bool FUeAgentHttpServer::CmdUmgScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentUmgScreenshotOps;

	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings || !Settings->bEnableScreenshots)
	{
		OutError = TEXT("screenshots_disabled");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprint(AssetPathInput);
	if (!WidgetBlueprint)
	{
		OutError = TEXT("widget_blueprint_not_found");
		return false;
	}

	int32 CaptureWidth = 1920;
	int32 CaptureHeight = 1080;
	double CaptureWidthValue = CaptureWidth;
	double CaptureHeightValue = CaptureHeight;
	if (JsonTryGetNumber(Ctx.Params, TEXT("capture_width"), CaptureWidthValue))
	{
		CaptureWidth = FMath::Clamp(FMath::RoundToInt(CaptureWidthValue), 64, 8192);
	}
	if (JsonTryGetNumber(Ctx.Params, TEXT("capture_height"), CaptureHeightValue))
	{
		CaptureHeight = FMath::Clamp(FMath::RoundToInt(CaptureHeightValue), 64, 8192);
	}
	const FVector2D DrawSize(CaptureWidth, CaptureHeight);
	const FIntPoint CanvasSize(CaptureWidth, CaptureHeight);

	FString Format = TEXT("png");
	JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	Format = Format.TrimStartAndEnd().ToLower();
	if (Format.IsEmpty())
	{
		Format = TEXT("png");
	}
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = TEXT("unsupported_format");
		return false;
	}

	int32 Quality = 90;
	double QualityValue = Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityValue))
	{
		Quality = FMath::Clamp(FMath::RoundToInt(QualityValue), 1, 100);
	}

	int32 MaxSize = 2048;
	double MaxSizeValue = MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeValue))
	{
		MaxSize = FMath::Clamp(FMath::RoundToInt(MaxSizeValue), 64, 8192);
	}

	int32 BoundsPadding = 0;
	double BoundsPaddingValue = BoundsPadding;
	if (JsonTryGetNumber(Ctx.Params, TEXT("bounds_padding"), BoundsPaddingValue) ||
		JsonTryGetNumber(Ctx.Params, TEXT("padding"), BoundsPaddingValue))
	{
		BoundsPadding = FMath::Clamp(FMath::RoundToInt(BoundsPaddingValue), 0, 1024);
	}

	int32 BoundsAlphaThreshold = 0;
	double BoundsAlphaThresholdValue = BoundsAlphaThreshold;
	if (JsonTryGetNumber(Ctx.Params, TEXT("bounds_alpha_threshold"), BoundsAlphaThresholdValue) ||
		JsonTryGetNumber(Ctx.Params, TEXT("alpha_threshold"), BoundsAlphaThresholdValue))
	{
		BoundsAlphaThreshold = FMath::Clamp(FMath::RoundToInt(BoundsAlphaThresholdValue), 0, 255);
	}

	FString CropMode = TEXT("auto_bounds");
	JsonTryGetString(Ctx.Params, TEXT("crop_mode"), CropMode);
	CropMode = CropMode.TrimStartAndEnd().ToLower();
	if (CropMode.IsEmpty())
	{
		CropMode = TEXT("auto_bounds");
	}
	const bool bManualRegionInput = HasManualRegionInput(Ctx.Params);
	if (bManualRegionInput && CropMode == TEXT("auto_bounds"))
	{
		CropMode = TEXT("manual_region");
	}
	if (CropMode == TEXT("auto"))
	{
		CropMode = TEXT("auto_bounds");
	}
	else if (CropMode == TEXT("manual") || CropMode == TEXT("region"))
	{
		CropMode = TEXT("manual_region");
	}
	else if (CropMode == TEXT("full"))
	{
		CropMode = TEXT("full_canvas");
	}
	if (CropMode != TEXT("auto_bounds") && CropMode != TEXT("manual_region") && CropMode != TEXT("full_canvas"))
	{
		OutError = TEXT("invalid_crop_mode");
		return false;
	}

	const EBlueprintStatus StatusBefore = WidgetBlueprint->Status;
	bool bCompileBeforeCapture = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_before_capture"), bCompileBeforeCapture);
	const bool bClassMissing = WidgetBlueprint->GeneratedClass == nullptr || !WidgetBlueprint->GeneratedClass->IsChildOf(UUserWidget::StaticClass());
	const bool bNeedsCompile = bCompileBeforeCapture || bClassMissing || WidgetBlueprint->Status == BS_Dirty || WidgetBlueprint->Status == BS_Error;
	if (bNeedsCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	const EBlueprintStatus StatusAfterCompile = WidgetBlueprint->Status;
	if (StatusAfterCompile == BS_Error)
	{
		OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("blueprint_status_before"), BlueprintStatusToString(StatusBefore));
		OutData->SetStringField(TEXT("blueprint_status_after_compile"), BlueprintStatusToString(StatusAfterCompile));
		OutData->SetBoolField(TEXT("compiled_before_capture"), bNeedsCompile);
		OutError = TEXT("compile_failed_before_capture");
		return false;
	}

	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		OutError = TEXT("generated_user_widget_class_not_found");
		return false;
	}

	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError.IsEmpty() ? TEXT("editor_world_not_found") : WorldError;
		return false;
	}

	UUserWidget* UserWidget = CreateWidget<UUserWidget>(World, WidgetClass, NAME_None);
	if (!UserWidget)
	{
		OutError = TEXT("create_user_widget_failed");
		return false;
	}
	TStrongObjectPtr<UUserWidget> UserWidgetGuard(UserWidget);
	UserWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	UserWidget->SetDesiredSizeInViewport(DrawSize);

	TSharedRef<SWidget> SlateWidget = UserWidget->TakeWidget();
	UserWidget->ForceLayoutPrepass();

	TArray<FColor> FullPixels;
	FIntPoint FullSize(0, 0);
	if (!ScreenshotWidgetWithDrawSize(SlateWidget, DrawSize, FullPixels, FullSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(
			TEXT("CmdUmgScreenshot failed render asset=%s error=%s"),
			*WidgetBlueprint->GetOutermost()->GetName(),
			*OutError);
		return false;
	}

	FIntRect ContentBounds(0, 0, FullSize.X, FullSize.Y);
	bool bAutoBoundsFound = false;
	if (FindAlphaBounds(FullPixels, FullSize, BoundsAlphaThreshold, ContentBounds))
	{
		bAutoBoundsFound = true;
	}

	FIntRect CropRect(0, 0, FullSize.X, FullSize.Y);
	if (CropMode == TEXT("auto_bounds"))
	{
		CropRect = bAutoBoundsFound ? PadRectToSize(ContentBounds, BoundsPadding, FullSize) : FIntRect(0, 0, FullSize.X, FullSize.Y);
	}
	else if (CropMode == TEXT("manual_region"))
	{
		if (!TryParseManualRegion(Ctx.Params, CropRect, OutError))
		{
			return false;
		}
		CropRect = ClampRectToSize(CropRect, FullSize);
		if (CropRect.Width() <= 0 || CropRect.Height() <= 0)
		{
			OutError = TEXT("empty_crop_region");
			return false;
		}
	}

	TArray<FColor> CroppedPixels;
	FIntPoint CroppedSize(0, 0);
	if (!CropPixels(FullPixels, FullSize, CropRect, CroppedPixels, CroppedSize, OutError))
	{
		return false;
	}

	TArray<FColor> OutputPixels;
	FIntPoint OutputSize(0, 0);
	if (!ResizePixelsMaxSize(CroppedPixels, CroppedSize, MaxSize, OutputPixels, OutputSize, OutError))
	{
		return false;
	}

	FString OutPath;
	ResolveOutputPath(Ctx.Params, Format, MakeShotPath(Format), OutPath);
	const bool bPreserveAlpha = Format == TEXT("png") || Format == TEXT("webp");
	if (!WriteCompressedImage(OutputPixels, OutputSize, Format, Quality, OutPath, OutError, !bPreserveAlpha))
	{
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
	OutData->SetStringField(TEXT("asset_path"), WidgetBlueprint->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("umg_widget_offscreen"));
	OutData->SetStringField(TEXT("crop_mode"), CropMode);
	OutData->SetObjectField(TEXT("canvas"), SizeToJson(FullSize));
	OutData->SetObjectField(TEXT("content_bounds"), RectToJson(ContentBounds));
	OutData->SetObjectField(TEXT("crop"), RectToJson(CropRect));
	OutData->SetNumberField(TEXT("width"), OutputSize.X);
	OutData->SetNumberField(TEXT("height"), OutputSize.Y);
	OutData->SetNumberField(TEXT("raw_width"), CroppedSize.X);
	OutData->SetNumberField(TEXT("raw_height"), CroppedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
	OutData->SetNumberField(TEXT("max_size"), MaxSize);
	OutData->SetNumberField(TEXT("quality"), Quality);
	OutData->SetNumberField(TEXT("bounds_padding"), BoundsPadding);
	OutData->SetNumberField(TEXT("bounds_alpha_threshold"), BoundsAlphaThreshold);
	OutData->SetBoolField(TEXT("auto_bounds_found"), bAutoBoundsFound);
	OutData->SetBoolField(TEXT("compiled_before_capture"), bNeedsCompile);
	OutData->SetBoolField(TEXT("preserve_alpha"), bPreserveAlpha);
	OutData->SetStringField(TEXT("blueprint_status_before"), BlueprintStatusToString(StatusBefore));
	OutData->SetStringField(TEXT("blueprint_status_after_compile"), BlueprintStatusToString(StatusAfterCompile));
	OutData->SetStringField(TEXT("generated_class"), WidgetClass->GetPathName());
	OutData->SetNumberField(TEXT("scale"), CroppedSize.X > 0 ? static_cast<double>(OutputSize.X) / static_cast<double>(CroppedSize.X) : 1.0);
	return true;
}
