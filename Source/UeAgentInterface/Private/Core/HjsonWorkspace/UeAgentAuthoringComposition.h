#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Templates/Function.h"

namespace UE::UAI::HjsonWorkspace
{
	class FHjsonNode;
	enum class EAuthoringSurfaceKind : uint8
	{
		Properties,
		Collection,
		Structure,
		Items,
		Relations,
		Timeline,
		Hierarchy,
		ExternalPayload,
	};

	struct UEAGENTINTERFACE_API FCompositionSurfaceContract
	{
		FString SurfaceId;
		EAuthoringSurfaceKind SurfaceKind =
			EAuthoringSurfaceKind::Properties;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::PropertySet;
	};

	enum class ECompositionSemanticRouteMatch : uint8
	{
		Exact,
		Suffix,
		Contains,
		Default,
	};

	enum class ECompositionSemanticRouteRole : uint8
	{
		Undeclared,
		TargetValues,
		Graph,
		Item,
		// A graph node may be owned by another graph node rather than the graph itself.
		GraphItem,
		Property,
		Relation,
		Document,
	};

	struct UEAGENTINTERFACE_API FCompositionSemanticRouteContract
	{
		ECompositionSemanticRouteMatch Match =
			ECompositionSemanticRouteMatch::Exact;
		FString Pattern;
		FString SurfaceId;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::PropertySet;
		ECompositionSemanticRouteRole Role =
			ECompositionSemanticRouteRole::Undeclared;
		int32 Priority = 0;
	};

	struct UEAGENTINTERFACE_API FTargetCompositionContract
	{
		FString SchemaId;
		FString TargetKind;
		TArray<FCompositionSurfaceContract> Surfaces;
		TArray<FCompositionSemanticRouteContract> SemanticRoutes;
		// Structured document values that retain their field name rather than
		// becoming anonymous collections. Keys are exact source semantic IDs.
		TMap<FString, TArray<FString>> DocumentValueFields;
	};

	class UEAGENTINTERFACE_API FTargetCompositionRegistry final
	{
	public:
		static FTargetCompositionRegistry& Get();

