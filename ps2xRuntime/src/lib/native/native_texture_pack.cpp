// The texture pack's loader and decoders (runtime/native/native_texture_pack.h, docs/native_packs.md).

#include "runtime/native/native_texture_pack.h"

#include <cstring>
#include <fstream>

namespace ps2x::native::packs
{
    bool TexturePack::load(const std::string &path)
    {
        m_files.clear();
        m_records = 0;
        m_ok = false;
        std::ifstream in(path, std::ios::binary);
        char magic[8] = {};
        uint32_t files = 0;
        if (!in.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0 || !in.read(reinterpret_cast<char *>(&files), 4))
            return false;
        for (uint32_t f = 0; f < files && in; ++f)
        {
            uint16_t length = 0;
            in.read(reinterpret_cast<char *>(&length), 2);
            std::string name(length, '\0');
            in.read(name.data(), length);
            uint32_t count = 0;
            in.read(reinterpret_cast<char *>(&count), 4);
            std::vector<TextureRecord> &list = m_files[name];
            list.resize(count);
            for (TextureRecord &r : list)
            {
                uint32_t header[4] = {};
                in.read(reinterpret_cast<char *>(header), 16);
                r.psm = header[0];
                r.width = header[1];
                r.height = header[2];
                r.texels.resize(header[3]);
                in.read(reinterpret_cast<char *>(r.texels.data()), header[3]);
            }
            m_records += count;
        }
        m_ok = true;
        return true;
    }

    const std::vector<TextureRecord> *TexturePack::file(const std::string &name) const
    {
        const auto it = m_files.find(name);
        return it != m_files.end() ? &it->second : nullptr;
    }

    const TextureRecord *TexturePack::nth(const std::vector<std::string> &files, uint32_t n) const
    {
        for (const std::string &name : files)
        {
            const auto it = m_files.find(name);
            if (it == m_files.end())
                return nullptr;
            if (n < it->second.size())
                return &it->second[n];
            n -= static_cast<uint32_t>(it->second.size());
        }
        return nullptr;
    }

    uint32_t ct16ToRgba(uint32_t c, uint64_t texa)
    {
        const uint32_t r = (c & 0x1fu) << 3, g = ((c >> 5) & 0x1fu) << 3, b = ((c >> 10) & 0x1fu) << 3;
        const uint32_t ta0 = texa & 0xffu, ta1 = (texa >> 32) & 0xffu;
        const bool aem = (texa >> 15) & 1u;
        const uint32_t a = (c & 0x8000u) ? ta1 : ((aem && (c & 0x7fffu) == 0u) ? 0u : ta0);
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t recordColour(const TextureRecord &r, uint32_t index, uint64_t texa)
    {
        if (r.psm == PSMCT16 || r.psm == PSMCT16S)
        {
            uint16_t c;
            std::memcpy(&c, r.texels.data() + index * 2u, 2);
            return ct16ToRgba(c, texa);
        }
        uint32_t c;
        std::memcpy(&c, r.texels.data() + index * 4u, 4);
        if (r.psm == PSMCT24)
        {
            const bool aem = (texa >> 15) & 1u;
            const uint32_t a = (aem && (c & 0xffffffu) == 0u) ? 0u : static_cast<uint32_t>(texa & 0xffu);
            return (c & 0xffffffu) | (a << 24);
        }
        return c;
    }

    void decodeRecord(TextureCache::Texture &out, uint32_t width, uint32_t height, uint32_t psm, const TextureRecord &texels,
                      const uint32_t palette[256], uint64_t texa)
    {
        const bool indexed = psmIndexed(psm);
        out.width = width;
        out.height = height;
        out.rgba.resize(static_cast<size_t>(width) * height);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t c = 0;
                if (x < texels.width && y < texels.height)
                {
                    const uint32_t at = y * texels.width + x;
                    c = indexed ? palette[texels.texels[at]] : recordColour(texels, at, texa);
                }
                out.rgba[static_cast<size_t>(y) * width + x] = c;
            }
    }
}
