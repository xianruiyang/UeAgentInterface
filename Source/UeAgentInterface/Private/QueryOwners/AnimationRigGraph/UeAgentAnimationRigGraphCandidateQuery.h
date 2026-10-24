#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::ContentDiscovery { class FReadOnlyCandidateBatch; }

class UObject;
class UControlRigBlueprint;
class URigVMNode;
class URigVMUnitNode;

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		TMap<FString, FString> SelectionFields;
		bool bSupported = false;
		TArray<FString> SupportedOperations;
	};

	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAnimationRigGraphCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
		FString GraphName;
	};

	using FAnimationRigGraphCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryAnimationRigGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAnimationRigGraphCandidateQueryResult& OutResult,
		FAnimationRigGraphCandidateQueryError& OutError);

	/** Re-enumerate the native default-graph blank context and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolveAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr,
		const TMap<FString, FString>& Select = {});

	/** Re-enumerate and invoke the same supported RigVM menu action. */
	UEAGENTINTERFACE_API bool TryPerformAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FVector2f& Location,
		URigVMNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});

	/** Unit-only compatibility caller; rejects other action kinds before mutation. */
	UEAGENTINTERFACE_API bool TryPerformAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FVector2f& Location,
		URigVMUnitNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});
}
