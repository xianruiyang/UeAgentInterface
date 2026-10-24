#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Discovery::Command
{
	using FCommandDiscoveryError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FCommandSearchRequest final
	{
		FString Intent;
	};

	struct UEAGENTINTERFACE_API FCommandCandidate final
	{
		FString CommandId;
	};

	struct UEAGENTINTERFACE_API FCommandSearchPage final
	{
		TArray<FCommandCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 ReturnedCount = 0;
	};

	struct UEAGENTINTERFACE_API FCommandDetail final
	{
		FString CommandId;
		FString PythonNamespace;
		FString PythonFunction;
		FString PublicOwner;
		FString VariantId;
		FString RequestSchemaJson;
		CommandContract::FUeAgentCommandPurposeDeclaration Purpose;
		TArray<CommandContract::FUeAgentCommandFieldDeclaration> InputFields;
		TArray<CommandContract::FUeAgentCommandFieldDeclaration> ResultFields;
		TArray<CommandContract::FUeAgentCommandErrorDeclaration> Errors;
		TArray<FString> TutorialReferences;
		FString LifecycleStatus;
		TArray<FString> SuccessorCommandIds;
		FString LifecycleReason;
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
			const FString& VariantId,
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
