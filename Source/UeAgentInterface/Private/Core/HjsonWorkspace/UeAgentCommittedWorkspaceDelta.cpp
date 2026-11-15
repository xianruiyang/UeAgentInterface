#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/StringConv.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

namespace UE::UAI::HjsonWorkspace
{
namespace
{
    using namespace StateSemantic;

    bool Fail(FWorkspaceError& Error, const FString& Reason)
    {
        Error.SetFailure(Contract::FDiagnostic::Failure(TEXT("workspace_committed_delta_invalid"),
            Reason, TEXT("workspace_committed_export")));
        return false;
    }

    bool SameStrings(const TArray<FString>& A, const TArray<FString>& B)
    {
        if (A.Num() != B.Num()) return false;
        for (int32 I = 0; I < A.Num(); ++I)
            if (!A[I].Equals(B[I], ESearchCase::CaseSensitive)) return false;
        return true;
    }

    bool SameDeclaration(const FSemanticUnit& A, const FSemanticUnit& B, bool bCompareItems = true)
    {
        // Observation provenance explains a value's origin; a different
        // readback evidence id is neither a type/owner change nor a conflict.
        // Exact receipt matching and native binding have their own owners.
        return AreSemanticUnitDeclarationsEquivalent(A, B, bCompareItems)
            && (A.GetUnitKind() == ESemanticUnitKind::PropertySet
                ? ReadablePropertyCategory(A.GetCategoryPath()).Equals(ReadablePropertyCategory(B.GetCategoryPath()), ESearchCase::CaseSensitive)
                : SameStrings(A.GetCategoryPath(), B.GetCategoryPath()));
    }

    bool Equal(const FUeAgentCanonicalValueRef& A, const FUeAgentCanonicalValueRef& B,
        FUeAgentResourceTracker& Resources, bool& OutEqual, FWorkspaceError& Error)
    {
        if (&A.Get() == &B.Get()) { OutEqual = true; return true; }
        FUeAgentSemanticComparison Comparison;
        FUeAgentStateSemanticError SemanticError;
        if (!TryCompareValues(A, B, TEXT("/values"), Resources, Comparison, SemanticError))
        { Error.SetFailure(MoveTemp(SemanticError)); return false; }
        OutEqual = Comparison.IsEquivalent();
        return true;
    }

    bool SameUnit(const FSemanticUnit& A, const FSemanticUnit& B, FUeAgentResourceTracker& Resources,
        bool& OutEqual, FWorkspaceError& Error)
    {
        OutEqual = A.SharesStorageWith(B);
        if (OutEqual) return true;
        if (!SameDeclaration(A, B)) return true;
        return TryAreSemanticUnitValuesEquivalent(A, B, Resources, OutEqual, Error);
    }

    bool IsSharedDefinition(const FSemanticUnit& Unit)
    {
        return Unit.GetUnitKind() == ESemanticUnitKind::CollectionState
            && Unit.GetRole().Equals(TEXT("property_definition"), ESearchCase::CaseSensitive)
            && Unit.GetSemanticTypeRef().Equals(TEXT("uai.authoring.property_definition/1"), ESearchCase::CaseSensitive);
    }

    // One synchronous destination fold. It borrows the caller's publication
    // rows; only changed values survive in its output. There is no owning Model
    // predecessor, per-record Model, or chain of patches to an old generation.
    class FDestinationChanges
    {
    public:
        explicit FDestinationChanges(const FAuthoringModel& InPublished)
            : Published(InPublished), Targets(InPublished.GetTargetUnitIds()), Revision(InPublished.GetBaseRevision()) {}

