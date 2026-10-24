// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString NormalizeRelativePath(const FString& Input)
	{
		FString Result = Input;
		FPaths::NormalizeFilename(Result);
		while (Result.StartsWith(TEXT("/"))) Result.RightChopInline(1, EAllowShrinking::No);
		return Result;
	}

	bool MatchSegmentRecursive(
		const FString& Pattern,
		int32 PatternIndex,
		const FString& Value,
		int32 ValueIndex,
		TMap<uint64, bool>& Memo)
	{
		const uint64 Key = (static_cast<uint64>(PatternIndex) << 32) | static_cast<uint32>(ValueIndex);
		if (const bool* Cached = Memo.Find(Key)) return *Cached;

		bool bMatches = false;
		if (PatternIndex == Pattern.Len())
		{
			bMatches = ValueIndex == Value.Len();
		}
		else if (Pattern[PatternIndex] == TEXT('*'))
		{
			bMatches = MatchSegmentRecursive(Pattern, PatternIndex + 1, Value, ValueIndex, Memo)
				|| (ValueIndex < Value.Len()
					&& MatchSegmentRecursive(Pattern, PatternIndex, Value, ValueIndex + 1, Memo));
		}
		else if (ValueIndex < Value.Len())
		{
			const TCHAR PatternChar = Pattern[PatternIndex];
			const TCHAR ValueChar = Value[ValueIndex];
			bMatches = (PatternChar == TEXT('?') || FChar::ToLower(PatternChar) == FChar::ToLower(ValueChar))
				&& MatchSegmentRecursive(Pattern, PatternIndex + 1, Value, ValueIndex + 1, Memo);
		}
		Memo.Add(Key, bMatches);
		return bMatches;
	}

	bool MatchSegment(const FString& Pattern, const FString& Value)
	{
		TMap<uint64, bool> Memo;
		return MatchSegmentRecursive(Pattern, 0, Value, 0, Memo);
	}

	bool MatchPathRecursive(
		const TArray<FString>& PatternSegments,
		int32 PatternIndex,
		const TArray<FString>& PathSegments,
		int32 PathIndex,
		TMap<uint64, bool>& Memo)
	{
		const uint64 Key = (static_cast<uint64>(PatternIndex) << 32) | static_cast<uint32>(PathIndex);
		if (const bool* Cached = Memo.Find(Key)) return *Cached;

		bool bMatches = false;
		if (PatternIndex == PatternSegments.Num())
		{
			bMatches = PathIndex == PathSegments.Num();
		}
		else if (PatternSegments[PatternIndex] == TEXT("**"))
		{
			bMatches = MatchPathRecursive(PatternSegments, PatternIndex + 1, PathSegments, PathIndex, Memo)
				|| (PathIndex < PathSegments.Num()
					&& MatchPathRecursive(PatternSegments, PatternIndex, PathSegments, PathIndex + 1, Memo));
		}
		else if (PathIndex < PathSegments.Num())
		{
			bMatches = MatchSegment(PatternSegments[PatternIndex], PathSegments[PathIndex])
				&& MatchPathRecursive(PatternSegments, PatternIndex + 1, PathSegments, PathIndex + 1, Memo);
		}
		Memo.Add(Key, bMatches);
		return bMatches;
	}

	void SetFailure(UeAgentAuthoringWorkspaceRouter::FRouteFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
        const UeAgentAuthoringWorkspaceRouter::FSourceProvenance* Provenance = nullptr)
    {
        OutFailure.Diagnostic = MoveTemp(Diagnostic);
        if (Provenance)
        {
            const auto Source = Provenance->ToDiagnosticSource();
            if (OutFailure.Diagnostic.Source.WorkspaceSource.IsEmpty()) OutFailure.Diagnostic.Source.WorkspaceSource = Source.WorkspaceSource;
            if (OutFailure.Diagnostic.Source.SourceFile.IsEmpty()) OutFailure.Diagnostic.Source.SourceFile = Source.SourceFile;
            if (OutFailure.Diagnostic.Source.SourceUnit.IsEmpty()) OutFailure.Diagnostic.Source.SourceUnit = Source.SourceUnit;
            if (OutFailure.Diagnostic.Source.InternalJsonPath.IsEmpty()) OutFailure.Diagnostic.Source.InternalJsonPath = Source.InternalJsonPath;
            if (OutFailure.Diagnostic.Source.JsonPath.IsEmpty()) OutFailure.Diagnostic.Source.JsonPath = Source.JsonPath;
            if (OutFailure.Diagnostic.Source.Line <= 0) OutFailure.Diagnostic.Source.Line = Source.Line;
            if (OutFailure.Diagnostic.Source.Column <= 0) OutFailure.Diagnostic.Source.Column = Source.Column;
        }
    }

	bool IsRequiredCardinality(const FString& Cardinality)
	{
		return Cardinality == TEXT("one") || Cardinality == TEXT("one_or_more");
	}

	bool ExceedsCardinality(const FString& Cardinality, int32 Count)
	{
		return (Cardinality == TEXT("one") || Cardinality == TEXT("zero_or_one")) && Count > 1;
	}

	TSharedPtr<const FJsonObject> MakeEmptyDocument()
	{
		return MakeShared<FJsonObject>();
	}
}

