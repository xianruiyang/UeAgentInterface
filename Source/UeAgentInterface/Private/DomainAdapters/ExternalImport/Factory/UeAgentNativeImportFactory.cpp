#include "DomainAdapters/ExternalImport/Factory/UeAgentNativeImportFactory.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::DomainAdapters::ExternalImport::Factory
{
    namespace
    {
        TArray<UFactory*> Candidates(const UClass* ExpectedClass)
        {
            TArray<UFactory*> Result;
            if (!ExpectedClass) return Result;
            for (TObjectIterator<UClass> It; It; ++It)
            {
                if (!It->IsChildOf(UFactory::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
                UFactory* Factory = It->GetDefaultObject<UFactory>();
                const UClass* Supported = Factory->ResolveSupportedClass();
                if (Factory->bEditorImport && Factory->ImportPriority >= 0 && Supported &&
                    (ExpectedClass->IsChildOf(Supported) || Supported->IsChildOf(ExpectedClass))) Result.Add(Factory);
            }
            Result.Sort([](const UFactory& A, const UFactory& B) {
                return A.ImportPriority != B.ImportPriority ? A.ImportPriority > B.ImportPriority
                    : A.GetClass()->GetPathName() < B.GetClass()->GetPathName();
            });
            return Result;
        }
    }

    TSet<FString> NativeImportExtensions(const UClass* ExpectedClass)
    {
        TSet<FString> Result;
        for (UFactory* Factory : Candidates(ExpectedClass))
        {
            TArray<FString> Formats;
            Factory->GetSupportedFileExtensions(Formats);
            for (FString& Extension : Formats) Result.Add(Extension.ToLower());
        }
        return Result;
    }

    UFactory* SelectNativeImportFactory(const UClass* ExpectedClass, const FString& Source)
    {
        UClass* Fallback = nullptr;
        const FString Extension = FPaths::GetExtension(Source);
        for (UFactory* Candidate : Candidates(ExpectedClass))
        {
            TArray<FString> Extensions;
            Candidate->GetSupportedFileExtensions(Extensions);
            if (!Extensions.ContainsByPredicate([&](const FString& Item) { return Item.Equals(Extension, ESearchCase::IgnoreCase); })) continue;
            if (!Fallback) Fallback = Candidate->GetClass();
            TStrongObjectPtr<UFactory> Instance(NewObject<UFactory>(GetTransientPackage(), Candidate->GetClass()));
            if (Instance->FactoryCanImport(Source)) return Instance.Get();
        }
        // Match AssetTools: the base FactoryCanImport returns false; advertised
        // format and native priority still provide its fallback selection.
        return Fallback ? NewObject<UFactory>(GetTransientPackage(), Fallback) : nullptr;
    }

    TArray<UObject*> ImportNativeAsset(UFactory* Factory, const FString& Source, const FString& DestinationPackage)
    {
        TStrongObjectPtr<UFactory> FactoryKeepAlive(Factory);
        TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
        Task->Filename = Source;
        Task->DestinationPath = FPackageName::GetLongPackagePath(DestinationPackage);
        Task->DestinationName = FPackageName::GetLongPackageAssetName(DestinationPackage);
        Task->Factory = Factory;
        Task->bAutomated = true;
        Task->bAsync = false;
        Task->bSave = false;
        Task->bReplaceExisting = false;
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task.Get()});
        return Task->GetObjects();
    }
}
