// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Deformer.h"

#include "UeAgentCurveJson.h"
#include "UeAgentJsonDiagnostics.h"

#include "Animation/MeshDeformerCollection.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "GeometryCache.h"
#include "GeometryCacheMeshData.h"
#include "GeometryCacheTrack.h"
#include "IOptimusComputeKernelProvider.h"
#include "IOptimusDataInterfaceProvider.h"
#include "IOptimusShaderTextProvider.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusDiagnostic.h"
#include "OptimusExecutionDomain.h"
#include "OptimusFunctionNodeGraph.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodeLink.h"
#include "OptimusNodePin.h"
#include "OptimusResourceDescription.h"
#include "OptimusVariableDescription.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace UeAgentDeformerOps
{
	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}
		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UClass* LoadClassByPath(const FString& ClassPath)
	{
		if (ClassPath.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		return LoadObject<UClass>(nullptr, *ClassPath);
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool IsPluginEnabled(const FString& PluginName)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		return Plugin.IsValid() && Plugin->IsEnabled();
	}

	static TSharedPtr<FJsonObject> MakePluginStatusJson()
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("GeometryCache"), IsPluginEnabled(TEXT("GeometryCache")));
		Obj->SetBoolField(TEXT("AlembicImporter"), IsPluginEnabled(TEXT("AlembicImporter")));
		Obj->SetBoolField(TEXT("GeometryCacheAbcFile"), IsPluginEnabled(TEXT("GeometryCacheAbcFile")));
		Obj->SetBoolField(TEXT("DeformerGraph"), IsPluginEnabled(TEXT("DeformerGraph")));
		Obj->SetBoolField(TEXT("MLDeformerFramework"), IsPluginEnabled(TEXT("MLDeformerFramework")));
		Obj->SetBoolField(TEXT("NearestNeighborModel"), IsPluginEnabled(TEXT("NearestNeighborModel")));
		Obj->SetBoolField(TEXT("NeuralMorphModel"), IsPluginEnabled(TEXT("NeuralMorphModel")));
		Obj->SetBoolField(TEXT("VertexDeltaModel"), IsPluginEnabled(TEXT("VertexDeltaModel")));
		return Obj;
	}

	static TArray<FString> GetDefaultClassPaths(const FString& Profile)
	{
		if (Profile == TEXT("geometry_cache"))
		{
			return { TEXT("/Script/GeometryCache.GeometryCache") };
		}
		if (Profile == TEXT("deformer_graph"))
		{
			return { TEXT("/Script/OptimusCore.OptimusDeformer") };
		}
		if (Profile == TEXT("mesh_deformer_collection"))
		{
			return { TEXT("/Script/Engine.MeshDeformerCollection") };
		}
		if (Profile == TEXT("ml_deformer"))
		{
			return {
				TEXT("/Script/NearestNeighborModel.NearestNeighborModel"),
				TEXT("/Script/NeuralMorphModel.NeuralMorphModel"),
				TEXT("/Script/VertexDeltaModel.VertexDeltaModel")
			};
		}
		return {};
	}

	static FString ResolveFirstAvailableClassPath(const FString& Profile, TArray<TSharedPtr<FJsonValue>>& OutClasses)
	{
		for (const FString& ClassPath : GetDefaultClassPaths(Profile))
		{
			UClass* Class = LoadClassByPath(ClassPath);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("class_path"), ClassPath);
			Item->SetBoolField(TEXT("available"), Class != nullptr);
			OutClasses.Add(MakeShared<FJsonValueObject>(Item));
			if (Class)
			{
				return ClassPath;
			}
		}
		return FString();
	}

	static TSharedPtr<FJsonObject> BuildPropertySummaryJson(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Properties;
		if (!Asset)
		{
			Obj->SetArrayField(TEXT("properties"), Properties);
			return Obj;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIterationFlags::IncludeSuper); It && Count < 200; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			FString ValueText;
			Property->ExportText_InContainer(0, ValueText, Asset, Asset, Asset, PPF_None);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Property->GetName());
			Item->SetStringField(TEXT("property_name"), Property->GetName());
			Item->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Item->SetStringField(TEXT("value_text"), ValueText);
			Item->SetBoolField(TEXT("can_edit"), Property->HasAnyPropertyFlags(CPF_Edit));
			Properties.Add(MakeShared<FJsonValueObject>(Item));
			++Count;
		}
		Obj->SetArrayField(TEXT("properties"), Properties);
		Obj->SetNumberField(TEXT("property_count"), Count);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildPropertyExportJson(UObject* Asset, const FString& Profile)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_properties.v1"));
		Root->SetStringField(TEXT("profile"), Profile);
		Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Root->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		if (Asset)
		{
			Root->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Asset->GetOutermost()->GetName()));
			Root->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Root->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		}

		TArray<TSharedPtr<FJsonValue>> Properties;
		if (Asset)
		{
			int32 Count = 0;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIterationFlags::IncludeSuper); It && Count < 200; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				FString ValueText;
				Property->ExportText_InContainer(0, ValueText, Asset, Asset, Asset, PPF_None);
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("property_name"), Property->GetName());
				Item->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
				Item->SetStringField(TEXT("value_text"), ValueText);
				Item->SetBoolField(TEXT("apply"), false);
				Item->SetStringField(
					TEXT("edit_policy"),
					Property->HasAnyPropertyFlags(CPF_Edit) ? TEXT("explicit_apply_only") : TEXT("readonly_or_internal"));
				TSharedPtr<FJsonObject> CurveJson;
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Asset);
				if (ValuePtr && UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr, CurveJson) && CurveJson.IsValid())
				{
					Item->SetObjectField(TEXT("curve_json"), CurveJson);
					Item->SetObjectField(TEXT("value_json"), CurveJson);
					Item->SetStringField(TEXT("value_json_schema"), UeAgentCurveJson::SchemaName);
				}
				Properties.Add(MakeShared<FJsonValueObject>(Item));
				++Count;
			}
		}
		Root->SetArrayField(TEXT("properties"), Properties);
		Root->SetNumberField(TEXT("property_count"), Properties.Num());
		Root->SetStringField(TEXT("apply_rule"), TEXT("Only entries with apply=true, or command parameter apply_all_properties=true, are sent to asset_apply_property_json."));
		return Root;
	}

	static TSharedPtr<FJsonObject> BuildAssetInfoJson(UObject* Asset, const FString& Profile)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		Obj->SetBoolField(TEXT("asset_found"), Asset != nullptr);
		if (Asset)
		{
			Obj->SetStringField(TEXT("asset_path"), NormalizeAssetPath(Asset->GetOutermost()->GetName()));
			Obj->SetStringField(TEXT("object_path"), Asset->GetPathName());
			Obj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
			Obj->SetBoolField(TEXT("dirty"), Asset->GetOutermost()->IsDirty());
			Obj->SetObjectField(TEXT("property_summary"), BuildPropertySummaryJson(Asset));
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector3f& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeBoxJson(const FBox3f& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		const bool bIsValid = Box.IsValid != 0;
		Obj->SetBoolField(TEXT("is_valid"), bIsValid);
		if (bIsValid)
		{
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		}
		return Obj;
	}

	static int32 GetSkeletalMeshLod0VertexCount(USkeletalMesh* SkeletalMesh)
	{
		const FSkeletalMeshModel* ImportedModel = SkeletalMesh ? SkeletalMesh->GetImportedModel() : nullptr;
		if (ImportedModel && ImportedModel->LODModels.IsValidIndex(0))
		{
			return ImportedModel->LODModels[0].NumVertices;
		}
		return INDEX_NONE;
	}

	static int32 GetGeometryCacheVertexCountAtTime(UGeometryCache* Cache, float TimeSeconds)
	{
		if (!Cache)
		{
			return INDEX_NONE;
		}
		TArray<FGeometryCacheMeshData> MeshDataAtTime;
		Cache->GetMeshDataAtTime(TimeSeconds, MeshDataAtTime);
		if (MeshDataAtTime.Num() == 0)
		{
			return INDEX_NONE;
		}
		int32 TotalVertexCount = 0;
		for (const FGeometryCacheMeshData& MeshData : MeshDataAtTime)
		{
			TotalVertexCount += MeshData.Positions.Num();
		}
		return TotalVertexCount;
	}

	static TSharedPtr<FJsonObject> BuildGeometryCacheFrameSampleJson(UGeometryCache* Cache, float TimeSeconds, TArray<TSharedPtr<FJsonValue>>& OutIssues)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Cache)
		{
			UeAgentJsonDiagnostics::AddIssue(OutIssues, TEXT("error"), TEXT("geometry_cache_not_found"), TEXT("asset_path"), TEXT("Geometry Cache asset could not be loaded."));
			return Obj;
		}

		const int32 StartFrame = Cache->GetStartFrame();
		const int32 EndFrame = Cache->GetEndFrame();
		const int32 FrameCount = EndFrame >= StartFrame ? EndFrame - StartFrame + 1 : 0;
		const float Duration = FMath::Max(Cache->CalculateDuration(), 0.0f);
		const float ClampedTime = FMath::Clamp(TimeSeconds, 0.0f, Duration);
		const float FrameAlpha = Duration > KINDA_SMALL_NUMBER ? ClampedTime / Duration : 0.0f;
		const int32 ApproxFrame = FrameCount > 1
			? FMath::Clamp(StartFrame + FMath::RoundToInt(FrameAlpha * static_cast<float>(FrameCount - 1)), StartFrame, EndFrame)
			: StartFrame;

		TArray<FGeometryCacheMeshData> MeshDataAtTime;
		Cache->GetMeshDataAtTime(ClampedTime, MeshDataAtTime);

		TArray<TSharedPtr<FJsonValue>> MeshesJson;
		FBox3f CombinedBounds(EForceInit::ForceInit);
		int32 TotalVertexCount = 0;
		int32 TotalIndexCount = 0;
		int32 TotalTriangleCount = 0;
		bool bHasNormals = false;
		bool bHasUvs = false;

		for (int32 MeshIndex = 0; MeshIndex < MeshDataAtTime.Num(); ++MeshIndex)
		{
			const FGeometryCacheMeshData& MeshData = MeshDataAtTime[MeshIndex];
			const int32 VertexCount = MeshData.Positions.Num();
			const int32 IndexCount = MeshData.Indices.Num();
			const int32 TriangleCount = IndexCount / 3;
			TotalVertexCount += VertexCount;
			TotalIndexCount += IndexCount;
			TotalTriangleCount += TriangleCount;
			if (MeshData.BoundingBox.IsValid != 0)
			{
				CombinedBounds += MeshData.BoundingBox;
			}
			const bool bMeshHasNormals = MeshData.VertexInfo.bHasTangentZ || MeshData.TangentsZ.Num() == VertexCount;
			const bool bMeshHasUvs = MeshData.VertexInfo.bHasUV0 || MeshData.TextureCoordinates.Num() > 0;
			bHasNormals = bHasNormals || bMeshHasNormals;
			bHasUvs = bHasUvs || bMeshHasUvs;

			TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
			MeshObj->SetNumberField(TEXT("track_index"), MeshIndex);
			MeshObj->SetNumberField(TEXT("vertex_count"), VertexCount);
			MeshObj->SetNumberField(TEXT("index_count"), IndexCount);
			MeshObj->SetNumberField(TEXT("triangle_count"), TriangleCount);
			MeshObj->SetNumberField(TEXT("uv_count"), MeshData.TextureCoordinates.Num());
			MeshObj->SetNumberField(TEXT("normal_count"), MeshData.TangentsZ.Num());
			MeshObj->SetNumberField(TEXT("batch_count"), MeshData.BatchesInfo.Num());
			MeshObj->SetBoolField(TEXT("has_normals"), bMeshHasNormals);
			MeshObj->SetBoolField(TEXT("has_uvs"), bMeshHasUvs);
			MeshObj->SetObjectField(TEXT("bounds"), MakeBoxJson(MeshData.BoundingBox));
			MeshesJson.Add(MakeShared<FJsonValueObject>(MeshObj));
		}

		if (MeshDataAtTime.Num() == 0)
		{
			UeAgentJsonDiagnostics::AddIssue(OutIssues, TEXT("warning"), TEXT("geometry_cache_no_mesh_sample_at_time"), TEXT("time_seconds"), TEXT("No mesh sample data was returned at the requested time."));
		}

		Obj->SetNumberField(TEXT("time_seconds"), ClampedTime);
		Obj->SetNumberField(TEXT("requested_time_seconds"), TimeSeconds);
		Obj->SetNumberField(TEXT("frame_index"), ApproxFrame);
		Obj->SetNumberField(TEXT("frame_start"), StartFrame);
		Obj->SetNumberField(TEXT("frame_end"), EndFrame);
		Obj->SetNumberField(TEXT("frame_count"), FrameCount);
		Obj->SetNumberField(TEXT("duration"), Duration);
		Obj->SetNumberField(TEXT("mesh_count"), MeshDataAtTime.Num());
		Obj->SetNumberField(TEXT("total_vertex_count"), TotalVertexCount);
		Obj->SetNumberField(TEXT("total_index_count"), TotalIndexCount);
		Obj->SetNumberField(TEXT("total_triangle_count"), TotalTriangleCount);
		Obj->SetBoolField(TEXT("has_normals"), bHasNormals);
		Obj->SetBoolField(TEXT("has_uvs"), bHasUvs);
		Obj->SetObjectField(TEXT("bounds"), MakeBoxJson(CombinedBounds));
		Obj->SetArrayField(TEXT("meshes"), MeshesJson);
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildGeometryCacheDetailsJson(UGeometryCache* Cache, TArray<TSharedPtr<FJsonValue>>& OutIssues)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Cache)
		{
			UeAgentJsonDiagnostics::AddIssue(OutIssues, TEXT("error"), TEXT("geometry_cache_not_found"), TEXT("asset_path"), TEXT("Geometry Cache asset could not be loaded."));
			return Obj;
		}

		const int32 StartFrame = Cache->GetStartFrame();
		const int32 EndFrame = Cache->GetEndFrame();
		const float Duration = Cache->CalculateDuration();
		Obj->SetNumberField(TEXT("frame_start"), StartFrame);
		Obj->SetNumberField(TEXT("frame_end"), EndFrame);
		Obj->SetNumberField(TEXT("frame_count"), EndFrame >= StartFrame ? EndFrame - StartFrame + 1 : 0);
		Obj->SetNumberField(TEXT("duration"), Duration);
		Obj->SetStringField(TEXT("hash"), Cache->GetHash());
		Obj->SetNumberField(TEXT("track_count"), Cache->Tracks.Num());
		Obj->SetNumberField(TEXT("material_count"), Cache->Materials.Num());

		TArray<TSharedPtr<FJsonValue>> Materials;
		for (int32 Index = 0; Index < Cache->Materials.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetStringField(TEXT("asset_path"), Cache->Materials[Index] ? Cache->Materials[Index]->GetPathName() : TEXT(""));
			Item->SetStringField(TEXT("slot_name"), Cache->MaterialSlotNames.IsValidIndex(Index) ? Cache->MaterialSlotNames[Index].ToString() : FString());
			Materials.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("materials"), Materials);

		TArray<TSharedPtr<FJsonValue>> Tracks;
		for (int32 Index = 0; Index < Cache->Tracks.Num(); ++Index)
		{
			UGeometryCacheTrack* Track = Cache->Tracks[Index];
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetBoolField(TEXT("valid"), Track != nullptr);
			if (Track)
			{
				Item->SetStringField(TEXT("name"), Track->GetName());
				Item->SetStringField(TEXT("object_path"), Track->GetPathName());
				Item->SetStringField(TEXT("class"), Track->GetClass() ? Track->GetClass()->GetPathName() : TEXT(""));
				Item->SetNumberField(TEXT("duration"), Track->GetDuration());
				Item->SetNumberField(TEXT("max_sample_time"), Track->GetMaxSampleTime());
				Item->SetNumberField(TEXT("material_count"), Track->GetNumMaterials());
			}
			Tracks.Add(MakeShared<FJsonValueObject>(Item));
		}
		Obj->SetArrayField(TEXT("tracks"), Tracks);

		TArray<float> SampleTimes;
		SampleTimes.Add(0.0f);
		if (Duration > KINDA_SMALL_NUMBER)
		{
			SampleTimes.Add(Duration * 0.5f);
			SampleTimes.Add(Duration);
		}

		TArray<TSharedPtr<FJsonValue>> SampleTimeJson;
		TArray<TSharedPtr<FJsonValue>> SamplesJson;
		TArray<TSharedPtr<FJsonValue>> VertexCountPerTrack;
		TArray<TSharedPtr<FJsonValue>> TriangleCountPerTrack;
		FBox3f CombinedBounds(EForceInit::ForceInit);
		bool bHasNormals = false;
		bool bHasUvs = false;
		bool bHasAnySample = false;
		bool bTopologyConstant = true;
		TArray<int32> BaselineVertexCounts;
		TArray<int32> BaselineIndexCounts;

		for (int32 SampleIndex = 0; SampleIndex < SampleTimes.Num(); ++SampleIndex)
		{
			const float SampleTime = FMath::Clamp(SampleTimes[SampleIndex], 0.0f, FMath::Max(Duration, 0.0f));
			SampleTimeJson.Add(MakeShared<FJsonValueNumber>(SampleTime));
			TArray<FGeometryCacheMeshData> MeshDataAtTime;
			Cache->GetMeshDataAtTime(SampleTime, MeshDataAtTime);
			TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
			SampleObj->SetNumberField(TEXT("time_seconds"), SampleTime);
			SampleObj->SetNumberField(TEXT("mesh_count"), MeshDataAtTime.Num());
			TArray<TSharedPtr<FJsonValue>> MeshesJson;
			if (MeshDataAtTime.Num() > 0)
			{
				bHasAnySample = true;
			}
			for (int32 MeshIndex = 0; MeshIndex < MeshDataAtTime.Num(); ++MeshIndex)
			{
				const FGeometryCacheMeshData& MeshData = MeshDataAtTime[MeshIndex];
				const int32 VertexCount = MeshData.Positions.Num();
				const int32 IndexCount = MeshData.Indices.Num();
				const int32 TriangleCount = IndexCount / 3;
				if (SampleIndex == 0)
				{
					BaselineVertexCounts.Add(VertexCount);
					BaselineIndexCounts.Add(IndexCount);
					VertexCountPerTrack.Add(MakeShared<FJsonValueNumber>(VertexCount));
					TriangleCountPerTrack.Add(MakeShared<FJsonValueNumber>(TriangleCount));
				}
				else if (!BaselineVertexCounts.IsValidIndex(MeshIndex)
					|| !BaselineIndexCounts.IsValidIndex(MeshIndex)
					|| BaselineVertexCounts[MeshIndex] != VertexCount
					|| BaselineIndexCounts[MeshIndex] != IndexCount)
				{
					bTopologyConstant = false;
				}
				if (MeshData.BoundingBox.IsValid != 0)
				{
					CombinedBounds += MeshData.BoundingBox;
				}
				bHasNormals = bHasNormals || MeshData.VertexInfo.bHasTangentZ || MeshData.TangentsZ.Num() == VertexCount;
				bHasUvs = bHasUvs || MeshData.VertexInfo.bHasUV0 || MeshData.TextureCoordinates.Num() > 0;

				TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
				MeshObj->SetNumberField(TEXT("track_index"), MeshIndex);
				MeshObj->SetNumberField(TEXT("vertex_count"), VertexCount);
				MeshObj->SetNumberField(TEXT("index_count"), IndexCount);
				MeshObj->SetNumberField(TEXT("triangle_count"), TriangleCount);
				MeshObj->SetNumberField(TEXT("uv_count"), MeshData.TextureCoordinates.Num());
				MeshObj->SetNumberField(TEXT("normal_count"), MeshData.TangentsZ.Num());
				MeshObj->SetNumberField(TEXT("batch_count"), MeshData.BatchesInfo.Num());
				MeshObj->SetBoolField(TEXT("has_normals"), MeshData.VertexInfo.bHasTangentZ || MeshData.TangentsZ.Num() == VertexCount);
				MeshObj->SetBoolField(TEXT("has_uvs"), MeshData.VertexInfo.bHasUV0 || MeshData.TextureCoordinates.Num() > 0);
				MeshObj->SetObjectField(TEXT("bounds"), MakeBoxJson(MeshData.BoundingBox));
				MeshesJson.Add(MakeShared<FJsonValueObject>(MeshObj));
			}
			SampleObj->SetArrayField(TEXT("meshes"), MeshesJson);
			SamplesJson.Add(MakeShared<FJsonValueObject>(SampleObj));
		}

		if (!bHasAnySample)
		{
			UeAgentJsonDiagnostics::AddIssue(OutIssues, TEXT("warning"), TEXT("geometry_cache_no_mesh_samples"), TEXT("samples"), TEXT("No mesh sample data was returned by GeometryCache::GetMeshDataAtTime."));
		}

		Obj->SetArrayField(TEXT("sampled_times"), SampleTimeJson);
		Obj->SetArrayField(TEXT("samples"), SamplesJson);
		Obj->SetArrayField(TEXT("vertex_count_per_track"), VertexCountPerTrack);
		Obj->SetArrayField(TEXT("triangle_count_per_track"), TriangleCountPerTrack);
		Obj->SetBoolField(TEXT("topology_constant"), bTopologyConstant);
		Obj->SetStringField(TEXT("topology_sample_policy"), TEXT("start/mid/end mesh count, vertex count and index count comparison"));
		Obj->SetBoolField(TEXT("has_normals"), bHasNormals);
		Obj->SetBoolField(TEXT("has_uvs"), bHasUvs);
		Obj->SetObjectField(TEXT("bounds"), MakeBoxJson(CombinedBounds));
