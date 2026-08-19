#include "CommandEdge/UeAgentCommandProtocol.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandEdge
{
	using namespace CommandContract;
	using namespace Contract;
	using namespace NetworkTransport;

	namespace
	{
		const FString WireSchema(TEXT("uai.command.v1"));
		const FString WireChannel(TEXT("uai.command"));
		constexpr int32 WireVersion = 1;

		FUeAgentCommandProtocolDiagnostic Diagnostic(
			const FString& Code,
			const FString& Message)
		{
			return {Code, Message};
		}

		bool Fail(
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic,
			const FString& Code,
			const FString& Message)
		{
			OutDiagnostic = Diagnostic(Code, Message);
			return false;
		}

		bool HasExactFields(
			const TSharedPtr<FJsonObject>& Object,
			const TSet<FString>& Required)
		{
			if (!Object.IsValid()
				|| Object->Values.Num() != Required.Num())
			{
				return false;
			}
			for (const FString& Name : Required)
			{
				if (!Object->HasField(Name))
				{
					return false;
				}
			}
			return true;
		}

		bool TryRequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Name,
			FString& OutValue)
		{
			return Object.IsValid()
				&& Object->TryGetStringField(Name, OutValue)
				&& !OutValue.IsEmpty();
		}

		bool TryWireVersion(
			const TSharedPtr<FJsonObject>& Object,
			int32& OutVersion)
		{
			const TSharedPtr<FJsonValue> Value =
				Object.IsValid()
					? Object->TryGetField(TEXT("version"))
					: nullptr;
			int64 Parsed = 0;
			if (!Value.IsValid()
				|| !Value->TryGetNumber(Parsed)
				|| Parsed < MIN_int32
				|| Parsed > MAX_int32)
			{
				return false;
			}
			OutVersion = static_cast<int32>(Parsed);
			return true;
		}

		bool TryCanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutText,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
		{
			FString Error;
			if (!Json::TryCanonicalize(Object, OutText, Error))
			{
				return Fail(
					OutDiagnostic,
					TEXT("command_protocol_json_invalid"),
					Error);
			}
			return true;
		}

		bool TryParseObjectText(
			const FString& Text,
			TSharedPtr<FJsonObject>& OutObject,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
		{
			FString Error;
			if (!Json::TryParseObject(Text, OutObject, Error))
			{
				return Fail(
					OutDiagnostic,
					TEXT("command_protocol_json_invalid"),
					Error);
			}
			return true;
		}

		bool TryValidateParameterValue(
			const FUeAgentCommandParameter& Parameter,
			const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				return false;
			}
			if (Value->Type == EJson::Null)
			{
				return Parameter.IsNullable();
			}

			switch (Parameter.GetWireType())
			{
			case EUeAgentCommandWireType::Boolean:
				return Value->Type == EJson::Boolean;
			case EUeAgentCommandWireType::Integer:
			{
				int64 Parsed = 0;
				return Value->Type == EJson::Number
					&& Value->TryGetNumber(Parsed);
			}
			case EUeAgentCommandWireType::Number:
				return Value->Type == EJson::Number;
			case EUeAgentCommandWireType::String:
				return Value->Type == EJson::String;
			case EUeAgentCommandWireType::Object:
				return Value->Type == EJson::Object;
			case EUeAgentCommandWireType::Array:
				return Value->Type == EJson::Array;
			case EUeAgentCommandWireType::Any:
				return true;
			default:
				return false;
			}
		}

		bool TryValidateParams(
			const FUeAgentCommandContract& Contract,
			const TSharedPtr<FJsonObject>& Params,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
		{
			if (!Params.IsValid())
			{
				return Fail(
					OutDiagnostic,
					TEXT("command_params_invalid"),
					TEXT("params must be an object"));
			}

			TMap<FString, const FUeAgentCommandParameter*> ByName;
			for (const FUeAgentCommandParameter& Parameter
				: Contract.GetParameters())
			{
				ByName.Add(Parameter.GetName(), &Parameter);
				const TSharedPtr<FJsonValue> Value =
					Params->TryGetField(Parameter.GetName());
				if (!Value.IsValid())
				{
					if (Parameter.IsRequired())
					{
						return Fail(
							OutDiagnostic,
							TEXT("command_param_required"),
							Parameter.GetName());
					}
					continue;
				}
				if (!TryValidateParameterValue(Parameter, Value))
				{
					return Fail(
						OutDiagnostic,
						TEXT("command_param_type_invalid"),
						Parameter.GetName());
				}
			}

			for (const TPair<
				FString,
				TSharedPtr<FJsonValue>>& Field : Params->Values)
			{
				if (!ByName.Contains(Field.Key))
				{
					return Fail(
						OutDiagnostic,
						TEXT("command_param_unknown"),
						Field.Key);
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> ErrorObject(
			const FString& Code,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("message"), Message);
			return Error;
		}
	}

	FUeAgentCommandRequest::FUeAgentCommandRequest(
		FString InCorrelationId,
		FString InRequestId,
		FString InCommandId,
		FString InCommandBindingHash,
		TSharedPtr<FJsonObject> InParams,
		FString InParamsJson)
		: CorrelationId(MoveTemp(InCorrelationId))
		, RequestId(MoveTemp(InRequestId))
		, CommandId(MoveTemp(InCommandId))
		, CommandBindingHash(MoveTemp(InCommandBindingHash))
		, Params(MoveTemp(InParams))
		, ParamsJson(MoveTemp(InParamsJson))
	{
		CorrelationId.TrimStartAndEndInline();
		RequestId.TrimStartAndEndInline();
		CommandId.TrimStartAndEndInline();
		CommandBindingHash.TrimStartAndEndInline();
	}

	const FString& FUeAgentCommandRequest::GetCorrelationId() const
	{
		return CorrelationId;
	}

	const FString& FUeAgentCommandRequest::GetRequestId() const
	{
		return RequestId;
	}

	const FString& FUeAgentCommandRequest::GetCommandId() const
	{
		return CommandId;
	}

	const FString&
	FUeAgentCommandRequest::GetCommandBindingHash() const
	{
		return CommandBindingHash;
	}

	const TSharedPtr<FJsonObject>&
	FUeAgentCommandRequest::GetParams() const
	{
		return Params;
	}

	const FString& FUeAgentCommandRequest::GetParamsJson() const
	{
		return ParamsJson;
	}

	bool FUeAgentCommandRequest::IsValid() const
	{
		return !CorrelationId.IsEmpty()
			&& !RequestId.IsEmpty()
			&& !CommandId.IsEmpty()
			&& !CommandBindingHash.IsEmpty()
			&& Params.IsValid()
			&& !ParamsJson.IsEmpty();
	}

	FUeAgentCommandResult FUeAgentCommandResult::Success(
		const FUeAgentCommandRequest& Request,
		FString PayloadJson)
	{
		return FUeAgentCommandResult(
			Request.GetCorrelationId(),
			Request.GetRequestId(),
			Request.GetCommandId(),
			Request.GetCommandBindingHash(),
			MoveTemp(PayloadJson),
			{},
			true,
			false);
	}

	FUeAgentCommandResult FUeAgentCommandResult::Failure(
		const FUeAgentCommandRequest& Request,
		FString PayloadJson,
		FString ErrorJson)
	{
		return FUeAgentCommandResult(
			Request.GetCorrelationId(),
			Request.GetRequestId(),
			Request.GetCommandId(),
			Request.GetCommandBindingHash(),
			MoveTemp(PayloadJson),
			MoveTemp(ErrorJson),
			false,
			false);
	}

	FUeAgentCommandResult FUeAgentCommandResult::ProtocolError(
		FString InCorrelationId,
		FString InRequestId,
		FString InCommandId,
		FString InCommandBindingHash,
		FString Code,
		FString Message)
	{
		FString ErrorJson(TEXT(
			"{\"code\":\"command_protocol_error\","
			"\"message\":\"failed to encode protocol error\"}"));
		FString Error;
		Json::TryCanonicalize(
			ErrorObject(Code, Message),
			ErrorJson,
			Error);
		return FUeAgentCommandResult(
			MoveTemp(InCorrelationId),
			MoveTemp(InRequestId),
			MoveTemp(InCommandId),
			MoveTemp(InCommandBindingHash),
			{},
			MoveTemp(ErrorJson),
			false,
			true);
	}

	FUeAgentCommandResult::FUeAgentCommandResult(
		FString InCorrelationId,
		FString InRequestId,
		FString InCommandId,
		FString InCommandBindingHash,
		FString InPayloadJson,
		FString InErrorJson,
		bool bInOk,
		bool bInProtocolError)
		: CorrelationId(MoveTemp(InCorrelationId))
		, RequestId(MoveTemp(InRequestId))
		, CommandId(MoveTemp(InCommandId))
		, CommandBindingHash(MoveTemp(InCommandBindingHash))
		, PayloadJson(MoveTemp(InPayloadJson))
		, ErrorJson(MoveTemp(InErrorJson))
		, bOk(bInOk)
		, bProtocolError(bInProtocolError)
	{
		CorrelationId.TrimStartAndEndInline();
		RequestId.TrimStartAndEndInline();
		CommandId.TrimStartAndEndInline();
		CommandBindingHash.TrimStartAndEndInline();
	}

	const FString& FUeAgentCommandResult::GetCorrelationId() const
	{
		return CorrelationId;
	}

	const FString& FUeAgentCommandResult::GetRequestId() const
	{
		return RequestId;
	}

	const FString& FUeAgentCommandResult::GetCommandId() const
	{
		return CommandId;
	}

	const FString&
	FUeAgentCommandResult::GetCommandBindingHash() const
	{
		return CommandBindingHash;
	}

	const FString& FUeAgentCommandResult::GetPayloadJson() const
	{
		return PayloadJson;
	}

	const FString& FUeAgentCommandResult::GetErrorJson() const
	{
		return ErrorJson;
	}

	bool FUeAgentCommandResult::IsOk() const
	{
		return bOk;
	}

	bool FUeAgentCommandResult::IsProtocolError() const
	{
		return bProtocolError;
	}

	bool FUeAgentCommandResult::IsValid() const
	{
		if (CorrelationId.IsEmpty())
		{
			return false;
		}
		if (bProtocolError)
		{
			return !bOk
				&& PayloadJson.IsEmpty()
				&& !ErrorJson.IsEmpty();
		}
		if (RequestId.IsEmpty()
			|| CommandId.IsEmpty()
			|| CommandBindingHash.IsEmpty()
			|| PayloadJson.IsEmpty())
		{
			return false;
		}
		return bOk
			? ErrorJson.IsEmpty()
			: !ErrorJson.IsEmpty();
	}

	bool FUeAgentCommandResult::MatchesRequest(
		const FUeAgentCommandRequest& Request) const
	{
		return CorrelationId == Request.GetCorrelationId()
			&& RequestId == Request.GetRequestId()
			&& CommandId == Request.GetCommandId()
			&& CommandBindingHash
				== Request.GetCommandBindingHash();
	}

	bool FUeAgentCommandProtocolCodec::EncodeRequest(
		const FUeAgentCommandRequest& Request,
		TArray<uint8>& OutPayload,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutPayload.Reset();
		OutDiagnostic = {};
		if (!Request.IsValid())
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_invalid"),
				TEXT("command request is invalid"));
		}

		TSharedPtr<FJsonObject> Params;
		if (!TryParseObjectText(
			Request.GetParamsJson(),
			Params,
			OutDiagnostic))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), WireSchema);
		Root->SetNumberField(TEXT("version"), WireVersion);
		Root->SetStringField(
			TEXT("message_kind"),
			TEXT("command_request"));
		Root->SetStringField(
			TEXT("correlation_id"),
			Request.GetCorrelationId());
		Root->SetStringField(
			TEXT("request_id"),
			Request.GetRequestId());
		Root->SetStringField(
			TEXT("command_id"),
			Request.GetCommandId());
		Root->SetStringField(
			TEXT("command_binding_hash"),
			Request.GetCommandBindingHash());
		Root->SetObjectField(TEXT("params"), Params);

		FString Error;
		if (!Json::TryCanonicalUtf8(Root, OutPayload, Error))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_encode_failed"),
				Error);
		}
		return true;
	}

	bool FUeAgentCommandProtocolCodec::DecodeRequest(
		const TArray<uint8>& Payload,
		TOptional<FUeAgentCommandRequest>& OutRequest,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutRequest.Reset();
		OutDiagnostic = {};
		TSharedPtr<FJsonObject> Root;
		FString Error;
		if (!Json::TryParseObject(Payload, Root, Error))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_json_invalid"),
				Error);
		}

		const TSet<FString> Fields{
			TEXT("schema"),
			TEXT("version"),
			TEXT("message_kind"),
			TEXT("correlation_id"),
			TEXT("request_id"),
			TEXT("command_id"),
			TEXT("command_binding_hash"),
			TEXT("params"),
		};
		if (!HasExactFields(Root, Fields))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_envelope_invalid"),
				TEXT("command request envelope fields are invalid"));
		}

		FString Schema;
		FString MessageKind;
		FString CorrelationId;
		FString RequestId;
		FString CommandId;
		FString BindingHash;
		int32 Version = 0;
		const TSharedPtr<FJsonObject>* ParamsField = nullptr;
		if (!TryRequiredString(Root, TEXT("schema"), Schema)
			|| Schema != WireSchema
			|| !TryWireVersion(Root, Version)
			|| Version != WireVersion
			|| !TryRequiredString(
				Root,
				TEXT("message_kind"),
				MessageKind)
			|| MessageKind != TEXT("command_request")
			|| !TryRequiredString(
				Root,
				TEXT("correlation_id"),
				CorrelationId)
			|| !TryRequiredString(
				Root,
				TEXT("request_id"),
				RequestId)
			|| !TryRequiredString(
				Root,
				TEXT("command_id"),
				CommandId)
			|| !TryRequiredString(
				Root,
				TEXT("command_binding_hash"),
				BindingHash)
			|| !Root->TryGetObjectField(
				TEXT("params"),
				ParamsField)
			|| !ParamsField
			|| !ParamsField->IsValid())
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_envelope_invalid"),
				TEXT("command request envelope values are invalid"));
		}

		FString ParamsJson;
		if (!TryCanonicalObject(
			*ParamsField,
			ParamsJson,
			OutDiagnostic))
		{
			return false;
		}
		OutRequest = FUeAgentCommandRequest(
			MoveTemp(CorrelationId),
			MoveTemp(RequestId),
			MoveTemp(CommandId),
			MoveTemp(BindingHash),
			*ParamsField,
			MoveTemp(ParamsJson));
		return OutRequest->IsValid();
	}

	bool FUeAgentCommandProtocolCodec::EncodeResult(
		const FUeAgentCommandResult& Result,
		TArray<uint8>& OutPayload,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutPayload.Reset();
		OutDiagnostic = {};
		if (!Result.IsValid())
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_invalid"),
				TEXT("command result is invalid"));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), WireSchema);
		Root->SetNumberField(TEXT("version"), WireVersion);
		Root->SetStringField(
			TEXT("message_kind"),
			Result.IsProtocolError()
				? TEXT("protocol_error")
				: TEXT("command_result"));
		Root->SetStringField(
			TEXT("correlation_id"),
			Result.GetCorrelationId());
		Root->SetStringField(
			TEXT("request_id"),
			Result.GetRequestId());
		Root->SetStringField(
			TEXT("command_id"),
			Result.GetCommandId());
		Root->SetStringField(
			TEXT("command_binding_hash"),
			Result.GetCommandBindingHash());

		if (Result.IsProtocolError())
		{
			TSharedPtr<FJsonObject> ErrorObjectValue;
			if (!TryParseObjectText(
				Result.GetErrorJson(),
				ErrorObjectValue,
				OutDiagnostic))
			{
				return false;
			}
			Root->SetObjectField(TEXT("error"), ErrorObjectValue);
		}
		else
		{
			TSharedPtr<FJsonObject> PayloadObject;
			if (!TryParseObjectText(
				Result.GetPayloadJson(),
				PayloadObject,
				OutDiagnostic))
			{
				return false;
			}
			Root->SetBoolField(TEXT("ok"), Result.IsOk());
			Root->SetObjectField(TEXT("payload"), PayloadObject);
			if (Result.IsOk())
			{
				Root->SetField(
					TEXT("error"),
					MakeShared<FJsonValueNull>());
			}
			else
			{
				TSharedPtr<FJsonObject> ErrorObjectValue;
				if (!TryParseObjectText(
					Result.GetErrorJson(),
					ErrorObjectValue,
					OutDiagnostic))
				{
					return false;
				}
				Root->SetObjectField(
					TEXT("error"),
					ErrorObjectValue);
			}
		}

		FString Error;
		if (!Json::TryCanonicalUtf8(Root, OutPayload, Error))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_encode_failed"),
				Error);
		}
		return true;
	}

	bool FUeAgentCommandProtocolCodec::DecodeResult(
		const TArray<uint8>& Payload,
		TOptional<FUeAgentCommandResult>& OutResult,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutResult.Reset();
		OutDiagnostic = {};
		TSharedPtr<FJsonObject> Root;
		FString Error;
		if (!Json::TryParseObject(Payload, Root, Error))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_json_invalid"),
				Error);
		}

		FString Schema;
		FString MessageKind;
		FString CorrelationId;
		int32 Version = 0;
		if (!TryRequiredString(Root, TEXT("schema"), Schema)
			|| Schema != WireSchema
			|| !TryWireVersion(Root, Version)
			|| Version != WireVersion
			|| !TryRequiredString(
				Root,
				TEXT("message_kind"),
				MessageKind)
			|| !TryRequiredString(
				Root,
				TEXT("correlation_id"),
				CorrelationId))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_envelope_invalid"),
				TEXT("command result envelope values are invalid"));
		}

		const bool bProtocol =
			MessageKind == TEXT("protocol_error");
		const TSet<FString> ProtocolFields{
			TEXT("schema"),
			TEXT("version"),
			TEXT("message_kind"),
			TEXT("correlation_id"),
			TEXT("request_id"),
			TEXT("command_id"),
			TEXT("command_binding_hash"),
			TEXT("error"),
		};
		const TSet<FString> ResultFields{
			TEXT("schema"),
			TEXT("version"),
			TEXT("message_kind"),
			TEXT("correlation_id"),
			TEXT("request_id"),
			TEXT("command_id"),
			TEXT("command_binding_hash"),
			TEXT("ok"),
			TEXT("payload"),
			TEXT("error"),
		};
		if ((!bProtocol && MessageKind != TEXT("command_result"))
			|| !HasExactFields(
				Root,
				bProtocol ? ProtocolFields : ResultFields))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_envelope_invalid"),
				TEXT("command result envelope fields are invalid"));
		}

		FString RequestId;
		FString CommandId;
		FString BindingHash;
		Root->TryGetStringField(TEXT("request_id"), RequestId);
		Root->TryGetStringField(TEXT("command_id"), CommandId);
		Root->TryGetStringField(
			TEXT("command_binding_hash"),
			BindingHash);
		FString ErrorJson;

		if (bProtocol)
		{
			const TSharedPtr<FJsonObject>* ErrorField = nullptr;
			if (!Root->TryGetObjectField(
				TEXT("error"),
				ErrorField)
				|| !ErrorField
				|| !ErrorField->IsValid()
				|| !TryCanonicalObject(
					*ErrorField,
					ErrorJson,
					OutDiagnostic))
			{
				return Fail(
					OutDiagnostic,
					TEXT("command_result_error_invalid"),
					TEXT(
						"protocol error must contain an error object"));
			}
			OutResult = FUeAgentCommandResult(
				MoveTemp(CorrelationId),
				MoveTemp(RequestId),
				MoveTemp(CommandId),
				MoveTemp(BindingHash),
				FString(),
				MoveTemp(ErrorJson),
				false,
				true);
			return OutResult->IsValid();
		}

		bool bOk = false;
		if (!Root->TryGetBoolField(TEXT("ok"), bOk))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_ok_invalid"),
				TEXT("command result ok must be boolean"));
		}
		const TSharedPtr<FJsonObject>* PayloadField = nullptr;
		if (!Root->TryGetObjectField(
			TEXT("payload"),
			PayloadField)
			|| !PayloadField
			|| !PayloadField->IsValid())
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_payload_invalid"),
				TEXT("command result payload must be an object"));
		}
		FString PayloadJson;
		if (!TryCanonicalObject(
			*PayloadField,
			PayloadJson,
			OutDiagnostic))
		{
			return false;
		}
		const TSharedPtr<FJsonValue> ErrorValue =
			Root->TryGetField(TEXT("error"));
		if (bOk
			&& (!ErrorValue.IsValid()
				|| ErrorValue->Type != EJson::Null))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_result_error_invalid"),
				TEXT("successful command result error must be null"));
		}
		if (bOk)
		{
			ErrorJson.Reset();
		}
		else
		{
			const TSharedPtr<FJsonObject>* ErrorField = nullptr;
			if (!Root->TryGetObjectField(
				TEXT("error"),
				ErrorField)
				|| !ErrorField
				|| !ErrorField->IsValid()
				|| !TryCanonicalObject(
					*ErrorField,
					ErrorJson,
					OutDiagnostic))
			{
				return Fail(
					OutDiagnostic,
					TEXT("command_result_error_invalid"),
					TEXT(
						"failed command result must contain an error object"));
			}
		}
		OutResult = FUeAgentCommandResult(
			MoveTemp(CorrelationId),
			MoveTemp(RequestId),
			MoveTemp(CommandId),
			MoveTemp(BindingHash),
			MoveTemp(PayloadJson),
			MoveTemp(ErrorJson),
			bOk,
			false);
		return OutResult->IsValid();
	}

	FUeAgentCommandIngress::FUeAgentCommandIngress(
		FUeAgentCommandProtocolCodec InCodec,
		TSharedRef<const FUeAgentCommandContractRegistry>
			InRegistry,
		TSharedRef<const FUeAgentCommandDispatch> InDispatch,
		FUeAgentCommandCallerFactory InCallerFactory,
		FUeAgentCommandCancellationFactory InCancellationFactory)
		: Codec(MoveTemp(InCodec))
		, Registry(MoveTemp(InRegistry))
		, Dispatch(MoveTemp(InDispatch))
		, CallerFactory(MoveTemp(InCallerFactory))
		, CancellationFactory(MoveTemp(InCancellationFactory))
	{
	}

	FUeAgentTransportReply FUeAgentCommandIngress::Handle(
		const FUeAgentTransportIngress& Ingress)
	{
		const FString& CorrelationId =
			Ingress.Frame.GetCorrelationId();
		const auto ProtocolFailure =
			[&](
				const FString& RequestId,
				const FString& CommandId,
				const FString& BindingHash,
				const FString& Code,
				const FString& Message)
			{
				return MakeReply(
					Ingress,
					FUeAgentCommandResult::ProtocolError(
						CorrelationId,
						RequestId,
						CommandId,
						BindingHash,
						Code,
						Message));
			};

		if (Ingress.Frame.GetChannel() != WireChannel)
		{
			return ProtocolFailure(
				{},
				{},
				{},
				TEXT("command_channel_invalid"),
				TEXT("transport channel is not uai.command"));
		}

		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic ProtocolDiagnostic;
		if (!Codec.DecodeRequest(
			Ingress.Frame.GetPayload(),
			Request,
			ProtocolDiagnostic))
		{
			return ProtocolFailure(
				{},
				{},
				{},
				ProtocolDiagnostic.Code,
				ProtocolDiagnostic.Message);
		}
		if (Request->GetCorrelationId() != CorrelationId)
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_correlation_mismatch"),
				TEXT("transport and command correlation differ"));
		}

		const FUeAgentCommandContract* Command =
			Registry->FindExact(Request->GetCommandId());
		if (!Command)
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_id_unknown"),
				TEXT("command ID is not registered"));
		}
		if (Command->GetBindingHash()
			!= Request->GetCommandBindingHash())
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_binding_hash_mismatch"),
				TEXT("command binding hash does not match"));
		}
		if (WireVersion < Command->GetMinWireVersion()
			|| WireVersion > Command->GetMaxWireVersion())
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_wire_version_unsupported"),
				TEXT("command does not support this wire version"));
		}
		if (!TryValidateParams(
			*Command,
			Request->GetParams(),
			ProtocolDiagnostic))
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				ProtocolDiagnostic.Code,
				ProtocolDiagnostic.Message);
		}
		if (!CallerFactory || !CancellationFactory)
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_ingress_dependency_missing"),
				TEXT("command ingress dependency is missing"));
		}

		FString CallerError;
		TOptional<FUeAgentCallerContext> Caller =
			CallerFactory(
				Ingress.Peer,
				CorrelationId,
				CallerError);
		if (!Caller.IsSet() || !Caller->IsValid())
		{
			return ProtocolFailure(
				Request->GetRequestId(),
				Request->GetCommandId(),
				Request->GetCommandBindingHash(),
				TEXT("command_caller_invalid"),
				CallerError);
		}

		const TSharedRef<IUeAgentCancellationSignal> Cancellation =
			CancellationFactory(CorrelationId);
		return MakeReply(
			Ingress,
			Dispatch->Submit(
				Request.GetValue(),
				Caller.GetValue(),
				Cancellation));
	}

	FUeAgentTransportReply FUeAgentCommandIngress::MakeReply(
		const FUeAgentTransportIngress& Ingress,
		const FUeAgentCommandResult& Result) const
	{
		FUeAgentCommandProtocolDiagnostic DiagnosticValue;
		TArray<uint8> Payload;
		if (!Codec.EncodeResult(
			Result,
			Payload,
			DiagnosticValue))
		{
			const FUeAgentCommandResult Replacement =
				FUeAgentCommandResult::ProtocolError(
					Ingress.Frame.GetCorrelationId(),
					{},
					{},
					{},
					DiagnosticValue.Code,
					DiagnosticValue.Message);
			Codec.EncodeResult(
				Replacement,
				Payload,
				DiagnosticValue);
		}
		return FUeAgentTransportReply(FUeAgentTransportFrame(
			Ingress.Frame.GetChannel(),
			MoveTemp(Payload),
			Ingress.Frame.GetCorrelationId(),
			Ingress.Peer));
	}
}
