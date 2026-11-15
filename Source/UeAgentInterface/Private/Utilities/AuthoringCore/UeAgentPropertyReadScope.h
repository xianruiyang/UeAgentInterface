// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UeAgentAuthoringState
{
	/** Release optional immutable request templates on the owning execution thread. */
	void ResetPropertyReadTemplates();
struct FPropertyReadCandidateState;

// Only for one synchronous, read-only request construction scope. Candidate
// metadata may be shared; instance editability and inclusion are always observed.
class FScopedPropertyReadCandidates final
{
public:
	FScopedPropertyReadCandidates();
	~FScopedPropertyReadCandidates();
	FScopedPropertyReadCandidates(const FScopedPropertyReadCandidates&) = delete;
	FScopedPropertyReadCandidates& operator=(const FScopedPropertyReadCandidates&) = delete;
private:
	TUniquePtr<FPropertyReadCandidateState> State;
};
}
