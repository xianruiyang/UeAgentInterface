// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"

#include "Internationalization/Text.h"

namespace UeAgentPaletteSearch
{
	FString StripUiOnlyMarkers(const FString& InName)
	{
		FString Name = InName.TrimStartAndEnd();
		while (Name.StartsWith(TEXT("*")) || Name.EndsWith(TEXT("*")))
		{
			Name.RemoveFromStart(TEXT("*"));
			Name.RemoveFromEnd(TEXT("*"));
			Name.TrimStartAndEndInline();
		}
		return Name;
	}

	FString MenuNameFromText(const FText& Text, const FString& Fallback)
	{
		const FString* SourceString = FTextInspector::GetSourceString(Text);
		FString Name = SourceString ? *SourceString : FString();
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			Name = Text.ToString();
		}
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			Name = Fallback;
		}
		return StripUiOnlyMarkers(Name);
	}

	FString ObjectPathOrPackagePath(const FString& ObjectPath, const FString& PackagePath)
	{
		const FString CleanObjectPath = ObjectPath.TrimStartAndEnd();
		if (!CleanObjectPath.IsEmpty())
		{
			return CleanObjectPath;
		}
		return PackagePath.TrimStartAndEnd();
	}
}