		bool RegisterContract(
			FTargetCompositionContract Contract,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool RegisterContracts(
			TArray<FTargetCompositionContract> InContracts,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindContract(
			const FString& TargetKind,
			FTargetCompositionContract& OutContract) const;
		TArray<FTargetCompositionContract> ListContracts() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FTargetCompositionContract> Contracts;
	};

	struct UEAGENTINTERFACE_API FCompositionRequest
	{
		FString TargetKind;
		FString ProfileId;
		FString BaseRevision;
		FString SourceProvenance;
		TArray<UeAgentAuthoringState::FStateEntry> Entries;
	};

	struct UEAGENTINTERFACE_API FCanonicalCompositionRequest
	{
		FString TargetKind;
		FString ProfileId;
		FString BaseRevision;
		FString SourceProvenance;
		TArray<UE::UAI::StateSemantic::FUeAgentStateEntry> Entries;
	};

	struct UEAGENTINTERFACE_API FCompositionSnapshotBinding
	{
		FString SnapshotId;
		UE::UAI::StateSemantic::FUeAgentStateRevision Revision;
		FString EvidenceId;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API FString
	GetTargetCompositionContractSchemaId();

	UEAGENTINTERFACE_API bool ValidateTargetCompositionContract(
		const FTargetCompositionContract& Contract,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	RegisterGraphTreeTargetCompositionContracts(UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	RegisterOtherTargetCompositionContracts(UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryResolveTargetCompositionSemanticRoute(
		const FString& TargetKind,
		const FString& SemanticId,
		FCompositionSemanticRouteContract& OutRoute);

	UEAGENTINTERFACE_API FString ResolveTargetCompositionKind(
		const FString& SourceTargetKind,
		const FString& SourceTypeId);

	UEAGENTINTERFACE_API bool TryComposeAuthoringModel(
		const FCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryComposeCanonicalAuthoringModel(
		const FCanonicalCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	/** Source addresses produced by the same document decomposition used for apply.
	 * They are transient projection metadata, never editable HJSON or state values. */
	struct FDocumentValueSourceBinding
	{
		FString DocumentIdentity;
		FString UnitId;
		FString DocumentPointer;
		FString UnitValuePointer;
	};
	using FAuthoringValueSourceResolver = TFunction<const FHjsonNode*(const FString&, const FString&)>;

	/** Resolve the existing interned property catalog for the shared typed comparison.
	 * Values without a declaration retain exact canonical semantics. */
	UEAGENTINTERFACE_API bool TryCompareWorkspaceValues(
		const FAuthoringModel& Model, const StateSemantic::FUeAgentCanonicalValue* Type,
		const StateSemantic::FUeAgentCanonicalValue* Definition,
		const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& After,
		StateSemantic::CanonicalJson::FValueComparison& OutComparison, FWorkspaceError& OutError);
	UEAGENTINTERFACE_API bool TryCompareWorkspaceValues(
		const FAuthoringModel& Model, const StateSemantic::FUeAgentCanonicalValue* Type,
		const StateSemantic::FUeAgentCanonicalValue* Definition,
		const StateSemantic::FUeAgentCanonicalValueRef& Before,
		const StateSemantic::FUeAgentCanonicalValueRef& After,
		StateSemantic::CanonicalJson::FValueComparison& OutComparison, FWorkspaceError& OutError);

	/** Field delta in the authored semantic values, using the shared value semantics.
	 * A missing baseline selects all supplied fields; no native mutation or defaults are invented. */
	UEAGENTINTERFACE_API bool TryCollectChangedUnitValueFields(
		const FSemanticUnit* Before, const FSemanticUnit& Edited,
		ContextResource::FUeAgentResourceTracker& Resources,
		TSet<FString>& OutFields, FWorkspaceError& OutError,
		const FAuthoringModel* Model = nullptr);

	/** Inverse of document decomposition for existing unit values; no structure mutation. */
	UEAGENTINTERFACE_API bool TryProjectDocumentValueChanges(
		const StateSemantic::FUeAgentCanonicalSnapshotRef& Snapshot,
		const FAuthoringModelRef& CurrentModel,
		const FAuthoringModelRef& EditedModel,
		const TOptional<TArray<FString>>& EditedUnitIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		TMap<FString, StateSemantic::FUeAgentCanonicalValuePtr>& OutDocuments,
		TSet<FString>& OutDocumentUnitIds,
		FWorkspaceError& OutError,
		const TMap<FString, TSet<FString>>* ResolvedKeyedOperationRoots = nullptr,
		TArray<FDocumentValueSourceBinding>* OutSourceBindings = nullptr,
		const FAuthoringValueSourceResolver& FindValueSource = {});

	inline constexpr const TCHAR* CanonicalAuthoringTargetIdentitySelector =
		TEXT("authoring_target_identity_key");

	/**
	 * Adapts the established authoring-state capture to the canonical state
	 * model used by semantic workspace import. Export and import must call this
	 * same owner so identity and revision material cannot drift.
	 */
	UEAGENTINTERFACE_API bool TryBuildCanonicalAuthoringCapture(
		const FString& TargetKind,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError, const FString& TargetKind = FString());

	UEAGENTINTERFACE_API bool TryExportCompositionWorkspace(
		const FString& TargetKind,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const FString& WorkspaceRoot,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult);

	/** Reuses composition's type/value and definition projection for nested reflected records.
	 * Native scope binds recursive rows to their live reflected schema instead of a definition catalog;
	 * in that mode definition references are omitted and OutDefinitions is empty. */
	struct UEAGENTINTERFACE_API FReadablePropertyAddress
	{
		FString Category;
		FString Key;
		FString Type;
		FString Definition;
	};
	UEAGENTINTERFACE_API FReadablePropertyAddress DescribeReadableProperty(
		const FSemanticUnit& Unit, const FAuthoringModel& Model);
	UEAGENTINTERFACE_API bool TryComposeReflectedPropertyTemplate(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel, FWorkspaceError& OutError);

	/** Reuses composition's type/value and definition projection for nested reflected records. */
	UEAGENTINTERFACE_API bool TryProjectReflectedRecord(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		TSharedPtr<FJsonObject>& OutProperties,
		TSharedPtr<FJsonObject>& OutDefinitions,
		FWorkspaceError& OutError,
		bool bGroupByCategory = false, bool bNativeScope = false);

	/** Projects a captured native value to the editable shape used by composition.
	 * The complete snapshot remains authoritative for preconditions and native preservation. */
	UEAGENTINTERFACE_API bool TryProjectAuthoringCaptureValue(
		const StateSemantic::FUeAgentStateEntry& Entry,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError);

	/** Uses the same ordinary value projection as export, without resolving edit identities. */
	UEAGENTINTERFACE_API bool TryProjectDirectPropertyValue(
		const StateSemantic::FUeAgentCanonicalValueRef& Value, const FAuthoringModel& Model,
		const StateSemantic::FUeAgentCanonicalValue* ValueDefinition,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue, FWorkspaceError& OutError,
		bool bNativeScope = false, const StateSemantic::FUeAgentCanonicalValue* ValueType = nullptr);
}
