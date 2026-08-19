#include "Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Async/Async.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "PCGGraph.h"
#include "Sound/SoundCue.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::Composition::ReadExportCommand
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool MergeDescriptors(
			const TSharedPtr<const FUeAgentDescriptorRegistry>& Registry,
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			FString& OutError)
		{
			if (!Registry.IsValid())
			{
				OutError = TEXT("read_export_descriptor_source_missing");
				return false;
			}
			OutDescriptors.Append(Registry->GetDescriptors());
			return true;
		}

		FString CanonicalObject(
			const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error =
				MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(
				TEXT("subject_id"),
				SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			return CanonicalObject(Error);
		}

		FString ErrorJson(const FUeAgentQueryBindingError& Error)
		{
			return ErrorJson(
				Error.Code,
				Error.SubjectId,
				Error.Detail);
		}

		FUeAgentResourceBudget MakeCommandBudget(
			const int64 MaxFrameBytes,
			const bool bExport)
		{
			const int64 ByteLimit = bExport
				? FMath::Max<int64>(
					MaxFrameBytes * 64,
					256ll * 1024 * 1024)
				: FMath::Max<int64>(
					MaxFrameBytes * 8,
					16ll * 1024 * 1024);
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, ByteLimit},
				{EUeAgentResourceDimension::OutputBytes, ByteLimit},
				{EUeAgentResourceDimension::StringBytes, ByteLimit},
				{EUeAgentResourceDimension::Nodes,
					bExport ? 1ll << 30 : 262144},
				{EUeAgentResourceDimension::Items,
					bExport ? 1ll << 30 : 262144},
				{EUeAgentResourceDimension::Files,
					bExport ? 1ll << 20 : 256},
				{EUeAgentResourceDimension::ReferenceEdges,
					bExport ? 1ll << 30 : 262144},
				{EUeAgentResourceDimension::WorkUnits,
					bExport ? 1ll << 32 : 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					bExport ? 1ll << 36 : ByteLimit * 4},
			});
		}

		FUeAgentResourceTracker MakeCommandResources(
			const int64 MaxFrameBytes,
			const bool bExport)
		{
			return FUeAgentResourceTracker(
				MakeCommandBudget(MaxFrameBytes, bExport));
		}

		EUeAgentExecutionWorldType WorldType(const UWorld* World)
		{
			if (!World)
			{
				return EUeAgentExecutionWorldType::Editor;
			}
			switch (World->WorldType)
			{
			case EWorldType::PIE:
				return EUeAgentExecutionWorldType::Pie;
			case EWorldType::Game:
				return EUeAgentExecutionWorldType::Game;
			case EWorldType::GamePreview:
			case EWorldType::EditorPreview:
				return EUeAgentExecutionWorldType::Preview;
			default:
				return EUeAgentExecutionWorldType::Editor;
			}
		}

		TArray<FString> StringArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field)
		{
			TArray<FString> Result;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values)
			{
				return Result;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (Value.IsValid()
					&& Value->Type == EJson::String)
				{
					Result.Add(Value->AsString());
				}
			}
			return Result;
		}

		FUeAgentExecutionContextInput ExecutionContext(
			const FUeAgentQueryDescriptor& Descriptor,
			const TSharedPtr<FJsonObject>& Overrides)
		{
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = FApp::GetProjectName();
			Input.EditorInstanceId = FString::Printf(
				TEXT("%s:%u"),
				FPlatformProcess::ComputerName(),
				FPlatformProcess::GetCurrentProcessId());
			const TArray<EUeAgentStatePlane>& Planes =
				Descriptor.GetExecutionContextPolicy()
					.GetAllowedStatePlanes();
			Input.StatePlane = Planes.IsEmpty()
				? EUeAgentStatePlane::AuthoringMemory
				: Planes[0];
			UWorld* World = GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
			Input.WorldType = WorldType(World);
			if (World)
			{
				Input.WorldIdentity = World->GetPathName();
				if (World->PersistentLevel)
				{
					Input.LevelIdentity =
						World->PersistentLevel->GetPathName();
				}
			}
			if (GEditor)
			{
				if (USelection* Selection =
					GEditor->GetSelectedActors())
				{
					for (FSelectionIterator It(*Selection);
						It;
						++It)
					{
						if (const AActor* Actor =
							Cast<AActor>(*It))
						{
							Input.SelectionIdentities.Add(
								Actor->GetPathName());
						}
					}
					Input.SelectionIdentities.Sort();
				}
			}
			Input.ContextRevision = FString::Printf(
				TEXT("%s:%llu"),
				World
					? *World->GetOutermost()->GetName()
					: TEXT("no-world"),
				FPlatformTime::Cycles64());
			Input.bResolvedFromCurrent = true;

			if (!Overrides.IsValid())
			{
				return Input;
			}
			FString Text;
			if (Overrides->TryGetStringField(
				TEXT("state_plane"),
				Text))
			{
				EUeAgentStatePlane Parsed =
					EUeAgentStatePlane::Invalid;
				if (TryParseStatePlane(Text, Parsed))
				{
					Input.StatePlane = Parsed;
				}
			}
			Overrides->TryGetStringField(
				TEXT("world_identity"),
				Input.WorldIdentity);
			Overrides->TryGetStringField(
				TEXT("level_identity"),
				Input.LevelIdentity);
			Overrides->TryGetStringField(
				TEXT("asset_editor_identity"),
				Input.AssetEditorIdentity);
			Overrides->TryGetStringField(
				TEXT("viewport_identity"),
				Input.ViewportIdentity);
			Overrides->TryGetStringField(
				TEXT("context_revision"),
				Input.ContextRevision);
			const TArray<FString> Selection =
				StringArray(
					Overrides,
					TEXT("selection_identities"));
			if (!Selection.IsEmpty())
			{
				Input.SelectionIdentities = Selection;
			}
			return Input;
		}

		FUeAgentProjectionRequest ProjectionRequest(
			const TSharedPtr<FJsonObject>& Object,
			const bool bExport)
		{
			FString ProfileId =
				bExport ? TEXT("compact") : TEXT("compact");
			bool bIncludeDefaults = false;
			bool bIncludeDetails = false;
			bool bIncludeProvenance = false;
			int32 MaxItems = bExport ? 1 : 200;
			FString Cursor;
			TOptional<int64> TokenBudget;
			if (Object.IsValid())
			{
				Object->TryGetStringField(
					TEXT("profile_id"),
					ProfileId);
				Object->TryGetBoolField(
					TEXT("include_defaults"),
					bIncludeDefaults);
				Object->TryGetBoolField(
					TEXT("include_details"),
					bIncludeDetails);
				Object->TryGetBoolField(
					TEXT("include_provenance"),
					bIncludeProvenance);
				double Number = 0.0;
				if (Object->TryGetNumberField(
					TEXT("max_items"),
					Number)
					&& Number > 0.0
					&& Number <= MAX_int32)
				{
					MaxItems = static_cast<int32>(Number);
				}
				Object->TryGetStringField(
					TEXT("continuation_token"),
					Cursor);
				if (Object->TryGetNumberField(
					TEXT("token_budget"),
					Number)
					&& Number > 0.0)
				{
					TokenBudget = static_cast<int64>(Number);
				}
			}
			return FUeAgentProjectionRequest(
				FUeAgentProjectionProfile(
					ProfileId,
					bIncludeDefaults,
					bIncludeDetails,
					bIncludeProvenance),
				FUeAgentPageRequest(MaxItems, Cursor),
				TokenBudget,
				{},
				{});
		}

		UObject* ResolveTargetObject(const FString& Target)
		{
			if (UObject* Existing = StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*Target))
			{
				return Existing;
			}
			return StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*Target);
		}

		FString TargetCompositionKind(UObject* Object)
		{
			if (Object && Object->IsA<UAnimBlueprint>())
			{
				return TEXT("ue.anim_blueprint.asset");
			}
			if (Object && Object->IsA<UWidgetBlueprint>())
			{
				return TEXT("ue.widget_blueprint.asset");
			}
			if (Object && Object->IsA<UBlueprint>())
			{
				return TEXT("ue.blueprint.asset");
			}
			if (Object && Object->IsA<UMaterialInterface>())
			{
				return TEXT("ue.material.asset");
			}
			if (Object && Object->IsA<UNiagaraSystem>())
			{
				return TEXT("ue.niagara_system.asset");
			}
			if (Object && Object->IsA<UNiagaraEmitter>())
			{
				return TEXT("ue.niagara_emitter.asset");
			}
			if (Object && Object->IsA<UNiagaraScript>())
			{
				return TEXT("ue.niagara_script.asset");
			}
			if (Object && Object->IsA<UPCGGraph>())
			{
				return TEXT("ue.pcg_graph.asset");
			}
			if (Object && Object->IsA<USoundCue>())
			{
				return TEXT("ue.sound_cue.asset");
			}
			if (Object && Object->IsA<UAnimationAsset>())
			{
				return TEXT("ue.animation_sequence.asset");
			}
			return TEXT("ue.generic.object");
		}

		bool TryTarget(
			const FString& PublicRef,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FUeAgentReadExportCommandHandlerConfig& Config,
			const FUeAgentCallerContext& CallerContext,
			TOptional<FUeAgentDomainQueryTarget>& OutTarget,
			FUeAgentQueryBindingError& OutError)
		{
			OutTarget.Reset();
			const FUeAgentCandidateBinding Binding(
				TEXT("public-command:") + PublicRef,
				FUeAgentCandidateSummary(
					FUeAgentCanonicalIdentity(
						TEXT("public-command-target:")
							+ PublicRef,
						TEXT("object"),
						PublicRef,
						TEXT("target"),
						TEXT("target"),
						TEXT("current")),
					DescriptorIdentity,
					TEXT("public_target"),
					FPaths::GetCleanFilename(PublicRef),
					TEXT("Exact public command target"),
					{TEXT("UAI"), TEXT("Target")}),
				FUeAgentCatalogRevision(
					TEXT("public-command-target"),
					TEXT("current"),
					0),
				PublicRef,
				TEXT("read-export-production"),
				{EUeAgentCandidateOperation::Read},
				TEXT("public_command_exact_target"));
			TOptional<FUeAgentQueryTicket> Ticket;
			if (!Binding.IsValid()
				|| !Config.TicketService->TryIssue(
					Binding,
					DescriptorIdentity,
					Config.SessionSource->GetCurrent(),
					CallerContext,
					{},
					Ticket,
					OutError)
				|| !Ticket.IsSet())
			{
				if (OutError.Code.IsEmpty())
				{
					OutError.Code =
						TEXT("read_export_target_ticket_failed");
					OutError.SubjectId = PublicRef;
				}
				return false;
			}
			OutTarget.Emplace(
				Ticket.GetValue(),
				Binding);
			return OutTarget->IsValid();
		}

		bool TryObjectField(
			const TSharedPtr<FJsonObject>& Parent,
			const FString& Field,
			TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Parent.IsValid()
				&& Parent->TryGetObjectField(Field, Found)
				&& Found
				&& Found->IsValid())
			{
				OutObject = *Found;
				return true;
			}
			return false;
		}

		bool TryExecuteDomainQuery(
			const FUeAgentReadExportCommandRoute& Route,
			const TSharedPtr<FJsonObject>& Params,
			const TSharedPtr<FJsonObject>& InputOverride,
			const FUeAgentReadExportCommandHandlerConfig& Config,
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>&
				Cancellation,
			const bool bExport,
			FUeAgentResourceTracker& Resources,
			TSharedPtr<const FUeAgentQueryBindingResult>& OutResult,
			FUeAgentQueryBindingError& OutError)
		{
			OutResult.Reset();
			const TOptional<FUeAgentQueryDescriptor> Descriptor =
				Config.DescriptorRegistry->FindQueryExact(
					Route.DescriptorIdentity);
			if (!Descriptor.IsSet())
			{
				OutError.Code =
					TEXT("read_export_descriptor_missing");
				OutError.SubjectId =
					Route.DescriptorIdentity.GetDescriptorId();
				return false;
			}
			TSharedPtr<FJsonObject> Input = InputOverride;
			if (!Input.IsValid()
				&& !TryObjectField(
					Params,
					TEXT("input"),
					Input))
			{
				Input = MakeShared<FJsonObject>();
			}
			TSharedPtr<FJsonObject> Context;
			TryObjectField(
				Params,
				TEXT("context"),
				Context);
			TSharedPtr<FJsonObject> Projection;
			TryObjectField(
				Params,
				TEXT("projection"),
				Projection);

			TOptional<FUeAgentDomainQueryTarget> Target;
			if (Route.bRequiresTarget)
			{
				FString PublicRef;
				if (!Params->TryGetStringField(
					TEXT("target"),
					PublicRef)
					|| PublicRef.TrimStartAndEnd().IsEmpty()
					|| !TryTarget(
						PublicRef,
						Route.DescriptorIdentity,
						Config,
						CallerContext,
						Target,
						OutError))
				{
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT("read_export_target_required");
						OutError.SubjectId = Route.CommandId;
					}
					return false;
				}
			}
			const FString InputJson = CanonicalObject(Input);
			const FUeAgentDomainQuery Query(
				Config.SessionSource->GetCurrent(),
				ExecutionContext(
					Descriptor.GetValue(),
					Context),
				FUeAgentContractValue(InputJson),
				MoveTemp(Target),
				ProjectionRequest(Projection, bExport));
			const FUeAgentQueryBindingRequest BindingRequest(
				Route.DescriptorIdentity,
				Request.GetRequestId(),
				FUeAgentQueryBindingPayload(Query));
			const auto Clock = []()
			{
				return FPlatformTime::Seconds();
			};
			FUeAgentCancellationSource CancellationSource(Clock);
			if (Cancellation->IsCancelled())
			{
				CancellationSource.Request(
					TEXT("command_cancellation_requested"));
			}
			const TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>
				QueryResources =
					MakeShared<
						FUeAgentResourceTracker,
						ESPMode::ThreadSafe>(
						MakeCommandBudget(
							Config.MaxFrameBytes,
							bExport));
			return Config.QueryBinding->TrySubmit(
				BindingRequest,
				CallerContext,
				QueryResources,
				FUeAgentOperationDeadline::FromTimeout(
					Config.TimeoutSeconds,
					Clock),
				CancellationSource.GetToken(),
				OutResult,
				OutError);
		}

		class FGameThreadHandler
			: public IUeAgentCommandHandler
		{
		public:
			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) final
			{
				if (IsInGameThread())
				{
					return Execute(
						Request,
						CallerContext,
						Cancellation);
				}
				const TSharedRef<
					TOptional<FUeAgentCommandResult>,
					ESPMode::ThreadSafe> Result =
					MakeShared<
						TOptional<FUeAgentCommandResult>,
						ESPMode::ThreadSafe>();
				FEvent* Completed =
					FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(
					ENamedThreads::GameThread,
					[
						this,
						Result,
						Completed,
						Request,
						CallerContext,
						Cancellation
					]()
					{
						Result->Emplace(Execute(
							Request,
							CallerContext,
							Cancellation));
						Completed->Trigger();
					});
				Completed->Wait();
				FPlatformProcess::ReturnSynchEventToPool(
					Completed);
				return Result->GetValue();
			}

		protected:
			virtual FUeAgentCommandResult Execute(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) = 0;
		};

		class FReadCommandHandler final
			: public FGameThreadHandler
		{
		public:
			FReadCommandHandler(
				FUeAgentReadExportCommandRoute InRoute,
				FUeAgentReadExportCommandHandlerConfig InConfig)
				: Route(MoveTemp(InRoute))
				, Config(MoveTemp(InConfig))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

		protected:
			virtual FUeAgentCommandResult Execute(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				FUeAgentResourceTracker Resources =
					MakeCommandResources(
						Config.MaxFrameBytes,
						false);
				TSharedPtr<const FUeAgentQueryBindingResult>
					Result;
				FUeAgentQueryBindingError QueryError;
				if (!TryExecuteDomainQuery(
						Route,
						Request.GetParams(),
						{},
						Config,
						Request,
						CallerContext,
						Cancellation,
						false,
						Resources,
						Result,
						QueryError)
					|| !Result.IsValid())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(QueryError));
				}
				FUeAgentContractValue Encoded(TEXT("{}"));
				if (!FUeAgentQueryPayloadCodec::TryEncode(
					Result->GetValue(),
					Resources,
					Encoded,
					QueryError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(QueryError));
				}
				return FUeAgentCommandResult::Success(
					Request,
					Encoded.GetJsonValue());
			}

		private:
			FUeAgentReadExportCommandRoute Route;
			FUeAgentReadExportCommandHandlerConfig Config;
		};

		class FExportCommandHandler final
			: public FGameThreadHandler
		{
		public:
			FExportCommandHandler(
				FUeAgentReadExportCommandRoute InRoute,
				FUeAgentReadExportCommandHandlerConfig InConfig)
				: Route(MoveTemp(InRoute))
				, Config(MoveTemp(InConfig))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return GetSemanticExportCommandId();
			}

		protected:
			virtual FUeAgentCommandResult Execute(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const TSharedPtr<FJsonObject>& Params =
					Request.GetParams();
				TSharedPtr<FJsonObject> Input;
				if (!TryObjectField(
					Params,
					TEXT("input"),
					Input))
				{
					Input = MakeShared<FJsonObject>();
				}
				else
				{
					const TSharedPtr<FJsonObject> Copy =
						MakeShared<FJsonObject>();
					Copy->Values = Input->Values;
					Input = Copy;
				}
				const TArray<TSharedPtr<FJsonValue>>* Sections =
					nullptr;
				if (!Input->TryGetArrayField(
					TEXT("sections"),
					Sections))
				{
					Input->SetArrayField(
						TEXT("sections"),
						{
							MakeShared<FJsonValueString>(
								TEXT("summary")),
							MakeShared<FJsonValueString>(
								TEXT("structure")),
							MakeShared<FJsonValueString>(
								TEXT("items")),
							MakeShared<FJsonValueString>(
								TEXT("relations")),
							MakeShared<FJsonValueString>(
								TEXT("properties")),
							MakeShared<FJsonValueString>(
								TEXT("parameters")),
							MakeShared<FJsonValueString>(
								TEXT("diagnostics")),
						});
				}
				if (!Input->HasField(TEXT("include_closure")))
				{
					Input->SetBoolField(
						TEXT("include_closure"),
						true);
				}

				FUeAgentResourceTracker Resources =
					MakeCommandResources(
						Config.MaxFrameBytes,
						true);
				TSharedPtr<const FUeAgentQueryBindingResult>
					Result;
				FUeAgentQueryBindingError QueryError;
				if (!TryExecuteDomainQuery(
						Route,
						Params,
						Input,
						Config,
						Request,
						CallerContext,
						Cancellation,
						true,
						Resources,
						Result,
						QueryError)
					|| !Result.IsValid()
					|| !Result->GetValue().GetSource().IsValid()
					|| !Result->GetValue().GetSource()
						->GetSnapshot().IsValid())
				{
					if (QueryError.Code.IsEmpty())
					{
						QueryError.Code =
							TEXT(
								"semantic_export_source_missing");
						QueryError.SubjectId =
							Request.GetRequestId();
					}
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(QueryError));
				}

				const FString Target =
					Params->GetStringField(TEXT("target"));
				FString ProfileId(TEXT("llm_authoring"));
				Params->TryGetStringField(
					TEXT("profile_id"),
					ProfileId);
				FExportWorkspacePlanPtr Plan;
				FWorkspaceError WorkspaceError;
				const FUeAgentCanonicalSnapshotRef Snapshot =
					Result->GetValue().GetSource()
						->GetSnapshot().ToSharedRef();
				if (!TryBuildSnapshotExportPlan(
					FSnapshotCompositionRequest(
						TargetCompositionKind(
							ResolveTargetObject(Target)),
						ProfileId,
						Snapshot),
					{
						{
							TEXT("state_entry_count"),
							Snapshot->GetEntries().Num(),
						},
					},
					Resources,
					Plan,
					WorkspaceError)
					|| !Plan.IsValid())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							WorkspaceError.GetCode(),
							WorkspaceError.GetSourceUnit(),
							WorkspaceError.GetJsonPointer()));
				}

				FString WorkspacePath =
					Params->GetStringField(
						TEXT("workspace_path"));
				FPaths::NormalizeDirectoryName(WorkspacePath);
				FArtifactPublishReceipt Receipt;
				FArtifactPublishError PublishError;
				if (!TryPublishExportPlan(
					FArtifactPublishRequest(
						Plan.ToSharedRef(),
						WorkspacePath,
						TEXT(
							"content_graph.export_workspace")),
					Resources,
					Receipt,
					PublishError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							PublishError.GetCode(),
							PublishError.GetPath(),
							PublishError.GetDetail()));
				}

				const TSharedPtr<FJsonObject> Payload =
					MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("status"),
					TEXT("published"));
				Payload->SetStringField(
					TEXT("workspace_path"),
					Receipt.GetFinalPath());
				Payload->SetStringField(
					TEXT("entry_file"),
					Receipt.GetEntryFile());
				Payload->SetStringField(
					TEXT("snapshot_id"),
					Receipt.GetSnapshotId());
				Payload->SetStringField(
					TEXT("snapshot_evidence_id"),
					Receipt.GetSnapshotEvidenceId());
				Payload->SetStringField(
					TEXT("plan_fingerprint"),
					Receipt.GetPlanFingerprint());
				Payload->SetStringField(
					TEXT("inventory_hash"),
					Receipt.GetInventoryHash());
				Payload->SetNumberField(
					TEXT("included_file_count"),
					Receipt.GetIncludedFiles().Num());
				Payload->SetNumberField(
					TEXT("observed_tokens"),
					Plan->GetObservedTokens());
				return FUeAgentCommandResult::Success(
					Request,
					CanonicalObject(Payload));
			}

		private:
			FUeAgentReadExportCommandRoute Route;
			FUeAgentReadExportCommandHandlerConfig Config;
		};
	}

	bool FUeAgentReadExportCommandRoute::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& DescriptorIdentity.GetDescriptorId()
				== TEXT("uai.query.") + CommandId;
	}

	bool TryCreateReadExportDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> ContentGraph;
		if (!DomainAdapters::ContentGraphRead::
				TryCreateContentGraphReadDescriptorRegistry(
					ContentGraph,
					OutError))
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(7);
		if (!MergeDescriptors(
				ContentGraph,
				Descriptors,
				OutError))
		{
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	const TArray<FUeAgentReadExportCommandRoute>&
	GetReadExportQueryCommandRoutes()
	{
		// The public content_graph routes migrated to the strict_v2 active
		// UObject/DataAsset contribution.  The legacy descriptor/binding path
		// intentionally exposes no executable routes.
		static const TArray<FUeAgentReadExportCommandRoute> Routes;
		return Routes;
	}

	FUeAgentReadExportCommandHandlerConfig::
		FUeAgentReadExportCommandHandlerConfig(
			FString InServerSessionId,
			const int64 InRegistryEpoch,
			const double InTimeoutSeconds,
			const int64 InMaxFrameBytes,
			TSharedRef<const FUeAgentDescriptorRegistry>
				InDescriptorRegistry,
			TSharedRef<
				const BindingGateway::Query::
					FUeAgentQueryBinding>
				InQueryBinding,
			TSharedRef<IUeAgentQuerySessionSource>
				InSessionSource,
			TSharedRef<const FUeAgentQueryTicketService>
				InTicketService)
		: ServerSessionId(MoveTemp(InServerSessionId))
		, RegistryEpoch(InRegistryEpoch)
		, TimeoutSeconds(InTimeoutSeconds)
		, MaxFrameBytes(InMaxFrameBytes)
		, DescriptorRegistry(MoveTemp(InDescriptorRegistry))
		, QueryBinding(MoveTemp(InQueryBinding))
		, SessionSource(MoveTemp(InSessionSource))
		, TicketService(MoveTemp(InTicketService))
	{
	}

	bool FUeAgentReadExportCommandHandlerConfig::IsValid() const
	{
		return !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& RegistryEpoch > 0
			&& TimeoutSeconds > 0.0
			&& MaxFrameBytes > 0
			&& DescriptorRegistry->GetDescriptors().Num() == 7
			&& GetReadExportQueryCommandRoutes().IsEmpty();
	}

	bool TryCreateReadExportCommandHandlers(
		const FUeAgentReadExportCommandHandlerConfig& Config,
		TArray<TSharedRef<IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (!Config.IsValid())
		{
			OutError =
				TEXT("read_export_command_handler_config_invalid");
			return false;
		}
		return true;
	}
}
