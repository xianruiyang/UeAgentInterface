#include "Utilities/AuthoringCore/UeAgentAuthoringAIGraphOwnership.h"

#include "AIGraphNode.h"
#include "Containers/Array.h"
#include "Containers/Set.h"
#include "EdGraph/EdGraph.h"

namespace UeAgentAuthoringAIGraphOwnership
{
	FCapture Capture(const UEdGraphNode& Node)
	{
		FCapture Result;
		const UAIGraphNode* Attachment = Cast<UAIGraphNode>(&Node);
		const UEdGraph* Graph = Node.GetGraph();
		if (!Attachment || !Graph) return Result;

		TArray<const UEdGraphNode*> Pending;
		for (const UEdGraphNode* Candidate : Graph->Nodes) Pending.Add(Candidate);
		TSet<const UAIGraphNode*> Visited;
		while (!Pending.IsEmpty())
		{
			const UAIGraphNode* Owner = Cast<UAIGraphNode>(Pending.Pop(EAllowShrinking::No));
			if (!Owner || Visited.Contains(Owner)) continue;
			Visited.Add(Owner);
			const int32 Index = Owner->SubNodes.IndexOfByKey(Attachment);
			if (Index != INDEX_NONE)
			{
				if (Result.Parent)
				{
					Result.ConflictingParent = Owner;
					return Result;
				}
				Result.Parent = Owner;
				Result.SubnodeIndex = Index;
			}
			for (const UAIGraphNode* Child : Owner->SubNodes) Pending.Add(Child);
		}
		return Result;
	}
}
