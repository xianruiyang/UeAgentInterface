#include "Misc/AutomationTest.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectTestSupport.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectTestTypes.h"

#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using namespace UE::UAI::Tests::EditorProjectSimpleEffect;

	class FOwnedEditorProjectState final
	{
	public:
		FOwnedEditorProjectState()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			TempRoot = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::ProjectSavedDir(),
					TEXT("UeAgentInterface"),
					TEXT("Tests"),
					TEXT("EditorProjectSimpleEffect"),
					Suffix));
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/EditorProject/") + Suffix));
			Settings =
				NewObject<
					UUeAgentEditorProjectSimpleEffectTestSettings>(
						Package,
						TEXT("OwnedSettings"),
						RF_Transactional);
			StringTable = NewObject<UStringTable>(
				Package,
				TEXT("OwnedStringTable"),
				RF_Public
					| RF_Standalone
					| RF_Transactional);

			ConfigSection.SetString(
				TEXT("UAI.Config"),
				TEXT("UAI.TestValue"),
				TEXT("before"));
			DeviceProfile.SetString(
				TEXT("Owned DeviceProfile"),
				TEXT("DeviceType"),
				TEXT("Android"));
			DeviceProfile.SetString(
				TEXT("Owned DeviceProfile"),
				TEXT("BaseProfileName"),
				TEXT("Base"));
			AddArrayValue(
				DeviceProfile,
				TEXT("Owned DeviceProfile"),
				TEXT("CVars"),
				TEXT("r.UAI.Test=0"));
			Localization.SetString(
				TEXT("CommonSettings"),
				TEXT("NativeCulture"),
				TEXT("fr"));
			AddArrayValue(
				Localization,
				TEXT("CommonSettings"),
				TEXT("CulturesToGenerate"),
				TEXT("fr"));
			VirtualLocalizationDirectories.Add(TEXT("fr"));
			PlatformConfig.SetString(
				TEXT("/Script/UAI.Platform"),
				TEXT("r.UAI.Platform"),
				TEXT("0"));
			Scalability.SetString(
				TEXT("ViewDistanceQuality@0"),
				TEXT("r.UAI.Quality"),
				TEXT("0"));

			if (Settings)
			{
				Settings->bTestEnabled = false;
				Settings->TestCount = 3;
				Settings->TestLabel = TEXT("before");
			}
			if (StringTable)
			{
				StringTable->GetMutableStringTable()->SetNamespace(
					TEXT("UAI.Before"));
				StringTable->GetMutableStringTable()->SetSourceString(
					TEXT("Existing"),
					TEXT("Before"));
				StringTable->GetMutableStringTable()->SetMetaData(
					TEXT("Existing"),
					TEXT("Context"),
					TEXT("Before"));
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
		}

		~FOwnedEditorProjectState()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		static void AddArrayValue(
			FConfigFile& Config,
			const FString& SectionName,
			const FString& Key,
			const FString& Value)
		{
			Config.AddToSection(
				*SectionName,
				FName(*Key),
				Value);
		}

		static FString ReadString(
			const FConfigFile& Config,
			const FString& Section,
			const FString& Key)
		{
			FString Value;
			Config.GetString(*Section, *Key, Value);
			return Value;
		}

		static TArray<FString> ReadArray(
			const FConfigFile& Config,
			const FString& SectionName,
			const FString& Key)
		{
			TArray<FString> Result;
			const FConfigSection* Section =
				Config.FindSection(SectionName);
			if (!Section)
			{
				return Result;
			}
			TArray<FConfigValue> Values;
			Section->MultiFind(FName(*Key), Values, true);
			for (const FConfigValue& Value : Values)
			{
				Result.Add(Value.GetValue());
			}
			Result.Sort();
			return Result;
		}

		FConfigFile* ConfigForProfile(const FString& Profile)
		{
			if (Profile == TEXT("config_section"))
			{
				return &ConfigSection;
			}
			if (Profile == TEXT("device_profile"))
			{
				return &DeviceProfile;
			}
			if (Profile == TEXT("localization_target"))
			{
				return &Localization;
			}
			if (Profile == TEXT("platform_config"))
			{
				return &PlatformConfig;
			}
			if (Profile == TEXT("scalability_group"))
			{
				return &Scalability;
			}
			return nullptr;
		}

		FString SectionForProfile(const FString& Profile) const
		{
			if (Profile == TEXT("config_section"))
			{
				return TEXT("UAI.Config");
			}
			if (Profile == TEXT("device_profile"))
			{
				return TEXT("Owned DeviceProfile");
			}
			if (Profile == TEXT("localization_target"))
			{
				return TEXT("CommonSettings");
			}
			if (Profile == TEXT("platform_config"))
			{
				return TEXT("/Script/UAI.Platform");
			}
			if (Profile == TEXT("scalability_group"))
			{
				return TEXT("ViewDistanceQuality@0");
			}
			return FString();
		}

		FString Fingerprint(const FString& Profile) const
		{
			if (Profile == TEXT("config_section"))
			{
				return ReadString(
					ConfigSection,
					TEXT("UAI.Config"),
					TEXT("UAI.TestValue"));
			}
			if (Profile == TEXT("project_settings"))
			{
				return Settings
					? FString::Printf(
						TEXT("%d|%d|%s"),
						Settings->bTestEnabled ? 1 : 0,
						Settings->TestCount,
						*Settings->TestLabel)
					: FString();
			}
			if (Profile == TEXT("device_profile"))
			{
				return ReadString(
						DeviceProfile,
						TEXT("Owned DeviceProfile"),
						TEXT("DeviceType"))
					+ TEXT("|")
					+ ReadString(
						DeviceProfile,
						TEXT("Owned DeviceProfile"),
						TEXT("BaseProfileName"))
					+ TEXT("|")
					+ FString::Join(
						ReadArray(
							DeviceProfile,
							TEXT("Owned DeviceProfile"),
							TEXT("CVars")),
						TEXT(","));
			}
			if (Profile == TEXT("localization_target"))
			{
				TArray<FString> Directories =
					VirtualLocalizationDirectories.Array();
				Directories.Sort();
				return ReadString(
						Localization,
						TEXT("CommonSettings"),
						TEXT("NativeCulture"))
					+ TEXT("|")
					+ FString::Join(
						ReadArray(
							Localization,
							TEXT("CommonSettings"),
							TEXT("CulturesToGenerate")),
						TEXT(","))
					+ TEXT("|")
					+ FString::Join(Directories, TEXT(","));
			}
			if (Profile == TEXT("platform_config"))
			{
				return ReadString(
					PlatformConfig,
					TEXT("/Script/UAI.Platform"),
					TEXT("r.UAI.Platform"));
			}
			if (Profile == TEXT("scalability_group"))
			{
				return ReadString(
					Scalability,
					TEXT("ViewDistanceQuality@0"),
					TEXT("r.UAI.Quality"));
			}
			if (Profile == TEXT("string_table") && StringTable)
			{
				TArray<FString> Rows;
				StringTable->GetStringTable()->EnumerateSourceStrings(
					[&](const FString& Key, const FString& Source)
					{
						TArray<FString> Metadata;
						StringTable->GetStringTable()->EnumerateMetaData(
							Key,
							[&](
								FName MetadataKey,
								const FString& MetadataValue)
							{
								Metadata.Add(
									MetadataKey.ToString()
										+ TEXT("=")
										+ MetadataValue);
								return true;
							});
						Metadata.Sort();
						Rows.Add(
							Key + TEXT("=") + Source
								+ TEXT("{")
								+ FString::Join(
									Metadata,
									TEXT(","))
								+ TEXT("}"));
						return true;
					});
				Rows.Sort();
				return StringTable->GetStringTable()->GetNamespace()
					+ TEXT("|")
					+ FString::Join(Rows, TEXT(","));
			}
			return FString();
		}

		bool IsIsolated() const
		{
			FString NormalizedRoot = TempRoot;
			FString ProjectConfig =
				FPaths::ConvertRelativePathToFull(
					FPaths::ProjectConfigDir());
			FString LocalizationRoot =
				FPaths::ConvertRelativePathToFull(
					FPaths::Combine(
						FPaths::ProjectContentDir(),
						TEXT("Localization")));
			FPaths::NormalizeDirectoryName(NormalizedRoot);
			FPaths::NormalizeDirectoryName(ProjectConfig);
			FPaths::NormalizeDirectoryName(LocalizationRoot);
			return !NormalizedRoot.StartsWith(
					ProjectConfig,
					ESearchCase::IgnoreCase)
				&& !NormalizedRoot.StartsWith(
					LocalizationRoot,
					ESearchCase::IgnoreCase)
				&& Package
				&& Package->GetName().StartsWith(
					TEXT("/Temp/UAI/EditorProject/"))
				&& Settings
				&& Settings->GetOutermost() == Package
				&& StringTable
				&& StringTable->GetOutermost() == Package;
		}

		FString TempRoot;
		UPackage* Package = nullptr;
		UUeAgentEditorProjectSimpleEffectTestSettings* Settings =
			nullptr;
		UStringTable* StringTable = nullptr;
		FConfigFile ConfigSection;
		FConfigFile DeviceProfile;
		FConfigFile Localization;
		FConfigFile PlatformConfig;
		FConfigFile Scalability;
		TSet<FString> VirtualLocalizationDirectories;
		TMap<FString, FString> Revisions;
	};
}


namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::DomainAdapters::
		EditorProjectSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	FString TargetRefForProfile(const FString& Profile)
	{
		return TEXT("uai://editor-project/owned/") + Profile;
	}

	class FOwnedTargetResolver final
		: public IUeAgentEditorProjectSimpleEffectTargetResolver
	{
	public:
		explicit FOwnedTargetResolver(
			FOwnedEditorProjectState& InOwner)
			: Owner(InOwner)
		{
			for (const FString& Profile : {
				TEXT("config_section"),
				TEXT("project_settings"),
				TEXT("device_profile"),
				TEXT("localization_target"),
				TEXT("platform_config"),
				TEXT("scalability_group"),
				TEXT("string_table"),
			})
			{
				Owner.Revisions.Add(
					TargetRefForProfile(Profile),
					TEXT("rev:target:1"));
			}
			Owner.Revisions.Add(
				TEXT("uai://editor-project/device-profile/base"),
				TEXT("rev:base-profile:1"));
		}

		bool TryResolveTarget(
			const FString& CanonicalRef,
			FUeAgentEditorProjectSimpleEffectResolvedTarget& OutTarget,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			FString Profile;
			for (const FString& Candidate : {
				TEXT("config_section"),
				TEXT("project_settings"),
				TEXT("device_profile"),
				TEXT("localization_target"),
				TEXT("platform_config"),
				TEXT("scalability_group"),
				TEXT("string_table"),
			})
			{
				if (CanonicalRef == TargetRefForProfile(Candidate))
				{
					Profile = Candidate;
					break;
				}
			}
			if (CanonicalRef
				== TEXT("uai://editor-project/device-profile/base"))
			{
				Profile = TEXT("device_profile");
			}
			const FString* Revision =
				Owner.Revisions.Find(CanonicalRef);
			if (Profile.IsEmpty() || !Revision)
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_target_unresolved"),
					CanonicalRef);
				return false;
			}
			OutTarget =
				FUeAgentEditorProjectSimpleEffectResolvedTarget(
					CanonicalRef,
					Profile,
					*Revision,
					Owner.ConfigForProfile(Profile),
					Owner.SectionForProfile(Profile),
					Profile == TEXT("project_settings")
						? Owner.Settings
						: nullptr,
					Profile == TEXT("string_table")
						? Owner.StringTable
						: nullptr,
					Profile == TEXT("localization_target")
						? &Owner.VirtualLocalizationDirectories
						: nullptr);
			return OutTarget.IsValid();
		}

		bool TryResolveSettingsField(
			UObject& SettingsObject,
			const FString& FieldRef,
			const FString& ExpectedRevision,
			FProperty*& OutProperty,
			FString& OutCurrentRevision,
			FExecutionMutationError& OutError) override
		{
			OutProperty = nullptr;
			OutCurrentRevision = TEXT("field:r1");
			if (FieldRef
					!= TEXT(
						"uai://editor-project-field/test-enabled")
				|| ExpectedRevision != OutCurrentRevision)
			{
				OutError = FExecutionMutationError(
					ExpectedRevision == OutCurrentRevision
						? TEXT(
							"editor_project_settings_field_unresolved")
						: TEXT(
							"editor_project_settings_field_revision_stale"),
					FieldRef);
				return false;
			}
			OutProperty = FindFProperty<FProperty>(
				SettingsObject.GetClass(),
				GET_MEMBER_NAME_CHECKED(
					UUeAgentEditorProjectSimpleEffectTestSettings,
					bTestEnabled));
			return OutProperty != nullptr;
		}

		bool TryReadSettingsFields(
			UObject& SettingsObject,
			TArray<FUeAgentEditorProjectSettingsFieldObservation>& OutFields,
			FExecutionMutationError& OutError) override
		{
			OutFields.Reset();
			OutError = FExecutionMutationError();
			OutFields.Add({
				TEXT("uai://editor-project-field/test-enabled"),
				TEXT("field:r1"),
				TEXT("uai.value.boolean.v1"),
				TEXT("true")});
			return true;
		}

		FOwnedEditorProjectState& Owner;
		int32 ResolveCalls = 0;
	};

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeNativeResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>();
	}

	bool TryCanonicalValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		OutValue.Reset();
		if (JsonValue->Type == EJson::String)
		{
			return FUeAgentCanonicalValue::TryString(
				JsonValue->AsString(),
				Resources,
				OutValue,
				OutError);
		}
		if (JsonValue->Type == EJson::Boolean)
		{
			return FUeAgentCanonicalValue::TryBoolean(
				JsonValue->AsBool(),
				Resources,
				OutValue,
				OutError);
		}
		if (JsonValue->Type == EJson::Number)
		{
			return FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(JsonValue->AsNumber()),
				Resources,
				OutValue,
				OutError);
		}
		if (JsonValue->Type == EJson::Null)
		{
			return FUeAgentCanonicalValue::TryNull(
				Resources,
				OutValue,
				OutError);
		}
		if (JsonValue->Type == EJson::Array)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const TSharedPtr<FJsonValue>& Item
				: JsonValue->AsArray())
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!TryCanonicalValue(
						Item,
						Resources,
						Canonical,
						OutError))
				{
					return false;
				}
				Items.Add(Canonical.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryArray(
				Items,
				Resources,
				OutValue,
				OutError);
		}
		if (JsonValue->Type == EJson::Object)
		{
			TArray<
				TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const TPair<
				FString,
				TSharedPtr<FJsonValue>>& Pair
				: JsonValue->AsObject()->Values)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!TryCanonicalValue(
						Pair.Value,
						Resources,
						Canonical,
						OutError))
				{
					return false;
				}
				Items.Emplace(
					Pair.Key,
					Canonical.ToSharedRef());
			}
			return FUeAgentCanonicalValue::TryObject(
				Items,
				Resources,
				OutValue,
				OutError);
		}
		return false;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:editor-project");
		Input.EditorInstanceId =
			TEXT("editor:editor-project-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision =
			TEXT("project:editor-project:r1");
		Input.bResolvedFromCurrent = false;
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	FUeAgentCanonicalSnapshotPtr MakeNativeSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const FString& Profile,
		const FString& TargetRef,
		const FString& TargetRevision,
		const FString& Fingerprint,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision SnapshotRevision(
			TEXT("editor-project-e2"),
			TEXT("project:editor-project:r1"),
			1);
		const TSharedPtr<FJsonObject> State =
			MakeShared<FJsonObject>();
		State->SetStringField(TEXT("target_ref"), TargetRef);
		State->SetStringField(
			TEXT("target_revision"),
			TargetRevision);
		State->SetStringField(TEXT("profile_id"), Profile);
		State->SetStringField(TEXT("fingerprint"), Fingerprint);
		FUeAgentCanonicalValuePtr Current;
		FUeAgentCanonicalValuePtr Default;
		FUeAgentStateSemanticError Error;
		if (!TryCanonicalValue(
				MakeShared<FJsonValueObject>(State),
				Resources,
				Current,
				Error)
			|| !FUeAgentCanonicalValue::TryObject(
				{},
				Resources,
				Default,
				Error))
		{
			return nullptr;
		}
		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("uai.editor_project:") + TargetRef,
				TEXT("editor_project.typed_update"),
				TEXT("project:editor-project"),
				TargetRef,
				Profile,
				TEXT("project:editor-project:r1")),
			FUeAgentCanonicalType(
				TEXT("editor_project_target_state"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			Current.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("editor-project:e2:default"),
				TEXT("owned target"),
				Default.ToSharedRef()),
			FUeAgentStateDetails(
				TargetRef,
				TEXT(""),
				{TEXT("EditorProjectSimpleEffect")},
				true,
				TEXT("available"),
				{}),
			SnapshotRevision);
		TArray<FUeAgentStateEntry> Entries{Entry};
		auto AppendDependency =
			[&](
				const FString& Ref,
				const FString& Revision,
				const FString& DependencyProfile,
				const FString& TypeId)
			{
				const TSharedPtr<FJsonObject> DependencyState =
					MakeShared<FJsonObject>();
				DependencyState->SetStringField(
					TEXT("target_ref"),
					Ref);
				DependencyState->SetStringField(
					TEXT("target_revision"),
					Revision);
				DependencyState->SetStringField(
					TEXT("profile_id"),
					DependencyProfile);
				DependencyState->SetStringField(
					TEXT("type_id"),
					TypeId);
				FUeAgentCanonicalValuePtr DependencyCurrent;
				if (!TryCanonicalValue(
						MakeShared<FJsonValueObject>(
							DependencyState),
						Resources,
						DependencyCurrent,
						Error))
				{
					return false;
				}
				Entries.Emplace(
					FUeAgentCanonicalIdentity(
						TEXT("uai.editor_project:") + Ref,
						TEXT("editor_project.typed_update"),
						TEXT("project:editor-project"),
						Ref,
						DependencyProfile,
						TEXT("project:editor-project:r1")),
					FUeAgentCanonicalType(
						TypeId,
						EUeAgentValueKind::Object,
						false,
						TEXT("object")),
					DependencyCurrent.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::Schema,
						TEXT("editor-project:e2:dependency"),
						TEXT("owned dependency"),
						Default.ToSharedRef()),
					FUeAgentStateDetails(
						Ref,
						TEXT(""),
						{TEXT("EditorProjectSimpleEffect")},
						true,
						TEXT("available"),
						{}),
					SnapshotRevision);
				return true;
			};
		if (!AppendDependency(
				TEXT("uai://editor-project/device-profile/base"),
				TEXT("rev:base-profile:1"),
				TEXT("device_profile"),
				TEXT("editor_project_device_profile_state"))
			|| !AppendDependency(
				TEXT("uai://editor-project-field/test-enabled"),
				TEXT("field:r1"),
				TEXT("project_settings_field"),
				TEXT("uai.value.boolean.v1")))
		{
			return nullptr;
		}
		FUeAgentCanonicalSnapshotPtr Snapshot;
		if (!Test->TestTrue(
				TEXT("create EditorProject owned snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:editor-project:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					SnapshotRevision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-editor-project-read-port"),
						TEXT("editor-project:e2:before")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			return nullptr;
		}
		return Snapshot;
	}

	bool BuildNativePlan(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>& Resources,
		FPlanningPreflightPlanPtr& OutPlan,
		FExecutionMutationError& OutError)
	{
		FString InputJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&InputJson);
		if (!FJsonSerializer::Serialize(
				Input.ToSharedRef(),
				Writer))
		{
			return false;
		}
		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		if (!FUeAgentContractSchema::TryCompile(
				Descriptor.GetInputSchema(),
				Compiled,
				SchemaError)
			|| !Compiled.IsValid())
		{
			return false;
		}
		TOptional<FUeAgentContractValue> CanonicalPayload;
		if (!Compiled->TryValidateAndCanonicalize(
				FUeAgentContractValue(InputJson),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				CanonicalPayload,
				SchemaError)
			|| !CanonicalPayload.IsSet())
		{
			return false;
		}
		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(Descriptor.GetIdentity());
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildEditorProjectSimpleEffectChangeSetDraft(
				FUeAgentOperationRequest(
					Descriptor.GetIdentity(),
					TEXT("request:editor-project-e2"),
					TEXT("idempotency:editor-project-e2"),
					CanonicalPayload.GetValue()),
				*Context,
				Snapshot,
				*Resources,
				Draft,
				OutError)
			|| !Draft.IsSet()
			|| !Draft->IsValid())
		{
			return false;
		}
		FPlanningResult PlanningResult;
		FPlanningPreflightError PlanningError;
		if (!TryPlanChanges(
				FPlanningRequest(
					Draft.GetValue(),
					FUeAgentCallerContext(
						TEXT("agent:editor-project-e2"),
						{
							TEXT(
								"uai:editor-project:write"),
							TEXT(
								"uai:project-config:write"),
							TEXT("uai:asset:write"),
						},
						TEXT(
							"correlation:editor-project-e2")),
					false),
				*Resources,
				PlanningResult,
				PlanningError)
			|| !PlanningResult.IsReady()
			|| !PlanningResult.GetPlan().IsValid())
		{
			return false;
		}
		OutPlan = PlanningResult.GetPlan();
		return true;
	}

	struct FNativeRequestOwner
	{
		FNativeRequestOwner(
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				InResources,
			FPlanningPreflightPlanRef InPlan,
			FUeAgentExecutionContextRefOwner InContext)
			: Resources(MoveTemp(InResources))
			, Cancellation([this]() { return Now; })
			, Plan(MoveTemp(InPlan))
			, Context(MoveTemp(InContext))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:editor-project"),
				TEXT("owner:editor-project"),
				Plan,
				Context,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					60.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = FPlatformTime::Seconds();
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner Context;
	};

	FUeAgentGenericEffectBinding MakeBinding(
		const FUeAgentOperationDescriptor& Descriptor,
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentGenericEffectBinding(
			FUeAgentCandidateBinding(
				TEXT("binding:")
					+ Descriptor.GetCapabilityId(),
				FUeAgentCandidateSummary(
					Identity,
					Descriptor.GetIdentity(),
					TEXT("editor_project"),
					Descriptor.GetCapabilityId(),
					TEXT("owned editor project target"),
					{TEXT("Editor"), TEXT("Project")}),
				FUeAgentCatalogRevision(
					TEXT("editor-project-e2"),
					TEXT("project:editor-project:r1"),
					1),
				TEXT("uai://editor-project/owned"),
				TEXT("editor-project-read"),
				{EUeAgentCandidateOperation::Update},
				TEXT(
					"fixture:editor-project-e2:binding")),
			TEXT("UAI.EditorProject:CanonicalObject"),
			FUeAgentCanonicalType(
				TEXT("editor_project_canonical_object"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT("fixture:editor-project-e2:type-binding"));
	}

	bool AppliedOracle(
		const FOwnedEditorProjectState& Owner,
		const FString& Profile)
	{
		if (Profile == TEXT("config_section"))
		{
			return Owner.Fingerprint(Profile) == TEXT("after");
		}
		if (Profile == TEXT("project_settings"))
		{
			return Owner.Settings
				&& Owner.Settings->bTestEnabled;
		}
		if (Profile == TEXT("device_profile"))
		{
			const FString Fingerprint =
				Owner.Fingerprint(Profile);
			return Fingerprint.Contains(TEXT("Windows"))
				&& Fingerprint.Contains(TEXT("r.UAI.Test=1"));
		}
		if (Profile == TEXT("localization_target"))
		{
			const FString Fingerprint =
				Owner.Fingerprint(Profile);
			return Fingerprint.Contains(TEXT("en"))
				&& Fingerprint.Contains(TEXT("zh-Hans"));
		}
		if (Profile == TEXT("platform_config"))
		{
			return Owner.Fingerprint(Profile) == TEXT("1");
		}
		if (Profile == TEXT("scalability_group"))
		{
			return Owner.Fingerprint(Profile) == TEXT("2");
		}
		if (Profile == TEXT("string_table"))
		{
			const FString Fingerprint =
				Owner.Fingerprint(Profile);
			return Fingerprint.Contains(TEXT("UAI.Test"))
				&& Fingerprint.Contains(
					TEXT("Greeting=Hello"));
		}
		return false;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditorProjectSimpleEffectOwnedStateTest,
	"UeAgentInterface.DomainAdapters.EditorProjectSimpleEffect.E2."
		"OwnedStateReadbackRollback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentEditorProjectSimpleEffectOwnedStateTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	FOwnedEditorProjectState Owner;
	if (!TestTrue(
			TEXT("owned EditorProject fixture is isolated"),
			Owner.IsIsolated()))
	{
		return false;
	}
	TestFalse(
		TEXT("owned fixture does not create a real config path"),
		FPaths::FileExists(Owner.TempRoot));
	TestFalse(
		TEXT("owned settings object is not a CDO"),
		Owner.Settings && Owner.Settings->HasAnyFlags(RF_ClassDefaultObject));
	TestFalse(
		TEXT("owned package begins clean"),
		Owner.Package && Owner.Package->IsDirty());

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create EditorProject production registry"),
			TryCreateEditorProjectSimpleEffectDescriptorRegistry(
				Registry,
				RegistryError))
		|| !Registry.IsValid())
	{
		AddError(
			TEXT("editor_project_registry_failed:")
				+ RegistryError);
		return false;
	}
	const TSharedRef<
		FOwnedTargetResolver,
		ESPMode::ThreadSafe> Resolver =
			MakeShared<
				FOwnedTargetResolver,
				ESPMode::ThreadSafe>(Owner);

	for (const TSharedPtr<FJsonValue>& ScenarioValue
		: Fixture->GetArrayField(TEXT("e2_scenarios")))
	{
		const TSharedPtr<FJsonObject> Scenario =
			ScenarioValue->AsObject();
		const FString DescriptorId =
			Scenario->GetStringField(TEXT("descriptor_id"));
		const FUeAgentOperationDescriptor* Descriptor =
			Registry->FindByDescriptorId(DescriptorId);
		if (!TestNotNull(
				*(DescriptorId + TEXT(" descriptor exists")),
				Descriptor))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Row;
		for (const TSharedPtr<FJsonValue>& DescriptorValue
			: Fixture->GetArrayField(TEXT("descriptors")))
		{
			if (DescriptorValue->AsObject()->GetStringField(
					TEXT("descriptor_id"))
				== DescriptorId)
			{
				Row = DescriptorValue->AsObject();
				break;
			}
		}
		const FString Profile =
			Row->GetStringField(TEXT("profile_id"));
		const FString TargetRef =
			TargetRefForProfile(Profile);
		TSharedPtr<FJsonObject> Input =
			BuildValidInput(Fixture, Row);
		Input->SetStringField(TEXT("target_ref"), TargetRef);
		const FString Before = Owner.Fingerprint(Profile);
		const int32 PreEditCountBefore =
			Owner.Settings
				? Owner.Settings->PreEditChangeCount
				: 0;
		const int32 PostEditCountBefore =
			Owner.Settings
				? Owner.Settings->PostEditChangeCount
				: 0;

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
				MakeNativeResources();
		const FUeAgentCanonicalSnapshotPtr Snapshot =
			MakeNativeSnapshot(
				this,
				*Descriptor,
				Profile,
				TargetRef,
				TEXT("rev:target:1"),
				Before,
				*Resources);
		FPlanningPreflightPlanPtr Plan;
		FExecutionMutationError Error;
		if (!TestTrue(
				*(DescriptorId + TEXT(" builds owned plan")),
				BuildNativePlan(
					this,
					*Descriptor,
					Input,
					Snapshot.ToSharedRef(),
					Resources,
					Plan,
					Error))
			|| !Plan.IsValid())
		{
			continue;
		}
		TSharedPtr<IUeAgentEditorProjectSimpleEffectNativeHook>
			Hook;
		if (!TestTrue(
				*(DescriptorId + TEXT(" creates native hook")),
				FUeAgentEditorProjectSimpleEffectNativeHookFactory::
					TryCreate(Resolver, Hook, Error))
			|| !Hook.IsValid())
		{
			continue;
		}
		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(Descriptor->GetIdentity());
		FNativeRequestOwner RequestOwner(
			Resources,
			Plan.ToSharedRef(),
			Context);
		FExecutionMutationRequest Request =
			RequestOwner.MakeRequest();
		TArray<FUeAgentGenericEffectBinding> Bindings;
		for (const FPlannedChange& Change
			: Plan->GetChangeSet().GetChanges())
		{
			Bindings.Add(
				MakeBinding(*Descriptor, Change.GetIdentity()));
		}
		FUeAgentGenericEffectApplyResult Applied;
		const bool bApplied = Hook->TryApply(
			Request,
			FLockFenceLease(
				TEXT("owner:editor-project-e2"),
				{TargetRef},
				1),
			Bindings,
			*Resources,
			Applied,
			Error);
		TestTrue(*(DescriptorId + TEXT(" applies")), bApplied);
		TestTrue(
			*(DescriptorId + TEXT(" reports mutation")),
			Applied.DidMutationStart());
		if (!bApplied)
		{
			AddError(DescriptorId + TEXT(":") + Error.GetCode());
			continue;
		}
		TestTrue(
			*(DescriptorId + TEXT(" independent readback matches")),
			AppliedOracle(Owner, Profile));
		TestTrue(
			*(DescriptorId + TEXT(" changes fingerprint")),
			Owner.Fingerprint(Profile) != Before);
		const int32 PreEditCountAfterApply =
			Owner.Settings
				? Owner.Settings->PreEditChangeCount
				: 0;
		const int32 PostEditCountAfterApply =
			Owner.Settings
				? Owner.Settings->PostEditChangeCount
				: 0;
		if (Profile == TEXT("project_settings"))
		{
			TestTrue(
				*(DescriptorId
					+ TEXT(" emits PreEditChange on apply")),
				PreEditCountAfterApply > PreEditCountBefore);
			TestTrue(
				*(DescriptorId
					+ TEXT(" emits PostEditChangeProperty on apply")),
				PostEditCountAfterApply > PostEditCountBefore);
		}

		bool bRollbackConfirmed = false;
		TestTrue(
			*(DescriptorId + TEXT(" rolls back")),
			Hook->TryRollback(
				Request,
				Applied,
				*Resources,
				bRollbackConfirmed,
				Error));
		TestTrue(
			*(DescriptorId + TEXT(" confirms rollback")),
			bRollbackConfirmed);
		TestEqual(
			*(DescriptorId
				+ TEXT(" restores independent fingerprint")),
			Owner.Fingerprint(Profile),
			Before);
		TestFalse(
			*(DescriptorId + TEXT(" restores package dirty state")),
			Owner.Package->IsDirty());
		if (Profile == TEXT("project_settings"))
		{
			TestTrue(
				*(DescriptorId
					+ TEXT(" rollback preserves notification evidence")),
				Owner.Settings->PreEditChangeCount
					>= PreEditCountAfterApply
					&& Owner.Settings->PostEditChangeCount
						>= PostEditCountAfterApply);
		}
	}

	const FString StaleProfile = TEXT("config_section");
	const FString StaleTarget =
		TargetRefForProfile(StaleProfile);
	const FUeAgentOperationDescriptor* StaleDescriptor =
		Registry->FindByDescriptorId(
			TEXT(
				"uai.operation.editor_project."
				"config_section.update"));
	TSharedPtr<FJsonObject> StaleRow;
	for (const TSharedPtr<FJsonValue>& DescriptorValue
		: Fixture->GetArrayField(TEXT("descriptors")))
	{
		if (DescriptorValue->AsObject()->GetStringField(
				TEXT("descriptor_id"))
			== TEXT(
				"uai.operation.editor_project."
				"config_section.update"))
		{
			StaleRow = DescriptorValue->AsObject();
			break;
		}
	}
	const FString BeforeStale =
		Owner.Fingerprint(StaleProfile);
	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> StaleResources =
			MakeNativeResources();
	const FUeAgentCanonicalSnapshotPtr StaleSnapshot =
		MakeNativeSnapshot(
			this,
			*StaleDescriptor,
			StaleProfile,
			StaleTarget,
			TEXT("rev:target:1"),
			BeforeStale,
			*StaleResources);
	TSharedPtr<FJsonObject> StaleInput =
		BuildValidInput(Fixture, StaleRow);
	StaleInput->SetStringField(TEXT("target_ref"), StaleTarget);
	FPlanningPreflightPlanPtr StalePlan;
	FExecutionMutationError StaleError;
	TestTrue(
		TEXT("stale scenario builds from original revision"),
		BuildNativePlan(
			this,
			*StaleDescriptor,
			StaleInput,
			StaleSnapshot.ToSharedRef(),
			StaleResources,
			StalePlan,
			StaleError));
	Owner.Revisions[StaleTarget] = TEXT("rev:target:2");
	TSharedPtr<IUeAgentEditorProjectSimpleEffectNativeHook>
		StaleHook;
	TestTrue(
		TEXT("stale scenario creates hook"),
		FUeAgentEditorProjectSimpleEffectNativeHookFactory::
			TryCreate(Resolver, StaleHook, StaleError));
	const FUeAgentExecutionContextRefOwner StaleContext =
		MakeExecutionContext(StaleDescriptor->GetIdentity());
	FNativeRequestOwner StaleRequestOwner(
		StaleResources,
		StalePlan.ToSharedRef(),
		StaleContext);
	FExecutionMutationRequest StaleRequest =
		StaleRequestOwner.MakeRequest();
	TArray<FUeAgentGenericEffectBinding> StaleBindings;
	for (const FPlannedChange& Change
		: StalePlan->GetChangeSet().GetChanges())
	{
		StaleBindings.Add(
			MakeBinding(*StaleDescriptor, Change.GetIdentity()));
	}
	FUeAgentGenericEffectApplyResult StaleApplied;
	TestFalse(
		TEXT("revision drift fails before mutation"),
		StaleHook->TryApply(
			StaleRequest,
			FLockFenceLease(
				TEXT("owner:editor-project-e2-stale"),
				{StaleTarget},
				1),
			StaleBindings,
			*StaleResources,
			StaleApplied,
			StaleError));
	TestEqual(
		TEXT("revision drift error is stable"),
		StaleError.GetCode(),
		FString(TEXT("editor_project_target_revision_stale")));
	TestFalse(
		TEXT("revision drift reports no mutation"),
		StaleApplied.DidMutationStart());
	TestEqual(
		TEXT("revision drift preserves target fingerprint"),
		Owner.Fingerprint(StaleProfile),
		BeforeStale);

	return true;
}
