#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

#define UAI_BLUEPRINT_UMG_GRAPH_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	enum class EBlueprintUmgGraphPinDirection : uint8
	{
		Input,
		Output,
	};

	class FBlueprintUmgGraphItemIdentity final
	{
	public:
		FBlueprintUmgGraphItemIdentity() = default;
		FBlueprintUmgGraphItemIdentity(
			FString InItemKey,
			FString InNativeGuid,
			FString InStableBuiltinRole)
			: ItemKey(MoveTemp(InItemKey))
			, NativeGuid(MoveTemp(InNativeGuid))
			, StableBuiltinRole(MoveTemp(InStableBuiltinRole))
		{
		}

		const FString& GetItemKey() const { return ItemKey; }
		const FString& GetNativeGuid() const { return NativeGuid; }
		const FString& GetStableBuiltinRole() const
		{
			return StableBuiltinRole;
		}

	private:
		FString ItemKey;
		FString NativeGuid;
		FString StableBuiltinRole;
	};

	class FBlueprintUmgGraphPinIdentity final
	{
	public:
		FBlueprintUmgGraphPinIdentity() = default;
		FBlueprintUmgGraphPinIdentity(
			FString InPinKey,
			FString InOwnerItemKey,
			FString InNativeName,
			EBlueprintUmgGraphPinDirection InDirection)
			: PinKey(MoveTemp(InPinKey))
			, OwnerItemKey(MoveTemp(InOwnerItemKey))
			, NativeName(MoveTemp(InNativeName))
			, Direction(InDirection)
		{
		}

		const FString& GetPinKey() const { return PinKey; }
		const FString& GetOwnerItemKey() const { return OwnerItemKey; }
		const FString& GetNativeName() const { return NativeName; }
		EBlueprintUmgGraphPinDirection GetDirection() const
		{
			return Direction;
		}

	private:
		FString PinKey;
		FString OwnerItemKey;
		FString NativeName;
		EBlueprintUmgGraphPinDirection Direction =
			EBlueprintUmgGraphPinDirection::Input;
	};

	class FBlueprintUmgGraphLinkIdentity final
	{
	public:
		FBlueprintUmgGraphLinkIdentity() = default;
		FBlueprintUmgGraphLinkIdentity(
			FString InLinkKey,
			FString InFromPinKey,
			FString InToPinKey)
			: LinkKey(MoveTemp(InLinkKey))
			, FromPinKey(MoveTemp(InFromPinKey))
			, ToPinKey(MoveTemp(InToPinKey))
		{
		}

		const FString& GetLinkKey() const { return LinkKey; }
		const FString& GetFromPinKey() const { return FromPinKey; }
		const FString& GetToPinKey() const { return ToPinKey; }

	private:
		FString LinkKey;
		FString FromPinKey;
		FString ToPinKey;
	};

	class FBlueprintUmgGraphHierarchyRelation final
	{
	public:
		FBlueprintUmgGraphHierarchyRelation() = default;
		FBlueprintUmgGraphHierarchyRelation(
			FString InRelationKey,
			FString InItemKey,
			FString InParentKey,
			FString InOrderingKey)
			: RelationKey(MoveTemp(InRelationKey))
			, ItemKey(MoveTemp(InItemKey))
			, ParentKey(MoveTemp(InParentKey))
			, OrderingKey(MoveTemp(InOrderingKey))
		{
		}

		const FString& GetRelationKey() const { return RelationKey; }
		const FString& GetItemKey() const { return ItemKey; }
		const FString& GetParentKey() const { return ParentKey; }
		const FString& GetOrderingKey() const { return OrderingKey; }

	private:
		FString RelationKey;
		FString ItemKey;
		FString ParentKey;
		FString OrderingKey;
	};

	class FBlueprintUmgGraphBindingIdentity final
	{
	public:
		FBlueprintUmgGraphBindingIdentity() = default;
		FBlueprintUmgGraphBindingIdentity(
			FString InBindingKey,
			FString InSourceIdentity,
			FString InSourceField,
			FString InTargetIdentity,
			FString InTargetField)
			: BindingKey(MoveTemp(InBindingKey))
			, SourceIdentity(MoveTemp(InSourceIdentity))
			, SourceField(MoveTemp(InSourceField))
			, TargetIdentity(MoveTemp(InTargetIdentity))
			, TargetField(MoveTemp(InTargetField))
		{
		}

		const FString& GetBindingKey() const { return BindingKey; }
		const FString& GetSourceIdentity() const { return SourceIdentity; }
		const FString& GetSourceField() const { return SourceField; }
		const FString& GetTargetIdentity() const { return TargetIdentity; }
		const FString& GetTargetField() const { return TargetField; }

	private:
		FString BindingKey;
		FString SourceIdentity;
		FString SourceField;
		FString TargetIdentity;
		FString TargetField;
	};

	class FBlueprintUmgGraphTopology final
	{
	public:
		FBlueprintUmgGraphTopology() = default;
		FBlueprintUmgGraphTopology(
			FString InTargetKind,
			FString InTargetIdentity,
			FString InStructureIdentity,
			TArray<FBlueprintUmgGraphItemIdentity> InItems,
			TArray<FBlueprintUmgGraphPinIdentity> InPins,
			TArray<FBlueprintUmgGraphLinkIdentity> InLinks,
			TArray<FBlueprintUmgGraphHierarchyRelation> InHierarchy,
			TArray<FBlueprintUmgGraphBindingIdentity> InBindings)
			: TargetKind(MoveTemp(InTargetKind))
			, TargetIdentity(MoveTemp(InTargetIdentity))
			, StructureIdentity(MoveTemp(InStructureIdentity))
			, Items(MoveTemp(InItems))
			, Pins(MoveTemp(InPins))
			, Links(MoveTemp(InLinks))
			, Hierarchy(MoveTemp(InHierarchy))
			, Bindings(MoveTemp(InBindings))
		{
		}

		const FString& GetTargetKind() const { return TargetKind; }
		const FString& GetTargetIdentity() const { return TargetIdentity; }
		const FString& GetStructureIdentity() const
		{
			return StructureIdentity;
		}
		const TArray<FBlueprintUmgGraphItemIdentity>& GetItems() const
		{
			return Items;
		}
		const TArray<FBlueprintUmgGraphPinIdentity>& GetPins() const
		{
			return Pins;
		}
		const TArray<FBlueprintUmgGraphLinkIdentity>& GetLinks() const
		{
			return Links;
		}
		const TArray<FBlueprintUmgGraphHierarchyRelation>& GetHierarchy() const
		{
			return Hierarchy;
		}
		const TArray<FBlueprintUmgGraphBindingIdentity>& GetBindings() const
		{
			return Bindings;
		}

	private:
		FString TargetKind;
		FString TargetIdentity;
		FString StructureIdentity;
		TArray<FBlueprintUmgGraphItemIdentity> Items;
		TArray<FBlueprintUmgGraphPinIdentity> Pins;
		TArray<FBlueprintUmgGraphLinkIdentity> Links;
		TArray<FBlueprintUmgGraphHierarchyRelation> Hierarchy;
		TArray<FBlueprintUmgGraphBindingIdentity> Bindings;
	};

	class FBlueprintUmgGraphLifecycleEvidence final
	{
	public:
		FBlueprintUmgGraphLifecycleEvidence(
			FString InBeforeRevision,
			FString InApplyEvidence,
			FString InSettleEvidence,
			FString InReadbackRevision,
			FString InReadbackEvidence,
			bool bInCompileComplete,
			int32 InErrorCount)
			: BeforeRevision(MoveTemp(InBeforeRevision))
			, ApplyEvidence(MoveTemp(InApplyEvidence))
			, SettleEvidence(MoveTemp(InSettleEvidence))
			, ReadbackRevision(MoveTemp(InReadbackRevision))
			, ReadbackEvidence(MoveTemp(InReadbackEvidence))
			, bCompileComplete(bInCompileComplete)
			, ErrorCount(InErrorCount)
		{
		}

		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetApplyEvidence() const { return ApplyEvidence; }
		const FString& GetSettleEvidence() const { return SettleEvidence; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetReadbackEvidence() const { return ReadbackEvidence; }
		bool IsCompileComplete() const { return bCompileComplete; }
		int32 GetErrorCount() const { return ErrorCount; }

	private:
		FString BeforeRevision;
		FString ApplyEvidence;
		FString SettleEvidence;
		FString ReadbackRevision;
		FString ReadbackEvidence;
		bool bCompileComplete = false;
		int32 ErrorCount = 0;
	};

	using FBlueprintUmgGraphAdapterError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryValidateBlueprintUmgGraphTopology(
		const FBlueprintUmgGraphTopology& Topology,
		FBlueprintUmgGraphAdapterError& OutError);

	UEAGENTINTERFACE_API bool TryValidateBlueprintUmgGraphLifecycleEvidence(
		const FBlueprintUmgGraphLifecycleEvidence& Evidence,
		FBlueprintUmgGraphAdapterError& OutError);
}
