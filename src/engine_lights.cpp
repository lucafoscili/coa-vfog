#include "engine_lights.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
constexpr uintptr_t kClientImageBase = 0x00400000;
constexpr uint32_t kClientTimestamp = 0x4C2452FE;
constexpr uintptr_t kWorldM2Scene = 0x00CD754C;
constexpr uintptr_t kScenePointLightBuckets = 0x24;
constexpr uint32_t kPointLightBucketCount = 4096;
constexpr uint32_t kMaxPointLightBucketNodes = 512;
constexpr uint32_t kMaxPointLightNodes = 8192;
constexpr uint32_t kNativePointLightType = 1;
constexpr float kMaxWorldCoordinate = 1000000.0f;
constexpr float kMaxLightComponent = 10000.0f;
constexpr float kMaxUploadedLightComponent = 1000000.0f;
constexpr float kMaxAttenuationCoefficient = 1000000.0f;
constexpr uintptr_t kCameraWmoInstance = 0x00CD87A4;
constexpr uintptr_t kCameraWmoGroupCount = 0x00CDB0D8;
constexpr uintptr_t kCameraWmoGroupIds = 0x00CDB0DC;
constexpr uintptr_t kCameraInteriorFogBlend = 0x00D38B9C;
constexpr uintptr_t kWmoInstanceRoot = 0xF4;
constexpr uintptr_t kWmoRootLoaded = 0x1E0;
constexpr uintptr_t kWmoRootGroupCount = 0x1F4;
constexpr uintptr_t kWmoRootGroups = 0x1F8;
constexpr uintptr_t kWmoGroupLoaded = 0x198;
constexpr uintptr_t kWmoGroupFlags = 0x30;
constexpr uint32_t kWmoExteriorFogFlags = 0x48;
constexpr uint32_t kMaxCameraWmoGroups = 64;
constexpr uint32_t kMaxWmoGroups = 4096;

struct NativePointLight
{
    uintptr_t scene;
    uint32_t frameStamp;
    uint32_t type;
    float position[3];
    float viewPosition[3];
    float direction[3];
    float ambient[3];
    float color[3];
    float specular[3];
    float attenuation[3];
    uint32_t enabled;
    uintptr_t previousLink;
    uintptr_t next;
};

static_assert(sizeof(NativePointLight) == 0x6C);
static_assert(offsetof(NativePointLight, position) == 0xC);
static_assert(offsetof(NativePointLight, color) == 0x3C);
static_assert(offsetof(NativePointLight, attenuation) == 0x54);
static_assert(offsetof(NativePointLight, enabled) == 0x60);
static_assert(offsetof(NativePointLight, next) == 0x68);

template <typename T>
T Read(uintptr_t address)
{
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool ValidPointer(uintptr_t address, size_t size)
{
    return address >= 0x10000 && !(address & 3) && address < 0xFFF00000 && size <= 0xFFF00000 - address;
}

bool ValidVector(const float values[3], float minimum, float maximum)
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(values[i]) || values[i] < minimum || values[i] > maximum)
            return false;
    return true;
}

double SquaredDistance(const float a[3], const float b[3])
{
    double result = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double delta = static_cast<double>(a[i]) - b[i];
        result += delta * delta;
    }
    return result;
}

float PeakColor(const float color[3])
{
    return std::max(color[0], std::max(color[1], color[2]));
}

float LinearChannel(float channel, float peak)
{
    const float gammaChannel = std::max(channel, 0.0f);
    if (peak <= 1.0f)
        return std::pow(gammaChannel, kLocalLightGamma);
    return std::pow(gammaChannel / peak, kLocalLightGamma) * peak;
}

double LightPriority(const LocalPointLight& light, const float cameraPosition[3])
{
    const double distanceSquared = SquaredDistance(light.position, cameraPosition);
    const double denominator = light.attenuation[0] + light.attenuation[1] * std::sqrt(distanceSquared) +
                               light.attenuation[2] * distanceSquared;
    return PeakColor(light.uploadedColor) / std::max(denominator, 1.0);
}

