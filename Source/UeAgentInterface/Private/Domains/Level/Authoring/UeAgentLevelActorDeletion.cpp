#include "Domains/Level/Authoring/UeAgentLevelActorDeletion.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Level/Authoring/UeAgentLevelActorDeletionUndo.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/LODActor.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Change.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/GCObject.h"
#include "UnrealEdGlobals.h"

namespace UeAgentLevelAuthoring
{
struct FActorDeletion::FRecord : FGCObject
{
    FTransaction Transaction{TEXT("UAIActorDeletion"), NSLOCTEXT("UeAgentInterface", "NativeActorDeletion", "Delete actors"), true};
    bool bDeleted = false;

    void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        // Match UTransBuffer: deletion calls GC before a possible rollback.
        Collector.AllowEliminatingReferences(false);
        Transaction.AddReferencedObjects(Collector);
        Collector.AllowEliminatingReferences(true);
    }
    FString GetReferencerName() const override { return TEXT("UAI.NativeActorDeletion"); }

    void SetDeleted(bool bDesired)
    {
        if (bDeleted == bDesired) return;
        TGuardValue<ITransaction*> UndoScope(GUndo, nullptr);
        TGuardValue<bool> TransactingScope(GIsTransacting, true);
        Transaction.BeginOperation();
        Transaction.Apply();
        Transaction.EndOperation();
        bDeleted = bDesired;
    }

    // Preserve normal Editor undo without moving or cancelling the user's
    // history. A private proxy avoids mixing custom and serialized records for
    // the actual World/Actor in the outer transaction.
    struct FUndo : FCommandChange
    {
        explicit FUndo(TSharedRef<FRecord> InRecord) : Record(MoveTemp(InRecord)) {}
        TSharedRef<FRecord> Record;
        void Apply(UObject*) override { Record->SetDeleted(true); }
        void Revert(UObject*) override { Record->SetDeleted(false); }
        void AddReferencedObjects(FReferenceCollector& Collector) override { Record->Transaction.AddReferencedObjects(Collector); }
        FString ToString() const override { return TEXT("UAI native actor deletion"); }
    };
};

FActorDeletion::FActorDeletion() = default;
FActorDeletion::~FActorDeletion() = default;

bool FActorDeletion::Apply(UWorld& World, const TArray<AActor*>& Actors, UE::UAI::Contract::FDiagnostic& Error)
{
    Error.Reset();
    if (!GUnrealEd || !GEditor || !GEditor->GetSelectedActors())
    {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_actor_editor_unavailable"),
            !GUnrealEd ? TEXT("The Unreal editor is unavailable for actor deletion.")
                : !GEditor ? TEXT("The editor engine is unavailable for actor deletion.")
                           : TEXT("The editor has no actor selection set for native deletion."), TEXT("actor_delete"));
        Error.PropertyPath = !GUnrealEd ? TEXT("unreal_editor_available") : !GEditor ? TEXT("editor_available") : TEXT("selection_set_available");
        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        Error.ActualValuePresent = Error.ExpectedValuePresent = true;
        Error.SourceKind = TEXT("native_result");
        return false;
    }
    TArray<TSharedPtr<FJsonValue>> RequestedActors;
    for (const AActor* Actor : Actors)
    {
        FText Reason;
        if (!IsValid(Actor) || Actor->GetWorld() != &World || !GUnrealEd->CanDeleteActor(Actor, &Reason))
        {
            const bool bValidActor = IsValid(Actor);
            const bool bTargetWorld = bValidActor && Actor->GetWorld() == &World;
            Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_actor_native_delete_rejected"),
                !bValidActor ? TEXT("An actor selected for deletion is no longer valid.")
                    : !bTargetWorld ? TEXT("The selected actor belongs to a different world.")
                    : Reason.IsEmpty() ? TEXT("Unreal rejected deletion of the selected actor without a more specific native reason.") : Reason.ToString(),
                TEXT("actor_delete"));
            Error.PropertyPath = !bValidActor ? TEXT("actor_valid") : !bTargetWorld ? TEXT("world_matches_target") : TEXT("can_delete_actor");
            Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
            Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            Error.ActualValuePresent = Error.ExpectedValuePresent = true;
            Error.SourceKind = TEXT("native_result");
            if (bValidActor) {
                Error.CandidateName = Actor->GetActorLabel();
                Error.InternalContext = Actor->GetPathName() + TEXT("; target_world=") + World.GetPathName();
            }
            return false;
        }
        RequestedActors.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));
    }
    Record = MakeShared<FRecord>();
    ITransaction* OuterTransaction = GUndo;
    bool bDeleted = false;
    Record->Transaction.BeginOperation();
    {
        TGuardValue<ITransaction*> UndoScope(GUndo, &Record->Transaction);
        // DeleteActors couples these native steps to its confirmation dialog.
        // Perform the same reference preparation without a modal prompt, then
        // let DeleteActors own groups, layers, typed selection, BSP, navigation
        // and destruction/GC. Keep native broken LevelScript references; they
        // are not a request to erase graph nodes.
        TArray<UClass*> Ignore;
        TMap<AActor*, TArray<AActor*>> Referencers;
        FBlueprintEditorUtils::GetActorReferenceMap(&World, Ignore, Referencers);
        for (AActor* Actor : Actors)
        {
            if (ULevelScriptBlueprint* Script = Actor->GetLevel()->GetLevelScriptBlueprint(true))
                FBlueprintEditorUtils::ModifyActorReferencedGraphNodes(Script, Actor);
            if (const auto* References = Referencers.Find(Actor))
                for (AActor* Referencer : *References)
                {
                    if (Actors.Contains(Referencer)) continue;
                    Referencer->Modify();
                    if (auto* Lod = Cast<ALODActor>(Referencer)) Lod->RemoveSubActor(Actor);
                }
        }
        bDeleted = GUnrealEd->DeleteActors(Actors, &World,
            GEditor->GetSelectedActors()->GetElementSelectionSet(), false, false, false);
    }
    Record->Transaction.Finalize();
    Record->Transaction.EndOperation();
    Record->bDeleted = true;
    if (!bDeleted)
    {
        Record->SetDeleted(false);
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_actor_native_delete_failed"),
            TEXT("Unreal's native actor deletion returned failure; the recorded deletion was rolled back."), TEXT("actor_delete"));
        Error.PropertyPath = TEXT("actors_deleted");
        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        Error.ActualValuePresent = Error.ExpectedValuePresent = true;
        Error.ProposedValue = MakeShared<FJsonValueArray>(MoveTemp(RequestedActors));
        Error.SourceKind = TEXT("native_result");
        Error.InternalContext = World.GetPathName();
        return false;
    }
    if (OuterTransaction)
    {
        UObject* Proxy = NewObject<UUeAgentLevelActorDeletionUndo>(GetTransientPackage(), NAME_None, RF_Transactional);
        OuterTransaction->StoreUndo(Proxy, MakeUnique<FRecord::FUndo>(Record.ToSharedRef()));
    }
    return true;
}

bool FActorDeletion::Restore(UE::UAI::Contract::FDiagnostic& Error)
{
    Error.Reset();
    if (Record) Record->SetDeleted(false);
    return true;
}
void FActorDeletion::Release() { Record.Reset(); }
}
