#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceRebase.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Paths.h"

namespace UE::UAI::HjsonWorkspace
{
namespace
{
    constexpr const TCHAR* ConsumerOwner = TEXT("uai.hjson.committed_export/1");
    // Durable owner/stream identity is unchanged. Only the atomically
    // published consumer state's baseline representation advances.
    constexpr const TCHAR* DirectConsumerStateSchema = TEXT("uai.hjson.committed_export/2");
    using namespace UeAgentAuthoringWorkspaceTransaction;

    struct FAcceptedFact {
        TOptional<FSemanticUnit> Desired, Actual;
        TSharedPtr<FJsonValue> PublishedBefore;
        FString SourceUnit, SourcePointer;
        TSet<FString> KeyedRoots;
    };
    using FAcceptedFacts = TMap<FString, FAcceptedFact>;
    struct FCommittedObservationMaterial final : IDurableConsumerMaterial {
        static constexpr const TCHAR* Type = TEXT("uai.hjson.committed_observation/1");
        TSharedPtr<const FCommittedWorkspaceDelta> Delta;
        FCompositionSnapshotBinding Binding;
        FString Scope;
        TSharedPtr<const FAcceptedFacts> AcceptedInputs;
        FCommittedObservationMaterial(TSharedPtr<const FCommittedWorkspaceDelta> InDelta,
            FCompositionSnapshotBinding InBinding, FString InScope, TSharedPtr<const FAcceptedFacts> InAcceptedInputs)
            : Delta(MoveTemp(InDelta)), Binding(MoveTemp(InBinding)), Scope(MoveTemp(InScope)), AcceptedInputs(MoveTemp(InAcceptedInputs)) {}
        const TCHAR* GetMaterialType() const override { return Type; }
    };

    bool Fail(FWorkspaceError& Error, const TCHAR* Code, const FString& Reason)
    {
        auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_committed_export"));
        Diagnostic.RecoveryAction = TEXT("export_incremental_or_full_to_new_workspace");
        Error.SetFailure(MoveTemp(Diagnostic));
        return false;
    }

    bool PendingAuthoring(FWorkspaceError& Error, const TCHAR* Code, const FString& Reason)
    {
        auto Notice = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_committed_export"));
        Notice.Severity = Contract::ESeverity::Warning; Notice.AssetApplied = true; Notice.RetryImport = false;
        Notice.RecoveryAction = TEXT("inspect_existing_transaction_without_replaying_import");
        Error.SetFailure(MoveTemp(Notice)); return false;
    }

    bool WriteJson(const TSharedRef<FJsonObject>& Json, FString& Out, FWorkspaceError& Error)
    {
        Out.Reset();
        const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        return FJsonSerializer::Serialize(Json, Writer)
            || Fail(Error, TEXT("workspace_committed_record_encode_failed"), TEXT("The committed export record could not be encoded."));
    }

    bool ReadJson(const FString& Text, TSharedPtr<FJsonObject>& Out, FWorkspaceError& Error)
    {
        const auto Reader = TJsonReaderFactory<TCHAR>::Create(Text);
        return (FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid())
            || Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("The committed export record is not a valid object."));
    }

    bool ReadPendingAuthorDocuments(const FJsonObject& Json,
        const FWorkspacePublication& Publication, TSet<FString>& OutDocuments)
    {
        OutDocuments.Reset();
        if (!Publication.bDirectDataflow) return false;
        FString Schema, Generation;
        const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
        if (!Json.TryGetStringField(TEXT("schema"), Schema) || Schema != DirectConsumerStateSchema
            || !Json.TryGetStringField(TEXT("publication_generation"), Generation) || Generation != Publication.Generation
            || !Json.TryGetArrayField(TEXT("pending_author_documents"), Documents)) return false;
        TSet<FString> Pending;
        for (const auto& Value : *Documents) {
            FString Path;
            if (!Value || !Value->TryGetString(Path) || Path.IsEmpty()) return false;
            Pending.Add(MoveTemp(Path));
        }
        OutDocuments = MoveTemp(Pending);
        return true;
    }

    bool ReadSequence(const FJsonObject& Json, const TCHAR* Field, int64& Out, FWorkspaceError& Error)
    {
        FString Text;
        if (!Json.TryGetStringField(Field, Text) || !LexTryParseString(Out, *Text) || Out < 0
            || !::LexToString(Out).Equals(Text, ESearchCase::CaseSensitive))
            return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("A committed export sequence is invalid."));
        return true;
    }

    TSharedRef<FJsonObject> BindingJson(const FCompositionSnapshotBinding& Binding)
    {
        const auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("snapshot"), Binding.SnapshotId);
        Json->SetStringField(TEXT("source"), Binding.Revision.GetSourceId());
        Json->SetStringField(TEXT("generation"), Binding.Revision.GetGeneration());
        Json->SetStringField(TEXT("sequence"), ::LexToString(Binding.Revision.GetSequence()));
        Json->SetStringField(TEXT("evidence"), Binding.EvidenceId);
        return Json;
    }

    bool ReadBinding(const FJsonObject& Json, TOptional<FCompositionSnapshotBinding>& Out, FWorkspaceError& Error)
    {
        FString Snapshot, Source, Generation, Evidence;
        int64 Sequence = -1;
        if (!Json.TryGetStringField(TEXT("snapshot"), Snapshot) || !Json.TryGetStringField(TEXT("source"), Source)
            || !Json.TryGetStringField(TEXT("generation"), Generation) || !Json.TryGetStringField(TEXT("evidence"), Evidence)
            || !ReadSequence(Json, TEXT("sequence"), Sequence, Error))
            return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("The committed snapshot binding is incomplete."));
        StateSemantic::FUeAgentStateRevision Revision(Source, Generation, Sequence);
        if (Snapshot.IsEmpty() || Evidence.IsEmpty() || !Revision.IsValid())
            return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("The committed snapshot binding is invalid."));
        Out.Emplace(FCompositionSnapshotBinding{Snapshot, Revision, Evidence});
        return true;
    }

    FString StreamKey(const FAuthoringModel& Model)
    {
        if (Model.GetTargetUnitIds().Num() != 1) return {};
        const auto Root = Model.FindDeclaration(Model.GetTargetUnitIds()[0]);
        if (!Root || Root->GetUnitKind() != ESemanticUnitKind::TargetState) return {};
        // Graph compositions bind their native identities on the actual graph
        // and item entries. Their validated target root still has a stable,
        // asset-scoped unit identity; an export.target entry is not required.
        return Root->GetTargetIdentityKey().IsEmpty() ? Root->GetUnitId() : Root->GetTargetIdentityKey();
    }

    bool ReadRecords(const FAuthoringModel& Model, TArray<FDurableConsumerRecord>& Records, FWorkspaceError& Error,
        EDurableConsumerBody Body = EDurableConsumerBody::Complete, int64 BodyAfterSequence = 0)
    {
        const FString Stream = StreamKey(Model);
        if (Stream.IsEmpty()) return Fail(Error, TEXT("workspace_committed_target_unsupported"),
            TEXT("Committed export requires one validated target root."));
        FOperationFailure Failure;
        if (!ReadDurableConsumerRecords(UeAgentAuthoringOperationApply::GetAuthoringDurableJournalRoot(),
            ConsumerOwner, Stream, Records, Failure, Body, BodyAfterSequence)) { Error.SetFailure(MoveTemp(Failure.Diagnostic)); return false; }
        return true;
    }

    bool IsDiscarded(const FString& Phase)
    {
        return Phase == TEXT("restored") || Phase == TEXT("startup_restored") || Phase == TEXT("memory_complete")
            || Phase == TEXT("disk_restored_memory_preserved");
    }

    TSharedRef<FJsonObject> StructuralIntentJson(const FStructuralEdit& Edit)
    {
        const auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("id"), Edit.GetEditId());
        Json->SetStringField(TEXT("kind"), LexToString(Edit.GetKind()));
        Json->SetStringField(TEXT("owner"), Edit.GetOwnerUnitId());
        Json->SetStringField(TEXT("subject"), Edit.GetSubjectUnitId());
        Json->SetStringField(TEXT("replacement"), Edit.GetReplacementUnitId());
        Json->SetStringField(TEXT("relation"), Edit.GetRelationId());
        TArray<TSharedPtr<FJsonValue>> Order;
        for (const auto& Id : Edit.GetOrderedUnitIds()) Order.Add(MakeShared<FJsonValueString>(Id));
        Json->SetArrayField(TEXT("ordered"), MoveTemp(Order));
        return Json;
    }

    bool StructuralIntentMatches(const FStructuralEdit& Edit, const FJsonObject& Json)
    {
        FString Value;
        const auto Matches = [&](const TCHAR* Field, const FString& Expected) {
            return Json.TryGetStringField(Field, Value) && Value == Expected;
        };
        if (!Matches(TEXT("kind"), LexToString(Edit.GetKind())) || !Matches(TEXT("owner"), Edit.GetOwnerUnitId())
            || !Matches(TEXT("subject"), Edit.GetSubjectUnitId()) || !Matches(TEXT("replacement"), Edit.GetReplacementUnitId())
            || !Matches(TEXT("relation"), Edit.GetRelationId())) return false;
        const TArray<TSharedPtr<FJsonValue>>* Order = nullptr;
        if (!Json.TryGetArrayField(TEXT("ordered"), Order) || Order->Num() != Edit.GetOrderedUnitIds().Num()) return false;
        for (int32 Index = 0; Index < Order->Num(); ++Index)
            if (!(*Order)[Index] || !(*Order)[Index]->TryGetString(Value) || Value != Edit.GetOrderedUnitIds()[Index]) return false;
        return true;
    }

    FSemanticUnit KeyedAcceptedUnit(const FSemanticUnit& Unit, TMap<FString, StateSemantic::FUeAgentCanonicalValueRef> Values)
    {
        // Operation rows do not consume the complete parent child/relation
        // arrays. Keep their binding/type, not another N-member inventory.
        return FSemanticUnit(Unit.GetSharedUnitId(), Unit.GetUnitKind(), Unit.GetOwnerRef(), Unit.GetSemanticTypeRef(),
            Unit.GetRole(), Unit.GetFieldSetRef(), MoveTemp(Values), {}, {}, Unit.GetOrderingRef(), Unit.GetLayoutRef(),
            Unit.GetDependencyRefs(), Unit.GetProfileVisibility(), Unit.GetSourceProvenance(), Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
    }

    bool MakeState(const FString& TargetKind, const FString& Target, const FAuthoringModel& Model,
        const FCompositionSnapshotBinding& Binding, int64 Sequence, FString& Out, FWorkspaceError& Error,
        const FString* PublishedSemanticFingerprint = nullptr, const FWorkspacePublication* Publication = nullptr,
        const TSet<FString>* PendingAuthorDocuments = nullptr)
    {
        const auto Json = MakeShared<FJsonObject>();
        const bool bDirect = Publication && Publication->bDirectDataflow;
        Json->SetStringField(TEXT("schema"), bDirect ? DirectConsumerStateSchema : ConsumerOwner);
        Json->SetStringField(TEXT("target_kind"), TargetKind);
        Json->SetStringField(TEXT("target"), Target);
        Json->SetStringField(TEXT("stream"), StreamKey(Model));
        Json->SetStringField(TEXT("profile"), Model.GetProfileId());
        Json->SetStringField(TEXT("through_sequence"), ::LexToString(Sequence));
        if (bDirect) Json->SetStringField(TEXT("publication_generation"), Publication->Generation);
        else Json->SetStringField(TEXT("model_fingerprint"), PublishedSemanticFingerprint ? *PublishedSemanticFingerprint : Model.GetSemanticFingerprint());
        Json->SetObjectField(TEXT("binding"), BindingJson(Binding));
        if (bDirect && PendingAuthorDocuments) {
            // Produced by the same actual formatter and pending-author patch
            // operation as this publication. Empty is known empty; absence in
            // an older/bootstrap state is unknown, never applied status.
            TArray<FString> Paths = PendingAuthorDocuments->Array(); Paths.Sort();
            TArray<TSharedPtr<FJsonValue>> Documents;
            for (const auto& Path : Paths) Documents.Add(MakeShared<FJsonValueString>(Path));
            Json->SetArrayField(TEXT("pending_author_documents"), MoveTemp(Documents));
        }
        return WriteJson(Json, Out, Error);
    }

    bool ComposeObservedState(const FString& TargetKind, const FAuthoringModel& Source,
        const UeAgentAuthoringState::FStateSnapshot& Observed, FUeAgentResourceTracker& Resources,
        StateSemantic::FUeAgentCanonicalSnapshotPtr& Canonical, FCompositionUnitSet& Units, FWorkspaceError& Error)
    {
        TArray<StateSemantic::FUeAgentCanonicalSnapshotPtr> Projections;
        TArray<int32> Plain;
        for (int32 Index = 0; Index < Observed.Num(); ++Index) {
            const auto& Entry = Observed.ReadAuthoringEntry(Index);
            if (Entry.AuthoringProjection) Projections.Add(Entry.AuthoringProjection);
            else if (Entry.bAuthoringVisible) Plain.Add(Index);
        }
        if (Plain.Num() == Observed.Num() && !Plain.IsEmpty()) {
            return TryBuildCanonicalAuthoringSnapshot(TargetKind, Source.GetProfileId(), Observed, Resources, Canonical, Error)
                && TryComposeScopedAuthoringUnits(TargetKind, Source.GetProfileId(), Canonical, Resources, Units, Error, &Source);
        }
        if (!Plain.IsEmpty()) {
            UeAgentAuthoringState::FStateSnapshot Selected;
            if (!Observed.TrySelectEntries(Plain, Selected)) return false;
            StateSemantic::FUeAgentCanonicalSnapshotPtr Fields;
            if (!TryBuildCanonicalAuthoringSnapshot(TargetKind, Source.GetProfileId(), Selected, Resources, Fields, Error)) return false;
            Projections.Add(MoveTemp(Fields));
        }
        if (Projections.IsEmpty()) return false; // No public observation was produced.
        if (Projections.Num() == 1) Canonical = Projections[0]; // Direct handoff; not a cardinality restriction.
        else {
            // These sealed projections are constituents of this one Before
            // or Actual observation. Its native binding identifies the union.
            const auto Binding = BuildNativeAuthoringCaptureBinding(TargetKind, Observed);
            TArray<StateSemantic::FUeAgentStateEntry> Entries;
            TMap<FString, int32> Indices;
            for (const auto& Projection : Projections) for (const auto& Entry : Projection->GetEntries()) {
                auto Identity = Entry.GetIdentity().WithGeneration(Binding.Revision.GetGeneration());
                auto Default = Entry.GetDefault();
                if (Default.GetSourceKind() == StateSemantic::EUeAgentDefaultSourceKind::PostCreateReadback
                    && Default.GetSourceIdentity() == Entry.GetIdentity().GetCanonicalKey())
                    Default = StateSemantic::FUeAgentDefaultValue(Default.GetSourceKind(), Identity.GetCanonicalKey(),
                        Default.GetProvenance(), Default.GetValue());
                const FString Key = Identity.GetCanonicalKey();
                auto Bound = Entry.WithIdentityAndDefault(MoveTemp(Identity), MoveTemp(Default), Binding.Revision);
                if (const auto* Existing = Indices.Find(Key)) Entries[*Existing] = MoveTemp(Bound);
                else { Indices.Add(Key, Entries.Num()); Entries.Add(MoveTemp(Bound)); }
            }
            const auto& First = Projections[0];
            StateSemantic::FUeAgentStateSemanticError Failure;
            StateSemantic::FUeAgentStateProvenance Origin(TEXT("independent_readback"), TargetKind, Binding.EvidenceId);
            if (!StateSemantic::FUeAgentCanonicalSnapshot::TryCreate(Binding.SnapshotId, First->GetStatePlane(),
                Binding.Revision, First->GetCapturedAt(), MoveTemp(Origin), MoveTemp(Entries), Resources, Canonical, Failure)) {
                Error.SetFailure(MoveTemp(Failure)); return false;
            }
        }
        return TryComposeScopedAuthoringUnits(TargetKind, Source.GetProfileId(), Canonical,
            Resources, Units, Error, &Source);
    }
}

