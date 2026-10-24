#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"

#include "EditorViewportClient.h"

namespace UE::UAI::EditorViewport
{
	FRealtimeState ReadRealtimeState(const FEditorViewportClient& Client)
	{
		FRealtimeState State;
		Client.SaveRealtimeStateToConfig(State.bConfigured);
		// IsRealtime also includes RequestRealTimeFrames, which is not a change
		// to the realtime switch. Compare the base value and last override instead.
		State.bEffective = Client.DoRealtimeAndOverridesMatch(State.bConfigured)
			? State.bConfigured : !State.bConfigured;
		if (Client.IsRealtimeOverrideSet())
		{
			State.OverrideReason = Client.GetRealtimeOverrideMessage().ToString();
		}
		return State;
	}

	void SetRealtimePreference(FEditorViewportClient& Client, bool bRealtime)
	{
		// Retire the old UAI override without touching other systems' overrides.
		// Editor background throttling reorders its override every tick, so a
		// one-tick UAI override cannot establish a verified sustained state.
		Client.RemoveRealtimeOverride(FText::FromString(TEXT("UeAgentInterface viewport state")), false);
		Client.SetRealtime(bRealtime);
	}

	struct FScopedViewportActivity::FImpl
	{
		FEditorViewportClient& Client;
		FViewportStateGetter PreviousVisibility;
		FDelegateHandle VisibilityHandle;

		explicit FImpl(FEditorViewportClient& InClient)
			: Client(InClient), PreviousVisibility(InClient.VisibilityDelegate)
		{
			Client.VisibilityDelegate = FViewportStateGetter::CreateLambda([] { return true; });
			VisibilityHandle = Client.VisibilityDelegate.GetHandle();
		}

		~FImpl()
		{
			if (Client.VisibilityDelegate.GetHandle() == VisibilityHandle)
				Client.VisibilityDelegate = MoveTemp(PreviousVisibility);
		}
	};

	void FScopedViewportActivity::Begin(FEditorViewportClient& Client)
	{
		Restore();
		// Capture scopes can overlap on the same native viewport. Only the last
		// owner restores the delegate that existed before any sampling started.
		static TMap<FEditorViewportClient*, TWeakPtr<FImpl>> ActiveViewports;
		for (auto It = ActiveViewports.CreateIterator(); It; ++It)
			if (!It.Value().IsValid()) It.RemoveCurrent();
		Impl = ActiveViewports.FindRef(&Client).Pin();
		if (!Impl)
		{
			Impl = MakeShared<FImpl>(Client);
			ActiveViewports.Add(&Client, Impl);
		}
		KeepActive();
	}

	void FScopedViewportActivity::KeepActive()
	{
		if (!Impl) return;
		// UE adds a background false override every Editor tick. Its native
		// short-frame request deliberately outranks that stack and then expires.
		Impl->Client.RequestRealTimeFrames(2);
		Impl->Client.Invalidate();
	}

	void FScopedViewportActivity::Restore() { Impl.Reset(); }
}
