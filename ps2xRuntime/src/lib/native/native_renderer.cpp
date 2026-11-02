#include "runtime/native/native_renderer.h"
#include "runtime/native/native_movie.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ps2x::native
{
    namespace
    {
        // The object's matrix takes a vertex to PS2 screen space (x, y in GS window coordinates, z GS depth, w); the
        // PS2 divides by w. Here the same point goes to the window: NDC from the screen rectangle, depth from the Z
        // range (larger is nearer on the GS), and w kept so texture coordinates interpolate in perspective and the
        // GPU clips what lies behind the camera (w <= 0: where the PS2 relies on the microprogram's clipping).
        const char *kVertexShader = R"(#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
layout(location = 3) in float aFog;
uniform vec4 uM[4];
uniform vec4 uScreen;
uniform float uDepthMax;
out vec2 vUv;
out vec4 vCol;
out float vFog;
void main()
{
    vec4 c = uM[0] * aPos.x + uM[1] * aPos.y + uM[2] * aPos.z + uM[3];
    float w = c.w;
    vec2 s = c.xy / c.w;
    vec2 ndc = vec2((s.x - uScreen.x) / uScreen.z * 2.0 - 1.0, 1.0 - (s.y - uScreen.y) / uScreen.w * 2.0);
    // GS Z as XYZF2 holds it: past the Z buffer's range it wraps (24 bits kept), as on the GS, rather than clamps.
    float gz = c.z / c.w;
    gz -= (uDepthMax + 1.0) * floor(gz / (uDepthMax + 1.0));
    float z = clamp(gz / uDepthMax, 0.0, 1.0);
    gl_Position = vec4(ndc * w, (1.0 - 2.0 * z) * w, w);
    vUv = aUv;
    vCol = aCol;
    vFog = aFog;
}
)";

        // GS modulate: texel * vertex colour, where a vertex colour of 0x80 is 1.0, then fog toward the fog colour (RGB
        // only, before blending). The render target holds
        // premultiplied colour and coverage (opaque draws cover fully), so present() composites it over the window.
        const char *kFragmentShader = R"(#version 330
in vec2 vUv;
in vec4 vCol;
in float vFog;
uniform sampler2D uTex;
uniform int uTextured;
uniform int uAlphaTest;
uniform float uAlphaRef;
uniform int uOpaque;
uniform int uFog;
uniform vec3 uFogColour;
out vec4 fragColor;
void main()
{
    vec4 colour = vCol * (255.0 / 128.0);
    vec4 texel = uTextured != 0 ? texture(uTex, vUv) * vec4(1.0, 1.0, 1.0, 255.0 / 128.0) : vec4(1.0); // GS alpha
    fragColor = clamp(texel * colour, 0.0, 1.0);
    if (uAlphaTest != 0 && fragColor.a <= uAlphaRef)
        discard;
    if (uFog != 0)
        fragColor.rgb = mix(uFogColour, fragColor.rgb, clamp(vFog, 0.0, 1.0));
    if (uOpaque != 0)
        fragColor.a = 1.0;
}
)";

        constexpr int kGlUnsignedByte = 0x1401;
        constexpr int kGlFloat = 0x1406;
        constexpr int kGlSrcAlpha = 0x0302;
        constexpr int kGlOneMinusSrcAlpha = 0x0303;
        constexpr int kGlOne = 1;
        constexpr int kGlFuncAdd = 0x8006;

        // Interpolation's maths. Matrices are columns (m[4c + r]), as draw() takes them.
        void lerpMatrix(const float *a, const float *b, float t, float *out)
        {
            for (int i = 0; i < 16; ++i)
                out[i] = a[i] + (b[i] - a[i]) * t;
        }

        void multiply(const float *a, const float *b, float *out) // out = a x b
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }

        // The upper 3 x 3 as a rotation (unit quaternion w, x, y, z) times a scale per column; false when it is not
        // one (a shear, a mirror, a degenerate scale): such a matrix is interpolated element by element.
        bool rotationOf(const float *m, float q[4], float scale[3])
        {
            float r[3][3]; // r[column][row]
            for (int c = 0; c < 3; ++c)
            {
                const float *col = m + c * 4;
                scale[c] = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
                if (!(scale[c] > 1e-6f))
                    return false;
                for (int k = 0; k < 3; ++k)
                    r[c][k] = col[k] / scale[c];
            }
            const auto dot = [&](int a, int b) { return r[a][0] * r[b][0] + r[a][1] * r[b][1] + r[a][2] * r[b][2]; };
            if (std::fabs(dot(0, 1)) > 0.02f || std::fabs(dot(0, 2)) > 0.02f || std::fabs(dot(1, 2)) > 0.02f)
                return false;
            const float det = r[2][0] * (r[0][1] * r[1][2] - r[0][2] * r[1][1]) + r[2][1] * (r[0][2] * r[1][0] - r[0][0] * r[1][2]) +
                              r[2][2] * (r[0][0] * r[1][1] - r[0][1] * r[1][0]);
            if (det < 0.9f)
                return false;
            // R[row][col] = r[col][row].
            const float m00 = r[0][0], m11 = r[1][1], m22 = r[2][2];
            const float trace = m00 + m11 + m22;
            if (trace > 0.0f)
            {
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                q[0] = 0.25f * s;
                q[1] = (r[1][2] - r[2][1]) / s;
                q[2] = (r[2][0] - r[0][2]) / s;
                q[3] = (r[0][1] - r[1][0]) / s;
            }
            else if (m00 > m11 && m00 > m22)
            {
                const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                q[0] = (r[1][2] - r[2][1]) / s;
                q[1] = 0.25f * s;
                q[2] = (r[1][0] + r[0][1]) / s;
                q[3] = (r[2][0] + r[0][2]) / s;
            }
            else if (m11 > m22)
            {
                const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                q[0] = (r[2][0] - r[0][2]) / s;
                q[1] = (r[1][0] + r[0][1]) / s;
                q[2] = 0.25f * s;
                q[3] = (r[2][1] + r[1][2]) / s;
            }
            else
            {
                const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                q[0] = (r[0][1] - r[1][0]) / s;
                q[1] = (r[2][0] + r[0][2]) / s;
                q[2] = (r[2][1] + r[1][2]) / s;
                q[3] = 0.25f * s;
            }
            return true;
        }

        // An object's matrix between a and b: its rotation along the arc (slerp), its scale and translation
        // linearly; element by element when either is not a rotation and scale. angle: the rotation between them
        // (degrees; 0 when not known).
        void interpolateObject(const float *a, const float *b, float t, float *out, float &angle)
        {
            float qa[4], qb[4], sa[3], sb[3];
            angle = 0.0f;
            if (!rotationOf(a, qa, sa) || !rotationOf(b, qb, sb))
            {
                lerpMatrix(a, b, t, out);
                return;
            }
            float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            if (d < 0.0f)
            {
                d = -d;
                for (float &v : qb)
                    v = -v;
            }
            d = std::min(d, 1.0f);
            const float theta = std::acos(d);
            angle = theta * 2.0f * 57.29578f;
            float q[4];
            if (theta < 1e-3f)
                for (int i = 0; i < 4; ++i)
                    q[i] = qa[i] + (qb[i] - qa[i]) * t;
            else
            {
                const float s = std::sin(theta), wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
                for (int i = 0; i < 4; ++i)
                    q[i] = qa[i] * wa + qb[i] * wb;
            }
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (float &v : q)
                v /= n;
            const float w = q[0], x = q[1], y = q[2], z = q[3];
            const float rot[3][3] = {// rot[column][row]
                                     {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                                     {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                                     {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
            lerpMatrix(a, b, t, out); // the translation and the fourth row, linearly
            for (int c = 0; c < 3; ++c)
            {
                const float scale = sa[c] + (sb[c] - sa[c]) * t;
                for (int k = 0; k < 3; ++k)
                    out[c * 4 + k] = rot[c][k] * scale;
            }
        }

        // The turn between two matrices' rotations (degrees; 0 when either is not a rotation and scale).
        float turnBetween(const float *a, const float *b)
        {
            float qa[4], qb[4], sa[3], sb[3];
            if (!rotationOf(a, qa, sa) || !rotationOf(b, qb, sb))
                return 0.0f;
            const float d = std::min(std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]), 1.0f);
            return std::acos(d) * 2.0f * 57.29578f;
        }

        // An affine matrix's inverse (its fourth row 0, 0, 0, 1); false when singular.
        bool invertAffine(const float *m, float *out)
        {
            const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], k = m[10];
            const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
            if (std::fabs(det) < 1e-12f)
                return false;
            const float r = 1.0f / det;
            const float inv[9] = {(e * k - f * h) * r, (c * h - b * k) * r, (b * f - c * e) * r,
                                  (f * g - d * k) * r, (a * k - c * g) * r, (c * d - a * f) * r,
                                  (d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}; // rows
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    out[col * 4 + row] = inv[row * 3 + col];
            for (int row = 0; row < 3; ++row)
                out[12 + row] = -(inv[row * 3] * m[12] + inv[row * 3 + 1] * m[13] + inv[row * 3 + 2] * m[14]);
            out[3] = out[7] = out[11] = 0.0f;
            out[15] = 1.0f;
            return true;
        }

        void transformPoint(const float *m, const float *p, float *out)
        {
            for (int r = 0; r < 3; ++r)
                out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
        }

        // Where a matrix puts the object's origin on the screen (GS coordinates); false behind the camera.
        bool originOnScreen(const float *m, float &x, float &y)
        {
            if (!(m[15] > 1e-3f))
                return false;
            x = m[12] / m[15];
            y = m[13] / m[15];
            return true;
        }

        // PS2X_PRESENT_STATS while interpolating: what the window showed over the last two seconds.
        struct InterpolationStats
        {
            uint32_t presents = 0, holds = 0;
            uint64_t keyed = 0, lerped = 0, jumped = 0, spins = 0, ambiguous = 0, cuts = 0, tracked = 0, trackedLerped = 0, over10 = 0;
            double cpu = 0.0, cpuMax = 0.0, stillMax = 0.0, windowStart = 0.0, pictureSince = 0.0, gapMax = 0.0, lastPresent = 0.0;
            uint64_t picture = 0, firstSerial = 0;
            float maxAngle = 0.0f;
        };
        InterpolationStats &interpolationStats()
        {
            static InterpolationStats stats;
            return stats;
        }

        uint64_t viewOf(const float clip[4])
        {
            uint64_t h = 1469598103934665603ull;
            uint32_t words[4];
            std::memcpy(words, clip, sizeof(words));
            for (uint32_t w : words)
                h = (h ^ w) * 1099511628211ull;
            return h;
        }
    }

    // native_renderer_gs.cpp (raw GL): glDepthFunc(GL_LEQUAL), which rlgl does not expose (its default is GL_LESS).
    void glDepthFuncLequal();

    struct Renderer::Gpu
    {
        unsigned int shader = 0;
        int locM = -1, locScreen = -1, locDepthMax = -1, locTex = -1, locTextured = -1, locAlphaTest = -1, locAlphaRef = -1, locOpaque = -1, locFog = -1,
            locFogColour = -1;
        unsigned int vao = 0, vbo = 0;
        size_t vboBytes = 0;
        uint64_t uploadedSerial = 0; // the frame the vertex buffer holds (interpolating: uploaded once)
        unsigned int uploadedVbo = 0;
        bool trackedMoved = false;   // its tracked triangles were moved since
        RenderTexture2D target{};
        int targetWidth = 0, targetHeight = 0;
        bool targetFiltered = false; // bilinear (render scale other than 1) rather than point
        struct GpuTexture
        {
            unsigned int id = 0;
            std::shared_ptr<const TextureCache::Texture> source; // held, so its address is not reused while compared
        };
        std::unordered_map<uint64_t, GpuTexture> textures;
        // The overlay's textures, their alpha from the GS's (0x80 is 1.0) to 255: raylib's own shader draws them.
        // By the texture's key when it has one (a picture that changes: one slot, its image replaced), else its address.
        std::unordered_map<uint64_t, GpuTexture> overlayTextures;
        unsigned int displayPicture = 0; // the display's circuit 2 picture (ps2x::native::displayPicture), its version
        uint64_t displayPictureVersion = 0;
        unsigned int overlayTexture(const std::shared_ptr<const TextureCache::Texture> &source)
        {
            GpuTexture &slot = overlayTextures[source->key != 0u ? source->key : reinterpret_cast<uintptr_t>(source.get())];
            if (slot.id == 0 || slot.source != source)
            {
                if (slot.id != 0)
                    rlUnloadTexture(slot.id);
                std::vector<uint32_t> rgba(source->rgba);
                for (uint32_t &c : rgba)
                    c = (c & 0xffffffu) | (std::min<uint32_t>(255u, ((c >> 24) * 255u + 64u) / 128u) << 24);
                slot.id = rlLoadTexture(rgba.data(), static_cast<int>(source->width), static_cast<int>(source->height),
                                        RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                rlTextureParameters(slot.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                rlTextureParameters(slot.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                rlTextureParameters(slot.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
                rlTextureParameters(slot.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
                slot.source = source;
            }
            return slot.id;
        }

        bool init()
        {
            shader = rlLoadShaderCode(kVertexShader, kFragmentShader);
            if (shader == 0)
                return false;
            locM = rlGetLocationUniform(shader, "uM");
            locScreen = rlGetLocationUniform(shader, "uScreen");
            locDepthMax = rlGetLocationUniform(shader, "uDepthMax");
            locTex = rlGetLocationUniform(shader, "uTex");
            locTextured = rlGetLocationUniform(shader, "uTextured");
            locAlphaTest = rlGetLocationUniform(shader, "uAlphaTest");
            locAlphaRef = rlGetLocationUniform(shader, "uAlphaRef");
            locOpaque = rlGetLocationUniform(shader, "uOpaque");
            locFog = rlGetLocationUniform(shader, "uFog");
            locFogColour = rlGetLocationUniform(shader, "uFogColour");
            vao = rlLoadVertexArray();
            return true;
        }

        unsigned int texture(uint64_t key, const std::shared_ptr<const TextureCache::Texture> &source)
        {
            GpuTexture &slot = textures[key];
            if (slot.source != source || slot.id == 0)
            {
                if (slot.id != 0)
                    rlUnloadTexture(slot.id);
                slot.id = rlLoadTexture(source->rgba.data(), static_cast<int>(source->width), static_cast<int>(source->height),
                                        RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                rlTextureParameters(slot.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                rlTextureParameters(slot.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                rlTextureParameters(slot.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT);
                rlTextureParameters(slot.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_REPEAT);
                slot.source = source;
            }
            return slot.id;
        }
    };

    Renderer::Renderer() = default;
    Renderer::~Renderer() = default;

    Material::Blend blendOf(uint64_t alpha)
    {
        const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
        if (a == b && d == 0u)
            return Material::Blend::Opaque;
        if (a == 0u && b == 2u && d == 1u)
            return c == 1u ? Material::Blend::AdditiveByDestination : Material::Blend::Additive;
        return Material::Blend::Alpha;
    }

    Renderer &Renderer::instance()
    {
        static Renderer renderer;
        return renderer;
    }

    void Renderer::beginFrame(const Screen &screen)
    {
        startBuilding(screen);
    }

    void Renderer::startBuilding(const Screen &screen)
    {
        std::vector<Vertex> vertices = std::move(m_building.vertices);
        std::vector<Batch> batches = std::move(m_building.batches);
        if (vertices.capacity() == 0u || batches.capacity() == 0u)
        {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            if (vertices.capacity() == 0u && !m_vertexPool.empty())
            {
                vertices = std::move(m_vertexPool.back());
                m_vertexPool.pop_back();
            }
            if (batches.capacity() == 0u && !m_batchPool.empty())
            {
                batches = std::move(m_batchPool.back());
                m_batchPool.pop_back();
            }
        }
        vertices.clear();
        batches.clear();
        m_building = Frame{};
        m_building.screen = screen;
        m_building.vertices = std::move(vertices);
        m_building.batches = std::move(batches);
    }

    void Renderer::draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count)
    {
        draw(material, matrix, vertices, count, nullptr);
    }

    void Renderer::draw(const Material &material, const float matrix[16], const Vertex *vertices, uint32_t count,
                        std::shared_ptr<const TextureCache::Texture> texture)
    {
        if (count == 0u)
            return;
        Batch batch;
        batch.material = material;
        if (!material.fromGsContext && material.texa == 0u)
            batch.material.texa = m_gsTexa.load(std::memory_order_relaxed); // the GS's (setGsTexa)
        std::memcpy(batch.matrix, matrix, sizeof(batch.matrix));
        // With stream draws, the texture is decoded where the batch is drawn in the GS stream (the GS thread, at its
        // marker, with the uploads before it in the stream: setUploadsInStreamOrder) rather than now: the game's
        // texture and CLUT uploads reach GS memory on the GS thread, behind this one (TMB rewrites a CLUT ring every
        // frame). PS2X_NATIVE_DECODE_AT_DRAW=0: decoded now.
        static const bool s_atDraw = []
        {
            const char *value = std::getenv("PS2X_NATIVE_DECODE_AT_DRAW");
            return !(value && *value == '0');
        }();
        if (material.textured && !material.fromGsContext && !(s_atDraw && m_streamDraws))
            batch.texture = ps2x::native::textures().texture(material.tex0, batch.material.texa, material.region);
        if (texture)
            batch.texture = std::move(texture);
        batch.first = static_cast<uint32_t>(m_building.vertices.size());
        batch.count = count;
        batch.key = m_key;
        batch.base = m_key != 0u ? m_keyBase : 0u;
        if (m_key != 0u && m_hasParts)
        {
            batch.hasParts = true;
            std::memcpy(batch.camera, m_camera, sizeof(batch.camera));
            std::memcpy(batch.object, m_object, sizeof(batch.object));
            // PS2X_INTERP_CHECK=1: the parts against the matrix (their product must be it), every 5000 batches.
            static const bool s_check = std::getenv("PS2X_INTERP_CHECK") != nullptr;
            if (s_check)
            {
                static uint64_t s_batches = 0, s_bad = 0;
                static float s_worst = 0.0f;
                float product[16], scale = 0.0f, error = 0.0f;
                multiply(m_camera, m_object, product);
                for (int i = 0; i < 16; ++i)
                {
                    scale = std::max(scale, std::fabs(matrix[i]));
                    error = std::max(error, std::fabs(product[i] - matrix[i]));
                }
                const float relative = scale > 0.0f ? error / scale : 0.0f;
                s_worst = std::max(s_worst, relative);
                s_bad += relative > 1e-4f ? 1u : 0u;
                if (++s_batches % 5000u == 0u)
                {
                    std::fprintf(stderr, "[interp-check] %llu batches with parts, %llu off by over 1e-4 of the matrix, worst %.3g\n",
                                 static_cast<unsigned long long>(s_batches), static_cast<unsigned long long>(s_bad), s_worst);
                }
            }
        }
        std::memcpy(batch.clip, m_clip, sizeof(batch.clip));
        batch.group = m_streamDraws ? m_group : 0u;
        m_building.lastGroup = std::max(m_building.lastGroup, batch.group);
        // Once the GS front end draws the GS-context batches itself (drawn into the GS frame), present() never draws
        // the frame: such a batch only goes to the stream.
        const bool frameCopy = !(m_streamDraws && material.fromGsContext && m_drewIntoGs.load(std::memory_order_relaxed));
        if (frameCopy)
            m_building.vertices.insert(m_building.vertices.end(), vertices, vertices + count);
        if (m_streamDraws)
        {
            StreamBatch entry;
            entry.group = m_group;
            entry.gsZScale = m_building.screen.gsZScale;
            entry.batch = batch;
            entry.vertices.assign(vertices, vertices + count);
            std::lock_guard<std::mutex> lock(m_streamMutex);
            m_stream.push_back(std::move(entry));
            if (m_stream.size() > 65536u) // nobody drains them (no GS markers): keep the latest frames' worth
                m_stream.pop_front();
        }
        if (frameCopy)
            m_building.batches.push_back(batch);
    }

    void Renderer::drawTracked(const Material &material, const Vertex *vertices, uint32_t count,
                               std::shared_ptr<const TextureCache::Texture> texture, const uint64_t *ids)
    {
        static constexpr float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const size_t before = m_building.batches.size();
        const uint64_t key = m_key;
        const bool parts = m_hasParts;
        m_key = 0; // screen space: no matrix to interpolate
        m_hasParts = false;
        draw(material, kScreenSpace, vertices, count, std::move(texture));
        m_key = key;
        m_hasParts = parts;
        if (ids && m_building.batches.size() == before + 1u)
        {
            Batch &batch = m_building.batches.back();
            batch.idFirst = static_cast<int32_t>(m_building.triangleIds.size());
            m_building.triangleIds.insert(m_building.triangleIds.end(), ids, ids + count / 3u);
        }
    }

    void Renderer::drawOverlay(const Material &material, const Vertex *vertices, uint32_t count,
                               std::shared_ptr<const TextureCache::Texture> texture)
    {
        if (count == 0u || m_overlayBuilding.vertices.size() > (1u << 20))
            return; // (never published: a port that does not publish keeps the latest million vertices' worth out)
        Batch batch;
        batch.material = material;
        batch.material.depthTest = false;
        batch.material.depthWrite = false;
        static constexpr float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(batch.matrix, kScreenSpace, sizeof(batch.matrix));
        batch.texture = std::move(texture);
        batch.first = static_cast<uint32_t>(m_overlayBuilding.vertices.size());
        batch.count = count;
        m_overlayBuilding.vertices.insert(m_overlayBuilding.vertices.end(), vertices, vertices + count);
        m_overlayBuilding.batches.push_back(batch);
    }

    // The overlay onto the window (raylib's immediate mode over what is there): GS coordinates to the window
    // rectangle, colours from the GS's (0x80 is 1.0), in submission order.
    void Renderer::drawOverlay(const Frame &overlay, float x, float y, float width, float height)
    {
        const Screen &screen = overlay.screen;
        const float sx = width / screen.width, sy = height / screen.height;
        rlDrawRenderBatchActive();
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        const auto scale = [](uint32_t c) { return static_cast<unsigned char>(std::min<uint32_t>(255u, (c * 255u + 64u) / 128u)); };
        // Everything in rlgl's quad mode (a sprite's two triangles one quad, any other triangle a quad with its last corner
        // twice), the texture set before
        // rlBegin (as raylib's own textured draws do). rlgl gives every draw it opens or resets the quad mode: a texture
        // change opens a new draw in that mode, and a flush (rlgl's 256 draws, its vertex buffer) leaves it. With
        // triangles, the draw a texture change opened kept the quad mode: glyphs after HUD shapes were drawn as quads,
        // sheared and stacked down the screen.
        for (const Batch &batch : overlay.batches)
        {
            const Material &m = batch.material;
            BeginBlendMode(m.blend == Material::Blend::Additive || m.blend == Material::Blend::AdditiveByDestination ? BLEND_ADDITIVE
                                                                                                                   : BLEND_ALPHA);
            rlSetTexture(m.textured && batch.texture ? m_gpu->overlayTexture(batch.texture) : rlGetTextureIdDefault());
            rlBegin(RL_QUADS);
            const auto put = [&](const Vertex &v)
            {
                rlColor4ub(scale(v.rgba & 0xffu), scale((v.rgba >> 8) & 0xffu), scale((v.rgba >> 16) & 0xffu),
                           m.blend == Material::Blend::Opaque ? 255 : scale(v.rgba >> 24));
                rlTexCoord2f(v.u, v.v);
                rlVertex2f(x + (v.x - screen.originX) * sx, y + (v.y - screen.originY) * sy);
            };
            const auto same = [](const Vertex &a, const Vertex &b)
            { return a.x == b.x && a.y == b.y && a.u == b.u && a.v == b.v && a.rgba == b.rgba; };
            const uint32_t end = batch.first + batch.count;
            for (uint32_t i = batch.first; i + 3u <= end;)
            {
                const Vertex *t = &overlay.vertices[i];
                if (i + 6u <= end && same(t[3], t[1]) && same(t[5], t[2]))
                {
                    // A sprite, a glyph, a line: (a, b, c) (b, e, c), the quad a, b, e, c.
                    put(t[0]);
                    put(t[1]);
                    put(t[4]);
                    put(t[2]);
                    i += 6u;
                    continue;
                }
                put(t[0]);
                put(t[1]);
                put(t[2]);
                put(t[2]);
                i += 3u;
            }
            rlEnd();
            EndBlendMode();
        }
        rlSetTexture(0);
        rlDrawRenderBatchActive();
    }

    void Renderer::publishOverlay()
    {
        publishOverlay(m_building.screen);
    }

    void Renderer::publishOverlay(const Screen &screen)
    {
        auto overlay = std::make_shared<Frame>(std::move(m_overlayBuilding));
        m_overlayBuilding = Frame{};
        // PS2X_NATIVE_STATS=1: the overlay's batches every 250 publishes.
        static const bool s_stats = std::getenv("PS2X_NATIVE_STATS") != nullptr;
        static uint32_t s_publishes = 0;
        if (s_stats && ++s_publishes % 250u == 0u)
            std::fprintf(stderr, "[native] overlay publish %u: %zu batches, %zu vertices\n", s_publishes, overlay->batches.size(),
                         overlay->vertices.size());
        overlay->screen = screen;
        overlay->time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overlayPublished = std::move(overlay);
    }

    void Renderer::setBackdrop(std::shared_ptr<const TextureCache::Texture> picture, const Screen &screen, float x, float y,
                               float width, float height, bool held)
    {
        auto frame = std::make_shared<Frame>();
        frame->screen = screen;
        frame->time = held ? 1e300 : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        Batch batch;
        batch.material.textured = true;
        batch.material.blend = Material::Blend::Opaque;
        batch.material.depthTest = false;
        batch.material.depthWrite = false;
        batch.texture = std::move(picture);
        batch.first = 0;
        batch.count = 6;
        const float xs[2] = {x, x + width}, ys[2] = {y, y + height};
        Vertex corners[4];
        for (int i = 0; i < 4; ++i)
        {
            corners[i].x = xs[i & 1];
            corners[i].y = ys[i >> 1];
            corners[i].u = static_cast<float>(i & 1);
            corners[i].v = static_cast<float>(i >> 1);
            corners[i].rgba = 0x80808080u;
            corners[i].fog = 1.0f;
        }
        frame->vertices = {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]};
        frame->batches.push_back(batch);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_backdrop = std::move(frame);
            ++m_publishes; // a movie's picture: shown when it comes (waitForFrame), as a game frame is
        }
        m_publishedChanged.notify_all();
    }

    void Renderer::clearBackdrop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backdrop.reset();
    }

    void Renderer::endFrame()
    {
        // Its arrays go back to the pool when the present thread lets the frame go (cleared there: the textures the
        // batches hold are released with them, as before).
        std::shared_ptr<Frame> frame(new Frame(std::move(m_building)), [this](Frame *done)
                                     {
                                         done->vertices.clear();
                                         done->batches.clear();
                                         {
                                             std::lock_guard<std::mutex> lock(m_poolMutex);
                                             if (m_vertexPool.size() < 4u && done->vertices.capacity() != 0u)
                                                 m_vertexPool.push_back(std::move(done->vertices));
                                             if (m_batchPool.size() < 4u && done->batches.capacity() != 0u)
                                                 m_batchPool.push_back(std::move(done->batches));
                                         }
                                         delete done; });
        startBuilding(Screen{});
        m_key = 0;
        frame->time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // The keys only serve the frames drawn between the game's (matchFrames, the traces): a map entry per object
        // each frame otherwise.
        const bool interpolate = frameRate() != 0;
        if (interpolate)
            for (uint32_t i = 0; i < frame->batches.size(); ++i)
                if (frame->batches[i].key != 0u)
                    frame->keys.emplace(frame->batches[i].key, i);
        if (m_streamDraws && frame->lastGroup != 0u)
        {
            std::lock_guard<std::mutex> lock(m_probeMutex);
            m_frameLastGroups.push_back(frame->lastGroup);
            if (m_frameLastGroups.size() > 64u)
                m_frameLastGroups.pop_front();
        }
        if (interpolate)
            for (const auto &[key, index] : frame->keys)
                frame->bases[frame->batches[index].base].push_back(index);
        if (interpolate)
            for (const Batch &batch : frame->batches)
                if (batch.idFirst >= 0)
                    for (uint32_t i = 0; i < batch.count / 3u; ++i)
                        if (const uint64_t id = frame->triangleIds[static_cast<size_t>(batch.idFirst) + i]; id != 0u)
                            frame->tracked.emplace(id, batch.first + 3u * i);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            frame->serial = ++m_serial;
            if (interpolate)
            {
                // Its place on the window's clock: the publishing times smoothed (the game publishes a frame when its
                // work is done, a few milliseconds early or late against its 60 Hz), so the frames shown between are
                // evenly spaced. After a pause (a load) or a hitch the clock starts again from this frame.
                const double t = frame->time;
                const std::shared_ptr<Frame> last = m_history.empty() ? nullptr : m_history.back();
                if (last && t > last->time && t - last->time < 0.25)
                {
                    m_interval += (std::clamp(t - last->time, 1.0 / 250.0, 1.0 / 15.0) - m_interval) * 0.05;
                    const double predicted = last->pts + m_interval;
                    double pts = predicted + (t - predicted) * 0.1;
                    if (std::fabs(t - pts) > 4.0 * m_interval)
                        pts = t;
                    frame->pts = std::max(pts, last->pts + 0.25 * m_interval);
                    frame->continuous = true;
                    m_late = std::max(t - frame->pts, m_late - 0.0001);
                }
                else
                {
                    frame->pts = t;
                    frame->continuous = false;
                    m_late = 0.0;
                }
                m_history.push_back(frame);
                while (m_history.size() > 6u)
                    m_history.pop_front();
            }
            else if (!m_history.empty())
                m_history.clear();
            m_previous = std::move(m_published);
            m_published = std::move(frame);
            ++m_publishes;
        }
        m_publishedChanged.notify_all();
    }

    bool Renderer::pickInterpolated(std::shared_ptr<Frame> &from, std::shared_ptr<Frame> &to, float &alpha, double now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_history.empty())
            return false;
        // The window is a game frame (and the frames' recent lateness) behind the newest: the clock's time now lies
        // between two published frames, drawn that far between them.
        const double delay = m_interval + std::clamp(m_late, 0.0, 0.025) + 0.002;
        const double t = now - delay;
        const size_t n = m_history.size();
        alpha = 1.0f;
        from = nullptr;
        if (t >= m_history[n - 1]->pts)
        {
            to = m_history[n - 1];
            if (n >= 2 && to->continuous)
                from = m_history[n - 2];
            return true;
        }
        size_t i = n - 1;
        while (i > 0 && m_history[i - 1]->pts > t)
            --i;
        to = m_history[i];
        if (i == 0 || !to->continuous)
            return true;
        from = m_history[i - 1];
        const double span = to->pts - from->pts;
        alpha = span > 0.0 ? static_cast<float>(std::clamp((t - from->pts) / span, 0.0, 1.0)) : 1.0f;
        return true;
    }

    bool Renderer::waitForFrame(uint64_t &seen, double timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool fresh = m_publishedChanged.wait_for(lock, std::chrono::duration<double>(timeout), [&] { return m_publishes != seen; });
        seen = m_publishes;
        return fresh;
    }

    bool Renderer::hasDraws()
    {
        if (m_drewIntoGs.load(std::memory_order_relaxed))
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_published && !m_published->batches.empty();
    }

    // native_renderer_gs.cpp (raw GL): the bound framebuffer's (width x height) depth in the given tiles of tile x tile
    // pixels (top-down, key ty << 32 | tx), each tile's floats top-down, 1 past the framebuffer's edge, from the call
    // before this one (false when there is none yet, or it was for another size).
    bool readFramebufferDepthTiles(int width, int height, int tile, const std::vector<uint64_t> &tiles,
                                   std::unordered_map<uint64_t, std::vector<float>> &out);

    namespace
    {
        double secondsNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    }

    void Renderer::wantDepth() { m_depthWantedAt.store(secondsNow(), std::memory_order_relaxed); }

    bool Renderer::depthAt(float gsX, float gsY, uint32_t &gsZ)
    {
        if (m_streamDraws) // the frame is in the GS's targets: a probe, read there after the frame (one frame late)
        {
            const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(gsX)) << 16) | static_cast<uint32_t>(gsY);
            std::lock_guard<std::mutex> lock(m_probeMutex);
            m_probeWanted[key] = 0u;
            const auto it = m_probeResults.find(key);
            if (it == m_probeResults.end())
                return false;
            gsZ = it->second;
            return true;
        }
        std::lock_guard<std::mutex> lock(m_depthMutex);
        if (m_depthWidth <= 0 || m_depthScreen.width <= 0.0f || m_depthScreen.height <= 0.0f)
            return false;
        const int x = static_cast<int>((gsX - m_depthScreen.originX) / m_depthScreen.width * static_cast<float>(m_depthWidth));
        const int y = static_cast<int>((gsY - m_depthScreen.originY) / m_depthScreen.height * static_cast<float>(m_depthHeight));
        if (x < 0 || y < 0 || x >= m_depthWidth || y >= m_depthHeight)
            return false;
        // Its tile and the ones around it are read from now on (what moves a little by the next frame is still read).
        const int tx = x / kDepthTile, ty = y / kDepthTile;
        const double now = secondsNow();
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (tx + dx >= 0 && ty + dy >= 0 && (tx + dx) * kDepthTile < m_depthWidth && (ty + dy) * kDepthTile < m_depthHeight)
                    m_depthTilesWanted[static_cast<uint64_t>(ty + dy) << 32 | static_cast<uint32_t>(tx + dx)] = now;
        const auto tile = m_depthTiles.find(static_cast<uint64_t>(ty) << 32 | static_cast<uint32_t>(tx));
        if (tile == m_depthTiles.end())
            return false; // not read yet
        // present()'s depth: window depth 1 - (z / w) / depthMax (cleared to 1, far), GS Z = z / w * gsZScale.
        const float d = tile->second[static_cast<size_t>(y - ty * kDepthTile) * kDepthTile + static_cast<size_t>(x - tx * kDepthTile)];
        const double z = (1.0 - static_cast<double>(d)) * m_depthScreen.depthMax * m_depthScreen.gsZScale;
        gsZ = static_cast<uint32_t>(std::clamp(z, 0.0, 4294967295.0));
        return true;
    }

    void Renderer::matchFrames(const Frame &from, const Frame &to)
    {
        Match &m = m_match;
        m.from = from.serial;
        m.to = to.serial;
        const size_t n = to.batches.size();
        m.previous.assign(n, -1);
        m.kind.assign(n, kUnmatched);
        m.cut.assign(n, 0u);
        m.cutViews.clear();
        InterpolationStats &stats = interpolationStats();
        // A teleport: the origin moved more than 30% of the screen's width, or, in the camera's space, by more than half
        // its distance (a respawn, the next of several alike objects). A cut: in a view (a split screen's: its clip),
        // 60% or more of the matched objects teleported or moved over 7.5% of the width (the camera cut away).
        const float jumpPixels = 0.3f * to.screen.width, cutPixels = 0.075f * to.screen.width;
        struct View
        {
            uint32_t matched = 0, moved = 0;
        };
        std::unordered_map<uint64_t, View> views;
        std::unordered_map<uint32_t, bool> used; // the older frame's objects (first batches) taken
        const auto translation = [](const Batch &batch, float t[3])
        {
            if (batch.hasParts)
                for (int k = 0; k < 3; ++k)
                    t[k] = batch.object[12 + k];
            else // the origin on the screen and its depth
            {
                const float w = std::fabs(batch.matrix[15]) > 1e-3f ? batch.matrix[15] : 1e-3f;
                t[0] = batch.matrix[12] / w;
                t[1] = batch.matrix[13] / w;
                t[2] = batch.matrix[15];
            }
        };
        static const bool s_log = std::getenv("PS2X_INTERP_LOG") != nullptr;
        static uint32_t s_lines = 0;
        // How a matched object is drawn between the two frames (teleport, spin or between), counted for its view.
        const auto classify = [&](size_t i, int32_t match)
        {
            const Batch &b = to.batches[i];
            used[static_cast<uint32_t>(match)] = true;
            const Batch &p = from.batches[static_cast<size_t>(match)];
            const bool parts = b.hasParts && p.hasParts;
            // The origin's move on the screen, where it says something: in front of the camera, not near its plane (a
            // large object's origin beside the camera projects anywhere).
            float moved = 0.0f, ax, ay, bx, by, step = 0.0f, distance = 0.0f;
            if (parts)
            {
                float ta[3], tb[3];
                translation(p, ta);
                translation(b, tb);
                step = std::hypot(ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]);
                distance = std::max(std::hypot(ta[0], ta[1], ta[2]), std::hypot(tb[0], tb[1], tb[2]));
            }
            const float nearPlane = parts ? 0.2f * distance : 1e-3f;
            if (p.matrix[15] > nearPlane && b.matrix[15] > nearPlane && originOnScreen(p.matrix, ax, ay) && originOnScreen(b.matrix, bx, by))
                moved = std::hypot(bx - ax, by - ay);
            // With the parts, a teleport is told in the camera's space (a step over half the distance); without, on the
            // screen.
            const bool teleport = parts ? step > 0.5f * distance + 1.0f : moved > jumpPixels;
            uint8_t kind = kLerp;
            float angle = 0.0f;
            if (parts)
            {
                angle = turnBetween(p.object, b.object);
                stats.maxAngle = std::max(stats.maxAngle, angle);
                stats.over10 += angle > 10.0f ? 1u : 0u;
                if (angle > 90.0f)
                    kind = kSpin;
            }
            if (teleport)
                kind = kTeleport;
            stats.jumped += teleport ? 1u : 0u;
            stats.spins += kind == kSpin ? 1u : 0u;
            // PS2X_INTERP_LOG=1: the first teleports and spins (key, move, turn, depth before and after).
            static uint32_t s_seen = 0;
            if (s_log && kind != kLerp && (++s_seen <= 100u || s_seen % 25u == 0u) && s_lines < 2000u)
            {
                ++s_lines;
                std::fprintf(stderr, "[interp-jump] frame %llu %s%s key %016llx moved %.1f px turned %.1f deg w %.2f -> %.2f, camera space step %.2f at %.2f\n",
                             static_cast<unsigned long long>(to.serial), teleport ? "teleport" : "spin", !parts && teleport ? " (screen)" : "",
                             static_cast<unsigned long long>(b.key), moved, angle, p.matrix[15], b.matrix[15], step, distance);
            }
            m.previous[i] = match;
            m.kind[i] = kind;
            View &view = views[viewOf(b.clip)];
            ++view.matched;
            view.moved += teleport || moved > cutPixels ? 1u : 0u;
        };
        // First the objects alone of their kind in their view (a split screen's: its clip): matched as they are. The
        // alike ones (the same model placed several times, no instance of its own) wait for the camera's move.
        std::vector<size_t> alike;
        for (size_t i = 0; i < n; ++i)
        {
            const Batch &b = to.batches[i];
            if (b.key == 0u)
                continue;
            if (const auto own = to.keys.find(b.key); own != to.keys.end() && own->second != i)
                continue; // the object's next batch (below)
            const auto group = from.bases.find(b.base);
            if (group == from.bases.end())
                continue; // new: where it is now
            const uint64_t view = viewOf(b.clip);
            int32_t only = -1;
            uint32_t count = 0;
            for (const uint32_t candidate : group->second)
                if (viewOf(from.batches[candidate].clip) == view)
                {
                    only = static_cast<int32_t>(candidate);
                    ++count;
                }
            if (count == 1u)
                classify(i, only);
            else if (count > 1u)
                alike.push_back(i);
        }
        // The camera's move in each view, from the objects alone of their kind: an object fixed in the world goes from
        // A to A' = D A in the camera's space (D: the camera's move); D taken from the object most others agree with
        // (the scenery), none when too few do (a cut, a view of moving cars).
        struct Move
        {
            bool known = false;
            float d[16];
        };
        std::unordered_map<uint64_t, Move> moves;
        if (!alike.empty())
        {
            std::unordered_map<uint64_t, std::vector<size_t>> unique; // view -> objects matched alone, between
            for (size_t i = 0; i < n; ++i)
                if (m.kind[i] == kLerp && to.batches[i].hasParts && from.batches[static_cast<size_t>(m.previous[i])].hasParts &&
                    to.keys.find(to.batches[i].key)->second == i)
                    unique[viewOf(to.batches[i].clip)].push_back(i);
            for (auto &[view, list] : unique)
            {
                if (list.size() < 4u)
                    continue;
                const size_t tries = std::min<size_t>(list.size(), 12u), checks = std::min<size_t>(list.size(), 40u);
                uint32_t bestScore = 0;
                Move &move = moves[view];
                for (size_t t = 0; t < tries; ++t)
                {
                    const size_t i = list[t * list.size() / tries];
                    const Batch &b = to.batches[i], &p = from.batches[static_cast<size_t>(m.previous[i])];
                    float inverse[16], d[16];
                    if (!invertAffine(p.object, inverse))
                        continue;
                    multiply(b.object, inverse, d);
                    uint32_t score = 0;
                    for (size_t c = 0; c < checks; ++c)
                    {
                        const size_t j = list[c * list.size() / checks];
                        const Batch &bj = to.batches[j], &pj = from.batches[static_cast<size_t>(m.previous[j])];
                        float predicted[3];
                        transformPoint(d, pj.object + 12, predicted);
                        const float error = std::hypot(predicted[0] - bj.object[12], predicted[1] - bj.object[13], predicted[2] - bj.object[14]);
                        score += error < 0.02f * std::hypot(bj.object[12], bj.object[13], bj.object[14]) + 0.5f ? 1u : 0u;
                    }
                    if (score > bestScore)
                    {
                        bestScore = score;
                        std::memcpy(move.d, d, sizeof(d));
                    }
                }
                move.known = bestScore >= 3u && bestScore * 2u >= checks;
            }
        }
        // The alike ones: the nearest of the older frame's in the same view, where it was either moved by the camera's
        // move (the scenery) or not (riding with the camera: a car's parts under a chase camera), whichever tells them
        // apart better. Ambiguous when another one lies within twice that distance (a row of alike objects passing
        // about one spacing a frame: the nearest is the wrong one, the row would seem to run backwards): drawn where it
        // is now, as an unmatched one.
        for (const size_t i : alike)
        {
            const Batch &b = to.batches[i];
            float tb[3];
            translation(b, tb);
            const uint64_t view = viewOf(b.clip);
            const auto group = from.bases.find(b.base);
            const auto found = moves.find(view);
            const Move *move = found != moves.end() && found->second.known && b.hasParts ? &found->second : nullptr;
            int32_t choice = -1;
            float choiceRatio = 0.0f;
            for (int way = 0; way < (move ? 2 : 1); ++way)
            {
                int32_t match = -1;
                float best = 0.0f, second = -1.0f;
                for (const uint32_t candidate : group->second)
                {
                    const Batch &c = from.batches[candidate];
                    if (viewOf(c.clip) != view)
                        continue;
                    float ta[3];
                    translation(c, ta);
                    if (way == 1 && c.hasParts)
                    {
                        float moved[3];
                        transformPoint(move->d, c.object + 12, moved);
                        std::memcpy(ta, moved, sizeof(ta));
                    }
                    const float d = std::hypot(ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]);
                    if (used.count(candidate))
                    {
                        if (second < 0.0f || d < second)
                            second = d;
                        continue;
                    }
                    if (match < 0 || d < best)
                    {
                        if (match >= 0 && (second < 0.0f || best < second))
                            second = best;
                        best = d;
                        match = static_cast<int32_t>(candidate);
                    }
                    else if (second < 0.0f || d < second)
                        second = d;
                }
                if (match < 0)
                    continue;
                const float ratio = second < 0.0f ? 1e30f : second / std::max(best, 1e-6f);
                if (ratio > choiceRatio)
                {
                    choiceRatio = ratio;
                    choice = match;
                }
            }
            if (choice < 0)
                continue; // new: where it is now
            if (choiceRatio < 2.0f)
            {
                used[static_cast<uint32_t>(choice)] = true;
                ++stats.ambiguous;
                continue;
            }
            classify(i, choice);
        }
        // Each object's other batches as its first.
        for (size_t i = 0; i < n; ++i)
            if (to.batches[i].key != 0u)
                if (const auto own = to.keys.find(to.batches[i].key); own != to.keys.end() && own->second != i)
                {
                    m.previous[i] = m.previous[own->second];
                    m.kind[i] = m.kind[own->second];
                }
        for (const auto &[id, view] : views)
            if (view.matched >= 6u && view.moved * 10u >= view.matched * 6u)
            {
                m.cutViews[id] = true;
                ++stats.cuts;
            }
        if (!m.cutViews.empty())
            for (size_t i = 0; i < n; ++i)
                m.cut[i] = m.cutViews.count(viewOf(to.batches[i].clip)) != 0u ? 1u : 0u;
    }

    bool Renderer::present(float x, float y, float width, float height)
    {
        const auto presentStart = std::chrono::steady_clock::now();
        const double presentNow = std::chrono::duration<double>(presentStart.time_since_epoch()).count();
        std::shared_ptr<Frame> published, before;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            published = m_published;
            before = m_previous;
        }
        // Interpolating: the two frames the window's clock lies between (pickInterpolated), the newer drawn with its
        // keyed objects and tracked triangles that far back toward the older.
        const bool interpolate = interpolating();
        std::shared_ptr<Frame> interpolateFrom;
        float alpha = 1.0f;
        if (interpolate)
        {
            std::shared_ptr<Frame> to;
            if (pickInterpolated(interpolateFrom, to, alpha, presentNow) && to != m_shown)
            {
                m_shownPrevious = std::move(m_shown);
                m_shown = std::move(to);
            }
        }
        // The game publishes a frame before the GS reaches its groups' markers (their clips), often the next one too:
        // the newest of the two the GS has reached the last group of is shown (the latest one after 100 ms of
        // neither, markers that never come).
        else
        {
            const uint64_t marked = m_markedGroup.load(std::memory_order_acquire);
            const auto reached = [marked](const std::shared_ptr<Frame> &f) { return f && (f->lastGroup == 0u || marked >= f->lastGroup); };
            std::shared_ptr<Frame> next = reached(published) ? published : reached(before) ? before : nullptr;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!next && published && (!m_shown || now - m_shown->time > 0.1))
                next = published;
            if (next && next != m_shown && (!m_shown || next->time > m_shown->time))
            {
                m_shownPrevious = std::move(m_shown);
                m_shown = std::move(next);
            }
        }
        std::shared_ptr<Frame> overlay, backdrop;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            overlay = m_overlayPublished;
            backdrop = m_backdrop;
        }
        if (backdrop && std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - backdrop->time > 0.2)
            backdrop = nullptr;
        // The GS frame has the native draws already (drawn into it in the GS stream): only the port's 2D over it. The
        // same when the native renderer presents its own frame but has none yet (a movie, a level's loading bar).
        const bool overlayOnly = (m_drewIntoGs.load(std::memory_order_relaxed) || (!m_streamDraws && !m_shown)) &&
                                 ((overlay && !overlay->batches.empty()) || backdrop);
        if (!m_enabled || (!overlayOnly && (!m_shown || m_drewIntoGs.load(std::memory_order_relaxed))))
            return false;
        if (overlayOnly)
        {
            if (!m_gpu)
            {
                m_gpu = std::make_unique<Gpu>();
                if (!m_gpu->init())
                {
                    m_enabled = false;
                    return false;
                }
            }
            if (backdrop)
                drawOverlay(*backdrop, x, y, width, height);
            if (overlay)
                drawOverlay(*overlay, x, y, width, height);
            return true;
        }
        const std::shared_ptr<Frame> frame = m_shown, previous = interpolate ? interpolateFrom : m_shownPrevious;
        if (!m_gpu)
        {
            m_gpu = std::make_unique<Gpu>();
            if (!m_gpu->init())
            {
                m_enabled = false;
                return false;
            }
        }
        Gpu &gpu = *m_gpu;
        const float scale = renderScale();
        const int w = std::max(1, static_cast<int>(std::lround(width * scale))), h = std::max(1, static_cast<int>(std::lround(height * scale)));
        if (gpu.targetWidth != w || gpu.targetHeight != h)
        {
            if (gpu.targetWidth != 0)
                UnloadRenderTexture(gpu.target);
            gpu.target = LoadRenderTexture(w, h);
            gpu.targetFiltered = false;
            gpu.targetWidth = w;
            gpu.targetHeight = h;
        }
        // Filtered to the window when the render scale makes it another size, as it is (point) otherwise.
        if (const bool filtered = scale != 1.0f; filtered != gpu.targetFiltered)
        {
            SetTextureFilter(gpu.target.texture, filtered ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
            gpu.targetFiltered = filtered;
        }

        const size_t bytes = frame->vertices.size() * sizeof(Vertex);
        rlEnableVertexArray(gpu.vao);
        if (bytes > gpu.vboBytes)
        {
            if (gpu.vbo != 0)
                rlUnloadVertexBuffer(gpu.vbo);
            gpu.vboBytes = std::max(bytes, gpu.vboBytes * 2u);
            gpu.vbo = rlLoadVertexBuffer(nullptr, static_cast<int>(gpu.vboBytes), true);
            rlSetVertexAttribute(0, 3, kGlFloat, false, sizeof(Vertex), 0);
            rlSetVertexAttribute(1, 2, kGlFloat, false, sizeof(Vertex), 12);
            rlSetVertexAttribute(2, 4, kGlUnsignedByte, true, sizeof(Vertex), 20);
            rlSetVertexAttribute(3, 1, kGlFloat, false, sizeof(Vertex), 24);
            rlEnableVertexAttribute(0);
            rlEnableVertexAttribute(1);
            rlEnableVertexAttribute(2);
            rlEnableVertexAttribute(3);
        }
        // Interpolating, a frame is drawn several times: uploaded once (its tracked triangles again when moved).
        const bool lerping = interpolate && previous && alpha < 1.0f;
        if (bytes != 0u && (!interpolate || gpu.uploadedSerial != frame->serial || gpu.uploadedVbo != gpu.vbo ||
                            (gpu.trackedMoved && !lerping)))
        {
            rlUpdateVertexBuffer(gpu.vbo, frame->vertices.data(), static_cast<int>(bytes), 0);
            gpu.uploadedSerial = frame->serial;
            gpu.uploadedVbo = gpu.vbo;
            gpu.trackedMoved = false;
        }

        // The keyed batches' matrices between the two frames, and the tracked triangles' corners.
        const Screen &screen = frame->screen;
        const float jumpPixels = 0.3f * screen.width;
        struct Lerped
        {
            float matrix[16];
            bool use = false;
        };
        static thread_local std::vector<Lerped> s_lerped;
        s_lerped.assign(lerping ? frame->batches.size() : 0u, Lerped{});
        uint32_t keyedBatches = 0, lerpedBatches = 0;
        InterpolationStats &stats = interpolationStats();
        if (lerping)
        {
            if (m_match.from != previous->serial || m_match.to != frame->serial)
                matchFrames(*previous, *frame); // once per pair of frames
            const size_t n = frame->batches.size();
            size_t last = SIZE_MAX;
            for (size_t i = 0; i < n; ++i)
            {
                const Batch &b = frame->batches[i];
                if (b.key == 0u)
                    continue;
                ++keyedBatches;
                const int32_t from = m_match.previous[i];
                const uint8_t kind = m_match.kind[i];
                if (from < 0 || kind == kTeleport || m_match.cut[i])
                    continue;
                if (last != SIZE_MAX && frame->batches[last].key == b.key && m_match.previous[last] == from) // the object's next batch
                {
                    s_lerped[i] = s_lerped[last];
                    lerpedBatches += s_lerped[i].use ? 1u : 0u;
                    continue;
                }
                last = i;
                const Batch &p = previous->batches[static_cast<size_t>(from)];
                if (b.hasParts && p.hasParts)
                {
                    float camera[16], object[16];
                    lerpMatrix(p.camera, b.camera, alpha, camera);
                    if (kind == kSpin)
                    {
                        std::memcpy(object, b.object, sizeof(object));
                        for (int k = 12; k < 16; ++k)
                            object[k] = p.object[k] + (b.object[k] - p.object[k]) * alpha;
                    }
                    else
                    {
                        float angle;
                        interpolateObject(p.object, b.object, alpha, object, angle);
                    }
                    multiply(camera, object, s_lerped[i].matrix);
                }
                else if (kind == kLerp)
                    lerpMatrix(p.matrix, b.matrix, alpha, s_lerped[i].matrix);
                else
                    continue;
                s_lerped[i].use = true;
                ++lerpedBatches;
            }
            // Tracked triangles: each corner between where the older frame had it and where the newer has it.
            for (size_t i = 0; i < n; ++i)
            {
                const Batch &b = frame->batches[i];
                if (b.idFirst < 0)
                    continue;
                const bool viewCut = m_match.cutViews.count(viewOf(b.clip)) != 0u;
                m_trackedScratch.assign(frame->vertices.begin() + b.first, frame->vertices.begin() + b.first + b.count);
                bool moved = false;
                for (uint32_t t = 0; t < b.count / 3u; ++t)
                {
                    const uint64_t id = frame->triangleIds[static_cast<size_t>(b.idFirst) + t];
                    if (id == 0u)
                        continue;
                    ++stats.tracked;
                    const auto found = previous->tracked.find(id);
                    if (viewCut || found == previous->tracked.end() || found->second + 3u > previous->vertices.size())
                        continue;
                    const Vertex *pv = &previous->vertices[found->second];
                    Vertex *cv = &m_trackedScratch[3u * t];
                    const float dx = (cv[0].x + cv[1].x + cv[2].x - pv[0].x - pv[1].x - pv[2].x) / 3.0f;
                    const float dy = (cv[0].y + cv[1].y + cv[2].y - pv[0].y - pv[1].y - pv[2].y) / 3.0f;
                    if (std::hypot(dx, dy) > jumpPixels)
                        continue;
                    for (int k = 0; k < 3; ++k)
                    {
                        cv[k].x = pv[k].x + (cv[k].x - pv[k].x) * alpha;
                        cv[k].y = pv[k].y + (cv[k].y - pv[k].y) * alpha;
                        cv[k].z = pv[k].z + (cv[k].z - pv[k].z) * alpha;
                    }
                    moved = true;
                    ++stats.trackedLerped;
                }
                if (moved || gpu.trackedMoved)
                    rlUpdateVertexBuffer(gpu.vbo, m_trackedScratch.data(), static_cast<int>(b.count * sizeof(Vertex)),
                                         static_cast<int>(b.first * sizeof(Vertex)));
                gpu.trackedMoved = gpu.trackedMoved || moved;
            }
        }
        rlDisableVertexArray();

        // PS2X_INTERP_TRACE (tracePresents): the origins of the keyed objects that moved most, as drawn.
        if (m_tracePresents > 0 && previous)
        {
            if (m_traceKeys.empty())
            {
                // The four that moved most on the screen (each at its own place: an object's parts share an origin), and
                // one that barely moved (the scenery under a still camera, or the player's car under a chase camera).
                struct Moving
                {
                    float moved, x, y;
                    uint64_t key;
                };
                std::vector<Moving> moving;
                for (const auto &[key, index] : frame->keys)
                    if (const auto found = previous->keys.find(key); found != previous->keys.end())
                    {
                        float ax, ay, bx, by;
                        if (originOnScreen(previous->batches[found->second].matrix, ax, ay) &&
                            originOnScreen(frame->batches[index].matrix, bx, by) && std::hypot(bx - ax, by - ay) < 0.3f * screen.width &&
                            bx > screen.originX && bx < screen.originX + screen.width && by > screen.originY && by < screen.originY + screen.height)
                            moving.push_back({std::hypot(bx - ax, by - ay), bx, by, key});
                    }
                std::sort(moving.begin(), moving.end(), [](const Moving &a, const Moving &b) { return a.moved > b.moved; });
                std::vector<const Moving *> chosen;
                for (const Moving &m : moving)
                {
                    if (chosen.size() == 4u)
                        break;
                    if (std::none_of(chosen.begin(), chosen.end(), [&](const Moving *c) { return std::hypot(c->x - m.x, c->y - m.y) < 8.0f; }))
                        chosen.push_back(&m);
                }
                if (!moving.empty())
                    chosen.push_back(&moving.back());
                for (const Moving *m : chosen)
                    m_traceKeys.push_back(m->key);
                m_traceSerial = frame->serial;
            }
            else if (m_traceSerial != frame->serial && lerping && m_match.to == frame->serial && m_traceSerial == previous->serial)
            {
                // A new frame: each traced object under the key the match gave it there (an alike object's key changes
                // as others are culled).
                for (uint64_t &key : m_traceKeys)
                    if (const auto own = previous->keys.find(key); own != previous->keys.end())
                        for (size_t i = 0; i < frame->batches.size(); ++i)
                            if (m_match.previous[i] == static_cast<int32_t>(own->second))
                            {
                                key = frame->batches[i].key;
                                break;
                            }
                m_traceSerial = frame->serial;
            }
            for (size_t slot = 0; slot < m_traceKeys.size(); ++slot)
            {
                const uint64_t key = m_traceKeys[slot];
                const auto found = frame->keys.find(key);
                float sx = 0.0f, sy = 0.0f;
                bool on = false;
                if (found != frame->keys.end())
                {
                    const float *m = !s_lerped.empty() && s_lerped[found->second].use ? s_lerped[found->second].matrix
                                                                                       : frame->batches[found->second].matrix;
                    on = originOnScreen(m, sx, sy);
                }
                // slot: the traced object (its key changes when an alike one's count does).
                std::fprintf(stderr, "[interp-trace] t=%.4f from=%llu to=%llu alpha=%.3f key=%016llx slot=%zu %s x=%.2f y=%.2f\n", presentNow,
                             static_cast<unsigned long long>(previous->serial), static_cast<unsigned long long>(frame->serial), alpha,
                             static_cast<unsigned long long>(key), slot, on ? "on" : "off", sx, sy);
            }
            if (--m_tracePresents == 0)
                m_traceKeys.clear();
        }

        BeginTextureMode(gpu.target);
        if (m_presentsAlone)
        {
            const uint32_t c = m_clearColour.load(std::memory_order_relaxed);
            ClearBackground(Color{static_cast<unsigned char>(c), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c >> 16), 0}); // alpha 0: under it, circuit 2 shows (menus)
        }
        else
            ClearBackground(BLANK);
        rlDrawRenderBatchActive();
        const float screenRect[4] = {screen.originX, screen.originY, screen.width, screen.height};
        const int slot = 0;
        rlDisableBackfaceCulling();
        rlEnableDepthTest();
        // Each batch's clip (its group's GS scissor) in the target's pixels, bottom-up; x1 <= x0: none.
        struct Rect
        {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        };
        std::vector<Rect> clips(frame->batches.size());
        {
            std::lock_guard<std::mutex> lock(m_streamMutex);
            for (size_t i = 0; i < frame->batches.size(); ++i)
            {
                const uint64_t group = frame->batches[i].group;
                Clip clip = m_clips[group % kClips];
                const float *own = frame->batches[i].clip; // the port's (setClip): first, and the only one presenting alone
                if (own[2] > own[0] || m_presentsAlone || group == 0u || clip.group != group)
                {
                    if (own[2] <= own[0])
                        continue;
                    clip.x0 = own[0];
                    clip.y0 = own[1];
                    clip.x1 = own[2];
                    clip.y1 = own[3];
                }
                const float sx = static_cast<float>(w) / screen.width, sy = static_cast<float>(h) / screen.height;
                // PS2X_NATIVE_LOG_CLIPS=1: now and then, a batch's clip.
                static const bool s_logClips = std::getenv("PS2X_NATIVE_LOG_CLIPS") != nullptr;
                static uint32_t s_clipLogs = 0;
                if (s_logClips && ++s_clipLogs % 5000u == 1u)
                    std::fprintf(stderr, "[native] clip group %llu: %.1f %.1f - %.1f %.1f (screen %.1f %.1f %.1fx%.1f)\n",
                                 static_cast<unsigned long long>(group), clip.x0, clip.y0, clip.x1, clip.y1, screen.originX,
                                 screen.originY, screen.width, screen.height);
                Rect &rect = clips[i];
                rect.x0 = std::clamp(static_cast<int>(std::lround((clip.x0 - screen.originX) * sx)), 0, w);
                rect.x1 = std::clamp(static_cast<int>(std::lround((clip.x1 - screen.originX) * sx)), 0, w);
                rect.y0 = std::clamp(static_cast<int>(std::lround(h - (clip.y1 - screen.originY) * sy)), 0, h);
                rect.y1 = std::clamp(static_cast<int>(std::lround(h - (clip.y0 - screen.originY) * sy)), 0, h);
                if (rect.x0 == 0 && rect.y0 == 0 && rect.x1 == w && rect.y1 == h)
                    rect = Rect{}; // the whole target: no clip
                else if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0)
                    rect = Rect{0, 0, 0, -1}; // nothing shows
            }
        }
        bool scissored = false;
        for (size_t index = 0; index < frame->batches.size(); ++index)
        {
            const Batch &batch = frame->batches[index];
            const Material &m = batch.material;
            const Rect &clip = clips[index];
            if (clip.y1 < 0)
                continue;
            const float *matrix = !s_lerped.empty() && s_lerped[index].use ? s_lerped[index].matrix : batch.matrix;
            // Blend state first: rlSetBlendMode flushes raylib's own batch, which unbinds the shader and the vertex
            // array (even with nothing to draw), so they are bound after it.
            if (m.blend == Material::Blend::Opaque)
                rlDisableColorBlend();
            else
            {
                rlEnableColorBlend();
                rlSetBlendFactorsSeparate(kGlSrcAlpha, m.blend == Material::Blend::Alpha ? kGlOneMinusSrcAlpha : kGlOne,
                                          kGlOne, kGlOneMinusSrcAlpha, kGlFuncAdd, kGlFuncAdd);
                rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
            }
            if (m.depthWrite)
                rlEnableDepthMask();
            else
                rlDisableDepthMask();
            // The GS's depth test is GEQUAL (larger nearer): here LEQUAL on 1 - z, so what lies at the far end (a sky at
            // Z 0, a clear) passes against the cleared depth as on the GS; a draw without ZTE is not tested.
            if (m.depthTest)
            {
                rlEnableDepthTest();
                glDepthFuncLequal();
            }
            else
                rlDisableDepthTest();
            rlEnableShader(gpu.shader);
            rlEnableVertexArray(gpu.vao);
            rlSetUniform(gpu.locScreen, screenRect, RL_SHADER_UNIFORM_VEC4, 1);
            rlSetUniform(gpu.locDepthMax, &screen.depthMax, RL_SHADER_UNIFORM_FLOAT, 1);
            rlSetUniform(gpu.locTex, &slot, RL_SHADER_UNIFORM_INT, 1);
            rlSetUniform(gpu.locM, matrix, RL_SHADER_UNIFORM_VEC4, 4);
            std::shared_ptr<const TextureCache::Texture> texture = batch.texture;
            if (m.textured && !texture)
                texture = ps2x::native::textures().texture(m.tex0, m.texa, m.region);
            // PS2X_NATIVE_DUMP_TEXTURE=<tex0, hex>: that texture as the cache decoded it, once, as texture_<tex0>.png
            // (in PS2X_NATIVE_CAPTURE_DIR, or the current folder).
            static const uint64_t s_dumpTex0 = []
            {
                const char *value = std::getenv("PS2X_NATIVE_DUMP_TEXTURE");
                return value && *value ? std::strtoull(value, nullptr, 16) : 0ull;
            }();
            static bool s_dumped = false;
            if (s_dumpTex0 != 0u && !s_dumped && texture && m.tex0 == s_dumpTex0)
            {
                s_dumped = true;
                Image image{const_cast<uint32_t *>(texture->rgba.data()), static_cast<int>(texture->width),
                            static_cast<int>(texture->height), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                const char *dir = std::getenv("PS2X_NATIVE_CAPTURE_DIR");
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/texture_%016llx.png", dir && *dir ? dir : ".", static_cast<unsigned long long>(s_dumpTex0));
                ExportImage(image, path);
            }
            const int textured = m.textured && texture ? 1 : 0, alphaTest = m.alphaTest ? 1 : 0;
            const int opaque = m.blend == Material::Blend::Opaque ? 1 : 0;
            rlSetUniform(gpu.locOpaque, &opaque, RL_SHADER_UNIFORM_INT, 1);
            const int fog = m.fog ? 1 : 0;
            const float fogColour[3] = {static_cast<float>(m.fogColour & 0xffu) / 255.0f, static_cast<float>((m.fogColour >> 8) & 0xffu) / 255.0f,
                                        static_cast<float>((m.fogColour >> 16) & 0xffu) / 255.0f};
            rlSetUniform(gpu.locFog, &fog, RL_SHADER_UNIFORM_INT, 1);
            rlSetUniform(gpu.locFogColour, fogColour, RL_SHADER_UNIFORM_VEC3, 1);
            const float alphaRef = m.alphaRef / 128.0f;
            rlSetUniform(gpu.locTextured, &textured, RL_SHADER_UNIFORM_INT, 1);
            rlSetUniform(gpu.locAlphaTest, &alphaTest, RL_SHADER_UNIFORM_INT, 1);
            rlSetUniform(gpu.locAlphaRef, &alphaRef, RL_SHADER_UNIFORM_FLOAT, 1);
            if (textured)
            {
                const unsigned int id = gpu.texture(m.tex0 ^ (m.texa * 0x9e3779b97f4a7c15ull) ^ (m.region * 0xc2b2ae3d27d4eb4full), texture);
                // Wrap per draw (rlTextureParameters binds and unbinds the texture: before enabling it).
                rlTextureParameters(id, RL_TEXTURE_WRAP_S, m.clampU ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                rlTextureParameters(id, RL_TEXTURE_WRAP_T, m.clampV ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                rlActiveTextureSlot(0);
                rlEnableTexture(id);
            }
            if (clip.x1 > clip.x0)
            {
                rlEnableScissorTest();
                rlScissor(clip.x0, clip.y0, clip.x1 - clip.x0, clip.y1 - clip.y0);
                scissored = true;
            }
            else if (scissored)
            {
                rlDisableScissorTest();
                scissored = false;
            }
            rlDrawVertexArray(static_cast<int>(batch.first), static_cast<int>(batch.count));
        }
        rlDisableScissorTest();
        if (secondsNow() - m_depthWantedAt.load(std::memory_order_relaxed) < 1.0)
        {
            // The tiles asked for in the last half second (none at first: depthAt maps to pixels with this size).
            std::vector<uint64_t> tiles;
            {
                std::lock_guard<std::mutex> lock(m_depthMutex);
                if (w != m_depthWidth || h != m_depthHeight)
                {
                    m_depthTiles.clear();
                    m_depthTilesWanted.clear();
                }
                m_depthWidth = w;
                m_depthHeight = h;
                m_depthScreen = screen;
                const double now = secondsNow();
                for (auto it = m_depthTilesWanted.begin(); it != m_depthTilesWanted.end();)
                    if (now - it->second > 0.5)
                        it = m_depthTilesWanted.erase(it);
                    else
                        tiles.push_back((it++)->first);
            }
            std::unordered_map<uint64_t, std::vector<float>> depth;
            if (readFramebufferDepthTiles(w, h, kDepthTile, tiles, depth))
            {
                std::lock_guard<std::mutex> lock(m_depthMutex);
                m_depthTiles.swap(depth);
            }
        }
        rlDisableVertexArray();
        rlDisableShader();
        rlDisableTexture();
        rlEnableDepthMask();
        rlDisableDepthTest();
        rlEnableColorBlend();
        rlSetBlendMode(RL_BLEND_ALPHA);
        EndTextureMode();

        static const bool s_stats = []
        {
            const char *value = std::getenv("PS2X_NATIVE_STATS");
            return value && *value == '1';
        }();
        static uint32_t s_presents = 0;
        if (s_stats && ++s_presents % 250u == 0u)
        {
            std::fprintf(stderr, "[native] present %u: %zu batches, %zu vertices, shader %u (default %u), target %dx%d, alpha %.2f, interpolated batches %llu\n",
                         s_presents, frame->batches.size(), frame->vertices.size(), gpu.shader, rlGetShaderIdDefault(), w, h, alpha,
                         static_cast<unsigned long long>(lerpedBatches));
        }

        // PS2X_NATIVE_CAPTURE_DIR=<dir>: the native frame as native_<n>.png every PS2X_NATIVE_CAPTURE_INTERVAL seconds
        // (default 5), at the window's resolution, to look at it without the GS frame; with gs_<n>.png, the GS frame of
        // the same moment (a reference to compare to, when the game still runs on VU1 what the native port draws).
        static const char *s_captureDir = std::getenv("PS2X_NATIVE_CAPTURE_DIR");
        if (s_captureDir && *s_captureDir)
        {
            static const double s_interval = []
            {
                const char *value = std::getenv("PS2X_NATIVE_CAPTURE_INTERVAL");
                return value && *value ? std::atof(value) : 5.0;
            }();
            static double s_next = 0.0;
            static int s_count = 0;
            const double now = GetTime();
            if (now >= s_next)
            {
                s_next = now + s_interval;
                Image image = LoadImageFromTexture(gpu.target.texture);
                ImageFlipVertical(&image);
                const std::string path = std::string(s_captureDir) + "/native_" + std::to_string(++s_count) + ".png";
                ExportImage(image, path.c_str());
                UnloadImage(image);
                if (m_gsFrame[0] != 0)
                {
                    const Texture2D gs{static_cast<unsigned int>(m_gsFrame[0]), m_gsFrame[1], m_gsFrame[2], 1, m_gsFrame[3]};
                    Image gsImage = LoadImageFromTexture(gs);
                    const std::string gsPath = std::string(s_captureDir) + "/gs_" + std::to_string(s_count) + ".png";
                    ExportImage(gsImage, gsPath.c_str());
                    UnloadImage(gsImage);
                }
            }
        }

        // Under the frame, as the display merges it: the picture the game shows through circuit 2 (a menu's movie frame,
        // where the frame's alpha is 0), then a movie's backdrop. Without either the frame is shown alone, its alpha
        // ignored (PMODE with circuit 1 only: what a draw leaves in the alpha, a sky's 0, never shows).
        bool under = backdrop != nullptr;
        {
            const DisplayPicture picture = displayPicture();
            under = under || (picture.rgba && picture.width != 0u && picture.height != 0u);
            if (picture.rgba && picture.width != 0u && picture.height != 0u)
            {
                if (gpu.displayPictureVersion != picture.version || gpu.displayPicture == 0u)
                {
                    if (gpu.displayPicture != 0u)
                        rlUnloadTexture(gpu.displayPicture);
                    std::vector<uint32_t> rgba(*picture.rgba);
                    for (uint32_t &c : rgba)
                        c = (c & 0xffffffu) | 0xff000000u;
                    gpu.displayPicture = rlLoadTexture(rgba.data(), static_cast<int>(picture.width), static_cast<int>(picture.height),
                                                       RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                    rlTextureParameters(gpu.displayPicture, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                    rlTextureParameters(gpu.displayPicture, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                    gpu.displayPictureVersion = picture.version;
                }
                const Texture2D texture{gpu.displayPicture, static_cast<int>(picture.width), static_cast<int>(picture.height), 1,
                                        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                DrawTexturePro(texture, Rectangle{0, 0, static_cast<float>(picture.width), static_cast<float>(picture.height)},
                               Rectangle{x, y, width, height}, Vector2{0, 0}, 0.0f, WHITE);
                rlDrawRenderBatchActive();
            }
        }
        if (backdrop)
            drawOverlay(*backdrop, x, y, width, height);
        // Render textures are stored bottom-up.
        if (under || !m_presentsAlone)
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
        else
        {
            rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD); // the frame as it is, alpha ignored
            BeginBlendMode(BLEND_CUSTOM);
        }
        DrawTexturePro(gpu.target.texture, Rectangle{0, 0, static_cast<float>(w), -static_cast<float>(h)},
                       Rectangle{x, y, width, height}, Vector2{0, 0}, 0.0f, WHITE);
        EndBlendMode();
        rlDrawRenderBatchActive();
        // Over it, the port's 2D.
        if (overlay)
            drawOverlay(*overlay, x, y, width, height);

        m_lastPresent = PresentInfo{presentNow, previous ? previous->serial : 0u, frame->serial, lerping ? alpha : 1.0f, lerpedBatches,
                                    keyedBatches};
        if (interpolate)
        {
            // PS2X_PRESENT_STATS=1, interpolating: every two seconds, what the window showed (interpolationStats).
            ++stats.presents;
            stats.keyed += keyedBatches;
            stats.lerped += lerpedBatches;
            stats.holds += lerping ? 0u : 1u;
            const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - presentStart).count();
            stats.cpu += cpu;
            stats.cpuMax = std::max(stats.cpuMax, cpu);
            // A still picture: presents drawing the same frame at the same point (the game late, a load).
            const uint64_t picture = frame->serial * 1000003u + (previous ? previous->serial : 0u) * 7919u +
                                     static_cast<uint64_t>(lerping ? alpha * 4096.0f : 4096.0f);
            if (picture != stats.picture)
            {
                stats.picture = picture;
                stats.pictureSince = presentNow;
            }
            stats.stillMax = std::max(stats.stillMax, presentNow - stats.pictureSince);
            if (stats.lastPresent > 0.0)
                stats.gapMax = std::max(stats.gapMax, presentNow - stats.lastPresent);
            stats.lastPresent = presentNow;
            if (stats.windowStart == 0.0)
            {
                stats.windowStart = presentNow;
                stats.firstSerial = frame->serial;
            }
            static const bool s_presentStats = std::getenv("PS2X_PRESENT_STATS") != nullptr;
            const double span = presentNow - stats.windowStart;
            if (span >= 2.0)
            {
                if (s_presentStats)
                    std::fprintf(stderr,
                                 "[present] interpolated, %.1f s: %.1f presents/s, %.1f game frames/s, keyed batches %.1f%% interpolated, tracked "
                                 "triangles %.1f%% interpolated, %llu teleports, %llu spins and %llu ambiguous (objects, summed over the game frames), %llu view cuts, %u held presents, longest still picture %.1f ms, longest gap between presents %.1f ms, "
                                 "present() %.2f ms avg / %.2f max, largest turn %.1f deg/frame (%llu over 10)\n",
                                 span, stats.presents / span, static_cast<double>(frame->serial - stats.firstSerial) / span,
                                 stats.keyed ? 100.0 * stats.lerped / stats.keyed : 0.0,
                                 stats.tracked ? 100.0 * stats.trackedLerped / stats.tracked : 0.0, static_cast<unsigned long long>(stats.jumped),
                                 static_cast<unsigned long long>(stats.spins), static_cast<unsigned long long>(stats.ambiguous),
                                 static_cast<unsigned long long>(stats.cuts), stats.holds, stats.stillMax * 1000.0, stats.gapMax * 1000.0, 1000.0 * stats.cpu / stats.presents,
                                 1000.0 * stats.cpuMax, stats.maxAngle, static_cast<unsigned long long>(stats.over10));
                const double since = stats.pictureSince, lastPresent = stats.lastPresent;
                const uint64_t picture = stats.picture;
                stats = InterpolationStats{};
                stats.pictureSince = since;
                stats.picture = picture;
                stats.lastPresent = lastPresent;
            }
        }
        return true;
    }
}
