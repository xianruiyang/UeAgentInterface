// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

class FJsonObject;

namespace UeAgentAuthoringWorkspaceRouter
{
	enum class EUnitPresence : uint8
	{
		Absent,
		Empty,
		Present,
	};

	enum class EUnitAction : uint8
	{
		Apply,
		ExplicitEmpty,
		EmptyValue,
		Remove,
		NoChange,
	};

	struct UEAGENTINTERFACE_API FSourceProvenance
	{
		UeAgentApplyWorkspaceContract::ESourceKind SourceKind =
			UeAgentApplyWorkspaceContract::ESourceKind::Directory;
		FString WorkspaceSource;
		FString SourceFile;
		FString SourceUnit;
		FString RelativePath;
		bool bVirtual = false;
		int32 Line = 0;
		int32 Column = 0;

		UeAgentAuthoringDiagnostics::FSourceLocation ToDiagnosticSource() const;
	};

	struct UEAGENTINTERFACE_API FDocumentCandidate
	{
		FString AbsolutePath;
		FString RelativePath;
		bool bVirtual = false;
		bool bEmpty = false;
		TSharedPtr<const FJsonObject> Document;
		TMap<FString, FUeAgentHjsonSourceLocation> SourceLocations;
		FSourceProvenance Provenance;
	};

	struct UEAGENTINTERFACE_API FWorkspaceUnit
	{
		FString UnitId;
		FString CanonicalUnitKey;
		FString RelativePath;
		FString Role;
		FString RouteId;
		FString ApplyContractRef;
		FString MergePolicy;
		EUnitPresence Presence = EUnitPresence::Absent;
		EUnitAction Action = EUnitAction::NoChange;
		TSharedPtr<const FJsonObject> Document;
		TMap<FString, FUeAgentHjsonSourceLocation> SourceLocations;
		FSourceProvenance Provenance;

		const FUeAgentHjsonSourceLocation* FindSourceLocation(const FString& JsonPointer) const;
		bool HasDocument() const { return Document.IsValid(); }
	};

	struct UEAGENTINTERFACE_API FWorkspace
	{
		FString ContractId;
		FString ContractVersion;
		FString ProfileId;
		UeAgentApplyWorkspaceContract::ESourceKind SourceKind =
			UeAgentApplyWorkspaceContract::ESourceKind::Directory;
		FString WorkspaceRoot;
		TArray<FWorkspaceUnit> Units;

		TArray<const FWorkspaceUnit*> FindUnits(const FString& UnitId) const;
		const FWorkspaceUnit* FindSingleUnit(const FString& UnitId) const;
		TSharedRef<FJsonObject> ToJsonObject(bool bIncludeDocuments = false) const;
	};

	struct UEAGENTINTERFACE_API FRouteFailure
	{
		FString ErrorCode;
		FString ErrorDetail;
		FSourceProvenance Provenance;
	};

	UEAGENTINTERFACE_API FString UnitPresenceToString(EUnitPresence Presence);
	UEAGENTINTERFACE_API FString UnitActionToString(EUnitAction Action);

	UEAGENTINTERFACE_API bool MatchesPathPattern(
		const FString& PathPattern,
		const FString& RelativePath);

	UEAGENTINTERFACE_API bool RouteDocuments(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		UeAgentApplyWorkspaceContract::ESourceKind SourceKind,
		const FString& WorkspaceRoot,
		const TArray<FDocumentCandidate>& Candidates,
		FWorkspace& OutWorkspace,
		FRouteFailure& OutFailure);
}
