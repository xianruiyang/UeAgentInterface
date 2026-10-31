#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

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
			FString InTargetIdentityKey = FString(),
			TArray<FString> InCategoryPath = {});

		const FString& GetUnitId() const;
		ESemanticUnitKind GetUnitKind() const;
		const FString& GetOwnerRef() const;
		const FString& GetSemanticTypeRef() const;
		const FString& GetRole() const;
		const FString& GetFieldSetRef() const;
		const TMap<FString, FUeAgentCanonicalValueRef>& GetValues() const;
		FSemanticUnit WithValues(TMap<FString, FUeAgentCanonicalValueRef> NewValues) const
		{
			return FSemanticUnit(UnitId, UnitKind, OwnerRef, SemanticTypeRef, Role, FieldSetRef,
				MoveTemp(NewValues), ItemRefs, RelationRefs, OrderingRef, LayoutRef, DependencyRefs,
				ProfileVisibility, SourceProvenance, TargetIdentityKey, CategoryPath);
		}
		const TArray<FString>& GetItemRefs() const;
		const TArray<FString>& GetRelationRefs() const;
		const FString& GetOrderingRef() const;
		const FString& GetLayoutRef() const;
		const TArray<FString>& GetDependencyRefs() const;
		const TArray<FString>& GetProfileVisibility() const;
		const FString& GetSourceProvenance() const;
		const FString& GetTargetIdentityKey() const;
		const TArray<FString>& GetCategoryPath() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		TArray<FString> CategoryPath;
		Contract::FDiagnostic ValidationDiagnostic;
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
		TArray<const FSemanticUnit*> FindUnitsForTargetIdentity(const FString& TargetIdentity) const;
		const FString* FindReadablePropertyDefinitionName(const FString& UnitId) const;
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
		TMap<uint32, TArray<int32>> TargetIdentityIndex;
		TMap<FString, FString> ReadablePropertyDefinitionNames;
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
		// Internal unit id -> short readable ref used only in model-visible HJSON.
		TMap<FString, FString> PublicRefMapping;
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
		const TMap<FString, FString>& GetPhysicalPlacements() const;
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
			TMap<FString, FString> InPhysicalPlacements,
			TMap<FString, TArray<FString>> InPhysicalFiles,
			FLlmWorkspaceIndex InLlmIndex,
			FMachineWorkspaceManifest InMachineManifest,
			TMap<FString, int64> InTelemetry);

		FAuthoringModelRef Model;
		int32 LayoutVersion = 0;
		EWorkspaceSourceMode SourceMode =
			EWorkspaceSourceMode::Directory;
		TMap<FString, FString> LogicalPlacements;
		TMap<FString, FString> PhysicalPlacements;
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

	// Format a business name; embedded hex runs and non-ASCII letters are valid.
	UEAGENTINTERFACE_API FString MakeReadableSemanticAlias(
		const FString& Value);

	// For fallback identity/source fields only, not authoritative business names.
	UEAGENTINTERFACE_API bool IsOpaqueSemanticIdentity(const FString& Value);

	// File navigation follows business ownership, independently of public reference handles.
	bool TryBuildAuthoringContentDirectories(const FAuthoringModel& Model,
		const TMap<FString, FString>& Placements, TMap<FString, FString>& OutDirectories, FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryHashSemanticUnitContent(
		const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError);
	/** Exact strings/keys and container membership; only number/bool leaves may differ. */
	bool HaveOnlyNumericOrBooleanValueChanges(const StateSemantic::FUeAgentCanonicalValue& A,
		const StateSemantic::FUeAgentCanonicalValue& B);

	UEAGENTINTERFACE_API bool TrySerializeSemanticHjsonObject(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceUnit = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticHjsonObject(
		const TSharedRef<FJsonObject>& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceUnit = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());
	/** Hash of the canonical row used by directory workspace inventory. */
	UEAGENTINTERFACE_API bool TryHashSemanticUnitRow(
		const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	/** Builds a fresh editable row using the same projection as serialization. */
	UEAGENTINTERFACE_API bool TryBuildSemanticModelUnitRow(
		const FSemanticUnit& Unit, TSharedPtr<FJsonObject>& OutRow,
		FWorkspaceError& OutError,
		const TMap<FString, FString>* PublicRefs = nullptr,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticModelUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticModelUnitRow(
		const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

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

	UEAGENTINTERFACE_API bool IsModelVisibleSemanticWorkspacePath(
		const FString& RelativePath);
}
