#include "DomainAdapters/Reference/UeAgentReferenceMechanicsBoundary.h"

#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace UE::UAI::DomainAdapters::Reference
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace BindingGateway::Ticket;
	using namespace Composition::ReferenceCrud;
	using namespace Contract;
	using namespace DomainAdapters::GenericRead;
	using namespace DomainAdapters::ReferenceAsset;

	namespace
	{
		const FString ReferenceOperationDescriptorId =
			TEXT("uai.operation.reference.crud");
		const FString ReferenceOperationDescriptorHash =
			TEXT("sha256:fbde900ec387e61915a21e97ae5575a85337b3866d4299be961f75a5fc888491");

		FUeAgentDescriptorIdentity GetReferenceOperationDescriptorIdentity()
		{
			return FUeAgentDescriptorIdentity(
				ReferenceOperationDescriptorId,
				1,
				ReferenceOperationDescriptorHash);
		}

		bool TryCreateReferenceOperationDescriptorRegistry(
			TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
			FString& OutError)
		{
			return FUeAgentDescriptorRegistry::TryCreate(
				{
					FUeAgentOperationDescriptor(
						GetReferenceOperationDescriptorIdentity(),
						TEXT("reference.asset_apply"),
						TEXT("reference"),
						EUeAgentDescriptorOperationKind::Operation,
						FUeAgentExecutionContextPolicy(
							{EUeAgentStatePlane::Persisted},
							{},
							EUeAgentContextResolutionPolicy::ExplicitOnly,
							EUeAgentTransientContextPolicy::ReadOnly),
						FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
						FUeAgentContractValue(TEXT("{\"type\":\"object\"}"))),
				},
				OutRegistry,
				OutError);
		}

		FString ErrorJson(
			const FString& Code,
			const FString& Subject,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Result;
			FString Ignored;
			Json::TryCanonicalize(Error, Result, Ignored);
			return Result;
		}

		FString CanonicalObject(const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Object.IsValid()
				&& Json::TryCanonicalize(Object.ToSharedRef(), Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		class FReferenceMechanicsHandler final
			: public IUeAgentCommandHandler
		{
		public:
			explicit FReferenceMechanicsHandler(
				TSharedRef<FUeAgentReferenceCrudApplication> InApplication)
				: Application(MoveTemp(InApplication))
			{
			}

			const FString& GetCommandId() const override
			{
				return UE::UAI::DomainAdapters::Reference::GetCommandId();
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FUeAgentReferenceCrudCommand Command;
				Command.RequestId = Request.GetRequestId();
				Command.IdempotencyKey = TEXT("uai-command:")
					+ Request.GetRequestId();
				Command.ObjectPath = Params->GetStringField(TEXT("object_path"));
				Command.Intent = Params->GetStringField(TEXT("intent"));
				const TSharedPtr<FJsonValue> Proposed =
					Params->TryGetField(TEXT("proposed_text"));
				if (Proposed.IsValid() && Proposed->Type == EJson::String)
				{
					Command.ProposedText.Emplace(Proposed->AsString());
				}

				const Contract::FUeAgentOperationResult Result =
					Application->SubmitCommand(Command, Caller, Cancellation);
				FString ParseError;
				TSharedPtr<FJsonObject> Root;
				if (!Json::TryParseObject(
						Result.GetValue().GetJsonValue(), Root, ParseError)
					|| !Root.IsValid())
				{
					return FUeAgentCommandResult::Failure(
						Request, TEXT("{}"),
						ErrorJson(
							TEXT("reference_operation_result_json_invalid"),
							Request.GetCommandId(), ParseError));
				}
				const TSharedPtr<FJsonObject>* OperationError = nullptr;
				if (!Result.IsValid()
					|| (Root->TryGetObjectField(
							TEXT("operation_error"), OperationError)
						&& OperationError && OperationError->IsValid()))
				{
					return FUeAgentCommandResult::Failure(
						Request, CanonicalObject(Root),
						OperationError && OperationError->IsValid()
							? CanonicalObject(*OperationError)
							: ErrorJson(
								TEXT("reference_operation_result_invalid"),
								Request.GetCommandId(),
								TEXT("reference mechanics returned an invalid result")));
				}
				return FUeAgentCommandResult::Success(
					Request, CanonicalObject(Root));
			}

		private:
			TSharedRef<FUeAgentReferenceCrudApplication> Application;
		};
	}

	const FString& GetCommandId()
	{
		static const FString Id(TEXT("reference.asset_apply"));
		return Id;
	}

	bool TryCreateApplication(
		TSharedPtr<FUeAgentReferenceCrudApplication>& OutApplication,
		FString& OutError)
	{
		OutApplication.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		if (!TryCreateReferenceOperationDescriptorRegistry(
				Descriptors,
				OutError)
			|| !Descriptors.IsValid())
		{
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService> Tickets;
		FUeAgentOperationTicketError TicketError;
		if (!FUeAgentOperationTicketService::TryCreate(
				FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
				300.0,
				[]() { return FPlatformTime::Seconds(); },
				[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
				Tickets,
				TicketError)
			|| !Tickets.IsValid())
		{
			OutError = TicketError.Code + TEXT(":") + TicketError.Detail;
			return false;
		}

		const FString SessionId = TEXT("reference-session:")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		const TSharedRef<FUeAgentReferenceAssetReadHook> ReadHook =
			MakeShared<FUeAgentReferenceAssetReadHook>(SessionId);
		const TSharedRef<FUeAgentGenericReadAdapter> ReadPort =
			MakeShared<FUeAgentGenericReadAdapter>(
				GetReferenceAssetReadPortId(),
				ReadHook);

		FUeAgentReferenceCrudApplicationConfig Config;
		Config.OperationDescriptor = GetReferenceOperationDescriptorIdentity();
		Config.OperationDescriptorRegistry = Descriptors;
		Config.ExecutionContext.ProjectInstanceId = TEXT("project:")
			+ FString(FApp::GetProjectName());
		Config.ExecutionContext.EditorInstanceId = FString::Printf(
			TEXT("editor-process:%u"),
			FPlatformProcess::GetCurrentProcessId());
		Config.ExecutionContext.StatePlane = EUeAgentStatePlane::Persisted;
		Config.ExecutionContext.ContextRevision = SessionId + TEXT(":1:")
			+ Descriptors->GetRegistryHash();
		Config.ExecutionContext.bResolvedFromCurrent = false;
		Config.ServerSessionId = SessionId;
		Config.RegistryEpoch = 1;
		Config.TimeoutSeconds = 120.0;
		Config.MaxFrameBytes = 16 * 1024 * 1024;
		Config.JournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/ReferenceOperation");
		FUeAgentReferenceCrudApplicationError ApplicationError;
		if (!FUeAgentReferenceCrudApplication::TryCreate(
				Config,
				Tickets.ToSharedRef(),
				ReadPort,
				OutApplication,
				ApplicationError)
			|| !OutApplication.IsValid())
		{
			OutError = ApplicationError.Code + TEXT(":")
				+ ApplicationError.Detail;
			return false;
		}
		return true;
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		if (CommandId != GetCommandId())
		{
			OutError = TEXT("reference_command_not_supported:") + CommandId;
			return false;
		}
		TSharedPtr<FUeAgentReferenceCrudApplication> Application;
		if (!TryCreateApplication(Application, OutError)
			|| !Application.IsValid())
		{
			return false;
		}
		OutHandler = MakeShared<FReferenceMechanicsHandler>(
			Application.ToSharedRef());
		OutAuthorityScopes.Add(TEXT("uai:reference:write"));
		return true;
	}
}
