#pragma once

#include <cstdint>

constexpr uint32_t kMaxLocalPointLights = 8;
constexpr float kLocalPointLightContributionCutoff = 1.0f / 256.0f;
constexpr float kMaxLocalPointLightRadius = 200.0f;
constexpr float kLocalLightGamma = 2.2f;
constexpr uint32_t kLocalPointLightCacheSize = 32;
constexpr double kLocalPointLightExpirySeconds = 2.0;
constexpr float kLocalPointLightTeleportDistance = 100.0f;

struct PointLightUpload
{
    bool linear = true;
    float intensity = 1.0f;
    float fadeSeconds = 0.4f;
};

struct LocalPointLight
{
    float position[3] = {};
    float color[3] = {};
    float attenuation[3] = {};
    float uploadedColor[3] = {};
    float cutoff = 0.0f;
    uint32_t enabled = 1;
    uintptr_t nativeId = 0;
};

enum class LocalLightCapture : uint32_t
{
    Captured,
    UnsupportedClient,
    CameraOutOfRange,
    DamagedTable,
    DisabledLight,
    NonPointLight,
    TableChanged,
    ReadFault,
    InteriorRejected,
};

struct LocalLightInputs
{
    LocalPointLight pointLights[kMaxLocalPointLights] = {};
    uint32_t pointLightCount = 0;
    float interiorBlend = 0.0f;
    bool cameraInterior = false;
    LocalLightCapture capture = LocalLightCapture::Captured;
};

class LocalPointLightHistory
{
public:
    void Update(const LocalLightInputs& captured, const float cameraPosition[3], int mapId, double seconds,
                const PointLightUpload& upload, LocalLightInputs& out);

private:
    struct CachedLight
    {
        LocalPointLight light;
        double lastRelevant = 0.0;
        bool occupied = false;
    };

    struct UploadedLight
    {
        LocalPointLight light;
        double weight = 0.0;
        bool occupied = false;
    };

    void Remember(const LocalPointLight& light, const float cameraPosition[3], double seconds);
    void Select(const LocalLightInputs& captured, const float cameraPosition[3], double seconds,
                const PointLightUpload& upload, LocalLightInputs& selected);
    void Fade(const LocalLightInputs& selected, double elapsed, const PointLightUpload& upload,
              LocalLightInputs& out);

    CachedLight m_cache[kLocalPointLightCacheSize];
    UploadedLight m_uploaded[kMaxLocalPointLights];
    float m_cameraPosition[3] = {};
    int m_mapId = -1;
    double m_seconds = 0.0;
    bool m_initialized = false;
};

namespace engine
{
void UploadedPointLightColor(const float captured[3], const PointLightUpload& upload, float uploaded[3]);
float PointLightCutoff(const float color[3], const float attenuation[3]);
bool SelectLocalPointLight(LocalLightInputs& out, const LocalPointLight& light, const float cameraPosition[3],
                           const PointLightUpload& upload);
const char* LocalLightCaptureName(LocalLightCapture capture);
LocalLightCapture CapturePointLightTable(uintptr_t sceneSlot, const float cameraPosition[3],
                                         const PointLightUpload& upload, LocalLightInputs& out);
bool CaptureLocalLightInputs(const float cameraPosition[3], bool withPointLights, const PointLightUpload& upload,
                             LocalLightInputs& out);
}
