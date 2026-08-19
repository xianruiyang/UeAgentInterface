#pragma once

#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::StateSemantic
{
	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalValuePtr& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalIdentity& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalType& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentDefaultValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateDetails& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateRevision& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentTemporalPoint& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialFrame& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateEntry& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentAuthoredPresence Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentOverrideState Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentStatePlane Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const TArray<FUeAgentCanonicalValueRef>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError);
}
