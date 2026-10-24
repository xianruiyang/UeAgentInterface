#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UTexture;
struct FImage;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob {
class FTextureSourceImage final {
public:
  static bool Read(UTexture *Texture, int32 MipIndex, int32 SliceIndex,
                   FImage &OutImage, UE::UAI::Contract::FDiagnostic& OutError);

  static bool ToColors(const FImage &Image, TArray<FColor> &OutColors, UE::UAI::Contract::FDiagnostic& OutError);

  static TSharedPtr<FJsonObject> BuildPixelStats(const TArray<FColor> &Pixels,
                                                 const FIntPoint &Size);

  static TSharedPtr<FJsonObject> BuildPixelStats(const FImage &Image, UE::UAI::Contract::FDiagnostic& OutError);
};
} // namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
