#pragma once
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentGraphReferenceBinding.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"

class UBlueprint;
namespace UeAgentAuthoringState { struct FReadRequest; }
namespace UeAgentAuthoringDiagnostics { class FScopedSourceLocations; }


namespace UE::UAI::Composition::HjsonImport
{
	class FPreparedAuthoringImport;
	struct FSemanticWorkspaceChangeProjection;
}

namespace UE::UAI::HjsonWorkspace::DomainAuthoring
{
	struct UEAGENTINTERFACE_API FDomainHjsonDocumentContract final
	{
		FDomainHjsonDocumentContract(
			FString InDomainId,
			FString InSchemaId,
			FString InRootTypeIdentity);
		FDomainHjsonDocumentContract(
			FString InDomainId,
			FString InSchemaId,
			TArray<FString> InRootTypeIdentities);

		bool IsValid() const;
		bool SupportsRootType(const FString& RootTypeIdentity) const;

		FString DomainId;
		FString SchemaId;
		FString RootTypeIdentity;
		TArray<FString> RootTypeIdentities;
	};

	enum class EDomainHjsonDocumentProjection : uint8 { Json, Ast };

	struct UEAGENTINTERFACE_API FResolvedDomainHjsonDocument final
	{
		FString TargetIdentity;
		FString ExpectedRevision;
		FString Carrier;
		FString SourceUnit;
		FString BaseRevision;
		FString EntryFile;
		FString SourceFingerprint;
		FString SemanticFingerprint;
		TSharedPtr<FJsonObject> Params;
		TSharedPtr<FJsonObject> Root;
		FHjsonDocumentPtr AstDocument;
		const FHjsonNode* AstRoot = nullptr;
		FResolvedSemanticWorkspacePtr SemanticWorkspace;
		bool bSemanticWorkspaceDirectory = false;
		Contract::FSourceLocation ResolveSource(const FString& Pointer, bool* bExact = nullptr) const;
		// PointerPrefix identifies an extracted native parameter object in this document.
		void LocateDiagnostic(Contract::FDiagnostic& Diagnostic, const FString& PointerPrefix = FString()) const;
	};

	// Preserve the parsed source across initial/file import adapters and nested
	// native calls. No metadata is inserted into the authored JSON object.
	class UEAGENTINTERFACE_API FScopedDomainHjsonDiagnosticSource final
	{
	public:
		FScopedDomainHjsonDiagnosticSource(const FResolvedDomainHjsonDocument& Document, FWorkspaceError& Error,
			FString PointerPrefix = FString());
		~FScopedDomainHjsonDiagnosticSource();
		FScopedDomainHjsonDiagnosticSource(const FScopedDomainHjsonDiagnosticSource&) = delete;
		FScopedDomainHjsonDiagnosticSource& operator=(const FScopedDomainHjsonDiagnosticSource&) = delete;
	private:
		const FResolvedDomainHjsonDocument& Document;
		FWorkspaceError& Error;
		FString PointerPrefix;
		TUniquePtr<UeAgentAuthoringDiagnostics::FScopedSourceLocations> Locations;
	};

	struct UEAGENTINTERFACE_API FDomainHjsonImportLifecycleReceipt final
	{
		bool IsComplete() const;

		bool bMutationApplied = false;
		// Completed native update for this operation (compile, rebuild, refresh,
		// or not required by its native contract); not stored VM freshness.
		bool bNativeUpdateSettled = false;
		bool bPackageSaved = false;
		FString PersistenceReceiptId;
	};

	struct UEAGENTINTERFACE_API FRootPropertyImportContract final
	{
		FString ProviderId;
		FString TargetIdentity;
		FString ObjectPath;
		FString RequestPrefix;
		FString PrepareFailedErrorCode;
		FString ApplyFailedErrorCode;
		FString ReadbackPayloadErrorCode;
		FString CompileNotSettledErrorCode;
		// Same native read-request owner as export, regenerated after mutation.
		TFunction<bool(TArray<UeAgentAuthoringState::FReadRequest>&, Contract::FDiagnostic&)> BuildFinalReadRequests;
	};

	UEAGENTINTERFACE_API bool TryReadNativeMutationPayload(const FString& Target,
		const FString& Payload, TSharedPtr<FJsonObject>& OutMutation, FWorkspaceError& OutError,
		FDomainHjsonImportLifecycleReceipt* OutReceipt = nullptr);

	struct UEAGENTINTERFACE_API FRootPropertyApplyOutcome final
	{
		bool bApplied = false;
		TArray<FGraphNodeReferenceBinding> GraphReferenceBindings;
		TFunction<bool(UeAgentAuthoringWorkspaceTransaction::FOperationFailure&)> FinalReadback;
		FString Phase;
		Contract::FDiagnostic Diagnostic;
	};

	/**
	 * The minimal identity selector accepted by domain HJSON authoring.
	 * Domain-specific context, assertions and action details remain owned by
	 * the consuming domain; this type only represents one readable name and the
	 * short semantic discriminators needed to resolve it.
	 */
	struct UEAGENTINTERFACE_API FDomainHjsonCandidateSelector final
	{
		bool IsValid() const;
		bool TryGetSelectValue(const FString& Key, FString& OutValue) const;

