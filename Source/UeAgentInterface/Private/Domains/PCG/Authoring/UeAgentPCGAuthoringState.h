// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UPCGGraph;

namespace UeAgentPCGAuthoring
{
	UEAGENTINTERFACE_API UPCGGraph* CreateFactorySeed(
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequests(
		UPCGGraph* Graph,
		const FString& CanonicalAssetPath,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UPCGGraph& Graph,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
	/** Shared native export snapshot for workspace export and independent read queries. */
	UEAGENTINTERFACE_API bool CaptureAssetExportSnapshot(
		UPCGGraph& Graph,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
}
