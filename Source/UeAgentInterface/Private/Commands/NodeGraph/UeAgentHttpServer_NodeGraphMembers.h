// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// NOTE: This header is included inside `FUeAgentLegacyMechanics`'s class declaration.
// It must only contain member declarations.
#if !defined(UEAGENT_HTTP_SERVER_MEMBER_DECL)
#error "UeAgentHttpServer_NodeGraphMembers.h must be included from UeAgentHttpServer.h inside FUeAgentLegacyMechanics."
#endif

	bool CmdNodeGraphList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
	bool CmdNodeGraphLayout(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const;