namespace UeAgentAuthoringWorkspaceRouter
{
	UeAgentAuthoringDiagnostics::FSourceLocation FSourceProvenance::ToDiagnosticSource() const
	{
		UeAgentAuthoringDiagnostics::FSourceLocation Result;
		Result.WorkspaceSource = WorkspaceSource;
		Result.SourceFile = SourceFile;
		Result.SourceUnit = SourceUnit;
		Result.JsonPath = TEXT("/");
		Result.Line = Line;
		Result.Column = Column;
		return Result;
	}

	const FUeAgentHjsonSourceLocation* FWorkspaceUnit::FindSourceLocation(const FString& JsonPointer) const
	{
		return SourceLocations.Find(JsonPointer);
	}

	UeAgentAuthoringDiagnostics::FSourceLocation FWorkspaceUnit::ResolveDiagnosticSource(const FString& JsonPointer) const
	{
		if (ResolveOriginalSource)
		{
			auto Original = ResolveOriginalSource(JsonPointer);
			if (!Original.SourceFile.IsEmpty()) return Original;
		}
		auto Location = Provenance.ToDiagnosticSource();
		Location.JsonPath = JsonPointer.IsEmpty() ? TEXT("/") : JsonPointer;
		Location.InternalJsonPath = Location.JsonPath;
		FString Ancestor = JsonPointer;
		for (;;)
		{
			if (const auto* Parsed = FindSourceLocation(Ancestor.IsEmpty() ? TEXT("/") : Ancestor))
			{
				if (!Parsed->FilePath.IsEmpty()) Location.SourceFile = Parsed->FilePath;
				Location.JsonPath = Ancestor.IsEmpty() ? TEXT("/") : Ancestor;
				Location.Line = Parsed->Line;
				Location.Column = Parsed->Column;
				break;
			}
			int32 Slash;
			if (!Ancestor.FindLastChar(TEXT('/'), Slash)) break;
			Ancestor.LeftInline(Slash);
		}
		return Location;
	}

	TArray<const FWorkspaceUnit*> FWorkspace::FindUnits(const FString& UnitId) const
	{
		TArray<const FWorkspaceUnit*> Result;
		for (const FWorkspaceUnit& Unit : Units)
		{
			if (Unit.UnitId == UnitId) Result.Add(&Unit);
		}
		return Result;
	}

	const FWorkspaceUnit* FWorkspace::FindSingleUnit(const FString& UnitId) const
	{
		const FWorkspaceUnit* Result = nullptr;
		for (const FWorkspaceUnit& Unit : Units)
		{
			if (Unit.UnitId != UnitId) continue;
			if (Result) return nullptr;
			Result = &Unit;
		}
		return Result;
	}

