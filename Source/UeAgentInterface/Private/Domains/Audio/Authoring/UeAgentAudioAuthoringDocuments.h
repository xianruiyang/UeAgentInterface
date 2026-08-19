// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"

class FJsonObject;
class UObject;

namespace UeAgentAudioAuthoring {
struct UEAGENTINTERFACE_API FMutationStats {
  int32 Settings = 0;
  int32 Members = 0;
  int32 Nodes = 0;
  int32 Edges = 0;
  int32 References = 0;

  int32 Total() const {
    return Settings + Members + Nodes + Edges + References;
  }
};

UEAGENTINTERFACE_API FString NormalizePackagePath(const FString &InPath);
UEAGENTINTERFACE_API FString ToObjectPath(const FString &PackagePath);
UEAGENTINTERFACE_API UObject *LoadAsset(EAssetKind Kind, const FString &InPath);
UEAGENTINTERFACE_API UObject *CreateFactorySeed(
    EAssetKind Kind, UObject *Outer, FName Name, EObjectFlags Flags,
    const TSharedPtr<const FJsonObject> &SeedDocument, FString &OutError);
UEAGENTINTERFACE_API bool
BuildDocument(EAssetKind Kind, UObject &Asset,
              TSharedPtr<FJsonObject> &OutDocument, FString &OutError,
              const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool
ApplyDocument(EAssetKind Kind, UObject &Asset,
              const TSharedPtr<const FJsonObject> &Document,
              FMutationStats &OutStats, FString &OutError,
              const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool CompileAsset(EAssetKind Kind, UObject &Asset,
                                       FString &OutError);
UEAGENTINTERFACE_API bool SaveAsset(EAssetKind Kind, UObject &Asset,
                                    FString &OutError);
} // namespace UeAgentAudioAuthoring
