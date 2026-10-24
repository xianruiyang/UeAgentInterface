#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

#include "Engine/Level.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::EditorResources {
void CollectOwnedPackages(UPackage* Package, TArray<UPackage*>& OutPackages) {
  if (!Package) return;
  auto Append = [&](UPackage* Owned) {
    if (Owned != Package && IsPersistentPackage(Owned, true)) OutPackages.AddUnique(Owned);
  };
  for (UPackage* External : Package->GetExternalPackages()) Append(External);
  if (UWorld* World = UWorld::FindWorldInPackage(Package)) {
    ULevel* Level = World->PersistentLevel;
    if (IsValid(Level)) {
      if (Level->MapBuildData) Append(Level->MapBuildData->GetOutermost());
      for (UPackage* External : Level->GetLoadedExternalObjectPackages()) Append(External);
    }
  }
}

namespace {

struct FConfigScope {
  const TCHAR* Ref;
  const FString* Filename;
};

const FConfigScope ConfigScopes[] = {
    {TEXT("config:engine"), &GEngineIni},
    {TEXT("config:game"), &GGameIni},
    {TEXT("config:editor"), &GEditorIni},
    {TEXT("config:input"), &GInputIni},
    {TEXT("config:device_profiles"), &GDeviceProfilesIni},
    {TEXT("config:scalability"), &GScalabilityIni},
};

bool ValidatePersistentPackage(const UPackage* Package, bool bAllowTransient, Contract::FDiagnostic* Error) {
  auto Reject = [&](const TCHAR* Reason) {
    if (Error) {
      *Error = Contract::FDiagnostic::Failure(TEXT("package_not_persistent"), Reason, TEXT("save"));
      Error->TargetPath = IsValid(Package) ? Package->GetName() : FString();
      Error->ActualValuePresent = IsValid(Package);
    }
    return false;
  };
  if (!IsValid(Package)) return Reject(TEXT("The package to save is null or no longer valid."));
  if (Package == GetTransientPackage()) return Reject(TEXT("The global transient package cannot be saved."));
  if (!bAllowTransient && Package->HasAnyFlags(RF_Transient)) return Reject(TEXT("The package has the transient flag and this operation requires a persistent package."));
  if (Package->HasAnyPackageFlags(PKG_CompiledIn)) return Reject(TEXT("A compiled-in package cannot be saved as an asset package."));
  if (Package->HasAnyPackageFlags(PKG_PlayInEditor)) return Reject(TEXT("A Play In Editor package cannot be persisted."));
  if (!FPackageName::IsValidLongPackageName(Package->GetName(), false)) return Reject(TEXT("The package name is not a valid long package name."));
  FString Filename;
  if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename)) return Reject(TEXT("The package name cannot be mapped to a file path."));
  return true;
}

FSaveResult Failed(FString Ref, Contract::FDiagnostic Diagnostic) {
  if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Ref;
  if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Ref;
  return {MoveTemp(Ref), TEXT("failed"), MoveTemp(Diagnostic)};
}

bool NeedsSave(UPackage* Package) {
  return Package->IsDirty() || (!FPackageName::DoesPackageExist(Package->GetName())
      && !UPackage::IsEmptyPackage(Package));
}

bool HasUnsavedOwnedPackages(UPackage* Package) {
  TArray<UPackage*> Owned;
  CollectOwnedPackages(Package, Owned);
  return Owned.ContainsByPredicate([](UPackage* Item) { return NeedsSave(Item); });
}

bool SameConfigValues(const FConfigFile& A, const FConfigFile& B) {
  for (const auto& Section : A) {
    if (Section.Value.IsEmpty()) continue;
    const FConfigSection* Other = B.FindSection(Section.Key);
    if (!Other || Other->Num() != Section.Value.Num()) return false;
    TSet<FName> Keys;
    Section.Value.GetKeys(Keys);
    for (FName Key : Keys) {
      TArray<FConfigValue> Left, Right;
      Section.Value.MultiFind(Key, Left, true);
      Other->MultiFind(Key, Right, true);
      if (Left.Num() != Right.Num()) return false;
      // Native CalculateDiff compares value multisets, not array ordering.
      for (const FConfigValue& Value : Left) {
        const int32 Match = Right.IndexOfByPredicate([&](const FConfigValue& Candidate) {
          return Value.GetSavedValueForWriting() == Candidate.GetSavedValueForWriting();
        });
        if (Match == INDEX_NONE) return false;
        Right.RemoveAt(Match);
      }
    }
  }
  for (const auto& Section : B) {
    if (Section.Value.IsEmpty()) continue;
    const FConfigSection* Other = A.FindSection(Section.Key);
    if (!Other || Other->Num() != Section.Value.Num()) return false;
  }
  return true;
}

