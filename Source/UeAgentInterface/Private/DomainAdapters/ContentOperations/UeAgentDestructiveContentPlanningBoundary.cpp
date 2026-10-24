#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeletePreflight.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

namespace UE::UAI::DomainAdapters::ContentOperations
{
	using namespace Contract;
	using namespace DomainAdapters::AssetDestructiveImport;

	namespace
	{
		TSharedPtr<FJsonValue> InputKind(const TSharedPtr<FJsonValue>& Value)
		{
			const TCHAR* Kind = !Value.IsValid() ? TEXT("missing")
				: Value->Type == EJson::Null ? TEXT("null")
				: Value->Type == EJson::String ? TEXT("string")
				: Value->Type == EJson::Number ? TEXT("number")
				: Value->Type == EJson::Boolean ? TEXT("boolean")
				: Value->Type == EJson::Array ? TEXT("array")
				: Value->Type == EJson::Object ? TEXT("object") : TEXT("none");
			return MakeShared<FJsonValueString>(Kind);
		}

		FDiagnostic PlanValueFailure(const FString& Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& SourceKind,
			const FString& Target = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("destructive_preflight"));
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.TargetPath = Target;
			return Error;
		}

		FDiagnostic NativeCondition(const FString& Code, const FString& Reason,
			const FString& Path, const FString& Target = FString(),
			bool Actual = false, bool Expected = true)
		{
			return PlanValueFailure(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected), TEXT("native_result"), Target);
		}

		void CollectGameRefs(
			const TSharedPtr<FJsonValue>& Value,
			TSet<FString>& OutRefs)
		{
			if (!Value.IsValid())
			{
				return;
			}
			switch (Value->Type)
			{
			case EJson::String:
			{
				FString Ref = Value->AsString().TrimStartAndEnd();
				if (Ref.StartsWith(TEXT("/Game/")))
				{
					int32 Colon = INDEX_NONE;
					if (Ref.FindChar(TEXT(':'), Colon))
					{
						Ref.LeftInline(Colon);
					}
					FString PackageName = Ref.Contains(TEXT("."))
						? FPackageName::ObjectPathToPackageName(Ref)
						: Ref;
					if (FPackageName::IsValidLongPackageName(PackageName, true))
					{
						OutRefs.Add(MoveTemp(PackageName));
					}
				}
				break;
			}
			case EJson::Array:
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					CollectGameRefs(Item, OutRefs);
				}
				break;
			case EJson::Object:
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
					: Value->AsObject()->Values)
				{
					if (Field.Key != TEXT("destructive_authorization_id"))
					{
						CollectGameRefs(Field.Value, OutRefs);
					}
				}
				break;
			default:
				break;
			}
		}

		bool TryCanonicalParams(
			const FString& CommandId,
			const TSharedPtr<FJsonObject>& Params,
			FString& OutCanonical,
			TSet<FString>& OutPackageRefs,
			TSet<FString>& OutReadOnlyPackageRefs,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Params.IsValid())
			{
				OutError = PlanValueFailure(TEXT("destructive_params_required"),
					TEXT("Destructive planning requires a parameter object."), TEXT("/params"),
					MakeShared<FJsonValueString>(TEXT("missing")), MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"));
				return false;
			}
			const TSharedRef<FJsonObject> Sanitized = MakeShared<FJsonObject>();
			// Classify command operands here, not by asset type. Native clone and
			// mesh replacement read these sources without saving or rolling them back.
			const FString ReadOnlyField = CommandId == TEXT("asset.object.clone")
				? TEXT("source_ref") : CommandId == TEXT("modeling.actor.mesh.replace")
				? TEXT("mesh_ref") : FString();
			TSet<FString> WritablePackageRefs;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Params->Values)
			{
				if (Field.Key == TEXT("destructive_authorization_id"))
				{
					OutError = PlanValueFailure(TEXT("destructive_authorization_id_not_supported"),
						TEXT("destructive_authorization_id is not accepted as a command parameter."),
						TEXT("/params/destructive_authorization_id"), MakeShared<FJsonValueBoolean>(true),
						MakeShared<FJsonValueBoolean>(false), TEXT("command_input"));
					return false;
				}
				if (Field.Key == TEXT("confirm"))
				{
					continue;
				}
				Sanitized->SetField(Field.Key, Field.Value);
				CollectGameRefs(Field.Value, OutPackageRefs);
				CollectGameRefs(Field.Value, Field.Key == ReadOnlyField
					? OutReadOnlyPackageRefs : WritablePackageRefs);
			}
			for (const FString& Package : WritablePackageRefs)
			{
				OutReadOnlyPackageRefs.Remove(Package);
			}
			if (!Json::TryCanonicalize(Sanitized, OutCanonical, OutError))
			{
				if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("command_input");
				OutError.PropertyPath = TEXT("/params") + OutError.PropertyPath;
				return false;
			}
			return true;
		}

		bool AppendPackageRevision(
			const FString& PackageName,
			const bool bReadOnly,
			TArray<FString>& OutRevisions,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (UPackage* const Loaded = FindPackage(nullptr, *PackageName))
			{
				if (Loaded->IsDirty() && !bReadOnly)
				{
					OutError = NativeCondition(TEXT("destructive_plan_dirty_package"),
						TEXT("A package in the write scope has unsaved changes."),
						TEXT("/package/dirty"), PackageName, true, false);
					OutError.RecoveryAction = TEXT("Save or explicitly discard this package's changes, then retry.");
					OutError.bRetrySafe = true;
					return false;
				}
			}
			FString Filename;
			if (!FPackageName::DoesPackageExist(PackageName, &Filename))
			{
				OutRevisions.Add(PackageName + TEXT("=missing"));
				return true;
			}
			const int64 Size = IFileManager::Get().FileSize(*Filename);
			const int64 Timestamp = IFileManager::Get().GetTimeStamp(*Filename).GetTicks();
			OutRevisions.Add(FString::Printf(
				TEXT("%s=size:%lld;timestamp:%lld"),
				*PackageName,
				static_cast<long long>(Size),
				static_cast<long long>(Timestamp)));
			return true;
		}

		bool TryResolveAssetDeletePlanSelection(
			const TSharedPtr<FJsonObject>& Params,
			FUeAgentDestructiveContentPlan& OutPlan,
			TSet<FString>& OutPackageRefs,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetArrayField(TEXT("targets"), TargetValues)
				|| !TargetValues
				|| TargetValues->IsEmpty())
			{
				OutError = PlanValueFailure(TEXT("asset_delete_selection_targets_empty"),
					TEXT("Asset deletion requires a non-empty targets array."), TEXT("/params/targets"),
					TargetValues ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(TargetValues->Num()))
						: InputKind(Params.IsValid() ? Params->TryGetField(TEXT("targets")) : nullptr),
					MakeShared<FJsonValueString>(TEXT("non-empty array")), TEXT("command_input"));
				return false;
			}
			FUeAgentAssetDeleteSelectionRequest Request;
			Request.Targets.Reserve(TargetValues->Num());
			for (int32 Index = 0; Index < TargetValues->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*TargetValues)[Index];
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					OutError = PlanValueFailure(TEXT("asset_delete_selection_target_not_string"),
						TEXT("Each deletion target must be a string asset reference."),
						TEXT("/params/targets/") + FString::FromInt(Index), InputKind(Value),
						MakeShared<FJsonValueString>(TEXT("string")), TEXT("command_input"));
					return false;
				}
				Request.Targets.Add(Value->AsString());
			}
			if (Params->HasField(TEXT("recursive"))
				&& !Params->TryGetBoolField(TEXT("recursive"), Request.bRecursive))
			{
				OutError = PlanValueFailure(TEXT("asset_delete_selection_recursive_not_boolean"),
					TEXT("The recursive selection flag must be a boolean."), TEXT("/params/recursive"),
					InputKind(Params->TryGetField(TEXT("recursive"))), MakeShared<FJsonValueString>(TEXT("boolean")), TEXT("command_input"));
				return false;
			}
			const FUeAgentAssetDeletePreflightResult Preflight =
				PrepareAssetDeleteSelection(Request);
			if (!Preflight.IsSuccess())
			{
				OutError = Preflight.Diagnostic;
				return false;
			}
			OutPackageRefs.Reset();
			OutPlan.Targets.Reset(Preflight.Targets.Num());
			for (const FUeAgentAssetDeletePreparedTarget& Item : Preflight.Targets)
			{
				OutPackageRefs.Add(Item.PackageName);
				FUeAgentDestructiveContentPlanTarget& Target =
					OutPlan.Targets.AddDefaulted_GetRef();
				Target.TargetRef = Item.ObjectPath;
				Target.PackageName = Item.PackageName;
				Target.ObjectPath = Item.ObjectPath;
				Target.BlockerDiagnostic = Item.Diagnostic;
				for (const FUeAgentAssetDeleteSelectionSource& Source : Item.Sources)
				{
					Target.Sources.AddUnique(Source.InputRef);
				}
				Target.Sources.Sort();
			}
			return true;
		}

		bool TryCollectLevelContentActorScope(
			const TSharedPtr<FJsonObject>& Params,
			UWorld* World,
			TSet<FString>& OutPackageRefs,
			TArray<FString>& OutImpacts,
			TArray<FString>& OutRevisions,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Params.IsValid() || !World)
			{
				OutError = !Params.IsValid()
					? PlanValueFailure(TEXT("destructive_world_scope_unavailable"), TEXT("World scope selection requires a parameter object."),
						TEXT("/params"), MakeShared<FJsonValueString>(TEXT("missing")), MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"))
					: NativeCondition(TEXT("destructive_world_scope_unavailable"), TEXT("The world for actor scope selection is unavailable."), TEXT("/world/available"));
				return false;
			}
			FString FolderPath;
			if (!Params->TryGetStringField(TEXT("folder_path"), FolderPath))
			{
				OutError = PlanValueFailure(TEXT("destructive_world_folder_path_required"),
					TEXT("Actor scope selection requires a string folder_path."), TEXT("/params/folder_path"),
					InputKind(Params->TryGetField(TEXT("folder_path"))), MakeShared<FJsonValueString>(TEXT("string")), TEXT("command_input"),
					World->GetOutermost()->GetName());
				return false;
			}
			FolderPath.TrimStartAndEndInline();
			if (FolderPath.IsEmpty())
			{
				OutError = PlanValueFailure(TEXT("destructive_world_folder_path_required"),
					TEXT("Actor scope selection requires a non-empty folder_path."), TEXT("/params/folder_path"),
					MakeShared<FJsonValueString>(FolderPath), MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input"),
					World->GetOutermost()->GetName());
				return false;
			}

			FString ManagedId;
			const bool bHasManagedId = Params->HasField(TEXT("managed_id"));
			if (bHasManagedId)
			{
				if (!Params->TryGetStringField(TEXT("managed_id"), ManagedId))
				{
					OutError = PlanValueFailure(TEXT("destructive_world_managed_id_invalid"),
						TEXT("The managed actor identifier must be a string."), TEXT("/params/managed_id"),
						InputKind(Params->TryGetField(TEXT("managed_id"))), MakeShared<FJsonValueString>(TEXT("string")), TEXT("command_input"),
						World->GetOutermost()->GetName());
					return false;
				}
				ManagedId.TrimStartAndEndInline();
				if (ManagedId.IsEmpty())
				{
					OutError = PlanValueFailure(TEXT("destructive_world_managed_id_required"),
						TEXT("The supplied managed actor identifier must not be empty."), TEXT("/params/managed_id"),
						MakeShared<FJsonValueString>(ManagedId), MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input"),
						World->GetOutermost()->GetName());
					return false;
				}
			}

			TArray<AActor*> Actors;
			if (bHasManagedId)
			{
				AActor* Actor = nullptr;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!UeAgentLevelAuthoring::TryFindManagedActorExact(
						*World,
						ManagedId,
						FolderPath,
						Actor,
						ResolveError)
					|| !Actor)
				{
					if (ResolveError.IsValid()) OutError = ResolveError;
					else
					{
						auto Selection = MakeShared<FJsonObject>();
						Selection->SetStringField(TEXT("managed_id"), ManagedId);
						Selection->SetStringField(TEXT("folder_path"), FolderPath);
						Selection->SetBoolField(TEXT("actor_available"), false);
						OutError = PlanValueFailure(TEXT("destructive_world_exact_actor_unavailable"),
							TEXT("No actor was returned for the exact managed actor and folder selection."), TEXT("/actor"),
							MakeShared<FJsonValueObject>(Selection), MakeShared<FJsonValueString>(TEXT("resolved actor")),
							TEXT("native_result"), World->GetOutermost()->GetName());
					}
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = World->GetOutermost()->GetName();
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ManagedId;
					return false;
				}
				Actors.Add(Actor);
			}
			else
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* const Actor = *It;
					if (!Actor || Actor->IsActorBeingDestroyed())
					{
						continue;
					}
					const FString ActorFolder =
						UeAgentLevelAuthoring::GetActorFolderPathText(*Actor);
					if (ActorFolder == FolderPath
						|| ActorFolder.StartsWith(FolderPath + TEXT("/")))
					{
						Actors.Add(Actor);
					}
				}
				if (Actors.IsEmpty())
				{
					OutError = PlanValueFailure(TEXT("destructive_world_folder_scope_empty"),
						TEXT("No live actors match the selected folder or its subfolders."), TEXT("/params/folder_path"),
						MakeShared<FJsonValueNumber>(Actors.Num()), MakeShared<FJsonValueString>(TEXT("at least one matching actor")), TEXT("native_result"),
						World->GetOutermost()->GetName());
					OutError.CandidateName = FolderPath;
					return false;
				}
			}

			Actors.Sort([](const AActor& Left, const AActor& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			for (const AActor* const Actor : Actors)
			{
				const FString ActorPath = Actor->GetPathName();
				const FString ActorFolder =
					UeAgentLevelAuthoring::GetActorFolderPathText(*Actor);
				const FString ActorManagedId =
					UeAgentLevelAuthoring::GetManagedActorId(*Actor);
				OutImpacts.Add(TEXT("actor:") + ActorPath);
				OutRevisions.Add(
					TEXT("actor:") + ActorPath
					+ TEXT("=folder:") + ActorFolder
					+ TEXT(";managed_id:") + ActorManagedId);
				const FString ActorPackage = Actor->GetOutermost()->GetName();
				if (ActorPackage.StartsWith(TEXT("/Game/")))
				{
					OutPackageRefs.Add(ActorPackage);
				}
			}
			return true;
		}

		bool TryCollectDataLayerDeleteScope(
			const TSharedPtr<FJsonObject>& Params,
			UWorld* ExplicitWorld,
			TSet<FString>& InOutPackageRefs,
			TArray<FString>& OutImpacts,
			TArray<FString>& OutRevisions,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Params.IsValid() || !ExplicitWorld)
			{
				OutError = !Params.IsValid()
					? PlanValueFailure(TEXT("world_data_layer_context_missing"), TEXT("Data layer deletion requires a parameter object."),
						TEXT("/params"), MakeShared<FJsonValueString>(TEXT("missing")), MakeShared<FJsonValueString>(TEXT("object")), TEXT("command_input"))
					: NativeCondition(TEXT("world_data_layer_context_missing"), TEXT("The world for data layer deletion is unavailable."), TEXT("/world/available"));
				return false;
			}
			FString TargetRef;
			if (!Params->TryGetStringField(TEXT("layer"), TargetRef)
				|| TargetRef.TrimStartAndEnd().IsEmpty())
			{
				const auto Value = Params->TryGetField(TEXT("layer"));
				OutError = PlanValueFailure(TEXT("world_data_layer_target_required"),
					TEXT("Data layer deletion requires a non-empty string layer reference."), TEXT("/params/layer"),
					Value.IsValid() && Value->Type == EJson::String ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Value->AsString())) : InputKind(Value),
					MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("command_input"), ExplicitWorld->GetOutermost()->GetName());
				return false;
			}
			AWorldDataLayers* const WorldDataLayers =
				ExplicitWorld->GetWorldDataLayers();
			UDataLayerInstance* Instance = nullptr;
			if (!WorldDataLayers
				|| !UeAgentLevelAuthoring::
					TryResolveDataLayerInstanceExact(
						*WorldDataLayers,
						TargetRef,
						Instance,
						OutError))
			{
				if (!WorldDataLayers)
				{
					OutError = NativeCondition(TEXT("world_data_layers_missing"),
						TEXT("The selected world has no world data layers owner."), TEXT("/world/data_layers/available"),
						ExplicitWorld->GetOutermost()->GetName());
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = ExplicitWorld->GetOutermost()->GetName();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TargetRef;
				return false;
			}
			if (!Instance->CanBeRemoved())
			{
				OutError = NativeCondition(TEXT("world_data_layer_not_removable"),
					TEXT("The selected data layer cannot be removed."), TEXT("/data_layer/removable"), ExplicitWorld->GetOutermost()->GetName());
				OutError.CandidateName = TargetRef;
				return false;
			}
			AWorldDataLayers* const MutationOwner =
				Instance->GetDirectOuterWorldDataLayers();
			if (!MutationOwner)
			{
				OutError = NativeCondition(TEXT("world_data_layer_owner_missing"),
					TEXT("The selected data layer has no direct world data layers owner."), TEXT("/data_layer/owner/available"), ExplicitWorld->GetOutermost()->GetName());
				OutError.CandidateName = TargetRef;
				return false;
			}
			const FString MutationPackage =
				MutationOwner->GetOutermost()->GetName();
			if (!MutationPackage.StartsWith(TEXT("/Game/"))
				|| !FPackageName::IsValidLongPackageName(
					MutationPackage,
					true))
			{
				OutError = PlanValueFailure(TEXT("world_data_layer_package_invalid"),
					TEXT("The data layer mutation owner must belong to a valid /Game package."), TEXT("/data_layer/owner/package"),
					MakeShared<FJsonValueString>(MutationPackage), MakeShared<FJsonValueString>(TEXT("valid /Game package")), TEXT("native_result"),
					ExplicitWorld->GetOutermost()->GetName());
				OutError.CandidateName = TargetRef;
				return false;
			}
			InOutPackageRefs.Add(MutationPackage);
			OutImpacts.Add(TEXT("data_layer:") + Instance->GetPathName());
			OutRevisions.Add(
				TEXT("data_layer=") + Instance->GetPathName()
				+ TEXT("|f_name=")
				+ Instance->GetDataLayerFName().ToString()
				+ TEXT("|short_name=")
				+ Instance->GetDataLayerShortName()
				+ TEXT("|owner_package=") + MutationPackage
				+ TEXT("|removable=true"));
			return true;
		}
	}

	bool FUeAgentDestructiveContentPlan::IsValid() const
	{
		return !PlanId.IsEmpty()
			&& PlanFingerprint.StartsWith(TEXT("sha256:"))
			&& AuthorizationId == TEXT("destructive-authorization:") + PlanFingerprint
			&& !CanonicalParamsJson.IsEmpty()
			&& !ImpactScope.IsEmpty()
			&& !RevisionSnapshot.IsEmpty();
	}

	bool TryCollectOwnedWorldExternalPackages(
		const FString& WorldPackageName,
		TArray<FString>& OutPackageNames,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPackageNames.Reset();
		OutError.Reset();
		FString RelativeWorldPath = WorldPackageName;
		if (!RelativeWorldPath.RemoveFromStart(TEXT("/Game/"))
			|| RelativeWorldPath.IsEmpty())
		{
			OutError = PlanValueFailure(TEXT("destructive_world_external_scope_invalid"),
				TEXT("External package discovery requires a non-empty world package below /Game/."), TEXT("/world/package"),
				MakeShared<FJsonValueString>(WorldPackageName), MakeShared<FJsonValueString>(TEXT("/Game/<non-empty world path>")),
				TEXT("native_result"), WorldPackageName);
			return false;
		}
		TSet<FString> Packages;
		for (const FString& ExternalRoot : {
			FString(TEXT("/Game/__ExternalActors__/")) + RelativeWorldPath,
			FString(TEXT("/Game/__ExternalObjects__/")) + RelativeWorldPath})
		{
			FString Directory;
			if (!FPackageName::TryConvertLongPackageNameToFilename(
					ExternalRoot, Directory))
			{
				OutError = PlanValueFailure(TEXT("destructive_world_external_path_unavailable"),
					TEXT("The external package root could not be converted to a filesystem directory."), TEXT("/external_packages/root"),
					MakeShared<FJsonValueString>(ExternalRoot), MakeShared<FJsonValueString>(TEXT("mounted package root")),
					TEXT("native_result"), WorldPackageName);
				return false;
			}
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(
				Files, *Directory, TEXT("*.uasset"), true, false, false);
			Files.Sort();
			for (const FString& Filename : Files)
			{
				FString PackageName;
				if (!FPackageName::TryConvertFilenameToLongPackageName(
						Filename, PackageName)
					|| !PackageName.StartsWith(ExternalRoot + TEXT("/")))
				{
					OutError = PlanValueFailure(TEXT("destructive_world_external_package_invalid"),
						TEXT("An external asset file does not resolve to a package below its expected world-owned root."), TEXT("/external_packages/file"),
						MakeShared<FJsonValueString>(Filename), MakeShared<FJsonValueString>(ExternalRoot + TEXT("/<package>")),
						TEXT("native_result"), WorldPackageName);
					OutError.Source.SourceFile = Filename;
					return false;
				}
				Packages.Add(MoveTemp(PackageName));
			}
		}
		OutPackageNames = Packages.Array();
		OutPackageNames.Sort();
		return true;
	}

	bool TryBuildDestructiveContentPlan(
		const FString& CommandId,
		const TSharedPtr<FJsonObject>& Params,
		UWorld* ExplicitWorld,
		FUeAgentDestructiveContentPlan& OutPlan,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPlan = {};
		OutError.Reset();
		FString CanonicalParams;
		TSet<FString> PackageRefs;
		TSet<FString> ReadOnlyPackageRefs;
		if (!TryCanonicalParams(
				CommandId, Params, CanonicalParams, PackageRefs, ReadOnlyPackageRefs, OutError))
		{
			return false;
		}
		TArray<FString> WorldObjectImpacts;
		TArray<FString> WorldObjectRevisions;
		if (CommandId == TEXT("world.level_content.delete_scope")
			&& !TryCollectLevelContentActorScope(
				Params,
				ExplicitWorld,
				PackageRefs,
				WorldObjectImpacts,
				WorldObjectRevisions,
				OutError))
		{
			return false;
		}
		if (CommandId == TEXT("world.data_layer.delete")
			&& !TryCollectDataLayerDeleteScope(
				Params,
				ExplicitWorld,
				PackageRefs,
				WorldObjectImpacts,
				WorldObjectRevisions,
				OutError))
		{
			return false;
		}

		if ((CommandId.StartsWith(TEXT("world."))
				|| CommandId.StartsWith(TEXT("modeling.")))
			&& ExplicitWorld)
		{
			const FString WorldPackage = ExplicitWorld->GetOutermost()->GetName();
			if (WorldPackage.StartsWith(TEXT("/Game/")))
			{
				PackageRefs.Add(WorldPackage);
				ReadOnlyPackageRefs.Remove(WorldPackage);
			}
		}
		if (CommandId == TEXT("asset.delete"))
		{
			if (!TryResolveAssetDeletePlanSelection(
					Params, OutPlan, PackageRefs, OutError))
			{
				return false;
			}
			const TArray<FString> DirectPackages = PackageRefs.Array();
			for (const FString& PackageName : DirectPackages)
			{
				TArray<FString> ExternalPackages;
				if (!TryCollectOwnedWorldExternalPackages(
						PackageName,
						ExternalPackages,
						OutError))
				{
					return false;
				}
				PackageRefs.Append(ExternalPackages);
			}
		}

		TArray<FString> SortedPackages = PackageRefs.Array();
		SortedPackages.Sort();

		TSet<FString> Impacts;
		Impacts.Add(TEXT("command:") + CommandId);
		for (const FString& ActorImpact : WorldObjectImpacts)
		{
			Impacts.Add(ActorImpact);
		}
		OutPlan.RevisionSnapshot.Append(WorldObjectRevisions);
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		for (const FString& PackageName : SortedPackages)
		{
			Impacts.Add(TEXT("package:") + PackageName);
			if (!AppendPackageRevision(
					PackageName, ReadOnlyPackageRefs.Contains(PackageName), OutPlan.RevisionSnapshot, OutError))
			{
				return false;
			}
			TArray<FName> Referencers;
			Registry.GetReferencers(FName(*PackageName), Referencers);
			for (const FName Referencer : Referencers)
			{
				Impacts.Add(TEXT("referencer:") + Referencer.ToString());
			}
		}
		if (OutPlan.RevisionSnapshot.IsEmpty())
		{
			OutPlan.RevisionSnapshot.Add(TEXT("no-package-ref=canonical-input-only"));
		}
		OutPlan.RevisionSnapshot.Sort();
		OutPlan.ImpactScope = Impacts.Array();
		OutPlan.ImpactScope.Sort();
		OutPlan.CanonicalParamsJson = CanonicalParams;
		const FString Material = CommandId
			+ TEXT("\n") + CanonicalParams
			+ TEXT("\n") + FString::Join(OutPlan.ImpactScope, TEXT("\n"))
			+ TEXT("\n") + FString::Join(OutPlan.RevisionSnapshot, TEXT("\n"));
		OutPlan.PlanFingerprint = Json::Sha256Utf8(Material);
		OutPlan.PlanId = TEXT("destructive-plan:")
			+ OutPlan.PlanFingerprint.RightChop(7);
		OutPlan.AuthorizationId = TEXT("destructive-authorization:")
			+ OutPlan.PlanFingerprint;
		if (!OutPlan.IsValid())
		{
			const TCHAR* Field = OutPlan.PlanId.IsEmpty() ? TEXT("/plan/id/present")
				: !OutPlan.PlanFingerprint.StartsWith(TEXT("sha256:")) ? TEXT("/plan/fingerprint/format_valid")
				: OutPlan.AuthorizationId != TEXT("destructive-authorization:") + OutPlan.PlanFingerprint ? TEXT("/plan/authorization/matches_fingerprint")
				: OutPlan.CanonicalParamsJson.IsEmpty() ? TEXT("/plan/canonical_params/present")
				: OutPlan.ImpactScope.IsEmpty() ? TEXT("/plan/impact_scope/present") : TEXT("/plan/revision_snapshot/present");
			const TCHAR* Reason = OutPlan.PlanId.IsEmpty() ? TEXT("The destructive plan identifier was not generated.")
				: !OutPlan.PlanFingerprint.StartsWith(TEXT("sha256:")) ? TEXT("The destructive plan fingerprint has an invalid format.")
				: OutPlan.AuthorizationId != TEXT("destructive-authorization:") + OutPlan.PlanFingerprint ? TEXT("The destructive plan authorization does not match its fingerprint.")
				: OutPlan.CanonicalParamsJson.IsEmpty() ? TEXT("The destructive plan has no canonical parameter capture.")
				: OutPlan.ImpactScope.IsEmpty() ? TEXT("The destructive plan has no impact scope.") : TEXT("The destructive plan has no revision capture.");
			OutError = NativeCondition(TEXT("destructive_plan_invalid"), Reason, Field);
			OutError.SubjectId = OutPlan.PlanId;
			auto Identity = MakeShared<FJsonObject>();
			Identity->SetStringField(TEXT("fingerprint"), OutPlan.PlanFingerprint);
			Identity->SetStringField(TEXT("authorization_id"), OutPlan.AuthorizationId);
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Identity);
			return false;
		}
		return true;
	}
}
