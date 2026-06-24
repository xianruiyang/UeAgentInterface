// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Commands/OriginResolve/UeAgentOriginResolveTypes.h"
#include "Dom/JsonObject.h"

bool FUeAgentHttpServer::CmdNodeOriginResolve(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentOriginResolve::FResolveParams Params;
	if (!UeAgentOriginResolve::ParseResolveParams(Ctx.Params, Params, OutError))
	{
		return false;
	}

	UeAgentOriginResolve::FResolveResult Result;
	if (!UeAgentOriginResolve::ResolveOrigin(Params, Result, OutError))
	{
		OutData = UeAgentOriginResolve::BuildResolveResponse(Params, Result);
		return false;
	}

	OutData = UeAgentOriginResolve::BuildResolveResponse(Params, Result);
	return true;
}
