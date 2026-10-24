#include "DomainAdapters/WorldDestructive/UeAgentPcgCleanupMutation.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "PCGComponent.h"
#include "PCGManagedResource.h"
#include "PCGSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::DomainAdapters::WorldDestructive
{
namespace
{
using namespace AssetDurable;
using Contract::FDiagnostic;

FDiagnostic Failure(const TCHAR* Code, const FString& Reason, const FString& Target,
    const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
{
    auto Error = FDiagnostic::Failure(Code, Reason, TEXT("pcg_cleanup"));
    Error.SourceKind = TEXT("native_result"); Error.TargetPath = Target;
    Error.PropertyPath = Field; Error.ActualValue = MoveTemp(Actual); Error.ExpectedValue = MoveTemp(Expected);
    return Error;
}

struct FComponentBefore
{
    FString Path;
    bool bGenerated = false;
    TArray<FString> ResourceClasses;
};

struct FOwnedObject
{
    FString Path;
    FString ClassPath;
};

TSharedPtr<FJsonValue> ComponentState(bool bPresent, bool bGenerated, const TArray<FString>& Resources)
{
    auto Value = MakeShared<FJsonObject>();
    Value->SetBoolField(TEXT("present"), bPresent); Value->SetBoolField(TEXT("generated"), bGenerated);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const auto& Resource : Resources) Items.Add(MakeShared<FJsonValueString>(Resource));
    Value->SetArrayField(TEXT("managed_resources"), MoveTemp(Items));
    return MakeShared<FJsonValueObject>(Value);
}

class FPcgCleanupMutation final : public IUeAgentWorldDestructiveDurableMutation
{
public:
    explicit FPcgCleanupMutation(UPCGComponent& InComponent)
        : Component(&InComponent), PackageName(InComponent.GetOwner()->GetLevel()->GetOutermost()->GetName()) {}

    bool Capture(FDiagnostic& Error)
    {
        TSet<UPCGComponent*> Seen;
        TSet<UObject*> Owned;
        bool bCancelledGeneration = false;
        TFunction<bool(UPCGComponent*)> Visit;
        auto AddActorPackage = [&](AActor& Actor, bool bDelete)
        {
            const FString MapName = Actor.GetLevel()->GetOutermost()->GetName();
            AddPackage(MapName, EPackageHeaderKind::Map, EAssetDurableIntent::Save, MapName);
            const FString ActorPackage = Actor.GetPackage()->GetName();
            if (ActorPackage != MapName)
                AddPackage(ActorPackage, EPackageHeaderKind::EvidenceRequired,
                    bDelete ? EAssetDurableIntent::Delete : EAssetDurableIntent::Save, MapName);
        };
        auto RecordObject = [&](UObject* Object)
        {
            if (IsValid(Object) && !Owned.Contains(Object))
            {
                Owned.Add(Object);
                Objects.Add({Object->GetPathName(), Object->GetClass()->GetPathName()});
            }
        };
        Visit = [&](UPCGComponent* Current) -> bool
        {
            if (!IsValid(Current) || Seen.Contains(Current)) return true;
            Seen.Add(Current);
            if (!Current->AreManagedResourcesAccessible() || Current->IsCleaningUp())
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetBoolField(TEXT("managed_resources_accessible"), Current->AreManagedResourcesAccessible());
                Actual->SetBoolField(TEXT("cleaning"), Current->IsCleaningUp());
                Error = Failure(TEXT("pcg_cleanup_resources_in_use"),
                    TEXT("An existing native cleanup still owns the PCG resources. Retry after that cleanup finishes."),
                    Current->GetPathName(), TEXT("cleanup"), MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(TEXT("accessible resources and no active cleanup")));
                Error.MutationMayHaveStarted = bCancelledGeneration;
                Error.bRetrySafe = true;
                return false;
            }
            // This is part of native Cleanup semantics. Do it before the package
            // checkpoint so no still-running generation can change its inventory.
            if (Current->IsGenerating())
            {
                Current->CancelGeneration();
                bCancelledGeneration = true;
            }
            AddActorPackage(*Current->GetOwner(), false);
            FComponentBefore Before;
            Before.Path = Current->GetPathName(); Before.bGenerated = Current->bGenerated;
            TArray<TObjectPtr<UPCGManagedResource>> Resources;
            Current->ForEachManagedResource([&Resources](UPCGManagedResource* Resource) { Resources.Add(Resource); });
            for (UPCGManagedResource* Resource : Resources)
            {
                if (!IsValid(Resource)) continue;
                Before.ResourceClasses.Add(Resource->GetName() + TEXT(": ") + Resource->GetClass()->GetPathName());
                if (auto* Actors = Cast<UPCGManagedActors>(Resource))
                {
                    for (const auto& Reference : Actors->GetConstGeneratedActors())
                    {
                        AActor* Actor = Reference.LoadSynchronous();
                        // Stale soft references already have no live resource to destroy.
                        if (!Actor) continue;
                        RecordObject(Actor); AddActorPackage(*Actor, true);
                        TArray<UPCGComponent*> Nested;
                        Actor->GetComponents(Nested);
                        for (UPCGComponent* Child : Nested) if (!Visit(Child)) return false;
                    }
                }
                else if (auto* Single = Cast<UPCGManagedComponent>(Resource))
                {
                    UActorComponent* Generated = Single->GeneratedComponent.Get();
                    RecordObject(Generated);
                    if (Generated && Generated->GetOwner()) AddActorPackage(*Generated->GetOwner(), false);
                    if (auto* Nested = Cast<UPCGComponent>(Generated))
                    {
                        if (!Visit(Nested)) return false;
                        NestedComponentCleanup.AddUnique(Nested);
                    }
                }
                else if (auto* List = Cast<UPCGManagedComponentList>(Resource))
                {
                    for (const auto& Reference : List->GeneratedComponents)
                    {
                        UActorComponent* Generated = Reference.Get();
                        RecordObject(Generated);
                        if (Generated && Generated->GetOwner()) AddActorPackage(*Generated->GetOwner(), false);
                        if (auto* Nested = Cast<UPCGComponent>(Generated))
                        {
                            if (!Visit(Nested)) return false;
                            NestedComponentCleanup.AddUnique(Nested);
                        }
                    }
                }
            }
            Before.ResourceClasses.Sort();
            Components.Add(MoveTemp(Before));
            bool bLocalsValid = true;
            if (auto* Subsystem = UPCGSubsystem::GetInstance(Current->GetWorld()))
                Subsystem->ForAllRegisteredLocalComponents(Current, [&](UPCGComponent* Local)
                {
                    if (bLocalsValid) bLocalsValid = Visit(Local);
                });
            return bLocalsValid;
        };
        if (!Visit(Component.Get())) return false;
        Packages.Sort([](const auto& A, const auto& B) { return A.PackageName < B.PackageName; });
        return true;
    }

    const FString& GetPackageName() const override { return PackageName; }
    const TArray<FWorldDestructivePackage>& GetMutationPackages() const override { return Packages; }
    const FString& GetPersistenceScope() const override
    {
        static const FString Scope(TEXT("world_map_asset_durable")); return Scope;
    }
    bool RequiresPackageCheckpoint() const override { return true; }
    bool Apply(FDiagnostic& Error) override
    {
        bool bNativeMutationStarted = false;
        ON_SCOPE_EXIT { if (!Error.IsEmpty()) Error.MutationMayHaveStarted = bNativeMutationStarted; };
        if (!Component.IsValid())
        {
            Error = Failure(TEXT("pcg_cleanup_component_unavailable"), TEXT("The PCG component is no longer available."),
                PackageName, TEXT("component"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            return false;
        }
        if (!Component->AreManagedResourcesAccessible() || Component->IsCleaningUp())
        {
            Error = Failure(TEXT("pcg_cleanup_resources_in_use"), TEXT("Native cleanup acquired the component resources before this cleanup could run."),
                Component->GetPathName(), TEXT("managed_resources_accessible"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            return false;
        }
        for (const auto& Before : Components)
        {
            auto* Current = FindObject<UPCGComponent>(nullptr, *Before.Path);
            if (!IsValid(Current) || !Current->AreManagedResourcesAccessible() || Current->IsCleaningUp())
            {
                Error = Failure(TEXT("pcg_cleanup_scope_changed"),
                    TEXT("A component in the captured cleanup scope is missing or its resources are owned by another native cleanup."),
                    Before.Path, TEXT("component_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
        }
        bNativeMutationStarted = true;
        // DestroyComponent does not release a nested PCG component's managed
        // Actors. Close that native resource closure before its owner destroys it.
        for (const auto& Nested : NestedComponentCleanup)
            if (Nested.IsValid() && Nested != Component) Nested->CleanupLocalImmediate(true, true);
        Component->CleanupLocalImmediate(true, true);
        // Check the original objects as well as the now-empty native ownership lists.
        for (const auto& Before : Components)
        {
            auto* Current = FindObject<UPCGComponent>(nullptr, *Before.Path);
            if (IsValid(Current) && !VerifyEmpty(*Current, Error)) return false;
        }
        for (const auto& Object : Objects)
        {
            if (IsValid(FindObject<UObject>(nullptr, *Object.Path)))
            {
                Error = Failure(TEXT("pcg_cleanup_owned_object_present"), TEXT("A PCG-managed object remains after native cleanup."),
                    Object.Path, TEXT("exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
                return false;
            }
        }
        return true;
    }
    bool Rollback(FDiagnostic& Error) override
    {
        Error = Failure(TEXT("pcg_cleanup_checkpoint_required"),
            TEXT("PCG cleanup recovery requires the world package checkpoint; Editor Undo cannot restore destroyed managed resources."),
            PackageName, TEXT("recovery"), MakeShared<FJsonValueString>(TEXT("native undo")),
            MakeShared<FJsonValueString>(TEXT("package checkpoint")));
        return false;
    }
    bool VerifyPersisted(UWorld&, const FPersistedWorldReferenceMap& References, FDiagnostic& Error) const override
    {
        for (const auto& Before : Components)
        {
            FString Path;
            if (!References.TryMapReference(Before.Path, Path)) continue;
            auto* Current = FindObject<UPCGComponent>(nullptr, *Path);
            const bool bOwnerRemoved = Objects.ContainsByPredicate([&Before](const FOwnedObject& Object)
                { return Before.Path == Object.Path || Before.Path.StartsWith(Object.Path + TEXT(".")); });
            if (!IsValid(Current) && bOwnerRemoved) continue;
            if (!IsValid(Current))
            {
                Error = Failure(TEXT("pcg_cleanup_persisted_component_missing"), TEXT("The retained PCG component is missing from independent persisted readback."),
                    Before.Path, TEXT("exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
                return false;
            }
            if (!VerifyEmpty(*Current, Error)) { Error.TargetPath = Before.Path; return false; }
        }
        for (const auto& Object : Objects)
        {
            FString Path;
            if (References.TryMapReference(Object.Path, Path) && IsValid(FindObject<UObject>(nullptr, *Path)))
            {
                Error = Failure(TEXT("pcg_cleanup_persisted_object_present"), TEXT("A removed PCG-managed object is still present in independent persisted readback."),
                    Object.Path, TEXT("exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
                return false;
            }
        }
        return true;
    }
    bool VerifyRestored(UWorld&, const FPersistedWorldReferenceMap& References, FDiagnostic& Error) const override
    {
        for (const auto& Before : Components)
        {
            FString Path;
            if (!References.TryMapReference(Before.Path, Path)) continue;
            auto* Current = FindObject<UPCGComponent>(nullptr, *Path);
            TArray<FString> Classes;
            if (IsValid(Current)) Current->ForEachConstManagedResource([&Classes](const UPCGManagedResource* Resource)
                { if (IsValid(Resource)) Classes.Add(Resource->GetName() + TEXT(": ") + Resource->GetClass()->GetPathName()); });
            Classes.Sort();
            if (!IsValid(Current) || Current->bGenerated != Before.bGenerated || Classes != Before.ResourceClasses)
            {
                Error = Failure(TEXT("pcg_cleanup_restore_unconfirmed"), TEXT("The package checkpoint did not restore the PCG component and its managed-resource inventory."),
                    Before.Path, TEXT("component_state"), ComponentState(IsValid(Current), IsValid(Current) && Current->bGenerated, Classes),
                    ComponentState(true, Before.bGenerated, Before.ResourceClasses));
                return false;
            }
        }
        for (const auto& Before : Objects)
        {
            FString Path;
            if (!References.TryMapReference(Before.Path, Path)) continue;
            UObject* Object = FindObject<UObject>(nullptr, *Path);
            if (!IsValid(Object) || Object->GetClass()->GetPathName() != Before.ClassPath)
            {
                Error = Failure(TEXT("pcg_cleanup_restore_object_missing"), TEXT("A PCG-managed object was not restored from the package checkpoint."),
                    Before.Path, TEXT("class"), MakeShared<FJsonValueString>(IsValid(Object) ? Object->GetClass()->GetPathName() : TEXT("missing")),
                    MakeShared<FJsonValueString>(Before.ClassPath));
                return false;
            }
        }
        return true;
    }

private:
    void AddPackage(const FString& Name, EPackageHeaderKind Kind, EAssetDurableIntent Intent, const FString& Map)
    {
        if (auto* Existing = Packages.FindByPredicate([&](const auto& Item) { return Item.PackageName == Name; }))
        {
            if (Intent == EAssetDurableIntent::Delete) Existing->Intent = Intent;
            return;
        }
        Packages.Add({Name, Kind, Intent, Map});
    }
    static bool VerifyEmpty(const UPCGComponent& Current, FDiagnostic& Error)
    {
        int32 ResourceCount = 0;
        Current.ForEachConstManagedResource([&ResourceCount](const UPCGManagedResource*) { ++ResourceCount; });
        const int32 Outputs = Current.GetGeneratedGraphOutput().TaggedData.Num();
        if (Current.IsGenerating() || Current.IsCleaningUp() || Current.bGenerated || ResourceCount != 0 || Outputs != 0)
        {
            auto Actual = MakeShared<FJsonObject>();
            Actual->SetBoolField(TEXT("generating"), Current.IsGenerating());
            Actual->SetBoolField(TEXT("cleaning"), Current.IsCleaningUp());
            Actual->SetBoolField(TEXT("generated"), Current.bGenerated);
            Actual->SetNumberField(TEXT("managed_resources"), ResourceCount);
            Actual->SetNumberField(TEXT("outputs"), Outputs);
            Error = Failure(TEXT("pcg_cleanup_incomplete"), TEXT("Native PCG cleanup has not released all generation state and managed resources."),
                Current.GetPathName(), TEXT("cleanup"), MakeShared<FJsonValueObject>(Actual),
                MakeShared<FJsonValueString>(TEXT("no active tasks, generated state, managed resources or outputs")));
            return false;
        }
        return true;
    }
    TWeakObjectPtr<UPCGComponent> Component;
    FString PackageName;
    TArray<FWorldDestructivePackage> Packages;
    TArray<FComponentBefore> Components;
    TArray<FOwnedObject> Objects;
    TArray<TWeakObjectPtr<UPCGComponent>> NestedComponentCleanup;
};
}

bool TryCreatePcgCleanupMutation(UPCGComponent& Component,
    TSharedPtr<IUeAgentWorldDestructiveDurableMutation>& OutMutation, Contract::FDiagnostic& OutError)
{
    OutMutation.Reset();
    auto Mutation = MakeShared<FPcgCleanupMutation>(Component);
    if (!Mutation->Capture(OutError)) return false;
    OutMutation = MoveTemp(Mutation);
    return true;
}
}