        const FString& GetContractId() const { return Published.GetContractId(); }
        int32 GetContractVersion() const { return Published.GetContractVersion(); }
        const FString& GetProfileId() const { return Published.GetProfileId(); }
        const TArray<FString>& GetTargetUnitIds() const { return Targets; }
        const FSemanticUnit* FindUnit(const FString& Id) const {
            if (const auto* Change = Changes.Find(Id)) return Change->After ? &Change->After.GetValue() : nullptr;
            return Published.FindUnit(Id);
        }
        bool VisitUnits(TFunctionRef<bool(const FSemanticUnit&)> Visit) const {
            if (!Published.VisitUnits([&](const FSemanticUnit& Unit) {
                const auto* Change = Changes.Find(Unit.GetUnitId());
                return !Change ? Visit(Unit) : !Change->After || Visit(Change->After.GetValue());
            })) return false;
            for (const auto& Pair : Changes)
                if (!Pair.Value.Before && Pair.Value.After && !Visit(*Pair.Value.After)) return false;
            return true;
        }
        bool VisitDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&)> Visit) const {
            if (!Published.VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
                const auto* Change = Changes.Find(Declaration.GetUnitId());
                return !Change ? Visit(Declaration) : !Change->After || Visit(Change->After->GetDeclaration());
            })) return false;
            for (const auto& Pair : Changes)
                if (!Pair.Value.Before && Pair.Value.After && !Visit(Pair.Value.After->GetDeclaration())) return false;
            return true;
        }
        TOptional<FSemanticUnit::FDeclarationView> FindDeclaration(const FString& Id) const {
            if (const auto* Change = Changes.Find(Id))
                return Change->After ? TOptional<FSemanticUnit::FDeclarationView>(Change->After->GetDeclaration()) : TOptional<FSemanticUnit::FDeclarationView>();
            return Published.FindDeclaration(Id);
        }
        TArray<const FSemanticUnit*> FindUnitsForTargetIdentity(const FString& Identity) const {
            TArray<const FSemanticUnit*> Result;
            VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Unit) {
                if (Unit.GetTargetIdentityKey() == Identity) Result.Add(FindUnit(Unit.GetUnitId()));
                return true;
            });
            return Result;
        }
        void Set(const FString& Id, TOptional<FSemanticUnit> Before, TOptional<FSemanticUnit> After) {
            auto* Change = Changes.Find(Id);
            if (!Change) {
                Change = &Changes.Add(Id, {Id, MoveTemp(Before), {}});
            }
            Change->After = MoveTemp(After);
            if (!Change->Before && !Change->After) Changes.Remove(Id);
        }
        bool Seal(FUeAgentResourceTracker& Resources, FCommittedWorkspaceDelta& Out, FWorkspaceError& Error) const {
            Out = {};
            Out.ContractId = GetContractId(); Out.ContractVersion = GetContractVersion(); Out.ProfileId = GetProfileId();
            Out.BeforeTargets = Published.GetTargetUnitIds(); Out.AfterTargets = Targets; Out.AfterRevision = Revision;
            TArray<FString> Ids; Changes.GetKeys(Ids); Ids.Sort();
            TSet<FString> ResolvedDependencies;
            for (const auto& Id : Ids) {
                const auto& Change = Changes.FindChecked(Id);
                if (Change.Before && Change.After) {
                    bool bSame = false;
                    if (!SameUnit(*Change.Before, *Change.After, Resources, bSame, Error)) return false;
                    if (bSame) continue;
                }
                // The native difference may omit a shared definition unchanged at
                // both capture points. Its explicit edge still belongs to the
                // final destination, before either publication writer selects a type.
                if (Change.After && (Change.After->GetUnitKind() == ESemanticUnitKind::PropertySet || IsSharedDefinition(*Change.After))) {
                    for (const FString& Dependency : Change.After->GetDependencyRefs()) {
                        if (ResolvedDependencies.Contains(Dependency)) continue;
                        if (!FindDeclaration(Dependency)) {
                            if (!Published.CheckValueReadResult(Error)) return false;
                            Error.SetSourceFailure(TEXT("unresolved_semantic_unit_reference"),
                                FString::Printf(TEXT("The committed row requires '%s', which is absent from its destination."), *Dependency), Id);
                            return false;
                        }
                        ResolvedDependencies.Add(Dependency);
                    }
                }
                Out.Changes.Add(Change);
            }
            return true;
        }

        const FAuthoringModel& Published;
        TArray<FString> Targets;
        FString Revision;
        TMap<FString, FCommittedUnitChange> Changes;
    };

    FSemanticUnit WithMembership(const FSemanticUnit& Unit, TArray<FString> Items, FString OrderingRef,
        const TArray<FString>* DependencyRefs = nullptr)
    {
        Items.Sort();
        return FSemanticUnit(Unit.GetUnitId(), Unit.GetUnitKind(), Unit.GetOwnerRef(),
            Unit.GetSemanticTypeRef(), Unit.GetRole(), Unit.GetFieldSetRef(), Unit.GetValues(),
            MoveTemp(Items), Unit.GetRelationRefs(), MoveTemp(OrderingRef), Unit.GetLayoutRef(),
            DependencyRefs ? *DependencyRefs : Unit.GetDependencyRefs(), Unit.GetProfileVisibility(), Unit.GetSourceProvenance(),
            Unit.GetTargetIdentityKey(), Unit.GetCategoryPath());
    }

    // Edge adapter for receipts produced while the native verification owner
    // used a package key for a root property exported with an object key.
    // Values remain the persisted real before/after observations. Never infer
    // a value or a missing observation from authored HJSON.
    template <typename Destination>
    bool RebindLegacyPackageProperty(const Destination& Baseline,
        TOptional<FSemanticUnit>& Old, TOptional<FSemanticUnit>& New, FString& Id)
    {
        if (!Old || !New || Old->GetSemanticTypeRef() != TEXT("core.property")
            || !SameDeclaration(*Old, *New) || Baseline.GetTargetUnitIds().Num() != 1) return false;
        UeAgentAuthoringIdentity::FCanonicalIdentity Property;
        Contract::FDiagnostic Error;
        const auto* Root = Baseline.FindUnit(Baseline.GetTargetUnitIds()[0]);
        if (!Root || !UeAgentAuthoringIdentity::ParseCanonicalKey(Old->GetTargetIdentityKey(), Property, Error)) return false;
        const auto* AssetPath = Root->FindValue(TEXT("asset_path"));
        const FString Package = Property.GetSelector(TEXT("owner_path"));
        if (!FPackageName::IsValidLongPackageName(Package)
            || !AssetPath || (*AssetPath)->GetKind() != EUeAgentValueKind::String
            || !Package.Equals((*AssetPath)->GetText(), ESearchCase::IgnoreCase)) return false;
        auto Selectors = Property.Selectors;
        Selectors.Add(TEXT("owner_path"), Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package));
        UeAgentAuthoringIdentity::FCanonicalIdentity ObjectProperty;
        if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(Property.ModelId, Property.TargetKind, Selectors, ObjectProperty, Error)) return false;
        const auto Rebind = [&](const FSemanticUnit& Unit, const FString& UnitId) {
            return FSemanticUnit(UnitId, Unit.GetUnitKind(), Unit.GetOwnerRef(), Unit.GetSemanticTypeRef(), Unit.GetRole(),
                Unit.GetFieldSetRef(), Unit.GetValues(), Unit.GetItemRefs(), Unit.GetRelationRefs(), Unit.GetOrderingRef(),
                Unit.GetLayoutRef(), Unit.GetDependencyRefs(), Unit.GetProfileVisibility(), Unit.GetSourceProvenance(),
                ObjectProperty.CanonicalKey, Unit.GetCategoryPath());
        };
        for (const auto* Candidate : Baseline.FindUnitsForTargetIdentity(ObjectProperty.CanonicalKey)) {
            auto Rebound = Rebind(*Old, Candidate->GetUnitId());
            if (!SameDeclaration(Rebound, *Candidate)) continue;
            Id = Candidate->GetUnitId();
            Old = MoveTemp(Rebound);
            New = Rebind(*New, Id);
            UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=committed_legacy_property_identity adapted=1 values=persisted_actual"));
            return true;
        }
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Values.Num());
        for (const auto& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
        return Result;
    }

    bool ReadStrings(const FJsonObject& Json, const TCHAR* Field, TArray<FString>& Out, FWorkspaceError& Error)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Json.TryGetArrayField(Field, Values)) return Fail(Error, FString::Printf(TEXT("Missing array %s."), Field));
        for (const auto& Value : *Values) {
            FString Text;
            if (!Value || !Value->TryGetString(Text)) return Fail(Error, TEXT("A string array contains a non-string item."));
            Out.Add(MoveTemp(Text));
        }
        return true;
    }

    bool EncodeUnit(const FSemanticUnit& Unit, FUeAgentResourceTracker& Resources,
        TSharedPtr<FJsonValue>& Out, FWorkspaceError& Error)
    {
        TArray<uint8> Bytes;
        if (!TrySerializeSemanticUnitRow(Unit, Bytes, Error)) return false;
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        const FString Row(Converted.Length(), Converted.Get());
        const auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("row"), Row);
        Json->SetNumberField(TEXT("kind"), static_cast<int32>(Unit.GetUnitKind()));
        // These presentation inputs are intentionally outside the historical
        // semantic row hash, but must survive a persisted delta exactly.
        Json->SetStringField(TEXT("source_provenance"), Unit.GetSourceProvenance());
        Json->SetArrayField(TEXT("category_path"), Strings(Unit.GetCategoryPath()));
        Out = MakeShared<FJsonValueObject>(Json);
        return true;
    }

    bool DecodeUnit(const FJsonValue& Value, FUeAgentResourceTracker& Resources,
        TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
    {
        if (Value.Type == EJson::Null) { Out.Reset(); return true; }
        if (Value.Type != EJson::Object) return Fail(Error, TEXT("A committed unit must be an object or null."));
        const auto Json = Value.AsObject();
        FString Row, Provenance;
        int32 Kind = -1;
        TArray<FString> Categories;
        if (!Json->TryGetStringField(TEXT("row"), Row) || !Json->TryGetStringField(TEXT("source_provenance"), Provenance)
            || !Json->TryGetNumberField(TEXT("kind"), Kind) || Kind < 0 || Kind > static_cast<int32>(ESemanticUnitKind::OpaqueExtension)
            || !ReadStrings(*Json, TEXT("category_path"), Categories, Error)) return Fail(Error, TEXT("The captured unit envelope is invalid."));
        FHjsonDocumentPtr Document;
        if (!TryParseHjsonDocument(Row, TEXT("durable://committed-unit"), Resources, Document, Error)) return false;
        const auto& Root = Document->GetRoot();
        auto Text = [&](const TCHAR* Field, FString& Result, bool bRequired = false) {
            const auto* Node = Root->Get(Field);
            if (!Node && !bRequired) { Result.Reset(); return true; }
            if (!Node || Node->GetKind() != EHjsonNodeKind::String) return Fail(Error, FString::Printf(TEXT("Invalid captured field %s."), Field));
            Result = Node->GetText(); return true;
        };
        auto Array = [&](const TCHAR* Field, TArray<FString>& Result) {
            const auto* Node = Root->Get(Field);
            if (!Node) return true;
            if (Node->GetKind() != EHjsonNodeKind::Array) return Fail(Error, TEXT("Invalid captured reference array."));
            for (const auto& Item : Node->GetItems()) {
                if (Item->GetKind() != EHjsonNodeKind::String) return Fail(Error, TEXT("Invalid captured reference."));
                Result.Add(Item->GetText());
            }
            return true;
        };
        FString Id, Owner, Type, Role, FieldSet, Ordering, Layout, Target, KindText;
        TArray<FString> Items, Relations, Dependencies, Visibility;
        if (!Text(TEXT("unit_kind"), KindText, true) || !KindText.Equals(LexToString(static_cast<ESemanticUnitKind>(Kind)), ESearchCase::CaseSensitive))
            return Fail(Error, TEXT("The captured unit kind differs from its envelope."));
        if (!Text(TEXT("unit_id"), Id, true) || !Text(TEXT("owner_ref"), Owner) || !Text(TEXT("semantic_type_ref"), Type, true)
            || !Text(TEXT("role"), Role, true) || !Text(TEXT("field_set_ref"), FieldSet) || !Text(TEXT("ordering_ref"), Ordering)
            || !Text(TEXT("layout_ref"), Layout) || !Text(TEXT("target_identity_key"), Target)
            || !Array(TEXT("item_refs"), Items) || !Array(TEXT("relation_refs"), Relations)
            || !Array(TEXT("dependency_refs"), Dependencies) || !Array(TEXT("profile_visibility"), Visibility)) return false;
        const auto* ValuesNode = Root->Get(TEXT("values"));
        if (!ValuesNode || ValuesNode->GetKind() != EHjsonNodeKind::Object) return Fail(Error, TEXT("The captured unit values are missing."));
        TMap<FString, FUeAgentCanonicalValueRef> Values;
        for (const auto& Field : ValuesNode->GetEntries()) {
            FUeAgentCanonicalValuePtr Canonical;
            if (!TryProjectHjsonCanonicalValue(Field.Value.Get(), Resources, Canonical, Error)) return false;
            Values.Add(Field.Key, Canonical.ToSharedRef());
        }
        Out.Emplace(MoveTemp(Id), static_cast<ESemanticUnitKind>(Kind), MoveTemp(Owner), MoveTemp(Type), MoveTemp(Role),
            MoveTemp(FieldSet), MoveTemp(Values), MoveTemp(Items), MoveTemp(Relations), MoveTemp(Ordering), MoveTemp(Layout),
            MoveTemp(Dependencies), MoveTemp(Visibility), MoveTemp(Provenance), MoveTemp(Target), MoveTemp(Categories));
        if (!Out->IsValid()) { Error.SetFailure(Out->GetValidationDiagnostic()); return false; }
        return TryNormalizeAuthoringUnit(Out.GetValue(), Resources, Error);
    }

    const FUeAgentCanonicalValueRef* ObjectField(const FUeAgentCanonicalValue& Value, const FString& Key)
    {
        for (const auto& Pair : Value.GetObjectItems())
            if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive)) return &Pair.Value;
        return nullptr;
    }

    // This applies an observed difference, not an author's proposed edit. A
    // changed leaf takes the actual committed value; an untouched leaf keeps
    // the independent workspace baseline. Arrays and typed leaves are atomic.
    bool PatchValue(const FUeAgentCanonicalValueRef& Before, const FUeAgentCanonicalValueRef& After,
        const FUeAgentCanonicalValueRef& Baseline, FUeAgentResourceTracker& Resources,
        FUeAgentCanonicalValuePtr& Out, FWorkspaceError& Error)
    {
        bool bEqual = false;
        if (!Equal(Before, After, Resources, bEqual, Error)) return false;
        if (bEqual) { Out = Baseline; return true; }
        if (Before->GetKind() != EUeAgentValueKind::Object || After->GetKind() != EUeAgentValueKind::Object
            || Baseline->GetKind() != EUeAgentValueKind::Object) {
            const bool bAggregate = Before->GetKind() == EUeAgentValueKind::Array
                || Before->GetKind() == EUeAgentValueKind::Spatial || Before->GetKind() == EUeAgentValueKind::Temporal;
            if (bAggregate) {
                bool bAtBefore = false, bAtAfter = false;
                if (!Equal(Before, Baseline, Resources, bAtBefore, Error)
                    || !Equal(After, Baseline, Resources, bAtAfter, Error)) return false;
                if (!bAtBefore && !bAtAfter) return Fail(Error,
                    TEXT("The committed aggregate overlaps unsynchronized external state; use incremental or full export."));
            }
            Out = After; return true;
        }
        TArray<TPair<FString, FUeAgentCanonicalValueRef>> Values;
        for (const auto& Pair : Baseline->GetObjectItems()) {
            const auto* Old = ObjectField(*Before, Pair.Key);
            const auto* New = ObjectField(*After, Pair.Key);
            if (Old && !New) continue;
            FUeAgentCanonicalValuePtr Patched = Pair.Value;
            if (Old && New && !PatchValue(*Old, *New, Pair.Value, Resources, Patched, Error)) return false;
            if (!Old && New) Patched = *New;
            Values.Emplace(Pair.Key, Patched.ToSharedRef());
        }
        for (const auto& Pair : After->GetObjectItems()) {
            if (ObjectField(*Baseline, Pair.Key)) continue;
            const auto* Old = ObjectField(*Before, Pair.Key);
            bool bUnchanged = false;
            if (Old && !Equal(*Old, Pair.Value, Resources, bUnchanged, Error)) return false;
            if (!Old || !bUnchanged) Values.Add(Pair);
        }
        FUeAgentStateSemanticError SemanticError;
        if (!FUeAgentCanonicalValue::TryObject(Values, Resources, Out, SemanticError))
        { Error.SetFailure(MoveTemp(SemanticError)); return false; }
        return true;
    }
}

