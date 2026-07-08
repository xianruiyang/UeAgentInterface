#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"

class UWidget;
class UWidgetBlueprint;

namespace UeAgentUmgOps
{
	FString NormalizeAssetPath(const FString& InPath);
	UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath);
	int32 SyncWidgetVariableGuidMap(UWidgetBlueprint* WidgetBlueprint);
	bool SaveWidgetBlueprintPackage(UWidgetBlueprint* WidgetBlueprint, FString& OutError);
	UWidget* FindWidgetByAnyName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName);
}
