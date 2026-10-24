// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ActorFactories/ActorFactory.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "UeAgentBrushBuilderActorFactory.generated.h"

class ULevel;

/** Native placement adapter for the brush-builder class published by BspMode. */
UCLASS(Transient)
class UUeAgentBrushBuilderActorFactory : public UActorFactory
{
	GENERATED_BODY()

public:
	UUeAgentBrushBuilderActorFactory();
	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	void ResetCreationDiagnostic() { CreationDiagnostic.Reset(); }
	const UE::UAI::Contract::FDiagnostic& GetCreationDiagnostic() const { return CreationDiagnostic; }

	/** Rebuild the owning level, including after a brush transform or rollback. */
	static bool SettleLevel(ULevel& Level, UE::UAI::Contract::FDiagnostic& OutError);

protected:
	virtual AActor* SpawnActor(UObject* Asset, ULevel* Level, const FTransform& Transform,
		const FActorSpawnParameters& SpawnParameters) override;

private:
	UE::UAI::Contract::FDiagnostic CreationDiagnostic;
};
