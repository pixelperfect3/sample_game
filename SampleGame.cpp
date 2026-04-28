#include "SampleGame.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "engine/assets/GltfLoader.h"
#include "engine/audio/IAudioEngine.h"
#include "engine/core/Engine.h"
#include "engine/ecs/Registry.h"
#include "engine/input/InputState.h"
#include "engine/input/Key.h"
#include "engine/physics/PhysicsComponents.h"
#include "engine/rendering/EcsComponents.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/MeshBuilder.h"
#include "engine/rendering/RenderPass.h"
#include "engine/rendering/ViewIds.h"
#include "engine/scene/SceneSerializer.h"
#include "engine/scene/TransformSystem.h"
#include "engine/ui/UiEvent.h"
#include "engine/ui/widgets/UiButton.h"
#include "engine/ui/widgets/UiPanel.h"
#include "engine/ui/widgets/UiText.h"

#ifdef __ANDROID__
#include <sys/stat.h>
#include <unistd.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include "engine/platform/android/AndroidGlobals.h"

namespace
{
// Extract all game assets from APK to internal storage and chdir there.
// AAssetManager scopes into the APK's assets/ folder, so paths passed
// to it must NOT have the "assets/" prefix.  But the game code loads
// via fopen("assets/..."), so the output path DOES need the prefix.
struct AssetEntry { const char* amPath; const char* fsPath; };

void extractAllAssets(const char* internalDir)
{
    const AssetEntry files[] = {
        {"beep.wav",                          "assets/beep.wav"},
        {"models/sphere.glb",                 "assets/models/sphere.glb"},
        {"models/coin.glb",                   "assets/models/coin.glb"},
        {"fonts/JetBrainsMono-msdf.json",     "assets/fonts/JetBrainsMono-msdf.json"},
        {"fonts/JetBrainsMono-msdf.png",      "assets/fonts/JetBrainsMono-msdf.png"},
        {"levels/plank.json",                 "levels/plank.json"},
        {"project.json",                      "project.json"},
    };
    int ok = 0;
    for (const auto& f : files)
    {
        // Open from APK using amPath, write to filesystem using fsPath.
        AAssetManager* am = engine::platform::getAssetManager();
        if (!am) continue;
        AAsset* asset = AAssetManager_open(am, f.amPath, AASSET_MODE_BUFFER);
        if (!asset)
        {
            __android_log_print(ANDROID_LOG_WARN, "SampleGame",
                "extractAllAssets: not found in APK: %s", f.amPath);
            continue;
        }
        size_t size = AAsset_getLength(asset);
        const void* buf = AAsset_getBuffer(asset);
        std::string outPath = std::string(internalDir) + "/" + f.fsPath;
        std::string dir = outPath.substr(0, outPath.rfind('/'));
        // Recursive mkdir
        for (size_t pos = 1; pos < dir.size(); ++pos)
        {
            if (dir[pos] == '/')
            {
                dir[pos] = '\0';
                mkdir(dir.c_str(), 0755);
                dir[pos] = '/';
            }
        }
        mkdir(dir.c_str(), 0755);
        FILE* fp = fopen(outPath.c_str(), "wb");
        if (fp) { fwrite(buf, 1, size, fp); fclose(fp); ++ok; }
        AAsset_close(asset);
    }

    chdir(internalDir);
    __android_log_print(ANDROID_LOG_INFO, "SampleGame",
        "Extracted %d/%zu assets to %s", ok, sizeof(files)/sizeof(files[0]), internalDir);
}

}  // namespace
#endif

using namespace engine::assets;
using namespace engine::core;
using namespace engine::ecs;
using namespace engine::input;
using namespace engine::physics;
using namespace engine::rendering;

namespace
{
// Compile-time perf-overlay toggle.  Set to 0 to compile out entirely
// (zero binary cost, P key has no effect).  When 1, the overlay is
// available; runtime visibility is controlled by `kPerfOverlayDefault`
// (initial state) and the P key (toggle at runtime).
#define SAMPLE_GAME_ENABLE_PERF_OVERLAY 1

// Initial visibility.  Toggle at runtime: P key (desktop) or tap the
// top-right ~120 px corner (Android).  Set false to start hidden.
constexpr bool kPerfOverlayDefault = true;

constexpr uint32_t kBackgroundColor = 0x1A1A2EFF;
constexpr float kBallRadius = 0.55f;
constexpr float kTau = 6.2831853f;
constexpr float kAndroidTopInset = 50.f;  // pixels (scaled by dpi)

#if SAMPLE_GAME_ENABLE_PERF_OVERLAY
// RAII scope timer — writes elapsed milliseconds to `out` on destruction.
// Designed to be a near-zero-cost wrapper: one steady_clock read pair.
class ScopedCpuTimer
{
public:
    explicit ScopedCpuTimer(float& out)
        : start_(std::chrono::steady_clock::now()), out_(&out) {}
    ~ScopedCpuTimer()
    {
        const auto end = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(end - start_).count();
        *out_ = static_cast<float>(ns / 1.0e6);
    }

private:
    std::chrono::steady_clock::time_point start_;
    float* out_;
};
#define SAMPLE_CPU_TIMER(member) ScopedCpuTimer _scoped_timer_##member(member)
#else
#define SAMPLE_CPU_TIMER(member) ((void)0)
#endif

static void setupUiView(bgfx::ViewId viewId, uint16_t w, uint16_t h, const char* name)
{
    bgfx::setViewName(viewId, name);
    bgfx::setViewRect(viewId, 0, 0, w, h);
    bgfx::setViewClear(viewId, BGFX_CLEAR_NONE);
    bgfx::touch(viewId);
}

std::vector<uint8_t> readFileBytes(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}
void registerCustomComponents(engine::scene::SceneSerializer& ser, uint32_t cubeMeshId)
{
    using namespace engine::ecs;
    using namespace engine::rendering;
    using namespace engine::physics;
    using namespace engine::io;

    // ---- MeshComponent (all level tiles are cubes) -------------------------
    ser.registerComponent(
        "Mesh",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            if (!reg.get<MeshComponent>(e)) return;
            w.key("Mesh"); w.startObject();
            w.key("type"); w.writeString("cube");
            w.endObject();
        },
        [cubeMeshId](EntityID e, Registry& reg, RenderResources&,
                     engine::assets::AssetManager&, JsonValue)
        {
            reg.emplace<MeshComponent>(e, MeshComponent{cubeMeshId});
        });

    // ---- MaterialComponent (inline PBR properties) -------------------------
    ser.registerComponent(
        "Material",
        [](EntityID e, const Registry& reg, const RenderResources& res, JsonWriter& w)
        {
            const auto* mc = reg.get<MaterialComponent>(e);
            if (!mc) return;
            const Material* mat = res.getMaterial(mc->material);
            if (!mat) return;
            w.key("Material"); w.startObject();
            w.key("albedo"); w.writeVec4(mat->albedo);
            w.key("roughness"); w.writeFloat(mat->roughness);
            w.key("metallic"); w.writeFloat(mat->metallic);
            w.key("emissiveScale"); w.writeFloat(mat->emissiveScale);
            w.key("transparent"); w.writeUint(mat->transparent);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources& res,
           engine::assets::AssetManager&, JsonValue val)
        {
            Material mat;
            mat.albedo = val.hasMember("albedo") ? val["albedo"].getVec4()
                                                 : engine::math::Vec4(1.0f);
            mat.roughness = val["roughness"].getFloat(0.5f);
            mat.metallic = val["metallic"].getFloat(0.0f);
            mat.emissiveScale = val["emissiveScale"].getFloat(0.0f);
            mat.transparent = static_cast<uint8_t>(val["transparent"].getUint(0));
            const uint32_t matId = res.addMaterial(mat);
            reg.emplace<MaterialComponent>(e, MaterialComponent{matId});
        });

    // ---- RigidBodyComponent ------------------------------------------------
    ser.registerComponent(
        "RigidBody",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* rb = reg.get<RigidBodyComponent>(e);
            if (!rb) return;
            w.key("RigidBody"); w.startObject();
            w.key("mass"); w.writeFloat(rb->mass);
            w.key("type"); w.writeUint(static_cast<uint32_t>(rb->type));
            w.key("friction"); w.writeFloat(rb->friction);
            w.key("restitution"); w.writeFloat(rb->restitution);
            w.key("linearDamping"); w.writeFloat(rb->linearDamping);
            w.key("angularDamping"); w.writeFloat(rb->angularDamping);
            w.key("layer"); w.writeUint(rb->layer);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            RigidBodyComponent rb;
            rb.mass = val["mass"].getFloat(1.0f);
            rb.type = static_cast<BodyType>(val["type"].getUint(0));
            rb.friction = val["friction"].getFloat(0.5f);
            rb.restitution = val["restitution"].getFloat(0.3f);
            rb.linearDamping = val["linearDamping"].getFloat(0.05f);
            rb.angularDamping = val["angularDamping"].getFloat(0.05f);
            rb.layer = static_cast<uint8_t>(val["layer"].getUint(0));
            reg.emplace<RigidBodyComponent>(e, rb);
        });

    // ---- ColliderComponent -------------------------------------------------
    ser.registerComponent(
        "Collider",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* col = reg.get<ColliderComponent>(e);
            if (!col) return;
            w.key("Collider"); w.startObject();
            w.key("shape"); w.writeUint(static_cast<uint32_t>(col->shape));
            w.key("offset"); w.writeVec3(col->offset);
            w.key("halfExtents"); w.writeVec3(col->halfExtents);
            w.key("radius"); w.writeFloat(col->radius);
            w.key("isSensor"); w.writeUint(col->isSensor);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            ColliderComponent col;
            col.shape = static_cast<ColliderShape>(val["shape"].getUint(0));
            col.offset = val.hasMember("offset") ? val["offset"].getVec3()
                                                 : engine::math::Vec3(0.0f);
            col.halfExtents = val.hasMember("halfExtents") ? val["halfExtents"].getVec3()
                                                           : engine::math::Vec3(0.5f);
            col.radius = val["radius"].getFloat(0.5f);
            col.isSensor = static_cast<uint8_t>(val["isSensor"].getUint(0));
            reg.emplace<ColliderComponent>(e, col);
        });

    // ---- VisibleTag (presence-only) ----------------------------------------
    ser.registerComponent(
        "Visible",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            if (!reg.has<VisibleTag>(e)) return;
            w.key("Visible"); w.startObject(); w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue)
        {
            reg.emplace<VisibleTag>(e);
        });

    // ---- ShadowVisibleTag --------------------------------------------------
    ser.registerComponent(
        "ShadowVisible",
        [](EntityID e, const Registry& reg, const RenderResources&, JsonWriter& w)
        {
            const auto* sv = reg.get<ShadowVisibleTag>(e);
            if (!sv) return;
            w.key("ShadowVisible"); w.startObject();
            w.key("cascadeMask"); w.writeUint(sv->cascadeMask);
            w.endObject();
        },
        [](EntityID e, Registry& reg, RenderResources&,
           engine::assets::AssetManager&, JsonValue val)
        {
            reg.emplace<ShadowVisibleTag>(e,
                ShadowVisibleTag{static_cast<uint8_t>(val["cascadeMask"].getUint(0xFF))});
        });
}

}  // namespace

