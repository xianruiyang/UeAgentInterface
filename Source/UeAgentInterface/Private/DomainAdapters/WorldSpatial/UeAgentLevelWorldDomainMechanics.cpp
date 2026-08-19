#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringRegistration.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/WorldFactory.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/UeAgentSpaceContract.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
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

	namespace
	{
		const FString SchemaId = TEXT("uai.world.edit/1");

		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
		{
		public:
			virtual bool IsCancelled() const override { return false; }
		};

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Source = FString(),
			FString Pointer = TEXT("/"))
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Source), MoveTemp(Pointer));
			return false;
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

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 32768},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> JsonValueFromHjson(
			const FHjsonNode& Node,
			FWorkspaceError& OutError)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return MakeShared<FJsonValueNull>();
			case EHjsonNodeKind::Boolean:
				return MakeShared<FJsonValueBoolean>(Node.GetBoolean());
			case EHjsonNodeKind::String:
				return MakeShared<FJsonValueString>(Node.GetText());
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return MakeShared<FJsonValueNumber>(FCString::Atod(*Node.GetLexeme()));
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Item, OutError);
					if (!Converted.IsValid()) return nullptr;
					Items.Add(MoveTemp(Converted));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					TSharedPtr<FJsonValue> Converted =
						JsonValueFromHjson(*Entry.Value, OutError);
					if (!Converted.IsValid()) return nullptr;
					Object->SetField(Entry.Key, MoveTemp(Converted));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				Fail(OutError, TEXT("level_world_domain_hjson_node_invalid"));
				return nullptr;
			}
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			FString Error;
			if (!Object.IsValid() || !Json::TryCanonicalize(Object, Out, Error))
			{
				return Fail(OutError,
					TEXT("level_world_domain_json_canonicalization_failed"), Error);
			}
			return true;
		}

		bool EnsureAuthoringRegistered(FWorkspaceError& OutError)
		{
			static const TPair<bool, FString> Registration = []()
			{
				FString Error;
				const bool bRegistered =
					UeAgentLevelAuthoring::RegisterAuthoring(Error);
				return TPair<bool, FString>(bRegistered, MoveTemp(Error));
			}();
			if (!Registration.Key)
			{
				return Fail(OutError,
					TEXT("level_world_domain_authoring_registration_failed"),
					Registration.Value);
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
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutDocument,
			FWorkspaceError& OutError)
		{
			OutDocument.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(OutError,
					TEXT("level_world_domain_legacy_source_contract_rejected"));
			}
			FString Target;
			const TSharedPtr<FJsonObject>* Source = nullptr;
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Target.IsEmpty()
				|| !Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid()
				|| !(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != Target
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_resolved_source_invalid"));
			}
			FString Text;
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
					? Value->AsObject() : nullptr;
				FString RelativePath;
				FString DocumentText;
				if (!DocumentObject.IsValid()
					|| !DocumentObject->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !DocumentObject->TryGetStringField(
						TEXT("document"), DocumentText))
				{
					return Fail(OutError,
						TEXT("level_world_domain_resolved_source_invalid"));
				}
				if (RelativePath == EntryFile)
				{
					Text = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || Text.IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_resolved_source_invalid"), EntryFile);
			}
			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(Text, TEXT("inline:level-world-domain"),
					Resources, Document, OutError)
				|| !Document.IsValid())
			{
				return false;
			}
			TSharedPtr<FJsonValue> Value =
				JsonValueFromHjson(*Document->GetRoot(), OutError);
			OutDocument = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Schema;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			const TSharedPtr<FJsonObject>* RootDocument = nullptr;
			const TSharedPtr<FJsonObject>* DocumentTarget = nullptr;
			FString Type;
			FString AssetPath;
			if (!OutDocument.IsValid()
				|| !OutDocument->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != SchemaId
				|| !OutDocument->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid()
				|| !(*Root)->TryGetStringField(TEXT("type"), Type)
				|| Type != TEXT("/Script/Engine.World")
				|| !(*Root)->TryGetObjectField(TEXT("document"), RootDocument)
				|| !RootDocument || !RootDocument->IsValid()
				|| !(*RootDocument)->TryGetObjectField(
					TEXT("target"), DocumentTarget)
				|| !DocumentTarget || !DocumentTarget->IsValid()
				|| !(*DocumentTarget)->TryGetStringField(
					TEXT("asset_path"), AssetPath))
			{
				return Fail(OutError,
					TEXT("level_world_domain_schema_or_type_invalid"), Type);
			}
			if (AssetPath != Target)
			{
				return Fail(OutError,
					TEXT("level_world_domain_document_target_mismatch"),
					AssetPath, Target);
			}
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
			if (!Target.Split(TEXT("."), &OutPackage, &OutObject)
				|| !FPackageName::IsValidLongPackageName(OutPackage)
				|| OutObject.IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_target_invalid"), Target);
			}
			FString AssetName;
			OutPackage.Split(TEXT("/"), nullptr, &AssetName,
				ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (AssetName != OutObject)
			{
				return Fail(OutError,
					TEXT("level_world_domain_target_object_mismatch"), Target);
			}
			return true;
		}

		FString MapFilename(const FString& PackageName)
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName, FPackageName::GetMapPackageExtension());
		}

		bool StableRevision(
			const FString& PackageName,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *MapFilename(PackageName))
				|| Bytes.IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_persisted_map_missing"), PackageName);
			}
			OutRevision = Sha256(Bytes);
			return true;
		}

		struct FMapRestoreContext
		{
			FString OriginalPackage;
			bool bSwitched = false;

			bool Restore(FWorkspaceError& OutError)
			{
				if (!bSwitched || OriginalPackage.IsEmpty()) return true;
				if (!FPackageName::DoesPackageExist(OriginalPackage))
				{
					return Fail(OutError,
						TEXT("level_world_domain_original_map_missing"),
						OriginalPackage);
				}
				if (!UEditorLoadingAndSavingUtils::LoadMap(
						MapFilename(OriginalPackage)))
				{
					return Fail(OutError,
						TEXT("level_world_domain_original_map_restore_failed"),
						OriginalPackage);
				}
				bSwitched = false;
				return true;
			}
		};

		bool CreateWorldAsset(
			const FString& PackageName,
			const FString& ObjectName,
			const bool bWorldPartition,
			FWorkspaceError& OutError)
		{
			UPackage* Package = CreatePackage(*PackageName);
			UWorldFactory* Factory = NewObject<UWorldFactory>();
			if (!Package || !Factory)
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_factory_unavailable"),
					PackageName);
			}
			Factory->WorldType = EWorldType::Editor;
			Factory->bInformEngineOfWorld = false;
			Factory->bCreateWorldPartition = bWorldPartition;
			Factory->bEnableWorldPartitionStreaming = bWorldPartition;
			UWorld* World = Cast<UWorld>(Factory->FactoryCreateNew(
				UWorld::StaticClass(), Package, FName(*ObjectName),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!World)
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_create_failed"), PackageName);
			}
			FAssetRegistryModule::AssetCreated(World);
			World->MarkPackageDirty();
			if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_create_save_failed"), PackageName);
			}
			Package->SetDirtyFlag(false);
			World = nullptr;
			Factory = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_create_unload_failed"),
					PackageName, UnloadError.ToString());
			}
			return true;
		}

		bool OpenWorld(
			const FString& PackageName,
			const FString& ObjectName,
			const bool bAllowCreate,
			const bool bWorldPartition,
			FMapRestoreContext& OutRestore,
			UWorld*& OutWorld,
			FWorkspaceError& OutError)
		{
			OutWorld = nullptr;
			if (!GEditor)
			{
				return Fail(OutError, TEXT("level_world_domain_editor_unavailable"));
			}
			UWorld* Current = GEditor->GetEditorWorldContext().World();
			if (!Current)
			{
				return Fail(OutError, TEXT("level_world_domain_current_world_missing"));
			}
			const FString CurrentPackage = Current->GetOutermost()->GetName();
			if (CurrentPackage == PackageName)
			{
				OutWorld = Current;
				return true;
			}
			if (Current->GetOutermost()->IsDirty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_current_world_dirty"), CurrentPackage);
			}
			OutRestore.OriginalPackage = CurrentPackage;
			const bool bExists = FPackageName::DoesPackageExist(PackageName);
			if (!bExists)
			{
				if (!bAllowCreate)
				{
					return Fail(OutError,
						TEXT("level_world_domain_target_not_found"), PackageName);
				}
				if (!CreateWorldAsset(PackageName, ObjectName,
						bWorldPartition, OutError))
				{
					return false;
				}
			}
			OutWorld = UEditorLoadingAndSavingUtils::LoadMap(MapFilename(PackageName));
			if (!OutWorld)
			{
				return Fail(OutError,
					TEXT("level_world_domain_target_open_failed"), PackageName);
			}
			OutRestore.bSwitched = true;
			return true;
		}

		bool ValidateFrame(
			const TSharedPtr<const FJsonObject>& Extension,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Frame = nullptr;
			double Dimension = 0.0;
			FString Semantic;
			FString Write;
			FString Read;
			FString Unit;
			if (!Extension.IsValid()
				|| !Extension->TryGetObjectField(TEXT("frame"), Frame)
				|| !Frame || !Frame->IsValid()
				|| !(*Frame)->TryGetNumberField(TEXT("dimension"), Dimension)
				|| Dimension != 3.0
				|| !(*Frame)->TryGetStringField(TEXT("semantic_frame"), Semantic)
				|| !(*Frame)->TryGetStringField(TEXT("write_frame"), Write)
				|| !(*Frame)->TryGetStringField(TEXT("read_frame"), Read)
				|| !(*Frame)->TryGetStringField(TEXT("unit"), Unit)
				|| Semantic != TEXT("ue_world") || Write != TEXT("ue_world")
				|| Read != TEXT("ue_world") || Unit != TEXT("centimeter"))
			{
				return Fail(OutError,
					TEXT("level_world_domain_space_contract_invalid"));
			}
			return true;
		}

		TSharedRef<FJsonObject> TransformForLevel(
			const TSharedPtr<const FJsonObject>& Source,
			const bool bActor)
		{
			TSharedRef<FJsonObject> Result = CopyObject(Source);
			Result->SetObjectField(TEXT("space_contract"),
				UeAgentSpaceContract::Make3D(
					bActor ? TEXT("actor.transform")
						: TEXT("component.relative_transform"),
					bActor ? TEXT("ue_actor_world_transform")
						: TEXT("ue_scene_component_parent_relative_transform"),
					TEXT("ue_world"), TEXT("identity")));
			return Result;
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
			FWorkspaceError& OutError)
		{
			OutDataLayers = nullptr;
			if (!ValidateFrame(Extension, OutError)) return false;
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

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			if (Extension->HasField(TEXT("actors"))
				&& (!Extension->TryGetArrayField(TEXT("actors"), Actors) || !Actors))
			{
				return Fail(OutError,
					TEXT("level_world_domain_actors_invalid"));
			}
			int32 ActorIndex = 0;
			for (const TSharedPtr<FJsonValue>& ActorValue :
				Actors ? *Actors : TArray<TSharedPtr<FJsonValue>>())
			{
				const TSharedPtr<FJsonObject> Actor = ActorValue->AsObject();
				FString ManagedId;
				FString ActorClass;
				FString Operation = TEXT("upsert");
				if (!Actor.IsValid()
					|| !Actor->TryGetStringField(TEXT("managed_id"), ManagedId)
					|| ManagedId.TrimStartAndEnd().IsEmpty()
					|| !Actor->TryGetStringField(TEXT("type"), ActorClass)
					|| ActorClass.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError,
						TEXT("level_world_domain_actor_identity_invalid"));
				}
				Actor->TryGetStringField(TEXT("operation"), Operation);
				TSharedRef<FJsonObject> Unit = MakeShared<FJsonObject>();
				Unit->SetStringField(TEXT("operation"), Operation);
				TSharedRef<FJsonObject> UnitTarget = MakeShared<FJsonObject>();
				UnitTarget->SetStringField(TEXT("world_path"), Target);
				UnitTarget->SetStringField(TEXT("managed_id"), ManagedId);
				UnitTarget->SetStringField(TEXT("actor_class"), ActorClass);
				Unit->SetObjectField(TEXT("target"), UnitTarget);
				if (!CopyOptionalArray(Actor, TEXT("properties"), Unit))
				{
					return Fail(OutError,
						TEXT("level_world_domain_actor_properties_invalid"), ManagedId);
				}
				TSharedRef<FJsonObject> ProviderExtension = MakeShared<FJsonObject>();
				for (const TCHAR* Field : {TEXT("label"), TEXT("folder_path")})
				{
					FString Value;
					if (Actor->TryGetStringField(Field, Value))
						ProviderExtension->SetStringField(Field, Value);
				}
				const TSharedPtr<FJsonObject>* Transform = nullptr;
				if (Actor->TryGetObjectField(TEXT("transform"), Transform)
					&& Transform && Transform->IsValid())
				{
					ProviderExtension->SetObjectField(TEXT("transform"),
						TransformForLevel(*Transform, true));
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

				const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
				if (Actor->HasField(TEXT("components"))
					&& (!Actor->TryGetArrayField(TEXT("components"), Components)
						|| !Components))
				{
					return Fail(OutError,
						TEXT("level_world_domain_components_invalid"), ManagedId);
				}
				int32 ComponentIndex = 0;
				for (const TSharedPtr<FJsonValue>& ComponentValue :
					Components ? *Components : TArray<TSharedPtr<FJsonValue>>())
				{
					const TSharedPtr<FJsonObject> Component =
						ComponentValue->AsObject();
					FString Name;
					FString Class;
					FString ComponentOperation = TEXT("upsert");
					if (!Component.IsValid()
						|| !Component->TryGetStringField(TEXT("component_name"), Name)
						|| Name.TrimStartAndEnd().IsEmpty()
						|| !Component->TryGetStringField(TEXT("type"), Class)
						|| Class.TrimStartAndEnd().IsEmpty())
					{
						return Fail(OutError,
							TEXT("level_world_domain_component_identity_invalid"),
							ManagedId);
					}
					Component->TryGetStringField(TEXT("operation"), ComponentOperation);
					TSharedRef<FJsonObject> ComponentUnit = MakeShared<FJsonObject>();
					ComponentUnit->SetStringField(TEXT("operation"), ComponentOperation);
					TSharedRef<FJsonObject> ComponentTarget = MakeShared<FJsonObject>();
					ComponentTarget->SetStringField(TEXT("world_path"), Target);
					ComponentTarget->SetStringField(TEXT("owner_managed_id"), ManagedId);
					ComponentTarget->SetStringField(TEXT("component_name"), Name);
					ComponentTarget->SetStringField(TEXT("component_class"), Class);
					ComponentUnit->SetObjectField(TEXT("target"), ComponentTarget);
					if (!CopyOptionalArray(Component, TEXT("properties"), ComponentUnit))
					{
						return Fail(OutError,
							TEXT("level_world_domain_component_properties_invalid"), Name);
					}
					const TSharedPtr<FJsonObject>* ComponentAttachment = nullptr;
					if (Component->TryGetObjectField(TEXT("attachment"),
							ComponentAttachment)
						&& ComponentAttachment && ComponentAttachment->IsValid())
					{
						TSharedRef<FJsonObject> LevelAttachment =
							CopyObject(*ComponentAttachment);
						const TSharedPtr<FJsonObject>* RelativeTransform = nullptr;
						if ((*ComponentAttachment)->TryGetObjectField(
								TEXT("transform"), RelativeTransform)
							&& RelativeTransform && RelativeTransform->IsValid())
						{
							LevelAttachment->SetObjectField(TEXT("transform"),
								TransformForLevel(*RelativeTransform, false));
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
				}
				++ActorIndex;
			}
			if (Extension->HasField(TEXT("data_layers"))
				&& (!Extension->TryGetArrayField(TEXT("data_layers"), OutDataLayers)
					|| !OutDataLayers))
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layers_invalid"));
			}
			OutBundle->SetObjectField(TEXT("units"), Units);
			return true;
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
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
					Bytes, OutRequest, Diagnostic)
				|| !OutRequest.IsSet())
			{
				return Fail(OutError,
					TEXT("level_world_domain_internal_request_invalid"),
					Diagnostic.Code, Diagnostic.Message);
			}
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
			FString Error;
			if (!Module::CommandRegistration::Active::
					TryComposeProcessActiveCommandComposition(
						Registry, OutDispatch, Error)
				|| !Registry.IsValid() || !OutDispatch.IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_command_registry_unavailable"), Error);
			}
			const CommandContract::FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				return Fail(OutError,
					TEXT("level_world_domain_command_contract_missing"), CommandId);
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
			FString Error;
			if (!OperationOwners::WorldCommands::TryResolveMechanics(
					CommandId, Mechanics, Scopes, Error)
				|| !Mechanics.IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_mechanics_unavailable"),
					CommandId, Error);
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
				return Fail(OutError,
					TEXT("level_world_domain_world_mutation_failed"), CommandId,
					Result.GetErrorJson());
			}
			FString JsonError;
			if (!Json::TryParseObject(Result.GetPayloadJson(), OutPayload, JsonError)
				|| !OutPayload.IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_world_mutation_payload_invalid"),
					CommandId, JsonError);
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
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_read_failed"),
					Result.GetErrorJson());
			}
			FString Error;
			if (!Json::TryParseObject(Result.GetPayloadJson(), OutPayload, Error)
				|| !OutPayload.IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_read_invalid"), Error);
			}
			return true;
		}

		bool ReadDataLayerRefsViaPublicOwner(
			UWorld& World,
			TSet<FString>& OutRefs,
			FWorkspaceError& OutError)
		{
			OutRefs.Reset();
			TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetStringField(TEXT("property_path"),
				TEXT("DataLayerInstances"));
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("target"),
				World.GetPathName() + TEXT(":PersistentLevel.WorldDataLayers"));
			Params->SetObjectField(TEXT("input"), Input);
			TOptional<FUeAgentCommandRequest> Request;
			TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
			if (!ComposeRequest(TEXT("world.property"), Params,
					Request, Dispatch, OutError))
				return false;
			const FUeAgentCommandResult Result = Dispatch->Submit(
				Request.GetValue(),
				Contract::FUeAgentCallerContext(TEXT("level-world-domain-query"),
					{TEXT("uai:query")}, Request->GetCorrelationId()),
				MakeShared<FNeverCancelled>());
			if (!Result.IsOk())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_reference_read_failed"),
					Result.GetErrorJson());
			}
			TSharedPtr<FJsonObject> Payload;
			FString JsonError;
			if (!Json::TryParseObject(Result.GetPayloadJson(), Payload, JsonError)
				|| !Payload.IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_reference_read_invalid"),
					JsonError);
			}
			const TSharedPtr<FJsonObject>* QueryResult = nullptr;
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* ProjectionPayload = nullptr;
			const TSharedPtr<FJsonObject>* Snapshot = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Payload->TryGetObjectField(TEXT("query_result"), QueryResult)
				|| !QueryResult || !QueryResult->IsValid()
				|| !(*QueryResult)->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"),
					ProjectionPayload)
				|| !ProjectionPayload || !ProjectionPayload->IsValid()
				|| !(*ProjectionPayload)->TryGetObjectField(TEXT("snapshot"), Snapshot)
				|| !Snapshot || !Snapshot->IsValid()
				|| !(*Snapshot)->TryGetArrayField(TEXT("entries"), Entries)
				|| !Entries || Entries->IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_reference_snapshot_missing"));
			}
			const TSharedPtr<FJsonObject> Entry = (*Entries)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FString Text;
			if (!Entry.IsValid()
				|| !Entry->TryGetObjectField(TEXT("value"), Value)
				|| !Value || !Value->IsValid()
				|| !(*Value)->TryGetStringField(TEXT("text"), Text))
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_reference_value_missing"));
			}
			FRegexMatcher Matcher(FRegexPattern(TEXT("'([^']+)'")), Text);
			while (Matcher.FindNext())
			{
				OutRefs.Add(Matcher.GetCaptureGroup(1));
			}
			return true;
		}

		bool ExtractCreatedSelector(
			const TSharedPtr<FJsonObject>& Payload,
			FString& OutSelector,
			FWorkspaceError& OutError)
		{
			OutSelector.Reset();
			const TSharedPtr<FJsonObject>* Projection = nullptr;
			const TSharedPtr<FJsonObject>* ProjectionPayload = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
			if (!Payload.IsValid()
				|| !Payload->TryGetObjectField(TEXT("projection"), Projection)
				|| !Projection || !Projection->IsValid()
				|| !(*Projection)->TryGetObjectField(TEXT("payload"), ProjectionPayload)
				|| !ProjectionPayload || !ProjectionPayload->IsValid()
				|| !(*ProjectionPayload)->TryGetArrayField(TEXT("targets"), Targets)
				|| !Targets || Targets->IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_selector_missing"));
			}
			const TSharedPtr<FJsonObject> Target = (*Targets)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Identity = nullptr;
			if (!Target.IsValid()
				|| !Target->TryGetObjectField(TEXT("identity"), Identity)
				|| !Identity || !Identity->IsValid()
				|| !(*Identity)->TryGetStringField(TEXT("selector"), OutSelector)
				|| OutSelector.IsEmpty())
			{
				return Fail(OutError,
					TEXT("level_world_domain_data_layer_selector_missing"));
			}
			return true;
		}

		bool ApplyDataLayers(
			UWorld& World,
			const TArray<TSharedPtr<FJsonValue>>* Layers,
			FWorkspaceError& OutError)
		{
			if (!Layers) return true;
			for (const TSharedPtr<FJsonValue>& LayerValue : *Layers)
			{
				const TSharedPtr<FJsonObject> Layer = LayerValue->AsObject();
				FString Type;
				FString ShortName;
				if (!Layer.IsValid()
					|| !Layer->TryGetStringField(TEXT("type"), Type)
					|| Type != TEXT("uai.world.data_layer")
					|| !Layer->TryGetStringField(TEXT("short_name"), ShortName)
					|| ShortName.TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError,
						TEXT("level_world_domain_data_layer_identity_invalid"));
				}
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
					return Fail(OutError,
						TEXT("level_world_domain_data_layer_exact_reference_unresolved"),
						ShortName);
				}
				const FString ExactDataLayerRef = AddedRefs[0];

				const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
				if (!Layer->HasField(TEXT("actor_managed_ids"))) continue;
				if (!Layer->TryGetArrayField(TEXT("actor_managed_ids"), ActorIds)
					|| !ActorIds)
				{
					return Fail(OutError,
						TEXT("level_world_domain_data_layer_actor_refs_invalid"),
						ShortName);
				}
				TArray<TSharedPtr<FJsonValue>> SubjectRefs;
				for (const TSharedPtr<FJsonValue>& IdValue : *ActorIds)
				{
					FString ManagedId;
					if (!IdValue.IsValid() || !IdValue->TryGetString(ManagedId))
					{
						return Fail(OutError,
							TEXT("level_world_domain_data_layer_actor_ref_invalid"),
							ShortName);
					}
					TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("world_path"),
						UeAgentLevelAuthoring::GetWorldPath(World));
					Target->SetStringField(TEXT("managed_id"), ManagedId);
					FString FindError;
					AActor* Actor = UeAgentLevelAuthoring::FindActorExact(
						World, Target, FindError);
					if (!Actor)
					{
						return Fail(OutError,
							TEXT("level_world_domain_data_layer_actor_not_found"),
							ManagedId, FindError);
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

		void RetainWorldPartitionActorsForAuthoring(
			UWorld& World,
			FWorldPartitionHelpers::FForEachActorWithLoadingResult& OutLoadResult)
		{
			UWorldPartition* const WorldPartition = World.GetWorldPartition();
			if (!WorldPartition)
			{
				return;
			}

			FWorldPartitionHelpers::FForEachActorWithLoadingParams LoadParams;
			LoadParams.bKeepReferences = true;
			LoadParams.ActorClasses = {AActor::StaticClass()};
			FWorldPartitionHelpers::ForEachActorWithLoading(
				WorldPartition,
				[](const FWorldPartitionActorDescInstance*)
				{
					return true;
				},
				LoadParams,
				OutLoadResult);
		}

		bool CaptureWorld(
			UWorld& World,
			const FString& Revision,
			const FString& CommandId,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			TArray<AActor*> Actors;
			FWorldPartitionHelpers::FForEachActorWithLoadingResult
				WorldPartitionLoadResult;
			RetainWorldPartitionActorsForAuthoring(
				World, WorldPartitionLoadResult);
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (*It) Actors.AddUnique(*It);
			}
			Actors.Sort([](const AActor& Left, const AActor& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			TArray<TSharedPtr<FJsonValue>> ActorValues;
			TSet<FString> ActorClasses;
			TSet<FString> ComponentClasses;
			for (AActor* Actor : Actors)
			{
				TSharedRef<FJsonObject> ActorObject = MakeShared<FJsonObject>();
				ActorObject->SetStringField(TEXT("object_path"), Actor->GetPathName());
				ActorObject->SetStringField(TEXT("actor_guid"),
					Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
				ActorObject->SetStringField(TEXT("managed_id"),
					UeAgentLevelAuthoring::GetManagedActorId(*Actor));
				ActorObject->SetStringField(TEXT("type"),
					Actor->GetClass()->GetPathName());
				ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
				ActorObject->SetStringField(TEXT("folder_path"),
					UeAgentLevelAuthoring::GetActorFolderPathText(*Actor));
				ActorObject->SetObjectField(TEXT("transform"),
					UeAgentLevelAuthoring::BuildTransformState(
						Actor->GetActorTransform(),
						UeAgentLevelAuthoring::ETransformFrame::ActorWorld));
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
				TArray<UActorComponent*> Components;
				Actor->GetComponents(Components);
				Components.Sort([](const UActorComponent& Left,
					const UActorComponent& Right)
				{
					return Left.GetName() < Right.GetName();
				});
				TArray<TSharedPtr<FJsonValue>> ComponentValues;
				for (UActorComponent* Component : Components)
				{
					if (!Component) continue;
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
									ComponentParentRelative));
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
			Snapshot->SetStringField(TEXT("revision"), Revision);
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
				Snapshot->SetStringField(TEXT("data_layers_diagnostic"),
					DataLayerError.GetCode());
			if (!bDataLayerRefsRead)
				Snapshot->SetStringField(TEXT("data_layer_refs_diagnostic"),
					DataLayerRefError.GetCode());

			TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("target"), World.GetPathName());
			Response->SetStringField(TEXT("root_type"),
				TEXT("/Script/Engine.World"));
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetStringField(TEXT("command_id"), CommandId);
			Response->SetBoolField(TEXT("complete"), true);
			Response->SetBoolField(TEXT("has_more"), false);
			Response->SetObjectField(TEXT("query_result"), Snapshot);
			return CanonicalObject(Response, OutPayload, OutError);
		}

		bool ImportWorld(
			const FString& Target,
			const FString& PackageName,
			const FString& ObjectName,
			const FString& ExpectedRevision,
			const TSharedPtr<FJsonObject>& Document,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			if (!EnsureAuthoringRegistered(OutError)) return false;
			const TSharedPtr<FJsonObject>* Root = nullptr;
			const TSharedPtr<FJsonObject>* RootDocument = nullptr;
			const TSharedPtr<FJsonObject>* Extension = nullptr;
			FString Type;
			FString Operation;
			if (!Document->TryGetObjectField(TEXT("root"), Root)
				|| !Root || !Root->IsValid()
				|| !(*Root)->TryGetStringField(TEXT("type"), Type)
				|| Type != TEXT("/Script/Engine.World")
				|| !(*Root)->TryGetObjectField(TEXT("document"), RootDocument)
				|| !RootDocument || !RootDocument->IsValid()
				|| !(*RootDocument)->TryGetStringField(TEXT("operation"), Operation)
				|| (Operation != TEXT("create") && Operation != TEXT("update"))
				|| !(*RootDocument)->TryGetObjectField(TEXT("provider_extension"),
					Extension)
				|| !Extension || !Extension->IsValid())
			{
				return Fail(OutError,
					TEXT("level_world_domain_root_document_invalid"));
			}
			const bool bExists = FPackageName::DoesPackageExist(PackageName);
			if ((Operation == TEXT("create")) != !bExists)
			{
				return Fail(OutError,
					Operation == TEXT("create")
						? TEXT("level_world_domain_target_already_exists")
						: TEXT("level_world_domain_target_not_found"),
					PackageName);
			}
			if (!bExists)
			{
				if (ExpectedRevision != TEXT("missing"))
				{
					return Fail(OutError,
						TEXT("level_world_domain_stale_revision"),
						ExpectedRevision, TEXT("missing"));
				}
			}
			else
			{
				FString CurrentRevision;
				if (!StableRevision(PackageName, CurrentRevision, OutError)) return false;
				if (ExpectedRevision != CurrentRevision)
				{
					return Fail(OutError,
						TEXT("level_world_domain_stale_revision"),
						ExpectedRevision, CurrentRevision);
				}
			}

			TSharedRef<FJsonObject> Bundle = MakeShared<FJsonObject>();
			const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
			if (!BuildLevelWorkspace(PackageName, *Extension, Bundle,
					DataLayers, OutError))
				return false;
			bool bWorldPartition = DataLayers && !DataLayers->IsEmpty();
			(*Extension)->TryGetBoolField(TEXT("world_partition"), bWorldPartition);
			FMapRestoreContext Restore;
			UWorld* World = nullptr;
			if (!OpenWorld(PackageName, ObjectName, !bExists, bWorldPartition,
					Restore, World, OutError))
				return false;
			if (World->GetOutermost()->IsDirty())
			{
				Restore.Restore(OutError);
				return Fail(OutError,
					TEXT("level_world_domain_target_world_dirty"), PackageName);
			}
			FWorldPartitionHelpers::FForEachActorWithLoadingResult
				WorldPartitionLoadResult;
			RetainWorldPartitionActorsForAuthoring(
				*World, WorldPartitionLoadResult);
			UeAgentLevelAuthoring::FWorkspaceApplyOptions Options;
			Options.bDryRun = false;
			Options.bUseEditorTransaction = true;
			UeAgentLevelAuthoring::FWorkspaceApplyResult ApplyResult;
			if (!UeAgentLevelAuthoring::ApplyWorkspaceSource(
					*World,
					UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
						Bundle, TEXT("inline://level-world-domain")),
					Options, ApplyResult,
					TEXT("level-world-domain-") + FGuid::NewGuid().ToString()) )
			{
				const FString Code = ApplyResult.ErrorCode.IsEmpty()
					? FString(TEXT("level_world_domain_workspace_apply_failed"))
					: ApplyResult.ErrorCode;
				const FString Detail = ApplyResult.ErrorDetail;
				Restore.Restore(OutError);
				return Fail(OutError, Code, Detail, ApplyResult.Phase);
			}
			if (!ApplyDataLayers(*World, DataLayers, OutError))
			{
				Restore.Restore(OutError);
				return false;
			}
			World->MarkPackageDirty();
			if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
			{
				Restore.Restore(OutError);
				return Fail(OutError,
					TEXT("level_world_domain_save_failed"), PackageName);
			}
			World = UEditorLoadingAndSavingUtils::LoadMap(MapFilename(PackageName));
			if (!World)
			{
				Restore.Restore(OutError);
				return Fail(OutError,
					TEXT("level_world_domain_reload_failed"), PackageName);
			}
			FString Revision;
			FString Readback;
			if (!StableRevision(PackageName, Revision, OutError)
				|| !CaptureWorld(*World, Revision,
					TEXT("content_graph.structure"), Readback, OutError))
			{
				Restore.Restore(OutError);
				return false;
			}
			TSharedPtr<FJsonObject> ReadbackObject;
			FString JsonError;
			if (!Json::TryParseObject(Readback, ReadbackObject, JsonError)
				|| !ReadbackObject.IsValid())
			{
				Restore.Restore(OutError);
				return Fail(OutError,
					TEXT("level_world_domain_independent_readback_invalid"),
					JsonError);
			}
			TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
			Response->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Response->SetStringField(TEXT("provider_id"),
				TEXT("level-world-domain-provider"));
			Response->SetStringField(TEXT("target"), Target);
			Response->SetStringField(TEXT("root_type"),
				TEXT("/Script/Engine.World"));
			Response->SetStringField(TEXT("revision"), Revision);
			Response->SetBoolField(TEXT("saved"), true);
			Response->SetBoolField(TEXT("reloaded"), true);
			Response->SetObjectField(TEXT("space_contract"), MakeFrame());
			Response->SetObjectField(TEXT("apply_result"), ApplyResult.ToJsonObject());
			Response->SetObjectField(TEXT("independent_readback"), ReadbackObject);
			if (!CanonicalObject(Response, OutPayload, OutError))
			{
				Restore.Restore(OutError);
				return false;
			}
			FWorkspaceError RestoreError;
			if (!Restore.Restore(RestoreError))
			{
				return Fail(OutError, RestoreError.GetCode(),
					RestoreError.GetSourceUnit(), RestoreError.GetJsonPointer());
			}
			return true;
		}
	}

	bool TryExecuteContentGraph(
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
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			return Fail(OutError,
				TEXT("level_world_domain_params_invalid"), JsonError);
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			return Fail(OutError,
				TEXT("level_world_domain_target_identity_mismatch"),
				TargetIdentity);
		}
		FString PackageName;
		FString ObjectName;
		if (!SplitTarget(Target, PackageName, ObjectName, OutError)) return false;
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			FString ExpectedRevision;
			if (!Params->TryGetStringField(
					TEXT("expected_revision"), ExpectedRevision))
			{
				return Fail(OutError,
					TEXT("level_world_domain_expected_revision_required"));
			}
			TSharedPtr<FJsonObject> Document;
			if (!ParseDocument(Params, Document, OutError)) return false;
			return ImportWorld(Target, PackageName, ObjectName, ExpectedRevision,
				Document, OutPayloadJson, OutError);
		}
		if (!CommandId.StartsWith(TEXT("content_graph.")))
		{
			return Fail(OutError,
				TEXT("level_world_domain_command_not_supported"), CommandId);
		}
		FMapRestoreContext Restore;
		UWorld* World = nullptr;
		if (!OpenWorld(PackageName, ObjectName, false, false,
				Restore, World, OutError))
			return false;
		FString Revision;
		const bool bCaptured = StableRevision(PackageName, Revision, OutError)
			&& CaptureWorld(*World, Revision, CommandId,
				OutPayloadJson, OutError);
		FWorkspaceError RestoreError;
		if (!Restore.Restore(RestoreError))
		{
			return Fail(OutError, RestoreError.GetCode(),
				RestoreError.GetSourceUnit(), RestoreError.GetJsonPointer());
		}
		bOutReadOnly = bCaptured;
		return bCaptured;
	}
}
