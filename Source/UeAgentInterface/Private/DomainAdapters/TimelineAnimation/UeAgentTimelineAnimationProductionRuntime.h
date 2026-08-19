#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationExecutionChains.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::BindingGateway::Query
{
	class FUeAgentDomainQueryProviderRegistry;
}

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationProductionRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = -1;
		TArray<FUeAgentTimelineAnimationSubjectBinding> SubjectBindings;
		TArray<FUeAgentTimelineAnimationRigCandidateBinding> RigCandidateBindings;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationProductionRuntime final
	{
	public:
		static bool TryCreate(
			const FUeAgentTimelineAnimationProductionRuntimeConfig& Config,
			TSharedPtr<FUeAgentTimelineAnimationProductionRuntime>& OutRuntime,
			FUeAgentTimelineAnimationExecutionError& OutError);

		const TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			GetDescriptorRegistry() const;
		const TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry>& GetQueryProviderRegistry() const;
		const TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>>&
			GetOperationChains() const;
		bool TryReadRevision(
			const FString& ExplicitRef,
			FTimelineAnimationRevision& OutRevision,
			FUeAgentTimelineAnimationExecutionError& OutError) const;
		void GetExplicitSubjectIdentities(TArray<FString>& OutIds) const;
		void GetExplicitRigCandidateIdentities(TArray<FString>& OutIds) const;

	private:
		FUeAgentTimelineAnimationProductionRuntime(
			TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>
				InDescriptors,
			TSharedPtr<const UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistry> InQueries,
			TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>> InOperations,
			FUeAgentTimelineAnimationNativeDependencies InDependencies);

		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry> Descriptors;
		TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry> Queries;
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>> Operations;
		FUeAgentTimelineAnimationNativeDependencies Dependencies;
	};
}
