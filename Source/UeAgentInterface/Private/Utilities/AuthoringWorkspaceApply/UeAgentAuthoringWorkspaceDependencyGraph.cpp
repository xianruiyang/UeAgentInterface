// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UeAgentAuthoringWorkspaceDependencyGraph;
	using namespace UeAgentAuthoringWorkspaceRouter;

	FString FoldKey(const FString& Value)
	{
		FString Result = Value;
		Result.TrimStartAndEndInline();
		return Result.ToLower();
	}

	bool NormalizeWorkspaceRelativeReference(const FString& Reference, FString& OutNormalized)
	{
		OutNormalized.Reset();
		FString Candidate = Reference;
		Candidate.TrimStartAndEndInline();
		FPaths::NormalizeFilename(Candidate);
		if (Candidate.IsEmpty() || !FPaths::IsRelative(Candidate) || Candidate.Contains(TEXT(":"))) return false;

		TArray<FString> Segments;
		Candidate.ParseIntoArray(Segments, TEXT("/"), false);
		TArray<FString> NormalizedSegments;
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".")) continue;
			if (Segment == TEXT("..")) return false;
			NormalizedSegments.Add(Segment);
		}
		if (NormalizedSegments.IsEmpty()) return false;
		OutNormalized = FString::Join(NormalizedSegments, TEXT("/"));
		return true;
	}

    UeAgentAuthoringDiagnostics::FSourceLocation MakeSourceLocation(const FWorkspaceUnit* Unit, const FString& JsonPointer)
    {
        return Unit ? Unit->ResolveDiagnosticSource(JsonPointer) : UeAgentAuthoringDiagnostics::FSourceLocation();
    }

	void SetFailure(
		FGraphFailure& OutFailure,
		UE::UAI::Contract::FDiagnostic Diagnostic,
		const FWorkspaceUnit* SourceUnit = nullptr,
		const FString& JsonPointer = TEXT("/"))
	{
		OutFailure.Diagnostic = MoveTemp(Diagnostic);
		OutFailure.Diagnostic.bRetrySafe = true;
		OutFailure.Diagnostic.SuggestedFix = TEXT("Fix the workspace reference or dependency declaration and retry the complete workspace.");
        if (OutFailure.Diagnostic.Source.SourceFile.IsEmpty() && !OutFailure.Diagnostic.Source.bCommandInput
            && OutFailure.Diagnostic.SourceKind != TEXT("native_result") && OutFailure.Diagnostic.SourceKind != TEXT("command_input"))
            OutFailure.Diagnostic.Source = MakeSourceLocation(SourceUnit, JsonPointer);
	}

	void AddAlias(TMap<FString, TArray<int32>>& Aliases, const FString& Alias, int32 UnitIndex)
	{
		if (Alias.IsEmpty()) return;
		Aliases.FindOrAdd(FoldKey(Alias)).AddUnique(UnitIndex);
	}

	int32 CompareUnitKeys(const FWorkspace& Workspace, int32 LeftIndex, int32 RightIndex)
	{
		const FString& Left = Workspace.Units[LeftIndex].CanonicalUnitKey;
		const FString& Right = Workspace.Units[RightIndex].CanonicalUnitKey;
		const int32 Folded = Left.Compare(Right, ESearchCase::IgnoreCase);
		return Folded != 0 ? Folded : Left.Compare(Right, ESearchCase::CaseSensitive);
	}

	bool FindDeterministicCycle(
		const FWorkspace& Workspace,
		const TArray<TArray<int32>>& Adjacency,
		const TArray<int32>& CandidateIndices,
		TArray<FString>& OutCycle)
	{
		OutCycle.Reset();
		TSet<int32> Candidates(CandidateIndices);
		TArray<uint8> States;
		States.Init(0, Workspace.Units.Num());
		TArray<int32> Stack;

		TFunction<bool(int32)> Visit = [&](int32 UnitIndex)
		{
			States[UnitIndex] = 1;
			Stack.Add(UnitIndex);
			TArray<int32> Next = Adjacency[UnitIndex];
			Next.Sort([&](int32 Left, int32 Right)
			{
				return CompareUnitKeys(Workspace, Left, Right) < 0;
			});
			for (int32 NextIndex : Next)
			{
				if (!Candidates.Contains(NextIndex)) continue;
				if (States[NextIndex] == 0 && Visit(NextIndex)) return true;
				if (States[NextIndex] != 1) continue;
				const int32 Start = Stack.Find(NextIndex);
				if (Start == INDEX_NONE) continue;
				for (int32 Index = Start; Index < Stack.Num(); ++Index)
				{
					OutCycle.Add(Workspace.Units[Stack[Index]].CanonicalUnitKey);
				}
				OutCycle.Add(Workspace.Units[NextIndex].CanonicalUnitKey);
				return true;
			}
			Stack.Pop(EAllowShrinking::No);
			States[UnitIndex] = 2;
			return false;
		};

		TArray<int32> SortedCandidates = CandidateIndices;
		SortedCandidates.Sort([&](int32 Left, int32 Right)
		{
			return CompareUnitKeys(Workspace, Left, Right) < 0;
		});
		for (int32 UnitIndex : SortedCandidates)
		{
			if (States[UnitIndex] == 0 && Visit(UnitIndex)) return true;
		}
		return false;
	}
}

