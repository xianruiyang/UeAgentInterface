#include "DomainAdapters/EditorProject/UeAgentSettingsAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "DomainAdapters/EditorProject/UeAgentSettingsConfig.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsDiscovery.h"
#include "ISettingsSection.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigContext.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
namespace
{
using namespace UeAgentAuthoringWorkspaceTransaction;
using namespace UeAgentAuthoringObjectApply;

bool Fail(FOperationFailure& Failure, UE::UAI::Contract::FDiagnostic Diagnostic)
{ Failure.Diagnostic = MoveTemp(Diagnostic); return false; }

UE::UAI::Contract::FDiagnostic SettingsApplyCondition(const FString& Code, const FString& Reason,
    const FString& Path)
{
    auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("settings_apply"));
    Error.SourceKind = TEXT("native_result");
    Error.PropertyPath = Path;
    Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
    Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    Error.ActualValuePresent = true;
    Error.ExpectedValuePresent = true;
    return Error;
}

UE::UAI::Contract::FDiagnostic SettingsNativeDifference(
    const FString& Code, const FString& Reason, FProperty& Property,
    const void* Expected, const void* Actual, UObject& Owner)
{
    const FString PropertyPath = TEXT("/") + Property.GetName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    auto Error = SettingsApplyCondition(Code, Reason, PropertyPath + TEXT("/native_value_matches"));
    Error.CandidateName = Property.GetName();
    Error.SubjectId = Owner.GetPathName();
    UeAgentPropertyValueCodec::FOptions Options;
    Options.bCompareStoredValues = true;
    const auto Difference = UeAgentPropertyValueCodec::ComparePropertyValues(
        &Property, Expected, Actual, &Owner, Options);
    if (!Difference.bEquivalent)
    {
        const auto LocatedDifference = Difference.WithPrefix(PropertyPath);
        LocatedDifference.ApplyTo(Error);
        Error.PropertyPath = LocatedDifference.Path;
        Error.Source.JsonPath.Reset();
    }
    return Error;
}

