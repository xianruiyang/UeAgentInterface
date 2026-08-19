#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/Families/AssetData/UeAgentAssetDataSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectFactoryCandidateTestSupport.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestSupport.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestTypes.h"
#include "Animation/Skeleton.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/VolumeTexture.h"
#include "EditorValidatorSubsystem.h"
#include "FileMediaSource.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescriptionBuilder.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "Particles/SubUVAnimation.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "StaticMeshAttributes.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"
#include "UeAgentSimpleAuthoringPackageE4TestSupport.h"

namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace
		UE::UAI::Composition::SimpleAuthoring::AssetData;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace
		UE::UAI::DomainAdapters::AssetDataSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::Verification;
	using UE::UAI::Tests::AssetDataSimpleEffect::
		FFixtureFactoryCandidateResolver;
	using UE::UAI::Tests::AssetDataSimpleEffect::LoadFixture;

	class FOwnedAssetDataCreatePackage final
	{
	public:
		FOwnedAssetDataCreatePackage()
		{
			Package = CreatePackage(
				*(TEXT(
					"/Temp/UAI/SA008/AssetDataCreate/")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits)));
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
		}

		~FOwnedAssetDataCreatePackage()
		{
			if (!Package)
			{
				return;
			}
			for (const TPair<FString, FName>& Target
				: Targets)
			{
				if (UObject* Object = StaticFindObjectFast(
						UObject::StaticClass(),
						Package,
						Target.Value))
				{
					Object->ClearFlags(RF_Standalone);
					Object->MarkAsGarbage();
				}
			}
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}

		FString AddTarget(const int32 Index)
		{
			const FString TargetRef = FString::Printf(
				TEXT("uai://asset/sa008-static-create-%02d"),
				Index);
			Targets.Add(
				TargetRef,
				FName(*FString::Printf(
					TEXT("SA008_StaticCreate_%02d"),
					Index)));
			return TargetRef;
		}

		void BindRef(
			const FString& Ref,
			const FName ObjectName)
		{
			Targets.Add(Ref, ObjectName);
		}

		FName FindTargetName(const FString& TargetRef) const
		{
			const FName* Name = Targets.Find(TargetRef);
			return Name ? *Name : NAME_None;
		}

		UObject* FindTarget(const FString& TargetRef) const
		{
			const FName Name = FindTargetName(TargetRef);
			return Package && !Name.IsNone()
				? StaticFindObjectFast(
					UObject::StaticClass(),
					Package,
					Name)
				: nullptr;
		}

		bool IsValid() const
		{
			return Package != nullptr;
		}

		UPackage* Package = nullptr;
		TMap<FString, FName> Targets;
	};

	class FOwnedAssetResolver final
		: public IUeAgentAssetDataSimpleEffectAssetResolver
	{
	public:
		explicit FOwnedAssetResolver(
			FOwnedAssetDataCreatePackage& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryResolveExisting(
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			OutObject = Owner.FindTarget(CanonicalRef);
			if (!IsValid(OutObject))
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		bool TryResolveCreationTarget(
			const FString& CanonicalRef,
			UPackage*& OutPackage,
			FName& OutObjectName,
			FExecutionMutationError& OutError) override
		{
			OutPackage = Owner.Package;
			OutObjectName =
				Owner.FindTargetName(CanonicalRef);
			if (!OutPackage || OutObjectName.IsNone())
			{
				OutError = FExecutionMutationError(
					TEXT(
						"asset_data_creation_target_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

	private:
		FOwnedAssetDataCreatePackage& Owner;
	};

	const TSharedPtr<FJsonObject> FindFactoryContract(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& ProfileId)
	{
		if (!Fixture.IsValid())
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("factory_contracts")))
		{
			const TSharedPtr<FJsonObject> Contract =
				Value->AsObject();
			if (Contract.IsValid()
				&& Contract->GetStringField(TEXT("profile_id"))
					== ProfileId)
			{
				return Contract;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> BuildCreatePayload(
		const FString& TargetRef,
		const FUeAgentAssetDataSimpleEffectProductionProfile&
			Profile,
		const TSharedPtr<FJsonObject>& Fixture,
		FString& OutExpectedClass)
	{
		OutExpectedClass = Profile.StaticCreateClassPath;
		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		Payload->SetStringField(
			TEXT("target_ref"),
			TargetRef);
		if (!Profile.UsesFactoryCandidate())
		{
			return Payload;
		}
		const TSharedPtr<FJsonObject> Contract =
			FindFactoryContract(Fixture, Profile.ProfileId);
		if (!Contract.IsValid())
		{
			return nullptr;
		}
		OutExpectedClass =
			Contract->GetStringField(TEXT("asset_class_path"));
		const TSharedPtr<FJsonObject> Revision =
			MakeShared<FJsonObject>();
		Revision->SetStringField(
			TEXT("source_id"),
			Contract->GetStringField(
				TEXT("catalog_source_id")));
		Revision->SetStringField(
			TEXT("generation"),
			Contract->GetStringField(
				TEXT("catalog_generation")));
		Revision->SetNumberField(
			TEXT("sequence"),
			Contract->GetNumberField(
				TEXT("catalog_sequence")));
		const TSharedPtr<FJsonObject> FactoryRef =
			MakeShared<FJsonObject>();
		FactoryRef->SetStringField(
			TEXT("candidate_ref"),
			Contract->GetStringField(TEXT("candidate_ref")));
		FactoryRef->SetObjectField(
			TEXT("catalog_revision"),
			Revision);
		Payload->SetObjectField(
			TEXT("factory_ref"),
			FactoryRef);
		return Payload;
	}

	FString BuildAssetRevision(UObject* Object)
	{
		return Object
			? FString::Printf(
				TEXT("asset-crc32:%08x"),
				FArchiveObjectCrc32().Crc32(Object))
			: FString();
	}

	TSharedPtr<FJsonObject> BuildClonePayload(
		const FString& TargetRef,
		const FString& SourceRef,
		const FString& SourceRevision)
	{
		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("target_ref"), TargetRef);
		Payload->SetStringField(TEXT("source_ref"), SourceRef);
		Payload->SetStringField(
			TEXT("expected_source_revision"),
			SourceRevision);
		return Payload;
	}

	TSharedPtr<FJsonObject> BuildTypedChange(
		const FString& FieldId,
		const FString& Presence,
		const FString& TypeId,
		const TSharedPtr<FJsonValue>& CanonicalValue)
	{
		const TSharedPtr<FJsonObject> Change =
			MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("field_id"), FieldId);
		Change->SetStringField(TEXT("presence"), Presence);
		if (CanonicalValue.IsValid())
		{
			const TSharedPtr<FJsonObject> TypedValue =
				MakeShared<FJsonObject>();
			TypedValue->SetStringField(TEXT("type_id"), TypeId);
			TypedValue->SetField(
				TEXT("canonical_value"),
				CanonicalValue);
			Change->SetObjectField(TEXT("value"), TypedValue);
		}
		return Change;
	}

	TSharedPtr<FJsonObject> BuildUpdatePayload(
		const FString& TargetRef,
		const FString& TargetRevision,
		const TArray<TSharedPtr<FJsonObject>>& Changes)
	{
		const TSharedPtr<FJsonObject> Payload =
			MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("target_ref"), TargetRef);
		Payload->SetStringField(
			TEXT("expected_revision"),
			TargetRevision);
		TArray<TSharedPtr<FJsonValue>> ChangeValues;
		ChangeValues.Reserve(Changes.Num());
		for (const TSharedPtr<FJsonObject>& Change : Changes)
		{
			ChangeValues.Add(
				MakeShared<FJsonValueObject>(Change));
		}
		Payload->SetArrayField(
			TEXT("changes"),
			ChangeValues);
		return Payload;
	}

	FString ReadCloneDataFingerprint(
		const
			UUeAgentAssetDataSimpleEffectDataAsset& Data)
	{
		return FString::Printf(
			TEXT("%s|%d|%.9g|%d|%s"),
			*Data.Label,
			Data.Count,
			static_cast<double>(Data.Weight),
			Data.bEnabled ? 1 : 0,
			Data.RelatedAsset
				? *Data.RelatedAsset->GetPathName()
			: TEXT(""));
	}

	FString ReadSparseVolumeTextureFingerprint(
		const UStreamableSparseVolumeTexture& Sparse)
	{
		return FString::Printf(
			TEXT("%d|%d|%d|%d|%.9g|%d|%.9g|%.9g"),
			static_cast<int32>(Sparse.AddressX),
			static_cast<int32>(Sparse.AddressY),
			static_cast<int32>(Sparse.AddressZ),
			Sparse.bLocalDDCOnly ? 1 : 0,
			static_cast<double>(Sparse.StreamingPoolSizeFactor),
			Sparse.NumberOfPrefetchFrames,
			static_cast<double>(
				Sparse.PrefetchPercentageStepSize),
			static_cast<double>(
				Sparse.PrefetchPercentageBias));
	}

	class FRecordingDurableService final
		: public IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		bool TryPrepare(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			++PrepareCount;
			if (!Request.IsValid()
				|| Request.GetTargets().Num() != 1
				|| Request.GetTargets()[0].GetArtifactKind()
					!= EDurableArtifactKind::Package)
			{
				OutError = FAssetDurableError(
					TEXT(
						"sa008_asset_data_durable_target_invalid"));
				return false;
			}
			SeenPackageNames.Add(
				Request.GetTargets()[0].GetPackageName());
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa008-asset-data-create:")
					+ FString::FromInt(PrepareCount),
				Request.GetTargets()[0].GetIdentity(),
				Request.GetTargets()[0].GetPackageName(),
				TEXT(
					"fixture://sa008-asset-data-create.uasset"),
				EAssetDurableIntent::Save,
				TEXT("asset_package"));
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
					Artifacts,
					Resources,
					InventoryHash,
					OutError))
			{
				return false;
			}
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				FDurableInventory(
					MoveTemp(Artifacts),
					MoveTemp(InventoryHash)),
				TEXT("journal:sa008-asset-data-create:")
					+ FString::FromInt(PrepareCount),
				TEXT(
					"runtimeLogs/SA008AssetDataCreate/"
					"journal.hjson"),
				TEXT(
					"runtimeLogs/SA008AssetDataCreate"),
				TEXT(
					"runtimeLogs/SA008AssetDataCreate/"
					"transaction"),
				TEXT(
					"runtimeLogs/SA008AssetDataCreate/"
					"staging"),
				TArray<FString>{TEXT("prepared")});
			return OutSession.IsValid();
		}

		bool TryPersist(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			++PersistCount;
			OutReceipt = FDurableReceipt(
				Session->GetJournalId(),
				EDurableStatus::PersistedUnverified,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				{TEXT("prepared"), TEXT("persisted_unverified")},
				ERecoveryAction::None,
				true,
				false);
			return CurrentFencingToken
					== Session->GetRequest().GetFencingToken()
				&& OutReceipt.RequiresIndependentVerification();
		}

		bool TryRollback(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			const bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError(
				TEXT(
					"sa008_asset_data_unexpected_durable_rollback"));
			return false;
		}

		bool TryAcknowledge(
			const FAssetDurableSessionRef& Session,
			const FDurableVerificationAcknowledgement&
				Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++AcknowledgeCount;
			return Acknowledgement.IsValid()
				&& Acknowledgement.GetJournalId()
					== Session->GetJournalId();
		}

		int32 PrepareCount = 0;
		int32 PersistCount = 0;
		int32 AcknowledgeCount = 0;
		TSet<FString> SeenPackageNames;
	};

	class FUnusedAbsenceOracle final : public IAbsenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FAbsenceObservation(
				Identity,
				false,
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FUnusedReferenceOracle final
		: public IReferenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FReferenceObservation(
				Identity,
				{},
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				128);
			return true;
		}
	};

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	bool Serialize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson)
	{
		OutJson.Reset();
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(
			Object.ToSharedRef(),
			Writer);
	}

	const FUeAgentSimpleAuthoringCommandRoute* FindRoute(
		const FString& CapabilityId)
	{
		return GetSimpleAuthoringCommandRoutes().FindByPredicate(
			[&CapabilityId](
				const FUeAgentSimpleAuthoringCommandRoute& Route)
			{
				return Route.CapabilityId == CapabilityId;
			});
	}

	bool IsComplete(const FUeAgentOperationResult& Result)
	{
		const FString Json = Result.GetValue().GetJsonValue();
		return Json.Contains(
				TEXT("\"effect_outcome\":\"complete\""))
			&& Json.Contains(TEXT("\"status\":\"verified\""));
	}

	bool TryCreateAssetDataApplication(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Fixture,
		FOwnedAssetDataCreatePackage& Owner,
		const TSharedRef<
			FRecordingDurableService,
			ESPMode::ThreadSafe>& Durable,
		const FUeAgentSimpleAuthoringCommandRoute& Route,
		const FString& Scope,
		TSharedPtr<FUeAgentSimpleAuthoringApplication>&
			OutApplication)
	{
		OutApplication.Reset();
		const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
			MutationResolver =
				MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
					Owner);
		const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
			PreStateResolver =
				MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
					Owner);
		const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
			PersistedResolver =
				MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
					Owner);
		const TSharedRef<
			FFixtureFactoryCandidateResolver,
			ESPMode::ThreadSafe>
			FactoryResolver =
				MakeShared<
					FFixtureFactoryCandidateResolver,
					ESPMode::ThreadSafe>(Fixture);

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!Test.TestTrue(
				*(Scope + TEXT(" descriptors compose")),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Registry.IsValid())
		{
			Test.AddError(
				Scope + TEXT(":registry:")
				+ RegistryError);
			return false;
		}

		FUeAgentAssetDataSimpleAuthoringProviderConfig
			ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.MutationAssetResolver =
			MutationResolver;
		ProviderConfig.PreStateAssetResolver =
			PreStateResolver;
		ProviderConfig.PersistedAssetResolver =
			PersistedResolver;
		ProviderConfig.FactoryResolver = FactoryResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:") + Scope;
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			FUeAgentPageRequest(),
			TOptional<int64>(),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>
			Provider;
		FUeAgentAssetDataSimpleAuthoringError ProviderError;
		if (!Test.TestTrue(
				*(Scope + TEXT(" provider composes")),
				FUeAgentAssetDataSimpleAuthoringProviderFactory::
					TryCreate(
						ProviderConfig,
						Provider,
						ProviderError))
			|| !Provider.IsValid())
		{
			Test.AddError(
				Scope + TEXT(":provider:")
				+ ProviderError.Code + TEXT(":")
				+ ProviderError.Detail);
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService>
			TicketService;
		FUeAgentOperationTicketError TicketError;
		if (!Test.TestTrue(
				*(Scope + TEXT(" ticket service creates")),
				FUeAgentOperationTicketService::TryCreate(
					Scope + TEXT("-integrity"),
					60.0,
					[]()
					{
						return 100.0;
					},
					[Scope]()
					{
						return TEXT("ticket:") + Scope;
					},
					TicketService,
					TicketError))
			|| !TicketService.IsValid())
		{
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!Test.TestTrue(
				*(Scope + TEXT(" response projector creates")),
				FUeAgentResponseProjector::TryCreate(
					MakeShared<FTokenEstimator>(),
					Projector,
					ProjectionError))
			|| !Projector.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig
			ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:") + Scope;
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:") + Scope;
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Editor;
		ApplicationConfig.ExecutionContext.ContextRevision =
			TEXT("project:") + Scope + TEXT(":r1");
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent =
			false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:") + Scope;
		ApplicationConfig.RegistryEpoch = 1;
		ApplicationConfig.TimeoutSeconds = 60.0;
		ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;

		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!Test.TestTrue(
				*(Scope + TEXT(" application composes")),
				FUeAgentSimpleAuthoringApplication::TryCreate(
					ApplicationConfig,
					{Route},
					{Provider.ToSharedRef()},
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					OutApplication,
					ApplicationError))
			|| !OutApplication.IsValid())
		{
			Test.AddError(
				Scope + TEXT(":application:")
				+ ApplicationError.Code + TEXT(":")
				+ ApplicationError.Detail);
			return false;
		}
		return true;
	}

	const FUeAgentAssetDataSimpleEffectProductionProfile*
		FindProductionProfile(
			const TArray<
				FUeAgentAssetDataSimpleEffectProductionProfile>&
					Profiles,
			const FString& DescriptorId)
	{
		return Profiles.FindByPredicate(
			[&DescriptorId](
				const
					FUeAgentAssetDataSimpleEffectProductionProfile&
						Candidate)
			{
				return Candidate.DescriptorId == DescriptorId;
			});
	}

	bool SubmitVerifiedAssetDataUpdate(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Fixture,
		FOwnedAssetDataCreatePackage& Owner,
		const TSharedRef<
			FRecordingDurableService,
			ESPMode::ThreadSafe>& Durable,
		const FUeAgentAssetDataSimpleEffectProductionProfile&
			Profile,
		const TSharedPtr<FJsonObject>& Payload,
		const FString& Scope,
		FUeAgentOperationResult* OutResult = nullptr)
	{
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Profile.CapabilityId);
		if (!Test.TestNotNull(
				*(Scope + TEXT(" has one exact route")),
				Route))
		{
			return false;
		}
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		if (!TryCreateAssetDataApplication(
				Test,
				Fixture,
				Owner,
				Durable,
				*Route,
				Scope,
				Application))
		{
			return false;
		}
		FString PayloadJson;
		if (!Test.TestTrue(
				*(Scope + TEXT(" payload serializes")),
				Serialize(Payload, PayloadJson)))
		{
			return false;
		}
		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:") + Scope,
					TEXT("idempotency:") + Scope,
					FUeAgentContractValue(PayloadJson)),
				FUeAgentCallerContext(
					TEXT("caller:") + Scope,
					{TEXT("uai:asset-data:write")},
					TEXT("correlation:") + Scope),
				MakeShared<FNeverCancelled>());
		if (OutResult)
		{
			*OutResult = Result;
		}
		if (!IsComplete(Result))
		{
			Test.AddError(
				Scope
				+ TEXT(
					" did not complete verified lifecycle; result=")
				+ Result.GetValue().GetJsonValue());
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonValue> BuildNumberArrayValue(
		const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Values.Num());
		for (const double Value : Values)
		{
			Items.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	TSharedPtr<FJsonValue> BuildStringArrayValue(
		const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(Value));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	TSharedPtr<FJsonValue> BuildTextureFormatSettingsValue(
		const bool bSrgb,
		const FString& Compression,
		const FString& MipGeneration)
	{
		const TSharedPtr<FJsonObject> Settings =
			MakeShared<FJsonObject>();
		Settings->SetBoolField(TEXT("srgb"), bSrgb);
		Settings->SetStringField(TEXT("compression"), Compression);
		Settings->SetStringField(
			TEXT("mip_generation"),
			MipGeneration);
		return MakeShared<FJsonValueObject>(Settings);
	}

	TSharedPtr<FJsonValue> BuildTextureCubeFaceRefsValue(
		const TArray<FString>& FaceRefs)
	{
		if (FaceRefs.Num() != 6)
		{
			return nullptr;
		}
		const TCHAR* FaceKeys[] = {
			TEXT("positive_x"),
			TEXT("negative_x"),
			TEXT("positive_y"),
			TEXT("negative_y"),
			TEXT("positive_z"),
			TEXT("negative_z"),
		};
		const TSharedPtr<FJsonObject> Faces =
			MakeShared<FJsonObject>();
		for (int32 Index = 0; Index < FaceRefs.Num(); ++Index)
		{
			Faces->SetStringField(FaceKeys[Index], FaceRefs[Index]);
		}
		return MakeShared<FJsonValueObject>(Faces);
	}

	bool InitializeTexture2DSource(
		UTexture2D& Texture,
		const int32 Size,
		const uint8 Seed)
	{
		if (Size <= 0)
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNumUninitialized(Size * Size * 4);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] = static_cast<uint8>(Seed + Index * 13);
		}
		Texture.Source.Init(
			Size,
			Size,
			1,
			1,
			TSF_BGRA8,
			Data.GetData());
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid();
	}

	bool InitializeTextureCubeSource(
		UTextureCube& Texture,
		const int32 Size,
		const uint8 Seed)
	{
		if (Size <= 0)
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNumUninitialized(Size * Size * 6 * 4);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] = static_cast<uint8>(Seed + Index * 17);
		}
		Texture.Source.Init(
			Size,
			Size,
			6,
			1,
			TSF_BGRA8,
			Data.GetData());
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid();
	}

	bool InitializeVolumeTextureSource(
		UVolumeTexture& Texture,
		const int32 Size,
		const int32 Depth,
		const uint8 Seed)
	{
		if (Size <= 0 || Depth <= 0)
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNumUninitialized(Size * Size * Depth * 4);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] = static_cast<uint8>(Seed + Index * 19);
		}
		Texture.Source.Init(
			Size,
			Size,
			Depth,
			1,
			TSF_BGRA8,
			Data.GetData());
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid();
	}

	bool TextureSourceMatchesOrderedConcatenation(
		UTexture& Target,
		const TArray<UTexture*>& Sources)
	{
		TArray64<uint8> Actual;
		if (Sources.IsEmpty()
			|| !Target.Source.IsValid()
			|| !Target.Source.GetMipData(Actual, 0))
		{
			return false;
		}
		TArray64<uint8> Expected;
		for (UTexture* Source : Sources)
		{
			TArray64<uint8> SourceData;
			if (!Source
				|| !Source->Source.IsValid()
				|| !Source->Source.GetMipData(SourceData, 0))
			{
				return false;
			}
			Expected.Append(SourceData.GetData(), SourceData.Num());
		}
		return Actual == Expected;
	}

	TSharedPtr<FJsonObject> BuildSoundMixModifier(
		const FString& SoundClassRef,
		const double Volume,
		const double Pitch)
	{
		const TSharedPtr<FJsonObject> Modifier =
			MakeShared<FJsonObject>();
		Modifier->SetStringField(
			TEXT("sound_class_ref"),
			SoundClassRef);
		Modifier->SetNumberField(TEXT("volume_adjuster"), Volume);
		Modifier->SetNumberField(TEXT("pitch_adjuster"), Pitch);
		Modifier->SetNumberField(
			TEXT("low_pass_filter_frequency"),
			16000.0);
		Modifier->SetBoolField(TEXT("apply_to_children"), true);
		Modifier->SetNumberField(
			TEXT("voice_center_channel_volume_adjuster"),
			0.9);
		return Modifier;
	}

	TSharedPtr<FJsonValue> BuildObjectArrayValue(
		const TArray<TSharedPtr<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Objects.Num());
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			Items.Add(MakeShared<FJsonValueObject>(Object));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	TSharedPtr<FJsonObject> BuildInputMappingValue(
		const FString& ActionRef,
		const FString& Key,
		const TArray<FString>& Triggers,
		const TArray<FString>& Modifiers)
	{
		const TSharedPtr<FJsonObject> Mapping =
			MakeShared<FJsonObject>();
		Mapping->SetStringField(TEXT("action_ref"), ActionRef);
		Mapping->SetStringField(TEXT("key"), Key);
		Mapping->SetField(
			TEXT("triggers"),
			BuildStringArrayValue(Triggers));
		Mapping->SetField(
			TEXT("modifiers"),
			BuildStringArrayValue(Modifiers));
		return Mapping;
	}

	TSharedPtr<FJsonObject> BuildInputMappingIdentityValue(
		const FString& ActionRef,
		const FString& Key)
	{
		const TSharedPtr<FJsonObject> Mapping =
			MakeShared<FJsonObject>();
		Mapping->SetStringField(TEXT("action_ref"), ActionRef);
		Mapping->SetStringField(TEXT("key"), Key);
		return Mapping;
	}

	TSharedPtr<FJsonValue> BuildLinearColorValue(
		const double Red,
		const double Green,
		const double Blue,
		const double Alpha)
	{
		const TSharedPtr<FJsonObject> Color =
			MakeShared<FJsonObject>();
		Color->SetNumberField(TEXT("r"), Red);
		Color->SetNumberField(TEXT("g"), Green);
		Color->SetNumberField(TEXT("b"), Blue);
		Color->SetNumberField(TEXT("a"), Alpha);
		return MakeShared<FJsonValueObject>(Color);
	}

	TSharedPtr<FJsonValue> BuildExtent2iValue(
		const int32 X,
		const int32 Y)
	{
		const TSharedPtr<FJsonObject> Extent =
			MakeShared<FJsonObject>();
		Extent->SetNumberField(TEXT("x"), X);
		Extent->SetNumberField(TEXT("y"), Y);
		return MakeShared<FJsonValueObject>(Extent);
	}

	TSharedPtr<FJsonValue> BuildRect2dValue(
		const int32 X,
		const int32 Y,
		const int32 Width,
		const int32 Height)
	{
		const TSharedPtr<FJsonObject> Rect =
			MakeShared<FJsonObject>();
		Rect->SetNumberField(TEXT("x"), X);
		Rect->SetNumberField(TEXT("y"), Y);
		Rect->SetNumberField(TEXT("width"), Width);
		Rect->SetNumberField(TEXT("height"), Height);
		return MakeShared<FJsonValueObject>(Rect);
	}

	TSharedPtr<FJsonValue> BuildVector2dValue(
		const int32 X,
		const int32 Y)
	{
		return BuildExtent2iValue(X, Y);
	}

	TSharedPtr<FJsonObject> BuildPaperFlipbookFrameValue(
		const TSharedPtr<FJsonValue>& SpriteRef,
		const int32 FrameRun)
	{
		const TSharedPtr<FJsonObject> Frame =
			MakeShared<FJsonObject>();
		Frame->SetField(TEXT("sprite_ref"), SpriteRef);
		Frame->SetNumberField(TEXT("frame_run"), FrameRun);
		return Frame;
	}

	TSharedPtr<FJsonObject> BuildPaperTileMapCellValue(
		const int32 X,
		const int32 Y,
		const FString& TileSetRef,
		const int32 TileIndex,
		const bool bFlipHorizontal,
		const bool bFlipVertical,
		const bool bFlipDiagonal)
	{
		const TSharedPtr<FJsonObject> Cell =
			MakeShared<FJsonObject>();
		Cell->SetNumberField(TEXT("x"), X);
		Cell->SetNumberField(TEXT("y"), Y);
		Cell->SetStringField(TEXT("tile_set_ref"), TileSetRef);
		Cell->SetNumberField(TEXT("tile_index"), TileIndex);
		Cell->SetBoolField(TEXT("flip_horizontal"), bFlipHorizontal);
		Cell->SetBoolField(TEXT("flip_vertical"), bFlipVertical);
		Cell->SetBoolField(TEXT("flip_diagonal"), bFlipDiagonal);
		return Cell;
	}

	TSharedPtr<FJsonObject> BuildPaperTileMapLayerValue(
		const FString& Name,
		const bool bVisibleInEditor,
		const bool bVisibleInGame,
		const bool bCollides,
		const TSharedPtr<FJsonValue>& ThicknessOverride,
		const TSharedPtr<FJsonValue>& OffsetOverride,
		const TSharedPtr<FJsonValue>& Color,
		const TArray<TSharedPtr<FJsonObject>>& Cells)
	{
		const TSharedPtr<FJsonObject> Layer =
			MakeShared<FJsonObject>();
		Layer->SetStringField(TEXT("name"), Name);
		Layer->SetBoolField(
			TEXT("visible_in_editor"),
			bVisibleInEditor);
		Layer->SetBoolField(TEXT("visible_in_game"), bVisibleInGame);
		Layer->SetBoolField(TEXT("collides"), bCollides);
		Layer->SetField(
			TEXT("collision_thickness_override"),
			ThicknessOverride);
		Layer->SetField(
			TEXT("collision_offset_override"),
			OffsetOverride);
		Layer->SetField(TEXT("color"), Color);
		Layer->SetField(TEXT("cells"), BuildObjectArrayValue(Cells));
		return Layer;
	}

	TSharedPtr<FJsonObject> BuildPaperTileSetTerrainValue(
		const FString& Name,
		const int32 CenterTileIndex)
	{
		const TSharedPtr<FJsonObject> Terrain =
			MakeShared<FJsonObject>();
		Terrain->SetStringField(TEXT("name"), Name);
		Terrain->SetNumberField(
			TEXT("center_tile_index"),
			CenterTileIndex);
		return Terrain;
	}

	bool ReadExactBoolProperty(
		const UObject& Object,
		const FName PropertyName,
		bool& bOutValue)
	{
		const FBoolProperty* Property =
			FindFProperty<FBoolProperty>(
				Object.GetClass(),
				PropertyName);
		if (!Property)
		{
			return false;
		}
		bOutValue = Property->GetPropertyValue_InContainer(&Object);
		return true;
	}

	bool ReadExactFloatProperty(
		const UObject& Object,
		const FName PropertyName,
		float& OutValue)
	{
		const FFloatProperty* Property =
			FindFProperty<FFloatProperty>(
				Object.GetClass(),
				PropertyName);
		if (!Property)
		{
			return false;
		}
		OutValue = Property->GetPropertyValue_InContainer(&Object);
		return true;
	}

	bool ReadMediaPlayerLoopSetting(
		const UMediaPlayer& MediaPlayer)
	{
		const FBoolProperty* LoopProperty =
			FindFProperty<FBoolProperty>(
				UMediaPlayer::StaticClass(),
				TEXT("Loop"));
		return LoopProperty
			&& LoopProperty->GetPropertyValue_InContainer(
				&MediaPlayer);
	}

	TSharedPtr<FJsonValue> BuildCurveKeysValue(
		const int32 Dimension)
	{
		const auto BuildKey =
			[Dimension](
				const double Time,
				const double BaseValue,
				const FString& Interp,
				const FString& Tangent,
				const FString& Weight)
			{
				TArray<double> Values;
				TArray<double> ArriveTangents;
				TArray<double> LeaveTangents;
				TArray<double> ArriveWeights;
				TArray<double> LeaveWeights;
				for (int32 Index = 0; Index < Dimension; ++Index)
				{
					Values.Add(BaseValue + Index * 0.25);
					ArriveTangents.Add(0.1 + Index * 0.05);
					LeaveTangents.Add(0.2 + Index * 0.05);
					ArriveWeights.Add(0.3 + Index * 0.05);
					LeaveWeights.Add(0.4 + Index * 0.05);
				}
				const TSharedPtr<FJsonObject> Key =
					MakeShared<FJsonObject>();
				Key->SetNumberField(TEXT("time"), Time);
				Key->SetField(
					TEXT("values"),
					BuildNumberArrayValue(Values));
				Key->SetStringField(TEXT("interp_mode"), Interp);
				Key->SetStringField(TEXT("tangent_mode"), Tangent);
				Key->SetStringField(
					TEXT("tangent_weight_mode"),
					Weight);
				Key->SetField(
					TEXT("arrive_tangents"),
					BuildNumberArrayValue(ArriveTangents));
				Key->SetField(
					TEXT("leave_tangents"),
					BuildNumberArrayValue(LeaveTangents));
				Key->SetField(
					TEXT("arrive_tangent_weights"),
					BuildNumberArrayValue(ArriveWeights));
				Key->SetField(
					TEXT("leave_tangent_weights"),
					BuildNumberArrayValue(LeaveWeights));
				return Key;
			};
		return BuildObjectArrayValue(
			{
				BuildKey(
					0.0,
					0.25,
					TEXT("cubic"),
					TEXT("user"),
					TEXT("both")),
				BuildKey(
					1.0,
					1.0,
					TEXT("linear"),
					TEXT("auto"),
					TEXT("none")),
			});
	}

	TSharedPtr<FJsonValue> BuildVector3Value(
		const double X,
		const double Y,
		const double Z)
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("x"), X);
		Value->SetNumberField(TEXT("y"), Y);
		Value->SetNumberField(TEXT("z"), Z);
		return MakeShared<FJsonValueObject>(Value);
	}

	TSharedPtr<FJsonValue> BuildRotatorValue(
		const double Pitch,
		const double Yaw,
		const double Roll)
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("pitch"), Pitch);
		Value->SetNumberField(TEXT("yaw"), Yaw);
		Value->SetNumberField(TEXT("roll"), Roll);
		return MakeShared<FJsonValueObject>(Value);
	}

	TSharedPtr<FJsonObject> BuildMaterialSlotValue(
		const FString& SlotName,
		const FString& MaterialRef)
	{
		const TSharedPtr<FJsonObject> Slot =
			MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("slot_name"), SlotName);
		Slot->SetStringField(TEXT("material_ref"), MaterialRef);
		return Slot;
	}

	TSharedPtr<FJsonValue> BuildStaticCollisionValue()
	{
		const TSharedPtr<FJsonObject> Box =
			MakeShared<FJsonObject>();
		Box->SetField(TEXT("center"), BuildVector3Value(1.0, 2.0, 3.0));
		Box->SetField(TEXT("extent"), BuildVector3Value(10.0, 20.0, 30.0));
		Box->SetField(TEXT("rotation"), BuildRotatorValue(5.0, 10.0, 15.0));

		const TSharedPtr<FJsonObject> Sphere =
			MakeShared<FJsonObject>();
		Sphere->SetField(TEXT("center"), BuildVector3Value(4.0, 5.0, 6.0));
		Sphere->SetNumberField(TEXT("radius"), 12.0);

		const TSharedPtr<FJsonObject> Capsule =
			MakeShared<FJsonObject>();
		Capsule->SetField(TEXT("center"), BuildVector3Value(7.0, 8.0, 9.0));
		Capsule->SetField(TEXT("rotation"), BuildRotatorValue(0.0, 30.0, 0.0));
		Capsule->SetNumberField(TEXT("radius"), 6.0);
		Capsule->SetNumberField(TEXT("length"), 24.0);

		const TSharedPtr<FJsonObject> Collision =
			MakeShared<FJsonObject>();
		Collision->SetStringField(
			TEXT("trace_mode"),
			TEXT("use_simple_and_complex"));
		Collision->SetField(TEXT("boxes"), BuildObjectArrayValue({Box}));
		Collision->SetField(TEXT("spheres"), BuildObjectArrayValue({Sphere}));
		Collision->SetField(TEXT("capsules"), BuildObjectArrayValue({Capsule}));
		return MakeShared<FJsonValueObject>(Collision);
	}

	TSharedPtr<FJsonObject> BuildStaticSocketValue(
		const FString& Name,
		const FVector& Location,
		const FRotator& Rotation,
		const FVector& Scale)
	{
		const TSharedPtr<FJsonObject> Socket =
			MakeShared<FJsonObject>();
		Socket->SetStringField(TEXT("name"), Name);
		Socket->SetField(
			TEXT("location"),
			BuildVector3Value(Location.X, Location.Y, Location.Z));
		Socket->SetField(
			TEXT("rotation"),
			BuildRotatorValue(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
		Socket->SetField(
			TEXT("scale"),
			BuildVector3Value(Scale.X, Scale.Y, Scale.Z));
		return Socket;
	}

	TSharedPtr<FJsonValue> BuildStaticBuildSettingsValue(
		const bool bRecomputeNormals,
		const bool bRecomputeTangents,
		const bool bWeightedNormals,
		const bool bRemoveDegenerates,
		const bool bUseMikk,
		const bool bGenerateLightmapUvs)
	{
		const TSharedPtr<FJsonObject> Lod =
			MakeShared<FJsonObject>();
		Lod->SetNumberField(TEXT("lod_index"), 0);
		Lod->SetBoolField(TEXT("recompute_normals"), bRecomputeNormals);
		Lod->SetBoolField(TEXT("recompute_tangents"), bRecomputeTangents);
		Lod->SetBoolField(TEXT("compute_weighted_normals"), bWeightedNormals);
		Lod->SetBoolField(TEXT("remove_degenerates"), bRemoveDegenerates);
		Lod->SetBoolField(TEXT("use_mikk_tspace"), bUseMikk);
		Lod->SetBoolField(
			TEXT("generate_lightmap_uvs"),
			bGenerateLightmapUvs);
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetField(TEXT("lods"), BuildObjectArrayValue({Lod}));
		return MakeShared<FJsonValueObject>(Value);
	}

	TSharedPtr<FJsonValue> BuildDataTableRowsValue()
	{
		const TSharedPtr<FJsonObject> Values =
			MakeShared<FJsonObject>();
		Values->SetStringField(TEXT("Label"), TEXT("Updated"));
		Values->SetNumberField(TEXT("Count"), 7);
		Values->SetNumberField(TEXT("Weight"), 1.5);
		Values->SetBoolField(TEXT("bEnabled"), true);
		const TSharedPtr<FJsonObject> Row =
			MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), TEXT("RowA"));
		Row->SetObjectField(TEXT("values"), Values);
		return BuildObjectArrayValue({Row});
	}

	TSharedPtr<FJsonValue> BuildBoundsValue(
		const FVector& Min,
		const FVector& Max)
	{
		const TSharedPtr<FJsonObject> Bounds =
			MakeShared<FJsonObject>();
		Bounds->SetField(
			TEXT("min"),
			BuildVector3Value(Min.X, Min.Y, Min.Z));
		Bounds->SetField(
			TEXT("max"),
			BuildVector3Value(Max.X, Max.Y, Max.Z));
		return MakeShared<FJsonValueObject>(Bounds);
	}

	TSharedPtr<FJsonValue> BuildLodSettingsValue(
		const FString& LodSettingsRef)
	{
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("lod_settings_ref"), LodSettingsRef);
		return MakeShared<FJsonValueObject>(Value);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataCreateProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataCreateProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataCreateProductionContractTest::
	RunTest(const FString& Parameters)
{
	static_assert(
		UAI_ASSET_DATA_SIMPLE_AUTHORING_PRODUCTION_V1 == 1,
		"AssetData Simple Authoring production contract changed");
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TArray<FUeAgentAssetDataSimpleEffectProductionProfile>
		Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT(
				"formal AssetData catalog projects production profiles"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		AddError(
			TEXT("sa008_asset_data_profile_catalog_invalid:")
			+ ProfileError);
		return false;
	}
	TestEqual(
		TEXT("all 53 AssetData profiles are projected"),
		Profiles.Num(),
		53);
	int32 CreateCount = 0;
	int32 StaticCreateCount = 0;
	int32 FactoryCreateCount = 0;
	int32 CloneCount = 0;
	TArray<const FUeAgentAssetDataSimpleEffectProductionProfile*>
		StaticCreates;
	TArray<const FUeAgentAssetDataSimpleEffectProductionProfile*>
		CreateProfiles;
	for (const FUeAgentAssetDataSimpleEffectProductionProfile&
		Profile : Profiles)
	{
		CreateCount += Profile.IsCreate() ? 1 : 0;
		StaticCreateCount +=
			Profile.UsesStaticCreateClass() ? 1 : 0;
		FactoryCreateCount +=
			Profile.UsesFactoryCandidate() ? 1 : 0;
		CloneCount +=
			Profile.NativeOperationType
					== TEXT("asset.object.clone")
				? 1
				: 0;
		if (Profile.UsesStaticCreateClass())
		{
			StaticCreates.Add(&Profile);
		}
		if (Profile.IsCreate())
		{
			CreateProfiles.Add(&Profile);
		}
	}
	TestEqual(TEXT("create profile count"), CreateCount, 23);
	TestEqual(
		TEXT("static create profile count"),
		StaticCreateCount,
		19);
	TestEqual(
		TEXT("factory create profile count"),
		FactoryCreateCount,
		4);
	TestEqual(TEXT("clone profile count"), CloneCount, 1);
	TestEqual(
		TEXT("mutable profile count"),
		Profiles.Num() - CreateCount - CloneCount,
		29);

	FOwnedAssetDataCreatePackage Owner;
	if (!TestTrue(
			TEXT("owned AssetData package is valid"),
			Owner.IsValid()))
	{
		return false;
	}
	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		MutationResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		PreStateResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		PersistedResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<
		FFixtureFactoryCandidateResolver,
		ESPMode::ThreadSafe>
		FactoryResolver =
			MakeShared<
				FFixtureFactoryCandidateResolver,
				ESPMode::ThreadSafe>(Fixture);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(
		TEXT("Simple Authoring descriptors compose"),
		TryCreateSimpleAuthoringDescriptorRegistry(
			Registry,
			RegistryError));
	if (!Registry.IsValid())
	{
		AddError(
			TEXT("sa008_asset_data_registry_invalid:")
			+ RegistryError);
		return false;
	}

	FUeAgentAssetDataSimpleAuthoringProviderConfig
		ProviderConfig;
	ProviderConfig.DescriptorRegistry = Registry;
	ProviderConfig.MutationAssetResolver = MutationResolver;
	ProviderConfig.PreStateAssetResolver = PreStateResolver;
	ProviderConfig.PersistedAssetResolver = PersistedResolver;
	ProviderConfig.FactoryResolver = FactoryResolver;
	ProviderConfig.DurableService = Durable;
	ProviderConfig.AbsenceOracle =
		MakeShared<FUnusedAbsenceOracle>();
	ProviderConfig.ReferenceOracle =
		MakeShared<FUnusedReferenceOracle>();
	ProviderConfig.OwnerLeaseId =
		TEXT("owner:sa008-asset-data-create");
	ProviderConfig.Projection.Emplace(
		FUeAgentProjectionProfile(
			TEXT("compact"),
			false,
			false,
			false),
		FUeAgentPageRequest(),
		TOptional<int64>(),
		TArray<FUeAgentArtifactDescriptor>(),
		TArray<FUeAgentProjectionDiagnostic>());

	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
	FUeAgentAssetDataSimpleAuthoringError ProviderError;
	TestTrue(
		TEXT("AssetData production family composes"),
		FUeAgentAssetDataSimpleAuthoringProviderFactory::TryCreate(
			ProviderConfig,
			Provider,
			ProviderError));
	if (!Provider.IsValid())
	{
		AddError(
			TEXT("sa008_asset_data_provider_invalid:")
			+ ProviderError.Code
			+ TEXT(":")
			+ ProviderError.Detail);
		return false;
	}

	TArray<FUeAgentSimpleAuthoringCommandRoute> Routes;
	for (const
		FUeAgentAssetDataSimpleEffectProductionProfile*
			Profile : CreateProfiles)
	{
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Profile->CapabilityId);
		if (!TestNotNull(
				*(Profile->CapabilityId
					+ TEXT(" has one exact route")),
				Route))
		{
			return false;
		}
		Routes.Add(*Route);
	}

	TSharedPtr<const FUeAgentOperationTicketService>
		TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("ticket service creates"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-asset-data-create-integrity"),
			60.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT("ticket:sa008-asset-data-create"));
			},
			TicketService,
			TicketError));
	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	TestTrue(
		TEXT("response projector creates"),
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FTokenEstimator>(),
			Projector,
			ProjectionError));
	if (!TicketService.IsValid() || !Projector.IsValid())
	{
		return false;
	}

	FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
	ApplicationConfig.DescriptorRegistry = Registry;
	ApplicationConfig.ExecutionContext.ProjectInstanceId =
		TEXT("project:sa008-asset-data-create");
	ApplicationConfig.ExecutionContext.EditorInstanceId =
		TEXT("editor:sa008-asset-data-create");
	ApplicationConfig.ExecutionContext.StatePlane =
		EUeAgentStatePlane::AuthoringMemory;
	ApplicationConfig.ExecutionContext.WorldType =
		EUeAgentExecutionWorldType::Editor;
	ApplicationConfig.ExecutionContext.ContextRevision =
		TEXT("project:sa008-asset-data-create:r1");
	ApplicationConfig.ExecutionContext.bResolvedFromCurrent =
		false;
	ApplicationConfig.ServerSessionId =
		TEXT("session:sa008-asset-data-create");
	ApplicationConfig.RegistryEpoch = 1;
	ApplicationConfig.TimeoutSeconds = 60.0;
	ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;

	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	TestTrue(
		TEXT(
			"application accepts exact AssetData static create routes"),
		FUeAgentSimpleAuthoringApplication::TryCreate(
			ApplicationConfig,
			MoveTemp(Routes),
			{Provider.ToSharedRef()},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Application,
			ApplicationError));
	if (!Application.IsValid())
	{
		AddError(
			TEXT("sa008_asset_data_application_invalid:")
			+ ApplicationError.Code
			+ TEXT(":")
			+ ApplicationError.Detail);
		return false;
	}

	const FUeAgentCallerContext Caller(
		TEXT("caller:sa008-asset-data-create"),
		{TEXT("uai:asset-data:write")},
		TEXT("correlation:sa008-asset-data-create"));
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	TArray<FString> CreatedTargets;
	for (int32 Index = 0;
		Index < CreateProfiles.Num();
		++Index)
	{
		const
			FUeAgentAssetDataSimpleEffectProductionProfile&
				Profile = *CreateProfiles[Index];
		const FString TargetRef = Owner.AddTarget(Index);
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Profile.CapabilityId);
		FString ExpectedClassPath;
		const TSharedPtr<FJsonObject> Payload =
			BuildCreatePayload(
				TargetRef,
				Profile,
				Fixture,
				ExpectedClassPath);
		if (!TestTrue(
				*(Profile.CapabilityId
					+ TEXT(" fixture payload is valid")),
				Payload.IsValid()))
		{
			continue;
		}
		FString PayloadJson;
		TestTrue(
			*(Profile.CapabilityId
				+ TEXT(" payload serializes")),
			Serialize(Payload, PayloadJson));
		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					FString::Printf(
						TEXT(
							"request:sa008-asset-data-create:%02d"),
						Index),
					FString::Printf(
						TEXT(
							"idempotency:sa008-asset-data-create:%02d"),
						Index),
					FUeAgentContractValue(PayloadJson)),
				Caller,
				Cancellation);
		if (!IsComplete(Result))
		{
			AddError(
				Profile.CapabilityId
				+ TEXT(
					" did not complete verified lifecycle; result=")
				+ Result.GetValue().GetJsonValue());
			continue;
		}
		UObject* Created = Owner.FindTarget(TargetRef);
		if (!TestNotNull(
				*(Profile.CapabilityId
					+ TEXT(
						" independently resolves created asset")),
				Created))
		{
			continue;
		}
		TestEqual(
			*(Profile.CapabilityId
				+ TEXT(" uses exact static class")),
			Created->GetClass()->GetPathName(),
			ExpectedClassPath);
		TestEqual(
			*(Profile.CapabilityId
				+ TEXT(" uses exact package")),
			Created->GetOutermost()->GetName(),
			Owner.Package->GetName());
		CreatedTargets.Add(TargetRef);
	}

	TestEqual(
		TEXT("23 create operations prepare exactly once"),
		Durable->PrepareCount,
		23);
	TestEqual(
		TEXT("23 create operations persist exactly once"),
		Durable->PersistCount,
		23);
	TestEqual(
		TEXT(
			"23 create operations acknowledge only after C9"),
		Durable->AcknowledgeCount,
		23);
	TestEqual(
		TEXT("all durable targets use the owned package"),
		Durable->SeenPackageNames.Num(),
		1);
	TestTrue(
		TEXT("durable target is the owned package"),
		Durable->SeenPackageNames.Contains(
			Owner.Package->GetName()));

	if (CreatedTargets.Num() == 23)
	{
		const
			FUeAgentAssetDataSimpleEffectProductionProfile&
				Profile = *CreateProfiles[0];
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Profile.CapabilityId);
		FString ExpectedClassPath;
		const TSharedPtr<FJsonObject> Payload =
			BuildCreatePayload(
				CreatedTargets[0],
				Profile,
				Fixture,
				ExpectedClassPath);
		FString PayloadJson;
		Serialize(Payload, PayloadJson);
		const FUeAgentOperationResult Duplicate =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT(
						"request:sa008-asset-data-create:duplicate"),
					TEXT(
						"idempotency:sa008-asset-data-create:duplicate"),
					FUeAgentContractValue(PayloadJson)),
				Caller,
				Cancellation);
		TestFalse(
			TEXT(
				"duplicate exact asset fails before mutation"),
			IsComplete(Duplicate));
		TestEqual(
			TEXT(
				"duplicate exact asset does not prepare durable work"),
			Durable->PrepareCount,
			23);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataCloneProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataCloneProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataCloneProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile>
		Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT("formal AssetData catalog projects clone profile"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}
	const
		FUeAgentAssetDataSimpleEffectProductionProfile*
			CloneProfile = Profiles.FindByPredicate(
				[](
					const
						FUeAgentAssetDataSimpleEffectProductionProfile&
							Profile)
				{
					return Profile.NativeOperationType
						== TEXT("asset.object.clone");
				});
	if (!TestNotNull(
			TEXT("one formal generic clone profile exists"),
			CloneProfile))
	{
		return false;
	}
	const FUeAgentSimpleAuthoringCommandRoute* Route =
		FindRoute(CloneProfile->CapabilityId);
	if (!TestNotNull(
			TEXT("generic clone has one exact route"),
			Route))
	{
		return false;
	}

	FOwnedAssetDataCreatePackage Owner;
	if (!TestTrue(
			TEXT("owned clone package is valid"),
			Owner.IsValid()))
	{
		return false;
	}
	const FString SourceRef =
		TEXT("uai://asset/sa008-clone-source");
	const FString TargetRef =
		TEXT("uai://asset/sa008-clone-target");
	const FString StaleTargetRef =
		TEXT("uai://asset/sa008-clone-stale-target");
	Owner.BindRef(
		SourceRef,
		FName(TEXT("SA008_CloneSource")));
	Owner.BindRef(
		TargetRef,
		FName(TEXT("SA008_CloneTarget")));
	Owner.BindRef(
		StaleTargetRef,
		FName(TEXT("SA008_CloneStaleTarget")));
	UUeAgentAssetDataSimpleEffectDataAsset* Related =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
			Owner.Package,
			TEXT("SA008_CloneRelated"),
			RF_Public | RF_Standalone | RF_Transactional);
	UUeAgentAssetDataSimpleEffectDataAsset* Source =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
			Owner.Package,
			TEXT("SA008_CloneSource"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(TEXT("clone source exists"), Source)
		|| !TestNotNull(TEXT("clone related asset exists"), Related))
	{
		return false;
	}
	Source->Label = TEXT("SA008 Complete Clone State");
	Source->Count = -901;
	Source->Weight = 7.625f;
	Source->bEnabled = true;
	Source->RelatedAsset = Related;
	const FString SourceFingerprint =
		ReadCloneDataFingerprint(*Source);
	const FString SourceRevision =
		BuildAssetRevision(Source);
	if (!TestFalse(
			TEXT("source revision is independently derived"),
			SourceRevision.IsEmpty()))
	{
		return false;
	}

	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		MutationResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		PreStateResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<FOwnedAssetResolver, ESPMode::ThreadSafe>
		PersistedResolver =
			MakeShared<FOwnedAssetResolver, ESPMode::ThreadSafe>(
				Owner);
	const TSharedRef<
		FFixtureFactoryCandidateResolver,
		ESPMode::ThreadSafe>
		FactoryResolver =
			MakeShared<
				FFixtureFactoryCandidateResolver,
				ESPMode::ThreadSafe>(Fixture);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("Simple Authoring descriptors compose for clone"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		return false;
	}
	FUeAgentAssetDataSimpleAuthoringProviderConfig
		ProviderConfig;
	ProviderConfig.DescriptorRegistry = Registry;
	ProviderConfig.MutationAssetResolver = MutationResolver;
	ProviderConfig.PreStateAssetResolver = PreStateResolver;
	ProviderConfig.PersistedAssetResolver = PersistedResolver;
	ProviderConfig.FactoryResolver = FactoryResolver;
	ProviderConfig.DurableService = Durable;
	ProviderConfig.AbsenceOracle =
		MakeShared<FUnusedAbsenceOracle>();
	ProviderConfig.ReferenceOracle =
		MakeShared<FUnusedReferenceOracle>();
	ProviderConfig.OwnerLeaseId =
		TEXT("owner:sa008-asset-data-clone");
	ProviderConfig.Projection.Emplace(
		FUeAgentProjectionProfile(
			TEXT("compact"),
			false,
			false,
			false),
		FUeAgentPageRequest(),
		TOptional<int64>(),
		TArray<FUeAgentArtifactDescriptor>(),
		TArray<FUeAgentProjectionDiagnostic>());

	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
	FUeAgentAssetDataSimpleAuthoringError ProviderError;
	if (!TestTrue(
			TEXT("AssetData clone production family composes"),
			FUeAgentAssetDataSimpleAuthoringProviderFactory::TryCreate(
				ProviderConfig,
				Provider,
				ProviderError))
		|| !Provider.IsValid())
	{
		AddError(
			TEXT("sa008_asset_data_clone_provider_invalid:")
			+ ProviderError.Code
			+ TEXT(":")
			+ ProviderError.Detail);
		return false;
	}

	TSharedPtr<const FUeAgentOperationTicketService>
		TicketService;
	FUeAgentOperationTicketError TicketError;
	if (!TestTrue(
			TEXT("clone ticket service creates"),
			FUeAgentOperationTicketService::TryCreate(
				TEXT("sa008-asset-data-clone-integrity"),
				60.0,
				[]()
				{
					return 100.0;
				},
				[]()
				{
					return FString(
						TEXT("ticket:sa008-asset-data-clone"));
				},
				TicketService,
				TicketError)))
	{
		return false;
	}
	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	if (!TestTrue(
			TEXT("clone response projector creates"),
			FUeAgentResponseProjector::TryCreate(
				MakeShared<FTokenEstimator>(),
				Projector,
				ProjectionError)))
	{
		return false;
	}

	FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
	ApplicationConfig.DescriptorRegistry = Registry;
	ApplicationConfig.ExecutionContext.ProjectInstanceId =
		TEXT("project:sa008-asset-data-clone");
	ApplicationConfig.ExecutionContext.EditorInstanceId =
		TEXT("editor:sa008-asset-data-clone");
	ApplicationConfig.ExecutionContext.StatePlane =
		EUeAgentStatePlane::AuthoringMemory;
	ApplicationConfig.ExecutionContext.WorldType =
		EUeAgentExecutionWorldType::Editor;
	ApplicationConfig.ExecutionContext.ContextRevision =
		TEXT("project:sa008-asset-data-clone:r1");
	ApplicationConfig.ExecutionContext.bResolvedFromCurrent =
		false;
	ApplicationConfig.ServerSessionId =
		TEXT("session:sa008-asset-data-clone");
	ApplicationConfig.RegistryEpoch = 1;
	ApplicationConfig.TimeoutSeconds = 60.0;
	ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;

	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	if (!TestTrue(
			TEXT("application accepts exact AssetData clone route"),
			FUeAgentSimpleAuthoringApplication::TryCreate(
				ApplicationConfig,
				{*Route},
				{Provider.ToSharedRef()},
				TicketService.ToSharedRef(),
				Projector.ToSharedRef(),
				Application,
				ApplicationError))
		|| !Application.IsValid())
	{
		AddError(
			TEXT("sa008_asset_data_clone_application_invalid:")
			+ ApplicationError.Code
			+ TEXT(":")
			+ ApplicationError.Detail);
		return false;
	}

	const FUeAgentCallerContext Caller(
		TEXT("caller:sa008-asset-data-clone"),
		{TEXT("uai:asset-data:write")},
		TEXT("correlation:sa008-asset-data-clone"));
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	FString PayloadJson;
	TestTrue(
		TEXT("clone payload serializes"),
		Serialize(
			BuildClonePayload(
				TargetRef,
				SourceRef,
				SourceRevision),
			PayloadJson));
	const FUeAgentOperationResult Result =
		Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-asset-data-clone"),
				TEXT("idempotency:sa008-asset-data-clone"),
				FUeAgentContractValue(PayloadJson)),
			Caller,
			Cancellation);
	if (!IsComplete(Result))
	{
		AddError(
			TEXT(
				"generic clone did not complete verified lifecycle; "
				"result=")
			+ Result.GetValue().GetJsonValue());
		return false;
	}

	UUeAgentAssetDataSimpleEffectDataAsset* Target =
		Cast<UUeAgentAssetDataSimpleEffectDataAsset>(
			Owner.FindTarget(TargetRef));
	if (!TestNotNull(
			TEXT("clone target independently resolves"),
			Target))
	{
		return false;
	}
	TestTrue(
		TEXT("clone target is an independent exact-class object"),
		Target != Source
			&& Target->GetClass() == Source->GetClass());
	TestEqual(
		TEXT("clone target preserves complete registered state"),
		ReadCloneDataFingerprint(*Target),
		SourceFingerprint);
	TestEqual(
		TEXT("clone source remains unchanged after C9"),
		ReadCloneDataFingerprint(*Source),
		SourceFingerprint);
	TestTrue(
		TEXT("clone target has durable asset flags"),
		Target->HasAllFlags(
			RF_Public | RF_Standalone | RF_Transactional));
	TestEqual(
		TEXT("clone prepares durable work once"),
		Durable->PrepareCount,
		1);
	TestEqual(
		TEXT("clone persists durable work once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("clone acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);

	FString StalePayloadJson;
	Serialize(
		BuildClonePayload(
			StaleTargetRef,
			SourceRef,
			TEXT("asset-crc32:00000000")),
		StalePayloadJson);
	const FUeAgentOperationResult StaleResult =
		Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-asset-data-clone-stale"),
				TEXT("idempotency:sa008-asset-data-clone-stale"),
				FUeAgentContractValue(StalePayloadJson)),
			Caller,
			Cancellation);
	TestFalse(
		TEXT("stale source revision fails before mutation"),
		IsComplete(StaleResult));
	TestNull(
		TEXT("stale source revision creates no target"),
		Owner.FindTarget(StaleTargetRef));
	TestEqual(
		TEXT("stale source revision prepares no durable work"),
		Durable->PrepareCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataRegisteredFieldsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataRegisteredFieldsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataRegisteredFieldsProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile>
		Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT(
				"formal catalog projects data_object update profile"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		Profile = Profiles.FindByPredicate(
			[](
				const
					FUeAgentAssetDataSimpleEffectProductionProfile&
						Candidate)
			{
				return Candidate.DescriptorId
					== TEXT(
						"uai.operation.asset.data_object.update");
			});
	if (!TestNotNull(
			TEXT("one exact data_object update profile exists"),
			Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("data_object update exposes one aggregate field"),
		Profile->FieldIds
			== TArray<FString>{TEXT("registered_fields")});
	TestTrue(
		TEXT(
			"data_object update preserves its formal field contract"),
		Profile->Fields.Num() == 1
			&& Profile->Fields[0].FieldId
				== TEXT("registered_fields")
			&& Profile->Fields[0].FieldBindingId
				== TEXT(
					"uai.asset_data.field:"
					"data_object:registered_fields:v1")
			&& Profile->Fields[0].TypeId
				== TEXT(
					"uai.asset.data_object."
					"registered_fields.v1")
			&& Profile->Fields[0].ValueKind
				== UE::UAI::StateSemantic::
					EUeAgentValueKind::Object
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
				}
			&& Profile->Fields[0].ReferencePolicy
				== TEXT("recursive_asset_refs"));
	const FUeAgentSimpleAuthoringCommandRoute* Route =
		FindRoute(Profile->CapabilityId);
	if (!TestNotNull(
			TEXT("data_object update has one exact route"),
			Route))
	{
		return false;
	}

	FOwnedAssetDataCreatePackage Owner;
	if (!TestTrue(
			TEXT("owned data_object update package is valid"),
			Owner.IsValid()))
	{
		return false;
	}
	const FString TargetRef =
		TEXT("uai://asset/sa008-data-object-target");
	const FString RelatedRef =
		TEXT("uai://asset/sa008-data-object-related");
	Owner.BindRef(
		TargetRef,
		FName(TEXT("SA008_DataObjectTarget")));
	Owner.BindRef(
		RelatedRef,
		FName(TEXT("SA008_DataObjectRelated")));
	UUeAgentAssetDataSimpleEffectDataAsset* Related =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
			Owner.Package,
			TEXT("SA008_DataObjectRelated"),
			RF_Public | RF_Standalone | RF_Transactional);
	UUeAgentAssetDataSimpleEffectDataAsset* Target =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
			Owner.Package,
			TEXT("SA008_DataObjectTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(
			TEXT("data_object update target exists"),
			Target)
		|| !TestNotNull(
			TEXT("data_object related asset exists"),
			Related))
	{
		return false;
	}
	Target->Label = TEXT("Before");
	Target->Count = -17;
	Target->Weight = 9.25f;
	Target->bEnabled = false;
	Target->RelatedAsset = nullptr;
	const FString InitialRevision =
		BuildAssetRevision(Target);
	if (!TestFalse(
			TEXT("target revision is independently derived"),
			InitialRevision.IsEmpty()))
	{
		return false;
	}

	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	const FString Scope =
		TEXT("sa008-asset-data-registered-fields");
	if (!TryCreateAssetDataApplication(
			*this,
			Fixture,
			Owner,
			Durable,
			*Route,
			Scope,
			Application))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> RegisteredFields =
		MakeShared<FJsonObject>();
	RegisteredFields->SetStringField(
		TEXT("label"),
		TEXT("After"));
	RegisteredFields->SetNumberField(TEXT("count"), 71);
	RegisteredFields->SetBoolField(TEXT("enabled"), true);
	RegisteredFields->SetStringField(
		TEXT("related_asset"),
		RelatedRef);
	const TSharedPtr<FJsonObject> TypedValue =
		MakeShared<FJsonObject>();
	TypedValue->SetStringField(
		TEXT("type_id"),
		TEXT("uai.asset.data_object.registered_fields.v1"));
	TypedValue->SetObjectField(
		TEXT("canonical_value"),
		RegisteredFields);
	const TSharedPtr<FJsonObject> Change =
		MakeShared<FJsonObject>();
	Change->SetStringField(
		TEXT("field_id"),
		TEXT("registered_fields"));
	Change->SetStringField(TEXT("presence"), TEXT("set"));
	Change->SetObjectField(TEXT("value"), TypedValue);
	const TSharedPtr<FJsonObject> Payload =
		MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("target_ref"), TargetRef);
	Payload->SetStringField(
		TEXT("expected_revision"),
		InitialRevision);
	Payload->SetArrayField(
		TEXT("changes"),
		{MakeShared<FJsonValueObject>(Change)});
	FString PayloadJson;
	if (!TestTrue(
			TEXT("registered_fields payload serializes"),
			Serialize(Payload, PayloadJson)))
	{
		return false;
	}

	const FUeAgentCallerContext Caller(
		TEXT("caller:") + Scope,
		{TEXT("uai:asset-data:write")},
		TEXT("correlation:") + Scope);
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	const FUeAgentOperationResult Result =
		Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT(
					"request:sa008-asset-data-registered-fields"),
				TEXT(
					"idempotency:sa008-asset-data-registered-fields"),
				FUeAgentContractValue(PayloadJson)),
			Caller,
			Cancellation);
	if (!IsComplete(Result))
	{
		AddError(
			TEXT(
				"registered_fields did not complete verified "
				"lifecycle; result=")
			+ Result.GetValue().GetJsonValue());
		return false;
	}

	TestTrue(
		TEXT("update preserves the exact target instance"),
		Owner.FindTarget(TargetRef) == Target);
	TestEqual(
		TEXT("registered string is independently read back"),
		Target->Label,
		FString(TEXT("After")));
	TestEqual(
		TEXT("registered integer is independently read back"),
		Target->Count,
		71);
	TestEqual(
		TEXT("omitted registered float remains unchanged"),
		Target->Weight,
		9.25f);
	TestTrue(
		TEXT("registered boolean is independently read back"),
		Target->bEnabled);
	TestTrue(
		TEXT("registered asset reference is independently read back"),
		Target->RelatedAsset == Related);
	TestEqual(
		TEXT("update prepares durable work exactly once"),
		Durable->PrepareCount,
		1);
	TestEqual(
		TEXT("update persists durable work exactly once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("update acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);

	const FString VerifiedFingerprint =
		ReadCloneDataFingerprint(*Target);
	Payload->SetStringField(
		TEXT("expected_revision"),
		TEXT("asset-crc32:00000000"));
	RegisteredFields->SetStringField(
		TEXT("label"),
		TEXT("MustNotApply"));
	Serialize(Payload, PayloadJson);
	const FUeAgentOperationResult StaleResult =
		Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT(
					"request:sa008-asset-data-registered-fields-stale"),
				TEXT(
					"idempotency:sa008-asset-data-registered-fields-stale"),
				FUeAgentContractValue(PayloadJson)),
			Caller,
			Cancellation);
	TestFalse(
		TEXT("stale target revision fails before mutation"),
		IsComplete(StaleResult));
	TestEqual(
		TEXT("stale target revision causes zero mutation"),
		ReadCloneDataFingerprint(*Target),
		VerifiedFingerprint);
	TestEqual(
		TEXT("stale target revision prepares no durable work"),
		Durable->PrepareCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataCoreConfigurationProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataCoreConfigurationProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataCoreConfigurationProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile>
		Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT("formal AssetData production catalog composes"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}
	const auto FindProfileByDescriptor =
		[&Profiles](const FString& DescriptorId)
		{
			return Profiles.FindByPredicate(
				[&DescriptorId](
					const
						FUeAgentAssetDataSimpleEffectProductionProfile&
							Candidate)
				{
					return Candidate.DescriptorId == DescriptorId;
				});
		};
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		MediaSourceProfile = FindProfileByDescriptor(
			TEXT("uai.operation.asset.media_source.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		RvtProfile = FindProfileByDescriptor(
			TEXT(
				"uai.operation.asset.runtime_virtual_texture.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		SparseProfile = FindProfileByDescriptor(
			TEXT(
				"uai.operation.asset.sparse_volume_texture.update"));
	if (!TestNotNull(
			TEXT("media_source update profile exists"),
			MediaSourceProfile)
		|| !TestNotNull(
			TEXT("runtime_virtual_texture update profile exists"),
			RvtProfile)
		|| !TestNotNull(
			TEXT("sparse_volume_texture update profile exists"),
			SparseProfile))
	{
		return false;
	}
	TestTrue(
		TEXT("media_source field contract remains exact"),
		MediaSourceProfile->FieldIds
			== TArray<FString>{
				TEXT("source_uri"),
				TEXT("options"),
			}
			&& MediaSourceProfile->Fields.Num() == 2
			&& MediaSourceProfile->Fields[0].IsValid()
			&& MediaSourceProfile->Fields[1].IsValid());
	TestTrue(
		TEXT("runtime virtual texture field contract remains exact"),
		RvtProfile->FieldIds
			== TArray<FString>{
				TEXT("size"),
				TEXT("material_type"),
				TEXT("compression"),
			}
			&& RvtProfile->Fields.Num() == 3);
	TestTrue(
		TEXT("sparse volume texture field contract remains exact"),
		SparseProfile->FieldIds
			== TArray<FString>{
				TEXT("addressing"),
				TEXT("streaming"),
			}
			&& SparseProfile->Fields.Num() == 2);

	FOwnedAssetDataCreatePackage Owner;
	if (!TestTrue(
			TEXT("owned core configuration package is valid"),
			Owner.IsValid()))
	{
		return false;
	}
	const FString MediaSourceRef =
		TEXT("uai://asset/sa008-media-source-target");
	const FString RvtRef =
		TEXT("uai://asset/sa008-rvt-target");
	const FString SparseRef =
		TEXT("uai://asset/sa008-sparse-volume-target");
	Owner.BindRef(
		MediaSourceRef,
		FName(TEXT("SA008_MediaSourceTarget")));
	Owner.BindRef(
		RvtRef,
		FName(TEXT("SA008_RuntimeVirtualTextureTarget")));
	Owner.BindRef(
		SparseRef,
		FName(TEXT("SA008_SparseVolumeTextureTarget")));
	UFileMediaSource* MediaSource =
		NewObject<UFileMediaSource>(
			Owner.Package,
			TEXT("SA008_MediaSourceTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	URuntimeVirtualTexture* Rvt =
		NewObject<URuntimeVirtualTexture>(
			Owner.Package,
			TEXT("SA008_RuntimeVirtualTextureTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	UStreamableSparseVolumeTexture* Sparse =
		NewObject<UStreamableSparseVolumeTexture>(
			Owner.Package,
			TEXT("SA008_SparseVolumeTextureTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!TestNotNull(
			TEXT("owned media source exists"),
			MediaSource)
		|| !TestNotNull(
			TEXT("owned runtime virtual texture exists"),
			Rvt)
		|| !TestNotNull(
			TEXT("owned sparse volume texture exists"),
			Sparse))
	{
		return false;
	}
	MediaSource->SetFilePath(TEXT("before.mov"));
	MediaSource->PrecacheFile = false;
	Sparse->AddressX = TA_Wrap;
	Sparse->AddressY = TA_Wrap;
	Sparse->AddressZ = TA_Wrap;
	Sparse->bLocalDDCOnly = true;
	Sparse->StreamingPoolSizeFactor = 3.0f;
	Sparse->NumberOfPrefetchFrames = 2;
	Sparse->PrefetchPercentageStepSize = 5.0f;
	Sparse->PrefetchPercentageBias = 7.0f;

	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
	const TSharedRef<FNeverCancelled> Cancellation =
		MakeShared<FNeverCancelled>();
	const auto SubmitComplete =
		[this,
			&Fixture,
			&Owner,
			&Durable,
			&Cancellation](
				const
					FUeAgentAssetDataSimpleEffectProductionProfile&
						Profile,
				const TSharedPtr<FJsonObject>& Payload,
				const FString& Scope)
		{
			const FUeAgentSimpleAuthoringCommandRoute* Route =
				FindRoute(Profile.CapabilityId);
			if (!TestNotNull(
					*(Scope + TEXT(" has one exact route")),
					Route))
			{
				return false;
			}
			TSharedPtr<FUeAgentSimpleAuthoringApplication>
				Application;
			if (!TryCreateAssetDataApplication(
					*this,
					Fixture,
					Owner,
					Durable,
					*Route,
					Scope,
					Application))
			{
				return false;
			}
			FString PayloadJson;
			if (!TestTrue(
					*(Scope + TEXT(" payload serializes")),
					Serialize(Payload, PayloadJson)))
			{
				return false;
			}
			const FUeAgentOperationResult Result =
				Application->Submit(
					FUeAgentOperationRequest(
						Route->DescriptorIdentity,
						TEXT("request:") + Scope,
						TEXT("idempotency:") + Scope,
						FUeAgentContractValue(PayloadJson)),
					FUeAgentCallerContext(
						TEXT("caller:") + Scope,
						{TEXT("uai:asset-data:write")},
						TEXT("correlation:") + Scope),
					Cancellation);
			if (!IsComplete(Result))
			{
				AddError(
					Scope
					+ TEXT(
						" did not complete verified lifecycle; result=")
					+ Result.GetValue().GetJsonValue());
				return false;
			}
			return true;
		};

	const TSharedPtr<FJsonObject> MediaOptions =
		MakeShared<FJsonObject>();
	MediaOptions->SetBoolField(TEXT("precache_file"), true);
	const TSharedPtr<FJsonObject> MediaPayload =
		BuildUpdatePayload(
			MediaSourceRef,
			BuildAssetRevision(MediaSource),
			{
				BuildTypedChange(
					TEXT("source_uri"),
					TEXT("set"),
					TEXT("uai.value.uri.v1"),
					MakeShared<FJsonValueString>(
						TEXT("file:///fixture"))),
				BuildTypedChange(
					TEXT("options"),
					TEXT("replace"),
					TEXT("uai.asset.media_source.options.v1"),
					MakeShared<FJsonValueObject>(MediaOptions)),
			});
	if (!SubmitComplete(
			*MediaSourceProfile,
			MediaPayload,
			TEXT("sa008-asset-data-media-source")))
	{
		return false;
	}
	TestEqual(
		TEXT("media source URI is independently normalized"),
		MediaSource->GetFilePath(),
		FPaths::ConvertRelativePathToFull(TEXT("/fixture")));
	TestTrue(
		TEXT("media source options are independently read back"),
		MediaSource->PrecacheFile);
	const FString VerifiedMediaPath =
		MediaSource->GetFilePath();
	const TSharedPtr<FJsonObject> MediaClearPayload =
		BuildUpdatePayload(
			MediaSourceRef,
			BuildAssetRevision(MediaSource),
			{
				BuildTypedChange(
					TEXT("options"),
					TEXT("clear"),
					TEXT("uai.asset.media_source.options.v1"),
					nullptr),
			});
	if (!SubmitComplete(
			*MediaSourceProfile,
			MediaClearPayload,
			TEXT("sa008-asset-data-media-source-clear")))
	{
		return false;
	}
	TestEqual(
		TEXT("unselected media URI remains unchanged"),
		MediaSource->GetFilePath(),
		VerifiedMediaPath);
	TestEqual(
		TEXT("cleared media options use the native class default"),
		MediaSource->PrecacheFile,
		GetDefault<UFileMediaSource>()->PrecacheFile);

	const TSharedPtr<FJsonObject> RvtPayload =
		BuildUpdatePayload(
			RvtRef,
			BuildAssetRevision(Rvt),
			{
				BuildTypedChange(
					TEXT("size"),
					TEXT("set"),
					TEXT("uai.value.integer.v1"),
					MakeShared<FJsonValueNumber>(256.0)),
				BuildTypedChange(
					TEXT("material_type"),
					TEXT("set"),
					TEXT(
						"uai.asset.runtime_virtual_texture."
						"material_type.v1"),
					MakeShared<FJsonValueString>(
						TEXT("base_color"))),
				BuildTypedChange(
					TEXT("compression"),
					TEXT("set"),
					TEXT("uai.value.boolean.v1"),
					MakeShared<FJsonValueBoolean>(true)),
			});
	if (!SubmitComplete(
			*RvtProfile,
			RvtPayload,
			TEXT("sa008-asset-data-rvt")))
	{
		return false;
	}
	TestEqual(
		TEXT("runtime virtual texture size is independently read back"),
		Rvt->GetSize(),
		256);
	TestTrue(
		TEXT(
			"runtime virtual texture material is independently read back"),
		Rvt->GetMaterialType()
			== ERuntimeVirtualTextureMaterialType::BaseColor);
	TestTrue(
		TEXT(
			"runtime virtual texture compression is independently read back"),
		Rvt->GetCompressTextures());

	const TSharedPtr<FJsonObject> Addressing =
		MakeShared<FJsonObject>();
	Addressing->SetStringField(TEXT("x"), TEXT("clamp"));
	Addressing->SetStringField(TEXT("y"), TEXT("mirror"));
	Addressing->SetStringField(TEXT("z"), TEXT("wrap"));
	const TSharedPtr<FJsonObject> Streaming =
		MakeShared<FJsonObject>();
	Streaming->SetBoolField(TEXT("local_ddc_only"), false);
	Streaming->SetNumberField(TEXT("pool_size_factor"), 4.0);
	Streaming->SetNumberField(TEXT("prefetch_frames"), 5.0);
	Streaming->SetNumberField(
		TEXT("prefetch_percentage_step"),
		15.0);
	Streaming->SetNumberField(
		TEXT("prefetch_percentage_bias"),
		25.0);
	const TSharedPtr<FJsonObject> SparsePayload =
		BuildUpdatePayload(
			SparseRef,
			BuildAssetRevision(Sparse),
			{
				BuildTypedChange(
					TEXT("addressing"),
					TEXT("set"),
					TEXT(
						"uai.asset.sparse_volume_texture."
						"addressing.v1"),
					MakeShared<FJsonValueObject>(Addressing)),
				BuildTypedChange(
					TEXT("streaming"),
					TEXT("set"),
					TEXT(
						"uai.asset.sparse_volume_texture."
						"streaming.v1"),
					MakeShared<FJsonValueObject>(Streaming)),
			});
	if (!SubmitComplete(
			*SparseProfile,
			SparsePayload,
			TEXT("sa008-asset-data-sparse")))
	{
		return false;
	}
	TestTrue(
		TEXT("sparse addressing is independently read back"),
		Sparse->AddressX == TA_Clamp
			&& Sparse->AddressY == TA_Mirror
			&& Sparse->AddressZ == TA_Wrap);
	TestTrue(
		TEXT("sparse streaming state is independently read back"),
		!Sparse->bLocalDDCOnly
			&& FMath::IsNearlyEqual(
				Sparse->StreamingPoolSizeFactor,
				4.0f)
			&& Sparse->NumberOfPrefetchFrames == 5
			&& FMath::IsNearlyEqual(
				Sparse->PrefetchPercentageStepSize,
				15.0f)
			&& FMath::IsNearlyEqual(
				Sparse->PrefetchPercentageBias,
				25.0f));
	const TSharedPtr<FJsonObject> SparseClearPayload =
		BuildUpdatePayload(
			SparseRef,
			BuildAssetRevision(Sparse),
			{
				BuildTypedChange(
					TEXT("addressing"),
					TEXT("clear"),
					TEXT(
						"uai.asset.sparse_volume_texture."
						"addressing.v1"),
					nullptr),
				BuildTypedChange(
					TEXT("streaming"),
					TEXT("clear"),
					TEXT(
						"uai.asset.sparse_volume_texture."
						"streaming.v1"),
					nullptr),
			});
	if (!SubmitComplete(
			*SparseProfile,
			SparseClearPayload,
			TEXT("sa008-asset-data-sparse-clear")))
	{
		return false;
	}
	const UStreamableSparseVolumeTexture* DefaultSparse =
		GetDefault<UStreamableSparseVolumeTexture>();
	TestTrue(
		TEXT("cleared sparse addressing uses native defaults"),
		Sparse->AddressX == DefaultSparse->AddressX
			&& Sparse->AddressY == DefaultSparse->AddressY
			&& Sparse->AddressZ == DefaultSparse->AddressZ);
	TestTrue(
		TEXT("cleared sparse streaming uses native defaults"),
		Sparse->bLocalDDCOnly == DefaultSparse->bLocalDDCOnly
			&& FMath::IsNearlyEqual(
				Sparse->StreamingPoolSizeFactor,
				DefaultSparse->StreamingPoolSizeFactor)
			&& Sparse->NumberOfPrefetchFrames
				== DefaultSparse->NumberOfPrefetchFrames
			&& FMath::IsNearlyEqual(
				Sparse->PrefetchPercentageStepSize,
				DefaultSparse->PrefetchPercentageStepSize)
			&& FMath::IsNearlyEqual(
				Sparse->PrefetchPercentageBias,
				DefaultSparse->PrefetchPercentageBias));
	TestEqual(
		TEXT("five updates prepare durable work exactly once each"),
		Durable->PrepareCount,
		5);
	TestEqual(
		TEXT("five updates persist durable work exactly once each"),
		Durable->PersistCount,
		5);
	TestEqual(
		TEXT("five updates acknowledge only after C9"),
		Durable->AcknowledgeCount,
		5);

	const FString VerifiedSparseFingerprint =
		ReadSparseVolumeTextureFingerprint(*Sparse);
	SparsePayload->SetStringField(
		TEXT("expected_revision"),
		TEXT("asset-crc32:00000000"));
	const FUeAgentSimpleAuthoringCommandRoute* SparseRoute =
		FindRoute(SparseProfile->CapabilityId);
	TSharedPtr<FUeAgentSimpleAuthoringApplication>
		SparseApplication;
	if (!SparseRoute
		|| !TryCreateAssetDataApplication(
			*this,
			Fixture,
			Owner,
			Durable,
			*SparseRoute,
			TEXT("sa008-asset-data-sparse-stale"),
			SparseApplication))
	{
		return false;
	}
	FString SparsePayloadJson;
	Serialize(SparsePayload, SparsePayloadJson);
	const FUeAgentOperationResult StaleResult =
		SparseApplication->Submit(
			FUeAgentOperationRequest(
				SparseRoute->DescriptorIdentity,
				TEXT("request:sa008-asset-data-sparse-stale"),
				TEXT("idempotency:sa008-asset-data-sparse-stale"),
				FUeAgentContractValue(SparsePayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-asset-data-sparse-stale"),
				{TEXT("uai:asset-data:write")},
				TEXT("correlation:sa008-asset-data-sparse-stale")),
			Cancellation);
	TestFalse(
		TEXT("stale core configuration revision fails"),
		IsComplete(StaleResult));
	TestEqual(
		TEXT("stale core configuration causes zero mutation"),
		ReadSparseVolumeTextureFingerprint(*Sparse),
		VerifiedSparseFingerprint);
	TestEqual(
		TEXT("stale core configuration prepares no durable work"),
		Durable->PrepareCount,
		5);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataAudioSettingsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataAudioSettingsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataAudioSettingsProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TestTrue(
			TEXT("formal AssetData production catalog composes"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* EqProfile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.audio_effect_preset.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		AttenuationProfile =
			FindProductionProfile(
				Profiles,
				TEXT(
					"uai.operation.asset.sound_attenuation.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		ConcurrencyProfile =
			FindProductionProfile(
				Profiles,
				TEXT(
					"uai.operation.asset.sound_concurrency.update"));
	if (!TestNotNull(TEXT("EQ update profile exists"), EqProfile)
		|| !TestNotNull(
			TEXT("attenuation update profile exists"),
			AttenuationProfile)
		|| !TestNotNull(
			TEXT("concurrency update profile exists"),
			ConcurrencyProfile))
	{
		return false;
	}
	TestTrue(
		TEXT("audio settings field contracts remain exact"),
		EqProfile->FieldIds
				== TArray<FString>{TEXT("settings")}
			&& AttenuationProfile->FieldIds
				== TArray<FString>{TEXT("attenuation")}
			&& ConcurrencyProfile->FieldIds
				== TArray<FString>{TEXT("concurrency")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString EqRef =
		TEXT("uai://asset/sa008-audio-eq-target");
	const FString AttenuationRef =
		TEXT("uai://asset/sa008-audio-attenuation-target");
	const FString ConcurrencyRef =
		TEXT("uai://asset/sa008-audio-concurrency-target");
	Owner.BindRef(EqRef, TEXT("SA008_AudioEqTarget"));
	Owner.BindRef(
		AttenuationRef,
		TEXT("SA008_AudioAttenuationTarget"));
	Owner.BindRef(
		ConcurrencyRef,
		TEXT("SA008_AudioConcurrencyTarget"));
	USubmixEffectSubmixEQPreset* Eq =
		NewObject<USubmixEffectSubmixEQPreset>(
			Owner.Package,
			TEXT("SA008_AudioEqTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	USoundAttenuation* Attenuation =
		NewObject<USoundAttenuation>(
			Owner.Package,
			TEXT("SA008_AudioAttenuationTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	USoundConcurrency* Concurrency =
		NewObject<USoundConcurrency>(
			Owner.Package,
			TEXT("SA008_AudioConcurrencyTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Eq || !Attenuation || !Concurrency)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Band = MakeShared<FJsonObject>();
	Band->SetNumberField(TEXT("frequency_hz"), 1200.0);
	Band->SetNumberField(TEXT("bandwidth_octaves"), 0.75);
	Band->SetNumberField(TEXT("gain_db"), 4.5);
	Band->SetBoolField(TEXT("enabled"), true);
	const TSharedPtr<FJsonObject> EqSettings =
		MakeShared<FJsonObject>();
	EqSettings->SetField(
		TEXT("eq_bands"),
		BuildObjectArrayValue({Band}));
	const TSharedPtr<FJsonObject> AttenuationSettings =
		MakeShared<FJsonObject>();
	AttenuationSettings->SetBoolField(TEXT("attenuate"), true);
	AttenuationSettings->SetBoolField(TEXT("spatialize"), true);
	AttenuationSettings->SetStringField(
		TEXT("distance_algorithm"),
		TEXT("logarithmic"));
	AttenuationSettings->SetStringField(
		TEXT("attenuation_shape"),
		TEXT("sphere"));
	AttenuationSettings->SetNumberField(
		TEXT("inner_radius"),
		350.0);
	AttenuationSettings->SetNumberField(
		TEXT("falloff_distance"),
		1850.0);
	AttenuationSettings->SetStringField(
		TEXT("spatialization_algorithm"),
		TEXT("binaural"));
	AttenuationSettings->SetBoolField(
		TEXT("enable_occlusion"),
		true);
	AttenuationSettings->SetNumberField(
		TEXT("occlusion_low_pass_filter_frequency"),
		900.0);
	AttenuationSettings->SetNumberField(
		TEXT("occlusion_volume_attenuation"),
		0.35);
	AttenuationSettings->SetNumberField(
		TEXT("occlusion_interpolation_time"),
		0.4);
	const TSharedPtr<FJsonObject> ConcurrencySettings =
		MakeShared<FJsonObject>();
	ConcurrencySettings->SetNumberField(TEXT("max_count"), 7.0);
	ConcurrencySettings->SetBoolField(
		TEXT("limit_to_owner"),
		true);
	ConcurrencySettings->SetNumberField(
		TEXT("retrigger_time"),
		0.3);
	ConcurrencySettings->SetStringField(
		TEXT("resolution_rule"),
		TEXT("stop_lowest_priority"));
	ConcurrencySettings->SetNumberField(
		TEXT("volume_scale"),
		0.65);
	ConcurrencySettings->SetStringField(
		TEXT("volume_scale_mode"),
		TEXT("priority"));
	ConcurrencySettings->SetNumberField(
		TEXT("volume_scale_attack_time"),
		0.08);
	ConcurrencySettings->SetNumberField(
		TEXT("volume_scale_release_time"),
		0.45);
	ConcurrencySettings->SetNumberField(
		TEXT("voice_steal_release_time"),
		0.12);

	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*EqProfile,
			BuildUpdatePayload(
				EqRef,
				BuildAssetRevision(Eq),
				{
					BuildTypedChange(
						TEXT("settings"),
						TEXT("set"),
						TEXT(
							"uai.asset.audio_effect_preset."
							"settings.v1"),
						MakeShared<FJsonValueObject>(
							EqSettings)),
				}),
			TEXT("sa008-audio-settings-eq-set"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*AttenuationProfile,
			BuildUpdatePayload(
				AttenuationRef,
				BuildAssetRevision(Attenuation),
				{
					BuildTypedChange(
						TEXT("attenuation"),
						TEXT("set"),
						TEXT(
							"uai.asset.sound_attenuation."
							"settings.v1"),
						MakeShared<FJsonValueObject>(
							AttenuationSettings)),
				}),
			TEXT("sa008-audio-settings-attenuation-set"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*ConcurrencyProfile,
			BuildUpdatePayload(
				ConcurrencyRef,
				BuildAssetRevision(Concurrency),
				{
					BuildTypedChange(
						TEXT("concurrency"),
						TEXT("set"),
						TEXT(
							"uai.asset.sound_concurrency."
							"settings.v1"),
						MakeShared<FJsonValueObject>(
							ConcurrencySettings)),
				}),
			TEXT("sa008-audio-settings-concurrency-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("EQ settings are independently read back"),
		Eq->Settings.EQBands.Num() == 1
			&& FMath::IsNearlyEqual(
				Eq->Settings.EQBands[0].Frequency,
				1200.0f)
			&& FMath::IsNearlyEqual(
				Eq->Settings.EQBands[0].GainDb,
				4.5f)
			&& Eq->Settings.EQBands[0].bEnabled);
	TestTrue(
		TEXT("attenuation settings are independently read back"),
		Attenuation->Attenuation.bAttenuate
			&& Attenuation->Attenuation.bSpatialize
			&& Attenuation->Attenuation.DistanceAlgorithm
				== EAttenuationDistanceModel::Logarithmic
			&& Attenuation->Attenuation.SpatializationAlgorithm
				== SPATIALIZATION_HRTF
			&& FMath::IsNearlyEqual(
				Attenuation->Attenuation.
					AttenuationShapeExtents.X,
				350.0f));
	TestTrue(
		TEXT("concurrency settings are independently read back"),
		Concurrency->Concurrency.MaxCount == 7
			&& Concurrency->Concurrency.bLimitToOwner
			&& Concurrency->Concurrency.ResolutionRule
				== EMaxConcurrentResolutionRule::
					StopLowestPriority
			&& Concurrency->Concurrency.VolumeScaleMode
				== EConcurrencyVolumeScaleMode::Priority);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*EqProfile,
			BuildUpdatePayload(
				EqRef,
				BuildAssetRevision(Eq),
				{
					BuildTypedChange(
						TEXT("settings"),
						TEXT("clear"),
						TEXT(
							"uai.asset.audio_effect_preset."
							"settings.v1"),
						nullptr),
				}),
			TEXT("sa008-audio-settings-eq-clear"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*AttenuationProfile,
			BuildUpdatePayload(
				AttenuationRef,
				BuildAssetRevision(Attenuation),
				{
					BuildTypedChange(
						TEXT("attenuation"),
						TEXT("clear"),
						TEXT(
							"uai.asset.sound_attenuation."
							"settings.v1"),
						nullptr),
				}),
			TEXT("sa008-audio-settings-attenuation-clear"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*ConcurrencyProfile,
			BuildUpdatePayload(
				ConcurrencyRef,
				BuildAssetRevision(Concurrency),
				{
					BuildTypedChange(
						TEXT("concurrency"),
						TEXT("clear"),
						TEXT(
							"uai.asset.sound_concurrency."
							"settings.v1"),
						nullptr),
				}),
			TEXT("sa008-audio-settings-concurrency-clear")))
	{
		return false;
	}
	TestEqual(
		TEXT("EQ clear uses class default band count"),
		Eq->Settings.EQBands.Num(),
		GetDefault<USubmixEffectSubmixEQPreset>()->
			Settings.EQBands.Num());
	TestEqual(
		TEXT("attenuation clear uses class default distance"),
		Attenuation->Attenuation.FalloffDistance,
		GetDefault<USoundAttenuation>()->
			Attenuation.FalloffDistance);
	TestEqual(
		TEXT("concurrency clear uses class default max count"),
		Concurrency->Concurrency.MaxCount,
		GetDefault<USoundConcurrency>()->
			Concurrency.MaxCount);
	TestEqual(
		TEXT("six audio settings updates persist once each"),
		Durable->PersistCount,
		6);
	TestEqual(
		TEXT("six audio settings updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		6);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataAudioRelationsProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataAudioRelationsProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataAudioRelationsProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		SoundClassProfile =
			FindProductionProfile(
				Profiles,
				TEXT("uai.operation.asset.sound_class.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		SoundMixProfile =
			FindProductionProfile(
				Profiles,
				TEXT("uai.operation.asset.sound_mix.update"));
	const FUeAgentAssetDataSimpleEffectProductionProfile*
		SoundSubmixProfile =
			FindProductionProfile(
				Profiles,
				TEXT("uai.operation.asset.sound_submix.update"));
	if (!SoundClassProfile || !SoundMixProfile || !SoundSubmixProfile)
	{
		return false;
	}
	TestTrue(
		TEXT("audio relation field contracts remain exact"),
		SoundClassProfile->FieldIds
				== TArray<FString>{
					TEXT("properties"),
					TEXT("child_classes"),
				}
			&& SoundMixProfile->FieldIds
				== TArray<FString>{
					TEXT("modifiers"),
					TEXT("initial_delay"),
				}
			&& SoundSubmixProfile->FieldIds
				== TArray<FString>{
					TEXT("parent"),
					TEXT("effect_chain"),
				});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString SoundClassRef =
		TEXT("uai://asset/sa008-sound-class-target");
	const FString ChildARef =
		TEXT("uai://asset/sa008-sound-class-child-a");
	const FString ChildBRef =
		TEXT("uai://asset/sa008-sound-class-child-b");
	const FString SoundMixRef =
		TEXT("uai://asset/sa008-sound-mix-target");
	const FString MixClassARef =
		TEXT("uai://asset/sa008-sound-mix-class-a");
	const FString MixClassBRef =
		TEXT("uai://asset/sa008-sound-mix-class-b");
	const FString SoundSubmixRef =
		TEXT("uai://asset/sa008-sound-submix-target");
	const FString ParentARef =
		TEXT("uai://asset/sa008-sound-submix-parent-a");
	const FString ParentBRef =
		TEXT("uai://asset/sa008-sound-submix-parent-b");
	const FString EffectARef =
		TEXT("uai://asset/sa008-sound-submix-effect-a");
	const FString EffectBRef =
		TEXT("uai://asset/sa008-sound-submix-effect-b");
	const TArray<TPair<FString, FName>> Bindings{
		{SoundClassRef, TEXT("SA008_SoundClassTarget")},
		{ChildARef, TEXT("SA008_SoundClassChildA")},
		{ChildBRef, TEXT("SA008_SoundClassChildB")},
		{SoundMixRef, TEXT("SA008_SoundMixTarget")},
		{MixClassARef, TEXT("SA008_SoundMixClassA")},
		{MixClassBRef, TEXT("SA008_SoundMixClassB")},
		{SoundSubmixRef, TEXT("SA008_SoundSubmixTarget")},
		{ParentARef, TEXT("SA008_SoundSubmixParentA")},
		{ParentBRef, TEXT("SA008_SoundSubmixParentB")},
		{EffectARef, TEXT("SA008_SoundSubmixEffectA")},
		{EffectBRef, TEXT("SA008_SoundSubmixEffectB")},
	};
	for (const TPair<FString, FName>& Binding : Bindings)
	{
		Owner.BindRef(Binding.Key, Binding.Value);
	}
	const auto NewOwned =
		[&Owner](UClass* Class, const FName Name)
		{
			return NewObject<UObject>(
				Owner.Package,
				Class,
				Name,
				RF_Public | RF_Standalone | RF_Transactional);
		};
	USoundClass* SoundClass =
		Cast<USoundClass>(NewOwned(
			USoundClass::StaticClass(),
			TEXT("SA008_SoundClassTarget")));
	USoundClass* ChildA =
		Cast<USoundClass>(NewOwned(
			USoundClass::StaticClass(),
			TEXT("SA008_SoundClassChildA")));
	USoundClass* ChildB =
		Cast<USoundClass>(NewOwned(
			USoundClass::StaticClass(),
			TEXT("SA008_SoundClassChildB")));
	USoundMix* SoundMix =
		Cast<USoundMix>(NewOwned(
			USoundMix::StaticClass(),
			TEXT("SA008_SoundMixTarget")));
	USoundClass* MixClassA =
		Cast<USoundClass>(NewOwned(
			USoundClass::StaticClass(),
			TEXT("SA008_SoundMixClassA")));
	USoundClass* MixClassB =
		Cast<USoundClass>(NewOwned(
			USoundClass::StaticClass(),
			TEXT("SA008_SoundMixClassB")));
	USoundSubmix* SoundSubmix =
		Cast<USoundSubmix>(NewOwned(
			USoundSubmix::StaticClass(),
			TEXT("SA008_SoundSubmixTarget")));
	USoundSubmix* ParentA =
		Cast<USoundSubmix>(NewOwned(
			USoundSubmix::StaticClass(),
			TEXT("SA008_SoundSubmixParentA")));
	USoundSubmix* ParentB =
		Cast<USoundSubmix>(NewOwned(
			USoundSubmix::StaticClass(),
			TEXT("SA008_SoundSubmixParentB")));
	USubmixEffectSubmixEQPreset* EffectA =
		Cast<USubmixEffectSubmixEQPreset>(NewOwned(
			USubmixEffectSubmixEQPreset::StaticClass(),
			TEXT("SA008_SoundSubmixEffectA")));
	USubmixEffectSubmixEQPreset* EffectB =
		Cast<USubmixEffectSubmixEQPreset>(NewOwned(
			USubmixEffectSubmixEQPreset::StaticClass(),
			TEXT("SA008_SoundSubmixEffectB")));
	if (!SoundClass || !ChildA || !ChildB || !SoundMix
		|| !MixClassA || !MixClassB || !SoundSubmix
		|| !ParentA || !ParentB || !EffectA || !EffectB)
	{
		return false;
	}
	SoundClass->ChildClasses.Add(ChildA);
	ChildA->ParentClass = SoundClass;
	FSoundClassAdjuster ExistingModifier;
	ExistingModifier.SoundClassObject = MixClassA;
	ExistingModifier.VolumeAdjuster = 0.4f;
	SoundMix->SoundClassEffects.Add(ExistingModifier);
	SoundMix->InitialDelay = 2.0f;
	SoundSubmix->SetParentSubmix(ParentA, true);
	SoundSubmix->SubmixEffectChain.Add(EffectA);

	const TSharedPtr<FJsonObject> Properties =
		MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("volume"), 0.7);
	Properties->SetNumberField(TEXT("pitch"), 1.2);
	Properties->SetBoolField(TEXT("is_ui_sound"), true);
	Properties->SetBoolField(TEXT("is_music"), false);
	const TSharedPtr<FJsonObject> NewModifier =
		BuildSoundMixModifier(MixClassBRef, 0.8, 1.1);
	const TSharedPtr<FJsonObject> RemoveModifier =
		BuildSoundMixModifier(MixClassARef, 1.0, 1.0);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundClassProfile,
			BuildUpdatePayload(
				SoundClassRef,
				BuildAssetRevision(SoundClass),
				{
					BuildTypedChange(
						TEXT("properties"),
						TEXT("set"),
						TEXT(
							"uai.asset.sound_class."
							"properties.v1"),
						MakeShared<FJsonValueObject>(
							Properties)),
					BuildTypedChange(
						TEXT("child_classes"),
						TEXT("set"),
						TEXT(
							"uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({ChildBRef})),
				}),
			TEXT("sa008-audio-relations-class-set"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundMixProfile,
			BuildUpdatePayload(
				SoundMixRef,
				BuildAssetRevision(SoundMix),
				{
					BuildTypedChange(
						TEXT("modifiers"),
						TEXT("set"),
						TEXT(
							"uai.asset.sound_mix.modifiers.v1"),
						BuildObjectArrayValue(
							{NewModifier})),
					BuildTypedChange(
						TEXT("initial_delay"),
						TEXT("set"),
						TEXT("uai.value.decimal.v1"),
						MakeShared<FJsonValueNumber>(0.75)),
				}),
			TEXT("sa008-audio-relations-mix-set"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundSubmixProfile,
			BuildUpdatePayload(
				SoundSubmixRef,
				BuildAssetRevision(SoundSubmix),
				{
					BuildTypedChange(
						TEXT("parent"),
						TEXT("set"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(
							ParentBRef)),
					BuildTypedChange(
						TEXT("effect_chain"),
						TEXT("set"),
						TEXT(
							"uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({EffectBRef})),
				}),
			TEXT("sa008-audio-relations-submix-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("sound class set preserves and appends ordered children"),
		SoundClass->ChildClasses
			== TArray<TObjectPtr<USoundClass>>{ChildA, ChildB}
			&& ChildA->ParentClass == SoundClass
			&& ChildB->ParentClass == SoundClass
			&& FMath::IsNearlyEqual(
				SoundClass->Properties.Volume,
				0.7f));
	TestTrue(
		TEXT("sound mix set merges by sound-class identity"),
		SoundMix->SoundClassEffects.Num() == 2
			&& SoundMix->SoundClassEffects[0].SoundClassObject
				== MixClassA
			&& SoundMix->SoundClassEffects[1].SoundClassObject
				== MixClassB
			&& FMath::IsNearlyEqual(
				SoundMix->InitialDelay,
				0.75f));
	TestTrue(
		TEXT("submix set updates parent and appends effects"),
		SoundSubmix->ParentSubmix == ParentB
			&& SoundSubmix->SubmixEffectChain
				== TArray<TObjectPtr<USoundEffectSubmixPreset>>{
					EffectA,
					EffectB,
				});

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundClassProfile,
			BuildUpdatePayload(
				SoundClassRef,
				BuildAssetRevision(SoundClass),
				{
					BuildTypedChange(
						TEXT("child_classes"),
						TEXT("remove"),
						TEXT(
							"uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({ChildARef})),
				}),
			TEXT("sa008-audio-relations-class-remove"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundMixProfile,
			BuildUpdatePayload(
				SoundMixRef,
				BuildAssetRevision(SoundMix),
				{
					BuildTypedChange(
						TEXT("modifiers"),
						TEXT("remove"),
						TEXT(
							"uai.asset.sound_mix.modifiers.v1"),
						BuildObjectArrayValue(
							{RemoveModifier})),
				}),
			TEXT("sa008-audio-relations-mix-remove"))
		|| !SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*SoundSubmixProfile,
			BuildUpdatePayload(
				SoundSubmixRef,
				BuildAssetRevision(SoundSubmix),
				{
					BuildTypedChange(
						TEXT("parent"),
						TEXT("null"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("effect_chain"),
						TEXT("remove"),
						TEXT(
							"uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({EffectARef})),
				}),
			TEXT("sa008-audio-relations-submix-remove")))
	{
		return false;
	}
	TestTrue(
		TEXT("sound class remove detaches exact child"),
		SoundClass->ChildClasses
				== TArray<TObjectPtr<USoundClass>>{ChildB}
			&& ChildA->ParentClass == nullptr);
	TestTrue(
		TEXT("sound mix remove matches only sound-class identity"),
		SoundMix->SoundClassEffects.Num() == 1
			&& SoundMix->SoundClassEffects[0].SoundClassObject
				== MixClassB);
	TestTrue(
		TEXT("submix null and remove preserve unselected effect"),
		SoundSubmix->ParentSubmix == nullptr
			&& SoundSubmix->SubmixEffectChain
				== TArray<TObjectPtr<USoundEffectSubmixPreset>>{
					EffectB,
				});
	TestEqual(
		TEXT("six relation updates persist once each"),
		Durable->PersistCount,
		6);
	TestEqual(
		TEXT("six relation updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		6);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataCurveProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataCurveProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataCurveProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* CurveProfile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.curve.update"));
	if (!TestNotNull(TEXT("curve update profile exists"), CurveProfile))
	{
		return false;
	}
	TestTrue(
		TEXT("curve replacement field contract remains exact"),
		CurveProfile->FieldIds
				== TArray<FString>{TEXT("keys")}
			&& CurveProfile->Fields.Num() == 1
			&& CurveProfile->Fields[0].IsValid()
			&& CurveProfile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString FloatRef =
		TEXT("uai://asset/sa008-curve-float-target");
	const FString VectorRef =
		TEXT("uai://asset/sa008-curve-vector-target");
	const FString ColorRef =
		TEXT("uai://asset/sa008-curve-color-target");
	Owner.BindRef(FloatRef, TEXT("SA008_CurveFloatTarget"));
	Owner.BindRef(VectorRef, TEXT("SA008_CurveVectorTarget"));
	Owner.BindRef(ColorRef, TEXT("SA008_CurveColorTarget"));
	UCurveFloat* FloatCurve =
		NewObject<UCurveFloat>(
			Owner.Package,
			TEXT("SA008_CurveFloatTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	UCurveVector* VectorCurve =
		NewObject<UCurveVector>(
			Owner.Package,
			TEXT("SA008_CurveVectorTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	UCurveLinearColor* ColorCurve =
		NewObject<UCurveLinearColor>(
			Owner.Package,
			TEXT("SA008_CurveColorTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!FloatCurve || !VectorCurve || !ColorCurve)
	{
		return false;
	}
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
	const auto SubmitCurve =
		[this,
			&Fixture,
			&Owner,
			&Durable,
			CurveProfile](
				UObject* Curve,
				const FString& Ref,
				const int32 Dimension,
				const FString& Scope)
		{
			return SubmitVerifiedAssetDataUpdate(
				*this,
				Fixture,
				Owner,
				Durable,
				*CurveProfile,
				BuildUpdatePayload(
					Ref,
					BuildAssetRevision(Curve),
					{
						BuildTypedChange(
							TEXT("keys"),
							TEXT("replace"),
							TEXT("uai.asset.curve.keys.v1"),
							BuildCurveKeysValue(Dimension)),
					}),
				Scope);
		};
	if (!SubmitCurve(
			FloatCurve,
			FloatRef,
			1,
			TEXT("sa008-curve-float-replace"))
		|| !SubmitCurve(
			VectorCurve,
			VectorRef,
			3,
			TEXT("sa008-curve-vector-replace"))
		|| !SubmitCurve(
			ColorCurve,
			ColorRef,
			4,
			TEXT("sa008-curve-color-replace")))
	{
		return false;
	}
	const TArray<FRichCurveKey>& FloatKeys =
		FloatCurve->FloatCurve.GetConstRefOfKeys();
	TestTrue(
		TEXT("float curve rich keys are independently read back"),
		FloatKeys.Num() == 2
			&& FMath::IsNearlyEqual(FloatKeys[0].Time, 0.0f)
			&& FMath::IsNearlyEqual(FloatKeys[0].Value, 0.25f)
			&& FloatKeys[0].InterpMode == RCIM_Cubic
			&& FloatKeys[0].TangentMode == RCTM_User
			&& FloatKeys[0].TangentWeightMode
				== RCTWM_WeightedBoth
			&& FMath::IsNearlyEqual(
				FloatKeys[0].ArriveTangent,
				0.1f)
			&& FMath::IsNearlyEqual(
				FloatKeys[0].LeaveTangentWeight,
				0.4f));
	bool bVectorChannelsExact = true;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const TArray<FRichCurveKey>& Keys =
			VectorCurve->FloatCurves[Index].GetConstRefOfKeys();
		bVectorChannelsExact =
			bVectorChannelsExact
			&& Keys.Num() == 2
			&& FMath::IsNearlyEqual(
				Keys[0].Value,
				0.25f + Index * 0.25f)
			&& FMath::IsNearlyEqual(
				Keys[0].LeaveTangent,
				0.2f + Index * 0.05f);
	}
	TestTrue(
		TEXT("vector curve preserves three rich-key channels"),
		bVectorChannelsExact);
	bool bColorChannelsExact = true;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const TArray<FRichCurveKey>& Keys =
			ColorCurve->FloatCurves[Index].GetConstRefOfKeys();
		bColorChannelsExact =
			bColorChannelsExact
			&& Keys.Num() == 2
			&& FMath::IsNearlyEqual(
				Keys[1].Value,
				1.0f + Index * 0.25f);
	}
	TestTrue(
		TEXT("linear-color curve preserves four rich-key channels"),
		bColorChannelsExact);
	TestEqual(
		TEXT("three curve replacements persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three curve replacements acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataEnhancedInputActionProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataEnhancedInputActionProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataEnhancedInputActionProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.input_action.update"));
	if (!TestNotNull(TEXT("input action update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("input action field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("value_type"),
					TEXT("consume_input"),
					TEXT("triggers"),
					TEXT("modifiers"),
				}
			&& Profile->Fields.Num() == 4
			&& Profile->Fields[0].IsValid()
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("set")}
			&& Profile->Fields[1].IsValid()
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set")}
			&& Profile->Fields[2].IsValid()
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("replace"),
				}
			&& Profile->Fields[3].IsValid()
			&& Profile->Fields[3].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("replace"),
				});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString ActionRef =
		TEXT("uai://asset/sa008-input-action-target");
	Owner.BindRef(ActionRef, TEXT("SA008_InputActionTarget"));
	UInputAction* Action =
		NewObject<UInputAction>(
			Owner.Package,
			TEXT("SA008_InputActionTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Action)
	{
		return false;
	}
	Action->ValueType = EInputActionValueType::Boolean;
	Action->bConsumeInput = true;
	UInputTriggerHold* InitialTrigger =
		NewObject<UInputTriggerHold>(
			Action,
			UInputTriggerHold::StaticClass(),
			NAME_None,
			RF_Transactional);
	UInputModifierSwizzleAxis* InitialModifier =
		NewObject<UInputModifierSwizzleAxis>(
			Action,
			UInputModifierSwizzleAxis::StaticClass(),
			NAME_None,
			RF_Transactional);
	Action->Triggers.Add(InitialTrigger);
	Action->Modifiers.Add(InitialModifier);

	const FString HoldClass =
		UInputTriggerHold::StaticClass()->GetPathName();
	const FString ReleasedClass =
		UInputTriggerReleased::StaticClass()->GetPathName();
	const FString ScalarClass =
		UInputModifierScalar::StaticClass()->GetPathName();
	const FString SwizzleClass =
		UInputModifierSwizzleAxis::StaticClass()->GetPathName();
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ActionRef,
				BuildAssetRevision(Action),
				{
					BuildTypedChange(
						TEXT("value_type"),
						TEXT("set"),
						TEXT(
							"uai.asset.input_action."
							"value_type.v1"),
						MakeShared<FJsonValueString>(
							TEXT("axis2d"))),
					BuildTypedChange(
						TEXT("consume_input"),
						TEXT("set"),
						TEXT("uai.value.boolean.v1"),
						MakeShared<FJsonValueBoolean>(false)),
					BuildTypedChange(
						TEXT("triggers"),
						TEXT("set"),
						TEXT(
							"uai.asset.input_action."
							"triggers.v1"),
						BuildStringArrayValue(
							{ReleasedClass})),
					BuildTypedChange(
						TEXT("modifiers"),
						TEXT("set"),
						TEXT(
							"uai.asset.input_action."
							"modifiers.v1"),
						BuildStringArrayValue(
							{ScalarClass})),
				}),
			TEXT("sa008-input-action-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("input action set preserves existing instances and appends"),
		Action->ValueType == EInputActionValueType::Axis2D
			&& !Action->bConsumeInput
			&& Action->Triggers.Num() == 2
			&& Action->Triggers[0] == InitialTrigger
			&& Action->Triggers[1]
				&& Action->Triggers[1]->GetClass()
					== UInputTriggerReleased::StaticClass()
			&& Action->Modifiers.Num() == 2
			&& Action->Modifiers[0] == InitialModifier
			&& Action->Modifiers[1]
				&& Action->Modifiers[1]->GetClass()
					== UInputModifierScalar::StaticClass());

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ActionRef,
				BuildAssetRevision(Action),
				{
					BuildTypedChange(
						TEXT("triggers"),
						TEXT("replace"),
						TEXT(
							"uai.asset.input_action."
							"triggers.v1"),
						BuildStringArrayValue(
							{ReleasedClass, HoldClass})),
					BuildTypedChange(
						TEXT("modifiers"),
						TEXT("replace"),
						TEXT(
							"uai.asset.input_action."
							"modifiers.v1"),
						BuildStringArrayValue(
							{ScalarClass, SwizzleClass})),
				}),
			TEXT("sa008-input-action-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("input action replace uses requested class order"),
		Action->ValueType == EInputActionValueType::Axis2D
			&& !Action->bConsumeInput
			&& Action->Triggers.Num() == 2
			&& Action->Triggers[0]
			&& Action->Triggers[0]->GetClass()
				== UInputTriggerReleased::StaticClass()
			&& Action->Triggers[1]
			&& Action->Triggers[1]->GetClass()
				== UInputTriggerHold::StaticClass()
			&& Action->Triggers[1] != InitialTrigger
			&& Action->Modifiers.Num() == 2
			&& Action->Modifiers[0]
			&& Action->Modifiers[0]->GetClass()
				== UInputModifierScalar::StaticClass()
			&& Action->Modifiers[1]
			&& Action->Modifiers[1]->GetClass()
				== UInputModifierSwizzleAxis::StaticClass()
			&& Action->Modifiers[1] != InitialModifier);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ActionRef,
				BuildAssetRevision(Action),
				{
					BuildTypedChange(
						TEXT("triggers"),
						TEXT("clear"),
						TEXT(
							"uai.asset.input_action."
							"triggers.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("modifiers"),
						TEXT("clear"),
						TEXT(
							"uai.asset.input_action."
							"modifiers.v1"),
						nullptr),
				}),
			TEXT("sa008-input-action-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("input action clear empties collections and preserves scalars"),
		Action->ValueType == EInputActionValueType::Axis2D
			&& !Action->bConsumeInput
			&& Action->Triggers.IsEmpty()
			&& Action->Modifiers.IsEmpty());
	TestEqual(
		TEXT("three input action updates persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three input action updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataEnhancedInputMappingProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataEnhancedInputMappingProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataEnhancedInputMappingProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT(
				"uai.operation.asset.input_mapping_context.update"));
	if (!TestNotNull(
			TEXT("input mapping context update profile exists"),
			Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("input mapping context field contract remains exact"),
		Profile->FieldIds == TArray<FString>{TEXT("mappings")}
			&& Profile->Fields.Num() == 1
			&& Profile->Fields[0].IsValid()
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("remove"),
					TEXT("replace"),
				});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString ContextRef =
		TEXT("uai://asset/sa008-input-mapping-target");
	const FString ActionARef =
		TEXT("uai://asset/sa008-input-mapping-action-a");
	const FString ActionBRef =
		TEXT("uai://asset/sa008-input-mapping-action-b");
	const FString ActionCRef =
		TEXT("uai://asset/sa008-input-mapping-action-c");
	Owner.BindRef(ContextRef, TEXT("SA008_InputMappingTarget"));
	Owner.BindRef(ActionARef, TEXT("SA008_InputMappingActionA"));
	Owner.BindRef(ActionBRef, TEXT("SA008_InputMappingActionB"));
	Owner.BindRef(ActionCRef, TEXT("SA008_InputMappingActionC"));
	UInputMappingContext* Context =
		NewObject<UInputMappingContext>(
			Owner.Package,
			TEXT("SA008_InputMappingTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	UInputAction* ActionA =
		NewObject<UInputAction>(
			Owner.Package,
			TEXT("SA008_InputMappingActionA"),
			RF_Public | RF_Standalone | RF_Transactional);
	UInputAction* ActionB =
		NewObject<UInputAction>(
			Owner.Package,
			TEXT("SA008_InputMappingActionB"),
			RF_Public | RF_Standalone | RF_Transactional);
	UInputAction* ActionC =
		NewObject<UInputAction>(
			Owner.Package,
			TEXT("SA008_InputMappingActionC"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Context || !ActionA || !ActionB || !ActionC)
	{
		return false;
	}

	FEnhancedActionKeyMapping& InitialA =
		Context->MapKey(ActionA, EKeys::SpaceBar);
	UInputTriggerHold* InitialATrigger =
		NewObject<UInputTriggerHold>(
			Context,
			UInputTriggerHold::StaticClass(),
			NAME_None,
			RF_Transactional);
	UInputModifierSwizzleAxis* InitialAModifier =
		NewObject<UInputModifierSwizzleAxis>(
			Context,
			UInputModifierSwizzleAxis::StaticClass(),
			NAME_None,
			RF_Transactional);
	InitialA.Triggers.Add(InitialATrigger);
	InitialA.Modifiers.Add(InitialAModifier);
	FEnhancedActionKeyMapping& InitialB =
		Context->MapKey(ActionB, EKeys::E);
	UInputTriggerReleased* InitialBTrigger =
		NewObject<UInputTriggerReleased>(
			Context,
			UInputTriggerReleased::StaticClass(),
			NAME_None,
			RF_Transactional);
	UInputModifierScalar* InitialBModifier =
		NewObject<UInputModifierScalar>(
			Context,
			UInputModifierScalar::StaticClass(),
			NAME_None,
			RF_Transactional);
	InitialB.Triggers.Add(InitialBTrigger);
	InitialB.Modifiers.Add(InitialBModifier);

	const FString HoldClass =
		UInputTriggerHold::StaticClass()->GetPathName();
	const FString ReleasedClass =
		UInputTriggerReleased::StaticClass()->GetPathName();
	const FString ScalarClass =
		UInputModifierScalar::StaticClass()->GetPathName();
	const FString SwizzleClass =
		UInputModifierSwizzleAxis::StaticClass()->GetPathName();
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ContextRef,
				BuildAssetRevision(Context),
				{
					BuildTypedChange(
						TEXT("mappings"),
						TEXT("set"),
						TEXT(
							"uai.asset.input_mapping_context."
							"mappings.v1"),
						BuildObjectArrayValue({
							BuildInputMappingValue(
								ActionARef,
								TEXT("SpaceBar"),
								{ReleasedClass},
								{ScalarClass}),
							BuildInputMappingValue(
								ActionCRef,
								TEXT("Q"),
								{HoldClass},
								{SwizzleClass}),
						})),
				}),
			TEXT("sa008-input-mapping-set")))
	{
		return false;
	}
	const TArray<FEnhancedActionKeyMapping>& AfterSet =
		Context->GetMappings();
	TestTrue(
		TEXT("mapping set upserts in place and appends in order"),
		AfterSet.Num() == 3
			&& AfterSet[0].Action == ActionA
			&& AfterSet[0].Key == EKeys::SpaceBar
			&& AfterSet[0].Triggers.Num() == 1
			&& AfterSet[0].Triggers[0]
			&& AfterSet[0].Triggers[0]->GetClass()
				== UInputTriggerReleased::StaticClass()
			&& AfterSet[0].Triggers[0] != InitialATrigger
			&& AfterSet[0].Modifiers.Num() == 1
			&& AfterSet[0].Modifiers[0]
			&& AfterSet[0].Modifiers[0]->GetClass()
				== UInputModifierScalar::StaticClass()
			&& AfterSet[0].Modifiers[0] != InitialAModifier
			&& AfterSet[1].Action == ActionB
			&& AfterSet[1].Key == EKeys::E
			&& AfterSet[1].Triggers.Num() == 1
			&& AfterSet[1].Triggers[0] == InitialBTrigger
			&& AfterSet[1].Modifiers.Num() == 1
			&& AfterSet[1].Modifiers[0] == InitialBModifier
			&& AfterSet[2].Action == ActionC
			&& AfterSet[2].Key == EKeys::Q);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ContextRef,
				BuildAssetRevision(Context),
				{
					BuildTypedChange(
						TEXT("mappings"),
						TEXT("remove"),
						TEXT(
							"uai.asset.input_mapping_context."
							"mappings.v1"),
						BuildObjectArrayValue({
							BuildInputMappingIdentityValue(
								ActionARef,
								TEXT("SpaceBar")),
						})),
				}),
			TEXT("sa008-input-mapping-remove")))
	{
		return false;
	}
	const TArray<FEnhancedActionKeyMapping>& AfterRemove =
		Context->GetMappings();
	TestTrue(
		TEXT("mapping remove accepts minimal identity and preserves order"),
		AfterRemove.Num() == 2
			&& AfterRemove[0].Action == ActionB
			&& AfterRemove[0].Key == EKeys::E
			&& AfterRemove[0].Triggers.Num() == 1
			&& AfterRemove[0].Triggers[0] == InitialBTrigger
			&& AfterRemove[0].Modifiers.Num() == 1
			&& AfterRemove[0].Modifiers[0] == InitialBModifier
			&& AfterRemove[1].Action == ActionC
			&& AfterRemove[1].Key == EKeys::Q);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ContextRef,
				BuildAssetRevision(Context),
				{
					BuildTypedChange(
						TEXT("mappings"),
						TEXT("replace"),
						TEXT(
							"uai.asset.input_mapping_context."
							"mappings.v1"),
						BuildObjectArrayValue({
							BuildInputMappingValue(
								ActionCRef,
								TEXT("Q"),
								{HoldClass},
								{SwizzleClass}),
							BuildInputMappingValue(
								ActionARef,
								TEXT("LeftShift"),
								{ReleasedClass},
								{ScalarClass}),
						})),
				}),
			TEXT("sa008-input-mapping-replace")))
	{
		return false;
	}
	const TArray<FEnhancedActionKeyMapping>& AfterReplace =
		Context->GetMappings();
	TestTrue(
		TEXT("mapping replace uses exact requested order and configuration"),
		AfterReplace.Num() == 2
			&& AfterReplace[0].Action == ActionC
			&& AfterReplace[0].Key == EKeys::Q
			&& AfterReplace[0].Triggers.Num() == 1
			&& AfterReplace[0].Triggers[0]
			&& AfterReplace[0].Triggers[0]->GetClass()
				== UInputTriggerHold::StaticClass()
			&& AfterReplace[0].Modifiers.Num() == 1
			&& AfterReplace[0].Modifiers[0]
			&& AfterReplace[0].Modifiers[0]->GetClass()
				== UInputModifierSwizzleAxis::StaticClass()
			&& AfterReplace[1].Action == ActionA
			&& AfterReplace[1].Key == EKeys::LeftShift
			&& AfterReplace[1].Triggers.Num() == 1
			&& AfterReplace[1].Triggers[0]
			&& AfterReplace[1].Triggers[0]->GetClass()
				== UInputTriggerReleased::StaticClass()
			&& AfterReplace[1].Modifiers.Num() == 1
			&& AfterReplace[1].Modifiers[0]
			&& AfterReplace[1].Modifiers[0]->GetClass()
				== UInputModifierScalar::StaticClass());

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				ContextRef,
				BuildAssetRevision(Context),
				{
					BuildTypedChange(
						TEXT("mappings"),
						TEXT("clear"),
						TEXT(
							"uai.asset.input_mapping_context."
							"mappings.v1"),
						nullptr),
				}),
			TEXT("sa008-input-mapping-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("mapping clear empties the mapping collection"),
		Context->GetMappings().IsEmpty());
	TestEqual(
		TEXT("four mapping updates persist once each"),
		Durable->PersistCount,
		4);
	TestEqual(
		TEXT("four mapping updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataMediaPlayerProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataMediaPlayerProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataMediaPlayerProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.media_player.update"));
	if (!TestNotNull(TEXT("media player update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("media player field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("play_on_open"),
					TEXT("loop"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].IsValid()
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("set")}
			&& Profile->Fields[1].IsValid()
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString PlayerRef =
		TEXT("uai://asset/sa008-media-player-target");
	Owner.BindRef(PlayerRef, TEXT("SA008_MediaPlayerTarget"));
	UMediaPlayer* Player =
		NewObject<UMediaPlayer>(
			Owner.Package,
			TEXT("SA008_MediaPlayerTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Player)
	{
		return false;
	}
	Player->PlayOnOpen = false;
	Player->SetLooping(false);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();
	const auto SubmitFlags =
		[this,
			&Fixture,
			&Owner,
			&Durable,
			Profile,
			Player,
			&PlayerRef](
				const bool bPlayOnOpen,
				const bool bLoop,
				const FString& Scope)
		{
			return SubmitVerifiedAssetDataUpdate(
				*this,
				Fixture,
				Owner,
				Durable,
				*Profile,
				BuildUpdatePayload(
					PlayerRef,
					BuildAssetRevision(Player),
					{
						BuildTypedChange(
							TEXT("play_on_open"),
							TEXT("set"),
							TEXT("uai.value.boolean.v1"),
							MakeShared<FJsonValueBoolean>(
								bPlayOnOpen)),
						BuildTypedChange(
							TEXT("loop"),
							TEXT("set"),
							TEXT("uai.value.boolean.v1"),
							MakeShared<FJsonValueBoolean>(
								bLoop)),
					}),
				Scope);
		};
	if (!SubmitFlags(
			true,
			true,
			TEXT("sa008-media-player-enable")))
	{
		return false;
	}
	TestTrue(
		TEXT("media player serialized flags are independently read back"),
		Player->PlayOnOpen
			&& ReadMediaPlayerLoopSetting(*Player));
	if (!SubmitFlags(
			false,
			false,
			TEXT("sa008-media-player-disable")))
	{
		return false;
	}
	TestTrue(
		TEXT("media player false values are not treated as omitted"),
		!Player->PlayOnOpen
			&& !ReadMediaPlayerLoopSetting(*Player));
	TestEqual(
		TEXT("two media player updates persist once each"),
		Durable->PersistCount,
		2);
	TestEqual(
		TEXT("two media player updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataMediaTextureProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataMediaTextureProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataMediaTextureProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.media_texture.update"));
	if (!TestNotNull(TEXT("media texture update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("media texture field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("media_player_ref"),
					TEXT("auto_clear"),
					TEXT("clear_color"),
				}
			&& Profile->Fields.Num() == 3
			&& Profile->Fields[0].IsValid()
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("null"),
					TEXT("clear"),
				}
			&& Profile->Fields[1].IsValid()
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[2].IsValid()
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TextureRef =
		TEXT("uai://asset/sa008-media-texture-target");
	const FString PlayerARef =
		TEXT("uai://asset/sa008-media-texture-player-a");
	const FString PlayerBRef =
		TEXT("uai://asset/sa008-media-texture-player-b");
	Owner.BindRef(TextureRef, TEXT("SA008_MediaTextureTarget"));
	Owner.BindRef(PlayerARef, TEXT("SA008_MediaTexturePlayerA"));
	Owner.BindRef(PlayerBRef, TEXT("SA008_MediaTexturePlayerB"));
	UMediaTexture* Texture =
		NewObject<UMediaTexture>(
			Owner.Package,
			TEXT("SA008_MediaTextureTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	UMediaPlayer* PlayerA =
		NewObject<UMediaPlayer>(
			Owner.Package,
			TEXT("SA008_MediaTexturePlayerA"),
			RF_Public | RF_Standalone | RF_Transactional);
	UMediaPlayer* PlayerB =
		NewObject<UMediaPlayer>(
			Owner.Package,
			TEXT("SA008_MediaTexturePlayerB"),
			RF_Public | RF_Standalone | RF_Transactional);
	const UMediaTexture* Default = GetDefault<UMediaTexture>();
	if (!Texture || !PlayerA || !PlayerB || !Default)
	{
		return false;
	}
	Texture->SetMediaPlayer(PlayerA);
	Texture->AutoClear = false;
	Texture->ClearColor = FLinearColor(0.9f, 0.8f, 0.7f, 0.6f);
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TextureRef,
				BuildAssetRevision(Texture),
				{
					BuildTypedChange(
						TEXT("media_player_ref"),
						TEXT("set"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(
							PlayerBRef)),
					BuildTypedChange(
						TEXT("auto_clear"),
						TEXT("set"),
						TEXT("uai.value.boolean.v1"),
						MakeShared<FJsonValueBoolean>(true)),
					BuildTypedChange(
						TEXT("clear_color"),
						TEXT("set"),
						TEXT("uai.value.linear_color.v1"),
						BuildLinearColorValue(
							0.1,
							0.2,
							0.3,
							0.4)),
				}),
			TEXT("sa008-media-texture-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("media texture set applies exact ref bool and color"),
		Texture->GetMediaPlayer() == PlayerB
			&& Texture->AutoClear
			&& Texture->ClearColor.Equals(
				FLinearColor(0.1f, 0.2f, 0.3f, 0.4f)));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TextureRef,
				BuildAssetRevision(Texture),
				{
					BuildTypedChange(
						TEXT("media_player_ref"),
						TEXT("null"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("auto_clear"),
						TEXT("clear"),
						TEXT("uai.value.boolean.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("clear_color"),
						TEXT("clear"),
						TEXT("uai.value.linear_color.v1"),
						nullptr),
				}),
			TEXT("sa008-media-texture-null-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("media texture null and clear use explicit defaults"),
		Texture->GetMediaPlayer() == nullptr
			&& Texture->AutoClear == Default->AutoClear
			&& Texture->ClearColor.Equals(Default->ClearColor));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TextureRef,
				BuildAssetRevision(Texture),
				{
					BuildTypedChange(
						TEXT("media_player_ref"),
						TEXT("set"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(
							PlayerARef)),
				}),
			TEXT("sa008-media-texture-ref-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("media texture omitted fields remain unchanged"),
		Texture->GetMediaPlayer() == PlayerA
			&& Texture->AutoClear == Default->AutoClear
			&& Texture->ClearColor.Equals(Default->ClearColor));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TextureRef,
				BuildAssetRevision(Texture),
				{
					BuildTypedChange(
						TEXT("media_player_ref"),
						TEXT("clear"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
				}),
			TEXT("sa008-media-texture-ref-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("media texture ref clear restores the default null ref"),
		Texture->GetMediaPlayer() == nullptr
			&& Texture->AutoClear == Default->AutoClear
			&& Texture->ClearColor.Equals(Default->ClearColor));
	TestEqual(
		TEXT("four media texture updates persist once each"),
		Durable->PersistCount,
		4);
	TestEqual(
		TEXT("four media texture updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataPaperFlipbookProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataPaperFlipbookProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataPaperFlipbookProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.paper_flipbook.update"));
	if (!TestNotNull(TEXT("paper flipbook update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("paper flipbook field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("frames"),
					TEXT("frames_per_second"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString FlipbookRef =
		TEXT("uai://asset/sa008-paper-flipbook-target");
	const FString SpriteARef =
		TEXT("uai://asset/sa008-paper-flipbook-sprite-a");
	const FString SpriteBRef =
		TEXT("uai://asset/sa008-paper-flipbook-sprite-b");
	Owner.BindRef(FlipbookRef, TEXT("SA008_PaperFlipbookTarget"));
	Owner.BindRef(SpriteARef, TEXT("SA008_PaperFlipbookSpriteA"));
	Owner.BindRef(SpriteBRef, TEXT("SA008_PaperFlipbookSpriteB"));
	UPaperFlipbook* Flipbook = NewObject<UPaperFlipbook>(
		Owner.Package,
		TEXT("SA008_PaperFlipbookTarget"),
		RF_Public | RF_Standalone | RF_Transactional);
	UPaperSprite* SpriteA = NewObject<UPaperSprite>(
		Owner.Package,
		TEXT("SA008_PaperFlipbookSpriteA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UPaperSprite* SpriteB = NewObject<UPaperSprite>(
		Owner.Package,
		TEXT("SA008_PaperFlipbookSpriteB"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Flipbook || !SpriteA || !SpriteB)
	{
		return false;
	}
	{
		FScopedFlipbookMutator Mutator(Flipbook);
		Mutator.FramesPerSecond = 12.0f;
		FPaperFlipbookKeyFrame Frame;
		Frame.Sprite = SpriteA;
		Frame.FrameRun = 2;
		Mutator.KeyFrames = {Frame};
	}
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				FlipbookRef,
				BuildAssetRevision(Flipbook),
				{
					BuildTypedChange(
						TEXT("frames"),
						TEXT("replace"),
						TEXT("uai.asset.paper_flipbook.frames.v1"),
						BuildObjectArrayValue({
							BuildPaperFlipbookFrameValue(
								MakeShared<FJsonValueString>(SpriteBRef),
								4),
							BuildPaperFlipbookFrameValue(
								MakeShared<FJsonValueNull>(),
								1),
							BuildPaperFlipbookFrameValue(
								MakeShared<FJsonValueString>(SpriteARef),
								2),
						})),
					BuildTypedChange(
						TEXT("frames_per_second"),
						TEXT("replace"),
						TEXT("uai.value.decimal.v1"),
						MakeShared<FJsonValueNumber>(24.0)),
				}),
			TEXT("sa008-paper-flipbook-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper flipbook ordered nullable frames are independently read"),
		Flipbook->GetNumKeyFrames() == 3
			&& Flipbook->GetKeyFrameChecked(0).Sprite == SpriteB
			&& Flipbook->GetKeyFrameChecked(0).FrameRun == 4
			&& !Flipbook->GetKeyFrameChecked(1).Sprite
			&& Flipbook->GetKeyFrameChecked(1).FrameRun == 1
			&& Flipbook->GetKeyFrameChecked(2).Sprite == SpriteA
			&& Flipbook->GetKeyFrameChecked(2).FrameRun == 2
			&& Flipbook->GetNumFrames() == 7
			&& FMath::IsNearlyEqual(
				Flipbook->GetFramesPerSecond(),
				24.0f));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				FlipbookRef,
				BuildAssetRevision(Flipbook),
				{
					BuildTypedChange(
						TEXT("frames"),
						TEXT("replace"),
						TEXT("uai.asset.paper_flipbook.frames.v1"),
						BuildObjectArrayValue({})),
				}),
			TEXT("sa008-paper-flipbook-clear-frames")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper flipbook empty frames preserve omitted fps"),
		Flipbook->GetNumKeyFrames() == 0
			&& FMath::IsNearlyEqual(
				Flipbook->GetFramesPerSecond(),
				24.0f));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				FlipbookRef,
				BuildAssetRevision(Flipbook),
				{
					BuildTypedChange(
						TEXT("frames_per_second"),
						TEXT("replace"),
						TEXT("uai.value.decimal.v1"),
						MakeShared<FJsonValueNumber>(0.0)),
				}),
			TEXT("sa008-paper-flipbook-zero-fps")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper flipbook zero fps is not treated as omitted"),
		Flipbook->GetNumKeyFrames() == 0
			&& FMath::IsNearlyZero(Flipbook->GetFramesPerSecond()));
	TestEqual(
		TEXT("three paper flipbook updates persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three paper flipbook updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataPaperSpriteProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataPaperSpriteProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataPaperSpriteProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.paper_sprite.update"));
	if (!TestNotNull(TEXT("paper sprite update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("paper sprite field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("source_texture_ref"),
					TEXT("source_region"),
					TEXT("pivot"),
				}
			&& Profile->Fields.Num() == 3
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("null"),
					TEXT("clear"),
				}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString SpriteRef =
		TEXT("uai://asset/sa008-paper-sprite-target");
	const FString TextureARef =
		TEXT("uai://asset/sa008-paper-sprite-texture-a");
	const FString TextureBRef =
		TEXT("uai://asset/sa008-paper-sprite-texture-b");
	Owner.BindRef(SpriteRef, TEXT("SA008_PaperSpriteTarget"));
	Owner.BindRef(TextureARef, TEXT("SA008_PaperSpriteTextureA"));
	Owner.BindRef(TextureBRef, TEXT("SA008_PaperSpriteTextureB"));
	UPaperSprite* Sprite = NewObject<UPaperSprite>(
		Owner.Package,
		TEXT("SA008_PaperSpriteTarget"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureA = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_PaperSpriteTextureA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureB = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_PaperSpriteTextureB"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Sprite || !TextureA || !TextureB)
	{
		return false;
	}
	TextureA->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	TextureB->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	FSpriteAssetInitParameters Initial;
	Initial.Texture = TextureA;
	Initial.Offset = FIntPoint(1, 2);
	Initial.Dimension = FIntPoint(12, 10);
	Sprite->InitializeSprite(Initial, false);
	Sprite->SetPivotMode(
		ESpritePivotMode::Custom,
		FVector2D(3.0, 4.0),
		false);
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	const auto SubmitSpriteState =
		[this,
			&Fixture,
			&Owner,
			&Durable,
			Profile,
			Sprite,
			&SpriteRef](
				const FString& SourcePresence,
				const TSharedPtr<FJsonValue>& SourceValue,
				const FString& RegionPresence,
				const TSharedPtr<FJsonValue>& RegionValue,
				const FString& PivotPresence,
				const TSharedPtr<FJsonValue>& PivotValue,
				const FString& Scope)
		{
			return SubmitVerifiedAssetDataUpdate(
				*this,
				Fixture,
				Owner,
				Durable,
				*Profile,
				BuildUpdatePayload(
					SpriteRef,
					BuildAssetRevision(Sprite),
					{
						BuildTypedChange(
							TEXT("source_texture_ref"),
							SourcePresence,
							TEXT("uai.ref.asset.v1"),
							SourceValue),
						BuildTypedChange(
							TEXT("source_region"),
							RegionPresence,
							TEXT("uai.value.rect2d.v1"),
							RegionValue),
						BuildTypedChange(
							TEXT("pivot"),
							PivotPresence,
							TEXT("uai.value.vector2d.v1"),
							PivotValue),
					}),
				Scope);
		};

	if (!SubmitSpriteState(
			TEXT("set"),
			MakeShared<FJsonValueString>(TextureBRef),
			TEXT("set"),
			BuildRect2dValue(4, 5, 20, 16),
			TEXT("set"),
			BuildVector2dValue(7, 8),
			TEXT("sa008-paper-sprite-set")))
	{
		return false;
	}
	FVector2D Pivot;
	TestTrue(
		TEXT("paper sprite source region and pivot are independently read"),
		Sprite->GetSourceTexture() == TextureB
			&& Sprite->GetSourceUV().Equals(FVector2D(4.0, 5.0))
			&& Sprite->GetSourceSize().Equals(FVector2D(20.0, 16.0))
			&& Sprite->GetPivotMode(Pivot) == ESpritePivotMode::Custom
			&& Pivot.Equals(FVector2D(7.0, 8.0)));

	if (!SubmitSpriteState(
			TEXT("clear"),
			nullptr,
			TEXT("clear"),
			nullptr,
			TEXT("clear"),
			nullptr,
			TEXT("sa008-paper-sprite-clear")))
	{
		return false;
	}
	const UPaperSprite* Default = GetDefault<UPaperSprite>();
	FVector2D DefaultPivot;
	FVector2D ClearedPivot;
	TestTrue(
		TEXT("paper sprite clear restores exact class defaults"),
		Default
			&& Sprite->GetSourceTexture() == Default->GetSourceTexture()
			&& Sprite->GetSourceUV().Equals(Default->GetSourceUV())
			&& Sprite->GetSourceSize().Equals(Default->GetSourceSize())
			&& Sprite->GetPivotMode(ClearedPivot)
				== Default->GetPivotMode(DefaultPivot)
			&& ClearedPivot.Equals(DefaultPivot));

	if (!SubmitSpriteState(
			TEXT("set"),
			MakeShared<FJsonValueString>(TextureARef),
			TEXT("set"),
			BuildRect2dValue(1, 2, 12, 10),
			TEXT("set"),
			BuildVector2dValue(3, 4),
			TEXT("sa008-paper-sprite-restore")))
	{
		return false;
	}
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				SpriteRef,
				BuildAssetRevision(Sprite),
				{
					BuildTypedChange(
						TEXT("source_texture_ref"),
						TEXT("null"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
				}),
			TEXT("sa008-paper-sprite-null")))
	{
		return false;
	}
	FVector2D PreservedPivot;
	TestTrue(
		TEXT("paper sprite null source preserves omitted region and pivot"),
		!Sprite->GetSourceTexture()
			&& Sprite->GetSourceUV().Equals(FVector2D(1.0, 2.0))
			&& Sprite->GetSourceSize().Equals(FVector2D(12.0, 10.0))
			&& Sprite->GetPivotMode(PreservedPivot)
				== ESpritePivotMode::Custom
			&& PreservedPivot.Equals(FVector2D(3.0, 4.0)));
	TestEqual(
		TEXT("four paper sprite updates persist once each"),
		Durable->PersistCount,
		4);
	TestEqual(
		TEXT("four paper sprite updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataPaperTileMapProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataPaperTileMapProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataPaperTileMapProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.paper_tilemap.update"));
	if (!TestNotNull(TEXT("paper tilemap update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tilemap field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("map_size"),
					TEXT("tile_size"),
					TEXT("layers"),
				}
			&& Profile->Fields.Num() == 3
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString MapRef = TEXT("uai://asset/sa008-paper-tilemap-target");
	const FString TileSetARef =
		TEXT("uai://asset/sa008-paper-tilemap-set-a");
	const FString TileSetBRef =
		TEXT("uai://asset/sa008-paper-tilemap-set-b");
	Owner.BindRef(MapRef, TEXT("SA008_PaperTileMapTarget"));
	Owner.BindRef(TileSetARef, TEXT("SA008_PaperTileMapSetA"));
	Owner.BindRef(TileSetBRef, TEXT("SA008_PaperTileMapSetB"));
	UPaperTileMap* TileMap = NewObject<UPaperTileMap>(
		Owner.Package,
		TEXT("SA008_PaperTileMapTarget"),
		RF_Public | RF_Standalone | RF_Transactional);
	UPaperTileSet* TileSetA = NewObject<UPaperTileSet>(
		Owner.Package,
		TEXT("SA008_PaperTileMapSetA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UPaperTileSet* TileSetB = NewObject<UPaperTileSet>(
		Owner.Package,
		TEXT("SA008_PaperTileMapSetB"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureA = NewObject<UTexture2D>(Owner.Package);
	UTexture2D* TextureB = NewObject<UTexture2D>(Owner.Package);
	if (!TileMap || !TileSetA || !TileSetB || !TextureA || !TextureB)
	{
		return false;
	}
	TextureA->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	TextureB->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	TileSetA->SetTileSheetTexture(TextureA);
	TileSetA->SetTileSize(FIntPoint(8, 8));
	TileSetA->PostEditChange();
	TileSetB->SetTileSheetTexture(TextureB);
	TileSetB->SetTileSize(FIntPoint(8, 8));
	TileSetB->PostEditChange();
	TileMap->MapWidth = 2;
	TileMap->MapHeight = 2;
	TileMap->TileWidth = 8;
	TileMap->TileHeight = 8;
	UPaperTileLayer* OriginalLayer = TileMap->AddNewLayer();
	if (!OriginalLayer)
	{
		return false;
	}
	OriginalLayer->LayerName = FText::FromString(TEXT("Original"));
	FPaperTileInfo OriginalCell;
	OriginalCell.TileSet = TileSetA;
	OriginalCell.PackedTileIndex = 3;
	OriginalLayer->SetCell(1, 1, OriginalCell);
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();

	const TSharedPtr<FJsonObject> Ground = BuildPaperTileMapLayerValue(
		TEXT("Ground"),
		true,
		true,
		true,
		MakeShared<FJsonValueNull>(),
		MakeShared<FJsonValueNull>(),
		BuildLinearColorValue(1.0, 0.5, 0.25, 1.0),
		{
			BuildPaperTileMapCellValue(
				0, 0, TileSetBRef, 2, true, false, false),
			BuildPaperTileMapCellValue(
				2, 1, TileSetARef, 1, false, false, true),
		});
	const TSharedPtr<FJsonObject> Overlay = BuildPaperTileMapLayerValue(
		TEXT("Overlay"),
		false,
		false,
		false,
		MakeShared<FJsonValueNumber>(25.0),
		MakeShared<FJsonValueNumber>(-3.0),
		BuildLinearColorValue(0.1, 0.2, 0.3, 0.4),
		{});
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				MapRef,
				BuildAssetRevision(TileMap),
				{
					BuildTypedChange(
						TEXT("map_size"),
						TEXT("replace"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(3, 2)),
					BuildTypedChange(
						TEXT("tile_size"),
						TEXT("replace"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(16, 24)),
					BuildTypedChange(
						TEXT("layers"),
						TEXT("replace"),
						TEXT("uai.asset.paper_tilemap.layers.v1"),
						BuildObjectArrayValue({Ground, Overlay})),
				}),
			TEXT("sa008-paper-tilemap-replace")))
	{
		return false;
	}
	const UPaperTileLayer* GroundActual =
		TileMap->TileLayers.Num() == 2 ? TileMap->TileLayers[0] : nullptr;
	const UPaperTileLayer* OverlayActual =
		TileMap->TileLayers.Num() == 2 ? TileMap->TileLayers[1] : nullptr;
	bool bGroundHidden = true;
	bool bGroundThicknessOverride = true;
	bool bGroundOffsetOverride = true;
	bool bOverlayHidden = false;
	bool bOverlayThicknessOverride = false;
	bool bOverlayOffsetOverride = false;
	float OverlayThickness = 0.0f;
	float OverlayOffset = 0.0f;
	const bool bPrivateStateRead = GroundActual
		&& OverlayActual
		&& ReadExactBoolProperty(
			*GroundActual,
			TEXT("bHiddenInGame"),
			bGroundHidden)
		&& ReadExactBoolProperty(
			*GroundActual,
			TEXT("bOverrideCollisionThickness"),
			bGroundThicknessOverride)
		&& ReadExactBoolProperty(
			*GroundActual,
			TEXT("bOverrideCollisionOffset"),
			bGroundOffsetOverride)
		&& ReadExactBoolProperty(
			*OverlayActual,
			TEXT("bHiddenInGame"),
			bOverlayHidden)
		&& ReadExactBoolProperty(
			*OverlayActual,
			TEXT("bOverrideCollisionThickness"),
			bOverlayThicknessOverride)
		&& ReadExactFloatProperty(
			*OverlayActual,
			TEXT("CollisionThicknessOverride"),
			OverlayThickness)
		&& ReadExactBoolProperty(
			*OverlayActual,
			TEXT("bOverrideCollisionOffset"),
			bOverlayOffsetOverride)
		&& ReadExactFloatProperty(
			*OverlayActual,
			TEXT("CollisionOffsetOverride"),
			OverlayOffset);
	const FPaperTileInfo GroundCell0 = GroundActual
		? GroundActual->GetCell(0, 0)
		: FPaperTileInfo();
	const FPaperTileInfo GroundCell1 = GroundActual
		? GroundActual->GetCell(2, 1)
		: FPaperTileInfo();
	TestTrue(
		TEXT("paper tilemap discrete grid and layer state are independently read"),
		TileMap->MapWidth == 3
			&& TileMap->MapHeight == 2
			&& TileMap->TileWidth == 16
			&& TileMap->TileHeight == 24
			&& bPrivateStateRead
			&& GroundActual->LayerName.ToString() == TEXT("Ground")
			&& GroundActual->ShouldRenderInEditor()
			&& !bGroundHidden
			&& GroundActual->GetLayerCollides()
			&& !bGroundThicknessOverride
			&& !bGroundOffsetOverride
			&& GroundActual->GetLayerColor().Equals(
				FLinearColor(1.0f, 0.5f, 0.25f, 1.0f))
			&& GroundCell0.TileSet == TileSetB
			&& GroundCell0.GetTileIndex() == 2
			&& GroundCell0.HasFlag(EPaperTileFlags::FlipHorizontal)
			&& GroundCell1.TileSet == TileSetA
			&& GroundCell1.GetTileIndex() == 1
			&& GroundCell1.HasFlag(EPaperTileFlags::FlipDiagonal)
			&& OverlayActual->LayerName.ToString() == TEXT("Overlay")
			&& !OverlayActual->ShouldRenderInEditor()
			&& bOverlayHidden
			&& !OverlayActual->GetLayerCollides()
			&& bOverlayThicknessOverride
			&& FMath::IsNearlyEqual(OverlayThickness, 25.0f)
			&& bOverlayOffsetOverride
			&& FMath::IsNearlyEqual(OverlayOffset, -3.0f)
			&& OverlayActual->GetLayerColor().Equals(
				FLinearColor(0.1f, 0.2f, 0.3f, 0.4f))
			&& OverlayActual->GetNumOccupiedCells() == 0);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				MapRef,
				BuildAssetRevision(TileMap),
				{
					BuildTypedChange(
						TEXT("layers"),
						TEXT("replace"),
						TEXT("uai.asset.paper_tilemap.layers.v1"),
						BuildObjectArrayValue({})),
				}),
			TEXT("sa008-paper-tilemap-clear-layers")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tilemap empty layers preserve omitted grid extents"),
		TileMap->MapWidth == 3
			&& TileMap->MapHeight == 2
			&& TileMap->TileWidth == 16
			&& TileMap->TileHeight == 24
			&& TileMap->TileLayers.IsEmpty());
	TestEqual(
		TEXT("two paper tilemap updates persist once each"),
		Durable->PersistCount,
		2);
	TestEqual(
		TEXT("two paper tilemap updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataPaperTileSetProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataPaperTileSetProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataPaperTileSetProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.paper_tileset.update"));
	if (!TestNotNull(TEXT("paper tileset update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tileset field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("source_texture_ref"),
					TEXT("tile_size"),
					TEXT("margin"),
					TEXT("spacing"),
					TEXT("terrain"),
				}
			&& Profile->Fields.Num() == 5
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[3].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[4].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TileSetRef = TEXT("uai://asset/sa008-paper-tileset-target");
	const FString TextureARef = TEXT("uai://asset/sa008-paper-tileset-texture-a");
	const FString TextureBRef = TEXT("uai://asset/sa008-paper-tileset-texture-b");
	Owner.BindRef(TileSetRef, TEXT("SA008_PaperTileSetTarget"));
	Owner.BindRef(TextureARef, TEXT("SA008_PaperTileSetTextureA"));
	Owner.BindRef(TextureBRef, TEXT("SA008_PaperTileSetTextureB"));
	UPaperTileSet* TileSet = NewObject<UPaperTileSet>(
		Owner.Package,
		TEXT("SA008_PaperTileSetTarget"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureA = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_PaperTileSetTextureA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureB = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_PaperTileSetTextureB"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!TileSet || !TextureA || !TextureB)
	{
		return false;
	}
	TextureA->Source.Init(32, 32, 1, 1, TSF_BGRA8);
	TextureB->Source.Init(64, 32, 1, 1, TSF_BGRA8);
	TileSet->SetTileSheetTexture(TextureA);
	TileSet->SetTileSize(FIntPoint(8, 8));
	TileSet->SetMargin(FIntMargin(0));
	TileSet->SetPerTileSpacing(FIntPoint::ZeroValue);
	TileSet->PostEditChange();
	FPaperTileSetTerrain InitialTerrain;
	InitialTerrain.TerrainName = TEXT("Initial");
	InitialTerrain.CenterTileIndex = 1;
	TileSet->AddTerrainDescription(InitialTerrain);
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TileSetRef,
				BuildAssetRevision(TileSet),
				{
					BuildTypedChange(
						TEXT("source_texture_ref"),
						TEXT("replace"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(TextureBRef)),
					BuildTypedChange(
						TEXT("tile_size"),
						TEXT("replace"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(16, 8)),
					BuildTypedChange(
						TEXT("margin"),
						TEXT("replace"),
						TEXT("uai.value.integer.v1"),
						MakeShared<FJsonValueNumber>(1)),
					BuildTypedChange(
						TEXT("spacing"),
						TEXT("replace"),
						TEXT("uai.value.integer.v1"),
						MakeShared<FJsonValueNumber>(2)),
					BuildTypedChange(
						TEXT("terrain"),
						TEXT("replace"),
						TEXT("uai.asset.paper_tileset.terrain.v1"),
						BuildObjectArrayValue({
							BuildPaperTileSetTerrainValue(TEXT("Grass"), 0),
							BuildPaperTileSetTerrainValue(TEXT("Stone"), 1),
						})),
				}),
			TEXT("sa008-paper-tileset-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tileset layout and terrain are independently read"),
		TileSet->GetTileSheetTexture() == TextureB
			&& TileSet->GetTileSize() == FIntPoint(16, 8)
			&& TileSet->GetMargin() == FIntMargin(1)
			&& TileSet->GetPerTileSpacing() == FIntPoint(2, 2)
			&& TileSet->GetNumTerrains() == 2
			&& TileSet->GetTerrain(0).TerrainName == TEXT("Grass")
			&& TileSet->GetTerrain(0).CenterTileIndex == 0
			&& TileSet->GetTerrain(1).TerrainName == TEXT("Stone")
			&& TileSet->GetTerrain(1).CenterTileIndex == 1);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TileSetRef,
				BuildAssetRevision(TileSet),
				{
					BuildTypedChange(
						TEXT("terrain"),
						TEXT("replace"),
						TEXT("uai.asset.paper_tileset.terrain.v1"),
						BuildObjectArrayValue({})),
				}),
			TEXT("sa008-paper-tileset-clear-terrain")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tileset empty terrain preserves omitted layout"),
		TileSet->GetTileSheetTexture() == TextureB
			&& TileSet->GetTileSize() == FIntPoint(16, 8)
			&& TileSet->GetMargin() == FIntMargin(1)
			&& TileSet->GetPerTileSpacing() == FIntPoint(2, 2)
			&& TileSet->GetNumTerrains() == 0);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TileSetRef,
				BuildAssetRevision(TileSet),
				{
					BuildTypedChange(
						TEXT("tile_size"),
						TEXT("replace"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(4, 4)),
					BuildTypedChange(
						TEXT("margin"),
						TEXT("replace"),
						TEXT("uai.value.integer.v1"),
						MakeShared<FJsonValueNumber>(2)),
					BuildTypedChange(
						TEXT("spacing"),
						TEXT("replace"),
						TEXT("uai.value.integer.v1"),
						MakeShared<FJsonValueNumber>(1)),
				}),
			TEXT("sa008-paper-tileset-layout-only")))
	{
		return false;
	}
	TestTrue(
		TEXT("paper tileset partial layout keeps source and terrain omission"),
		TileSet->GetTileSheetTexture() == TextureB
			&& TileSet->GetTileSize() == FIntPoint(4, 4)
			&& TileSet->GetMargin() == FIntMargin(2)
			&& TileSet->GetPerTileSpacing() == FIntPoint(1, 1)
			&& TileSet->GetNumTerrains() == 0);
	TestEqual(
		TEXT("three paper tileset updates persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three paper tileset updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataRenderTargetProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataRenderTargetProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataRenderTargetProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.render_target.update"));
	if (!TestNotNull(TEXT("render target update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("render target field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("size"),
					TEXT("format"),
					TEXT("clear_color"),
					TEXT("auto_generate_mips"),
				}
			&& Profile->Fields.Num() == 4
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[3].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef =
		TEXT("uai://asset/sa008-render-target");
	Owner.BindRef(TargetRef, TEXT("SA008_RenderTarget"));
	UTextureRenderTarget2D* RenderTarget =
		NewObject<UTextureRenderTarget2D>(
			Owner.Package,
			TEXT("SA008_RenderTarget"),
			RF_Public | RF_Standalone | RF_Transactional);
	const UTextureRenderTarget2D* Default =
		GetDefault<UTextureRenderTarget2D>();
	if (!RenderTarget || !Default)
	{
		return false;
	}
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor(0.9f, 0.8f, 0.7f, 0.6f);
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->ResizeTarget(32, 24);
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(RenderTarget),
				{
					BuildTypedChange(
						TEXT("size"),
						TEXT("set"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(64, 48)),
					BuildTypedChange(
						TEXT("format"),
						TEXT("set"),
						TEXT("uai.asset.render_target.format.v1"),
						MakeShared<FJsonValueString>(
							TEXT("rgba16f"))),
					BuildTypedChange(
						TEXT("clear_color"),
						TEXT("set"),
						TEXT("uai.value.linear_color.v1"),
						BuildLinearColorValue(0.1, 0.2, 0.3, 0.4)),
					BuildTypedChange(
						TEXT("auto_generate_mips"),
						TEXT("set"),
						TEXT("uai.value.boolean.v1"),
						MakeShared<FJsonValueBoolean>(true)),
				}),
			TEXT("sa008-render-target-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("render target set is read from exact UObject fields"),
		RenderTarget->SizeX == 64
			&& RenderTarget->SizeY == 48
			&& RenderTarget->RenderTargetFormat == RTF_RGBA16f
			&& RenderTarget->ClearColor.Equals(
				FLinearColor(0.1f, 0.2f, 0.3f, 0.4f))
			&& RenderTarget->bAutoGenerateMips);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(RenderTarget),
				{
					BuildTypedChange(
						TEXT("auto_generate_mips"),
						TEXT("set"),
						TEXT("uai.value.boolean.v1"),
						MakeShared<FJsonValueBoolean>(false)),
				}),
			TEXT("sa008-render-target-partial")))
	{
		return false;
	}
	TestTrue(
		TEXT("render target partial update preserves omitted fields"),
		RenderTarget->SizeX == 64
			&& RenderTarget->SizeY == 48
			&& RenderTarget->RenderTargetFormat == RTF_RGBA16f
			&& RenderTarget->ClearColor.Equals(
				FLinearColor(0.1f, 0.2f, 0.3f, 0.4f))
			&& !RenderTarget->bAutoGenerateMips);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(RenderTarget),
				{
					BuildTypedChange(
						TEXT("size"),
						TEXT("clear"),
						TEXT("uai.value.extent2i.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("format"),
						TEXT("clear"),
						TEXT("uai.asset.render_target.format.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("clear_color"),
						TEXT("clear"),
						TEXT("uai.value.linear_color.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("auto_generate_mips"),
						TEXT("clear"),
						TEXT("uai.value.boolean.v1"),
						nullptr),
				}),
			TEXT("sa008-render-target-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("render target clear restores exact class defaults"),
		RenderTarget->SizeX == Default->SizeX
			&& RenderTarget->SizeY == Default->SizeY
			&& RenderTarget->RenderTargetFormat
				== Default->RenderTargetFormat
			&& RenderTarget->ClearColor.Equals(Default->ClearColor)
			&& RenderTarget->bAutoGenerateMips
				== Default->bAutoGenerateMips);
	TestEqual(
		TEXT("three render target updates persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three render target updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataSubUVProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataSubUVProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringAssetDataSubUVProductionContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.subuv_animation.update"));
	if (!TestNotNull(TEXT("SubUV update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("SubUV field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("source_texture_ref"),
					TEXT("grid"),
					TEXT("bounding_geometry"),
				}
			&& Profile->Fields.Num() == 3
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("null"),
					TEXT("clear"),
				}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString SubUVRef = TEXT("uai://asset/sa008-subuv-target");
	const FString TextureARef = TEXT("uai://asset/sa008-subuv-texture-a");
	const FString TextureBRef = TEXT("uai://asset/sa008-subuv-texture-b");
	Owner.BindRef(SubUVRef, TEXT("SA008_SubUVTarget"));
	Owner.BindRef(TextureARef, TEXT("SA008_SubUVTextureA"));
	Owner.BindRef(TextureBRef, TEXT("SA008_SubUVTextureB"));
	USubUVAnimation* SubUV = NewObject<USubUVAnimation>(
		Owner.Package,
		TEXT("SA008_SubUVTarget"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureA = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_SubUVTextureA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureB = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_SubUVTextureB"),
		RF_Public | RF_Standalone | RF_Transactional);
	const USubUVAnimation* Default = GetDefault<USubUVAnimation>();
	if (!SubUV || !TextureA || !TextureB || !Default)
	{
		return false;
	}
	TextureA->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	TextureB->Source.Init(64, 64, 1, 1, TSF_BGRA8);
	SubUV->SubUVTexture = TextureA;
	SubUV->SubImages_Horizontal = 2;
	SubUV->SubImages_Vertical = 2;
	SubUV->BoundingMode = BVC_FourVertices;
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				SubUVRef,
				BuildAssetRevision(SubUV),
				{
					BuildTypedChange(
						TEXT("source_texture_ref"),
						TEXT("set"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(TextureBRef)),
					BuildTypedChange(
						TEXT("grid"),
						TEXT("set"),
						TEXT("uai.value.extent2i.v1"),
						BuildExtent2iValue(4, 8)),
					BuildTypedChange(
						TEXT("bounding_geometry"),
						TEXT("set"),
						TEXT(
							"uai.asset.subuv_animation."
							"bounding_geometry.v1"),
						MakeShared<FJsonValueString>(
							TEXT("eight_vertices"))),
				}),
			TEXT("sa008-subuv-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("SubUV set is read from exact UObject fields"),
		SubUV->SubUVTexture == TextureB
			&& SubUV->SubImages_Horizontal == 4
			&& SubUV->SubImages_Vertical == 8
			&& SubUV->BoundingMode == BVC_EightVertices);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				SubUVRef,
				BuildAssetRevision(SubUV),
				{
					BuildTypedChange(
						TEXT("source_texture_ref"),
						TEXT("null"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
				}),
			TEXT("sa008-subuv-null")))
	{
		return false;
	}
	TestTrue(
		TEXT("SubUV null source preserves omitted grid and geometry"),
		SubUV->SubUVTexture == nullptr
			&& SubUV->SubImages_Horizontal == 4
			&& SubUV->SubImages_Vertical == 8
			&& SubUV->BoundingMode == BVC_EightVertices);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				SubUVRef,
				BuildAssetRevision(SubUV),
				{
					BuildTypedChange(
						TEXT("source_texture_ref"),
						TEXT("clear"),
						TEXT("uai.ref.asset.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("grid"),
						TEXT("clear"),
						TEXT("uai.value.extent2i.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("bounding_geometry"),
						TEXT("clear"),
						TEXT(
							"uai.asset.subuv_animation."
							"bounding_geometry.v1"),
						nullptr),
				}),
			TEXT("sa008-subuv-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("SubUV clear restores exact class defaults"),
		SubUV->SubUVTexture == Default->SubUVTexture
			&& SubUV->SubImages_Horizontal
				== Default->SubImages_Horizontal
			&& SubUV->SubImages_Vertical
				== Default->SubImages_Vertical
			&& SubUV->BoundingMode == Default->BoundingMode);
	TestEqual(
		TEXT("three SubUV updates persist once each"),
		Durable->PersistCount,
		3);
	TestEqual(
		TEXT("three SubUV updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataTextureCollectionProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataTextureCollectionProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataTextureCollectionProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.texture_collection.update"));
	if (!TestNotNull(TEXT("texture collection update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("texture collection field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{TEXT("texture_refs")}
			&& Profile->Fields.Num() == 1
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("remove"),
					TEXT("replace"),
				});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString CollectionRef =
		TEXT("uai://asset/sa008-texture-collection");
	const FString TextureARef =
		TEXT("uai://asset/sa008-texture-collection-a");
	const FString TextureBRef =
		TEXT("uai://asset/sa008-texture-collection-b");
	const FString TextureCRef =
		TEXT("uai://asset/sa008-texture-collection-c");
	Owner.BindRef(CollectionRef, TEXT("SA008_TextureCollection"));
	Owner.BindRef(TextureARef, TEXT("SA008_TextureCollectionA"));
	Owner.BindRef(TextureBRef, TEXT("SA008_TextureCollectionB"));
	Owner.BindRef(TextureCRef, TEXT("SA008_TextureCollectionC"));
	UTextureCollection* Collection = NewObject<UTextureCollection>(
		Owner.Package,
		TEXT("SA008_TextureCollection"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureA = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureCollectionA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureB = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureCollectionB"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* TextureC = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureCollectionC"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Collection || !TextureA || !TextureB || !TextureC)
	{
		return false;
	}
	Collection->Textures = {TextureA, TextureB};
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	const auto SubmitRefs =
		[this,
			&Fixture,
			&Owner,
			&Durable,
			Profile,
			Collection,
			&CollectionRef](
				const FString& Presence,
				const TSharedPtr<FJsonValue>& Value,
				const FString& Scope)
		{
			return SubmitVerifiedAssetDataUpdate(
				*this,
				Fixture,
				Owner,
				Durable,
				*Profile,
				BuildUpdatePayload(
					CollectionRef,
					BuildAssetRevision(Collection),
					{
						BuildTypedChange(
							TEXT("texture_refs"),
							Presence,
							TEXT("uai.value.ordered_asset_refs.v1"),
							Value),
					}),
				Scope);
		};

	if (!SubmitRefs(
			TEXT("set"),
			BuildStringArrayValue({TextureCRef, TextureARef}),
			TEXT("sa008-texture-collection-set")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture collection set appends unique identities in order"),
		Collection->Textures.Num() == 3
			&& Collection->Textures[0] == TextureA
			&& Collection->Textures[1] == TextureB
			&& Collection->Textures[2] == TextureC);

	if (!SubmitRefs(
			TEXT("remove"),
			BuildStringArrayValue({TextureARef}),
			TEXT("sa008-texture-collection-remove")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture collection remove preserves remaining order"),
		Collection->Textures.Num() == 2
			&& Collection->Textures[0] == TextureB
			&& Collection->Textures[1] == TextureC);

	if (!SubmitRefs(
			TEXT("replace"),
			BuildStringArrayValue({TextureCRef, TextureARef}),
			TEXT("sa008-texture-collection-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture collection replace uses exact requested order"),
		Collection->Textures.Num() == 2
			&& Collection->Textures[0] == TextureC
			&& Collection->Textures[1] == TextureA);

	if (!SubmitRefs(
			TEXT("clear"),
			nullptr,
			TEXT("sa008-texture-collection-clear")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture collection clear produces the exact empty state"),
		Collection->Textures.IsEmpty());
	TestEqual(
		TEXT("four texture collection updates persist once each"),
		Durable->PersistCount,
		4);
	TestEqual(
		TEXT("four texture collection updates acknowledge after C9"),
		Durable->AcknowledgeCount,
		4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataTextureArrayProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataTextureArrayProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataTextureArrayProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.texture_array.replace"));
	if (!TestNotNull(TEXT("texture array replace profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("texture array field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("source_texture_refs"),
					TEXT("format_settings"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-texture-array");
	const FString InitialRef = TEXT("uai://asset/sa008-texture-array-initial");
	const FString SourceARef = TEXT("uai://asset/sa008-texture-array-a");
	const FString SourceBRef = TEXT("uai://asset/sa008-texture-array-b");
	Owner.BindRef(TargetRef, TEXT("SA008_TextureArray"));
	Owner.BindRef(InitialRef, TEXT("SA008_TextureArrayInitial"));
	Owner.BindRef(SourceARef, TEXT("SA008_TextureArrayA"));
	Owner.BindRef(SourceBRef, TEXT("SA008_TextureArrayB"));
	UTexture2DArray* Target = NewObject<UTexture2DArray>(
		Owner.Package,
		TEXT("SA008_TextureArray"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* Initial = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureArrayInitial"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* SourceA = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureArrayA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTexture2D* SourceB = NewObject<UTexture2D>(
		Owner.Package,
		TEXT("SA008_TextureArrayB"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Target
		|| !Initial
		|| !SourceA
		|| !SourceB
		|| !InitializeTexture2DSource(*Initial, 4, 11)
		|| !InitializeTexture2DSource(*SourceA, 4, 47)
		|| !InitializeTexture2DSource(*SourceB, 4, 103))
	{
		return false;
	}
	Target->SourceTextures = {Initial};
	if (!TestTrue(
			TEXT("texture array initial source assembles"),
			Target->UpdateSourceFromSourceTextures(false)))
	{
		return false;
	}
	Target->SRGB = true;
	Target->CompressionSettings = TC_Default;
	Target->MipGenSettings = TMGS_NoMipmaps;
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Target),
				{
					BuildTypedChange(
						TEXT("source_texture_refs"),
						TEXT("replace"),
						TEXT("uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({SourceARef, SourceBRef})),
					BuildTypedChange(
						TEXT("format_settings"),
						TEXT("replace"),
						TEXT("uai.asset.texture.format_settings.v1"),
						BuildTextureFormatSettingsValue(
							false,
							TEXT("masks"),
							TEXT("simple_average"))),
				}),
			TEXT("sa008-texture-array-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture array exact assembled source is read from UObject"),
		Target->SourceTextures.Num() == 2
			&& Target->SourceTextures[0] == SourceA
			&& Target->SourceTextures[1] == SourceB
			&& Target->bSourceGeneratedFromSourceTexturesArray
			&& Target->Source.GetSizeX() == 4
			&& Target->Source.GetSizeY() == 4
			&& Target->Source.GetNumSlices() == 2
			&& Target->Source.GetNumMips() == 1
			&& Target->Source.GetFormat() == TSF_BGRA8
			&& !Target->SRGB
			&& Target->CompressionSettings == TC_Masks
			&& Target->MipGenSettings == TMGS_SimpleAverage
			&& TextureSourceMatchesOrderedConcatenation(
				*Target,
				TArray<UTexture*>{SourceA, SourceB}));
	TestEqual(TEXT("texture array persists exactly once"), Durable->PersistCount, 1);
	TestEqual(
		TEXT("texture array acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataTextureCubeArrayProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataTextureCubeArrayProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataTextureCubeArrayProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.texture_cube_array.replace"));
	if (!TestNotNull(TEXT("texture cube array replace profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("texture cube array field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("source_cube_refs"),
					TEXT("format_settings"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-texture-cube-array");
	const FString InitialRef = TEXT("uai://asset/sa008-texture-cube-array-initial");
	const FString SourceARef = TEXT("uai://asset/sa008-texture-cube-array-a");
	const FString SourceBRef = TEXT("uai://asset/sa008-texture-cube-array-b");
	Owner.BindRef(TargetRef, TEXT("SA008_TextureCubeArray"));
	Owner.BindRef(InitialRef, TEXT("SA008_TextureCubeArrayInitial"));
	Owner.BindRef(SourceARef, TEXT("SA008_TextureCubeArrayA"));
	Owner.BindRef(SourceBRef, TEXT("SA008_TextureCubeArrayB"));
	UTextureCubeArray* Target = NewObject<UTextureCubeArray>(
		Owner.Package,
		TEXT("SA008_TextureCubeArray"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTextureCube* Initial = NewObject<UTextureCube>(
		Owner.Package,
		TEXT("SA008_TextureCubeArrayInitial"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTextureCube* SourceA = NewObject<UTextureCube>(
		Owner.Package,
		TEXT("SA008_TextureCubeArrayA"),
		RF_Public | RF_Standalone | RF_Transactional);
	UTextureCube* SourceB = NewObject<UTextureCube>(
		Owner.Package,
		TEXT("SA008_TextureCubeArrayB"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Target
		|| !Initial
		|| !SourceA
		|| !SourceB
		|| !InitializeTextureCubeSource(*Initial, 2, 19)
		|| !InitializeTextureCubeSource(*SourceA, 2, 59)
		|| !InitializeTextureCubeSource(*SourceB, 2, 131))
	{
		return false;
	}
	Target->SourceTextures = {Initial};
	if (!TestTrue(
			TEXT("texture cube array initial source assembles"),
			Target->UpdateSourceFromSourceTextures(false)))
	{
		return false;
	}
	Target->SRGB = true;
	Target->CompressionSettings = TC_Default;
	Target->MipGenSettings = TMGS_NoMipmaps;
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Target),
				{
					BuildTypedChange(
						TEXT("source_cube_refs"),
						TEXT("replace"),
						TEXT("uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue({SourceARef, SourceBRef})),
					BuildTypedChange(
						TEXT("format_settings"),
						TEXT("replace"),
						TEXT("uai.asset.texture.format_settings.v1"),
						BuildTextureFormatSettingsValue(
							false,
							TEXT("masks"),
							TEXT("simple_average"))),
				}),
			TEXT("sa008-texture-cube-array-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("texture cube array exact assembled source is read from UObject"),
		Target->SourceTextures.Num() == 2
			&& Target->SourceTextures[0] == SourceA
			&& Target->SourceTextures[1] == SourceB
			&& Target->bSourceGeneratedFromSourceTexturesArray
			&& Target->Source.GetSizeX() == 2
			&& Target->Source.GetSizeY() == 2
			&& Target->Source.GetNumSlices() == 12
			&& Target->Source.GetNumMips() == 1
			&& Target->Source.GetFormat() == TSF_BGRA8
			&& !Target->SRGB
			&& Target->CompressionSettings == TC_Masks
			&& Target->MipGenSettings == TMGS_SimpleAverage
			&& TextureSourceMatchesOrderedConcatenation(
				*Target,
				TArray<UTexture*>{SourceA, SourceB}));
	TestEqual(
		TEXT("texture cube array persists exactly once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("texture cube array acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataTextureCubeProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataTextureCubeProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataTextureCubeProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.texture_cube.replace"));
	if (!TestNotNull(TEXT("texture cube replace profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("texture cube field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("face_texture_refs"),
					TEXT("format_settings"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-texture-cube");
	const TArray<FString> FaceRefs{
		TEXT("uai://asset/sa008-texture-cube-positive-x"),
		TEXT("uai://asset/sa008-texture-cube-negative-x"),
		TEXT("uai://asset/sa008-texture-cube-positive-y"),
		TEXT("uai://asset/sa008-texture-cube-negative-y"),
		TEXT("uai://asset/sa008-texture-cube-positive-z"),
		TEXT("uai://asset/sa008-texture-cube-negative-z"),
	};
	const TArray<FName> FaceNames{
		TEXT("SA008_TextureCubePositiveX"),
		TEXT("SA008_TextureCubeNegativeX"),
		TEXT("SA008_TextureCubePositiveY"),
		TEXT("SA008_TextureCubeNegativeY"),
		TEXT("SA008_TextureCubePositiveZ"),
		TEXT("SA008_TextureCubeNegativeZ"),
	};
	Owner.BindRef(TargetRef, TEXT("SA008_TextureCube"));
	UTextureCube* Target = NewObject<UTextureCube>(
		Owner.Package,
		TEXT("SA008_TextureCube"),
		RF_Public | RF_Standalone | RF_Transactional);
	TArray<UTexture2D*> Faces;
	bool bValid = Target && InitializeTextureCubeSource(*Target, 2, 23);
	for (int32 Index = 0; Index < FaceRefs.Num(); ++Index)
	{
		Owner.BindRef(FaceRefs[Index], FaceNames[Index]);
		UTexture2D* Face = NewObject<UTexture2D>(
			Owner.Package,
			FaceNames[Index],
			RF_Public | RF_Standalone | RF_Transactional);
		bValid = bValid
			&& Face
			&& InitializeTexture2DSource(
				*Face,
				2,
				static_cast<uint8>(41 + Index * 29));
		Faces.Add(Face);
	}
	if (!bValid)
	{
		return false;
	}
	Target->SRGB = true;
	Target->CompressionSettings = TC_Default;
	Target->MipGenSettings = TMGS_NoMipmaps;
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Target),
				{
					BuildTypedChange(
						TEXT("face_texture_refs"),
						TEXT("replace"),
						TEXT("uai.asset.texture_cube.face_refs.v1"),
						BuildTextureCubeFaceRefsValue(FaceRefs)),
					BuildTypedChange(
						TEXT("format_settings"),
						TEXT("replace"),
						TEXT("uai.asset.texture.format_settings.v1"),
						BuildTextureFormatSettingsValue(
							false,
							TEXT("masks"),
							TEXT("simple_average"))),
				}),
			TEXT("sa008-texture-cube-replace")))
	{
		return false;
	}
	TArray<UTexture*> OrderedFaces;
	for (UTexture2D* Face : Faces)
	{
		OrderedFaces.Add(Face);
	}
	TestTrue(
		TEXT("texture cube exact fixed face assembly is read from UObject"),
		Target->Source.GetSizeX() == 2
			&& Target->Source.GetSizeY() == 2
			&& Target->Source.GetNumSlices() == 6
			&& Target->Source.GetNumMips() == 1
			&& Target->Source.GetFormat() == TSF_BGRA8
			&& !Target->SRGB
			&& Target->CompressionSettings == TC_Masks
			&& Target->MipGenSettings == TMGS_SimpleAverage
			&& TextureSourceMatchesOrderedConcatenation(
				*Target,
				OrderedFaces));
	TestEqual(TEXT("texture cube persists exactly once"), Durable->PersistCount, 1);
	TestEqual(
		TEXT("texture cube acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataVolumeTextureProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataVolumeTextureProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataVolumeTextureProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.volume_texture.replace"));
	if (!TestNotNull(TEXT("volume texture replace profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("volume texture field contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("slice_texture_refs"),
					TEXT("format_settings"),
				}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-volume-texture");
	const TArray<FString> SliceRefs{
		TEXT("uai://asset/sa008-volume-texture-a"),
		TEXT("uai://asset/sa008-volume-texture-b"),
		TEXT("uai://asset/sa008-volume-texture-c"),
	};
	const TArray<FName> SliceNames{
		TEXT("SA008_VolumeTextureA"),
		TEXT("SA008_VolumeTextureB"),
		TEXT("SA008_VolumeTextureC"),
	};
	Owner.BindRef(TargetRef, TEXT("SA008_VolumeTexture"));
	UVolumeTexture* Target = NewObject<UVolumeTexture>(
		Owner.Package,
		TEXT("SA008_VolumeTexture"),
		RF_Public | RF_Standalone | RF_Transactional);
	TArray<UTexture2D*> Slices;
	bool bValid = Target
		&& InitializeVolumeTextureSource(*Target, 2, 2, 17);
	for (int32 Index = 0; Index < SliceRefs.Num(); ++Index)
	{
		Owner.BindRef(SliceRefs[Index], SliceNames[Index]);
		UTexture2D* Slice = NewObject<UTexture2D>(
			Owner.Package,
			SliceNames[Index],
			RF_Public | RF_Standalone | RF_Transactional);
		bValid = bValid
			&& Slice
			&& InitializeTexture2DSource(
				*Slice,
				2,
				static_cast<uint8>(53 + Index * 48));
		Slices.Add(Slice);
	}
	if (!bValid)
	{
		return false;
	}
	Target->SRGB = true;
	Target->CompressionSettings = TC_Default;
	Target->MipGenSettings = TMGS_NoMipmaps;
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Target),
				{
					BuildTypedChange(
						TEXT("slice_texture_refs"),
						TEXT("replace"),
						TEXT("uai.value.ordered_asset_refs.v1"),
						BuildStringArrayValue(SliceRefs)),
					BuildTypedChange(
						TEXT("format_settings"),
						TEXT("replace"),
						TEXT("uai.asset.texture.format_settings.v1"),
						BuildTextureFormatSettingsValue(
							false,
							TEXT("masks"),
							TEXT("simple_average"))),
				}),
			TEXT("sa008-volume-texture-replace")))
	{
		return false;
	}
	TArray<UTexture*> OrderedSlices;
	for (UTexture2D* Slice : Slices)
	{
		OrderedSlices.Add(Slice);
	}
	TestTrue(
		TEXT("volume texture exact ordered slice assembly is read from UObject"),
		Target->Source.GetSizeX() == 2
			&& Target->Source.GetSizeY() == 2
			&& Target->Source.GetNumSlices() == 3
			&& Target->Source.GetNumMips() == 1
			&& Target->Source.GetFormat() == TSF_BGRA8
			&& Target->Source2DTexture == nullptr
			&& Target->Source2DTileSizeX == 0
			&& Target->Source2DTileSizeY == 0
			&& !Target->SRGB
			&& Target->CompressionSettings == TC_Masks
			&& Target->MipGenSettings == TMGS_SimpleAverage
			&& TextureSourceMatchesOrderedConcatenation(
				*Target,
				OrderedSlices));
	TestEqual(TEXT("volume texture persists exactly once"), Durable->PersistCount, 1);
	TestEqual(
		TEXT("volume texture acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataStaticMeshProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataStaticMeshProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataStaticMeshProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		AddError(ProfileError);
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.static_mesh.update"));
	if (!TestNotNull(TEXT("static mesh update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("static mesh field and presence contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("materials"),
					TEXT("collision"),
					TEXT("sockets"),
					TEXT("build_settings"),
				}
			&& Profile->Fields.Num() == 4
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("remove"),
					TEXT("replace"),
				}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace"), TEXT("clear")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{
					TEXT("set"),
					TEXT("clear"),
					TEXT("remove"),
					TEXT("replace"),
				}
			&& Profile->Fields[3].AllowedPresences
				== TArray<FString>{TEXT("set"), TEXT("clear")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-static-mesh");
	const FString MaterialRef = TEXT("uai://asset/sa008-static-material");
	Owner.BindRef(TargetRef, TEXT("SA008_StaticMesh"));
	Owner.BindRef(MaterialRef, TEXT("SA008_StaticMaterial"));
	UStaticMesh* Mesh = NewObject<UStaticMesh>(
		Owner.Package,
		TEXT("SA008_StaticMesh"),
		RF_Public | RF_Standalone | RF_Transactional);
	UMaterial* Material = NewObject<UMaterial>(
		Owner.Package,
		TEXT("SA008_StaticMaterial"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Mesh || !Material)
	{
		return false;
	}
	Mesh->CreateBodySetup();
	Mesh->AddSourceModel();
	FMeshDescription MeshDescription;
	FStaticMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();
	FMeshDescriptionBuilder MeshBuilder;
	MeshBuilder.SetMeshDescription(&MeshDescription);
	const FPolygonGroupID PolygonGroup =
		MeshBuilder.AppendPolygonGroup(TEXT("Original"));
	const FVertexID VertexA =
		MeshBuilder.AppendVertex(FVector::ZeroVector);
	const FVertexID VertexB =
		MeshBuilder.AppendVertex(FVector(100.0, 0.0, 0.0));
	const FVertexID VertexC =
		MeshBuilder.AppendVertex(FVector(0.0, 100.0, 0.0));
	MeshBuilder.AppendTriangle(VertexA, VertexB, VertexC, PolygonGroup);
	Mesh->CreateMeshDescription(0, MeshDescription);
	Mesh->CommitMeshDescription(0);
	Mesh->SetStaticMaterials({
		FStaticMaterial(nullptr, TEXT("Original")),
	});
	FKBoxElem OriginalBox;
	OriginalBox.Center = FVector(-1.0, -2.0, -3.0);
	OriginalBox.X = 2.0;
	OriginalBox.Y = 4.0;
	OriginalBox.Z = 6.0;
	Mesh->GetBodySetup()->AggGeom.BoxElems.Add(OriginalBox);
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseDefault;
	UStaticMeshSocket* OriginalSocket = NewObject<UStaticMeshSocket>(
		Mesh,
		NAME_None,
		RF_Transactional);
	OriginalSocket->SocketName = TEXT("OriginalSocket");
	OriginalSocket->RelativeLocation = FVector(-1.0, -2.0, -3.0);
	Mesh->Sockets.Add(OriginalSocket);
	FMeshBuildSettings& InitialBuild =
		Mesh->GetSourceModel(0).BuildSettings;
	InitialBuild.bRecomputeNormals = false;
	InitialBuild.bRecomputeTangents = false;
	InitialBuild.bComputeWeightedNormals = false;
	InitialBuild.bRemoveDegenerates = true;
	InitialBuild.bUseMikkTSpace = false;
	InitialBuild.bGenerateLightmapUVs = true;
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Mesh),
				{
					BuildTypedChange(
						TEXT("materials"),
						TEXT("set"),
						TEXT("uai.asset.mesh.material_slots.v1"),
						BuildObjectArrayValue({
							BuildMaterialSlotValue(TEXT("Main"), MaterialRef),
						})),
					BuildTypedChange(
						TEXT("collision"),
						TEXT("replace"),
						TEXT("uai.asset.static_mesh.collision.v1"),
						BuildStaticCollisionValue()),
					BuildTypedChange(
						TEXT("sockets"),
						TEXT("set"),
						TEXT("uai.asset.static_mesh.sockets.v1"),
						BuildObjectArrayValue({
							BuildStaticSocketValue(
								TEXT("SocketA"),
								FVector(1.0, 2.0, 3.0),
								FRotator(4.0, 5.0, 6.0),
								FVector(1.0, 2.0, 1.0)),
						})),
					BuildTypedChange(
						TEXT("build_settings"),
						TEXT("set"),
						TEXT("uai.asset.static_mesh.build_settings.v1"),
						BuildStaticBuildSettingsValue(
							true,
							true,
							true,
							false,
							true,
							false)),
				}),
			TEXT("sa008-static-mesh-set")))
	{
		return false;
	}
	const UBodySetup* BodySetup = Mesh->GetBodySetup();
	const TObjectPtr<UStaticMeshSocket>* SocketSlot =
		Mesh->Sockets.FindByPredicate(
			[](const UStaticMeshSocket* Candidate)
			{
				return Candidate
					&& Candidate->SocketName == TEXT("SocketA");
			});
	const UStaticMeshSocket* Socket =
		SocketSlot ? SocketSlot->Get() : nullptr;
	const FMeshBuildSettings& AppliedBuild =
		Mesh->GetSourceModel(0).BuildSettings;
	TestTrue(
		TEXT("static mesh set and replace read exact asset-local state"),
		Mesh->GetStaticMaterials().Num() == 2
			&& Mesh->GetStaticMaterials()[1].MaterialSlotName == TEXT("Main")
			&& Mesh->GetStaticMaterials()[1].MaterialInterface == Material
			&& BodySetup
			&& BodySetup->CollisionTraceFlag == CTF_UseSimpleAndComplex
			&& BodySetup->AggGeom.BoxElems.Num() == 1
			&& BodySetup->AggGeom.SphereElems.Num() == 1
			&& BodySetup->AggGeom.SphylElems.Num() == 1
			&& BodySetup->AggGeom.BoxElems[0].Center.Equals(
				FVector(1.0, 2.0, 3.0))
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].X, 20.0f)
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].Y, 40.0f)
			&& FMath::IsNearlyEqual(BodySetup->AggGeom.BoxElems[0].Z, 60.0f)
			&& BodySetup->AggGeom.BoxElems[0].Rotation.Equals(
				FRotator(5.0, 10.0, 15.0))
			&& Socket
			&& Mesh->Sockets.Num() == 2
			&& Socket->RelativeLocation.Equals(FVector(1.0, 2.0, 3.0))
			&& Socket->RelativeRotation.Equals(FRotator(4.0, 5.0, 6.0))
			&& Socket->RelativeScale.Equals(FVector(1.0, 2.0, 1.0))
			&& AppliedBuild.bRecomputeNormals
			&& AppliedBuild.bRecomputeTangents
			&& AppliedBuild.bComputeWeightedNormals
			&& !AppliedBuild.bRemoveDegenerates
			&& AppliedBuild.bUseMikkTSpace
			&& !AppliedBuild.bGenerateLightmapUVs);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Mesh),
				{
					BuildTypedChange(
						TEXT("materials"),
						TEXT("remove"),
						TEXT("uai.asset.mesh.material_slots.v1"),
						BuildStringArrayValue({TEXT("Original")})),
					BuildTypedChange(
						TEXT("collision"),
						TEXT("clear"),
						TEXT("uai.asset.static_mesh.collision.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("sockets"),
						TEXT("remove"),
						TEXT("uai.asset.static_mesh.sockets.v1"),
						BuildStringArrayValue({TEXT("OriginalSocket")})),
					BuildTypedChange(
						TEXT("build_settings"),
						TEXT("clear"),
						TEXT("uai.asset.static_mesh.build_settings.v1"),
						nullptr),
				}),
			TEXT("sa008-static-mesh-remove-clear")))
	{
		return false;
	}
	const FMeshBuildSettings DefaultBuild;
	const FMeshBuildSettings& ClearedBuild =
		Mesh->GetSourceModel(0).BuildSettings;
	TestTrue(
		TEXT("static mesh remove and clear preserve only unremoved entries"),
		Mesh->GetStaticMaterials().Num() == 1
			&& Mesh->GetStaticMaterials()[0].MaterialSlotName == TEXT("Main")
			&& Mesh->Sockets.Num() == 1
			&& Mesh->Sockets[0]
			&& Mesh->Sockets[0]->SocketName == TEXT("SocketA")
			&& Mesh->GetBodySetup()->CollisionTraceFlag == CTF_UseDefault
			&& Mesh->GetBodySetup()->AggGeom.BoxElems.IsEmpty()
			&& Mesh->GetBodySetup()->AggGeom.SphereElems.IsEmpty()
			&& Mesh->GetBodySetup()->AggGeom.SphylElems.IsEmpty()
			&& ClearedBuild.bRecomputeNormals == DefaultBuild.bRecomputeNormals
			&& ClearedBuild.bRecomputeTangents == DefaultBuild.bRecomputeTangents
			&& ClearedBuild.bComputeWeightedNormals
				== DefaultBuild.bComputeWeightedNormals
			&& ClearedBuild.bRemoveDegenerates
				== DefaultBuild.bRemoveDegenerates
			&& ClearedBuild.bUseMikkTSpace == DefaultBuild.bUseMikkTSpace
			&& ClearedBuild.bGenerateLightmapUVs
				== DefaultBuild.bGenerateLightmapUVs);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Mesh),
				{
					BuildTypedChange(
						TEXT("materials"),
						TEXT("replace"),
						TEXT("uai.asset.mesh.material_slots.v1"),
						BuildObjectArrayValue({
							BuildMaterialSlotValue(
								TEXT("Replacement"),
								MaterialRef),
						})),
					BuildTypedChange(
						TEXT("sockets"),
						TEXT("replace"),
						TEXT("uai.asset.static_mesh.sockets.v1"),
						BuildObjectArrayValue({
							BuildStaticSocketValue(
								TEXT("ReplacementSocket"),
								FVector(9.0, 8.0, 7.0),
								FRotator(6.0, 5.0, 4.0),
								FVector(2.0, 1.0, 2.0)),
						})),
				}),
			TEXT("sa008-static-mesh-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("static mesh replace discards prior collections exactly"),
		Mesh->GetStaticMaterials().Num() == 1
			&& Mesh->GetStaticMaterials()[0].MaterialSlotName
				== TEXT("Replacement")
			&& Mesh->GetStaticMaterials()[0].MaterialInterface == Material
			&& Mesh->Sockets.Num() == 1
			&& Mesh->Sockets[0]
			&& Mesh->Sockets[0]->SocketName == TEXT("ReplacementSocket")
			&& Mesh->Sockets[0]->RelativeLocation.Equals(
				FVector(9.0, 8.0, 7.0)));

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Mesh),
				{
					BuildTypedChange(
						TEXT("materials"),
						TEXT("clear"),
						TEXT("uai.asset.mesh.material_slots.v1"),
						nullptr),
					BuildTypedChange(
						TEXT("sockets"),
						TEXT("clear"),
						TEXT("uai.asset.static_mesh.sockets.v1"),
						nullptr),
				}),
			TEXT("sa008-static-mesh-clear-collections")))
	{
		return false;
	}
	TestTrue(
		TEXT("static mesh collection clear produces exact empty state"),
		Mesh->GetStaticMaterials().IsEmpty() && Mesh->Sockets.IsEmpty());
	TestEqual(TEXT("static mesh persists four exact updates"), Durable->PersistCount, 4);
	TestEqual(
		TEXT("static mesh acknowledges four updates only after C9"),
		Durable->AcknowledgeCount,
		4);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataDataTableProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataDataTableProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataDataTableProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		AddError(ProfileError);
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.data_table.replace"));
	if (!TestNotNull(TEXT("data table replace profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("data table replacement contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{TEXT("row_struct_ref"), TEXT("rows")}
			&& Profile->Fields.Num() == 2
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-data-table");
	Owner.BindRef(TargetRef, TEXT("SA008_DataTable"));
	UDataTable* Table = NewObject<UDataTable>(
		Owner.Package,
		TEXT("SA008_DataTable"),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Table)
	{
		return false;
	}
	Table->RowStruct = FUeAgentAssetDataSimpleEffectTableRow::StaticStruct();
	FUeAgentAssetDataSimpleEffectTableRow OriginalRow;
	OriginalRow.Label = TEXT("Original");
	OriginalRow.Count = 3;
	OriginalRow.Weight = 0.25f;
	OriginalRow.bEnabled = false;
	Table->AddRow(TEXT("OriginalRow"), OriginalRow);
	Owner.Package->SetDirtyFlag(false);

	const FString RowStructPath =
		FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()->GetPathName();
	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Table),
				{
					BuildTypedChange(
						TEXT("row_struct_ref"),
						TEXT("replace"),
						TEXT("uai.ref.struct.v1"),
						MakeShared<FJsonValueString>(RowStructPath)),
					BuildTypedChange(
						TEXT("rows"),
						TEXT("replace"),
						TEXT("uai.asset.data_table.rows.v1"),
						BuildDataTableRowsValue()),
				}),
			TEXT("sa008-data-table-replace")))
	{
		return false;
	}
	const uint8* const* RowPtr = Table->GetRowMap().Find(TEXT("RowA"));
	const FUeAgentAssetDataSimpleEffectTableRow* TypedRow =
		RowPtr
			? reinterpret_cast<
				const FUeAgentAssetDataSimpleEffectTableRow*>(*RowPtr)
			: nullptr;
	TestTrue(
		TEXT("data table exact typed row is read from table storage"),
		Table->GetRowStruct()
				== FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()
			&& Table->GetRowMap().Num() == 1
			&& TypedRow
			&& TypedRow->Label == TEXT("Updated")
			&& TypedRow->Count == 7
			&& FMath::IsNearlyEqual(TypedRow->Weight, 1.5f)
			&& TypedRow->bEnabled);

	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Table),
				{
					BuildTypedChange(
						TEXT("row_struct_ref"),
						TEXT("replace"),
						TEXT("uai.ref.struct.v1"),
						MakeShared<FJsonValueString>(RowStructPath)),
					BuildTypedChange(
						TEXT("rows"),
						TEXT("replace"),
						TEXT("uai.asset.data_table.rows.v1"),
						BuildObjectArrayValue({})),
				}),
			TEXT("sa008-data-table-replace-empty")))
	{
		return false;
	}
	TestTrue(
		TEXT("data table empty replacement remains a real exact replacement"),
		Table->GetRowStruct()
				== FUeAgentAssetDataSimpleEffectTableRow::StaticStruct()
			&& Table->GetRowMap().IsEmpty());
	TestEqual(TEXT("data table persists two exact replacements"), Durable->PersistCount, 2);
	TestEqual(
		TEXT("data table acknowledges two replacements only after C9"),
		Durable->AcknowledgeCount,
		2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataSkeletalMeshProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"AssetDataSkeletalMeshProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentSimpleAuthoringAssetDataSkeletalMeshProductionContractTest::
	RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!Fixture.IsValid()
		|| !TryGetAssetDataSimpleEffectProductionProfiles(
			Profiles,
			ProfileError))
	{
		AddError(ProfileError);
		return false;
	}
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		FindProductionProfile(
			Profiles,
			TEXT("uai.operation.asset.skeletal_mesh.update"));
	if (!TestNotNull(TEXT("skeletal mesh update profile exists"), Profile))
	{
		return false;
	}
	TestTrue(
		TEXT("skeletal mesh replacement contract remains exact"),
		Profile->FieldIds
				== TArray<FString>{
					TEXT("materials"),
					TEXT("lod_settings"),
					TEXT("bounds"),
					TEXT("skeleton_ref"),
				}
			&& Profile->Fields.Num() == 4
			&& Profile->Fields[0].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[1].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[2].AllowedPresences
				== TArray<FString>{TEXT("replace")}
			&& Profile->Fields[3].AllowedPresences
				== TArray<FString>{TEXT("replace")});

	FOwnedAssetDataCreatePackage Owner;
	if (!Owner.IsValid())
	{
		return false;
	}
	const FString TargetRef = TEXT("uai://asset/sa008-skeletal-mesh");
	const FString MaterialRef = TEXT("uai://asset/sa008-skeletal-material");
	const FString LodSettingsRef = TEXT("uai://asset/sa008-lod-settings");
	const FString SkeletonRef = TEXT("uai://asset/sa008-skeleton");
	Owner.BindRef(TargetRef, TEXT("SA008_SkeletalMesh"));
	Owner.BindRef(MaterialRef, TEXT("SA008_SkeletalMaterial"));
	Owner.BindRef(LodSettingsRef, TEXT("SA008_LodSettings"));
	Owner.BindRef(SkeletonRef, TEXT("SA008_Skeleton"));
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>(
		Owner.Package,
		TEXT("SA008_SkeletalMesh"),
		RF_Public | RF_Standalone | RF_Transactional);
	UMaterial* Material = NewObject<UMaterial>(
		Owner.Package,
		TEXT("SA008_SkeletalMaterial"),
		RF_Public | RF_Standalone | RF_Transactional);
	USkeletalMeshLODSettings* LodSettings =
		NewObject<USkeletalMeshLODSettings>(
			Owner.Package,
			TEXT("SA008_LodSettings"),
			RF_Public | RF_Standalone | RF_Transactional);
	USkeleton* Skeleton = NewObject<USkeleton>(
		Owner.Package,
		TEXT("SA008_Skeleton"),
		RF_Public | RF_Standalone | RF_Transactional);
	USkeletalMeshLODSettings* OriginalLodSettings =
		NewObject<USkeletalMeshLODSettings>(
			Owner.Package,
			TEXT("SA008_OriginalLodSettings"),
			RF_Transactional);
	USkeleton* OriginalSkeleton = NewObject<USkeleton>(
		Owner.Package,
		TEXT("SA008_OriginalSkeleton"),
		RF_Transactional);
	if (!Mesh
		|| !Material
		|| !LodSettings
		|| !Skeleton
		|| !OriginalLodSettings
		|| !OriginalSkeleton)
	{
		return false;
	}
	Mesh->SetMaterials({
		FSkeletalMaterial(
			nullptr,
			FName(TEXT("Original")),
			FName(TEXT("Original"))),
	});
	Mesh->SetImportedBounds(
		FBoxSphereBounds(
			FBox(
				FVector(-1.0, -2.0, -3.0),
				FVector(1.0, 2.0, 3.0))));
	Mesh->SetLODSettings(OriginalLodSettings);
	Mesh->SetSkeleton(OriginalSkeleton);
	Owner.Package->SetDirtyFlag(false);

	const TSharedRef<FRecordingDurableService, ESPMode::ThreadSafe> Durable =
		MakeShared<FRecordingDurableService, ESPMode::ThreadSafe>();
	if (!SubmitVerifiedAssetDataUpdate(
			*this,
			Fixture,
			Owner,
			Durable,
			*Profile,
			BuildUpdatePayload(
				TargetRef,
				BuildAssetRevision(Mesh),
				{
					BuildTypedChange(
						TEXT("materials"),
						TEXT("replace"),
						TEXT("uai.asset.mesh.material_slots.v1"),
						BuildObjectArrayValue({
							BuildMaterialSlotValue(TEXT("Body"), MaterialRef),
						})),
					BuildTypedChange(
						TEXT("lod_settings"),
						TEXT("replace"),
						TEXT("uai.asset.skeletal_mesh.lod_settings.v1"),
						BuildLodSettingsValue(LodSettingsRef)),
					BuildTypedChange(
						TEXT("bounds"),
						TEXT("replace"),
						TEXT("uai.value.box3d.v1"),
						BuildBoundsValue(
							FVector(-10.0, -20.0, -30.0),
							FVector(10.0, 20.0, 30.0))),
					BuildTypedChange(
						TEXT("skeleton_ref"),
						TEXT("replace"),
						TEXT("uai.ref.asset.v1"),
						MakeShared<FJsonValueString>(SkeletonRef)),
				}),
			TEXT("sa008-skeletal-mesh-replace")))
	{
		return false;
	}
	TestTrue(
		TEXT("skeletal mesh exact imported-space content is read from UObject"),
		Mesh->GetMaterials().Num() == 1
			&& Mesh->GetMaterials()[0].MaterialSlotName == TEXT("Body")
			&& Mesh->GetMaterials()[0].MaterialInterface == Material
			&& Mesh->GetLODSettings() == LodSettings
			&& Mesh->GetImportedBounds().GetBox().Equals(
				FBox(
					FVector(-10.0, -20.0, -30.0),
					FVector(10.0, 20.0, 30.0)))
			&& Mesh->GetSkeleton() == Skeleton);
	TestEqual(TEXT("skeletal mesh persists exactly once"), Durable->PersistCount, 1);
	TestEqual(
		TEXT("skeletal mesh acknowledges only after C9"),
		Durable->AcknowledgeCount,
		1);
	return !HasAnyErrors();
}

namespace
{
	using UE::UAI::Tests::SimpleAuthoring::E4::
		CountActiveTransactions;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		CountCompletedJournals;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableAssetPackage;
	using UE::UAI::Tests::SimpleAuthoring::E4::
		FOwnedReloadableAssetResolver;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetDataOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4."
		"AssetDataOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringAssetDataOwnedPackageTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TArray<FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	FString ProfileError;
	if (!TestTrue(
			TEXT("formal AssetData catalog projects E4 profiles"),
			TryGetAssetDataSimpleEffectProductionProfiles(
				Profiles,
				ProfileError)))
	{
		return false;
	}
	const FString Capability = TEXT("asset.sound_attenuation.create");
	const FUeAgentAssetDataSimpleEffectProductionProfile* Profile =
		Profiles.FindByPredicate(
			[&Capability](
				const FUeAgentAssetDataSimpleEffectProductionProfile&
					Candidate)
			{
				return Candidate.CapabilityId == Capability;
			});
	if (!TestNotNull(
			TEXT("AssetData E4 representative profile exists"),
			Profile))
	{
		return false;
	}

	const FString TargetRef = TEXT("uai://asset/sa008-asset-data-e4");
	const FName ObjectName(TEXT("AssetE4"));
	FString OwnedRoot;
	{
		FOwnedReloadableAssetPackage Owner(TEXT("AssetDataE4"));
		OwnedRoot = Owner.Root;
		Owner.BindTarget(TargetRef, ObjectName);
		if (!TestTrue(
				TEXT("owned durable AssetData package is valid"),
				Owner.IsValid()))
		{
			return false;
		}

		const TSharedRef<
			FOwnedReloadableAssetResolver,
			ESPMode::ThreadSafe> MutationResolver = MakeShared<
				FOwnedReloadableAssetResolver,
				ESPMode::ThreadSafe>(Owner, false);
		const TSharedRef<
			FOwnedReloadableAssetResolver,
			ESPMode::ThreadSafe> PreStateResolver = MakeShared<
				FOwnedReloadableAssetResolver,
				ESPMode::ThreadSafe>(Owner, false);
		const TSharedRef<
			FOwnedReloadableAssetResolver,
			ESPMode::ThreadSafe> PersistedResolver = MakeShared<
				FOwnedReloadableAssetResolver,
				ESPMode::ThreadSafe>(Owner, true);
		const TSharedRef<
			FFixtureFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryResolver = MakeShared<
				FFixtureFactoryCandidateResolver,
				ESPMode::ThreadSafe>(Fixture);

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Owner.JournalRoot;
		DurableConfig.PackageBackend = MakeShared<
			FPackageAssetDurableBackend,
			ESPMode::ThreadSafe>(Owner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
			Durable;
		FUeAgentSimpleAuthoringProductionError DurableError;
		if (!TestTrue(
				TEXT("real AssetData package C8D service composes"),
				FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
					DurableConfig,
					Durable,
					DurableError))
			|| !Durable.IsValid())
		{
			AddError(
				TEXT("sa008_asset_data_e4_durable_invalid:")
					+ DurableError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute(Capability);
		if (!TestTrue(
				TEXT("Simple Authoring descriptors compose for AssetData E4"),
				TryCreateSimpleAuthoringDescriptorRegistry(
					Registry,
					RegistryError))
			|| !TestNotNull(
				TEXT("AssetData E4 has one exact route"),
				Route)
			|| !Registry.IsValid())
		{
			return false;
		}

		FUeAgentAssetDataSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.MutationAssetResolver = MutationResolver;
		ProviderConfig.PreStateAssetResolver = PreStateResolver;
		ProviderConfig.PersistedAssetResolver = PersistedResolver;
		ProviderConfig.FactoryResolver = FactoryResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa008-asset-data-e4");
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			FUeAgentPageRequest(),
			TOptional<int64>(),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		FUeAgentAssetDataSimpleAuthoringError ProviderError;
		if (!TestTrue(
				TEXT("AssetData E4 family composes"),
				FUeAgentAssetDataSimpleAuthoringProviderFactory::TryCreate(
					ProviderConfig,
					Provider,
					ProviderError))
			|| !Provider.IsValid())
		{
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-asset-data-e4-integrity"),
			60.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("ticket:sa008-asset-data-e4")); },
			TicketService,
			TicketError);
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FTokenEstimator>(),
			Projector,
			ProjectionError);
		if (!TicketService.IsValid() || !Projector.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:sa008-asset-data-e4");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-asset-data-e4");
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Editor;
		ApplicationConfig.ExecutionContext.ContextRevision =
			TEXT("project:sa008-asset-data-e4:r1");
		ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:sa008-asset-data-e4");
		ApplicationConfig.RegistryEpoch = 1;
		ApplicationConfig.TimeoutSeconds = 60.0;
		ApplicationConfig.MaxFrameBytes = 4 * 1024 * 1024;
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!TestTrue(
				TEXT("application accepts AssetData E4 route"),
				FUeAgentSimpleAuthoringApplication::TryCreate(
					ApplicationConfig,
					{*Route},
					{Provider.ToSharedRef()},
					TicketService.ToSharedRef(),
					Projector.ToSharedRef(),
					Application,
					ApplicationError))
			|| !Application.IsValid())
		{
			return false;
		}

		FString ExpectedClass;
		const TSharedPtr<FJsonObject> Payload = BuildCreatePayload(
			TargetRef,
			*Profile,
			Fixture,
			ExpectedClass);
		FString PayloadJson;
		if (!TestTrue(
				TEXT("AssetData E4 payload serializes"),
				Payload.IsValid()
					&& Serialize(Payload, PayloadJson)))
		{
			return false;
		}
		const FUeAgentOperationResult Result = Application->Submit(
			FUeAgentOperationRequest(
				Route->DescriptorIdentity,
				TEXT("request:sa008-asset-data-e4"),
				TEXT("idempotency:sa008-asset-data-e4"),
				FUeAgentContractValue(PayloadJson)),
			FUeAgentCallerContext(
				TEXT("caller:sa008-asset-data-e4"),
				{TEXT("uai:asset-data:write")},
				TEXT("correlation:sa008-asset-data-e4")),
			MakeShared<FNeverCancelled>());
		if (!TestTrue(
				TEXT("AssetData E4 completes after reload readback"),
				IsComplete(Result)))
		{
			AddError(Result.GetValue().GetJsonValue());
			return false;
		}

		UObject* Reloaded = Owner.FindTarget(TargetRef);
		TestTrue(TEXT("AssetData E4 uses a reloaded package owner"), Owner.WasReloaded());
		TestTrue(TEXT("AssetData E4 collects the original object"), Owner.WasOriginalObjectCollected());
		TestTrue(TEXT("AssetData E4 publishes the owned asset package"), IFileManager::Get().FileExists(*Owner.HeaderPath()));
		if (TestNotNull(TEXT("reloaded AssetData object is independently found"), Reloaded))
		{
			TestEqual(TEXT("reloaded AssetData object preserves the exact class"), Reloaded->GetClass()->GetPathName(), ExpectedClass);
			TestEqual(TEXT("reloaded AssetData object preserves the exact package"), Reloaded->GetOutermost()->GetName(), Owner.PackageName);
		}
		TestTrue(TEXT("AssetData E4 C9 uses the dedicated persisted resolver"), PersistedResolver->ResolveExistingCount >= 1);
		TestEqual(TEXT("AssetData E4 archives one verified journal"), CountCompletedJournals(Owner.JournalRoot), 1);
		TestEqual(TEXT("AssetData E4 leaves no active transaction"), CountActiveTransactions(Owner.JournalRoot), 0);
	}
	TestFalse(
		TEXT("AssetData E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return !HasAnyErrors();
}
