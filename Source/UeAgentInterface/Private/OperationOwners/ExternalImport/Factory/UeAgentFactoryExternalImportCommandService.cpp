#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"

#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UE::UAI::OperationOwners::ExternalImport::Factory
{
	using namespace BindingGateway::Operation;
	using namespace CommandContract;
	using namespace DomainAdapters::ExternalImport::Factory;

	namespace
	{
		FString GFailureStage;

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
        bool Fail(FUeAgentFactoryExternalImportCommandError& OutError, const UE::UAI::Contract::FDiagnostic& Diagnostic,
            FString TerminalStatus = TEXT("rejected"), bool bMutationStarted = false, bool bRetrySafe = true)
        {
            OutError = Diagnostic;
            OutError.TerminalStatus = MoveTemp(TerminalStatus);
            OutError.MutationStarted = bMutationStarted;
            OutError.bRetrySafe = bRetrySafe;
            return false;
        }


		const FUeAgentFactoryExternalImportCommandRoute* FindRoute(
			const FString& CommandId)
		{
			return GetFactoryExternalImportCommandRoutes().FindByPredicate(
				[&CommandId](
					const FUeAgentFactoryExternalImportCommandRoute& Route)
				{
					return Route.CommandId == CommandId;
				});
		}

		bool InvalidOption(const TSharedRef<FJsonObject>& Options, const TCHAR* Name,
			const FString& Expected, FFactoryExternalImportError& Error, int32 Index = INDEX_NONE)
		{
			
			FString Path = TEXT("/options/") + FString(Name).Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			auto Actual = Options->TryGetField(Name);
			if (Index != INDEX_NONE) { Path += TEXT("/") + ::LexToString(Index); Actual = Actual->AsArray()[Index]; }
			Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_option_type_invalid"), Path,
				MakeShared<FJsonValueString>(Expected), Actual);
			Error.SubjectId = Name;
			return false;
		}

		bool TryReadBool(
			const TSharedRef<FJsonObject>& Options,
			const TCHAR* Name,
			bool& OutValue,
			FFactoryExternalImportError& OutError)
		{
			if (!Options->HasField(Name))
			{
				return true;
			}
			if (!Options->TryGetBoolField(Name, OutValue))
			{
				return InvalidOption(Options, Name, TEXT("boolean"), OutError);
			}
			return true;
		}

		bool TryReadString(
			const TSharedRef<FJsonObject>& Options,
			const TCHAR* Name,
			FString& OutValue,
			FFactoryExternalImportError& OutError)
		{
			if (!Options->HasField(Name))
			{
				return true;
			}
			const auto Value = Options->TryGetField(Name);
			if (!Value || Value->Type != EJson::String || !Value->TryGetString(OutValue))
			{
				return InvalidOption(Options, Name, TEXT("string"), OutError);
			}
			OutValue.TrimStartAndEndInline();
			return true;
		}

		bool TryReadStringArray(
			const TSharedRef<FJsonObject>& Options,
			const TCHAR* Name,
			TArray<FString>& OutValues,
			FFactoryExternalImportError& OutError)
		{
			if (!Options->HasField(Name))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Options->TryGetArrayField(Name, Values) || !Values)
			{
				return InvalidOption(Options, Name, TEXT("array of strings"), OutError);
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Text))
				{
					return InvalidOption(Options, Name, TEXT("string"), OutError, Index);
				}
				Text.TrimStartAndEndInline();
				if (!Text.IsEmpty())
				{
					OutValues.AddUnique(MoveTemp(Text));
				}
			}
			OutValues.Sort();
			return true;
		}

		bool TryBuildSpec(
			const FUeAgentFactoryExternalImportCommandRequest& Request,
			const FUeAgentFactoryExternalImportCommandRoute& Route,
			FFactoryExternalImportSpec& OutSpec,
			FFactoryExternalImportError& OutError)
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
			OutSpec.Operation = Route.Operation;
			OutSpec.Kind = Route.Kind;
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Params,
				{{TEXT("source_ref"), EJson::String}, {TEXT("destination_ref"), EJson::String},
				 {TEXT("target_ref"), EJson::String}, {TEXT("skeleton_ref"), EJson::String}}, OutError))
			{
				
				return false;
			}
			if (!Params->TryGetStringField(TEXT("source_ref"), OutSpec.SourceRef) || OutSpec.SourceRef.IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_source_required"), TEXT("/source_ref"),
					MakeShared<FJsonValueString>(TEXT("non-empty source path")), Params->TryGetField(TEXT("source_ref")));
				return false;
			}
			if (Route.Operation == EFactoryExternalImportOperation::Import)
			{
				if (!Params->TryGetStringField(
						TEXT("destination_ref"),
						OutSpec.DestinationRef) || OutSpec.DestinationRef.IsEmpty())
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_destination_required"), TEXT("/destination_ref"),
						MakeShared<FJsonValueString>(TEXT("non-empty destination asset path")), Params->TryGetField(TEXT("destination_ref")));
					return false;
				}
				if (Route.Kind == EFactoryExternalImportKind::FbxAnimation
					&& (!Params->TryGetStringField(
						TEXT("skeleton_ref"),
						OutSpec.SkeletonRef) || OutSpec.SkeletonRef.IsEmpty()))
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_skeleton_required"), TEXT("/skeleton_ref"),
						MakeShared<FJsonValueString>(TEXT("non-empty skeleton asset path")), Params->TryGetField(TEXT("skeleton_ref")));
					return false;
				}
			}
			else if (!Params->TryGetStringField(
					TEXT("target_ref"),
					OutSpec.DestinationRef) || OutSpec.DestinationRef.IsEmpty())
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_reimport_target_required"), TEXT("/target_ref"),
					MakeShared<FJsonValueString>(TEXT("non-empty target asset path")), Params->TryGetField(TEXT("target_ref")));
				return false;
			}

			TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
			if (Params->HasField(TEXT("options")))
			{
				if (!Params->TryGetObjectField(TEXT("options"), OptionsPtr)
					|| !OptionsPtr
					|| !OptionsPtr->IsValid())
				{
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_options_invalid"), TEXT("/options"),
						MakeShared<FJsonValueString>(TEXT("object")), Params->TryGetField(TEXT("options")));
					return false;
				}
				Options = OptionsPtr->ToSharedRef();
			}
			if (!UeAgentAuthoringJsonFields::ValidateDeclaredFields(*Options,
				{{TEXT("replace_existing"), EJson::Boolean}, {TEXT("import_materials"), EJson::Boolean},
				 {TEXT("import_textures"), EJson::Boolean}, {TEXT("create_physics_asset"), EJson::Boolean},
				 {TEXT("import_animations"), EJson::Boolean}, {TEXT("import_morph_targets"), EJson::Boolean},
				 {TEXT("update_skeleton_reference_pose"), EJson::Boolean}, {TEXT("validate_after_import"), EJson::Boolean},
				 {TEXT("skeleton_ref"), EJson::String}, {TEXT("srgb"), EJson::Boolean},
				 {TEXT("compression_settings"), EJson::String}, {TEXT("mip_gen_settings"), EJson::String},
				 {TEXT("lod_group"), EJson::String}, {TEXT("expected_morph_targets"), EJson::Array},
				 {TEXT("expected_material_slots"), EJson::Array}, {TEXT("expected_lod_count"), EJson::Number}},
				OutError, TEXT("/options")))
			{
				OutError.SubjectId = OutError.PropertyPath;
				return false;
			}
			OutSpec.Options.bReplaceExisting =
				Route.Operation == EFactoryExternalImportOperation::Reimport;
			if (!TryReadBool(
					Options,
					TEXT("replace_existing"),
					OutSpec.Options.bReplaceExisting,
					OutError)
				|| !TryReadBool(
					Options,
					TEXT("import_materials"),
					OutSpec.Options.bImportMaterials,
					OutError)
				|| !TryReadBool(
					Options,
					TEXT("import_textures"),
					OutSpec.Options.bImportTextures,
					OutError)
				|| !TryReadBool(
					Options,
					TEXT("create_physics_asset"),
					OutSpec.Options.bCreatePhysicsAsset,
					OutError)
				|| !TryReadBool(Options, TEXT("import_animations"), OutSpec.Options.bImportAnimations, OutError)
				|| !TryReadBool(Options, TEXT("import_morph_targets"), OutSpec.Options.bImportMorphTargets, OutError)
				|| !TryReadBool(Options, TEXT("update_skeleton_reference_pose"), OutSpec.Options.bUpdateSkeletonReferencePose, OutError)
				|| !TryReadBool(Options, TEXT("validate_after_import"), OutSpec.Options.bValidateAfterImport, OutError)
				|| !TryReadString(Options, TEXT("skeleton_ref"), OutSpec.SkeletonRef, OutError)
				|| !TryReadString(Options, TEXT("compression_settings"), OutSpec.Options.CompressionSettings, OutError)
				|| !TryReadString(Options, TEXT("mip_gen_settings"), OutSpec.Options.MipGenSettings, OutError)
				|| !TryReadString(Options, TEXT("lod_group"), OutSpec.Options.LodGroup, OutError)
				|| !TryReadStringArray(Options, TEXT("expected_morph_targets"), OutSpec.Options.ExpectedMorphTargets, OutError)
				|| !TryReadStringArray(Options, TEXT("expected_material_slots"), OutSpec.Options.ExpectedMaterialSlots, OutError))
			{
				return false;
			}
			if (Options->HasField(TEXT("srgb")))
			{
				bool bSrgb = false;
				if (!TryReadBool(Options, TEXT("srgb"), bSrgb, OutError))
				{
					return false;
				}
				OutSpec.Options.Srgb = bSrgb;
			}
			if (Options->HasField(TEXT("expected_lod_count")))
			{
				int64 Value = 0;
				if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(
					Options->TryGetField(TEXT("expected_lod_count")), 0, MAX_int32, Value, OutError))
				{
					
					OutError.SubjectId = TEXT("expected_lod_count");
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/options/expected_lod_count"));
					return false;
				}
				OutSpec.Options.ExpectedLodCount = static_cast<int32>(Value);
			}
			if (OutSpec.Options.bReplaceExisting
				!= (Route.Operation
					== EFactoryExternalImportOperation::Reimport))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_replace_mode_invalid"), TEXT("/options/replace_existing"),
					MakeShared<FJsonValueBoolean>(Route.Operation == EFactoryExternalImportOperation::Reimport), Options->TryGetField(TEXT("replace_existing")),
					TEXT("input_decode"), TEXT("replace_existing must match whether this command imports or reimports."));
				return false;
			}
			const bool bFbx = Route.Kind
					== EFactoryExternalImportKind::FbxAnimation
				|| Route.Kind
					== EFactoryExternalImportKind::FbxSkeletalMesh;
			auto RejectInapplicableOptions = [&](bool bApplicable, std::initializer_list<const TCHAR*> Fields, const TCHAR* SupportedImporter)
			{
				if (bApplicable) return false;
				for (const auto* Field : Fields)
				{
					if (!Options->HasField(Field)) continue;
					OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("factory_import_option_not_applicable"), TEXT("/options/") + FString(Field),
						MakeShared<FJsonValueString>(SupportedImporter), Options->TryGetField(Field), TEXT("input_decode"),
						FString::Printf(TEXT("Option '%s' requires %s."), Field, SupportedImporter));
					return true;
				}
				return false;
			};
			if (RejectInapplicableOptions(bFbx,
				{TEXT("import_materials"), TEXT("import_textures"), TEXT("create_physics_asset"), TEXT("import_animations"),
				 TEXT("import_morph_targets"), TEXT("update_skeleton_reference_pose"), TEXT("skeleton_ref"),
				 TEXT("expected_morph_targets"), TEXT("expected_material_slots"), TEXT("expected_lod_count")}, TEXT("an FBX importer"))) return false;
			const bool bTexture = Route.Kind == EFactoryExternalImportKind::Texture;
			if (RejectInapplicableOptions(bTexture,
				{TEXT("srgb"), TEXT("compression_settings"), TEXT("mip_gen_settings"), TEXT("lod_group")}, TEXT("a texture importer"))) return false;
			if (RejectInapplicableOptions(Route.Kind == EFactoryExternalImportKind::FbxSkeletalMesh,
				{TEXT("import_animations"), TEXT("import_morph_targets"), TEXT("update_skeleton_reference_pose"), TEXT("skeleton_ref"),
				 TEXT("expected_morph_targets"), TEXT("expected_material_slots"), TEXT("expected_lod_count")}, TEXT("an FBX skeletal-mesh importer"))) return false;
			FString CanonicalOptions;
			if (!Json::TryCanonicalize(
					Options,
					CanonicalOptions,
					JsonError))
			{
				OutError = MoveTemp(JsonError);
				return false;
			}
			OutSpec.Options.Fingerprint = Json::Sha256Utf8(CanonicalOptions);
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
				TSharedRef<IFactoryExternalImportSession> InSession)
				: Session(MoveTemp(InSession))
			{
			}

			TSharedRef<IFactoryExternalImportSession> Session;
			FFactoryExternalImportError LastError;
			FFactoryExternalImportReadback Readback;
		};

		void SetStageFailure(
			const FFactoryExternalImportError& DomainError,
			const EOperationLifecycleStage Stage, const FString& Target,
			FOperationLifecycleError& OutError)
		{
			OutError = DomainError;
			if (OutError.IsEmpty())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("factory_import_operation_failed"),
					FString(TEXT("Factory import failed during ")) + LexToString(Stage) + TEXT(" without providing a diagnostic."),
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
						TEXT("factory-import:") + Plan.GetPlanId(),
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
				if (GFailureStage == TEXT("persist"))
				{
					OutError = Contract::FDiagnostic::Failure(TEXT("factory_import_test_failure"),
						TEXT("A test-injected failure stopped factory import persistence."), LexToString(EOperationLifecycleStage::Persist));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Context->Session->GetTargetIdentity();
					OutError.PropertyPath = TEXT("operation_succeeded");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
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
			const FFactoryExternalImportReadback& Readback)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("operation"), Readback.Operation);
			Result->SetStringField(
				TEXT("target_identity"),
				Readback.TargetIdentity);
			Result->SetStringField(TEXT("asset_class"), Readback.AssetClass);
			Result->SetStringField(TEXT("source_ref"), Readback.SourceRef);
			Result->SetStringField(
				TEXT("business_state_kind"),
				Readback.BusinessStateKind);
			Result->SetNumberField(
				TEXT("business_item_count"),
				Readback.BusinessItemCount);
			TArray<TSharedPtr<FJsonValue>> Dependencies;
			for (const FString& Identity : Readback.DependencyIdentities)
			{
				Dependencies.Add(MakeShared<FJsonValueString>(Identity));
			}
			Result->SetArrayField(TEXT("dependencies"), MoveTemp(Dependencies));
			if (Readback.Srgb.IsSet())
			{
				Result->SetBoolField(TEXT("srgb"), Readback.Srgb.GetValue());
			}
			if (!Readback.CompressionSettings.IsEmpty()) Result->SetStringField(TEXT("compression_settings"), Readback.CompressionSettings);
			if (!Readback.MipGenSettings.IsEmpty()) Result->SetStringField(TEXT("mip_gen_settings"), Readback.MipGenSettings);
			if (!Readback.LodGroup.IsEmpty()) Result->SetStringField(TEXT("lod_group"), Readback.LodGroup);
			if (Readback.LodCount != INDEX_NONE) Result->SetNumberField(TEXT("lod_count"), Readback.LodCount);
			Result->SetNumberField(TEXT("morph_target_count"), Readback.MorphTargetNames.Num());
			Result->SetNumberField(TEXT("material_slot_count"), Readback.MaterialSlotNames.Num());
			return Result;
		}

		class FService final : public IUeAgentFactoryExternalImportCommandService
		{
		public:
			explicit FService(TSharedRef<IFactoryExternalImportProvider> InProvider)
				: Provider(MoveTemp(InProvider))
			{
			}

			virtual bool Execute(
				const FUeAgentFactoryExternalImportCommandRequest& Request,
				FUeAgentFactoryExternalImportCommandResponse& OutResponse,
				FUeAgentFactoryExternalImportCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (!IsInGameThread())
				{
					return Fail(OutError, CommandGuardFailure(TEXT("factory_import_game_thread_required"),
						TEXT("Factory import must execute on the game thread."), Request.CommandId, TEXT("game_thread"), false, true));
				}
				if (!Request.AuthorityScopes.Contains(TEXT("uai:operate")))
				{
					return Fail(OutError, CommandGuardFailure(TEXT("factory_import_authority_required"),
						TEXT("Factory import requires the uai:operate authority scope."), Request.CommandId, TEXT("operate_authority"), false, true));
				}
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, CommandGuardFailure(TEXT("factory_import_cancelled"),
						TEXT("Factory import was cancelled before preparation."), Request.RequestId, TEXT("cancellation_requested"), true, false));
				}
				const FUeAgentFactoryExternalImportCommandRoute* const Route =
					FindRoute(Request.CommandId);
				if (!Route)
				{
					return Fail(OutError, CommandGuardFailure(TEXT("factory_import_command_not_supported"),
						TEXT("No factory import route is registered for the requested command."), Request.CommandId, TEXT("command_supported"), false, true));
				}
				FFactoryExternalImportSpec Spec;
				FFactoryExternalImportError DomainError;
				if (!TryBuildSpec(Request, *Route, Spec, DomainError))
				{
					return Fail(OutError, DomainError);
				}
				TSharedPtr<IFactoryExternalImportSession> Session;
				if (!Provider->Prepare(Spec, Session, DomainError)
					|| !Session.IsValid())
				{
					if (DomainError.IsEmpty())
					{
						DomainError = CommandGuardFailure(TEXT("factory_import_session_unavailable"),
							TEXT("Factory import preparation did not produce a session or a diagnostic."),
							Request.RequestId, TEXT("session_ready"), false, true);
						DomainError.Stage = TEXT("prepare");
						DomainError.TargetPath = Spec.DestinationRef;
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
					TEXT("t28:") + Request.RequestId,
					PlanFingerprint,
					{Target},
					{{Target, Session->GetExpectedRevision()}},
					{TEXT("asset_import_data_attached"), TEXT("asset_imported")});
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
				Payload->SetObjectField(TEXT("readback"), ReadbackJson(Context->Readback));
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
			TSharedRef<IFactoryExternalImportProvider> Provider;
		};
	}

	bool FUeAgentFactoryExternalImportCommandRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentFactoryExternalImportCommandRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("factory_import_route_invalid"),
				TEXT("The route requires a non-empty command_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonFunction.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("factory_import_route_invalid"),
				TEXT("The route requires a non-empty python_function."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_function");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(PythonFunction);
			return false;
		}
		const bool bRequiresReimport = Kind == EFactoryExternalImportKind::StaticMesh;
		const bool bRequiresImport = Kind == EFactoryExternalImportKind::FbxAnimation
			|| Kind == EFactoryExternalImportKind::FbxSkeletalMesh
			|| Kind == EFactoryExternalImportKind::GeometryCache
			|| Kind == EFactoryExternalImportKind::LightProfile;
		if ((bRequiresReimport && Operation != EFactoryExternalImportOperation::Reimport)
			|| (bRequiresImport && Operation != EFactoryExternalImportOperation::Import))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("factory_import_route_invalid"),
				TEXT("The import operation is not supported for this factory kind."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("operation");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(bRequiresReimport ? TEXT("reimport") : TEXT("import"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Operation));
			return false;
		}
		return true;
	}

	const TArray<FUeAgentFactoryExternalImportCommandRoute>&
	GetFactoryExternalImportCommandRoutes()
	{
		static const TArray<FUeAgentFactoryExternalImportCommandRoute> Routes = {
			{TEXT("asset.reimport_sound_wave"), TEXT("reimport_sound_wave"), EFactoryExternalImportOperation::Reimport, EFactoryExternalImportKind::SoundWave},
			{TEXT("asset.reimport_sparse_volume_texture"), TEXT("reimport_sparse_volume_texture"), EFactoryExternalImportOperation::Reimport, EFactoryExternalImportKind::SparseVolumeTexture},
			{TEXT("asset.reimport_static_mesh"), TEXT("reimport_static_mesh"), EFactoryExternalImportOperation::Reimport, EFactoryExternalImportKind::StaticMesh},
			{TEXT("asset.reimport_texture"), TEXT("reimport_texture"), EFactoryExternalImportOperation::Reimport, EFactoryExternalImportKind::Texture},
			{TEXT("external.fbx.animation.import"), TEXT("fbx_animation_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::FbxAnimation},
			{TEXT("external.fbx.skeletal_mesh.import"), TEXT("fbx_skeletal_mesh_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::FbxSkeletalMesh},
			{TEXT("external.geometry_cache.import"), TEXT("geometry_cache_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::GeometryCache},
			{TEXT("external.light_profile.import"), TEXT("light_profile_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::LightProfile},
			{TEXT("external.sound_wave.import"), TEXT("sound_wave_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::SoundWave},
			{TEXT("external.sparse_volume_texture.import"), TEXT("sparse_volume_texture_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::SparseVolumeTexture},
			{TEXT("external.texture.import"), TEXT("texture_import"), EFactoryExternalImportOperation::Import, EFactoryExternalImportKind::Texture},
		};
		return Routes;
	}

	bool TryCreateProductionFactoryExternalImportCommandService(
		TSharedPtr<IUeAgentFactoryExternalImportCommandService>& OutService,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutService.Reset();
		TSharedPtr<IFactoryExternalImportProvider> Provider;
		if (!TryCreateFactoryExternalImportProvider(Provider, OutError)
			|| !Provider.IsValid())
		{
			return false;
		}
		OutService = MakeShared<FService>(Provider.ToSharedRef());
		OutError.Reset();
		return true;
	}

	namespace TestHooks
	{
		FScopedFactoryImportFailureAtStage::
		FScopedFactoryImportFailureAtStage(FString Stage)
			: Previous(GFailureStage)
		{
			Stage.TrimStartAndEndInline();
			GFailureStage = MoveTemp(Stage);
		}

		FScopedFactoryImportFailureAtStage::~
		FScopedFactoryImportFailureAtStage()
		{
			GFailureStage = MoveTemp(Previous);
		}
	}
}
