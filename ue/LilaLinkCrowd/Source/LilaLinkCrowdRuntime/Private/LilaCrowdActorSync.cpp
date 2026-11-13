#include "LilaCrowdActorSync.h"

#include "Mass/EntityFragments.h"
#include "MassActorSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaCrowdActorSync)

ULilaCrowdActorSync::ULilaCrowdActorSync()
	: EntityQuery(*this)
{
	ExecutionFlags = int32(EProcessorExecutionFlags::AllNetModes);
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::Representation);
	bRequiresGameThreadExecution = true;
}

void ULilaCrowdActorSync::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddTagRequirement<FLilaCrowdAgentTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassActorFragment>(EMassFragmentAccess::ReadWrite);
}

void ULilaCrowdActorSync::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaCrowdActorSync::Execute);

	EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Chunk) {
		const TConstArrayView<FTransformFragment> Transforms = Chunk.GetFragmentView<FTransformFragment>();
		const TArrayView<FMassActorFragment> Actors = Chunk.GetMutableFragmentView<FMassActorFragment>();
		for (int32 I = 0; I < Chunk.GetNumEntities(); ++I)
		{
			if (AActor* Actor = Actors[I].GetMutable())
			{
				Actor->SetActorTransform(Transforms[I].GetTransform(), false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	});
}
