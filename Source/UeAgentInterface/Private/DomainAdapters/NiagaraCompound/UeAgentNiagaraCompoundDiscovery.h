#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"

class UObject;
class UNiagaraNodeFunctionCall;

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	struct UEAGENTINTERFACE_API FUeAgentNiagaraCompoundSelector
	{
		FString StageKey;
		FString EmitterHandleId;
		FString ModuleNodeGuid;
		FString InputName;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentResolvedNiagaraCompoundInput
	{
		UObject* Asset = nullptr;
		UNiagaraNodeFunctionCall* Module = nullptr;
		UeAgentNiagaraStageModuleAuthoring::FResolvedModuleInput Input;
		FUeAgentNiagaraCompoundSelector Selector;
	};

	UEAGENTINTERFACE_API FString MakeNiagaraCompoundSelector(
		const FUeAgentNiagaraCompoundSelector& Selector);
	UEAGENTINTERFACE_API bool TryParseNiagaraCompoundSelector(
		const FString& Text,
		FUeAgentNiagaraCompoundSelector& OutSelector,
		UE::UAI::Contract::FDiagnostic& OutError);

	class UEAGENTINTERFACE_API FUeAgentNiagaraCompoundCatalogSource final
		: public Discovery::IUeAgentCatalogSource
	{
	public:
		FUeAgentNiagaraCompoundCatalogSource(
			Contract::FUeAgentDescriptorIdentity InOperationDescriptor,
			FString InGeneration);

		const FString& GetSourceId() const override;
		const TArray<FString>& GetCatalogKinds() const override;
		bool TrySearch(
			const Discovery::FUeAgentDiscoveryQuery& Query,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const Discovery::FUeAgentCatalogSnapshot>& OutCatalog,
			Discovery::FUeAgentDiscoveryError& OutError) override;
		bool TryResolveExact(
			const FString& CandidateId,
			const Discovery::FUeAgentCatalogRevision& ExpectedRevision,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const Discovery::FUeAgentCandidateBinding>& OutBinding,
			Discovery::FUeAgentDiscoveryError& OutError) override;

	private:
		Contract::FUeAgentDescriptorIdentity OperationDescriptor;
		FString Generation;
		TArray<FString> Kinds;
		FCriticalSection CacheMutex;
		TMap<FString, FString> PublicRefByCandidateId;
	};

	UEAGENTINTERFACE_API bool TryDiscoverNiagaraCompoundBindings(
		const FString& AssetPath,
		const Contract::FUeAgentDescriptorIdentity& OperationDescriptor,
		const FString& Generation,
		ContextResource::FUeAgentResourceTracker& Resources,
		const ContextResource::FUeAgentOperationDeadline& Deadline,
		const ContextResource::FUeAgentCancellationToken& Cancellation,
		TArray<Discovery::FUeAgentCandidateBinding>& OutBindings,
		Discovery::FUeAgentDiscoveryError& OutError);

	UEAGENTINTERFACE_API bool TryResolveNiagaraCompoundInput(
		const Discovery::FUeAgentCandidateBinding& Binding,
		const FString& ExpectedGeneration,
		FUeAgentResolvedNiagaraCompoundInput& OutResolved,
		UE::UAI::Contract::FDiagnostic& OutError);
}
