#include "CommandRegistry/UeAgentActiveCommandManifest.h"

#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::UAI::CommandRegistry
{
	using namespace CommandContract;

	namespace
	{
		const FString WireSchema(TEXT("uai.command.v1"));

		FString Clean(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		TSharedPtr<FJsonValue> IntegerValue(const int32 Value)
		{
			return MakeShared<FJsonValueNumberString>(
				FString::FromInt(Value));
		}

		TSharedPtr<FJsonObject> ParameterObject(
			const FUeAgentCommandFieldDeclaration& Field)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Field.Name);
			Object->SetStringField(TEXT("wire_type"), Field.WireType);
			Object->SetBoolField(TEXT("required"), Field.bRequired);
			Object->SetBoolField(TEXT("nullable"), Field.bNullable);
			return Object;
		}

		TSharedPtr<FJsonObject> CommandObject(
			const FUeAgentFrozenCommandContract& Contract)
		{
			const TSharedPtr<FJsonObject> Object =
				MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("command_id"),
				Contract.GetCommandId());
			Object->SetStringField(
				TEXT("namespace"),
				Contract.GetPythonNamespace());
			Object->SetStringField(
				TEXT("method"),
				Contract.GetPythonFunction());
			TArray<TSharedPtr<FJsonValue>> Parameters;
			Parameters.Reserve(Contract.GetInputFields().Num());
			for (const FUeAgentCommandFieldDeclaration& Field :
				Contract.GetInputFields())
			{
				Parameters.Add(MakeShared<FJsonValueObject>(
					ParameterObject(Field)));
			}
			Object->SetArrayField(TEXT("parameters"), Parameters);
			Object->SetField(TEXT("min_wire_version"), IntegerValue(1));
			Object->SetField(TEXT("max_wire_version"), IntegerValue(1));
			Object->SetStringField(
				TEXT("status"),
				Contract.GetAvailability()
					== EUeAgentCommandAvailability::Enabled
					? TEXT("active")
					: TEXT("deprecated"));
			Object->SetStringField(
				TEXT("binding_hash"),
				Contract.GetContractHash());
			return Object;
		}

		TArray<uint8> Utf8Bytes(const FString& Text)
		{
			const FTCHARToUTF8 Converted(*Text);
			TArray<uint8> Result;
			Result.Append(
				reinterpret_cast<const uint8*>(Converted.Get()),
				Converted.Length());
			return Result;
		}
	}

	bool FUeAgentActiveCommandManifest::TryBuild(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		const FString& PluginVersion,
		const FString& BuildId,
		FString& OutManifest,
		FString& OutError)
	{
		OutManifest.Reset();
		OutError.Reset();
		const FString CleanPluginVersion = Clean(PluginVersion);
		const FString CleanBuildId = Clean(BuildId);
		if (CleanPluginVersion.IsEmpty() || CleanBuildId.IsEmpty())
		{
			OutError = TEXT("active_command_manifest_source_invalid");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Commands;
		Commands.Reserve(Registry->GetContracts().Num());
		for (const FUeAgentFrozenCommandContract& Contract :
			Registry->GetContracts())
		{
			Commands.Add(MakeShared<FJsonValueObject>(
				CommandObject(Contract)));
		}

		const TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(
			TEXT("kind"),
			TEXT("cpp_immutable_command_registry_export"));
		Source->SetStringField(TEXT("plugin_version"), CleanPluginVersion);
		Source->SetStringField(TEXT("build_id"), CleanBuildId);

		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.command_manifest.v1"));
		Root->SetField(TEXT("manifest_version"), IntegerValue(1));
		Root->SetStringField(TEXT("wire_schema"), WireSchema);
		Root->SetField(TEXT("wire_version"), IntegerValue(1));
		Root->SetObjectField(TEXT("source"), Source);
		Root->SetStringField(
			TEXT("registry_hash"),
			Registry->GetRegistryHash());
		Root->SetArrayField(TEXT("commands"), Commands);
		return Json::TryCanonicalize(Root, OutManifest, OutError);
	}

	bool FUeAgentActiveCommandManifest::TryWrite(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		const FString& PluginVersion,
		const FString& BuildId,
		const FString& ManifestPath,
		FString& OutError)
	{
		FString Manifest;
		if (!TryBuild(
			Registry,
			PluginVersion,
			BuildId,
			Manifest,
			OutError))
		{
			return false;
		}
		const FString Directory = FPaths::GetPath(ManifestPath);
		if (Directory.IsEmpty()
			|| !IFileManager::Get().MakeDirectory(*Directory, true))
		{
			OutError = TEXT("active_command_manifest_directory_failed");
			return false;
		}
		if (!FFileHelper::SaveArrayToFile(Utf8Bytes(Manifest), *ManifestPath))
		{
			OutError = TEXT("active_command_manifest_write_failed");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool FUeAgentActiveCommandManifest::TryVerify(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		const FString& PluginVersion,
		const FString& BuildId,
		const FString& ManifestPath,
		FString& OutError)
	{
		FString Expected;
		if (!TryBuild(
			Registry,
			PluginVersion,
			BuildId,
			Expected,
			OutError))
		{
			return false;
		}
		TArray<uint8> Actual;
		if (!FFileHelper::LoadFileToArray(Actual, *ManifestPath))
		{
			OutError = TEXT("active_command_manifest_file_missing");
			return false;
		}
		if (Actual != Utf8Bytes(Expected))
		{
			OutError = TEXT("active_command_manifest_projection_mismatch");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