SampleGame::SampleGame()
{
    assets_.registerLoader(std::make_unique<GltfLoader>());
}

SampleGame::~SampleGame() = default;

void SampleGame::onInit(Engine& engine, Registry& registry)
{
    engine_ = &engine;
    registry_ = &registry;

#ifdef __ANDROID__
    // APK assets aren't accessible via fopen. Extract everything to
    // internal storage and chdir there so all relative paths work.
    extractAllAssets("/data/data/com.pixelperfect3.samplegame/cache");
#endif

    // ---- Kick off glTF loads (async; applied once Ready) ------------------
    sphereHandle_ = assets_.load<GltfAsset>("assets/models/sphere.glb");
    coinHandle_ = assets_.load<GltfAsset>("assets/models/coin.glb");

    // ---- Audio ------------------------------------------------------------
    if (audio_.init())
    {
        const auto bytes = readFileBytes("assets/beep.wav");
        if (!bytes.empty())
            beepClipId_ = audio_.loadClip(bytes.data(), bytes.size());
        else
            std::fprintf(stderr, "SampleGame: failed to read assets/beep.wav\n");
    }

    // ---- IBL --------------------------------------------------------------
    ibl_.generateDefault();

    // ---- UI (font + renderer) -----------------------------------------------
    // On Android, extractAllAssets + chdir means these paths now resolve
    // against internal storage. On desktop, they resolve against cwd.
    if (msdfFont_.loadFromFile("assets/fonts/JetBrainsMono-msdf.json",
                               "assets/fonts/JetBrainsMono-msdf.png"))
    {
        hudFont_ = &msdfFont_;
        hudFontLoaded_ = true;
    }
    else
    {
        std::fprintf(stderr, "SampleGame: failed to load MSDF font\n");
    }
    uiRenderer_.init();

#if SAMPLE_GAME_ENABLE_PERF_OVERLAY
    perfHud_.init();
    perfHudInitialized_ = true;
    showPerfOverlay_ = kPerfOverlayDefault;

    // Sama doesn't set view names itself — give the engine views readable
    // labels so they show up correctly in the perf overlay (and in GPU
    // debuggers like RenderDoc / AGI / Instruments).
    for (uint16_t i = 0; i < engine::rendering::kMaxShadowViews; ++i)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Shadow %u", i);
        bgfx::setViewName(static_cast<bgfx::ViewId>(engine::rendering::kViewShadowBase + i),
                          buf);
    }
    bgfx::setViewName(engine::rendering::kViewDepth,       "Depth Prepass");
    bgfx::setViewName(engine::rendering::kViewOpaque,      "Opaque");
    bgfx::setViewName(engine::rendering::kViewTransparent, "Transparent");
    bgfx::setViewName(engine::rendering::kViewUi,          "UI 3D");
    bgfx::setViewName(engine::rendering::kViewImGui,       "ImGui");
#endif

    // ---- Shared cube mesh -------------------------------------------------
    MeshData cubeData = makeCubeMeshData();
    Mesh cubeMesh = buildMesh(cubeData);
    cubeMeshId_ = engine.resources().addMesh(std::move(cubeMesh));

    // ---- Materials --------------------------------------------------------
    Material greyMat;
    greyMat.albedo = {0.55f, 0.55f, 0.58f, 1.0f};
    greyMat.roughness = 0.7f;
    greyMat.metallic = 0.1f;
    greyMatId_ = engine.resources().addMaterial(greyMat);

    Material groundMat;
    groundMat.albedo = {0.22f, 0.22f, 0.25f, 1.0f};
    groundMat.roughness = 0.9f;
    groundMat.metallic = 0.0f;
    groundMatId_ = engine.resources().addMaterial(groundMat);

    Material coinMat;
    coinMat.albedo = {1.0f, 0.85f, 0.2f, 1.0f};
    coinMat.roughness = 0.35f;
    coinMat.metallic = 0.9f;
    coinMatId_ = engine.resources().addMaterial(coinMat);

    Material ballMat;
    ballMat.albedo = {0.85f, 0.15f, 0.15f, 1.0f};
    ballMat.roughness = 0.4f;
    ballMat.metallic = 0.2f;
    ballMatId_ = engine.resources().addMaterial(ballMat);

    // Placeholder mesh IDs (ball & coin) — swapped to glTF IDs once loaded.
    coinMeshId_ = cubeMeshId_;
    ballMeshId_ = cubeMeshId_;

    // ---- Physics ----------------------------------------------------------
    if (!physics_.init())
    {
        std::fprintf(stderr, "SampleGame: failed to initialize Jolt physics\n");
        return;
    }

    // ---- Generate scene files for both levels ------------------------------
    // Only generate level files if they don't already exist.
    {
        FILE* check = fopen("levels/plank.json", "r");
        if (!check)
        {
            // Plank level
            Registry tempReg;
            spawnPlankLevel(tempReg);

            engine::scene::SceneSerializer ser;
            ser.registerEngineComponents();
            registerCustomComponents(ser, cubeMeshId_);
            ser.saveScene(tempReg, engine.resources(), "levels/plank.json");
            std::fprintf(stderr, "SampleGame: saved levels/plank.json\n");
        }
        else
        {
            fclose(check);
        }
    }
    // Figure-8 is built procedurally each load (single merged mesh —
    // can't be serialized through the scene system).  No JSON cache.

    // Don't load a level yet — title screen shows first.
    // loadLevel() is called when the player clicks "Start Game".
}

void SampleGame::applyLoadedAssets(Engine& engine, Registry& registry)
{
    if (assets_.state(sphereHandle_) != AssetState::Ready ||
        assets_.state(coinHandle_) != AssetState::Ready)
    {
        return;
    }

    const GltfAsset* sphere = assets_.get<GltfAsset>(sphereHandle_);
    const GltfAsset* coin = assets_.get<GltfAsset>(coinHandle_);
    if (!sphere || sphere->meshes.empty() || !coin || coin->meshes.empty())
    {
        std::fprintf(stderr, "SampleGame: loaded asset has no mesh\n");
        assetsApplied_ = true;  // don't keep retrying
        return;
    }

    // Register the GLB's mesh+material into RenderResources.
    // The GltfAsset retains ownership of the bgfx handles (must outlive entities).
    ballMeshId_ = engine.resources().addMesh(Mesh(sphere->meshes[0]));
    if (!sphere->materials.empty())
    {
        Material sphereMat = sphere->materials[0];
        sphereMat.transparent = 1;
        sphereMat.albedo.w = 0.75f;  // ~75% opaque — coin still visible through it
        ballMatId_ = engine.resources().addMaterial(sphereMat);
    }

    coinMeshId_ = engine.resources().addMesh(Mesh(coin->meshes[0]));
    if (!coin->materials.empty())
        coinMatId_ = engine.resources().addMaterial(coin->materials[0]);

    // Swap the live ball entity over to the sphere mesh/material.
    if (auto* mc = registry.get<MeshComponent>(ballEntity_))
        mc->mesh = ballMeshId_;
    if (auto* mm = registry.get<MaterialComponent>(ballEntity_))
        mm->material = ballMatId_;

    // Swap each live coin's mesh/material.
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinEntities_[i] == 0) continue;
        if (auto* mc = registry.get<MeshComponent>(coinEntities_[i]))
            mc->mesh = coinMeshId_;
        if (auto* mm = registry.get<MaterialComponent>(coinEntities_[i]))
            mm->material = coinMatId_;
    }

    assetsApplied_ = true;
}

