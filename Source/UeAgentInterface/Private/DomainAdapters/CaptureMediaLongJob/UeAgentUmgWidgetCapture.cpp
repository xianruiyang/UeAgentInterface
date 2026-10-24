// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/CaptureMediaLongJob/UeAgentUmgWidgetCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "Misc/ScopeExit.h"
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Engine/UserInterfaceSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UObject/StrongObjectPtr.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgScreenshotOps {
using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob::CaptureDiagnostics;
static FString NormalizeAssetPath(const FString &InPath) {
  FString OutPath = InPath;
  OutPath.TrimStartAndEndInline();

  int32 DotIndex = INDEX_NONE;
  if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0) {
    OutPath = OutPath.Left(DotIndex);
  }
  return OutPath;
}

static FString ToObjectPath(const FString &InPath) {
  FString InObjectPath = InPath;
  InObjectPath.TrimStartAndEndInline();
  if (InObjectPath.IsEmpty()) {
    return FString();
  }
  if (InObjectPath.Contains(TEXT("."))) {
    return InObjectPath;
  }

  const FString AssetPath = NormalizeAssetPath(InObjectPath);
  const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
  if (AssetName.IsEmpty()) {
    return FString();
  }
  return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static UWidgetBlueprint *LoadWidgetBlueprint(const FString &InPath) {
  const FString ObjectPath = ToObjectPath(InPath);
  if (ObjectPath.IsEmpty()) {
    return nullptr;
  }
  if (UWidgetBlueprint *Existing =
          FindObject<UWidgetBlueprint>(nullptr, *ObjectPath)) {
    return Existing;
  }
  return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
}

static FString BlueprintStatusToString(const EBlueprintStatus InStatus) {
  switch (InStatus) {
  case BS_Unknown:
    return TEXT("Unknown");
  case BS_Dirty:
    return TEXT("Dirty");
  case BS_Error:
    return TEXT("Error");
  case BS_UpToDate:
    return TEXT("UpToDate");
  case BS_BeingCreated:
    return TEXT("BeingCreated");
  case BS_UpToDateWithWarnings:
    return TEXT("UpToDateWithWarnings");
  default:
    return TEXT("Unknown");
  }
}

static bool TryGetNumberAny(const TSharedPtr<FJsonObject> &Obj,
                            const TArray<FString> &Keys, double &OutValue, FString* OutField = nullptr) {
  if (OutField) OutField->Reset();
  if (!Obj.IsValid()) {
    return false;
  }
  for (const FString &Key : Keys) {
    if (Obj->TryGetNumberField(Key, OutValue)) {
      if (OutField) *OutField = Key;
      return true;
    }
  }
  return false;
}

static bool TryGetIntAny(const TSharedPtr<FJsonObject> &Obj,
                         const TArray<FString> &Keys, int32 &OutValue, FString* OutField = nullptr) {
  double NumberValue = 0.0;
  if (!TryGetNumberAny(Obj, Keys, NumberValue, OutField)) {
    return false;
  }
  OutValue = FMath::RoundToInt(NumberValue);
  return true;
}

static TSharedPtr<FJsonObject> RectToJson(const FIntRect &Rect) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetNumberField(TEXT("x"), Rect.Min.X);
  Obj->SetNumberField(TEXT("y"), Rect.Min.Y);
  Obj->SetNumberField(TEXT("width"), Rect.Width());
  Obj->SetNumberField(TEXT("height"), Rect.Height());
  Obj->SetNumberField(TEXT("right"), Rect.Max.X);
  Obj->SetNumberField(TEXT("bottom"), Rect.Max.Y);
  return Obj;
}

static TSharedPtr<FJsonObject> SizeToJson(const FIntPoint &Size) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetNumberField(TEXT("width"), Size.X);
  Obj->SetNumberField(TEXT("height"), Size.Y);
  return Obj;
}

