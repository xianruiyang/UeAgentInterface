#include "UaiProtocol/UeAgentUaiProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Protocol
{
	using namespace Contract;
	using namespace NetworkTransport;

	namespace
	{
		static const FString WireSchema(TEXT("uai.wire.v1"));
		static constexpr int32 WireVersion = 1;

		static FUeAgentProtocolDiagnostic WireError(
			const FString& Code,
			const FString& Message)
		{
			return FUeAgentProtocolDiagnostic(
				EUeAgentProtocolErrorLayer::Wire,
				Code,
				Message);
		}

		static FUeAgentProtocolDiagnostic TransportError(
			const FString& Code,
			const FString& Message,
			const EUeAgentTransportDeliveryState State)
		{
			return FUeAgentProtocolDiagnostic(
				EUeAgentProtocolErrorLayer::Transport,
				Code,
				Message,
				State);
		}

		static FString RequiredText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		static FString MessageKindText(
			const EUeAgentWireMessageKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentWireMessageKind::QueryRequest:
				return TEXT("query_request");
			case EUeAgentWireMessageKind::QueryResult:
				return TEXT("query_result");
			case EUeAgentWireMessageKind::OperationRequest:
				return TEXT("operation_request");
			case EUeAgentWireMessageKind::OperationResult:
				return TEXT("operation_result");
			case EUeAgentWireMessageKind::ProtocolError:
				return TEXT("protocol_error");
			default:
				return FString();
			}
		}

		static bool TryParseMessageKind(
			const FString& Text,
			EUeAgentWireMessageKind& OutKind)
		{
			if (Text == TEXT("query_request"))
			{
				OutKind = EUeAgentWireMessageKind::QueryRequest;
				return true;
			}
			if (Text == TEXT("query_result"))
			{
				OutKind = EUeAgentWireMessageKind::QueryResult;
				return true;
			}
			if (Text == TEXT("operation_request"))
			{
				OutKind = EUeAgentWireMessageKind::OperationRequest;
				return true;
			}
			if (Text == TEXT("operation_result"))
			{
				OutKind = EUeAgentWireMessageKind::OperationResult;
				return true;
			}
			if (Text == TEXT("protocol_error"))
			{
				OutKind = EUeAgentWireMessageKind::ProtocolError;
				return true;
			}
			return false;
		}

		static FString DeliveryStateText(
			const EUeAgentTransportDeliveryState State)
		{
			switch (State)
			{
			case EUeAgentTransportDeliveryState::Delivered:
				return TEXT("delivered");
			case EUeAgentTransportDeliveryState::Backpressured:
				return TEXT("backpressured");
			case EUeAgentTransportDeliveryState::TimedOut:
				return TEXT("timed_out");
			case EUeAgentTransportDeliveryState::Disconnected:
				return TEXT("disconnected");
			case EUeAgentTransportDeliveryState::Failed:
				return TEXT("failed");
			case EUeAgentTransportDeliveryState::Unsupported:
				return TEXT("unsupported");
			default:
				return FString();
			}
		}

		static bool TryParseDeliveryState(
			const FString& Text,
			EUeAgentTransportDeliveryState& OutState)
		{
			if (Text == TEXT("delivered"))
			{
				OutState = EUeAgentTransportDeliveryState::Delivered;
				return true;
			}
			if (Text == TEXT("backpressured"))
			{
				OutState = EUeAgentTransportDeliveryState::Backpressured;
				return true;
			}
			if (Text == TEXT("timed_out"))
			{
				OutState = EUeAgentTransportDeliveryState::TimedOut;
				return true;
			}
			if (Text == TEXT("disconnected"))
			{
				OutState = EUeAgentTransportDeliveryState::Disconnected;
				return true;
			}
			if (Text == TEXT("failed"))
			{
				OutState = EUeAgentTransportDeliveryState::Failed;
				return true;
			}
			if (Text == TEXT("unsupported"))
			{
				OutState = EUeAgentTransportDeliveryState::Unsupported;
				return true;
			}
			return false;
		}

		static FString TransportFailureCode(
			const EUeAgentTransportDeliveryState State)
		{
			switch (State)
			{
			case EUeAgentTransportDeliveryState::Backpressured:
				return TEXT("transport_backpressured");
			case EUeAgentTransportDeliveryState::TimedOut:
				return TEXT("transport_timed_out");
			case EUeAgentTransportDeliveryState::Disconnected:
				return TEXT("transport_disconnected");
			case EUeAgentTransportDeliveryState::Unsupported:
				return TEXT("transport_unsupported");
			case EUeAgentTransportDeliveryState::Failed:
			default:
				return TEXT("transport_failed");
			}
		}

		static bool HasExactFields(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Expected,
			const FString& Subject,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!Object.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_object_type_invalid"),
					Subject + TEXT(" must be an object"));
				return false;
			}

			TSet<FString> ExpectedSet(Expected);
			TArray<FString> Actual;
			Object->Values.GetKeys(Actual);
			for (const FString& Field : Expected)
			{
				if (!Object->Values.Contains(Field))
				{
					OutDiagnostic = WireError(
						TEXT("wire_field_missing"),
						Subject + TEXT(" missing field: ") + Field);
					return false;
				}
			}
			for (const FString& Field : Actual)
			{
				if (!ExpectedSet.Contains(Field))
				{
					OutDiagnostic = WireError(
						TEXT("wire_field_unknown"),
						Subject + TEXT(" unknown field: ") + Field);
					return false;
				}
			}
			return true;
		}

		static bool TryGetRequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			FString& OutValue,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, OutValue))
			{
				OutDiagnostic = WireError(
					TEXT("wire_text_invalid"),
					Field + TEXT(" must be text"));
				return false;
			}
			OutValue = RequiredText(MoveTemp(OutValue));
			if (OutValue.IsEmpty())
			{
				OutDiagnostic = WireError(
					TEXT("wire_text_invalid"),
					Field + TEXT(" must be non-empty"));
				return false;
			}
			return true;
		}

		static bool ParseUtf8Object(
			const TArray<uint8>& Payload,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			OutObject.Reset();
			if (Payload.IsEmpty())
			{
				OutDiagnostic = WireError(
					TEXT("wire_json_malformed"),
					TEXT("wire payload is empty"));
				return false;
			}
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(Payload.GetData()),
				Payload.Num());
			const FString Text(Converted.Length(), Converted.Get());
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_json_malformed"),
					TEXT("wire payload is not a JSON object"));
				return false;
			}
			return true;
		}

		static bool SerializeScalar(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText)
		{
			TArray<TSharedPtr<FJsonValue>> Wrapper;
			Wrapper.Add(Value);
			FString Wrapped;
			const TSharedRef<
				TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Wrapped);
			if (!FJsonSerializer::Serialize(Wrapper, Writer)
				|| Wrapped.Len() < 2)
			{
				return false;
			}
			OutText = Wrapped.Mid(1, Wrapped.Len() - 2);
			return true;
		}

		static bool WriteCanonicalJson(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutText,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!Value.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_value_type_invalid"),
					TEXT("wire JSON value is invalid"));
				return false;
			}

			switch (Value->Type)
			{
			case EJson::Null:
				OutText += TEXT("null");
				return true;
			case EJson::String:
			case EJson::Number:
			case EJson::Boolean:
			{
				FString Scalar;
				if (!SerializeScalar(Value, Scalar))
				{
					OutDiagnostic = WireError(
						TEXT("wire_encode_failed"),
						TEXT("failed to encode scalar JSON value"));
					return false;
				}
				OutText += Scalar;
				return true;
			}
			case EJson::Array:
			{
				OutText += TEXT("[");
				const TArray<TSharedPtr<FJsonValue>>& Values =
					Value->AsArray();
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					if (Index > 0)
					{
						OutText += TEXT(",");
					}
					if (!WriteCanonicalJson(
						Values[Index],
						OutText,
						OutDiagnostic))
					{
						return false;
					}
				}
				OutText += TEXT("]");
				return true;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				if (!Object.IsValid())
				{
					OutDiagnostic = WireError(
						TEXT("wire_value_type_invalid"),
						TEXT("wire JSON object is invalid"));
					return false;
				}
				TArray<FString> Keys;
				Object->Values.GetKeys(Keys);
				Keys.Sort();
				OutText += TEXT("{");
				for (int32 Index = 0; Index < Keys.Num(); ++Index)
				{
					if (Index > 0)
					{
						OutText += TEXT(",");
					}
					FString EncodedKey;
					if (!SerializeScalar(
						MakeShared<FJsonValueString>(Keys[Index]),
						EncodedKey))
					{
						OutDiagnostic = WireError(
							TEXT("wire_encode_failed"),
							TEXT("failed to encode JSON object key"));
						return false;
					}
					OutText += EncodedKey;
					OutText += TEXT(":");
					if (!WriteCanonicalJson(
						Object->Values.FindRef(Keys[Index]),
						OutText,
						OutDiagnostic))
					{
						return false;
					}
				}
				OutText += TEXT("}");
				return true;
			}
			default:
				OutDiagnostic = WireError(
					TEXT("wire_value_type_invalid"),
					TEXT("unsupported wire JSON value"));
				return false;
			}
		}

		static bool JsonValueFromContractValue(
			const FUeAgentContractValue& Value,
			TSharedPtr<FJsonValue>& OutValue,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			const FString Wrapped =
				TEXT("{\"value\":") + Value.GetJsonValue() + TEXT("}");
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Wrapped);
			if (!FJsonSerializer::Deserialize(Reader, Object)
				|| !Object.IsValid()
				|| Object->Values.Num() != 1
				|| !Object->Values.Contains(TEXT("value")))
			{
				OutDiagnostic = WireError(
					TEXT("wire_value_invalid"),
					TEXT("contract value is not valid JSON"));
				return false;
			}
			OutValue = Object->Values.FindRef(TEXT("value"));
			return OutValue.IsValid();
		}

		static bool ContractValueFromJson(
			const TSharedPtr<FJsonValue>& Value,
			TOptional<FUeAgentContractValue>& OutValue,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			FString Canonical;
			if (!WriteCanonicalJson(Value, Canonical, OutDiagnostic))
			{
				return false;
			}
			OutValue.Emplace(MoveTemp(Canonical));
			return true;
		}

		static TSharedPtr<FJsonObject> EncodeIdentity(
			const FUeAgentDescriptorIdentity& Identity)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("descriptor_id"),
				Identity.GetDescriptorId());
			Object->SetNumberField(
				TEXT("version"),
				Identity.GetVersion());
			Object->SetStringField(
				TEXT("descriptor_hash"),
				Identity.GetDescriptorHash());
			return Object;
		}

		static bool DecodeIdentity(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentDescriptorIdentity>& OutIdentity,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!HasExactFields(
				Object,
				{
					TEXT("descriptor_id"),
					TEXT("version"),
					TEXT("descriptor_hash"),
				},
				TEXT("descriptor_identity"),
				OutDiagnostic))
			{
				return false;
			}
			FString DescriptorId;
			FString DescriptorHash;
			double VersionNumber = 0.0;
			if (!TryGetRequiredString(
					Object,
					TEXT("descriptor_id"),
					DescriptorId,
					OutDiagnostic)
				|| !TryGetRequiredString(
					Object,
					TEXT("descriptor_hash"),
					DescriptorHash,
					OutDiagnostic)
				|| !Object->TryGetNumberField(
					TEXT("version"),
					VersionNumber)
				|| VersionNumber < 1.0
				|| FMath::FloorToDouble(VersionNumber)
					!= VersionNumber)
			{
				if (OutDiagnostic.GetCode().IsEmpty())
				{
					OutDiagnostic = WireError(
						TEXT("wire_integer_invalid"),
						TEXT("descriptor version is invalid"));
				}
				return false;
			}
			FUeAgentDescriptorIdentity Identity(
				MoveTemp(DescriptorId),
				static_cast<int32>(VersionNumber),
				MoveTemp(DescriptorHash));
			if (!Identity.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_descriptor_identity_invalid"),
					TEXT("descriptor identity is invalid"));
				return false;
			}
			OutIdentity.Emplace(MoveTemp(Identity));
			return true;
		}

		static TSharedPtr<FJsonObject> EncodeCaller(
			const FUeAgentCallerContext& Caller)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("caller_id"), Caller.GetCallerId());
			TArray<TSharedPtr<FJsonValue>> Scopes;
			for (const FString& Scope : Caller.GetAuthorityScopes())
			{
				Scopes.Add(MakeShared<FJsonValueString>(Scope));
			}
			Object->SetArrayField(TEXT("authority_scopes"), Scopes);
			return Object;
		}

		static bool DecodeCaller(
			const TSharedPtr<FJsonObject>& Object,
			const FString& CorrelationId,
			TOptional<FUeAgentCallerContext>& OutCaller,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!HasExactFields(
				Object,
				{TEXT("caller_id"), TEXT("authority_scopes")},
				TEXT("caller_context"),
				OutDiagnostic))
			{
				return false;
			}
			FString CallerId;
			if (!TryGetRequiredString(
				Object,
				TEXT("caller_id"),
				CallerId,
				OutDiagnostic))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* RawScopes = nullptr;
			if (!Object->TryGetArrayField(
				TEXT("authority_scopes"),
				RawScopes))
			{
				OutDiagnostic = WireError(
					TEXT("wire_authority_scopes_invalid"),
					TEXT("authority_scopes must be an array"));
				return false;
			}
			TArray<FString> Scopes;
			for (const TSharedPtr<FJsonValue>& Value : *RawScopes)
			{
				FString Scope;
				if (!Value.IsValid()
					|| !Value->TryGetString(Scope)
					|| RequiredText(Scope).IsEmpty())
				{
					OutDiagnostic = WireError(
						TEXT("wire_authority_scopes_invalid"),
						TEXT("authority scope must be non-empty text"));
					return false;
				}
				Scopes.Add(RequiredText(MoveTemp(Scope)));
			}
			Scopes.Sort();
			for (int32 Index = 1; Index < Scopes.Num(); ++Index)
			{
				if (Scopes[Index] == Scopes[Index - 1])
				{
					OutDiagnostic = WireError(
						TEXT("wire_authority_scope_duplicate"),
						TEXT("authority scopes must be unique"));
					return false;
				}
			}
			FUeAgentCallerContext Caller(
				MoveTemp(CallerId),
				MoveTemp(Scopes),
				CorrelationId);
			if (!Caller.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_caller_context_invalid"),
					TEXT("caller context is invalid"));
				return false;
			}
			OutCaller.Emplace(MoveTemp(Caller));
			return true;
		}

		static bool ValidateRoot(
			const TSharedPtr<FJsonObject>& Root,
			const TArray<FString>& ExpectedFields,
			EUeAgentWireMessageKind& OutKind,
			FString& OutCorrelationId,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			if (!HasExactFields(
				Root,
				ExpectedFields,
				TEXT("envelope"),
				OutDiagnostic))
			{
				return false;
			}
			FString Schema;
			FString MessageKind;
			double VersionNumber = 0.0;
			if (!TryGetRequiredString(
					Root,
					TEXT("schema"),
					Schema,
					OutDiagnostic)
				|| !TryGetRequiredString(
					Root,
					TEXT("message_kind"),
					MessageKind,
					OutDiagnostic)
				|| !TryGetRequiredString(
					Root,
					TEXT("correlation_id"),
					OutCorrelationId,
					OutDiagnostic))
			{
				return false;
			}
			if (Schema != WireSchema)
			{
				OutDiagnostic = WireError(
					TEXT("wire_schema_unsupported"),
					TEXT("unsupported wire schema"));
				return false;
			}
			if (!Root->TryGetNumberField(
					TEXT("version"),
					VersionNumber)
				|| FMath::FloorToDouble(VersionNumber)
					!= VersionNumber
				|| static_cast<int32>(VersionNumber)
					!= WireVersion)
			{
				OutDiagnostic = WireError(
					TEXT("wire_version_unsupported"),
					TEXT("unsupported wire version"));
				return false;
			}
			if (!TryParseMessageKind(MessageKind, OutKind))
			{
				OutDiagnostic = WireError(
					TEXT("wire_message_kind_unsupported"),
					TEXT("unsupported wire message kind"));
				return false;
			}
			return true;
		}

		static bool CanonicalPayloadBytes(
			const TSharedPtr<FJsonObject>& Root,
			TArray<uint8>& OutPayload,
			FUeAgentProtocolDiagnostic& OutDiagnostic)
		{
			FString Canonical;
			if (!WriteCanonicalJson(
				MakeShared<FJsonValueObject>(Root),
				Canonical,
				OutDiagnostic))
			{
				return false;
			}
			const FTCHARToUTF8 Converted(*Canonical);
			OutPayload.Reset(Converted.Length());
			OutPayload.Append(
				reinterpret_cast<const uint8*>(Converted.Get()),
				Converted.Length());
			return true;
		}

		static TSharedPtr<FJsonObject> MakeRoot(
			const EUeAgentWireMessageKind Kind,
			const FString& CorrelationId)
		{
			const TSharedPtr<FJsonObject> Root =
				MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), WireSchema);
			Root->SetNumberField(TEXT("version"), WireVersion);
			Root->SetStringField(
				TEXT("message_kind"),
				MessageKindText(Kind));
			Root->SetStringField(
				TEXT("correlation_id"),
				CorrelationId);
			return Root;
		}

		static bool ValidateResultMatchesRequest(
			const FUeAgentUaiWireRequest& Request,
			const FUeAgentUaiWireResult& Result)
		{
			if (Request.IsQuery() && Result.HasQueryResult())
			{
				return Request.GetQueryRequest().GetRequestId()
						== Result.GetQueryResult().GetRequestId()
					&& Request.GetQueryRequest().GetDescriptorIdentity()
						== Result.GetQueryResult().GetDescriptorIdentity();
			}
			if (Request.IsOperation() && Result.HasOperationResult())
			{
				return Request.GetOperationRequest().GetRequestId()
						== Result.GetOperationResult().GetRequestId()
					&& Request.GetOperationRequest().GetDescriptorIdentity()
						== Result.GetOperationResult().GetDescriptorIdentity();
			}
			return Result.HasDiagnostic();
		}
	}

	const FString& GetUaiWireSchema()
	{
		return WireSchema;
	}

	int32 GetUaiWireVersion()
	{
		return WireVersion;
	}

	FUeAgentProtocolDiagnostic::FUeAgentProtocolDiagnostic() = default;

	FUeAgentProtocolDiagnostic::FUeAgentProtocolDiagnostic(
		const EUeAgentProtocolErrorLayer InLayer,
		FString InCode,
		FString InMessage,
		TOptional<EUeAgentTransportDeliveryState> InTransportState)
		: Layer(InLayer)
		, Code(RequiredText(MoveTemp(InCode)))
		, Message(MoveTemp(InMessage))
		, TransportState(MoveTemp(InTransportState))
	{
		if (Layer == EUeAgentProtocolErrorLayer::Wire)
		{
			TransportState.Reset();
		}
	}

	EUeAgentProtocolErrorLayer
	FUeAgentProtocolDiagnostic::GetLayer() const
	{
		return Layer;
	}

	const FString& FUeAgentProtocolDiagnostic::GetCode() const
	{
		return Code;
	}

	const FString& FUeAgentProtocolDiagnostic::GetMessage() const
	{
		return Message;
	}

	const TOptional<EUeAgentTransportDeliveryState>&
	FUeAgentProtocolDiagnostic::GetTransportState() const
	{
		return TransportState;
	}

	bool FUeAgentProtocolDiagnostic::IsValid() const
	{
		return !Code.IsEmpty()
			&& (Layer == EUeAgentProtocolErrorLayer::Transport
				|| !TransportState.IsSet());
	}

	FUeAgentUaiWireRequest::FUeAgentUaiWireRequest(
		const EUeAgentWireMessageKind InMessageKind,
		FString InCorrelationId,
		FUeAgentCallerContext InCallerContext)
		: MessageKind(InMessageKind)
		, CorrelationId(RequiredText(MoveTemp(InCorrelationId)))
		, CallerContext(MoveTemp(InCallerContext))
	{
	}

	FUeAgentUaiWireRequest FUeAgentUaiWireRequest::MakeQuery(
		FString CorrelationId,
		FUeAgentQueryRequest Request,
		FUeAgentCallerContext CallerContext)
	{
		FUeAgentUaiWireRequest Result(
			EUeAgentWireMessageKind::QueryRequest,
			MoveTemp(CorrelationId),
			MoveTemp(CallerContext));
		Result.QueryRequest.Emplace(MoveTemp(Request));
		return Result;
	}

	FUeAgentUaiWireRequest FUeAgentUaiWireRequest::MakeOperation(
		FString CorrelationId,
		FUeAgentOperationRequest Request,
		FUeAgentCallerContext CallerContext)
	{
		FUeAgentUaiWireRequest Result(
			EUeAgentWireMessageKind::OperationRequest,
			MoveTemp(CorrelationId),
			MoveTemp(CallerContext));
		Result.OperationRequest.Emplace(MoveTemp(Request));
		return Result;
	}

	EUeAgentWireMessageKind
	FUeAgentUaiWireRequest::GetMessageKind() const
	{
		return MessageKind;
	}

	const FString& FUeAgentUaiWireRequest::GetCorrelationId() const
	{
		return CorrelationId;
	}

	const FUeAgentCallerContext&
	FUeAgentUaiWireRequest::GetCallerContext() const
	{
		return CallerContext;
	}

	bool FUeAgentUaiWireRequest::IsQuery() const
	{
		return MessageKind == EUeAgentWireMessageKind::QueryRequest
			&& QueryRequest.IsSet()
			&& !OperationRequest.IsSet();
	}

	bool FUeAgentUaiWireRequest::IsOperation() const
	{
		return MessageKind
				== EUeAgentWireMessageKind::OperationRequest
			&& OperationRequest.IsSet()
			&& !QueryRequest.IsSet();
	}

	const FUeAgentQueryRequest&
	FUeAgentUaiWireRequest::GetQueryRequest() const
	{
		check(QueryRequest.IsSet());
		return QueryRequest.GetValue();
	}

	const FUeAgentOperationRequest&
	FUeAgentUaiWireRequest::GetOperationRequest() const
	{
		check(OperationRequest.IsSet());
		return OperationRequest.GetValue();
	}

	bool FUeAgentUaiWireRequest::IsValid() const
	{
		if (CorrelationId.IsEmpty()
			|| !CallerContext.IsValid()
			|| CallerContext.GetCorrelationId() != CorrelationId)
		{
			return false;
		}
		return (IsQuery() && GetQueryRequest().IsValid())
			|| (IsOperation() && GetOperationRequest().IsValid());
	}

	FUeAgentUaiWireResult::FUeAgentUaiWireResult(
		const EUeAgentWireMessageKind InMessageKind,
		FString InCorrelationId)
		: MessageKind(InMessageKind)
		, CorrelationId(RequiredText(MoveTemp(InCorrelationId)))
	{
	}

	FUeAgentUaiWireResult FUeAgentUaiWireResult::MakeQuery(
		FString CorrelationId,
		FUeAgentQueryResult Result)
	{
		FUeAgentUaiWireResult WireResult(
			EUeAgentWireMessageKind::QueryResult,
			MoveTemp(CorrelationId));
		WireResult.QueryResult.Emplace(MoveTemp(Result));
		return WireResult;
	}

	FUeAgentUaiWireResult FUeAgentUaiWireResult::MakeOperation(
		FString CorrelationId,
		FUeAgentOperationResult Result)
	{
		FUeAgentUaiWireResult WireResult(
			EUeAgentWireMessageKind::OperationResult,
			MoveTemp(CorrelationId));
		WireResult.OperationResult.Emplace(MoveTemp(Result));
		return WireResult;
	}

	FUeAgentUaiWireResult FUeAgentUaiWireResult::MakeError(
		FString CorrelationId,
		FUeAgentProtocolDiagnostic DiagnosticValue)
	{
		FUeAgentUaiWireResult WireResult(
			EUeAgentWireMessageKind::ProtocolError,
			MoveTemp(CorrelationId));
		WireResult.Diagnostic.Emplace(MoveTemp(DiagnosticValue));
		return WireResult;
	}

	EUeAgentWireMessageKind
	FUeAgentUaiWireResult::GetMessageKind() const
	{
		return MessageKind;
	}

	const FString& FUeAgentUaiWireResult::GetCorrelationId() const
	{
		return CorrelationId;
	}

	bool FUeAgentUaiWireResult::HasQueryResult() const
	{
		return QueryResult.IsSet();
	}

	bool FUeAgentUaiWireResult::HasOperationResult() const
	{
		return OperationResult.IsSet();
	}

	bool FUeAgentUaiWireResult::HasDiagnostic() const
	{
		return Diagnostic.IsSet();
	}

	const FUeAgentQueryResult&
	FUeAgentUaiWireResult::GetQueryResult() const
	{
		check(QueryResult.IsSet());
		return QueryResult.GetValue();
	}

	const FUeAgentOperationResult&
	FUeAgentUaiWireResult::GetOperationResult() const
	{
		check(OperationResult.IsSet());
		return OperationResult.GetValue();
	}

	const FUeAgentProtocolDiagnostic&
	FUeAgentUaiWireResult::GetDiagnostic() const
	{
		check(Diagnostic.IsSet());
		return Diagnostic.GetValue();
	}

	bool FUeAgentUaiWireResult::IsValid() const
	{
		const int32 PayloadCount =
			(QueryResult.IsSet() ? 1 : 0)
			+ (OperationResult.IsSet() ? 1 : 0)
			+ (Diagnostic.IsSet() ? 1 : 0);
		if (CorrelationId.IsEmpty() || PayloadCount != 1)
		{
			return false;
		}
		if (MessageKind == EUeAgentWireMessageKind::QueryResult)
		{
			return QueryResult.IsSet()
				&& QueryResult->IsValid();
		}
		if (MessageKind == EUeAgentWireMessageKind::OperationResult)
		{
			return OperationResult.IsSet()
				&& OperationResult->IsValid();
		}
		return MessageKind == EUeAgentWireMessageKind::ProtocolError
			&& Diagnostic.IsSet()
			&& Diagnostic->IsValid();
	}

	bool FUeAgentUaiProtocolCodec::EncodeRequest(
		const FUeAgentUaiWireRequest& Request,
		TArray<uint8>& OutPayload,
		FUeAgentProtocolDiagnostic& OutDiagnostic) const
	{
		OutPayload.Reset();
		if (!Request.IsValid())
		{
			OutDiagnostic = WireError(
				TEXT("wire_request_invalid"),
				TEXT("wire request is invalid"));
			return false;
		}
		const TSharedPtr<FJsonObject> Root = MakeRoot(
			Request.GetMessageKind(),
			Request.GetCorrelationId());
		Root->SetObjectField(
			TEXT("caller_context"),
			EncodeCaller(Request.GetCallerContext()));

		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		TSharedPtr<FJsonValue> ContractPayload;
		if (Request.IsQuery())
		{
			const FUeAgentQueryRequest& Query =
				Request.GetQueryRequest();
			Payload->SetObjectField(
				TEXT("descriptor_identity"),
				EncodeIdentity(Query.GetDescriptorIdentity()));
			Payload->SetStringField(
				TEXT("request_id"),
				Query.GetRequestId());
			if (!JsonValueFromContractValue(
				Query.GetPayload(),
				ContractPayload,
				OutDiagnostic))
			{
				return false;
			}
		}
		else
		{
			const FUeAgentOperationRequest& Operation =
				Request.GetOperationRequest();
			Payload->SetObjectField(
				TEXT("descriptor_identity"),
				EncodeIdentity(Operation.GetDescriptorIdentity()));
			Payload->SetStringField(
				TEXT("request_id"),
				Operation.GetRequestId());
			Payload->SetStringField(
				TEXT("idempotency_key"),
				Operation.GetIdempotencyKey());
			if (!JsonValueFromContractValue(
				Operation.GetPayload(),
				ContractPayload,
				OutDiagnostic))
			{
				return false;
			}
		}
		Payload->SetField(TEXT("payload"), ContractPayload);
		Root->SetObjectField(TEXT("payload"), Payload);
		return CanonicalPayloadBytes(
			Root,
			OutPayload,
			OutDiagnostic);
	}

	bool FUeAgentUaiProtocolCodec::DecodeRequest(
		const TArray<uint8>& Payload,
		TOptional<FUeAgentUaiWireRequest>& OutRequest,
		FUeAgentProtocolDiagnostic& OutDiagnostic) const
	{
		OutRequest.Reset();
		TSharedPtr<FJsonObject> Root;
		if (!ParseUtf8Object(Payload, Root, OutDiagnostic))
		{
			return false;
		}
		EUeAgentWireMessageKind Kind;
		FString CorrelationId;
		if (!ValidateRoot(
			Root,
			{
				TEXT("schema"),
				TEXT("version"),
				TEXT("message_kind"),
				TEXT("correlation_id"),
				TEXT("caller_context"),
				TEXT("payload"),
			},
			Kind,
			CorrelationId,
			OutDiagnostic))
		{
			return false;
		}
		if (Kind != EUeAgentWireMessageKind::QueryRequest
			&& Kind != EUeAgentWireMessageKind::OperationRequest)
		{
			OutDiagnostic = WireError(
				TEXT("wire_request_kind_invalid"),
				TEXT("wire message is not a request"));
			return false;
		}
		const TSharedPtr<FJsonObject>* CallerObject = nullptr;
		const TSharedPtr<FJsonObject>* RequestObject = nullptr;
		if (!Root->TryGetObjectField(
				TEXT("caller_context"),
				CallerObject)
			|| !Root->TryGetObjectField(
				TEXT("payload"),
				RequestObject))
		{
			OutDiagnostic = WireError(
				TEXT("wire_object_type_invalid"),
				TEXT("request envelope objects are invalid"));
			return false;
		}
		TOptional<FUeAgentCallerContext> Caller;
		if (!DecodeCaller(
			*CallerObject,
			CorrelationId,
			Caller,
			OutDiagnostic))
		{
			return false;
		}
		const bool bQuery =
			Kind == EUeAgentWireMessageKind::QueryRequest;
		if (!HasExactFields(
			*RequestObject,
			bQuery
				? TArray<FString>{
					TEXT("descriptor_identity"),
					TEXT("request_id"),
					TEXT("payload"),
				}
				: TArray<FString>{
					TEXT("descriptor_identity"),
					TEXT("request_id"),
					TEXT("idempotency_key"),
					TEXT("payload"),
				},
			TEXT("payload"),
			OutDiagnostic))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
		if (!(*RequestObject)->TryGetObjectField(
			TEXT("descriptor_identity"),
			IdentityObject))
		{
			OutDiagnostic = WireError(
				TEXT("wire_object_type_invalid"),
				TEXT("descriptor_identity must be an object"));
			return false;
		}
		TOptional<FUeAgentDescriptorIdentity> Identity;
		if (!DecodeIdentity(
			*IdentityObject,
			Identity,
			OutDiagnostic))
		{
			return false;
		}
		FString RequestId;
		if (!TryGetRequiredString(
			*RequestObject,
			TEXT("request_id"),
			RequestId,
			OutDiagnostic))
		{
			return false;
		}
		const TSharedPtr<FJsonValue> RawPayload =
			(*RequestObject)->Values.FindRef(TEXT("payload"));
		TOptional<FUeAgentContractValue> ContractPayload;
		if (!ContractValueFromJson(
			RawPayload,
			ContractPayload,
			OutDiagnostic))
		{
			return false;
		}
		if (bQuery)
		{
			OutRequest.Emplace(FUeAgentUaiWireRequest::MakeQuery(
				CorrelationId,
				FUeAgentQueryRequest(
					Identity.GetValue(),
					MoveTemp(RequestId),
					ContractPayload.GetValue()),
				Caller.GetValue()));
		}
		else
		{
			FString IdempotencyKey;
			if (!TryGetRequiredString(
				*RequestObject,
				TEXT("idempotency_key"),
				IdempotencyKey,
				OutDiagnostic))
			{
				return false;
			}
			OutRequest.Emplace(
				FUeAgentUaiWireRequest::MakeOperation(
					CorrelationId,
					FUeAgentOperationRequest(
						Identity.GetValue(),
						MoveTemp(RequestId),
						MoveTemp(IdempotencyKey),
						ContractPayload.GetValue()),
					Caller.GetValue()));
		}
		return OutRequest->IsValid();
	}

	bool FUeAgentUaiProtocolCodec::EncodeResult(
		const FUeAgentUaiWireResult& Result,
		TArray<uint8>& OutPayload,
		FUeAgentProtocolDiagnostic& OutDiagnostic) const
	{
		OutPayload.Reset();
		if (!Result.IsValid())
		{
			OutDiagnostic = WireError(
				TEXT("wire_result_invalid"),
				TEXT("wire result is invalid"));
			return false;
		}
		const TSharedPtr<FJsonObject> Root = MakeRoot(
			Result.GetMessageKind(),
			Result.GetCorrelationId());
		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		if (Result.HasDiagnostic())
		{
			const FUeAgentProtocolDiagnostic& Diagnostic =
				Result.GetDiagnostic();
			const TSharedPtr<FJsonObject> DiagnosticObject =
				MakeShared<FJsonObject>();
			DiagnosticObject->SetStringField(
				TEXT("layer"),
				Diagnostic.GetLayer()
						== EUeAgentProtocolErrorLayer::Wire
					? TEXT("wire")
					: TEXT("transport"));
			DiagnosticObject->SetStringField(
				TEXT("code"),
				Diagnostic.GetCode());
			DiagnosticObject->SetStringField(
				TEXT("message"),
				Diagnostic.GetMessage());
			if (Diagnostic.GetTransportState().IsSet())
			{
				DiagnosticObject->SetStringField(
					TEXT("transport_state"),
					DeliveryStateText(
						Diagnostic.GetTransportState().GetValue()));
			}
			else
			{
				DiagnosticObject->SetField(
					TEXT("transport_state"),
					MakeShared<FJsonValueNull>());
			}
			Payload->SetObjectField(
				TEXT("diagnostic"),
				DiagnosticObject);
		}
		else
		{
			const FUeAgentDescriptorIdentity* Identity = nullptr;
			const FUeAgentContractValue* Value = nullptr;
			FString RequestId;
			if (Result.HasQueryResult())
			{
				Identity =
					&Result.GetQueryResult().GetDescriptorIdentity();
				Value = &Result.GetQueryResult().GetValue();
				RequestId = Result.GetQueryResult().GetRequestId();
			}
			else
			{
				Identity =
					&Result.GetOperationResult().GetDescriptorIdentity();
				Value = &Result.GetOperationResult().GetValue();
				RequestId =
					Result.GetOperationResult().GetRequestId();
			}
			Payload->SetObjectField(
				TEXT("descriptor_identity"),
				EncodeIdentity(*Identity));
			Payload->SetStringField(TEXT("request_id"), RequestId);
			TSharedPtr<FJsonValue> ContractValue;
			if (!JsonValueFromContractValue(
				*Value,
				ContractValue,
				OutDiagnostic))
			{
				return false;
			}
			Payload->SetField(TEXT("value"), ContractValue);
		}
		Root->SetObjectField(TEXT("payload"), Payload);
		return CanonicalPayloadBytes(
			Root,
			OutPayload,
			OutDiagnostic);
	}

	bool FUeAgentUaiProtocolCodec::DecodeResult(
		const TArray<uint8>& Payload,
		TOptional<FUeAgentUaiWireResult>& OutResult,
		FUeAgentProtocolDiagnostic& OutDiagnostic) const
	{
		OutResult.Reset();
		TSharedPtr<FJsonObject> Root;
		if (!ParseUtf8Object(Payload, Root, OutDiagnostic))
		{
			return false;
		}
		EUeAgentWireMessageKind Kind;
		FString CorrelationId;
		if (!ValidateRoot(
			Root,
			{
				TEXT("schema"),
				TEXT("version"),
				TEXT("message_kind"),
				TEXT("correlation_id"),
				TEXT("payload"),
			},
			Kind,
			CorrelationId,
			OutDiagnostic))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* ResultObject = nullptr;
		if (!Root->TryGetObjectField(
			TEXT("payload"),
			ResultObject))
		{
			OutDiagnostic = WireError(
				TEXT("wire_object_type_invalid"),
				TEXT("result payload must be an object"));
			return false;
		}
		if (Kind == EUeAgentWireMessageKind::ProtocolError)
		{
			if (!HasExactFields(
				*ResultObject,
				{TEXT("diagnostic")},
				TEXT("payload"),
				OutDiagnostic))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* DiagnosticObject = nullptr;
			if (!(*ResultObject)->TryGetObjectField(
				TEXT("diagnostic"),
				DiagnosticObject)
				|| !HasExactFields(
					*DiagnosticObject,
					{
						TEXT("layer"),
						TEXT("code"),
						TEXT("message"),
						TEXT("transport_state"),
					},
					TEXT("diagnostic"),
					OutDiagnostic))
			{
				return false;
			}
			FString LayerText;
			FString Code;
			FString Message;
			if (!TryGetRequiredString(
					*DiagnosticObject,
					TEXT("layer"),
					LayerText,
					OutDiagnostic)
				|| !TryGetRequiredString(
					*DiagnosticObject,
					TEXT("code"),
					Code,
					OutDiagnostic)
				|| !(*DiagnosticObject)->TryGetStringField(
					TEXT("message"),
					Message))
			{
				return false;
			}
			const EUeAgentProtocolErrorLayer Layer =
				LayerText == TEXT("transport")
				? EUeAgentProtocolErrorLayer::Transport
				: EUeAgentProtocolErrorLayer::Wire;
			TOptional<EUeAgentTransportDeliveryState> State;
			const TSharedPtr<FJsonValue> StateValue =
				(*DiagnosticObject)
					->Values.FindRef(TEXT("transport_state"));
			if (StateValue.IsValid()
				&& StateValue->Type != EJson::Null)
			{
				FString StateText;
				EUeAgentTransportDeliveryState ParsedState;
				if (!StateValue->TryGetString(StateText)
					|| !TryParseDeliveryState(
						StateText,
						ParsedState))
				{
					OutDiagnostic = WireError(
						TEXT("wire_transport_state_invalid"),
						TEXT("transport state is invalid"));
					return false;
				}
				State = ParsedState;
			}
			const FUeAgentProtocolDiagnostic Decoded(
				Layer,
				MoveTemp(Code),
				MoveTemp(Message),
				State);
			if (!Decoded.IsValid())
			{
				OutDiagnostic = WireError(
					TEXT("wire_diagnostic_invalid"),
					TEXT("protocol diagnostic is invalid"));
				return false;
			}
			OutResult.Emplace(FUeAgentUaiWireResult::MakeError(
				CorrelationId,
				Decoded));
			return true;
		}
		if (Kind != EUeAgentWireMessageKind::QueryResult
			&& Kind != EUeAgentWireMessageKind::OperationResult)
		{
			OutDiagnostic = WireError(
				TEXT("wire_result_kind_invalid"),
				TEXT("wire message is not a result"));
			return false;
		}
		if (!HasExactFields(
			*ResultObject,
			{
				TEXT("descriptor_identity"),
				TEXT("request_id"),
				TEXT("value"),
			},
			TEXT("payload"),
			OutDiagnostic))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* IdentityObject = nullptr;
		if (!(*ResultObject)->TryGetObjectField(
			TEXT("descriptor_identity"),
			IdentityObject))
		{
			OutDiagnostic = WireError(
				TEXT("wire_object_type_invalid"),
				TEXT("descriptor_identity must be an object"));
			return false;
		}
		TOptional<FUeAgentDescriptorIdentity> Identity;
		if (!DecodeIdentity(
			*IdentityObject,
			Identity,
			OutDiagnostic))
		{
			return false;
		}
		FString RequestId;
		if (!TryGetRequiredString(
			*ResultObject,
			TEXT("request_id"),
			RequestId,
			OutDiagnostic))
		{
			return false;
		}
		TOptional<FUeAgentContractValue> Value;
		if (!ContractValueFromJson(
			(*ResultObject)->Values.FindRef(TEXT("value")),
			Value,
			OutDiagnostic))
		{
			return false;
		}
		if (Kind == EUeAgentWireMessageKind::QueryResult)
		{
			OutResult.Emplace(FUeAgentUaiWireResult::MakeQuery(
				CorrelationId,
				FUeAgentQueryResult(
					Identity.GetValue(),
					MoveTemp(RequestId),
					Value.GetValue())));
		}
		else
		{
			OutResult.Emplace(
				FUeAgentUaiWireResult::MakeOperation(
					CorrelationId,
					FUeAgentOperationResult(
						Identity.GetValue(),
						MoveTemp(RequestId),
						Value.GetValue())));
		}
		return OutResult->IsValid();
	}

	FUeAgentUaiProtocolClient::FUeAgentUaiProtocolClient(
		IUeAgentTransportClient& InTransportClient,
		FUeAgentUaiProtocolCodec InCodec,
		FUeAgentTransportPeer InPeer,
		FString InChannel,
		FUeAgentDeliveryRequirements InDeliveryRequirements)
		: TransportClient(InTransportClient)
		, Codec(MoveTemp(InCodec))
		, Peer(MoveTemp(InPeer))
		, Channel(RequiredText(MoveTemp(InChannel)))
		, DeliveryRequirements(MoveTemp(InDeliveryRequirements))
	{
	}

	FUeAgentUaiWireResult FUeAgentUaiProtocolClient::Invoke(
		const FUeAgentUaiWireRequest& Request)
	{
		FUeAgentProtocolDiagnostic Diagnostic;
		TArray<uint8> Payload;
		if (!Codec.EncodeRequest(Request, Payload, Diagnostic))
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				Diagnostic);
		}
		const FUeAgentTransportFrame Frame(
			Channel,
			MoveTemp(Payload),
			Request.GetCorrelationId(),
			Peer);
		const FUeAgentTransportDelivery Delivery =
			TransportClient.Send(Frame, DeliveryRequirements);
		if (Delivery.CorrelationId != Request.GetCorrelationId())
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				TransportError(
					TEXT("transport_correlation_mismatch"),
					TEXT("transport delivery correlation mismatch"),
					Delivery.State));
		}
		if (Delivery.State
			!= EUeAgentTransportDeliveryState::Delivered)
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				TransportError(
					TransportFailureCode(Delivery.State),
					Delivery.Diagnostic.IsSet()
						? Delivery.Diagnostic->Message
						: TEXT("transport delivery failed"),
					Delivery.State));
		}
		if (!Delivery.Reply.IsSet())
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				TransportError(
					TEXT("transport_reply_missing"),
					TEXT("delivered request has no reply"),
					Delivery.State));
		}
		const FUeAgentTransportFrame& ReplyFrame =
			Delivery.Reply->Frame;
		if (ReplyFrame.GetCorrelationId()
			!= Request.GetCorrelationId())
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				TransportError(
					TEXT("transport_reply_correlation_mismatch"),
					TEXT("transport reply correlation mismatch"),
					Delivery.State));
		}
		TOptional<FUeAgentUaiWireResult> Result;
		if (!Codec.DecodeResult(
			ReplyFrame.GetPayload(),
			Result,
			Diagnostic))
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				Diagnostic);
		}
		if (Result->GetCorrelationId()
			!= Request.GetCorrelationId())
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				WireError(
					TEXT("wire_correlation_mismatch"),
					TEXT("wire result correlation mismatch")));
		}
		if (!ValidateResultMatchesRequest(Request, Result.GetValue()))
		{
			return FUeAgentUaiWireResult::MakeError(
				Request.GetCorrelationId(),
				WireError(
					TEXT("wire_application_result_identity_mismatch"),
					TEXT("wire result does not match request")));
		}
		return Result.GetValue();
	}

	FUeAgentUaiProtocolIngress::FUeAgentUaiProtocolIngress(
		FUeAgentUaiProtocolCodec InCodec,
		IUeAgentQueryApplicationPort& InQueryPort,
		IUeAgentOperationApplicationPort& InOperationPort,
		FUeAgentCancellationFactory InCancellationFactory)
		: Codec(MoveTemp(InCodec))
		, QueryPort(InQueryPort)
		, OperationPort(InOperationPort)
		, CancellationFactory(MoveTemp(InCancellationFactory))
	{
	}

	FUeAgentTransportReply FUeAgentUaiProtocolIngress::Handle(
		const FUeAgentTransportIngress& Ingress)
	{
		const FString& CorrelationId =
			Ingress.Frame.GetCorrelationId();
		FUeAgentProtocolDiagnostic Diagnostic;
		TOptional<FUeAgentUaiWireRequest> Request;
		if (!Codec.DecodeRequest(
			Ingress.Frame.GetPayload(),
			Request,
			Diagnostic))
		{
			return MakeReply(
				Ingress,
				FUeAgentUaiWireResult::MakeError(
					CorrelationId,
					Diagnostic));
		}
		if (Request->GetCorrelationId() != CorrelationId)
		{
			return MakeReply(
				Ingress,
				FUeAgentUaiWireResult::MakeError(
					CorrelationId,
					WireError(
						TEXT("wire_correlation_mismatch"),
						TEXT("wire request correlation mismatch"))));
		}
		if (!CancellationFactory)
		{
			return MakeReply(
				Ingress,
				FUeAgentUaiWireResult::MakeError(
					CorrelationId,
					WireError(
						TEXT("wire_cancellation_factory_missing"),
						TEXT("cancellation factory is missing"))));
		}
		const TSharedRef<IUeAgentCancellationSignal> Cancellation =
			CancellationFactory(CorrelationId);
		FUeAgentUaiWireResult Result =
			Request->IsQuery()
				? FUeAgentUaiWireResult::MakeQuery(
					CorrelationId,
					QueryPort.Submit(
						Request->GetQueryRequest(),
						Request->GetCallerContext(),
						Cancellation))
				: FUeAgentUaiWireResult::MakeOperation(
					CorrelationId,
					OperationPort.Submit(
						Request->GetOperationRequest(),
						Request->GetCallerContext(),
						Cancellation));
		if (!ValidateResultMatchesRequest(Request.GetValue(), Result))
		{
			Result = FUeAgentUaiWireResult::MakeError(
				CorrelationId,
				WireError(
					TEXT("wire_application_result_identity_mismatch"),
					TEXT("application result does not match request")));
		}
		return MakeReply(Ingress, Result);
	}

	FUeAgentTransportReply FUeAgentUaiProtocolIngress::MakeReply(
		const FUeAgentTransportIngress& Ingress,
		const FUeAgentUaiWireResult& Result) const
	{
		FUeAgentProtocolDiagnostic Diagnostic;
		TArray<uint8> Payload;
		if (!Codec.EncodeResult(Result, Payload, Diagnostic))
		{
			const FUeAgentUaiWireResult Fallback =
				FUeAgentUaiWireResult::MakeError(
					Ingress.Frame.GetCorrelationId(),
					Diagnostic);
			Codec.EncodeResult(Fallback, Payload, Diagnostic);
		}
		return FUeAgentTransportReply(FUeAgentTransportFrame(
			Ingress.Frame.GetChannel(),
			MoveTemp(Payload),
			Ingress.Frame.GetCorrelationId(),
			Ingress.Peer));
	}
}
