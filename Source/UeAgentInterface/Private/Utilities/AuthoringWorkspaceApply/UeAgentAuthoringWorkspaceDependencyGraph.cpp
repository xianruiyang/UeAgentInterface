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

	UeAgentAuthoringDiagnostics::FSourceLocation MakeSourceLocation(
		const FWorkspaceUnit* Unit,
		const FString& JsonPointer)
	{
		UeAgentAuthoringDiagnostics::FSourceLocation Source;
		if (!Unit) return Source;
		Source = Unit->Provenance.ToDiagnosticSource();
		Source.JsonPath = JsonPointer.IsEmpty() ? TEXT("/") : JsonPointer;
		if (const FUeAgentHjsonSourceLocation* Location = Unit->FindSourceLocation(Source.JsonPath))
		{
			Source.SourceFile = Location->FilePath;
			Source.Line = Location->Line;
			Source.Column = Location->Column;
		}
		return Source;
	}

	void SetFailure(
		FGraphFailure& OutFailure,
		const FString& ErrorCode,
		const FString& ErrorDetail,
		const FWorkspaceUnit* SourceUnit = nullptr,
		const FString& JsonPointer = TEXT("/"))
	{
		OutFailure.ErrorCode = ErrorCode;
		OutFailure.ErrorDetail = ErrorDetail;
		OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic();
		OutFailure.Diagnostic.Code = ErrorCode;
		OutFailure.Diagnostic.Stage = TEXT("workspace_dependency");
		OutFailure.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		OutFailure.Diagnostic.bRetrySafe = true;
		OutFailure.Diagnostic.Message = ErrorDetail;
		OutFailure.Diagnostic.SuggestedFix = TEXT("Fix the workspace reference or dependency declaration and retry the complete workspace.");
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

	bool FDependencyGraph::IsReadyFor(const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace) const
	{
		if (ContractId != Workspace.ContractId || OrderedUnitIndices.Num() != Workspace.Units.Num()) return false;
		TSet<int32> Seen;
		TArray<int32> Positions;
		Positions.Init(INDEX_NONE, Workspace.Units.Num());
		int32 Position = 0;
		for (int32 UnitIndex : OrderedUnitIndices)
		{
			if (!Workspace.Units.IsValidIndex(UnitIndex) || Seen.Contains(UnitIndex)) return false;
			Seen.Add(UnitIndex);
			Positions[UnitIndex] = Position++;
		}
		for (const FResolvedEdge& Edge : Edges)
		{
			if (!Workspace.Units.IsValidIndex(Edge.BeforeUnitIndex)
				|| !Workspace.Units.IsValidIndex(Edge.AfterUnitIndex)
				|| !Workspace.Units.IsValidIndex(Edge.DeclarationUnitIndex)
				|| Positions[Edge.BeforeUnitIndex] >= Positions[Edge.AfterUnitIndex]) return false;
		}
		return CycleUnitKeys.IsEmpty();
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
		if (!Contract.IsValid()
			|| Contract.ContractId != Workspace.ContractId
			|| Contract.ContractVersion != Workspace.ContractVersion)
		{
			SetFailure(OutFailure, TEXT("workspace_preflight_failed"), TEXT("workspace_contract_mismatch"));
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
				SetFailure(OutFailure, TEXT("workspace_duplicate_target"), Unit.CanonicalUnitKey, &Unit);
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

		auto ResolveTarget = [&](const FString& Reference, int32& OutUnitIndex, bool& bOutAmbiguous)
		{
			OutUnitIndex = INDEX_NONE;
			bOutAmbiguous = false;
			TArray<int32> Matches;
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
			else if (Matches.Num() > 1) bOutAmbiguous = true;
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
					SetFailure(
						OutFailure,
						TEXT("workspace_reference_unresolved"),
						TEXT("source:") + Declaration.SourceUnitKey,
						nullptr,
						Declaration.SourceJsonPointer);
					return false;
				}
				const int32 SourceIndex = *SourceIndexPtr;
				const FWorkspaceUnit& SourceUnit = Workspace.Units[SourceIndex];
				if (SourceUnit.Presence == EUnitPresence::Absent && SourceUnit.Action == EUnitAction::NoChange)
				{
					continue;
				}

				int32 TargetIndex = INDEX_NONE;
				bool bAmbiguous = false;
				ResolveTarget(Declaration.TargetReference, TargetIndex, bAmbiguous);
				if (TargetIndex == INDEX_NONE)
				{
					if (!Declaration.bRequired) continue;
					SetFailure(
						OutFailure,
						bAmbiguous ? TEXT("workspace_reference_ambiguous") : TEXT("workspace_reference_unresolved"),
						Declaration.TargetReference,
						&SourceUnit,
						Declaration.SourceJsonPointer);
					return false;
				}
				const FWorkspaceUnit& TargetUnit = Workspace.Units[TargetIndex];
				if (TargetUnit.Presence == EUnitPresence::Absent && TargetUnit.Action == EUnitAction::NoChange)
				{
					if (!Declaration.bRequired) continue;
					SetFailure(
						OutFailure,
						TEXT("workspace_reference_unresolved"),
						Declaration.TargetReference + TEXT(":absent"),
						&SourceUnit,
						Declaration.SourceJsonPointer);
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
			const FString Detail = OutGraph.CycleUnitKeys.IsEmpty()
				? TEXT("cycle")
				: FString::Join(OutGraph.CycleUnitKeys, TEXT(" -> "));
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
			SetFailure(OutFailure, TEXT("workspace_dependency_cycle"), Detail, SourceUnit, JsonPointer);
			OutGraph.OrderedUnitIndices.Reset();
			return false;
		}
		return true;
	}
}
