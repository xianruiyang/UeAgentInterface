#pragma once

#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringApplication.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectTestTypes.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::Tests::SimpleAuthoring::EditorProject
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace
		UE::UAI::Composition::SimpleAuthoring::EditorProject;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::
		EditorProjectSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	inline FString TargetRefForProfile(const FString& ProfileId)
	{
		return TEXT("uai://editor-project/sa008/") + ProfileId;
	}

	inline FString BaseDeviceProfileRef()
	{
		return TEXT("uai://editor-project/sa008/device-profile/base");
	}

	class FOwnedEditorProjectProductionState final
	{
	public:
		FOwnedEditorProjectProductionState()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			TempRoot = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::ProjectSavedDir(),
					TEXT("UeAgentInterface"),
					TEXT("SA008"),
					TEXT("EditorProjectE3"),
					Suffix));
			IFileManager::Get().MakeDirectory(*TempRoot, true);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/SA008/EditorProject/")
					+ Suffix));
			Settings = NewObject<
				UUeAgentEditorProjectSimpleEffectTestSettings>(
					Package,
					TEXT("OwnedSettings"),
					RF_Transactional);
			StringTable = NewObject<UStringTable>(
				Package,
				TEXT("OwnedStringTable"),
				RF_Public | RF_Standalone | RF_Transactional);

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
			DeviceProfile.AddToSection(
				TEXT("Owned DeviceProfile"),
				FName(TEXT("CVars")),
				TEXT("r.UAI.Test=0"));
			DeviceProfile.SetString(
				TEXT("Base DeviceProfile"),
				TEXT("DeviceType"),
				TEXT("Windows"));
			Localization.SetString(
				TEXT("CommonSettings"),
				TEXT("NativeCulture"),
				TEXT("fr"));
			Localization.AddToSection(
				TEXT("CommonSettings"),
				FName(TEXT("CulturesToGenerate")),
				TEXT("fr"));
			LocalizationDirectories.Add(TEXT("fr"));
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
				StringTable->GetMutableStringTable()
					->SetNamespace(TEXT("UAI.Before"));
				StringTable->GetMutableStringTable()
					->SetSourceString(TEXT("Existing"), TEXT("Before"));
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}

			for (const FString& ProfileId : ProfileIds())
			{
				Revisions.Add(
					TargetRefForProfile(ProfileId),
					TEXT("rev:target:1"));
			}
			Revisions.Add(
				BaseDeviceProfileRef(),
				TEXT("rev:base-profile:1"));
		}

		~FOwnedEditorProjectProductionState()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
			IFileManager::Get().DeleteDirectory(
				*TempRoot,
				false,
				true);
		}

		static TArray<FString> ProfileIds()
		{
			return {
				TEXT("config_section"),
				TEXT("project_settings"),
				TEXT("device_profile"),
				TEXT("localization_target"),
				TEXT("platform_config"),
				TEXT("scalability_group"),
				TEXT("string_table"),
			};
		}

		FConfigFile* ConfigForProfile(const FString& ProfileId)
		{
			if (ProfileId == TEXT("config_section"))
			{
				return &ConfigSection;
			}
			if (ProfileId == TEXT("device_profile"))
			{
				return &DeviceProfile;
			}
			if (ProfileId == TEXT("localization_target"))
			{
				return &Localization;
			}
			if (ProfileId == TEXT("platform_config"))
			{
				return &PlatformConfig;
			}
			if (ProfileId == TEXT("scalability_group"))
			{
				return &Scalability;
			}
			return nullptr;
		}

		FString SectionForProfile(const FString& ProfileId) const
		{
			if (ProfileId == TEXT("config_section"))
			{
				return TEXT("UAI.Config");
			}
			if (ProfileId == TEXT("device_profile"))
			{
				return TEXT("Owned DeviceProfile");
			}
			if (ProfileId == TEXT("localization_target"))
			{
				return TEXT("CommonSettings");
			}
			if (ProfileId == TEXT("platform_config"))
			{
				return TEXT("/Script/UAI.Platform");
			}
			if (ProfileId == TEXT("scalability_group"))
			{
				return TEXT("ViewDistanceQuality@0");
			}
			return FString();
		}

		FString DurableLocationForProfile(
			const FString& ProfileId) const
		{
			if (ProfileId == TEXT("string_table"))
			{
				return StringTable
					? StringTable->GetOutermost()->GetName()
					: FString();
			}
			return FPaths::ConvertRelativePathToFull(
				FPaths::Combine(TempRoot, ProfileId + TEXT(".ini")));
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
			const FString& Section,
			const FString& Key)
		{
			TArray<FString> Result;
			const FConfigSection* Found = Config.FindSection(Section);
			if (!Found)
			{
				return Result;
			}
			TArray<FConfigValue> Values;
			Found->MultiFind(FName(*Key), Values, true);
			for (const FConfigValue& Value : Values)
			{
				Result.Add(Value.GetValue());
			}
			Result.Sort();
			return Result;
		}

		bool IsApplied(const FString& ProfileId) const
		{
			if (ProfileId == TEXT("config_section"))
			{
				return ReadString(
					ConfigSection,
					TEXT("UAI.Config"),
					TEXT("UAI.TestValue")) == TEXT("after");
			}
			if (ProfileId == TEXT("project_settings"))
			{
				return Settings && Settings->bTestEnabled;
			}
			if (ProfileId == TEXT("device_profile"))
			{
				return ReadString(
						DeviceProfile,
						TEXT("Owned DeviceProfile"),
						TEXT("DeviceType")) == TEXT("Windows")
					&& ReadString(
						DeviceProfile,
						TEXT("Owned DeviceProfile"),
						TEXT("BaseProfileName"))
						== BaseDeviceProfileRef()
					&& ReadArray(
						DeviceProfile,
						TEXT("Owned DeviceProfile"),
						TEXT("CVars"))
						== TArray<FString>{TEXT("r.UAI.Test=1")};
			}
			if (ProfileId == TEXT("localization_target"))
			{
				const bool bDirectoriesApplied =
					LocalizationDirectories.Num() == 2
					&& LocalizationDirectories.Contains(TEXT("en"))
					&& LocalizationDirectories.Contains(TEXT("zh-Hans"));
				return ReadString(
						Localization,
						TEXT("CommonSettings"),
						TEXT("NativeCulture")) == TEXT("en")
					&& ReadArray(
						Localization,
						TEXT("CommonSettings"),
						TEXT("CulturesToGenerate"))
						== TArray<FString>{TEXT("en"), TEXT("zh-Hans")}
					&& bDirectoriesApplied;
			}
			if (ProfileId == TEXT("platform_config"))
			{
				return ReadString(
					PlatformConfig,
					TEXT("/Script/UAI.Platform"),
					TEXT("r.UAI.Platform")) == TEXT("1");
			}
			if (ProfileId == TEXT("scalability_group"))
			{
				return ReadString(
					Scalability,
					TEXT("ViewDistanceQuality@0"),
					TEXT("r.UAI.Quality")) == TEXT("2");
			}
			if (ProfileId == TEXT("string_table") && StringTable)
			{
				bool bFound = false;
				StringTable->GetStringTable()->EnumerateSourceStrings(
					[&bFound](
						const FString& Key,
						const FString& Source)
					{
						bFound = bFound
							|| (Key == TEXT("Greeting")
								&& Source == TEXT("Hello"));
						return true;
					});
				return StringTable->GetStringTable()->GetNamespace()
						== TEXT("UAI.After")
					&& bFound;
			}
			return false;
		}

		bool IsValid() const
		{
			return Package
				&& Settings
				&& StringTable
				&& Settings->GetOutermost() == Package
				&& StringTable->GetOutermost() == Package
				&& TempRoot.StartsWith(
					FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir()),
					ESearchCase::IgnoreCase);
		}

		FString TempRoot;
		UPackage* Package = nullptr;
		UUeAgentEditorProjectSimpleEffectTestSettings* Settings = nullptr;
		UStringTable* StringTable = nullptr;
		FConfigFile ConfigSection;
		FConfigFile DeviceProfile;
		FConfigFile Localization;
		FConfigFile PlatformConfig;
		FConfigFile Scalability;
		TSet<FString> LocalizationDirectories;
		TMap<FString, FString> Revisions;
	};

	class FOwnedEditorProjectTargetResolver final
		: public IUeAgentEditorProjectSimpleEffectTargetResolver
	{
	public:
		explicit FOwnedEditorProjectTargetResolver(
			FOwnedEditorProjectProductionState& InOwner,
			FString InRevisionOverride = FString())
			: Owner(InOwner)
			, RevisionOverride(MoveTemp(InRevisionOverride))
		{
		}

		bool TryResolveTarget(
			const FString& CanonicalRef,
			FUeAgentEditorProjectSimpleEffectResolvedTarget& OutTarget,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			FString ProfileId;
			FString Section;
			for (const FString& Candidate
				: FOwnedEditorProjectProductionState::ProfileIds())
			{
				if (CanonicalRef == TargetRefForProfile(Candidate))
				{
					ProfileId = Candidate;
					Section = Owner.SectionForProfile(Candidate);
					break;
				}
			}
			if (CanonicalRef == BaseDeviceProfileRef())
			{
				ProfileId = TEXT("device_profile");
				Section = TEXT("Base DeviceProfile");
			}
			const FString* Revision = Owner.Revisions.Find(CanonicalRef);
			if (ProfileId.IsEmpty() || !Revision)
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_target_unresolved"),
					CanonicalRef);
				return false;
			}
			OutTarget = FUeAgentEditorProjectSimpleEffectResolvedTarget(
				CanonicalRef,
				ProfileId,
				RevisionOverride.IsEmpty()
					? *Revision
					: RevisionOverride,
				Owner.ConfigForProfile(ProfileId),
				Section,
				ProfileId == TEXT("project_settings")
					? Owner.Settings
					: nullptr,
				ProfileId == TEXT("string_table")
					? Owner.StringTable
					: nullptr,
				ProfileId == TEXT("localization_target")
					? &Owner.LocalizationDirectories
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
			if (ExpectedRevision != OutCurrentRevision
				|| &SettingsObject != Owner.Settings)
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_settings_field_revision_stale"),
					FieldRef);
				return false;
			}
			if (FieldRef
				== TEXT("uai://editor-project-field/test-enabled"))
			{
				OutProperty = FindFProperty<FProperty>(
					SettingsObject.GetClass(),
					GET_MEMBER_NAME_CHECKED(
						UUeAgentEditorProjectSimpleEffectTestSettings,
						bTestEnabled));
			}
			if (!OutProperty)
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_settings_field_unresolved"),
					FieldRef);
				return false;
			}
			return true;
		}

		FOwnedEditorProjectProductionState& Owner;
		FString RevisionOverride;
		int32 ResolveCalls = 0;
	};

	class FReloadedEditorProjectConfigTargetResolver final
		: public IUeAgentEditorProjectSimpleEffectTargetResolver
	{
	public:
		explicit FReloadedEditorProjectConfigTargetResolver(
			FOwnedEditorProjectProductionState& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryResolveTarget(
			const FString& CanonicalRef,
			FUeAgentEditorProjectSimpleEffectResolvedTarget& OutTarget,
			FExecutionMutationError& OutError) override
		{
			++ReloadCount;
			const FString ProfileId = TEXT("config_section");
			const FString ExpectedRef = TargetRefForProfile(ProfileId);
			const FString Location = Owner.DurableLocationForProfile(ProfileId);
			const FString* Revision = Owner.Revisions.Find(ExpectedRef);
			PersistedConfig.Empty();
			if (CanonicalRef != ExpectedRef
				|| !Revision
				|| !IFileManager::Get().FileExists(*Location))
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_e4_config_reload_failed"),
					CanonicalRef);
				return false;
			}
			PersistedConfig.Read(Location);
			if (!PersistedConfig.FindSection(
					Owner.SectionForProfile(ProfileId)))
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_e4_config_reload_failed"),
					CanonicalRef);
				return false;
			}
			OutTarget = FUeAgentEditorProjectSimpleEffectResolvedTarget(
				CanonicalRef,
				ProfileId,
				*Revision,
				&PersistedConfig,
				Owner.SectionForProfile(ProfileId),
				nullptr,
				nullptr,
				nullptr);
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
			OutCurrentRevision.Reset();
			OutError = FExecutionMutationError(
				TEXT("editor_project_e4_settings_field_not_supported"),
				FieldRef);
			return false;
		}

		FString ReadPersistedValue() const
		{
			return FOwnedEditorProjectProductionState::ReadString(
				PersistedConfig,
				TEXT("UAI.Config"),
				TEXT("UAI.TestValue"));
		}

		int32 ReloadCount = 0;

	private:
		FOwnedEditorProjectProductionState& Owner;
		FConfigFile PersistedConfig;
	};

	class FOwnedEditorProjectDurableBindingResolver final
		: public IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver
	{
	public:
		explicit FOwnedEditorProjectDurableBindingResolver(
			FOwnedEditorProjectProductionState& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryResolveDurableBinding(
			const FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
			FUeAgentEditorProjectSimpleAuthoringDurableBinding& OutBinding,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			const FString Location =
				Owner.DurableLocationForProfile(Target.GetProfileId());
			if (Location.IsEmpty())
			{
				OutError = FExecutionMutationError(
					TEXT("editor_project_durable_target_unresolved"),
					Target.GetCanonicalRef());
				return false;
			}
			OutBinding = FUeAgentEditorProjectSimpleAuthoringDurableBinding(
				Target.GetProfileId() == TEXT("string_table")
					? EDurableArtifactKind::Package
					: EDurableArtifactKind::ProjectConfig,
				Location);
			return OutBinding.IsValid();
		}

		FOwnedEditorProjectProductionState& Owner;
		int32 ResolveCalls = 0;
	};

	class FRecordingEditorProjectDurableService final
		: public IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		bool TryPrepare(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			++PrepareCount;
			if (!Request.IsValid() || Request.GetTargets().Num() != 1)
			{
				OutError = FAssetDurableError(
					TEXT("sa008_editor_project_durable_target_invalid"));
				return false;
			}
			const FAssetDurableTarget& Target = Request.GetTargets()[0];
			Kinds.Add(Target.GetArtifactKind());
			Locations.Add(Target.GetDurableLocation());
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa008-editor-project:")
					+ FString::FromInt(PrepareCount),
				Target.GetIdentity(),
				Target.GetArtifactKind(),
				Target.GetDurableLocation(),
				TEXT("fixture://sa008-editor-project/")
					+ FString::FromInt(PrepareCount),
				EAssetDurableIntent::Save,
				Target.GetArtifactKind()
						== EDurableArtifactKind::Package
					? TEXT("asset_package")
					: TEXT("project_config_document"));
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
					Artifacts,
					Resources,
					InventoryHash,
					OutError))
			{
				return false;
			}
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				FDurableInventory(
					MoveTemp(Artifacts),
					MoveTemp(InventoryHash)),
				TEXT("journal:sa008-editor-project:")
					+ FString::FromInt(PrepareCount),
				TEXT("runtimeLogs/SA008EditorProject/journal.hjson"),
				TEXT("runtimeLogs/SA008EditorProject"),
				TEXT("runtimeLogs/SA008EditorProject/transaction"),
				TEXT("runtimeLogs/SA008EditorProject/staging"),
				TArray<FString>{TEXT("prepared")});
			return OutSession.IsValid();
		}

		bool TryEnlist(
			const FAssetDurableSessionRef& Session,
			const TArray<FAssetDurableTarget>& MutationTargets,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError();
			OutSession = Session;
			return MutationTargets.IsEmpty()
				&& CurrentFencingToken
					== Session->GetRequest().GetFencingToken();
		}

		bool TryPersist(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			++PersistCount;
			OutReceipt = FDurableReceipt(
				Session->GetJournalId(),
				EDurableStatus::PersistedUnverified,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				{TEXT("prepared"), TEXT("persisted_unverified")},
				ERecoveryAction::None,
				true,
				false);
			return CurrentFencingToken
					== Session->GetRequest().GetFencingToken()
				&& OutReceipt.RequiresIndependentVerification();
		}

		bool TryRollback(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			const bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError(
				TEXT("sa008_editor_project_unexpected_rollback"));
			return false;
		}

		bool TryAcknowledge(
			const FAssetDurableSessionRef& Session,
			const FDurableVerificationAcknowledgement& Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++AcknowledgeCount;
			return Acknowledgement.IsValid()
				&& Acknowledgement.GetJournalId()
					== Session->GetJournalId();
		}

		int32 PrepareCount = 0;
		int32 PersistCount = 0;
		int32 AcknowledgeCount = 0;
		TArray<EDurableArtifactKind> Kinds;
		TArray<FString> Locations;
	};

	class FUnusedAbsenceOracle final : public IAbsenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FAbsenceObservation(
				Identity,
				false,
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FUnusedReferenceOracle final : public IReferenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FReferenceObservation(
				Identity,
				{},
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				128);
			return true;
		}
	};

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	inline TSharedPtr<FJsonObject> BuildTypedChange(
		const FString& FieldId,
		const FString& Presence,
		const FString& TypeId = FString(),
		TSharedPtr<FJsonValue> CanonicalValue = nullptr)
	{
		const TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("field_id"), FieldId);
		Change->SetStringField(TEXT("presence"), Presence);
		if (CanonicalValue.IsValid())
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("type_id"), TypeId);
			Value->SetField(TEXT("canonical_value"), CanonicalValue);
			Change->SetObjectField(TEXT("value"), Value);
		}
		return Change;
	}

	inline TSharedPtr<FJsonValue> StringMapValue(
		const TMap<FString, FString>& Values)
	{
		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Values)
		{
			Object->SetStringField(Pair.Key, Pair.Value);
		}
		return MakeShared<FJsonValueObject>(Object);
	}

	inline TSharedPtr<FJsonValue> StringArrayValue(
		const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& Value : Values)
		{
			Items.Add(MakeShared<FJsonValueString>(Value));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Items));
	}

	inline TSharedPtr<FJsonObject> BuildPayloadForProfile(
		const FString& ProfileId)
	{
		TArray<TSharedPtr<FJsonValue>> Changes;
		auto Add = [&Changes](const TSharedPtr<FJsonObject>& Change)
		{
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		};
		if (ProfileId == TEXT("config_section"))
		{
			Add(BuildTypedChange(
				TEXT("entries"),
				TEXT("replace"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				StringMapValue({{TEXT("UAI.TestValue"), TEXT("after")}})));
		}
		else if (ProfileId == TEXT("project_settings"))
		{
			const TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
			Nested->SetStringField(
				TEXT("field_ref"),
				TEXT("uai://editor-project-field/test-enabled"));
			Nested->SetStringField(
				TEXT("expected_revision"),
				TEXT("field:r1"));
			Nested->SetStringField(TEXT("presence"), TEXT("set"));
			Nested->SetStringField(
				TEXT("type_id"),
				TEXT("uai.value.boolean.v1"));
			Nested->SetBoolField(TEXT("value"), true);
			Add(BuildTypedChange(
				TEXT("properties"),
				TEXT("set"),
				TEXT("uai.editor_project.settings_property_changes.v1"),
				MakeShared<FJsonValueArray>(
					TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueObject>(Nested),
					})));
		}
		else if (ProfileId == TEXT("device_profile"))
		{
			Add(BuildTypedChange(
				TEXT("device_type"),
				TEXT("set"),
				TEXT("uai.value.string.v1"),
				MakeShared<FJsonValueString>(TEXT("Windows"))));
			Add(BuildTypedChange(
				TEXT("base_profile_ref"),
				TEXT("set"),
				TEXT("uai.value.canonical_ref.v1"),
				MakeShared<FJsonValueString>(BaseDeviceProfileRef())));
			Add(BuildTypedChange(
				TEXT("cvars"),
				TEXT("replace"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				StringMapValue({{TEXT("r.UAI.Test"), TEXT("1")}})));
		}
		else if (ProfileId == TEXT("localization_target"))
		{
			Add(BuildTypedChange(
				TEXT("native_culture"),
				TEXT("set"),
				TEXT("uai.value.culture_name.v1"),
				MakeShared<FJsonValueString>(TEXT("en"))));
			Add(BuildTypedChange(
				TEXT("cultures"),
				TEXT("replace"),
				TEXT("uai.value.culture_name_set.v1"),
				StringArrayValue({TEXT("en"), TEXT("zh-Hans")})));
		}
		else if (ProfileId == TEXT("platform_config"))
		{
			Add(BuildTypedChange(
				TEXT("entries"),
				TEXT("replace"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				StringMapValue({{TEXT("r.UAI.Platform"), TEXT("1")}})));
		}
		else if (ProfileId == TEXT("scalability_group"))
		{
			Add(BuildTypedChange(
				TEXT("entries"),
				TEXT("replace"),
				TEXT("uai.editor_project.config_entry_map.v1"),
				StringMapValue({{TEXT("r.UAI.Quality"), TEXT("2")}})));
		}
		else if (ProfileId == TEXT("string_table"))
		{
			Add(BuildTypedChange(
				TEXT("namespace"),
				TEXT("set"),
				TEXT("uai.value.string.v1"),
				MakeShared<FJsonValueString>(TEXT("UAI.After"))));
			const TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
			Metadata->SetStringField(TEXT("Context"), TEXT("Automation"));
			const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("key"), TEXT("Greeting"));
			Row->SetStringField(TEXT("source"), TEXT("Hello"));
			Row->SetObjectField(TEXT("metadata"), Metadata);
			Add(BuildTypedChange(
				TEXT("entries"),
				TEXT("replace"),
				TEXT("uai.editor_project.string_table_entries.v1"),
				MakeShared<FJsonValueArray>(
					TArray<TSharedPtr<FJsonValue>>{
						MakeShared<FJsonValueObject>(Row),
					})));
		}
		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(
			TEXT("target_ref"),
			TargetRefForProfile(ProfileId));
		Payload->SetStringField(
			TEXT("expected_revision"),
			TEXT("rev:target:1"));
		Payload->SetArrayField(TEXT("changes"), MoveTemp(Changes));
		return Payload;
	}

	inline bool SerializeJson(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutJson)
	{
		OutJson.Reset();
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	inline bool IsComplete(const FUeAgentOperationResult& Result)
	{
		const FString& Json = Result.GetValue().GetJsonValue();
		return Json.Contains(TEXT("\"effect_outcome\":\"complete\""))
			&& Json.Contains(TEXT("\"status\":\"verified\""));
	}

	inline const FUeAgentSimpleAuthoringCommandRoute* FindRoute(
		const FString& CapabilityId)
	{
		return GetSimpleAuthoringCommandRoutes().FindByPredicate(
			[&CapabilityId](
				const FUeAgentSimpleAuthoringCommandRoute& Route)
			{
				return Route.CapabilityId == CapabilityId;
			});
	}

	inline TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 32768},
				{EUeAgentResourceDimension::Items, 32768},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 32768},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 134217728},
			}));
	}
}