UeAgentApplyWorkspaceContract::FContract WorkspaceContract()
{
    using namespace UeAgentApplyWorkspaceContract;
    FContract Contract;
    Contract.Schema = TEXT("uai.apply_workspace_contract.v1");
    Contract.ContractId = TEXT("uai.apply_workspace.project_settings");
    Contract.ContractVersion = TEXT("1.0.0");
    Contract.ProfileId = TEXT("project_settings");
    Contract.Sources = {{ESourceKind::Directory, TEXT("physical"), TEXT("object")},
        {ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
        {ESourceKind::Inline, TEXT("virtual"), TEXT("object")}};
    Contract.Entry.DirectoryEntry = TEXT("index.hjson");
    Contract.Entry.VirtualUnitId = TEXT("settings");
    FUnitDescriptor Unit;
    Unit.UnitId = TEXT("settings"); Unit.PathPattern = TEXT("index.hjson");
    Unit.Role = TEXT("object"); Unit.Cardinality = TEXT("one"); Unit.bRequired = true;
    Unit.RouteId = TEXT("project_settings"); Unit.ApplyContractRef = TEXT("uai.apply_input.project_settings.native");
    Unit.MergePolicy = TEXT("replace"); Unit.AbsentSemantics = TEXT("error"); Unit.EmptySemantics = TEXT("error");
    Contract.Units = {Unit};
    Contract.ReferencePolicy.Base = TEXT("workspace_root");
    Contract.ReferencePolicy.RootContainment = TEXT("required");
    Contract.ReferencePolicy.ExternalReferences = TEXT("reject");
    Contract.ReferencePolicy.CaseCollision = TEXT("reject");
    Contract.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
    Contract.DependencyPolicy.CyclePolicy = TEXT("reject");
    Contract.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
    Contract.DependencyPolicy.Order = TEXT("stable_topological");
    Contract.Preflight.Scope = TEXT("whole_workspace");
    Contract.Preflight.MutationBeforeSuccess = TEXT("forbid");
    Contract.Preflight.bDiagnosticsIncludeSource = true;
    Contract.Transaction.Mode = TEXT("atomic");
    Contract.Transaction.RollbackOn = {TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")};
    Contract.Execution.Compile = TEXT("provider_hook");
    Contract.Execution.Save = TEXT("caller_controlled");
    Contract.Execution.Verification = TEXT("shared_snapshot_required");
    Contract.UnknownFilePolicy = TEXT("reject");
    Contract.DuplicateTargetPolicy = TEXT("reject");
    return Contract;
}

struct FPrepared final : IPreparedMutation
{
    UObject* Object = nullptr;
    // A same-class UObject can receive CDO default propagation. Keep immutable
    // native values in the same storage used by the common property apply layer.
    TMap<const FProperty*, TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage>> Before;
    FPreparedApply Edit;
    FString ConfigPath;
    FString CachePath;
    FString CacheFilePath;
    TUniquePtr<FConfigFile> CacheBefore;
    TSharedPtr<ISettingsSection> Section;
    FString GetUnitKey() const override { return TEXT("settings"); }
    FString GetTargetKey() const override { return Object->GetPathName(); }
    bool IsNoOp() const override { return Edit.NumProperties() == 0; }
    bool HasChanged(const FProperty& Property) const
    {
        const auto* Value = Before.Find(&Property);
        if (!Value) return false;
        for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
            if (!Property.Identical(Property.ContainerPtrToValuePtr<void>(Object, Index),
                static_cast<const uint8*>((*Value)->GetValuePtr()) + Index * Property.GetElementSize())) return true;
        return false;
    }
    bool ConfirmOriginal(const FString& Code, const FString& Reason,
        UE::UAI::Contract::FDiagnostic& OutError) const
    {
        for (const auto& Pair : Before)
        {
            if (!HasChanged(*Pair.Key)) continue;
            OutError = SettingsNativeDifference(Code, Reason, *const_cast<FProperty*>(Pair.Key),
                Pair.Value->GetValuePtr(), Pair.Key->ContainerPtrToValuePtr<void>(Object), *Object);
            return false;
        }
        return true;
    }
    UObject* MakeReadback() const
    {
        UObject* Copy = NewObject<UObject>(GetTransientPackage(), Object->GetClass(), NAME_None, RF_Transient);
        UObject* SuperDefault = Object->GetClass()->GetSuperClass()->GetDefaultObject();
        for (const auto& Pair : Before)
        {
            // Native SaveConfig removes inherited values equal to their parent
            // default; a missing override must not retain the old edited value.
            const void* Default = SuperDefault && SuperDefault->IsA(Pair.Key->GetOwnerClass())
                ? Pair.Key->ContainerPtrToValuePtr<void>(SuperDefault) : Pair.Value->GetValuePtr();
            Pair.Key->CopyCompleteValue(Pair.Key->ContainerPtrToValuePtr<void>(Copy), Default);
        }
        return Copy;
    }
};

class FPreflight final : public IPreflightProvider
{
public:
    explicit FPreflight(UObject& InObject) : Object(InObject) {}
    bool PreflightUnit(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit& Unit,
        TUniquePtr<IPreparedMutation>& Out, FOperationFailure& Failure) const override
    {
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
    if (!Unit.Document || !Unit.Document->TryGetArrayField(TEXT("properties"), Properties))
        {
            auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_properties_missing"),
                TEXT("The settings document requires a properties array."), TEXT("settings_apply"));
            Error.PropertyPath = TEXT("/properties");
            const auto Actual = Unit.Document ? Unit.Document->Values.FindRef(TEXT("properties")) : nullptr;
            Error.ActualValue = MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Actual));
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array"));
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = true;
            if (Unit.Document) UeAgentAuthoringDiagnostics::LocateFailure(Error, *Unit.Document,
                TEXT("settings_apply"), FString(), TEXT("properties"));
            return Fail(Failure, MoveTemp(Error));
        }
        FSettingsPage Page;
        UE::UAI::Contract::FDiagnostic Error;
        if (!FindSettingsPage(Object.GetPathName(), Page, Error)) return Fail(Failure, Error);
        if (Page.Section && !Page.Section->CanEdit())
        {
            auto ReadOnlyError = SettingsApplyCondition(TEXT("settings_native_read_only"),
                TEXT("The native settings section does not allow edits."), TEXT("/section_can_edit"));
            ReadOnlyError.CandidateName = Page.SectionId;
            return Fail(Failure, MoveTemp(ReadOnlyError));
        }
        auto Prepared = MakeUnique<FPrepared>();
        Prepared->Object = &Object;
        for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Config)) continue;
            auto Value = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(*It, It->ContainerPtrToValuePtr<void>(&Object), Error);
            if (!Value) return Fail(Failure, Error);
            Prepared->Before.Add(*It, MoveTemp(Value));
        }
        Prepared->ConfigPath = FPaths::ConvertRelativePathToFull(SettingsConfigFilename(Object));
        Prepared->CachePath = Object.GetClass()->GetConfigName();
        if (const FConfigFile* Cached = GConfig->FindConfigFile(Prepared->CachePath)) Prepared->CacheBefore = MakeUnique<FConfigFile>(*Cached);
        if (const FConfigBranch* Branch = GConfig->FindBranch(NAME_None, Prepared->CachePath))
            Prepared->CacheFilePath = FPaths::ConvertRelativePathToFull(Branch->IniPath);
        Prepared->Section = Page.Section;
        FOptions Options;
        Options.bAllowTemplateOwners = true;
        Options.bAllowDomainAuthorizedOwners = true;
        Options.bMarkPackageDirty = false;
        Options.bCompactValues = true;
        Options.IncludeRootProperty = [](const FProperty* Property) {
            return Property && Property->HasAnyPropertyFlags(CPF_Config);
        };
        FFailure ErrorResult;
        if (!PrepareObject(&Object, *Properties, Options, Prepared->Edit, ErrorResult))
        {
            Failure.Diagnostic = ErrorResult.Diagnostic;
            return Fail(Failure, ErrorResult.Diagnostic);
        }
        Out = MoveTemp(Prepared);
        return true;
    }
