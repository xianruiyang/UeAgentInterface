#include "Misc/AutomationTest.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
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
TSharedPtr<FJsonObject> FindPhysicalMaterialProperty(const TSharedPtr<FJsonObject>& Object, const FString& Name)
{
	if (!Object) return nullptr;
	for (const auto& Field : Object->Values)
	{
		if (Field.Value->Type != EJson::Object) continue;
		const auto Child = Field.Value->AsObject();
		if (Field.Key == Name && Child->HasField(TEXT("value"))) return Child;
		if (auto Found = FindPhysicalMaterialProperty(Child, Name)) return Found;
	}
	return nullptr;
}

TMap<FString, FString> PhysicalMaterialNonTargetState(UPhysicalMaterial& Material)
{
	TMap<FString, FString> State;
	for (TFieldIterator<FProperty> It(Material.GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Edit) || It->GetName() == TEXT("Friction") || It->GetName() == TEXT("Restitution")) continue;
		FString Value;
		It->ExportText_InContainer(0, Value, &Material, &Material, &Material, PPF_None);
		State.Add(It->GetName(), Value);
	}
	return State;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208PhysicalMaterialRootWorkspaceTest,
	"UAI.Root.T208.PhysicalMaterial.RootWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208PhysicalMaterialRootWorkspaceTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208PhysicalMaterial/PM_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-physical-material/native") / Token);
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args)
	{
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-physical-material"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")});
	};
	// Native creation supplies a fixture; editing goes through the public workspace commands.
	auto* Material = NewObject<UPhysicalMaterial>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone);
	Material->Friction = 0.25f;
	Material->Restitution = 0.1f;
	Material->Density = 2.5f;
	FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
	ON_SCOPE_EXIT
	{
		if (auto* Current = FindObject<UPhysicalMaterial>(nullptr, *ObjectPath); Current && Current->GetPackage()->IsDirty())
			TestTrue(TEXT("settle only this fixture after assertions"), UPackage::SavePackage(Current->GetPackage(), Current, *Filename, Save));
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Delete = MakeShared<FJsonObject>();
			Delete->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)});
			Delete->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true})
			{
				Delete->SetBoolField(TEXT("confirm"), Confirm);
				const auto Result = Submit(TEXT("asset.delete"), Delete);
				if (!Result.IsOk()) AddError(Result.GetErrorJson());
			}
			TestFalse(TEXT("own fixture package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	if (!TestTrue(TEXT("native fixture saved"), UPackage::SavePackage(Material->GetPackage(), Material, *Filename, Save))) return false;
	const auto NonTarget = PhysicalMaterialNonTargetState(*Material);
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
			if (auto Row = FindPhysicalMaterialProperty(Root, Name))
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
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), TEXT("Friction"), MakeShared<FJsonValueNumber>(0.75))
		|| !Edit(TEXT("edit"), TEXT("Restitution"), MakeShared<FJsonValueNumber>(0.6))) return false;
	Material = nullptr;
	const auto Applied = Import(TEXT("edit"));
	if (!TestTrue(TEXT("public PhysicalMaterial workspace import"), Applied.IsOk())) { AddError(Applied.GetErrorJson()); return false; }
	if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
	Material = LoadObject<UPhysicalMaterial>(nullptr, *ObjectPath);
	if (!TestTrue(TEXT("native values persist and non-target fields are preserved"), Material
		&& FMath::IsNearlyEqual(Material->Friction, 0.75f) && FMath::IsNearlyEqual(Material->Restitution, 0.6f)
		&& !Material->GetPackage()->IsDirty() && PhysicalMaterialNonTargetState(*Material).OrderIndependentCompareEqual(NonTarget))) return false;
	if (!Export(TEXT("invalid"))) return false;
	TArray<uint8> Before, After;
	if (!FFileHelper::LoadFileToArray(Before, *Filename) || !Edit(TEXT("invalid"), TEXT("Friction"), MakeShared<FJsonValueString>(TEXT("not-a-number")))) return false;
	const auto Rejected = Import(TEXT("invalid"));
	TestFalse(TEXT("invalid numeric value rejected"), Rejected.IsOk());
	TestTrue(TEXT("invalid input preserves memory and original disk"), FMath::IsNearlyEqual(Material->Friction, 0.75f)
		&& FMath::IsNearlyEqual(Material->Restitution, 0.6f) && !Material->GetPackage()->IsDirty()
		&& PhysicalMaterialNonTargetState(*Material).OrderIndependentCompareEqual(NonTarget)
		&& FFileHelper::LoadFileToArray(After, *Filename) && !Before.IsEmpty() && Before == After);
	TestTrue(TEXT("independent final workspace export"), Export(TEXT("readback")));
	return !HasAnyErrors();
}
