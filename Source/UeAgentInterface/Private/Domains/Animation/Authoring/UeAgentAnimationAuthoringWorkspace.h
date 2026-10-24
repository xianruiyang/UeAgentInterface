// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

class UObject;
namespace UeAgentAnimationAuthoring {
struct FProfile;

// Animation profiles share capture, semantic projection and one apply transaction.
bool ImportAssetWorkspace(
    const FProfile& Profile, UObject& Asset,
    const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspaceRef& Workspace,
    TFunctionRef<bool(FString&, FString&, UE::UAI::HjsonWorkspace::FWorkspaceError&)> Readback,
    FString& OutPayload, UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
}
