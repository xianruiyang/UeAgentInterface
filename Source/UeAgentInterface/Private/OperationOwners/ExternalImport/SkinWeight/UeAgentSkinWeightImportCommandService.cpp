#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"

#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::OperationOwners::ExternalImport::SkinWeight
{
	using namespace BindingGateway::Operation;
	using namespace CommandContract;
	using namespace DomainAdapters::ExternalImport::SkinWeight;

	namespace
	{
		bool Fail(
			FUeAgentSkinWeightImportCommandError& OutError,
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
			const FUeAgentSkinWeightImportCommandRequest& Request,
			FSkinWeightImportSpec& OutSpec,
			FSkinWeightImportError& OutError)
		{
			TSharedPtr<FJsonObject> Params;
			FString JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError)
				|| !Params.IsValid())
			{
				OutError.Code = TEXT("skin_weight_import_params_invalid");
				OutError.Detail = MoveTemp(JsonError);
				return false;
			}
			OutSpec.RequestId = Request.RequestId;
			if (!Params->TryGetStringField(TEXT("source_ref"), OutSpec.SourceRef))
			{
				OutError.Code = TEXT("skin_weight_import_source_required");
				return false;
			}
			if (!Params->TryGetStringField(TEXT("target_ref"), OutSpec.TargetRef))
			{
				OutError.Code = TEXT("skin_weight_import_target_required");
				return false;
			}
			if (!Params->TryGetStringField(
					TEXT("profile_name"),
					OutSpec.ProfileName))
			{
				OutError.Code = TEXT("skin_weight_import_profile_required");
				return false;
			}
			double LodIndex = -1.0;
			if (!Params->TryGetNumberField(TEXT("lod_index"), LodIndex)
				|| LodIndex < 0.0
				|| LodIndex > static_cast<double>(MAX_int32)
				|| LodIndex != static_cast<double>(static_cast<int32>(LodIndex)))
			{
				OutError.Code = TEXT("skin_weight_import_lod_invalid");
				return false;
			}
			OutSpec.LodIndex = static_cast<int32>(LodIndex);
			if (!OutSpec.IsValid())
			{
				OutError.Code = TEXT("skin_weight_import_spec_invalid");
				return false;
			}
			return true;
		}

		struct FExecutionContext final
		{
			explicit FExecutionContext(
				TSharedRef<ISkinWeightImportSession> InSession)
				: Session(MoveTemp(InSession))
			{
			}

			TSharedRef<ISkinWeightImportSession> Session;
			FSkinWeightImportError LastError;
			FSkinWeightImportReadback Readback;
		};

		void ConvertError(
			const FSkinWeightImportError& DomainError,
			FOperationLifecycleError& OutError)
		{
			OutError = FOperationLifecycleError(
				DomainError.Code.IsEmpty()
					? FString(TEXT("skin_weight_import_operation_failed"))
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
						TEXT("skin-weight-import:") + Plan.GetPlanId(),
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
			const FSkinWeightImportReadback& Readback)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target_identity"), Readback.TargetIdentity);
			Result->SetStringField(TEXT("asset_class"), Readback.AssetClass);
			Result->SetStringField(TEXT("profile_name"), Readback.ProfileName);
			Result->SetStringField(TEXT("source_ref"), Readback.SourceRef);
			Result->SetStringField(TEXT("source_sha256"), Readback.SourceSha256);
			Result->SetNumberField(TEXT("lod_index"), Readback.LodIndex);
			Result->SetNumberField(TEXT("profile_count"), Readback.ProfileCount);
			Result->SetBoolField(TEXT("reloaded"), Readback.bReloaded);
			return Result;
		}

		class FService final : public IUeAgentSkinWeightImportCommandService
		{
		public:
			explicit FService(TSharedRef<ISkinWeightImportProvider> InProvider)
				: Provider(MoveTemp(InProvider))
			{
			}

			virtual bool Execute(
				const FUeAgentSkinWeightImportCommandRequest& Request,
				FUeAgentSkinWeightImportCommandResponse& OutResponse,
				FUeAgentSkinWeightImportCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (!IsInGameThread())
				{
					return Fail(
						OutError,
						TEXT("skin_weight_import_game_thread_required"),
						Request.CommandId);
				}
				if (!Request.AuthorityScopes.Contains(TEXT("uai:operate")))
				{
					return Fail(
						OutError,
						TEXT("skin_weight_import_authority_required"),
						Request.CommandId);
				}
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("skin_weight_import_cancelled"),
						Request.RequestId);
				}
				if (Request.CommandId
					!= TEXT("external.skin_weight_profile.import"))
				{
					return Fail(
						OutError,
						TEXT("skin_weight_import_command_not_supported"),
						Request.CommandId);
				}

				FSkinWeightImportSpec Spec;
				FSkinWeightImportError DomainError;
				if (!TryBuildSpec(Request, Spec, DomainError))
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("skin_weight_import_params_invalid"))
							: DomainError.Code,
						DomainError.SubjectId,
						DomainError.Detail);
				}
				TSharedPtr<ISkinWeightImportSession> Session;
				if (!Provider->Prepare(Spec, Session, DomainError)
					|| !Session.IsValid())
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("skin_weight_import_prepare_failed"))
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
					TEXT("t29:") + Request.RequestId,
					PlanFingerprint,
					{Target},
					{{Target, Session->GetExpectedRevision()}},
					{TEXT("skin_weight_profile_registered"),
						TEXT("skin_weight_source_metadata_persisted")});
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
							? FString(TEXT("skin_weight_import_operation_failed"))
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
						TEXT("skin_weight_import_result_invalid"),
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
			TSharedRef<ISkinWeightImportProvider> Provider;
		};
	}

	bool FUeAgentSkinWeightImportCommandRoute::IsValid() const
	{
		return CommandId == TEXT("external.skin_weight_profile.import")
			&& PythonFunction == TEXT("skin_weight_profile_import");
	}

	const TArray<FUeAgentSkinWeightImportCommandRoute>&
	GetSkinWeightImportCommandRoutes()
	{
		static const TArray<FUeAgentSkinWeightImportCommandRoute> Routes = {
			{TEXT("external.skin_weight_profile.import"),
				TEXT("skin_weight_profile_import")},
		};
		return Routes;
	}

	bool TryCreateProductionSkinWeightImportCommandService(
		TSharedPtr<IUeAgentSkinWeightImportCommandService>& OutService,
		FString& OutError)
	{
		OutService.Reset();
		TSharedPtr<ISkinWeightImportProvider> Provider;
		if (!TryCreateSkinWeightImportProvider(Provider, OutError)
			|| !Provider.IsValid())
		{
			return false;
		}
		OutService = MakeShared<FService>(Provider.ToSharedRef());
		OutError.Reset();
		return true;
	}
}