bool HasEmptyNativeConfigOutput(FConfigBranch& Branch) {
  const IConsoleVariable* Mode = IConsoleManager::Get().FindConsoleVariable(TEXT("ini.UseNewSaveTracking"));
  const int32 Tracking = Mode ? Mode->GetInt() : 0;
  if (Tracking == 1) return Branch.SavedLayer.IsEmpty();
  if (Tracking == 2) return SameConfigValues(Branch.InMemoryFile, Branch.FinalCombinedLayers);
  // This is the same native serializer used by SaveBranch in the default mode.
  // An absent file alone does not prove that Flush persisted an empty result.
  FString Output;
  Branch.InMemoryFile.WriteToString(Output, Branch.IniPath);
  return Output.IsEmpty();
}

} // namespace

FSaveResult SaveConfigCacheFile(const FString& CacheKey) {
  const FString& Ref = CacheKey;
  FConfigFile* Config = GConfig ? GConfig->FindConfigFile(CacheKey) : nullptr;
  if (!Config) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_resource_not_found"),
      !GConfig ? TEXT("The native config cache is unavailable.") : TEXT("The requested config file is not present in the native cache."), TEXT("save")));
  if (!Config->Dirty) return {Ref};
  // Do not persist the global Editor cache while native initialization is incomplete.
  if (!GConfig->IsReadyForUse()) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_cache_not_ready"), TEXT("The native config cache has not finished initialization."), TEXT("save")));
  if (GConfig->AreFileOperationsDisabled() || Config->NoSave
      || FParse::Param(FCommandLine::Get(), TEXT("nowrite"))
      || (FParse::Param(FCommandLine::Get(), TEXT("Multiprocess"))
          && !FParse::Param(FCommandLine::Get(), TEXT("MultiprocessSaveConfig"))))
    return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_save_disabled"),
        GConfig->AreFileOperationsDisabled() ? TEXT("File operations are disabled on the native config cache.")
        : Config->NoSave ? TEXT("The requested config file has its NoSave flag set.")
        : FParse::Param(FCommandLine::Get(), TEXT("nowrite")) ? TEXT("Config saving is disabled by the nowrite launch option.")
        : TEXT("Multiprocess mode requires MultiprocessSaveConfig to save config files."), TEXT("save")));

  FConfigBranch* Branch = GConfig->FindBranch(NAME_None, CacheKey);
  if (!Branch) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_resource_not_found"), TEXT("The requested config file has no native config branch."), TEXT("save")));
  if (Config != &Branch->InMemoryFile)
    return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_resource_branch_mismatch"), TEXT("The cached config file is not the in-memory file owned by its native branch."), TEXT("save")));
  const FString Path = Branch->IniPath;
  const bool bEmptyNativeOutput = HasEmptyNativeConfigOutput(*Branch);
  const bool bExisted = IFileManager::Get().FileExists(*Path);
  struct FNativeWrite {
    FCriticalSection Mutex;
    bool bObserved = false;
    FString Output;
  };
  const auto Write = MakeShared<FNativeWrite, ESPMode::ThreadSafe>();
  // Observe UE's own hierarchy/delta serialization, without replacing its writer
  // or claiming a successful delegate write. Flush itself has no result value.
  const FDelegateHandle Observer = FCoreDelegates::TSPreSaveConfigFileDelegate().AddLambda(
      [Write, Path](const TCHAR* SavedFilename, const FString& Contents, int32&) {
        if (FPaths::IsSamePath(Path, SavedFilename)) {
          FScopeLock Lock(&Write->Mutex);
          Write->Output = Contents;
          Write->bObserved = true;
        }
      });
  ON_SCOPE_EXIT {
    FCoreDelegates::TSPreSaveConfigFileDelegate().Remove(Observer);
  };
  // Known config files use a logical cache key (e.g. Engine), not necessarily
  // the physical IniPath. Preserve that key for native cache operations.
  GConfig->Flush(false, CacheKey);

  bool bObservedWrite;
  FString NativeOutput;
  {
    FScopeLock Lock(&Write->Mutex);
    bObservedWrite = Write->bObserved;
    NativeOutput = Write->Output;
  }
  bool bConfirmed = false;
  Contract::FDiagnostic Failure = Contract::FDiagnostic::Failure(TEXT("config_native_save_not_observed"), TEXT("The native config save did not report a write."), TEXT("save"));
  if (bObservedWrite) {
    FString Persisted;
    const bool bRead = FFileHelper::LoadFileToString(Persisted, *Path);
    bConfirmed = bRead && Persisted == NativeOutput;
    Failure = Contract::FDiagnostic::Failure(bRead ? TEXT("config_saved_content_mismatch") : TEXT("config_saved_file_unreadable"),
        bRead ? TEXT("The saved config does not match the native output.") : TEXT("The saved config file could not be read."), TEXT("save"));
  } else if (!IFileManager::Get().FileExists(*Path)) {
    // UE's empty-delta branch deletes directly and emits no PreSave event.
    // Require an actual deletion or a proven empty native output. A temporary
    // cache with nonempty changes and no file must remain dirty after Flush.
    bConfirmed = bExisted || bEmptyNativeOutput;
    Failure = Contract::FDiagnostic::Failure(TEXT("config_empty_delta_unconfirmed"), TEXT("The native config save did not confirm an empty delta."), TEXT("save"));
  }
  Config = GConfig->FindConfigFile(CacheKey);
  if (!Config) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_resource_unavailable_after_save"), TEXT("The config file disappeared from the native cache during saving."), TEXT("save_readback")));
  Config->Dirty = !bConfirmed;
  if (!bConfirmed) { Failure.Source.SourceFile = Path; return Failed(Ref, MoveTemp(Failure)); }
  return {Ref, !bObservedWrite && !bExisted ? TEXT("unchanged") : TEXT("saved")};
}

