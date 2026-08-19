// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UPCGGraph;

namespace UeAgentPCGAuthoring
{
	UEAGENTINTERFACE_API UPCGGraph* CreateFactorySeed(
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UPCGGraph* Graph,
		const FString& CanonicalAssetPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UPCGGraph& Graph,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(FString& OutError);
}
