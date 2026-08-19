#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphAdapter.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UObject;
class UPanelWidget;
class USCS_Node;
class UWidget;
class UWidgetBlueprint;

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	class FBlueprintUmgGraphNativeMutationReceipt final
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

	class FBlueprintUmgGraphCompileSettleReceipt final
	{
	public:
		bool IsCompileComplete() const { return bCompileComplete; }
		int32 GetErrorCount() const { return ErrorCount; }
		const FString& GetCompileEvidenceId() const
		{
			return CompileEvidenceId;
		}
		const FString& GetSettleEvidenceId() const
		{
			return SettleEvidenceId;
		}

	private:
		friend class FBlueprintUmgGraphNativeHook;
		bool bCompileComplete = false;
		int32 ErrorCount = 0;
		FString CompileEvidenceId;
		FString SettleEvidenceId;
	};

	class FBlueprintUmgGraphEventIdentity final
	{
	public:
		FBlueprintUmgGraphEventIdentity(
			FString InEventIdentity,
			FString InOwnerItemIdentity,
			FString InOwnerClassIdentity,
			FString InNativeFieldName,
			FString InSignatureIdentity)
			: EventIdentity(MoveTemp(InEventIdentity))
			, OwnerItemIdentity(MoveTemp(InOwnerItemIdentity))
			, OwnerClassIdentity(MoveTemp(InOwnerClassIdentity))
			, NativeFieldName(MoveTemp(InNativeFieldName))
			, SignatureIdentity(MoveTemp(InSignatureIdentity))
		{
		}

		const FString& GetEventIdentity() const { return EventIdentity; }
		const FString& GetOwnerItemIdentity() const
		{
			return OwnerItemIdentity;
		}
		const FString& GetOwnerClassIdentity() const
		{
			return OwnerClassIdentity;
		}
		const FString& GetNativeFieldName() const { return NativeFieldName; }
		const FString& GetSignatureIdentity() const
		{
			return SignatureIdentity;
		}

	private:
		FString EventIdentity;
		FString OwnerItemIdentity;
		FString OwnerClassIdentity;
		FString NativeFieldName;
		FString SignatureIdentity;
	};

	class UEAGENTINTERFACE_API FBlueprintUmgGraphNativeHook final
	{
	public:
		bool TryCreateBlueprint(
			UObject& Outer,
			FName AssetName,
			UClass& ParentClass,
			UBlueprint*& OutBlueprint,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCreateWidgetBlueprint(
			UObject& Outer,
			FName AssetName,
			UWidgetBlueprint*& OutBlueprint,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCreateUberGraph(
			UBlueprint& Blueprint,
			FName GraphName,
			UEdGraph*& OutGraph,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryAddKnotNode(
			UEdGraph& Graph,
			const FGuid& NodeGuid,
			int32 PositionX,
			int32 PositionY,
			UEdGraphNode*& OutNode,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryConnectPins(
			UEdGraphPin& FromPin,
			UEdGraphPin& ToPin,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryAddSCSComponent(
			UBlueprint& Blueprint,
			UClass& ComponentClass,
			FName VariableName,
			const FGuid& VariableGuid,
			USCS_Node* Parent,
			USCS_Node*& OutNode,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryAddWidget(
			UWidgetBlueprint& Blueprint,
			UClass& WidgetClass,
			FName WidgetName,
			const FGuid& WidgetGuid,
			UPanelWidget* Parent,
			int32 InsertIndex,
			UWidget*& OutWidget,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TrySetBlueprintDescription(
			UBlueprint& Blueprint,
			const FString& Description,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCompileAndSettle(
			UBlueprint& Blueprint,
			FBlueprintUmgGraphCompileSettleReceipt& OutReceipt,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCaptureBlueprintGraph(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const FString& TargetIdentity,
			const FString& StructureIdentity,
			FBlueprintUmgGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCaptureBlueprintScs(
			UBlueprint& Blueprint,
			const FString& TargetIdentity,
			FBlueprintUmgGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryCaptureWidgetTree(
			UWidgetBlueprint& Blueprint,
			const FString& TargetIdentity,
			FBlueprintUmgGraphTopology& OutTopology,
			FString& OutRevision,
			FString& OutEvidenceId,
			FBlueprintUmgGraphAdapterError& OutError) const;

		bool TryDiscoverAssignableEvents(
			const UObject& Owner,
			const FString& OwnerItemIdentity,
			TArray<FBlueprintUmgGraphEventIdentity>& OutEvents,
			FBlueprintUmgGraphAdapterError& OutError) const;
	};
}
