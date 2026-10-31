#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "EditorReimportHandler.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "HAL/ThreadSafeCounter.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		FDiagnostic OperationFact(const FString& Code, const FString& Reason, const FString& Target,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Name = FString(), const FString& Kind = TEXT("native_result"))
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("asset_operation"));
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Path;
			Diagnostic.CandidateName = Name;
			Diagnostic.SourceKind = Kind;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FDiagnostic WithOperationTarget(FDiagnostic Diagnostic, const FString& Target)
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		void AppendOperationRollback(FDiagnostic& Diagnostic, const bool bRestored, const FDiagnostic& RollbackError)
		{
			if (!Diagnostic.RollbackAttempted.IsSet()) Diagnostic.RollbackAttempted = true;
			if (!Diagnostic.RollbackSucceeded.IsSet()) Diagnostic.RollbackSucceeded = bRestored;
			Diagnostic.Append(RollbackError);
		}

		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker();
		}

		bool TryPackageName(FString Ref, FString& OutPackageName)
		{
			Ref.TrimStartAndEndInline();
			OutPackageName = Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: MoveTemp(Ref);
			return OutPackageName.StartsWith(TEXT("/Game/"))
				&& FPackageName::IsValidLongPackageName(OutPackageName, true);
		}

		FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		UObject* ReloadForIndependentReadback(const FString& PackageName, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(PackageName, Error))
			{
				// This owner supplies a validated authored package reference. The
				// shared readback helper also serves private transaction packages.
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackageName;
				return nullptr;
			}
			UObject* Object = FindObject<UObject>(nullptr, *ObjectPath(PackageName));
			if (!Object)
				Error = OperationFact(TEXT("asset_operation_readback_object_missing"), TEXT("The persisted package reloaded but contains no asset at its expected object path."),
					PackageName, TEXT("/readback/object/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return Object;
		}

		FUeAgentCanonicalIdentity IdentityFor(
			const FString& PackageName,
			const FString& Selector)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.asset.") + Selector,
				TEXT("asset"),
				PackageName,
				Selector,
				TEXT("asset"),
				TEXT("asset-destructive-v1"));
		}

		FPlanningPreflightPlanRef MakePlan(
			const FUeAgentAssetOperationRequest& Request,
			const FString& PackageName,
			const FString& OwnerId,
			const FUeAgentCanonicalIdentity& Identity,
			const EChangeIntent Intent)
		{
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				Identity, Intent, FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr(),
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("asset-destructive:direct"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				Identity, Identity, TEXT("direct"),
				TEXT("asset-destructive:direct"));
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.asset.destructive/v1"), 1,
					Json::Sha256Utf8(TEXT("uai.asset.destructive/v1"))),
				Request.RequestId,
				TEXT("asset-destructive:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("asset-destructive:preflight:") + Request.RequestId,
				FUeAgentStateRevision(OwnerId, TEXT("asset-destructive-v1"), 1),
				TEXT("asset-destructive:preflight:native"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Json::Sha256Utf8(
					Request.CommandId + TEXT(":") + Request.RequestId
					+ TEXT(":") + PackageName));
		}

		class FDurableMutation final
		{
		public:
			bool Prepare(
				const FUeAgentAssetOperationRequest& Request,
				const FString& PackageName,
				const FString& OwnerId,
				const FString& JournalRoot,
				const EAssetDurableIntent DurableIntent,
				const int64 FencingToken,
				UE::UAI::Contract::FDiagnostic& OutError,
				const TArray<TPair<FString, EPackageHeaderKind>>& AdditionalPackages = {})
			{
				Identity = IdentityFor(PackageName, TEXT("state"));
				Token = FencingToken;
				Plan = MakePlan(
					Request, PackageName, OwnerId, Identity,
					DurableIntent == EAssetDurableIntent::Delete
						? EChangeIntent::Delete : EChangeIntent::Update);
				TArray<FAssetDurableTarget> Targets;
				Targets.Emplace(Identity, PackageName, EPackageHeaderKind::EvidenceRequired,
					DurableIntent, ERollbackGrade::DurableBackup);
				for (const auto& Package : AdditionalPackages)
					Targets.Emplace(Identity, Package.Key, Package.Value, DurableIntent, ERollbackGrade::DurableBackup);
				const FAssetDurableRequest DurableRequest(
					Plan.ToSharedRef(), MoveTemp(Targets),
					Token, OwnerId, EAtomicityMode::CompensatedSaga);
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot,
						Resources, Session, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Persist(UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurabilityForVerification(
						Session.ToSharedRef(), Backend, Token,
						Resources, Receipt, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Acknowledge(
				const FString& VerificationId,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(), Session->GetInventory(),
						Session->GetJournalId(), Session->GetJournalPath(),
						Session->GetJournalRoot(), Session->GetTransactionRoot(),
						Session->GetStagingRoot(), Receipt.GetPhaseHistory(),
						Receipt.GetPreMutationStates(), Receipt.GetExpectedPersistedStates());
				const FDurableVerificationAcknowledgement Ack(
					Receipt.GetJournalId(), Receipt.GetPlanFingerprint(),
					Receipt.GetInventoryHash(), VerificationId,
					Session->GetRequest().GetJournalOwnerId(), Token);
				FAssetDurableError Error;
				if (!TryAcknowledgeAssetDurability(
						Persisted, Ack, Token, Resources, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool PersistNative(TFunctionRef<bool(FAssetDurableError&)> Operation, UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistNativeAssetDurabilityForVerification(Session, Backend, Token,
					Operation, Resources, Receipt, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Rollback(const bool bMemoryConfirmed, FDiagnostic& OutError)
			{
				FDurableReceipt RollbackReceipt;
				if (!TryRollbackAssetDurability(Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, OutError)) return false;
				if (RollbackReceipt.GetStatus() == EDurableStatus::RolledBack) return true;
				OutError = RollbackReceipt.GetFailureDiagnostic();
				return false;
			}

			FString JournalId() const
			{
				return Session.IsValid() ? Session->GetJournalId() : FString();
			}

		private:
			FUeAgentResourceTracker Resources = MakeTracker();
			FPackageAssetDurableBackend Backend;
			FUeAgentCanonicalIdentity Identity = IdentityFor(
				TEXT("/Game/Invalid"), TEXT("state"));
			FPlanningPreflightPlanPtr Plan;
			FAssetDurableSessionPtr Session;
			FDurableReceipt Receipt;
			int64 Token = -1;
		};

		class FAssetOperationsOwner final : public IUeAgentAssetOperationsOwner
		{
		public:
			FAssetOperationsOwner(FString InJournalRoot, FString InOwnerId)
				: JournalRoot(MoveTemp(InJournalRoot)), OwnerId(MoveTemp(InOwnerId))
			{
			}

			FUeAgentAssetOperationResult Execute(
				const FUeAgentAssetOperationRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				auto Fail = [&Result, &Request](FDiagnostic Diagnostic)
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Diagnostic), Request.TargetRef);
					return Result;
				};
				if (!IsInGameThread())
				{
					return Fail(OperationFact(TEXT("asset_operation_game_thread_required"), TEXT("Native asset operations require the game thread."),
						Request.TargetRef, TEXT("/execution/on_game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.CommandId));
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = OperationFact(TEXT("asset_operation_cancelled"), TEXT("The asset operation was cancelled before execution."),
						Request.TargetRef, TEXT("/execution/cancel_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					Diagnostic.SubjectId = Request.RequestId;
					return Fail(MoveTemp(Diagnostic));
				}
				if (Request.CommandId == TEXT("asset.move"))
				{
					return Move(Request);
				}
				if (Request.CommandId == TEXT("asset.fixup_redirectors"))
				{
					return Fixup(Request);
				}
				if (Request.CommandId.StartsWith(TEXT("asset.reimport_")))
				{
					return Reimport(Request);
				}
				if (Request.CommandId.StartsWith(TEXT("skeletal_mesh.remove_")))
				{
					return RemoveSkeletalItem(Request);
				}
				return Fail(OperationFact(TEXT("asset_operation_unknown"), TEXT("The asset operation owner does not support this command."),
					Request.TargetRef, TEXT("/command"), MakeShared<FJsonValueString>(Request.CommandId),
					MakeShared<FJsonValueString>(TEXT("asset.move | asset.fixup_redirectors | asset.reimport_* | skeletal_mesh.remove_*")), FString(), TEXT("command_input")));
			}

		private:
			FUeAgentAssetOperationResult Move(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				Result.ResultRef = Request.Operand;
				FString SourcePackage;
				FString DestinationPackage;
				const bool bSourceValid = TryPackageName(Request.TargetRef, SourcePackage);
				if (!bSourceValid || !TryPackageName(Request.Operand, DestinationPackage))
				{
					Result.Diagnostic = OperationFact(TEXT("asset_move_target_invalid"), TEXT("Asset move requires valid source and destination package references under /Game."),
						Request.TargetRef, bSourceValid ? TEXT("/destination") : TEXT("/target"),
						MakeShared<FJsonValueString>(bSourceValid ? Request.Operand : Request.TargetRef),
						MakeShared<FJsonValueString>(TEXT("valid /Game package reference")), FString(), TEXT("command_input"));
					return Result;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(SourcePackage));
				FString Existing;
				if (!Asset || FPackageName::DoesPackageExist(DestinationPackage, &Existing))
				{
					Result.Diagnostic = OperationFact(Asset ? TEXT("asset_move_destination_conflict") : TEXT("asset_move_target_missing"),
						Asset ? TEXT("The destination package already exists.") : TEXT("The source asset could not be loaded."),
						Request.TargetRef, Asset ? TEXT("/destination/exists") : TEXT("/target/available"),
						MakeShared<FJsonValueBoolean>(Asset != nullptr), MakeShared<FJsonValueBoolean>(Asset == nullptr), Asset ? DestinationPackage : FString());
					if (Asset) Result.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Existing);
					return Result;
				}
				if (Asset->GetOutermost()->IsDirty())
				{
					Result.Diagnostic = OperationFact(TEXT("asset_move_dirty_package"), TEXT("The source asset package has unsaved changes; save or discard them before moving it."),
						Request.TargetRef, TEXT("/source/package/dirty"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					return Result;
				}
				FDurableMutation Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, SourcePackage, OwnerId, JournalRoot,
						EAssetDurableIntent::Native, Counter.Increment(), Error,
						{{DestinationPackage, Asset->IsA<UWorld>() ? EPackageHeaderKind::Map : EPackageHeaderKind::Asset}}))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				FAssetRenameData Rename(
					Asset, FPackageName::GetLongPackagePath(DestinationPackage),
					FPackageName::GetLongPackageAssetName(DestinationPackage));
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				const FString ExpectedClass = Asset->GetClass()->GetPathName();
				if (!Durable.PersistNative([&](FAssetDurableError& NativeError) {
					if (FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().RenameAssets({Rename})) return true;
					NativeError = OperationFact(TEXT("asset_move_native_failed"), TEXT("AssetTools rejected the requested asset rename."),
						Request.TargetRef, TEXT("/rename/accepted"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), DestinationPackage);
					return false;
				}, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				FString SourceFilename;
				FString DestinationFilename;
				const bool bSourceRemains = FPackageName::DoesPackageExist(SourcePackage, &SourceFilename);
				UObject* Destination = FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilename)
					? ReloadForIndependentReadback(DestinationPackage, Error) : nullptr;
				Result.bReadbackVerified = Destination && Destination->GetClass()->GetPathName() == ExpectedClass;
				if (!Result.bReadbackVerified && Error.IsEmpty())
				{
					Error = OperationFact(TEXT("asset_move_readback_failed"),
						Destination ? TEXT("The persisted destination asset has a different class from the source asset.") : TEXT("The destination package does not exist after the move."),
						Request.TargetRef, Destination ? TEXT("/destination/class") : TEXT("/destination/package/exists"),
						Destination ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Destination->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
						Destination ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExpectedClass)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), DestinationPackage);
				}
				if (Result.bReadbackVerified && bSourceRemains)
				{
					// UE may preserve the source as a redirector for unresolved references
					// or collections. It is a native result, not a residual file to delete.
					UObject* SourceReadback = ReloadForIndependentReadback(SourcePackage, Error);
					const auto* Redirector = Cast<UObjectRedirector>(SourceReadback);
					Result.bReadbackVerified = Redirector && Redirector->DestinationObject == Destination;
					if (!Result.bReadbackVerified && Error.IsEmpty())
					{
						Error = OperationFact(TEXT("asset_move_readback_failed"),
							Redirector ? TEXT("The source redirector does not point to the persisted destination asset.") : TEXT("The remaining source asset is not a redirector."),
							Request.TargetRef, Redirector ? TEXT("/source/redirector/destination") : TEXT("/source/class"),
							Redirector ? (Redirector->DestinationObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Redirector->DestinationObject->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(SourceReadback->GetClass()->GetPathName())),
							MakeShared<FJsonValueString>(Redirector ? Destination->GetPathName() : UObjectRedirector::StaticClass()->GetPathName()), SourcePackage);
					}
				}
				Result.VerificationId = TEXT("asset-move-readback:") + Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentAssetOperationResult Fixup(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString Root = Request.TargetRef;
				Root.TrimStartAndEndInline();
				if (!Root.StartsWith(TEXT("/Game/")))
				{
					Result.Diagnostic = OperationFact(TEXT("asset_fixup_root_invalid"), TEXT("Redirector fixup requires a folder below /Game/."),
						Request.TargetRef, TEXT("/target"), MakeShared<FJsonValueString>(Root), MakeShared<FJsonValueString>(TEXT("/Game/...")), FString(), TEXT("command_input"));
					return Result;
				}
				FARFilter Filter;
				Filter.PackagePaths.Add(FName(*Root));
				Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
				Filter.bRecursivePaths = true;
				// Finish discovery before collecting targets, so native fixup cannot queue
				// a discovery-dialog callback after this invocation's readback.
				FAssetRegistryModule::GetRegistry().SearchAllAssets(true);
				TArray<FAssetData> Data;
				FAssetRegistryModule::GetRegistry().GetAssets(Filter, Data);
				TArray<UObjectRedirector*> Redirectors;
				for (const FAssetData& Item : Data)
				{
					if (UObjectRedirector* const Redirector =
						Cast<UObjectRedirector>(Item.GetAsset()))
					{
						Redirectors.Add(Redirector);
					}
				}
				if (Redirectors.IsEmpty())
				{
					Result.bReadbackVerified = true;
					Result.TerminalState = TEXT("committed");
					return Result;
				}
				FDurableMutation Durable;
				UE::UAI::Contract::FDiagnostic Error;
				TArray<TPair<FString, EPackageHeaderKind>> AdditionalPackages;
				const FString FirstPackage = Redirectors[0]->GetOutermost()->GetName();
				TSet<FString> Packages{FirstPackage};
				for (const UObjectRedirector* Redirector : Redirectors)
				{
					const FString Package = Redirector->GetOutermost()->GetName();
					if (Packages.Contains(Package)) continue;
					Packages.Add(Package);
					AdditionalPackages.Emplace(Package, EPackageHeaderKind::EvidenceRequired);
				}
				if (!Durable.Prepare(Request, FirstPackage, OwnerId, JournalRoot,
					EAssetDurableIntent::Native, Counter.Increment(), Error, AdditionalPackages))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef); return Result;
				}
				Result.JournalId = Durable.JournalId();
				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				if (!Durable.PersistNative([&](FAssetDurableError&) {
					FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().FixupReferencers(
						Redirectors, false, ERedirectFixupMode::DeleteFixedUpRedirectors);
					return true;
				}, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef); return Result;
				}
				Data.Reset();
				FAssetRegistryModule::GetRegistry().GetAssets(Filter, Data);
				Result.bChanged = !Redirectors.IsEmpty();
				Result.bReadbackVerified = Data.IsEmpty();
				Result.VerificationId = TEXT("asset-fixup-readback:") + Request.RequestId;
				if (Result.bReadbackVerified && !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.bReadbackVerified = false; Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
				}
				Result.TerminalState = Result.bReadbackVerified
					? TEXT("committed") : TEXT("state_unknown");
				if (!Result.bReadbackVerified)
				{
					if (Result.Diagnostic.IsEmpty())
						Result.Diagnostic = OperationFact(TEXT("asset_fixup_readback_failed"), TEXT("Redirectors remain under the requested folder after native fixup."),
							Request.TargetRef, TEXT("/readback/redirector_count"), MakeShared<FJsonValueNumber>(Data.Num()), MakeShared<FJsonValueNumber>(0));
				}
				return Result;
			}

			FUeAgentAssetOperationResult Reimport(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.Diagnostic = OperationFact(TEXT("asset_reimport_target_invalid"), TEXT("Reimport requires a valid asset package reference under /Game."),
						Request.TargetRef, TEXT("/target"), MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("valid /Game package reference")), FString(), TEXT("command_input"));
					return Result;
				}
				UObject* const Asset = LoadObject<UObject>(
					nullptr, *ObjectPath(PackageName));
				if (!Asset)
				{
					Result.Diagnostic = OperationFact(TEXT("asset_reimport_target_missing"), TEXT("The reimport target asset could not be loaded."),
						Request.TargetRef, TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Result;
				}
				const bool bTypeMatches =
					(Request.CommandId == TEXT("asset.reimport_texture")
						&& Asset->IsA<UTexture>())
					|| (Request.CommandId == TEXT("asset.reimport_sound_wave")
						&& Asset->IsA<USoundWave>())
					|| (Request.CommandId == TEXT("asset.reimport_static_mesh")
						&& Asset->IsA<UStaticMesh>())
					|| (Request.CommandId
							== TEXT("asset.reimport_sparse_volume_texture")
						&& Asset->GetClass()->GetName().Contains(
							TEXT("SparseVolumeTexture")));
				if (!bTypeMatches || !FPaths::FileExists(Request.Operand))
				{
					const FString ExpectedClass = Request.CommandId == TEXT("asset.reimport_texture") ? UTexture::StaticClass()->GetPathName()
						: Request.CommandId == TEXT("asset.reimport_sound_wave") ? USoundWave::StaticClass()->GetPathName()
						: Request.CommandId == TEXT("asset.reimport_static_mesh") ? UStaticMesh::StaticClass()->GetPathName()
						: Request.CommandId == TEXT("asset.reimport_sparse_volume_texture") ? TEXT("class name containing SparseVolumeTexture") : TEXT("supported reimport command and matching asset class");
					Result.Diagnostic = OperationFact(!bTypeMatches ? TEXT("asset_reimport_type_mismatch") : TEXT("asset_reimport_source_missing"),
						!bTypeMatches ? TEXT("The reimport target class does not match the requested reimport command.") : TEXT("The reimport source file does not exist."),
						Request.TargetRef, !bTypeMatches ? TEXT("/target/class") : TEXT("/source_file"),
						MakeShared<FJsonValueString>(!bTypeMatches ? Asset->GetClass()->GetPathName() : Request.Operand),
						MakeShared<FJsonValueString>(!bTypeMatches ? ExpectedClass : TEXT("existing source file")), Request.CommandId);
					return Result;
				}
				FDurableMutation Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						EAssetDurableIntent::Save, Counter.Increment(), Error))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				if (!FReimportManager::Instance()->Reimport(
						Asset, false, false, Request.Operand, nullptr, INDEX_NONE,
						false, true, false))
				{
					Result.Diagnostic = OperationFact(TEXT("asset_reimport_native_failed"), TEXT("The native reimport manager rejected the asset reimport."),
						Request.TargetRef, TEXT("/reimport/accepted"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.Operand);
					FDiagnostic RollbackError;
					const bool bRestored = Durable.Rollback(true, RollbackError);
					AppendOperationRollback(Result.Diagnostic, bRestored, RollbackError);
					if (!bRestored) Result.TerminalState = TEXT("state_unknown");
					return Result;
				}
				if (!Durable.Persist(Error))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					FDiagnostic RollbackError;
					const bool bRestored = Durable.Rollback(false, RollbackError);
					AppendOperationRollback(Result.Diagnostic, bRestored, RollbackError);
					Result.TerminalState = TEXT("state_unknown");
					return Result;
				}
				Result.bReadbackVerified =
					ReloadForIndependentReadback(PackageName, Error) != nullptr;
				Result.VerificationId = TEXT("asset-reimport-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentAssetOperationResult RemoveSkeletalItem(
				const FUeAgentAssetOperationRequest& Request)
			{
				FUeAgentAssetOperationResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.Diagnostic = OperationFact(TEXT("skeletal_mesh_target_invalid"), TEXT("Skeletal mesh removal requires a valid asset package reference under /Game."),
						Request.TargetRef, TEXT("/target"), MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("valid /Game package reference")), FString(), TEXT("command_input"));
					return Result;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr, *ObjectPath(PackageName));
				const FName ItemName(*Request.Operand);
				const bool bMorph = Request.CommandId
					== TEXT("skeletal_mesh.remove_morph_target");
				UMorphTarget* const Morph = Mesh
					? (Mesh->FindMorphTarget(ItemName)
						? Mesh->FindMorphTarget(ItemName)
						: FindObject<UMorphTarget>(Mesh, *Request.Operand))
					: nullptr;
				const bool bExists = Mesh && (bMorph
					? Morph != nullptr
					: Mesh->GetSkinWeightProfiles().ContainsByPredicate(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						}));
				if (!bExists)
				{
					Result.Diagnostic = OperationFact(Mesh ? TEXT("skeletal_mesh_item_missing") : TEXT("skeletal_mesh_target_missing"),
						Mesh ? (bMorph ? TEXT("The skeletal mesh has no morph target with this name.") : TEXT("The skeletal mesh has no skin-weight profile with this name."))
							: TEXT("The target could not be loaded as a skeletal mesh."),
						Request.TargetRef, Mesh ? (bMorph ? TEXT("/morph_target/exists") : TEXT("/skin_weight_profile/exists")) : TEXT("/target/available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Mesh ? Request.Operand : FString());
					return Result;
				}
				FDurableMutation Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						EAssetDurableIntent::Save, Counter.Increment(), Error))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				if (bMorph)
				{
					if (Mesh->FindMorphTarget(ItemName))
					{
						if (!Mesh->RemoveMorphTargets({ItemName}))
						{
							Result.Diagnostic = OperationFact(TEXT("skeletal_mesh_remove_failed"), TEXT("The native skeletal mesh rejected removal of the registered morph target."),
								Request.TargetRef, TEXT("/morph_target/removed"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Request.Operand);
							FDiagnostic RollbackError;
							const bool bRestored = Durable.Rollback(true, RollbackError);
							AppendOperationRollback(Result.Diagnostic, bRestored, RollbackError);
							if (!bRestored) Result.TerminalState = TEXT("state_unknown");
							return Result;
						}
					}
					else
					{
						// This orphan has no registered morph lifecycle to release an
						// existing root. Detach it and leave collection to its native owner.
						Morph->ClearFlags(RF_Standalone);
						Morph->Rename(
							nullptr,
							GetTransientPackage(),
							REN_DontCreateRedirectors | REN_NonTransactional);
					}
				}
				else
				{
					Mesh->GetSkinWeightProfiles().RemoveAll(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						});
				}
				Mesh->MarkPackageDirty();
				if (!Durable.Persist(Error))
				{
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					FDiagnostic RollbackError;
					const bool bRestored = Durable.Rollback(false, RollbackError);
					AppendOperationRollback(Result.Diagnostic, bRestored, RollbackError);
					Result.TerminalState = TEXT("state_unknown");
					return Result;
				}
				UObject* const ReadbackObject = ReloadForIndependentReadback(PackageName, Error);
				USkeletalMesh* const ReadbackMesh = Cast<USkeletalMesh>(ReadbackObject);
				Result.bReadbackVerified = ReadbackMesh && (bMorph
					? ReadbackMesh->FindMorphTarget(ItemName) == nullptr
					: !ReadbackMesh->GetSkinWeightProfiles().ContainsByPredicate(
						[ItemName](const FSkinWeightProfileInfo& Item)
						{
							return Item.Name == ItemName;
						}));
				Result.VerificationId = TEXT("skeletal-remove-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified && Error.IsEmpty())
					Error = OperationFact(TEXT("skeletal_mesh_readback_failed"),
						ReadbackMesh ? (bMorph ? TEXT("The persisted skeletal mesh still has the removed morph target.") : TEXT("The persisted skeletal mesh still has the removed skin-weight profile."))
							: TEXT("The persisted asset is not a skeletal mesh."),
						Request.TargetRef, ReadbackMesh ? (bMorph ? TEXT("/readback/morph_target/exists") : TEXT("/readback/skin_weight_profile/exists")) : TEXT("/readback/class_matches_skeletal_mesh"),
						ReadbackMesh ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
							: ReadbackObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ReadbackObject->GetClass()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						ReadbackMesh ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName())), Request.Operand);
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = WithOperationTarget(MoveTemp(Error), Request.TargetRef);
					return Result;
				}
				Result.bChanged = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FString JournalRoot;
			FString OwnerId;
			FThreadSafeCounter64 Counter;
		};
	}

	bool TryCreateAssetOperationsOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentAssetOperationsOwner>& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot = FPaths::ConvertRelativePathToFull(JournalRoot);
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("journal_root_available"), !JournalRoot.IsEmpty());
			Actual->SetBoolField(TEXT("journal_owner_available"), !JournalOwnerId.IsEmpty());
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("journal_root_available"), true);
			Expected->SetBoolField(TEXT("journal_owner_available"), true);
			OutError = OperationFact(TEXT("asset_operations_owner_config_invalid"), TEXT("The asset operation owner requires a journal root and journal owner."),
				FString(), TEXT("/owner/configuration"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
			auto Internal = MakeShared<FJsonObject>();
			Internal->SetStringField(TEXT("journal_root"), JournalRoot);
			Internal->SetStringField(TEXT("journal_owner"), JournalOwnerId);
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Internal);
			return false;
		}
		OutOwner = MakeShared<FAssetOperationsOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}
