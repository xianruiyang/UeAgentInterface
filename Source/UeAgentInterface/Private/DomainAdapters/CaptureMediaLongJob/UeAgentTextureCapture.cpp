#include "DomainAdapters/CaptureMediaLongJob/UeAgentTextureCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Dom/JsonObject.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentTextureSourceImage.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "Engine/Texture.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob {
namespace {
FString NormalizeAssetPath(const FString &Input) {
  FString Path = Input;
  Path.TrimStartAndEndInline();
  int32 DotIndex = INDEX_NONE;
  if (Path.FindChar(TEXT('.'), DotIndex) && DotIndex > 0) {
    Path = Path.Left(DotIndex);
  }
  return Path;
}

FString ToObjectPath(const FString &Input) {
  FString Path = Input;
  Path.TrimStartAndEndInline();
  if (Path.IsEmpty() || Path.Contains(TEXT("."))) {
    return Path;
  }
  const FString AssetPath = NormalizeAssetPath(Path);
  const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
  return AssetName.IsEmpty()
             ? FString()
             : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

UTexture *LoadTexture(const FString &AssetPath) {
  const FString ObjectPath = ToObjectPath(AssetPath);
  if (ObjectPath.IsEmpty()) {
    return nullptr;
  }
  if (UTexture *Existing = FindObject<UTexture>(nullptr, *ObjectPath)) {
    return Existing;
  }
  return LoadObject<UTexture>(nullptr, *ObjectPath);
}

bool TryGetOptionalInt(const TSharedPtr<FJsonObject> &Parameters,
                       const TCHAR *Field, int32 &InOutValue,
                       UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Parameters.IsValid() || !Parameters->HasField(Field)) {
    return true;
  }
  int64 Value = InOutValue;
  UE::UAI::Contract::FDiagnostic NumberError;
  if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(Parameters->TryGetField(Field),
                                                     MIN_int32, MAX_int32, Value, NumberError)) {
    OutError = MoveTemp(NumberError);
    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field;
    CaptureDiagnostics::LocateInput(OutError, TEXT("/") + FString(Field));
    return false;
  }
  InOutValue = static_cast<int32>(Value);
  return true;
}
} // namespace

