#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "DomainAdapters/EditorProject/UeAgentEditorProjectQueryMechanics.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ConfigCacheIni.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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

				bool bOk = false;
				if (Action == TEXT("target.read"))
				{
					FString TargetRef;
					if (!Params->TryGetStringField(TEXT("target_ref"), TargetRef)
						|| TargetRef.IsEmpty())
					{
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_project_target_read_target_ref_required"),
							TEXT("Reading an editor project target requires a nonempty string target reference."), TEXT("runtime_factory"));
						const auto Actual = Params->Values.FindRef(TEXT("target_ref"));
						OutError.SourceKind = TEXT("command_input");
						OutError.PropertyPath = TEXT("/target_ref");
						OutError.ActualValue = Actual.IsValid() && Actual->Type == EJson::String
							? Actual : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UeAgentJsonDiagnostics::JsonValueTypeToString(Actual)));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty target reference"));
						OutError.ActualValuePresent = Actual.IsValid();
						OutError.ExpectedValuePresent = true;
						return false;
					}

					using namespace UE::UAI::Composition::SimpleAuthoring;
					FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers Resolvers;
					FUeAgentSimpleAuthoringRuntimeResolverError ResolverError;
					if (!TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
							Resolvers, ResolverError))
					{
						OutError = ResolverError;
						return false;
					}

					using namespace UE::UAI::DomainAdapters::EditorProjectSimpleEffect;
					FUeAgentEditorProjectSimpleEffectResolvedTarget Target;
					ExecutionMutation::FExecutionMutationError TargetError;
					if (!Resolvers.PreStateTargetResolver->TryResolveTarget(
							TargetRef, Target, TargetError))
					{
						OutError = TargetError;
						if (OutError.IsEmpty()) OutError = Target.GetValidationDiagnostic();
						if (OutError.IsEmpty())
						{
							OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_project_target_read_unresolved"),
								TEXT("The editor project target resolver did not resolve the requested target."), TEXT("query"));
							OutError.SourceKind = TEXT("native_result");
							OutError.PropertyPath = TEXT("/target_resolved");
							OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
							OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
							OutError.ActualValuePresent = true;
							OutError.ExpectedValuePresent = true;
						}
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
						return false;
					}

					OutObservation = MakeShared<FJsonObject>();
					OutObservation->SetStringField(
						TEXT("target_ref"), Target.GetCanonicalRef());
					OutObservation->SetStringField(
						TEXT("profile_id"), Target.GetProfileId());
					if (const FConfigFile* Config = Target.GetConfigDocument())
					{
						const TSharedPtr<FJsonObject> Entries =
							MakeShared<FJsonObject>();
						if (const FConfigSection* Section = Config->FindSection(
								Target.GetConfigSection()))
						{
							for (FConfigSection::TConstIterator It(*Section); It; ++It)
							{
								const FString Key = It.Key().ToString();
								const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(It.Value().GetSavedValue());
								const TSharedPtr<FJsonValue>* Existing = Entries->Values.Find(Key);
								if (!Existing)
								{
									Entries->SetField(Key, Value);
								}
								else
								{
									TArray<TSharedPtr<FJsonValue>> Values = (*Existing)->Type == EJson::Array
										? (*Existing)->AsArray() : TArray<TSharedPtr<FJsonValue>>{*Existing};
									Values.Add(Value);
									Entries->SetArrayField(Key, MoveTemp(Values));
								}
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
							OutError = FieldError;
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
				else bOk = FEditorProjectQueryMechanics::Execute(Action, Params, OutObservation, OutError);


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
