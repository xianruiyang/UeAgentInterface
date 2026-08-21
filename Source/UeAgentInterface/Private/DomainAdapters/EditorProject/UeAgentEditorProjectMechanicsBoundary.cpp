#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Dom/JsonObject.h"
#include "Misc/ConfigCacheIni.h"
#include "LegacyMechanics/UeAgentLegacyMechanics.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
	namespace
	{
		class FEditorProjectQueryMechanicsPort final
			: public IEditorProjectQueryMechanicsPort
		{
		public:
			bool Execute(
				const FString& Action,
				const FEditorProjectQueryRequest& Request,
				TSharedPtr<FJsonObject>& OutObservation,
				FString& OutError) const override
			{
				// The legacy read mechanics contract accepts an allocated result
				// object and may populate it before returning a typed validation
				// failure. Never forward a null shared pointer into those mechanics.
				OutObservation = MakeShared<FJsonObject>();
				OutError.Reset();
				const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				if (Request.Params.IsValid())
				{
					Params->Values = Request.Params->Values;
				}

				FString RequestedOutputFile;
				Params->TryGetStringField(TEXT("output_file"), RequestedOutputFile);
				Params->RemoveField(TEXT("output_file"));
				Params->RemoveField(TEXT("output_path"));
				Params->RemoveField(TEXT("output_directory"));
				Params->SetBoolField(TEXT("write_file"), false);
				Params->SetBoolField(TEXT("write_files"), false);

				FUeAgentRequestContext Context;
				Context.RequestId = Request.RequestId;
				Context.Command = TEXT("editor_project_query");
				Context.Params = Params;

				bool bOk = false;
				if (Action == TEXT("target.read"))
				{
					FString TargetRef;
					if (!Params->TryGetStringField(TEXT("target_ref"), TargetRef)
						|| TargetRef.IsEmpty())
					{
						OutError = TEXT("editor_project_target_read_target_ref_required");
						return false;
					}

					using namespace UE::UAI::Composition::SimpleAuthoring;
					FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers Resolvers;
					FUeAgentSimpleAuthoringRuntimeResolverError ResolverError;
					if (!TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
							Resolvers, ResolverError))
					{
						OutError = ResolverError.Code;
						if (!ResolverError.Detail.IsEmpty())
						{
							OutError += TEXT(": ") + ResolverError.Detail;
						}
						return false;
					}

					using namespace UE::UAI::DomainAdapters::EditorProjectSimpleEffect;
					FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
					ExecutionMutation::FExecutionMutationError TargetError;
					if (!Resolvers.PreStateTargetResolver->TryResolveTarget(
							TargetRef, Target, TargetError))
					{
						OutError = TargetError.GetCode();
						if (!TargetError.GetDetail().IsEmpty())
						{
							OutError += TEXT(": ") + TargetError.GetDetail();
						}
						return false;
					}

					OutObservation = MakeShared<FJsonObject>();
					OutObservation->SetStringField(
						TEXT("target_ref"), Target.GetCanonicalRef());
					OutObservation->SetStringField(
						TEXT("profile_id"), Target.GetProfileId());
					OutObservation->SetStringField(
						TEXT("target_revision"), Target.GetRevision());
					if (const FConfigFile* Config = Target.GetConfigDocument())
					{
						const TSharedPtr<FJsonObject> Entries =
							MakeShared<FJsonObject>();
						if (const FConfigSection* Section = Config->FindSection(
								Target.GetConfigSection()))
						{
							for (FConfigSection::TConstIterator It(*Section); It; ++It)
							{
								Entries->SetStringField(
									It.Key().ToString(), It.Value().GetSavedValue());
							}
						}
						OutObservation->SetObjectField(TEXT("entries"), Entries);
					}
					if (UObject* SettingsObject = Target.GetSettingsObject())
					{
						TArray<
							FUeAgentEditorProjectSettingsFieldObservation> Fields;
						ExecutionMutation::FExecutionMutationError FieldError;
						if (!Resolvers.PreStateTargetResolver->TryReadSettingsFields(
								*SettingsObject, Fields, FieldError))
						{
							OutError = FieldError.GetCode();
							if (!FieldError.GetDetail().IsEmpty())
							{
								OutError += TEXT(": ") + FieldError.GetDetail();
							}
							return false;
						}

						TArray<TSharedPtr<FJsonValue>> FieldValues;
						for (const FUeAgentEditorProjectSettingsFieldObservation& Field
							: Fields)
						{
							const TSharedPtr<FJsonObject> FieldObject =
								MakeShared<FJsonObject>();
							FieldObject->SetStringField(
								TEXT("field_ref"), Field.FieldRef);
							FieldObject->SetStringField(
								TEXT("field_revision"), Field.FieldRevision);
							FieldObject->SetStringField(
								TEXT("type_id"), Field.TypeId);
							if (Field.TypeId == TEXT("uai.value.boolean.v1"))
							{
								FieldObject->SetBoolField(
									TEXT("canonical_value"),
									Field.CanonicalValue.Equals(
										TEXT("True"), ESearchCase::IgnoreCase)
										|| Field.CanonicalValue == TEXT("1"));
							}
							else if (Field.TypeId == TEXT("uai.value.integer.v1"))
							{
								FieldObject->SetNumberField(
									TEXT("canonical_value"),
									FCString::Atoi64(*Field.CanonicalValue));
							}
							else
							{
								FieldObject->SetStringField(
									TEXT("canonical_value"), Field.CanonicalValue);
							}
							FieldValues.Add(MakeShared<FJsonValueObject>(FieldObject));
						}
						OutObservation->SetArrayField(TEXT("fields"), FieldValues);
					}
					bOk = true;
				}
				else if (Action == TEXT("device_profile.diff")) bOk = Mechanics.CmdDeviceProfileDiffJson(Context, OutObservation, OutError);
				else if (Action == TEXT("device_profile.export")) bOk = Mechanics.CmdDeviceProfileExportJson(Context, OutObservation, OutError);
				else if (Action == TEXT("device_profile.query")) bOk = Mechanics.CmdDeviceProfileQuery(Context, OutObservation, OutError);
				else if (Action == TEXT("device_profile.resolve_cvars")) bOk = Mechanics.CmdDeviceProfileResolveCVars(Context, OutObservation, OutError);
				else if (Action == TEXT("device_profile.validate")) bOk = Mechanics.CmdDeviceProfileValidateJson(Context, OutObservation, OutError);
				else if (Action == TEXT("platform_config.export")) bOk = Mechanics.CmdPlatformConfigExportJson(Context, OutObservation, OutError);
				else if (Action == TEXT("platform_config.validate")) bOk = Mechanics.CmdPlatformConfigValidateJson(Context, OutObservation, OutError);
				else if (Action == TEXT("platform_profile.validate_packaging")) bOk = Mechanics.CmdPlatformProfileValidatePackaging(Context, OutObservation, OutError);
				else if (Action == TEXT("project_settings.diff")) bOk = Mechanics.CmdProjectSettingsDiffPage(Context, OutObservation, OutError);
				else if (Action == TEXT("project_settings.index")) bOk = Mechanics.CmdProjectSettingsExportIndex(Context, OutObservation, OutError);
				else if (Action == TEXT("project_settings.page")) bOk = Mechanics.CmdProjectSettingsExportPage(Context, OutObservation, OutError);
				else if (Action == TEXT("project_settings.validate")) bOk = Mechanics.CmdProjectSettingsValidatePage(Context, OutObservation, OutError);
				else if (Action == TEXT("scalability.export")) bOk = Mechanics.CmdScalabilityExportJson(Context, OutObservation, OutError);
				else if (Action == TEXT("scalability.validate")) bOk = Mechanics.CmdScalabilityValidateJson(Context, OutObservation, OutError);
				else if (Action == TEXT("target_platform.query")) bOk = Mechanics.CmdTargetPlatformQuery(Context, OutObservation, OutError);
				else OutError = TEXT("editor_project_query_action_unknown");

				if (bOk && OutObservation.IsValid())
				{
					OutObservation->SetStringField(TEXT("editor_project_capability"), Action);
					OutObservation->SetBoolField(TEXT("server_file_written"), false);
					if (!RequestedOutputFile.IsEmpty())
					{
						OutObservation->SetStringField(
							TEXT("requested_output_file"), RequestedOutputFile);
					}
				}
				return bOk;
			}

		private:
			FUeAgentLegacyMechanics Mechanics;
		};
	}

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("editor_project.config_section.update"),
			TEXT("editor_project.device_profile.update"),
			TEXT("editor_project.localization_target.update"),
			TEXT("editor_project.platform_config.update"),
			TEXT("editor_project.project_settings.update"),
			TEXT("editor_project.scalability_group.update"),
			TEXT("editor_project.string_table.update"),
		};
		return Ids;
	}

	TArray<FString> GetRequiredAuthorityScopes(const FString& CommandId)
	{
		if (!GetCommandIds().Contains(CommandId))
		{
			return {};
		}
		TArray<FString> Scopes = {
			TEXT("uai:editor-project:write"),
		};
		Scopes.Add(
			CommandId == TEXT("editor_project.string_table.update")
				? TEXT("uai:asset:write")
				: TEXT("uai:project-config:write"));
		return Scopes;
	}

	bool IsEditorProjectQueryRoute(
		const FString& ProviderBinding,
		const FString& Action)
	{
		if (Action == TEXT("target.read"))
		{
			return ProviderBinding == TEXT("asset_project.read.config_section")
				|| ProviderBinding == TEXT("asset_project.read.config_resolved_value")
				|| ProviderBinding == TEXT("asset_project.read.project_settings")
				|| ProviderBinding == TEXT("asset_project.read.project_platform")
				|| ProviderBinding == TEXT("asset_project.read.project_validation");
		}
		if (ProviderBinding == TEXT("asset_project.read.config_section"))
		{
			return Action == TEXT("device_profile.export")
				|| Action == TEXT("device_profile.query")
				|| Action == TEXT("scalability.export");
		}
		if (ProviderBinding == TEXT("asset_project.read.config_resolved_value"))
		{
			return Action == TEXT("device_profile.resolve_cvars");
		}
		if (ProviderBinding == TEXT("asset_project.read.project_settings"))
		{
			return Action == TEXT("project_settings.index")
				|| Action == TEXT("project_settings.page");
		}
		if (ProviderBinding == TEXT("asset_project.read.project_platform"))
		{
			return Action == TEXT("platform_config.export")
				|| Action == TEXT("target_platform.query");
		}
		if (ProviderBinding == TEXT("asset_project.read.project_validation"))
		{
			return Action == TEXT("device_profile.diff")
				|| Action == TEXT("device_profile.validate")
				|| Action == TEXT("platform_config.validate")
				|| Action == TEXT("platform_profile.validate_packaging")
				|| Action == TEXT("project_settings.diff")
				|| Action == TEXT("project_settings.validate")
				|| Action == TEXT("scalability.validate");
		}
		return false;
	}

	TSharedRef<const IEditorProjectQueryMechanicsPort>
	CreateEditorProjectQueryMechanicsPort()
	{
		return MakeShared<FEditorProjectQueryMechanicsPort>();
	}
}
