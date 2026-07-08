// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commands/EditorViewport/UeAgentEditorViewportTypes.h"

class FEditorViewportClient;

namespace UeAgentEditorViewport
{
	struct FResolveRequest
	{
		FViewportTarget Target;
		TSharedPtr<FJsonObject> Params;
	};

	struct FResolvedViewport
	{
		FResolvedViewport();

		FViewportTarget Target;
		FString AdapterName;
		FString ClientClass;
		FEditorViewportClient* ViewportClient;
		UObject* Asset;
	};

	class IViewportAdapter
	{
	public:
		virtual ~IViewportAdapter() = default;

		virtual FString GetName() const = 0;
		virtual bool CanResolve(const FResolveRequest& Request) const = 0;
		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const = 0;
		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const = 0;

		virtual bool WriteCamera(const FResolvedViewport&, const FViewportCamera&, FString& OutError) const;
		virtual bool Focus(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const;
		virtual bool ApplyVisualOptions(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const;
		virtual bool ApplyPreviewState(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString& OutError) const;
		virtual bool CaptureScreenshot(const FResolvedViewport&, const TSharedPtr<FJsonObject>&, FViewportScreenshotResult&, FString& OutError) const;
		virtual bool SnapshotState(const FResolvedViewport&, FViewportSnapshot&, FString& OutError) const;
		virtual bool RestoreState(const FResolvedViewport&, const FViewportSnapshot&, FString& OutError) const;
	};

	class FViewportResolver
	{
	public:
		void RegisterAdapter(TSharedRef<IViewportAdapter> Adapter);
		bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const;
		TArray<TSharedRef<IViewportAdapter>> GetAdapters() const;

	private:
		TArray<TSharedRef<IViewportAdapter>> Adapters;
	};

	class FScopedViewportStateRestore
	{
	public:
		FScopedViewportStateRestore(const IViewportAdapter& InAdapter, const FResolvedViewport& InViewport, bool bInRestoreOnDestruct);
		~FScopedViewportStateRestore();

		bool IsSnapshotValid() const;
		bool RestoreNow();
		void Dismiss();
		TSharedPtr<FJsonObject> MakeRestoreResultJson() const;

	private:
		const IViewportAdapter& Adapter;
		FResolvedViewport Viewport;
		FViewportSnapshot Snapshot;
		FString SnapshotError;
		FString RestoreError;
		bool bRestoreOnDestruct;
		bool bRestored;
		bool bDismissed;
	};
}
