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

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps2x::native
{
    struct Vertex
    {
        float x = 0, y = 0, z = 0; // the object's space: the draw's matrix takes it to PS2 screen space
        float u = 0, v = 0;        // texture coordinates, 0..1 across the texture (the GS's S / Q, T / Q)
        uint32_t rgba = 0;         // lit colour, R in the low byte; 0x80 per channel is 1.0, as the GS modulates
        float fog = 1.0f;          // the GS's F / 255: 1 keeps the colour, 0 is all fog colour (Material::fog)
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
            Subtract, // Cd - Cs * As
            Darken,   // Cd * (1 - As)
            Brighten, // Cd * (1 + As)
            // A shadow volume (closed, faces outward): counted per pixel where it is in front of the scene (faces
            // seen from the front +1, from the back -1), then the target darkened once where the count is not 0, by
            // the alpha of the vertices' colour (0x80 = black).
            ShadowVolume,
            // The GS's (A - B) * C + D as gsAlpha (ALPHA's low byte) and gsFix (its FIX) give it, every combination
            // a GPU blend can do (those with a factor of 1 + C keep only their C part).
            Gs,
        } blend = Blend::Alpha;
        uint8_t gsAlpha = 0, gsFix = 0x80;
        uint8_t alphaRef = 0;   // texels at or below are discarded when alphaTest
        bool alphaTest = false;
        bool depthTest = true;  // GS depth: larger is nearer (GEQUAL)
        bool depthWrite = true;
        bool clampU = false, clampV = false; // CLAMP's WMS / WMT: the texture's edge past 0..1 (else it repeats)
        // The CLAMP register as written, for its region modes (WMS / WMT 2 REGION_CLAMP to MINU..MAXU, 3 REGION_REPEAT
        // (u & MINU) | MAXU): a texture atlas's part. 0: clampU / clampV alone.
        uint64_t clamp = 0;
        // FBA: the alpha written gets its high bit set (0x80 added below it), as the GS's FBA register does.
        bool fba = false;
        // A scissor of the draw's own in GS window coordinates (x0, y0, x1, y1, the last pixels included); x1 < x0: none.
        float scissor[4] = {0.0f, 0.0f, -1.0f, -1.0f};
        bool fog = false;           // PRIM.FGE: the colour goes toward fogColour by 1 - Vertex::fog, after texturing
        uint32_t fogColour = 0;     // FOGCOL, R in the low byte
        // The GS state where the draw's group sits in the stream (a stream draw, beginGroup) gives tex0, texa, blend,
        // the alpha and depth tests, clampU / clampV and fogColour, for games whose meshes take their material from
        // GS packets around them; textured, fog and alphaBlend (PRIM.ABE) stay the draw's own.
        bool fromGsContext = false;
        // A draw with its own material but the fog colour and TEXA the GS has where it sits (FOGCOL, TEXA: registers
        // a game may set outside the data its adapter reads).
        bool fogColourFromGs = false;
        bool texaFromGs = false;
        bool alphaBlend = true;
        uint8_t context = 0; // PRIM.CTXT of a fromGsContext draw
        // How the texel and the vertex colour combine (the game's texture function): 0 modulate, 1 decal, 2 highlight,
        // 3 highlight2; textureAlpha false takes the alpha from the vertex alone (the texture's colour only).
        uint8_t textureFunction = 0;
        bool textureAlpha = true;
    };

    // The blend of a draw whose PRIM.ABE is set, from the GS's ALPHA register ((A - B) * C + D): Opaque when A = B,
    // else the matching mode (C = As or FIX, taken as As); Alpha for a combination these modes do not cover.
    Material::Blend blendFromAlpha(uint64_t alpha);

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
        // The same with the texture given (a port's own, from its game's data: no TEX0 looked up).
        void draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count,
                  std::shared_ptr<const TextureCache::Texture> texture);
        void endFrame();
        // A full-screen pass at this point of the frame (a port's game's own post effect, from its parameters): the
        // frame drawn so far shrunk to width x height (bilinear), brightened by gain (clamped to 1), blurred (Gaussian,
        // sigma in shrunk pixels), stretched back (bilinear) and mixed over the frame by alpha (0..1), only where the
        // frame's depth is at least depthFrom (0..1 of the window's range, 1 the cleared depth, 0 everywhere).
        struct PostEffect
        {
            int width = 128, height = 89;
            float gain = 1.0f, sigma = 0.0f, alpha = 1.0f;
            float depthFrom = 1.0f;
            // depthWeights: the blurred copy is mixed by alpha x weights[i] / 128 per pixel instead, i = min(255, Z >>
            // weightShift) from the frame's GS Z (24-bit): a game's depth of field table (depthFrom unused, unless
            // depthLimit: then only where the depth is at least depthFrom too).
            bool depthLimit = false;
            bool depthWeights = false;
            int weightShift = 16;
            uint8_t weights[256] = {};
            // levels instead of the blur: every pixel's RGB becomes clamp(levelGain x max(0, min(F, levelHigh) -
            // levelLow)) per channel (0..1 values; a game's brightness / tone pass over its finished frame).
            bool levels = false;
            // levelsByAlpha: each pixel moves toward its levels by its own alpha (0..1 of the target's, a GS frame's
            // 0x80 = 1): a game whose pass adds the frame onto itself with Cs x As + Cd leaves its pixels of alpha 0
            // (drawn so as not to be brightened) as they are.
            bool levelsByAlpha = false;
            // scatter instead: a game's aerial perspective over its frame. Each pixel's GS Z (24-bit) gives the index
            // i = min(255, Z >> scatterShift) into scatterTable (R, G, B in-scatter, A transmittance, 128 = 1, R in the
            // low byte); RGB becomes F x A x 127 / 128 / 128 + table RGB x V / 128, V the in-scatter colour of the
            // screen point from scatterGrid (gridWidth x gridHeight RGBA, row 0 the top, points on the screen's
            // corners and between, bilinear).
            bool scatter = false;
            int scatterShift = 16;
            uint32_t scatterTable[256] = {};
            int gridWidth = 0, gridHeight = 0;
            std::vector<uint32_t> scatterGrid;
            float levelGain[3] = {1.0f, 1.0f, 1.0f}, levelHigh[3] = {1.0f, 1.0f, 1.0f}, levelLow[3] = {0.0f, 0.0f, 0.0f};
            // glow instead: a game's bloom. The frame x gain less glowThreshold (0..1, at least 0) shrunk to width x
            // height, then glowLevelCount levels (each half the last, from the unblurred one), each blurred by sigma
            // texels, all added onto the frame (Cs x As + Cd) with As = alpha x glowLevelWeight.
            bool glow = false;
            float glowThreshold = 0.0f, glowLevelWeight = 1.0f;
            int glowLevelCount = 1;
        };
        // at: the batch it goes before (batchCount() when that point was reached; past the end: at the end).
        // Returns the pass's index in the frame being built, for adjustPostEffect.
        size_t postEffect(const PostEffect &effect, size_t at = SIZE_MAX);
        // A pass of the frame being built, changeable until endFrame (a game that finishes an effect's parameters later
        // in its frame than where the pass goes); null for an index not in this frame.
        PostEffect *adjustPostEffect(size_t index);
        size_t batchCount() const { return m_building.batches.size(); }
        // Depth a game reads back from the GS (a visibility test: TMB's lens flares sample the Z buffer, which the
        // native frame is not in). While asked for (wantDepth, again within a second), present() copies the native
        // frame's depth; depthAt gives it at a GS screen point as GS Z, from the frame presented last (false before).
        void wantDepth();
        bool depthAt(float gsX, float gsY, uint32_t &gsZ);
        // The frame being built's GS Z per unit of z / w (screen-space draws give GS Z over it).
        float gsZScale() const { return m_building.screen.gsZScale; }
        // A picture of the screen, for a game that shows a frame of its own again (VP2's pause draws its last field
        // frame, frozen and blurred, under its menu): the next frame present() draws (the one on screen, unless the game
        // published another meanwhile) is read back once drawn, RGBA8 with row 0 the top and alpha opaque, as
        // capturedFrame(), a new texture each capture (null before the first). Any thread.
        void captureFrame();
        std::shared_ptr<const TextureCache::Texture> capturedFrame();
        // A material's tex0 for capturedFrame() as it is when present() draws the batch (drawn without a texture): a
        // capture asked for while a frame is built is ready by the time that frame is presented.
        static constexpr uint64_t kCapturedFrame = 0xCA97F00000000000ull;
        TextureCache &textures() { return ps2x::native::textures(); }
        // Drawing inside the GS stream (runtime/native/native_gs_hook.h). The native port numbers the VU1 programs it
        // draws instead of VU1, in stream order (1, 2, ...: the VIF1 interpreter numbers the programs it skips the
        // same way), and draws a program's batches after beginGroup(its number). With a GS renderer that keeps GL
        // render targets, each group is drawn into its target at its point of the GS stream, so it sorts with the GS
        // draws around it, and present() leaves the frame to the GS; otherwise present() draws as before.
        void setStreamDraws(bool on);
        // A blended draw writes its own alpha to the target (As, as the GS does) instead of blending it with the
        // target's (As + Ad x (1 - As)): for a game whose later pass reads the frame's alpha (setGsAlphaWrites).
        void setGsAlphaWrites(bool on) { m_gsAlphaWrites = on; }
        // An untextured draw's colour as the GS has it: the vertex colour as it is (0x80 = 128 of 255), the 0x80 = 1.0
        // scale being the texture modulation's only (its alpha stays a blend factor, 0x80 = 1). Off: x 255 / 128 too.
        void setGsUntexturedColour(bool on) { m_gsUntextured = on; }
        // The frame being built starts from this opaque colour (the GS's frame clear, 0x80 = 128 of 255) instead of a
        // transparent target: what the game's own geometry leaves uncovered shows it, blends over it included.
        void setClearColour(uint8_t r, uint8_t g, uint8_t b)
        {
            m_building.cleared = true;
            m_building.clearColour[0] = r;
            m_building.clearColour[1] = g;
            m_building.clearColour[2] = b;
        }
        void beginGroup(uint64_t group) { m_group = group; }
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
        // Frames faster than the game's (PS2X_NATIVE_INTERPOLATE=1): the native port keys each object's batches by
        // something stable from one game frame to the next (its own data, not its place in the stream); present()
        // then draws a key found in the previous frame too with its matrix between the two, by how far the window's
        // time is into the current game frame (one game frame behind, as interpolation is). 0: not interpolated.
        void setKey(uint64_t key) { m_key = key; }

        // Present thread: draws the latest frame into the given window rectangle; false when there is none yet.
        bool present(float x, float y, float width, float height);
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
            // Taken from the texture cache when the batch is drawn (the game's thread, in stream order): games reuse
            // texture memory before the frame is presented. Null when its upload was not there yet (an upload in the
            // same VIF1 transfer reaches the cache when VIF1 gets to it): looked up again where the batch is drawn,
            // in the GS stream (after the upload) or at present.
            std::shared_ptr<const TextureCache::Texture> texture;
            int post = -1; // >= 0: not a draw but Frame::posts[post] (postEffect)
        };
        struct Frame
        {
            Screen screen;
            std::vector<Vertex> vertices;
            std::vector<Batch> batches;
            std::vector<PostEffect> posts;
            double time = 0.0;                           // when it was published (seconds)
            std::unordered_map<uint64_t, uint32_t> keys; // key -> its first batch
            uint64_t lastGroup = 0;                      // its batches' latest group (0: none)
            bool cleared = false;                        // the target starts from clearColour (else transparent)
            uint8_t clearColour[3] = {};
        };

        // Any thread (the MPEG decoder): a movie picture just decoded (RGBA8, width x height). While pictures keep
        // coming (the last within 0.25 s), present() shows the latest over the frame, fitted to the picture: a game's
        // movies otherwise reach the screen only through the GS.
        void movieFrame(int width, int height, const uint8_t *rgba);

    private:
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

        Frame m_building;
        std::mutex m_mutex;
        std::shared_ptr<Frame> m_published;
        std::shared_ptr<Frame> m_previous; // the one before m_published (interpolation)
        uint64_t m_key = 0;
        int m_gsFrame[4] = {}; // id, width, height, pixel format
        std::unique_ptr<Gpu> m_gpu; // present thread only
        std::mutex m_movieMutex;
        std::vector<uint8_t> m_movie;
        int m_movieWidth = 0, m_movieHeight = 0;
        uint64_t m_movieGeneration = 0;
        double m_movieTime = 0.0;
        bool m_enabled = false;
        bool m_streamDraws = false;
        bool m_gsAlphaWrites = false;
        bool m_gsUntextured = false;
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
        std::shared_ptr<Frame> m_shown, m_shownPrevious; // present thread: the frame drawn and the one before it
        std::atomic<double> m_depthWantedAt{-10.0};      // wantDepth()'s time (seconds)
        std::mutex m_depthMutex;
        std::atomic<double> m_captureAt{-1.0}; // captureFrame()'s time (seconds), -1 when none is waiting
        std::mutex m_captureMutex;
        std::shared_ptr<const TextureCache::Texture> m_captured;
        uint64_t m_captures = 0;
        std::vector<float> m_depth; // window depth, bottom-up rows, of the frame presented last
        int m_depthWidth = 0, m_depthHeight = 0;
        Screen m_depthScreen;
    };
}

#endif
