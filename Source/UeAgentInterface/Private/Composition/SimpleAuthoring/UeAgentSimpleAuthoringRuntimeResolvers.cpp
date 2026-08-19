#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeIdentity.h"

#include "Engine/World.h"
#include "Misc/PackageName.h"
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

		void SetError(
			FUeAgentSimpleAuthoringRuntimeResolverError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
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
				OutError.Code = Cancelled.Code;
				OutError.SubjectId = Cancelled.Reason;
				OutError.Detail = Stage;
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				OutError.Code = Expired.Code;
				OutError.SubjectId = Stage;
				OutError.Detail = TEXT("factory catalog deadline expired");
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
				OutError = FExecutionMutationError(
					TEXT("simple_authoring_reference_invalid"),
					CanonicalRef);
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
				OutError = FExecutionMutationError(
					TEXT("simple_authoring_reference_not_found"),
					Ref);
				return false;
			}
			UWorld* OwningWorld = Object->GetTypedOuter<UWorld>();
			if (OwningWorld && OwningWorld != &SelectedWorld)
			{
				OutError = FExecutionMutationError(
					TEXT("simple_authoring_reference_world_mismatch"),
					Ref);
				return false;
			}
			OutObject = Object;
			return true;
		}

		bool TryNormalizeAssetObjectPath(
			const FString& Input,
			FString& OutPackageName,
			FString& OutObjectPath,
			FString& OutErrorCode,
			const bool bAllowEngineRoot = false)
		{
			OutPackageName.Reset();
			OutObjectPath.Reset();
			OutErrorCode.Reset();
			if (Input.IsEmpty() || Input != Input.TrimStartAndEnd())
			{
				OutErrorCode = TEXT("asset_data_asset_ref_invalid");
				return false;
			}
			const int32 LastSlash = Input.Find(
				TEXT("/"),
				ESearchCase::CaseSensitive,
				ESearchDir::FromEnd);
			const int32 DotIndex = Input.Find(
				TEXT("."),
				ESearchCase::CaseSensitive,
				ESearchDir::FromEnd);
			if (LastSlash == INDEX_NONE
				|| DotIndex <= LastSlash
				|| DotIndex == Input.Len() - 1)
			{
				OutErrorCode = TEXT("asset_data_asset_ref_invalid");
				return false;
			}
			OutPackageName = Input.Left(DotIndex);
			const FString ObjectName = Input.Mid(DotIndex + 1);
			const bool bAllowedRoot = OutPackageName.StartsWith(
				TEXT("/Game/"), ESearchCase::CaseSensitive)
				|| (bAllowEngineRoot && OutPackageName.StartsWith(
					TEXT("/Engine/"), ESearchCase::CaseSensitive));
			if (!bAllowedRoot
				|| !FPackageName::IsValidLongPackageName(OutPackageName)
				|| FPackageName::GetLongPackageAssetName(OutPackageName)
					!= ObjectName)
			{
				OutPackageName.Reset();
				OutErrorCode = TEXT("asset_data_asset_ref_invalid");
				return false;
			}
			OutObjectPath = OutPackageName + TEXT(".") + ObjectName;
			return true;
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
				FString ErrorCode;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						ErrorCode)
					|| CanonicalRef != ObjectPath)
				{
					OutError = FExecutionMutationError(
						ErrorCode.IsEmpty()
							? TEXT("asset_data_asset_ref_invalid")
							: ErrorCode,
						CanonicalRef);
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
				if (!IsValid(Object)
					|| Object->GetPathName() != ObjectPath)
				{
					OutError = FExecutionMutationError(
						TEXT("asset_data_existing_not_found"),
						CanonicalRef);
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
				FString ErrorCode;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						ErrorCode,
						true)
					|| CanonicalRef != ObjectPath)
				{
					OutError = FExecutionMutationError(
						ErrorCode.IsEmpty()
							? TEXT("asset_data_clone_source_ref_invalid")
							: ErrorCode,
						CanonicalRef);
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
					OutError = FExecutionMutationError(
						TEXT("asset_data_clone_source_not_found"),
						CanonicalRef);
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
				FString ErrorCode;
				if (!TryNormalizeAssetObjectPath(
						CanonicalRef,
						PackageName,
						ObjectPath,
						ErrorCode)
					|| CanonicalRef != ObjectPath)
				{
					OutError = FExecutionMutationError(
						ErrorCode.IsEmpty()
							? TEXT("asset_data_asset_ref_invalid")
							: ErrorCode,
						CanonicalRef);
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
					OutError = FExecutionMutationError(
						TEXT("asset_data_creation_target_exists"),
						CanonicalRef);
					return false;
				}
				UPackage* Package = CreatePackage(*PackageName);
				const FString AssetName =
					FPackageName::GetLongPackageAssetName(PackageName);
				if (!Package || AssetName.IsEmpty())
				{
					OutError = FExecutionMutationError(
						TEXT("asset_data_creation_package_unavailable"),
						CanonicalRef);
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
			FString ClassPath;
			FString PublicRef;
			FString BindingId;

			bool IsValid() const
			{
				return Profile.UsesFactoryCandidate()
					&& ConcreteClass.IsValid()
					&& !ClassPath.IsEmpty()
					&& !PublicRef.IsEmpty()
					&& !BindingId.IsEmpty();
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
			Record.ConcreteClass = Class;
			Record.ClassPath = ClassPath;
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

		bool TryBuildFactoryRecords(
			TArray<FFactoryCandidateRecord>& OutRecords,
			FUeAgentCatalogRevision& OutRevision,
			FString& OutError)
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
			for (const FUeAgentAssetDataSimpleEffectProductionProfile& Profile
				: Profiles)
			{
				if (!Profile.UsesFactoryCandidate())
				{
					continue;
				}
				if (Profile.ProfileId == TEXT("audio_effect_preset"))
				{
					AddCandidateClass(
						Profile,
						LoadClassExact(
							TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset")),
						OutRecords);
				}
				else if (Profile.ProfileId == TEXT("sound_submix"))
				{
					AddCandidateClass(
						Profile,
						LoadClassExact(TEXT("/Script/Engine.SoundSubmix")),
						OutRecords);
				}
				else if (Profile.ProfileId == TEXT("media_source"))
				{
					AddCandidateClass(
						Profile,
						LoadClassExact(TEXT("/Script/MediaAssets.FileMediaSource")),
						OutRecords);
				}
				else if (Profile.ProfileId == TEXT("data_object"))
				{
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
					return ProfileOrder == 0
						? Left.ClassPath < Right.ClassPath
						: ProfileOrder < 0;
				});
			FString RevisionInput;
			for (const FFactoryCandidateRecord& Record : OutRecords)
			{
				RevisionInput += Record.Profile.FactoryContractId
					+ TEXT("\n")
					+ Record.Profile.ClassConstraintId
					+ TEXT("\n")
					+ Record.ClassPath
					+ TEXT("\n");
			}
			OutRevision = FUeAgentCatalogRevision(
				FactoryCatalogSourceId,
				RuntimePrivate::Sha256(RevisionInput),
				1);
			OutRecords.RemoveAll(
				[&OutRevision](const FFactoryCandidateRecord& Record)
				{
					const FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
						BuildFactoryBinding(Record, OutRevision);
					FString ValidationError;
					return !TryValidateAssetDataSimpleEffectFactoryBinding(
						Record.Profile.NativeBindingId,
						Record.BindingId,
						OutRevision,
						Binding,
						ValidationError);
				});
			RevisionInput.Reset();
			for (const FFactoryCandidateRecord& Record : OutRecords)
			{
				RevisionInput += Record.Profile.FactoryContractId
					+ TEXT("\n")
					+ Record.Profile.ClassConstraintId
					+ TEXT("\n")
					+ Record.ClassPath
					+ TEXT("\n");
			}
			OutRevision = FUeAgentCatalogRevision(
				FactoryCatalogSourceId,
				RuntimePrivate::Sha256(RevisionInput),
				1);
			return OutRevision.IsValid() && !OutRecords.IsEmpty();
		}

		FUeAgentAssetDataSimpleEffectFactoryBinding BuildFactoryBinding(
			const FFactoryCandidateRecord& Record,
			const FUeAgentCatalogRevision& Revision)
		{
			const FString CandidateHash = RuntimePrivate::Sha256(
				Record.Profile.FactoryContractId
				+ TEXT("\n")
				+ Record.Profile.ClassConstraintId
				+ TEXT("\n")
				+ Record.ClassPath);
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
						Record.ConcreteClass->GetDisplayNameText().ToString(),
						Record.ClassPath,
						{TEXT("Asset"), TEXT("Factory"), Record.Profile.ProfileId}),
					Revision,
					Record.PublicRef,
					FactoryReadPortId,
					{EUeAgentCandidateOperation::Create},
					Record.Profile.ClassConstraintId),
				Record.Profile.ProfileId,
				Record.ConcreteClass.Get(),
				FactoryMechanicId);
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
				OutCatalog.Reset();
				OutError = {};
				if (!Query.IsValid()
					|| Query.GetCatalogKind() != FactoryCatalogKind)
				{
					OutError.Code = TEXT("factory_catalog_query_invalid");
					OutError.SubjectId = Query.GetCatalogKind();
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
				FString BuildError;
				if (!TryBuildFactoryRecords(
						Records,
						Revision,
						BuildError))
				{
					OutError.Code = TEXT("factory_catalog_unavailable");
					OutError.Detail = BuildError;
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
					const bool bExact = Search == Record.ClassPath
						|| Search == Record.PublicRef
						|| Search == Candidate->GetSummary()
							.GetIdentity()
							.GetCanonicalKey();
					const bool bBroad = Search.IsEmpty()
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
						Candidates.Add(Candidate->GetSummary());
					}
				}
				return FUeAgentCatalogSnapshot::TryCreate(
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
				FString BuildError;
				if (!TryBuildFactoryRecords(Records, Revision, BuildError))
				{
					OutError.Code = TEXT("factory_catalog_unavailable");
					OutError.Detail = BuildError;
					return false;
				}
				if (ExpectedRevision != Revision)
				{
					OutError.Code = TEXT("catalog_revision_stale");
					OutError.SubjectId = CandidateId;
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
						FUeAgentResourceBudgetExceeded BudgetError;
						if (!Resources.TryCharge(
								EUeAgentResourceDimension::Items,
								1,
								TEXT("asset_data_factory.resolve"),
								BudgetError))
						{
							OutError.Code = BudgetError.Code;
							OutError.SubjectId = BudgetError.Path;
							return false;
						}
						OutBinding =
							MakeShared<FUeAgentCandidateBinding>(*Candidate);
						return true;
					}
				}
				OutError.Code = TEXT("candidate_not_found");
				OutError.SubjectId = CandidateId;
				return false;
			}

			bool TryResolveForPlanning(
				const FString& FactoryRef,
				const FUeAgentCatalogRevision& ExpectedRevision,
				const FString& ProfileId,
				FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
				FExecutionMutationError& OutError) override
			{
				return TryResolveDomainBinding(
					FactoryRef,
					ExpectedRevision,
					ProfileId,
					false,
					OutBinding,
					OutError);
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
				FString BuildError;
				if (!TryBuildFactoryRecords(Records, Revision, BuildError))
				{
					OutError = FExecutionMutationError(
						TEXT("asset_data_factory_catalog_unavailable"),
						BuildError);
					return false;
				}
				if (ExpectedRevision != Revision)
				{
					OutError = FExecutionMutationError(
						TEXT("asset_data_factory_candidate_stale"),
						Identity);
					return false;
				}
				for (const FFactoryCandidateRecord& Record : Records)
				{
					if ((bBindingId
							? Record.BindingId == Identity
							: Record.PublicRef == Identity)
						&& Record.Profile.ProfileId == ProfileId)
					{
						FUeAgentAssetDataSimpleEffectFactoryBinding Binding =
							BuildFactoryBinding(Record, Revision);
						FString ValidationError;
						if (!TryValidateAssetDataSimpleEffectFactoryBinding(
								Record.Profile.NativeBindingId,
								Record.BindingId,
								Revision,
								Binding,
								ValidationError))
						{
							OutError = FExecutionMutationError(
								ValidationError,
								Identity);
							return false;
						}
						OutBinding = MoveTemp(Binding);
						return true;
					}
				}
				OutError = FExecutionMutationError(
					TEXT("asset_data_factory_candidate_not_found"),
					Identity);
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
		FString BuildError;
		if (!TryBuildFactoryRecords(Records, Revision, BuildError))
		{
			SetError(
				OutError,
				TEXT("asset_data_factory_catalog_unavailable"),
				FactoryCatalogSourceId,
				BuildError);
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
