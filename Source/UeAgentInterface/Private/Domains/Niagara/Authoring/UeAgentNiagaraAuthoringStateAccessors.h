// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
	UEAGENTINTERFACE_API bool RegisterStateAccessors(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProviders(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UObject& Asset,
		bool bIncludeStack,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& CanonicalAssetPathOverride = FString());

	// Editable model capture is distinct from domain before/full verification.
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		UClass* ObjectClass,
		UObject* Outer,
		const FString& AssetKind,
		bool bCreateDefaultSystemNodes,
		bool bAddDefaultEmitterModulesAndRenderers,
		UE::UAI::Contract::FDiagnostic& OutError,
		const FString& ScriptUsage = FString());
}
