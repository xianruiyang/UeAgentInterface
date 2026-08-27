#include "DomainAdapters/Query/UeAgentQueryService.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeHeightQuery.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProductionEnvironment.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::Query
{
	using namespace BindingGateway::Query;
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace Projection;
	using namespace StateSemantic;

	namespace
	{
		const TArray<FQueryRoute> Routes = {
			{TEXT("asset.collection"), TEXT("asset.collection"), TEXT("asset_collection"), true, false},
			{TEXT("asset.properties"), TEXT("asset.properties"), TEXT("asset_properties"), true, false},
			{TEXT("asset.registry_record"), TEXT("asset.registry_record"), TEXT("asset_registry_record"), true, false},
			{TEXT("asset.registry_relations"), TEXT("asset.registry_relations"), TEXT("asset_registry_relations"), true, false},
			{TEXT("asset.relations"), TEXT("asset.relations"), TEXT("asset_relations"), true, false},
			{TEXT("asset.sample"), TEXT("asset.sample"), TEXT("asset_sample"), true, false},
			{TEXT("asset.summary"), TEXT("asset.summary"), TEXT("asset_summary"), true, false},
			{TEXT("asset.validation"), TEXT("asset.validation"), TEXT("asset_validation"), true, false},
			{TEXT("config.resolved_value"), TEXT("config.resolved_value"), TEXT("config_resolved_value"), false, false},
			{TEXT("config.section"), TEXT("config.section"), TEXT("config_section"), false, false},
			{TEXT("editor.dirty_resources"), TEXT("editor.dirty_resources"), TEXT("editor_dirty_resources"), false, false},
			{TEXT("editor.open_assets"), TEXT("editor.open_assets"), TEXT("editor_open_assets"), false, false},
			{TEXT("landscape.height.read"), TEXT("landscape.height.read"), TEXT("landscape_height_read"), true, false},
			{TEXT("project.platform"), TEXT("project.platform"), TEXT("project_platform"), false, false},
			{TEXT("project.settings"), TEXT("project.settings"), TEXT("project_settings"), false, false},
			{TEXT("project.summary"), TEXT("project.summary"), TEXT("project_summary"), false, false},
			{TEXT("project.validation"), TEXT("project.validation"), TEXT("project_validation"), false, false},
			{TEXT("reference.asset_read"), TEXT("reference.read"), TEXT("reference_asset_read"), true, true, TEXT("BlueprintDisplayName")},
			{TEXT("reference.origin.resolve"), TEXT("reference.origin.resolve"), TEXT("reference_origin_resolve"), false, false},
			{TEXT("world.actor_components"), TEXT("world.actor_components"), TEXT("world_actor_components"), true, false},
			{TEXT("world.actors"), TEXT("world.actors"), TEXT("world_actors"), false, false},
			{TEXT("world.capsule_path"), TEXT("world.capsule_path"), TEXT("world_capsule_path"), false, false},
			{TEXT("world.capsule_sweep"), TEXT("world.capsule_sweep"), TEXT("world_capsule_sweep"), false, false},
			{TEXT("world.content"), TEXT("world.content"), TEXT("world_content"), false, false},
			{TEXT("world.data_layers"), TEXT("world.data_layers"), TEXT("world_data_layers"), false, false},
			{TEXT("world.hlod_assignments"), TEXT("world.hlod_assignments"), TEXT("world_hlod_assignments"), false, false},
			{TEXT("world.mesh_closest_vertex"), TEXT("world.mesh_closest_vertex"), TEXT("world_mesh_closest_vertex"), true, false},
			{TEXT("world.mesh_vertex_position"), TEXT("world.mesh_vertex_position"), TEXT("world_mesh_vertex_position"), true, false},
			{TEXT("world.nav_connectivity"), TEXT("world.nav_connectivity"), TEXT("world_nav_connectivity"), false, false},
			{TEXT("world.nav_find_path"), TEXT("world.nav_find_path"), TEXT("world_nav_find_path"), false, false},
			{TEXT("world.nav_project_point"), TEXT("world.nav_project_point"), TEXT("world_nav_project_point"), false, false},
			{TEXT("world.nearby_shapes"), TEXT("world.nearby_shapes"), TEXT("world_nearby_shapes"), false, false},
			{TEXT("world.overlap"), TEXT("world.overlap"), TEXT("world_overlap"), false, false},
			{TEXT("world.partition"), TEXT("world.partition"), TEXT("world_partition"), false, false},
			{TEXT("world.partition_health"), TEXT("world.partition_health"), TEXT("world_partition_health"), false, false},
			{TEXT("world.physics_component"), TEXT("world.physics_component"), TEXT("world_physics_component"), true, false},
			{TEXT("world.physics_constraint"), TEXT("world.physics_constraint"), TEXT("world_physics_constraint"), true, false},
			{TEXT("world.property"), TEXT("world.property"), TEXT("world_property"), true, false},
			{TEXT("world.ray_trace"), TEXT("world.ray_trace"), TEXT("world_ray_trace"), false, false},
			{TEXT("world.selection"), TEXT("world.selection"), TEXT("world_selection"), false, false},
			{TEXT("world.streaming_levels"), TEXT("world.streaming_levels"), TEXT("world_streaming_levels"), false, false},
			{TEXT("world.summary"), TEXT("world.summary"), TEXT("world_summary"), false, false},
		};

		const FQueryRoute* FindRoute(const FString& CommandId)
		{
			return Routes.FindByPredicate(
				[&CommandId](const FQueryRoute& Route)
				{
					return Route.CommandId == CommandId;
				});
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			FQueryServiceError& OutError,
			const FString& Subject)
		{
			FString Error;
			if (!Json::TryCanonicalize(Object, OutJson, Error))
			{
				OutError.Code = TEXT("query_json_invalid");
				OutError.SubjectId = Subject;
				OutError.Detail = Error;
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> ObjectField(
			const TSharedPtr<FJsonObject>& Parent,
			const FString& Name)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			return Parent.IsValid()
				&& Parent->TryGetObjectField(Name, Found)
				&& Found
				&& Found->IsValid()
				? *Found
				: TSharedPtr<FJsonObject>();
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
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Result.Add(Value->AsString());
				}
			}
			return Result;
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
				Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
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
					Input.LevelIdentity = World->PersistentLevel->GetPathName();
				}
			}
			if (GEditor)
			{
				if (USelection* Selection = GEditor->GetSelectedActors())
				{
					for (FSelectionIterator It(*Selection); It; ++It)
					{
						if (const AActor* Actor = Cast<AActor>(*It))
						{
							Input.SelectionIdentities.Add(Actor->GetPathName());
						}
					}
					Input.SelectionIdentities.Sort();
				}
			}
			Input.ContextRevision = FString::Printf(
				TEXT("%s:%llu"),
				World ? *World->GetOutermost()->GetName() : TEXT("no-world"),
				FPlatformTime::Cycles64());
			Input.bResolvedFromCurrent = true;

			if (!Overrides.IsValid())
			{
				return Input;
			}
			FString Text;
			if (Overrides->TryGetStringField(TEXT("state_plane"), Text))
			{
				EUeAgentStatePlane Parsed = EUeAgentStatePlane::Invalid;
				if (TryParseStatePlane(Text, Parsed))
				{
					Input.StatePlane = Parsed;
				}
			}
			Overrides->TryGetStringField(TEXT("world_identity"), Input.WorldIdentity);
			Overrides->TryGetStringField(TEXT("level_identity"), Input.LevelIdentity);
			Overrides->TryGetStringField(TEXT("asset_editor_identity"), Input.AssetEditorIdentity);
			Overrides->TryGetStringField(TEXT("viewport_identity"), Input.ViewportIdentity);
			Overrides->TryGetStringField(TEXT("context_revision"), Input.ContextRevision);
			const TArray<FString> Selection =
				StringArray(Overrides, TEXT("selection_identities"));
			if (!Selection.IsEmpty())
			{
				Input.SelectionIdentities = Selection;
			}
			return Input;
		}

		FUeAgentProjectionRequest ProjectionRequest(
			const TSharedPtr<FJsonObject>& Object)
		{
			FString ProfileId = TEXT("compact");
			bool bIncludeDefaults = false;
			bool bIncludeDetails = false;
			bool bIncludeProvenance = true;
			int32 MaxItems = 200;
			FString Cursor;
			if (Object.IsValid())
			{
				Object->TryGetStringField(TEXT("profile_id"), ProfileId);
				Object->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);
				Object->TryGetBoolField(TEXT("include_details"), bIncludeDetails);
				Object->TryGetBoolField(TEXT("include_provenance"), bIncludeProvenance);
				double Number = 0.0;
				if (Object->TryGetNumberField(TEXT("max_items"), Number)
					&& FMath::IsFinite(Number)
					&& Number == FMath::FloorToDouble(Number)
					&& Number > 0.0
					&& Number <= 10000.0)
				{
					MaxItems = static_cast<int32>(Number);
				}
				Object->TryGetStringField(TEXT("continuation_token"), Cursor);
			}
			return FUeAgentProjectionRequest(
				FUeAgentProjectionProfile(
					ProfileId,
					bIncludeDefaults,
					bIncludeDetails,
					bIncludeProvenance),
				FUeAgentPageRequest(MaxItems, Cursor),
				{},
				{},
				{});
		}

		FUeAgentResourceBudget QueryBudget()
		{
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16ll * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 64ll * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 64ll * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 1048576},
				{EUeAgentResourceDimension::Items, 1048576},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 1048576},
				{EUeAgentResourceDimension::WorkUnits, 4194304},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 512ll * 1024 * 1024},
			});
		}

		class FQueryTokenEstimator final : public IUeAgentTokenEstimator
		{
		public:
			virtual bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutError = {};
				FUeAgentCancellationRequested Cancelled;
				if (!Cancellation.TryCheckpoint(Cancelled))
				{
					OutError.Code = Cancelled.Code;
					OutError.SubjectId = Cancelled.Reason;
					OutError.Detail = TEXT("query projection token estimation was cancelled");
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(TEXT("query.projection.estimate"), Expired))
				{
					OutError.Code = Expired.Code;
					OutError.SubjectId = TEXT("query.projection.estimate");
					OutError.Detail = TEXT("query projection token estimation deadline expired");
					return false;
				}
				int64 Utf8Bytes = 512 + Artifacts.Num() * 512 + Diagnostics.Num() * 256;
				if (Payload.GetCatalog().IsValid())
				{
					Utf8Bytes += Payload.GetCatalog()->GetCandidates().Num() * 1024;
				}
				else if (Payload.GetCandidateBinding().IsValid())
				{
					Utf8Bytes += 1536;
				}
				else if (Payload.GetSnapshot().IsValid())
				{
					Utf8Bytes += Payload.GetSnapshot()->GetEntries().Num() * 2048;
				}
				FUeAgentResourceBudgetExceeded BudgetError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("query.projection.estimate"),
						BudgetError))
				{
					OutError.Code = BudgetError.Code;
					OutError.SubjectId = BudgetError.Path;
					OutError.Detail = TEXT("query projection resource budget exceeded");
					return false;
				}
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					FMath::Max<int64>(1, (Utf8Bytes + 2) / 3),
					Utf8Bytes);
				return true;
			}
		};

		class FQuerySessionSource final : public IUeAgentQuerySessionSource
		{
		public:
			explicit FQuerySessionSource(FUeAgentQuerySessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			virtual FUeAgentQuerySessionIdentity GetCurrent() const override
			{
				return Session;
			}

		private:
			FUeAgentQuerySessionIdentity Session;
		};

		bool MergeDescriptors(
			const TSharedPtr<const FUeAgentDescriptorRegistry>& Source,
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			FString& OutError)
		{
			if (!Source.IsValid())
			{
				OutError = TEXT("query_descriptor_source_missing");
				return false;
			}
			OutDescriptors.Append(Source->GetDescriptors());
			return true;
		}

		class FProductionQueryService final : public IUeAgentQueryService
		{
		public:
			FProductionQueryService(
				TSharedRef<const FUeAgentDescriptorRegistry> InDescriptors,
				TSharedRef<const FUeAgentDiscoveryService> InDiscovery,
				TSharedRef<const FUeAgentResponseProjector> InProjector,
				TSharedRef<IUeAgentQuerySessionSource> InSessions,
				TSharedRef<const FUeAgentQueryTicketService> InTickets)
				: Descriptors(MoveTemp(InDescriptors))
				, Discovery(MoveTemp(InDiscovery))
				, Projector(MoveTemp(InProjector))
				, Sessions(MoveTemp(InSessions))
				, Tickets(MoveTemp(InTickets))
			{
			}

			virtual bool Execute(
				const FQueryServiceRequest& Request,
				FQueryServiceResponse& OutResponse,
				FQueryServiceError& OutError) const override
			{
				OutResponse = {};
				OutError = {};
				if (!IsInGameThread())
				{
					OutError.Code = TEXT("query_thread_invalid");
					OutError.SubjectId = Request.CommandId;
					OutError.Detail = TEXT("real UE Query mechanics require the game thread");
					return false;
				}
				const FQueryRoute* Route = FindRoute(Request.CommandId);
				if (!Route)
				{
					OutError.Code = TEXT("query_command_not_supported");
					OutError.SubjectId = Request.CommandId;
					OutError.Detail = TEXT("the exact command has no active T06 route");
					return false;
				}

				TSharedPtr<FJsonObject> Params;
				FString ParseError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson,
						Params,
						ParseError))
				{
					OutError.Code = TEXT("query_params_invalid");
					OutError.SubjectId = Request.CommandId;
					OutError.Detail = ParseError;
					return false;
				}

				const FString DescriptorId = TEXT("uai.query.") + Route->MechanicsCommandId;
				const FUeAgentOperationDescriptor* DescriptorOperation =
					Descriptors->FindByDescriptorId(DescriptorId);
				if (!DescriptorOperation)
				{
					OutError.Code = TEXT("query_descriptor_missing");
					OutError.SubjectId = DescriptorId;
					OutError.Detail = TEXT("the server-owned mechanics descriptor is unavailable");
					return false;
				}
				const TOptional<FUeAgentQueryDescriptor> Descriptor =
					Descriptors->FindQueryExact(DescriptorOperation->GetIdentity());
				if (!Descriptor.IsSet())
				{
					OutError.Code = TEXT("query_descriptor_missing");
					OutError.SubjectId = DescriptorId;
					OutError.Detail = TEXT("the descriptor does not expose Query mechanics");
					return false;
				}

				TSharedPtr<FJsonObject> Input = ObjectField(Params, TEXT("input"));
				if (!Input.IsValid())
				{
					Input = MakeShared<FJsonObject>();
				}
				if (!Route->RequiredProperty.IsEmpty())
				{
					TArray<TSharedPtr<FJsonValue>> FieldIds;
					FieldIds.Add(MakeShared<FJsonValueString>(
						Route->RequiredProperty));
					Input->SetArrayField(TEXT("field_ids"), MoveTemp(FieldIds));
				}
				TSharedPtr<FJsonObject> Context = ObjectField(Params, TEXT("context"));
				if (Route->bUsesObjectPath && !Context.IsValid())
				{
					Context = MakeShared<FJsonObject>();
					Context->SetStringField(TEXT("state_plane"), TEXT("persisted"));
				}
				const TSharedPtr<FJsonObject> Projection = ObjectField(Params, TEXT("projection"));

				const FUeAgentCallerContext Caller(
					Request.CallerId,
					Request.AuthorityScopes,
					Request.CorrelationId);
				TOptional<FUeAgentDomainQueryTarget> Target;
				if (Route->bRequiresTarget)
				{
					FString PublicRef;
					const FString TargetField = Route->bUsesObjectPath
						? FString(TEXT("object_path"))
						: FString(TEXT("target"));
					if (!Params->TryGetStringField(TargetField, PublicRef)
						|| PublicRef.TrimStartAndEnd().IsEmpty())
					{
						OutError.Code = TEXT("query_target_required");
						OutError.SubjectId = Request.CommandId;
						OutError.Detail = TargetField + TEXT(" is required");
						return false;
					}
					const FUeAgentCandidateBinding Candidate(
						TEXT("query-target:") + PublicRef,
						FUeAgentCandidateSummary(
							FUeAgentCanonicalIdentity(
								TEXT("query-target:") + PublicRef,
								TEXT("object"),
								PublicRef,
								TEXT("target"),
								TEXT("target"),
								TEXT("current")),
							DescriptorOperation->GetIdentity(),
							TEXT("query_target"),
							FPaths::GetCleanFilename(PublicRef),
							TEXT("Exact public Query target"),
							{TEXT("UAI"), TEXT("Query")}),
						FUeAgentCatalogRevision(
							TEXT("query-target"),
							TEXT("current"),
							0),
						PublicRef,
						TEXT("query-service"),
						{EUeAgentCandidateOperation::Read},
						TEXT("exact_public_query_target"));
					TOptional<FUeAgentQueryTicket> Ticket;
					FUeAgentQueryBindingError TicketError;
					if (!Candidate.IsValid()
						|| !Tickets->TryIssue(
							Candidate,
							DescriptorOperation->GetIdentity(),
							Sessions->GetCurrent(),
							Caller,
							{},
							Ticket,
							TicketError)
						|| !Ticket.IsSet())
					{
						OutError.Code = TicketError.Code.IsEmpty()
							? FString(TEXT("query_target_ticket_failed"))
							: TicketError.Code;
						OutError.SubjectId = PublicRef;
						OutError.Detail = TicketError.Detail;
						return false;
					}
					Target.Emplace(Ticket.GetValue(), Candidate);
				}

				FString InputJson;
				if (!CanonicalObject(Input, InputJson, OutError, Request.CommandId))
				{
					return false;
				}
				const FUeAgentDomainQuery Query(
					Sessions->GetCurrent(),
					ExecutionContext(Descriptor.GetValue(), Context),
					FUeAgentContractValue(InputJson),
					MoveTemp(Target),
					ProjectionRequest(Projection));
				const FUeAgentQueryBindingRequest BindingRequest(
					DescriptorOperation->GetIdentity(),
					Request.RequestId,
					FUeAgentQueryBindingPayload(Query));
				const auto Clock = []() { return FPlatformTime::Seconds(); };
				FUeAgentCancellationSource Cancellation(Clock);
				if (Request.bCancellationRequested)
				{
					Cancellation.Request(TEXT("command_cancellation_requested"));
				}
				const TSharedRef<
					FUeAgentResourceTracker,
					ESPMode::ThreadSafe> Resources =
						MakeShared<
							FUeAgentResourceTracker,
							ESPMode::ThreadSafe>(QueryBudget());
				TSharedPtr<const FUeAgentQueryBindingResult> Result;
				FUeAgentQueryBindingError QueryError;
				TSharedPtr<const BindingGateway::Query::FUeAgentQueryBinding>
					QueryBinding;
				if (!TryGetBinding(QueryBinding, OutError))
				{
					return false;
				}
				if (!QueryBinding->TrySubmit(
						BindingRequest,
						Caller,
						Resources,
						FUeAgentOperationDeadline::FromTimeout(30.0, Clock),
						Cancellation.GetToken(),
						Result,
						QueryError)
					|| !Result.IsValid())
				{
					OutError.Code = QueryError.Code.IsEmpty()
						? FString(TEXT("query_execution_failed"))
						: QueryError.Code;
					OutError.SubjectId = QueryError.SubjectId.IsEmpty()
						? Request.CommandId
						: QueryError.SubjectId;
					OutError.Detail = QueryError.Detail;
					return false;
				}

				FUeAgentContractValue Encoded(TEXT("{}"));
				if (!FUeAgentQueryPayloadCodec::TryEncode(
						Result->GetValue(),
						Resources.Get(),
						Encoded,
						QueryError))
				{
					OutError.Code = QueryError.Code;
					OutError.SubjectId = QueryError.SubjectId;
					OutError.Detail = QueryError.Detail;
					return false;
				}
				TSharedPtr<FJsonObject> QueryResult;
				if (!Json::TryParseObject(
						Encoded.GetJsonValue(),
						QueryResult,
						ParseError))
				{
					OutError.Code = TEXT("query_result_invalid");
					OutError.SubjectId = Request.CommandId;
					OutError.Detail = ParseError;
					return false;
				}
				const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetStringField(TEXT("command_id"), Route->CommandId);
				Root->SetStringField(TEXT("mechanics_command_id"), Route->MechanicsCommandId);
				Root->SetBoolField(TEXT("read_only"), true);
				Root->SetObjectField(TEXT("query_result"), QueryResult);
				if (!CanonicalObject(
						Root,
						OutResponse.PayloadJson,
						OutError,
						Request.CommandId))
				{
					return false;
				}
				OutResponse.bReadOnly = true;
				return true;
			}

		private:
			bool TryGetBinding(
				TSharedPtr<
					const BindingGateway::Query::FUeAgentQueryBinding>&
						OutBinding,
				FQueryServiceError& OutError) const
			{
				OutBinding.Reset();
				FScopeLock Lock(&BindingMutex);
				if (Binding.IsValid())
				{
					OutBinding = Binding;
					return true;
				}

				DomainCapability::FDomainCapabilityAuthorities Authorities;
				FString AuthorityError;
				if (!DomainCapability::
					TryGetOrBuildProcessDomainCapabilityAuthorities(
						Authorities,
						AuthorityError)
					|| !Authorities.GetQueryProviderRegistry().IsValid())
				{
					OutError.Code =
						TEXT("query_provider_authority_unavailable");
					OutError.SubjectId = TEXT("domain-capability");
					OutError.Detail = AuthorityError;
					return false;
				}

				FUeAgentQueryBindingError QueryError;
				TSharedPtr<
					const BindingGateway::Query::FUeAgentQueryBinding>
						Built;
				if (!BindingGateway::Query::FUeAgentQueryBinding::TryCreate(
						Descriptors,
						Authorities.GetQueryProviderRegistry().ToSharedRef(),
						Discovery,
						Projector,
						Sessions,
						Tickets,
						Built,
						QueryError)
					|| !Built.IsValid())
				{
					OutError.Code = QueryError.Code.IsEmpty()
						? FString(TEXT("query_binding_unavailable"))
						: QueryError.Code;
					OutError.SubjectId = QueryError.SubjectId;
					OutError.Detail = QueryError.Detail;
					return false;
				}
				Binding = MoveTemp(Built);
				OutBinding = Binding;
				return true;
			}

			TSharedRef<const FUeAgentDescriptorRegistry> Descriptors;
			TSharedRef<const FUeAgentDiscoveryService> Discovery;
			TSharedRef<const FUeAgentResponseProjector> Projector;
			TSharedRef<IUeAgentQuerySessionSource> Sessions;
			TSharedRef<const FUeAgentQueryTicketService> Tickets;
			mutable FCriticalSection BindingMutex;
			mutable TSharedPtr<
				const BindingGateway::Query::FUeAgentQueryBinding> Binding;
		};
	}

	bool FQueryRoute::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& !MechanicsCommandId.IsEmpty()
			&& !PythonFunction.IsEmpty()
			&& (!bUsesObjectPath || bRequiresTarget)
			&& (RequiredProperty.IsEmpty() || bUsesObjectPath);
	}

	bool FProductionQueryComposition::IsValid() const
	{
		return Service.IsValid() && DescriptorRegistry.IsValid();
	}

	const TArray<FQueryRoute>& GetNonHjsonQueryRoutes()
	{
		return Routes;
	}

	bool TryCreateProductionQueryProviderRegistrations(
		TArray<FUeAgentDomainQueryProviderRegistration>&
			OutRegistrations,
		FString& OutError)
	{
		OutRegistrations.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> WorldDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> AssetDescriptors;
		if (!DomainAdapters::WorldRead::TryCreateWorldReadDescriptorRegistry(
				WorldDescriptors,
				OutError)
			|| !DomainAdapters::AssetProjectRead::
				TryCreateAssetProjectReadDescriptorRegistry(
					AssetDescriptors,
					OutError))
		{
			return false;
		}

		const TSharedRef<
			DomainAdapters::WorldRead::
				FUeAgentWorldReadProductionEnvironment>
			WorldEnvironment = MakeShared<
				DomainAdapters::WorldRead::
					FUeAgentWorldReadProductionEnvironment>();
		const TSharedRef<
			DomainAdapters::AssetProjectRead::
				FUeAgentAssetProjectReadProductionEnvironment>
			AssetEnvironment = MakeShared<
				DomainAdapters::AssetProjectRead::
					FUeAgentAssetProjectReadProductionEnvironment>();
		OutRegistrations.Reserve(41);
		for (const FUeAgentOperationDescriptor& Descriptor
			: WorldDescriptors->GetDescriptors())
		{
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				OutError = TEXT("query_provider_binding_missing:")
					+ Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			const FString ProviderBinding =
				Descriptor.GetQueryBinding()->GetProviderBinding();
			TSharedPtr<
				DomainAdapters::WorldRead::IUeAgentWorldReadHook> Hook;
			DomainAdapters::WorldRead::FUeAgentWorldReadError Error;
			if (!DomainAdapters::WorldRead::
				FUeAgentWorldReadNativeHookFactory::TryCreate(
					ProviderBinding,
					WorldEnvironment,
					Hook,
					Error)
				|| !Hook.IsValid())
			{
				OutError = Error.Code + TEXT(":") + Error.Detail;
				return false;
			}
			OutRegistrations.Emplace(
				ProviderBinding,
				MakeShared<
					DomainAdapters::WorldRead::FUeAgentWorldReadProvider>(
						ProviderBinding,
						Hook.ToSharedRef()));
		}
		for (const FUeAgentOperationDescriptor& Descriptor
			: AssetDescriptors->GetDescriptors())
		{
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				OutError = TEXT("query_provider_binding_missing:")
					+ Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			const FString ProviderBinding =
				Descriptor.GetQueryBinding()->GetProviderBinding();
			TSharedPtr<
				DomainAdapters::AssetProjectRead::
					IUeAgentAssetProjectReadHook> Hook;
			DomainAdapters::AssetProjectRead::
				FUeAgentAssetProjectReadError Error;
			if (!DomainAdapters::AssetProjectRead::
				FUeAgentAssetProjectReadNativeHookFactory::TryCreate(
					ProviderBinding,
					AssetEnvironment,
					Hook,
					Error)
				|| !Hook.IsValid())
			{
				OutError = Error.Code + TEXT(":") + Error.Detail;
				return false;
			}
			OutRegistrations.Emplace(
				ProviderBinding,
				MakeShared<
					DomainAdapters::AssetProjectRead::
						FUeAgentAssetProjectReadProvider>(
							ProviderBinding,
							Hook.ToSharedRef()));
		}
		if (OutRegistrations.Num() != 41)
		{
			OutRegistrations.Reset();
			OutError = TEXT("query_provider_inventory_invalid");
			return false;
		}
		return true;
	}

	bool TryCreateProductionQueryService(
		FProductionQueryComposition& OutComposition,
		FString& OutError)
	{
		OutComposition = {};
		OutError.Reset();

		TSharedPtr<const FUeAgentDescriptorRegistry> WorldDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> AssetDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> LandscapeDescriptors;
		if (!DomainAdapters::WorldRead::TryCreateWorldReadDescriptorRegistry(
				WorldDescriptors,
				OutError)
			|| !DomainAdapters::AssetProjectRead::TryCreateAssetProjectReadDescriptorRegistry(
				AssetDescriptors,
				OutError)
			|| !DomainAdapters::Landscape::
				TryCreateLandscapeHeightReadDescriptorRegistry(
					LandscapeDescriptors,
				OutError))
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(42);
		if (!MergeDescriptors(WorldDescriptors, Descriptors, OutError)
			|| !MergeDescriptors(AssetDescriptors, Descriptors, OutError)
			|| !MergeDescriptors(
				LandscapeDescriptors,
				Descriptors,
				OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		if (!FUeAgentDescriptorRegistry::TryCreate(
				MoveTemp(Descriptors),
				DescriptorRegistry,
				OutError)
			|| !DescriptorRegistry.IsValid()
			|| DescriptorRegistry->GetDescriptors().Num() != 42)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("query_descriptor_inventory_invalid");
			}
			return false;
		}

		FUeAgentQueryBindingError QueryError;
		TSharedPtr<const FUeAgentDiscoveryService> DiscoveryService;
		FUeAgentDiscoveryError DiscoveryError;
		if (!FUeAgentDiscoveryService::TryCreate(
				{},
				{},
				DiscoveryService,
				DiscoveryError)
			|| !DiscoveryService.IsValid())
		{
			OutError = DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail;
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				MakeShared<FQueryTokenEstimator>(),
				Projector,
				ProjectionError)
			|| !Projector.IsValid())
		{
			OutError = ProjectionError.Code + TEXT(":") + ProjectionError.Detail;
			return false;
		}
		const FString SessionId = FString::Printf(
			TEXT("query-service:%s:%u"),
			FPlatformProcess::ComputerName(),
			FPlatformProcess::GetCurrentProcessId());
		const TSharedRef<FQuerySessionSource> Sessions =
			MakeShared<FQuerySessionSource>(
				FUeAgentQuerySessionIdentity(
					SessionId,
					1,
					DescriptorRegistry->GetRegistryHash()));
		TSharedPtr<const FUeAgentQueryTicketService> Tickets;
		if (!FUeAgentQueryTicketService::TryCreate(
				FGuid::NewGuid().ToString(EGuidFormats::Digits),
				60.0,
				[]() { return FPlatformTime::Seconds(); },
				[]() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); },
				Tickets,
				QueryError)
			|| !Tickets.IsValid())
		{
			OutError = QueryError.Code + TEXT(":") + QueryError.Detail;
			return false;
		}
		OutComposition.Service = MakeShared<FProductionQueryService>(
			DescriptorRegistry.ToSharedRef(),
			DiscoveryService.ToSharedRef(),
			Projector.ToSharedRef(),
			Sessions,
			Tickets.ToSharedRef());
		OutComposition.DescriptorRegistry = MoveTemp(DescriptorRegistry);
		return OutComposition.IsValid();
	}
}