bool HasCommittedExportConsumer(const FString& TargetKind) {
    FTargetCompositionContract Contract;
    if (FTargetCompositionRegistry::Get().FindContract(TargetKind, Contract)) return true;
    // Registration is lazy in the composition owner. A cold SDK request must
    // have the same consumer as a request preceded by a full export.
    Contract::FDiagnostic Error;
    return RegisterGraphTreeTargetCompositionContracts(Error)
        && RegisterOtherTargetCompositionContracts(Error)
        && FTargetCompositionRegistry::Get().FindContract(TargetKind, Contract);
}

bool TryPrepareCommittedWorkspacePlan(const FCommittedExportSelection& Selection, const FString& Destination,
    FPreparedWorkspaceExport& Prepared, FUeAgentResourceTracker& Resources, FWorkspaceError& Error)
{
    Prepared.PendingAuthorUnits.Reset(); Prepared.PendingAuthorLocations.Reset(); Prepared.PendingKeyedAuthorRows.Reset();
    if (!Selection.Model || !Selection.Binding.IsSet()) return Fail(Error,
        TEXT("workspace_committed_selection_missing"), TEXT("The committed model and its binding must be selected before publication."));
    FAuthoringModelPtr OutputModel = Selection.Model;
    if (!Prepared.Publication.bDirectDataflow)
        return FailWorkspaceDataflowHandoff(Error, TEXT("committed.plan"), TEXT("direct destination publication"),
            Destination, TEXT("legacy writer selected"), GetDirectWorkspaceSchemaId(), TEXT("publication request dispatch"));
    Prepared.SourcePublication = Selection.SourcePublication;
    Prepared.SourcePublisherOwnerId = Selection.PublisherOwnerId;
	Prepared.PublishedRows = Selection.PublishedRows;
    Prepared.SourceThroughSequence = Selection.SourceThroughSequence;
    Prepared.ActualChanges = Selection.ActualChanges;
    Prepared.ActualExportMode = TEXT("committed");
    const FString AuthorRoot = Prepared.AcceptedWorkspaceRoot.IsEmpty() ? Destination : Prepared.AcceptedWorkspaceRoot;
    if (Selection.AppliedRecords == 0 && !Selection.ActualChanges && Selection.ConsumerStateUpgradeDocuments.IsSet()) {
        // Reuse the original header/publisher transaction. No empty actual
        // delta, layout, value model or public body is manufactured for migration.
        if (!TryBuildCommittedPublicationHeader(Selection.Binding.GetValue(), Selection.TargetKind,
            Prepared, Resources, Prepared.Plan, Error)) return false;
        Prepared.PublisherOwnerId = Selection.PublisherOwnerId;
        TSet<FString> Pending = Selection.ConsumerStateUpgradeDocuments.GetValue();
        if (!TryCollectUnconsumedWorkspaceFileCandidates(AuthorRoot, Pending, Error)) return false;
        return MakeState(Selection.TargetKind, Selection.Target, *OutputModel,
            Selection.Binding.GetValue(), Selection.ThroughSequence, Prepared.ConsumerState, Error,
            nullptr, &Prepared.Publication, &Pending);
    }
    if (const auto Source = GetAcceptedSemanticWorkspace(AuthorRoot)) {
        auto Remaining = Source.ToSharedRef();
        TMap<FString, FSemanticUnit> AcceptedDesired;
        if (!TryResolveUnconsumedAuthoring(AuthorRoot, Remaining, Resources, Remaining, Error, &AcceptedDesired)) return false;
        {
            if (!Remaining->GetEditedUnitIds().IsSet()) return PendingAuthoring(Error,
                TEXT("workspace_committed_author_baseline_unavailable"),
                TEXT("The legacy source has no consumed author baseline. Its input and commit remain pending; no unrelated current model was substituted."));
            if (!Remaining->GetStructuralEdits().IsEmpty()) return PendingAuthoring(Error,
                TEXT("workspace_committed_author_structure_conflict"),
                TEXT("New structural author input remains pending in this publication. Its native operations were not replayed; retain the author input before resolving this whole commit."));
            for (const auto& Id : *Remaining->GetEditedUnitIds()) {
                const auto* Authored = Remaining->GetModel()->FindUnit(Id);
                bool bKeyed = false;
                if (Authored) if (const auto* Roots = Remaining->GetResolvedKeyedOperationRoots().Find(Id))
                    for (const auto& Root : *Roots) {
                        FKeyedItemProjectionContract Contract;
                        if (!TryGetKeyedItemProjectionContract(Root, Contract) || !Contract.bEmitOperationRecords) continue;
                        const auto* Rows = Authored->FindValue(Root);
                        if (!Rows || (*Rows)->GetKind() != StateSemantic::EUeAgentValueKind::Object) continue;
                        bKeyed = true;
                        for (const auto& Row : (*Rows)->GetObjectItems()) {
                            const auto* Node = Remaining->FindValueNode(Id, TEXT("/") + EscapeJsonPointerSegment(Root) + TEXT("/") + EscapeJsonPointerSegment(Row.Key));
                            StateSemantic::FUeAgentCanonicalValuePtr Value;
                            if (!Node || !TryProjectHjsonCanonicalValue(*Node, Resources, Value, Error, false))
                                return PendingAuthoring(Error, TEXT("workspace_committed_author_location_missing"), TEXT("A pending keyed record has no original parser node. Its authored input and commit remain available."));
                            if (Value->GetKind() == StateSemantic::EUeAgentValueKind::Object && Row.Value->GetKind() == StateSemantic::EUeAgentValueKind::Object
                                && !Value->GetObjectItems().ContainsByPredicate([](const auto& Field) { return Field.Key == TEXT("operation"); }))
                                if (const auto* Operation = Row.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("operation"); })) {
                                    auto Fields = Value->GetObjectItems(); Fields.Add(*Operation);
                                    StateSemantic::FUeAgentStateSemanticError Failure;
                                    if (!StateSemantic::FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Value, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                                }
                            Prepared.PendingKeyedAuthorRows.FindOrAdd(Id).FindOrAdd(Root).Add(Row.Key, Value.ToSharedRef());
                        }
                    }
                if (bKeyed) {
                    // Parent metadata is separate from its K operation rows.
                    // Read only the original parent's non-member fields.
                    TOptional<FSemanticUnit> BeforeMetadata;
                    const auto* Roots = Remaining->GetResolvedKeyedOperationRoots().Find(Id);
                    const FString RootField = Roots && !Roots->IsEmpty() ? *Roots->CreateConstIterator() : FString();
                    if (const auto* Accepted = AcceptedDesired.Find(Id)) BeforeMetadata = *Accepted;
                    else if (!RootField.IsEmpty() && !Source->TryReadBaselineMembers(Id, RootField, {}, Resources, BeforeMetadata, Error)) return false;
                    const auto* Raw = Source->GetModel()->FindUnit(Id);
                    if (BeforeMetadata.IsSet() && Raw) for (const auto& Field : Authored->GetValues()) {
                        if (Roots && Roots->Contains(Field.Key)) continue;
                        const auto* RawField = Raw->FindValue(Field.Key);
                        const auto* BeforeField = BeforeMetadata->FindValue(Field.Key);
                        if (!RawField || !BeforeField) continue;
                        StateSemantic::FUeAgentSemanticComparison IsRaw, WasChanged;
                        StateSemantic::FUeAgentStateSemanticError Failure;
                        if (!StateSemantic::TryCompareValues(*RawField, Field.Value, TEXT("/"), Resources, IsRaw, Failure)
                            || !StateSemantic::TryCompareValues(*BeforeField, *RawField, TEXT("/"), Resources, WasChanged, Failure))
                        { Error.SetFailure(MoveTemp(Failure)); return false; }
                        if (!IsRaw.IsEquivalent() || WasChanged.IsEquivalent()) continue;
                        const auto* Node = Remaining->FindValueNode(Id, TEXT("/") + EscapeJsonPointerSegment(Field.Key));
                        StateSemantic::FUeAgentCanonicalValuePtr Value;
                        if (!Node || !TryProjectHjsonCanonicalValue(*Node, Resources, Value, Error, false))
                            return PendingAuthoring(Error, TEXT("workspace_committed_author_location_missing"), TEXT("A pending parent field has no original parser node."));
                        Prepared.PendingKeyedAuthorRows.FindOrAdd(Id).FindOrAdd(FString()).Add(Field.Key, Value.ToSharedRef());
                    }
                    if (!Prepared.PendingKeyedAuthorRows.Contains(Id)) return PendingAuthoring(Error,
                        TEXT("workspace_committed_author_projection_missing"), TEXT("The newer keyed parent edit produced no pending public field. Its current bytes and the whole commit's consumption position were retained."));
                    continue;
                }
                const auto* Actual = OutputModel->FindUnit(Id);
                const auto* Before = Remaining->FindBaselineUnit(Id);
                if (!Authored || !Actual || !Before) return PendingAuthoring(Error,
                    TEXT("workspace_committed_author_structure_conflict"),
                    TEXT("A newer author edit has no matching published/actual field. No authored bytes or consumption frontier were changed."));
                TOptional<FSemanticUnit> Merged;
                if (!Composition::HjsonImport::TryMergeAcceptedAuthoringValues(Remaining, OutputModel.ToSharedRef(), *Before,
                    *Actual, *Authored, Resources, Merged, Error)) return false;
                bool bSame = false;
                if (!TryAreSemanticUnitsEquivalent(Merged.GetValue(), *Actual, Resources, bSame, Error, OutputModel.Get())) return false;
                if (!bSame) Prepared.PendingAuthorUnits.Add(Id, Merged.GetValue());
            }
        }
    }
    UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
    if (!TryExportCapturedCompositionWorkspace(Selection.TargetKind, OutputModel->GetProfileId(),
        OutputModel.ToSharedRef(), Selection.Binding.GetValue(), Destination, Resources, Result, &Prepared)) {
        Error.SetFailure(Result.Diagnostic); return false;
    }
    TSet<FString> PendingDocuments;
    if (!TryPreservePendingAuthoringValues(Prepared, OutputModel.ToSharedRef(), Resources, Error, PendingDocuments)
        || !TryCollectUnconsumedWorkspaceFileCandidates(AuthorRoot, PendingDocuments, Error)) return false;
    return MakeState(Selection.TargetKind, Selection.Target, *OutputModel,
        Selection.Binding.GetValue(), Selection.ThroughSequence, Prepared.ConsumerState, Error,
        nullptr, &Prepared.Publication, &PendingDocuments);
}