private:
    UObject& Object;
};

class FBackend final : public ITransactionBackend
{
public:
    FBackend(FPrepared& InState, TFunction<bool(FOperationFailure&)> Readback)
        : State(InState), FinalReadback(MoveTemp(Readback)) {}
    bool BeginWorkspace(const UeAgentAuthoringWorkspaceRouter::FWorkspace&,
        const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph&, FOperationFailure&) override { return true; }
    bool CaptureRollback(IPreparedMutation&, FOperationFailure& Failure) override
    { return State.ConfirmOriginal(TEXT("settings_preflight_state_changed"),
        TEXT("A native settings field changed after preflight captured its value."), Failure.Diagnostic); }
    bool ApplyUnit(IPreparedMutation&, FOperationFailure& Failure) override
    {
        FApplyResult Result;
        if (ApplyPrepared(State.Edit, Result))
        {
            if (State.Section && !State.Section->NotifySectionOnPropertyModified())
            {
                auto Error = SettingsApplyCondition(TEXT("settings_native_change_rejected"),
                    TEXT("The native settings section rejected its property modification notification."),
                    TEXT("/native_change_accepted"));
                Error.CandidateName = State.Section->GetName().ToString();
                return Fail(Failure, MoveTemp(Error));
            }
            return true;
        }
        Failure.Diagnostic = Result.Diagnostic;
        return Fail(Failure, Result.Diagnostic);
    }
    bool Compile(FOperationFailure&) override { return true; }
    bool Save(FOperationFailure&) override { return true; }
    bool Verify(FOperationFailure& Failure) override
    {
        ObservePrepared(State.Edit);
        return true;
    }
    bool RollbackUnit(IPreparedMutation&, FOperationFailure& Failure) override
    {
        FApplyResult Result;
        if (!RollbackPrepared(State.Edit, Result)) return Fail(Failure, Result.Diagnostic);
        // Native callbacks may also change dependent config fields.
        TArray<FProperty*> Changed;
        for (TFieldIterator<FProperty> It(State.Object->GetClass()); It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Config)) continue;
            if (State.HasChanged(**It)) Changed.Add(*It);
        }
        for (FProperty* Property : Changed)
        {
            State.Object->PreEditChange(Property);
            Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(State.Object), State.Before.FindChecked(Property)->GetValuePtr());
            FPropertyChangedEvent Event(Property);
            State.Object->PostEditChangeProperty(Event);
        }
        return true;
    }
    bool EndWorkspace(bool bCommitted, FOperationFailure&) override
    {
        if (!bCommitted && State.CacheBefore) GConfig->SetFile(State.CachePath, State.CacheBefore.Get());
        return true;
    }
    bool SupportsDurableTransactions() const override { return true; }
    bool RequiresPersistentReadback() const override { return true; }
    bool RequiresMemoryCheckpoint() const override { return false; }
    bool VerifyRestoredState(FOperationFailure& Failure) override
    { return State.ConfirmOriginal(TEXT("settings_restore_unconfirmed"),
        TEXT("A native settings field still differs from its captured value after rollback."), Failure.Diagnostic); }
    bool BuildDurableArtifactInventory(const TArray<IPreparedMutation*>&,
        TArray<FDurableArtifact>& Artifacts, FOperationFailure&) override
    {
        Artifacts = {{TEXT("project.settings"), State.ConfigPath}};
        if (!State.CacheFilePath.IsEmpty() && !FPaths::IsSamePath(State.CacheFilePath, State.ConfigPath))
            Artifacts.Add({TEXT("project.settings.generated"), State.CacheFilePath});
        return true;
    }
    bool StageDurableArtifacts(TArray<FDurableArtifact>& Artifacts, FOperationFailure& Failure) override
    {
        UE::UAI::Contract::FDiagnostic Error;
        // As in UE's settings save, preserve pending changes before rebuilding
        // the config hierarchy. The generated file is covered by the same disk rollback.
        if (State.CacheBefore) {
            const auto Saved = EditorResources::SaveConfigCacheFile(State.CachePath);
            if (Saved.Status == TEXT("failed"))
                return Fail(Failure, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("settings_pending_config_save_failed"), Saved.Diagnostic, TEXT("settings_apply")));
        }
        if (!StageSettingsConfig(*State.Object, [this](const FProperty& Property) { return State.HasChanged(Property); },
            State.ConfigPath, Artifacts[0].StagingPath, Error))
            return Fail(Failure, Error);
        Artifacts[0].bStagedExists = true;
        for (int32 Index = 1; Index < Artifacts.Num(); ++Index)
        {
            auto& Artifact = Artifacts[Index];
            Artifact.bStagedExists = FPaths::FileExists(Artifact.FinalPath);
            if (Artifact.bStagedExists)
            {
                const uint32 CopyResult = IFileManager::Get().Copy(*Artifact.StagingPath, *Artifact.FinalPath);
                if (CopyResult != COPY_OK)
                {
                    auto CopyError = SettingsApplyCondition(TEXT("settings_generated_config_stage_failed"),
                        TEXT("The generated settings configuration could not be copied to its staging file."),
                        TEXT("/copy_result"));
                    CopyError.Source.SourceFile = Artifact.FinalPath;
                    CopyError.TargetPath = Artifact.StagingPath;
                    CopyError.ActualValue = MakeShared<FJsonValueNumber>(CopyResult);
                    CopyError.ExpectedValue = MakeShared<FJsonValueNumber>(COPY_OK);
                    return Fail(Failure, MoveTemp(CopyError));
                }
            }
        }
        return true;
    }
    bool FinalizeVerification(FOperationFailure& Failure) override
    {
        TStrongObjectPtr<UObject> Persisted(State.MakeReadback());
        UE::UAI::Contract::FDiagnostic Error;
        if (!ReadSettingsConfig(*Persisted, *State.Object, State.ConfigPath, Error)) return Fail(Failure, Error);
        for (TFieldIterator<FProperty> It(State.Object->GetClass()); It; ++It)
        {
            if (!It->HasAnyPropertyFlags(CPF_Config)) continue;
            for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                if (State.HasChanged(**It)
                    && !It->Identical_InContainer(State.Object, Persisted.Get(), Index))
                {
                    auto ReadbackError = SettingsNativeDifference(TEXT("settings_persistent_readback_mismatch"),
                        TEXT("The saved settings field differs from the value applied in memory."), **It,
                        It->ContainerPtrToValuePtr<void>(State.Object), It->ContainerPtrToValuePtr<void>(Persisted.Get()),
                        *State.Object);
                    ReadbackError.Source.SourceFile = State.ConfigPath;
                    return Fail(Failure, MoveTemp(ReadbackError));
                }
        }
        FConfigContext Context = FConfigContext::ForceReloadIntoGConfig();
        Context.bWriteDestIni = false;
        if (!Context.Load(*State.Object->GetClass()->ClassConfigName.ToString()))
        {
            auto ReloadError = SettingsApplyCondition(TEXT("settings_config_cache_reload_failed"),
                TEXT("The native settings configuration hierarchy could not be reloaded into the cache."),
                TEXT("/cache_reload_succeeded"));
            ReloadError.CandidateName = State.Object->GetClass()->ClassConfigName.ToString();
            ReloadError.Source.SourceFile = State.CacheFilePath;
            ReloadError.SubjectId = State.CachePath;
            return Fail(Failure, MoveTemp(ReloadError));
        }
        return !FinalReadback || FinalReadback(Failure);
    }