#if WITH_EDITORONLY_DATA
		Obj->SetStringField(TEXT("import_file"), Cache->AssetImportData ? Cache->AssetImportData->GetFirstFilename() : TEXT(""));
#else
		Obj->SetStringField(TEXT("import_file"), TEXT(""));
#endif
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<const TCHAR*> InValues)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Value : InValues)
		{
			Values.Add(MakeShared<FJsonValueString>(FString(Value)));
		}
		return Values;
	}

	static TSharedPtr<FJsonObject> MakeVector2DJson(const FVector2D& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		return Obj;
	}

	static FString ObjectPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static FString ClassPathOrEmpty(const UObject* Object)
	{
		return Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString();
	}

	static FString OptimusDeformerStatusToString(EOptimusDeformerStatus Status)
	{
		switch (Status)
		{
		case EOptimusDeformerStatus::Compiled:
			return TEXT("Compiled");
		case EOptimusDeformerStatus::CompiledWithWarnings:
			return TEXT("CompiledWithWarnings");
		case EOptimusDeformerStatus::Modified:
			return TEXT("Modified");
		case EOptimusDeformerStatus::HasErrors:
			return TEXT("HasErrors");
		default:
			return TEXT("Unknown");
		}
	}

	static FString OptimusDiagnosticLevelToString(EOptimusDiagnosticLevel Level)
	{
		switch (Level)
		{
		case EOptimusDiagnosticLevel::Info:
			return TEXT("info");
		case EOptimusDiagnosticLevel::Warning:
			return TEXT("warning");
		case EOptimusDiagnosticLevel::Error:
			return TEXT("error");
		case EOptimusDiagnosticLevel::None:
		default:
			return TEXT("none");
		}
	}

	static FString OptimusGraphTypeToString(EOptimusNodeGraphType GraphType)
	{
		switch (GraphType)
		{
		case EOptimusNodeGraphType::Setup:
			return TEXT("Setup");
		case EOptimusNodeGraphType::Update:
			return TEXT("Update");
		case EOptimusNodeGraphType::ExternalTrigger:
			return TEXT("ExternalTrigger");
		case EOptimusNodeGraphType::Function:
			return TEXT("Function");
		case EOptimusNodeGraphType::SubGraph:
			return TEXT("SubGraph");
		case EOptimusNodeGraphType::Transient:
			return TEXT("Transient");
		default:
			return TEXT("Unknown");
		}
	}

	static FString OptimusPinDirectionToString(EOptimusNodePinDirection Direction)
	{
		switch (Direction)
		{
		case EOptimusNodePinDirection::Input:
			return TEXT("Input");
		case EOptimusNodePinDirection::Output:
			return TEXT("Output");
		case EOptimusNodePinDirection::Unknown:
		default:
			return TEXT("Unknown");
		}
	}

	static TSharedPtr<FJsonObject> BuildOptimusDataTypeJson(const FOptimusDataTypeHandle& DataType)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), DataType.IsValid());
		if (DataType.IsValid())
		{
			Obj->SetStringField(TEXT("type_name"), DataType->TypeName.ToString());
			Obj->SetStringField(TEXT("display_name"), DataType->DisplayName.ToString());
			Obj->SetStringField(TEXT("type_category"), DataType->TypeCategory.ToString());
			Obj->SetNumberField(TEXT("shader_value_size"), DataType->ShaderValueSize);
			Obj->SetBoolField(TEXT("is_array_type"), DataType->IsArrayType());
			Obj->SetStringField(TEXT("type_object"), ObjectPathOrEmpty(DataType->TypeObject.Get()));
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusDataTypeRefJson(const FOptimusDataTypeRef& DataTypeRef)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), DataTypeRef.IsValid());
		Obj->SetStringField(TEXT("type_name"), DataTypeRef.TypeName.ToString());
		Obj->SetStringField(TEXT("type_object"), DataTypeRef.TypeObject.ToString());
		Obj->SetObjectField(TEXT("resolved"), BuildOptimusDataTypeJson(DataTypeRef.Resolve()));
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusPinJson(const UOptimusNodePin* Pin, int32 Depth = 0)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Pin != nullptr);
		if (!Pin)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Pin->GetUniqueName().ToString());
		Obj->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
		Obj->SetStringField(TEXT("path"), Pin->GetPinPath());
		Obj->SetStringField(TEXT("direction"), OptimusPinDirectionToString(Pin->GetDirection()));
		Obj->SetBoolField(TEXT("is_grouping_pin"), Pin->IsGroupingPin());
		Obj->SetObjectField(TEXT("data_type"), BuildOptimusDataTypeJson(Pin->GetDataType()));
		Obj->SetStringField(TEXT("data_domain"), Pin->GetDataDomain().ToString());
		Obj->SetStringField(TEXT("value_text"), Pin->GetValueAsString());
		Obj->SetNumberField(TEXT("connected_pin_count"), Pin->GetConnectedPins().Num());

		TArray<TSharedPtr<FJsonValue>> SubPins;
		if (Depth < 8)
		{
			for (const TObjectPtr<UOptimusNodePin>& SubPin : Pin->GetSubPins())
			{
				SubPins.Add(MakeShared<FJsonValueObject>(BuildOptimusPinJson(SubPin.Get(), Depth + 1)));
			}
		}
		Obj->SetArrayField(TEXT("sub_pins"), SubPins);
		Obj->SetNumberField(TEXT("sub_pin_count"), SubPins.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusNodeJson(const UOptimusNode* Node, bool bIncludePins = true)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Node != nullptr);
		if (!Node)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Node->GetNodeName().ToString());
		Obj->SetStringField(TEXT("display_name"), Node->GetDisplayName().ToString());
		Obj->SetStringField(TEXT("path"), Node->GetNodePath());
		Obj->SetStringField(TEXT("class"), ClassPathOrEmpty(Node));
		Obj->SetStringField(TEXT("category"), Node->GetNodeCategory().ToString());
		Obj->SetObjectField(TEXT("position"), MakeVector2DJson(Node->GetGraphPosition()));
		Obj->SetStringField(TEXT("diagnostic_level"), OptimusDiagnosticLevelToString(Node->GetDiagnosticLevel()));
		Obj->SetBoolField(TEXT("can_user_delete"), Node->CanUserDeleteNode());
		Obj->SetBoolField(TEXT("is_compute_kernel_provider"), Node->GetClass()->ImplementsInterface(UOptimusComputeKernelProvider::StaticClass()));
		Obj->SetBoolField(TEXT("is_data_interface_provider"), Node->GetClass()->ImplementsInterface(UOptimusDataInterfaceProvider::StaticClass()));
		Obj->SetBoolField(TEXT("is_shader_text_provider"), Node->GetClass()->ImplementsInterface(UOptimusShaderTextProvider::StaticClass()));

		TArray<TSharedPtr<FJsonValue>> Pins;
		if (bIncludePins)
		{
			for (UOptimusNodePin* Pin : Node->GetPins())
			{
				Pins.Add(MakeShared<FJsonValueObject>(BuildOptimusPinJson(Pin)));
			}
		}
		Obj->SetArrayField(TEXT("pins"), Pins);
		Obj->SetNumberField(TEXT("pin_count"), Node->GetPins().Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusLinkJson(const UOptimusNodeLink* Link)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Link != nullptr);
		if (!Link)
		{
			return Obj;
		}
		const UOptimusNodePin* OutputPin = Link->GetNodeOutputPin();
		const UOptimusNodePin* InputPin = Link->GetNodeInputPin();
		Obj->SetStringField(TEXT("output_pin"), OutputPin ? OutputPin->GetPinPath() : FString());
		Obj->SetStringField(TEXT("input_pin"), InputPin ? InputPin->GetPinPath() : FString());
		return Obj;
	}

	static void AppendOptimusGraphJson(const UOptimusNodeGraph* Graph, TArray<TSharedPtr<FJsonValue>>& OutGraphs, int32 Depth = 0)
	{
		if (!Graph || Depth > 16)
		{
			return;
		}
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Graph->GetName());
		Obj->SetStringField(TEXT("path"), Graph->GetCollectionPath());
		Obj->SetStringField(TEXT("class"), ClassPathOrEmpty(Graph));
		Obj->SetStringField(TEXT("graph_type"), OptimusGraphTypeToString(Graph->GetGraphType()));
		Obj->SetBoolField(TEXT("is_execution_graph"), Graph->IsExecutionGraph());
		Obj->SetBoolField(TEXT("is_function_graph"), Graph->IsFunctionGraph());
		Obj->SetBoolField(TEXT("is_read_only"), Graph->IsReadOnly());
		Obj->SetNumberField(TEXT("graph_index"), Graph->GetGraphIndex());
		Obj->SetNumberField(TEXT("depth"), Depth);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const UOptimusNode* Node : Graph->GetAllNodes())
		{
			Nodes.Add(MakeShared<FJsonValueObject>(BuildOptimusNodeJson(Node)));
		}
		Obj->SetArrayField(TEXT("nodes"), Nodes);
		Obj->SetNumberField(TEXT("node_count"), Nodes.Num());

		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UOptimusNodeLink* Link : Graph->GetAllLinks())
		{
			Links.Add(MakeShared<FJsonValueObject>(BuildOptimusLinkJson(Link)));
		}
		Obj->SetArrayField(TEXT("links"), Links);
		Obj->SetNumberField(TEXT("link_count"), Links.Num());

		TArray<TSharedPtr<FJsonValue>> ChildGraphNames;
		for (const UOptimusNodeGraph* ChildGraph : Graph->GetGraphs())
		{
			if (ChildGraph)
			{
				ChildGraphNames.Add(MakeShared<FJsonValueString>(ChildGraph->GetName()));
			}
		}
		Obj->SetArrayField(TEXT("child_graphs"), ChildGraphNames);
		Obj->SetNumberField(TEXT("child_graph_count"), ChildGraphNames.Num());
		OutGraphs.Add(MakeShared<FJsonValueObject>(Obj));

		for (const UOptimusNodeGraph* ChildGraph : Graph->GetGraphs())
		{
			AppendOptimusGraphJson(ChildGraph, OutGraphs, Depth + 1);
		}
	}

	static TSharedPtr<FJsonObject> BuildOptimusGraphsJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_graphs.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("graphs"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary; mutation requires deformer_graph_apply_folder raw_properties or a dedicated Optimus graph adapter"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Graphs;
		if (Deformer)
		{
			for (const UOptimusNodeGraph* Graph : Deformer->GetGraphs())
			{
				AppendOptimusGraphJson(Graph, Graphs);
			}
		}
		Obj->SetArrayField(TEXT("graphs"), Graphs);
		Obj->SetArrayField(TEXT("items"), Graphs);
		Obj->SetNumberField(TEXT("graph_count"), Graphs.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusBindingsJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_bindings.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("bindings"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Bindings;
		if (Deformer)
		{
			for (const UOptimusComponentSourceBinding* Binding : Deformer->GetComponentBindings())
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetBoolField(TEXT("valid"), Binding != nullptr);
				if (Binding)
				{
					Item->SetStringField(TEXT("name"), Binding->BindingName.ToString());
					Item->SetStringField(TEXT("object_path"), Binding->GetPathName());
					Item->SetBoolField(TEXT("is_primary"), Binding->IsPrimaryBinding());
					Item->SetStringField(TEXT("component_type"), Binding->ComponentType ? Binding->ComponentType->GetPathName() : FString());
					const UOptimusComponentSource* Source = Binding->GetComponentSource();
					Item->SetStringField(TEXT("component_source_display_name"), Source ? Source->GetDisplayName().ToString() : FString());
					TArray<TSharedPtr<FJsonValue>> Tags;
					for (const FName& Tag : Binding->ComponentTags)
					{
						Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Item->SetArrayField(TEXT("component_tags"), Tags);
				}
				Bindings.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("bindings"), Bindings);
		Obj->SetArrayField(TEXT("items"), Bindings);
		Obj->SetNumberField(TEXT("binding_count"), Bindings.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusResourcesJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_resources.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("resources"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Resources;
		if (Deformer)
		{
			for (const UOptimusResourceDescription* Resource : Deformer->GetResources())
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetBoolField(TEXT("valid"), Resource != nullptr);
				if (Resource)
				{
					Item->SetStringField(TEXT("name"), Resource->ResourceName.ToString());
					Item->SetStringField(TEXT("object_path"), Resource->GetPathName());
					Item->SetNumberField(TEXT("index"), Resource->GetIndex());
					Item->SetObjectField(TEXT("data_type"), BuildOptimusDataTypeRefJson(Resource->DataType));
					Item->SetStringField(TEXT("data_domain"), Resource->DataDomain.ToString());
					Item->SetStringField(TEXT("component_binding"), ObjectPathOrEmpty(Resource->ComponentBinding.Get()));
					Item->SetBoolField(TEXT("has_persistent_buffer_data_interface"), Resource->DataInterface != nullptr);
					Item->SetObjectField(TEXT("property_summary"), BuildPropertySummaryJson(const_cast<UOptimusResourceDescription*>(Resource)));
				}
				Resources.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("resources"), Resources);
		Obj->SetArrayField(TEXT("items"), Resources);
		Obj->SetNumberField(TEXT("resource_count"), Resources.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusVariablesJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_variables.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("variables"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Variables;
		if (Deformer)
		{
			for (const UOptimusVariableDescription* Variable : Deformer->GetVariables())
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetBoolField(TEXT("valid"), Variable != nullptr);
				if (Variable)
				{
					Item->SetStringField(TEXT("name"), Variable->VariableName.ToString());
					Item->SetStringField(TEXT("object_path"), Variable->GetPathName());
					Item->SetStringField(TEXT("guid"), Variable->Guid.ToString());
					Item->SetNumberField(TEXT("index"), Variable->GetIndex());
					Item->SetObjectField(TEXT("data_type"), BuildOptimusDataTypeRefJson(Variable->DataType));
					Item->SetObjectField(TEXT("property_summary"), BuildPropertySummaryJson(const_cast<UOptimusVariableDescription*>(Variable)));
				}
				Variables.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("variables"), Variables);
		Obj->SetArrayField(TEXT("items"), Variables);
		Obj->SetNumberField(TEXT("variable_count"), Variables.Num());
		return Obj;
	}

	static void AppendOptimusNodesByInterface(const UOptimusDeformer* Deformer, const UClass* InterfaceClass, TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		if (!Deformer || !InterfaceClass)
		{
			return;
		}
		for (const UOptimusNodeGraph* Graph : Deformer->GetGraphs())
		{
			TArray<TSharedPtr<FJsonValue>> Graphs;
			AppendOptimusGraphJson(Graph, Graphs);
			for (const TSharedPtr<FJsonValue>& GraphValue : Graphs)
			{
				const TSharedPtr<FJsonObject> GraphObj = GraphValue.IsValid() ? GraphValue->AsObject() : nullptr;
				if (!GraphObj.IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (!GraphObj->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
				{
					const TSharedPtr<FJsonObject> NodeObj = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
					if (!NodeObj.IsValid())
					{
						continue;
					}
					FString NodeClassPath;
					NodeObj->TryGetStringField(TEXT("class"), NodeClassPath);
					UClass* NodeClass = LoadClassByPath(NodeClassPath);
					if (NodeClass && NodeClass->ImplementsInterface(InterfaceClass))
					{
						OutNodes.Add(NodeValue);
					}
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> BuildOptimusDataInterfacesJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_data_interfaces.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("data_interfaces"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Items;
		AppendOptimusNodesByInterface(Deformer, UOptimusDataInterfaceProvider::StaticClass(), Items);
		Obj->SetArrayField(TEXT("nodes"), Items);
		Obj->SetArrayField(TEXT("items"), Items);
		Obj->SetNumberField(TEXT("data_interface_provider_count"), Items.Num());
		return Obj;
	}

	static FString TruncateLongText(const FString& Value, int32 MaxChars, bool& bOutTruncated)
	{
		bOutTruncated = Value.Len() > MaxChars;
		return bOutTruncated ? Value.Left(MaxChars) : Value;
	}

	static TSharedPtr<FJsonObject> BuildOptimusKernelsJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_kernels.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("kernels"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());

		TArray<TSharedPtr<FJsonValue>> Kernels;
		TFunction<void(const UOptimusNodeGraph*)> VisitGraphForKernels = [&](const UOptimusNodeGraph* Graph)
		{
			if (!Graph)
			{
				return;
			}
			for (const UOptimusNode* Node : Graph->GetAllNodes())
			{
					const bool bKernelProvider = Node->GetClass()->ImplementsInterface(UOptimusComputeKernelProvider::StaticClass());
					const bool bShaderTextProvider = Node->GetClass()->ImplementsInterface(UOptimusShaderTextProvider::StaticClass());
					if (!bKernelProvider && !bShaderTextProvider)
					{
						continue;
					}
					TSharedPtr<FJsonObject> Item = BuildOptimusNodeJson(Node, true);
					if (const IOptimusComputeKernelProvider* KernelProvider = Cast<IOptimusComputeKernelProvider>(Node))
					{
						Item->SetStringField(TEXT("execution_domain"), KernelProvider->GetExecutionDomain().AsExpression());
					}
#if WITH_EDITOR
					if (const IOptimusShaderTextProvider* ShaderTextProvider = Cast<IOptimusShaderTextProvider>(Node))
					{
						const FString Declarations = ShaderTextProvider->GetDeclarations();
						const FString ShaderText = ShaderTextProvider->GetShaderText();
						bool bDeclarationsTruncated = false;
						bool bShaderTextTruncated = false;
						Item->SetStringField(TEXT("shader_editor_name"), ShaderTextProvider->GetNameForShaderTextEditor());
						Item->SetBoolField(TEXT("shader_text_read_only"), ShaderTextProvider->IsShaderTextReadOnly());
						Item->SetNumberField(TEXT("declarations_length"), Declarations.Len());
						Item->SetNumberField(TEXT("shader_text_length"), ShaderText.Len());
						Item->SetStringField(TEXT("declarations"), TruncateLongText(Declarations, 20000, bDeclarationsTruncated));
						Item->SetStringField(TEXT("shader_text"), TruncateLongText(ShaderText, 20000, bShaderTextTruncated));
						Item->SetBoolField(TEXT("declarations_truncated"), bDeclarationsTruncated);
						Item->SetBoolField(TEXT("shader_text_truncated"), bShaderTextTruncated);
					}
#endif
					Kernels.Add(MakeShared<FJsonValueObject>(Item));
			}
			for (const UOptimusNodeGraph* ChildGraph : Graph->GetGraphs())
			{
				VisitGraphForKernels(ChildGraph);
			}
		};
		if (Deformer)
		{
			for (const UOptimusNodeGraph* Graph : Deformer->GetGraphs())
			{
				VisitGraphForKernels(Graph);
			}
		}
		Obj->SetArrayField(TEXT("kernels"), Kernels);
		Obj->SetArrayField(TEXT("items"), Kernels);
		Obj->SetNumberField(TEXT("kernel_provider_count"), Kernels.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusSourceLibrariesJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_source_libraries.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("source_libraries"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		TArray<TSharedPtr<FJsonValue>> Functions;
		if (Deformer)
		{
			for (const UOptimusFunctionNodeGraph* FunctionGraph : Deformer->GetFunctionGraphs())
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetBoolField(TEXT("valid"), FunctionGraph != nullptr);
				if (FunctionGraph)
				{
					Item->SetStringField(TEXT("name"), FunctionGraph->GetName());
					Item->SetStringField(TEXT("node_name"), FunctionGraph->GetNodeName());
					Item->SetStringField(TEXT("class"), ClassPathOrEmpty(FunctionGraph));
					Item->SetStringField(TEXT("guid"), FunctionGraph->GetGuid().ToString());
					Item->SetStringField(TEXT("category"), FunctionGraph->Category.ToString());
					Item->SetStringField(TEXT("access_specifier"), FunctionGraph->AccessSpecifier.ToString());
					Item->SetNumberField(TEXT("node_count"), FunctionGraph->GetAllNodes().Num());
					Item->SetNumberField(TEXT("link_count"), FunctionGraph->GetAllLinks().Num());
				}
				Functions.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("function_graphs"), Functions);
		Obj->SetArrayField(TEXT("items"), Functions);
		Obj->SetNumberField(TEXT("function_graph_count"), Functions.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusSettingsJson(const UOptimusDeformer* Deformer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_settings.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), TEXT("settings"));
		Obj->SetStringField(TEXT("edit_policy"), TEXT("read_only_optimus_summary"));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		Obj->SetBoolField(TEXT("is_optimus_deformer"), Deformer != nullptr);
		if (Deformer)
		{
			Obj->SetStringField(TEXT("status"), OptimusDeformerStatusToString(Deformer->GetStatus()));
			Obj->SetStringField(TEXT("preview_mesh"), ObjectPathOrEmpty(Deformer->GetPreviewMesh()));
			Obj->SetNumberField(TEXT("graph_count"), Deformer->GetGraphs().Num());
			Obj->SetNumberField(TEXT("resource_count"), Deformer->GetResources().Num());
			Obj->SetNumberField(TEXT("variable_count"), Deformer->GetVariables().Num());
			Obj->SetNumberField(TEXT("binding_count"), Deformer->GetComponentBindings().Num());
			Obj->SetNumberField(TEXT("function_graph_count"), Deformer->GetFunctionGraphs().Num());
			Obj->SetObjectField(TEXT("raw_property_summary"), BuildPropertySummaryJson(const_cast<UOptimusDeformer*>(Deformer)));
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildReflectionSectionJson(UObject* Asset, const FString& Profile, const FString& Section, const TArray<FString>& Keywords, const FString& EditPolicy)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.reflection_section.v1"));
		Obj->SetStringField(TEXT("profile"), Profile);
		Obj->SetStringField(TEXT("section"), Section);
		Obj->SetStringField(TEXT("edit_policy"), EditPolicy);
		Obj->SetStringField(TEXT("asset_path"), Asset ? NormalizeAssetPath(Asset->GetOutermost()->GetName()) : FString());
		Obj->SetStringField(TEXT("asset_class"), ClassPathOrEmpty(Asset));
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());

		TArray<TSharedPtr<FJsonValue>> KeywordValues;
		for (const FString& Keyword : Keywords)
		{
			KeywordValues.Add(MakeShared<FJsonValueString>(Keyword));
		}
		Obj->SetArrayField(TEXT("keywords"), KeywordValues);

		TArray<TSharedPtr<FJsonValue>> Items;
		int32 PropertyCount = 0;
		if (Asset)
		{
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property)
				{
					continue;
				}
				++PropertyCount;
				const FString Name = Property->GetName();
				const FString CppType = Property->GetCPPType();
				bool bMatched = Keywords.Num() == 0;
				for (const FString& Keyword : Keywords)
				{
					bMatched = bMatched
						|| Name.Contains(Keyword, ESearchCase::IgnoreCase)
						|| CppType.Contains(Keyword, ESearchCase::IgnoreCase);
				}
				if (!bMatched)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("property_name"), Name);
				Item->SetStringField(TEXT("cpp_type"), CppType);
				Item->SetStringField(TEXT("property_class"), Property->GetClass() ? Property->GetClass()->GetName() : FString());
				Item->SetBoolField(TEXT("can_edit"), Property->HasAnyPropertyFlags(CPF_Edit));
				Item->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
				Item->SetBoolField(TEXT("is_array"), Property->IsA<FArrayProperty>());
				Item->SetBoolField(TEXT("is_set"), Property->IsA<FSetProperty>());
				Item->SetBoolField(TEXT("is_map"), Property->IsA<FMapProperty>());
				Item->SetBoolField(TEXT("is_object_reference"), Property->IsA<FObjectPropertyBase>());
				FString ValueText;
				Property->ExportText_InContainer(0, ValueText, Asset, Asset, Asset, PPF_None);
				Item->SetStringField(TEXT("value_text"), ValueText);
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		Obj->SetArrayField(TEXT("items"), Items);
		Obj->SetNumberField(TEXT("property_count"), PropertyCount);
		Obj->SetNumberField(TEXT("matching_property_count"), Items.Num());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusCompileDiagnosticJson(const FOptimusCompilerDiagnostic& Diagnostic)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("severity"), OptimusDiagnosticLevelToString(Diagnostic.Level));
		Obj->SetStringField(TEXT("message"), Diagnostic.Message.ToString());
		Obj->SetNumberField(TEXT("line"), Diagnostic.Line);
		Obj->SetNumberField(TEXT("column_start"), Diagnostic.ColumnStart);
		Obj->SetNumberField(TEXT("column_end"), Diagnostic.ColumnEnd);
		Obj->SetStringField(TEXT("absolute_file_path"), Diagnostic.AbsoluteFilePath);
		Obj->SetStringField(TEXT("object_path"), Diagnostic.Object.IsValid() ? Diagnostic.Object->GetPathName() : FString());
		Obj->SetStringField(TEXT("object_class"), Diagnostic.Object.IsValid() ? ClassPathOrEmpty(Diagnostic.Object.Get()) : FString());
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildOptimusValidationReportJson(const UOptimusDeformer* Deformer, const FString& Section, const FString& Status)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("format_version"), 1);
		Obj->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.deformer_validation_report.v1"));
		Obj->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
		Obj->SetStringField(TEXT("section"), Section);
		Obj->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
		Obj->SetBoolField(TEXT("is_optimus_deformer"), Deformer != nullptr);
		Obj->SetStringField(TEXT("status"), Status);
		Obj->SetStringField(TEXT("deformer_status"), Deformer ? OptimusDeformerStatusToString(Deformer->GetStatus()) : FString());
		Obj->SetStringField(TEXT("diagnostics_scope"), TEXT("asset state and synchronous Optimus graph diagnostics only; async shader compiler diagnostics may arrive after compile returns"));
		Obj->SetArrayField(TEXT("diagnostics"), {});
		return Obj;
	}

	static bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObj = nullptr;
			if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
			{
				continue;
			}
			FString Severity;
			if ((*IssueObj)->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static int32 CountIssuesBySeverity(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& SeverityText)
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
		{
			const TSharedPtr<FJsonObject> IssueObj = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			FString Severity;
			if (IssueObj.IsValid() && IssueObj->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(SeverityText, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}
		return Count;
	}

	static bool PreparePropertyApplyParams(
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& JsonRoot,
		const FString& FallbackAssetPath,
		const FString& SourcePath,
		TSharedPtr<FJsonObject>& OutParams,
		int32& OutSelectedPropertyCount,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutError)
	{
		OutSelectedPropertyCount = 0;
		OutParams = MakeShared<FJsonObject>();
		if (!JsonRoot.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_root_invalid"), SourcePath, TEXT("Expected JSON object root."));
			return false;
		}

		UeAgentJsonDiagnostics::WarnUnknownFields(
			JsonRoot,
			SourcePath,
			{
				TEXT("format_version"),
				TEXT("schema"),
				TEXT("profile"),
				TEXT("asset_path"),
				TEXT("object_path"),
				TEXT("asset_class"),
				TEXT("engine_version"),
				TEXT("plugin_status"),
				TEXT("properties"),
				TEXT("property_count"),
				TEXT("apply_rule"),
				TEXT("runtime_settings"),
				TEXT("training_inputs"),
				TEXT("training_parameters")
			},
			Issues);

		bool bApplyAllProperties = false;
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("apply_all_properties"), bApplyAllProperties);
		}

		FString AssetPath = FallbackAssetPath;
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			JsonRoot->TryGetStringField(TEXT("asset_path"), AssetPath);
		}
		if (AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_asset_path");
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), SourcePath + TEXT(".asset_path"), TEXT("asset_path is required."));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!JsonRoot->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			OutParams->SetStringField(TEXT("asset_path"), AssetPath);
			OutParams->SetArrayField(TEXT("properties"), {});
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> SelectedProperties;
		for (int32 Index = 0; Index < Properties->Num(); ++Index)
		{
			const FString PropertyPath = FString::Printf(TEXT("%s.properties[%d]"), *SourcePath, Index);
			TSharedPtr<FJsonObject> PropertyObj;
			if (!UeAgentJsonDiagnostics::ReadObjectFromValue((*Properties)[Index], PropertyPath, PropertyObj, Issues))
			{
				continue;
			}

			UeAgentJsonDiagnostics::WarnUnknownFields(
				PropertyObj,
				PropertyPath,
				{
					TEXT("property_name"),
					TEXT("name"),
					TEXT("value_text"),
					TEXT("cpp_type"),
					TEXT("value_json"),
					TEXT("curve_json"),
					TEXT("value_json_schema"),
					TEXT("apply"),
					TEXT("can_edit"),
					TEXT("edit_policy")
				},
				Issues);

			bool bApply = bApplyAllProperties;
			PropertyObj->TryGetBoolField(TEXT("apply"), bApply);
			if (!bApply)
			{
				continue;
			}

			FString PropertyName;
			if (!PropertyObj->TryGetStringField(TEXT("property_name"), PropertyName))
			{
				PropertyObj->TryGetStringField(TEXT("name"), PropertyName);
			}
			if (PropertyName.TrimStartAndEnd().IsEmpty())
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), PropertyPath + TEXT(".property_name"), TEXT("Selected property requires property_name."));
				continue;
			}

			FString ValueText;
			const bool bHasValueText = PropertyObj->TryGetStringField(TEXT("value_text"), ValueText);
			const TSharedPtr<FJsonObject>* ValueJsonObj = nullptr;
			const bool bHasTypedJson =
				(PropertyObj->TryGetObjectField(TEXT("curve_json"), ValueJsonObj) && ValueJsonObj && ValueJsonObj->IsValid())
				|| (PropertyObj->TryGetObjectField(TEXT("value_json"), ValueJsonObj) && ValueJsonObj && ValueJsonObj->IsValid());
			if (!bHasValueText && !bHasTypedJson)
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), PropertyPath + TEXT(".value_text"), TEXT("Selected property requires value_text or curve_json/value_json."));
				continue;
			}

			TSharedPtr<FJsonObject> SelectedObj = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropertyObj->Values)
			{
				if (!Pair.Key.Equals(TEXT("name"), ESearchCase::CaseSensitive)
					&& !Pair.Key.Equals(TEXT("apply"), ESearchCase::CaseSensitive)
					&& !Pair.Key.Equals(TEXT("can_edit"), ESearchCase::CaseSensitive)
					&& !Pair.Key.Equals(TEXT("edit_policy"), ESearchCase::CaseSensitive))
				{
					SelectedObj->SetField(Pair.Key, Pair.Value);
				}
			}
			SelectedObj->SetStringField(TEXT("property_name"), PropertyName.TrimStartAndEnd());
			SelectedProperties.Add(MakeShared<FJsonValueObject>(SelectedObj));
		}

		OutSelectedPropertyCount = SelectedProperties.Num();
		OutParams->SetStringField(TEXT("asset_path"), AssetPath);
		OutParams->SetArrayField(TEXT("properties"), SelectedProperties);
		bool bSaveAfterApply = false;
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetBoolField(TEXT("save_after_apply"), bSaveAfterApply);
		}
		OutParams->SetBoolField(TEXT("save_after_apply"), bSaveAfterApply);
		if (HasErrorIssue(Issues))
		{
			OutError = TEXT("json_validation_failed");
			return false;
		}
		return true;
	}

	static FString ResolveExpectedPropertyProfile(const FString& Command, const FString& FallbackProfile)
	{
		const FString CommandLower = Command.ToLower();
		if (CommandLower.StartsWith(TEXT("mesh_deformer_collection_")))
		{
			return TEXT("mesh_deformer_collection");
		}
		if (CommandLower.StartsWith(TEXT("ml_deformer_")))
		{
			return TEXT("ml_deformer");
		}
		if (CommandLower.StartsWith(TEXT("deformer_graph_")))
		{
			return TEXT("deformer_graph");
		}
		if (CommandLower.StartsWith(TEXT("deformer_source_library_")))
		{
			return TEXT("deformer_source_library");
		}
		return FallbackProfile;
	}

	static bool ValidatePropertyJsonProfile(
		const TSharedPtr<FJsonObject>& JsonRoot,
		const FString& SourcePath,
		const FString& ExpectedProfile,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutError)
	{
		if (ExpectedProfile.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		FString Profile;
		if (!JsonRoot.IsValid() || !JsonRoot->TryGetStringField(TEXT("profile"), Profile) || Profile.TrimStartAndEnd().IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), SourcePath + TEXT(".profile"), FString::Printf(TEXT("Dedicated deformer property JSON requires profile='%s'."), *ExpectedProfile));
			OutError = TEXT("json_validation_failed");
			return false;
		}
		if (!Profile.Equals(ExpectedProfile, ESearchCase::IgnoreCase))
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_profile_mismatch"), SourcePath + TEXT(".profile"), FString::Printf(TEXT("Expected profile '%s' but got '%s'."), *ExpectedProfile, *Profile));
			OutError = TEXT("json_validation_failed");
			return false;
		}

		FString AssetClassPath;
		if (!JsonRoot->TryGetStringField(TEXT("asset_class"), AssetClassPath) || AssetClassPath.TrimStartAndEnd().IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_missing_required_field"), SourcePath + TEXT(".asset_class"), TEXT("Dedicated deformer property JSON must come from export_json and include asset_class."));
			OutError = TEXT("json_validation_failed");
			return false;
		}

		FString AssetPath;
		JsonRoot->TryGetStringField(TEXT("asset_path"), AssetPath);
		UObject* Asset = AssetPath.TrimStartAndEnd().IsEmpty() ? nullptr : LoadAssetObject(AssetPath);
		if (Asset && Asset->GetClass() && !Asset->GetClass()->GetPathName().Equals(AssetClassPath, ESearchCase::CaseSensitive))
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("json_asset_class_mismatch"), SourcePath + TEXT(".asset_class"), FString::Printf(TEXT("JSON asset_class '%s' does not match loaded asset class '%s'."), *AssetClassPath, *Asset->GetClass()->GetPathName()));
			OutError = TEXT("json_validation_failed");
			return false;
		}
		if (Asset)
		{
			if (ExpectedProfile == TEXT("deformer_graph") && !Cast<UOptimusDeformer>(Asset))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("asset_is_not_deformer_graph"), SourcePath + TEXT(".asset_path"), TEXT("Loaded asset is not an Optimus Deformer Graph."));
			}
			else if (ExpectedProfile == TEXT("mesh_deformer_collection") && !Cast<UMeshDeformerCollection>(Asset))
			{
				UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("asset_is_not_mesh_deformer_collection"), SourcePath + TEXT(".asset_path"), TEXT("Loaded asset is not a Mesh Deformer Collection."));
			}
			else if (ExpectedProfile == TEXT("ml_deformer"))
			{
				bool bMatchesKnownModelClass = false;
				for (const FString& ClassPath : GetDefaultClassPaths(TEXT("ml_deformer")))
				{
					if (UClass* ModelClass = LoadClassByPath(ClassPath))
					{
						if (Asset->IsA(ModelClass))
						{
							bMatchesKnownModelClass = true;
							break;
						}
					}
				}
				if (!bMatchesKnownModelClass)
				{
					UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("error"), TEXT("asset_is_not_ml_deformer_model"), SourcePath + TEXT(".asset_path"), TEXT("Loaded asset is not one of the known ML Deformer model classes enabled in this project."));
				}
			}
		}
		if (UeAgentJsonDiagnostics::HasErrorIssue(Issues))
		{
			OutError = TEXT("json_validation_failed");
			return false;
		}
		return true;
	}

	static bool LoadJsonRootForApply(
		const FUeAgentRequestContext& Ctx,
		TSharedPtr<FJsonObject>& OutRoot,
		FString& OutSourcePath,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		FString& OutError)
	{
		FString JsonFile;
		if (Ctx.Params.IsValid())
		{
			Ctx.Params->TryGetStringField(TEXT("json_file"), JsonFile);
		}
		JsonFile.TrimStartAndEndInline();
		if (!JsonFile.IsEmpty())
		{
			OutSourcePath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile);
			return UeAgentJsonDiagnostics::LoadJsonObjectFile(JsonFile, OutRoot, &Issues, OutError, false);
		}
		OutRoot = Ctx.Params;
		OutSourcePath = TEXT("params");
		return OutRoot.IsValid();
	}

	static bool SaveJsonFile(const FString& FilePathInput, const TSharedPtr<FJsonObject>& Obj, FString& OutResolvedPath, FString& OutError)
	{
		OutResolvedPath.Reset();
		if (!Obj.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}
		FString FilePath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePathInput);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_failed");
			return false;
		}
		OutResolvedPath = FilePath;
		return true;
	}

	static bool CreateAssetByClassPath(const FUeAgentRequestContext& Ctx, const FString& Profile, const FString& DefaultClassPath, TSharedPtr<FJsonObject>& OutData, FString& OutError)
	{
		FString AssetPath;
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("missing_asset_path");
			return false;
		}
		FString ClassPath = DefaultClassPath;
		Ctx.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		UClass* AssetClass = LoadClassByPath(ClassPath);
		if (!AssetClass)
		{
			OutError = FString::Printf(TEXT("asset_class_not_available:%s"), *ClassPath);
			OutData->SetObjectField(TEXT("plugin_status"), MakePluginStatusJson());
			return false;
		}
		if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("asset_class_is_abstract:%s"), *ClassPath);
			return false;
		}
		const FString PackageName = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		if (LoadAssetObject(PackageName))
		{
			OutError = TEXT("asset_already_exists");
			return false;
		}
		UPackage* Package = CreatePackage(*PackageName);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UObject* Asset = NewObject<UObject>(Package, AssetClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("create_asset_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		bool bSaveAfterCreate = false;
		Ctx.Params->TryGetBoolField(TEXT("save_after_create"), bSaveAfterCreate);
		if (bSaveAfterCreate && !SaveAssetPackage(Asset, OutError))
		{
			return false;
		}
		OutData = BuildAssetInfoJson(Asset, Profile);
		OutData->SetBoolField(TEXT("created"), true);
		OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
		return true;
	}
}