bool TryBuildCommittedExportBaseline(const FResolvedSemanticWorkspace& Workspace, FUeAgentResourceTracker& Resources,
    FAuthoringModelPtr& OutModel, FWorkspaceError& Error)
{
    OutModel = FindScopedCommittedBaseline(Workspace, Resources, Error);
    if (OutModel) return true;
    if (Error.HasErrors()) return false;
    if (Workspace.GetSourceKind() == EWorkspaceSourceKind::Directory)
        return FailWorkspaceDataflowHandoff(Error, TEXT("committed.baseline"), TEXT("addressed publication baseline"),
            GetAcceptedSemanticWorkspaceRoot(), TEXT("FindScopedCommittedBaseline returned no view"),
            TEXT("same accepted publication's row/declaration directory"), TEXT("ReadPublishedDirectory / FindScopedCommittedBaseline"));
    const auto UsageBefore = Resources.Snapshot();
    if (!Workspace.GetEditedUnitIds().IsSet())
        return Fail(Error, TEXT("workspace_committed_baseline_unavailable"), TEXT("The workspace has no exact captured edit baseline."));
    const auto& Hashes = Workspace.GetBaselineUnitHashes();
    TArray<FSemanticUnit> Units;
    Units.Reserve(Hashes.Num());
    // The resolver's current model is an import intent projection. In
    // particular an unchanged keyed collection may contain an empty operation
    // set, not the captured collection. Always consume the resolver-verified
    // baseline units, even when there are no authored edits, before exporting.
    // Preserve the resolver/compiler order rather than map iteration order.
    for (const auto& Unit : Workspace.GetModel()->GetUnits()) {
        if (!Hashes.Contains(Unit.GetUnitId())) continue;
        const auto* Baseline = Workspace.FindBaselineUnit(Unit.GetUnitId());
        if (!Baseline) return Fail(Error, TEXT("workspace_committed_baseline_unavailable"),
            TEXT("A captured unit is missing; use a new incremental/full workspace without replaying any committed import."));
        Units.Add(*Baseline);
    }
    if (Units.Num() != Hashes.Num()) return Fail(Error, TEXT("workspace_committed_baseline_unavailable"),
        TEXT("The complete captured membership cannot be reconstructed from this edited workspace."));
    const auto& Model = Workspace.GetModel();
    // Reconstruct content from the verified baseline, but retain the naming
    // candidates accepted from this destination workspace. The compiler still
    // validates contract, target identity and each definition's current content.
    const auto ProjectionHints = GetAcceptedDefinitionProjectionHints();
    // The directory resolver requires the old digest for every /2-/3
    // manifest; /4 deliberately has no such field. This is a format edge,
    // independent of an optional last Model or generated-source cache hit.
    const bool bLegacyFingerprint = Workspace.GetSourceKind() != EWorkspaceSourceKind::Directory
        || !Workspace.GetPublishedSemanticFingerprint().IsEmpty();
    if (!TryCompileAuthoringModel(Model->GetContractId(), Model->GetContractVersion(), Model->GetProfileId(),
        Model->GetBaseRevision(), MoveTemp(Units), Model->GetTargetUnitIds(), Resources, OutModel, Error,
        ProjectionHints.Get(), &Model.Get(), nullptr, bLegacyFingerprint)) return false;
    TMap<ContextResource::EUeAgentResourceDimension, int64> Charges;
    const auto UsageAfter = Resources.Snapshot();
    for (uint8 D = 0; D <= static_cast<uint8>(ContextResource::EUeAgentResourceDimension::EstimatedMemoryBytes); ++D) {
        const auto Dimension = static_cast<ContextResource::EUeAgentResourceDimension>(D);
        const int64 Delta = UsageAfter.Consumed(Dimension) - UsageBefore.Consumed(Dimension);
        if (Delta > 0) Charges.Add(Dimension, Delta);
    }
    RememberScopedCommittedBaseline(Workspace, OutModel, MoveTemp(Charges));
    return true;
}

