#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"

#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::GenericEffect
{
	FString GetUePropertySourceTypeId(const FProperty& Property)
	{
		return FString::Printf(
			TEXT("UE.%s:%s"),
			*Property.GetClass()->GetName(),
			*Property.GetCPPType());
	}

	const FString& GetUeStringPropertySourceTypeId()
	{
		static const FString Value =
			TEXT("UE.StrProperty:FString");
		return Value;
	}

	namespace
	{
		using namespace ContextResource;
		using namespace Discovery;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;

		FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		TArray<FString> NormalizeStringSet(TArray<FString> Values)
		{
			TArray<FString> Normalized;
			Normalized.Reserve(Values.Num());
			for (FString& Value : Values)
			{
				Value = NormalizeText(MoveTemp(Value));
				if (!Value.IsEmpty())
				{
					Normalized.Add(MoveTemp(Value));
				}
			}
			Normalized.Sort();
			for (int32 Index = Normalized.Num() - 1; Index > 0; --Index)
			{
				if (Normalized[Index] == Normalized[Index - 1])
				{
					Normalized.RemoveAt(Index);
				}
			}
			return Normalized;
		}

		bool HasNonEmptyStrings(const TArray<FString>& Values)
		{
			for (const FString& Value : Values)
			{
				if (NormalizeText(Value).IsEmpty())
				{
					return false;
				}
			}
			return true;
		}

		bool ContainsExact(
			const TArray<FString>& Values,
			const FString& Expected)
		{
			return Values.ContainsByPredicate(
				[&Expected](const FString& Value)
				{
					return Value == Expected;
				});
		}

		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Update:
				return EUeAgentCandidateOperation::Update;
			case EChangeIntent::Replace:
				return EUeAgentCandidateOperation::Replace;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			default:
				return EUeAgentCandidateOperation::Read;
			}
		}

		bool MatchesType(
			const FUeAgentCanonicalValuePtr& Value,
			const FUeAgentCanonicalType& Type)
		{
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->GetKind() == Type.GetValueKind())
			{
				return true;
			}
			return Type.IsNullable()
				&& Value->GetKind() == EUeAgentValueKind::Null;
		}

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		bool CheckContext(
			const FExecutionMutationRequest& Request,
			const FString& Stage,
			FExecutionMutationError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Request.GetCancellation().TryCheckpoint(Cancelled))
			{
				return Fail(
					OutError,
					Cancelled.Code,
					Cancelled.Reason);
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Request.GetDeadline().TryCheckpoint(Stage, Expired))
			{
				return Fail(
					OutError,
					Expired.Code,
					Expired.Stage);
			}
			return true;
		}

		bool ChargeWork(
			FUeAgentResourceTracker& Resources,
			const int64 Items,
			const FString& Path,
			FExecutionMutationError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(
					{
						{EUeAgentResourceDimension::Items, Items},
						{EUeAgentResourceDimension::WorkUnits, Items},
					},
					Path,
					BudgetError))
			{
				return true;
			}
			return Fail(
				OutError,
				BudgetError.Code,
				FString::Printf(
					TEXT("%s limit=%lld observed=%lld"),
					*BudgetError.Path,
					BudgetError.Limit,
					BudgetError.Observed));
		}

		const FUeAgentGenericEffectBinding* FindBinding(
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			const FString& IdentityKey)
		{
			return Bindings.FindByPredicate(
				[&IdentityKey](
					const FUeAgentGenericEffectBinding& Binding)
				{
					return Binding.GetCandidateBinding()
						.GetSummary().GetIdentity()
						.GetCanonicalKey() == IdentityKey;
				});
		}

		bool ValidateExecutionBoundary(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid() || !Lease.IsValid())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_request_invalid"));
			}
			if (Lease.GetOwnerId() != Request.GetOwnerLeaseId())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_lease_owner_mismatch"),
					Lease.GetOwnerId());
			}
			if (!CheckContext(
					Request,
					TEXT("domain_adapter.generic_effect.before_validation"),
					OutError))
			{
				return false;
			}

			const TArray<FPlannedChange>& Changes =
				Request.GetPlan()->GetChangeSet().GetChanges();
			if (Changes.IsEmpty() || Bindings.Num() != Changes.Num())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_binding_missing"));
			}
			if (!ChargeWork(
					Resources,
					Changes.Num(),
					TEXT("domain_adapter.generic_effect.bindings"),
					OutError))
			{
				return false;
			}

			TSet<FString> SeenBindings;
			for (const FUeAgentGenericEffectBinding& Binding : Bindings)
			{
				if (!Binding.IsValid())
				{
					return Fail(
						OutError,
						TEXT("generic_effect_binding_invalid"));
				}
				const FString& Key = Binding.GetCandidateBinding()
					.GetSummary().GetIdentity().GetCanonicalKey();
				if (SeenBindings.Contains(Key))
				{
					return Fail(
						OutError,
						TEXT("generic_effect_binding_duplicate"),
						Key);
				}
				SeenBindings.Add(Key);
			}

			for (const FPlannedChange& Change : Changes)
			{
				const FString& Key =
					Change.GetIdentity().GetCanonicalKey();
				const FUeAgentGenericEffectBinding* Binding =
					FindBinding(Bindings, Key);
				if (!Binding)
				{
					return Fail(
						OutError,
						TEXT("generic_effect_binding_missing"),
						Key);
				}
				if (!Binding->GetCandidateBinding().Supports(
						CandidateOperation(Change.GetIntent())))
				{
					return Fail(
						OutError,
						TEXT("generic_effect_operation_unsupported"),
						Key);
				}
				if (!MatchesType(
						Change.GetBefore(),
						Binding->GetCanonicalType())
					|| !MatchesType(
						Change.GetAfter(),
						Binding->GetCanonicalType()))
				{
					return Fail(
						OutError,
						TEXT("generic_effect_type_binding_mismatch"),
						Key);
				}
				if (!ContainsExact(Lease.GetLockKeys(), Key))
				{
					return Fail(
						OutError,
						TEXT("generic_effect_lock_missing"),
						Key);
				}
			}
			return CheckContext(
				Request,
				TEXT("domain_adapter.generic_effect.before_ue_hook"),
				OutError);
		}

		bool SameFacts(
			const FMutationFacts& Facts,
			const FUeAgentGenericEffectApplyResult& Applied)
		{
			return Facts.GetSubjectGeneration()
					== Applied.GetSubjectGeneration()
				&& Facts.GetMutatedIdentityKeys()
					== Applied.GetMutatedIdentityKeys()
				&& Facts.GetRequiredBarrierSignals()
					== Applied.GetRequiredBarrierSignals()
				&& Facts.IsRollbackSupported()
					== Applied.IsRollbackSupported();
		}

		class FStoredPropertyValue final
		{
		public:
			explicit FStoredPropertyValue(FProperty& InProperty)
				: Property(&InProperty)
			{
				Data = FMemory::Malloc(
					Property->GetSize(),
					Property->GetMinAlignment());
				Property->InitializeValue(Data);
			}

			~FStoredPropertyValue()
			{
				if (Data)
				{
					Property->DestroyValue(Data);
					FMemory::Free(Data);
				}
			}

			void Capture(const void* Source)
			{
				Property->CopyCompleteValue(Data, Source);
			}

			void Restore(void* Destination) const
			{
				Property->CopyCompleteValue(Destination, Data);
			}

		private:
			FProperty* Property = nullptr;
			void* Data = nullptr;
		};

		struct FPropertyMutation
		{
			TWeakObjectPtr<UObject> Object;
			FProperty* Property = nullptr;
			TSharedPtr<FStoredPropertyValue> OriginalValue;
			bool bPackageWasDirty = false;
		};

		struct FOperationRecord
		{
			FString OperationId;
			FString RollbackToken;
			int64 Generation = -1;
			TArray<FPropertyMutation> Mutations;
			bool bRollbackStarted = false;
			bool bRolledBack = false;
		};

		struct FSettleRecord
		{
			int64 Generation = -1;
			TArray<TWeakObjectPtr<UObject>> Objects;
			TArray<FString> ObservedSignals;
			FString Revision;
		};

		UObject* ResolveBoundObject(
			const FUeAgentGenericEffectBinding& Binding)
		{
			const FString& OwnerId = Binding.GetCandidateBinding()
				.GetSummary().GetIdentity().GetOwnerId();
			return StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*OwnerId);
		}

		bool TryApplyStringProperty(
			const FPlannedChange& Change,
			const FUeAgentGenericEffectBinding& Binding,
			FOperationRecord& Record,
			FExecutionMutationError& OutError)
		{
			if (Change.GetIntent() != EChangeIntent::Update
				&& Change.GetIntent() != EChangeIntent::Replace)
			{
				return Fail(
					OutError,
					TEXT(
						"generic_effect_reflection_intent_unsupported"),
					LexToString(Change.GetIntent()));
			}
			UObject* Object = ResolveBoundObject(Binding);
			if (!::IsValid(Object))
			{
				return Fail(
					OutError,
					TEXT("generic_effect_object_not_found"),
					Change.GetIdentity().GetOwnerId());
			}
			if (!IsInGameThread())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_requires_game_thread"));
			}

			FProperty* Property = FindFProperty<FProperty>(
				Object->GetClass(),
				*Change.GetIdentity().GetSelector());
			if (!Property)
			{
				return Fail(
					OutError,
					TEXT("generic_effect_property_not_found"),
					Change.GetIdentity().GetSelector());
			}
			if (!Property->HasAnyPropertyFlags(CPF_Edit)
				|| Property->HasAnyPropertyFlags(CPF_EditConst))
			{
				return Fail(
					OutError,
					TEXT("generic_effect_property_not_editable"),
					Property->GetName());
			}
			if (GetUePropertySourceTypeId(*Property)
				!= Binding.GetSourceTypeId())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_source_type_mismatch"),
					GetUePropertySourceTypeId(*Property));
			}
			FStrProperty* StringProperty =
				CastField<FStrProperty>(Property);
			if (!StringProperty
				|| Binding.GetCanonicalType().GetValueKind()
					!= EUeAgentValueKind::String
				|| !Change.GetBefore().IsValid()
				|| !Change.GetAfter().IsValid())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_reflection_type_unsupported"),
					Property->GetName());
			}

			const FString Current =
				StringProperty->GetPropertyValue_InContainer(Object);
			if (Current != Change.GetBefore()->GetText())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_before_mismatch"),
					Change.GetIdentity().GetCanonicalKey());
			}

			void* ValuePtr =
				Property->ContainerPtrToValuePtr<void>(Object);
			if (!ValuePtr)
			{
				return Fail(
					OutError,
					TEXT("generic_effect_property_storage_unavailable"),
					Property->GetName());
			}
			TSharedPtr<FStoredPropertyValue> Original =
				MakeShared<FStoredPropertyValue>(*Property);
			Original->Capture(ValuePtr);
			UPackage* Package = Object->GetOutermost();
			const bool bPackageWasDirty =
				Package && Package->IsDirty();

			Object->Modify();
			Object->PreEditChange(Property);
			StringProperty->SetPropertyValue_InContainer(
				Object,
				Change.GetAfter()->GetText());
			FPropertyChangedEvent Event(
				Property,
				EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
			if (Package
				&& Package != GetTransientPackage())
			{
				Object->MarkPackageDirty();
			}
			Record.Mutations.Add({
				Object,
				Property,
				MoveTemp(Original),
				bPackageWasDirty,
			});
			return true;
		}
	}

	FUeAgentGenericEffectBinding::FUeAgentGenericEffectBinding(
		FUeAgentCandidateBinding InCandidateBinding,
		FString InSourceTypeId,
		FUeAgentCanonicalType InCanonicalType,
		FString InTypeBindingEvidence)
		: CandidateBinding(MoveTemp(InCandidateBinding))
		, SourceTypeId(NormalizeText(MoveTemp(InSourceTypeId)))
		, CanonicalType(MoveTemp(InCanonicalType))
		, TypeBindingEvidence(
			NormalizeText(MoveTemp(InTypeBindingEvidence)))
	{
		bValid = CandidateBinding.IsValid()
			&& !SourceTypeId.IsEmpty()
			&& CanonicalType.IsValid()
			&& !TypeBindingEvidence.IsEmpty();
	}

	const FUeAgentCandidateBinding&
	FUeAgentGenericEffectBinding::GetCandidateBinding() const
	{
		return CandidateBinding;
	}

	const FString& FUeAgentGenericEffectBinding::GetSourceTypeId() const
	{
		return SourceTypeId;
	}

	const FUeAgentCanonicalType&
	FUeAgentGenericEffectBinding::GetCanonicalType() const
	{
		return CanonicalType;
	}

	const FString&
	FUeAgentGenericEffectBinding::GetTypeBindingEvidence() const
	{
		return TypeBindingEvidence;
	}

	bool FUeAgentGenericEffectBinding::IsValid() const
	{
		return bValid;
	}

	FUeAgentGenericEffectApplyResult::FUeAgentGenericEffectApplyResult(
		const int64 InSubjectGeneration,
		TArray<FString> InMutatedIdentityKeys,
		TArray<FString> InRequiredBarrierSignals,
		const bool bInRollbackSupported,
		FString InRollbackToken,
		const bool bInMutationStarted,
		TArray<AssetDurable::FAssetDurableTarget>
			InDurableTargets)
		: SubjectGeneration(InSubjectGeneration)
		, MutatedIdentityKeys(
			NormalizeStringSet(MoveTemp(InMutatedIdentityKeys)))
		, RequiredBarrierSignals(
			NormalizeStringSet(MoveTemp(InRequiredBarrierSignals)))
		, bRollbackSupported(bInRollbackSupported)
		, RollbackToken(NormalizeText(MoveTemp(InRollbackToken)))
		, DurableTargets(MoveTemp(InDurableTargets))
		, bMutationStarted(bInMutationStarted)
	{
		bValid = bMutationStarted
			&& SubjectGeneration >= 0
			&& !MutatedIdentityKeys.IsEmpty()
			&& HasNonEmptyStrings(MutatedIdentityKeys)
			&& HasNonEmptyStrings(RequiredBarrierSignals)
			&& (!bRollbackSupported || !RollbackToken.IsEmpty());
		for (const AssetDurable::FAssetDurableTarget& Target :
			DurableTargets)
		{
			bValid = bValid && Target.IsValid();
		}
	}

	int64 FUeAgentGenericEffectApplyResult::GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	const TArray<FString>&
	FUeAgentGenericEffectApplyResult::GetMutatedIdentityKeys() const
	{
		return MutatedIdentityKeys;
	}

	const TArray<FString>&
	FUeAgentGenericEffectApplyResult::GetRequiredBarrierSignals() const
	{
		return RequiredBarrierSignals;
	}

	bool FUeAgentGenericEffectApplyResult::IsRollbackSupported() const
	{
		return bRollbackSupported;
	}

	const FString&
	FUeAgentGenericEffectApplyResult::GetRollbackToken() const
	{
		return RollbackToken;
	}

	const TArray<AssetDurable::FAssetDurableTarget>&
	FUeAgentGenericEffectApplyResult::GetDurableTargets() const
	{
		return DurableTargets;
	}

	bool FUeAgentGenericEffectApplyResult::DidMutationStart() const
	{
		return bMutationStarted;
	}

	bool FUeAgentGenericEffectApplyResult::IsValid() const
	{
		return bValid;
	}

	FUeAgentGenericEffectMutationHook::
	FUeAgentGenericEffectMutationHook(
		TArray<FUeAgentGenericEffectBinding> InBindings,
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: Bindings(MoveTemp(InBindings))
		, UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentGenericEffectMutationHook::Execute(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		FUeAgentResourceTracker& Resources,
		FMutationFacts& OutFacts,
		FExecutionMutationError& OutError)
	{
		OutFacts = FMutationFacts();
		OutError = FExecutionMutationError();
		{
			FScopeLock Lock(&StateMutex);
			if (bExecutionStarted)
			{
				return Fail(
					OutError,
					TEXT("generic_effect_execution_state_invalid"));
			}
			bExecutionStarted = true;
		}
		if (!ValidateExecutionBoundary(
				Request,
				Lease,
				Bindings,
				Resources,
				OutError))
		{
			return false;
		}

		FUeAgentGenericEffectApplyResult Result;
		const bool bApplied = UeHook->TryApply(
			Request,
			Lease,
			Bindings,
			Resources,
			Result,
			OutError);
		if (Result.DidMutationStart() && Result.IsValid())
		{
			OutFacts = FMutationFacts(
				Result.GetSubjectGeneration(),
				Result.GetMutatedIdentityKeys(),
				Result.GetRequiredBarrierSignals(),
				Result.IsRollbackSupported(),
				Result.GetDurableTargets());
			FScopeLock Lock(&StateMutex);
			AppliedOperationId = Request.GetOperationId();
			AppliedResult = Result;
		}
		if (!bApplied)
		{
			if (!OutError.IsPresent())
			{
				OutError = FExecutionMutationError(
					TEXT("generic_effect_apply_failed"));
			}
			return false;
		}
		if (!Result.IsValid() || !OutFacts.IsValid())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_apply_result_invalid"));
		}
		return true;
	}

	bool FUeAgentGenericEffectMutationHook::Rollback(
		const FExecutionMutationRequest& Request,
		const FMutationFacts& Facts,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		FUeAgentGenericEffectApplyResult Applied;
		{
			FScopeLock Lock(&StateMutex);
			if (bRollbackAttempted
				|| !AppliedResult.IsSet()
				|| AppliedOperationId != Request.GetOperationId())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_rollback_state_invalid"));
			}
			if (!SameFacts(Facts, AppliedResult.GetValue()))
			{
				return Fail(
					OutError,
					TEXT("generic_effect_rollback_facts_mismatch"));
			}
			if (!AppliedResult->IsRollbackSupported())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_rollback_unsupported"));
			}
			bRollbackAttempted = true;
			Applied = AppliedResult.GetValue();
		}
		return UeHook->TryRollback(
			Request,
			Applied,
			Resources,
			bOutConfirmed,
			OutError);
	}

	FUeAgentGenericEffectBarrierProvider::
	FUeAgentGenericEffectBarrierProvider(
		TSharedRef<
			IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentGenericEffectBarrierProvider::Wait(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_barrier_request_invalid"));
		}
		if (!ChargeWork(
				Resources,
				FMath::Max(1, Request.GetRequiredSignals().Num()),
				TEXT("domain_adapter.generic_effect.settle"),
				OutError))
		{
			return false;
		}
		if (!UeHook->TryWaitForSettle(
				Request,
				Resources,
				OutResult,
				OutError))
		{
			return false;
		}
		if (!OutResult.IsValid())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_settle_result_invalid"));
		}
		return true;
	}

	class FUeAgentReflectionGenericEffectUeHook::FState final
	{
	public:
		FCriticalSection Mutex;
		int64 NextGeneration = 1;
		TMap<FString, TSharedPtr<FOperationRecord>> Operations;
		TMap<int64, FSettleRecord> SettleRecords;
	};

	FUeAgentReflectionGenericEffectUeHook::
	FUeAgentReflectionGenericEffectUeHook()
		: State(MakeShared<FState, ESPMode::ThreadSafe>())
	{
	}

	FUeAgentReflectionGenericEffectUeHook::
	~FUeAgentReflectionGenericEffectUeHook() = default;

	bool FUeAgentReflectionGenericEffectUeHook::TryApply(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		const TArray<FUeAgentGenericEffectBinding>& Bindings,
		FUeAgentResourceTracker& Resources,
		FUeAgentGenericEffectApplyResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FUeAgentGenericEffectApplyResult();
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_requires_game_thread"));
		}

		const TSharedRef<FOperationRecord> Record =
			MakeShared<FOperationRecord>();
		Record->OperationId = Request.GetOperationId();
		{
			FScopeLock Lock(&State->Mutex);
			Record->Generation = State->NextGeneration++;
		}
		Record->RollbackToken = FString::Printf(
			TEXT("generic-effect:%s:%lld"),
			*Request.GetOperationId(),
			Record->Generation);

		TArray<FString> MutatedKeys;
		TArray<TWeakObjectPtr<UObject>> MutatedObjects;
		const TArray<FPlannedChange>& Changes =
			Request.GetPlan()->GetChangeSet().GetChanges();
		for (const FPlannedChange& Change : Changes)
		{
			if (!ChargeWork(
					Resources,
					1,
					TEXT("domain_adapter.generic_effect.reflection"),
					OutError))
			{
				break;
			}
			const FString& Key =
				Change.GetIdentity().GetCanonicalKey();
			const FUeAgentGenericEffectBinding* Binding =
				FindBinding(Bindings, Key);
			if (!Binding)
			{
				Fail(
					OutError,
					TEXT("generic_effect_binding_missing"),
					Key);
				break;
			}
			if (!TryApplyStringProperty(
					Change,
					*Binding,
					*Record,
					OutError))
			{
				break;
			}
			MutatedKeys.Add(Key);
			MutatedObjects.Add(
				Record->Mutations.Last().Object);
		}

		if (!Record->Mutations.IsEmpty())
		{
			const TArray<FString> Signals = {
				TEXT("ue_property_notification_complete"),
				TEXT("ue_object_state_settled"),
			};
			OutResult = FUeAgentGenericEffectApplyResult(
				Record->Generation,
				MutatedKeys,
				Signals,
				true,
				Record->RollbackToken,
				true);
			FScopeLock Lock(&State->Mutex);
			State->Operations.Add(
				Record->RollbackToken,
				Record);
			State->SettleRecords.Add(
				Record->Generation,
				{
					Record->Generation,
					MutatedObjects,
					Signals,
					FString::Printf(
						TEXT("generic-effect:%lld:settled"),
						Record->Generation),
				});
		}
		if (OutError.IsPresent())
		{
			return false;
		}
		if (!OutResult.IsValid())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_no_mutation"));
		}
		return true;
	}

	bool FUeAgentReflectionGenericEffectUeHook::TryRollback(
		const FExecutionMutationRequest& Request,
		const FUeAgentGenericEffectApplyResult& Applied,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_requires_game_thread"));
		}

		TSharedPtr<FOperationRecord> Record;
		{
			FScopeLock Lock(&State->Mutex);
			Record = State->Operations.FindRef(
				Applied.GetRollbackToken());
			if (!Record.IsValid()
				|| Record->OperationId != Request.GetOperationId()
				|| Record->Generation
					!= Applied.GetSubjectGeneration()
				|| Record->bRollbackStarted)
			{
				return Fail(
					OutError,
					TEXT("generic_effect_rollback_record_invalid"));
			}
			Record->bRollbackStarted = true;
		}

		for (int32 Index = Record->Mutations.Num() - 1;
			Index >= 0;
			--Index)
		{
			if (!ChargeWork(
					Resources,
					1,
					TEXT("domain_adapter.generic_effect.rollback"),
					OutError))
			{
				return false;
			}
			FPropertyMutation& Mutation =
				Record->Mutations[Index];
			UObject* Object = Mutation.Object.Get();
			if (!::IsValid(Object)
				|| !Mutation.Property
				|| !Mutation.OriginalValue.IsValid())
			{
				return Fail(
					OutError,
					TEXT("generic_effect_rollback_target_invalid"));
			}
			void* ValuePtr =
				Mutation.Property->ContainerPtrToValuePtr<void>(
					Object);
			if (!ValuePtr)
			{
				return Fail(
					OutError,
					TEXT(
						"generic_effect_property_storage_unavailable"));
			}
			Object->PreEditChange(Mutation.Property);
			Mutation.OriginalValue->Restore(ValuePtr);
			FPropertyChangedEvent Event(
				Mutation.Property,
				EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
			if (UPackage* Package = Object->GetOutermost())
			{
				Package->SetDirtyFlag(
					Mutation.bPackageWasDirty);
			}
		}

		{
			FScopeLock Lock(&State->Mutex);
			Record->bRolledBack = true;
			State->SettleRecords.Remove(Record->Generation);
			State->Operations.Remove(
				Applied.GetRollbackToken());
		}
		bOutConfirmed = true;
		return true;
	}

	void FUeAgentReflectionGenericEffectUeHook::
		ReleaseVerifiedOperation(const FString& OperationId)
	{
		TArray<TSharedPtr<FOperationRecord>> Released;
		{
			FScopeLock Lock(&State->Mutex);
			for (auto Iterator = State->Operations.CreateIterator();
				Iterator;
				++Iterator)
			{
				const TSharedPtr<FOperationRecord>& Record =
					Iterator.Value();
				if (Record.IsValid()
					&& Record->OperationId == OperationId)
				{
					State->SettleRecords.Remove(
						Record->Generation);
					Released.Add(Record);
					Iterator.RemoveCurrent();
				}
			}
		}
	}

	bool FUeAgentReflectionGenericEffectUeHook::TryWaitForSettle(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("generic_effect_requires_game_thread"));
		}
		FSettleRecord Record;
		{
			FScopeLock Lock(&State->Mutex);
			const FSettleRecord* Found =
				State->SettleRecords.Find(
					Request.GetSubjectGeneration());
			if (!Found)
			{
				return Fail(
					OutError,
					TEXT("generic_effect_settle_generation_unknown"));
			}
			Record = *Found;
		}

		bool bObjectsValid = true;
		for (const TWeakObjectPtr<UObject>& Object : Record.Objects)
		{
			if (!Object.IsValid())
			{
				bObjectsValid = false;
				break;
			}
		}
		bool bSignalsObserved = true;
		for (const FString& Required :
			Request.GetRequiredSignals())
		{
			if (!Record.ObservedSignals.Contains(Required))
			{
				bSignalsObserved = false;
				break;
			}
		}
		OutResult = FBarrierResult(
			Record.Generation,
			Record.ObservedSignals,
			Record.Revision,
			bObjectsValid && bSignalsObserved);
		return true;
	}
}
