#include "Core/Contract/UeAgentCommandContract.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
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
			if (!Parameter.GetSchemaJson().IsEmpty())
			{
				TSharedPtr<FJsonObject> Schema;
				UE::UAI::Contract::FDiagnostic Error;
				if (Json::TryParseObject(Parameter.GetSchemaJson(), Schema, Error))
				{
					Object->SetObjectField(TEXT("schema"), Schema);
				}
			}
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
			UE::UAI::Contract::FDiagnostic Error;
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
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(
				MakeShared<FJsonValueArray>(Commands),
				Canonical,
				Error))
			{
				return FString();
			}
			return Json::Sha256Utf8(Canonical);
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
		const bool bInNullable,
		FString InSchemaJson)
		: Name(RequiredText(MoveTemp(InName)))
		, WireType(InWireType)
		, bRequired(bInRequired)
		, bNullable(bInNullable)
		, SchemaJson(MoveTemp(InSchemaJson))
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

	const FString& FUeAgentCommandParameter::GetSchemaJson() const
	{
		return SchemaJson;
	}

	bool FUeAgentCommandParameter::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCommandParameter::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (Name.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_parameter_invalid"),
				TEXT("A command parameter requires a non-empty name."), TEXT("contract_validation"));
			OutError.PropertyPath = TEXT("name");
			return false;
		}
		if (FString(WireTypeText(WireType)).IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_parameter_invalid"),
				TEXT("The command parameter uses an unsupported wire type."), TEXT("contract_validation"), FString(), Name);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Name;
			OutError.PropertyPath = TEXT("wire_type");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(WireType));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("boolean, integer, number, string, object, array or any"));
			return false;
		}
		return true;
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
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCommandContract::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The command contract requires a non-empty command ID."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			return false;
		}
		if (Namespace.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The command contract requires a non-empty Python namespace."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("namespace");
			return false;
		}
		if (Method.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The command contract requires a non-empty Python method."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("method");
			return false;
		}
		if (MinWireVersion < 1)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The minimum wire version must be at least one."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("min_wire_version");
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(MinWireVersion);
			return false;
		}
		if (MaxWireVersion < MinWireVersion)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The maximum wire version is less than the minimum wire version."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("max_wire_version");
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(MinWireVersion);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(MaxWireVersion);
			return false;
		}
		if (!IsSha256Identity(BindingHash))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("The command binding hash could not be computed as a SHA-256 identity."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("binding_hash");
			return false;
		}
		if (Status == EUeAgentCommandStatus::Deprecated && Deprecation.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
				TEXT("A deprecated command requires a deprecation explanation."), TEXT("contract_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("deprecation");
			return false;
		}
		TSet<FString> Names;
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			const FUeAgentCommandParameter& Parameter = Parameters[Index];
			if (!Parameter.TryValidate(OutError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("command_contract_invalid"), OutError, TEXT("contract_validation"));
				OutError.CandidateName = CommandId;
				OutError.PropertyPath = FString::Printf(TEXT("parameters[%d]."), Index) + OutError.PropertyPath;
				OutError.SubjectId = CommandId;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				return false;
			}
			if (Names.Contains(Parameter.GetName()))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_invalid"),
					TEXT("The command parameter name is declared more than once."), TEXT("contract_validation"), FString(), CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
				OutError.PropertyPath = FString::Printf(TEXT("parameters[%d].name"), Index);
				OutError.ActualValue = MakeShared<FJsonValueString>(Parameter.GetName());
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Contracts.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_registry_empty"), TEXT("The command contract registry contains no commands."), TEXT("registry"));
			OutError.PropertyPath = TEXT("commands");
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
			if (!Contracts[Index].TryValidate(OutError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("command_contract_invalid"), OutError, TEXT("registry"));
				OutError.CandidateName = Contracts[Index].GetCommandId();
				return false;
			}
			if (Index > 0
				&& Contracts[Index - 1].GetCommandId()
					== Contracts[Index].GetCommandId())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_id_duplicate"), TEXT("The command ID is registered more than once."), TEXT("registry"));
				OutError.CandidateName = Contracts[Index].GetCommandId();
				OutError.PropertyPath = TEXT("command_id");
				return false;
			}
		}
		const FString RegistryHash =
			CalculateRegistryHash(Contracts);
		if (!IsSha256Identity(RegistryHash))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_contract_registry_hash_failed"), TEXT("The command contract registry hash could not be computed."), TEXT("registry"));
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
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutManifest.Reset();
		OutError.Reset();
		const FString CleanPluginVersion =
			RequiredText(PluginVersion);
		const FString CleanBuildId = RequiredText(BuildId);
		if (CleanPluginVersion.IsEmpty() || CleanBuildId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_manifest_source_invalid"), TEXT("The command manifest requires non-empty plugin version and build ID fields."), TEXT("registry"));
			OutError.PropertyPath = CleanPluginVersion.IsEmpty() ? TEXT("plugin_version") : TEXT("build_id");
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
			AddReadCommandContracts(Contracts);
			AddSemanticExportCommandContract(Contracts);
			TSharedPtr<const FUeAgentCommandContractRegistry>
				Created;
			UE::UAI::Contract::FDiagnostic Error;
			const bool bCreated =
				FUeAgentCommandContractRegistry::TryCreate(
					MoveTemp(Contracts),
					Created,
					Error);
			checkf(
				bCreated && Created.IsValid(),
				TEXT("Default command registry invalid: %s"),
				*Error.Code);
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
