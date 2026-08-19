// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"

namespace UeAgentExportWorkspaceWriter
{
	struct FCompoundPlanPolicy;
}

namespace UeAgentLevelAuthoring
{
	enum class EExportTargetKind : uint8
	{
		World,
		Actor,
		Component,
	};

	UEAGENTINTERFACE_API bool RegisterExportAuthoring(FString& OutError);
	UEAGENTINTERFACE_API bool GetExportContract(
		EExportTargetKind Kind,
		UeAgentExportContract::FContract& OutContract,
		FString& OutError);
	UEAGENTINTERFACE_API bool GetWorkspaceExportPolicy(
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FCompoundPlanPolicy& OutPolicy,
		FString& OutError);
}