	TSharedRef<FJsonObject> FWorkspace::ToJsonObject(bool bIncludeDocuments) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), TEXT("uai.apply_workspace_runtime.v1"));
		Object->SetStringField(TEXT("contract_id"), ContractId);
		Object->SetStringField(TEXT("contract_version"), ContractVersion);
		Object->SetStringField(TEXT("profile_id"), ProfileId);
		Object->SetStringField(TEXT("source_kind"), UeAgentApplyWorkspaceContract::SourceKindToString(SourceKind));
		if (!WorkspaceRoot.IsEmpty()) Object->SetStringField(TEXT("workspace_root"), WorkspaceRoot);

		TArray<TSharedPtr<FJsonValue>> UnitValues;
		UnitValues.Reserve(Units.Num());
		for (const FWorkspaceUnit& Unit : Units)
		{
			TSharedRef<FJsonObject> UnitObject = MakeShared<FJsonObject>();
			UnitObject->SetStringField(TEXT("unit_id"), Unit.UnitId);
			UnitObject->SetStringField(TEXT("canonical_unit_key"), Unit.CanonicalUnitKey);
			UnitObject->SetStringField(TEXT("relative_path"), Unit.RelativePath);
			UnitObject->SetStringField(TEXT("role"), Unit.Role);
			UnitObject->SetStringField(TEXT("route_id"), Unit.RouteId);
			UnitObject->SetStringField(TEXT("apply_contract_ref"), Unit.ApplyContractRef);
			UnitObject->SetStringField(TEXT("merge_policy"), Unit.MergePolicy);
			UnitObject->SetStringField(TEXT("presence"), UnitPresenceToString(Unit.Presence));
			UnitObject->SetStringField(TEXT("action"), UnitActionToString(Unit.Action));
			TSharedRef<FJsonObject> ProvenanceObject = MakeShared<FJsonObject>();
			ProvenanceObject->SetStringField(TEXT("source_kind"), UeAgentApplyWorkspaceContract::SourceKindToString(Unit.Provenance.SourceKind));
			ProvenanceObject->SetStringField(TEXT("workspace_source"), Unit.Provenance.WorkspaceSource);
			ProvenanceObject->SetStringField(TEXT("source_file"), Unit.Provenance.SourceFile);
			ProvenanceObject->SetStringField(TEXT("source_unit"), Unit.Provenance.SourceUnit);
			ProvenanceObject->SetBoolField(TEXT("virtual"), Unit.Provenance.bVirtual);
			UnitObject->SetObjectField(TEXT("provenance"), ProvenanceObject);
			if (bIncludeDocuments && Unit.Document.IsValid())
			{
				TSharedRef<FJsonObject> DocumentCopy = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Unit.Document->Values)
				{
					DocumentCopy->SetField(Pair.Key, Pair.Value);
				}
				UnitObject->SetObjectField(TEXT("document"), DocumentCopy);
			}
			UnitValues.Add(MakeShared<FJsonValueObject>(UnitObject));
		}
		Object->SetArrayField(TEXT("units"), UnitValues);
		return Object;
	}

	FString UnitPresenceToString(EUnitPresence Presence)
	{
		switch (Presence)
		{
		case EUnitPresence::Absent: return TEXT("absent");
		case EUnitPresence::Empty: return TEXT("empty");
		case EUnitPresence::Present: return TEXT("present");
		default: return TEXT("unknown");
		}
	}

	FString UnitActionToString(EUnitAction Action)
	{
		switch (Action)
		{
		case EUnitAction::Apply: return TEXT("apply");
		case EUnitAction::ExplicitEmpty: return TEXT("explicit_empty");
		case EUnitAction::EmptyValue: return TEXT("empty_value");
		case EUnitAction::Remove: return TEXT("remove");
		case EUnitAction::NoChange: return TEXT("no_change");
		default: return TEXT("unknown");
		}
	}

	bool MatchesPathPattern(const FString& PathPattern, const FString& RelativePath)
	{
		TArray<FString> PatternSegments;
		TArray<FString> PathSegments;
		NormalizeRelativePath(PathPattern).ParseIntoArray(PatternSegments, TEXT("/"), true);
		NormalizeRelativePath(RelativePath).ParseIntoArray(PathSegments, TEXT("/"), true);
		TMap<uint64, bool> Memo;
		return MatchPathRecursive(PatternSegments, 0, PathSegments, 0, Memo);
	}

	bool RouteDocuments(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		UeAgentApplyWorkspaceContract::ESourceKind SourceKind,
		const FString& WorkspaceRoot,
		const TArray<FDocumentCandidate>& Candidates,
		FWorkspace& OutWorkspace,
		FRouteFailure& OutFailure)
	{
		OutWorkspace = FWorkspace();
		OutFailure = FRouteFailure();
        UE::UAI::Contract::FDiagnostic ContractError;
        if (!UeAgentApplyWorkspaceContract::ValidateContract(Contract, ContractError))
        {
            SetFailure(OutFailure, MoveTemp(ContractError));
			return false;
		}

		TArray<TArray<const FDocumentCandidate*>> CandidatesByUnit;
		CandidatesByUnit.SetNum(Contract.Units.Num());
		FSourceProvenance WorkspaceProvenance;
		WorkspaceProvenance.SourceKind = SourceKind;
		WorkspaceProvenance.WorkspaceSource = WorkspaceRoot;
		WorkspaceProvenance.SourceFile = WorkspaceRoot;
		WorkspaceProvenance.bVirtual = SourceKind != UeAgentApplyWorkspaceContract::ESourceKind::Directory;
		if (!Candidates.IsEmpty()) WorkspaceProvenance = Candidates[0].Provenance;
		const UeAgentApplyWorkspaceContract::FSourceDescriptor* SourceDescriptor =
			Contract.FindSource(SourceKind);
		const bool bRoutedWorkspace =
			SourceKind == UeAgentApplyWorkspaceContract::ESourceKind::Directory
			|| (SourceDescriptor && SourceDescriptor->EntryMode == TEXT("virtual_bundle"));
		if (bRoutedWorkspace)
		{
			for (const FDocumentCandidate& Candidate : Candidates)
			{
				TArray<int32> Matches;
				for (int32 UnitIndex = 0; UnitIndex < Contract.Units.Num(); ++UnitIndex)
				{
					if (MatchesPathPattern(Contract.Units[UnitIndex].PathPattern, Candidate.RelativePath))
					{
						Matches.Add(UnitIndex);
					}
				}
				if (Matches.IsEmpty())
				{
					SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_file_unknown"), TEXT("The file is not supported by this workspace contract."), TEXT("workspace_route"), Candidate.RelativePath), &Candidate.Provenance);
					return false;
				}
				if (Matches.Num() > 1)
				{
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_file_route_ambiguous"), TEXT("The file matches multiple workspace routes."), TEXT("workspace_route"));
                    Diagnostic.CandidateName = Candidate.RelativePath;
                    Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
                    Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
                    TArray<TSharedPtr<FJsonValue>> Choices;
                    for (int32 Match : Matches) {
                        const auto& Route = Contract.Units[Match];
                        const auto Choice = MakeShared<FJsonObject>();
                        Choice->SetStringField(TEXT("unit_id"), Route.UnitId);
                        Choice->SetStringField(TEXT("route_id"), Route.RouteId);
                        Choice->SetStringField(TEXT("path_pattern"), Route.PathPattern);
                        Choices.Add(MakeShared<FJsonValueObject>(Choice));
                    }
                    Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
                    SetFailure(OutFailure, MoveTemp(Diagnostic), &Candidate.Provenance);
					return false;
				}
				CandidatesByUnit[Matches[0]].Add(&Candidate);
			}

			const int32 EntryUnitIndex = Contract.Units.IndexOfByPredicate(
				[&Contract](const UeAgentApplyWorkspaceContract::FUnitDescriptor& Unit)
				{
					return Unit.UnitId == Contract.Entry.VirtualUnitId;
				});
			bool bEntryRoutedToVirtualUnit = false;
			if (CandidatesByUnit.IsValidIndex(EntryUnitIndex))
			{
				bEntryRoutedToVirtualUnit = CandidatesByUnit[EntryUnitIndex].ContainsByPredicate(
					[&Contract](const FDocumentCandidate* Candidate)
					{
						return Candidate && Candidate->RelativePath.Equals(Contract.Entry.DirectoryEntry, ESearchCase::IgnoreCase);
					});
			}
			if (!bEntryRoutedToVirtualUnit)
			{
				FSourceProvenance EntryProvenance = WorkspaceProvenance;
				EntryProvenance.SourceUnit = Contract.Entry.VirtualUnitId;
				EntryProvenance.RelativePath = Contract.Entry.DirectoryEntry;
                auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_entry_missing"), TEXT("The entry file did not route to the workspace's declared entry unit."), TEXT("workspace_route"));
                Diagnostic.CandidateName = Contract.Entry.VirtualUnitId;
                Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Contract.Entry.DirectoryEntry);
                Diagnostic.ActualValuePresent = false;
                SetFailure(OutFailure, MoveTemp(Diagnostic), &EntryProvenance);
				return false;
			}
		}
		else
		{
			if (Candidates.Num() != 1)
			{
                auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), TEXT("document_count"),
                    MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(Candidates.Num()), TEXT("workspace_route"), TEXT("A virtual workspace source requires exactly one document."));
                SetFailure(OutFailure, MoveTemp(Diagnostic), &WorkspaceProvenance);
				return false;
			}
			const int32 VirtualUnitIndex = Contract.Units.IndexOfByPredicate(
				[&Contract](const UeAgentApplyWorkspaceContract::FUnitDescriptor& Unit)
				{
					return Unit.UnitId == Contract.Entry.VirtualUnitId;
				});
			if (!CandidatesByUnit.IsValidIndex(VirtualUnitIndex))
			{
                auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("apply_workspace_contract_invalid"), TEXT("The workspace contract does not define its virtual entry unit."), TEXT("workspace_route"));
                Diagnostic.CandidateName = Contract.Entry.VirtualUnitId;
                Diagnostic.PropertyPath = TEXT("/entry/virtual_unit_id");
                Diagnostic.SourceKind = TEXT("native_result");
                Diagnostic.ExpectedValuePresent = true;
                Diagnostic.ActualValuePresent = false;
                SetFailure(OutFailure, MoveTemp(Diagnostic));
				return false;
			}
			CandidatesByUnit[VirtualUnitIndex].Add(&Candidates[0]);
		}

		OutWorkspace.ContractId = Contract.ContractId;
		OutWorkspace.ContractVersion = Contract.ContractVersion;
		OutWorkspace.ProfileId = Contract.ProfileId;
		OutWorkspace.SourceKind = SourceKind;
		OutWorkspace.WorkspaceRoot = WorkspaceRoot;

		for (int32 UnitIndex = 0; UnitIndex < Contract.Units.Num(); ++UnitIndex)
		{
			const UeAgentApplyWorkspaceContract::FUnitDescriptor& Descriptor = Contract.Units[UnitIndex];
			TArray<const FDocumentCandidate*>& UnitCandidates = CandidatesByUnit[UnitIndex];
			UnitCandidates.Sort([](const FDocumentCandidate& Left, const FDocumentCandidate& Right)
			{
				const int32 Folded = Left.RelativePath.Compare(Right.RelativePath, ESearchCase::IgnoreCase);
				return Folded == 0
					? Left.RelativePath.Compare(Right.RelativePath, ESearchCase::CaseSensitive) < 0
					: Folded < 0;
			});

			if (ExceedsCardinality(Descriptor.Cardinality, UnitCandidates.Num()))
			{
				const FSourceProvenance* Provenance = UnitCandidates.IsEmpty() ? nullptr : &UnitCandidates[0]->Provenance;
                auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_duplicate_target"), TEXT("document_count"),
                    MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(UnitCandidates.Num()), TEXT("workspace_route"), TEXT("Multiple documents target a workspace unit that permits only one."));
                Diagnostic.CandidateName = Descriptor.UnitId;
                TArray<TSharedPtr<FJsonValue>> Choices;
                for (const auto* Candidate : UnitCandidates) if (Candidate) Choices.Add(MakeShared<FJsonValueString>(Candidate->RelativePath));
                Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
                SetFailure(OutFailure, MoveTemp(Diagnostic), Provenance);
				return false;
			}
			if (UnitCandidates.IsEmpty())
			{
				if (Descriptor.bRequired || IsRequiredCardinality(Descriptor.Cardinality) || Descriptor.AbsentSemantics == TEXT("error"))
				{
					FSourceProvenance MissingProvenance = WorkspaceProvenance;
					MissingProvenance.SourceUnit = Descriptor.UnitId;
					MissingProvenance.RelativePath = Descriptor.PathPattern;
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_entry_missing"), TEXT("No document matches a required workspace unit."), TEXT("workspace_route"));
                    Diagnostic.CandidateName = Descriptor.UnitId;
                    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Descriptor.PathPattern);
                    Diagnostic.ActualValuePresent = false;
                    SetFailure(OutFailure, MoveTemp(Diagnostic), &MissingProvenance);
					return false;
				}

				FWorkspaceUnit Unit;
				Unit.UnitId = Descriptor.UnitId;
				Unit.CanonicalUnitKey = Descriptor.UnitId;
				Unit.RelativePath = Descriptor.PathPattern;
				Unit.Role = Descriptor.Role;
				Unit.RouteId = Descriptor.RouteId;
				Unit.ApplyContractRef = Descriptor.ApplyContractRef;
				Unit.MergePolicy = Descriptor.MergePolicy;
				Unit.Presence = EUnitPresence::Absent;
				Unit.Action = Descriptor.AbsentSemantics == TEXT("empty_value")
					? EUnitAction::EmptyValue
					: EUnitAction::NoChange;
				if (Unit.Action == EUnitAction::EmptyValue) Unit.Document = MakeEmptyDocument();
				Unit.Provenance = WorkspaceProvenance;
				Unit.Provenance.SourceUnit = Descriptor.UnitId;
				Unit.Provenance.RelativePath = Descriptor.PathPattern;
				Unit.Provenance.bVirtual = SourceKind != UeAgentApplyWorkspaceContract::ESourceKind::Directory;
				OutWorkspace.Units.Add(MoveTemp(Unit));
				continue;
			}

			for (const FDocumentCandidate* Candidate : UnitCandidates)
			{
				if (!Candidate)
				{
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The routed unit contains a null document candidate."), TEXT("workspace_route"));
                    Diagnostic.CandidateName = Descriptor.UnitId;
                    Diagnostic.SourceKind = TEXT("native_result");
                    Diagnostic.ExpectedValuePresent = true;
                    Diagnostic.ActualValuePresent = false;
                    SetFailure(OutFailure, MoveTemp(Diagnostic));
					return false;
				}

				FWorkspaceUnit Unit;
				Unit.UnitId = Descriptor.UnitId;
				Unit.CanonicalUnitKey = UnitCandidates.Num() == 1
					? Descriptor.UnitId
					: Descriptor.UnitId + TEXT(":") + Candidate->RelativePath.ToLower();
				Unit.RelativePath = Candidate->RelativePath;
				Unit.Role = Descriptor.Role;
				Unit.RouteId = Descriptor.RouteId;
				Unit.ApplyContractRef = Descriptor.ApplyContractRef;
				Unit.MergePolicy = Descriptor.MergePolicy;
				Unit.Provenance = Candidate->Provenance;
				Unit.SourceLocations = Candidate->SourceLocations;
                Unit.ResolveOriginalSource = Candidate->ResolveOriginalSource;

				if (Candidate->bEmpty)
				{
					Unit.Presence = EUnitPresence::Empty;
					if (Descriptor.EmptySemantics == TEXT("error"))
					{
						SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The workspace contract does not allow an empty document for this unit."), TEXT("workspace_route"), TEXT("empty_unit:") + Descriptor.UnitId), &Candidate->Provenance);
						return false;
					}
					if (Descriptor.EmptySemantics == TEXT("explicit_empty"))
					{
						Unit.Action = EUnitAction::ExplicitEmpty;
						Unit.Document = MakeEmptyDocument();
					}
					else if (Descriptor.EmptySemantics == TEXT("remove")) Unit.Action = EUnitAction::Remove;
					else Unit.Action = EUnitAction::NoChange;
				}
				else
				{
					if (!Candidate->Document.IsValid())
					{
                        auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_source_invalid"), FString(),
                            MakeShared<FJsonValueString>(TEXT("parsed document object")), nullptr, TEXT("workspace_route"), TEXT("The candidate is marked nonempty but has no parsed document object."));
                        Diagnostic.CandidateName = Candidate->RelativePath;
                        SetFailure(OutFailure, MoveTemp(Diagnostic), &Candidate->Provenance);
						return false;
					}
					Unit.Presence = EUnitPresence::Present;
					Unit.Action = EUnitAction::Apply;
					Unit.Document = Candidate->Document;
				}
				OutWorkspace.Units.Add(MoveTemp(Unit));
			}
		}
		return true;
	}
}
