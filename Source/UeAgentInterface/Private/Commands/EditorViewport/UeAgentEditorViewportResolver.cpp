// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportResolver.h"

namespace UeAgentEditorViewport
{
	FResolvedViewport::FResolvedViewport()
		: ViewportClient(nullptr)
		, Asset(nullptr)
	{
	}

	bool IViewportAdapter::WriteCamera(const FResolvedViewport&, const FViewportCamera&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_camera_not_supported");
		return false;
	}

	bool IViewportAdapter::Focus(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_focus_not_supported");
		return false;
	}

	bool IViewportAdapter::ApplyVisualOptions(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_visual_options_not_supported");
		return false;
	}

	bool IViewportAdapter::ApplyPreviewState(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_preview_state_not_supported");
		return false;
	}



	bool IViewportAdapter::SnapshotState(const FResolvedViewport&, FViewportSnapshot&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_snapshot_not_supported");
		return false;
	}

	bool IViewportAdapter::RestoreState(const FResolvedViewport&, const FViewportSnapshot&, FString& OutError) const
	{
		OutError = TEXT("editor_viewport_restore_not_supported");
		return false;
	}

	void FViewportResolver::RegisterAdapter(TSharedRef<IViewportAdapter> Adapter)
	{
		Adapters.Add(Adapter);
	}

	bool FViewportResolver::Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const
	{
		TArray<FString> CandidateNames;
		for (const TSharedRef<IViewportAdapter>& Adapter : Adapters)
		{
			if (!Adapter->CanResolve(Request))
			{
				continue;
			}

			CandidateNames.Add(Adapter->GetName());
			if (Adapter->Resolve(Request, OutViewport, OutError))
			{
				OutViewport.AdapterName = Adapter->GetName();
				return true;
			}
		}

		if (CandidateNames.Num() > 1)
		{
			OutError = TEXT("ambiguous_viewport_target");
			return false;
		}
		OutError = CandidateNames.Num() == 0 ? TEXT("unsupported_editor_viewport_target") : OutError;
		return false;
	}

	TArray<TSharedRef<IViewportAdapter>> FViewportResolver::GetAdapters() const
	{
		return Adapters;
	}

	FScopedViewportStateRestore::FScopedViewportStateRestore(const IViewportAdapter& InAdapter, const FResolvedViewport& InViewport, bool bInRestoreOnDestruct)
		: Adapter(InAdapter)
		, Viewport(InViewport)
		, bRestoreOnDestruct(bInRestoreOnDestruct)
		, bRestored(false)
		, bDismissed(false)
	{
		FString Error;
		if (!Adapter.SnapshotState(Viewport, Snapshot, Error))
		{
			SnapshotError = Error;
			Snapshot.bValid = false;
		}
	}

	FScopedViewportStateRestore::~FScopedViewportStateRestore()
	{
		if (bRestoreOnDestruct && !bRestored && !bDismissed)
		{
			RestoreNow();
		}
	}

	bool FScopedViewportStateRestore::IsSnapshotValid() const
	{
		return Snapshot.bValid;
	}

	bool FScopedViewportStateRestore::RestoreNow()
	{
		if (bDismissed)
		{
			RestoreError.Empty();
			return true;
		}
		if (!Snapshot.bValid)
		{
			RestoreError = SnapshotError.IsEmpty() ? TEXT("snapshot_invalid") : SnapshotError;
			return false;
		}
		if (bRestored)
		{
			return true;
		}
		FString Error;
		if (!Adapter.RestoreState(Viewport, Snapshot, Error))
		{
			RestoreError = Error;
			return false;
		}
		bRestored = true;
		bRestoreOnDestruct = false;
		RestoreError.Empty();
		return true;
	}

	void FScopedViewportStateRestore::Dismiss()
	{
		bDismissed = true;
		bRestoreOnDestruct = false;
	}

	TSharedPtr<FJsonObject> FScopedViewportStateRestore::MakeRestoreResultJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("requested"), bRestoreOnDestruct || bRestored);
		Obj->SetBoolField(TEXT("snapshot_valid"), Snapshot.bValid);
		Obj->SetBoolField(TEXT("restored"), bRestored);
		Obj->SetBoolField(TEXT("dismissed"), bDismissed);
		Obj->SetBoolField(TEXT("success"), (bRestored || bDismissed || !bRestoreOnDestruct) && SnapshotError.IsEmpty() && RestoreError.IsEmpty());
		if (!SnapshotError.IsEmpty())
		{
			Obj->SetStringField(TEXT("snapshot_error"), SnapshotError);
		}
		if (!RestoreError.IsEmpty())
		{
			Obj->SetStringField(TEXT("restore_error"), RestoreError);
		}
		return Obj;
	}
}
