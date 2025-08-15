#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/Vector.h"
#include "Math/Rotator.h"
#include "Engine/EngineTypes.h"

#if WITH_EDITOR
#include "Editor/UnrealEdTypes.h"
#endif

#include "CameraArrayManager.generated.h"

class USceneCaptureComponent2D; // Forward declaration
class UTextureRenderTarget2D;
class APostProcessVolume;

UENUM(BlueprintType)
enum class ECameraArrayImageFormat : uint8
{
	PNG UMETA(DisplayName = "PNG (8-bit)"),
	JPEG UMETA(DisplayName = "JPEG (8-bit)"),
	BMP UMETA(DisplayName = "BMP (8-bit)"),
	TGA UMETA(DisplayName = "TGA (8-bit)"),

	// High Bit-Depth Formats
	EXR UMETA(DisplayName = "EXR (16-bit Float)"),
	//TIFF UMETA(DisplayName = "TIFF (16-bit)"),
	//HDR UMETA(DisplayName = "HDR (Radiance)")
};


UCLASS()
class CAMERAARRAYTOOLS_API ACameraArrayManager : public AActor
{
	GENERATED_BODY()

public:
	ACameraArrayManager();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override; // Cleanup

public:
	virtual void Tick(float DeltaTime) override;
	
#if WITH_EDITOR
	// 添加保存视口原始状态的结构体
	struct FViewportState
	{
		FVector Location;
		FRotator Rotation;
		float FOV;
		bool bIsRealtime;
		bool bIsInGameView;
		ELevelViewportType ViewportType;
		bool bIsValid;

		FViewportState() : Location(FVector::ZeroVector), Rotation(FRotator::ZeroRotator), FOV(90.0f), 
			bIsRealtime(false), bIsInGameView(false), ViewportType(LVT_Perspective), bIsValid(false) {}
	};
#endif
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings",
		meta = (DisplayName = "相机数量", UIMin = "1", UIMax = "99", Delta = "1", EditCondition = "!bIsRenderingLocked"))
	int32 NumCameras = 80;

	// 总Y方向距离
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "总Y方向距离 (米)", EditCondition = "!bIsRenderingLocked"))
	float TotalYDistance = 3.5f;

	// 起始位置 X, Y, Z
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings",
		meta = (DisplayName = "起始位置 (X, Y, Z)", EditCondition = "!bIsRenderingLocked"))
	FVector StartLocation = FVector(-55.0f, 0.0f, 16.0f);

	// 如果LookAtTarget 不启用则统一旋转 pitch roll yaw
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings",
		meta = (DisplayName = "统一旋转 (Roll, Pitch, Yaw)", EditCondition = "!bIsRenderingLocked"))
	FRotator SharedRotation = FRotator(0.0f, 0.0f, 0.0f);

	// 相机FOV
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "相机FOV", EditCondition = "!bIsRenderingLocked"))
	float CameraFOV = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Others", 
		meta = (DisplayName = "后处理引用", EditCondition = "!bIsRenderingLocked"))
	TObjectPtr<APostProcessVolume> PostProcessVolumeRef;

	// 是否启用LookAtTarget功能
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Others",
		meta = (DisplayName = "启用LookAtTarget", EditCondition = "!bIsRenderingLocked"))
	bool bUseLookAtTarget = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Others", 
		meta = (DisplayName = "场景目标点", EditCondition = "!bIsRenderingLocked"))
	TObjectPtr<AActor> LookAtTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "输出宽度", EditCondition = "!bIsRenderingLocked"))
	int32 RenderTargetX = 1920;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "输出高度", EditCondition = "!bIsRenderingLocked"))
	int32 RenderTargetY = 1080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "格式", EditCondition = "!bIsRenderingLocked"))
	ECameraArrayImageFormat FileFormat = ECameraArrayImageFormat::PNG;

	// 输出路径
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings",
		meta = (DisplayName = "输出路径", Subtype = "DirPath", EditCondition = "!bIsRenderingLocked"))
	FString OutputPath = TEXT("RenderOutput"); // 默认在工程的Saved文件夹下面

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "覆盖已有", EditCondition = "!bIsRenderingLocked"))
	bool bOverwriteExisting = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings", 
		meta = (DisplayName = "相机前缀", EditCondition = "!bIsRenderingLocked"))
	FString CameraNamePrefix = TEXT("Camera");

	/*UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Array Settings",
		meta = (DisplayName = "路径追踪渲染时间 (秒)", EditCondition = "!bIsRenderingLocked"))
	float PathTracingRenderTime = 3.0f;*/

	UPROPERTY(VisibleAnywhere, Category = "[READONLY]",
		meta = (DisplayName = "渲染进度", UIMin = "0", UIMax = "100", Delta = "1"))
	int32 RenderProgress = 0;

	UPROPERTY(VisibleAnywhere, Category = "[READONLY]", meta = (DisplayName = "渲染状态"))
	FString RenderStatus = TEXT("未开始");
	
	// 添加只读属性来控制编辑器中的可编辑性
	UPROPERTY(VisibleAnywhere, Category = "[READONLY]", meta = (DisplayName = "正在渲染"))
	bool bIsRenderingLocked = false;

	// 创建或更新相机
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", 
		meta = (DisplayName = "创建或更新相机", CallInEditorCondition = "!bIsRenderingLocked"))
	void CreateOrUpdateCameras();

	// 清除所有相机
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", 
		meta = (DisplayName = "清除相机", CallInEditorCondition = "!bIsRenderingLocked"))
	void ClearAllCameras();

	//选择第一个相机
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", 
		meta = (DisplayName = "选择第一个相机", CallInEditorCondition = "!bIsRenderingLocked"))
	void SelectFirstCamera();

	// 选择最后一个相机
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", 
		meta = (DisplayName = "选择最后一个相机", CallInEditorCondition = "!bIsRenderingLocked"))
	void SelectLastCamera();
	
	// 打开输出文件夹
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "批处理", 
		meta = (DisplayName = "打开输出文件夹", CallInEditorCondition = "!bIsRenderingLocked"))
	void OpenOutputFolder();

	// 渲染所有相机视角
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "批处理", 
		meta = (DisplayName = "为所有相机拍摄高清截图", CallInEditorCondition = "!bIsRenderingLocked"))
	void TakeHighResScreenshots();

	 // 为第一个相机渲染
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", meta = (DisplayName = "为第一个相机拍摄高清截图", CallInEditorCondition = "!bIsRenderingLocked"))
	void TakeFirstCameraScreenshot();

	// 为最后一个相机渲染
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", meta = (DisplayName = "为最后一个相机拍摄高清截图", CallInEditorCondition = "!bIsRenderingLocked"))
	void TakeLastCameraScreenshot();

	/*UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", meta = (DisplayName = "渲染第一个相机"))
	void RenderFirstCamera();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "执行函数", meta = (DisplayName = "渲染最后一个相机"))
	void RenderLastCamera();

	UFUNCTION(BlueprintCallable,CallInEditor, Category = "批处理", meta = (DisplayName = "渲染到图片"))
	void RenderAllViews();*/
	
