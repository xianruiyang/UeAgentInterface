#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class UWidgetBlueprint;
namespace UE::UAI::ContentDiscovery { class FReadOnlyCandidateBatch; }

namespace UE::UAI::QueryOwners::UmgWidget
{
	struct UEAGENTINTERFACE_API FUmgWidgetCandidate
	{
		/** Server-side identity retained for internal re-resolution only. */
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString ClassPath;
		FString SourceOrigin;
		TMap<FString, FString> SelectionFields;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		bool bSupported = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FUmgWidgetCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
	};

	using FUmgWidgetCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryUmgWidgetCandidates(
		UWidgetBlueprint& Target,
		const FString& Query,
		FUmgWidgetCandidateQueryResult& OutResult,
		FUmgWidgetCandidateQueryError& OutError);

	/** Re-enumerate the same target-bound partial catalog and resolve one selector. */
	/** Read-only selection includes native actions outside HJSON creation coverage. */
	UEAGENTINTERFACE_API bool TrySelectUmgWidgetCandidate(
		UWidgetBlueprint& Target,
		const FString& Name,
		FUmgWidgetCandidate& OutCandidate,
		FUmgWidgetCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {},
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr);

	UEAGENTINTERFACE_API bool TryResolveUmgWidgetCandidate(
		UWidgetBlueprint& Target,
		const FString& Name,
		FUmgWidgetCandidate& OutCandidate,
		FUmgWidgetCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {},
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr);

	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}
