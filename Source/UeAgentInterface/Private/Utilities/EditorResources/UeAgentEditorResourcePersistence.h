#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class UPackage;

namespace UE::UAI::EditorResources {

struct FDirtyResource {
  FString ResourceRef;
  FString ResourceKind;
  FString ObjectPath;
  UPackage* Package = nullptr;
};

struct FSaveResult {
  FString ResourceRef;
  FString Status = TEXT("unchanged");
  Contract::FDiagnostic Diagnostic;
};

FString NormalizeResourceRef(const FString& ResourceRef);
bool IsPersistentPackage(const UPackage* Package, bool bAllowTransient = false);
const FString* ConfigFilenameForResourceRef(const FString& ResourceRef);
FSaveResult SaveConfigCacheFile(const FString& CacheKey);
void CollectDirtyResources(TArray<FDirtyResource>& OutResources);
// Native package ownership only: external objects and map build data, not references.
void CollectOwnedPackages(UPackage* Package, TArray<UPackage*>& OutPackages);
// Saves only the supplied packages and native owned packages, never their references.
bool SavePackages(const TArray<UPackage*>& Packages, bool bOnlyDirty, Contract::FDiagnostic& OutError);
FSaveResult SaveResource(const FString& ResourceRef);

} // namespace UE::UAI::EditorResources