namespace UeAgentAuthoringWorkspaceDependencyGraph
{
	FString DependencySourceToString(EDependencySource Source)
	{
		return Source == EDependencySource::Reference ? TEXT("reference") : TEXT("contract");
	}

 bool FDependencyGraph::IsReadyFor(const FWorkspace& Workspace) const
 {
   return IsReadyFor(Workspace, nullptr);
 }

 bool FDependencyGraph::IsReadyFor(const FWorkspace& Workspace, UeAgentAuthoringDiagnostics::FDiagnostic* OutError) const
 {
   if (OutError) OutError->Reset();
   const auto Reject = [&](const TCHAR* Reason, const FString& Path,
                           const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual) {
     if (OutError) {
       *OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_dependency_graph_not_ready"), Path, Expected, Actual, TEXT("workspace_preflight"), Reason);
       OutError->SourceKind = TEXT("native_result");
     }
     return false;
   };
   if (ContractId != Workspace.ContractId)
     return Reject(TEXT("The dependency graph belongs to a different workspace contract."), TEXT("/graph/contract_id"), MakeShared<FJsonValueString>(Workspace.ContractId), MakeShared<FJsonValueString>(ContractId));
   if (OrderedUnitIndices.Num() != Workspace.Units.Num())
     return Reject(TEXT("The dependency order does not contain exactly one entry per workspace unit."), TEXT("/graph/ordered_unit_indices/length"), MakeShared<FJsonValueNumber>(Workspace.Units.Num()), MakeShared<FJsonValueNumber>(OrderedUnitIndices.Num()));
   TSet<int32> Seen;
   TArray<int32> Positions;
   Positions.Init(INDEX_NONE, Workspace.Units.Num());
   int32 Position = 0;
   for (int32 UnitIndex : OrderedUnitIndices) {
     if (!Workspace.Units.IsValidIndex(UnitIndex))
       return Reject(TEXT("The dependency order references a nonexistent workspace unit."), TEXT("/graph/ordered_unit_indices/") + FString::FromInt(Position), MakeShared<FJsonValueString>(FString::Printf(TEXT("index in [0, %d)"), Workspace.Units.Num())), MakeShared<FJsonValueNumber>(UnitIndex));
     if (Seen.Contains(UnitIndex))
       return Reject(TEXT("The dependency order repeats a workspace unit."), TEXT("/graph/ordered_unit_indices/") + FString::FromInt(Position), MakeShared<FJsonValueString>(TEXT("unique unit index")), MakeShared<FJsonValueNumber>(UnitIndex));
     Seen.Add(UnitIndex);
     Positions[UnitIndex] = Position++;
   }
   for (int32 Index = 0; Index < Edges.Num(); ++Index) {
     const auto& Edge = Edges[Index];
     const FString Path = TEXT("/graph/edges/") + FString::FromInt(Index);
     for (const auto& Field : {TPair<const TCHAR*, int32>(TEXT("before_unit_index"), Edge.BeforeUnitIndex),
         {TEXT("after_unit_index"), Edge.AfterUnitIndex}, {TEXT("declaration_unit_index"), Edge.DeclarationUnitIndex}})
       if (!Workspace.Units.IsValidIndex(Field.Value))
         return Reject(TEXT("A dependency edge references a nonexistent workspace unit."), Path / Field.Key, MakeShared<FJsonValueString>(FString::Printf(TEXT("index in [0, %d)"), Workspace.Units.Num())), MakeShared<FJsonValueNumber>(Field.Value));
     if (Positions[Edge.BeforeUnitIndex] >= Positions[Edge.AfterUnitIndex]) {
       TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
       Actual->SetNumberField(TEXT("before_unit_index"), Edge.BeforeUnitIndex);
       Actual->SetNumberField(TEXT("after_unit_index"), Edge.AfterUnitIndex);
       Actual->SetNumberField(TEXT("before_position"), Positions[Edge.BeforeUnitIndex]);
       Actual->SetNumberField(TEXT("after_position"), Positions[Edge.AfterUnitIndex]);
       return Reject(TEXT("The unit order violates a declared dependency edge."), Path, MakeShared<FJsonValueString>(TEXT("before_position < after_position")), MakeShared<FJsonValueObject>(Actual));
     }
   }
   if (!CycleUnitKeys.IsEmpty()) {
     TArray<TSharedPtr<FJsonValue>> Cycle;
     for (const auto& Key : CycleUnitKeys) Cycle.Add(MakeShared<FJsonValueString>(Key));
     return Reject(TEXT("The dependency graph still contains a cycle."), TEXT("/graph/cycle_unit_keys"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()), MakeShared<FJsonValueArray>(Cycle));
   }
   return true;
 }

