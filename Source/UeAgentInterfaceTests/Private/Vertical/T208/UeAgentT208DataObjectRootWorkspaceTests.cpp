#include "Misc/AutomationTest.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Vertical/T208/UeAgentT208DataObjectTestTypes.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentDataObjectEligibility.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindDataObjectProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindDataObjectProperty(Child, Name)) return Found;
	}
	return nullptr;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208DataObjectRootWorkspaceTest,
	"UAI.Root.T208.DataObject.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208DataObjectRootWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208DataObject/DA_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-data-object/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-data-object"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	TestTrue(TEXT("inherited metadata class eligible"), DomainAdapters::AssetDataSimpleEffect::IsEligibleDataObjectClass(UUeAgentT208RegisteredData::StaticClass()));
	TestFalse(TEXT("duplicate metadata IDs rejected"), DomainAdapters::AssetDataSimpleEffect::IsEligibleDataObjectClass(UUeAgentT208InvalidRegisteredData::StaticClass()));
	TestFalse(TEXT("bare DataAsset is not registered"), DomainAdapters::AssetDataSimpleEffect::IsEligibleDataObjectClass(UDataAsset::StaticClass()));
	const auto* Profile = UeAgentReflectionAuthoring::FindProfileByTypeId(TEXT("data_object"));
	TestTrue(TEXT("ordinary reflection does not require legacy metadata ID uniqueness"), Profile
		&& Profile->AcceptsConcreteClass(UUeAgentT208InvalidRegisteredData::StaticClass()));
	const FString MaterialPackage = PackageName + TEXT("_Material");
	const FString MaterialPath = MaterialPackage + TEXT(".") + FPackageName::GetLongPackageAssetName(MaterialPackage);
	const FString MaterialFilename = FPackageName::LongPackageNameToFilename(MaterialPackage, FPackageName::GetAssetPackageExtension());
	auto* Material = NewObject<UPhysicalMaterial>(CreatePackage(*MaterialPackage), *FPackageName::GetLongPackageAssetName(MaterialPackage), RF_Public | RF_Standalone);
	auto* Data = NewObject<UUeAgentT208RegisteredData>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	ON_SCOPE_EXIT
	{
		for (const auto& Path : {ObjectPath, MaterialPath})
		{
			const FString Package = FPackageName::ObjectPathToPackageName(Path);
			if (auto* Current = FindObject<UObject>(nullptr, *Path); Current && Current->GetPackage()->IsDirty())
				TestTrue(TEXT("settle only owned fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current,
					*FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension()), Save));
			if (!FPackageName::DoesPackageExist(Package)) continue;
			auto Delete = MakeShared<FJsonObject>();
			Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(Path)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				const auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
			TestFalse(TEXT("owned fixture package removed"), FPackageName::DoesPackageExist(Package));
		}
	};
	if (!TestTrue(TEXT("native fixtures saved"), UPackage::SavePackage(Material->GetPackage(), Material, *MaterialFilename, Save)
		&& UPackage::SavePackage(Data->GetPackage(), Data, *Filename, Save))) return false;
	auto Export = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath);
		Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args);
		if (!Result.IsOk()) AddError(Result.GetErrorJson());
		return Result.IsOk();
	};
	auto Edit = [&](const FString& Stage, const FString& Name, const TSharedRef<FJsonValue>& Value)
	{
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false);
		int32 Count = 0;
		for (const auto& File : Files)
		{
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) { AddError(Error); return false; }
			if (auto Row = FindDataObjectProperty(Root, Name))
			{
				Row->SetField(TEXT("value"), Value); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error)
					|| !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { AddError(Error); return false; }
			}
		}
		return TestEqual(TEXT("one editable property row"), Count, 1);
	};
	auto Import = [&](const FString& Stage)
	{
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source);
		return Submit(TEXT("content_graph.import_workspace"), Args);
	};
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("Title"), MakeShared<FJsonValueString>(TEXT("changed")))
		|| !Edit(TEXT("edit"), TEXT("Counter"), MakeShared<FJsonValueNumber>(8))
		|| !Edit(TEXT("edit"), TEXT("Unregistered"), MakeShared<FJsonValueNumber>(41))) return false;
	if (!Edit(TEXT("edit"), TEXT("Material"), MakeShared<FJsonValueString>(MaterialPath))) return false;
	Data = nullptr; Material = nullptr;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public DataObject workspace import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Data = LoadObject<UUeAgentT208RegisteredData>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("normal editable fields, metadata authorization and references persist"), Data
		&& Data->Title == TEXT("changed") && Data->Counter == 8 && Data->Unregistered == 41
		&& Data->Material && Data->Material->GetPathName() == MaterialPath && !Data->GetPackage()->IsDirty())) return false;
	for (bool WrongReference : {false, true})
	{
		const FString Stage = WrongReference ? TEXT("bad-reference") : TEXT("bad-number");
		if (!Export(Stage)) return false;
		TArray<uint8> Before, After;
		if (!FFileHelper::LoadFileToArray(Before, *Filename)) return false;
		if (!Edit(Stage, WrongReference ? TEXT("Material") : TEXT("Counter"), WrongReference
			? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(ObjectPath))
			: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(TEXT("not-a-number"))))) return false;
		AddExpectedError(WrongReference ? TEXT("UAI workspace operation: reference_not_found_or_wrong_class")
			: TEXT("UAI workspace operation: integer_out_of_range"), EAutomationExpectedErrorFlags::Contains, 1);
		const auto Rejected = Import(Stage);
		TestFalse(TEXT("bad value/reference rejected"), Rejected.IsOk());
		TestTrue(TEXT("rejection preserves original memory and disk"), Data->Title == TEXT("changed") && Data->Counter == 8
			&& Data->Unregistered == 41 && Data->Material && Data->Material->GetPathName() == MaterialPath
			&& !Data->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
	}
	// Ordinary reflected fields share the same eligibility through workspace and direct apply.
	UeAgentReflectionAuthoring::FAssetApplyRequest OutsideScope;
	OutsideScope.TypeId = TEXT("data_object"); OutsideScope.AssetPath = PackageName;
	OutsideScope.bSave = true; OutsideScope.bCompactValues = true;
	auto Patch = MakeShared<FJsonObject>(); Patch->SetStringField(TEXT("property_path"), TEXT("Unregistered"));
	Patch->SetNumberField(TEXT("value"), 99); OutsideScope.Properties.Add(MakeShared<FJsonValueObject>(Patch));
	UeAgentReflectionAuthoring::FAssetApplyResult ScopeResult;
	if (!TestTrue(TEXT("normal editable field uses common apply"), UeAgentReflectionAuthoring::ApplyAsset(OutsideScope, ScopeResult)))
	{
		AddError(ScopeResult.ErrorCode + TEXT(":") + ScopeResult.ErrorDetail);
		return false;
	}
	Data = LoadObject<UUeAgentT208RegisteredData>(nullptr, *ObjectPath);
	TestTrue(TEXT("common apply persists the ordinary editable field"), Data && Data->Unregistered == 99 && !Data->GetPackage()->IsDirty());
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}
