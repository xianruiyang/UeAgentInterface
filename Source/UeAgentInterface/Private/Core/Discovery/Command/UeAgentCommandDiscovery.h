#pragma once

#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Discovery::Command
{
	struct UEAGENTINTERFACE_API FCommandDiscoveryError final
	{
		FString Code;
		FString Detail;
	};

	struct UEAGENTINTERFACE_API FCommandSearchRequest final
	{
		FString Intent;
		int32 Limit = 3;
		int32 Cursor = 0;
	};

	struct UEAGENTINTERFACE_API FCommandCandidate final
	{
		FString CommandId;
		FString ContractHash;
		FString Summary;
		FString Target;
		FString MatchReason;
		bool bHasSideEffects = false;
	};

	struct UEAGENTINTERFACE_API FCommandSearchPage final
	{
		TArray<FCommandCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = -1;
		bool bComplete = true;
	};

	struct UEAGENTINTERFACE_API FCommandDetail final
	{
		FString CommandId;
		FString ContractHash;
		FString PythonNamespace;
		FString PythonFunction;
		FString PublicOwner;
		CommandContract::FUeAgentCommandPurposeDeclaration Purpose;
		TArray<CommandContract::FUeAgentCommandFieldDeclaration> InputFields;
		TArray<CommandContract::FUeAgentCommandFieldDeclaration> ResultFields;
		TArray<CommandContract::FUeAgentCommandErrorDeclaration> Errors;
		TArray<FString> TutorialReferences;
		bool bHasSideEffects = false;
	};

	class UEAGENTINTERFACE_API FCommandDiscoveryService final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry,
			TSharedPtr<const FCommandDiscoveryService>& OutService,
			FCommandDiscoveryError& OutError);

		bool Search(
			const FCommandSearchRequest& Request,
			FCommandSearchPage& OutPage,
			FCommandDiscoveryError& OutError) const;

		bool GetDetail(
			const FString& CommandId,
			const FString& ContractHash,
			FCommandDetail& OutDetail,
			FCommandDiscoveryError& OutError) const;

	private:
		explicit FCommandDiscoveryService(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				InRegistry);

		TSharedRef<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>
			Registry;
	};
}
