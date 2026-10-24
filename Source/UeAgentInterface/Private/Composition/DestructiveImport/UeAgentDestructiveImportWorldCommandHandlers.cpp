#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/UnrealMathUtility.h"

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
			UE::UAI::Contract::FDiagnostic Error;
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

		UE::UAI::Contract::FDiagnostic ErrorDiagnostic(const FUeAgentWorldDestructiveResult& Result)
        {
            auto Diagnostic = Result.Diagnostic;
            if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Result.TargetRef;
            Diagnostic.TerminalStatus = Result.TerminalState;
            return Diagnostic;
        }

		bool ParameterFailure(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			const FString& Expected, FDiagnostic& OutError)
		{
			OutError = FDiagnostic::Failure(TEXT("world_destructive_params_invalid"),
				FString::Printf(TEXT("Parameter '%s' must be %s."), Field, *Expected), TEXT("command_input"));
			OutError.SourceKind = TEXT("command_input");
			OutError.PropertyPath = TEXT("/") + FString(Field);
			OutError.Source.JsonPath = TEXT("/params/") + FString(Field);
			const TSharedPtr<FJsonValue> Actual = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
			OutError.ActualValuePresent = Actual.IsValid();
			if (!Actual.IsValid()) OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("<missing>"));
			else if (Actual->Type == EJson::Object || Actual->Type == EJson::Array)
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(Actual->Type == EJson::Object ? TEXT("object") : TEXT("array"));
				OutError.ActualEvidence = Actual;
			}
			else OutError.ActualValue = Actual;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		bool TryString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& Out, FDiagnostic& OutError)
		{
			return (Params.IsValid() && Params->TryGetStringField(Field, Out) && !Out.TrimStartAndEnd().IsEmpty())
				|| ParameterFailure(Params, Field, TEXT("a non-empty string"), OutError);
		}

		bool TryNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, double& Out, FDiagnostic& OutError)
		{
			return (Params.IsValid() && Params->TryGetNumberField(Field, Out))
				|| ParameterFailure(Params, Field, TEXT("a number"), OutError);
		}

		bool TryBuildRequest(
			const FUeAgentCommandRequest& CommandRequest,
			const FUeAgentCallerContext& CallerContext,
			FUeAgentWorldDestructiveRequest& Out, FDiagnostic& OutError)
		{
			Out.RequestId = CommandRequest.GetRequestId();
			Out.CommandId = CommandRequest.GetCommandId();
			Out.CallerId = CallerContext.GetCallerId();
			const TSharedPtr<FJsonObject> Params = CommandRequest.GetParams();
			if (Out.CommandId == TEXT("world.data_layer.delete"))
			{
				return TryString(Params, TEXT("layer"), Out.TargetRef, OutError);
			}
			if (Out.CommandId == TEXT("world.level_content.delete_scope"))
			{
				if (!TryString(Params, TEXT("folder_path"), Out.TargetRef, OutError))
				{
					return false;
				}
				if (Params->HasField(TEXT("managed_id"))
					&& !TryString(Params, TEXT("managed_id"), Out.ManagedId, OutError))
				{
					return false;
				}
				return true;
			}
			if (Out.CommandId == TEXT("world.pcg.cleanup"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef, OutError)
					&& TryString(
						Params, TEXT("component_name"), Out.ComponentName, OutError);
			}
			if (Out.CommandId == TEXT("world.level_streaming.move_actors"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef, OutError)
					&& TryString(
						Params, TEXT("level_package"), Out.OperandRef, OutError);
			}
			if (Out.CommandId == TEXT("world.level_streaming.remove"))
			{
				return TryString(
					Params, TEXT("level_package"), Out.TargetRef, OutError);
			}
			if (Out.CommandId == TEXT("modeling.actor.mesh.replace"))
			{
				return TryString(Params, TEXT("actor_ref"), Out.TargetRef, OutError)
					&& TryString(Params, TEXT("mesh_ref"), Out.OperandRef, OutError);
			}
			if (Out.CommandId == TEXT("modeling.asset.material_slot.remove"))
			{
				double Index = -1;
				if (!TryString(Params, TEXT("target_ref"), Out.TargetRef, OutError)
					|| !TryNumber(Params, TEXT("index"), Index, OutError))
				{
					return false;
				}
				if (!FMath::IsFinite(Index) || Index < MIN_int32 || Index > MAX_int32)
					return ParameterFailure(Params, TEXT("index"), TEXT("32-bit integer"), OutError);
				Out.Index = static_cast<int32>(Index);
				return Index == static_cast<double>(Out.Index)
					|| ParameterFailure(Params, TEXT("index"), TEXT("32-bit integer"), OutError);
			}
			if (Out.CommandId == TEXT("modeling.voxel.bounds.remove"))
			{
				double MinX = 0;
				double MinY = 0;
				double MinZ = 0;
				double MaxX = 0;
				double MaxY = 0;
				double MaxZ = 0;
				if (!TryString(Params, TEXT("actor_ref"), Out.TargetRef, OutError)
					|| !TryString(
						Params, TEXT("component_name"), Out.ComponentName, OutError)
					|| !TryString(Params, TEXT("space"), Out.Space, OutError)
					|| !TryNumber(Params, TEXT("min_x"), MinX, OutError)
					|| !TryNumber(Params, TEXT("min_y"), MinY, OutError)
					|| !TryNumber(Params, TEXT("min_z"), MinZ, OutError)
					|| !TryNumber(Params, TEXT("max_x"), MaxX, OutError)
					|| !TryNumber(Params, TEXT("max_y"), MaxY, OutError)
					|| !TryNumber(Params, TEXT("max_z"), MaxZ, OutError))
				{
					return false;
				}
				Out.BoundsMin = FVector(MinX, MinY, MinZ);
				Out.BoundsMax = FVector(MaxX, MaxY, MaxZ);
				return true;
			}
			OutError = FDiagnostic::Failure(TEXT("world_destructive_params_invalid"),
				TEXT("The command has no world destructive parameter contract."), TEXT("command_input"));
			OutError.SourceKind = TEXT("command_input");
			OutError.PropertyPath = OutError.Source.JsonPath = TEXT("/command_id");
			OutError.ActualValue = MakeShared<FJsonValueString>(Out.CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered world destructive command"));
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
				FUeAgentWorldDestructiveResult Invalid;
				Invalid.CommandId = CommandId;
				if (!TryBuildRequest(Request, CallerContext, DomainRequest, Invalid.Diagnostic))
				{
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorDiagnostic(Invalid));
				}
				const FUeAgentWorldDestructiveResult Result =
					Owner->Execute(DomainRequest, Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorDiagnostic(Result));
			}

		private:
			FString CommandId;
			TSharedRef<IUeAgentWorldDestructiveOwner> Owner;
		};
	}

	bool TryCreateDestructiveImportWorldCommandHandlers(
		TSharedRef<IUeAgentWorldDestructiveOwner> Owner,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
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
