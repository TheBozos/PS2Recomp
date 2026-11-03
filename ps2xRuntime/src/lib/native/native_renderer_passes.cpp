// Renderer::passKeyed: a game's off-screen work (its glow, its shadow volumes, its depth readbacks, its 2D sampling the
// frame) done natively where the GS stream reaches a port's GIF chain marker, into the GL target the GS renderer keeps
// for the game's frame. Runs on the GS thread with the GS renderer's GL context current: raw GL, its own programs,
// vertex array and work textures.

#include "runtime/native/native_renderer.h"
#include "runtime/native/native_gs_hook.h"

#include "external/glad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ps2x::native
{
    namespace
    {
        // x, y in the destination's pixels (no flip: the GS renderer's targets keep row 0 at GL's bottom), z its depth
        // (0..1), u, v in 0..1 of the source.
        const char *kVertexShader = R"(#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
uniform vec2 uSize;
out vec2 vUv;
out vec4 vCol;
void main()
{
    gl_Position = vec4(aPos.xy / uSize * 2.0 - 1.0, aPos.z * 2.0 - 1.0, 1.0);
    vUv = aUv;
    vCol = aCol;
}
)";

        // uMode 0: the source where its alpha (GS scale) reaches uRef, else nothing drawn; 1: the source; 2: the source
        // (modulated by the vertex colour, 0x80 = 1.0, when uModulate) through the GS blend uBlend; 3: the vertex colour
        // through uBlend; 4: the source's depth (colour masked). uBlend 0: Cs; 1: Cs * f added; 2: Cs * f taken away;
        // 3: Cd + (Cs - Cd) * f; f = As (uFactorFix < 0) or uFactorFix. The second output carries f for 3.
        const char *kFragmentShader = R"(#version 330
in vec2 vUv;
in vec4 vCol;
uniform sampler2D uTex;
uniform int uMode;
uniform float uRef;
uniform int uModulate;
uniform int uBlend;
uniform float uFactorFix;
uniform float uTexelAlpha;
layout(location = 0, index = 0) out vec4 fragColor;
layout(location = 0, index = 1) out vec4 fragFactor;
void main()
{
    fragFactor = vec4(0.0);
    if (uMode == 4)
    {
        gl_FragDepth = texture(uTex, vUv).r;
        fragColor = vec4(0.0);
        return;
    }
    vec4 c;
    if (uMode == 3)
        c = vCol * (255.0 / 128.0);
    else
    {
        c = texture(uTex, vUv);
        c.a = uTexelAlpha >= 0.0 ? uTexelAlpha : c.a * (255.0 / 128.0);
        if (uMode == 0 && c.a * 128.0 < uRef - 0.5)
            discard;
        if (uMode == 2 && uModulate != 0)
            c = c * vCol * (255.0 / 128.0);
    }
    c = clamp(c, vec4(0.0), vec4(1.0, 1.0, 1.0, 255.0 / 128.0));
    float f = uFactorFix < 0.0 ? c.a : uFactorFix;
    if (uMode >= 2 && (uBlend == 1 || uBlend == 2))
        c.rgb = min(c.rgb * f, vec3(1.0));
    fragFactor = vec4(min(f, 1.0));
    fragColor = vec4(c.rgb, c.a * (128.0 / 255.0));
}
)";

        GLuint compile(GLenum type, const char *source)
        {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024] = {};
                glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                std::fprintf(stderr, "[native] target pass shader: %s\n", log);
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        struct PassVertex
        {
            float x, y, z, u, v;
            uint32_t rgba;
        };

        // The GS ALPHA register as the pass shader's blend (uBlend, factor): the forms a game's full-screen passes use.
        struct Blend
        {
            int mode = 0;
            float fix = -1.0f; // < 0: As
            bool known = true;
        };
        Blend blendOf(uint64_t alpha, bool enabled)
        {
            Blend b;
            if (!enabled)
                return b;
            const uint32_t a = alpha & 3u, bb = (alpha >> 2) & 3u, c = (alpha >> 4) & 3u, d = (alpha >> 6) & 3u;
            b.fix = c == 2u ? static_cast<float>((alpha >> 32) & 0xFFu) / 128.0f : -1.0f;
            if (c == 1u) // Ad: not a form these passes take
                b.known = false;
            if (a == 0u && bb == 1u && d == 1u)
                b.mode = 3;
            else if (a == 0u && bb == 2u && d == 1u)
                b.mode = 1;
            else if (a == 2u && bb == 0u && d == 1u)
                b.mode = 2;
            else if (a == 0u && bb == 2u && d == 2u && c == 2u && ((alpha >> 32) & 0xFFu) == 0x80u)
                b.mode = 0;
            else if ((a == bb) && d == 1u)
                b.mode = -1; // Cd: nothing drawn
            else if ((a == bb) && d == 0u)
                b.mode = 0;
            else
                b.known = false;
            return b;
        }
    }

    struct Renderer::PassGpu
    {
        GLuint program = 0, vao = 0, vbo = 0, fbo = 0;
        GLint locSize = -1, locTex = -1, locMode = -1, locRef = -1, locModulate = -1, locBlend = -1, locFactorFix = -1, locTexelAlpha = -1;
        bool failed = false;
        struct Work
        {
            GLuint texture = 0, fbo = 0;
            GLsizei width = 0, height = 0;
        };
        Work half, quarter, quarterB, glow, display, copy;
        GLuint stencil = 0; // depth + stencil of the target's size (Volumes / Darken)
        GLsizei stencilWidth = 0, stencilHeight = 0;
        uint32_t glowWidth = 0, glowHeight = 0, displayWidth = 0, displayHeight = 0; // GS pixels they cover
        uint64_t unknownBlends = 0;

        bool init()
        {
            const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader), fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
            if (vs == 0 || fs == 0)
                return false;
            program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint ok = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok)
                return false;
            locSize = glGetUniformLocation(program, "uSize");
            locTex = glGetUniformLocation(program, "uTex");
            locMode = glGetUniformLocation(program, "uMode");
            locRef = glGetUniformLocation(program, "uRef");
            locModulate = glGetUniformLocation(program, "uModulate");
            locBlend = glGetUniformLocation(program, "uBlend");
            locFactorFix = glGetUniformLocation(program, "uFactorFix");
            locTexelAlpha = glGetUniformLocation(program, "uTexelAlpha");
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glGenFramebuffers(1, &fbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PassVertex), reinterpret_cast<const void *>(0));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(PassVertex), reinterpret_cast<const void *>(12));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PassVertex), reinterpret_cast<const void *>(20));
            for (GLuint i = 0; i < 3; ++i)
                glEnableVertexAttribArray(i);
            glBindVertexArray(0);
            return true;
        }

        static void sized(Work &work, GLsizei width, GLsizei height)
        {
            width = std::max<GLsizei>(width, 1);
            height = std::max<GLsizei>(height, 1);
            if (work.texture && work.width == width && work.height == height)
                return;
            if (!work.texture)
            {
                glGenTextures(1, &work.texture);
                glGenFramebuffers(1, &work.fbo);
            }
            glBindTexture(GL_TEXTURE_2D, work.texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, work.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, work.texture, 0);
            work.width = width;
            work.height = height;
        }

        // A texture of the GS renderer's sampled with this filter until the scope ends (its own filter put back).
        struct BorrowedFilter
        {
            GLuint texture = 0;
            GLint min = GL_NEAREST, mag = GL_NEAREST;
            BorrowedFilter(GLuint t, bool linear) : texture(t)
            {
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag);
                filter(texture, linear);
            }
            ~BorrowedFilter()
            {
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
            }
        };

        static void filter(GLuint texture, bool linear)
        {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        }

        // The target's own framebuffer with this pass's stencil in place of its depth (the target's depth copied in
        // by Volumes' clear).
        bool stencilTarget(const GsDrawTarget &target)
        {
            const GLsizei w = static_cast<GLsizei>(target.width * target.scale), h = static_cast<GLsizei>(target.height * target.scale);
            if (!stencil || stencilWidth != w || stencilHeight != h)
            {
                if (!stencil)
                    glGenRenderbuffers(1, &stencil);
                glBindRenderbuffer(GL_RENDERBUFFER, stencil);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, w, h);
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                stencilWidth = w;
                stencilHeight = h;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
            return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }

        void draw(const std::vector<PassVertex> &vertices, GLenum primitive, float width, float height)
        {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(PassVertex)), vertices.data(), GL_STREAM_DRAW);
            const float size[2] = {width, height};
            glUniform2fv(locSize, 1, size);
            glDrawArrays(primitive, 0, static_cast<GLsizei>(vertices.size()));
        }

        // A rectangle (x0, y0)-(x1, y1) of the destination sampling (u0, v0)-(u1, v1).
        void rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float width, float height,
                  uint32_t rgba = 0x80808080u)
        {
            const std::vector<PassVertex> quad = {{x0, y0, 0, u0, v0, rgba}, {x1, y0, 0, u1, v0, rgba}, {x0, y1, 0, u0, v1, rgba},
                                                  {x1, y1, 0, u1, v1, rgba}};
            draw(quad, GL_TRIANGLE_STRIP, width, height);
        }

        void mode(int m, int blend = 0, float fix = -1.0f, bool modulate = false, float ref = 0.0f, int texelAlpha = -1)
        {
            glUniform1f(locTexelAlpha, texelAlpha < 0 ? -1.0f : static_cast<float>(texelAlpha) / 128.0f);
            glUniform1i(locMode, m);
            glUniform1i(locBlend, blend);
            glUniform1f(locFactorFix, fix);
            glUniform1i(locModulate, modulate ? 1 : 0);
            glUniform1f(locRef, ref);
        }

        // GL's blending for a pass shader blend (the shader has applied the factor for 1 and 2).
        static void glBlend(int mode)
        {
            if (mode == 0)
            {
                glDisable(GL_BLEND);
                return;
            }
            glEnable(GL_BLEND);
            if (mode == 3)
            {
                glBlendEquation(GL_FUNC_ADD);
                glBlendFuncSeparate(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA, GL_ONE, GL_ZERO);
                return;
            }
            glBlendEquationSeparate(mode == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        }
    };

    void Renderer::passKeyed(uint64_t key, TargetPass pass)
    {
        if (!m_streamDraws)
            return;
        std::lock_guard<std::mutex> lock(m_streamMutex);
        auto [it, added] = m_keyedPasses.try_emplace(key);
        it->second.push_back(std::move(pass));
        if (!added)
            return;
        m_keyedPassOrder.push_back(key);
        while (m_keyedPassOrder.size() > 65536u)
        {
            m_keyedPasses.erase(m_keyedPassOrder.front());
            m_keyedPassOrder.pop_front();
        }
    }

    void Renderer::standingPassKeyed(uint64_t key, TargetPass pass)
    {
        if (!m_streamDraws)
            return;
        std::lock_guard<std::mutex> lock(m_streamMutex);
        m_standingPasses[key] = std::move(pass);
    }

    void Renderer::runKeyedPasses(uint64_t key, TargetSource source, void *user)
    {
        std::vector<TargetPass> passes;
        {
            std::lock_guard<std::mutex> lock(m_streamMutex);
            const auto it = m_keyedPasses.find(key);
            if (it != m_keyedPasses.end())
            {
                passes = std::move(it->second);
                m_keyedPasses.erase(it);
            }
            const auto standing = m_standingPasses.find(key);
            if (standing != m_standingPasses.end())
                passes.push_back(standing->second);
            if (passes.empty())
                return;
        }
        if (!m_passGpu)
        {
            m_passGpu = new PassGpu();
            m_passGpu->failed = !m_passGpu->init();
        }
        if (m_passGpu->failed)
            return;
        GsDrawTarget last;
        for (const TargetPass &pass : passes)
        {
            GsDrawTarget target;
            if (!source(user, pass.frame, pass.zbuf, pass.height, target) || target.framebuffer == 0u)
                continue;
            m_drewIntoGs.store(true, std::memory_order_relaxed);
            runPass(*m_passGpu, pass, target);
            last = target;
        }
        if (last.framebuffer != 0u) // the GS renderer's target bound again, as after drawGroups
        {
            glBindFramebuffer(GL_FRAMEBUFFER, last.framebuffer);
            glViewport(0, 0, static_cast<GLsizei>(last.width * std::max(last.scale, 1u)), static_cast<GLsizei>(last.height * std::max(last.scale, 1u)));
        }
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glBindVertexArray(0);
        glUseProgram(0);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glBlendEquation(GL_FUNC_ADD);
        glStencilMask(0xFFu);
    }

    void Renderer::runPass(PassGpu &gpu, const TargetPass &pass, const GsDrawTarget &target)
    {
        const uint32_t scale = std::max(target.scale, 1u);
        const float tw = static_cast<float>(target.width), th = static_cast<float>(target.height);
        const float w = static_cast<float>(std::min(pass.width, target.width)), h = static_cast<float>(std::min(pass.height, target.height));
        const uint32_t xOffset = static_cast<uint32_t>(pass.xyoffset & 0xFFFFu), yOffset = static_cast<uint32_t>((pass.xyoffset >> 32) & 0xFFFFu);
        const float ox = static_cast<float>(xOffset) / 16.0f, oy = static_cast<float>(yOffset) / 16.0f;
        glUseProgram(gpu.program);
        glBindVertexArray(gpu.vao);
        glUniform1i(gpu.locTex, 0);
        glActiveTexture(GL_TEXTURE0);
        GLint sampler = 0;
        glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
        glBindSampler(0, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const auto into = [&](const PassGpu::Work &work) {
            glBindFramebuffer(GL_FRAMEBUFFER, work.fbo);
            glViewport(0, 0, work.width, work.height);
        };
        const auto intoTarget = [&]() {
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            glViewport(0, 0, static_cast<GLsizei>(target.width * scale), static_cast<GLsizei>(target.height * scale));
        };
        switch (pass.kind)
        {
        case TargetPass::Kind::Glow:
        {
            // As the GS draws it at 1x: half and quarter sizes in GS pixels, then the internal scale on top.
            const uint32_t hw = static_cast<uint32_t>(w) / 2u, hh = static_cast<uint32_t>(h) / 2u;
            const uint32_t qw = hw / 2u, qh = hh / 2u;
            PassGpu::sized(gpu.half, static_cast<GLsizei>(hw * scale), static_cast<GLsizei>(hh * scale));
            PassGpu::sized(gpu.quarter, static_cast<GLsizei>(qw * scale), static_cast<GLsizei>(qh * scale));
            PassGpu::sized(gpu.quarterB, static_cast<GLsizei>(qw * scale), static_cast<GLsizei>(qh * scale));
            PassGpu::sized(gpu.glow, static_cast<GLsizei>(hw * scale), static_cast<GLsizei>(hh * scale));
            glDisable(GL_BLEND);
            // The frame's part with alpha at least alphaRef, over black: each half pixel the average of its 2 x 2.
            into(gpu.half);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            {
                const PassGpu::BorrowedFilter borrowed(target.color, true);
                gpu.mode(0, 0, -1.0f, false, static_cast<float>(pass.alphaRef));
                gpu.rect(0, 0, static_cast<float>(hw), static_cast<float>(hh), 0, 0, w / tw, h / th, static_cast<float>(hw), static_cast<float>(hh));
            }
            gpu.mode(1);
            into(gpu.quarter);
            PassGpu::filter(gpu.half.texture, true);
            gpu.rect(0, 0, static_cast<float>(qw), static_cast<float>(qh), 0, 0, 1, 1, static_cast<float>(qw), static_cast<float>(qh));
            // Each blur: the quarter picture copied half a pixel down-right, then back half a pixel up-left, bilinear.
            const float du = 0.5f / static_cast<float>(qw), dv = 0.5f / static_cast<float>(qh);
            for (uint32_t i = 0; i < pass.blurs; ++i)
            {
                into(gpu.quarterB);
                PassGpu::filter(gpu.quarter.texture, true);
                gpu.rect(0, 0, static_cast<float>(qw), static_cast<float>(qh), du, dv, 1 + du, 1 + dv, static_cast<float>(qw), static_cast<float>(qh));
                into(gpu.quarter);
                PassGpu::filter(gpu.quarterB.texture, true);
                gpu.rect(0, 0, static_cast<float>(qw), static_cast<float>(qh), -du, -dv, 1 - du, 1 - dv, static_cast<float>(qw), static_cast<float>(qh));
            }
            into(gpu.glow);
            PassGpu::filter(gpu.quarter.texture, true);
            gpu.rect(0, 0, static_cast<float>(hw), static_cast<float>(hh), 0, 0, 1, 1, static_cast<float>(hw), static_cast<float>(hh));
            gpu.glowWidth = hw;
            gpu.glowHeight = hh;
            // Back over the target, RGB only.
            const Blend b = blendOf(pass.alpha, true);
            if (!b.known && gpu.unknownBlends++ == 0u)
                std::fprintf(stderr, "[native] target pass: ALPHA %#llx not a form the passes draw (drawn as Cs)\n",
                             static_cast<unsigned long long>(pass.alpha));
            if (b.mode < 0)
                break;
            intoTarget();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            PassGpu::glBlend(b.known ? b.mode : 0);
            PassGpu::filter(gpu.glow.texture, true);
            gpu.mode(2, b.known ? b.mode : 0, b.fix, true);
            gpu.rect(0, 0, w, h, 0, 0, 1, 1, tw, th, pass.rgba);
            break;
        }
        case TargetPass::Kind::Volumes:
        {
            if (!gpu.stencilTarget(target))
                break;
            glViewport(0, 0, static_cast<GLsizei>(target.width * scale), static_cast<GLsizei>(target.height * scale));
            if (pass.clear)
            {
                glClearStencil(0);
                glClear(GL_STENCIL_BUFFER_BIT);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_ALWAYS);
                glDepthMask(GL_TRUE);
                const PassGpu::BorrowedFilter borrowed(target.depth, false);
                gpu.mode(4);
                gpu.rect(0, 0, tw, th, 0, 0, 1, 1, tw, th);
            }
            if (pass.up.empty() && pass.down.empty())
                break;
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GEQUAL);
            glDepthMask(GL_FALSE);
            glEnable(GL_DEPTH_CLAMP);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xFFu);
            glStencilFunc(GL_ALWAYS, 0, 0xFFu);
            gpu.mode(3);
            for (int side = 0; side < 2; ++side)
            {
                const std::vector<GsVertex> &tris = side == 0 ? pass.up : pass.down;
                if (tris.empty())
                    continue;
                std::vector<PassVertex> vertices;
                vertices.reserve(tris.size());
                for (const GsVertex &v : tris)
                {
                    const double z = static_cast<double>(v.z & 0xFFFFFFu) * target.zScale;
                    vertices.push_back({v.x - ox, v.y - oy, static_cast<float>(z), 0.0f, 0.0f, 0u});
                }
                glStencilOp(GL_KEEP, GL_KEEP, side == 0 ? GL_INCR_WRAP : GL_DECR_WRAP);
                gpu.draw(vertices, GL_TRIANGLES, tw, th);
            }
            glDisable(GL_DEPTH_CLAMP);
            break;
        }
        case TargetPass::Kind::Darken:
        {
            if (!gpu.stencilTarget(target))
                break;
            glViewport(0, 0, static_cast<GLsizei>(target.width * scale), static_cast<GLsizei>(target.height * scale));
            const Blend b = blendOf(pass.alpha, pass.blend);
            if (b.mode >= 0)
            {
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, (target.fbmsk >> 24) == 0xFFu ? GL_FALSE : GL_TRUE);
                PassGpu::glBlend(b.known ? b.mode : 0);
                gpu.mode(3, b.known ? b.mode : 0, b.fix);
                gpu.rect(0, 0, w, h, 0, 0, 0, 0, tw, th, pass.rgba);
            }
            glDisable(GL_STENCIL_TEST);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            break;
        }
        case TargetPass::Kind::Probe:
            m_probeTarget = {target.framebuffer, target.width, target.height, scale, xOffset, yOffset, target.zScale};
            readProbes();
            break;
        case TargetPass::Kind::KeepDisplay:
        {
            PassGpu::sized(gpu.display, static_cast<GLsizei>(static_cast<uint32_t>(w) * scale), static_cast<GLsizei>(static_cast<uint32_t>(h) * scale));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu.display.fbo);
            glBlitFramebuffer(0, 0, gpu.display.width, gpu.display.height, 0, 0, gpu.display.width, gpu.display.height, GL_COLOR_BUFFER_BIT,
                              GL_NEAREST);
            gpu.displayWidth = static_cast<uint32_t>(w);
            gpu.displayHeight = static_cast<uint32_t>(h);
            break;
        }
        case TargetPass::Kind::Sprites:
        {
            GLuint texture = 0;
            float sw = 1.0f, sh = 1.0f;
            if (pass.surface == TargetPass::Surface::Target)
            {
                const GLsizei cw = static_cast<GLsizei>(target.width * scale), ch = static_cast<GLsizei>(target.height * scale);
                PassGpu::sized(gpu.copy, cw, ch);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu.copy.fbo);
                glBlitFramebuffer(0, 0, cw, ch, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                texture = gpu.copy.texture;
                sw = tw;
                sh = th;
            }
            else if (pass.surface == TargetPass::Surface::Glow && gpu.glow.texture)
            {
                texture = gpu.glow.texture;
                sw = static_cast<float>(gpu.glowWidth);
                sh = static_cast<float>(gpu.glowHeight);
            }
            else if (pass.surface == TargetPass::Surface::Display && gpu.display.texture)
            {
                texture = gpu.display.texture;
                sw = static_cast<float>(gpu.displayWidth);
                sh = static_cast<float>(gpu.displayHeight);
            }
            if (!texture || pass.up.size() < 2u)
                break;
            const Blend b = blendOf(pass.alpha, pass.blend);
            if (!b.known && gpu.unknownBlends++ == 0u)
                std::fprintf(stderr, "[native] target pass: ALPHA %#llx not a form the passes draw (drawn as Cs)\n",
                             static_cast<unsigned long long>(pass.alpha));
            if (b.mode < 0)
                break;
            intoTarget();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, (target.fbmsk >> 24) == 0xFFu ? GL_FALSE : GL_TRUE);
            PassGpu::glBlend(b.known ? b.mode : 0);
            PassGpu::filter(texture, pass.linear);
            gpu.mode(2, b.known ? b.mode : 0, b.fix, pass.modulate, 0.0f, pass.texelAlpha);
            std::vector<PassVertex> vertices;
            vertices.reserve(pass.up.size() * 3u);
            if (pass.triangles)
                for (const GsVertex &v : pass.up)
                    vertices.push_back({v.x - ox, v.y - oy, 0, v.s / sw, v.t / sh, v.rgba});
            for (size_t i = 0; !pass.triangles && i + 1u < pass.up.size(); i += 2u)
            {
                const GsVertex &a = pass.up[i], &c = pass.up[i + 1u];
                const PassVertex p0{a.x - ox, a.y - oy, 0, a.s / sw, a.t / sh, c.rgba}, p1{c.x - ox, a.y - oy, 0, c.s / sw, a.t / sh, c.rgba},
                    p2{a.x - ox, c.y - oy, 0, a.s / sw, c.t / sh, c.rgba}, p3{c.x - ox, c.y - oy, 0, c.s / sw, c.t / sh, c.rgba};
                vertices.insert(vertices.end(), {p0, p1, p2, p1, p3, p2});
            }
            gpu.draw(vertices, GL_TRIANGLES, tw, th);
            break;
        }
        }
        glBindSampler(0, static_cast<GLuint>(sampler));
    }
}
