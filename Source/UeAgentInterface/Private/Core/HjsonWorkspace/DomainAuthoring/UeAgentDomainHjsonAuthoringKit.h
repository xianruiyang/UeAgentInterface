#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "Dom/JsonObject.h"

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
	};

	struct UEAGENTINTERFACE_API FDomainHjsonImportLifecycleReceipt final
	{
		bool IsComplete() const;

		bool bMutationApplied = false;
		bool bCompileSettled = false;
		bool bPackageSaved = false;
		bool bUnloadedReloaded = false;
		FString PersistenceReceiptId;
	};

	struct UEAGENTINTERFACE_API FDomainHjsonCandidateBinding final
	{
		bool IsValid() const;
		bool SupportsOperation(const FString& Operation) const;

		FString Name;
		FString Kind;
		TArray<FString> SupportedOperations;
		FString InputContract;
		FString PlacementContract;
		FString RequiredContext;
		FString BindingRevision;
	};

	UEAGENTINTERFACE_API bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError);

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

	UEAGENTINTERFACE_API bool TryVerifyDomainHjsonStagedWorkspace(
		const FString& StagedWorkspacePath,
		const FString& TargetIdentity,
		const FDomainHjsonDocumentContract& Contract,
		const FString& BaseRevision,
		const FString& EntryFile,
		ContextResource::FUeAgentResourceTracker& Resources,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API void SetWorkspaceErrorFromCommandFailure(
		const FString& ErrorJson,
		FString FallbackCode,
		FString FallbackSubject,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildImportSuccessPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& Revision,
		const FDomainHjsonImportLifecycleReceipt& LifecycleReceipt,
		const TSharedPtr<FJsonObject>& IndependentReadback,
		const TSharedPtr<FJsonObject>& Mutation,
		FString& OutPayloadJson,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryReadCandidateBinding(
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& JsonPointer,
		FDomainHjsonCandidateBinding& OutBinding,
		FWorkspaceError& OutError);
}
