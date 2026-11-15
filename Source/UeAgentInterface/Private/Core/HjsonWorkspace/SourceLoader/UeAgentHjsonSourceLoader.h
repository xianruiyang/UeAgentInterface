#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::HjsonWorkspace
{
	class UEAGENTINTERFACE_API FContentGraphImportDocument final
	{
	public:
		FContentGraphImportDocument(
			FString InRelativePath,
			FString InDocument,
			FString InContentHash);

		const FString& GetRelativePath() const;
		const FString& GetDocument() const;
		const FString& GetContentHash() const;

	private:
		FString RelativePath;
		FString Document;
		FString ContentHash;
	};

	class FResolvedContentGraphImportSource;
	using FResolvedContentGraphImportSourcePtr =
		TSharedPtr<const FResolvedContentGraphImportSource>;

	/**
	 * Immutable result of validating and loading the public source union.
	 * Legacy file-backed sources are completely resident before domain routing.
	 * Semantic workspace directories retain only their immutable absolute ingress
	 * identity; the semantic resolver owns manifest and document verification.
	 */
	class UEAGENTINTERFACE_API FResolvedContentGraphImportSource final
	{
	public:
		FResolvedContentGraphImportSource(
			EWorkspaceSourceKind InKind,
			FString InSourceUnit,
			FString InTargetIdentity,
			FString InBaseRevision,
			FString InEntryFile,
			FString InSourceFingerprint,
			FString InSemanticFingerprint,
			TArray<FContentGraphImportDocument> InDocuments,
			bool bInSemanticWorkspaceDirectory = false,
			FString InWorkspaceProfileId = FString());

		EWorkspaceSourceKind GetKind() const;
		const FString& GetSourceUnit() const;
		const FString& GetTargetIdentity() const;
		const FString& GetBaseRevision() const;
		const FString& GetWorkspaceProfileId() const { return WorkspaceProfileId; }
		const FString& GetEntryFile() const;
		const FString& GetSourceFingerprint() const;
		const FString& GetSemanticFingerprint() const;
		const TArray<FContentGraphImportDocument>& GetDocuments() const;
		const FContentGraphImportDocument* GetEntryDocument() const;
		bool IsSemanticWorkspaceDirectory() const;
		TSharedRef<FJsonObject> MakeNormalizedSourceObject() const;

	private:
		EWorkspaceSourceKind Kind = EWorkspaceSourceKind::Inline;
		FString SourceUnit;
		FString TargetIdentity;
		FString BaseRevision;
		FString WorkspaceProfileId;
		FString EntryFile;
		FString SourceFingerprint;
		FString SemanticFingerprint;
		TArray<FContentGraphImportDocument> Documents;
		bool bSemanticWorkspaceDirectory = false;
	};

	/**
	 * Validates the exact public {kind, document|path} source union and loads it
	 * without UE mutation. Directory manifests route either the legacy
	 * uai.content_graph.workspace/1 integrity contract or the authored semantic
	 * workspace contract without exposing an additional public source kind.
	 */
	UEAGENTINTERFACE_API const FString&
	GetContentGraphImportSourceSchemaJson();

	UEAGENTINTERFACE_API bool TryLoadContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError);
	/** Identify the source and, for a semantic directory, accept its resolver
	 * result under the same namespace interval rather than two paired scans. */
	bool TryLoadContentGraphImportWorkspace(const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity, ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource, FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);

	/**
	 * Recovers the revision and impact authorization used by the internal
	 * import pipeline from a verified directory workspace. These machine
	 * values never belong to the public command contract: semantic workspaces
	 * own them through the hidden manifest, legacy directory workspaces retain
	 * their manifest baseline, and complete inline/single-file documents carry
	 * only the short internal "missing" create expectation. Existing targets
	 * therefore still fail before mutation for non-directory sources. When
	 * requested, managed paths (including the manifest) come from that same
	 * verified semantic parse for subsequent workspace refresh.
	 */
	UEAGENTINTERFACE_API bool TryRecoverContentGraphImportMachineContext(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutResolvedSource,
		FString& OutExpectedRevision,
		FString& OutAuthorizationId,
		FWorkspaceError& OutError,
		TArray<FString>* OutManagedSourcePaths = nullptr);

	/**
	 * Validates the internal kind=resolved envelope emitted by
	 * FResolvedContentGraphImportSource::MakeNormalizedSourceObject(). This does
	 * not extend the public source union or interpret domain schema/root fields.
	 */
	UEAGENTINTERFACE_API bool TryLoadNormalizedContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		const FString& ExpectedRevision,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError);

	/** Returns the same successful semantic resolution used to validate the
	 * envelope. Immediate model consumers need not resolve it again. This is a
	 * point-in-time result, not permission to skip later source/version checks. */
	UEAGENTINTERFACE_API bool TryLoadNormalizedContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		const FString& ExpectedRevision,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError,
		FResolvedSemanticWorkspacePtr& OutSemanticWorkspace);

	/**
	 * Loads one discriminated HJSON source without performing UE mutation.
	 * Directory, single-file and inline carriers converge on the same semantic
	 * workspace contract before import planning begins.
	 */
	UEAGENTINTERFACE_API bool TryLoadHjsonImportSource(
		const FWorkspaceSource& Source,
		ESemanticWorkspaceMode ExpectedMode,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedSemanticWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError);
}
