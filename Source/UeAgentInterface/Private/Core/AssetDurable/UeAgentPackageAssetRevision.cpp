#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"

#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/SecureHash.h"
#include "Serialization/Archive.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "Templates/UniquePtr.h"
#include "UObject/Package.h"

namespace UE::UAI::AssetDurable
{
	namespace
	{
		bool TryReadResolvedPackageRevision(const FString& TargetRef, const FString& Filename,
			FString& OutRevision, Contract::FDiagnostic& OutError)
		{
			auto FailRead = [&OutError, &TargetRef, &Filename](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Condition)
			{
				OutError = Contract::FDiagnostic::Failure(Code, Reason, TEXT("revision_read"), Filename);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = TargetRef;
				OutError.PropertyPath = Condition;
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			};
			if (!IFileManager::Get().FileExists(*Filename))
				return FailRead(TEXT("package_revision_header_missing"), TEXT("The persisted package header does not exist for revision reading."), TEXT("package_header_exists"));
			TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
			if (!Reader)
				return FailRead(TEXT("package_revision_header_open_failed"), TEXT("The persisted package header could not be opened to compute its revision."), TEXT("package_header_readable"));
			const FMD5Hash Hash = FMD5Hash::HashFileFromArchive(Reader.Get());
			if (!Hash.IsValid() || Reader->IsError())
				return FailRead(TEXT("package_revision_header_read_failed"), TEXT("Reading the persisted package header failed while computing its revision."), TEXT("package_header_read_succeeded"));
			OutRevision = TEXT("package-md5:") + LexToString(Hash).ToLower();
			return true;
		}
	}

	bool TryReadPackageAssetRevision(const FString& TargetRef, FString& OutRevision, Contract::FDiagnostic& OutError)
	{
		OutRevision.Reset();
		OutError.Reset();
		FString Filename;
		if (!TryResolvePackageHeaderFilename(TargetRef, EPackageHeaderKind::EvidenceRequired, Filename, OutError))
			return false;
		return TryReadResolvedPackageRevision(TargetRef, Filename, OutRevision, OutError);
	}

	bool TryReadStableAssetRevision(UObject* Object, FString& OutRevision, Contract::FDiagnostic& OutError)
	{
		OutRevision.Reset();
		OutError.Reset();
		if (!IsValid(Object))
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("stable_asset_revision_object_invalid"),
				TEXT("A valid asset object is required to read a stable revision."), TEXT("revision_read"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("asset_valid");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		UPackage* const Package = Object->GetPackage();
		if (Package && !Package->HasAnyFlags(RF_Transient))
		{
			FString Filename;
			Contract::FDiagnostic HeaderError;
			const bool bHeaderResolved = TryResolvePackageHeaderFilename(
				Package->GetName(), EPackageHeaderKind::EvidenceRequired, Filename, HeaderError);
			if (bHeaderResolved && IFileManager::Get().FileExists(*Filename))
				return TryReadResolvedPackageRevision(Package->GetName(), Filename, OutRevision, OutError);
			// A valid package without a persisted header retains its memory revision.
			// Resolution errors other than absent type evidence must remain errors.
			if (!bHeaderResolved && HeaderError.Code != TEXT("durable_package_header_kind_evidence_required"))
			{
				OutError = MoveTemp(HeaderError);
				return false;
			}
		}
		OutRevision = FString::Printf(TEXT("asset-crc32:%08x"), FArchiveObjectCrc32().Crc32(Object));
		return true;
	}
}
