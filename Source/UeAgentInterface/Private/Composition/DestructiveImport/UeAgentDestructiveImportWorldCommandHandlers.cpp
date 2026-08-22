#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace DomainAdapters::WorldDestructive;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error) ? Text : TEXT("{}");
		}

		FString PayloadJson(const FUeAgentWorldDestructiveResult& Result)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("command_id"), Result.CommandId);
			Root->SetStringField(TEXT("target_ref"), Result.TargetRef);
			Root->SetStringField(TEXT("result_ref"), Result.ResultRef);
			Root->SetStringField(TEXT("terminal_state"), Result.TerminalState);
			Root->SetStringField(
				TEXT("persistence_scope"), Result.PersistenceScope);
			Root->SetBoolField(TEXT("changed"), Result.bChanged);
			Root->SetBoolField(TEXT("persisted"), Result.bPersisted);
			Root->SetBoolField(
				TEXT("readback_verified"), Result.bReadbackVerified);
			Root->SetStringField(TEXT("journal_id"), Result.JournalId);
			Root->SetStringField(
				TEXT("verification_id"), Result.VerificationId);
			return JsonText(Root);
		}

		FString ErrorJson(const FUeAgentWorldDestructiveResult& Result)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Result.ErrorCode);
			Error->SetStringField(TEXT("detail"), Result.ErrorDetail);
			Error->SetStringField(TEXT("target_ref"), Result.TargetRef);
			return JsonText(Error);
		}

		bool TryString(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* Field,
			FString& Out)
		{
			return Params.IsValid()
				&& Params->TryGetStringField(Field, Out)
				&& !Out.TrimStartAndEnd().IsEmpty();
		}

		bool TryBuildRequest(
			const FUeAgentCommandRequest& CommandRequest,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentWorldDestructiveRequest& Out)
		{
			Out.RequestId = CommandRequest.GetRequestId();
			Out.CommandId = CommandRequest.GetCommandId();
			Out.CallerId = CallerContext.GetCallerId();
			const TSharedPtr<FJsonObject> Params = CommandRequest.GetParams();
			if (Out.CommandId == TEXT("world.data_layer.delete"))
			{
				return TryString(Params, TEXT("layer"), Out.TargetRef);
			}
			if (Out.CommandId == TEXT("world.level_content.delete_scope"))
			{
				if (!TryString(Params, TEXT("folder_path"), Out.TargetRef))
				{
					return false;
				}
				if (Params->HasField(TEXT("managed_id"))
					&& !TryString(Params, TEXT("managed_id"), Out.ManagedId))
				{
					return false;
				}
				return true;
			}
			if (Out.CommandId == TEXT("world.pcg.cleanup"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef)
					&& TryString(
						Params, TEXT("component_name"), Out.ComponentName);
			}
			if (Out.CommandId == TEXT("world.level_streaming.move_actors"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef)
					&& TryString(
						Params, TEXT("level_package"), Out.OperandRef);
			}
			if (Out.CommandId == TEXT("world.level_streaming.remove"))
			{
				return TryString(
					Params, TEXT("level_package"), Out.TargetRef);
			}
			if (Out.CommandId == TEXT("modeling.actor.mesh.replace"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef)
					&& TryString(Params, TEXT("mesh_ref"), Out.OperandRef);
			}
			if (Out.CommandId == TEXT("modeling.asset.material_slot.remove"))
			{
				double Index = -1;
				if (!TryString(Params, TEXT("target_ref"), Out.TargetRef)
					|| !Params->TryGetNumberField(TEXT("index"), Index))
				{
					return false;
				}
				Out.Index = static_cast<int32>(Index);
				return Index == static_cast<double>(Out.Index);
			}
			if (Out.CommandId == TEXT("modeling.voxel.bounds.remove"))
			{
				double MinX = 0;
				double MinY = 0;
				double MinZ = 0;
				double MaxX = 0;
				double MaxY = 0;
				double MaxZ = 0;
				if (!TryString(Params, TEXT("actor_ref"), Out.TargetRef)
					|| !TryString(
						Params, TEXT("component_name"), Out.ComponentName)
					|| !TryString(Params, TEXT("space"), Out.Space)
					|| !Params->TryGetNumberField(TEXT("min_x"), MinX)
					|| !Params->TryGetNumberField(TEXT("min_y"), MinY)
					|| !Params->TryGetNumberField(TEXT("min_z"), MinZ)
					|| !Params->TryGetNumberField(TEXT("max_x"), MaxX)
					|| !Params->TryGetNumberField(TEXT("max_y"), MaxY)
					|| !Params->TryGetNumberField(TEXT("max_z"), MaxZ))
				{
					return false;
				}
				Out.BoundsMin = FVector(MinX, MinY, MinZ);
				Out.BoundsMax = FVector(MaxX, MaxY, MaxZ);
				return true;
			}
			return false;
		}

		class FWorldDestructiveCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FWorldDestructiveCommandHandler(
				FString InCommandId,
				TSharedRef<IUeAgentWorldDestructiveOwner> InOwner)
				: CommandId(MoveTemp(InCommandId))
				, Owner(MoveTemp(InOwner))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentWorldDestructiveRequest DomainRequest;
				if (!TryBuildRequest(Request, CallerContext, DomainRequest))
				{
					FUeAgentWorldDestructiveResult Invalid;
					Invalid.CommandId = CommandId;
					Invalid.ErrorCode = TEXT("world_destructive_params_invalid");
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorJson(Invalid));
				}
				const FUeAgentWorldDestructiveResult Result =
					Owner->Execute(DomainRequest, Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorJson(Result));
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentWorldDestructiveOwner> Owner;
		};
	}

	bool TryCreateDestructiveImportWorldCommandHandlers(
		TSharedRef<IUeAgentWorldDestructiveOwner> Owner,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const TCHAR* CommandId : {
			TEXT("world.data_layer.delete"),
			TEXT("world.level_content.delete_scope"),
			TEXT("world.pcg.cleanup"),
			TEXT("world.level_streaming.move_actors"),
			TEXT("world.level_streaming.remove"),
			TEXT("modeling.actor.mesh.replace"),
			TEXT("modeling.asset.material_slot.remove"),
			TEXT("modeling.voxel.bounds.remove")})
		{
			OutHandlers.Add(MakeShared<FWorldDestructiveCommandHandler>(
				CommandId, Owner));
		}
		return true;
	}
}
