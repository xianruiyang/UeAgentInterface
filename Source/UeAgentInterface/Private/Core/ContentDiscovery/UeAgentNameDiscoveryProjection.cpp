#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::ContentDiscovery
{
	namespace
	{
		bool TryValidateSelectionSyntax(
			const FString& Name,
			const TMap<FString, FString>& Select,
			UE::UAI::Contract::FDiagnostic* OutError = nullptr,
			const FNameSelectionInputContext* InputContext = nullptr)
		{
			const FString NameField = InputContext ? InputContext->NameField : TEXT("name");
			const FString SelectField = InputContext ? InputContext->SelectField : TEXT("select");
			const FString SelectPointer = TEXT("/") + SelectField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			auto Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(NameField, Name);
			auto Selection = MakeShared<FJsonObject>();
			for (const TPair<FString, FString>& Field : Select)
			{
				if (!IsReadableDiscoveryName(Field.Key)
					|| !IsReadableDiscoveryName(Field.Value))
				{
					if (OutError)
					{
						*OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_selector_invalid"),
							!IsReadableDiscoveryName(Field.Key) ? TEXT("A candidate selection field has an empty or unreadable semantic name.") : TEXT("A candidate selection field has an empty or unreadable semantic value."), TEXT("candidate_selection"));
						OutError->PropertyPath = IsReadableDiscoveryName(Field.Key) ? SelectField + TEXT(".") + Field.Key : SelectField;
						if (InputContext) OutError->Source.JsonPath = IsReadableDiscoveryName(Field.Key)
							? SelectPointer + TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))
							: SelectPointer;
						OutError->ActualEvidence = MakeShared<FJsonValueString>(!IsReadableDiscoveryName(Field.Key) ? Field.Key : Field.Value);
						OutError->ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable semantic string"));
					}
					return false;
				}
				Selection->SetStringField(Field.Key, Field.Value);
			}
			if (!Select.IsEmpty())
			{
				Candidate->SetObjectField(SelectField, Selection);
			}
			HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Parsed;
			HjsonWorkspace::FWorkspaceError SyntaxError;
			if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
				Candidate, InputContext ? TEXT("") : TEXT("candidate"), Parsed, SyntaxError, NameField, SelectField))
			{
				if (OutError) *OutError = SyntaxError.GetDiagnostic();
				return false;
			}
			if (Parsed.Name != Name || !Parsed.Select.OrderIndependentCompareEqual(Select))
			{
				if (OutError) *OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_selector_invalid"), TEXT("The candidate selector does not preserve its semantic name and fields when parsed."), TEXT("candidate_selection"));
				return false;
			}
			return true;
		}

		bool TryBuildAmbiguity(
			const TArray<FNameSelectionEntry>& Entries,
			const TArray<int32>& NamedEntries,
			const TArray<int32>& Matches,
			const FString& Name,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Matches.Num() < 2)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_discriminator_missing"),
					TEXT("Building ambiguity alternatives requires at least two matching candidates."), TEXT("candidate_selection"));
				OutError.CandidateName = Name;
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("candidate_count");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least two"));
				return false;
			}

			auto IsReplayable = [&Entries, &NamedEntries, &Name](
				const int32 Match, const TMap<FString, FString>& Selection)
			{
				if (!TryValidateSelectionSyntax(Name, Selection)) return false;
				// Each alternative is a complete retry without the caller's partial
				// select, so it must distinguish this identity from the entire name group.
				for (const int32 Other : NamedEntries)
				{
					if (Entries[Other].Identity == Entries[Match].Identity) continue;
					bool bSame = true;
					for (const auto& Field : Selection)
					{
						const FString* Value = Entries[Other].Select.Find(Field.Key);
						if (!Value || *Value != Field.Value) { bSame = false; break; }
					}
					if (bSame) return false;
				}
				return true;
			};

			TArray<TMap<FString, FString>> Selections;
			for (const int32 Match : Matches)
			{
				TMap<FString, FString> BestSelection;
				int32 BestCost = MAX_int32;
				auto Consider = [&](const TMap<FString, FString>& Selection)
				{
					int32 Cost = 0;
					for (const auto& Field : Selection) Cost += Field.Key.Len() + Field.Value.Len();
					if (Cost < BestCost && IsReplayable(Match, Selection))
					{
						BestSelection = Selection;
						BestCost = Cost;
					}
				};
				// Equivalent native actions may have several searchable/selection representations.
				// Keep every representation until choosing a replayable public discriminator.
				for (int32 FieldCount = 1; FieldCount <= 2 && BestSelection.IsEmpty(); ++FieldCount)
				{
					for (const int32 Representation : NamedEntries)
					{
						if (Entries[Representation].Identity != Entries[Match].Identity) continue;
						const auto& Fields = Entries[Representation].Select;
						TArray<FString> Keys;
						Fields.GetKeys(Keys);
						Keys.Sort();
						for (int32 Left = 0; Left < Keys.Num(); ++Left)
						{
							if (FieldCount == 1) Consider({{Keys[Left], Fields.FindChecked(Keys[Left])}});
							else for (int32 Right = Left + 1; Right < Keys.Num(); ++Right)
								Consider({{Keys[Left], Fields.FindChecked(Keys[Left])},
									{Keys[Right], Fields.FindChecked(Keys[Right])}});
						}
					}
				}
				if (BestSelection.IsEmpty())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_discriminator_missing"), FString::Printf(TEXT("The catalog contains %d distinct candidates named '%s', but at least one cannot be uniquely selected with at most two public semantic fields."), Matches.Num(), *Name), TEXT("candidate_selection")); OutError.CandidateName = Name;
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("replayable_semantic_selector_available");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				Selections.Add(MoveTemp(BestSelection));
			}

			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_ambiguous"),
				FString::Printf(TEXT("More than one candidate matches '%s'. Choose one of the supplied alternatives."), *Name), TEXT("candidate_selection"));
			OutError.CandidateName = Name;
			FString CommonSingleKey;
			if (Selections[0].Num() == 1)
			{
				CommonSingleKey = Selections[0].CreateConstIterator().Key();
				for (const auto& Selection : Selections)
				{
					if (Selection.Num() != 1 || !Selection.Contains(CommonSingleKey))
					{
						CommonSingleKey.Reset();
						break;
					}
				}
			}
			if (!CommonSingleKey.IsEmpty())
			{
				auto Choose = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const auto& Selection : Selections)
				{
					Values.Add(MakeShared<FJsonValueString>(
						Selection.FindChecked(CommonSingleKey)));
				}
				Choose->SetArrayField(CommonSingleKey, MoveTemp(Values));
				OutError.CandidateChoices = MakeShared<FJsonValueObject>(Choose);
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Selection : Selections)
				{
					auto Choice = MakeShared<FJsonObject>();
					for (const auto& Field : Selection)
					{
						Choice->SetStringField(Field.Key, Field.Value);
					}
					Choices.Add(MakeShared<FJsonValueObject>(Choice));
				}
				OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
			}
			return true;
		}
	}

	namespace
	{
		template <typename EntryType>
		bool TryGroupDiscoveryEntries(const TArray<EntryType>& Entries,
			const TArray<int32>& OrderedIndices, TArray<FNameSelectionGroup>& OutGroups,
			UE::UAI::Contract::FDiagnostic& OutError,
			const TCHAR* InvalidIndexCode = TEXT("candidate_index_invalid"))
		{
			OutGroups.Reset();
			OutError.Reset();
			TMap<FString, int32> GroupByIdentity;
			for (const int32 Index : OrderedIndices)
			{
				if (!Entries.IsValidIndex(Index))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(InvalidIndexCode,
						TEXT("A candidate projection refers to an entry outside the native catalog."), TEXT("candidate_selection"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("candidate_index");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Index);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Entries.Num()));
					return false;
				}
				const auto& Entry = Entries[Index];
				if (Entry.Identity.IsEmpty())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_identity_missing"),
						TEXT("The candidate catalog entry has no native identity and cannot be resolved safely."), TEXT("candidate_selection"));
					OutError.CandidateName = Entry.Name;
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("native_identity_present");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				int32* GroupIndex = GroupByIdentity.Find(Entry.Identity);
				if (!GroupIndex)
				{
					const int32 NewIndex = OutGroups.AddDefaulted();
					GroupIndex = &GroupByIdentity.Add(Entry.Identity, NewIndex);
				}
				OutGroups[*GroupIndex].EntryIndices.Add(Index);
			}
			return true;
		}
	}

	bool TryGroupNameSelectionEntries(const TArray<FNameSelectionEntry>& Entries,
		const TArray<int32>& OrderedIndices, TArray<FNameSelectionGroup>& OutGroups,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TryGroupDiscoveryEntries(Entries, OrderedIndices, OutGroups, OutError);
	}

	bool TryResolveNameSelection(const TArray<FNameSelectionEntry>& Entries, const FString& Name,
		const TMap<FString, FString>& Select, int32& OutIndex, UE::UAI::Contract::FDiagnostic& OutError,
		const ENameComparison Comparison, const FNameSelectionInputContext* InputContext)
	{
		OutIndex = INDEX_NONE; OutError.Reset();
		const FString NameField = InputContext ? InputContext->NameField : TEXT("name");
		const FString SelectField = InputContext ? InputContext->SelectField : TEXT("select");
		const FString NamePointer = TEXT("/") + NameField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		const FString SelectPointer = TEXT("/") + SelectField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		auto AttachInputSource = [&](const FString& RelativePointer)
		{
			if (!InputContext || OutError.SourceKind == TEXT("native_result") || !OutError.Source.SourceFile.IsEmpty()) return;
			if (OutError.Source.JsonPath.IsEmpty()) OutError.Source.JsonPath = RelativePointer;
			UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, InputContext->JsonPath);
			TFunction<void(UE::UAI::Contract::FDiagnostic&)> MarkInput;
			MarkInput = [&](UE::UAI::Contract::FDiagnostic& Detail)
			{
				if (Detail.SourceKind == TEXT("native_result") || !Detail.Source.SourceFile.IsEmpty()) return;
				Detail.SourceKind = InputContext->SourceKind;
				if (Detail.PublicCause.IsValid())
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Detail.PublicCause);
					MarkInput(*Cause);
					Detail.PublicCause = MoveTemp(Cause);
				}
				for (auto& Child : Detail.Diagnostics) MarkInput(Child);
			};
			MarkInput(OutError);
		};
		UE::UAI::Contract::FDiagnostic SyntaxError;
		if (!Select.IsEmpty() && !TryValidateSelectionSyntax(Name, Select, &SyntaxError, InputContext))
		{
			OutError = !SyntaxError.IsEmpty() ? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("candidate_selector_invalid"), SyntaxError, TEXT("candidate_selection"))
				: UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_selector_invalid"), TEXT("Candidate select requires at most two readable semantic string fields."), TEXT("candidate_selection"));
			OutError.CandidateName = Name;
			AttachInputSource(SelectPointer);
			return false;
		}
		TArray<int32> NamedEntries;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const auto& Entry = Entries[Index];
			if (!Entry.Name.Equals(Name, Comparison == ENameComparison::IgnoreCase
				? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive)) continue;
			NamedEntries.Add(Index);
		}
		if (NamedEntries.IsEmpty()) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_name_not_found"), FString::Printf(TEXT("No candidate named '%s' exists in the current creation context."), *Name), TEXT("candidate_selection")); OutError.CandidateName = Name; AttachInputSource(NamePointer); return false; }
		TArray<FNameSelectionGroup> Groups;
		if (!TryGroupNameSelectionEntries(Entries, NamedEntries, Groups, OutError)) return false;
		TArray<int32> Representatives;
		TArray<int32> Matches;
		for (const FNameSelectionGroup& Group : Groups)
		{
			Representatives.Add(Group.EntryIndices[0]);
			for (const int32 Index : Group.EntryIndices)
			{
				const auto& Entry = Entries[Index];
				bool bMatches = true;
				for (const auto& Field : Select)
				{
					const auto* Value = Entry.Select.Find(Field.Key);
					if (!Value || *Value != Field.Value) { bMatches = false; break; }
				}
				if (bMatches) { Matches.Add(Index); break; }
			}
		}
		if (Matches.Num() == 1) { OutIndex = Matches[0]; return true; }
		if (Matches.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_selector_invalid"), FString::Printf(TEXT("The supplied selector does not match a candidate named '%s'."), *Name), TEXT("candidate_selection"));
			UE::UAI::Contract::FDiagnostic Alternatives;
			if (Representatives.Num() > 1)
			{
				if (TryBuildAmbiguity(Entries, NamedEntries, Representatives, Name, Alternatives)) OutError.CandidateChoices = Alternatives.CandidateChoices;
				else if (!Alternatives.IsEmpty()) OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Alternatives));
			}
			else
			{
				OutError.PublicMessage += FString::Printf(TEXT(" This name is unique; omit %s."), *SelectField);
				OutError.ExpectedValue = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
			}
			OutError.CandidateName = Name;
			auto ActualSelection = MakeShared<FJsonObject>();
			for (const auto& Field : Select) ActualSelection->SetStringField(Field.Key, Field.Value);
			OutError.PropertyPath = SelectField; OutError.ActualValue = MakeShared<FJsonValueObject>(ActualSelection);
			AttachInputSource(SelectPointer);
			return false;
		}
		if (!TryBuildAmbiguity(Entries, NamedEntries, Matches, Name, OutError))
		{
			AttachInputSource(Select.IsEmpty() ? NamePointer : SelectPointer);
			return false;
		}
		AttachInputSource(Select.IsEmpty() ? NamePointer : SelectPointer);
		return false;
	}

	namespace
	{
		bool IsReadableName(const FString& Value, FString* OutReason = nullptr)
		{
			const auto Reject = [OutReason](const TCHAR* Reason) { if (OutReason) *OutReason = Reason; return false; };
			const bool bDrivePath = Value.Len() >= 3
				&& FChar::IsAlpha(Value[0])
				&& Value[1] == TEXT(':')
				&& (Value[2] == TEXT('/') || Value[2] == TEXT('\\'));
			if (Value.IsEmpty()
				|| Value.StartsWith(TEXT("/"))
				|| Value.Contains(TEXT("\\"))
				|| Value.Contains(TEXT("://"))
				|| bDrivePath)
			{
				return Reject(Value.IsEmpty() ? TEXT("The discovery name is empty.") : TEXT("A public discovery name cannot be a filesystem path, object path or URI."));
			}
			for (const TCHAR Character : Value)
			{
				if (FChar::IsControl(Character) || Character == TCHAR(0xfffd))
				{
					return Reject(TEXT("The discovery name contains a control or replacement character."));
				}
			}
			FGuid Guid;
			// Auto-detect also accepts any 22-character Base64 name as a Short GUID.
			// Native display names (for example SlerpVectorToDirection) are not identity tokens.
			if (FGuid::ParseExact(Value, EGuidFormats::Digits, Guid)
				|| FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, Guid)
				|| FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphensInBraces, Guid)
				|| FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphensInParentheses, Guid))
			{
				return Reject(TEXT("A public discovery name cannot be a GUID identity token."));
			}
			const FString LowerValue = Value.ToLower();
			for (const TCHAR* Prefix : {TEXT("sha"), TEXT("md5"), TEXT("hash"), TEXT("digest"), TEXT("fingerprint")})
			{
				if (LowerValue.StartsWith(Prefix))
				{
					const int32 PrefixLength = FCString::Strlen(Prefix);
					if (LowerValue.Len() == PrefixLength || LowerValue[PrefixLength] == TEXT(':')
						|| LowerValue[PrefixLength] == TEXT('_') || LowerValue[PrefixLength] == TEXT('-')
						|| FChar::IsDigit(LowerValue[PrefixLength]))
					{
						return Reject(TEXT("A public discovery name cannot be a hash or fingerprint identity token."));
					}
				}
			}
			if (Value.Len() >= 32)
			{
				bool bHexLike = true;
				bool bHasDigit = false;
				int32 HexCharacterCount = 0;
				for (const TCHAR Character : LowerValue)
				{
					bHasDigit |= FChar::IsDigit(Character);
					if (FChar::IsHexDigit(Character)) ++HexCharacterCount;
					else if (Character != TEXT('-') && Character != TEXT('_') && Character != TEXT(':'))
					{
						bHexLike = false;
						break;
					}
				}
				if (bHexLike && bHasDigit && HexCharacterCount >= 24) return Reject(TEXT("A public discovery name cannot be a hexadecimal identity token."));
			}
			FString SegmentedValue = Value;
			for (const TCHAR* Separator : {
				TEXT("-"), TEXT("."), TEXT("["), TEXT("]"), TEXT(" ")})
			{
				SegmentedValue.ReplaceInline(Separator, TEXT("_"));
			}
			TArray<FString> Segments;
			SegmentedValue.ParseIntoArray(Segments, TEXT("_"), true);
			for (const FString& Segment : Segments)
			{
				if (Segment.Len() < 24)
				{
					continue;
				}
				bool bAlphaNumeric = true;
				bool bHasLetter = false;
				bool bHasSegmentDigit = false;
				bool bHasLowercase = false;
				for (const TCHAR Character : Segment)
				{
					if (!FChar::IsAlnum(Character))
					{
						bAlphaNumeric = false;
						break;
					}
					bHasLetter |= FChar::IsAlpha(Character);
					bHasSegmentDigit |= FChar::IsDigit(Character);
					bHasLowercase |= FChar::IsLower(Character);
				}
				if (bAlphaNumeric && bHasLetter && bHasSegmentDigit
					&& !bHasLowercase)
				{
					return Reject(TEXT("The discovery name contains an opaque machine identity segment."));
				}
			}
			return true;
		}

		FString ComparisonKey(
			const FString& Value,
			const ENameComparison Comparison)
		{
			return Comparison == ENameComparison::IgnoreCase
				? Value.ToLower()
				: Value;
		}

		bool IsReadableScope(const FString& Scope)
		{
			return IsReadableName(Scope);
		}

		UE::UAI::Contract::FDiagnostic UnreadableNameFailure(const TCHAR* Code, const FString& Value, const FString& Field)
		{
			FString Reason;
			IsReadableName(Value, &Reason);
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("name_discovery"));
			Error.SourceKind = TEXT("native_result"); Error.PropertyPath = Field;
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable public name without machine identity"));
			Error.ActualValuePresent = !Value.IsEmpty();
			Error.ActualEvidence = MakeShared<FJsonValueString>(Value);
			return Error;
		}

		bool TryPrepareScopedEntries(
			const TArray<FScopedNameDiscoveryEntry>& Entries,
			const ENameComparison Comparison,
			TArray<FScopedNameDiscoveryEntry>& OutCleanEntries,
			TMap<FString, TArray<int32>>& OutGroups,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutCleanEntries.Reset();
			OutGroups.Reset();
			OutCleanEntries.Reserve(Entries.Num());
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				FScopedNameDiscoveryEntry Clean = Entries[Index];
				Clean.Name.TrimStartAndEndInline();
				TArray<FString> ReadableScopes;
				for (FString Scope : Clean.Scopes)
				{
					Scope.TrimStartAndEndInline();
					if (!Scope.IsEmpty())
					{
						ReadableScopes.Add(MoveTemp(Scope));
					}
				}
				Clean.Scopes = MoveTemp(ReadableScopes);
				OutGroups.FindOrAdd(
					ComparisonKey(Clean.Name, Comparison)).Add(Index);
				OutCleanEntries.Add(MoveTemp(Clean));
			}
			return true;
		}

		void CollectScopedNameMatches(const TArray<FScopedNameDiscoveryEntry>& CleanEntries,
			const TMap<FString, TArray<int32>>& Groups, const FString& CleanPublicName,
			ENameComparison Comparison, TArray<int32>& Matches,
			UE::UAI::Contract::FDiagnostic& RelevantError)
		{
			const FString Wanted = ComparisonKey(CleanPublicName, Comparison);
			const ESearchCase::Type SearchCase = Comparison == ENameComparison::IgnoreCase
				? ESearchCase::IgnoreCase
				: ESearchCase::CaseSensitive;
			RelevantError.Reset();
			TArray<int32> RelevantIndices;
			auto AddGroup = [&Groups, &RelevantIndices](const FString& Key)
			{
				if (const TArray<int32>* Group = Groups.Find(Key))
				{
					RelevantIndices.Append(*Group);
				}
			};
			AddGroup(Wanted);
			// A literal name may itself contain brackets. Consider every possible
			// scope prefix. A previously emitted scope stays usable when the
			// candidate set changes, provided it still identifies exactly one entry.
			for (int32 Position = Wanted.Find(TEXT(" [")); Position != INDEX_NONE;
				Position = Wanted.Find(TEXT(" ["), ESearchCase::CaseSensitive,
					ESearchDir::FromStart, Position + 1))
			{
				AddGroup(Wanted.Left(Position));
			}
			RelevantIndices.Sort();
			Matches.Reset();
			for (const int32 Index : RelevantIndices)
			{
				const FScopedNameDiscoveryEntry& Entry = CleanEntries[Index];
				const bool bBareName = ComparisonKey(Entry.Name, Comparison) == Wanted;
				const bool bScopedName = CleanPublicName.StartsWith(
					Entry.Name + TEXT(" ["), SearchCase);
				if (!bBareName && !bScopedName)
				{
					continue;
				}
				if (!IsReadableName(Entry.Name))
				{
					RelevantError = UnreadableNameFailure(TEXT("name_discovery_entry_invalid"), Entry.Name, FString::Printf(TEXT("entries[%d].name"), Index));
					continue;
				}
				bool bMatches = bBareName;
				if (bScopedName)
				{
					FString Prefix;
					for (int32 ScopeIndex = 0; ScopeIndex < Entry.Scopes.Num(); ++ScopeIndex)
					{
						const FString& Scope = Entry.Scopes[ScopeIndex];
						if (!IsReadableScope(Scope))
						{
							RelevantError = UnreadableNameFailure(TEXT("name_discovery_scope_invalid"), Scope, FString::Printf(TEXT("entries[%d].scopes[%d]"), Index, ScopeIndex)); RelevantError.CandidateName = Entry.Name;
							break;
						}
						if (!Prefix.IsEmpty()) Prefix += TEXT(" · ");
						Prefix += Scope;
						if (ComparisonKey(Entry.Name + TEXT(" [") + Prefix + TEXT("]"), Comparison) == Wanted)
						{
							bMatches = true;
							break;
						}
					}
				}
				if (!bMatches) continue;
				Matches.Add(Index);
			}
		}

		bool TryBuildScopedDiscoveryNameForRepresentation(
			const TArray<FScopedNameDiscoveryEntry>& CleanEntries,
			const TMap<FString, TArray<int32>>& Groups,
			const int32 Index,
			const ENameComparison Comparison,
			FString& OutName,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutName.Reset();
			if (!CleanEntries.IsValidIndex(Index))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_match_index_out_of_range"), TEXT("The matched index is outside the discovery catalog."), TEXT("name_discovery"));
				OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = TEXT("matched_index");
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), CleanEntries.Num())); OutError.ActualValue = MakeShared<FJsonValueNumber>(Index);
				return false;
			}
			const FScopedNameDiscoveryEntry& Entry = CleanEntries[Index];
			if (!IsReadableName(Entry.Name))
			{
				OutError = UnreadableNameFailure(TEXT("name_discovery_entry_invalid"), Entry.Name, FString::Printf(TEXT("entries[%d].name"), Index));
				return false;
			}
			const TArray<int32>& Group = Groups.FindChecked(ComparisonKey(Entry.Name, Comparison));
			TArray<FNameSelectionGroup> NativeGroups;
			if (!TryGroupDiscoveryEntries(CleanEntries, Group, NativeGroups, OutError)) return false;
			auto IsReplayable = [&](const FString& PublicName)
			{
				TArray<int32> Matches;
				UE::UAI::Contract::FDiagnostic MatchError;
				CollectScopedNameMatches(CleanEntries, Groups, PublicName, Comparison, Matches, MatchError);
				return !Matches.IsEmpty() && !Matches.ContainsByPredicate([&](int32 Match) {
					return CleanEntries[Match].Identity != Entry.Identity;
				});
			};
			if (IsReplayable(Entry.Name))
			{
				OutName = Entry.Name;
				return true;
			}

			// A native object may have several provider representations. Select a
			// replayable scope from all of them; never discard alternatives first.
			int32 BestDepth = MAX_int32;
			for (const int32 Representation : Group)
			{
				const auto& Candidate = CleanEntries[Representation];
				if (Candidate.Identity != Entry.Identity) continue;
				FString Prefix;
				for (int32 Depth = 1; Depth <= Candidate.Scopes.Num(); ++Depth)
				{
					const FString& Scope = Candidate.Scopes[Depth - 1];
					if (!IsReadableScope(Scope)) break;
					if (!Prefix.IsEmpty()) Prefix += TEXT(" · ");
					Prefix += Scope;
					const FString PublicName = FString::Printf(TEXT("%s [%s]"), *Entry.Name, *Prefix);
					if (IsReplayable(PublicName))
					{
						if (Depth < BestDepth || (Depth == BestDepth && PublicName.Len() < OutName.Len()))
						{
							BestDepth = Depth;
							OutName = PublicName;
						}
						break;
					}
				}
			}
			if (!OutName.IsEmpty()) return true;

			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_semantic_scope_ambiguous"), TEXT("The available semantic scopes cannot uniquely distinguish this catalog entry from other entries with the same name."), TEXT("name_discovery"));
			OutError.SourceKind = TEXT("native_result"); OutError.CandidateName = Entry.Name;
			TArray<TSharedPtr<FJsonValue>> Scopes; for (const FString& Scope : Entry.Scopes) Scopes.Add(MakeShared<FJsonValueString>(Scope));
			OutError.PropertyPath = FString::Printf(TEXT("entries[%d].scopes"), Index); OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Scopes));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("scopes distinguishing %d same-name entries"), NativeGroups.Num()));
			return false;
		}

		bool TryBuildScopedDiscoveryNameAtIndex(
			const TArray<FScopedNameDiscoveryEntry>& CleanEntries,
			const TMap<FString, TArray<int32>>& Groups, const int32 Index,
			const ENameComparison Comparison, FString& OutName,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (TryBuildScopedDiscoveryNameForRepresentation(CleanEntries, Groups, Index, Comparison, OutName, OutError)) return true;
			if (!CleanEntries.IsValidIndex(Index) || CleanEntries[Index].Identity.IsEmpty()) return false;
			const auto FirstError = OutError;
			// Search may match an alias which cannot itself disambiguate the
			// object. Other native representations remain available for output.
			for (int32 Other = 0; Other < CleanEntries.Num(); ++Other)
			{
				if (CleanEntries[Other].Identity != CleanEntries[Index].Identity
					|| CleanEntries[Other].Name == CleanEntries[Index].Name) continue;
				if (TryBuildScopedDiscoveryNameForRepresentation(CleanEntries, Groups, Other, Comparison, OutName, OutError))
				{
					OutError.Reset();
					return true;
				}
			}
			OutName.Reset();
			OutError = FirstError;
			return false;
		}
	}

	void AppendReadableDiscoveryPathScopes(const FString& Path, TArray<FString>& OutScopes)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("/"), true);
		for (int32 Index = Segments.Num() - 1; Index >= 0; --Index)
		{
			// Keep repeated directory names: A/A and A are different namespaces.
			if (IsReadableScope(Segments[Index])) OutScopes.Add(Segments[Index]);
		}
	}

	FString MakeReadableDiscoveryPath(const FString& Path)
	{
		if (Path.IsEmpty()) return FString();
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		const FString Object = FPackageName::ObjectPathToObjectName(Path);
		TArray<FString> Parts;
		Package.ParseIntoArray(Parts, TEXT("/"), true);
		if (!Object.IsEmpty() && (Parts.IsEmpty() || Parts.Last() != Object)) Parts.Add(Object);
		for (const FString& Part : Parts)
			if (!IsReadableName(Part)) return FString();
		return FString::Join(Parts, TEXT(" · "));
	}

	bool IsReadableDiscoveryName(const FString& Value)
	{
		FString Clean = Value;
		Clean.TrimStartAndEndInline();
		return IsReadableName(Clean);
	}

	UE::UAI::Contract::FDiagnostic CandidateCatalogIncomplete(const FString& Reason, const FString& Target)
	{
		auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("candidate_catalog_incomplete"),
			Reason, TEXT("candidate_discovery"));
		Diagnostic.TargetPath = Target;
		return Diagnostic;
	}

	bool TryValidateNameDiscoveryCompleteness(
		const FNameDiscoveryCompleteness& Completeness,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Completeness.SourceDiagnostic.IsEmpty()
			&& Completeness.SourceDiagnostic.Severity == UE::UAI::Contract::ESeverity::Error)
		{
			OutError = Completeness.SourceDiagnostic;
			return false;
		}
		if (Completeness.TotalMatches < 0
			|| Completeness.ReturnedCount < 0
			|| Completeness.ReturnedCount != Completeness.TotalMatches)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_completeness_invalid"),
				FString::Printf(TEXT("Discovery matched %d entries but returned %d."),
					Completeness.TotalMatches, Completeness.ReturnedCount), TEXT("name_discovery"));
			return false;
		}
		return true;
	}

	namespace
	{
		bool TryProjectCandidateNames(const TArray<FString>& OrderedNames, ENameComparison Comparison,
			TArray<FString>& OutNames, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutNames.Reset();
			OutError.Reset();
			TSet<FString> Seen;
			for (const FString& Name : OrderedNames)
			{
				const FString Clean = Name.TrimStartAndEnd();
				if (!IsReadableName(Clean))
				{
					OutError = UnreadableNameFailure(TEXT("name_discovery_name_invalid"), Name, TEXT("name"));
					OutNames.Reset();
					return false;
				}
				const FString Key = ComparisonKey(Clean, Comparison);
				if (!Seen.Contains(Key)) { Seen.Add(Key); OutNames.Add(Clean); }
			}
			return true;
		}

		bool TrySerializeCandidateNames(const TArray<FString>& Names,
			const UE::UAI::Contract::FDiagnostic& SourceDiagnostic, FString& OutPayloadJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			auto Payload = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name));
			Payload->SetArrayField(TEXT("names"), MoveTemp(Values));
			if (!SourceDiagnostic.IsEmpty())
				Payload->SetArrayField(TEXT("diagnostics"), {MakeShared<FJsonValueObject>(SourceDiagnostic.ToPublicJson())});
			return CommandContract::Json::TryCanonicalize(Payload, OutPayloadJson, OutError);
		}
	}

	bool TryBuildCandidateDiscoveryNamesForIndices(const TArray<FNameSelectionEntry>& Entries,
		const TArray<int32>& MatchedIndices, ENameComparison Comparison,
		TArray<FString>& OutNames, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNames.Reset();
		TArray<FNameSelectionGroup> Groups;
		if (!TryGroupNameSelectionEntries(Entries, MatchedIndices, Groups, OutError)) return false;
		TArray<FString> OrderedNames;
		OrderedNames.Reserve(Groups.Num());
		for (const auto& Group : Groups) OrderedNames.Add(Entries[Group.EntryIndices[0]].Name);
		return TryProjectCandidateNames(OrderedNames, Comparison, OutNames, OutError);
	}

	bool TryBuildCandidateNamesOnlyPayload(const TArray<FNameSelectionEntry>& OrderedEntries,
		const ENameComparison Comparison, FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FDiagnostic& SourceDiagnostic)
	{
		TArray<int32> Indices;
		Indices.Reserve(OrderedEntries.Num());
		for (int32 Index = 0; Index < OrderedEntries.Num(); ++Index) Indices.Add(Index);
		return TryBuildCandidateNamesOnlyPayloadForIndices(OrderedEntries, Indices, Comparison, OutPayloadJson, OutError, SourceDiagnostic);
	}

	bool TryBuildCandidateNamesOnlyPayloadForIndices(const TArray<FNameSelectionEntry>& Entries,
		const TArray<int32>& MatchedIndices, const ENameComparison Comparison,
		FString& OutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError,
		const UE::UAI::Contract::FDiagnostic& SourceDiagnostic)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		if (!SourceDiagnostic.IsEmpty() && SourceDiagnostic.Severity == UE::UAI::Contract::ESeverity::Error)
		{
			OutError = SourceDiagnostic;
			return false;
		}
		TArray<FString> Names;
		if (!TryBuildCandidateDiscoveryNamesForIndices(Entries, MatchedIndices, Comparison, Names, OutError)) return false;
		return TrySerializeCandidateNames(Names, SourceDiagnostic, OutPayloadJson, OutError);
	}

	bool TryBuildNamesOnlyPayload(
		const TArray<FString>& OrderedNames,
		const ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();

		TSet<FString> SeenNames;
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Reserve(OrderedNames.Num());
		for (FString Name : OrderedNames)
		{
			Name.TrimStartAndEndInline();
			if (!IsReadableName(Name))
			{
				OutError = UnreadableNameFailure(TEXT("name_discovery_name_invalid"), Name, FString::Printf(TEXT("names[%d]"), Names.Num()));
				return false;
			}
			const FString Key = ComparisonKey(Name, Comparison);
			if (SeenNames.Contains(Key))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_scope_required"), TEXT("The discovery catalog contains a repeated public name and must provide a semantic scope to disambiguate it."), TEXT("name_discovery"));
				OutError.CandidateName = Name; OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = FString::Printf(TEXT("names[%d]"), Names.Num());
				return false;
			}
			SeenNames.Add(Key);
			Names.Add(MakeShared<FJsonValueString>(MoveTemp(Name)));
		}
		const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetArrayField(TEXT("names"), MoveTemp(Names));
		if (!CommandContract::Json::TryCanonicalize(
				Payload, OutPayloadJson, OutError))
		{
			OutPayloadJson.Reset();
			return false;
		}
		return true;
	}

	bool TryBuildScopedDiscoveryNames(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const ENameComparison Comparison,
		TArray<FString>& OutNames,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNames.Reset();
		OutError.Reset();
		TArray<FScopedNameDiscoveryEntry> CleanEntries;
		TMap<FString, TArray<int32>> Groups;
		if (!TryPrepareScopedEntries(
				Entries, Comparison, CleanEntries, Groups, OutError))
		{
			return false;
		}
		OutNames.SetNum(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (!TryBuildScopedDiscoveryNameAtIndex(
					CleanEntries, Groups, Index, Comparison,
					OutNames[Index], OutError))
			{
				OutNames.Reset();
				return false;
			}
		}

		TMap<FString, FString> IdentityByPublicName;
		for (int32 Index = 0; Index < OutNames.Num(); ++Index)
		{
			const FString Key = ComparisonKey(OutNames[Index], Comparison);
			const FString* Identity = IdentityByPublicName.Find(Key);
			if (Identity && *Identity != CleanEntries[Index].Identity)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_public_name_collision"), TEXT("Different native objects produced the same public scoped name."), TEXT("name_discovery"));
				OutError.CandidateName = OutNames[Index]; OutError.SourceKind = TEXT("native_result");
				OutNames.Reset();
				return false;
			}
			IdentityByPublicName.Add(Key, CleanEntries[Index].Identity);
		}
		return true;
	}

	bool TryAddDiscoveryScope(
		const TSharedRef<FJsonObject>& Scope, FString& InOutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FJsonObject> Payload;
		if (!CommandContract::Json::TryParseObject(InOutPayloadJson, Payload, OutError)) return false;
		Payload->SetObjectField(TEXT("scope"), Scope);
		return CommandContract::Json::TryCanonicalize(Payload, InOutPayloadJson, OutError);
	}

	bool TryBuildScopedNamesOnlyPayload(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TArray<int32> Indices;
		Indices.Reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			Indices.Add(Index);
		}
		TArray<FString> Names;
		if (!TryBuildScopedDiscoveryNamesForIndices(
				Entries, Indices, Comparison, Names, OutError))
		{
			OutPayloadJson.Reset();
			return false;
		}
		return TryBuildNamesOnlyPayload(
			Names, Comparison, OutPayloadJson, OutError);
	}

	bool TryBuildScopedNamesOnlyPayloadForIndices(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const TArray<int32>& MatchedIndices,
		const ENameComparison Comparison,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TArray<FString> Names;
		if (!TryBuildScopedDiscoveryNamesForIndices(
				Entries, MatchedIndices, Comparison, Names, OutError))
		{
			return false;
		}
		return TryBuildNamesOnlyPayload(
			Names, Comparison, OutPayloadJson, OutError);
	}

	bool TryBuildScopedDiscoveryNamesForIndices(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const TArray<int32>& MatchedIndices,
		const ENameComparison Comparison,
		TArray<FString>& OutNames,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutNames.Reset();
		OutError.Reset();
		TArray<FScopedNameDiscoveryEntry> CleanEntries;
		TMap<FString, TArray<int32>> Groups;
		if (!TryPrepareScopedEntries(
				Entries, Comparison, CleanEntries, Groups, OutError))
		{
			return false;
		}

		TArray<FNameSelectionGroup> MatchedGroups;
		if (!TryGroupDiscoveryEntries(CleanEntries, MatchedIndices, MatchedGroups, OutError, TEXT("name_discovery_match_index_out_of_range"))) return false;
		TSet<FString> SeenPublicNames;
		OutNames.Reserve(MatchedGroups.Num());
		for (const FNameSelectionGroup& Group : MatchedGroups)
		{
			FString PublicName;
			if (!TryBuildScopedDiscoveryNameAtIndex(CleanEntries, Groups, Group.EntryIndices[0], Comparison, PublicName, OutError))
			{
				OutNames.Reset();
				return false;
			}
			const FString PublicKey = ComparisonKey(PublicName, Comparison);
			if (SeenPublicNames.Contains(PublicKey))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_public_name_collision"), TEXT("Different native objects produced the same public scoped name."), TEXT("name_discovery"));
				OutError.CandidateName = PublicName; OutError.SourceKind = TEXT("native_result");
				OutNames.Reset();
				return false;
			}
			SeenPublicNames.Add(PublicKey);
			OutNames.Add(MoveTemp(PublicName));
		}
		return true;
	}

	bool TryResolveScopedDiscoveryName(
		const TArray<FScopedNameDiscoveryEntry>& Entries,
		const FString& PublicName,
		const ENameComparison Comparison,
		int32& OutIndex,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return FScopedNameDiscoveryIndex(Entries, Comparison).TryResolve(PublicName, OutIndex, OutError);
	}

	FScopedNameDiscoveryIndex::FScopedNameDiscoveryIndex(
		const TArray<FScopedNameDiscoveryEntry>& Entries, const ENameComparison InComparison)
		: Comparison(InComparison)
	{
		UE::UAI::Contract::FDiagnostic Error;
		TryPrepareScopedEntries(Entries, Comparison, CleanEntries, Groups, Error);
	}

	bool FScopedNameDiscoveryIndex::TryResolve(
		const FString& PublicName, int32& OutIndex, UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutIndex = INDEX_NONE;
		OutError.Reset();
		FString CleanPublicName = PublicName;
		CleanPublicName.TrimStartAndEndInline();
		if (CleanPublicName.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_name_not_found"), TEXT("A non-empty public discovery name is required."), TEXT("name_discovery"));
			OutError.PropertyPath = TEXT("name"); OutError.ActualValue = MakeShared<FJsonValueString>(PublicName);
			return false;
		}
		TArray<int32> Matches;
		UE::UAI::Contract::FDiagnostic RelevantError;
		CollectScopedNameMatches(CleanEntries, Groups, CleanPublicName, Comparison, Matches, RelevantError);
		TArray<FNameSelectionGroup> NativeGroups;
		if (!TryGroupDiscoveryEntries(CleanEntries, Matches, NativeGroups, OutError)) return false;
		if (NativeGroups.Num() == 1)
		{
			OutIndex = NativeGroups[0].EntryIndices[0];
			return true;
		}
		if (NativeGroups.Num() > 1)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_semantic_scope_ambiguous"), TEXT("The supplied name matches multiple native objects; use a scoped public name."), TEXT("name_discovery"));
			OutError.CandidateName = PublicName;
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(NativeGroups.Num());
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const auto& Group : NativeGroups)
			{
				FString Name;
				UE::UAI::Contract::FDiagnostic ScopeError;
				if (TryBuildScopedDiscoveryNameAtIndex(CleanEntries, Groups, Group.EntryIndices[0], Comparison, Name, ScopeError))
					Choices.Add(MakeShared<FJsonValueString>(MoveTemp(Name)));
			}
			if (Choices.Num() == NativeGroups.Num())
				OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
			return false;
		}
		OutError = RelevantError.IsEmpty()
			? UE::UAI::Contract::FDiagnostic::Failure(TEXT("name_discovery_name_not_found"), Comparison == ENameComparison::IgnoreCase ? TEXT("No discovery entry matches the supplied public name using case-insensitive comparison.") : TEXT("No discovery entry matches the supplied public name using case-sensitive comparison."), TEXT("name_discovery"))
			: MoveTemp(RelevantError);
		if (OutError.CandidateName.IsEmpty() && IsReadableName(PublicName)) OutError.CandidateName = PublicName;
		return false;
	}

}
