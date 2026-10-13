#pragma once

float InvalidLocalLightFloat()
{
    const uint32_t bits = 0x7FC00000;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool LocalNear(float a, float b)
{
    return std::fabs(a - b) <= 1.0e-5f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

LocalPointLight PointLightFixture(float x)
{
    LocalPointLight light;
    light.position[0] = x;
    light.color[0] = 1.0f;
    light.color[1] = 0.5f;
    light.color[2] = 0.25f;
    light.attenuation[0] = 1.0f;
    light.attenuation[2] = 1.0f;
    return light;
}

void CheckLocalLightInputs()
{
    LocalPointLight light = PointLightFixture(0.0f);
    const float quadraticRadius = engine::PointLightCutoff(light.color, light.attenuation);
    light.attenuation[1] = 2.0f;
    light.attenuation[2] = 0.0f;
    const float linearRadius = engine::PointLightCutoff(light.color, light.attenuation);
    light.attenuation[1] = 0.0f;
    const float constantRadius = engine::PointLightCutoff(light.color, light.attenuation);
    Check(LocalNear(quadraticRadius, std::sqrt(255.0f)) && LocalNear(linearRadius, 127.5f) &&
              constantRadius == kMaxLocalPointLightRadius,
          "point-light bounds follow native quadratic, linear and constant attenuation");
    light.attenuation[0] = 1000.0f;
    const float dimRadius = engine::PointLightCutoff(light.color, light.attenuation);
    light.attenuation[0] = -1.0f;
    const float negativeRadius = engine::PointLightCutoff(light.color, light.attenuation);
    light.attenuation[0] = 1.0f;
    light.color[1] = InvalidLocalLightFloat();
    const float invalidRadius = engine::PointLightCutoff(light.color, light.attenuation);
    Check(dimRadius == 0.0f && negativeRadius == 0.0f && invalidRadius == 0.0f,
          "dim, negative and nonfinite point-light inputs cannot create a fog light");

    const float camera[3] = {};
    LocalLightInputs forwards;
    LocalLightInputs backwards;
    for (int i = 0; i < 16; ++i)
    {
        engine::SelectLocalPointLight(forwards, PointLightFixture(static_cast<float>(i)), camera, {});
        engine::SelectLocalPointLight(backwards, PointLightFixture(static_cast<float>(15 - i)), camera, {});
    }
    bool strongest = forwards.pointLightCount == kMaxLocalPointLights &&
                     backwards.pointLightCount == kMaxLocalPointLights;
    for (uint32_t i = 0; i < kMaxLocalPointLights; ++i)
        strongest = strongest && forwards.pointLights[i].position[0] == static_cast<float>(i) &&
                    backwards.pointLights[i].position[0] == static_cast<float>(i);
    Check(strongest, "eight strongest nearby point lights are selected independently of native list order");
    light = PointLightFixture(40.0f);
    light.color[0] = 64.0f;
    const bool brighterSelected = engine::SelectLocalPointLight(forwards, light, camera, {});
    bool present = false;
    for (uint32_t i = 0; i < forwards.pointLightCount; ++i)
        present = present || forwards.pointLights[i].position[0] == 40.0f;
    Check(brighterSelected && present && forwards.pointLightCount == kMaxLocalPointLights,
          "point-light selection accounts for brightness as well as distance");
    light = PointLightFixture(1000.0f);
    const bool farSelected = engine::SelectLocalPointLight(forwards, light, camera, {});
    light.position[0] = InvalidLocalLightFloat();
    const bool invalidSelected = engine::SelectLocalPointLight(forwards, light, camera, {});
    Check(!farSelected && !invalidSelected && forwards.pointLightCount == kMaxLocalPointLights,
          "point-light selection rejects invalid positions and centres outside the capture neighbourhood");

    bool clearedBoth = true;
    for (bool withPointLights : {true, false})
    {
        LocalLightInputs unsupported = forwards;
        unsupported.cameraInterior = true;
        unsupported.interiorBlend = 1.0f;
        const bool captured = engine::CaptureLocalLightInputs(camera, withPointLights, {}, unsupported);
        clearedBoth = clearedBoth && !captured && unsupported.pointLightCount == 0 &&
                      !unsupported.cameraInterior && unsupported.interiorBlend == 0.0f &&
                      unsupported.capture == LocalLightCapture::UnsupportedClient;
    }
    Check(clearedBoth,
          "native local-light acquisition rejects the harness image as an unsupported client and clears stale inputs "
          "whether or not the point-light walk is requested (the walk skip itself needs the client image)");
}

struct TablePointLight
{
    uintptr_t scene = 0;
    uint32_t frameStamp = 0;
    uint32_t type = 1;
    float position[3] = {};
    float viewPosition[3] = {};
    float direction[3] = {};
    float ambient[3] = {};
    float color[3] = {1.0f, 0.5f, 0.25f};
    float specular[3] = {};
    float attenuation[3] = {0.0f, 0.7f, 0.03f};
    uint32_t enabled = 1;
    uintptr_t previousLink = 0;
    uintptr_t next = 0;
};

static_assert(sizeof(TablePointLight) == 0x6C && offsetof(TablePointLight, color) == 0x3C &&
                  offsetof(TablePointLight, enabled) == 0x60 && offsetof(TablePointLight, next) == 0x68,
              "the table fixture follows the client's point-light layout recorded in README");

constexpr size_t kSceneBucketsWord = 0x24 / sizeof(uintptr_t);
constexpr size_t kTableBucketCount = 4096;
constexpr size_t kLitBucket = 1234;

struct PointLightTable
{
    uintptr_t sceneSlot = 0;
    uintptr_t scene[kSceneBucketsWord + 1] = {};
    uintptr_t buckets[kTableBucketCount] = {};
    TablePointLight lights[2];
};

void LinkPointLightTable(PointLightTable& table)
{
    table.sceneSlot = reinterpret_cast<uintptr_t>(table.scene);
    table.scene[kSceneBucketsWord] = reinterpret_cast<uintptr_t>(table.buckets);
    uintptr_t* link = &table.buckets[kLitBucket];
    float x = 10.0f;
    for (TablePointLight& light : table.lights)
    {
        light = {};
        light.position[0] = x;
        x += 10.0f;
        light.scene = table.sceneSlot;
        light.previousLink = reinterpret_cast<uintptr_t>(link);
        *link = reinterpret_cast<uintptr_t>(&light);
        link = &light.next;
    }
}

LocalLightCapture CaptureTable(PointLightTable& table, LocalLightInputs& out)
{
    const float camera[3] = {};
    return engine::CapturePointLightTable(reinterpret_cast<uintptr_t>(&table.sceneSlot), camera, {}, out);
}

bool RejectedWith(PointLightTable& table, LocalLightCapture expected)
{
    LocalLightInputs out;
    out.pointLightCount = 1;
    return CaptureTable(table, out) == expected && out.capture == expected && out.pointLightCount == 0;
}

void CheckPointLightTableCapture()
{
    auto table = std::make_unique<PointLightTable>();
    LinkPointLightTable(*table);
    LocalLightInputs walked;
    const LocalLightCapture captured = CaptureTable(*table, walked);
    Check(captured == LocalLightCapture::Captured && walked.capture == LocalLightCapture::Captured &&
              walked.pointLightCount == 2 &&
              walked.pointLights[0].nativeId == reinterpret_cast<uintptr_t>(&table->lights[0]) &&
              walked.pointLights[1].nativeId == reinterpret_cast<uintptr_t>(&table->lights[1]) &&
              walked.pointLights[1].enabled == 1 && walked.pointLights[1].attenuation[2] == 0.03f,
          "the point-light walk captures every enabled point light of a linked table with its address and inputs");

    table->lights[1].enabled = 0;
    const bool disabled = RejectedWith(*table, LocalLightCapture::DisabledLight);
    LinkPointLightTable(*table);
    table->lights[1].type = 0;
    const bool nonPoint = RejectedWith(*table, LocalLightCapture::NonPointLight);
    LinkPointLightTable(*table);
    table->lights[1].previousLink = reinterpret_cast<uintptr_t>(&table->buckets[kLitBucket]);
    const bool damaged = RejectedWith(*table, LocalLightCapture::DamagedTable);
    Check(disabled && nonPoint && damaged,
          "the point-light walk names why it rejects a table: a disabled light, a non-point light or a broken link, "
          "and keeps no lights from it");

    void* unmapped = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    LinkPointLightTable(*table);
    table->sceneSlot = reinterpret_cast<uintptr_t>(unmapped);
    const bool fault = unmapped && RejectedWith(*table, LocalLightCapture::ReadFault);
    if (unmapped)
        VirtualFree(unmapped, 0, MEM_RELEASE);
    table->sceneSlot = 0;
    LocalLightInputs noScene;
    const bool empty = CaptureTable(*table, noScene) == LocalLightCapture::Captured && noScene.pointLightCount == 0;
    Check(fault && empty,
          "a scene pointer into unmapped memory is a read fault, and no scene is an empty capture, not a rejection");
}

void CheckViewCulledPointLightCapture()
{
    auto table = std::make_unique<PointLightTable>();
    LinkPointLightTable(*table);
    LocalLightInputs before;
    LocalLightInputs turned;
    const bool captured = CaptureTable(*table, before) == LocalLightCapture::Captured;
    table->lights[0].next = 0;
    const bool recaptured = CaptureTable(*table, turned) == LocalLightCapture::Captured;
    std::printf("     fixed camera, simulated view cull: native/uploaded lights %u -> %u; "
                "remaining lamp x %.1f -> %.1f\n", before.pointLightCount, turned.pointLightCount,
                before.pointLights[0].position[0], turned.pointLights[0].position[0]);
    Check(captured && recaptured && before.pointLightCount == 2 && turned.pointLightCount == 1 &&
              before.pointLights[0].nativeId == turned.pointLights[0].nativeId,
          "at a fixed camera a lamp unlinked by simulated view culling disappears from raw capture in one frame");
}

LocalLightInputs CapturedLight(float x)
{
    const float camera[3] = {};
    LocalLightInputs captured;
    engine::SelectLocalPointLight(captured, PointLightFixture(x), camera, {});
    return captured;
}

float UploadedRedAt(const LocalLightInputs& lights, float x)
{
    for (uint32_t i = 0; i < lights.pointLightCount; ++i)
        if (lights.pointLights[i].position[0] == x)
            return lights.pointLights[i].uploadedColor[0];
    return 0.0f;
}

void CheckMissingPointLightsPersist()
{
    auto table = std::make_unique<PointLightTable>();
    LinkPointLightTable(*table);
    const float camera[3] = {};
    const PointLightUpload immediate = {true, 1.0f, 0.0f};
    LocalPointLightHistory history;
    LocalLightInputs captured;
    LocalLightInputs uploaded;
    CaptureTable(*table, captured);
    history.Update(captured, camera, 0, 0.0, immediate, uploaded);
    table->lights[0].next = 0;
    bool persists = uploaded.pointLightCount == 2;
    for (int frame = 1; frame <= 300; ++frame)
    {
        CaptureTable(*table, captured);
        history.Update(captured, camera, 0, frame / 60.0, immediate, uploaded);
        persists = persists && uploaded.pointLightCount == 2 && UploadedRedAt(uploaded, 20.0f) == 1.0f;
    }
    Check(persists && captured.pointLightCount == 1,
          "a view-culled lamp stays uploaded beyond the expiry time while the fixed camera remains in reach");
    LinkPointLightTable(*table);
    CaptureTable(*table, captured);
    captured.pointLights[1].nativeId += 4;
    history.Update(captured, camera, 0, 5.1, {}, uploaded);
    Check(uploaded.pointLightCount == 2 && UploadedRedAt(uploaded, 20.0f) == 1.0f,
          "relinking a position/colour under another native address neither duplicates nor restarts its fade");
}

void CheckPointLightFades()
{
    const float nearCamera[3] = {};
    const float farCamera[3] = {25.0f, 0.0f, 0.0f};
    const LocalLightInputs captured = CapturedLight(0.0f);
    const PointLightUpload upload = {true, 1.0f, 0.4f};
    bool monotonic = true;
    bool independent = true;
    for (int rate : {20, 60, 120})
    {
        LocalPointLightHistory history;
        LocalLightInputs uploaded;
        history.Update(captured, nearCamera, 0, 0.0, upload, uploaded);
        float previous = 0.0f;
        for (int frame = 1; frame <= rate * 2 / 5; ++frame)
        {
            history.Update(captured, nearCamera, 0, static_cast<double>(frame) / rate, upload, uploaded);
            const float red = UploadedRedAt(uploaded, 0.0f);
            monotonic = monotonic && red >= previous && red <= 1.0f;
            if (frame == rate / 5)
                independent = independent && LocalNear(red, 0.5f);
            previous = red;
        }
        independent = independent && LocalNear(previous, 1.0f);
        history.Update({}, farCamera, 0, 0.4, upload, uploaded);
        for (int frame = 1; frame <= rate * 2 / 5; ++frame)
        {
            history.Update({}, farCamera, 0, 0.4 + static_cast<double>(frame) / rate, upload, uploaded);
            const float red = UploadedRedAt(uploaded, 0.0f);
            monotonic = monotonic && red <= previous && red >= 0.0f;
            if (frame == rate / 5)
                independent = independent && LocalNear(red, 0.5f);
            previous = red;
        }
        independent = independent && LocalNear(previous, 0.0f);
    }
    Check(monotonic && independent,
          "uploaded intensity eases monotonically in/out in 0.4 s with matching midpoints at 20/60/120 fps");
    LocalPointLightHistory irregular;
    LocalLightInputs uploaded;
    irregular.Update(captured, nearCamera, 0, 0.0, upload, uploaded);
    for (double seconds : {0.013, 0.09, 0.11, 0.2})
        irregular.Update(captured, nearCamera, 0, seconds, upload, uploaded);
    const bool halfIn = LocalNear(UploadedRedAt(uploaded, 0.0f), 0.5f);
    irregular.Update({}, farCamera, 0, 0.3, upload, uploaded);
    const bool reverses = LocalNear(UploadedRedAt(uploaded, 0.0f), 0.15625f);
    irregular.Update({}, farCamera, 0, 0.4, upload, uploaded);
    Check(halfIn && reverses && LocalNear(UploadedRedAt(uploaded, 0.0f), 0.0f),
          "irregular frame intervals preserve the fade and a half-lit light can reverse continuously");
    LocalPointLightHistory immediate;
    immediate.Update(captured, nearCamera, 0, 0.0, {true, 1.0f, 0.0f}, uploaded);
    const bool full = UploadedRedAt(uploaded, 0.0f) == 1.0f;
    immediate.Update({}, farCamera, 0, 0.1, {true, 1.0f, 0.0f}, uploaded);
    Check(full && uploaded.pointLightCount == 0, "LocalLightFadeSeconds=0 disables both fade directions");
}

void CheckPointLightHistoryExpiry()
{
    const float nearCamera[3] = {};
    const float farCamera[3] = {25.0f, 0.0f, 0.0f};
    const float teleported[3] = {150.0f, 0.0f, 0.0f};
    const PointLightUpload immediate = {true, 1.0f, 0.0f};
    const LocalLightInputs captured = CapturedLight(0.0f);
    LocalPointLightHistory history;
    LocalLightInputs uploaded;
    history.Update({}, nearCamera, 0, 0.0, immediate, uploaded);
    const bool noInvented = uploaded.pointLightCount == 0;
    history.Update(captured, nearCamera, 0, 0.1, immediate, uploaded);
    history.Update({}, farCamera, 0, 0.5, immediate, uploaded);
    history.Update({}, nearCamera, 0, 1.0, immediate, uploaded);
    const bool returned = uploaded.pointLightCount == 1;
    for (double seconds : {1.5, 2.0, 2.5})
        history.Update({}, farCamera, 0, seconds, immediate, uploaded);
    history.Update({}, nearCamera, 0, 3.2, immediate, uploaded);
    Check(noInvented && returned && uploaded.pointLightCount == 0,
          "only reported lights persist; a missing light expires after two seconds outside its cutoff");
    history.Update(captured, nearCamera, 0, 3.3, immediate, uploaded);
    history.Update({}, nearCamera, 1, 3.4, immediate, uploaded);
    const bool mapCleared = uploaded.pointLightCount == 0;
    history.Update(captured, nearCamera, 1, 3.5, immediate, uploaded);
    history.Update({}, teleported, 1, 3.6, immediate, uploaded);
    history.Update({}, nearCamera, 1, 3.7, immediate, uploaded);
    const bool jumpCleared = uploaded.pointLightCount == 0;
    history.Update(captured, nearCamera, 1, 3.8, immediate, uploaded);
    history.Update({}, nearCamera, 1, 7.0, immediate, uploaded);
    Check(mapCleared && jumpCleared && uploaded.pointLightCount == 0,
          "map changes, 100-yard camera jumps and capture gaps clear lights from the previous scene");
    history.Update(captured, nearCamera, 1, 7.1, immediate, uploaded);
    LocalLightInputs rejected;
    rejected.capture = LocalLightCapture::DamagedTable;
    history.Update(rejected, nearCamera, 1, 7.2, immediate, uploaded);
    const bool rejectedCleared = uploaded.pointLightCount == 0 && uploaded.capture == rejected.capture;
    history.Update({}, nearCamera, 1, 7.3, immediate, uploaded);
    Check(rejectedCleared && uploaded.pointLightCount == 0, "rejected capture clears history and keeps its reason");
}

void CheckPointLightSelectionFades()
{
    const float camera[3] = {};
    LocalLightInputs initial;
    for (uint32_t i = 0; i < kMaxLocalPointLights; ++i)
        engine::SelectLocalPointLight(initial, PointLightFixture(static_cast<float>(i)), camera, {});
    LocalPointLight bright = PointLightFixture(10.0f);
    bright.color[0] = 100.0f;
    LocalLightInputs replacement;
    engine::SelectLocalPointLight(replacement, bright, camera, {});
    bool bounded = true;
    bool independent = true;
    for (int rate : {20, 60, 120})
    {
        LocalPointLightHistory history;
        LocalLightInputs uploaded;
        history.Update(initial, camera, 0, 0.0, {true, 1.0f, 0.0f}, uploaded);
        float previousOutgoing = 1.0f;
        float previousIncoming = 0.0f;
        for (int frame = 1; frame <= rate; ++frame)
        {
            history.Update(replacement, camera, 0, static_cast<double>(frame) / rate, {}, uploaded);
            const float outgoing = UploadedRedAt(uploaded, 7.0f);
            const float incoming = UploadedRedAt(uploaded, 10.0f);
            bounded = bounded && uploaded.pointLightCount <= kMaxLocalPointLights &&
                      outgoing <= previousOutgoing && incoming >= previousIncoming;
            if (frame == rate / 5)
                independent = independent && LocalNear(outgoing, 0.5f) && incoming == 0.0f;
            if (frame == rate * 3 / 5)
                independent = independent && outgoing == 0.0f && LocalNear(incoming, 50.0f);
            previousOutgoing = outgoing;
            previousIncoming = incoming;
        }
        independent = independent && uploaded.pointLightCount == kMaxLocalPointLights &&
                      LocalNear(UploadedRedAt(uploaded, 10.0f), 100.0f);
    }
    Check(bounded && independent,
          "priority replacement fades out the weakest lamp before admitting its successor, never exceeding eight");
    LocalPointLightHistory crowded;
    LocalLightInputs uploaded;
    for (uint32_t i = 0; i < kLocalPointLightCacheSize * 2; ++i)
        crowded.Update(CapturedLight(static_cast<float>(i) * 0.1f), camera, 0, i * 0.01,
                       {true, 1.0f, 0.0f}, uploaded);
    crowded.Update({}, camera, 0, 1.0, {true, 1.0f, 0.0f}, uploaded);
    Check(uploaded.pointLightCount == kMaxLocalPointLights && UploadedRedAt(uploaded, 0.0f) == 1.0f &&
              UploadedRedAt(uploaded, 6.3f) == 0.0f,
          "a full bounded cache preserves priority when more reported lamps arrive than it can hold");
}

void CheckAnimatedPointLightHistory()
{
    const float camera[3] = {};
    LocalPointLightHistory history;
    LocalLightInputs captured = CapturedLight(0.0f);
    captured.pointLights[0].nativeId = 123;
    LocalLightInputs uploaded;
    history.Update(captured, camera, 0, 0.0, {}, uploaded);
    bool continuous = true;
    for (int frame = 1; frame <= 24; ++frame)
    {
        LocalPointLight& light = captured.pointLights[0];
        light.position[0] = frame * 0.2f;
        light.color[0] = 1.0f - frame * 0.01f;
        history.Update(captured, camera, 0, frame / 60.0, {}, uploaded);
        const double weight = frame / 24.0;
        const float expected = static_cast<float>(std::pow(light.color[0], kLocalLightGamma) *
                                                   weight * weight * (3.0 - 2.0 * weight));
        continuous = continuous && uploaded.pointLightCount == 1 &&
                     LocalNear(UploadedRedAt(uploaded, light.position[0]), expected);
    }
    Check(continuous, "a reported light can animate colour and position without restarting its fade or trailing");
    LocalPointLightHistory distinct;
    LocalLightInputs pair = CapturedLight(0.0f);
    pair.pointLights[0].nativeId = 100;
    pair.pointLights[1] = pair.pointLights[0];
    pair.pointLights[1].nativeId = 200;
    pair.pointLights[1].attenuation[2] = 0.5f;
    pair.pointLightCount = 2;
    distinct.Update(pair, camera, 0, 0.0, {true, 1.0f, 0.0f}, uploaded);
    Check(uploaded.pointLightCount == 2 &&
              uploaded.pointLights[0].attenuation[2] != uploaded.pointLights[1].attenuation[2],
          "coincident lights with different attenuation keep their distinct reported contributions");
}

constexpr float kStatueGreen = 255.0f;
constexpr float kStatueRed = 65.9f;
constexpr float kLinearGamma = 2.2f;

LocalPointLight ColouredLightFixture(float x, float red, float green, float blue)
{
    LocalPointLight light = PointLightFixture(x);
    light.color[0] = red;
    light.color[1] = green;
    light.color[2] = blue;
    return light;
}

bool UploadedColourIs(const LocalPointLight& light, const PointLightUpload& upload, const float (&expected)[3])
{
    float uploaded[3];
    engine::UploadedPointLightColor(light.color, upload, uploaded);
    bool matches = true;
    for (int channel = 0; channel < 3; ++channel)
        matches = matches && LocalNear(uploaded[channel], expected[channel]);
    if (!matches)
        std::printf("     uploaded colour %.6g %.6g %.6g, expected %.6g %.6g %.6g\n", uploaded[0], uploaded[1],
                    uploaded[2], expected[0], expected[1], expected[2]);
    return matches;
}

void CheckHdrLocalLightColours()
{
    const PointLightUpload linear = {true, 1.0f};
    const PointLightUpload gamma = {false, 1.0f};
    const LocalPointLight statue = ColouredLightFixture(0.0f, kStatueRed, kStatueGreen, kStatueGreen);
    const float statueChroma = std::pow(kStatueRed / kStatueGreen, kLinearGamma) * kStatueGreen;
    Check(UploadedColourIs(statue, linear, {statueChroma, kStatueGreen, kStatueGreen}),
          "an HDR point light linearises its chromaticity and keeps its peak intensity linear (255, not 255^2.2)");
    const LocalPointLight halfStatue =
        ColouredLightFixture(0.0f, kStatueRed * 0.5f, kStatueGreen * 0.5f, kStatueGreen * 0.5f);
    Check(UploadedColourIs(halfStatue, linear, {statueChroma * 0.5f, kStatueGreen * 0.5f, kStatueGreen * 0.5f}),
          "halving an HDR light's intensity halves its uploaded colour");
    const LocalPointLight torch = ColouredLightFixture(0.0f, 1.0f, 0.5f, 0.25f);
    Check(UploadedColourIs(torch, linear, {1.0f, std::pow(0.5f, kLinearGamma), std::pow(0.25f, kLinearGamma)}) &&
              UploadedColourIs(torch, {true, 2.0f},
                               {2.0f, 2.0f * std::pow(0.5f, kLinearGamma), 2.0f * std::pow(0.25f, kLinearGamma)}),
          "a point light no brighter than 1 keeps the gamma 2.2 decode, times LocalLightIntensity");
    Check(UploadedColourIs(statue, gamma, {kStatueRed, kStatueGreen, kStatueGreen}),
          "with ColorSpace=0 the captured colour is uploaded unchanged");

    const float camera[3] = {};
    const LocalPointLight dim = ColouredLightFixture(0.0f, 0.5f, 0.5f, 0.5f);
    LocalLightInputs linearDim;
    LocalLightInputs gammaDim;
    engine::SelectLocalPointLight(linearDim, dim, camera, linear);
    engine::SelectLocalPointLight(gammaDim, dim, camera, gamma);
    const float linearPeak = std::pow(0.5f, kLinearGamma);
    Check(linearDim.pointLightCount == 1 && gammaDim.pointLightCount == 1 &&
              LocalNear(linearDim.pointLights[0].cutoff,
                        std::sqrt(linearPeak / kLocalPointLightContributionCutoff - 1.0f)) &&
              LocalNear(gammaDim.pointLights[0].cutoff, std::sqrt(0.5f / kLocalPointLightContributionCutoff - 1.0f)) &&
              LocalNear(linearDim.pointLights[0].uploadedColor[0], linearPeak),
          "a point light's reach is cut off at 1/256 of the colour it uploads, not of the captured gamma colour");

    const LocalPointLight nearGrey = ColouredLightFixture(0.0f, 0.9f, 0.9f, 0.9f);
    const LocalPointLight fartherBright = ColouredLightFixture(1.163f, 2.0f, 2.0f, 2.0f);
    LocalLightInputs linearOrder;
    LocalLightInputs gammaOrder;
    for (const LocalPointLight* light : {&nearGrey, &fartherBright})
    {
        engine::SelectLocalPointLight(linearOrder, *light, camera, linear);
        engine::SelectLocalPointLight(gammaOrder, *light, camera, gamma);
    }
    Check(linearOrder.pointLightCount == 2 && gammaOrder.pointLightCount == 2 &&
              linearOrder.pointLights[0].position[0] == fartherBright.position[0] &&
              gammaOrder.pointLights[0].position[0] == nearGrey.position[0],
          "point lights are ranked by the colour they upload: in linear light a farther HDR light outranks a nearer "
          "grey one that is brighter in gamma");

    LocalLightInputs unlit;
    Check(!engine::SelectLocalPointLight(unlit, torch, camera, {true, 0.0f}) && unlit.pointLightCount == 0,
          "LocalLightIntensity=0 uploads no point light");
}

FrameInputs InteriorFogFixture()
{
    FrameInputs input = {};
    input.farClip = 1000.0f;
    input.fogStart = 60.0f;
    input.fogEnd = 600.0f;
    input.zoneFogDistance = 600.0f;
    input.toLight[2] = 1.0f;
    input.fogColor = 0xFF4080C0;
    input.directColor = 0xFFFFFFFF;
    input.sunColor = 0xFFFFFFFF;
    return input;
}

void CheckInteriorFogInputs()
{
    Config config;
    FrameInputs input = InteriorFogFixture();
    const FogParams outside = BuildFogParams(input, config, nullptr);
    input.localLights.interiorBlend = 1.0f;
    const FogParams unclassified = BuildFogParams(input, config, nullptr);
    input.localLights.cameraInterior = true;
    input.localLights.interiorBlend = 0.0f;
    const FogParams threshold = BuildFogParams(input, config, nullptr);
    Check(LocalNear(outside.layers[0].density, unclassified.layers[0].density) &&
              LocalNear(outside.layers[0].density, threshold.layers[0].density) &&
              LocalNear(outside.layers[0].diffuse[0], unclassified.layers[0].diffuse[0]),
          "outdoor fog is unchanged without interior membership or at the doorway threshold");

    input.localLights.interiorBlend = 0.5f;
    const FogParams middle = BuildFogParams(input, config, nullptr);
    input.localLights.interiorBlend = 1.0f;
    const FogParams inside = BuildFogParams(input, config, nullptr);
    bool fades = true;
    for (int i = 0; i < kSceneLayers; ++i)
        fades = fades && LocalNear(middle.layers[i].density, outside.layers[i].density * 0.575f) &&
                LocalNear(inside.layers[i].density, outside.layers[i].density * 0.15f);
    for (int i = 0; i < kFogLayers; ++i)
        for (int channel = 0; channel < 3; ++channel)
            fades = fades && LocalNear(middle.layers[i].diffuse[channel], outside.layers[i].diffuse[channel] * 0.5f) &&
                    inside.layers[i].diffuse[channel] == 0.0f;
    Check(fades && inside.lightVisibility == 0.0f && LocalNear(middle.lightVisibility, 0.5f),
          "interior transitions continuously reduce outdoor density and direct sunlight");
    Check(LocalNear(inside.layers[kDistanceFogLayer].density, outside.layers[kDistanceFogLayer].density * 0.4f) &&
              LocalNear(middle.layers[kDistanceFogLayer].density, outside.layers[kDistanceFogLayer].density * 0.7f) &&
              LocalNear(inside.layers[kDistanceFogLayer].emissive[2], outside.layers[kDistanceFogLayer].emissive[2]) &&
              inside.farLimit == input.fogEnd,
          "interior transitions preserve the native WMO distance-fog colour and range");

    for (float retained : {0.0f, 0.4f, 1.0f, -1.0f, 2.0f, InvalidLocalLightFloat()})
    {
        config.interiorFarFog = retained;
        const float expected = std::isfinite(retained) ? std::clamp(retained, 0.0f, 1.0f) : 0.4f;
        input.localLights.interiorBlend = 1.0f;
        const FogParams full = BuildFogParams(input, config, nullptr);
        input.localLights.interiorBlend = 0.5f;
        const FogParams half = BuildFogParams(input, config, nullptr);
        Check(LocalNear(full.layers[kDistanceFogLayer].density,
                        outside.layers[kDistanceFogLayer].density * expected) &&
                  LocalNear(half.layers[kDistanceFogLayer].density,
                            outside.layers[kDistanceFogLayer].density * (1.0f + expected) * 0.5f) &&
                  LocalNear(full.layers[0].density, inside.layers[0].density),
              "InteriorFarFog clamps, fades continuously and leaves scene-layer density independent");
    }
    config.interiorFarFog = 0.4f;
    input.localLights.interiorBlend = 1.0f;
    config.interiorAware = false;
    const FogParams disabled = BuildFogParams(input, config, nullptr);
    config.interiorAware = true;
    input.localLights.interiorBlend = InvalidLocalLightFloat();
    const FogParams invalid = BuildFogParams(input, config, nullptr);
    Check(LocalNear(disabled.layers[0].density, outside.layers[0].density) &&
              LocalNear(disabled.layers[0].diffuse[0], outside.layers[0].diffuse[0]) &&
              LocalNear(invalid.layers[0].density, outside.layers[0].density) &&
              LocalNear(disabled.layers[kDistanceFogLayer].density, outside.layers[kDistanceFogLayer].density) &&
              LocalNear(invalid.layers[kDistanceFogLayer].density, outside.layers[kDistanceFogLayer].density),
          "interior bypass and nonfinite transition weights preserve outdoor fog");

    AuthoredFog authored = {};
    authored.layerCount = 1;
    authored.coverage = 1.0f;
    authored.layers[0].density = 1.0f;
    authored.layers[0].exponent = 1.0f;
    authored.layers[0].intensity = 1.0f;
    authored.layers[0].diffuse[0] = 1.0f;
    authored.layers[0].emissive[0] = 0.5f;
    input.localLights.interiorBlend = 0.0f;
    const FogParams authoredOutside = BuildFogParams(input, config, &authored);
    input.localLights.interiorBlend = 1.0f;
    const FogParams authoredInside = BuildFogParams(input, config, &authored);
    Check(LocalNear(authoredInside.layers[0].density, authoredOutside.layers[0].density * 0.15f) &&
              authoredInside.layers[0].diffuse[0] == 0.0f &&
              authoredOutside.layers[kDistanceFogLayer].density == 0.0f &&
              LocalNear(authoredInside.layers[kDistanceFogLayer].density, inside.layers[kDistanceFogLayer].density),
          "dense Classic outdoor layers give way to native interior distance fog");

    ConfigStore settings;
    Config edited = settings.Get();
    edited.localLightIntensity = 20.0f;
    edited.localLightFadeSeconds = 20.0f;
    edited.localLightPhase = 2.0f;
    edited.interiorDensity = -1.0f;
    edited.interiorFarFog = 2.0f;
    edited.localLights = false;
    edited.interiorAware = false;
    settings.Apply(edited);
    Check(settings.Get().localLightIntensity == 8.0f && settings.Get().localLightFadeSeconds == 5.0f &&
              settings.Get().localLightPhase == 0.9f &&
              settings.Get().interiorDensity == 0.0f && settings.Get().interiorFarFog == 1.0f &&
              !settings.Get().localLights &&
              !settings.Get().interiorAware && settings.HasUnsavedChanges(),
          "local-light and interior controls clamp and participate in live-setting changes");
    edited.interiorFarFog = -1.0f;
    edited.localLightPhase = -2.0f;
    settings.Apply(edited);
    Config shipped;
    Config farEdited = shipped;
    farEdited.interiorFarFog = 0.8f;
    Check(settings.Get().interiorFarFog == 0.0f && shipped.interiorFarFog == 0.4f &&
              !SameFogSettings(shipped, farEdited) &&
              SettingChanges(shipped, farEdited) == "InteriorFarFog 0.4 -> 0.8",
          "InteriorFarFog defaults to 0.4, clamps to 0..1, discards fog history and logs its changes");
    Check(shipped.localLightFadeSeconds == 0.4f && LocalLightUpload(shipped).fadeSeconds == 0.4f,
          "the default local-light fade reaches frame acquisition through the existing upload settings");
    Config phased = shipped;
    phased.localLightPhase = 0.5f;
    Check(settings.Get().localLightPhase == -0.9f && shipped.localLightPhase == 0.3f &&
              !SameFogSettings(shipped, phased) &&
              SettingChanges(shipped, phased) == "LocalLightPhase 0.3 -> 0.5",
          "LocalLightPhase defaults to 0.3, clamps to -0.9..0.9, discards fog history and logs its changes");
}

void CheckStormHidesTheSun()
{
    Config config;
    FrameInputs input = InteriorFogFixture();
    const FogParams clear = BuildFogParams(input, config, nullptr);
    input.lightParams.stormBlend = 0.5f;
    const FogParams half = BuildFogParams(input, config, nullptr);
    input.lightParams.stormBlend = 1.0f;
    const FogParams full = BuildFogParams(input, config, nullptr);
    input.lightParams.stormBlend = 2.0f;
    const FogParams beyond = BuildFogParams(input, config, nullptr);
    input.lightParams.stormBlend = InvalidLocalLightFloat();
    const FogParams invalid = BuildFogParams(input, config, nullptr);
    Check(clear.lightVisibility > 0.0f && LocalNear(half.lightVisibility, clear.lightVisibility * 0.5f) &&
              full.lightVisibility == 0.0f && beyond.lightVisibility == 0.0f &&
              LocalNear(invalid.lightVisibility, clear.lightVisibility),
          "a storm hides direct sunlight continuously with its weight; nonfinite weights leave it unchanged");
    Check(LocalNear(full.layers[0].density, clear.layers[0].density) &&
              LocalNear(full.layers[kDistanceFogLayer].density, clear.layers[kDistanceFogLayer].density),
          "hiding the sun leaves fog density to the storm's light tables");
}
