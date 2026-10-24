#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::BindingGateway::Ticket
{
	using FUeAgentBindingTicketJsonError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentBindingTicketJsonCodec final
	{
	public:
		static bool TryParseSession(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentOperationSessionIdentity>& OutSession,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("session"));
		static TSharedRef<FJsonObject> SessionToJson(
			const FUeAgentOperationSessionIdentity& Session);

		static bool TryParseDescriptor(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Contract::FUeAgentDescriptorIdentity>& OutDescriptor,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("descriptor"));
		static TSharedRef<FJsonObject> DescriptorToJson(
			const Contract::FUeAgentDescriptorIdentity& Descriptor);

		static bool TryParseIdentity(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<StateSemantic::FUeAgentCanonicalIdentity>& OutIdentity,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("identity"));
		static TSharedRef<FJsonObject> IdentityToJson(
			const StateSemantic::FUeAgentCanonicalIdentity& Identity);

		static bool TryParseCatalogRevision(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Discovery::FUeAgentCatalogRevision>& OutRevision,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("catalog_revision"));
		static TSharedRef<FJsonObject> CatalogRevisionToJson(
			const Discovery::FUeAgentCatalogRevision& Revision);

		static bool TryParseStateRevision(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<StateSemantic::FUeAgentStateRevision>& OutRevision,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("state_revision"));
		static TSharedRef<FJsonObject> StateRevisionToJson(
			const StateSemantic::FUeAgentStateRevision& Revision);

		static bool TryParseBinding(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Discovery::FUeAgentCandidateBinding>& OutBinding,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("binding"));
		static TSharedRef<FJsonObject> BindingToJson(
			const Discovery::FUeAgentCandidateBinding& Binding);

		static bool TryParseTicket(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentOperationTicket>& OutTicket,
			FUeAgentBindingTicketJsonError& OutError,
			const FString& InputPath = TEXT("ticket"));
		static TSharedRef<FJsonObject> TicketToJson(
			const FUeAgentOperationTicket& Ticket);
	};
}
