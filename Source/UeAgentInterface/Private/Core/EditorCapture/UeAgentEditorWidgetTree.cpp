#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"

#include "Layout/Children.h"
#include "Widgets/SWidget.h"

namespace UE::UAI::EditorCapture::WidgetTree
{
	bool Contains(const TSharedRef<SWidget>& Root, const TSharedRef<SWidget>& Widget)
	{
		if (Root == Widget) return true;
		if (FChildren* Children = Root->GetChildren())
			for (int32 Index = 0; Index < Children->Num(); ++Index)
				if (Contains(Children->GetChildAt(Index), Widget)) return true;
		return false;
	}

	bool FindFirstByType(
		const TSharedRef<SWidget>& Root,
		FName TypeName,
		TSharedPtr<SWidget>& OutWidget)
	{
		if (Root->GetType() == TypeName)
		{
			OutWidget = Root;
			return true;
		}
		if (FChildren* Children = Root->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (FindFirstByType(Children->GetChildAt(Index), TypeName, OutWidget))
					return true;
			}
		}
		return false;
	}

	bool FindAllByType(
		const TSharedRef<SWidget>& Root,
		FName TypeName,
		TArray<TSharedRef<SWidget>>& OutWidgets)
	{
		const int32 PreviousCount = OutWidgets.Num();
		if (Root->GetType() == TypeName)
			OutWidgets.Add(Root);
		if (FChildren* Children = Root->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
				FindAllByType(Children->GetChildAt(Index), TypeName, OutWidgets);
		}
		return OutWidgets.Num() != PreviousCount;
	}
}
