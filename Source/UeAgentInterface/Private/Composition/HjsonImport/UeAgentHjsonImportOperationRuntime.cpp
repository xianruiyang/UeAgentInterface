#include "Composition/HjsonImport/UeAgentHjsonImportOperationRuntime.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::HjsonImport
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Projection;

	namespace
	{
		void SetError(
			FUeAgentHjsonImportTargetError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		void CopyError(
			const FUeAgentOperationBindingError& Source,
			FUeAgentHjsonImportTargetError& OutError)
		{
			SetError(
				OutError,
				Source.Code,
				Source.SubjectId,
				Source.Detail);
		}

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error)
				? Text
				: FString();
		}

		class FSessionSource final : public IUeAgentOperationSessionSource
		{
		public:
			explicit FSessionSource(FUeAgentOperationSessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FUeAgentOperationSessionIdentity GetCurrent() const override
			{
				return Session;
			}

		private:
			FUeAgentOperationSessionIdentity Session;
		};

		class FContextValidator final
			: public IUeAgentExecutionContextValidator
		{
		public:
			explicit FContextValidator(
				FUeAgentHjsonImportOperationRuntimeConfig InConfig)
				: Config(MoveTemp(InConfig))
			{
			}

			bool TryValidate(
				const FUeAgentExecutionContextRef& Context,
				FUeAgentExecutionContextError& OutError) const override
			{
				OutError = {};
				const FString ExpectedRevision =
					Config.ServerSessionId
					+ TEXT(":")
					+ FString::Printf(TEXT("%lld"), Config.RegistryEpoch)
					+ TEXT(":")
					+ Context.GetDescriptorIdentity().GetDescriptorHash();
				if (Context.GetProjectInstanceId() != Config.ProjectInstanceId
					|| Context.GetEditorInstanceId() != Config.EditorInstanceId)
				{
					OutError.Code = TEXT("execution_context_instance_conflict");
					return false;
				}
				if (Context.GetStatePlane() != EUeAgentStatePlane::Persisted
					|| Context.GetContextRevision() != ExpectedRevision
					|| Context.WasResolvedFromCurrent())
				{
					OutError.Code = TEXT("execution_context_revision_conflict");
					return false;
				}
				return true;
			}

		private:
			FUeAgentHjsonImportOperationRuntimeConfig Config;
		};
	}

	class FUeAgentHjsonImportOperationRuntime::FState final
	{
	public:
		FUeAgentHjsonImportOperationRuntimeConfig Config;
		TSharedRef<const FUeAgentDescriptorRegistry> Descriptors;
		FUeAgentOperationSessionIdentity Session;
		TSharedRef<FSessionSource> SessionSource;
		TSharedRef<const FContextValidator> ContextValidator;
		TSharedRef<const FUeAgentOperationTicketService> TicketService;
		TSharedRef<const FUeAgentResponseProjector> Projector;

		FState(
			FUeAgentHjsonImportOperationRuntimeConfig InConfig,
			TSharedRef<const FUeAgentDescriptorRegistry> InDescriptors,
			FUeAgentOperationSessionIdentity InSession,
			TSharedRef<FSessionSource> InSessionSource,
			TSharedRef<const FContextValidator> InContextValidator,
			TSharedRef<const FUeAgentOperationTicketService> InTicketService,
			TSharedRef<const FUeAgentResponseProjector> InProjector)
			: Config(MoveTemp(InConfig))
			, Descriptors(MoveTemp(InDescriptors))
			, Session(MoveTemp(InSession))
			, SessionSource(MoveTemp(InSessionSource))
			, ContextValidator(MoveTemp(InContextValidator))
			, TicketService(MoveTemp(InTicketService))
			, Projector(MoveTemp(InProjector))
		{
		}
	};

	bool FUeAgentHjsonImportOperationRuntimeConfig::IsValid() const
	{
		return !ProjectInstanceId.TrimStartAndEnd().IsEmpty()
			&& !EditorInstanceId.TrimStartAndEnd().IsEmpty()
			&& !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& RegistryEpoch > 0;
	}

	bool FUeAgentHjsonImportC3OResult::IsValid() const
	{
		return ImportReceipt.IsValid()
			&& !OperationId.IsEmpty()
			&& PlanFingerprint.StartsWith(TEXT("sha256:"))
			&& VerificationStatus == TEXT("verified");
	}

	FUeAgentHjsonImportOperationRuntime::FUeAgentHjsonImportOperationRuntime(
		TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	bool FUeAgentHjsonImportOperationRuntime::TryCreate(
		const FUeAgentHjsonImportOperationRuntimeConfig& Config,
		TSharedRef<const FUeAgentOperationTicketService> TicketService,
		TSharedRef<const FUeAgentResponseProjector> Projector,
		TSharedPtr<FUeAgentHjsonImportOperationRuntime>& OutRuntime,
		FUeAgentHjsonImportTargetError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(OutError, TEXT("hjson_import_runtime_config_invalid"));
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		FString DescriptorError;
		if (!TryCreateHjsonImportDescriptorRegistry(
				Descriptors, DescriptorError)
			|| !Descriptors.IsValid())
		{
			SetError(
				OutError,
				TEXT("hjson_import_descriptor_registry_invalid"),
				FString(),
				DescriptorError);
			return false;
		}
		const FUeAgentOperationSessionIdentity Session(
			Config.ServerSessionId,
			Config.RegistryEpoch,
			Descriptors->GetRegistryHash());
		if (!Session.IsValid())
		{
			SetError(OutError, TEXT("hjson_import_session_invalid"));
			return false;
		}
		const TSharedRef<FSessionSource> SessionSource =
			MakeShared<FSessionSource>(Session);
		const TSharedRef<const FContextValidator> ContextValidator =
			MakeShared<FContextValidator>(Config);
		OutRuntime = MakeShareable(
			new FUeAgentHjsonImportOperationRuntime(
				MakeShared<FState, ESPMode::ThreadSafe>(
					Config,
					Descriptors.ToSharedRef(),
					Session,
					SessionSource,
					ContextValidator,
					MoveTemp(TicketService),
					MoveTemp(Projector))));
		return true;
	}

	bool FUeAgentHjsonImportOperationRuntime::TrySubmit(
		FUeAgentHjsonTargetImportRequest Request,
		const FString& RequestId,
		const FUeAgentCallerContext& CallerContext,
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		FUeAgentHjsonImportC3OResult& OutResult,
		FUeAgentHjsonImportTargetError& OutError) const
	{
		OutResult = {};
		OutError = {};
		const FUeAgentDescriptorIdentity DescriptorIdentity =
			GetHjsonImportDescriptorIdentity(Request.GetCompositionKind());
		if (!Request.IsValid()
			|| RequestId.TrimStartAndEnd().IsEmpty()
			|| !CallerContext.IsValid()
			|| !DescriptorIdentity.IsValid()
			|| !State->Descriptors->FindExact(DescriptorIdentity))
		{
			SetError(
				OutError,
				TEXT("hjson_import_operation_request_invalid"),
				RequestId);
			return false;
		}

		const FString CompositionKind = Request.GetCompositionKind();
		const FString Target = Request.GetTargetAssetPath();
		const FString WorkspaceFingerprint =
			Request.GetWorkspace()->GetWorkspaceFingerprint();
		const FString ExpectedRevision = Request.GetExpectedBaseRevision();
		const FString Intent = PlanningPreflight::LexToString(
			Request.GetIntent());
		const bool bDryRun = Request.IsDryRun();
		TSharedPtr<IUeAgentHjsonImportOperation> Stages;
		if (!GetHjsonImportTargetRegistry().TryCreateOperationExact(
				MoveTemp(Request), Stages, OutError)
			|| !Stages.IsValid())
		{
			return false;
		}

		TSharedPtr<const FUeAgentOperationBinding> Binding;
		FUeAgentOperationBindingError BindingError;
		const TSharedRef<IUeAgentHjsonImportOperation> StageRef =
			Stages.ToSharedRef();
		if (!FUeAgentOperationBinding::TryCreate(
				State->Descriptors,
				DescriptorIdentity,
				State->ContextValidator,
				StaticCastSharedRef<IUeAgentOperationPlanner>(StageRef),
				StaticCastSharedRef<IUeAgentOperationExecutor>(StageRef),
				StaticCastSharedRef<IUeAgentOperationVerifier>(StageRef),
				State->Projector,
				State->SessionSource,
				State->TicketService,
				Binding,
				BindingError)
			|| !Binding.IsValid())
		{
			CopyError(BindingError, OutError);
			return false;
		}

		const TSharedRef<FJsonObject> InputObject = MakeShared<FJsonObject>();
		InputObject->SetStringField(TEXT("composition_kind"), CompositionKind);
		InputObject->SetStringField(TEXT("target"), Target);
		InputObject->SetStringField(
			TEXT("workspace_fingerprint"), WorkspaceFingerprint);
		InputObject->SetStringField(
			TEXT("expected_revision"), ExpectedRevision);
		InputObject->SetStringField(TEXT("intent"), Intent);
		InputObject->SetBoolField(TEXT("dry_run"), bDryRun);
		const FUeAgentContractValue Input(JsonText(InputObject));
		FUeAgentExecutionContextInput ExecutionInput;
		ExecutionInput.ProjectInstanceId = State->Config.ProjectInstanceId;
		ExecutionInput.EditorInstanceId = State->Config.EditorInstanceId;
		ExecutionInput.StatePlane = EUeAgentStatePlane::Persisted;
		ExecutionInput.WorldType = EUeAgentExecutionWorldType::Editor;
		ExecutionInput.ContextRevision =
			State->Config.ServerSessionId
			+ TEXT(":")
			+ FString::Printf(TEXT("%lld"), State->Config.RegistryEpoch)
			+ TEXT(":")
			+ DescriptorIdentity.GetDescriptorHash();
		ExecutionInput.bResolvedFromCurrent = false;
		const FUeAgentProjectionRequest ProjectionRequest(
			FUeAgentProjectionProfile(TEXT("compact"), false, false, false),
			FUeAgentPageRequest(),
			{},
			{},
			{});
		const FUeAgentOperationBindingRequest TypedRequest(
			DescriptorIdentity,
			RequestId,
			TEXT("hjson-import:") + RequestId,
			FUeAgentOperationBindingPayload(
				State->Session,
				{},
				MoveTemp(ExecutionInput),
				Input,
				StaticCastSharedRef<IUeAgentOperationRequestPreparer>(StageRef),
				ProjectionRequest));
		TSharedPtr<const FUeAgentOperationBindingResult> BindingResult;
		if (!Binding->TrySubmit(
				TypedRequest,
				CallerContext,
				MoveTemp(Resources),
				Deadline,
				Cancellation,
				BindingResult,
				BindingError)
			|| !BindingResult.IsValid()
			|| !BindingResult->IsValid()
			|| !Stages->GetImportReceipt().IsValid()
			|| !Stages->UsedOneOperationContext())
		{
			if (BindingError.IsValid())
			{
				CopyError(BindingError, OutError);
			}
			else
			{
				SetError(
					OutError,
					TEXT("hjson_import_operation_incomplete"),
					RequestId,
					TEXT(
						"C3O did not complete prepare, plan, execute and "
						"verify on one OperationContext."));
			}
			return false;
		}

		OutResult.ImportReceipt = Stages->GetImportReceipt();
		OutResult.OperationId =
			BindingResult->GetValue().GetOperationId();
		OutResult.PlanFingerprint =
			BindingResult->GetValue().GetPlanFingerprint();
		OutResult.VerificationStatus = TEXT("verified");
		return OutResult.IsValid();
	}

	const TSharedRef<const FUeAgentDescriptorRegistry>&
	FUeAgentHjsonImportOperationRuntime::GetDescriptorRegistry() const
	{
		return State->Descriptors;
	}
}
