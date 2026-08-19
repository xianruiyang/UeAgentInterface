// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UObject;

namespace UeAgentRiggingAuthoring
{
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		const FProfile& Profile,
		UObject* Asset,
		const FString& CanonicalAssetPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		const FProfile& Profile,
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(FString& OutError);
}
