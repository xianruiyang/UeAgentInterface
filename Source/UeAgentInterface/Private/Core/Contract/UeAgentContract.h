#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::Contract
{
	class UEAGENTINTERFACE_API FUeAgentDescriptorIdentity final
	{
	public:
		FUeAgentDescriptorIdentity(
			FString InDescriptorId,
			int32 InVersion,
			FString InDescriptorHash);

		const FString& GetDescriptorId() const;
		int32 GetVersion() const;
		const FString& GetDescriptorHash() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

		bool operator==(const FUeAgentDescriptorIdentity& Other) const;

	private:
		FString DescriptorId;
		int32 Version = 0;
		FString DescriptorHash;
		FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FUeAgentContractValue final
	{
	public:
		explicit FUeAgentContractValue(FString InJsonValue);

		const FString& GetJsonValue() const;
		bool IsValid() const;

		bool operator==(const FUeAgentContractValue& Other) const;

	private:
		FString JsonValue;
	};

	class UEAGENTINTERFACE_API FUeAgentQueryRequest final
	{
	public:
		FUeAgentQueryRequest(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FUeAgentContractValue InPayload);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FUeAgentContractValue& GetPayload() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FUeAgentContractValue Payload;
		FDiagnostic ValidationDiagnostic;
	};

	template <typename TPayload>
	class TUeAgentQueryRequest final
	{
	public:
		TUeAgentQueryRequest(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			TPayload InPayload)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
			, RequestId(MoveTemp(InRequestId))
			, Payload(MoveTemp(InPayload))
		{
			RequestId.TrimStartAndEndInline();
		}

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const
		{
			return DescriptorIdentity;
		}

		const FString& GetRequestId() const
		{
			return RequestId;
		}

		const TPayload& GetPayload() const
		{
			return Payload;
		}

		bool IsValid() const
		{
			return DescriptorIdentity.IsValid()
				&& !RequestId.IsEmpty()
				&& Payload.IsValid();
		}

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		TPayload Payload;
	};

	class UEAGENTINTERFACE_API FUeAgentQueryResult final
	{
	public:
		FUeAgentQueryResult(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FUeAgentContractValue InValue, TArray<FDiagnostic> InDiagnostics = {});
		FUeAgentQueryResult(FUeAgentDescriptorIdentity InDescriptorIdentity, FString InRequestId, FDiagnostic InDiagnostic);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FUeAgentContractValue& GetValue() const;
		const FDiagnostic& GetDiagnostic() const;
		const TArray<FDiagnostic>& GetDiagnostics() const;
		bool HasDiagnostic() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const;

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FUeAgentContractValue Value;
		FDiagnostic Diagnostic;
		TArray<FDiagnostic> Diagnostics;
		FDiagnostic ValidationDiagnostic;
	};

	template <typename TValue>
	class TUeAgentQueryResult final
	{
	public:
		TUeAgentQueryResult(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			TValue InValue)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
			, RequestId(MoveTemp(InRequestId))
			, Value(MoveTemp(InValue))
		{
			RequestId.TrimStartAndEndInline();
		}

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const
		{
			return DescriptorIdentity;
		}

		const FString& GetRequestId() const
		{
			return RequestId;
		}

		const TValue& GetValue() const
		{
			return Value;
		}

		bool IsValid() const
		{
			return DescriptorIdentity.IsValid()
				&& !RequestId.IsEmpty()
				&& Value.IsValid();
		}

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		TValue Value;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationRequest final
	{
	public:
		FUeAgentOperationRequest(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FString InIdempotencyKey,
			FUeAgentContractValue InPayload);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FString& GetIdempotencyKey() const;
		const FUeAgentContractValue& GetPayload() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FString IdempotencyKey;
		FUeAgentContractValue Payload;
		FDiagnostic ValidationDiagnostic;
	};

	template <typename TPayload>
	class TUeAgentOperationRequest final
	{
	public:
		TUeAgentOperationRequest(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FString InIdempotencyKey,
			TPayload InPayload)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
			, RequestId(MoveTemp(InRequestId))
			, IdempotencyKey(MoveTemp(InIdempotencyKey))
			, Payload(MoveTemp(InPayload))
		{
			RequestId.TrimStartAndEndInline();
			IdempotencyKey.TrimStartAndEndInline();
		}

		const FUeAgentDescriptorIdentity&
		GetDescriptorIdentity() const
		{
			return DescriptorIdentity;
		}

		const FString& GetRequestId() const
		{
			return RequestId;
		}

		const FString& GetIdempotencyKey() const
		{
			return IdempotencyKey;
		}

		const TPayload& GetPayload() const
		{
			return Payload;
		}

		bool IsValid() const
		{
			return DescriptorIdentity.IsValid()
				&& !RequestId.IsEmpty()
				&& !IdempotencyKey.IsEmpty()
				&& Payload.IsValid();
		}

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FString IdempotencyKey;
		TPayload Payload;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationResult final
	{
	public:
		FUeAgentOperationResult(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			FUeAgentContractValue InValue, TArray<FDiagnostic> InDiagnostics = {});
		FUeAgentOperationResult(FUeAgentDescriptorIdentity InDescriptorIdentity, FString InRequestId, FDiagnostic InDiagnostic);

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const;
		const FString& GetRequestId() const;
		const FUeAgentContractValue& GetValue() const;
		const FDiagnostic& GetDiagnostic() const;
		const TArray<FDiagnostic>& GetDiagnostics() const;
		bool HasDiagnostic() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const;

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		FUeAgentContractValue Value;
		FDiagnostic Diagnostic;
		TArray<FDiagnostic> Diagnostics;
		FDiagnostic ValidationDiagnostic;
	};

	template <typename TValue>
	class TUeAgentOperationResult final
	{
	public:
		TUeAgentOperationResult(
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InRequestId,
			TValue InValue)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
			, RequestId(MoveTemp(InRequestId))
			, Value(MoveTemp(InValue))
		{
			RequestId.TrimStartAndEndInline();
		}

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const
		{
			return DescriptorIdentity;
		}

		const FString& GetRequestId() const
		{
			return RequestId;
		}

		const TValue& GetValue() const
		{
			return Value;
		}

		bool IsValid() const
		{
			return DescriptorIdentity.IsValid()
				&& !RequestId.IsEmpty()
				&& Value.IsValid();
		}

	private:
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FString RequestId;
		TValue Value;
	};

	class UEAGENTINTERFACE_API FUeAgentCallerContext final
	{
	public:
		FUeAgentCallerContext(
			FString InCallerId,
			TArray<FString> InAuthorityScopes,
			FString InCorrelationId);

		const FString& GetCallerId() const;
		const TArray<FString>& GetAuthorityScopes() const;
		const FString& GetCorrelationId() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString CallerId;
		TArray<FString> AuthorityScopes;
		FString CorrelationId;
		FDiagnostic ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentCancellationSignal
	{
	public:
		virtual ~IUeAgentCancellationSignal() = default;

		virtual bool IsCancelled() const = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentQueryApplicationPort
	{
	public:
		virtual ~IUeAgentQueryApplicationPort() = default;

		virtual FUeAgentQueryResult Submit(
			const FUeAgentQueryRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentOperationApplicationPort
	{
	public:
		virtual ~IUeAgentOperationApplicationPort() = default;

		virtual FUeAgentOperationResult Submit(
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) = 0;
	};
}
