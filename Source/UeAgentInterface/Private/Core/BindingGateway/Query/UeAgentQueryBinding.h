#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryPublicProjection.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::BindingGateway::Query
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentMonotonicClock;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::Discovery::FUeAgentCatalogRevision;
	using UE::UAI::Discovery::FUeAgentCatalogSnapshot;
	using UE::UAI::Discovery::FUeAgentDiscoveryQuery;
	using UE::UAI::Projection::FUeAgentProjectedResponse;
	using UE::UAI::Projection::FUeAgentProjectionRequest;
	using UE::UAI::StateSemantic::EUeAgentStatePlane;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	using FUeAgentQuerySessionIdentity =
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationSessionIdentity;
	using FUeAgentQueryTicket =
		UE::UAI::BindingGateway::Ticket::FUeAgentOperationTicket;

	class UEAGENTINTERFACE_API FUeAgentCatalogSearchQuery final
	{
	public:
		FUeAgentCatalogSearchQuery(
			FUeAgentQuerySessionIdentity InSession,
			FUeAgentDiscoveryQuery InQuery,
			FUeAgentProjectionRequest InProjection);

		const FUeAgentQuerySessionIdentity& GetSession() const;
		const FUeAgentDiscoveryQuery& GetQuery() const;
		const FUeAgentProjectionRequest& GetProjection() const;
		bool IsValid() const;
		const FUeAgentQueryBindingError& GetValidationDiagnostic() const;

	private:
		FUeAgentQuerySessionIdentity Session;
		FUeAgentDiscoveryQuery Query;
		FUeAgentProjectionRequest Projection;
		FUeAgentQueryBindingError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentCatalogResolveQuery final
	{
	public:
		FUeAgentCatalogResolveQuery(
			FUeAgentQuerySessionIdentity InSession,
			TSharedRef<const FUeAgentCatalogSnapshot> InCatalog,
			FString InCandidateId,
			FUeAgentProjectionRequest InProjection);

		const FUeAgentQuerySessionIdentity& GetSession() const;
		const TSharedRef<const FUeAgentCatalogSnapshot>& GetCatalog() const;
		const FString& GetCandidateId() const;
		const FUeAgentProjectionRequest& GetProjection() const;
		bool IsValid() const;
		const FUeAgentQueryBindingError& GetValidationDiagnostic() const;

	private:
		FUeAgentQuerySessionIdentity Session;
		TSharedRef<const FUeAgentCatalogSnapshot> Catalog;
		FString CandidateId;
		FUeAgentProjectionRequest Projection;
		FUeAgentQueryBindingError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentStateReadQuery final
	{
	public:
		FUeAgentStateReadQuery(
			FUeAgentQuerySessionIdentity InSession,
			FUeAgentQueryTicket InTicket,
			FUeAgentCandidateBinding InBinding,
			EUeAgentStatePlane InStatePlane,
			FUeAgentProjectionRequest InProjection);

		const FUeAgentQuerySessionIdentity& GetSession() const;
		const FUeAgentQueryTicket& GetTicket() const;
		const FUeAgentCandidateBinding& GetBinding() const;
		EUeAgentStatePlane GetStatePlane() const;
		const FUeAgentProjectionRequest& GetProjection() const;
		bool IsValid() const;
		const FUeAgentQueryBindingError& GetValidationDiagnostic() const;

	private:
		FUeAgentQuerySessionIdentity Session;
		FUeAgentQueryTicket Ticket;
		FUeAgentCandidateBinding Binding;
		EUeAgentStatePlane StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		FUeAgentProjectionRequest Projection;
		FUeAgentQueryBindingError ValidationDiagnostic;
	};

	enum class EUeAgentQueryBindingPayloadKind : uint8
	{
		CatalogSearch,
		CatalogResolve,
		StateRead,
		DomainQuery,
	};

	class UEAGENTINTERFACE_API FUeAgentQueryBindingPayload final
	{
	public:
		explicit FUeAgentQueryBindingPayload(
			FUeAgentCatalogSearchQuery InQuery);
		explicit FUeAgentQueryBindingPayload(
			FUeAgentCatalogResolveQuery InQuery);
		explicit FUeAgentQueryBindingPayload(
			FUeAgentStateReadQuery InQuery);
		explicit FUeAgentQueryBindingPayload(
			FUeAgentDomainQuery InQuery);

		EUeAgentQueryBindingPayloadKind GetKind() const;
		const TSharedPtr<const FUeAgentCatalogSearchQuery>&
			GetCatalogSearch() const;
		const TSharedPtr<const FUeAgentCatalogResolveQuery>&
			GetCatalogResolve() const;
		const TSharedPtr<const FUeAgentStateReadQuery>& GetStateRead() const;
		const TSharedPtr<const FUeAgentDomainQuery>& GetDomainQuery() const;
		bool IsValid() const;
		const FUeAgentQueryBindingError& GetValidationDiagnostic() const;

	private:
		EUeAgentQueryBindingPayloadKind Kind =
			EUeAgentQueryBindingPayloadKind::CatalogSearch;
		TSharedPtr<const FUeAgentCatalogSearchQuery> CatalogSearch;
		TSharedPtr<const FUeAgentCatalogResolveQuery> CatalogResolve;
		TSharedPtr<const FUeAgentStateReadQuery> StateRead;
		TSharedPtr<const FUeAgentDomainQuery> DomainQuery;
		FUeAgentQueryBindingError ValidationDiagnostic;
	};

	using FUeAgentQueryBindingRequest =
		UE::UAI::Contract::TUeAgentQueryRequest<
			FUeAgentQueryBindingPayload>;

	class UEAGENTINTERFACE_API FUeAgentQueryBindingResponse final
	{
	public:
		FUeAgentQueryBindingResponse(
			TSharedRef<const FUeAgentProjectedResponse> InProjection,
			TOptional<FUeAgentQueryTicket> InTicket,
			TSharedPtr<const
				UE::UAI::Projection::FUeAgentProjectionSourceValue>
				InSource = {});

		const TSharedPtr<const FUeAgentProjectedResponse>&
			GetProjection() const;
		TArray<UE::UAI::Contract::FDiagnostic> GetDiagnostics() const;
		const TOptional<FUeAgentQueryTicket>& GetTicket() const;
		const TSharedPtr<const
			UE::UAI::Projection::FUeAgentProjectionSourceValue>&
			GetSource() const;
		bool IsValid() const;
		const FUeAgentQueryBindingError& GetValidationDiagnostic() const;

	private:
		TSharedPtr<const FUeAgentProjectedResponse> Projection;
		TOptional<FUeAgentQueryTicket> Ticket;
		TSharedPtr<const
			UE::UAI::Projection::FUeAgentProjectionSourceValue>
				Source;
		FUeAgentQueryBindingError ValidationDiagnostic;
	};

	using FUeAgentQueryBindingResult =
		UE::UAI::Contract::TUeAgentQueryResult<
			FUeAgentQueryBindingResponse>;

	class UEAGENTINTERFACE_API IUeAgentQuerySessionSource
	{
	public:
		virtual ~IUeAgentQuerySessionSource() = default;

		virtual FUeAgentQuerySessionIdentity GetCurrent() const = 0;
	};

	using FUeAgentQueryTicketIdFactory =
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketIdFactory;

	class UEAGENTINTERFACE_API FUeAgentQueryTicketService final
	{
	public:
		static bool TryCreate(
			const FString& IntegrityKey,
			double TtlSeconds,
			FUeAgentMonotonicClock Clock,
			FUeAgentQueryTicketIdFactory TicketIdFactory,
			TSharedPtr<const FUeAgentQueryTicketService>& OutService,
			FUeAgentQueryBindingError& OutError);
		static bool TryCreateFromShared(
			TSharedRef<
				const UE::UAI::BindingGateway::Ticket::
					FUeAgentOperationTicketService> SharedService,
			TSharedPtr<const FUeAgentQueryTicketService>& OutService,
			FUeAgentQueryBindingError& OutError);

		bool TryIssue(
			const FUeAgentCandidateBinding& Binding,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FUeAgentQuerySessionIdentity& Session,
			const FUeAgentCallerContext& CallerContext,
			TOptional<FUeAgentStateRevision> TargetRevision,
			TOptional<FUeAgentQueryTicket>& OutTicket,
			FUeAgentQueryBindingError& OutError) const;

		bool TryValidate(
			const FUeAgentQueryTicket& Ticket,
			const FUeAgentCandidateBinding& Binding,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FUeAgentQuerySessionIdentity& Session,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentQueryBindingError& OutError) const;

	private:
		explicit FUeAgentQueryTicketService(
			TSharedRef<
				const UE::UAI::BindingGateway::Ticket::
					FUeAgentOperationTicketService> InSharedService);

		TSharedRef<
			const UE::UAI::BindingGateway::Ticket::
				FUeAgentOperationTicketService> SharedService;
	};

	class UEAGENTINTERFACE_API FUeAgentQueryPayloadCodec final
	{
	public:
		static bool TryEncode(
			const FUeAgentQueryBindingResponse& Response,
			FUeAgentResourceTracker& Resources,
			FUeAgentContractValue& OutValue,
			FUeAgentQueryBindingError& OutError);
		static bool TryEncodePublic(
			const FQueryPublicProjectionDescriptor& PublicProjection,
			const FString& CommandId,
			const FString& MechanicsCommandId,
			bool bReadOnly,
			const FUeAgentQueryBindingResponse& Response,
			FUeAgentResourceTracker& Resources,
			FUeAgentContractValue& OutValue,
			FUeAgentQueryBindingError& OutError);
	};

	class UEAGENTINTERFACE_API FUeAgentQueryBinding final
	{
	public:
		static bool TryCreate(
			FUeAgentDescriptorIdentity DescriptorIdentity,
			FString RegistryHash,
			TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
				Discovery,
			TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
				Projector,
			TSharedRef<IUeAgentQuerySessionSource> SessionSource,
			TSharedRef<const FUeAgentQueryTicketService> TicketService,
			TSharedPtr<const FUeAgentQueryBinding>& OutBinding,
			FUeAgentQueryBindingError& OutError);
		static bool TryCreate(
			TSharedRef<
				const UE::UAI::Contract::FUeAgentDescriptorRegistry>
				DescriptorRegistry,
			TSharedRef<const FUeAgentDomainQueryProviderRegistry>
				DomainProviders,
			TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
				Discovery,
			TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
				Projector,
			TSharedRef<IUeAgentQuerySessionSource> SessionSource,
			TSharedRef<const FUeAgentQueryTicketService> TicketService,
			TSharedPtr<const FUeAgentQueryBinding>& OutBinding,
			FUeAgentQueryBindingError& OutError);

		bool TrySubmit(
			const FUeAgentQueryBindingRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentQueryBindingResult>& OutResult,
			FUeAgentQueryBindingError& OutError) const;
		bool TrySubmit(
			const FUeAgentQueryBindingRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			FUeAgentOperationDeadline Deadline,
			FUeAgentCancellationToken Cancellation,
			TSharedPtr<const FUeAgentQueryBindingResult>& OutResult,
			FUeAgentQueryBindingError& OutError) const;

	private:
		FUeAgentQueryBinding(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRegistryHash,
			TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
				InDiscovery,
			TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
				InProjector,
			TSharedRef<IUeAgentQuerySessionSource> InSessionSource,
			TSharedRef<const FUeAgentQueryTicketService> InTicketService);
		FUeAgentQueryBinding(
			TSharedRef<
				const UE::UAI::Contract::FUeAgentDescriptorRegistry>
				InDescriptorRegistry,
			TSharedRef<const FUeAgentDomainQueryProviderRegistry>
				InDomainProviders,
			TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
				InDiscovery,
			TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
				InProjector,
			TSharedRef<IUeAgentQuerySessionSource> InSessionSource,
			TSharedRef<const FUeAgentQueryTicketService> InTicketService);

		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RegistryHash;
		TSharedPtr<
			const UE::UAI::Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>
			DomainProviders;
		TSharedRef<const UE::UAI::Discovery::FUeAgentDiscoveryService>
			Discovery;
		TSharedRef<const UE::UAI::Projection::FUeAgentResponseProjector>
			Projector;
		TSharedRef<IUeAgentQuerySessionSource> SessionSource;
		TSharedRef<const FUeAgentQueryTicketService> TicketService;
	};
}
