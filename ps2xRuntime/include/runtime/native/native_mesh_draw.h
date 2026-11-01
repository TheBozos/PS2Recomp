#ifndef PS2X_NATIVE_MESH_DRAW_H
#define PS2X_NATIVE_MESH_DRAW_H

// Native PC rendering, shared by the ports: the draws a port builds per object from its mesh pack
// (runtime/native/native_mesh_pack.h) and EE state, submitted to the Renderer in the order the game sends its objects,
// with the GS state (ALPHA, TEST, ZBUF.ZMSK) they would have had in the GS.
//
// - MeshBatch / MeshDraw: an object's batches (triangles in the object's space, one material each, the GS registers
//   the batch's register block sets) with its object-to-screen matrix and the microprogram's clip volume;
// - MeshViewList: a view's objects as a game's chain sends them: the opaque ones in traversal order, then the alpha
//   ones from a depth-sorted list of buckets (the farthest bucket first, each bucket's newest first: a PS2 ordering
//   table built by prepending);
// - MeshSubmitter: the GS state followed in that order (a draw's context block, then each batch's writes), the
//   material made from it, the CPU clip against the clip volume (VU1 CLIP: outside where |x|, |y| or |z| > |w|; a
//   triangle partly outside is cut, its colour, texture coordinates and fog along the cut) and nothing behind the
//   camera, then Renderer::draw; and the live diagnostic toggles a port can read from a file while the game runs.
//
// docs/native_packs.md: what a port supplies (its hooks, its per-object EE state, where a view's chain is kicked).

