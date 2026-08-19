#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::Contract
{
	namespace
	{
		static FString RequiredText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
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
		return !DescriptorId.IsEmpty()
			&& Version > 0
			&& DescriptorHash.StartsWith(TEXT("sha256:"))
			&& DescriptorHash.Len() == 71;
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
		return DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& Payload.IsValid();
	}

	FUeAgentQueryResult::FUeAgentQueryResult(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FUeAgentContractValue InValue)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(MoveTemp(InValue))
	{
	}

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
		return DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& Value.IsValid();
	}

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
		return DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& !IdempotencyKey.IsEmpty()
			&& Payload.IsValid();
	}

	FUeAgentOperationResult::FUeAgentOperationResult(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		FUeAgentContractValue InValue)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(RequiredText(MoveTemp(InRequestId)))
		, Value(MoveTemp(InValue))
	{
	}

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
		return DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& Value.IsValid();
	}

	FUeAgentCallerContext::FUeAgentCallerContext(
		FString InCallerId,
		TArray<FString> InAuthorityScopes,
		FString InCorrelationId)
		: CallerId(RequiredText(MoveTemp(InCallerId)))
		, AuthorityScopes(MoveTemp(InAuthorityScopes))
		, CorrelationId(RequiredText(MoveTemp(InCorrelationId)))
	{
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
		return !CallerId.IsEmpty() && !CorrelationId.IsEmpty();
	}
}
