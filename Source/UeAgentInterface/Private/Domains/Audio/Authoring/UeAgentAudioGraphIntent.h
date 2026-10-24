// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UObject;
class USoundCue;
class USoundCueGraphNode_Root;

namespace UeAgentAudioAuthoring {
USoundCueGraphNode_Root* FindSoundCueOutputNode(USoundCue& Cue);
// Expectations come from the request (or an unchanged native baseline), never
// a post-write capture. Only native identities cross compile/package replacement.
struct FMetaSoundNodeIntent {
  FGuid Id;
  bool bPresent = true;
  FString ClassName;
  int32 Major = INDEX_NONE;
  int32 Minor = INDEX_NONE;
  TOptional<FVector2D> Position;
  FString SourcePointer;
  FString PositionSourcePointer;
  FString PublicName;
};
struct FMetaSoundLinkIntent {
  FGuid FromNode, FromPin, ToNode, ToPin;
  bool bPresent = true;
  FString SourcePointer;
};
struct FMetaSoundDefaultIntent {
  FGuid Node, Pin;
  TSharedPtr<const FJsonObject> Value;
  TSharedPtr<const FJsonObject> Source;
  bool bRequireInstanceOverride = false;
};
struct FSoundCueNodeIntent {
  FName Name;
  bool bPresent = true;
  FString ClassPath;
  TOptional<FIntPoint> Position;
  FString SourcePointer;
  bool bExactClass = true;
};
struct FSoundCueLinkIntent {
  FName Parent, Child;
  int32 Order = INDEX_NONE;
  bool bPresent = true;
  FString SourcePointer;
};
struct UEAGENTINTERFACE_API FAudioGraphIntent {
  TArray<FMetaSoundNodeIntent> MetaNodes;
  TArray<FMetaSoundLinkIntent> MetaLinks;
  TArray<FMetaSoundDefaultIntent> MetaDefaults;
  TOptional<TSet<FGuid>> CompleteMetaExternalNodes;
  TOptional<TSet<FGuid>> CompleteMetaLinkNodes;
  TArray<FSoundCueNodeIntent> SoundNodes;
  TArray<FSoundCueLinkIntent> SoundLinks;
  TOptional<TSet<FName>> CompleteSoundNodes;
  TMap<FName, TArray<FName>> CompleteSoundChildren;
  TOptional<FName> SoundRoot;
  TOptional<FIntPoint> SoundOutputPosition;
  FString SoundOutputSourcePointer;

  bool Verify(UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError) const;
};
} // namespace UeAgentAudioAuthoring
