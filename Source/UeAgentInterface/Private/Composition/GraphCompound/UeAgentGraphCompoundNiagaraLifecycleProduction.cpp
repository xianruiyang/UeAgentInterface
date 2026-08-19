#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraLifecycleProduction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::MaterialNiagaraGraph;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		const FString NiagaraAssetReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_asset_lifecycle");
		const FString NiagaraCompileReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_compile_settle");
		const FString CompileSemanticId =
			TEXT("property:lifecycle:compile_refresh_settle");

		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		bool IsNiagaraAsset(const UObject* Asset)
		{
			return Asset
				&& (Asset->IsA<UNiagaraEmitter>()
					|| Asset->IsA<UNiagaraSystem>()
					|| Asset->IsA<UNiagaraScript>());
		}

		FString PackageNameFromRef(const FString& Ref)
		{
			return Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: Ref;
		}

		FString ObjectPathFromRef(const FString& Ref)
		{
			if (Ref.Contains(TEXT(".")))
			{
				return Ref;
			}
			return Ref + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(Ref);
		}

		UObject* TryResolveNiagaraAsset(
			const FString& Ref,
			FGraphCompoundRuntimeError& OutError,
			const bool bSource)
		{
			const FString PackageName = PackageNameFromRef(Ref);
			const FString ObjectPath = ObjectPathFromRef(Ref);
			UObject* const Asset = FPackageName::IsValidLongPackageName(PackageName)
				? LoadObject<UObject>(nullptr, *ObjectPath)
				: nullptr;
			if (!IsNiagaraAsset(Asset)
				|| Asset->GetOutermost()->GetName() != PackageName)
			{
				SetError(OutError,
					bSource
						? TEXT("graph_compound_niagara_source_invalid")
						: TEXT("graph_compound_niagara_target_invalid"),
					Ref);
				return nullptr;
			}
			return Asset;
		}

		UObject* TryResolveSource(
			const FGraphCompoundAssetCreateRequest& Request,
			FGraphCompoundRuntimeError& OutError)
		{
			UObject* const Source = TryResolveNiagaraAsset(
				Request.GetSourceRef(), OutError, true);
			const bool bExpectedClass = Source
				&& ((Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate
						&& Source->GetClass() == UNiagaraEmitter::StaticClass())
					|| (Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate
						&& Source->GetClass() == UNiagaraSystem::StaticClass())
					|| Request.GetInputKind()
						== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate);
			if (!bExpectedClass)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_source_invalid"),
					Request.GetSourceRef());
				return nullptr;
			}
			return Source;
		}

		bool TryPackageFilename(
			const FString& PackageName,
			FString& OutFilename)
		{
			OutFilename.Reset();
			return FPackageName::IsValidLongPackageName(PackageName)
				&& !FPackageName::GetPackageMountPoint(PackageName).IsNone()
				&& FPackageName::TryConvertLongPackageNameToFilename(
					PackageName, OutFilename,
					FPackageName::GetAssetPackageExtension());
		}

		bool TryCheckTargetAbsent(
			const FGraphCompoundAssetCreateRequest& Request,
			FString& OutFilename,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!Request.IsValid()
				|| !TryPackageFilename(Request.GetTargetRef(), OutFilename))
			{
				SetError(OutError,
					TEXT("graph_compound_asset_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (FindPackage(nullptr, *Request.GetTargetRef())
				|| IFileManager::Get().FileExists(*OutFilename))
			{
				SetError(OutError,
					TEXT("graph_compound_asset_target_already_exists"),
					Request.GetTargetRef());
				return false;
			}
			return true;
		}

		FString NiagaraFingerprint(const UObject& Asset)
		{
			FString Detail = Asset.GetClass()->GetPathName();
			if (const UNiagaraSystem* const System = Cast<UNiagaraSystem>(&Asset))
			{
				Detail += TEXT(":warmup=")
					+ FString::SanitizeFloat(System->GetWarmupTime());
			}
			else if (const UNiagaraEmitter* const Emitter =
				Cast<UNiagaraEmitter>(&Asset))
			{
				const FVersionedNiagaraEmitterData* const Data =
					Emitter->GetLatestEmitterData();
				Detail += FString::Printf(
					TEXT(":local_space=%d"), Data && Data->bLocalSpace ? 1 : 0);
			}
			Detail += FString::Printf(
				TEXT(":dirty=%d"), Asset.GetOutermost()->IsDirty() ? 1 : 0);
			return Detail;
		}

		FUeAgentCanonicalIdentity Identity(
			const FString& SemanticId,
			const FString& TargetKind,
			const FString& OwnerId,
			const FString& Generation)
		{
			return FUeAgentCanonicalIdentity(
				SemanticId, TargetKind, OwnerId, SemanticId,
				TEXT("graph_compound"), Generation);
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Text, Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError, Error.Code, Subject, Error.Path);
				return false;
			}
			return true;
		}

		bool TryBuildSnapshot(
			const FString& SnapshotId,
			const FString& SubjectRef,
			const FString& TargetKind,
			const FString& Generation,
			const FString& SemanticId,
			const FString& OwnerId,
			const FString& CurrentText,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentCanonicalValuePtr Current;
			if (!TryStringValue(
					CurrentText, Resources, Current, OutError, SubjectRef))
			{
				return false;
			}
			const FUeAgentStateRevision Revision(
				TEXT("graph-compound:c4:niagara"), Generation, 0);
			FUeAgentStateEntry Entry(
				Identity(SemanticId, TargetKind, OwnerId, Generation),
				FUeAgentCanonicalType(
					TEXT("uai.graph_compound.state.v1"),
					EUeAgentValueKind::String, false, TEXT("scalar")),
				Current.ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					SubjectRef, TEXT("independent Niagara readback"),
					Current.ToSharedRef()),
				FUeAgentStateDetails(
					SemanticId,
					TEXT("GraphCompound independent Niagara state"),
					{TEXT("GraphCompound"), TEXT("Niagara")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
			if (!Entry.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_state_entry_invalid"),
					SemanticId);
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					SnapshotId,
					EUeAgentStatePlane::Persisted,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						SubjectRef,
						TEXT("niagara:") + Generation),
					{MoveTemp(Entry)}, Resources,
					OutSnapshot, Error)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, Error.Code, SubjectRef, Error.Path);
				return false;
			}
			return true;
		}

		FString ExpectedDefinition(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundAssetCreateRequest& Request)
		{
			return Request.GetInputKind()
				== EGraphCompoundTypedInputKind::NiagaraAssetDuplicate
				? Request.GetSourceRef()
				: Profile.GetTargetKind() + TEXT(":")
					+ Request.GetSourceRef();
		}

		bool TryUnloadPackage(
			const FString& PackageName,
			const FString& AssetName,
			const bool bNotifyAssetRegistry,
			FString& OutDetail)
		{
			OutDetail.Reset();
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			if (bNotifyAssetRegistry)
			{
				if (UObject* const Asset = FindObject<UObject>(Package, *AssetName))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *PackageName))
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
		}

		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(), Prepared->GetInventory(),
				Prepared->GetJournalId(), Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(), Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(), Receipt.GetPhaseHistory());
		}

		bool TryRollback(
			const FString& PackageName,
			const FString& AssetName,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadPackage(
				PackageName, AssetName, bNotifyAssetRegistry, UnloadDetail);
			FDurableReceipt Receipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, Receipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail
				: Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		bool TryValidateCompileReadback(
			UObject& Asset,
			FString& OutDiagnostic)
		{
			const UeAgentNiagaraCompileValidation::FResult Compile =
				UeAgentNiagaraCompileValidation::InspectAsset(&Asset);
			if (!Compile.bCompilationComplete || Compile.bHasError
				|| Compile.ScriptCount <= 0)
			{
				OutDiagnostic =
					TEXT("graph_compound_niagara_compile_readback_failed");
				return false;
			}
			if (UNiagaraSystem* const System = Cast<UNiagaraSystem>(&Asset))
			{
				if (System->HasOutstandingCompilationRequests(true)
					|| System->NeedsRequestCompile())
				{
					OutDiagnostic =
						TEXT("graph_compound_niagara_compile_not_settled");
					return false;
				}
			}
			return true;
		}

		bool TryValidateReloadedAsset(
			const FGraphCompoundAssetCreateRequest& Request,
			FString& OutDiagnostic)
		{
			OutDiagnostic.Reset();
			FGraphCompoundRuntimeError ResolveError;
			UObject* const Source = TryResolveSource(Request, ResolveError);
			UObject* const Target = LoadObject<UObject>(
				nullptr, *ObjectPathFromRef(Request.GetTargetRef()));
			if (!Source || !Target)
			{
				OutDiagnostic = Source
					? TEXT("graph_compound_niagara_reload_failed")
					: ResolveError.Code;
				return false;
			}
			if (Target->GetClass() != Source->GetClass()
				|| Target->GetOutermost()->GetName() != Request.GetTargetRef()
				|| Target->GetPathName() == Source->GetPathName()
				|| Target->GetOutermost()->IsDirty())
			{
				OutDiagnostic =
					TEXT("graph_compound_niagara_reload_identity_mismatch");
				return false;
			}
			if (const UNiagaraSystem* const SourceSystem =
				Cast<UNiagaraSystem>(Source))
			{
				const UNiagaraSystem* const TargetSystem =
					CastChecked<UNiagaraSystem>(Target);
				if (!FMath::IsNearlyEqual(
						SourceSystem->GetWarmupTime(),
						TargetSystem->GetWarmupTime()))
				{
					OutDiagnostic =
						TEXT("graph_compound_niagara_template_state_mismatch");
					return false;
				}
			}
			else if (const UNiagaraEmitter* const SourceEmitter =
				Cast<UNiagaraEmitter>(Source))
			{
				const FVersionedNiagaraEmitterData* const SourceData =
					SourceEmitter->GetLatestEmitterData();
				const FVersionedNiagaraEmitterData* const TargetData =
					CastChecked<UNiagaraEmitter>(Target)->GetLatestEmitterData();
				if (!SourceData || !TargetData
					|| SourceData->bLocalSpace != TargetData->bLocalSpace)
				{
					OutDiagnostic =
						TEXT("graph_compound_niagara_template_state_mismatch");
					return false;
				}
			}
			return TryValidateCompileReadback(*Target, OutDiagnostic);
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			const FString& Revision,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("revision"), Revision);
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_result_invalid"),
					TargetRef, Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FNiagaraAssetReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FNiagaraAssetReadbackOracle(
				FGraphCompoundDomainRouteProfile InProfile,
				FGraphCompoundAssetCreateRequest InRequest)
				: Profile(MoveTemp(InProfile))
				, Request(MoveTemp(InRequest))
			{
			}

			const FString& GetOracleId() const override
			{
				return NiagaraAssetReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& ReadbackRequest,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				FString Detail;
				if (!TryUnloadPackage(
						Request.GetTargetRef(), Request.GetAssetName(),
						false, Detail))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							TEXT("graph_compound_niagara_unload_failed"), Detail);
					return true;
				}
				FString Diagnostic;
				if (!TryValidateReloadedAsset(Request, Diagnostic))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							Diagnostic, TEXT("reload:")
								+ Request.GetTargetRef());
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryStringValue(
						ExpectedDefinition(Profile, Request), Resources,
						Actual, Error, Request.GetTargetRef()))
				{
					OutError = FVerificationError(Error.Code, Error.Detail);
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + ReadbackRequest.GetSubjectRef()
						+ TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			FGraphCompoundAssetCreateRequest Request;
		};

		class FNiagaraCompileReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			explicit FNiagaraCompileReadbackOracle(FString InTargetRef)
				: TargetRef(MoveTemp(InTargetRef))
			{
			}

			const FString& GetOracleId() const override
			{
				return NiagaraCompileReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& ReadbackRequest,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				const FString AssetName =
					FPackageName::GetLongPackageAssetName(TargetRef);
				FString Detail;
				if (!TryUnloadPackage(TargetRef, AssetName, false, Detail))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							TEXT("graph_compound_niagara_unload_failed"), Detail);
					return true;
				}
				FGraphCompoundRuntimeError ResolveError;
				UObject* const Target = TryResolveNiagaraAsset(
					TargetRef, ResolveError, false);
				FString Diagnostic;
				if (!Target || Target->GetOutermost()->IsDirty()
					|| !TryValidateCompileReadback(*Target, Diagnostic))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(
							!Target ? ResolveError.Code : Diagnostic,
							TEXT("reload:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryStringValue(
						TEXT("requested"), Resources, Actual,
						Error, TargetRef))
				{
					OutError = FVerificationError(Error.Code, Error.Detail);
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + ReadbackRequest.GetSubjectRef()
						+ TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
		};
	}

	FGraphCompoundNiagaraLifecycleProduction::
	FGraphCompoundNiagaraLifecycleProduction(
		FString InJournalRoot,
		FMaterialNiagaraGraphNativeHook& InNativeHook,
		FPackageAssetDurableBackend& InDurableBackend,
		int64& InLastFencingToken)
		: JournalRoot(MoveTemp(InJournalRoot))
		, NativeHook(InNativeHook)
		, DurableBackend(InDurableBackend)
		, LastFencingToken(InLastFencingToken)
	{
	}

	bool FGraphCompoundNiagaraLifecycleProduction::
	TryCaptureAssetCreateSnapshot(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundAssetCreateRequest& Request,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FGraphCompoundRuntimeError& OutError) const
	{
		OutSnapshot.Reset();
		OutError = {};
		FString Filename;
		UObject* const Source = TryResolveSource(Request, OutError);
		if (!Source || !TryCheckTargetAbsent(Request, Filename, OutError))
		{
			return false;
		}
		const FString Generation =
			TEXT("absent:") + Request.GetTargetRef()
			+ TEXT(":source:") + NiagaraFingerprint(*Source);
		return TryBuildSnapshot(
			TEXT("graph-compound:c4:niagara-create:")
				+ Request.GetTargetRef(),
			Request.GetSourceRef(), Profile.GetTargetKind(), Generation,
			TEXT("target:") + Request.GetSourceRef(),
			Request.GetSourceRef(), Source->GetPathName(),
			Resources, OutSnapshot, OutError);
	}

	bool FGraphCompoundNiagaraLifecycleProduction::TryExecuteAssetCreate(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundAssetCreateRequest& Request,
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentResourceTracker& Resources,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		FUeAgentContractValue& OutResult,
		FGraphCompoundRuntimeError& OutError)
	{
		OutResult = FUeAgentContractValue(TEXT("{}"));
		OutError = {};
		if (!IsInGameThread() || Cancellation->IsCancelled())
		{
			SetError(OutError,
				Cancellation->IsCancelled()
					? TEXT("graph_compound_niagara_asset_cancelled")
					: TEXT("graph_compound_niagara_asset_thread_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		if (Plan->GetDescriptorIdentity()
				!= Profile.GetExecutionContract().GetDescriptorIdentity()
			|| Plan->GetChangeSet().GetChanges().Num() != 1
			|| Plan->GetChangeSet().GetChanges()[0].GetIntent()
				!= EChangeIntent::Create
			|| Plan->GetChangeSet().GetChanges()[0]
				.GetIdentity().GetOwnerId() != Request.GetTargetRef())
		{
			SetError(OutError,
				TEXT("graph_compound_niagara_asset_plan_invalid"),
				Request.GetTargetRef());
			return false;
		}

		UObject* Source = TryResolveSource(Request, OutError);
		if (!Source)
		{
			return false;
		}
		const bool bSourceWasDirty = Source->GetOutermost()->IsDirty();
		FUeAgentCanonicalSnapshotPtr Current;
		if (!TryCaptureAssetCreateSnapshot(
				Profile, Request, Resources, Current, OutError))
		{
			return false;
		}
		FPlanningPreflightError RevalidateError;
		if (!TryRevalidatePlan(
				Plan, Current.ToSharedRef(), CallerContext,
				Resources, RevalidateError))
		{
			SetError(OutError, RevalidateError.GetCode(),
				RevalidateError.GetIdentityKey(),
				RevalidateError.GetDetail());
			return false;
		}

		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				Request.GetTargetRef(), EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				Resources, Prepared, DurableError)
			|| !Prepared.IsValid())
		{
			SetError(OutError, DurableError.GetCode(),
				Request.GetTargetRef(), DurableError.GetDetail());
			return false;
		}

		UPackage* Package = CreatePackage(*Request.GetTargetRef());
		UObject* CreatedAsset = Package
			? StaticDuplicateObject(
				Source, Package, *Request.GetAssetName(),
				RF_Public | RF_Standalone | RF_Transactional)
			: nullptr;
		FMaterialNiagaraGraphCompileSettleReceipt Settle;
		FMaterialNiagaraGraphAdapterError NativeError;
		const bool bCreated = CreatedAsset
			&& CreatedAsset->GetClass() == Source->GetClass()
			&& CreatedAsset->GetOutermost() == Package;
		const bool bSettled = bCreated
			&& NativeHook.TryCompileAndSettleNiagara(
				*CreatedAsset, Settle, NativeError)
			&& Settle.IsCompilationComplete()
			&& Settle.GetCompileErrorCount() == 0
			&& Settle.GetStackIssueCount() == 0;
		const bool bSourceUnchanged = Source->GetOutermost()->IsDirty()
			== bSourceWasDirty;
		if (!bCreated || !bSettled || !bSourceUnchanged)
		{
			CreatedAsset = nullptr;
			Package = nullptr;
			Source = nullptr;
			FString RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), false,
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError,
				!bSourceUnchanged
					? TEXT("graph_compound_niagara_source_mutated")
					: NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_niagara_asset_create_failed")
						: NativeError.GetCode(),
				Request.GetTargetRef(),
				NativeError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}

		CreatedAsset->SetFlags(RF_Public | RF_Standalone);
		CreatedAsset->PostEditChange();
		CreatedAsset->GetOutermost()->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(CreatedAsset);
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurability(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, DurableReceipt, DurableError))
		{
			CreatedAsset = nullptr;
			Package = nullptr;
			Source = nullptr;
			FString RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(),
				Request.GetTargetRef(),
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		CreatedAsset->GetOutermost()->SetDirtyFlag(false);
		CreatedAsset = nullptr;
		Package = nullptr;
		Source = nullptr;

		const FAssetDurableSessionRef Persisted =
			PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
		const FUeAgentCanonicalValuePtr& Expected =
			Plan->GetChangeSet().GetChanges()[0].GetAfter();
		TSharedPtr<FIndependentReadbackService> Readback;
		FVerificationError VerifyError;
		const TSharedRef<FNiagaraAssetReadbackOracle, ESPMode::ThreadSafe>
			Oracle = MakeShared<FNiagaraAssetReadbackOracle,
				ESPMode::ThreadSafe>(Profile, Request);
		FIndependentReadbackOutcome Outcome;
		if (!Expected.IsValid()
			|| !FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback.IsValid()
			|| !Readback->TryVerify(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraAssetReadbackOracleId,
					Request.GetTargetRef(), Expected.ToSharedRef()),
				Resources, Outcome, VerifyError)
			|| Outcome.GetStatus() != EVerificationStatus::Verified)
		{
			FString RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError,
				VerifyError.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_asset_readback_failed")
					: VerifyError.GetCode(),
				Request.GetTargetRef(),
				Outcome.GetDiagnosticCode() + TEXT(";") + RollbackDetail);
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				Resources, DurableError))
		{
			FString RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(),
				Request.GetTargetRef(),
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		return TrySerializeResult(
			Request.GetTargetRef(), Outcome.GetEvidenceId(),
			OutResult, OutError);
	}

	bool FGraphCompoundNiagaraLifecycleProduction::
	TryCaptureCompileSnapshot(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FString& TargetRef,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FGraphCompoundRuntimeError& OutError) const
	{
		OutSnapshot.Reset();
		OutError = {};
		UObject* const Target = TryResolveNiagaraAsset(
			TargetRef, OutError, false);
		if (!Target)
		{
			return false;
		}
		const FString Generation =
			TEXT("target:") + TargetRef + TEXT(":")
			+ NiagaraFingerprint(*Target);
		return TryBuildSnapshot(
			TEXT("graph-compound:c4:niagara-compile:") + TargetRef,
			TargetRef, Profile.GetTargetKind(), Generation,
			CompileSemanticId, TargetRef, TEXT("current"),
			Resources, OutSnapshot, OutError);
	}

	bool FGraphCompoundNiagaraLifecycleProduction::TryExecuteCompile(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FString& TargetRef,
		const FGraphCompoundCanonicalPayload& Payload,
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCallerContext& CallerContext,
		FUeAgentResourceTracker& Resources,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		FUeAgentContractValue& OutResult,
		FGraphCompoundRuntimeError& OutError)
	{
		OutResult = FUeAgentContractValue(TEXT("{}"));
		OutError = {};
		const bool bPayloadValid = Payload.GetEdits().Num() == 1
			&& Payload.GetEdits()[0].GetIntent() == EChangeIntent::Update
			&& Payload.GetEdits()[0].GetIdentity().GetSemanticId()
				== CompileSemanticId
			&& Payload.GetEdits()[0].GetIdentity().GetOwnerId() == TargetRef
			&& Payload.GetEdits()[0].GetProposedAfter().IsValid()
			&& Payload.GetEdits()[0].GetProposedAfter()->GetKind()
				== EUeAgentValueKind::String
			&& Payload.GetEdits()[0].GetProposedAfter()->GetText()
				== TEXT("requested");
		if (!IsInGameThread() || Cancellation->IsCancelled()
			|| !bPayloadValid
			|| Plan->GetDescriptorIdentity()
				!= Profile.GetExecutionContract().GetDescriptorIdentity()
			|| Plan->GetChangeSet().GetChanges().Num() != 1)
		{
			SetError(OutError,
				Cancellation->IsCancelled()
					? TEXT("graph_compound_niagara_compile_cancelled")
					: !IsInGameThread()
						? TEXT("graph_compound_niagara_compile_thread_invalid")
						: TEXT("graph_compound_niagara_compile_plan_invalid"),
				TargetRef);
			return false;
		}

		UObject* Target = TryResolveNiagaraAsset(TargetRef, OutError, false);
		if (!Target)
		{
			return false;
		}
		FUeAgentCanonicalSnapshotPtr Current;
		if (!TryCaptureCompileSnapshot(
				Profile, TargetRef, Resources, Current, OutError))
		{
			return false;
		}
		FPlanningPreflightError RevalidateError;
		if (!TryRevalidatePlan(
				Plan, Current.ToSharedRef(), CallerContext,
				Resources, RevalidateError))
		{
			SetError(OutError, RevalidateError.GetCode(),
				RevalidateError.GetIdentityKey(),
				RevalidateError.GetDetail());
			return false;
		}

		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				TargetRef, EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				Resources, Prepared, DurableError)
			|| !Prepared.IsValid())
		{
			SetError(OutError, DurableError.GetCode(),
				TargetRef, DurableError.GetDetail());
			return false;
		}

		FMaterialNiagaraGraphCompileSettleReceipt Settle;
		FMaterialNiagaraGraphAdapterError NativeError;
		if (!NativeHook.TryCompileAndSettleNiagara(
				*Target, Settle, NativeError)
			|| !Settle.IsCompilationComplete()
			|| Settle.GetCompileErrorCount() != 0
			|| Settle.GetStackIssueCount() != 0)
		{
			Target = nullptr;
			FString RollbackDetail;
			TryRollback(
				TargetRef, FPackageName::GetLongPackageAssetName(TargetRef),
				false, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, Resources, RollbackDetail);
			SetError(OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_compile_failed")
					: NativeError.GetCode(),
				TargetRef,
				NativeError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		Target->GetOutermost()->MarkPackageDirty();
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurability(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, DurableReceipt, DurableError))
		{
			Target = nullptr;
			FString RollbackDetail;
			TryRollback(
				TargetRef, FPackageName::GetLongPackageAssetName(TargetRef),
				false, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(), TargetRef,
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		Target->GetOutermost()->SetDirtyFlag(false);
		Target = nullptr;

		const FAssetDurableSessionRef Persisted =
			PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
		const FUeAgentCanonicalValuePtr& Expected =
			Plan->GetChangeSet().GetChanges()[0].GetAfter();
		TSharedPtr<FIndependentReadbackService> Readback;
		FVerificationError VerifyError;
		const TSharedRef<FNiagaraCompileReadbackOracle, ESPMode::ThreadSafe>
			Oracle = MakeShared<FNiagaraCompileReadbackOracle,
				ESPMode::ThreadSafe>(TargetRef);
		FIndependentReadbackOutcome Outcome;
		if (!Expected.IsValid()
			|| !FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback.IsValid()
			|| !Readback->TryVerify(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraCompileReadbackOracleId,
					TargetRef, Expected.ToSharedRef()),
				Resources, Outcome, VerifyError)
			|| Outcome.GetStatus() != EVerificationStatus::Verified)
		{
			FString RollbackDetail;
			TryRollback(
				TargetRef, FPackageName::GetLongPackageAssetName(TargetRef),
				false, Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError,
				VerifyError.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_compile_readback_failed")
					: VerifyError.GetCode(),
				TargetRef,
				Outcome.GetDiagnosticCode() + TEXT(";") + RollbackDetail);
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				Resources, DurableError))
		{
			FString RollbackDetail;
			TryRollback(
				TargetRef, FPackageName::GetLongPackageAssetName(TargetRef),
				false, Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(), TargetRef,
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		return TrySerializeResult(
			TargetRef, Outcome.GetEvidenceId(), OutResult, OutError);
	}
}
