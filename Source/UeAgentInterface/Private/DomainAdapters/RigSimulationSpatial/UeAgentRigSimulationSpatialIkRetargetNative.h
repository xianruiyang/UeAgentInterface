#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialNative.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UIKRigDefinition;
class UIKRetargeter;

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialIkRigBinding
	{
		FString Identity;
		TWeakObjectPtr<UIKRigDefinition> Rig;
		FString PreviewMeshIdentity;
		TMap<FString, FName> BoneBindings;
		TMap<FString, FName> GoalBindings;
		TMap<FString, FName> ChainBindings;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialRetargeterBinding
	{
		FString Identity;
		TWeakObjectPtr<UIKRetargeter> Retargeter;
		FString SourceRigIdentity;
		FString TargetRigIdentity;
		FString SourceNativePoseFrameId;
		FString TargetNativePoseFrameId;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialRetargetAssetEffectRequest
	{
		TWeakObjectPtr<UObject> SourceSubject;
		TWeakObjectPtr<UObject> TargetSubject;
		TWeakObjectPtr<UIKRetargeter> Retargeter;
		FString DestinationRoot;
		TArray<FRigSimulationSpatialAssetRequest> AssetRequests;

		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialRetargetAssetEffectResult
	{
		TArray<TWeakObjectPtr<UObject>> GeneratedAssets;
		TArray<FString> GeneratedAssetRefs;

		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialAssetEffectPort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialAssetEffectPort() = default;

		virtual bool TryApplyRetargetAssets(
			const FUeAgentRigSimulationSpatialRetargetAssetEffectRequest& Request,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialRetargetAssetEffectResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialIkRetargetNativePorts(
		TSharedRef<IUeAgentRigSimulationSpatialNativeEnvironment> Environment,
		TSharedPtr<IUeAgentRigSimulationSpatialAssetEffectPort> AssetEffectPort,
		FUeAgentRigSimulationSpatialNativePorts& OutPorts,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}