bool TryMakeCommittedExportState(const FString& TargetKind, const FString& Target, const FAuthoringModel& Model,
    const FCompositionSnapshotBinding& Binding, const FCommittedExportFence& CaptureFence, FString& OutState, FWorkspaceError& Error,
    const FWorkspacePublication* Publication)
{
    TArray<FDurableConsumerRecord> Records;
    if (!ReadRecords(Model, Records, Error, EDurableConsumerBody::Metadata)) return false;
    int64 Through = 0;
    for (const auto& Record : Records) {
        if (Record.Phase != TEXT("committed") && !IsDiscarded(Record.Phase)) break;
        if (Record.Phase == TEXT("committed")) {
            // A later commit is outside this capture even when its local
            // sequence is lower than an unrelated target's old maximum.
            if (!CaptureFence.CompletedRequestKeys.Contains(Record.RequestKey)) break;
            Through = Record.Sequence;
        }
    }
    const TSet<FString> PendingDocuments; // This explicit current capture publishes actual values.
    return MakeState(TargetKind, Target, Model, Binding, Through, OutState, Error, nullptr, Publication, &PendingDocuments);
}

bool TryReadPublishedPendingAuthorDocuments(const FString& State,
    const FWorkspacePublication& Publication, TSet<FString>& OutDocuments)
{
    OutDocuments.Reset();
    if (!Publication.bDirectDataflow || State.IsEmpty()) return false;
    TSharedPtr<FJsonObject> Json;
    const auto Reader = TJsonReaderFactory<TCHAR>::Create(State);
    return FJsonSerializer::Deserialize(Reader, Json) && Json
        && ReadPendingAuthorDocuments(*Json, Publication, OutDocuments);
}

bool TryCaptureCommittedExportFence(TSharedPtr<const FCommittedExportFence>& OutFence, FWorkspaceError& Error)
{
    auto Fence = MakeShared<FCommittedExportFence>();
    CaptureDurableCompletedRequestKeys(UeAgentAuthoringOperationApply::GetAuthoringDurableJournalRoot(), Fence->CompletedRequestKeys);
    OutFence = MoveTemp(Fence);
    return true;
}

bool TryFindAppliedCommittedSource(const FString& Root, const FResolvedSemanticWorkspace& Source, bool& bApplied, FWorkspaceError& Error)
{
    bApplied = false;
    if (const auto Accepted = GetAcceptedSemanticWorkspace(Root); Accepted && Accepted.Get() == &Source) {
        auto Remaining = Accepted.ToSharedRef();
        auto Resources = MakeSemanticWorkspaceResourceTracker();
        if (!TryResolveUnconsumedAuthoring(Root, Remaining, Resources, Remaining, Error)) return false;
        bApplied = Remaining->HasResolvedAcceptedAuthoring() && Remaining->GetEditedUnitIds().IsSet()
            && Remaining->GetEditedUnitIds()->IsEmpty() && Remaining->GetStructuralEdits().IsEmpty();
        return true;
    }
    return FailWorkspaceDataflowHandoff(Error, TEXT("committed.applied_source"),
        TEXT("same-request accepted workspace binding"), Root,
        TEXT("source is not the active accepted workspace"), TEXT("accepted typed source / actual record"),
        TEXT("SourceLoader successful source handoff"));
}

bool TryPrepareCommittedAuthoringWriteback(const FString& Root, const FResolvedSemanticWorkspaceRef& Source, FWorkspaceError& Error)
{
    auto Remaining = Source;
    auto Resources = MakeSemanticWorkspaceResourceTracker();
    // Current writer preserves unconsumed typed author fields separately from
    // actual UE output. An exact whole-input fingerprint is no longer its
    // eligibility proof. Old records lacking that separation remain explicit.
    return TryResolveUnconsumedAuthoring(Root, Source, Resources, Remaining, Error);
}

