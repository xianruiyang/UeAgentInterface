// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UeAgentAudioAuthoring {
enum class EAssetKind : uint8 {
  MetaSound,
  SoundCue,
  SoundSubmix,
};

struct UEAGENTINTERFACE_API FProfile {
  EAssetKind Kind = EAssetKind::MetaSound;
  FString TypeId;
  FString TargetKind;
  FString ProfileId;
  FString IdentityModelId;
  FString DocumentSemanticId;
  FString ExportTargetSemanticId;
  FString InputContractId;
  FString WorkspaceContractId;
  FString ExportContractId;
};

inline constexpr const TCHAR *DomainId = TEXT("audio");
inline constexpr const TCHAR *ApplyProviderId = TEXT("audio.asset.apply");
inline constexpr const TCHAR *StateAccessorId = TEXT("audio.asset.state");
inline constexpr const TCHAR *FactoryDefaultProviderId =
    TEXT("audio.asset.factory");

UEAGENTINTERFACE_API const TArray<FProfile> &GetProfiles();
UEAGENTINTERFACE_API const FProfile *FindProfile(EAssetKind Kind);
UEAGENTINTERFACE_API const FProfile *FindProfileByTypeId(const FString &TypeId);
UEAGENTINTERFACE_API const FProfile *
FindProfileByTargetKind(const FString &TargetKind);
UEAGENTINTERFACE_API const TCHAR *AssetKindToString(EAssetKind Kind);
UEAGENTINTERFACE_API bool ParseAssetKind(const FString &Text,
                                         EAssetKind &OutKind);
UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAudioAuthoring
