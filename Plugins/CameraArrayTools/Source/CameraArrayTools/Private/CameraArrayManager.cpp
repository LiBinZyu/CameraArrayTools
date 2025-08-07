#include "CameraArrayManager.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Modules/ModuleManager.h"
#include "TimerManager.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Selection.h"
#endif

ACameraArrayManager::ACameraArrayManager()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ACameraArrayManager::BeginPlay()
{
	Super::BeginPlay();
	InitializeCaptureComponents();
}

void ACameraArrayManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (ReusableCaptureComponent)
    {
        ReusableCaptureComponent->DestroyComponent();
        ReusableCaptureComponent = nullptr;
    }
    if (ReusableRenderTarget)
    {
        ReusableRenderTarget->MarkAsGarbage();
        ReusableRenderTarget = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void ACameraArrayManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (bIsTaskRunning) return; // 渲染时禁止修改，防止冲突

    const FName PropertyName = (PropertyChangedEvent.Property != nullptr)
        ? PropertyChangedEvent.Property->GetFName()
        : NAME_None;

    if (PropertyName == NAME_None) return;

    // --- 按需更新，将破坏性操作降到最低 ---

    // 1. 只有相机数量改变时，才执行完全重建
    if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, NumCameras))
    {
        CreateOrUpdateCameras();
    }
    // 2. 只更新位置，保留手动调整过的旋转
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, TotalYDistance) ||
             PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, StartLocation))
    {
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FRotator CurrentRotation = Camera->GetActorRotation(); // 保存当前旋转
                const FVector NewLocation = GetCameraTransform(i).GetLocation(); // 计算新位置
                Camera->SetActorLocationAndRotation(NewLocation, CurrentRotation, false, nullptr, ETeleportType::None);
            }
        }
    }
    // 3. 只更新旋转，保留手动调整过的位置
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, SharedRotation) ||
             PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, bUseLookAtTarget) ||
             PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, LookAtTarget))
    {
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FVector CurrentLocation = Camera->GetActorLocation(); // 保存当前位置
                const FRotator NewRotation = GetCameraTransform(i).GetRotation().Rotator(); // 计算新旋转
                Camera->SetActorLocationAndRotation(CurrentLocation, NewRotation, false, nullptr, ETeleportType::None);
            }
        }
    }
    // 4. 只更新相机命名和文件夹路径
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, CameraNamePrefix))
    {
        const FName FolderName = FName(TEXT("CameraArray"));
        for (int32 i = 0; i < ManagedCameras.Num(); ++i)
        {
            if (AActor* Camera = ManagedCameras[i])
            {
                const FString NewLabel = FString::Printf(TEXT("%s_%03d"), *CameraNamePrefix, i);
                Camera->SetActorLabel(NewLabel); // 更新Actor在编辑器中的名字
                Camera->SetFolderPath(FolderName); // 更新文件夹
            }
        }
    }
    // 5. 只更新FOV
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, CameraFOV))
    {
        for (AActor* Camera : ManagedCameras)
        {
            if (Camera)
            {
                if (UCineCameraComponent* CineCamComponent = Camera->FindComponentByClass<UCineCameraComponent>())
                {
                    CineCamComponent->SetFieldOfView(CameraFOV);
                }
            }
        }
    }
    // 6. 只更新Filmback的宽高比
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, RenderTargetX) ||
             PropertyName == GET_MEMBER_NAME_CHECKED(ACameraArrayManager, RenderTargetY))
    {
        if (RenderTargetY > 0 && RenderTargetX > 0)
        {
            const float DesiredAspectRatio = static_cast<float>(RenderTargetX) / static_cast<float>(RenderTargetY);
            for (AActor* Camera : ManagedCameras)
            {
                if (UCineCameraComponent* CineCamComponent = Camera->FindComponentByClass<UCineCameraComponent>())
                {
                    // 保持宽度，根据宽高比调整高度
                    CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth / DesiredAspectRatio;
                }
            }
        }
    }
}
#endif

void ACameraArrayManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void ACameraArrayManager::InitializeCaptureComponents()
{
    if (!IsValid(ReusableCaptureComponent))
    {
        ReusableCaptureComponent = NewObject<USceneCaptureComponent2D>(this, TEXT("ReusableCaptureComponent"));
        ReusableCaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        ReusableCaptureComponent->bCaptureEveryFrame = false;
        ReusableCaptureComponent->bCaptureOnMovement = false;
        
        ReusableCaptureComponent->ShowFlags.SetAtmosphere(true);
        ReusableCaptureComponent->ShowFlags.SetBSP(true);
        ReusableCaptureComponent->ShowFlags.SetSkeletalMeshes(true);
        ReusableCaptureComponent->ShowFlags.SetStaticMeshes(true);
        ReusableCaptureComponent->ShowFlags.SetLighting(true);
        ReusableCaptureComponent->ShowFlags.SetSkyLighting(true);
        ReusableCaptureComponent->ShowFlags.SetParticles(true);
        ReusableCaptureComponent->ShowFlags.SetTranslucency(true);
        ReusableCaptureComponent->ShowFlags.SetAntiAliasing(true);
        
        ReusableCaptureComponent->RegisterComponentWithWorld(GetWorld());
    }

    if (!IsValid(ReusableRenderTarget) || ReusableRenderTarget->SizeX != RenderTargetX || ReusableRenderTarget->SizeY != RenderTargetY)
    {
        ReusableRenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("ReusableRenderTarget"));
        ReusableRenderTarget->RenderTargetFormat = RTF_RGBA8_SRGB;
        ReusableRenderTarget->SizeX = RenderTargetX;
        ReusableRenderTarget->SizeY = RenderTargetY;
        ReusableRenderTarget->InitAutoFormat(RenderTargetX, RenderTargetY);
        ReusableRenderTarget->UpdateResource();
    }

    ReusableCaptureComponent->TextureTarget = ReusableRenderTarget;
}

void ACameraArrayManager::CreateOrUpdateCameras()
{
    ClearAllCameras();

    UWorld* const World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("CreateOrUpdateCameras: 获取UWorld失败。"));
        return;
    }
    
    if (NumCameras <= 0)
    {
        UE_LOG(LogTemp, Log, TEXT("CreateOrUpdateCameras: NumCameras为0，不创建相机。"));
        return;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;

    for (int32 i = 0; i < NumCameras; ++i)
    {
        const FTransform CameraTransform = GetCameraTransform(i);
        ACineCameraActor* NewCamera = World->SpawnActor<ACineCameraActor>(ACineCameraActor::StaticClass(), CameraTransform, SpawnParams);
        
        if (NewCamera)
        {
            UCineCameraComponent* CineCamComponent = NewCamera->GetCineCameraComponent();
            if (CineCamComponent)
            {
                CineCamComponent->SetFieldOfView(CameraFOV);

                if (RenderTargetY > 0 && RenderTargetX > 0)
                {
                    const float DesiredAspectRatio = static_cast<float>(RenderTargetX) / static_cast<float>(RenderTargetY);
                    CineCamComponent->Filmback.SensorHeight = CineCamComponent->Filmback.SensorWidth / DesiredAspectRatio;
                }
            }

            FString CameraLabel = FString::Printf(TEXT("%s_%03d"), *CameraNamePrefix, i);
#if WITH_EDITOR
            NewCamera->SetActorLabel(CameraLabel);
#endif
            ManagedCameras.Add(NewCamera);
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("CreateOrUpdateCameras: 生成相机 %d 失败。"), i);
        }
    }

    OrganizeCamerasInFolder();
    UE_LOG(LogTemp, Log, TEXT("CreateOrUpdateCameras: 成功创建或更新了 %d 个相机。"), NumCameras);
}

void ACameraArrayManager::ClearAllCameras()
{
    if (bIsTaskRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClearAllCameras: 无法在渲染任务进行中清除相机。"));
        return;
    }
    
    UWorld* const World = GetWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("ClearAllCameras: 获取UWorld失败。"));
        return;
    }

    int32 DestroyedCount = 0;
    for (AActor* Camera : ManagedCameras)
    {
        if (IsValid(Camera)) 
        {
#if WITH_EDITOR
            Camera->SetFolderPath(NAME_None);
#endif
            World->DestroyActor(Camera);
            DestroyedCount++;
        }
    }
    
    ManagedCameras.Empty();
    UE_LOG(LogTemp, Log, TEXT("ClearAllCameras: 成功销毁了 %d 个相机。"), DestroyedCount);
}

void ACameraArrayManager::RenderAllViews()
{
    if (bIsTaskRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderAllViews: 另一个渲染任务已在进行中。"));
        return;
    }
    if (ManagedCameras.Num() <= 0) // 检查ManagedCameras数组而不是NumCameras属性
    {
        UE_LOG(LogTemp, Warning, TEXT("RenderAllViews: 场景中没有可渲染的相机。"));
        return;
    }

    bIsTaskRunning = true;
    InitializeCaptureComponents(); 
    
    GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
    CurrentRenderIndex = 0;
    RenderProgress = 0;
    
    PerformSingleCapture();
    OpenOutputFolder();
}

void ACameraArrayManager::PerformSingleCapture()
{
    if (CurrentRenderIndex >= ManagedCameras.Num()) // 检查是否超出数组范围
    {
        UE_LOG(LogTemp, Log, TEXT("渲染流程完成!"));
        GetWorld()->GetTimerManager().ClearTimer(RenderTimerHandle);
        RenderProgress = 100;
        RenderStatus = TEXT("渲染完成");
        bIsTaskRunning = false;
        return;
    }
    
    if (!GetWorld() || !IsValid(ReusableCaptureComponent) || !IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCapture: World或渲染组件无效!"));
        RenderStatus = TEXT("渲染失败: 内部组件错误");
        bIsTaskRunning = false;
        return;
    }
    
    // <--- 修改开始: 核心修改，从场景相机读取实时参数用于渲染
    AActor* CameraActor = ManagedCameras[CurrentRenderIndex];
    if (!IsValid(CameraActor))
    {
        UE_LOG(LogTemp, Warning, TEXT("跳过无效的相机索引 %d"), CurrentRenderIndex);
        CurrentRenderIndex++;
        PerformSingleCapture(); // 立即尝试渲染下一个
        return;
    }

    const UCineCameraComponent* CineCamComponent = CameraActor->FindComponentByClass<UCineCameraComponent>();
    if (!CineCamComponent)
    {
        UE_LOG(LogTemp, Warning, TEXT("相机 %s 没有CineCameraComponent，跳过。"), *CameraActor->GetName());
        CurrentRenderIndex++;
        PerformSingleCapture(); // 立即尝试渲染下一个
        return;
    }

    // 1. 获取相机在场景中的实时Transform
    const FTransform CameraTransform = CameraActor->GetActorTransform();
    // 2. 获取相机在场景中的实时FOV
    const float CameraRealFOV = CineCamComponent->FieldOfView;
    
    RenderProgress = FMath::RoundHalfFromZero((float)CurrentRenderIndex / ManagedCameras.Num() * 100);
    RenderStatus = FString::Printf(TEXT("渲染中... (%d/%d)"), CurrentRenderIndex + 1, ManagedCameras.Num());
    UE_LOG(LogTemp, Log, TEXT("开始渲染相机 %s"), *CameraActor->GetName());

    // 3. 将实时参数应用到渲染组件
    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraRealFOV;
    // <--- 修改结束
    
    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, CurrentRenderIndex, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName);
    
    CurrentRenderIndex++;
    
    FTimerDelegate TimerDel;
    TimerDel.BindUObject(this, &ACameraArrayManager::PerformSingleCapture);
    GetWorld()->GetTimerManager().SetTimer(RenderTimerHandle, TimerDel, 0.02f, false);
}

void ACameraArrayManager::SaveRenderTargetToFileAsync(const FString& FullOutputPath, const FString& FileName)
{
    if (!IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: 可复用的RenderTarget无效。"));
        return;
    }
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*FullOutputPath))
    {
        PlatformFile.CreateDirectoryTree(*FullOutputPath);
    }

    const FString FilePath = FullOutputPath / FileName;
    const int32 Width = ReusableRenderTarget->SizeX;
    const int32 Height = ReusableRenderTarget->SizeY;
    const ECameraArrayImageFormat ImageFormatToSave = FileFormat;

    TArray <FColor> RawPixels;
    FTextureRenderTargetResource* RenderTargetResource = 
    ReusableRenderTarget->GameThread_GetRenderTargetResource();

    if (!RenderTargetResource || !RenderTargetResource->ReadPixels(RawPixels))
    {
        UE_LOG(LogTemp, Error, TEXT("SaveRenderTargetToFileAsync: 从RenderTarget读取像素失败。"));
        return;
    }
    
    RenderTargetResource->ReadPixels(RawPixels);
    
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Width, Height, FilePath, ImageFormatToSave, RawPixels{MoveTemp(RawPixels)}]()
    {
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        
        ::EImageFormat EngineImageFormat;
        switch (ImageFormatToSave)
        {
            case ECameraArrayImageFormat::JPEG: EngineImageFormat = ::EImageFormat::JPEG; break;
            case ECameraArrayImageFormat::BMP:  EngineImageFormat = ::EImageFormat::BMP; break;
            case ECameraArrayImageFormat::PNG:
            default:                            EngineImageFormat = ::EImageFormat::PNG; break;
        }

        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EngineImageFormat);
        if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(RawPixels.GetData(), RawPixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
        {
            UE_LOG(LogTemp, Error, TEXT("为 %s 编码图像数据失败。"), *FilePath);
            return;
        }

        const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();
        if (FFileHelper::SaveArrayToFile(CompressedData, *FilePath))
        {
            UE_LOG(LogTemp, Log, TEXT("成功异步保存图像到: %s"), *FilePath);
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("保存图像文件失败: %s"), *FilePath);
        }
    });
}

FString ACameraArrayManager::GetFileExtension() const
{
    switch (FileFormat)
    {
    case ECameraArrayImageFormat::PNG: return TEXT("png");
    case ECameraArrayImageFormat::JPEG: return TEXT("jpg");
    case ECameraArrayImageFormat::BMP: return TEXT("bmp");
    default: return TEXT("png");
    }
}

FTransform ACameraArrayManager::GetCameraTransform(int32 CameraIndex) const
{
    FVector Location = StartLocation;
    if (NumCameras > 1)
    {
        // 如果只有一个相机，间距为0，避免除以0
        const float Spacing = (NumCameras > 1) ? (TotalYDistance * 100.0f / (NumCameras - 1)) : 0.0f;
        Location.Y += CameraIndex * Spacing;
    }

    FRotator Rotation = SharedRotation;
    if (bUseLookAtTarget && LookAtTarget != nullptr)
    {
        FVector TargetLocation = LookAtTarget->GetActorLocation();
        FVector Direction = (TargetLocation - Location).GetSafeNormal();
        Rotation = Direction.ToOrientationRotator();
    }
    
    return FTransform(Rotation, Location);
}

void ACameraArrayManager::SelectFirstCamera()
{
    if (ManagedCameras.Num() > 0 && IsValid(ManagedCameras[0]))
    {
#if WITH_EDITOR
        if (GEditor)
        {
            GEditor->SelectNone(true, true);
            GEditor->SelectActor(ManagedCameras[0], true, true);
        }
#endif
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SelectFirstCamera: 没有可用的相机。"));
    }
}

void ACameraArrayManager::SelectLastCamera()
{
    if (ManagedCameras.Num() > 0)
    {
        const int32 LastIndex = ManagedCameras.Num() - 1;
        if (IsValid(ManagedCameras[LastIndex]))
        {
#if WITH_EDITOR
            if (GEditor)
            {
                GEditor->SelectNone(true, true);
                GEditor->SelectActor(ManagedCameras[LastIndex], true, true);
            }
#endif
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("SelectLastCamera: 最后一个相机无效。"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("SelectLastCamera: 没有可用的相机。"));
    }
}

void ACameraArrayManager::OrganizeCamerasInFolder()
{
#if WITH_EDITOR
    for (AActor* Camera : ManagedCameras)
    {
        if (IsValid(Camera))
        {
            Camera->SetFolderPath(FName(TEXT("CameraArray")));
        }
    }
#endif
}

void ACameraArrayManager::RenderFirstCamera()
{
    if (bIsTaskRunning) return;
    if (ManagedCameras.Num() <= 0) return;
    
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    PerformSingleCaptureForSpecificIndex(0);
    OpenOutputFolder();
}

void ACameraArrayManager::RenderLastCamera()
{
    if (bIsTaskRunning) return;
    if (ManagedCameras.Num() <= 0) return;
    
    bIsTaskRunning = true;
    InitializeCaptureComponents();
    const int32 LastCameraIndex = ManagedCameras.Num() - 1;
    PerformSingleCaptureForSpecificIndex(LastCameraIndex);
    OpenOutputFolder();
}

void ACameraArrayManager::OpenOutputFolder()
{
    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*FullOutputPath))
    {
        PlatformFile.CreateDirectoryTree(*FullOutputPath);
    }
    
    FPlatformProcess::ExploreFolder(*FullOutputPath);
    UE_LOG(LogTemp, Log, TEXT("已打开输出文件夹: %s"), *FullOutputPath);
}

void ACameraArrayManager::PerformSingleCaptureForSpecificIndex(int32 IndexToCapture)
{
    if (!GetWorld() || !IsValid(ReusableCaptureComponent) || !IsValid(ReusableRenderTarget))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCaptureForSpecificIndex: World或渲染组件无效!"));
        RenderStatus = TEXT("渲染失败: 内部组件错误");
        bIsTaskRunning = false;
        return;
    }

    if (!ManagedCameras.IsValidIndex(IndexToCapture) || !IsValid(ManagedCameras[IndexToCapture]))
    {
        UE_LOG(LogTemp, Error, TEXT("PerformSingleCaptureForSpecificIndex: 要渲染的相机索引 %d 无效!"), IndexToCapture);
        bIsTaskRunning = false;
        return;
    }
    
    // <--- 修改开始: 同样，从场景相机读取实时参数
    AActor* CameraActor = ManagedCameras[IndexToCapture];
    const UCineCameraComponent* CineCamComponent = CameraActor->FindComponentByClass<UCineCameraComponent>();

    if (!CineCamComponent)
    {
        UE_LOG(LogTemp, Error, TEXT("相机 %s 没有CineCameraComponent，无法渲染。"), *CameraActor->GetName());
        bIsTaskRunning = false;
        return;
    }

    const FTransform CameraTransform = CameraActor->GetActorTransform();
    const float CameraRealFOV = CineCamComponent->FieldOfView;
    
    RenderProgress = 50;
    RenderStatus = FString::Printf(TEXT("渲染中... (相机 %s)"), *CameraActor->GetName());

    ReusableCaptureComponent->SetWorldTransform(CameraTransform);
    ReusableCaptureComponent->FOVAngle = CameraRealFOV;
    // <--- 修改结束

    ReusableCaptureComponent->CaptureScene();

    const FString FullOutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputPath);
    const FString FileName = FString::Printf(TEXT("%s_%03d.%s"), *CameraNamePrefix, IndexToCapture, *GetFileExtension());
    
    SaveRenderTargetToFileAsync(FullOutputPath, FileName);

    // 异步任务在后台保存，我们可以认为渲染指令已完成
    FTimerHandle TempHandle;
    GetWorld()->GetTimerManager().SetTimer(TempHandle, [this]()
    {
        RenderProgress = 100;
        RenderStatus = TEXT("渲染完成 (文件后台保存中)");
        bIsTaskRunning = false;
    }, 0.5f, false);
}