#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/UeAgentReadableName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FString& SourceUnit = FString(), const FString& Field = FString(),
			TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("keyed_item_projection"));
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
			return false;
		}

		bool FailAt(FWorkspaceError& OutError, const FString& Code, const FString& Reason,
			const FString& SourceUnit, const FString& Pointer, TSharedPtr<FJsonValue> Actual = nullptr,
			TSharedPtr<FJsonValue> Expected = nullptr, bool bNativeResult = false)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("keyed_item_projection"));
			Diagnostic.PropertyPath = Pointer;
			Diagnostic.SourceKind = bNativeResult ? TEXT("native_result") : TEXT("authored_value");
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit, Pointer);
			return false;
		}
	}

	bool TryProjectKeyedItemOriginalIntent(const FKeyedItemProjectionContract& Contract,
		TSharedPtr<FJsonValue>& Before, TSharedPtr<FJsonValue>& Desired,
		TSharedPtr<FJsonValue>& Actual, TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error,
		const TSharedPtr<FJsonObject>& CreationArguments, TMap<FString, FString>* OutNativeAliases)
	{
		if (OutNativeAliases) OutNativeAliases->Reset();
		if (!Before || !Desired || !Actual || Before->Type != EJson::Object
			|| Desired->Type != EJson::Object || Actual->Type != EJson::Object)
		{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_intent_shape_invalid"), TEXT("Original intent requires object collections for the captured, desired and actual states."), TEXT("original_intent")); return false; }
		const auto Records = Desired->AsObject();
		auto CapturedIdentity = [&](const TSharedPtr<FJsonObject>& Row) {
			FString Identity;
			if (!Contract.AggregatedItemSemanticSuffix.IsEmpty()
				&& Row->TryGetStringField(TEXT("unit_id"), Identity) && !Identity.IsEmpty())
				return TEXT("unit:") + Identity;
			if (Row->TryGetStringField(TEXT("id"), Identity) && !Identity.IsEmpty()) return TEXT("id:") + Identity;
			return FString();
		};
		auto RestoreProjectedName = [&](const FString& Alias, const TSharedPtr<FJsonValue>& Value) {
			const auto Row = UeAgentAuthoringState::CloneCanonicalValue(Value)->AsObject();
			// Composition omits an exact name/label already carried by the map key.
			// Recover it before rebinding aliases; duplicate labels remain explicit.
			for (const FString& Field : Contract.NamingPolicy.Fields)
				if ((Field == TEXT("name") || Field == TEXT("label")) && !Row->HasField(Field)) Row->SetStringField(Field, Alias);
			return Row;
		};
		// A fresh composition may assign different aliases to duplicate labels.
		// Align the capture with the immutable request by native identity before
		// interpreting operation rows; a readable label is not the item identity.
		TMap<FString, FString> AuthoredAliasById;
		for (const auto& Record : Records->Values)
		{
			FString Id;
			if (Record.Value && Record.Value->Type == EJson::Object
				&& !(Id = CapturedIdentity(Record.Value->AsObject())).IsEmpty())
				AuthoredAliasById.Add(Id, Record.Key);
		}
		const auto Base = MakeShared<FJsonObject>();
		for (const auto& Record : Before->AsObject()->Values)
		{
			FString Id;
			Id = CapturedIdentity(Record.Value->AsObject());
			const auto* Alias = AuthoredAliasById.Find(Id);
			Base->SetObjectField(Alias ? *Alias : Record.Key, RestoreProjectedName(Record.Key, Record.Value));
		}
		Before = MakeShared<FJsonValueObject>(Base);
		const auto Resolved = UeAgentAuthoringState::CloneCanonicalValue(Before)->AsObject();
		TSet<FString> Created;
		for (const auto& Record : Records->Values)
		{
			if (!Record.Value || Record.Value->Type != EJson::Object)
			{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_intent_record_invalid"), FString::Printf(TEXT("Desired record '%s' must be an object."), *Record.Key), TEXT("original_intent")); return false; }
			const auto Row = Record.Value->AsObject();
			FString Operation; Row->TryGetStringField(TEXT("operation"), Operation);
			if (Operation == TEXT("remove")) { Resolved->RemoveField(Record.Key); continue; }
			const auto Captured = Base->TryGetField(Record.Key);
			const auto Argument = CreationArguments ? CreationArguments->TryGetField(Record.Key) : nullptr;
			if (Operation != TEXT("create") && Captured && Captured->Type == EJson::Object)
			{
				const auto Merged = UeAgentAuthoringState::CloneCanonicalValue(Argument ? Argument : Captured)->AsObject();
				for (const auto& Field : Row->Values)
					if ((!Argument || UeAgentAuthoringState::MakeStableCanonicalValueKey(Field.Value)
						!= UeAgentAuthoringState::MakeStableCanonicalValueKey(Captured->AsObject()->TryGetField(Field.Key)))
						&& !(Contract.bRequiresCreationCandidate && Field.Key == TEXT("candidate")))
						Merged->SetField(Field.Key, UeAgentAuthoringState::CloneCanonicalValue(Field.Value));
				Resolved->SetObjectField(Record.Key, Merged);
			}
			else {
				const auto Value = UeAgentAuthoringState::CloneCanonicalValue(Argument ? Argument : Record.Value);
				for (const auto& Field : Row->Values) if (Contract.VisibleFieldPolicy.Allows(Field.Key)
					&& !(Contract.bRequiresCreationCandidate && Field.Key == TEXT("candidate"))) {
					FString NewKey;
					if (Contract.ReferenceFields.Contains(Field.Key) && Field.Value->Type == EJson::Object
						&& Field.Value->AsObject()->TryGetStringField(TEXT("new_item"), NewKey))
					{
						const auto Target = CreationArguments ? CreationArguments->TryGetField(NewKey) : nullptr;
						if (!Target) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_creation_reference_missing"), FString::Printf(TEXT("Record '%s', field '%s', refers to new item '%s' without creation arguments."), *Record.Key, *Field.Key, *NewKey), TEXT("original_intent")); return false; }
						Value->AsObject()->SetField(Field.Key, Target->AsObject()->TryGetField(TEXT("id")));
					}
					else Value->AsObject()->SetField(Field.Key, UeAgentAuthoringState::CloneCanonicalValue(Field.Value));
				}
				Resolved->SetField(Record.Key, Value);
			}
			if (Operation == TEXT("create")) Created.Add(Record.Key);
		}
		for (const auto& Record : Resolved->Values)
			for (const auto& Field : Contract.ReferenceFields)
			{
				const auto Value = Record.Value->AsObject()->TryGetField(Field.Key);
				FString NewKey;
				if (!Value || Value->Type != EJson::Object || !Value->AsObject()->TryGetStringField(TEXT("new_item"), NewKey)) continue;
				const auto Target = CreationArguments ? CreationArguments->TryGetField(NewKey) : nullptr;
				if (!Target) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_creation_reference_missing"), FString::Printf(TEXT("Record '%s', field '%s', refers to new item '%s' without creation arguments."), *Record.Key, *Field.Key, *NewKey), TEXT("original_intent")); return false; }
				Record.Value->AsObject()->SetField(Field.Key, Target->AsObject()->TryGetField(TEXT("id")));
			}
		// These contracts declare array ranks. Compute their meaning from the
		// immutable request, never from actual positions or the domain DTO.
		if (!Contract.OrderRankField.IsEmpty())
		{
			TMap<FString, TArray<TPair<FString, TSharedPtr<FJsonObject>>>> Groups;
			for (const auto& Record : Resolved->Values)
			{
				const auto Row = Record.Value->AsObject();
				FString Kind; Row->TryGetStringField(TEXT("kind"), Kind);
				if (Contract.UnorderedKinds.Contains(Kind)) continue;
				FString Group;
				for (const auto& Field : Contract.OrderRankGroupFields)
				{
					const FString Part = UeAgentAuthoringState::MakeStableCanonicalValueKey(Row->TryGetField(Field));
					Group += FString::FromInt(Part.Len()) + TEXT(":") + Part;
				}
				Groups.FindOrAdd(Group).Emplace(Record.Key, Row);
			}
			for (auto& Group : Groups)
			{
				TMap<FString, FString> Ranks;
				TSet<FString> Append;
				for (const auto& Record : Group.Value)
				{
					FString Rank; double Finite = 0; int32 Comparison = 0;
					const auto Value = Record.Value->TryGetField(Contract.OrderRankField);
					if (!Value || !CanonicalJson::TryGetNumberLexeme(*Value, Rank)
						|| !IsIntegralNumberLexeme(Rank) || !TryParseFiniteDecimal(Rank, Finite)
						|| !TryCompareNumberLexemes(Rank, TEXT("0"), Comparison)
						|| (Comparison < 0 && (!CanonicalJson::AreNumbersEquivalent(*Value, FJsonValueNumber(-1)) || !Created.Contains(Record.Key))))
					{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_intent_order_invalid"), FString::Printf(TEXT("Record '%s', field '%s', requires a finite nonnegative integer rank; -1 is allowed only for a newly created item."), *Record.Key, *Contract.OrderRankField), TEXT("original_intent")); return false; }
					if (Comparison < 0) Append.Add(Record.Key);
					Ranks.Add(Record.Key, MoveTemp(Rank));
				}
				Group.Value.Sort([&](const auto& A, const auto& B) {
					if (Append.Contains(A.Key) != Append.Contains(B.Key)) return !Append.Contains(A.Key);
					int32 Comparison = 0;
					TryCompareNumberLexemes(Ranks.FindChecked(A.Key), Ranks.FindChecked(B.Key), Comparison);
					FString Left = A.Key, Right = B.Key;
					if (!Contract.OrderRankTieIdentityField.IsEmpty()) {
						A.Value->TryGetStringField(Contract.OrderRankTieIdentityField, Left);
						B.Value->TryGetStringField(Contract.OrderRankTieIdentityField, Right);
					}
					return Comparison != 0 ? Comparison < 0 : Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
				});
				for (int32 Index = 0; Index < Group.Value.Num(); ++Index)
					Group.Value[Index].Value->SetNumberField(Contract.OrderRankField, Index);
			}
		}
		TMap<FString, FString> AliasById;
		TMap<FString, FString> AliasByNativeId;
		TMap<FString, FString> AliasByDerivedIdentity;
		auto DerivedIdentity = [&](const TSharedPtr<FJsonObject>& Row) {
			FString Kind; Row->TryGetStringField(TEXT("kind"), Kind);
			const auto* Fields = Contract.DerivedIdentityFieldsByKind.Find(Kind);
			if (!Fields) return FString();
			FString Key = Kind;
			for (const auto& Field : *Fields) {
				const auto Value = Row->TryGetField(Field);
				if (!Value) return FString();
				const FString Part = UeAgentAuthoringState::MakeStableCanonicalValueKey(Value);
				Key += FString::FromInt(Part.Len()) + TEXT(":") + Part;
			}
			return Key;
		};
		for (const auto& Record : Resolved->Values) {
			const FString Key = DerivedIdentity(Record.Value->AsObject());
			if (!Key.IsEmpty()) AliasByDerivedIdentity.Add(Key, Record.Key);
		}
		for (const auto& Record : Base->Values)
		{
			FString Id;
			if (!(Id = CapturedIdentity(Record.Value->AsObject())).IsEmpty())
				AliasById.Add(Id, Record.Key);
			if (Record.Value->AsObject()->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
				AliasByNativeId.Add(Id, Record.Key);
		}
		// The native receipt also covers candidate replacement (an update),
		// whose resulting object can have a new identity just like a creation.
		if (CreationArguments) for (const auto& Argument : CreationArguments->Values) {
			if (!Resolved->HasField(Argument.Key) || !Argument.Value || Argument.Value->Type != EJson::Object) continue;
			FString Id; const auto Row = Argument.Value->AsObject();
			if (!(Id = CapturedIdentity(Row)).IsEmpty()) AliasById.Add(Id, Argument.Key);
			if (Row->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty()) AliasByNativeId.Add(Id, Argument.Key);
		}
		// Reserve request/baseline aliases before reading native rows. An unmatched
		// native object must not overwrite a matched one or impersonate an edited
		// object merely because UE assigned it the same readable title.
		TSet<FString, FExactStringKeyFuncs> ReservedAliases;
		for (const auto& Record : Base->Values) ReservedAliases.Add(Record.Key);
		for (const auto& Record : Resolved->Values) ReservedAliases.Add(Record.Key);
		const auto Native = MakeShared<FJsonObject>();
		for (const auto& Record : Actual->AsObject()->Values)
		{
			FString Id = CapturedIdentity(Record.Value->AsObject());
			const auto* Alias = AliasByDerivedIdentity.Find(DerivedIdentity(Record.Value->AsObject()));
			if (!Alias) Alias = AliasById.Find(Id);
			// Creation receipts bind native IDs before composition adds unit_id.
			// Both are native provenance, but they are distinct namespaces.
			FString NativeId;
			if (!Alias && Record.Value->AsObject()->TryGetStringField(TEXT("id"), NativeId))
				Alias = AliasByNativeId.Find(NativeId);
			FString NativeAlias = Alias ? *Alias : Record.Key;
			const auto Existing = Base->TryGetField(Record.Key);
			const bool bIdentityBearing = !Id.IsEmpty()
				|| (Existing && !CapturedIdentity(Existing->AsObject()).IsEmpty());
			if (!Alias && bIdentityBearing)
				NativeAlias = UE::UAI::AllocateReadableName(Record.Key, [&](const FString& Name) {
					return ReservedAliases.Contains(Name) || Native->HasField(Name);
				});
			// A valid correspondence is one-to-one. Never silently overwrite a row.
			if (Native->HasField(NativeAlias))
			{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_intent_identity_ambiguous"), FString::Printf(TEXT("More than one native record resolves to the authored alias '%s'."), *NativeAlias), TEXT("original_intent")); return false; }
			// These are native facts, not newly exported rows. A native field must
			// never be synthesized from a display key to satisfy an import check.
			Native->SetObjectField(NativeAlias, UeAgentAuthoringState::CloneCanonicalValue(Record.Value)->AsObject());
			ReservedAliases.Add(NativeAlias);
			if (OutNativeAliases) OutNativeAliases->Add(Record.Key, NativeAlias);
		}
		for (const FString& Key : Created) {
			Base->RemoveField(Key);
			if (!CreationArguments || !CreationArguments->HasField(Key)) Resolved->RemoveField(Key);
		}
		// Diagnostic paths retain the authored alias after native renaming.
		for (const auto& Record : Resolved->Values)
		{
			const FString Path = TEXT("/") + EscapeJsonPointerSegment(Contract.RootField) + TEXT("/") + EscapeJsonPointerSegment(Record.Key);
			SourcePaths.Add(Path, Path);
		}
		// Bindings/candidate instructions are consumed above, not business values.
		// Select accepted input fields using the existing record contract. Do not
		// project or validate actual state through the export's visible-row builder.
		auto SelectInputFields = [&](const TSharedPtr<FJsonObject>& Rows, TSharedPtr<FJsonValue>& Out) {
			const auto Projected = MakeShared<FJsonObject>();
			for (const auto& Row : Rows->Values)
			{
				auto Business = MakeShared<FJsonObject>();
				for (const auto& Field : Row.Value->AsObject()->Values)
					if (Contract.VisibleFieldPolicy.Allows(Field.Key)) Business->SetField(Field.Key, Field.Value);
				if (Contract.bRequiresCreationCandidate) Business->RemoveField(TEXT("candidate"));
				Projected->SetObjectField(Row.Key, Business);
			}
			Out = MakeShared<FJsonValueObject>(Projected); return true;
		};
		SelectInputFields(Base, Before);
		SelectInputFields(Resolved, Desired);
		Actual = MakeShared<FJsonValueObject>(Native);
		return true;
	}

	bool TryRecordKeyedCreationArguments(const FString& RootField, const TSharedPtr<FJsonValue>& SourceRows,
		const TSharedPtr<FJsonObject>& NativeRows, const TMap<FString, FString>& NativeByNewKey, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (!UeAgentNativeNormalizationIntent::IsActive() || NativeByNewKey.IsEmpty()) return true;
		FKeyedItemProjectionContract Contract;
		if (!TryGetKeyedItemProjectionContract(RootField, Contract)) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_contract_missing"), FString::Printf(TEXT("Collection '%s' has no keyed-item projection contract."), *RootField), TEXT("original_intent")); return false; }
		const auto Arguments = MakeShared<FJsonObject>();
		for (const auto& Pair : NativeByNewKey) {
			const auto Value = NativeRows->TryGetField(Pair.Value);
			if (!Value || Value->Type != EJson::Object) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_creation_argument_missing"), FString::Printf(TEXT("Created item '%s' has no native object record for its creation arguments."), *Pair.Key), TEXT("original_intent")); return false; }
			const auto Row = UeAgentAuthoringState::CloneCanonicalValue(Value)->AsObject();
			Row->SetStringField(TEXT("id"), Pair.Value);
			Arguments->SetObjectField(Pair.Key, Row);
		}
		UeAgentNativeNormalizationIntent::RecordKeyedArguments(RootField, SourceRows, Arguments);
		return true;
	}

	bool TryCollectOwnedItemRemovals(const TArray<FOwnedItemRemovalCandidate>& Candidates,
		const TArray<FString>& RemovedOwners, TFunctionRef<bool(const FString&, bool&)> IsUnchanged,
		TArray<FString>& OutRemovedItems)
	{
		OutRemovedItems.Reset();
		TSet<FString, FExactStringKeyFuncs> Removed;
		for (const FString& Owner : RemovedOwners) Removed.Add(Owner);
		bool bChanged;
		do
		{
			bChanged = false;
			for (const auto& Candidate : Candidates)
			{
				if (Removed.Contains(Candidate.ItemId) || !Removed.Contains(Candidate.OwnerId)) continue;
				bool bUnchanged = false;
				if (!IsUnchanged(Candidate.ItemId, bUnchanged)) return false;
				if (!bUnchanged) continue;
				Removed.Add(Candidate.ItemId);
				OutRemovedItems.Add(Candidate.ItemId);
				bChanged = true;
			}
		} while (bChanged);
		return true;
	}

	namespace
	{
		bool SameProjectedValue(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left.IsValid() == Right.IsValid()
				&& (!Left.IsValid() || FJsonValue::CompareEqual(*Left, *Right));
		}

		bool MergeProjectedValueEdits(const TSharedPtr<FJsonValue>& Captured,
			const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right,
			const FString& Path, TSharedPtr<FJsonValue>& OutMerged, Contract::FDiagnostic& OutError)
		{
			// Projections need not expose the same fields. Absence is not an edit;
			// an explicit null or empty collection still participates in the merge.
			if (!Left.IsValid() && !Right.IsValid())
			{
				OutMerged = Captured;
				return true;
			}
			if ((!Left.IsValid() || (Left->Type == EJson::Object && Left->AsObject().IsValid()))
				&& (!Right.IsValid() || (Right->Type == EJson::Object && Right->AsObject().IsValid()))
				&& (!Captured.IsValid() || (Captured->Type == EJson::Object && Captured->AsObject().IsValid())))
			{
				// Even equal partial objects can omit captured fields.
				const TSharedPtr<FJsonObject> Base = Captured.IsValid() ? Captured->AsObject() : nullptr;
				TSet<FString> FieldSet;
				if (Base.IsValid()) for (const auto& Field : Base->Values) FieldSet.Add(Field.Key);
				if (Left.IsValid()) for (const auto& Field : Left->AsObject()->Values) FieldSet.Add(Field.Key);
				if (Right.IsValid()) for (const auto& Field : Right->AsObject()->Values) FieldSet.Add(Field.Key);
				TArray<FString> Fields = FieldSet.Array();
				Fields.Sort();
				const TSharedRef<FJsonObject> Merged = MakeShared<FJsonObject>();
				for (const FString& Field : Fields)
				{
					TSharedPtr<FJsonValue> Value;
					const FString FieldPath = Path + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					if (!MergeProjectedValueEdits(Base.IsValid() ? Base->TryGetField(Field) : nullptr,
						Left.IsValid() ? Left->AsObject()->TryGetField(Field) : nullptr,
						Right.IsValid() ? Right->AsObject()->TryGetField(Field) : nullptr,
						FieldPath, Value, OutError)) return false;
					if (Value.IsValid()) Merged->SetField(Field, Value);
				}
				OutMerged = MakeShared<FJsonValueObject>(Merged);
				return true;
			}
			if (!Left.IsValid() || !Right.IsValid())
			{
				OutMerged = Left.IsValid() ? Left : Right;
				return true;
			}
			if (SameProjectedValue(Left, Right) || SameProjectedValue(Right, Captured))
			{
				OutMerged = Left;
				return true;
			}
			if (SameProjectedValue(Left, Captured))
			{
				OutMerged = Right;
				return true;
			}
			OutError = Contract::FDiagnostic::Failure(TEXT("keyed_item_state_projection_conflict"),
				TEXT("Two authored projections change the same field to different values."), TEXT("document_restore"));
			OutError.Source.InternalJsonPath = Path.IsEmpty() ? TEXT("/") : Path;
			OutError.ExpectedValue = Left;
			OutError.ActualValue = Right;
			OutError.ExpectedValuePresent = Left.IsValid();
			OutError.ActualValuePresent = Right.IsValid();
			return false;
		}

		const FUeAgentCanonicalValue* OwnedDefinitionField(const FUeAgentCanonicalValue& Value, const FString& Name)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object) return nullptr;
			const auto* Field = Value.GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Name; });
			return Field ? &Field->Value.Get() : nullptr;
		}
	}

	bool TryMergeKeyedItemStateEdits(const TSharedPtr<FJsonObject>& Captured,
		const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right,
		TSharedPtr<FJsonObject>& OutMerged, Contract::FDiagnostic& OutError)
	{
		OutMerged.Reset();
		OutError.Reset();
		if (!Left.IsValid() || !Right.IsValid())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("keyed_item_state_projection_invalid"),
				!Left.IsValid() ? TEXT("The first authored projection is missing its object.")
					: TEXT("The second authored projection is missing its object."), TEXT("document_restore"));
			OutError.Source.InternalJsonPath = TEXT("/");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			return false;
		}
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<FJsonValue> Base;
		if (Captured.IsValid()) Base = MakeShared<FJsonValueObject>(Captured);
		if (!MergeProjectedValueEdits(Base, MakeShared<FJsonValueObject>(Left), MakeShared<FJsonValueObject>(Right),
			TEXT(""), Value, OutError)) return false;
		OutMerged = Value->AsObject();
		return true;
	}

	bool TryGetOwnedItemBinding(const FUeAgentCanonicalValue& Definition, FString& OutKey,
		FString& OutRelativePath, FWorkspaceError& OutError, int32 Occurrence)
	{
		const auto* Builtin = OwnedDefinitionField(Definition, TEXT("builtin"));
		const auto* Name = OwnedDefinitionField(Definition, TEXT("instance_name"));
		const auto* Path = OwnedDefinitionField(Definition, TEXT("owner_relative_path"));
		if (!Builtin || Builtin->GetKind() != EUeAgentValueKind::String || Builtin->GetText() != TEXT("owned_object")
			|| !Name || Name->GetKind() != EUeAgentValueKind::String
			|| !Path || Path->GetKind() != EUeAgentValueKind::String || Path->GetText().IsEmpty())
		{ return Fail(OutError, TEXT("owned_item_binding_invalid"), !Builtin || Builtin->GetKind() != EUeAgentValueKind::String || Builtin->GetText() != TEXT("owned_object")
			? TEXT("Captured owned-item definition requires builtin = owned_object.")
			: !Name || Name->GetKind() != EUeAgentValueKind::String ? TEXT("Captured owned-item instance_name must be a string.")
			: TEXT("Captured owned-item owner_relative_path must be a nonempty string.")); }
		OutRelativePath = Path->GetText();
		return TryFormatReadableKeyedItemAlias(Name->GetText(), Occurrence, OutKey, OutError);
	}

	bool TryResolveOrderedOwnedItems(const FUeAgentCanonicalValue& ProposedItems,
		const FUeAgentCanonicalValue& CapturedDefinitions, FUeAgentResourceTracker& Resources,
		TArray<FOrderedOwnedItemBinding>& OutItems, FWorkspaceError& OutError)
	{
		OutItems.Reset();
		if (ProposedItems.GetKind() != EUeAgentValueKind::Array || CapturedDefinitions.GetKind() != EUeAgentValueKind::Array)
		{ return Fail(OutError, TEXT("ordered_owned_items_shape_invalid"), ProposedItems.GetKind() != EUeAgentValueKind::Array
			? TEXT("Proposed owned items must be an array.") : TEXT("Captured owned-item definitions must be an array.")); }
		TMap<FString, int32> CapturedKeys;
		TArray<FString> Paths;
		TMap<FString, int32> Occurrences;
		for (int32 Index = 0; Index < CapturedDefinitions.GetArrayItems().Num(); ++Index)
		{
			FString Key, Path;
			const auto& Definition = CapturedDefinitions.GetArrayItems()[Index].Get();
			const auto* Name = OwnedDefinitionField(Definition, TEXT("instance_name"));
			if (!Name) { Paths.Add(FString()); continue; } // Captured null has no instance.
			if (Name->GetKind() != EUeAgentValueKind::String) { return Fail(OutError, TEXT("owned_item_binding_invalid"), TEXT("Captured instance_name must be a string."), FString(), FString::Printf(TEXT("/%d/instance_name"), Index)); }
			int32& Occurrence = Occurrences.FindOrAdd(Name->GetText().TrimStartAndEnd().ToLower());
			if (!TryGetOwnedItemBinding(Definition, Key, Path, OutError, Occurrence++)) return false;
			if (const auto* Previous = CapturedKeys.Find(Key); Previous && Paths[*Previous] != Path)
			{ return Fail(OutError, TEXT("owned_item_key_ambiguous"), FString::Printf(TEXT("Owned-item key '%s' resolves to more than one captured object."), *Key)); }
			CapturedKeys.Add(Key, Index);
			Paths.Add(Path);
		}
		for (const auto& Item : ProposedItems.GetArrayItems())
		{
			const auto* NullPath = OwnedDefinitionField(Item.Get(), TEXT("path"));
			if (Item->GetKind() == EUeAgentValueKind::Null || (NullPath && NullPath->GetKind() == EUeAgentValueKind::Null
				&& Item->GetObjectItems().Num() == 1))
			{
				FOrderedOwnedItemBinding NullItem; NullItem.Value = Item; OutItems.Add(MoveTemp(NullItem)); continue;
			}
			const auto* Owned = OwnedDefinitionField(Item.Get(), TEXT("owned_object"));
			if (!Owned || Owned->GetKind() != EUeAgentValueKind::Object || Item->GetObjectItems().Num() != 1)
			{ return Fail(OutError, TEXT("ordered_owned_item_invalid"), TEXT("An owned-item entry must contain exactly one owned_object object."), FString(), FString::Printf(TEXT("/%d"), OutItems.Num())); }
			FOrderedOwnedItemBinding Binding;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			for (const auto& Field : Owned->GetObjectItems())
			{
				if (Field.Key == TEXT("instance"))
				{
					if (Field.Value->GetKind() != EUeAgentValueKind::String)
					{ return Fail(OutError, TEXT("owned_item_key_invalid"), TEXT("Owned-item instance must be a string."), FString(), FString::Printf(TEXT("/%d/owned_object/instance"), OutItems.Num())); }
					const auto* Index = CapturedKeys.Find(Field.Value->GetText());
					if (!Index) { return Fail(OutError, TEXT("owned_item_key_unknown"), TEXT("Owned-item instance does not identify a captured object."), FString(), FString::Printf(TEXT("/%d/owned_object/instance"), OutItems.Num()), MakeShared<FJsonValueString>(Field.Value->GetText())); }
					Binding.CapturedIndex = *Index;
					Binding.RelativePath = Paths[*Index];
				}
				else if (Field.Key == TEXT("properties") || Field.Key == TEXT("name") || Field.Key == TEXT("select")) Fields.Add(Field);
				else { return Fail(OutError, TEXT("owned_item_visible_field_forbidden"), FString::Printf(TEXT("Owned-item field '%s' is not supported; allowed fields are instance, properties, name and select."), *Field.Key), FString(), FString::Printf(TEXT("/%d/owned_object/"), OutItems.Num()) + EscapeJsonPointerSegment(Field.Key)); }
			}
			const bool bHasName = OwnedDefinitionField(*Owned, TEXT("name")) != nullptr;
			if (const auto* Properties = OwnedDefinitionField(*Owned, TEXT("properties"));
				Properties && Properties->GetKind() != EUeAgentValueKind::Object)
			{ return Fail(OutError, TEXT("owned_item_properties_invalid"), TEXT("Owned-item properties must be an object."), FString(), FString::Printf(TEXT("/%d/owned_object/properties"), OutItems.Num())); }
			const bool bHasSelect = OwnedDefinitionField(*Owned, TEXT("select")) != nullptr;
			if (Binding.CapturedIndex != INDEX_NONE && (bHasName || bHasSelect))
			{ return Fail(OutError, TEXT("owned_item_existing_selector_forbidden"), TEXT("An existing owned-item instance cannot also specify name or select."), FString(), FString::Printf(TEXT("/%d/owned_object"), OutItems.Num())); }
			if (Binding.CapturedIndex == INDEX_NONE && !bHasName)
			{ return Fail(OutError, TEXT("owned_item_new_name_required"), TEXT("A new owned item requires name when no captured instance is selected."), FString(), FString::Printf(TEXT("/%d/owned_object/name"), OutItems.Num())); }
			FUeAgentStateSemanticError Error;
			FUeAgentCanonicalValuePtr Value;
			if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error)
				|| !FUeAgentCanonicalValue::TryObject({{TEXT("owned_object"), Value.ToSharedRef()}}, Resources, Binding.Value, Error))
			{ OutError.SetFailure(Error); return false; }
			OutItems.Add(MoveTemp(Binding));
		}
		return true;
	}

	bool TryResolveOrderedStructuredItems(const FUeAgentCanonicalValue& ProposedItems,
		const FUeAgentCanonicalValue& CapturedDefinitions, FUeAgentResourceTracker& Resources,
		TArray<FOrderedStructuredItemBinding>& OutItems, FWorkspaceError& OutError)
	{
		if (CapturedDefinitions.GetKind() != EUeAgentValueKind::Array)
		{ OutItems.Reset(); return FailAt(OutError, TEXT("ordered_structured_items_shape_invalid"), TEXT("Captured structured-item definitions must be an array."), FString(), TEXT("/"), nullptr, MakeShared<FJsonValueString>(TEXT("captured array")), true); }
		return TryResolveOrderedStructuredItems(ProposedItems, CapturedDefinitions.GetArrayItems().Num(),
			Resources, OutItems, OutError);
	}

	bool TryResolveOrderedStructuredItems(const FUeAgentCanonicalValue& ProposedItems,
		int32 CapturedCount, FUeAgentResourceTracker& Resources,
		TArray<FOrderedStructuredItemBinding>& OutItems, FWorkspaceError& OutError)
	{
		OutItems.Reset();
		if (ProposedItems.GetKind() != EUeAgentValueKind::Array
			|| CapturedCount < 0)
		{ return FailAt(OutError, TEXT("ordered_structured_items_shape_invalid"), ProposedItems.GetKind() != EUeAgentValueKind::Array ? TEXT("Proposed structured items must be an array.") : FString::Printf(TEXT("Captured structured-item count must be nonnegative; received %d."), CapturedCount),
			FString(), TEXT("/"), ProposedItems.GetKind() == EUeAgentValueKind::Array ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(CapturedCount)) : nullptr,
			MakeShared<FJsonValueString>(ProposedItems.GetKind() == EUeAgentValueKind::Array ? TEXT("a nonnegative captured count") : TEXT("an array")),
			ProposedItems.GetKind() == EUeAgentValueKind::Array); }
		TMap<FString, int32, FDefaultSetAllocator, TExactStringMapKeyFuncs<int32>> CapturedKeys;
		for (int32 Index = 0; Index < CapturedCount; ++Index)
		{
			FString Key;
			if (!TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, OutError)) return false;
			CapturedKeys.Add(Key, Index);
		}
		TSet<int32> UsedCaptured;
		for (const auto& Item : ProposedItems.GetArrayItems())
		{
			if (Item->GetKind() != EUeAgentValueKind::Object)
			{ return FailAt(OutError, TEXT("ordered_structured_item_invalid"), TEXT("A structured-item entry must be an object."), FString(), FString::Printf(TEXT("/%d"), OutItems.Num()), nullptr, MakeShared<FJsonValueString>(TEXT("an object"))); }
			FOrderedStructuredItemBinding Binding;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			for (const auto& Field : Item->GetObjectItems())
			{
				if (Field.Key == TEXT("instance"))
				{
					if (Field.Value->GetKind() != EUeAgentValueKind::String || Field.Value->GetText().IsEmpty())
					{ return FailAt(OutError, TEXT("structured_item_key_invalid"), TEXT("Structured-item instance must be a nonempty string."), FString(), FString::Printf(TEXT("/%d/instance"), OutItems.Num()),
						Field.Value->GetKind() == EUeAgentValueKind::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Field.Value->GetText())) : nullptr,
						MakeShared<FJsonValueString>(TEXT("a nonempty captured entry key"))); }
					const int32* Captured = CapturedKeys.Find(Field.Value->GetText());
					if (!Captured) { return FailAt(OutError, TEXT("structured_item_key_unknown"), TEXT("Structured-item instance does not identify a captured entry."), FString(), FString::Printf(TEXT("/%d/instance"), OutItems.Num()), MakeShared<FJsonValueString>(Field.Value->GetText())); }
					if (UsedCaptured.Contains(*Captured)) { return FailAt(OutError, TEXT("structured_item_key_duplicate"), TEXT("A captured structured-item instance cannot be used more than once."), FString(), FString::Printf(TEXT("/%d/instance"), OutItems.Num()), MakeShared<FJsonValueString>(Field.Value->GetText())); }
					Binding.CapturedIndex = *Captured;
					UsedCaptured.Add(*Captured);
				}
				else Fields.Add(Field);
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Binding.Value, Error))
			{ OutError.SetFailure(Error, FString(), FString::Printf(TEXT("/%d"), OutItems.Num())); return false; }
			OutItems.Add(MoveTemp(Binding));
		}
		return true;
	}

	TSharedPtr<FJsonValue> ProjectPublicOwnedItemDefinition(const TSharedPtr<FJsonValue>& Value)
	{
		if (Value && Value->Type == EJson::Null) return nullptr; // Definition placeholder, not a business null value.
		if (!Value || Value->Type != EJson::Object) return Value;
		const auto Object = Value->AsObject();
		FString Builtin;
		const bool bOwned = Object->TryGetStringField(TEXT("builtin"), Builtin) && Builtin == TEXT("owned_object");
		if (bOwned && !Object->HasField(TEXT("fields"))) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Object->TryGetArrayField(TEXT("items"), Items))
		{
			TArray<TSharedPtr<FJsonValue>> Projected;
			bool bAny = false;
			for (const auto& Item : *Items) { auto Child = ProjectPublicOwnedItemDefinition(Item); bAny |= Child.IsValid(); Projected.Add(Child ? Child : MakeShared<FJsonValueNull>()); }
			if (!bAny && !Object->HasField(TEXT("element_definition"))) return nullptr;
			auto Result = MakeShared<FJsonObject>(); Result->SetArrayField(TEXT("items"), Projected);
			if (Object->HasField(TEXT("element_definition"))) Result->SetField(TEXT("element_definition"), Object->TryGetField(TEXT("element_definition")));
			// This is a value template, not a definition placeholder: preserve nulls.
			if (Object->HasField(TEXT("element_template"))) Result->SetField(TEXT("element_template"), Object->TryGetField(TEXT("element_template")));
			if (Object->HasField(TEXT("element_kind"))) Result->SetField(TEXT("element_kind"), Object->TryGetField(TEXT("element_kind")));
			return MakeShared<FJsonValueObject>(Result);
		}
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		if (Object->TryGetObjectField(TEXT("fields"), Fields))
		{
			auto Projected = MakeShared<FJsonObject>();
			for (const auto& Field : (*Fields)->Values)
				if (auto Child = ProjectPublicOwnedItemDefinition(Field.Value)) Projected->SetField(Field.Key, Child);
			if (Projected->Values.IsEmpty()) return nullptr;
			auto Result = MakeShared<FJsonObject>(); Result->SetObjectField(TEXT("fields"), Projected);
			if (bOwned) Result->SetStringField(TEXT("builtin"), Builtin);
			return MakeShared<FJsonValueObject>(Result);
		}
		return Value;
	}

	namespace
	{
		using FExactItems = TMap<FString, FUeAgentCanonicalValueRef, FDefaultSetAllocator,
			TExactStringMapKeyFuncs<FUeAgentCanonicalValueRef>>;
		void LocateKeyedItemError(FWorkspaceError& Error, const FHjsonNode* Collection,
			const FString& Key, const FString& Field = FString())
		{
			if (!Collection || Error.GetLine() > 0) return;
			const FHjsonNode* Source = Collection->Get(Key);
			if (!Source) Source = Collection;
			const FString FieldKey = Field.IsEmpty() ? Error.GetDiagnostic().PropertyPath : Field;
			if (const FHjsonNode* Property = Source->Get(FieldKey)) Source = Property;
			else if (const FHjsonNode* Properties = Source->Get(TEXT("properties")))
				if (const FHjsonNode* NestedProperty = Properties->Get(FieldKey)) Source = NestedProperty;
			Error.Locate(Source->GetProvenance());
		}

		bool IsMachineField(const FString& Field)
		{
			return Field == TEXT("id")
				|| Field == TEXT("managed_id")
				|| Field == TEXT("actor_guid")
				|| Field == TEXT("object_path")
				|| Field == TEXT("revision")
				|| Field == TEXT("hash")
				|| Field == TEXT("content_hash")
				|| Field == TEXT("guid")
				|| Field == TEXT("operation");
		}

		bool IsForbiddenVisibleField(const FString& Field)
		{
			return Field == TEXT("unit_id")
				|| Field == TEXT("managed_id")
				|| Field == TEXT("owner_managed_id")
				|| Field == TEXT("parent_managed_id")
				|| Field == TEXT("actor_guid")
				|| Field == TEXT("owner_actor_guid")
				|| Field == TEXT("object_path")
				|| Field == TEXT("world_path")
				|| Field == TEXT("guid")
				|| Field == TEXT("hash")
				|| Field == TEXT("content_hash")
				|| Field == TEXT("revision")
				|| Field == TEXT("carrier_revision")
				|| Field == TEXT("captured_values")
				|| Field == TEXT("envelope")
				|| Field == TEXT("operation");
		}

		bool ValidateVisibleValue(const FUeAgentCanonicalValue& Value, const FString& SourceUnit,
			const FString& PropertyPath, FWorkspaceError& OutError, const FHjsonNode* Source = nullptr)
		{
			if (Value.GetKind() == EUeAgentValueKind::Array)
			{
				for (int32 Index = 0; Index < Value.GetArrayItems().Num(); ++Index)
				{
					const auto* Child = Source && Source->GetKind() == EHjsonNodeKind::Array && Source->GetItems().IsValidIndex(Index)
						? &Source->GetItems()[Index].Get() : nullptr;
					if (!ValidateVisibleValue(Value.GetArrayItems()[Index].Get(), SourceUnit,
						PropertyPath + TEXT("/") + FString::FromInt(Index), OutError, Child)) return false;
				}
			}
			else if (Value.GetKind() == EUeAgentValueKind::Object)
			{
				for (const auto& Field : Value.GetObjectItems())
				{
					const FString ChildPath = PropertyPath + TEXT("/") + EscapeJsonPointerSegment(Field.Key);
					const auto* Child = Source ? Source->Get(Field.Key) : nullptr;
					if (IsForbiddenVisibleField(Field.Key))
					{
						Fail(OutError, TEXT("keyed_item_machine_field_forbidden"),
							FString::Printf(TEXT("Field '%s' is reserved and cannot be supplied in visible desired state."), *ChildPath), SourceUnit, ChildPath);
						if (Child) OutError.Locate(Child->GetProvenance());
						return false;
					}
					if (!ValidateVisibleValue(Field.Value.Get(), SourceUnit, ChildPath, OutError, Child)) return false;
				}
			}
			return true;
		}

		TMap<FString, FUeAgentCanonicalValueRef> ObjectMap(
			const FUeAgentCanonicalValue& Object)
		{
			TMap<FString, FUeAgentCanonicalValueRef> Result;
			if (Object.GetKind() == EUeAgentValueKind::Object)
			{
				for (const auto& Field : Object.GetObjectItems())
				{
					Result.Add(Field.Key, Field.Value);
				}
			}
			return Result;
		}

		bool TryObject(
			const TMap<FString, FUeAgentCanonicalValueRef>& Fields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			TArray<FString> Keys;
			Fields.GetKeys(Keys);
			Keys.Sort();
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Ordered;
			for (const FString& Key : Keys)
			{
				Ordered.Emplace(Key, Fields.FindChecked(Key));
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
				Ordered, Resources, OutValue, Error))
			{
				OutError.SetFailure(Error, FString(), Error.Source.JsonPath);
				return false;
			}
			return true;
		}

		bool TryString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FWorkspaceError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
				Text, Resources, OutValue, Error))
			{
				OutError.SetFailure(Error, FString(), Error.Source.JsonPath);
				return false;
			}
			return true;
		}

		bool TryOverlayObjectMembers(const FUeAgentCanonicalValue& Visible,
			const FUeAgentCanonicalValue* Captured, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError)
		{
			struct FFrame
			{
				const FUeAgentCanonicalValue* Visible;
				TMap<FString, FUeAgentCanonicalValueRef> Fields;
				FString ParentField;
				int32 Next = 0;
			};
			TArray<FFrame> Stack;
			Stack.Add({&Visible, Captured ? ObjectMap(*Captured) : TMap<FString, FUeAgentCanonicalValueRef>(), {}});
			while (!Stack.IsEmpty())
			{
				auto& Frame = Stack.Last();
				if (Frame.Next == Frame.Visible->GetObjectItems().Num())
				{
					FUeAgentCanonicalValuePtr Merged;
					if (!TryObject(Frame.Fields, Resources, Merged, OutError)) return false;
					const FString ParentField = Frame.ParentField;
					Stack.Pop(EAllowShrinking::No);
					if (Stack.IsEmpty()) OutValue = Merged;
					else Stack.Last().Fields.Add(ParentField, Merged.ToSharedRef());
					continue;
				}
				const auto& Field = Frame.Visible->GetObjectItems()[Frame.Next++];
				const auto* Before = Frame.Fields.Find(Field.Key);
				if (Field.Value->GetKind() == EUeAgentValueKind::Object && Before
					&& (*Before)->GetKind() == EUeAgentValueKind::Object)
					Stack.Add({&Field.Value.Get(), ObjectMap(Before->Get()), Field.Key});
				else Frame.Fields.Add(Field.Key, Field.Value);
			}
			return true;
		}

		bool TryCapturedItems(const FUeAgentCanonicalValue& Value,
			const FKeyedItemProjectionContract& Contract, const FString& SourceUnit,
			FExactItems& OutItems, FWorkspaceError& OutError)
		{
			if (Contract.CapturedRecordIdentityField.IsEmpty())
			{
				if (Value.GetKind() != EUeAgentValueKind::Object) return FailAt(OutError, TEXT("keyed_item_shape_invalid"), TEXT("Captured keyed items must be an object when their map keys supply identity."), SourceUnit, TEXT("/"), nullptr, MakeShared<FJsonValueString>(TEXT("captured object")), true);
				for (const auto& Item : Value.GetObjectItems()) OutItems.Add(Item.Key, Item.Value);
				return true;
			}
			if (Value.GetKind() != EUeAgentValueKind::Array) return FailAt(OutError, TEXT("keyed_item_shape_invalid"), TEXT("Captured keyed records must be an array when identity is stored in each record."), SourceUnit, TEXT("/"), nullptr, MakeShared<FJsonValueString>(TEXT("captured array")), true);
			int32 RecordIndex = 0;
			int32 RecordOrdinal = 0;
			TSet<FString, FExactStringKeyFuncs> CapturedIdentities;
			TMap<FString, int32> SemanticOccurrences;
			TSet<FString> UsedVisibleKeys;
			auto ReadSemanticName = [&](const FUeAgentCanonicalValue& Record) {
				for (const FString& FieldName : Contract.NamingPolicy.Fields)
				{
					const auto* Field = Record.GetObjectItems().FindByPredicate([&](const auto& Candidate) {
						return Candidate.Key.Equals(FieldName, ESearchCase::CaseSensitive); });
					if (Field && Field->Value->GetKind() == EUeAgentValueKind::String
						&& !Field->Value->GetText().TrimStartAndEnd().IsEmpty()) return Field->Value->GetText();
				}
				return Contract.NamingPolicy.FallbackName;
			};
			TSet<FString> ReservedAliases;
			for (const auto& Record : Value.GetArrayItems())
				if (Record->GetKind() == EUeAgentValueKind::Object)
					ReservedAliases.Add(ReadSemanticName(Record.Get()).TrimStartAndEnd().ToLower());
			for (const auto& Record : Value.GetArrayItems())
			{
				const FString RecordPath = TEXT("/") + Contract.RootField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
					+ TEXT("/") + FString::FromInt(RecordOrdinal++);
				if (Record->GetKind() != EUeAgentValueKind::Object)
				{
					return FailAt(OutError, TEXT("keyed_record_capture_identity_invalid"),
						TEXT("Captured collection record must be an object."), SourceUnit, RecordPath,
						nullptr, MakeShared<FJsonValueString>(TEXT("captured object record")), true);
				}
				const auto* Identity = Record->GetObjectItems().FindByPredicate([&](const auto& Field) {
					return Field.Key.Equals(Contract.CapturedRecordIdentityField, ESearchCase::CaseSensitive); });
				if (!Identity
					|| Identity->Value->GetKind() != EUeAgentValueKind::String || Identity->Value->GetText().IsEmpty()
					|| CapturedIdentities.Contains(Identity->Value->GetText()))
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_record_capture_identity_invalid"),
						!Identity ? TEXT("Captured identity field is missing.")
						: Identity->Value->GetKind() != EUeAgentValueKind::String ? TEXT("Captured identity must be a string.")
						: Identity->Value->GetText().IsEmpty() ? TEXT("Captured identity is empty.")
						: TEXT("Captured identity is duplicated."), TEXT("keyed_item_projection"));
					if (Identity && Identity->Value->GetKind() == EUeAgentValueKind::String)
						Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Identity->Value->GetText());
					Diagnostic.PropertyPath = RecordPath + TEXT("/") + EscapeJsonPointerSegment(Contract.CapturedRecordIdentityField);
					Diagnostic.SourceKind = TEXT("native_result");
					OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
					return false;
				}
				CapturedIdentities.Add(Identity->Value->GetText());
				FString VisibleKey;
				const FString SemanticName = ReadSemanticName(Record.Get());
				if (!Contract.NamingPolicy.Fields.IsEmpty())
				{
					if (!TryAllocateReadableKeyedItemAlias(SemanticName, SemanticOccurrences,
						UsedVisibleKeys, VisibleKey, OutError, &ReservedAliases))
					{
						auto Diagnostic = OutError.GetDiagnostic();
						Diagnostic.SourceKind = TEXT("native_result");
						Diagnostic.Source = {};
						if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = RecordPath;
						OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
						return false;
					}
				}
				else VisibleKey = Contract.VisibleKeyPrefix.IsEmpty()
					? Identity->Value->GetText()
					: Contract.VisibleKeyPrefix + FString::FromInt(++RecordIndex);
				UsedVisibleKeys.Add(VisibleKey.ToLower());
				OutItems.Add(VisibleKey, Record);
			}
			return true;
		}
	}

	const FKeyedItemVisibleFieldPolicy&
	GetActorComponentHierarchyItemVisibleFieldPolicy()
	{
		static const FKeyedItemVisibleFieldPolicy Policy{{
			TEXT("label"), TEXT("name"), TEXT("type"), TEXT("actor_class"),
			TEXT("component_class"), TEXT("component_name")}};
		return Policy;
	}

	bool TryFormatReadableKeyedItemAlias(
		const FString& SemanticName,
		const int32 Occurrence,
		FString& OutAlias,
		FWorkspaceError& OutError)
	{
		OutAlias = SemanticName.TrimStartAndEnd();
		OutError.Reset();
		if (OutAlias.IsEmpty())
		{
			return Fail(OutError, TEXT("keyed_item_semantic_name_missing"), TEXT("A readable item name must contain a non-whitespace character."));
		}
		if (Occurrence < 0)
		{
			return Fail(OutError, TEXT("keyed_item_occurrence_invalid"), FString::Printf(TEXT("Occurrence for item '%s' must be nonnegative."), *OutAlias), FString(), FString(), MakeShared<FJsonValueNumber>(Occurrence));
		}
		OutAlias = MakeNumberedReadableName(OutAlias, Occurrence + 1);
		return true;
	}

	bool TryFormatReadableKeyedItemOrdinal(const FString& Prefix, int32 CapturedIndex,
		FString& OutKey, FWorkspaceError& OutError)
	{
		OutKey.Reset(); OutError.Reset();
		if (Prefix.IsEmpty() || CapturedIndex < 0)
		{ return Fail(OutError, TEXT("keyed_item_occurrence_invalid"), Prefix.IsEmpty() ? TEXT("An ordinal item key requires a nonempty prefix.") : TEXT("An ordinal item key requires a nonnegative captured index."), FString(), FString(), MakeShared<FJsonValueNumber>(CapturedIndex)); }
		OutKey = Prefix + FString::FromInt(CapturedIndex + 1);
		return true;
	}

	bool TryAllocateReadableKeyedItemAlias(const FString& SemanticName,
		TMap<FString, int32>& Occurrences, TSet<FString>& UsedAliases,
		FString& OutAlias, FWorkspaceError& OutError, const TSet<FString>* ReservedAliases)
	{
		FString Base;
		if (!TryFormatReadableKeyedItemAlias(SemanticName, 0, Base, OutError)) return false;
		int32& Occurrence = Occurrences.FindOrAdd(Base.ToLower());
		OutAlias = AllocateReadableName(Base, [&](const FString& Candidate) {
			return UsedAliases.Contains(Candidate.ToLower())
				|| (ReservedAliases && !Candidate.Equals(Base, ESearchCase::IgnoreCase)
					&& ReservedAliases->Contains(Candidate.ToLower()));
		}, ++Occurrence);
		UsedAliases.Add(OutAlias.ToLower());
		return true;
	}

	const TArray<FKeyedItemProjectionContract>& GetKeyedItemProjectionContracts()
	{
		static const TArray<FKeyedItemProjectionContract> Contracts = [] {
		TArray<FKeyedItemProjectionContract> Items{
			{TEXT("actors"), {{TEXT("label"), TEXT("type"), TEXT("folder_path"), TEXT("transform")}}, false,
				{{TEXT("label"), TEXT("name"), TEXT("actor_class")}, TEXT("Actor")}},
			{TEXT("components"), {{TEXT("name"), TEXT("class"), TEXT("parent")}}, false,
				{{TEXT("name"), TEXT("component_name"), TEXT("component_class")}, TEXT("Component")}},
			{TEXT("rows"), {{TEXT("properties")}}, true, {}, {}, {}, {}, true, TEXT("row_schema"), true, {TEXT("table_curve_mode")}},
			{TEXT("entries"), {{TEXT("source"), TEXT("metadata")}}, true, {}, {}, {}, {}, true, {}, false,
				{TEXT("namespace")}, TEXT("key"), false},
			{TEXT("targets"), {{TEXT("native_culture"), TEXT("cultures"), TEXT("settings")}}, true, {}, {}, {}, {}, true, {}, false,
				{}, TEXT("name"), false},
			{TEXT("modules"), {{TEXT("module_index"), TEXT("inputs"), TEXT("assignment_targets"), TEXT("module_kind")}}, false,
				{{TEXT("display_name"), TEXT("name")}, TEXT("Module")}, TEXT(".niagara.module"), TEXT(".niagara.stack_entry"),
				{TEXT("name"), TEXT("usage"), TEXT("emitter_name")}, false},
			{TEXT("widgets"), {{TEXT("parent"), TEXT("index"), TEXT("make_variable")}}, false,
				{{TEXT("name")}, TEXT("Widget")}, TEXT(".umg.widget"), {}, {}, false},
		};
		for (FKeyedItemProjectionContract& Contract : Items)
		{
			if (Contract.RootField == TEXT("modules"))
			{
				Contract.NativeOrderField = TEXT("module_index");
				Contract.DirectCreationFields = {TEXT("assignment_targets")};
			}
			if (Contract.RootField == TEXT("components") || Contract.RootField == TEXT("widgets"))
				Contract.DeltaOnlyFields = {TEXT("parent")};
		}
		FKeyedItemProjectionContract Tree;
		FKeyedItemProjectionContract RigElements;
		RigElements.RootField = TEXT("rig_elements");
		RigElements.VisibleFieldPolicy.VisibleTopLevelFields = {
			TEXT("kind"), TEXT("name"), TEXT("parent"), TEXT("bone_type"),
			TEXT("initial_local_transform"), TEXT("settings"), TEXT("initial_value"),
			TEXT("initial_offset_transform"), TEXT("initial_shape_transform"), TEXT("value")};
		RigElements.bIdentityFromMapKey = true;
		RigElements.bRequiresCreationCandidate = false;
		RigElements.bMergeChildPropertyValues = false;
		RigElements.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(RigElements));
		FKeyedItemProjectionContract RigGraphs;
		RigGraphs.RootField = TEXT("rig_graphs");
		RigGraphs.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("nodes"), TEXT("links")};
		RigGraphs.bIdentityFromMapKey = true;
		RigGraphs.bRequiresCreationCandidate = false;
		RigGraphs.bMergeChildPropertyValues = false;
		RigGraphs.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(RigGraphs));
		const auto AddRigMap = [&Items](const TCHAR* Name, std::initializer_list<FString> Fields)
		{
			FKeyedItemProjectionContract Contract;
			Contract.RootField = Name;
			for (const auto& Field : Fields) Contract.VisibleFieldPolicy.VisibleTopLevelFields.Add(Field);
			Contract.bIdentityFromMapKey = true;
			Contract.bRequiresCreationCandidate = false;
			Contract.bMergeChildPropertyValues = false;
			Contract.bInterpretSemanticValues = false;
			Items.Add(MoveTemp(Contract));
		};
		AddRigMap(TEXT("ik_goals"), {TEXT("bone_name"), TEXT("transform_space"), TEXT("current_transform"), TEXT("properties")});
		AddRigMap(TEXT("ik_solvers"), {TEXT("struct_path"), TEXT("value"), TEXT("enabled"), TEXT("start_bone"), TEXT("end_bone")});
		AddRigMap(TEXT("ik_chains"), {TEXT("start_bone"), TEXT("end_bone"), TEXT("goal")});
		AddRigMap(TEXT("ik_retarget_ops"), {TEXT("struct_path"), TEXT("value"), TEXT("enabled"), TEXT("parent")});
		AddRigMap(TEXT("ik_retarget_mappings"), {TEXT("op"), TEXT("target_chain"), TEXT("source_chain")});
		AddRigMap(TEXT("ik_retarget_poses"), {TEXT("root_translation_space"), TEXT("bone_rotation_space"), TEXT("current_pose"), TEXT("poses")});
		FKeyedItemProjectionContract GraphDeclarations;
		FKeyedItemProjectionContract Emitters;
		Emitters.RootField = TEXT("emitters");
		Emitters.AggregatedItemSemanticSuffix = TEXT(".niagara.emitter_owner");
		Emitters.VisibleFieldPolicy = {{TEXT("enabled"), TEXT("source_asset_path"),
			TEXT("source_sync_mode"), TEXT("emitter_version")}};
		Emitters.ImplicitCapturedFields = {TEXT("name"), TEXT("emitter_version")};
		Emitters.NamingPolicy = {{TEXT("name"), TEXT("emitter_name")}, TEXT("Emitter")};
		Emitters.bIdentityFromMapKey = true;
		Emitters.bEmitOperationRecords = true;
		Emitters.bRequiresCreationCandidate = false;
		Emitters.bMergeChildPropertyValues = false;
		Emitters.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(Emitters));
		FKeyedItemProjectionContract UserParameters;
		UserParameters.RootField = TEXT("user_parameters");
		UserParameters.AggregatedItemSemanticSuffix = TEXT(".niagara.user_parameter");
		UserParameters.VisibleFieldPolicy = {{TEXT("type"), TEXT("value"), TEXT("has_value")}};
		UserParameters.ImplicitCapturedFields = {TEXT("name"), TEXT("kind"), TEXT("value_text")};
		UserParameters.NamingPolicy = {{TEXT("name")}, TEXT("Parameter")};
		UserParameters.bIdentityFromMapKey = true;
		UserParameters.bEmitOperationRecords = true;
		UserParameters.bRequiresCreationCandidate = false;
		UserParameters.bMergeChildPropertyValues = false;
		UserParameters.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(UserParameters));
		GraphDeclarations.RootField = TEXT("graph_declarations");
		GraphDeclarations.VisibleFieldPolicy = {{TEXT("graph_kind"), TEXT("name")}};
		GraphDeclarations.ImplicitCapturedFields = {TEXT("name")};
		GraphDeclarations.bIdentityFromMapKey = true;
		GraphDeclarations.bEmitOperationRecords = true;
		GraphDeclarations.bRequiresCreationCandidate = false;
		GraphDeclarations.bAllowRemoveItems = true;
		GraphDeclarations.bMergeChildPropertyValues = false;
		GraphDeclarations.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(GraphDeclarations));
		FKeyedItemProjectionContract Delegates;
		Delegates.RootField = TEXT("delegates");
		Delegates.AggregatedItemSemanticSuffix = TEXT(".member.delegate");
		Delegates.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("name"), TEXT("signature")};
		Delegates.ImplicitCapturedFields = {TEXT("name")};
		Delegates.bIdentityFromMapKey = true;
		Delegates.bEmitOperationRecords = true;
		Delegates.bRequiresCreationCandidate = false;
		Delegates.bMergeChildPropertyValues = false;
		Delegates.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(Delegates));
		FKeyedItemProjectionContract Variables;
		Variables.RootField = TEXT("variables");
		Variables.AggregatedItemSemanticSuffix = TEXT(".member.variable");
		Variables.VisibleFieldPolicy = {{TEXT("name"), TEXT("type"), TEXT("instance_editable"), TEXT("default_property")}};
		Variables.ImplicitCapturedFields = {TEXT("name")};
		Variables.bIdentityFromMapKey = true;
		Variables.bEmitOperationRecords = true;
		Variables.bRequiresCreationCandidate = false;
		Variables.bMergeChildPropertyValues = false;
		Variables.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(Variables));
		FKeyedItemProjectionContract Interfaces;
		Interfaces.RootField = TEXT("interfaces");
		Interfaces.VisibleFieldPolicy = {{TEXT("class_path")}};
		Interfaces.bIdentityFromMapKey = true;
		Interfaces.bEmitOperationRecords = true;
		Interfaces.bRequiresCreationCandidate = false;
		Interfaces.bMergeChildPropertyValues = false;
		Interfaces.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(Interfaces));
		FKeyedItemProjectionContract UmgBindings;
		UmgBindings.RootField = TEXT("bindings");
		UmgBindings.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("widget_name"), TEXT("property_name"),
			TEXT("binding_kind"), TEXT("source_variable_name"), TEXT("function_name")};
		UmgBindings.NamingPolicy = {{TEXT("widget_name")}, TEXT("Binding")};
		UmgBindings.AggregatedItemSemanticSuffix = TEXT(".umg.binding");
		UmgBindings.bMergeChildPropertyValues = false;
		UmgBindings.bRequiresCreationCandidate = false;
		Items.Add(MoveTemp(UmgBindings));
		FKeyedItemProjectionContract Folders;
		Folders.RootField = TEXT("folders");
		Folders.VisibleFieldPolicy = {{TEXT("name"), TEXT("parent"), TEXT("bindings"), TEXT("root_tracks")}};
		Folders.bIdentityFromMapKey = true;
		Folders.bEmitOperationRecords = true;
		Folders.bRequiresCreationCandidate = false;
		Folders.bInterpretSemanticValues = false;
		Items.Add(MoveTemp(Folders));
		// Standalone MovieScene roots use the same contract as tracks nested in
		// WidgetAnimation. Keep discovery, physical grouping and examples aligned.
		Items.Add(GetLevelSequenceRootScalarTrackProjectionContract());
		FKeyedItemProjectionContract UmgAnimations;
		UmgAnimations.RootField = TEXT("animations");
		UmgAnimations.VisibleFieldPolicy = {{TEXT("display_label"), TEXT("display_rate"), TEXT("tick_resolution"), TEXT("playback_range"), TEXT("bindings"), TEXT("root_tracks")}};
		// The same exact bound projection as sections retains clock identity in captured context.
		for (const auto& Policy : GetLevelSequenceScalarSectionProjectionContract().ObjectFieldPolicies)
		{
			const FString Path = TEXT("playback_range") + Policy.Key.RightChop(5);
			UmgAnimations.ObjectFieldPolicies.Add(Path, Policy.Value);
		}
		UmgAnimations.NamingPolicy = {{TEXT("name")}, TEXT("Animation")};
		UmgAnimations.AggregatedItemSemanticSuffix = TEXT(".umg.animation");
		UmgAnimations.bIdentityFromMapKey = true;
		UmgAnimations.bEmitOperationRecords = true;
		UmgAnimations.bInterpretSemanticValues = false;
		UmgAnimations.bMergeChildPropertyValues = false;
		UmgAnimations.bRequiresCreationCandidate = false;
		UmgAnimations.CollectionFieldContracts.Add(TEXT("bindings"), &GetLevelSequenceBindingProjectionContract(false));
		UmgAnimations.CollectionFieldContracts.Add(TEXT("root_tracks"), &GetLevelSequenceRootScalarTrackProjectionContract());
		Items.Add(MoveTemp(UmgAnimations));
		Tree.RootField = TEXT("tree_items");
		Tree.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("kind"), TEXT("label"), TEXT("parent"), TEXT("index"),
			TEXT("placement"), TEXT("target"), TEXT("transition_type"), TEXT("parameter_overrides"),
			TEXT("source"), TEXT("source_path"), TEXT("target_path"), TEXT("source_type"), TEXT("linked_state"), TEXT("linked_state_type"), TEXT("color_ref")};
		Tree.NamingPolicy = {{TEXT("label")}, TEXT("Item")};
		Tree.AggregatedItemSemanticSuffix = TEXT(".state_tree.item");
		Tree.AggregatedParentSemanticSuffix = TEXT(".state_tree.tree");
		Tree.ReadOnlyRootFields = {TEXT("schema_class")};
		Tree.bMergeChildPropertyValues = false;
		Tree.bRetainUnchangedOperationRecords = true;
		Tree.OrderRankField = TEXT("index");
		Tree.OrderRankGroupFields = {TEXT("kind"), TEXT("parent"), TEXT("placement")};
		Tree.OrderRankTieIdentityField = TEXT("id");
		Tree.UnorderedKinds = {TEXT("settings"), TEXT("parameters"), TEXT("color"), TEXT("binding_source")};
		Tree.DerivedIdentityFieldsByKind = {{TEXT("binding"), {TEXT("target"), TEXT("target_path")}}};
		Tree.ReferenceIdentityField = TEXT("id");
		Tree.ReferenceFields = {{TEXT("parent"), {}}, {TEXT("target"), {}}, {TEXT("source"), {}}, {TEXT("linked_state"), {}}, {TEXT("color_ref"), {}}};
		Items.Add(MoveTemp(Tree));
		FKeyedItemProjectionContract Blackboard;
		Blackboard.RootField = TEXT("blackboard_keys");
		Blackboard.VisibleFieldPolicy.VisibleTopLevelFields = {
			TEXT("key_type"), TEXT("index"), TEXT("description"), TEXT("category"),
			TEXT("instance_synced"), TEXT("candidate")};
		Blackboard.NamingPolicy = {{TEXT("name")}, TEXT("Key")};
		Blackboard.AggregatedItemSemanticSuffix = TEXT(".blackboard.key");
		Blackboard.AggregatedParentSemanticSuffix = TEXT(".blackboard.document");
		Blackboard.ReadOnlyRootFields = {TEXT("inherited_keys")};
		Blackboard.EditableRootFields = {TEXT("parent")};
		Blackboard.bMergeChildPropertyValues = false;
		Blackboard.bRetainUnchangedOperationRecords = true;
		Blackboard.NativeOrderField = TEXT("index");
		Blackboard.OrderRankField = TEXT("index");
		Items.Add(MoveTemp(Blackboard));
		FKeyedItemProjectionContract SmartObject;
		SmartObject.RootField = TEXT("smart_object_items");
		SmartObject.VisibleFieldPolicy.VisibleTopLevelFields = {TEXT("kind"), TEXT("label"), TEXT("parent"), TEXT("index"),
			TEXT("source"), TEXT("target"), TEXT("source_path"), TEXT("target_path"), TEXT("linked_slot")};
		SmartObject.NamingPolicy = {{TEXT("label")}, TEXT("Item")};
		SmartObject.AggregatedItemSemanticSuffix = TEXT(".smart_object.item");
		SmartObject.AggregatedParentSemanticSuffix = TEXT(".smart_object.definition");
		SmartObject.bMergeChildPropertyValues = false;
		SmartObject.bRetainUnchangedOperationRecords = true;
		SmartObject.OrderRankField = TEXT("index");
		SmartObject.OrderRankGroupFields = {TEXT("kind"), TEXT("parent")};
		SmartObject.UnorderedKinds = {TEXT("settings"), TEXT("parameters")};
		SmartObject.DerivedIdentityFieldsByKind = {
			{TEXT("behavior"), {TEXT("parent"), TEXT("index")}},
			{TEXT("binding"), {TEXT("target"), TEXT("target_path")}}};
		SmartObject.ReferenceIdentityField = TEXT("id");
		SmartObject.ReferenceFields = {{TEXT("parent"), {}}, {TEXT("source"), {}}, {TEXT("target"), {}}, {TEXT("linked_slot"), {}}};
		Items.Add(MoveTemp(SmartObject));
		return Items;
		}();
		return Contracts;
	}

	const FKeyedItemProjectionContract& GetLevelSequenceScalarKeyProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract{
			TEXT("keys"),
			{{TEXT("time"), TEXT("value_type"), TEXT("value_lexeme"), TEXT("value"), TEXT("properties")}},
			true,
			{}, {}, {}, {}, true, {}, false, {}, TEXT("key_ref"), false,
			TEXT("Key"), {{TEXT("time"), {{TEXT("tick"), TEXT("rate"), TEXT("state")}}}}};
		return Contract;
	}

	const FKeyedItemProjectionContract& GetLevelSequenceScalarChannelProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract = [] {
			FKeyedItemProjectionContract Result;
			Result.RootField = TEXT("channels");
			Result.VisibleFieldPolicy = {{TEXT("keys"), TEXT("default"), TEXT("properties")}};
			Result.ObjectFieldPolicies = {{TEXT("default"), {{TEXT("is_set"), TEXT("value_lexeme"), TEXT("value")}}}};
			auto& Default = Result.ObjectFieldPolicies.FindChecked(TEXT("default"));
			Default.DiscriminatorField = TEXT("is_set");
			Default.BranchFields = {{TEXT("false"), {TEXT("is_set")}},
				{TEXT("true"), {TEXT("is_set"), TEXT("value_lexeme"), TEXT("value")}}};
			Default.ExclusiveBranchFields = {{TEXT("value_lexeme"), TEXT("value")}};
			Result.bIdentityFromMapKey = true;
			Result.NamingPolicy = {{TEXT("name")}, TEXT("Channel")};
			Result.CapturedRecordIdentityField = TEXT("channel_ref");
			Result.bInterpretSemanticValues = false;
			Result.bAllowNewItems = false;
			Result.bAllowRemoveItems = false;
			Result.CollectionFieldContracts.Add(TEXT("keys"), &GetLevelSequenceScalarKeyProjectionContract());
			Result.ChildRecordDefaultObjectSources.Add(TEXT("keys"), TEXT("default_key"));
			return Result;
		}();
		return Contract;
	}

	const FKeyedItemProjectionContract& GetLevelSequenceScalarSectionProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract = [] {
			FKeyedItemProjectionContract Result;
			Result.RootField = TEXT("sections");
			Result.VisibleFieldPolicy = {{TEXT("range"), TEXT("channels"), TEXT("properties"), TEXT("native_channels")}};
			Result.ReadOnlyRootFields.Add(TEXT("native_channels"));
			Result.bIdentityFromMapKey = true;
			Result.CapturedRecordIdentityField = TEXT("section_ref");
			Result.bInterpretSemanticValues = false;
			Result.VisibleKeyPrefix = TEXT("Section");
			Result.ObjectFieldPolicies = {
				{TEXT("range"), {{TEXT("lower"), TEXT("upper"), TEXT("state")}}},
				{TEXT("range/lower"), {{TEXT("kind"), TEXT("point")}}},
				{TEXT("range/upper"), {{TEXT("kind"), TEXT("point")}}},
				{TEXT("range/lower/point"), {{TEXT("tick"), TEXT("rate"), TEXT("state")}}},
				{TEXT("range/upper/point"), {{TEXT("tick"), TEXT("rate"), TEXT("state")}}}};
			Result.CollectionFieldContracts.Add(TEXT("channels"), &GetLevelSequenceScalarChannelProjectionContract());
			for (const FString Path : {FString(TEXT("range/lower")), FString(TEXT("range/upper"))})
			{
				auto& Bound = Result.ObjectFieldPolicies.FindChecked(Path);
				Bound.DiscriminatorField = TEXT("kind");
				Bound.BranchFields = {
					{TEXT("open"), {TEXT("kind")}},
					{TEXT("inclusive"), {TEXT("kind"), TEXT("point")}},
					{TEXT("exclusive"), {TEXT("kind"), TEXT("point")}}};
			}
			return Result;
		}();
		return Contract;
	}

	namespace
	{
		FKeyedItemProjectionContract MakeLevelSequenceScalarTrackProjectionContract(const TCHAR* RootField)
		{
			FKeyedItemProjectionContract Result;
			Result.RootField = RootField;
			Result.VisibleFieldPolicy = {{TEXT("name"), TEXT("select"), TEXT("display_name"), TEXT("track_kind"), TEXT("property_path"), TEXT("sections"), TEXT("properties")}};
			Result.bIdentityFromMapKey = true;
			Result.NamingPolicy = {{TEXT("display_name")}, TEXT("Track")};
			Result.CapturedRecordIdentityField = TEXT("track_ref");
			Result.bInterpretSemanticValues = false;
			Result.ReadOnlyRootFields = {TEXT("section_definitions"), TEXT("channel_definitions"),
				TEXT("key_definitions"), TEXT("object_property_definitions")};
			Result.CollectionFieldContracts.Add(TEXT("sections"), &GetLevelSequenceScalarSectionProjectionContract());
			Result.ChildRecordDefaultSources.Add(TEXT("sections"), {{TEXT("section_kind"), TEXT("track_kind")},
				{TEXT("channels"), TEXT("default_channels")}, {TEXT("properties"), TEXT("default_properties")},
				{TEXT("native_channels"), TEXT("default_native_channels")},
				{TEXT("range"), TEXT("default_range")}});
			return Result;
		}
	}

	const FKeyedItemProjectionContract& GetLevelSequenceRootScalarTrackProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract =
			MakeLevelSequenceScalarTrackProjectionContract(TEXT("root_tracks"));
		return Contract;
	}

	const FKeyedItemProjectionContract& GetLevelSequenceBindingScalarTrackProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract =
			MakeLevelSequenceScalarTrackProjectionContract(TEXT("tracks"));
		return Contract;
	}

	const FKeyedItemProjectionContract& GetLevelSequenceFolderProjectionContract()
	{
		static const FKeyedItemProjectionContract Contract = [] {
			FKeyedItemProjectionContract Result;
			Result.RootField = TEXT("folders"); Result.bIdentityFromMapKey = true;
			Result.CapturedRecordIdentityField = TEXT("folder_ref"); Result.ReferenceIdentityField = TEXT("folder_ref");
			Result.NamingPolicy = {{TEXT("name")}, TEXT("Folder")};
			Result.VisibleFieldPolicy = {{TEXT("name"), TEXT("parent"), TEXT("bindings"), TEXT("root_tracks")}};
			Result.bRequiresCreationCandidate = false; Result.bInterpretSemanticValues = false;
			Result.bUseNewItemReferenceForIdentity = true;
			Result.ReferenceFields = {{TEXT("parent"), {TEXT(""), EKeyedItemReferenceShape::Single}},
				{TEXT("bindings"), {TEXT("bindings"), EKeyedItemReferenceShape::Set}},
				{TEXT("root_tracks"), {TEXT("root_tracks"), EKeyedItemReferenceShape::Set}}};
			return Result;
		}();
		return Contract;
	}
	const FKeyedItemProjectionContract& GetLevelSequenceBindingProjectionContract(bool bIncludeLocalBindingTags)
	{
		static const FKeyedItemProjectionContract Contract = [] {
			FKeyedItemProjectionContract Result;
			Result.RootField = TEXT("bindings");
			Result.VisibleFieldPolicy = {{TEXT("tracks")}};
			Result.bIdentityFromMapKey = true;
			Result.NamingPolicy = {{TEXT("display_name")}, TEXT("Binding")};
			Result.CapturedRecordIdentityField = TEXT("binding_ref");
			Result.bInterpretSemanticValues = false;
			Result.bAllowNewItems = false;
			Result.bAllowRemoveItems = true;
			Result.CollectionFieldContracts.Add(TEXT("tracks"), &GetLevelSequenceBindingScalarTrackProjectionContract());
			return Result;
		}();
		static const FKeyedItemProjectionContract WithLocalTags = [] {
			auto Result = Contract;
			Result.VisibleFieldPolicy = {{TEXT("tracks"), TEXT("tags"), TEXT("binding_kind"), TEXT("object_ref"), TEXT("properties"), TEXT("template_properties")}};
			Result.bAllowNewItems = true;
			Result.bRequiresCreationCandidate = false;
			return Result;
		}();
		static const FKeyedItemProjectionContract WidgetBindings = [] {
			auto Result = Contract;
			Result.VisibleFieldPolicy = {{TEXT("tracks"), TEXT("binding_kind"), TEXT("object_ref")}};
			Result.bAllowNewItems = true;
			Result.bRequiresCreationCandidate = false;
			return Result;
		}();
		return bIncludeLocalBindingTags ? WithLocalTags : WidgetBindings;
	}

	namespace
	{
		bool TryProjectRecordObject(const FUeAgentCanonicalValue& Value,
			const FString& Path, const FKeyedItemProjectionContract& Contract,
			FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError)
		{
			const auto* Policy = Contract.ObjectFieldPolicies.Find(Path);
			if (!Policy) { OutValue = MakeShared<FUeAgentCanonicalValue>(Value); return true; }
			if (Value.GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, TEXT("keyed_record_object_required"), FString::Printf(TEXT("Record field '%s' must be an object."), *Path), FString(), Path);
			TMap<FString, FUeAgentCanonicalValueRef> Fields;
			for (const auto& Field : Value.GetObjectItems())
			{
				if (!Policy->Allows(Field.Key)) continue;
				FUeAgentCanonicalValuePtr Child;
				if (!TryProjectRecordObject(Field.Value.Get(), Path + TEXT("/") + Field.Key,
					Contract, Resources, Child, OutError)) return false;
				Fields.Add(Field.Key, Child.ToSharedRef());
			}
			return TryObject(Fields, Resources, OutValue, OutError);
		}

		bool TryMergeRecordObject(const FUeAgentCanonicalValue& Value, const FUeAgentCanonicalValue* Captured,
			const FUeAgentCanonicalValue* Defaults, const FString& Path, const FKeyedItemProjectionContract& Contract,
			FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
			const FString& InputPointer)
		{
			// Policy keys retain their existing spelling. Input pointers independently
			// escape each segment and retain provenance without reconstructing syntax.
			auto Reject = [&](const TCHAR* Code, const FString& Reason, const FString& Field = FString(),
				TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr,
				bool bNativeResult = false) {
				const FString Pointer = Field.IsEmpty() ? InputPointer : InputPointer + TEXT("/") + EscapeJsonPointerSegment(Field);
				return FailAt(OutError, Code, Reason, FString(), Pointer,
					MoveTemp(Actual), MoveTemp(Expected), bNativeResult);
			};
			const auto* Policy = Contract.ObjectFieldPolicies.Find(Path);
			if (!Policy)
			{
				if (Value.GetKind() != EUeAgentValueKind::Object)
				{ OutValue = MakeShared<FUeAgentCanonicalValue>(Value); return true; }
				return TryOverlayObjectMembers(Value, Captured, Resources, OutValue, OutError);
			}
			if (Value.GetKind() != EUeAgentValueKind::Object)
				return Reject(TEXT("keyed_record_object_required"), FString::Printf(TEXT("Record field '%s' must be an object."), *Path));
			auto Fields = Captured && Captured->GetKind() == EUeAgentValueKind::Object
				? ObjectMap(*Captured) : TMap<FString, FUeAgentCanonicalValueRef>();
			const TSet<FString>* Branch = nullptr;
			if (!Policy->DiscriminatorField.IsEmpty())
			{
				const auto* VisibleKind = OwnedDefinitionField(Value, Policy->DiscriminatorField);
				const auto* Kind = VisibleKind;
				if (!Kind && Captured) Kind = OwnedDefinitionField(*Captured, Policy->DiscriminatorField);
				if (!Kind && Defaults) Kind = OwnedDefinitionField(*Defaults, Policy->DiscriminatorField);
				if (Kind && Kind->GetKind() == EUeAgentValueKind::String)
					Branch = Policy->BranchFields.Find(Kind->GetText());
				else if (Kind && Kind->GetKind() == EUeAgentValueKind::Boolean)
					Branch = Policy->BranchFields.Find(Kind->GetBoolean() ? TEXT("true") : TEXT("false"));
				if (!Branch)
				{
					TArray<FString> BranchNames;
					Policy->BranchFields.GetKeys(BranchNames);
					BranchNames.Sort();
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (const FString& Name : BranchNames) Allowed.Add(MakeShared<FJsonValueString>(Name));
					TSharedPtr<FJsonValue> Actual;
					if (Kind && Kind->GetKind() == EUeAgentValueKind::String) Actual = MakeShared<FJsonValueString>(Kind->GetText());
					else if (Kind && Kind->GetKind() == EUeAgentValueKind::Boolean) Actual = MakeShared<FJsonValueBoolean>(Kind->GetBoolean());
					return Reject(TEXT("keyed_record_branch_invalid"), FString::Printf(TEXT("Discriminator '%s': %s"),
						*Policy->DiscriminatorField, !Kind ? TEXT("required field is missing.")
						: !Actual ? TEXT("value must be a string or boolean.") : TEXT("value does not select a supported branch.")),
						Policy->DiscriminatorField, Actual, MakeShared<FJsonValueArray>(MoveTemp(Allowed)), Kind && !VisibleKind);
				}
				for (const FString& Name : Policy->VisibleTopLevelFields)
					if (!Branch->Contains(Name)) Fields.Remove(Name);
				for (const FString& Name : *Branch)
					if (!Fields.Contains(Name))
					{
						const auto* Default = Defaults ? OwnedDefinitionField(*Defaults, Name) : nullptr;
						if (Default) Fields.Add(Name, MakeShared<FUeAgentCanonicalValue>(*Default));
					}
			}
			for (const auto& Field : Value.GetObjectItems())
			{
				if (!Policy->Allows(Field.Key)) return Reject(TEXT("keyed_record_field_unknown"), FString::Printf(TEXT("Field '%s' is not allowed by this record contract."), *Field.Key), Field.Key);
				if (Branch && !Branch->Contains(Field.Key))
					return Reject(TEXT("keyed_record_branch_field_forbidden"), FString::Printf(TEXT("Field '%s' is not allowed in the selected '%s' branch."), *Field.Key, *Policy->DiscriminatorField), Field.Key);
				const auto* Previous = Fields.Find(Field.Key);
				const auto* Default = Defaults ? OwnedDefinitionField(*Defaults, Field.Key) : nullptr;
				FUeAgentCanonicalValuePtr Child;
				if (!TryMergeRecordObject(Field.Value.Get(), Previous ? &Previous->Get() : nullptr, Default,
					Path + TEXT("/") + Field.Key, Contract, Resources, Child, OutError,
					InputPointer + TEXT("/") + EscapeJsonPointerSegment(Field.Key))) return false;
				Fields.Add(Field.Key, Child.ToSharedRef());
			}
			if (Branch)
			{
				TSet<FString> Alternatives;
				for (const auto& Group : Policy->ExclusiveBranchFields)
				{
					int32 Applicable = 0, Present = 0;
					for (const auto& Name : Group) if (Branch->Contains(Name)) { ++Applicable; Present += Fields.Contains(Name) ? 1 : 0; Alternatives.Add(Name); }
					if (Applicable && Present != 1)
					{
						TArray<FString> Names;
						for (const auto& Name : Group) if (Branch->Contains(Name)) Names.Add(Name);
						Names.Sort();
						return Reject(TEXT("keyed_record_branch_exclusive_field_required"),
							FString::Printf(TEXT("Exactly one of [%s] is required; %d are present."), *FString::Join(Names, TEXT(", ")), Present));
					}
				}
				for (const FString& Name : *Branch)
					if (!Alternatives.Contains(Name) && !Fields.Contains(Name)) return Reject(TEXT("keyed_record_branch_field_required"), FString::Printf(TEXT("The selected branch requires field '%s'."), *Name), Name);
			}
			return TryObject(Fields, Resources, OutValue, OutError);
		}
	}

	bool TryGetKeyedItemProjectionContract(
		const FString& RootField,
		FKeyedItemProjectionContract& OutContract)
	{
		for (const FKeyedItemProjectionContract& Contract : GetKeyedItemProjectionContracts())
		{
			if (RootField == Contract.RootField)
			{
				OutContract = Contract;
				return true;
			}
		}
		return false;
	}

	bool TryProjectItemVisibleObject(
		const FString& SourceUnit,
		const TSharedRef<FJsonObject>& CapturedItem,
		const FKeyedItemVisibleFieldPolicy& FieldPolicy,
		TSharedRef<FJsonObject>& OutVisibleItem,
		FWorkspaceError& OutError)
	{
		OutError.Reset();
		if (!FieldPolicy.IsValid())
		{
			return Fail(OutError, TEXT("keyed_item_visible_field_policy_invalid"), TEXT("The item contract has no valid visible-field policy."), SourceUnit);
		}
		OutVisibleItem = MakeShared<FJsonObject>();
		for (const auto& Field : CapturedItem->Values)
		{
			if (FieldPolicy.Allows(Field.Key))
			{
				OutVisibleItem->SetField(Field.Key, Field.Value);
			}
		}
		return true;
	}

	bool TryResolveCapturedItemDesiredState(
		const FUeAgentCanonicalValue& VisibleItem,
		const FUeAgentCanonicalValue& CapturedItem,
		const FString& SourceUnit,
		const FKeyedItemVisibleFieldPolicy& FieldPolicy,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutResolvedItem,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError)
	{
		OutResolvedItem.Reset();
		bOutSemanticChanged = false;
		OutError.Reset();
		if (!FieldPolicy.IsValid()
			|| VisibleItem.GetKind() != EUeAgentValueKind::Object
			|| CapturedItem.GetKind() != EUeAgentValueKind::Object)
		{
			return Fail(OutError, TEXT("keyed_item_shape_invalid"), !FieldPolicy.IsValid()
				? TEXT("The item contract has no valid visible-field policy.")
				: VisibleItem.GetKind() != EUeAgentValueKind::Object ? TEXT("The visible item must be an object.")
				: TEXT("The captured item must be an object."), SourceUnit);
		}
		auto Resolved = ObjectMap(CapturedItem);
		for (const auto& Field : VisibleItem.GetObjectItems())
		{
			if (!FieldPolicy.Allows(Field.Key)
				|| IsMachineField(Field.Key)
				|| IsForbiddenVisibleField(Field.Key))
			{
				return Fail(OutError, TEXT("keyed_item_machine_field_forbidden"), FString::Printf(TEXT("Field '%s' is not an allowed visible field."), *Field.Key), SourceUnit, Field.Key);
			}
			if (!ValidateVisibleValue(Field.Value.Get(), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key), OutError)) return false;
			const FUeAgentCanonicalValueRef* CapturedField = Resolved.Find(Field.Key);
			if (CapturedField == nullptr)
			{
				bOutSemanticChanged = true;
			}
			else
			{
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError SemanticError;
				if (!TryCompareValues(*CapturedField, Field.Value,
					TEXT("/") + Field.Key, Resources, Comparison, SemanticError))
				{
					OutError.SetFailure(SemanticError, SourceUnit);
					return false;
				}
				bOutSemanticChanged |= !Comparison.IsEquivalent();
			}
			Resolved.Add(Field.Key, Field.Value);
		}
		return TryObject(Resolved, Resources, OutResolvedItem, OutError);
	}

	namespace
	{
		bool TryTransformKeyedReferences(const FUeAgentCanonicalValue& Items,
			const FUeAgentCanonicalValue& CapturedItems, const FKeyedItemProjectionContract& Contract,
			bool bProject, const FString& SourceUnit, FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutItems, FWorkspaceError& OutError,
			const FKeyedItemReferenceContext* Context, bool bOperationRecords = false,
			const FHjsonNode* VisibleSource = nullptr)
		{
			if (Items.GetKind() != EUeAgentValueKind::Object) return Fail(OutError, TEXT("keyed_reference_contract_invalid"), TEXT("A keyed-reference collection must be an object."), SourceUnit);
			FExactItems SourceCaptured;
			if (!TryCapturedItems(CapturedItems, Contract, SourceUnit, SourceCaptured, OutError)) return false;
			auto IsRemoved = [](const FUeAgentCanonicalValue& Row, bool bInternal) {
				const auto* Op = bInternal ? OwnedDefinitionField(Row, TEXT("operation")) : nullptr;
				return Op && Op->GetKind() == EUeAgentValueKind::String && Op->GetText() == TEXT("remove");
			};
			using FReferenceMap = TMap<FString, FString, FDefaultSetAllocator, TExactStringMapKeyFuncs<FString>>;
			struct FTarget { FReferenceMap KeyByIdentity, IdentityByKey, UnresolvedIdentityByKey; TSet<FString, FExactStringKeyFuncs> Desired; };
			TMap<FString, FTarget> Targets;
			for (const auto& Field : Contract.ReferenceFields)
			{
				if (!Contract.VisibleFieldPolicy.Allows(Field.Key)) return Fail(OutError, TEXT("keyed_reference_contract_invalid"), FString::Printf(TEXT("Reference field '%s' is absent from the collection visible-field policy."), *Field.Key), SourceUnit, Field.Key);
				const auto& Spec = Field.Value;
				if (Targets.Contains(Spec.TargetCollection)) continue;
				FKeyedItemReferenceCollection Self{&Contract, &CapturedItems, &Items, bOperationRecords};
				const auto* Collection = Spec.TargetCollection.IsEmpty() ? &Self : (Context ? Context->Find(Spec.TargetCollection) : nullptr);
				if (!Collection || !Collection->Contract || !Collection->CapturedItems)
					return Fail(OutError, TEXT("keyed_reference_collection_missing"), FString::Printf(TEXT("Reference field '%s' requires captured target collection '%s' and its contract."), *Field.Key, *Spec.TargetCollection), SourceUnit, Field.Key);
				const FString IdentityField = Collection->Contract->ReferenceIdentityField.IsEmpty()
					? Collection->Contract->CapturedRecordIdentityField : Collection->Contract->ReferenceIdentityField;
				if (IdentityField.IsEmpty()) return Fail(OutError, TEXT("keyed_reference_contract_invalid"), FString::Printf(TEXT("Reference field '%s' has no target identity-field contract."), *Field.Key), SourceUnit, Field.Key);
				FExactItems Captured;
				if (!TryCapturedItems(*Collection->CapturedItems, *Collection->Contract, SourceUnit, Captured, OutError)) return false;
				FTarget Target;
				for (const auto& Item : Captured)
				{
					const auto* Identity = OwnedDefinitionField(Item.Value.Get(), IdentityField);
					if (!Identity || Identity->GetKind() != EUeAgentValueKind::String || Identity->GetText().IsEmpty() || Target.KeyByIdentity.Contains(Identity->GetText()))
						return Fail(OutError, TEXT("keyed_reference_identity_invalid"), FString::Printf(TEXT("Captured item '%s': %s"), *Item.Key, !Identity ? TEXT("reference identity is missing.") : Identity->GetKind() != EUeAgentValueKind::String ? TEXT("reference identity must be a string.") : Identity->GetText().IsEmpty() ? TEXT("reference identity is empty.") : TEXT("reference identity is duplicated.")), SourceUnit);
					Target.KeyByIdentity.Add(Identity->GetText(), Item.Key); Target.IdentityByKey.Add(Item.Key, Identity->GetText());
					if (bProject || !Collection->DesiredItems) Target.Desired.Add(Item.Key);
				}
				// Native editors can retain invalid references. Give those captured
				// identities readable handles too, without inventing target items.
				TSet<FString, FExactStringKeyFuncs> UnresolvedIdentities;
				for (const auto& Source : SourceCaptured)
					for (const auto& Reference : Contract.ReferenceFields)
					{
						if (Reference.Value.TargetCollection != Spec.TargetCollection) continue;
						const auto* Value = OwnedDefinitionField(Source.Value.Get(), Reference.Key);
						if (!Value) continue;
						auto Collect = [&](const FUeAgentCanonicalValue& Entry) {
							if (Entry.GetKind() == EUeAgentValueKind::String && !Entry.GetText().IsEmpty()
								&& !Target.KeyByIdentity.Contains(Entry.GetText())) UnresolvedIdentities.Add(Entry.GetText());
						};
						if (Reference.Value.Shape == EKeyedItemReferenceShape::Set && Value->GetKind() == EUeAgentValueKind::Array)
							for (const auto& Entry : Value->GetArrayItems()) Collect(Entry.Get());
						else Collect(*Value);
					}
				TArray<FString> OrderedUnresolved = UnresolvedIdentities.Array(); OrderedUnresolved.Sort();
				TMap<FString, int32> Occurrences;
				TSet<FString> UsedAliases;
				for (const auto& Entry : Target.IdentityByKey) UsedAliases.Add(Entry.Key.ToLower());
				for (const FString& Identity : OrderedUnresolved)
				{
					FString Alias;
					if (!TryAllocateReadableKeyedItemAlias(TEXT("Unresolved reference"), Occurrences, UsedAliases, Alias, OutError)) return false;
					Target.KeyByIdentity.Add(Identity, Alias);
					Target.UnresolvedIdentityByKey.Add(Alias, Identity);
				}
				if (!bProject && Collection->DesiredItems)
				{
					if (Collection->DesiredItems->GetKind() != EUeAgentValueKind::Object) return Fail(OutError, TEXT("keyed_reference_collection_invalid"), FString::Printf(TEXT("Desired target collection '%s' must be an object."), *Spec.TargetCollection), SourceUnit);
					for (const auto& Item : Collection->DesiredItems->GetObjectItems())
						if (!IsRemoved(Item.Value.Get(), Collection->bDesiredOperationRecords)) Target.Desired.Add(Item.Key);
				}
				Targets.Add(Spec.TargetCollection, MoveTemp(Target));
			}
			FUeAgentStateSemanticError Error;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Rows;
			FWorkspaceError ItemErrors;
			for (const auto& Item : Items.GetObjectItems())
			{
				FWorkspaceError ItemError;
				auto ResolveItem = [&](FWorkspaceError& OutError) -> bool {
					if (Item.Value->GetKind() != EUeAgentValueKind::Object) return Fail(OutError, TEXT("keyed_item_shape_invalid"), FString::Printf(TEXT("Item '%s' must be an object."), *Item.Key), SourceUnit);
					if (!bProject && IsRemoved(Item.Value.Get(), bOperationRecords)) { Rows.Add(Item); return true; }
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
					for (const auto& Field : Item.Value->GetObjectItems()) if (!Contract.ReferenceFields.Contains(Field.Key)) Fields.Add(Field);
					for (const auto& Field : Contract.ReferenceFields)
					{
						FWorkspaceError FieldError;
						auto ResolveReference = [&](FWorkspaceError& OutError) -> bool {
							const auto& Spec = Field.Value; const auto& Target = Targets.FindChecked(Spec.TargetCollection);
							const auto* Before = SourceCaptured.Find(Item.Key);
							const auto* Original = Before ? OwnedDefinitionField(Before->Get(), Field.Key) : nullptr;
							const auto* Value = OwnedDefinitionField(Item.Value.Get(), Field.Key);
							bool bCaptured = bProject;
							if (!Value && !bProject) { Value = Original; bCaptured = true; }
							if (!Value) return true;
							auto ReadKeys = [&](const FUeAgentCanonicalValue& Input, bool bIdentity, TArray<FString>& Keys) {
								auto Reject = [&](const FString& Reason, int32 Index = INDEX_NONE, const FString* Key = nullptr) {
									auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_reference_value_invalid"),
										FString::Printf(TEXT("Reference '%s.%s': %s"), *Item.Key, *Field.Key, *Reason), TEXT("keyed_item_projection"));
									Diagnostic.PropertyPath = Field.Key;
									if (Key) {
										if (bIdentity) Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(*Key);
										else Diagnostic.ActualValue = MakeShared<FJsonValueString>(*Key);
									}
									OutError.SetFailure(MoveTemp(Diagnostic), SourceUnit);
									if (VisibleSource && !bIdentity) {
										const auto* Record = VisibleSource->Get(Item.Key);
										const auto* Node = Record ? Record->Get(Field.Key) : nullptr;
										if (Node && Index != INDEX_NONE && Node->GetKind() == EHjsonNodeKind::Array
											&& Node->GetItems().IsValidIndex(Index)) Node = &Node->GetItems()[Index].Get();
										if (Node) OutError.Locate(Node->GetProvenance());
									}
									return false;
								};
								TArray<const FUeAgentCanonicalValue*> Values;
								if (Spec.Shape == EKeyedItemReferenceShape::Set) {
									if (Input.GetKind() != EUeAgentValueKind::Array) return Reject(TEXT("a reference set must be an array."));
									for (const auto& V : Input.GetArrayItems()) Values.Add(&V.Get());
								} else Values.Add(&Input);
								for (int32 Index = 0; Index < Values.Num(); ++Index) {
									const auto* V = Values[Index];
									if (Spec.Shape == EKeyedItemReferenceShape::Single && (V->GetKind() == EUeAgentValueKind::Null || (V->GetKind() == EUeAgentValueKind::String && V->GetText().IsEmpty()))) continue;
									if (V->GetKind() != EUeAgentValueKind::String || V->GetText().IsEmpty())
										return Reject(V->GetKind() != EUeAgentValueKind::String ? TEXT("reference must be a string.") : TEXT("reference cannot be empty."), Index);
									FString Key = V->GetText();
									if (bIdentity) if (const auto* K = Target.KeyByIdentity.Find(Key)) Key = *K;
									if (Keys.Contains(Key)) return Reject(TEXT("reference is duplicated."), Index, &Key);
									Keys.Add(Key);
								}
								return true;
							};
							TArray<FString> Keys, BeforeKeys;
							if (!ReadKeys(*Value, bCaptured, Keys)) return false;
							if (Original && !ReadKeys(*Original, true, BeforeKeys))
								return false;
							TArray<FUeAgentCanonicalValueRef> Values;
							for (const FString& Key : Keys)
							{
								const auto* Identity = Target.IdentityByKey.Find(Key);
								// Captured relations are context, including native unresolved identities.
								// The native owner decides their fate after deletion. In a set, adding
								// another member does not turn the retained members into explicit edits.
								const bool bCapturedMember = BeforeKeys.Contains(Key);
								if (!Identity && bCapturedMember) Identity = Target.UnresolvedIdentityByKey.Find(Key);
								if (!Target.Desired.Contains(Key) && !bCapturedMember)
									return Fail(OutError, TEXT("keyed_reference_target_missing"), FString::Printf(TEXT("Reference field '%s' on item '%s' points to an absent desired item."), *Field.Key, *Item.Key), SourceUnit, Field.Key, MakeShared<FJsonValueString>(Key));
								FUeAgentCanonicalValuePtr Reference;
								if (!TryString(bProject || !Identity ? Key : *Identity, Resources, Reference, OutError)) return false;
								if (!bProject && !Identity && !bCapturedMember) {
									FUeAgentCanonicalValuePtr NewReference;
								if (!FUeAgentCanonicalValue::TryObject({{TEXT("new_item"), Reference.ToSharedRef()}}, Resources, NewReference, Error)) { OutError.SetFailure(Error, SourceUnit); return false; }
									Reference = NewReference;
								}
								Values.Add(Reference.ToSharedRef());
							}
							FUeAgentCanonicalValuePtr Reference;
							if (Spec.Shape == EKeyedItemReferenceShape::Set) {
								if (!FUeAgentCanonicalValue::TryArray(Values, Resources, Reference, Error)) { OutError.SetFailure(Error, SourceUnit); return false; }
							} else if (!Values.IsEmpty()) Reference = Values[0];
							else Reference = MakeShared<FUeAgentCanonicalValue>(*Value);
							Fields.Emplace(Field.Key, Reference.ToSharedRef());
							return true;
						};
						if (!ResolveReference(FieldError))
						{
							LocateKeyedItemError(FieldError, VisibleSource, Item.Key, Field.Key);
							ItemErrors.Append(FieldError);
						}
					}
					FUeAgentCanonicalValuePtr Row;
					if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Row, Error)) { OutError.SetFailure(Error, SourceUnit); return false; }
					Rows.Emplace(Item.Key, Row.ToSharedRef());
					return true;
				};
				if (!ResolveItem(ItemError))
				{
					LocateKeyedItemError(ItemError, VisibleSource, Item.Key);
					ItemErrors.Append(ItemError);
				}
			}
			if (ItemErrors.HasErrors()) { OutError = MoveTemp(ItemErrors); return false; }
			if (!FUeAgentCanonicalValue::TryObject(Rows, Resources, OutItems, Error)) { OutError.SetFailure(Error, SourceUnit); return false; }
			return true;
		}
	}
	bool TryGetNewKeyedItemReference(const FJsonValue& Value, FString& OutKey)
	{
		OutKey.Reset();
		if (Value.Type != EJson::Object) return false;
		const auto Object = Value.AsObject();
		return Object.IsValid() && Object->Values.Num() == 1
			&& Object->TryGetStringField(TEXT("new_item"), OutKey) && !OutKey.IsEmpty();
	}
	bool TryGetNewKeyedItemReference(const FUeAgentCanonicalValue& Value, FString& OutKey)
	{
		OutKey.Reset();
		const auto* Key = OwnedDefinitionField(Value, TEXT("new_item"));
		if (!Key || Value.GetObjectItems().Num() != 1 || Key->GetKind() != EUeAgentValueKind::String || Key->GetText().IsEmpty()) return false;
		OutKey = Key->GetText();
		return true;
	}

	bool TryProjectKeyedItemVisibleValue(
		const FString& SourceUnit, const FUeAgentCanonicalValue& CapturedItems,
		const FKeyedItemProjectionContract& Contract, FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutVisibleItems, FWorkspaceError& OutError,
		const EKeyedItemProjectionInputKind InputKind, const FKeyedItemReferenceContext* ReferenceContext)
	{
		OutVisibleItems.Reset();
		FExactItems Captured;
		if (!Contract.VisibleFieldPolicy.IsValid())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("keyed_item_visible_field_policy_invalid"),
				TEXT("Keyed-item projection contract requires at least one visible field."), TEXT("content_projection"), FString(), SourceUnit);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Contract.RootField;
			Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Contract.VisibleFieldPolicy.VisibleTopLevelFields.Num());
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one visible field"));
			OutError.SetFailure(Diagnostic);
			return false;
		}
		if (InputKind == EKeyedItemProjectionInputKind::ProjectedMap)
		{
			if (CapturedItems.GetKind() != EUeAgentValueKind::Object) return Fail(OutError, TEXT("keyed_item_shape_invalid"), TEXT("Captured keyed items must be an object."), SourceUnit);
			for (const auto& Item : CapturedItems.GetObjectItems()) Captured.Add(Item.Key, Item.Value);
		}
		else if (!TryCapturedItems(CapturedItems, Contract, SourceUnit, Captured, OutError)) return false;
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
		FUeAgentStateSemanticError Error;
		for (const auto& Item : Captured)
		{
			if (Item.Value->GetKind() != EUeAgentValueKind::Object) return Fail(OutError, TEXT("keyed_item_shape_invalid"), FString::Printf(TEXT("Captured item '%s' must be an object."), *Item.Key), SourceUnit);
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
			for (const auto& Field : Item.Value->GetObjectItems())
			{
				if (!Contract.VisibleFieldPolicy.Allows(Field.Key)) continue;
				if (Contract.ImplicitCapturedFields.Contains(Field.Key)) continue;
				if (const auto* ChildContract = Contract.CollectionFieldContracts.Find(Field.Key))
				{
					FUeAgentCanonicalValuePtr Projected;
					if (!TryProjectKeyedItemVisibleValue(SourceUnit + TEXT("/") + Item.Key + TEXT("/") + Field.Key,
						Field.Value.Get(), **ChildContract, Resources, Projected, OutError, InputKind, ReferenceContext)) return false;
					Fields.Emplace(Field.Key, Projected.ToSharedRef());
				}
				else if (Contract.ObjectFieldPolicies.Contains(Field.Key))
				{
					FUeAgentCanonicalValuePtr Projected;
					if (!TryProjectRecordObject(Field.Value.Get(), Field.Key, Contract, Resources, Projected, OutError)) return false;
					Fields.Emplace(Field.Key, Projected.ToSharedRef());
				}
				else Fields.Add(Field);
			}
			FUeAgentCanonicalValuePtr Visible;
			if (!FUeAgentCanonicalValue::TryObject(Fields, Resources, Visible, Error))
			{
				OutError.SetFailure(Error, SourceUnit); return false;
			}
			Items.Emplace(Item.Key, Visible.ToSharedRef());
		}
		if (!FUeAgentCanonicalValue::TryObject(Items, Resources, OutVisibleItems, Error))
		{
			OutError.SetFailure(Error, SourceUnit); return false;
		}
		if (InputKind == EKeyedItemProjectionInputKind::CapturedItems && !Contract.ReferenceFields.IsEmpty())
		{
			FUeAgentCanonicalValuePtr Referenced;
			if (!TryTransformKeyedReferences(*OutVisibleItems, CapturedItems, Contract, true,
				SourceUnit, Resources, Referenced, OutError, ReferenceContext)) return false;
			OutVisibleItems = Referenced;
		}
		return true;
	}

	namespace
	{
		bool TryBuildKeyedItemOperationRecords(const FUeAgentCanonicalValue& DesiredItems,
			const FUeAgentCanonicalValue& CapturedItems, const FKeyedItemProjectionContract& Contract,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutRecords, FWorkspaceError& OutError)
		{
			const auto Desired = ObjectMap(DesiredItems);
			const auto Captured = ObjectMap(CapturedItems);
			TMap<FString, FUeAgentCanonicalValueRef> Records;
			auto Add = [&](const FString& Key, const FUeAgentCanonicalValue& Value, const TCHAR* Operation) {
				auto Fields = ObjectMap(Value);
				if (FCString::Strcmp(Operation, TEXT("update")) == 0)
				{
					const auto& Before = Captured.FindChecked(Key);
					FUeAgentSemanticComparison Comparison;
					FUeAgentStateSemanticError Error;
					if (!TryCompareValues(Before, MakeShared<FUeAgentCanonicalValue>(Value),
						TEXT("/") + Key, Resources, Comparison, Error))
					{ OutError.SetFailure(Error, FString(), Error.Source.JsonPath); return false; }
					// Reconstructing a complete set and mutating individual entries
					// have different context requirements, independent of key identity.
					if (Comparison.IsEquivalent() && !Contract.bRetainUnchangedOperationRecords) return true;
					for (const FString& Field : Contract.DeltaOnlyFields)
					{
						const auto* Previous = OwnedDefinitionField(Before.Get(), Field);
						const auto* After = Fields.Find(Field);
						if (!Previous || !After) continue;
						if (!TryCompareValues(MakeShared<FUeAgentCanonicalValue>(*Previous), *After,
							TEXT("/") + Key + TEXT("/") + Field, Resources, Comparison, Error))
						{ OutError.SetFailure(Error, FString(), Error.Source.JsonPath); return false; }
						if (Comparison.IsEquivalent()) Fields.Remove(Field);
					}
				}
				FUeAgentCanonicalValuePtr Op, Record;
				if (!TryString(Operation, Resources, Op, OutError)) return false;
				Fields.Add(TEXT("operation"), Op.ToSharedRef());
				if (!TryObject(Fields, Resources, Record, OutError)) return false;
				Records.Add(Key, Record.ToSharedRef());
				return true;
			};
			for (const auto& Item : Captured)
			{
				const auto* After = Desired.Find(Item.Key);
				if (!Add(Item.Key, After ? After->Get() : Item.Value.Get(), After ? TEXT("update") : TEXT("remove"))) return false;
			}
			for (const auto& Item : Desired)
				if (!Captured.Contains(Item.Key) && !Add(Item.Key, Item.Value.Get(), TEXT("create"))) return false;
			return TryObject(Records, Resources, OutRecords, OutError);
		}
	}

	bool TryResolveKeyedItemDesiredState(
		const FUeAgentCanonicalValue& VisibleItems,
		const FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		const FKeyedItemProjectionContract& Contract,
		const FUeAgentCanonicalValue* RecordSchema,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError,
		const FUeAgentCanonicalValue* NestedRecordDefaults,
		const EKeyedItemInputKind InputKind, const FKeyedItemReferenceContext* ReferenceContext,
		const FHjsonNode* VisibleSource)
	{
		OutResolvedItems.Reset();
		bOutSemanticChanged = false;
		OutError.Reset();
		if (!Contract.ReferenceFields.IsEmpty())
		{
			FUeAgentCanonicalValuePtr ResolvedReferences;
			FWorkspaceError ReferenceErrors;
			const bool bReferencesValid = TryTransformKeyedReferences(VisibleItems, CapturedItems, Contract, false,
				SourceUnit, Resources, ResolvedReferences, ReferenceErrors, ReferenceContext,
				InputKind == EKeyedItemInputKind::ResolvedOperationRecords, VisibleSource);
			FKeyedItemProjectionContract ValueContract = Contract;
			ValueContract.ReferenceFields.Reset();
			FWorkspaceError ItemErrors;
			const bool bItemsValid = TryResolveKeyedItemDesiredState(
				bReferencesValid ? *ResolvedReferences : VisibleItems, CapturedItems, SourceUnit,
				ValueContract, RecordSchema, Resources, OutResolvedItems, bOutSemanticChanged,
				ItemErrors, NestedRecordDefaults, InputKind, ReferenceContext, VisibleSource);
			if (!bReferencesValid || !bItemsValid)
			{
				OutResolvedItems.Reset();
				OutError = MoveTemp(ReferenceErrors);
				OutError.Append(ItemErrors);
				return false;
			}
			return true;
		}
		if (VisibleItems.GetKind() != EUeAgentValueKind::Object
			|| (Contract.CapturedRecordIdentityField.IsEmpty()
				? CapturedItems.GetKind() != EUeAgentValueKind::Object
				: CapturedItems.GetKind() != EUeAgentValueKind::Array))
		{
			return FailAt(OutError, TEXT("keyed_item_shape_invalid"), VisibleItems.GetKind() != EUeAgentValueKind::Object
				? TEXT("Visible keyed items must be an object.")
				: Contract.CapturedRecordIdentityField.IsEmpty() ? TEXT("Captured keyed items must be an object.")
				: TEXT("Captured keyed records must be an array."), SourceUnit, TEXT("/"), nullptr, nullptr,
				VisibleItems.GetKind() == EUeAgentValueKind::Object);
		}
		if (Contract.bIdentityFromMapKey)
		{
			const bool bTypedRecord = !Contract.RecordSchemaField.IsEmpty();
			if (bTypedRecord && (!RecordSchema || RecordSchema->GetKind() != EUeAgentValueKind::Object))
				return FailAt(OutError, TEXT("keyed_record_schema_missing"), FString::Printf(TEXT("Collection '%s' requires a captured object record schema in '%s'."), *Contract.RootField, *Contract.RecordSchemaField), SourceUnit, TEXT("/"), nullptr, MakeShared<FJsonValueString>(TEXT("captured object record schema")), true);
			const auto Schema = RecordSchema ? ObjectMap(*RecordSchema) : TMap<FString, FUeAgentCanonicalValueRef>();
			const FUeAgentCanonicalValueRef* DefaultFields = Schema.Find(TEXT("properties"));
			if (bTypedRecord && (!DefaultFields || (*DefaultFields)->GetKind() != EUeAgentValueKind::Object))
				return FailAt(OutError, TEXT("keyed_record_schema_invalid"), TEXT("The captured record schema requires a properties object containing field defaults."), SourceUnit, TEXT("/properties"), nullptr, MakeShared<FJsonValueString>(TEXT("captured properties object")), true);
			const auto Defaults = DefaultFields ? ObjectMap(DefaultFields->Get()) : TMap<FString, FUeAgentCanonicalValueRef>();
			FExactItems Captured;
			if (!TryCapturedItems(CapturedItems, Contract, SourceUnit, Captured, OutError)) return false;
			TMap<FString, FUeAgentCanonicalValueRef> CapturedNameIdentities;
			if (Contract.bMapKeyUsesNameIdentity)
				for (const auto& Item : Captured) CapturedNameIdentities.Add(Item.Key, Item.Value);
			auto FindCaptured = [&](const FString& Key) -> const FUeAgentCanonicalValueRef*
			{
				return Contract.bMapKeyUsesNameIdentity ? CapturedNameIdentities.Find(Key) : Captured.Find(Key);
			};
			TSet<FString, FExactStringKeyFuncs> Names;
			FExactItems Desired;
			FWorkspaceError ItemErrors;
			for (const auto& Item : VisibleItems.GetObjectItems())
			{
				FWorkspaceError ItemError;
				auto ResolveItem = [&](FWorkspaceError& OutError) -> bool {
					const FString Name = Contract.bMapKeyUsesNameIdentity ? Item.Key.TrimStartAndEnd() : Item.Key;
					const FString Identity = Contract.bMapKeyUsesNameIdentity ? Name.ToLower() : Name;
					if (Name.IsEmpty() || (Contract.bMapKeyUsesNameIdentity && Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
						|| Names.Contains(Identity) || Name != Item.Key
						|| Item.Value->GetKind() != EUeAgentValueKind::Object)
						return FailAt(OutError, TEXT("keyed_record_name_invalid"), FString::Printf(TEXT("Record '%s': %s"), *Item.Key, Name.IsEmpty() ? TEXT("name is empty.") : (Contract.bMapKeyUsesNameIdentity && Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)) ? TEXT("None is a reserved name.") : Names.Contains(Identity) ? TEXT("name duplicates another record.") : Name != Item.Key ? TEXT("name contains leading or trailing whitespace.") : TEXT("record value must be an object.")), SourceUnit, TEXT("/"), MakeShared<FJsonValueString>(Item.Key));
					Names.Add(Identity);
					const auto VisibleRecord = ObjectMap(Item.Value.Get());
					const FUeAgentCanonicalValueRef* Operation = VisibleRecord.Find(TEXT("operation"));
					const FUeAgentCanonicalValueRef* Existing = FindCaptured(Item.Key);
					if (Operation)
					{
						if (InputKind != EKeyedItemInputKind::ResolvedOperationRecords)
							return FailAt(OutError, TEXT("keyed_item_machine_field_forbidden"), FString::Printf(TEXT("Record '%s' cannot specify the internal operation field in visible desired state."), *Item.Key), SourceUnit, TEXT("/operation"));
						if ((*Operation)->GetKind() != EUeAgentValueKind::String)
							return FailAt(OutError, TEXT("keyed_item_operation_invalid"), FString::Printf(TEXT("Operation for record '%s' must be a string."), *Item.Key), SourceUnit, TEXT("/operation"));
						const FString& Kind = (*Operation)->GetText();
						if (Kind == TEXT("remove"))
						{
							if (!Existing) return FailAt(OutError, TEXT("keyed_item_remove_unknown"), FString::Printf(TEXT("Record '%s' cannot be removed because it is absent from captured state."), *Item.Key), SourceUnit, TEXT("/operation"), MakeShared<FJsonValueString>(Kind));
							return true;
						}
						if (Kind == TEXT("create"))
						{
							if (Existing || !Contract.bAllowNewItems)
								return FailAt(OutError, TEXT("keyed_item_create_unsupported"), Existing
									? FString::Printf(TEXT("Record '%s' already exists and cannot be created again."), *Item.Key)
									: FString::Printf(TEXT("Collection '%s' does not allow creating record '%s'."), *Contract.RootField, *Item.Key), SourceUnit, TEXT("/operation"), MakeShared<FJsonValueString>(Kind));
						}
						else if (Kind != TEXT("update") || !Existing)
							return FailAt(OutError, TEXT("keyed_item_operation_invalid"), Kind != TEXT("update") ? TEXT("Operation must be create, update or remove.") : FString::Printf(TEXT("Record '%s' cannot be updated because it is absent from captured state."), *Item.Key), SourceUnit, TEXT("/operation"), MakeShared<FJsonValueString>(Kind));
					}
					for (const auto& Field : Item.Value->GetObjectItems())
						if (Field.Key != TEXT("operation") && !Contract.VisibleFieldPolicy.Allows(Field.Key))
							return FailAt(OutError, TEXT("keyed_record_field_unknown"), FString::Printf(TEXT("Record field '%s' is not allowed by this collection contract."), *Field.Key), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key));
					if (!bTypedRecord)
					{
						TMap<FString, FUeAgentCanonicalValueRef> Fields = Schema;
						const FUeAgentCanonicalValueRef* Before = FindCaptured(Item.Key);
						if (!Before && !Contract.bAllowNewItems)
							return FailAt(OutError, TEXT("keyed_item_create_unsupported"), FString::Printf(TEXT("Collection '%s' does not allow creating record '%s'."), *Contract.RootField, *Item.Key), SourceUnit, TEXT("/"), MakeShared<FJsonValueString>(Item.Key));
						if (Before)
						{
							if ((*Before)->GetKind() != EUeAgentValueKind::Object)
								return FailAt(OutError, TEXT("keyed_record_capture_invalid"), FString::Printf(TEXT("Captured record '%s' must be an object."), *Item.Key), SourceUnit, TEXT("/"), nullptr, MakeShared<FJsonValueString>(TEXT("captured object record")), true);
							Fields = ObjectMap(Before->Get());
						}
						for (const auto& Field : VisibleRecord)
						{
							if (Field.Key == TEXT("operation")) continue;
							if (const auto* ChildContract = Contract.CollectionFieldContracts.Find(Field.Key))
							{
								const auto* Original = Fields.Find(Field.Key);
								FUeAgentCanonicalValuePtr EmptyChild;
								if (!Original && !Before)
								{
									FUeAgentStateSemanticError EmptyError;
									const bool bBuilt = (*ChildContract)->CapturedRecordIdentityField.IsEmpty()
										? FUeAgentCanonicalValue::TryObject({}, Resources, EmptyChild, EmptyError)
										: FUeAgentCanonicalValue::TryArray({}, Resources, EmptyChild, EmptyError);
									if (!bBuilt) { OutError.SetFailure(MoveTemp(EmptyError), SourceUnit); return false; }
									Fields.Add(Field.Key, EmptyChild.ToSharedRef());
									Original = Fields.Find(Field.Key);
								}
								if (!Original) return FailAt(OutError, TEXT("keyed_record_capture_invalid"), FString::Printf(TEXT("Captured record '%s' is missing nested collection '%s'."), *Item.Key, *Field.Key), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key), nullptr, MakeShared<FJsonValueString>(TEXT("captured nested collection")), true);
								const auto* ChildDefaults = NestedRecordDefaults ? OwnedDefinitionField(*NestedRecordDefaults, Field.Key) : nullptr;
								const auto* ChildRecordDefaults = ChildDefaults
									? OwnedDefinitionField(*ChildDefaults, TEXT("record_defaults")) : nullptr;
								const auto* ChildNestedDefaults = ChildDefaults
									? OwnedDefinitionField(*ChildDefaults, TEXT("nested_record_defaults")) : nullptr;
								if (!ChildRecordDefaults && !ChildNestedDefaults)
								{
									ChildRecordDefaults = ChildDefaults;
									ChildNestedDefaults = ChildDefaults;
								}
								FUeAgentCanonicalValuePtr DerivedChildDefaults;
								if (const auto* Source = Contract.ChildRecordDefaultObjectSources.Find(Field.Key))
								{
									const auto Parent = Before ? ObjectMap(Before->Get()) : Schema;
									const auto* NativeDefaults = Parent.Find(*Source);
									if (!NativeDefaults || (*NativeDefaults)->GetKind() != EUeAgentValueKind::Object)
										return FailAt(OutError, TEXT("keyed_child_default_source_missing"), FString::Printf(TEXT("Nested collection '%s' requires a captured object default source '%s'."), *Field.Key, **Source), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key), nullptr, MakeShared<FJsonValueString>(*Source), true);
									auto ChildFields = ChildRecordDefaults ? ObjectMap(*ChildRecordDefaults) : TMap<FString, FUeAgentCanonicalValueRef>();
									for (const auto& NativeField : (*NativeDefaults)->GetObjectItems()) ChildFields.Add(NativeField.Key, NativeField.Value);
									if (!TryObject(ChildFields, Resources, DerivedChildDefaults, OutError)) return false;
									ChildRecordDefaults = DerivedChildDefaults.Get();
								}

								if (const auto* Sources = Contract.ChildRecordDefaultSources.Find(Field.Key))
								{
									const auto Parent = Before ? ObjectMap(Before->Get()) : Schema;
									auto ChildFields = ChildRecordDefaults ? ObjectMap(*ChildRecordDefaults) : TMap<FString, FUeAgentCanonicalValueRef>();
									for (const auto& Source : *Sources)
									{
										const auto* ParentDefault = Parent.Find(Source.Value);
										if (!ParentDefault) return FailAt(OutError, TEXT("keyed_child_default_source_missing"), FString::Printf(TEXT("Nested collection '%s' is missing captured parent default '%s'."), *Field.Key, *Source.Value), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key), nullptr, MakeShared<FJsonValueString>(Source.Value), true);
										ChildFields.Add(Source.Key, *ParentDefault);
									}
									if (!TryObject(ChildFields, Resources, DerivedChildDefaults, OutError)) return false;
									ChildRecordDefaults = DerivedChildDefaults.Get();
								}
								FUeAgentCanonicalValuePtr Resolved;
								bool bChanged = false;
								if (!TryResolveKeyedItemDesiredState(Field.Value.Get(), Original->Get(),
									SourceUnit, **ChildContract,
									ChildRecordDefaults, Resources, Resolved, bChanged, OutError, ChildNestedDefaults, EKeyedItemInputKind::VisibleDesiredState, nullptr,
									VisibleSource && VisibleSource->Get(Item.Key) ? VisibleSource->Get(Item.Key)->Get(Field.Key) : nullptr))
								{
									OutError.PrependPath(TEXT("/") + EscapeJsonPointerSegment(Field.Key), true);
									return false;
								}
								Fields.Add(Field.Key, Resolved.ToSharedRef());
								continue;
							}
							const auto* Policy = Contract.ObjectFieldPolicies.Find(Field.Key);
							if (!Policy) { Fields.Add(Field.Key, Field.Value); continue; }
							if (Field.Value->GetKind() != EUeAgentValueKind::Object)
								return FailAt(OutError, TEXT("keyed_record_object_required"), FString::Printf(TEXT("Field '%s' must be an object."), *Field.Key), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key));
							const auto* Original = Fields.Find(Field.Key);
							FUeAgentCanonicalValuePtr Merged;
							const auto* Default = Schema.Find(Field.Key);
							if (!TryMergeRecordObject(Field.Value.Get(), Original ? &Original->Get() : nullptr,
						Default ? &Default->Get() : nullptr, Field.Key, Contract, Resources, Merged, OutError,
						TEXT("/") + EscapeJsonPointerSegment(Field.Key))) return false;
							Fields.Add(Field.Key, Merged.ToSharedRef());
						}
						if (!Before && !Contract.CapturedRecordIdentityField.IsEmpty())
						{
							FUeAgentCanonicalValuePtr IdentityValue;
							if (!TryString(Item.Key, Resources, IdentityValue, OutError)) return false;
							if (Contract.bUseNewItemReferenceForIdentity)
							{
								FUeAgentStateSemanticError IdentityError;
								FUeAgentCanonicalValuePtr NewIdentity;
								if (!FUeAgentCanonicalValue::TryObject({{TEXT("new_item"), IdentityValue.ToSharedRef()}}, Resources, NewIdentity, IdentityError))
								{ OutError.SetFailure(MoveTemp(IdentityError), SourceUnit); return false; }
								IdentityValue = NewIdentity;
							}
							Fields.Add(Contract.CapturedRecordIdentityField, IdentityValue.ToSharedRef());
						}
						FUeAgentCanonicalValuePtr Record;
						if (!TryObject(Fields, Resources, Record, OutError)) return false;
						Desired.Add(Item.Key, Record.ToSharedRef());
						return true;
					}
					auto Fields = Defaults;
					if (const FUeAgentCanonicalValueRef* Before = FindCaptured(Item.Key))
					{
						const auto BeforeRecord = ObjectMap(Before->Get());
						const FUeAgentCanonicalValueRef* Properties = BeforeRecord.Find(TEXT("properties"));
						if (!Properties) return FailAt(OutError, TEXT("keyed_record_capture_invalid"), FString::Printf(TEXT("Captured record '%s' is missing its properties object."), *Item.Key), SourceUnit, TEXT("/properties"), nullptr, MakeShared<FJsonValueString>(TEXT("captured properties object")), true);
						Fields = ObjectMap(Properties->Get());
					}
					if (const FUeAgentCanonicalValueRef* Properties = VisibleRecord.Find(TEXT("properties")))
					{
						if ((*Properties)->GetKind() != EUeAgentValueKind::Object)
							return FailAt(OutError, TEXT("keyed_record_properties_invalid"), FString::Printf(TEXT("Properties of record '%s' must be an object."), *Item.Key), SourceUnit, TEXT("/properties"));
						for (const auto& Property : (*Properties)->GetObjectItems())
						{
							const FUeAgentCanonicalValueRef* Definition = Defaults.Find(Property.Key);
							if (!Definition || Property.Value->GetKind() != EUeAgentValueKind::Object)
								return FailAt(OutError, TEXT("keyed_record_property_unknown"), !Definition ? FString::Printf(TEXT("Property '%s' is absent from the captured record schema."), *Property.Key) : FString::Printf(TEXT("Property descriptor '%s' must be an object."), *Property.Key), SourceUnit, TEXT("/properties/") + EscapeJsonPointerSegment(Property.Key));
							const auto Descriptor = ObjectMap(Property.Value.Get());
							const auto DefaultDescriptor = ObjectMap(Definition->Get());
							const FUeAgentCanonicalValueRef* Type = Descriptor.Find(TEXT("type"));
							const FUeAgentCanonicalValueRef* ExpectedType = DefaultDescriptor.Find(TEXT("type"));
							if (Descriptor.Num() != 2 || !Descriptor.Contains(TEXT("value")) || !Type || !ExpectedType
								|| (*Type)->GetKind() != EUeAgentValueKind::String
								|| (*Type)->GetText() != (*ExpectedType)->GetText())
								return FailAt(OutError, TEXT("semantic_workspace_property_type_mismatch"),
									FString::Printf(TEXT("Property '%s': %s"), *Property.Key,
										!Type ? TEXT("type is missing.") : !ExpectedType ? TEXT("captured schema type is missing.")
										: (*Type)->GetKind() != EUeAgentValueKind::String ? TEXT("type must be a string.")
										: (*Type)->GetText() != (*ExpectedType)->GetText() ? TEXT("type differs from the captured schema.")
										: TEXT("descriptor must contain exactly type and value.")), SourceUnit, TEXT("/properties/") + EscapeJsonPointerSegment(Property.Key)
										+ ((!Type || (Type && ExpectedType && ((*Type)->GetKind() != EUeAgentValueKind::String || (*Type)->GetText() != (*ExpectedType)->GetText()))) ? TEXT("/type") : TEXT("")),
									Type && (*Type)->GetKind() == EUeAgentValueKind::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>((*Type)->GetText())) : nullptr,
									ExpectedType ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>((*ExpectedType)->GetText())) : nullptr,
									Type && !ExpectedType);
							Fields.Add(Property.Key, Property.Value);
						}
					}
					FUeAgentCanonicalValuePtr Properties;
					FUeAgentCanonicalValuePtr RowName;
					FUeAgentCanonicalValuePtr Record;
					if (!TryObject(Fields, Resources, Properties, OutError)
						|| !TryString(Item.Key, Resources, RowName, OutError)
						|| !TryObject({{TEXT("name"), RowName.ToSharedRef()}, {TEXT("properties"), Properties.ToSharedRef()}},
							Resources, Record, OutError)) return false;
					Desired.Add(Item.Key, Record.ToSharedRef());
					return true;
				};
				if (!ResolveItem(ItemError))
				{
					ItemError.PrependPath(TEXT("/") + EscapeJsonPointerSegment(Item.Key), true);
					if (VisibleSource) ItemError.Locate(*VisibleSource);
					ItemErrors.Append(ItemError);
				}
			}
			if (ItemErrors.HasErrors()) { OutError = MoveTemp(ItemErrors); return false; }
			if (!Contract.bAllowRemoveItems)
				for (const auto& Item : Captured)
					if (!Desired.Contains(Item.Key))
						return FailAt(OutError, TEXT("keyed_item_remove_unsupported"), FString::Printf(TEXT("Collection '%s' does not allow removing captured item '%s'."), *Contract.RootField, *Item.Key), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Item.Key), nullptr, MakeShared<FJsonValueString>(TEXT("retain this captured item")));
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Ordered;
			for (const auto& Item : Desired) Ordered.Add(Item);
			Ordered.Sort([](const auto& A, const auto& B) { return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0; });
			FUeAgentStateSemanticError BuildError;
			if (Contract.CapturedRecordIdentityField.IsEmpty())
			{
				if (!FUeAgentCanonicalValue::TryObject(Ordered, Resources, OutResolvedItems, BuildError))
				{ OutError.SetFailure(MoveTemp(BuildError), SourceUnit); return false; }
			}
			else
			{
				TArray<FUeAgentCanonicalValueRef> Records;
				for (const auto& Item : Ordered) Records.Add(Item.Value);
				if (!FUeAgentCanonicalValue::TryArray(Records, Resources, OutResolvedItems, BuildError))
				{ OutError.SetFailure(MoveTemp(BuildError), SourceUnit); return false; }
			}
			FUeAgentSemanticComparison Comparison;
			FUeAgentStateSemanticError CompareError;
			if (!TryCompareValues(OutResolvedItems.ToSharedRef(),
				MakeShared<FUeAgentCanonicalValue>(CapturedItems), TEXT("/") + Contract.RootField, Resources, Comparison, CompareError))
			{
				OutError.SetFailure(MoveTemp(CompareError), SourceUnit);
				return false;
			}
			bOutSemanticChanged = !Comparison.IsEquivalent();
			if (Contract.bEmitOperationRecords)
			{
				FUeAgentCanonicalValuePtr Records;
				if (!TryBuildKeyedItemOperationRecords(*OutResolvedItems, CapturedItems, Contract, Resources, Records, OutError)) return false;
				OutResolvedItems = Records;
			}
			return true;
		}

		const auto Visible = ObjectMap(VisibleItems);
		const auto Captured = ObjectMap(CapturedItems);
		TMap<FString, FUeAgentCanonicalValueRef> Resolved;
		FWorkspaceError ItemErrors;
		for (const auto& CapturedItem : Captured)
		{
			FWorkspaceError ItemError;
			auto ResolveItem = [&](FWorkspaceError& OutError) -> bool {
				if (CapturedItem.Value->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, TEXT("keyed_item_shape_invalid"), FString::Printf(TEXT("Item '%s' must be an object."), *CapturedItem.Key), SourceUnit);
				}
				auto Fields = ObjectMap(CapturedItem.Value.Get());
				if (const FUeAgentCanonicalValueRef* VisibleItem = Visible.Find(CapturedItem.Key))
				{
					if ((*VisibleItem)->GetKind() != EUeAgentValueKind::Object)
					{
						return Fail(OutError, TEXT("keyed_item_shape_invalid"), FString::Printf(TEXT("Item '%s' must be an object."), *CapturedItem.Key), SourceUnit);
					}
					for (const auto& Field : (*VisibleItem)->GetObjectItems())
					{
						// Definition references are navigation metadata, never authoring edits.
						if (Field.Key == TEXT("definition") && !Contract.AggregatedItemSemanticSuffix.IsEmpty()) continue;
						if (IsMachineField(Field.Key)
							|| IsForbiddenVisibleField(Field.Key))
						{
							return Fail(OutError,
								TEXT("keyed_item_machine_field_forbidden"), FString::Printf(TEXT("Field '%s' of item '%s' is not allowed."), *Field.Key, *CapturedItem.Key), SourceUnit, Field.Key);
						}
						const auto* RecordSource = VisibleSource ? VisibleSource->Get(CapturedItem.Key) : nullptr;
						if (!ValidateVisibleValue(Field.Value.Get(), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key),
							OutError, RecordSource ? RecordSource->Get(Field.Key) : nullptr)) return false;
						const FUeAgentCanonicalValueRef* CapturedField = Fields.Find(Field.Key);
						if (CapturedField == nullptr)
						{
							bOutSemanticChanged = true;
						}
						else
						{
							FUeAgentSemanticComparison Comparison;
							FUeAgentStateSemanticError SemanticError;
							if (!TryCompareValues(
								*CapturedField, Field.Value,
								TEXT("/") + CapturedItem.Key + TEXT("/") + Field.Key,
								Resources, Comparison, SemanticError))
							{
								OutError.SetFailure(MoveTemp(SemanticError), CapturedItem.Key);
								return false;
							}
							bOutSemanticChanged |= !Comparison.IsEquivalent();
						}
						Fields.Add(Field.Key, Field.Value);
					}
				}
				else
				{
					bOutSemanticChanged = true;
					return true;
				}
				FUeAgentCanonicalValuePtr Item;
				if (!TryObject(Fields, Resources, Item, OutError))
				{
					return false;
				}
				Resolved.Add(CapturedItem.Key, Item.ToSharedRef());
				return true;
			};
			if (!ResolveItem(ItemError))
			{
				LocateKeyedItemError(ItemError, VisibleSource, CapturedItem.Key);
				ItemErrors.Append(ItemError);
			}
		}

		for (const auto& VisibleItem : Visible)
		{
			FWorkspaceError ItemError;
			auto ResolveItem = [&](FWorkspaceError& OutError) -> bool {
				if (Captured.Contains(VisibleItem.Key))
				{
					return true;
				}
				if (VisibleItem.Value->GetKind() != EUeAgentValueKind::Object)
				{
					return Fail(OutError, TEXT("keyed_item_shape_invalid"), FString::Printf(TEXT("New item '%s' must be an object."), *VisibleItem.Key), SourceUnit);
				}
				auto Fields = ObjectMap(VisibleItem.Value.Get());
				if (!Contract.AggregatedItemSemanticSuffix.IsEmpty()) Fields.Remove(TEXT("definition"));
				bOutSemanticChanged = true;
				for (const auto& Field : Fields)
				{
					if ((IsMachineField(Field.Key) && Field.Key != TEXT("id"))
						|| IsForbiddenVisibleField(Field.Key))
					{
						return Fail(OutError,
							TEXT("keyed_item_machine_field_forbidden"), FString::Printf(TEXT("Field '%s' of new item '%s' is not an allowed visible field."), *Field.Key, *VisibleItem.Key), SourceUnit, Field.Key);
					}
					const auto* RecordSource = VisibleSource ? VisibleSource->Get(VisibleItem.Key) : nullptr;
					if (!ValidateVisibleValue(Field.Value.Get(), SourceUnit, TEXT("/") + EscapeJsonPointerSegment(Field.Key),
						OutError, RecordSource ? RecordSource->Get(Field.Key) : nullptr)) return false;
				}
				bool bDirectCreation = false;
				if (!Fields.Contains(TEXT("id")) && !Fields.Contains(TEXT("name")))
					for (const auto& Field : Contract.DirectCreationFields)
						if (Fields.Contains(Field)) { bDirectCreation = true; break; }
				if (Contract.bRequiresCreationCandidate && !bDirectCreation)
				{
					const FUeAgentCanonicalValueRef* Selector = Fields.Find(TEXT("id"));
					FString SelectorField = TEXT("id");
					if (Selector == nullptr)
					{
						Selector = Fields.Find(TEXT("name"));
						SelectorField = TEXT("name");
					}
					if (Selector == nullptr
						|| (*Selector)->GetKind() != EUeAgentValueKind::String
						|| (*Selector)->GetText().TrimStartAndEnd().IsEmpty())
					{
						return Fail(OutError, TEXT("keyed_item_candidate_missing"), FString::Printf(TEXT("New item '%s' requires a nonempty string id or name selecting its creation candidate."), *VisibleItem.Key), SourceUnit, SelectorField);
					}
					TMap<FString, FUeAgentCanonicalValueRef> CandidateFields;
					CandidateFields.Add(SelectorField, *Selector);
					if (const FUeAgentCanonicalValueRef* Selection = Fields.Find(TEXT("select")))
					{
						CandidateFields.Add(TEXT("select"), *Selection);
					}
					FUeAgentCanonicalValuePtr Candidate;
					if (!TryObject(CandidateFields, Resources, Candidate, OutError))
					{
						return false;
					}
					Fields.Remove(SelectorField);
					Fields.Remove(TEXT("select"));
					Fields.Add(TEXT("candidate"), Candidate.ToSharedRef());
				}
				else
				{
					for (const auto& Field : Fields)
						if (!Contract.VisibleFieldPolicy.Allows(Field.Key))
							return Fail(OutError, TEXT("keyed_record_field_unknown"), FString::Printf(TEXT("Field '%s' is not allowed by this collection contract."), *Field.Key), SourceUnit, Field.Key);
				}
				FUeAgentCanonicalValuePtr LocalId;
				if (!TryString(VisibleItem.Key, Resources, LocalId, OutError))
				{
					return false;
				}
				Fields.Add(TEXT("id"), LocalId.ToSharedRef());
				FUeAgentCanonicalValuePtr Item;
				if (!TryObject(Fields, Resources, Item, OutError))
				{
					return false;
				}
				Resolved.Add(VisibleItem.Key, Item.ToSharedRef());
				return true;
			};
			if (!ResolveItem(ItemError))
			{
				LocateKeyedItemError(ItemError, VisibleSource, VisibleItem.Key);
				ItemErrors.Append(ItemError);
			}
		}
		if (ItemErrors.HasErrors()) { OutError = MoveTemp(ItemErrors); return false; }
		FUeAgentCanonicalValuePtr Desired;
		if (!TryObject(Resolved, Resources, Desired, OutError)) return false;
		return TryBuildKeyedItemOperationRecords(*Desired, CapturedItems, Contract, Resources, OutResolvedItems, OutError);
	}

	bool TryResolveKeyedItemDesiredState(
		const FUeAgentCanonicalValue& VisibleItems,
		const FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		const FKeyedItemProjectionContract& Contract,
		const FUeAgentCanonicalValue* RecordSchema,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError,
		const FUeAgentCanonicalValue* NestedRecordDefaults,
		const EKeyedItemInputKind InputKind,
		const FKeyedItemReferenceContext* ReferenceContext)
	{
		return TryResolveKeyedItemDesiredState(VisibleItems, CapturedItems, SourceUnit,
			Contract, RecordSchema, Resources, OutResolvedItems, bOutSemanticChanged,
			OutError, NestedRecordDefaults, InputKind, ReferenceContext, nullptr);
	}

	bool TryResolveKeyedItemDesiredState(
		const FUeAgentCanonicalValue& VisibleItems,
		const FUeAgentCanonicalValue& CapturedItems,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutResolvedItems,
		bool& bOutSemanticChanged,
		FWorkspaceError& OutError)
	{
		return TryResolveKeyedItemDesiredState(VisibleItems, CapturedItems,
			SourceUnit, FKeyedItemProjectionContract(), nullptr, Resources,
			OutResolvedItems, bOutSemanticChanged, OutError);
	}
}
