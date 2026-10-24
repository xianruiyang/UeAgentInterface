#include "Domains/PCG/UeAgentPcgGenerationWork.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::PcgGeneration
{
namespace
{
    Contract::FDiagnostic Failure(const TCHAR* Code, const TCHAR* Reason,
        const FString& Target, const TCHAR* Property, TSharedPtr<FJsonValue> Actual,
        TSharedPtr<FJsonValue> Expected, bool bMutationStarted)
    {
        auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("pcg_generation"));
        Error.TargetPath = Target;
        Error.PropertyPath = Property;
        Error.SourceKind = TEXT("native_result");
        Error.Source.JsonPath = TEXT("/subject_ref");
        Error.ActualValue = MoveTemp(Actual);
        Error.ExpectedValue = MoveTemp(Expected);
        Error.MutationStarted = bMutationStarted;
        Error.MutationMayHaveStarted = bMutationStarted;
        return Error;
    }

    TSharedRef<FJsonObject> ComponentState(const UPCGComponent& Component)
    {
        auto State = MakeShared<FJsonObject>();
        State->SetBoolField(TEXT("generated"), Component.bGenerated);
        State->SetBoolField(TEXT("generating"), Component.IsGenerating());
        State->SetBoolField(TEXT("cleaning_up"), Component.IsCleaningUp());
        State->SetBoolField(TEXT("refreshing"), Component.IsRefreshInProgress());
        State->SetNumberField(TEXT("output_count"), Component.GetGeneratedGraphOutput().TaggedData.Num());
        return State;
    }

    // Native delegates execute on the game thread. Capturing this separate state never
    // retains the session or a raw work pointer when deferred teardown is necessary.
    struct FGenerationSignals
    {
        FPCGTaskId OwnedTask = InvalidPCGTaskId;
        bool bRequesting = false;
        bool bCompleted = false;
        bool bCancelled = false;
        bool bSuperseded = false;
    };

    class FNativeGenerationWork final : public IGenerationWork
    {
    public:
        explicit FNativeGenerationWork(UPCGComponent& InComponent)
            : Component(&InComponent), ComponentPath(InComponent.GetPathName()),
              Signals(MakeShared<FGenerationSignals, ESPMode::ThreadSafe>()) {}

        ~FNativeGenerationWork() override
        {
            Detach();
        }

        bool Start(Contract::FDiagnostic& OutError)
        {
            check(IsInGameThread());
            UPCGComponent* Target = Component.Get();
            const auto State = Signals;
            StartedHandle = Target->OnPCGGraphStartGeneratingDelegate.AddLambda(
                [State](UPCGComponent* Source)
                {
                    if (State->bRequesting && State->OwnedTask == InvalidPCGTaskId)
                        State->OwnedTask = Source->GetGenerationTaskId();
                    else if (!State->bCompleted && !State->bCancelled)
                        State->bSuperseded = true;
                });
            GeneratedHandle = Target->OnPCGGraphGeneratedDelegate.AddLambda(
                [State](UPCGComponent*)
                {
                    if (State->OwnedTask != InvalidPCGTaskId && !State->bSuperseded && !State->bCancelled)
                        State->bCompleted = true;
                });
            CancelledHandle = Target->OnPCGGraphCancelledDelegate.AddLambda(
                [State](UPCGComponent*)
                {
                    if (State->OwnedTask != InvalidPCGTaskId && !State->bSuperseded && !State->bCompleted)
                        State->bCancelled = true;
                });
            Signals->bRequesting = true;
            // Explicit Generate always requests a generation, including unchanged inputs.
            // Native graph caching remains intact; no global cache flush or editor setting change.
            const FPCGTaskId Task = Target->GenerateLocalGetTaskId(true);
            Signals->bRequesting = false;
            if (Task == InvalidPCGTaskId || Task != Signals->OwnedTask)
            {
                OutError = Failure(TEXT("pcg_generation_not_scheduled"),
                    TEXT("Native PCG did not schedule the requested generation."), ComponentPath,
                    TEXT("generation_scheduled"), MakeShared<FJsonValueBoolean>(false),
                    MakeShared<FJsonValueBoolean>(true), true);
                Detach();
                return false;
            }
            return true;
        }

        FObservation Observe() override
        {
            check(IsInGameThread());
            if (Last.bFinished) return Last;
            auto Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("component_path"), ComponentPath);
            Details->SetBoolField(TEXT("generation_started"), Signals->OwnedTask != InvalidPCGTaskId);
            Details->SetBoolField(TEXT("generation_completed"), Signals->bCompleted);
            Details->SetBoolField(TEXT("cancel_requested"), bCancelRequested);
            UPCGComponent* Target = Component.Get();
            if (Target) Details->SetObjectField(TEXT("component_state"), ComponentState(*Target));
            Last.Details = MoveTemp(Details);

            if (Signals->bCompleted)
            {
                Last.bFinished = true;
                Last.Details->SetStringField(TEXT("outcome"), TEXT("generated"));
            }
            else if (!Target)
            {
                FinishFailure(TEXT("pcg_generation_component_unavailable"),
                    TEXT("The PCG component became unavailable before its generation completed."),
                    TEXT("component_available"), MakeShared<FJsonValueBoolean>(false),
                    MakeShared<FJsonValueBoolean>(true));
            }
            else if (Signals->bSuperseded || (Target->IsGenerating()
                && Target->GetGenerationTaskId() != Signals->OwnedTask))
            {
                FinishFailure(TEXT("pcg_generation_replaced"),
                    TEXT("Another generation replaced the operation-owned PCG task; it was left untouched."),
                    TEXT("generation_owner"), MakeShared<FJsonValueString>(TEXT("another generation")),
                    MakeShared<FJsonValueString>(TEXT("this operation")));
            }
            else if (Signals->bCancelled)
            {
                Last.bFinished = true;
                Last.Details->SetStringField(TEXT("outcome"), TEXT("cancelled"));
                if (!bCancelRequested)
                    FinishFailure(TEXT("pcg_generation_aborted"),
                        TEXT("Native PCG aborted the generation before completion."),
                        TEXT("generation_completed"), MakeShared<FJsonValueBoolean>(false),
                        MakeShared<FJsonValueBoolean>(true));
            }
            else if (!Target->IsGenerating() && !Target->IsCleaningUp() && !Target->IsRefreshInProgress())
            {
                FinishFailure(TEXT("pcg_generation_completion_missing"),
                    TEXT("The native PCG task ended without a generation completion or cancellation event."),
                    TEXT("generation_completed"), MakeShared<FJsonValueBoolean>(false),
                    MakeShared<FJsonValueBoolean>(true));
            }
            else Last.Details->SetStringField(TEXT("outcome"), bCancelRequested ? TEXT("cancelling") : TEXT("generating"));
            if (Last.bFinished) Detach();
            return Last;
        }

        void RequestCancel() override
        {
            check(IsInGameThread());
            if (Last.bFinished || Signals->bCompleted || Signals->bCancelled) return;
            bCancelRequested = true;
            if (UPCGComponent* Target = Component.Get())
            {
                if (Target->IsGenerating() && Target->GetGenerationTaskId() == Signals->OwnedTask)
                    Target->CancelGeneration();
            }
        }

    private:
        void FinishFailure(const TCHAR* Code, const TCHAR* Reason, const TCHAR* Property,
            TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
        {
            Last.bFinished = true;
            Last.Details->SetStringField(TEXT("outcome"), TEXT("failed"));
            Last.Diagnostic = Failure(Code, Reason, ComponentPath, Property, MoveTemp(Actual), MoveTemp(Expected), true);
        }

        void Detach()
        {
            if (!StartedHandle.IsValid() && !GeneratedHandle.IsValid() && !CancelledHandle.IsValid()) return;
            auto Remove = [Weak = Component, Started = StartedHandle, Generated = GeneratedHandle, Cancelled = CancelledHandle]()
            {
                if (UPCGComponent* Target = Weak.Get())
                {
                    Target->OnPCGGraphStartGeneratingDelegate.Remove(Started);
                    Target->OnPCGGraphGeneratedDelegate.Remove(Generated);
                    Target->OnPCGGraphCancelledDelegate.Remove(Cancelled);
                }
            };
            StartedHandle.Reset();
            GeneratedHandle.Reset();
            CancelledHandle.Reset();
            if (IsInGameThread()) Remove();
            else AsyncTask(ENamedThreads::GameThread, MoveTemp(Remove));
        }

        TWeakObjectPtr<UPCGComponent> Component;
        FString ComponentPath;
        TSharedRef<FGenerationSignals, ESPMode::ThreadSafe> Signals;
        FDelegateHandle StartedHandle, GeneratedHandle, CancelledHandle;
        FObservation Last;
        bool bCancelRequested = false;
    };
}