bool TryEncodeCommittedWorkspaceUnit(const FSemanticUnit& Unit, FUeAgentResourceTracker& Resources,
    TSharedPtr<FJsonValue>& Out, FWorkspaceError& Error)
{
    return EncodeUnit(Unit, Resources, Out, Error);
}

bool TryDecodeCommittedWorkspaceUnit(const FJsonValue& Value, FUeAgentResourceTracker& Resources,
    TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
{
    return DecodeUnit(Value, Resources, Out, Error);
}

template <typename TBefore, typename TAfter>
static bool BuildCommittedWorkspaceDelta(const TBefore& Before, const TAfter& After,
    FUeAgentResourceTracker& Resources, TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
    TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped)
{
    if (Before.GetContractId() != After.GetContractId() || Before.GetContractVersion() != After.GetContractVersion()
        || Before.GetProfileId() != After.GetProfileId()) return Fail(Error, TEXT("The observed transaction changed its model contract or profile."));
    const auto Delta = MakeShared<FJsonObject>();
    Delta->SetStringField(TEXT("schema"), TEXT("uai.committed_workspace_delta/1"));
    Delta->SetStringField(TEXT("contract"), After.GetContractId());
    Delta->SetNumberField(TEXT("contract_version"), After.GetContractVersion());
    Delta->SetStringField(TEXT("profile"), After.GetProfileId());
    Delta->SetStringField(TEXT("after_revision"), After.GetBaseRevision());
    Delta->SetArrayField(TEXT("before_targets"), Strings(Before.GetTargetUnitIds()));
    Delta->SetArrayField(TEXT("after_targets"), Strings(After.GetTargetUnitIds()));
    TSharedPtr<FCommittedWorkspaceDelta> Typed;
    if (OutTyped) {
        Typed = MakeShared<FCommittedWorkspaceDelta>();
        Typed->ContractId = After.GetContractId(); Typed->ContractVersion = After.GetContractVersion();
        Typed->ProfileId = After.GetProfileId(); Typed->AfterRevision = After.GetBaseRevision();
        Typed->BeforeTargets = Before.GetTargetUnitIds(); Typed->AfterTargets = After.GetTargetUnitIds();
    }
    TArray<TSharedPtr<FJsonValue>> Changes;
    auto AddChange = [&](const FString& Id, const FSemanticUnit* Old, const FSemanticUnit* New) {
        const auto Change = MakeShared<FJsonObject>();
        Change->SetStringField(TEXT("id"), Id);
        TSharedPtr<FJsonValue> OldValue = MakeShared<FJsonValueNull>(), NewValue = MakeShared<FJsonValueNull>();
        if ((Old && !EncodeUnit(*Old, Resources, OldValue, Error)) || (New && !EncodeUnit(*New, Resources, NewValue, Error))) return false;
        Change->SetField(TEXT("before"), OldValue); Change->SetField(TEXT("after"), NewValue);
        if (Typed) {
            FCommittedUnitChange Item; Item.UnitId = Id;
            if (Old) Item.Before = *Old;
            if (New) Item.After = *New;
            Typed->Changes.Add(MoveTemp(Item));
        }
        Changes.Add(MakeShared<FJsonValueObject>(Change)); return true;
    };
    for (const auto& Unit : After.GetUnits()) {
        const auto* Old = Before.FindUnit(Unit.GetUnitId());
        bool bSame = false;
        if (Old && !SameUnit(*Old, Unit, Resources, bSame, Error)) return false;
        if (!bSame && !AddChange(Unit.GetUnitId(), Old, &Unit)) return false;
    }
    for (const auto& Unit : Before.GetUnits())
        if (!After.FindUnit(Unit.GetUnitId()) && !AddChange(Unit.GetUnitId(), &Unit, nullptr)) return false;
    Delta->SetArrayField(TEXT("changes"), MoveTemp(Changes));
    OutDelta = Delta;
    if (OutTyped) *OutTyped = MoveTemp(Typed);
    return true;
}

bool TryBuildCommittedWorkspaceDelta(const FAuthoringModel& Before, const FAuthoringModel& After,
    FUeAgentResourceTracker& Resources, TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
    TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped)
{
    return BuildCommittedWorkspaceDelta(Before, After, Resources, OutDelta, Error, OutTyped);
}

bool TryBuildCommittedWorkspaceDelta(const FCompositionUnitSet& Before, const FCompositionUnitSet& After,
    FUeAgentResourceTracker& Resources, TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
    TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped)
{
    struct FUnitView {
        const FCompositionUnitSet& Set;
        TMap<FString, const FSemanticUnit*> Index;
        explicit FUnitView(const FCompositionUnitSet& InSet) : Set(InSet) {
            Index.Reserve(Set.Units.Num());
            for (const auto& Unit : Set.Units) Index.Add(Unit.GetUnitId(), &Unit);
        }
        const FString& GetContractId() const { return Set.ContractId; }
        int32 GetContractVersion() const { return Set.ContractVersion; }
        const FString& GetProfileId() const { return Set.ProfileId; }
        const FString& GetBaseRevision() const { return Set.BaseRevision; }
        const TArray<FString>& GetTargetUnitIds() const { return Set.TargetUnitIds; }
        const TArray<FSemanticUnit>& GetUnits() const { return Set.Units; }
        const FSemanticUnit* FindUnit(const FString& Id) const {
            const auto* Found = Index.Find(Id); return Found ? *Found : nullptr;
        }
    };
    return BuildCommittedWorkspaceDelta(FUnitView(Before), FUnitView(After), Resources, OutDelta, Error, OutTyped);
}

TOptional<bool> TryBuildCommittedPropertyDelta(const FAuthoringModel& Baseline,
    const FAuthoringModel& Before, const FAuthoringModel& After, const TArray<FString>& ChangedUnits,
    const FString& ActualAfterRevision, FUeAgentResourceTracker& Resources,
    TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
    TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped)
{
    if (Baseline.GetContractId() != Before.GetContractId() || Before.GetContractId() != After.GetContractId()
        || Baseline.GetContractVersion() != Before.GetContractVersion() || Before.GetContractVersion() != After.GetContractVersion()
        || Baseline.GetProfileId() != Before.GetProfileId() || Before.GetProfileId() != After.GetProfileId()) return {};
    TArray<TSharedPtr<FJsonValue>> Changes;
    TSharedPtr<FCommittedWorkspaceDelta> Typed;
    if (OutTyped) {
        Typed = MakeShared<FCommittedWorkspaceDelta>();
        Typed->ContractId = Baseline.GetContractId(); Typed->ContractVersion = Baseline.GetContractVersion();
        Typed->ProfileId = Baseline.GetProfileId(); Typed->AfterRevision = ActualAfterRevision;
        Typed->BeforeTargets = Baseline.GetTargetUnitIds(); Typed->AfterTargets = Baseline.GetTargetUnitIds();
    }
    for (const auto& Id : ChangedUnits) {
        const auto* Published = Baseline.FindUnit(Id);
        const auto* Old = Before.FindUnit(Id);
        const auto* New = After.FindUnit(Id);
        if (!Published || !Old || !New || Old->GetUnitKind() != ESemanticUnitKind::PropertySet
            || !SameDeclaration(*Published, *Old) || !SameDeclaration(*Old, *New)
            || Old->GetValueCount() != 1 || New->GetValueCount() != 1) return {};
        const auto OldField = Old->GetValues().CreateConstIterator();
        const auto NewField = New->GetValues().CreateConstIterator();
        if (!OldField.Key().Equals(NewField.Key(), ESearchCase::CaseSensitive)
            || OldField.Value()->GetKind() != NewField.Value()->GetKind()) return {};
        const auto Kind = OldField.Value()->GetKind();
        if (Kind != EUeAgentValueKind::Decimal && Kind != EUeAgentValueKind::Integer && Kind != EUeAgentValueKind::Boolean) return {};
        TArray<FString> Dependencies = Old->GetDependencyRefs();
        TSet<FString> Seen;
        while (!Dependencies.IsEmpty()) {
            const FString Dependency = Dependencies.Pop(EAllowShrinking::No);
            if (Seen.Contains(Dependency)) continue;
            Seen.Add(Dependency);
            const auto* A = Baseline.FindUnit(Dependency);
            const auto* B = Before.FindUnit(Dependency);
            const auto* C = After.FindUnit(Dependency);
            bool bSame = false;
            if (!A || !B || !C) return {};
            if (!SameUnit(*A, *B, Resources, bSame, Error)) return false;
            if (!bSame) return {};
            if (!SameUnit(*B, *C, Resources, bSame, Error)) return false;
            if (!bSame) return {};
            Dependencies.Append(B->GetDependencyRefs());
        }
        const auto Change = MakeShared<FJsonObject>();
        Change->SetStringField(TEXT("id"), Id);
        TSharedPtr<FJsonValue> OldJson, NewJson;
        if (!EncodeUnit(*Old, Resources, OldJson, Error) || !EncodeUnit(*New, Resources, NewJson, Error)) return false;
        Change->SetField(TEXT("before"), OldJson); Change->SetField(TEXT("after"), NewJson);
        if (Typed) Typed->Changes.Add({Id, *Old, *New});
        Changes.Add(MakeShared<FJsonValueObject>(Change));
    }
    const auto Delta = MakeShared<FJsonObject>();
    Delta->SetStringField(TEXT("schema"), TEXT("uai.committed_workspace_delta/1"));
    Delta->SetStringField(TEXT("contract"), Baseline.GetContractId());
    Delta->SetNumberField(TEXT("contract_version"), Baseline.GetContractVersion());
    Delta->SetStringField(TEXT("profile"), Baseline.GetProfileId());
    Delta->SetStringField(TEXT("after_revision"), ActualAfterRevision);
    Delta->SetArrayField(TEXT("before_targets"), Strings(Baseline.GetTargetUnitIds()));
    Delta->SetArrayField(TEXT("after_targets"), Strings(Baseline.GetTargetUnitIds()));
    Delta->SetArrayField(TEXT("changes"), MoveTemp(Changes));
    OutDelta = Delta;
    if (OutTyped) *OutTyped = MoveTemp(Typed);
    return true;
}

bool TryReadCommittedWorkspaceDelta(const FJsonObject& Delta,
    FUeAgentResourceTracker& Resources, FCommittedWorkspaceDelta& Typed, FWorkspaceError& Error)
{
    Typed = {};
    FString Schema, Contract, Profile, Revision;
    int32 Version = 0;
    const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
    if (!Delta.TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("uai.committed_workspace_delta/1")
        || !Delta.TryGetStringField(TEXT("contract"), Contract)
        || !Delta.TryGetNumberField(TEXT("contract_version"), Version)
        || !Delta.TryGetStringField(TEXT("profile"), Profile)
        || !Delta.TryGetStringField(TEXT("after_revision"), Revision) || !Delta.TryGetArrayField(TEXT("changes"), Changes))
        return Fail(Error, TEXT("The committed difference is incompatible with this workspace baseline."));
    TArray<FString> OldTargets, NewTargets;
    if (!ReadStrings(Delta, TEXT("before_targets"), OldTargets, Error) || !ReadStrings(Delta, TEXT("after_targets"), NewTargets, Error)) return false;
    Typed.ContractId = MoveTemp(Contract); Typed.ContractVersion = Version; Typed.ProfileId = MoveTemp(Profile);
    Typed.AfterRevision = MoveTemp(Revision); Typed.BeforeTargets = MoveTemp(OldTargets); Typed.AfterTargets = MoveTemp(NewTargets);
    TSet<FString> Seen;
    Typed.Changes.Reserve(Changes->Num());
    for (const auto& Value : *Changes) {
        if (!Value || Value->Type != EJson::Object) return Fail(Error, TEXT("A committed change is invalid."));
        const auto Change = Value->AsObject();
        FCommittedUnitChange Item;
        const auto* OldValue = Change->Values.Find(TEXT("before"));
        const auto* NewValue = Change->Values.Find(TEXT("after"));
        if (!Change->TryGetStringField(TEXT("id"), Item.UnitId) || Seen.Contains(Item.UnitId)
            || !OldValue || !NewValue || !OldValue->IsValid() || !NewValue->IsValid())
            return Fail(Error, TEXT("A committed change has missing or duplicate identity."));
        Seen.Add(Item.UnitId);
        if (!DecodeUnit(**OldValue, Resources, Item.Before, Error) || !DecodeUnit(**NewValue, Resources, Item.After, Error)) return false;
        Typed.Changes.Add(MoveTemp(Item));
    }
    return true;
}


static bool TryFoldCommittedDifference(FDestinationChanges& Destination, const FCommittedWorkspaceDelta& Delta,
    FUeAgentResourceTracker& Resources, FWorkspaceError& Error)
{
    const auto* Baseline = &Destination;
    const auto& Contract = Delta.ContractId; const int32 Version = Delta.ContractVersion;
    const auto& Profile = Delta.ProfileId; const auto& Revision = Delta.AfterRevision;
    const auto& OldTargets = Delta.BeforeTargets; const auto& NewTargets = Delta.AfterTargets;
    if (Contract != Baseline->GetContractId() || Version != Baseline->GetContractVersion() || Profile != Baseline->GetProfileId())
        return Fail(Error, TEXT("The committed difference is incompatible with this workspace baseline."));
    TMap<FString, FSemanticUnit> Replacements;
    TSet<FString> Removed, Seen, DefinitionRemovalCandidates;
    TMap<FString, FSemanticUnit> RemovedDefinitions, ObservedRemovedUnits;
    TMap<FString, TPair<FSemanticUnit, FSemanticUnit>> ScopedMembership, ScopedOrdering;
    TMap<FString, const FCommittedUnitChange*> ScopedDependencies;
    TArray<FSemanticUnit> ObservedBefore, ObservedAfter;
    TArray<FString> AddedOrder;
    // Resolve this fold's real destination-before once. A native add is not
    // itself evidence that a lagging publication lacks that identity. Native
    // hierarchy members do have a complete owning item_refs declaration; use
    // that same destination's membership (or a proven absent added ancestor)
    // before asking the directory to search for an impossible child row.
    TMap<FString, const FCommittedUnitChange*> ActualById;
    for (const auto& Change : Delta.Changes) ActualById.Add(Change.UnitId, &Change);
    TMap<FString, const FSemanticUnit*> BeforeFacts;
    TSet<FString> Resolving;
    TOptional<TSet<FString>> DefinitionIds;
    int32 MembershipAbsences = 0, AncestorAbsences = 0, DefinitionAbsences = 0;
    TFunction<const FSemanticUnit*(const FString&)> FindBefore;
    FindBefore = [&](const FString& Id) -> const FSemanticUnit* {
        if (const auto* Fact = BeforeFacts.Find(Id)) return *Fact;
        if (const auto* Changed = Destination.Changes.Find(Id)) {
            const auto* Unit = Changed->After ? &Changed->After.GetValue() : nullptr;
            BeforeFacts.Add(Id, Unit); return Unit;
        }
        const auto* Actual = ActualById.Find(Id);
        const auto* Added = Actual && !(*Actual)->Before && (*Actual)->After ? &(*Actual)->After.GetValue() : nullptr;
        if (Added && !Resolving.Contains(Id)) {
            Resolving.Add(Id);
            const auto* OwnerChange = ActualById.Find(Added->GetOwnerRef());
            const auto* AddedOwner = OwnerChange && (*OwnerChange)->After ? &(*OwnerChange)->After.GetValue() : nullptr;
            const bool bNativeItem = Added->GetUnitKind() == ESemanticUnitKind::ItemState
                && Added->GetRole() == TEXT("actor_component_hierarchy")
                && (Added->GetSemanticTypeRef() == TEXT("uai.semantic.level.actor.target")
                    || Added->GetSemanticTypeRef() == TEXT("uai.semantic.level.component.target"));
            const bool bOwnedByAddedItem = AddedOwner && !(*OwnerChange)->Before
                && AddedOwner->GetUnitKind() == ESemanticUnitKind::ItemState
                && AddedOwner->GetRole() == TEXT("actor_component_hierarchy")
                && (AddedOwner->GetSemanticTypeRef() == TEXT("uai.semantic.level.actor.target")
                    || AddedOwner->GetSemanticTypeRef() == TEXT("uai.semantic.level.component.target"));
            if (AddedOwner && ((bNativeItem && AddedOwner->GetItemRefs().Contains(Id)) || bOwnedByAddedItem)) {
                const auto* OwnerBefore = FindBefore(Added->GetOwnerRef());
                const bool bAbsentOwner = !OwnerBefore && !(*OwnerChange)->Before;
                const bool bAbsentMember = bNativeItem && OwnerBefore
                    && OwnerBefore->GetSemanticTypeRef() == AddedOwner->GetSemanticTypeRef()
                    && OwnerBefore->GetTargetIdentityKey() == AddedOwner->GetTargetIdentityKey()
                    && !OwnerBefore->GetItemRefs().Contains(Id);
                if (bAbsentOwner || bAbsentMember) {
                    if (bAbsentOwner) ++AncestorAbsences; else ++MembershipAbsences;
                    Resolving.Remove(Id); BeforeFacts.Add(Id, nullptr); return nullptr;
                }
            }
            if (IsSharedDefinition(*Added)) {
                if (!DefinitionIds) {
                    DefinitionIds.Emplace();
                    if (!Destination.Published.VisitDefinitionDeclarations([&](const auto& Declaration) {
                        DefinitionIds->Add(Declaration.GetUnitId()); return true;
                    })) { Resolving.Remove(Id); return nullptr; }
                }
                if (!DefinitionIds->Contains(Id)) {
                    ++DefinitionAbsences;
                    Resolving.Remove(Id); BeforeFacts.Add(Id, nullptr); return nullptr;
                }
            }
            Resolving.Remove(Id);
        }
        const auto* Unit = Destination.Published.FindUnit(Id);
        BeforeFacts.Add(Id, Unit); return Unit;
    };
    for (const auto& Change : Delta.Changes) {
        FString Id = Change.UnitId;
        if (Seen.Contains(Id))
            return Fail(Error, TEXT("A committed change has missing or duplicate identity."));
        TOptional<FSemanticUnit> Old = Change.Before, New = Change.After;
        if ((!Old && !New) || (Old && !Old->GetUnitId().Equals(Id, ESearchCase::CaseSensitive))
            || (New && !New->GetUnitId().Equals(Id, ESearchCase::CaseSensitive))) return Fail(Error, TEXT("The committed unit identity differs from its record."));
        if (Old && New && Old->GetSemanticTypeRef() == TEXT("core.property")
            && !FindBefore(Id)) RebindLegacyPackageProperty(*Baseline, Old, New, Id);
        if (Seen.Contains(Id)) return Fail(Error, TEXT("A committed change has duplicate resolved identity."));
        Seen.Add(Id);
        const auto IsObservedContributor = [](const FSemanticUnit& Unit) {
            return (Unit.GetUnitKind() == ESemanticUnitKind::TargetState || Unit.GetUnitKind() == ESemanticUnitKind::ItemState
                || Unit.GetUnitKind() == ESemanticUnitKind::PropertySet) && Unit.GetDeclaration().HasDirectAssetPathField();
        };
        if (Old && IsObservedContributor(*Old)) ObservedBefore.Add(*Old);
        if (New && IsObservedContributor(*New)) ObservedAfter.Add(*New);
        const auto* Base = FindBefore(Id);
        const auto& Dependency = New ? *New : *Old;
        if ((!Old || !New) && IsCompositionAssetDependencies(Dependency, Dependency.GetOwnerRef())) {
            const auto* Root = ActualById.FindRef(Dependency.GetOwnerRef());
            const auto* AcceptedRoot = FindBefore(Dependency.GetOwnerRef());
            // A scoped projection may contain zero contributing paths on one side.
            // If the target itself was created or deleted, ordinary ownership applies.
            if (Root && Root->Before && Root->After && AcceptedRoot
                && (AcceptedRoot->GetSemanticTypeRef() == TEXT("ue.blueprint.asset")
                    || !SameDeclaration(*Root->Before, *AcceptedRoot))) {
                if (Root->Before->GetDependencyRefs().Contains(Id) != Old.IsSet()
                    || Root->After->GetDependencyRefs().Contains(Id) != New.IsSet()
                    || AcceptedRoot->GetDependencyRefs().Contains(Id) != (Base != nullptr)
                    || (Base && !SameDeclaration(Dependency, *Base)))
                    return Fail(Error, TEXT("The committed dependency row is inconsistent with its observed or accepted target."));
                if (Base) {
                    ScopedDependencies.Add(Id, &Change);
                    continue;
                }
                if (Old) return Fail(Error, TEXT("The removed dependency row is absent from its destination."));
            }
        }
        if (!New) {
            // Leaving an observed owner's dependency closure is not proof that
            // a shared definition disappeared from the complete workspace.
            if (Old && IsSharedDefinition(*Old)) {
                DefinitionRemovalCandidates.Add(Id); RemovedDefinitions.Add(Id, *Old);
            }
            else { Removed.Add(Id); ObservedRemovedUnits.Add(Id, *Old); }
            continue;
        }
        if (!Old) {
            if (Base) {
                bool bSame = false;
                if (IsSharedDefinition(*New) && IsSharedDefinition(*Base)) {
                    if (!SameUnit(*New, *Base, Resources, bSame, Error)) return false;
                    if (bSame) continue;
                }
                return Fail(Error, TEXT("An added committed unit conflicts with an existing workspace identity."));
            }
            AddedOrder.Add(Id); Replacements.Add(Id, *New); continue;
        }
        if (!Base) return Fail(Error, TEXT("A committed change requires an owner absent from this workspace; synchronize with incremental or full export."));
        const bool bDeclarationChanged = !SameDeclaration(*Old, *New);
        bool bScopedMembers = false;
        TOptional<FSemanticUnit> DependencyOwner;
        if (bDeclarationChanged && !SameDeclaration(*Old, *Base) && !SameDeclaration(*New, *Base)) {
            const FSemanticUnit* OldShape = &*Old;
            const FSemanticUnit* NewShape = &*New;
            const FSemanticUnit* BaseShape = Base;
            TOptional<FSemanticUnit> ComparableOld, ComparableNew, ComparableBase;
            if (Old->GetUnitKind() == ESemanticUnitKind::TargetState && Old->GetRole() == TEXT("authoring_target")) {
                auto OldRefs = Old->GetDependencyRefs(), NewRefs = New->GetDependencyRefs(), BaseRefs = Base->GetDependencyRefs();
                auto DestinationRefs = BaseRefs;
                TArray<FString> Refs = BaseRefs;
                for (const auto& Ref : OldRefs) Refs.AddUnique(Ref);
                for (const auto& Ref : NewRefs) Refs.AddUnique(Ref);
                // Project the same owned binding out of each declaration independently.
                // A change needs its native child row. An unobserved accepted link
                // needs its existing child and stays intact until proven retired.
                for (const auto& Ref : Refs) {
                    const bool bOld = OldRefs.Contains(Ref), bNew = NewRefs.Contains(Ref), bBase = BaseRefs.Contains(Ref);
                    if (bOld == bNew && bOld == bBase) continue;
                    const auto* ActualEdge = ActualById.FindRef(Ref);
                    const auto* AcceptedEdge = FindBefore(Ref);
                    if (bBase != (AcceptedEdge != nullptr) || (AcceptedEdge && !IsCompositionAssetDependencies(*AcceptedEdge, Id))) continue;
                    if (bOld != bNew) {
                        if (!ActualEdge || ActualEdge->Before.IsSet() != bOld || ActualEdge->After.IsSet() != bNew
                            || !IsCompositionAssetDependencies(bNew ? *ActualEdge->After : *ActualEdge->Before, Id)) continue;
                        if (bNew) DestinationRefs.AddUnique(Ref);
                    } else if (bOld || !bBase || ActualEdge) continue;
                    OldRefs.Remove(Ref); NewRefs.Remove(Ref); BaseRefs.Remove(Ref);
                }
                if (!SameStrings(OldRefs, Old->GetDependencyRefs()) || !SameStrings(NewRefs, New->GetDependencyRefs())
                    || !SameStrings(BaseRefs, Base->GetDependencyRefs())) {
                    ComparableOld.Emplace(WithMembership(*Old, Old->GetItemRefs(), Old->GetOrderingRef(), &OldRefs));
                    ComparableNew.Emplace(WithMembership(*New, New->GetItemRefs(), New->GetOrderingRef(), &NewRefs));
                    ComparableBase.Emplace(WithMembership(*Base, Base->GetItemRefs(), Base->GetOrderingRef(), &BaseRefs));
                    OldShape = &*ComparableOld; NewShape = &*ComparableNew; BaseShape = &*ComparableBase;
                    DestinationRefs.Sort();
                    DependencyOwner.Emplace(WithMembership(*Base, Base->GetItemRefs(), Base->GetOrderingRef(), &DestinationRefs));
                }
            }
            bScopedMembers = SameDeclaration(*OldShape, *NewShape, false) && SameDeclaration(*OldShape, *BaseShape, false);
            if (!bScopedMembers)
                return Fail(Error, TEXT("The committed declaration overlaps a different workspace context; use incremental or full export."));
            ScopedMembership.Add(Id, {*Old, *New});
        }
        const auto* MergedDeclaration = DependencyOwner ? &*DependencyOwner : Base;
        TMap<FString, FUeAgentCanonicalValueRef> Values = Base->GetValues();
        for (const auto& Pair : Old->GetValues()) if (!New->HasValue(Pair.Key)) Values.Remove(Pair.Key);
        for (const auto& Pair : New->GetValues()) {
            const auto* OldField = Old->FindValue(Pair.Key);
            const auto* BaseField = Base->FindValue(Pair.Key);
            if (!OldField) { Values.Add(Pair.Key, Pair.Value); continue; }
            bool bSame = false;
            if (!Equal(*OldField, Pair.Value, Resources, bSame, Error)) return false;
            if (bSame) continue;
            if (BaseField && Old->GetUnitKind() == ESemanticUnitKind::DependencyState
                && Old->GetRole() == TEXT("asset_dependencies") && Old->GetSemanticTypeRef() == TEXT("uai.authoring.dependencies/1")
                && Pair.Key == TEXT("asset_paths") && Old->GetValueCount() == 1 && New->GetValueCount() == 1
                && SameDeclaration(*Old, *New) && SameDeclaration(*Old, *Base)) {
                bool bAtBefore = false, bAtAfter = false;
                if (!Equal(*OldField, *BaseField, Resources, bAtBefore, Error)
                    || !Equal(Pair.Value, *BaseField, Resources, bAtAfter, Error)) return false;
                const auto* Target = FindBefore(Old->GetOwnerRef());
                if ((!bAtBefore && !bAtAfter) || (Target && Target->GetSemanticTypeRef() == TEXT("ue.blueprint.asset"))) {
                    ScopedDependencies.Add(Id, &Change);
                    continue;
                }
            }
            if (BaseField && Old->GetUnitKind() == ESemanticUnitKind::OrderingState
                && Old->GetRole() == TEXT("stable_item_order") && Old->GetSemanticTypeRef() == TEXT("uai.authoring.ordering/1")
                && Pair.Key == TEXT("item_refs") && Old->GetValueCount() == 1 && New->GetValueCount() == 1) {
                bool bAtBefore = false, bAtAfter = false;
                if (!Equal(*OldField, *BaseField, Resources, bAtBefore, Error)
                    || !Equal(Pair.Value, *BaseField, Resources, bAtAfter, Error)) return false;
                if (!bAtBefore && !bAtAfter) {
                    ScopedOrdering.Add(Id, {*Old, *New});
                    continue;
                }
            }
            FUeAgentCanonicalValuePtr Patched = Pair.Value;
            if (BaseField && !PatchValue(*OldField, Pair.Value, *BaseField, Resources, Patched, Error)) return false;
            Values.Add(Pair.Key, Patched.ToSharedRef());
        }
        Replacements.Add(Id, (bDeclarationChanged && !bScopedMembers ? *New : *MergedDeclaration).WithValues(MoveTemp(Values)));
    }
    for (const auto& Pair : ScopedMembership) {
        const auto& OldItems = Pair.Value.Key.GetItemRefs();
        const auto& NewItems = Pair.Value.Value.GetItemRefs();
        TArray<FString> Items = Baseline->FindUnit(Pair.Key)->GetItemRefs();
        const auto IsOwnedDefinition = [&](const FString& Id) {
            const auto* Unit = Replacements.Find(Id);
            if (!Unit) Unit = Baseline->FindUnit(Id);
            if (!Unit) Unit = RemovedDefinitions.Find(Id);
            return Unit && IsSharedDefinition(*Unit) && Unit->GetOwnerRef().Equals(Pair.Key, ESearchCase::CaseSensitive);
        };
        for (const auto& Id : OldItems) if (!NewItems.Contains(Id) && !IsOwnedDefinition(Id)) {
            const auto* Member = ObservedRemovedUnits.Find(Id);
            if (!Member || !Member->GetOwnerRef().Equals(Pair.Key, ESearchCase::CaseSensitive))
                return Fail(Error, TEXT("A scoped membership removal lacks an actual owned removed unit."));
            Items.Remove(Id);
        }
        for (const auto& Id : NewItems) if (!OldItems.Contains(Id)) {
            if (!IsOwnedDefinition(Id)) {
                const auto* Member = Replacements.Find(Id);
                if (!Member || FindBefore(Id) || !Member->GetOwnerRef().Equals(Pair.Key, ESearchCase::CaseSensitive))
                    return Fail(Error, TEXT("A scoped membership addition lacks an actual owned new unit."));
            }
            Items.AddUnique(Id);
        }
        // Removals are decided from actual users below, not the root's owning
        // list or a subset capture's absence.
        auto& Owner = Replacements.FindChecked(Pair.Key);
        Owner = WithMembership(Owner, MoveTemp(Items), Owner.GetOrderingRef());
    }
    for (const auto& Pair : ScopedOrdering) {
        const auto& Old = Pair.Value.Key;
        const auto& New = Pair.Value.Value;
        const auto* OwnerChange = ScopedMembership.Find(Old.GetOwnerRef());
        const auto* Base = Baseline->FindUnit(Pair.Key);
        const auto* BaseOwner = Baseline->FindUnit(Old.GetOwnerRef());
        const auto* Owner = Replacements.Find(Old.GetOwnerRef());
        if (!OwnerChange || !Base || !BaseOwner || !Owner
            || OwnerChange->Key.GetOrderingRef() != Pair.Key || OwnerChange->Value.GetOrderingRef() != Pair.Key
            || !SameDeclaration(Old, New) || !SameDeclaration(Old, *Base))
            return Fail(Error, TEXT("A scoped ordering difference lacks its proven owner membership change."));
        TMap<FString, FUeAgentCanonicalValueRef> ValuesById;
        const auto ReadSortedOrder = [&](const FSemanticUnit& Unit, const TArray<FString>& Members) {
            const auto* Values = Unit.FindValue(TEXT("item_refs"));
            if (!Values || (*Values)->GetKind() != EUeAgentValueKind::Array || (*Values)->GetArrayItems().Num() != Members.Num()) return false;
            FString Previous;
            for (int32 Index = 0; Index < Members.Num(); ++Index) {
                const auto& Value = (*Values)->GetArrayItems()[Index];
                if (Value->GetKind() != EUeAgentValueKind::String || !Value->GetText().Equals(Members[Index], ESearchCase::CaseSensitive)
                    || (Index > 0 && !(Previous < Value->GetText()))) return false;
                Previous = Value->GetText(); ValuesById.Add(Previous, Value);
            }
            return true;
        };
        // This owner emits stable sorted identity order. Arbitrary author order
        // and unobserved membership changes keep the existing aggregate conflict.
        if (!ReadSortedOrder(Old, OwnerChange->Key.GetItemRefs())
            || !ReadSortedOrder(New, OwnerChange->Value.GetItemRefs()) || !ReadSortedOrder(*Base, BaseOwner->GetItemRefs()))
            return Fail(Error, TEXT("The scoped ordering is not the exact stable identity order of its owner."));
        TArray<FUeAgentCanonicalValueRef> Items;
        for (const FString& Id : Owner->GetItemRefs()) {
            const auto* Value = ValuesById.Find(Id);
            if (!Value) return Fail(Error, TEXT("A merged ordering member has no observed or accepted identity value."));
            Items.Add(*Value);
        }
        FUeAgentCanonicalValuePtr Ordered;
        FUeAgentStateSemanticError SemanticError;
        if (!FUeAgentCanonicalValue::TryArray(MoveTemp(Items), Resources, Ordered, SemanticError))
        { Error.SetFailure(MoveTemp(SemanticError)); return false; }
        Replacements.Add(Pair.Key, Base->WithValues({{TEXT("item_refs"), Ordered.ToSharedRef()}}));
    }
    if (!DefinitionRemovalCandidates.IsEmpty()) {
        // Examine the actual merged dependency graph, including untouched
        // owners in this destination baseline. No outside native values are
        // consumed: this is lifetime of immutable declaration data only.
        TSet<FString> Required;
        TArray<FString> Pending;
        const auto VisitReferences = [&](const auto& Unit) {
            const auto Visit = [&](const TArray<FString>& Refs) {
                for (const FString& Id : Refs)
                    if (DefinitionRemovalCandidates.Contains(Id) && !Required.Contains(Id)) {
                        Required.Add(Id); Pending.Add(Id);
                    }
            };
            Visit(Unit.GetDependencyRefs()); Visit(Unit.GetRelationRefs());
            // This root lists every owned definition; ownership alone must not
            // keep an otherwise unused definition alive forever.
            if (Unit.GetUnitKind() != ESemanticUnitKind::TargetState
                || !Unit.GetRole().Equals(TEXT("authoring_target"), ESearchCase::CaseSensitive)) Visit(Unit.GetItemRefs());
        };
        Baseline->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Unit) {
            if (Removed.Contains(Unit.GetUnitId()) || DefinitionRemovalCandidates.Contains(Unit.GetUnitId())) return true;
            const auto* Replacement = Replacements.Find(Unit.GetUnitId());
            if (Replacement) VisitReferences(Replacement->GetDeclaration());
            else VisitReferences(Unit);
            return true;
        });
        for (const auto& Id : AddedOrder) VisitReferences(Replacements.FindChecked(Id).GetDeclaration());
        while (!Pending.IsEmpty()) {
            const FString Id = Pending.Pop(EAllowShrinking::No);
            if (const auto* Unit = Replacements.Find(Id)) VisitReferences(Unit->GetDeclaration());
            else if (const auto Declaration = Baseline->FindDeclaration(Id)) VisitReferences(Declaration.GetValue());
        }
        for (const auto& Id : DefinitionRemovalCandidates)
            if (!Required.Contains(Id)) Removed.Add(Id);
        for (const auto& Pair : RemovedDefinitions) {
            const auto* Owner = Replacements.Find(Pair.Value.GetOwnerRef());
            if (!Owner) Owner = Baseline->FindUnit(Pair.Value.GetOwnerRef());
            if (!Owner || Owner->GetUnitKind() != ESemanticUnitKind::TargetState
                || !Owner->GetRole().Equals(TEXT("authoring_target"), ESearchCase::CaseSensitive)) continue;
            TArray<FString> Items = Owner->GetItemRefs();
            if (Removed.Contains(Pair.Key)) Items.Remove(Pair.Key);
            else Items.AddUnique(Pair.Key);
            const FString OwnerId = Owner->GetUnitId();
            Replacements.Add(OwnerId, WithMembership(*Owner, MoveTemp(Items), Owner->GetOrderingRef()));
        }
    }
    // A scoped empty order binds a partial observation to the destination.
    // Full composition emits no such row once its last actual member has left.
    for (auto& Pair : Replacements) {
        auto& Owner = Pair.Value;
        if (!Owner.GetItemRefs().IsEmpty() || Owner.GetOrderingRef().IsEmpty() || Removed.Contains(Pair.Key)) continue;
        const auto* Previous = FindBefore(Pair.Key);
        if (!Previous || Previous->GetItemRefs().IsEmpty() || Previous->GetOrderingRef() != Owner.GetOrderingRef()) continue;
        const auto* Ordering = FindBefore(Owner.GetOrderingRef());
        if (!Ordering || !IsStableItemOrdering(*Ordering, Pair.Key)) continue;
        const auto* CurrentOrder = Replacements.Find(Owner.GetOrderingRef());
        if (CurrentOrder && !SameDeclaration(*Ordering, *CurrentOrder)) continue;
        const auto* Items = (CurrentOrder ? CurrentOrder : Ordering)->FindValue(TEXT("item_refs"));
        if (!Items || (*Items)->GetKind() != EUeAgentValueKind::Array || !(*Items)->GetArrayItems().IsEmpty()) continue;
        const bool bUnconfirmedRemoval = Previous->GetItemRefs().ContainsByPredicate([&](const FString& Id) {
            const auto* Actual = ActualById.FindRef(Id);
            if (!Actual || !Actual->Before || Actual->Before->GetOwnerRef() != Pair.Key) return true;
            if (IsSharedDefinition(*Actual->Before)) return !Removed.Contains(Id);
            return Actual->After && Actual->After->GetOwnerRef() == Pair.Key;
        });
        if (bUnconfirmedRemoval) continue;
        Removed.Add(Owner.GetOrderingRef());
        Owner = WithMembership(Owner, {}, {});
    }
    TArray<FSemanticUnit> BaselineContributors, MergedContributors;
    if (!ScopedDependencies.IsEmpty()) {
        // Inspect declaration shapes. Fetch only rows that may have contributed a
        // direct path on either side, from the already accepted destination.
        const bool bContributorsAvailable = Baseline->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration) {
            const FString& Id = Declaration.GetUnitId();
            const auto* Replacement = Replacements.Find(Id);
            const auto Kind = Declaration.GetUnitKind();
            if (Kind != ESemanticUnitKind::TargetState && Kind != ESemanticUnitKind::ItemState && Kind != ESemanticUnitKind::PropertySet) return true;
            const FSemanticUnit* Row = nullptr;
            if (Declaration.HasDirectAssetPathField()) {
                Row = Baseline->FindUnit(Id);
                if (!Row) return false;
                BaselineContributors.Add(*Row);
            }
            if (!Removed.Contains(Id)) {
                if (Replacement && Replacement->GetDeclaration().HasDirectAssetPathField()) MergedContributors.Add(*Replacement);
                else if (!Replacement && Row) MergedContributors.Add(*Row);
            }
            return true;
        });
        if (!bContributorsAvailable) return Fail(Error, TEXT("A required accepted dependency contributor is unavailable."));
        for (const auto& Id : AddedOrder) {
            const auto& Unit = Replacements.FindChecked(Id);
            if (Unit.GetDeclaration().HasDirectAssetPathField()) MergedContributors.Add(Unit);
        }
    }
    for (const auto& Pair : ScopedDependencies) {
        const auto& Actual = *Pair.Value;
        const auto& Side = Actual.After ? *Actual.After : *Actual.Before;
        const auto* Base = Baseline->FindUnit(Pair.Key);
        const auto* Target = Baseline->FindUnit(Side.GetOwnerRef());
        TArray<FString> BaselinePaths, RemovedContributors, AddedContributors, MergedPaths;
        if (!Base || !Target
            || !TryCollectCompositionDependencyPaths(*Target, BaselineContributors, BaselinePaths)
            || !TryCollectCompositionDependencyPaths(*Target, ObservedBefore, RemovedContributors)
            || !TryCollectCompositionDependencyPaths(*Target, ObservedAfter, AddedContributors)
            || !TryCollectCompositionDependencyPaths(*Target, MergedContributors, MergedPaths))
            return Fail(Error, TEXT("The scoped dependency projection has no lossless composition contract."));
        TMap<FString, FUeAgentCanonicalValueRef> ValuesByPath;
        const auto ReadPaths = [&](const FSemanticUnit& Unit, TArray<FString>& Paths) {
            const auto* Value = Unit.FindValue(TEXT("asset_paths"));
            if (!Value || (*Value)->GetKind() != EUeAgentValueKind::Array) return false;
            FString Previous;
            for (const auto& Item : (*Value)->GetArrayItems()) {
                if (Item->GetKind() != EUeAgentValueKind::String
                    || (!Previous.IsEmpty() && !(Previous < Item->GetText()))) return false;
                Previous = Item->GetText(); Paths.Add(Previous); ValuesByPath.Add(Previous, Item);
            }
            return true;
        };
        TArray<FString> AcceptedPaths, OldPaths, NewPaths;
        if (!ReadPaths(*Base, AcceptedPaths) || (Actual.Before && !ReadPaths(*Actual.Before, OldPaths))
            || (Actual.After && !ReadPaths(*Actual.After, NewPaths)) || !SameStrings(AcceptedPaths, BaselinePaths))
            return Fail(Error, TEXT("The accepted dependency set differs from its complete composition contributors."));
        for (const auto& Path : OldPaths) if (!NewPaths.Contains(Path) && !RemovedContributors.Contains(Path))
            return Fail(Error, TEXT("A scoped dependency removal lacks an actually observed contributor change."));
        for (const auto& Path : NewPaths) if (!OldPaths.Contains(Path) && !AddedContributors.Contains(Path))
            return Fail(Error, TEXT("A scoped dependency addition lacks an actually observed contributor change."));
        TArray<FUeAgentCanonicalValueRef> Values;
        for (const auto& Path : MergedPaths) {
            const auto* Value = ValuesByPath.Find(Path);
            if (!Value) return Fail(Error, TEXT("A merged dependency has no accepted or observed path value."));
            Values.Add(*Value);
        }
        if (Actual.Before.IsSet() != Actual.After.IsSet()) {
            auto* Owner = Replacements.Find(Side.GetOwnerRef());
            if (!Owner || !Target->GetDependencyRefs().Contains(Pair.Key))
                return Fail(Error, TEXT("The reprojected asset dependency has no verified target binding."));
            auto Refs = Owner->GetDependencyRefs();
            if (MergedPaths.IsEmpty()) Refs.Remove(Pair.Key); else Refs.AddUnique(Pair.Key);
            Refs.Sort();
            *Owner = WithMembership(*Owner, Owner->GetItemRefs(), Owner->GetOrderingRef(), &Refs);
        }
        if (MergedPaths.IsEmpty()) { Removed.Add(Pair.Key); continue; }
        FUeAgentCanonicalValuePtr Value;
        FUeAgentStateSemanticError SemanticError;
        if (!FUeAgentCanonicalValue::TryArray(MoveTemp(Values), Resources, Value, SemanticError))
        { Error.SetFailure(MoveTemp(SemanticError)); return false; }
        Replacements.Add(Pair.Key, Base->WithValues({{TEXT("asset_paths"), Value.ToSharedRef()}}));
        UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=committed_dependency_projection accepted_paths=%d scoped_before_paths=%d scoped_after_paths=%d merged_paths=%d"),
            AcceptedPaths.Num(), OldPaths.Num(), NewPaths.Num(), MergedPaths.Num());
    }
    TArray<FString> Targets = Baseline->GetTargetUnitIds();
    if (!SameStrings(OldTargets, NewTargets)) {
        for (const auto& Id : OldTargets) if (!NewTargets.Contains(Id)) Targets.Remove(Id);
        for (const auto& Id : NewTargets) if (!OldTargets.Contains(Id)) Targets.AddUnique(Id);
    }
    // Copy only these K before handles before mutating Destination.Changes;
    // its map may relocate, and its existing entries keep their original
    // publication-before across multiple commits.
    TArray<FCommittedUnitChange> Pending;
    const auto AddPending = [&](const FString& Id, TOptional<FSemanticUnit> After) {
        TOptional<FSemanticUnit> Before;
        if (const auto* Unit = FindBefore(Id)) Before = *Unit;
        Pending.Add({Id, MoveTemp(Before), MoveTemp(After)});
    };
    for (const auto& Id : Removed) AddPending(Id, {});
    for (auto& Pair : Replacements) if (!Removed.Contains(Pair.Key)) AddPending(Pair.Key, MoveTemp(Pair.Value));
    for (auto& Change : Pending) Destination.Set(Change.UnitId, MoveTemp(Change.Before), MoveTemp(Change.After));
    UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=committed_destination_presence actual_rows=%d before_facts=%d member_absent=%d ancestor_absent=%d definition_absent=%d"),
        Delta.Changes.Num(), BeforeFacts.Num(), MembershipAbsences, AncestorAbsences, DefinitionAbsences);
    Destination.Targets = MoveTemp(Targets); Destination.Revision = Revision;
    return true;
}

bool TryFoldCommittedWorkspaceDeltas(const FAuthoringModel& Published,
    TConstArrayView<TSharedPtr<const FCommittedWorkspaceDelta>> Records,
    FUeAgentResourceTracker& Resources, FCommittedWorkspaceDelta& Out, FWorkspaceError& Error)
{
    FDestinationChanges Destination(Published);
    for (const auto& Record : Records) {
        if (!Record) return Fail(Error, TEXT("The selected committed record has no actual difference."));
        if (!TryFoldCommittedDifference(Destination, *Record, Resources, Error)) return false;
    }
    return Destination.Seal(Resources, Out, Error);
}

}