bool FUeAgentHttpServer::ExecuteGeometryCacheCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("geometry_cache_get_info")) return CmdGeometryCacheGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("geometry_cache_validate_against_skeletal_mesh")) return CmdGeometryCacheValidateAgainstSkeletalMesh(Ctx, OutData, OutError);
	if (CommandLower == TEXT("geometry_cache_screenshot_frame")) return CmdGeometryCacheScreenshotFrame(Ctx, OutData, OutError);
	OutError = TEXT("unknown_geometry_cache_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteDeformerGraphCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("deformer_graph_create")) return CmdDeformerGraphCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_get_info")) return CmdDeformerGraphGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_export_folder")) return CmdDeformerGraphExportFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_validate_folder")) return CmdDeformerGraphValidateFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_apply_folder")) return CmdDeformerGraphApplyFolder(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_compile")) return CmdDeformerGraphCompile(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_graph_get_compile_log")) return CmdDeformerGraphGetCompileLog(Ctx, OutData, OutError);
	OutError = TEXT("unknown_deformer_graph_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteDeformerSourceLibraryCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("deformer_source_library_create")) return CmdDeformerSourceLibraryCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_source_library_export_json")) return CmdDeformerSourceLibraryExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_source_library_validate_json")) return CmdDeformerSourceLibraryValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("deformer_source_library_apply_json")) return CmdDeformerSourceLibraryApplyJson(Ctx, OutData, OutError);
	OutError = TEXT("unknown_deformer_source_library_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteMeshDeformerCollectionCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("mesh_deformer_collection_create")) return CmdMeshDeformerCollectionCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("mesh_deformer_collection_export_json")) return CmdMeshDeformerCollectionExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("mesh_deformer_collection_validate_json")) return CmdMeshDeformerCollectionValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("mesh_deformer_collection_apply_json")) return CmdMeshDeformerCollectionApplyJson(Ctx, OutData, OutError);
	OutError = TEXT("unknown_mesh_deformer_collection_command");
	return false;
}

