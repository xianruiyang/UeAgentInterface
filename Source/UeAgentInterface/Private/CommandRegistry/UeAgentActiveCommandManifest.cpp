#include "CommandRegistry/UeAgentActiveCommandManifest.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "CommandRegistry/UeAgentOperationRecoveryPolicy.h"
#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/UeAgentContractJson.h"
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
			if (!Field.SchemaJson.IsEmpty())
			{
				TSharedPtr<FJsonObject> Schema;
				UE::UAI::Contract::FDiagnostic Error;
				if (Contract::Json::TryParseObject(
						Field.SchemaJson,
						Schema,
						Error)
					&& Schema.IsValid())
				{
					Object->SetObjectField(TEXT("schema"), MoveTemp(Schema));
				}
			}
			return Object;
		}

		TSharedPtr<FJsonObject> ModelResultFieldObject(
			const FUeAgentModelResultFieldDeclaration& Field)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Field.Name);
			TArray<TSharedPtr<FJsonValue>> SourcePath;
			SourcePath.Reserve(Field.SourcePath.Num());
			for (const FString& Segment : Field.SourcePath)
			{
				SourcePath.Add(MakeShared<FJsonValueString>(Segment));
			}
			Object->SetArrayField(TEXT("source_path"), MoveTemp(SourcePath));
			return Object;
		}

		bool TryCommandObject(
			const FUeAgentFrozenCommandContract& Contract,
			TSharedPtr<FJsonObject>& Object,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			Object = MakeShared<FJsonObject>();
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
			if (!Contract.GetVariants().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Variants;
				for (const FUeAgentCommandVariantDeclaration& Variant : Contract.GetVariants())
				{
					TSharedPtr<FJsonObject> RequestSchema;
					if (!Json::TryParseObject(Variant.RequestSchemaJson, RequestSchema, OutError))
					{
						OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(
							TEXT("command_manifest_variant_schema_invalid"), OutError, TEXT("command_manifest"));
						OutError.SubjectId = Contract.GetCommandId();
						OutError.PropertyPath = TEXT("/variants/") + Variant.VariantId + TEXT("/request_schema");
						return false;
					}
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("variant_id"), Variant.VariantId);
					Item->SetObjectField(TEXT("request_schema"), MoveTemp(RequestSchema));
					Item->SetStringField(TEXT("summary"), Variant.Purpose.Summary);
					Item->SetStringField(TEXT("parameter_guidance"), Variant.Purpose.ParameterGuidance);
					Item->SetStringField(TEXT("result_guidance"), Variant.Purpose.ResultGuidance);
					Item->SetStringField(TEXT("minimum_example"), Variant.Purpose.MinimumExample);
					Variants.Add(MakeShared<FJsonValueObject>(Item));
				}
				Object->SetArrayField(TEXT("variants"), MoveTemp(Variants));
			}
			Object->SetBoolField(TEXT("supports_operation_recovery"), SupportsOperationRequestRecovery(Contract));
			if (!Contract.GetModelResultFields().IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> ModelResultFields;
				ModelResultFields.Reserve(Contract.GetModelResultFields().Num());
				for (const FUeAgentModelResultFieldDeclaration& Field :
					Contract.GetModelResultFields())
				{
					ModelResultFields.Add(MakeShared<FJsonValueObject>(
						ModelResultFieldObject(Field)));
				}
				Object->SetArrayField(
					TEXT("model_result_fields"), MoveTemp(ModelResultFields));
			}
			Object->SetField(TEXT("min_wire_version"), IntegerValue(1));
			Object->SetField(TEXT("max_wire_version"), IntegerValue(1));
			Object->SetStringField(
				TEXT("status"),
				CommandLifecycle::LexToString(
					Contract.GetAvailability()));
			Object->SetStringField(
				TEXT("binding_hash"),
				Contract.GetContractHash());
			TArray<TSharedPtr<FJsonValue>> TutorialReferences;
			TutorialReferences.Reserve(
				Contract.GetTutorialReferences().Num());
			for (const FString& Reference :
				Contract.GetTutorialReferences())
			{
				TutorialReferences.Add(
					MakeShared<FJsonValueString>(Reference));
			}
			Object->SetArrayField(
				TEXT("tutorial_references"),
				TutorialReferences);
			return true;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutManifest.Reset();
		OutError.Reset();
		const FString CleanPluginVersion = Clean(PluginVersion);
		const FString CleanBuildId = Clean(BuildId);
		if (CleanPluginVersion.IsEmpty() || CleanBuildId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_manifest_source_invalid"),
				CleanPluginVersion.IsEmpty() ? TEXT("The active command manifest requires a non-empty plugin version.")
					: TEXT("The active command manifest requires a non-empty build identifier."), TEXT("command_manifest"));
			OutError.PropertyPath = CleanPluginVersion.IsEmpty() ? TEXT("/source/plugin_version") : TEXT("/source/build_id");
			OutError.ActualValue = MakeShared<FJsonValueString>(CleanPluginVersion.IsEmpty() ? PluginVersion : BuildId);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> Commands;
		Commands.Reserve(Registry->GetContracts().Num());
		for (const FUeAgentFrozenCommandContract& Contract :
			Registry->GetContracts())
		{
			TSharedPtr<FJsonObject> Command;
			if (!TryCommandObject(Contract, Command, OutError)) return false;
			Commands.Add(MakeShared<FJsonValueObject>(MoveTemp(Command)));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_manifest_directory_failed"),
				Directory.IsEmpty() ? TEXT("The manifest output path must include a directory.")
					: TEXT("The command manifest output directory could not be created."), TEXT("command_manifest"), ManifestPath);
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = FPaths::GetCleanFilename(ManifestPath);
			return false;
		}
		if (!FFileHelper::SaveArrayToFile(Utf8Bytes(Manifest), *ManifestPath))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("active_command_manifest_write_failed"),
				TEXT("The command manifest file could not be written."), TEXT("command_manifest"), ManifestPath);
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = FPaths::GetCleanFilename(ManifestPath);
			return false;
		}
		OutError.Reset();
		return true;
	}
}
