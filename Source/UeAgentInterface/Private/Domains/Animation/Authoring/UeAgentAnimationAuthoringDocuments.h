// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "UObject/ObjectMacros.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentAnimationAuthoring {
struct UEAGENTINTERFACE_API FMutationStats {
  int32 Settings = 0;
  int32 Sockets = 0;
  int32 VirtualBones = 0;
  int32 SlotGroups = 0;
  int32 RetargetModes = 0;
  int32 CompatibleSkeletons = 0;
  int32 Materials = 0;
  int32 References = 0;

  int32 Total() const {
    return Settings + Sockets + VirtualBones + SlotGroups + RetargetModes +
           CompatibleSkeletons + Materials + References;
  }
};

UEAGENTINTERFACE_API FString NormalizePackagePath(const FString &InPath);
// Reuse the document's native parser semantics before original-argument proof.
UEAGENTINTERFACE_API bool ProjectDocumentOriginalIntent(
    TSharedPtr<FJsonValue>& Desired, UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API FString ToObjectPath(const FString &PackagePath);
UEAGENTINTERFACE_API UObject *LoadAsset(EAssetKind Kind, const FString &InPath);
struct FFactorySeedDiagnosticSource {
  FString TargetPath;
  UE::UAI::Contract::FSourceLocation Skeleton;
  UE::UAI::Contract::FSourceLocation Dimension;
  UE::UAI::Contract::FSourceLocation Settings;
  FString InputSourceKind = TEXT("related_input");
};
UEAGENTINTERFACE_API UObject *CreateFactorySeed(
    EAssetKind Kind, UObject *Outer, FName Name, EObjectFlags Flags,
    const TSharedPtr<const FJsonObject> &SeedDocument, UE::UAI::Contract::FDiagnostic& OutError,
    const FFactorySeedDiagnosticSource& DiagnosticSource = FFactorySeedDiagnosticSource());
UEAGENTINTERFACE_API bool
BuildDocument(EAssetKind Kind, UObject &Asset,
              TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
              const FString &CanonicalAssetPath = FString());
// Verification uses the shared generated Blueprint program representation.
// Authoring and rollback documents retain complete native child identities.
UEAGENTINTERFACE_API bool BuildVerificationDocument(
    EAssetKind Kind, UObject &Asset, TSharedPtr<FJsonObject> &OutDocument,
    UE::UAI::Contract::FDiagnostic& OutError, const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool BuildAnimBlueprintSettingsDocument(
    UObject &Asset, TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool
ApplyDocument(EAssetKind Kind, UObject &Asset,
              const TSharedPtr<const FJsonObject> &Document,
              FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
              const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool CompileAsset(EAssetKind Kind, UObject &Asset,
                                       UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool SaveAsset(EAssetKind Kind, UObject &Asset,
                                    UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAnimationAuthoring
