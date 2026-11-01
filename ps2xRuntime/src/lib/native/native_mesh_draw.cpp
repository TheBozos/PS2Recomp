// The draws built from a mesh pack, submitted in the game's order (runtime/native/native_mesh_draw.h).

#include "runtime/native/native_mesh_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ps2x::native
{
    Material::Blend meshBlend(uint64_t alpha)
    {
        const uint32_t b = (alpha >> 2) & 3u, d = (alpha >> 6) & 3u;
        if (b == 2u && d == 1u)
            return Material::Blend::Additive; // (A - 0) * C + Cd
        return Material::Blend::Alpha;
    }

    void MeshLiveToggles::read(const char *path, uint32_t frame, const char *tag)
    {
        if (!path)
            return;
        std::string words;
        if (FILE *f = std::fopen(path, "rb"))
        {
            char buffer[256];
            const size_t n = std::fread(buffer, 1, sizeof(buffer), f);
            std::fclose(f);
            words.assign(buffer, n);
        }
        const MeshLiveToggles was = *this;
        noDepth = words.find("nodepth") != std::string::npos;
        noFog = words.find("nofog") != std::string::npos;
        noClip = words.find("noclip") != std::string::npos;
        flat = words.find("flat") != std::string::npos;
        opaque = words.find("opaque") != std::string::npos;
        noAtest = words.find("noatest") != std::string::npos;
        noTex = words.find("notex") != std::string::npos;
        noGuard = words.find("noguard") != std::string::npos;
        const bool dumpNow = words.find("dump") != std::string::npos;
        dumping = dumpNow && !dump;
        dump = dumpNow;
        if (std::memcmp(&was, this, sizeof(was)) != 0)
            std::fprintf(stderr, "[%s:live] frame %u: nodepth %d nofog %d noclip %d flat %d opaque %d noatest %d notex %d\n", tag, frame, noDepth, noFog,
                         noClip, flat, opaque, noAtest, noTex);
    }

    bool clipMeshTriangles(const Vertex *vertices, size_t count, const float matrix[16], const float clip[16], bool hasClip, std::vector<Vertex> &out)
    {
        const float *m = matrix, *c = clip;
        const int planeCount = hasClip ? 7 : 1;
        constexpr float kWMin = 1.0e-3f;
        // Plane p's distance (inside > 0): 0 the screen's w > kWMin, then the clip volume's w -+ x, y, z.
        const auto distance = [m, c](const Vertex &v, int plane)
        {
            if (plane == 0)
                return m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] - kWMin;
            const int axis = (plane - 1) >> 1;
            const float w = c[3] * v.x + c[7] * v.y + c[11] * v.z + c[15];
            const float a = c[axis] * v.x + c[4 + axis] * v.y + c[8 + axis] * v.z + c[12 + axis];
            return (plane & 1) ? w - a : w + a;
        };
        const auto lerp = [](const Vertex &a, const Vertex &b, float t)
        {
            Vertex o = a;
            o.x = a.x + t * (b.x - a.x), o.y = a.y + t * (b.y - a.y), o.z = a.z + t * (b.z - a.z);
            o.u = a.u + t * (b.u - a.u), o.v = a.v + t * (b.v - a.v), o.fog = a.fog + t * (b.fog - a.fog);
            uint32_t rgba = 0;
            for (int ch = 0; ch < 32; ch += 8)
            {
                const float ca = static_cast<float>((a.rgba >> ch) & 0xffu), cb = static_cast<float>((b.rgba >> ch) & 0xffu);
                rgba |= static_cast<uint32_t>(std::lround(ca + t * (cb - ca))) << ch;
            }
            o.rgba = rgba;
            return o;
        };
        bool change = false;
        for (size_t t = 0; t < count && !change; ++t)
            for (int p = 0; p < planeCount && !change; ++p)
                change = !(distance(vertices[t], p) > 0.0f);
        if (!change)
            return false;
        out.clear();
        for (size_t t = 0; t + 2u < count; t += 3u)
        {
            Vertex poly[10], next[10];
            int n = 3;
            for (int k = 0; k < 3; ++k)
                poly[k] = vertices[t + static_cast<size_t>(k)];
            for (int p = 0; p < planeCount && n >= 3; ++p)
            {
                float d[10];
                bool allIn = true;
                for (int k = 0; k < n; ++k)
                    allIn = (d[k] = distance(poly[k], p)) > 0.0f && allIn;
                if (allIn)
                    continue;
                int n2 = 0;
                for (int k = 0; k < n && n2 < 9; ++k)
                {
                    const int k1 = (k + 1) % n;
                    if (d[k] > 0.0f)
                        next[n2++] = poly[k];
                    if ((d[k] > 0.0f) != (d[k1] > 0.0f))
                        next[n2++] = lerp(poly[k], poly[k1], d[k] / (d[k] - d[k1]));
                }
                for (int k = 0; k < n2; ++k)
                    poly[k] = next[k];
                n = n2;
            }
            for (int f = 1; f + 1 < n; ++f)
            {
                out.push_back(poly[0]);
                out.push_back(poly[f]);
                out.push_back(poly[f + 1]);
            }
        }
        return true;
    }

    void MeshSubmitter::submit(const MeshDraw &drawn)
    {
        Renderer &renderer = Renderer::instance();
        renderer.setKey(drawn.screenSpace ? 0u : drawn.key, drawn.keyBase); // screen space: by triangle (drawTracked) instead
        if (!drawn.screenSpace && drawn.hasParts)
            renderer.setParts(drawn.partCamera, drawn.partObject);
        const bool tracked = drawn.screenSpace && drawn.key != 0u && renderer.interpolating();
        uint64_t batchIndex = 0;
        static const float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (drawn.hasContext)
            gs.apply(drawn.context);
        if (observer)
            observer->draw(drawn);
        const uint32_t fog = fogColour ? fogColour() : 0u;
        for (const MeshBatch &batch : drawn.batches)
        {
            gs.apply(batch);
            if (observer)
                observer->batch(batch, gs);
            if (batch.vertices.empty())
                continue;
            // The strips' clip volume (the microprograms' CLIP, MeshDraw::clip): a triangle wholly outside one of its
            // planes is not drawn, one partly outside is cut to it (its colour, texture coordinates and fog along the
            // cuts: VU1's new vertices there), so what is nearer than its near plane never shows. Then nothing behind
            // the camera (w <= 0) of what is left: cut there too.
            const std::vector<Vertex> *vertices = &batch.vertices;
            static thread_local std::vector<Vertex> s_kept;
            if (!drawn.screenSpace && !live.noGuard &&
                clipMeshTriangles(batch.vertices.data(), batch.vertices.size(), drawn.matrix, drawn.clip, drawn.hasClip, s_kept))
            {
                if (s_kept.empty())
                    continue;
                vertices = &s_kept;
            }
            Material material;
            material.textured = batch.textured && batch.texture != nullptr;
            material.tex0 = batch.tex0;
            material.blend = batch.blended ? meshBlend(gs.alpha) : Material::Blend::Opaque;
            material.depthWrite = !gs.depthMasked;
            const uint32_t atst = (gs.test >> 1) & 7u, aref = (gs.test >> 4) & 0xffu;
            if ((gs.test & 1u) && (atst == 5u || atst == 6u))
            {
                material.alphaTest = atst == 6u || aref > 0u;
                material.alphaRef = static_cast<uint8_t>(atst == 6u ? aref : aref - 1u);
            }
            material.fog = batch.fogged;
            material.fogColour = fog;
            if (live.noDepth)
                material.depthTest = false;
            if (live.noFog)
                material.fog = false;
            if (live.opaque)
                material.blend = Material::Blend::Opaque;
            if (live.noAtest)
                material.alphaTest = false;
            if (live.noTex)
                material.textured = false;
            if (live.flat)
            {
                material.textured = false;
                material.blend = Material::Blend::Opaque;
                std::vector<Vertex> tinted(*vertices);
                const uint32_t h = drawn.object * 2654435761u;
                const uint32_t colour = 0x80000000u | ((h >> 8) & 0x7f7f7fu) | 0x202020u;
                for (Vertex &v : tinted)
                    v.rgba = colour;
                renderer.draw(material, drawn.screenSpace ? kScreenSpace : drawn.matrix, tinted.data(), static_cast<uint32_t>(tinted.size()), nullptr);
                continue;
            }
            if (live.dumping)
            {
                uint32_t aMin = 255u, aMax = 0u;
                for (const Vertex &v : batch.vertices)
                    aMin = std::min(aMin, v.rgba >> 24), aMax = std::max(aMax, v.rgba >> 24);
                // Its triangles through the matrix: all in front (w > 0), some corner behind, all behind; the
                // screen box and GS Z range of the corners in front.
                const float *m = drawn.screenSpace ? kScreenSpace : drawn.matrix;
                uint32_t front = 0, mixed = 0, behind = 0;
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
                for (size_t t = 0; t + 2u < batch.vertices.size(); t += 3u)
                {
                    int in = 0;
                    for (size_t k = 0; k < 3u; ++k)
                    {
                        const Vertex &v = batch.vertices[t + k];
                        const float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
                        w0 = std::min(w0, w), w1 = std::max(w1, w);
                        if (w > 0.0f)
                        {
                            ++in;
                            const float x = (m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12]) / w, y = (m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13]) / w,
                                        z = (m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]) / w;
                            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y), z0 = std::min(z0, z), z1 = std::max(z1, z);
                        }
                    }
                    (in == 3 ? front : in == 0 ? behind : mixed) += 1u;
                }
                std::fprintf(stderr, "[%s:dump]   tris front %u mixed %u behind %u; w %.3g..%.3g; x %.0f..%.0f y %.0f..%.0f z %.4g..%.4g\n", tag, front, mixed,
                             behind, w0, w1, x0, x1, y0, y1, z0, z1);
                std::fprintf(stderr, "[%s:dump] obj %#x file %u off %#x: %zu verts blend %d alpha %#llx test %#llx atest %d ref %u zw %d tex %d found %d tex0 %#llx vA %u-%u fog %d\n",
                             tag, drawn.object, drawn.file, drawn.offset, batch.vertices.size(), static_cast<int>(material.blend),
                             static_cast<unsigned long long>(gs.alpha), static_cast<unsigned long long>(gs.test), material.alphaTest, material.alphaRef,
                             material.depthWrite, batch.textured, batch.texture != nullptr, static_cast<unsigned long long>(batch.tex0), aMin, aMax,
                             batch.fogged);
            }
            if (tracked)
            {
                // A keyed sprite's or ribbon's triangles, each by its place in the object (the same next frame).
                static thread_local std::vector<uint64_t> s_ids;
                s_ids.resize(vertices->size() / 3u);
                const uint64_t seed = (drawn.key ^ (++batchIndex * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;
                for (size_t t = 0; t < s_ids.size(); ++t)
                    s_ids[t] = (seed + t * 2u) | 1u;
                renderer.drawTracked(material, vertices->data(), static_cast<uint32_t>(vertices->size()),
                                     material.textured ? batch.texture : nullptr, s_ids.data());
                continue;
            }
            renderer.draw(material, drawn.screenSpace ? kScreenSpace : drawn.matrix, vertices->data(), static_cast<uint32_t>(vertices->size()),
                          material.textured ? batch.texture : nullptr);
        }
    }
}
