#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class FJsonObject;
class UAnimBlueprint;
class UEdGraph;
class UEdGraphNode;
class UAnimationStateMachineGraph;

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidate
	{
		/** Server-side identity retained for internal re-resolution only. */
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		FString Id;
		FString Name;
		FString Label;
		FString Surface;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString NodeClassPath;
		FString SourceOrigin;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		TMap<FString, FString> SelectionFields;
		/** Original provider binding, retained only for native re-resolution. */
		TMap<FString, FString> NativeSelectors;
		bool bSupported = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		FString Surface;
		TArray<FAnimationBlueprintCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
	};

	using FAnimationBlueprintCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	/**
	 * Enumerates one native creation surface on a transient AnimBlueprint made
	 * by the same factory seed used by T16 create preflight.
	 */
	UEAGENTINTERFACE_API bool TryQueryAnimationBlueprintCandidates(
		const FString& Target,
		const FString& Surface,
		const TSharedPtr<FJsonObject>& ClassSettings,
		const FString& Query,
		FAnimationBlueprintCandidateQueryResult& OutResult,
		FAnimationBlueprintCandidateQueryError& OutError);

	/** Re-enumerates and invokes the native AnimGraph StateMachine spawner. */
	UEAGENTINTERFACE_API bool TryPerformAnimationGraphStateMachineCandidate(
		UAnimBlueprint& Blueprint,
		UEdGraph& Graph,
		const FString& Name,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});

	/** Re-enumerates and invokes one native StateMachine graph schema action. */
	UEAGENTINTERFACE_API bool TryPerformStateMachineGraphCandidate(
		UAnimationStateMachineGraph& Graph,
		const FString& Name,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});

	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}