private:
	UPROPERTY()
	TArray<TObjectPtr<AActor>> ManagedCameras;

	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> ReusableCaptureComponent;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ReusableLdrRenderTarget; // LDR

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> ReusableHdrRenderTarget; // HDR

	void InitializeCaptureComponents();

	bool bIsTaskRunning = false;
	FTransform GetCameraTransform(int32 CameraIndex) const;
	int32 CurrentRenderIndex;
	FTimerHandle RenderTimerHandle;
	bool IsHdrFormat() const;

	/*void PerformSingleCapture();
	void PerformSingleCaptureForSpecificIndex(int32 IndexToCapture);
	
	//void SaveRenderTargetToFileAsync(const FString& FullOutputPath, const FString& FileName);
	void SaveRenderTargetToFileAsync(const FString& FullOutputPath, const FString& FileName, UTextureRenderTarget2D* RenderTargetToSave);*/

	FString GetFileExtension() const;
	void OrganizeCamerasInFolder();

	int32 CurrentScreenshotIndex;
	FTimerHandle ScreenshotTimerHandle;
	FTimerHandle PathTracingLogTimerHandle;

#if WITH_EDITOR
	// 添加保存视口原始状态的变量
	FViewportState OriginalViewportState;
	
	void SyncShowFlagsWithEditorViewport();
	void SyncPostProcessSettings();
	void TakeNextHighResScreenshot();
	float GetPathTracingProgress(int32& CurrentSPP, int32& TotalSPP);
	void LogPathTracingProgress();
	void TakeSingleHighResScreenshot(int32 CameraIndex);
	void ExecuteScreenshotForCamera(int32 CameraIndex, TFunction<void()> OnComplete);
	void TakeNextHighResScreenshot_Recursive();
	
	// 添加清理定时器的函数
	void ClearAllTimers();
	
	// 保存和恢复视口状态的函数
	void SaveOriginalViewportState();
	void RestoreOriginalViewportState();
	
	// 禁用和启用编辑器属性编辑的函数
	void LockEditorProperties();
	void UnlockEditorProperties();
#endif
};