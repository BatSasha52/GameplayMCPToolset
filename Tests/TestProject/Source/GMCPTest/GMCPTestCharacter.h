// Trivial test pawn for GameplayMCPToolset's end-to-end tests. Not part of the plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"

#include "GMCPTestCharacter.generated.h"

class UInputAction;
struct FInputActionValue;

USTRUCT(BlueprintType)
struct FGMCPTestStats
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	int32 Level = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FVector Offset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	TArray<FName> Perks;
};

USTRUCT(BlueprintType)
struct FGMCPTestItem
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	int32 Count = 0;
};

UCLASS()
class AGMCPTestCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AGMCPTestCharacter();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	float Health = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	FGMCPTestStats Stats;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	TArray<FGMCPTestItem> Inventory;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	TMap<FName, int32> Ammo;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
	TObjectPtr<AActor> TargetActor;

	// Written by the input bindings so tests can check what Enhanced Input delivered.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	FVector2D LastMoveInput = FVector2D::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	int32 MoveTriggeredFrames = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	int32 MoveCompletedCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	int32 JumpPressCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	int32 JumpReleaseCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	float LastThrottle = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	float MaxThrottle = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Input")
	bool bInputBound = false;

	UFUNCTION(BlueprintCallable, Category = "Test")
	float AddHealth(float Amount, bool bClamp, int32& OutTimesCalled);

	UFUNCTION(BlueprintCallable, Category = "Test")
	FString DescribeActor(AActor* Other, int32 Repeat = 1) const;

	UFUNCTION(BlueprintCallable, Category = "Test")
	void ResetInputCounters();

protected:
	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void OnMove(const FInputActionValue& Value);
	void OnMoveCompleted(const FInputActionValue& Value);
	void OnJumpStarted(const FInputActionValue& Value);
	void OnJumpCompleted(const FInputActionValue& Value);
	void OnThrottle(const FInputActionValue& Value);

	int32 AddHealthCalls = 0;
};

UCLASS()
class AGMCPTestGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGMCPTestGameMode();
};
