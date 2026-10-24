#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"

#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::OperationOwners::ExternalImport::Localization
{
	using namespace BindingGateway::Operation;
	using namespace CommandContract;
	using namespace DomainAdapters::ExternalImport::Localization;

	namespace
	{
		Contract::FDiagnostic CommandGuardFailure(
			FString Code, FString PublicReason, const FString& PrivateSubject,
			const TCHAR* Condition, const bool bActual, const bool bExpected)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(PublicReason),
				TEXT("command_execute"), FString(), PrivateSubject);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Condition;
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(bActual);
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(bExpected);
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}
        bool Fail(FUeAgentLocalizationImportCommandError& OutError, const UE::UAI::Contract::FDiagnostic& Diagnostic,
            FString TerminalStatus = TEXT("rejected"), bool bMutationStarted = false, bool bRetrySafe = true)
        {
            OutError = Diagnostic;
            OutError.TerminalStatus = MoveTemp(TerminalStatus);
            OutError.MutationStarted = bMutationStarted;
            OutError.bRetrySafe = bRetrySafe;
            return false;
        }


		bool TryBuildSpec(
			const FUeAgentLocalizationImportCommandRequest& Request,
			FLocalizationImportSpec& OutSpec,
			FLocalizationImportError& OutError)
		{
			TSharedPtr<FJsonObject> Params;
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError)
				|| !Params.IsValid())
			{
				OutError = MoveTemp(JsonError);
				return false;
			}
			OutSpec.RequestId = Request.RequestId;
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Params,
				{{TEXT("source_ref"), EJson::String}, {TEXT("target_ref"), EJson::String},
				 {TEXT("options"), EJson::Object}}, OutError))
			{
				
				return false;
			}
			const TSharedPtr<FJsonObject>* Options = nullptr;
			if (Params->TryGetObjectField(TEXT("options"), Options) && Options &&
				!UeAgentAuthoringJsonFields::ValidateDeclaredFields(**Options, {}, OutError, TEXT("/options")))
			{
				
				return false;
			}
			if (!Params->TryGetStringField(TEXT("source_ref"), OutSpec.SourceRef) || OutSpec.SourceRef.IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("localization_import_source_required"), TEXT("/source_ref"),
					MakeShared<FJsonValueString>(TEXT("non-empty source path")), Params->TryGetField(TEXT("source_ref")));
				return false;
			}
			if (!Params->TryGetStringField(TEXT("target_ref"), OutSpec.TargetRef) || OutSpec.TargetRef.IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("localization_import_target_required"), TEXT("/target_ref"),
					MakeShared<FJsonValueString>(TEXT("non-empty localization target")), Params->TryGetField(TEXT("target_ref")));
				return false;
			}
			if (OutSpec.RequestId.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("request_id_missing"),
					TEXT("The request requires a non-empty request identifier."), TEXT("input_decode"));
				OutError.SourceKind = TEXT("native_request");
				OutError.Source.JsonPath = TEXT("/request_id");
				OutError.PropertyPath = TEXT("/request_id");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(OutSpec.RequestId);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
				return false;
			}
			return true;
		}

		struct FExecutionContext final
		{
			explicit FExecutionContext(
				TSharedRef<ILocalizationImportSession> InSession)
				: Session(MoveTemp(InSession))
			{
			}

			TSharedRef<ILocalizationImportSession> Session;
			FLocalizationImportError LastError;
			FLocalizationImportReadback Readback;
		};

		void SetStageFailure(
			const FLocalizationImportError& DomainError,
			const EOperationLifecycleStage Stage, const FString& Target,
			FOperationLifecycleError& OutError)
		{
			OutError = DomainError;
			if (OutError.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("localization_import_operation_failed"),
					FString(TEXT("Localization import failed during ")) + LexToString(Stage) + TEXT(" without providing a diagnostic."),
					LexToString(Stage));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("stage_requirement_satisfied");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target;
		}

		class FVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FVersionFence(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			virtual bool Check(
				const FOperationLifecyclePlan& Plan,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				bOutCurrent = Stage == EOperationLifecycleStage::PreExecuteFence
					? Context->Session->CheckPreExecute(Context->LastError)
					: Context->Session->CheckPrePersist(Context->LastError);
				if (!bOutCurrent)
				{
					SetStageFailure(Context->LastError, Stage, Context->Session->GetTargetIdentity(), OutError);
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FMutationPort final : public IOperationDomainMutationPort
		{
		public:
			explicit FMutationPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			virtual bool Execute(
				const FOperationLifecyclePlan& Plan,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				const bool bSucceeded =
					Context->Session->Execute(Context->LastError);
				if (Context->Session->DidMutationStart())
				{
					OutFacts = FOperationMutationFacts(
						TEXT("localization-import:") + Plan.GetPlanId(),
						{Context->Session->GetTargetIdentity()},
						true);
				}
				if (!bSucceeded)
				{
					SetStageFailure(Context->LastError, EOperationLifecycleStage::DomainExecute, Context->Session->GetTargetIdentity(), OutError);
				}
				return bSucceeded;
			}

			virtual bool Rollback(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				const bool bSucceeded = Context->Session->Rollback(
					bOutConfirmed,
					Context->LastError);
				if (!bSucceeded || !bOutConfirmed)
				{
					SetStageFailure(Context->LastError, EOperationLifecycleStage::Rollback, Context->Session->GetTargetIdentity(), OutError);
				}
				return bSucceeded;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FStabilityPort final : public IOperationStabilityPort
		{
		public:
			explicit FStabilityPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			virtual bool Settle(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				const bool bSucceeded =
					Context->Session->Settle(Context->LastError);
				if (!bSucceeded)
				{
					SetStageFailure(Context->LastError, EOperationLifecycleStage::Settle, Context->Session->GetTargetIdentity(), OutError);
				}
				return bSucceeded;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FPersistencePort final : public IOperationPersistencePort
		{
		public:
			explicit FPersistencePort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			virtual bool Persist(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FString& OutPersistenceReceiptId,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				const bool bSucceeded = Context->Session->Persist(
					OutPersistenceReceiptId,
					Context->LastError);
				if (!bSucceeded)
				{
					SetStageFailure(Context->LastError, EOperationLifecycleStage::Persist, Context->Session->GetTargetIdentity(), OutError);
				}
				return bSucceeded;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FIndependentVerificationPort final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FIndependentVerificationPort(
				TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			virtual bool Verify(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				Context->LastError = {};
				bOutEquivalent = Context->Session->Verify(
					Context->Readback,
					Context->LastError);
				if (!bOutEquivalent)
				{
					SetStageFailure(Context->LastError, EOperationLifecycleStage::IndependentVerify, Context->Session->GetTargetIdentity(), OutError);
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		TSharedPtr<FJsonObject> ReadbackJson(
			const FLocalizationImportReadback& Readback)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target_ref"), Readback.TargetRef);
			Result->SetStringField(TEXT("culture"), Readback.Culture);
			Result->SetStringField(TEXT("source_ref"), Readback.SourceRef);
			Result->SetStringField(TEXT("collapse_mode"), Readback.CollapseMode);
			Result->SetStringField(TEXT("po_format"), Readback.PoFormat);
			Result->SetBoolField(TEXT("target_resolved"), Readback.bTargetResolved);
			Result->SetBoolField(TEXT("formal_importer_completed"), Readback.bFormalImporterCompleted);
			Result->SetBoolField(TEXT("formal_compiler_completed"), Readback.bFormalCompilerCompleted);
			Result->SetBoolField(TEXT("refresh_completed"), Readback.bRefreshCompleted);
			return Result;
		}

		class FService final : public IUeAgentLocalizationImportCommandService
		{
		public:
			explicit FService(TSharedRef<ILocalizationImportProvider> InProvider)
				: Provider(MoveTemp(InProvider))
			{
			}

			virtual bool Execute(
				const FUeAgentLocalizationImportCommandRequest& Request,
				FUeAgentLocalizationImportCommandResponse& OutResponse,
				FUeAgentLocalizationImportCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (!IsInGameThread())
				{
					return Fail(OutError, CommandGuardFailure(TEXT("localization_import_game_thread_required"),
						TEXT("Localization import must execute on the game thread."), Request.CommandId, TEXT("game_thread"), false, true));
				}
				if (!Request.AuthorityScopes.Contains(TEXT("uai:operate")))
				{
					return Fail(OutError, CommandGuardFailure(TEXT("localization_import_authority_required"),
						TEXT("Localization import requires the uai:operate authority scope."), Request.CommandId, TEXT("operate_authority"), false, true));
				}
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, CommandGuardFailure(TEXT("localization_import_cancelled"),
						TEXT("Localization import was cancelled before preparation."), Request.RequestId, TEXT("cancellation_requested"), true, false));
				}
				if (Request.CommandId != TEXT("external.localization.po.import"))
				{
					return Fail(OutError, CommandGuardFailure(TEXT("localization_import_command_not_supported"),
						TEXT("This service accepts only external.localization.po.import."), Request.CommandId, TEXT("command_supported"), false, true));
				}

				FLocalizationImportSpec Spec;
				FLocalizationImportError DomainError;
				if (!TryBuildSpec(Request, Spec, DomainError))
				{
					return Fail(OutError, DomainError);
				}
				TSharedPtr<ILocalizationImportSession> Session;
				if (!Provider->Prepare(Spec, Session, DomainError)
					|| !Session.IsValid())
				{
					if (DomainError.IsEmpty())
					{
						DomainError = CommandGuardFailure(TEXT("localization_import_session_unavailable"),
							TEXT("Localization import preparation did not produce a session or a diagnostic."),
							Request.RequestId, TEXT("session_ready"), false, true);
						DomainError.Stage = TEXT("prepare");
						DomainError.TargetPath = Spec.TargetRef;
					}
					return Fail(OutError, DomainError);
				}

				const TSharedRef<FExecutionContext> Context =
					MakeShared<FExecutionContext>(Session.ToSharedRef());
				const FString PlanFingerprint = Json::Sha256Utf8(
					Request.CommandId + TEXT("|")
					+ Request.CanonicalParamsJson + TEXT("|")
					+ Request.RequestId);
				const FString Target = Session->GetTargetIdentity();
				const FOperationLifecyclePlan Plan(
					TEXT("t30:") + Request.RequestId,
					PlanFingerprint,
					{Target},
					{{Target, Session->GetExpectedRevision()}},
					{TEXT("localization_po_imported"),
						TEXT("localization_resources_compiled"),
						TEXT("localization_resources_refreshed")});
				const FOperationImpactAuthorization Authorization(
					PlanFingerprint,
					{Target});
				const FOperationLifecycleService Lifecycle(
					MakeShared<FVersionFence>(Context),
					MakeShared<FMutationPort>(Context),
					MakeShared<FStabilityPort>(Context),
					MakeShared<FPersistencePort>(Context),
					MakeShared<FIndependentVerificationPort>(Context));
				FOperationLifecycleReceipt Receipt;
				if (!Lifecycle.Run(Plan, Authorization, Receipt))
				{
					return Fail(OutError, Receipt.GetDiagnostic(), LexToString(Receipt.GetStatus()), Receipt.DidMutationStart(), Receipt.IsRetrySafe());
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(
					TEXT("terminal_status"),
					LexToString(Receipt.GetStatus()));
				const TSharedPtr<FJsonObject> Readback = ReadbackJson(Context->Readback);
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Readback->Values)
				{
					Payload->SetField(Field.Key, Field.Value);
				}
				FString CanonicalPayload;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryCanonicalize(
						Payload,
						CanonicalPayload,
						JsonError))
				{
					return Fail(OutError, JsonError, TEXT("state_unknown"), true, false);
				}
				OutResponse.PayloadJson = MoveTemp(CanonicalPayload);
				return true;
			}

		private:
			TSharedRef<ILocalizationImportProvider> Provider;
		};
	}

	bool FUeAgentLocalizationImportCommandRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentLocalizationImportCommandRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId != TEXT("external.localization.po.import"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("external_import_route_invalid"),
				TEXT("The route field does not match the required command binding."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("external.localization.po.import"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonFunction != TEXT("localization_po_import"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("external_import_route_invalid"),
				TEXT("The route field does not match the required command binding."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_function");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("localization_po_import"));
			OutError.ActualValue = MakeShared<FJsonValueString>(PythonFunction);
			return false;
		}
		return true;
	}

	const TArray<FUeAgentLocalizationImportCommandRoute>&
	GetLocalizationImportCommandRoutes()
	{
		static const TArray<FUeAgentLocalizationImportCommandRoute> Routes = {
			{TEXT("external.localization.po.import"),
				TEXT("localization_po_import")},
		};
		return Routes;
	}

	bool TryCreateProductionLocalizationImportCommandService(
		TSharedPtr<IUeAgentLocalizationImportCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService.Reset();
		TSharedPtr<ILocalizationImportProvider> Provider;
		if (!TryCreateLocalizationImportProvider(Provider, OutError)
			|| !Provider.IsValid())
		{
			return false;
		}
		OutService = MakeShared<FService>(Provider.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
