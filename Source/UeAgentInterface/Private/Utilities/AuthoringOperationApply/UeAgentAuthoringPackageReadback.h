#pragma once

#include "CoreMinimal.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"

class UPackage;

namespace UeAgentAuthoringOperationApply
{
// Call after publication or restoration, with native mutation values released.
bool TryReplaceResidentPackages(TArray<UPackage*> Packages, UE::UAI::Contract::FDiagnostic& OutError);
// Map readback restores an active editor map through its native loading lifecycle.
UEAGENTINTERFACE_API bool TryReloadPersistedPackage(const FString& TargetRef, UE::UAI::Contract::FDiagnostic& OutError);
bool TryRestorePackageDurability(
    const FString& TargetRef,
    bool bNotifyAssetRegistryOnRemoval,
    const UE::UAI::AssetDurable::FAssetDurableSessionRef& Session,
    UE::UAI::AssetDurable::IAssetDurableBackend& Backend,
    int64 FencingToken,
    UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
    UE::UAI::Contract::FDiagnostic& OutError,
    const TFunction<bool(UE::UAI::Contract::FDiagnostic&)>& ConfirmRestoredState = {});
}
