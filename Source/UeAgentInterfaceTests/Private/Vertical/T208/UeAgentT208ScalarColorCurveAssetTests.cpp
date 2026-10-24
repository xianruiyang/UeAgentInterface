#include "Misc/AutomationTest.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
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
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Vertical/T85/UeAgentT85FixtureLifecycle.h"

namespace
{
TSharedPtr<FJsonObject> FindAssetCurveRow(const TSharedPtr<FJsonValue>& Value, const FString& Kind)
{
	if (!Value) return nullptr;
	if (Value->Type == EJson::Array) for (const auto& Item : Value->AsArray()) if (auto Found = FindAssetCurveRow(Item, Kind)) return Found;
	if (Value->Type != EJson::Object) return nullptr;
	FString Actual;
	if (Value->AsObject()->TryGetStringField(TEXT("curve_kind"), Actual) && Actual == Kind) return Value->AsObject();
	for (const auto& Field : Value->AsObject()->Values) if (auto Found = FindAssetCurveRow(Field.Value, Kind)) return Found;
	return nullptr;
}
TSharedPtr<FJsonObject> FindAssetNamedRow(const TSharedPtr<FJsonValue>& Value, const FString& Name)
{
	if (!Value || Value->Type != EJson::Object) return nullptr;
	for (const auto& Field : Value->AsObject()->Values) {
		if (Field.Key == Name && Field.Value->Type == EJson::Object && Field.Value->AsObject()->HasField(TEXT("value"))) return Field.Value->AsObject();
		if (auto Found = FindAssetNamedRow(Field.Value, Name)) return Found;
	}
	return nullptr;
}
TArray<FRichCurve> NativeCurves(UCurveBase* Asset)
{
	TArray<FRichCurve> Result;
	if (Asset) for (const auto& Curve : Asset->GetCurves()) Result.Add(*static_cast<const FRichCurve*>(Curve.CurveToEdit));
	return Result;
}
TArray<float> OtherAdjustments(const UCurveLinearColor* Asset)
{
	return {Asset->AdjustSaturation, Asset->AdjustBrightness, Asset->AdjustBrightnessCurve,
		Asset->AdjustVibrance, Asset->AdjustMinAlpha, Asset->AdjustMaxAlpha};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208ScalarColorCurveAssetTest,
	"UAI.Root.T208.CurveAssets.FloatAndColorWorkspace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentT208ScalarColorCurveAssetTest::RunTest(const FString&)
{
	using namespace UE::UAI;
	FString Error;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	if (!Module::CommandRegistration::Active::TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error)) { AddError(Error); return false; }
	auto Submit = [&](const FString& Command, const TSharedRef<FJsonObject>& Args) {
		return Tests::PublicCommand::SubmitActiveCommandWithScopes(Registry, Dispatch, Command, Args,
			TEXT("t208-scalar-color"), {TEXT("uai:query"), TEXT("uai:export"), TEXT("uai:operate")}); };
	for (bool bColor : {false, true})
	{
		const FString Kind = bColor ? TEXT("linear_color") : TEXT("float");
		const FString ProfileId = TEXT("curve_") + Kind;
		const TArray<FString> Names = bColor ? TArray<FString>{TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")} : TArray<FString>{TEXT("value")};
		const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = TEXT("/Game/UAI/Transient/T208ScalarColorCurve/C_") + Token;
		const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName);
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("tmp/TROOT-UAI-208-curve-float-color-assets/native") / Kind / Token);
		auto Save = [&](UObject* Object) { FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
			return UPackage::SavePackage(Object->GetPackage(), Object, *Filename, Args); };
		ON_SCOPE_EXIT {
			if (auto* Object = FindObject<UObject>(nullptr, *ObjectPath); Object && Object->GetPackage()->IsDirty()) TestTrue(TEXT("own fixture settles after assertions"), Save(Object));
			if (FPackageName::DoesPackageExist(PackageName)) {
				auto Args = MakeShared<FJsonObject>(); Args->SetArrayField(TEXT("targets"), {MakeShared<FJsonValueString>(ObjectPath)}); Args->SetBoolField(TEXT("recursive"), false);
				for (bool Confirm : {false, true}) { Args->SetBoolField(TEXT("confirm"), Confirm); auto Result = Submit(TEXT("asset.delete"), Args); if (!Result.IsOk()) AddError(Result.GetErrorJson()); }
				TestFalse(TEXT("own curve removed"), FPackageName::DoesPackageExist(PackageName));
			}
		};
		auto Create = MakeShared<FJsonObject>(); Create->SetStringField(TEXT("factory"), bColor ? TEXT("Curve Linear Color") : TEXT("Curve Float")); Create->SetStringField(TEXT("target_ref"), ObjectPath);
		auto Result = Submit(TEXT("asset.object.create"), Create); if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
		UCurveBase* Asset = LoadObject<UCurveBase>(nullptr, *ObjectPath);
		if (!Asset || (bColor ? !Asset->IsA<UCurveLinearColor>() : !Asset->IsA<UCurveFloat>())) return false;
		// Discriminating native non-target setup, never a replacement for public creation or curve editing.
		if (auto* Scalar = Cast<UCurveFloat>(Asset)) Scalar->bIsEventCurve = true;
		if (auto* Color = Cast<UCurveLinearColor>(Asset)) {
			Color->AdjustSaturation = .7f; Color->AdjustBrightness = .9f; Color->AdjustBrightnessCurve = .8f;
			Color->AdjustVibrance = .1f; Color->AdjustMinAlpha = .2f; Color->AdjustMaxAlpha = .85f;
		}
		if (!Save(Asset)) return false;
		const FString ImportData = Asset->AssetImportData->GetSourceData().ToJson();
		const TArray<float> Adjustments = bColor ? OtherAdjustments(CastChecked<UCurveLinearColor>(Asset)) : TArray<float>();
		auto NonTarget = [&](UCurveBase* Value) {
			return Value && Value->AssetImportData && Value->AssetImportData->GetOuter() == Value
				&& Value->AssetImportData->GetSourceData().ToJson() == ImportData
				&& (bColor ? OtherAdjustments(CastChecked<UCurveLinearColor>(Value)) == Adjustments : CastChecked<UCurveFloat>(Value)->bIsEventCurve); };
		auto Export = [&](const FString& Stage) {
			auto Args = MakeShared<FJsonObject>(); Args->SetStringField(TEXT("target"), ObjectPath); Args->SetStringField(TEXT("workspace_path"), Directory / Stage); Args->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
			auto Out = Submit(TEXT("content_graph.export_workspace"), Args); if (!Out.IsOk()) AddError(Out.GetErrorJson()); return Out.IsOk(); };
		auto Edit = [&](const FString& Stage, TFunctionRef<void(TSharedPtr<FJsonObject>)> ChangeCurve, TOptional<double> Hue = {}) {
			TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *(Directory / Stage), TEXT("*.hjson"), true, false); int32 Curves = 0, Roots = 0;
			for (const auto& File : Files) {
				if (File.Contains(TEXT("/.uai/")) || File.Contains(TEXT("\\.uai\\")) || File.EndsWith(TEXT("definitions.hjson"))) continue;
				FString Text; TSharedPtr<FJsonObject> Root; bool Changed = false;
				if (!FFileHelper::LoadFileToString(Text, *File) || !FUeAgentStructuredHjsonParser::ParseObject(Text, File, Root, Error)) return false;
				if (auto Curve = FindAssetCurveRow(MakeShared<FJsonValueObject>(Root), Kind)) { ChangeCurve(Curve); ++Curves; Changed = true; }
				if (Hue.IsSet()) if (auto Row = FindAssetNamedRow(MakeShared<FJsonValueObject>(Root), TEXT("AdjustHue"))) { Row->SetNumberField(TEXT("value"), Hue.GetValue()); ++Roots; Changed = true; }
				if (Changed && (!FUeAgentStructuredHjsonFormatter::TryWriteObject(Root.ToSharedRef(), Text, Error) || !FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
			}
			return TestTrue(TEXT("one native curve state and requested root row"), Curves == 1 && (!Hue.IsSet() || Roots == 1)); };
		auto Import = [&](const FString& Stage) {
			auto Args = MakeShared<FJsonObject>(); auto Source = MakeShared<FJsonObject>(); Source->SetStringField(TEXT("kind"), TEXT("directory")); Source->SetStringField(TEXT("path"), Directory / Stage);
			Args->SetStringField(TEXT("target"), ObjectPath); Args->SetObjectField(TEXT("source"), Source); return Submit(TEXT("content_graph.import_workspace"), Args); };
		auto Reload = [&]() { Asset = nullptr; if (!T85::FixtureLifecycle::TryUnloadPackageForReadback(PackageName, Error)) { AddError(Error); return false; }
			Asset = LoadObject<UCurveBase>(nullptr, *ObjectPath); return Asset != nullptr; };
		TArray<FRichCurve> Expected; Expected.SetNum(Names.Num());
		for (int32 I = 0; I < Expected.Num(); ++I) {
			auto& Curve = Expected[I]; Curve.SetDefaultValue(.15f + I); Curve.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(I); Curve.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(5 - I);
			for (float X : {-.25f, .5f, 1.25f}) { auto& Key = Curve.GetKey(Curve.AddKey(X, X + I)); Key.InterpMode = RCIM_Cubic; Key.TangentMode = RCTM_User;
				Key.TangentWeightMode = RCTWM_WeightedBoth; Key.ArriveTangent = .2f + I; Key.LeaveTangent = .3f + I; Key.ArriveTangentWeight = .1f; Key.LeaveTangentWeight = .2f; }
		}
		if (!Export(TEXT("initialize")) || !Edit(TEXT("initialize"), [&](auto Curve) {
			for (int32 I = 0; I < Names.Num(); ++I) {
				auto Scalar = UeAgentCurveJson::BuildFloatCurveJson(Expected[I], {}, {}, true, {});
				auto Channel = Scalar->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"));
				for (const TCHAR* Field : {TEXT("pre_infinity_extrap"), TEXT("post_infinity_extrap")}) {
					if (bColor) Channel->SetStringField(Field, Scalar->GetStringField(Field)); else Curve->SetStringField(Field, Scalar->GetStringField(Field));
				}
				Curve->GetObjectField(TEXT("channels"))->SetObjectField(Names[I], Channel);
			}
		}, bColor ? TOptional<double>(20.) : TOptional<double>())) return false;
		Result = Import(TEXT("initialize")); if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
		if (!Reload() || !TestTrue(TEXT("public curve initialization and non-target persist"), NativeCurves(Asset) == Expected && NonTarget(Asset)
			&& (!bColor || CastChecked<UCurveLinearColor>(Asset)->AdjustHue == 20.f))) return false;
		auto SetFirst = [&](auto Curve, double Value) { for (const auto& Key : Curve->GetObjectField(TEXT("channels"))->GetObjectField(Names[0])->GetArrayField(TEXT("keys"))) if (Key->AsObject()->GetNumberField(TEXT("time")) == .5) Key->AsObject()->SetNumberField(TEXT("value"), Value); };
		if (!Export(TEXT("edit")) || !Edit(TEXT("edit"), [&](auto Curve) {
			SetFirst(Curve, .75); (bColor ? Curve->GetObjectField(TEXT("channels"))->GetObjectField(Names[0]) : Curve)->SetStringField(TEXT("post_infinity_extrap"), TEXT("Cycle"));
		}, bColor ? TOptional<double>(35.) : TOptional<double>())) return false;
		Result = Import(TEXT("edit")); if (!Result.IsOk()) { AddError(Result.GetErrorJson()); return false; }
		Expected[0].GetKey(Expected[0].FindKey(.5f)).Value = .75f; Expected[0].PostInfinityExtrap = RCCE_Cycle;
		if (!Reload() || !TestTrue(TEXT("public key/extrap and color root mixed edit persist"), NativeCurves(Asset) == Expected && NonTarget(Asset)
			&& (!bColor || CastChecked<UCurveLinearColor>(Asset)->AdjustHue == 35.f)) || !Export(TEXT("independent"))) return false;
		TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
		for (const FString Stage : {TEXT("invalid_enum"), TEXT("invalid_kind")}) {
			if (!Export(Stage) || !Edit(Stage, [&](auto Curve) {
				if (Stage == TEXT("invalid_kind")) Curve->SetStringField(TEXT("curve_kind"), TEXT("wrong"));
				else (bColor ? Curve->GetObjectField(TEXT("channels"))->GetObjectField(Names[0]) : Curve)->SetStringField(TEXT("pre_infinity_extrap"), TEXT("Invalid"));
			})) return false;
			TestFalse(TEXT("invalid input rejected"), Import(Stage).IsOk()); FFileHelper::LoadFileToArray(After, *Filename);
			TestTrue(TEXT("invalid preserves clean native root/curve and disk"), NativeCurves(Asset) == Expected && NonTarget(Asset) && Before == After && !Asset->GetPackage()->IsDirty()
				&& (!bColor || CastChecked<UCurveLinearColor>(Asset)->AdjustHue == 35.f));
		}
		if (!bColor) continue;
		auto* Color = CastChecked<UCurveLinearColor>(Asset); Color->FloatCurves[0].GetKey(Color->FloatCurves[0].FindKey(.5f)).Value = .85f; Color->AdjustHue = 45.f; Color->MarkPackageDirty();
		const auto Dirty = NativeCurves(Asset); auto Published = Dirty; Published[0].GetKey(Published[0].FindKey(.5f)).Value = .95f;
		auto LateCurve = UeAgentCurveJson::BuildCurveAssetJson(Asset, true); SetFirst(LateCurve, .95);
		TWeakObjectPtr<UCurveBase> Old = Asset; Asset = nullptr; Color = nullptr; bool SawPublished = false;
		UeAgentReflectionAuthoring::FAssetApplyRequest Request; Request.TypeId = ProfileId; Request.AssetPath = PackageName; Request.bSave = true; Request.bCompactValues = true; Request.bEmitEditorNotifications = true;
		Request.ProviderExtension = MakeShared<FJsonObject>(); Request.ProviderExtension->SetObjectField(TEXT("curve"), LateCurve);
		auto RootEdit = MakeShared<FJsonObject>(); RootEdit->SetStringField(TEXT("property_path"), TEXT("AdjustHue")); RootEdit->SetNumberField(TEXT("value"), 55.); Request.Properties.Add(MakeShared<FJsonValueObject>(RootEdit));
		Request.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure) {
			auto* Current = LoadObject<UCurveLinearColor>(nullptr, *ObjectPath); TArray<uint8> Bytes;
			SawPublished = !Old.IsValid() && NativeCurves(Current) == Published && NonTarget(Current) && Current->AdjustHue == 55.f && !Current->GetPackage()->IsDirty()
				&& FFileHelper::LoadFileToArray(Bytes, *Filename) && Bytes != Before;
			Failure.Diagnostic.Code = TEXT("t208_color_curve_mixed_late_rejected"); return false;
		};
		UeAgentReflectionAuthoring::FAssetApplyResult Late;
		TestFalse(TEXT("mixed Color late rejection in shared Reflection boundary"), UeAgentReflectionAuthoring::ApplyAsset(Request, Late));
		TestTrue(TEXT("saved clean reloaded curve plus root observed before refusal"), SawPublished);
		TestTrue(TEXT("checkpoint succeeds after injected refusal"), Late.Diagnostic.Code == TEXT("t208_color_curve_mixed_late_rejected") && Late.bRollbackAttempted && Late.bRollbackSucceeded);
		Asset = LoadObject<UCurveLinearColor>(nullptr, *ObjectPath); FFileHelper::LoadFileToArray(After, *Filename);
		TestTrue(TEXT("dirty mixed curve/root memory restored"), NativeCurves(Asset) == Dirty && NonTarget(Asset) && CastChecked<UCurveLinearColor>(Asset)->AdjustHue == 45.f && Asset->GetPackage()->IsDirty());
		TestTrue(TEXT("original mixed-state disk restored independently"), !Before.IsEmpty() && Before == After);
	}
	return !HasAnyErrors();
}
