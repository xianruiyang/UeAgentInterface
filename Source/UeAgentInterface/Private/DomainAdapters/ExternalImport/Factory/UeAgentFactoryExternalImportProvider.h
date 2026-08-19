#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainAdapters::ExternalImport::Factory
{
	enum class EFactoryExternalImportOperation : uint8
	{
		Import,
		Reimport,
	};

	enum class EFactoryExternalImportKind : uint8
	{
		FbxAnimation,
		FbxSkeletalMesh,
		GeometryCache,
		LightProfile,
		SoundWave,
		SparseVolumeTexture,
		StaticMesh,
		Texture,
	};

	struct UEAGENTINTERFACE_API FFactoryExternalImportOptions final
	{
		bool bReplaceExisting = false;
		bool bImportMaterials = false;
		bool bImportTextures = false;
		bool bCreatePhysicsAsset = false;
		bool bImportAnimations = false;
		bool bImportMorphTargets = false;
		bool bUpdateSkeletonReferencePose = false;
		bool bValidateAfterImport = true;
		TOptional<bool> Srgb;
		FString CompressionSettings;
		FString MipGenSettings;
		FString LodGroup;
		TArray<FString> ExpectedMorphTargets;
		TArray<FString> ExpectedMaterialSlots;
		int32 ExpectedLodCount = INDEX_NONE;
		FString Fingerprint;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FFactoryExternalImportSpec final
	{
		FString RequestId;
		EFactoryExternalImportOperation Operation =
			EFactoryExternalImportOperation::Import;
		EFactoryExternalImportKind Kind = EFactoryExternalImportKind::Texture;
		FString SourceRef;
		FString DestinationRef;
		FString SkeletonRef;
		FFactoryExternalImportOptions Options;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FFactoryExternalImportReadback final
	{
		FString Operation;
		FString TargetIdentity;
		FString AssetClass;
		FString SourceRef;
		FString SourceSha256;
		FString ImportDataClass;
		FString OptionsFingerprint;
		FString BusinessStateKind;
		int32 BusinessItemCount = 0;
		TArray<FString> DependencyIdentities;
		TOptional<bool> Srgb;
		FString CompressionSettings;
		FString MipGenSettings;
		FString LodGroup;
		TArray<FString> MorphTargetNames;
		TArray<FString> MaterialSlotNames;
		int32 LodCount = INDEX_NONE;
		bool bReloaded = false;
	};

	struct UEAGENTINTERFACE_API FFactoryExternalImportError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsPresent() const { return !Code.IsEmpty(); }
	};

	class UEAGENTINTERFACE_API IFactoryExternalImportSession
	{
	public:
		virtual ~IFactoryExternalImportSession() = default;

		virtual const FString& GetTargetIdentity() const = 0;
		virtual const FString& GetExpectedRevision() const = 0;
		virtual const FString& GetSourceSha256() const = 0;
		virtual bool CheckPreExecute(
			FFactoryExternalImportError& OutError) = 0;
		virtual bool Execute(FFactoryExternalImportError& OutError) = 0;
		virtual bool DidMutationStart() const = 0;
		virtual bool Settle(FFactoryExternalImportError& OutError) = 0;
		virtual bool CheckPrePersist(
			FFactoryExternalImportError& OutError) = 0;
		virtual bool Persist(
			FString& OutPersistenceReceiptId,
			FFactoryExternalImportError& OutError) = 0;
		virtual bool Verify(
			FFactoryExternalImportReadback& OutReadback,
			FFactoryExternalImportError& OutError) = 0;
		virtual bool Rollback(
			bool& bOutConfirmed,
			FFactoryExternalImportError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IFactoryExternalImportProvider
	{
	public:
		virtual ~IFactoryExternalImportProvider() = default;

		virtual bool Prepare(
			const FFactoryExternalImportSpec& Spec,
			TSharedPtr<IFactoryExternalImportSession>& OutSession,
			FFactoryExternalImportError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateFactoryExternalImportProvider(
		TSharedPtr<IFactoryExternalImportProvider>& OutProvider,
		FString& OutError);
}
