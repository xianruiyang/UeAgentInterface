#include "CommandHandlers/ModelingGeometry/UeAgentModelingGeometryCommandHandlers.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::CommandHandlers::ModelingGeometry
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("modeling-geometry-owner");

		void BindSessionInputSource(Contract::FDiagnostic& Error, bool bInjectedActorIds, bool bInjectedActorId)
		{
			for (auto& Child : Error.Diagnostics) BindSessionInputSource(Child, bInjectedActorIds, bInjectedActorId);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
				BindSessionInputSource(*Cause, bInjectedActorIds, bInjectedActorId); Error.PublicCause = MoveTemp(Cause);
			}
			if (Error.SourceKind == TEXT("native_result") || !Error.Source.SourceFile.IsEmpty()) return;
			const FString Pointer = !Error.Source.InternalJsonPath.IsEmpty() ? Error.Source.InternalJsonPath
				: !Error.SemanticPointer.IsEmpty() ? Error.SemanticPointer : Error.Source.JsonPath;
			if (Pointer.IsEmpty() || Pointer.StartsWith(TEXT("/params/"))) return;
			const bool bTarget = (bInjectedActorIds && (Pointer == TEXT("/actor_ids") || Pointer.StartsWith(TEXT("/actor_ids/"))))
				|| (bInjectedActorId && (Pointer == TEXT("/actor_id") || Pointer.StartsWith(TEXT("/actor_id/"))));
			const FString SourcePointer = bTarget ? TEXT("/params/target") : TEXT("/params/input") + (Pointer == TEXT("/") ? FString() : Pointer);
			Error.Source.JsonPath = SourcePointer; Error.Source.InternalJsonPath = SourcePointer; Error.SemanticPointer = SourcePointer;
			Error.SourceKind = TEXT("command_input");
		}

		FUeAgentCommandResult InputFailure(
            const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            Diagnostic.TerminalStatus = TEXT("not_started");
            Diagnostic.bRetrySafe = true;
            Diagnostic.MutationStarted = false;
            Diagnostic.MutationMayHaveStarted = false;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic),
                TEXT("modeling_session_input_invalid"));
        }
		FUeAgentCommandResult Unauthorized(const FUeAgentCommandRequest& Request)
        {
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_authority_required"),
                    TEXT("uai:operate is required."), TEXT("authorization"),
                    FString(), Request.GetCommandId()));
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
					return InputFailure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("request_cancelled_before_start"), TEXT("The request was cancelled before execution started."), TEXT("modeling_session_input")));
				}
				FString Action;
				if (!Request.GetParams().IsValid()
					|| !Request.GetParams()->TryGetStringField(TEXT("action"), Action)
					|| !DomainAdapters::ModelingGeometry::IsModelingSessionAction(Action))
				{
					auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("action_missing_or_unknown"), TEXT("/params/action"), MakeShared<FJsonValueString>(TEXT("supported modeling session action")), Request.GetParams().IsValid() ? Request.GetParams()->Values.FindRef(TEXT("action")) : nullptr, TEXT("modeling_session_input"), TEXT("Specify a supported modeling session action."));
					Error.SourceKind = TEXT("command_input");
					return InputFailure(Request, MoveTemp(Error));
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
				bool bInjectedActorIds = false, bInjectedActorId = false;
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
							bInjectedActorIds = true;
						}
					}
					else
					{
						Params->SetStringField(TEXT("actor_id"), Target);
						bInjectedActorId = true;
					}
				}

				DomainAdapters::ModelingGeometry::FModelingSessionNativeRequest NativeRequest;
				NativeRequest.RequestId = Request.GetRequestId();
				NativeRequest.CommandId = Request.GetCommandId();
				NativeRequest.Params = MoveTemp(Params);
				TSharedPtr<FJsonObject> Observation;
				UE::UAI::Contract::FDiagnostic Error;
				const bool bOk = NativeMechanicsPort->Execute(
					Action, NativeRequest, Observation, Error);
				if (!bOk)
				{
                    if (Error.IsEmpty()) {
                        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_session_action_failed"),
                            TEXT("The native modeling action returned failure without a diagnostic."), TEXT("modeling_session.") + Action);
                        Error.PropertyPath = TEXT("/action");
                        Error.ActualValue = MakeShared<FJsonValueString>(Action);
                        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native result with a specific diagnostic"));
                        Error.SourceKind = TEXT("native_result");
                    }
                    BindSessionInputSource(Error, bInjectedActorIds, bInjectedActorId);
                    if (Error.Stage.IsEmpty()) Error.Stage = TEXT("modeling_session.") + Action;
                    Error.TerminalStatus = TEXT("failed");
                    Error.bRetrySafe = false;
                    return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Error),
                        TEXT("modeling_session_action_failed"));
				}
				const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("action"), Action);
				Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
				Payload->SetObjectField(TEXT("observation"), Observation.IsValid()
					? Observation : MakeShared<FJsonObject>());
				Payload->SetArrayField(TEXT("errors"), {});
				Payload->SetArrayField(TEXT("warnings"), {});
				FString PayloadJson;
				Contract::FDiagnostic EncodeError;
				if (!CommandContract::Json::TryCanonicalize(Payload, PayloadJson, EncodeError))
				{
					if (EncodeError.SourceKind.IsEmpty()) EncodeError.SourceKind = TEXT("native_result");
					EncodeError.TerminalStatus = TEXT("failed");
					EncodeError.MutationMayHaveStarted = true;
					EncodeError.bRetrySafe = false;
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(EncodeError), TEXT("modeling_observation_encode_failed"));
				}
				return FUeAgentCommandResult::Success(Request, PayloadJson);
			}

			TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
				NativeMechanicsPort;
		};
	}

	bool TryCreateHandlers(
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset(); OutError.Reset();
		for (const FString& Id : DomainAdapters::ModelingGeometry::GetCommandIds())
		{
			OutHandlers.Add(Id == TEXT("mesh.modeling_session")
				? StaticCastSharedRef<IUeAgentExactCommandHandler>(
					MakeShared<FModelingSessionHandler>(NativeMechanicsPort))
				: StaticCastSharedRef<IUeAgentExactCommandHandler>(MakeShared<FHandler>(Id)));
		}
		return true;
	}
}
