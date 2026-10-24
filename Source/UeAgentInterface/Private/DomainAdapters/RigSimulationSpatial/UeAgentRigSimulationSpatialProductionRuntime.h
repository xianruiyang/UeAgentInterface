#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDeformerPcgNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialIkRetargetNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"
#include "HAL/ThreadSafeCounter.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

namespace UE::UAI::BindingGateway::Query
{
	class FUeAgentDomainQueryProviderRegistry;
}

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialObjectBinding
	{
		FUeAgentRigSimulationSpatialObjectBinding() = default;
		FUeAgentRigSimulationSpatialObjectBinding(
			FString InIdentity,
			UObject* InObject);

		FString Identity;
		TWeakObjectPtr<UObject> Object;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialClockBinding
	{
		FUeAgentRigSimulationSpatialClockBinding() = default;
		FUeAgentRigSimulationSpatialClockBinding(
			FString InClockId,
			FString InNativeClockId,
			FString InEvidenceId);

		FString ClockId;
		FString NativeClockId;
		FString EvidenceId;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialFrameBinding
	{
		FUeAgentRigSimulationSpatialFrameBinding(
			UE::UAI::StateSemantic::FUeAgentSpatialFrame InFrame,
			bool bInNativeFrame);

		UE::UAI::StateSemantic::FUeAgentSpatialFrame Frame;
		bool bNativeFrame = false;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialProductionRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		FString RuntimeGeneration;
		// Location of the canonical input within the caller's request; empty for a standalone input.
		FString InputRootPointer;
		int64 RegistryEpoch = -1;
		TArray<FUeAgentRigSimulationSpatialObjectBinding> ObjectBindings;
		TArray<FUeAgentRigSimulationSpatialClockBinding> ClockBindings;
		TArray<FUeAgentRigSimulationSpatialFrameBinding> FrameBindings;
		TArray<FUeAgentRigSimulationSpatialSkeletonBinding> SkeletonBindings;
		TArray<FUeAgentRigSimulationSpatialSocketBinding> SocketBindings;
		TArray<FUeAgentRigSimulationSpatialRigBakeBinding> RigBakeBindings;
		TArray<FUeAgentRigSimulationSpatialIkRigBinding> IkRigBindings;
		TArray<FUeAgentRigSimulationSpatialRetargeterBinding> RetargeterBindings;
		TArray<FUeAgentRigSimulationSpatialPhysicsAssetBinding> PhysicsAssetBindings;
		TArray<FUeAgentRigSimulationSpatialPhysicsSubjectBinding> PhysicsSubjectBindings;
		TArray<FUeAgentRigSimulationSpatialDeformerGeometryBinding>
			DeformerGeometryBindings;
		TArray<FUeAgentRigSimulationSpatialDeformerTrainingInputBinding>
			DeformerTrainingInputBindings;
		TArray<FUeAgentRigSimulationSpatialDeformerEvaluationBinding>
			DeformerEvaluationBindings;
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort;
		TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort> PhysicsSessionPort;
		TArray<UE::UAI::StateSemantic::FUeAgentSpatialConversion>
			SpatialConversions;
		TArray<UE::UAI::StateSemantic::FUeAgentTemporalConversion>
			TemporalConversions;

		bool TryValidate(Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool
	TryCreateRigSimulationSpatialProductionAssetEffectPort(
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort>& OutPort,
		FUeAgentRigSimulationSpatialExecutionError& OutError);

	class UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialProductionRuntime final
	{
	public:
		static bool TryCreate(
			const FUeAgentRigSimulationSpatialProductionRuntimeConfig& Config,
			TSharedPtr<FUeAgentRigSimulationSpatialProductionRuntime>& OutRuntime,
			FUeAgentRigSimulationSpatialExecutionError& OutError);

		const TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			GetDescriptorRegistry() const;
		const TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry>& GetQueryProviderRegistry() const;
		const TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>&
			GetOperationChains() const;
		bool TryReadRevision(
			const FString& ExplicitRef,
			FRigSimulationSpatialRevision& OutRevision,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const;
		bool TryGetIdentityFields(
			const FString& CapabilityId,
			TArray<FString>& OutFields,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const;
		bool TryDecodeForExecution(
			const FString& CapabilityId,
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			TSharedPtr<const FUeAgentRigSimulationSpatialRequest>& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const;
		int32 GetFamilyDispatchCallCount() const;
		void GetExplicitObjectIdentities(TArray<FString>& OutIds) const;

	private:
		FUeAgentRigSimulationSpatialProductionRuntime(
			TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>
				InDescriptors,
			TSharedPtr<const UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistry> InQueries,
			TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>
				InOperations,
			TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment>
				InEnvironment,
			TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> InDecoder,
			TSharedRef<FThreadSafeCounter> InFamilyDispatchCounter);

		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry> Descriptors;
		TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry> Queries;
		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>> Operations;
		TSharedRef<IUeAgentRigSimulationSpatialRequestEnvironment> Environment;
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder;
		TSharedRef<FThreadSafeCounter> FamilyDispatchCounter;
	};
}
