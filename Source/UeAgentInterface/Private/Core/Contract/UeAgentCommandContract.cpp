#include "Core/Contract/UeAgentCommandContract.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::UAI::CommandContract
{
	namespace
	{
		const FString WireSchema(TEXT("uai.command.v1"));
		const FString ManifestBuildId(
			TEXT("uai-command-registry-v1"));

		FString RequiredText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		const TCHAR* WireTypeText(
			const EUeAgentCommandWireType Type)
		{
			switch (Type)
			{
			case EUeAgentCommandWireType::Boolean:
				return TEXT("boolean");
			case EUeAgentCommandWireType::Integer:
				return TEXT("integer");
			case EUeAgentCommandWireType::Number:
				return TEXT("number");
			case EUeAgentCommandWireType::String:
				return TEXT("string");
			case EUeAgentCommandWireType::Object:
				return TEXT("object");
			case EUeAgentCommandWireType::Array:
				return TEXT("array");
			case EUeAgentCommandWireType::Any:
				return TEXT("any");
			default:
				return TEXT("");
			}
		}

		const TCHAR* StatusText(const EUeAgentCommandStatus Status)
		{
			return Status == EUeAgentCommandStatus::Deprecated
				? TEXT("deprecated")
				: TEXT("active");
		}

		TSharedPtr<FJsonValue> IntegerValue(const int32 Value)
		{
			return MakeShared<FJsonValueNumberString>(
				FString::FromInt(Value));
		}

		TSharedPtr<FJsonObject> ParameterObject(
			const FUeAgentCommandParameter& Parameter)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("name"),
				Parameter.GetName());
			Object->SetStringField(
				TEXT("wire_type"),
				WireTypeText(Parameter.GetWireType()));
			Object->SetBoolField(
				TEXT("required"),
				Parameter.IsRequired());
			Object->SetBoolField(
				TEXT("nullable"),
				Parameter.IsNullable());
			return Object;
		}

		bool IsSha256Identity(const FString& Value)
		{
			return Value.Len() == 71
				&& Value.Left(7) == TEXT("sha256:");
		}

		TArray<TSharedPtr<FJsonValue>> ParameterArray(
			const FUeAgentCommandContract& Contract)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Contract.GetParameters().Num());
			for (const FUeAgentCommandParameter& Parameter :
				Contract.GetParameters())
			{
				Result.Add(MakeShared<FJsonValueObject>(
					ParameterObject(Parameter)));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> BindingObject(
			const FUeAgentCommandContract& Contract)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("command_id"),
				Contract.GetCommandId());
			Object->SetStringField(
				TEXT("namespace"),
				Contract.GetNamespace());
			Object->SetStringField(
				TEXT("method"),
				Contract.GetMethod());
			Object->SetArrayField(
				TEXT("parameters"),
				ParameterArray(Contract));
			Object->SetStringField(
				TEXT("wire_schema"),
				WireSchema);
			Object->SetField(
				TEXT("min_wire_version"),
				IntegerValue(Contract.GetMinWireVersion()));
			Object->SetField(
				TEXT("max_wire_version"),
				IntegerValue(Contract.GetMaxWireVersion()));
			return Object;
		}

		FString CalculateBindingHash(
			const FUeAgentCommandContract& Contract)
		{
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(
				BindingObject(Contract),
				Canonical,
				Error))
			{
				return FString();
			}
			return Json::Sha256Utf8(Canonical);
		}

		TSharedPtr<FJsonObject> ManifestCommandObject(
			const FUeAgentCommandContract& Contract)
		{
			const TSharedPtr<FJsonObject> Object =
				BindingObject(Contract);
			Object->RemoveField(TEXT("wire_schema"));
			Object->SetStringField(
				TEXT("status"),
				StatusText(Contract.GetStatus()));
			if (!Contract.GetDeprecation().IsEmpty())
			{
				Object->SetStringField(
					TEXT("deprecation"),
					Contract.GetDeprecation());
			}
			Object->SetStringField(
				TEXT("binding_hash"),
				Contract.GetBindingHash());
			return Object;
		}

		FString CalculateRegistryHash(
			const TArray<FUeAgentCommandContract>& Contracts)
		{
			TArray<TSharedPtr<FJsonValue>> Commands;
			Commands.Reserve(Contracts.Num());
			for (const FUeAgentCommandContract& Contract :
				Contracts)
			{
				Commands.Add(MakeShared<FJsonValueObject>(
					ManifestCommandObject(Contract)));
			}
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(
				MakeShared<FJsonValueArray>(Commands),
				Canonical,
				Error))
			{
				return FString();
			}
			return Json::Sha256Utf8(Canonical);
		}

		TArray<uint8> Utf8Bytes(const FString& Text)
		{
			const FTCHARToUTF8 Converted(*Text);
			TArray<uint8> Result;
			Result.Append(
				reinterpret_cast<const uint8*>(
					Converted.Get()),
				Converted.Length());
			return Result;
		}

		void AddReadCommandContracts(
			TArray<FUeAgentCommandContract>& Contracts)
		{
			for (const FUeAgentReadCommandContractDefinition&
				Definition : GetReadCommandContractDefinitions())
			{
				const int32 Separator =
					Definition.CommandId.Find(
						TEXT("."),
						ESearchCase::CaseSensitive,
						ESearchDir::FromEnd);
				const FString Namespace =
					Definition.CommandId.Left(Separator);
				const FString Method =
					Definition.CommandId.Mid(Separator + 1);
				TArray<FUeAgentCommandParameter> Parameters;
				if (Definition.bRequiresTarget)
				{
					Parameters.Emplace(
						TEXT("target"),
						EUeAgentCommandWireType::String,
						true,
						false);
				}
				Parameters.Emplace(
					TEXT("input"),
					EUeAgentCommandWireType::Object,
					false,
					false);
				Parameters.Emplace(
					TEXT("context"),
					EUeAgentCommandWireType::Object,
					false,
					false);
				Parameters.Emplace(
					TEXT("projection"),
					EUeAgentCommandWireType::Object,
					false,
					false);
				Contracts.Emplace(
					Definition.CommandId,
					Namespace,
					Method,
					MoveTemp(Parameters),
					1,
					1,
					EUeAgentCommandStatus::Active);
			}
		}

		void AddSemanticExportCommandContract(
			TArray<FUeAgentCommandContract>& Contracts)
		{
			Contracts.Emplace(
				GetSemanticExportCommandId(),
				TEXT("content_graph"),
				TEXT("export_workspace"),
				TArray<FUeAgentCommandParameter>{
					{
						TEXT("target"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("workspace_path"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("input"),
						EUeAgentCommandWireType::Object,
						false,
						false,
					},
					{
						TEXT("context"),
						EUeAgentCommandWireType::Object,
						false,
						false,
					},
					{
						TEXT("projection"),
						EUeAgentCommandWireType::Object,
						false,
						false,
					},
					{
						TEXT("profile_id"),
						EUeAgentCommandWireType::String,
						false,
						false,
					},
				},
				1,
				1,
				EUeAgentCommandStatus::Active);
		}
	}

	FUeAgentCommandParameter::FUeAgentCommandParameter(
		FString InName,
		const EUeAgentCommandWireType InWireType,
		const bool bInRequired,
		const bool bInNullable)
		: Name(RequiredText(MoveTemp(InName)))
		, WireType(InWireType)
		, bRequired(bInRequired)
		, bNullable(bInNullable)
	{
	}

	const FString& FUeAgentCommandParameter::GetName() const
	{
		return Name;
	}

	EUeAgentCommandWireType
	FUeAgentCommandParameter::GetWireType() const
	{
		return WireType;
	}

	bool FUeAgentCommandParameter::IsRequired() const
	{
		return bRequired;
	}

	bool FUeAgentCommandParameter::IsNullable() const
	{
		return bNullable;
	}

	bool FUeAgentCommandParameter::IsValid() const
	{
		return !Name.IsEmpty()
			&& FString(WireTypeText(WireType)).Len() > 0;
	}

	FUeAgentCommandContract::FUeAgentCommandContract(
		FString InCommandId,
		FString InNamespace,
		FString InMethod,
		TArray<FUeAgentCommandParameter> InParameters,
		const int32 InMinWireVersion,
		const int32 InMaxWireVersion,
		const EUeAgentCommandStatus InStatus,
		FString InDeprecation)
		: CommandId(RequiredText(MoveTemp(InCommandId)))
		, Namespace(RequiredText(MoveTemp(InNamespace)))
		, Method(RequiredText(MoveTemp(InMethod)))
		, Parameters(MoveTemp(InParameters))
		, MinWireVersion(InMinWireVersion)
		, MaxWireVersion(InMaxWireVersion)
		, Status(InStatus)
		, Deprecation(RequiredText(MoveTemp(InDeprecation)))
	{
		BindingHash = CalculateBindingHash(*this);
	}

	const FString& FUeAgentCommandContract::GetCommandId() const
	{
		return CommandId;
	}

	const FString& FUeAgentCommandContract::GetNamespace() const
	{
		return Namespace;
	}

	const FString& FUeAgentCommandContract::GetMethod() const
	{
		return Method;
	}

	const TArray<FUeAgentCommandParameter>&
	FUeAgentCommandContract::GetParameters() const
	{
		return Parameters;
	}

	int32 FUeAgentCommandContract::GetMinWireVersion() const
	{
		return MinWireVersion;
	}

	int32 FUeAgentCommandContract::GetMaxWireVersion() const
	{
		return MaxWireVersion;
	}

	EUeAgentCommandStatus FUeAgentCommandContract::GetStatus() const
	{
		return Status;
	}

	const FString& FUeAgentCommandContract::GetDeprecation() const
	{
		return Deprecation;
	}

	const FString& FUeAgentCommandContract::GetBindingHash() const
	{
		return BindingHash;
	}

	bool FUeAgentCommandContract::IsValid() const
	{
		if (CommandId.IsEmpty()
			|| Namespace.IsEmpty()
			|| Method.IsEmpty()
			|| MinWireVersion < 1
			|| MaxWireVersion < MinWireVersion
			|| !IsSha256Identity(BindingHash)
			|| (Status == EUeAgentCommandStatus::Deprecated
				&& Deprecation.IsEmpty()))
		{
			return false;
		}
		TSet<FString> Names;
		for (const FUeAgentCommandParameter& Parameter : Parameters)
		{
			if (!Parameter.IsValid()
				|| Names.Contains(Parameter.GetName()))
			{
				return false;
			}
			Names.Add(Parameter.GetName());
		}
		return true;
	}

	FUeAgentCommandContractRegistry::
		FUeAgentCommandContractRegistry(
			TArray<FUeAgentCommandContract> InContracts,
			FString InRegistryHash)
		: Contracts(MoveTemp(InContracts))
		, RegistryHash(MoveTemp(InRegistryHash))
	{
		for (int32 Index = 0; Index < Contracts.Num(); ++Index)
		{
			IndexByCommandId.Add(
				Contracts[Index].GetCommandId(),
				Index);
		}
	}

	bool FUeAgentCommandContractRegistry::TryCreate(
		TArray<FUeAgentCommandContract> Contracts,
		TSharedPtr<const FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Contracts.IsEmpty())
		{
			OutError = TEXT("command_contract_registry_empty");
			return false;
		}
		Contracts.Sort(
			[](const FUeAgentCommandContract& Left,
			   const FUeAgentCommandContract& Right)
			{
				return Left.GetCommandId() < Right.GetCommandId();
			});
		for (int32 Index = 0; Index < Contracts.Num(); ++Index)
		{
			if (!Contracts[Index].IsValid())
			{
				OutError = TEXT("command_contract_invalid");
				return false;
			}
			if (Index > 0
				&& Contracts[Index - 1].GetCommandId()
					== Contracts[Index].GetCommandId())
			{
				OutError = TEXT("command_contract_id_duplicate");
				return false;
			}
		}
		const FString RegistryHash =
			CalculateRegistryHash(Contracts);
		if (!IsSha256Identity(RegistryHash))
		{
			OutError = TEXT("command_contract_registry_hash_failed");
			return false;
		}
		OutRegistry =
			MakeShareable(new FUeAgentCommandContractRegistry(
				MoveTemp(Contracts),
				RegistryHash));
		return true;
	}

	const TArray<FUeAgentCommandContract>&
	FUeAgentCommandContractRegistry::GetContracts() const
	{
		return Contracts;
	}

	const FUeAgentCommandContract*
	FUeAgentCommandContractRegistry::FindExact(
		const FString& CommandId) const
	{
		const int32* Index = IndexByCommandId.Find(CommandId);
		return Index ? &Contracts[*Index] : nullptr;
	}

	const FString&
	FUeAgentCommandContractRegistry::GetRegistryHash() const
	{
		return RegistryHash;
	}

	bool FUeAgentCommandContractRegistry::BuildManifest(
		const FString& PluginVersion,
		const FString& BuildId,
		FString& OutManifest,
		FString& OutError) const
	{
		OutManifest.Reset();
		OutError.Reset();
		const FString CleanPluginVersion =
			RequiredText(PluginVersion);
		const FString CleanBuildId = RequiredText(BuildId);
		if (CleanPluginVersion.IsEmpty() || CleanBuildId.IsEmpty())
		{
			OutError = TEXT("command_manifest_source_invalid");
			return false;
		}
		const TSharedPtr<FJsonObject> Source =
			MakeShared<FJsonObject>();
		Source->SetStringField(
			TEXT("kind"),
			TEXT("cpp_command_registry_export"));
		Source->SetStringField(
			TEXT("plugin_version"),
			CleanPluginVersion);
		Source->SetStringField(TEXT("build_id"), CleanBuildId);

		TArray<TSharedPtr<FJsonValue>> Commands;
		Commands.Reserve(Contracts.Num());
		for (const FUeAgentCommandContract& Contract : Contracts)
		{
			Commands.Add(MakeShared<FJsonValueObject>(
				ManifestCommandObject(Contract)));
		}
		const TSharedPtr<FJsonObject> Root =
			MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("schema"),
			TEXT("uai.command_manifest.v1"));
		Root->SetField(
			TEXT("manifest_version"),
			IntegerValue(1));
		Root->SetStringField(TEXT("wire_schema"), WireSchema);
		Root->SetField(TEXT("wire_version"), IntegerValue(1));
		Root->SetObjectField(TEXT("source"), Source);
		Root->SetStringField(
			TEXT("registry_hash"),
			RegistryHash);
		Root->SetArrayField(TEXT("commands"), Commands);
		return Json::TryCanonicalize(
			Root,
			OutManifest,
			OutError);
	}

	bool FUeAgentCommandManifestExporter::TryWrite(
		const FUeAgentCommandContractRegistry& Registry,
		const FString& PluginVersion,
		const FString& BuildId,
		const FString& ManifestPath,
		FString& OutError)
	{
		OutError.Reset();
		FString Manifest;
		if (!Registry.BuildManifest(
			PluginVersion,
			BuildId,
			Manifest,
			OutError))
		{
			return false;
		}
		const FString Directory =
			FPaths::GetPath(ManifestPath);
		if (Directory.IsEmpty()
			|| !IFileManager::Get().MakeDirectory(
				*Directory,
				true))
		{
			OutError = TEXT("command_manifest_directory_failed");
			return false;
		}
		const TArray<uint8> Bytes = Utf8Bytes(Manifest);
		if (!FFileHelper::SaveArrayToFile(Bytes, *ManifestPath))
		{
			OutError = TEXT("command_manifest_write_failed");
			return false;
		}
		return true;
	}

	bool FUeAgentCommandManifestExporter::TryVerify(
		const FUeAgentCommandContractRegistry& Registry,
		const FString& PluginVersion,
		const FString& BuildId,
		const FString& ManifestPath,
		FString& OutError)
	{
		OutError.Reset();
		FString Manifest;
		if (!Registry.BuildManifest(
			PluginVersion,
			BuildId,
			Manifest,
			OutError))
		{
			return false;
		}
		TArray<uint8> Actual;
		if (!FFileHelper::LoadFileToArray(Actual, *ManifestPath))
		{
			OutError = TEXT("command_manifest_file_missing");
			return false;
		}
		if (Actual != Utf8Bytes(Manifest))
		{
			OutError = TEXT("command_manifest_projection_mismatch");
			return false;
		}
		return true;
	}

	bool FUeAgentReadCommandContractDefinition::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& CommandId.Contains(TEXT("."))
			&& !CommandId.StartsWith(TEXT("."))
			&& !CommandId.EndsWith(TEXT("."));
	}

	const TArray<FUeAgentReadCommandContractDefinition>&
	GetReadCommandContractDefinitions()
	{
		static const TArray<
			FUeAgentReadCommandContractDefinition> Definitions = {
			{TEXT("content_graph.summary"), true},
			{TEXT("content_graph.structure"), true},
			{TEXT("content_graph.items"), true},
			{TEXT("content_graph.relations"), true},
			{TEXT("content_graph.properties"), true},
			{TEXT("content_graph.diagnostics"), true},
			{TEXT("content_graph.compound"), true},
		};
		return Definitions;
	}

	const FString& GetSemanticExportCommandId()
	{
		static const FString CommandId(
			TEXT("content_graph.export_workspace"));
		return CommandId;
	}

	TSharedRef<const FUeAgentCommandContractRegistry>
	GetDefaultCommandContractRegistry()
	{
		static const TSharedRef<
			const FUeAgentCommandContractRegistry> Registry = []()
		{
			TArray<FUeAgentCommandContract> Contracts;
			Contracts.Emplace(
				TEXT("reference.catalog_search"),
				TEXT("reference"),
				TEXT("catalog_search"),
				TArray<FUeAgentCommandParameter>{
					{
						TEXT("catalog_kind"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("search_text"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("match_mode"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("materialization_policy"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("projection"),
						EUeAgentCommandWireType::Object,
						false,
						false,
					},
				},
				1,
				1,
				EUeAgentCommandStatus::Active);
			Contracts.Emplace(
				TEXT("reference.asset_apply"),
				TEXT("reference"),
				TEXT("asset_apply"),
				TArray<FUeAgentCommandParameter>{
					{
						TEXT("object_path"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("intent"),
						EUeAgentCommandWireType::String,
						true,
						false,
					},
					{
						TEXT("proposed_text"),
						EUeAgentCommandWireType::String,
						false,
						true,
					},
				},
				1,
				1,
				EUeAgentCommandStatus::Active);
			AddReadCommandContracts(Contracts);
			AddSemanticExportCommandContract(Contracts);
			TSharedPtr<const FUeAgentCommandContractRegistry>
				Created;
			FString Error;
			const bool bCreated =
				FUeAgentCommandContractRegistry::TryCreate(
					MoveTemp(Contracts),
					Created,
					Error);
			checkf(
				bCreated && Created.IsValid(),
				TEXT("Default command registry invalid: %s"),
				*Error);
			return Created.ToSharedRef();
		}();
		return Registry;
	}

	const FString& GetUaiCommandManifestBuildId()
	{
		return ManifestBuildId;
	}

	FString GetUaiCommandManifestPluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(
				TEXT("UeAgentInterface"));
		return Plugin.IsValid()
			? Plugin->GetDescriptor().VersionName
			: FString();
	}

	FString GetDefaultCommandManifestPath()
	{
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(
				TEXT("UeAgentInterface"));
		return Plugin.IsValid()
			? FPaths::Combine(
				Plugin->GetBaseDir(),
				TEXT("Resources"),
				TEXT("UaiCommandManifest.json"))
			: FString();
	}
}
