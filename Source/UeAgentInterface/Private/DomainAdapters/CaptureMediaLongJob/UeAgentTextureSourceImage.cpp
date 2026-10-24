#include "DomainAdapters/CaptureMediaLongJob/UeAgentTextureSourceImage.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "ImageCore.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob {
namespace {
TSharedPtr<FJsonObject> MakeColorJson(const FLinearColor &Color) {
  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("r"), Color.R);
  Result->SetNumberField(TEXT("g"), Color.G);
  Result->SetNumberField(TEXT("b"), Color.B);
  Result->SetNumberField(TEXT("a"), Color.A);
  return Result;
}
} // namespace

bool FTextureSourceImage::Read(UTexture *Texture, int32 MipIndex,
                               int32 SliceIndex, FImage &OutImage,
                               UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Texture) {
    OutError = CaptureDiagnostics::Native(TEXT("texture_not_found"), TEXT("No texture object is available for source-image readback."), TEXT("texture_readback"), TEXT("texture_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  if (!Texture->Source.IsValid()) {
    OutError = CaptureDiagnostics::Native(TEXT("texture_source_unavailable"), TEXT("The texture has no readable source data."), TEXT("texture_readback"), TEXT("source_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = Texture->GetPathName();
    return false;
  }
  MipIndex = FMath::Clamp(MipIndex, 0,
                          FMath::Max(0, Texture->Source.GetNumMips() - 1));
  FImage SourceImage;
  if (!Texture->Source.GetMipImage(SourceImage, 0, 0, MipIndex)) {
    OutError = CaptureDiagnostics::Native(TEXT("texture_source_read_failed"), TEXT("The texture source mip could not be read."), TEXT("texture_readback"), TEXT("mip_index"), CaptureDiagnostics::Number(MipIndex), MakeShared<FJsonValueString>(TEXT("readable texture source mip")));
    OutError.TargetPath = Texture->GetPathName();
    return false;
  }
  if (SourceImage.NumSlices <= 0) {
    OutError = CaptureDiagnostics::Native(TEXT("texture_source_slice_unavailable"), TEXT("The texture source image contains no slices."), TEXT("texture_readback"), TEXT("slice_count"), CaptureDiagnostics::Number(SourceImage.NumSlices), MakeShared<FJsonValueString>(TEXT("at least one slice")));
    OutError.TargetPath = Texture->GetPathName();
    return false;
  }
  SliceIndex = FMath::Clamp(SliceIndex, 0, SourceImage.NumSlices - 1);
  if (SourceImage.NumSlices > 1) {
    SourceImage.GetSlice(SliceIndex)
        .CopyTo(OutImage, ERawImageFormat::BGRA8,
                Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
  } else {
    SourceImage.CopyTo(OutImage, ERawImageFormat::BGRA8,
                       Texture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
  }
  return true;
}

bool FTextureSourceImage::ToColors(const FImage &Image,
                                   TArray<FColor> &OutColors, UE::UAI::Contract::FDiagnostic& OutError) {
  if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.NumSlices != 1 ||
      static_cast<int64>(Image.SizeX) * Image.SizeY > MAX_int32) {
    if (Image.SizeX <= 0 || Image.SizeY <= 0)
      OutError = CaptureDiagnostics::Native(TEXT("image_size_exceeds_pixel_storage"), TEXT("The source image dimension must be positive."),
          TEXT("image_decode"), Image.SizeX <= 0 ? TEXT("width") : TEXT("height"),
          CaptureDiagnostics::Number(Image.SizeX <= 0 ? Image.SizeX : Image.SizeY), MakeShared<FJsonValueString>(TEXT("positive pixel dimension")));
    else if (Image.NumSlices != 1)
      OutError = CaptureDiagnostics::Native(TEXT("image_size_exceeds_pixel_storage"), TEXT("Pixel conversion requires exactly one image slice."), TEXT("image_decode"), TEXT("slice_count"), CaptureDiagnostics::Number(Image.NumSlices), CaptureDiagnostics::Number(1));
    else
      OutError = CaptureDiagnostics::Native(TEXT("image_size_exceeds_pixel_storage"), TEXT("The image pixel count exceeds the supported storage size."), TEXT("image_decode"), TEXT("pixel_count"), CaptureDiagnostics::Number(static_cast<int64>(Image.SizeX) * Image.SizeY), CaptureDiagnostics::Number(MAX_int32));
    return false;
  }
  FImage BgraImage;
  Image.CopyTo(BgraImage, ERawImageFormat::BGRA8, Image.GammaSpace);
  const FImageView BgraView = BgraImage;
  const TArrayView64<FColor> View = BgraView.AsBGRA8();
  OutColors.Reset();
  OutColors.Append(View.GetData(), static_cast<int32>(View.Num()));
  return true;
}

TSharedPtr<FJsonObject>
FTextureSourceImage::BuildPixelStats(const TArray<FColor> &Pixels,
                                     const FIntPoint &Size) {
  TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
  Stats->SetNumberField(TEXT("width"), Size.X);
  Stats->SetNumberField(TEXT("height"), Size.Y);
  Stats->SetNumberField(TEXT("sampled_pixel_count"), Pixels.Num());
  if (Pixels.IsEmpty()) {
    Stats->SetBoolField(TEXT("appears_non_black"), false);
    Stats->SetNumberField(TEXT("non_black_pixel_count"), 0);
    Stats->SetNumberField(TEXT("transparent_pixel_count"), 0);
    Stats->SetNumberField(TEXT("unique_color_estimate"), 0);
    return Stats;
  }

  FLinearColor Min(1.f, 1.f, 1.f, 1.f);
  FLinearColor Max(0.f, 0.f, 0.f, 0.f);
  FLinearColor Sum(0.f, 0.f, 0.f, 0.f);
  int32 NonBlack = 0;
  int32 Transparent = 0;
  TSet<uint32> Unique;
  for (const FColor &Pixel : Pixels) {
    const FLinearColor Color = Pixel.ReinterpretAsLinear();
    Min.R = FMath::Min(Min.R, Color.R);
    Min.G = FMath::Min(Min.G, Color.G);
    Min.B = FMath::Min(Min.B, Color.B);
    Min.A = FMath::Min(Min.A, Color.A);
    Max.R = FMath::Max(Max.R, Color.R);
    Max.G = FMath::Max(Max.G, Color.G);
    Max.B = FMath::Max(Max.B, Color.B);
    Max.A = FMath::Max(Max.A, Color.A);
    Sum += Color;
    NonBlack += (Pixel.R + Pixel.G + Pixel.B) > 12 ? 1 : 0;
    Transparent += Pixel.A < 250 ? 1 : 0;
    if (Unique.Num() < 65536) {
      Unique.Add(Pixel.ToPackedARGB());
    }
  }
  const float Count = static_cast<float>(Pixels.Num());
  Stats->SetObjectField(TEXT("min"), MakeColorJson(Min));
  Stats->SetObjectField(TEXT("max"), MakeColorJson(Max));
  Stats->SetObjectField(TEXT("mean"), MakeColorJson(Sum / Count));
  Stats->SetNumberField(TEXT("non_black_pixel_count"), NonBlack);
  Stats->SetNumberField(TEXT("transparent_pixel_count"), Transparent);
  Stats->SetNumberField(TEXT("unique_color_estimate"), Unique.Num());
  Stats->SetBoolField(TEXT("appears_non_black"), NonBlack > 0);
  return Stats;
}

TSharedPtr<FJsonObject>
FTextureSourceImage::BuildPixelStats(const FImage &Image, UE::UAI::Contract::FDiagnostic& OutError) {
  TArray<FColor> Colors;
  if (!ToColors(Image, Colors, OutError)) return nullptr;
  return BuildPixelStats(Colors, FIntPoint(Image.SizeX, Image.SizeY));
}
} // namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