FString NormalizeResourceRef(const FString& ResourceRef) {
  const FString Ref = ResourceRef.TrimStartAndEnd();
  return Ref.StartsWith(TEXT("config:"))
      ? Ref : FPackageName::ObjectPathToPackageName(Ref);
}

bool IsPersistentPackage(const UPackage* Package, bool bAllowTransient) {
  return ValidatePersistentPackage(Package, bAllowTransient, nullptr);
}

const FString* ConfigFilenameForResourceRef(const FString& ResourceRef) {
  for (const FConfigScope& Scope : ConfigScopes)
    if (ResourceRef == Scope.Ref) return Scope.Filename;
  return nullptr;
}

void CollectDirtyResources(TArray<FDirtyResource>& OutResources) {
  OutResources.Reset();
  TSet<UPackage*> Seen;
  auto Append = [&](UPackage* Package, const TCHAR* Kind, bool bAllowTransient = false) {
    if (!IsPersistentPackage(Package, bAllowTransient) || !Package->IsDirty()
        || Seen.Contains(Package)) return;
    Seen.Add(Package);
    OutResources.Add({Package->GetName(), Kind, FString(), Package});
  };
  // These are ownership relationships, not arbitrary asset dependencies.
  for (TObjectIterator<UWorld> It; It; ++It) {
    UWorld* World = *It;
    if (!IsValid(World) || !IsPersistentPackage(World->GetOutermost())) continue;
    Append(World->GetOutermost(), TEXT("map"));
    for (ULevel* Level : World->GetLevels()) {
      if (!IsValid(Level)) continue;
      Append(Level->GetOutermost(), TEXT("map"));
      if (Level->MapBuildData) Append(Level->MapBuildData->GetOutermost(), TEXT("map"));
      for (UPackage* External : Level->GetLoadedExternalObjectPackages())
        Append(External, TEXT("map"), true);
    }
  }
  for (TObjectIterator<UPackage> It; It; ++It)
    Append(*It, It->HasAnyPackageFlags(PKG_ContainsMap) ? TEXT("map") : TEXT("asset"));
  if (GConfig) {
    for (const FConfigScope& Scope : ConfigScopes) {
      const FConfigFile* Config = GConfig->FindConfigFile(*Scope.Filename);
      if (Config && Config->Dirty)
        OutResources.Add({Scope.Ref, TEXT("config"), *Scope.Filename, nullptr});
    }
  }
  OutResources.Sort([](const FDirtyResource& A, const FDirtyResource& B) {
    return A.ResourceRef < B.ResourceRef;
  });
}

