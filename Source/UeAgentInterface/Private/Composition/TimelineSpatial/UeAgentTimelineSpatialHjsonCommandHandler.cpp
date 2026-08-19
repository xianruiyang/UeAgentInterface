#include "Composition/TimelineSpatial/UeAgentTimelineSpatialHjsonCommandHandler.h"

#include "Async/Async.h"
#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace CommandEdge;
	using namespace Composition::HjsonImport;
	using namespace ContextResource;
	using namespace Contract;
	using namespace HjsonWorkspace;
	using namespace PlanningPreflight;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error)
				? Text
				: FString();
		}

		FUeAgentCommandResult ErrorResult(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				JsonText(Error));
		}

		FUeAgentResourceBudget WorkspaceBudget(const int64 MaxFrameBytes)
		{
			const int64 ByteLimit = FMath::Max<int64>(
				MaxFrameBytes * 16,
				16 * 1024 * 1024);
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, ByteLimit},
				{EUeAgentResourceDimension::OutputBytes, ByteLimit},
				{EUeAgentResourceDimension::StringBytes, ByteLimit},
				{EUeAgentResourceDimension::Nodes, 131072},
				{EUeAgentResourceDimension::Items, 131072},
				{EUeAgentResourceDimension::Files, 512},
				{EUeAgentResourceDimension::ReferenceEdges, 131072},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					ByteLimit * 8,
				},
			});
		}

		bool TryParseSource(
			const TSharedPtr<FJsonObject>& SourceObject,
			TOptional<FWorkspaceSource>& OutSource)
		{
			OutSource.Reset();
			if (!SourceObject.IsValid())
			{
				return false;
			}
			FString Kind;
			if (!SourceObject->TryGetStringField(TEXT("kind"), Kind))
			{
				return false;
			}
			if (Kind == TEXT("directory"))
			{
				FString Path;
				if (SourceObject->TryGetStringField(TEXT("path"), Path)
					&& !Path.TrimStartAndEnd().IsEmpty())
				{
					OutSource.Emplace(FWorkspaceSource::Directory(Path));
				}
			}
			else if (Kind == TEXT("single_file"))
			{
				FString Path;
				if (SourceObject->TryGetStringField(TEXT("path"), Path)
					&& !Path.TrimStartAndEnd().IsEmpty())
				{
					OutSource.Emplace(FWorkspaceSource::SingleFile(Path));
				}
			}
			else if (Kind == TEXT("inline"))
			{
				FString Text;
				FString SourceUnit;
				FString SourceId;
				if (SourceObject->TryGetStringField(TEXT("text"), Text)
					&& SourceObject->TryGetStringField(
						TEXT("source_unit"),
						SourceUnit)
					&& SourceObject->TryGetStringField(
						TEXT("source_id"),
						SourceId)
					&& !Text.IsEmpty()
					&& !SourceUnit.TrimStartAndEnd().IsEmpty()
					&& !SourceId.TrimStartAndEnd().IsEmpty())
				{
					OutSource.Emplace(FWorkspaceSource::Inline(
						Text,
						SourceUnit,
						SourceId));
				}
			}
			return OutSource.IsSet();
		}

		bool TryResolveComposition(
			const FWorkspaceSource& Source,
			FUeAgentResourceTracker& Resources,
			FResolvedSemanticWorkspacePtr& OutWorkspace,
			FString& OutCompositionKind,
			FWorkspaceError& OutError)
		{
			OutWorkspace.Reset();
			OutCompositionKind.Reset();
			if (!TryResolveEditedSemanticWorkspace(
					Source,
					ESemanticWorkspaceMode::State,
					Resources,
					OutWorkspace,
					OutError)
				|| !OutWorkspace.IsValid())
			{
				return false;
			}
			int32 TargetCount = 0;
			for (const FSemanticUnit& Unit :
				OutWorkspace->GetModel()->GetUnits())
			{
				if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState)
				{
					++TargetCount;
					OutCompositionKind = Unit.GetSemanticTypeRef();
				}
			}
			if (TargetCount != 1 || OutCompositionKind.IsEmpty())
			{
				OutError.Set(
					TEXT("hjson_import_target_unit_count_invalid"),
					FString(),
					TEXT("/target_unit_ids"),
					TargetCount);
				return false;
			}
			return true;
		}

		class FHjsonCommandHandler final : public IUeAgentCommandHandler
		{
		public:
			FHjsonCommandHandler(
				FUeAgentTimelineSpatialHjsonCommandHandlerConfig InConfig,
				TSharedRef<FUeAgentHjsonImportOperationRuntime> InRuntime)
				: Config(MoveTemp(InConfig))
				, Runtime(MoveTemp(InRuntime))
			{
			}

			const FString& GetCommandId() const override
			{
				static const FString CommandId =
					TEXT("content_graph.import_workspace");
				return CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!IsInGameThread())
				{
					const TSharedRef<
						TOptional<FUeAgentCommandResult>,
						ESPMode::ThreadSafe> Result =
							MakeShared<
								TOptional<FUeAgentCommandResult>,
								ESPMode::ThreadSafe>();
					FEvent* Completed =
						FPlatformProcess::GetSynchEventFromPool(true);
					FHjsonCommandHandler* Self = this;
					AsyncTask(
						ENamedThreads::GameThread,
						[
							Self,
							Request,
							CallerContext,
							Cancellation,
							Result,
							Completed
						]()
						{
							Result->Emplace(Self->Submit(
								Request,
								CallerContext,
								Cancellation));
							Completed->Trigger();
						});
					Completed->Wait();
					FPlatformProcess::ReturnSynchEventToPool(Completed);
					return Result->GetValue();
				}

				if (!CallerContext.GetAuthorityScopes().Contains(
						TEXT("uai:authoring:hjson:import")))
				{
					return ErrorResult(
						Request,
						TEXT("authority_scope_missing"),
						TEXT("uai:authoring:hjson:import"),
						TEXT("HJSON import authority is required."));
				}
				if (Cancellation->IsCancelled())
				{
					return ErrorResult(
						Request,
						TEXT("operation_cancelled"),
						Request.GetRequestId(),
						TEXT("Cancellation was requested."));
				}

				TSharedPtr<FJsonObject> Root;
				const TSharedRef<TJsonReader<>> Reader =
					TJsonReaderFactory<>::Create(Request.GetParamsJson());
				if (!FJsonSerializer::Deserialize(Reader, Root)
					|| !Root.IsValid())
				{
					return ErrorResult(
						Request,
						TEXT("hjson_import_payload_invalid"),
						Request.GetRequestId(),
						TEXT("Import parameters must be an object."));
				}
				FString Target;
				FString Intent;
				FString ExpectedRevision;
				bool bDryRun = false;
				const TSharedPtr<FJsonObject>* SourceObject = nullptr;
				if (!Root->TryGetStringField(TEXT("target"), Target)
					|| !Root->TryGetStringField(TEXT("intent"), Intent)
					|| !Root->TryGetStringField(
						TEXT("expected_revision"),
						ExpectedRevision)
					|| !Root->TryGetObjectField(
						TEXT("source"),
						SourceObject)
					|| !SourceObject
					|| !SourceObject->IsValid())
				{
					return ErrorResult(
						Request,
						TEXT("hjson_import_fields_missing"),
						Request.GetRequestId(),
						TEXT(
							"target, source, intent and expected_revision "
							"are required."));
				}
				Root->TryGetBoolField(TEXT("dry_run"), bDryRun);
				Target.TrimStartAndEndInline();
				ExpectedRevision.TrimStartAndEndInline();
				Intent.TrimStartAndEndInline();
				EChangeIntent ChangeIntent = EChangeIntent::Update;
				if (Intent == TEXT("replace"))
				{
					ChangeIntent = EChangeIntent::Replace;
				}
				if (Target.IsEmpty() || ExpectedRevision.IsEmpty()
					|| !(Intent == TEXT("update")
						|| Intent == TEXT("replace")))
				{
					return ErrorResult(
						Request,
						TEXT("hjson_import_contract_invalid"),
						Intent,
						TEXT(
							"Current persisted targets support explicit "
							"update or replace intent."));
				}

				TOptional<FWorkspaceSource> Source;
				if (!TryParseSource(*SourceObject, Source))
				{
					return ErrorResult(
						Request,
						TEXT("hjson_import_source_invalid"),
						Request.GetRequestId(),
						TEXT(
							"source must be directory, single_file or "
							"inline."));
				}
				const TSharedRef<
					FUeAgentResourceTracker,
					ESPMode::ThreadSafe> Resources =
						MakeShared<
							FUeAgentResourceTracker,
							ESPMode::ThreadSafe>(
								WorkspaceBudget(Config.MaxFrameBytes));
				FString CompositionKind;
				FResolvedSemanticWorkspacePtr Workspace;
				FWorkspaceError WorkspaceError;
				if (!TryResolveComposition(
						Source.GetValue(),
						Resources.Get(),
						Workspace,
						CompositionKind,
						WorkspaceError))
				{
					return ErrorResult(
						Request,
						WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				const FUeAgentHjsonImportTargetRegistry& Registry =
					GetHjsonImportTargetRegistry();
				if (!Registry.FindExact(CompositionKind))
				{
					return ErrorResult(
						Request,
						TEXT("hjson_import_target_not_found"),
						CompositionKind,
						TEXT(
							"No exact persisted target owns this "
							"composition kind."));
				}
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
				const FUeAgentOperationDeadline Deadline =
					FUeAgentOperationDeadline::FromTimeout(
						Config.TimeoutSeconds,
						Clock);
				FUeAgentHjsonImportC3OResult OperationResult;
				FUeAgentHjsonImportTargetError ImportError;
				if (!Runtime->TrySubmit(
						FUeAgentHjsonTargetImportRequest(
							CompositionKind,
							Target,
							Workspace.ToSharedRef(),
							ChangeIntent,
							ExpectedRevision,
							bDryRun),
						Request.GetRequestId(),
						CallerContext,
						Resources,
						Deadline,
						CancellationSource.GetToken(),
						OperationResult,
						ImportError)
					|| !OperationResult.IsValid())
				{
					return ErrorResult(
						Request,
						ImportError.Code,
						ImportError.SubjectId,
						ImportError.Detail);
				}
				const FUeAgentHjsonTargetImportReceipt& Receipt =
					OperationResult.ImportReceipt;

				const TSharedRef<FJsonObject> Import =
					MakeShared<FJsonObject>();
				Import->SetStringField(
					TEXT("composition_kind"),
					CompositionKind);
				Import->SetStringField(
					TEXT("target"),
					Target);
				Import->SetStringField(
					TEXT("workspace_fingerprint"),
					Receipt.GetWorkspaceFingerprint());
				Import->SetStringField(
					TEXT("pre_revision"),
					Receipt.GetPreRevision());
				Import->SetStringField(
					TEXT("persisted_revision"),
					Receipt.GetPersistedRevision());
				Import->SetBoolField(
					TEXT("mutation_applied"),
					Receipt.WasMutationApplied());
				Import->SetBoolField(
					TEXT("compile_settled"),
					Receipt.WasCompileSettled());
				Import->SetBoolField(
					TEXT("package_saved"),
					Receipt.WasPackageSaved());
				Import->SetBoolField(
					TEXT("unloaded_reloaded"),
					Receipt.WasUnloadedReloaded());
				Import->SetStringField(
					TEXT("operation_id"),
					OperationResult.OperationId);
				Import->SetStringField(
					TEXT("plan_fingerprint"),
					OperationResult.PlanFingerprint);
				Import->SetStringField(
					TEXT("verification_status"),
					OperationResult.VerificationStatus);
				Import->SetBoolField(TEXT("dry_run"), bDryRun);
				const TSharedRef<FJsonObject> Response =
					MakeShared<FJsonObject>();
				Response->SetObjectField(TEXT("import"), Import);
				return FUeAgentCommandResult::Success(
					Request,
					JsonText(Response));
			}

		private:
			FUeAgentTimelineSpatialHjsonCommandHandlerConfig Config;
			TSharedRef<FUeAgentHjsonImportOperationRuntime> Runtime;
		};
	}

	bool FUeAgentTimelineSpatialHjsonCommandHandlerConfig::IsValid() const
	{
		return Operation.IsValid()
			&& TimeoutSeconds > 0.0
			&& MaxFrameBytes > 0;
	}

	bool TryCreateTimelineSpatialHjsonCommandHandler(
		const FUeAgentTimelineSpatialHjsonCommandHandlerConfig& Config,
		TSharedRef<const BindingGateway::Ticket::
			FUeAgentOperationTicketService> TicketService,
		TSharedRef<const Projection::FUeAgentResponseProjector> Projector,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		FString& OutError)
	{
		OutHandler.Reset();
		OutError = TEXT("legacy_hjson_import_retired");
		return false;
	}
}
