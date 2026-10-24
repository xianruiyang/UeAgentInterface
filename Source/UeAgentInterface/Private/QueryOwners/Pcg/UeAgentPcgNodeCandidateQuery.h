#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::ContentDiscovery { class FReadOnlyCandidateBatch; }
namespace UE::UAI::CandidateProperties { struct FRequest; }
class FJsonObject;

class UObject;
class UPCGGraph;
class UPCGNode;

namespace UE::UAI::QueryOwners::Pcg
{
	using FPcgNodeCandidateQueryError = UE::UAI::Contract::FDiagnostic;
	UEAGENTINTERFACE_API bool TryDescribePcgNodeCandidate(UPCGGraph& Target,
		const CandidateProperties::FRequest& Request, TSharedPtr<FJsonObject>& OutDescription,
		FPcgNodeCandidateQueryError& OutError);
	struct UEAGENTINTERFACE_API FPcgNodeCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		TMap<FString, FString> SelectionFields;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		bool bSupported = false;
	};

	struct UEAGENTINTERFACE_API FPcgNodeCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FPcgNodeCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
	};

	

	UEAGENTINTERFACE_API bool TryQueryPcgNodeCandidates(
		UObject& Target,
		const FString& Query,
		FPcgNodeCandidateQueryResult& OutResult,
		FPcgNodeCandidateQueryError& OutError,
		const FString& CandidateId = FString());

	/** Re-enumerate the exact native PCG graph context and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolvePcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {},
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr);

	/** Re-enumerate and invoke the selected native PCG schema action. */
	UEAGENTINTERFACE_API bool TryPerformPcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UPCGNode*& OutNode,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});
}
