// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

class FJsonObject;
class UObject;
class UMetaSoundBuilderBase;
struct FMetasoundFrontendLiteral;

namespace UeAgentAudioAuthoring {
struct FAudioGraphIntent;
UEAGENTINTERFACE_API TSharedPtr<FJsonObject> EncodeMetaSoundLiteral(const FMetasoundFrontendLiteral& Literal);
UEAGENTINTERFACE_API bool DecodeMetaSoundLiteral(const TSharedPtr<const FJsonObject>& Value,
    FMetasoundFrontendLiteral& OutLiteral, UE::UAI::Contract::FDiagnostic& OutError);
const FMetasoundFrontendLiteral* FindMetaSoundEffectiveInputDefault(
    UMetaSoundBuilderBase& Builder, const FGuid& Node, const FGuid& Pin);
inline TSet<FName> SoundNodeStructuralProperties() { return {TEXT("ChildNodes")}; }
// Frozen original property intent; native owners are resolved again after compile
// and package replacement. No node or property storage pointer crosses that boundary.
struct UEAGENTINTERFACE_API FNodePropertyIntent {
  FName NodeName;
  UeAgentAuthoringObjectApply::FPropertyIntent Properties;
  TSharedPtr<const FJsonObject> Source;
};
UEAGENTINTERFACE_API bool VerifyNodePropertyIntents(
    UObject& Asset, const TArray<FNodePropertyIntent>& Intents, UE::UAI::Contract::FDiagnostic& OutError);
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
    const TSharedPtr<const FJsonObject> &SeedDocument, UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool
BuildDocument(EAssetKind Kind, UObject &Asset,
              TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
              const FString &CanonicalAssetPath = FString());
UEAGENTINTERFACE_API bool
ApplyDocument(EAssetKind Kind, UObject &Asset,
              const TSharedPtr<const FJsonObject> &Document,
              FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
              const FString &CanonicalAssetPath = FString(),
              TArray<FNodePropertyIntent>* OutPropertyIntents = nullptr,
              bool bCompleteDocument = false,
              FAudioGraphIntent* OutGraphIntent = nullptr);
// Reads original member values against the current native owner, including
// after package replacement. Does not use the post-apply capture as expectation.
UEAGENTINTERFACE_API bool VerifyDocumentIntent(
    EAssetKind Kind, UObject& Asset, const TSharedPtr<const FJsonObject>& Document,
    bool bCompleteDocument, UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool CompileAsset(EAssetKind Kind, UObject &Asset,
                                       UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API bool SaveAsset(EAssetKind Kind, UObject &Asset,
                                    UE::UAI::Contract::FDiagnostic& OutError);
} // namespace UeAgentAudioAuthoring
