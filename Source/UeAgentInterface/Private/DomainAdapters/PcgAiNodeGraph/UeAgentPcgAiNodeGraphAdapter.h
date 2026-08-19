#pragma once

#include "CoreMinimal.h"

#define UAI_PCG_AI_NODE_GRAPH_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::PcgAiNodeGraph
{
	enum class EPcgAiNodeGraphPinDirection : uint8
	{
		Input,
		Output,
	};

	enum class EPcgAiNodeGraphBarrierKind : uint8
	{
		Compile,
		Refresh,
		Validate,
	};

	enum class EPcgAiNodeGraphEntryOwnership : uint8
	{
		Local,
		Inherited,
	};

	class FPcgAiNodeGraphItemIdentity final
	{
	public:
		FPcgAiNodeGraphItemIdentity() = default;
		FPcgAiNodeGraphItemIdentity(
			FString InItemKey,
			FString InNativeIdentity,
			FString InStableBuiltinRole,
			FString InNativeCarrierTypeIdentity,
			FString InNativeTypeIdentity)
			: ItemKey(MoveTemp(InItemKey))
			, NativeIdentity(MoveTemp(InNativeIdentity))
			, StableBuiltinRole(MoveTemp(InStableBuiltinRole))
			, NativeCarrierTypeIdentity(
				MoveTemp(InNativeCarrierTypeIdentity))
			, NativeTypeIdentity(MoveTemp(InNativeTypeIdentity))
		{
		}

		const FString& GetItemKey() const { return ItemKey; }
		const FString& GetNativeIdentity() const { return NativeIdentity; }
		const FString& GetStableBuiltinRole() const
		{
			return StableBuiltinRole;
		}
		const FString& GetNativeCarrierTypeIdentity() const
		{
			return NativeCarrierTypeIdentity;
		}
		const FString& GetNativeTypeIdentity() const
		{
			return NativeTypeIdentity;
		}

	private:
		FString ItemKey;
		FString NativeIdentity;
		FString StableBuiltinRole;
		FString NativeCarrierTypeIdentity;
		FString NativeTypeIdentity;
	};

	class FPcgAiNodeGraphPinIdentity final
	{
	public:
		FPcgAiNodeGraphPinIdentity() = default;
		FPcgAiNodeGraphPinIdentity(
			FString InPinKey,
			FString InOwnerItemKey,
			FString InNativeLabel,
			EPcgAiNodeGraphPinDirection InDirection,
			FString InNativeTypeIdentity)
			: PinKey(MoveTemp(InPinKey))
			, OwnerItemKey(MoveTemp(InOwnerItemKey))
			, NativeLabel(MoveTemp(InNativeLabel))
			, Direction(InDirection)
			, NativeTypeIdentity(MoveTemp(InNativeTypeIdentity))
		{
		}

		const FString& GetPinKey() const { return PinKey; }
		const FString& GetOwnerItemKey() const { return OwnerItemKey; }
		const FString& GetNativeLabel() const { return NativeLabel; }
		EPcgAiNodeGraphPinDirection GetDirection() const { return Direction; }
		const FString& GetNativeTypeIdentity() const
		{
			return NativeTypeIdentity;
		}

	private:
		FString PinKey;
		FString OwnerItemKey;
		FString NativeLabel;
		EPcgAiNodeGraphPinDirection Direction =
			EPcgAiNodeGraphPinDirection::Input;
		FString NativeTypeIdentity;
	};

	class FPcgAiNodeGraphRelationIdentity final
	{
	public:
		FPcgAiNodeGraphRelationIdentity() = default;
		FPcgAiNodeGraphRelationIdentity(
			FString InRelationKey,
			FString InRelationKind,
			FString InNativeRelationIdentity,
			FString InFromIdentity,
			FString InToIdentity,
			int32 InOrderIndex)
			: RelationKey(MoveTemp(InRelationKey))
			, RelationKind(MoveTemp(InRelationKind))
			, NativeRelationIdentity(MoveTemp(InNativeRelationIdentity))
			, FromIdentity(MoveTemp(InFromIdentity))
			, ToIdentity(MoveTemp(InToIdentity))
			, OrderIndex(InOrderIndex)
		{
		}

		const FString& GetRelationKey() const { return RelationKey; }
		const FString& GetRelationKind() const { return RelationKind; }
		const FString& GetNativeRelationIdentity() const
		{
			return NativeRelationIdentity;
		}
		const FString& GetFromIdentity() const { return FromIdentity; }
		const FString& GetToIdentity() const { return ToIdentity; }
		int32 GetOrderIndex() const { return OrderIndex; }

	private:
		FString RelationKey;
		FString RelationKind;
		FString NativeRelationIdentity;
		FString FromIdentity;
		FString ToIdentity;
		int32 OrderIndex = INDEX_NONE;
	};

	class FPcgAiNodeGraphTopology final
	{
	public:
		FPcgAiNodeGraphTopology() = default;
		FPcgAiNodeGraphTopology(
			FString InTargetKind,
			FString InTargetIdentity,
			FString InStructureIdentity,
			TArray<FPcgAiNodeGraphItemIdentity> InItems,
			TArray<FPcgAiNodeGraphPinIdentity> InPins,
			TArray<FPcgAiNodeGraphRelationIdentity> InRelations)
			: TargetKind(MoveTemp(InTargetKind))
			, TargetIdentity(MoveTemp(InTargetIdentity))
			, StructureIdentity(MoveTemp(InStructureIdentity))
			, Items(MoveTemp(InItems))
			, Pins(MoveTemp(InPins))
			, Relations(MoveTemp(InRelations))
		{
		}

		const FString& GetTargetKind() const { return TargetKind; }
		const FString& GetTargetIdentity() const { return TargetIdentity; }
		const FString& GetStructureIdentity() const
		{
			return StructureIdentity;
		}
		const TArray<FPcgAiNodeGraphItemIdentity>& GetItems() const
		{
			return Items;
		}
		const TArray<FPcgAiNodeGraphPinIdentity>& GetPins() const
		{
			return Pins;
		}
		const TArray<FPcgAiNodeGraphRelationIdentity>& GetRelations() const
		{
			return Relations;
		}

	private:
		FString TargetKind;
		FString TargetIdentity;
		FString StructureIdentity;
		TArray<FPcgAiNodeGraphItemIdentity> Items;
		TArray<FPcgAiNodeGraphPinIdentity> Pins;
		TArray<FPcgAiNodeGraphRelationIdentity> Relations;
	};

	class FPcgAiNodeGraphAssetIdentity final
	{
	public:
		FPcgAiNodeGraphAssetIdentity() = default;
		FPcgAiNodeGraphAssetIdentity(
			FString InAssetKind,
			FString InAssetPath,
			FString InNativeClass)
			: AssetKind(MoveTemp(InAssetKind))
			, AssetPath(MoveTemp(InAssetPath))
			, NativeClass(MoveTemp(InNativeClass))
		{
		}
		const FString& GetAssetKind() const { return AssetKind; }
		const FString& GetAssetPath() const { return AssetPath; }
		const FString& GetNativeClass() const { return NativeClass; }
	private:
		FString AssetKind;
		FString AssetPath;
		FString NativeClass;
	};

	class FPcgAiNodeGraphBlackboardEntryIdentity final
	{
	public:
		FPcgAiNodeGraphBlackboardEntryIdentity() = default;
		FPcgAiNodeGraphBlackboardEntryIdentity(
			FString InOwnerIdentity,
			FString InEntryName,
			FString InKeyTypeIdentity,
			EPcgAiNodeGraphEntryOwnership InOwnership)
			: OwnerIdentity(MoveTemp(InOwnerIdentity))
			, EntryName(MoveTemp(InEntryName))
			, KeyTypeIdentity(MoveTemp(InKeyTypeIdentity))
			, Ownership(InOwnership)
		{
		}
		const FString& GetOwnerIdentity() const { return OwnerIdentity; }
		const FString& GetEntryName() const { return EntryName; }
		const FString& GetKeyTypeIdentity() const { return KeyTypeIdentity; }
		EPcgAiNodeGraphEntryOwnership GetOwnership() const
		{
			return Ownership;
		}
	private:
		FString OwnerIdentity;
		FString EntryName;
		FString KeyTypeIdentity;
		EPcgAiNodeGraphEntryOwnership Ownership =
			EPcgAiNodeGraphEntryOwnership::Local;
	};

	class FPcgAiNodeGraphParameterIdentity final
	{
	public:
		FPcgAiNodeGraphParameterIdentity() = default;
		FPcgAiNodeGraphParameterIdentity(
			FString InOwnerIdentity,
			FGuid InPropertyGuid,
			FName InPropertyName,
			FString InPropertyType)
			: OwnerIdentity(MoveTemp(InOwnerIdentity))
			, PropertyGuid(InPropertyGuid)
			, PropertyName(InPropertyName)
			, PropertyType(MoveTemp(InPropertyType))
		{
		}
		const FString& GetOwnerIdentity() const { return OwnerIdentity; }
		const FGuid& GetPropertyGuid() const { return PropertyGuid; }
		FName GetPropertyName() const { return PropertyName; }
		const FString& GetPropertyType() const { return PropertyType; }
	private:
		FString OwnerIdentity;
		FGuid PropertyGuid;
		FName PropertyName;
		FString PropertyType;
	};

	class FPcgAiNodeGraphComponentBindingIdentity final
	{
	public:
		FPcgAiNodeGraphComponentBindingIdentity() = default;
		FPcgAiNodeGraphComponentBindingIdentity(
			FString InActorIdentity,
			FString InComponentIdentity,
			FString InGraphIdentity,
			FString InGenerationPolicy)
			: ActorIdentity(MoveTemp(InActorIdentity))
			, ComponentIdentity(MoveTemp(InComponentIdentity))
			, GraphIdentity(MoveTemp(InGraphIdentity))
			, GenerationPolicy(MoveTemp(InGenerationPolicy))
		{
		}
		const FString& GetActorIdentity() const { return ActorIdentity; }
		const FString& GetComponentIdentity() const { return ComponentIdentity; }
		const FString& GetGraphIdentity() const { return GraphIdentity; }
		const FString& GetGenerationPolicy() const { return GenerationPolicy; }
	private:
		FString ActorIdentity;
		FString ComponentIdentity;
		FString GraphIdentity;
		FString GenerationPolicy;
	};

	class FPcgAiNodeGraphDataAssetIdentity final
	{
	public:
		FPcgAiNodeGraphDataAssetIdentity() = default;
		FPcgAiNodeGraphDataAssetIdentity(
			FString InDataAssetIdentity,
			FString InDataTypeIdentity,
			FString InSourceIdentity,
			FString InContentRevision)
			: DataAssetIdentity(MoveTemp(InDataAssetIdentity))
			, DataTypeIdentity(MoveTemp(InDataTypeIdentity))
			, SourceIdentity(MoveTemp(InSourceIdentity))
			, ContentRevision(MoveTemp(InContentRevision))
		{
		}
		const FString& GetDataAssetIdentity() const { return DataAssetIdentity; }
		const FString& GetDataTypeIdentity() const { return DataTypeIdentity; }
		const FString& GetSourceIdentity() const { return SourceIdentity; }
		const FString& GetContentRevision() const { return ContentRevision; }
	private:
		FString DataAssetIdentity;
		FString DataTypeIdentity;
		FString SourceIdentity;
		FString ContentRevision;
	};

	class FPcgAiNodeGraphLifecycleEvidence final
	{
	public:
		FPcgAiNodeGraphLifecycleEvidence(
			FString InBeforeRevision,
			FString InApplyEvidence,
			FString InSettleEvidence,
			FString InReadbackRevision,
			FString InReadbackEvidence,
			EPcgAiNodeGraphBarrierKind InBarrierKind,
			bool bInBarrierComplete,
			int32 InBarrierErrorCount,
			bool bInSettleComplete,
			bool bInCaptureComplete,
			bool bInReadbackIndependent)
			: BeforeRevision(MoveTemp(InBeforeRevision))
			, ApplyEvidence(MoveTemp(InApplyEvidence))
			, SettleEvidence(MoveTemp(InSettleEvidence))
			, ReadbackRevision(MoveTemp(InReadbackRevision))
			, ReadbackEvidence(MoveTemp(InReadbackEvidence))
			, BarrierKind(InBarrierKind)
			, bBarrierComplete(bInBarrierComplete)
			, BarrierErrorCount(InBarrierErrorCount)
			, bSettleComplete(bInSettleComplete)
			, bCaptureComplete(bInCaptureComplete)
			, bReadbackIndependent(bInReadbackIndependent)
		{
		}
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetApplyEvidence() const { return ApplyEvidence; }
		const FString& GetSettleEvidence() const { return SettleEvidence; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetReadbackEvidence() const { return ReadbackEvidence; }
		EPcgAiNodeGraphBarrierKind GetBarrierKind() const { return BarrierKind; }
		bool IsBarrierComplete() const { return bBarrierComplete; }
		int32 GetBarrierErrorCount() const { return BarrierErrorCount; }
		bool IsSettleComplete() const { return bSettleComplete; }
		bool IsCaptureComplete() const { return bCaptureComplete; }
		bool IsReadbackIndependent() const { return bReadbackIndependent; }
	private:
		FString BeforeRevision;
		FString ApplyEvidence;
		FString SettleEvidence;
		FString ReadbackRevision;
		FString ReadbackEvidence;
		EPcgAiNodeGraphBarrierKind BarrierKind =
			EPcgAiNodeGraphBarrierKind::Compile;
		bool bBarrierComplete = false;
		int32 BarrierErrorCount = 0;
		bool bSettleComplete = false;
		bool bCaptureComplete = false;
		bool bReadbackIndependent = false;
	};

	class FPcgAiNodeGraphAdapterError final
	{
	public:
		const FString& GetCode() const { return Code; }
		const FString& GetDetail() const { return Detail; }
		void Reset() { Code.Reset(); Detail.Reset(); }
		void Set(FString InCode, FString InDetail = FString())
		{
			Code = MoveTemp(InCode);
			Detail = MoveTemp(InDetail);
		}
	private:
		FString Code;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryValidatePcgAiNodeGraphTopology(
		const FPcgAiNodeGraphTopology& Topology,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidatePcgAiNodeGraphAssetIdentity(
		const FPcgAiNodeGraphAssetIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool
	TryValidatePcgAiNodeGraphBlackboardEntryIdentity(
		const FPcgAiNodeGraphBlackboardEntryIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidatePcgAiNodeGraphParameterIdentity(
		const FPcgAiNodeGraphParameterIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool
	TryValidatePcgAiNodeGraphComponentBindingIdentity(
		const FPcgAiNodeGraphComponentBindingIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidatePcgAiNodeGraphDataAssetIdentity(
		const FPcgAiNodeGraphDataAssetIdentity& Identity,
		FPcgAiNodeGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidatePcgAiNodeGraphLifecycleEvidence(
		const FPcgAiNodeGraphLifecycleEvidence& Evidence,
		FPcgAiNodeGraphAdapterError& OutError);
}
