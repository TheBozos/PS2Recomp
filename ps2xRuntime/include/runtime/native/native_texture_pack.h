#ifndef PS2X_NATIVE_TEXTURE_PACK_H
#define PS2X_NATIVE_TEXTURE_PACK_H

// Native PC rendering, shared by the ports: a game's textures converted offline from its own files into a texture
// pack (a port's converter through tools/native_pack, texture.py), so nothing is read from GS memory or the game's
// texture records while it runs. The pack holds, per file the port names, its records in order: PSM, width, height
// and the texels unpacked (indexed formats a byte per texel, CT32 / CT24 four bytes (CT24's alpha 0), CT16 two
// bytes). A port finds a record by its own key (the game's texture id, the n-th record its loader sets up, the VRAM
// block a palette was loaded to) at its hooks; the decoders here make the RGBA image the Renderer takes, with the GS's
// rules for the palette (CSM1) and for CT24 / CT16 alpha (TEXA).
//
// docs/native_packs.md has the format and TMB's key scheme as the worked example.

#include "runtime/native/native_texture_cache.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::native::packs
{
    enum Psm : uint32_t
    {
        PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0a, PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1b, PSMT4HL = 0x24, PSMT4HH = 0x2c
    };
    inline bool psmIsH(uint32_t psm) { return psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH; }
    inline bool psmIndexed(uint32_t psm) { return psm == PSMT8 || psm == PSMT4 || psmIsH(psm); }
    inline bool psmEightBit(uint32_t psm) { return psm == PSMT8 || psm == PSMT8H; }

    struct TextureRecord
    {
        uint32_t psm = 0, width = 0, height = 0;
        std::vector<uint8_t> texels; // indexed: a byte per texel; CT32 / CT24: 4 bytes; CT16: 2 bytes
    };

    class TexturePack
    {
    public:
        static constexpr char kMagic[9] = "PS2XTEX1";

        // Reads the whole pack (false: missing or not this format; it stays empty). Records never move once loaded:
        // a port may keep pointers to them.
        bool load(const std::string &path);
        bool loaded() const { return m_ok; }

        // A file's records in order (null: not in the pack).
        const std::vector<TextureRecord> *file(const std::string &name) const;
        // The n-th record of these files taken in order (null past their end or at a file the pack lacks).
        const TextureRecord *nth(const std::vector<std::string> &files, uint32_t n) const;
        size_t fileCount() const { return m_files.size(); }
        size_t recordCount() const { return m_records; }

    private:
        std::unordered_map<std::string, std::vector<TextureRecord>> m_files;
        size_t m_records = 0;
        bool m_ok = false;
    };

    // A CT16 colour as RGBA with the GS's alpha (TEXA: TA0, TA1, AEM).
    uint32_t ct16ToRgba(uint32_t c, uint64_t texa);
    // A direct-colour record's entry `index` as RGBA with the GS's alpha (0x80 is 1.0): CT32 as stored, CT24 with
    // TEXA's TA0 (0 under AEM for black), CT16 / CT16S by ct16ToRgba.
    uint32_t recordColour(const TextureRecord &record, uint32_t index, uint64_t texa);
    // An indexed texture's palette (16 or 256 entries by the PSM, CSA for T4): entry i from the CLUT position CSM1 puts it
    // at, entry(px, py, rgba) the port's (false: none there; the entry stays 0).
    template <typename Entry>
    void csm1Palette(uint32_t psm, uint32_t csa, Entry &&entry, uint32_t out[256])
    {
        const bool eight = psmEightBit(psm);
        for (uint32_t i = 0; i < (eight ? 256u : 16u); ++i)
        {
            uint32_t px = 0, py = 0;
            csm1Position(eight ? i : (csa * 16u + i) & 0xffu, px, py);
            entry(px, py, out[i]);
        }
    }
    // The image a TEX0 of width x height and this PSM shows from a record: its texels where the record has them (0 past
    // its size), through the palette when the PSM is indexed.
    void decodeRecord(TextureCache::Texture &out, uint32_t width, uint32_t height, uint32_t psm, const TextureRecord &texels,
                      const uint32_t palette[256], uint64_t texa);
}

#endif
