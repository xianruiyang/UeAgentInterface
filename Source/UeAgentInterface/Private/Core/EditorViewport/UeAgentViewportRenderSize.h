#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FSceneViewport;
class FJsonObject;

namespace UE::UAI::EditorViewport
{
	// Uses one window-associated monitor's work area, never the virtual desktop.
	FIntPoint ResolveZeroViewportExtent(const TSharedPtr<FSceneViewport>& Viewport, FString& OutSource);

	// Uses the existing native scene viewport without changing its Slate layout.
	class FScopedRenderSize
	{
	public:
		~FScopedRenderSize();
		bool Apply(const TSharedPtr<FSceneViewport>& Viewport, FIntPoint Requested, UE::UAI::Contract::FDiagnostic& OutError);
		void Restore();
		TSharedPtr<FJsonObject> Describe() const;
	private:
		TWeakPtr<FSceneViewport> NativeViewport;
		FIntPoint OriginalSize = FIntPoint::ZeroValue;
		FIntPoint RenderSize = FIntPoint::ZeroValue;
		FString ExtentSource = TEXT("existing_viewport");
		bool bOriginallyFixed = false;
		bool bApplied = false;
		bool bChanged = false;
		bool bRestored = true;
	};
}