bool FUeAgentHttpServer::ExecuteMLDeformerCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("ml_deformer_create")) return CmdMLDeformerCreate(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_get_info")) return CmdMLDeformerGetInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_export_json")) return CmdMLDeformerExportJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_validate_json")) return CmdMLDeformerValidateJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_apply_json")) return CmdMLDeformerApplyJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_validate_training_inputs")) return CmdMLDeformerValidateTrainingInputs(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_get_training_log")) return CmdMLDeformerGetTrainingLog(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_train")) return CmdMLDeformerTrain(Ctx, OutData, OutError);
	if (CommandLower == TEXT("ml_deformer_preview")) return CmdMLDeformerPreview(Ctx, OutData, OutError);
	OutError = TEXT("unknown_ml_deformer_command");
	return false;
}

bool FUeAgentHttpServer::CmdAssetImportGeometryCache(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_file"), SourceFile) || SourceFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_source_file");
		return false;
	}
	FString DestinationPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_path"), DestinationPath) || DestinationPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_destination_path");
		return false;
	}
	SourceFile = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(SourceFile);
	if (!FPaths::FileExists(SourceFile))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames = { SourceFile };
	ImportData->DestinationPath = DestinationPath;
	ImportData->bReplaceExisting = true;
	TArray<UObject*> ImportedObjects = FAssetToolsModule::GetModule().Get().ImportAssetsAutomated(ImportData);
	TArray<TSharedPtr<FJsonValue>> Imported;
	for (UObject* Asset : ImportedObjects)
	{
		if (!Asset)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("object_path"), Asset->GetPathName());
		Item->SetStringField(TEXT("asset_path"), UeAgentDeformerOps::NormalizeAssetPath(Asset->GetOutermost()->GetName()));
		Item->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
		Imported.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetStringField(TEXT("source_file"), SourceFile);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetArrayField(TEXT("imported_assets"), Imported);
	OutData->SetNumberField(TEXT("imported_asset_count"), Imported.Num());
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	return Imported.Num() > 0;
}

