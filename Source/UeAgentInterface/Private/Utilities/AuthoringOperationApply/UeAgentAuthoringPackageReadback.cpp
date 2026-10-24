#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/EditorWorld/UeAgentEditorWorldActivation.h"
#include "Editor.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAuthoringOperationApply
{
namespace
{
FString PackageNameFromRef(const FString& Ref)
{
    return Ref.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Ref) : Ref;
}
}

bool TryReplaceResidentPackages(TArray<UPackage*> Packages, UE::UAI::Contract::FDiagnostic& OutError)
{
    OutError.Reset();
    if (Packages.IsEmpty()) return true;
    for (UPackage* Package : Packages)
    {
        if (!Package || Package->HasAnyPackageFlags(PKG_ContainsMap))
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_replacement_map_unsupported"),
                Package ? TEXT("Resident package replacement does not support a package containing a map.") : TEXT("The package selected for resident replacement is null."), TEXT("package_readback"));
            OutError.SubjectId = Package ? Package->GetName() : FString();
            OutError.ActualValuePresent = Package != nullptr;
            return false;
        }
    }
    struct FReloadEntry { FString Name; UPackage* Before; int32 ClassDepth = 0; bool bWasDirty = false; };
    TArray<FReloadEntry> Entries;
    for (UPackage* Package : Packages)
    {
        FReloadEntry& Entry = Entries.AddDefaulted_GetRef();
        Entry.Name = Package->GetName(); Entry.Before = Package; Entry.bWasDirty = Package->IsDirty();
        TArray<UObject*> Objects;
        GetObjectsWithPackage(Package, Objects);
        for (UObject* Object : Objects) if (const UClass* Class = Cast<UClass>(Object))
        {
            TSet<UPackage*> Ancestors;
            for (const UClass* Parent = Class->GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
                if (UPackage* Owner = Parent->GetOutermost(); Owner != Package && Packages.Contains(Owner))
                    Ancestors.Add(Owner);
            Entry.ClassDepth = FMath::Max(Entry.ClassDepth, Ancestors.Num());
        }
    }
    // Use the complete editor lifetime, including its loading guard and
    // selection preparation, rather than copying only the render detach step.
    // UE's per-package ReparentHierarchies fixes live derived classes. Replacing
    // an ancestor and its derived class in the same native batch also feeds the
    // already-replaced derived class into that fixup (RF_NewerVersionExists is
    // not CLASS_NewerVersionExists), causing old/new CDO cross-talk. Finish each
    // native inheritance layer before loading the next; independent packages
    // remain batched. This is based on native class ownership, not asset names.
    Entries.StableSort([](const FReloadEntry& A, const FReloadEntry& B) { return A.ClassDepth < B.ClassDepth; });
    // Package loading creates replacement classes after the reload's initial
    // GC. Prepare those classes at the native pre-fixup boundary, before the
    // editor reparents/reinstances them (the same schema precondition used by
    // FReload::Reinstance and FBlueprintCompileReinstancer::ReparentChild).
    const FDelegateHandle SchemaPreparation = FCoreUObjectDelegates::OnPackageReloaded.AddLambda(
        [](EPackageReloadPhase Phase, FPackageReloadedEvent* Event) {
            if (Phase != EPackageReloadPhase::PrePackageFixup || !Event) return;
            for (const auto& Pair : Event->GetRepointedObjects())
                if (UClass* Class = Cast<UClass>(Pair.Value)) Class->AssembleReferenceTokenStream();
        });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(SchemaPreparation); };
    for (int32 Begin = 0; Begin < Entries.Num();)
    {
        int32 End = Begin;
        TArray<UPackage*> Batch;
        while (End < Entries.Num() && Entries[End].ClassDepth == Entries[Begin].ClassDepth)
        {
            // A prior layer may have reinstanced this package's classes.
            UPackage* Package = FindPackage(nullptr, *Entries[End].Name);
            if (!Package) {
                OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_replacement_missing"), TEXT("The resident package disappeared before its inheritance layer could be reloaded."), TEXT("package_readback"));
                OutError.SubjectId = Entries[End].Name; OutError.ActualValuePresent = false; return false;
            }
            Package->SetDirtyFlag(Entries[End].bWasDirty);
            Batch.AddUnique(Package);
            ++End;
        }
        FText ReloadError;
        if (!UPackageTools::ReloadPackages(Batch, ReloadError, EReloadPackagesInteractionMode::AssumePositive))
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_replacement_incomplete"),
                TEXT("Native reload could not replace the resident package batch."), TEXT("native_package_reload"));
            TArray<TSharedPtr<FJsonValue>> Names;
            for (int32 Index = Begin; Index < End; ++Index) Names.Add(MakeShared<FJsonValueString>(Entries[Index].Name));
            OutError.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(Names));
            OutError.ResourceDimension = TEXT("packages");
            OutError.ResourceObserved = Batch.Num();
            OutError.InternalContext = ReloadError.ToString();
            return false;
        }
        Begin = End;
    }
    for (const FReloadEntry& Entry : Entries)
    {
        UPackage* Reloaded = FindPackage(nullptr, *Entry.Name);
        if (!Reloaded || Reloaded == Entry.Before)
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_replacement_failed"),
                !Reloaded ? TEXT("The package is not loaded after native replacement.") : TEXT("Native reload retained the original package instance instead of replacing it."), TEXT("package_readback"));
            OutError.SubjectId = Entry.Name;
            OutError.ActualValuePresent = Reloaded != nullptr;
            return false;
        }
    }
    return true;
}

