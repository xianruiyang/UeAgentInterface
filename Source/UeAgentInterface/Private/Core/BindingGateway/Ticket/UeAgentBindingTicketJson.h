#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::BindingGateway::Ticket
{
	struct UEAGENTINTERFACE_API FUeAgentBindingTicketJsonError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API FUeAgentBindingTicketJsonCodec final
	{
	public:
		static bool TryParseSession(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentOperationSessionIdentity>& OutSession,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> SessionToJson(
			const FUeAgentOperationSessionIdentity& Session);

		static bool TryParseDescriptor(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Contract::FUeAgentDescriptorIdentity>& OutDescriptor,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> DescriptorToJson(
			const Contract::FUeAgentDescriptorIdentity& Descriptor);

		static bool TryParseIdentity(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<StateSemantic::FUeAgentCanonicalIdentity>& OutIdentity,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> IdentityToJson(
			const StateSemantic::FUeAgentCanonicalIdentity& Identity);

		static bool TryParseCatalogRevision(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Discovery::FUeAgentCatalogRevision>& OutRevision,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> CatalogRevisionToJson(
			const Discovery::FUeAgentCatalogRevision& Revision);

		static bool TryParseStateRevision(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<StateSemantic::FUeAgentStateRevision>& OutRevision,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> StateRevisionToJson(
			const StateSemantic::FUeAgentStateRevision& Revision);

		static bool TryParseBinding(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<Discovery::FUeAgentCandidateBinding>& OutBinding,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> BindingToJson(
			const Discovery::FUeAgentCandidateBinding& Binding);

		static bool TryParseTicket(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentOperationTicket>& OutTicket,
			FUeAgentBindingTicketJsonError& OutError);
		static TSharedRef<FJsonObject> TicketToJson(
			const FUeAgentOperationTicket& Ticket);
	};
}