bool Precedes(const LocalPointLight& a, const LocalPointLight& b, const float cameraPosition[3])
{
    const double aPriority = LightPriority(a, cameraPosition);
    const double bPriority = LightPriority(b, cameraPosition);
    if (aPriority != bPriority)
        return aPriority > bPriority;
    for (int i = 0; i < 3; ++i)
        if (a.position[i] != b.position[i])
            return a.position[i] < b.position[i];
    for (int i = 0; i < 3; ++i)
        if (a.color[i] != b.color[i])
            return a.color[i] > b.color[i];
    for (int i = 0; i < 3; ++i)
        if (a.attenuation[i] != b.attenuation[i])
            return a.attenuation[i] < b.attenuation[i];
    return false;
}

bool SameLight(const LocalPointLight& a, const LocalPointLight& b)
{
    constexpr double kPositionToleranceSquared = 0.01;
    constexpr float kColorTolerance = 0.001f;
    if (SquaredDistance(a.position, b.position) > kPositionToleranceSquared)
        return false;
    for (int channel = 0; channel < 3; ++channel)
        if (std::fabs(a.color[channel] - b.color[channel]) > kColorTolerance ||
            a.attenuation[channel] != b.attenuation[channel])
            return false;
    return true;
}

int FindLight(const LocalLightInputs& lights, const LocalPointLight& light)
{
    if (light.nativeId)
        for (uint32_t i = 0; i < lights.pointLightCount; ++i)
            if (lights.pointLights[i].nativeId == light.nativeId)
                return static_cast<int>(i);
    for (uint32_t i = 0; i < lights.pointLightCount; ++i)
        if (SameLight(lights.pointLights[i], light))
            return static_cast<int>(i);
    return -1;
}

bool WithinLightReach(const LocalPointLight& light, const float cameraPosition[3])
{
    return light.cutoff > 0.0f &&
           SquaredDistance(light.position, cameraPosition) <= static_cast<double>(light.cutoff) * light.cutoff;
}

template <size_t Size>
bool Matches(uintptr_t address, const unsigned char (&expected)[Size])
{
    return std::memcmp(reinterpret_cast<const void*>(address), expected, Size) == 0;
}

bool SupportedLayoutUnsafe()
{
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleA(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != kClientImageBase)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kClientTimestamp ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;
    const unsigned char sceneLoad[] = {0x8B, 0x3D, 0x4C, 0x75, 0xCD, 0x00};
    const unsigned char pointTypeAndBuckets[] = {
        0x83, 0x7E, 0x08, 0x01, 0x0F, 0x85, 0xC6, 0x00, 0x00, 0x00, 0x83, 0x78, 0x24, 0x00};
    const unsigned char pointLinks[] = {0x8D, 0x0C, 0xB8, 0x89, 0x4E, 0x64, 0x8B, 0x09, 0x8B, 0x56, 0x64};
    const unsigned char attenuationUpload[] = {
        0xD9, 0x40, 0x54, 0xD9, 0x5D, 0xD0, 0xD9, 0x40, 0x58, 0xD9, 0x5D, 0xD4, 0xD9, 0x40, 0x5C};
    const unsigned char interiorBlendStore[] = {0xD9, 0x15, 0x9C, 0x8B, 0xD3, 0x00};
    const unsigned char cameraGroups[] = {0x8B, 0x0D, 0xDC, 0xB0, 0xCD, 0x00, 0x8B, 0x14, 0xB1};
    const unsigned char interiorFlags[] = {0xF6, 0x40, 0x30, 0x48, 0x75, 0x23};
    const unsigned char loadedGroups[] = {
        0x83, 0xB9, 0xE0, 0x01, 0x00, 0x00, 0x00, 0x74, 0x19, 0x8B, 0x45, 0x08,
        0x8B, 0x84, 0x81, 0xF8, 0x01, 0x00, 0x00};
    return Matches(0x004F90EC, sceneLoad) && Matches(0x00834C8D, pointTypeAndBuckets) &&
           Matches(0x00834D3C, pointLinks) && Matches(0x00835539, attenuationUpload) &&
           Matches(0x007F1931, interiorBlendStore) && Matches(0x007A11B0, cameraGroups) &&
           Matches(0x007A11C7, interiorFlags) && Matches(0x007AEA83, loadedGroups);
}

