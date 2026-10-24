#include "Core/Contract/UeAgentContract.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Contract
{
	namespace
	{
		static FString RequiredText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FDiagnostic ContractValidationFailure(const TCHAR* Code, const TCHAR* Reason, const TCHAR* Path)
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("contract_validation"));
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}
		FDiagnostic ValidateResult(const FUeAgentDescriptorIdentity& Identity, const FString& RequestId,
			const FUeAgentContractValue& Value, const FDiagnostic& Diagnostic)
		{
			if (!Identity.IsValid()) return Identity.GetValidationDiagnostic();
			if (RequestId.IsEmpty()) return ContractValidationFailure(TEXT("result_request_id_missing"),
				TEXT("The result requires a non-empty request identifier."), TEXT("/request_id"));
			if (Diagnostic.IsEmpty() && !Value.IsValid()) return ContractValidationFailure(TEXT("result_value_missing"),
				TEXT("The result contains neither a value nor a failure diagnostic."), TEXT("/value"));
			return {};
		}

		FDiagnostic ValidateRequest(const FUeAgentDescriptorIdentity& Identity, const FString& RequestId,
			const FUeAgentContractValue& Payload, const FString* IdempotencyKey = nullptr)
		{
			if (!Identity.IsValid()) return Identity.GetValidationDiagnostic();
			FDiagnostic Diagnostic;
			if (RequestId.IsEmpty())
				Diagnostic = ContractValidationFailure(TEXT("request_id_missing"),
					TEXT("The request requires a non-empty request identifier."), TEXT("/request_id"));
			else if (IdempotencyKey && IdempotencyKey->IsEmpty())
				Diagnostic = ContractValidationFailure(TEXT("request_idempotency_key_missing"),
					TEXT("The operation request requires a non-empty idempotency key."), TEXT("/idempotency_key"));
			else if (!Payload.IsValid())
				Diagnostic = ContractValidationFailure(TEXT("request_payload_missing"),
					TEXT("The request payload must contain a non-empty JSON document."), TEXT("/payload"));
			if (!Diagnostic.IsEmpty())
			{
				Diagnostic.SourceKind = TEXT("native_request");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(FString());
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			}
			return Diagnostic;
		}

	}

	FUeAgentDescriptorIdentity::FUeAgentDescriptorIdentity(
		FString InDescriptorId,
		const int32 InVersion,
		FString InDescriptorHash)
		: DescriptorId(RequiredText(MoveTemp(InDescriptorId)))
		, Version(InVersion)
		, DescriptorHash(RequiredText(MoveTemp(InDescriptorHash)))
	{
		if (DescriptorId.IsEmpty())
			ValidationDiagnostic = ContractValidationFailure(TEXT("descriptor_identity_id_missing"),
				TEXT("The operation descriptor requires a non-empty identifier."), TEXT("/descriptor_id"));
		else if (Version <= 0)
		{
			ValidationDiagnostic = ContractValidationFailure(TEXT("descriptor_identity_version_invalid"),
				TEXT("The operation descriptor version must be a positive integer."), TEXT("/version"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(Version);
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive integer"));
		}
		else if (!DescriptorHash.StartsWith(TEXT("sha256:")) || DescriptorHash.Len() != 71)
		{
			ValidationDiagnostic = ContractValidationFailure(TEXT("descriptor_identity_hash_invalid"),
				TEXT("The operation descriptor requires a sha256 digest with a 64-character payload."), TEXT("/descriptor_hash"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(DescriptorHash);
		}
	}

	const FString& FUeAgentDescriptorIdentity::GetDescriptorId() const
	{
		return DescriptorId;
	}

	int32 FUeAgentDescriptorIdentity::GetVersion() const
	{
		return Version;
	}

	const FString& FUeAgentDescriptorIdentity::GetDescriptorHash() const
	{
		return DescriptorHash;
	}

	bool FUeAgentDescriptorIdentity::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	bool FUeAgentDescriptorIdentity::operator==(
		const FUeAgentDescriptorIdentity& Other) const
	{
		return DescriptorId == Other.DescriptorId
			&& Version == Other.Version
			&& DescriptorHash == Other.DescriptorHash;
	}

	FUeAgentContractValue::FUeAgentContractValue(FString InJsonValue)
		: JsonValue(MoveTemp(InJsonValue))
	{
		JsonValue.TrimStartAndEndInline();
	}

	const FString& FUeAgentContractValue::GetJsonValue() const
	{
		return JsonValue;
	}

	bool FUeAgentContractValue::IsValid() const
	{
		return !JsonValue.IsEmpty();
	}

	bool FUeAgentContractValue::operator==(
		const FUeAgentContractValue& Other) const
	{
		return JsonValue == Other.JsonValue;
	}

	FUeAgentQueryRequest::FUeAgentQueryRequest(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FUeAgentContractValue InPayload)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Payload(MoveTemp(InPayload))
	{
		ValidationDiagnostic = ValidateRequest(DescriptorIdentity, RequestId, Payload);
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentQueryRequest::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentQueryRequest::GetRequestId() const
	{
		return RequestId;
	}

	const FUeAgentContractValue& FUeAgentQueryRequest::GetPayload() const
	{
		return Payload;
	}

	bool FUeAgentQueryRequest::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	FUeAgentQueryResult::FUeAgentQueryResult(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FUeAgentContractValue InValue, TArray<FDiagnostic> InDiagnostics)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(MoveTemp(InValue))
		, Diagnostics(MoveTemp(InDiagnostics))
	{
		ValidationDiagnostic = ValidateResult(DescriptorIdentity, RequestId, Value, Diagnostic);
	}

	FUeAgentQueryResult::FUeAgentQueryResult(FUeAgentDescriptorIdentity InDescriptorIdentity, FString InRequestId, FDiagnostic InDiagnostic)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(FString())
		, Diagnostic(MoveTemp(InDiagnostic))
	{
		ValidationDiagnostic = ValidateResult(DescriptorIdentity, RequestId, Value, Diagnostic);
	}

	const FDiagnostic& FUeAgentQueryResult::GetDiagnostic() const { return Diagnostic; }
	const TArray<FDiagnostic>& FUeAgentQueryResult::GetDiagnostics() const { return Diagnostics; }
	bool FUeAgentQueryResult::HasDiagnostic() const { return !Diagnostic.IsEmpty(); }


	const FUeAgentDescriptorIdentity&
	FUeAgentQueryResult::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentQueryResult::GetRequestId() const
	{
		return RequestId;
	}

	const FUeAgentContractValue& FUeAgentQueryResult::GetValue() const
	{
		return Value;
	}

	bool FUeAgentQueryResult::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentQueryResult::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentOperationRequest::FUeAgentOperationRequest(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FString InIdempotencyKey,
		FUeAgentContractValue InPayload)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, IdempotencyKey(RequiredText(MoveTemp(InIdempotencyKey)))
		, Payload(MoveTemp(InPayload))
	{
		ValidationDiagnostic = ValidateRequest(DescriptorIdentity, RequestId, Payload, &IdempotencyKey);
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentOperationRequest::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentOperationRequest::GetRequestId() const
	{
		return RequestId;
	}

	const FString& FUeAgentOperationRequest::GetIdempotencyKey() const
	{
		return IdempotencyKey;
	}

	const FUeAgentContractValue&
	FUeAgentOperationRequest::GetPayload() const
	{
		return Payload;
	}

	bool FUeAgentOperationRequest::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	FUeAgentOperationResult::FUeAgentOperationResult(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FUeAgentContractValue InValue, TArray<FDiagnostic> InDiagnostics)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(MoveTemp(InValue))
		, Diagnostics(MoveTemp(InDiagnostics))
	{
		ValidationDiagnostic = ValidateResult(DescriptorIdentity, RequestId, Value, Diagnostic);
	}

	FUeAgentOperationResult::FUeAgentOperationResult(FUeAgentDescriptorIdentity InDescriptorIdentity, FString InRequestId, FDiagnostic InDiagnostic)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(FString())
		, Diagnostic(MoveTemp(InDiagnostic))
	{
		ValidationDiagnostic = ValidateResult(DescriptorIdentity, RequestId, Value, Diagnostic);
	}

	const FDiagnostic& FUeAgentOperationResult::GetDiagnostic() const { return Diagnostic; }
	const TArray<FDiagnostic>& FUeAgentOperationResult::GetDiagnostics() const { return Diagnostics; }
	bool FUeAgentOperationResult::HasDiagnostic() const { return !Diagnostic.IsEmpty(); }


	const FUeAgentDescriptorIdentity&
	FUeAgentOperationResult::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentOperationResult::GetRequestId() const
	{
		return RequestId;
	}

	const FUeAgentContractValue& FUeAgentOperationResult::GetValue() const
	{
		return Value;
	}

	bool FUeAgentOperationResult::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FDiagnostic& FUeAgentOperationResult::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentCallerContext::FUeAgentCallerContext(
		FString InCallerId,
		TArray<FString> InAuthorityScopes,
		FString InCorrelationId)
		: CallerId(RequiredText(MoveTemp(InCallerId)))
		, AuthorityScopes(MoveTemp(InAuthorityScopes))
		, CorrelationId(RequiredText(MoveTemp(InCorrelationId)))
	{
		if (CallerId.IsEmpty())
			ValidationDiagnostic = ContractValidationFailure(TEXT("caller_context_id_missing"),
				TEXT("The caller context requires a non-empty caller identifier."), TEXT("/caller_id"));
		else if (CorrelationId.IsEmpty())
			ValidationDiagnostic = ContractValidationFailure(TEXT("caller_context_correlation_missing"),
				TEXT("The caller context requires a non-empty correlation identifier."), TEXT("/correlation_id"));
		for (FString& Scope : AuthorityScopes)
		{
			Scope = RequiredText(MoveTemp(Scope));
		}
		AuthorityScopes.RemoveAll(
			[](const FString& Scope)
			{
				return Scope.IsEmpty();
			});
		AuthorityScopes.Sort();
		for (int32 Index = AuthorityScopes.Num() - 1; Index > 0; --Index)
		{
			if (AuthorityScopes[Index] == AuthorityScopes[Index - 1])
			{
				AuthorityScopes.RemoveAt(Index);
			}
		}
	}

	const FString& FUeAgentCallerContext::GetCallerId() const
	{
		return CallerId;
	}

	const TArray<FString>&
	FUeAgentCallerContext::GetAuthorityScopes() const
	{
		return AuthorityScopes;
	}

	const FString& FUeAgentCallerContext::GetCorrelationId() const
	{
		return CorrelationId;
	}

	bool FUeAgentCallerContext::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}
}
