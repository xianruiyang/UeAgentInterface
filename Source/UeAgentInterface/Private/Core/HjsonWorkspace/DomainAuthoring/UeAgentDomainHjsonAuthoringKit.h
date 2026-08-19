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

		bool IsValid() const;

		FString DomainId;
		FString SchemaId;
		FString RootTypeIdentity;
	};

	struct UEAGENTINTERFACE_API FResolvedDomainHjsonDocument final
	{
		FString TargetIdentity;
		FString ExpectedRevision;
		FString EntryFile;
		TSharedPtr<FJsonObject> Params;
		TSharedPtr<FJsonObject> Root;
	};

	UEAGENTINTERFACE_API bool TryDecodeResolvedDocument(
		const FString& RequestTargetIdentity,
		const FString& CanonicalParamsJson,
		const FDomainHjsonDocumentContract& Contract,
		FResolvedDomainHjsonDocument& OutDocument,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API FString ObjectPathFromPackageIdentity(
		const FString& TargetIdentity);

	UEAGENTINTERFACE_API bool TryCaptureObject(
		const FString& TargetIdentity,
		const FString& ExpectedRootTypeIdentity,
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

	UEAGENTINTERFACE_API bool TryBuildImportSuccessPayload(
		const FString& ProviderId,
		const FString& TargetIdentity,
		const FString& Revision,
		const TSharedPtr<FJsonObject>& IndependentReadback,
		const TSharedPtr<FJsonObject>& Mutation,
		FString& OutPayloadJson,
		FWorkspaceError& OutError);
}
