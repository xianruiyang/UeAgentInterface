#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "EditorClassUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Modules/ModuleManager.h"

namespace UE::UAI::ContentDiscovery
{
    bool TryEnsureAssetDiscovery(Contract::FDiagnostic& OutError)
    {
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (!Registry.IsSearchAllAssets()) Registry.SearchAllAssets(true);
        Registry.WaitForCompletion();
        if (Registry.IsSearchAllAssets() && !Registry.IsLoadingAssets()) return true;
        OutError = Contract::FDiagnostic::Failure(TEXT("candidate_registry_incomplete"),
            TEXT("Asset discovery did not finish; the complete candidate catalog could not be collected."),
            TEXT("candidate_discovery"));
        OutError.SourceKind = TEXT("native_result");
        OutError.ActualValue = MakeShared<FJsonValueString>(Registry.IsLoadingAssets()
            ? TEXT("loading") : TEXT("full search incomplete"));
        OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("completed asset registry discovery"));
        return false;
    }

    bool TryCollectTypeAssets(UClass& AssetClass, TArray<FAssetData>& OutAssets,
        Contract::FDiagnostic& OutError)
    {
        OutAssets.Reset();
        if (!TryEnsureAssetDiscovery(OutError)) return false;
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.GetAssetsByClass(AssetClass.GetClassPathName(), OutAssets, true);
        return true;
    }

    bool TryLoadDerivedBlueprintClasses(UClass& BaseClass,
        TArray<TStrongObjectPtr<UClass>>& OutLoadedClasses, Contract::FDiagnostic& OutError)
    {
        if (!TryEnsureAssetDiscovery(OutError)) return false;
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TSet<FTopLevelAssetPath> Derived, Unloaded;
        Registry.GetDerivedClassNames({BaseClass.GetClassPathName()}, {}, Derived);
        TArray<FAssetData> Assets;
        Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, true);
        for (const FAssetData& Asset : Assets)
        {
            const FTopLevelAssetPath Path = FEditorClassUtils::GetClassPathNameFromAssetTag(Asset);
            if (Derived.Contains(Path) && !FindObject<UClass>(nullptr, *Path.ToString())) Unloaded.Add(Path);
        }
        Assets.Reset();
        Registry.GetAssetsByClass(UBlueprintGeneratedClass::StaticClass()->GetClassPathName(), Assets, true);
        for (const FAssetData& Asset : Assets)
        {
            const FTopLevelAssetPath Path = Asset.GetSoftObjectPath().GetAssetPath();
            if (Derived.Contains(Path) && !FindObject<UClass>(nullptr, *Path.ToString())) Unloaded.Add(Path);
        }
        for (const FTopLevelAssetPath& Path : Unloaded)
        {
            UClass* Class = LoadObject<UClass>(nullptr, *Path.ToString());
            if (!Class || !Class->IsChildOf(&BaseClass))
            {
                OutError = Contract::FDiagnostic::Failure(TEXT("candidate_class_load_failed"),
                    TEXT("A derived class discovered by the asset registry could not be loaded as the required type."),
                    TEXT("candidate_discovery"));
                OutError.SourceKind = TEXT("native_result");
                OutError.TargetPath = Path.ToString();
                OutError.SubjectId = BaseClass.GetPathName();
                OutError.ActualValue = MakeShared<FJsonValueString>(Path.ToString());
                OutError.ExpectedValue = MakeShared<FJsonValueString>(BaseClass.GetPathName());
                return false;
            }
            OutLoadedClasses.Emplace(Class);
        }
        return true;
    }
}