	TSharedRef<FJsonObject> FDependencyGraph::ToJsonObject(
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), TEXT("uai.apply_workspace_dependency_graph.v1"));
		Object->SetStringField(TEXT("contract_id"), ContractId);
		Object->SetBoolField(TEXT("ready"), IsReadyFor(Workspace));

		TArray<TSharedPtr<FJsonValue>> OrderedValues;
		for (int32 UnitIndex : OrderedUnitIndices)
		{
			if (Workspace.Units.IsValidIndex(UnitIndex))
			{
				OrderedValues.Add(MakeShared<FJsonValueString>(Workspace.Units[UnitIndex].CanonicalUnitKey));
			}
		}
		Object->SetArrayField(TEXT("ordered_unit_keys"), OrderedValues);

		TArray<TSharedPtr<FJsonValue>> EdgeValues;
		for (const FResolvedEdge& Edge : Edges)
		{
			if (!Workspace.Units.IsValidIndex(Edge.BeforeUnitIndex)
				|| !Workspace.Units.IsValidIndex(Edge.AfterUnitIndex)) continue;
			TSharedRef<FJsonObject> EdgeObject = MakeShared<FJsonObject>();
			EdgeObject->SetStringField(TEXT("before"), Workspace.Units[Edge.BeforeUnitIndex].CanonicalUnitKey);
			EdgeObject->SetStringField(TEXT("after"), Workspace.Units[Edge.AfterUnitIndex].CanonicalUnitKey);
			if (Workspace.Units.IsValidIndex(Edge.DeclarationUnitIndex))
			{
				EdgeObject->SetStringField(TEXT("declared_by"), Workspace.Units[Edge.DeclarationUnitIndex].CanonicalUnitKey);
			}
			EdgeObject->SetStringField(TEXT("source"), DependencySourceToString(Edge.Source));
			EdgeObject->SetStringField(TEXT("source_json_pointer"), Edge.SourceJsonPointer);
			EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeObject));
		}
		Object->SetArrayField(TEXT("edges"), EdgeValues);
		return Object;
	}

	bool BuildDependencyGraph(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const FDependencyInputs& Inputs,
		FDependencyGraph& OutGraph,
		FGraphFailure& OutFailure)
	{
		OutGraph = FDependencyGraph();
		OutFailure = FGraphFailure();
        UE::UAI::Contract::FDiagnostic ContractError;
        if (!UeAgentApplyWorkspaceContract::ValidateContract(Contract, ContractError)) {
            SetFailure(OutFailure, MoveTemp(ContractError));
            return false;
        }
        if (Contract.ContractId != Workspace.ContractId || Contract.ContractVersion != Workspace.ContractVersion) {
            const bool bIdMismatch = Contract.ContractId != Workspace.ContractId;
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_preflight_failed"),
                bIdMismatch ? TEXT("/workspace/contract_id") : TEXT("/workspace/contract_version"),
                MakeShared<FJsonValueString>(bIdMismatch ? Contract.ContractId : Contract.ContractVersion),
                MakeShared<FJsonValueString>(bIdMismatch ? Workspace.ContractId : Workspace.ContractVersion),
                TEXT("workspace_dependency"), bIdMismatch ? TEXT("The routed workspace belongs to a different contract.") : TEXT("The routed workspace belongs to a different contract version."));
            Diagnostic.SourceKind = TEXT("native_result");
            SetFailure(OutFailure, MoveTemp(Diagnostic));
            return false;
        }

		OutGraph.ContractId = Contract.ContractId;
		TMap<FString, int32> CanonicalKeys;
		TMap<FString, TArray<int32>> Aliases;
		for (int32 UnitIndex = 0; UnitIndex < Workspace.Units.Num(); ++UnitIndex)
		{
			const FWorkspaceUnit& Unit = Workspace.Units[UnitIndex];
			const FString FoldedCanonicalKey = FoldKey(Unit.CanonicalUnitKey);
			if (FoldedCanonicalKey.IsEmpty() || CanonicalKeys.Contains(FoldedCanonicalKey))
			{
                auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_duplicate_target"), TEXT("canonical_unit_key"),
                    MakeShared<FJsonValueString>(FoldedCanonicalKey.IsEmpty() ? TEXT("nonempty unit identity") : TEXT("unique unit identity (case-insensitive)")), MakeShared<FJsonValueString>(Unit.CanonicalUnitKey),
                    TEXT("workspace_dependency"), FoldedCanonicalKey.IsEmpty() ? TEXT("A workspace unit has an empty target identity.") : TEXT("Two workspace units have the same target identity."));
                Diagnostic.CandidateName = Unit.UnitId;
                SetFailure(OutFailure, MoveTemp(Diagnostic), &Unit);
				return false;
			}
			CanonicalKeys.Add(FoldedCanonicalKey, UnitIndex);
			AddAlias(Aliases, Unit.CanonicalUnitKey, UnitIndex);
			AddAlias(Aliases, Unit.UnitId, UnitIndex);
			FString NormalizedPath;
			if (NormalizeWorkspaceRelativeReference(Unit.RelativePath, NormalizedPath))
			{
				AddAlias(Aliases, NormalizedPath, UnitIndex);
			}
		}

		auto ResolveTarget = [&](const FString& Reference, int32& OutUnitIndex, TArray<int32>& Matches)
		{
			OutUnitIndex = INDEX_NONE;
            Matches.Reset();
			if (const TArray<int32>* Direct = Aliases.Find(FoldKey(Reference))) Matches.Append(*Direct);
			FString NormalizedPath;
			if (NormalizeWorkspaceRelativeReference(Reference, NormalizedPath))
			{
				if (const TArray<int32>* PathMatches = Aliases.Find(FoldKey(NormalizedPath)))
				{
					for (int32 Match : *PathMatches) Matches.AddUnique(Match);
				}
			}
			if (Matches.Num() == 1) OutUnitIndex = Matches[0];
		};

		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(Workspace.Units.Num());
		TArray<int32> InDegree;
		InDegree.Init(0, Workspace.Units.Num());
		TSet<FString> EdgeKeys;

		auto AddDeclarations = [&](const TArray<FDependencyDeclaration>& Declarations, EDependencySource Source)
		{
			for (const FDependencyDeclaration& Declaration : Declarations)
			{
				const int32* SourceIndexPtr = CanonicalKeys.Find(FoldKey(Declaration.SourceUnitKey));
				if (!SourceIndexPtr)
				{
					if (!Declaration.bRequired) continue;
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_reference_unresolved"), TEXT("A required dependency source is missing from the workspace."), TEXT("workspace_dependency"));
                    Diagnostic.CandidateName = Declaration.SourceUnitKey;
                    Diagnostic.PropertyPath = TEXT("source_unit_key");
                    Diagnostic.ExpectedValuePresent = true;
                    Diagnostic.ActualValuePresent = false;
                    SetFailure(OutFailure, MoveTemp(Diagnostic), nullptr, Declaration.SourceJsonPointer);
					return false;
				}
				const int32 SourceIndex = *SourceIndexPtr;
				const FWorkspaceUnit& SourceUnit = Workspace.Units[SourceIndex];
				if (SourceUnit.Presence == EUnitPresence::Absent && SourceUnit.Action == EUnitAction::NoChange)
				{
					continue;
				}

				int32 TargetIndex = INDEX_NONE;
                TArray<int32> Matches;
                ResolveTarget(Declaration.TargetReference, TargetIndex, Matches);
				if (TargetIndex == INDEX_NONE)
				{
					if (!Declaration.bRequired) continue;
                    const bool bAmbiguous = Matches.Num() > 1;
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(bAmbiguous ? TEXT("workspace_reference_ambiguous") : TEXT("workspace_reference_unresolved"),
                        bAmbiguous ? TEXT("The required workspace reference matches more than one unit.") : TEXT("The required workspace reference matches no unit."), TEXT("workspace_dependency"));
                    Diagnostic.CandidateName = Declaration.TargetReference;
                    Diagnostic.PropertyPath = Declaration.SourceJsonPointer;
                    Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
                    Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
                    if (bAmbiguous) {
                        TArray<TSharedPtr<FJsonValue>> Choices;
                        for (int32 Match : Matches) {
                            const auto& Candidate = Workspace.Units[Match];
                            const auto Choice = MakeShared<FJsonObject>();
                            Choice->SetStringField(TEXT("unit_key"), Candidate.CanonicalUnitKey);
                            Choice->SetStringField(TEXT("relative_path"), Candidate.RelativePath);
                            Choices.Add(MakeShared<FJsonValueObject>(Choice));
                        }
                        Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(Choices);
                    }
                    SetFailure(OutFailure, MoveTemp(Diagnostic), &SourceUnit, Declaration.SourceJsonPointer);
					return false;
				}
				const FWorkspaceUnit& TargetUnit = Workspace.Units[TargetIndex];
				if (TargetUnit.Presence == EUnitPresence::Absent && TargetUnit.Action == EUnitAction::NoChange)
				{
					if (!Declaration.bRequired) continue;
                    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_reference_unresolved"), TEXT("The required dependency target is declared but has no input content."), TEXT("workspace_dependency"));
                    Diagnostic.CandidateName = Declaration.TargetReference;
                    Diagnostic.PropertyPath = Declaration.SourceJsonPointer;
                    Diagnostic.ExpectedValuePresent = true;
                    Diagnostic.ActualValuePresent = false;
                    SetFailure(OutFailure, MoveTemp(Diagnostic), &SourceUnit, Declaration.SourceJsonPointer);
					return false;
				}

				const int32 BeforeIndex = Declaration.Order == EOrderConstraint::TargetBeforeSource
					? TargetIndex
					: SourceIndex;
				const int32 AfterIndex = Declaration.Order == EOrderConstraint::TargetBeforeSource
					? SourceIndex
					: TargetIndex;
				const FString EdgeKey = FString::Printf(TEXT("%d>%d"), BeforeIndex, AfterIndex);
				if (EdgeKeys.Contains(EdgeKey)) continue;
				EdgeKeys.Add(EdgeKey);
				Adjacency[BeforeIndex].Add(AfterIndex);
				++InDegree[AfterIndex];
				FResolvedEdge& Edge = OutGraph.Edges.AddDefaulted_GetRef();
				Edge.BeforeUnitIndex = BeforeIndex;
				Edge.AfterUnitIndex = AfterIndex;
				Edge.DeclarationUnitIndex = SourceIndex;
				Edge.Source = Source;
				Edge.SourceJsonPointer = Declaration.SourceJsonPointer;
			}
			return true;
		};

		if (!AddDeclarations(Inputs.ContractDependencies, EDependencySource::Contract)
			|| !AddDeclarations(Inputs.ReferenceDependencies, EDependencySource::Reference))
		{
			OutGraph = FDependencyGraph();
			return false;
		}

		OutGraph.Edges.Sort([&](const FResolvedEdge& Left, const FResolvedEdge& Right)
		{
			const int32 BeforeCompare = CompareUnitKeys(Workspace, Left.BeforeUnitIndex, Right.BeforeUnitIndex);
			if (BeforeCompare != 0) return BeforeCompare < 0;
			const int32 AfterCompare = CompareUnitKeys(Workspace, Left.AfterUnitIndex, Right.AfterUnitIndex);
			if (AfterCompare != 0) return AfterCompare < 0;
			return static_cast<uint8>(Left.Source) < static_cast<uint8>(Right.Source);
		});

		TArray<int32> Ready;
		for (int32 UnitIndex = 0; UnitIndex < InDegree.Num(); ++UnitIndex)
		{
			if (InDegree[UnitIndex] == 0) Ready.Add(UnitIndex);
		}
		while (!Ready.IsEmpty())
		{
			Ready.Sort([&](int32 Left, int32 Right)
			{
				return CompareUnitKeys(Workspace, Left, Right) < 0;
			});
			const int32 UnitIndex = Ready[0];
			Ready.RemoveAt(0, 1, EAllowShrinking::No);
			OutGraph.OrderedUnitIndices.Add(UnitIndex);

			TArray<int32> Next = Adjacency[UnitIndex];
			Next.Sort([&](int32 Left, int32 Right)
			{
				return CompareUnitKeys(Workspace, Left, Right) < 0;
			});
			for (int32 NextIndex : Next)
			{
				if (--InDegree[NextIndex] == 0) Ready.Add(NextIndex);
			}
		}

		if (OutGraph.OrderedUnitIndices.Num() != Workspace.Units.Num())
		{
			TArray<int32> Remaining;
			for (int32 UnitIndex = 0; UnitIndex < InDegree.Num(); ++UnitIndex)
			{
				if (InDegree[UnitIndex] > 0) Remaining.Add(UnitIndex);
			}
			FindDeterministicCycle(Workspace, Adjacency, Remaining, OutGraph.CycleUnitKeys);
			const FWorkspaceUnit* SourceUnit = nullptr;
			FString JsonPointer = TEXT("/");
			if (OutGraph.CycleUnitKeys.Num() >= 2)
			{
				const FString& BeforeKey = OutGraph.CycleUnitKeys[OutGraph.CycleUnitKeys.Num() - 2];
				const FString& AfterKey = OutGraph.CycleUnitKeys.Last();
				for (const FResolvedEdge& Edge : OutGraph.Edges)
				{
					if (Workspace.Units[Edge.BeforeUnitIndex].CanonicalUnitKey == BeforeKey
						&& Workspace.Units[Edge.AfterUnitIndex].CanonicalUnitKey == AfterKey)
					{
						SourceUnit = &Workspace.Units[Edge.DeclarationUnitIndex];
						JsonPointer = Edge.SourceJsonPointer;
						break;
					}
				}
			}
			SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::Failure(TEXT("workspace_dependency_cycle"), TEXT("The workspace dependency declarations form a cycle."), TEXT("workspace_dependency"), FString()) ,SourceUnit, JsonPointer);
            OutFailure.Diagnostic.CyclePath = OutGraph.CycleUnitKeys;
            TArray<TSharedPtr<FJsonValue>> Cycle;
            for (const auto& Key : OutGraph.CycleUnitKeys) Cycle.Add(MakeShared<FJsonValueString>(Key));
            OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("acyclic dependencies"));
            OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueArray>(Cycle);
			OutGraph.OrderedUnitIndices.Reset();
			return false;
		}
		return true;
	}
}
