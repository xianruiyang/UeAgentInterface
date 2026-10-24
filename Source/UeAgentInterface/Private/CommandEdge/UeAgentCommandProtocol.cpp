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
			return FDiagnostic::Failure(Code, Message, TEXT("command_protocol"));
		}

		bool Fail(
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic,
			const FString& Code,
			const FString& Message)
		{
			OutDiagnostic = Diagnostic(Code, Message);
			return false;
		}

		bool Fail(FUeAgentCommandProtocolDiagnostic& OutDiagnostic, const FString& Code, const FDiagnostic& Cause)
		{
			OutDiagnostic = FDiagnostic::WrapFailure(Code, Cause, TEXT("command_protocol"));
			return false;
		}

		enum class EValueAudience { Public, Internal };

		FString FieldPath(const FString& Name, const TCHAR* Parent = TEXT(""))
		{
			return FString(Parent) + TEXT("/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		bool FailValue(FUeAgentCommandProtocolDiagnostic& OutDiagnostic, const FString& Code,
			const FString& Message, const FString& Path, TSharedPtr<FJsonValue> Expected,
			TSharedPtr<FJsonValue> Actual, EValueAudience Audience)
		{
			OutDiagnostic = Diagnostic(Code, Message);
			OutDiagnostic.Source.JsonPath = Path;
			OutDiagnostic.ExpectedValue = MoveTemp(Expected);
			OutDiagnostic.ActualValuePresent = Actual.IsValid();
			if (Audience == EValueAudience::Public) OutDiagnostic.ActualValue = MoveTemp(Actual);
			else OutDiagnostic.ActualEvidence = MoveTemp(Actual);
			return false;
		}

		bool TryValidateEnvelopeFields(
			const TSharedPtr<FJsonObject>& Object,
			const TArray<FString>& Required,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
		{
			for (const FString& Name : Required)
			{
				if (!Object->HasField(Name))
				{
					return FailValue(OutDiagnostic, TEXT("command_request_envelope_invalid"),
						FString::Printf(TEXT("Required command envelope field '%s' is missing."), *Name),
						FieldPath(Name), MakeShared<FJsonValueString>(TEXT("required field")), nullptr, EValueAudience::Internal);
				}
			}
			for (const auto& Field : Object->Values)
			{
				if (!Required.Contains(Field.Key))
				{
					return FailValue(OutDiagnostic, TEXT("command_request_envelope_invalid"),
						FString::Printf(TEXT("Unknown command envelope field '%s'."), *Field.Key),
						FieldPath(Field.Key), nullptr, Field.Value, EValueAudience::Internal);
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
			FDiagnostic Error;
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
			FDiagnostic Error;
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

		FString ExpectedParameterType(const FUeAgentCommandParameter& Parameter)
		{
			const TCHAR* Type = TEXT("any");
			switch (Parameter.GetWireType())
			{
			case EUeAgentCommandWireType::Boolean: Type = TEXT("boolean"); break;
			case EUeAgentCommandWireType::Integer: Type = TEXT("integer"); break;
			case EUeAgentCommandWireType::Number: Type = TEXT("number"); break;
			case EUeAgentCommandWireType::String: Type = TEXT("string"); break;
			case EUeAgentCommandWireType::Object: Type = TEXT("object"); break;
			case EUeAgentCommandWireType::Array: Type = TEXT("array"); break;
			default: break;
			}
			return FString(Type) + (Parameter.IsNullable() ? TEXT(" or null") : TEXT(""));
		}

		bool TryValidateParams(
			const FUeAgentCommandContract& Contract,
			const TSharedPtr<FJsonObject>& Params,
			FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
		{
			if (!Params.IsValid())
			{
				return FailValue(OutDiagnostic, TEXT("command_params_invalid"), TEXT("Command params must be an object."),
					TEXT("/params"), MakeShared<FJsonValueString>(TEXT("object")), nullptr, EValueAudience::Public);
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
						return FailValue(
							OutDiagnostic,
							TEXT("command_param_required"),
							FString::Printf(TEXT("Required parameter '%s' is missing; expected %s."),
								*Parameter.GetName(), *ExpectedParameterType(Parameter)),
							FieldPath(Parameter.GetName(), TEXT("/params")),
							MakeShared<FJsonValueString>(ExpectedParameterType(Parameter)), nullptr, EValueAudience::Public);
					}
					continue;
				}
				if (!TryValidateParameterValue(Parameter, Value))
				{
					return FailValue(
						OutDiagnostic,
						TEXT("command_param_type_invalid"),
						FString::Printf(TEXT("Parameter '%s' must be %s."),
							*Parameter.GetName(), *ExpectedParameterType(Parameter)),
						FieldPath(Parameter.GetName(), TEXT("/params")),
						MakeShared<FJsonValueString>(ExpectedParameterType(Parameter)), Value, EValueAudience::Public);
				}
			}

			for (const TPair<
				FString,
				TSharedPtr<FJsonValue>>& Field : Params->Values)
			{
				if (!ByName.Contains(Field.Key))
				{
					return FailValue(
						OutDiagnostic,
						TEXT("command_param_unknown"),
						FString::Printf(TEXT("Unknown parameter '%s'; use query.command.usage for required parameter names."), *Field.Key),
						FieldPath(Field.Key, TEXT("/params")), nullptr, Field.Value, EValueAudience::Public);
				}
			}
			return true;
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

	bool FUeAgentCommandRequest::TryCreate(
		FString InCorrelationId, FString InRequestId, FString InCommandId, FString InCommandBindingHash,
		TSharedPtr<FJsonObject> InParams, TOptional<FUeAgentCommandRequest>& OutRequest,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic)
	{
		OutRequest.Reset();
		OutDiagnostic.Reset();
		FString ParamsJson;
		if (!TryCanonicalObject(InParams, ParamsJson, OutDiagnostic)) return false;
		FUeAgentCommandRequest Request(MoveTemp(InCorrelationId), MoveTemp(InRequestId), MoveTemp(InCommandId),
			MoveTemp(InCommandBindingHash), MoveTemp(InParams), MoveTemp(ParamsJson));
		if (!Request.IsValid()) return Fail(OutDiagnostic, TEXT("command_request_invalid"), TEXT("Command request fields are invalid."));
		OutRequest = MoveTemp(Request);
		return true;
	}

	FUeAgentCommandResult FUeAgentCommandResult::Success(
		const FUeAgentCommandRequest& Request,
		FString PayloadJson, TArray<FDiagnostic> Diagnostics)
	{
		return FUeAgentCommandResult(
			Request.GetCorrelationId(),
			Request.GetRequestId(),
			Request.GetCommandId(),
			Request.GetCommandBindingHash(),
			MoveTemp(PayloadJson),
			{},
			true,
			false, FString(), EApplicationFailureKind::None, MoveTemp(Diagnostics));
	}

	FUeAgentCommandResult FUeAgentCommandResult::Failure(
		const FUeAgentCommandRequest& Request, FString PayloadJson, FDiagnostic Diagnostic, FString EnvelopeCode, EApplicationFailureKind ApplicationFailureKind, TArray<FDiagnostic> Diagnostics)
	{
		if (Diagnostic.TargetPath.IsEmpty() && Request.GetParams().IsValid())
			Request.GetParams()->TryGetStringField(TEXT("target"), Diagnostic.TargetPath);
		return FUeAgentCommandResult(Request.GetCorrelationId(), Request.GetRequestId(),
			Request.GetCommandId(), Request.GetCommandBindingHash(), MoveTemp(PayloadJson), MoveTemp(Diagnostic),
			false, false, MoveTemp(EnvelopeCode), ApplicationFailureKind, MoveTemp(Diagnostics));
	}

	FUeAgentCommandResult FUeAgentCommandResult::ProtocolError(
		FString InCorrelationId, FString InRequestId, FString InCommandId, FString InCommandBindingHash,
		FDiagnostic Diagnostic)
	{
		return FUeAgentCommandResult(MoveTemp(InCorrelationId), MoveTemp(InRequestId), MoveTemp(InCommandId),
			MoveTemp(InCommandBindingHash), {}, MoveTemp(Diagnostic), false, true);
	}

	FUeAgentCommandResult::FUeAgentCommandResult(
		FString InCorrelationId,
		FString InRequestId,
		FString InCommandId,
		FString InCommandBindingHash,
		FString InPayloadJson,
		FDiagnostic InDiagnostic,
		bool bInOk,
		bool bInProtocolError, FString InEnvelopeCode, EApplicationFailureKind InApplicationFailureKind, TArray<FDiagnostic> InDiagnostics)
		: CorrelationId(MoveTemp(InCorrelationId))
		, RequestId(MoveTemp(InRequestId))
		, CommandId(MoveTemp(InCommandId))
		, CommandBindingHash(MoveTemp(InCommandBindingHash))
		, PayloadJson(MoveTemp(InPayloadJson))
		, Diagnostic(MoveTemp(InDiagnostic))
		, Diagnostics(MoveTemp(InDiagnostics))
		, EnvelopeCode(MoveTemp(InEnvelopeCode))
		, ApplicationFailureKind(InApplicationFailureKind)
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

	const FDiagnostic& FUeAgentCommandResult::GetDiagnostic() const
	{
		return Diagnostic;
	}

	const TArray<FDiagnostic>& FUeAgentCommandResult::GetDiagnostics() const { return Diagnostics; }

	FUeAgentCommandResult FUeAgentCommandResult::WithPayload(FString InPayloadJson) const
	{
		FUeAgentCommandResult Copy = *this;
		Copy.PayloadJson = MoveTemp(InPayloadJson);
		return Copy;
	}

	const FString& FUeAgentCommandResult::GetErrorCode() const
	{
		return EnvelopeCode.IsEmpty() ? Diagnostic.Code : EnvelopeCode;
	}

	TSharedRef<FJsonObject> FUeAgentCommandResult::ToPublicError() const
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("code"), GetErrorCode());
		Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
		if (!bProtocolError)
		{
			if (!Diagnostic.RecoveryAction.IsEmpty()) Object->SetStringField(TEXT("recovery_action"), Diagnostic.RecoveryAction);
			if (!Diagnostic.TerminalStatus.IsEmpty()) Object->SetStringField(TEXT("terminal_status"), Diagnostic.TerminalStatus);
			Object->SetBoolField(TEXT("retry_safe"), Diagnostic.bRetrySafe);
			if (Diagnostic.MutationStarted.IsSet()) Object->SetBoolField(TEXT("mutation_started"), Diagnostic.MutationStarted.GetValue());
			if (Diagnostic.MutationMayHaveStarted.IsSet()) Object->SetBoolField(TEXT("mutation_may_have_started"), Diagnostic.MutationMayHaveStarted.GetValue());
		}
		return Object;
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
		FDiagnostic ValidationError;
		return TryValidate(ValidationError);
	}

	bool FUeAgentCommandResult::TryValidate(FDiagnostic& OutDiagnostic) const
	{
		OutDiagnostic.Reset();
		const TCHAR* Field = nullptr;
		const TCHAR* Reason = nullptr;
		if (CorrelationId.IsEmpty()) { Field = TEXT("correlation_id"); Reason = TEXT("The result requires a non-empty correlation identifier."); }
		else if (bProtocolError && bOk) { Field = TEXT("ok"); Reason = TEXT("A protocol error cannot report success."); }
		else if (bProtocolError && !PayloadJson.IsEmpty()) { Field = TEXT("payload"); Reason = TEXT("A protocol error must not contain a business payload."); }
		else if (bProtocolError && Diagnostic.IsEmpty()) { Field = TEXT("error.diagnostic"); Reason = TEXT("A protocol error requires a structured diagnostic."); }
		else if (!bProtocolError)
		{
			if (RequestId.IsEmpty()) { Field = TEXT("request_id"); Reason = TEXT("The result requires a non-empty request identifier."); }
			else if (CommandId.IsEmpty()) { Field = TEXT("command_id"); Reason = TEXT("The result requires a non-empty command identifier."); }
			else if (CommandBindingHash.IsEmpty()) { Field = TEXT("command_binding_hash"); Reason = TEXT("The result requires a command binding identity."); }
			else if (PayloadJson.IsEmpty()) { Field = TEXT("payload"); Reason = TEXT("The result requires a JSON business payload."); }
			else if (bOk && !Diagnostic.IsEmpty()) { Field = TEXT("error.diagnostic"); Reason = TEXT("A successful result cannot contain a primary failure diagnostic."); }
			else if (!bOk && Diagnostic.IsEmpty()) { Field = TEXT("error.diagnostic"); Reason = TEXT("A failed result requires a structured diagnostic."); }
		}
		if (!Reason) return true;
		OutDiagnostic = FDiagnostic::Failure(TEXT("command_result_invalid"), Reason, TEXT("command_protocol"));
		OutDiagnostic.PropertyPath = Field;
		if (!Diagnostic.IsEmpty()) OutDiagnostic.Diagnostics.Add(Diagnostic);
		OutDiagnostic.Diagnostics.Append(Diagnostics);
		return false;
	}

	bool FUeAgentCommandResult::TryValidateForRequest(const FUeAgentCommandRequest& Request, FDiagnostic& OutDiagnostic) const
	{
		if (!TryValidate(OutDiagnostic)) return false;
		const TCHAR* Field = nullptr;
		const FString* Actual = nullptr;
		const FString* Expected = nullptr;
		if (CorrelationId != Request.GetCorrelationId()) { Field = TEXT("correlation_id"); Actual = &CorrelationId; Expected = &Request.GetCorrelationId(); }
		else if (RequestId != Request.GetRequestId()) { Field = TEXT("request_id"); Actual = &RequestId; Expected = &Request.GetRequestId(); }
		else if (CommandId != Request.GetCommandId()) { Field = TEXT("command_id"); Actual = &CommandId; Expected = &Request.GetCommandId(); }
		else if (CommandBindingHash != Request.GetCommandBindingHash()) { Field = TEXT("command_binding_hash"); Actual = &CommandBindingHash; Expected = &Request.GetCommandBindingHash(); }
		if (!Field) return true;
		OutDiagnostic = FDiagnostic::Failure(TEXT("command_handler_result_identity_mismatch"),
			TEXT("The handler result identity does not match the submitted request."), TEXT("command_protocol"));
		OutDiagnostic.PropertyPath = Field;
		OutDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(*Actual);
		OutDiagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(*Expected);
		if (!Diagnostic.IsEmpty()) OutDiagnostic.Diagnostics.Add(Diagnostic);
		OutDiagnostic.Diagnostics.Append(Diagnostics);
		return false;
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

		FDiagnostic Error;
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
		FDiagnostic Error;
		if (!Json::TryParseObject(Payload, Root, Error))
		{
			return Fail(
				OutDiagnostic,
				TEXT("command_request_json_invalid"),
				Error);
		}

		const TArray<FString> Fields{
			TEXT("schema"),
			TEXT("version"),
			TEXT("message_kind"),
			TEXT("correlation_id"),
			TEXT("request_id"),
			TEXT("command_id"),
			TEXT("command_binding_hash"),
			TEXT("params"),
		};
		if (!TryValidateEnvelopeFields(Root, Fields, OutDiagnostic)) return false;

		FString Schema;
		FString MessageKind;
		FString CorrelationId;
		FString RequestId;
		FString CommandId;
		FString BindingHash;
		int32 Version = 0;
		const TSharedPtr<FJsonObject>* ParamsField = nullptr;
		const auto InvalidField = [&](const FString& Name, const FString& Expected, EValueAudience Audience)
		{
			return FailValue(OutDiagnostic, TEXT("command_request_envelope_invalid"),
				FString::Printf(TEXT("Command envelope field '%s' must be %s."), *Name, *Expected),
				FieldPath(Name), MakeShared<FJsonValueString>(Expected), Root->TryGetField(Name), Audience);
		};
		if (!TryRequiredString(Root, TEXT("schema"), Schema) || Schema != WireSchema)
			return InvalidField(TEXT("schema"), WireSchema, EValueAudience::Public);
		if (!TryWireVersion(Root, Version) || Version != WireVersion)
			return InvalidField(TEXT("version"), FString::FromInt(WireVersion), EValueAudience::Public);
		if (!TryRequiredString(Root, TEXT("message_kind"), MessageKind) || MessageKind != TEXT("command_request"))
			return InvalidField(TEXT("message_kind"), TEXT("command_request"), EValueAudience::Public);
		const auto ReadIdentity = [&](const FString& Name, FString& Value, EValueAudience Audience)
		{
			if (!TryRequiredString(Root, Name, Value) || Value.TrimStartAndEnd().IsEmpty())
				return InvalidField(Name, TEXT("a non-empty string"), Audience);
			return true;
		};
		if (!ReadIdentity(TEXT("correlation_id"), CorrelationId, EValueAudience::Internal)
			|| !ReadIdentity(TEXT("request_id"), RequestId, EValueAudience::Internal)
			|| !ReadIdentity(TEXT("command_id"), CommandId, EValueAudience::Public)
			|| !ReadIdentity(TEXT("command_binding_hash"), BindingHash, EValueAudience::Internal)) return false;
		if (!Root->TryGetObjectField(TEXT("params"), ParamsField) || !ParamsField || !ParamsField->IsValid())
			return InvalidField(TEXT("params"), TEXT("an object"), EValueAudience::Public);

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

	bool FUeAgentCommandProtocolCodec::TryProjectResult(
		const FUeAgentCommandResult& Result,
		TSharedPtr<FJsonObject>& OutObject,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutObject.Reset();
		OutDiagnostic = {};
		if (!Result.TryValidate(OutDiagnostic))
		{
			return false;
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
			Root->SetObjectField(TEXT("error"), Result.ToPublicError());
		}
		else
		{
			TSharedPtr<FJsonObject> PayloadObject;
			if (!TryParseObjectText(
				Result.GetPayloadJson(),
				PayloadObject,
				OutDiagnostic))
			{
				if (!Result.GetDiagnostic().IsEmpty()) OutDiagnostic.Diagnostics.Add(Result.GetDiagnostic());
				OutDiagnostic.Diagnostics.Append(Result.GetDiagnostics());
				return false;
			}
			if (!Result.IsOk() && Result.GetApplicationFailureKind() != EApplicationFailureKind::None)
			{
				PayloadObject->SetObjectField(Result.GetApplicationFailureKind() == EApplicationFailureKind::Query
					? TEXT("query_error") : TEXT("operation_error"), Result.ToPublicError());
			}
			if (!Result.GetDiagnostics().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> PublicDiagnostics;
				for (const FDiagnostic& Diagnostic : Result.GetDiagnostics())
					PublicDiagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic.ToPublicJson()));
				Root->SetArrayField(TEXT("diagnostics"), MoveTemp(PublicDiagnostics));
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
			Root->SetObjectField(TEXT("error"), Result.ToPublicError());
			}
		}
		OutObject = MoveTemp(Root);
		return true;
	}

	bool FUeAgentCommandProtocolCodec::EncodeResult(
		const FUeAgentCommandResult& Result,
		TArray<uint8>& OutPayload,
		FUeAgentCommandProtocolDiagnostic& OutDiagnostic) const
	{
		OutPayload.Reset();
		TSharedPtr<FJsonObject> Root;
		if (!TryProjectResult(Result, Root, OutDiagnostic)) return false;

		FDiagnostic Error;
		if (!Json::TryCanonicalUtf8(Root, OutPayload, Error))
		{
			Fail(
				OutDiagnostic,
				TEXT("command_result_encode_failed"),
				Error);
			if (!Result.GetDiagnostic().IsEmpty()) OutDiagnostic.Diagnostics.Add(Result.GetDiagnostic());
			OutDiagnostic.Diagnostics.Append(Result.GetDiagnostics());
			return false;
		}
		return true;
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
				const UE::UAI::Contract::FDiagnostic& Diagnostic)
			{
					FDiagnostic LocatedDiagnostic = Diagnostic;
					if (LocatedDiagnostic.CandidateName.IsEmpty()) LocatedDiagnostic.CandidateName = CommandId;
				return MakeReply(
					Ingress,
						FUeAgentCommandResult::ProtocolError(CorrelationId, RequestId, CommandId, BindingHash, MoveTemp(LocatedDiagnostic)));
			};

		if (Ingress.Frame.GetChannel() != WireChannel)
		{
			return ProtocolFailure({}, {}, {}, UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_channel_invalid"), TEXT("transport channel is not uai.command"), TEXT("command_protocol")));
		}

		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic ProtocolDiagnostic;
		if (!Codec.DecodeRequest(
			Ingress.Frame.GetPayload(),
			Request,
			ProtocolDiagnostic))
		{
			return ProtocolFailure({}, {}, {}, ProtocolDiagnostic);
		}
		if (Request->GetCorrelationId() != CorrelationId)
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_correlation_mismatch"), TEXT("transport and command correlation differ"), TEXT("command_protocol")));
		}

		const FUeAgentCommandContract* Command =
			Registry->FindExact(Request->GetCommandId());
		if (!Command)
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_id_unknown"), TEXT("command ID is not registered"), TEXT("command_protocol")));
		}
		if (WireVersion < Command->GetMinWireVersion()
			|| WireVersion > Command->GetMaxWireVersion())
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_wire_version_unsupported"), TEXT("command does not support this wire version"), TEXT("command_protocol")));
		}
		if (!TryValidateParams(
			*Command,
			Request->GetParams(),
			ProtocolDiagnostic))
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), ProtocolDiagnostic);
		}
		if (!CallerFactory || !CancellationFactory)
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_ingress_dependency_missing"), TEXT("command ingress dependency is missing"), TEXT("command_protocol")));
		}

		UE::UAI::Contract::FDiagnostic CallerError;
		TOptional<FUeAgentCallerContext> Caller =
			CallerFactory(
				Ingress.Peer,
				CorrelationId,
				CallerError);
		if (!Caller.IsSet() || !Caller->IsValid())
		{
			return ProtocolFailure(Request->GetRequestId(), Request->GetCommandId(), Request->GetCommandBindingHash(), CallerError.IsValid() ? CallerError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_caller_invalid"), TEXT("The command caller is invalid."), TEXT("command_protocol")));
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
				FUeAgentCommandResult::ProtocolError(Ingress.Frame.GetCorrelationId(), Result.GetRequestId(),
					Result.GetCommandId(), Result.GetCommandBindingHash(), DiagnosticValue);
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