bool TryReloadPersistedPackage(const FString& TargetRef, UE::UAI::Contract::FDiagnostic& OutError)
{
    OutError.Reset();
    const FString Name = PackageNameFromRef(TargetRef);
    if (!FPackageName::DoesPackageExist(Name))
    {
        OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_not_published"), TEXT("The package has no published file to reload."), TEXT("package_readback"));
        OutError.SubjectId = Name; OutError.ActualValuePresent = false;
        return false;
    }
    if (UPackage* Package = FindPackage(nullptr, *Name))
    {
        ResetLoaders(Package);
        Package->SetDirtyFlag(false);
        Package->ClearPackageFlags(PKG_NewlyCreated);
        if (!Package->HasAnyPackageFlags(PKG_ContainsMap))
            return TryReplaceResidentPackages({Package}, OutError);

        const UWorld* CurrentWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        const bool bRestoreEditorWorld = CurrentWorld && CurrentWorld->GetOutermost() == Package;
        FText UnloadError;
        const bool bUnloaded = UPackageTools::UnloadPackages({Package}, UnloadError, true);
        CollectGarbage(RF_NoFlags, true);
        // UE may consume the entire unload set while replacing the active world,
        // then return false because its ordinary package loop had nothing left.
        if (!bUnloaded && FindPackage(nullptr, *Name))
        {
            OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_unload_failed"),
                TEXT("Native unload failed and the map package remains loaded."), TEXT("native_package_unload"));
            OutError.SubjectId = Name;
            OutError.InternalContext = UnloadError.ToString();
            return false;
        }
        // Native map unloading creates a blank editor world. Reloading just the
        // package would verify its bytes but leave the user's active map behind.
        if (bRestoreEditorWorld)
        {
            UE::UAI::EditorWorld::FWorldActivationResult Activation;
            UE::UAI::EditorWorld::FWorldActivationError Error;
            if (!UE::UAI::EditorWorld::TryActivateExistingWorld(Name,
                UE::UAI::EditorWorld::ECurrentWorldDirtyPolicy::RequireClean, Activation, Error))
            {
                OutError = Error;
                return false;
            }
        }
    }
    UPackage* Package = LoadPackage(nullptr, *Name, LOAD_None);
    if (!Package || Package->IsDirty())
    {
        OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_reload_failed"),
            !Package ? TEXT("The native loader could not reload the persisted package.") : TEXT("The package is dirty immediately after persistent reload."), TEXT("package_readback"));
        OutError.SubjectId = Name;
        return false;
    }
    return true;
}

