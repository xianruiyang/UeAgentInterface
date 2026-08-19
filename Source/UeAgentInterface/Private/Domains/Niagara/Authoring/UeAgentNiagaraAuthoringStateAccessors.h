// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UObject;
class UClass;

namespace UeAgentAuthoringNativeDefaults
{
	class FScratchScope;
}

namespace UeAgentNiagaraAuthoring
{
	UEAGENTINTERFACE_API bool RegisterStateAccessors(FString& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProviders(FString& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UObject& Asset,
		bool bIncludeStack,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError,
		const FString& CanonicalAssetPathOverride = FString());

	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		UClass* ObjectClass,
		UObject* Outer,
		const FString& AssetKind,
		bool bCreateDefaultSystemNodes,
		bool bAddDefaultEmitterModulesAndRenderers,
		FString& OutError,
		const FString& ScriptUsage = FString());
}
