#include "OperationOwners/DestructiveContent/UeAgentDestructiveContentOperationOwner.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportCommandHandlers.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldCommandHandlers.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
	using namespace CommandEdge;
	using namespace Composition::DestructiveImport;
	using namespace DomainAdapters::AssetDestructiveImport;
	using namespace DomainAdapters::ContentOperations;
	using namespace DomainAdapters::WorldDestructive;

	namespace
	{
		struct FProductionHost final
		{
			FCriticalSection Mutex;
			TWeakObjectPtr<UWorld> BoundWorld;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
		};

		FProductionHost& ProductionHost()
		{
			static FProductionHost Host;
			return Host;
		}

		bool TryCreateHandlersLocked(
			FProductionHost& Host,
			UWorld& World,
			FString& OutError)
		{
			OutError.Reset();
			if (Host.BoundWorld.Get() == &World && Host.Handlers.Num() == 13)
			{
				return true;
			}
			Host.BoundWorld.Reset();
			Host.Handlers.Reset();
			const FString OwnerId = TEXT("destructive-content:")
				+ FString(FApp::GetProjectName());
			TSharedPtr<IUeAgentAssetDeleteOwner> DeleteOwner;
			if (!TryCreateAssetDeleteOwner(
					FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/DestructiveContent/AssetDelete"),
					OwnerId + TEXT(":asset-delete"),
					DeleteOwner,
					OutError)
				|| !DeleteOwner.IsValid())
			{
				return false;
			}
			TSharedPtr<IUeAgentAssetOperationsOwner> AssetOwner;
			if (!TryCreateAssetOperationsOwner(
					FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/DestructiveContent/AssetOperations"),
					OwnerId + TEXT(":asset-operations"),
					AssetOwner,
					OutError)
				|| !AssetOwner.IsValid())
			{
				return false;
			}
			TSharedPtr<IUeAgentWorldDestructiveOwner> WorldOwner;
			if (!TryCreateWorldDestructiveOwner(
					World,
					FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/DestructiveContent/World"),
					OwnerId + TEXT(":world"),
					WorldOwner,
					OutError)
				|| !WorldOwner.IsValid())
			{
				return false;
			}

			TArray<TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<TSharedRef<IUeAgentCommandHandler>> WorldHandlers;
			if (!TryCreateDestructiveImportCommandHandlers(
					DeleteOwner.ToSharedRef(),
					AssetOwner.ToSharedRef(),
					Handlers,
					OutError)
				|| !TryCreateDestructiveImportWorldCommandHandlers(
					WorldOwner.ToSharedRef(),
					WorldHandlers,
					OutError))
			{
				return false;
			}
			Handlers.Append(WorldHandlers);
			const TSet<FString> ExactIds(GetDestructiveContentCommandIds());
			for (const TSharedRef<IUeAgentCommandHandler>& Handler : Handlers)
			{
				if (ExactIds.Contains(Handler->GetCommandId()))
				{
					Host.Handlers.Add(Handler->GetCommandId(), Handler);
				}
			}
			if (Host.Handlers.Num() != 13)
			{
				Host.Handlers.Reset();
				OutError = TEXT("destructive_content_legacy_handler_count_mismatch");
				return false;
			}
			Host.BoundWorld = &World;
			return true;
		}

		TSharedPtr<FJsonObject> ParseObject(const FString& Text)
		{
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, Object)
				? Object : TSharedPtr<FJsonObject>();
		}

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}

		TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		FString Payload(
			const FUeAgentDestructiveContentPlan& Plan,
			const FString& TerminalStatus,
			const TSharedPtr<FJsonObject>& Projection)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("operation_id"), Plan.PlanId);
			Root->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Root->SetStringField(
				TEXT("destructive_authorization_id"), Plan.AuthorizationId);
			Root->SetStringField(TEXT("plan_fingerprint"), Plan.PlanFingerprint);
			Root->SetArrayField(
				TEXT("impact_scope"), StringValues(Plan.ImpactScope));
			Root->SetArrayField(
				TEXT("revision_snapshot"), StringValues(Plan.RevisionSnapshot));
			Root->SetObjectField(
				TEXT("projection"),
				Projection.IsValid() ? Projection.ToSharedRef() : MakeShared<FJsonObject>());
			return JsonText(Root);
		}

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail,
			const FUeAgentDestructiveContentPlan* Plan = nullptr)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Error->SetBoolField(TEXT("retry_safe"), true);
			const FString ResultPayload = Plan
				? Payload(*Plan, TEXT("rejected"), nullptr)
				: TEXT("{}");
			return FUeAgentCommandResult::Failure(
				Request, ResultPayload, JsonText(Error));
		}
	}

	const TArray<FString>& GetDestructiveContentCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("asset.delete"),
			TEXT("asset.fixup_redirectors"),
			TEXT("asset.move"),
			TEXT("asset.object.clone"),
			TEXT("modeling.actor.mesh.replace"),
			TEXT("modeling.asset.material_slot.remove"),
			TEXT("modeling.voxel.bounds.remove"),
			TEXT("skeletal_mesh.remove_morph_target"),
			TEXT("skeletal_mesh.remove_skin_weight_profile"),
			TEXT("world.data_layer.delete"),
			TEXT("world.level_content.delete_scope"),
			TEXT("world.level_streaming.move_actors"),
			TEXT("world.level_streaming.remove"),
			TEXT("world.pcg.cleanup"),
		};
		return Ids;
	}

	FUeAgentCommandResult SubmitProductionDestructiveContentOperation(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		if (!IsInGameThread())
		{
			return Failure(
				Request,
				TEXT("destructive_content_game_thread_required"),
				TEXT("destructive UE mechanics must start on the game thread"));
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			return Failure(
				Request,
				TEXT("destructive_content_world_unavailable"),
				TEXT("the current Editor world is unavailable"));
		}

		FUeAgentDestructiveContentPlan Plan;
		FString Error;
		if (!TryBuildDestructiveContentPlan(
				Request.GetCommandId(), Request.GetParams(), World, Plan, Error))
		{
			return Failure(
				Request, TEXT("destructive_plan_rejected"), Error);
		}
		FString AuthorizationId;
		if (!Request.GetParams()->TryGetStringField(
				TEXT("destructive_authorization_id"), AuthorizationId)
			|| AuthorizationId.TrimStartAndEnd().IsEmpty())
		{
			const TSharedRef<FJsonObject> Projection = MakeShared<FJsonObject>();
			Projection->SetStringField(TEXT("status"), TEXT("planned"));
			if (Request.GetCommandId() == TEXT("asset.object.clone"))
			{
				FString SourceRef;
				if (Request.GetParams()->TryGetStringField(
						TEXT("source_ref"), SourceRef))
				{
					if (UObject* const Source = LoadObject<UObject>(nullptr, *SourceRef))
					{
						Projection->SetStringField(
							TEXT("source_revision"),
							AssetDurable::ReadStableAssetRevision(Source));
					}
				}
			}
			Projection->SetStringField(
				TEXT("instruction"),
				TEXT("review the complete impact scope and repeat the exact command with destructive_authorization_id"));
			return FUeAgentCommandResult::Success(
				Request, Payload(Plan, TEXT("planned"), Projection));
		}
		AuthorizationId.TrimStartAndEndInline();
		if (AuthorizationId != Plan.AuthorizationId)
		{
			return Failure(
				Request,
				TEXT("destructive_authorization_mismatch"),
				TEXT("the authorization is not bound to the current impact scope, plan, and target revisions"),
				&Plan);
		}

		FUeAgentCommandResult MechanicsResult = Request.GetCommandId()
			== TEXT("asset.object.clone")
			? [&]()
			{
				TArray<FString> EffectiveScopes =
					CallerContext.GetAuthorityScopes();
				for (const FString& Scope : DomainAdapters::AssetSimple::
					GetAssetSimpleRequiredAuthorityScopes())
				{
					EffectiveScopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					CallerContext.GetCallerId(), MoveTemp(EffectiveScopes),
					CallerContext.GetCorrelationId());
				return AssetSimple::SubmitProductionAssetSimpleOperation(
					Request, EffectiveCaller, Cancellation);
			}()
			: [&]()
			{
				TSharedPtr<IUeAgentCommandHandler> Handler;
				{
					FProductionHost& Host = ProductionHost();
					FScopeLock Lock(&Host.Mutex);
					if (TryCreateHandlersLocked(Host, *World, Error))
					{
						const TSharedRef<IUeAgentCommandHandler>* Found =
							Host.Handlers.Find(Request.GetCommandId());
						if (Found)
						{
							Handler = *Found;
						}
					}
				}
				return Handler.IsValid()
					? Handler->Submit(Request, CallerContext, Cancellation)
					: Failure(
						Request,
						TEXT("destructive_content_mechanics_unavailable"),
						Error.IsEmpty()
							? TEXT("the exact destructive command has no mechanics binding")
							: Error,
						&Plan);
			}();

		TSharedPtr<FJsonObject> Projection = ParseObject(
			MechanicsResult.GetPayloadJson());
		FString Terminal = MechanicsResult.IsOk()
			? TEXT("committed") : TEXT("failed");
		if (Projection.IsValid())
		{
			Projection->TryGetStringField(TEXT("terminal_state"), Terminal);
		}
		const FString ResultPayload = Payload(Plan, Terminal, Projection);
		return MechanicsResult.IsOk()
			? FUeAgentCommandResult::Success(Request, ResultPayload)
			: FUeAgentCommandResult::Failure(
				Request, ResultPayload, MechanicsResult.GetErrorJson());
	}
}
