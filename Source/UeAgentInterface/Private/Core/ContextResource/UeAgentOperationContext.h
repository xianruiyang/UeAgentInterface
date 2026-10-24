#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::ContextResource
{
	enum class EUeAgentRequestKind : uint8
	{
		Invalid,
		Query,
		Operation,
	};

	enum class EUeAgentExecutionWorldType : uint8
	{
		Invalid,
		Editor,
		Pie,
		SimulateInEditor,
		Game,
		Preview,
	};

	struct UEAGENTINTERFACE_API FUeAgentExecutionContextInput
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		Contract::EUeAgentStatePlane StatePlane =
			Contract::EUeAgentStatePlane::Invalid;
		EUeAgentExecutionWorldType WorldType =
			EUeAgentExecutionWorldType::Invalid;
		FString WorldIdentity;
		FString LevelIdentity;
		TArray<FString> DataLayerIdentities;
		FString WorldPartitionIdentity;
		TOptional<int32> PieInstanceId;
		FString NetMode;
		FString NetRole;
		FString AssetEditorIdentity;
		FString ViewportIdentity;
		TArray<FString> SelectionIdentities;
		FString ContextRevision;
		bool bResolvedFromCurrent = false;
	};

	using FUeAgentExecutionContextError = Contract::FDiagnostic;

	class FUeAgentExecutionContextRef;
	using FUeAgentExecutionContextRefPtr =
		TSharedPtr<
			const FUeAgentExecutionContextRef,
			ESPMode::ThreadSafe>;
	using FUeAgentExecutionContextRefOwner =
		TSharedRef<
			const FUeAgentExecutionContextRef,
			ESPMode::ThreadSafe>;
	using FUeAgentResourceTrackerOwner =
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>;

	class UEAGENTINTERFACE_API FUeAgentExecutionContextRef final
	{
	public:
		static bool TryCreate(
			const FUeAgentExecutionContextInput& Input,
			const Contract::FUeAgentDescriptorIdentity& DescriptorIdentity,
			EUeAgentRequestKind RequestKind,
			const Contract::FUeAgentExecutionContextPolicy& Policy,
			FUeAgentExecutionContextRefPtr& OutContext,
			FUeAgentExecutionContextError& OutError);

		const FString& GetProjectInstanceId() const;
		const FString& GetEditorInstanceId() const;
		Contract::EUeAgentStatePlane GetStatePlane() const;
		EUeAgentExecutionWorldType GetWorldType() const;
		const FString& GetWorldIdentity() const;
		const FString& GetLevelIdentity() const;
		const TArray<FString>& GetDataLayerIdentities() const;
		const FString& GetWorldPartitionIdentity() const;
		const TOptional<int32>& GetPieInstanceId() const;
		const FString& GetNetMode() const;
		const FString& GetNetRole() const;
		const FString& GetAssetEditorIdentity() const;
		const FString& GetViewportIdentity() const;
		const TArray<FString>& GetSelectionIdentities() const;
		const FString& GetContextRevision() const;
		bool WasResolvedFromCurrent() const;
		const Contract::FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const;
		EUeAgentRequestKind GetRequestKind() const;

		bool TryValidateRevision(
			const FString& ObservedRevision,
			FUeAgentExecutionContextError& OutError) const;

	private:
		FUeAgentExecutionContextRef(
			FUeAgentExecutionContextInput InInput,
			Contract::FUeAgentDescriptorIdentity InDescriptorIdentity,
			EUeAgentRequestKind InRequestKind);

		const FUeAgentExecutionContextInput Input;
		const Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		const EUeAgentRequestKind RequestKind;
	};

	class UEAGENTINTERFACE_API IUeAgentExecutionContextValidator
	{
	public:
		virtual ~IUeAgentExecutionContextValidator() = default;

		virtual bool TryValidate(
			const FUeAgentExecutionContextRef& Context,
			FUeAgentExecutionContextError& OutError)
			const = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentExactExecutionContextValidator final
		: public IUeAgentExecutionContextValidator
	{
	public:
		explicit FUeAgentExactExecutionContextValidator(
			FUeAgentExecutionContextRefOwner InExpected);

		bool TryValidate(
			const FUeAgentExecutionContextRef& Context,
			FUeAgentExecutionContextError& OutError)
			const override;

	private:
		FUeAgentExecutionContextRefOwner Expected;
	};

	using FUeAgentOperationContextError = Contract::FDiagnostic;

	class UEAGENTINTERFACE_API FUeAgentOperationContext final
	{
	public:
		static bool TryCreate(
			const Contract::FUeAgentQueryRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			FUeAgentExecutionContextRefOwner ExecutionContext,
			FUeAgentResourceTrackerOwner ResourceTracker,
			FUeAgentOperationDeadline Deadline,
			FUeAgentCancellationToken CancellationToken,
			TSharedPtr<FUeAgentOperationContext>& OutContext,
			FUeAgentOperationContextError& OutError);

		static bool TryCreate(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			FUeAgentExecutionContextRefOwner ExecutionContext,
			FUeAgentResourceTrackerOwner ResourceTracker,
			FUeAgentOperationDeadline Deadline,
			FUeAgentCancellationToken CancellationToken,
			TSharedPtr<FUeAgentOperationContext>& OutContext,
			FUeAgentOperationContextError& OutError);

		template <typename TPayload>
		static bool TryCreate(
			const Contract::TUeAgentOperationRequest<TPayload>& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			FUeAgentExecutionContextRefOwner ExecutionContext,
			FUeAgentResourceTrackerOwner ResourceTracker,
			FUeAgentOperationDeadline Deadline,
			FUeAgentCancellationToken CancellationToken,
			TSharedPtr<FUeAgentOperationContext>& OutContext,
			FUeAgentOperationContextError& OutError)
		{
			if (!Request.IsValid())
			{
				OutContext.Reset();
				OutError = Contract::FDiagnostic::Failure(TEXT("operation_context_request_invalid"), TEXT("The operation context requires a valid request."), TEXT("context"));
				OutError.Schema = TEXT("uai.runtime.diagnostic/1");
				return false;
			}
			return TryCreateInternal(
				Request.GetDescriptorIdentity(),
				EUeAgentRequestKind::Operation,
				Request.GetRequestId(),
				Request.GetIdempotencyKey(),
				CallerContext,
				MoveTemp(ExecutionContext),
				MoveTemp(ResourceTracker),
				MoveTemp(Deadline),
				MoveTemp(CancellationToken),
				OutContext,
				OutError);
		}

		const Contract::FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const;
		EUeAgentRequestKind GetRequestKind() const;
		const FString& GetRequestId() const;
		const FString& GetIdempotencyKey() const;
		const Contract::FUeAgentCallerContext&
			GetCallerContext() const;
		const FUeAgentExecutionContextRef&
			GetExecutionContext() const;
		const FUeAgentExecutionContextRefOwner&
			GetExecutionContextOwner() const;
		FUeAgentResourceTracker& GetResourceTracker() const;
		const FUeAgentResourceTrackerOwner&
			GetResourceTrackerOwner() const;
		const FUeAgentOperationDeadline& GetDeadline() const;
		const FUeAgentCancellationToken&
			GetCancellationToken() const;
		FUeAgentResourceScope Scope(FString Path) const;

	private:
		static bool TryCreateInternal(
			const Contract::FUeAgentDescriptorIdentity&
				DescriptorIdentity,
			EUeAgentRequestKind RequestKind,
			const FString& RequestId,
			const FString& IdempotencyKey,
			const Contract::FUeAgentCallerContext& CallerContext,
			FUeAgentExecutionContextRefOwner ExecutionContext,
			FUeAgentResourceTrackerOwner ResourceTracker,
			FUeAgentOperationDeadline Deadline,
			FUeAgentCancellationToken CancellationToken,
			TSharedPtr<FUeAgentOperationContext>& OutContext,
			FUeAgentOperationContextError& OutError);

		FUeAgentOperationContext(
			Contract::FUeAgentDescriptorIdentity InDescriptorIdentity,
			EUeAgentRequestKind InRequestKind,
			FString InRequestId,
			FString InIdempotencyKey,
			Contract::FUeAgentCallerContext InCallerContext,
			FUeAgentExecutionContextRefOwner InExecutionContext,
			FUeAgentResourceTrackerOwner InResourceTracker,
			FUeAgentOperationDeadline InDeadline,
			FUeAgentCancellationToken InCancellationToken);

		const Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		const EUeAgentRequestKind RequestKind;
		const FString RequestId;
		const FString IdempotencyKey;
		const Contract::FUeAgentCallerContext CallerContext;
		const FUeAgentExecutionContextRefOwner
			ExecutionContext;
		const FUeAgentResourceTrackerOwner ResourceTracker;
		const FUeAgentOperationDeadline Deadline;
		const FUeAgentCancellationToken CancellationToken;
	};
}
