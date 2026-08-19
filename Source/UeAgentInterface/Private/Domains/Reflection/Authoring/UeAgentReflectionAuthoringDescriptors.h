// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UeAgentReflectionAuthoring {
inline constexpr const TCHAR *DomainId = TEXT("reflection");
inline constexpr const TCHAR *ApplyProviderId = TEXT("reflection.asset.apply");
inline constexpr const TCHAR *StateAccessorId = TEXT("reflection.asset.state");
inline constexpr const TCHAR *FactoryDefaultProviderId =
    TEXT("reflection.asset.factory");

struct FProfile {
  FString TypeId;
  FString DisplayName;
  FString TargetKind;
  FString AssetClassPath;
  FString CreateClassPath;
  FString IdentityModelId;
  FString SeedSemanticId;
  FString ExportTargetSemanticId;
  FString ProfileStateSemanticId;
  FString ProfileStateField;
  FString ProfileStateInputType;
  FString InputContractId;
  FString WorkspaceContractId;
  FString ExportContractId;
  TSet<FName> ExcludedRootProperties;
  bool bAllowCreate = true;
};

UEAGENTINTERFACE_API const TArray<FProfile> &GetProfiles();
UEAGENTINTERFACE_API const FProfile *FindProfileByTypeId(const FString &TypeId);
UEAGENTINTERFACE_API const FProfile *
FindProfileByTargetKind(const FString &TargetKind);
UEAGENTINTERFACE_API bool RegisterDescriptors(FString &OutError);
} // namespace UeAgentReflectionAuthoring
