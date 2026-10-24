#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringApplication.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_RUNTIME_V1 1

class UWorld;

namespace UE::UAI::Composition::SimpleAuthoring
{
	class IUeAgentSimpleAuthoringAssetDurableService;

	struct UEAGENTINTERFACE_API FUeAgentSimpleAuthoringRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		FString JournalRoot;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	using FUeAgentSimpleAuthoringRuntimeError = Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentSimpleAuthoringRuntime final
	{
	public:
		static bool TryCreate(
			const FUeAgentSimpleAuthoringRuntimeConfig& Config,
			UWorld& ExplicitWorld,
			TSharedRef<const BindingGateway::Ticket::
				FUeAgentOperationTicketService> TicketService,
			TSharedRef<const Projection::FUeAgentResponseProjector>
				Projector,
			TSharedPtr<FUeAgentSimpleAuthoringRuntime>& OutRuntime,
			FUeAgentSimpleAuthoringRuntimeError& OutError);

		TSharedRef<const CommandContract::
			FUeAgentCommandContractRegistry> GetCommandRegistry() const;
		TSharedRef<FUeAgentSimpleAuthoringApplication>
			GetApplication() const;
		TSharedRef<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> GetDurableService() const;
		const TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			GetCommandHandlers() const;
		const TArray<TSharedRef<Discovery::IUeAgentCatalogSource>>&
			GetCatalogSources() const;
		const TArray<FString>& GetFamilyIds() const;
		const TArray<FString>& GetAuthorityScopes() const;

	private:
		FUeAgentSimpleAuthoringRuntime(
			TSharedRef<const CommandContract::
				FUeAgentCommandContractRegistry> InCommandRegistry,
			TSharedRef<FUeAgentSimpleAuthoringApplication> InApplication,
			TSharedRef<IUeAgentSimpleAuthoringAssetDurableService,
				ESPMode::ThreadSafe> InDurableService,
			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
				InCommandHandlers,
			TArray<TSharedRef<Discovery::IUeAgentCatalogSource>>
				InCatalogSources,
			TArray<FString> InFamilyIds,
			TArray<FString> InAuthorityScopes);

		TSharedRef<const CommandContract::FUeAgentCommandContractRegistry>
			CommandRegistry;
		TSharedRef<FUeAgentSimpleAuthoringApplication> Application;
		TSharedRef<IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			CommandHandlers;
		TArray<TSharedRef<Discovery::IUeAgentCatalogSource>> CatalogSources;
		TArray<FString> FamilyIds;
		TArray<FString> AuthorityScopes;
	};
}