bool TryRestorePackageDurability(
    const FString& TargetRef,
    const bool bNotifyAssetRegistryOnRemoval,
    const UE::UAI::AssetDurable::FAssetDurableSessionRef& Session,
    UE::UAI::AssetDurable::IAssetDurableBackend& Backend,
    const int64 FencingToken,
    UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
    UE::UAI::Contract::FDiagnostic& OutError,
    const TFunction<bool(UE::UAI::Contract::FDiagnostic&)>& ConfirmRestoredState)
{
    using namespace UE::UAI::AssetDurable;
    TArray<FString> Names{PackageNameFromRef(TargetRef)};
    for (const FDurableArtifact& Artifact : Session->GetInventory().GetArtifacts())
    {
        if (Artifact.GetArtifactKind() == EDurableArtifactKind::Package)
            Names.AddUnique(Artifact.GetPackageName());
    }
    for (const FString& Name : Names)
        if (UPackage* Package = FindPackage(nullptr, *Name)) ResetLoaders(Package);
    FDurableReceipt Receipt;
    FAssetDurableError Error;
    UE::UAI::Contract::FDiagnostic MemoryError;
    const bool bRestored = TryRollbackAssetDurability(
        Session, Backend, FencingToken,
        [&]()
        {
            // Restore every registered package, including enlisted dependencies.
            TArray<UPackage*> Resident, Removed;
            TArray<FString> RemovedNames, ResidentMapNames;
            for (const FString& Name : Names)
            {
                UPackage* Package = FindPackage(nullptr, *Name);
                if (FPackageName::DoesPackageExist(Name))
                {
                    if (!Package)
                    {
                        Package = LoadPackage(nullptr, *Name, LOAD_None);
                        if (!Package || Package->IsDirty())
                        {
                            MemoryError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_reload_failed"),
                                !Package ? TEXT("The native loader could not reload the restored package.") : TEXT("The restored package is dirty immediately after reload."), TEXT("package_readback"));
                            MemoryError.SubjectId = Name;
                            return false;
                        }
                    }
                    else
                    {
                        ResetLoaders(Package);
                        Package->SetDirtyFlag(false);
                        Package->ClearPackageFlags(PKG_NewlyCreated);
                        if (Package->HasAnyPackageFlags(PKG_ContainsMap)) ResidentMapNames.Add(Name);
                        else Resident.Add(Package);
                    }
                }
                else if (Package)
                {
                    Removed.Add(Package);
                    RemovedNames.Add(Name);
                }
            }
            for (UPackage* Package : Removed)
            {
                if (bNotifyAssetRegistryOnRemoval)
                {
                    if (UObject* Asset = FindObject<UObject>(Package,
                        *FPackageName::GetLongPackageAssetName(Package->GetName())))
                        FAssetRegistryModule::AssetDeleted(Asset);
                }
            }
            // Reload may GC children; reacquire removal targets by name afterwards.
            if (!TryReplaceResidentPackages(Resident, MemoryError)) return false;
            // Maps need the native world lifecycle, not ordinary asset reload.
            // It reactivates the editor world only when that map was active.
            for (const FString& Name : ResidentMapNames)
                if (!TryReloadPersistedPackage(Name, MemoryError)) return false;
            Removed.Reset();
            for (const FString& Name : RemovedNames)
            {
                if (UPackage* Package = FindPackage(nullptr, *Name))
                {
                    Package->SetDirtyFlag(false);
                    Removed.Add(Package);
                }
            }
            if (Removed.IsEmpty()) return !ConfirmRestoredState || ConfirmRestoredState(MemoryError);
            FText UnloadError;
            if (!UPackageTools::UnloadPackages(Removed, UnloadError, true))
            {
                MemoryError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_unload_failed"),
                    TEXT("Native unload could not release the removed packages during rollback."), TEXT("package_restore"));
                TArray<TSharedPtr<FJsonValue>> Targets;
                for (const FString& Name : RemovedNames) Targets.Add(MakeShared<FJsonValueString>(Name));
                MemoryError.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(Targets));
                MemoryError.ResourceDimension = TEXT("packages");
                MemoryError.ResourceObserved = Removed.Num();
                MemoryError.InternalContext = UnloadError.ToString();
                return false;
            }
            CollectGarbage(RF_NoFlags, true);
            for (const FString& Name : RemovedNames)
            {
                if (FindPackage(nullptr, *Name))
                {
                    MemoryError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_removal_unconfirmed"), TEXT("The package remains loaded after removal."), TEXT("package_restore"));
                    MemoryError.SubjectId = Name;
                    return false;
                }
            }
            return !ConfirmRestoredState || ConfirmRestoredState(MemoryError);
        }, Resources, Receipt, Error) && Receipt.GetStatus() == EDurableStatus::RolledBack;
    OutError.Reset();
    if (!bRestored)
    {
        OutError = Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_package_restore_failed"),
            TEXT("The package rollback could not be confirmed."), TEXT("package_restore")) : Error;
        OutError.Append(MemoryError);
        if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
        OutError.RollbackAttempted = true;
        OutError.RollbackSucceeded = false;
    }
    return bRestored;
}
}