#include "runtime/native/native_renderer.h"
#include "runtime/native/native_texture_cache.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ps2x::native
{
    // One batch of an object: its material as the strip program keeps it, and its triangles in the object's space.
    struct MeshBatch
    {
        uint64_t tex0 = 0;
        bool textured = false, blended = false, fogged = false;
        // GS registers the strip starting it set from the object's register block besides TEX0_1: ALPHA_1, TEST_1,
        // ZBUF_1.ZMSK (they persist into the objects after it, as in the GS).
        bool setsAlpha = false, setsTest = false, setsZbuf = false, setsTex0 = false;
        uint64_t alpha = 0, test = 0;
        bool depthMasked = false;
        std::shared_ptr<const TextureCache::Texture> texture;
        std::vector<Vertex> vertices; // triangles, three vertices each
    };

    // One object's draws, waiting for its place in the view's order.
    struct MeshDraw
    {
        float matrix[16] = {}; // object to GS screen (columns)
        // The microprograms' clip volume (object to clip space, columns): a triangle with a corner outside it
        // (|x|, |y| or |z| past |w|) is cut to it. hasClip: the object's program clips.
        float clip[16] = {};
        bool hasClip = false;
        bool screenSpace = false; // vertices already in screen space (sprites): no matrix, no clip; interpolated by
                                  // triangle (Renderer::drawTracked) when keyed
        uint64_t key = 0;         // Renderer::setKey
        uint64_t keyBase = 0;     // its base (Renderer::setKey): alike objects told apart by where they are
        // matrix = camera x object, for interpolation (Renderer::setParts): hasParts when the port knows them.
        bool hasParts = false;
        float partCamera[16] = {}, partObject[16] = {};
        std::vector<MeshBatch> batches;
        // The GS state a context block sets before the batches (ALPHA_1, TEST_1, ZBUF_1: its sets* fields).
        bool hasContext = false;
        MeshBatch context;
        uint32_t object = 0, file = 0, offset = 0; // the port's identity of the object (diagnostics)
    };

    // The GS registers the draws depend on, as context and register blocks leave them in submission order.
    struct MeshGsState
    {
        uint64_t alpha = 0x44, test = 0;
        bool depthMasked = false;
        void apply(const MeshBatch &writes)
        {
            if (writes.setsAlpha)
                alpha = writes.alpha;
            if (writes.setsTest)
                test = writes.test;
            if (writes.setsZbuf)
                depthMasked = writes.depthMasked;
        }
    };

    // A blended batch's blend from ALPHA_1: (A - 0) * C + Cd (B = 2, D = 1) adds, anything else is (Cs - Cd) * As + Cd.
    Material::Blend meshBlend(uint64_t alpha);

    // A view's objects in the order its chain sends them. D: MeshDraw or a port's type derived from it.
    template <typename D>
    struct MeshViewList
    {
        explicit MeshViewList(size_t bucketCount = 129) : buckets(bucketCount) {}
        std::vector<D> opaque;
        std::vector<std::vector<D>> buckets; // the alpha list, by depth bucket (the higher, the farther)
        bool pending = false; // an alpha object built, waiting for its bucket (the game's link call)
        D pendingAlpha;
        uint32_t frame = 0;

        bool empty() const
        {
            if (!opaque.empty())
                return false;
            for (const std::vector<D> &b : buckets)
                if (!b.empty())
                    return false;
            return true;
        }
        size_t size() const
        {
            size_t n = opaque.size();
            for (const std::vector<D> &b : buckets)
                n += b.size();
            return n;
        }
        void clear()
        {
            opaque.clear();
            for (std::vector<D> &b : buckets)
                b.clear();
            pending = false;
        }
        // The pending alpha object into its bucket (clamped to the list).
        void link(int bucket)
        {
            if (!pending || buckets.empty())
                return;
            bucket = bucket < 0 ? 0 : bucket >= static_cast<int>(buckets.size()) ? static_cast<int>(buckets.size()) - 1 : bucket;
            buckets[static_cast<size_t>(bucket)].push_back(std::move(pendingAlpha));
            pending = false;
        }
        // fn(const D &) in the chain's order: the opaque objects, then the buckets from the farthest, each bucket's
        // newest first.
        template <typename F>
        void forEachInOrder(F &&fn) const
        {
            for (const D &d : opaque)
                fn(d);
            for (size_t b = buckets.size(); b-- > 0;)
                for (size_t i = buckets[b].size(); i-- > 0;)
                    fn(buckets[b][i]);
        }
    };

    // Diagnostics a port reads from a file while the game runs (words in it): nodepth (no depth test), nofog, noclip
    // (no view clip: the port's), flat (each object untextured in a colour of its own), opaque (no blending), noatest,
    // notex, noguard (no CPU clip), dump (one frame's draws logged).
    struct MeshLiveToggles
    {
        bool noDepth = false, noFog = false, noClip = false, flat = false, opaque = false, noAtest = false, noTex = false, dump = false, noGuard = false;
        bool dumping = false; // this frame
        // Reads the words from the file (logs "[<tag>:live]" when they change).
        void read(const char *path, uint32_t frame, const char *tag);
    };

    // Cuts triangles (three vertices each) to the clip volume: the screen matrix's w > 1e-3 (nothing behind the
    // camera) and, with hasClip, the clip matrix's -w < x, y, z < w; colour, texture coordinates and fog interpolated
    // along the cuts. False when every vertex is inside every plane (out untouched); else out holds what is left.
    bool clipMeshTriangles(const Vertex *vertices, size_t count, const float matrix[16], const float clip[16], bool hasClip,
                           std::vector<Vertex> &out);

    class MeshSubmitter
    {
    public:
        // Told of each draw (after its context block applied) and each batch (after its writes applied) in
        // submission order: a port's checks.
        struct Observer
        {
            virtual ~Observer() = default;
            virtual void draw(const MeshDraw &) {}
            virtual void batch(const MeshBatch &, const MeshGsState &) {}
        };

        MeshGsState gs;
        MeshLiveToggles live;
        Observer *observer = nullptr;
        uint32_t (*fogColour)() = nullptr; // FOGCOL at each draw (0 without)
        const char *tag = "mesh";          // log prefix

        // One draw: its context block, then each batch's writes and its triangles to the Renderer.
        void submit(const MeshDraw &draw);
        template <typename D>
        void submitInOrder(const MeshViewList<D> &list)
        {
            list.forEachInOrder([this](const D &d) { submit(d); });
        }
    };
}

#endif
