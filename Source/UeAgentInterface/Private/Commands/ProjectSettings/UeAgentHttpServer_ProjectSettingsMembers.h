// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// NOTE: This header is included inside `FUeAgentLegacyMechanics`'s class declaration.
// It must only contain member declarations.
#if !defined(UEAGENT_HTTP_SERVER_MEMBER_DECL)
#error "UeAgentHttpServer_ProjectSettingsMembers.h must be included from UeAgentHttpServer.h inside FUeAgentLegacyMechanics."
#endif

	bool CmdProjectSettingsExportIndex(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdProjectSettingsExportPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdProjectSettingsValidatePage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdProjectSettingsApplyPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdProjectSettingsDiffPage(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdConfigExportSection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdConfigApplySection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
