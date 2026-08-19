#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::BindingGateway::Ticket
{
	namespace
	{
		using UE::UAI::Discovery::EUeAgentCandidateOperation;

		void SetError(
			FUeAgentOperationTicketError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		void AppendField(FString& Out, const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			Out += FString::Printf(TEXT("%d:"), Utf8.Length());
			Out += Value;
		}

		void AppendInt(FString& Out, int64 Value)
		{
			AppendField(Out, LexToString(Value));
		}

		void AppendDouble(FString& Out, double Value)
		{
			AppendField(
				Out,
				FString::Printf(TEXT("%.17g"), Value));
		}

		FString Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			SHA256(
				reinterpret_cast<const unsigned char*>(
					Utf8.Get()),
				static_cast<size_t>(Utf8.Length()),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		TStaticArray<uint8, SHA256_DIGEST_LENGTH> Sha256Bytes(
			const TArray<uint8>& Bytes)
		{
			TStaticArray<uint8, SHA256_DIGEST_LENGTH> Digest = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest.GetData());
			return Digest;
		}

		FString SignatureString(
			const TStaticArray<
				uint8,
				SHA256_DIGEST_LENGTH>& Signature)
		{
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (uint8 Byte : Signature)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString HmacSha256(
			const FString& Key,
			const FString& Value)
		{
			constexpr int32 BlockSize = 64;
			const FTCHARToUTF8 KeyUtf8(*Key);
			TArray<uint8> NormalizedKey;
			NormalizedKey.Append(
				reinterpret_cast<const uint8*>(KeyUtf8.Get()),
				KeyUtf8.Length());
			if (NormalizedKey.Num() > BlockSize)
			{
				const auto HashedKey =
					Sha256Bytes(NormalizedKey);
				NormalizedKey.Reset();
				NormalizedKey.Append(
					HashedKey.GetData(),
					HashedKey.Num());
			}
			NormalizedKey.SetNumZeroed(BlockSize);

			const FTCHARToUTF8 ValueUtf8(*Value);
			TArray<uint8> Inner;
			Inner.Reserve(BlockSize + ValueUtf8.Length());
			for (uint8 Byte : NormalizedKey)
			{
				Inner.Add(Byte ^ 0x36);
			}
			Inner.Append(
				reinterpret_cast<const uint8*>(ValueUtf8.Get()),
				ValueUtf8.Length());
			const auto InnerDigest = Sha256Bytes(Inner);

			TArray<uint8> Outer;
			Outer.Reserve(BlockSize + InnerDigest.Num());
			for (uint8 Byte : NormalizedKey)
			{
				Outer.Add(Byte ^ 0x5c);
			}
			Outer.Append(
				InnerDigest.GetData(),
				InnerDigest.Num());
			return SignatureString(Sha256Bytes(Outer));
		}

		FString OperationName(
			EUeAgentCandidateOperation Operation)
		{
			switch (Operation)
			{
			case EUeAgentCandidateOperation::Read:
				return TEXT("read");
			case EUeAgentCandidateOperation::Create:
				return TEXT("create");
			case EUeAgentCandidateOperation::Update:
				return TEXT("update");
			case EUeAgentCandidateOperation::Replace:
				return TEXT("replace");
			case EUeAgentCandidateOperation::Delete:
				return TEXT("delete");
			default:
				return TEXT("unknown");
			}
		}

		bool ConstantTimeEquals(
			const FString& Left,
			const FString& Right)
		{
			const FTCHARToUTF8 LeftUtf8(*Left);
			const FTCHARToUTF8 RightUtf8(*Right);
			const int32 Maximum = FMath::Max(
				LeftUtf8.Length(),
				RightUtf8.Length());
			uint8 Difference = static_cast<uint8>(
				LeftUtf8.Length() ^ RightUtf8.Length());
			for (int32 Index = 0; Index < Maximum; ++Index)
			{
				const uint8 L = Index < LeftUtf8.Length()
					? static_cast<uint8>(LeftUtf8.Get()[Index])
					: 0;
				const uint8 R = Index < RightUtf8.Length()
					? static_cast<uint8>(RightUtf8.Get()[Index])
					: 0;
				Difference |= L ^ R;
			}
			return Difference == 0;
		}
	}

	FUeAgentOperationSessionIdentity::
		FUeAgentOperationSessionIdentity(
			FString InServerSessionId,
			int64 InRegistryEpoch,
			FString InRegistryHash)
		: ServerSessionId(MoveTemp(InServerSessionId))
		, RegistryEpoch(InRegistryEpoch)
		, RegistryHash(MoveTemp(InRegistryHash))
	{
		ServerSessionId.TrimStartAndEndInline();
		RegistryHash.TrimStartAndEndInline();
		bValid = !ServerSessionId.IsEmpty()
			&& RegistryEpoch >= 0
			&& RegistryHash.StartsWith(TEXT("sha256:"));
	}

	const FString&
	FUeAgentOperationSessionIdentity::GetServerSessionId() const
	{
		return ServerSessionId;
	}

	int64 FUeAgentOperationSessionIdentity::GetRegistryEpoch() const
	{
		return RegistryEpoch;
	}

	const FString&
	FUeAgentOperationSessionIdentity::GetRegistryHash() const
	{
		return RegistryHash;
	}

	bool FUeAgentOperationSessionIdentity::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentOperationSessionIdentity::operator==(
		const FUeAgentOperationSessionIdentity& Other) const
	{
		return ServerSessionId == Other.ServerSessionId
			&& RegistryEpoch == Other.RegistryEpoch
			&& RegistryHash == Other.RegistryHash;
	}

	FUeAgentOperationTicket::FUeAgentOperationTicket(
		FString InTicketId,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		UE::UAI::StateSemantic::FUeAgentCanonicalIdentity
			InCandidateIdentity,
		FString InCandidateBindingId,
		FString InCandidateFingerprint,
		FUeAgentCatalogRevision InCatalogRevision,
		FString InServerSessionId,
		int64 InRegistryEpoch,
		FString InRegistryHash,
		FString InContextFingerprint,
		FString InSignature,
		double InIssuedAt,
		double InExpiresAt,
		TOptional<FUeAgentStateRevision> InTargetRevision)
		: TicketId(MoveTemp(InTicketId))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, CandidateIdentity(MoveTemp(InCandidateIdentity))
		, CandidateBindingId(MoveTemp(InCandidateBindingId))
		, CandidateFingerprint(MoveTemp(InCandidateFingerprint))
		, CatalogRevision(MoveTemp(InCatalogRevision))
		, ServerSessionId(MoveTemp(InServerSessionId))
		, RegistryEpoch(InRegistryEpoch)
		, RegistryHash(MoveTemp(InRegistryHash))
		, ContextFingerprint(MoveTemp(InContextFingerprint))
		, Signature(MoveTemp(InSignature))
		, IssuedAt(InIssuedAt)
		, ExpiresAt(InExpiresAt)
		, TargetRevision(MoveTemp(InTargetRevision))
	{
		bValid = !TicketId.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& CandidateIdentity.IsValid()
			&& !CandidateBindingId.IsEmpty()
			&& CandidateFingerprint.StartsWith(TEXT("sha256:"))
			&& CatalogRevision.IsValid()
			&& !ServerSessionId.IsEmpty()
			&& RegistryEpoch >= 0
			&& RegistryHash.StartsWith(TEXT("sha256:"))
			&& ContextFingerprint.StartsWith(TEXT("sha256:"))
			&& Signature.StartsWith(TEXT("sha256:"))
			&& IssuedAt >= 0.0
			&& ExpiresAt > IssuedAt
			&& (!TargetRevision.IsSet()
				|| TargetRevision->IsValid());
	}

	const FString&
	FUeAgentOperationTicket::GetTicketId() const
	{
		return TicketId;
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentOperationTicket::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const UE::UAI::StateSemantic::FUeAgentCanonicalIdentity&
	FUeAgentOperationTicket::GetCandidateIdentity() const
	{
		return CandidateIdentity;
	}

	const FString&
	FUeAgentOperationTicket::GetCandidateBindingId() const
	{
		return CandidateBindingId;
	}

	const FString&
	FUeAgentOperationTicket::GetCandidateFingerprint() const
	{
		return CandidateFingerprint;
	}

	const FUeAgentCatalogRevision&
	FUeAgentOperationTicket::GetCatalogRevision() const
	{
		return CatalogRevision;
	}

	const FString&
	FUeAgentOperationTicket::GetServerSessionId() const
	{
		return ServerSessionId;
	}

	int64 FUeAgentOperationTicket::GetRegistryEpoch() const
	{
		return RegistryEpoch;
	}

	const FString&
	FUeAgentOperationTicket::GetRegistryHash() const
	{
		return RegistryHash;
	}

	const FString&
	FUeAgentOperationTicket::GetContextFingerprint() const
	{
		return ContextFingerprint;
	}

	const FString&
	FUeAgentOperationTicket::GetSignature() const
	{
		return Signature;
	}

	double FUeAgentOperationTicket::GetIssuedAt() const
	{
		return IssuedAt;
	}

	double FUeAgentOperationTicket::GetExpiresAt() const
	{
		return ExpiresAt;
	}

	const TOptional<FUeAgentStateRevision>&
	FUeAgentOperationTicket::GetTargetRevision() const
	{
		return TargetRevision;
	}

	bool FUeAgentOperationTicket::IsValid() const
	{
		return bValid;
	}

	FUeAgentOperationTicketService::
		FUeAgentOperationTicketService(
			FString InIntegrityKey,
			double InTtlSeconds,
			FUeAgentMonotonicClock InClock,
			FUeAgentOperationTicketIdFactory InTicketIdFactory)
		: IntegrityKey(MoveTemp(InIntegrityKey))
		, TtlSeconds(InTtlSeconds)
		, Clock(MoveTemp(InClock))
		, TicketIdFactory(MoveTemp(InTicketIdFactory))
	{
	}

	bool FUeAgentOperationTicketService::TryCreate(
		const FString& IntegrityKey,
		double TtlSeconds,
		FUeAgentMonotonicClock Clock,
		FUeAgentOperationTicketIdFactory TicketIdFactory,
		TSharedPtr<const FUeAgentOperationTicketService>& OutService,
		FUeAgentOperationTicketError& OutError)
	{
		OutService.Reset();
		OutError = {};
		if (IntegrityKey.TrimStartAndEnd().IsEmpty()
			|| TtlSeconds <= 0.0
			|| !Clock
			|| !TicketIdFactory)
		{
			SetError(
				OutError,
				TEXT("ticket_service_invalid"),
				TEXT("ticket_service"),
				TEXT(
					"Integrity key, positive TTL, clock, and "
					"ticket-id factory are required."));
			return false;
		}
		OutService = MakeShareable(
			new FUeAgentOperationTicketService(
				IntegrityKey,
				TtlSeconds,
				MoveTemp(Clock),
				MoveTemp(TicketIdFactory)));
		return true;
	}

	FString
	FUeAgentOperationTicketService::CandidateFingerprint(
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentDescriptorIdentity& DescriptorIdentity) const
	{
		FString Canonical;
		AppendField(
			Canonical,
			DescriptorIdentity.GetDescriptorId());
		AppendInt(Canonical, DescriptorIdentity.GetVersion());
		AppendField(
			Canonical,
			DescriptorIdentity.GetDescriptorHash());
		const auto& Identity = Binding.GetSummary().GetIdentity();
		AppendField(Canonical, Identity.GetSemanticId());
		AppendField(Canonical, Identity.GetTargetKind());
		AppendField(Canonical, Identity.GetOwnerId());
		AppendField(Canonical, Identity.GetSelector());
		AppendField(Canonical, Identity.GetObjectRole());
		AppendField(Canonical, Identity.GetGeneration());
		AppendField(Canonical, Identity.GetCanonicalKey());
		AppendField(Canonical, Binding.GetBindingId());
		AppendField(Canonical, Binding.GetPublicRef());
		AppendField(Canonical, Binding.GetReadPortId());
		AppendField(Canonical, Binding.GetBindingEvidence());
		TArray<FString> Operations;
		for (EUeAgentCandidateOperation Operation :
			Binding.GetSupportedOperations())
		{
			Operations.Add(OperationName(Operation));
		}
		Operations.Sort();
		for (const FString& Operation : Operations)
		{
			AppendField(Canonical, Operation);
		}
		const auto& Revision = Binding.GetCatalogRevision();
		AppendField(Canonical, Revision.GetSourceId());
		AppendField(Canonical, Revision.GetGeneration());
		AppendInt(Canonical, Revision.GetSequence());
		return Sha256(Canonical);
	}

	FString FUeAgentOperationTicketService::ContextFingerprint(
		const FUeAgentCallerContext& CallerContext) const
	{
		FString Canonical;
		AppendField(Canonical, CallerContext.GetCallerId());
		TArray<FString> Scopes =
			CallerContext.GetAuthorityScopes();
		Scopes.Sort();
		for (const FString& Scope : Scopes)
		{
			AppendField(Canonical, Scope);
		}
		return Sha256(Canonical);
	}

	FString FUeAgentOperationTicketService::Sign(
		const FUeAgentOperationTicket& Ticket) const
	{
		FString Canonical;
		AppendField(Canonical, Ticket.GetTicketId());
		const auto& Descriptor =
			Ticket.GetDescriptorIdentity();
		AppendField(
			Canonical,
			Descriptor.GetDescriptorId());
		AppendInt(Canonical, Descriptor.GetVersion());
		AppendField(
			Canonical,
			Descriptor.GetDescriptorHash());
		AppendField(
			Canonical,
			Ticket.GetCandidateIdentity().GetCanonicalKey());
		AppendField(
			Canonical,
			Ticket.GetCandidateBindingId());
		AppendField(
			Canonical,
			Ticket.GetCandidateFingerprint());
		const auto& Catalog = Ticket.GetCatalogRevision();
		AppendField(Canonical, Catalog.GetSourceId());
		AppendField(Canonical, Catalog.GetGeneration());
		AppendInt(Canonical, Catalog.GetSequence());
		AppendField(
			Canonical,
			Ticket.GetServerSessionId());
		AppendInt(Canonical, Ticket.GetRegistryEpoch());
		AppendField(Canonical, Ticket.GetRegistryHash());
		AppendField(
			Canonical,
			Ticket.GetContextFingerprint());
		AppendDouble(Canonical, Ticket.GetIssuedAt());
		AppendDouble(Canonical, Ticket.GetExpiresAt());
		if (Ticket.GetTargetRevision().IsSet())
		{
			AppendField(Canonical, TEXT("target_revision"));
			AppendField(
				Canonical,
				Ticket.GetTargetRevision()->GetSourceId());
			AppendField(
				Canonical,
				Ticket.GetTargetRevision()->GetGeneration());
			AppendInt(
				Canonical,
				Ticket.GetTargetRevision()->GetSequence());
		}
		else
		{
			AppendField(
				Canonical,
				TEXT("no_target_revision"));
		}
		return HmacSha256(IntegrityKey, Canonical);
	}

	bool FUeAgentOperationTicketService::TryIssue(
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentOperationSessionIdentity& Session,
		const FUeAgentCallerContext& CallerContext,
		TOptional<FUeAgentStateRevision> TargetRevision,
		TOptional<FUeAgentOperationTicket>& OutTicket,
		FUeAgentOperationTicketError& OutError) const
	{
		OutTicket.Reset();
		OutError = {};
		if (!Binding.IsValid()
			|| !DescriptorIdentity.IsValid()
			|| !Session.IsValid()
			|| !CallerContext.IsValid()
			|| (TargetRevision.IsSet()
				&& !TargetRevision->IsValid()))
		{
			SetError(
				OutError,
				TEXT("ticket_claim_invalid"),
				Binding.GetBindingId(),
				TEXT("A ticket claim is invalid."));
			return false;
		}
		const FString NewTicketId = TicketIdFactory();
		const double Now = Clock();
		if (NewTicketId.TrimStartAndEnd().IsEmpty()
			|| !FMath::IsFinite(Now)
			|| Now < 0.0)
		{
			SetError(
				OutError,
				TEXT("ticket_host_invalid"),
				Binding.GetBindingId(),
				TEXT(
					"The host clock or ticket-id factory "
					"returned an invalid value."));
			return false;
		}
		FUeAgentOperationTicket Unsigned(
			NewTicketId,
			DescriptorIdentity,
			Binding.GetSummary().GetIdentity(),
			Binding.GetBindingId(),
			CandidateFingerprint(
				Binding,
				DescriptorIdentity),
			Binding.GetCatalogRevision(),
			Session.GetServerSessionId(),
			Session.GetRegistryEpoch(),
			Session.GetRegistryHash(),
			ContextFingerprint(CallerContext),
			TEXT("sha256:pending"),
			Now,
			Now + TtlSeconds,
			MoveTemp(TargetRevision));
		FUeAgentOperationTicket Signed(
			Unsigned.TicketId,
			Unsigned.DescriptorIdentity,
			Unsigned.CandidateIdentity,
			Unsigned.CandidateBindingId,
			Unsigned.CandidateFingerprint,
			Unsigned.CatalogRevision,
			Unsigned.ServerSessionId,
			Unsigned.RegistryEpoch,
			Unsigned.RegistryHash,
			Unsigned.ContextFingerprint,
			Sign(Unsigned),
			Unsigned.IssuedAt,
			Unsigned.ExpiresAt,
			Unsigned.TargetRevision);
		if (!Signed.IsValid())
		{
			SetError(
				OutError,
				TEXT("ticket_creation_failed"),
				Binding.GetBindingId(),
				TEXT(
					"The signed ticket did not satisfy its "
					"immutable contract."));
			return false;
		}
		OutTicket.Emplace(MoveTemp(Signed));
		return true;
	}

	bool FUeAgentOperationTicketService::TryValidate(
		const FUeAgentOperationTicket& Ticket,
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FUeAgentOperationSessionIdentity& Session,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentOperationTicketError& OutError) const
	{
		OutError = {};
		if (!Ticket.IsValid()
			|| !Binding.IsValid()
			|| !DescriptorIdentity.IsValid()
			|| !Session.IsValid()
			|| !CallerContext.IsValid())
		{
			SetError(
				OutError,
				TEXT("ticket_claim_invalid"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket validation received invalid "
					"typed input."));
			return false;
		}
		if (!(Ticket.GetDescriptorIdentity()
			== DescriptorIdentity))
		{
			SetError(
				OutError,
				TEXT("descriptor_hash_mismatch"),
				DescriptorIdentity.GetDescriptorId(),
				TEXT(
					"Ticket descriptor identity differs from "
					"the active descriptor."));
			return false;
		}
		if (Ticket.GetServerSessionId()
			!= Session.GetServerSessionId())
		{
			SetError(
				OutError,
				TEXT("server_session_stale"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket belongs to another server "
					"session."));
			return false;
		}
		if (Ticket.GetRegistryEpoch()
			!= Session.GetRegistryEpoch())
		{
			SetError(
				OutError,
				TEXT("registry_epoch_stale"),
				Ticket.GetTicketId(),
				TEXT("Ticket registry epoch is stale."));
			return false;
		}
		if (Ticket.GetRegistryHash()
			!= Session.GetRegistryHash())
		{
			SetError(
				OutError,
				TEXT("registry_hash_mismatch"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket registry hash differs from the "
					"active registry."));
			return false;
		}
		if (Ticket.GetCatalogRevision()
			!= Binding.GetCatalogRevision())
		{
			SetError(
				OutError,
				TEXT("catalog_revision_stale"),
				Ticket.GetTicketId(),
				TEXT("Ticket catalog revision is stale."));
			return false;
		}
		if (Ticket.GetCandidateBindingId()
				!= Binding.GetBindingId()
			|| !(Ticket.GetCandidateIdentity()
				== Binding.GetSummary().GetIdentity())
			|| Ticket.GetCandidateFingerprint()
				!= CandidateFingerprint(
					Binding,
					DescriptorIdentity))
		{
			SetError(
				OutError,
				TEXT("candidate_fingerprint_mismatch"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket no longer binds the supplied "
					"candidate."));
			return false;
		}
		if (Ticket.GetContextFingerprint()
			!= ContextFingerprint(CallerContext))
		{
			SetError(
				OutError,
				TEXT("ticket_context_mismatch"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket caller or authority scopes "
					"differ."));
			return false;
		}
		const double Now = Clock();
		if (!FMath::IsFinite(Now)
			|| Now >= Ticket.GetExpiresAt())
		{
			SetError(
				OutError,
				TEXT("ticket_expired"),
				Ticket.GetTicketId(),
				TEXT("Ticket has expired."));
			return false;
		}
		if (!ConstantTimeEquals(
				Ticket.GetSignature(),
				Sign(Ticket)))
		{
			SetError(
				OutError,
				TEXT("ticket_integrity_mismatch"),
				Ticket.GetTicketId(),
				TEXT("Ticket signature is invalid."));
			return false;
		}
		return true;
	}
}
