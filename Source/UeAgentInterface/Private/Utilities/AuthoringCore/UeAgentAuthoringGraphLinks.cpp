#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectHash.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentAuthoringGraphLinks
{
    void FIntent::Observe() const
    {
        FDiagnostic Error;
        if (!Verify(Error))
            UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("graph_connection_intent_not_retained"),
                Error, TEXT("graph_connection_observation"));
    }

    namespace
    {
        bool Fail(const FString& Pointer, FDiagnostic& Error, const TCHAR* Reason, const FString& Field = FString())
        {
            Error = FDiagnostic::Failure(TEXT("graph_connection_intent_not_retained"), Reason, TEXT("graph_connection_verification"));
            Error.PropertyPath = Field;
            UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Pointer);
            return false;
        }
        bool LinkFailure(const FString& Pointer, const UEdGraphPin* From, const UEdGraphPin* To,
            bool bExpectedConnected, FDiagnostic& Error)
        {
            Fail(Pointer, Error, bExpectedConnected
                ? TEXT("The requested connection is missing or is not present in both pin link lists.")
                : TEXT("The requested disconnection still appears in a pin link list."));
            const auto Actual = MakeShared<FJsonObject>();
            Actual->SetBoolField(TEXT("from_present"), From != nullptr);
            Actual->SetBoolField(TEXT("to_present"), To != nullptr);
            Actual->SetBoolField(TEXT("from_to"), From && To && From->LinkedTo.Contains(To));
            Actual->SetBoolField(TEXT("to_from"), From && To && To->LinkedTo.Contains(From));
            Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
            Error.ExpectedValue = MakeShared<FJsonValueBoolean>(bExpectedConnected);
            if (From) Error.CandidateName = From->PinName.ToString();
            else if (To) Error.CandidateName = To->PinName.ToString();
            return false;
        }

        bool Connected(const UEdGraphPin* From, const UEdGraphPin* To)
        {
            return From && To && From->LinkedTo.Contains(To) && To->LinkedTo.Contains(From);
        }
    }

    FIntent::FEndpoint::FEndpoint(UEdGraphPin* InPin)
        : Pin(InPin), PendingNode(InPin ? InPin->GetOwningNode() : nullptr)
    {
        if (!InPin) return;
        UEdGraphNode* Node = InPin->GetOwningNode();
        if (const UEdGraph* Graph = Node->GetGraph())
        {
            GraphPath = Graph->GetPathName();
            GraphId = Graph->GraphGuid;
        }
        NodeId = Node->NodeGuid;
        PinId = InPin->PinId;
        PinName = InPin->PinName;
        Direction = InPin->Direction;
        for (const UEdGraphPin* Candidate : Node->Pins)
        {
            if (Candidate == InPin) break;
            if (Candidate && Candidate->PinName == PinName && Candidate->Direction == Direction) ++Occurrence;
        }
    }

    bool FIntent::FEndpoint::Freeze(UObject& Owner, bool bAllowRemoved, FDiagnostic& Error)
    {
        if (bFrozen) return true;
        auto Reject = [&](const TCHAR* Reason, const TCHAR* Field) {
            Fail(FString(), Error, Reason, Field);
            Error.CandidateName = PinName.ToString();
            const auto Evidence = MakeShared<FJsonObject>();
            Evidence->SetStringField(TEXT("graph_path"), GraphPath);
            Evidence->SetStringField(TEXT("graph_id"), GraphId.ToString());
            Evidence->SetStringField(TEXT("node_id"), NodeId.ToString());
            Evidence->SetStringField(TEXT("pin_id"), PinId.ToString());
            Error.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
            return false;
        };
        UEdGraphNode* Node = PendingNode.Get();
        if (Node && (!Node->GetGraph() || (Node->GetGraph() != &Owner && !Node->GetGraph()->IsIn(&Owner))))
        {
            if (!bAllowRemoved) return Reject(TEXT("A connected endpoint no longer belongs to the requested graph owner."), TEXT("graph"));
            Node = nullptr;
        }
        if (Node)
        {
            UEdGraph* Graph = Node->GetGraph();
            if (!Graph || (Graph != &Owner && !Graph->IsIn(&Owner))) return Reject(TEXT("The endpoint graph is missing or belongs to a different owner."), TEXT("graph"));
            GraphPath = Graph == &Owner ? FString() : Graph->GetPathName(&Owner);
            GraphId = Graph->GraphGuid;
            NodeId = Node->NodeGuid;
            const UEdGraphPin* Current = Pin.Get();
            // Blueprint creation replay assigns the proposal's pin identities
            // after applying the graph. Resolve that binding once, while the
            // same native node is still available; late readback uses IDs only.
            if (!Current)
            {
                int32 Index = 0;
                for (const UEdGraphPin* Candidate : Node->Pins)
                    if (Candidate && Candidate->PinName == PinName && Candidate->Direction == Direction
                        && Index++ == Occurrence) { Current = Candidate; break; }
            }
            if (Current) PinId = Current->PinId;
        }
        else
        {
            const FString Prefix = Owner.GetPathName();
            if (!GraphPath.StartsWith(Prefix + TEXT(".")) && !GraphPath.StartsWith(Prefix + TEXT(":"))) return Reject(TEXT("The removed endpoint was not inside the requested graph owner."), TEXT("graph"));
            GraphPath.RightChopInline(Prefix.Len() + 1);
        }
        Pin = FEdGraphPinReference();
        PendingNode.Reset();
        bFrozen = NodeId.IsValid() && PinId.IsValid();
        if (!bFrozen) return Reject(!NodeId.IsValid() ? TEXT("The endpoint has no valid node identity.") : TEXT("The endpoint has no valid pin identity."), !NodeId.IsValid() ? TEXT("node") : TEXT("pin"));
        return true;
    }

    void FIntent::Expect(UEdGraphPin* From, UEdGraphPin* To, bool bConnected, FString SourcePointer)
    {
        Links.Add({FEndpoint(From), FEndpoint(To), bConnected, MoveTemp(SourcePointer)});
        bFrozen = false;
    }

    bool FIntent::ExpectConnected(UEdGraphPin* From, UEdGraphPin* To,
        const TSet<UEdGraphNode*>& ConversionNodes, const FString& SourcePointer, FDiagnostic& Error)
    {
        if (Connected(From, To))
        {
            Expect(From, To, true, SourcePointer);
            return true;
        }
        TSet<UEdGraphPin*> Visited;
        TArray<TPair<UEdGraphPin*, UEdGraphPin*>> Path;
        TFunction<bool(UEdGraphPin*)> FindPath = [&](UEdGraphPin* Output)
        {
            if (!Output || Output->Direction != EGPD_Output || Visited.Contains(Output)) return false;
            Visited.Add(Output);
            for (UEdGraphPin* Input : Output->LinkedTo)
            {
                if (!Connected(Output, Input)) continue;
                Path.Emplace(Output, Input);
                if (Input == To) return true;
                UEdGraphNode* Node = Input->GetOwningNode();
                if (ConversionNodes.Contains(Node))
                    for (UEdGraphPin* Next : Node->Pins)
                        if (FindPath(Next)) return true;
                Path.Pop(EAllowShrinking::No);
            }
            return false;
        };
        if (!From || !To) return LinkFailure(SourcePointer, From, To, true, Error);
        if (ConversionNodes.IsEmpty() || !FindPath(From)) {
            LinkFailure(SourcePointer, From, To, true, Error);
            Error.PublicMessage = ConversionNodes.IsEmpty()
                ? TEXT("The requested pins are not connected and no conversion nodes were created.")
                : TEXT("The conversion nodes created for this request do not form a complete connection path.");
            return false;
        }
        for (const auto& Link : Path) Expect(Link.Key, Link.Value, true, SourcePointer);
        return true;
    }

    bool FIntent::Verify(FDiagnostic& Error) const
    {
        if (bFrozen && !Links.IsEmpty()) return Fail(Links[0].SourcePointer, Error, TEXT("A frozen graph intent requires verification against its owner."), TEXT("verification_owner"));
        for (const FLink& Link : Links)
        {
            const UEdGraphPin* From = Link.From.Pin.Get();
            const UEdGraphPin* To = Link.To.Pin.Get();
            const bool bValid = Link.bConnected ? Connected(From, To)
                : !From || !To || (!From->LinkedTo.Contains(To) && !To->LinkedTo.Contains(From));
            if (!bValid) return LinkFailure(Link.SourcePointer, From, To, Link.bConnected, Error);
        }
        return true;
    }

    bool FIntent::Freeze(UObject& Owner, FDiagnostic& Error)
    {
        if (!bFrozen)
        {
            for (FLink& Link : Links)
            {
                const bool bFromValid = Link.From.Freeze(Owner, !Link.bConnected, Error);
                if (!bFromValid || !Link.To.Freeze(Owner, !Link.bConnected, Error)) {
                    Error.PropertyPath = (bFromValid ? TEXT("to.") : TEXT("from.")) + Error.PropertyPath;
                    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Link.SourcePointer);
                    return false;
                }
            }
            bFrozen = true;
        }
        return true;
    }

    bool FIntent::Verify(UObject& Owner, FDiagnostic& Error) const
    {
        TArray<UObject*> Objects;
        GetObjectsWithOuter(&Owner, Objects, true);
        if (Owner.IsA<UEdGraph>()) Objects.Add(&Owner);
        TArray<UEdGraph*> Graphs;
        for (UObject* Object : Objects) if (auto* Graph = Cast<UEdGraph>(Object)) Graphs.Add(Graph);
        return Verify(Owner, Graphs, Error);
    }

    bool FIntent::Verify(UObject& Owner, TConstArrayView<UEdGraph*> Graphs, FDiagnostic& Error) const
    {
        if (Links.IsEmpty()) return true;
        if (!bFrozen) return Fail(Links[0].SourcePointer, Error, TEXT("The graph intent identities were not frozen before owner verification."), TEXT("frozen"));
        // The native owner supplies its active graphs. Retired/scratch objects
        // sharing the same outer are not the authored topology. Index once so
        // a large edge batch does not repeatedly scan every node.
        TMap<FGuid, UEdGraph*> ById;
        TMap<FString, UEdGraph*> ByPath;
        TMap<UEdGraph*, TMap<FGuid, UEdGraphNode*>> Nodes;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph || Nodes.Contains(Graph)) continue;
            if (Graph->GraphGuid.IsValid())
                ById.Add(Graph->GraphGuid, ById.Contains(Graph->GraphGuid) ? nullptr : Graph);
            const FString Path = Graph == &Owner ? FString() : Graph->GetPathName(&Owner);
            ByPath.Add(Path, ByPath.Contains(Path) ? nullptr : Graph);
            auto& Index = Nodes.FindOrAdd(Graph);
            for (UEdGraphNode* Node : Graph->Nodes)
                if (Node)
                {
                    if (UEdGraphNode** Existing = Index.Find(Node->NodeGuid))
                    {
                        if (*Existing != Node) *Existing = nullptr;
                    }
                    else Index.Add(Node->NodeGuid, Node);
                }
        }
        auto Resolve = [&](const FEndpoint& End, const UEdGraphPin*& OutPin, FDiagnostic& Detail)
        {
            OutPin = nullptr;
            auto Explain = [&](const TCHAR* Reason, const TCHAR* Field) {
                Fail(FString(), Detail, Reason, Field); Detail.CandidateName = End.PinName.ToString();
                const auto Evidence = MakeShared<FJsonObject>();
                Evidence->SetStringField(TEXT("graph_path"), End.GraphPath);
                Evidence->SetStringField(TEXT("graph_id"), End.GraphId.ToString());
                Evidence->SetStringField(TEXT("node_id"), End.NodeId.ToString());
                Evidence->SetStringField(TEXT("pin_id"), End.PinId.ToString());
                Detail.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
            };
            UEdGraph* const* Graph = End.GraphId.IsValid() ? ById.Find(End.GraphId) : ByPath.Find(End.GraphPath);
            if (!Graph) { Explain(TEXT("The connected endpoint graph no longer exists."), TEXT("graph")); return true; }
            if (!*Graph) { Explain(TEXT("More than one graph matches the endpoint identity."), TEXT("graph")); return false; }
            if (*Graph != &Owner && !(*Graph)->IsIn(&Owner)) { Explain(TEXT("The endpoint graph belongs to a different owner."), TEXT("graph")); return false; }
            UEdGraphNode* const* Node = Nodes.FindChecked(*Graph).Find(End.NodeId);
            if (!Node) { Explain(TEXT("The connected endpoint node no longer exists."), TEXT("node")); return true; }
            if (!*Node) { Explain(TEXT("More than one node matches the endpoint identity."), TEXT("node")); return false; }
            OutPin = (*Node)->FindPinById(End.PinId);
            if (!OutPin) Explain(TEXT("The connected endpoint pin no longer exists."), TEXT("pin"));
            return true;
        };
        for (const FLink& Link : Links)
        {
            const UEdGraphPin* From = nullptr;
            const UEdGraphPin* To = nullptr;
            FDiagnostic FromDetail, ToDetail;
            const bool bFromResolved = Resolve(Link.From, From, FromDetail);
            const bool bToResolved = Resolve(Link.To, To, ToDetail);
            if (!bFromResolved || !bToResolved || (Link.bConnected && (!From || !To))) {
                const bool bFromFailure = !bFromResolved || (bToResolved && !From);
                Error = bFromFailure ? FromDetail : ToDetail;
                Error.PropertyPath = (bFromFailure ? TEXT("from.") : TEXT("to.")) + Error.PropertyPath;
                UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, Link.SourcePointer);
                return false;
            }
            const bool bValid = Link.bConnected ? Connected(From, To)
                : !From || !To || (!From->LinkedTo.Contains(To) && !To->LinkedTo.Contains(From));
            if (!bValid) return LinkFailure(Link.SourcePointer, From, To, Link.bConnected, Error);
        }
        return true;
    }
}
