#pragma once

#include "CoreMinimal.h"

class FEditorViewportClient;

namespace UE::UAI::EditorViewport
{
	struct FRealtimeState
	{
		bool bConfigured = false;
		bool bEffective = false;
		FString OverrideReason;
	};

	// Persistent preference and sustained effective state, excluding one-off redraw frames.
	FRealtimeState ReadRealtimeState(const FEditorViewportClient& Client);
	void SetRealtimePreference(FEditorViewportClient& Client, bool bRealtime);

	// Keep one native viewport eligible for normal Editor ticks while sampling,
	// even when its window is in the background. Does not change user settings.
	class FScopedViewportActivity final
	{
	public:
		void Begin(FEditorViewportClient& Client);
		void KeepActive();
		void Restore();
	private:
		struct FImpl;
		TSharedPtr<FImpl> Impl;
	};
}
