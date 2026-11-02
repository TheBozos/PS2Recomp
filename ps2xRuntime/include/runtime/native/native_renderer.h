#ifndef PS2X_NATIVE_RENDERER_H
#define PS2X_NATIVE_RENDERER_H

// Native PC rendering, shared by the ports: a port's adapter reads its game's own 3D data (meshes, matrices,
// materials) and submits it here instead of the PS2 VU1 / GS path; the runtime's present loop draws it with the PC
// GPU at the window's resolution, over the GS frame while a game is only partly native.
//
// Game thread: beginFrame(), draw(...) per mesh, endFrame() publishes the frame. Present thread: present() draws
// the latest published frame. Textures come from the game's own GS uploads (textures(), a TextureCache, fed by the
// runtime's GS front end), decoded when the frame is drawn, by which time the frame's own uploads have arrived.

#include "runtime/native/native_gs_hook.h"
#include "runtime/native/native_texture_cache.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::native
{
    struct Vertex
    {
        float x = 0, y = 0, z = 0; // the object's space: the draw's matrix takes it to PS2 screen space
        float u = 0, v = 0;        // texture coordinates, 0..1 across the texture (the GS's S / Q, T / Q)
        uint32_t rgba = 0;         // lit colour, R in the low byte; 0x80 per channel is 1.0, as the GS modulates
        float fog = 1.0f;          // the GS's F / 255: 1 keeps the colour, 0 is all fog colour (Material::fog)
        float q = 1.0f;            // the GS's Q, for a draw the GS front end makes (fromGsContext): U / V across the
                                   // triangle in perspective, as the GS interpolates S, T and Q
    };

    // How a draw blends and tests, from the GS registers a game sets (ALPHA, TEST) reduced to what a PC GPU does.
    struct Material
    {
        uint64_t tex0 = 0; // the texture (TextureCache::texture); 0 with textured = false
        uint64_t texa = 0;
        bool textured = true;
        enum class Blend : uint8_t
        {
            Opaque,   // no blending
            Alpha,    // (Cs - Cd) * As + Cd
            Additive, // Cs * As + Cd
            AdditiveByDestination, // Cs * Ad + Cd, Ad the target's alpha (0x80 is 1.0)
        } blend = Blend::Alpha;
        uint8_t alphaRef = 0;   // texels at or below are discarded when alphaTest
        bool alphaTest = false;
        bool depthTest = true;  // GS depth: larger is nearer (GEQUAL)
        bool depthWrite = true;
        bool clampU = false, clampV = false; // CLAMP's WMS / WMT: the texture's edge past 0..1 (else it repeats)
        uint64_t region = 0; // CLAMP, for a REGION_REPEAT window (TextureCache::regionWindow): the texture is that window
        bool fog = false;           // PRIM.FGE: the colour goes toward fogColour by 1 - Vertex::fog, after texturing
        // The texture is the target being drawn into, as it is when this batch is drawn (a game's screen distortion:
        // heat haze sampling the frame through TEX0 = the frame buffer); u, v in the target's GS pixels. Stream draws
        // only (the GS pass): the frame is not in GS memory for the texture cache to decode.
        bool sampleTarget = false;
        uint32_t fogColour = 0;     // FOGCOL, R in the low byte
        // The GS state where the draw's group sits in the stream (a stream draw, beginGroup) gives tex0, texa, blend,
        // the alpha and depth tests, clampU / clampV and fogColour, for games whose meshes take their material from
        // GS packets around them; textured, fog and alphaBlend (PRIM.ABE) stay the draw's own.
        bool fromGsContext = false;
        bool alphaBlend = true;
        uint8_t context = 0; // PRIM.CTXT of a fromGsContext draw
        // GS register writes (address, value) the GS front end makes before a fromGsContext draw, in order: the A+D
        // and packed register writes of the packet it came from (a sandboxed microprogram's TEX0 with its CLUT load,
        // CLAMP, TEX1, ALPHA, ...), so it draws with the state the packet would have set. Not part of batching.
        std::shared_ptr<const std::vector<std::pair<uint8_t, uint64_t>>> gsWrites;
    };

    // The blend of a draw with PRIM.ABE set, from the GS's ALPHA register.
    Material::Blend blendOf(uint64_t alpha);

    // PS2 screen space as the game draws to it: the screen point at the window's top-left corner and the size
    // that fills the window (the GS XYOFFSET and the frame size; a 640 x 448 / 512 or 640 x 256 picture).
    struct Screen
    {
        float originX = 2048.0f - 320.0f, originY = 2048.0f - 256.0f;
        float width = 640.0f, height = 512.0f;
        float depthMax = 16777215.0f; // the Z buffer's range (24-bit)
        float gsZScale = 1.0f;        // GS Z per unit of the matrices' z / w (a microprogram writing FTOI4 z: 16)
    };

    class Renderer
    {
    public:
        static Renderer &instance();

        // Game thread.
        void beginFrame(const Screen &screen);
        // Triangles (three vertices each) with an object-to-screen matrix: column c is matrix[4c..4c+3], screen
        // point = M * (x, y, z, 1), then divided by w (as a VU's projection).
        void draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count);
        // The same with the texture handed over, decoded by the port from its game's own data (its texture assets in EE
        // RAM): no texture cache lookup for this batch (material.textured still says whether it is textured; material.tex0
        // keys its GPU copy, so each asset needs its own value there).
        void draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count,
                  std::shared_ptr<const TextureCache::Texture> texture);
        // A port's 2D drawn natively (its HUD, text, menus, from the game's own routines): screen-space triangles (x, y
        // the GS's primitive coordinates over 16, as a draw with no matrix gets them) over the frame, in submission order,
        // without depth; publishOverlay at the game frame's end shows them from the next present() on, over the frame
        // (the GS's, or this renderer's own) until the next publish.
        void drawOverlay(const Material &material, const Vertex *vertices, uint32_t count,
                         std::shared_ptr<const TextureCache::Texture> texture);
        void publishOverlay();
        // The same, the overlay's GS coordinates mapped to the window by this screen (the game's own view) instead of the
        // frame being built's.
        void publishOverlay(const Screen &screen);
        // A picture under the overlay (a movie frame), at (x, y, width, height) in the screen's GS coordinates; shown while
        // set again within a fifth of a second, or, held, until clearBackdrop or the next one (a picture the game left in
        // its display buffers and draws its menus over: a movie's last frame).
        void setBackdrop(std::shared_ptr<const TextureCache::Texture> picture, const Screen &screen, float x, float y, float width,
                         float height, bool held = false);
        void clearBackdrop();
        // The GS's TEXA as the game last wrote it (GS front end): a draw whose material sets no TEXA of its own (texa
        // 0, not fromGsContext) decodes its texture with it, as the GS would (16- and 24-bit texels take their alpha
        // from it; a game may write TEXA once, outside the stream a port reads).
        void setGsTexa(uint64_t texa) { m_gsTexa.store(texa, std::memory_order_relaxed); }
        void endFrame();
        // Depth a game reads back from the GS (a visibility test: TMB's lens flares sample the Z buffer, which the
        // native frame is not in). While asked for (wantDepth, again within a second), present() copies the native
        // frame's depth; depthAt gives it at a GS screen point as GS Z, from the frame presented last (false before).
        void wantDepth();
        bool depthAt(float gsX, float gsY, uint32_t &gsZ);
        // The frame being built's GS Z per unit of z / w (screen-space draws give GS Z over it).
        float gsZScale() const { return m_building.screen.gsZScale; }
        TextureCache &textures() { return ps2x::native::textures(); }
        // Drawing inside the GS stream (runtime/native/native_gs_hook.h). The native port numbers the VU1 programs it
        // draws instead of VU1, in stream order (1, 2, ...: the VIF1 interpreter numbers the programs it skips the
        // same way), and draws a program's batches after beginGroup(its number). With a GS renderer that keeps GL
        // render targets, each group is drawn into its target at its point of the GS stream, so it sorts with the GS
        // draws around it, and present() leaves the frame to the GS; otherwise present() draws as before.
        void setStreamDraws(bool on);
        void beginGroup(uint64_t group) { m_group = group; }
        // The port's frame is the whole picture: the window shows only what present() draws (its frame, the display's
        // circuit 2 picture, a movie's backdrop, the 2D), never the GS front end's frame.
        void setPresentsAlone(bool on) { m_presentsAlone = on; }
        // The rectangle the next draws are clipped to, in GS primitive coordinates, until set again (a game's view in a
        // split screen: where the GS scissor would have cut them); clearClip: none. A group's GS scissor, when the GS
        // reached its marker, is used only for draws without one.
        void setClip(float x0, float y0, float x1, float y1)
        {
            m_clip[0] = x0;
            m_clip[1] = y0;
            m_clip[2] = x1;
            m_clip[3] = y1;
        }
        void clearClip() { setClip(0, 0, 0, 0); }
        // What the frame starts from presenting alone (the game's clear: a background or fog colour, R in the low byte).
        void setClearColour(uint32_t rgb) { m_clearColour.store(rgb, std::memory_order_relaxed); }
        bool presentsAlone() const { return m_enabled && m_presentsAlone; }
        // A group has batches waiting (its GS marker is worth queueing): the VIF1 interpreter reaches a program after
        // the native port has drawn it (at the transfer's start), so a group without batches by then has none.
        bool hasGroup(uint64_t group);
        // GS thread, at a group's marker: hands the GS front end the fromGsContext batches of the groups up to
        // `group` (the others stay for the hook), for a front end that draws them itself with the GS state it has
        // there (its own renderer, textures decoded where the GS keeps them) instead of this renderer's pass.
        using StreamVisitor = void (*)(void *user, const Material &material, const float matrix[16], float gsZScale,
                                       const Vertex *vertices, uint32_t count);
        // Returns whether batches up to `group` are left for the hook (GsDrawHook): none, and the GS front end needs
        // no target for this marker.
        bool takeGsContextBatches(uint64_t group, StreamVisitor visit, void *user);
        // Frames faster than the game's (setFrameRate other than 0): the native port keys each object's batches by
        // something stable from one game frame to the next (its own data and instance, not its place in the stream);
        // present() then draws an object found in the previous frame too with its matrix between the two, by where
        // the window's clock is between the two frames' times (one game frame behind, as interpolation is). 0: not
        // interpolated (drawn as the latest frame has it). base: what the object is without telling alike ones apart
        // (the key less its count among them, e.g. the same scenery model placed several times): such objects are
        // matched to the nearest of the previous frame's alike ones rather than by their count, which changes when
        // one of them is culled; 0: the key alone. setKey also ends the parts setParts gave.
        void setKey(uint64_t key, uint64_t base = 0)
        {
            m_key = key;
            m_keyBase = base != 0u ? base : key;
            m_hasParts = false;
        }
        // The keyed batches that follow are camera x object (columns, as draw()'s matrix, which must be their product):
        // interpolated apart, the camera's linearly and the object's rotation as a rotation (no shrinking on a fast
        // turn), its scale and translation linearly. Until the next setKey.
        void setParts(const float camera[16], const float object[16])
        {
            std::copy(camera, camera + 16, m_camera);
            std::copy(object, object + 16, m_object);
            m_hasParts = true;
        }
        // Screen-space triangles (identity matrix: sprites, particles) whose vertices move from frame to frame:
        // triangle i is ids[i] (0: none, drawn as it is), a triangle with the same id in the previous frame is drawn
        // with its corners between the two (the same corner order: a particle's quad is two ids).
        void drawTracked(const Material &material, const Vertex *vertices, uint32_t count,
                         std::shared_ptr<const TextureCache::Texture> texture, const uint64_t *ids);
        // The window's frame rate: 0 the game's (each published frame shown once, as it comes: the default), -1 the
        // monitor's (vsync; frames between the game's drawn interpolated), n > 0 that many a second (no vsync).
        // Interpolation needs the port's own frame (presentsAlone, no stream draws); otherwise frames are shown as they
        // come whatever the rate.
        void setFrameRate(int rate) { m_frameRate.store(rate, std::memory_order_relaxed); }
        int frameRate() const { return m_frameRate.load(std::memory_order_relaxed); }
        bool interpolating() const { return frameRate() != 0 && m_presentsAlone && !m_streamDraws && m_enabled; }
        // What the last present() drew: when, the two published frames (their serials, from = 0: none) and how far
        // between them (1: the newer as it is).
        struct PresentInfo
        {
            double time = 0.0;
            uint64_t from = 0, to = 0;
            float alpha = 1.0f;
            uint32_t interpolatedBatches = 0, keyedBatches = 0;
        };
        PresentInfo lastPresent() const { return m_lastPresent; }
        // The next `presents` present()s log the screen position of the origin of the keyed objects that moved most
        // ([interp-trace]: the smoothness check, with the sequence capture).
        void tracePresents(int presents) { m_tracePresents = presents; }

        // Present thread: draws the latest frame into the given window rectangle; false when there is none yet.
        bool present(float x, float y, float width, float height);
        // The 3D's own resolution against the window rectangle's (the Video settings' render scale, 0.25 to 4): drawn
        // at that many times its size each way, then filtered to it (2: four samples a window pixel).
        void setRenderScale(float scale) { m_renderScale.store(std::clamp(scale, 0.25f, 4.0f), std::memory_order_relaxed); }
        float renderScale() const { return m_renderScale.load(std::memory_order_relaxed); }
        // The GS frame shown with the native one (a GL texture): PS2X_NATIVE_CAPTURE_DIR saves it with each capture.
        void setGsFrame(unsigned int textureId, int width, int height, int format)
        {
            m_gsFrame[0] = static_cast<int>(textureId);
            m_gsFrame[1] = width;
            m_gsFrame[2] = height;
            m_gsFrame[3] = format;
        }
        bool enabled() const { return m_enabled; }
        // The latest published frame has something to draw (not a menu or a movie the native port does not draw yet),
        // and present() draws it (the GS frame does not already have it).
        bool hasDraws();
        // Present thread: waits up to timeout seconds for a frame (or a movie picture: setBackdrop) published after the
        // one counted in seen, and counts it there. With no GS front end the window then shows each game frame once, on
        // the game's own clock: with a 60 Hz timer of its own as well, the two clocks drift and every few seconds one
        // frame shows twice and the next not at all (a stutter). (DC_Recomp's fcb3ebe.)
        bool waitForFrame(uint64_t &seen, double timeout);
        void setEnabled(bool on)
        {
            m_enabled = on;
            if (on)
                setUploadsWanted(true);
        }

        struct Batch
        {
            Material material;
            float matrix[16];
            uint32_t first = 0, count = 0;
            uint64_t group = 0; // beginGroup's (its clip at present: the GS scissor where that group's marker was)
            uint64_t key = 0; // setKey
            uint64_t base = 0; // setKey's base (the key when none)
            bool hasParts = false;           // setParts: matrix = camera x object
            float camera[16], object[16];    // (set only with hasParts)
            int32_t idFirst = -1;            // drawTracked: its triangles' ids from Frame::triangleIds[idFirst]
            float clip[4] = {0, 0, 0, 0}; // setClip's, in GS primitive coordinates (x0, y0, x1, y1); x1 <= x0: none
            // Taken from the texture cache when the batch is drawn (the game's thread, in stream order): games reuse
            // texture memory before the frame is presented. Null when its upload was not there yet (an upload in the
            // same VIF1 transfer reaches the cache when VIF1 gets to it): looked up again where the batch is drawn,
            // in the GS stream (after the upload) or at present.
            std::shared_ptr<const TextureCache::Texture> texture;
        };
        struct Frame
        {
            Screen screen;
            std::vector<Vertex> vertices;
            std::vector<Batch> batches;
            double time = 0.0;                           // when it was published (seconds)
            std::unordered_map<uint64_t, uint32_t> keys; // key -> its first batch
            uint64_t lastGroup = 0;                      // its batches' latest group (0: none)
            // Interpolation: the tracked triangles' ids (drawTracked) and, built at publishing, id -> its first vertex;
            // the frame's number, its time on the smoothed clock the window follows, and whether it follows the one
            // before it (false after a pause in publishing: a load, nothing to interpolate across).
            std::vector<uint64_t> triangleIds;
            std::unordered_map<uint64_t, uint32_t> tracked;
            std::unordered_map<uint64_t, std::vector<uint32_t>> bases; // base -> each object's first batch
            uint64_t serial = 0;
            double pts = 0.0;
            bool continuous = false;
        };

    private:
        std::atomic<uint64_t> m_gsTexa{0};
        Renderer();
        ~Renderer();
        struct Gpu;
        struct GsPass; // GS thread only (native_renderer_gs.cpp)
        struct StreamBatch
        {
            uint64_t group = 0;
            float gsZScale = 1.0f;
            Batch batch;
            std::vector<Vertex> vertices;
        };
        static void gsDraw(uint64_t group, const GsDrawTarget &target);
        void drawGroups(uint64_t group, const GsDrawTarget &target);
        void drawOverlay(const Frame &overlay, float x, float y, float width, float height); // present thread

        Frame m_building;
        Frame m_overlayBuilding;                  // game thread (drawOverlay)
        std::shared_ptr<Frame> m_overlayPublished; // under m_mutex
        bool m_presentsAlone = false;
        float m_clip[4] = {0, 0, 0, 0};
        std::atomic<uint32_t> m_clearColour{0};
        std::atomic<float> m_renderScale{1.0f};
        std::shared_ptr<Frame> m_backdrop;         // under m_mutex: one textured batch, its screen, its time
        std::mutex m_mutex;
        std::shared_ptr<Frame> m_published;
        std::shared_ptr<Frame> m_previous; // the one before m_published (interpolation)
        uint64_t m_publishes = 0;          // frames and backdrops published, under m_mutex (waitForFrame)
        std::condition_variable m_publishedChanged;
        uint64_t m_key = 0, m_keyBase = 0;
        bool m_hasParts = false;
        float m_camera[16] = {}, m_object[16] = {};
        std::atomic<int> m_frameRate{0};
        // Interpolation's frames (newest last, a few game frames' worth) and the clock they are placed on, under m_mutex.
        std::deque<std::shared_ptr<Frame>> m_history;
        uint64_t m_serial = 0;
        double m_interval = 1.0 / 60.0; // the publishing interval, smoothed
        double m_late = 0.0;            // how late frames come against their place on the clock, recent worst (decaying)
        PresentInfo m_lastPresent;      // present thread
        int m_tracePresents = 0;        // present thread
        std::vector<uint64_t> m_traceKeys;
        uint64_t m_traceSerial = 0; // the frame the traced keys are of (followed to the next frame by its match)
        std::vector<Vertex> m_trackedScratch; // present thread: a tracked batch's vertices as drawn
        // Present thread: picks the two frames to draw between and how far (interpolation); false: none yet.
        bool pickInterpolated(std::shared_ptr<Frame> &from, std::shared_ptr<Frame> &to, float &alpha, double now);
        // Present thread: which of the older frame's objects each keyed batch of the newer one is, once per pair of
        // frames (matchFrames), and how it is drawn between them.
        enum : uint8_t
        {
            kUnmatched = 0,
            kLerp = 1,     // between the two
            kSpin = 2,     // turned over 90 degrees (a wheel): its place between, its turn the newer frame's
            kTeleport = 3, // moved too far (a respawn): as the newer frame has it
        };
        struct Match
        {
            uint64_t from = 0, to = 0;     // the frames' serials
            std::vector<int32_t> previous; // per batch of the newer: the older's batch with its matrix (-1: none)
            std::vector<uint8_t> kind;
            std::vector<uint8_t> cut;      // its view cut away (the camera): as the newer frame has it
            std::unordered_map<uint64_t, bool> cutViews;
        } m_match;
        void matchFrames(const Frame &from, const Frame &to);
        int m_gsFrame[4] = {}; // id, width, height, pixel format
        std::unique_ptr<Gpu> m_gpu; // present thread only
        bool m_enabled = false;
        bool m_streamDraws = false;
        uint64_t m_group = 0;
        std::mutex m_streamMutex;
        std::deque<StreamBatch> m_stream; // batches waiting for their group's point of the GS stream
        GsPass *m_gsPass = nullptr;       // its GL objects live in the GS thread's context: kept to the end
        std::atomic<bool> m_drewIntoGs{false};
        // Without GL targets (present() draws the frame over the GS one): each group's GS scissor, in primitive
        // coordinates (scissor plus XYOFFSET), as the GS reached its marker; a frame is shown once the GS has reached
        // its last group, so its clips are known (until then, the one before).
        struct Clip
        {
            uint64_t group = 0;
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        };
        static constexpr size_t kClips = 1u << 14;
        std::unique_ptr<Clip[]> m_clips{new Clip[kClips]};
        std::atomic<uint64_t> m_markedGroup{0}; // the latest group whose marker the GS reached
        // Depth probes with stream draws (depthAt): the points asked for, read on the GS thread from the frame's target
        // once its last group is drawn (the first group of the next frame), their GS Z kept for the next asks.
        struct ProbeTarget
        {
            uint32_t framebuffer = 0, width = 0, height = 0, scale = 1, xOffset = 0, yOffset = 0;
            double zScale = 0.0;
        };
        std::mutex m_probeMutex;
        std::unordered_map<uint64_t, uint32_t> m_probeWanted, m_probeResults; // (x << 16 | y) in GS pixels -> GS Z
        std::deque<uint64_t> m_frameLastGroups; // each published frame's last group, oldest first
        ProbeTarget m_probeTarget;              // GS thread: the target the last group drew into
        void readProbes();                      // GS thread
        std::shared_ptr<Frame> m_shown, m_shownPrevious; // present thread: the frame drawn and the one before it
        std::atomic<double> m_depthWantedAt{-10.0};      // wantDepth()'s time (seconds)
        std::mutex m_depthMutex;
        // The window depth read back, by tiles of kDepthTile x kDepthTile pixels (top-down, key ty << 32 | tx; rows
        // top-down in the tile, 1 past the window's edge): only around the pixels depthAt was asked for lately (the lens
        // flares' few samples), not the whole frame each present.
        static constexpr int kDepthTile = 32;
        std::unordered_map<uint64_t, std::vector<float>> m_depthTiles; // of the frame presented last (or the one before)
        std::unordered_map<uint64_t, double> m_depthTilesWanted;       // tile -> when depthAt last asked for it (seconds)
        int m_depthWidth = 0, m_depthHeight = 0;
        Screen m_depthScreen;
    };
}

#endif
