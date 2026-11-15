#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "DomainAdapters/WorldRead/SceneAnalysis/UeAgentSceneAnalysis.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceExport.h"
#include "Core/HjsonWorkspace/UeAgentCommittedWorkspaceDelta.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentCapturedReferenceReadScope.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelSemanticWorkspaceAdapter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/EditorWorld/UeAgentEditorWorldActivation.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Composition/HjsonImport/UeAgentSemanticWorkspaceRebase.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"
#include "Domains/Level/Authoring/UeAgentLevelPartitionReadScope.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringRegistration.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelComponentCandidates.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "EngineUtils.h"
#include "Factories/WorldFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformTime.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/UeAgentSpaceContract.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace ContextResource;
	using namespace HjsonWorkspace;
	using namespace UE::UAI::EditorWorld;
	using UE::UAI::PlanningPreflight::EChangeIntent;
	using UE::UAI::PlanningPreflight::FChangeDraft;
	using UE::UAI::StateSemantic::EUeAgentValueKind;
	using UE::UAI::StateSemantic::FUeAgentCanonicalIdentity;

	namespace
	{
		const FString SchemaId = TEXT("uai.world.edit/1");
		const FString ProviderId = TEXT("level-world-domain-provider");

		bool MayContainNativeObjectReference(const FResolvedSemanticWorkspace& Source,
			const FSemanticUnit::FDeclarationView& Property, bool* OutClosedContainer = nullptr,
			TConstArrayView<const UObject*> RemovedObjects = {}, bool* OutIncompatibleReference = nullptr)
		{
			if (OutClosedContainer) *OutClosedContainer = false;
			if (OutIncompatibleReference) *OutIncompatibleReference = false;
			bool bKnownSafeType = false, bClosedContainer = false;
			for (const FString& Id : Property.GetDependencyRefs()) {
				const auto Declaration = Source.GetModel()->FindDeclaration(Id);
				if (!Declaration || Declaration->GetRole() != TEXT("property_definition")) continue;
				const auto* Definition = Source.FindBaselineUnit(Id);
				if (!Definition) return true;
				const auto* Type = Definition->GetValues().Find(TEXT("type"));
				if (!Type || (*Type)->GetKind() != EUeAgentValueKind::Object) return true;
				if (UeAgentAuthoringTypeDescriptors::MayContainNativeObjectReferenceTo(Type->Get(), RemovedObjects)) return true;
				if (OutIncompatibleReference && !RemovedObjects.IsEmpty()
					&& UeAgentAuthoringTypeDescriptors::MayContainNativeObjectReference(Type->Get()))
					*OutIncompatibleReference = true;
				bKnownSafeType = true;
				if (OutClosedContainer) {
					const auto* Kind = (*Type)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("kind"); });
					const FString Wrapper = UeAgentAuthoringTypeDescriptors::NativeCodecValueWrapper(Kind->Value->GetText());
					bClosedContainer |= Wrapper != TEXT("literal") && Wrapper != TEXT("selected") && Wrapper != TEXT("components");
				}
			}
			if (OutClosedContainer) *OutClosedContainer = bKnownSafeType && bClosedContainer;
			return !bKnownSafeType;
		}

		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(FWorkspaceError& OutError, Contract::FDiagnostic Diagnostic)
		{
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		TSharedPtr<FJsonValue> LevelValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return MakeShared<FJsonValueNull>();
			if (Value->Type == EJson::Object || Value->Type == EJson::Array)
			{
				auto Shape = MakeShared<FJsonObject>();
				Shape->SetStringField(TEXT("type"), Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
				if (Value->Type == EJson::Object && !Value->AsObject().IsValid()) return MakeShared<FJsonValueNull>();
				Shape->SetNumberField(TEXT("count"), Value->Type == EJson::Object ? Value->AsObject()->Values.Num() : Value->AsArray().Num());
				return MakeShared<FJsonValueObject>(Shape);
			}
			if (Value->Type == EJson::Number && !FMath::IsFinite(Value->AsNumber()))
				return MakeShared<FJsonValueString>(FString::SanitizeFloat(Value->AsNumber()));
			return Value;
		}

		TSharedPtr<FJsonValue> LevelField(const TSharedPtr<const FJsonObject>& Object, const FString& Field)
		{
			return LevelValue(Object.IsValid() ? Object->TryGetField(Field) : TSharedPtr<FJsonValue>());
		}

		Contract::FDiagnostic LevelInputFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.PropertyPath = Path;
			Error.Source.InternalJsonPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.TargetPath = Target;
			return Error;
		}

		Contract::FDiagnostic LevelNativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = LevelInputFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			Error.Source.InternalJsonPath.Reset();
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		Contract::FDiagnostic LevelCommandFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			auto Error = LevelInputFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
			Error.SourceKind = TEXT("command_input");
			return Error;
		}

		Contract::FDiagnostic LevelRevisionMismatch(const FString& Expected, const FString& Actual, const FString& Target)
		{
			auto Error = LevelCommandFact(TEXT("level_world_domain_stale_revision"),
				Actual == TEXT("missing") ? TEXT("The target map is missing, but the requested baseline is not missing.")
					: TEXT("The persisted map revision differs from the requested baseline."),
				TEXT("/params/expected_revision"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		Contract::FDiagnostic AsLevelNativeResult(Contract::FDiagnostic Error)
		{
			if (!Error.IsEmpty() && Error.Source.SourceFile.IsEmpty()) Error.SourceKind = TEXT("native_result");
			for (auto& Child : Error.Diagnostics) Child = AsLevelNativeResult(MoveTemp(Child));
			if (Error.PublicCause) Error.PublicCause = MakeShared<Contract::FDiagnostic>(AsLevelNativeResult(*Error.PublicCause));
			return Error;
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()), Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Object.IsValid())
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_json_canonicalization_failed"),
					TEXT("The object to canonicalize is missing."), TEXT("/object"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))));
			if (!Json::TryCanonicalize(Object, Out, Error)) return Fail(OutError, AsLevelNativeResult(MoveTemp(Error)));
			return true;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, UE::UAI::Contract::FDiagnostic> Registration = []()
			{
				UE::UAI::Contract::FDiagnostic Error;
				const bool bRegistered =
					UeAgentLevelAuthoring::RegisterAuthoring(Error);
				return TPair<bool, UE::UAI::Contract::FDiagnostic>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError, Registration.Value.IsValid() ? Registration.Value : LevelNativeFact(
					TEXT("level_world_domain_authoring_registration_failed"), TEXT("Level authoring registration failed without returning a diagnostic."),
					TEXT("/authoring_registration/succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			return true;
		}

		TSharedRef<FJsonObject> CopyObject(const TSharedPtr<const FJsonObject>& Source)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (Source.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
				{
					Result->SetField(Pair.Key, Pair.Value);
				}
			}
			return Result;
		}

		bool ParseDocument(
			const FString& Target,
			const FString& CanonicalParamsJson,
			DomainAuthoring::FResolvedDomainHjsonDocument& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument = {};
			auto& Decoded = OutDocument;
			const DomainAuthoring::FDomainHjsonDocumentContract Contract(
				ProviderId, SchemaId, TEXT("/Script/Engine.World"));
			if (!DomainAuthoring::TryDecodeResolvedDocument(
				Target, CanonicalParamsJson, Contract, Decoded, OutError)) return false;
			DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Decoded, OutError);
			const TSharedPtr<FJsonObject>* RootDocument = nullptr;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString AssetPath;
			if (!Decoded.Root.IsValid())
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_root_document_invalid"), TEXT("The resolved document root is missing."),
					TEXT("/"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")), Target));
			if (!Decoded.Root->TryGetObjectField(TEXT("document"), RootDocument) || !RootDocument || !RootDocument->IsValid())
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_root_document_invalid"), TEXT("The root document field must be an object."),
					TEXT("/document"), LevelField(Decoded.Root, TEXT("document")), MakeShared<FJsonValueString>(TEXT("object")), Target));
			if (!(*RootDocument)->TryGetObjectField(TEXT("target"), DocumentTarget) || !DocumentTarget || !DocumentTarget->IsValid())
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_root_document_invalid"), TEXT("The document target must be an object."),
					TEXT("/document/target"), LevelField(*RootDocument, TEXT("target")), MakeShared<FJsonValueString>(TEXT("object")), Target));
			if (!(*DocumentTarget)->TryGetStringField(TEXT("asset_path"), AssetPath))
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_root_document_invalid"), TEXT("The document target asset_path must be a string."),
					TEXT("/document/target/asset_path"), LevelField(*DocumentTarget, TEXT("asset_path")), MakeShared<FJsonValueString>(TEXT("string")), Target));
			if (AssetPath != Target)
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_document_target_mismatch"), TEXT("The document asset path differs from the requested target."),
					TEXT("/document/target/asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(Target), Target));
			return true;
		}

		bool SplitTarget(
			const FString& Target,
			FString& OutPackage,
			FString& OutObject,
			FWorkspaceError& OutError)
		{
			OutPackage.Reset();
			OutObject.Reset();
			if (!Target.Split(TEXT("."), &OutPackage, &OutObject))
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_target_invalid"), TEXT("The world target must contain a package path and object name separated by a dot."),
					TEXT("/params/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("/Package/Path/Name.Name")), Target));
			if (!FPackageName::IsValidLongPackageName(OutPackage))
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_target_invalid"), TEXT("The world target has an invalid long package name."),
					TEXT("/params/target"), MakeShared<FJsonValueString>(OutPackage), MakeShared<FJsonValueString>(TEXT("valid long package name")), Target));
			if (OutObject.IsEmpty())
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_target_invalid"), TEXT("The world target has an empty object name."),
					TEXT("/params/target"), MakeShared<FJsonValueString>(Target), MakeShared<FJsonValueString>(TEXT("non-empty object name")), Target));
			FString AssetName;
			OutPackage.Split(TEXT("/"), nullptr, &AssetName,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (AssetName != OutObject)
			{
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_target_object_mismatch"),
					TEXT("The world object name must match the final package path segment."), TEXT("/params/target"),
					MakeShared<FJsonValueString>(OutObject), MakeShared<FJsonValueString>(AssetName), Target));
			}
			return true;
		}

		FString MapFilename(const FString& PackageName)
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetMapPackageExtension());
		}

		bool ReadPersistedMap(
			const FString& PackageName, FString* OutRevision, FWorkspaceError& OutError)
		{
			TArray<uint8> Bytes;
			const FString Filename = MapFilename(PackageName);
			const bool bRead = FFileHelper::LoadFileToArray(Bytes, *Filename);
			if (!bRead || Bytes.IsEmpty())
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_persisted_map_missing"),
					bRead ? TEXT("The persisted map file is empty.") : TEXT("The persisted map file could not be read."),
					bRead ? TEXT("/persisted_map/byte_count") : TEXT("/persisted_map/read_succeeded"),
					bRead ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Bytes.Num())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
					bRead ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("greater than zero"))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), PackageName));
			if (OutRevision) *OutRevision = Sha256(Bytes);
			return true;
		}
		bool StableRevision(const FString& PackageName, FString& OutRevision, FWorkspaceError& OutError)
		{
			return ReadPersistedMap(PackageName, &OutRevision, OutError);
		}

		struct FMapRestoreContext
		{
			FString OriginalPackage;
			bool bOriginalHasPersistentPackage = false;
			bool bSwitched = false;

			bool Restore(FWorkspaceError& OutError)
			{
				if (!bSwitched || OriginalPackage.IsEmpty())
				{
					return true;
				}
				if (!bOriginalHasPersistentPackage)
				{
					if (!UEditorLoadingAndSavingUtils::NewBlankMap(false))
					{
						return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_original_blank_map_restore_failed"),
							TEXT("The editor could not create a blank map while restoring the original world."), TEXT("/world_restore/blank_map_created"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OriginalPackage));
					}
					bSwitched = false;
					return true;
				}
				FWorldActivationResult Activation;
				FWorldActivationError ActivationError;
				if (!TryActivateExistingWorld(OriginalPackage,
						ECurrentWorldDirtyPolicy::RequireClean, Activation, ActivationError))
				{
					return Fail(OutError, MoveTemp(ActivationError));
				}
				bSwitched = false;
				return true;
			}
		};

		TArray<UeAgentAuthoringWorkspaceTransaction::FDurableArtifact> WorldDurableArtifacts(
			const FString& PackageName, UWorld* World)
		{
			using namespace UeAgentAuthoringWorkspaceTransaction;
			TArray<FDurableArtifact> Artifacts;
			auto AddFile = [&](const FString& Filename)
			{
				FDurableArtifact& Artifact = Artifacts.AddDefaulted_GetRef();
				Artifact.ArtifactId = Filename;
				Artifact.FinalPath = FPaths::ConvertRelativePathToFull(Filename);
			};
			AddFile(MapFilename(PackageName));
			AddFile(FPaths::ChangeExtension(MapFilename(PackageName), TEXT("upayload")));
			if (World && World->PersistentLevel && World->PersistentLevel->MapBuildData)
			{
				UPackage* BuildData = World->PersistentLevel->MapBuildData->GetOutermost();
				if (BuildData != World->GetOutermost())
				{
					const FString Filename = FPackageName::LongPackageNameToFilename(
						BuildData->GetName(), FPackageName::GetAssetPackageExtension());
					AddFile(Filename);
					AddFile(FPaths::ChangeExtension(Filename, TEXT("upayload")));
				}
			}
			TArray<FString> ExternalPaths = ULevel::GetExternalActorsPaths(PackageName);
			for (const FString& Path : ULevel::GetExternalObjectsPaths(PackageName)) ExternalPaths.AddUnique(Path);
			for (const FString& Path : ExternalPaths)
			{
				FDurableArtifact& Artifact = Artifacts.AddDefaulted_GetRef();
				Artifact.ArtifactId = Path;
				Artifact.FinalPath = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Path));
				Artifact.Kind = EDurableArtifactKind::DirectoryTree;
			}
			return Artifacts;
		}

		// Only discard the target's transaction-owned native state, before the common
		// durable owner restores its disk before-images. Never save a failed mutation.
		bool ReleaseWorldForRestore(const FString& PackageName, const bool bNewAsset, UE::UAI::Contract::FDiagnostic& Error)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (!Package) return true;
			UWorld* TargetWorld = UWorld::FindWorldInPackage(Package);
			if (bNewAsset && TargetWorld) FAssetRegistryModule::AssetDeleted(TargetWorld);
			TArray<UPackage*> Packages = Package->GetExternalPackages();
			Packages.AddUnique(Package);
			if (TargetWorld && TargetWorld->PersistentLevel && TargetWorld->PersistentLevel->MapBuildData)
				Packages.AddUnique(TargetWorld->PersistentLevel->MapBuildData->GetOutermost());
			for (UPackage* Owned : Packages) { Owned->SetDirtyFlag(false); ResetLoaders(Owned); }
			if (TargetWorld && GEditor->GetEditorWorldContext().World() == TargetWorld)
			{
				if (UEditorLoadingAndSavingUtils::NewBlankMap(false)) return true;
				Error = LevelNativeFact(TEXT("level_world_domain_release_failed"),
					TEXT("The editor could not switch to a blank map before restoring the target package."), TEXT("/world_release/blank_map_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName);
				return false;
			}
			FText UnloadError;
			const bool bUnloaded = UPackageTools::UnloadPackages(Packages, UnloadError, true);
			if (!bUnloaded) Error = LevelNativeFact(TEXT("level_world_domain_world_unload_failed"),
				UnloadError.IsEmpty() ? TEXT("The editor failed to unload the target world packages.") : UnloadError.ToString(),
				TEXT("/world_release/packages_unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName);
			return bUnloaded;
		}

		bool CreateWorldAsset(const FString& PackageName, const FString& ObjectName,
			const bool bWorldPartition, FWorkspaceError& OutError)
		{
			UPackage* Package = CreatePackage(*PackageName);
			UWorldFactory* Factory = NewObject<UWorldFactory>();
			if (!Package || !Factory)
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_world_factory_unavailable"),
					!Package ? TEXT("The target world package could not be created.") : TEXT("The world factory could not be created."),
					!Package ? TEXT("/world_create/package") : TEXT("/world_create/factory"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			Factory->WorldType = EWorldType::Editor;
			Factory->bInformEngineOfWorld = false;
			Factory->bCreateWorldPartition = bWorldPartition;
			Factory->bEnableWorldPartitionStreaming = bWorldPartition;
			UWorld* World = Cast<UWorld>(Factory->FactoryCreateNew(UWorld::StaticClass(), Package,
				FName(*ObjectName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!World)
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_world_create_failed"),
					TEXT("The world factory did not return a World object."), TEXT("/world_create/world"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			FAssetRegistryModule::AssetCreated(World);
			World->MarkPackageDirty();
			if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_world_create_save_failed"),
					TEXT("The newly created world could not be saved."), TEXT("/world_create/map_saved"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			Package->SetDirtyFlag(false);
			World = nullptr;
			Factory = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_world_create_unload_failed"),
					UnloadError.IsEmpty() ? TEXT("The newly created world package could not be unloaded.") : UnloadError.ToString(),
					TEXT("/world_create/package_unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			return true;
		}

		bool OpenWorld(const FString& PackageName, const FString& ObjectName,
			const bool bAllowCreate, const bool bWorldPartition, FMapRestoreContext& OutRestore,
			UWorld*& OutWorld, FWorkspaceError& OutError, TFunction<void()> BeforeCreate = {})
		{
			OutWorld = nullptr;
			if (!GEditor)
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_editor_unavailable"), TEXT("The editor is unavailable."),
					TEXT("/editor"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			UWorld* Current = GEditor->GetEditorWorldContext().World();
			if (!Current || !Current->GetOutermost())
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_current_world_missing"),
					!Current ? TEXT("The editor has no current world.") : TEXT("The current editor world has no outermost package."),
					!Current ? TEXT("/editor/world") : TEXT("/editor/world/package"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			const FString CurrentPackage = Current->GetOutermost()->GetName();
			if (CurrentPackage == PackageName)
			{
				OutWorld = Current;
				return true;
			}
			if (Current->GetOutermost()->IsDirty())
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_current_world_dirty"),
					TEXT("The current world has unsaved changes and cannot be replaced."), TEXT("/editor/world/package/dirty"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), CurrentPackage));
			}
			OutRestore.OriginalPackage = CurrentPackage;
			OutRestore.bOriginalHasPersistentPackage =
				FPackageName::DoesPackageExist(CurrentPackage);
			const bool bExists = FPackageName::DoesPackageExist(PackageName);
			if (!bExists)
			{
				if (!bAllowCreate)
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_target_not_found"),
						TEXT("The target world package does not exist and world creation is disabled."), TEXT("/target/package/exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
				}
				if (BeforeCreate) BeforeCreate();
				if (!CreateWorldAsset(PackageName, ObjectName, bWorldPartition, OutError))
				{
					return false;
				}
			}
			FWorldActivationResult Activation;
			FWorldActivationError ActivationError;
			if (!TryActivateExistingWorld(PackageName, ECurrentWorldDirtyPolicy::RequireClean,
					Activation, ActivationError))
			{
				return Fail(OutError, MoveTemp(ActivationError));
			}
			OutWorld = Activation.World;
			OutRestore.bSwitched = Activation.bSwitched;
			return true;
		}

		bool ValidateFrame(const TSharedPtr<const FJsonObject>& Extension, FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Frame = nullptr;
			const FString Code = TEXT("level_world_domain_space_contract_invalid");
			if (!Extension.IsValid())
				return Fail(OutError, LevelInputFact(Code, TEXT("The world provider extension is missing."), TEXT("/"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))));
			if (!Extension->TryGetObjectField(TEXT("frame"), Frame) || !Frame || !Frame->IsValid())
				return Fail(OutError, LevelInputFact(Code, TEXT("The world frame must be an object."), TEXT("/frame"),
					LevelField(Extension, TEXT("frame")), MakeShared<FJsonValueString>(TEXT("object"))));
			double Dimension = 0.0;
			if (!(*Frame)->TryGetNumberField(TEXT("dimension"), Dimension))
				return Fail(OutError, LevelInputFact(Code, TEXT("World frame dimension must be a number."), TEXT("/frame/dimension"),
					LevelField(*Frame, TEXT("dimension")), MakeShared<FJsonValueNumber>(3)));
			if (Dimension != 3.0)
				return Fail(OutError, LevelInputFact(Code, TEXT("World authoring requires a three-dimensional frame."), TEXT("/frame/dimension"),
					LevelField(*Frame, TEXT("dimension")), MakeShared<FJsonValueNumber>(3)));
			const TCHAR* Fields[] = {TEXT("semantic_frame"), TEXT("write_frame"), TEXT("read_frame"), TEXT("unit")};
			const TCHAR* Expected[] = {TEXT("ue_world"), TEXT("ue_world"), TEXT("ue_world"), TEXT("centimeter")};
			FString Values[4];
			for (int32 Index = 0; Index < 4; ++Index)
				if (!(*Frame)->TryGetStringField(Fields[Index], Values[Index]))
					return Fail(OutError, LevelInputFact(Code, TEXT("World frame field must be a string."), TEXT("/frame/") + FString(Fields[Index]),
						LevelField(*Frame, Fields[Index]), MakeShared<FJsonValueString>(Expected[Index])));
			for (int32 Index = 0; Index < 4; ++Index)
				if (Values[Index] != Expected[Index])
					return Fail(OutError, LevelInputFact(Code, TEXT("World frame field does not match the authoring coordinate contract."), TEXT("/frame/") + FString(Fields[Index]),
						MakeShared<FJsonValueString>(Values[Index]), MakeShared<FJsonValueString>(Expected[Index])));
			return true;
		}

		bool TryReadNamedComponents(const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* First, const TCHAR* Second, const TCHAR* Third,
			const FString& Code, const FString& Path, const FString& Target, const bool bNative,
			TArray<TSharedPtr<FJsonValue>>& OutComponents, FWorkspaceError& OutError)
		{
			OutComponents.Reset();
			const TCHAR* Names[] = {First, Second, Third};
			double Values[3] = {};
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!Object.IsValid() || !Object->TryGetNumberField(Names[Index], Values[Index]))
				{
					auto Error = LevelInputFact(Code, TEXT("A named transform component must be a number."), Path + TEXT("/") + Names[Index],
						LevelField(Object, Names[Index]), MakeShared<FJsonValueString>(TEXT("finite number")), Target);
					return Fail(OutError, bNative ? AsLevelNativeResult(MoveTemp(Error)) : MoveTemp(Error));
				}
			}
			for (int32 Index = 0; Index < 3; ++Index)
				if (!FMath::IsFinite(Values[Index]))
				{
					auto Error = LevelInputFact(Code, TEXT("A named transform component must be finite."), Path + TEXT("/") + Names[Index],
						LevelField(Object, Names[Index]), MakeShared<FJsonValueString>(TEXT("finite number")), Target);
					return Fail(OutError, bNative ? AsLevelNativeResult(MoveTemp(Error)) : MoveTemp(Error));
				}
			for (const double Value : Values) OutComponents.Add(MakeShared<FJsonValueNumber>(Value));
			return true;
		}

                bool TransformForLevel(
			const TSharedPtr<const FJsonObject>& Source,
			const bool bActor,
			const FString& Target,
			const FString& ErrorPointer,
			TSharedPtr<FJsonObject>& OutResult,
			FWorkspaceError& OutError, const bool bNative = false)
		{
			OutResult.Reset();
			auto Reject = [&](const FString& Code, const FString& Reason, const FString& Path,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				auto Error = LevelInputFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected), Target);
				return Fail(OutError, bNative ? AsLevelNativeResult(MoveTemp(Error)) : MoveTemp(Error));
			};
			if (!Source.IsValid())
			{
				return Reject(TEXT("level_world_domain_transform_object_invalid"), TEXT("The transform must be an object."), ErrorPointer,
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")));
			}

			TSharedRef<FJsonObject> Result = CopyObject(Source);
			const bool bHasLocation = Source->HasField(TEXT("location"));
			const bool bHasTranslation = Source->HasField(TEXT("translation"));
			if (bHasLocation && bHasTranslation)
			{
				return Reject(TEXT("level_world_domain_transform_translation_conflict"), TEXT("Transform location and translation cannot both be specified."),
					ErrorPointer + TEXT("/location"), MakeShared<FJsonValueString>(TEXT("location and translation")), MakeShared<FJsonValueString>(TEXT("one translation representation")));
			}
			if (bHasLocation)
			{
				const TSharedPtr<FJsonObject>* Location = nullptr;
				TArray<TSharedPtr<FJsonValue>> Components;
				if (!Source->TryGetObjectField(TEXT("location"), Location) || !Location || !Location->IsValid())
					return Reject(TEXT("level_world_domain_transform_location_invalid"), TEXT("Transform location must be an object."),
						ErrorPointer + TEXT("/location"), LevelField(Source, TEXT("location")), MakeShared<FJsonValueString>(TEXT("object")));
				if (!TryReadNamedComponents(*Location, TEXT("x"), TEXT("y"), TEXT("z"),
					TEXT("level_world_domain_transform_location_invalid"), ErrorPointer + TEXT("/location"), Target, bNative, Components, OutError)) return false;
				TSharedRef<FJsonObject> Translation = MakeShared<FJsonObject>();
				Translation->SetArrayField(TEXT("components"), MoveTemp(Components));
				Result->RemoveField(TEXT("location"));
				Result->SetObjectField(TEXT("translation"), Translation);
			}
			else if (bHasTranslation)
			{
				const TSharedPtr<FJsonObject>* Translation = nullptr;
				if (!Source->TryGetObjectField(TEXT("translation"), Translation)
					|| !Translation || !Translation->IsValid())
				{
					return Reject(TEXT("level_world_domain_transform_translation_invalid"), TEXT("Transform translation must be an object."),
						ErrorPointer + TEXT("/translation"), LevelField(Source, TEXT("translation")), MakeShared<FJsonValueString>(TEXT("object")));
				}
			}

			struct FNamedTransformField
			{
				const TCHAR* Field;
				const TCHAR* First;
				const TCHAR* Second;
				const TCHAR* Third;
				const TCHAR* InvalidCode;
				const TCHAR* ConflictCode;
			};
			for (const FNamedTransformField& Named : {
				FNamedTransformField{TEXT("rotation"), TEXT("pitch"), TEXT("yaw"),
					TEXT("roll"), TEXT("level_world_domain_transform_rotation_invalid"),
					TEXT("level_world_domain_transform_rotation_conflict")},
				FNamedTransformField{TEXT("scale"), TEXT("x"), TEXT("y"), TEXT("z"),
					TEXT("level_world_domain_transform_scale_invalid"),
					TEXT("level_world_domain_transform_scale_conflict")}})
			{
				if (!Source->HasField(Named.Field))
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (!Source->TryGetObjectField(Named.Field, Value)
					|| !Value || !Value->IsValid())
				{
					return Reject(Named.InvalidCode, TEXT("Transform field must be an object."), ErrorPointer + TEXT("/") + Named.Field,
						LevelField(Source, Named.Field), MakeShared<FJsonValueString>(TEXT("object")));
				}
				const bool bHasNamedAxes = (*Value)->HasField(Named.First)
					|| (*Value)->HasField(Named.Second)
					|| (*Value)->HasField(Named.Third);
				if (bHasNamedAxes && (*Value)->HasField(TEXT("components")))
				{
					return Reject(Named.ConflictCode, TEXT("Named transform axes and components cannot both be specified."), ErrorPointer + TEXT("/") + Named.Field,
						MakeShared<FJsonValueString>(TEXT("named axes and components")), MakeShared<FJsonValueString>(TEXT("one component representation")));
				}
				if (bHasNamedAxes)
				{
					TArray<TSharedPtr<FJsonValue>> Components;
					if (!TryReadNamedComponents(*Value, Named.First, Named.Second, Named.Third, Named.InvalidCode,
						ErrorPointer + TEXT("/") + Named.Field, Target, bNative, Components, OutError)) return false;
					TSharedRef<FJsonObject> Canonical = MakeShared<FJsonObject>();
					Canonical->SetArrayField(TEXT("components"), MoveTemp(Components));
					Result->SetObjectField(Named.Field, Canonical);
				}
			}

			Result->SetObjectField(TEXT("space_contract"),
				UeAgentSpaceContract::Make3D(
					bActor ? TEXT("actor.transform")
						: TEXT("component.relative_transform"),
					bActor ? TEXT("ue_actor_world_transform")
						: TEXT("ue_scene_component_parent_relative_transform"),
					TEXT("ue_world"), TEXT("identity")));
			OutResult = Result;
			return true;
		}

		bool CopyOptionalArray(
			const TSharedPtr<const FJsonObject>& Source,
			const TCHAR* Field,
			const TSharedRef<FJsonObject>& Target)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Source.IsValid() || !Source->HasField(Field)) return true;
			if (!Source->TryGetArrayField(Field, Values) || !Values) return false;
			Target->SetArrayField(Field, *Values);
			return true;
		}

		bool BuildLevelWorkspace(
			const FString& Target,
			const TSharedPtr<const FJsonObject>& Extension,
			TSharedRef<FJsonObject>& OutBundle,
			const TArray<TSharedPtr<FJsonValue>>*& OutDataLayers,
			FWorkspaceError& OutError, TMap<FString, FString>& SourceBindings)
		{
			const FString ExtensionPointer = TEXT("/root/document/provider_extension");
			FString CurrentPointer = ExtensionPointer;
			auto Fail = [&](FWorkspaceError& Error, Contract::FDiagnostic Diagnostic,
				const FString& TargetContext = FString(), const FString& Pointer = FString())
			{
				if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetContext;
				if (Diagnostic.SourceKind != TEXT("native_result") && Diagnostic.SourceKind != TEXT("command_input")
					&& Diagnostic.SemanticPointer.IsEmpty())
				{
					if (!Pointer.IsEmpty())
						Diagnostic.SemanticPointer = Pointer.StartsWith(TEXT("/root/")) ? Pointer
							: CurrentPointer + (Pointer == TEXT("/") ? FString() : Pointer);
					else if (Diagnostic.Source.InternalJsonPath.StartsWith(TEXT("/root/")))
						Diagnostic.SemanticPointer = Diagnostic.Source.InternalJsonPath;
					else if (Diagnostic.Source.InternalJsonPath.IsEmpty())
						Diagnostic.SemanticPointer = CurrentPointer;
				}
				Error.SetFailure(MoveTemp(Diagnostic));
				return false;
			};
			OutDataLayers = nullptr;
			if (!ValidateFrame(Extension, OutError))
			{
				auto Diagnostic = OutError.GetDiagnostic();
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Diagnostic, ExtensionPointer);
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			OutBundle = MakeShared<FJsonObject>();
			OutBundle->SetStringField(TEXT("schema"),
				TEXT("uai.virtual_workspace.v1"));
			TSharedRef<FJsonObject> Units = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> WorldUnit = MakeShared<FJsonObject>();
			WorldUnit->SetStringField(TEXT("operation"), TEXT("update"));
			TSharedRef<FJsonObject> WorldTarget = MakeShared<FJsonObject>();
			WorldTarget->SetStringField(TEXT("world_path"), Target);
			WorldUnit->SetObjectField(TEXT("target"), WorldTarget);
			Units->SetObjectField(TEXT("index.hjson"), WorldUnit);
			SourceBindings.Add(TEXT("index.hjson"), TEXT("/root/document"));

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			CurrentPointer = ExtensionPointer + TEXT("/actors");
			if (Extension->HasField(TEXT("actors"))
				&& (!Extension->TryGetArrayField(TEXT("actors"), Actors) || !Actors))
			{
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actors_invalid"), TEXT("World actors must be an array."), CurrentPointer,
LevelField(Extension, TEXT("actors")), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			int32 ActorIndex = 0;
			for (const TSharedPtr<FJsonValue>& ActorValue :
				Actors ? *Actors : TArray<TSharedPtr<FJsonValue>>())
			{
				const FString ActorPointer = ExtensionPointer + TEXT("/actors/") + FString::FromInt(ActorIndex);
				CurrentPointer = ActorPointer + TEXT("/managed_id");
				const TSharedPtr<FJsonObject> Actor = ActorValue.IsValid() && ActorValue->Type == EJson::Object ? ActorValue->AsObject() : nullptr;
				FString ManagedId;
				FString ActorClass;
				FString Operation = TEXT("upsert");
				if (!Actor.IsValid())
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_identity_invalid"), TEXT("Each actor entry must be an object."),
						ActorPointer, LevelValue(ActorValue), MakeShared<FJsonValueString>(TEXT("object"))));
				if (!Actor->TryGetStringField(TEXT("managed_id"), ManagedId) || ManagedId.TrimStartAndEnd().IsEmpty())
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_identity_invalid"), TEXT("Actor managed_id must be a non-empty string."), CurrentPointer,
LevelField(Actor, TEXT("managed_id")), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
				Actor->TryGetStringField(TEXT("operation"), Operation);
				Actor->TryGetStringField(TEXT("type"), ActorClass);
				ActorClass.TrimStartAndEndInline();

				const TSharedPtr<FJsonObject>* CandidatePtr = nullptr;
				CurrentPointer = ActorPointer + TEXT("/candidate");
				const bool bHasCandidate = Actor->HasField(TEXT("candidate"));
				if (bHasCandidate
					&& (!Actor->TryGetObjectField(TEXT("candidate"), CandidatePtr)
						|| !CandidatePtr || !CandidatePtr->IsValid()))
				{
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_candidate_invalid"), TEXT("Actor candidate must be an object."), CurrentPointer,
LevelField(Actor, TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("object")), ManagedId));
				}
				const TSharedPtr<FJsonObject>* PlacementContextPtr = nullptr;
				CurrentPointer = ActorPointer + TEXT("/placement_context");
				if (Actor->HasField(TEXT("placement_context"))
					&& (!Actor->TryGetObjectField(TEXT("placement_context"), PlacementContextPtr)
						|| !PlacementContextPtr || !PlacementContextPtr->IsValid()))
				{
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_placement_context_invalid"), TEXT("Actor placement_context must be an object."), CurrentPointer,
LevelField(Actor, TEXT("placement_context")), MakeShared<FJsonValueString>(TEXT("object")), ManagedId));
				}

				if (bHasCandidate)
				{
					CurrentPointer = ActorPointer + TEXT("/candidate");
					const FString CandidatePointer = FString::Printf(
						TEXT("/root/document/provider_extension/actors/%d/candidate"),
						ActorIndex);
					DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					if (!DomainAuthoring::TryReadCandidateSelector(
						*CandidatePtr, CandidatePointer, Selector, OutError))
					{
						return Fail(OutError, OutError.GetDiagnostic(), ManagedId);
					}
					FString PlacementCategory;
					if (PlacementContextPtr && PlacementContextPtr->IsValid())
					{
						(*PlacementContextPtr)->TryGetStringField(
							TEXT("category"), PlacementCategory);
					}
					PlacementCategory.TrimStartAndEndInline();

					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					UE::UAI::Contract::FDiagnostic ResolveError;
					if (!UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
					Selector.Name,
						FString(),
							PlacementCategory,
							ResolvedCandidate,
							ResolveError, Selector.Select))
					{
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(ResolveError, CandidatePointer);
						return Fail(OutError, ResolveError, ManagedId);
					}

					const FString ResolvedClass =
						ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
					if (ResolvedClass.IsEmpty()
						|| (!ActorClass.IsEmpty()
							&& !ActorClass.Equals(ResolvedClass, ESearchCase::CaseSensitive)))
					{
						CurrentPointer = ActorPointer + (ActorClass.IsEmpty() ? TEXT("/candidate") : TEXT("/type"));
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_candidate_class_mismatch"),
							ResolvedClass.IsEmpty() ? TEXT("The resolved actor candidate has no actor class selector.") : TEXT("The declared actor type differs from the resolved candidate class."),
							CurrentPointer, MakeShared<FJsonValueString>(ResolvedClass.IsEmpty() ? ResolvedClass : ActorClass),
							MakeShared<FJsonValueString>(ResolvedClass.IsEmpty() ? TEXT("non-empty actor class") : ResolvedClass), ManagedId));
					}

					ActorClass = ResolvedClass;
				}
				else if (ActorClass.IsEmpty()
					&& (Operation.Equals(TEXT("create"), ESearchCase::IgnoreCase)
						|| Operation.Equals(TEXT("upsert"), ESearchCase::IgnoreCase)))
				{
					CurrentPointer = ActorPointer + TEXT("/candidate");
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_create_selector_required"), TEXT("Creating or upserting an actor requires a candidate selector or type."), CurrentPointer,
LevelField(Actor, TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("candidate selector or type")), ManagedId));
				}

				TSharedRef<FJsonObject> Unit = MakeShared<FJsonObject>();
				Unit->SetStringField(TEXT("operation"), Operation);
				TSharedRef<FJsonObject> UnitTarget = MakeShared<FJsonObject>();
				UnitTarget->SetStringField(TEXT("world_path"), Target);
				UnitTarget->SetStringField(TEXT("managed_id"), ManagedId);
				if (!ActorClass.IsEmpty())
					UnitTarget->SetStringField(TEXT("actor_class"), ActorClass);
				Unit->SetObjectField(TEXT("target"), UnitTarget);
				CurrentPointer = ActorPointer + TEXT("/properties");
				if (!CopyOptionalArray(Actor, TEXT("properties"), Unit))
				{
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_properties_invalid"), TEXT("Actor properties must be an array."), CurrentPointer,
LevelField(Actor, TEXT("properties")), MakeShared<FJsonValueString>(TEXT("array")), ManagedId));
				}
				TSharedRef<FJsonObject> ProviderExtension = MakeShared<FJsonObject>();
				for (const TCHAR* Field : {TEXT("label"), TEXT("folder_path")})
				{
					FString Value;
					if (Actor->TryGetStringField(Field, Value))
						ProviderExtension->SetStringField(Field, Value);
				}
				if (bHasCandidate)
				{
					ProviderExtension->SetObjectField(
						TEXT("candidate"), CopyObject(*CandidatePtr));
				}
				if (PlacementContextPtr && PlacementContextPtr->IsValid())
				{
					ProviderExtension->SetObjectField(
						TEXT("placement_context"), CopyObject(*PlacementContextPtr));
				}
				if (Actor->HasField(TEXT("transform")))
				{
					const TSharedPtr<FJsonObject>* Transform = nullptr;
					TSharedPtr<FJsonObject> LevelTransform;
					if (!Actor->TryGetObjectField(TEXT("transform"), Transform)
						|| !Transform || !Transform->IsValid()
						|| !TransformForLevel(*Transform, true, ManagedId,
							ActorPointer + TEXT("/transform"), LevelTransform, OutError))
					{
						if (OutError.GetCode().IsEmpty())
						{
							return Fail(OutError, LevelInputFact(TEXT("level_world_domain_actor_transform_invalid"), TEXT("Actor transform must be an object."),
								ActorPointer + TEXT("/transform"), LevelField(Actor, TEXT("transform")), MakeShared<FJsonValueString>(TEXT("object")), ManagedId));
						}
						return false;
					}
					ProviderExtension->SetObjectField(
						TEXT("transform"), LevelTransform);
				}
				const TSharedPtr<FJsonObject>* Attachment = nullptr;
				if (Actor->TryGetObjectField(TEXT("attachment"), Attachment)
					&& Attachment && Attachment->IsValid())
				{
					ProviderExtension->SetObjectField(TEXT("attachment"),
						CopyObject(*Attachment));
				}
				if (!ProviderExtension->Values.IsEmpty())
					Unit->SetObjectField(TEXT("provider_extension"), ProviderExtension);
				const FString ActorPath = FString::Printf(
					TEXT("actors/items/actor_%04d/actor.hjson"), ActorIndex);
				Units->SetObjectField(ActorPath, Unit);
				SourceBindings.Add(ActorPath, ActorPointer);

				const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
				CurrentPointer = ActorPointer + TEXT("/components");
				if (Actor->HasField(TEXT("components"))
					&& (!Actor->TryGetArrayField(TEXT("components"), Components)
						|| !Components))
				{
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_components_invalid"), TEXT("Actor components must be an array."), CurrentPointer,
LevelField(Actor, TEXT("components")), MakeShared<FJsonValueString>(TEXT("array")), ManagedId));
				}
				int32 ComponentIndex = 0;
				for (const TSharedPtr<FJsonValue>& ComponentValue :
					Components ? *Components : TArray<TSharedPtr<FJsonValue>>())
				{
					const FString ComponentPointer = ActorPointer + TEXT("/components/") + FString::FromInt(ComponentIndex);
					CurrentPointer = ComponentPointer;
					const TSharedPtr<FJsonObject> Component = ComponentValue.IsValid() && ComponentValue->Type == EJson::Object ? ComponentValue->AsObject() : nullptr;
					FString Name;
					FString Class;
					FString ComponentOperation = TEXT("upsert");
					if (!Component.IsValid())
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_component_identity_invalid"), TEXT("Each component entry must be an object."),
							ComponentPointer, LevelValue(ComponentValue), MakeShared<FJsonValueString>(TEXT("object")), ManagedId));
					if (!Component->TryGetStringField(TEXT("component_name"), Name) || Name.TrimStartAndEnd().IsEmpty())
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_component_identity_invalid"), TEXT("Component name must be a non-empty string."),
							ComponentPointer + TEXT("/component_name"), LevelField(Component, TEXT("component_name")), MakeShared<FJsonValueString>(TEXT("non-empty string")), ManagedId));
					if (!Component->TryGetStringField(TEXT("type"), Class) || Class.TrimStartAndEnd().IsEmpty())
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_component_identity_invalid"), TEXT("Component type must be a non-empty string."),
							ComponentPointer + TEXT("/type"), LevelField(Component, TEXT("type")), MakeShared<FJsonValueString>(TEXT("non-empty string")), Name));
					Component->TryGetStringField(TEXT("operation"), ComponentOperation);
					TSharedRef<FJsonObject> ComponentUnit = MakeShared<FJsonObject>();
					ComponentUnit->SetStringField(TEXT("operation"), ComponentOperation);
					TSharedRef<FJsonObject> ComponentTarget = MakeShared<FJsonObject>();
					ComponentTarget->SetStringField(TEXT("world_path"), Target);
					ComponentTarget->SetStringField(TEXT("owner_managed_id"), ManagedId);
					ComponentTarget->SetStringField(TEXT("component_name"), Name);
					ComponentTarget->SetStringField(TEXT("component_class"), Class);
					ComponentUnit->SetObjectField(TEXT("target"), ComponentTarget);
					CurrentPointer = ComponentPointer + TEXT("/properties");
					if (!CopyOptionalArray(Component, TEXT("properties"), ComponentUnit))
					{
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_component_properties_invalid"), TEXT("Component properties must be an array."), CurrentPointer,
LevelField(Component, TEXT("properties")), MakeShared<FJsonValueString>(TEXT("array")), Name));
					}
					const TSharedPtr<FJsonObject>* ComponentAttachment = nullptr;
					if (Component->TryGetObjectField(TEXT("attachment"),
							ComponentAttachment)
						&& ComponentAttachment && ComponentAttachment->IsValid())
					{
						TSharedRef<FJsonObject> LevelAttachment =
							CopyObject(*ComponentAttachment);
						if ((*ComponentAttachment)->HasField(TEXT("transform")))
						{
							const TSharedPtr<FJsonObject>* RelativeTransform = nullptr;
							TSharedPtr<FJsonObject> LevelTransform;
							if (!(*ComponentAttachment)->TryGetObjectField(
									TEXT("transform"), RelativeTransform)
								|| !RelativeTransform || !RelativeTransform->IsValid()
								|| !TransformForLevel(*RelativeTransform, false, Name,
									ComponentPointer + TEXT("/attachment/transform"),
									LevelTransform, OutError))
							{
								if (OutError.GetCode().IsEmpty())
								{
									return Fail(OutError, LevelInputFact(TEXT("level_world_domain_component_relative_transform_invalid"),
										TEXT("Component attachment transform must be an object."), ComponentPointer + TEXT("/attachment/transform"),
										LevelField(*ComponentAttachment, TEXT("transform")), MakeShared<FJsonValueString>(TEXT("object")), Name));
								}
								return false;
							}
							LevelAttachment->SetObjectField(
								TEXT("transform"), LevelTransform);
						}
						TSharedRef<FJsonObject> ComponentExtension =
							MakeShared<FJsonObject>();
						ComponentExtension->SetObjectField(TEXT("attachment"),
							LevelAttachment);
						ComponentUnit->SetObjectField(TEXT("provider_extension"),
							ComponentExtension);
					}
					const FString ComponentPath = FString::Printf(
						TEXT("actors/items/actor_%04d/components/component_%04d/component.hjson"),
						ActorIndex, ComponentIndex++);
					Units->SetObjectField(ComponentPath, ComponentUnit);
					SourceBindings.Add(ComponentPath, ComponentPointer);
				}
				++ActorIndex;
			}
			CurrentPointer = ExtensionPointer + TEXT("/data_layers");
			if (Extension->HasField(TEXT("data_layers"))
				&& (!Extension->TryGetArrayField(TEXT("data_layers"), OutDataLayers)
					|| !OutDataLayers))
			{
				return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layers_invalid"), TEXT("World data_layers must be an array."), CurrentPointer,
LevelField(Extension, TEXT("data_layers")), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			OutBundle->SetObjectField(TEXT("units"), Units);
			return true;
		}

		void SupplementLevelSemanticSource(const FResolvedSemanticWorkspaceRef& Workspace, const FString& UnitId,
			const FString& Prefix, Contract::FDiagnostic& Error)
		{
			if (Error.IsEmpty()) return;
			if (!UnitId.IsEmpty() && Error.Source.SourceFile.IsEmpty()
				&& Error.SourceKind != TEXT("native_result") && Error.SourceKind != TEXT("command_input"))
			{
				if (Error.Source.SourceUnit.IsEmpty())
				{
					Error.Source.SourceUnit = UnitId;
					const FString Relative = Error.Source.InternalJsonPath;
					Error.Source.InternalJsonPath = Prefix + (Relative.IsEmpty() || Relative == TEXT("/") ? FString() : Relative);
				}
				Workspace->LocateDocumentDiagnostic({}, {}, Error);
			}
			for (auto& Child : Error.Diagnostics) SupplementLevelSemanticSource(Workspace, UnitId, Prefix, Child);
			if (Error.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
				SupplementLevelSemanticSource(Workspace, UnitId, Prefix, *Cause);
				Error.PublicCause = MoveTemp(Cause);
			}
		}

		bool TryBuildLevelProviderExtension(const TSharedPtr<FJsonObject>& State,
			bool bActorState, const FString& Target, TSharedRef<FJsonObject>& OutExtension,
			FWorkspaceError& OutError, const bool bNative = false)
		{
			OutExtension = MakeShared<FJsonObject>();
			auto Reject = [&](const FString& Path, TSharedPtr<FJsonValue> Actual)
			{
				auto Error = LevelInputFact(TEXT("level_semantic_provider_extension_invalid"),
					TEXT("The semantic state field must be an object."), Path, MoveTemp(Actual),
					MakeShared<FJsonValueString>(TEXT("object")), Target);
				return Fail(OutError, bNative ? AsLevelNativeResult(MoveTemp(Error)) : MoveTemp(Error));
			};
			if (!State.IsValid()) return Reject(TEXT("/"), MakeShared<FJsonValueNull>());
			if (bActorState)
			{
				for (const TCHAR* Field : { TEXT("label"), TEXT("folder_path"), TEXT("tags"), TEXT("attachment"), TEXT("level") })
				{
					if (const TSharedPtr<FJsonValue> Value = State->TryGetField(Field); Value.IsValid())
						OutExtension->SetField(Field, Value);
				}
				if (State->HasField(TEXT("transform")))
				{
					const TSharedPtr<FJsonObject>* Transform = nullptr;
					TSharedPtr<FJsonObject> LevelTransform;
					if (!State->TryGetObjectField(TEXT("transform"), Transform) || !Transform || !Transform->IsValid())
						return Reject(TEXT("/transform"), LevelField(State, TEXT("transform")));
					if (!TransformForLevel(*Transform, true, Target, TEXT("/transform"), LevelTransform, OutError, bNative)) return false;
					OutExtension->SetObjectField(TEXT("transform"), LevelTransform);
				}
			}
			else
			{
				if (const auto Pcg = State->TryGetField(TEXT("pcg"))) OutExtension->SetField(TEXT("pcg"), Pcg);
				const TSharedPtr<FJsonObject>* Attachment = nullptr;
				if (State->TryGetObjectField(TEXT("attachment"), Attachment) && Attachment && Attachment->IsValid())
				{
					TSharedRef<FJsonObject> LevelAttachment = CopyObject(*Attachment);
					if ((*Attachment)->HasField(TEXT("transform")))
					{
						const TSharedPtr<FJsonObject>* Transform = nullptr;
						TSharedPtr<FJsonObject> LevelTransform;
						if (!(*Attachment)->TryGetObjectField(TEXT("transform"), Transform) || !Transform || !Transform->IsValid())
							return Reject(TEXT("/attachment/transform"), LevelField(*Attachment, TEXT("transform")));
						if (!TransformForLevel(*Transform, false, Target, TEXT("/attachment/transform"), LevelTransform, OutError, bNative)) return false;
						LevelAttachment->SetObjectField(TEXT("transform"), LevelTransform);
					}
					OutExtension->SetObjectField(TEXT("attachment"), LevelAttachment);
				}
			}
			return true;
		}

	}

	static FString AllocateLevelActorManagedId(const FString& WorldPath, const FString& Key, TSet<FString>& Reserved)
	{
		const FString Material = WorldPath + TEXT("|") + Key;
		const FTCHARToUTF8 Utf8(*Material);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		const FString Base = TEXT("semantic-") + Sha256(Bytes).Right(20);
		FString Id = Base;
		for (int32 Suffix = 2; Reserved.Contains(Id); ++Suffix) Id = Base + TEXT("-") + FString::FromInt(Suffix);
		Reserved.Add(Id);
		return Id;
	}

	static bool TryBuildLevelSemanticWorkspaceBundleWithSnapshot(
			const FResolvedSemanticWorkspaceRef& Workspace,
			const TArray<FChangeDraft>& Changes,
			TSharedRef<FJsonObject>& OutBundle,
			FWorkspaceError& OutError,
			TFunction<Contract::FSourceLocation(const FString&, const FString&)>* OutSourceResolver,
			const StateSemantic::FUeAgentCanonicalSnapshot* CurrentSnapshot)
		{
			const double BundleStarted = FPlatformTime::Seconds();
			OutBundle = MakeShared<FJsonObject>();
			OutBundle->SetStringField(TEXT("schema"), TEXT("uai.virtual_workspace.v1"));
			TSharedRef<FJsonObject> Units = MakeShared<FJsonObject>();
			TMap<FString, TSharedPtr<FJsonObject>> UnitsByKey;
			TMap<FString, TSharedPtr<FJsonObject>> CapturedExtensionsByKey;
			TMap<FString, TArray<TSharedPtr<FJsonValue>>> PropertiesByKey;
			TMap<FString, TPair<FString, FString>> ActorSourcesByKey;
			TMap<FString, TPair<FString, FString>> SourcesByUnitPath;
			TMap<FString, const StateSemantic::FUeAgentStateEntry*> CurrentNativeActors;
			TArray<StateSemantic::FUeAgentStateEntry> CurrentNativeActorStorage;
			TSet<FString> AmbiguousNativeActors;
			int32 UnchangedNativeActors = 0;
			const auto ActorIndices = CurrentSnapshot
				? CurrentSnapshot->FindSemanticEntryIndices(UeAgentLevelAuthoring::ActorStateSemanticId) : TArray<int32>();
			CurrentNativeActorStorage.Reserve(ActorIndices.Num());
			for (const int32 EntryIndex : ActorIndices)
			{
				const auto& Entry = CurrentNativeActorStorage.Add_GetRef(CurrentSnapshot->ReadEntry(EntryIndex));
				const auto& Identity = Entry.GetIdentity();
				if (Identity.GetSemanticId() != UeAgentLevelAuthoring::ActorStateSemanticId
					|| Identity.GetSelector(TEXT("actor_class")) != TEXT("/Script/Engine.StaticMeshActor")) continue;
				FString Owner = Identity.GetSelector(TEXT("managed_id"));
				if (Owner.IsEmpty()) Owner = Identity.GetSelector(TEXT("actor_guid"));
				if (Owner.IsEmpty()) Owner = Identity.GetSelector(TEXT("object_path"));
				const FString WorldPath = Identity.GetSelector(TEXT("world_path"));
				if (Owner.IsEmpty() || WorldPath.IsEmpty()) continue;
				const FString Key = WorldPath + TEXT("|") + Owner + TEXT("|");
				if (AmbiguousNativeActors.Contains(Key)) continue;
				if (CurrentNativeActors.Contains(Key)) { CurrentNativeActors.Remove(Key); AmbiguousNativeActors.Add(Key); }
				else CurrentNativeActors.Add(Key, &Entry);
			}
			FString WorkspaceWorldPath;
			for (const FString& TargetId : Workspace->GetModel()->GetTargetUnitIds())
			{
				const auto* TargetUnit = Workspace->GetModel()->FindUnit(TargetId);
				if (!TargetUnit) continue;
				const auto& Unit = *TargetUnit;
				if (Unit.GetUnitKind() == ESemanticUnitKind::TargetState)
				{
					const FUeAgentCanonicalValueRef* WorldPath =
						Unit.GetValues().Find(TEXT("world_path"));
					if (WorldPath && (*WorldPath)->GetKind() == EUeAgentValueKind::String)
					{
						WorkspaceWorldPath = (*WorldPath)->GetText();
						break;
					}
				}
			}
			const double BindingsFinished = FPlatformTime::Seconds();
			for (const FChangeDraft& Change : Changes)
			{
				const FUeAgentCanonicalIdentity& Identity = Change.GetIdentity();
				// This synthetic desired-membership row is handled below from the
				// original authored action. It is not a native property identity or
				// a source row to search merely for a possible diagnostic.
				if (Identity.GetTargetKind() == TEXT("authoring_keyed_item_desired_state")) continue;
				const FString AuthoringKey = Identity.GetSelector(TEXT("authoring_target_identity_key"));
				const FSemanticUnit* SourceUnit = nullptr;
				bool bAmbiguousSource = false;
				// The capture's explicit authoring binding is the source identity.
				// Its canonical observation key also carries this observation's
				// selectors/revision and is not a second source namespace to scan.
				const auto SourceUnits = Workspace->GetModel()->FindUnitsForTargetIdentity(
					AuthoringKey.IsEmpty() ? Identity.GetCanonicalKey() : AuthoringKey);
				for (const FSemanticUnit* Unit : SourceUnits)
				{
					if (SourceUnit) { bAmbiguousSource = true; break; }
					SourceUnit = Unit;
				}
				const FString SourceUnitId = !bAmbiguousSource && SourceUnit ? SourceUnit->GetUnitId() : FString();
				ON_SCOPE_EXIT
				{
					if (!OutError.HasErrors()) return;
					auto Diagnostic = OutError.GetDiagnostic();
					SupplementLevelSemanticSource(Workspace, SourceUnitId, TEXT("/value"), Diagnostic);
					OutError.SetFailure(MoveTemp(Diagnostic));
				};
				const TMap<FString, FString>& Selectors = Identity.GetSelectors();
				const FString* PropertyPath = Selectors.Find(TEXT("property_path"));
				const bool bActorState =
					Identity.GetSemanticId() == UeAgentLevelAuthoring::ActorStateSemanticId;
				const bool bComponentState =
					Identity.GetSemanticId() == UeAgentLevelAuthoring::ComponentStateSemanticId;
				const bool bWorldState = Identity.GetSemanticId() == UeAgentLevelAuthoring::WorldStateSemanticId;
				if ((!PropertyPath || PropertyPath->IsEmpty())
					&& !bActorState && !bComponentState && !bWorldState)
				{
					auto Error = LevelNativeFact(TEXT("level_semantic_change_kind_unsupported"),
						TEXT("The semantic change is neither a property update nor a supported actor or component state."),
						TEXT("/change/supported_family"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.SubjectId = Identity.GetSemanticId();
					return Fail(OutError, MoveTemp(Error));
				}
				if (Change.GetIntent() != EChangeIntent::Update)
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_semantic_change_intent_unsupported"),
						TEXT("The semantic state projection requires Update intent."), TEXT("/change/intent"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Change.GetIntent())), MakeShared<FJsonValueNumber>(static_cast<int32>(EChangeIntent::Update))));
				}
				const FString WorldPath = Selectors.FindRef(TEXT("world_path"));
				if (WorkspaceWorldPath.IsEmpty())
				{
					WorkspaceWorldPath = WorldPath;
				}
				else if (WorkspaceWorldPath != WorldPath)
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_semantic_world_mismatch"),
						TEXT("The semantic change targets a different world from the workspace."), TEXT("/change/selectors/world_path"),
						MakeShared<FJsonValueString>(WorldPath), MakeShared<FJsonValueString>(WorkspaceWorldPath), WorldPath));
				}
				FString OwnerId = Selectors.FindRef(TEXT("managed_id"));
				if (OwnerId.IsEmpty())
				{
					OwnerId = Selectors.FindRef(TEXT("owner_managed_id"));
				}
				FString ActorGuid = Selectors.FindRef(TEXT("actor_guid"));
				if (ActorGuid.IsEmpty())
				{
					ActorGuid = Selectors.FindRef(TEXT("owner_actor_guid"));
				}
				const FString ComponentName = Selectors.FindRef(TEXT("component_name"));
				if (OwnerId.IsEmpty() && ActorGuid.IsEmpty()
					&& Selectors.FindRef(TEXT("world_path")).IsEmpty())
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_semantic_owner_selector_missing"),
						TEXT("The semantic change has no managed actor id, actor GUID, or world path selector."), TEXT("/change/selectors"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("managed_id, actor_guid, or world_path"))));
				}
				TSharedPtr<FJsonValue> Value;
				UE::UAI::Contract::FDiagnostic ValueError;
				if (!Change.GetProposedAfter().IsValid())
					return Fail(OutError, LevelNativeFact(TEXT("level_semantic_proposed_value_missing"),
						TEXT("The semantic change has no proposed value."), TEXT("/change/proposed_after"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), WorldPath));
				if (!UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetProposedAfter(), Value, ValueError) || !Value.IsValid())
					return Fail(OutError, ValueError.IsEmpty() ? LevelNativeFact(TEXT("level_semantic_proposed_value_missing"),
						TEXT("Canonical value conversion returned no JSON value."), TEXT("/change/proposed_after"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), WorldPath) : MoveTemp(ValueError));
				const FString EffectiveOwner = OwnerId.IsEmpty() ? ActorGuid : OwnerId;
				const FString UnitKey = WorldPath + TEXT("|")
					+ EffectiveOwner + TEXT("|") + ComponentName;
				TSharedPtr<FJsonObject>& Unit = UnitsByKey.FindOrAdd(UnitKey);
				if (!Unit.IsValid())
				{
					Unit = MakeShared<FJsonObject>();
					Unit->SetStringField(TEXT("operation"), TEXT("update"));
					TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"), WorldPath);
				if (!ComponentName.IsEmpty())
				{
					if (!OwnerId.IsEmpty()) Target->SetStringField(TEXT("owner_managed_id"), OwnerId);
					else Target->SetStringField(TEXT("owner_actor_guid"), ActorGuid);
					Target->SetStringField(TEXT("component_name"), ComponentName);
					Target->SetStringField(TEXT("component_class"), Selectors.FindRef(TEXT("component_class")));
				}
				else if (!OwnerId.IsEmpty())
				{
					Target->SetStringField(TEXT("managed_id"), OwnerId);
				}
				else if (!ActorGuid.IsEmpty())
				{
					Target->SetStringField(TEXT("actor_guid"), ActorGuid);
				}
				Unit->SetObjectField(TEXT("target"), Target);
				}
				if (bWorldState)
				{
					if (Value->Type != EJson::Object) return Fail(OutError, LevelInputFact(
						TEXT("level_semantic_world_state_invalid"), TEXT("World state must be an object."), TEXT("/"), Value,
						MakeShared<FJsonValueString>(TEXT("object")), WorldPath));
					auto Extension = CopyObject(Value->AsObject());
					const TSharedPtr<FJsonObject>* Levels = nullptr;
					if (Extension->TryGetObjectField(TEXT("streaming_levels"), Levels))
						for (const auto& Pair : (*Levels)->Values)
						{
							if (Pair.Value->Type != EJson::Object) continue;
							const auto Row = Pair.Value->AsObject();
							const TSharedPtr<FJsonObject>* Transform = nullptr;
							if (Row->TryGetObjectField(TEXT("transform"), Transform))
							{
								TSharedPtr<FJsonObject> Converted;
								if (!TransformForLevel(*Transform, true, WorldPath, TEXT("/streaming_levels/transform"), Converted, OutError)) return false;
								Row->SetObjectField(TEXT("transform"), Converted);
							}
						}
					Unit->SetObjectField(TEXT("provider_extension"), Extension);
					continue;
				}
				if (bActorState || bComponentState)
				{
					if (Value->Type != EJson::Object || !Value->AsObject().IsValid())
					{
						return Fail(OutError, LevelInputFact(TEXT("level_semantic_provider_extension_invalid"),
							TEXT("The proposed actor or component state must be an object."), TEXT("/"), LevelValue(Value),
							MakeShared<FJsonValueString>(TEXT("object")), EffectiveOwner));
					}
					TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
					if (!TryBuildLevelProviderExtension(Value->AsObject(), bActorState,
						EffectiveOwner, Extension, OutError)) return false;
					if (Change.GetExpectedBefore().IsValid())
					{
						TSharedPtr<FJsonValue> Before;
						TSharedRef<FJsonObject> CapturedExtension = MakeShared<FJsonObject>();
						const bool bConverted = UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*Change.GetExpectedBefore(), Before, ValueError);
						if (!bConverted || !Before.IsValid() || Before->Type != EJson::Object)
							return Fail(OutError, !ValueError.IsEmpty() ? AsLevelNativeResult(MoveTemp(ValueError)) : LevelNativeFact(
								TEXT("level_semantic_provider_baseline_invalid"), TEXT("The captured baseline actor or component state must be an object."),
								TEXT("/change/expected_before"), LevelValue(Before), MakeShared<FJsonValueString>(TEXT("object")), EffectiveOwner));
						if (!TryBuildLevelProviderExtension(Before->AsObject(), bActorState,
							EffectiveOwner, CapturedExtension, OutError, true)) return false;
						CapturedExtensionsByKey.Add(UnitKey, CapturedExtension);
					}
					Unit->SetObjectField(TEXT("provider_extension"), Extension);
					continue;
				}
				TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
				Property->SetStringField(TEXT("property_path"), *PropertyPath);
				Property->SetField(TEXT("value"), Value);
				const FString SourceKey = Identity.GetSelector(TEXT("authoring_target_identity_key"));
				UE::UAI::Composition::HjsonImport::AttachSemanticPropertySource(
					Workspace, SourceKey.IsEmpty() ? Identity.GetCanonicalKey() : SourceKey, *Property);
				PropertiesByKey.FindOrAdd(UnitKey).Add(
					MakeShared<FJsonValueObject>(Property));
			}
			const double PropertiesFinished = FPlatformTime::Seconds();
			TArray<FSemanticUnit> AuthoredUnits;
			if (Workspace->GetEditedUnitIds().IsSet()) for (const FString& Id : *Workspace->GetEditedUnitIds())
				if (const auto* Unit = Workspace->GetModel()->FindUnit(Id)) AuthoredUnits.Add(*Unit);
			AuthoredUnits.Sort([](const FSemanticUnit& A, const FSemanticUnit& B) { return A.GetUnitId() < B.GetUnitId(); });
			for (const FSemanticUnit& Unit : AuthoredUnits)
			{
				const TOptional<TArray<FString>>& EditedUnitIds =
					Workspace->GetEditedUnitIds();
				const FUeAgentCanonicalValueRef* Actors = Unit.GetValues().Find(TEXT("actors"));
				if (Unit.GetUnitKind() != ESemanticUnitKind::StructureState
					|| !EditedUnitIds.IsSet()
					|| !EditedUnitIds->Contains(Unit.GetUnitId())
					|| Actors == nullptr
					|| (*Actors)->GetKind() != EUeAgentValueKind::Object)
				{
					continue;
				}
				TSet<FString> ReservedManagedIds;
				for (const auto& ActorRow : (*Actors)->GetObjectItems())
				{
					if (ActorRow.Value->GetKind() != EUeAgentValueKind::Object)
					{
						continue;
					}
					for (const auto& Field : ActorRow.Value->GetObjectItems())
					{
						if (Field.Key == TEXT("managed_id")
							&& Field.Value->GetKind() == EUeAgentValueKind::String)
						{
							ReservedManagedIds.Add(Field.Value->GetText());
						}
					}
				}
				for (const auto& ActorRow : (*Actors)->GetObjectItems())
				{
					const FString RowPointer = TEXT("/actors/") + EscapeJsonPointerSegment(ActorRow.Key);
					ON_SCOPE_EXIT
					{
						if (!OutError.HasErrors()) return;
						auto Diagnostic = OutError.GetDiagnostic();
						SupplementLevelSemanticSource(Workspace, Unit.GetUnitId(), RowPointer, Diagnostic);
						OutError.SetFailure(MoveTemp(Diagnostic));
					};
					TSharedPtr<FJsonValue> RowValue;
					UE::UAI::Contract::FDiagnostic JsonError;
					if (ActorRow.Value->GetKind() != EUeAgentValueKind::Object)
						return Fail(OutError, LevelInputFact(TEXT("level_semantic_actor_row_invalid"), TEXT("The authored actor row must be an object."),
							TEXT("/"), MakeShared<FJsonValueNumber>(static_cast<int32>(ActorRow.Value->GetKind())), MakeShared<FJsonValueString>(TEXT("object")), ActorRow.Key));
					if (!UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(ActorRow.Value.Get(), RowValue, JsonError) || !RowValue.IsValid() || RowValue->Type != EJson::Object)
						return Fail(OutError, JsonError.IsEmpty() ? LevelInputFact(TEXT("level_semantic_actor_row_invalid"),
							TEXT("Actor row conversion did not return an object."), TEXT("/"), LevelValue(RowValue), MakeShared<FJsonValueString>(TEXT("object")), ActorRow.Key) : MoveTemp(JsonError));
					const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
					FString Operation;
					if (!Row->TryGetStringField(TEXT("operation"), Operation))
					{
						return Fail(OutError, LevelInputFact(TEXT("level_semantic_actor_operation_missing"), TEXT("Actor operation must be a string."),
							TEXT("/operation"), LevelField(Row, TEXT("operation")), MakeShared<FJsonValueString>(TEXT("string")), ActorRow.Key));
					}
					TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"), WorkspaceWorldPath);
					for (const TCHAR* Field : {
						TEXT("actor_guid"), TEXT("managed_id"), TEXT("object_path")})
					{
						FString Value;
						if (Row->TryGetStringField(Field, Value) && !Value.IsEmpty())
						{
							Target->SetStringField(Field, Value);
						}
					}
					if (Operation == TEXT("create"))
					{
						FString ActorClass;
						if (Row->TryGetStringField(TEXT("actor_class"), ActorClass)
							&& !ActorClass.IsEmpty())
						{
							Target->SetStringField(TEXT("actor_class"), ActorClass);
						}
					}
					if (Operation == TEXT("create")
						&& !Target->HasField(TEXT("actor_guid"))
						&& !Target->HasField(TEXT("managed_id"))
						&& !Target->HasField(TEXT("object_path")))
					{
						Target->SetStringField(TEXT("managed_id"), AllocateLevelActorManagedId(WorkspaceWorldPath, ActorRow.Key, ReservedManagedIds));
					}
					FString ManagedId;
					FString ActorGuid;
					FString ObjectPath;
					Target->TryGetStringField(TEXT("managed_id"), ManagedId);
					Target->TryGetStringField(TEXT("actor_guid"), ActorGuid);
					Target->TryGetStringField(TEXT("object_path"), ObjectPath);
					const FString EffectiveOwner = !ManagedId.IsEmpty()
						? ManagedId : !ActorGuid.IsEmpty() ? ActorGuid : ObjectPath;
					const FString UnitKey = WorkspaceWorldPath + TEXT("|")
						+ EffectiveOwner + TEXT("|");
					ActorSourcesByKey.Add(UnitKey, {Unit.GetUnitId(), TEXT("/actors/")
						+ ActorRow.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))});
					if (Operation == TEXT("remove")
						&& (!PropertiesByKey.FindRef(UnitKey).IsEmpty()
							|| CapturedExtensionsByKey.Contains(UnitKey)))
					{
						return Fail(OutError, LevelInputFact(TEXT("level_semantic_actor_remove_property_conflict"),
							TEXT("Removing an actor conflicts with state or property edits to the same actor."), TEXT("/operation"),
							MakeShared<FJsonValueString>(Operation), MakeShared<FJsonValueString>(TEXT("operation compatible with actor edits")), EffectiveOwner));
					}
					TSharedPtr<FJsonObject>& MergedUnit = UnitsByKey.FindOrAdd(UnitKey);
					const bool bHadExistingUnit = MergedUnit.IsValid();
					if (bHadExistingUnit)
					{
						FString ExistingOperation;
						MergedUnit->TryGetStringField(TEXT("operation"), ExistingOperation);
						if (ExistingOperation != TEXT("update") || Operation == TEXT("create"))
						{
							auto ActualOperations = MakeShared<FJsonObject>();
							ActualOperations->SetStringField(TEXT("incoming_operation"), Operation);
							ActualOperations->SetStringField(TEXT("existing_operation"), ExistingOperation);
							return Fail(OutError, LevelInputFact(TEXT("level_semantic_actor_operation_conflict"),
								TEXT("The actor operation conflicts with an operation already projected for the same actor."), TEXT("/operation"),
								MakeShared<FJsonValueObject>(ActualOperations), MakeShared<FJsonValueString>(TEXT("existing update without a new create")), EffectiveOwner));
						}
					}
					else
					{
						MergedUnit = MakeShared<FJsonObject>();
					}
					MergedUnit->SetStringField(TEXT("operation"), Operation);
					MergedUnit->SetObjectField(TEXT("target"), Target);
					if (Operation != TEXT("remove"))
					{
						TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
						for (const TCHAR* Field : {
							TEXT("candidate"), TEXT("label"), TEXT("folder_path"),
							TEXT("tags"), TEXT("attachment"), TEXT("level"),
							TEXT("placement_context")})
						{
							if (const TSharedPtr<FJsonValue> Value = Row->TryGetField(Field);
								Value.IsValid())
							{
								Extension->SetField(Field, Value);
							}
						}
						if (Row->HasField(TEXT("transform")))
						{
							const TSharedPtr<FJsonObject>* Transform = nullptr;
							TSharedPtr<FJsonObject> LevelTransform;
							if (!Row->TryGetObjectField(TEXT("transform"), Transform)
								|| !Transform || !Transform->IsValid()
								|| !TransformForLevel(*Transform, true, ActorRow.Key,
									TEXT("/transform"), LevelTransform, OutError))
							{
								if (OutError.GetCode().IsEmpty())
								{
									return Fail(OutError, LevelInputFact(TEXT("level_semantic_actor_transform_invalid"), TEXT("Actor transform must be an object."),
										TEXT("/transform"), LevelField(Row, TEXT("transform")), MakeShared<FJsonValueString>(TEXT("object")), EffectiveOwner));
								}
								return false;
							}
							Extension->SetObjectField(TEXT("transform"), LevelTransform);
						}
						// The public decimal spelling can differ while both values map
						// to the same native double. Do not prepare/mutate an unchanged
						// native Actor merely because the aggregate document was edited.
						// The proof is the complete actual current snapshot, not edit IDs.
						const auto* PropertyEdits = PropertiesByKey.Find(UnitKey);
						if (Operation == TEXT("update") && !bHadExistingUnit
							&& (!PropertyEdits || PropertyEdits->IsEmpty()))
						if (const auto* Actual = CurrentNativeActors.Find(UnitKey))
						{
							bool bSameTarget = true;
							for (const auto& Selector : Target->Values)
							{
								FString Expected;
								if (!Selector.Value || !Selector.Value->TryGetString(Expected)
									|| !(*Actual)->GetIdentity().GetSelector(Selector.Key).Equals(Expected, ESearchCase::CaseSensitive))
								{ bSameTarget = false; break; }
							}
							TSharedPtr<FJsonValue> CurrentValue;
							UE::UAI::Contract::FDiagnostic CurrentError;
							TSharedRef<FJsonObject> CurrentExtension = MakeShared<FJsonObject>();
							FWorkspaceError ProofError;
							if (bSameTarget && StateSemantic::CanonicalJson::TryToJsonValue((*Actual)->GetValue().Get(), CurrentValue, CurrentError)
								&& CurrentValue && CurrentValue->Type == EJson::Object
								&& TryBuildLevelProviderExtension(CurrentValue->AsObject(), true, EffectiveOwner,
									CurrentExtension, ProofError, true))
							{
								const TSharedPtr<FJsonObject>* Attachment = nullptr;
								bool bUnattached = CurrentExtension->TryGetObjectField(TEXT("attachment"), Attachment)
									&& Attachment && Attachment->IsValid();
								if (bUnattached) for (const auto& Field : (*Attachment)->Values)
									bUnattached &= (Field.Key == TEXT("parent_managed_id") || Field.Key == TEXT("parent_actor_guid")
										|| Field.Key == TEXT("parent_object_path") || Field.Key == TEXT("socket_name"))
										&& Field.Value && Field.Value->Type == EJson::String && Field.Value->AsString().IsEmpty();
							// Actor labels are FString text (SetActorLabel uses Strcmp),
							// unlike native FName/tag identity. JSON CompareEqual uses
							// FString's IgnoreCase operator and cannot prove this field.
							FString CurrentLabel, DesiredLabel;
							const bool bCurrentLabel = CurrentExtension->TryGetStringField(TEXT("label"), CurrentLabel);
							const bool bDesiredLabel = Extension->TryGetStringField(TEXT("label"), DesiredLabel);
							const bool bSameLabel = bCurrentLabel == bDesiredLabel
								&& (!bCurrentLabel || CurrentLabel.Equals(DesiredLabel, ESearchCase::CaseSensitive));
							if (bUnattached && bSameLabel && FJsonValue::CompareEqual(FJsonValueObject(CurrentExtension), FJsonValueObject(Extension)))
								{
									UnitsByKey.Remove(UnitKey);
									ActorSourcesByKey.Remove(UnitKey);
									++UnchangedNativeActors;
									continue;
								}
							}
						}
						const TSharedPtr<FJsonObject>* ExistingExtension = nullptr;
						if (MergedUnit->TryGetObjectField(TEXT("provider_extension"), ExistingExtension)
							&& ExistingExtension && ExistingExtension->IsValid())
						{
							TSharedPtr<FJsonObject> MergedExtension;
							UE::UAI::Contract::FDiagnostic Conflict;
							if (!TryMergeKeyedItemStateEdits(CapturedExtensionsByKey.FindRef(UnitKey),
								*ExistingExtension, Extension, MergedExtension, Conflict))
							{
								const FString Pointer = TEXT("/actors/")
									+ EscapeJsonPointerSegment(ActorRow.Key) + Conflict.Source.InternalJsonPath;
								Conflict.Source.InternalJsonPath = Pointer;
								Conflict.CandidateName = ActorRow.Key;
								OutError.SetFailure(MoveTemp(Conflict), Unit.GetUnitId(), Pointer);
								if (const auto* Source = Workspace->FindValueSource(Unit.GetUnitId(), Pointer)) OutError.Locate(*Source);
								else if (const auto* UnitSource = Workspace->GetUnitSources().Find(Unit.GetUnitId())) OutError.Locate(*UnitSource);
								return false;
							}
							MergedUnit->SetObjectField(TEXT("provider_extension"), MergedExtension);
						}
						else MergedUnit->SetObjectField(TEXT("provider_extension"), Extension);
					}
				}
			}
			// Component membership is authored in an Actor-owned collection. Existing
			// component state/property documents keep their original native bindings.
			const double ActorsFinished = FPlatformTime::Seconds();
			for (const FSemanticUnit& Collection : AuthoredUnits)
			{
				if (Collection.GetRole() != TEXT("level_components")
					|| !Workspace->GetEditedUnitIds().IsSet()
					|| !Workspace->GetEditedUnitIds()->Contains(Collection.GetUnitId())) continue;
				const auto* Owner = Workspace->GetModel()->FindUnit(Collection.GetOwnerRef());
				const auto* Components = Collection.GetValues().Find(TEXT("components"));
				if (!Owner || !Components || (*Components)->GetKind() != EUeAgentValueKind::Object)
					return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_collection_invalid"),
						TEXT("The Level component collection has no captured Actor owner or component map."), TEXT("component_project")));
				auto OwnerString = [Owner](const TCHAR* Field) {
					const auto* Value = Owner->GetValues().Find(Field);
					return Value && (*Value)->GetKind() == EUeAgentValueKind::String ? (*Value)->GetText() : FString();
				};
				const FString ManagedId = OwnerString(TEXT("managed_id"));
				const FString ActorGuid = OwnerString(TEXT("actor_guid"));
				const FString OwnerKey = WorkspaceWorldPath + TEXT("|") + (ManagedId.IsEmpty() ? ActorGuid : ManagedId) + TEXT("|");
				for (const auto& ComponentRow : (*Components)->GetObjectItems())
				{
					const FString Pointer = TEXT("/components/") + EscapeJsonPointerSegment(ComponentRow.Key);
					ON_SCOPE_EXIT {
						if (!OutError.HasErrors()) return;
						auto Error = OutError.GetDiagnostic();
						SupplementLevelSemanticSource(Workspace, Collection.GetUnitId(), Pointer, Error);
						OutError.SetFailure(MoveTemp(Error));
					};
					Contract::FDiagnostic Error;
					TSharedPtr<FJsonValue> Value;
					if (!StateSemantic::CanonicalJson::TryToJsonValue(ComponentRow.Value.Get(), Value, Error)) return Fail(OutError, Error);
					if (!Value.IsValid() || Value->Type != EJson::Object)
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_row_invalid"), TEXT("Each component row must be an object."), TEXT("component_project")));
					const auto Row = Value->AsObject();
					if (Row->HasField(TEXT("parent")))
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_parent_shape_invalid"),
							TEXT("Level components use attachment.parent_component; parent belongs to the Blueprint component tree."), TEXT("component_project")));
					FString Operation, ComponentName, ClassPath, AssetOverride;
					Row->TryGetStringField(TEXT("operation"), Operation);
					Row->TryGetStringField(TEXT("component_name"), ComponentName);
					Row->TryGetStringField(TEXT("component_class"), ClassPath);
					if (Operation == TEXT("create"))
					{
						ComponentName = ComponentRow.Key;
						FText NameError;
						if (ComponentName.IsEmpty() || !FName::IsValidXName(ComponentName, INVALID_OBJECTNAME_CHARACTERS, &NameError))
							return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_name_invalid"),
								TEXT("The component map key must be a valid UE object name: ") + NameError.ToString(), TEXT("component_project")));
						const TSharedPtr<FJsonObject>* Candidate = nullptr;
						if (!Row->TryGetObjectField(TEXT("candidate"), Candidate))
							return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_candidate_required"), TEXT("A new component requires a native candidate name."), TEXT("component_project")));
						if (!UeAgentLevelAuthoring::ResolveComponentCreationCandidate(*Candidate, ClassPath, AssetOverride, Error)) return Fail(OutError, Error);
					}
					else if (Operation != TEXT("update") && Operation != TEXT("remove"))
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_operation_invalid"), TEXT("Expected a projected component create, update or remove."), TEXT("component_project")));
					FString AuthoredClass;
					if (Row->TryGetStringField(TEXT("class"), AuthoredClass) && AuthoredClass != ClassPath)
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_class_conflict"), TEXT("class is captured context and must match the selected component class; replace membership to change type."), TEXT("component_project")));
					const FString Key = OwnerKey + ComponentName;
					if (const auto ActorOperation = UnitsByKey.FindRef(OwnerKey); ActorOperation.IsValid()
						&& ActorOperation->GetStringField(TEXT("operation")) == TEXT("remove"))
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_owner_remove_conflict"), TEXT("Component edits conflict with removal of their owning Actor."), TEXT("component_project")));
					if (Operation == TEXT("remove") && UnitsByKey.Contains(Key))
						return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_remove_edit_conflict"), TEXT("Removing a component conflicts with state or property edits to that component."), TEXT("component_project")));
					auto Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"), WorkspaceWorldPath);
					Target->SetStringField(ManagedId.IsEmpty() ? TEXT("owner_actor_guid") : TEXT("owner_managed_id"), ManagedId.IsEmpty() ? ActorGuid : ManagedId);
					Target->SetStringField(TEXT("component_name"), ComponentName);
					Target->SetStringField(TEXT("component_class"), ClassPath);
					auto& Projected = UnitsByKey.FindOrAdd(Key);
					if (!Projected.IsValid()) Projected = MakeShared<FJsonObject>();
					Projected->SetStringField(TEXT("operation"), Operation);
					Projected->SetObjectField(TEXT("target"), Target);
					ActorSourcesByKey.Add(Key, {Collection.GetUnitId(), Pointer});
					if (Operation == TEXT("remove")) continue;
					auto Extension = MakeShared<FJsonObject>();
					if (!TryBuildLevelProviderExtension(Row, false, ComponentName, Extension, OutError)) return false;
					if (!AssetOverride.IsEmpty()) Extension->SetStringField(TEXT("asset_override"), AssetOverride);
					const TSharedPtr<FJsonObject>* ExistingExtension = nullptr;
					if (Projected->TryGetObjectField(TEXT("provider_extension"), ExistingExtension))
					{
						TSharedPtr<FJsonObject> Merged;
						if (!TryMergeKeyedItemStateEdits(CapturedExtensionsByKey.FindRef(Key), *ExistingExtension, Extension, Merged, Error)) return Fail(OutError, Error);
						Projected->SetObjectField(TEXT("provider_extension"), Merged);
					}
					else Projected->SetObjectField(TEXT("provider_extension"), Extension);
					if (Row->HasField(TEXT("properties")))
					{
						if (Operation != TEXT("create"))
							return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_initial_properties_only"), TEXT("Use the component's properties.hjson for existing property edits; inline properties initialize new components."), TEXT("component_project")));
						TSharedPtr<FJsonObject> Document;
						TArray<TSharedPtr<FJsonValue>> Properties;
						if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Row, TEXT("properties"), Document, Error)
							|| !UeAgentDetailsAuthoringService::DecodePropertiesDocument(Document, Properties, Error)) return Fail(OutError, Error);
						PropertiesByKey.FindOrAdd(Key).Append(Properties);
					}
				}
			}
			// A separate component property file may be edited without its collection.
			for (const auto& Pair : UnitsByKey)
			{
				const auto Target = Pair.Value->GetObjectField(TEXT("target"));
				FString ComponentName;
				if (!Target->TryGetStringField(TEXT("component_name"), ComponentName)) continue;
				const FString OwnerKey = Pair.Key.LeftChop(ComponentName.Len());
				if (const auto Owner = UnitsByKey.FindRef(OwnerKey); Owner.IsValid() && Owner->GetStringField(TEXT("operation")) == TEXT("remove"))
					return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("level_component_owner_remove_conflict"), TEXT("Component edits conflict with removal of their owning Actor."), TEXT("component_project")));
			}
			if (WorkspaceWorldPath.IsEmpty())
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_semantic_world_path_missing"),
					TEXT("No world path was available from the workspace target state or projected changes."), TEXT("/workspace/world_path"),
					MakeShared<FJsonValueString>(WorkspaceWorldPath), MakeShared<FJsonValueString>(TEXT("non-empty world path"))));
			}
			TSharedRef<FJsonObject> WorldUnit = MakeShared<FJsonObject>();
			WorldUnit->SetStringField(TEXT("operation"), TEXT("update"));
			TSharedRef<FJsonObject> WorldTarget = MakeShared<FJsonObject>();
			WorldTarget->SetStringField(TEXT("world_path"), WorkspaceWorldPath);
			WorldUnit->SetObjectField(TEXT("target"), WorldTarget);
			Units->SetObjectField(TEXT("index.hjson"), WorldUnit);
			int32 ActorIndex = 0;
			for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : UnitsByKey)
			{
				const TArray<TSharedPtr<FJsonValue>> Properties =
					PropertiesByKey.FindRef(Pair.Key);
				if (!Properties.IsEmpty())
				{
					Pair.Value->SetArrayField(TEXT("properties"), Properties);
				}
				if (Pair.Key == WorkspaceWorldPath + TEXT("||"))
				{
					// WorldSettings belong to the world operation, not an Actor
					// operation with missing Actor identity selectors.
					Units->SetObjectField(TEXT("index.hjson"), Pair.Value.ToSharedRef());
					continue;
				}
				const TSharedPtr<FJsonObject>* Target = nullptr;
				FString ComponentName;
				if (Pair.Value->TryGetObjectField(TEXT("target"), Target)
					&& Target && Target->IsValid())
				{
					(*Target)->TryGetStringField(
						TEXT("component_name"),
						ComponentName);
				}
				if (Workspace->GetModel()->GetProfileId() == UeAgentLevelAuthoring::SceneEditingProfileId)
				{
					for (const auto& Property : Properties)
					{
						const TSharedPtr<FJsonObject>* Row = nullptr;
						FString PropertyPath;
						if (!Property.IsValid() || !Property->TryGetObject(Row) || !Row->IsValid()
							|| !(*Row)->TryGetStringField(TEXT("property_path"), PropertyPath)) continue;
						if (UeAgentLevelAuthoring::IsSceneEditingProperty(PropertyPath, !ComponentName.IsEmpty())
							|| UeAgentLevelAuthoring::RequiresSceneEditingClassCheck(PropertyPath, !ComponentName.IsEmpty())) continue;
						auto Error = Contract::FDiagnostic::Failure(TEXT("level_scene_editing_property_unsupported"),
							TEXT("This Actor/Component property is outside scene_editing; use a full llm_authoring workspace to edit it."), TEXT("level_profile"));
						Error.PropertyPath = PropertyPath;
						Error.ActualValue = MakeShared<FJsonValueString>(PropertyPath);
						if (const auto* Source = ActorSourcesByKey.Find(Pair.Key))
							SupplementLevelSemanticSource(Workspace, Source->Key, Source->Value + TEXT("/properties"), Error);
						return Fail(OutError, MoveTemp(Error));
					}
					const TSharedPtr<FJsonObject>* Extension = nullptr;
					if (!ComponentName.IsEmpty() && Pair.Value->TryGetObjectField(TEXT("provider_extension"), Extension)
						&& (*Extension)->HasField(TEXT("pcg")))
					{
						auto Error = Contract::FDiagnostic::Failure(TEXT("level_scene_editing_state_unsupported"),
							TEXT("PCG authoring state is outside scene_editing; use a full llm_authoring workspace to edit it."), TEXT("level_profile"));
						Error.PropertyPath = TEXT("pcg");
						if (const auto* Source = ActorSourcesByKey.Find(Pair.Key))
							SupplementLevelSemanticSource(Workspace, Source->Key, Source->Value + TEXT("/pcg"), Error);
						return Fail(OutError, MoveTemp(Error));
					}
				}
				const FString UnitPath = ComponentName.IsEmpty()
					? FString::Printf(
						TEXT("actors/items/actor_%04d/actor.hjson"),
						ActorIndex++)
					: FString::Printf(
						TEXT("actors/items/actor_%04d/components/component_0000/component.hjson"),
						ActorIndex++);
				Units->SetObjectField(UnitPath, Pair.Value.ToSharedRef());
				if (const auto* Source = ActorSourcesByKey.Find(Pair.Key)) SourcesByUnitPath.Add(UnitPath, *Source);
			}
			const double ComponentsFinished = FPlatformTime::Seconds();
			OutBundle->SetObjectField(TEXT("units"), Units);
			if (OutSourceResolver)
			{
				*OutSourceResolver = [Workspace, Bindings = MoveTemp(SourcesByUnitPath)](const FString& UnitPath, const FString& Pointer)
				{
					Contract::FSourceLocation Location;
					const auto* Binding = Bindings.Find(UnitPath);
					if (!Binding) return Location;
					FString Relative = Pointer;
					// Captured property edits carry their own exact source. Initial
					// component properties instead originate in this collection row.
					if (Relative.StartsWith(TEXT("/properties/")) && !Binding->Value.StartsWith(TEXT("/components/"))) return Location;
					Relative.RemoveFromStart(TEXT("/provider_extension"));
					Relative.RemoveFromStart(TEXT("/target"));
					FString SourcePointer = Binding->Value + (Relative.IsEmpty() || Relative == TEXT("/") ? FString() : Relative);
					for (;;)
					{
						if (const auto* Source = Workspace->FindValueSource(Binding->Key, SourcePointer))
						{
							FWorkspaceError Located;
							Located.Locate(*Source);
							return Located.GetDiagnostic().Source;
						}
						if (SourcePointer == Binding->Value) break;
						int32 Slash;
						if (!SourcePointer.FindLastChar(TEXT('/'), Slash) || Slash < Binding->Value.Len()) break;
						SourcePointer.LeftInline(Slash);
					}
					// A removed captured row has no authored node. Point at the
					// collection file rather than leaking the generated native bundle.
					if (const auto* Source = Workspace->GetUnitSources().Find(Binding->Key))
					{
						FWorkspaceError Located;
						Located.Locate(*Source);
						Location = Located.GetDiagnostic().Source;
						Location.JsonPath = Binding->Value;
					}
					return Location;
				};
			}
			UE_LOG(LogTemp, Display, TEXT("UAI import projection unchanged_native_actors=%d"), UnchangedNativeActors);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_bundle changes=%d bindings_s=%.6f properties_s=%.6f actors_s=%.6f components_and_units_s=%.6f source_binding_s=%.6f total_s=%.6f"),
				Changes.Num(), BindingsFinished - BundleStarted, PropertiesFinished - BindingsFinished,
				ActorsFinished - PropertiesFinished, ComponentsFinished - ActorsFinished,
				FPlatformTime::Seconds() - ComponentsFinished, FPlatformTime::Seconds() - BundleStarted);
			return true;
		}

	bool TryBuildLevelSemanticWorkspaceBundle(
		const FResolvedSemanticWorkspaceRef& Workspace, const TArray<FChangeDraft>& Changes,
		TSharedRef<FJsonObject>& OutBundle, FWorkspaceError& OutError,
		TFunction<Contract::FSourceLocation(const FString&, const FString&)>* OutSourceResolver)
	{
		return TryBuildLevelSemanticWorkspaceBundleWithSnapshot(Workspace, Changes, OutBundle, OutError, OutSourceResolver, nullptr);
	}

	static TMap<FGuid, AActor*> ObserveLevelActorMembership(UWorld& World)
	{
		TMap<FGuid, AActor*> Result;
		for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
			if (UeAgentLevelAuthoring::EvaluateActorAuthorability(World, **It).bAuthorable)
				Result.Add(It->GetActorGuid(), *It);
		return Result;
	}

	static TOptional<bool> TryCaptureScopedLevelPreflight(UWorld& World, const FString& WorkspaceRoot,
		const FString& ProfileId, UeAgentLevelAuthoring::FLevelAuthoringCapture& Capture,
		Composition::HjsonImport::FScopedCurrentCoverage& Coverage, FWorkspaceError& Error,
		const FResolvedSemanticWorkspaceRef& Source, TArray<UeAgentLevelAuthoring::FLevelActorCaptureScope>& Scope)
	{
		using namespace UeAgentLevelAuthoring;
		using namespace UeAgentAuthoringState;
		const auto Missing = [&](const TCHAR* Reason) -> TOptional<bool> {
			Error.SetSourceFailure(TEXT("level_semantic_capture_incomplete"),
				FString::Printf(TEXT("The addressed Level fact is unavailable (%s). No unrelated World state was captured."), Reason), WorkspaceRoot);
			return false;
		};
		if (!Source->GetEditedUnitIds().IsSet()) return Missing(TEXT("source_edit_scope_missing"));
		if (Source->GetModel()->GetProfileId() != ProfileId) {
			Error.SetSourceFailure(TEXT("workspace_incremental_profile_mismatch"),
				TEXT("Level preparation requires the authored workspace profile."), WorkspaceRoot);
			return false;
		}
		if (!Source->GetStructuralEdits().IsEmpty()) {
			// This representation is not a Level native command. The original
			// lowering rejects it as well; a complete World cannot implement it.
			Error.SetSourceFailure(TEXT("level_semantic_change_kind_unsupported"),
				TEXT("Level creation/removal uses the actors/components keyed operation records; generic structural_edits is not a Level native change."), WorkspaceRoot);
			return false;
		}
		// World streaming topology is a genuine complete-domain action. It is
		// the only full observation selection, never a missing-cache fallback.
		for (const auto& Id : *Source->GetEditedUnitIds())
			if (const auto Unit = Source->GetModel()->FindDeclaration(Id);
				Unit && Unit->GetSemanticTypeRef() == WorldStateSemanticId) {
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_scoped_preflight selected=false reason=explicit_world_topology_action"));
				return {};
			}
		const double Started = FPlatformTime::Seconds();
		TSet<FString> WorldPropertyNames;
		TMap<AActor*, FLevelActorCaptureScope> ActorScopes;
		TArray<FLevelActorCaptureScope> PlannedActors;
		const auto IncludeActor = [&](AActor* Actor) -> FLevelActorCaptureScope& {
			auto& Scope = ActorScopes.FindOrAdd(Actor);
			Scope.ActorGuid = Actor->GetActorGuid();
			Scope.ManagedId = GetManagedActorId(*Actor);
			// The keyed Actor declaration consumes label/attachment metadata.
			// This is one state request, not the Actor's reflected properties.
			Scope.bActorState = true;
			return Scope;
		};
		const auto IncludeCompleteActor = [&](AActor* Actor) {
			auto& Scope = IncludeActor(Actor);
			Scope.bActorState = Scope.bActorProperties = Scope.bAllComponents = true;
		};
		const auto IncludeComponent = [&](UActorComponent* Component) {
			auto& Scope = IncludeActor(Component->GetOwner());
			Scope.ComponentNames.Add(Component->GetName());
			// A custom/Blueprint component can rebuild its owning Actor. Keep
			// that actual owner closure; its module is not a World-wide scope.
			if (Component->GetClass()->GetOutermost()->GetName() != TEXT("/Script/Engine")
				|| Component->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
				Scope.bActorProperties = Scope.bAllComponents = true;
		};
		const auto IncludeAttachmentClosure = [&](USceneComponent* Component) {
			if (!Component) return;
			for (USceneComponent* Parent = Component; Parent; Parent = Parent->GetAttachParent()) {
				IncludeComponent(Parent);
				if (Parent == Parent->GetOwner()->GetRootComponent()) IncludeActor(Parent->GetOwner()).bActorState = true;
			}
			TArray<USceneComponent*> Children;
			Component->GetChildrenComponents(true, Children);
			for (USceneComponent* Child : Children) {
				IncludeComponent(Child);
				if (Child == Child->GetOwner()->GetRootComponent()) IncludeActor(Child->GetOwner()).bActorState = true;
			}
		};
		TMap<FString, AActor*> ActorsByPath;
		TMap<FString, AActor*> ActorsByManagedId;
		const auto ActorMembership = ObserveLevelActorMembership(World);
		for (const auto& Pair : ActorMembership) {
			ActorsByPath.Add(Pair.Value->GetPathName(), Pair.Value);
			const FString ManagedId = GetManagedActorId(*Pair.Value);
			if (!ManagedId.IsEmpty()) ActorsByManagedId.Add(ManagedId, Pair.Value);
		}
		TSet<FString> RemovedObjectPaths;
		TArray<const UObject*> RemovedObjects;
		int32 ActorOperationRows = 0;
		const auto IncludeReferencedActors = [&](const FSemanticUnit& Unit) {
			TArray<StateSemantic::FUeAgentCanonicalValueRef> Values;
			for (const auto& Pair : Unit.GetValues()) Values.Add(Pair.Value);
			while (!Values.IsEmpty()) {
				const auto Value = Values.Pop(EAllowShrinking::No);
				if (Value->GetKind() == StateSemantic::EUeAgentValueKind::String) {
					if (AActor* const* Referenced = ActorsByPath.Find(Value->GetText())) IncludeActor(*Referenced);
					if (AActor* const* Referenced = ActorsByManagedId.Find(Value->GetText())) IncludeActor(*Referenced);
					if (Value->GetText().StartsWith(GetWorldPath(World) + TEXT(":"), ESearchCase::CaseSensitive))
						if (UActorComponent* Referenced = FindObject<UActorComponent>(nullptr, *Value->GetText());
							Referenced && Referenced->GetWorld() == &World && EvaluateComponentAuthorability(World, *Referenced).bAuthorable)
							IncludeComponent(Referenced);
				} else if (Value->GetKind() == StateSemantic::EUeAgentValueKind::Array) {
					Values.Append(Value->GetArrayItems());
				} else if (Value->GetKind() == StateSemantic::EUeAgentValueKind::Object) {
					for (const auto& Pair : Value->GetObjectItems()) Values.Add(Pair.Value);
				}
			}
		};
		Contract::FDiagnostic Diagnostic;
		for (const auto& Id : *Source->GetEditedUnitIds()) {
			const auto* Unit = Source->GetModel()->FindUnit(Id);
			if (Unit && Unit->GetRole() == TEXT("actor_component_hierarchy")) {
				const auto* Rows = Unit->GetValues().Find(TEXT("actors"));
				if (!Rows || (*Rows)->GetKind() != StateSemantic::EUeAgentValueKind::Object) return Missing(TEXT("actor_collection_shape_changed"));
				TSet<FString> ReservedManagedIds;
				const auto StringField = [](const StateSemantic::FUeAgentCanonicalValue& Row, const TCHAR* Key) {
					const auto* Value = Row.GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Key; });
					return Value && Value->Value->GetKind() == StateSemantic::EUeAgentValueKind::String ? Value->Value->GetText() : FString();
				};
				for (const auto& Row : (*Rows)->GetObjectItems())
					if (Row.Value->GetKind() == StateSemantic::EUeAgentValueKind::Object) {
						const FString ManagedId = StringField(Row.Value.Get(), TEXT("managed_id"));
						if (!ManagedId.IsEmpty()) ReservedManagedIds.Add(ManagedId);
					}
				auto& KeyScope = Coverage.KeyedValueScopes.FindOrAdd(Id);
				KeyScope.RootField = TEXT("actors");
				for (const auto& Row : (*Rows)->GetObjectItems()) {
					if (Row.Value->GetKind() != StateSemantic::EUeAgentValueKind::Object) return Missing(TEXT("actor_operation_shape_changed"));
					// The Source keyed owner has already emitted mutation records,
					// omitting unchanged updates. Do not compare that result again.
					++ActorOperationRows;
					const FString Operation = StringField(Row.Value.Get(), TEXT("operation"));
					KeyScope.ObservedKeys.Add(Row.Key);
					if (Operation == TEXT("create")) {
						FLevelActorCaptureScope New;
						New.ManagedId = StringField(Row.Value.Get(), TEXT("managed_id"));
						const FString GuidText = StringField(Row.Value.Get(), TEXT("actor_guid"));
						FGuid RequestedGuid;
						if (!GuidText.IsEmpty()) {
							if (!FGuid::Parse(GuidText, RequestedGuid)) return Missing(TEXT("actor_creation_guid_invalid"));
							if (AActor* const* Existing = ActorMembership.Find(RequestedGuid)) IncludeCompleteActor(*Existing);
						}
						// Creation owns the actual native GUID. The same managed ID
						// assigned by the factory binds its before/after logical slot.
						const FString ObjectPath = StringField(Row.Value.Get(), TEXT("object_path"));
						if (!ObjectPath.IsEmpty())
							if (AActor* Existing = FindObject<AActor>(nullptr, *ObjectPath); Existing && Existing->GetWorld() == &World)
								IncludeCompleteActor(Existing);
						if (New.ManagedId.IsEmpty()) New.ManagedId = AllocateLevelActorManagedId(GetWorldPath(World), Row.Key, ReservedManagedIds);
						New.bActorState = New.bActorProperties = New.bAllComponents = New.bAllowMissing = true;
						PlannedActors.Add(MoveTemp(New));
						continue;
					}
					if (Operation != TEXT("update") && Operation != TEXT("remove")) return Missing(TEXT("actor_operation_not_supported"));
					const auto Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"), GetWorldPath(World));
					for (const TCHAR* Field : {TEXT("actor_guid"), TEXT("managed_id"), TEXT("object_path")}) {
						const FString Value = StringField(Row.Value.Get(), Field);
						if (!Value.IsEmpty()) Target->SetStringField(Field, Value);
					}
					AActor* Actor = FindActorExact(World, Target, Diagnostic);
					if (!Actor) { Error.SetFailure(MoveTemp(Diagnostic)); return false; }
					IncludeCompleteActor(Actor);
					if (Operation == TEXT("remove")) {
						IncludeActor(Actor).bAllowMissing = true;
						RemovedObjectPaths.Add(Actor->GetPathName());
						RemovedObjects.AddUnique(Actor);
						TArray<UActorComponent*> Components; Actor->GetComponents(Components);
						for (const auto* Component : Components) if (Component) {
							RemovedObjectPaths.Add(Component->GetPathName()); RemovedObjects.AddUnique(Component);
							}
					}
				}
				IncludeReferencedActors(*Unit);
				continue;
			}
			if (Unit && Unit->GetRole() == TEXT("level_components")) {
				// The current complete owner membership is the conflict/structural
				// prerequisite for component create/remove/reparent. It does not
				// require reflected values from other Actors in the World.
				const auto* Owner = Source->GetModel()->FindUnit(Unit->GetOwnerRef());
				if (!Owner) return Missing(TEXT("component_collection_owner_missing"));
				const auto Target = MakeShared<FJsonObject>();
				Target->SetStringField(TEXT("world_path"), GetWorldPath(World));
				for (const TCHAR* Field : {TEXT("actor_guid"), TEXT("managed_id")}) {
					const auto* Value = Owner->GetValues().Find(Field);
					if (Value && (*Value)->GetKind() == StateSemantic::EUeAgentValueKind::String)
						Target->SetStringField(Field, (*Value)->GetText());
				}
				AActor* Actor = FindActorExact(World, Target, Diagnostic);
				if (!Actor) { Error.SetFailure(MoveTemp(Diagnostic)); return false; }
				IncludeCompleteActor(Actor);
				auto& KeyScope = Coverage.KeyedValueScopes.FindOrAdd(Id);
				KeyScope.RootField = TEXT("components");
				if (const auto* Rows = Unit->GetValues().Find(TEXT("components")); Rows && (*Rows)->GetKind() == StateSemantic::EUeAgentValueKind::Object)
					for (const auto& Row : (*Rows)->GetObjectItems()) {
						KeyScope.ObservedKeys.Add(Row.Key);
						if (Row.Value->GetKind() != StateSemantic::EUeAgentValueKind::Object) continue;
						const auto StringField = [&](const TCHAR* Key) {
							const auto* Value = Row.Value->GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Key; });
							return Value && Value->Value->GetKind() == StateSemantic::EUeAgentValueKind::String ? Value->Value->GetText() : FString();
						};
						if (StringField(TEXT("operation")) != TEXT("remove")) continue;
						const FString Name = StringField(TEXT("component_name"));
						if (UActorComponent* Component = FindComponentExact(*Actor, Name.IsEmpty() ? Row.Key : Name, Diagnostic)) {
							RemovedObjectPaths.Add(Component->GetPathName()); RemovedObjects.AddUnique(Component);
							}
					}
				// The original native owner captures this Actor's complete component
				// membership/attachment closure. Only the new K operations add source
				// references; decoding all old component records adds no before fact.
				IncludeReferencedActors(*Unit);
				continue;
			}
			const auto* Baseline = Source->FindBaselineUnit(Id);
			if (!Unit || Unit->GetUnitKind() != ESemanticUnitKind::PropertySet)
				return Missing(TEXT("edited_unit_not_a_native_value"));
			if (Unit->GetSemanticTypeRef() == WorldPropertySemanticId) {
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
				if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Unit->GetTargetIdentityKey(), Identity, Diagnostic))
				{ Error.SetFailure(MoveTemp(Diagnostic)); return false; }
				if (Identity.GetSelector(TEXT("world_path")) != GetWorldPath(World)) return Missing(TEXT("world_binding_changed"));
				FString Name = Identity.GetSelector(TEXT("property_path"));
				int32 End = Name.Len(), Index;
				if (Name.FindChar(TEXT('.'), Index)) End = FMath::Min(End, Index);
				if (Name.FindChar(TEXT('['), Index)) End = FMath::Min(End, Index);
				if (End == 0) return Missing(TEXT("world_property_path_missing"));
				WorldPropertyNames.Add(Name.Left(End));
				continue;
			}
			const bool bMetadata = Unit->GetSemanticTypeRef() == ComponentStateSemanticId || Unit->GetSemanticTypeRef() == ActorStateSemanticId;
			if (!bMetadata && Unit->GetSemanticTypeRef() != ComponentPropertySemanticId && Unit->GetSemanticTypeRef() != ActorPropertySemanticId)
				return Missing(TEXT("edited_semantic_not_a_level_native_value"));
			if (bMetadata) {
				// Type/presence/binding validation belongs to rebase and the native
				// provider. Shape differences do not require unrelated Actor values.
				if (Unit->GetSemanticTypeRef() == ActorStateSemanticId) {
					for (const FSemanticUnit* Side : {Baseline, Unit}) {
						if (!Side) continue;
						const auto* Attachment = Side->GetValues().Find(TEXT("attachment"));
						if (!Attachment || (*Attachment)->GetKind() != EUeAgentValueKind::Object) continue;
						const auto* Parent = (*Attachment)->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("parent_managed_id"); });
						if (!Parent || Parent->Value->GetKind() != EUeAgentValueKind::String) continue;
						if (Parent->Value->GetText().IsEmpty()) continue;
						const auto ParentTarget = MakeShared<FJsonObject>();
						ParentTarget->SetStringField(TEXT("world_path"), GetWorldPath(World));
						ParentTarget->SetStringField(TEXT("managed_id"), Parent->Value->GetText());
						AActor* ParentActor = FindActorExact(World, ParentTarget, Diagnostic);
						if (ParentActor) IncludeAttachmentClosure(ParentActor->GetRootComponent());
						// A missing old parent or a parent created by this same keyed
						// request is not a reason to observe every Actor. Native apply
						// still resolves the requested new parent and reports absence.
					}
				}
			}
			const bool bComponent = Unit->GetSemanticTypeRef() == ComponentPropertySemanticId || Unit->GetSemanticTypeRef() == ComponentStateSemanticId;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Unit->GetTargetIdentityKey(), Identity, Diagnostic))
			{ Error.SetFailure(MoveTemp(Diagnostic)); return false; }
			if (Identity.GetSelector(TEXT("world_path")) != GetWorldPath(World)) return Missing(TEXT("world_binding_changed"));
			const auto Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("world_path"), Identity.GetSelector(TEXT("world_path")));
			Target->SetStringField(TEXT("actor_guid"), Identity.GetSelector(bComponent ? TEXT("owner_actor_guid") : TEXT("actor_guid")));
			Target->SetStringField(TEXT("managed_id"), Identity.GetSelector(bComponent ? TEXT("owner_managed_id") : TEXT("managed_id")));
			AActor* Actor = FindActorExact(World, Target, Diagnostic);
			if (!Actor) { Error.SetFailure(MoveTemp(Diagnostic)); return false; }
			if (bComponent) {
				UActorComponent* Component = FindComponentExact(*Actor, Identity.GetSelector(TEXT("component_name")), Diagnostic);
				if (!Component) { Error.SetFailure(MoveTemp(Diagnostic)); return false; }
				IncludeComponent(Component);
				if (bMetadata) IncludeAttachmentClosure(Cast<USceneComponent>(Component));
				// Actual property notifications on registered BP components rerun
				// construction (ActorComponent::ConsolidatedPostEditChange). Metadata
				// attachment setters do not use that property notification path.
				if (!bMetadata && Component->IsRegistered() && Actor->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
					IncludeCompleteActor(Actor);
			} else {
				IncludeCompleteActor(Actor);
			}
			// Public object references are exact paths. Include both previous and
			// requested in-world Actors in the same declared attribution union.
			if (Baseline) IncludeReferencedActors(*Baseline);
			IncludeReferencedActors(*Unit);
		}
		// Deletion can clear direct native references on another owner. Select
		// the actual property candidates from declarations before reading their
		// captured values. Target/ordering/graph/collection values have no reader
		// here; visiting a complete typed Model would decode them only to reject
		// them. Missing local capture is never evidence of deletion.
		int32 ReferencePropertyCandidates = 0, ReferenceLeafSkips = 0, ReferenceContainerSkips = 0, ReferenceTargetSkips = 0, ReferenceValueReads = 0;
		if (!RemovedObjectPaths.IsEmpty() && !TryReadCapturedReferenceScope([&]() {
			return Source->GetModel()->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& SourceUnit, int32) {
			const bool bComponent = SourceUnit.GetSemanticTypeRef() == ComponentPropertySemanticId;
			const bool bWorld = SourceUnit.GetSemanticTypeRef() == WorldPropertySemanticId;
			if (!bComponent && !bWorld && SourceUnit.GetSemanticTypeRef() != ActorPropertySemanticId) return true;
			++ReferencePropertyCandidates;
			bool bClosedContainer = false, bIncompatibleReference = false;
			if (!MayContainNativeObjectReference(*Source, SourceUnit, &bClosedContainer, RemovedObjects, &bIncompatibleReference)) {
				if (bIncompatibleReference) ++ReferenceTargetSkips;
				else if (bClosedContainer) ++ReferenceContainerSkips; else ++ReferenceLeafSkips; return true;
			}
			++ReferenceValueReads;
			// No captured value: observe this candidate property's actual owner,
			// not a complete World and not a guessed absent reference.
			TOptional<bool> ReferencesRemoved;
			if (!Source->TryBaselineContainsAnyString(SourceUnit.GetUnitId(), [&](const FString& Value) {
				return RemovedObjectPaths.Contains(Value);
			}, ReferencesRemoved, Error)) return false;
			const bool bReferencesRemoved = !ReferencesRemoved.IsSet() || ReferencesRemoved.GetValue();
			if (!bReferencesRemoved) return true;
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(SourceUnit.GetTargetIdentityKey(), Identity, Diagnostic))
			{ Error.SetFailure(MoveTemp(Diagnostic)); return false; }
			if (bWorld) {
				WorldPropertyNames.Add(Identity.GetSelector(TEXT("property_path")));
				return true;
			}
			const auto Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("world_path"), GetWorldPath(World));
			Target->SetStringField(TEXT("actor_guid"), Identity.GetSelector(bComponent ? TEXT("owner_actor_guid") : TEXT("actor_guid")));
			Target->SetStringField(TEXT("managed_id"), Identity.GetSelector(bComponent ? TEXT("owner_managed_id") : TEXT("managed_id")));
			AActor* Actor = FindActorExact(World, Target, Diagnostic);
			// An already absent source owner cannot acquire a new ref->null
			// effect. It is not deleted by this operation and is not re-created.
			if (!Actor) return true;
			if (bComponent) {
				UActorComponent* Component = FindComponentExact(*Actor, Identity.GetSelector(TEXT("component_name")), Diagnostic);
				if (Component) IncludeComponent(Component);
			} else IncludeCompleteActor(Actor);
			return true;
			});
		}, Error)) return false;
		if (!RemovedObjectPaths.IsEmpty()) UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_reference_candidates properties=%d excluded_codec_leaves=%d excluded_closed_containers=%d excluded_target_classes=%d value_candidates=%d scope=declared_target"),
			ReferencePropertyCandidates, ReferenceLeafSkips, ReferenceContainerSkips, ReferenceTargetSkips, ReferenceValueReads);
		// This is the declared attribution boundary, not a claim about arbitrary
		// global notification receivers. Normal notifications still execute;
		// undeclared effects outside it remain for ordinary incremental export.
		TArray<AActor*> CompleteActors;
		for (const auto& Pair : ActorScopes) if (Pair.Value.bAllComponents) CompleteActors.Add(Pair.Key);
		for (AActor* Actor : CompleteActors) {
			for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor()) IncludeCompleteActor(Parent);
			TArray<AActor*> Attached;
			Actor->GetAttachedActors(Attached, true, true);
			for (AActor* Child : Attached) IncludeCompleteActor(Child);
		}
		for (const auto& Pair : ActorScopes) {
			Scope.Add(Pair.Value);
		}
		Scope.Append(PlannedActors);
		if (!TryCaptureLevelScopedNativeState(World, Scope, Capture, Diagnostic, ProfileId, WorldPropertyNames))
		{ Error.SetFailure(MoveTemp(Diagnostic)); return false; }
		Capture.WorldPropertyNames = MoveTemp(WorldPropertyNames);
		const double NativeReady = FPlatformTime::Seconds();
		auto Resources = MakeSemanticWorkspaceResourceTracker();
		const FKeyedMemberNameLookup KeyedNames = [Source, &Resources](const FString& Id, const FString& RootField,
			FKeyedMemberNameVisitor Visitor, FWorkspaceError& Failure) {
			return Source->VisitBaselineMemberNames(Id, RootField, Visitor, Resources, Failure);
		};
		if (!TryBuildScopedCanonicalAuthoringCapture(TEXT("ue.level.asset"), ProfileId, Capture.Snapshot,
			Resources, Capture.CanonicalSnapshot, Capture.Model, Error, &Source->GetModel().Get(), {}, KeyedNames)) return false;
		// Key allocation consumed captured stable identities at the original
		// composition owner above. A subset no longer invents another ordinal
		// namespace or needs complete native values to compare that namespace.
		// Native binding and addressed member conflicts remain in FindActorExact
		// and the ordinary scoped rebase, respectively.
		for (const auto& Unit : Capture.Model->GetUnits()) {
			Coverage.BindingUnits.Add(Unit.GetUnitId());
			if (Unit.GetRole() == TEXT("property_definition") || Source->GetEditedUnitIds()->Contains(Unit.GetUnitId()))
				Coverage.ValueUnits.Add(Unit.GetUnitId());
		}
		// The addressed field must really be observable. The native field/type
		// owner reports an unavailable value; extra Actors cannot supply it.
		for (const auto& Id : *Source->GetEditedUnitIds()) if (!Coverage.ValueUnits.Contains(Id)) return Missing(TEXT("current_property_not_exportable"));
		Capture.bScopedPreflight = true;
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_scoped_preflight scope=component_closure actors=%d components=%d requests=%d edited_units=%d scope_units=%d actor_operation_rows=%d current_model_compile=false selection_and_native_s=%.6f canonical_and_baseline_s=%.6f total_s=%.6f"),
			Capture.ActorCount, Capture.ComponentCount, Capture.Snapshot.Num(), Source->GetEditedUnitIds()->Num(), Capture.Model->GetUnits().Num(),
			ActorOperationRows,
			NativeReady - Started, FPlatformTime::Seconds() - NativeReady, FPlatformTime::Seconds() - Started);
		return true;
	}

