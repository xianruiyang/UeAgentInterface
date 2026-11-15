#include "Composition/HjsonImport/UeAgentSemanticWorkspaceRebase.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::HjsonImport
{
	using namespace HjsonWorkspace;
	using namespace StateSemantic;

	namespace
	{
		FString ChildPath(const FString& Path, const FString& Key)
		{
			return Path + TEXT("/") + EscapeJsonPointerSegment(Key);
		}

		enum class EBindingComparison { Complete, OwnerIdentity };

		const TCHAR* BindingDifference(const FSemanticUnit& A, const FSemanticUnit& B,
			const EBindingComparison Purpose = EBindingComparison::Complete)
		{
			if (A.GetUnitId() != B.GetUnitId()) return TEXT("unit");
			if (A.GetUnitKind() != B.GetUnitKind()) return TEXT("unit kind");
			if (A.GetOwnerRef() != B.GetOwnerRef()) return TEXT("owner");
			if (A.GetSemanticTypeRef() != B.GetSemanticTypeRef()) return TEXT("semantic type");
			if (A.GetRole() != B.GetRole()) return TEXT("role");
			if (A.GetFieldSetRef() != B.GetFieldSetRef()) return TEXT("field set");
			if (A.GetTargetIdentityKey() != B.GetTargetIdentityKey()) return TEXT("target");
			if (Purpose == EBindingComparison::Complete && A.GetDependencyRefs() != B.GetDependencyRefs()) return TEXT("dependencies");
			if (A.GetOrderingRef() != B.GetOrderingRef()) return TEXT("ordering");
			if (A.GetLayoutRef() != B.GetLayoutRef()) return TEXT("layout");
			return nullptr;
		}

		FUeAgentCanonicalValuePtr Field(const FUeAgentCanonicalValuePtr& Value, const FString& Key)
		{
			if (Value && Value->GetKind() == EUeAgentValueKind::Object)
				for (const auto& Item : Value->GetObjectItems())
					if (Item.Key.Equals(Key, ESearchCase::CaseSensitive)) return Item.Value;
			return nullptr;
		}

		struct FRebase
		{
			const FResolvedSemanticWorkspaceRef& Source;
			const FAuthoringModelRef& Current;
			FUeAgentResourceTracker& Resources;
			const FScopedCurrentCoverage* Coverage = nullptr;
			TArray<Contract::FDiagnostic> Issues;
			Contract::FDiagnostic EvaluationFailure;
			TSet<FString> CheckedDependencies;
			bool bKeepNewAuthorLeaves = false;

			bool Equal(const FString& Id, const FString& Path, const FUeAgentCanonicalValuePtr& A, const FUeAgentCanonicalValuePtr& B)
			{
				if (!A || !B) return A == B;
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError Error;
				if (!TryCompareValues(A.ToSharedRef(), B.ToSharedRef(), Path, Resources, Comparison, Error))
				{ AddIssue(Id, Path, MoveTemp(Error), true); return false; }
				return Comparison.IsEquivalent();
			}

			void AddIssue(const FString& Id, const FString& Path, Contract::FDiagnostic Item, bool bEvaluationFailure = false)
			{
				if (Item.SemanticPointer.IsEmpty()) Item.SemanticPointer = Path.IsEmpty() ? TEXT("/") : Path;
				const auto* Location = Source->FindValueSource(Id, Path);
				const bool bExact = Location != nullptr;
				if (!Location) Location = Source->FindUnitSource(Id);
				if (Location && Item.Source.SourceFile.IsEmpty())
				{
					Item.Source.SourceUnit = Location->GetSourceUnit();
					Item.Source.SourceFile = Location->GetSourceUnit();
					Item.Source.JsonPath = Location->GetJsonPointer();
					Item.Source.Line = Location->GetLine();
					Item.Source.Column = Location->GetColumn();
					Item.SourceKind = bExact ? TEXT("authored_value") : TEXT("related_input");
				}
				const auto* Unit = Source->GetModel()->FindUnit(Id);
				if (!Unit) Unit = Current->FindUnit(Id);
				if (Unit) Item.SemanticId = Unit->GetSemanticTypeRef();
				if (bEvaluationFailure) EvaluationFailure.Append(Item);
				else Issues.Add(MoveTemp(Item));
			}

			void Issue(const FString& Id, const FString& Path, const FString& Code, const FString& Reason,
				const FUeAgentCanonicalValuePtr& Before = nullptr, const FUeAgentCanonicalValuePtr& Now = nullptr)
			{
				auto Item = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_rebase"));
				Item.Source.JsonPath = Path;
				Item.ExpectedValuePresent = Before.IsValid();
				Item.ActualValuePresent = Now.IsValid();
				auto CaptureValue = [&](const FUeAgentCanonicalValuePtr& Value, TSharedPtr<FJsonValue>& Destination)
				{
					if (!Value) return;
					Contract::FDiagnostic Error;
					if (!CanonicalJson::TryToJsonValue(*Value, Destination, Error)) Item.Diagnostics.Add(MoveTemp(Error));
				};
				CaptureValue(Before, Item.ExpectedValue);
				CaptureValue(Now, Item.ActualValue);
				AddIssue(Id, Path, MoveTemp(Item));
			}

			void Descriptor(const FUeAgentCanonicalValuePtr& Schema,
				FUeAgentCanonicalValuePtr& Type, FUeAgentCanonicalValuePtr& Definition)
			{
				Type = Field(Schema, TEXT("type"));
				Definition = Field(Schema, TEXT("value_definition"));
				// Domain records declare members directly; reflected values carry a
				// type/value_definition pair. Both refer to the same field catalog.
				if (!Definition && Field(Schema, TEXT("fields"))) Definition = Schema;
				if (Schema && Schema->GetKind() == EUeAgentValueKind::String)
					if (const auto* Unit = Current->FindUnit(Schema->GetText()))
					{
						if (const auto* Value = Unit->GetValues().Find(TEXT("type"))) Type = *Value;
						if (const auto* Value = Unit->GetValues().Find(TEXT("value_definition"))) Definition = *Value;
					}
			}

			// Merge declared records, not arbitrary JSON shapes. Plain document objects
			// expose named members; reflected structs expose them under "fields".
			// Without a member descriptor, that member remains atomic (in particular
			// arrays, references and tagged values never acquire guessed identities).
			bool Merge(const FString& Id, const FString& Path, const FUeAgentCanonicalValuePtr& Base,
				const FUeAgentCanonicalValuePtr& Now, const FUeAgentCanonicalValuePtr& Edit,
				const FUeAgentCanonicalValuePtr& Type, const FUeAgentCanonicalValuePtr& Definition,
				FUeAgentCanonicalValuePtr& Out)
			{
				FWorkspaceError CompareError;
				auto Compare = [&](const auto& A, const auto& B, CanonicalJson::FValueComparison& Result) {
					if (!A || !B)
					{
						if (A == B) { Result = {}; return true; }
						TSharedPtr<FJsonValue> Left, Right;
						Contract::FDiagnostic Error;
						if ((A && !CanonicalJson::TryToJsonValue(*A, Left, Error))
							|| (B && !CanonicalJson::TryToJsonValue(*B, Right, Error)))
						{ CompareError.SetFailure(Error, Id); return false; }
						Result = CanonicalJson::FValueComparison::Different(FString(), Left, Right);
						return true;
					}
					return TryCompareWorkspaceValues(*Current, Type.Get(), Definition.Get(),
						A.ToSharedRef(), B.ToSharedRef(), Result, CompareError);
				};
				CanonicalJson::FValueComparison EditedDifference, CurrentDifference, ConvergedDifference;
				if (!Compare(Base, Edit, EditedDifference) || !Compare(Base, Now, CurrentDifference)
					|| !Compare(Now, Edit, ConvergedDifference))
				{ AddIssue(Id, Path, CompareError.GetDiagnostic(), true); return false; }
				if (EditedDifference.bEquivalent) { Out = Now; return true; }
				if (CurrentDifference.bEquivalent || ConvergedDifference.bEquivalent) { Out = Edit; return true; }
				const auto Kind = Field(Type, TEXT("kind"));
				const bool bStruct = Kind && Kind->GetKind() == EUeAgentValueKind::String
					&& (Kind->GetText() == TEXT("struct") || Kind->GetText() == TEXT("instanced_struct")
						|| Kind->GetText() == TEXT("property_bag"));
				const auto SchemaFields = Field(Definition, TEXT("fields"));
				const auto CollectionSemantics = Field(Definition, TEXT("collection_semantics"));
				const bool bCompleteMembership = CollectionSemantics && CollectionSemantics->GetKind() == EUeAgentValueKind::String
					&& CollectionSemantics->GetText() == TEXT("keyed_desired_state");
				const bool bObject = !bCompleteMembership && ((Kind && Kind->GetKind() == EUeAgentValueKind::String
					&& Kind->GetText() == TEXT("object")) || (!Kind && SchemaFields && SchemaFields->GetKind() == EUeAgentValueKind::Object));
				const auto BaseFields = bObject ? Base : Field(Base, TEXT("fields"));
				const auto NowFields = bObject ? Now : Field(Now, TEXT("fields"));
				const auto EditFields = bObject ? Edit : Field(Edit, TEXT("fields"));
				auto SameShell = [&](const FUeAgentCanonicalValuePtr& A, const FUeAgentCanonicalValuePtr& B)
				{
					if (!A || !B || A->GetKind() != EUeAgentValueKind::Object || B->GetKind() != EUeAgentValueKind::Object) return false;
					for (const auto& Pair : A->GetObjectItems())
						if (Pair.Key != TEXT("fields") && !Equal(Id, ChildPath(Path, Pair.Key), Pair.Value, Field(B, Pair.Key))) return false;
					for (const auto& Pair : B->GetObjectItems())
						if (Pair.Key != TEXT("fields") && !Equal(Id, ChildPath(Path, Pair.Key), Pair.Value, Field(A, Pair.Key))) return false;
					return true;
				};
				if ((bObject || (bStruct && SchemaFields && SchemaFields->GetKind() == EUeAgentValueKind::Object))
					&& BaseFields && NowFields && EditFields && BaseFields->GetKind() == EUeAgentValueKind::Object
					&& NowFields->GetKind() == EUeAgentValueKind::Object && EditFields->GetKind() == EUeAgentValueKind::Object
					&& (bObject || (SameShell(Base, Now) && SameShell(Base, Edit))))
				{
					TArray<FString> Keys;
					for (const auto& Value : {BaseFields, NowFields, EditFields})
						for (const auto& Item : Value->GetObjectItems())
							if (!Keys.ContainsByPredicate([&](const FString& Key) { return Key.Equals(Item.Key, ESearchCase::CaseSensitive); }))
								Keys.Add(Item.Key);
					Keys.Sort();
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Values;
					bool bOk = true;
					for (const FString& Key : Keys)
					{
						FUeAgentCanonicalValuePtr Value;
						FUeAgentCanonicalValuePtr MemberType, MemberDefinition;
						Descriptor(Field(SchemaFields, Key), MemberType, MemberDefinition);
						bOk &= Merge(Id, ChildPath(bObject ? Path : ChildPath(Path, TEXT("fields")), Key),
							Field(BaseFields, Key), Field(NowFields, Key), Field(EditFields, Key), MemberType, MemberDefinition, Value);
						if (Value) Values.Emplace(Key, Value.ToSharedRef());
					}
					FUeAgentStateSemanticError Error;
					FUeAgentCanonicalValuePtr MergedFields;
					if (!bOk) return false;
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Values), Resources, MergedFields, Error))
					{ AddIssue(Id, Path, MoveTemp(Error), true); return false; }
					if (bObject) { Out = MergedFields; return true; }
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Shell;
					for (const auto& Pair : Now->GetObjectItems()) Shell.Emplace(Pair.Key, Pair.Key == TEXT("fields") ? MergedFields.ToSharedRef() : Pair.Value);
					if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Shell), Resources, Out, Error))
					{ AddIssue(Id, Path, MoveTemp(Error), true); return false; }
					return true;
				}
				if (!EvaluationFailure.IsEmpty()) return false;
				if (bKeepNewAuthorLeaves) { Out = Edit; return true; }
				auto Conflict = Contract::FDiagnostic::Failure(TEXT("workspace_edit_conflict"),
					TEXT("The current value and the authored edit both changed from the baseline."), TEXT("workspace_rebase"));
				CurrentDifference.WithPrefix(Path).ApplyTo(Conflict);
				// The proposed edit can differ at another member of an atomic value.
				// Retain both source results instead of looking up an unrelated index or comparing again.
				if (CurrentDifference.Path == EditedDifference.Path && EditedDifference.ActualValue.IsValid())
					Conflict.ProposedValue = EditedDifference.ActualValue;
				else
				{
					auto Proposed = Contract::FDiagnostic::Failure(TEXT("workspace_proposed_edit"),
						TEXT("The authored edit relative to the baseline."), TEXT("workspace_rebase"));
					EditedDifference.WithPrefix(Path).ApplyTo(Proposed);
					Conflict.Diagnostics.Add(MoveTemp(Proposed));
				}
				AddIssue(Id, Path + CurrentDifference.Path, MoveTemp(Conflict));
				return false;
			}

			void Differences(const FString& Id, const FString& Path, const FUeAgentCanonicalValuePtr& Base,
				const FUeAgentCanonicalValuePtr& Now, const FString& Code, const FString& Reason)
			{
				if (!Base || !Now)
				{ if (Base != Now) Issue(Id, Path, Code, Reason, Base, Now); return; }
				FUeAgentSemanticComparison Compared;
				FUeAgentStateSemanticError Error;
				if (!TryCompareValues(Base.ToSharedRef(), Now.ToSharedRef(), Path.IsEmpty() ? TEXT("/") : Path, Resources, Compared, Error))
				{ AddIssue(Id, Path, Error, true); return; }
				for (const auto& Difference : Compared.GetDifferences())
					Issue(Id, Difference.GetPublicPath(), Code, Reason, Difference.GetPublicLeft(), Difference.GetPublicRight());
			}

			FUeAgentCanonicalValuePtr Values(const FSemanticUnit& Unit)
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
				for (const auto& Pair : Unit.GetValues()) Fields.Emplace(Pair.Key, Pair.Value);
				FUeAgentCanonicalValuePtr Result;
				FUeAgentStateSemanticError Error;
				if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Fields), Resources, Result, Error))
					AddIssue(Unit.GetUnitId(), FString(), MoveTemp(Error), true);
				return Result;
			}

			void Changed(const FString& Id, const FString& Code, const FString& Reason)
			{
				const auto* Base = Source->FindBaselineUnit(Id);
				const auto* Now = Current->FindUnit(Id);
				const int32 Before = Issues.Num();
				if (Base && Now)
				{
					Differences(Id, FString(), Values(*Base), Values(*Now), Code, Reason);
					// Definition ids are content-addressed. A new type usually means a
					// different dependency id, not changed values in the old definition.
					// Diagnose the declared contracts at the edited property's location.
					const FSemanticUnit* BeforeDefinition = nullptr;
					const FSemanticUnit* CurrentDefinition = nullptr;
					for (const FString& Ref : Base->GetDependencyRefs())
						if (const auto* Unit = Source->FindBaselineUnit(Ref); Unit && Unit->GetRole() == TEXT("property_definition"))
						{ BeforeDefinition = Unit; break; }
					for (const FString& Ref : Now->GetDependencyRefs())
						if (const auto* Unit = Current->FindUnit(Ref); Unit && Unit->GetRole() == TEXT("property_definition"))
						{ CurrentDefinition = Unit; break; }
					if (BeforeDefinition && CurrentDefinition)
						Differences(Id, FString(), Values(*BeforeDefinition), Values(*CurrentDefinition), Code, Reason);
				}
				if (Issues.Num() == Before && EvaluationFailure.IsEmpty()) Issue(Id, FString(), Code, Reason);
			}

			bool Hash(const FSemanticUnit& Unit, FString& OutHash)
			{
				FWorkspaceError Error;
				if (TryHashSemanticUnitRow(Unit, OutHash, Error)) return true;
				AddIssue(Unit.GetUnitId(), Error.GetJsonPointer(), Error.GetDiagnostic(), true);
				return false;
			}

			void Dependencies(const FSemanticUnit& Unit)
			{
				TArray<FString> Pending = Unit.GetDependencyRefs();
				if (HasSemanticFieldSetReference(Unit.GetUnitKind(), Unit.GetSemanticTypeRef(), Unit.GetFieldSetRef()))
					Pending.Add(Unit.GetFieldSetRef());
				// Child inventories, relation inventories, ordering and layout are owned
				// state, not value dependencies. Replaying an edit must retain their current
				// units; only explicit dependency contracts are required to stay unchanged.
				// Authored structural changes are checked separately below.
				while (!Pending.IsEmpty())
				{
					const FString Id = Pending.Pop(EAllowShrinking::No);
					if (Id.IsEmpty() || CheckedDependencies.Contains(Id)) continue;
					CheckedDependencies.Add(Id);
					const FString RecordedHash = Source->ReadBaselineUnitHash(Id);
					const auto* Base = Source->FindBaselineUnit(Id);
					const auto* Now = Current->FindUnit(Id);
					if (Coverage && !Coverage->ValueUnits.Contains(Id) && (!RecordedHash.IsEmpty() || Now || Source->GetModel()->FindUnit(Id)))
					{ Changed(Id, TEXT("workspace_current_scope_incomplete"), TEXT("The dependency was not observed in the current capture scope.")); continue; }
					// A field-set may be a native field name rather than a semantic unit ref.
					if (RecordedHash.IsEmpty() && !Now && !Source->GetModel()->FindUnit(Id)) continue;
					bool bEquivalent = false;
					if (Base && Now) {
						FWorkspaceError Error;
						if (!TryAreSemanticUnitsEquivalent(*Base, *Now, Resources, bEquivalent, Error)) {
							AddIssue(Id, Error.GetJsonPointer(), Error.GetDiagnostic(), true); continue;
						}
					} else if (!RecordedHash.IsEmpty() && Now) {
						// Older envelopes without captured values retain their exact
						// recorded-row fallback; never invent an unobserved baseline.
						FString CurrentHash;
						if (!this->Hash(*Now, CurrentHash)) continue;
						bEquivalent = RecordedHash.Equals(CurrentHash, ESearchCase::CaseSensitive);
					}
					if (!bEquivalent)
						Changed(Id, TEXT("workspace_dependency_changed"), TEXT("A dependency required by this edit changed or was removed since export."));
					if (Now) Dependencies(*Now);
				}
				// Owner identity must survive; an unrelated sibling added to its inventory
				// does not change this unit's meaning and is retained from Current.
				if (!Unit.GetOwnerRef().IsEmpty())
				{
					const auto* OldOwner = Source->GetModel()->FindUnit(Unit.GetOwnerRef());
					const auto* NewOwner = Current->FindUnit(Unit.GetOwnerRef());
					// A binding-only observation identifies the owner. The edited unit's
					// own value dependencies were checked above; siblings can contribute
					// other dependencies to the owner outside this observation.
					const auto Purpose = Coverage && !Coverage->ValueUnits.Contains(Unit.GetOwnerRef())
						? EBindingComparison::OwnerIdentity : EBindingComparison::Complete;
					if (Coverage && !Coverage->BindingUnits.Contains(Unit.GetOwnerRef()))
					{ Issue(Unit.GetUnitId(), FString(), TEXT("workspace_current_scope_incomplete"), TEXT("The current owner identity was not observed.")); return; }
					if (!OldOwner || !NewOwner)
						Issue(Unit.GetUnitId(), FString(), TEXT("workspace_owner_changed"), TEXT("The edited unit's owner is missing from the authored or current asset."));
					else if (const TCHAR* Field = BindingDifference(*OldOwner, *NewOwner, Purpose))
						Issue(Unit.GetUnitId(), FString(), TEXT("workspace_owner_changed"),
							FString::Printf(TEXT("The edited unit's owner changed its %s binding since export."), Field));
				}
			}

			bool Fail(FSemanticWorkspaceImportError& Error, const FString& Reason = TEXT("workspace_edit_conflict"))
			{
				if (!EvaluationFailure.IsEmpty())
				{
					Error = EvaluationFailure;
					Error.Diagnostics.Append(Issues);
					Error.AssetApplied = false;
					return false;
				}
				Error = Contract::FDiagnostic::Failure(TEXT("hjson_import_stale_base_revision"),
					TEXT("The asset changed since export; these edits or their dependencies cannot be replayed unchanged."), TEXT("workspace_rebase"));
				Error.CauseCode = Reason;
				Error.AssetApplied = false;
				Error.RetryImport = false;
				Error.RecoveryAction = TEXT("Preserve edits; export the current asset to a new workspace and resolve the reported conflicts.");
				Error.Diagnostics = Issues;
				return false;
			}
		};
	}

	bool TryMergeAcceptedAuthoringValues(const FResolvedSemanticWorkspaceRef& Latest,
		const FAuthoringModelRef& ActualModel, const FSemanticUnit& Accepted,
		const FSemanticUnit& Actual, const FSemanticUnit& Authored,
		FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error)
	{
		Out.Reset();
		if (BindingDifference(Accepted, Authored) || BindingDifference(Accepted, Actual)
			|| Authored.GetUnitKind() != ESemanticUnitKind::PropertySet) {
			Error.SetSourceFailure(TEXT("workspace_committed_author_structure_conflict"),
				TEXT("The newer author input changes this committed unit's binding or structure; retain the input and resolve this commit before advancing its publication."), Authored.GetUnitId());
			return false;
		}
		FRebase Merge{Latest, ActualModel, Resources};
		Merge.bKeepNewAuthorLeaves = true;
		const bool bWrapped = Actual.GetValues().Num() == 1 && Actual.GetValues().Contains(TEXT("value"));
		const auto Payload = [&](const FSemanticUnit& Unit) -> FUeAgentCanonicalValuePtr {
			return bWrapped ? (Unit.GetValues().Contains(TEXT("value")) ? FUeAgentCanonicalValuePtr(Unit.GetValues().FindChecked(TEXT("value"))) : nullptr)
				: Merge.Values(Unit);
		};
		FUeAgentCanonicalValuePtr Type, Definition;
		for (const FString& Id : Actual.GetDependencyRefs())
			if (const auto* Unit = ActualModel->FindUnit(Id); Unit && Unit->GetRole() == TEXT("property_definition")) {
				if (const auto* Value = Unit->GetValues().Find(TEXT("type"))) Type = *Value;
				if (const auto* Value = Unit->GetValues().Find(TEXT("value_definition"))) Definition = *Value;
				break;
			}
		const auto Native = Payload(Actual), Desired = Payload(Accepted), NewAuthor = Payload(Authored);
		FUeAgentCanonicalValuePtr RestoredAccepted, RestoredAuthor, Result;
		FUeAgentStateSemanticError RestoreError;
		if (!Native || !Desired || !NewAuthor
			|| !TryRestoreSemanticPropertyValue(Accepted, *Latest->GetModel(), Native.ToSharedRef(), Desired.ToSharedRef(), Resources, RestoredAccepted, RestoreError)
			|| !TryRestoreSemanticPropertyValue(Authored, *Latest->GetModel(), Native.ToSharedRef(), NewAuthor.ToSharedRef(), Resources, RestoredAuthor, RestoreError)) {
			Error.SetFailure(RestoreError.IsEmpty() ? Contract::FDiagnostic::Failure(TEXT("workspace_committed_author_value_unavailable"),
				TEXT("The accepted, actual and newer author values cannot be restored with their declared type.")) : RestoreError, Authored.GetUnitId());
			return false;
		}
		if (!Merge.Merge(Authored.GetUnitId(), bWrapped ? TEXT("/value") : FString(), RestoredAccepted, Native,
			RestoredAuthor, Type, Definition, Result)) {
			FSemanticWorkspaceImportError Diagnostic;
			Merge.Fail(Diagnostic);
			Error.SetFailure(MoveTemp(Diagnostic), Authored.GetUnitId()); return false;
		}
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		if (bWrapped) Values.Add(TEXT("value"), Result.ToSharedRef());
		else for (const auto& Field : Result->GetObjectItems()) Values.Add(Field.Key, Field.Value);
		Out.Emplace(Actual.WithValues(MoveTemp(Values)));
		return true;
	}

	bool TryRebaseSemanticWorkspace(const FResolvedSemanticWorkspaceRef& Input, const FAuthoringModelRef& Current,
		const FUeAgentStateRevision& Revision, FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspaceRef& OutWorkspace, FSemanticWorkspaceImportError& OutError,
		const FScopedCurrentCoverage* Coverage)
	{
		OutError.Reset();
		FResolvedSemanticWorkspaceRef Source = Input;
		FWorkspaceError AcceptedError;
		const FString AcceptedRoot = GetAcceptedSemanticWorkspaceRoot();
		const auto AcceptedSource = GetAcceptedSemanticWorkspace(AcceptedRoot);
		if (AcceptedSource.Get() == &Input.Get()
			&& !TryResolveUnconsumedAuthoring(AcceptedRoot, Input, Resources, Source, AcceptedError)) {
			OutError = AcceptedError.GetDiagnostic(); return false;
		}
		OutWorkspace = Source;
		const bool bLocalState = Source->GetSourceKind() == EWorkspaceSourceKind::Directory
			&& Source->GetMode() == ESemanticWorkspaceMode::State;
		if (bLocalState && !Source->GetEditedUnitIds().IsSet()) {
			FWorkspaceError Error;
			FailWorkspaceDataflowHandoff(Error, TEXT("workspace_rebase"), TEXT("edited_unit_ids"), AcceptedRoot,
				TEXT("unset"), TEXT("the source resolver's explicit edit set"), TEXT("ReadDirectorySource / typed intent resolution"));
			OutError = Error.GetDiagnostic(); return false;
		}
		FRebase Rebase{Source, Current, Resources, Coverage};
		if (Coverage && (!Source->GetStructuralEdits().IsEmpty() || !Source->GetEditedUnitIds().IsSet())) {
			Rebase.Issue(FString(), FString(), TEXT("workspace_current_scope_incomplete"), TEXT("Structural or unknown edits require a complete current capture."));
			return Rebase.Fail(OutError);
		}
		if (Source->GetBaseStateRevision() == Revision
			&& (!Coverage || Coverage->KeyedValueScopes.IsEmpty())) {
			// A matching revision does not itself establish a partial capture's
			// declared coverage. Repeated preparation may reuse the rebase result,
			// but must still bind its edited units and dependencies to this scope.
			if (Coverage) for (const FString& Id : *Source->GetEditedUnitIds()) {
				const auto* Edit = Source->GetModel()->FindUnit(Id);
				const auto* Now = Current->FindUnit(Id);
				if (!Coverage->ValueUnits.Contains(Id) || !Edit || !Now) {
					Rebase.Issue(Id, FString(), TEXT("workspace_current_scope_incomplete"), TEXT("An authored edit is outside the actual current observation scope."));
					continue;
				}
				if (const TCHAR* Field = BindingDifference(*Edit, *Now))
					Rebase.Changed(Id, TEXT("workspace_binding_changed"), FString::Printf(TEXT("The edited unit's %s binding differs from the current asset."), Field));
				Rebase.Dependencies(*Edit);
				Rebase.Dependencies(*Now);
			}
			if (!Rebase.Issues.IsEmpty() || !Rebase.EvaluationFailure.IsEmpty()) return Rebase.Fail(OutError);
			return true;
		}
		const auto& Proposed = Source->GetModel();
		if (Proposed->GetContractId() != Current->GetContractId() || Proposed->GetContractVersion() != Current->GetContractVersion()
			|| Proposed->GetProfileId() != Current->GetProfileId())
		{
			auto Issue = Contract::FDiagnostic::Failure(TEXT("workspace_contract_changed"),
				TEXT("The authored workspace and current asset use different authoring contracts or profiles."), TEXT("workspace_rebase"));
			if (Proposed->GetContractId() != Current->GetContractId())
			{
				Issue.PropertyPath = TEXT("contract_id");
				Issue.ExpectedValue = MakeShared<FJsonValueString>(Proposed->GetContractId());
				Issue.ActualValue = MakeShared<FJsonValueString>(Current->GetContractId());
			}
			else if (Proposed->GetContractVersion() != Current->GetContractVersion())
			{
				Issue.PropertyPath = TEXT("contract_version");
				Issue.ExpectedValue = MakeShared<FJsonValueNumber>(Proposed->GetContractVersion());
				Issue.ActualValue = MakeShared<FJsonValueNumber>(Current->GetContractVersion());
			}
			else
			{
				Issue.PropertyPath = TEXT("profile_id");
				Issue.ExpectedValue = MakeShared<FJsonValueString>(Proposed->GetProfileId());
				Issue.ActualValue = MakeShared<FJsonValueString>(Current->GetProfileId());
			}
			Rebase.Issues.Add(MoveTemp(Issue));
			return Rebase.Fail(OutError, TEXT("workspace_contract_changed"));
		}
		if (!Source->GetEditedUnitIds().IsSet())
		{
			Rebase.Issue(FString(), FString(), TEXT("workspace_edit_baseline_unavailable"),
				TEXT("The workspace does not record which units were edited."));
			return Rebase.Fail(OutError, TEXT("workspace_edit_baseline_unavailable"));
		}
		TMap<FString, FSemanticUnit> Replacements;
		bool bMergedSourceValues = false;
		for (const FString& Id : *Source->GetEditedUnitIds())
		{
			if (Coverage && !Coverage->ValueUnits.Contains(Id)) {
				Rebase.Issue(Id, FString(), TEXT("workspace_current_scope_incomplete"), TEXT("An authored edit is outside the actual current observation scope.")); continue;
			}
			const auto* Edit = Proposed->FindUnit(Id);
			const auto* Now = Current->FindUnit(Id);
			TOptional<FSemanticUnit> AddressedBaseline;
			const FSemanticUnit* Base = nullptr;
			if (const auto* Keyed = Coverage ? Coverage->KeyedValueScopes.Find(Id) : nullptr) {
				FWorkspaceError Error;
				if (!Source->TryReadBaselineMembers(Id, Keyed->RootField, Keyed->ObservedKeys, Resources, AddressedBaseline, Error)) {
					Rebase.AddIssue(Id, Error.GetJsonPointer(), Error.GetDiagnostic(), true); continue;
				}
				if (AddressedBaseline.IsSet()) Base = &AddressedBaseline.GetValue();
			} else Base = Source->FindBaselineUnit(Id);
			FString Hash = Source->ReadBaselineUnitHash(Id);
			if (!Edit || !Now || Hash.IsEmpty()) { Rebase.Changed(Id, TEXT("workspace_edited_target_removed"),
				!Edit ? TEXT("The edited unit is missing from the authored workspace.") : !Now ? TEXT("The edited target was removed from the current asset.")
				: TEXT("The edited unit has no recorded baseline.")); continue; }
			const TCHAR* BindingField = BindingDifference(*Edit, *Now);
			if (!BindingField && Base) BindingField = BindingDifference(*Base, *Now);
			if (BindingField)
			{ Rebase.Changed(Id, TEXT("workspace_binding_changed"),
				FString::Printf(TEXT("The edited unit's %s binding differs from the current asset."), BindingField)); continue; }
			Rebase.Dependencies(*Edit);
			Rebase.Dependencies(*Now);
			TOptional<FSemanticUnit> ScopedCurrent, ScopedBaseline;
			if (const auto* Scope = Coverage ? Coverage->KeyedValueScopes.Find(Id) : nullptr) {
				FKeyedItemProjectionContract Contract;
				const auto* BeforeItems = Base ? Base->GetValues().Find(Scope->RootField) : nullptr;
				const auto* CurrentItems = Now->GetValues().Find(Scope->RootField);
				const auto* EditedItems = Edit->GetValues().Find(Scope->RootField);
				if (!TryGetKeyedItemProjectionContract(Scope->RootField, Contract)
					|| (Contract.bIdentityFromMapKey && !Contract.bEmitOperationRecords)
					|| !BeforeItems || !CurrentItems || !EditedItems
					|| (*BeforeItems)->GetKind() != EUeAgentValueKind::Object || (*CurrentItems)->GetKind() != EUeAgentValueKind::Object
					|| (*EditedItems)->GetKind() != EUeAgentValueKind::Object) {
					Rebase.Issue(Id, FString(), TEXT("workspace_current_scope_incomplete"), TEXT("The partial keyed scope has no complete accepted baseline and current keyed observations.")); continue;
				}
				bool bComplete = true;
				for (const auto& Item : (*EditedItems)->GetObjectItems()) bComplete &= Scope->ObservedKeys.Contains(Item.Key);
				if (!bComplete) { Rebase.Issue(Id, FString(), TEXT("workspace_current_scope_incomplete"), TEXT("An authored keyed operation is outside the declared current key scope.")); continue; }
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items, BeforeSubset;
				for (const auto& Item : (*BeforeItems)->GetObjectItems())
					if (Scope->ObservedKeys.Contains(Item.Key)) BeforeSubset.Add(Item);
				for (const auto& Item : (*CurrentItems)->GetObjectItems())
					if (Scope->ObservedKeys.Contains(Item.Key)) Items.Add(Item);
				FUeAgentCanonicalValuePtr Observed, Captured;
				FUeAgentStateSemanticError BuildError;
				if (!FUeAgentCanonicalValue::TryObject(MoveTemp(Items), Resources, Observed, BuildError)) {
					Rebase.AddIssue(Id, FString(), MoveTemp(BuildError), true); continue;
				}
				if (!FUeAgentCanonicalValue::TryObject(MoveTemp(BeforeSubset), Resources, Captured, BuildError)) {
					Rebase.AddIssue(Id, FString(), MoveTemp(BuildError), true); continue;
				}
				// Compare only the actual addressed members. Unchanged membership
				// stays in the source baseline, without copying or hashing its values.
				TMap<FString, FUeAgentCanonicalValueRef> Values = Base->GetValues();
				Values.Add(Scope->RootField, Captured.ToSharedRef());
				ScopedBaseline.Emplace(Base->WithValues(MoveTemp(Values)));
				Values = Base->GetValues();
				Values.Add(Scope->RootField, Observed.ToSharedRef());
				ScopedCurrent.Emplace(Base->WithValues(MoveTemp(Values)));
				Base = &ScopedBaseline.GetValue();
				Now = &ScopedCurrent.GetValue();
			}
			FString CurrentHash, EditHash, SemanticBaselineHash;
			if (ScopedBaseline.IsSet()) {
				bool bSame = false;
				FWorkspaceError Error;
				if (!TryAreSemanticUnitValuesEquivalent(*Base, *Now, Resources, bSame, Error)) {
					Rebase.AddIssue(Id, FString(), Error.GetDiagnostic(), true); continue;
				}
				if (bSame) { Replacements.Add(Id, *Edit); continue; }
				if (!TryAreSemanticUnitValuesEquivalent(*Edit, *Now, Resources, bSame, Error)) {
					Rebase.AddIssue(Id, FString(), Error.GetDiagnostic(), true); continue;
				}
				if (bSame) continue;
			} else {
			if (Base) {
				if (!Rebase.Hash(*Base, SemanticBaselineHash)) continue;
				Hash = SemanticBaselineHash;
			}
			if (!Rebase.Hash(*Now, CurrentHash)) continue;
			if (Hash == CurrentHash) { Replacements.Add(Id, *Edit); continue; }
			if (!Rebase.Hash(*Edit, EditHash)) continue;
			if (EditHash == CurrentHash) continue;
			}
			if (!Base) { Rebase.Issue(Id, FString(), TEXT("workspace_edit_baseline_unavailable"), TEXT("The captured baseline values for this edit are unavailable.")); continue; }
			// Composition stores native object values as their top-level fields;
			// scalar values and document property leaves use the explicit value wrapper.
			auto Wrapped = [](const FSemanticUnit& Unit) { return Unit.GetValues().Num() == 1 && Unit.GetValues().Contains(TEXT("value")); };
			const bool bWrapped = Wrapped(*Base);
			if (Wrapped(*Now) != bWrapped || Wrapped(*Edit) != bWrapped)
			{ Rebase.Changed(Id, TEXT("workspace_edit_conflict"), TEXT("The baseline, authored edit and current value disagree on whether the value is wrapped or represented as fields.")); continue; }
			FUeAgentCanonicalValuePtr Type, Definition;
			for (const FString& Dependency : Edit->GetDependencyRefs())
				if (const auto* Unit = Current->FindUnit(Dependency); Unit && Unit->GetRole() == TEXT("property_definition"))
				{
					if (const auto* Value = Unit->GetValues().Find(TEXT("type"))) Type = *Value;
					if (const auto* Value = Unit->GetValues().Find(TEXT("value_definition"))) Definition = *Value;
					break;
				}
			// A declared ItemState record has the same member semantics as a
			// PropertySet. Native topology and undeclared items remain atomic.
			const auto DeclaredFields = Field(Definition, TEXT("fields"));
			bool bDeclaredAggregateRoot = false;
			if (Edit->GetUnitKind() == ESemanticUnitKind::StructureState && !bWrapped)
				for (const auto& Value : Edit->GetValues())
				{
					FKeyedItemProjectionContract Contract;
					if (TryGetKeyedItemProjectionContract(Value.Key, Contract)
						&& !Contract.AggregatedItemSemanticSuffix.IsEmpty()
						&& (Contract.AggregatedParentSemanticSuffix.IsEmpty()
							|| Edit->GetSemanticTypeRef().EndsWith(Contract.AggregatedParentSemanticSuffix)))
					{ bDeclaredAggregateRoot = true; break; }
				}
			if ((Edit->GetUnitKind() != ESemanticUnitKind::PropertySet
				&& !bDeclaredAggregateRoot
				&& (Edit->GetUnitKind() != ESemanticUnitKind::ItemState || !DeclaredFields
					|| DeclaredFields->GetKind() != EUeAgentValueKind::Object))
				|| Base->GetItemRefs() != Edit->GetItemRefs() || Base->GetItemRefs() != Now->GetItemRefs()
				|| Base->GetRelationRefs() != Edit->GetRelationRefs() || Base->GetRelationRefs() != Now->GetRelationRefs())
			{ Rebase.Changed(Id, TEXT("workspace_structure_changed"),
				Base->GetItemRefs() != Edit->GetItemRefs() || Base->GetItemRefs() != Now->GetItemRefs()
				? TEXT("The edited unit's child inventory changed since export.")
				: Base->GetRelationRefs() != Edit->GetRelationRefs() || Base->GetRelationRefs() != Now->GetRelationRefs()
				? TEXT("The edited unit's relation inventory changed since export.")
				: TEXT("This unit does not declare mergeable property fields; its concurrent structural changes require manual reconciliation.")); continue; }
			if (bDeclaredAggregateRoot)
			{
				// Top-level collections remain atomic. Only unchanged native context
				// is refreshed; type/dependency and inventory checks above still apply.
				FUeAgentCanonicalValuePtr EmptyFields;
				FUeAgentStateSemanticError BuildError;
				if (!FUeAgentCanonicalValue::TryObject({}, Resources, EmptyFields, BuildError)
					|| !FUeAgentCanonicalValue::TryObject({{TEXT("fields"), EmptyFields.ToSharedRef()}}, Resources, Definition, BuildError))
				{ Rebase.AddIssue(Id, FString(), MoveTemp(BuildError), true); continue; }
				Type.Reset();
			}
			FUeAgentCanonicalValuePtr Merged;
			auto Payload = [&](const FSemanticUnit& Unit) -> FUeAgentCanonicalValuePtr
			{ return bWrapped ? FUeAgentCanonicalValuePtr(Unit.GetValues().FindChecked(TEXT("value"))) : Rebase.Values(Unit); };
			const auto Captured = Payload(*Base), ProposedValue = Payload(*Edit);
			FUeAgentCanonicalValuePtr RestoredEdit;
			FUeAgentStateSemanticError RestoreError;
			if (!Captured || !ProposedValue || !TryRestoreSemanticPropertyValue(*Edit, *Proposed,
				Captured.ToSharedRef(), ProposedValue.ToSharedRef(), Resources, RestoredEdit, RestoreError))
			{
				if (!RestoreError.IsEmpty()) Rebase.AddIssue(Id, RestoreError.Source.JsonPath, RestoreError, true);
				else Rebase.Issue(Id, bWrapped ? TEXT("/value") : TEXT(""), TEXT("workspace_rebase_value_invalid"),
					TEXT("The captured or proposed value required to restore this edit is missing."), Captured, ProposedValue);
				continue;
			}
			const int32 IssuesBeforeMerge = Rebase.Issues.Num();
			if (!Rebase.Merge(Id, bWrapped ? TEXT("/value") : TEXT(""), Captured, Payload(*Now), RestoredEdit, Type, Definition, Merged))
			{
				if (Rebase.Issues.Num() == IssuesBeforeMerge && Rebase.EvaluationFailure.IsEmpty()) Rebase.Issue(Id, FString(), TEXT("workspace_rebase_value_invalid"),
					TEXT("The edited value could not be merged with the current value."), Captured, ProposedValue);
				continue;
			}
			TMap<FString, FUeAgentCanonicalValueRef> Values;
			if (bWrapped) Values.Add(TEXT("value"), Merged.ToSharedRef());
			else for (const auto& Pair : Merged->GetObjectItems()) Values.Add(Pair.Key, Pair.Value);
			Replacements.Add(Id, Now->WithValues(MoveTemp(Values)));
			bMergedSourceValues = true;
		}
		for (const auto& Edit : Source->GetStructuralEdits())
		{
			TArray<FString> Affected = Edit.GetOrderedUnitIds();
			Affected.Append({Edit.GetOwnerUnitId(), Edit.GetSubjectUnitId(), Edit.GetReplacementUnitId(), Edit.GetRelationId()});
			for (const FString& Id : Affected)
			{
				if (Id.IsEmpty()) continue;
				const FString Hash = Source->ReadBaselineUnitHash(Id);
				const auto* Now = Current->FindUnit(Id);
				FString CurrentHash;
				if (!Hash.IsEmpty() && Now && !Rebase.Hash(*Now, CurrentHash)) continue;
				if (!Hash.IsEmpty() && (!Now || Hash != CurrentHash)) Rebase.Changed(Id, TEXT("workspace_structure_changed"),
					Now ? TEXT("A target of the structural edit changed since export.") : TEXT("A target of the structural edit was removed since export."));
				else if (Hash.IsEmpty() && Now) Rebase.Issue(Id, FString(), TEXT("workspace_created_identity_conflict"),
					TEXT("A unit created by this workspace already exists in the current asset."));
				if (const auto* Unit = Proposed->FindUnit(Id)) Rebase.Dependencies(*Unit);
			}
		}
		if (!Rebase.EvaluationFailure.IsEmpty() || !Rebase.Issues.IsEmpty()) return Rebase.Fail(OutError);
		if (!Proposed->IsCompiled()) {
			// Current facts/conflicts above concern the explicit edits. Keep their
			// merged values as an overlay; no second complete Units array or graph.
			const int32 Count = Replacements.Num();
			OutWorkspace = Source->Rebind(FAuthoringModel::RebasePropertySourceView(Proposed,
				Current->GetBaseRevision(), MoveTemp(Replacements)), Revision);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=property_intent_rebase replacements=%d full_compile=false complete_units_copy=false"), Count);
			return true;
		}
		if (Coverage && !bMergedSourceValues) {
			// The scoped current observation checked the edits and dependencies,
			// but did not change the complete, already compiled authoring input.
			// Bind that actual observation on the workspace; do not recompile the
			// source solely to stamp a partial capture's revision into its digest.
			OutWorkspace = Source->Rebind(Proposed, Revision);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=structured_intent_rebase compiled_source_reused=true replacements=%d full_compile=false"), Replacements.Num());
			return true;
		}
		if (bLocalState) {
			FWorkspaceError Error;
			FailWorkspaceDataflowHandoff(Error, TEXT("workspace_rebase"), TEXT("local merged-value view"), AcceptedRoot,
				TEXT("compiled source would require a complete Units copy and compile"),
				TEXT("the addressed source view with current facts"), TEXT("ReadDirectorySource / RebasePropertySourceView"));
			OutError = Error.GetDiagnostic(); return false;
		}
		TArray<FSemanticUnit> Units;
		// A scoped capture supplies actual facts only for its explicit coverage.
		// Keep the complete accepted source elsewhere, without claiming it was read.
		for (const auto& Unit : (Coverage ? Proposed : Current)->GetUnits())
		{
			const auto* Replacement = Replacements.Find(Unit.GetUnitId());
			Units.Add(Replacement ? *Replacement : Unit);
		}
		if (!Coverage) for (const auto& Unit : Proposed->GetUnits())
			if (!Source->GetBaselineUnitHashes().Contains(Unit.GetUnitId()) && !Current->FindUnit(Unit.GetUnitId())) Units.Add(Unit);
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		if (!TryCompileAuthoringModel(Current->GetContractId(), Current->GetContractVersion(), Current->GetProfileId(),
			Current->GetBaseRevision(), MoveTemp(Units), (Coverage ? Proposed : Current)->GetTargetUnitIds(), Resources, Model, Error,
			nullptr, &(Coverage ? Proposed : Current).Get(), nullptr))
		{
			OutError = Error.GetDiagnostic();
            if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("workspace_rebase");
			return false;
		}
		OutWorkspace = Source->Rebind(Model.ToSharedRef(), Revision);
		return true;
	}
}