// ---------------------------------------------------------------------------
// Local copies of MeshBuilder.cpp's static oct-encoding helpers — used to
// build the merged figure-8 floor mesh.  These are static there, so we
// duplicate them here.  Match the shader's expected encoding exactly.
// ---------------------------------------------------------------------------
namespace
{
glm::vec2 octWrapLocal(glm::vec2 v)
{
    return (glm::vec2(1.0f) - glm::abs(glm::vec2(v.y, v.x))) *
           glm::vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}
glm::vec2 encodeOctF(glm::vec3 n)
{
    n /= (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
    return (n.z >= 0.0f) ? glm::vec2(n.x, n.y)
                         : octWrapLocal(glm::vec2(n.x, n.y));
}
glm::i16vec2 encodeOct16(glm::vec3 n)
{
    const glm::vec2 oct = encodeOctF(n);
    return glm::i16vec2(static_cast<int16_t>(std::round(oct.x * 32767.0f)),
                        static_cast<int16_t>(std::round(oct.y * 32767.0f)));
}
glm::u8vec4 encodeTan8(glm::vec3 t, float sign)
{
    const glm::vec2 oct = encodeOctF(t);
    const auto u8 = [](float v) -> uint8_t
    { return static_cast<uint8_t>(std::round((v * 0.5f + 0.5f) * 255.0f)); };
    return glm::u8vec4(u8(oct.x), u8(oct.y), 128u, u8(sign));
}
}  // namespace

void SampleGame::spawnFigureEightFloor(Registry& registry, uint32_t /*meshId*/,
                                       uint32_t matId)
{
    // ------------------------------------------------------------------
    // Procedural figure-8 floor — single merged mesh.
    //
    // Previously this spawned 384 individual cube entities (2 lobes × 64
    // angular × 3 radial), each with its own draw call + kinematic body.
    // Now the geometry is baked into ONE mesh (one draw call for opaque
    // and one for shadow), and physics uses a single safety floor box.
    // ------------------------------------------------------------------
    constexpr int kAngular = 64;
    constexpr int kRadial = 3;
    constexpr float kROuter = 4.0f;
    constexpr float kRInner = 2.5f;
    constexpr float kCx = 3.5f;
    constexpr float kThick = 1.0f;
    const float dR = (kROuter - kRInner) / static_cast<float>(kRadial);
    const float dTheta = kTau / static_cast<float>(kAngular);
    const float halfThick = kThick * 0.5f;
    const glm::vec3 centers[2] = {{-kCx, 0.0f, 0.0f}, {kCx, 0.0f, 0.0f}};

    // Reference faces of a unit cube (matches MeshBuilder::makeCubeMeshData).
    struct Face
    {
        glm::vec3 normal;
        glm::vec3 tangent;
        glm::vec3 corners[4];
        glm::vec2 uvs[4];
    };
    static const Face faces[6] = {
        {{ 1, 0, 0}, { 0, 0,-1},
         {{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
        {{-1, 0, 0}, { 0, 0, 1},
         {{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
        {{ 0, 1, 0}, { 1, 0, 0},
         {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
        {{ 0,-1, 0}, { 1, 0, 0},
         {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
        {{ 0, 0, 1}, { 1, 0, 0},
         {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
        {{ 0, 0,-1}, {-1, 0, 0},
         {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}},
         {{0,0},{1,0},{1,1},{0,1}}},
    };

    constexpr int kWedges = 2 * kAngular * kRadial;          // 384
    constexpr int kVertsPerWedge = 24;                       // 6 faces × 4
    constexpr int kIdxPerWedge = 36;                         // 6 faces × 6
    MeshData md;
    md.positions.reserve(kWedges * kVertsPerWedge * 3);
    md.normals.reserve(kWedges * kVertsPerWedge * 2);
    md.tangents.reserve(kWedges * kVertsPerWedge * 4);
    md.uvs.reserve(kWedges * kVertsPerWedge * 2);
    md.indices.reserve(kWedges * kIdxPerWedge);

    glm::vec3 boundsMin{ FLT_MAX,  FLT_MAX,  FLT_MAX};
    glm::vec3 boundsMax{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (int circle = 0; circle < 2; ++circle)
    {
        const glm::vec3 center = centers[circle];
        for (int a = 0; a < kAngular; ++a)
        {
            const float theta = dTheta * (static_cast<float>(a) + 0.5f);
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            const glm::quat rot = glm::angleAxis(-theta, glm::vec3(0.0f, 1.0f, 0.0f));
            for (int r = 0; r < kRadial; ++r)
            {
                const float rMid = kRInner + dR * (static_cast<float>(r) + 0.5f);
                const float arcLen = dTheta * rMid;
                const glm::vec3 pos{center.x + rMid * cosT,
                                    -halfThick,
                                    center.z + rMid * sinT};
                // 2% overlap → no visible seam, no crack to fall through.
                const glm::vec3 scl{dR * 1.02f, kThick, arcLen * 1.02f};

                const uint16_t baseVtx =
                    static_cast<uint16_t>(md.positions.size() / 3);

                for (int fi = 0; fi < 6; ++fi)
                {
                    const Face& f = faces[fi];
                    // Rotation-only on normals/tangents (uniform rotation,
                    // non-uniform scale → upper-3x3 inverse-transpose simplifies
                    // to just rotating because we apply Y-axis rotation only).
                    const glm::vec3 n = rot * f.normal;
                    const glm::vec3 t = rot * f.tangent;
                    const glm::i16vec2 encN = encodeOct16(n);
                    const glm::u8vec4 encT = encodeTan8(t, 1.0f);

                    for (int ci = 0; ci < 4; ++ci)
                    {
                        // Corner = pos + rot * (scl ⊙ unit_corner)
                        const glm::vec3 local{f.corners[ci].x * scl.x,
                                              f.corners[ci].y * scl.y,
                                              f.corners[ci].z * scl.z};
                        const glm::vec3 world = pos + rot * local;
                        md.positions.push_back(world.x);
                        md.positions.push_back(world.y);
                        md.positions.push_back(world.z);
                        boundsMin = glm::min(boundsMin, world);
                        boundsMax = glm::max(boundsMax, world);

                        md.normals.push_back(encN.x);
                        md.normals.push_back(encN.y);

                        md.tangents.push_back(encT.x);
                        md.tangents.push_back(encT.y);
                        md.tangents.push_back(encT.z);
                        md.tangents.push_back(encT.w);

                        const uint32_t packed = glm::packHalf2x16(f.uvs[ci]);
                        md.uvs.push_back(static_cast<uint16_t>(packed & 0xFFFFu));
                        md.uvs.push_back(static_cast<uint16_t>((packed >> 16) & 0xFFFFu));
                    }
                    const uint16_t b = static_cast<uint16_t>(baseVtx + fi * 4);
                    md.indices.push_back(b + 0);
                    md.indices.push_back(b + 1);
                    md.indices.push_back(b + 2);
                    md.indices.push_back(b + 0);
                    md.indices.push_back(b + 2);
                    md.indices.push_back(b + 3);
                }
            }
        }
    }
    md.boundsMin = boundsMin;
    md.boundsMax = boundsMax;

    const Mesh floorMesh = buildMesh(md);
    if (!floorMesh.isValid())
    {
        std::fprintf(stderr, "spawnFigureEightFloor: buildMesh failed\n");
        return;
    }
    const uint32_t floorMeshId = engine_->resources().addMesh(Mesh(floorMesh));

    // Single visible entity at origin (mesh is already in world space).
    EntityID floor = registry.createEntity();
    TransformComponent tc{};
    tc.position = {0, 0, 0};
    tc.rotation = glm::quat(1, 0, 0, 0);
    tc.scale = {1, 1, 1};
    tc.flags = 1;
    registry.emplace<TransformComponent>(floor, tc);
    registry.emplace<WorldTransformComponent>(floor);
    registry.emplace<MeshComponent>(floor, MeshComponent{floorMeshId});
    registry.emplace<MaterialComponent>(floor, MaterialComponent{matId});
    registry.emplace<VisibleTag>(floor);
    registry.emplace<ShadowVisibleTag>(floor, ShadowVisibleTag{0xFF});

    // Physics: one collider per angular segment (kRadial=1), Static type.
    // Going from 384 (Kinematic) → 128 (Static) eliminates 384 per-fixed-step
    // moveKinematic calls AND lets Jolt place bodies in the static broadphase
    // tree, which has cheaper queries than the dynamic/kinematic tree.
    // kAngular=64 tangential steps still give the ball a smooth roll.
    constexpr int kPhysRadial = 1;
    const float dRPhys = (kROuter - kRInner) / static_cast<float>(kPhysRadial);
    for (int circle = 0; circle < 2; ++circle)
    {
        const glm::vec3 center = centers[circle];
        for (int a = 0; a < kAngular; ++a)
        {
            const float theta = dTheta * (static_cast<float>(a) + 0.5f);
            const float cosT = std::cos(theta);
            const float sinT = std::sin(theta);
            for (int r = 0; r < kPhysRadial; ++r)
            {
                const float rMid = kRInner + dRPhys * (static_cast<float>(r) + 0.5f);
                const float arcLen = dTheta * rMid;

                EntityID col = registry.createEntity();
                TransformComponent t2{};
                t2.position = {center.x + rMid * cosT, -halfThick,
                               center.z + rMid * sinT};
                t2.rotation = glm::angleAxis(-theta, glm::vec3(0.0f, 1.0f, 0.0f));
                t2.scale = {1, 1, 1};
                t2.flags = 1;
                registry.emplace<TransformComponent>(col, t2);
                registry.emplace<WorldTransformComponent>(col);

                RigidBodyComponent rb;
                rb.mass = 0.0f;
                rb.type = BodyType::Static;
                rb.friction = 0.8f;
                rb.restitution = 0.1f;
                registry.emplace<RigidBodyComponent>(col, rb);

                ColliderComponent cc;
                cc.shape = ColliderShape::Box;
                cc.halfExtents = {dRPhys * 0.51f, halfThick, arcLen * 0.51f};
                registry.emplace<ColliderComponent>(col, cc);
            }
        }
    }
}

void SampleGame::spawnPlankLevel(Registry& registry)
{
    // Plank only — no large ground plane
    {
        EntityID plank = registry.createEntity();
        TransformComponent tc{};
        tc.position = {0.0f, 2.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {6.0f, 0.15f, 1.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(plank, tc);
        registry.emplace<WorldTransformComponent>(plank);
        registry.emplace<MeshComponent>(plank, MeshComponent{cubeMeshId_});
        registry.emplace<MaterialComponent>(plank, MaterialComponent{greyMatId_});
        registry.emplace<VisibleTag>(plank);
        registry.emplace<ShadowVisibleTag>(plank, ShadowVisibleTag{0xFF});

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.8f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(plank, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {3.0f, 0.075f, 0.5f};
        registry.emplace<ColliderComponent>(plank, col);
    }
}

void SampleGame::spawnBall(Registry& registry)
{
    ballEntity_ = registry.createEntity();
    const float r = kBallRadius;
    TransformComponent tc{};
    tc.position = {ballStartPos_.x, ballStartPos_.y, ballStartPos_.z};
    tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    tc.scale = {r * 2.0f, r * 2.0f, r * 2.0f};
    tc.flags = 1;
    registry.emplace<TransformComponent>(ballEntity_, tc);
    registry.emplace<WorldTransformComponent>(ballEntity_);
    registry.emplace<MeshComponent>(ballEntity_, MeshComponent{ballMeshId_});
    registry.emplace<MaterialComponent>(ballEntity_, MaterialComponent{ballMatId_});
    registry.emplace<VisibleTag>(ballEntity_);
    registry.emplace<ShadowVisibleTag>(ballEntity_, ShadowVisibleTag{0xFF});

    RigidBodyComponent rb;
    rb.mass = 15.0f;
    rb.type = BodyType::Dynamic;
    rb.friction = 0.4f;
    rb.restitution = 0.3f;
    rb.linearDamping = 0.05f;
    rb.angularDamping = 0.05f;
    registry.emplace<RigidBodyComponent>(ballEntity_, rb);

    ColliderComponent col;
    col.shape = ColliderShape::Sphere;
    col.radius = r;
    registry.emplace<ColliderComponent>(ballEntity_, col);
}

void SampleGame::clearLevel(Registry& registry)
{
    // Destroy every entity in the registry — nothing persists across levels.
    std::vector<EntityID> all;
    registry.forEachEntity([&](EntityID e) { all.push_back(e); });
    for (auto e : all)
        registry.destroyEntity(e);

    // Reset handles.
    coinEntities_.fill(0);
    coinCollectedFlags_.fill(false);
    coinsRemaining_ = 0;
    ballEntity_ = 0;
    groundEntity_ = 0;
    levelEntities_.clear();
}

void SampleGame::loadLevel(Engine& engine, Registry& registry, int level)
{
    clearLevel(registry);
    currentLevel_ = level;
    gyroCalibrated_ = false;  // recalibrate tilt base on next frame

    if (level == 0)
    {
        // Plank level
        ballStartPos_ = {-2.4f, 2.7f, 0.0f};
        coinCount_ = 1;

        engine::scene::SceneSerializer ser;
        ser.registerEngineComponents();
        registerCustomComponents(ser, cubeMeshId_);
        ser.loadScene("levels/plank.json", registry,
                      engine.resources(), assets_);
    }
    else if (level == 1)
    {
        // Figure-8 level — built procedurally as a single merged mesh
        // (one draw call per pass).  We can't serialize a runtime-built
        // mesh, so this skips the JSON cache path entirely.
        ballStartPos_ = {-7.0f, kBallRadius, 0.0f};
        coinCount_ = 3;

        spawnFigureEightFloor(registry, cubeMeshId_, greyMatId_);

        // Invisible safety floor at y=-20
        groundEntity_ = registry.createEntity();
        TransformComponent tc{};
        tc.position = {0.0f, -20.0f, 0.0f};
        tc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        tc.scale = {100.0f, 1.0f, 100.0f};
        tc.flags = 1;
        registry.emplace<TransformComponent>(groundEntity_, tc);
        registry.emplace<WorldTransformComponent>(groundEntity_);

        RigidBodyComponent rb;
        rb.mass = 0.0f;
        rb.type = BodyType::Kinematic;
        rb.friction = 0.5f;
        rb.restitution = 0.1f;
        registry.emplace<RigidBodyComponent>(groundEntity_, rb);

        ColliderComponent col;
        col.shape = ColliderShape::Box;
        col.halfExtents = {50.0f, 0.5f, 50.0f};
        registry.emplace<ColliderComponent>(groundEntity_, col);
    }

    spawnBall(registry);
    spawnAllCoins(registry);

    engine::scene::TransformSystem transformSys;
    transformSys.update(registry);
}

void SampleGame::spawnAllCoins(Registry& registry)
{
    for (int i = 0; i < coinCount_; ++i)
        spawnCoin(registry, i);
    coinsRemaining_ = coinCount_;
    coinCollectedFlags_.fill(false);

    // Snap smoothed camera target to the nearest coin — no swing on reset.
    const glm::vec3 ballStart = ballStartPos_;
    int nearest = 0;
    float bestSq = 1e30f;
    for (int i = 0; i < coinCount_; ++i)
    {
        const glm::vec3 d = coinPositions_[i] - ballStart;
        const float sq = glm::dot(d, d);
        if (sq < bestSq) { bestSq = sq; nearest = i; }
    }
    smoothedTargetPos_ = coinPositions_[nearest];
    glm::vec2 fwd{smoothedTargetPos_.x - ballStart.x, smoothedTargetPos_.z - ballStart.z};
    const float l = glm::length(fwd);
    smoothedFwd_ = (l > 1e-4f) ? (fwd / l) : glm::vec2(0.0f, -1.0f);
}

void SampleGame::spawnCoin(Registry& registry, int index)
{
    const float r = 0.2f;

    if (currentLevel_ == 0)
    {
        // Plank level: single coin at fixed position
        coinPositions_[index] = {2.6f, 2.35f, 0.0f};
    }
    else
    {
        // Figure-8 level: random ring placement
        constexpr float kROuter = 4.0f;
        constexpr float kRInner = 2.5f;
        constexpr float kCx = 3.5f;
        std::uniform_real_distribution<float> angleDist(0.0f, kTau);
        std::uniform_real_distribution<float> radiusDist(
            kRInner + 0.4f, kROuter - 0.4f);
        std::uniform_int_distribution<int> ringDist(0, 1);
        const float theta = angleDist(rng_);
        const float radius = radiusDist(rng_);
        const int ring = (index == 0) ? 1 : ringDist(rng_);
        const float cx = (ring == 0) ? -kCx : kCx;
        coinPositions_[index] = {cx + radius * std::cos(theta), kBallRadius,
                                 radius * std::sin(theta)};
    }

    const EntityID coin = registry.createEntity();
    coinEntities_[index] = coin;

    TransformComponent tc{};
    tc.position = {coinPositions_[index].x, coinPositions_[index].y,
                   coinPositions_[index].z};
    // Stand the coin upright, then spin it 90° around Y.
    tc.rotation =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    tc.scale = {r * 2.0f, r * 2.0f, r * 2.0f};
    tc.flags = 1;
    registry.emplace<TransformComponent>(coin, tc);
    registry.emplace<WorldTransformComponent>(coin);
    registry.emplace<MeshComponent>(coin, MeshComponent{coinMeshId_});
    registry.emplace<MaterialComponent>(coin, MaterialComponent{coinMatId_});
    registry.emplace<VisibleTag>(coin);
    registry.emplace<ShadowVisibleTag>(coin, ShadowVisibleTag{0xFF});

    RigidBodyComponent rb;
    rb.mass = 0.0f;
    rb.type = BodyType::Kinematic;
    rb.friction = 0.5f;
    rb.restitution = 0.3f;
    registry.emplace<RigidBodyComponent>(coin, rb);

    ColliderComponent col;
    col.shape = ColliderShape::Sphere;
    col.radius = r;
    col.isSensor = 1;  // overlap only — ball passes through, no impulse
    registry.emplace<ColliderComponent>(coin, col);
}

void SampleGame::onFixedUpdate(Engine& engine, Registry& registry, float fixedDt)
{
    if (showTitleScreen_) return;
    SAMPLE_CPU_TIMER(cpuMsOnFixedUpdate_);

    // Apply directional force to the ball while movement keys are held.
    // Force is persistent per fixed step, so holding a direction accelerates.
    const auto& input = engine.inputState();
    glm::vec3 force{0.0f};
    constexpr float kForceMag = 45.0f;  // N; mass=15 → 3.0 m/s² of acceleration
    // Movement is camera-relative. smoothedFwd_ is the XZ unit vector from
    // ball toward the current look-target. In a right-handed world with
    // Y up, the camera's "right" = cross(forward, up), which in XZ is
    // (-fwd.z, fwd.x). Using (fwd.x, fwd.y) as XZ:
    const glm::vec2 fwd = smoothedFwd_;
    const glm::vec2 right{-fwd.y, fwd.x};
    float axisF = 0.0f, axisR = 0.0f;
    if (input.isKeyHeld(Key::Up) || input.isKeyHeld(Key::W))    axisF += 1.0f;
    if (input.isKeyHeld(Key::Down) || input.isKeyHeld(Key::S))  axisF -= 1.0f;
    if (input.isKeyHeld(Key::Right) || input.isKeyHeld(Key::D)) axisR += 1.0f;
    if (input.isKeyHeld(Key::Left) || input.isKeyHeld(Key::A))  axisR -= 1.0f;

    // Gyroscope/accelerometer tilt input (primarily for Android).
    // Sama's AndroidGyro already normalizes gravity by 9.8, so
    // gravityX/Y/Z are in [-1, 1] (1.0 = 90° tilt).
    //
    // Device coords (portrait): X = right, Y = up, Z = out of screen.
    // Landscape 90° CCW: screen-forward = device -Y, screen-right = device +X.
    const auto& gyro = input.gyro();
    if (gyro.available)
    {
        if (!gyroCalibrated_)
        {
            gyroBaseX_ = gyro.gravityX;
            gyroBaseY_ = gyro.gravityY;
            gyroCalibrated_ = true;
        }

        float deltaX = gyro.gravityX - gyroBaseX_;
        float deltaY = gyro.gravityY - gyroBaseY_;

        // When the phone is flat, gravity is mostly along Z and the XY
        // deltas for a given physical tilt become very small.  Adaptive
        // gain scales them up so the controls feel the same regardless
        // of whether you're sitting up or lying in bed.
        float xyMag = std::sqrt(gyroBaseX_ * gyroBaseX_
                              + gyroBaseY_ * gyroBaseY_);
        float gain = 1.0f / std::max(xyMag, 0.2f);
        gain = std::min(gain, 5.0f);

        deltaX *= gain;
        deltaY *= gain;

        constexpr float kSensitivity = 2.5f;
        constexpr float kDeadzone = 0.04f;

        if (std::abs(deltaX) < kDeadzone) deltaX = 0.0f;
        if (std::abs(deltaY) < kDeadzone) deltaY = 0.0f;

        // Landscape (90° CCW): forward = -deltaX, right = +deltaY.
        axisF += std::clamp(-deltaX * kSensitivity, -1.0f, 1.0f);
        axisR += std::clamp(deltaY * kSensitivity, -1.0f, 1.0f);
        axisF = std::clamp(axisF, -1.0f, 1.0f);
        axisR = std::clamp(axisR, -1.0f, 1.0f);
    }

    force.x = (axisF * fwd.x + axisR * right.x) * kForceMag;
    force.z = (axisF * fwd.y + axisR * right.y) * kForceMag;

    if (force.x != 0.0f || force.z != 0.0f)
    {
        if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_); rb && rb->bodyID != ~0u)
            physics_.applyForce(rb->bodyID, {force.x, 0.0f, force.z});
    }

    {
        SAMPLE_CPU_TIMER(cpuMsPhysics_);
        physicsSys_.update(registry, physics_, fixedDt);
    }

    // If the ball falls off the figure-8:
    //   - Mid-game: reset the level.
    //   - All coins collected: let the ball fall freely, freeze physics at y=-20.
    if (auto* tc = registry.get<TransformComponent>(ballEntity_))
    {
        if (tc->position.y < -10.0f && coinsRemaining_ > 0)
        {
            resetLevel(registry);
            return;
        }
        if (tc->position.y < -20.0f && coinsRemaining_ == 0)
        {
            if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_);
                rb && rb->bodyID != ~0u)
            {
                physics_.setLinearVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
                physics_.setAngularVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
            }
            return;
        }
    }

    // Detect ball-vs-coin contacts — play the beep and remove any hit coin.
    for (const auto& evt : physics_.getContactBeginEvents())
    {
        EntityID other = 0;
        if (evt.entityA == ballEntity_)      other = evt.entityB;
        else if (evt.entityB == ballEntity_) other = evt.entityA;
        else continue;

        for (int i = 0; i < coinCount_; ++i)
        {
            if (coinCollectedFlags_[i] || coinEntities_[i] != other)
                continue;

            if (beepClipId_ != 0)
                audio_.play(beepClipId_, engine::audio::SoundCategory::SFX, 1.0f, false);

            registry.destroyEntity(coinEntities_[i]);
            coinEntities_[i] = 0;
            coinCollectedFlags_[i] = true;
            --coinsRemaining_;
            break;
        }
    }
}

void SampleGame::onUpdate(Engine& engine, Registry& registry, float dt)
{
    SAMPLE_CPU_TIMER(cpuMsOnUpdate_);
#if SAMPLE_GAME_ENABLE_PERF_OVERLAY
    // P toggles the perf overlay (desktop).
    if (engine.inputState().isKeyPressed(Key::P))
        showPerfOverlay_ = !showPerfOverlay_;

    // Tap top-right ~120 px corner toggles on Android (and via mouse).
    {
        const auto& in = engine.inputState();
        if (in.isMouseButtonPressed(engine::input::MouseButton::Left))
        {
#ifdef __ANDROID__
            const float mx = static_cast<float>(in.mouseX());
            const float my = static_cast<float>(in.mouseY());
#else
            const float mx = static_cast<float>(in.mouseX()) * engine.contentScaleX();
            const float my = static_cast<float>(in.mouseY()) * engine.contentScaleY();
#endif
            const float W = static_cast<float>(engine.fbWidth());
            const float corner = 120.0f * (static_cast<float>(engine.fbHeight()) / 1080.0f);
            if (mx > W - corner && my < corner)
                showPerfOverlay_ = !showPerfOverlay_;
        }
    }

    // Smoothed FPS for stable display: low-pass dt → fps.
    if (dt > 0.0f)
    {
        const float instFps = 1.0f / dt;
        const float k = 1.0f - std::exp(-3.0f * dt);
        fpsSmoothed_ = fpsSmoothed_ + (instFps - fpsSmoothed_) * k;
    }
#endif

    if (showTitleScreen_) return;

    // Drain async asset uploads; apply GLB meshes once Ready.
    assets_.processUploads();
    if (!assetsApplied_)
        applyLoadedAssets(engine, registry);

    // Pick the nearest uncollected coin and smoothly track it.
    glm::vec3 ballPos = ballStartPos_;
    if (auto* tc = registry.get<TransformComponent>(ballEntity_))
        ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);

    if (coinsRemaining_ > 0)
    {
        int nearest = -1;
        float bestSq = 1e30f;
        for (int i = 0; i < coinCount_; ++i)
        {
            if (coinCollectedFlags_[i]) continue;
            const glm::vec3 d = coinPositions_[i] - ballPos;
            const float sq = glm::dot(d, d);
            if (sq < bestSq) { bestSq = sq; nearest = i; }
        }
        if (nearest >= 0)
        {
            const glm::vec3 target = coinPositions_[nearest];
            const float kTarget = 1.0f - std::exp(-3.0f * dt);
            smoothedTargetPos_ = glm::mix(smoothedTargetPos_, target, kTarget);
        }
    }
    // Recompute smoothedFwd_ from the ball→target XZ direction.
    {
        glm::vec2 want{smoothedTargetPos_.x - ballPos.x,
                       smoothedTargetPos_.z - ballPos.z};
        const float l = glm::length(want);
        if (l > 1e-3f)
        {
            want /= l;
            const float kDir = 1.0f - std::exp(-4.0f * dt);
            smoothedFwd_ = glm::normalize(glm::mix(smoothedFwd_, want, kDir));
        }
    }

    // Spin all remaining coins around Y at 180°/s.
    coinSpinTime_ += dt;
    const float spinDeg = coinSpinTime_ * 180.0f;
    const glm::quat spinRot =
        glm::angleAxis(glm::radians(spinDeg), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinCollectedFlags_[i]) continue;
        if (auto* tc = registry.get<TransformComponent>(coinEntities_[i]))
        {
            tc->rotation = spinRot;
            tc->flags |= 1;
        }
    }

    const auto& input = engine.inputState();

    // R key resets everything.
    if (input.isKeyPressed(Key::R))
        resetLevel(registry);
}

void SampleGame::resetLevel(Registry& registry)
{
    gyroCalibrated_ = false;  // recalibrate tilt base on next frame
    if (auto* rb = registry.get<RigidBodyComponent>(ballEntity_); rb && rb->bodyID != ~0u)
    {
        physics_.setBodyPosition(rb->bodyID,
            {ballStartPos_.x, ballStartPos_.y, ballStartPos_.z});
        physics_.setBodyRotation(rb->bodyID, {1.0f, 0.0f, 0.0f, 0.0f});
        physics_.setLinearVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
        physics_.setAngularVelocity(rb->bodyID, {0.0f, 0.0f, 0.0f});
    }
    for (int i = 0; i < coinCount_; ++i)
    {
        if (coinEntities_[i] != 0)
            registry.destroyEntity(coinEntities_[i]);
        coinEntities_[i] = 0;
    }
    spawnAllCoins(registry);
}

void SampleGame::onRender(Engine& engine)
{
    SAMPLE_CPU_TIMER(cpuMsOnRender_);
    engine.renderer().beginFrameDirect();

    const auto W = engine.fbWidth();
    const auto H = engine.fbHeight();
    const float fbW = static_cast<float>(W);
    const float fbH = static_cast<float>(H);

    // Handle deferred return-to-title (set by "Back to Home Screen" button).
    // Must happen before any canvas access to avoid use-after-free.
    if (returnToTitlePending_)
    {
        returnToTitlePending_ = false;
        if (registry_)
            clearLevel(*registry_);
        showTitleScreen_ = true;
        endLevelCanvasBuilt_ = false;
        endLevelCanvas_.reset();
        coinsRemaining_ = 1;  // prevent stale "level complete" state

        // Clear all views so no stale 3D scene or HUD bleeds through.
        RenderPass(kViewOpaque).rect(0, 0, W, H).clearColorAndDepth(kBackgroundColor);
        RenderPass(kViewTransparent).rect(0, 0, W, H).clearColorAndDepth(0x00000000);
        bgfx::setViewRect(engine::rendering::kViewGameUi, 0, 0, W, H);
        bgfx::setViewClear(engine::rendering::kViewGameUi,
                           BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
        bgfx::touch(engine::rendering::kViewGameUi);
    }

    // Ensure canvases exist and are sized to the current framebuffer.
    if (!titleCanvas_ || canvasW_ != W || canvasH_ != H)
    {
        canvasW_ = W;
        canvasH_ = H;
        // Scale UI relative to screen height so layouts fit on both desktop
        // (2160 physical px) and phone (1080 physical px landscape).
        canvasDpi_ = static_cast<float>(H) / 1080.f;
        if (!titleCanvas_)
            titleCanvas_ = std::make_unique<engine::ui::UiCanvas>(W, H);
        else
            titleCanvas_->setScreenSize(W, H);
        buildTitleCanvas();

        if (endLevelCanvas_)
            endLevelCanvas_->setScreenSize(W, H);
    }

    // ---- Title screen -----------------------------------------------------
    if (showTitleScreen_)
    {
        // Just clear the screen.
        RenderPass(kViewOpaque)
            .rect(0, 0, W, H)
            .clearColorAndDepth(kBackgroundColor);

        if (hudFontLoaded_ && titleCanvas_)
        {
            dispatchMouseEvents(engine, *titleCanvas_);
            titleCanvas_->update();

            const bgfx::ViewId uiView = engine::rendering::kViewGameUi;
            setupUiView(uiView, W, H, "TitleUI");
            uiRenderer_.render(titleCanvas_->drawList(), uiView, W, H);
        }
        return;  // skip 3D rendering
    }

    // Chase camera: behind the ball along -smoothedFwd_, looking toward
    // the smoothed target (nearest uncollected coin).
    glm::vec3 ballPos = ballStartPos_;
    if (registry_)
    {
        if (auto* tc = registry_->get<TransformComponent>(ballEntity_))
            ballPos = glm::vec3(tc->position.x, tc->position.y, tc->position.z);
    }
    const glm::vec3 fwd3{smoothedFwd_.x, 0.0f, smoothedFwd_.y};
    const glm::vec3 camPos = ballPos - fwd3 * 6.0f + glm::vec3(0.0f, 3.5f, 0.0f);
    const glm::mat4 viewMat = glm::lookAt(camPos, smoothedTargetPos_, glm::vec3(0, 1, 0));
    const glm::mat4 projMat = glm::perspective(glm::radians(45.f), fbW / fbH, 0.05f, 100.f);

    const glm::vec3 kLightDir = glm::normalize(glm::vec3(1.0f, 2.0f, 1.0f));
    constexpr float kLightIntens = 6.0f;
    const float lightData[8] = {
        kLightDir.x,         kLightDir.y,          kLightDir.z,          0.f,
        1.0f * kLightIntens, 0.95f * kLightIntens, 0.85f * kLightIntens, 0.f};

    const glm::vec3 kLightPos = kLightDir * 20.f;
    const glm::mat4 lightView = glm::lookAt(kLightPos, glm::vec3(0.f), glm::vec3(0, 1, 0));
    const glm::mat4 lightProj = glm::ortho(-12.f, 12.f, -12.f, 12.f, 0.1f, 50.f);

    engine.shadow().beginCascade(0, lightView, lightProj);

    if (!registry_)
        return;

    {
        SAMPLE_CPU_TIMER(cpuMsShadowSubmit_);
        drawCallSys_.submitShadowDrawCalls(*registry_, engine.resources(),
                                           engine.shadowProgram(), 0);
    }

    RenderPass(kViewOpaque)
        .rect(0, 0, W, H)
        .clearColorAndDepth(kBackgroundColor)
        .transform(viewMat, projMat);

    // Transparent pass shares the view/proj; no clear (blends onto opaque).
    // Explicitly reset clear flags — they persist across frames in bgfx, and
    // the returnToTitlePending_ path sets clearColorAndDepth on this view.
    bgfx::setViewClear(kViewTransparent, BGFX_CLEAR_NONE);
    RenderPass(kViewTransparent)
        .rect(0, 0, W, H)
        .transform(viewMat, projMat);

    const glm::mat4 shadowMat = engine.shadow().shadowMatrix(0);
    PbrFrameParams frame{
        lightData, glm::value_ptr(shadowMat), engine.shadow().atlasTexture(), W, H, 0.05f, 100.f};
    frame.camPos[0] = camPos.x;
    frame.camPos[1] = camPos.y;
    frame.camPos[2] = camPos.z;

    if (ibl_.isValid())
    {
        frame.iblEnabled = true;
        frame.maxMipLevels = 7.0f;
        frame.irradiance = ibl_.irradiance();
        frame.prefiltered = ibl_.prefiltered();
        frame.brdfLut = ibl_.brdfLut();
    }

    {
        SAMPLE_CPU_TIMER(cpuMsDrawCallUpdate_);
        drawCallSys_.update(*registry_, engine.resources(), engine.pbrProgram(),
                            engine.uniforms(), frame);
    }

    // ---- HUD --------------------------------------------------------------
    const int collected = coinCount_ - coinsRemaining_;
    const bool levelComplete = (coinsRemaining_ == 0);
    const bool hasNextLevel = (currentLevel_ < kLevelCount - 1);

    // Per-frame coin counter via UiDrawList (MSDF).
    const float dpi = static_cast<float>(H) / 1080.f;

    // Top inset to clear the Android status bar / display cutout.
#ifdef __ANDROID__
    const float topInset = kAndroidTopInset * dpi;
#else
    const float topInset = 0.f;
#endif
    if (hudFontLoaded_ && !levelComplete)
    {
        hudDrawList_.clear();

        const engine::math::Vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
        const engine::math::Vec2 hudPos{40.0f * dpi, 32.0f * dpi + topInset};
        const float hudSize = 48.0f * dpi;

        char buf[64];
        std::snprintf(buf, sizeof(buf), "Coins Collected: %d/%d", collected, coinCount_);
        hudDrawList_.drawText(hudPos, buf, white, hudFont_, hudSize);

        // Reset button — top-right corner
        const float btnW = 120.f * dpi;
        const float btnH = 50.f * dpi;
        const float btnX = static_cast<float>(W) - btnW - 30.f * dpi;
        const float btnY = 30.f * dpi + topInset;
        const engine::math::Vec4 btnColor{0.25f, 0.25f, 0.38f, 0.85f};
        hudDrawList_.drawRect({btnX, btnY}, {btnW, btnH}, btnColor, 10.f * dpi);
        const float lblSize = 28.f * dpi;
        const float lblX = btnX + (btnW - lblSize * 3.2f) * 0.5f;  // ~3.2 chars wide
        const float lblY = btnY + (btnH - lblSize) * 0.5f;
        hudDrawList_.drawText({lblX, lblY}, "Reset", white, hudFont_, lblSize);

        // Hit-test: if mouse/touch pressed inside the button rect, reset.
        {
            const auto& input = engine.inputState();
#ifdef __ANDROID__
            const float mx = static_cast<float>(input.mouseX());
            const float my = static_cast<float>(input.mouseY());
#else
            const float mx = static_cast<float>(input.mouseX()) * engine.contentScaleX();
            const float my = static_cast<float>(input.mouseY()) * engine.contentScaleY();
#endif
            if (input.isMouseButtonPressed(engine::input::MouseButton::Left) &&
                mx >= btnX && mx <= btnX + btnW &&
                my >= btnY && my <= btnY + btnH)
            {
                if (registry_)
                    resetLevel(*registry_);
            }
        }

        const bgfx::ViewId hudView = engine::rendering::kViewGameUi;
        setupUiView(hudView, W, H, "HUD");
        uiRenderer_.render(hudDrawList_, hudView, W, H);
    }

    // Level-complete / you-win canvas: built lazily when level completes,
    // discarded when a new level starts.
    if (hudFontLoaded_ && levelComplete)
    {
        if (!endLevelCanvasBuilt_ || endLevelCanvasHasNext_ != hasNextLevel)
        {
            buildEndLevelCanvas(hasNextLevel);
        }

        if (endLevelCanvas_)
        {
            dispatchMouseEvents(engine, *endLevelCanvas_);
            endLevelCanvas_->update();

            const bgfx::ViewId uiView = engine::rendering::kViewGameUi;
            setupUiView(uiView, W, H, "HUD");
            uiRenderer_.render(endLevelCanvas_->drawList(), uiView, W, H);
        }
    }
    else
    {
        // Level not complete -> ensure the end-level canvas is rebuilt next
        // time we finish a level.
        endLevelCanvasBuilt_ = false;
    }

    // Perf overlay last — sits on top of everything.
    renderPerfOverlay(engine, 0.0f);
}

// ---------------------------------------------------------------------------
// UI construction (retained-mode UiCanvas widgets)
// ---------------------------------------------------------------------------

void SampleGame::buildTitleCanvas()
{
    using namespace engine::ui;
    if (!titleCanvas_) return;

    // Rebuild from scratch.
    titleCanvas_ = std::make_unique<UiCanvas>(canvasW_, canvasH_);

    const float s = canvasDpi_;  // DPI scale (2 on retina, 1 elsewhere)

    auto* root = titleCanvas_->root();

    // Full-screen background panel.
    auto* bg = titleCanvas_->createNode<UiPanel>("titleBg");
    bg->anchor = {{0.f, 0.f}, {1.f, 1.f}};
    bg->offsetMin = {0.f, 0.f};
    bg->offsetMax = {0.f, 0.f};
    bg->color = {0.10f, 0.10f, 0.18f, 0.0f};  // transparent; scene clears to 0x1A1A2E
    root->addChild(bg);

    const engine::math::Vec4 yellow{1.0f, 0.95f, 0.3f, 1.0f};
    const engine::math::Vec4 light{0.9f, 0.9f, 0.9f, 1.0f};
    const engine::math::Vec4 lightBlue{0.7f, 0.7f, 0.8f, 1.0f};

    // "Sample Game" title — centered, high on screen.
    auto* title = titleCanvas_->createNode<UiText>("title");
    title->anchor = {{0.f, 0.f}, {1.f, 0.f}};
    title->offsetMin = {0.f, 40.f * s};
    title->offsetMax = {0.f, 160.f * s};
    title->text = "Sample Game";
    title->font = hudFont_;
    title->fontSize = 96.f * s;
    title->color = yellow;
    title->align = TextAlign::Center;
    bg->addChild(title);

    // Objective lines
    auto* obj1 = titleCanvas_->createNode<UiText>("obj1");
    obj1->anchor = {{0.f, 0.f}, {1.f, 0.f}};
    obj1->offsetMin = {0.f, 180.f * s};
    obj1->offsetMax = {0.f, 220.f * s};
    obj1->text = "Roll the ball to collect all the coins.";
    obj1->font = hudFont_;
    obj1->fontSize = 30.f * s;
    obj1->color = light;
    obj1->align = TextAlign::Center;
    bg->addChild(obj1);

    auto* obj2 = titleCanvas_->createNode<UiText>("obj2");
    obj2->anchor = {{0.f, 0.f}, {1.f, 0.f}};
    obj2->offsetMin = {0.f, 224.f * s};
    obj2->offsetMax = {0.f, 264.f * s};
    obj2->text = "Don't fall off the edge!";
    obj2->font = hudFont_;
    obj2->fontSize = 30.f * s;
    obj2->color = light;
    obj2->align = TextAlign::Center;
    bg->addChild(obj2);

    // Start Game button group
    const float btnHalfW = 240.f * s;
    const float btnHalfH = 68.f * s;
    const float btnRadius = 24.f * s;

    // (1) Drop shadow — slightly larger, offset down-right, semi-transparent.
    auto* shadow = titleCanvas_->createNode<UiPanel>("startBtnShadow");
    shadow->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    shadow->offsetMin = {-btnHalfW - 4.f * s, -btnHalfH + 8.f * s};
    shadow->offsetMax = { btnHalfW + 4.f * s,  btnHalfH + 16.f * s};
    shadow->color = {0.0f, 0.0f, 0.05f, 0.45f};
    shadow->cornerRadius = btnRadius + 4.f * s;
    shadow->interactable = false;
    bg->addChild(shadow);

    // (2) The clickable button itself.
    auto* startBtn = titleCanvas_->createNode<UiButton>("startBtn");
    startBtn->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    startBtn->offsetMin = {-btnHalfW, -btnHalfH};
    startBtn->offsetMax = { btnHalfW,  btnHalfH};
    startBtn->label = "Start Game";
    startBtn->font = hudFont_;
    startBtn->fontSize = 52.f * s;
    // Rich blue-purple gradient fake: main fill is the mid tone,
    // highlight stripe on top brightens it, shadow below deepens it.
    startBtn->normalColor  = {0.28f, 0.36f, 0.72f, 1.0f};
    startBtn->hoverColor   = {0.40f, 0.52f, 0.95f, 1.0f};
    startBtn->pressedColor = {0.18f, 0.24f, 0.56f, 1.0f};
    startBtn->textColor    = {1.0f, 1.0f, 1.0f, 1.0f};
    startBtn->cornerRadius = btnRadius;
    startBtn->onClick = [this](engine::ui::UiNode&)
    {
        showTitleScreen_ = false;
        if (engine_ && registry_)
            loadLevel(*engine_, *registry_, 0);
    };
    bg->addChild(startBtn);

    // (3) Top highlight stripe — thin lighter band at the top of the button
    // to fake a gradient "lit from above" look. Non-interactive, drawn over
    // the button fill but under the button's text (UiButton draws its text
    // last inside its own onDraw, so the stripe ends up visually under the
    // text since it's a sibling drawn immediately after the button).
    auto* highlight = titleCanvas_->createNode<UiPanel>("startBtnHighlight");
    highlight->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
    highlight->offsetMin = {-btnHalfW + 6.f * s, -btnHalfH + 6.f * s};
    highlight->offsetMax = { btnHalfW - 6.f * s, -btnHalfH + 24.f * s};
    highlight->color = {1.0f, 1.0f, 1.0f, 0.12f};
    highlight->cornerRadius = btnRadius - 4.f * s;
    highlight->interactable = false;
    bg->addChild(highlight);

    // "Controls" label
    auto* ctrlLabel = titleCanvas_->createNode<UiText>("ctrlLabel");
    ctrlLabel->anchor = {{0.f, 0.5f}, {1.f, 0.5f}};
    ctrlLabel->offsetMin = {0.f, 90.f * s};
    ctrlLabel->offsetMax = {0.f, 130.f * s};
    ctrlLabel->text = "Controls";
    ctrlLabel->font = hudFont_;
    ctrlLabel->fontSize = 30.f * s;
    ctrlLabel->color = lightBlue;
    ctrlLabel->align = TextAlign::Center;
    bg->addChild(ctrlLabel);

    // Control lines — platform-specific, em dash for clean alignment
#ifdef __ANDROID__
    const char* ctrlLines[] = {
        "Tilt forward  --  Move forward",
        "Tilt back  --  Move backward",
        "Tilt left  --  Move left",
        "Tilt right  --  Move right",
        "Tap button  --  Start / Next level",
    };
#else
    const char* ctrlLines[] = {
        "W / Up  --  Move forward",
        "S / Down  --  Move backward",
        "A / Left  --  Move left",
        "D / Right  --  Move right",
        "R  --  Reset level",
    };
#endif
    float y = 140.f * s;
    for (const char* line : ctrlLines)
    {
        auto* t = titleCanvas_->createNode<UiText>("ctrl");
        t->anchor = {{0.f, 0.5f}, {1.f, 0.5f}};
        t->offsetMin = {0.f, y};
        t->offsetMax = {0.f, y + 30.f * s};
        t->text = line;
        t->font = hudFont_;
        t->fontSize = 24.f * s;
        t->color = light;
        t->align = TextAlign::Center;
        bg->addChild(t);
        y += 36.f * s;
    }
}

void SampleGame::buildEndLevelCanvas(bool hasNextLevel)
{
    using namespace engine::ui;

    endLevelCanvas_ = std::make_unique<UiCanvas>(canvasW_, canvasH_);
    endLevelCanvasBuilt_ = true;
    endLevelCanvasHasNext_ = hasNextLevel;

    const float s = canvasDpi_;  // DPI scale

    auto* root = endLevelCanvas_->root();

    auto* bg = endLevelCanvas_->createNode<UiPanel>("endBg");
    bg->anchor = {{0.f, 0.f}, {1.f, 1.f}};
    bg->color = {0.f, 0.f, 0.f, 0.0f};  // transparent overlay
    root->addChild(bg);

    const engine::math::Vec4 yellow{1.0f, 0.95f, 0.3f, 1.0f};

    auto* msg = endLevelCanvas_->createNode<UiText>("endMsg");
    msg->anchor = {{0.f, 0.f}, {0.f, 0.f}};
#ifdef __ANDROID__
    const float endTopInset = kAndroidTopInset * s;
#else
    const float endTopInset = 0.f;
#endif
    msg->offsetMin = {40.f * s, 32.f * s + endTopInset};
    msg->offsetMax = {900.f * s, 110.f * s + endTopInset};
    msg->text = hasNextLevel ? "LEVEL COMPLETE!" : "YOU WIN!";
    msg->font = hudFont_;
    msg->fontSize = 60.f * s;
    msg->color = yellow;
    msg->align = TextAlign::Left;
    bg->addChild(msg);

    if (hasNextLevel)
    {
        const float btnLeft = 40.f * s;
        const float btnTop = 130.f * s + endTopInset;
        const float btnRight = 340.f * s;
        const float btnBottom = 220.f * s + endTopInset;
        const float btnRadius = 22.f * s;

        auto* shadow = endLevelCanvas_->createNode<UiPanel>("nextBtnShadow");
        shadow->anchor = {{0.f, 0.f}, {0.f, 0.f}};
        shadow->offsetMin = {btnLeft - 4.f * s, btnTop + 8.f * s};
        shadow->offsetMax = {btnRight + 4.f * s, btnBottom + 16.f * s};
        shadow->color = {0.0f, 0.0f, 0.05f, 0.45f};
        shadow->cornerRadius = btnRadius + 4.f * s;
        shadow->interactable = false;
        bg->addChild(shadow);

        auto* nextBtn = endLevelCanvas_->createNode<UiButton>("nextBtn");
        nextBtn->anchor = {{0.f, 0.f}, {0.f, 0.f}};
        nextBtn->offsetMin = {btnLeft, btnTop};
        nextBtn->offsetMax = {btnRight, btnBottom};
        nextBtn->label = "Next Level";
        nextBtn->font = hudFont_;
        nextBtn->fontSize = 40.f * s;
        nextBtn->normalColor  = {0.28f, 0.36f, 0.72f, 1.0f};
        nextBtn->hoverColor   = {0.40f, 0.52f, 0.95f, 1.0f};
        nextBtn->pressedColor = {0.18f, 0.24f, 0.56f, 1.0f};
        nextBtn->textColor    = {1.0f, 1.0f, 1.0f, 1.0f};
        nextBtn->cornerRadius = btnRadius;
        nextBtn->onClick = [this](engine::ui::UiNode&)
        {
            if (engine_ && registry_)
                loadLevel(*engine_, *registry_, currentLevel_ + 1);
        };
        bg->addChild(nextBtn);

        auto* highlight = endLevelCanvas_->createNode<UiPanel>("nextBtnHighlight");
        highlight->anchor = {{0.f, 0.f}, {0.f, 0.f}};
        highlight->offsetMin = {btnLeft + 6.f * s, btnTop + 6.f * s};
        highlight->offsetMax = {btnRight - 6.f * s, btnTop + 22.f * s};
        highlight->color = {1.0f, 1.0f, 1.0f, 0.12f};
        highlight->cornerRadius = btnRadius - 4.f * s;
        highlight->interactable = false;
        bg->addChild(highlight);
    }
    else
    {
        // Final level won — "Back to Home Screen" button centered.
        const float btnHalfW = 280.f * s;
        const float btnHalfH = 68.f * s;
        const float btnRadius = 24.f * s;

        auto* shadow = endLevelCanvas_->createNode<UiPanel>("homeBtnShadow");
        shadow->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
        shadow->offsetMin = {-btnHalfW - 4.f * s, -btnHalfH + 8.f * s};
        shadow->offsetMax = { btnHalfW + 4.f * s,  btnHalfH + 16.f * s};
        shadow->color = {0.0f, 0.0f, 0.05f, 0.45f};
        shadow->cornerRadius = btnRadius + 4.f * s;
        shadow->interactable = false;
        bg->addChild(shadow);

        auto* homeBtn = endLevelCanvas_->createNode<UiButton>("homeBtn");
        homeBtn->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
        homeBtn->offsetMin = {-btnHalfW, -btnHalfH};
        homeBtn->offsetMax = { btnHalfW,  btnHalfH};
        homeBtn->label = "Back to Home Screen";
        homeBtn->font = hudFont_;
        homeBtn->fontSize = 44.f * s;
        homeBtn->normalColor  = {0.28f, 0.36f, 0.72f, 1.0f};
        homeBtn->hoverColor   = {0.40f, 0.52f, 0.95f, 1.0f};
        homeBtn->pressedColor = {0.18f, 0.24f, 0.56f, 1.0f};
        homeBtn->textColor    = {1.0f, 1.0f, 1.0f, 1.0f};
        homeBtn->cornerRadius = btnRadius;
        homeBtn->onClick = [this](engine::ui::UiNode&)
        {
            // Defer to next frame — can't destroy the canvas while
            // we're inside its event dispatch.
            returnToTitlePending_ = true;
        };
        bg->addChild(homeBtn);

        auto* highlight = endLevelCanvas_->createNode<UiPanel>("homeBtnHighlight");
        highlight->anchor = {{0.5f, 0.5f}, {0.5f, 0.5f}};
        highlight->offsetMin = {-btnHalfW + 6.f * s, -btnHalfH + 6.f * s};
        highlight->offsetMax = { btnHalfW - 6.f * s, -btnHalfH + 24.f * s};
        highlight->color = {1.0f, 1.0f, 1.0f, 0.12f};
        highlight->cornerRadius = btnRadius - 4.f * s;
        highlight->interactable = false;
        bg->addChild(highlight);
    }
}

void SampleGame::dispatchMouseEvents(engine::core::Engine& engine, engine::ui::UiCanvas& canvas)
{
    const auto& input = engine.inputState();

    // On desktop, mouseX/Y are logical pixels — multiply by contentScale
    // to match the physical-pixel canvas. On Android, touch coordinates
    // are already in physical pixels (no scaling needed).
#ifdef __ANDROID__
    const float mx = static_cast<float>(input.mouseX());
    const float my = static_cast<float>(input.mouseY());
#else
    const float scaleX = engine.contentScaleX();
    const float scaleY = engine.contentScaleY();
    const float mx = static_cast<float>(input.mouseX()) * scaleX;
    const float my = static_cast<float>(input.mouseY()) * scaleY;
#endif

    if (mx != prevMouseX_ || my != prevMouseY_)
    {
        engine::ui::UiEvent e;
        e.type = engine::ui::UiEventType::MouseMove;
        e.position = {mx, my};
        e.button = 0;
        canvas.dispatchEvent(e);
    }

    if (input.isMouseButtonPressed(engine::input::MouseButton::Left))
    {
        engine::ui::UiEvent e;
        e.type = engine::ui::UiEventType::MouseDown;
        e.position = {mx, my};
        e.button = 0;
        canvas.dispatchEvent(e);
    }

    if (input.isMouseButtonReleased(engine::input::MouseButton::Left))
    {
        engine::ui::UiEvent e;
        e.type = engine::ui::UiEventType::MouseUp;
        e.position = {mx, my};
        e.button = 0;
        canvas.dispatchEvent(e);
    }

    prevMouseX_ = mx;
    prevMouseY_ = my;
}

void SampleGame::onShutdown(Engine& /*engine*/, Registry& /*registry*/)
{
    uiRenderer_.shutdown();
#if SAMPLE_GAME_ENABLE_PERF_OVERLAY
    if (perfHudInitialized_)
    {
        perfHud_.shutdown();
        perfHudInitialized_ = false;
    }
#endif
    if (hudFontLoaded_)
    {
        msdfFont_.shutdown();
        hudFont_ = nullptr;
        hudFontLoaded_ = false;
    }
    ibl_.shutdown();
    physics_.shutdown();
    audio_.shutdown();
}

void SampleGame::renderPerfOverlay(Engine& engine, float /*dt*/)
{
#if SAMPLE_GAME_ENABLE_PERF_OVERLAY
    if (!showPerfOverlay_ || !perfHudInitialized_)
        return;

    // bgfx only populates viewStats when the profiler debug flag is set.
    // Enable it lazily on first show; cheap when already enabled.
    static bool profilerEnabled = false;
    if (!profilerEnabled)
    {
        bgfx::setDebug(BGFX_DEBUG_PROFILER);
        profilerEnabled = true;
    }

    const bgfx::Stats* s = bgfx::getStats();
    if (!s)
        return;

    const double cpuFreq = static_cast<double>(s->cpuTimerFreq);
    const double gpuFreq = static_cast<double>(s->gpuTimerFreq);
    const double frameCpuMs = (cpuFreq > 0.0)
        ? 1000.0 * static_cast<double>(s->cpuTimeEnd - s->cpuTimeBegin) / cpuFreq : 0.0;
    const double frameGpuMs = (gpuFreq > 0.0)
        ? 1000.0 * static_cast<double>(s->gpuTimeEnd - s->gpuTimeBegin) / gpuFreq : 0.0;

    perfHud_.begin(engine.fbWidth(), engine.fbHeight());

    constexpr uint32_t kHeader = 0xFFFF80FF;  // pale yellow
    constexpr uint32_t kRow    = 0xCFCFCFFF;  // light grey
    constexpr uint32_t kHot    = 0xFF6060FF;  // red (high-cost lines)

    // Push the overlay below the Android status bar / camera cutout.
    // DebugHud cells are 16 px tall; kAndroidTopInset (~50 px @ 1080) ≈ 3-4 rows.
#ifdef __ANDROID__
    const float dpi = static_cast<float>(engine.fbHeight()) / 1080.0f;
    const uint16_t startRow = static_cast<uint16_t>((kAndroidTopInset * dpi) / 16.0f) + 1;
#else
    const uint16_t startRow = 0;
#endif

    // Right-align: DebugHud positions text by `col * 8 px`, but the
    // BitmapFont actually renders glyphs ~16 px wide at kFontSize=16,
    // so the visual block is wider than `numChars * 8`.  Compute the
    // start position in pixels from the actual rendered width, then
    // convert back to a column index.
    constexpr uint16_t kCharWidthPx = 16;   // empirical glyph advance
    constexpr uint16_t kBlockChars  = 67;   // longest format string
    constexpr uint16_t kRightMarginPx = 32; // ~4 chars of breathing room
    const uint32_t blockPx = static_cast<uint32_t>(kBlockChars) * kCharWidthPx;
    const uint32_t startPx = (engine.fbWidth() > blockPx + kRightMarginPx)
        ? (engine.fbWidth() - blockPx - kRightMarginPx) : 0;
    const uint16_t col0 = static_cast<uint16_t>(startPx / 8);

    perfHud_.printf(col0, startRow, kHeader,
                    "FPS %5.1f   CPU %5.2f ms   GPU %5.2f ms   Draws %4u   Prims %5u",
                    fpsSmoothed_, frameCpuMs, frameGpuMs,
                    static_cast<unsigned>(s->numDraw),
                    static_cast<unsigned>(s->numPrims[0]));  // 0 = triangles

    perfHud_.printf(col0, startRow + 1, kHeader, "%-18s %8s %8s",
                    "Pass", "CPU(ms)", "GPU(ms)");

    uint16_t row = startRow + 2;
    const uint16_t maxRow = startRow + 12;  // cap engine-pass rows; game CPU follows below
    for (uint16_t i = 0; i < s->numViews && row < maxRow; ++i)
    {
        const bgfx::ViewStats& v = s->viewStats[i];
        // Skip unnamed views — bgfx keeps a slot for every view ID, even
        // ones the game never touches.  An empty name means we never
        // labelled it, so it's almost certainly a no-op.
        if (v.name[0] == '\0')
            continue;
        const double passCpuMs = (cpuFreq > 0.0)
            ? 1000.0 * static_cast<double>(v.cpuTimeEnd - v.cpuTimeBegin) / cpuFreq : 0.0;
        const double passGpuMs = (gpuFreq > 0.0)
            ? 1000.0 * static_cast<double>(v.gpuTimeEnd - v.gpuTimeBegin) / gpuFreq : 0.0;
        const uint32_t color = (passGpuMs > 5.0 || passCpuMs > 5.0) ? kHot : kRow;
        perfHud_.printf(col0, row++, color, "%-18s %8.2f %8.2f",
                        v.name, passCpuMs, passGpuMs);
    }

    // Game-side CPU timings — bgfx::Stats only measures bgfx submission,
    // so the bulk of frame CPU time lives in these sections.
    perfHud_.printf(col0, row++, kHeader, "%-18s %8s",
                    "Game CPU", "ms");
    auto gameRow = [&](const char* name, float ms) {
        const uint32_t color = (ms > 5.0f) ? kHot : kRow;
        perfHud_.printf(col0, row++, color, "%-18s %8.2f", name, ms);
    };
    gameRow("onFixedUpdate",   cpuMsOnFixedUpdate_);
    gameRow("  Physics",       cpuMsPhysics_);
    gameRow("onUpdate",        cpuMsOnUpdate_);
    gameRow("onRender",        cpuMsOnRender_);
    gameRow("  ShadowSubmit",  cpuMsShadowSubmit_);
    gameRow("  DrawCallUpdate",cpuMsDrawCallUpdate_);

    if (registry_)
    {
        size_t entityCount = 0;
        registry_->forEachEntity([&](EntityID) { ++entityCount; });
        perfHud_.printf(col0, row++, kRow, "Entities: %zu", entityCount);
    }
    perfHud_.printf(col0, row++, kRow, "Tex %u MB   RT %u MB",
                    static_cast<unsigned>(s->textureMemoryUsed >> 20),
                    static_cast<unsigned>(s->rtMemoryUsed >> 20));
    perfHud_.printf(col0, row, 0x808080FF,
                    "[P] / tap top-right corner to toggle");

    perfHud_.end();
#endif
}
