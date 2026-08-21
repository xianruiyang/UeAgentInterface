// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_AnimationAssets.h"

#include "Commands/Animation/UeAgentAnimationFolderCommandBridge.h"

bool FUeAgentLegacyMechanics::CmdSkeletonExportFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Export(
      UeAgentAnimationAuthoring::EAssetKind::Skeleton, Ctx.Params, OutData,
      OutError);
}

bool FUeAgentLegacyMechanics::CmdSkeletonApplyFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Apply(
      UeAgentAnimationAuthoring::EAssetKind::Skeleton, Ctx.Params, OutData,
      OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdSkeletonValidateFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Validate(
      UeAgentAnimationAuthoring::EAssetKind::Skeleton, Ctx.Params, OutData,
      OutError);
}
