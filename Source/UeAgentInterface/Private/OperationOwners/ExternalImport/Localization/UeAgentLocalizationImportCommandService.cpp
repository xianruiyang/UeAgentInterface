#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"

#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::OperationOwners::ExternalImport::Localization
{
	using namespace BindingGateway::Operation;
	using namespace CommandContract;
	using namespace DomainAdapters::ExternalImport::Localization;

	namespace
	{
		bool Fail(
			FUeAgentLocalizationImportCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString(),
			FString TerminalStatus = TEXT("rejected"),
			const bool bMutationStarted = false,
			const bool bRetrySafe = true)
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			OutError.TerminalStatus = MoveTemp(TerminalStatus);
			OutError.bMutationStarted = bMutationStarted;
			OutError.bRetrySafe = bRetrySafe;
			return false;
		}

		bool TryBuildSpec(
			const FUeAgentLocalizationImportCommandRequest& Request,
			FLocalizationImportSpec& OutSpec,
			FLocalizationImportError& OutError)
		{
			TSharedPtr<FJsonObject> Params;
			FString JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError)
				|| !Params.IsValid())
			{
				OutError.Code = TEXT("localization_import_params_invalid");
				OutError.Detail = MoveTemp(JsonError);
				return false;
			}
			OutSpec.RequestId = Request.RequestId;
			if (!Params->TryGetStringField(TEXT("source_ref"), OutSpec.SourceRef))
			{
				OutError.Code = TEXT("localization_import_source_required");
				return false;
			}
			if (!Params->TryGetStringField(TEXT("target_ref"), OutSpec.TargetRef))
			{
				OutError.Code = TEXT("localization_import_target_required");
				return false;
			}
			if (!OutSpec.IsValid())
			{
				OutError.Code = TEXT("localization_import_spec_invalid");
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

		void ConvertError(
			const FLocalizationImportError& DomainError,
			FOperationLifecycleError& OutError)
		{
			OutError = FOperationLifecycleError(
				DomainError.Code.IsEmpty()
					? FString(TEXT("localization_import_operation_failed"))
					: DomainError.Code,
				DomainError.Detail.IsEmpty()
					? DomainError.SubjectId
					: DomainError.Detail);
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
					ConvertError(Context->LastError, OutError);
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
					ConvertError(Context->LastError, OutError);
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
					ConvertError(Context->LastError, OutError);
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
					ConvertError(Context->LastError, OutError);
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
					ConvertError(Context->LastError, OutError);
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
					ConvertError(Context->LastError, OutError);
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
			Result->SetStringField(TEXT("target_identity"), Readback.TargetIdentity);
			Result->SetStringField(TEXT("culture"), Readback.Culture);
			Result->SetStringField(TEXT("source_ref"), Readback.SourceRef);
			Result->SetStringField(TEXT("source_sha256"), Readback.SourceSha256);
			Result->SetStringField(TEXT("persisted_po_path"), Readback.PersistedPoPath);
			Result->SetStringField(TEXT("persisted_po_sha256"), Readback.PersistedPoSha256);
			Result->SetStringField(TEXT("archive_path"), Readback.ArchivePath);
			Result->SetStringField(TEXT("archive_sha256"), Readback.ArchiveSha256);
			Result->SetStringField(TEXT("locres_path"), Readback.LocResPath);
			Result->SetStringField(TEXT("locres_sha256"), Readback.LocResSha256);
			Result->SetStringField(TEXT("collapse_mode"), Readback.CollapseMode);
			Result->SetStringField(TEXT("po_format"), Readback.PoFormat);
			Result->SetNumberField(TEXT("refresh_revision_before"), Readback.RefreshRevisionBefore);
			Result->SetNumberField(TEXT("refresh_revision_after"), Readback.RefreshRevisionAfter);
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
					return Fail(
						OutError,
						TEXT("localization_import_game_thread_required"),
						Request.CommandId);
				}
				if (!Request.AuthorityScopes.Contains(TEXT("uai:operate")))
				{
					return Fail(
						OutError,
						TEXT("localization_import_authority_required"),
						Request.CommandId);
				}
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("localization_import_cancelled"),
						Request.RequestId);
				}
				if (Request.CommandId != TEXT("external.localization.po.import"))
				{
					return Fail(
						OutError,
						TEXT("localization_import_command_not_supported"),
						Request.CommandId);
				}

				FLocalizationImportSpec Spec;
				FLocalizationImportError DomainError;
				if (!TryBuildSpec(Request, Spec, DomainError))
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("localization_import_params_invalid"))
							: DomainError.Code,
						DomainError.SubjectId,
						DomainError.Detail);
				}
				TSharedPtr<ILocalizationImportSession> Session;
				if (!Provider->Prepare(Spec, Session, DomainError)
					|| !Session.IsValid())
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("localization_import_prepare_failed"))
							: DomainError.Code,
						DomainError.SubjectId,
						DomainError.Detail);
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
					return Fail(
						OutError,
						Receipt.GetErrorCode().IsEmpty()
							? FString(TEXT("localization_import_operation_failed"))
							: Receipt.GetErrorCode(),
						Target,
						Context->LastError.Detail,
						LexToString(Receipt.GetStatus()),
						Receipt.DidMutationStart(),
						Receipt.IsRetrySafe());
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(
					TEXT("terminal_status"),
					LexToString(Receipt.GetStatus()));
				Payload->SetBoolField(
					TEXT("mutation_started"),
					Receipt.DidMutationStart());
				Payload->SetStringField(
					TEXT("persistence_receipt_id"),
					Receipt.GetPersistenceReceiptId());
				Payload->SetStringField(
					TEXT("plan_fingerprint"),
					Plan.GetPlanFingerprint());
				TArray<TSharedPtr<FJsonValue>> Phases;
				for (const FString& Phase : Receipt.GetPhaseHistory())
				{
					Phases.Add(MakeShared<FJsonValueString>(Phase));
				}
				Payload->SetArrayField(TEXT("phase_history"), MoveTemp(Phases));
				Payload->SetObjectField(
					TEXT("independent_readback"),
					ReadbackJson(Context->Readback));
				FString CanonicalPayload;
				FString JsonError;
				if (!Json::TryCanonicalize(
						Payload,
						CanonicalPayload,
						JsonError))
				{
					return Fail(
						OutError,
						TEXT("localization_import_result_invalid"),
						Target,
						JsonError,
						TEXT("state_unknown"),
						true,
						false);
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
		return CommandId == TEXT("external.localization.po.import")
			&& PythonFunction == TEXT("localization_po_import");
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
		FString& OutError)
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
