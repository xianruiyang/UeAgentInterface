#include "Composition/TimelineSpatial/UeAgentTimelineSpatialRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Async/Async.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialCommandHandlers.h"
#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"
#include "Core/Verification/UeAgentVerificationOutcomeView.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionRuntime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace BindingGateway::Operation;
	using namespace BindingGateway::Query;
	using namespace BindingGateway::Ticket;
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace PlanningPreflight;
	using namespace Projection;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		FDiagnostic RuntimeFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("runtime"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
		}

		FDiagnostic InputFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
			const TSharedPtr<FJsonValue>& Actual, const TCHAR* Expected)
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("input_validation"));
			Diagnostic.PropertyPath = Pointer;
			Diagnostic.Source.JsonPath = Pointer;
			Diagnostic.ActualValue = Actual;
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Diagnostic;
		}

		FDiagnostic ValidateRuntimeConfig(const FUeAgentTimelineSpatialRuntimeConfig& Config)
		{
			FDiagnostic Diagnostic;
			if (Config.ProjectInstanceId.TrimStartAndEnd().IsEmpty())
			{
				Diagnostic = RuntimeFailure(TEXT("timeline_spatial_runtime_config_invalid"), TEXT("timeline_spatial"),
					TEXT("The runtime configuration requires a project instance identifier."));
				Diagnostic.PropertyPath = TEXT("config.project_instance_id");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Config.ProjectInstanceId);
			}
			else if (Config.EditorInstanceId.TrimStartAndEnd().IsEmpty())
			{
				Diagnostic = RuntimeFailure(TEXT("timeline_spatial_runtime_config_invalid"), TEXT("timeline_spatial"),
					TEXT("The runtime configuration requires an editor instance identifier."));
				Diagnostic.PropertyPath = TEXT("config.editor_instance_id");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Config.EditorInstanceId);
			}
			else if (Config.ServerSessionId.TrimStartAndEnd().IsEmpty())
			{
				Diagnostic = RuntimeFailure(TEXT("timeline_spatial_runtime_config_invalid"), TEXT("timeline_spatial"),
					TEXT("The runtime configuration requires a server session identifier."));
				Diagnostic.PropertyPath = TEXT("config.server_session_id");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Config.ServerSessionId);
			}
			else if (Config.RegistryEpoch <= 0)
			{
				Diagnostic = RuntimeFailure(TEXT("timeline_spatial_runtime_config_invalid"), TEXT("timeline_spatial"),
					TEXT("The runtime configuration requires a positive registry epoch."));
				Diagnostic.PropertyPath = TEXT("config.registry_epoch");
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Config.RegistryEpoch));
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive integer"));
			}
			if (!Diagnostic.IsEmpty() && !Diagnostic.ExpectedValue.IsValid())
				Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty identifier"));
			return Diagnostic;
		}

		void SetError(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
        }

		bool TryObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject, FDiagnostic& OutError)
		{
			return UE::UAI::Contract::Json::TryParseObject(Json, OutObject, OutError);
		}

		FUeAgentQueryResult QueryErrorResult(
			const FUeAgentQueryRequest& Request, FDiagnostic Diagnostic)
		{
			if (Diagnostic.IsEmpty())
			{
				Diagnostic.Code = TEXT("query_execution_failed");
				Diagnostic.PublicMessage = TEXT("The query application received a failure without a primary diagnostic.");
				Diagnostic.Stage = TEXT("query_application");
			}
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetDescriptorIdentity().GetDescriptorId();
			return FUeAgentQueryResult(Request.GetDescriptorIdentity(), Request.GetRequestId(), MoveTemp(Diagnostic));
		}

		FUeAgentOperationResult OperationErrorResult(
			const FUeAgentOperationRequest& Request, FDiagnostic Diagnostic)
		{
			if (Diagnostic.IsEmpty())
			{
				Diagnostic.Code = TEXT("timeline_spatial_operation_failed");
				Diagnostic.PublicMessage = TEXT("The operation application received a failure without a primary diagnostic.");
				Diagnostic.Stage = TEXT("application");
			}
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetDescriptorIdentity().GetDescriptorId();
			return FUeAgentOperationResult(Request.GetDescriptorIdentity(), Request.GetRequestId(), MoveTemp(Diagnostic));
		}


		bool TryReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FString>& OutValues,
			FDiagnostic& OutError)
		{
			OutValues.Reset();
			if (!Object.IsValid() || !Object->HasField(Field))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || !Values)
			{
				OutError = InputFailure(TEXT("timeline_spatial_context_identity_array_invalid"),
					TEXT("The execution context field must be an array of non-empty strings."),
					FString(TEXT("/context/")) + Field, Object->TryGetField(Field), TEXT("array of non-empty strings"));
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid()
					|| !Value->TryGetString(Text)
					|| Text.TrimStartAndEnd().IsEmpty())
				{
					OutError = InputFailure(TEXT("timeline_spatial_context_identity_array_invalid"),
						TEXT("The execution context array item must be a non-empty string."),
						FString::Printf(TEXT("/context/%s/%d"), Field, Index), Value, TEXT("non-empty string"));
					return false;
				}
				OutValues.Add(MoveTemp(Text));
			}
			return true;
		}

		bool TryParseWorldType(
			const FString& Text,
			EUeAgentExecutionWorldType& OutType)
		{
			if (Text == TEXT("editor"))
			{
				OutType = EUeAgentExecutionWorldType::Editor;
				return true;
			}
			if (Text == TEXT("pie"))
			{
				OutType = EUeAgentExecutionWorldType::Pie;
				return true;
			}
			if (Text == TEXT("simulate_in_editor"))
			{
				OutType = EUeAgentExecutionWorldType::SimulateInEditor;
				return true;
			}
			if (Text == TEXT("game"))
			{
				OutType = EUeAgentExecutionWorldType::Game;
				return true;
			}
			if (Text == TEXT("preview"))
			{
				OutType = EUeAgentExecutionWorldType::Preview;
				return true;
			}
			return false;
		}

		bool TryBuildExecutionContext(
			const FUeAgentTimelineSpatialRuntimeConfig& Config,
			const FUeAgentOperationDescriptor& Descriptor,
			const TSharedPtr<FJsonObject>& Root,
			const FString& WorldIdentity,
			const FString& LevelIdentity,
			FUeAgentExecutionContextInput& OutInput,
			FDiagnostic& OutError)
		{
			OutError.Reset();
			const TArray<EUeAgentStatePlane>& AllowedPlanes =
				Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
			if (AllowedPlanes.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("timeline_spatial_context_state_plane_missing"),
					TEXT("The command execution policy declares no allowed state plane."), TEXT("input_validation"));
				OutError.PropertyPath = TEXT("execution_context_policy.allowed_state_planes");
				OutError.ActualValue = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{});
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one allowed state plane"));
				OutError.SubjectId = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			OutInput = {};
			OutInput.ProjectInstanceId = Config.ProjectInstanceId;
			OutInput.EditorInstanceId = Config.EditorInstanceId;
			OutInput.StatePlane = AllowedPlanes[0];
			OutInput.WorldType = EUeAgentExecutionWorldType::Editor;
			OutInput.WorldIdentity = WorldIdentity;
			OutInput.LevelIdentity = LevelIdentity;
			OutInput.ContextRevision =
				Config.ServerSessionId
				+ TEXT(":")
				+ FString::Printf(TEXT("%lld"), Config.RegistryEpoch)
				+ TEXT(":")
				+ Descriptor.GetIdentity().GetDescriptorHash();
			OutInput.bResolvedFromCurrent = false;

			const TSharedPtr<FJsonObject>* PublicContext = nullptr;
			if (!Root.IsValid()
				|| !Root->TryGetObjectField(TEXT("context"), PublicContext)
				|| !PublicContext
				|| !PublicContext->IsValid())
			{
				return true;
			}
			const TSharedPtr<FJsonObject>& Context = *PublicContext;
			for (const TCHAR* Field : {TEXT("project_instance_id"), TEXT("editor_instance_id"),
				TEXT("context_revision"), TEXT("resolved_from_current")})
			{
				if (Context->HasField(Field))
				{
					OutError = FDiagnostic::Failure(TEXT("timeline_spatial_context_server_field_override"),
						TEXT("This execution context field is supplied by the server and cannot be overridden."), TEXT("input_validation"));
					OutError.PropertyPath = FString(TEXT("/context/")) + Field;
					OutError.Source.JsonPath = OutError.PropertyPath;
					OutError.ActualValuePresent = true;
					OutError.ExpectedValuePresent = false;
					OutError.ActualEvidence = Context->TryGetField(Field);
					return false;
				}
			}

			FString Text;
			if (Context->TryGetStringField(TEXT("state_plane"), Text)
				&& !TryParseStatePlane(Text, OutInput.StatePlane))
			{
				OutError = InputFailure(TEXT("timeline_spatial_context_state_plane_invalid"),
					TEXT("The execution context state plane is not recognized."), TEXT("/context/state_plane"),
					Context->TryGetField(TEXT("state_plane")), TEXT("authoring_memory, persisted, compiled_generated, runtime_instance, rendered_observation, registry_index"));
				return false;
			}
			if (Context->TryGetStringField(TEXT("world_type"), Text)
				&& !TryParseWorldType(Text, OutInput.WorldType))
			{
				OutError = InputFailure(TEXT("timeline_spatial_context_world_type_invalid"),
					TEXT("The execution context world type is not recognized."), TEXT("/context/world_type"),
					Context->TryGetField(TEXT("world_type")), TEXT("editor, pie, simulate_in_editor, game, preview"));
				return false;
			}
			Context->TryGetStringField(
				TEXT("world"),
				OutInput.WorldIdentity);
			Context->TryGetStringField(
				TEXT("level"),
				OutInput.LevelIdentity);
			Context->TryGetStringField(
				TEXT("world_partition"),
				OutInput.WorldPartitionIdentity);
			Context->TryGetStringField(
				TEXT("net_mode"),
				OutInput.NetMode);
			Context->TryGetStringField(
				TEXT("net_role"),
				OutInput.NetRole);
			Context->TryGetStringField(
				TEXT("asset_editor"),
				OutInput.AssetEditorIdentity);
			Context->TryGetStringField(
				TEXT("viewport"),
				OutInput.ViewportIdentity);
			if (OutInput.ViewportIdentity.StartsWith(
					TEXT("asset:"), ESearchCase::CaseSensitive))
			{
				FString PublicTarget;
				FString ViewportScope;
				FString ViewportRole;
				Root->TryGetStringField(TEXT("target"), PublicTarget);
				Context->TryGetStringField(
					TEXT("viewport_scope"), ViewportScope);
				Context->TryGetStringField(
					TEXT("viewport_role"), ViewportRole);
				const FString ExpectedIdentity = TEXT("asset:")
					+ OutInput.AssetEditorIdentity
					+ TEXT("#blueprint_components");
				if (OutInput.AssetEditorIdentity.IsEmpty())
				{
					OutError = InputFailure(TEXT("timeline_spatial_blueprint_viewport_target_invalid"),
						TEXT("A Blueprint component viewport requires an explicit asset editor reference."),
						TEXT("/context/asset_editor"), Context->TryGetField(TEXT("asset_editor")), TEXT("non-empty asset editor reference"));
					return false;
				}
				if (PublicTarget != OutInput.ViewportIdentity)
				{
					OutError = InputFailure(TEXT("timeline_spatial_blueprint_viewport_target_invalid"),
						TEXT("The target must identify the viewport selected in the execution context."),
						TEXT("/target"), Root->TryGetField(TEXT("target")), *OutInput.ViewportIdentity);
					return false;
				}
				if (OutInput.ViewportIdentity != ExpectedIdentity)
				{
					OutError = InputFailure(TEXT("timeline_spatial_blueprint_viewport_target_invalid"),
						TEXT("The viewport must identify the component viewport of the selected asset editor."),
						TEXT("/context/viewport"), Context->TryGetField(TEXT("viewport")), *ExpectedIdentity);
					return false;
				}
				if (ViewportScope != TEXT("asset_editor"))
				{
					OutError = InputFailure(TEXT("timeline_spatial_blueprint_viewport_target_invalid"),
						TEXT("The Blueprint component viewport requires asset editor scope."),
						TEXT("/context/viewport_scope"), Context->TryGetField(TEXT("viewport_scope")), TEXT("asset_editor"));
					return false;
				}
				if (ViewportRole != TEXT("components"))
				{
					OutError = InputFailure(TEXT("timeline_spatial_blueprint_viewport_target_invalid"),
						TEXT("The Blueprint component viewport requires the components role."),
						TEXT("/context/viewport_role"), Context->TryGetField(TEXT("viewport_role")), TEXT("components"));
					return false;
				}
			}
			if (!TryReadStringArray(
					Context,
					TEXT("data_layers"),
					OutInput.DataLayerIdentities, OutError)
				|| !TryReadStringArray(
					Context,
					TEXT("selection"),
					OutInput.SelectionIdentities, OutError))
			{
				return false;
			}
			double PieInstance = -1.0;
			if (Context->TryGetNumberField(
					TEXT("pie_instance"),
					PieInstance))
			{
				OutInput.PieInstanceId =
					static_cast<int32>(PieInstance);
			}
			return true;
		}

		bool TryProjection(
			const TSharedPtr<FJsonObject>& Root,
			FUeAgentProjectionRequest& OutProjection,
			FDiagnostic& OutError)
		{
			OutError.Reset();
			FString ProfileId = TEXT("compact");
			bool bDefaults = false;
			bool bDetails = false;
			bool bProvenance = false;

			const TSharedPtr<FJsonObject>* PublicProjection = nullptr;
			if (Root.IsValid()
				&& Root->TryGetObjectField(
					TEXT("projection"),
					PublicProjection)
				&& PublicProjection
				&& PublicProjection->IsValid())
			{
				const TSharedPtr<FJsonObject>& Projection =
					*PublicProjection;
				Projection->TryGetStringField(
					TEXT("profile_id"),
					ProfileId);
				Projection->TryGetBoolField(
					TEXT("include_defaults"),
					bDefaults);
				Projection->TryGetBoolField(
					TEXT("include_details"),
					bDetails);
				Projection->TryGetBoolField(
					TEXT("include_provenance"),
					bProvenance);
			}
			OutProjection = FUeAgentProjectionRequest(
				FUeAgentProjectionProfile(
					ProfileId,
					bDefaults,
					bDetails,
					bProvenance),
				{},
				{});
			if (!OutProjection.IsValid())
			{
				OutError = OutProjection.GetValidationDiagnostic();
				OutError.PropertyPath = TEXT("projection.") + OutError.PropertyPath;
				OutError.Source.JsonPath = TEXT("/projection/profile_id");
				if (!OutError.ActualValue.IsValid()) OutError.ActualValue = MakeShared<FJsonValueString>(ProfileId);
				if (!OutError.ExpectedValue.IsValid()) OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
				return false;
			}
			return true;
		}

		bool TryInputObject(
			const TSharedPtr<FJsonObject>& Root,
			FUeAgentContractValue& OutInput,
			FDiagnostic& OutError)
		{
			OutError.Reset();
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!Root.IsValid())
			{
				OutError = InputFailure(TEXT("timeline_spatial_public_wrapper_invalid"),
					TEXT("The public request wrapper is unavailable."), TEXT("/"), nullptr, TEXT("object"));
				return false;
			}
			if (!Root->TryGetObjectField(TEXT("input"), Input)
				|| !Input
				|| !Input->IsValid())
			{
				OutInput = FUeAgentContractValue(TEXT("{}"));
				return true;
			}
			FString Json;
			if (!Json::TryCanonicalize(Input->ToSharedRef(), Json, OutError)) return false;
			OutInput = FUeAgentContractValue(MoveTemp(Json));
			if (!OutInput.IsValid())
			{
				OutError = FDiagnostic::Failure(TEXT("timeline_spatial_input_value_invalid"),
					TEXT("Serializing the input object produced an empty JSON document."), TEXT("input_validation"));
				OutError.PropertyPath = TEXT("input");
				OutError.Source.JsonPath = TEXT("/input");
				OutError.ActualValue = MakeShared<FJsonValueString>(OutInput.GetJsonValue());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty JSON document"));
				return false;
			}
			return true;
		}

		FUeAgentCandidateBinding MakeCandidate(
			const FString& CapabilityId,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FString& PublicRef,
			const FString& Generation)
		{
			const FString IdentityId =
				TEXT("timeline-spatial:")
				+ CapabilityId
				+ TEXT(":")
				+ PublicRef;
			return FUeAgentCandidateBinding(
				TEXT("timeline-spatial-binding:") + IdentityId,
				FUeAgentCandidateSummary(
					FUeAgentCanonicalIdentity(
						IdentityId,
						TEXT("timeline_spatial_target"),
						TEXT("uai"),
						PublicRef,
						TEXT("target"),
						Generation),
					DescriptorIdentity,
					TEXT("timeline_spatial_target"),
					PublicRef,
					TEXT("explicit public Timeline/Spatial target"),
					{TEXT("TimelineSpatial"), CapabilityId}),
				FUeAgentCatalogRevision(
					TEXT("timeline-spatial-explicit"),
					Generation,
					1),
				PublicRef,
				TEXT("timeline-spatial-production"),
				{
					EUeAgentCandidateOperation::Read,
					EUeAgentCandidateOperation::Create,
					EUeAgentCandidateOperation::Update,
					EUeAgentCandidateOperation::Replace,
					EUeAgentCandidateOperation::Delete,
				},
				TEXT("explicit-public-wrapper"));
		}

		TSharedPtr<FJsonValue> CanonicalValueToJson(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Value.GetBoolean());
			case EUeAgentValueKind::String:
				return MakeShared<FJsonValueString>(Value.GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				return MakeShared<FJsonValueString>(Value.GetLexeme());
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FUeAgentCanonicalValueRef& Item :
					Value.GetArrayItems())
				{
					Items.Add(CanonicalValueToJson(*Item));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EUeAgentValueKind::Object:
			{
				const TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				for (const TPair<FString, FUeAgentCanonicalValueRef>& Item :
					Value.GetObjectItems())
				{
					Object->SetField(
						Item.Key,
						CanonicalValueToJson(*Item.Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Temporal:
			{
				const TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				if (Value.GetTemporal().IsValid())
				{
					Object->SetStringField(
						TEXT("clock_id"),
						Value.GetTemporal()->GetClockId());
					Object->SetStringField(
						TEXT("tick"),
						Value.GetTemporal()->GetTick());
					Object->SetStringField(
						TEXT("rate_numerator"),
						Value.GetTemporal()->GetRateNumerator());
					Object->SetStringField(
						TEXT("rate_denominator"),
						Value.GetTemporal()->GetRateDenominator());
					Object->SetStringField(
						TEXT("state"),
						Value.GetTemporal()->GetState());
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Spatial:
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		class FTimelineSpatialSessionSource final
			: public IUeAgentQuerySessionSource
			, public IUeAgentOperationSessionSource
		{
		public:
			explicit FTimelineSpatialSessionSource(
				FUeAgentOperationSessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FUeAgentQuerySessionIdentity GetCurrent() const override
			{
				return Session;
			}

		private:
			FUeAgentOperationSessionIdentity Session;
		};

		class FTimelineSpatialExecutionContextValidator final
			: public IUeAgentExecutionContextValidator
		{
		public:
			FTimelineSpatialExecutionContextValidator(
				FUeAgentTimelineSpatialRuntimeConfig InConfig,
				FString InWorldIdentity,
				FString InLevelIdentity)
				: Config(MoveTemp(InConfig))
				, WorldIdentity(MoveTemp(InWorldIdentity))
				, LevelIdentity(MoveTemp(InLevelIdentity))
			{
			}

			bool TryValidate(
				const FUeAgentExecutionContextRef& Context,
				FUeAgentExecutionContextError& OutError)
				const override
			{
				OutError = {};
				if (Context.GetProjectInstanceId()
						!= Config.ProjectInstanceId
					|| Context.GetEditorInstanceId()
						!= Config.EditorInstanceId)
				{
					const bool bProject = Context.GetProjectInstanceId() != Config.ProjectInstanceId;
					OutError = FDiagnostic::Failure(TEXT("execution_context_instance_conflict"),
						bProject ? TEXT("The execution context belongs to a different project instance.")
							: TEXT("The execution context belongs to a different editor instance."), TEXT("context_validation"));
					OutError.PropertyPath = bProject ? TEXT("context.project_instance_id") : TEXT("context.editor_instance_id");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(bProject ? Context.GetProjectInstanceId() : Context.GetEditorInstanceId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(bProject ? Config.ProjectInstanceId : Config.EditorInstanceId);
					return false;
				}
				const FString ExpectedRevision =
					Config.ServerSessionId
						+ TEXT(":")
						+ FString::Printf(
							TEXT("%lld"),
							Config.RegistryEpoch)
						+ TEXT(":")
						+ Context.GetDescriptorIdentity()
							.GetDescriptorHash();
				if (Context.GetContextRevision()
						!= ExpectedRevision
					|| Context.WasResolvedFromCurrent())
				{
					const bool bRevision = Context.GetContextRevision() != ExpectedRevision;
					OutError = FDiagnostic::Failure(TEXT("execution_context_revision_conflict"),
						bRevision ? TEXT("The execution context revision does not match the active runtime.")
							: TEXT("This runtime requires an explicitly bound context, not one resolved from the current selection."), TEXT("context_validation"));
					OutError.PropertyPath = bRevision ? TEXT("context.context_revision") : TEXT("context.resolved_from_current");
					if (bRevision)
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Context.GetContextRevision());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedRevision);
					}
					else
					{
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					}
					return false;
				}
				if ((!Context.GetWorldIdentity().IsEmpty()
						&& Context.GetWorldIdentity()
							!= WorldIdentity)
					|| (!Context.GetLevelIdentity().IsEmpty()
						&& Context.GetLevelIdentity()
							!= LevelIdentity))
				{
					const bool bWorld = !Context.GetWorldIdentity().IsEmpty() && Context.GetWorldIdentity() != WorldIdentity;
					OutError = FDiagnostic::Failure(TEXT("execution_context_world_conflict"),
						bWorld ? TEXT("The execution context world does not match the bound runtime world.")
							: TEXT("The execution context level does not match the bound runtime level."), TEXT("context_validation"));
					OutError.PropertyPath = bWorld ? TEXT("context.world") : TEXT("context.level");
					OutError.ActualValue = MakeShared<FJsonValueString>(bWorld ? Context.GetWorldIdentity() : Context.GetLevelIdentity());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(bWorld ? WorldIdentity : LevelIdentity);
					return false;
				}
				return true;
			}

		private:
			FUeAgentTimelineSpatialRuntimeConfig Config;
			FString WorldIdentity;
			FString LevelIdentity;
		};

		class IOperationRoute
			: public IUeAgentOperationRequestPreparer
		{
		public:
			virtual ~IOperationRoute() = default;
			virtual bool TryPrepare(
				const FUeAgentContractValue& CanonicalInput,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				TOptional<FPlanningRequest>& OutRequest,
				FUeAgentOperationBindingError& OutError) override = 0;
			virtual IUeAgentOperationPlanner& GetPlanner() const = 0;
			virtual IUeAgentOperationExecutor& GetExecutor() const = 0;
			virtual IUeAgentOperationVerifier& GetVerifier() const = 0;
			virtual bool TryReadRevision(
				const FString& PublicRef,
				FUeAgentStateRevision& OutRevision,
				FDiagnostic& OutError) const = 0;
		};

		class FWorldSpatialOperationRoute final : public IOperationRoute
		{
		public:
			FWorldSpatialOperationRoute(
				TSharedRef<DomainAdapters::WorldSpatial::
					FUeAgentWorldSpatialProductionRuntime> InRuntime,
				TSharedRef<DomainAdapters::WorldSpatial::
					FUeAgentWorldSpatialOperationChain> InChain)
				: Runtime(MoveTemp(InRuntime))
				, Chain(MoveTemp(InChain))
			{
			}
			bool TryPrepare(
				const FUeAgentContractValue& Input,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				TOptional<FPlanningRequest>& Out,
				FUeAgentOperationBindingError& Error) override
			{
				return Chain->TryBuildPlanningRequest(
					Input, Targets, Context, Out, Error);
			}
			IUeAgentOperationPlanner& GetPlanner() const override
			{
				return Chain->GetPlanner();
			}
			IUeAgentOperationExecutor& GetExecutor() const override
			{
				return Chain->GetExecutor();
			}
			IUeAgentOperationVerifier& GetVerifier() const override
			{
				return Chain->GetVerifier();
			}
			bool TryReadRevision(
				const FString& PublicRef,
				FUeAgentStateRevision& OutRevision,
				FDiagnostic& OutError) const override
			{
				DomainAdapters::WorldSpatial::FWorldSpatialRevision Revision;
				DomainAdapters::WorldSpatial::FUeAgentWorldSpatialExecutionError
					Error;
				if (!Runtime->TryReadRevision(PublicRef, Revision, Error))
				{
					OutError = Error;
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					Revision.SourceId,
					Revision.Generation,
					Revision.Sequence);
				if (!OutRevision.IsValid())
				{
					OutError = OutRevision.GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PublicRef;
					return false;
				}
				return true;
			}
		private:
			TSharedRef<DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialProductionRuntime> Runtime;
			TSharedRef<DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialOperationChain> Chain;
		};

		class FTimelineAnimationOperationRoute final : public IOperationRoute
		{
		public:
			FTimelineAnimationOperationRoute(
				TSharedRef<DomainAdapters::TimelineAnimation::
					FUeAgentTimelineAnimationProductionRuntime> InRuntime,
				TSharedRef<DomainAdapters::TimelineAnimation::
					FUeAgentTimelineAnimationOperationChain> InChain)
				: Runtime(MoveTemp(InRuntime))
				, Chain(MoveTemp(InChain))
			{
			}
			bool TryPrepare(
				const FUeAgentContractValue& Input,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				TOptional<FPlanningRequest>& Out,
				FUeAgentOperationBindingError& Error) override
			{
				return Chain->TryBuildPlanningRequest(
					Input, Targets, Context, Out, Error);
			}
			IUeAgentOperationPlanner& GetPlanner() const override
			{
				return Chain->GetPlanner();
			}
			IUeAgentOperationExecutor& GetExecutor() const override
			{
				return Chain->GetExecutor();
			}
			IUeAgentOperationVerifier& GetVerifier() const override
			{
				return Chain->GetVerifier();
			}
			bool TryReadRevision(
				const FString& PublicRef,
				FUeAgentStateRevision& OutRevision,
				FDiagnostic& OutError) const override
			{
				DomainAdapters::TimelineAnimation::FTimelineAnimationRevision
					Revision;
				DomainAdapters::TimelineAnimation::
					FUeAgentTimelineAnimationExecutionError Error;
				if (!Runtime->TryReadRevision(PublicRef, Revision, Error))
				{
					OutError = Error;
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					Revision.SourceId,
					Revision.Generation,
					Revision.Sequence);
				if (!OutRevision.IsValid())
				{
					OutError = OutRevision.GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PublicRef;
					return false;
				}
				return true;
			}
		private:
			TSharedRef<DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationProductionRuntime> Runtime;
			TSharedRef<DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationOperationChain> Chain;
		};

		class FRigSimulationSpatialOperationRoute final : public IOperationRoute
		{
		public:
			FRigSimulationSpatialOperationRoute(
				TSharedRef<DomainAdapters::RigSimulationSpatial::
					FUeAgentRigSimulationSpatialProductionRuntime> InRuntime,
				TSharedRef<DomainAdapters::RigSimulationSpatial::
					FUeAgentRigSimulationSpatialOperationChain> InChain)
				: Runtime(MoveTemp(InRuntime))
				, Chain(MoveTemp(InChain))
			{
			}
			bool TryPrepare(
				const FUeAgentContractValue& Input,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				TOptional<FPlanningRequest>& Out,
				FUeAgentOperationBindingError& Error) override
			{
				return Chain->TryBuildPlanningRequest(
					Input, Targets, Context, Out, Error);
			}
			IUeAgentOperationPlanner& GetPlanner() const override
			{
				return Chain->GetPlanner();
			}
			IUeAgentOperationExecutor& GetExecutor() const override
			{
				return Chain->GetExecutor();
			}
			IUeAgentOperationVerifier& GetVerifier() const override
			{
				return Chain->GetVerifier();
			}
			bool TryReadRevision(
				const FString& PublicRef,
				FUeAgentStateRevision& OutRevision,
				FDiagnostic& OutError) const override
			{
				DomainAdapters::RigSimulationSpatial::
					FRigSimulationSpatialRevision Revision;
				DomainAdapters::RigSimulationSpatial::
					FUeAgentRigSimulationSpatialExecutionError Error;
				if (!Runtime->TryReadRevision(PublicRef, Revision, Error))
				{
					OutError = Error;
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					Revision.SourceId,
					Revision.Generation,
					Revision.Sequence);
				if (!OutRevision.IsValid())
				{
					OutError = OutRevision.GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PublicRef;
					return false;
				}
				return true;
			}
		private:
			TSharedRef<DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialProductionRuntime> Runtime;
			TSharedRef<DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialOperationChain> Chain;
		};
	}

	class FUeAgentTimelineSpatialRuntime::FState final
	{
	public:
		FState(
			FUeAgentTimelineSpatialRuntimeConfig InConfig,
			const TArray<FUeAgentTimelineSpatialActivationProfile>& InProfiles,
			TSharedRef<const FUeAgentDescriptorRegistry> InDescriptors,
			TSharedRef<const FUeAgentOperationTicketService> InTicketService,
			TSharedRef<const FUeAgentQueryTicketService> InQueryTicketService,
			TSharedRef<const FUeAgentResponseProjector> InProjector,
			TSharedRef<const BindingGateway::Query::
				FUeAgentQueryBinding> InQueryBinding,
			FUeAgentOperationSessionIdentity InSession,
			FString InWorldIdentity,
			FString InLevelIdentity,
			TSharedRef<DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialProductionRuntime> InWorldRuntime,
			TSharedRef<DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationProductionRuntime> InTimelineRuntime,
			TSharedRef<DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialProductionRuntime> InRigRuntime,
			TMap<FString, TSharedRef<IOperationRoute>> InOperationRoutes,
			TMap<FString, TSharedRef<const FUeAgentOperationBinding>>
				InOperationBindings)
			: Config(MoveTemp(InConfig))
			, Descriptors(MoveTemp(InDescriptors))
			, TicketService(MoveTemp(InTicketService))
			, QueryTicketService(MoveTemp(InQueryTicketService))
			, Projector(MoveTemp(InProjector))
			, QueryBinding(MoveTemp(InQueryBinding))
			, Session(MoveTemp(InSession))
			, WorldIdentity(MoveTemp(InWorldIdentity))
			, LevelIdentity(MoveTemp(InLevelIdentity))
			, WorldRuntime(MoveTemp(InWorldRuntime))
			, TimelineRuntime(MoveTemp(InTimelineRuntime))
			, RigRuntime(MoveTemp(InRigRuntime))
			, OperationRoutes(MoveTemp(InOperationRoutes))
			, OperationBindings(MoveTemp(InOperationBindings))
		{
			for (const FUeAgentTimelineSpatialActivationProfile& Profile :
				InProfiles)
			{
				ProfileByDescriptor.Add(
					Profile.DescriptorIdentity.GetDescriptorId(),
					&Profile);
			}
		}

		const FUeAgentTimelineSpatialActivationProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity) const
		{
			const FUeAgentTimelineSpatialActivationProfile* const* Profile =
				ProfileByDescriptor.Find(Identity.GetDescriptorId());
			return Profile
				&& (*Profile)->DescriptorIdentity == Identity
					? *Profile
					: nullptr;
		}

		FUeAgentTimelineSpatialRuntimeConfig Config;
		TSharedRef<const FUeAgentDescriptorRegistry> Descriptors;
		TSharedRef<const FUeAgentOperationTicketService> TicketService;
		TSharedRef<const FUeAgentQueryTicketService> QueryTicketService;
		TSharedRef<const FUeAgentResponseProjector> Projector;
		TSharedRef<const BindingGateway::Query::
			FUeAgentQueryBinding> QueryBinding;
		FUeAgentOperationSessionIdentity Session;
		FString WorldIdentity;
		FString LevelIdentity;
		TSharedRef<DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime> WorldRuntime;
		TSharedRef<DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationProductionRuntime> TimelineRuntime;
		TSharedRef<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialProductionRuntime> RigRuntime;
		TMap<FString, TSharedRef<IOperationRoute>> OperationRoutes;
		TMap<FString, TSharedRef<const FUeAgentOperationBinding>>
			OperationBindings;
		TMap<FString, const FUeAgentTimelineSpatialActivationProfile*>
			ProfileByDescriptor;
	};

	namespace
	{
		FUeAgentQueryResult ExecuteQuery(
			const TSharedRef<
				FUeAgentTimelineSpatialRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentQueryRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			const FUeAgentTimelineSpatialActivationProfile* Profile =
				State->FindProfile(Request.GetDescriptorIdentity());
			if (!Request.IsValid()) return QueryErrorResult(Request, Request.GetValidationDiagnostic());
			if (!CallerContext.IsValid()) return QueryErrorResult(Request, CallerContext.GetValidationDiagnostic());
			if (!Profile
				|| Profile->OperationKind != TEXT("query"))
			{
				auto Error = RuntimeFailure(TEXT("timeline_spatial_query_route_invalid"), Request.GetDescriptorIdentity().GetDescriptorId(),
					Profile ? TEXT("The selected command is not registered as a query.") : TEXT("No active query profile matches the request descriptor."));
				Error.PropertyPath = Profile ? TEXT("activation.operation_kind") : TEXT("activation.profile");
				if (Profile)
				{
					Error.ActualValue = MakeShared<FJsonValueString>(Profile->OperationKind);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("query"));
				}
				return QueryErrorResult(Request, MoveTemp(Error));
			}
			const FUeAgentOperationDescriptor* Descriptor =
				State->Descriptors->FindExact(Request.GetDescriptorIdentity());
			if (!Descriptor)
			{
				auto Error = RuntimeFailure(TEXT("timeline_spatial_query_wrapper_invalid"), Request.GetDescriptorIdentity().GetDescriptorId(),
					TEXT("The request descriptor is absent from the active query registry."));
				Error.PropertyPath = TEXT("descriptor_identity");
				return QueryErrorResult(Request, MoveTemp(Error));
			}
			TSharedPtr<FJsonObject> Root;
			FDiagnostic ErrorText;
			FUeAgentContractValue Input(TEXT("{}"));
			FUeAgentExecutionContextInput ExecutionInput;
			FUeAgentProjectionRequest Projection(
				FUeAgentProjectionProfile(TEXT("compact"), false, false, false),
				{},
				{},
				EUeAgentProjectionPageUnit::Entry);
			if (!TryObject(Request.GetPayload().GetJsonValue(), Root, ErrorText)
				|| !TryInputObject(Root, Input, ErrorText)
				|| !TryBuildExecutionContext(
					State->Config,
					*Descriptor,
					Root,
					State->WorldIdentity,
					State->LevelIdentity,
					ExecutionInput,
					ErrorText)
				|| !TryProjection(Root, Projection, ErrorText))
			{
				return QueryErrorResult(Request, MoveTemp(ErrorText));
			}

			TOptional<FUeAgentDomainQueryTarget> Target;
			FString PublicRef;
			Root->TryGetStringField(TEXT("target"), PublicRef);
			PublicRef.TrimStartAndEndInline();
			if (Profile->bRequiresTarget && PublicRef.IsEmpty())
			{
				return QueryErrorResult(Request, InputFailure(TEXT("timeline_spatial_query_target_missing"),
					TEXT("This query requires an explicit target reference."), TEXT("/target"), Root->TryGetField(TEXT("target")), TEXT("non-empty target reference")));
			}
			if (!PublicRef.IsEmpty())
			{
				FUeAgentCandidateBinding Candidate = MakeCandidate(
					Profile->CapabilityId,
					Profile->DescriptorIdentity,
					PublicRef,
					State->Config.ServerSessionId);
				TOptional<FUeAgentQueryTicket> Ticket;
				FUeAgentQueryBindingError TicketError;
				if (!State->QueryTicketService->TryIssue(
						Candidate,
						Profile->DescriptorIdentity,
						State->Session,
						CallerContext,
						{},
						Ticket,
						TicketError)
					|| !Ticket.IsSet())
				{
					if (TicketError.IsEmpty()) TicketError = RuntimeFailure(TEXT("timeline_spatial_query_ticket_missing"), PublicRef,
						Ticket.IsSet() ? TEXT("The query ticket service failed without a diagnostic.") : TEXT("The query ticket service returned no ticket for the target."));
					if (TicketError.TargetPath.IsEmpty()) TicketError.TargetPath = PublicRef;
					return QueryErrorResult(Request, TicketError);
				}
				Target.Emplace(
					Ticket.GetValue(),
					MoveTemp(Candidate));
			}
			TArray<FDiagnostic> Diagnostics;
			FDiagnostic ProjectionDiagnostic;
			const FQueryPublicProjectionDescriptor* PublicProjection =
				FindQueryPublicProjectionDescriptorForCommand(
					Profile->CapabilityId, &ProjectionDiagnostic);
			if (!ProjectionDiagnostic.IsEmpty()) Diagnostics.Add(MoveTemp(ProjectionDiagnostic));
			if (PublicProjection)
			{
				Projection = FUeAgentProjectionRequest(
					Projection.GetProfile(),
					Projection.GetArtifacts(),
					Projection.GetDiagnostics(),
					QueryPublicProjectionPageUnit(*PublicProjection));
			}

			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				Resources =
					MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
			const auto Clock = []()
			{
				return FPlatformTime::Seconds();
			};
			FUeAgentCancellationSource CancellationSource(Clock);
			if (Cancellation->IsCancelled())
			{
				CancellationSource.Request(
					TEXT("protocol_cancellation_requested"));
			}
			const FUeAgentOperationDeadline Deadline;
			const FUeAgentDomainQuery DomainQuery(
				State->Session,
				MoveTemp(ExecutionInput),
				MoveTemp(Input),
				MoveTemp(Target),
				MoveTemp(Projection));
			const FUeAgentQueryBindingRequest TypedRequest(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentQueryBindingPayload(DomainQuery));
			TSharedPtr<const FUeAgentQueryBindingResult> TypedResult;
			FUeAgentQueryBindingError BindingError;
			if (!State->QueryBinding->TrySubmit(
					TypedRequest,
					CallerContext,
					Resources,
					Deadline,
					CancellationSource.GetToken(),
					TypedResult,
					BindingError)
				|| !TypedResult.IsValid())
			{
				if (BindingError.IsEmpty()) BindingError = FDiagnostic::Failure(TEXT("query_execution_failed"),
					TEXT("The query binding returned no result or failure diagnostic."), TEXT("query"));
				BindingError.Diagnostics.Append(Diagnostics);
				return QueryErrorResult(Request, BindingError);
			}
			Diagnostics.Append(TypedResult->GetValue().GetDiagnostics());
			FUeAgentContractValue Encoded(TEXT("{}"));
			const bool bEncoded = PublicProjection
				? FUeAgentQueryPayloadCodec::TryEncodePublic(
					*PublicProjection,
					Profile->CapabilityId,
					Profile->TypedOperationId,
					true,
					TypedResult->GetValue(),
					*Resources,
					Encoded,
					BindingError)
				: FUeAgentQueryPayloadCodec::TryEncode(
					TypedResult->GetValue(),
					*Resources,
					Encoded,
					BindingError);
			if (!bEncoded)
			{
				BindingError.Diagnostics.Append(Diagnostics);
				return QueryErrorResult(Request, BindingError);
			}
			return FUeAgentQueryResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				MoveTemp(Encoded), MoveTemp(Diagnostics));
		}

		FUeAgentOperationResult ExecuteOperation(
			const TSharedRef<
				FUeAgentTimelineSpatialRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			const FUeAgentTimelineSpatialActivationProfile* Profile =
				State->FindProfile(Request.GetDescriptorIdentity());
			TSharedRef<IOperationRoute>* Route =
				State->OperationRoutes.Find(
					Request.GetDescriptorIdentity().GetDescriptorId());
			TSharedRef<const FUeAgentOperationBinding>* OperationBinding =
				State->OperationBindings.Find(
					Request.GetDescriptorIdentity().GetDescriptorId());
			if (!Request.IsValid()) return OperationErrorResult(Request, Request.GetValidationDiagnostic());
			if (!CallerContext.IsValid()) return OperationErrorResult(Request, CallerContext.GetValidationDiagnostic());
			if (!Profile
				|| Profile->OperationKind != TEXT("operation")
				|| !Route
				|| !OperationBinding)
			{
				auto Error = RuntimeFailure(TEXT("timeline_spatial_operation_route_invalid"), Request.GetDescriptorIdentity().GetDescriptorId(),
					!Profile ? TEXT("No active operation profile matches the request descriptor.")
						: Profile->OperationKind != TEXT("operation") ? TEXT("The selected command is not registered as an operation.")
						: !Route ? TEXT("No execution route is registered for this operation.")
						: TEXT("No operation binding is registered for this command."));
				Error.PropertyPath = !Profile ? TEXT("activation.profile") : Profile->OperationKind != TEXT("operation")
					? TEXT("activation.operation_kind") : !Route ? TEXT("operation_route") : TEXT("operation_binding");
				if (Profile && Profile->OperationKind != TEXT("operation"))
				{
					Error.ActualValue = MakeShared<FJsonValueString>(Profile->OperationKind);
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("operation"));
				}
				return OperationErrorResult(Request, MoveTemp(Error));
			}
			const FUeAgentOperationDescriptor* Descriptor =
				State->Descriptors->FindExact(Request.GetDescriptorIdentity());
			if (!Descriptor)
			{
				auto Error = RuntimeFailure(TEXT("timeline_spatial_operation_wrapper_invalid"), Request.GetDescriptorIdentity().GetDescriptorId(),
					TEXT("The request descriptor is absent from the active operation registry."));
				Error.PropertyPath = TEXT("descriptor_identity");
				return OperationErrorResult(Request, MoveTemp(Error));
			}
			TSharedPtr<FJsonObject> Root;
			FDiagnostic ErrorText;
			FUeAgentContractValue Input(TEXT("{}"));
			FUeAgentExecutionContextInput ExecutionInput;
			FUeAgentProjectionRequest Projection(
				FUeAgentProjectionProfile(TEXT("compact"), false, false, false),
				{},
				{},
				EUeAgentProjectionPageUnit::Entry);
			if (!TryObject(Request.GetPayload().GetJsonValue(), Root, ErrorText)
				|| !TryInputObject(Root, Input, ErrorText)
				|| !TryBuildExecutionContext(
					State->Config,
					*Descriptor,
					Root,
					State->WorldIdentity,
					State->LevelIdentity,
					ExecutionInput,
					ErrorText)
				|| !TryProjection(Root, Projection, ErrorText))
			{
				return OperationErrorResult(Request, MoveTemp(ErrorText));
			}

			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				Resources =
					MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
			const auto Clock = []()
			{
				return FPlatformTime::Seconds();
			};
			FUeAgentCancellationSource CancellationSource(Clock);
			if (Cancellation->IsCancelled())
			{
				CancellationSource.Request(
					TEXT("protocol_cancellation_requested"));
			}
			const FUeAgentCancellationToken CancellationToken =
				CancellationSource.GetToken();
			const FUeAgentOperationDeadline Deadline;

			FDiagnostic RevisionError;
			TArray<FUeAgentOperationTargetBinding> Targets;
			FString PublicRef;
			Root->TryGetStringField(TEXT("target"), PublicRef);
			PublicRef.TrimStartAndEndInline();
			if (Profile->bRequiresTarget && PublicRef.IsEmpty())
			{
				return OperationErrorResult(Request, InputFailure(TEXT("timeline_spatial_operation_target_missing"),
					TEXT("This operation requires an explicit target reference."), TEXT("/target"), Root->TryGetField(TEXT("target")), TEXT("non-empty target reference")));
			}
			if (!PublicRef.IsEmpty())
			{
				FUeAgentStateRevision Revision(
					TEXT("invalid"),
					TEXT("invalid"),
					-1);
				if (!(*Route)->TryReadRevision(
						PublicRef,
						Revision,
					RevisionError))
				{
					return OperationErrorResult(Request, !RevisionError.IsEmpty() ? RevisionError : RuntimeFailure(TEXT("timeline_spatial_revision_read_failed"), PublicRef, TEXT("The target revision could not be read.")));
				}
				FUeAgentCandidateBinding Candidate = MakeCandidate(
					Profile->CapabilityId,
					Profile->DescriptorIdentity,
					PublicRef,
					Revision.GetGeneration());
				TOptional<FUeAgentOperationTicket> Ticket;
				FUeAgentOperationTicketError TicketError;
				if (!State->TicketService->TryIssue(
						Candidate,
						Profile->DescriptorIdentity,
						State->Session,
						CallerContext,
						Revision,
						Ticket,
						TicketError)
					|| !Ticket.IsSet())
				{
					if (TicketError.IsEmpty()) TicketError = RuntimeFailure(TEXT("timeline_spatial_operation_ticket_missing"), PublicRef,
						Ticket.IsSet() ? TEXT("The operation ticket service failed without a diagnostic.") : TEXT("The operation ticket service returned no ticket for the target."));
					if (TicketError.TargetPath.IsEmpty()) TicketError.TargetPath = PublicRef;
					return OperationErrorResult(Request, TicketError);
				}
				Targets.Emplace(Ticket.GetValue(), MoveTemp(Candidate));
			}

			const FUeAgentOperationBindingRequest TypedRequest(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Request.GetIdempotencyKey(),
				FUeAgentOperationBindingPayload(
					State->Session,
					MoveTemp(Targets),
					ExecutionInput,
					Input,
					*Route,
					Projection));
			TSharedPtr<const FUeAgentOperationBindingResult>
				BindingResult;
			FUeAgentOperationBindingError BindingError;
			if (!(*OperationBinding)->TrySubmit(
					TypedRequest,
					CallerContext,
					Resources,
					Deadline,
					CancellationToken,
					BindingResult,
					BindingError)
				|| !BindingResult.IsValid())
			{
				if (BindingError.IsEmpty()) BindingError = RuntimeFailure(TEXT("timeline_spatial_operation_failed"), Request.GetRequestId(),
					BindingResult.IsValid() ? TEXT("The operation binding failed without a primary diagnostic.") : TEXT("The operation binding returned no result or failure diagnostic."));
				if (BindingResult.IsValid()) BindingError.Diagnostics.Append(BindingResult->GetValue().GetDiagnostics());
				return OperationErrorResult(Request, MoveTemp(BindingError));
			}
			TArray<FDiagnostic> Diagnostics = BindingResult->GetValue().GetDiagnostics();
			if (!BindingResult->IsValid())
			{
				FDiagnostic Error;
				if (!BindingResult->GetDescriptorIdentity().IsValid()) Error = BindingResult->GetDescriptorIdentity().GetValidationDiagnostic();
				else if (BindingResult->GetRequestId().IsEmpty())
				{
					Error = RuntimeFailure(TEXT("timeline_spatial_operation_result_invalid"), Request.GetRequestId(), TEXT("The operation binding result has an empty request identifier."));
					Error.PropertyPath = TEXT("request_id");
				}
				else Error = BindingResult->GetValue().GetValidationDiagnostic();
				Error.Diagnostics.Append(Diagnostics);
				return OperationErrorResult(Request, MoveTemp(Error));
			}
			const TSharedPtr<const FUeAgentProjectedResponse>& Projected =
				BindingResult->GetValue().GetProjection();
			if (!Projected.IsValid()
				|| !Projected->GetPayload().GetValue().IsValid())
			{
				FDiagnostic Error = RuntimeFailure(TEXT("timeline_spatial_projection_missing"), Request.GetRequestId(),
					!Projected.IsValid() ? TEXT("The operation result contains no response projection.") : TEXT("The response projection contains no canonical payload."));
				Error.PropertyPath = !Projected.IsValid() ? TEXT("projection") : TEXT("projection.payload");
				Error.ActualValuePresent = false;
				Error.Diagnostics.Append(Diagnostics);
				return OperationErrorResult(Request, MoveTemp(Error));
			}
			const TSharedRef<FJsonObject> ProjectionJson =
				MakeShared<FJsonObject>();
			ProjectionJson->SetStringField(
				TEXT("profile_id"),
				Projected->GetProfileId());
			ProjectionJson->SetField(
				TEXT("payload"),
				CanonicalValueToJson(
					*Projected->GetPayload().GetValue()));
			const TSharedRef<FJsonObject> RootResult =
				MakeShared<FJsonObject>();
			RootResult->SetStringField(
				TEXT("operation_id"),
				BindingResult->GetValue().GetOperationId());
			RootResult->SetObjectField(
				TEXT("projection"),
				ProjectionJson);
			FString Text;
			FDiagnostic JsonError;
			if (!Json::TryCanonicalize(RootResult, Text, JsonError))
			{
				JsonError.Diagnostics.Append(Diagnostics);
				return OperationErrorResult(Request, MoveTemp(JsonError));
			}
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(MoveTemp(Text)), MoveTemp(Diagnostics));
		}

		class FQueryApplication final : public IUeAgentQueryApplicationPort
		{
		public:
			explicit FQueryApplication(
				TSharedRef<
					FUeAgentTimelineSpatialRuntime::FState,
					ESPMode::ThreadSafe> InState)
				: State(MoveTemp(InState))
			{
			}

			FUeAgentQueryResult Submit(
				const FUeAgentQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (IsInGameThread())
				{
					return ExecuteQuery(
						State,
						Request,
						CallerContext,
						Cancellation);
				}
				const TSharedRef<
					TOptional<FUeAgentQueryResult>,
					ESPMode::ThreadSafe> Result =
						MakeShared<
							TOptional<FUeAgentQueryResult>,
							ESPMode::ThreadSafe>();
				FEvent* Completed =
					FPlatformProcess::GetSynchEventFromPool(true);
				const auto StateRef = State;
				AsyncTask(
					ENamedThreads::GameThread,
					[
						StateRef,
						Request,
						CallerContext,
						Cancellation,
						Result,
						Completed
					]()
					{
						Result->Emplace(ExecuteQuery(
							StateRef,
							Request,
							CallerContext,
							Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(Completed);
				return Result->GetValue();
			}
		private:
			TSharedRef<
				FUeAgentTimelineSpatialRuntime::FState,
				ESPMode::ThreadSafe> State;
		};

		class FOperationApplication final
			: public IUeAgentOperationApplicationPort
		{
		public:
			explicit FOperationApplication(
				TSharedRef<
					FUeAgentTimelineSpatialRuntime::FState,
					ESPMode::ThreadSafe> InState)
				: State(MoveTemp(InState))
			{
			}

			FUeAgentOperationResult Submit(
				const FUeAgentOperationRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (IsInGameThread())
				{
					return ExecuteOperation(
						State,
						Request,
						CallerContext,
						Cancellation);
				}
				const TSharedRef<
					TOptional<FUeAgentOperationResult>,
					ESPMode::ThreadSafe> Result =
						MakeShared<
							TOptional<FUeAgentOperationResult>,
							ESPMode::ThreadSafe>();
				FEvent* Completed =
					FPlatformProcess::GetSynchEventFromPool(true);
				const auto StateRef = State;
				AsyncTask(
					ENamedThreads::GameThread,
					[
						StateRef,
						Request,
						CallerContext,
						Cancellation,
						Result,
						Completed
					]()
					{
						Result->Emplace(ExecuteOperation(
							StateRef,
							Request,
							CallerContext,
							Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(Completed);
				return Result->GetValue();
			}
		private:
			TSharedRef<
				FUeAgentTimelineSpatialRuntime::FState,
				ESPMode::ThreadSafe> State;
		};
	}

	bool FUeAgentTimelineSpatialRuntimeConfig::IsValid() const
	{
		return ValidateRuntimeConfig(*this).IsEmpty();
	}

	FUeAgentTimelineSpatialRuntime::FUeAgentTimelineSpatialRuntime(
		TSharedRef<FState, ESPMode::ThreadSafe> InState,
		TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
		TSharedRef<IUeAgentOperationApplicationPort> InOperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
			InCommandHandlers)
		: State(MoveTemp(InState))
		, QueryPort(MoveTemp(InQueryPort))
		, OperationPort(MoveTemp(InOperationPort))
		, CommandHandlers(MoveTemp(InCommandHandlers))
		, AuthorityScopes({
			TEXT("uai:timeline-spatial:read"),
			TEXT("uai:timeline-spatial:write"),
			TEXT("world-spatial:write"),
			TEXT("timeline-animation:write"),
			TEXT("rig-simulation-spatial:write"),
		})
	{
	}

	bool FUeAgentTimelineSpatialRuntime::TryCreate(
		const FUeAgentTimelineSpatialRuntimeConfig& Config,
		UWorld& ExplicitWorld,
		TSharedRef<const FUeAgentOperationTicketService> TicketService,
		TSharedRef<const FUeAgentDiscoveryService> Discovery,
		TSharedRef<const FUeAgentResponseProjector> Projector,
		TSharedPtr<FUeAgentTimelineSpatialRuntime>& OutRuntime,
		FUeAgentTimelineSpatialRuntimeError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		OutError = ValidateRuntimeConfig(Config);
		if (!OutError.IsEmpty()) return false;
		if (!IsValid(&ExplicitWorld))
		{
			SetError(OutError, FDiagnostic::Failure(TEXT("timeline_spatial_runtime_config_invalid"),
				TEXT("The explicit runtime world is unavailable."), TEXT("runtime")), TEXT("timeline_spatial"));
			OutError.PropertyPath = TEXT("explicit_world");
			return false;
		}

		const TArray<FUeAgentTimelineSpatialActivationProfile>* Profiles = nullptr;
		if (!TryGetTimelineSpatialActivationProfiles(Profiles, OutError)) return false;
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		UE::UAI::Contract::FDiagnostic ErrorText;
		if (!TryCreateTimelineSpatialDescriptorRegistry(
				Descriptors,
				ErrorText)
			|| !Descriptors.IsValid())
		{
			SetError(OutError, ErrorText, TEXT("timeline_spatial"));
			return false;
		}

		DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntimeConfig WorldConfig;
		WorldConfig.ProjectInstanceId = Config.ProjectInstanceId;
		WorldConfig.EditorInstanceId = Config.EditorInstanceId;
		WorldConfig.ServerSessionId = Config.ServerSessionId;
		WorldConfig.RegistryEpoch = Config.RegistryEpoch;
		TSharedPtr<DomainAdapters::WorldSpatial::
			FUeAgentWorldSpatialProductionRuntime> WorldRuntime;
		DomainAdapters::WorldSpatial::FUeAgentWorldSpatialExecutionError
			WorldError;
		if (!DomainAdapters::WorldSpatial::
				FUeAgentWorldSpatialProductionRuntime::TryCreate(
					WorldConfig,
					ExplicitWorld,
					WorldRuntime,
					WorldError)
			|| !WorldRuntime.IsValid())
		{
			OutError = WorldError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = WorldError.SubjectId;
			return false;
		}

		DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationProductionRuntimeConfig TimelineConfig;
		TimelineConfig.ProjectInstanceId = Config.ProjectInstanceId;
		TimelineConfig.EditorInstanceId = Config.EditorInstanceId;
		TimelineConfig.ServerSessionId = Config.ServerSessionId;
		TimelineConfig.RegistryEpoch = Config.RegistryEpoch;
		TimelineConfig.SubjectBindings = Config.TimelineSubjectBindings;
		TimelineConfig.RigCandidateBindings = Config.TimelineRigCandidateBindings;
		TSharedPtr<DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationProductionRuntime> TimelineRuntime;
		DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationExecutionError TimelineError;
		if (!DomainAdapters::TimelineAnimation::
				FUeAgentTimelineAnimationProductionRuntime::TryCreate(
					TimelineConfig,
					TimelineRuntime,
					TimelineError)
			|| !TimelineRuntime.IsValid())
		{
			OutError = TimelineError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TimelineError.SubjectId;
			return false;
		}

		DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialProductionRuntimeConfig RigConfig;
		RigConfig.ProjectInstanceId = Config.ProjectInstanceId;
		RigConfig.EditorInstanceId = Config.EditorInstanceId;
		RigConfig.ServerSessionId = Config.ServerSessionId;
		RigConfig.RuntimeGeneration =
			Config.ServerSessionId
				+ TEXT(":")
				+ FString::Printf(TEXT("%lld"), Config.RegistryEpoch);
		RigConfig.RegistryEpoch = Config.RegistryEpoch;
		RigConfig.InputRootPointer = TEXT("/input");
		RigConfig.ObjectBindings = Config.RigObjectBindings;
		RigConfig.ClockBindings = Config.RigClockBindings;
		RigConfig.FrameBindings = Config.RigFrameBindings;
		RigConfig.SkeletonBindings = Config.RigSkeletonBindings;
		RigConfig.SocketBindings = Config.RigSocketBindings;
		RigConfig.RigBakeBindings = Config.RigBakeBindings;
		RigConfig.IkRigBindings = Config.RigIkBindings;
		RigConfig.RetargeterBindings = Config.RigRetargeterBindings;
		RigConfig.AssetEffectPort = Config.RigAssetEffectPort;
		RigConfig.PhysicsSessionPort = Config.RigPhysicsSessionPort;
		TSharedPtr<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialProductionRuntime> RigRuntime;
		DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialExecutionError RigError;
		if (!DomainAdapters::RigSimulationSpatial::
				FUeAgentRigSimulationSpatialProductionRuntime::TryCreate(
					RigConfig,
					RigRuntime,
					RigError)
			|| !RigRuntime.IsValid())
		{
			OutError = RigError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RigError.SubjectId;
			return false;
		}

		TArray<FUeAgentDomainQueryProviderRegistration>
			QueryRegistrations;
		TMap<FString, TSharedRef<IOperationRoute>> OperationRoutes;
		for (const FUeAgentTimelineSpatialActivationProfile& Profile :
			*Profiles)
		{
			if (Profile.OperationKind == TEXT("query"))
			{
				TSharedPtr<IUeAgentDomainQueryProvider> Provider;
				if (Profile.AdapterId == TEXT("world_spatial"))
				{
					Provider = WorldRuntime->GetQueryProviderRegistry()
						->FindExact(Profile.ProviderBinding);
				}
				else if (Profile.AdapterId == TEXT("timeline_animation"))
				{
					Provider = TimelineRuntime->GetQueryProviderRegistry()
						->FindExact(Profile.ProviderBinding);
				}
				else
				{
					Provider = RigRuntime->GetQueryProviderRegistry()
						->FindExact(Profile.ProviderBinding);
				}
				if (!Provider.IsValid())
				{
					SetError(OutError, FDiagnostic::Failure(TEXT("timeline_spatial_query_provider_missing"),
						TEXT("No query provider is registered for the command's provider binding."), TEXT("runtime")), Profile.CapabilityId);
					OutError.PropertyPath = TEXT("provider_binding");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Profile.ProviderBinding);
					return false;
				}
				QueryRegistrations.Emplace(
					Profile.ProviderBinding,
					Provider.ToSharedRef());
				continue;
			}

			if (Profile.AdapterId == TEXT("world_spatial"))
			{
				const TSharedRef<DomainAdapters::WorldSpatial::
					FUeAgentWorldSpatialOperationChain>* Chain =
						WorldRuntime->GetOperationChains().FindByPredicate(
							[&Profile](const auto& Candidate)
							{
								return Candidate->GetContract()
									.GetCapabilityId()
										== Profile.CapabilityId;
							});
				if (Chain)
				{
					OperationRoutes.Add(
						Profile.DescriptorIdentity.GetDescriptorId(),
						MakeShared<FWorldSpatialOperationRoute>(
							WorldRuntime.ToSharedRef(),
							*Chain));
				}
			}
			else if (Profile.AdapterId == TEXT("timeline_animation"))
			{
				const TSharedRef<DomainAdapters::TimelineAnimation::
					FUeAgentTimelineAnimationOperationChain>* Chain =
						TimelineRuntime->GetOperationChains().FindByPredicate(
							[&Profile](const auto& Candidate)
							{
								return Candidate->GetContract()
									.GetCapabilityId()
										== Profile.CapabilityId;
							});
				if (Chain)
				{
					OperationRoutes.Add(
						Profile.DescriptorIdentity.GetDescriptorId(),
						MakeShared<FTimelineAnimationOperationRoute>(
							TimelineRuntime.ToSharedRef(),
							*Chain));
				}
			}
			else
			{
				const TSharedRef<DomainAdapters::RigSimulationSpatial::
					FUeAgentRigSimulationSpatialOperationChain>* Chain =
						RigRuntime->GetOperationChains().FindByPredicate(
							[&Profile](const auto& Candidate)
							{
								return Candidate->GetContract()
									.GetCapabilityId()
										== Profile.CapabilityId;
							});
				if (Chain)
				{
					OperationRoutes.Add(
						Profile.DescriptorIdentity.GetDescriptorId(),
						MakeShared<FRigSimulationSpatialOperationRoute>(
							RigRuntime.ToSharedRef(),
							*Chain));
				}
			}
		}
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> QueryProviders;
		FUeAgentQueryBindingError QueryError;
		if (!FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(QueryRegistrations),
				QueryProviders,
				QueryError)
			|| !QueryProviders.IsValid())
		{
			OutError = QueryError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = QueryError.SubjectId;
			return false;
		}
		TSharedPtr<const FUeAgentQueryTicketService> QueryTicketService;
		if (!FUeAgentQueryTicketService::TryCreateFromShared(
				TicketService,
				QueryTicketService,
				QueryError)
			|| !QueryTicketService.IsValid())
		{
			OutError = QueryError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = QueryError.SubjectId;
			return false;
		}
		const FUeAgentOperationSessionIdentity Session(
			Config.ServerSessionId,
			Config.RegistryEpoch,
			Descriptors->GetRegistryHash());
		const TSharedRef<FTimelineSpatialSessionSource> SessionSource =
			MakeShared<FTimelineSpatialSessionSource>(Session);
		TSharedPtr<const BindingGateway::Query::
			FUeAgentQueryBinding> QueryBinding;
		if (!BindingGateway::Query::FUeAgentQueryBinding::TryCreate(
				Descriptors.ToSharedRef(),
				QueryProviders.ToSharedRef(),
				Discovery,
				Projector,
				SessionSource,
				QueryTicketService.ToSharedRef(),
				QueryBinding,
				QueryError)
			|| !QueryBinding.IsValid())
		{
			OutError = QueryError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = QueryError.SubjectId;
			return false;
		}

		const FString WorldIdentity = ExplicitWorld.GetPathName();
		const FString LevelIdentity =
			IsValid(ExplicitWorld.PersistentLevel)
				? ExplicitWorld.PersistentLevel->GetPathName()
				: FString();
		const TSharedRef<FTimelineSpatialExecutionContextValidator>
			ContextValidator =
				MakeShared<FTimelineSpatialExecutionContextValidator>(
					Config,
					WorldIdentity,
					LevelIdentity);
		TMap<FString, TSharedRef<const FUeAgentOperationBinding>>
			OperationBindings;
		for (const FUeAgentTimelineSpatialActivationProfile& Profile :
			*Profiles)
		{
			if (Profile.OperationKind != TEXT("operation"))
			{
				continue;
			}
			const TSharedRef<IOperationRoute>* Route =
				OperationRoutes.Find(
					Profile.DescriptorIdentity.GetDescriptorId());
			if (!Route)
			{
				SetError(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_spatial_operation_route_missing"), TEXT("Exact operation route is required."), TEXT("runtime")), Profile.CapabilityId);
				OutError.PropertyPath = TEXT("operation_route");
				return false;
			}
			TSharedPtr<const FUeAgentOperationBinding> Binding;
			FUeAgentOperationBindingError BindingError;
			if (!FUeAgentOperationBinding::TryCreate(
					Descriptors.ToSharedRef(),
					Profile.DescriptorIdentity,
					ContextValidator,
					MakeShareable(
						&(*Route)->GetPlanner(),
						[](IUeAgentOperationPlanner*) {}),
					MakeShareable(
						&(*Route)->GetExecutor(),
						[](IUeAgentOperationExecutor*) {}),
					MakeShareable(
						&(*Route)->GetVerifier(),
						[](IUeAgentOperationVerifier*) {}),
					Projector,
					SessionSource,
					TicketService,
					Binding,
					BindingError)
				|| !Binding.IsValid())
			{
				OutError = BindingError.IsValid() ? BindingError : FDiagnostic::Failure(TEXT("timeline_spatial_operation_binding_invalid"),
					TEXT("The operation binding factory returned no valid binding or failure diagnostic."), TEXT("runtime"));
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("operation_binding");
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Profile.CapabilityId;
				return false;
			}
			OperationBindings.Add(
				Profile.DescriptorIdentity.GetDescriptorId(),
				Binding.ToSharedRef());
		}
		const TSharedRef<FState, ESPMode::ThreadSafe> State =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				*Profiles,
				Descriptors.ToSharedRef(),
				TicketService,
				QueryTicketService.ToSharedRef(),
				Projector,
				QueryBinding.ToSharedRef(),
				Session,
				WorldIdentity,
				LevelIdentity,
				WorldRuntime.ToSharedRef(),
				TimelineRuntime.ToSharedRef(),
				RigRuntime.ToSharedRef(),
				MoveTemp(OperationRoutes),
				MoveTemp(OperationBindings));
		const TSharedRef<IUeAgentQueryApplicationPort> QueryPort =
			MakeShared<FQueryApplication>(State);
		const TSharedRef<IUeAgentOperationApplicationPort> OperationPort =
			MakeShared<FOperationApplication>(State);
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
		if (!TryCreateTimelineSpatialCommandHandlers(
				*Profiles,
				QueryPort,
				OperationPort,
				Handlers,
				ErrorText))
		{
			SetError(OutError, ErrorText, TEXT("timeline_spatial"));
			return false;
		}
		OutRuntime = MakeShareable(
			new FUeAgentTimelineSpatialRuntime(
				State,
				QueryPort,
				OperationPort,
				MoveTemp(Handlers)));
		return true;
	}

	const TSharedRef<IUeAgentQueryApplicationPort>&
	FUeAgentTimelineSpatialRuntime::GetQueryPort() const
	{
		return QueryPort;
	}

	const TSharedRef<IUeAgentOperationApplicationPort>&
	FUeAgentTimelineSpatialRuntime::GetOperationPort() const
	{
		return OperationPort;
	}

	const TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
	FUeAgentTimelineSpatialRuntime::GetCommandHandlers() const
	{
		return CommandHandlers;
	}

	const TArray<FString>&
	FUeAgentTimelineSpatialRuntime::GetAuthorityScopes() const
	{
		return AuthorityScopes;
	}

	int32 FUeAgentTimelineSpatialRuntime::GetQueryBindingCount() const
	{
		return 24;
	}

	int32 FUeAgentTimelineSpatialRuntime::GetOperationBindingCount() const
	{
		return State->OperationBindings.Num();
	}
}
