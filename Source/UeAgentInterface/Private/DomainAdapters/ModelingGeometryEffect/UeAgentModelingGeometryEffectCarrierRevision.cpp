#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/MemoryWriter.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using UE::Geometry::FDynamicMesh3;

	namespace
	{
		FString RevisionTarget(const UObject& Carrier)
		{
			return Carrier.IsA<ADynamicMeshActor>() || Carrier.IsA<UStaticMesh>() ? Carrier.GetPathName() : FString();
		}

		FExecutionMutationError RevisionFact(const FString& Code, const FString& Reason, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			FExecutionMutationError Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field,
				MoveTemp(Expected), MoveTemp(Actual), TEXT("execution"), Reason);
			Error.SourceKind = TEXT("native_result"); Error.Source.JsonPath.Reset(); Error.SemanticPointer.Reset(); Error.TargetPath = Target;
			return Error;
		}

		class FCarrierRevisionHash final
		{
		public:
			FCarrierRevisionHash()
			{
				const int32 Status = SHA256_Init(&Context);
				bValid = Status == 1;
				if (!bValid)
				{
					FailureDiagnostic = RevisionFact(TEXT("modeling_geometry_revision_hash_failed"),
						TEXT("Native modeling revision hash initialization failed."), TEXT("/revision_hash/initialize_status"),
						MakeShared<FJsonValueNumber>(Status), MakeShared<FJsonValueNumber>(1));
					FailureDiagnostic.InternalContext = TEXT("SHA256_Init");
				}
			}

			void AddBytes(const void* Data, const uint64 Size)
			{
				if (bValid && Size > 0)
				{
					const int32 Status = SHA256_Update(&Context, Data, static_cast<size_t>(Size));
					bValid = Status == 1;
					if (!bValid)
					{
						FailureDiagnostic = RevisionFact(TEXT("modeling_geometry_revision_hash_failed"),
							TEXT("Native modeling revision hash update failed."), TEXT("/revision_hash/update_status"),
							MakeShared<FJsonValueNumber>(Status), MakeShared<FJsonValueNumber>(1));
						FailureDiagnostic.InternalContext = TEXT("SHA256_Update");
						FailureDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%llu"), Size));
					}
				}
			}

			void AddUInt8(const uint8 Value)
			{
				AddBytes(&Value, sizeof(Value));
			}

			void AddUInt32(const uint32 Value)
			{
				const uint8 Bytes[] = {
					static_cast<uint8>((Value >> 24) & 0xff),
					static_cast<uint8>((Value >> 16) & 0xff),
					static_cast<uint8>((Value >> 8) & 0xff),
					static_cast<uint8>(Value & 0xff),
				};
				AddBytes(Bytes, sizeof(Bytes));
			}

			void AddUInt64(const uint64 Value)
			{
				uint8 Bytes[8];
				for (int32 Index = 0; Index < 8; ++Index)
				{
					Bytes[Index] = static_cast<uint8>(
						(Value >> (56 - Index * 8)) & 0xff);
				}
				AddBytes(Bytes, sizeof(Bytes));
			}

			void AddInt32(const int32 Value)
			{
				AddUInt32(static_cast<uint32>(Value));
			}

			void AddString(const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				AddUInt64(static_cast<uint64>(Utf8.Length()));
				AddBytes(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
			}

			void AddName(const FName& Value)
			{
				AddString(Value.ToString());
			}

			void AddFloat(const float Value)
			{
				const float Normalized = Value == 0.0f ? 0.0f : Value;
				uint32 Bits = 0;
				FMemory::Memcpy(&Bits, &Normalized, sizeof(Bits));
				AddUInt32(Bits);
			}

			void AddDouble(const double Value)
			{
				const double Normalized = Value == 0.0 ? 0.0 : Value;
				uint64 Bits = 0;
				FMemory::Memcpy(&Bits, &Normalized, sizeof(Bits));
				AddUInt64(Bits);
			}

			void AddVector(const FVector& Value)
			{
				AddDouble(Value.X);
				AddDouble(Value.Y);
				AddDouble(Value.Z);
			}

			void AddRotator(const FRotator& Value)
			{
				AddDouble(Value.Pitch);
				AddDouble(Value.Yaw);
				AddDouble(Value.Roll);
			}

			void AddTransform(const FTransform& Value)
			{
				AddVector(Value.GetTranslation());
				const FQuat Rotation = Value.GetRotation();
				AddDouble(Rotation.X);
				AddDouble(Rotation.Y);
				AddDouble(Rotation.Z);
				AddDouble(Rotation.W);
				AddVector(Value.GetScale3D());
			}

			bool Finalize(int64& OutSequence, FExecutionMutationError& OutError)
			{
				OutSequence = -1;
				if (!bValid)
				{
					OutError = FailureDiagnostic;
					return false;
				}
				uint8 Digest[SHA256_DIGEST_LENGTH] = {};
				const int32 Status = SHA256_Final(Digest, &Context);
				if (Status != 1)
				{
					OutError = RevisionFact(TEXT("modeling_geometry_revision_hash_failed"),
						TEXT("Native modeling revision hash finalization failed."), TEXT("/revision_hash/finalize_status"),
						MakeShared<FJsonValueNumber>(Status), MakeShared<FJsonValueNumber>(1));
					OutError.InternalContext = TEXT("SHA256_Final");
					return false;
				}
				uint64 Prefix = 0;
				for (int32 Index = 0; Index < 8; ++Index)
				{
					Prefix = (Prefix << 8) | Digest[Index];
				}
				constexpr uint64 ExactJsonMask = (1ULL << 53) - 1ULL;
				OutSequence = static_cast<int64>(Prefix & ExactJsonMask);
				return true;
			}

		private:
			SHA256_CTX Context{};
			bool bValid = false;
			FExecutionMutationError FailureDiagnostic;
		};

		void HashMaterial(
			FCarrierRevisionHash& Hash,
			const UMaterialInterface* Material)
		{
			Hash.AddString(
				Material ? Material->GetPathName() : FString());
		}

		void HashShapeBase(
			FCarrierRevisionHash& Hash,
			const FKShapeElem& Shape)
		{
			Hash.AddName(Shape.GetName());
			Hash.AddInt32(static_cast<int32>(Shape.GetShapeType()));
			Hash.AddUInt8(Shape.GetContributeToMass() ? 1 : 0);
			Hash.AddInt32(static_cast<int32>(Shape.GetCollisionEnabled()));
			Hash.AddFloat(Shape.RestOffset);
		}

		void HashBodySetup(
			FCarrierRevisionHash& Hash,
			const UBodySetup* BodySetup)
		{
			Hash.AddUInt8(BodySetup ? 1 : 0);
			if (!BodySetup)
			{
				return;
			}
			Hash.AddInt32(
				static_cast<int32>(BodySetup->GetCollisionTraceFlag()));
			Hash.AddUInt8(BodySetup->bDoubleSidedGeometry ? 1 : 0);
			Hash.AddUInt8(BodySetup->bMeshCollideAll ? 1 : 0);
			Hash.AddUInt8(
				BodySetup->bNeverNeedsCookedCollisionData ? 1 : 0);
			Hash.AddVector(BodySetup->BuildScale3D);

			const FKAggregateGeom& Geometry = BodySetup->AggGeom;
			Hash.AddInt32(Geometry.BoxElems.Num());
			for (const FKBoxElem& Shape : Geometry.BoxElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.X);
				Hash.AddFloat(Shape.Y);
				Hash.AddFloat(Shape.Z);
			}
			Hash.AddInt32(Geometry.SphereElems.Num());
			for (const FKSphereElem& Shape : Geometry.SphereElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddFloat(Shape.Radius);
			}
			Hash.AddInt32(Geometry.SphylElems.Num());
			for (const FKSphylElem& Shape : Geometry.SphylElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.Radius);
				Hash.AddFloat(Shape.Length);
			}
			Hash.AddInt32(Geometry.TaperedCapsuleElems.Num());
			for (const FKTaperedCapsuleElem& Shape
				: Geometry.TaperedCapsuleElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddVector(Shape.Center);
				Hash.AddRotator(Shape.Rotation);
				Hash.AddFloat(Shape.Radius0);
				Hash.AddFloat(Shape.Radius1);
				Hash.AddFloat(Shape.Length);
				Hash.AddUInt8(Shape.bOneSidedCollision ? 1 : 0);
			}
			Hash.AddInt32(Geometry.ConvexElems.Num());
			for (const FKConvexElem& Shape : Geometry.ConvexElems)
			{
				HashShapeBase(Hash, Shape);
				Hash.AddTransform(Shape.GetTransform());
				Hash.AddInt32(Shape.VertexData.Num());
				for (const FVector& Vertex : Shape.VertexData)
				{
					Hash.AddVector(Vertex);
				}
				Hash.AddInt32(Shape.IndexData.Num());
				for (const int32 Index : Shape.IndexData)
				{
					Hash.AddInt32(Index);
				}
			}
			Hash.AddInt32(Geometry.LevelSetElems.Num());
			Hash.AddInt32(Geometry.SkinnedLevelSetElems.Num());
			Hash.AddInt32(Geometry.MLLevelSetElems.Num());
			Hash.AddInt32(Geometry.SkinnedTriangleMeshElems.Num());
		}

		bool HashDynamicMesh(
			FCarrierRevisionHash& Hash,
			const FDynamicMesh3& Mesh, FExecutionMutationError& OutError)
		{
			FDynamicMesh3 Copy(Mesh);
			TArray<uint8> Serialized;
			FMemoryWriter Writer(Serialized, false);
			// A non-transacting archive selects UE's CompactData serializer. This
			// canonicalizes sparse element IDs created by topology edits exactly as
			// durable DynamicMesh serialization does, so the semantic revision does
			// not drift solely because the carrier was saved and reloaded.
			Copy.Serialize(Writer);
			Writer.Close();
			if (Writer.IsError())
			{
				OutError = RevisionFact(TEXT("modeling_geometry_revision_mesh_read_failed"),
					TEXT("The dynamic mesh could not be serialized for modeling revision calculation."), TEXT("/dynamic_mesh/archive_error"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}
			Hash.AddString(TEXT("ue5.6-dynamic-mesh-compact-v2"));
			Hash.AddUInt64(static_cast<uint64>(Serialized.Num()));
			Hash.AddBytes(Serialized.GetData(), Serialized.Num());

			TArray<FName> GenericAttributeNames;
			if (const UE::Geometry::FDynamicMeshAttributeSet* Attributes =
				Mesh.Attributes())
			{
				Attributes->GetAttachedAttributes().GenerateKeyArray(
					GenericAttributeNames);
				GenericAttributeNames.Sort(FNameLexicalLess());
			}
			Hash.AddInt32(GenericAttributeNames.Num());
			for (const FName& Name : GenericAttributeNames)
			{
				Hash.AddName(Name);
			}
			return true;
		}

		bool HashMeshDescription(
			FCarrierRevisionHash& Hash,
			const FMeshDescription& Description, FExecutionMutationError& OutError)
		{
			FMeshDescription Copy(Description);
			TArray<uint8> Serialized;
			FMemoryWriter Writer(Serialized, false);
			Copy.Serialize(Writer);
			Writer.Close();
			if (Writer.IsError())
			{
				OutError = RevisionFact(TEXT("modeling_geometry_revision_mesh_read_failed"),
					TEXT("The static mesh description could not be serialized for modeling revision calculation."), TEXT("/mesh_description/archive_error"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return false;
			}
			Hash.AddString(TEXT("ue5.6-mesh-description-v1"));
			Hash.AddUInt64(static_cast<uint64>(Serialized.Num()));
			Hash.AddBytes(Serialized.GetData(), Serialized.Num());
			return true;
		}

		FString CarrierIdentity(UObject& Carrier, const FString& Kind)
		{
			FString StableIdentity;
			if (const ADynamicMeshActor* Actor =
				Cast<ADynamicMeshActor>(&Carrier))
			{
				const FGuid ActorGuid = Actor->GetActorGuid();
				StableIdentity = ActorGuid.IsValid()
					? TEXT("actor-guid:")
						+ ActorGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
					: FString::Printf(
						TEXT("object-uid:%u"),
						Carrier.GetUniqueID());
			}
			else
			{
				const UPackage* Package = Carrier.GetOutermost();
				const FGuid PackageGuid = Package
					? Package->GetPersistentGuid()
					: FGuid();
				StableIdentity = PackageGuid.IsValid()
					? TEXT("package-guid:")
						+ PackageGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
					: FString::Printf(
						TEXT("object-uid:%u"),
						Carrier.GetUniqueID());
			}
			const FString AlgorithmGeneration =
				Kind == TEXT("dynamic_mesh_actor")
					? TEXT("modeling-carrier-v2")
					: TEXT("modeling-carrier-v1");
			return AlgorithmGeneration + TEXT(":") + Kind + TEXT(":")
				+ Carrier.GetPathName() + TEXT(":") + StableIdentity;
		}
	}

	bool IsModelingGeometryCarrier(const UObject& Object)
	{
		return Object.IsA<ADynamicMeshActor>()
			|| Object.IsA<UStaticMesh>();
	}

	bool TryReadModelingGeometryCarrierRevision(
		const FString& SourceId,
		UObject& Carrier,
		TOptional<FUeAgentStateRevision>& OutRevision,
		FExecutionMutationError& OutError)
	{
		OutRevision.Reset();
		OutError = FExecutionMutationError();
		if (SourceId.IsEmpty())
		{
			OutError = RevisionFact(TEXT("modeling_geometry_revision_source_missing"),
				TEXT("A non-empty modeling revision source identity is required."), TEXT("/revision/source_identity_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), RevisionTarget(Carrier));
			return false;
		}

		FCarrierRevisionHash Hash;
		FString CarrierKind;
		if (ADynamicMeshActor* Actor = Cast<ADynamicMeshActor>(&Carrier))
		{
			CarrierKind = TEXT("dynamic_mesh_actor");
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!Component || !DynamicMesh)
			{
				OutError = RevisionFact(TEXT("modeling_geometry_dynamic_mesh_missing"), Component
					? TEXT("The modeling actor's dynamic mesh component has no mesh for revision calculation.")
					: TEXT("The modeling actor has no dynamic mesh component for revision calculation."), Component
					? TEXT("/dynamic_mesh_available") : TEXT("/dynamic_mesh_component_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Carrier.GetPathName());
				return false;
			}
			Hash.AddTransform(Actor->GetActorTransform());
			Hash.AddTransform(Component->GetRelativeTransform());
			Hash.AddInt32(static_cast<int32>(Component->GetCollisionEnabled()));
			Hash.AddName(Component->GetCollisionProfileName());
			Hash.AddUInt8(Component->GetGenerateOverlapEvents() ? 1 : 0);
			Hash.AddInt32(Component->GetNumMaterials());
			const TArray<FName> SlotNames = Component->GetMaterialSlotNames();
			Hash.AddInt32(SlotNames.Num());
			for (const FName& SlotName : SlotNames)
			{
				Hash.AddName(SlotName);
			}
			for (int32 Index = 0;
				Index < Component->GetNumMaterials();
				++Index)
			{
				HashMaterial(Hash, Component->GetMaterial(Index));
			}
			HashBodySetup(Hash, Component->GetBodySetup());
			bool bMeshRead = false;
			bool bMeshVisited = false;
			DynamicMesh->ProcessMesh([&](const FDynamicMesh3& Mesh)
			{
				bMeshVisited = true;
				bMeshRead = HashDynamicMesh(Hash, Mesh, OutError);
			});
			if (!bMeshVisited)
			{
				OutError = RevisionFact(TEXT("modeling_geometry_revision_mesh_read_failed"),
					TEXT("The dynamic mesh read callback did not execute during revision calculation."), TEXT("/dynamic_mesh/read_callback_invoked"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Carrier.GetPathName());
				return false;
			}
			if (!bMeshRead)
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Carrier.GetPathName();
				return false;
			}
		}
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(&Carrier))
		{
			CarrierKind = TEXT("static_mesh_asset");
			const FMeshDescription* Description =
				StaticMesh->GetMeshDescription(0);
			if (!Description)
			{
				OutError = RevisionFact(TEXT("modeling_geometry_static_mesh_source_missing"),
					TEXT("The static mesh has no LOD 0 mesh description for revision calculation."), TEXT("/lods/0/mesh_description_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Carrier.GetPathName());
				return false;
			}
			if (!HashMeshDescription(Hash, *Description, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Carrier.GetPathName();
				return false;
			}
			Hash.AddInt32(StaticMesh->GetStaticMaterials().Num());
			for (const FStaticMaterial& Material
				: StaticMesh->GetStaticMaterials())
			{
				Hash.AddName(Material.MaterialSlotName);
				Hash.AddName(Material.ImportedMaterialSlotName);
				HashMaterial(Hash, Material.MaterialInterface);
			}
			HashBodySetup(Hash, StaticMesh->GetBodySetup());
		}
		else
		{
			OutError = RevisionFact(TEXT("modeling_geometry_carrier_kind_unsupported"),
				TEXT("Modeling revisions support DynamicMeshActor and StaticMesh carriers."), TEXT("/carrier/class"),
				MakeShared<FJsonValueString>(Carrier.GetClass()->GetPathName()), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueString>(ADynamicMeshActor::StaticClass()->GetPathName()), MakeShared<FJsonValueString>(UStaticMesh::StaticClass()->GetPathName())}));
			OutError.InternalContext = Carrier.GetPathName();
			return false;
		}

		const FString Generation = CarrierIdentity(Carrier, CarrierKind);
		Hash.AddString(TEXT("modeling-carrier-revision-v1"));
		Hash.AddString(SourceId);
		Hash.AddString(CarrierKind);
		Hash.AddString(Generation);
		Hash.AddString(Carrier.GetClass()->GetPathName());
		int64 Sequence = -1;
		if (!Hash.Finalize(Sequence, OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Carrier.GetPathName();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SourceId;
			return false;
		}
		FUeAgentStateRevision Revision(SourceId, Generation, Sequence);
		if (!Revision.IsValid())
		{
			OutError = Contract::FDiagnostic::WrapFailure(TEXT("modeling_geometry_revision_invalid"), Revision.GetValidationDiagnostic(),
				TEXT("execution"), TEXT("The modeling carrier revision could not be constructed."));
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Carrier.GetPathName();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SourceId;
			return false;
		}
		OutRevision.Emplace(MoveTemp(Revision));
		return true;
	}
}
