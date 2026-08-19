#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;

	enum class ESemanticUnitKind : uint8
	{
		WorkspaceIndex,
		TargetState,
		EntityState,
		PropertySet,
		StructureState,
		ItemState,
		RelationSet,
		OrderingState,
		LayoutState,
		CollectionState,
		DependencyState,
		ExternalPayloadState,
		OpaqueExtension,
	};

	enum class EWorkspaceSourceMode : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	class UEAGENTINTERFACE_API FSemanticUnit final
	{
	public:
		FSemanticUnit(
			FString InUnitId,
			ESemanticUnitKind InUnitKind,
			FString InOwnerRef,
			FString InSemanticTypeRef,
			FString InRole,
			FString InFieldSetRef,
			TMap<FString, FUeAgentCanonicalValueRef> InValues,
			TArray<FString> InItemRefs,
			TArray<FString> InRelationRefs,
			FString InOrderingRef,
			FString InLayoutRef,
			TArray<FString> InDependencyRefs,
			TArray<FString> InProfileVisibility,
			FString InSourceProvenance,
			FString InTargetIdentityKey = FString());

		const FString& GetUnitId() const;
		ESemanticUnitKind GetUnitKind() const;
		const FString& GetOwnerRef() const;
		const FString& GetSemanticTypeRef() const;
		const FString& GetRole() const;
		const FString& GetFieldSetRef() const;
		const TMap<FString, FUeAgentCanonicalValueRef>& GetValues() const;
		const TArray<FString>& GetItemRefs() const;
		const TArray<FString>& GetRelationRefs() const;
		const FString& GetOrderingRef() const;
		const FString& GetLayoutRef() const;
		const TArray<FString>& GetDependencyRefs() const;
		const TArray<FString>& GetProfileVisibility() const;
		const FString& GetSourceProvenance() const;
		const FString& GetTargetIdentityKey() const;
		bool IsValid() const;

	private:
		FString UnitId;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::OpaqueExtension;
		FString OwnerRef;
		FString SemanticTypeRef;
		FString Role;
		FString FieldSetRef;
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		TArray<FString> ItemRefs;
		TArray<FString> RelationRefs;
		FString OrderingRef;
		FString LayoutRef;
		TArray<FString> DependencyRefs;
		TArray<FString> ProfileVisibility;
		FString SourceProvenance;
		FString TargetIdentityKey;
		bool bValid = false;
	};

	class FAuthoringModel;
	using FAuthoringModelPtr = TSharedPtr<const FAuthoringModel>;
	using FAuthoringModelRef = TSharedRef<const FAuthoringModel>;

	class UEAGENTINTERFACE_API FAuthoringModel final
	{
	public:
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetProfileId() const;
		const FString& GetBaseRevision() const;
		const TArray<FSemanticUnit>& GetUnits() const;
		const TArray<FString>& GetTargetUnitIds() const;
		const FSemanticUnit* FindUnit(const FString& UnitId) const;
		const FString& GetSemanticFingerprint() const;

	private:
		FAuthoringModel(
			FString InContractId,
			int32 InContractVersion,
			FString InProfileId,
			FString InBaseRevision,
			TArray<FSemanticUnit> InUnits,
			TArray<FString> InTargetUnitIds,
			FString InSemanticFingerprint);

		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		FString BaseRevision;
		TArray<FSemanticUnit> Units;
		TArray<FString> TargetUnitIds;
		TMap<FString, int32> UnitIndex;
		FString SemanticFingerprint;

		friend UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
			FString,
			int32,
			FString,
			FString,
			TArray<FSemanticUnit>,
			TArray<FString>,
			FUeAgentResourceTracker&,
			FAuthoringModelPtr&,
			FWorkspaceError&);
	};

	struct UEAGENTINTERFACE_API FWorkspaceUnitInventoryEntry
	{
		FString UnitId;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::OpaqueExtension;
		FString OwnerRef;
		FString SemanticTypeRef;
		FString Role;
		FString LocalHandle;
		FString LogicalPath;
		FString ContentHash;
	};

	struct UEAGENTINTERFACE_API FLlmWorkspaceIndex
	{
		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		TArray<FString> TargetUnitIds;
		TArray<FString> SurfaceUnitIds;
		TMap<ESemanticUnitKind, int32> UnitKindCounts;
	};

	struct UEAGENTINTERFACE_API FMachineWorkspaceManifest
	{
		FString WorkspaceSchema;
		FString ContractId;
		int32 ContractVersion = 0;
		int32 LayoutVersion = 0;
		FString ProfileId;
		FString SemanticFingerprint;
		TArray<FWorkspaceUnitInventoryEntry> UnitInventory;
		TMap<FString, FString> LocalHandleMapping;
		FString BaseRevision;
		TMap<FString, TArray<FString>> PhysicalFiles;
	};

	class FWorkspaceLayoutPlan;
	using FWorkspaceLayoutPlanPtr =
		TSharedPtr<const FWorkspaceLayoutPlan>;

	class UEAGENTINTERFACE_API FWorkspaceLayoutPlan final
	{
	public:
		const FAuthoringModelRef& GetModel() const;
		int32 GetLayoutVersion() const;
		EWorkspaceSourceMode GetSourceMode() const;
		const FString& GetSemanticFingerprint() const;
		const TMap<FString, FString>& GetLogicalPlacements() const;
		const TMap<FString, TArray<FString>>& GetPhysicalFiles() const;
		const FLlmWorkspaceIndex& GetLlmIndex() const;
		const FMachineWorkspaceManifest& GetMachineManifest() const;
		const TMap<FString, int64>& GetTelemetry() const;

	private:
		FWorkspaceLayoutPlan(
			FAuthoringModelRef InModel,
			int32 InLayoutVersion,
			EWorkspaceSourceMode InSourceMode,
			TMap<FString, FString> InLogicalPlacements,
			TMap<FString, TArray<FString>> InPhysicalFiles,
			FLlmWorkspaceIndex InLlmIndex,
			FMachineWorkspaceManifest InMachineManifest,
			TMap<FString, int64> InTelemetry);

		FAuthoringModelRef Model;
		int32 LayoutVersion = 0;
		EWorkspaceSourceMode SourceMode =
			EWorkspaceSourceMode::Directory;
		TMap<FString, FString> LogicalPlacements;
		TMap<FString, TArray<FString>> PhysicalFiles;
		FLlmWorkspaceIndex LlmIndex;
		FMachineWorkspaceManifest MachineManifest;
		TMap<FString, int64> Telemetry;

		friend UEAGENTINTERFACE_API bool TryPlanWorkspaceLayout(
			FAuthoringModelRef,
			int32,
			EWorkspaceSourceMode,
			TMap<FString, int64>,
			FUeAgentResourceTracker&,
			FWorkspaceLayoutPlanPtr&,
			FWorkspaceError&);
	};

	UEAGENTINTERFACE_API FString GetSemanticWorkspaceSchemaId();

	UEAGENTINTERFACE_API FString LexToString(
		ESemanticUnitKind UnitKind);

	UEAGENTINTERFACE_API FString StableLocalHandle(
		const FString& UnitId,
		ESemanticUnitKind UnitKind);

	UEAGENTINTERFACE_API FString SemanticUnitContentHash(
		const FSemanticUnit& Unit);

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8);

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8);

	UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
		FString ContractId,
		int32 ContractVersion,
		FString ProfileId,
		FString BaseRevision,
		TArray<FSemanticUnit> Units,
		TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryPlanWorkspaceLayout(
		FAuthoringModelRef Model,
		int32 LayoutVersion,
		EWorkspaceSourceMode SourceMode,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FWorkspaceLayoutPlanPtr& OutPlan,
		FWorkspaceError& OutError);
}