bool FUeAgentHttpServer::CmdGeometryCacheGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("geometry_cache_not_found");
		return false;
	}
	UGeometryCache* GeometryCache = Cast<UGeometryCache>(Asset);
	if (!GeometryCache)
	{
		OutError = TEXT("asset_is_not_geometry_cache");
		return false;
	}
	OutData = UeAgentDeformerOps::BuildAssetInfoJson(Asset, TEXT("geometry_cache"));
	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	const TSharedPtr<FJsonObject> Details = UeAgentDeformerOps::BuildGeometryCacheDetailsJson(GeometryCache, ValidationIssues);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Details->Values)
	{
		OutData->SetField(Pair.Key, Pair.Value);
	}
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	return true;
}

bool FUeAgentHttpServer::CmdGeometryCacheValidateAgainstSkeletalMesh(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString GeometryCachePath;
	FString SkeletalMeshPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("geometry_cache"), GeometryCachePath) || GeometryCachePath.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("geometry_cache_path"), GeometryCachePath);
	}
	if (!JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh"), SkeletalMeshPath) || SkeletalMeshPath.TrimStartAndEnd().IsEmpty())
	{
		JsonTryGetString(Ctx.Params, TEXT("skeletal_mesh_path"), SkeletalMeshPath);
	}
	UObject* GeometryCacheAsset = UeAgentDeformerOps::LoadAssetObject(GeometryCachePath);
	UObject* SkeletalMeshAsset = UeAgentDeformerOps::LoadAssetObject(SkeletalMeshPath);
	UGeometryCache* GeometryCache = Cast<UGeometryCache>(GeometryCacheAsset);
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(SkeletalMeshAsset);
	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetBoolField(TEXT("geometry_cache_found"), GeometryCache != nullptr);
	OutData->SetBoolField(TEXT("skeletal_mesh_found"), SkeletalMesh != nullptr);
	OutData->SetStringField(TEXT("geometry_cache_class"), GeometryCacheAsset ? GeometryCacheAsset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("skeletal_mesh_class"), SkeletalMeshAsset ? SkeletalMeshAsset->GetClass()->GetPathName() : TEXT(""));
	if (!GeometryCacheAsset)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("geometry_cache_not_found"), TEXT("geometry_cache"), TEXT("Geometry Cache asset was not found."));
	}
	else if (!GeometryCache)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("asset_is_not_geometry_cache"), TEXT("geometry_cache"), TEXT("Asset exists but is not a Geometry Cache."));
	}
	if (!SkeletalMeshAsset)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("skeletal_mesh_not_found"), TEXT("skeletal_mesh"), TEXT("SkeletalMesh asset was not found."));
	}
	else if (!SkeletalMesh)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("asset_is_not_skeletal_mesh"), TEXT("skeletal_mesh"), TEXT("Asset exists but is not a SkeletalMesh."));
	}
	const int32 CacheVertexCount = UeAgentDeformerOps::GetGeometryCacheVertexCountAtTime(GeometryCache, 0.0f);
	const int32 SkeletalVertexCount = UeAgentDeformerOps::GetSkeletalMeshLod0VertexCount(SkeletalMesh);
	const bool bCanCompareVertexCount = CacheVertexCount >= 0 && SkeletalVertexCount >= 0;
	const bool bCompatible = bCanCompareVertexCount && CacheVertexCount == SkeletalVertexCount;
	OutData->SetNumberField(TEXT("geometry_cache_vertex_count"), CacheVertexCount);
	OutData->SetNumberField(TEXT("skeletal_mesh_lod0_vertex_count"), SkeletalVertexCount);
	OutData->SetBoolField(TEXT("compatible_with_skeletal_mesh"), bCompatible);
	OutData->SetStringField(TEXT("validation_policy"), TEXT("asset type, first-sample vertex count and skeletal LOD0 vertex count comparison; exact deformation mapping remains model/importer specific"));
	if (GeometryCache && SkeletalMesh && !bCanCompareVertexCount)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("warning"), TEXT("topology_vertex_count_unavailable"), TEXT("vertex_count"), TEXT("Unable to read both Geometry Cache sample vertices and SkeletalMesh LOD0 vertices."));
	}
	else if (GeometryCache && SkeletalMesh && !bCompatible)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("topology_vertex_count_mismatch"), TEXT("vertex_count"), TEXT("Geometry Cache first sample vertex count does not match SkeletalMesh LOD0 vertex count."));
	}
	int32 ValidationErrorCount = 0;
	for (const TSharedPtr<FJsonValue>& IssueValue : ValidationIssues)
	{
		const TSharedPtr<FJsonObject> IssueObj = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
		FString Severity;
		if (IssueObj.IsValid() && IssueObj->TryGetStringField(TEXT("severity"), Severity) && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			++ValidationErrorCount;
		}
	}
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	OutData->SetNumberField(TEXT("validation_error_count"), ValidationErrorCount);
	OutData->SetBoolField(TEXT("validation_passed"), ValidationErrorCount == 0);
	return GeometryCache && SkeletalMesh;
}