static bool FindAlphaBounds(const TArray<FColor> &Pixels, const FIntPoint &Size,
                            const int32 AlphaThreshold, FIntRect &OutBounds) {
  if (Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != Size.X * Size.Y) {
    return false;
  }

  int32 MinX = Size.X;
  int32 MinY = Size.Y;
  int32 MaxX = -1;
  int32 MaxY = -1;
  const int32 Threshold = FMath::Clamp(AlphaThreshold, 0, 255);
  for (int32 Y = 0; Y < Size.Y; ++Y) {
    const int32 RowOffset = Y * Size.X;
    for (int32 X = 0; X < Size.X; ++X) {
      if (Pixels[RowOffset + X].A <= Threshold) {
        continue;
      }
      MinX = FMath::Min(MinX, X);
      MinY = FMath::Min(MinY, Y);
      MaxX = FMath::Max(MaxX, X);
      MaxY = FMath::Max(MaxY, Y);
    }
  }

  if (MaxX < MinX || MaxY < MinY) {
    return false;
  }
  OutBounds = FIntRect(MinX, MinY, MaxX + 1, MaxY + 1);
  return true;
}

static FIntRect ClampRectToSize(const FIntRect &Rect, const FIntPoint &Size) {
  const int32 MinX = FMath::Clamp(Rect.Min.X, 0, Size.X);
  const int32 MinY = FMath::Clamp(Rect.Min.Y, 0, Size.Y);
  const int32 MaxX = FMath::Clamp(Rect.Max.X, 0, Size.X);
  const int32 MaxY = FMath::Clamp(Rect.Max.Y, 0, Size.Y);
  return FIntRect(MinX, MinY, FMath::Max(MinX, MaxX), FMath::Max(MinY, MaxY));
}

static FIntRect PadRectToSize(const FIntRect &Rect, const int32 Padding,
                              const FIntPoint &Size) {
	const int32 SafePadding = FMath::Max(Padding, 0);
  return ClampRectToSize(
      FIntRect(Rect.Min.X - SafePadding, Rect.Min.Y - SafePadding,
               Rect.Max.X + SafePadding, Rect.Max.Y + SafePadding),
      Size);
}

static bool CropPixels(const TArray<FColor> &InPixels, const FIntPoint &InSize,
                       const FIntRect &CropRect, TArray<FColor> &OutPixels,
                       FIntPoint &OutSize, UE::UAI::Contract::FDiagnostic& OutError) {
  if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != InSize.X * InSize.Y) {
    OutError = Native(TEXT("invalid_pixels"), TEXT("The widget image dimensions or pixel count are invalid."),
        TEXT("widget_crop"), TEXT("image"), Facts({ { TEXT("width"), Number(InSize.X) }, { TEXT("height"), Number(InSize.Y) }, { TEXT("pixel_count"), Number(InPixels.Num()) } }),
        MakeShared<FJsonValueString>(TEXT("positive dimensions and pixel_count == width * height")));
    return false;
  }

  const FIntRect SafeRect = ClampRectToSize(CropRect, InSize);
  if (SafeRect.Width() <= 0 || SafeRect.Height() <= 0) {
    OutError = Native(TEXT("empty_crop_region"), TEXT("The widget crop rectangle has no area inside the image."),
        TEXT("widget_crop"), TEXT("crop"), MakeShared<FJsonValueObject>(RectToJson(SafeRect)), Size(InSize));
    return false;
  }

  OutSize = FIntPoint(SafeRect.Width(), SafeRect.Height());
  OutPixels.SetNumUninitialized(OutSize.X * OutSize.Y);
  for (int32 Y = 0; Y < OutSize.Y; ++Y) {
    const FColor *SourceRow =
        InPixels.GetData() + (SafeRect.Min.Y + Y) * InSize.X + SafeRect.Min.X;
    FColor *DestRow = OutPixels.GetData() + Y * OutSize.X;
    FMemory::Memcpy(DestRow, SourceRow, sizeof(FColor) * OutSize.X);
  }
  return true;
}

