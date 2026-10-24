#include "Composition/DestructiveImport/UeAgentDestructiveImportCommandHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace DomainAdapters::AssetDestructiveImport;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			UE::UAI::Contract::FDiagnostic Error;
			return Json::TryCanonicalize(Object, Text, Error) ? Text : TEXT("{}");
		}

		FString PayloadJson(const FUeAgentAssetDeleteResult& Result)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("terminal_state"), Result.TerminalState);
			if (!Result.Diagnostic.IsEmpty())
			{
				Root->SetStringField(TEXT("failure_reason"), Result.Diagnostic.PublicMessage);
			}
			TArray<TSharedPtr<FJsonValue>> RequestedTargets;
			RequestedTargets.Reserve(Result.RequestedTargets.Num());
			for (const FString& TargetRef : Result.RequestedTargets)
			{
				RequestedTargets.Add(MakeShared<FJsonValueString>(TargetRef));
			}
			Root->SetArrayField(TEXT("requested_targets"), MoveTemp(RequestedTargets));
			Root->SetBoolField(TEXT("recursive"), Result.bRecursive);

			int32 DeletedCount = 0;
			int32 BlockedCount = 0;
			int32 RemainingCount = 0;
			TArray<TSharedPtr<FJsonValue>> Targets;
			Targets.Reserve(Result.Targets.Num());
			for (const FUeAgentAssetDeleteTargetResult& Target : Result.Targets)
			{
				DeletedCount += Target.bDeleted ? 1 : 0;
				BlockedCount += Target.Status == TEXT("blocked") ? 1 : 0;
				RemainingCount += Target.bRemaining ? 1 : 0;
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("target_ref"), Target.TargetRef);
				Item->SetStringField(TEXT("package_name"), Target.PackageName);
				Item->SetStringField(TEXT("object_path"), Target.ObjectPath);
				Item->SetStringField(TEXT("status"), Target.Status);
				Item->SetStringField(TEXT("terminal_state"), Target.TerminalState);
				Item->SetBoolField(TEXT("deleted"), Target.bDeleted);
				Item->SetBoolField(TEXT("remaining"), Target.bRemaining);
                if (!Target.Diagnostic.IsEmpty())
                {
                    auto Diagnostic = Target.Diagnostic;
                    if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TargetRef;
                    Diagnostic.TerminalStatus = Target.TerminalState;
                    Item->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
                }
				if (!Target.JournalId.IsEmpty())
				{
					Item->SetStringField(TEXT("journal_id"), Target.JournalId);
				}
				if (!Target.VerificationId.IsEmpty())
				{
					Item->SetStringField(TEXT("verification_id"), Target.VerificationId);
				}
				const TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
				Readback->SetBoolField(TEXT("file_absent"), Target.bFileAbsent);
				Readback->SetBoolField(TEXT("package_absent"), Target.bPackageAbsent);
				Readback->SetBoolField(TEXT("package_empty"), Target.bPackageEmpty);
				Readback->SetBoolField(TEXT("registry_absent"), Target.bRegistryAbsent);
			Readback->SetBoolField(TEXT("object_absent"), Target.bObjectAbsent);
			Readback->SetBoolField(TEXT("object_retired"), Target.bObjectRetired);
				Item->SetObjectField(TEXT("readback"), Readback);
				Targets.Add(MakeShared<FJsonValueObject>(Item));
			}
			Root->SetNumberField(TEXT("deleted_count"), DeletedCount);
			Root->SetNumberField(TEXT("blocked_count"), BlockedCount);
			Root->SetNumberField(TEXT("remaining_count"), RemainingCount);
			Root->SetArrayField(TEXT("targets"), MoveTemp(Targets));
			return JsonText(Root);
		}

		UE::UAI::Contract::FDiagnostic ErrorDiagnostic(const FString& Code, const FString& Reason)
        {
            return UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_operation"));
        }

		UE::UAI::Contract::FDiagnostic ErrorDiagnostic(const FUeAgentAssetDeleteResult& Result)
        {
            auto Diagnostic = Result.Diagnostic;
            for (const auto& Target : Result.Targets)
            {
                auto Issue = Target.Diagnostic;
                if (Issue.IsEmpty()) continue;
                if (Issue.TargetPath.IsEmpty()) Issue.TargetPath = Target.TargetRef;
                Issue.TerminalStatus = Target.TerminalState;
                Diagnostic.Append(Issue);
            }
            if (Diagnostic.IsEmpty()) Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(
                TEXT("asset_delete_failed"), TEXT("Asset deletion did not complete."), TEXT("asset_delete"));
            Diagnostic.TerminalStatus = Result.TerminalState;
            return Diagnostic;
        }

		FString PayloadJson(const FUeAgentAssetOperationResult& Result)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("command_id"), Result.CommandId);
			Root->SetStringField(TEXT("target_ref"), Result.TargetRef);
			Root->SetStringField(TEXT("result_ref"), Result.ResultRef);
			Root->SetStringField(TEXT("terminal_state"), Result.TerminalState);
			Root->SetBoolField(TEXT("changed"), Result.bChanged);
			Root->SetBoolField(TEXT("readback_verified"), Result.bReadbackVerified);
			Root->SetStringField(TEXT("journal_id"), Result.JournalId);
			Root->SetStringField(TEXT("verification_id"), Result.VerificationId);
			return JsonText(Root);
		}

		UE::UAI::Contract::FDiagnostic ErrorDiagnostic(const FUeAgentAssetOperationResult& Result)
        {
            auto Diagnostic = Result.Diagnostic;
            if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Result.TargetRef;
            Diagnostic.TerminalStatus = Result.TerminalState;
            return Diagnostic;
        }

		class FAssetDeleteCommandHandler final : public IUeAgentCommandHandler
		{
		public:
			explicit FAssetDeleteCommandHandler(
				TSharedRef<IUeAgentAssetDeleteOwner> InOwner)
				: Owner(MoveTemp(InOwner))
			{
			}

			const FString& GetCommandId() const override
			{
				static const FString CommandId(TEXT("asset.delete"));
				return CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				TArray<FString> TargetRefs;
				const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
				if (!Request.GetParams().IsValid()
					|| !Request.GetParams()->TryGetArrayField(TEXT("targets"), Targets)
					|| !Targets
					|| Targets->IsEmpty())
				{
					FUeAgentAssetDeleteResult Invalid;
					return FUeAgentCommandResult::Failure(
						Request,
						PayloadJson(Invalid),
						ErrorDiagnostic(
							TEXT("asset_delete_targets_required"),
							TEXT("targets must be a non-empty string array")));
				}
				TargetRefs.Reserve(Targets->Num());
				for (const TSharedPtr<FJsonValue>& Value : *Targets)
				{
					FString TargetRef;
					if (!Value.IsValid() || !Value->TryGetString(TargetRef))
					{
						FUeAgentAssetDeleteResult Invalid;
						return FUeAgentCommandResult::Failure(
							Request,
							PayloadJson(Invalid),
							ErrorDiagnostic(
								TEXT("asset_delete_targets_required"),
								TEXT("targets must be a non-empty string array")));
					}
					TargetRef.TrimStartAndEndInline();
					if (TargetRef.IsEmpty())
					{
						FUeAgentAssetDeleteResult Invalid;
						return FUeAgentCommandResult::Failure(
							Request,
							PayloadJson(Invalid),
							ErrorDiagnostic(
								TEXT("asset_delete_targets_required"),
								TEXT("targets must contain only non-empty strings")));
					}
					TargetRefs.Add(MoveTemp(TargetRef));
				}
				bool bRecursive = false;
				if (Request.GetParams()->HasField(TEXT("recursive"))
					&& !Request.GetParams()->TryGetBoolField(
						TEXT("recursive"), bRecursive))
				{
					FUeAgentAssetDeleteResult Invalid;
					Invalid.RequestedTargets = TargetRefs;
					return FUeAgentCommandResult::Failure(
						Request,
						PayloadJson(Invalid),
						ErrorDiagnostic(
							TEXT("asset_delete_recursive_invalid"),
							TEXT("recursive must be a boolean")));
				}
				const FUeAgentAssetDeleteResult Result = Owner->Delete(
					FUeAgentAssetDeleteRequest{
						Request.GetRequestId(), TargetRefs, bRecursive,
						CallerContext.GetCallerId()},
					Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorDiagnostic(Result));
			}

		private:
			TSharedRef<IUeAgentAssetDeleteOwner> Owner;
		};

		class FAssetOperationCommandHandler final : public IUeAgentCommandHandler
		{
		public:
			FAssetOperationCommandHandler(
				FString InCommandId,
				FString InTargetField,
				FString InOperandField,
				TSharedRef<IUeAgentAssetOperationsOwner> InOwner)
				: CommandId(MoveTemp(InCommandId))
				, TargetField(MoveTemp(InTargetField))
				, OperandField(MoveTemp(InOperandField))
				, Owner(MoveTemp(InOwner))
			{
			}

			const FString& GetCommandId() const override { return CommandId; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FString Target;
				FString Operand;
				if (!Request.GetParams().IsValid()
					|| !Request.GetParams()->TryGetStringField(TargetField, Target)
					|| (!OperandField.IsEmpty()
						&& !Request.GetParams()->TryGetStringField(
							OperandField, Operand)))
				{
					FUeAgentAssetOperationResult Invalid;
					Invalid.CommandId = CommandId;
					Invalid.TargetRef = Target;
					Invalid.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_operation_params_invalid"), TEXT("Required asset operation parameters are missing or invalid."));
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorDiagnostic(Invalid));
				}
				const FUeAgentAssetOperationResult Result = Owner->Execute(
					FUeAgentAssetOperationRequest{
						Request.GetRequestId(), CommandId, Target, Operand,
						CallerContext.GetCallerId()},
					Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorDiagnostic(Result));
			}

		private:
			FString CommandId;
			FString TargetField;
			FString OperandField;
			TSharedRef<IUeAgentAssetOperationsOwner> Owner;
		};
	}

	bool TryCreateDestructiveImportCommandHandlers(
		TSharedRef<IUeAgentAssetDeleteOwner> AssetDeleteOwner,
		TSharedRef<IUeAgentAssetOperationsOwner> AssetOperationsOwner,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FAssetDeleteCommandHandler>(
			MoveTemp(AssetDeleteOwner)));
		auto Add = [&OutHandlers, &AssetOperationsOwner](
			const TCHAR* CommandId,
			const TCHAR* TargetField,
			const TCHAR* OperandField)
		{
			OutHandlers.Add(MakeShared<FAssetOperationCommandHandler>(
				CommandId, TargetField, OperandField, AssetOperationsOwner));
		};
		Add(TEXT("asset.move"), TEXT("target_ref"), TEXT("destination_ref"));
		Add(TEXT("asset.fixup_redirectors"), TEXT("root_ref"), TEXT(""));
		Add(TEXT("asset.reimport_texture"), TEXT("target_ref"), TEXT("source_ref"));
		Add(TEXT("asset.reimport_sound_wave"), TEXT("target_ref"), TEXT("source_ref"));
		Add(TEXT("asset.reimport_static_mesh"), TEXT("target_ref"), TEXT("source_ref"));
		Add(TEXT("asset.reimport_sparse_volume_texture"), TEXT("target_ref"), TEXT("source_ref"));
		Add(TEXT("skeletal_mesh.remove_morph_target"), TEXT("target_ref"), TEXT("name"));
		Add(TEXT("skeletal_mesh.remove_skin_weight_profile"), TEXT("target_ref"), TEXT("name"));
		return true;
	}
}
