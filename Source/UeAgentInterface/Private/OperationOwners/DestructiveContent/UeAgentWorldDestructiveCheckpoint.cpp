#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveCheckpoint.h"

#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
using namespace DomainAdapters::WorldDestructive;
namespace Workspace = UeAgentAuthoringWorkspaceTransaction;
namespace Operation = UeAgentAuthoringOperationApply;
using Contract::FDiagnostic;

namespace
{
FDiagnostic Failure(const TCHAR* Code, const FString& Reason, const FString& Target)
{
    auto Error = FDiagnostic::Failure(Code, Reason, TEXT("world_destructive_checkpoint"));
    Error.SourceKind = TEXT("native_result"); Error.TargetPath = Target;
    return Error;
}
}

FUeAgentWorldDestructiveResult ExecuteCheckpointedWorldMutation(
    UWorld& World, const FUeAgentWorldDestructiveRequest& Request,
    const TSharedRef<IUeAgentWorldDestructiveDurableMutation>& Mutation,
    const PlanningPreflight::FPlanningPreflightPlanRef& Plan, const FString& JournalRoot,
    const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
{
    FUeAgentWorldDestructiveResult Result;
    Result.CommandId = Request.CommandId; Result.TargetRef = Request.TargetRef;
    Result.PersistenceScope = Mutation->GetPersistenceScope();
    const FString ActiveMap = World.GetOutermost()->GetName();
    const bool bActiveEditorWorld = GEditor && GEditor->GetEditorWorldContext().World() == &World;
    TArray<Operation::FPackageDurablePlan> Packages;
    TMap<FString, bool> DirtyBefore;
    auto AddPackage = [&](UPackage* Package, const FString& OwningMap, Operation::EPackageDurableIntent Intent)
    {
        if (!Package || Package == GetTransientPackage()) return;
        if (auto* Existing = Packages.FindByPredicate([&](const auto& Item) { return Item.PackageName == Package->GetName(); }))
        {
            if (Intent != Operation::EPackageDurableIntent::Preserve) Existing->Intent = Intent;
            return;
        }
        Operation::FPackageDurablePlan Item;
        Item.PlanId = Package->GetName(); Item.PackageName = Package->GetName(); Item.Package = Package;
        Item.TopLevelAsset = Package->FindAssetInPackage(); Item.Intent = Intent;
        if (Item.PackageName != OwningMap) Item.OwningPackageName = OwningMap;
        DirtyBefore.Add(Item.PackageName, Package->IsDirty());
        Packages.Add(MoveTemp(Item));
    };
    // Reloading a map releases all loaded levels and their external packages.
    // Checkpoint this closure while preserving untouched disk versions on success.
    for (ULevel* Level : World.GetLevels())
    {
        if (!Level) continue;
        UPackage* Map = Level->GetOutermost();
        AddPackage(Map, Map->GetName(), Operation::EPackageDurableIntent::Preserve);
        for (UPackage* External : Map->GetExternalPackages())
            AddPackage(External, Map->GetName(), Operation::EPackageDurableIntent::Preserve);
        if (Level->MapBuildData)
            AddPackage(Level->MapBuildData->GetPackage(), Map->GetName(), Operation::EPackageDurableIntent::Preserve);
    }
    for (const auto& Target : Mutation->GetMutationPackages())
    {
        UPackage* Package = FindPackage(nullptr, *Target.PackageName);
        if (!Package)
        {
            Result.Diagnostic = Failure(TEXT("world_destructive_checkpoint_package_missing"),
                TEXT("A package in the native mutation scope is unavailable before checkpoint capture."), Target.PackageName);
            return Result;
        }
        AddPackage(Package, Target.WorldPackageName.IsEmpty() ? Mutation->GetPackageName() : Target.WorldPackageName,
            Target.Intent == AssetDurable::EAssetDurableIntent::Delete
                ? Operation::EPackageDurableIntent::Delete : Operation::EPackageDurableIntent::Save);
    }
    auto Stager = Operation::CreateDefaultDurablePackageStager();
    TArray<Workspace::FDurableArtifact> Artifacts;
    Workspace::FOperationFailure Detail;
    if (!Stager->BuildArtifactInventory(Packages, Artifacts, Detail))
    {
        Result.Diagnostic = MoveTemp(Detail.Diagnostic); return Result;
    }
    TMap<FString, FString> HeaderPaths;
    for (const auto& Target : Mutation->GetMutationPackages())
    {
        FString Header;
        if (!AssetDurable::TryResolvePackageHeaderFilename(Target.PackageName, Target.HeaderKind, Header, Result.Diagnostic))
            return Result;
        HeaderPaths.Add(Target.PackageName, MoveTemp(Header));
    }
    Workspace::FDurableTransactionCoordinator Durable;
    Workspace::FDurabilityResult Durability;
    Workspace::FExecutionOptions Options;
    Options.RequestId = Request.RequestId; Options.JournalRoot = JournalRoot;
    if (!Durable.RecoverPending(Options, Durability, Detail)
        || !Durable.Prepare(Options, Plan->GetFingerprint(), Artifacts, Durability, Detail))
    {
        Result.Diagnostic = MoveTemp(Detail.Diagnostic); return Result;
    }
    Result.JournalId = FPaths::GetCleanFilename(FPaths::GetPath(Durability.JournalPath));
    bool bMutated = false;
    auto RestoreDirty = [&]()
    {
        for (const auto& Before : DirtyBefore)
            if (UPackage* Package = FindPackage(nullptr, *Before.Key)) Package->SetDirtyFlag(Before.Value);
    };
    auto Fail = [&](FDiagnostic Error)
    {
        bool bMemoryRestored = !bMutated;
        Workspace::FOperationFailure Recovery;
        if (bMutated)
        {
            TArray<UPackage*> Loaded;
            for (const auto& Item : Packages)
                if (UPackage* Package = FindPackage(nullptr, *Item.PackageName))
                {
                    Package->SetDirtyFlag(false); ResetLoaders(Package); Loaded.AddUnique(Package);
                }
            bool bReleased;
            if (bActiveEditorWorld)
                bReleased = UEditorLoadingAndSavingUtils::NewBlankMap(false) != nullptr;
            else
            {
                FText UnloadError;
                bReleased = UPackageTools::UnloadPackages(Loaded, UnloadError, true);
                if (!bReleased) Recovery.Diagnostic = Failure(TEXT("world_destructive_checkpoint_unload_failed"), UnloadError.ToString(), ActiveMap);
            }
            if (!bReleased && Recovery.Diagnostic.IsEmpty())
                Recovery.Diagnostic = Failure(TEXT("world_destructive_checkpoint_unload_failed"),
                    TEXT("The mutated world could not be released before checkpoint recovery."), ActiveMap);
            if (bReleased && Durable.PublishMemoryCheckpoint(Durability, Recovery))
            {
                UWorld* RestoredWorld = bActiveEditorWorld ? UEditorLoadingAndSavingUtils::LoadMap(ActiveMap) : nullptr;
                if (!bActiveEditorWorld)
                    if (UPackage* Package = LoadPackage(nullptr, *ActiveMap, LOAD_DisableCompileOnLoad))
                        RestoredWorld = UWorld::FindWorldInPackage(Package);
                if (RestoredWorld)
                {
                    bMemoryRestored = true;
                    for (ULevel* Level : RestoredWorld->GetLevels())
                    {
                        if (!Level || !Level->GetWorld()) continue;
                        const FString Source = Level->GetOutermost()->GetName() + TEXT(".")
                            + FPackageName::GetShortName(Level->GetOutermost()->GetName());
                        AssetDurable::FPersistedWorldReferenceMap References(Source, Source);
                        FDiagnostic Semantic;
                        if (!Mutation->VerifyRestored(*RestoredWorld, References, Semantic))
                        { bMemoryRestored = false; Recovery.Diagnostic.Append(Semantic); }
                    }
                }
                else Recovery.Diagnostic.Append(Failure(TEXT("world_destructive_checkpoint_reload_failed"),
                    TEXT("The world could not be reloaded from its memory checkpoint."), ActiveMap));
            }
        }
        Error.Append(Recovery.Diagnostic);
        // The checkpoint supplied the in-memory version; disk keeps its own before-image.
        for (const auto& Item : Packages)
            if (UPackage* Package = FindPackage(nullptr, *Item.PackageName)) ResetLoaders(Package);
        Recovery = {};
        const bool bDiskRestored = Durable.RestoreDiskArtifacts(Durability, Recovery);
        Error.Append(Recovery.Diagnostic);
        Recovery = {};
        const bool bFinalized = Durable.FinalizeRestore(bMemoryRestored && bDiskRestored, Durability, Recovery);
        Error.Append(Recovery.Diagnostic); RestoreDirty();
        const bool bRestored = bMemoryRestored && bDiskRestored && bFinalized;
        Result.TerminalState = bRestored ? TEXT("rolled_back") : TEXT("state_unknown");
        Result.bChanged = bMutated && !bMemoryRestored; Result.bPersisted = !bDiskRestored;
        Error.RollbackAttempted = bMutated; Error.RollbackSucceeded = bRestored;
        Error.MutationMayHaveStarted = bMutated; Error.bRetrySafe = bRestored;
        Error.TerminalStatus = Result.TerminalState; Result.Diagnostic = MoveTemp(Error);
        return Result;
    };
    TArray<Operation::FPackageDurablePlan> BeforePlans = Packages;
    for (auto& Before : BeforePlans) Before.Intent = Operation::EPackageDurableIntent::Save;
    if (!Durable.CaptureMemoryCheckpoint([&](auto& Items, auto& Error)
        { return Stager->StagePackages(BeforePlans, Items, Error); }, Durability, Detail))
        return Fail(MoveTemp(Detail.Diagnostic));
    if (Cancellation->IsCancelled())
        return Fail(Failure(TEXT("world_destructive_cancelled"), TEXT("Cleanup was cancelled before native mutation."), Request.TargetRef));
    FDiagnostic MutationError;
    if (!Mutation->Apply(MutationError))
    {
        bMutated = MutationError.MutationMayHaveStarted.Get(true);
        return Fail(MoveTemp(MutationError));
    }
    bMutated = true;
    if (!Durable.Stage([&](auto& Items, auto& Error)
        { return Stager->StagePackages(Packages, Items, Error); }, Durability, Detail)
        || !Durable.Publish(Durability, Detail))
        return Fail(MoveTemp(Detail.Diagnostic));
    Result.bChanged = true; Result.bPersisted = true;
    // Observe each affected map with exactly its external package closure.
    for (const auto& Map : Mutation->GetMutationPackages())
    {
        if (Map.HeaderKind != AssetDurable::EPackageHeaderKind::Map) continue;
        TArray<AssetDurable::FDurableArtifact> ReadbackArtifacts;
        for (const auto& Target : Mutation->GetMutationPackages())
        {
            const FString OwnerMap = Target.WorldPackageName.IsEmpty() ? Mutation->GetPackageName() : Target.WorldPackageName;
            if (OwnerMap != Map.PackageName) continue;
            const FString& Header = HeaderPaths.FindChecked(Target.PackageName);
            const auto* Change = Plan->GetChangeSet().GetChanges().FindByPredicate([&](const auto& Item)
                { return Item.GetIdentity().GetOwnerId() == Target.PackageName; });
            if (!Change)
                return Fail(Failure(TEXT("world_destructive_package_plan_missing"),
                    TEXT("The affected package is missing from the prepared mutation plan."), Target.PackageName));
            ReadbackArtifacts.Emplace(Target.PackageName, Change->GetIdentity(), Target.PackageName,
                Header, Target.Intent, TEXT("package_header"));
        }
        if (!AssetDurable::TryObservePersistedWorld(ReadbackArtifacts,
            [&](UWorld& Persisted, const AssetDurable::FPersistedWorldReferenceMap& References, FDiagnostic& Error)
            { return Mutation->VerifyPersisted(Persisted, References, Error); }, MutationError))
            return Fail(MoveTemp(MutationError));
    }
    Result.bReadbackVerified = true;
    Result.VerificationId = TEXT("world-destructive-persisted-readback:") + Request.RequestId;
    if (!Durable.Commit(Durability, Detail)) return Fail(MoveTemp(Detail.Diagnostic));
    for (const auto& Item : Packages)
        if (Item.Intent != Operation::EPackageDurableIntent::Preserve)
            if (UPackage* Package = FindPackage(nullptr, *Item.PackageName)) Package->SetDirtyFlag(false);
    Result.TerminalState = TEXT("committed");
    return Result;
}
}
