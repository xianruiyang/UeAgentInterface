#include "Composition/DestructiveImport/UeAgentDestructiveImportCommandHandlers.h"

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
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error) ? Text : TEXT("{}");
		}

		FString PayloadJson(const FUeAgentAssetDeleteResult& Result)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), Result.TargetRef);
			Root->SetStringField(TEXT("package_name"), Result.PackageName);
			Root->SetStringField(TEXT("terminal_state"), Result.TerminalState);
			Root->SetBoolField(TEXT("deleted"), Result.bDeleted);
			Root->SetStringField(TEXT("journal_id"), Result.JournalId);
			Root->SetStringField(TEXT("verification_id"), Result.VerificationId);
			const TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
			Readback->SetBoolField(TEXT("file_absent"), Result.bFileAbsent);
			Readback->SetBoolField(TEXT("package_absent"), Result.bPackageAbsent);
			Readback->SetBoolField(TEXT("registry_absent"), Result.bRegistryAbsent);
			Readback->SetBoolField(TEXT("object_absent"), Result.bObjectAbsent);
			Root->SetObjectField(TEXT("readback"), Readback);
			return JsonText(Root);
		}

		FString ErrorJson(const FUeAgentAssetDeleteResult& Result)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Result.ErrorCode);
			Error->SetStringField(TEXT("detail"), Result.ErrorDetail);
			Error->SetStringField(TEXT("target_ref"), Result.TargetRef);
			return JsonText(Error);
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

		FString ErrorJson(const FUeAgentAssetOperationResult& Result)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Result.ErrorCode);
			Error->SetStringField(TEXT("detail"), Result.ErrorDetail);
			Error->SetStringField(TEXT("target_ref"), Result.TargetRef);
			return JsonText(Error);
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
				FString TargetRef;
				if (!Request.GetParams().IsValid()
					|| !Request.GetParams()->TryGetStringField(
						TEXT("target_ref"), TargetRef))
				{
					FUeAgentAssetDeleteResult Invalid;
					Invalid.ErrorCode = TEXT("asset_delete_target_required");
					Invalid.ErrorDetail = TEXT("target_ref must be a string");
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorJson(Invalid));
				}
				const FUeAgentAssetDeleteResult Result = Owner->Delete(
					FUeAgentAssetDeleteRequest{
						Request.GetRequestId(), TargetRef, CallerContext.GetCallerId()},
					Cancellation);
				const FString Payload = PayloadJson(Result);
				return Result.IsSuccess()
					? FUeAgentCommandResult::Success(Request, Payload)
					: FUeAgentCommandResult::Failure(
						Request, Payload, ErrorJson(Result));
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
					Invalid.ErrorCode = TEXT("asset_operation_params_invalid");
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson(Invalid), ErrorJson(Invalid));
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
						Request, Payload, ErrorJson(Result));
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
		FString& OutError)
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
