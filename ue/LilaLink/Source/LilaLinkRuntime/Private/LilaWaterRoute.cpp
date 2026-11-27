#include "LilaWaterRoute.h"

#include "GameFramework/Actor.h"

ULilaWaterRoute* ULilaWaterRoute::FindOn(const AActor* Actor)
{
	return Actor ? Actor->FindComponentByClass<ULilaWaterRoute>() : nullptr;
}
