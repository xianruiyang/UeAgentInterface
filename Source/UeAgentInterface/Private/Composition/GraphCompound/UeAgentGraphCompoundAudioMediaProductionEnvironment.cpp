#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaIdentity.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphMetaSoundNativeHook.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphSoundCueNativeHook.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "Dom/JsonObject.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "HAL/FileManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_Pin.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace BindingGateway::Operation;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::AudioMediaGraph;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		const FString ReadbackOracleId =
			TEXT("uai.readback.graph_compound.audio_media");

		enum class EAudioMediaReadbackKind : uint8
		{
			AssetCreate,
			MetaSoundInterface,
			MetaSoundNodes,
			MetaSoundLinks,
			MetaSoundDefault,
			MetaSoundCompileSettle,
			SoundCueNodes,
			SoundCueRelations,
			SoundCueRoot,
			SoundCueCompileSettle,
			TextureGraphNodes,
			TextureGraphLinks,
			TextureGraphParameters,
			TextureGraphValidateEvaluateSettle,
		};

		struct FAudioMediaReadbackSpec final
		{
			EAudioMediaReadbackKind Kind = EAudioMediaReadbackKind::AssetCreate;
			FString AssetKind;
			FString NativeClassRef;
			FString ExpectedRevision;
			TArray<FAudioMediaGraphMetaSoundInterfaceMutation> MetaInterfaces;
			TArray<FAudioMediaGraphMetaSoundNodeMutation> MetaNodes;
			TArray<FAudioMediaGraphMetaSoundLinkMutation> MetaLinks;
			TOptional<FAudioMediaGraphMetaSoundDefaultMutation> MetaDefault;
			TArray<FAudioMediaGraphSoundCueNodeMutation> SoundNodes;
			TArray<FAudioMediaGraphSoundCueRelationMutation> SoundRelations;
			FString SoundRoot;
			TArray<FAudioMediaGraphTextureGraphNodeMutation> TextureNodes;
			TArray<FAudioMediaGraphTextureGraphLinkMutation> TextureLinks;
			TArray<FAudioMediaGraphTextureGraphParameterMutation> TextureParameters;
		};

		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		FString TargetPackageName(const FString& TargetRef)
		{
			return FPackageName::ObjectPathToPackageName(TargetRef);
		}

		FString TargetAssetName(const FString& TargetRef)
		{
			return FPackageName::GetLongPackageAssetName(
				TargetPackageName(TargetRef));
		}

		FString TargetObjectPath(const FString& TargetRef)
		{
			const FString PackageName = TargetPackageName(TargetRef);
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool TryUnloadTarget(
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString PackageName = TargetPackageName(TargetRef);
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (Package == nullptr)
			{
				return true;
			}
			if (bNotifyAssetRegistry)
			{
				if (UObject* const Asset = FindObject<UObject>(
						Package, *TargetAssetName(TargetRef)))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
				}
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *PackageName) != nullptr)
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
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
			const FString& TargetRef,
			const bool bNotifyAssetRegistry,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadTarget(
				TargetRef, bNotifyAssetRegistry, UnloadDetail);
			FDurableReceipt Receipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, Receipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail : Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		const FUeAgentCanonicalValue* FindField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Name)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryStringField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FString& OutValue)
		{
			const FUeAgentCanonicalValue* const Value = FindField(Object, Name);
			if (Value == nullptr || Value->GetKind() != EUeAgentValueKind::String)
			{
				return false;
			}
			OutValue = Value->GetText();
			return !OutValue.IsEmpty();
		}

		bool TryIntegerField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			int32& OutValue)
		{
			const FUeAgentCanonicalValue* const Value = FindField(Object, Name);
			if (Value == nullptr
				|| (Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal))
			{
				return false;
			}
			const double Number = FCString::Atod(*Value->GetLexeme());
			const double Rounded = FMath::RoundToDouble(Number);
			if (!FMath::IsFinite(Number) || !FMath::IsNearlyEqual(Number, Rounded)
				|| Rounded < static_cast<double>(MIN_int32)
				|| Rounded > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Rounded);
			return true;
		}

		bool TryGuidField(
			const FUeAgentCanonicalValue& Object,
			const FString& Name,
			FGuid& OutValue)
		{
			FString Text;
			return TryStringField(Object, Name, Text)
				&& FGuid::Parse(Text, OutValue) && OutValue.IsValid();
		}

		bool TryMetaSoundLiteral(
			const FUeAgentCanonicalValue* Value,
			const FString& DataType,
			FMetasoundFrontendLiteral& OutLiteral)
		{
			if (DataType == TEXT("Float"))
			{
				const double Number = Value == nullptr ? 0.0
					: FCString::Atod(*Value->GetLexeme());
				if (Value != nullptr
					&& Value->GetKind() != EUeAgentValueKind::Integer
					&& Value->GetKind() != EUeAgentValueKind::Decimal)
				{
					return false;
				}
				OutLiteral.Set(static_cast<float>(Number));
				return FMath::IsFinite(Number);
			}
			if (DataType == TEXT("Int32"))
			{
				const int32 Number = Value == nullptr ? 0
					: FCString::Atoi(*Value->GetLexeme());
				if (Value != nullptr
					&& Value->GetKind() != EUeAgentValueKind::Integer)
				{
					return false;
				}
				OutLiteral.Set(Number);
				return true;
			}
			if (DataType == TEXT("Bool"))
			{
				if (Value != nullptr
					&& Value->GetKind() != EUeAgentValueKind::Boolean)
				{
					return false;
				}
				OutLiteral.Set(Value != nullptr && Value->GetBoolean());
				return true;
			}
			if (DataType == TEXT("String"))
			{
				if (Value != nullptr
					&& Value->GetKind() != EUeAgentValueKind::String)
				{
					return false;
				}
				OutLiteral.Set(Value == nullptr ? FString() : Value->GetText());
				return true;
			}
			return false;
		}

		bool TryTextureGraphValue(
			const FUeAgentCanonicalValue* Value,
			FAudioMediaGraphTextureGraphValue& OutValue)
		{
			if (Value == nullptr)
			{
				return false;
			}
			if (Value->GetKind() == EUeAgentValueKind::Integer
				|| Value->GetKind() == EUeAgentValueKind::Decimal)
			{
				const double Number = FCString::Atod(*Value->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return false;
				}
				OutValue = FAudioMediaGraphTextureGraphValue::Number(Number);
				return true;
			}
			if (Value->GetKind() == EUeAgentValueKind::Boolean)
			{
				OutValue = FAudioMediaGraphTextureGraphValue::Boolean(
					Value->GetBoolean());
				return true;
			}
			if (Value->GetKind() == EUeAgentValueKind::String)
			{
				OutValue = FAudioMediaGraphTextureGraphValue::String(
					Value->GetText());
				return true;
			}
			if (Value->GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			TArray<double> Numbers;
			for (const FUeAgentCanonicalValueRef& Item : Value->GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::Integer
					&& Item->GetKind() != EUeAgentValueKind::Decimal)
				{
					return false;
				}
				const double Number = FCString::Atod(*Item->GetLexeme());
				if (!FMath::IsFinite(Number))
				{
					return false;
				}
				Numbers.Add(Number);
			}
			if (Numbers.Num() != 4)
			{
				return false;
			}
			OutValue = FAudioMediaGraphTextureGraphValue::NumberArray(
				MoveTemp(Numbers));
			return true;
		}

		bool TryCanonicalObject(
			const TSharedRef<FJsonObject>& Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FString Error;
			if (!CanonicalJson::TryFromJsonValue(
					MakeShared<FJsonValueObject>(Object),
					Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_value_invalid"),
					FString(), Error);
				return false;
			}
			return true;
		}

		EAudioMediaGraphMutationIntent NativeIntent(const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EAudioMediaGraphMutationIntent::Create;
			case EChangeIntent::Update:
				return EAudioMediaGraphMutationIntent::Update;
			case EChangeIntent::Replace:
				return EAudioMediaGraphMutationIntent::Replace;
			case EChangeIntent::Delete:
				return EAudioMediaGraphMutationIntent::Delete;
			}
			return EAudioMediaGraphMutationIntent::Update;
		}

		bool TryIdentitySelector(
			const FGraphCompoundCanonicalEdit& Edit,
			const FString& Name,
			FString& OutValue)
		{
			OutValue = Edit.GetIdentity().GetSelector(Name);
			return !OutValue.IsEmpty();
		}

		bool TryIdentityGuid(
			const FGraphCompoundCanonicalEdit& Edit,
			const FString& Name,
			FGuid& OutValue)
		{
			FString Text;
			return TryIdentitySelector(Edit, Name, Text)
				&& FGuid::Parse(Text, OutValue);
		}

		bool TryParseReadbackSpec(
			const FAudioMediaGraphRouteProfile& Profile,
			const FGraphCompoundAudioMediaOperationRequest& Request,
			FAudioMediaReadbackSpec& OutSpec,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSpec = {};
			const TArray<FGraphCompoundCanonicalEdit>& Edits =
				Request.GetPayload().GetEdits();
			if (Edits.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_payload_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
			if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
			{
				OutSpec.Kind = EAudioMediaReadbackKind::AssetCreate;
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				if (!Value.IsValid()
					|| !TryStringField(*Value, TEXT("asset_kind"), OutSpec.AssetKind)
					|| !TryStringField(*Value, TEXT("native_class_ref"),
						OutSpec.NativeClassRef))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_asset_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				return true;
			}

			switch (Profile.GetInputKind())
			{
			case EAudioMediaGraphInputKind::MetaSoundInterface:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundInterface;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FString InterfaceKind;
						FString Name;
						if (!TryIdentitySelector(
								Edit, TEXT("interface_kind"), InterfaceKind)
							|| !TryIdentitySelector(Edit, TEXT("name"), Name))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_metasound_interface_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.MetaInterfaces.Emplace(
							MoveTemp(InterfaceKind),
							FName(*Name),
							NAME_None,
							false,
							FMetasoundFrontendLiteral(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString InterfaceKind;
					FString Name;
					FString DataType;
					FString AccessType;
					FMetasoundFrontendLiteral Default;
					if (!Value.IsValid()
						|| !TryStringField(*Value, TEXT("interface_kind"), InterfaceKind)
						|| !TryStringField(*Value, TEXT("name"), Name)
						|| !TryStringField(*Value, TEXT("data_type_ref"), DataType)
						|| !TryStringField(*Value, TEXT("access_type"), AccessType)
						|| !TryMetaSoundLiteral(
							FindField(*Value, TEXT("default_value")), DataType, Default))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_metasound_interface_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.MetaInterfaces.Emplace(
						MoveTemp(InterfaceKind), FName(*Name), FName(*DataType),
						AccessType == TEXT("value"), MoveTemp(Default),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundNode:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FGuid NodeId;
						if (!TryIdentityGuid(Edit, TEXT("node_guid"), NodeId))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_metasound_node_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.MetaNodes.Emplace(
							NodeId,
							FString(),
							0,
							0,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FGuid NodeId;
					FString ClassName;
					int32 Major = 0;
					int32 Minor = 0;
					if (!Value.IsValid()
						|| !TryGuidField(*Value, TEXT("node_guid"), NodeId)
						|| !TryStringField(*Value, TEXT("class_name"), ClassName)
						|| !TryIntegerField(*Value, TEXT("major_version"), Major)
						|| !TryIntegerField(*Value, TEXT("minor_version"), Minor))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_metasound_node_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.MetaNodes.Emplace(
						NodeId, MoveTemp(ClassName), Major, Minor,
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundLink:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundLinks;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FGuid FromNode;
						FGuid FromVertex;
						FGuid ToNode;
						FGuid ToVertex;
						if (!TryIdentityGuid(
								Edit, TEXT("from_node_guid"), FromNode)
							|| !TryIdentityGuid(
								Edit, TEXT("from_vertex_guid"), FromVertex)
							|| !TryIdentityGuid(Edit, TEXT("to_node_guid"), ToNode)
							|| !TryIdentityGuid(
								Edit, TEXT("to_vertex_guid"), ToVertex))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_metasound_link_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.MetaLinks.Emplace(
							FromNode,
							FromVertex,
							ToNode,
							ToVertex,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					const FUeAgentCanonicalValue* const From = Value.IsValid()
						? FindField(*Value, TEXT("from")) : nullptr;
					const FUeAgentCanonicalValue* const To = Value.IsValid()
						? FindField(*Value, TEXT("to")) : nullptr;
					FGuid FromNode;
					FGuid FromVertex;
					FGuid ToNode;
					FGuid ToVertex;
					if (From == nullptr || To == nullptr
						|| !TryGuidField(*From, TEXT("node_guid"), FromNode)
						|| !TryGuidField(*From, TEXT("vertex_guid"), FromVertex)
						|| !TryGuidField(*To, TEXT("node_guid"), ToNode)
						|| !TryGuidField(*To, TEXT("vertex_guid"), ToVertex))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_metasound_link_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.MetaLinks.Emplace(
						FromNode, FromVertex, ToNode, ToVertex,
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::MetaSoundDefault:
			{
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundDefault;
				const FUeAgentCanonicalValuePtr& Value = Edits[0].GetProposedAfter();
				FGuid NodeId;
				FGuid VertexId;
				FString DataType;
				FMetasoundFrontendLiteral Literal;
				if (!Value.IsValid()
					|| !TryGuidField(*Value, TEXT("node_guid"), NodeId)
					|| !TryGuidField(*Value, TEXT("vertex_guid"), VertexId)
					|| !TryStringField(*Value, TEXT("data_type_ref"), DataType)
					|| !TryMetaSoundLiteral(
						FindField(*Value, TEXT("value")), DataType, Literal))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_metasound_default_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				OutSpec.MetaDefault.Emplace(NodeId, VertexId, MoveTemp(Literal));
				return true;
			}
			case EAudioMediaGraphInputKind::MetaSoundCompileSettle:
				OutSpec.Kind = EAudioMediaReadbackKind::MetaSoundCompileSettle;
				if (!Edits[0].GetProposedAfter().IsValid()
					|| !TryStringField(*Edits[0].GetProposedAfter(),
						TEXT("expected_document_revision"), OutSpec.ExpectedRevision))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_metasound_settle_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueNode:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FString ObjectPath;
						if (!TryIdentitySelector(
								Edit, TEXT("object_path"), ObjectPath))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_sound_node_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.SoundNodes.Emplace(
							MoveTemp(ObjectPath),
							FString(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					FString ObjectPath;
					FString ClassRef;
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					if (!Value.IsValid()
						|| !TryStringField(*Value, TEXT("object_path"), ObjectPath)
						|| !TryStringField(*Value, TEXT("native_class_ref"), ClassRef))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_sound_node_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.SoundNodes.Emplace(
						MoveTemp(ObjectPath), MoveTemp(ClassRef),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueRelation:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueRelations;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FString Parent;
						FString Child;
						if (!TryIdentitySelector(
								Edit, TEXT("parent_object_path"), Parent)
							|| !TryIdentitySelector(
								Edit, TEXT("child_object_path"), Child))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_sound_relation_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.SoundRelations.Emplace(
							MoveTemp(Parent),
							MoveTemp(Child),
							INDEX_NONE,
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					FString Parent;
					FString Child;
					int32 Order = INDEX_NONE;
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					if (!Value.IsValid()
						|| !TryStringField(*Value, TEXT("parent_object_path"), Parent)
						|| !TryStringField(*Value, TEXT("child_object_path"), Child)
						|| !TryIntegerField(*Value, TEXT("order_index"), Order))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_sound_relation_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.SoundRelations.Emplace(
						MoveTemp(Parent), MoveTemp(Child), Order,
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueRoot:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueRoot;
				if (!Edits[0].GetProposedAfter().IsValid()
					|| !TryStringField(*Edits[0].GetProposedAfter(),
						TEXT("root_node_object_path"), OutSpec.SoundRoot))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_sound_root_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				return true;
			case EAudioMediaGraphInputKind::SoundCueCompileSettle:
				OutSpec.Kind = EAudioMediaReadbackKind::SoundCueCompileSettle;
				if (!Edits[0].GetProposedAfter().IsValid()
					|| !TryStringField(*Edits[0].GetProposedAfter(),
						TEXT("expected_graph_revision"), OutSpec.ExpectedRevision))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_sound_settle_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphNode:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphNodes;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FString ElementId;
						if (!TryIdentitySelector(
								Edit, TEXT("element_id"), ElementId))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_texture_node_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.TextureNodes.Emplace(
							MoveTemp(ElementId),
							FString(),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString ElementId;
					FString ClassRef;
					if (!Value.IsValid()
						|| !TryStringField(*Value, TEXT("element_id"), ElementId)
						|| !TryStringField(*Value, TEXT("expression_class_ref"), ClassRef))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_texture_node_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.TextureNodes.Emplace(
						MoveTemp(ElementId), MoveTemp(ClassRef),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphLink:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphLinks;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					if (Edit.GetIntent() == EChangeIntent::Delete)
					{
						FString FromElement;
						FString FromPin;
						FString ToElement;
						FString ToPin;
						if (!TryIdentitySelector(
								Edit, TEXT("from_element_id"), FromElement)
							|| !TryIdentitySelector(
								Edit, TEXT("from_pin_name"), FromPin)
							|| !TryIdentitySelector(
								Edit, TEXT("to_element_id"), ToElement)
							|| !TryIdentitySelector(
								Edit, TEXT("to_pin_name"), ToPin))
						{
							SetError(OutError,
								TEXT("graph_compound_audio_media_texture_link_delete_selector_invalid"),
								Edit.GetIdentity().GetSemanticId());
							return false;
						}
						OutSpec.TextureLinks.Emplace(
							FAudioMediaGraphTextureGraphEndpoint(
								MoveTemp(FromElement), MoveTemp(FromPin)),
							FAudioMediaGraphTextureGraphEndpoint(
								MoveTemp(ToElement), MoveTemp(ToPin)),
							EAudioMediaGraphMutationIntent::Delete);
						continue;
					}
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					const FUeAgentCanonicalValue* const From = Value.IsValid()
						? FindField(*Value, TEXT("from")) : nullptr;
					const FUeAgentCanonicalValue* const To = Value.IsValid()
						? FindField(*Value, TEXT("to")) : nullptr;
					FString FromElement;
					FString FromPin;
					FString ToElement;
					FString ToPin;
					if (From == nullptr || To == nullptr
						|| !TryStringField(*From, TEXT("element_id"), FromElement)
						|| !TryStringField(*From, TEXT("pin_name"), FromPin)
						|| !TryStringField(*To, TEXT("element_id"), ToElement)
						|| !TryStringField(*To, TEXT("pin_name"), ToPin))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_texture_link_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.TextureLinks.Emplace(
						FAudioMediaGraphTextureGraphEndpoint(
							MoveTemp(FromElement), MoveTemp(FromPin)),
						FAudioMediaGraphTextureGraphEndpoint(
							MoveTemp(ToElement), MoveTemp(ToPin)),
						NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphParameter:
				OutSpec.Kind = EAudioMediaReadbackKind::TextureGraphParameters;
				for (const FGraphCompoundCanonicalEdit& Edit : Edits)
				{
					const FUeAgentCanonicalValuePtr& Value = Edit.GetProposedAfter();
					FString ElementId;
					FString PinName;
					FString Alias;
					FAudioMediaGraphTextureGraphValue NativeValue;
					if (!Value.IsValid()
						|| !TryStringField(*Value, TEXT("element_id"), ElementId)
						|| !TryStringField(*Value, TEXT("pin_name"), PinName)
						|| !TryStringField(*Value, TEXT("alias"), Alias)
						|| !TryTextureGraphValue(
							FindField(*Value, TEXT("value")), NativeValue))
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_texture_parameter_spec_invalid"),
							Edit.GetIdentity().GetSemanticId());
						return false;
					}
					OutSpec.TextureParameters.Emplace(
						MoveTemp(ElementId), MoveTemp(PinName), MoveTemp(Alias),
						MoveTemp(NativeValue), NativeIntent(Edit.GetIntent()));
				}
				return true;
			case EAudioMediaGraphInputKind::TextureGraphValidateEvaluateSettle:
				OutSpec.Kind =
					EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle;
				if (!Edits[0].GetProposedAfter().IsValid()
					|| !TryStringField(*Edits[0].GetProposedAfter(),
						TEXT("expected_graph_revision"), OutSpec.ExpectedRevision))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_texture_settle_spec_invalid"),
						Request.GetTargetRef());
					return false;
				}
				return true;
			default:
				SetError(OutError,
					TEXT("graph_compound_audio_media_production_binding_unimplemented"),
					Profile.GetCapabilityId());
				return false;
			}
		}

		bool TryCreateAbsentSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			FString Filename;
			FString HeaderErrorCode;
			if (!TryResolvePackageHeaderFilename(
					Request.GetTargetRef(),
					Filename,
					HeaderErrorCode))
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			if (FindPackage(nullptr, *TargetPackageName(Request.GetTargetRef()))
				|| IFileManager::Get().FileExists(*Filename))
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_already_exists"),
					Request.GetTargetRef());
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:absent:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted,
					FUeAgentStateRevision(
						TEXT("graph-compound:audio-media:create"),
						TEXT("absent:") + Request.GetTargetRef(), 0),
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("file-absence:") + Filename),
					{}, Resources, OutSnapshot, Error)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, Error.Code,
					Request.GetTargetRef(), Error.Detail);
				return false;
			}
			return true;
		}

		bool TryCaptureMetaSoundSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UMetaSoundSource* const Source = LoadObject<UMetaSoundSource>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			const IMetaSoundDocumentInterface* const Interface =
				Cast<IMetaSoundDocumentInterface>(Source);
			const FString Revision = ReadPackageAssetRevision(Request.GetTargetRef());
			if (Source == nullptr || Interface == nullptr || Revision.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_invalid"),
					Request.GetTargetRef());
				return false;
			}

			const FMetasoundFrontendDocument& Document =
				Interface->GetConstDocument();
			const FMetasoundFrontendGraph& Graph =
				Document.RootGraph.GetConstDefaultGraph();
			TMap<FString, TMap<FString, FString>> Identities;
			for (const FMetasoundFrontendClassInput& Input
				: Document.RootGraph.GetDefaultInterface().Inputs)
			{
				const FString Name = Input.Name.ToString();
				Identities.Add(
					FString::Printf(
						TEXT("input:metasound_interface:input:%s"), *Name),
					AudioMediaIdentity::MetaSoundInterface(
						TEXT("input"), Name));
			}
			for (const FMetasoundFrontendClassOutput& Output
				: Document.RootGraph.GetDefaultInterface().Outputs)
			{
				const FString Name = Output.Name.ToString();
				Identities.Add(
					FString::Printf(
						TEXT("input:metasound_interface:output:%s"), *Name),
					AudioMediaIdentity::MetaSoundInterface(
						TEXT("output"), Name));
			}
			for (const FMetasoundFrontendNode& Node : Graph.Nodes)
			{
				const FString NodeId = Node.GetID().ToString(
					EGuidFormats::DigitsWithHyphensLower);
				Identities.Add(
					TEXT("item:metasound_node:") + NodeId,
					AudioMediaIdentity::MetaSoundNode(NodeId));
				for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
				{
					const FString VertexId = Input.VertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					Identities.Add(
						FString::Printf(
							TEXT("input:metasound_default:%s:%s:%s"),
							*NodeId, *VertexId, *Input.TypeName.ToString()),
						AudioMediaIdentity::MetaSoundDefault(
							NodeId, VertexId));
				}
			}
			for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
			{
				const FMetasoundFrontendNode* const FromNode =
					Graph.Nodes.FindByPredicate([&Edge](const FMetasoundFrontendNode& Node)
					{
						return Node.GetID() == Edge.FromNodeID;
					});
				const FMetasoundFrontendVertex* const FromVertex = FromNode
					? FromNode->Interface.Outputs.FindByPredicate(
						[&Edge](const FMetasoundFrontendVertex& Vertex)
						{
							return Vertex.VertexID == Edge.FromVertexID;
						}) : nullptr;
				if (FromVertex != nullptr)
				{
					const FString FromNodeId = Edge.FromNodeID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString FromVertexId = Edge.FromVertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString ToNodeId = Edge.ToNodeID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					const FString ToVertexId = Edge.ToVertexID.ToString(
						EGuidFormats::DigitsWithHyphensLower);
					Identities.Add(
						FString::Printf(
							TEXT("relation:metasound_link:%s:%s->%s:%s:%s"),
							*FromNodeId, *FromVertexId, *ToNodeId,
							*ToVertexId, *FromVertex->TypeName.ToString()),
						AudioMediaIdentity::MetaSoundLink(
							FromNodeId, FromVertexId, ToNodeId, ToVertexId));
				}
			}
			Identities.Add(TEXT("dependency:metasound_compile_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:metasound"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native MetaSound readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("MetaSound persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:metasound:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			return true;
		}

		FString TextureElementId(const UTG_Node& Node)
		{
			const UTG_Expression* const Expression = Node.GetExpression();
			if (Cast<UTG_Expression_Output>(Expression) != nullptr)
			{
				return TEXT("builtin_output");
			}
			return Expression ? Expression->GetName() : FString();
		}

		UTG_Node* FindTextureNode(UTG_Graph& Graph, const FString& ElementId)
		{
			UTG_Node* Result = nullptr;
			Graph.ForEachNodes(
				[&Result, &ElementId](const UTG_Node* Node, const uint32)
				{
					if (Result == nullptr && Node != nullptr
						&& TextureElementId(*Node) == ElementId)
					{
						Result = const_cast<UTG_Node*>(Node);
					}
				});
			return Result;
		}

		bool TryCaptureTextureGraphSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			UTextureGraph* const Asset = LoadObject<UTextureGraph>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			UTG_Graph* const Graph = Asset ? Asset->Graph() : nullptr;
			const FString Revision = ReadPackageAssetRevision(Request.GetTargetRef());
			if (Graph == nullptr || Revision.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_invalid"),
					Request.GetTargetRef());
				return false;
			}
			TMap<FString, TMap<FString, FString>> Identities;
			Graph->ForEachNodes(
				[&Identities](const UTG_Node* Node, const uint32)
				{
					if (Node == nullptr || Node->GetExpression() == nullptr)
					{
						return;
					}
					const FString ElementId = TextureElementId(*Node);
					Identities.Add(
						TEXT("item:texture_expression:") + ElementId,
						AudioMediaIdentity::TextureGraphNode(ElementId));
					for (const UTG_Pin* Pin : Node->Pins)
					{
						if (Pin == nullptr)
						{
							continue;
						}
						const FString Direction = Pin->IsInput()
							? TEXT("input") : TEXT("output");
						const FString PinId = FString::Printf(
							TEXT("pin:texture:%s:%s:%s"), *ElementId,
							*Direction, *Pin->GetArgumentName().ToString());
						const FString PinName =
							Pin->GetArgumentName().ToString();
						Identities.Add(
							PinId,
							AudioMediaIdentity::TextureGraphPin(
								ElementId, Direction, PinName));
						if (Pin->IsInput())
						{
							Identities.Add(
								FString::Printf(
									TEXT("input:texture_parameter:%s:%s"),
									*ElementId, *PinName),
								AudioMediaIdentity::TextureGraphParameter(
									ElementId, PinName));
						}
					}
				});
			Graph->ForEachPins(
				[Graph, &Identities](const UTG_Pin* Pin, const uint32, const uint32)
				{
					if (Pin == nullptr || !Pin->IsOutput() || Pin->GetNodePtr() == nullptr)
					{
						return;
					}
					const FString FromId = FString::Printf(
						TEXT("pin:texture:%s:output:%s"),
						*TextureElementId(*Pin->GetNodePtr()),
						*Pin->GetArgumentName().ToString());
					for (const FTG_Id& EdgeId : Pin->GetEdges())
					{
						const UTG_Pin* const ToPin = Graph->GetPin(EdgeId);
						if (ToPin == nullptr || ToPin->GetNodePtr() == nullptr)
						{
							continue;
						}
						const FString ToId = FString::Printf(
							TEXT("pin:texture:%s:input:%s"),
							*TextureElementId(*ToPin->GetNodePtr()),
							*ToPin->GetArgumentName().ToString());
						const FString FromElementId =
							TextureElementId(*Pin->GetNodePtr());
						const FString FromPinName =
							Pin->GetArgumentName().ToString();
						const FString ToElementId =
							TextureElementId(*ToPin->GetNodePtr());
						const FString ToPinName =
							ToPin->GetArgumentName().ToString();
						Identities.Add(
							TEXT("relation:texture_link:") + FromId + TEXT("->") + ToId,
							AudioMediaIdentity::TextureGraphLink(
								FromElementId,
								FromPinName,
								ToElementId,
								ToPinName));
					}
				});
			Identities.Add(TEXT("dependency:texture_graph_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:texture-graph"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native TextureGraph readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("TextureGraph persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:texture-graph:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			return true;
		}

		bool TryCaptureSoundCueSnapshot(
			const FGraphCompoundAudioMediaTargetRequest& Request,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			OutSnapshot.Reset();
			USoundCue* const Cue = LoadObject<USoundCue>(
				nullptr, *TargetObjectPath(Request.GetTargetRef()));
			const FString Revision = ReadPackageAssetRevision(Request.GetTargetRef());
			if (Cue == nullptr || Revision.IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_target_invalid"),
					Request.GetTargetRef());
				return false;
			}

			TMap<FString, TMap<FString, FString>> Identities;
			for (USoundNode* Node : Cue->AllNodes)
			{
				if (Node == nullptr)
				{
					continue;
				}
				const FString ParentPath = Node->GetPathName();
				Identities.Add(
					TEXT("item:sound_node:") + ParentPath,
					AudioMediaIdentity::SoundCueNode(ParentPath));
				for (USoundNode* Child : Node->ChildNodes)
				{
					if (Child != nullptr)
					{
						const FString ChildPath = Child->GetPathName();
						Identities.Add(
							FString::Printf(
								TEXT("relation:sound_child:%s->%s"),
								*ParentPath, *ChildPath),
							AudioMediaIdentity::SoundCueRelation(
								ParentPath, ChildPath));
					}
				}
			}
			Identities.Add(TEXT("binding:sound_root"), {});
			Identities.Add(TEXT("dependency:sound_compile_settle"), {});

			FUeAgentCanonicalValuePtr Present;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("present"), Resources, Present, StateError))
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			const FUeAgentStateRevision StateRevision(
				TEXT("graph-compound:audio-media:sound-cue"), Revision, 0);
			TArray<FUeAgentStateEntry> Entries;
			for (const TPair<FString, TMap<FString, FString>>& Identity
				: Identities)
			{
				Entries.Emplace(
					AudioMediaIdentity::MakeIdentity(
						Identity.Key,
						Request.GetTargetKind(),
						Request.GetTargetRef(),
						TEXT("graph_compound"),
						Revision,
						Identity.Value),
					FUeAgentCanonicalType(
						TEXT("uai.graph_compound.audio_media.present.v1"),
						EUeAgentValueKind::String, false, TEXT("scalar")),
					Present.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::DomainNative,
						Request.GetTargetRef(), TEXT("native sound cue readback"),
						Present.ToSharedRef()),
					FUeAgentStateDetails(
						Identity.Key, TEXT("SoundCue persisted state"),
						{TEXT("GraphCompound")}, true, TEXT("available"), {}),
					StateRevision,
					EUeAgentAuthoredPresence::Present,
					EUeAgentOverrideState::Overridden);
			}
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:audio-media:sound-cue:")
						+ Request.GetTargetRef(),
					EUeAgentStatePlane::Persisted, StateRevision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"), FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"), Request.GetTargetRef(),
						TEXT("package-revision:") + Revision),
					MoveTemp(Entries), Resources, OutSnapshot, StateError)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, StateError.Code,
					Request.GetTargetRef(), StateError.Detail);
				return false;
			}
			return true;
		}

		UObject* TryCreateAsset(
			const FAudioMediaReadbackSpec& Spec,
			const FString& TargetRef,
			UPackage& Package,
			FGraphCompoundRuntimeError& OutError)
		{
			const FName Name(*TargetAssetName(TargetRef));
			const EObjectFlags Flags =
				RF_Public | RF_Standalone | RF_Transactional;
			if (Spec.AssetKind == TEXT("metasound_source"))
			{
				UMetaSoundSourceFactory* const Factory =
					NewObject<UMetaSoundSourceFactory>();
				UObject* const Created = Factory
					? Factory->FactoryCreateNew(
						UMetaSoundSource::StaticClass(), &Package, Name,
						Flags, nullptr, GWarn)
					: nullptr;
				if (Created == nullptr)
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_factory_failed"),
						TargetRef, TEXT("metasound_source"));
				}
				return Created;
			}
			if (Spec.AssetKind == TEXT("sound_cue"))
			{
				USoundCue* const Cue = NewObject<USoundCue>(
					&Package, USoundCue::StaticClass(), Name, Flags);
				if (Cue != nullptr)
				{
					Cue->CreateGraph();
				}
				return Cue;
			}
			if (Spec.AssetKind == TEXT("texture_graph"))
			{
				UTextureGraph* const Graph = NewObject<UTextureGraph>(
					&Package, UTextureGraph::StaticClass(), Name, Flags);
				if (Graph != nullptr)
				{
					Graph->Construct(Name.ToString());
				}
				return Graph;
			}
			SetError(OutError,
				TEXT("graph_compound_audio_media_asset_kind_invalid"),
				TargetRef, Spec.AssetKind);
			return nullptr;
		}

		bool TryApplyMetaSound(
			UMetaSoundSource& Source,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphMetaSoundNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError)
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::MetaSoundInterface:
				bApplied = Hook.TryApplyInterfaceMutations(
					Source, Spec.MetaInterfaces, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Source, Spec.MetaNodes, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundLinks:
				bApplied = Hook.TryApplyLinkMutations(
					Source, Spec.MetaLinks, NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundDefault:
				bApplied = Spec.MetaDefault.IsSet()
					&& Hook.TryApplyDefaultMutation(
						Source, Spec.MetaDefault.GetValue(), NativeError);
				break;
			case EAudioMediaReadbackKind::MetaSoundCompileSettle:
				bApplied = Hook.TryRegisterCompileSettle(Source, NativeError);
				break;
			default:
				break;
			}
			if (!bApplied)
			{
				SetError(OutError,
					NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_audio_media_metasound_native_failed")
						: NativeError.GetCode(),
					Source.GetPathName(), NativeError.GetMessage());
				return false;
			}
			return true;
		}

		bool TryApplySoundCue(
			USoundCue& Cue,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphSoundCueNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError)
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::SoundCueNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Cue, Spec.SoundNodes, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueRelations:
				bApplied = Hook.TryApplyRelationMutations(
					Cue, Spec.SoundRelations, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueRoot:
				bApplied = Hook.TrySetRoot(Cue, Spec.SoundRoot, NativeError);
				break;
			case EAudioMediaReadbackKind::SoundCueCompileSettle:
				bApplied = Hook.TrySynchronizeAndCompile(Cue, NativeError);
				break;
			default:
				break;
			}
			if (!bApplied)
			{
				SetError(OutError,
					NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_audio_media_sound_cue_native_failed")
						: NativeError.GetCode(),
					Cue.GetPathName(), NativeError.GetMessage());
				return false;
			}
			return true;
		}

		bool TryApplyTextureGraph(
			UTextureGraph& Graph,
			const FAudioMediaReadbackSpec& Spec,
			const FAudioMediaGraphTextureGraphNativeHook& Hook,
			FGraphCompoundRuntimeError& OutError)
		{
			FAudioMediaGraphError NativeError;
			bool bApplied = false;
			switch (Spec.Kind)
			{
			case EAudioMediaReadbackKind::TextureGraphNodes:
				bApplied = Hook.TryApplyNodeMutations(
					Graph, Spec.TextureNodes, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphLinks:
				bApplied = Hook.TryApplyLinkMutations(
					Graph, Spec.TextureLinks, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphParameters:
				bApplied = Hook.TryApplyParameterMutations(
					Graph, Spec.TextureParameters, NativeError);
				break;
			case EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle:
				bApplied = Hook.TryValidateEvaluateSettle(Graph, NativeError);
				break;
			default:
				break;
			}
			if (!bApplied)
			{
				SetError(OutError,
					NativeError.GetCode().IsEmpty()
						? TEXT("graph_compound_audio_media_texture_graph_native_failed")
						: NativeError.GetCode(),
					Graph.GetPathName(), NativeError.GetMessage());
				return false;
			}
			return true;
		}

		bool VerifyMetaSoundReadback(
			UMetaSoundSource& Source,
			const FAudioMediaReadbackSpec& Spec,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const IMetaSoundDocumentInterface* const Interface =
				Cast<IMetaSoundDocumentInterface>(&Source);
			if (Interface == nullptr)
			{
				OutDetail = TEXT("MetaSound document interface missing");
				return false;
			}
			const FMetasoundFrontendDocument& Document =
				Interface->GetConstDocument();
			const FMetasoundFrontendGraph& Graph =
				Document.RootGraph.GetConstDefaultGraph();
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundInterface)
			{
				for (const FAudioMediaGraphMetaSoundInterfaceMutation& Mutation
					: Spec.MetaInterfaces)
				{
					const bool bInput = Mutation.GetInterfaceKind() == TEXT("input");
					const FMetasoundFrontendClassVertex* Vertex = bInput
						? static_cast<const FMetasoundFrontendClassVertex*>(
							Document.RootGraph.GetDefaultInterface().Inputs.FindByPredicate(
								[&Mutation](const FMetasoundFrontendClassInput& Item)
								{
									return Item.Name == Mutation.GetName();
								}))
						: static_cast<const FMetasoundFrontendClassVertex*>(
							Document.RootGraph.GetDefaultInterface().Outputs.FindByPredicate(
								[&Mutation](const FMetasoundFrontendClassOutput& Item)
								{
									return Item.Name == Mutation.GetName();
								}));
					const bool bShouldExist = Mutation.GetIntent()
						!= EAudioMediaGraphMutationIntent::Delete;
					if (bShouldExist != (Vertex != nullptr)
						|| (Vertex != nullptr
							&& Vertex->TypeName != Mutation.GetDataType()))
					{
						OutDetail = TEXT("MetaSound interface native readback mismatch");
						return false;
					}
					if (bInput && Vertex != nullptr)
					{
						const FMetasoundFrontendClassInput* const Input =
							static_cast<const FMetasoundFrontendClassInput*>(Vertex);
						const FMetasoundFrontendLiteral* const Default =
							Input->FindConstDefault(
								Metasound::Frontend::DefaultPageID);
						if (Default == nullptr
							|| *Default != Mutation.GetDefaultValue())
						{
							OutDetail = TEXT("MetaSound interface default readback mismatch");
							return false;
						}
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundNodes)
			{
				for (const FAudioMediaGraphMetaSoundNodeMutation& Mutation
					: Spec.MetaNodes)
				{
					const FMetasoundFrontendNode* const Node =
						Graph.Nodes.FindByPredicate(
							[&Mutation](const FMetasoundFrontendNode& Item)
							{
								return Item.GetID() == Mutation.GetNodeId();
							});
					if (Mutation.GetIntent()
						== EAudioMediaGraphMutationIntent::Delete)
					{
						if (Node != nullptr)
						{
							OutDetail = TEXT("deleted MetaSound node still exists");
							return false;
						}
						continue;
					}
					FMetasoundFrontendClassName ExpectedClassName;
					const FMetasoundFrontendClass* const NativeClass = Node
						? Document.Dependencies.FindByPredicate(
							[Node](const FMetasoundFrontendClass& Item)
							{
								return Item.ID == Node->ClassID;
							}) : nullptr;
					if (Node == nullptr || NativeClass == nullptr
						|| !FMetasoundFrontendClassName::Parse(
							Mutation.GetClassName(), ExpectedClassName)
						|| NativeClass->Metadata.GetClassName() != ExpectedClassName
						|| NativeClass->Metadata.GetVersion().Major
							!= Mutation.GetMajorVersion()
						|| NativeClass->Metadata.GetVersion().Minor
							!= Mutation.GetMinorVersion())
					{
						OutDetail = TEXT("MetaSound node native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundLinks)
			{
				for (const FAudioMediaGraphMetaSoundLinkMutation& Mutation
					: Spec.MetaLinks)
				{
					const bool bPresent = Graph.Edges.ContainsByPredicate(
						[&Mutation](const FMetasoundFrontendEdge& Edge)
						{
							return Edge.FromNodeID == Mutation.GetFromNodeId()
								&& Edge.FromVertexID == Mutation.GetFromVertexId()
								&& Edge.ToNodeID == Mutation.GetToNodeId()
								&& Edge.ToVertexID == Mutation.GetToVertexId();
						});
					if ((Mutation.GetIntent()
						== EAudioMediaGraphMutationIntent::Delete) ? bPresent : !bPresent)
					{
						OutDetail = TEXT("MetaSound link native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundDefault)
			{
				if (!Spec.MetaDefault.IsSet())
				{
					OutDetail = TEXT("MetaSound default spec missing");
					return false;
				}
				UMetaSoundBuilderBase& Builder =
					Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
						.FindOrBeginBuilding(Source, true);
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
				const FMetasoundFrontendLiteral Actual = Builder.GetNodeInputDefault(
					FMetaSoundBuilderNodeInputHandle(
						Spec.MetaDefault->GetNodeId(),
						Spec.MetaDefault->GetVertexId()), Result);
				const FMetasoundFrontendClassName ClassName = Builder.GetBuilder()
					.GetConstDocumentChecked().RootGraph.Metadata.GetClassName();
				const FTopLevelAssetPath AssetPath = Builder.GetBuilder().GetHintPath();
				const bool bFinished =
					Metasound::Engine::FDocumentBuilderRegistry::GetChecked()
						.FinishBuilding(ClassName, AssetPath);
				if (Result != EMetaSoundBuilderResult::Succeeded || !bFinished
					|| Actual != Spec.MetaDefault->GetValue())
				{
					OutDetail = TEXT("MetaSound default native readback mismatch");
					return false;
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::MetaSoundCompileSettle)
			{
				return Document.RootGraph.Metadata.GetClassName()
					!= FMetasoundFrontendClassName();
			}
			OutDetail = TEXT("MetaSound readback kind invalid");
			return false;
		}

		bool VerifySoundCueReadback(
			USoundCue& Cue,
			const FAudioMediaReadbackSpec& Spec,
			FString& OutDetail)
		{
			OutDetail.Reset();
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueNodes)
			{
				for (const FAudioMediaGraphSoundCueNodeMutation& Mutation
					: Spec.SoundNodes)
				{
					const FString Prefix = Cue.GetPathName() + TEXT(":");
					const FString Name = Mutation.GetObjectPath().StartsWith(Prefix)
						? Mutation.GetObjectPath().RightChop(Prefix.Len()) : FString();
					USoundNode* const Node = Name.IsEmpty()
						? nullptr : FindObject<USoundNode>(&Cue, *Name);
					if (Mutation.GetIntent()
						== EAudioMediaGraphMutationIntent::Delete)
					{
						if (Node != nullptr)
						{
							OutDetail = TEXT("deleted sound node still exists");
							return false;
						}
						continue;
					}
					UClass* const ExpectedClass = LoadObject<UClass>(
						nullptr, *Mutation.GetNativeClassRef());
					if (Node == nullptr || ExpectedClass == nullptr
						|| !Node->IsA(ExpectedClass)
						|| !Cue.AllNodes.Contains(Node))
					{
						OutDetail = TEXT("sound node native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueRelations)
			{
				for (const FAudioMediaGraphSoundCueRelationMutation& Mutation
					: Spec.SoundRelations)
				{
					USoundNode* Parent = nullptr;
					for (USoundNode* Node : Cue.AllNodes)
					{
						if (Node != nullptr && Node->GetPathName()
							== Mutation.GetParentObjectPath())
						{
							Parent = Node;
							break;
						}
					}
					const bool bExact = Parent != nullptr
						&& Parent->ChildNodes.IsValidIndex(Mutation.GetOrder())
						&& Parent->ChildNodes[Mutation.GetOrder()] != nullptr
						&& Parent->ChildNodes[Mutation.GetOrder()]->GetPathName()
							== Mutation.GetChildObjectPath();
					const bool bPresent = Parent != nullptr
						&& Parent->ChildNodes.ContainsByPredicate(
							[&Mutation](const USoundNode* Child)
							{
								return Child != nullptr
									&& Child->GetPathName()
										== Mutation.GetChildObjectPath();
							});
					if (Mutation.GetIntent()
							== EAudioMediaGraphMutationIntent::Delete
						? bPresent
						: !bExact)
					{
						OutDetail = TEXT("sound relation native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueRoot)
			{
				if (Cue.FirstNode == nullptr
					|| Cue.FirstNode->GetPathName() != Spec.SoundRoot)
				{
					OutDetail = TEXT("sound root native readback mismatch");
					return false;
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::SoundCueCompileSettle)
			{
				if (Cue.GetGraph() == nullptr)
				{
					OutDetail = TEXT("sound cue graph missing after compile");
					return false;
				}
				return true;
			}
			OutDetail = TEXT("sound cue readback kind invalid");
			return false;
		}

		bool VerifyTextureGraphReadback(
			UTextureGraph& Asset,
			const FAudioMediaReadbackSpec& Spec,
			FString& OutDetail)
		{
			OutDetail.Reset();
			UTG_Graph* const Graph = Asset.Graph();
			if (Graph == nullptr)
			{
				OutDetail = TEXT("TextureGraph native graph missing");
				return false;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphNodes)
			{
				for (const FAudioMediaGraphTextureGraphNodeMutation& Mutation
					: Spec.TextureNodes)
				{
					UTG_Node* const Node = FindTextureNode(*Graph, Mutation.GetElementId());
					if (Mutation.GetIntent() == EAudioMediaGraphMutationIntent::Delete)
					{
						if (Node != nullptr)
						{
							OutDetail = TEXT("deleted TextureGraph expression still exists");
							return false;
						}
						continue;
					}
					UClass* const ExpectedClass = LoadObject<UClass>(
						nullptr, *Mutation.GetExpressionClassRef());
					if (Node == nullptr || Node->GetExpression() == nullptr
						|| ExpectedClass == nullptr
						|| Node->GetExpression()->GetClass() != ExpectedClass)
					{
						OutDetail = TEXT("TextureGraph expression native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphLinks)
			{
				for (const FAudioMediaGraphTextureGraphLinkMutation& Mutation
					: Spec.TextureLinks)
				{
					UTG_Node* const From = FindTextureNode(
						*Graph, Mutation.GetFrom().GetElementId());
					UTG_Node* const To = FindTextureNode(
						*Graph, Mutation.GetTo().GetElementId());
					FTG_Name FromName(*Mutation.GetFrom().GetPinName());
					FTG_Name ToName(*Mutation.GetTo().GetPinName());
					const UTG_Pin* const FromPin = From
						? From->GetOutputPin(FromName) : nullptr;
					const UTG_Pin* const ToPin = To
						? To->GetInputPin(ToName) : nullptr;
					const bool bPresent = FromPin != nullptr && ToPin != nullptr
						&& FromPin->GetEdges().Contains(ToPin->GetId());
					if ((Mutation.GetIntent()
						== EAudioMediaGraphMutationIntent::Delete) ? bPresent : !bPresent)
					{
						OutDetail = TEXT("TextureGraph edge native readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::TextureGraphParameters)
			{
				for (const FAudioMediaGraphTextureGraphParameterMutation& Mutation
					: Spec.TextureParameters)
				{
					UTG_Node* const Node = FindTextureNode(*Graph, Mutation.GetElementId());
					FTG_Name PinName(*Mutation.GetPinName());
					const UTG_Pin* const Pin = Node ? Node->GetInputPin(PinName) : nullptr;
					if (Pin == nullptr || Pin->GetAliasName() != FName(*Mutation.GetAlias()))
					{
						OutDetail = TEXT("TextureGraph parameter alias readback mismatch");
						return false;
					}
					bool bValueMatches = false;
					switch (Mutation.GetValue().GetKind())
					{
					case EAudioMediaGraphTextureGraphValueKind::Number:
					{
						float Actual = 0.0f;
						bValueMatches = Pin->GetValue(Actual)
							&& FMath::IsNearlyEqual(
								Actual, static_cast<float>(Mutation.GetValue().GetNumber()));
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::Boolean:
					{
						bool Actual = false;
						bValueMatches = Pin->GetValue(Actual)
							&& Actual == Mutation.GetValue().GetBoolean();
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::String:
					{
						FString Actual;
						bValueMatches = Pin->GetValue(Actual)
							&& Actual == Mutation.GetValue().GetString();
						break;
					}
					case EAudioMediaGraphTextureGraphValueKind::NumberArray:
					{
						const TArray<double>& Expected = Mutation.GetValue().GetNumberArray();
						FVector4f Actual;
						bValueMatches = Expected.Num() == 4 && Pin->GetValue(Actual)
							&& FMath::IsNearlyEqual(Actual.X, static_cast<float>(Expected[0]))
							&& FMath::IsNearlyEqual(Actual.Y, static_cast<float>(Expected[1]))
							&& FMath::IsNearlyEqual(Actual.Z, static_cast<float>(Expected[2]))
							&& FMath::IsNearlyEqual(Actual.W, static_cast<float>(Expected[3]));
						break;
					}
					}
					if (!bValueMatches)
					{
						OutDetail = TEXT("TextureGraph parameter value readback mismatch");
						return false;
					}
				}
				return true;
			}
			if (Spec.Kind
				== EAudioMediaReadbackKind::TextureGraphValidateEvaluateSettle)
			{
				return FindTextureNode(*Graph, TEXT("builtin_output")) != nullptr;
			}
			OutDetail = TEXT("TextureGraph readback kind invalid");
			return false;
		}

		bool TryVerifyReadback(
			const FString& TargetRef,
			const FAudioMediaReadbackSpec& Spec,
			FString& OutDetail)
		{
			OutDetail.Reset();
			UObject* const Target = LoadObject<UObject>(
				nullptr, *TargetObjectPath(TargetRef));
			if (Target == nullptr)
			{
				OutDetail = TEXT("persisted asset failed to reload");
				return false;
			}
			if (Spec.Kind == EAudioMediaReadbackKind::AssetCreate)
			{
				UClass* const ExpectedClass = LoadObject<UClass>(
					nullptr, *Spec.NativeClassRef);
				if (ExpectedClass == nullptr || !Target->IsA(ExpectedClass))
				{
					OutDetail = TEXT("persisted asset class mismatch");
					return false;
				}
				if (Spec.AssetKind == TEXT("metasound_source")
					&& Cast<IMetaSoundDocumentInterface>(Target) == nullptr)
				{
					OutDetail = TEXT("persisted MetaSound document missing");
					return false;
				}
				if (Spec.AssetKind == TEXT("texture_graph")
					&& (!Cast<UTextureGraph>(Target)
						|| Cast<UTextureGraph>(Target)->Graph() == nullptr))
				{
					OutDetail = TEXT("persisted TextureGraph graph missing");
					return false;
				}
				return true;
			}
			if (UMetaSoundSource* const Source = Cast<UMetaSoundSource>(Target))
			{
				return VerifyMetaSoundReadback(*Source, Spec, OutDetail);
			}
			if (UTextureGraph* const TextureGraph = Cast<UTextureGraph>(Target))
			{
				return VerifyTextureGraphReadback(*TextureGraph, Spec, OutDetail);
			}
			USoundCue* const Cue = Cast<USoundCue>(Target);
			return Cue != nullptr && VerifySoundCueReadback(*Cue, Spec, OutDetail);
		}

		class FAudioMediaReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FAudioMediaReadbackOracle(
				FString InTargetRef,
				FAudioMediaReadbackSpec InSpec)
				: TargetRef(MoveTemp(InTargetRef))
				, Spec(MoveTemp(InSpec))
			{
			}

			const FString& GetOracleId() const override
			{
				return ReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				FString UnloadDetail;
				if (Request.GetOracleId() != ReadbackOracleId
					|| Request.GetSubjectRef() != TargetRef
					|| !TryUnloadTarget(TargetRef, false, UnloadDetail))
				{
					OutError = FVerificationError(
						TEXT("audio_media_readback_unload_failed"),
						TargetRef + TEXT(":") + UnloadDetail);
					return false;
				}
				FString Detail;
				if (!TryVerifyReadback(TargetRef, Spec, Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("audio_media_native_readback_mismatch"),
						TargetRef + TEXT(":") + Detail);
					return true;
				}
				const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("verified"), true);
				Actual->SetStringField(
					TEXT("revision"), ReadPackageAssetRevision(TargetRef));
				FUeAgentCanonicalValuePtr ActualValue;
				FGraphCompoundRuntimeError CanonicalError;
				if (!TryCanonicalObject(
						Actual, Resources, ActualValue, CanonicalError))
				{
					OutError = FVerificationError(
						CanonicalError.Code,
						TargetRef + TEXT(":") + CanonicalError.Detail);
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					ActualValue.ToSharedRef(),
					TEXT("audio-media-readback:") + TargetRef + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FString TargetRef;
			FAudioMediaReadbackSpec Spec;
		};

		bool TrySerializeResult(
			const FString& TargetRef,
			const FString& Revision,
			const FOperationLifecycleReceipt& Receipt,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TArray<FString>& Phases = Receipt.GetPhaseHistory();
			const bool bMutationApplied = Receipt.DidMutationStart()
				&& Phases.Contains(LexToString(
					EOperationLifecycleStage::DomainExecute));
			const bool bCompileSettled = Phases.Contains(LexToString(
				EOperationLifecycleStage::Settle));
			const bool bSaved = Phases.Contains(LexToString(
				EOperationLifecycleStage::Persist));
			const bool bReloaded = Phases.Contains(LexToString(
				EOperationLifecycleStage::IndependentVerify));
			if (Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded
				|| !bMutationApplied
				|| !bCompileSettled
				|| !bSaved
				|| !bReloaded
				|| Receipt.GetPersistenceReceiptId().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("graph_compound_audio_media_lifecycle_receipt_invalid"),
					TargetRef);
				return false;
			}
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Object->SetStringField(TEXT("target_ref"), TargetRef);
			Object->SetStringField(TEXT("revision"), Revision);
			Object->SetBoolField(TEXT("mutation_applied"), bMutationApplied);
			Object->SetBoolField(TEXT("compile_settled"), bCompileSettled);
			Object->SetBoolField(TEXT("saved"), bSaved);
			Object->SetBoolField(TEXT("reloaded"), bReloaded);
			Object->SetStringField(
				TEXT("persistence_receipt_id"),
				Receipt.GetPersistenceReceiptId());
			FString Canonical;
			FString JsonError;
			if (!Json::TryCanonicalize(Object, Canonical, JsonError))
			{
				SetError(OutError,
					TEXT("graph_compound_audio_media_result_invalid"),
					TargetRef, JsonError);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FAudioMediaLifecycleContext final
			: public IGraphCompoundAudioMediaLifecycleContext
		{
		public:
			FAudioMediaLifecycleContext(
				FAudioMediaGraphRouteProfile InProfile,
				FGraphCompoundAudioMediaOperationRequest InRequest,
				FPlanningPreflightPlanRef InPlanningPlan,
				FUeAgentCallerContext InCallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> InResources,
				TSharedRef<IUeAgentCancellationSignal> InCancellation,
				FAudioMediaReadbackSpec InSpec,
				FString InJournalRoot)
				: Profile(MoveTemp(InProfile))
				, Request(MoveTemp(InRequest))
				, PlanningPlan(MoveTemp(InPlanningPlan))
				, CallerContext(MoveTemp(InCallerContext))
				, Resources(MoveTemp(InResources))
				, Cancellation(MoveTemp(InCancellation))
				, Spec(MoveTemp(InSpec))
				, JournalRoot(MoveTemp(InJournalRoot))
			{
			}

			bool IsCreate() const
			{
				return Profile.GetInputKind()
					== EAudioMediaGraphInputKind::AssetCreate;
			}

			bool IsMetaSound() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::MetaSoundCompileSettle;
			}

			bool IsSoundCue() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::SoundCueNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::SoundCueCompileSettle;
			}

			bool IsTextureGraph() const
			{
				return Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::TextureGraphNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::
							TextureGraphValidateEvaluateSettle;
			}

			bool TryCaptureCurrent(
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError)
			{
				OutSnapshot.Reset();
				if (IsCreate())
				{
					return TryCreateAbsentSnapshot(
						Request.GetTarget(), *Resources, OutSnapshot, OutError);
				}
				if (IsMetaSound())
				{
					return TryCaptureMetaSoundSnapshot(
						Request.GetTarget(), *Resources, OutSnapshot, OutError);
				}
				if (IsSoundCue())
				{
					return TryCaptureSoundCueSnapshot(
						Request.GetTarget(), *Resources, OutSnapshot, OutError);
				}
				if (IsTextureGraph())
				{
					return TryCaptureTextureGraphSnapshot(
						Request.GetTarget(), *Resources, OutSnapshot, OutError);
				}
				SetError(OutError,
					TEXT("graph_compound_audio_media_lifecycle_kind_invalid"),
					Profile.GetCapabilityId());
				return false;
			}

			bool TryBuildResult(
				const FOperationLifecycleReceipt& Receipt,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) const override
			{
				if (Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded
					|| Revision.IsEmpty())
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_terminal_result_invalid"),
						Request.GetTargetRef(),
						LexToString(Receipt.GetStatus()));
					return false;
				}
				return TrySerializeResult(
					Request.GetTargetRef(), Revision, Receipt,
					OutResult, OutError);
			}

			FAudioMediaGraphRouteProfile Profile;
			FGraphCompoundAudioMediaOperationRequest Request;
			FPlanningPreflightPlanRef PlanningPlan;
			FUeAgentCallerContext CallerContext;
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources;
			TSharedRef<IUeAgentCancellationSignal> Cancellation;
			FAudioMediaReadbackSpec Spec;
			FString JournalRoot;
			FUeAgentCanonicalSnapshotPtr CurrentSnapshot;
			FPackageAssetDurableBackend DurableBackend;
			FAssetDurableSessionPtr DurableSession;
			FDurableReceipt DurableReceipt;
			int64 FencingToken = 0;
			UPackage* Package = nullptr;
			UObject* Target = nullptr;
			FString Revision;
		};

		class FAudioMediaVersionFence final : public IOperationVersionFencePort
		{
		public:
			explicit FAudioMediaVersionFence(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			bool Check(
				const FOperationLifecyclePlan&,
				const EOperationLifecycleStage Stage,
				bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = false;
				if (Context->Cancellation->IsCancelled())
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_cancelled"));
					return false;
				}
				if (Stage == EOperationLifecycleStage::PrePersistFence)
				{
					bOutCurrent = Context->Target != nullptr
						&& Context->Package != nullptr
						&& Context->DurableSession.IsValid();
					if (!bOutCurrent)
					{
						OutError = FOperationLifecycleError(
							TEXT("graph_compound_audio_media_pre_persist_fence_failed"),
							Context->Request.GetTargetRef());
					}
					return true;
				}

				FGraphCompoundRuntimeError RuntimeError;
				if (!Context->TryCaptureCurrent(
						Context->CurrentSnapshot, RuntimeError)
					|| !Context->CurrentSnapshot.IsValid())
				{
					OutError = FOperationLifecycleError(
						RuntimeError.Code.IsEmpty()
							? TEXT("graph_compound_audio_media_fence_capture_failed")
							: RuntimeError.Code,
						RuntimeError.Detail);
					return false;
				}
				if (!Context->Spec.ExpectedRevision.IsEmpty()
					&& Context->Spec.ExpectedRevision
						!= Context->CurrentSnapshot->GetRevision().GetGeneration())
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_revision_conflict"),
						Context->CurrentSnapshot->GetRevision().GetGeneration());
					return true;
				}
				FPlanningPreflightError RevalidateError;
				bOutCurrent = TryRevalidatePlan(
					Context->PlanningPlan,
					Context->CurrentSnapshot.ToSharedRef(),
					Context->CallerContext,
					*Context->Resources,
					RevalidateError);
				if (!bOutCurrent)
				{
					OutError = FOperationLifecycleError(
						RevalidateError.GetCode(), RevalidateError.GetDetail());
				}
				return true;
			}

		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaMutation final : public IOperationDomainMutationPort
		{
		public:
			explicit FAudioMediaMutation(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}

			bool Execute(
				const FOperationLifecyclePlan&,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				const FString& TargetRef = Context->Request.GetTargetRef();
				const FString PackageName = TargetPackageName(TargetRef);
				FGraphCompoundRuntimeError RuntimeError;
				static int64 NextFencingToken = 10000;
				Context->FencingToken = ++NextFencingToken;
				const FString JournalOwner =
					TEXT("graph-compound-audio-media:")
					+ Context->PlanningPlan->GetRequestId();
				const FAssetDurableRequest DurableRequest(
					Context->PlanningPlan,
					{FAssetDurableTarget(
						Context->PlanningPlan->GetChangeSet().GetChanges()[0]
							.GetIdentity(),
						PackageName,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					Context->FencingToken,
					JournalOwner,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest,
						Context->DurableBackend,
						Context->JournalRoot,
						*Context->Resources,
						Context->DurableSession,
						DurableError)
					|| !Context->DurableSession.IsValid())
				{
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
					return false;
				}

				if (Context->IsCreate())
				{
					Context->Package = CreatePackage(*PackageName);
					Context->Target = Context->Package
						? TryCreateAsset(
							Context->Spec, TargetRef,
							*Context->Package, RuntimeError)
						: nullptr;
					if (Context->Target)
					{
						FAssetRegistryModule::AssetCreated(Context->Target);
					}
				}
				else
				{
					Context->Target = LoadObject<UObject>(
						nullptr, *TargetObjectPath(TargetRef));
					Context->Package = Context->Target
						? Context->Target->GetOutermost() : nullptr;
					bool bApplied = false;
					if (UMetaSoundSource* Source =
						Cast<UMetaSoundSource>(Context->Target))
					{
						bApplied = TryApplyMetaSound(
							*Source, Context->Spec, MetaSoundHook, RuntimeError);
					}
					else if (USoundCue* Cue = Cast<USoundCue>(Context->Target))
					{
						bApplied = TryApplySoundCue(
							*Cue, Context->Spec, SoundCueHook, RuntimeError);
					}
					else if (UTextureGraph* Graph =
						Cast<UTextureGraph>(Context->Target))
					{
						bApplied = TryApplyTextureGraph(
							*Graph, Context->Spec, TextureGraphHook, RuntimeError);
					}
					if (!bApplied)
					{
						Context->Target = nullptr;
						Context->Package = nullptr;
					}
				}

				OutFacts = FOperationMutationFacts(
					TEXT("graph-compound-audio-media:")
						+ Context->PlanningPlan->GetRequestId(),
					{TargetRef},
					true);
				if (!Context->Target || !Context->Package)
				{
					OutError = FOperationLifecycleError(
						RuntimeError.Code.IsEmpty()
							? TEXT("graph_compound_audio_media_native_failed")
							: RuntimeError.Code,
						RuntimeError.Detail);
					return false;
				}
				Context->Package->MarkPackageDirty();
				return OutFacts.IsValid();
			}

			bool Rollback(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				bOutConfirmed = false;
				if (!Context->DurableSession.IsValid())
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_rollback_session_missing"));
					return false;
				}
				Context->Target = nullptr;
				Context->Package = nullptr;
				FString Detail;
				bOutConfirmed = TryRollback(
					Context->Request.GetTargetRef(),
					Context->IsCreate(),
					Context->DurableSession.ToSharedRef(),
					Context->DurableBackend,
					Context->FencingToken,
					*Context->Resources,
					Detail);
				if (!bOutConfirmed)
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_rollback_failed"),
						Detail);
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
			FAudioMediaGraphMetaSoundNativeHook MetaSoundHook;
			FAudioMediaGraphSoundCueNativeHook SoundCueHook;
			FAudioMediaGraphTextureGraphNativeHook TextureGraphHook;
		};

		class FAudioMediaStability final : public IOperationStabilityPort
		{
		public:
			explicit FAudioMediaStability(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Settle(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				FString Detail;
				bool bStable = Context->Target != nullptr;
				if (bStable && !Context->IsCreate())
				{
					if (UMetaSoundSource* Source =
						Cast<UMetaSoundSource>(Context->Target))
					{
						bStable = VerifyMetaSoundReadback(
							*Source, Context->Spec, Detail);
					}
					else if (USoundCue* Cue = Cast<USoundCue>(Context->Target))
					{
						bStable = VerifySoundCueReadback(
							*Cue, Context->Spec, Detail);
					}
					else if (UTextureGraph* Graph =
						Cast<UTextureGraph>(Context->Target))
					{
						bStable = VerifyTextureGraphReadback(
							*Graph, Context->Spec, Detail);
					}
					else
					{
						bStable = false;
					}
				}
				if (!bStable)
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_settle_failed"), Detail);
				}
				return bStable;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaPersistence final : public IOperationPersistencePort
		{
		public:
			explicit FAudioMediaPersistence(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Persist(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FString& OutReceipt,
				FOperationLifecycleError& OutError) override
			{
				FAssetDurableError DurableError;
				const FAssetDurableSessionRef Prepared =
					Context->DurableSession.ToSharedRef();
				if (!TryPersistAssetDurability(
						Prepared,
						Context->DurableBackend,
						Context->FencingToken,
						*Context->Resources,
						Context->DurableReceipt,
						DurableError))
				{
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
					return false;
				}
				Context->Package->SetDirtyFlag(false);
				Context->Target = nullptr;
				Context->Package = nullptr;
				Context->Revision = ReadPackageAssetRevision(
					Context->Request.GetTargetRef());
				Context->DurableSession = PersistedSession(
					Prepared, Context->DurableReceipt);
				OutReceipt = Context->DurableReceipt.GetJournalId();
				if (Context->Revision.IsEmpty() || OutReceipt.IsEmpty())
				{
					OutError = FOperationLifecycleError(
						TEXT("graph_compound_audio_media_persistence_incomplete"));
					return false;
				}
				return true;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaVerification final
			: public IOperationIndependentVerificationPort
		{
		public:
			explicit FAudioMediaVerification(
				TSharedRef<FAudioMediaLifecycleContext> InContext)
				: Context(MoveTemp(InContext))
			{
			}
			bool Verify(
				const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				bOutEquivalent = false;
				const TSharedRef<FJsonObject> ExpectedObject =
					MakeShared<FJsonObject>();
				ExpectedObject->SetBoolField(TEXT("verified"), true);
				ExpectedObject->SetStringField(
					TEXT("revision"), Context->Revision);
				FUeAgentCanonicalValuePtr Expected;
				FGraphCompoundRuntimeError RuntimeError;
				if (!TryCanonicalObject(
						ExpectedObject, *Context->Resources, Expected, RuntimeError)
					|| !Expected.IsValid())
				{
					OutError = FOperationLifecycleError(
						RuntimeError.Code, RuntimeError.Detail);
					return false;
				}
				TSharedPtr<FIndependentReadbackService> Readback;
				FVerificationError VerifyError;
				const TSharedRef<FAudioMediaReadbackOracle, ESPMode::ThreadSafe> Oracle =
					MakeShared<FAudioMediaReadbackOracle, ESPMode::ThreadSafe>(
						Context->Request.GetTargetRef(), Context->Spec);
				FIndependentReadbackOutcome Outcome;
				if (!FIndependentReadbackService::TryCreate(
						{Oracle}, Readback, VerifyError)
					|| !Readback.IsValid()
					|| !Readback->TryVerify(
						FIndependentReadbackRequest(
							TEXT("graph-compound-audio-media:")
								+ Context->PlanningPlan->GetRequestId(),
							ReadbackOracleId,
							Context->Request.GetTargetRef(),
							Expected.ToSharedRef()),
						*Context->Resources,
						Outcome,
						VerifyError)
					|| Outcome.GetStatus() != EVerificationStatus::Verified)
				{
					OutError = FOperationLifecycleError(
						VerifyError.GetCode().IsEmpty()
							? TEXT("graph_compound_audio_media_readback_failed")
							: VerifyError.GetCode(),
						Outcome.GetDiagnosticCode());
					return false;
				}
				FAssetDurableError DurableError;
				if (!TryAcknowledgeAssetDurability(
						Context->DurableSession.ToSharedRef(),
						FDurableVerificationAcknowledgement(
							PersistenceReceiptId,
							Context->PlanningPlan->GetFingerprint(),
							Context->DurableSession->GetInventory()
								.GetInventoryHash(),
							Outcome.GetEvidenceId(),
							TEXT("graph-compound-audio-media:")
								+ Context->PlanningPlan->GetRequestId(),
							Context->FencingToken),
						Context->FencingToken,
						*Context->Resources,
						DurableError))
				{
					OutError = FOperationLifecycleError(
						DurableError.GetCode(), DurableError.GetDetail());
					return false;
				}
				bOutEquivalent = true;
				return true;
			}
		private:
			TSharedRef<FAudioMediaLifecycleContext> Context;
		};

		class FAudioMediaProductionEnvironment final
			: public IGraphCompoundAudioMediaEnvironment
		{
		public:
			explicit FAudioMediaProductionEnvironment(
				const FGraphCompoundAudioMediaProductionConfig& Config)
				: JournalRoot(Config.DurableJournalRoot.IsEmpty()
					? FPaths::ProjectSavedDir()
						/ TEXT("UeAgentInterface/GraphCompoundDurable")
					: Config.DurableJournalRoot)
			{
			}

			bool TryCaptureSnapshot(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaTargetRequest& Request,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentCanonicalSnapshotPtr& OutSnapshot,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutSnapshot.Reset();
				OutError = {};
				if (!IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_audio_media_cancelled")
							: TEXT("graph_compound_audio_media_thread_invalid"),
						Profile.GetCapabilityId());
					return false;
				}
				if (Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate)
				{
					return TryCreateAbsentSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::MetaSoundCompileSettle)
				{
					return TryCaptureMetaSoundSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind() >= EAudioMediaGraphInputKind::SoundCueNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::SoundCueCompileSettle)
				{
					return TryCaptureSoundCueSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				if (Profile.GetInputKind() >= EAudioMediaGraphInputKind::TextureGraphNode
					&& Profile.GetInputKind()
						<= EAudioMediaGraphInputKind::TextureGraphValidateEvaluateSettle)
				{
					return TryCaptureTextureGraphSnapshot(
						Request, *Resources, OutSnapshot, OutError);
				}
				SetError(OutError,
					TEXT("graph_compound_audio_media_production_binding_unimplemented"),
					Profile.GetCapabilityId());
				return false;
			}

			bool TryExecuteOperation(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				{
					FGraphCompoundAudioMediaLifecycleInvocation Invocation;
					if (!TryCreateOperationLifecycleInvocation(
							Profile,
							Request,
							Plan,
							CallerContext,
							MoveTemp(Resources),
							Cancellation,
							Invocation,
							OutError))
					{
						return false;
					}
					FOperationLifecycleReceipt Receipt;
					if (!Invocation.GetService()->Run(
							*Invocation.GetPlan(),
							*Invocation.GetAuthorization(),
							Receipt))
					{
						SetError(OutError,
							Receipt.GetErrorCode().IsEmpty()
								? TEXT("graph_compound_audio_media_operation_failed")
								: Receipt.GetErrorCode(),
							Request.GetTargetRef(),
							LexToString(Receipt.GetStatus()));
						return false;
					}
					return Invocation.GetContext()->TryBuildResult(
						Receipt, OutResult, OutError);
				}
			}
			bool TryCreateOperationLifecycleInvocation(
				const FAudioMediaGraphRouteProfile& Profile,
				const FGraphCompoundAudioMediaOperationRequest& Request,
				const FPlanningPreflightPlanRef& Plan,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
				FGraphCompoundRuntimeError& OutError) override
			{
				OutInvocation = {};
				OutError = {};
				const bool bSupported = Profile.GetInputKind()
					== EAudioMediaGraphInputKind::AssetCreate
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::MetaSoundInterface
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::MetaSoundCompileSettle)
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::SoundCueNode
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::SoundCueCompileSettle)
					|| (Profile.GetInputKind()
						>= EAudioMediaGraphInputKind::TextureGraphNode
						&& Profile.GetInputKind()
							<= EAudioMediaGraphInputKind::
								TextureGraphValidateEvaluateSettle);
				if (!bSupported || !IsInGameThread()
					|| Cancellation->IsCancelled()
					|| Plan->GetDescriptorIdentity()
						!= Profile.GetExecutionContract().GetDescriptorIdentity()
					|| Plan->GetChangeSet().GetChanges().IsEmpty())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_audio_media_cancelled")
							: TEXT("graph_compound_audio_media_plan_invalid"),
						Request.GetTargetRef());
					return false;
				}
				for (const FPlannedChange& Change
					: Plan->GetChangeSet().GetChanges())
				{
					if (Change.GetIdentity().GetOwnerId()
						!= Request.GetTargetRef())
					{
						SetError(OutError,
							TEXT("graph_compound_audio_media_plan_target_mismatch"),
							Change.GetIdentity().GetOwnerId());
						return false;
					}
				}
				FAudioMediaReadbackSpec Spec;
				if (!TryParseReadbackSpec(Profile, Request, Spec, OutError))
				{
					return false;
				}
				const FString ExpectedRevision =
					Profile.GetInputKind() == EAudioMediaGraphInputKind::AssetCreate
						? FString(TEXT("absent"))
						: (Spec.ExpectedRevision.IsEmpty()
							? Plan->GetObservedRevision().GetGeneration()
							: Spec.ExpectedRevision);
				if (ExpectedRevision.IsEmpty())
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_expected_revision_missing"),
						Request.GetTargetRef());
					return false;
				}
				const TSharedRef<FAudioMediaLifecycleContext> Context =
					MakeShared<FAudioMediaLifecycleContext>(
						Profile,
						Request,
						Plan,
						CallerContext,
						MoveTemp(Resources),
						Cancellation,
						MoveTemp(Spec),
						JournalRoot);
				const TArray<FString> ImpactScope = {Request.GetTargetRef()};
				const TSharedRef<const FOperationLifecyclePlan> LifecyclePlan =
					MakeShared<FOperationLifecyclePlan>(
						TEXT("graph-compound-audio-media:") + Plan->GetRequestId(),
						Plan->GetFingerprint(),
						ImpactScope,
						TMap<FString, FString>{
							{Request.GetTargetRef(), ExpectedRevision}},
						TArray<FString>{Profile.GetCapabilityId()});
				const TSharedRef<const FOperationImpactAuthorization> Authorization =
					MakeShared<FOperationImpactAuthorization>(
						Plan->GetFingerprint(), ImpactScope);
				const TSharedRef<FOperationLifecycleService> Service =
					MakeShared<FOperationLifecycleService>(
						MakeShared<FAudioMediaVersionFence>(Context),
						MakeShared<FAudioMediaMutation>(Context),
						MakeShared<FAudioMediaStability>(Context),
						MakeShared<FAudioMediaPersistence>(Context),
						MakeShared<FAudioMediaVerification>(Context));
				return FGraphCompoundAudioMediaLifecycleInvocation::TryCreate(
					LifecyclePlan,
					Authorization,
					Service,
					StaticCastSharedRef<
						IGraphCompoundAudioMediaLifecycleContext>(Context),
					OutInvocation,
					OutError);
			}

			bool TryExecuteQuery(
				const FAudioMediaGraphRouteProfile& Profile,
				const FAudioMediaGraphArtifactQueryRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
				FUeAgentContractValue& OutResult,
				FGraphCompoundRuntimeError& OutError) override
			{
				(void)CallerContext;
				(void)Resources;
				OutResult = FUeAgentContractValue(TEXT("{}"));
				OutError = {};
				if (Profile.GetInputKind()
						!= EAudioMediaGraphInputKind::TextureGraphArtifactCapture
					|| !IsInGameThread() || Cancellation->IsCancelled())
				{
					SetError(OutError,
						Cancellation->IsCancelled()
							? TEXT("graph_compound_audio_media_cancelled")
							: TEXT("graph_compound_audio_media_query_invalid"),
						Profile.GetCapabilityId(), Request.GetTargetRef());
					return false;
				}
				const FString Revision =
					ReadPackageAssetRevision(Request.GetTargetRef());
				if (Revision.IsEmpty()
					|| Revision != Request.GetExpectedSettledRevision())
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_revision_conflict"),
						Request.GetTargetRef(), Revision);
					return false;
				}
				UTextureGraph* const Asset = LoadObject<UTextureGraph>(
					nullptr, *TargetObjectPath(Request.GetTargetRef()));
				if (Asset == nullptr || Asset->Graph() == nullptr)
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_target_invalid"),
						Request.GetTargetRef());
					return false;
				}
				TArray<FAudioMediaGraphTextureGraphArtifactSelector> Selectors;
				for (const FAudioMediaGraphArtifactOutputSelector& Output
					: Request.GetOutputs())
				{
					Selectors.Emplace(Output.GetElementId(), Output.GetOutputName());
				}
				TArray<FAudioMediaGraphTextureGraphCapturedOutput> Captured;
				FAudioMediaGraphError NativeError;
				if (!TextureGraphHook.TryCaptureArtifacts(
						*Asset, Selectors, Captured, NativeError))
				{
					SetError(OutError,
						NativeError.GetCode().IsEmpty()
							? TEXT("graph_compound_audio_media_capture_failed")
							: NativeError.GetCode(),
						Request.GetTargetRef(), NativeError.GetMessage());
					return false;
				}
				const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetBoolField(TEXT("published"), false);
				Root->SetStringField(TEXT("settled_revision"), Revision);
				Root->SetStringField(TEXT("target_ref"), Request.GetTargetRef());
				TArray<TSharedPtr<FJsonValue>> Outputs;
				for (const FAudioMediaGraphTextureGraphCapturedOutput& Output : Captured)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("expression_object_path"),
						Output.GetExpressionObjectPath());
					Item->SetStringField(TEXT("output_name"), Output.GetOutputName());
					Item->SetNumberField(TEXT("node_index"), Output.GetNodeIndex());
					Item->SetNumberField(TEXT("pin_index"), Output.GetPinIndex());
					Item->SetStringField(TEXT("evaluated_value_type"),
						Output.GetEvaluatedValueType());
					Item->SetStringField(TEXT("settings_fingerprint"),
						Output.GetSettingsFingerprint());
					Outputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				Root->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
				FString Canonical;
				FString JsonError;
				if (!Json::TryCanonicalize(Root, Canonical, JsonError))
				{
					SetError(OutError,
						TEXT("graph_compound_audio_media_result_invalid"),
						Request.GetTargetRef(), JsonError);
					return false;
				}
				OutResult = FUeAgentContractValue(MoveTemp(Canonical));
				return true;
			}

		private:
			FString JournalRoot;
			FAudioMediaGraphMetaSoundNativeHook MetaSoundHook;
			FAudioMediaGraphSoundCueNativeHook SoundCueHook;
			FAudioMediaGraphTextureGraphNativeHook TextureGraphHook;
			FPackageAssetDurableBackend DurableBackend;
			int64 LastFencingToken = 0;
		};
	}

	bool TryCreateAudioMediaGraphProductionEnvironment(
		const FGraphCompoundAudioMediaProductionConfig& Config,
		TSharedPtr<IGraphCompoundAudioMediaEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError)
	{
		OutEnvironment.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(OutError,
				TEXT("graph_compound_audio_media_config_invalid"),
				TEXT("audio_media_graph"));
			return false;
		}
		OutEnvironment = MakeShared<FAudioMediaProductionEnvironment>(Config);
		return OutEnvironment.IsValid();
	}
}
