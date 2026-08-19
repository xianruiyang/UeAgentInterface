#pragma once

#include "CoreMinimal.h"

#define UAI_ANIMATION_RIG_GRAPH_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	enum class EAnimationRigGraphPinDirection : uint8
	{
		Input,
		Output,
	};

	enum class EAnimationRigGraphBarrierKind : uint8
	{
		Compile,
		Refresh,
		Validate,
	};

	class FAnimationRigGraphItemIdentity final
	{
	public:
		FAnimationRigGraphItemIdentity() = default;
		FAnimationRigGraphItemIdentity(
			FString InItemKey,
			FString InIdentitySource,
			FString InNativeIdentity,
			FString InStableBuiltinRole,
			FString InNativeCarrierTypeIdentity,
			FString InNativeTypeIdentity,
			FString InSemanticRole)
			: ItemKey(MoveTemp(InItemKey))
			, IdentitySource(MoveTemp(InIdentitySource))
			, NativeIdentity(MoveTemp(InNativeIdentity))
			, StableBuiltinRole(MoveTemp(InStableBuiltinRole))
			, NativeCarrierTypeIdentity(MoveTemp(InNativeCarrierTypeIdentity))
			, NativeTypeIdentity(MoveTemp(InNativeTypeIdentity))
			, SemanticRole(MoveTemp(InSemanticRole))
		{
		}

		const FString& GetItemKey() const { return ItemKey; }
		const FString& GetIdentitySource() const { return IdentitySource; }
		const FString& GetNativeIdentity() const { return NativeIdentity; }
		const FString& GetStableBuiltinRole() const { return StableBuiltinRole; }
		const FString& GetNativeCarrierTypeIdentity() const { return NativeCarrierTypeIdentity; }
		const FString& GetNativeTypeIdentity() const { return NativeTypeIdentity; }
		const FString& GetSemanticRole() const { return SemanticRole; }

	private:
		FString ItemKey;
		FString IdentitySource;
		FString NativeIdentity;
		FString StableBuiltinRole;
		FString NativeCarrierTypeIdentity;
		FString NativeTypeIdentity;
		FString SemanticRole;
	};

	class FAnimationRigGraphPinIdentity final
	{
	public:
		FAnimationRigGraphPinIdentity() = default;
		FAnimationRigGraphPinIdentity(
			FString InPinKey,
			FString InOwnerItemKey,
			FString InNativeLabel,
			EAnimationRigGraphPinDirection InDirection,
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
		EAnimationRigGraphPinDirection GetDirection() const { return Direction; }
		const FString& GetNativeTypeIdentity() const { return NativeTypeIdentity; }

	private:
		FString PinKey;
		FString OwnerItemKey;
		FString NativeLabel;
		EAnimationRigGraphPinDirection Direction = EAnimationRigGraphPinDirection::Input;
		FString NativeTypeIdentity;
	};

	class FAnimationRigGraphRelationIdentity final
	{
	public:
		FAnimationRigGraphRelationIdentity() = default;
		FAnimationRigGraphRelationIdentity(
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
		const FString& GetNativeRelationIdentity() const { return NativeRelationIdentity; }
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

	class FAnimationRigGraphTopology final
	{
	public:
		FAnimationRigGraphTopology() = default;
		FAnimationRigGraphTopology(
			TArray<FAnimationRigGraphItemIdentity> InItems,
			TArray<FAnimationRigGraphPinIdentity> InPins,
			TArray<FAnimationRigGraphRelationIdentity> InRelations)
			: Items(MoveTemp(InItems))
			, Pins(MoveTemp(InPins))
			, Relations(MoveTemp(InRelations))
		{
		}

		const TArray<FAnimationRigGraphItemIdentity>& GetItems() const { return Items; }
		const TArray<FAnimationRigGraphPinIdentity>& GetPins() const { return Pins; }
		const TArray<FAnimationRigGraphRelationIdentity>& GetRelations() const { return Relations; }

	private:
		TArray<FAnimationRigGraphItemIdentity> Items;
		TArray<FAnimationRigGraphPinIdentity> Pins;
		TArray<FAnimationRigGraphRelationIdentity> Relations;
	};

	class FAnimationRigGraphAssetIdentity final
	{
	public:
		FAnimationRigGraphAssetIdentity() = default;
		FAnimationRigGraphAssetIdentity(FString InAssetKind, FString InAssetPath, FString InNativeClass)
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

	class FAnimationRigGraphLifecycleEvidence final
	{
	public:
		FAnimationRigGraphLifecycleEvidence(
			EAnimationRigGraphBarrierKind InBarrierKind,
			bool bInBarrierComplete,
			int32 InBarrierErrorCount,
			FString InBeforeRevision,
			FString InApplyEvidence,
			FString InBarrierEvidence,
			FString InSettleEvidence,
			bool bInSettleComplete,
			FString InReadbackRevision,
			FString InReadbackEvidence)
			: BarrierKind(InBarrierKind)
			, bBarrierComplete(bInBarrierComplete)
			, BarrierErrorCount(InBarrierErrorCount)
			, BeforeRevision(MoveTemp(InBeforeRevision))
			, ApplyEvidence(MoveTemp(InApplyEvidence))
			, BarrierEvidence(MoveTemp(InBarrierEvidence))
			, SettleEvidence(MoveTemp(InSettleEvidence))
			, bSettleComplete(bInSettleComplete)
			, ReadbackRevision(MoveTemp(InReadbackRevision))
			, ReadbackEvidence(MoveTemp(InReadbackEvidence))
		{
		}

		EAnimationRigGraphBarrierKind GetBarrierKind() const { return BarrierKind; }
		bool IsBarrierComplete() const { return bBarrierComplete; }
		int32 GetBarrierErrorCount() const { return BarrierErrorCount; }
		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetApplyEvidence() const { return ApplyEvidence; }
		const FString& GetBarrierEvidence() const { return BarrierEvidence; }
		const FString& GetSettleEvidence() const { return SettleEvidence; }
		bool IsSettleComplete() const { return bSettleComplete; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetReadbackEvidence() const { return ReadbackEvidence; }

	private:
		EAnimationRigGraphBarrierKind BarrierKind = EAnimationRigGraphBarrierKind::Compile;
		bool bBarrierComplete = false;
		int32 BarrierErrorCount = 0;
		FString BeforeRevision;
		FString ApplyEvidence;
		FString BarrierEvidence;
		FString SettleEvidence;
		bool bSettleComplete = false;
		FString ReadbackRevision;
		FString ReadbackEvidence;
	};

	class FAnimationRigGraphAdapterError final
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

	UEAGENTINTERFACE_API bool TryValidateAnimationRigGraphTopology(
		const FAnimationRigGraphTopology& Topology,
		FAnimationRigGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidateAnimationRigGraphAssetIdentity(
		const FAnimationRigGraphAssetIdentity& Identity,
		FAnimationRigGraphAdapterError& OutError);
	UEAGENTINTERFACE_API bool TryValidateAnimationRigGraphLifecycleEvidence(
		const FAnimationRigGraphLifecycleEvidence& Evidence,
		FAnimationRigGraphAdapterError& OutError);
}