private:
    FPrepared& State;
    TFunction<bool(FOperationFailure&)> FinalReadback;
};
}

bool ApplySettingsProperties(UObject& Object, const FString& RequestId,
    const TArray<TSharedPtr<FJsonValue>>& Properties,
    HjsonWorkspace::DomainAuthoring::FRootPropertyApplyOutcome& Outcome)
{
    // Authorize only this already discovered native settings package for this operation.
    const auto Handle = RegisterDomainAuthorizedOwnerPackagePrefix(Object.GetOutermost()->GetName());
    ON_SCOPE_EXIT { UnregisterDomainAuthorizedOwnerPackagePrefix(Handle); };
    const auto Contract = WorkspaceContract();
    auto Desired = MakeShared<FJsonObject>();
    Desired->SetArrayField(TEXT("properties"), Properties);
    UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
    if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(Contract,
        UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Desired, TEXT("inline://project-settings")), Load))
    { Outcome.Diagnostic = Load.Diagnostic; return false; }
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
    UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
    if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(Contract, Load.Workspace, {}, Graph, GraphFailure))
    { Outcome.Diagnostic = GraphFailure.Diagnostic; return false; }
    FPreflightResult Prepared;
    if (!PreflightWorkspace(Contract, Load.Workspace, Graph, FPreflight(Object), Prepared))
    { Outcome.Diagnostic = Prepared.Diagnostic; return false; }
    FExecutionOptions Options;
    FOperationFailure Failure;
    if (!UeAgentAuthoringOperationApply::BuildApplyEntryExecutionOptions(RequestId, Options, Failure))
    { Outcome.Diagnostic = Failure.Diagnostic; return false; }
    Options.bSaveWhenCallerControlled = true;
    FBackend Backend(static_cast<FPrepared&>(*Prepared.Units[0].Mutation), Outcome.FinalReadback);
    FExecutionResult Result;
    const bool bApplied = ExecuteAtomic(Contract, Load.Workspace, Graph, Prepared, Backend, Options, Result);
    Outcome.bApplied = bApplied;
    Outcome.Phase = Result.Phase;
    Outcome.Diagnostic = Result.Diagnostic;
    if (!bApplied && Result.bRollbackAttempted && !Result.bRollbackSucceeded)
    {
        Outcome.Diagnostic.PublicCause = MakeShared<const UE::UAI::Contract::FDiagnostic>(Result.Diagnostic);
        Outcome.Diagnostic.CauseCode = Result.Diagnostic.Code;
        Outcome.Diagnostic.Code = TEXT("project_settings_state_unknown");
        Outcome.Diagnostic.PublicMessage = TEXT("The project settings could not be confirmed restored after failure.");
        Outcome.Diagnostic.bRetrySafe = false;
    }
    return bApplied;
}
}
