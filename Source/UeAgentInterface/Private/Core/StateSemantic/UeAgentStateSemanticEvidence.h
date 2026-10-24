#pragma once

#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::StateSemantic
{
	// Materialize facts already selected by the comparison producer; never reclassify JSON keys.
	UEAGENTINTERFACE_API Contract::FDiagnostic MakeComparisonDiagnostic(
		const FUeAgentSemanticComparison& Comparison, const FString& Code,
		const FString& Reason, const FString& Stage,
		const FString& Schema = TEXT("uai.authoring.diagnostic/1"));

	enum class EDifferenceEvidenceAudience : uint8 { Internal, Public };
	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalValuePtr& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalIdentity& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentCanonicalType& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentDefaultValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateDetails& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateRevision& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentTemporalPoint& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialFrame& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentSpatialValue& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const FUeAgentStateEntry& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentAuthoredPresence Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentOverrideState Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		EUeAgentStatePlane Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const TArray<FUeAgentCanonicalValueRef>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);

	UEAGENTINTERFACE_API bool TryCanonicalizeDifferenceEvidence(
		const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Source,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError,
		EDifferenceEvidenceAudience Audience = EDifferenceEvidenceAudience::Internal);
}
