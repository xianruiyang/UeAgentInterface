#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::AssetPreview
{
	// Asset identity also covers reload of all of its native preview targets.
	int64 EnqueuePreviewWork(const FString& AssetPath);
	bool IsPreviewWorkReady(const FString& AssetPath, int64 Ticket);
	void FinishPreviewWork(const FString& AssetPath, int64 Ticket);
	void DeferPreviewWork(const FString& AssetPath, TFunction<void(bool bCancelled)> Completion);
	void CancelDeferredPreviewWork();
}
