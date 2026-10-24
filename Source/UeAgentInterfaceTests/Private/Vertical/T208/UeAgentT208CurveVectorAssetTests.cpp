#include "Misc/AutomationTest.h"
#include "Curves/CurveVector.h"
#include "EditorFramework/AssetImportData.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "TestSupport/UeAgentPublicCommandTestHarness.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace
{
TSharedPtr<FJsonObject> FindVectorAssetCurve(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value) return nullptr;
	if (Value->Type == EJson::Array)
		for (const auto& Item : Value->AsArray()) if (auto Found = FindVectorAssetCurve(Item)) return Found;
	if (Value->Type != EJson::Object) return nullptr;
	FString Kind;
	if (Value->AsObject()->TryGetStringField(TEXT("curve_kind"), Kind) && Kind == TEXT("vector")) return Value->AsObject();
	for (const auto& Field : Value->AsObject()->Values) if (auto Found = FindVectorAssetCurve(Field.Value)) return Found;
	return nullptr;
}
bool SameVectorAsset(const UCurveVector* Asset, const TArray<FRichCurve>& Curves)
{
	if (!Asset || Curves.Num() != 3) return false;
	for (int32 I = 0; I < 3; ++I) if (!(Asset->FloatCurves[I] == Curves[I])) return false;
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208CurveVectorAssetTest,
	"UAI.Root.T208.CurveVector.AssetWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208CurveVectorAssetTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName = TEXT("/Game/UAI/Transient/T208CurveVector/CV_") + Token;
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-curve-vector-asset/native") / Token);
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-curve-vector"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	ON_SCOPE_EXIT
	{
		if (auto* Object = FindObject<UObject>(nullptr, *ObjectPath); Object && Object->GetPackage()->IsDirty())
		{
			FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
			TestTrue(TEXT("self fixture settles after restore assertions"), UPackage::SavePackage(Object->GetPackage(), Object, *Filename, Save));
		}
		if (FPackageName::DoesPackageExist(PackageName))
		{
			auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)}); Args->SetBoolField(TEXT("recursive"), false);
			for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); const auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
			TestFalse(TEXT("own package removed"), FPackageName::DoesPackageExist(PackageName));
		}
	};
	auto Create = MakeShared<FJsonObject>(); Create->SetStringField(TEXT("factory"), TEXT("Curve Vector")); Create->SetStringField(TEXT("target_ref"), ObjectPath);
	const auto Created = Submit(TEXT("asset.object.create"), Create);
	if (!TestTrue(TEXT("public native factory creates vector"), Created.IsOk())) { AddError(Created.GetErrorJson()); return false; }
	auto* Asset = LoadObject<UCurveVector>(nullptr, *ObjectPath);
	if (!Asset) return false;
	const FString ImportData = Asset->AssetImportData ? Asset->AssetImportData->GetSourceData().ToJson() : FString();
	auto NonTarget = [&](const UCurveVector* Value) { return Value && Value->AssetImportData
		&& Value->AssetImportData->GetOuter() == Value && Value->AssetImportData->GetSourceData().ToJson() == ImportData; };
	auto Export = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		const auto Result = Submit(TEXT("content_graph.export_workspace"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); return Result.IsOk(); };
	auto Edit = [&](const FString& Stage, TFunctionRef<void(TSharedPtr<FJsonObject>)> Change) {
		TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Count = 0;
		for (const auto& File : Files) {
			if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
			FString Text; TSharedPtr<FJsonObject> Root;
			if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
			if (auto Curve = FindVectorAssetCurve(MakeShared<FJsonValueObject>(Root))) {
				Change(Curve); ++Count;
				if (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
			}
		} return TestTrue(TEXT("one ordinary whole curve"), Count == 1); };
	auto Import = [&](const FString& Stage) {
		auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
		Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
	auto Reload = [&]() { Asset = nullptr; if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
		Asset = LoadObject<UCurveVector>(nullptr, *ObjectPath); return Asset != nullptr; };
	TArray<FRichCurve> Expected; Expected.SetNum(3);
	for (int32 I = 0; I < 3; ++I) {
		auto& Curve = Expected[I]; Curve.SetDefaultValue(0.15f + I); Curve.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(I); Curve.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(5 - I);
		for (float X : {-0.25f, 0.5f, 1.25f}) {
			auto& Key = Curve.GetKey(Curve.AddKey(X, X + I)); Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_User;
			Key.TangentWeightMode = RCTWM_WeightedBoth; Key.ArriveTangent = 0.2f + I; Key.LeaveTangent = 0.3f + I; Key.ArriveTangentWeight = 0.1f; Key.LeaveTangentWeight = 0.2f;
		}
	}
	if (!Export(TEXT("initialize")) || !Edit(TEXT("initialize"), [&](auto Curve) {
		int32 I = 0;
		for (const TCHAR* Name : {TEXT("x"), TEXT("y"), TEXT("z")}) {
			auto Scalar = UeAgentCurveJson::BuildFloatCurveJson(Expected[I++], {}, {}, true, {});
			auto Channel = Scalar->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"));
			for (const TCHAR* Field : {TEXT("pre_infinity_extrap"), TEXT("post_infinity_extrap")}) Channel->SetStringField(Field, Scalar->GetStringField(Field));
			Curve->GetObjectField(TEXT("channels"))->SetObjectField(Name, Channel);
		}
	})) return false;
	auto Result = Import(TEXT("initialize")); if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	if (!Reload() || !TestTrue(TEXT("public XYZ initialization persists exactly"), SameVectorAsset(Asset, Expected) && NonTarget(Asset))) return false;
	auto SetX = [](auto Curve, double Value) { for (const auto& Key : Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("x"))->GetArrayField(TEXT("keys"))) if (Key->AsObject()->GetNumberField(TEXT("time")) == 0.5) Key->AsObject()->SetNumberField(TEXT("value"), Value); };
	if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), [&](auto Curve) { SetX(Curve, 0.75); Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("x"))->SetStringField(TEXT("post_infinity_extrap"), TEXT("Cycle")); })) return false;
	Result = Import(TEXT("edit")); if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
	Expected[0].GetKey(Expected[0].FindKey(0.5f)).Value = 0.75f; Expected[0].PostInfinityExtrap = RCCE_Cycle;
	if (!Reload() || !TestTrue(TEXT("local key and extrap preserve all other native state"), SameVectorAsset(Asset, Expected) && NonTarget(Asset)) || !Export(TEXT("independent"))) return false;
	TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
	for (const FString Stage : {TEXT("invalid_enum"), TEXT("invalid_shape")}) {
		if (!Export(Stage) || !Edit(Stage, [&](auto Curve) { if (Stage == TEXT("invalid_enum")) Curve->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("y"))->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Invalid")); else Curve->SetStringField(TEXT("channels"), TEXT("invalid")); })) return false;
		TestFalse(TEXT("invalid input rejected before write"), Import(Stage).IsOk()); FFileHelper::LoadFileToArray(After, *Filename);
		TestTrue(TEXT("invalid preserves native and original file clean"), SameVectorAsset(Asset, Expected) && NonTarget(Asset) && Before == After && !Asset->GetPackage()->IsDirty());
	}
	Asset->FloatCurves[0].GetKey(Asset->FloatCurves[0].FindKey(0.5f)).Value = 0.85f; Asset->MarkPackageDirty();
	auto Dirty = Expected; Dirty[0] = Asset->FloatCurves[0]; auto Published = Dirty; Published[0].GetKey(Published[0].FindKey(0.5f)).Value = 0.95f;
	auto LateCurve = UeAgentCurveJson::BuildCurveAssetJson(Asset, true); SetX(LateCurve, 0.95);
	TWeakObjectPtr<UCurveVector> Old = Asset; Asset = nullptr; bool SawPublished = false;
	UeAgentReflectionAuthoring::FAssetApplyRequest Request; Request.TypeId = TEXT("curve_vector"); Request.AssetPath = PackageName; Request.bSave = true; Request.bCompactValues = true;
	Request.ProviderExtension = MakeShared<FJsonObject>(); Request.ProviderExtension->SetObjectField(TEXT("curve"), LateCurve);
	Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
		auto* Current = LoadObject<UCurveVector>(nullptr, *ObjectPath); TArray<uint8> Bytes;
		SawPublished = !Old.IsValid() && SameVectorAsset(Current, Published) && NonTarget(Current) && !Current->GetPackage()->IsDirty() && FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != Before;
		Failure.Diagnostic.Code = TEXT("t208_curve_vector_late_rejected"); return false;
	};
	UeAgentReflectionAuthoring::FAssetApplyResult Late;
	TestFalse(TEXT("same Reflection boundary late rejection"), UeAgentReflectionAuthoring::ApplyAsset(Request, Late));
	TestTrue(TEXT("late observes saved clean reloaded complete new state first"), SawPublished);
	TestTrue(TEXT("checkpoint restores after exact injected failure"), Late.Diagnostic.Code == TEXT("t208_curve_vector_late_rejected") && Late.bRollbackAttempted && Late.bRollbackSucceeded);
	Asset = LoadObject<UCurveVector>(nullptr, *ObjectPath); FFileHelper::LoadFileToArray(After, *Filename);
	TestTrue(TEXT("dirty memory restored independently"), SameVectorAsset(Asset, Dirty) && NonTarget(Asset) && Asset->GetPackage()->IsDirty());
	TestTrue(TEXT("original disk restored independently"), !Before.IsEmpty() && Before == After);
	return !HasAnyErrors();
}
