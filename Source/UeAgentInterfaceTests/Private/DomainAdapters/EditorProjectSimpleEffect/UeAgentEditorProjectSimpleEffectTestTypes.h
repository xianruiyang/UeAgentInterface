#pragma once

#include "CoreMinimal.h"

#include "UeAgentEditorProjectSimpleEffectTestTypes.generated.h"

UCLASS(Config=EditorPerProjectUserSettings)
class UUeAgentEditorProjectSimpleEffectTestSettings final
	: public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Config)
	bool bTestEnabled = false;

	UPROPERTY(EditAnywhere, Config)
	int32 TestCount = 3;

	UPROPERTY(EditAnywhere, Config)
	FString TestLabel = TEXT("before");

	int32 PreEditChangeCount = 0;
	int32 PostEditChangeCount = 0;

	virtual void PreEditChange(FProperty* PropertyAboutToChange) override
	{
		++PreEditChangeCount;
		Super::PreEditChange(PropertyAboutToChange);
	}

	virtual void PostEditChangeProperty(
		FPropertyChangedEvent& PropertyChangedEvent) override
	{
		++PostEditChangeCount;
		Super::PostEditChangeProperty(PropertyChangedEvent);
	}
};