bool TryCaptureTextureImage(const TSharedPtr<FJsonObject> &Parameters,
                            const FString &, TSharedPtr<FJsonObject> &OutData,
                            UE::UAI::Contract::FDiagnostic& OutError) {
  FString AssetPath;
  if (!Parameters.IsValid() ||
      !Parameters->TryGetStringField(TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = CaptureDiagnostics::Input(TEXT("missing_asset_path"), TEXT("Texture capture requires a non-empty asset_path."), TEXT("input_decode"), TEXT("/asset_path"), Parameters.IsValid() ? Parameters->TryGetField(TEXT("asset_path")) : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty texture asset path")));
    return false;
  }
  AssetPath.TrimStartAndEndInline();
  UTexture *Texture = LoadTexture(AssetPath);
  if (!Texture) {
    OutError = CaptureDiagnostics::Input(TEXT("texture_not_found"), TEXT("The requested asset could not be loaded as a texture."), TEXT("asset_resolve"), TEXT("/asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("loadable texture asset")));
    OutError.TargetPath = AssetPath;
    return false;
  }

  FString Format = TEXT("png");
  Parameters->TryGetStringField(TEXT("format"), Format);
  Format = Format.ToLower();
  FString OutputFile;
  Parameters->TryGetStringField(TEXT("output_file"), OutputFile);
  if (OutputFile.TrimStartAndEnd().IsEmpty()) {
    OutputFile = FViewportCapture::MakeShotPath(Format);
  } else {
    OutputFile =
        UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);
  }

  int32 MipIndex = 0;
  int32 SliceIndex = 0;
  int32 MaxSize = 0;
  if (!TryGetOptionalInt(Parameters, TEXT("mip_index"), MipIndex, OutError) ||
      !TryGetOptionalInt(Parameters, TEXT("slice"), SliceIndex, OutError) ||
      !TryGetOptionalInt(Parameters, TEXT("face"), SliceIndex, OutError) ||
      !TryGetOptionalInt(Parameters, TEXT("max_size"), MaxSize, OutError)) {
    return false;
  }

  FImage Image;
  if (!FTextureSourceImage::Read(Texture, MipIndex, SliceIndex, Image,
                                 OutError)) {
    return false;
  }
  TArray<FColor> Colors;
  if (!FTextureSourceImage::ToColors(Image, Colors, OutError)) { if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Texture->GetPathName(); return false; }
  FIntPoint OutputSize(Image.SizeX, Image.SizeY);
  if (MaxSize > 0 && FMath::Max(OutputSize.X, OutputSize.Y) > MaxSize) {
    const FIntPoint SourceSize = OutputSize;
    TArray<FColor> Resized;
    if (!FCaptureImageProcessing::ResizePixelsMaxSize(Colors, SourceSize, MaxSize,
                                                     Resized, OutputSize, OutError)) { if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Texture->GetPathName(); return false; }
    Colors = MoveTemp(Resized);
  }
  if (!FCaptureImageProcessing::WriteCompressedImage(
          Colors, OutputSize, Format, 90, OutputFile,
          OutError)) {
    return false;
  }

  OutData = MakeShared<FJsonObject>();
  OutData->SetStringField(
      TEXT("asset_path"),
      Texture->GetOutermost()
          ? NormalizeAssetPath(Texture->GetOutermost()->GetName())
          : FString());
  OutData->SetStringField(TEXT("object_path"), Texture->GetPathName());
  OutData->SetStringField(TEXT("class"), Texture->GetClass()->GetName());
  OutData->SetStringField(TEXT("class_path"),
                          Texture->GetClass()->GetPathName());
  OutData->SetStringField(TEXT("file_path"), OutputFile);
  OutData->SetNumberField(TEXT("width"), OutputSize.X);
  OutData->SetNumberField(TEXT("height"), OutputSize.Y);
  OutData->SetStringField(TEXT("format"), Format);
  OutData->SetNumberField(TEXT("mip_index"), MipIndex);
  OutData->SetNumberField(TEXT("slice"), SliceIndex);
  OutData->SetNumberField(TEXT("face"), SliceIndex);
  OutData->SetNumberField(
      TEXT("bytes"),
      static_cast<double>(IFileManager::Get().FileSize(*OutputFile)));
  OutData->SetNumberField(TEXT("max_size"), MaxSize);

  FImage StatsImage;
  UE::UAI::Contract::FDiagnostic PixelError;
  TSharedPtr<FJsonObject> Stats;
  if (FTextureSourceImage::Read(Texture, MipIndex, 0, StatsImage, PixelError))
    Stats = FTextureSourceImage::BuildPixelStats(StatsImage, PixelError);
  if (Stats.IsValid()) {
    OutData->SetObjectField(TEXT("channel_stats"), Stats);
    OutData->SetBoolField(TEXT("appears_non_black"),
                          Stats->GetNumberField(TEXT("non_black_pixel_count")) >
                              0);
  } else {
    OutData->SetBoolField(TEXT("appears_non_black"), false);
    OutData->SetObjectField(TEXT("channel_stats_error"), PixelError.ToPublicJson());
  }

  bool bRequireNonBlack = false;
  if (Parameters->HasField(TEXT("require_non_black")) &&
      !Parameters->TryGetBoolField(TEXT("require_non_black"),
                                   bRequireNonBlack)) {
    OutError = CaptureDiagnostics::Input(TEXT("invalid_require_non_black"), TEXT("require_non_black must be a boolean."), TEXT("input_decode"), TEXT("/require_non_black"), Parameters->TryGetField(TEXT("require_non_black")), MakeShared<FJsonValueString>(TEXT("boolean")));
    return false;
  }
  OutData->SetBoolField(TEXT("require_non_black"), bRequireNonBlack);
  if (bRequireNonBlack && !OutData->GetBoolField(TEXT("appears_non_black"))) {
    OutError = CaptureDiagnostics::Native(TEXT("texture_screenshot_appears_black"), TEXT("The captured texture did not meet the non-black requirement."), TEXT("capture_verify"), TEXT("appears_non_black"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = Texture->GetPathName();
    if (!Stats.IsValid() && !PixelError.IsEmpty())
    {
      OutError.PublicMessage = TEXT("The texture capture could not be confirmed non-black because pixel statistics failed.");
      OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(PixelError);
    }
    return false;
  }
  return true;
}
} // namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
