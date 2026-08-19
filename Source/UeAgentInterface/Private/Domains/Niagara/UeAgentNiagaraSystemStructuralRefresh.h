// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;
class UObject;

namespace UeAgentNiagaraOps
{
	UEAGENTINTERFACE_API bool PrepareAssetForStructuralNiagaraEdit(
		UObject& Asset,
		int32& OutClosedEditorCount,
		FString& OutError);

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
