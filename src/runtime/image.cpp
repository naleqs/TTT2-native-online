#include "image.h"
#include "guest.h"
#include "memory.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>

namespace tt2
{
    ImageInfo g_image;
    std::map<uint32_t, uint32_t> g_xex_headers; // key -> inline value, or guest address of the data

    namespace
    {
        // Storage for the kernel's exported data, referenced by the image's data imports.
        uint32_t make_variable(const std::string& name)
        {
            auto block = [](uint32_t size) { return g_memory.system_alloc(size); };

            if (name == "XboxHardwareInfo")
            {
                // be32 Flags, u8 NumberOfProcessors, u8 PCIBridgeRevisionID, u8 Reserved[6], be16 BldrMagic, be16 BldrFlags
                const uint32_t v = block(16);
                store32(v, 0x00000020);
                g_base[v + 4] = 6;
                return v;
            }
            if (name == "XboxKrnlVersion")
            {
                const uint32_t v = block(8);
                store16(v, 2);
                store16(v + 2, 0);
                store16(v + 4, 17559);
                store16(v + 6, 0);
                return v;
            }
            if (name == "KeTimeStampBundle")
                return block(24); // updated by the timebase thread
            if (name == "ExLoadedCommandLine")
            {
                const uint32_t v = block(0x100);
                std::memcpy(g_base + v, "\"default.xex\"", 13);
                return v;
            }
            if (name == "XexExecutableModuleHandle")
            {
                const uint32_t v = block(4);
                store32(v, block(0x100)); // fake loader entry
                return v;
            }
            if (name == "VdGpuClockInMHz")
            {
                const uint32_t v = block(4);
                store32(v, 500);
                return v;
            }
            if (name == "VdHSIOCalibrationLock")
            {
                const uint32_t v = block(0x1C);
                g_base[v] = 1;
                store32(v + 0x10, 0xFFFFFFFF);
                return v;
            }
            // ExThreadObjectType, KeDebugMonitorData, KeCertMonitorData, VdGlobalDevice,
            // VdGlobalXamDevice, ...: zeroed storage is what an idle console has.
            return block(0x40);
        }

        bool is_data_export(const std::string& name)
        {
            static const char* names[] = {
                "XboxHardwareInfo", "XboxKrnlVersion", "KeTimeStampBundle", "ExLoadedCommandLine", "XexExecutableModuleHandle",
                "VdGpuClockInMHz", "VdHSIOCalibrationLock", "ExThreadObjectType", "KeDebugMonitorData", "KeCertMonitorData",
                "VdGlobalDevice", "VdGlobalXamDevice", "XboxKrnlBaseVersion", "ExLoadedImageName", "KeDebugMonitorData",
            };
            for (const char* n : names)
                if (name == n)
                    return true;
            return false;
        }

        uint32_t g_timestamp_bundle = 0;
    }

    uint32_t timestamp_bundle() { return g_timestamp_bundle; }

    bool load_image(const std::filesystem::path& dir, std::string& error)
    {
        std::ifstream meta(dir / "tt2_image.json");
        if (!meta)
        {
            error = "missing " + (dir / "tt2_image.json").string() + " - run Setup.bat";
            return false;
        }
        const auto j = nlohmann::json::parse(meta, nullptr, false);
        if (j.is_discarded())
        {
            error = "tt2_image.json is damaged - run Setup.bat -Rebuild";
            return false;
        }
        g_image.base = j.value("base", 0u);
        g_image.size = j.value("size", 0u);
        g_image.entry = j.value("entry", 0u);
        g_image.stack_size = j.value("stack_size", 0x40000u);
        if (j.contains("tls"))
        {
            g_image.tls_slots = j["tls"].value("slots", 0u);
            g_image.tls_raw_address = j["tls"].value("raw_address", 0u);
            g_image.tls_data_size = j["tls"].value("data_size", 0u);
            g_image.tls_raw_size = j["tls"].value("raw_size", 0u);
        }
        if (g_image.base != PPC_IMAGE_BASE || g_image.size != PPC_IMAGE_SIZE)
        {
            error = "tt2_image.json does not match the compiled game code - run Setup.bat -Rebuild";
            return false;
        }

        std::ifstream bin(dir / "tt2_image.bin", std::ios::binary);
        if (!bin.read(reinterpret_cast<char*>(g_base + g_image.base), g_image.size))
        {
            error = "cannot read tt2_image.bin";
            return false;
        }

        for (const auto& h : j.value("xex_headers", nlohmann::json::array()))
        {
            const uint32_t key = h.value("key", 0u);
            if (h.contains("value"))
                g_xex_headers[key] = h.value("value", 0u);
            else
            {
                const std::string hex = h.value("data", "");
                const uint32_t blob = g_memory.system_alloc(uint32_t(hex.size() / 2) + 4);
                for (size_t i = 0; i + 1 < hex.size(); i += 2)
                    g_base[blob + i / 2] = uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16));
                g_xex_headers[key] = blob;
            }
        }

        int filled = 0;
        for (const auto& v : j.value("variable_imports", nlohmann::json::array()))
        {
            const std::string name = v.value("name", "");
            const uint32_t slot = v.value("slot", 0u);
            if (!slot || !is_data_export(name))
                continue;
            const uint32_t storage = make_variable(name);
            if (name == "KeTimeStampBundle")
                g_timestamp_bundle = storage;
            store32(slot, storage);
            filled++;
        }
        log("image: %08X +%X, entry %08X, %d kernel data imports", g_image.base, g_image.size, g_image.entry, filled);
        return true;
    }
}

namespace tt2
{
    uint32_t image_data_start()
    {
        // The loaded image starts with its PE headers: e_lfanew, then IMAGE_NT_HEADERS and the
        // section table (little-endian).
        const uint32_t b = g_image.base;
        auto le16 = [](uint32_t a) { return uint32_t(g_base[a] | (g_base[a + 1] << 8)); };
        auto le32 = [&](uint32_t a) { return le16(a) | (le16(a + 2) << 16); };
        if (g_base[b] != 'M' || g_base[b + 1] != 'Z')
            return b;
        const uint32_t nt = b + le32(b + 0x3C);
        const uint32_t sections = le16(nt + 6);
        const uint32_t table = nt + 24 + le16(nt + 20);
        for (uint32_t i = 0; i < sections; i++)
        {
            const uint32_t s = table + i * 40;
            if (le32(s + 36) & 0x80000000) // IMAGE_SCN_MEM_WRITE
                return b + le32(s + 12);
        }
        return b;
    }
}
