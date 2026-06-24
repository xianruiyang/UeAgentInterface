// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"

bool FUeAgentHttpServer::CmdNodeCatalogSearch(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentPaletteSearch::FSearchParams Params;
	if (!UeAgentPaletteSearch::ParseSearchParams(Ctx.Params, Params, OutError))
	{
		return false;
	}

	UeAgentPaletteSearch::FSearchResult Result;
	if (!UeAgentPaletteSearch::ExecuteSearch(Params, Result, OutError))
	{
		return false;
	}

	OutData = UeAgentPaletteSearch::BuildSearchResponse(Params, Result);
	return true;
}

bool FUeAgentHttpServer::CmdNodeCatalogCategories(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UeAgentPaletteSearch::FSearchParams Params;
	if (!UeAgentPaletteSearch::ParseSearchParams(Ctx.Params, Params, OutError))
	{
		return false;
	}

	Params.bIncludeItems = false;
	Params.bIncludeGroups = true;
	Params.bIncludeCategoryTree = true;

	UeAgentPaletteSearch::FSearchResult Result;
	if (!UeAgentPaletteSearch::ExecuteSearch(Params, Result, OutError))
	{
		return false;
	}

	OutData = UeAgentPaletteSearch::BuildCategoriesResponse(Params, Result);
	return true;
}
