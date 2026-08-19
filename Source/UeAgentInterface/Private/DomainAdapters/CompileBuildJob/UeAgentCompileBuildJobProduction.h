#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobAdapter.h"

#define UAI_COMPILE_BUILD_JOB_PRODUCTION_V1 1

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobPolicyDescriptor final
	{
	public:
		FUeAgentCompileBuildJobPolicyDescriptor(
			EUeAgentCompileBuildJobFamily InFamily,
			FString InPolicyId,
			FString InNativeMechanismId,
			FString InReadbackOracleId);

		EUeAgentCompileBuildJobFamily GetFamily() const;
		const FString& GetPolicyId() const;
		const FString& GetNativeMechanismId() const;
		const FString& GetReadbackOracleId() const;
		bool IsValid() const;

	private:
		EUeAgentCompileBuildJobFamily Family;
		FString PolicyId;
		FString NativeMechanismId;
		FString ReadbackOracleId;
	};

	class UEAGENTINTERFACE_API IUeAgentCompileBuildJobPortBuilder
	{
	public:
		virtual ~IUeAgentCompileBuildJobPortBuilder() = default;

		virtual bool TryCreatePort(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobPolicyRegistration final
	{
	public:
		FUeAgentCompileBuildJobPolicyRegistration(
			FUeAgentCompileBuildJobPolicyDescriptor InDescriptor,
			TSharedRef<
				IUeAgentCompileBuildJobPortBuilder,
				ESPMode::ThreadSafe> InBuilder);

		const FUeAgentCompileBuildJobPolicyDescriptor&
			GetDescriptor() const;
		IUeAgentCompileBuildJobPortBuilder& GetBuilder() const;

	private:
		FUeAgentCompileBuildJobPolicyDescriptor Descriptor;
		TSharedRef<IUeAgentCompileBuildJobPortBuilder, ESPMode::ThreadSafe>
			Builder;
	};

	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobProductionPortFactory final
		: public IUeAgentCompileBuildJobPortFactory
	{
	public:
		static bool TryCreate(
			const TArray<FUeAgentCompileBuildJobPolicyRegistration>&
				Registrations,
			TSharedPtr<
				FUeAgentCompileBuildJobProductionPortFactory>& OutFactory,
			ExecutionMutation::FExecutionMutationError& OutError);

		const TArray<FUeAgentCompileBuildJobPolicyDescriptor>&
			GetPolicyDescriptors() const;

		bool TryCreateOperationPort(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		explicit FUeAgentCompileBuildJobProductionPortFactory(
			TArray<FUeAgentCompileBuildJobPolicyRegistration>
				InRegistrations);

		TArray<FUeAgentCompileBuildJobPolicyRegistration> Registrations;
		TArray<FUeAgentCompileBuildJobPolicyDescriptor> Descriptors;
	};

	class UEAGENTINTERFACE_API IUeAgentCompileBuildJobGameThreadStage
	{
	public:
		virtual ~IUeAgentCompileBuildJobGameThreadStage() = default;

		virtual bool Execute(
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual void RequestStop(const FString& Reason) = 0;
		virtual FString GetReportRef() const { return {}; }
	};

	UEAGENTINTERFACE_API bool TryCreateCompileBuildJobGameThreadStagePort(
		const FUeAgentCompileBuildJobTypedRequest& Request,
		const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
		TSharedRef<IUeAgentCompileBuildJobGameThreadStage, ESPMode::ThreadSafe>
			Stage,
		TSharedPtr<
			ExecutionMutation::ILongRunningOperationPort,
			ESPMode::ThreadSafe>& OutPort,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobProcessLaunchSpec final
	{
	public:
		FUeAgentCompileBuildJobProcessLaunchSpec(
			FString InExecutable,
			FString InArguments,
			FString InWorkingDirectory,
			double InTimeoutSeconds = 0.0);

		const FString& GetExecutable() const;
		const FString& GetArguments() const;
		const FString& GetWorkingDirectory() const;
		double GetTimeoutSeconds() const;
		bool IsValid() const;

	private:
		FString Executable;
		FString Arguments;
		FString WorkingDirectory;
		double TimeoutSeconds = 0.0;
	};

	class UEAGENTINTERFACE_API
	IUeAgentCompileBuildJobProcessOutputSink
	{
	public:
		virtual ~IUeAgentCompileBuildJobProcessOutputSink() = default;

		virtual bool Append(
			const FString& Chunk,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual FString GetReportRef() const = 0;
		virtual bool Release(
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateCompileBuildJobOwnedProcessPort(
		const FUeAgentCompileBuildJobTypedRequest& Request,
		const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
		const FUeAgentCompileBuildJobProcessLaunchSpec& LaunchSpec,
		TSharedRef<
			IUeAgentCompileBuildJobProcessOutputSink,
			ESPMode::ThreadSafe> OutputSink,
		TSharedPtr<
			ExecutionMutation::ILongRunningOperationPort,
			ESPMode::ThreadSafe>& OutPort,
		ExecutionMutation::FExecutionMutationError& OutError);
}