bool TryResolveUnconsumedAuthoring(const FString& Root, const FResolvedSemanticWorkspaceRef& Source,
    FUeAgentResourceTracker& Resources, FResolvedSemanticWorkspaceRef& Out, FWorkspaceError& Error,
    TMap<FString, FSemanticUnit>* OutAcceptedDesired)
{
    Out = Source;
    if (OutAcceptedDesired) OutAcceptedDesired->Reset();
    if (Root.IsEmpty() || Source->HasResolvedAcceptedAuthoring() || Source->GetSourceKind() != EWorkspaceSourceKind::Directory
        || !Source->GetEditedUnitIds().IsSet()) return true;
    FString State;
    ArtifactPublisher::FArtifactPublishError PublishError;
    if (!ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), State, PublishError))
    { Error.SetFailure(MoveTemp(PublishError)); return false; }
    int64 Through = 0;
    TSharedPtr<FJsonObject> StateJson;
    if (!State.IsEmpty() && (!ReadJson(State, StateJson, Error) || !ReadSequence(*StateJson, TEXT("through_sequence"), Through, Error))) return false;
    TArray<FDurableConsumerRecord> Records;
    if (!ReadRecords(*Source->GetModel(), Records, Error, EDurableConsumerBody::Complete, Through)) return false;
    FString NormalRoot = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(NormalRoot);
    TMap<FString, FAcceptedFact> Facts;
    TMap<FString, TSharedPtr<FJsonObject>> StructuralFacts;
    bool bFound = false;
    const auto Pending = [&](const TCHAR* Code, const TCHAR* Reason) {
        auto Notice = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_committed_export"));
        Notice.Severity = Contract::ESeverity::Warning; Notice.AssetApplied = true; Notice.RetryImport = false;
        Notice.RecoveryAction = TEXT("inspect_existing_transaction_without_replaying_import");
        Error.SetFailure(MoveTemp(Notice)); return false;
    };
    const auto MergeFact = [&](const FAcceptedFact& Incoming) {
        if (!Incoming.Desired.IsSet()) return Fail(Error, TEXT("workspace_committed_typed_handoff_missing"), TEXT("The accepted typed input has no desired unit."));
        auto Unit = Incoming.Desired; auto KeyedRoots = Incoming.KeyedRoots;
        auto& Fact = Facts.FindOrAdd(Unit->GetUnitId());
            if (Fact.Desired.IsSet() && !Fact.KeyedRoots.IsEmpty() && !KeyedRoots.IsEmpty()) {
                auto Values = Unit->GetValues();
                for (const auto& Root : Fact.KeyedRoots) {
                    const auto* OldRows = Fact.Desired->FindValue(Root);
                    const auto* NewRows = Values.Find(Root);
                    if (!OldRows || (*OldRows)->GetKind() != StateSemantic::EUeAgentValueKind::Object) continue;
                    TArray<TPair<FString, StateSemantic::FUeAgentCanonicalValueRef>> Combined = (*OldRows)->GetObjectItems();
                    if (NewRows && (*NewRows)->GetKind() == StateSemantic::EUeAgentValueKind::Object)
                        for (const auto& Row : (*NewRows)->GetObjectItems()) {
                            Combined.RemoveAll([&](const auto& Prior) { return Prior.Key == Row.Key; }); Combined.Add(Row);
                        }
                    StateSemantic::FUeAgentCanonicalValuePtr Rows;
                    StateSemantic::FUeAgentStateSemanticError Failure;
                    if (!StateSemantic::FUeAgentCanonicalValue::TryObject(MoveTemp(Combined), Resources, Rows, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                    Values.Add(Root, Rows.ToSharedRef()); KeyedRoots.Add(Root);
                }
                Unit = KeyedAcceptedUnit(Unit.GetValue(), MoveTemp(Values));
            }
            Fact.Desired = MoveTemp(Unit); Fact.KeyedRoots = MoveTemp(KeyedRoots);
            Fact.PublishedBefore = Incoming.PublishedBefore; Fact.Actual = Incoming.Actual;
            Fact.SourceUnit = Incoming.SourceUnit; Fact.SourcePointer = Incoming.SourcePointer;
            return true;
    };
    for (const auto& Record : Records) {
        if (Record.Sequence <= Through || Record.Phase != TEXT("committed")) continue;
        TSharedPtr<FJsonObject> Intent;
        FString IntentRoot;
        if (!ReadJson(Record.IntentJson, Intent, Error)) return false;
        if (!Intent->TryGetStringField(TEXT("workspace_root"), IntentRoot)
            || !IntentRoot.Equals(NormalRoot, ESearchCase::IgnoreCase)) continue;
        const TArray<TSharedPtr<FJsonValue>>* Units = nullptr;
        if (!Intent->TryGetArrayField(TEXT("accepted_units"), Units)) {
            return Pending(TEXT("workspace_committed_accepted_input_unavailable"),
                TEXT("Stage=committed.author_writeback; missing=accepted_units; expected=typed accepted input from the native request's committed writer. "
                    "This older record cannot separate newer author edits. No whole-source fingerprint matcher or native replay was invoked; keep its custody and authored files."));
        }
        bFound = true;
        const TArray<TSharedPtr<FJsonValue>>* Structural = nullptr;
        if (Intent->TryGetArrayField(TEXT("accepted_structural_edits"), Structural)) for (const auto& Value : *Structural) {
            FString Id;
            if (!Value || Value->Type != EJson::Object || !Value->AsObject()->TryGetStringField(TEXT("id"), Id))
                return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("An accepted structural record is invalid."));
            StructuralFacts.Add(Id, Value->AsObject());
        }
        TMap<FString, FSemanticUnit> Actual;
        if (Record.Material && FString(Record.Material->GetMaterialType()) == FCommittedObservationMaterial::Type)
            Actual = StaticCastSharedPtr<const FCommittedObservationMaterial>(Record.Material)->Delta->AcceptedActualUnits;
        else {
            if (Record.bCurrentRequestCommit) return Pending(TEXT("workspace_committed_typed_handoff_missing"),
                TEXT("Stage=committed.author_writeback; missing=current request typed actual; producer=native seal consumer. Native changes are already applied. No journal decoder or native replay was selected; retain the existing custody and author bytes."));
            TSharedPtr<FJsonObject> Verified;
            if (!ReadJson(Record.VerifiedJson, Verified, Error)) return false;
            const TArray<TSharedPtr<FJsonValue>>* Observed = nullptr;
            if (Verified->TryGetArrayField(TEXT("accepted_actual"), Observed)) for (const auto& Item : *Observed) {
                TOptional<FSemanticUnit> Unit;
                if (!Item || !TryDecodeCommittedWorkspaceUnit(*Item, Resources, Unit, Error) || !Unit.IsSet()) return false;
                Actual.Add(Unit->GetUnitId(), Unit.GetValue());
            }
        }
        if (Record.bCurrentRequestCommit) {
            const auto Material = StaticCastSharedPtr<const FCommittedObservationMaterial>(Record.Material);
            if (!Material || !Material->AcceptedInputs) return Pending(TEXT("workspace_committed_typed_handoff_missing"),
                TEXT("The current request did not hand off its accepted typed fields. No journal input decoder was selected."));
            for (const auto& Pair : *Material->AcceptedInputs) {
                auto Incoming = Pair.Value;
                if (const auto* Observed = Actual.Find(Pair.Key)) Incoming.Actual = *Observed;
                if (!MergeFact(Incoming)) return false;
            }
            continue;
        }
        for (const auto& Value : *Units) {
            if (!Value || Value->Type != EJson::Object) return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("An accepted unit record is invalid."));
            const auto Item = Value->AsObject();
            const auto* Desired = Item->Values.Find(TEXT("desired"));
            TOptional<FSemanticUnit> Unit;
            if (!Desired || !TryDecodeCommittedWorkspaceUnit(**Desired, Resources, Unit, Error) || !Unit.IsSet()) return false;
            const TArray<TSharedPtr<FJsonValue>>* Keyed = nullptr;
            TSet<FString> KeyedRoots;
            if (Item->TryGetArrayField(TEXT("keyed_operation_roots"), Keyed)) for (const auto& Root : *Keyed) {
                FString Name;
                if (!Root || !Root->TryGetString(Name)) return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("An accepted keyed root is invalid."));
                KeyedRoots.Add(MoveTemp(Name));
            }
            FAcceptedFact Incoming; Incoming.Desired = MoveTemp(Unit); Incoming.KeyedRoots = MoveTemp(KeyedRoots);
            if (const auto* Before = Item->Values.Find(TEXT("published_before"))) Incoming.PublishedBefore = *Before;
            Item->TryGetStringField(TEXT("source_unit"), Incoming.SourceUnit);
            Item->TryGetStringField(TEXT("source_pointer"), Incoming.SourcePointer);
            if (const auto* Observed = Actual.Find(Incoming.Desired->GetUnitId())) Incoming.Actual = *Observed;
            if (!MergeFact(Incoming)) return false;
        }
    }
    if (!bFound) return true;
    if (OutAcceptedDesired) for (const auto& Pair : Facts) OutAcceptedDesired->Add(Pair.Key, Pair.Value.Desired.GetValue());
    TArray<FString> Remaining = Source->GetEditedUnitIds().GetValue();
    TMap<FString, FSemanticUnit> Replacements, ActualBaselines;
    TMap<FString, FString> ActualHashes;
    for (const auto& Pair : Facts) {
        const auto* Authored = Source->GetModel()->FindUnit(Pair.Key);
        if (!Authored || !Remaining.Contains(Pair.Key)) continue;
        const auto& Fact = Pair.Value;
        if (!Fact.KeyedRoots.IsEmpty()) {
            if (!AreSemanticUnitDeclarationsEquivalent(KeyedAcceptedUnit(*Authored, {}), KeyedAcceptedUnit(Fact.Desired.GetValue(), {})))
                return Pending(TEXT("workspace_committed_author_structure_conflict"), TEXT("The accepted keyed operation's target/type binding changed; its prior native action was not replayed."));
            auto Values = Authored->GetValues();
            bool bPendingRows = false;
            for (const auto& Field : Values) if (!Fact.KeyedRoots.Contains(Field.Key)) {
                const auto* Old = Fact.Desired->FindValue(Field.Key);
                StateSemantic::FUeAgentSemanticComparison Compared;
                StateSemantic::FUeAgentStateSemanticError Failure;
                if (!Old) { bPendingRows = true; continue; }
                if (!StateSemantic::TryCompareValues(*Old, Field.Value, TEXT("/"), Resources, Compared, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                bPendingRows |= !Compared.IsEquivalent();
            }
            if (Fact.Actual.IsSet()) for (const auto& Field : Fact.Actual->GetValues()) {
                if (Fact.KeyedRoots.Contains(Field.Key)) continue;
                const auto* Accepted = Fact.Desired->FindValue(Field.Key);
                const auto* Current = Values.Find(Field.Key);
                if (!Accepted || !Current) continue;
                StateSemantic::FUeAgentSemanticComparison Compared;
                StateSemantic::FUeAgentStateSemanticError Failure;
                if (!StateSemantic::TryCompareValues(*Accepted, *Current, TEXT("/"), Resources, Compared, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                if (Compared.IsEquivalent()) Values.Add(Field.Key, Field.Value);
            }
            for (const auto& Root : Fact.KeyedRoots) {
                const auto* BeforeRows = Fact.Desired->FindValue(Root);
                const auto* Rows = Values.Find(Root);
                if (!BeforeRows || !Rows || (*BeforeRows)->GetKind() != StateSemantic::EUeAgentValueKind::Object
                    || (*Rows)->GetKind() != StateSemantic::EUeAgentValueKind::Object)
                    return Pending(TEXT("workspace_committed_author_structure_conflict"), TEXT("The newer keyed author collection has a different declared shape; both inputs remain available."));
                TArray<TPair<FString, StateSemantic::FUeAgentCanonicalValueRef>> PendingRows;
                for (const auto& Row : (*Rows)->GetObjectItems()) {
                    const auto* Accepted = (*BeforeRows)->GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Row.Key; });
                    if (!Accepted) { PendingRows.Add(Row); continue; }
                    StateSemantic::FUeAgentSemanticComparison Compared;
                    StateSemantic::FUeAgentStateSemanticError Failure;
                    if (!StateSemantic::TryCompareValues(Accepted->Value, Row.Value, TEXT("/"), Resources, Compared, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                    if (!Compared.IsEquivalent()) return Pending(TEXT("workspace_committed_author_structure_conflict"),
                        TEXT("A newer author edit changes the same accepted keyed operation. Its actual identity must be reconciled; the prior operation was not replayed."));
                }
                bPendingRows |= !PendingRows.IsEmpty();
                StateSemantic::FUeAgentCanonicalValuePtr Selected;
                StateSemantic::FUeAgentStateSemanticError Failure;
                if (!StateSemantic::FUeAgentCanonicalValue::TryObject(MoveTemp(PendingRows), Resources, Selected, Failure)) { Error.SetFailure(MoveTemp(Failure)); return false; }
                Values.Add(Root, Selected.ToSharedRef());
            }
            if (!bPendingRows) Remaining.Remove(Pair.Key);
            else Replacements.Add(Pair.Key, Authored->WithValues(MoveTemp(Values)));
            continue;
        }
        bool bSame = false;
        if (!TryAreSemanticUnitsEquivalent(*Authored, Fact.Desired.GetValue(), Resources, bSame, Error, &Source->GetModel().Get())) return false;
        if (bSame) {
            Remaining.Remove(Pair.Key); // Accepted intent is never native work twice.
            if (Fact.Actual.IsSet() && Authored->GetUnitKind() == ESemanticUnitKind::PropertySet
                && AreSemanticUnitDeclarationsEquivalent(*Authored, Fact.Actual.GetValue())) Replacements.Add(Pair.Key, Fact.Actual.GetValue());
        } else {
            if (!Fact.Actual.IsSet()) {
                Pending(TEXT("workspace_committed_accepted_actual_unavailable"),
                    TEXT("The prior commit has no observed actual value for this newly edited field. The newer input was retained and native work was not replayed."));
                auto Notice = Error.GetDiagnostic();
                Notice.Source.SourceFile = Fact.SourceUnit; Notice.Source.JsonPath = Fact.SourcePointer;
                if (Fact.PublishedBefore) {
                    TOptional<FSemanticUnit> Before;
                    FWorkspaceError DecodeError;
                    if (TryDecodeCommittedWorkspaceUnit(*Fact.PublishedBefore, Resources, Before, DecodeError) && Before.IsSet()) {
                        const auto Evidence = MakeShared<FJsonObject>();
                        Evidence->SetField(TEXT("published_before"), Fact.PublishedBefore);
                        Notice.ExpectedEvidence = MakeShared<FJsonValueObject>(Evidence);
                    }
                }
                Error.SetFailure(MoveTemp(Notice)); return false;
            }
            TOptional<FSemanticUnit> Merged;
            if (!Composition::HjsonImport::TryMergeAcceptedAuthoringValues(Source, Source->GetModel(), Fact.Desired.GetValue(),
                Fact.Actual.GetValue(), *Authored, Resources, Merged, Error)) return false;
            Replacements.Add(Pair.Key, Merged.GetValue());
        }
        if (Fact.Actual.IsSet()) {
            ActualBaselines.Add(Pair.Key, Fact.Actual.GetValue());
            FString Hash;
            if (!TryHashSemanticUnitRow(Fact.Actual.GetValue(), Hash, Error)) return false;
            ActualHashes.Add(Pair.Key, MoveTemp(Hash));
        }
    }
    const int32 PendingCount = Remaining.Num();
    TArray<FStructuralEdit> RemainingStructural;
    for (const auto& Edit : Source->GetStructuralEdits()) {
        if (const auto* Accepted = StructuralFacts.Find(Edit.GetEditId())) {
            if (StructuralIntentMatches(Edit, **Accepted)) continue;
            return Pending(TEXT("workspace_committed_author_structure_conflict"),
                TEXT("A newer author edit changes the same accepted structural action. Both inputs were retained and the existing native operation was not replayed."));
        }
        RemainingStructural.Add(Edit);
    }
    Out = Source->RebindUnconsumedAuthoring(FAuthoringModel::RebasePropertySourceView(Source->GetModel(),
        Source->GetModel()->GetBaseRevision(), MoveTemp(Replacements)), MoveTemp(Remaining), MoveTemp(ActualBaselines), MoveTemp(ActualHashes), MoveTemp(RemainingStructural));
    UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=accepted_author_handoff accepted_units=%d pending_units=%d complete_model=false published_baseline_unchanged=true"), Facts.Num(), PendingCount);
    return true;
}

bool TryPrepareCommittedImportState(const FString& Root, const FString& TargetKind, const FString& Target,
    const FAuthoringModel& Before, const FCompositionSnapshotBinding& Binding, FUeAgentResourceTracker& Resources,
    FString& OutBootstrapState, FWorkspaceError& Error)
{
    OutBootstrapState.Reset();
    FString Existing;
    ArtifactPublisher::FArtifactPublishError PublishError;
    if (!ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), Existing, PublishError))
    { Error.SetFailure(MoveTemp(PublishError)); return false; }
    if (Existing.IsEmpty()) return FailWorkspaceDataflowHandoff(Error, TEXT("committed.import_state"),
        TEXT("published consumer state"), Root, TEXT("absent"), TEXT("publication-bound through_sequence and target binding"),
        TEXT("the explicit full/initial publication writer"));
    FResolvedSemanticWorkspacePtr Source;
    if (!TryLoadHjsonImportSource(FWorkspaceSource::Directory(Root), ESemanticWorkspaceMode::State, Resources, Source, Error)) return false;
    FAuthoringModelPtr Baseline;
    if (!TryBuildCommittedExportBaseline(*Source, Resources, Baseline, Error)) return false;
    FCommittedExportSelection Selection;
    return TrySelectCommittedWorkspaceExport(Root, Target, Baseline.ToSharedRef(), Source->GetPublishedSemanticFingerprint(), Resources, Selection, Error);
}

bool TrySelectCommittedWorkspaceExport(const FString& Root, const FString& Target, FAuthoringModelRef Baseline,
    const FString& PublishedSemanticFingerprint, FUeAgentResourceTracker& Resources, FCommittedExportSelection& Out, FWorkspaceError& Error)
{
    Out = {};
    Out.Target = Target;
	Out.PublishedRows = Baseline;
    FString State;
    ArtifactPublisher::FArtifactPublishError PublishError;
    if (!ArtifactPublisher::TryReadWorkspaceConsumerState(Root, FString(), State, Out.PublisherOwnerId, PublishError))
    { Error.SetFailure(MoveTemp(PublishError)); return false; }
    if (State.IsEmpty()) return FailWorkspaceDataflowHandoff(Error, TEXT("committed.destination_selection"),
        TEXT("published consumer state"), Root, TEXT("absent"), TEXT("the destination publication's consumer state"),
        TEXT("the explicit full/initial publication writer"));
    TSharedPtr<FJsonObject> Json;
    int64 BodyAfterSequence = 0;
    if (!State.IsEmpty() && (!ReadJson(State, Json, Error)
        || !ReadSequence(*Json, TEXT("through_sequence"), BodyAfterSequence, Error))) return false;
    TArray<FDurableConsumerRecord> Records;
    if (!ReadRecords(*Baseline, Records, Error, EDurableConsumerBody::Complete, BodyAfterSequence)) return false;
    FString Schema, RecordedStream, Profile, Fingerprint;
    const TSharedPtr<FJsonObject>* Binding = nullptr;
    if (!Json && !ReadJson(State, Json, Error)) return false;
    const auto CheckStateField = [&](const TCHAR* Field, FString& Value, const FString& Expected) {
        if (Json->TryGetStringField(Field, Value) && Value.Equals(Expected, ESearchCase::CaseSensitive)) return true;
        auto Diagnostic = Contract::FDiagnostic::Failure(TEXT("workspace_committed_baseline_mismatch"),
            TEXT("The published committed-export watermark does not match this complete workspace baseline."), TEXT("workspace_committed_export"));
        Diagnostic.PropertyPath = Field;
        Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
        Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Value);
        Diagnostic.RecoveryAction = TEXT("export_incremental_or_full_to_new_workspace");
        Error.SetFailure(MoveTemp(Diagnostic));
        return false;
    };
    if (const auto* Publication = FindAcceptedWorkspacePublication(Root)) Out.SourcePublication = *Publication;
    else if (!TryReadWorkspacePublication(Root, Out.SourcePublication, Error)) return false;
    if (!Out.SourcePublication.bDirectDataflow)
        return FailWorkspaceDataflowHandoff(Error, TEXT("committed.destination_selection"),
            TEXT("direct /4 destination publication"), Root, TEXT("legacy publication"), GetDirectWorkspaceSchemaId(),
            TEXT("composition publication writer"));
    if (!CheckStateField(TEXT("schema"), Schema, Out.SourcePublication.bDirectDataflow ? DirectConsumerStateSchema : ConsumerOwner)
        || !CheckStateField(TEXT("stream"), RecordedStream, StreamKey(*Baseline))
        || !CheckStateField(TEXT("profile"), Profile, Baseline->GetProfileId())) return false;
    if (Out.SourcePublication.bDirectDataflow) {
        if (!CheckStateField(TEXT("publication_generation"), Fingerprint, Out.SourcePublication.Generation)) return false;
    } else if (!CheckStateField(TEXT("model_fingerprint"), Fingerprint, PublishedSemanticFingerprint)) return false;
    TSet<FString> PublishedPendingDocuments;
    if (!Out.SourcePublication.ScopedMachineBuckets.IsSet() || !Out.SourcePublication.ExpandedAuthorDocuments.IsSet()
        || !Out.SourcePublication.PublishedUnitCount.IsSet())
        return FailWorkspaceDataflowHandoff(Error, TEXT("committed.destination_selection"),
            TEXT("machine_bucket_organization / published_unit_count / expanded_author_documents"), Root,
            GetDirectWorkspaceSchemaId(), TEXT("same-generation physical organization fields"), TEXT("publication row writer"));
    if (!ReadPendingAuthorDocuments(*Json, Out.SourcePublication, PublishedPendingDocuments))
        return FailWorkspaceDataflowHandoff(Error, TEXT("committed.destination_selection"),
            TEXT("pending_author_documents"), Root, TEXT("missing, unknown or generation mismatch"),
            TEXT("same-generation known pending document set (including known empty)"), TEXT("publication consumer-state writer"));
    if (!Json->TryGetStringField(TEXT("target_kind"), Out.TargetKind)
        || !Json->TryGetObjectField(TEXT("binding"), Binding))
        return Fail(Error, TEXT("workspace_committed_record_invalid"), TEXT("The committed export target or snapshot binding is missing."));
    if (!ReadSequence(*Json, TEXT("through_sequence"), Out.ThroughSequence, Error)
        || !ReadBinding(**Binding, Out.Binding, Error)) return false;
    Out.SourceThroughSequence = Out.ThroughSequence;
    if (!Baseline->GetBaseRevision().Equals(Out.Binding->Revision.GetGeneration(), ESearchCase::CaseSensitive)
        || !Baseline->FindDeclaration(Baseline->GetTargetUnitIds()[0])->GetSemanticTypeRef().Equals(Out.TargetKind, ESearchCase::CaseSensitive))
        return Fail(Error, TEXT("workspace_committed_baseline_mismatch"), TEXT("The workspace revision or target type differs from its published binding."));
    if (!HasCommittedExportConsumer(Out.TargetKind)) return Fail(Error, TEXT("workspace_committed_domain_unsupported"),
        TEXT("This domain does not yet provide committed-change evidence. Choose explicit incremental or full export; no external UE changes were consumed."));
    if (Out.ThroughSequence > 0 && !Records.ContainsByPredicate([&](const auto& R) { return R.Sequence == Out.ThroughSequence; }))
        return Fail(Error, TEXT("workspace_committed_history_missing"), TEXT("The workspace watermark refers to durable history that is no longer available; synchronize explicitly with incremental/full export."));
    Out.Model = Baseline;
    const auto Pending = [&](const FString& Reason, const TCHAR* Recovery) {
        auto Notice = Contract::FDiagnostic::Failure(TEXT("workspace_committed_observation_pending"), Reason, TEXT("workspace_committed_export"));
        Notice.Severity = Contract::ESeverity::Warning; Notice.RetryImport = false;
        Notice.RecoveryAction = Recovery; Error.SetFailure(MoveTemp(Notice)); return false;
    };
    TSet<int64> OrderedSequences;
    TArray<TSharedPtr<const FCommittedWorkspaceDelta>> SelectedChanges;
    for (const auto& Record : Records) if (Record.Sequence > Out.ThroughSequence && Record.Phase == TEXT("committed")) {
        if (OrderedSequences.Contains(Record.Sequence)) return Pending(
            TEXT("Retained commits for this stream have ambiguous ordering. Native work was not replayed and the workspace watermark was not advanced."),
            TEXT("synchronize_incremental_or_full_without_replaying_import"));
        OrderedSequences.Add(Record.Sequence);
    }
    for (const auto& Record : Records) {
        if (Record.Sequence <= Out.ThroughSequence || IsDiscarded(Record.Phase)) continue;
        if (Record.Phase != TEXT("committed")) return Pending(
            TEXT("This stream has unresolved durable evidence. Native work is not replayed and its workspace watermark remains unchanged; inspect the existing recovery owner."),
            TEXT("inspect_existing_transaction_without_replaying_import"));
        if (Record.Material && FString(Record.Material->GetMaterialType()) == FCommittedObservationMaterial::Type) {
            const auto Actual = StaticCastSharedPtr<const FCommittedObservationMaterial>(Record.Material);
            SelectedChanges.Add(Actual->Delta);
            Out.Binding = Actual->Binding; Out.ThroughSequence = Record.Sequence;
            Out.ObservationScopes.AddUnique(Actual->Scope); ++Out.AppliedRecords;
            UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=committed_typed_handoff changed_units=%d decode_rows=0"), Actual->Delta->Changes.Num());
            continue;
        }
        TSharedPtr<FJsonObject> Verified;
        const TSharedPtr<FJsonObject>* Delta = nullptr;
        const TSharedPtr<FJsonObject>* ActualBinding = nullptr;
        if (!ReadJson(Record.VerifiedJson, Verified, Error)) return false;
        FString ObservationStatus;
        if (Verified->TryGetStringField(TEXT("observation_status"), ObservationStatus) && ObservationStatus == TEXT("pending")) {
            auto Notice = Contract::FDiagnostic::Failure(TEXT("workspace_committed_observation_pending"),
                TEXT("The native import completed, but its observed HJSON difference is incomplete. The workspace watermark was not advanced; do not replay the import."), TEXT("workspace_committed_export"));
            Notice.Severity = Contract::ESeverity::Warning;
            Notice.AssetApplied = true; Notice.RetryImport = false;
            Notice.RecoveryAction = TEXT("synchronize_incremental_or_full_without_replaying_import");
            Error.SetFailure(MoveTemp(Notice));
            return false;
        }
        if (Record.bCurrentRequestCommit) {
            auto Notice = Contract::FDiagnostic::Failure(TEXT("workspace_committed_typed_handoff_missing"),
                TEXT("The current request committed native state but its sealed typed result did not reach refresh. The original custody is retained; no journal-to-typed replacement algorithm or native replay was selected."), TEXT("workspace_committed_export"));
            Notice.AssetApplied = true; Notice.RetryImport = false;
            Notice.RecoveryAction = TEXT("refresh_existing_committed_request_without_native_replay");
            Error.SetFailure(MoveTemp(Notice)); return false;
        }
        if (!Verified->TryGetObjectField(TEXT("delta"), Delta)
            || !Verified->TryGetObjectField(TEXT("binding"), ActualBinding)
            || !ReadBinding(**ActualBinding, Out.Binding, Error))
            return Fail(Error, TEXT("workspace_committed_evidence_unavailable"), TEXT("The committed transaction has no complete verified export difference; synchronize with incremental or full export without replaying it."));
        auto Typed = MakeShared<FCommittedWorkspaceDelta>();
        if (!TryReadCommittedWorkspaceDelta(**Delta, Resources, *Typed, Error)) return false;
        SelectedChanges.Add(MoveTemp(Typed));
        Out.ThroughSequence = Record.Sequence;
        FString Scope;
        if (Verified->TryGetStringField(TEXT("observation_scope"), Scope)) Out.ObservationScopes.AddUnique(Scope);
        ++Out.AppliedRecords;
    }
    if (!SelectedChanges.IsEmpty()) {
        auto Net = MakeShared<FCommittedWorkspaceDelta>();
        if (!TryFoldCommittedWorkspaceDeltas(Baseline.Get(), SelectedChanges, Resources, *Net, Error)) return false;
        Out.ActualChanges = Net;
		if (!FAuthoringModel::TryMakeCommittedPublicationView(Baseline, *Net, Out.Model, Error)) return false;
        UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=committed_destination_fold records=%d net_units=%d intermediate_models=0 compatibility_model_materialized=%s"),
            SelectedChanges.Num(), Net->Changes.Num(), TEXT("false"));
    }
    return true; // State is produced once, beside the chosen output publication.
}

