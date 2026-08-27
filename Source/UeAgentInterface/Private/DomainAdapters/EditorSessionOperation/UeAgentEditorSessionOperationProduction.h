#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationAdapter.h"

class FJsonObject;

#define UAI_EDITOR_SESSION_OPERATION_PRODUCTION_V1 1

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionOperationPolicyDescriptor final
	{
	public:
		FUeAgentEditorSessionOperationPolicyDescriptor(
			EUeAgentEditorSessionOperationFamily InFamily,
			FString InPolicyId,
			FString InNativeMechanismId,
			FString InReadbackOracleId,
			Contract::EUeAgentStatePlane InRequiredStatePlane,
			TArray<ContextResource::EUeAgentExecutionWorldType>
				InAllowedWorldTypes = {},
			FString InSubjectSchemaJson = FString(),
			FString InParameterSchemaJson = FString(),
			FString InParameterGuidance = FString(),
			FString InResultGuidance = FString(),
			FString InMinimumExample = FString());

		EUeAgentEditorSessionOperationFamily GetFamily() const;
		const FString& GetPolicyId() const;
		const FString& GetNativeMechanismId() const;
		const FString& GetReadbackOracleId() const;
		Contract::EUeAgentStatePlane GetRequiredStatePlane() const;
		const TArray<ContextResource::EUeAgentExecutionWorldType>&
			GetAllowedWorldTypes() const;
		const FString& GetSubjectSchemaJson() const;
		const FString& GetParameterSchemaJson() const;
		const FString& GetParameterGuidance() const;
		const FString& GetResultGuidance() const;
		const FString& GetMinimumExample() const;
		bool HasDiscoveryContract() const;
		bool IsValid() const;

	private:
		EUeAgentEditorSessionOperationFamily Family;
		FString PolicyId;
		FString NativeMechanismId;
		FString ReadbackOracleId;
		Contract::EUeAgentStatePlane RequiredStatePlane;
		TArray<ContextResource::EUeAgentExecutionWorldType>
			AllowedWorldTypes;
		FString SubjectSchemaJson;
		FString ParameterSchemaJson;
		FString ParameterGuidance;
		FString ResultGuidance;
		FString MinimumExample;
	};

	class UEAGENTINTERFACE_API
	IUeAgentEditorSessionOperationPortBuilder
	{
	public:
		virtual ~IUeAgentEditorSessionOperationPortBuilder() = default;

		virtual bool TryCreatePort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const ExecutionMutation::FLongRunningOperationRequest&
				OperationRequest,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionOperationPolicyRegistration final
	{
	public:
		FUeAgentEditorSessionOperationPolicyRegistration(
			FUeAgentEditorSessionOperationPolicyDescriptor InDescriptor,
			TSharedRef<
				IUeAgentEditorSessionOperationPortBuilder,
				ESPMode::ThreadSafe> InBuilder);

		const FUeAgentEditorSessionOperationPolicyDescriptor&
			GetDescriptor() const;
		IUeAgentEditorSessionOperationPortBuilder& GetBuilder() const;

	private:
		FUeAgentEditorSessionOperationPolicyDescriptor Descriptor;
		TSharedRef<IUeAgentEditorSessionOperationPortBuilder,
			ESPMode::ThreadSafe> Builder;
	};

	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionOperationProductionPortFactory final
		: public IUeAgentEditorSessionOperationPortFactory
	{
	public:
		static bool TryCreate(
			const TArray<FUeAgentEditorSessionOperationPolicyRegistration>&
				Registrations,
			TSharedPtr<
				FUeAgentEditorSessionOperationProductionPortFactory>& OutFactory,
			ExecutionMutation::FExecutionMutationError& OutError);

		const TArray<FUeAgentEditorSessionOperationPolicyDescriptor>&
			GetPolicyDescriptors() const;

		bool TryCreateOperationPort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const ExecutionMutation::FLongRunningOperationRequest&
				OperationRequest,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		explicit FUeAgentEditorSessionOperationProductionPortFactory(
			TArray<FUeAgentEditorSessionOperationPolicyRegistration>
				InRegistrations);

		TArray<FUeAgentEditorSessionOperationPolicyRegistration>
			Registrations;
		TArray<FUeAgentEditorSessionOperationPolicyDescriptor> Descriptors;
	};

	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionNativeObservation final
	{
	public:
		FUeAgentEditorSessionNativeObservation() = default;
		FUeAgentEditorSessionNativeObservation(
			ExecutionMutation::ELongRunningWorkState InState,
			FString InPhase,
			FString InErrorCode = FString(),
			FString InSessionRef = FString(),
			TSharedPtr<FJsonObject> InDetails = nullptr);

		ExecutionMutation::ELongRunningWorkState GetState() const;
		const FString& GetPhase() const;
		const FString& GetErrorCode() const;
		const FString& GetSessionRef() const;
		const TSharedPtr<FJsonObject>& GetDetails() const;
		bool IsValid() const;

	private:
		ExecutionMutation::ELongRunningWorkState State =
			ExecutionMutation::ELongRunningWorkState::Invalid;
		FString Phase;
		FString ErrorCode;
		FString SessionRef;
		TSharedPtr<FJsonObject> Details;
	};

	class UEAGENTINTERFACE_API IUeAgentEditorSessionNativeSession
	{
	public:
		virtual ~IUeAgentEditorSessionNativeSession() = default;

		virtual bool Start(
			const ExecutionMutation::FLongRunningOperationRequest& Request,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			FString& OutNativeResourceId,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Observe(
			FUeAgentEditorSessionNativeObservation& OutObservation,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool RequestStop(
			const FString& Reason,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Join(
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Release(
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateEditorSessionNativeResourcePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		TSharedRef<IUeAgentEditorSessionNativeSession, ESPMode::ThreadSafe>
			NativeSession,
		TSharedPtr<
			ExecutionMutation::ILongRunningOperationPort,
			ESPMode::ThreadSafe>& OutPort,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API IUeAgentEditorHostExitTarget
	{
	public:
		virtual ~IUeAgentEditorHostExitTarget() = default;

		virtual const FString& GetEditorInstanceId() const = 0;
		virtual void RequestExit() = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateEditorPlatformHostExitTarget(
		FString EditorInstanceId,
		TSharedPtr<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>&
			OutTarget,
		ExecutionMutation::FExecutionMutationError& OutError);

	UEAGENTINTERFACE_API bool TryCreateEditorHostExitSchedulePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		double ExitDelaySeconds,
		TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
			ExitTarget,
		TSharedPtr<
			ExecutionMutation::ILongRunningOperationPort,
			ESPMode::ThreadSafe>& OutPort,
		ExecutionMutation::FExecutionMutationError& OutError);
}
