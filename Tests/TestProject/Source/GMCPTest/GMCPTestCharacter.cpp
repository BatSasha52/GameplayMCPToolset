// Trivial test pawn for GameplayMCPToolset's end-to-end tests. Not part of the plugin.

#include "GMCPTestCharacter.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "Engine/SkeletalMesh.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// Created by Tests/gameplay_smoke_test.py before PIE starts.
	const TCHAR* MoveActionPath = TEXT("/Game/GMCPTest/Input/IA_Move.IA_Move");
	const TCHAR* JumpActionPath = TEXT("/Game/GMCPTest/Input/IA_Jump.IA_Jump");
	const TCHAR* ThrottleActionPath = TEXT("/Game/GMCPTest/Input/IA_Throttle.IA_Throttle");
	const TCHAR* FixtureAnimClassPath = TEXT("/Game/GMCPTest/ABP_GMCPTest.ABP_GMCPTest_C");
}

AGMCPTestCharacter::AGMCPTestCharacter()
{
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshFinder(TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP"));
	if (MeshFinder.Succeeded())
	{
		GetMesh()->SetSkeletalMeshAsset(MeshFinder.Object);
		GetMesh()->SetRelativeLocationAndRotation(FVector(0, 0, -90), FRotator(0, -90, 0));
	}
	Tags.Add(TEXT("GMCPPlayer"));
	Inventory = { { TEXT("Sword"), 1 }, { TEXT("Potion"), 3 } };
	Ammo.Add(TEXT("Rifle"), 30);
	Ammo.Add(TEXT("Pistol"), 12);
}

void AGMCPTestCharacter::BeginPlay()
{
	if (UClass* AnimClass = LoadObject<UClass>(nullptr, FixtureAnimClassPath, nullptr, LOAD_NoWarn))
	{
		GetMesh()->SetAnimInstanceClass(AnimClass);
	}
	Super::BeginPlay();
}

void AGMCPTestCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	const UInputAction* Move = LoadObject<UInputAction>(nullptr, MoveActionPath, nullptr, LOAD_NoWarn);
	const UInputAction* Jump = LoadObject<UInputAction>(nullptr, JumpActionPath, nullptr, LOAD_NoWarn);
	const UInputAction* Throttle = LoadObject<UInputAction>(nullptr, ThrottleActionPath, nullptr, LOAD_NoWarn);
	if (!Input || !Move || !Jump || !Throttle)
	{
		UE_LOG(LogTemp, Warning, TEXT("GMCPTestCharacter: Enhanced Input component or test input actions missing."));
		return;
	}
	Input->BindAction(Move, ETriggerEvent::Triggered, this, &AGMCPTestCharacter::OnMove);
	Input->BindAction(Move, ETriggerEvent::Completed, this, &AGMCPTestCharacter::OnMoveCompleted);
	Input->BindAction(Jump, ETriggerEvent::Started, this, &AGMCPTestCharacter::OnJumpStarted);
	Input->BindAction(Jump, ETriggerEvent::Completed, this, &AGMCPTestCharacter::OnJumpCompleted);
	Input->BindAction(Throttle, ETriggerEvent::Triggered, this, &AGMCPTestCharacter::OnThrottle);
	bInputBound = true;
}

void AGMCPTestCharacter::OnMove(const FInputActionValue& Value)
{
	LastMoveInput = Value.Get<FVector2D>();
	++MoveTriggeredFrames;
	AddMovementInput(FVector::ForwardVector, LastMoveInput.Y);
	AddMovementInput(FVector::RightVector, LastMoveInput.X);
}

void AGMCPTestCharacter::OnMoveCompleted(const FInputActionValue& Value)
{
	++MoveCompletedCount;
}

void AGMCPTestCharacter::OnJumpStarted(const FInputActionValue& Value)
{
	++JumpPressCount;
	Jump();
}

void AGMCPTestCharacter::OnJumpCompleted(const FInputActionValue& Value)
{
	++JumpReleaseCount;
	StopJumping();
}

void AGMCPTestCharacter::OnThrottle(const FInputActionValue& Value)
{
	LastThrottle = Value.Get<float>();
	MaxThrottle = FMath::Max(MaxThrottle, LastThrottle);
}

float AGMCPTestCharacter::AddHealth(float Amount, bool bClamp, int32& OutTimesCalled)
{
	Health += Amount;
	if (bClamp)
	{
		Health = FMath::Clamp(Health, 0.f, 100.f);
	}
	OutTimesCalled = ++AddHealthCalls;
	return Health;
}

FString AGMCPTestCharacter::DescribeActor(AActor* Other, int32 Repeat) const
{
	FString Out;
	for (int32 Index = 0; Index < Repeat; ++Index)
	{
		Out += Other ? Other->GetName() : TEXT("None");
	}
	return Out;
}

void AGMCPTestCharacter::ResetInputCounters()
{
	LastMoveInput = FVector2D::ZeroVector;
	MoveTriggeredFrames = 0;
	MoveCompletedCount = 0;
	JumpPressCount = 0;
	JumpReleaseCount = 0;
	LastThrottle = 0.f;
	MaxThrottle = 0.f;
}

AGMCPTestGameMode::AGMCPTestGameMode()
{
	DefaultPawnClass = AGMCPTestCharacter::StaticClass();
}