static bool TryParseManualRegion(const TSharedPtr<FJsonObject> &Params,
                                 FIntRect &OutRegion, UE::UAI::Contract::FDiagnostic& OutError, FString& OutPointer) {
  TSharedPtr<FJsonObject> RegionObject;
  OutPointer.Reset();
  if (Params.IsValid()) {
    const TSharedPtr<FJsonObject> *FoundRegion = nullptr;
    if (Params->TryGetObjectField(TEXT("region"), FoundRegion) && FoundRegion) {
      RegionObject = *FoundRegion;
      if (RegionObject.IsValid()) OutPointer = TEXT("/region");
    } else if (Params->TryGetObjectField(TEXT("crop"), FoundRegion) && FoundRegion) {
      RegionObject = *FoundRegion;
      if (RegionObject.IsValid()) OutPointer = TEXT("/crop");
    }
  }

  const TSharedPtr<FJsonObject> &Source = RegionObject.IsValid() ? RegionObject : Params;
  int32 X = 0, Y = 0, Width = 0, Height = 0;
  FString WidthField, HeightField;
  TryGetIntAny(Source, {TEXT("crop_x"), TEXT("x"), TEXT("left")}, X);
  TryGetIntAny(Source, {TEXT("crop_y"), TEXT("y"), TEXT("top")}, Y);
  const bool bHasWidth = TryGetIntAny(Source, {TEXT("crop_width"), TEXT("width"), TEXT("w")}, Width, &WidthField);
  const bool bHasHeight = TryGetIntAny(Source, {TEXT("crop_height"), TEXT("height"), TEXT("h")}, Height, &HeightField);
  if (!bHasWidth || !bHasHeight) {
    const bool bWidthMissing = !bHasWidth;
    const TCHAR* First = bWidthMissing ? TEXT("crop_width") : TEXT("crop_height");
    const TCHAR* Second = bWidthMissing ? TEXT("width") : TEXT("height");
    const TCHAR* Third = bWidthMissing ? TEXT("w") : TEXT("h");
    OutError = Input(TEXT("missing_crop_width_or_height"),
        bWidthMissing ? TEXT("The widget crop requires a numeric width.") : TEXT("The widget crop requires a numeric height."),
        TEXT("widget_crop"), OutPointer,
        Facts({ { First, Source.IsValid() ? Source->TryGetField(First) : nullptr },
          { Second, Source.IsValid() ? Source->TryGetField(Second) : nullptr },
          { Third, Source.IsValid() ? Source->TryGetField(Third) : nullptr } }),
        MakeShared<FJsonValueString>(bWidthMissing ? TEXT("numeric crop_width, width or w") : TEXT("numeric crop_height, height or h")));
    return false;
  }
  if (Width <= 0 || Height <= 0) {
    const FString& Field = Width <= 0 ? WidthField : HeightField;
    OutError = Input(TEXT("invalid_crop_size"), TEXT("The rounded widget crop dimension must be positive."),
        TEXT("widget_crop"), OutPointer + TEXT("/") + Field,
        Facts({ { TEXT("input_value"), Source->TryGetField(Field) }, { TEXT("rounded_size"), Number(Width <= 0 ? Width : Height) } }),
        MakeShared<FJsonValueString>(TEXT("rounded dimension > 0")));
    return false;
  }
  OutRegion = FIntRect(X, Y, X + Width, Y + Height);
  return true;
}

static bool HasManualRegionInput(const TSharedPtr<FJsonObject> &Params) {
  if (!Params.IsValid()) {
    return false;
  }
  if (Params->HasTypedField<EJson::Object>(TEXT("region")) ||
      Params->HasTypedField<EJson::Object>(TEXT("crop"))) {
    return true;
  }
  static const TCHAR *RegionKeys[] = {
      TEXT("crop_x"), TEXT("crop_y"), TEXT("crop_width"), TEXT("crop_height"),
      TEXT("x"),      TEXT("y"),      TEXT("width"),      TEXT("height"),
      TEXT("left"),   TEXT("top"),    TEXT("w"),          TEXT("h")};
  for (const TCHAR *Key : RegionKeys) {
    if (Params->HasField(Key)) {
      return true;
    }
  }
  return false;
}

static bool ResolveOutputPath(const TSharedPtr<FJsonObject> &Params,
                              const FString &Format, const FString &DefaultPath,
                              FString &OutPath) {
  FString OutputFile;
  if (Params.IsValid()) {
    Params->TryGetStringField(TEXT("output_file"), OutputFile);
  }
  OutputFile.TrimStartAndEndInline();
  if (OutputFile.IsEmpty()) {
    OutPath = DefaultPath;
    return true;
  }

  if (FPaths::IsRelative(OutputFile)) {
    OutputFile =
        FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputFile);
  }
  if (FPaths::GetExtension(OutputFile).IsEmpty()) {
    OutputFile += TEXT(".") + Format;
  }
  OutPath = OutputFile;
  return true;
}
} // namespace UeAgentUmgScreenshotOps



