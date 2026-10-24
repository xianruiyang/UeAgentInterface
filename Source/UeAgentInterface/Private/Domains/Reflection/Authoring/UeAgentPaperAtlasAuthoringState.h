// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

class FJsonValue;
struct FPropertyChangedEvent;
namespace UeAgentReflectionAuthoring::PaperSpriteState {
// Supplies the native atlas dependency closure to the common transaction.
// Property decoding, checkpoints, package persistence and comparison stay shared.
class FAtlasState {
public:
  FAtlasState();
  ~FAtlasState();
  bool Prepare(UObject* Existing, UObject& Proposed, const FString& TargetPath,
      const TArray<TSharedPtr<FJsonValue>>& Properties, UE::UAI::Contract::FDiagnostic& Error);
  bool NotifyPreview(UObject& Object, FPropertyChangedEvent& Event);
  void BindActual(UObject& Object);
  void CollectRollbackObjects(TArray<UObject*>& Objects) const;
  bool AppendPackagePlans(TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
      UeAgentAuthoringOperationApply::FOperationFailure& Failure) const;
  TSharedPtr<FJsonValue> Capture(bool bExpected) const;
  void Release();
  bool Rebind(UE::UAI::Contract::FDiagnostic& Error, bool bTargetAbsent = false);
private:
  struct FImpl;
  TUniquePtr<FImpl> Impl;
};
}