static bool IsLevelScalarPropertyValue(const UeAgentAuthoringState::FStateEntry& Entry)
{
	const auto Carrier = Entry.Value.IsValid() && Entry.Value->Type == EJson::Object ? Entry.Value->AsObject() : nullptr;
	const auto Literal = Carrier ? Carrier->TryGetField(TEXT("literal")) : nullptr;
	return Carrier && Carrier->Values.Num() == 1 && Literal && (Literal->Type == EJson::Number || Literal->Type == EJson::Boolean);
}

/** Level owns the owner relation; composition still owns all unit/definition conversion. */
	static bool SelectLevelPropertyClosure(const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const TArray<int32>& PropertyIndices, UeAgentAuthoringState::FStateSnapshot& Out)
	{
		using namespace UeAgentLevelAuthoring;
		TArray<int32> Indices = PropertyIndices;
		Contract::FDiagnostic Diagnostic;
		const auto AddTarget = [&](const TCHAR* Model, const TCHAR* Kind, const TCHAR* Semantic, const TMap<FString, FString>& Selectors) {
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(Model, Kind, Selectors, Identity, Diagnostic)) return false;
			const int32 Index = Snapshot.FindEntryIndex(Semantic, Identity.CanonicalKey);
			if (Index == INDEX_NONE) return false;
			Indices.AddUnique(Index); return true;
		};
		for (const int32 Index : PropertyIndices) {
			const auto& Entry = Snapshot.ReadAuthoringEntry(Index);
			const auto& S = Entry.TargetIdentity.Selectors;
			auto ComponentSelectors = S;
			ComponentSelectors.Remove(TEXT("property_path"));
			if (!AddTarget(WorldIdentityModelId, TEXT("level_world"), WorldTargetSemanticId,
				{{TEXT("world_path"), S.FindRef(TEXT("world_path"))}})
				|| !AddTarget(ActorIdentityModelId, TEXT("level_actor"), ActorTargetSemanticId,
				{{TEXT("world_path"), S.FindRef(TEXT("world_path"))}, {TEXT("actor_guid"), S.FindRef(TEXT("owner_actor_guid"))},
				 {TEXT("managed_id"), S.FindRef(TEXT("owner_managed_id"))}})
				|| !AddTarget(ComponentIdentityModelId, TEXT("level_component"), ComponentTargetSemanticId, ComponentSelectors)) return false;
		}
		return Snapshot.TrySelectEntries(Indices, Out);
	}

	static bool TryRetainPartitionReadScope(UWorld& World, const FResolvedSemanticWorkspace& Source,
		TSharedRef<TArray<UeAgentLevelAuthoring::FLevelActorCaptureScope>> LogicalScope,
		TFunction<void(UWorld*, TArray<UPackage*>*)>& OutScope, FWorkspaceError& Error)
	{
		OutScope = {};
		UWorldPartition* Partition = World.GetWorldPartition();
		if (!Partition || !Source.GetEditedUnitIds().IsSet()) return true;
		auto Guids = MakeShared<TSet<FGuid>>();
		TSet<FString> Mentioned, RemovedPaths;
		TSet<FGuid> RemovedGuids;
		const auto AddIdentity = [&](const FSemanticUnit::FDeclarationView& Declaration) {
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			Contract::FDiagnostic Failure;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Declaration.GetTargetIdentityKey(), Identity, Failure)) return;
			FGuid Guid;
			for (const TCHAR* Field : {TEXT("actor_guid"), TEXT("owner_actor_guid")})
				if (FGuid::Parse(Identity.GetSelector(Field), Guid)) Guids->Add(Guid);
		};
		TFunction<void(const StateSemantic::FUeAgentCanonicalValue&)> Observe;
		Observe = [&](const StateSemantic::FUeAgentCanonicalValue& Value) {
			if (Value.GetKind() == EUeAgentValueKind::String) {
				Mentioned.Add(Value.GetText());
				FGuid Guid; if (FGuid::Parse(Value.GetText(), Guid) && Partition->GetActorDescInstance(Guid)) Guids->Add(Guid);
			} else if (Value.GetKind() == EUeAgentValueKind::Object) {
				for (const auto& Field : Value.GetObjectItems()) Observe(Field.Value.Get());
			} else if (Value.GetKind() == EUeAgentValueKind::Array) {
				for (const auto& Item : Value.GetArrayItems()) Observe(Item.Get());
			}
		};
		for (const auto& Id : *Source.GetEditedUnitIds()) {
			const auto* Unit = Source.GetModel()->FindUnit(Id);
			if (!Unit) continue; // The original Source/native owner diagnoses a missing requested row.
			AddIdentity(Unit->GetDeclaration());
			if (const auto Owner = Source.GetModel()->FindDeclaration(Unit->GetOwnerRef())) AddIdentity(*Owner);
			for (const auto& Field : Unit->GetValues()) Observe(Field.Value.Get());
			if (Unit->GetUnitKind() == ESemanticUnitKind::PropertySet) {
				if (const auto* Before = Source.FindBaselineUnit(Id))
					for (const auto& Field : Before->GetValues()) Observe(Field.Value.Get());
			}
			if (Unit->GetRole() == TEXT("actor_component_hierarchy"))
				if (const auto* Actors = Unit->GetValues().Find(TEXT("actors")))
					for (const auto& Row : (*Actors)->GetObjectItems()) {
						if (Row.Value->GetKind() != EUeAgentValueKind::Object) continue;
						const auto* Operation = Row.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("operation"); });
						if (!Operation || Operation->Value->GetText() != TEXT("remove")) continue;
						for (const auto& Field : Row.Value->GetObjectItems()) if (Field.Key == TEXT("actor_guid")) {
							FGuid Guid;
							if (FGuid::Parse(Field.Value->GetText(), Guid)) {
								RemovedGuids.Add(Guid);
								if (const auto* Desc = Partition->GetActorDescInstance(Guid)) RemovedPaths.Add(Desc->GetActorSoftPath().ToString());
							}
						}
					}
			if (Unit->GetRole() == TEXT("level_components")) {
				const auto Owner = Source.GetModel()->FindDeclaration(Unit->GetOwnerRef());
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity; Contract::FDiagnostic Failure; FGuid Guid;
				if (Owner && UeAgentAuthoringIdentity::ParseCanonicalKey(Owner->GetTargetIdentityKey(), Identity, Failure)
					&& FGuid::Parse(Identity.GetSelector(TEXT("actor_guid")), Guid))
					if (const auto* Desc = Partition->GetActorDescInstance(Guid))
						if (const auto* Components = Unit->GetValues().Find(TEXT("components")))
							for (const auto& Row : (*Components)->GetObjectItems()) {
								if (Row.Value->GetKind() != EUeAgentValueKind::Object) continue;
								const auto* Operation = Row.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("operation"); });
								if (!Operation || Operation->Value->GetText() != TEXT("remove")) continue;
								const auto* Name = Row.Value->GetObjectItems().FindByPredicate([](const auto& Field) { return Field.Key == TEXT("component_name"); });
								RemovedPaths.Add(Desc->GetActorSoftPath().ToString() + TEXT(".") + (Name ? Name->Value->GetText() : Row.Key));
								RemovedGuids.Add(Guid);
							}
			}
		}
		// Managed ids are authoring identities; descriptors own current native
		// GUID/path membership. Consult declaration selectors and descriptor
		// metadata, without loading or reading every Actor's properties.
		Source.GetModel()->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity; Contract::FDiagnostic Failure;
			if (UeAgentAuthoringIdentity::ParseCanonicalKey(Declaration.GetTargetIdentityKey(), Identity, Failure))
				for (const TCHAR* Field : {TEXT("managed_id"), TEXT("owner_managed_id")})
					if (!Identity.GetSelector(Field).IsEmpty() && Mentioned.Contains(Identity.GetSelector(Field))) { AddIdentity(Declaration); break; }
			return true;
		});
		FWorldPartitionHelpers::ForEachActorDescInstance<AActor>(Partition, [&](const FWorldPartitionActorDescInstance* Desc) {
			const FString Path = Desc->GetActorSoftPath().ToString();
			for (const auto& Text : Mentioned) if (Text == Path || Text.StartsWith(Path + TEXT("."), ESearchCase::CaseSensitive)) { Guids->Add(Desc->GetGuid()); break; }
			for (const FGuid& Reference : Desc->GetReferences()) if (RemovedGuids.Contains(Reference)) { Guids->Add(Desc->GetGuid()); break; }
			return true;
		});
		if (!RemovedPaths.IsEmpty()) {
			int32 ReferencePropertyCandidates = 0, ReferenceLeafSkips = 0, ReferenceContainerSkips = 0, ReferenceValueReads = 0;
			if (!TryReadCapturedReferenceScope([&]() {
				return Source.GetModel()->VisitDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
				if (Declaration.GetSemanticTypeRef() != UeAgentLevelAuthoring::ActorPropertySemanticId
					&& Declaration.GetSemanticTypeRef() != UeAgentLevelAuthoring::ComponentPropertySemanticId) return true;
				++ReferencePropertyCandidates;
				bool bClosedContainer = false;
				if (!MayContainNativeObjectReference(Source, Declaration, &bClosedContainer)) {
					if (bClosedContainer) ++ReferenceContainerSkips; else ++ReferenceLeafSkips; return true;
				}
				++ReferenceValueReads;
				TOptional<bool> ReferencesRemoved;
				if (!Source.TryBaselineContainsAnyString(Declaration.GetUnitId(), [&](const FString& Value) {
					for (const auto& Path : RemovedPaths) if (Value == Path || Value.StartsWith(Path + TEXT("."), ESearchCase::CaseSensitive)) return true;
					return false;
				}, ReferencesRemoved, Error)) return false;
				if (ReferencesRemoved.IsSet() && ReferencesRemoved.GetValue()) AddIdentity(Declaration);
				return true;
				});
			}, Error)) return false;
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=partition_reference_candidates properties=%d excluded_codec_leaves=%d excluded_closed_containers=%d value_candidates=%d scope=declared_target"),
				ReferencePropertyCandidates, ReferenceLeafSkips, ReferenceContainerSkips, ReferenceValueReads);
		}
		if (!Source.GetModel()->CheckValueReadResult(Error)) return false;
		auto References = MakeShared<FWorldPartitionHelpers::FForEachActorWithLoadingResult>();
		auto CurrentWorld = MakeShared<TWeakObjectPtr<UWorld>>();
		OutScope = [Guids, References, LogicalScope, CurrentWorld](UWorld* Reloaded, TArray<UPackage*>* Packages) {
			if (Packages) {
				if (!Reloaded) return;
				int32 LoadedActors = 0, SelectedActors = 0;
				for (TActorIterator<AActor> It(Reloaded); It; ++It) {
					++LoadedActors;
					// Loading a referenced Actor is not permission to save its
					// package. Only the actual native observation/write scope owns it.
					bool bSelected = false;
					for (const auto& Slot : *LogicalScope)
						if ((Slot.ActorGuid.IsValid() && Slot.ActorGuid == It->GetActorGuid())
							|| (!Slot.ManagedId.IsEmpty() && Slot.ManagedId == UeAgentLevelAuthoring::GetManagedActorId(**It))) { bSelected = true; break; }
					if (bSelected) { ++SelectedActors; if (UPackage* Package = It->GetExternalPackage()) Packages->AddUnique(Package); }
				}
				if (Reloaded->PersistentLevel && Reloaded->PersistentLevel->MapBuildData)
					Packages->AddUnique(Reloaded->PersistentLevel->MapBuildData->GetOutermost());
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=partition_package_scope loaded_actor_identities=%d selected_actors=%d owned_packages=%d actor_values_read=0"),
					LoadedActors, SelectedActors, Packages->Num());
				return;
			}
			if (!Reloaded) {
				// Newly created Actors acquire their real GUID during native spawn.
				// Capture only matching logical slots before releasing old bindings.
				if (CurrentWorld->IsValid()) for (TActorIterator<AActor> It(CurrentWorld->Get()); It; ++It)
					for (const auto& Slot : *LogicalScope)
						if ((Slot.ActorGuid.IsValid() && Slot.ActorGuid == It->GetActorGuid())
							|| (!Slot.ManagedId.IsEmpty() && Slot.ManagedId == UeAgentLevelAuthoring::GetManagedActorId(**It))) Guids->Add(It->GetActorGuid());
				References->ActorReferences.Reset(); CurrentWorld->Reset(); return;
			}
			*CurrentWorld = Reloaded;
			if (Guids->IsEmpty() || !Reloaded->GetWorldPartition()) return; // Empty ActorGuids means ALL to UE.
			FWorldPartitionHelpers::FForEachActorWithLoadingParams Params;
			Params.bKeepReferences = true; Params.ActorClasses = {AActor::StaticClass()}; Params.ActorGuids = Guids->Array();
			FWorldPartitionHelpers::ForEachActorWithLoading(Reloaded->GetWorldPartition(),
				[](const FWorldPartitionActorDescInstance*) { return true; }, Params, *References);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=partition_read_scope requested_guids=%d loaded_reference_closure=%d complete_actor_values=false"), Guids->Num(), References->ActorReferences.Num());
		};
		OutScope(&World, nullptr);
		return true;
	}

	bool TryPrepareLevelWorkspace(UWorld& World, const FString& WorkspaceRoot, const FString& ProfileId,
		Composition::HjsonImport::FPreparedAuthoringImportPtr& OutPrepared,
		TSharedRef<FJsonObject>& OutBundle, FWorkspaceError& OutError,
		TFunction<Contract::FSourceLocation(const FString&, const FString&)>* OutSourceResolver,
		UeAgentLevelAuthoring::FLevelAuthoringCapture* OutCapture, TFunction<void(UWorld*, TArray<UPackage*>*)>* OutPartitionReadScope)
	{
			if (!EnsureAuthoringRegistered(OutError)) return false;
			FUeAgentResourceTracker Resources = MakeSemanticWorkspaceResourceTracker();
			// Source intent precedes native capture for every caller, including
			// analysis consumers which do not retain the optional capture output.
			if (!GetAcceptedSemanticWorkspace(WorkspaceRoot)) {
				FResolvedSemanticWorkspacePtr Source;
				if (!TryLoadHjsonImportSource(FWorkspaceSource::Directory(WorkspaceRoot), ESemanticWorkspaceMode::State,
					Resources, Source, OutError)) return false;
			}
			UeAgentLevelAuthoring::FLevelAuthoringCapture Capture;
			auto LogicalScope = MakeShared<TArray<UeAgentLevelAuthoring::FLevelActorCaptureScope>>();
			TFunction<void(UWorld*, TArray<UPackage*>*)> PartitionScope;
			ON_SCOPE_EXIT {
				if (PartitionScope) {
					// Failure and read-only callers have no native transaction to
					// perform the last release. Clear native buffers first, then
					// permit the selected external Actors to unload.
					Capture.Snapshot.ReleaseNativeValues();
					PartitionScope(nullptr, nullptr);
				}
			};
			const auto AcceptedSource = GetAcceptedSemanticWorkspace(WorkspaceRoot);
			if (!AcceptedSource) {
				OutError.SetSourceFailure(TEXT("level_semantic_source_missing"), TEXT("Level preparation returned no authored Source."), WorkspaceRoot);
				return false;
			}
			auto EffectiveSource = AcceptedSource.ToSharedRef();
			if (!TryResolveUnconsumedAuthoring(WorkspaceRoot, EffectiveSource, Resources, EffectiveSource, OutError)
				|| !TryRetainPartitionReadScope(World, *EffectiveSource, LogicalScope, PartitionScope, OutError)) return false;
			UE::UAI::Contract::FDiagnostic CaptureError;
			Composition::HjsonImport::FScopedCurrentCoverage Coverage;
			const auto Scoped = TryCaptureScopedLevelPreflight(World, WorkspaceRoot, ProfileId, Capture, Coverage, OutError, EffectiveSource, *LogicalScope);
			if (Scoped.IsSet() && !Scoped.GetValue()) return false;
			if ((!Scoped.IsSet() && !UeAgentLevelAuthoring::TryCaptureLevelAuthoringState(
					World, Capture, CaptureError, ProfileId)
				)
				|| !Capture.CanonicalSnapshot.IsValid()
				|| !Capture.Model.IsValid())
			{
				OutError.SetFailure(!CaptureError.IsEmpty() ? AsLevelNativeResult(MoveTemp(CaptureError)) : LevelNativeFact(
					TEXT("level_semantic_capture_incomplete"),
					!Capture.CanonicalSnapshot.IsValid() ? TEXT("World capture returned no canonical snapshot.") : !Capture.Model.IsValid()
						? TEXT("World capture returned no semantic model.") : TEXT("World capture failed without returning a diagnostic."),
					!Capture.CanonicalSnapshot.IsValid() ? TEXT("/world_capture/snapshot") : !Capture.Model.IsValid() ? TEXT("/world_capture/model") : TEXT("/world_capture/succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
				return false;
			}
			UE::UAI::Composition::HjsonImport::FSemanticWorkspaceImportError ImportError;
			const Contract::FUeAgentCallerContext Caller(
				TEXT("level-world-semantic-import"),
				{TEXT("uai:query"), TEXT("uai:operate"), TEXT("authoring.write"),
					TEXT("authoring.structure")},
				TEXT("level-semantic-import"));
			if (!UE::UAI::Composition::HjsonImport::TryPrepareSemanticWorkspaceImport(
				FWorkspaceSource::Directory(WorkspaceRoot),
				ESemanticWorkspaceMode::State,
				Capture.Model.ToSharedRef(),
				Capture.CanonicalSnapshot.ToSharedRef(),
				DomainAuthoring::BuildSemanticWorkspaceImportDescriptor(TEXT("level.semantic_workspace"), *Capture.Model),
				TEXT("level-semantic-import"),
				TEXT("level-semantic-import"),
				Caller, false, FString(), FString(), Resources, OutPrepared, ImportError, Capture.bScopedPreflight ? &Coverage : nullptr)
			|| !OutPrepared.IsValid())
		{
			OutError.SetFailure(!ImportError.IsEmpty() ? MoveTemp(ImportError) : LevelNativeFact(TEXT("level_semantic_preparation_missing"),
				TEXT("Semantic workspace preparation returned no prepared import."), TEXT("/semantic_import/prepared"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()));
			return false;
		}
		if (!TryBuildLevelSemanticWorkspaceBundleWithSnapshot(OutPrepared->GetWorkspace(),
			OutPrepared->GetChangeSetDraft().GetChanges(), OutBundle, OutError, OutSourceResolver, Capture.CanonicalSnapshot.Get())) return false;
			if (Capture.bScopedPreflight) Capture.AttributionScope = LogicalScope;
			if (OutPartitionReadScope) *OutPartitionReadScope = MoveTemp(PartitionScope);
		if (OutCapture) *OutCapture = MoveTemp(Capture);
		return true;
	}

	namespace
	{

		bool ApplySemanticWorkspace(
			UWorld& World,
			const FString& WorkspaceRoot,
			const FString& RequestId,
			const UeAgentLevelAuthoring::FWorkspaceApplyOptions& Options,
			UeAgentLevelAuthoring::FWorkspaceApplyResult& OutResult,
			FWorkspaceError& OutError,
			const FString& WorkspaceProfileId, const FString& AnalysisRef, bool& bMutationStarted)
		{
		Composition::HjsonImport::FPreparedAuthoringImportPtr Prepared;
		TSharedRef<FJsonObject> Bundle = MakeShared<FJsonObject>();
		TFunction<Contract::FSourceLocation(const FString&, const FString&)> SourceResolver;
		UeAgentLevelAuthoring::FLevelAuthoringCapture BeforeCapture;
		TFunction<void(UWorld*, TArray<UPackage*>*)> PartitionReadScope;
		if (!TryPrepareLevelWorkspace(World, WorkspaceRoot, WorkspaceProfileId,
			Prepared, Bundle, OutError, &SourceResolver, Options.bPersistentReadback ? &BeforeCapture : nullptr, &PartitionReadScope)) return false;
		if (!AnalysisRef.IsEmpty())
		{
			Contract::FDiagnostic AnalysisError;
			if (!SceneAnalysis::ValidateForApply(World, WorkspaceRoot, AnalysisRef, *Prepared, Bundle, AnalysisError))
			{ OutError.SetFailure(AnalysisError); return false; }
		}
		auto Source = UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
			Bundle, TEXT("inline://semantic-level-workspace"));
		Source.ResolveSource = MoveTemp(SourceResolver);
		TOptional<UeAgentAuthoringState::FStateSnapshot> AttributionBefore;
		TOptional<FCompositionSnapshotBinding> AttributionBinding;
		if (BeforeCapture.bScopedPreflight) {
			const double Started = FPlatformTime::Seconds();
			// Preparation and apply run synchronously on the same Editor thread.
			// Reuse the selected component closure, not a second World capture.
			AttributionBinding.Emplace(BuildNativeAuthoringCaptureBinding(TEXT("ue.level.asset.component_scope"), BeforeCapture.Snapshot));
			const double Bound = FPlatformTime::Seconds();
			AttributionBefore.Emplace(BeforeCapture.Snapshot.MakeAuthoringOnlySnapshot());
			const double Retained = FPlatformTime::Seconds();
			const int32 NativeCount = BeforeCapture.Snapshot.Num();
			UeAgentLevelAuthoring::PrepareLevelCaptureReuse(World, BeforeCapture, WorkspaceProfileId);
			UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_attribution_before scope=component_closure reused_preflight=true complete_entries=0 total_observed_entries=%d binding_s=%.6f retain_s=%.6f reuse_prepare_s=%.6f full_canonical=false total_s=%.6f"),
				NativeCount, Bound - Started,
				Retained - Bound, FPlatformTime::Seconds() - Retained, FPlatformTime::Seconds() - Started);
		} else if (Options.bPersistentReadback) UeAgentLevelAuthoring::PrepareLevelCaptureReuse(World, BeforeCapture, WorkspaceProfileId);
		auto ApplyOptions = Options;
		ApplyOptions.PartitionReadScope = MoveTemp(PartitionReadScope);
		ApplyOptions.bScopedPackageCapture = BeforeCapture.bScopedPreflight;
		ApplyOptions.AuthoringProfileId = Prepared->GetWorkspace()->GetModel()->GetProfileId();
		TUniquePtr<FCommittedWorkspaceTransaction> CommittedTransaction;
		if (Options.bPersistentReadback && BeforeCapture.bScopedPreflight) {
			if (!BeforeCapture.Model || !BeforeCapture.AttributionScope || !AttributionBefore.IsSet() || !AttributionBinding.IsSet()) {
				OutError.SetSourceFailure(TEXT("level_committed_capture_missing"), TEXT("Scoped preflight has no Actor attribution evidence.")); return false;
			}
			const FString WorldPath = World.GetPathName();
			const FString Profile = ApplyOptions.AuthoringProfileId;
			const auto LocalModel = BeforeCapture.Model.ToSharedRef();
			const auto NamingBaseline = Prepared->GetWorkspace();
			bool bPropertyIntent = NamingBaseline->GetEditedUnitIds().IsSet();
			if (bPropertyIntent) {
				for (const auto& Id : *NamingBaseline->GetEditedUnitIds()) {
					const auto* Unit = LocalModel->FindUnit(Id);
					if (!Unit || Unit->GetSemanticTypeRef() != UeAgentLevelAuthoring::ComponentPropertySemanticId) {
						bPropertyIntent = false;
						break;
					}
				}
			}
			const auto AttributionScope = BeforeCapture.AttributionScope.ToSharedRef();
			ApplyOptions.OnActorMoved = [AttributionScope](const FGuid& Previous, const AActor& Moved) {
				const FString Managed = UeAgentLevelAuthoring::GetManagedActorId(Moved);
				for (auto& Slot : *AttributionScope) {
					if (Slot.ActorGuid.IsValid() ? Slot.ActorGuid != Previous : Slot.ManagedId.IsEmpty() || Slot.ManagedId != Managed) continue;
					Slot.ActorGuid = Moved.GetActorGuid();
					Slot.ManagedId = Managed;
					Slot.RelocatedActorPath = Moved.GetPathName();
				}
			};
			const auto WorldPropertyNames = BeforeCapture.WorldPropertyNames;
			CommittedTransaction = MakeUnique<FCommittedWorkspaceTransaction>(TEXT("ue.level.asset"), WorldPath, WorkspaceRoot,
				Prepared->GetWorkspace()->GetWorkspaceFingerprint(), FString(), LocalModel, AttributionBinding.GetValue(),
				[WorldPath, Profile, LocalModel, NamingBaseline, bPropertyIntent, AttributionScope, WorldPropertyNames, Before = MoveTemp(AttributionBefore.GetValue())](
					TSharedPtr<FJsonObject>& Delta, TSharedPtr<const FCommittedWorkspaceDelta>& Typed, TOptional<FCompositionSnapshotBinding>& Binding, FString& Scope, FWorkspaceError& Error) {
					const double Started = FPlatformTime::Seconds();
					UWorld* CurrentWorld = FindObject<UWorld>(nullptr, *WorldPath);
					if (!CurrentWorld) { Error.SetSourceFailure(TEXT("level_committed_world_unavailable"), TEXT("The transaction world is unavailable."), WorldPath); return false; }
					for (const auto& Slot : *AttributionScope) if (!Slot.RelocatedActorPath.IsEmpty() && !Slot.bAllowMissing) {
						const auto* Moved = FindObject<AActor>(nullptr, *Slot.RelocatedActorPath);
						if (!Moved || Moved->GetWorld() != CurrentWorld || Moved->GetActorGuid() != Slot.ActorGuid
							|| !UeAgentLevelAuthoring::EvaluateActorAuthorability(*CurrentWorld, *Moved).bAuthorable) {
							Error.SetSourceFailure(TEXT("level_relocated_actor_binding_missing"), TEXT("The actor returned by the native move could not be rebound for actual observation."), Slot.RelocatedActorPath);
							return false;
						}
					}
					UeAgentLevelAuthoring::FLevelAuthoringCapture After;
					Contract::FDiagnostic Diagnostic;
					// The same selected fields/relationships own both observations.
					// Capturing the complete current World against an older published
					// baseline would attribute unrelated external changes to this edit.
					if (!UeAgentLevelAuthoring::TryCaptureLevelScopedNativeState(*CurrentWorld, *AttributionScope, After, Diagnostic, Profile, WorldPropertyNames))
					{ Error.SetFailure(MoveTemp(Diagnostic)); return false; }
					const double Observed = FPlatformTime::Seconds();
					TArray<int32> Changed;
					bool bPropertyScope = Before.TryFindAuthoringChanges(After.Snapshot, Changed) && bPropertyIntent;
					for (const int32 Index : Changed) {
						if (!bPropertyScope) break;
						const auto& Old = Before.ReadAuthoringEntry(Index);
						const auto& New = After.Snapshot.ReadAuthoringEntry(Index);
						bPropertyScope = UeAgentAuthoringState::HaveSameStateEntryDeclaration(Old, New)
							&& Old.SemanticId == UeAgentLevelAuthoring::ComponentPropertySemanticId;
						if (bPropertyScope) {
							if (!Old.bAvailable || !Old.bAuthoringVisible || !Old.Value || !New.Value) {
								Error.SetSourceFailure(TEXT("level_actual_property_closure_missing"), TEXT("The selected actual property delta has no matching before/after closure. No larger projection was selected."), WorldPath); return false;
							}
							bPropertyScope = IsLevelScalarPropertyValue(Old) && IsLevelScalarPropertyValue(New);
						}
					}
					const double Compared = FPlatformTime::Seconds();
					auto Resources = MakeSemanticWorkspaceResourceTracker();
					const FKeyedMemberNameLookup KeyedNames = [NamingBaseline, &Resources](const FString& Id, const FString& RootField,
						FKeyedMemberNameVisitor Visitor, FWorkspaceError& Failure) {
						return NamingBaseline->VisitBaselineMemberNames(Id, RootField, Visitor, Resources, Failure);
						};
					// The property-delta owner reads only these selected declarations
					// and their dependencies. Native preflight already bound them to
					// the authored source. Full publication values belong to refresh.
					const auto Baseline = LocalModel;
					FAuthoringModelPtr BeforeModel = LocalModel, AfterModel = LocalModel;
					StateSemantic::FUeAgentCanonicalSnapshotPtr BeforeCanonical, AfterCanonical;
					TArray<FString> ChangedUnits;
					if (bPropertyScope && !Changed.IsEmpty()) {
						UeAgentAuthoringState::FStateSnapshot BeforePart, AfterPart;
						if (!SelectLevelPropertyClosure(Before, Changed, BeforePart)
							|| !SelectLevelPropertyClosure(After.Snapshot, Changed, AfterPart)) {
							Error.SetSourceFailure(TEXT("level_actual_property_closure_missing"), TEXT("The selected actual property delta has no matching before/after closure. No larger projection was selected."), WorldPath); return false;
							}
						if (bPropertyScope) {
							if (!TryBuildScopedCanonicalAuthoringCapture(TEXT("ue.level.asset"), Profile, BeforePart, Resources, BeforeCanonical, BeforeModel, Error, &Baseline.Get(), {}, KeyedNames)
								|| !TryBuildScopedCanonicalAuthoringCapture(TEXT("ue.level.asset"), Profile, AfterPart, Resources, AfterCanonical, AfterModel, Error, &Baseline.Get(), {}, KeyedNames)) return false;
							for (const auto& Unit : BeforeModel->GetUnits()) if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
								&& Unit.GetSemanticTypeRef() == UeAgentLevelAuthoring::ComponentPropertySemanticId) ChangedUnits.Add(Unit.GetUnitId());
							if (ChangedUnits.Num() != Changed.Num()) {
								Error.SetSourceFailure(TEXT("level_actual_property_binding_missing"), TEXT("The selected actual properties did not produce matching stable units. No larger projection was selected."), WorldPath); return false;
								}
						}
					}
					TOptional<bool> Built;
					if (bPropertyScope) {
						Binding.Emplace(BuildNativeAuthoringCaptureBinding(TEXT("ue.level.asset.component_scope"), After.Snapshot));
						Built = TryBuildCommittedPropertyDelta(*Baseline, *BeforeModel, *AfterModel, ChangedUnits,
							Binding->Revision.GetGeneration(), Resources, Delta, Error, &Typed);
					}
					if (Built.IsSet() && !Built.GetValue()) return false;
					if (bPropertyScope && !Built.IsSet()) {
						Error.SetSourceFailure(TEXT("level_actual_property_delta_missing"), TEXT("The selected property-delta producer did not return its result. No structural projection was selected."), WorldPath); return false;
						}
					if (!Built.IsSet()) {
						// Aggregate/collision/definition changes use the same complete
						// Actor observations. Missing World rows are not deletions:
						// shared delta applies only differences inside this scope.
						BeforeModel = LocalModel;
						if (!TryBuildScopedCanonicalAuthoringCapture(TEXT("ue.level.asset"), Profile, After.Snapshot,
							Resources, AfterCanonical, AfterModel, Error, &Baseline.Get(), {}, KeyedNames)
							|| !TryBuildCommittedWorkspaceDelta(*BeforeModel, *AfterModel, Resources, Delta, Error, &Typed)) return false;
						// The complete semantic delta carries AfterModel's canonical
						// generation. Keep its matching binding; the raw observation
						// binding has a different (equally valid) revision grammar.
						Binding.Emplace(FCompositionSnapshotBinding{AfterCanonical->GetSnapshotId(),
							AfterCanonical->GetRevision(), AfterCanonical->GetProvenance().GetEvidenceId()});
						Scope = TEXT("component_closure_semantic_projection");
					} else Scope = TEXT("component_closure_changed_property_projection");
					UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=level_attribution_after scope=component_closure complete_entries=0 attribution_entries=%d changed_entries=%d changed_units=%d native_s=%.6f compare_s=%.6f projection_and_delta_s=%.6f full_canonical=%s total_s=%.6f"),
						After.Snapshot.Num(), Changed.Num(), ChangedUnits.Num(), Observed - Started, Compared - Observed,
						FPlatformTime::Seconds() - Compared, Built.IsSet() ? TEXT("false") : TEXT("true"), FPlatformTime::Seconds() - Started);
					AttachAcceptedActualUnits(&NamingBaseline.Get(), *AfterModel, Typed);
					if (bPropertyScope && Typed && NamingBaseline->GetEditedUnitIds().IsSet()) {
						// The paired native observations proved every unselected row
						// unchanged. Keep the accepted setter's real value even when
						// its delta is zero beside another changed property.
						auto WithActual = MakeShared<FCommittedWorkspaceDelta>(*Typed);
						for (const auto& Id : *NamingBaseline->GetEditedUnitIds())
							if (!WithActual->AcceptedActualUnits.Contains(Id) && !ChangedUnits.Contains(Id))
								if (const auto* Unit = LocalModel->FindUnit(Id)) WithActual->AcceptedActualUnits.Add(Id, *Unit);
						Typed = MoveTemp(WithActual);
					}
					return true;
				}, GetAcceptedSemanticWorkspace(WorkspaceRoot));
		} else if (Options.bPersistentReadback) {
			auto Resources = MakeSemanticWorkspaceResourceTracker();
			FString BootstrapState;
			const FCompositionSnapshotBinding Binding{BeforeCapture.CanonicalSnapshot->GetSnapshotId(),
				BeforeCapture.CanonicalSnapshot->GetRevision(), BeforeCapture.CanonicalSnapshot->GetProvenance().GetEvidenceId()};
			if (!TryPrepareCommittedImportState(WorkspaceRoot, TEXT("ue.level.asset"), World.GetPathName(),
				*BeforeCapture.Model, Binding, Resources, BootstrapState, OutError)) return false;
			const FString WorldPath = World.GetPathName();
			const FString Profile = ApplyOptions.AuthoringProfileId;
			CommittedTransaction = MakeUnique<FCommittedWorkspaceTransaction>(TEXT("ue.level.asset"), WorldPath,
				WorkspaceRoot, Prepared->GetWorkspace()->GetWorkspaceFingerprint(), MoveTemp(BootstrapState), BeforeCapture.Model.ToSharedRef(),
				[WorldPath, Profile](FAuthoringModelPtr& Model, TOptional<FCompositionSnapshotBinding>& AfterBinding,
					FString& ObservationScope, FWorkspaceError& Error) {
					UWorld* CurrentWorld = FindObject<UWorld>(nullptr, *WorldPath);
					if (!CurrentWorld) {
						Error.SetSourceFailure(TEXT("level_committed_world_unavailable"),
							TEXT("The transaction's current world is unavailable for its actual export evidence."), WorldPath);
						return false;
					}
					UeAgentLevelAuthoring::FLevelAuthoringCapture After;
					Contract::FDiagnostic Diagnostic;
					if (!UeAgentLevelAuthoring::TryCaptureLevelAuthoringState(*CurrentWorld, After, Diagnostic, Profile)) {
						Error.SetFailure(MoveTemp(Diagnostic)); return false;
					}
					Model = MoveTemp(After.Model);
					AfterBinding.Emplace(FCompositionSnapshotBinding{After.CanonicalSnapshot->GetSnapshotId(),
						After.CanonicalSnapshot->GetRevision(), After.CanonicalSnapshot->GetProvenance().GetEvidenceId()});
					ObservationScope = TEXT("complete_world_topology_action");
					return true;
				}, GetAcceptedSemanticWorkspace(WorkspaceRoot));
		}
		if (CommittedTransaction && !CommittedTransaction->IsReady(OutError)) return false;
		bMutationStarted = true;
		const bool bApplied = UeAgentLevelAuthoring::ApplyWorkspaceSource(
			World,
			Source,
			ApplyOptions, OutResult, RequestId);
		return bApplied;
		}

		bool DecodeRequest(
			const FString& CommandId,
			const FString& ContractHash,
			const FString& ParamsJson,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			FWorkspaceError& OutError)
		{
			const FString RequestId = TEXT("level-world-domain-")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			const FString Envelope = FString::Printf(
				TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
				TEXT("\"message_kind\":\"command_request\",")
				TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
				TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
				TEXT("\"params\":%s}"),
				*RequestId, *RequestId, *CommandId, *ContractHash, *ParamsJson);
			FTCHARToUTF8 Encoded(*Envelope);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()),
				Encoded.Length());
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			const bool bDecoded = FUeAgentCommandProtocolCodec().DecodeRequest(Bytes, OutRequest, Diagnostic);
			if (!bDecoded || !OutRequest.IsSet())
				return Fail(OutError, !Diagnostic.IsEmpty() ? MoveTemp(Diagnostic) : LevelNativeFact(
					TEXT("level_world_domain_internal_request_invalid"),
					bDecoded ? TEXT("The internal command decoder returned no request.") : TEXT("The internal command decoder failed without returning a diagnostic."),
					bDecoded ? TEXT("/internal_command/request") : TEXT("/internal_command/decode_succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			return true;
		}

		bool ComposeRequest(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			TOptional<FUeAgentCommandRequest>& OutRequest,
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
			FWorkspaceError& OutError)
		{
			TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry;
			UE::UAI::Contract::FDiagnostic Error;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, OutDispatch, Error)
				|| !Registry.IsValid() || !OutDispatch.IsValid())
			{
				return Fail(OutError, Error.IsValid() ? MoveTemp(Error) : LevelNativeFact(TEXT("level_world_domain_command_registry_unavailable"),
					!Registry.IsValid() ? TEXT("Active command composition returned no registry.") : !OutDispatch.IsValid()
						? TEXT("Active command composition returned no dispatch.") : TEXT("Active command composition failed without returning a diagnostic."),
					!Registry.IsValid() ? TEXT("/command_composition/registry") : !OutDispatch.IsValid() ? TEXT("/command_composition/dispatch") : TEXT("/command_composition/succeeded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			const CommandContract::FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				Error = LevelNativeFact(TEXT("level_world_domain_command_contract_missing"),
					TEXT("The active registry has no contract for the required command."), TEXT("/command_registry/contract"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.SubjectId = CommandId;
				return Fail(OutError, MoveTemp(Error));
			}
			FString ParamsJson;
			if (!CanonicalObject(Params, ParamsJson, OutError)) return false;
			return DecodeRequest(CommandId, Contract->GetContractHash(),
				ParamsJson, OutRequest, OutError);
		}

		bool SubmitWorldMutation(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutPayload,
			FWorkspaceError& OutError)
		{
			TOptional<FUeAgentCommandRequest> Request;
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
			if (!ComposeRequest(CommandId, Params, Request, Dispatch, OutError))
				return false;
			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> Scopes;
			UE::UAI::Contract::FDiagnostic Error;
			if (!OperationOwners::WorldCommands::TryResolveMechanics(
					CommandId, Mechanics, Scopes, Error)
				|| !Mechanics.IsValid())
			{
				if (Error.IsEmpty()) Error = LevelNativeFact(TEXT("level_world_domain_command_mechanics_missing"),
					TEXT("Command resolution did not return command mechanics."), TEXT("/command_mechanics"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (Error.SubjectId.IsEmpty()) Error.SubjectId = CommandId;
				return Fail(OutError, MoveTemp(Error));
			}
			Scopes.AddUnique(TEXT("uai:query"));
			Scopes.AddUnique(TEXT("uai:operate"));
			const FUeAgentCommandResult Result =
				OperationOwners::WorldCommands::Submit(
					Mechanics.ToSharedRef(), Request.GetValue(),
					Contract::FUeAgentCallerContext(TEXT("level-world-domain-provider"),
						Scopes, Request->GetCorrelationId()),
					MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				UE::UAI::HjsonWorkspace::DomainAuthoring::SetWorkspaceErrorFromCommandFailure(Result.GetDiagnostic(),
					TEXT("level_world_domain_world_mutation_failed"), CommandId, OutError);
				return false;
			}
			UE::UAI::Contract::FDiagnostic JsonError;
			if (!Json::TryParseObject(Result.GetPayloadJson(), OutPayload, JsonError)
				|| !OutPayload.IsValid())
			{
				return Fail(OutError, JsonError.IsEmpty() ? LevelNativeFact(TEXT("level_world_domain_mutation_payload_missing"),
					TEXT("The world mutation command returned no result object."), TEXT("/command_result/payload"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))) : AsLevelNativeResult(MoveTemp(JsonError)));
			}
			return true;
		}

		bool ReadDataLayersViaPublicOwner(
			TSharedPtr<FJsonObject>& OutPayload,
			FWorkspaceError& OutError)
		{
			TOptional<FUeAgentCommandRequest> Request;
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
			if (!ComposeRequest(TEXT("world.data_layers"), MakeShared<FJsonObject>(),
					Request, Dispatch, OutError))
				return false;
			const FUeAgentCommandResult Result = Dispatch->Submit(
				Request.GetValue(),
				Contract::FUeAgentCallerContext(TEXT("level-world-domain-query"),
					{TEXT("uai:query")}, Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				UE::UAI::HjsonWorkspace::DomainAuthoring::SetWorkspaceErrorFromCommandFailure(Result.GetDiagnostic(),
					TEXT("level_world_domain_data_layer_read_failed"), TEXT("world.data_layers"), OutError);
				return false;
			}
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryParseObject(Result.GetPayloadJson(), OutPayload, Error)
				|| !OutPayload.IsValid())
			{
				return Fail(OutError, Error.IsValid() ? AsLevelNativeResult(MoveTemp(Error)) : LevelNativeFact(TEXT("level_world_domain_data_layer_read_invalid"),
					TEXT("The data-layer query returned no result object."), TEXT("/command_result/payload"),
					MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))));
			}
			return true;
		}

		bool ReadDataLayerRefsViaPublicOwner(
			UWorld& World,
			TSet<FString>& OutRefs,
			FWorkspaceError& OutError)
		{
			OutRefs.Reset();
			AWorldDataLayers* WorldDataLayers = World.GetWorldDataLayers();
			if (!WorldDataLayers)
			{
				return true;
			}
			WorldDataLayers->ForEachDataLayerInstance(
				[&OutRefs](UDataLayerInstance* Instance)
				{
					if (IsValid(Instance))
					{
						OutRefs.Add(Instance->GetPathName());
					}
					return true;
				});
			return true;
		}



		bool ApplyDataLayers(
			UWorld& World,
			const TArray<TSharedPtr<FJsonValue>>* Layers,
			FWorkspaceError& OutError)
		{
			if (!Layers) return true;
			for (int32 LayerIndex = 0; LayerIndex < Layers->Num(); ++LayerIndex)
			{
				const auto& LayerValue = (*Layers)[LayerIndex];
				const FString LayerPointer = TEXT("/root/document/provider_extension/data_layers/") + FString::FromInt(LayerIndex);
				const TSharedPtr<FJsonObject> Layer = LayerValue.IsValid() && LayerValue->Type == EJson::Object ? LayerValue->AsObject() : nullptr;
				FString Type;
				FString ShortName;
				if (!Layer.IsValid())
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layer_identity_invalid"), TEXT("Each data layer must be an object."),
						LayerPointer, LevelValue(LayerValue), MakeShared<FJsonValueString>(TEXT("object"))));
				if (!Layer->TryGetStringField(TEXT("type"), Type) || Type != TEXT("uai.world.data_layer"))
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layer_identity_invalid"), TEXT("Data-layer type must identify a world data layer."),
						LayerPointer + TEXT("/type"), LevelField(Layer, TEXT("type")), MakeShared<FJsonValueString>(TEXT("uai.world.data_layer"))));
				if (!Layer->TryGetStringField(TEXT("short_name"), ShortName) || ShortName.TrimStartAndEnd().IsEmpty())
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layer_identity_invalid"), TEXT("Data-layer short_name must be a non-empty string."),
						LayerPointer + TEXT("/short_name"), LevelField(Layer, TEXT("short_name")), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
				TSharedRef<FJsonObject> Create = MakeShared<FJsonObject>();
				Create->SetStringField(TEXT("short_name"), ShortName);
				bool bPrivate = true;
				Layer->TryGetBoolField(TEXT("private"), bPrivate);
				Create->SetBoolField(TEXT("private"), bPrivate);
				for (const TCHAR* Field : {TEXT("asset_ref"), TEXT("parent_ref")})
				{
					FString Value;
					if (Layer->TryGetStringField(Field, Value))
						Create->SetStringField(Field, Value);
				}
				const TSharedPtr<FJsonObject>* InitialState = nullptr;
				if (Layer->TryGetObjectField(TEXT("initial_state"), InitialState)
					&& InitialState && InitialState->IsValid())
					Create->SetObjectField(TEXT("initial_state"), CopyObject(*InitialState));
				TSet<FString> BeforeRefs;
				if (!ReadDataLayerRefsViaPublicOwner(World, BeforeRefs, OutError))
					return false;
				TSharedPtr<FJsonObject> CreatePayload;
				if (!SubmitWorldMutation(TEXT("world.data_layer_instance.create"),
						Create, CreatePayload, OutError))
					return false;
				TSet<FString> AfterRefs;
				if (!ReadDataLayerRefsViaPublicOwner(World, AfterRefs, OutError))
					return false;
				TArray<FString> AddedRefs;
				for (const FString& Ref : AfterRefs)
				{
					if (!BeforeRefs.Contains(Ref)) AddedRefs.Add(Ref);
				}
				if (AddedRefs.Num() != 1)
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_data_layer_exact_reference_unresolved"),
						TEXT("Data-layer creation did not produce exactly one new instance reference."), TEXT("/data_layer_creation/new_reference_count"),
						MakeShared<FJsonValueNumber>(AddedRefs.Num()), MakeShared<FJsonValueNumber>(1), ShortName));
				}
				const FString ExactDataLayerRef = AddedRefs[0];

				const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
				if (!Layer->HasField(TEXT("actor_managed_ids"))) continue;
				if (!Layer->TryGetArrayField(TEXT("actor_managed_ids"), ActorIds)
					|| !ActorIds)
				{
					return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layer_actor_refs_invalid"),
						TEXT("Data-layer actor_managed_ids must be an array."), LayerPointer + TEXT("/actor_managed_ids"),
						LevelField(Layer, TEXT("actor_managed_ids")), MakeShared<FJsonValueString>(TEXT("array")), ShortName));
				}
				TArray<TSharedPtr<FJsonValue>> SubjectRefs;
				for (int32 ActorIdIndex = 0; ActorIdIndex < ActorIds->Num(); ++ActorIdIndex)
				{
					const auto& IdValue = (*ActorIds)[ActorIdIndex];
					const FString ActorIdPointer = LayerPointer + TEXT("/actor_managed_ids/") + FString::FromInt(ActorIdIndex);
					FString ManagedId;
					if (!IdValue.IsValid() || !IdValue->TryGetString(ManagedId))
					{
						return Fail(OutError, LevelInputFact(TEXT("level_world_domain_data_layer_actor_ref_invalid"),
							TEXT("A data-layer actor managed id must be a string."), ActorIdPointer,
							LevelValue(IdValue), MakeShared<FJsonValueString>(TEXT("string")), ShortName));
					}
					TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"),
						UeAgentLevelAuthoring::GetWorldPath(World));
					Target->SetStringField(TEXT("managed_id"), ManagedId);
					UE::UAI::Contract::FDiagnostic FindError;
					AActor* Actor = UeAgentLevelAuthoring::FindActorExact(
						World, Target, FindError);
					if (!Actor)
					{
						if (FindError.IsEmpty()) FindError = LevelNativeFact(TEXT("level_world_domain_data_layer_actor_unresolved"),
							TEXT("The data-layer actor selector did not resolve an actor."), TEXT("/actor/present"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ManagedId);
						if (FindError.Source.SourceFile.IsEmpty())
						{
							FindError.SourceKind = TEXT("related_input");
							FindError.SemanticPointer = ActorIdPointer;
						}
						return Fail(OutError, MoveTemp(FindError));
					}
					SubjectRefs.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
				}
				TSharedRef<FJsonObject> Relation = MakeShared<FJsonObject>();
				Relation->SetStringField(TEXT("relation_kind"),
					TEXT("actor_membership"));
				Relation->SetStringField(TEXT("mode"), TEXT("add"));
				Relation->SetArrayField(TEXT("subject_refs"), SubjectRefs);
				Relation->SetArrayField(TEXT("collection_refs"),
					{MakeShared<FJsonValueString>(ExactDataLayerRef)});
				TSharedPtr<FJsonObject> RelationPayload;
				if (!SubmitWorldMutation(TEXT("world.data_layer_relation.update"),
						Relation, RelationPayload, OutError))
					return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> MakeFrame()
		{
			TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
			Frame->SetNumberField(TEXT("dimension"), 3);
			Frame->SetStringField(TEXT("semantic_frame"), TEXT("ue_world"));
			Frame->SetStringField(TEXT("write_frame"), TEXT("ue_world"));
			Frame->SetStringField(TEXT("read_frame"), TEXT("ue_world"));
			Frame->SetStringField(TEXT("unit"), TEXT("centimeter"));
			return Frame;
		}

		bool ReadInputStringSet(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* Field,
			TSet<FString>& OutValues,
			FWorkspaceError& OutError)
		{
			OutValues.Reset();
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("input"), Input)
				|| !Input || !Input->IsValid()
				|| !(*Input)->HasField(Field))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!(*Input)->TryGetArrayField(Field, Values) || !Values)
			{
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_read_filter_invalid"),
					TEXT("The read filter must be an array."), TEXT("/params/input/") + FString(Field),
					LevelField(*Input, Field), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid()
					|| !Value->TryGetString(Text)
					|| Text.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_read_filter_invalid"),
						TEXT("Each read-filter item must be a non-empty string."), TEXT("/params/input/") + FString(Field) + TEXT("/") + FString::FromInt(Index),
						LevelValue(Value), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
				}
				OutValues.Add(MoveTemp(Text));
			}
			return true;
		}

		bool CaptureEditableProperties(
			UWorld& World,
			const TSharedPtr<FJsonObject>& Params,
			TArray<TSharedPtr<FJsonValue>>& OutProperties,
			FWorkspaceError& OutError)
		{
			OutProperties.Reset();
			TSet<FString> OwnerIds;
			TSet<FString> FieldIds;
			if (!ReadInputStringSet(
					Params, TEXT("owner_ids"), OwnerIds, OutError)
				|| !ReadInputStringSet(
					Params, TEXT("field_ids"), FieldIds, OutError))
			{
				return false;
			}
			if (OwnerIds.IsEmpty())
			{
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_property_owner_required"),
					TEXT("Reading editable properties requires at least one owner id."), TEXT("/params/input/owner_ids"),
					MakeShared<FJsonValueNumber>(OwnerIds.Num()), MakeShared<FJsonValueString>(TEXT("at least one owner id")), World.GetPathName()));
			}
			if (!EnsureAuthoringRegistered(OutError))
			{
				return false;
			}

			TMap<FString, UObject*> Objects;
			for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor))
				{
					continue;
				}
				Objects.Add(Actor->GetPathName(), Actor);
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				for (UActorComponent* Component : Components)
				{
					if (IsValid(Component))
					{
						Objects.Add(Component->GetPathName(), Component);
					}
				}
			}

			TArray<FString> SortedOwners = OwnerIds.Array();
			SortedOwners.Sort();
			for (const FString& OwnerId : SortedOwners)
			{
				UObject* const* Found = Objects.Find(OwnerId);
				if (!Found || !IsValid(*Found))
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_property_owner_unresolved"),
						TEXT("The requested property owner was not found in the target world."), TEXT("/property_owner/present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), OwnerId));
				}
				TArray<UeAgentAuthoringState::FReadRequest> Requests;
				UE::UAI::Contract::FDiagnostic ReadError;
				if (AActor* Actor = Cast<AActor>(*Found))
				{
					if (!UeAgentLevelAuthoring::BuildActorExportReadRequests(
							*Actor, Requests, ReadError))
					{
						OutError.SetFailure(MoveTemp(ReadError));
						return false;
					}
				}
				else if (UActorComponent* Component =
					Cast<UActorComponent>(*Found))
				{
					if (!UeAgentLevelAuthoring::BuildComponentExportReadRequests(
							*Component, Requests, ReadError))
					{
						OutError.SetFailure(MoveTemp(ReadError));
						return false;
					}
				}
				else
				{
					return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_property_owner_type_unsupported"),
						TEXT("Editable property export supports actors and actor components."), TEXT("/property_owner/type"),
						MakeShared<FJsonValueString>((*Found)->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("Actor or ActorComponent")), OwnerId));
				}

				UeAgentAuthoringState::FCaptureResult Capture;
				if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
				{
					OutError.SetFailure(MoveTemp(Capture.Diagnostic));
					return false;
				}
				for (const UeAgentAuthoringState::FStateEntry& Entry
					: Capture.Snapshot.CopyEntries())
				{
					const FString PropertyPath =
						Entry.TargetIdentity.GetSelector(TEXT("property_path"));
					if (PropertyPath.IsEmpty()
						|| (!FieldIds.IsEmpty()
							&& !FieldIds.Contains(PropertyPath)))
					{
						continue;
					}
					TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
					Property->SetStringField(TEXT("owner_id"), OwnerId);
					Property->SetStringField(TEXT("property_path"), PropertyPath);
					Property->SetBoolField(TEXT("available"), Entry.bAvailable);
					Property->SetBoolField(TEXT("editable"), Entry.bEditable);
					if (!Entry.AvailabilityReason.IsEmpty())
					{
						Property->SetStringField(
							TEXT("availability_reason"), Entry.AvailabilityReason);
					}
					if (TSharedPtr<FJsonObject> Type = Entry.Type.ToJsonObject();
						Type.IsValid())
					{
						Property->SetObjectField(TEXT("type"), Type);
					}
					if (Entry.bAvailable && Entry.Value.IsValid())
					{
						Property->SetField(
							TEXT("value"),
							UeAgentAuthoringState::CloneCanonicalValue(Entry.Value));
					}
					OutProperties.Add(MakeShared<FJsonValueObject>(Property));
				}
			}
			return true;
		}

		bool CaptureWorld(
			UWorld& World,
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TArray<AActor*> Actors;
			FWorldPartitionHelpers::FForEachActorWithLoadingResult
				WorldPartitionLoadResult;
			UeAgentLevelAuthoring::RetainWorldPartitionActorsForAuthoring(
				World, WorldPartitionLoadResult);
			for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
			{
				if (*It) Actors.AddUnique(*It);
			}
			Actors.Sort([](const AActor& Left, const AActor& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			if (CommandId == TEXT("content_graph.properties"))
			{
				TArray<TSharedPtr<FJsonValue>> Properties;
				if (!CaptureEditableProperties(
						World, Params, Properties, OutError))
				{
					return false;
				}
				TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
				Snapshot->SetStringField(TEXT("schema"), SchemaId);
				Snapshot->SetStringField(
					TEXT("type"), TEXT("/Script/Engine.World"));
				Snapshot->SetStringField(TEXT("target"), World.GetPathName());
				Snapshot->SetArrayField(TEXT("properties"), Properties);
				Snapshot->SetNumberField(
					TEXT("property_count"), Properties.Num());

				TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
				Response->SetStringField(TEXT("target"), World.GetPathName());
				Response->SetStringField(
					TEXT("root_type"), TEXT("/Script/Engine.World"));
				Response->SetStringField(TEXT("command_id"), CommandId);
				Response->SetBoolField(TEXT("complete"), true);
				Response->SetBoolField(TEXT("has_more"), false);
				Response->SetObjectField(TEXT("query_result"), Snapshot);
				return CanonicalObject(Response, OutPayload, OutError);
			}

			TSet<FString> ItemIds;
			if (CommandId == TEXT("content_graph.items")
				&& !ReadInputStringSet(
					Params, TEXT("item_ids"), ItemIds, OutError))
			{
				return false;
			}
			TArray<TSharedPtr<FJsonValue>> ActorValues;
			TSet<FString> ActorClasses;
			TSet<FString> ComponentClasses;
			for (AActor* Actor : Actors)
			{
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				Components.Sort([](const UActorComponent& Left,
					const UActorComponent& Right)
				{
					return Left.GetName() < Right.GetName();
				});
				const bool bActorSelected = ItemIds.IsEmpty()
					|| ItemIds.Contains(Actor->GetPathName());
				const bool bComponentSelected = Components.ContainsByPredicate(
					[&ItemIds](const UActorComponent* Component)
					{
						return Component
							&& ItemIds.Contains(Component->GetPathName());
					});
				if (!bActorSelected && !bComponentSelected)
				{
					continue;
				}
				TSharedRef<FJsonObject> ActorObject = MakeShared<FJsonObject>();
				ActorObject->SetStringField(TEXT("object_path"), Actor->GetPathName());
				ActorObject->SetStringField(TEXT("managed_id"),
					UeAgentLevelAuthoring::GetManagedActorId(*Actor));
				ActorObject->SetStringField(TEXT("type"),
					Actor->GetClass()->GetPathName());
				ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
				ActorObject->SetStringField(TEXT("level"), Actor->GetLevel()->GetPackage()->GetName());
				ActorObject->SetStringField(TEXT("folder_path"),
					UeAgentLevelAuthoring::GetActorFolderPathText(*Actor));
				ActorObject->SetObjectField(TEXT("transform"),
					UeAgentLevelAuthoring::BuildTransformState(
						Actor->GetActorTransform(),
						UeAgentLevelAuthoring::ETransformFrame::ActorWorld, true));
				TSharedRef<FJsonObject> Attachment = MakeShared<FJsonObject>();
				const AActor* Parent = Actor->GetAttachParentActor();
				Attachment->SetStringField(TEXT("parent_managed_id"),
					Parent ? UeAgentLevelAuthoring::GetManagedActorId(*Parent) : FString());
				Attachment->SetStringField(TEXT("socket_name"),
					Actor->GetAttachParentSocketName().IsNone()
						? FString()
						: Actor->GetAttachParentSocketName().ToString());
				ActorObject->SetObjectField(TEXT("attachment"), Attachment);
				TArray<FString> ActorDataLayerRefs;
				for (const UDataLayerInstance* Instance :
					Actor->GetDataLayerInstances())
				{
					if (Instance) ActorDataLayerRefs.Add(Instance->GetPathName());
				}
				ActorDataLayerRefs.Sort();
				TArray<TSharedPtr<FJsonValue>> ActorDataLayerValues;
				for (const FString& Ref : ActorDataLayerRefs)
					ActorDataLayerValues.Add(MakeShared<FJsonValueString>(Ref));
				ActorObject->SetArrayField(TEXT("data_layer_refs"),
					ActorDataLayerValues);
				TArray<TSharedPtr<FJsonValue>> ComponentValues;
				for (UActorComponent* Component : Components)
				{
					if (!Component
						|| (!bActorSelected
							&& !ItemIds.Contains(Component->GetPathName())))
					{
						continue;
					}
					TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
					ComponentObject->SetStringField(TEXT("object_path"),
						Component->GetPathName());
					ComponentObject->SetStringField(TEXT("component_name"),
						Component->GetName());
					ComponentObject->SetStringField(TEXT("type"),
						Component->GetClass()->GetPathName());
					if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
					{
						TSharedRef<FJsonObject> ComponentAttachment =
							MakeShared<FJsonObject>();
						ComponentAttachment->SetStringField(TEXT("parent_component"),
							Scene->GetAttachParent()
								? Scene->GetAttachParent()->GetName() : FString());
						ComponentAttachment->SetStringField(TEXT("socket_name"),
							Scene->GetAttachSocketName().IsNone()
								? FString() : Scene->GetAttachSocketName().ToString());
						ComponentAttachment->SetObjectField(TEXT("transform"),
							UeAgentLevelAuthoring::BuildTransformState(
								Scene->GetRelativeTransform(),
								UeAgentLevelAuthoring::ETransformFrame::
									ComponentParentRelative, true));
						ComponentObject->SetObjectField(TEXT("attachment"),
							ComponentAttachment);
					}
					ComponentClasses.Add(Component->GetClass()->GetPathName());
					ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
				}
				ActorObject->SetArrayField(TEXT("components"), ComponentValues);
				ActorClasses.Add(Actor->GetClass()->GetPathName());
				ActorValues.Add(MakeShared<FJsonValueObject>(ActorObject));
			}

			TArray<FString> SortedActorClasses = ActorClasses.Array();
			SortedActorClasses.AddUnique(TEXT("/Script/Engine.Actor"));
			SortedActorClasses.AddUnique(TEXT("/Script/Engine.StaticMeshActor"));
			SortedActorClasses.Sort();
			TArray<FString> SortedComponentClasses = ComponentClasses.Array();
			SortedComponentClasses.AddUnique(TEXT("/Script/Engine.SceneComponent"));
			SortedComponentClasses.Sort();
			auto Strings = [](const TArray<FString>& Values)
			{
				TArray<TSharedPtr<FJsonValue>> Result;
				for (const FString& Value : Values)
					Result.Add(MakeShared<FJsonValueString>(Value));
				return Result;
			};
			TSharedRef<FJsonObject> Candidates = MakeShared<FJsonObject>();
			Candidates->SetArrayField(TEXT("actor_classes"),
				Strings(SortedActorClasses));
			Candidates->SetArrayField(TEXT("component_classes"),
				Strings(SortedComponentClasses));
			TSharedPtr<FJsonObject> DataLayerPayload;
			FWorkspaceError DataLayerError;
			const bool bDataLayersRead =
				ReadDataLayersViaPublicOwner(DataLayerPayload, DataLayerError);
			TSet<FString> DataLayerRefs;
			FWorkspaceError DataLayerRefError;
			const bool bDataLayerRefsRead =
				ReadDataLayerRefsViaPublicOwner(World, DataLayerRefs,
					DataLayerRefError);
			TArray<FString> SortedDataLayerRefs = DataLayerRefs.Array();
			SortedDataLayerRefs.Sort();

			TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
			Snapshot->SetStringField(TEXT("schema"), SchemaId);
			Snapshot->SetStringField(TEXT("type"), TEXT("/Script/Engine.World"));
			Snapshot->SetStringField(TEXT("target"), World.GetPathName());
			Snapshot->SetObjectField(TEXT("frame"), MakeFrame());
			Snapshot->SetArrayField(TEXT("actors"), ActorValues);
			Snapshot->SetObjectField(TEXT("candidates"), Candidates);
			Snapshot->SetBoolField(TEXT("data_layers_read"), bDataLayersRead);
			Snapshot->SetBoolField(TEXT("data_layer_refs_read"),
				bDataLayerRefsRead);
			Snapshot->SetArrayField(TEXT("data_layer_refs"),
				Strings(SortedDataLayerRefs));
			if (bDataLayersRead)
				Snapshot->SetObjectField(TEXT("data_layers"), DataLayerPayload);
			else
				Snapshot->SetObjectField(TEXT("data_layers_diagnostic"), DataLayerError.GetDiagnostic().ToPublicJson());
			if (!bDataLayerRefsRead)
				Snapshot->SetObjectField(TEXT("data_layer_refs_diagnostic"), DataLayerRefError.GetDiagnostic().ToPublicJson());

			TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), World.GetPathName());
			Response->SetStringField(TEXT("root_type"),
				TEXT("/Script/Engine.World"));
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Snapshot);
			return CanonicalObject(Response, OutPayload, OutError);
		}

		bool ExportWorldWorkspace(
			UWorld& World,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayload,
			FWorkspaceError& OutError,
			HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport)
		{
			if (!EnsureAuthoringRegistered(OutError))
			{
				return false;
			}
			FString WorkspacePath;
			FString ProfileId(TEXT("llm_authoring"));
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath))
			{
				return Fail(OutError, LevelCommandFact(TEXT("content_graph_export_params_invalid"),
					TEXT("Export workspace_path must be a string."), TEXT("/params/workspace_path"),
					LevelField(Params, TEXT("workspace_path")), MakeShared<FJsonValueString>(TEXT("absolute directory path")), World.GetPathName()));
			}
			Params->TryGetStringField(TEXT("profile_id"), ProfileId);
			WorkspacePath.TrimStartAndEndInline();
			ProfileId.TrimStartAndEndInline();
			if (WorkspacePath.IsEmpty() || FPaths::IsRelative(WorkspacePath))
				return Fail(OutError, LevelCommandFact(TEXT("workspace_path_invalid"),
					TEXT("Export workspace_path must be a non-empty absolute path."), TEXT("/params/workspace_path"),
					MakeShared<FJsonValueString>(WorkspacePath), MakeShared<FJsonValueString>(TEXT("non-empty absolute path")), World.GetPathName()));
			if (ProfileId.IsEmpty())
				return Fail(OutError, LevelCommandFact(TEXT("workspace_path_invalid"),
					TEXT("Export profile_id must be non-empty."), TEXT("/params/profile_id"),
					MakeShared<FJsonValueString>(ProfileId), MakeShared<FJsonValueString>(TEXT("non-empty profile id")), World.GetPathName()));

			UeAgentLevelAuthoring::FLevelExportResult ExportResult;
			UE::UAI::Contract::FDiagnostic ExportError;
			if (PreparedExport) PreparedExport->bIncludeResultFileDetails = false;
			if (!UeAgentLevelAuthoring::ExportLevelWorkspace(World, WorkspacePath, ProfileId, ExportResult, PreparedExport)
				|| (PreparedExport && PreparedExport->bPrepareOnly ? !PreparedExport->Plan.IsValid() : !ExportResult.IsWritten()))
			{
				OutError.SetFailure(ExportResult.Diagnostic.IsEmpty() ? LevelNativeFact(TEXT("level_world_domain_export_failed"),
					TEXT("Level workspace export did not produce a written workspace receipt."), TEXT("/workspace_export/written"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), World.GetPathName()) : MoveTemp(ExportResult.Diagnostic));
                return false;
			}

			if (PreparedExport && PreparedExport->bPrepareOnly)
			{
				const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
				Response->SetStringField(TEXT("status"), TEXT("prepared"));
				return CanonicalObject(Response, OutPayload, OutError);
			}
			TSharedRef<FJsonObject> Response =
				UeAgentExportWorkspaceWriter::
					MakePublicWorkspaceSuccessResult(ExportResult.PublisherReceipt);
			return CanonicalObject(Response, OutPayload, OutError);
		}

		bool FinalizeWorldImport(const FString& Target, const FString& PackageName,
			UWorld& World, FString& OutPayload, FWorkspaceError& OutError, const FString& AnalysisRef)
		{
			// Persisted operation readback already reports actual values and warnings.
			// This response consumes only success and the optional requested analysis.
			UE::UAI::Contract::FDiagnostic JsonError;
			OutPayload = TEXT("{\"status\":\"succeeded\"}");
			if (AnalysisRef.IsEmpty()) return true;
			TSharedPtr<FJsonObject> PayloadObject;
			if (!Json::TryParseObject(OutPayload, PayloadObject, JsonError))
				return Fail(OutError, AsLevelNativeResult(MoveTemp(JsonError)));
			PayloadObject->SetObjectField(TEXT("scene_analysis"), SceneAnalysis::ReadApplied(World, AnalysisRef));
			return CanonicalObject(PayloadObject.ToSharedRef(), OutPayload, OutError);
		}

		bool ImportWorld(const FString& Target, const FString& PackageName,
			const FString& ObjectName, const FString& ExpectedRevision,
			const DomainAuthoring::FResolvedDomainHjsonDocument& DocumentSource, const FString& SemanticWorkspaceRoot,
			FString& OutPayload, FWorkspaceError& OutError, const FString& RequestId,
			const FString& WorkspaceProfileId, const FString& AnalysisRef)
		{
			const auto& Document = DocumentSource.Root;
			DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(DocumentSource, OutError);
			if (!EnsureAuthoringRegistered(OutError))
				return false;
			const TSharedPtr<FJsonObject>* RootDocument = nullptr;
			const TSharedPtr<FJsonObject>* Extension = nullptr;
			FString Type;
			FString Operation;
			if (SemanticWorkspaceRoot.IsEmpty())
			{
				const FString Code = TEXT("level_world_domain_root_document_invalid");
				if (!Document.IsValid())
					return Fail(OutError, LevelInputFact(Code, TEXT("The resolved world document is missing."), TEXT("/"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")), Target));
				if (!Document->TryGetStringField(TEXT("type"), Type) || Type != TEXT("/Script/Engine.World"))
					return Fail(OutError, LevelInputFact(Code, TEXT("World authoring requires the Engine.World document type."), TEXT("/type"),
						LevelField(Document, TEXT("type")), MakeShared<FJsonValueString>(TEXT("/Script/Engine.World")), Target));
				if (!Document->TryGetObjectField(TEXT("document"), RootDocument) || !RootDocument || !RootDocument->IsValid())
					return Fail(OutError, LevelInputFact(Code, TEXT("The world document field must be an object."), TEXT("/document"),
						LevelField(Document, TEXT("document")), MakeShared<FJsonValueString>(TEXT("object")), Target));
				if (!(*RootDocument)->TryGetStringField(TEXT("operation"), Operation)
					|| (Operation != TEXT("create") && Operation != TEXT("update")))
					return Fail(OutError, LevelInputFact(Code, TEXT("World document operation must be create or update."), TEXT("/document/operation"),
						LevelField(*RootDocument, TEXT("operation")), MakeShared<FJsonValueString>(TEXT("create or update")), Target));
				if (!(*RootDocument)->TryGetObjectField(TEXT("provider_extension"), Extension) || !Extension || !Extension->IsValid())
					return Fail(OutError, LevelInputFact(Code, TEXT("World provider_extension must be an object."), TEXT("/document/provider_extension"),
						LevelField(*RootDocument, TEXT("provider_extension")), MakeShared<FJsonValueString>(TEXT("object")), Target));
			}
			const bool bExists = FPackageName::DoesPackageExist(PackageName);
			if (SemanticWorkspaceRoot.IsEmpty() && (Operation == TEXT("create")) != !bExists)
			{
				return Fail(OutError, LevelNativeFact(Operation == TEXT("create") ? TEXT("level_world_domain_target_already_exists") : TEXT("level_world_domain_target_not_found"),
					Operation == TEXT("create") ? TEXT("World creation requires an absent target package.") : TEXT("World update requires an existing target package."),
					TEXT("/target/package/exists"), MakeShared<FJsonValueBoolean>(bExists), MakeShared<FJsonValueBoolean>(Operation != TEXT("create")), PackageName));
			}
			if (SemanticWorkspaceRoot.IsEmpty() && !bExists)
			{
				if (ExpectedRevision != TEXT("missing"))
				{
					return Fail(OutError, LevelRevisionMismatch(ExpectedRevision, TEXT("missing"), Target));
				}
			}
			else if (SemanticWorkspaceRoot.IsEmpty())
			{
				FString CurrentRevision;
				if (!StableRevision(PackageName, CurrentRevision, OutError))
					return false;
				if (ExpectedRevision != CurrentRevision)
				{
					return Fail(OutError, LevelRevisionMismatch(ExpectedRevision, CurrentRevision, Target));
				}
			}

			TSharedRef<FJsonObject> Bundle = MakeShared<FJsonObject>();
			TMap<FString, FString> SourceBindings;
			const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
			if (SemanticWorkspaceRoot.IsEmpty() &&
				!BuildLevelWorkspace(PackageName, *Extension, Bundle, DataLayers, OutError, SourceBindings))
				return false;
			bool bWorldPartition = DataLayers && !DataLayers->IsEmpty();
			if (Extension && *Extension)
				(*Extension)->TryGetBoolField(TEXT("world_partition"), bWorldPartition);
			FMapRestoreContext Restore;
			UWorld* World = nullptr;
			using namespace UeAgentAuthoringWorkspaceTransaction;
			FDurableTransactionCoordinator Durable;
			FDurabilityResult Durability;
			FOperationFailure DurableFailure;
			UeAgentLevelAuthoring::FLevelAuthoringCapture BeforeMutation;
			FWorldPartitionHelpers::FForEachActorWithLoadingResult WorldPartitionLoadResult;
			bool bNativeMutationStarted = false;
			const auto PrepareDurability = [&]()
			{
				FExecutionOptions Execution;
				Execution.RequestId = RequestId;
				Execution.JournalRoot = GetDurableTransactionRoot(true);
				if (Durable.Prepare(Execution, Target + TEXT(":") + RequestId,
					WorldDurableArtifacts(PackageName, World), Durability, DurableFailure)) return true;
				OutError.SetFailure(DurableFailure.Diagnostic);
				return false;
			};
			ON_SCOPE_EXIT
			{
				if (!Durable.IsPrepared())
				{
					WorldPartitionLoadResult.ActorReferences.Reset();
					if (Restore.bSwitched && OutError.HasErrors())
					{
						FWorkspaceError ContextError;
						if (!Restore.Restore(ContextError)) OutError.Append(ContextError);
					}
					return;
				}
				const FWorkspaceError Cause = OutError;
				WorldPartitionLoadResult.ActorReferences.Reset();
				World = nullptr;
				UE::UAI::Contract::FDiagnostic RecoveryError;
				bool bRecovered = !bNativeMutationStarted || ReleaseWorldForRestore(PackageName, !bExists, RecoveryError);
				if (bRecovered && !Durable.RestoreDiskArtifacts(Durability, DurableFailure))
				{
					bRecovered = false;
					RecoveryError.Append(DurableFailure.Diagnostic);
				}
				if (bRecovered && bNativeMutationStarted)
				{
					ULevel::ScanLevelAssets(PackageName);
					if (bExists)
					{
						World = UEditorLoadingAndSavingUtils::LoadMap(MapFilename(PackageName));
						bRecovered = World != nullptr;
						if (!bRecovered) RecoveryError = LevelNativeFact(TEXT("level_world_domain_recovery_reload_failed"),
							TEXT("The original persisted world could not be reloaded during recovery."), TEXT("/recovery/world_loaded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName);
						if (World && BeforeMutation.CanonicalSnapshot.IsValid())
						{
							UeAgentLevelAuthoring::RetainWorldPartitionActorsForAuthoring(*World, WorldPartitionLoadResult);
							bRecovered = UeAgentLevelAuthoring::VerifyLevelAuthoringReadback(*World,
								BeforeMutation.Snapshot, RecoveryError);
						}
					}
					else
					{
						bRecovered = !FPackageName::DoesPackageExist(PackageName);
						if (!bRecovered) RecoveryError = LevelNativeFact(TEXT("level_world_domain_recovery_package_remains"),
							TEXT("The new world package still exists after restoring the recorded absence."), TEXT("/recovery/package_exists"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), PackageName);
					}
				}
				WorldPartitionLoadResult.ActorReferences.Reset();
				FWorkspaceError ContextError;
				if (!Restore.Restore(ContextError))
				{
					bRecovered = false;
					RecoveryError.Append(ContextError.GetDiagnostic());
				}
				if (!Durable.FinalizeRestore(bRecovered, Durability, DurableFailure))
				{
					bRecovered = false;
					RecoveryError.Append(DurableFailure.Diagnostic);
				}
				auto Diagnostic = Cause.GetDiagnostic();
				Diagnostic.TransactionStatus = bRecovered ? TEXT("rolled_back") : TEXT("rollback_unconfirmed");
				Diagnostic.RollbackAttempted = true;
				Diagnostic.RollbackSucceeded = bRecovered;
				if (bRecovered) Diagnostic.AssetApplied = false;
				Diagnostic.bRetrySafe = bRecovered;
				if (!bRecovered)
					Diagnostic.Append(RecoveryError.IsValid() ? RecoveryError : LevelNativeFact(TEXT("level_world_domain_recovery_unconfirmed"),
						TEXT("World recovery failed without returning a diagnostic."), TEXT("/recovery/confirmed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
				OutPayload.Reset();
				OutError.SetFailure(MoveTemp(Diagnostic));
			};
			// The factory's first SaveMap is already a publication. Capture absence
			// before opening/creating the world, including future external packages.
			if (!bExists && !PrepareDurability()) return false;
			if (!OpenWorld(
					PackageName, ObjectName, !bExists, bWorldPartition, Restore, World, OutError,
					[&]() { bNativeMutationStarted = true; }))
				return false;
			const bool bPersistentUpdate = bExists && (!World->IsPartitionedWorld()
				|| (!SemanticWorkspaceRoot.IsEmpty() && (!DataLayers || DataLayers->IsEmpty())));
			if (!World->IsPartitionedWorld() && DataLayers && !DataLayers->IsEmpty())
			{
				OutError.SetFailure(LevelNativeFact(TEXT("level_world_domain_data_layers_require_partitioned_world"),
					TEXT("Applying data layers to an existing world requires World Partition."), TEXT("/world/partitioned"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
				FWorkspaceError RestoreError;
				if (!Restore.Restore(RestoreError)) OutError.Append(RestoreError);
				return false;
			}
			if (!bPersistentUpdate && World->GetOutermost()->IsDirty())
			{
				OutError.SetFailure(LevelNativeFact(TEXT("level_world_domain_target_world_dirty"),
					TEXT("The target world has unsaved changes before import."), TEXT("/target/package/dirty"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), PackageName));
				FWorkspaceError RestoreError;
				if (!Restore.Restore(RestoreError)) OutError.Append(RestoreError);
				return false;
			}
			if (!bPersistentUpdate) UeAgentLevelAuthoring::RetainWorldPartitionActorsForAuthoring(*World, WorldPartitionLoadResult);
			if (!bPersistentUpdate && bExists)
			{
				// These before-images represent disk state; do not discard unrelated
				// unsaved external actor or map build-data changes on recovery.
				TArray<UPackage*> OwnedPackages = World->GetOutermost()->GetExternalPackages();
				if (World->PersistentLevel && World->PersistentLevel->MapBuildData)
					OwnedPackages.AddUnique(World->PersistentLevel->MapBuildData->GetOutermost());
				for (UPackage* Owned : OwnedPackages)
					if (Owned->IsDirty())
						return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_target_world_dirty"),
							TEXT("An external or build-data package owned by the target world has unsaved changes."), TEXT("/target/owned_package/dirty"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Owned->GetName()));
				UE::UAI::Contract::FDiagnostic CaptureError;
				if (!UeAgentLevelAuthoring::TryCaptureLevelAuthoringState(*World, BeforeMutation, CaptureError))
				{
					OutError.SetFailure(CaptureError.IsEmpty() ? LevelNativeFact(TEXT("level_world_domain_capture_failed"),
						TEXT("World capture failed without returning a diagnostic."), TEXT("/world_capture/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName) : AsLevelNativeResult(MoveTemp(CaptureError)));
					return false;
				}
				BeforeMutation.Snapshot.ReleaseNativeValues();
				if (!PrepareDurability()) return false;
			}
			UeAgentLevelAuthoring::FWorkspaceApplyOptions Options;
			Options.bDryRun = false;
			Options.bUseEditorTransaction = true;
			Options.bPersistentReadback = bPersistentUpdate;
			Options.AuthoringProfileId = WorkspaceProfileId;
			if (bPersistentUpdate)
			{
				Options.FinalReadback = [&](UeAgentAuthoringOperationApply::FOperationFailure& Failure)
				{
					UWorld* Reloaded = FindObject<UWorld>(nullptr, *(PackageName + TEXT(".") + ObjectName));
					if (Reloaded && FinalizeWorldImport(Target, PackageName, *Reloaded, OutPayload, OutError, AnalysisRef))
					{
						return true;
					}
					Failure.Diagnostic = OutError.HasErrors() ? OutError.GetDiagnostic() : LevelNativeFact(TEXT("level_world_domain_reload_failed"),
						TEXT("The saved world object was not found for final readback."), TEXT("/final_readback/world_loaded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
					return false;
				};
			}
			UeAgentLevelAuthoring::FWorkspaceApplyResult ApplyResult;
			bool bApplied = false;
			if (!SemanticWorkspaceRoot.IsEmpty())
			{
				bApplied =
					ApplySemanticWorkspace(*World, SemanticWorkspaceRoot, RequestId, Options, ApplyResult, OutError, WorkspaceProfileId, AnalysisRef, bNativeMutationStarted);
			}
			else
			{
				bNativeMutationStarted = true;
				auto Source = UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(Bundle, TEXT("inline://level-world-domain"));
				Source.ResolveSource = [DocumentSource, SourceBindings](const FString& UnitPath, const FString& Pointer)
				{
					const FString* Binding = SourceBindings.Find(UnitPath);
					if (!Binding) return Contract::FSourceLocation();
					FString Relative = Pointer;
					Relative.RemoveFromStart(TEXT("/provider_extension"));
					Relative.RemoveFromStart(TEXT("/target"));
					if (Relative == TEXT("/actor_class") || Relative == TEXT("/component_class")) Relative = TEXT("/type");
					if (Relative == TEXT("/world_path") && UnitPath == TEXT("index.hjson")) Relative = TEXT("/target/asset_path");
					return DocumentSource.ResolveSource(*Binding + (Relative == TEXT("/") ? FString() : Relative));
				};
				bApplied = UeAgentLevelAuthoring::ApplyWorkspaceSource(*World,
					Source,
					Options, ApplyResult,
					RequestId);
			}
			if (!bApplied)
			{
                auto Diagnostic = OutError.HasErrors() ? OutError.GetDiagnostic() : ApplyResult.Diagnostic;
                if (Diagnostic.IsEmpty()) Diagnostic = LevelNativeFact(TEXT("level_world_domain_workspace_apply_failed"),
                    TEXT("The world workspace apply provider failed without returning a diagnostic."), TEXT("/workspace_apply/succeeded"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
                if (bPersistentUpdate)
                {
                    FWorkspaceError RestoreError;
                    const bool bRestored = Restore.Restore(RestoreError);
                    Diagnostic.RollbackAttempted = true;
                    Diagnostic.RollbackSucceeded = bRestored;
                    if (!bRestored) Diagnostic.Append(RestoreError.GetDiagnostic());
                }
                OutError.SetFailure(MoveTemp(Diagnostic));
                return false;
            }
            if (bPersistentUpdate)
            {
                FWorkspaceError RestoreError;
                if (!Restore.Restore(RestoreError))
                {
                    // The native/durable transaction is already committed.
                    // Context cleanup cannot authorize rollback or replay.
                    TSharedPtr<FJsonObject> Payload;
                    UE::UAI::Contract::FDiagnostic JsonError;
                    if (Json::TryParseObject(OutPayload, Payload, JsonError) && Payload)
                    {
                        Payload->SetBoolField(TEXT("editor_context_restored"), false);
                        Payload->SetBoolField(TEXT("retry_safe"), false);
                        auto Warning = RestoreError.GetDiagnostic();
                        Warning.Severity = Contract::ESeverity::Warning;
                        Warning.AssetApplied = true;
                        Warning.RetryImport = false;
                        TArray<TSharedPtr<FJsonValue>> Warnings;
                        const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
                        if (Payload->TryGetArrayField(TEXT("warnings"), Existing)) Warnings = *Existing;
                        Warnings.Add(MakeShared<FJsonValueObject>(Warning.ToPublicJson()));
                        Payload->SetArrayField(TEXT("warnings"), MoveTemp(Warnings));
                        return CanonicalObject(Payload.ToSharedRef(), OutPayload, OutError);
                    }
                    UE_LOG(LogTemp, Warning, TEXT("UAI committed world import: Editor context restoration failed: %s"),
                        *RestoreError.GetDiagnostic().PublicMessage);
                }
                return true;
            }
                        if (!ApplyDataLayers(*World, DataLayers, OutError)) {
                          return false;
                        }
			// The workspace already verified requested values and native relationships.
			// Carry that settled semantic state through this outer persistence branch.
			UeAgentLevelAuthoring::FLevelAuthoringCapture BeforeSave;
			UE::UAI::Contract::FDiagnostic ReadbackError;
			if (!UeAgentLevelAuthoring::TryCaptureLevelAuthoringState(*World, BeforeSave, ReadbackError))
			{
				OutError.SetFailure(MoveTemp(ReadbackError));
				return false;
			}
			// Only detached typed values may cross package unload/reload. Native
			// property buffers can retain object references owned by the old world.
			BeforeSave.Snapshot.ReleaseNativeValues();
			World->MarkPackageDirty();
			if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_save_failed"),
					TEXT("The editor failed to save the imported world."), TEXT("/world_import/map_saved"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			if (!Durable.RecordNativePublication(Durability, DurableFailure))
			{
				OutError.SetFailure(DurableFailure.Diagnostic);
				return false;
			}
			WorldPartitionLoadResult.ActorReferences.Reset();
			World = UEditorLoadingAndSavingUtils::LoadMap(MapFilename(PackageName));
			if (!World)
			{
				return Fail(OutError, LevelNativeFact(TEXT("level_world_domain_reload_failed"),
					TEXT("The saved imported world could not be reloaded."), TEXT("/world_import/world_reloaded"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName));
			}
			UeAgentLevelAuthoring::RetainWorldPartitionActorsForAuthoring(*World, WorldPartitionLoadResult);
                        if (!UeAgentLevelAuthoring::VerifyLevelAuthoringReadback(*World,
                            BeforeSave.Snapshot, ReadbackError)) {
                          auto Diagnostic = ReadbackError;
                          OutError.SetFailure(Diagnostic.IsEmpty() ? LevelNativeFact(TEXT("level_world_domain_readback_failed"),
                              TEXT("World readback verification failed without returning a diagnostic."), TEXT("/world_readback/verified"),
                              MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), PackageName) : MoveTemp(Diagnostic));
                          return false;
                        }
                        if (!FinalizeWorldImport(Target, PackageName, *World, OutPayload, OutError, AnalysisRef)) {
                          return false;
                        }
                        WorldPartitionLoadResult.ActorReferences.Reset();
                        FWorkspaceError RestoreError;
                        if (!Restore.Restore(RestoreError)) {
                          return Fail(OutError, RestoreError.GetDiagnostic());
                        }
                        if (!Durable.Commit(Durability, DurableFailure)) {
                          Restore.bSwitched = !Restore.OriginalPackage.IsEmpty();
                          OutError.SetFailure(DurableFailure.Diagnostic);
                          return false;
                        }
                        return true;
		}
				} // namespace

                bool TryQueryWorldActorCandidates(
                    const FUeAgentWorldActorCandidateQuery &Query,
                    FString &OutPayloadJson, FWorkspaceError &OutError) {
                  OutPayloadJson.Reset();
                  OutError.Reset();
                  FString Target = Query.Target.TrimStartAndEnd();
                  UE::UAI::ContentDiscovery::FContentDiscoveryError TargetError;
                  if (!UE::UAI::DomainAdapters::ContentDiscovery::TryResolveEditorContentReference(Target, TEXT("asset"), TargetError)) {
                    return Fail(OutError, MoveTemp(TargetError));
                  }
                  FString PackageName;
                  FString ObjectName;
                  if (!SplitTarget(Target, PackageName, ObjectName, OutError) ||
                      !FPackageName::DoesPackageExist(PackageName)) {
                    if (OutError.GetCode().IsEmpty()) {
                      return Fail(OutError, LevelNativeFact(TEXT("world_actor_candidate_target_unresolved"),
                          TEXT("The actor-candidate target package does not exist."), TEXT("/target/package/exists"),
                          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target));
                    }
                    return false;
                  }

                  UeAgentPaletteSearch::FSearchParams Params;
                  Params.CatalogType = TEXT("placeable_actor");
                  Params.SearchText = Query.Query.TrimStartAndEnd();
                  if (!Query.Category.TrimStartAndEnd().IsEmpty()) {
                    Params.CategoryFilters.Add(
                        Query.Category.TrimStartAndEnd());
                  }

                  UeAgentPaletteSearch::FSearchResult SearchResult;
                  UE::UAI::Contract::FDiagnostic SearchError;
                  if (!UeAgentPaletteSearch::ExecuteSearch(Params, SearchResult,
                                                           SearchError)) {
                    return Fail(OutError, MoveTemp(SearchError));
                  }

                  TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Entries;
                  UE::UAI::Contract::FDiagnostic ProjectionError;
                  for (const UeAgentPaletteSearch::FCandidate &Match :
                       SearchResult.MatchingCandidates) {
                    Entries.Add({Match.Name.IsEmpty() ? Match.DisplayName : Match.Name,
                                 Match.Identity.CanonicalKey, {}});
                  }
                  UE::UAI::ContentDiscovery::FNameDiscoveryCompleteness Completeness;
                  Completeness.TotalMatches = SearchResult.TotalMatching;
                  Completeness.ReturnedCount = Entries.Num();
                  if (!UE::UAI::ContentDiscovery::TryValidateNameDiscoveryCompleteness(
                          Completeness, ProjectionError)) {
                    return Fail(OutError, MoveTemp(ProjectionError));
                  }
		if (!UE::UAI::ContentDiscovery::TryBuildCandidateNamesOnlyPayload(
                          Entries,
                          UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase,
                          OutPayloadJson, ProjectionError)) {
                    return Fail(OutError, MoveTemp(ProjectionError));
                  }
                  const auto Scope = MakeShared<FJsonObject>();
                  Scope->SetStringField(TEXT("target"), Target);
                  if (!Query.Category.IsEmpty()) Scope->SetStringField(TEXT("category"), Query.Category);
                  if (!UE::UAI::ContentDiscovery::TryAddDiscoveryScope(Scope, OutPayloadJson, ProjectionError))
                    return Fail(OutError, MoveTemp(ProjectionError));
                  return true;
                }

        bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError,
		const FString& RequestId,
		HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport,
		const FString& WorkspaceProfileId)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			if (JsonError.IsEmpty()) JsonError = LevelCommandFact(TEXT("level_world_domain_params_invalid"),
				TEXT("Command params must be an object."), TEXT("/params"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")), TargetIdentity);
			else
			{
				JsonError.SourceKind = TEXT("command_input");
				if (JsonError.Source.InternalJsonPath.IsEmpty()) JsonError.Source.InternalJsonPath = TEXT("/params");
			}
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_target_identity_mismatch"),
				TEXT("Command target must be a string matching the bound target identity."), TEXT("/params/target"),
				LevelField(Params, TEXT("target")), MakeShared<FJsonValueString>(TargetIdentity), TargetIdentity));
		}
		FString PackageName;
		FString ObjectName;
		if (!SplitTarget(Target, PackageName, ObjectName, OutError)) return false;
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			if (RequestId.TrimStartAndEnd().IsEmpty())
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_request_id_required"),
					TEXT("World import requires a non-empty request id."), TEXT("/request_id"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("non-empty request id")), Target));
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_expected_revision_required"),
					TEXT("World import expected_revision must be a string."), TEXT("/params/expected_revision"),
					LevelField(Params, TEXT("expected_revision")), MakeShared<FJsonValueString>(TEXT("revision string")), Target));
			}
			FString SemanticWorkspaceRoot;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString SourceKind;
			if (Params->TryGetObjectField(TEXT("source"), Source)
				&& Source && Source->IsValid()
				&& (*Source)->TryGetStringField(TEXT("kind"), SourceKind)
				&& SourceKind == TEXT("semantic_workspace"))
			{
				FString SourceTarget;
				if (!(*Source)->TryGetStringField(TEXT("workspace_root"), SemanticWorkspaceRoot))
					return Fail(OutError, LevelCommandFact(TEXT("level_semantic_source_invalid"),
						TEXT("Semantic workspace_root must be a string."), TEXT("/params/source/workspace_root"),
						LevelField(*Source, TEXT("workspace_root")), MakeShared<FJsonValueString>(TEXT("absolute directory path")), Target));
				if (!(*Source)->TryGetStringField(TEXT("target_identity"), SourceTarget))
					return Fail(OutError, LevelCommandFact(TEXT("level_semantic_source_invalid"),
						TEXT("Semantic source target_identity must be a string."), TEXT("/params/source/target_identity"),
						LevelField(*Source, TEXT("target_identity")), MakeShared<FJsonValueString>(Target), Target));
				if (SourceTarget != Target)
					return Fail(OutError, LevelCommandFact(TEXT("level_semantic_source_invalid"),
						TEXT("Semantic source target_identity differs from the command target."), TEXT("/params/source/target_identity"),
						MakeShared<FJsonValueString>(SourceTarget), MakeShared<FJsonValueString>(Target), Target));
				if (SemanticWorkspaceRoot.IsEmpty() || FPaths::IsRelative(SemanticWorkspaceRoot))
					return Fail(OutError, LevelCommandFact(TEXT("level_semantic_source_invalid"),
						TEXT("Semantic workspace_root must be a non-empty absolute path."), TEXT("/params/source/workspace_root"),
						MakeShared<FJsonValueString>(SemanticWorkspaceRoot), MakeShared<FJsonValueString>(TEXT("non-empty absolute directory path")), Target));
			}
			DomainAuthoring::FResolvedDomainHjsonDocument Document;
			if (SemanticWorkspaceRoot.IsEmpty()
				&& !ParseDocument(Target, CanonicalParamsJson, Document, OutError)) return false;
			FString AnalysisRef;
			if (Params->HasField(TEXT("analysis_ref")))
			{
				if (!Params->TryGetStringField(TEXT("analysis_ref"), AnalysisRef) || AnalysisRef.IsEmpty()
					|| SemanticWorkspaceRoot.IsEmpty() || WorkspaceProfileId != TEXT("scene_editing"))
					return Fail(OutError, Contract::FDiagnostic::Failure(TEXT("scene_analysis_binding_invalid"),
						TEXT("analysis_ref requires a nonempty reference and a Level scene_editing workspace."),TEXT("scene_analysis")));
			}
			return ImportWorld(Target, PackageName, ObjectName, ExpectedRevision,
				Document, SemanticWorkspaceRoot, OutPayloadJson, OutError, RequestId, WorkspaceProfileId, AnalysisRef);
		}
		if (!CommandId.StartsWith(TEXT("content_graph.")))
		{
			return Fail(OutError, LevelCommandFact(TEXT("level_world_domain_command_not_supported"),
				TEXT("The level-world domain requires a content_graph command."), TEXT("/command_id"),
				MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph command")), Target));
		}
		const bool bTimeWorkspaceExport = CommandId == TEXT("content_graph.export_workspace");
		const double OpenStarted = bTimeWorkspaceExport ? FPlatformTime::Seconds() : 0;
		FMapRestoreContext Restore;
		UWorld* World = nullptr;
		if (!OpenWorld(PackageName, ObjectName, false, false,
				Restore, World, OutError))
			return false;
		const double OpenFinished = bTimeWorkspaceExport ? FPlatformTime::Seconds() : 0;
		// This branch only needs the existing persisted/readable/nonempty error
		// contract. Its former revision text had no consumer, including export.
		bool bCaptured = ReadPersistedMap(PackageName, nullptr, OutError);
		const double RevisionFinished = bTimeWorkspaceExport ? FPlatformTime::Seconds() : 0;
		if (bCaptured)
		{
			if (CommandId == TEXT("content_graph.export_workspace"))
			{
				bCaptured = ExportWorldWorkspace(
					*World, Params, OutPayloadJson, OutError, PreparedExport);
			}
			else
			{
				bCaptured = CaptureWorld(*World, CommandId, Params,
					OutPayloadJson, OutError);
			}
		}
		const double ExportFinished = bTimeWorkspaceExport ? FPlatformTime::Seconds() : 0;
		FWorkspaceError RestoreError;
		if (!Restore.Restore(RestoreError))
		{
			if (OutError.HasErrors()) { OutError.Append(RestoreError); return false; }
			return Fail(OutError, RestoreError.GetDiagnostic());
		}
		bOutReadOnly = bCaptured;
		if (bTimeWorkspaceExport) UE_LOG(LogTemp, Display,
			TEXT("UAI export timing phase=level_world_export_shell open_s=%.6f persisted_check_s=%.6f export_s=%.6f restore_s=%.6f total_s=%.6f persisted_revision_generated=false"),
			OpenFinished - OpenStarted, RevisionFinished - OpenFinished, ExportFinished - RevisionFinished,
			FPlatformTime::Seconds() - ExportFinished, FPlatformTime::Seconds() - OpenStarted);
		return bCaptured;
	}
}
