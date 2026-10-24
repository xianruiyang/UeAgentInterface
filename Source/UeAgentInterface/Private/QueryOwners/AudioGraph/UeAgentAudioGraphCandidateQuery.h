#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::ContentDiscovery { class FReadOnlyCandidateBatch; }
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

class UObject;
namespace Metasound::Editor { class ISchemaQueryResult; }

namespace UE::UAI::QueryOwners::AudioGraph
{
	struct UEAGENTINTERFACE_API FAudioGraphCandidate
	{
		/** Server-side identity authority for native MetaSound candidates. */
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		FString TargetRef;
		FString TargetRevision;
		FString Id;
		/** Server-only native MetaSound/SoundCue class selector. Never serialize on the model surface. */
		FString NativeClassName;
		/** Native lazy load/registration owner; retained only on the server. */
		TSharedPtr<Metasound::Editor::ISchemaQueryResult> NativeQueryResult;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		/** Readable semantic scopes used to disambiguate duplicate class names. */
		TArray<FString> Scopes;
		/** Native namespace/variant or asset provenance, preserved without generated class IDs. */
		TMap<FString, FString> SelectionFields;
		FString WriteSupport;
		FString SupportReason;
		Contract::FDiagnostic SupportDiagnostic;
		bool bSupported = false;
		TArray<FString> SupportedOperations;
		FString InputContract;
		FString PlacementContract;
		FString RequiredContext;
		FString BindingRevision;
		int32 MajorVersion = 0;
		int32 MinorVersion = 0;
	};

	struct UEAGENTINTERFACE_API FAudioGraphCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAudioGraphCandidate> Candidates;
		int32 TotalMatches = 0;
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
	};

	using FAudioGraphCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryAudioGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAudioGraphCandidateQueryResult& OutResult,
		FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr);

	/** Project an existing native class through the same catalog and name owner.
	 * Native identity stays server-side; unrelated catalog entries are not projected.
	 * This readback projection does not capture the target or create a mutation binding. */
	UEAGENTINTERFACE_API bool TryResolveAudioGraphNativeCandidate(
		UObject& Target, const FString& NativeClassName, int32 MajorVersion, int32 MinorVersion,
		FAudioGraphCandidate& OutCandidate, FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr);

	/** Resolve the native MetaSound catalog and materialize only the selected class. */
	UEAGENTINTERFACE_API bool TryResolveAudioGraphCandidate(
		UObject& Target,
		const FString& Name,
		FAudioGraphCandidate& OutCandidate,
		FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr,
		const TMap<FString, FString>& Select = {});

	UEAGENTINTERFACE_API bool TryResolveSoundCueCandidate(
		UObject& Target, const FString& Name, FAudioGraphCandidate& OutCandidate,
		FAudioGraphCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr,
		const TMap<FString, FString>& Select = {});

}