bool UE::UAI::DomainAdapters::CaptureMediaLongJob::TryCaptureUmgWidgetImage(
    const TSharedPtr<FJsonObject> &Parameters, const FString &,
    TSharedPtr<FJsonObject> &OutData, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentUmgScreenshotOps;
  const TSharedPtr<FJsonObject> &Params = Parameters;

  if (!OutData.IsValid()) {
    OutData = MakeShared<FJsonObject>();
  }

  FString AssetPathInput;
  if (!Params.IsValid() ||
      !Params->TryGetStringField(TEXT("asset_path"), AssetPathInput) ||
      AssetPathInput.IsEmpty()) {
    OutError = Input(TEXT("missing_asset_path"), TEXT("Widget capture requires a non-empty asset_path."),
        TEXT("widget_capture"), TEXT("/asset_path"), Params.IsValid() ? Params->TryGetField(TEXT("asset_path")) : nullptr,
        MakeShared<FJsonValueString>(TEXT("non-empty WidgetBlueprint asset path")));
    return false;
  }

  ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPathInput; };
  UWidgetBlueprint *WidgetBlueprint = LoadWidgetBlueprint(AssetPathInput);
  if (!WidgetBlueprint) {
    OutError = Input(TEXT("widget_blueprint_not_found"), TEXT("The requested WidgetBlueprint asset could not be loaded."),
        TEXT("widget_capture"), TEXT("/asset_path"), MakeShared<FJsonValueString>(AssetPathInput), MakeShared<FJsonValueString>(TEXT("loadable WidgetBlueprint asset")));
    return false;
  }

  int32 CaptureWidth = 1920;
  int32 CaptureHeight = 1080;
  double CaptureWidthValue = CaptureWidth;
  double CaptureHeightValue = CaptureHeight;
  if (Params->TryGetNumberField(TEXT("capture_width"), CaptureWidthValue)) {
    CaptureWidth = FMath::Max(FMath::RoundToInt(CaptureWidthValue), 1);
  }
  if (Params->TryGetNumberField(TEXT("capture_height"), CaptureHeightValue)) {
    CaptureHeight =
        FMath::Max(FMath::RoundToInt(CaptureHeightValue), 1);
  }
  const FVector2D DrawSize(CaptureWidth, CaptureHeight);
  const FIntPoint CanvasSize(CaptureWidth, CaptureHeight);
  bool bUseProjectDpiScale = false;
  Params->TryGetBoolField(TEXT("use_project_dpi_scale"), bUseProjectDpiScale);
  const float DpiScale = bUseProjectDpiScale
      ? GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(CanvasSize)
      : 1.0f;

  FString Format = TEXT("png");
  Params->TryGetStringField(TEXT("format"), Format);
  Format = Format.TrimStartAndEnd().ToLower();
  if (Format.IsEmpty()) {
    Format = TEXT("png");
  }
  if (Format == TEXT("jpeg")) {
    Format = TEXT("jpg");
  }
  if (Format != TEXT("png") && Format != TEXT("jpg") &&
      Format != TEXT("webp")) {
    OutError = Input(TEXT("unsupported_format"), TEXT("The widget capture image format is unsupported."),
        TEXT("widget_capture"), TEXT("/format"), Params->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg or webp")));
    return false;
  }

  int32 Quality = 90;
  double QualityValue = Quality;
  if (Params->TryGetNumberField(TEXT("quality"), QualityValue)) {
    Quality = FMath::Clamp(FMath::RoundToInt(QualityValue), 1, 100);
  }

  int32 MaxSize = 0;
  double MaxSizeValue = MaxSize;
  if (Params->TryGetNumberField(TEXT("max_size"), MaxSizeValue)) {
    MaxSize = FMath::Max(FMath::RoundToInt(MaxSizeValue), 1);
  }

  int32 BoundsPadding = 0;
  double BoundsPaddingValue = BoundsPadding;
  if (Params->TryGetNumberField(TEXT("bounds_padding"), BoundsPaddingValue) ||
      Params->TryGetNumberField(TEXT("padding"), BoundsPaddingValue)) {
    BoundsPadding =
		FMath::Max(FMath::RoundToInt(BoundsPaddingValue), 0);
  }

  int32 BoundsAlphaThreshold = 0;
  double BoundsAlphaThresholdValue = BoundsAlphaThreshold;
  if (Params->TryGetNumberField(TEXT("bounds_alpha_threshold"),
                                BoundsAlphaThresholdValue) ||
      Params->TryGetNumberField(TEXT("alpha_threshold"),
                                BoundsAlphaThresholdValue)) {
    BoundsAlphaThreshold =
        FMath::Clamp(FMath::RoundToInt(BoundsAlphaThresholdValue), 0, 255);
  }

  FString CropMode = TEXT("auto_bounds");
  Params->TryGetStringField(TEXT("crop_mode"), CropMode);
  CropMode = CropMode.TrimStartAndEnd().ToLower();
  if (CropMode.IsEmpty()) {
    CropMode = TEXT("auto_bounds");
  }
  const bool bManualRegionInput = HasManualRegionInput(Params);
  if (bManualRegionInput && CropMode == TEXT("auto_bounds")) {
    CropMode = TEXT("manual_region");
  }
  if (CropMode == TEXT("auto")) {
    CropMode = TEXT("auto_bounds");
  } else if (CropMode == TEXT("manual") || CropMode == TEXT("region")) {
    CropMode = TEXT("manual_region");
  } else if (CropMode == TEXT("full")) {
    CropMode = TEXT("full_canvas");
  }
  if (CropMode != TEXT("auto_bounds") && CropMode != TEXT("manual_region") &&
      CropMode != TEXT("full_canvas")) {
    OutError = Input(TEXT("invalid_crop_mode"), TEXT("The widget capture crop mode is unsupported."),
        TEXT("widget_capture"), TEXT("/crop_mode"), Params->TryGetField(TEXT("crop_mode")),
        MakeShared<FJsonValueString>(TEXT("auto_bounds/auto, manual_region/manual/region or full_canvas/full")));
    return false;
  }

  const EBlueprintStatus StatusBefore = WidgetBlueprint->Status;
  const bool bClassMissing =
      WidgetBlueprint->GeneratedClass == nullptr ||
      !WidgetBlueprint->GeneratedClass->IsChildOf(UUserWidget::StaticClass());
  // Compiling an unchanged class can reinstance its live PIE widgets. Capture
  // creates an isolated design widget and only needs to prepare a stale class.
  const bool bNeedsCompile = bClassMissing || StatusBefore == BS_Dirty ||
                             StatusBefore == BS_Error;
  if (bNeedsCompile) {
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
  }
  const EBlueprintStatus StatusAfterCompile = WidgetBlueprint->Status;
  if (StatusAfterCompile == BS_Error) {
    OutData->SetStringField(TEXT("asset_path"),
                            WidgetBlueprint->GetOutermost()->GetName());
    OutData->SetStringField(TEXT("blueprint_status_before"),
                            BlueprintStatusToString(StatusBefore));
    OutData->SetStringField(TEXT("blueprint_status_after_compile"),
                            BlueprintStatusToString(StatusAfterCompile));
    OutData->SetBoolField(TEXT("compiled_before_capture"), bNeedsCompile);
    OutError = Native(TEXT("compile_failed_before_capture"), TEXT("The WidgetBlueprint remains in the Error state after compilation."),
        TEXT("widget_capture"), TEXT("blueprint_status"), MakeShared<FJsonValueString>(BlueprintStatusToString(StatusAfterCompile)),
        MakeShared<FJsonValueString>(TEXT("status other than Error")));
    return false;
  }

  UClass *WidgetClass = WidgetBlueprint->GeneratedClass;
  if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass())) {
    OutError = Native(TEXT("generated_user_widget_class_not_found"), TEXT("The WidgetBlueprint has no generated UUserWidget class."),
        TEXT("widget_capture"), TEXT("generated_class"), WidgetClass ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(WidgetClass->GetPathName())) : nullptr,
        MakeShared<FJsonValueString>(UUserWidget::StaticClass()->GetPathName()));
    return false;
  }

  UWorld *World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
  if (!World) {
    OutError = Native(TEXT("editor_world_not_found"),
        GEditor ? TEXT("The Editor world is unavailable for widget capture.") : TEXT("Widget capture requires an active Editor."),
        TEXT("widget_capture"), GEditor ? TEXT("editor_world_available") : TEXT("editor_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }

  UUserWidget *UserWidget =
      CreateWidget<UUserWidget>(World, WidgetClass, NAME_None);
  if (!UserWidget) {
    OutError = Native(TEXT("create_user_widget_failed"),
        FString::Printf(TEXT("The Editor could not create a capture widget of class %s."), *WidgetClass->GetPathName()),
        TEXT("widget_capture"), TEXT("widget_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  TStrongObjectPtr<UUserWidget> UserWidgetGuard(UserWidget);
  UserWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
  UserWidget->SetDesiredSizeInViewport(DrawSize);

  TSharedRef<SWidget> SlateWidget = UserWidget->TakeWidget();
  UserWidget->ForceLayoutPrepass();

  TArray<FColor> FullPixels;
  FIntPoint FullSize(0, 0);
  if (!UeAgentSlateWidgetCapture::CaptureWidgetPixels(SlateWidget, DrawSize, DpiScale, FullPixels, FullSize,
                                    OutError)) {
    FUeAgentInterfaceLogger::Log(
			TEXT("UmgWidgetCapture failed render asset=%s error=%s"),
        *WidgetBlueprint->GetOutermost()->GetName(), *OutError.ToPublicDetail());
    return false;
  }

  FIntRect ContentBounds(0, 0, FullSize.X, FullSize.Y);
  bool bAutoBoundsFound = false;
  if (FindAlphaBounds(FullPixels, FullSize, BoundsAlphaThreshold,
                      ContentBounds)) {
    bAutoBoundsFound = true;
  }

  FIntRect CropRect(0, 0, FullSize.X, FullSize.Y);
  if (CropMode == TEXT("auto_bounds")) {
    CropRect = bAutoBoundsFound
                   ? PadRectToSize(ContentBounds, BoundsPadding, FullSize)
                   : FIntRect(0, 0, FullSize.X, FullSize.Y);
  } else if (CropMode == TEXT("manual_region")) {
    FString CropPointer;
    if (!TryParseManualRegion(Params, CropRect, OutError, CropPointer)) {
      return false;
    }
    const FIntRect RequestedCrop = CropRect;
    CropRect = ClampRectToSize(CropRect, FullSize);
    if (CropRect.Width() <= 0 || CropRect.Height() <= 0) {
      OutError = Input(TEXT("empty_crop_region"), TEXT("The requested crop rectangle does not overlap the captured widget image."),
          TEXT("widget_crop"), CropPointer, MakeShared<FJsonValueObject>(RectToJson(RequestedCrop)), Size(FullSize));
      return false;
    }
  }

  TArray<FColor> CroppedPixels;
  FIntPoint CroppedSize(0, 0);
  if (!CropPixels(FullPixels, FullSize, CropRect, CroppedPixels, CroppedSize,
                  OutError)) {
    return false;
  }

  TArray<FColor> OutputPixels;
  FIntPoint OutputSize(0, 0);
  if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::
          ResizePixelsMaxSize(CroppedPixels, CroppedSize, MaxSize, OutputPixels,
                              OutputSize, OutError)) {
    return false;
  }

  FString OutPath;
  ResolveOutputPath(Params, Format, FViewportCapture::MakeShotPath(Format),
                    OutPath);
  const bool bPreserveAlpha = Format == TEXT("png") || Format == TEXT("webp");
  if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::
          WriteCompressedImage(OutputPixels, OutputSize, Format, Quality,
                               OutPath, OutError, !bPreserveAlpha)) {
    return false;
  }

  const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
  OutData->SetStringField(TEXT("asset_path"),
                          WidgetBlueprint->GetOutermost()->GetName());
  OutData->SetStringField(TEXT("file_path"), OutPath);
  OutData->SetStringField(TEXT("format"), Format);
  OutData->SetStringField(TEXT("capture_mode"), TEXT("umg_widget_offscreen"));
  OutData->SetBoolField(TEXT("use_project_dpi_scale"), bUseProjectDpiScale);
  OutData->SetNumberField(TEXT("dpi_scale"), DpiScale);
  OutData->SetStringField(TEXT("widget_execution_mode"), TEXT("design_time"));
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
  OutData->SetStringField(TEXT("blueprint_status_before"),
                          BlueprintStatusToString(StatusBefore));
  OutData->SetStringField(TEXT("blueprint_status_after_compile"),
                          BlueprintStatusToString(StatusAfterCompile));
  OutData->SetStringField(TEXT("generated_class"), WidgetClass->GetPathName());
  OutData->SetNumberField(
      TEXT("scale"), CroppedSize.X > 0 ? static_cast<double>(OutputSize.X) /
                                             static_cast<double>(CroppedSize.X)
                                       : 1.0);
  return true;
}
