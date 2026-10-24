#pragma once

#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::LongRunningSession
{
	struct FUeAgentSafeExitExternalWitnessRef final
	{
		FString LaunchId;
		FString ExitAttemptId;
		FString Nonce;

		bool IsValid() const;
		bool TryValidate(ExecutionMutation::FExecutionMutationError& OutError) const;
	};

	struct FUeAgentSafeExitDirtyResolutionReceipt final
	{
		FString Mode;
		TArray<FString> SaveResources;
		TArray<FString> DiscardResources;
		TArray<FString> RemainingDirtyResources;

		bool IsValid() const;
	};

	class FUeAgentValidatedSafeExitWitness final
	{
	public:
		const FString& GetLaunchId() const;
		const FString& GetExitAttemptId() const;
		const FString& GetNonce() const;
		const FString& GetAttemptRoot() const;
		const TSharedRef<FJsonObject>& GetEditorProcessIdentity() const;

	private:
		friend bool TryValidateSafeExitExternalWitness(
			const FUeAgentSafeExitExternalWitnessRef&,
			FUeAgentValidatedSafeExitWitness&,
			ExecutionMutation::FExecutionMutationError&);

		FString LaunchId;
		FString ExitAttemptId;
		FString Nonce;
		FString AttemptRoot;
		TSharedRef<FJsonObject> EditorProcessIdentity =
			MakeShared<FJsonObject>();
	};

	bool TryValidateSafeExitExternalWitness(
		const FUeAgentSafeExitExternalWitnessRef& Ref,
		FUeAgentValidatedSafeExitWitness& OutWitness,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryCreateDurableSafeExitTarget(
		const FUeAgentValidatedSafeExitWitness& Witness,
		const FUeAgentSafeExitDirtyResolutionReceipt& DirtyResolution,
		const ExecutionMutation::FLongRunningOperationRequest& OperationRequest,
		FString EditorInstanceId,
		TSharedPtr<
			DomainAdapters::EditorSessionOperation::IUeAgentEditorHostExitTarget,
			ESPMode::ThreadSafe>& OutTarget,
		ExecutionMutation::FExecutionMutationError& OutError);
}
