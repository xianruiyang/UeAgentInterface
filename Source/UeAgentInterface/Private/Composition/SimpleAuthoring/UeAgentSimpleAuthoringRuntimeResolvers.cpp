#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeIdentity.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Engine/DataAsset.h"
#include "Factories/Factory.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::AssetDataSimpleEffect;
		using namespace DomainAdapters::LandscapeGeometryEffect;
		using namespace DomainAdapters::ModelingGeometryEffect;
		using namespace DomainAdapters::WorldSimpleEffect;
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		const FString FactoryCatalogKind =
			TEXT("asset_data_factory");
		const FString FactoryCatalogSourceId =
			TEXT("asset-data-factory-catalog");
		const FString FactoryReadPortId =
			TEXT("asset-data-factory-candidate-read");
		const FString FactoryMechanicId =
			TEXT("uai.asset_data.factory:new_object:v1");
		const FString NativeFactoryMechanicId =
			TEXT("uai.asset_data.factory:asset_tools_create:v1");

		TSharedRef<FJsonValueObject> FactoryRevisionEvidence(const FUeAgentCatalogRevision& Value)
		{
			auto Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("source_id"), Value.GetSourceId());
			Object->SetStringField(TEXT("generation"), Value.GetGeneration());
			Object->SetStringField(TEXT("sequence"), ::LexToString(Value.GetSequence()));
			return MakeShared<FJsonValueObject>(Object);
		}

		const TArray<FString>& GenericNativeFactoryClasses()
		{
			// These factories need no additional interactive configuration. The
			// same declaration governs execution eligibility and menu-hidden
			// typed factories that only preconfigure their native parent factory.
			static const TArray<FString> Classes{
				TEXT("/Script/UnrealEd.WorldFactory"),
				TEXT("/Script/UnrealEd.MaterialFactoryNew"),
				TEXT("/Script/UnrealEd.MaterialFunctionFactoryNew"),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerFactory"),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerBlendFactory"),
				TEXT("/Script/UnrealEd.MaterialParameterCollectionFactoryNew"),
				TEXT("/Script/UnrealEd.MaterialFunctionInstanceFactory"),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerInstanceFactory"),
				TEXT("/Script/UnrealEd.MaterialFunctionMaterialLayerBlendInstanceFactory"),
				TEXT("/Script/UnrealEd.SpecularProfileFactory"),
				TEXT("/Script/UnrealEd.CurveVectorFactory"),
				TEXT("/Script/UnrealEd.CurveFloatFactory"),
				TEXT("/Script/UnrealEd.CurveLinearColorFactory"),
				TEXT("/Script/TextureGraphEditor.TG_Factory"),
				TEXT("/Script/OptimusEditor.OptimusSourceFactory"),
				TEXT("/Script/MLDeformerFrameworkEditor.MLDeformerFactory"),
				TEXT("/Script/Paper2DEditor.PaperTileSetFactory"),
				TEXT("/Script/Paper2DEditor.PaperSpriteAtlasFactory"),
			};
			return Classes;
		}

		// Candidate names are part of the model-facing discovery contract.  Keep
		// them derived from the reflected class name, not from localized editor UI
		// text, so catalog identity and public name projection remain stable.
		FString StableClassName(const UClass* Class)
		{
			if (!Class)
			{
				return FString();
			}
			return FName::NameToDisplayString(Class->GetName(), false);
		}

		FString StableClassModuleName(const UClass* Class)
		{
			if (!Class || !Class->GetOutermost())
			{
				return FString();
			}
			return FName::NameToDisplayString(
				FPackageName::GetShortName(Class->GetOutermost()->GetName()),
				false);
		}

		void SetError(FUeAgentSimpleAuthoringProductionError& OutError, const FString& Code,
			const FString& SubjectId = FString(), const FString& Reason = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"), FString(), SubjectId);
			OutError.Schema = TEXT("uai.runtime.diagnostic/1");
		}

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError = Cancelled;
				OutError.Stage = Stage;
				return false;
			}
			return true;
		}

		bool TryResolveExactObject(
			UWorld& SelectedWorld,
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError)
		{
			OutObject = nullptr;
			OutError = FExecutionMutationError();
			const FString Ref = CanonicalRef.TrimStartAndEnd();
			if (Ref.IsEmpty() || Ref != CanonicalRef)
			{
				OutError = FExecutionMutationError::Failure(TEXT("simple_authoring_reference_invalid"),
					Ref.IsEmpty() ? TEXT("The exact native object reference is empty.")
						: TEXT("The exact native object reference contains leading or trailing whitespace."), TEXT("execution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = CanonicalRef;
				OutError.PropertyPath = TEXT("/target");
				OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalRef);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Ref.IsEmpty() ? FString(TEXT("non-empty exact reference")) : Ref);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				return false;
			}
			UObject* Object = StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*Ref);
			if (!Object)
			{
				Object = StaticLoadObject(
					UObject::StaticClass(),
					nullptr,
					*Ref,
					nullptr,
					LOAD_NoWarn);
			}
			if (!IsValid(Object) || Object->GetPathName() != Ref)
			{
				const bool bObjectValid = IsValid(Object);
				OutError = FExecutionMutationError::Failure(TEXT("simple_authoring_reference_not_found"),
					!bObjectValid ? TEXT("No valid native object was found for the exact reference.")
						: TEXT("The resolved native object does not match the exact reference."), TEXT("execution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = CanonicalRef;
				OutError.PropertyPath = !bObjectValid ? TEXT("/resolved_object_valid") : TEXT("/resolved_object_matches_reference");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				if (bObjectValid) OutError.ActualEvidence = MakeShared<FJsonValueString>(Object->GetPathName());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Ref);
				return false;
			}
			UWorld* OwningWorld = Object->GetTypedOuter<UWorld>();
			if (OwningWorld && OwningWorld != &SelectedWorld)
			{
				OutError = FExecutionMutationError::Failure(TEXT("simple_authoring_reference_world_mismatch"),
					TEXT("The resolved native object belongs to a different world."), TEXT("execution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = CanonicalRef;
				OutError.PropertyPath = TEXT("/world_matches_requested");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValuePresent = true;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(OwningWorld->GetPathName());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(SelectedWorld.GetPathName());
				return false;
			}
			OutObject = Object;
			return true;
		}

		bool TryNormalizeAssetObjectPath(
			const FString& Input,
			FString& OutPackageName,
			FString& OutObjectPath,
			UE::UAI::Contract::FDiagnostic& OutError,
			const bool bAllowEngineRoot = false)
		{
			OutPackageName.Reset();
			OutObjectPath.Reset();
			OutError.Reset();
			if (Input.IsEmpty() || Input != Input.TrimStartAndEnd())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_data_asset_ref_invalid"), TEXT("Expected a permitted package path or matching top-level object path."), TEXT("input_decode"));
				return false;
			}
			OutPackageName = UeAgentAuthoringAssetIdentity::NormalizePackagePath(Input);
			OutObjectPath = UeAgentAuthoringAssetIdentity::ToObjectPath(Input);
			const bool bAllowedRoot = OutPackageName.StartsWith(
				TEXT("/Game/"), ESearchCase::CaseSensitive)
				|| (bAllowEngineRoot && OutPackageName.StartsWith(
					TEXT("/Engine/"), ESearchCase::CaseSensitive));
			if (!bAllowedRoot
				|| !FPackageName::IsValidLongPackageName(OutPackageName)
				|| (Input != OutPackageName && Input != OutObjectPath))
			{
				OutPackageName.Reset();
				OutObjectPath.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_data_asset_ref_invalid"), TEXT("Expected a permitted package path or matching top-level object path."), TEXT("input_decode"));
				return false;
			}
			return true;
		}

		FExecutionMutationError AssetReferenceError(
			const FString& Code, const TCHAR* Parameter, const TCHAR* Message)
		{
			return UE::UAI::Contract::MakeInputDiagnostic(
                    Code, FString(TEXT("/")) + Parameter, TEXT("asset_reference"), Message);
		}

		class FWorldReferenceResolver final
			: public IUeAgentWorldSimpleEffectReferenceResolver
		{
		public:
			bool TryResolveReference(
				UWorld& World,
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				return TryResolveExactObject(
					World,
					CanonicalRef,
					OutObject,
					OutError);
			}
		};

		class FLandscapeReferenceResolver final
			: public IUeAgentLandscapeGeometryEffectReferenceResolver
		{
		public:
			bool TryResolveReference(
				UWorld& World,
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				return TryResolveExactObject(
					World,
					CanonicalRef,
					OutObject,
					OutError);
			}
		};

		class FModelingReferenceResolver final
			: public IUeAgentModelingGeometryEffectReferenceResolver
		{
		public:
			bool TryResolveReference(
				UWorld& World,
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				return TryResolveExactObject(
					World,
					CanonicalRef,
					OutObject,
					OutError);
			}
		};

		class FAssetResolver final
			: public IUeAgentAssetDataSimpleEffectAssetResolver
		{
		public:
			bool TryResolveExisting(
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError();
				FString PackageName;
				FString ObjectPath;
				UE::UAI::Contract::FDiagnostic PathError;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						PathError))
				{
					OutError = PathError;
                    OutError.PropertyPath = TEXT("/target_ref");
                    OutError.PublicMessage = TEXT("Expected a /Game package path (/Game/Folder/Asset) or matching top-level object path (/Game/Folder/Asset.Asset).");
					return false;
				}
				UObject* Object = StaticFindObject(
					UObject::StaticClass(),
					nullptr,
					*ObjectPath);
				if (!Object)
				{
					Object = StaticLoadObject(
						UObject::StaticClass(),
						nullptr,
						*ObjectPath,
						nullptr,
						LOAD_NoWarn);
				}
				if (!Object)
				{
					UPackage* Package = FindPackage(nullptr, *PackageName);
					if (!Package)
					{
						Package = LoadPackage(
							nullptr,
							*PackageName,
							LOAD_NoWarn);
					}
					if (UWorld* World = Package
						? UWorld::FindWorldInPackage(Package)
						: nullptr)
					{
						if (World->PersistentLevel
							&& World->GetPathName() == ObjectPath)
						{
							Object = World;
						}
					}
				}
				if (!IsValid(Object)
					|| Object->GetPathName() != ObjectPath
					|| (Object->IsA<UWorld>()
						&& !CastChecked<UWorld>(Object)->PersistentLevel))
				{
					OutError = AssetReferenceError(
						TEXT("asset_data_existing_not_found"),
						TEXT("target_ref"), TEXT("The target asset could not be loaded at the supplied path."));
					return false;
				}
				OutObject = Object;
				return true;
			}

			bool TryResolveCloneSource(
				const FString& CanonicalRef,
				UObject*& OutObject,
				FExecutionMutationError& OutError) override
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError();
				FString PackageName;
				FString ObjectPath;
				UE::UAI::Contract::FDiagnostic PathError;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						PathError,
						true))
				{
					OutError = PathError;
                    OutError.PropertyPath = TEXT("/source_ref");
                    OutError.PublicMessage = TEXT("Expected a /Game or /Engine package path, or matching top-level object path (Package.Asset).");
					return false;
				}
				OutObject = StaticFindObject(
					UObject::StaticClass(), nullptr, *ObjectPath);
				if (!OutObject)
				{
					OutObject = StaticLoadObject(
						UObject::StaticClass(), nullptr, *ObjectPath,
						nullptr, LOAD_NoWarn);
				}
				if (!IsValid(OutObject)
					|| OutObject->GetPathName() != ObjectPath)
				{
					OutObject = nullptr;
					OutError = AssetReferenceError(
						TEXT("asset_data_clone_source_not_found"),
						TEXT("source_ref"), TEXT("The source asset could not be loaded at the supplied path."));
					return false;
				}
				return true;
			}

			bool TryResolveCreationTarget(
				const FString& CanonicalRef,
				UPackage*& OutPackage,
				FName& OutObjectName,
				FExecutionMutationError& OutError) override
			{
				OutPackage = nullptr;
				OutObjectName = NAME_None;
				OutError = FExecutionMutationError();
				FString PackageName;
				FString ObjectPath;
				UE::UAI::Contract::FDiagnostic PathError;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						PathError))
				{
					OutError = PathError;
                    OutError.PropertyPath = TEXT("/target_ref");
                    OutError.PublicMessage = TEXT("Expected a /Game package path (/Game/Folder/Asset) or matching top-level object path (/Game/Folder/Asset.Asset).");
					return false;
				}
				if (StaticFindObject(
						UObject::StaticClass(),
						nullptr,
						*ObjectPath)
					|| StaticLoadObject(
						UObject::StaticClass(),
						nullptr,
						*ObjectPath,
						nullptr,
						LOAD_NoWarn))
				{
					OutError = AssetReferenceError(
						TEXT("asset_data_creation_target_exists"),
						TEXT("target_ref"), TEXT("An asset already exists at this target; choose a new target path."));
					return false;
				}
				UPackage* Package = CreatePackage(*PackageName);
				const FString AssetName =
					FPackageName::GetLongPackageAssetName(PackageName);
				if (!Package || AssetName.IsEmpty())
				{
					OutError = FExecutionMutationError::Failure(TEXT("asset_data_creation_package_unavailable"),
						!Package ? TEXT("The target package could not be created.")
							: TEXT("The target package has no asset name."), TEXT("execution"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = CanonicalRef;
					if (!Package)
					{
						OutError.PropertyPath = TEXT("/package_exists");
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					else
					{
						OutError.PropertyPath = TEXT("/asset_name");
						OutError.ActualValue = MakeShared<FJsonValueString>(AssetName);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty asset name"));
					}
					return false;
				}
				OutPackage = Package;
				OutObjectName = FName(*AssetName);
				return true;
			}
		};

		struct FFactoryCandidateRecord
		{
			FUeAgentAssetDataSimpleEffectProductionProfile Profile;
			TWeakObjectPtr<UClass> ConcreteClass;
			TWeakObjectPtr<UFactory> Factory;
			FString StableName;
			FString ClassPath;
			FString FactoryClassPath;
			FString MechanicId;
			FString PublicRef;
			FString BindingId;
			TArray<FString> DiscoveryScopes;
			bool bGenericReady = false;

			bool IsValid() const
			{
				return Profile.UsesFactoryCandidate()
					&& ConcreteClass.IsValid()
					&& !StableName.IsEmpty()
					&& !ClassPath.IsEmpty()
					&& !PublicRef.IsEmpty()
					&& !BindingId.IsEmpty()
					&& (Profile.ProfileId
							!= TEXT("generic_default_create")
						|| (MechanicId == NativeFactoryMechanicId
							? Factory.IsValid()
								&& !FactoryClassPath.IsEmpty()
							: MechanicId == FactoryMechanicId));
			}
		};

		FUeAgentAssetDataSimpleEffectFactoryBinding BuildFactoryBinding(
			const FFactoryCandidateRecord& Record,
			const FUeAgentCatalogRevision& Revision);

		UClass* LoadClassExact(const FString& ClassPath)
		{
			UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *ClassPath);
			}
			return Class;
		}

		void AddCandidateClass(
			const FUeAgentAssetDataSimpleEffectProductionProfile& Profile,
			UClass* Class,
			TArray<FFactoryCandidateRecord>& OutRecords)
		{
			if (!Class)
			{
				return;
			}
			const FString ClassPath = Class->GetPathName();
			FFactoryCandidateRecord Record;
			Record.Profile = Profile;
			Record.MechanicId = FactoryMechanicId;
			Record.ConcreteClass = Class;
			Record.StableName = StableClassName(Class);
			Record.ClassPath = ClassPath;
			Record.DiscoveryScopes = {
				TEXT("constructor"), StableClassModuleName(Class)};
			Record.PublicRef =
				TEXT("uai://factory/asset-data/")
				+ Profile.ProfileId
				+ TEXT("/")
				+ RuntimePrivate::EncodeUriSegment(ClassPath);
			Record.BindingId =
				TEXT("uai.asset_data.factory_binding:")
				+ Profile.ProfileId
				+ TEXT(":")
				+ RuntimePrivate::Sha256(
					Profile.ProfileId + TEXT("\n") + ClassPath).Mid(7);
			if (Record.IsValid())
			{
				OutRecords.Add(MoveTemp(Record));
			}
		}

		void AddGenericConstructorCandidate(
			const FUeAgentAssetDataSimpleEffectProductionProfile& GenericProfile,
			UClass* Class,
			TArray<FFactoryCandidateRecord>& OutRecords)
		{
			if (!Class
				|| Class->HasAnyClassFlags(
					CLASS_Abstract
						| CLASS_Deprecated
						| CLASS_NewerVersionExists))
			{
				return;
			}
			const FString ClassPath = Class->GetPathName();
			FFactoryCandidateRecord Record;
			Record.Profile = GenericProfile;
			Record.ConcreteClass = Class;
			Record.MechanicId = FactoryMechanicId;
			Record.StableName = StableClassName(Class);
			Record.ClassPath = ClassPath;
			Record.DiscoveryScopes = {
				TEXT("constructor"), StableClassModuleName(Class)};
			Record.PublicRef =
				TEXT("uai://factory/asset-data/generic_default_create/constructor/")
				+ RuntimePrivate::EncodeUriSegment(ClassPath);
			const FString CandidateHash = RuntimePrivate::Sha256(
				Record.MechanicId + TEXT("\n") + ClassPath);
			Record.BindingId =
				TEXT("uai.asset_data.factory_binding:generic_default_create:")
				+ CandidateHash.Mid(7);
			Record.bGenericReady = true;
			if (Record.IsValid())
			{
				OutRecords.Add(MoveTemp(Record));
			}
		}

		void AddNativeFactoryCandidate(
			const FUeAgentAssetDataSimpleEffectProductionProfile& Profile,
			UFactory* Factory,
			TArray<FFactoryCandidateRecord>& OutRecords)
		{
			if (!Factory
				|| !Factory->HasAnyFlags(RF_ClassDefaultObject)
				|| !Factory->GetClass()
				|| !Factory->GetSupportedClass())
			{
				return;
			}
			FFactoryCandidateRecord Record;
			Record.Profile = Profile;
			Record.MechanicId = NativeFactoryMechanicId;
			Record.Factory = Factory;
			Record.ConcreteClass = Factory->GetSupportedClass();
			Record.StableName = StableClassName(Factory->GetSupportedClass());
			Record.ClassPath = Factory->GetSupportedClass()->GetPathName();
			Record.FactoryClassPath = Factory->GetClass()->GetPathName();
			Record.DiscoveryScopes = {
				StableClassName(Factory->GetClass()),
				StableClassModuleName(Factory->GetClass())};
			const FString CandidateHash = RuntimePrivate::Sha256(
				Record.FactoryClassPath + TEXT("\n") + Record.ClassPath);
			Record.PublicRef = TEXT("factory_")
				+ CandidateHash.Mid(7, 16);
			Record.BindingId =
				TEXT("uai.asset_data.factory_binding:generic_default_create:")
				+ CandidateHash.Mid(7);
			const bool bSupportedSlice = GenericNativeFactoryClasses().Contains(Record.FactoryClassPath);
			Record.bGenericReady = bSupportedSlice && CanCreateNativeFactoryObject(Factory);
			if (Record.IsValid())
			{
				OutRecords.Add(MoveTemp(Record));
			}
		}

		void AppendFactoryRevisionInput(
			const FFactoryCandidateRecord& Record,
			FString& InOutRevisionInput)
		{
			InOutRevisionInput += Record.Profile.FactoryContractId
				+ TEXT("\n")
				+ Record.Profile.ClassConstraintId
				+ TEXT("\n");
			if (Record.Profile.ProfileId
				== TEXT("generic_default_create"))
			{
				InOutRevisionInput += Record.StableName
					+ TEXT("\n")
					+ Record.ClassPath
					+ TEXT("\n")
					+ Record.FactoryClassPath
					+ TEXT("\n")
					+ Record.MechanicId
					+ TEXT("\n")
					+ (Record.bGenericReady
						? TEXT("ready")
						: TEXT("interactive_required"))
					+ TEXT("\n");
				return;
			}
			InOutRevisionInput += Record.ClassPath + TEXT("\n");
		}

		bool TryBuildFactoryRecords(
			TArray<FFactoryCandidateRecord>& OutRecords,
			FUeAgentCatalogRevision& OutRevision,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutRecords.Reset();
			OutError.Reset();
			TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
			if (!TryGetAssetDataSimpleEffectProductionProfiles(
					Profiles,
					OutError))
			{
				return false;
			}
			const FUeAgentAssetDataSimpleEffectProductionProfile* GenericProfile =
				Profiles.FindByPredicate(
					[](const FUeAgentAssetDataSimpleEffectProductionProfile& Profile)
					{
						return Profile.CapabilityId == TEXT("asset.object.create");
					});
			if (!GenericProfile || !GenericProfile->UsesFactoryCandidate())
			{
				OutError = FDiagnostic::Failure(TEXT("asset_data_generic_factory_profile_unavailable"),
					GenericProfile ? TEXT("The asset.object.create profile does not provide factory candidates.")
						: TEXT("No production profile is registered for asset.object.create."), TEXT("factory_catalog"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = TEXT("asset.object.create");
				OutError.PropertyPath = GenericProfile ? TEXT("factory_candidate") : TEXT("profile");
				OutError.ActualValuePresent = GenericProfile != nullptr;
				OutError.ExpectedValuePresent = true;
				if (GenericProfile) OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				return false;
			}
			for (const FUeAgentAssetDataSimpleEffectProductionProfile& Profile
				: Profiles)
			{
				if (Profile.UsesStaticCreateClass())
				{
					AddGenericConstructorCandidate(
						*GenericProfile,
						LoadClassExact(Profile.StaticCreateClassPath),
						OutRecords);
					continue;
				}
				if (!Profile.UsesFactoryCandidate())
				{
					continue;
				}
				if (Profile.ProfileId == TEXT("generic_default_create"))
				{
					// PCG data assets have a native UObject constructor, but no
					// dedicated interactive factory. Use the shared constructor path.
					AddGenericConstructorCandidate(Profile, LoadClassExact(TEXT("/Script/PCG.PCGDataAsset")), OutRecords);
					FAssetToolsModule* AssetToolsModule =
						FModuleManager::GetModulePtr<FAssetToolsModule>(
							TEXT("AssetTools"));
					if (!AssetToolsModule)
					{
						AssetToolsModule =
							FModuleManager::LoadModulePtr<FAssetToolsModule>(
								TEXT("AssetTools"));
					}
					if (!AssetToolsModule)
					{
						OutError = FDiagnostic::Failure(TEXT("asset_data_native_factory_catalog_unavailable"),
							TEXT("The AssetTools module could not be loaded to enumerate native asset factories."), TEXT("factory_catalog"));
						OutError.SourceKind = TEXT("native_result");
						OutError.PropertyPath = TEXT("modules/AssetTools");
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
						return false;
					}
					for (UFactory* Factory
						: AssetToolsModule->Get().GetNewAssetFactories())
					{
						AddNativeFactoryCandidate(
							Profile,
							Factory,
							OutRecords);
					}
					for (const FString& FactoryPath : GenericNativeFactoryClasses())
					{
						UClass* FactoryClass = LoadClassExact(FactoryPath);
						if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass())
							|| FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
						UFactory* Factory = FactoryClass->GetDefaultObject<UFactory>();
						// Supplement only menu-hidden declared factories. Factories
						// excluded by menu permissions are never reintroduced here.
						if (!Factory || (Factory->ShouldShowInNewMenu() && Factory->CanCreateNew()) || Factory->GetDisplayName().IsEmpty()
							|| !CanCreateNativeFactoryObject(Factory)
							|| !AssetToolsModule->Get().IsAssetClassSupported(Factory->GetSupportedClass())) continue;
						AddNativeFactoryCandidate(Profile, Factory, OutRecords);
					}
				}
				else if (Profile.ProfileId == TEXT("audio_effect_preset"))
				{
					UClass* CandidateClass = LoadClassExact(
						TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset"));
					AddCandidateClass(
						Profile,
						CandidateClass,
						OutRecords);
					AddGenericConstructorCandidate(
						*GenericProfile, CandidateClass, OutRecords);
				}
				else if (Profile.ProfileId == TEXT("sound_submix"))
				{
					UClass* CandidateClass =
						LoadClassExact(TEXT("/Script/Engine.SoundSubmix"));
					AddCandidateClass(
						Profile,
						CandidateClass,
						OutRecords);
					AddGenericConstructorCandidate(
						*GenericProfile, CandidateClass, OutRecords);
				}
				else if (Profile.ProfileId == TEXT("media_source"))
				{
					UClass* CandidateClass = LoadClassExact(
						TEXT("/Script/MediaAssets.FileMediaSource"));
					AddCandidateClass(
						Profile,
						CandidateClass,
						OutRecords);
					AddGenericConstructorCandidate(
						*GenericProfile, CandidateClass, OutRecords);
				}
				else if (Profile.ProfileId == TEXT("data_object"))
				{
					TArray<TStrongObjectPtr<UClass>> LoadedClasses;
					if (!ContentDiscovery::TryLoadDerivedBlueprintClasses(*UDataAsset::StaticClass(), LoadedClasses, OutError)) return false;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						AddCandidateClass(Profile, *It, OutRecords);
					}
				}
			}
			OutRecords.Sort(
				[](const FFactoryCandidateRecord& Left,
					const FFactoryCandidateRecord& Right)
				{
					const int32 ProfileOrder =
						Left.Profile.ProfileId.Compare(
							Right.Profile.ProfileId,
							ESearchCase::CaseSensitive);
					if (ProfileOrder != 0)
					{
						return ProfileOrder < 0;
					}
					if (Left.Profile.ProfileId
						== TEXT("generic_default_create"))
					{
						const int32 NameOrder = Left.StableName.Compare(
							Right.StableName, ESearchCase::CaseSensitive);
						return NameOrder == 0
							? Left.PublicRef < Right.PublicRef
							: NameOrder < 0;
					}
					return Left.ClassPath < Right.ClassPath;
				});
			FString RevisionInput;
			for (const FFactoryCandidateRecord& Record : OutRecords)
			{
				AppendFactoryRevisionInput(Record, RevisionInput);
			}
			OutRevision = FUeAgentCatalogRevision(
				FactoryCatalogSourceId,
				RuntimePrivate::Sha256(RevisionInput),
				1);
			const int32 CandidateCount = OutRecords.Num();
			FDiagnostic RejectedCandidates;
			OutRecords.RemoveAll(
				[&OutRevision, &RejectedCandidates](const FFactoryCandidateRecord& Record)
				{
					const FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
						BuildFactoryBinding(Record, OutRevision);
					FDiagnostic ValidationError;
					if (Record.Profile.ProfileId == TEXT("generic_default_create") && !Record.bGenericReady)
					{
						if (Binding.IsValid() && Binding.GetCandidateBinding()) return false;
						TryValidateAssetDataSimpleEffectFactoryBinding(
							Record.Profile.NativeBindingId, Record.BindingId, OutRevision, Binding, ValidationError);
					}
					else if (TryValidateAssetDataSimpleEffectFactoryBinding(
						Record.Profile.NativeBindingId, Record.BindingId, OutRevision, Binding, ValidationError))
						return false;
					if (ValidationError.CandidateName.IsEmpty()) ValidationError.CandidateName = Record.StableName;
					RejectedCandidates.Append(ValidationError);
					return true;
				});
			RevisionInput.Reset();
			for (const FFactoryCandidateRecord& Record : OutRecords)
			{
				AppendFactoryRevisionInput(Record, RevisionInput);
			}
			OutRevision = FUeAgentCatalogRevision(
				FactoryCatalogSourceId,
				RuntimePrivate::Sha256(RevisionInput),
				1);
			if (!OutRevision.IsValid())
			{
				OutError = *OutRevision.GetValidationDiagnostic();
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (OutRecords.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("factory_catalog_unavailable"),
					CandidateCount == 0 ? TEXT("No native or generic asset factory candidates were discovered.")
						: TEXT("All discovered asset factory candidates failed binding validation."), TEXT("factory_catalog"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("factory_candidates");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one valid candidate"));
				OutError.Append(RejectedCandidates);
				return false;
			}
			return true;
		}

		FUeAgentAssetDataSimpleEffectFactoryBinding BuildFactoryBinding(
			const FFactoryCandidateRecord& Record,
			const FUeAgentCatalogRevision& Revision)
		{
			const bool bGeneric = Record.Profile.ProfileId
				== TEXT("generic_default_create");
			FString CandidateIdentityInput =
				Record.Profile.FactoryContractId
				+ TEXT("\n")
				+ Record.Profile.ClassConstraintId
				+ TEXT("\n");
			if (bGeneric)
			{
				CandidateIdentityInput += Record.StableName
					+ TEXT("\n")
					+ Record.ClassPath
					+ TEXT("\n")
					+ Record.FactoryClassPath
					+ TEXT("\n")
					+ Record.MechanicId
					+ TEXT("\n")
					+ (Record.bGenericReady
						? TEXT("ready")
						: TEXT("interactive_required"));
			}
			else
			{
				CandidateIdentityInput += Record.ClassPath;
			}
			const FString CandidateHash =
				RuntimePrivate::Sha256(CandidateIdentityInput);
			TArray<FString> Categories{
				TEXT("Asset"),
				TEXT("Factory"),
				Record.Profile.ProfileId,
			};
			if (bGeneric)
			{
				Categories.Add(
					Record.bGenericReady
						? TEXT("ready")
						: TEXT("interactive_required"));
			}
			return FUeAgentAssetDataSimpleEffectFactoryBinding(
				FUeAgentCandidateBinding(
					Record.BindingId,
					FUeAgentCandidateSummary(
						FUeAgentCanonicalIdentity(
							TEXT("uai.asset_data.factory:")
								+ Record.PublicRef,
							TEXT("factory_candidate"),
							FactoryCatalogSourceId,
							Record.PublicRef,
							TEXT("asset_data_factory"),
							Revision.GetGeneration()),
						FUeAgentDescriptorIdentity(
							Record.Profile.FactoryContractId,
							1,
							CandidateHash),
						FactoryCatalogKind,
							Record.StableName,
							Record.ClassPath,
							MoveTemp(Categories),
							Record.DiscoveryScopes),
					Revision,
					Record.PublicRef,
					FactoryReadPortId,
					Record.bGenericReady || !bGeneric
						? TArray<EUeAgentCandidateOperation>{
							EUeAgentCandidateOperation::Create,
						}
						: TArray<EUeAgentCandidateOperation>{
							EUeAgentCandidateOperation::Read,
						},
					Record.Profile.ClassConstraintId),
				Record.Profile.ProfileId,
				Record.ConcreteClass.Get(),
				Record.MechanicId,
				Record.Factory.Get(),
				Record.bGenericReady);
		}

		class FFactoryCatalog final
			: public IUeAgentCatalogSource
			, public IUeAgentAssetDataSimpleEffectFactoryCandidateResolver
		{
		public:
			FFactoryCatalog()
				: Kinds({FactoryCatalogKind})
			{
			}

			const FString& GetSourceId() const override
			{
				return FactoryCatalogSourceId;
			}

			const TArray<FString>& GetCatalogKinds() const override
			{
				return Kinds;
			}

			bool TrySearch(
				const FUeAgentDiscoveryQuery& Query,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
				FUeAgentDiscoveryError& OutError) override
			{
				TArray<TSharedRef<const FUeAgentCandidateBinding>> Bindings;
				return TrySearchResolved(
					Query,
					Resources,
					Deadline,
					Cancellation,
					OutCatalog,
					Bindings,
					OutError);
			}

			bool TrySearchResolved(
				const FUeAgentDiscoveryQuery& Query,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
				TArray<TSharedRef<const FUeAgentCandidateBinding>>& OutBindings,
				FUeAgentDiscoveryError& OutError) override
			{
				OutCatalog.Reset();
				OutBindings.Reset();
				OutError = {};
				if (!Query.IsValid())
				{
					OutError = *Query.GetValidationDiagnostic();
					return false;
				}
				if (Query.GetCatalogKind() != FactoryCatalogKind)
				{
					OutError = FDiagnostic::Failure(TEXT("factory_catalog_query_invalid"),
						TEXT("The factory resolver received a query for a different catalog kind."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("catalog_kind");
					OutError.ActualValue = MakeShared<FJsonValueString>(Query.GetCatalogKind());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(FactoryCatalogKind);
					return false;
				}
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("asset_data_factory.search"),
						OutError))
				{
					return false;
				}
				TArray<FFactoryCandidateRecord> Records;
				FUeAgentCatalogRevision Revision(
					TEXT("invalid"),
					TEXT("invalid"),
					-1);
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildFactoryRecords(
						Records,
						Revision,
						BuildError))
				{
					OutError = BuildError;
					return false;
				}
				const FString Search = Query.GetSearchText().TrimStartAndEnd();
				TArray<FUeAgentCandidateSummary> Candidates;
				for (const FFactoryCandidateRecord& Record : Records)
				{
					const FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
						BuildFactoryBinding(Record, Revision);
					const FUeAgentCandidateBinding* Candidate =
						Binding.GetCandidateBinding();
					if (!Candidate || !Binding.IsValid())
					{
						continue;
					}
					const bool bExact = Search == Record.StableName
						|| Search == Record.ClassPath
						|| Search == Record.PublicRef
						|| Search == Candidate->GetSummary()
							.GetIdentity()
							.GetCanonicalKey();
					const bool bBroad = Search.IsEmpty()
						|| Record.StableName.Contains(
							Search,
							ESearchCase::IgnoreCase)
						|| Record.ClassPath.Contains(
							Search,
							ESearchCase::IgnoreCase)
						|| Record.Profile.ProfileId.Contains(
							Search,
							ESearchCase::IgnoreCase);
					if ((Query.GetMatchMode()
							== EUeAgentDiscoveryMatchMode::Exact
						&& bExact)
						|| (Query.GetMatchMode()
							== EUeAgentDiscoveryMatchMode::Broad
						&& bBroad))
					{
						FUeAgentResourceChargeError ChargeError;
						if (!Resources.TryCharge(
								EUeAgentResourceDimension::Items,
								1,
								TEXT("asset_data_factory.search_resolved"),
								ChargeError))
						{
							OutError = ChargeError;
							OutBindings.Reset();
							return false;
						}
						Candidates.Add(Candidate->GetSummary());
						OutBindings.Add(
							MakeShared<FUeAgentCandidateBinding>(*Candidate));
					}
				}
				const bool bCreated = FUeAgentCatalogSnapshot::TryCreate(
					TEXT("uai-asset-data-factory-catalog:")
						+ Revision.GetGeneration(),
					FactoryCatalogKind,
					Revision,
					EUeAgentCatalogCompleteness::Complete,
					Candidates,
					{},
					Resources,
					OutCatalog,
					OutError);
				if (!bCreated)
				{
					OutBindings.Reset();
				}
				return bCreated;
			}

			bool TryResolveExact(
				const FString& CandidateId,
				const FUeAgentCatalogRevision& ExpectedRevision,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
				FUeAgentDiscoveryError& OutError) override
			{
				OutBinding.Reset();
				OutError = {};
				if (!Checkpoint(
						Deadline,
						Cancellation,
						TEXT("asset_data_factory.resolve"),
						OutError))
				{
					return false;
				}
				TArray<FFactoryCandidateRecord> Records;
				FUeAgentCatalogRevision Revision(
					TEXT("invalid"),
					TEXT("invalid"),
					-1);
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildFactoryRecords(Records, Revision, BuildError))
				{
					OutError = BuildError;
					return false;
				}
				if (ExpectedRevision != Revision)
				{
					OutError = FDiagnostic::Failure(TEXT("catalog_revision_stale"),
						TEXT("The requested factory candidate revision differs from the current catalog revision."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = CandidateId;
					OutError.PropertyPath = TEXT("catalog_revision");
					OutError.ActualEvidence = FactoryRevisionEvidence(Revision);
					OutError.ExpectedEvidence = FactoryRevisionEvidence(ExpectedRevision);
					return false;
				}
				for (const FFactoryCandidateRecord& Record : Records)
				{
					const FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
						BuildFactoryBinding(Record, Revision);
					const FUeAgentCandidateBinding* Candidate =
						Binding.GetCandidateBinding();
					if (Candidate
						&& Candidate->GetSummary()
							.GetIdentity()
							.GetCanonicalKey()
							== CandidateId)
					{
						FUeAgentResourceChargeError ChargeError;
						if (!Resources.TryCharge(
								EUeAgentResourceDimension::Items,
								1,
								TEXT("asset_data_factory.resolve"),
								ChargeError))
						{
							OutError = ChargeError;
							return false;
						}
						OutBinding =
							MakeShared<FUeAgentCandidateBinding>(*Candidate);
						return true;
					}
				}
				OutError = FDiagnostic::Failure(TEXT("candidate_not_found"),
					TEXT("The requested asset factory candidate is absent from the current catalog."), TEXT("simple_authoring"));
				OutError.SourceKind = TEXT("native_result");
				OutError.SubjectId = CandidateId;
				OutError.PropertyPath = TEXT("candidate_id");
				OutError.ActualValuePresent = false;
				OutError.ExpectedValuePresent = true;
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CandidateId);
				return false;
			}

			bool TryResolveForPlanning(
				const FString& FactoryName,
				const TMap<FString, FString>& Select,
				const FString& ProfileId,
				FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
				FExecutionMutationError& OutError) override
			{
				OutBinding = FUeAgentAssetDataSimpleEffectFactoryBinding();
				OutError = FExecutionMutationError();
				TArray<FFactoryCandidateRecord> Records;
				FUeAgentCatalogRevision Revision(TEXT("invalid"), TEXT("invalid"), -1);
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildFactoryRecords(Records, Revision, BuildError))
				{
					OutError = MoveTemp(BuildError);
					return false;
				}
				TArray<const FFactoryCandidateRecord*> ProfileRecords;
				TArray<ContentDiscovery::FNameSelectionEntry> Entries;
				for (const FFactoryCandidateRecord& Record : Records)
				{
					if (Record.Profile.ProfileId != ProfileId)
					{
						continue;
					}
					// Resolve the same create-capable set exposed by factory discovery.
					// Read-only interactive factories cannot be usable alternatives here.
					const auto CandidateBinding = BuildFactoryBinding(Record, Revision);
					const FUeAgentCandidateBinding* Candidate = CandidateBinding.GetCandidateBinding();
					if (!Candidate || !Candidate->Supports(EUeAgentCandidateOperation::Create))
					{
						continue;
					}
					ProfileRecords.Add(&Record);
					ContentDiscovery::FNameSelectionEntry Entry;
					Entry.Name = Record.StableName;
					Entry.Identity = Record.BindingId;
					Entry.Select.Add(TEXT("creation"), Record.Factory.IsValid() ? TEXT("factory") : TEXT("constructor"));
					Entry.Select.Add(TEXT("class"), StableClassName(Record.ConcreteClass.Get()));
					Entry.Select.Add(TEXT("source"), StableClassModuleName(Record.ConcreteClass.Get()));
					Entry.Select.Add(TEXT("class_source"), ContentDiscovery::MakeReadableDiscoveryPath(Record.ConcreteClass->GetPathName()));
					if (Record.Factory.IsValid())
					{
						Entry.Select.Add(TEXT("factory"), StableClassName(Record.Factory->GetClass()));
						Entry.Select.Add(TEXT("factory source"), StableClassModuleName(Record.Factory->GetClass()));
						Entry.Select.Add(TEXT("factory_class_source"), ContentDiscovery::MakeReadableDiscoveryPath(Record.Factory->GetClass()->GetPathName()));
					}
					Entries.Add(MoveTemp(Entry));
				}
				int32 ResolvedIndex = INDEX_NONE;
				UE::UAI::Contract::FDiagnostic NameError;
				if (!ContentDiscovery::TryResolveNameSelection(
						Entries, FactoryName, Select, ResolvedIndex, NameError))
				{
					OutError = MoveTemp(NameError);
					return false;
				}
				const FFactoryCandidateRecord* Match = ProfileRecords[ResolvedIndex];
				OutBinding = BuildFactoryBinding(*Match, Revision);
				UE::UAI::Contract::FDiagnostic ValidationError;
				if (!TryValidateAssetDataSimpleEffectFactoryBinding(
					Match->Profile.NativeBindingId, Match->BindingId, Revision,
					OutBinding, ValidationError))
				{
					OutError = MoveTemp(ValidationError);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = FactoryName;
					return false;
				}
				return true;
			}

			bool TryResolveFrozenBinding(
				const FString& FactoryBindingId,
				const FUeAgentCatalogRevision& ExpectedRevision,
				const FString& ProfileId,
				FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
				FExecutionMutationError& OutError) override
			{
				return TryResolveDomainBinding(
					FactoryBindingId,
					ExpectedRevision,
					ProfileId,
					true,
					OutBinding,
					OutError);
			}

		private:
			bool TryResolveDomainBinding(
				const FString& Identity,
				const FUeAgentCatalogRevision& ExpectedRevision,
				const FString& ProfileId,
				const bool bBindingId,
				FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
				FExecutionMutationError& OutError)
			{
				OutBinding = FUeAgentAssetDataSimpleEffectFactoryBinding();
				OutError = FExecutionMutationError();
				TArray<FFactoryCandidateRecord> Records;
				FUeAgentCatalogRevision Revision(
					TEXT("invalid"),
					TEXT("invalid"),
					-1);
				UE::UAI::Contract::FDiagnostic BuildError;
				if (!TryBuildFactoryRecords(Records, Revision, BuildError))
				{
					OutError = MoveTemp(BuildError);
					return false;
				}
				if (ExpectedRevision != Revision)
				{
					OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_candidate_stale"),
						TEXT("The frozen factory binding revision differs from the current catalog revision."), TEXT("execution"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Identity;
					if (!bBindingId) OutError.CandidateName = Identity;
					OutError.PropertyPath = TEXT("catalog_revision");
					OutError.ActualEvidence = FactoryRevisionEvidence(Revision);
					OutError.ExpectedEvidence = FactoryRevisionEvidence(ExpectedRevision);
					return false;
				}
				const FFactoryCandidateRecord* Match = nullptr;
				int32 MatchCount = 0;
				bool bFoundInOtherContext = false;
				TArray<FString> OtherProfiles;
				for (const FFactoryCandidateRecord& Record : Records)
				{
					const bool bIdentityMatch = bBindingId
						? Record.BindingId == Identity
						: (Record.PublicRef == Identity
							|| (ProfileId
									== TEXT("generic_default_create")
								&& Record.StableName == Identity));
					if (!bIdentityMatch)
					{
						continue;
					}
					if (Record.Profile.ProfileId != ProfileId)
					{
						bFoundInOtherContext = true;
						OtherProfiles.AddUnique(Record.Profile.ProfileId);
						continue;
					}
					Match = &Record;
					++MatchCount;
				}
				if (MatchCount > 1)
				{
					OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_candidate_ambiguous"),
						FString::Printf(TEXT("More than one asset factory candidate matches the requested identity in profile '%s'."), *ProfileId), TEXT("execution"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Identity;
					if (!bBindingId) OutError.CandidateName = Identity;
					OutError.PropertyPath = TEXT("matching_candidates");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(MatchCount);
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					return false;
				}
				if (MatchCount == 1 && Match)
				{
					FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
						BuildFactoryBinding(*Match, Revision);
					UE::UAI::Contract::FDiagnostic ValidationError;
					if (!TryValidateAssetDataSimpleEffectFactoryBinding(
							Match->Profile.NativeBindingId,
							Match->BindingId,
							Revision,
							Binding,
							ValidationError))
					{
						OutError = MoveTemp(ValidationError);
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Identity;
						return false;
					}
					OutBinding = MoveTemp(Binding);
					return true;
				}
				if (bFoundInOtherContext)
				{
					OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_candidate_wrong_context"),
						TEXT("The requested asset factory exists only under other production profiles."), TEXT("execution"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Identity;
					if (!bBindingId) OutError.CandidateName = Identity;
					OutError.PropertyPath = TEXT("profile_id");
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& Profile : OtherProfiles) Values.Add(MakeShared<FJsonValueString>(Profile));
					OutError.ActualValue = MakeShared<FJsonValueArray>(Values);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(ProfileId);
					return false;
				}
				OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_candidate_not_found"),
					FString::Printf(TEXT("No asset factory candidate matches the requested identity in profile '%s'."), *ProfileId), TEXT("execution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.SubjectId = Identity;
				if (!bBindingId) OutError.CandidateName = Identity;
				OutError.PropertyPath = TEXT("matching_candidates");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				return false;
			}

			TArray<FString> Kinds;
		};
	}

	bool TryCreateWorldSimpleAuthoringReferenceResolver(
		TSharedPtr<
			IUeAgentWorldSimpleEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutResolver = MakeShared<FWorldReferenceResolver, ESPMode::ThreadSafe>();
		OutError = {};
		return OutResolver.IsValid();
	}

	bool TryCreateLandscapeSimpleAuthoringReferenceResolver(
		TSharedPtr<
			IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutResolver = MakeShared<
			FLandscapeReferenceResolver,
			ESPMode::ThreadSafe>();
		OutError = {};
		return OutResolver.IsValid();
	}

	bool TryCreateModelingSimpleAuthoringReferenceResolver(
		TSharedPtr<
			IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutResolver = MakeShared<
			FModelingReferenceResolver,
			ESPMode::ThreadSafe>();
		OutError = {};
		return OutResolver.IsValid();
	}

	bool TryCreateAssetDataSimpleAuthoringAssetResolver(
		TSharedPtr<
			IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutResolver = MakeShared<FAssetResolver, ESPMode::ThreadSafe>();
		OutError = {};
		return OutResolver.IsValid();
	}

	bool TryResolveGenericAssetFactory(const FString& Name, const TMap<FString, FString>& Select,
		FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding, FExecutionMutationError& OutError)
	{
		TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe> Catalog;
		TSharedPtr<IUeAgentAssetDataSimpleEffectFactoryCandidateResolver, ESPMode::ThreadSafe> Resolver;
		FUeAgentSimpleAuthoringRuntimeResolverError Error;
		if (!TryCreateAssetDataSimpleAuthoringFactoryCatalog(Catalog, Resolver, Error))
		{ OutError = Error; return false; }
		if (!Resolver->TryResolveForPlanning(Name, Select, TEXT("generic_default_create"), OutBinding, OutError)) return false;
		if (!OutBinding.IsGenericReady())
		{
			OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_interactive_configuration_required"),
				TEXT("The selected factory requires interactive configuration before it can create an asset."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = Name;
			OutError.PropertyPath = TEXT("generic_ready");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		return true;
	}
	bool TryValidateGenericAssetFactoryBinding(const FUeAgentAssetDataSimpleEffectFactoryBinding& Binding, FExecutionMutationError& OutError)
	{
		if (!Binding.IsValid() || !Binding.GetCandidateBinding())
		{
			const FUeAgentCandidateBinding* Candidate = Binding.GetCandidateBinding();
			if (Candidate && !Candidate->IsValid())
			{
				OutError = *Candidate->GetValidationDiagnostic();
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			const FString Field = !Candidate ? TEXT("candidate")
				: Binding.GetProfileId().IsEmpty() ? TEXT("profile_id")
				: Binding.GetFactoryMechanicId().IsEmpty() ? TEXT("factory_mechanic_id")
				: TEXT("concrete_class");
			OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_binding_invalid"),
				FString::Printf(TEXT("The asset factory binding has no valid '%s'."), *Field), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			OutError.ActualValuePresent = Field == TEXT("profile_id") || Field == TEXT("factory_mechanic_id");
			if (OutError.ActualValuePresent.GetValue())
				OutError.ActualValue = MakeShared<FJsonValueString>(FString());
			if (Candidate) OutError.SubjectId = Candidate->GetBindingId();
			OutError.ExpectedValuePresent = true;
			return false;
		}
		TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe> Catalog;
		TSharedPtr<IUeAgentAssetDataSimpleEffectFactoryCandidateResolver, ESPMode::ThreadSafe> Resolver;
		FUeAgentSimpleAuthoringRuntimeResolverError Error;
		if (!TryCreateAssetDataSimpleAuthoringFactoryCatalog(Catalog, Resolver, Error))
		{ OutError = Error; return false; }
		FUeAgentAssetDataSimpleEffectFactoryBinding Current;
		if (!Resolver->TryResolveFrozenBinding(Binding.GetCandidateBinding()->GetBindingId(),
			Binding.GetCandidateBinding()->GetCatalogRevision(), TEXT("generic_default_create"), Current, OutError))
			return false;
		if (!Current.IsGenericReady())
		{
			OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_interactive_configuration_required"),
				TEXT("The current factory binding requires interactive configuration."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("generic_ready");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		if (Current.GetConcreteClass() != Binding.GetConcreteClass())
		{
			OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_binding_mismatch"),
				TEXT("The current factory binding resolves to a different concrete class."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("concrete_class");
			OutError.ActualValue = MakeShared<FJsonValueString>(Current.GetConcreteClass() ? Current.GetConcreteClass()->GetPathName() : FString());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Binding.GetConcreteClass() ? Binding.GetConcreteClass()->GetPathName() : FString());
			if (Current.GetConcreteClass()) OutError.ActualEvidence = MakeShared<FJsonValueString>(::LexToString(Current.GetConcreteClass()->GetUniqueID()));
			if (Binding.GetConcreteClass()) OutError.ExpectedEvidence = MakeShared<FJsonValueString>(::LexToString(Binding.GetConcreteClass()->GetUniqueID()));
			return false;
		}
		if (Current.GetFactory() != Binding.GetFactory())
		{
			OutError = FExecutionMutationError::Failure(TEXT("asset_data_factory_binding_mismatch"),
				TEXT("The current factory binding resolves to a different factory instance."), TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("factory");
			OutError.ActualValuePresent = Current.GetFactory() != nullptr;
			OutError.ExpectedValuePresent = Binding.GetFactory() != nullptr;
			if (Current.GetFactory())
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(Current.GetFactory()->GetPathName());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(::LexToString(Current.GetFactory()->GetUniqueID()));
			}
			if (Binding.GetFactory())
			{
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Binding.GetFactory()->GetPathName());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(::LexToString(Binding.GetFactory()->GetUniqueID()));
			}
			return false;
		}
		return true;
	}

	bool TryCreateAssetDataSimpleAuthoringFactoryCatalog(
		TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe>&
			OutCatalogSource,
		TSharedPtr<
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError)
	{
		OutCatalogSource.Reset();
		OutResolver.Reset();
		OutError = {};
		TArray<FFactoryCandidateRecord> Records;
		FUeAgentCatalogRevision Revision(
			TEXT("invalid"),
			TEXT("invalid"),
			-1);
		UE::UAI::Contract::FDiagnostic BuildError;
		if (!TryBuildFactoryRecords(Records, Revision, BuildError))
		{
			OutError = BuildError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = FactoryCatalogSourceId;
			return false;
		}
		const TSharedRef<FFactoryCatalog, ESPMode::ThreadSafe> Owner =
			MakeShared<FFactoryCatalog, ESPMode::ThreadSafe>();
		OutCatalogSource = StaticCastSharedRef<IUeAgentCatalogSource>(Owner);
		OutResolver = StaticCastSharedRef<
			IUeAgentAssetDataSimpleEffectFactoryCandidateResolver>(Owner);
		return true;
	}
}
