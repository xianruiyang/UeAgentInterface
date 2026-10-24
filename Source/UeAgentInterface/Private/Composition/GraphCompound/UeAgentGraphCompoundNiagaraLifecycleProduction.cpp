#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraLifecycleProduction.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCanonical.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
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
        void SetError(FGraphCompoundRuntimeError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
            FString SubjectId = FString(), const UE::UAI::Contract::FDiagnostic& Recovery = UE::UAI::Contract::FDiagnostic())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = MoveTemp(SubjectId);
            OutError.Append(Recovery);
        }

		FDiagnostic NiagaraFailure(const FString& Code, const FString& Reason,
			const FString& Target, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Stage = TEXT("graph_compound"))
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.TargetPath = Target;
			Error.SubjectId = Target;
			Error.PropertyPath = Field;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			MarkGraphCompoundNativeDiagnostic(Error);
			return Error;
		}

		FDiagnostic DescriptorMismatch(const FString& Code, const FString& Target,
			const FUeAgentDescriptorIdentity& Actual, const FUeAgentDescriptorIdentity& Expected)
		{
			if (Actual.GetDescriptorId() != Expected.GetDescriptorId())
				return NiagaraFailure(Code, TEXT("The Niagara plan descriptor id does not match the route descriptor."),
					Target, TEXT("descriptor_identity.descriptor_id"),
					MakeShared<FJsonValueString>(Actual.GetDescriptorId()), MakeShared<FJsonValueString>(Expected.GetDescriptorId()));
			if (Actual.GetVersion() != Expected.GetVersion())
				return NiagaraFailure(Code, TEXT("The Niagara plan descriptor version does not match the route descriptor."),
					Target, TEXT("descriptor_identity.version"),
					MakeShared<FJsonValueNumber>(Actual.GetVersion()), MakeShared<FJsonValueNumber>(Expected.GetVersion()));
			FDiagnostic Error = NiagaraFailure(Code, TEXT("The Niagara plan descriptor hash does not match the route descriptor."),
				Target, TEXT("descriptor_identity.descriptor_hash"), nullptr, nullptr);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetDescriptorHash());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			return Error;
		}

		FDiagnostic SettleFailure(const FString& Target, const FMaterialNiagaraGraphCompileSettleReceipt& Settle)
		{
			if (!Settle.IsCompilationComplete())
				return NiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
					TEXT("Niagara compilation did not complete."), Target, TEXT("compilation_complete"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("compile"));
			if (Settle.GetCompileErrorCount() != 0)
				return NiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
					TEXT("Niagara compilation reported errors."), Target, TEXT("compile_error_count"),
					MakeShared<FJsonValueNumber>(Settle.GetCompileErrorCount()), MakeShared<FJsonValueNumber>(0), TEXT("compile"));
			return NiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
				TEXT("Niagara stack validation reported blocking issues."), Target, TEXT("stack_issue_count"),
				MakeShared<FJsonValueNumber>(Settle.GetStackIssueCount()), MakeShared<FJsonValueNumber>(0), TEXT("compile"));
		}

		const FString NiagaraAssetReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_asset_lifecycle");
		const FString NiagaraCompileReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_compile_settle");
		const FString CompileSemanticId =
			TEXT("property:lifecycle:compile_refresh_settle");

		

        

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
			const bool bValidPackage = FPackageName::IsValidLongPackageName(PackageName);
			UObject* const Asset = bValidPackage ? LoadObject<UObject>(nullptr, *ObjectPath) : nullptr;
			const FString Code = bSource ? TEXT("graph_compound_niagara_source_invalid") : TEXT("graph_compound_niagara_target_invalid");
			if (!bValidPackage)
			{
				OutError = NiagaraFailure(Code, TEXT("The Niagara reference does not contain a valid long package name."),
					Ref, TEXT("package_name"), MakeShared<FJsonValueString>(PackageName),
					MakeShared<FJsonValueString>(TEXT("valid long package name")));
				return nullptr;
			}
			if (!Asset)
			{
				OutError = NiagaraFailure(Code, TEXT("The referenced Niagara object could not be loaded."),
					Ref, TEXT("object_path"), MakeShared<FJsonValueString>(ObjectPath),
					MakeShared<FJsonValueString>(TEXT("loadable Niagara emitter, system or script")));
				return nullptr;
			}
			if (!IsNiagaraAsset(Asset))
			{
				OutError = NiagaraFailure(Code, TEXT("The loaded object is not a Niagara emitter, system or script."),
					Ref, TEXT("asset.class"), MakeShared<FJsonValueString>(Asset->GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(TEXT("NiagaraEmitter, NiagaraSystem or NiagaraScript")));
				return nullptr;
			}
			if (Asset->GetOutermost()->GetName() != PackageName)
			{
				OutError = NiagaraFailure(Code, TEXT("The loaded Niagara object's package does not match the reference."),
					Ref, TEXT("asset.package"), MakeShared<FJsonValueString>(Asset->GetOutermost()->GetName()),
					MakeShared<FJsonValueString>(PackageName));
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
			if (!Source) return nullptr;
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
				const bool bEmitter = Request.GetInputKind() == EGraphCompoundTypedInputKind::NiagaraEmitterAssetCreate;
				const bool bSystem = Request.GetInputKind() == EGraphCompoundTypedInputKind::NiagaraSystemAssetCreate;
				OutError = bEmitter || bSystem
					? NiagaraFailure(TEXT("graph_compound_niagara_source_invalid"),
						TEXT("The Niagara template class does not match the asset creation kind."), Request.GetSourceRef(),
						TEXT("source.class"), MakeShared<FJsonValueString>(Source->GetClass()->GetPathName()),
						MakeShared<FJsonValueString>((bEmitter ? UNiagaraEmitter::StaticClass() : UNiagaraSystem::StaticClass())->GetPathName()))
					: NiagaraFailure(TEXT("graph_compound_niagara_source_invalid"),
						TEXT("The input kind does not support Niagara asset creation or duplication."), Request.GetSourceRef(),
						TEXT("input_kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetInputKind())),
						MakeShared<FJsonValueString>(TEXT("NiagaraEmitterAssetCreate, NiagaraSystemAssetCreate or NiagaraAssetDuplicate")));
				return nullptr;
			}
			return Source;
		}

		bool TryPackageFilename(const FString& PackageName, FString& OutFilename, FGraphCompoundRuntimeError& OutError)
		{
			OutFilename.Reset();
			if (!FPackageName::IsValidLongPackageName(PackageName))
			{
				OutError = NiagaraFailure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The target package name is invalid."), PackageName, TEXT("package_name"),
					MakeShared<FJsonValueString>(PackageName), MakeShared<FJsonValueString>(TEXT("valid long package name")));
				return false;
			}
			if (FPackageName::GetPackageMountPoint(PackageName).IsNone())
			{
				OutError = NiagaraFailure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The target package has no registered mount point."), PackageName, TEXT("package.mount_point"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("registered mount point")));
				return false;
			}
			if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFilename, FPackageName::GetAssetPackageExtension()))
			{
				OutError = NiagaraFailure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The mounted target package could not be converted to an asset filename."), PackageName, TEXT("package.filename_conversion.succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return true;
		}

		bool TryCheckTargetAbsent(
			const FGraphCompoundAssetCreateRequest& Request,
			FString& OutFilename,
			FGraphCompoundRuntimeError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				return false;
			}
			if (!TryPackageFilename(Request.GetTargetRef(), OutFilename, OutError)) return false;
			bool bTargetPresent = false;
			if (!HjsonWorkspace::DomainAuthoring::
				TryInspectObjectTargetPresence(
					Request.GetTargetRef(), bTargetPresent))
			{
				OutError = NiagaraFailure(TEXT("graph_compound_asset_target_invalid"),
					TEXT("The target reference cannot be resolved to a package identity for presence inspection."),
					Request.GetTargetRef(), TEXT("target_ref"), MakeShared<FJsonValueString>(Request.GetTargetRef()),
					MakeShared<FJsonValueString>(TEXT("valid object or package reference")));
				return false;
			}
			if (bTargetPresent)
			{
				OutError = NiagaraFailure(TEXT("graph_compound_asset_target_already_exists"),
					TEXT("target_ref already resolves to an existing asset."), Request.GetTargetRef(),
					TEXT("target.present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
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
			if (!FUeAgentCanonicalValue::TryString(Text, Resources, OutValue, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Subject;
				SetError(OutError, Error, Subject);
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
				SetError(OutError, Entry.GetValidationDiagnostic(), SemanticId);
				MarkGraphCompoundNativeDiagnostic(OutError);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = SubjectRef;
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
					OutSnapshot, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = SubjectRef;
				SetError(OutError, Error, SubjectRef);
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
			UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			return UeAgentAuthoringOperationApply::TryRestorePackageDurability(
				PackageName, bNotifyAssetRegistry, Session, Backend, FencingToken,
				Resources, OutDetail);
		}

		bool TryValidateCompileReadback(
			UObject& Asset,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			const UeAgentNiagaraCompileValidation::FResult Compile =
				UeAgentNiagaraCompileValidation::InspectAsset(&Asset);
			if (!Compile.bCompilationComplete || Compile.bHasError
				|| Compile.ScriptCount <= 0)
			{
				OutDiagnostic = Compile.GetFailure(Asset.GetPathName());
				if (OutDiagnostic.IsEmpty())
					OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_compile_readback_failed"),
						TEXT("Niagara compilation produced no scripts for independent readback."), Asset.GetPathName(),
						TEXT("script_count"), MakeShared<FJsonValueNumber>(Compile.ScriptCount),
						MakeShared<FJsonValueString>(TEXT("positive script count")), TEXT("native_readback"));
				return false;
			}
			if (UNiagaraSystem* const System = Cast<UNiagaraSystem>(&Asset))
			{
				const bool bOutstanding = System->HasOutstandingCompilationRequests(true);
				const bool bNeedsCompile = !bOutstanding && System->NeedsRequestCompile();
				if (bOutstanding || bNeedsCompile)
				{
					OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_compile_not_settled"),
						bOutstanding ? TEXT("The Niagara system still has outstanding compilation requests.")
							: TEXT("The Niagara system still requires a compile request."),
						Asset.GetPathName(), bOutstanding ? TEXT("compilation.outstanding_requests") : TEXT("compilation.needs_request"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("native_readback"));
					return false;
				}
			}
			return true;
		}

		bool TryValidateReloadedAsset(
			const FGraphCompoundAssetCreateRequest& Request,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic)
		{
			OutDiagnostic.Reset();
			FGraphCompoundRuntimeError ResolveError;
			UObject* const Source = TryResolveSource(Request, ResolveError);
			UObject* const Target = LoadObject<UObject>(
				nullptr, *ObjectPathFromRef(Request.GetTargetRef()));
			if (!Source || !Target)
			{
				OutDiagnostic = Source ? NiagaraFailure(TEXT("graph_compound_niagara_reload_failed"),
					TEXT("The persisted Niagara asset could not be loaded."), Request.GetTargetRef(),
					TEXT("object_path"), MakeShared<FJsonValueString>(ObjectPathFromRef(Request.GetTargetRef())),
					MakeShared<FJsonValueString>(TEXT("loadable persisted Niagara asset")), TEXT("native_readback")) : ResolveError;
				return false;
			}
			auto IdentityFailure = [&](const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_reload_identity_mismatch"),
					Reason, Request.GetTargetRef(), Field, MoveTemp(Actual), MoveTemp(Expected), TEXT("native_readback"));
				return false;
			};
			if (Target->GetClass() != Source->GetClass())
				return IdentityFailure(TEXT("The reloaded Niagara asset class differs from the template class."),
					TEXT("asset.class"), MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()),
					MakeShared<FJsonValueString>(Source->GetClass()->GetPathName()));
			if (Target->GetOutermost()->GetName() != Request.GetTargetRef())
				return IdentityFailure(TEXT("The reloaded Niagara asset belongs to a different package."),
					TEXT("asset.package"), MakeShared<FJsonValueString>(Target->GetOutermost()->GetName()),
					MakeShared<FJsonValueString>(Request.GetTargetRef()));
			if (Target->GetPathName() == Source->GetPathName())
				return IdentityFailure(TEXT("The reloaded Niagara asset resolves to the source template instead of a distinct asset."),
					TEXT("asset.object_path"), MakeShared<FJsonValueString>(Target->GetPathName()),
					MakeShared<FJsonValueString>(ObjectPathFromRef(Request.GetTargetRef())));
			if (Target->GetOutermost()->IsDirty())
				return IdentityFailure(TEXT("The reloaded Niagara package is dirty."),
					TEXT("package.dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			if (const UNiagaraSystem* const SourceSystem =
				Cast<UNiagaraSystem>(Source))
			{
				const UNiagaraSystem* const TargetSystem =
					CastChecked<UNiagaraSystem>(Target);
				if (!FMath::IsNearlyEqual(
						SourceSystem->GetWarmupTime(),
						TargetSystem->GetWarmupTime()))
				{
					OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_template_state_mismatch"),
						TEXT("The reloaded Niagara system warmup time differs from the source beyond the native comparison tolerance."),
						Request.GetTargetRef(), TEXT("system.warmup_time"),
						MakeShared<FJsonValueString>(::LexToString(TargetSystem->GetWarmupTime())),
						MakeShared<FJsonValueString>(::LexToString(SourceSystem->GetWarmupTime())), TEXT("native_readback"));
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
				if (!SourceData || !TargetData)
				{
					OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_template_state_mismatch"),
						!SourceData ? TEXT("The source Niagara emitter has no latest version data.")
							: TEXT("The reloaded Niagara emitter has no latest version data."),
						!SourceData ? Request.GetSourceRef() : Request.GetTargetRef(),
						TEXT("emitter.latest_version.present"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), TEXT("native_readback"));
					return false;
				}
				if (SourceData->bLocalSpace != TargetData->bLocalSpace)
				{
					OutDiagnostic = NiagaraFailure(TEXT("graph_compound_niagara_template_state_mismatch"),
						TEXT("The reloaded Niagara emitter local-space setting differs from its source."),
						Request.GetTargetRef(), TEXT("emitter.local_space"),
						MakeShared<FJsonValueBoolean>(TargetData->bLocalSpace),
						MakeShared<FJsonValueBoolean>(SourceData->bLocalSpace), TEXT("native_readback"));
					return false;
				}
			}
			return TryValidateCompileReadback(*Target, OutDiagnostic);
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				MarkGraphCompoundNativeDiagnostic(Error);
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = TargetRef;
				SetError(OutError, Error, TargetRef);
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
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(Request.GetTargetRef(), Detail))
				{
					OutError = Detail.IsEmpty() ? NiagaraFailure(TEXT("graph_compound_niagara_unload_failed"),
						TEXT("The persisted Niagara package could not be reloaded for independent readback."),
						Request.GetTargetRef(), TEXT("package.reload.succeeded"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), TEXT("verification")) : Detail;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetTargetRef();
					return false;
				}
				if (!LoadObject<UObject>(nullptr, *ObjectPathFromRef(Request.GetTargetRef())))
				{
					OutError = NiagaraFailure(TEXT("graph_compound_niagara_reload_failed"),
						TEXT("The persisted Niagara asset could not be loaded after package reload."),
						Request.GetTargetRef(), TEXT("object_path"), MakeShared<FJsonValueString>(ObjectPathFromRef(Request.GetTargetRef())),
						MakeShared<FJsonValueString>(TEXT("loadable persisted Niagara asset")), TEXT("verification"));
					return false;
				}
				UE::UAI::Contract::FDiagnostic Diagnostic;
				if (!TryValidateReloadedAsset(Request, Diagnostic))
				{
					OutObservation =
						FIndependentReadbackObservation::Unavailable(Diagnostic, TEXT("reload:")
								+ Request.GetTargetRef());
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryStringValue(
						ExpectedDefinition(Profile, Request), Resources,
						Actual, Error, Request.GetTargetRef()))
				{
					OutError = Error;
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
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(TargetRef, Detail))
				{
					OutError = Detail.IsEmpty() ? NiagaraFailure(TEXT("graph_compound_niagara_unload_failed"),
						TEXT("The persisted Niagara package could not be reloaded for independent readback."),
						TargetRef, TEXT("package.reload.succeeded"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), TEXT("verification")) : Detail;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
					return false;
				}
				FGraphCompoundRuntimeError ResolveError;
				UObject* const Target = TryResolveNiagaraAsset(
					TargetRef, ResolveError, false);
				if (!Target)
				{
					OutError = ResolveError;
					return false;
				}
				UE::UAI::Contract::FDiagnostic Diagnostic;
				const bool bDirty = Target->GetOutermost()->IsDirty();
				if (bDirty)
					Diagnostic = NiagaraFailure(TEXT("graph_compound_niagara_readback_dirty"),
						TEXT("The reloaded Niagara package is dirty, so persisted state cannot be verified."),
						TargetRef, TEXT("package.dirty"), MakeShared<FJsonValueBoolean>(true),
						MakeShared<FJsonValueBoolean>(false), TEXT("independent_readback"));
				if (bDirty || !TryValidateCompileReadback(*Target, Diagnostic))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(Diagnostic, TEXT("reload:") + TargetRef);
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryStringValue(
						TEXT("requested"), Resources, Actual,
						Error, TargetRef))
				{
					OutError = Error;
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
			const bool bCancelled = Cancellation->IsCancelled();
			OutError = NiagaraFailure(bCancelled ? TEXT("graph_compound_niagara_asset_cancelled") : TEXT("graph_compound_niagara_asset_thread_invalid"),
				bCancelled ? TEXT("Niagara asset creation was cancelled before execution.") : TEXT("Niagara asset creation must run on the game thread."),
				Request.GetTargetRef(), bCancelled ? TEXT("cancellation.requested") : TEXT("execution.game_thread"),
				MakeShared<FJsonValueBoolean>(bCancelled), MakeShared<FJsonValueBoolean>(!bCancelled));
			return false;
		}
		if (Plan->GetDescriptorIdentity() != Profile.GetExecutionContract().GetDescriptorIdentity())
		{
			OutError = DescriptorMismatch(TEXT("graph_compound_niagara_asset_plan_invalid"), Request.GetTargetRef(),
				Plan->GetDescriptorIdentity(), Profile.GetExecutionContract().GetDescriptorIdentity());
			return false;
		}
		if (Plan->GetChangeSet().GetChanges().Num() != 1)
		{
			OutError = NiagaraFailure(TEXT("graph_compound_niagara_asset_plan_invalid"),
				TEXT("Niagara asset creation requires exactly one planned change."), Request.GetTargetRef(), TEXT("changes.count"),
				MakeShared<FJsonValueNumber>(Plan->GetChangeSet().GetChanges().Num()), MakeShared<FJsonValueNumber>(1));
			return false;
		}
		const auto& CreateChange = Plan->GetChangeSet().GetChanges()[0];
		if (CreateChange.GetIntent() != EChangeIntent::Create)
		{
			OutError = NiagaraFailure(TEXT("graph_compound_niagara_asset_plan_invalid"),
				TEXT("The Niagara asset plan change must have create intent."), Request.GetTargetRef(), TEXT("changes[0].intent"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(CreateChange.GetIntent())),
				MakeShared<FJsonValueNumber>(static_cast<int32>(EChangeIntent::Create)));
			OutError.OperationIndex = 0;
			return false;
		}
		if (CreateChange.GetIdentity().GetOwnerId() != Request.GetTargetRef())
		{
			OutError = NiagaraFailure(TEXT("graph_compound_niagara_asset_plan_invalid"),
				TEXT("The Niagara asset plan change owner differs from the target."), Request.GetTargetRef(), TEXT("changes[0].owner"),
				MakeShared<FJsonValueString>(CreateChange.GetIdentity().GetOwnerId()), MakeShared<FJsonValueString>(Request.GetTargetRef()));
			OutError.OperationIndex = 0;
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
			OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
			return false;
		}

		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				Request.GetTargetRef(), EPackageHeaderKind::Asset,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				Resources, Prepared, DurableError))
		{
			OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetTargetRef();
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
		const bool bCompiled = bCreated && NativeHook.TryCompileAndSettleNiagara(*CreatedAsset, Settle, NativeError);
		const bool bSettled = bCompiled
			&& Settle.IsCompilationComplete()
			&& Settle.GetCompileErrorCount() == 0
			&& Settle.GetStackIssueCount() == 0;
		const bool bSourceUnchanged = Source->GetOutermost()->IsDirty()
			== bSourceWasDirty;
		if (!bCreated || !bSettled || !bSourceUnchanged)
		{
			if (NativeError.IsEmpty())
			{
				if (!Package)
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_create_failed"),
						TEXT("The target Niagara package could not be created."), Request.GetTargetRef(),
						TEXT("package.created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else if (!CreatedAsset)
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_create_failed"),
						TEXT("Duplicating the source Niagara asset into the target package failed."), Request.GetTargetRef(),
						TEXT("source_ref"), MakeShared<FJsonValueString>(Request.GetSourceRef()),
						MakeShared<FJsonValueString>(TEXT("source successfully duplicated into target")));
				else if (CreatedAsset->GetClass() != Source->GetClass())
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_create_failed"),
						TEXT("The duplicated Niagara asset class differs from its source."), Request.GetTargetRef(),
						TEXT("asset.class"), MakeShared<FJsonValueString>(CreatedAsset->GetClass()->GetPathName()),
						MakeShared<FJsonValueString>(Source->GetClass()->GetPathName()));
				else if (CreatedAsset->GetOutermost() != Package)
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_create_failed"),
						TEXT("The duplicated Niagara asset belongs to a different package."), Request.GetTargetRef(),
						TEXT("asset.package"), MakeShared<FJsonValueString>(CreatedAsset->GetOutermost()->GetName()),
						MakeShared<FJsonValueString>(Package->GetName()));
				else if (!bCompiled)
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_create_failed"),
						TEXT("Native compilation of the duplicated Niagara asset failed."), Request.GetTargetRef(),
						TEXT("compilation.succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("compile"));
				else if (!bSettled)
					NativeError = SettleFailure(Request.GetTargetRef(), Settle);
				else
					NativeError = NiagaraFailure(TEXT("graph_compound_niagara_asset_source_changed"),
						TEXT("Creating the Niagara asset changed the source template package's dirty state."), Request.GetSourceRef(),
						TEXT("source.package.dirty"), MakeShared<FJsonValueBoolean>(Source->GetOutermost()->IsDirty()),
						MakeShared<FJsonValueBoolean>(bSourceWasDirty));
			}
			if (Package) NativeError.MutationMayHaveStarted = true;
			CreatedAsset = nullptr;
			Package = nullptr;
			Source = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), false,
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, RollbackDetail);
			if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = Request.GetTargetRef();
			SetError(OutError, NativeError, Request.GetTargetRef(), RollbackDetail);
			return false;
		}

		CreatedAsset->SetFlags(RF_Public | RF_Standalone);
		CreatedAsset->PostEditChange();
		CreatedAsset->GetOutermost()->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(CreatedAsset);
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurabilityForVerification(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, DurableReceipt, DurableError))
		{
			CreatedAsset = nullptr;
			Package = nullptr;
			Source = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
			OutError.MutationMayHaveStarted = true;
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
		if (!Expected.IsValid())
			VerifyError = NiagaraFailure(TEXT("graph_compound_niagara_asset_readback_failed"),
				TEXT("The planned Niagara change has no expected value for independent readback."), Request.GetTargetRef(),
				TEXT("changes[0].after.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("verification"));
		if (!Expected.IsValid()
			|| !FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback->TryObserve(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraAssetReadbackOracleId,
					Request.GetTargetRef(), Expected.ToSharedRef()),
				Resources, Outcome, VerifyError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, Outcome.GetFailureDiagnostic(VerifyError), Request.GetTargetRef(), RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				Resources, DurableError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				Request.GetTargetRef(), Request.GetAssetName(), true,
				Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError, Request.GetTargetRef(), RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}
		return TrySerializeResult(
			Request.GetTargetRef(),
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
			const bool bCancelled = Cancellation->IsCancelled();
			auto Fail = [&](const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = NiagaraFailure(bCancelled ? TEXT("graph_compound_niagara_compile_cancelled")
					: !IsInGameThread() ? TEXT("graph_compound_niagara_compile_thread_invalid")
					: TEXT("graph_compound_niagara_compile_plan_invalid"), Reason, TargetRef, Field, MoveTemp(Actual), MoveTemp(Expected));
				return false;
			};
			if (bCancelled)
				return Fail(TEXT("The Niagara compile operation was cancelled before execution."), TEXT("cancellation.requested"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			if (!IsInGameThread())
				return Fail(TEXT("Niagara compilation must run on the game thread."), TEXT("execution.game_thread"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			if (!bPayloadValid)
			{
				if (Payload.GetEdits().Num() != 1)
					return Fail(TEXT("Niagara compilation requires exactly one payload edit."), TEXT("edits.count"),
						MakeShared<FJsonValueNumber>(Payload.GetEdits().Num()), MakeShared<FJsonValueNumber>(1));
				const auto& Edit = Payload.GetEdits()[0];
				if (Edit.GetIntent() != EChangeIntent::Update)
					return Fail(TEXT("The Niagara compile edit must have update intent."), TEXT("edits[0].intent"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetIntent())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EChangeIntent::Update)));
				if (Edit.GetIdentity().GetSemanticId() != CompileSemanticId)
				{
					Fail(TEXT("The edit does not address the Niagara compile lifecycle operation."), TEXT("edits[0].semantic_id"), nullptr, nullptr);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Edit.GetIdentity().GetSemanticId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CompileSemanticId);
					return false;
				}
				if (Edit.GetIdentity().GetOwnerId() != TargetRef)
					return Fail(TEXT("The Niagara compile edit owner differs from the target."), TEXT("edits[0].owner"),
						MakeShared<FJsonValueString>(Edit.GetIdentity().GetOwnerId()), MakeShared<FJsonValueString>(TargetRef));
				if (!Edit.GetProposedAfter().IsValid())
					return Fail(TEXT("The Niagara compile edit has no proposed value."), TEXT("edits[0].after.present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (Edit.GetProposedAfter()->GetKind() != EUeAgentValueKind::String)
					return Fail(TEXT("The Niagara compile proposed value must be a string."), TEXT("edits[0].after.kind"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Edit.GetProposedAfter()->GetKind())),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentValueKind::String)));
				return Fail(TEXT("The Niagara compile proposed value must request compilation."), TEXT("edits[0].after"),
					MakeShared<FJsonValueString>(Edit.GetProposedAfter()->GetText()), MakeShared<FJsonValueString>(TEXT("requested")));
			}
			if (Plan->GetDescriptorIdentity() != Profile.GetExecutionContract().GetDescriptorIdentity())
			{
				OutError = DescriptorMismatch(TEXT("graph_compound_niagara_compile_plan_invalid"), TargetRef,
					Plan->GetDescriptorIdentity(), Profile.GetExecutionContract().GetDescriptorIdentity());
				return false;
			}
			return Fail(TEXT("Niagara compilation requires exactly one planned change."), TEXT("changes.count"),
				MakeShared<FJsonValueNumber>(Plan->GetChangeSet().GetChanges().Num()), MakeShared<FJsonValueNumber>(1));
		}

		UObject* Target = TryResolveNiagaraAsset(TargetRef, OutError, false);
		if (!Target)
		{
			return false;
		}
		const FString TargetPackageName = Target->GetOutermost()->GetName();
		const FString TargetAssetName = Target->GetName();
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
			OutError = RevalidateError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = RevalidateError.SubjectId;
			return false;
		}

		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				TargetPackageName, EPackageHeaderKind::Asset,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				Resources, Prepared, DurableError))
		{
			OutError = DurableError;
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = TargetRef;
			return false;
		}

		FMaterialNiagaraGraphCompileSettleReceipt Settle;
		FMaterialNiagaraGraphAdapterError NativeError;
		const bool bCompiled = NativeHook.TryCompileAndSettleNiagara(*Target, Settle, NativeError);
		if (!bCompiled
			|| !Settle.IsCompilationComplete()
			|| Settle.GetCompileErrorCount() != 0
			|| Settle.GetStackIssueCount() != 0)
		{
			if (NativeError.IsEmpty())
				NativeError = bCompiled ? SettleFailure(TargetRef, Settle)
					: NiagaraFailure(TEXT("graph_compound_niagara_compile_failed"),
						TEXT("Native Niagara compilation failed."), TargetRef, TEXT("compilation.succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("compile"));
			NativeError.MutationMayHaveStarted = true;
			Target = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				TargetPackageName, TargetAssetName,
				false, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, Resources, RollbackDetail);
			if (NativeError.TargetPath.IsEmpty()) NativeError.TargetPath = TargetRef;
			SetError(OutError, NativeError, TargetRef, RollbackDetail);
			return false;
		}
		Target->GetOutermost()->MarkPackageDirty();
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurabilityForVerification(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				Resources, DurableReceipt, DurableError))
		{
			Target = nullptr;
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				TargetPackageName, TargetAssetName,
				false, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, Resources, RollbackDetail);
			SetError(OutError, DurableError, TargetRef, RollbackDetail);
			OutError.MutationMayHaveStarted = true;
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
		if (!Expected.IsValid())
			VerifyError = NiagaraFailure(TEXT("graph_compound_niagara_compile_readback_failed"),
				TEXT("The planned Niagara change has no expected value for independent readback."), TargetRef,
				TEXT("changes[0].after.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("verification"));
		if (!Expected.IsValid()
			|| !FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback->TryObserve(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraCompileReadbackOracleId,
					TargetRef, Expected.ToSharedRef()),
				Resources, Outcome, VerifyError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				TargetPackageName, TargetAssetName,
				false, Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, Outcome.GetFailureDiagnostic(VerifyError), TargetRef, RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken, Outcome.GetStatus() == EVerificationStatus::Verified);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				Resources, DurableError))
		{
			UE::UAI::Contract::FDiagnostic RollbackDetail;
			TryRollback(
				TargetPackageName, TargetAssetName,
				false, Persisted, DurableBackend, FencingToken,
				Resources, RollbackDetail);
			SetError(OutError, DurableError, TargetRef, RollbackDetail);
			OutError.MutationMayHaveStarted = true;
			return false;
		}
		return TrySerializeResult(
			TargetRef, OutResult, OutError);
	}
}
