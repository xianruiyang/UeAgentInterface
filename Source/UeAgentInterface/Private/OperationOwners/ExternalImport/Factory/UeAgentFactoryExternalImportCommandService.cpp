#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"

#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::OperationOwners::ExternalImport::Factory
{
	using namespace BindingGateway::Operation;
	using namespace CommandContract;
	using namespace DomainAdapters::ExternalImport::Factory;

	namespace
	{
		FString GFailureStage;

		bool Fail(
			FUeAgentFactoryExternalImportCommandError& OutError,
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
				OutError.Code = TEXT("factory_import_option_type_invalid");
				OutError.SubjectId = Name;
				return false;
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
			if (!Options->TryGetStringField(Name, OutValue))
			{
				OutError.Code = TEXT("factory_import_option_type_invalid");
				OutError.SubjectId = Name;
				return false;
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
				OutError.Code = TEXT("factory_import_option_type_invalid");
				OutError.SubjectId = Name;
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					OutError.Code = TEXT("factory_import_option_type_invalid");
					OutError.SubjectId = Name;
					return false;
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
			FString JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError)
				|| !Params.IsValid())
			{
				OutError.Code = TEXT("factory_import_params_invalid");
				OutError.Detail = MoveTemp(JsonError);
				return false;
			}
			OutSpec.RequestId = Request.RequestId;
			OutSpec.Operation = Route.Operation;
			OutSpec.Kind = Route.Kind;
			if (!Params->TryGetStringField(TEXT("source_ref"), OutSpec.SourceRef))
			{
				OutError.Code = TEXT("factory_import_source_required");
				return false;
			}
			if (Route.Operation == EFactoryExternalImportOperation::Import)
			{
				if (!Params->TryGetStringField(
						TEXT("destination_ref"),
						OutSpec.DestinationRef))
				{
					OutError.Code = TEXT("factory_import_destination_required");
					return false;
				}
				if (Route.Kind == EFactoryExternalImportKind::FbxAnimation
					&& !Params->TryGetStringField(
						TEXT("skeleton_ref"),
						OutSpec.SkeletonRef))
				{
					OutError.Code = TEXT("factory_import_skeleton_required");
					return false;
				}
			}
			else if (!Params->TryGetStringField(
					TEXT("target_ref"),
					OutSpec.DestinationRef))
			{
				OutError.Code = TEXT("factory_reimport_target_required");
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
					OutError.Code = TEXT("factory_import_options_invalid");
					return false;
				}
				Options = OptionsPtr->ToSharedRef();
			}
			const TSet<FString> AllowedOptionNames = {
				TEXT("replace_existing"),
				TEXT("import_materials"),
				TEXT("import_textures"),
				TEXT("create_physics_asset"),
				TEXT("import_animations"),
				TEXT("import_morph_targets"),
				TEXT("update_skeleton_reference_pose"),
				TEXT("validate_after_import"),
				TEXT("skeleton_ref"),
				TEXT("srgb"),
				TEXT("compression_settings"),
				TEXT("mip_gen_settings"),
				TEXT("lod_group"),
				TEXT("expected_morph_targets"),
				TEXT("expected_material_slots"),
				TEXT("expected_lod_count"),
			};
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: Options->Values)
			{
				if (!AllowedOptionNames.Contains(Pair.Key))
				{
					OutError.Code = TEXT("factory_import_option_unknown");
					OutError.SubjectId = Pair.Key;
					return false;
				}
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
				double Value = 0.0;
				if (!Options->TryGetNumberField(TEXT("expected_lod_count"), Value)
					|| Value < 0.0 || Value != FMath::FloorToDouble(Value))
				{
					OutError.Code = TEXT("factory_import_option_type_invalid");
					OutError.SubjectId = TEXT("expected_lod_count");
					return false;
				}
				OutSpec.Options.ExpectedLodCount = static_cast<int32>(Value);
			}
			if (OutSpec.Options.bReplaceExisting
				!= (Route.Operation
					== EFactoryExternalImportOperation::Reimport))
			{
				OutError.Code = TEXT("factory_import_replace_mode_invalid");
				return false;
			}
			const bool bFbx = Route.Kind
					== EFactoryExternalImportKind::FbxAnimation
				|| Route.Kind
					== EFactoryExternalImportKind::FbxSkeletalMesh;
			if (!bFbx
				&& (Options->HasField(TEXT("import_materials"))
					|| Options->HasField(TEXT("import_textures"))
					|| Options->HasField(TEXT("create_physics_asset"))
					|| Options->HasField(TEXT("import_animations"))
					|| Options->HasField(TEXT("import_morph_targets"))
					|| Options->HasField(TEXT("update_skeleton_reference_pose"))
					|| Options->HasField(TEXT("skeleton_ref"))
					|| Options->HasField(TEXT("expected_morph_targets"))
					|| Options->HasField(TEXT("expected_material_slots"))
					|| Options->HasField(TEXT("expected_lod_count"))))
			{
				OutError.Code = TEXT("factory_import_option_not_applicable");
				return false;
			}
			const bool bTexture = Route.Kind == EFactoryExternalImportKind::Texture;
			if (!bTexture
				&& (Options->HasField(TEXT("srgb"))
					|| Options->HasField(TEXT("compression_settings"))
					|| Options->HasField(TEXT("mip_gen_settings"))
					|| Options->HasField(TEXT("lod_group"))))
			{
				OutError.Code = TEXT("factory_import_option_not_applicable");
				return false;
			}
			if (Route.Kind != EFactoryExternalImportKind::FbxSkeletalMesh
				&& (Options->HasField(TEXT("import_animations"))
					|| Options->HasField(TEXT("import_morph_targets"))
					|| Options->HasField(TEXT("update_skeleton_reference_pose"))
					|| Options->HasField(TEXT("skeleton_ref"))
					|| Options->HasField(TEXT("expected_morph_targets"))
					|| Options->HasField(TEXT("expected_material_slots"))
					|| Options->HasField(TEXT("expected_lod_count"))))
			{
				OutError.Code = TEXT("factory_import_option_not_applicable");
				return false;
			}
			FString CanonicalOptions;
			if (!Json::TryCanonicalize(
					Options,
					CanonicalOptions,
					JsonError))
			{
				OutError.Code = TEXT("factory_import_options_invalid");
				OutError.Detail = MoveTemp(JsonError);
				return false;
			}
			OutSpec.Options.Fingerprint = Json::Sha256Utf8(CanonicalOptions);
			return OutSpec.IsValid();
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

		void ConvertError(
			const FFactoryExternalImportError& DomainError,
			FOperationLifecycleError& OutError)
		{
			OutError = FOperationLifecycleError(
				DomainError.Code.IsEmpty()
					? FString(TEXT("factory_import_operation_failed"))
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
						TEXT("factory-import:") + Plan.GetPlanId(),
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
				if (GFailureStage == TEXT("persist"))
				{
					OutError = FOperationLifecycleError(
						TEXT("factory_import_test_failure"),
						TEXT("persist"));
					return false;
				}
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
				TEXT("source_sha256"),
				Readback.SourceSha256);
			Result->SetStringField(
				TEXT("import_data_class"),
				Readback.ImportDataClass);
			Result->SetStringField(
				TEXT("options_fingerprint"),
				Readback.OptionsFingerprint);
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
			Result->SetArrayField(
				TEXT("dependency_identities"),
				MoveTemp(Dependencies));
			Result->SetBoolField(TEXT("reloaded"), Readback.bReloaded);
			if (Readback.Srgb.IsSet())
			{
				Result->SetBoolField(TEXT("srgb"), Readback.Srgb.GetValue());
			}
			if (!Readback.CompressionSettings.IsEmpty()) Result->SetStringField(TEXT("compression_settings"), Readback.CompressionSettings);
			if (!Readback.MipGenSettings.IsEmpty()) Result->SetStringField(TEXT("mip_gen_settings"), Readback.MipGenSettings);
			if (!Readback.LodGroup.IsEmpty()) Result->SetStringField(TEXT("lod_group"), Readback.LodGroup);
			if (Readback.LodCount != INDEX_NONE) Result->SetNumberField(TEXT("lod_count"), Readback.LodCount);
			auto Strings = [](const TArray<FString>& Values)
			{
				TArray<TSharedPtr<FJsonValue>> ResultValues;
				for (const FString& Value : Values) ResultValues.Add(MakeShared<FJsonValueString>(Value));
				return ResultValues;
			};
			Result->SetArrayField(TEXT("morph_target_names"), Strings(Readback.MorphTargetNames));
			Result->SetArrayField(TEXT("material_slot_names"), Strings(Readback.MaterialSlotNames));
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
					return Fail(
						OutError,
						TEXT("factory_import_game_thread_required"),
						Request.CommandId);
				}
				if (!Request.AuthorityScopes.Contains(TEXT("uai:operate")))
				{
					return Fail(
						OutError,
						TEXT("factory_import_authority_required"),
						Request.CommandId);
				}
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("factory_import_cancelled"),
						Request.RequestId);
				}
				const FUeAgentFactoryExternalImportCommandRoute* const Route =
					FindRoute(Request.CommandId);
				if (!Route)
				{
					return Fail(
						OutError,
						TEXT("factory_import_command_not_supported"),
						Request.CommandId);
				}
				FFactoryExternalImportSpec Spec;
				FFactoryExternalImportError DomainError;
				if (!TryBuildSpec(Request, *Route, Spec, DomainError))
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("factory_import_params_invalid"))
							: DomainError.Code,
						DomainError.SubjectId,
						DomainError.Detail);
				}
				TSharedPtr<IFactoryExternalImportSession> Session;
				if (!Provider->Prepare(Spec, Session, DomainError)
					|| !Session.IsValid())
				{
					return Fail(
						OutError,
						DomainError.Code.IsEmpty()
							? FString(TEXT("factory_import_prepare_failed"))
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
					return Fail(
						OutError,
						Receipt.GetErrorCode().IsEmpty()
							? FString(TEXT("factory_import_operation_failed"))
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
						TEXT("factory_import_result_invalid"),
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
			TSharedRef<IFactoryExternalImportProvider> Provider;
		};
	}

	bool FUeAgentFactoryExternalImportCommandRoute::IsValid() const
	{
		if (CommandId.IsEmpty() || PythonFunction.IsEmpty())
		{
			return false;
		}
		if (Kind == EFactoryExternalImportKind::StaticMesh)
		{
			return Operation == EFactoryExternalImportOperation::Reimport;
		}
		if (Kind == EFactoryExternalImportKind::FbxAnimation
			|| Kind == EFactoryExternalImportKind::FbxSkeletalMesh
			|| Kind == EFactoryExternalImportKind::GeometryCache
			|| Kind == EFactoryExternalImportKind::LightProfile)
		{
			return Operation == EFactoryExternalImportOperation::Import;
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
		FString& OutError)
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
