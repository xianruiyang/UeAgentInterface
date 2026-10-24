// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentAuthoringOperationApply {
class IApplyProvider;
class IDurablePackageStager;
}

namespace UeAgentAnimationAuthoring {
struct UEAGENTINTERFACE_API FAssetApplyRequest {
  EAssetKind Kind = EAssetKind::AnimBlueprint;
  UeAgentApplyInputContract::EOperation Operation =
      UeAgentApplyInputContract::EOperation::Update;
  FString AssetPath;
  FString SourceAssetPath;
  TArray<TSharedPtr<FJsonValue>> Properties;
  TSharedPtr<FJsonObject> Document;
  TSharedPtr<FJsonObject> BlueprintSurfaces;
  TSharedPtr<UeAgentAuthoringOperationApply::IDurablePackageStager> PackageStager;
  bool bCompile = true;
  bool bSave = false;
  TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure &)>
      FinalReadback;
};

struct UEAGENTINTERFACE_API FAssetApplyResult {
  FString Status;
  FString Phase;
  UE::UAI::Contract::FDiagnostic Diagnostic;
  FString ResolvedOperation;
  FString AssetPath;
  FString ObjectPath;
  UObject *Asset = nullptr;
  FMutationStats Stats;
  int32 UnitsChecked = 0;
  int32 MutationsApplied = 0;
  bool bRollbackAttempted = false;
  bool bRollbackSucceeded = false;

  bool IsApplied() const { return Status == TEXT("applied"); }
  TSharedRef<FJsonObject> ToJsonObject() const;
};

struct UEAGENTINTERFACE_API FWorkspaceInspectResult {
  FString Status;
  UE::UAI::Contract::FDiagnostic Diagnostic;
  FString AssetPath;
  FString RequestedOperation;
  FMutationStats PlannedStats;
  int32 UnitsChecked = 0;
  bool bAssetExists = false;
  bool bWouldCreate = false;
  bool bNoOp = false;

  bool IsReady() const { return Status == TEXT("ready"); }
  TSharedRef<FJsonObject> ToJsonObject() const;
};

UEAGENTINTERFACE_API bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError);
UEAGENTINTERFACE_API TSharedRef<UeAgentAuthoringOperationApply::IApplyProvider>
MakeApplyProvider();
UEAGENTINTERFACE_API bool ApplyAsset(const FAssetApplyRequest &Request,
                                    FAssetApplyResult &OutResult,
                                    const FString &RequestId = FString());
UEAGENTINTERFACE_API bool ApplyAssetWorkspace(const FProfile &Profile,
                                              const FString &WorkspaceRoot,
                                              const FString &ExpectedAssetPath,
    FAssetApplyResult &OutResult,
    const FString &RequestId = FString());
UEAGENTINTERFACE_API bool
InspectAssetWorkspace(const FProfile &Profile, const FString &WorkspaceRoot,
                      const FString &ExpectedAssetPath,
                      FWorkspaceInspectResult &OutResult);
} // namespace UeAgentAnimationAuthoring