		FString Name;
		TMap<FString, FString> Select;
	};

	UEAGENTINTERFACE_API bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError,
		EDomainHjsonDocumentProjection Projection = EDomainHjsonDocumentProjection::Json);

	UEAGENTINTERFACE_API FString ObjectPathFromPackageIdentity(
		const FString& TargetIdentity);

	UEAGENTINTERFACE_API bool TryInspectObjectTargetPresence(
		const FString& TargetIdentity,
		bool& bOutPresent);

	UEAGENTINTERFACE_API bool TryCaptureObject(
		const FString& TargetIdentity,
		const FString& ExpectedRootTypeIdentity,
		UObject*& OutObject,
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture& OutCapture,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCaptureObject(
		const FString& TargetIdentity,
		const TArray<FString>& ExpectedRootTypeIdentities,
		UObject*& OutObject,
		DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture& OutCapture,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildCaptureSnapshot(
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture& Capture,
		TSharedPtr<FJsonObject>& OutSnapshot,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalizeObject(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildDomainHjsonDirectoryExportPlan(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	/**
	 * Builds the same verified directory contract while formatting the model-facing
	 * entry as readable multi-line HJSON. The hidden workspace manifest remains the
	 * canonical machine projection owned by this common layer.
	 */
	UEAGENTINTERFACE_API bool TryBuildReadableDomainHjsonDirectoryExportPlan(
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& ProfileId,
		const FString& ProviderId,
		const FString& EntryFile,
		const TSharedPtr<FJsonObject>& Document,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryVerifyDomainHjsonStagedWorkspace(
		const FString& StagedWorkspacePath,
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& EntryFile,
		ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API void SetWorkspaceErrorFromCommandFailure(
		const UE::UAI::Contract::FDiagnostic& Failure,
		FString FallbackCode,
		FString FallbackSubject,
		FWorkspaceError& OutError);

	/**
	 * Validates the complete internal import outcome, then emits the minimal
	 * model-facing success result. Callers inspect the new state through the
	 * existing query or re-export path instead of receiving a duplicate readback.
	 */
	UEAGENTINTERFACE_API bool TryBuildImportSuccessPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& Revision,
		const FDomainHjsonImportLifecycleReceipt& LifecycleReceipt,
		const TSharedPtr<FJsonObject>& IndependentReadback,
		const TSharedPtr<FJsonObject>& Mutation,
		FString& OutPayloadJson,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryReadbackImportPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& ObjectPath,
		const TSharedPtr<FJsonObject>& Mutation,
		const FString& ReadbackPayloadErrorCode,
		const FString& CompileNotSettledErrorCode,
		TFunctionRef<bool(FString&, FString&, FWorkspaceError&)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError,
		const UeAgentAuthoringWorkspaceTransaction::FFinalReadbackEvidence* CompletedOperation = nullptr);

	UEAGENTINTERFACE_API bool TryValidateReadbackDiagnostics(
		const TSharedPtr<FJsonObject>& Readback,
		const FString& TargetIdentity,
		const FString& FailureCode,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API void AddStoredBlueprintDiagnostics(
		const UBlueprint& Blueprint,
		const TSharedRef<FJsonObject>& Readback);

	UEAGENTINTERFACE_API bool TryReadPersistedPackageRevision(
		const FString& PackageIdentity,
		FString& OutRevision,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCoordinateRootPropertyImport(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		const FString& ExpectedRootOwnerPath,
		ContextResource::FUeAgentResourceTracker& Resources,
		const FRootPropertyImportContract& ImportContract,
		TFunctionRef<bool(
			const FString& RequestId,
			TArray<TSharedPtr<FJsonValue>>&& Properties,
			FRootPropertyApplyOutcome& OutOutcome)> TryApply,
		TFunctionRef<bool(
			FString& OutReadbackPayload,
			FString& OutRevision,
			FWorkspaceError& OutReadError)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCoordinateSemanticWorkspaceImport(
		const FResolvedSemanticWorkspaceRef& Workspace,
		const FAuthoringModelRef& CurrentModel,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity,
		const FString& ExpectedRootOwnerPath,
		ContextResource::FUeAgentResourceTracker& Resources,
		const FRootPropertyImportContract& ImportContract,
		TFunctionRef<bool(
			const FString& RequestId,
			const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
			Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
			FRootPropertyApplyOutcome& OutOutcome)> TryApply,
		TFunctionRef<bool(
			FString& OutReadbackPayload,
			FString& OutRevision,
			FWorkspaceError& OutReadError)> TryReadback,
		FString& OutPayloadJson,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryReadCandidateSelector(
		const TSharedPtr<const FJsonObject>& Candidate,
		const FString& JsonPointer,
		FDomainHjsonCandidateSelector& OutSelector,
		FWorkspaceError& OutError);
}
