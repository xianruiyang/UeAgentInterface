// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimBlueprint.h"

#include "Commands/Animation/UeAgentAnimationFolderCommandBridge.h"

bool FUeAgentHttpServer::CmdAnimBlueprintExportFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Export(
      UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint, Ctx.Params, OutData,
      OutError);
}

bool FUeAgentHttpServer::CmdAnimBlueprintApplyFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Apply(
      UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint, Ctx.Params, OutData,
      OutError, Ctx.RequestId);
}

bool FUeAgentHttpServer::CmdAnimBlueprintValidateFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Validate(
      UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint, Ctx.Params, OutData,
      OutError);
}