bool TryStart(UWorld* EditorWorld, const FString& ComponentPath,
    TSharedPtr<IGenerationWork, ESPMode::ThreadSafe>& OutWork, Contract::FDiagnostic& OutError)
{
    check(IsInGameThread());
    OutWork.Reset();
    UPCGComponent* Component = ComponentPath.StartsWith(TEXT("/"))
        ? FindObject<UPCGComponent>(nullptr, *ComponentPath) : nullptr;
    auto Reject = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Property,
        TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
    {
        OutError = Failure(Code, Reason, ComponentPath, Property, MoveTemp(Actual), MoveTemp(Expected), false);
        return false;
    };
    if (!IsValid(Component) || !Component->GetPathName().Equals(ComponentPath, ESearchCase::CaseSensitive))
        return Reject(TEXT("pcg_generation_component_not_found"),
            TEXT("No loaded PCG component matches the exact object path; read its component_path in the current world."),
            TEXT("component_path"), MakeShared<FJsonValueString>(ComponentPath),
            MakeShared<FJsonValueString>(TEXT("exact loaded PCG component object path")));
    if (!EditorWorld || EditorWorld->WorldType != EWorldType::Editor || Component->GetWorld() != EditorWorld)
        return Reject(TEXT("pcg_generation_world_mismatch"),
            TEXT("The PCG component must belong to the current Editor world."), TEXT("world"),
            MakeShared<FJsonValueString>(GetPathNameSafe(Component->GetWorld())),
            MakeShared<FJsonValueString>(GetPathNameSafe(EditorWorld)));
    if (Component->IsTemplate() || !IsValid(Component->GetOwner()) || Component->GetOwner()->IsTemplate() || Component->IsInPreviewMode())
        return Reject(TEXT("pcg_generation_preview_or_template"),
            TEXT("PCG generation requires a placed component outside preview mode."), TEXT("placed_non_preview_component"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    if (!Component->bActivated)
        return Reject(TEXT("pcg_generation_inactive"), TEXT("The PCG component is inactive."), TEXT("bActivated"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    if (!Component->GetGraph())
        return Reject(TEXT("pcg_generation_graph_missing"), TEXT("The PCG component has no graph assigned."), TEXT("graph"),
            MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("assigned PCG graph")));
    if (!Component->GetSubsystem())
        return Reject(TEXT("pcg_generation_subsystem_unavailable"), TEXT("The component's native PCG subsystem is unavailable."),
            TEXT("subsystem_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    if (Component->IsManagedByRuntimeGenSystem())
        return Reject(TEXT("pcg_generation_runtime_managed"),
            TEXT("This component belongs to the runtime generation scheduler and cannot accept a manual Editor generation."),
            TEXT("runtime_managed"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
    if (Component->IsGenerating() || Component->IsCleaningUp() || Component->IsRefreshInProgress())
        return Reject(TEXT("pcg_generation_component_busy"),
            TEXT("The component already has native generation, cleanup or refresh work; wait for its owner to finish."),
            TEXT("component_state"), MakeShared<FJsonValueObject>(ComponentState(*Component)),
            MakeShared<FJsonValueString>(TEXT("no active generation, cleanup or refresh")));

    const auto Work = MakeShared<FNativeGenerationWork, ESPMode::ThreadSafe>(*Component);
    if (!Work->Start(OutError)) return false;
    OutWork = Work;
    return true;
}
}
