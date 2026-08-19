#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::ReferenceAsset
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace DomainAdapters::GenericRead;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;

		const FString CatalogKind = TEXT("uai_reference_asset");
		const FString CatalogSourceId =
			TEXT("uai-reference-asset-catalog");
		const FString ReadPortId =
			TEXT("uai-reference-asset-generic-read");
		const FString AllowedRoot = TEXT("/Game/UAI/Reference");
		const FString Selector = TEXT("BlueprintDisplayName");

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError.Code = Cancelled.Code;
				OutError.SubjectId = Stage;
				OutError.Detail = Cancelled.Reason;
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				OutError.Code = Expired.Code;
				OutError.SubjectId = Stage;
				OutError.Detail = Expired.Stage;
				return false;
			}
			return true;
		}

		FUeAgentCanonicalIdentity MakeIdentity(
			const FString& ObjectPath,
			const FString& Generation)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("core.reference_asset.display_name"),
				TEXT("asset_property"),
				ObjectPath,
				Selector,
				TEXT("blueprint_asset"),
				Generation);
		}

		FUeAgentCandidateBinding MakeBinding(
			const FString& ObjectPath,
			const FString& Generation,
			const FUeAgentDescriptorIdentity& OperationDescriptor)
		{
			const FUeAgentCanonicalIdentity Identity =
				MakeIdentity(ObjectPath, Generation);
			const FString AssetName =
				FPackageName::ObjectPathToObjectName(ObjectPath);
			return FUeAgentCandidateBinding(
				TEXT("uai-reference-asset-binding:")
					+ Identity.GetCanonicalKey(),
				FUeAgentCandidateSummary(
					Identity,
					OperationDescriptor,
					CatalogKind,
					AssetName + TEXT(" Blueprint Display Name"),
					TEXT("Controlled reference CRUD asset"),
					{TEXT("UAI"), TEXT("ReferenceCrud")}),
				FUeAgentCatalogRevision(
					CatalogSourceId,
					Generation,
					1),
				ObjectPath,
				ReadPortId,
				{
					EUeAgentCandidateOperation::Read,
					EUeAgentCandidateOperation::Create,
					EUeAgentCandidateOperation::Update,
					EUeAgentCandidateOperation::Replace,
					EUeAgentCandidateOperation::Delete,
				},
				TEXT(
					"ue-reflection:UBlueprint.BlueprintDisplayName"));
		}

		FString PackageFilename(const FString& PackageName)
		{
			FString Filename;
			FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				Filename,
				FPackageName::GetAssetPackageExtension());
			return FPaths::ConvertRelativePathToFull(Filename);
		}

		int64 PersistedSequence(const FString& PackageName)
		{
			const FString Filename = PackageFilename(PackageName);
			if (!IFileManager::Get().FileExists(*Filename))
			{
				return 0;
			}
			return FMath::Max<int64>(
				1,
				IFileManager::Get()
					.GetTimeStamp(*Filename)
					.GetTicks());
		}

		bool Fail(
			FExecutionMutationError& OutError,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutError = FExecutionMutationError(Code, Detail);
			return false;
		}

		const FUeAgentGenericEffectBinding* FindBinding(
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			const FString& IdentityKey)
		{
			for (const FUeAgentGenericEffectBinding& Binding : Bindings)
			{
				if (Binding.GetCandidateBinding()
						.GetSummary()
						.GetIdentity()
						.GetCanonicalKey()
					== IdentityKey)
				{
					return &Binding;
				}
			}
			return nullptr;
		}
	}

	const FString& GetReferenceAssetCatalogKind()
	{
		return CatalogKind;
	}

	const FString& GetReferenceAssetCatalogSourceId()
	{
		return CatalogSourceId;
	}

	const FString& GetReferenceAssetReadPortId()
	{
		return ReadPortId;
	}

	const FString& GetReferenceAssetAllowedRoot()
	{
		return AllowedRoot;
	}

	bool TryNormalizeReferenceAssetPath(
		const FString& Input,
		FString& OutPackageName,
		FString& OutObjectPath,
		FString& OutErrorCode)
	{
		OutPackageName.Reset();
		OutObjectPath.Reset();
		OutErrorCode.Reset();
		FString Normalized = Input.TrimStartAndEnd();
		Normalized.RemoveFromStart(TEXT("'"));
		Normalized.RemoveFromEnd(TEXT("'"));
		int32 DotIndex = INDEX_NONE;
		const int32 LastSlash = Normalized.Find(
			TEXT("/"),
			ESearchCase::CaseSensitive,
			ESearchDir::FromEnd);
		if (Normalized.FindChar(TEXT('.'), DotIndex)
			&& DotIndex > LastSlash)
		{
			OutPackageName = Normalized.Left(DotIndex);
		}
		else
		{
			OutPackageName = Normalized;
		}
		OutPackageName.TrimStartAndEndInline();
		const FString RootPrefix = AllowedRoot + TEXT("/");
		if (!OutPackageName.StartsWith(
				RootPrefix,
				ESearchCase::CaseSensitive)
			|| !FPackageName::IsValidLongPackageName(OutPackageName)
			|| FPackageName::GetLongPackageAssetName(
					OutPackageName)
				.IsEmpty())
		{
			OutErrorCode = TEXT("reference_asset_path_invalid");
			OutPackageName.Reset();
			return false;
		}
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(OutPackageName);
		OutObjectPath =
			OutPackageName + TEXT(".") + AssetName;
		if (DotIndex != INDEX_NONE
			&& Normalized != OutObjectPath)
		{
			OutErrorCode =
				TEXT("reference_asset_object_name_mismatch");
			OutPackageName.Reset();
			OutObjectPath.Reset();
			return false;
		}
		return true;
	}

	FUeAgentReferenceAssetCatalogSource::
		FUeAgentReferenceAssetCatalogSource(
			FUeAgentDescriptorIdentity InOperationDescriptor,
			FString InGeneration)
		: OperationDescriptor(MoveTemp(InOperationDescriptor))
		, Generation(MoveTemp(InGeneration))
		, Kinds({CatalogKind})
	{
		Generation.TrimStartAndEndInline();
	}

	const FString&
	FUeAgentReferenceAssetCatalogSource::GetSourceId() const
	{
		return CatalogSourceId;
	}

	const TArray<FString>&
	FUeAgentReferenceAssetCatalogSource::GetCatalogKinds() const
	{
		return Kinds;
	}

	bool FUeAgentReferenceAssetCatalogSource::TrySearch(
		const FUeAgentDiscoveryQuery& Query,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
		FUeAgentDiscoveryError& OutError)
	{
		OutCatalog.Reset();
		OutError = {};
		if (!OperationDescriptor.IsValid()
			|| Generation.IsEmpty())
		{
			OutError.Code =
				TEXT("reference_asset_catalog_invalid");
			return false;
		}
		if (!Checkpoint(
				Deadline,
				Cancellation,
				TEXT("reference_asset_catalog.search"),
				OutError))
		{
			return false;
		}
		FString PackageName;
		FString ObjectPath;
		FString ErrorCode;
		TArray<FUeAgentCandidateSummary> Candidates;
		if (TryNormalizeReferenceAssetPath(
				Query.GetSearchText(),
				PackageName,
				ObjectPath,
				ErrorCode))
		{
			const FUeAgentCandidateBinding Binding =
				MakeBinding(
					ObjectPath,
					Generation,
					OperationDescriptor);
			Candidates.Add(Binding.GetSummary());
			FScopeLock Lock(&CacheMutex);
			ObjectPathByCandidateId.Add(
				Binding.GetSummary()
					.GetIdentity()
					.GetCanonicalKey(),
				ObjectPath);
		}
		else if (Query.GetMatchMode()
			== EUeAgentDiscoveryMatchMode::Exact)
		{
			OutError.Code = ErrorCode;
			OutError.SubjectId = Query.GetSearchText();
			OutError.Detail =
				TEXT("reference asset path is outside the controlled root");
			return false;
		}
		return FUeAgentCatalogSnapshot::TryCreate(
			TEXT("uai-reference-asset-catalog:") + Generation,
			CatalogKind,
			FUeAgentCatalogRevision(
				CatalogSourceId,
				Generation,
				1),
			EUeAgentCatalogCompleteness::Complete,
			Candidates,
			{},
			Resources,
			OutCatalog,
			OutError);
	}

	bool FUeAgentReferenceAssetCatalogSource::TryResolveExact(
		const FString& CandidateId,
		const FUeAgentCatalogRevision& ExpectedRevision,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
		FUeAgentDiscoveryError& OutError)
	{
		OutBinding.Reset();
		OutError = {};
		if (!Checkpoint(
				Deadline,
				Cancellation,
				TEXT("reference_asset_catalog.resolve"),
				OutError))
		{
			return false;
		}
		const FUeAgentCatalogRevision Current(
			CatalogSourceId,
			Generation,
			1);
		if (ExpectedRevision != Current)
		{
			OutError.Code = TEXT("catalog_revision_stale");
			OutError.SubjectId = CandidateId;
			return false;
		}
		FString ObjectPath;
		{
			FScopeLock Lock(&CacheMutex);
			const FString* Found =
				ObjectPathByCandidateId.Find(CandidateId);
			if (Found)
			{
				ObjectPath = *Found;
			}
		}
		if (ObjectPath.IsEmpty())
		{
			OutError.Code =
				TEXT("reference_asset_candidate_not_found");
			OutError.SubjectId = CandidateId;
			return false;
		}
		const FUeAgentCandidateBinding Binding =
			MakeBinding(
				ObjectPath,
				Generation,
				OperationDescriptor);
		OutBinding =
			MakeShared<const FUeAgentCandidateBinding>(Binding);
		return true;
	}

	FUeAgentReferenceAssetReadHook::
		FUeAgentReferenceAssetReadHook(FString InGeneration)
		: Generation(MoveTemp(InGeneration))
	{
		Generation.TrimStartAndEndInline();
	}

	bool FUeAgentReferenceAssetReadHook::TryCapture(
		const FUeAgentStateReadRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentUeReadCapture>& OutCapture,
		FUeAgentGenericReadError& OutError)
	{
		OutCapture.Reset();
		OutError = {};
		FUeAgentDiscoveryError CheckpointError;
		if (!Checkpoint(
				Deadline,
				Cancellation,
				TEXT("reference_asset_read.capture"),
				CheckpointError))
		{
			OutError = {
				CheckpointError.Code,
				CheckpointError.SubjectId,
				CheckpointError.Detail,
			};
			return false;
		}
		const FUeAgentCandidateBinding& Binding =
			Request.GetBinding();
		FString PackageName;
		FString ObjectPath;
		FString ErrorCode;
		if (Binding.GetReadPortId() != ReadPortId
			|| !TryNormalizeReferenceAssetPath(
				Binding.GetPublicRef(),
				PackageName,
				ObjectPath,
				ErrorCode)
			|| Binding.GetSummary().GetIdentity().GetOwnerId()
				!= ObjectPath)
		{
			OutError = {
				ErrorCode.IsEmpty()
					? TEXT("reference_asset_binding_invalid")
					: ErrorCode,
				Binding.GetBindingId(),
				TEXT("reference asset binding does not match its public ref"),
			};
			return false;
		}
		const int64 Sequence = PersistedSequence(PackageName);
		const FUeAgentStateRevision Revision(
			ReadPortId,
			Generation,
			Sequence);
		TArray<FUeAgentUeReadEntry> Entries;
		if (Sequence > 0)
		{
			UBlueprint* Blueprint =
				LoadObject<UBlueprint>(nullptr, *ObjectPath);
			if (!IsValid(Blueprint)
				|| (Request.GetStatePlane()
						== EUeAgentStatePlane::Persisted
					&& Blueprint->GetOutermost()->IsDirty()))
			{
				OutError = {
					TEXT(
						"reference_asset_persisted_state_unavailable"),
					ObjectPath,
					TEXT(
						"persisted reference asset is unavailable or dirty"),
				};
				return false;
			}
			FUeAgentCanonicalValuePtr Value;
			FUeAgentCanonicalValuePtr DefaultValue;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					Blueprint->BlueprintDisplayName,
					Resources,
					Value,
					StateError)
				|| !FUeAgentCanonicalValue::TryString(
					FString(),
					Resources,
					DefaultValue,
					StateError))
			{
				OutError = {
					StateError.Code,
					ObjectPath,
					StateError.Detail,
				};
				return false;
			}
			const FUeAgentCanonicalType CanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar"));
			Entries.Emplace(
				MakeIdentity(ObjectPath, Generation),
				FUeAgentUeTypeBinding(
					GetUeStringPropertySourceTypeId(),
					CanonicalType,
					TEXT(
						"ue-reflection:UBlueprint.BlueprintDisplayName")),
				Value.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::ClassDefaultObject,
					TEXT("/Script/Engine.Blueprint"),
					TEXT("UBlueprint.BlueprintDisplayName"),
					DefaultValue.ToSharedRef()),
				FUeAgentStateDetails(
					TEXT("Blueprint Display Name"),
					TEXT("Controlled reference CRUD string property"),
					{TEXT("UAI"), TEXT("ReferenceCrud")},
					true,
					TEXT("available"),
					{{TEXT("allowed_root"), AllowedRoot}}),
				Revision);
		}
		const FString EvidenceId = FString::Printf(
			TEXT("reference-asset-persisted:%s:%lld:%s"),
			*PackageName,
			Sequence,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		OutCapture =
			MakeShared<const FUeAgentUeReadCapture>(
				TEXT("reference-asset-snapshot:")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits),
				Request.GetStatePlane(),
				Revision,
				FUeAgentTemporalPoint(
					TEXT("ue-editor"),
					FString::Printf(
						TEXT("%lld"),
						FDateTime::UtcNow().GetTicks())),
				EvidenceId,
				true,
				MoveTemp(Entries));
		return OutCapture->IsValid();
	}

	class FUeAgentReferenceAssetEffectHook::FState final
	{
	public:
		enum class EKind : uint8
		{
			Reflection,
			Create,
			Delete,
		};

		struct FRecord
		{
			EKind Kind = EKind::Reflection;
			FString OperationId;
			FString RollbackToken;
			int64 Generation = -1;
			FUeAgentGenericEffectApplyResult ReflectionResult;
			TStrongObjectPtr<UObject> Object;
			TWeakObjectPtr<UPackage> OriginalPackage;
			FString OriginalName;
			EObjectFlags OriginalFlags = RF_NoFlags;
			bool bPackageWasDirty = false;
		};

		FCriticalSection Mutex;
		int64 NextGeneration = 1;
		TMap<FString, TSharedPtr<FRecord>> Records;
	};

	FUeAgentReferenceAssetEffectHook::
		FUeAgentReferenceAssetEffectHook()
		: State(MakeShared<FState, ESPMode::ThreadSafe>())
		, Reflection(
			MakeShared<
				FUeAgentReflectionGenericEffectUeHook,
				ESPMode::ThreadSafe>())
	{
	}

	FUeAgentReferenceAssetEffectHook::
		~FUeAgentReferenceAssetEffectHook() = default;

	bool FUeAgentReferenceAssetEffectHook::TryApply(
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
				TEXT("reference_asset_effect_requires_game_thread"));
		}
		const TArray<FPlannedChange>& Changes =
			Request.GetPlan()->GetChangeSet().GetChanges();
		if (Changes.Num() != 1)
		{
			return Fail(
				OutError,
				TEXT("reference_asset_effect_change_count_invalid"));
		}
		const FPlannedChange& Change = Changes[0];
		const FUeAgentGenericEffectBinding* Binding =
			FindBinding(
				Bindings,
				Change.GetIdentity().GetCanonicalKey());
		if (!Binding)
		{
			return Fail(
				OutError,
				TEXT("generic_effect_binding_missing"),
				Change.GetIdentity().GetCanonicalKey());
		}
		FString PackageName;
		FString ObjectPath;
		FString PathError;
		if (!TryNormalizeReferenceAssetPath(
				Binding->GetCandidateBinding().GetPublicRef(),
				PackageName,
				ObjectPath,
				PathError))
		{
			return Fail(OutError, PathError, ObjectPath);
		}

		const TSharedRef<FState::FRecord> Record =
			MakeShared<FState::FRecord>();
		Record->OperationId = Request.GetOperationId();
		{
			FScopeLock Lock(&State->Mutex);
			Record->Generation = State->NextGeneration++;
		}
		Record->RollbackToken = FString::Printf(
			TEXT("reference-asset:%s:%lld"),
			*Request.GetOperationId(),
			Record->Generation);

		if (Change.GetIntent() == EChangeIntent::Update
			|| Change.GetIntent() == EChangeIntent::Replace)
		{
			Record->Kind = FState::EKind::Reflection;
			if (!Reflection->TryApply(
					Request,
					Lease,
					Bindings,
					Resources,
					Record->ReflectionResult,
					OutError))
			{
				return false;
			}
			OutResult = FUeAgentGenericEffectApplyResult(
				Record->Generation,
				Record->ReflectionResult.GetMutatedIdentityKeys(),
				Record->ReflectionResult
					.GetRequiredBarrierSignals(),
				Record->ReflectionResult.IsRollbackSupported(),
				Record->RollbackToken,
				Record->ReflectionResult.DidMutationStart());
		}
		else if (Change.GetIntent() == EChangeIntent::Create)
		{
			if (!Change.GetAfter().IsValid()
				|| Change.GetAfter()->GetKind()
					!= EUeAgentValueKind::String
				|| IFileManager::Get().FileExists(
					*PackageFilename(PackageName))
				|| FindObject<UObject>(nullptr, *ObjectPath))
			{
				return Fail(
					OutError,
					TEXT("reference_asset_create_precondition_failed"),
					ObjectPath);
			}
			UPackage* Package = CreatePackage(*PackageName);
			const FString AssetName =
				FPackageName::GetLongPackageAssetName(PackageName);
			UBlueprint* Blueprint =
				FKismetEditorUtilities::CreateBlueprint(
					AActor::StaticClass(),
					Package,
					*AssetName,
					BPTYPE_Normal,
					UBlueprint::StaticClass(),
					UBlueprintGeneratedClass::StaticClass());
			if (!IsValid(Blueprint))
			{
				return Fail(
					OutError,
					TEXT("reference_asset_create_failed"),
					ObjectPath);
			}
			Blueprint->BlueprintDisplayName =
				Change.GetAfter()->GetText();
			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();
			Record->Kind = FState::EKind::Create;
			Record->Object.Reset(Blueprint);
			Record->OriginalPackage = Package;
			Record->OriginalName = AssetName;
			Record->OriginalFlags = Blueprint->GetFlags();
			OutResult = FUeAgentGenericEffectApplyResult(
				Record->Generation,
				{Change.GetIdentity().GetCanonicalKey()},
				{TEXT("reference_asset_object_settled")},
				true,
				Record->RollbackToken,
				true);
		}
		else if (Change.GetIntent() == EChangeIntent::Delete)
		{
			UObject* Object =
				FindObject<UObject>(nullptr, *ObjectPath);
			if (!IsValid(Object))
			{
				Object = LoadObject<UObject>(
					nullptr,
					*ObjectPath);
			}
			if (!IsValid(Object)
				|| !IFileManager::Get().FileExists(
					*PackageFilename(PackageName)))
			{
				return Fail(
					OutError,
					TEXT("reference_asset_delete_precondition_failed"),
					ObjectPath);
			}
			UPackage* Package = Object->GetOutermost();
			Record->Kind = FState::EKind::Delete;
			Record->Object.Reset(Object);
			Record->OriginalPackage = Package;
			Record->OriginalName = Object->GetName();
			Record->OriginalFlags = Object->GetFlags();
			Record->bPackageWasDirty = Package->IsDirty();
			FAssetRegistryModule::AssetDeleted(Object);
			Object->ClearFlags(RF_Public | RF_Standalone);
			if (!Object->Rename(
					nullptr,
					GetTransientPackage(),
					REN_DontCreateRedirectors
						| REN_NonTransactional))
			{
				return Fail(
					OutError,
					TEXT("reference_asset_delete_detach_failed"),
					ObjectPath);
			}
			Package->MarkPackageDirty();
			OutResult = FUeAgentGenericEffectApplyResult(
				Record->Generation,
				{Change.GetIdentity().GetCanonicalKey()},
				{TEXT("reference_asset_object_settled")},
				true,
				Record->RollbackToken,
				true);
		}
		else
		{
			return Fail(
				OutError,
				TEXT("reference_asset_intent_unsupported"));
		}
		{
			FScopeLock Lock(&State->Mutex);
			State->Records.Add(
				Record->RollbackToken,
				Record);
		}
		return OutResult.IsValid();
	}

	bool FUeAgentReferenceAssetEffectHook::TryRollback(
		const FExecutionMutationRequest& Request,
		const FUeAgentGenericEffectApplyResult& Applied,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		TSharedPtr<FState::FRecord> Record;
		{
			FScopeLock Lock(&State->Mutex);
			Record = State->Records.FindRef(
				Applied.GetRollbackToken());
		}
		if (!Record.IsValid()
			|| Record->OperationId != Request.GetOperationId())
		{
			return Fail(
				OutError,
				TEXT("reference_asset_rollback_record_invalid"));
		}
		if (Record->Kind == FState::EKind::Reflection)
		{
			const bool bRolledBack = Reflection->TryRollback(
				Request,
				Record->ReflectionResult,
				Resources,
				bOutConfirmed,
				OutError);
			if (bRolledBack && bOutConfirmed)
			{
				FScopeLock Lock(&State->Mutex);
				State->Records.Remove(
					Applied.GetRollbackToken());
			}
			return bRolledBack;
		}
		UObject* Object = Record->Object.Get();
		if (!IsValid(Object))
		{
			return Fail(
				OutError,
				TEXT("reference_asset_rollback_object_invalid"));
		}
		if (Record->Kind == FState::EKind::Create)
		{
			FAssetRegistryModule::AssetDeleted(Object);
			Object->ClearFlags(RF_Public | RF_Standalone);
			bOutConfirmed = Object->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors
					| REN_NonTransactional);
		}
		else
		{
			UPackage* Package = Record->OriginalPackage.Get();
			if (!IsValid(Package))
			{
				return Fail(
					OutError,
					TEXT(
						"reference_asset_rollback_package_invalid"));
			}
			bOutConfirmed = Object->Rename(
				*Record->OriginalName,
				Package,
				REN_DontCreateRedirectors
					| REN_NonTransactional);
			if (bOutConfirmed)
			{
				Object->SetFlags(Record->OriginalFlags);
				FAssetRegistryModule::AssetCreated(Object);
				Package->SetDirtyFlag(
					Record->bPackageWasDirty);
			}
		}
		if (!bOutConfirmed)
		{
			return Fail(
				OutError,
				TEXT("reference_asset_rollback_failed"));
		}
		{
			FScopeLock Lock(&State->Mutex);
			State->Records.Remove(
				Applied.GetRollbackToken());
		}
		return true;
	}

	void FUeAgentReferenceAssetEffectHook::
		ReleaseVerifiedOperation(const FString& OperationId)
	{
		TSharedPtr<FState::FRecord> Released;
		{
			FScopeLock Lock(&State->Mutex);
			FString RollbackToken;
			for (const TPair<
				FString,
				TSharedPtr<FState::FRecord>>& Pair :
				State->Records)
			{
				if (Pair.Value.IsValid()
					&& Pair.Value->OperationId == OperationId)
				{
					RollbackToken = Pair.Key;
					Released = Pair.Value;
					break;
				}
			}
			if (!RollbackToken.IsEmpty())
			{
				State->Records.Remove(RollbackToken);
			}
		}
		if (Released.IsValid()
			&& Released->Kind == FState::EKind::Reflection)
		{
			Reflection->ReleaseVerifiedOperation(OperationId);
		}
	}

	bool FUeAgentReferenceAssetEffectHook::TryWaitForSettle(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		TSharedPtr<FState::FRecord> Record;
		{
			FScopeLock Lock(&State->Mutex);
			for (const TPair<
				FString,
				TSharedPtr<FState::FRecord>>& Pair :
				State->Records)
			{
				if (Pair.Value.IsValid()
					&& Pair.Value->Generation
						== Request.GetSubjectGeneration())
				{
					Record = Pair.Value;
					break;
				}
			}
		}
		if (!Record.IsValid())
		{
			return Fail(
				OutError,
				TEXT("reference_asset_settle_generation_unknown"));
		}
		if (Record->Kind == FState::EKind::Reflection)
		{
			FBarrierResult ReflectionResult;
			if (!Reflection->TryWaitForSettle(
				FBarrierRequest(
					Record->ReflectionResult
						.GetSubjectGeneration(),
					Request.GetRequiredSignals()),
				Resources,
				ReflectionResult,
				OutError))
			{
				return false;
			}
			OutResult = FBarrierResult(
				Record->Generation,
				ReflectionResult.GetObservedSignals(),
				ReflectionResult.GetPostBarrierRevision(),
				ReflectionResult.IsSettled());
			return OutResult.IsValid();
		}
		const TArray<FString> Signals = {
			TEXT("reference_asset_object_settled"),
		};
		const bool bSignalsMatch =
			Request.GetRequiredSignals() == Signals;
		OutResult = FBarrierResult(
			Record->Generation,
			Signals,
			TEXT("reference-asset-settled:")
				+ FString::FromInt(Record->Generation),
			bSignalsMatch);
		return true;
	}
}
