#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FSceneViewport;

namespace UE::UAI::EditorViewport
{
	// Uses the existing native scene viewport without changing its Slate layout.
	class FScopedRenderSize
	{
	public:
		~FScopedRenderSize();
		bool Apply(const TSharedPtr<FSceneViewport>& Viewport, FIntPoint Requested, UE::UAI::Contract::FDiagnostic& OutError);
		void Restore();
	private:
		TWeakPtr<FSceneViewport> NativeViewport;
		FIntPoint OriginalSize = FIntPoint::ZeroValue;
		bool bOriginallyFixed = false;
		bool bApplied = false;
	};
}
