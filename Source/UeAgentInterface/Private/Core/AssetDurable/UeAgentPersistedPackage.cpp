#include "Core/AssetDurable/UeAgentPersistedPackage.h"

#include "HAL/FileManager.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::AssetDurable
{
	namespace
	{
		void SetError(
			FPersistedPackageLoadError& OutError,
			FString Code,
			FString Reason,
			FString Target = FString(),
			FString Detail = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("persisted_package_readback"), MoveTemp(Detail));
			OutError.TargetPath = MoveTemp(Target); OutError.SourceKind = TEXT("native_result");
		}

		class FScopedPersistedPackage final
		{
		public:
			~FScopedPersistedPackage()
			{
				if (!bCleanupAttempted)
				{
					UE::UAI::Contract::FDiagnostic Ignored;
					TryUnload(Ignored);
				}
			}

			void Set(UPackage* InPackage)
			{
				Package = InPackage;
			}

			bool TryUnload(UE::UAI::Contract::FDiagnostic& OutError)
			{
				bCleanupAttempted = true;
				OutError.Reset();
				UPackage* Loaded = Package.Get();
				if (!Loaded)
				{
					return true;
				}
				Loaded->SetDirtyFlag(false);
				FText UnloadError;
				if (!UPackageTools::UnloadPackages({Loaded}, UnloadError, true))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("persisted_package_cleanup_failed"), UnloadError.IsEmpty() ? TEXT("Unreal could not unload the isolated persisted package after readback.") : UnloadError.ToString(), TEXT("native_package_unload"), Loaded->GetName());
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				Package.Reset();
				return true;
			}

		private:
			TWeakObjectPtr<UPackage> Package;
			bool bCleanupAttempted = false;
		};
	}

	bool TryObservePersistedPackage(
		const TArray<FDurableArtifact>& Artifacts,
		const FString& SourcePackageName,
		const FPersistedPackageObserver Observer,
		FPersistedPackageLoadError& OutError)
	{
		OutError = {};
		if (Artifacts.IsEmpty()
			|| !FPackageName::IsValidLongPackageName(SourcePackageName))
		{
			SetError(
				OutError,
				TEXT("persisted_package_request_invalid"),
				Artifacts.IsEmpty() ? TEXT("Persisted package readback requires at least one durable artifact.") : TEXT("The source package is not a valid long package name."),
				SourcePackageName);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Artifacts.IsEmpty() ? TEXT("at least one durable artifact") : TEXT("valid long package name"));
			OutError.ActualValue = Artifacts.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Artifacts.Num())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(SourcePackageName));
			return false;
		}

		const FDurableArtifact* Header = nullptr;
		for (const FDurableArtifact& Artifact : Artifacts)
		{
			if (Artifact.GetArtifactKind() != EDurableArtifactKind::Package
				|| Artifact.GetRole() != TEXT("package_header")
				|| Artifact.GetDurableLocation() != SourcePackageName)
			{
				continue;
			}
			if (Header)
			{
				SetError(
					OutError,
					TEXT("persisted_package_artifact_ambiguous"),
					TEXT("More than one package-header artifact identifies the requested source package."),
					SourcePackageName);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("at least 2 matching headers"));
				return false;
			}
			Header = &Artifact;
		}
		if (!Header
			|| !FPaths::GetExtension(Header->GetFinalPath()).Equals(
				TEXT("uasset"),
				ESearchCase::IgnoreCase)
			|| !IFileManager::Get().FileExists(*Header->GetFinalPath()))
		{
			SetError(
				OutError,
				TEXT("persisted_package_artifact_invalid"),
				!Header ? TEXT("No package-header artifact identifies the requested source package.")
					: !FPaths::GetExtension(Header->GetFinalPath()).Equals(TEXT("uasset"), ESearchCase::IgnoreCase)
					? TEXT("The package-header artifact does not point to a .uasset file.") : TEXT("The package-header artifact file does not exist."),
				SourcePackageName);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("existing .uasset package-header artifact"));
			OutError.ActualValuePresent = Header != nullptr;
			if (Header) { OutError.InternalContext = Header->GetFinalPath(); OutError.ActualValue = MakeShared<FJsonValueString>(FPaths::GetExtension(Header->GetFinalPath())); }
			return false;
		}

		const FString SnapshotToken =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString SnapshotPackageName =
			TEXT("/Temp/UAI/PersistedPackage/")
			+ SnapshotToken
			+ TEXT("/")
			+ FPackageName::GetShortName(SourcePackageName);
		FLinkerInstancingContext InstancingContext;
		InstancingContext.AddPackageMapping(
			FName(*SourcePackageName),
			FName(*SnapshotPackageName));

		FScopedPersistedPackage LoadedPackage;
		UPackage* SnapshotPackage = CreatePackage(*SnapshotPackageName);
		if (!SnapshotPackage)
		{
			SetError(
				OutError,
				TEXT("persisted_package_create_failed"),
				TEXT("Unreal could not create the isolated package used for persisted readback."), SourcePackageName,
				SnapshotPackageName);
			return false;
		}
		SnapshotPackage->SetFlags(RF_Transient);
		LoadedPackage.Set(SnapshotPackage);
		SnapshotPackage = LoadPackage(
			SnapshotPackage,
			*SourcePackageName,
			LOAD_DisableCompileOnLoad,
			nullptr,
			&InstancingContext);
		if (!SnapshotPackage
			|| SnapshotPackage->GetName() != SnapshotPackageName)
		{
			SetError(
				OutError,
				TEXT("persisted_package_load_failed"),
				!SnapshotPackage ? TEXT("Unreal could not load the saved package into the readback snapshot.") : TEXT("The loaded package does not have the requested isolated snapshot identity."),
				SourcePackageName);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(SnapshotPackageName);
			OutError.ActualValuePresent = SnapshotPackage != nullptr;
			if (SnapshotPackage) OutError.ActualEvidence = MakeShared<FJsonValueString>(SnapshotPackage->GetName());
			return false;
		}
		LoadedPackage.Set(SnapshotPackage);

		FPersistedPackageLoadError ObserverError;
		const bool bObserved = Observer(*SnapshotPackage, ObserverError);
		UE::UAI::Contract::FDiagnostic CleanupError;
		const bool bCleaned = LoadedPackage.TryUnload(CleanupError);
		if (!bObserved)
		{
			OutError = MoveTemp(ObserverError);
			if (!OutError.IsValid())
			{
				SetError(OutError, TEXT("persisted_package_observer_failed"), TEXT("The persisted-package observer rejected the loaded snapshot without returning a diagnostic."), SourcePackageName);
			}
			if (!bCleaned && !CleanupError.IsEmpty())
			{
				OutError.Append(CleanupError);
			}
			return false;
		}
		if (!bCleaned)
		{
			OutError = MoveTemp(CleanupError);
			return false;
		}
		return true;
	}
}
