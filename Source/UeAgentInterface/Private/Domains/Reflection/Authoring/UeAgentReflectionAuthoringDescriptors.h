// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FProperty;
class UClass;

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
  bool bProfileStateAsProperty = false;
  // An input template is not persisted asset state. Verification captures the
  // generated semantic independently, never the submitted source references.
  FString GenerationInputSemanticId;
  FString GenerationInputField;
  FString GeneratedStateSemanticId;
  FString InputContractId;
  FString WorkspaceContractId;
  FString ExportContractId;
  TSet<FName> ExcludedRootProperties;
  // Native members explicitly declared by this profile, without changing flags.
  TSet<FString> AuthorizedPropertyPaths;
  // Optional domain qualification and field scope. Empty callbacks preserve ordinary reflection.
  TFunction<bool(const UClass*)> AcceptsConcreteClass;
  TFunction<bool(const FProperty*)> IncludeProperty;
  TFunction<bool(const FProperty*)> AdditionalPropertyAuthorization;
  bool AuthorizesProperty(const FProperty* Property) const;
  bool bAllowCreate = true;
  bool bFallbackDomain = false;
};

UEAGENTINTERFACE_API const TArray<FProfile> &GetProfiles();
UEAGENTINTERFACE_API const FProfile *FindProfileByTypeId(const FString &TypeId);
UEAGENTINTERFACE_API const FProfile *
FindProfileByTargetKind(const FString &TargetKind);
UEAGENTINTERFACE_API bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentReflectionAuthoring
