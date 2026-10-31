// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class UWorld;
class ULevel;
class ULevelStreaming;
class UDataLayerInstance;

namespace UeAgentLevelAuthoring {

// The Level authoring owner captures and applies native map relationships.
TSharedRef<FJsonObject> CaptureWorldTopology(const UWorld& World);
ULevel* FindAuthoringLevel(UWorld& World, const FString& PackageName);

class FWorldTopologyEdit {
public:
  bool Prepare(UWorld& World, const TSharedPtr<const FJsonObject>& Extension,
      UE::UAI::Contract::FDiagnostic& Error);
  bool Apply(UWorld& World, UE::UAI::Contract::FDiagnostic& Error);
  bool Restore(UWorld& World, UE::UAI::Contract::FDiagnostic& Error);
  bool IsPresent() const { return Desired.IsValid(); }
  const TSharedPtr<FJsonObject>& GetDesired() const { return Desired; }
  void Release();

private:
  bool ApplyState(UWorld& World, const TSharedPtr<FJsonObject>& State,
      UE::UAI::Contract::FDiagnostic& Error);
  TSharedPtr<FJsonObject> Before, Desired;
  TMap<FString, TWeakObjectPtr<ULevelStreaming>> Removed;
  TArray<TWeakObjectPtr<UDataLayerInstance>> CreatedDataLayers;
};
}
