// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UNiagaraSystem;
class UObject;

namespace UeAgentNiagaraOps
{
	// Release live instances before changing emitter handle storage. Kept
	// separate from asset-editor isolation, which is also used by previews.
	void ReleaseSystemInstancesForEmitterHandleEdit(UNiagaraSystem& System);
	void RemoveEmitterHandlesForStructuralEdit(UNiagaraSystem& System, const TSet<FGuid>& HandleIds);

	UEAGENTINTERFACE_API bool PrepareAssetForStructuralNiagaraEdit(
		UObject& Asset,
		int32& OutClosedEditorCount,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool SynchronizeSystemEditorData(
		UNiagaraSystem& NiagaraSystem,
		int32& OutOverviewNodeCountBefore,
		int32& OutOverviewNodeCountAfter);

	struct FNiagaraSystemRefreshResult
	{
		bool bPerformed = false;
		bool bSynchronizedOverviewGraph = false;
		bool bRebuiltEmitterNodes = false;
		bool bComputedExecutionOrder = false;
		bool bInvalidatedCachedData = false;
		bool bBroadcastPostEditChange = false;
		bool bCreatedDataProcessingViewModel = false;
		bool bHadExistingSystemViewModel = false;
		bool bMarkedDirty = false;
		int32 OverviewNodeCountBefore = INDEX_NONE;
		int32 OverviewNodeCountAfter = INDEX_NONE;
		int32 EmitterCount = 0;
	};

	FNiagaraSystemRefreshResult RefreshNiagaraSystemAfterStructuralEdit(
		UNiagaraSystem& NiagaraSystem,
		bool bBroadcastPostEditChange,
		bool bMarkDirty);
}