FSaveResult SaveResource(const FString& ResourceRef) {
  const FString Ref = NormalizeResourceRef(ResourceRef);
  if (Ref.StartsWith(TEXT("config:"))) {
    const FString* CacheKey = ConfigFilenameForResourceRef(Ref);
    if (!CacheKey) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("config_resource_not_found"), TEXT("The requested config resource is not registered."), TEXT("save")));
    FSaveResult Result = SaveConfigCacheFile(*CacheKey);
    Result.ResourceRef = Ref;
    return Result;
  }
  if (!FPackageName::IsValidLongPackageName(Ref, false))
    return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("package_ref_invalid"), TEXT("The resource reference is not a valid long package name."), TEXT("save")));
  UPackage* Package = FindPackage(nullptr, *Ref);
  if (!Package) {
    if (!FPackageName::DoesPackageExist(Ref))
      return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("package_not_found"), TEXT("The package is neither loaded nor present on disk."), TEXT("save")));
    Package = LoadPackage(nullptr, *Ref, LOAD_NoWarn | LOAD_Quiet);
    if (!Package) return Failed(Ref, Contract::FDiagnostic::Failure(TEXT("package_load_failed"), TEXT("The native package loader could not load the existing package."), TEXT("save")));
  }
  Contract::FDiagnostic Error;
  if (!ValidatePersistentPackage(Package, true, &Error)) return Failed(Ref, MoveTemp(Error));
  if (!Package->IsDirty() && !HasUnsavedOwnedPackages(Package)
      && FPackageName::DoesPackageExist(Ref)) return {Ref};
  if (!SavePackages({Package}, false, Error)) return Failed(Ref, MoveTemp(Error));
  return {Ref, TEXT("saved")};
}

bool SavePackages(const TArray<UPackage*>& Packages, bool bOnlyDirty, Contract::FDiagnostic& OutError) {
  OutError.Reset();
  auto Fail = [&](Contract::FDiagnostic Diagnostic) {
    OutError = MoveTemp(Diagnostic);
    return false;
  };
  TArray<UPackage*> Targets;
  for (UPackage* Package : Packages) {
    Contract::FDiagnostic Diagnostic;
    if (!ValidatePersistentPackage(Package, true, &Diagnostic)) return Fail(MoveTemp(Diagnostic));
    if (!bOnlyDirty || Package->IsDirty()) Targets.AddUnique(Package);
    TArray<UPackage*> Owned;
    CollectOwnedPackages(Package, Owned);
    for (UPackage* Item : Owned)
      if (NeedsSave(Item)) Targets.AddUnique(Item);
  }
  if (Targets.IsEmpty()) return true;
  struct FExpectedPackage {
    FString Name;
    TWeakObjectPtr<UPackage> Package;
    bool bEmpty;
  };
  TArray<FExpectedPackage> Expected;
  for (UPackage* Package : Targets)
    Expected.Add({Package->GetName(), Package, UPackage::IsEmptyPackage(Package)});
  // UE retains the serializer and map lifecycle. Explicitly include build data
  // and loaded external objects even when the parent package itself is clean.
  if (!UEditorLoadingAndSavingUtils::SavePackages(Targets, false)) {
    auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("package_save_failed"), TEXT("The native editor could not save all requested packages; it did not identify the failing package."), TEXT("save"));
    TArray<TSharedPtr<FJsonValue>> Names;
    for (const auto& Item : Expected) Names.Add(MakeShared<FJsonValueString>(Item.Name));
    Diagnostic.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Names));
    return Fail(MoveTemp(Diagnostic));
  }
  for (const FExpectedPackage& Item : Expected) {
    // Native saving cleans up empty packages, including external actor tombstones.
    // Cleanup can invalidate their objects; retain identity independently of UObject lifetime.
    const bool bExists = FPackageName::DoesPackageExist(Item.Name);
    if (Item.bEmpty) {
      if (bExists) {
        auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("empty_package_remains_on_disk"), TEXT("The empty package still exists on disk after saving."), TEXT("save_readback"));
        Diagnostic.TargetPath = Item.Name;
        return Fail(MoveTemp(Diagnostic));
      }
      continue;
    }
    UPackage* Package = Item.Package.Get();
    if (!Package) {
      auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("saved_package_unavailable"), TEXT("The saved package is no longer available for readback."), TEXT("save_readback"));
      Diagnostic.TargetPath = Item.Name;
      return Fail(MoveTemp(Diagnostic));
    }
    if (Package->IsDirty()) {
      auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("resource_remains_dirty"), TEXT("The package remains dirty after saving."), TEXT("save_readback"));
      Diagnostic.TargetPath = Item.Name;
      return Fail(MoveTemp(Diagnostic));
    }
    if (HasUnsavedOwnedPackages(Package)) {
      auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("owned_resource_remains_dirty"), TEXT("A native owned package remains dirty after saving."), TEXT("save_readback"));
      Diagnostic.TargetPath = Item.Name;
      return Fail(MoveTemp(Diagnostic));
    }
    if (!bExists) {
      auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("package_not_persisted"), TEXT("The package does not exist on disk after saving."), TEXT("save_readback"));
      Diagnostic.TargetPath = Item.Name;
      return Fail(MoveTemp(Diagnostic));
    }
  }
  return true;
}

} // namespace UE::UAI::EditorResources