bool SupportedLayout()
{
    __try
    {
        return SupportedLayoutUnsafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

LocalLightCapture NativeLightRejection(const NativePointLight& native, uintptr_t scene, uintptr_t previousLink)
{
    if (native.scene != scene || native.previousLink != previousLink)
        return LocalLightCapture::DamagedTable;
    if (native.type != kNativePointLightType)
        return LocalLightCapture::NonPointLight;
    if (!native.enabled)
        return LocalLightCapture::DisabledLight;
    return LocalLightCapture::Captured;
}

LocalLightCapture CapturePointLightsUnsafe(uintptr_t sceneSlot, const float cameraPosition[3],
                                           const PointLightUpload& upload, LocalLightInputs& out)
{
    const uintptr_t scene = Read<uintptr_t>(sceneSlot);
    if (!scene)
        return LocalLightCapture::Captured;
    if (!ValidPointer(scene, kScenePointLightBuckets + sizeof(uintptr_t)))
        return LocalLightCapture::DamagedTable;
    const uintptr_t buckets = Read<uintptr_t>(scene + kScenePointLightBuckets);
    if (!buckets)
        return LocalLightCapture::Captured;
    if (!ValidPointer(buckets, kPointLightBucketCount * sizeof(uintptr_t)))
        return LocalLightCapture::DamagedTable;
    uint32_t nodeCount = 0;
    for (uint32_t bucket = 0; bucket < kPointLightBucketCount; ++bucket)
    {
        uintptr_t previousLink = buckets + bucket * sizeof(uintptr_t);
        uintptr_t address = Read<uintptr_t>(previousLink);
        uint32_t bucketNodes = 0;
        while (address)
        {
            if (++bucketNodes > kMaxPointLightBucketNodes || ++nodeCount > kMaxPointLightNodes ||
                !ValidPointer(address, sizeof(NativePointLight)))
                return LocalLightCapture::DamagedTable;
            const NativePointLight native = Read<NativePointLight>(address);
            const LocalLightCapture rejection = NativeLightRejection(native, scene, previousLink);
            if (rejection != LocalLightCapture::Captured)
                return rejection;
            LocalPointLight light;
            std::memcpy(light.position, native.position, sizeof(light.position));
            std::memcpy(light.color, native.color, sizeof(light.color));
            std::memcpy(light.attenuation, native.attenuation, sizeof(light.attenuation));
            light.enabled = native.enabled;
            light.nativeId = address;
            engine::SelectLocalPointLight(out, light, cameraPosition, upload);
            previousLink = address + offsetof(NativePointLight, next);
            address = native.next;
        }
    }
    const bool unchanged =
        Read<uintptr_t>(sceneSlot) == scene && Read<uintptr_t>(scene + kScenePointLightBuckets) == buckets;
    return unchanged ? LocalLightCapture::Captured : LocalLightCapture::TableChanged;
}

bool CaptureInteriorUnsafe(LocalLightInputs& out)
{
    const uintptr_t instance = Read<uintptr_t>(kCameraWmoInstance);
    const uint32_t count = Read<uint32_t>(kCameraWmoGroupCount);
    if (!instance || !count)
        return true;
    if (count > kMaxCameraWmoGroups || !ValidPointer(instance, kWmoInstanceRoot + sizeof(uintptr_t)))
        return false;
    const uintptr_t root = Read<uintptr_t>(instance + kWmoInstanceRoot);
    if (!ValidPointer(root, kWmoRootGroups) || !Read<uint32_t>(root + kWmoRootLoaded))
        return false;
    const uint32_t groupCount = Read<uint32_t>(root + kWmoRootGroupCount);
    const uintptr_t ids = Read<uintptr_t>(kCameraWmoGroupIds);
    if (!groupCount || groupCount > kMaxWmoGroups || !ValidPointer(ids, count * sizeof(uint32_t)) ||
        !ValidPointer(root, kWmoRootGroups + groupCount * sizeof(uintptr_t)))
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t groupId = Read<uint32_t>(ids + i * sizeof(uint32_t));
        if (groupId >= groupCount)
            return false;
        const uintptr_t group = Read<uintptr_t>(root + kWmoRootGroups + groupId * sizeof(uintptr_t));
        if (!ValidPointer(group, kWmoGroupLoaded + sizeof(uint32_t)))
            return false;
        if ((Read<uint32_t>(group + kWmoGroupLoaded) & 1) &&
            !(Read<uint32_t>(group + kWmoGroupFlags) & kWmoExteriorFogFlags))
            out.cameraInterior = true;
    }
    const float weight = Read<float>(kCameraInteriorFogBlend);
    if (out.cameraInterior && std::isfinite(weight))
        out.interiorBlend = std::clamp(weight, 0.0f, 1.0f);
    return Read<uintptr_t>(kCameraWmoInstance) == instance && Read<uint32_t>(kCameraWmoGroupCount) == count &&
           Read<uintptr_t>(kCameraWmoGroupIds) == ids;
}

LocalLightCapture CaptureInputsUnsafe(const float cameraPosition[3], bool withPointLights,
                                      const PointLightUpload& upload, LocalLightInputs& out)
{
    const LocalLightCapture points = withPointLights
                                         ? CapturePointLightsUnsafe(kWorldM2Scene, cameraPosition, upload, out)
                                         : LocalLightCapture::Captured;
    if (points != LocalLightCapture::Captured)
        return points;
    return CaptureInteriorUnsafe(out) ? LocalLightCapture::Captured : LocalLightCapture::InteriorRejected;
}

LocalLightCapture CaptureInputsGuarded(const float cameraPosition[3], bool withPointLights,
                                       const PointLightUpload& upload, LocalLightInputs& out)
{
    __try
    {
        return CaptureInputsUnsafe(cameraPosition, withPointLights, upload, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return LocalLightCapture::ReadFault;
    }
}

LocalLightCapture CapturePointLightTableGuarded(uintptr_t sceneSlot, const float cameraPosition[3],
                                                const PointLightUpload& upload, LocalLightInputs& out)
{
    __try
    {
        return CapturePointLightsUnsafe(sceneSlot, cameraPosition, upload, out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return LocalLightCapture::ReadFault;
    }
}
}

void LocalPointLightHistory::Remember(const LocalPointLight& light, const float cameraPosition[3], double seconds)
{
    if (light.nativeId)
        for (CachedLight& cached : m_cache)
            if (cached.occupied && cached.light.nativeId == light.nativeId)
            {
                cached = {light, seconds, true};
                return;
            }
    CachedLight* destination = nullptr;
    for (CachedLight& cached : m_cache)
    {
        if (cached.occupied && SameLight(cached.light, light))
        {
            cached = {light, seconds, true};
            return;
        }
        if (!cached.occupied)
            destination = &cached;
    }
    if (!destination)
    {
        destination = &m_cache[0];
        for (CachedLight& cached : m_cache)
            if (Precedes(destination->light, cached.light, cameraPosition))
                destination = &cached;
        if (!Precedes(light, destination->light, cameraPosition))
            return;
    }
    *destination = {light, seconds, true};
}

void LocalPointLightHistory::Select(const LocalLightInputs& captured, const float cameraPosition[3], double seconds,
                                   const PointLightUpload& upload, LocalLightInputs& selected)
{
    for (CachedLight& cached : m_cache)
    {
        if (!cached.occupied)
            continue;
        engine::UploadedPointLightColor(cached.light.color, upload, cached.light.uploadedColor);
        cached.light.cutoff = engine::PointLightCutoff(cached.light.uploadedColor, cached.light.attenuation);
        if (seconds - cached.lastRelevant >= kLocalPointLightExpirySeconds)
            cached.occupied = false;
        else if (WithinLightReach(cached.light, cameraPosition))
            cached.lastRelevant = seconds;
    }
    LocalLightInputs valid;
    for (uint32_t i = 0; i < std::min(captured.pointLightCount, kMaxLocalPointLights); ++i)
        if (captured.pointLights[i].enabled)
            engine::SelectLocalPointLight(valid, captured.pointLights[i], cameraPosition, upload);
    for (uint32_t i = 0; i < valid.pointLightCount; ++i)
        Remember(valid.pointLights[i], cameraPosition, seconds);
    for (const CachedLight& cached : m_cache)
        if (cached.occupied && (WithinLightReach(cached.light, cameraPosition) || FindLight(valid, cached.light) >= 0))
            engine::SelectLocalPointLight(selected, cached.light, cameraPosition, upload);
}

void LocalPointLightHistory::Fade(const LocalLightInputs& selected, double elapsed, const PointLightUpload& upload,
                                 LocalLightInputs& out)
{
    const double duration = std::isfinite(upload.fadeSeconds) ? std::max(upload.fadeSeconds, 0.0f) : 0.4;
    double availableSeconds[kMaxLocalPointLights];
    bool assigned[kMaxLocalPointLights] = {};
    for (uint32_t i = 0; i < kMaxLocalPointLights; ++i)
    {
        UploadedLight& slot = m_uploaded[i];
        availableSeconds[i] = elapsed;
        if (!slot.occupied)
            continue;
        const int selectedIndex = FindLight(selected, slot.light);
        if (selectedIndex >= 0 && !assigned[selectedIndex])
        {
            slot.light = selected.pointLights[selectedIndex];
            assigned[selectedIndex] = true;
            slot.weight = duration > 0.0 ? std::min(1.0, slot.weight + elapsed / duration) : 1.0;
        }
        else
        {
            availableSeconds[i] = std::max(0.0, elapsed - slot.weight * duration);
            slot.weight = duration > 0.0 ? std::max(0.0, slot.weight - elapsed / duration) : 0.0;
            if (slot.weight <= 0.0)
                slot = {};
        }
    }
    for (uint32_t index = 0; index < selected.pointLightCount; ++index)
    {
        if (assigned[index])
            continue;
        for (uint32_t i = 0; i < kMaxLocalPointLights; ++i)
        {
            UploadedLight& slot = m_uploaded[i];
            if (slot.occupied)
                continue;
            slot = {selected.pointLights[index],
                    duration > 0.0 ? std::min(1.0, availableSeconds[i] / duration) : 1.0, true};
            break;
        }
    }
    out.pointLightCount = 0;
    for (const UploadedLight& slot : m_uploaded)
    {
        if (!slot.occupied || slot.weight <= 0.0)
            continue;
        LocalPointLight& light = out.pointLights[out.pointLightCount++];
        light = slot.light;
        engine::UploadedPointLightColor(light.color, upload, light.uploadedColor);
        light.cutoff = engine::PointLightCutoff(light.uploadedColor, light.attenuation);
        const float intensity = static_cast<float>(slot.weight * slot.weight * (3.0 - 2.0 * slot.weight));
        for (float& channel : light.uploadedColor)
            channel *= intensity;
    }
}

void LocalPointLightHistory::Update(const LocalLightInputs& captured, const float cameraPosition[3], int mapId,
                                   double seconds, const PointLightUpload& upload, LocalLightInputs& out)
{
    if (captured.capture != LocalLightCapture::Captured || !std::isfinite(seconds) ||
        !ValidVector(cameraPosition, -kMaxWorldCoordinate, kMaxWorldCoordinate))
    {
        *this = {};
        out = captured;
        out.pointLightCount = 0;
        return;
    }
    if (m_initialized && (m_mapId != mapId || seconds < m_seconds ||
                          seconds - m_seconds > kLocalPointLightExpirySeconds ||
                          SquaredDistance(m_cameraPosition, cameraPosition) >=
                              kLocalPointLightTeleportDistance * kLocalPointLightTeleportDistance))
        *this = {};
    const double elapsed = m_initialized ? seconds - m_seconds : 0.0;
    LocalLightInputs selected;
    Select(captured, cameraPosition, seconds, upload, selected);
    out = captured;
    Fade(selected, elapsed, upload, out);
    std::memcpy(m_cameraPosition, cameraPosition, sizeof(m_cameraPosition));
    m_mapId = mapId;
    m_seconds = seconds;
    m_initialized = true;
}

namespace engine
{
void UploadedPointLightColor(const float captured[3], const PointLightUpload& upload, float uploaded[3])
{
    const float peak = PeakColor(captured);
    for (int channel = 0; channel < 3; ++channel)
        uploaded[channel] = (upload.linear ? LinearChannel(captured[channel], peak)
                                           : std::max(captured[channel], 0.0f)) * upload.intensity;
}

float PointLightCutoff(const float color[3], const float attenuation[3])
{
    if (!ValidVector(color, 0.0f, kMaxUploadedLightComponent) ||
        !ValidVector(attenuation, 0.0f, kMaxAttenuationCoefficient))
        return 0.0f;
    const double peak = PeakColor(color);
    const double target = peak / kLocalPointLightContributionCutoff;
    if (peak <= 0.0 || target <= std::max(static_cast<double>(attenuation[0]), 1.0))
        return 0.0f;
    const double remaining = target - attenuation[0];
    double radius = kMaxLocalPointLightRadius;
    if (attenuation[2] > 0.0f)
    {
        const double linear = attenuation[1];
        radius = 2.0 * remaining /
                 (linear + std::sqrt(linear * linear + 4.0 * attenuation[2] * remaining));
    }
    else if (attenuation[1] > 0.0f)
        radius = remaining / attenuation[1];
    return static_cast<float>(std::min(radius, static_cast<double>(kMaxLocalPointLightRadius)));
}

bool SelectLocalPointLight(LocalLightInputs& out, const LocalPointLight& light, const float cameraPosition[3],
                           const PointLightUpload& upload)
{
    if (out.pointLightCount > kMaxLocalPointLights ||
        !ValidVector(cameraPosition, -kMaxWorldCoordinate, kMaxWorldCoordinate) ||
        !ValidVector(light.position, -kMaxWorldCoordinate, kMaxWorldCoordinate) ||
        !ValidVector(light.color, 0.0f, kMaxLightComponent) || !std::isfinite(upload.intensity) ||
        upload.intensity < 0.0f)
        return false;
    LocalPointLight candidate = light;
    UploadedPointLightColor(candidate.color, upload, candidate.uploadedColor);
    candidate.cutoff = PointLightCutoff(candidate.uploadedColor, candidate.attenuation);
    const double limit = candidate.cutoff + kMaxLocalPointLightRadius;
    if (candidate.cutoff <= 0.0f || SquaredDistance(candidate.position, cameraPosition) > limit * limit)
        return false;
    uint32_t index = 0;
    while (index < out.pointLightCount && !Precedes(candidate, out.pointLights[index], cameraPosition))
        ++index;
    if (index == kMaxLocalPointLights)
        return false;
    const uint32_t count = std::min(out.pointLightCount + 1, kMaxLocalPointLights);
    for (uint32_t i = count - 1; i > index; --i)
        out.pointLights[i] = out.pointLights[i - 1];
    out.pointLights[index] = candidate;
    out.pointLightCount = count;
    return true;
}

const char* LocalLightCaptureName(LocalLightCapture capture)
{
    switch (capture)
    {
    case LocalLightCapture::Captured:
        return "captured";
    case LocalLightCapture::UnsupportedClient:
        return "client code differs from build 12340";
    case LocalLightCapture::CameraOutOfRange:
        return "camera position out of range";
    case LocalLightCapture::DamagedTable:
        return "damaged light table";
    case LocalLightCapture::DisabledLight:
        return "disabled light in the table";
    case LocalLightCapture::NonPointLight:
        return "non-point light in the table";
    case LocalLightCapture::TableChanged:
        return "table changed during the walk";
    case LocalLightCapture::ReadFault:
        return "read fault";
    case LocalLightCapture::InteriorRejected:
        return "camera interior groups rejected";
    }
    return "unknown";
}

LocalLightCapture CapturePointLightTable(uintptr_t sceneSlot, const float cameraPosition[3],
                                         const PointLightUpload& upload, LocalLightInputs& out)
{
    LocalLightInputs captured;
    const LocalLightCapture result = CapturePointLightTableGuarded(sceneSlot, cameraPosition, upload, captured);
    out = result == LocalLightCapture::Captured ? captured : LocalLightInputs{};
    out.capture = result;
    return result;
}

bool CaptureLocalLightInputs(const float cameraPosition[3], bool withPointLights, const PointLightUpload& upload,
                             LocalLightInputs& out)
{
    out = {};
    static const bool supported = SupportedLayout();
    if (!supported)
        out.capture = LocalLightCapture::UnsupportedClient;
    else if (!ValidVector(cameraPosition, -kMaxWorldCoordinate, kMaxWorldCoordinate))
        out.capture = LocalLightCapture::CameraOutOfRange;
    else
    {
        LocalLightInputs captured;
        const LocalLightCapture result = CaptureInputsGuarded(cameraPosition, withPointLights, upload, captured);
        if (result == LocalLightCapture::Captured)
            out = captured;
        out.capture = result;
    }
    return out.capture == LocalLightCapture::Captured;
}
}