bool FUeAgentHttpServer::CmdGeometryCacheScreenshotFrame(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("geometry_cache_not_found");
		return false;
	}
	UGeometryCache* GeometryCache = Cast<UGeometryCache>(Asset);
	if (!GeometryCache)
	{
		OutError = TEXT("asset_is_not_geometry_cache");
		return false;
	}

	const int32 StartFrame = GeometryCache->GetStartFrame();
	const int32 EndFrame = GeometryCache->GetEndFrame();
	const int32 FrameCount = EndFrame >= StartFrame ? EndFrame - StartFrame + 1 : 0;
	const float Duration = FMath::Max(GeometryCache->CalculateDuration(), 0.0f);
	double TimeSecondsValue = 0.0;
	double FrameIndexValue = 0.0;
	const bool bHasTimeSeconds = JsonTryGetNumber(Ctx.Params, TEXT("time_seconds"), TimeSecondsValue);
	const bool bHasFrameIndex = JsonTryGetNumber(Ctx.Params, TEXT("frame_index"), FrameIndexValue) || JsonTryGetNumber(Ctx.Params, TEXT("frame"), FrameIndexValue);
	float SampleTimeSeconds = 0.0f;
	int32 RequestedFrameIndex = StartFrame;
	if (bHasFrameIndex && FrameCount > 1)
	{
		RequestedFrameIndex = FMath::Clamp(FMath::RoundToInt(FrameIndexValue), StartFrame, EndFrame);
		const float FrameAlpha = static_cast<float>(RequestedFrameIndex - StartFrame) / static_cast<float>(FrameCount - 1);
		SampleTimeSeconds = Duration * FrameAlpha;
	}
	else if (bHasTimeSeconds)
	{
		SampleTimeSeconds = FMath::Clamp(static_cast<float>(TimeSecondsValue), 0.0f, Duration);
		const float FrameAlpha = Duration > KINDA_SMALL_NUMBER ? SampleTimeSeconds / Duration : 0.0f;
		RequestedFrameIndex = FrameCount > 1
			? FMath::Clamp(StartFrame + FMath::RoundToInt(FrameAlpha * static_cast<float>(FrameCount - 1)), StartFrame, EndFrame)
			: StartFrame;
	}

	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	const TSharedPtr<FJsonObject> Sample = UeAgentDeformerOps::BuildGeometryCacheFrameSampleJson(GeometryCache, SampleTimeSeconds, ValidationIssues);

	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("capture_status"), TEXT("headless_frame_sample"));
	OutData->SetBoolField(TEXT("visual_screenshot_available"), false);
	OutData->SetStringField(TEXT("recommended_visual_flow"), TEXT("spawn or open a GeometryCache preview actor, advance the editor preview normally, then use viewport screenshot commands."));
	OutData->SetNumberField(TEXT("requested_frame_index"), RequestedFrameIndex);
	OutData->SetNumberField(TEXT("requested_time_seconds"), SampleTimeSeconds);
	OutData->SetObjectField(TEXT("sample"), Sample);
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	OutData->SetNumberField(TEXT("validation_error_count"), UeAgentDeformerOps::CountIssuesBySeverity(ValidationIssues, TEXT("error")));
	OutData->SetBoolField(TEXT("validation_passed"), !UeAgentDeformerOps::HasErrorIssue(ValidationIssues));
	return true;
}

bool FUeAgentHttpServer::CmdDeformerGraphCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Classes;
	const FString DefaultClassPath = UeAgentDeformerOps::ResolveFirstAvailableClassPath(TEXT("deformer_graph"), Classes);
	const bool bOk = UeAgentDeformerOps::CreateAssetByClassPath(Ctx, TEXT("deformer_graph"), DefaultClassPath, OutData, OutError);
	OutData->SetArrayField(TEXT("candidate_classes"), Classes);
	return bOk;
}

bool FUeAgentHttpServer::CmdDeformerGraphGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("deformer_graph_not_found");
		return false;
	}
	UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(Asset);
	if (!Deformer)
	{
		OutError = TEXT("asset_is_not_deformer_graph");
		OutData->SetStringField(TEXT("asset_path"), AssetPath);
		OutData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
		return false;
	}
	OutData = UeAgentDeformerOps::BuildAssetInfoJson(Asset, TEXT("deformer_graph"));
	OutData->SetObjectField(TEXT("deformer_graph"), UeAgentDeformerOps::BuildOptimusSettingsJson(Deformer));
	return true;
}

bool FUeAgentHttpServer::CmdDeformerGraphExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdDeformerGraphGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(OutData->GetStringField(TEXT("asset_path")));
	UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(Asset);
	if (!Deformer)
	{
		OutError = TEXT("asset_is_not_deformer_graph");
		return false;
	}
	FString OutputPath;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), OutData, OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings.json")), UeAgentDeformerOps::BuildOptimusSettingsJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("resources.json")), UeAgentDeformerOps::BuildOptimusResourcesJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("variables.json")), UeAgentDeformerOps::BuildOptimusVariablesJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("data_interfaces.json")), UeAgentDeformerOps::BuildOptimusDataInterfacesJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("kernels.json")), UeAgentDeformerOps::BuildOptimusKernelsJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("graphs.json")), UeAgentDeformerOps::BuildOptimusGraphsJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("bindings.json")), UeAgentDeformerOps::BuildOptimusBindingsJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("source_libraries.json")), UeAgentDeformerOps::BuildOptimusSourceLibrariesJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), UeAgentDeformerOps::BuildPropertyExportJson(Asset, TEXT("deformer_graph")), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("readonly_properties.json")), OutData->GetObjectField(TEXT("property_summary")), OutputPath, OutError)) return false;
	TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetStringField(TEXT("profile"), TEXT("deformer_graph"));
	Coverage->SetStringField(TEXT("implementation_status"), TEXT("optimus_summary_export_with_explicit_raw_property_apply_and_real_compile_command"));
	Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
	Coverage->SetBoolField(TEXT("deep_graph_mutation_available"), false);
	Coverage->SetArrayField(TEXT("implemented_profiles"), UeAgentDeformerOps::MakeStringArray({
		TEXT("asset_identity"),
		TEXT("optimus_settings_summary"),
		TEXT("resources_summary"),
		TEXT("variables_summary"),
		TEXT("bindings_summary"),
		TEXT("graphs_nodes_pins_links_summary"),
		TEXT("kernel_provider_summary"),
		TEXT("data_interface_provider_summary"),
		TEXT("source_function_graph_summary"),
		TEXT("raw_uobject_property_apply"),
		TEXT("synchronous_optimus_compile")
	}));
	Coverage->SetArrayField(TEXT("adapter_boundaries"), UeAgentDeformerOps::MakeStringArray({
		TEXT("deep node/kernel/link mutation is intentionally not inferred from summary JSON"),
		TEXT("async shader compiler diagnostics may continue after Compile() returns"),
		TEXT("runtime geometry readback comparison needs scene/runtime adapter")
	}));
	Coverage->SetArrayField(TEXT("blocking_gaps"), {});
	Coverage->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), Coverage, OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("compile_report.json")), UeAgentDeformerOps::BuildOptimusValidationReportJson(Deformer, TEXT("compile_report"), TEXT("not_run_export_only")), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("shader_diagnostics.json")), UeAgentDeformerOps::BuildOptimusValidationReportJson(Deformer, TEXT("shader_diagnostics"), TEXT("not_run_export_only")), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("binding_report.json")), UeAgentDeformerOps::BuildOptimusBindingsJson(Deformer), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("readback_diff.json")), UeAgentDeformerOps::BuildOptimusValidationReportJson(Deformer, TEXT("readback_diff"), TEXT("runtime_readback_adapter_required")), OutputPath, OutError)) return false;
	if (!UeAgentDeformerOps::SaveJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("diagnostics.json")), Coverage, OutputPath, OutError)) return false;
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	return true;
}

bool FUeAgentHttpServer::CmdDeformerGraphValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> AssetJson;
	const bool bOk = UeAgentJsonDiagnostics::LoadJsonObjectFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, &Issues, OutError, false);
	const TArray<FString> OptionalFiles = {
		TEXT("settings.json"),
		TEXT("resources.json"),
		TEXT("variables.json"),
		TEXT("data_interfaces.json"),
		TEXT("kernels.json"),
		TEXT("graphs.json"),
		TEXT("bindings.json"),
		TEXT("source_libraries.json"),
		TEXT("raw_properties.json"),
		TEXT("readonly_properties.json"),
		FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("compile_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("shader_diagnostics.json")),
		FPaths::Combine(TEXT("validation"), TEXT("binding_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json")),
		FPaths::Combine(TEXT("validation"), TEXT("diagnostics.json"))
	};
	const TSet<FString> ReadOnlyOrSummaryFiles = {
		TEXT("settings.json"),
		TEXT("resources.json"),
		TEXT("variables.json"),
		TEXT("data_interfaces.json"),
		TEXT("kernels.json"),
		TEXT("graphs.json"),
		TEXT("bindings.json"),
		TEXT("source_libraries.json"),
		TEXT("readonly_properties.json"),
		FPaths::Combine(TEXT("validation"), TEXT("coverage_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("compile_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("shader_diagnostics.json")),
		FPaths::Combine(TEXT("validation"), TEXT("binding_report.json")),
		FPaths::Combine(TEXT("validation"), TEXT("readback_diff.json")),
		FPaths::Combine(TEXT("validation"), TEXT("diagnostics.json"))
	};
	for (const FString& OptionalFile : OptionalFiles)
	{
		TSharedPtr<FJsonObject> LoadedJson;
		if (!UeAgentJsonDiagnostics::LoadJsonObjectFile(FPaths::Combine(FolderPath, OptionalFile), LoadedJson, &Issues, OutError, true))
		{
			OutData->SetBoolField(TEXT("valid"), false);
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
			OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
			OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
			return false;
		}
		if (ReadOnlyOrSummaryFiles.Contains(OptionalFile))
		{
			UeAgentJsonDiagnostics::AddUnsupportedApplyIssueIfWriteIntent(LoadedJson, OptionalFile, TEXT("Deformer Graph summary profile"), Issues);
		}
	}
	const bool bHasErrors = UeAgentJsonDiagnostics::HasErrorIssue(Issues);
	if (bHasErrors && OutError.IsEmpty())
	{
		OutError = TEXT("json_validation_failed");
	}
	OutData->SetBoolField(TEXT("valid"), bOk && !bHasErrors);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	return bOk && !bHasErrors;
}

bool FUeAgentHttpServer::CmdDeformerGraphApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FolderPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FolderPath);

	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> FolderValidation = MakeShared<FJsonObject>();
	FString FolderValidationError;
	if (!CmdDeformerGraphValidateFolder(Ctx, FolderValidation, FolderValidationError))
	{
		OutError = FolderValidationError.IsEmpty() ? TEXT("json_validation_failed") : FolderValidationError;
		OutData->SetObjectField(TEXT("folder_validation"), FolderValidation);
		if (FolderValidation.IsValid() && FolderValidation->HasTypedField<EJson::Array>(TEXT("json_issues")))
		{
			OutData->SetArrayField(TEXT("json_issues"), FolderValidation->GetArrayField(TEXT("json_issues")));
			OutData->SetNumberField(TEXT("json_issue_count"), FolderValidation->GetIntegerField(TEXT("json_issue_count")));
			OutData->SetNumberField(TEXT("error_count"), FolderValidation->GetIntegerField(TEXT("error_count")));
			OutData->SetNumberField(TEXT("warning_count"), FolderValidation->GetIntegerField(TEXT("warning_count")));
		}
		return false;
	}

	TSharedPtr<FJsonObject> RawPropertiesJson;
	if (!UeAgentJsonDiagnostics::LoadJsonObjectFile(FPaths::Combine(FolderPath, TEXT("raw_properties.json")), RawPropertiesJson, &Issues, OutError, true))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	TSharedPtr<FJsonObject> PropertyApplyParams;
	int32 SelectedPropertyCount = 0;
	if (RawPropertiesJson.IsValid()
		&& !UeAgentDeformerOps::PreparePropertyApplyParams(Ctx, RawPropertiesJson, FString(), TEXT("raw_properties.json"), PropertyApplyParams, SelectedPropertyCount, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	TSharedPtr<FJsonObject> PropertyApplyData = MakeShared<FJsonObject>();
	if (SelectedPropertyCount > 0 && !(bDryRun || bValidateOnly))
	{
		FString PropertyApplyError;
		FUeAgentRequestContext PropertyCtx{ TEXT("deformer_graph_apply_raw_properties"), TEXT("asset_apply_property_json"), PropertyApplyParams };
		if (!CmdAssetApplyPropertyJson(PropertyCtx, PropertyApplyData, PropertyApplyError))
		{
			OutError = PropertyApplyError;
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			OutData->SetObjectField(TEXT("property_apply"), PropertyApplyData);
			return false;
		}
	}

	OutData->SetBoolField(TEXT("applied"), SelectedPropertyCount > 0 && !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetNumberField(TEXT("selected_property_count"), SelectedPropertyCount);
	OutData->SetObjectField(TEXT("property_apply"), PropertyApplyData);
	OutData->SetObjectField(TEXT("folder_validation"), FolderValidation);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetStringField(TEXT("graph_mutation_policy"), TEXT("kernel/node/source-library graph mutation still requires version-specific adapter; raw UObject properties use asset_apply_property_json."));
	return !UeAgentJsonDiagnostics::HasErrorIssue(Issues);
}

bool FUeAgentHttpServer::CmdDeformerGraphCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdDeformerGraphGetInfo(Ctx, OutData, OutError))
	{
		OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
		OutData->SetStringField(TEXT("compile_status"), TEXT("asset_not_found_or_unavailable"));
		return false;
	}
	const FString AssetPath = OutData->GetStringField(TEXT("asset_path"));
	UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(UeAgentDeformerOps::LoadAssetObject(AssetPath));
	if (!Deformer)
	{
		OutError = TEXT("asset_is_not_deformer_graph");
		OutData->SetStringField(TEXT("compile_status"), TEXT("asset_is_not_deformer_graph"));
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	FDelegateHandle CompileMessageHandle = Deformer->GetCompileMessageDelegate().AddLambda(
		[&Diagnostics](const FOptimusCompilerDiagnostic& Diagnostic)
		{
			Diagnostics.Add(MakeShared<FJsonValueObject>(UeAgentDeformerOps::BuildOptimusCompileDiagnosticJson(Diagnostic)));
		});
	const bool bCompileOk = Deformer->Compile();
	Deformer->GetCompileMessageDelegate().Remove(CompileMessageHandle);

	const FString NewStatus = UeAgentDeformerOps::OptimusDeformerStatusToString(Deformer->GetStatus());
	OutData->SetBoolField(TEXT("compile_invoked"), true);
	OutData->SetBoolField(TEXT("compile_returned_ok"), bCompileOk);
	OutData->SetStringField(TEXT("compile_status"), bCompileOk ? TEXT("compiled") : TEXT("compile_failed"));
	OutData->SetStringField(TEXT("deformer_status"), NewStatus);
	OutData->SetArrayField(TEXT("diagnostics"), Diagnostics);
	OutData->SetNumberField(TEXT("diagnostic_count"), Diagnostics.Num());
	OutData->SetStringField(TEXT("diagnostics_scope"), TEXT("synchronous Optimus graph compile messages; shader compilation diagnostics can be async"));
	return bCompileOk;
}

bool FUeAgentHttpServer::CmdDeformerGraphGetCompileLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdDeformerGraphGetInfo(Ctx, OutData, OutError))
	{
		OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
		OutData->SetStringField(TEXT("compile_log_status"), TEXT("asset_not_found_or_unavailable"));
		return false;
	}
	const FString AssetPath = OutData->GetStringField(TEXT("asset_path"));
	UOptimusDeformer* Deformer = Cast<UOptimusDeformer>(UeAgentDeformerOps::LoadAssetObject(AssetPath));
	OutData->SetStringField(TEXT("compile_log_status"), TEXT("asset_status_snapshot"));
	OutData->SetStringField(TEXT("deformer_status"), Deformer ? UeAgentDeformerOps::OptimusDeformerStatusToString(Deformer->GetStatus()) : FString());
	OutData->SetStringField(TEXT("diagnostics_scope"), TEXT("This command reports current Optimus asset status. Use deformer_graph_compile to capture synchronous compile diagnostics."));
	return Deformer != nullptr;
}

