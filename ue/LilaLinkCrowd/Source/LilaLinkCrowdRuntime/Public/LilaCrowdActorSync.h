#pragma once

#include "MassEntityTypes.h"
#include "MassProcessor.h"

#include "LilaCrowdActorSync.generated.h"

/** Marks a Lila crowd agent whose representation actor follows it every frame. */
USTRUCT()
struct LILALINKCROWDRUNTIME_API FLilaCrowdAgentTag : public FMassTag
{
	GENERATED_BODY()
};

/**
 * Moves each Lila crowd agent's representation actor (its character, walking) onto the agent every frame; Mass places
 * such actors only when it spawns or swaps them.
 */
UCLASS()
class LILALINKCROWDRUNTIME_API ULilaCrowdActorSync : public UMassProcessor
{
	GENERATED_BODY()

public:
	ULilaCrowdActorSync();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};
