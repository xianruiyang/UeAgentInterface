#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "DomainAdapters/UObjectDataAsset/UeAgentRecursiveDataAsset.h"
#include "Dom/JsonObject.h"
#include "Factories/DataAssetFactory.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::OperationOwners::UObjectDataAsset
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString RootType =
			TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset");
		const FString OwnedType =
			TEXT("/Script/UeAgentInterface.UeAgentRecursiveOwnedObject");
		const FString ContractId =
			TEXT("uai.uobject_dataasset.recursive/1");
		const FString RootInitializationContractId =
			TEXT("uai.init.uobject_dataasset.root/1");
		const FString OwnedInitializationContractId =
			TEXT("uai.init.uobject_dataasset.owned/1");
		const FString DomainProviderId =
			TEXT("uobject-dataasset-domain-provider");
		FString FailureAfterNode;

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Sha256(const FString& Value)
		{
			FTCHARToUTF8 Utf8(*Value);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256Bytes(Bytes);
		}

		FString Canonical(const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		bool Fail(
			FUeAgentUObjectDataAssetCommandError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool ParseObjectPath(
			const FString& Target,
			FString& OutPackageName,
			FString& OutAssetName)
		{
			const FString Normalized = Normalize(Target);
			int32 Dot = INDEX_NONE;
			if (!Normalized.FindLastChar(TEXT('.'), Dot)
				|| Dot <= 0
				|| Dot + 1 >= Normalized.Len())
			{
				return false;
			}
			OutPackageName = Normalized.Left(Dot);
			OutAssetName = Normalized.Mid(Dot + 1);
			return FPackageName::IsValidLongPackageName(OutPackageName)
				&& !OutAssetName.IsEmpty()
				&& FName::IsValidXName(
					OutAssetName,
					INVALID_OBJECTNAME_CHARACTERS);
		}

		bool TargetExists(const FString& Target)
		{
			if (StaticFindObject(UObject::StaticClass(), nullptr, *Target))
			{
				return true;
			}
			FString PackageName;
			FString AssetName;
			return ParseObjectPath(Target, PackageName, AssetName)
				&& FPackageName::DoesPackageExist(PackageName);
		}

		FUeAgentStateRevision TargetRevision(const FString& Target)
		{
			return TargetExists(Target)
				? FUeAgentStateRevision(
					TEXT("uobject-dataasset"), TEXT("present"), 1)
				: FUeAgentStateRevision(
					TEXT("uobject-dataasset"), TEXT("absent"), 0);
		}

		FUeAgentResourceTracker MakeResources(
			const int64 MaxNodes = 4096,
			const int64 MaxWorkUnits = 262144)
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, MaxNodes},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, MaxWorkUnits},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 * 1024 * 1024},
			}));
		}

		FSemanticUnit Unit(
			FString UnitId,
			const ESemanticUnitKind Kind,
			FString Owner,
			FString Role)
		{
			return FSemanticUnit(
				MoveTemp(UnitId),
				Kind,
				MoveTemp(Owner),
				TEXT("uai.uobject_dataasset.recursive.semantic/1"),
				MoveTemp(Role),
				FString(),
				{},
				{},
				{},
				FString(),
				FString(),
				{},
				{TEXT("state"), TEXT("minimal_create"), TEXT("local_edit")},
				TEXT("strict_v2:t13:current-design"));
		}

		bool TryBuildSemanticModel(
			FUnifiedHjsonSemanticModelPtr& OutModel,
			FWorkspaceError& OutError)
		{
			const TArray<FString> Ids{
				TEXT("target"), TEXT("property"), TEXT("member"),
				TEXT("structure"), TEXT("item"), TEXT("endpoint"),
				TEXT("relation"), TEXT("order"), TEXT("layout"),
				TEXT("temporal"), TEXT("spatial"), TEXT("external"),
			};
			TArray<FSemanticUnit> Units{
				Unit(Ids[0], ESemanticUnitKind::TargetState, FString(), TEXT("target")),
				Unit(Ids[1], ESemanticUnitKind::PropertySet, Ids[0], TEXT("property_set")),
				Unit(Ids[2], ESemanticUnitKind::EntityState, Ids[0], TEXT("member")),
				Unit(Ids[3], ESemanticUnitKind::StructureState, Ids[0], TEXT("structure")),
				Unit(Ids[4], ESemanticUnitKind::ItemState, Ids[3], TEXT("item")),
				Unit(Ids[5], ESemanticUnitKind::EntityState, Ids[4], TEXT("endpoint")),
				Unit(Ids[6], ESemanticUnitKind::RelationSet, Ids[3], TEXT("relation")),
				Unit(Ids[7], ESemanticUnitKind::OrderingState, Ids[3], TEXT("order")),
				Unit(Ids[8], ESemanticUnitKind::LayoutState, Ids[3], TEXT("layout")),
				Unit(Ids[9], ESemanticUnitKind::PropertySet, Ids[0], TEXT("temporal_contract")),
				Unit(Ids[10], ESemanticUnitKind::PropertySet, Ids[0], TEXT("spatial_contract")),
				Unit(Ids[11], ESemanticUnitKind::ExternalPayloadState, Ids[0], TEXT("external_input")),
			};
			FUeAgentResourceTracker CoreResources = MakeResources();
			FAuthoringModelPtr Core;
			if (!TryCompileAuthoringModel(
					TEXT("uai.uobject_dataasset.recursive.hjson"),
					1,
					TEXT("unified"),
					TEXT("strict-v2-r25"),
					MoveTemp(Units),
					{Ids[0]},
					CoreResources,
					Core,
					OutError)
				|| !Core.IsValid())
			{
				return false;
			}
			TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions{
				{EUnifiedHjsonCompositionKind::Target, {Ids[0]}},
				{EUnifiedHjsonCompositionKind::PropertySet, {Ids[1]}},
				{EUnifiedHjsonCompositionKind::MemberSet, {Ids[2]}},
				{EUnifiedHjsonCompositionKind::Structure, {Ids[3]}},
				{EUnifiedHjsonCompositionKind::Item, {Ids[4]}},
				{EUnifiedHjsonCompositionKind::Endpoint, {Ids[5]}},
				{EUnifiedHjsonCompositionKind::Relation, {Ids[6]}},
				{EUnifiedHjsonCompositionKind::Order, {Ids[7]}},
				{EUnifiedHjsonCompositionKind::Layout, {Ids[8]}},
				{EUnifiedHjsonCompositionKind::TemporalContract, {Ids[9]}},
				{EUnifiedHjsonCompositionKind::SpatialContract, {Ids[10]}},
				{EUnifiedHjsonCompositionKind::ExternalInput, {Ids[11]}},
			};
			FUeAgentResourceTracker ModelResources = MakeResources();
			return TryCreateUnifiedHjsonSemanticModel(
				Core.ToSharedRef(),
				EUnifiedHjsonAuthoringIntent::MinimalCreate,
				MoveTemp(Compositions),
				ModelResources,
				OutModel,
				OutError);
		}

		struct FNodeValues final
		{
			TOptional<FString> Label;
			TOptional<int32> ExplicitNumber;
			TOptional<TArray<int32>> Numbers;
			TOptional<TMap<FString, int32>> NamedNumbers;
		};

		struct FNodeSpec final
		{
			FString Id;
			FString Name;
			FString Type;
			FString OwnerId;
			FNodeValues Values;
			bool bRoot = false;
		};

		struct FReferenceSpec final
		{
			FString From;
			FString Field;
			FString To;
		};

		struct FPreparedImport final
		{
			FString Target;
			FString PackageName;
			FString AssetName;
			FString AuthorizationId;
			FString SourceFingerprint;
			EImportSourceCarrier SourceCarrier = EImportSourceCarrier::Inline;
			TArray<FNodeSpec> Nodes;
			TMap<FString, int32> NodeIndexById;
			TArray<FInitializationEdge> OwnershipEdges;
			TArray<FReferenceSpec> References;
			FImportInitializationPlanPtr InitializationPlan;
		};

		bool TryReadResolvedSource(
			const TSharedPtr<FJsonObject>& Params,
			const FString& ExpectedTarget,
			const FString& ExpectedRevision,
			FString& OutDocument,
			FString& OutSourceUnit,
			FString& OutSourceFingerprint,
			EImportSourceCarrier& OutCarrier,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			OutDocument.Reset();
			OutSourceUnit.Reset();
			OutSourceFingerprint.Reset();
			OutCarrier = EImportSourceCarrier::Inline;
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("content_graph_legacy_source_contract_rejected"),
					ExpectedTarget);
			}
			const TSharedPtr<FJsonObject>* Source = nullptr;
			if (!Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(
					OutError,
					TEXT("content_graph_source_required"),
					ExpectedTarget);
			}
			FString Kind;
			FString Carrier;
			FString TargetIdentity;
			FString BaseRevision;
			FString EntryFile;
			if (!(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(TEXT("carrier"), Carrier)
				|| !(*Source)->TryGetStringField(TEXT("source_unit"), OutSourceUnit)
				|| !(*Source)->TryGetStringField(TEXT("target_identity"), TargetIdentity)
				|| !(*Source)->TryGetStringField(TEXT("base_revision"), BaseRevision)
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| !(*Source)->TryGetStringField(
					TEXT("source_fingerprint"),
					OutSourceFingerprint)
				|| TargetIdentity != ExpectedTarget
				|| (Carrier == TEXT("directory")
					&& BaseRevision != ExpectedRevision))
			{
				return Fail(
					OutError,
					TEXT("content_graph_resolved_source_invalid"),
					ExpectedTarget);
			}
			if (Carrier == TEXT("directory"))
			{
				OutCarrier = EImportSourceCarrier::Directory;
			}
			else if (Carrier == TEXT("single_file"))
			{
				OutCarrier = EImportSourceCarrier::SingleFile;
			}
			else if (Carrier != TEXT("inline"))
			{
				return Fail(
					OutError,
					TEXT("content_graph_resolved_source_invalid"),
					Carrier);
			}
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("uobject_dataasset_multi_document_unsupported"),
					ExpectedTarget);
			}
			const TSharedPtr<FJsonObject> DocumentObject = (*Documents)[0].IsValid()
				? (*Documents)[0]->AsObject()
				: nullptr;
			FString RelativePath;
			if (!DocumentObject.IsValid()
				|| !DocumentObject->TryGetStringField(TEXT("path"), RelativePath)
				|| !DocumentObject->TryGetStringField(TEXT("document"), OutDocument)
				|| RelativePath != EntryFile
				|| OutDocument.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_resolved_source_invalid"),
					ExpectedTarget);
			}
			return true;
		}

		bool ReadString(
			const FHjsonNode& Object,
			const TCHAR* Key,
			FString& Out,
			FUeAgentUObjectDataAssetCommandError& OutError,
			const bool bRequired = true)
		{
			const FHjsonNode* Value = Object.Get(Key);
			if (!Value)
			{
				return !bRequired || Fail(
					OutError,
					TEXT("uobject_dataasset_field_required"),
					Key);
			}
			if (Value->GetKind() != EHjsonNodeKind::String)
			{
				return Fail(
					OutError,
					TEXT("uobject_dataasset_field_type_invalid"),
					Key);
			}
			Out = Normalize(Value->GetText());
			return !Out.IsEmpty() || !bRequired;
		}

		bool ReadInteger(
			const FHjsonNode& Value,
			int32& Out,
			FUeAgentUObjectDataAssetCommandError& OutError,
			const FString& Subject)
		{
			int64 Parsed = 0;
			if (Value.GetKind() != EHjsonNodeKind::Integer
				|| !LexTryParseString(Parsed, *Value.GetLexeme())
				|| Parsed < MIN_int32
				|| Parsed > MAX_int32)
			{
				return Fail(
					OutError,
					TEXT("uobject_dataasset_value_type_invalid"),
					Subject);
			}
			Out = static_cast<int32>(Parsed);
			return true;
		}

		bool ReadValues(
			const FHjsonNode& Node,
			const bool bRoot,
			FNodeValues& OutValues,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			const FHjsonNode* Values = Node.Get(TEXT("values"));
			if (!Values)
			{
				return true;
			}
			if (Values->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					TEXT("uobject_dataasset_values_invalid"));
			}
			for (const TPair<FString, FHjsonNodeRef>& Entry
				: Values->GetEntries())
			{
				if (Entry.Key == TEXT("label"))
				{
					if (Entry.Value->GetKind() != EHjsonNodeKind::String)
					{
						return Fail(OutError, TEXT("uobject_dataasset_value_type_invalid"), Entry.Key);
					}
					OutValues.Label = Entry.Value->GetText();
				}
				else if (bRoot && Entry.Key == TEXT("explicit_number"))
				{
					int32 Value = 0;
					if (!ReadInteger(*Entry.Value, Value, OutError, Entry.Key))
					{
						return false;
					}
					OutValues.ExplicitNumber = Value;
				}
				else if (bRoot && Entry.Key == TEXT("numbers"))
				{
					if (Entry.Value->GetKind() != EHjsonNodeKind::Array)
					{
						return Fail(OutError, TEXT("uobject_dataasset_value_type_invalid"), Entry.Key);
					}
					TArray<int32> Numbers;
					for (const FHjsonNodeRef& Item : Entry.Value->GetItems())
					{
						int32 Value = 0;
						if (!ReadInteger(*Item, Value, OutError, Entry.Key))
						{
							return false;
						}
						Numbers.Add(Value);
					}
					OutValues.Numbers = MoveTemp(Numbers);
				}
				else if (bRoot && Entry.Key == TEXT("named_numbers"))
				{
					if (Entry.Value->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, TEXT("uobject_dataasset_value_type_invalid"), Entry.Key);
					}
					TMap<FString, int32> ValuesByName;
					for (const TPair<FString, FHjsonNodeRef>& Named
						: Entry.Value->GetEntries())
					{
						int32 Value = 0;
						if (!ReadInteger(*Named.Value, Value, OutError, Named.Key))
						{
							return false;
						}
						ValuesByName.Add(Named.Key, Value);
					}
					OutValues.NamedNumbers = MoveTemp(ValuesByName);
				}
				else
				{
					return Fail(
						OutError,
						TEXT("uobject_dataasset_value_unsupported"),
						Entry.Key);
				}
			}
			return true;
		}

		class FPreflight final : public IImportInitializationPreflight
		{
		public:
			FPreflight(FString InTarget, FString InAuthorizationId)
				: Target(MoveTemp(InTarget))
				, AuthorizationId(MoveTemp(InAuthorizationId))
			{
			}

			virtual bool TryReadCurrentTargetRevision(
				const FString& TargetIdentity,
				FUeAgentStateRevision& OutRevision,
				FWorkspaceError& OutError) override
			{
				if (TargetIdentity != Target)
				{
					OutError.Set(TEXT("uobject_dataasset_target_identity_changed"));
					return false;
				}
				OutRevision = TargetRevision(Target);
				return true;
			}

			virtual bool TryResolveCandidate(
				const FString& CandidateName,
				const FString& BindingRevision,
				FString& OutResolvedName,
				FWorkspaceError& OutError) override
			{
				OutResolvedName = CandidateName;
				return true;
			}

			virtual bool SupportsChange(
				const FImportChange& Change,
				FWorkspaceError& OutError) override
			{
				return Change.IsValid();
			}

			virtual bool TryAuthorizeImpact(
				const TArray<FString>& ImpactScopes,
				const FString& PlanFingerprint,
				const FString& InAuthorizationId,
				FWorkspaceError& OutError) override
			{
				if (InAuthorizationId != AuthorizationId
					|| AuthorizationId.IsEmpty()
					|| ImpactScopes != TArray<FString>{Target}
					|| !PlanFingerprint.StartsWith(TEXT("sha256:")))
				{
					OutError.Set(TEXT("uobject_dataasset_impact_unauthorized"));
					return false;
				}
				return true;
			}

		private:
			FString Target;
			FString AuthorizationId;
		};

		bool TryPrepareImport(
			const TSharedPtr<FJsonObject>& Params,
			FPreparedImport& OutPrepared,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			OutPrepared = {};
			FString DocumentText;
			FString SourceUnit;
			FString ExpectedRevision;
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("target"), OutPrepared.Target)
				|| !Params->TryGetStringField(TEXT("expected_revision"), ExpectedRevision)
				|| !Params->TryGetStringField(TEXT("authorization_id"), OutPrepared.AuthorizationId))
			{
				return Fail(OutError, TEXT("uobject_dataasset_import_params_invalid"));
			}
			if (Normalize(ExpectedRevision) != TEXT("absent"))
			{
				return Fail(OutError, TEXT("uobject_dataasset_expected_revision_invalid"));
			}
			if (!ParseObjectPath(
					OutPrepared.Target,
					OutPrepared.PackageName,
					OutPrepared.AssetName))
			{
				return Fail(OutError, TEXT("uobject_dataasset_target_invalid"), OutPrepared.Target);
			}
			if (!TryReadResolvedSource(
					Params,
					OutPrepared.Target,
					ExpectedRevision,
					DocumentText,
					SourceUnit,
					OutPrepared.SourceFingerprint,
					OutPrepared.SourceCarrier,
					OutError))
			{
				return false;
			}

			int64 MaxNodes = 4096;
			int64 MaxWorkUnits = 262144;
			const TSharedPtr<FJsonObject>* Limits = nullptr;
			if (Params->TryGetObjectField(TEXT("limits"), Limits)
				&& Limits && Limits->IsValid())
			{
				double Number = 0.0;
				if ((*Limits)->TryGetNumberField(TEXT("max_nodes"), Number))
				{
					MaxNodes = FMath::Max<int64>(0, static_cast<int64>(Number));
				}
				if ((*Limits)->TryGetNumberField(TEXT("max_work_units"), Number))
				{
					MaxWorkUnits = FMath::Max<int64>(0, static_cast<int64>(Number));
				}
			}

			// The public max_nodes limit describes the recursive UObject graph,
			// not the parser's scalar/object/array syntax-node accounting.  Keep
			// parsing independently bounded, then enforce the requested limit on
			// the validated initialization nodes below.
			FUeAgentResourceTracker ParseResources = MakeResources();
			FHjsonDocumentPtr Document;
			FWorkspaceError WorkspaceError;
			if (!TryParseHjsonDocument(
					DocumentText,
					SourceUnit,
					ParseResources,
					Document,
					WorkspaceError)
				|| !Document.IsValid())
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("uobject_dataasset_hjson_invalid"))
						: WorkspaceError.GetCode(),
					WorkspaceError.GetSourceUnit(),
					WorkspaceError.GetJsonPointer());
			}
			const FHjsonNode& DocumentRoot = *Document->GetRoot();
			if (DocumentRoot.GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(OutError, TEXT("uobject_dataasset_hjson_root_invalid"));
			}
			FString Schema;
			if (!ReadString(DocumentRoot, TEXT("schema"), Schema, OutError)
				|| Schema != ContractId)
			{
				return Schema.IsEmpty()
					? false
					: Fail(OutError, TEXT("uobject_dataasset_contract_unsupported"), Schema);
			}
			const FHjsonNode* Root = DocumentRoot.Get(TEXT("root"));
			if (!Root || Root->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(OutError, TEXT("uobject_dataasset_root_required"));
			}

			struct FPending final
			{
				const FHjsonNode* Node = nullptr;
				FString OwnerId;
				bool bRoot = false;
			};
			TArray<FPending> Worklist{{Root, FString(), true}};
			TArray<FInitializationEdge> InferredOwnership;
			for (int32 Cursor = 0; Cursor < Worklist.Num(); ++Cursor)
			{
				const FPending Pending = Worklist[Cursor];
				FNodeSpec Spec;
				Spec.OwnerId = Pending.OwnerId;
				Spec.bRoot = Pending.bRoot;
				if (!ReadString(*Pending.Node, TEXT("id"), Spec.Id, OutError)
					|| !ReadString(*Pending.Node, TEXT("type"), Spec.Type, OutError))
				{
					return false;
				}
				Spec.Name = Pending.bRoot ? OutPrepared.AssetName : FString();
				if (!Pending.bRoot
					&& !ReadString(*Pending.Node, TEXT("name"), Spec.Name, OutError))
				{
					return false;
				}
				if ((Pending.bRoot && Spec.Type != RootType)
					|| (!Pending.bRoot && Spec.Type != OwnedType))
				{
					return Fail(
						OutError,
						TEXT("uobject_dataasset_type_unknown"),
						Spec.Type);
				}
				if (OutPrepared.NodeIndexById.Contains(Spec.Id))
				{
					return Fail(OutError, TEXT("uobject_dataasset_node_duplicate"), Spec.Id);
				}
				if (!ReadValues(*Pending.Node, Pending.bRoot, Spec.Values, OutError))
				{
					return false;
				}
				OutPrepared.NodeIndexById.Add(Spec.Id, OutPrepared.Nodes.Num());
				OutPrepared.Nodes.Add(MoveTemp(Spec));
				if (!Pending.OwnerId.IsEmpty())
				{
					InferredOwnership.Emplace(
						Pending.OwnerId,
						OutPrepared.Nodes.Last().Id,
						EInitializationEdgeKind::Ownership);
				}
				const FHjsonNode* Owned = Pending.Node->Get(TEXT("owned"));
				if (!Owned)
				{
					continue;
				}
				if (Owned->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, TEXT("uobject_dataasset_owned_invalid"), OutPrepared.Nodes.Last().Id);
				}
				for (const FHjsonNodeRef& Child : Owned->GetItems())
				{
					if (Child->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, TEXT("uobject_dataasset_owned_item_invalid"));
					}
					Worklist.Add({&Child.Get(), OutPrepared.Nodes.Last().Id, false});
				}
			}
			if (OutPrepared.Nodes.Num() > MaxNodes)
			{
				return Fail(
					OutError,
					TEXT("resource_budget_exceeded"),
					OutPrepared.Target,
					FString::Printf(
						TEXT("recursive object nodes %d exceed max_nodes %lld"),
						OutPrepared.Nodes.Num(),
						MaxNodes));
			}

			OutPrepared.OwnershipEdges = InferredOwnership;
			const FHjsonNode* ExplicitOwnership = Root->Get(TEXT("ownership"));
			if (ExplicitOwnership)
			{
				if (ExplicitOwnership->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, TEXT("uobject_dataasset_ownership_invalid"));
				}
				TArray<FInitializationEdge> ExplicitEdges;
				TSet<FString> ExplicitChildren;
				for (const FHjsonNodeRef& Edge : ExplicitOwnership->GetItems())
				{
					if (Edge->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, TEXT("uobject_dataasset_ownership_item_invalid"));
					}
					FString Owner;
					FString Child;
					if (!ReadString(*Edge, TEXT("owner"), Owner, OutError)
						|| !ReadString(*Edge, TEXT("child"), Child, OutError))
					{
						return false;
					}
					if (!OutPrepared.NodeIndexById.Contains(Owner)
						|| !OutPrepared.NodeIndexById.Contains(Child))
					{
						return Fail(OutError, TEXT("uobject_dataasset_ownership_endpoint_unknown"));
					}
					ExplicitChildren.Add(Child);
					ExplicitEdges.Emplace(Owner, Child, EInitializationEdgeKind::Ownership);
				}
				OutPrepared.OwnershipEdges.RemoveAll(
					[&ExplicitChildren](const FInitializationEdge& Edge)
					{
						return ExplicitChildren.Contains(Edge.GetToNodeId());
					});
				OutPrepared.OwnershipEdges.Append(ExplicitEdges);
			}

			const FHjsonNode* References = Root->Get(TEXT("references"));
			if (References)
			{
				if (References->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(OutError, TEXT("uobject_dataasset_references_invalid"));
				}
				for (const FHjsonNodeRef& Edge : References->GetItems())
				{
					if (Edge->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(OutError, TEXT("uobject_dataasset_reference_item_invalid"));
					}
					FReferenceSpec Reference;
					if (!ReadString(*Edge, TEXT("from"), Reference.From, OutError)
						|| !ReadString(*Edge, TEXT("field"), Reference.Field, OutError)
						|| !ReadString(*Edge, TEXT("to"), Reference.To, OutError))
					{
						return false;
					}
					if (!OutPrepared.NodeIndexById.Contains(Reference.From)
						|| !OutPrepared.NodeIndexById.Contains(Reference.To)
						|| (Reference.Field != TEXT("peer")
							&& Reference.Field != TEXT("shared")))
					{
						return Fail(OutError, TEXT("uobject_dataasset_reference_invalid"), Reference.From);
					}
					OutPrepared.References.Add(MoveTemp(Reference));
				}
			}

			FUnifiedHjsonSemanticModelPtr Model;
			if (!TryBuildSemanticModel(Model, WorkspaceError)
				|| !Model.IsValid())
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("uobject_dataasset_semantic_model_failed"))
						: WorkspaceError.GetCode());
			}

			TArray<FImportChange> Changes;
			TArray<FInitializationNode> InitializationNodes;
			TArray<FInitializationEdge> AllEdges = OutPrepared.OwnershipEdges;
			for (const FNodeSpec& Spec : OutPrepared.Nodes)
			{
				Changes.Emplace(
					TEXT("create:") + Spec.Id,
					EImportChangeKind::Create,
					Spec.Id);
				InitializationNodes.Emplace(
					Spec.Id,
					Spec.bRoot
						? EInitializationNodeRole::RootTarget
						: EInitializationNodeRole::OwnedObject,
					Spec.Type,
					Spec.bRoot
						? RootInitializationContractId
						: OwnedInitializationContractId);
			}
			for (const FReferenceSpec& Reference : OutPrepared.References)
			{
				Changes.Emplace(
					TEXT("connect:") + Reference.From + TEXT(":") + Reference.Field,
					EImportChangeKind::Connect,
					Reference.From);
				AllEdges.Emplace(
					Reference.From,
					Reference.To,
					EInitializationEdgeKind::Reference);
			}

			const FImportInitializationPlanningRequest PlanningRequest(
				OutPrepared.SourceCarrier,
				Model.ToSharedRef(),
				OutPrepared.Target,
				FUeAgentStateRevision(TEXT("uobject-dataasset"), TEXT("absent"), 0),
				OutPrepared.SourceFingerprint,
				Sha256(ContractId + TEXT("|") + OutPrepared.Target),
				MoveTemp(Changes),
				MoveTemp(InitializationNodes),
				MoveTemp(AllEdges),
				{OutPrepared.Target},
				OutPrepared.AuthorizationId);
			FUeAgentResourceTracker PlanningResources =
				MakeResources(MaxNodes, MaxWorkUnits);
			if (!TryPlanHjsonImportInitialization(
					PlanningRequest,
					MakeShared<FPreflight>(
						OutPrepared.Target,
						OutPrepared.AuthorizationId),
					PlanningResources,
					OutPrepared.InitializationPlan,
					WorkspaceError)
				|| !OutPrepared.InitializationPlan.IsValid())
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("uobject_dataasset_import_plan_failed"))
						: WorkspaceError.GetCode(),
					WorkspaceError.GetSourceUnit(),
					WorkspaceError.GetJsonPointer());
			}
			return true;
		}

		struct FExecutionContext final
		{
			explicit FExecutionContext(FPreparedImport InPrepared)
				: Prepared(MoveTemp(InPrepared))
			{
			}

			FPreparedImport Prepared;
			UUeAgentRecursiveDataAsset* Asset = nullptr;
			TMap<FString, UUeAgentRecursiveOwnedObject*> OwnedById;
			TSharedPtr<FJsonObject> IndependentReadback;
		};

		void ApplyValues(
			UUeAgentRecursiveDataAsset& Asset,
			const FNodeValues& Values)
		{
			if (Values.Label.IsSet()) Asset.Label = Values.Label.GetValue();
			if (Values.ExplicitNumber.IsSet()) Asset.ExplicitNumber = Values.ExplicitNumber.GetValue();
			if (Values.Numbers.IsSet()) Asset.Numbers = Values.Numbers.GetValue();
			if (Values.NamedNumbers.IsSet()) Asset.NamedNumbers = Values.NamedNumbers.GetValue();
		}

		void ApplyValues(
			UUeAgentRecursiveOwnedObject& Object,
			const FNodeValues& Values)
		{
			if (Values.Label.IsSet()) Object.Label = Values.Label.GetValue();
		}

		class FVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FVersionFence(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Check(
				const FOperationLifecyclePlan& Plan,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = Stage == EOperationLifecycleStage::PreExecuteFence
					? !TargetExists(Context->Prepared.Target)
					: Context->Asset != nullptr
						&& Context->Asset->GetPathName() == Context->Prepared.Target;
				if (!bOutCurrent)
				{
					OutError = FOperationLifecycleError(TEXT("operation_version_conflict"));
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FMutationPort final : public IOperationDomainMutationPort
		{
		public:
			explicit FMutationPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Execute(
				const FOperationLifecyclePlan& Plan,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				FAssetToolsModule& AssetToolsModule =
					FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
				UDataAssetFactory* Factory = NewObject<UDataAssetFactory>();
				Factory->DataAssetClass = UUeAgentRecursiveDataAsset::StaticClass();
				UObject* Created = AssetToolsModule.Get().CreateAsset(
					Context->Prepared.AssetName,
					FPackageName::GetLongPackagePath(Context->Prepared.PackageName),
					UUeAgentRecursiveDataAsset::StaticClass(),
					Factory);
				Context->Asset = Cast<UUeAgentRecursiveDataAsset>(Created);
				if (!Context->Asset)
				{
					OutError = FOperationLifecycleError(
						TEXT("uobject_dataasset_factory_create_failed"));
					return false;
				}
				OutFacts = FOperationMutationFacts(
					FGuid::NewGuid().ToString(EGuidFormats::Digits),
					{Context->Prepared.Target},
					true);
				const int32* RootIndex =
					Context->Prepared.NodeIndexById.Find(TEXT("root"));
				if (!RootIndex)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_root_plan_missing"));
					return false;
				}
				Context->Asset->Modify();
				ApplyValues(
					*Context->Asset,
					Context->Prepared.Nodes[*RootIndex].Values);

				for (const FString& NodeId
					: Context->Prepared.InitializationPlan->GetCreationOrder())
				{
					const int32* Index = Context->Prepared.NodeIndexById.Find(NodeId);
					if (!Index)
					{
						OutError = FOperationLifecycleError(TEXT("uobject_dataasset_plan_node_unknown"), NodeId);
						return false;
					}
					const FNodeSpec& Spec = Context->Prepared.Nodes[*Index];
					if (Spec.bRoot)
					{
						continue;
					}
					UObject* Outer = Spec.OwnerId == TEXT("root")
						? static_cast<UObject*>(Context->Asset)
						: static_cast<UObject*>(Context->OwnedById.FindRef(Spec.OwnerId));
					if (!Outer)
					{
						OutError = FOperationLifecycleError(TEXT("uobject_dataasset_owner_missing"), Spec.OwnerId);
						return false;
					}
					UUeAgentRecursiveOwnedObject* Object =
						NewObject<UUeAgentRecursiveOwnedObject>(
							Outer,
							UUeAgentRecursiveOwnedObject::StaticClass(),
							FName(*Spec.Name),
							RF_Transactional);
					if (!Object)
					{
						OutError = FOperationLifecycleError(TEXT("uobject_dataasset_child_create_failed"), Spec.Id);
						return false;
					}
					Object->NodeId = Spec.Id;
					ApplyValues(*Object, Spec.Values);
					if (UUeAgentRecursiveDataAsset* Root = Cast<UUeAgentRecursiveDataAsset>(Outer))
					{
						Root->OwnedObjects.Add(Object);
					}
					else if (UUeAgentRecursiveOwnedObject* Owner = Cast<UUeAgentRecursiveOwnedObject>(Outer))
					{
						Owner->Children.Add(Object);
					}
					Context->OwnedById.Add(Spec.Id, Object);
					if (!FailureAfterNode.IsEmpty()
						&& FailureAfterNode == Spec.Id)
					{
						OutError = FOperationLifecycleError(
							TEXT("uobject_dataasset_child_create_failed"),
							Spec.Id);
						return false;
					}
				}

				for (const FReferenceSpec& Reference
					: Context->Prepared.References)
				{
					UUeAgentRecursiveOwnedObject* From =
						Context->OwnedById.FindRef(Reference.From);
					UUeAgentRecursiveOwnedObject* To =
						Context->OwnedById.FindRef(Reference.To);
					if (!From || !To)
					{
						OutError = FOperationLifecycleError(TEXT("uobject_dataasset_reference_endpoint_missing"));
						return false;
					}
					if (Reference.Field == TEXT("peer")) From->Peer = To;
					else From->Shared = To;
				}
				Context->Asset->GetOutermost()->SetDirtyFlag(true);
				Context->Asset->PostEditChange();
				return true;
			}

			virtual bool Rollback(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				UUeAgentRecursiveDataAsset* Asset = Context->Asset;
				if (!Asset)
				{
					Asset = LoadObject<UUeAgentRecursiveDataAsset>(
						nullptr, *Context->Prepared.Target);
				}
				Context->OwnedById.Reset();
				Context->Asset = nullptr;
				if (Asset)
				{
					ObjectTools::DeleteObjectsUnchecked({Asset});
				}
				const FString Filename = FPackageName::LongPackageNameToFilename(
					Context->Prepared.PackageName,
					FPackageName::GetAssetPackageExtension());
				if (IFileManager::Get().FileExists(*Filename))
				{
					IFileManager::Get().Delete(*Filename, false, true, true);
				}
				bOutConfirmed = StaticFindObject(
					UObject::StaticClass(),
					nullptr,
					*Context->Prepared.Target) == nullptr
					&& !IFileManager::Get().FileExists(*Filename);
				if (!bOutConfirmed)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_rollback_unconfirmed"));
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FStabilityPort final : public IOperationStabilityPort
		{
		public:
			explicit FStabilityPort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Settle(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FOperationLifecycleError& OutError) override
			{
				if (!Context->Asset
					|| !Context->Asset->NativeDefaultSubobject
					|| Context->Asset->NativeDefaultSubobject->GetOuter()
						!= Context->Asset)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_settle_root_invalid"));
					return false;
				}
				for (const TPair<FString, UUeAgentRecursiveOwnedObject*>& Pair
					: Context->OwnedById)
				{
					if (!Pair.Value
						|| !Pair.Value->NativeDefaultSubobject
						|| Pair.Value->NativeDefaultSubobject->GetOuter()
							!= Pair.Value)
					{
						OutError = FOperationLifecycleError(TEXT("uobject_dataasset_settle_child_invalid"), Pair.Key);
						return false;
					}
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FPersistencePort final : public IOperationPersistencePort
		{
		public:
			explicit FPersistencePort(TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Persist(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				FString& OutPersistenceReceiptId,
				FOperationLifecycleError& OutError) override
			{
				if (!Context->Asset)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_persist_asset_missing"));
					return false;
				}
				UPackage* Package = Context->Asset->GetOutermost();
				const FString Filename = FPackageName::LongPackageNameToFilename(
					Context->Prepared.PackageName,
					FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				if (!UPackage::SavePackage(
						Package,
						Context->Asset,
						*Filename,
						Args))
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_persist_failed"), Filename);
					return false;
				}
				OutPersistenceReceiptId = Sha256(
					Context->Prepared.Target + TEXT("|") + Filename);
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		TSharedPtr<FJsonObject> BuildReadback(
			UUeAgentRecursiveDataAsset& Asset,
			const bool bReloaded)
		{
			TMap<FString, UUeAgentRecursiveOwnedObject*> ById;
			int32 DeepCount = 0;
			bool bDefaults = Asset.NativeDefaultSubobject
				&& Asset.NativeDefaultSubobject->GetOuter() == &Asset
				&& Asset.NativeDefaultSubobject->NativeDefaultValue == 29;
			for (UUeAgentRecursiveOwnedObject* Object : Asset.OwnedObjects)
			{
				if (!Object) continue;
				ById.Add(Object->NodeId, Object);
				bDefaults = bDefaults
					&& Object->NativeCounter == 17
					&& Object->NativeDefaultSubobject
					&& Object->NativeDefaultSubobject->GetOuter() == Object
					&& Object->NativeDefaultSubobject->NativeDefaultValue == 29;
				for (UUeAgentRecursiveOwnedObject* Child : Object->Children)
				{
					if (!Child) continue;
					++DeepCount;
					ById.Add(Child->NodeId, Child);
					bDefaults = bDefaults
						&& Child->NativeCounter == 17
						&& Child->NativeDefaultSubobject
						&& Child->NativeDefaultSubobject->GetOuter() == Child;
				}
			}
			UUeAgentRecursiveOwnedObject* Left = ById.FindRef(TEXT("left"));
			UUeAgentRecursiveOwnedObject* Right = ById.FindRef(TEXT("right"));
			UUeAgentRecursiveOwnedObject* Shared = ById.FindRef(TEXT("shared"));
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("reloaded"), bReloaded);
			Result->SetStringField(TEXT("asset_class"), Asset.GetClass()->GetPathName());
			Result->SetStringField(TEXT("root_outer"), Asset.GetOuter()->GetPathName());
			Result->SetNumberField(TEXT("native_counter"), Asset.NativeCounter);
			Result->SetStringField(TEXT("label"), Asset.Label);
			Result->SetNumberField(TEXT("explicit_number"), Asset.ExplicitNumber);
			Result->SetNumberField(TEXT("owned_count"), Asset.OwnedObjects.Num());
			Result->SetNumberField(TEXT("deep_owned_count"), DeepCount);
			Result->SetBoolField(TEXT("native_default_subobjects_preserved"), bDefaults);
			Result->SetBoolField(
				TEXT("shared_reference_preserved"),
				Left && Right && Shared
					&& Left->Shared == Shared
					&& Right->Shared == Shared);
			Result->SetBoolField(
				TEXT("reference_cycle_preserved"),
				Left && Right && Left->Peer == Right && Right->Peer == Left);
			Result->SetBoolField(
				TEXT("containers_preserved"),
				Asset.Numbers == TArray<int32>({3, 5, 8})
					&& Asset.NamedNumbers.FindRef(TEXT("alpha")) == 13
					&& Asset.NamedNumbers.FindRef(TEXT("beta")) == 21);
			return Result;
		}

		class FIndependentVerificationPort final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FIndependentVerificationPort(
				TSharedRef<FExecutionContext> InContext)
				: Context(MoveTemp(InContext)) {}

			virtual bool Verify(
				const FOperationLifecyclePlan& Plan,
				const FOperationMutationFacts& Facts,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				UPackage* Package = Context->Asset
					? Context->Asset->GetOutermost()
					: nullptr;
				if (!Package)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_readback_package_missing"));
					return false;
				}
				Context->OwnedById.Reset();
				Context->Asset = nullptr;
				FText UnloadError;
				if (!UPackageTools::UnloadPackages({Package}, UnloadError, true))
				{
					OutError = FOperationLifecycleError(
						TEXT("uobject_dataasset_unload_failed"),
						UnloadError.ToString());
					return false;
				}
				UUeAgentRecursiveDataAsset* Reloaded =
					LoadObject<UUeAgentRecursiveDataAsset>(
						nullptr,
						*Context->Prepared.Target);
				if (!Reloaded)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_reload_failed"));
					return false;
				}
				Context->Asset = Reloaded;
				Context->IndependentReadback = BuildReadback(*Reloaded, true);
				bOutEquivalent = Reloaded->GetClass() ==
						UUeAgentRecursiveDataAsset::StaticClass()
					&& Reloaded->NativeCounter == 11
					&& Reloaded->Label == TEXT("explicit-root")
					&& Reloaded->ExplicitNumber == 42
					&& Context->IndependentReadback->GetBoolField(
						TEXT("native_default_subobjects_preserved"))
					&& Context->IndependentReadback->GetBoolField(
						TEXT("shared_reference_preserved"))
					&& Context->IndependentReadback->GetBoolField(
						TEXT("reference_cycle_preserved"))
					&& Context->IndependentReadback->GetBoolField(
						TEXT("containers_preserved"));
				if (!bOutEquivalent)
				{
					OutError = FOperationLifecycleError(TEXT("uobject_dataasset_readback_mismatch"));
				}
				return true;
			}

		private:
			TSharedRef<FExecutionContext> Context;
		};

		class FVerifiedDataAssetExportSource final
			: public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedDataAssetExportSource(
				FString InTarget,
				FString InCanonicalDocument)
				: Target(MoveTemp(InTarget))
				, CanonicalDocument(MoveTemp(InCanonicalDocument))
			{
			}

			virtual bool TryFreezeTarget(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

			virtual bool TryCaptureWorkspace(
				const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				FTCHARToUTF8 Utf8(*CanonicalDocument);
				TArray<uint8> Bytes;
				Bytes.Append(
					reinterpret_cast<const uint8*>(Utf8.Get()),
					Utf8.Length());
				const FString FileHash = Sha256Bytes(Bytes);
				OutSemantic = Signature(Bytes);
				OutPlan = MakeShared<FExportWorkspacePlan>(
					EWorkspaceKind::Directory,
					TEXT("asset.hjson"),
					Target,
					FrozenRevision,
					Sha256(Target + TEXT("|") + FileHash),
					TArray<FExportFilePlan>{
						FExportFilePlan(TEXT("asset.hjson"), Bytes, FileHash),
					},
					Sha256(FileHash + TEXT("|") + FrozenRevision.GetGeneration()),
					FMath::Max<int64>(1, Bytes.Num() / 4));
				return true;
			}

			virtual bool TryRebuildStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				TArray<uint8> Bytes;
				const FString Path = StagedWorkspacePath / EntryFile;
				if (!FFileHelper::LoadFileToArray(Bytes, *Path))
				{
					OutError.Set(TEXT("uobject_dataasset_export_rebuild_failed"), Path);
					return false;
				}
				OutSemantic = Signature(Bytes);
				return true;
			}

			virtual bool TryReadCurrentTargetRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

		private:
			FExportSemanticSignature Signature(
				const TArray<uint8>& Bytes) const
			{
				const FString FileHash = Sha256Bytes(Bytes);
				return FExportSemanticSignature(
					Sha256(TEXT("asset.hjson")),
					Sha256(TEXT("uobject-dataasset-reference-closure|") + FileHash),
					FileHash,
					Sha256(ContractId),
					Sha256(TEXT("uobject-dataasset-business|") + FileHash));
			}

			bool ReadRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) const
			{
				UUeAgentRecursiveDataAsset* Asset =
					LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
				if (!Asset)
				{
					OutError.Set(TEXT("uobject_dataasset_export_target_missing"), Target);
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					TEXT("uobject-dataasset-export"),
					Asset->GetOutermost()->GetPersistentGuid().ToString(
						EGuidFormats::Digits),
					1);
				return true;
			}

			FString Target;
			FString CanonicalDocument;
		};

		class FProductionService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			virtual bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(OutError, TEXT("uobject_dataasset_cancelled"), Request.CommandId);
				}
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson,
						Params,
						JsonError))
				{
					return Fail(OutError, TEXT("uobject_dataasset_params_invalid"), Request.CommandId, JsonError);
				}
				const FUeAgentUObjectDataAssetCommandRoute* Route =
					GetUObjectDataAssetCommandRoutes().FindByPredicate(
						[&Request](const FUeAgentUObjectDataAssetCommandRoute& Candidate)
						{
							return Candidate.CommandId == Request.CommandId;
						});
				if (!Route)
				{
					return Fail(OutError, TEXT("uobject_dataasset_command_not_supported"), Request.CommandId);
				}
				if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Import)
				{
					return ExecuteImport(Request, Params, OutResponse, OutError);
				}
				FString Target;
				if (!Params->TryGetStringField(TEXT("target"), Target))
				{
					return Fail(OutError, TEXT("uobject_dataasset_target_required"), Request.CommandId);
				}
				UUeAgentRecursiveDataAsset* Asset =
					LoadObject<UUeAgentRecursiveDataAsset>(nullptr, *Target);
				if (!Asset)
				{
					return Fail(OutError, TEXT("content_graph_read_target_unresolved"), Target);
				}
				if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Export)
				{
					return ExecuteExport(Request, Params, *Asset, OutResponse, OutError);
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(TEXT("mechanics_command_id"), Route->MechanicsCommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetObjectField(TEXT("query_result"), BuildReadback(*Asset, false));
				OutResponse.PayloadJson = Canonical(Payload);
				OutResponse.bReadOnly = true;
				return true;
			}

		private:
			bool ExecuteImport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				const TSharedPtr<FJsonObject>& Params,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError)
			{
				FPreparedImport Prepared;
				if (!TryPrepareImport(Params, Prepared, OutError))
				{
					return false;
				}
				const TSharedRef<FExecutionContext> Context =
					MakeShared<FExecutionContext>(MoveTemp(Prepared));
				const FOperationLifecyclePlan Plan(
					TEXT("t13:") + Request.RequestId,
					Context->Prepared.InitializationPlan->GetPlanFingerprint(),
					{Context->Prepared.Target},
					{{Context->Prepared.Target, TEXT("absent")}},
					{TEXT("asset_registered"), TEXT("owned_objects_created")});
				const FOperationImpactAuthorization Authorization(
					Plan.GetPlanFingerprint(),
					{Context->Prepared.Target});
				const FOperationLifecycleService Lifecycle(
					MakeShared<FVersionFence>(Context),
					MakeShared<FMutationPort>(Context),
					MakeShared<FStabilityPort>(Context),
					MakeShared<FPersistencePort>(Context),
					MakeShared<FIndependentVerificationPort>(Context));
				FOperationLifecycleReceipt Receipt;
				if (!Lifecycle.Run(Plan, Authorization, Receipt))
				{
					const TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("terminal_status"), LexToString(Receipt.GetStatus()));
					Detail->SetBoolField(TEXT("mutation_started"), Receipt.DidMutationStart());
					Detail->SetStringField(TEXT("error_code"), Receipt.GetErrorCode());
					return Fail(
						OutError,
						Receipt.GetErrorCode().IsEmpty()
							? FString(TEXT("uobject_dataasset_operation_failed"))
							: Receipt.GetErrorCode(),
						Context->Prepared.Target,
						Canonical(Detail));
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(TEXT("terminal_status"), LexToString(Receipt.GetStatus()));
				Payload->SetBoolField(TEXT("mutation_started"), Receipt.DidMutationStart());
				Payload->SetStringField(TEXT("persistence_receipt_id"), Receipt.GetPersistenceReceiptId());
				Payload->SetStringField(TEXT("plan_fingerprint"), Plan.GetPlanFingerprint());
				TArray<TSharedPtr<FJsonValue>> Phases;
				for (const FString& Phase : Receipt.GetPhaseHistory())
				{
					Phases.Add(MakeShared<FJsonValueString>(Phase));
				}
				Payload->SetArrayField(TEXT("phase_history"), MoveTemp(Phases));
				Payload->SetObjectField(
					TEXT("independent_readback"),
					Context->IndependentReadback.ToSharedRef());
				OutResponse.PayloadJson = Canonical(Payload);
				OutResponse.bReadOnly = false;
				return true;
			}

			bool ExecuteExport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				const TSharedPtr<FJsonObject>& Params,
				UUeAgentRecursiveDataAsset& Asset,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError)
			{
				FString WorkspacePath;
				if (!Params->TryGetStringField(TEXT("workspace_path"), WorkspacePath)
					|| Normalize(WorkspacePath).IsEmpty())
				{
					return Fail(OutError, TEXT("uobject_dataasset_export_path_required"));
				}
				const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
				Document->SetStringField(TEXT("schema"), ContractId);
				Document->SetStringField(TEXT("target"), Asset.GetPathName());
				Document->SetObjectField(TEXT("readback"), BuildReadback(Asset, false));
				FUeAgentResourceTracker Resources = MakeResources();
				FArtifactPublishReceipt Receipt;
				FArtifactPublishError PublishError;
				if (!TryExportVerifiedWorkspace(
						FVerifiedExportRequest(
							MakeShared<FVerifiedDataAssetExportSource>(
								Asset.GetPathName(),
								Canonical(Document)),
							WorkspacePath,
							TEXT("uobject-dataasset-recursive-initialization-owner")),
						Resources,
						Receipt,
						PublishError))
				{
					return Fail(
						OutError,
						PublishError.GetCode(),
						PublishError.GetPath(),
						PublishError.GetDetail());
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(TEXT("status"), TEXT("published"));
				Payload->SetStringField(TEXT("workspace_path"), Receipt.GetFinalPath());
				Payload->SetStringField(TEXT("entry_file"), Receipt.GetEntryFile());
				Payload->SetStringField(TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
				Payload->SetStringField(TEXT("inventory_hash"), Receipt.GetInventoryHash());
				const TSharedPtr<FJsonObject> Semantic = MakeShared<FJsonObject>();
				for (const TPair<FString, FString>& Field
					: Receipt.GetSemanticVerificationFields())
				{
					Semantic->SetStringField(Field.Key, Field.Value);
				}
				Payload->SetObjectField(TEXT("semantic_verification"), Semantic);
				OutResponse.PayloadJson = Canonical(Payload);
				OutResponse.bReadOnly = true;
				return true;
			}
		};

		bool TryBuildDomainRequest(
			const FUeAgentUObjectDataAssetCommandRequest& Request,
			TOptional<FImportInitializationDomainRequest>& OutRequest,
			FUeAgentUObjectDataAssetCommandError& OutError)
		{
			OutRequest.Reset();
			TSharedPtr<FJsonObject> Params;
			FString JsonError;
			if (!Json::TryParseObject(
					Request.CanonicalParamsJson,
					Params,
					JsonError))
			{
				return Fail(
					OutError,
					TEXT("hjson_domain_params_invalid"),
					Request.CommandId,
					JsonError);
			}
			const FUeAgentUObjectDataAssetCommandRoute* Route =
				GetUObjectDataAssetCommandRoutes().FindByPredicate(
					[&Request](
						const FUeAgentUObjectDataAssetCommandRoute& Candidate)
					{
						return Candidate.CommandId == Request.CommandId;
					});
			if (!Route)
			{
				return Fail(
					OutError,
					TEXT("hjson_domain_command_unregistered"),
					Request.CommandId);
			}
			FString Target;
			if (!Params->TryGetStringField(TEXT("target"), Target)
				|| Normalize(Target).IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("hjson_domain_target_required"),
					Request.CommandId);
			}
			FString SchemaId = TEXT("uai.content_graph.resolved_target/1");
			FString RootTypeIdentity;
			if (Route->Kind == EUeAgentUObjectDataAssetRouteKind::Import)
			{
				if (Params->HasField(TEXT("carrier"))
					|| Params->HasField(TEXT("hjson_document")))
				{
					return Fail(
						OutError,
						TEXT("content_graph_legacy_source_contract_rejected"),
						Target);
				}
				const TSharedPtr<FJsonObject>* SourceObject = nullptr;
				if (!Params->TryGetObjectField(TEXT("source"), SourceObject)
					|| !SourceObject || !SourceObject->IsValid())
				{
					return Fail(
						OutError,
						TEXT("content_graph_source_required"),
						Target);
				}
				FUeAgentResourceTracker Resources = MakeResources();
				FResolvedContentGraphImportSourcePtr ResolvedSource;
				FWorkspaceError WorkspaceError;
				if (!TryLoadContentGraphImportSource(
						*SourceObject,
						Target,
						Resources,
						ResolvedSource,
						WorkspaceError)
					|| !ResolvedSource.IsValid()
					|| !ResolvedSource->GetEntryDocument())
				{
					return Fail(
						OutError,
						WorkspaceError.GetCode().IsEmpty()
							? FString(TEXT("content_graph_source_invalid"))
							: WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				const FString& DocumentText =
					ResolvedSource->GetEntryDocument()->GetDocument();
				FHjsonDocumentPtr Document;
				if (!TryParseHjsonDocument(
						DocumentText,
						ResolvedSource->GetSourceUnit(),
						Resources,
						Document,
						WorkspaceError)
					|| !Document.IsValid()
					|| Document->GetRoot()->GetKind()
						!= EHjsonNodeKind::Object)
				{
					return Fail(
						OutError,
						WorkspaceError.GetCode().IsEmpty()
							? FString(TEXT("hjson_domain_document_invalid"))
							: WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				const FHjsonNode& DocumentRoot = *Document->GetRoot();
				const FHjsonNode* Schema = DocumentRoot.Get(TEXT("schema"));
				const FHjsonNode* Root = DocumentRoot.Get(TEXT("root"));
				const FHjsonNode* Type = Root
					&& Root->GetKind() == EHjsonNodeKind::Object
					? Root->Get(TEXT("type"))
					: nullptr;
				if (!Schema || Schema->GetKind() != EHjsonNodeKind::String
					|| !Type || Type->GetKind() != EHjsonNodeKind::String)
				{
					return Fail(
						OutError,
						TEXT("hjson_domain_identity_required"),
						Target);
				}
				SchemaId = Normalize(Schema->GetText());
				RootTypeIdentity = Normalize(Type->GetText());
				const TSharedRef<FJsonObject> NormalizedParams =
					MakeShared<FJsonObject>();
				NormalizedParams->Values = Params->Values;
				NormalizedParams->SetObjectField(
					TEXT("source"),
					ResolvedSource->MakeNormalizedSourceObject());
				FString NormalizedJson;
				FString NormalizedError;
				if (!Json::TryCanonicalize(
						NormalizedParams,
						NormalizedJson,
						NormalizedError))
				{
					return Fail(
						OutError,
						TEXT("content_graph_resolved_source_invalid"),
						Target,
						NormalizedError);
				}
				OutRequest.Emplace(
					Request.CommandId,
					MoveTemp(SchemaId),
					MoveTemp(Target),
					MoveTemp(RootTypeIdentity),
					EInitializationNodeRole::RootTarget,
					MoveTemp(NormalizedJson));
			}
			else
			{
				UObject* TargetObject = StaticLoadObject(
					UObject::StaticClass(),
					nullptr,
					*Target);
				if (!TargetObject)
				{
					return Fail(
						OutError,
						TEXT("content_graph_read_target_unresolved"),
						Target);
				}
				RootTypeIdentity = TargetObject->GetClass()->GetPathName();
			}
			if (!OutRequest.IsSet())
			{
				OutRequest.Emplace(
					Request.CommandId,
					MoveTemp(SchemaId),
					MoveTemp(Target),
					MoveTemp(RootTypeIdentity),
					EInitializationNodeRole::RootTarget,
					Request.CanonicalParamsJson);
			}
			if (!OutRequest->IsValid())
			{
				OutRequest.Reset();
				return Fail(
					OutError,
					TEXT("hjson_domain_request_invalid"),
					Request.CommandId);
			}
			return true;
		}

		class FUObjectDataAssetDomainProvider final
			: public IImportInitializationDomainProvider
		{
		public:
			virtual const FString& GetProviderId() const override
			{
				return DomainProviderId;
			}

			virtual bool Matches(
				const FImportInitializationDomainRequest& Request)
				const override
			{
				if (Request.GetRootTypeIdentity() != RootType
					&& Request.GetRootTypeIdentity() != OwnedType)
				{
					return false;
				}
				return Request.GetCommandId()
						!= TEXT("content_graph.import_workspace")
					|| Request.GetSchemaId() == ContractId;
			}

			virtual bool TryResolveInitializationContract(
				const FImportInitializationDomainRequest& Request,
				FString& OutInitializationContractId,
				FWorkspaceError& OutError) const override
			{
				OutInitializationContractId.Reset();
				OutError.Reset();
				if (Request.GetRole() == EInitializationNodeRole::RootTarget
					&& Request.GetRootTypeIdentity() == RootType)
				{
					OutInitializationContractId =
						RootInitializationContractId;
					return true;
				}
				if (Request.GetRole() == EInitializationNodeRole::OwnedObject
					&& Request.GetRootTypeIdentity() == OwnedType)
				{
					OutInitializationContractId =
						OwnedInitializationContractId;
					return true;
				}
				return false;
			}

			virtual bool TryExecute(
				const FImportInitializationDomainRequest& Request,
				const FString& InitializationContractId,
				FImportInitializationDomainResponse& OutResponse,
				FWorkspaceError& OutError) const override
			{
				OutResponse = {};
				OutError.Reset();
				if (InitializationContractId
					!= RootInitializationContractId)
				{
					OutError.Set(
						TEXT("hjson_domain_contract_not_executable"),
						InitializationContractId);
					return false;
				}
				FUeAgentUObjectDataAssetCommandRequest DomainRequest;
				DomainRequest.CommandId = Request.GetCommandId();
				DomainRequest.RequestId = Sha256(
					Request.GetCommandId()
					+ TEXT("|")
					+ Request.GetCanonicalParamsJson());
				DomainRequest.CanonicalParamsJson =
					Request.GetCanonicalParamsJson();
				FUeAgentUObjectDataAssetCommandResponse DomainResponse;
				FUeAgentUObjectDataAssetCommandError DomainError;
				FProductionService Service;
				if (!Service.Execute(
						DomainRequest,
						DomainResponse,
						DomainError))
				{
					OutError.Set(
						DomainError.Code.IsEmpty()
							? FString(TEXT("hjson_domain_execution_failed"))
							: DomainError.Code,
						DomainError.SubjectId,
						DomainError.Detail.IsEmpty()
							? FString(TEXT("/"))
							: DomainError.Detail);
					return false;
				}
				OutResponse = FImportInitializationDomainResponse(
					MoveTemp(DomainResponse.PayloadJson),
					DomainResponse.bReadOnly);
				return OutResponse.IsValid();
			}
		};

		bool TryCreateUObjectDataAssetDomainProviderInternal(
			TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
			FWorkspaceError& OutError)
		{
			OutProvider = MakeShared<FUObjectDataAssetDomainProvider>();
			OutError.Reset();
			return true;
		}

		class FRegistryRoutedProductionService final
			: public IUeAgentUObjectDataAssetCommandService
		{
		public:
			explicit FRegistryRoutedProductionService(
				TSharedRef<const FImportInitializationDomainProviderRegistry>
					InRegistry)
				: Registry(MoveTemp(InRegistry))
			{
			}

			virtual bool Execute(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) override
			{
				OutResponse = {};
				OutError = {};
				if (Request.bCancellationRequested)
				{
					return Fail(
						OutError,
						TEXT("uobject_dataasset_cancelled"),
						Request.CommandId);
				}
				if (Request.CommandId == TEXT("content_graph.export_workspace"))
				{
					return ExecuteSharedExport(
						Request, OutResponse, OutError);
				}
				TOptional<FImportInitializationDomainRequest> DomainRequest;
				if (!TryBuildDomainRequest(Request, DomainRequest, OutError)
					|| !DomainRequest.IsSet())
				{
					return false;
				}
				FImportInitializationDomainResolution Resolution;
				FWorkspaceError WorkspaceError;
				if (!Registry->TryResolve(
						DomainRequest.GetValue(),
						Resolution,
						WorkspaceError)
					|| !Resolution.IsValid())
				{
					return Fail(
						OutError,
						WorkspaceError.GetCode().IsEmpty()
							? FString(TEXT("hjson_domain_provider_unresolved"))
							: WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				FImportInitializationDomainResponse DomainResponse;
				if (!Resolution.GetProvider()->TryExecute(
						DomainRequest.GetValue(),
						Resolution.GetInitializationContractId(),
						DomainResponse,
						WorkspaceError)
					|| !DomainResponse.IsValid())
				{
					return Fail(
						OutError,
						WorkspaceError.GetCode().IsEmpty()
							? FString(TEXT("hjson_domain_execution_failed"))
							: WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				OutResponse.PayloadJson = DomainResponse.GetPayloadJson();
				OutResponse.bReadOnly = DomainResponse.IsReadOnly();
				return true;
			}

		private:
			bool ExecuteSharedExport(
				const FUeAgentUObjectDataAssetCommandRequest& Request,
				FUeAgentUObjectDataAssetCommandResponse& OutResponse,
				FUeAgentUObjectDataAssetCommandError& OutError) const
			{
				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				FString Target;
				FString WorkspacePath;
				FString ProfileId(TEXT("llm_authoring"));
				if (!Json::TryParseObject(
						Request.CanonicalParamsJson, Params, JsonError)
					|| !Params.IsValid()
					|| !Params->TryGetStringField(TEXT("target"), Target)
					|| !Params->TryGetStringField(
						TEXT("workspace_path"), WorkspacePath))
				{
					return Fail(
						OutError,
						TEXT("content_graph_export_params_invalid"),
						Request.CommandId,
						JsonError);
				}
				Params->TryGetStringField(TEXT("profile_id"), ProfileId);
				Target = Normalize(Target);
				WorkspacePath = Normalize(WorkspacePath);
				ProfileId = Normalize(ProfileId);
				FString LoadPath = Target;
				if (!LoadPath.Contains(TEXT(".")))
				{
					const FString AssetName =
						FPackageName::GetLongPackageAssetName(LoadPath);
					LoadPath = AssetName.IsEmpty()
						? LoadPath : LoadPath + TEXT(".") + AssetName;
				}
				UObject* Object = StaticLoadObject(
					UObject::StaticClass(), nullptr, *LoadPath);
				if (!Object || WorkspacePath.IsEmpty() || ProfileId.IsEmpty())
				{
					return Fail(
						OutError,
						Object
							? FString(TEXT("content_graph_export_path_required"))
							: FString(TEXT("content_graph_read_target_unresolved")),
						Target);
				}

				TSharedPtr<const DomainCapability::FFrozenDomainCapabilitySet>
					FrozenCapabilities;
				FString CapabilityError;
				if (!DomainCapability::TryFreezeProcessDomainCapabilities(
						FrozenCapabilities, CapabilityError)
					|| !FrozenCapabilities.IsValid())
				{
					return Fail(
						OutError,
						TEXT("content_graph_export_domain_capabilities_unavailable"),
						Target,
						CapabilityError);
				}
				DomainCapability::FDomainCapabilityAuthorities Authorities;
				if (!FrozenCapabilities->TryBuildAuthorities(
						Authorities, CapabilityError))
				{
					return Fail(
						OutError,
						TEXT("content_graph_export_domain_authorities_unavailable"),
						Target,
						CapabilityError);
				}
				const FString ClassPath = Object->GetClass()->GetPathName();
				FString TargetKind;
				for (const TSharedRef<const DomainCapability::
					FDomainCapabilityContribution>& Contribution
					: FrozenCapabilities->GetContributions())
				{
					for (const DomainCapability::FDomainHjsonRoute& Route
						: Contribution->GetDeclaration().Hjson.Routes)
					{
						if (Route.CommandId
								== TEXT("content_graph.import_workspace")
							&& Route.RootTypeIdentity == ClassPath)
						{
							if (!TargetKind.IsEmpty()
								&& TargetKind != Route.TargetKind)
							{
								return Fail(
									OutError,
									TEXT("content_graph_export_target_kind_ambiguous"),
									ClassPath);
							}
							TargetKind = Route.TargetKind;
						}
					}
				}
				if (TargetKind.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("content_graph_export_target_kind_unresolved"),
						ClassPath);
				}

				DomainAdapters::ContentGraphRead::FUeAgentContentGraphCapture
					Capture;
				DomainAdapters::ContentGraphRead::FUeAgentContentGraphReadError
					CaptureError;
				if (!DomainAdapters::ContentGraphRead::
						FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
							*Object, Capture, CaptureError))
				{
					return Fail(
						OutError,
						CaptureError.Code,
						CaptureError.SubjectId,
						CaptureError.Detail);
				}

				const Contract::FUeAgentDescriptorIdentity DescriptorIdentity(
					TEXT("uai.query.content_graph.compound"),
					1,
					TEXT("sha256:18e5a1357c045a15637fce2f530a7b1d337a5b6cd8d2aac08c958e106d810065"));

				const FUeAgentResourceTrackerOwner Resources =
					MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
						FUeAgentResourceBudget({
							{EUeAgentResourceDimension::InputBytes, 16ll * 1024 * 1024},
							{EUeAgentResourceDimension::OutputBytes, 64ll * 1024 * 1024},
							{EUeAgentResourceDimension::StringBytes, 64ll * 1024 * 1024},
							{EUeAgentResourceDimension::Nodes, 65536},
							{EUeAgentResourceDimension::Items, 65536},
							{EUeAgentResourceDimension::Files, 4096},
							{EUeAgentResourceDimension::ReferenceEdges, 65536},
							{EUeAgentResourceDimension::WorkUnits, 262144},
							{EUeAgentResourceDimension::EstimatedMemoryBytes,
								512ll * 1024 * 1024},
						}));
				FUeAgentExecutionContextInput ExecutionInput;
				ExecutionInput.ProjectInstanceId = TEXT("content-graph-export-project");
				ExecutionInput.EditorInstanceId = TEXT("content-graph-export-editor");
				ExecutionInput.StatePlane = EUeAgentStatePlane::AuthoringMemory;
				ExecutionInput.WorldType = EUeAgentExecutionWorldType::Editor;
				ExecutionInput.ContextRevision = Capture.Revision;
				const Contract::FUeAgentExecutionContextPolicy ContextPolicy(
					{EUeAgentStatePlane::AuthoringMemory}, {},
					Contract::EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
					Contract::EUeAgentTransientContextPolicy::ReadOnly);
				FUeAgentExecutionContextRefPtr ExecutionContext;
				FUeAgentExecutionContextError ExecutionError;
				if (!FUeAgentExecutionContextRef::TryCreate(
						ExecutionInput,
						DescriptorIdentity,
						EUeAgentRequestKind::Query,
						ContextPolicy,
						ExecutionContext,
						ExecutionError)
					|| !ExecutionContext.IsValid())
				{
					return Fail(
						OutError,
						TEXT("content_graph_export_context_unavailable"),
						Target,
						ExecutionError.Code);
				}
				const Contract::FUeAgentQueryRequest QueryRequest(
					DescriptorIdentity,
					Request.RequestId,
					Contract::FUeAgentContractValue(TEXT("{}")));
				const Contract::FUeAgentCallerContext Caller(
					Request.CallerId,
					Request.AuthorityScopes,
					Request.CorrelationId);
				const auto Clock = []() { return FPlatformTime::Seconds(); };
				const FUeAgentOperationDeadline Deadline =
					FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
				const FUeAgentCancellationSource Cancellation(Clock);
				TSharedPtr<FUeAgentOperationContext> OperationContext;
				FUeAgentOperationContextError OperationContextError;
				if (!FUeAgentOperationContext::TryCreate(
						QueryRequest,
						Caller,
						ExecutionContext.ToSharedRef(),
						Resources,
						Deadline,
						Cancellation.GetToken(),
						OperationContext,
						OperationContextError)
					|| !OperationContext.IsValid())
				{
					return Fail(
						OutError,
						OperationContextError.Code,
						Target);
				}
				TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot> Snapshot;
				if (!DomainAdapters::ContentGraphRead::
						TryBuildContentGraphCompoundSnapshot(
							Capture,
							*OperationContext,
							Snapshot,
							CaptureError)
					|| !Snapshot.IsValid())
				{
					return Fail(
						OutError,
						CaptureError.Code,
						CaptureError.SubjectId,
						CaptureError.Detail);
				}

				FExportWorkspacePlanPtr Plan;
				FWorkspaceError WorkspaceError;
				if (!TryBuildSnapshotExportPlan(
						FSnapshotCompositionRequest(
							TargetKind, ProfileId, Snapshot.ToSharedRef()),
						{{TEXT("state_entry_count"), Snapshot->GetEntries().Num()}},
						*Resources,
						Plan,
						WorkspaceError)
					|| !Plan.IsValid())
				{
					return Fail(
						OutError,
						WorkspaceError.GetCode(),
						WorkspaceError.GetSourceUnit(),
						WorkspaceError.GetJsonPointer());
				}
				FArtifactPublishReceipt Receipt;
				FArtifactPublishError PublishError;
				if (!TryPublishExportPlan(
						FArtifactPublishRequest(
							Plan.ToSharedRef(), WorkspacePath,
							TEXT("content_graph.export_workspace")),
						*Resources,
						Receipt,
						PublishError))
				{
					return Fail(
						OutError,
						PublishError.GetCode(),
						PublishError.GetPath(),
						PublishError.GetDetail());
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Request.CommandId);
				Payload->SetStringField(TEXT("status"), TEXT("published"));
				Payload->SetStringField(
					TEXT("workspace_path"), Receipt.GetFinalPath());
				Payload->SetStringField(TEXT("entry_file"), Receipt.GetEntryFile());
				Payload->SetStringField(
					TEXT("plan_fingerprint"), Receipt.GetPlanFingerprint());
				Payload->SetStringField(
					TEXT("inventory_hash"), Receipt.GetInventoryHash());
				Payload->SetStringField(TEXT("target_kind"), TargetKind);
				OutResponse.PayloadJson = Canonical(Payload);
				OutResponse.bReadOnly = true;
				return true;
			}

			TSharedRef<const FImportInitializationDomainProviderRegistry>
				Registry;
		};
	}

	bool FUeAgentUObjectDataAssetCommandRoute::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& !PythonFunction.IsEmpty()
			&& !MechanicsCommandId.IsEmpty();
	}

	const TArray<FUeAgentUObjectDataAssetCommandRoute>&
	GetUObjectDataAssetCommandRoutes()
	{
		static const TArray<FUeAgentUObjectDataAssetCommandRoute> Routes{
			{TEXT("content_graph.compound"), TEXT("content_graph_compound"), TEXT("content_graph.read.compound"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.diagnostics"), TEXT("content_graph_diagnostics"), TEXT("content_graph.read.diagnostics"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.export_workspace"), TEXT("content_graph_export_workspace"), TEXT("content_graph.export.verified"), EUeAgentUObjectDataAssetRouteKind::Export, true},
			{TEXT("content_graph.import_workspace"), TEXT("content_graph_import_workspace"), TEXT("content_graph.import.recursive"), EUeAgentUObjectDataAssetRouteKind::Import, true},
			{TEXT("content_graph.items"), TEXT("content_graph_items"), TEXT("content_graph.read.items"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.properties"), TEXT("content_graph_properties"), TEXT("content_graph.read.properties"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.relations"), TEXT("content_graph_relations"), TEXT("content_graph.read.relations"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.structure"), TEXT("content_graph_structure"), TEXT("content_graph.read.structure"), EUeAgentUObjectDataAssetRouteKind::Query, true},
			{TEXT("content_graph.summary"), TEXT("content_graph_summary"), TEXT("content_graph.read.summary"), EUeAgentUObjectDataAssetRouteKind::Query, true},
		};
		return Routes;
	}

	bool TryCreateUObjectDataAssetDomainProvider(
		TSharedPtr<IImportInitializationDomainProvider>& OutProvider,
		FWorkspaceError& OutError)
	{
		return TryCreateUObjectDataAssetDomainProviderInternal(
			OutProvider,
			OutError);
	}

	bool TryCreateProductionUObjectDataAssetCommandService(
		TSharedPtr<IUeAgentUObjectDataAssetCommandService>& OutService,
		FString& OutError)
	{
		OutError.Reset();
		TSharedPtr<const FImportInitializationDomainProviderRegistry>
			Registry;
		FWorkspaceError WorkspaceError;
		if (!TryFreezeProcessImportInitializationDomainProviders(
				Registry,
				WorkspaceError)
			|| !Registry.IsValid())
		{
			OutService.Reset();
			OutError = WorkspaceError.GetCode().IsEmpty()
				? FString(TEXT("hjson_domain_provider_registry_unavailable"))
				: WorkspaceError.GetCode();
			return false;
		}
		OutService = MakeShared<FRegistryRoutedProductionService>(
			Registry.ToSharedRef());
		return true;
	}

	namespace TestHooks
	{
		FScopedFailureAfterNode::FScopedFailureAfterNode(FString NodeId)
			: Previous(FailureAfterNode)
		{
			FailureAfterNode = Normalize(MoveTemp(NodeId));
		}

		FScopedFailureAfterNode::~FScopedFailureAfterNode()
		{
			FailureAfterNode = MoveTemp(Previous);
		}
	}
}