bool FUeAgentHttpServer::CmdDeformerSourceLibraryCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ClassPath;
	JsonTryGetString(Ctx.Params, TEXT("class_path"), ClassPath);
	if (ClassPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_class_path");
		OutData->SetStringField(TEXT("reason"), TEXT("UE 5.6 exposes source-library UI through deformer plugin-specific classes; provide class_path for the concrete asset class."));
		OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
		return false;
	}
	return UeAgentDeformerOps::CreateAssetByClassPath(Ctx, TEXT("deformer_source_library"), ClassPath, OutData, OutError);
}

bool FUeAgentHttpServer::CmdDeformerSourceLibraryExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("missing_asset_path"); return false; }
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset) { OutError = TEXT("asset_not_found"); return false; }
	OutData = UeAgentDeformerOps::BuildPropertyExportJson(Asset, TEXT("deformer_source_library"));
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentDeformerOps::SaveJsonFile(OutputFile, OutData, SavedOutputFile, OutError))
	{
		return false;
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdDeformerSourceLibraryValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> JsonRoot;
	FString SourcePath;
	if (!UeAgentDeformerOps::LoadJsonRootForApply(Ctx, JsonRoot, SourcePath, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		OutData->SetBoolField(TEXT("valid"), false);
		return false;
	}
	TSharedPtr<FJsonObject> PropertyApplyParams;
	int32 SelectedPropertyCount = 0;
	const FString ExpectedProfile = UeAgentDeformerOps::ResolveExpectedPropertyProfile(Ctx.Command, TEXT("deformer_source_library"));
	const bool bProfileOk = UeAgentDeformerOps::ValidatePropertyJsonProfile(JsonRoot, SourcePath, ExpectedProfile, Issues, OutError);
	const bool bOk = bProfileOk && UeAgentDeformerOps::PreparePropertyApplyParams(Ctx, JsonRoot, FString(), SourcePath, PropertyApplyParams, SelectedPropertyCount, Issues, OutError);
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetBoolField(TEXT("valid"), bOk);
	OutData->SetStringField(TEXT("expected_profile"), ExpectedProfile);
	OutData->SetNumberField(TEXT("selected_property_count"), SelectedPropertyCount);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetStringField(TEXT("validation_policy"), TEXT("validates explicit property JSON and plugin availability; only apply=true entries are write candidates."));
	return bOk;
}

bool FUeAgentHttpServer::CmdDeformerSourceLibraryApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> JsonRoot;
	FString SourcePath;
	if (!UeAgentDeformerOps::LoadJsonRootForApply(Ctx, JsonRoot, SourcePath, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		return false;
	}

	FString AssetPath;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
	TSharedPtr<FJsonObject> PropertyApplyParams;
	int32 SelectedPropertyCount = 0;
	const FString ExpectedProfile = UeAgentDeformerOps::ResolveExpectedPropertyProfile(Ctx.Command, TEXT("deformer_source_library"));
	if (!UeAgentDeformerOps::ValidatePropertyJsonProfile(JsonRoot, SourcePath, ExpectedProfile, Issues, OutError))
	{
		OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
		OutData->SetStringField(TEXT("expected_profile"), ExpectedProfile);
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
		OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
		return false;
	}
	if (!UeAgentDeformerOps::PreparePropertyApplyParams(Ctx, JsonRoot, AssetPath, SourcePath, PropertyApplyParams, SelectedPropertyCount, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
		OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
		return false;
	}

	bool bDryRun = false;
	bool bValidateOnly = false;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	JsonTryGetBool(Ctx.Params, TEXT("validate_only"), bValidateOnly);
	TSharedPtr<FJsonObject> PropertyApplyData = MakeShared<FJsonObject>();
	if (SelectedPropertyCount > 0 && !(bDryRun || bValidateOnly))
	{
		FString PropertyApplyError;
		FUeAgentRequestContext PropertyCtx{ TEXT("deformer_source_library_apply_properties"), TEXT("asset_apply_property_json"), PropertyApplyParams };
		if (!CmdAssetApplyPropertyJson(PropertyCtx, PropertyApplyData, PropertyApplyError))
		{
			OutError = PropertyApplyError;
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
			OutData->SetObjectField(TEXT("property_apply"), PropertyApplyData);
			return false;
		}
	}

	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetBoolField(TEXT("applied"), SelectedPropertyCount > 0 && !(bDryRun || bValidateOnly));
	OutData->SetBoolField(TEXT("dry_run"), bDryRun || bValidateOnly);
	OutData->SetStringField(TEXT("expected_profile"), ExpectedProfile);
	OutData->SetNumberField(TEXT("selected_property_count"), SelectedPropertyCount);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetObjectField(TEXT("property_apply"), PropertyApplyData);
	return true;
}

bool FUeAgentHttpServer::CmdMeshDeformerCollectionCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Classes;
	const FString DefaultClassPath = UeAgentDeformerOps::ResolveFirstAvailableClassPath(TEXT("mesh_deformer_collection"), Classes);
	const bool bOk = UeAgentDeformerOps::CreateAssetByClassPath(Ctx, TEXT("mesh_deformer_collection"), DefaultClassPath, OutData, OutError);
	OutData->SetArrayField(TEXT("candidate_classes"), Classes);
	return bOk;
}

bool FUeAgentHttpServer::CmdMeshDeformerCollectionExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("missing_asset_path"); return false; }
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset) { OutError = TEXT("asset_not_found"); return false; }
	OutData = UeAgentDeformerOps::BuildPropertyExportJson(Asset, TEXT("mesh_deformer_collection"));
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentDeformerOps::SaveJsonFile(OutputFile, OutData, SavedOutputFile, OutError))
	{
		return false;
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdMeshDeformerCollectionValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdDeformerSourceLibraryValidateJson(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdMeshDeformerCollectionApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdDeformerSourceLibraryApplyJson(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdMLDeformerCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Classes;
	const FString DefaultClassPath = UeAgentDeformerOps::ResolveFirstAvailableClassPath(TEXT("ml_deformer"), Classes);
	const bool bOk = UeAgentDeformerOps::CreateAssetByClassPath(Ctx, TEXT("ml_deformer"), DefaultClassPath, OutData, OutError);
	OutData->SetArrayField(TEXT("candidate_classes"), Classes);
	return bOk;
}

bool FUeAgentHttpServer::CmdMLDeformerGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("missing_asset_path"); return false; }
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset) { OutError = TEXT("asset_not_found"); return false; }
	OutData = UeAgentDeformerOps::BuildAssetInfoJson(Asset, TEXT("ml_deformer"));
	return true;
}

bool FUeAgentHttpServer::CmdMLDeformerExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty()) { OutError = TEXT("missing_asset_path"); return false; }
	UObject* Asset = UeAgentDeformerOps::LoadAssetObject(AssetPath);
	if (!Asset) { OutError = TEXT("asset_not_found"); return false; }
	OutData = UeAgentDeformerOps::BuildPropertyExportJson(Asset, TEXT("ml_deformer"));
	OutData->SetObjectField(
		TEXT("training_inputs"),
		UeAgentDeformerOps::BuildReflectionSectionJson(
			Asset,
			TEXT("ml_deformer"),
			TEXT("training_inputs"),
			{ TEXT("Skeletal"), TEXT("Skeleton"), TEXT("Anim"), TEXT("Sequence"), TEXT("Geometry"), TEXT("Cache"), TEXT("Training"), TEXT("Input"), TEXT("Target"), TEXT("Mesh"), TEXT("Graph") },
			TEXT("reflection_summary; apply selected properties through ml_deformer_apply_json")));
	OutData->SetObjectField(
		TEXT("training_parameters"),
		UeAgentDeformerOps::BuildReflectionSectionJson(
			Asset,
			TEXT("ml_deformer"),
			TEXT("training_parameters"),
			{ TEXT("Training"), TEXT("Train"), TEXT("Epoch"), TEXT("Batch"), TEXT("Learning"), TEXT("Rate"), TEXT("Iteration"), TEXT("Device"), TEXT("Quality"), TEXT("Model"), TEXT("Loss") },
			TEXT("reflection_summary; explicit training requires ml_deformer_train adapter")));
	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty() && !UeAgentDeformerOps::SaveJsonFile(OutputFile, OutData, SavedOutputFile, OutError))
	{
		return false;
	}
	if (!SavedOutputFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdMLDeformerValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> JsonRoot;
	FString SourcePath;
	if (!UeAgentDeformerOps::LoadJsonRootForApply(Ctx, JsonRoot, SourcePath, Issues, OutError))
	{
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
		OutData->SetBoolField(TEXT("valid"), false);
		return false;
	}
	TSharedPtr<FJsonObject> PropertyApplyParams;
	int32 SelectedPropertyCount = 0;
	const FString ExpectedProfile = TEXT("ml_deformer");
	const bool bProfileOk = UeAgentDeformerOps::ValidatePropertyJsonProfile(JsonRoot, SourcePath, ExpectedProfile, Issues, OutError);
	const bool bOk = bProfileOk && UeAgentDeformerOps::PreparePropertyApplyParams(Ctx, JsonRoot, FString(), SourcePath, PropertyApplyParams, SelectedPropertyCount, Issues, OutError);
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetBoolField(TEXT("valid"), bOk);
	OutData->SetStringField(TEXT("expected_profile"), ExpectedProfile);
	OutData->SetNumberField(TEXT("selected_property_count"), SelectedPropertyCount);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetNumberField(TEXT("json_issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error")));
	OutData->SetNumberField(TEXT("warning_count"), UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("warning")));
	OutData->SetStringField(TEXT("validation_policy"), TEXT("checks plugin/class availability and explicit property JSON; training is never implicit"));
	return bOk;
}

bool FUeAgentHttpServer::CmdMLDeformerApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const bool bOk = CmdDeformerSourceLibraryApplyJson(Ctx, OutData, OutError);
	OutData->SetStringField(TEXT("training_status"), TEXT("no_training_started"));
	OutData->SetStringField(TEXT("training_policy"), TEXT("ml_deformer_apply_json never starts training; use ml_deformer_train with explicit confirmation."));
	return bOk;
}

