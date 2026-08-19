// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UClass;
class UObject;
class UWidgetBlueprint;

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API UWidgetBlueprint* CreateFactorySeed(
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UWidgetBlueprint* Asset,
		const FString& CanonicalAssetPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UWidgetBlueprint& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(FString& OutError);
}
