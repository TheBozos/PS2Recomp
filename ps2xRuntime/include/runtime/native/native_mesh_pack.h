#ifndef PS2X_NATIVE_MESH_PACK_H
#define PS2X_NATIVE_MESH_PACK_H

// Native PC rendering, shared by the ports: a game's 3D models converted offline from its own files (the prebuilt
// VIF / GIF packets a PS2 game keeps on disc) into a mesh pack, and looked up while the game runs by the file the
// game loaded and the object's offset in it. No packet is read at runtime: a port hooks the game's per-object draw
// call, finds the object here, and builds its draws from the pack and the EE state the game computes per object.
//
// docs/native_packs.md has the format and what a port supplies. The pack is written by a port's converter through
// tools/native_pack (mesh.py); this is its loader and object store:
// - MeshPack: the pack's files, each with its objects (sorted by offset) and its GS register blocks (A+D writes the
//   draws depend on, recorded offline at their offset in the image);
// - MeshObject: an object's port-defined kind and attributes, its positions, its strips (PS2 triangle strips as the
//   VU program reads them: a GIF tag's PRIM, the material word, a texture slot, 16-byte vertices) and any number of
//   port-defined sections (tagged byte blocks: morph deltas, skin weights, program parameters, ...);
// - LoadedImages: where the game loaded each of the pack's files in EE memory (checked by size and CRC32), so an
//   EE address resolves to (file, offset).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::native::packs
{
    // CRC-32 (IEEE, zlib's crc32): a loaded image against the one the pack was converted from.
    uint32_t crc32(const uint8_t *data, size_t size);

    // A section tag from four characters ("AUX "), as the writer packs it (little-endian u32).
    constexpr uint32_t fourcc(const char (&s)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
    }

    // A strip's header in the pack (16 bytes, followed by its vertices).
    struct StripHeader
    {
        uint16_t prim = 0;     // GIF PRIM (11 bits)
        uint16_t gifFlags = 0; // bit 0 PRE, bit 1 EOP
        uint32_t material = 0; // the port's material word (TMB: the GIF tag's word 3, its texture header)
        int32_t slot = -1;     // texture slot the strip selects, -1: keeps the current texture
        uint32_t vertexCount = 0;
    };
    static_assert(sizeof(StripHeader) == 16, "pack layout");

    // A strip vertex in the pack (16 bytes).
    struct StripVertex
    {
        uint16_t position = 0;   // index into the object's positions
        uint8_t normal[3] = {};  // raw normal bytes, as the packet has them (the port's program decodes them)
        uint8_t flags = 0;       // kStripVertex*
        uint8_t rgba[4] = {};    // colour, 0x80 = 1.0
        int16_t s = 0, t = 0;    // texture coordinates as the packet has them (TMB: signed 4.12)
        uint16_t extra = 0;      // the port's
    };
    static_assert(sizeof(StripVertex) == 16, "pack layout");
    constexpr uint8_t kStripVertexInline = 1u; // its position is an inline point (a sprite's or ribbon's)
    constexpr uint8_t kStripVertexAdc = 2u;    // ADC: the triangle this vertex closes is not drawn
    constexpr uint8_t kStripVertexCorner = 4u; // a corner the program expands from its point

    // A port-defined block of an object (its tag, its bytes).
    struct MeshSection
    {
        uint32_t tag = 0;
        uint32_t size = 0;
        const uint8_t *data = nullptr;
    };

    struct MeshObject
    {
        uint32_t offset = 0; // in its file's image (the port's key)
        uint32_t kind = 0;   // the port's
        const uint8_t *attributes = nullptr; // the port's fixed fields (flags, bounds, texture numbers, ...)
        uint32_t attributeSize = 0;
        const float *positions = nullptr; // x, y, z, w each
        uint32_t positionCount = 0;
        const uint8_t *strips = nullptr; // stripCount (StripHeader, vertexCount StripVertex) in a row
        uint32_t stripCount = 0;
        std::vector<MeshSection> sections;

        const MeshSection *section(uint32_t tag) const
        {
            for (const MeshSection &s : sections)
                if (s.tag == tag)
                    return &s;
            return nullptr;
        }
        // An attribute field at a byte offset (0 past the end).
        template <typename T>
        T attribute(uint32_t at) const
        {
            T value{};
            if (attributes && at + sizeof(T) <= attributeSize)
                std::memcpy(&value, attributes + at, sizeof(T));
            return value;
        }
    };

    // Walks an object's strips in order.
    class StripCursor
    {
    public:
        explicit StripCursor(const MeshObject &object) : m_at(object.strips), m_left(object.stripCount) {}
        // The next strip (false after the last): its header and its vertices (unaligned: read through memcpy).
        bool next(StripHeader &header, const uint8_t *&vertices)
        {
            if (m_left == 0u || !m_at)
                return false;
            --m_left;
            std::memcpy(&header, m_at, sizeof(header));
            vertices = m_at + sizeof(header);
            m_at = vertices + static_cast<size_t>(header.vertexCount) * sizeof(StripVertex);
            return true;
        }

    private:
        const uint8_t *m_at;
        uint32_t m_left;
    };

    // GS register writes recorded offline at an offset of an image (an A+D block the game sends with its draws:
    // a GS context, a material's registers).
    struct RegisterBlock
    {
        uint32_t offset = 0;
        std::vector<std::pair<uint32_t, uint64_t>> writes; // (GS register address, value)
    };

    struct MeshFile
    {
        std::string name; // as the port names it (TMB: the disc file's name, upper case, no directory)
        uint32_t size = 0, crc = 0; // the image the objects were converted from
        std::vector<MeshObject> objects;    // by offset
        std::vector<RegisterBlock> blocks;  // by offset

        const MeshObject *object(uint32_t offset) const;
        const RegisterBlock *block(uint32_t offset) const;
    };

    class MeshPack
    {
    public:
        static constexpr char kMagic[9] = "PS2XMSH1";

        // Reads the whole pack (false: missing or not this format, error() says which; the pack stays empty).
        bool load(const std::string &path);
        bool loaded() const { return m_ok; }
        const std::string &error() const { return m_error; }

        const std::vector<MeshFile> &files() const { return m_files; }
        const MeshFile *file(const std::string &name, uint32_t *index = nullptr) const;
        size_t objectCount() const;

    private:
        std::vector<uint8_t> m_blob; // the objects point into it
        std::vector<MeshFile> m_files;
        std::string m_error;
        bool m_ok = false;
    };

    // Where the game loaded the pack's files in EE memory: each image the port reports (a file loaded at an address)
    // is checked against the pack's file of that name by size and CRC32. Thread-safe.
    class LoadedImages
    {
    public:
        struct Image
        {
            uint32_t base = 0, size = 0;
            const MeshFile *file = nullptr; // null: not in the pack, or not the image it was converted from
            uint32_t index = 0;             // the file's index in the pack
            uint32_t first = 0;             // a copy of part of the file: it holds base + first .. base + size
        };
        struct Bound
        {
            Image image;
            const MeshFile *named = nullptr; // the pack's file of that name, matching or not
            uint32_t crc = 0;                // the loaded image's (0 when the size differs)
        };

        // An image of `size` bytes at `base` (rdram: EE memory, base masked to 32 MB), named as the pack names files.
        // replace: the game's list of loaded files starts again with this one.
        Bound bind(const MeshPack &pack, const uint8_t *rdram, const std::string &name, uint32_t base, uint32_t size, bool replace);
        // The game's copy at `to` of `size` bytes from `from`, an address in `source` (a bound image): the file's objects
        // in that range are found at the copy. Not checked against the file: the bytes are the source's.
        Image bindCopy(const Image &source, uint32_t to, uint32_t from, uint32_t size);
        void clear();
        // The images in the order they were bound, under the lock: fn(const Image &) returns false to stop.
        template <typename F>
        void forEach(F &&fn) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const Image &image : m_images)
                if (!fn(image))
                    return;
        }
        // The last image bound that holds this address (null file included).
        bool find(uint32_t address, Image &out) const;

    private:
        mutable std::mutex m_mutex;
        std::vector<Image> m_images;
    };
}

#endif