bool FUeAgentHttpServer::CmdMLDeformerValidateTrainingInputs(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	auto ReadStringAlias = [this, &Ctx](std::initializer_list<const TCHAR*> Aliases, FString& OutValue) -> bool
	{
		for (const TCHAR* Alias : Aliases)
		{
			if (JsonTryGetString(Ctx.Params, Alias, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
			{
				return true;
			}
		}
		OutValue.Reset();
		return false;
	};

	FString SkeletalMeshPath;
	FString AnimSequencePath;
	FString GeometryCachePath;
	FString DeformerGraphPath;
	ReadStringAlias({ TEXT("skeletal_mesh"), TEXT("skeletal_mesh_path") }, SkeletalMeshPath);
	ReadStringAlias({ TEXT("training_anim_sequence"), TEXT("anim_sequence"), TEXT("animation_sequence"), TEXT("animation") }, AnimSequencePath);
	ReadStringAlias({ TEXT("target_geometry_cache"), TEXT("geometry_cache"), TEXT("geometry_cache_path") }, GeometryCachePath);
	ReadStringAlias({ TEXT("deformer_graph"), TEXT("deformer_graph_path") }, DeformerGraphPath);

	UObject* SkeletalMeshAsset = SkeletalMeshPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDeformerOps::LoadAssetObject(SkeletalMeshPath);
	UObject* AnimSequenceAsset = AnimSequencePath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDeformerOps::LoadAssetObject(AnimSequencePath);
	UObject* GeometryCacheAsset = GeometryCachePath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDeformerOps::LoadAssetObject(GeometryCachePath);
	UObject* DeformerGraphAsset = DeformerGraphPath.TrimStartAndEnd().IsEmpty() ? nullptr : UeAgentDeformerOps::LoadAssetObject(DeformerGraphPath);
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(SkeletalMeshAsset);
	UAnimSequence* AnimSequence = Cast<UAnimSequence>(AnimSequenceAsset);
	UGeometryCache* GeometryCache = Cast<UGeometryCache>(GeometryCacheAsset);

	TArray<TSharedPtr<FJsonValue>> ValidationIssues;
	TArray<TSharedPtr<FJsonValue>> Inputs;
	auto AddInput = [&Inputs](const TCHAR* Field, const FString& Path, bool bRequired, UObject* Asset, UClass* ExpectedClass)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("field"), Field);
		Item->SetStringField(TEXT("path"), Path);
		Item->SetBoolField(TEXT("required"), bRequired);
		Item->SetBoolField(TEXT("found"), Asset != nullptr);
		Item->SetStringField(TEXT("class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		Item->SetStringField(TEXT("expected_class"), ExpectedClass ? ExpectedClass->GetPathName() : TEXT(""));
		Item->SetBoolField(TEXT("valid_type"), Asset && ExpectedClass ? Asset->IsA(ExpectedClass) : Asset != nullptr);
		Inputs.Add(MakeShared<FJsonValueObject>(Item));
	};
	AddInput(TEXT("skeletal_mesh"), SkeletalMeshPath, true, SkeletalMeshAsset, USkeletalMesh::StaticClass());
	AddInput(TEXT("training_anim_sequence"), AnimSequencePath, true, AnimSequenceAsset, UAnimSequence::StaticClass());
	AddInput(TEXT("target_geometry_cache"), GeometryCachePath, true, GeometryCacheAsset, UGeometryCache::StaticClass());
	AddInput(TEXT("deformer_graph"), DeformerGraphPath, false, DeformerGraphAsset, nullptr);

	auto AddRequiredAssetIssue = [&ValidationIssues](const FString& Path, UObject* Asset, UObject* TypedAsset, const TCHAR* MissingCode, const TCHAR* NotFoundCode, const TCHAR* WrongTypeCode, const TCHAR* Field, const TCHAR* ExpectedType)
	{
		if (Path.TrimStartAndEnd().IsEmpty())
		{
			UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), MissingCode, Field, FString::Printf(TEXT("Required %s path was not provided."), ExpectedType));
		}
		else if (!Asset)
		{
			UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), NotFoundCode, Field, FString::Printf(TEXT("%s asset was not found."), ExpectedType));
		}
		else if (!TypedAsset)
		{
			UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), WrongTypeCode, Field, FString::Printf(TEXT("Asset exists but is not a %s."), ExpectedType));
		}
	};
	AddRequiredAssetIssue(SkeletalMeshPath, SkeletalMeshAsset, SkeletalMesh, TEXT("missing_skeletal_mesh"), TEXT("skeletal_mesh_not_found"), TEXT("asset_is_not_skeletal_mesh"), TEXT("skeletal_mesh"), TEXT("SkeletalMesh"));
	AddRequiredAssetIssue(AnimSequencePath, AnimSequenceAsset, AnimSequence, TEXT("missing_training_anim_sequence"), TEXT("training_anim_sequence_not_found"), TEXT("asset_is_not_anim_sequence"), TEXT("training_anim_sequence"), TEXT("AnimSequence"));
	AddRequiredAssetIssue(GeometryCachePath, GeometryCacheAsset, GeometryCache, TEXT("missing_target_geometry_cache"), TEXT("target_geometry_cache_not_found"), TEXT("asset_is_not_geometry_cache"), TEXT("target_geometry_cache"), TEXT("Geometry Cache"));
	if (!DeformerGraphPath.TrimStartAndEnd().IsEmpty() && !DeformerGraphAsset)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("warning"), TEXT("deformer_graph_not_found"), TEXT("deformer_graph"), TEXT("Deformer Graph asset path was provided but could not be loaded."));
	}

	const int32 SkeletalVertexCount = UeAgentDeformerOps::GetSkeletalMeshLod0VertexCount(SkeletalMesh);
	const int32 CacheVertexCountStart = UeAgentDeformerOps::GetGeometryCacheVertexCountAtTime(GeometryCache, 0.0f);
	const float GeometryCacheDuration = GeometryCache ? FMath::Max(GeometryCache->CalculateDuration(), 0.0f) : 0.0f;
	const int32 CacheVertexCountMid = GeometryCache ? UeAgentDeformerOps::GetGeometryCacheVertexCountAtTime(GeometryCache, GeometryCacheDuration * 0.5f) : INDEX_NONE;
	const int32 CacheVertexCountEnd = GeometryCache ? UeAgentDeformerOps::GetGeometryCacheVertexCountAtTime(GeometryCache, GeometryCacheDuration) : INDEX_NONE;
	const float AnimDuration = AnimSequence ? AnimSequence->GetPlayLength() : 0.0f;
	double DurationToleranceValue = 1.0 / 30.0;
	JsonTryGetNumber(Ctx.Params, TEXT("duration_tolerance_seconds"), DurationToleranceValue);
	bool bStrictDurationMatch = false;
	JsonTryGetBool(Ctx.Params, TEXT("strict_duration_match"), bStrictDurationMatch);

	USkeleton* SkeletalMeshSkeleton = SkeletalMesh ? SkeletalMesh->GetSkeleton() : nullptr;
	USkeleton* AnimSkeleton = AnimSequence ? AnimSequence->GetSkeleton() : nullptr;
	const bool bCanCompareSkeleton = SkeletalMesh && AnimSequence;
	const bool bSkeletonCompatible = bCanCompareSkeleton && SkeletalMeshSkeleton && AnimSkeleton && SkeletalMeshSkeleton == AnimSkeleton;
	if (bCanCompareSkeleton && (!SkeletalMeshSkeleton || !AnimSkeleton))
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("skeleton_missing"), TEXT("skeleton"), TEXT("SkeletalMesh or AnimSequence is missing a Skeleton reference."));
	}
	else if (bCanCompareSkeleton && !bSkeletonCompatible)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("skeleton_mismatch"), TEXT("skeleton"), TEXT("SkeletalMesh and training AnimSequence do not reference the same Skeleton."));
	}

	const bool bCanCompareTopology = SkeletalVertexCount >= 0 && CacheVertexCountStart >= 0;
	const bool bTopologyVertexCountMatches = bCanCompareTopology && SkeletalVertexCount == CacheVertexCountStart;
	if (SkeletalMesh && GeometryCache && !bCanCompareTopology)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("warning"), TEXT("topology_vertex_count_unavailable"), TEXT("vertex_count"), TEXT("Unable to read both Geometry Cache sample vertices and SkeletalMesh LOD0 vertices."));
	}
	else if (SkeletalMesh && GeometryCache && !bTopologyVertexCountMatches)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("topology_vertex_count_mismatch"), TEXT("vertex_count"), TEXT("Geometry Cache first sample vertex count does not match SkeletalMesh LOD0 vertex count."));
	}
	if (GeometryCache && CacheVertexCountStart >= 0 && (CacheVertexCountMid >= 0 || CacheVertexCountEnd >= 0))
	{
		const bool bCacheTopologyStable = (CacheVertexCountMid < 0 || CacheVertexCountMid == CacheVertexCountStart)
			&& (CacheVertexCountEnd < 0 || CacheVertexCountEnd == CacheVertexCountStart);
		if (!bCacheTopologyStable)
		{
			UeAgentJsonDiagnostics::AddIssue(ValidationIssues, TEXT("error"), TEXT("geometry_cache_topology_not_constant"), TEXT("target_geometry_cache"), TEXT("Geometry Cache sample vertex counts differ between start, middle and end."));
		}
	}

	const double DurationDelta = (AnimSequence && GeometryCache) ? FMath::Abs(static_cast<double>(AnimDuration) - static_cast<double>(GeometryCacheDuration)) : 0.0;
	const bool bDurationCompatible = !(AnimSequence && GeometryCache) || DurationDelta <= FMath::Max(DurationToleranceValue, 0.0);
	if (AnimSequence && GeometryCache && !bDurationCompatible)
	{
		UeAgentJsonDiagnostics::AddIssue(ValidationIssues, bStrictDurationMatch ? TEXT("error") : TEXT("warning"), TEXT("duration_mismatch"), TEXT("duration"), TEXT("Training AnimSequence and Geometry Cache durations differ beyond duration_tolerance_seconds."));
	}

	TSharedPtr<FJsonObject> SkeletonObj = MakeShared<FJsonObject>();
	SkeletonObj->SetStringField(TEXT("skeletal_mesh_skeleton"), SkeletalMeshSkeleton ? SkeletalMeshSkeleton->GetPathName() : TEXT(""));
	SkeletonObj->SetStringField(TEXT("animation_skeleton"), AnimSkeleton ? AnimSkeleton->GetPathName() : TEXT(""));
	SkeletonObj->SetBoolField(TEXT("compatible"), bSkeletonCompatible);

	TSharedPtr<FJsonObject> TopologyObj = MakeShared<FJsonObject>();
	TopologyObj->SetNumberField(TEXT("skeletal_mesh_lod0_vertex_count"), SkeletalVertexCount);
	TopologyObj->SetNumberField(TEXT("geometry_cache_start_vertex_count"), CacheVertexCountStart);
	TopologyObj->SetNumberField(TEXT("geometry_cache_mid_vertex_count"), CacheVertexCountMid);
	TopologyObj->SetNumberField(TEXT("geometry_cache_end_vertex_count"), CacheVertexCountEnd);
	TopologyObj->SetBoolField(TEXT("can_compare"), bCanCompareTopology);
	TopologyObj->SetBoolField(TEXT("vertex_count_matches"), bTopologyVertexCountMatches);

	TSharedPtr<FJsonObject> TimingObj = MakeShared<FJsonObject>();
	TimingObj->SetNumberField(TEXT("training_anim_duration"), AnimDuration);
	TimingObj->SetNumberField(TEXT("geometry_cache_duration"), GeometryCacheDuration);
	TimingObj->SetNumberField(TEXT("duration_delta_seconds"), DurationDelta);
	TimingObj->SetNumberField(TEXT("duration_tolerance_seconds"), DurationToleranceValue);
	TimingObj->SetBoolField(TEXT("strict_duration_match"), bStrictDurationMatch);
	TimingObj->SetBoolField(TEXT("duration_compatible"), bDurationCompatible);

	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetArrayField(TEXT("inputs"), Inputs);
	OutData->SetObjectField(TEXT("skeleton_check"), SkeletonObj);
	OutData->SetObjectField(TEXT("topology_check"), TopologyObj);
	OutData->SetObjectField(TEXT("timing_check"), TimingObj);
	OutData->SetStringField(TEXT("deformer_graph_class"), DeformerGraphAsset && DeformerGraphAsset->GetClass() ? DeformerGraphAsset->GetClass()->GetPathName() : TEXT(""));
	OutData->SetStringField(TEXT("validation_policy"), TEXT("checks required asset types, Skeleton match, SkeletalMesh LOD0 vs Geometry Cache sample vertex counts, Geometry Cache topology stability, and AnimSequence/GeometryCache duration tolerance; model-specific training adapters still own final semantic validation"));
	OutData->SetArrayField(TEXT("validation_issues"), ValidationIssues);
	OutData->SetNumberField(TEXT("validation_issue_count"), ValidationIssues.Num());
	OutData->SetNumberField(TEXT("validation_error_count"), UeAgentDeformerOps::CountIssuesBySeverity(ValidationIssues, TEXT("error")));
	OutData->SetNumberField(TEXT("validation_warning_count"), UeAgentDeformerOps::CountIssuesBySeverity(ValidationIssues, TEXT("warning")));
	OutData->SetBoolField(TEXT("validation_passed"), !UeAgentDeformerOps::HasErrorIssue(ValidationIssues));
	return true;
}

bool FUeAgentHttpServer::CmdMLDeformerGetTrainingLog(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString LogFile;
	if (JsonTryGetString(Ctx.Params, TEXT("log_file"), LogFile) && !LogFile.TrimStartAndEnd().IsEmpty())
	{
		LogFile = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(LogFile);
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *LogFile))
		{
			OutData->SetStringField(TEXT("log_file"), LogFile);
			OutData->SetStringField(TEXT("tail"), Text.Right(8000));
			return true;
		}
	}
	OutData->SetStringField(TEXT("status"), TEXT("training_log_not_found"));
	return true;
}

bool FUeAgentHttpServer::CmdMLDeformerTrain(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bExplicitConfirm = false;
	JsonTryGetBool(Ctx.Params, TEXT("confirm_long_running_training"), bExplicitConfirm);
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetBoolField(TEXT("confirm_long_running_training"), bExplicitConfirm);
	OutData->SetStringField(TEXT("status"), TEXT("training_adapter_not_available"));
	OutData->SetStringField(TEXT("reason"), TEXT("Training is long-running and model-specific; this command currently refuses implicit training and reports required confirmation/adapter state."));
	return true;
}

bool FUeAgentHttpServer::CmdMLDeformerPreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	OutData->SetObjectField(TEXT("plugin_status"), UeAgentDeformerOps::MakePluginStatusJson());
	OutData->SetStringField(TEXT("status"), TEXT("preview_adapter_not_available"));
	return true;
}
