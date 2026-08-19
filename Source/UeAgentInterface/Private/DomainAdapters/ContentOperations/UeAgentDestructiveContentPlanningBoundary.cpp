#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::ContentOperations
{
	using namespace Contract;

	namespace
	{
		constexpr int32 MaxImpactedPackages = 2048;

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
			const TSharedPtr<FJsonObject>& Params,
			FString& OutCanonical,
			TSet<FString>& OutPackageRefs,
			FString& OutError)
		{
			if (!Params.IsValid())
			{
				OutError = TEXT("destructive_params_required");
				return false;
			}
			const TSharedRef<FJsonObject> Sanitized = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Params->Values)
			{
				if (Field.Key == TEXT("destructive_authorization_id"))
				{
					continue;
				}
				Sanitized->SetField(Field.Key, Field.Value);
				CollectGameRefs(Field.Value, OutPackageRefs);
			}
			return Json::TryCanonicalize(Sanitized, OutCanonical, OutError);
		}

		bool AppendPackageRevision(
			const FString& PackageName,
			TArray<FString>& OutRevisions,
			FString& OutError)
		{
			if (UPackage* const Loaded = FindPackage(nullptr, *PackageName))
			{
				if (Loaded->IsDirty())
				{
					OutError = TEXT("destructive_plan_dirty_package:") + PackageName;
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
	}

	bool FUeAgentDestructiveContentPlan::IsValid() const
	{
		return !PlanId.IsEmpty()
			&& PlanFingerprint.StartsWith(TEXT("sha256:"))
			&& AuthorizationId == TEXT("destructive-authorization:") + PlanFingerprint
			&& !ImpactScope.IsEmpty()
			&& !RevisionSnapshot.IsEmpty();
	}

	bool TryCollectOwnedWorldExternalPackages(
		const FString& WorldPackageName,
		const int32 MaxPackages,
		TArray<FString>& OutPackageNames,
		FString& OutError)
	{
		OutPackageNames.Reset();
		OutError.Reset();
		FString RelativeWorldPath = WorldPackageName;
		if (MaxPackages <= 0
			|| !RelativeWorldPath.RemoveFromStart(TEXT("/Game/"))
			|| RelativeWorldPath.IsEmpty())
		{
			OutError = TEXT("destructive_world_external_scope_invalid:")
				+ WorldPackageName;
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
				OutError = TEXT("destructive_world_external_path_unavailable:")
					+ ExternalRoot;
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
					OutError = TEXT("destructive_world_external_package_invalid:")
						+ Filename;
					return false;
				}
				Packages.Add(MoveTemp(PackageName));
				if (Packages.Num() > MaxPackages)
				{
					OutError = TEXT("destructive_impact_scope_exceeded");
					return false;
				}
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
		FString& OutError)
	{
		OutPlan = {};
		OutError.Reset();
		FString CanonicalParams;
		TSet<FString> PackageRefs;
		if (!TryCanonicalParams(
				Params, CanonicalParams, PackageRefs, OutError))
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
			}
		}
		if (CommandId == TEXT("asset.delete"))
		{
			const TArray<FString> DirectPackages = PackageRefs.Array();
			for (const FString& PackageName : DirectPackages)
			{
				TArray<FString> ExternalPackages;
				if (!TryCollectOwnedWorldExternalPackages(
						PackageName,
						MaxImpactedPackages,
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
		if (SortedPackages.Num() > MaxImpactedPackages)
		{
			OutError = TEXT("destructive_impact_scope_exceeded");
			return false;
		}

		TSet<FString> Impacts;
		Impacts.Add(TEXT("command:") + CommandId);
		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		for (const FString& PackageName : SortedPackages)
		{
			Impacts.Add(TEXT("package:") + PackageName);
			if (!AppendPackageRevision(
					PackageName, OutPlan.RevisionSnapshot, OutError))
			{
				return false;
			}
			TArray<FName> Referencers;
			Registry.GetReferencers(FName(*PackageName), Referencers);
			for (const FName Referencer : Referencers)
			{
				Impacts.Add(TEXT("referencer:") + Referencer.ToString());
				if (Impacts.Num() > MaxImpactedPackages)
				{
					OutError = TEXT("destructive_impact_scope_exceeded");
					return false;
				}
			}
		}
		if (OutPlan.RevisionSnapshot.IsEmpty())
		{
			OutPlan.RevisionSnapshot.Add(TEXT("no-package-ref=canonical-input-only"));
		}
		OutPlan.RevisionSnapshot.Sort();
		OutPlan.ImpactScope = Impacts.Array();
		OutPlan.ImpactScope.Sort();
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
			OutError = TEXT("destructive_plan_invalid");
			return false;
		}
		return true;
	}
}
