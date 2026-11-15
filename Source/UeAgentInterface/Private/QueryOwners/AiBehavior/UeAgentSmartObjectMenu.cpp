#include "QueryOwners/AiBehavior/UeAgentSmartObjectMenu.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "ClassViewerModule.h"
#include "ClassViewerProjectSettings.h"
#include "EditorClassUtils.h"
#include "Engine/Blueprint.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "PropertyEditorUtils.h"
#include "PropertyHandle.h"
#include "SClassViewer.h"
#include "Settings/ClassViewerSettings.h"
#include "Settings/StructViewerSettings.h"
#include "SmartObjectDefinition.h"
#include "StructViewerProjectSettings.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::QueryOwners::AiBehavior
{
	void VisitSmartObjectBehaviorTypes(USmartObjectDefinition& Definition, bool bSlot,
		TFunctionRef<void(const FString&, const FTopLevelAssetPath&)> Visit)
	{
		const UStruct* Owner = bSlot ? static_cast<const UStruct*>(FSmartObjectSlotDefinition::StaticStruct()) : Definition.GetClass();
		const auto* Array = FindFieldChecked<FArrayProperty>(Owner, bSlot ? TEXT("BehaviorDefinitions") : TEXT("DefaultBehaviorDefinitions"));
		const auto* Property = CastFieldChecked<FObjectPropertyBase>(Array->Inner);
		const UClass* Interface = Array->GetClassMetaData(TEXT("ObjectMustImplement"));
		TArray<const UClass*> Allowed, Disallowed;
		PropertyEditorUtils::GetAllowedAndDisallowedClasses({&Definition}, *Property, Allowed, Disallowed, false);
		const auto* Settings = GetDefault<UClassViewerSettings>();
		const auto* Project = GetDefault<UClassViewerProjectSettings>();
		const FString Developers = FPackageName::FilenameToLongPackageName(FPaths::GameDevelopersDir());
		const FString User = FPackageName::FilenameToLongPackageName(FPaths::GameUserDeveloperDir());
		auto Visible = [&](const FString& Path, const UClass* Loaded)
		{
			if (Path.StartsWith(Developers) && (Settings->DeveloperFolderType == EClassViewerDeveloperType::CVDT_None
				|| (Settings->DeveloperFolderType == EClassViewerDeveloperType::CVDT_CurrentUser && !Path.StartsWith(User)))) return false;
			if (!Settings->AllowedClasses.IsEmpty() && !Settings->AllowedClasses.Contains(Path)) return false;
			if (Settings->DisplayInternalClasses) return true;
			if (Project->InternalOnlyPaths.ContainsByPredicate([&](const FDirectoryPath& Dir) { return Path.StartsWith(Dir.Path); })) return false;
			return !Loaded || !Project->InternalOnlyClasses.ContainsByPredicate([&](const FSoftClassPath& Ref) {
				const UClass* Internal = Ref.ResolveClass();
				return Internal && Loaded->IsChildOf(Internal);
			});
		};
		constexpr EClassFlags Excluded = CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden | CLASS_HideDropDown | CLASS_NewerVersionExists;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(Property->PropertyClass) || It->HasAnyClassFlags(Excluded) || !It->HasAnyClassFlags(CLASS_EditInlineNew)
				|| FBlueprintSupport::IsClassPlaceholder(*It) || FKismetEditorUtilities::IsClassABlueprintSkeleton(*It) || !Visible(It->GetPathName(), *It)
				|| (It->ClassWithin && !Definition.IsA(It->ClassWithin)) || (Interface && !It->ImplementsInterface(Interface))
				|| Disallowed.ContainsByPredicate([&](const UClass* Class) { return It->IsChildOf(Class); })
				|| (!Allowed.IsEmpty() && !Allowed.ContainsByPredicate([&](const UClass* Class) { return It->IsChildOf(Class); }))) continue;
			Visit(It->GetDisplayNameText().ToString(), It->GetClassPathName());
		}
		auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		auto BlueprintAsset = [&](const FTopLevelAssetPath& ClassPath)
		{
			FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(ClassPath), true);
			if (!Asset.IsValid() && ClassPath.GetAssetName().ToString().EndsWith(TEXT("_C")))
				Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(ClassPath.ToString().LeftChop(2)), true);
			return Asset;
		};
		TSet<FTopLevelAssetPath> Derived;
		Registry.GetDerivedClassNames({Property->PropertyClass->GetClassPathName()}, {}, Derived);
		for (const FTopLevelAssetPath& Path : Derived)
		{
			if (FindObject<UClass>(nullptr, *Path.ToString()) || !Visible(Path.ToString(), nullptr)) continue;
			const FAssetData Asset = BlueprintAsset(Path);
			if (!Asset.IsValid()) continue;
			const uint32 Flags = Asset.GetTagValueRef<uint32>(FBlueprintTags::ClassFlags);
			if (!(Flags & CLASS_EditInlineNew) || (Flags & Excluded)) continue;
			TArray<FTopLevelAssetPath> Ancestors;
			Registry.GetAncestorClassNames(Path, Ancestors);
			const auto Under = [&](const UClass* Class) { return Path == Class->GetClassPathName() || Ancestors.Contains(Class->GetClassPathName()); };
			if (Disallowed.ContainsByPredicate(Under) || (!Allowed.IsEmpty() && !Allowed.ContainsByPredicate(Under))) continue;
			const UClass* LoadedParent = nullptr;
			for (const auto& Ancestor : Ancestors)
				if ((LoadedParent = FindObject<UClass>(nullptr, *Ancestor.ToString()))) break;
			if (LoadedParent && LoadedParent->ClassWithin && !Definition.IsA(LoadedParent->ClassWithin)) continue;
			if (Interface && (!LoadedParent || !LoadedParent->ImplementsInterface(Interface)))
			{
				const FString Required = Interface->GetPathName();
				auto Implements = [&](const FAssetData& BP) {
					TArray<FString> Paths;
					if (BP.IsValid()) FEditorClassUtils::GetImplementedInterfaceClassPathsFromAsset(BP, Paths);
					return Paths.Contains(Required);
				};
				if (!Implements(Asset) && !Ancestors.ContainsByPredicate([&](const FTopLevelAssetPath& Parent) {
					return Implements(BlueprintAsset(Parent));
				})) continue;
			}
			FString Name = Asset.GetTagValueRef<FString>(FBlueprintTags::BlueprintDisplayName);
			if (Name.IsEmpty()) Name = Asset.AssetName.ToString();
			Visit(Name, Path);
		}
	}

	bool CanAssignSmartObjectBehavior(USmartObjectDefinition& Definition, int32 Slot, int32 Index, const UClass& Class)
	{
		auto& Editor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		const auto Generator = Editor.CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
		Generator->SetObjects({&Definition});
		const TCHAR* Name = Slot == INDEX_NONE ? TEXT("DefaultBehaviorDefinitions") : TEXT("BehaviorDefinitions");
		TArray<TSharedRef<IDetailTreeNode>> Rows = Generator->GetRootTreeNodes();
		while (!Rows.IsEmpty())
		{
			const auto Row = Rows.Pop();
			const auto Handle = Row->CreatePropertyHandle();
			if (Handle && Handle->GetProperty() && Handle->GetProperty()->GetFName() == Name && Handle->AsArray())
			{
				const auto Parent = Handle->GetParentHandle();
				if (Slot == INDEX_NONE || (Parent && Parent->GetIndexInArray() == Slot))
				{
					const auto Element = Handle->AsArray()->GetElement(Index);
					if (!Element->IsValidHandle()) return false;
					const auto Picker = PropertyCustomizationHelpers::MakeEditInlineObjectClassPicker(Element, FOnClassPicked());
					return StaticCastSharedRef<SClassViewer>(Picker)->IsClassAllowed(&Class);
				}
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Row->GetChildren(Children);
			Rows.Append(Children);
		}
		return false;
	}

	void VisitSmartObjectDataMenu(const USmartObjectDefinition& Definition, bool bSlot,
		TFunctionRef<void(UScriptStruct&)> Visit)
	{
		const FProperty* Data = FindFieldChecked<FProperty>(FSmartObjectDefinitionDataProxy::StaticStruct(), TEXT("Data"));
		const UStruct* Owner = bSlot ? static_cast<const UStruct*>(FSmartObjectSlotDefinition::StaticStruct()) : Definition.GetClass();
		const FProperty* Array = FindFieldChecked<FArrayProperty>(Owner, TEXT("DefinitionData"));
		const FProperty* Slots = bSlot ? FindFieldChecked<FArrayProperty>(Definition.GetClass(), TEXT("Slots")) : nullptr;
		TArray<const UScriptStruct*> Disallowed;
		for (const FProperty* Property : {Data, Array, Slots})
		{
			if (!Property) continue;
			const FString& Spec = Property->GetMetaData(TEXT("DisallowedStructs"));
			if (Spec.IsEmpty()) continue;
			TArray<FString> Paths;
			Spec.ParseIntoArray(Paths, TEXT(","));
			for (const FString& Path : Paths)
				if (const auto* Type = LoadObject<UScriptStruct>(nullptr, *Path)) Disallowed.Add(Type);
			break;
		}
		const auto* Base = FSmartObjectDefinitionData::StaticStruct();
		const auto* Settings = GetDefault<UStructViewerSettings>();
		const auto* Project = GetDefault<UStructViewerProjectSettings>();
		const bool bOptions = !Data->HasMetaData(TEXT("HideViewOptions"));
		const FString Developers = FPackageName::FilenameToLongPackageName(FPaths::GameDevelopersDir());
		const FString User = FPackageName::FilenameToLongPackageName(FPaths::GameUserDeveloperDir());
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->GetOutermost() == GetTransientPackage() || !It->IsChildOf(Base)
				|| (*It == Base ? Data->HasMetaData(TEXT("ExcludeBaseStruct")) : It->HasMetaData(TEXT("Hidden")))
				|| Disallowed.ContainsByPredicate([&](const UScriptStruct* Type) { return It->IsChildOf(Type); })) continue;
			if (bOptions)
			{
				const FString Path = It->GetPathName();
				if (Path.StartsWith(Developers) && (Settings->DeveloperFolderType == EStructViewerDeveloperType::SVDT_None
					|| (Settings->DeveloperFolderType == EStructViewerDeveloperType::SVDT_CurrentUser && !Path.StartsWith(User)))) continue;
				if (!Settings->DisplayInternalStructs && (Project->InternalOnlyPaths.ContainsByPredicate(
						[&](const FDirectoryPath& Directory) { return Path.StartsWith(Directory.Path); })
					|| Project->InternalOnlyStructs.ContainsByPredicate(
						[&](const auto& Type) { return Type.Get() && It->IsChildOf(Type.Get()); }))) continue;
			}
			Visit(**It);
		}
	}
}
