#include "DomainAdapters/WorldCommands/UeAgentWorldHlodHjsonMechanics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODLayerFactory.h"

namespace UE::UAI::DomainAdapters::WorldCommands
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString ProviderId = TEXT("world-hlod-domain-provider");
		const FString SchemaId = TEXT("uai.world_hlod_layer.edit/1");
		const FString RootType = TEXT("/Script/Engine.HLODLayer");
		const FDomainHjsonDocumentContract DocumentContract(
			TEXT("world-hlod"), SchemaId, RootType);

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Subject = FString(),
			FString Detail = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Subject), MoveTemp(Detail));
			return false;
		}

		bool TryParseLayerType(
			const FString& Text,
			EHLODLayerType& OutType)
		{
			if (Text.Equals(TEXT("Instancing"), ESearchCase::IgnoreCase))
			{
				OutType = EHLODLayerType::Instancing;
				return true;
			}
			if (Text.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase))
			{
				OutType = EHLODLayerType::MeshMerge;
				return true;
			}
			if (Text.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase))
			{
				OutType = EHLODLayerType::MeshSimplify;
				return true;
			}
			if (Text.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase))
			{
				OutType = EHLODLayerType::MeshApproximate;
				return true;
			}
			if (Text.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
			{
				OutType = EHLODLayerType::Custom;
				return true;
			}
			return false;
		}

		bool TryRead(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			TSharedPtr<FJsonObject>& OutSnapshot,
			FWorkspaceError& OutError)
		{
			UObject* Object = nullptr;
			FUeAgentContentGraphCapture Capture;
			if (!TryCaptureObject(
				Target, RootType, Object, Capture, OutError))
			{
				return false;
			}
			OutRevision = Capture.Revision;
			if (!TryBuildCaptureSnapshot(
				Target, RootType, Capture, OutSnapshot, OutError))
			{
				return false;
			}
			return TryCanonicalizeObject(OutSnapshot, OutPayload, OutError);
		}

		UHLODLayer* TryLoadLayer(const FString& Target)
		{
			return Cast<UHLODLayer>(StaticLoadObject(
				UHLODLayer::StaticClass(), nullptr,
				*ObjectPathFromPackageIdentity(Target)));
		}

		bool TryApplyNumericProperty(
			UHLODLayer& Layer,
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* JsonField,
			const TCHAR* PropertyName,
			const bool bInteger,
			FWorkspaceError& OutError)
		{
			double Value = 0.0;
			if (!Root->TryGetNumberField(JsonField, Value)) return true;
			if (!FMath::IsFinite(Value) || (bInteger && Value < 1.0))
			{
				return Fail(
					OutError, TEXT("world_hlod_numeric_value_invalid"), JsonField);
			}
			if (bInteger)
			{
				FIntProperty* Property = FindFProperty<FIntProperty>(
					UHLODLayer::StaticClass(), PropertyName);
				if (!Property)
				{
					return Fail(
						OutError, TEXT("world_hlod_property_unavailable"), PropertyName);
				}
				Property->SetPropertyValue_InContainer(
					&Layer, FMath::RoundToInt(Value));
			}
			else
			{
				FDoubleProperty* Property = FindFProperty<FDoubleProperty>(
					UHLODLayer::StaticClass(), PropertyName);
				if (!Property)
				{
					return Fail(
						OutError, TEXT("world_hlod_property_unavailable"), PropertyName);
				}
				Property->SetPropertyValue_InContainer(&Layer, Value);
			}
			return true;
		}

		bool TryCreateLayer(
			const FString& Target,
			UHLODLayer*& OutLayer,
			FWorkspaceError& OutError)
		{
			OutLayer = nullptr;
			if (!FPackageName::IsValidLongPackageName(Target))
			{
				return Fail(
					OutError, TEXT("world_hlod_target_package_invalid"), Target);
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(Target);
			const FString PackagePath = FPackageName::GetLongPackagePath(Target);
			if (AssetName.IsEmpty() || PackagePath.IsEmpty())
			{
				return Fail(
					OutError, TEXT("world_hlod_target_package_invalid"), Target);
			}
			UHLODLayerFactory* Factory = NewObject<UHLODLayerFactory>();
			OutLayer = Cast<UHLODLayer>(
				FAssetToolsModule::GetModule().Get().CreateAsset(
					AssetName,
					PackagePath,
					UHLODLayer::StaticClass(),
					Factory));
			if (!OutLayer)
			{
				return Fail(
					OutError, TEXT("world_hlod_create_failed"), Target);
			}
			return true;
		}

		bool TrySaveAndReload(
			const FString& Target,
			UHLODLayer*& Layer,
			FWorkspaceError& OutError)
		{
			UPackage* Package = Layer ? Layer->GetOutermost() : nullptr;
			if (!Package)
			{
				return Fail(OutError, TEXT("world_hlod_package_unavailable"), Target);
			}
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Target, FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Layer, *Filename, SaveArgs))
			{
				return Fail(OutError, TEXT("world_hlod_save_failed"), Target);
			}
			FAssetRegistryModule::AssetCreated(Layer);
			Layer = nullptr;
			TArray<UPackage*> Packages{Package};
			FText UnloadError;
			if (!UPackageTools::UnloadPackages(Packages, UnloadError, false))
			{
				return Fail(
					OutError, TEXT("world_hlod_unload_failed"), Target,
					UnloadError.ToString());
			}
			CollectGarbage(RF_NoFlags);
			Layer = TryLoadLayer(Target);
			if (!Layer)
			{
				return Fail(OutError, TEXT("world_hlod_reload_failed"), Target);
			}
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
				Target, CanonicalParamsJson, DocumentContract, Document, OutError))
			{
				return false;
			}
			UHLODLayer* Layer = TryLoadLayer(Target);
			const bool bCreated = Layer == nullptr;
			if (bCreated)
			{
				if (Document.ExpectedRevision != TEXT("absent"))
				{
					return Fail(
						OutError, TEXT("world_hlod_expected_absent_required"), Target);
				}
				if (!TryCreateLayer(Target, Layer, OutError)) return false;
			}
			else
			{
				FString BeforePayload;
				FString Revision;
				TSharedPtr<FJsonObject> BeforeSnapshot;
				if (!TryRead(
					Target, BeforePayload, Revision, BeforeSnapshot, OutError))
				{
					return false;
				}
				if (Revision != Document.ExpectedRevision)
				{
					return Fail(
						OutError, TEXT("world_hlod_stale_revision"), Target, Revision);
				}
			}

			FString LayerTypeText;
			EHLODLayerType LayerType = EHLODLayerType::Instancing;
			if (Document.Root->TryGetStringField(TEXT("layer_type"), LayerTypeText)
				&& !TryParseLayerType(LayerTypeText, LayerType))
			{
				return Fail(
					OutError, TEXT("world_hlod_layer_type_invalid"), LayerTypeText);
			}
			FString ParentPath;
			UHLODLayer* Parent = nullptr;
			if (Document.Root->TryGetStringField(TEXT("parent_layer"), ParentPath)
				&& !ParentPath.IsEmpty())
			{
				Parent = TryLoadLayer(ParentPath);
				if (!Parent)
				{
					return Fail(
						OutError, TEXT("world_hlod_parent_unresolved"), ParentPath);
				}
			}

			Layer->Modify();
			Layer->PreEditChange(nullptr);
			if (!LayerTypeText.IsEmpty()) Layer->SetLayerType(LayerType);
			bool bSpatiallyLoaded = true;
			if (Document.Root->TryGetBoolField(
				TEXT("is_spatially_loaded"), bSpatiallyLoaded))
			{
				Layer->SetIsSpatiallyLoaded(bSpatiallyLoaded);
			}
			if (!TryApplyNumericProperty(
				*Layer, Document.Root, TEXT("cell_size"), TEXT("CellSize"), true,
				OutError)
				|| !TryApplyNumericProperty(
					*Layer, Document.Root, TEXT("loading_range"),
					TEXT("LoadingRange"), false, OutError))
			{
				return false;
			}
			if (Document.Root->HasField(TEXT("parent_layer")))
			{
				Layer->SetParentLayer(Parent);
			}
			Layer->PostEditChange();
			Layer->MarkPackageDirty();
			if (!TrySaveAndReload(Target, Layer, OutError)) return false;

			FString ReadbackPayload;
			FString Revision;
			TSharedPtr<FJsonObject> Readback;
			if (!TryRead(
				Target, ReadbackPayload, Revision, Readback, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
			Mutation->SetStringField(
				TEXT("kind"), bCreated ? TEXT("created") : TEXT("updated"));
			Mutation->SetStringField(TEXT("root_type"), RootType);
			FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
			LifecycleReceipt.bMutationApplied = true;
			LifecycleReceipt.bCompileSettled = true;
			LifecycleReceipt.bPackageSaved = true;
			LifecycleReceipt.bUnloadedReloaded = true;
			LifecycleReceipt.PersistenceReceiptId = Revision;
			return TryBuildImportSuccessPayload(
				ProviderId, Target, Revision, LifecycleReceipt, Readback, Mutation,
				OutPayload, OutError);
		}
	}

	bool TryExecuteHlodContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				TargetIdentity, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		if (CommandId.StartsWith(TEXT("content_graph.")))
		{
			FString Revision;
			TSharedPtr<FJsonObject> Snapshot;
			bOutReadOnly = true;
			return TryRead(
				TargetIdentity, OutPayloadJson, Revision, Snapshot, OutError);
		}
		return Fail(
			OutError, TEXT("world_hlod_command_not_supported"), CommandId);
	}
}
