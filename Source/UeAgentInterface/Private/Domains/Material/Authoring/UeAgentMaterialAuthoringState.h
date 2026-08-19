// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

namespace UeAgentMaterialAuthoring
{
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(FString& OutError);
}