FCommittedWorkspaceTransaction::FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target,
    const FString& Root, FString AcceptedSourceFingerprint, FString BootstrapState, FAuthoringModelRef Before, FCaptureAfter CaptureAfter,
    FResolvedSemanticWorkspacePtr AcceptedSource)
{
    Initialize(TargetKind, Root, MoveTemp(AcceptedSourceFingerprint), MoveTemp(BootstrapState), Before, nullptr,
        [Before, AcceptedSource, CaptureAfter = MoveTemp(CaptureAfter)](TSharedPtr<FJsonObject>& Delta, TSharedPtr<const FCommittedWorkspaceDelta>& Typed,
            TOptional<FCompositionSnapshotBinding>& Binding, FString& Scope, FWorkspaceError& Error) {
            FAuthoringModelPtr After;
            if (!CaptureAfter(After, Binding, Scope, Error) || !After) return false;
            auto Resources = MakeSemanticWorkspaceResourceTracker();
            if (!TryBuildCommittedWorkspaceDelta(*Before, *After, Resources, Delta, Error, &Typed)) return false;
            AttachAcceptedActualUnits(AcceptedSource.Get(), *After, Typed);
            return true;
        }, {}, AcceptedSource);
}

FCommittedWorkspaceTransaction::FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target,
    const FString& Root, FString AcceptedSourceFingerprint, FAuthoringModelRef SourceBindings, FResolvedSemanticWorkspacePtr AcceptedSource)
{
    struct FObserved {
        int32 Pairs = 0;
        UeAgentAuthoringState::FStateSnapshot Before, Actual;
    };
    const auto Observed = MakeShared<FObserved>();
    Initialize(TargetKind, Root, MoveTemp(AcceptedSourceFingerprint), FString(), SourceBindings, nullptr,
        [Observed, TargetKind, SourceBindings, AcceptedSource](TSharedPtr<FJsonObject>& Delta, TSharedPtr<const FCommittedWorkspaceDelta>& Typed, TOptional<FCompositionSnapshotBinding>& Binding,
            FString& Scope, FWorkspaceError& Error) {
            if (!Observed->Pairs) return false;
            auto Resources = MakeSemanticWorkspaceResourceTracker();
            StateSemantic::FUeAgentCanonicalSnapshotPtr BeforeCanonical, ActualCanonical;
            FCompositionUnitSet BeforeUnits, ActualUnits;
            if (!ComposeObservedState(TargetKind, *SourceBindings, Observed->Before, Resources, BeforeCanonical, BeforeUnits, Error)
                || !ComposeObservedState(TargetKind, *SourceBindings, Observed->Actual, Resources, ActualCanonical, ActualUnits, Error)
                || !TryBuildCommittedWorkspaceDelta(BeforeUnits, ActualUnits, Resources, Delta, Error, &Typed)) return false;
            if (AcceptedSource && AcceptedSource->GetEditedUnitIds().IsSet()) {
                auto WithActual = MakeShared<FCommittedWorkspaceDelta>(*Typed);
                for (const auto& Unit : ActualUnits.Units)
                    if (AcceptedSource->GetEditedUnitIds()->Contains(Unit.GetUnitId())) WithActual->AcceptedActualUnits.Add(Unit.GetUnitId(), Unit);
                Typed = MoveTemp(WithActual);
            }
            Binding.Emplace(FCompositionSnapshotBinding{ActualCanonical->GetSnapshotId(), ActualCanonical->GetRevision(),
                ActualCanonical->GetProvenance().GetEvidenceId()});
            Scope = TEXT("native_operation_actual_before_after");
            UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=committed_native_observation pairs=%d before_entries=%d actual_entries=%d changed_units=%d pre_native_models=false observation_models=false"),
                Observed->Pairs, Observed->Before.Num(), Observed->Actual.Num(), Delta->GetArrayField(TEXT("changes")).Num());
            return true;
        },
        [Observed, SourceBindings](const UeAgentAuthoringState::FStateSnapshot& Before,
            const UeAgentAuthoringState::FStateSnapshot& Actual) {
            const auto SelectIdentity = [&](const UeAgentAuthoringState::FStateEntry& Entry,
                const UeAgentAuthoringState::FStateNativeBinding& Native) -> const UeAgentAuthoringIdentity::FCanonicalIdentity* {
                if (Entry.SemanticId != TEXT("core.property") || !Native.NativeTargetIdentity.IsValid()) return nullptr;
                const auto HasBinding = [&](const FString& Key) {
                    return SourceBindings->FindUnitsForTargetIdentity(Key).ContainsByPredicate([&](const auto* Unit) {
                        return Unit->GetSemanticTypeRef() == Entry.SemanticId;
                    });
                };
                // Native operations use a stable scratch/live verification key.
                // Export may use the actual object identity. Consume that
                // already captured public identity when it is the source binding.
                return !HasBinding(Entry.TargetIdentity.CanonicalKey) && HasBinding(Native.NativeTargetIdentity.CanonicalKey)
                    ? &Native.NativeTargetIdentity : nullptr;
            };
            ++Observed->Pairs;
            Observed->Before = Observed->Before.MergeAuthoringObservations(Before.MakeAuthoringObservation(SelectIdentity), false);
            Observed->Actual = Observed->Actual.MergeAuthoringObservations(Actual.MakeAuthoringObservation(SelectIdentity), true);
        }, AcceptedSource);
}

