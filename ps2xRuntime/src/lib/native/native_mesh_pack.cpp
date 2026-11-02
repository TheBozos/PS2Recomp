// The mesh pack's loader and object store (runtime/native/native_mesh_pack.h, docs/native_packs.md).

#include "runtime/native/native_mesh_pack.h"

#include <algorithm>
#include <fstream>

namespace ps2x::native::packs
{
    uint32_t crc32(const uint8_t *data, size_t size)
    {
        static uint32_t s_table[256];
        static const bool s_made = []
        {
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                s_table[i] = c;
            }
            return true;
        }();
        (void)s_made;
        uint32_t c = 0xffffffffu;
        for (size_t i = 0; i < size; ++i)
            c = s_table[(c ^ data[i]) & 0xffu] ^ (c >> 8);
        return c ^ 0xffffffffu;
    }

    const MeshObject *MeshFile::object(uint32_t offset) const
    {
        const auto it = std::lower_bound(objects.begin(), objects.end(), offset, [](const MeshObject &o, uint32_t v) { return o.offset < v; });
        return it != objects.end() && it->offset == offset ? &*it : nullptr;
    }

    const RegisterBlock *MeshFile::block(uint32_t offset) const
    {
        const auto it = std::lower_bound(blocks.begin(), blocks.end(), offset, [](const RegisterBlock &b, uint32_t v) { return b.offset < v; });
        return it != blocks.end() && it->offset == offset ? &*it : nullptr;
    }

    namespace
    {
        // A bounds-checked reader over the pack's bytes.
        struct Reader
        {
            const uint8_t *p, *end;
            bool ok = true;
            bool has(size_t n) const { return ok && static_cast<size_t>(end - p) >= n; }
            template <typename T>
            T get()
            {
                T v{};
                if (!has(sizeof(T)))
                {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, p, sizeof(T));
                p += sizeof(T);
                return v;
            }
            const uint8_t *skip(size_t n)
            {
                if (!has(n))
                {
                    ok = false;
                    return nullptr;
                }
                const uint8_t *at = p;
                p += n;
                return at;
            }
        };
    }

    bool MeshPack::load(const std::string &path)
    {
        m_blob.clear();
        m_files.clear();
        m_ok = false;
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            m_error = "missing";
            return false;
        }
        m_blob.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        in.read(reinterpret_cast<char *>(m_blob.data()), static_cast<std::streamsize>(m_blob.size()));
        Reader r{m_blob.data(), m_blob.data() + m_blob.size()};
        if (m_blob.size() < 12u || std::memcmp(m_blob.data(), kMagic, 8) != 0)
        {
            m_error = "not a mesh pack of this version";
            m_blob.clear();
            return false;
        }
        r.skip(8);
        const uint32_t fileCount = r.get<uint32_t>();
        for (uint32_t f = 0; f < fileCount && r.ok; ++f)
        {
            MeshFile file;
            const uint16_t length = r.get<uint16_t>();
            if (const uint8_t *name = r.skip(length))
                file.name.assign(reinterpret_cast<const char *>(name), length);
            file.size = r.get<uint32_t>();
            file.crc = r.get<uint32_t>();
            const uint32_t count = r.get<uint32_t>();
            if (!r.has(static_cast<size_t>(count) * 24u))
                break;
            file.objects.resize(count);
            for (MeshObject &o : file.objects)
            {
                o.offset = r.get<uint32_t>();
                o.kind = r.get<uint32_t>();
                o.attributeSize = r.get<uint32_t>();
                o.attributes = r.skip(o.attributeSize);
                o.positionCount = r.get<uint32_t>();
                o.positions = reinterpret_cast<const float *>(r.skip(static_cast<size_t>(o.positionCount) * 16u));
                const uint32_t sections = r.get<uint32_t>();
                for (uint32_t s = 0; s < sections && r.ok; ++s)
                {
                    MeshSection section;
                    section.tag = r.get<uint32_t>();
                    section.size = r.get<uint32_t>();
                    section.data = r.skip(section.size);
                    o.sections.push_back(section);
                }
                o.stripCount = r.get<uint32_t>();
                o.strips = r.p;
                for (uint32_t s = 0; s < o.stripCount && r.ok; ++s)
                {
                    const uint8_t *header = r.skip(sizeof(StripHeader));
                    if (!header)
                        break;
                    StripHeader h;
                    std::memcpy(&h, header, sizeof(h));
                    r.skip(static_cast<size_t>(h.vertexCount) * sizeof(StripVertex));
                }
                if (!r.ok)
                    break;
            }
            const uint32_t blocks = r.get<uint32_t>();
            if (!r.has(static_cast<size_t>(blocks) * 8u))
                r.ok = false;
            else
                file.blocks.resize(blocks);
            for (RegisterBlock &b : file.blocks)
            {
                b.offset = r.get<uint32_t>();
                const uint32_t writes = r.get<uint32_t>();
                if (!r.has(static_cast<size_t>(writes) * 12u))
                {
                    r.ok = false;
                    break;
                }
                b.writes.resize(writes);
                for (auto &w : b.writes)
                {
                    w.first = r.get<uint32_t>();
                    w.second = r.get<uint64_t>();
                }
            }
            if (!r.ok)
                break;
            m_files.push_back(std::move(file));
        }
        if (!r.ok)
        {
            m_error = "truncated";
            m_files.clear();
            m_blob.clear();
            return false;
        }
        m_ok = true;
        return true;
    }

    const MeshFile *MeshPack::file(const std::string &name, uint32_t *index) const
    {
        for (size_t i = 0; i < m_files.size(); ++i)
            if (m_files[i].name == name)
            {
                if (index)
                    *index = static_cast<uint32_t>(i);
                return &m_files[i];
            }
        return nullptr;
    }

    size_t MeshPack::objectCount() const
    {
        size_t n = 0;
        for (const MeshFile &f : m_files)
            n += f.objects.size();
        return n;
    }

    LoadedImages::Bound LoadedImages::bind(const MeshPack &pack, const uint8_t *rdram, const std::string &name, uint32_t base, uint32_t size, bool replace)
    {
        Bound bound;
        bound.image.base = base;
        bound.image.size = size;
        uint32_t index = 0;
        if (const MeshFile *file = pack.file(name, &index))
        {
            bound.named = file;
            bound.crc = size == file->size && (base & 0x1ffffffu) + size <= 0x2000000u ? crc32(rdram + (base & 0x1ffffffu), size) : 0u;
            if (size == file->size && bound.crc == file->crc)
            {
                bound.image.file = file;
                bound.image.index = index;
            }
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (replace)
            m_images.clear();
        m_images.push_back(bound.image);
        return bound;
    }

    LoadedImages::Image LoadedImages::bindCopy(const Image &source, uint32_t to, uint32_t from, uint32_t size)
    {
        Image copy = source;
        const uint32_t offset = from - source.base;
        copy.base = to - offset;
        copy.first = offset;
        copy.size = std::min(offset + size, source.size);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_images.push_back(copy);
        return copy;
    }

    void LoadedImages::clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_images.clear();
    }

    bool LoadedImages::find(uint32_t address, Image &out) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool found = false;
        for (const Image &image : m_images)
            if (address >= image.base + image.first && address < image.base + image.size)
            {
                out = image;
                found = true;
            }
        return found;
    }
}
