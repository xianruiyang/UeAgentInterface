#pragma once

#include "CoreMinimal.h"

#define UAI_MATERIAL_NIAGARA_GRAPH_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	enum class EMaterialNiagaraGraphPinDirection : uint8
	{
		Input,
		Output,
	};

	enum class EMaterialNiagaraGraphParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
		StaticSwitch,
		StaticComponentMask,
	};

	class FMaterialNiagaraGraphParameterIdentity final
	{
	public:
		FMaterialNiagaraGraphParameterIdentity() = default;
		FMaterialNiagaraGraphParameterIdentity(
			FString InParameterName,
			FString InParameterAssociation,
			int32 InParameterIndex,
			EMaterialNiagaraGraphParameterType InParameterType)
			: ParameterName(MoveTemp(InParameterName))
			, ParameterAssociation(MoveTemp(InParameterAssociation))
			, ParameterIndex(InParameterIndex)
			, ParameterType(InParameterType)
		{
		}

		const FString& GetParameterName() const { return ParameterName; }
		const FString& GetParameterAssociation() const
		{
			return ParameterAssociation;
		}
		int32 GetParameterIndex() const { return ParameterIndex; }
		EMaterialNiagaraGraphParameterType GetParameterType() const
		{
			return ParameterType;
		}
		FString GetParameterKey() const;

	private:
		FString ParameterName;
		FString ParameterAssociation;
		int32 ParameterIndex = INDEX_NONE;
		EMaterialNiagaraGraphParameterType ParameterType =
			EMaterialNiagaraGraphParameterType::Scalar;
	};

	class FMaterialNiagaraGraphItemIdentity final
	{
	public:
		FMaterialNiagaraGraphItemIdentity() = default;
		FMaterialNiagaraGraphItemIdentity(
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

	class FMaterialNiagaraGraphPinIdentity final
	{
	public:
		FMaterialNiagaraGraphPinIdentity() = default;
		FMaterialNiagaraGraphPinIdentity(
			FString InPinKey,
			FString InOwnerItemKey,
			FString InNativeName,
			int32 InNativeIndex,
			EMaterialNiagaraGraphPinDirection InDirection)
			: PinKey(MoveTemp(InPinKey))
			, OwnerItemKey(MoveTemp(InOwnerItemKey))
			, NativeName(MoveTemp(InNativeName))
			, NativeIndex(InNativeIndex)
			, Direction(InDirection)
		{
		}

		const FString& GetPinKey() const { return PinKey; }
		const FString& GetOwnerItemKey() const { return OwnerItemKey; }
		const FString& GetNativeName() const { return NativeName; }
		int32 GetNativeIndex() const { return NativeIndex; }
		EMaterialNiagaraGraphPinDirection GetDirection() const
		{
			return Direction;
		}

	private:
		FString PinKey;
		FString OwnerItemKey;
		FString NativeName;
		int32 NativeIndex = INDEX_NONE;
		EMaterialNiagaraGraphPinDirection Direction =
			EMaterialNiagaraGraphPinDirection::Input;
	};

	class FMaterialNiagaraGraphLinkIdentity final
	{
	public:
		FMaterialNiagaraGraphLinkIdentity() = default;
		FMaterialNiagaraGraphLinkIdentity(
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

	class FMaterialNiagaraGraphOrderingIdentity final
	{
	public:
		FMaterialNiagaraGraphOrderingIdentity() = default;
		FMaterialNiagaraGraphOrderingIdentity(
			FString InOrderingKey,
			FString InItemKey,
			FString InPredecessorItemKey)
			: OrderingKey(MoveTemp(InOrderingKey))
			, ItemKey(MoveTemp(InItemKey))
			, PredecessorItemKey(MoveTemp(InPredecessorItemKey))
		{
		}

		const FString& GetOrderingKey() const { return OrderingKey; }
		const FString& GetItemKey() const { return ItemKey; }
		const FString& GetPredecessorItemKey() const
		{
			return PredecessorItemKey;
		}

	private:
		FString OrderingKey;
		FString ItemKey;
		FString PredecessorItemKey;
	};

	class FMaterialNiagaraGraphTopology final
	{
	public:
		FMaterialNiagaraGraphTopology() = default;
		FMaterialNiagaraGraphTopology(
			FString InTargetKind,
			FString InTargetIdentity,
			FString InStructureIdentity,
			TArray<FMaterialNiagaraGraphItemIdentity> InItems,
			TArray<FMaterialNiagaraGraphPinIdentity> InPins,
			TArray<FMaterialNiagaraGraphLinkIdentity> InLinks,
			TArray<FMaterialNiagaraGraphOrderingIdentity> InOrdering)
			: TargetKind(MoveTemp(InTargetKind))
			, TargetIdentity(MoveTemp(InTargetIdentity))
			, StructureIdentity(MoveTemp(InStructureIdentity))
			, Items(MoveTemp(InItems))
			, Pins(MoveTemp(InPins))
			, Links(MoveTemp(InLinks))
			, Ordering(MoveTemp(InOrdering))
		{
		}

		const FString& GetTargetKind() const { return TargetKind; }
		const FString& GetTargetIdentity() const { return TargetIdentity; }
		const FString& GetStructureIdentity() const
		{
			return StructureIdentity;
		}
		const TArray<FMaterialNiagaraGraphItemIdentity>& GetItems() const
		{
			return Items;
		}
		const TArray<FMaterialNiagaraGraphPinIdentity>& GetPins() const
		{
			return Pins;
		}
		const TArray<FMaterialNiagaraGraphLinkIdentity>& GetLinks() const
		{
			return Links;
		}
		const TArray<FMaterialNiagaraGraphOrderingIdentity>&
		GetOrdering() const
		{
			return Ordering;
		}

	private:
		FString TargetKind;
		FString TargetIdentity;
		FString StructureIdentity;
		TArray<FMaterialNiagaraGraphItemIdentity> Items;
		TArray<FMaterialNiagaraGraphPinIdentity> Pins;
		TArray<FMaterialNiagaraGraphLinkIdentity> Links;
		TArray<FMaterialNiagaraGraphOrderingIdentity> Ordering;
	};

	class FMaterialNiagaraGraphLifecycleEvidence final
	{
	public:
		FMaterialNiagaraGraphLifecycleEvidence(
			FString InBeforeRevision,
			FString InApplyEvidence,
			FString InSettleEvidence,
			FString InReadbackRevision,
			FString InReadbackEvidence,
			bool bInCompilationComplete,
			int32 InCompileErrorCount,
			int32 InStackIssueCount,
			bool bInStackCaptureComplete,
			bool bInCompoundCaptureComplete)
			: BeforeRevision(MoveTemp(InBeforeRevision))
			, ApplyEvidence(MoveTemp(InApplyEvidence))
			, SettleEvidence(MoveTemp(InSettleEvidence))
			, ReadbackRevision(MoveTemp(InReadbackRevision))
			, ReadbackEvidence(MoveTemp(InReadbackEvidence))
			, bCompilationComplete(bInCompilationComplete)
			, CompileErrorCount(InCompileErrorCount)
			, StackIssueCount(InStackIssueCount)
			, bStackCaptureComplete(bInStackCaptureComplete)
			, bCompoundCaptureComplete(bInCompoundCaptureComplete)
		{
		}

		const FString& GetBeforeRevision() const { return BeforeRevision; }
		const FString& GetApplyEvidence() const { return ApplyEvidence; }
		const FString& GetSettleEvidence() const { return SettleEvidence; }
		const FString& GetReadbackRevision() const { return ReadbackRevision; }
		const FString& GetReadbackEvidence() const { return ReadbackEvidence; }
		bool IsCompilationComplete() const { return bCompilationComplete; }
		int32 GetCompileErrorCount() const { return CompileErrorCount; }
		int32 GetStackIssueCount() const { return StackIssueCount; }
		bool IsStackCaptureComplete() const { return bStackCaptureComplete; }
		bool IsCompoundCaptureComplete() const
		{
			return bCompoundCaptureComplete;
		}

	private:
		FString BeforeRevision;
		FString ApplyEvidence;
		FString SettleEvidence;
		FString ReadbackRevision;
		FString ReadbackEvidence;
		bool bCompilationComplete = false;
		int32 CompileErrorCount = 0;
		int32 StackIssueCount = 0;
		bool bStackCaptureComplete = false;
		bool bCompoundCaptureComplete = false;
	};

	class FMaterialNiagaraGraphAdapterError final
	{
	public:
		FMaterialNiagaraGraphAdapterError() = default;
		FMaterialNiagaraGraphAdapterError(
			FString InCode,
			FString InDetail)
			: Code(MoveTemp(InCode)), Detail(MoveTemp(InDetail))
		{
		}

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

	UEAGENTINTERFACE_API bool TryValidateMaterialNiagaraGraphTopology(
		const FMaterialNiagaraGraphTopology& Topology,
		FMaterialNiagaraGraphAdapterError& OutError);

	UEAGENTINTERFACE_API bool
	TryValidateMaterialNiagaraGraphParameterIdentities(
		const TArray<FMaterialNiagaraGraphParameterIdentity>& Parameters,
		FMaterialNiagaraGraphAdapterError& OutError);

	UEAGENTINTERFACE_API bool
	TryValidateMaterialNiagaraGraphLifecycleEvidence(
		const FMaterialNiagaraGraphLifecycleEvidence& Evidence,
		FMaterialNiagaraGraphAdapterError& OutError);

	UEAGENTINTERFACE_API bool
	TryValidateMaterialNiagaraGraphCompoundCapture(
		bool bCaptureSucceeded,
		const FString& DelegatedErrorCode,
		const FString& DelegatedErrorDetail,
		FMaterialNiagaraGraphAdapterError& OutError);
}
