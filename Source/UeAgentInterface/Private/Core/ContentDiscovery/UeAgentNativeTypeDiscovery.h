#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"

struct FAssetData;

namespace UE::UAI::ContentDiscovery
{
    /** Finish registry discovery before taking a candidate catalog snapshot. */
    bool TryEnsureAssetDiscovery(Contract::FDiagnostic& OutError);

    /** Include unloaded Blueprint-derived classes; the caller retains its native eligibility policy. */
    bool TryLoadDerivedBlueprintClasses(UClass& BaseClass,
        TArray<TStrongObjectPtr<UClass>>& OutLoadedClasses, Contract::FDiagnostic& OutError);

    /** Collect asset-backed types without losing declarations that have not been loaded. */
    bool TryCollectTypeAssets(UClass& AssetClass, TArray<FAssetData>& OutAssets,
        Contract::FDiagnostic& OutError);
}
