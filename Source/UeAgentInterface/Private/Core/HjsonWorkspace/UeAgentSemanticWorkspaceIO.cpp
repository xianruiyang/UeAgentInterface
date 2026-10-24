#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"

namespace UE::UAI::HjsonWorkspace
{
	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool NormalizeUniqueValues(TArray<FString>& Values, Contract::FDiagnostic& Error,
			const FString& Field, bool bSort)
		{
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index) {
				FString& Value = Values[Index];
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty() || Seen.Contains(Value)) {
					Error = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_invalid"), Value.IsEmpty()
						? TEXT("The workspace list contains an empty item.") : TEXT("The workspace list contains a duplicate item."), TEXT("workspace_resolve"));
					Error.SemanticPointer = TEXT("/") + Field + TEXT("/") + FString::FromInt(Index);
					if (Value.IsEmpty()) Error.ActualValue = MakeShared<FJsonValueString>(Value);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty unique item"));
					return false;
				}
				Seen.Add(Value);
			}
			if (bSort) Values.Sort();
			return true;
		}

		TSharedPtr<FJsonValue> RevisionEvidence(const FUeAgentStateRevision& Revision)
		{
			auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Value->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Value->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
			return MakeShared<FJsonValueObject>(Value);
		}

		bool TryMatchContract(const FAuthoringModel& Actual, const FAuthoringModel& Expected,
			bool bAllowVerifiedRevisionDrift, Contract::FDiagnostic& Error)
		{
			auto Fail = [&](const TCHAR* Field, const TCHAR* Reason) {
				Error = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_base_revision_mismatch"), Reason, TEXT("workspace_binding"));
				Error.PropertyPath = Field;
			};
			if (Actual.GetContractId() != Expected.GetContractId()) {
				Fail(TEXT("contract_id"), TEXT("The workspace contract does not match the current authoring model."));
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetContractId());
				Error.ActualValue = MakeShared<FJsonValueString>(Actual.GetContractId());
			} else if (Actual.GetContractVersion() != Expected.GetContractVersion()) {
				Fail(TEXT("contract_version"), TEXT("The workspace contract version does not match the current authoring model."));
				Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetContractVersion());
				Error.ActualValue = MakeShared<FJsonValueNumber>(Actual.GetContractVersion());
			} else if (Actual.GetProfileId() != Expected.GetProfileId()) {
				Fail(TEXT("profile_id"), TEXT("The workspace profile does not match the current authoring model."));
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetProfileId());
				Error.ActualValue = MakeShared<FJsonValueString>(Actual.GetProfileId());
			} else if (!bAllowVerifiedRevisionDrift && Actual.GetBaseRevision() != Expected.GetBaseRevision()) {
				Fail(TEXT("base_revision"), TEXT("The workspace base revision differs from the current authoring model."));
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetBaseRevision());
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetBaseRevision());
			} else return true;
			return false;
		}

	}

	FString LexToString(ESemanticWorkspaceMode Mode)
	{
		switch (Mode)
		{
		case ESemanticWorkspaceMode::State:
			return TEXT("state");
		case ESemanticWorkspaceMode::Seed:
			return TEXT("seed");
		case ESemanticWorkspaceMode::EditBundle:
			return TEXT("edit_bundle");
		default:
			return FString();
		}
	}

	FString LexToString(EStructuralEditKind Kind)
	{
		switch (Kind)
		{
		case EStructuralEditKind::Add:
			return TEXT("add");
		case EStructuralEditKind::Remove:
			return TEXT("remove");
		case EStructuralEditKind::Replace:
			return TEXT("replace");
		case EStructuralEditKind::Rename:
			return TEXT("rename");
		case EStructuralEditKind::AddRelation:
			return TEXT("add_relation");
		case EStructuralEditKind::RemoveRelation:
			return TEXT("remove_relation");
		case EStructuralEditKind::SetOrder:
			return TEXT("set_order");
		default:
			return FString();
		}
	}

	FStructuralEdit::FStructuralEdit(
		FString InEditId,
		EStructuralEditKind InKind,
		FString InOwnerUnitId,
		FString InSubjectUnitId,
		FString InReplacementUnitId,
		FString InRelationId,
		TArray<FString> InOrderedUnitIds,
		TOptional<FSourceProvenance> InSource)
		: EditId(Normalize(MoveTemp(InEditId)))
		, Kind(InKind)
		, OwnerUnitId(Normalize(MoveTemp(InOwnerUnitId)))
		, SubjectUnitId(Normalize(MoveTemp(InSubjectUnitId)))
		, ReplacementUnitId(
			Normalize(MoveTemp(InReplacementUnitId)))
		, RelationId(Normalize(MoveTemp(InRelationId)))
		, OrderedUnitIds(MoveTemp(InOrderedUnitIds))
		, Source(MoveTemp(InSource))
	{
		auto Invalid = [&](const TCHAR* Field, const TCHAR* Reason) {
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_structural_edit_invalid"), Reason, TEXT("workspace_resolve"));
			ValidationDiagnostic.SemanticPointer = TEXT("/") + FString(Field);
			ValidationDiagnostic.SubjectId = EditId;
		};
		if (EditId.IsEmpty()) { Invalid(TEXT("edit_id"), TEXT("The structural edit requires a nonempty edit identity.")); return; }
		if (OwnerUnitId.IsEmpty()) { Invalid(TEXT("owner_unit_id"), TEXT("The structural edit requires an owner unit reference.")); return; }
		if (LexToString(Kind).IsEmpty()) {
			Invalid(TEXT("kind"), TEXT("The structural edit kind is unsupported."));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)); return;
		}
		const bool bNeedsSubject = Kind == EStructuralEditKind::Add || Kind == EStructuralEditKind::Remove
			|| Kind == EStructuralEditKind::Rename || Kind == EStructuralEditKind::Replace;
		if (bNeedsSubject && SubjectUnitId.IsEmpty()) { Invalid(TEXT("subject_unit_id"), TEXT("This structural edit kind requires a subject unit reference.")); return; }
		if (Kind == EStructuralEditKind::Replace && ReplacementUnitId.IsEmpty()) { Invalid(TEXT("replacement_unit_id"), TEXT("Replacing a semantic unit requires a replacement unit reference.")); return; }
		if ((Kind == EStructuralEditKind::AddRelation || Kind == EStructuralEditKind::RemoveRelation) && RelationId.IsEmpty()) { Invalid(TEXT("relation_id"), TEXT("A relation edit requires a relation reference.")); return; }
		if (Kind == EStructuralEditKind::SetOrder) {
			if (OrderedUnitIds.IsEmpty()) { Invalid(TEXT("ordered_unit_ids"), TEXT("The set-order edit requires a nonempty ordered unit list.")); return; }
			if (!NormalizeUniqueValues(OrderedUnitIds, ValidationDiagnostic, TEXT("ordered_unit_ids"), false))
				ValidationDiagnostic.Code = TEXT("semantic_structural_edit_invalid");
		} else if (!OrderedUnitIds.IsEmpty()) {
			Invalid(TEXT("ordered_unit_ids"), TEXT("An ordered unit list is only supported by a set-order edit."));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(OrderedUnitIds.Num());
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(0);
		}

	}

	const FString& FStructuralEdit::GetEditId() const
	{
		return EditId;
	}

	EStructuralEditKind FStructuralEdit::GetKind() const
	{
		return Kind;
	}

	const FString& FStructuralEdit::GetOwnerUnitId() const
	{
		return OwnerUnitId;
	}

	const FString& FStructuralEdit::GetSubjectUnitId() const
	{
		return SubjectUnitId;
	}

	const FString& FStructuralEdit::GetReplacementUnitId() const
	{
		return ReplacementUnitId;
	}

	const FString& FStructuralEdit::GetRelationId() const
	{
		return RelationId;
	}

	const TArray<FString>& FStructuralEdit::GetOrderedUnitIds() const
	{
		return OrderedUnitIds;
	}

	bool FStructuralEdit::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FHjsonNode* FResolvedSemanticWorkspace::FindValueNode(
		const FString& UnitId, const FString& JsonPointer) const
	{
		const FHjsonDocumentRef* Document = UnitSourceDocuments.Find(UnitId);
		return Document ? (*Document)->Find(JsonPointer) : nullptr;
	}

	const FSourceProvenance* FResolvedSemanticWorkspace::FindValueSource(
		const FString& UnitId, const FString& JsonPointer) const
	{
		const FHjsonNode* Node = FindValueNode(UnitId, JsonPointer);
		return Node ? &Node->GetProvenance() : nullptr;
	}

	void FResolvedSemanticWorkspace::ProjectValueSources(const FString& UnitId, const FString& ValuePointer,
		const FString& DocumentPointer, TMap<FString, Contract::FSourceLocation>& OutSources) const
	{
		const FHjsonNode* Root = FindValueNode(UnitId, ValuePointer);
		if (!Root) return;
		struct FPendingSource
		{
			const FHjsonNode* Node;
			FString ValuePath;
			FString DocumentPath;
		};
		TArray<FPendingSource> Pending{{Root, ValuePointer, DocumentPointer}};
		while (!Pending.IsEmpty())
		{
			const auto Item = Pending.Pop(EAllowShrinking::No);
			const auto& Provenance = Item.Node->GetProvenance();
			Contract::FSourceLocation Source;
			Source.SourceFile = Provenance.GetSourceUnit();
			Source.JsonPath = Provenance.GetJsonPointer();
			Source.Line = Provenance.GetLine();
			Source.Column = Provenance.GetColumn();
			Source.SourceUnit = UnitId;
			Source.InternalJsonPath = Item.ValuePath;
			OutSources.Add(Item.DocumentPath.IsEmpty() ? TEXT("/") : Item.DocumentPath, MoveTemp(Source));
			for (const auto& Entry : Item.Node->GetEntries())
			{
				const FString Suffix = TEXT("/") + EscapeJsonPointerSegment(Entry.Key);
				Pending.Add({&Entry.Value.Get(), Item.ValuePath + Suffix, Item.DocumentPath + Suffix});
			}
			for (int32 Index = 0; Index < Item.Node->GetItems().Num(); ++Index)
			{
				const FString Suffix = TEXT("/") + FString::FromInt(Index);
				Pending.Add({&Item.Node->GetItems()[Index].Get(), Item.ValuePath + Suffix, Item.DocumentPath + Suffix});
			}
		}
	}

	const FSourceProvenance* FResolvedSemanticWorkspace::FindDocumentValueSource(
		const FString& DocumentIdentity, const FString& DocumentPointer,
		const TArray<FDocumentValueSourceBinding>& Bindings) const
	{
		const FSourceProvenance* Selected = nullptr;
		int32 SelectedPrefix = -1;
		for (const auto& Binding : Bindings)
		{
			if (Binding.DocumentIdentity != DocumentIdentity
				|| !(Binding.DocumentPointer.IsEmpty() || DocumentPointer == Binding.DocumentPointer
					|| DocumentPointer.StartsWith(Binding.DocumentPointer + TEXT("/")))) continue;
			const auto* Source = FindValueSource(Binding.UnitId, Binding.UnitValuePointer
				+ DocumentPointer.RightChop(Binding.DocumentPointer.Len()));
			if (Source && Binding.DocumentPointer.Len() > SelectedPrefix)
			{
				Selected = Source;
				SelectedPrefix = Binding.DocumentPointer.Len();
			}
		}
		if (Selected) return Selected;
		// Some document surfaces are authored as a single PropertySet rather than
		// decomposed document units. Its identity still names the same canonical
		// document, and its value nodes retain expanded content_ref provenance.
		for (const auto& Unit : Model->GetUnits())
		{
			if (Unit.GetUnitKind() != ESemanticUnitKind::PropertySet
				|| Unit.GetTargetIdentityKey() != DocumentIdentity) continue;
			const auto* Source = FindValueSource(Unit.GetUnitId(), TEXT("/value") + DocumentPointer);
			if (!Source) continue;
			if (Selected && (Selected->GetSourceUnit() != Source->GetSourceUnit()
				|| Selected->GetJsonPointer() != Source->GetJsonPointer())) return nullptr;
			Selected = Source;
		}
		return Selected;
	}

	void FResolvedSemanticWorkspace::LocateDocumentDiagnostic(const TArray<FString>& DocumentIdentities,
		const TArray<FDocumentValueSourceBinding>& Bindings, Contract::FDiagnostic& Diagnostic, const FString& GraphUnitId) const
	{
		const FString& File = Diagnostic.Source.SourceFile;
		// The owner-qualified internal path addresses the source map. A public
		// semantic pointer may still describe a relative field of the diagnostic.
		const bool bInternalPointer = !Diagnostic.Source.InternalJsonPath.IsEmpty();
		const FString Pointer = bInternalPointer ? Diagnostic.Source.InternalJsonPath
			: !Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.SemanticPointer : Diagnostic.Source.JsonPath;
		auto IsPublic = [](const FSourceProvenance* Source)
		{
			return Source && !Source->GetSourceUnit().Contains(TEXT(".uai/"))
				&& !Source->GetSourceUnit().Contains(TEXT(".uai\\"));
		};
		auto WriteSource = [](Contract::FSourceLocation& Location, const FSourceProvenance& Source)
		{
			Location.SourceFile = Source.GetSourceUnit();
			Location.JsonPath = Source.GetJsonPointer();
			Location.Line = Source.GetLine();
			Location.Column = Source.GetColumn();
		};
		// A provider can already have a concrete container location. Its explicit
		// unit binding still lets us resolve the declared leaf through content_ref
		// provenance, without guessing a filename or parsing an error message.
		if (!Diagnostic.Source.SourceUnit.IsEmpty() && !Diagnostic.Source.InternalJsonPath.IsEmpty())
		{
			const FString Requested = Diagnostic.Source.InternalJsonPath;
			FString Ancestor = Requested;
			for (;;)
			{
				if (const auto* Source = FindValueSource(Diagnostic.Source.SourceUnit, Ancestor); IsPublic(Source))
				{
					WriteSource(Diagnostic.Source, *Source);
					Diagnostic.SourceKind = Ancestor == Requested && Diagnostic.SourceKind != TEXT("related_input")
						? TEXT("authored_value") : TEXT("related_input");
					return;
				}
				int32 Slash = INDEX_NONE;
				if (!Ancestor.FindLastChar(TEXT('/'), Slash)) break;
				Ancestor.LeftInline(Slash);
			}
		}
		if (!File.IsEmpty() && !File.StartsWith(TEXT("inline://"))) return;
		if (Pointer.IsEmpty()) return;
		auto FindSources = [&](const FString& Path, TArray<const FSourceProvenance*>& Sources)
		{
			for (const auto& Identity : DocumentIdentities)
				if (const auto* Source = FindDocumentValueSource(Identity, Path, Bindings); IsPublic(Source)) Sources.AddUnique(Source);
		};
		TArray<const FSourceProvenance*> Sources;
		FindSources(Pointer, Sources);
		bool bExact = Sources.Num() == 1;
		if (Sources.IsEmpty())
		{
			const FSourceProvenance* Exact = nullptr;
			FindGraphAuthoringDiagnosticSources(*this, Pointer, Exact, Sources, GraphUnitId);
			Sources.RemoveAll([&](const auto* Source) { return !IsPublic(Source); });
			if (IsPublic(Exact)) { Sources = {Exact}; bExact = true; }
		}
		if (Sources.IsEmpty())
		{
			FString Parent = Pointer;
			int32 Slash = INDEX_NONE;
			while (Parent.FindLastChar(TEXT('/'), Slash) && Slash > 0)
			{
				Parent.LeftInline(Slash);
				FindSources(Parent, Sources);
				if (!Sources.IsEmpty()) break;
			}
		}
		if (Sources.IsEmpty()) return;
		if (!bInternalPointer) Diagnostic.SemanticPointer = Pointer;
		Diagnostic.SourceKind = bExact && Diagnostic.SourceKind != TEXT("related_input") ? TEXT("authored_value") : TEXT("related_input");
		if (bExact) WriteSource(Diagnostic.Source, *Sources[0]);
		else
		{
			TArray<TSharedPtr<FJsonValue>> Related;
			TSet<FString> Seen;
			for (const auto* Source : Sources)
			{
				const FString Key = Source->GetSourceUnit() + TEXT("\n") + Source->GetJsonPointer();
				if (Seen.Contains(Key)) continue;
				Seen.Add(Key);
				const auto Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("source_file"), Source->GetSourceUnit());
				Item->SetStringField(TEXT("json_pointer"), Source->GetJsonPointer());
				Item->SetNumberField(TEXT("line"), Source->GetLine());
				Item->SetNumberField(TEXT("column"), Source->GetColumn());
				Related.Add(MakeShared<FJsonValueObject>(Item));
			}
			Diagnostic.RelatedSources = MoveTemp(Related);
			// A navigation anchor only: related sources are not proof that an input leaf is erroneous.
			WriteSource(Diagnostic.Source, *Sources[0]);
		}
	}

	void AttachProjectedPropertySources(const TArray<TSharedPtr<FJsonValue>>& Projected, FJsonObject& Properties)
	{
		const TArray<TSharedPtr<FJsonValue>>* Descriptors = nullptr;
		if (!Properties.TryGetArrayField(TEXT("properties"), Descriptors)) return;
		TMap<FString, TSharedPtr<FJsonObject>> Sources;
		for (const auto& Value : Projected)
		{
			FString Path; const TSharedPtr<FJsonObject>* Source = nullptr;
			if (Value && Value->Type == EJson::Object
				&& Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)
				&& Value->AsObject()->TryGetObjectField(TEXT("authoring_source"), Source)) Sources.Add(Path, *Source);
		}
		for (const auto& Value : *Descriptors)
		{
			FString Path;
			if (!Value || Value->Type != EJson::Object || !Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)) continue;
			if (const auto* Source = Sources.Find(Path)) Value->AsObject()->SetObjectField(TEXT("authoring_source"), MakeShared<FJsonObject>(**Source));
		}
	}

	void FResolvedSemanticWorkspace::AttachDocumentPropertySources(
		const FString& DocumentIdentity, const FString& DocumentPointer,
		const TArray<FDocumentValueSourceBinding>& Bindings, FJsonObject& Object,
		const FString& PreferredField) const
	{
		const FDocumentValueSourceBinding* Selected = nullptr;
		FString SelectedPointer;
		for (const auto& Binding : Bindings)
		{
			if (Binding.DocumentIdentity != DocumentIdentity
				|| !(Binding.DocumentPointer.IsEmpty() || DocumentPointer == Binding.DocumentPointer
					|| DocumentPointer.StartsWith(Binding.DocumentPointer + TEXT("/")))) continue;
			const FString Pointer = Binding.UnitValuePointer
				+ DocumentPointer.RightChop(Binding.DocumentPointer.Len());
			if (!FindValueSource(Binding.UnitId, Pointer)
				&& (PreferredField.IsEmpty() || !FindValueSource(Binding.UnitId, Pointer + TEXT("/") + PreferredField))) continue;
			if (!Selected || Binding.DocumentPointer.Len() > Selected->DocumentPointer.Len())
			{
				Selected = &Binding;
				SelectedPointer = Pointer;
			}
		}
		if (Selected) AttachInlinePropertySources(Selected->UnitId, SelectedPointer, Object, PreferredField);
	}

	static void AttachInlinePropertySourcesImpl(
		const FString& UnitId, const FString& ObjectPointer, FJsonObject& Object, const FString& PreferredField,
		TFunctionRef<const FSourceProvenance*(const FString&)> FindSource)
	{
		auto MakeLocation = [](const FSourceProvenance& Source)
		{
			const auto Location = MakeShared<FJsonObject>();
			Location->SetStringField(TEXT("source_file"), Source.GetSourceUnit());
			Location->SetStringField(TEXT("json_pointer"), Source.GetJsonPointer());
			Location->SetNumberField(TEXT("line"), Source.GetLine());
			Location->SetNumberField(TEXT("column"), Source.GetColumn());
			return Location;
		};
		const auto* ObjectSource = !PreferredField.IsEmpty()
			? FindSource(ObjectPointer + TEXT("/") + PreferredField) : nullptr;
		if (!ObjectSource) ObjectSource = FindSource(ObjectPointer);
		if (ObjectSource)
		{
			const auto Location = MakeLocation(*ObjectSource);
			if (!UnitId.IsEmpty())
			{
				Location->SetStringField(TEXT("source_unit"), UnitId);
				Location->SetStringField(TEXT("internal_json_path"), ObjectPointer);
				if (PreferredField == TEXT("value"))
					Location->SetStringField(TEXT("value_internal_json_path"), ObjectPointer + TEXT("/value"));
			}
			const auto Fields = MakeShared<FJsonObject>();
			for (const auto& Field : Object.Values)
			{
				const FString Token = EscapeJsonPointerSegment(Field.Key);
				if (const auto* Source = FindSource(ObjectPointer + TEXT("/") + Token))
					Fields->SetObjectField(Field.Key, MakeLocation(*Source));
			}
			if (!Fields->Values.IsEmpty()) Location->SetObjectField(TEXT("fields"), Fields);
			Object.SetObjectField(TEXT("authoring_source"), Location);
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		FString PropertiesPointer = ObjectPointer + TEXT("/properties");
		if (!Object.TryGetArrayField(TEXT("properties"), Properties))
		{
			// Reflected property captures wrap their descriptors with profile metadata;
			// inline authoring may instead use the descriptor array directly.
			const TSharedPtr<FJsonObject>* Capture = nullptr;
			if (!Object.TryGetObjectField(TEXT("properties"), Capture)
				|| !(*Capture)->TryGetArrayField(TEXT("properties"), Properties)) return;
			PropertiesPointer += TEXT("/properties");
		}
		for (int32 Index = 0; Index < Properties->Num(); ++Index)
		{
			const auto& Value = (*Properties)[Index];
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;
			const FString Pointer = PropertiesPointer + FString::Printf(TEXT("/%d"), Index);
			const auto* Source = FindSource(Pointer + TEXT("/value"));
			if (!Source) Source = FindSource(Pointer);
			if (!Source) continue;
			const auto Location = MakeLocation(*Source);
			if (!UnitId.IsEmpty())
			{
				Location->SetStringField(TEXT("source_unit"), UnitId);
				Location->SetStringField(TEXT("internal_json_path"), Pointer);
				Location->SetStringField(TEXT("value_internal_json_path"), Pointer + TEXT("/value"));
			}
			Value->AsObject()->SetObjectField(TEXT("authoring_source"), Location);
		}
	}

	void FResolvedSemanticWorkspace::AttachInlinePropertySources(
		const FString& UnitId, const FString& ObjectPointer, FJsonObject& Object,
		const FString& PreferredField) const
	{
		AttachInlinePropertySourcesImpl(UnitId, ObjectPointer, Object, PreferredField,
			[&](const FString& Pointer) { return FindValueSource(UnitId, Pointer); });
	}

	void AttachInlinePropertySources(const FHjsonDocument& Document, const FString& ObjectPointer,
		FJsonObject& Object, const FString& PreferredField)
	{
		AttachInlinePropertySourcesImpl(FString(), ObjectPointer, Object, PreferredField,
			[&](const FString& Pointer) -> const FSourceProvenance* {
				const auto* Node = Document.Find(Pointer);
				return Node ? &Node->GetProvenance() : nullptr;
			});
	}

	FResolvedSemanticWorkspace::FResolvedSemanticWorkspace(
		EWorkspaceSourceKind InSourceKind,
		ESemanticWorkspaceMode InMode,
		FAuthoringModelRef InModel,
		FUeAgentStateRevision InBaseStateRevision,
		TArray<FStructuralEdit> InStructuralEdits,
		TArray<FString> InManagedUnitPaths,
		FString InSelectionFingerprint,
		FString InClosureFingerprint,
		TArray<FString> InClosureUnitIds,
		FString InWorkspaceFingerprint,
		TOptional<TArray<FString>> InEditedUnitIds,
		TMap<FString, TSet<FString>> InResolvedKeyedOperationRoots,
		TMap<FString, FSourceProvenance> InUnitSources,
		TMap<FString, FString> InBaselineUnitHashes,
		TMap<FString, FHjsonDocumentRef> InUnitSourceDocuments,
		TMap<FString, FSemanticUnit> InBaselineUnits,
		TOptional<FSourceProvenance> InStructuralEditsSource)
		: SourceKind(InSourceKind)
		, Mode(InMode)
		, Model(MoveTemp(InModel))
		, BaseStateRevision(MoveTemp(InBaseStateRevision))
		, StructuralEdits(MoveTemp(InStructuralEdits))
		, StructuralEditsSource(MoveTemp(InStructuralEditsSource))
		, ManagedUnitPaths(MoveTemp(InManagedUnitPaths))
		, SelectionFingerprint(
			Normalize(MoveTemp(InSelectionFingerprint)))
		, ClosureFingerprint(
			Normalize(MoveTemp(InClosureFingerprint)))
		, ClosureUnitIds(MoveTemp(InClosureUnitIds))
		, WorkspaceFingerprint(
			Normalize(MoveTemp(InWorkspaceFingerprint)))
		, EditedUnitIds(MoveTemp(InEditedUnitIds))
		, ResolvedKeyedOperationRoots(MoveTemp(InResolvedKeyedOperationRoots))
		, UnitSources(MoveTemp(InUnitSources))
		, BaselineUnitHashes(MoveTemp(InBaselineUnitHashes))
		, UnitSourceDocuments(MoveTemp(InUnitSourceDocuments))
		, BaselineUnits(MoveTemp(InBaselineUnits))
	{
		Validate();
	}

	void FResolvedSemanticWorkspace::Validate()
	{
		ValidationDiagnostic.Reset();
		auto Invalid = [&](const TCHAR* Field, const TCHAR* Reason) {
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_invalid"), Reason, TEXT("workspace_resolve"));
			ValidationDiagnostic.SemanticPointer = TEXT("/") + FString(Field);
		};
		if (LexToString(Mode).IsEmpty()) { Invalid(TEXT("mode"), TEXT("The semantic workspace mode is unsupported.")); ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Mode)); return; }
		if (Model->GetSemanticFingerprint().IsEmpty()) { Invalid(TEXT("model"), TEXT("The semantic workspace model has no fingerprint.")); return; }
		if (WorkspaceFingerprint.IsEmpty()) { Invalid(TEXT("workspace_fingerprint"), TEXT("The semantic workspace has no fingerprint.")); return; }
		if (!NormalizeUniqueValues(ManagedUnitPaths, ValidationDiagnostic, TEXT("managed_unit_paths"), true)) return;
		if (EditedUnitIds.IsSet() && !NormalizeUniqueValues(EditedUnitIds.GetValue(), ValidationDiagnostic, TEXT("edited_unit_ids"), true)) return;
		TSet<FString> EditIds;
		for (int32 Index = 0; Index < StructuralEdits.Num(); ++Index) {
			const FStructuralEdit& Edit = StructuralEdits[Index];
			if (!Edit.IsValid()) {
				ValidationDiagnostic = Edit.GetValidationDiagnostic();
				ValidationDiagnostic.SemanticPointer = TEXT("/structural_edits/") + FString::FromInt(Index) + ValidationDiagnostic.SemanticPointer;
				return;
			}
			if (EditIds.Contains(Edit.GetEditId())) {
				Invalid(TEXT("structural_edits"), TEXT("More than one structural edit has the same edit identity."));
				ValidationDiagnostic.SemanticPointer += TEXT("/") + FString::FromInt(Index) + TEXT("/edit_id");
				return;
			}
			EditIds.Add(Edit.GetEditId());
		}
		Contract::FDiagnostic ClosureError;
		const bool bClosureIsValid = NormalizeUniqueValues(ClosureUnitIds, ClosureError, TEXT("closure_unit_ids"), true);
		if (Mode == ESemanticWorkspaceMode::EditBundle) {
			if (SelectionFingerprint.IsEmpty()) { Invalid(TEXT("selection_fingerprint"), TEXT("An edit bundle requires a selection binding.")); return; }
			if (ClosureFingerprint.IsEmpty()) { Invalid(TEXT("closure_fingerprint"), TEXT("An edit bundle requires a dependency closure binding.")); return; }
			if (ClosureUnitIds.IsEmpty()) { Invalid(TEXT("closure_unit_ids"), TEXT("An edit bundle requires at least one closure unit.")); return; }
			if (!bClosureIsValid) ValidationDiagnostic = MoveTemp(ClosureError);
		}

	}

	EWorkspaceSourceKind
	FResolvedSemanticWorkspace::GetSourceKind() const
	{
		return SourceKind;
	}

	ESemanticWorkspaceMode
	FResolvedSemanticWorkspace::GetMode() const
	{
		return Mode;
	}

	const FAuthoringModelRef&
	FResolvedSemanticWorkspace::GetModel() const
	{
		return Model;
	}

	const FUeAgentStateRevision&
	FResolvedSemanticWorkspace::GetBaseStateRevision() const
	{
		return BaseStateRevision;
	}

	const TArray<FStructuralEdit>&
	FResolvedSemanticWorkspace::GetStructuralEdits() const
	{
		return StructuralEdits;
	}

	const TArray<FString>&
	FResolvedSemanticWorkspace::GetManagedUnitPaths() const
	{
		return ManagedUnitPaths;
	}

	const FString&
	FResolvedSemanticWorkspace::GetSelectionFingerprint() const
	{
		return SelectionFingerprint;
	}

	const FString&
	FResolvedSemanticWorkspace::GetClosureFingerprint() const
	{
		return ClosureFingerprint;
	}

	const TArray<FString>&
	FResolvedSemanticWorkspace::GetClosureUnitIds() const
	{
		return ClosureUnitIds;
	}

	const FString&
	FResolvedSemanticWorkspace::GetWorkspaceFingerprint() const
	{
		return WorkspaceFingerprint;
	}

	const TOptional<TArray<FString>>&
	FResolvedSemanticWorkspace::GetEditedUnitIds() const
	{
		return EditedUnitIds;
	}

	bool FResolvedSemanticWorkspace::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	TSharedRef<const FResolvedSemanticWorkspace> FResolvedSemanticWorkspace::Rebind(
		FAuthoringModelRef NewModel, const FUeAgentStateRevision& Revision) const
	{
		auto Result = MakeShared<FResolvedSemanticWorkspace>(*this);
		Result->Model = MoveTemp(NewModel);
		Result->BaseStateRevision = Revision;
		Result->Validate();
		return Result;
	}

	bool TryValidateSemanticWorkspaceBinding(
		const FResolvedSemanticWorkspaceRef& Workspace,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentStateRevision& CurrentRevision,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FWorkspaceError& OutError,
		bool bAllowVerifiedRevisionDrift)
	{
		OutError.Reset();
		if (!Workspace->IsValid())
		{
			OutError.SetFailure(Workspace->GetValidationDiagnostic());
			return false;
		}
		if (Workspace->GetMode() != ExpectedMode)
		{
			auto Failure = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_mode_mismatch"), TEXT("The workspace mode does not match the requested operation."), TEXT("workspace_binding"));
			Failure.PropertyPath = TEXT("mode");
			Failure.ExpectedValue = MakeShared<FJsonValueString>(LexToString(ExpectedMode));
			Failure.ActualValue = MakeShared<FJsonValueString>(LexToString(Workspace->GetMode()));
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		Contract::FDiagnostic ContractError;
		if (!TryMatchContract(*Workspace->GetModel(), *CurrentModel, bAllowVerifiedRevisionDrift, ContractError))
		{
			OutError.SetFailure(MoveTemp(ContractError));
			return false;
		}
		if (!bAllowVerifiedRevisionDrift && !(Workspace->GetBaseStateRevision() == CurrentRevision))
		{
			auto Failure = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_state_revision_mismatch"), TEXT("The workspace base state revision differs from the current asset state."), TEXT("workspace_binding"));
			Failure.PropertyPath = TEXT("base_state_revision");
			Failure.ExpectedEvidence = RevisionEvidence(CurrentRevision);
			Failure.ActualEvidence = RevisionEvidence(Workspace->GetBaseStateRevision());
			OutError.SetFailure(MoveTemp(Failure));
			return false;
		}
		if (ExpectedMode == ESemanticWorkspaceMode::EditBundle)
		{
			if (Workspace->GetSelectionFingerprint()
					!= Normalize(ExpectedSelectionFingerprint)
				|| Workspace->GetClosureFingerprint()
					!= Normalize(ExpectedClosureFingerprint))
			{
				const bool bSelectionMismatch = Workspace->GetSelectionFingerprint() != Normalize(ExpectedSelectionFingerprint);
				auto Failure = Contract::FDiagnostic::Failure(TEXT("semantic_workspace_closure_mismatch"), bSelectionMismatch
					? TEXT("The edit bundle selection differs from the requested selection.") : TEXT("The edit bundle dependency closure differs from the current closure."), TEXT("workspace_binding"));
				Failure.PropertyPath = bSelectionMismatch ? TEXT("selection") : TEXT("closure");
				OutError.SetFailure(MoveTemp(Failure));
				return false;
			}
		}
		return true;
	}
}
