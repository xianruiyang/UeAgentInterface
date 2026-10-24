#pragma once

#include "CoreTypes.h"

class UAIGraphNode;
class UEdGraphNode;

namespace UeAgentAuthoringAIGraphOwnership
{
	struct FCapture
	{
		const UAIGraphNode* Parent = nullptr;
		const UAIGraphNode* ConflictingParent = nullptr;
		int32 SubnodeIndex = INDEX_NONE;
	};

	// ParentNode is transient and may survive removal. Read persisted SubNodes instead.
	// Domains retain their node identities, membership rules and diagnostic vocabulary.
	FCapture Capture(const UEdGraphNode& Node);
}
