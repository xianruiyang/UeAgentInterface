#include "CommandHandlers/ModelingGeometry/UeAgentModelingGeometryCommandHandlers.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::ModelingGeometry
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("modeling-geometry-owner");

		FString JsonText(const TSharedPtr<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return CommandContract::Json::TryCanonicalize(Object, Text, Error)
				? Text : FString(TEXT("{}"));
		}

		FUeAgentCommandResult InputFailure(
			const FUeAgentCommandRequest& Request,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("modeling_session_input_invalid"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Error->SetBoolField(TEXT("retry_safe"), true);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), JsonText(Error));
		}
		FUeAgentCommandResult Unauthorized(const FUeAgentCommandRequest& Request)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("modeling_geometry_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), TEXT("uai:operate is required"));
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InId) : Id(MoveTemp(InId)) {}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Unauthorized(Request);
				}
				return OperationOwners::ModelingGeometry::Submit(
					Request, Caller, Cancellation);
			}
		private:
			FString Id;
		};

		class FModelingSessionHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FModelingSessionHandler(
				TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
					InNativeMechanicsPort)
				: NativeMechanicsPort(MoveTemp(InNativeMechanicsPort))
			{
			}

			const FString& GetCommandId() const override
			{
				static const FString Id(TEXT("mesh.modeling_session"));
				return Id;
			}
			const FString& GetPublicOwner() const override { return Owner; }

			bool BindNestedExactDispatch(
				TWeakPtr<const FUeAgentExactCommandDispatch> InDispatch,
				TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry> InRegistry,
				FString& OutError) override
			{
				OutError.Reset();
				if (!InDispatch.IsValid()
					|| !InRegistry->FindExact(TEXT("world.selection"))
					|| !InRegistry->FindExact(TEXT("mesh.element.select_by_ray"))
					|| !InRegistry->FindExact(TEXT("modeling.actor.mesh.replace"))
					|| !InRegistry->FindExact(TEXT("world.object_snap_to_surface")))
				{
					OutError = TEXT("modeling_session_native_successor_dependency_missing");
					return false;
				}
				Dispatch = MoveTemp(InDispatch);
				Registry = MoveTemp(InRegistry);
				return true;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Unauthorized(Request);
				}
				if (Cancellation->IsCancelled())
				{
					return InputFailure(Request, TEXT("request_cancelled_before_start"));
				}
				FString Action;
				if (!Request.GetParams().IsValid()
					|| !Request.GetParams()->TryGetStringField(TEXT("action"), Action)
					|| !DomainAdapters::ModelingGeometry::IsModelingSessionAction(Action))
				{
					return InputFailure(Request, TEXT("action_missing_or_unknown"));
				}
				if (const FString* Successor = ExactSuccessorByAction.Find(Action))
				{
					return SubmitExactSuccessor(Request, Caller, Cancellation, *Successor);
				}
				return SubmitNativeModelingAction(Request, Action);
			}

		private:
			FUeAgentCommandResult SubmitNativeModelingAction(
				const FUeAgentCommandRequest& Request,
				const FString& Action) const
			{
				const TSharedPtr<FJsonObject>* Input = nullptr;
				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				if (Request.GetParams()->TryGetObjectField(TEXT("input"), Input)
					&& Input && Input->IsValid())
				{
					Params = MakeShared<FJsonObject>(**Input);
				}
				FString Target;
				if (Request.GetParams()->TryGetStringField(TEXT("target"), Target)
					&& !Target.IsEmpty())
				{
					if (Action == TEXT("set_actor_selection")
						|| Action == TEXT("save_mesh_asset"))
					{
						if (!Params->HasField(TEXT("actor_ids")))
						{
							Params->SetArrayField(TEXT("actor_ids"), {
								MakeShared<FJsonValueString>(Target)});
						}
					}
					else
					{
						Params->SetStringField(TEXT("actor_id"), Target);
					}
				}

				DomainAdapters::ModelingGeometry::FModelingSessionNativeRequest NativeRequest;
				NativeRequest.RequestId = Request.GetRequestId();
				NativeRequest.CommandId = Request.GetCommandId();
				NativeRequest.Params = MoveTemp(Params);
				TSharedPtr<FJsonObject> Observation;
				FString Error;
				const bool bOk = NativeMechanicsPort->Execute(
					Action, NativeRequest, Observation, Error);
				if (!bOk)
				{
					const TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
					ErrorObject->SetStringField(TEXT("code"), TEXT("modeling_session_action_failed"));
					ErrorObject->SetStringField(TEXT("subject_id"), Action);
					ErrorObject->SetStringField(TEXT("detail"), Error);
					ErrorObject->SetStringField(TEXT("terminal_status"), TEXT("failed"));
					ErrorObject->SetBoolField(TEXT("retry_safe"), false);
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), JsonText(ErrorObject));
				}
				const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("action"), Action);
				Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Payload->SetObjectField(TEXT("observation"), Observation.IsValid()
					? Observation : MakeShared<FJsonObject>());
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {});
				return FUeAgentCommandResult::Success(Request, JsonText(Payload));
			}

			FUeAgentCommandResult SubmitExactSuccessor(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
				const FString& SuccessorId) const
			{
				const TSharedPtr<const FUeAgentExactCommandDispatch> Bound = Dispatch.Pin();
				const CommandContract::FUeAgentFrozenCommandContract* Contract =
					Registry.IsValid() ? Registry->FindExact(SuccessorId) : nullptr;
				if (!Bound.IsValid() || Contract == nullptr)
				{
					return InputFailure(Request, TEXT("modeling_session_successor_unavailable"));
				}
				const TSharedPtr<FJsonObject>* Input = nullptr;
				TSharedPtr<FJsonObject> SuccessorParams = MakeShared<FJsonObject>();
				if (Request.GetParams()->TryGetObjectField(TEXT("input"), Input)
					&& Input && Input->IsValid())
				{
					SuccessorParams = MakeShared<FJsonObject>(**Input);
				}
				FString Target;
				if (Request.GetParams()->TryGetStringField(TEXT("target"), Target)
					&& !Target.IsEmpty())
				{
					if (SuccessorId == TEXT("modeling.actor.mesh.replace"))
					{
						SuccessorParams->SetStringField(TEXT("actor_ref"), Target);
					}
					else
					{
						SuccessorParams->SetStringField(TEXT("target"), Target);
					}
				}
				FString ParamsJson;
				FString JsonError;
				if (!CommandContract::Json::TryCanonicalize(SuccessorParams, ParamsJson, JsonError))
				{
					return InputFailure(Request, JsonError);
				}
				const TSharedPtr<FJsonObject> Wire = MakeShared<FJsonObject>();
				Wire->SetStringField(TEXT("schema"), TEXT("uai.command.v1"));
				Wire->SetNumberField(TEXT("version"), 1);
				Wire->SetStringField(TEXT("channel"), TEXT("uai.command"));
				Wire->SetStringField(TEXT("correlation_id"), Request.GetCorrelationId());
				Wire->SetStringField(TEXT("request_id"), Request.GetRequestId() + TEXT(":modeling-session"));
				Wire->SetStringField(TEXT("command_id"), SuccessorId);
				Wire->SetStringField(TEXT("command_binding_hash"), Contract->GetContractHash());
				Wire->SetObjectField(TEXT("params"), SuccessorParams);
				FString WireText;
				if (!CommandContract::Json::TryCanonicalize(Wire, WireText, JsonError))
				{
					return InputFailure(Request, JsonError);
				}
				FUeAgentCommandProtocolCodec Codec;
				const FTCHARToUTF8 Converted(*WireText);
				TArray<uint8> Bytes;
				Bytes.Append(
					reinterpret_cast<const uint8*>(Converted.Get()),
					Converted.Length());
				TOptional<FUeAgentCommandRequest> Nested;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				if (!Codec.DecodeRequest(Bytes, Nested, Diagnostic) || !Nested.IsSet())
				{
					return InputFailure(Request, Diagnostic.Message);
				}
				const FUeAgentCommandResult NestedResult = Bound->Submit(Nested.GetValue(), Caller, Cancellation);
				if (!NestedResult.IsOk())
				{
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), NestedResult.GetErrorJson());
				}
				const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("action"), Request.GetParams()->GetStringField(TEXT("action")));
				Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Payload->SetStringField(TEXT("observation_json"), NestedResult.GetPayloadJson());
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {});
				return FUeAgentCommandResult::Success(Request, JsonText(Payload));
			}

			TWeakPtr<const FUeAgentExactCommandDispatch> Dispatch;
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
			TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
				NativeMechanicsPort;
			const TMap<FString, FString> ExactSuccessorByAction = {
				{TEXT("observe_selection"), TEXT("world.selection")},
				{TEXT("set_actor_selection"), TEXT("world.selection")},
				{TEXT("select_via_world_ray"), TEXT("mesh.element.select_by_ray")},
				{TEXT("replace_actor_mesh"), TEXT("modeling.actor.mesh.replace")},
				{TEXT("snap_to_surface"), TEXT("world.object_snap_to_surface")},
			};
		};
	}

	bool TryCreateHandlers(
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset(); OutError.Reset();
		for (const FString& Id : DomainAdapters::ModelingGeometry::GetCommandIds())
		{
			OutHandlers.Add(Id == TEXT("mesh.modeling_session")
				? StaticCastSharedRef<IUeAgentExactCommandHandler>(
					MakeShared<FModelingSessionHandler>(NativeMechanicsPort))
				: StaticCastSharedRef<IUeAgentExactCommandHandler>(MakeShared<FHandler>(Id)));
		}
		return OutHandlers.Num() == 34;
	}
}
