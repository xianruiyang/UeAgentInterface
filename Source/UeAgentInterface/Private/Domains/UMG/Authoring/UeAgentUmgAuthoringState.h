// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UClass;
class UObject;
class UWidgetBlueprint;

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API TSet<FName> WidgetStructuralProperties();
	UEAGENTINTERFACE_API UWidgetBlueprint* CreateFactorySeed(
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UWidgetBlueprint* Asset,
		const FString& CanonicalAssetPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UWidgetBlueprint& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
}
