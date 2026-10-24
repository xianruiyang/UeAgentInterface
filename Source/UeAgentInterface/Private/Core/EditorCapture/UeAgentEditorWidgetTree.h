#pragma once

#include "CoreMinimal.h"

class SWidget;

namespace UE::UAI::EditorCapture::WidgetTree
{
	// The caller supplies the asset's own tab subtree. This does not select an
	// active window or infer a C++ base class from a partial Slate type name.
	bool FindFirstByType(
		const TSharedRef<SWidget>& Root,
		FName TypeName,
		TSharedPtr<SWidget>& OutWidget);

	bool FindAllByType(
		const TSharedRef<SWidget>& Root,
		FName TypeName,
		TArray<TSharedRef<SWidget>>& OutWidgets);
	bool Contains(const TSharedRef<SWidget>& Root, const TSharedRef<SWidget>& Widget);
}
