#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class UObject;
class UBehaviorTree;
class UBehaviorTreeGraphNode;
class UAIGraphNode;
class UEdGraph;
class UEdGraphNode;

namespace UE::UAI::QueryOwners::AiBehavior
{
	/** Native menu context, resolved by the caller from the shared graph identity. */
	struct FAiBehaviorCandidateContext
	{
		UAIGraphNode* Parent = nullptr;
		FString AttachmentKind;
		UEdGraph* Graph = nullptr;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidate
	{
		/** Server-side identity authority; not projected into the public result. */
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		TArray<FString> Scopes;
		TMap<FString, FString> SelectionFields;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		FString PayloadType;
		FString GraphNodeType;
		FString LogicMode;
		int32 DefaultInputCount = 0;
		bool bSupported = false;
		TArray<FString> SupportedOperations;

		// Legacy compatibility fields for AI contexts not yet migrated to a
		// native creation surface. New BehaviorTree candidates do not require
		// clients to copy these fields.
		FString InputContract;
		FString PlacementContract;
		FString RequiredContext;
		FString BindingRevision;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAiBehaviorCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
		FString GraphName;
	};

	using FAiBehaviorCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryAiBehaviorCandidates(
		UObject& Target,
		const FString& Query,
		FAiBehaviorCandidateQueryResult& OutResult,
		FAiBehaviorCandidateQueryError& OutError,
		const FString& CandidateId = FString(),
		const FAiBehaviorCandidateContext& Context = {});

	/** Re-enumerate the target's native AI candidate catalog. */
	UEAGENTINTERFACE_API bool TryResolveAiBehaviorCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {},
		const FAiBehaviorCandidateContext& Context = {});

	/** Invoke the selected native AI action for a supported graph asset. */
	UEAGENTINTERFACE_API bool TryPerformAiGraphCandidate(
		UObject& Target, const FString& Name, const FString& Id, const FVector2f& Location,
		UEdGraphNode*& OutNode, FAiBehaviorCandidate& OutCandidate, FAiBehaviorCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {}, const FAiBehaviorCandidateContext& Context = {});

}
