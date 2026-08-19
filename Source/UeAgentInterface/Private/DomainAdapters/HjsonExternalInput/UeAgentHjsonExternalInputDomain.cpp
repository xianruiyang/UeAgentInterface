#include "DomainAdapters/HjsonExternalInput/UeAgentHjsonExternalInputDomain.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	using namespace HjsonWorkspace;
	using namespace ContextResource;
	using namespace CommandContract;

	namespace
	{
		const FString SchemaId = TEXT("uai.hjson.external_input/1");

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 * 1024 * 1024},
			}));
		}

		TSharedPtr<FJsonValue> ToJsonValue(const FHjsonNode& Node, bool& bOutOk)
		{
			bOutOk = true;
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
			{
				double Value = 0.0;
				bOutOk = LexTryParseString(Value, *Node.GetLexeme());
				if (!bOutOk)
				{
					return nullptr;
				}
				return MakeShared<FJsonValueNumber>(Value);
			}
			case EHjsonNodeKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FHjsonNodeRef& Item : Node.GetItems())
				{
					bool bItemOk = false;
					TSharedPtr<FJsonValue> Value = ToJsonValue(Item.Get(), bItemOk);
					if (!bItemOk || !Value.IsValid())
					{
						bOutOk = false;
						return nullptr;
					}
					Values.Add(MoveTemp(Value));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Values));
			}
			case EHjsonNodeKind::Object:
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					bool bValueOk = false;
					TSharedPtr<FJsonValue> Value = ToJsonValue(Entry.Value.Get(), bValueOk);
					if (!bValueOk || !Value.IsValid())
					{
						bOutOk = false;
						return nullptr;
					}
					Object->SetField(Entry.Key, MoveTemp(Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			default:
				bOutOk = false;
				return nullptr;
			}
		}

		bool StringField(
			const FHjsonNode& Object,
			const FString& Key,
			FString& OutValue)
		{
			const FHjsonNode* Node = Object.Get(Key);
			if (!Node || Node->GetKind() != EHjsonNodeKind::String)
			{
				OutValue.Reset();
				return false;
			}
			OutValue = Normalize(Node->GetText());
			return !OutValue.IsEmpty();
		}
	}

	bool FHjsonExternalInputFamilyContract::IsValid() const
	{
		if (RootTypeIdentity.IsEmpty() || InitializationContractId.IsEmpty()
			|| ImporterCommandIds.IsEmpty())
		{
			return false;
		}
		TSet<FString> Unique;
		for (const FString& CommandId : ImporterCommandIds)
		{
			if (CommandId.IsEmpty() || Unique.Contains(CommandId))
			{
				return false;
			}
			Unique.Add(CommandId);
		}
		return true;
	}

	bool FHjsonExternalInputDelegation::IsValid() const
	{
		return !CommandId.IsEmpty() && !CanonicalParamsJson.IsEmpty();
	}

	const TArray<FHjsonExternalInputFamilyContract>&
	GetHjsonExternalInputFamilyContracts()
	{
		static const TArray<FHjsonExternalInputFamilyContract> Contracts = {
			{EHjsonExternalInputFamily::Factory,
				TEXT("uai.external_input.factory"),
				TEXT("uai.init.hjson_external_input.factory_importer/1"),
				{
					TEXT("asset.reimport_sound_wave"),
					TEXT("asset.reimport_sparse_volume_texture"),
					TEXT("asset.reimport_static_mesh"),
					TEXT("asset.reimport_texture"),
					TEXT("external.fbx.animation.import"),
					TEXT("external.fbx.skeletal_mesh.import"),
					TEXT("external.geometry_cache.import"),
					TEXT("external.light_profile.import"),
					TEXT("external.sound_wave.import"),
					TEXT("external.sparse_volume_texture.import"),
					TEXT("external.texture.import"),
				}},
			{EHjsonExternalInputFamily::SkinWeightProfile,
				TEXT("uai.external_input.skin_weight_profile"),
				TEXT("uai.init.hjson_external_input.skin_weight_importer/1"),
				{TEXT("external.skin_weight_profile.import")}},
			{EHjsonExternalInputFamily::Localization,
				TEXT("uai.external_input.localization"),
				TEXT("uai.init.hjson_external_input.localization_importer/1"),
				{TEXT("external.localization.po.import")}},
		};
		return Contracts;
	}

	const FHjsonExternalInputFamilyContract*
	FindHjsonExternalInputFamilyContract(const FString& RootTypeIdentity)
	{
		return GetHjsonExternalInputFamilyContracts().FindByPredicate(
			[&RootTypeIdentity](const FHjsonExternalInputFamilyContract& Contract)
			{
				return Contract.RootTypeIdentity == RootTypeIdentity;
			});
	}

	bool TryBuildHjsonExternalInputDelegation(
		const FString& CanonicalContentGraphParamsJson,
		const FHjsonExternalInputFamilyContract& Contract,
		FHjsonExternalInputDelegation& OutDelegation,
		FWorkspaceError& OutError)
	{
		OutDelegation = {};
		OutError.Reset();
		if (!Contract.IsValid())
		{
			OutError.Set(TEXT("hjson_external_input_contract_invalid"));
			return false;
		}
		TSharedPtr<FJsonObject> Params;
		FString JsonError;
		if (!Json::TryParseObject(CanonicalContentGraphParamsJson, Params, JsonError)
			|| !Params.IsValid())
		{
			OutError.Set(TEXT("hjson_external_input_params_invalid"), JsonError);
			return false;
		}
		FString PublicTarget;
		if (Params->HasField(TEXT("carrier"))
			|| Params->HasField(TEXT("hjson_document")))
		{
			OutError.Set(TEXT("hjson_external_input_legacy_source_contract_rejected"));
			return false;
		}
		if (!Params->TryGetStringField(TEXT("target"), PublicTarget)
			|| Normalize(PublicTarget).IsEmpty())
		{
			OutError.Set(TEXT("hjson_external_input_target_required"));
			return false;
		}
		const TSharedPtr<FJsonObject>* ResolvedSource = nullptr;
		FString SourceKind;
		FString SourceTargetIdentity;
		FString EntryFile;
		const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
		if (!Params->TryGetObjectField(TEXT("source"), ResolvedSource)
			|| !ResolvedSource || !ResolvedSource->IsValid()
			|| !(*ResolvedSource)->TryGetStringField(TEXT("kind"), SourceKind)
			|| SourceKind != TEXT("resolved")
			|| !(*ResolvedSource)->TryGetStringField(
				TEXT("target_identity"), SourceTargetIdentity)
			|| SourceTargetIdentity != PublicTarget
			|| !(*ResolvedSource)->TryGetStringField(TEXT("entry_file"), EntryFile)
			|| EntryFile.IsEmpty()
			|| !(*ResolvedSource)->TryGetArrayField(TEXT("documents"), Documents)
			|| !Documents || Documents->IsEmpty())
		{
			OutError.Set(TEXT("hjson_external_input_resolved_source_invalid"));
			return false;
		}
		FString DocumentText;
		int32 EntryMatches = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Documents)
		{
			const TSharedPtr<FJsonObject> DocumentObject = Value.IsValid()
				? Value->AsObject()
				: nullptr;
			FString RelativePath;
			FString Text;
			if (!DocumentObject.IsValid()
				|| !DocumentObject->TryGetStringField(TEXT("path"), RelativePath)
				|| !DocumentObject->TryGetStringField(TEXT("document"), Text))
			{
				OutError.Set(TEXT("hjson_external_input_resolved_source_invalid"));
				return false;
			}
			if (RelativePath == EntryFile)
			{
				DocumentText = MoveTemp(Text);
				++EntryMatches;
			}
		}
		if (EntryMatches != 1 || DocumentText.IsEmpty())
		{
			OutError.Set(
				TEXT("hjson_external_input_resolved_source_invalid"), EntryFile);
			return false;
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FHjsonDocumentPtr Document;
		if (!TryParseHjsonDocument(
				DocumentText,
				TEXT("inline:hjson-external-input"),
				Resources,
				Document,
				OutError)
			|| !Document.IsValid())
		{
			return false;
		}
		const FHjsonNode& DocumentRoot = Document->GetRoot().Get();
		const FHjsonNode* Root = DocumentRoot.Get(TEXT("root"));
		const FHjsonNode* External = Root && Root->GetKind() == EHjsonNodeKind::Object
			? Root->Get(TEXT("external_input")) : nullptr;
		const FHjsonNode* Source = External && External->GetKind() == EHjsonNodeKind::Object
			? External->Get(TEXT("source")) : nullptr;
		const FHjsonNode* Options = External && External->GetKind() == EHjsonNodeKind::Object
			? External->Get(TEXT("options")) : nullptr;
		const FHjsonNode* Policy = External && External->GetKind() == EHjsonNodeKind::Object
			? External->Get(TEXT("target_policy")) : nullptr;
		FString Schema;
		FString RootType;
		FString SourceIdentity;
		FString Format;
		FString ContentSha256;
		FString CommandId;
		if (!StringField(DocumentRoot, TEXT("schema"), Schema) || Schema != SchemaId
			|| !Root || !StringField(*Root, TEXT("type"), RootType)
			|| RootType != Contract.RootTypeIdentity
			|| !Source || !StringField(*Source, TEXT("identity"), SourceIdentity)
			|| !StringField(*Source, TEXT("format"), Format)
			|| !StringField(*Source, TEXT("content_sha256"), ContentSha256)
			|| !IsSha256(ContentSha256)
			|| !Options || Options->GetKind() != EHjsonNodeKind::Object
			|| !Policy || Policy->GetKind() != EHjsonNodeKind::Object
			|| !StringField(*Policy, TEXT("command_id"), CommandId))
		{
			OutError.Set(TEXT("hjson_external_input_document_invalid"));
			return false;
		}
		if (!Contract.ImporterCommandIds.Contains(CommandId))
		{
			OutError.Set(
				TEXT("hjson_external_input_importer_unregistered"),
				CommandId,
				TEXT("/root/external_input/target_policy/command_id"));
			return false;
		}
		const FHjsonNode* PolicyParams = Policy->Get(TEXT("parameters"));
		if (!PolicyParams || PolicyParams->GetKind() != EHjsonNodeKind::Object)
		{
			OutError.Set(TEXT("hjson_external_input_target_parameters_invalid"));
			return false;
		}
		const FString TargetField = CommandId.StartsWith(TEXT("external."))
			&& CommandId != TEXT("external.skin_weight_profile.import")
			&& CommandId != TEXT("external.localization.po.import")
			? FString(TEXT("destination_ref"))
			: FString(TEXT("target_ref"));
		FString PolicyTarget;
		if (!StringField(*PolicyParams, TargetField, PolicyTarget)
			|| PolicyTarget != PublicTarget)
		{
			OutError.Set(
				TEXT("hjson_external_input_target_policy_mismatch"),
				PolicyTarget,
				TEXT("/root/external_input/target_policy/parameters/")
					+ TargetField);
			return false;
		}
		bool bParamsOk = false;
		TSharedPtr<FJsonValue> DelegatedValue = ToJsonValue(*PolicyParams, bParamsOk);
		const TSharedPtr<FJsonObject> DelegatedParams = bParamsOk && DelegatedValue.IsValid()
			? DelegatedValue->AsObject() : nullptr;
		if (!DelegatedParams.IsValid())
		{
			OutError.Set(TEXT("hjson_external_input_target_parameters_invalid"));
			return false;
		}
		DelegatedParams->SetStringField(TEXT("source_ref"), SourceIdentity);
		DelegatedParams->SetStringField(
			TEXT("_hjson_expected_source_sha256"),
			ContentSha256.ToLower());
		DelegatedParams->SetStringField(TEXT("_hjson_source_format"), Format.ToLower());
		if (!Options->GetEntries().IsEmpty())
		{
			bool bOptionsOk = false;
			TSharedPtr<FJsonValue> OptionsValue = ToJsonValue(*Options, bOptionsOk);
			if (!bOptionsOk || !OptionsValue.IsValid())
			{
				OutError.Set(TEXT("hjson_external_input_options_invalid"));
				return false;
			}
			DelegatedParams->SetField(TEXT("options"), MoveTemp(OptionsValue));
		}
		FString Canonical;
		if (!Json::TryCanonicalize(DelegatedParams, Canonical, JsonError))
		{
			OutError.Set(TEXT("hjson_external_input_delegation_invalid"), JsonError);
			return false;
		}
		OutDelegation.CommandId = MoveTemp(CommandId);
		OutDelegation.CanonicalParamsJson = MoveTemp(Canonical);
		return OutDelegation.IsValid();
	}
}
