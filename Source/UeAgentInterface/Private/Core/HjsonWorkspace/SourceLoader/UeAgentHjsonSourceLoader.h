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
	 * File-backed sources are completely resident before domain routing begins.
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
			TArray<FContentGraphImportDocument> InDocuments);

		EWorkspaceSourceKind GetKind() const;
		const FString& GetSourceUnit() const;
		const FString& GetTargetIdentity() const;
		const FString& GetBaseRevision() const;
		const FString& GetEntryFile() const;
		const FString& GetSourceFingerprint() const;
		const FString& GetSemanticFingerprint() const;
		const TArray<FContentGraphImportDocument>& GetDocuments() const;
		const FContentGraphImportDocument* GetEntryDocument() const;
		TSharedRef<FJsonObject> MakeNormalizedSourceObject() const;

	private:
		EWorkspaceSourceKind Kind = EWorkspaceSourceKind::Inline;
		FString SourceUnit;
		FString TargetIdentity;
		FString BaseRevision;
		FString EntryFile;
		FString SourceFingerprint;
		FString SemanticFingerprint;
		TArray<FContentGraphImportDocument> Documents;
	};

	/**
	 * Validates the exact public {kind, document|path} source union and loads it
	 * without UE mutation. Directory sources use .uai/workspace.hjson with the
	 * uai.content_graph.workspace/1 integrity contract.
	 */
	UEAGENTINTERFACE_API const FString&
	GetContentGraphImportSourceSchemaJson();

	UEAGENTINTERFACE_API bool TryLoadContentGraphImportSource(
		const TSharedPtr<FJsonObject>& SourceObject,
		const FString& ExpectedTargetIdentity,
		ContextResource::FUeAgentResourceTracker& Resources,
		FResolvedContentGraphImportSourcePtr& OutSource,
		FWorkspaceError& OutError);

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
