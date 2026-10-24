#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"

#include "Dom/JsonValue.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::BindingGateway::Ticket
{
	namespace
	{
		using UE::UAI::Discovery::EUeAgentCandidateOperation;

		FUeAgentOperationTicketError InvalidTicketField(
			const TCHAR* Code, const TCHAR* Reason, const TCHAR* Path, const FString& InternalValue)
		{
			auto Error = FUeAgentOperationTicketError::Failure(Code, Reason, TEXT("binding_ticket"));
			Error.PropertyPath = Path;
			Error.ActualEvidence = MakeShared<FJsonValueString>(InternalValue);
			return Error;
		}

		void SetFailure(
			FUeAgentOperationTicketError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = FUeAgentOperationTicketError::Failure(Code, Detail, TEXT("binding_ticket"), FString(), SubjectId);
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
		if (ServerSessionId.IsEmpty())
			ValidationDiagnostic = InvalidTicketField(TEXT("operation_session_invalid"), TEXT("The operation session has no server session identifier."), TEXT("server_session_id"), ServerSessionId);
		else if (RegistryEpoch < 0)
			ValidationDiagnostic = InvalidTicketField(TEXT("operation_session_invalid"), TEXT("The operation session registry epoch must be non-negative."), TEXT("registry_epoch"), FString::Printf(TEXT("%lld"), RegistryEpoch));
		else if (!RegistryHash.StartsWith(TEXT("sha256:")))
			ValidationDiagnostic = InvalidTicketField(TEXT("operation_session_invalid"), TEXT("The operation session registry hash must start with 'sha256:'."), TEXT("registry_hash"), RegistryHash);
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentOperationTicketError& FUeAgentOperationSessionIdentity::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
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
		auto Reject = [this](const TCHAR* Reason, const TCHAR* Path, const FString& Actual)
		{
			ValidationDiagnostic = InvalidTicketField(TEXT("operation_ticket_invalid"), Reason, Path, Actual);
		};
		if (TicketId.IsEmpty()) Reject(TEXT("The operation ticket has no identifier."), TEXT("ticket_id"), TicketId);
		else if (!DescriptorIdentity.IsValid()) ValidationDiagnostic = DescriptorIdentity.GetValidationDiagnostic();
		else if (!CandidateIdentity.IsValid()) ValidationDiagnostic = CandidateIdentity.GetValidationDiagnostic();
		else if (CandidateBindingId.IsEmpty()) Reject(TEXT("The operation ticket has no candidate binding identifier."), TEXT("candidate_binding_id"), CandidateBindingId);
		else if (!CandidateFingerprint.StartsWith(TEXT("sha256:"))) Reject(TEXT("The ticket candidate fingerprint must start with 'sha256:'."), TEXT("candidate_fingerprint"), CandidateFingerprint);
		else if (!CatalogRevision.IsValid()) ValidationDiagnostic = *CatalogRevision.GetValidationDiagnostic();
		else if (ServerSessionId.IsEmpty()) Reject(TEXT("The operation ticket has no server session identifier."), TEXT("server_session_id"), ServerSessionId);
		else if (RegistryEpoch < 0) Reject(TEXT("The ticket registry epoch must be non-negative."), TEXT("registry_epoch"), FString::Printf(TEXT("%lld"), RegistryEpoch));
		else if (!RegistryHash.StartsWith(TEXT("sha256:"))) Reject(TEXT("The ticket registry hash must start with 'sha256:'."), TEXT("registry_hash"), RegistryHash);
		else if (!ContextFingerprint.StartsWith(TEXT("sha256:"))) Reject(TEXT("The ticket context fingerprint must start with 'sha256:'."), TEXT("context_fingerprint"), ContextFingerprint);
		else if (!Signature.StartsWith(TEXT("sha256:"))) Reject(TEXT("The ticket signature must start with 'sha256:'."), TEXT("signature"), Signature);
		else if (!(IssuedAt >= 0.0)) Reject(TEXT("The ticket issue time must be non-negative."), TEXT("issued_at"), FString::Printf(TEXT("%.17g"), IssuedAt));
		else if (!(ExpiresAt > IssuedAt))
		{
			Reject(TEXT("The ticket expiry time must be later than its issue time."), TEXT("expires_at"), FString::Printf(TEXT("%.17g"), ExpiresAt));
			ValidationDiagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("greater than %.17g"), IssuedAt));
		}
		else if (TargetRevision.IsSet() && !TargetRevision->IsValid()) ValidationDiagnostic = TargetRevision->GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentOperationTicketError& FUeAgentOperationTicket::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
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
		if (IntegrityKey.TrimStartAndEnd().IsEmpty())
			OutError = InvalidTicketField(TEXT("ticket_service_invalid"), TEXT("Ticket signing requires a non-empty integrity key."), TEXT("integrity_key"), FString());
		else if (TtlSeconds <= 0.0)
		{
			OutError = FUeAgentOperationTicketError::Failure(TEXT("ticket_service_invalid"), TEXT("Ticket lifetime must be positive."), TEXT("binding_ticket"));
			OutError.PropertyPath = TEXT("ttl_seconds");
			OutError.ActualValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), TtlSeconds));
		}
		else if (!Clock)
			OutError = InvalidTicketField(TEXT("ticket_service_invalid"), TEXT("The ticket service requires a host clock."), TEXT("clock"), TEXT("missing"));
		else if (!TicketIdFactory)
			OutError = InvalidTicketField(TEXT("ticket_service_invalid"), TEXT("The ticket service requires an identifier factory."), TEXT("ticket_id_factory"), TEXT("missing"));
		if (!OutError.IsEmpty()) return false;
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
		if (!Binding.IsValid())
		{
			OutError = *Binding.GetValidationDiagnostic();
			return false;
		}
		if (!DescriptorIdentity.IsValid()) OutError = DescriptorIdentity.GetValidationDiagnostic();
		else if (!Session.IsValid()) OutError = Session.GetValidationDiagnostic();
		else if (!CallerContext.IsValid()) OutError = CallerContext.GetValidationDiagnostic();
		else if (TargetRevision.IsSet() && !TargetRevision->IsValid()) OutError = TargetRevision->GetValidationDiagnostic();
		if (!OutError.IsEmpty()) return false;
		const FString NewTicketId = TicketIdFactory();
		const double Now = Clock();
		if (NewTicketId.TrimStartAndEnd().IsEmpty())
		{
			OutError = InvalidTicketField(TEXT("ticket_host_invalid"), TEXT("The ticket-id factory returned an empty identifier."), TEXT("ticket_id"), NewTicketId);
			return false;
		}
		if (!FMath::IsFinite(Now) || Now < 0.0)
		{
			OutError = InvalidTicketField(TEXT("ticket_host_invalid"), TEXT("The host clock must return a finite, non-negative time."), TEXT("issued_at"), FString::Printf(TEXT("%.17g"), Now));
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
			OutError = Signed.GetValidationDiagnostic();
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
		if (!Ticket.IsValid()) OutError = Ticket.GetValidationDiagnostic();
		else if (!Binding.IsValid()) OutError = *Binding.GetValidationDiagnostic();
		else if (!DescriptorIdentity.IsValid()) OutError = DescriptorIdentity.GetValidationDiagnostic();
		else if (!Session.IsValid()) OutError = Session.GetValidationDiagnostic();
		else if (!CallerContext.IsValid()) OutError = CallerContext.GetValidationDiagnostic();
		if (!OutError.IsEmpty()) return false;
		auto Mismatch = [&](const TCHAR* Path, const FString& Actual, const FString& Expected)
		{
			OutError.PropertyPath = Path;
			OutError.TargetPath = Binding.GetPublicRef();
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
		};
		if (!(Ticket.GetDescriptorIdentity()
			== DescriptorIdentity))
		{
			SetFailure(
				OutError,
				TEXT("descriptor_hash_mismatch"),
				DescriptorIdentity.GetDescriptorId(),
				TEXT(
					"Ticket descriptor identity differs from "
					"the active descriptor."));
			Mismatch(TEXT("descriptor"), Ticket.GetDescriptorIdentity().GetDescriptorId() + TEXT(":") + FString::FromInt(Ticket.GetDescriptorIdentity().GetVersion()) + TEXT(":") + Ticket.GetDescriptorIdentity().GetDescriptorHash(), DescriptorIdentity.GetDescriptorId() + TEXT(":") + FString::FromInt(DescriptorIdentity.GetVersion()) + TEXT(":") + DescriptorIdentity.GetDescriptorHash());
			return false;
		}
		if (Ticket.GetServerSessionId()
			!= Session.GetServerSessionId())
		{
			SetFailure(
				OutError,
				TEXT("server_session_stale"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket belongs to another server "
					"session."));
			Mismatch(TEXT("server_session_id"), Ticket.GetServerSessionId(), Session.GetServerSessionId());
			return false;
		}
		if (Ticket.GetRegistryEpoch()
			!= Session.GetRegistryEpoch())
		{
			SetFailure(
				OutError,
				TEXT("registry_epoch_stale"),
				Ticket.GetTicketId(),
				TEXT("Ticket registry epoch is stale."));
			Mismatch(TEXT("registry_epoch"), FString::Printf(TEXT("%lld"), Ticket.GetRegistryEpoch()), FString::Printf(TEXT("%lld"), Session.GetRegistryEpoch()));
			return false;
		}
		if (Ticket.GetRegistryHash()
			!= Session.GetRegistryHash())
		{
			SetFailure(
				OutError,
				TEXT("registry_hash_mismatch"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket registry hash differs from the "
					"active registry."));
			Mismatch(TEXT("registry_hash"), Ticket.GetRegistryHash(), Session.GetRegistryHash());
			return false;
		}
		if (Ticket.GetCatalogRevision()
			!= Binding.GetCatalogRevision())
		{
			SetFailure(
				OutError,
				TEXT("catalog_revision_stale"),
				Ticket.GetTicketId(),
				TEXT("Ticket catalog revision is stale."));
			Mismatch(TEXT("catalog_revision"), Ticket.GetCatalogRevision().GetSourceId() + TEXT(":") + Ticket.GetCatalogRevision().GetGeneration() + TEXT(":") + FString::Printf(TEXT("%lld"), Ticket.GetCatalogRevision().GetSequence()), Binding.GetCatalogRevision().GetSourceId() + TEXT(":") + Binding.GetCatalogRevision().GetGeneration() + TEXT(":") + FString::Printf(TEXT("%lld"), Binding.GetCatalogRevision().GetSequence()));
			return false;
		}
		if (Ticket.GetCandidateBindingId() != Binding.GetBindingId())
		{
			SetFailure(OutError, TEXT("candidate_fingerprint_mismatch"), Ticket.GetTicketId(), TEXT("The ticket refers to a different candidate binding."));
			Mismatch(TEXT("candidate_binding_id"), Ticket.GetCandidateBindingId(), Binding.GetBindingId());
			return false;
		}
		if (!(Ticket.GetCandidateIdentity() == Binding.GetSummary().GetIdentity()))
		{
			SetFailure(OutError, TEXT("candidate_fingerprint_mismatch"), Ticket.GetTicketId(), TEXT("The ticket candidate identity no longer matches the resolved candidate."));
			Mismatch(TEXT("candidate_identity"), Ticket.GetCandidateIdentity().GetCanonicalKey(), Binding.GetSummary().GetIdentity().GetCanonicalKey());
			return false;
		}
		const FString ExpectedCandidateFingerprint = CandidateFingerprint(Binding, DescriptorIdentity);
		if (Ticket.GetCandidateFingerprint() != ExpectedCandidateFingerprint)
		{
			SetFailure(OutError, TEXT("candidate_fingerprint_mismatch"), Ticket.GetTicketId(), TEXT("The candidate has changed since the ticket was issued."));
			Mismatch(TEXT("candidate_fingerprint"), Ticket.GetCandidateFingerprint(), ExpectedCandidateFingerprint);
			return false;
		}
		if (Ticket.GetContextFingerprint()
			!= ContextFingerprint(CallerContext))
		{
			SetFailure(
				OutError,
				TEXT("ticket_context_mismatch"),
				Ticket.GetTicketId(),
				TEXT(
					"Ticket caller or authority scopes "
					"differ."));
			Mismatch(TEXT("context_fingerprint"), Ticket.GetContextFingerprint(), ContextFingerprint(CallerContext));
			return false;
		}
		const double Now = Clock();
		if (!FMath::IsFinite(Now))
		{
			OutError = InvalidTicketField(TEXT("ticket_host_invalid"), TEXT("The host clock returned a non-finite time during ticket validation."), TEXT("clock"), FString::Printf(TEXT("%.17g"), Now));
			return false;
		}
		if (Now >= Ticket.GetExpiresAt())
		{
			SetFailure(OutError, TEXT("ticket_expired"), Ticket.GetTicketId(), TEXT("The ticket has expired; resolve the candidate again before submitting."));
			OutError.PropertyPath = TEXT("expires_at");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Now);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("time before %.17g"), Ticket.GetExpiresAt()));
			return false;
		}
		if (!ConstantTimeEquals(
				Ticket.GetSignature(),
				Sign(Ticket)))
		{
			SetFailure(
				OutError,
				TEXT("ticket_integrity_mismatch"),
				Ticket.GetTicketId(),
				TEXT("Ticket signature is invalid."));
			Mismatch(TEXT("signature"), Ticket.GetSignature(), Sign(Ticket));
			return false;
		}
		return true;
	}
}
