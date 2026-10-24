#include "DomainAdapters/Query/UeAgentQueryService.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProvider.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
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
			{TEXT("asset.owned_candidates"), TEXT("asset.owned_candidates"), TEXT("asset_owned_candidates"), true, false, FString(), TEXT("owned_candidate_names")},
			{TEXT("asset.collection"), TEXT("asset.collection"), TEXT("asset_collection"), true, false, FString(), TEXT("asset_collection")},
			{TEXT("asset.properties"), TEXT("asset.properties"), TEXT("asset_properties"), true, false, FString(), TEXT("asset_properties")},
			{TEXT("asset.registry_record"), TEXT("asset.registry_record"), TEXT("asset_registry_record"), true, false, FString(), TEXT("asset_registry_records")},
			{TEXT("asset.registry_relations"), TEXT("asset.registry_relations"), TEXT("asset_registry_relations"), true, false},
			{TEXT("asset.relations"), TEXT("asset.relations"), TEXT("asset_relations"), true, false, FString(), TEXT("asset_relations")},
			{TEXT("asset.sample"), TEXT("asset.sample"), TEXT("asset_sample"), true, false, FString(), TEXT("asset_sample")},
			{TEXT("asset.summary"), TEXT("asset.summary"), TEXT("asset_summary"), true, false, FString(), TEXT("object_summaries")},
			{TEXT("asset.validation"), TEXT("asset.validation"), TEXT("asset_validation"), true, false, FString(), TEXT("validation_results")},
			{TEXT("config.resolved_value"), TEXT("config.resolved_value"), TEXT("config_resolved_value"), false, false, FString(), TEXT("config_resolved_value")},
			{TEXT("config.section"), TEXT("config.section"), TEXT("config_section"), false, false, FString(), TEXT("config_section")},
			{TEXT("content_graph.compound"), TEXT("content_graph.compound"), TEXT("content_graph_compound"), true, false, FString(), TEXT("content_graph_sections")},
			{TEXT("content_graph.diagnostics"), TEXT("content_graph.diagnostics"), TEXT("content_graph_diagnostics"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("content_graph.items"), TEXT("content_graph.items"), TEXT("content_graph_items"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("content_graph.properties"), TEXT("content_graph.properties"), TEXT("content_graph_properties"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("content_graph.relations"), TEXT("content_graph.relations"), TEXT("content_graph_relations"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("content_graph.structure"), TEXT("content_graph.structure"), TEXT("content_graph_structure"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("content_graph.summary"), TEXT("content_graph.summary"), TEXT("content_graph_summary"), true, false, FString(), TEXT("content_graph_entries")},
			{TEXT("editor.dirty_resources"), TEXT("editor.dirty_resources"), TEXT("editor_dirty_resources"), false, false, FString(), TEXT("dirty_resources")},
			{TEXT("editor.open_assets"), TEXT("editor.open_assets"), TEXT("editor_open_assets"), false, false, FString(), TEXT("editor_resources")},
			{TEXT("landscape.height.read"), TEXT("landscape.height.read"), TEXT("landscape_height_read"), true, false, FString(), TEXT("landscape_height_grid")},
			{TEXT("project.platform"), TEXT("project.platform"), TEXT("project_platform"), false, false, FString(), TEXT("platform_observations")},
			{TEXT("project.settings"), TEXT("project.settings"), TEXT("project_settings"), false, false, FString(), TEXT("project_settings")},
			{TEXT("project.summary"), TEXT("project.summary"), TEXT("project_summary"), false, false, FString(), TEXT("object_summaries")},
			{TEXT("project.validation"), TEXT("project.validation"), TEXT("project_validation"), false, false, FString(), TEXT("validation_results")},
			{TEXT("reference.origin.resolve"), TEXT("reference.origin.resolve"), TEXT("reference_origin_resolve"), false, false, FString(), TEXT("origin_resolution")},
			{TEXT("world.actor_components"), TEXT("world.actor_components"), TEXT("world_actor_components"), true, false},
			{TEXT("world.actors"), TEXT("world.actors"), TEXT("world_actors"), false, false},
			{TEXT("world.capsule_path"), TEXT("world.capsule_path"), TEXT("world_capsule_path"), false, false, FString(), TEXT("world_capsule_path")},
			{TEXT("world.capsule_sweep"), TEXT("world.capsule_sweep"), TEXT("world_capsule_sweep"), false, false, FString(), TEXT("world_capsule_sweep")},
			{TEXT("world.content"), TEXT("world.content"), TEXT("world_content"), false, false},
			{TEXT("world.data_layers"), TEXT("world.data_layers"), TEXT("world_data_layers"), false, false, FString(), TEXT("world_data_layers")},
			{TEXT("world.hlod_assignments"), TEXT("world.hlod_assignments"), TEXT("world_hlod_assignments"), false, false, FString(), TEXT("world_hlod_assignments")},
			{TEXT("world.mesh_closest_vertex"), TEXT("world.mesh_closest_vertex"), TEXT("world_mesh_closest_vertex"), true, false, FString(), TEXT("world_mesh_vertex")},
			{TEXT("world.mesh_vertex_position"), TEXT("world.mesh_vertex_position"), TEXT("world_mesh_vertex_position"), true, false, FString(), TEXT("world_mesh_vertex")},
			{TEXT("world.nav_connectivity"), TEXT("world.nav_connectivity"), TEXT("world_nav_connectivity"), false, false, FString(), TEXT("world_nav_connectivity")},
			{TEXT("world.nav_find_path"), TEXT("world.nav_find_path"), TEXT("world_nav_find_path"), false, false, FString(), TEXT("world_nav_find_path")},
			{TEXT("world.nav_project_point"), TEXT("world.nav_project_point"), TEXT("world_nav_project_point"), false, false, FString(), TEXT("world_nav_project_point")},
			{TEXT("world.nearby_shapes"), TEXT("world.nearby_shapes"), TEXT("world_nearby_shapes"), false, false, FString(), TEXT("world_nearby_shapes")},
			{TEXT("world.overlap"), TEXT("world.overlap"), TEXT("world_overlap"), false, false, FString(), TEXT("world_overlap")},
			{TEXT("world.partition"), TEXT("world.partition"), TEXT("world_partition"), false, false, FString(), TEXT("world_partition")},
			{TEXT("world.partition_health"), TEXT("world.partition_health"), TEXT("world_partition_health"), false, false, FString(), TEXT("world_partition_health")},
			{TEXT("world.physics_component"), TEXT("world.physics_component"), TEXT("world_physics_component"), true, false, FString(), TEXT("world_physics_component")},
			{TEXT("world.physics_constraint"), TEXT("world.physics_constraint"), TEXT("world_physics_constraint"), true, false, FString(), TEXT("world_physics_constraint")},
			{TEXT("world.property"), TEXT("world.property"), TEXT("world_property"), true, false},
			{TEXT("world.ray_trace"), TEXT("world.ray_trace"), TEXT("world_ray_trace"), false, false, FString(), TEXT("world_ray_trace")},
			{TEXT("world.selection"), TEXT("world.selection"), TEXT("world_selection"), false, false, FString(), TEXT("world_selection")},
			{TEXT("world.streaming_levels"), TEXT("world.streaming_levels"), TEXT("world_streaming_levels"), false, false, FString(), TEXT("world_streaming_levels")},
			{TEXT("world.summary"), TEXT("world.summary"), TEXT("world_summary"), false, false, FString(), TEXT("world_summary")},
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Object, OutJson, Error))
			{
				OutError = Error; if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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

		FDiagnostic QueryFact(const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SourceKind = TEXT("native_result"))
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("query_service"));
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.SourceKind = SourceKind;
			if (SourceKind == TEXT("command_input")) Error.Source.JsonPath = Path;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		TSharedPtr<FJsonValue> QueryValueKindFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			const auto Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("unknown");
			switch (Value->Type)
			{
			case EJson::Null: Kind = TEXT("null"); break;
			case EJson::String: Kind = TEXT("string"); break;
			case EJson::Number: Kind = TEXT("number"); break;
			case EJson::Boolean: Kind = TEXT("boolean"); break;
			case EJson::Array: Kind = TEXT("array"); break;
			case EJson::Object: Kind = TEXT("object"); break;
			default: break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			if (Value->Type == EJson::Array) Shape->SetNumberField(TEXT("count"), Value->AsArray().Num());
			if (Value->Type == EJson::Object) Shape->SetNumberField(TEXT("count"), Value->AsObject()->Values.Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

		TSharedPtr<FJsonValue> QueryValueFact(const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array)
				? QueryValueKindFact(Value) : Value;
		}

		TSharedPtr<FJsonValue> QueryChoices(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const FString& Value : Values) Choices.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Choices));
		}

		TSharedPtr<FJsonValue> QueryStatePlaneChoices(const FUeAgentQueryDescriptor& Descriptor)
		{
			TArray<FString> Values;
			for (const EUeAgentStatePlane Plane : Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes())
				Values.Add(LexToString(Plane));
			return QueryChoices(Values);
		}

		FString QueryFieldPath(const FString& Parent, FString Field)
		{
			Field.ReplaceInline(TEXT("~"), TEXT("~0"));
			Field.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Parent + TEXT("/") + Field;
		}

		bool QueryOptionFailure(const FString& Path, const FString& Reason,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			FDiagnostic& OutError, TSharedPtr<FJsonValue> PrivateActual = nullptr)
		{
			OutError = QueryFact(TEXT("query_option_invalid"), Reason, Path,
				MoveTemp(Actual), MoveTemp(Expected), TEXT("command_input"));
			OutError.Stage = TEXT("query_input");
			OutError.Source.JsonPath = Path;
			OutError.ActualEvidence = MoveTemp(PrivateActual);
			return false;
		}

		bool TryValidateQueryOptions(const TSharedPtr<FJsonObject>& Params,
			const FUeAgentQueryDescriptor& Descriptor, FDiagnostic& OutError)
		{
			for (const TCHAR* Section : {TEXT("input"), TEXT("context"), TEXT("projection")})
			{
				if (Params->HasField(Section) && !ObjectField(Params, Section).IsValid())
					return QueryOptionFailure(QueryFieldPath(FString(), Section), TEXT("The query section must be an object."),
						QueryValueFact(Params->TryGetField(Section)), MakeShared<FJsonValueString>(TEXT("object")), OutError);
			}
			if (const auto Context = ObjectField(Params, TEXT("context")); Context.IsValid())
			{
				for (const auto& Field : Context->Values)
				{
					const FString Path = QueryFieldPath(TEXT("/context"), Field.Key);
					if (Field.Key == TEXT("selection_identities"))
					{
						const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
						if (!Context->TryGetArrayField(Field.Key, Values))
							return QueryOptionFailure(Path, TEXT("Query selection identities must be an array of strings."),
								QueryValueFact(Field.Value), MakeShared<FJsonValueString>(TEXT("array of strings")), OutError);
						for (int32 Index = 0; Index < Values->Num(); ++Index)
						{
							FString Value;
							if (!(*Values)[Index].IsValid() || (*Values)[Index]->Type != EJson::String
								|| !(*Values)[Index]->TryGetString(Value) || Value.TrimStartAndEnd().IsEmpty())
								return QueryOptionFailure(Path + TEXT("/") + FString::FromInt(Index),
									TEXT("A query selection identity must be a non-empty string."), QueryValueFact((*Values)[Index]),
									MakeShared<FJsonValueString>(TEXT("non-empty string")), OutError);
						}
						continue;
					}
					if (Field.Key != TEXT("state_plane") && Field.Key != TEXT("world_identity")
						&& Field.Key != TEXT("level_identity") && Field.Key != TEXT("asset_editor_identity")
						&& Field.Key != TEXT("viewport_identity") && Field.Key != TEXT("context_revision"))
						return QueryOptionFailure(Path, TEXT("The query context field is unsupported."),
							MakeShared<FJsonValueString>(Field.Key), QueryChoices({TEXT("state_plane"), TEXT("world_identity"),
								TEXT("level_identity"), TEXT("asset_editor_identity"), TEXT("viewport_identity"),
								TEXT("context_revision"), TEXT("selection_identities")}), OutError, Field.Value);
					FString Value;
					if (!Field.Value.IsValid() || Field.Value->Type != EJson::String
						|| !Context->TryGetStringField(Field.Key, Value))
					{
						// These source fields carry opaque editor/viewport identities or a context fence.
						const bool bPrivateIdentity = Field.Key == TEXT("asset_editor_identity")
							|| Field.Key == TEXT("viewport_identity") || Field.Key == TEXT("context_revision");
						return QueryOptionFailure(Path, TEXT("The query context value must be a string."),
							bPrivateIdentity ? QueryValueKindFact(Field.Value) : QueryValueFact(Field.Value),
							MakeShared<FJsonValueString>(TEXT("string")), OutError, bPrivateIdentity ? Field.Value : nullptr);
					}
					if (Field.Key == TEXT("state_plane"))
					{
						EUeAgentStatePlane Plane = EUeAgentStatePlane::Invalid;
						if (!TryParseStatePlane(Value, Plane))
							return QueryOptionFailure(Path, TEXT("The query state plane is unknown."),
								MakeShared<FJsonValueString>(Value), QueryStatePlaneChoices(Descriptor), OutError);
						if (!Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes().Contains(Plane))
							return QueryOptionFailure(Path, TEXT("The state plane is not supported by this query."),
								MakeShared<FJsonValueString>(Value), QueryStatePlaneChoices(Descriptor), OutError);
					}
				}
			}
			if (const auto Projection = ObjectField(Params, TEXT("projection")); Projection.IsValid())
			{
				for (const auto& Field : Projection->Values)
				{
					const FString Path = QueryFieldPath(TEXT("/projection"), Field.Key);
					if (Field.Key == TEXT("profile_id"))
					{
						FString Value;
						if (!Field.Value.IsValid() || Field.Value->Type != EJson::String
							|| !Projection->TryGetStringField(Field.Key, Value) || Value.TrimStartAndEnd().IsEmpty())
							return QueryOptionFailure(Path, TEXT("The query projection profile must be a non-empty name."),
								QueryValueFact(Field.Value), MakeShared<FJsonValueString>(TEXT("non-empty profile name")), OutError);
					}
					else if (Field.Key == TEXT("include_defaults") || Field.Key == TEXT("include_details") || Field.Key == TEXT("include_provenance"))
					{
						bool Value = false;
						if (!Field.Value.IsValid() || Field.Value->Type != EJson::Boolean
							|| !Projection->TryGetBoolField(Field.Key, Value))
							return QueryOptionFailure(Path, TEXT("The query projection option must be a boolean."),
								QueryValueFact(Field.Value), MakeShared<FJsonValueString>(TEXT("boolean")), OutError);
					}
					else return QueryOptionFailure(Path, TEXT("The query projection field is unsupported."),
						MakeShared<FJsonValueString>(Field.Key), QueryChoices({TEXT("profile_id"), TEXT("include_defaults"),
							TEXT("include_details"), TEXT("include_provenance")}), OutError, Field.Value);
				}
			}
			return true;
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
			if (Overrides->HasField(TEXT("selection_identities")))
			{
				Input.SelectionIdentities = StringArray(Overrides, TEXT("selection_identities"));
			}
			return Input;
		}

		FUeAgentProjectionRequest ProjectionRequest(
			const TSharedPtr<FJsonObject>& Object,
			const FQueryPublicProjectionDescriptor& PublicProjection)
		{
			FString ProfileId = TEXT("compact");
			bool bIncludeDefaults = false;
			bool bIncludeDetails = false;
			bool bIncludeProvenance = false;
			const EUeAgentProjectionPageUnit PageUnit = QueryPublicProjectionPageUnit(PublicProjection);
			if (Object.IsValid())
			{
				Object->TryGetStringField(TEXT("profile_id"), ProfileId);
				Object->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);
				Object->TryGetBoolField(TEXT("include_details"), bIncludeDetails);
				Object->TryGetBoolField(TEXT("include_provenance"), bIncludeProvenance);
			}
			return FUeAgentProjectionRequest(
				FUeAgentProjectionProfile(
					ProfileId,
					bIncludeDefaults,
					bIncludeDetails,
					bIncludeProvenance),
				{},
				{},
				PageUnit);
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
					OutError = Cancelled;
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
				FUeAgentResourceChargeError ChargeError;
				if (!Resources.TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						1,
						TEXT("query.projection.estimate"),
						ChargeError))
				{
					OutError = ChargeError;
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
			const FString& SourceFamily,
			const TSharedPtr<const FUeAgentDescriptorRegistry>& Source,
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Source.IsValid())
			{
				OutError = QueryFact(TEXT("query_descriptor_source_missing"),
					TEXT("The query descriptor source registry is unavailable."), TEXT("/descriptors/source_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = SourceFamily;
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
					OutError = QueryFact(TEXT("query_thread_invalid"),
						TEXT("Query execution requires the game thread."), TEXT("/execution/game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.Stage = TEXT("query");
					OutError.CandidateName = Request.CommandId;
					return false;
				}
				const FQueryRoute* Route = FindRoute(Request.CommandId);
				if (!Route)
				{
					TArray<FString> Commands;
					for (const FQueryRoute& PublicRoute : GetNonHjsonQueryRoutes()) Commands.Add(PublicRoute.CommandId);
					OutError = QueryFact(TEXT("query_command_not_supported"),
						TEXT("The command has no active query route."), TEXT("/command_id"),
						MakeShared<FJsonValueString>(Request.CommandId), QueryChoices(Commands), TEXT("command_input"));
					OutError.Stage = TEXT("query");
					OutError.CandidateName = Request.CommandId;
					return false;
				}

				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic ParseError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson,
						Params,
						ParseError))
				{
					OutError = ParseError;
                    if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.CommandId;
					return false;
				}

				const FString DescriptorId = TEXT("uai.query.") + Route->MechanicsCommandId;
				const FUeAgentOperationDescriptor* DescriptorOperation =
					Descriptors->FindByDescriptorId(DescriptorId);
				if (!DescriptorOperation)
				{
					OutError = QueryFact(TEXT("query_descriptor_missing"),
						TEXT("The query mechanics descriptor is unavailable."), TEXT("/query/descriptor_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.Stage = TEXT("query");
					OutError.CandidateName = Request.CommandId;
					OutError.InternalContext = DescriptorId;
					return false;
				}
				const TOptional<FUeAgentQueryDescriptor> Descriptor =
					Descriptors->FindQueryExact(DescriptorOperation->GetIdentity());
				if (!Descriptor.IsSet())
				{
					OutError = QueryFact(TEXT("query_descriptor_missing"),
						TEXT("The descriptor does not provide query mechanics."), TEXT("/query/query_binding_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.Stage = TEXT("query");
					OutError.CandidateName = Request.CommandId;
					OutError.InternalContext = DescriptorId;
					return false;
				}

				if (!TryValidateQueryOptions(Params, Descriptor.GetValue(), OutError))
				{
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Request.CommandId;
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
				FString PublicProjectionId = Route->PublicProjectionId;
				FString EditorProjectAction;
				if (Input->TryGetStringField(TEXT("editor_project_capability"), EditorProjectAction)
					&& EditorProject::IsEditorProjectQueryRoute(
						Descriptor.GetValue().GetProviderBinding(), EditorProjectAction))
				{
					PublicProjectionId = TEXT("editor_project_observation");
				}
				const FQueryPublicProjectionDescriptor* PublicProjection =
					FindQueryPublicProjectionDescriptor(PublicProjectionId, &OutError);
				if (!PublicProjection)
				{
					if (OutError.IsEmpty())
					{
						OutError = QueryFact(TEXT("query_public_projection_invalid"),
							TEXT("The query references an unregistered public projection descriptor."), TEXT("/query/public_projection_available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = PublicProjectionId;
					}
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Request.CommandId;
					return false;
				}

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
						OutError = QueryFact(TEXT("query_target_required"),
							TEXT("The query requires a non-empty target reference."), QueryFieldPath(FString(), TargetField),
							QueryValueFact(Params->TryGetField(TargetField)),
							MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input"));
						OutError.CandidateName = Request.CommandId;
						OutError.ActualEvidence = Params->TryGetField(TargetField);
						return false;
					}
					if (Request.CommandId.StartsWith(TEXT("asset.")) || Request.CommandId.StartsWith(TEXT("content_graph.")))
					{
						UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
						const FString RequiredKind = Request.CommandId.StartsWith(TEXT("asset.")) ? TEXT("asset") : TEXT("");
						if (!DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(PublicRef, RequiredKind, ResolveError))
						{
							OutError = ResolveError;
							return false;
						}
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
					if (!Candidate.IsValid())
					{
						OutError = *Candidate.GetValidationDiagnostic();
						return false;
					}
					if (!Tickets->TryIssue(
							Candidate,
							DescriptorOperation->GetIdentity(),
							Sessions->GetCurrent(),
							Caller,
							{},
							Ticket,
							TicketError)
						|| !Ticket.IsSet())
					{
						OutError = TicketError.IsEmpty()
							? UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_target_ticket_failed"),
								TEXT("The ticket service returned no query target ticket or failure diagnostic."), TEXT("query_service"))
							: TicketError;
						if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = PublicRef;
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
					ProjectionRequest(Projection, *PublicProjection));
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
							ESPMode::ThreadSafe>();
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
						FUeAgentOperationDeadline(),
						Cancellation.GetToken(),
						Result,
						QueryError)
					|| !Result.IsValid())
				{
					OutError = QueryError.IsEmpty()
						? UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_execution_failed"),
							TEXT("The query binding did not return a result or a failure diagnostic."), TEXT("query_service"))
						: QueryError;
					OutError.SubjectId = QueryError.SubjectId.IsEmpty()
						? Request.CommandId
						: QueryError.SubjectId;
					return false;
				}

				OutResponse.Diagnostics = Result->GetValue().GetDiagnostics();

				FUeAgentContractValue Encoded(TEXT("{}"));
				if (!FUeAgentQueryPayloadCodec::TryEncodePublic(
						*PublicProjection,
						Request.CommandId,
						Descriptor.GetValue().GetProviderBinding(),
						true,
						Result->GetValue(),
						Resources.Get(),
						Encoded,
						QueryError))
				{
					OutError = QueryError;
					OutError.Diagnostics.Append(OutResponse.Diagnostics);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.CommandId;
					return false;
				}
				TSharedPtr<FJsonObject> QueryResult;
				if (!Json::TryParseObject(
						Encoded.GetJsonValue(),
						QueryResult,
						ParseError))
				{
					OutError = ParseError; if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.CommandId;
					OutError.Diagnostics.Append(OutResponse.Diagnostics);
					return false;
				}
				if (!CanonicalObject(
						QueryResult,
						OutResponse.PayloadJson,
						OutError,
						Request.CommandId))
				{
					OutError.Diagnostics.Append(OutResponse.Diagnostics);
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
				UE::UAI::Contract::FDiagnostic AuthorityError;
				if (!DomainCapability::
					TryGetOrBuildProcessDomainCapabilityAuthorities(
						Authorities,
						AuthorityError)
					|| !Authorities.GetQueryProviderRegistry().IsValid())
				{
					OutError = AuthorityError; if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TEXT("domain-capability");
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
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(QueryError.Code.IsEmpty()
						? FString(TEXT("query_binding_unavailable"))
						: QueryError.Code, QueryError);
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
		Contract::FDiagnostic Diagnostic;
		return TryValidate(Diagnostic);
	}

	bool FQueryRoute::TryValidate(Contract::FDiagnostic& OutDiagnostic) const
	{
		OutDiagnostic.Reset();
		const TCHAR* InvalidField = CommandId.IsEmpty() ? TEXT("command_id")
			: MechanicsCommandId.IsEmpty() ? TEXT("mechanics_command_id")
			: PythonFunction.IsEmpty() ? TEXT("python_function") : nullptr;
		const TCHAR* Reason = InvalidField ? TEXT("The query route requires a non-empty identifier in this field.") : nullptr;
		if (!Reason && bUsesObjectPath && !bRequiresTarget)
		{
			InvalidField = TEXT("requires_target");
			Reason = TEXT("A query route using an object path must require a target.");
		}
		if (!Reason && !RequiredProperty.IsEmpty() && !bUsesObjectPath)
		{
			InvalidField = TEXT("required_property");
			Reason = TEXT("A query route with a required property must use an object path.");
		}
		if (Reason)
		{
			OutDiagnostic = Contract::FDiagnostic::Failure(TEXT("query_route_invalid"), Reason, TEXT("query_registration"));
			OutDiagnostic.PropertyPath = InvalidField;
			OutDiagnostic.CandidateName = CommandId;
			return false;
		}
		if (FindQueryPublicProjectionDescriptor(PublicProjectionId, &OutDiagnostic)) return true;
		if (OutDiagnostic.IsEmpty())
		{
			OutDiagnostic = Contract::FDiagnostic::Failure(TEXT("query_public_projection_invalid"),
				TEXT("The query route references an unregistered public projection descriptor."), TEXT("query_registration"), PublicProjectionId);
			OutDiagnostic.PropertyPath = TEXT("public_projection");
		}
		OutDiagnostic.CandidateName = CommandId;
		return false;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistrations.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> WorldDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> AssetDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> ContentGraphDescriptors;
		if (!DomainAdapters::WorldRead::TryCreateWorldReadDescriptorRegistry(
				WorldDescriptors,
				OutError)
			|| !DomainAdapters::AssetProjectRead::
				TryCreateAssetProjectReadDescriptorRegistry(
					AssetDescriptors,
					OutError)
			|| !DomainAdapters::ContentGraphRead::
				TryCreateContentGraphReadDescriptorRegistry(
					ContentGraphDescriptors,
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
		const TSharedRef<
			DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadProductionEnvironment>
			ContentGraphEnvironment = MakeShared<
				DomainAdapters::ContentGraphRead::
					FUeAgentContentGraphReadProductionEnvironment>();
		const int32 ExpectedRegistrations = WorldDescriptors->GetDescriptors().Num()
			+ AssetDescriptors->GetDescriptors().Num() + ContentGraphDescriptors->GetDescriptors().Num();
		OutRegistrations.Reserve(ExpectedRegistrations);
		int32 WorldDescriptorIndex = 0;
		for (const FUeAgentOperationDescriptor& Descriptor
			: WorldDescriptors->GetDescriptors())
		{
			const int32 CurrentDescriptorIndex = WorldDescriptorIndex++;
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				OutError = QueryFact(TEXT("query_provider_binding_missing"),
					TEXT("A query descriptor has no provider binding."),
					FString::Printf(TEXT("/providers/world_read/descriptors/%d/query_binding_available"), CurrentDescriptorIndex),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = TEXT("world_read");
				OutError.InternalContext = Descriptor.GetIdentity().GetDescriptorId();
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
					Error))
			{
				OutError = Error;
				return false;
			}
			OutRegistrations.Emplace(
				ProviderBinding,
				MakeShared<
					DomainAdapters::WorldRead::FUeAgentWorldReadProvider>(
						ProviderBinding,
						Hook.ToSharedRef()));
		}
		int32 AssetDescriptorIndex = 0;
		for (const FUeAgentOperationDescriptor& Descriptor
			: AssetDescriptors->GetDescriptors())
		{
			const int32 CurrentDescriptorIndex = AssetDescriptorIndex++;
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				OutError = QueryFact(TEXT("query_provider_binding_missing"),
					TEXT("A query descriptor has no provider binding."),
					FString::Printf(TEXT("/providers/asset_project_read/descriptors/%d/query_binding_available"), CurrentDescriptorIndex),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = TEXT("asset_project_read");
				OutError.InternalContext = Descriptor.GetIdentity().GetDescriptorId();
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
					Error))
			{
				OutError = Error;
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
		int32 ContentGraphDescriptorIndex = 0;
		for (const FUeAgentOperationDescriptor& Descriptor
			: ContentGraphDescriptors->GetDescriptors())
		{
			const int32 CurrentDescriptorIndex = ContentGraphDescriptorIndex++;
			if (!Descriptor.GetQueryBinding().IsSet())
			{
				OutError = QueryFact(TEXT("query_provider_binding_missing"),
					TEXT("A query descriptor has no provider binding."),
					FString::Printf(TEXT("/providers/content_graph_read/descriptors/%d/query_binding_available"), CurrentDescriptorIndex),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = TEXT("content_graph_read");
				OutError.InternalContext = Descriptor.GetIdentity().GetDescriptorId();
				return false;
			}
			const FString ProviderBinding =
				Descriptor.GetQueryBinding()->GetProviderBinding();
			TSharedPtr<
				DomainAdapters::ContentGraphRead::
					IUeAgentContentGraphReadHook> Hook;
			DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadError Error;
			if (!DomainAdapters::ContentGraphRead::
				FUeAgentContentGraphReadNativeHookFactory::TryCreate(
					ProviderBinding,
					ContentGraphEnvironment,
					Hook,
					Error))
			{
				OutError = Error;
				return false;
			}
			OutRegistrations.Emplace(
				ProviderBinding,
				MakeShared<
					DomainAdapters::ContentGraphRead::
						FUeAgentContentGraphReadProvider>(
							ProviderBinding,
							Hook.ToSharedRef()));
		}
		if (OutRegistrations.Num() != ExpectedRegistrations)
		{
			OutError = QueryFact(TEXT("query_provider_inventory_invalid"),
				TEXT("The query provider registration count differs from the descriptor inventory."),
				TEXT("/providers/count"), MakeShared<FJsonValueNumber>(OutRegistrations.Num()),
				MakeShared<FJsonValueNumber>(ExpectedRegistrations));
			OutRegistrations.Reset();
			return false;
		}
		return true;
	}

	bool TryCreateProductionQueryService(
		FProductionQueryComposition& OutComposition,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutComposition = {};
		OutError.Reset();

		TSharedPtr<const FUeAgentDescriptorRegistry> WorldDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> AssetDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> ContentGraphDescriptors;
		TSharedPtr<const FUeAgentDescriptorRegistry> LandscapeDescriptors;
		if (!DomainAdapters::WorldRead::TryCreateWorldReadDescriptorRegistry(
				WorldDescriptors,
				OutError)
			|| !DomainAdapters::AssetProjectRead::TryCreateAssetProjectReadDescriptorRegistry(
				AssetDescriptors,
				OutError)
			|| !DomainAdapters::ContentGraphRead::
				TryCreateContentGraphReadDescriptorRegistry(
					ContentGraphDescriptors,
					OutError)
			|| !DomainAdapters::Landscape::
				TryCreateLandscapeHeightReadDescriptorRegistry(
					LandscapeDescriptors,
				OutError))
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(49);
		if (!MergeDescriptors(TEXT("world_read"), WorldDescriptors, Descriptors, OutError)
			|| !MergeDescriptors(TEXT("asset_project_read"), AssetDescriptors, Descriptors, OutError)
			|| !MergeDescriptors(
				TEXT("content_graph_read"), ContentGraphDescriptors,
				Descriptors,
				OutError)
			|| !MergeDescriptors(
				TEXT("landscape"), LandscapeDescriptors,
				Descriptors,
				OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		if (!FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), DescriptorRegistry, OutError))
		{
			return false;
		}

		FUeAgentQueryBindingError QueryError;
		TSharedPtr<const FUeAgentDiscoveryService> DiscoveryService;
		FUeAgentDiscoveryError DiscoveryError;
		if (!FUeAgentDiscoveryService::TryCreate(
				{},
				{},
				DiscoveryService,
				DiscoveryError))
		{
			OutError = DiscoveryError;
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				MakeShared<FQueryTokenEstimator>(),
				Projector,
				ProjectionError))
		{
			OutError = ProjectionError;
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
				QueryError))
		{
			OutError = QueryError;
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
