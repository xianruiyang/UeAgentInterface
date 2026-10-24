#include "OperationOwners/DestructiveContent/UeAgentDestructiveContentOperationOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportCommandHandlers.h"
#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldCommandHandlers.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"
#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveDurableRunner.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
	using namespace CommandEdge;
	using namespace Composition::DestructiveImport;
	using namespace DomainAdapters::AssetDestructiveImport;
	using namespace DomainAdapters::ContentOperations;
	using namespace DomainAdapters::WorldDestructive;

	namespace
	{
		struct FPendingConfirmation final
		{
			TWeakObjectPtr<UWorld> World;
			FString CallerId;
			FString CommandId;
			FString CanonicalParamsJson;
			FString PlanFingerprint;
		};

		FCriticalSection PendingConfirmationMutex;
		TArray<FPendingConfirmation> PendingConfirmations;

		void PruneInvalidConfirmations()
		{
			PendingConfirmations.RemoveAll(
				[](const FPendingConfirmation& Pending)
				{
					return !Pending.World.IsValid();
				});
		}

		void BindPendingConfirmation(
			UWorld& World,
			const Contract::FUeAgentCallerContext& CallerContext,
			const FString& CommandId,
			const FString& CanonicalParamsJson,
			const FString& PlanFingerprint)
		{
			FScopeLock Lock(&PendingConfirmationMutex);
			PruneInvalidConfirmations();
			PendingConfirmations.RemoveAll(
				[&](const FPendingConfirmation& Pending)
				{
					return Pending.World.Get() == &World
						&& Pending.CallerId == CallerContext.GetCallerId()
						&& Pending.CommandId == CommandId;
				});
			PendingConfirmations.Add(FPendingConfirmation{
				&World,
				CallerContext.GetCallerId(),
				CommandId,
				CanonicalParamsJson,
				PlanFingerprint,
			});
		}

		bool ConsumePendingConfirmation(
			UWorld& World,
			const Contract::FUeAgentCallerContext& CallerContext,
			const FString& CommandId,
			const FString& CanonicalParamsJson,
			const FString& PlanFingerprint,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			FScopeLock Lock(&PendingConfirmationMutex);
			PruneInvalidConfirmations();
			const int32 Index = PendingConfirmations.IndexOfByPredicate(
				[&](const FPendingConfirmation& Pending)
				{
					return Pending.World.Get() == &World
						&& Pending.CallerId == CallerContext.GetCallerId()
						&& Pending.CommandId == CommandId
						&& Pending.CanonicalParamsJson == CanonicalParamsJson;
				});
			if (Index == INDEX_NONE)
			{
				const int32 ChangedParamsIndex =
					PendingConfirmations.IndexOfByPredicate(
						[&](const FPendingConfirmation& Pending)
						{
							return Pending.World.Get() == &World
								&& Pending.CallerId
									== CallerContext.GetCallerId()
								&& Pending.CommandId == CommandId;
						});
				if (ChangedParamsIndex != INDEX_NONE)
				{
					PendingConfirmations.RemoveAt(ChangedParamsIndex);
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_confirmation_stale"), TEXT("The destructive operation parameters changed after the plan was prepared."), TEXT("destructive_confirmation"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("pending_parameters_match");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_confirmation_missing"), TEXT("No matching destructive plan is awaiting confirmation."), TEXT("destructive_confirmation"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("pending_confirmation_available");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			if (PendingConfirmations[Index].PlanFingerprint != PlanFingerprint)
			{
				PendingConfirmations.RemoveAt(Index);
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_confirmation_stale"), TEXT("The destructive plan scope or target revisions changed after the plan was prepared."), TEXT("destructive_confirmation"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("pending_plan_matches");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			PendingConfirmations.RemoveAt(Index);
			return true;
		}

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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (Host.BoundWorld.Get() == &World)
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
			TSharedPtr<IUeAgentWorldDestructiveDurableRunner> DurableRunner;
			const FString WorldJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/DestructiveContent/World");
			const FString WorldOwnerId = OwnerId + TEXT(":world");
			if (!TryCreateWorldDestructiveDurableRunner(
					WorldJournalRoot,
					WorldOwnerId,
					DurableRunner,
					OutError)
				|| !DurableRunner.IsValid())
			{
				return false;
			}
			if (!TryCreateWorldDestructiveOwner(
					World,
					WorldJournalRoot,
					WorldOwnerId,
					DurableRunner.ToSharedRef(),
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

		TArray<TSharedPtr<FJsonValue>> PlanTargetValues(
			const TArray<FUeAgentDestructiveContentPlanTarget>& Targets)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Targets.Num());
			for (const FUeAgentDestructiveContentPlanTarget& Target : Targets)
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("target_ref"), Target.TargetRef);
				Object->SetStringField(TEXT("package_name"), Target.PackageName);
				Object->SetStringField(TEXT("object_path"), Target.ObjectPath);
				Object->SetArrayField(TEXT("sources"), StringValues(Target.Sources));
				if (Target.BlockerDiagnostic.IsValid())
				{
					Object->SetStringField(TEXT("blocker_code"), Target.BlockerDiagnostic.Code);
					Object->SetStringField(TEXT("blocker_detail"), Target.BlockerDiagnostic.PublicMessage);
					Object->SetObjectField(TEXT("blocker_diagnostic"), Target.BlockerDiagnostic.ToPublicJson());
				}
				Result.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Result;
		}

		FString PublicPayload(
			const FUeAgentDestructiveContentPlan& Plan,
			const FString& TerminalStatus,
			const bool bIncludeImpact,
			const FString& Instruction = FString(),
			const TSharedPtr<FJsonObject>& NativeProjection = nullptr)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("status"), TerminalStatus);
			if (bIncludeImpact)
			{
				Root->SetArrayField(
					TEXT("impact_scope"), StringValues(Plan.ImpactScope));
				if (!Plan.Targets.IsEmpty())
				{
					const TArray<TSharedPtr<FJsonValue>>* NativeTargets = nullptr;
					if (NativeProjection.IsValid()
						&& NativeProjection->TryGetArrayField(
							TEXT("targets"), NativeTargets)
						&& NativeTargets)
					{
						TArray<TSharedPtr<FJsonValue>> Targets;
						for (const TSharedPtr<FJsonValue>& Value : *NativeTargets)
						{
							const TSharedPtr<FJsonObject>* NativeTarget = nullptr;
							if (!Value.IsValid()
								|| !Value->TryGetObject(NativeTarget)
								|| !NativeTarget
								|| !NativeTarget->IsValid())
							{
								continue;
							}
							const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
							for (const TCHAR* Field : {
								TEXT("target_ref"), TEXT("package_name"),
								TEXT("object_path"), TEXT("status"),
								TEXT("deleted"), TEXT("remaining"),
								TEXT("error_code"), TEXT("error_detail"),
								TEXT("terminal_state"), TEXT("readback")})
							{
								const TSharedPtr<FJsonValue>* FieldValue =
									(*NativeTarget)->Values.Find(Field);
								if (FieldValue && FieldValue->IsValid())
								{
									Target->SetField(Field, *FieldValue);
								}
							}
							Targets.Add(MakeShared<FJsonValueObject>(Target));
						}
						if (!Targets.IsEmpty())
						{
							Root->SetArrayField(TEXT("targets"), MoveTemp(Targets));
							for (const TCHAR* Field : {
								TEXT("deleted_count"), TEXT("blocked_count"),
								TEXT("remaining_count"), TEXT("recursive"),
								TEXT("terminal_state"), TEXT("failure_reason")})
							{
								const TSharedPtr<FJsonValue>* FieldValue =
									NativeProjection->Values.Find(Field);
								if (FieldValue && FieldValue->IsValid())
								{
									Root->SetField(Field, *FieldValue);
								}
							}
						}
						else
						{
							Root->SetArrayField(
								TEXT("targets"), PlanTargetValues(Plan.Targets));
						}
					}
					else
					{
						Root->SetArrayField(
							TEXT("targets"), PlanTargetValues(Plan.Targets));
					}
				}
			}
			if (!Instruction.IsEmpty())
			{
				Root->SetStringField(TEXT("instruction"), Instruction);
			}
			return JsonText(Root);
		}

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FUeAgentDestructiveContentPlan* Plan = nullptr)
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("destructive_preflight");
            Diagnostic.bRetrySafe = true;
            Diagnostic.TerminalStatus = TEXT("not_started");
            const FString ResultPayload = Plan ? PublicPayload(*Plan, TEXT("rejected"), false) : TEXT("{}");
            return FUeAgentCommandResult::Failure(Request, ResultPayload, MoveTemp(Diagnostic));
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
			return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_content_game_thread_required"), TEXT("destructive UE mechanics must start on the game thread"), TEXT("destructive_preflight")));
		}
		UWorld* const World = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!World)
		{
			return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_content_world_unavailable"), TEXT("the current Editor world is unavailable"), TEXT("destructive_preflight")));
		}

		FUeAgentDestructiveContentPlan Plan;
		UE::UAI::Contract::FDiagnostic Error;
		if (!TryBuildDestructiveContentPlan(
				Request.GetCommandId(), Request.GetParams(), World, Plan, Error))
		{
			return Failure(Request, Error);
		}
		const bool bRequiresConfirmation =
			Request.GetCommandId() != TEXT("asset.object.clone")
			&& Request.GetCommandId() != TEXT("asset.move")
			&& Request.GetCommandId() != TEXT("world.level_streaming.move_actors");
		bool bConfirm = !bRequiresConfirmation;
		if (!Request.GetParams()->TryGetBoolField(TEXT("confirm"), bConfirm))
		{
			bConfirm = !bRequiresConfirmation;
		}
		if (!bConfirm)
		{
			BindPendingConfirmation(
				*World,
				CallerContext,
				Request.GetCommandId(),
				Plan.CanonicalParamsJson,
				Plan.PlanFingerprint);
			return FUeAgentCommandResult::Success(
				Request,
				PublicPayload(
					Plan,
					TEXT("planned"),
					true,
					TEXT("review the impact scope and repeat the unchanged command with confirm=true")));
		}
		UE::UAI::Contract::FDiagnostic ConfirmationError;
		if (bRequiresConfirmation && !ConsumePendingConfirmation(
				*World,
				CallerContext,
				Request.GetCommandId(),
				Plan.CanonicalParamsJson,
				Plan.PlanFingerprint,
				ConfirmationError))
		{
			return Failure(Request, ConfirmationError, &Plan);
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
				// Confirmation belongs to this owner. Forward only the business parameters
				// whose caller, target revisions and fingerprint were just verified.
				TSharedPtr<FJsonObject> ConfirmedParams;
				if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Plan.CanonicalParamsJson), ConfirmedParams)
					|| !ConfirmedParams.IsValid())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_confirmed_params_invalid"), Request.GetCommandId(), TEXT("destructive_preflight")), &Plan);
				}
				const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
				Envelope->SetStringField(TEXT("schema"), TEXT("uai.command.v1"));
				Envelope->SetNumberField(TEXT("version"), 1);
				Envelope->SetStringField(TEXT("message_kind"), TEXT("command_request"));
				Envelope->SetStringField(TEXT("correlation_id"), Request.GetCorrelationId());
				Envelope->SetStringField(TEXT("request_id"), Request.GetRequestId());
				Envelope->SetStringField(TEXT("command_id"), Request.GetCommandId());
				Envelope->SetStringField(TEXT("command_binding_hash"), Request.GetCommandBindingHash());
				Envelope->SetObjectField(TEXT("params"), ConfirmedParams);
				const FString EnvelopeJson = JsonText(Envelope);
				FTCHARToUTF8 Encoded(*EnvelopeJson);
				TArray<uint8> Bytes;
				Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
				TOptional<FUeAgentCommandRequest> ConfirmedRequest;
				FUeAgentCommandProtocolDiagnostic Diagnostic;
				if (!FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, ConfirmedRequest, Diagnostic)
					|| !ConfirmedRequest.IsSet())
				{
					return Failure(Request, Diagnostic, &Plan);
				}
				return AssetSimple::SubmitProductionAssetSimpleOperation(
					ConfirmedRequest.GetValue(), EffectiveCaller, Cancellation);
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
					: Failure(Request, Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_content_mechanics_unavailable"), TEXT("The exact destructive command has no mechanics binding."), TEXT("destructive_preflight")), &Plan);
			}();

		TSharedPtr<FJsonObject> Projection = ParseObject(
			MechanicsResult.GetPayloadJson());
		FString Terminal = MechanicsResult.IsOk()
			? TEXT("committed") : TEXT("failed");
		if (Projection.IsValid())
		{
			Projection->TryGetStringField(TEXT("terminal_state"), Terminal);
		}
		const FString ResultPayload = PublicPayload(
			Plan, Terminal, true, FString(), Projection);
		return MechanicsResult.IsOk()
			? FUeAgentCommandResult::Success(Request, ResultPayload)
			: FUeAgentCommandResult::Failure(
				Request, ResultPayload, MechanicsResult.GetDiagnostic(), MechanicsResult.GetErrorCode(), MechanicsResult.GetApplicationFailureKind());
	}
}
