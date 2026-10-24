#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphAdapter.h"

class UBehaviorTree;
class UClass;
class UEnvQuery;
class UPCGComponent;
class UPCGData;
class UPCGDataAsset;
class UPCGGraph;
class UPCGGraphInterface;
class UPCGNode;
class USmartObjectDefinition;
class UStateTree;

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	class FPcgAiNodeGraphNativeMutationReceipt final
	{
	public:
		const FString& GetEvidenceId() const { return EvidenceId; }
		void SetEvidenceId(FString InEvidenceId)
		{
			EvidenceId = MoveTemp(InEvidenceId);
		}
	private:
		FString EvidenceId;
	};

	class FPcgAiNodeGraphBarrierReceipt final
	{
	public:
		EPcgAiNodeGraphBarrierKind GetBarrierKind() const
		{
			return BarrierKind;
		}
		// Completion of native editor settle/compile/validation, not runtime readiness.
		bool IsComplete() const { return bComplete; }
		int32 GetErrorCount() const { return ErrorCount; }
		const FString& GetBarrierEvidenceId() const
		{
			return BarrierEvidenceId;
		}
		const FString& GetSettleEvidenceId() const
		{
			return SettleEvidenceId;
		}
	private:
		friend class FPcgAiNodeGraphNativeHook;
		EPcgAiNodeGraphBarrierKind BarrierKind =
			EPcgAiNodeGraphBarrierKind::Compile;
		bool bComplete = false;
		int32 ErrorCount = 0;
		FString BarrierEvidenceId;
		FString SettleEvidenceId;
	};

	class UEAGENTINTERFACE_API FPcgAiNodeGraphNativeHook final
	{
	public:
		bool TryCaptureOwnedAssetObject(
			UObject& Outer,
			const FString& AssetKind,
			UClass& AssetClass,
			FName ObjectName,
			UObject& Object,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphAssetIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryCreateAndCaptureOwnedAssetObject(
			UObject& Outer,
			const FString& AssetKind,
			UClass& AssetClass,
			FName ObjectName,
			UObject*& OutObject,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphAssetIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryCompileAndCaptureBehaviorTree(
			UBehaviorTree& Tree,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryCompileAndCaptureEqs(
			UEnvQuery& Query,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryCompileAndCaptureStateTree(
			UStateTree& Tree,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryEnsureCompileAndCaptureStateTree(
			UStateTree& Tree,
			FName RootStateName,
			FName ChildStateName,
			const FGuid& TransitionGuid,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryAddPcgNode(
			UPCGGraph& Graph,
			UClass& SettingsClass,
			UPCGNode*& OutNode,
			FPcgAiNodeGraphNativeMutationReceipt& OutReceipt,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryConnectPcgNodes(
			UPCGGraph& Graph,
			UPCGNode& FromNode,
			FName FromPinLabel,
			UPCGNode& ToNode,
			FName ToPinLabel,
			FPcgAiNodeGraphNativeMutationReceipt& OutReceipt,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TrySetPcgDoubleParameter(
			UPCGGraph& Graph,
			const FGuid& PropertyGuid,
			FName PropertyName,
			double Value,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphParameterIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryCompileAndCapturePcgGraph(
			UPCGGraph& Graph,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphTopology& OutTopology,
			TArray<FPcgAiNodeGraphParameterIdentity>& OutParameters,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TryBindAndCapturePcgComponent(
			UPCGComponent& Component,
			UPCGGraphInterface& Graph,
			const FString& GenerationPolicy,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphComponentBindingIdentity& OutIdentity,
			FString& OutRevision,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;

		bool TrySetAndCapturePcgDataAsset(
			UPCGDataAsset& DataAsset,
			UPCGData& Data,
			const FSoftObjectPath& SourceIdentity,
			FPcgAiNodeGraphNativeMutationReceipt& OutApplyReceipt,
			FPcgAiNodeGraphBarrierReceipt& OutBarrierReceipt,
			FPcgAiNodeGraphDataAssetIdentity& OutIdentity,
			FString& OutEvidenceId,
			FPcgAiNodeGraphAdapterError& OutError) const;
	};
}