FCommittedWorkspaceTransaction::FCommittedWorkspaceTransaction(const FString& TargetKind, const FString& Target,
    const FString& Root, FString AcceptedSourceFingerprint, FString BootstrapState, FAuthoringModelRef PublishedBaseline,
    const FCompositionSnapshotBinding& BeforeObservation, FCaptureDelta CaptureDelta, FResolvedSemanticWorkspacePtr AcceptedSource)
{
    Initialize(TargetKind, Root, MoveTemp(AcceptedSourceFingerprint), MoveTemp(BootstrapState),
        PublishedBaseline, &BeforeObservation, MoveTemp(CaptureDelta), {}, AcceptedSource);
}

void FCommittedWorkspaceTransaction::Initialize(const FString& TargetKind, const FString& Root,
    FString AcceptedSourceFingerprint, FString BootstrapState, FAuthoringModelRef StreamModel,
    const FCompositionSnapshotBinding* BeforeObservation, FCaptureDelta CaptureDelta, FScopedDurableConsumer::FObserve Observe,
    FResolvedSemanticWorkspacePtr AcceptedSource)
{
    const auto Intent = MakeShared<FJsonObject>();
    Intent->SetStringField(TEXT("target_kind"), TargetKind);
    Intent->SetStringField(TEXT("profile"), StreamModel->GetProfileId());
    if (BeforeObservation) Intent->SetObjectField(TEXT("before_observation"), BindingJson(*BeforeObservation));
    FString NormalRoot = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(NormalRoot);
    Intent->SetStringField(TEXT("workspace_root"), NormalRoot);
    Intent->SetStringField(TEXT("accepted_source_fingerprint"), MoveTemp(AcceptedSourceFingerprint));
    Intent->SetStringField(TEXT("bootstrap_state"), MoveTemp(BootstrapState));
    auto Resources = MakeSemanticWorkspaceResourceTracker();
    const auto AcceptedInputs = MakeShared<FAcceptedFacts>();
    if (AcceptedSource) {
        const auto RawAcceptedSource = AcceptedSource;
        auto Effective = AcceptedSource.ToSharedRef();
        if (!TryResolveUnconsumedAuthoring(Root, Effective, Resources, Effective, PreparationError)) return;
        AcceptedSource = Effective;
        TArray<TSharedPtr<FJsonValue>> Units;
        if (AcceptedSource->GetEditedUnitIds().IsSet()) for (const auto& Id : *AcceptedSource->GetEditedUnitIds()) {
            // The applied draft may contain merged current/normalization
            // facts. They were not additional author edits. Keep the exact
            // admitted author value separately from those native facts.
            const auto* Desired = RawAcceptedSource->GetModel()->FindUnit(Id);
            if (!Desired) continue; // Omission is not an authored removal.
            const auto Item = MakeShared<FJsonObject>();
            TSharedPtr<FJsonValue> Encoded;
            TOptional<FSemanticUnit> KeyedDesired;
            if (const auto* Roots = AcceptedSource->GetResolvedKeyedOperationRoots().Find(Id); Roots && !Roots->IsEmpty()) {
                auto Values = Desired->GetValues();
                const auto* EffectiveUnit = AcceptedSource->GetModel()->FindUnit(Id);
                TArray<TSharedPtr<FJsonValue>> Names;
                for (const auto& Root : *Roots) {
                    FKeyedItemProjectionContract Contract;
                    if (!TryGetKeyedItemProjectionContract(Root, Contract) || !Contract.bEmitOperationRecords) continue;
                    Names.Add(MakeShared<FJsonValueString>(Root));
                    const auto* RawRows = Values.Find(Root);
                    const auto* Selected = EffectiveUnit ? EffectiveUnit->FindValue(Root) : nullptr;
                    if (!RawRows || !Selected || (*RawRows)->GetKind() != StateSemantic::EUeAgentValueKind::Object || (*Selected)->GetKind() != StateSemantic::EUeAgentValueKind::Object) continue;
                    TArray<TPair<FString, StateSemantic::FUeAgentCanonicalValueRef>> Rows;
                    for (const auto& Row : (*RawRows)->GetObjectItems())
                        if ((*Selected)->GetObjectItems().ContainsByPredicate([&](const auto& Key) { return Key.Key == Row.Key; })) Rows.Add(Row);
                    StateSemantic::FUeAgentCanonicalValuePtr SelectedRows;
                    StateSemantic::FUeAgentStateSemanticError Failure;
                    if (!StateSemantic::FUeAgentCanonicalValue::TryObject(MoveTemp(Rows), Resources, SelectedRows, Failure)) { PreparationError.SetFailure(MoveTemp(Failure)); return; }
                    Values.Add(Root, SelectedRows.ToSharedRef());
                }
                if (!Names.IsEmpty()) {
                    Item->SetArrayField(TEXT("keyed_operation_roots"), MoveTemp(Names));
                    KeyedDesired = KeyedAcceptedUnit(*Desired, MoveTemp(Values));
                    Desired = &KeyedDesired.GetValue();
                }
            }
            if (!TryEncodeCommittedWorkspaceUnit(*Desired, Resources, Encoded, PreparationError)) return;
            Item->SetField(TEXT("desired"), Encoded);
            if (Desired->GetUnitKind() == ESemanticUnitKind::PropertySet) if (const auto* Before = RawAcceptedSource->FindBaselineUnit(Id)) {
                if (!TryEncodeCommittedWorkspaceUnit(*Before, Resources, Encoded, PreparationError)) return;
                Item->SetField(TEXT("published_before"), Encoded);
            }
            if (const auto* Source = RawAcceptedSource->FindUnitSource(Id)) {
                Item->SetStringField(TEXT("source_unit"), Source->GetSourceUnit());
                Item->SetStringField(TEXT("source_pointer"), Source->GetJsonPointer());
            }
            auto& Fact = AcceptedInputs->FindOrAdd(Desired->GetUnitId()); Fact.Desired = *Desired;
            if (const auto* Before = Item->Values.Find(TEXT("published_before"))) Fact.PublishedBefore = *Before;
            Item->TryGetStringField(TEXT("source_unit"), Fact.SourceUnit); Item->TryGetStringField(TEXT("source_pointer"), Fact.SourcePointer);
            const TArray<TSharedPtr<FJsonValue>>* SavedRoots = nullptr;
            if (Item->TryGetArrayField(TEXT("keyed_operation_roots"), SavedRoots)) for (const auto& Saved : *SavedRoots) Fact.KeyedRoots.Add(Saved->AsString());
            Units.Add(MakeShared<FJsonValueObject>(Item));
        }
        Intent->SetArrayField(TEXT("accepted_units"), MoveTemp(Units));
        TArray<TSharedPtr<FJsonValue>> Structural;
        for (const auto& Edit : AcceptedSource->GetStructuralEdits()) Structural.Add(MakeShared<FJsonValueObject>(StructuralIntentJson(Edit)));
        Intent->SetArrayField(TEXT("accepted_structural_edits"), MoveTemp(Structural));
    }
    FString IntentJson;
    FWorkspaceError IntentError;
    WriteJson(Intent, IntentJson, IntentError); // Pure strings; serialization cannot introduce native effects.
    DurableScope = MakeUnique<FScopedDurableConsumer>(ConsumerOwner, StreamKey(*StreamModel), MoveTemp(IntentJson),
        [CaptureDelta = MoveTemp(CaptureDelta), AcceptedSource, AcceptedInputs](int64, FString& OutJson, TSharedPtr<const IDurableConsumerMaterial>& OutMaterial, FOperationFailure& Failure) {
            TSharedPtr<FJsonObject> Delta;
            TSharedPtr<const FCommittedWorkspaceDelta> Typed;
            TOptional<FCompositionSnapshotBinding> Binding;
            FString Scope;
            FWorkspaceError Error;
            if (FScopedDurableConsumer::IsObservationPending() || !CaptureDelta(Delta, Typed, Binding, Scope, Error) || !Delta || !Binding.IsSet()) {
                FScopedDurableConsumer::MarkObservationPending();
                auto Diagnostic = Error.HasErrors() ? Error.GetDiagnostic() : Contract::FDiagnostic::Failure(
                    TEXT("workspace_committed_capture_missing"), TEXT("The domain did not provide its actual verified after state."), TEXT("workspace_final_readback"));
                Diagnostic.Severity = Contract::ESeverity::Warning;
                Diagnostic.AssetApplied = true;
                Diagnostic.RetryImport = false;
                Diagnostic.RecoveryAction = TEXT("synchronize_incremental_or_full_without_replaying_import");
                // The native result exists independently of export observation.
                // Persist the gap in the same journal. No empty delta/binding
                // is invented, so committed-only consumers cannot advance it.
                const auto Pending = MakeShared<FJsonObject>();
                Pending->SetStringField(TEXT("observation_status"), TEXT("pending"));
                Pending->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
                if (!WriteJson(Pending, OutJson, Error)) { Failure.Diagnostic = Error.GetDiagnostic(); return false; }
                UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Diagnostic);
                return true;
            }
            const auto Verified = MakeShared<FJsonObject>();
            Verified->SetObjectField(TEXT("delta"), Delta);
            Verified->SetObjectField(TEXT("binding"), BindingJson(Binding.GetValue()));
            Verified->SetStringField(TEXT("observation_scope"), Scope);
            TArray<TSharedPtr<FJsonValue>> ActualUnits;
            if (Typed && AcceptedSource && AcceptedSource->GetEditedUnitIds().IsSet())
                for (const auto& Id : *AcceptedSource->GetEditedUnitIds()) {
                    const auto* Actual = Typed->AcceptedActualUnits.Find(Id);
                    if (!Actual) continue; // Unknown is never invented as absence.
                    TOptional<FSemanticUnit> KeyedMetadata;
                    if (Actual->GetUnitKind() != ESemanticUnitKind::PropertySet) {
                        const auto* Roots = AcceptedSource->GetResolvedKeyedOperationRoots().Find(Id);
                        if (!Roots || Roots->IsEmpty()) continue;
                        auto Values = Actual->GetValues();
                        for (const auto& Root : *Roots) Values.Remove(Root);
                        KeyedMetadata = KeyedAcceptedUnit(*Actual, MoveTemp(Values));
                        Actual = &KeyedMetadata.GetValue();
                    }
                    TSharedPtr<FJsonValue> Encoded;
                    auto Resources = MakeSemanticWorkspaceResourceTracker();
                    if (!TryEncodeCommittedWorkspaceUnit(*Actual, Resources, Encoded, Error)) { Failure.Diagnostic = Error.GetDiagnostic(); return false; }
                    ActualUnits.Add(MoveTemp(Encoded));
                }
            Verified->SetArrayField(TEXT("accepted_actual"), MoveTemp(ActualUnits));
            if (!WriteJson(Verified, OutJson, Error)) { Failure.Diagnostic = Error.GetDiagnostic(); return false; }
            if (Typed) {
                OutMaterial = MakeShared<FCommittedObservationMaterial>(MoveTemp(Typed), Binding.GetValue(), MoveTemp(Scope), AcceptedInputs);
            }
            return true;
        }, MoveTemp(Observe));
}

FCommittedWorkspaceTransaction::~FCommittedWorkspaceTransaction() = default;

bool FCommittedWorkspaceTransaction::IsReady(FWorkspaceError& Error) const
{
    if (!PreparationError.HasErrors()) return true;
    Error.SetFailure(PreparationError.GetDiagnostic()); return false;
}

void AttachAcceptedActualUnits(const FResolvedSemanticWorkspace* AcceptedSource,
    const FAuthoringModel& Actual, TSharedPtr<const FCommittedWorkspaceDelta>& Delta)
{
    if (!Delta || !AcceptedSource || !AcceptedSource->GetEditedUnitIds().IsSet()) return;
    auto WithActual = MakeShared<FCommittedWorkspaceDelta>(*Delta);
    for (const auto& Id : *AcceptedSource->GetEditedUnitIds())
        if (const auto* Unit = Actual.FindUnit(Id)) WithActual->AcceptedActualUnits.Add(Id, *Unit);
    Delta = MoveTemp(WithActual);
}
}
