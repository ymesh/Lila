#include "ui/TextureCache.h"

#include "bake/HeightNormal.h"
#include "core/Dds.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "render/HdrImage.h"
#include "render/Ocio.h"
#include "render/VirtualTexture.h"
#include "ui/ViewportTool.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QThreadPool>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <thread>
#include <unordered_map>

namespace lila::ui::textures
{
namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kMaxSize = 8192;

struct Result
{
    render::TexturePtr image;
    QImage thumb;
    std::string error;
    std::vector<fs::file_time_type> times;
    /** When not empty, the files the result came from (a UDIM set's tiles), replacing the slot's. */
    std::vector<std::string> files;
};

struct Slot
{
    bool loading = true;
    render::TexturePtr image;
    QImage thumb;
    std::string error;
    std::vector<std::string> files;
    std::vector<fs::file_time_type> times;
    Clock::time_point checked = Clock::now();
    std::function<Result()> work;
};

struct Cache
{
    std::unordered_map<std::string, Slot> entries;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
};

// Leaked on purpose: workers may still post results while the app shuts down.
Cache& cache()
{
    static Cache* c = new Cache;
    return *c;
}

fs::path toPath(const std::string& s)
{
    return fs::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

std::vector<fs::file_time_type> stamps(const std::vector<std::string>& files)
{
    std::vector<fs::file_time_type> out;
    for (const std::string& f : files)
    {
        std::error_code ec;
        out.push_back(f.empty() ? fs::file_time_type() : fs::last_write_time(toPath(f), ec));
    }
    return out;
}

/** Uncompressed or RLE truecolor TGA, which Qt can't read without its extra image formats. */
QImage readTga(const fs::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 18)
    {
        error = "not a TGA file";
        return {};
    }
    int idLen = d[0], cmapType = d[1], type = d[2];
    int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16], desc = d[17];
    if (cmapType != 0 || (type != 2 && type != 3 && type != 10 && type != 11) || (bpp != 8 && bpp != 24 && bpp != 32) || w <= 0 || h <= 0)
    {
        error = "unsupported TGA (only 8, 24 and 32 bit truecolor or grayscale)";
        return {};
    }
    size_t bytes = size_t(bpp / 8);
    size_t pos = 18 + size_t(idLen);
    std::vector<uint8_t> px(size_t(w) * size_t(h) * bytes);
    if (type == 2 || type == 3)
    {
        if (pos + px.size() > d.size())
        {
            error = "truncated TGA";
            return {};
        }
        std::copy(d.begin() + std::ptrdiff_t(pos), d.begin() + std::ptrdiff_t(pos + px.size()), px.begin());
    }
    else
    {
        size_t out = 0;
        while (out < px.size() && pos < d.size())
        {
            int header = d[pos++];
            size_t count = size_t(header & 0x7f) + 1;
            bool run = header & 0x80;
            for (size_t i = 0; i < count && out < px.size(); ++i)
            {
                size_t src = run ? pos : pos + i * bytes;
                if (src + bytes > d.size())
                    break;
                std::copy(d.begin() + std::ptrdiff_t(src), d.begin() + std::ptrdiff_t(src + bytes), px.begin() + std::ptrdiff_t(out));
                out += bytes;
            }
            pos += run ? bytes : count * bytes;
        }
    }
    QImage img(w, h, QImage::Format_RGBA8888);
    bool topDown = desc & 0x20;
    for (int y = 0; y < h; ++y)
    {
        uchar* row = img.scanLine(topDown ? y : h - 1 - y);
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * bytes];
            if (bytes == 1)
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = p[0];
            else
            {
                row[x * 4] = p[2];
                row[x * 4 + 1] = p[1];
                row[x * 4 + 2] = p[0];
            }
            row[x * 4 + 3] = bytes == 4 ? p[3] : 255;
        }
    }
    return img;
}

/**
 * A decoded file: 8-bit RGBA, or RGBA floats when the file holds more than 8 bits (16-bit PNG, TIFF and PSD as stored,
 * EXR and HDR linear).
 */
struct Pixels
{
    QImage rgba8;
    image::Image floats;
    bool linear = false;
    /** A DDS's compressed blocks, kept as they are for the GPU (srgb still to be set). */
    std::shared_ptr<render::TextureImage> blocks;
    /** The file's embedded colour profile; invalid when it carries none. */
    QColorSpace space;

    bool empty() const { return rgba8.isNull() && floats.rgba.empty() && !blocks; }
};

/**
 * A DDS file: its blocks as they are when allowBlocks and the GPU can take them with the file's own mips, else its
 * largest level within kMaxSize decoded.
 */
Pixels readDds(const fs::path& path, std::string& error, bool allowBlocks)
{
    Pixels px;
    dds::File f;
    if (!dds::read(path, f, &error))
        return px;
    size_t first = 0;
    while (first + 1 < f.levels.size() && std::max(f.levels[first].width, f.levels[first].height) > kMaxSize)
        ++first;
    const dds::Level& top = f.levels[first];
    // Without a mip chain, a decoded copy gets mips generated on the GPU, which blocks can't.
    const bool keepBlocks = allowBlocks && dds::isBlockCompressed(f.format) && f.format != dds::Format::BC6H &&
                            (f.levels.size() - first > 1 || std::max(top.width, top.height) <= 4);
    if (keepBlocks)
    {
        auto t = std::make_shared<render::TextureImage>();
        t->width = top.width;
        t->height = top.height;
        t->format = f.format == dds::Format::BC4 ? render::TextureFormat::R8
                    : f.format == dds::Format::BC5 ? render::TextureFormat::RG8
                                                   : render::TextureFormat::RGBA8;
        t->blocks = f.format;
        t->mipLevels = int(f.levels.size() - first);
        t->rgba.assign(f.data.begin() + std::ptrdiff_t(top.offset), f.data.end());
        px.blocks = std::move(t);
        return px;
    }
    if (f.format == dds::Format::BC6H)
    {
        px.floats = {top.width, top.height, dds::decodeFloat(f, first)};
        px.linear = true;
        return px;
    }
    std::vector<uint8_t> rgba = dds::decodeRgba8(f.format, top.width, top.height, f.data.data() + top.offset);
    px.rgba8 = QImage(rgba.data(), top.width, top.height, top.width * 4, QImage::Format_RGBA8888).copy();
    return px;
}

float encodeSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float decodeSrgb(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

/** True when every value sits on an 8-bit step, so 8 bits hold it exactly (a 16-bit file saved from 8-bit data). */
bool onByteGrid(const image::Image& img)
{
    for (float v : img.rgba)
    {
        const float s = v * 255.0f;
        if (v < 0.0f || v > 1.0f || std::abs(s - std::round(s)) > 1e-3f)
            return false;
    }
    return true;
}

/** Floats as 8-bit RGBA: linear data sRGB-encoded when encode, else stored as it is (clamped). */
QImage toRgba8(const image::Image& img, bool encode)
{
    QImage out(img.width, img.height, QImage::Format_RGBA8888);
    for (int y = 0; y < img.height; ++y)
    {
        uchar* row = out.scanLine(y);
        const float* src = &img.rgba[size_t(y) * size_t(img.width) * 4];
        for (int i = 0; i < img.width * 4; ++i)
        {
            const float v = encode && i % 4 != 3 ? encodeSrgb(src[i]) : std::clamp(src[i], 0.0f, 1.0f);
            row[i] = uchar(std::lround(v * 255.0f));
        }
    }
    return out;
}

/** Halves an image (box filter) until it fits kMaxSize. */
void shrinkToFit(image::Image& img)
{
    while (img.width > kMaxSize || img.height > kMaxSize)
    {
        const int w = std::max(img.width / 2, 1), h = std::max(img.height / 2, 1);
        std::vector<float> out(size_t(w) * size_t(h) * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k)
                    {
                        const int sx = std::min(x * 2 + (k & 1), img.width - 1), sy = std::min(y * 2 + (k >> 1), img.height - 1);
                        sum += img.rgba[(size_t(sy) * size_t(img.width) + size_t(sx)) * 4 + size_t(c)];
                    }
                    out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = sum * 0.25f;
                }
        img = {w, h, std::move(out)};
    }
}

/** full keeps the decoded image at its own size, else it is shrunk to kMaxSize. */
Pixels readPixels(const std::string& file, std::string& error, bool allowBlocks = false, bool full = false)
{
    LILA_ZONE();

    Pixels px;
    fs::path path = toPath(file);
    std::error_code ec;
    if (!fs::exists(path, ec))
    {
        error = "file not found";
        return px;
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".dds")
        return readDds(path, error, allowBlocks);
    if (ext == ".tga")
        px.rgba8 = readTga(path, error);
    else if (ext == ".hdr")
    {
        render::HdrImage hdr;
        if (render::loadHdr(path, hdr, error))
        {
            px.floats.width = hdr.width;
            px.floats.height = hdr.height;
            px.floats.rgba.reserve(size_t(hdr.width) * size_t(hdr.height) * 4);
            for (int y = 0; y < hdr.height; ++y)
                for (int x = 0; x < hdr.width; ++x)
                {
                    const glm::vec3 c = hdr.at(x, y);
                    px.floats.rgba.insert(px.floats.rgba.end(), {c.r, c.g, c.b, 1.0f});
                }
            px.linear = true;
        }
    }
    else if (ext == ".webp")
    {
        std::string err;
        px.floats = image::readWebp(path, &err);
        if (px.floats.rgba.empty())
            error = err;
        else
        {
            px.rgba8 = toRgba8(px.floats, false);
            px.floats = {};
        }
    }
    else if (ext == ".exr" || ext == ".tif" || ext == ".tiff" || ext == ".psd")
    {
        std::string err;
        px.floats = image::read(path, &err);
        if (!px.floats.icc.empty())
            px.space = QColorSpace::fromIccProfile(QByteArray(reinterpret_cast<const char*>(px.floats.icc.data()), qsizetype(px.floats.icc.size())));
        if (px.floats.rgba.empty())
        {
            // Qt's image format plugins read variants the built-in readers do not (tiled TIFF, compressed PSD).
            if (ext != ".exr")
                px.rgba8 = QImage(QString::fromStdU16String(path.u16string()));
            if (px.rgba8.isNull())
                error = err.empty() ? "unsupported or unreadable image" : err;
        }
        else if (ext == ".exr")
            px.linear = true;
        else if (onByteGrid(px.floats))
        {
            px.rgba8 = toRgba8(px.floats, false);
            px.floats = {};
        }
    }
    else
    {
        QImage img(QString::fromStdU16String(path.u16string()));
        px.space = img.colorSpace();
        if (img.isNull())
            error = "unsupported or unreadable image";
        else if (img.depth() > 32 || img.format() == QImage::Format_Grayscale16)
        {
            img = img.convertToFormat(QImage::Format_RGBA64);
            px.floats.width = img.width();
            px.floats.height = img.height();
            px.floats.rgba.resize(size_t(img.width()) * size_t(img.height()) * 4);
            for (int y = 0; y < img.height(); ++y)
            {
                const auto* row = reinterpret_cast<const quint16*>(img.constScanLine(y));
                for (int i = 0; i < img.width() * 4; ++i)
                    px.floats.rgba[size_t(y) * size_t(img.width()) * 4 + size_t(i)] = float(row[i]) / 65535.0f;
            }
        }
        else
            px.rgba8 = img;
    }
    if (!px.floats.rgba.empty())
    {
        if (!full)
            shrinkToFit(px.floats);
    }
    else if (!px.rgba8.isNull())
    {
        if (!full && (px.rgba8.width() > kMaxSize || px.rgba8.height() > kMaxSize))
            px.rgba8 = px.rgba8.scaled(kMaxSize, kMaxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        px.rgba8 = px.rgba8.convertToFormat(QImage::Format_RGBA8888);
    }
    return px;
}

/** A file as 8-bit RGBA, for packing and thumbnails: linear float data sRGB-encoded when encodeLinear. */
QImage readImage(const std::string& file, std::string& error, bool encodeLinear = false)
{
    Pixels px = readPixels(file, error);
    if (!px.floats.rgba.empty())
        return toRgba8(px.floats, encodeLinear && px.linear);
    return px.rgba8;
}

render::TexturePtr toTexture(const QImage& rgba, bool srgb)
{
    auto t = std::make_shared<render::TextureImage>();
    t->width = rgba.width();
    t->height = rgba.height();
    t->srgb = srgb;
    t->rgba.resize(size_t(t->width) * size_t(t->height) * 4);
    for (int y = 0; y < t->height; ++y)
        std::memcpy(t->rgba.data() + size_t(y) * size_t(t->width) * 4, rgba.constScanLine(y), size_t(t->width) * 4);
    return t;
}

/** Float pixels upload as RGBA16F, always linear: sRGB-encoded colour data is decoded here. */
render::TexturePtr toTexture(const Pixels& px, bool srgb)
{
    if (px.blocks)
    {
        // BC4 and BC5 hold data, never colour.
        px.blocks->srgb = srgb && px.blocks->format == render::TextureFormat::RGBA8;
        return px.blocks;
    }
    if (px.floats.rgba.empty())
        return px.rgba8.isNull() ? nullptr : toTexture(px.rgba8, srgb);
    std::vector<float> rgba = px.floats.rgba;
    if (srgb && !px.linear)
        for (size_t i = 0; i < rgba.size(); ++i)
            if (i % 4 != 3)
                rgba[i] = decodeSrgb(rgba[i]);
    auto t = std::make_shared<render::TextureImage>();
    t->width = px.floats.width;
    t->height = px.floats.height;
    t->format = render::TextureFormat::RGBA16F;
    t->srgb = false;
    render::encodeTexels(t->format, false, rgba, t->rgba);
    return t;
}

/**
 * A decoded image's mip chain on the CPU, handing the virtual texture system the pages it asks for. Edge texels wrap,
 * so the map repeats like a plain texture.
 */
class ImagePages final : public render::vt::Producer
{
public:
    ImagePages(int width, int height, render::TextureFormat format) : mFormat(format), mBytes(size_t(render::texelBytes(format)))
    {
        mLevels.push_back({width, height, {}});
    }

    /** Level 0 from 8-bit RGBA rows, then every coarser level down to one texel. */
    void setRgba8(const QImage& img)
    {
        Level& top = mLevels[0];
        top.texels.resize(size_t(top.width) * size_t(top.height) * 4);
        for (int y = 0; y < top.height; ++y)
            std::memcpy(&top.texels[size_t(y) * size_t(top.width) * 4], img.constScanLine(y), size_t(top.width) * 4);
        while (mLevels.back().width > 1 || mLevels.back().height > 1)
        {
            const Level& a = mLevels.back();
            Level b{std::max(a.width / 2, 1), std::max(a.height / 2, 1), {}};
            b.texels.resize(size_t(b.width) * size_t(b.height) * 4);
            for (int y = 0; y < b.height; ++y)
                for (int x = 0; x < b.width; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        int sum = 0;
                        for (int k = 0; k < 4; ++k)
                        {
                            const int sx = std::min(x * 2 + (k & 1), a.width - 1), sy = std::min(y * 2 + (k >> 1), a.height - 1);
                            sum += a.texels[(size_t(sy) * size_t(a.width) + size_t(sx)) * 4 + size_t(c)];
                        }
                        b.texels[(size_t(y) * size_t(b.width) + size_t(x)) * 4 + size_t(c)] = uint8_t((sum + 2) / 4);
                    }
            mLevels.push_back(std::move(b));
        }
    }

    /** Level 0 from linear RGBA floats (consumed), then every coarser level, each encoded in the producer's format. */
    void setFloats(image::Image img)
    {
        mLevels.clear();
        for (;;)
        {
            Level l{img.width, img.height, {}};
            render::encodeTexels(mFormat, false, img.rgba, l.texels);
            mLevels.push_back(std::move(l));
            if (img.width == 1 && img.height == 1)
                break;
            const int w = std::max(img.width / 2, 1), h = std::max(img.height / 2, 1);
            std::vector<float> out(size_t(w) * size_t(h) * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        float sum = 0.0f;
                        for (int k = 0; k < 4; ++k)
                        {
                            const int sx = std::min(x * 2 + (k & 1), img.width - 1), sy = std::min(y * 2 + (k >> 1), img.height - 1);
                            sum += img.rgba[(size_t(sy) * size_t(img.width) + size_t(sx)) * 4 + size_t(c)];
                        }
                        out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = sum * 0.25f;
                    }
            img = {w, h, std::move(out)};
        }
    }

    std::vector<uint8_t> record(render::VulkanContext& vk, VkCommandBuffer cmd, std::span<const render::vt::Job> jobs,
                                std::vector<render::compute::Buffer>& temps) override
    {
        const size_t slotBytes = mBytes * size_t(render::vt::kSlotSize) * size_t(render::vt::kSlotSize);
        std::vector<uint8_t> made(jobs.size(), 0);
        render::compute::Buffer host = render::compute::makeBuffer(vk, slotBytes * jobs.size(), 0, render::compute::Host::Write);
        temps.push_back(host);
        std::vector<VkBufferCopy> copies;
        VkBuffer dst = VK_NULL_HANDLE;
        for (size_t i = 0; i < jobs.size(); ++i)
        {
            const render::vt::Job& j = jobs[i];
            if (j.layers.empty() || size_t(j.page.mip) >= mLevels.size())
                continue;
            fillSlot(mLevels[size_t(j.page.mip)], j.x0, j.y0, static_cast<uint8_t*>(host.mapped) + i * slotBytes);
            copies.push_back({VkDeviceSize(i * slotBytes), VkDeviceSize(j.layers[0] - j.stagingAddress), VkDeviceSize(slotBytes)});
            dst = j.staging;
            made[i] = 1;
        }
        vmaFlushAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        if (!copies.empty())
            vkCmdCopyBuffer(cmd, host.buffer, dst, uint32_t(copies.size()), copies.data());
        return made;
    }

private:
    struct Level
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> texels;
    };

    /** The slot's texels, rows top first: level texels from (x0, y0), wrapping at the edges. */
    void fillSlot(const Level& l, int x0, int y0, uint8_t* out) const
    {
        const int n = render::vt::kSlotSize;
        for (int y = 0; y < n; ++y)
        {
            const int sy = ((y0 + y) % l.height + l.height) % l.height;
            const uint8_t* row = &l.texels[size_t(sy) * size_t(l.width) * mBytes];
            for (int x = 0; x < n; ++x)
            {
                const int sx = ((x0 + x) % l.width + l.width) % l.width;
                std::memcpy(out + (size_t(y) * size_t(n) + size_t(x)) * mBytes, row + size_t(sx) * mBytes, mBytes);
            }
        }
    }

    render::TextureFormat mFormat;
    size_t mBytes;
    std::vector<Level> mLevels;
};

/** An image past kMaxSize as a repeating virtual texture map, or null when the renderer has no virtual texturing. */
render::TexturePtr toVirtual(const std::string& path, Pixels& px, bool srgb)
{
    if (!render::vt::available())
        return nullptr;
    const bool floats = !px.floats.rgba.empty();
    const int w = floats ? px.floats.width : px.rgba8.width(), h = floats ? px.floats.height : px.rgba8.height();
    const render::TextureFormat format = floats ? render::TextureFormat::RGBA16F : render::TextureFormat::RGBA8;
    auto pages = std::make_shared<ImagePages>(w, h, format);
    if (floats)
    {
        if (srgb && !px.linear)
            for (size_t i = 0; i < px.floats.rgba.size(); ++i)
                if (i % 4 != 3)
                    px.floats.rgba[i] = decodeSrgb(px.floats.rgba[i]);
        pages->setFloats(std::move(px.floats));
    }
    else
        pages->setRgba8(px.rgba8.convertToFormat(QImage::Format_RGBA8888));
    auto vt = render::vt::Texture::create(path, {{format, srgb && !floats}}, {{1001, w, h}}, pages);
    vt->setRepeat(true);
    return vt->map(0);
}

/**
 * Colour data in a file profiled other than sRGB (Adobe RGB, Display P3, ...) converted to sRGB, the space the viewport
 * reads colour textures in. Linear float files and compressed blocks are left alone.
 */
void convertToSrgb(Pixels& px)
{
    if (!px.space.isValid() || px.space.colorModel() != QColorSpace::ColorModel::Rgb || px.space == QColorSpace(QColorSpace::SRgb) || px.blocks ||
        px.linear)
        return;
    if (px.floats.rgba.empty())
    {
        if (px.rgba8.isNull())
            return;
        QImage img = px.rgba8.convertToFormat(QImage::Format_RGBA8888);
        img.setColorSpace(px.space);
        img.convertToColorSpace(QColorSpace::SRgb);
        px.rgba8 = img.convertToFormat(QImage::Format_RGBA8888);
        return;
    }
    image::Image& f = px.floats;
    QImage img(f.width, f.height, QImage::Format_RGBA64);
    for (int y = 0; y < f.height; ++y)
    {
        auto* row = reinterpret_cast<quint16*>(img.scanLine(y));
        const float* src = &f.rgba[size_t(y) * size_t(f.width) * 4];
        for (int i = 0; i < f.width * 4; ++i)
            row[i] = quint16(std::lround(std::clamp(src[i], 0.0f, 1.0f) * 65535.0f));
    }
    img.setColorSpace(px.space);
    img.convertToColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < f.height; ++y)
    {
        const auto* row = reinterpret_cast<const quint16*>(img.constScanLine(y));
        float* dst = &f.rgba[size_t(y) * size_t(f.width) * 4];
        for (int i = 0; i < f.width * 4; ++i)
            dst[i] = float(row[i]) / 65535.0f;
    }
}

/** One file as a texture: past kMaxSize, a virtual texture at full size when the renderer has it, else shrunk to fit. */
Result loadSingle(const std::string& path, bool srgb)
{
    Result r;
    r.times = stamps({path});
    Pixels px = readPixels(path, r.error, true, true);
    if (px.empty())
        return r;
    if (srgb)
        convertToSrgb(px);
    const int w = px.blocks ? 0 : !px.floats.rgba.empty() ? px.floats.width : px.rgba8.width();
    const int h = px.blocks ? 0 : !px.floats.rgba.empty() ? px.floats.height : px.rgba8.height();
    if (w > kMaxSize || h > kMaxSize)
    {
        try
        {
            if (render::TexturePtr t = toVirtual(path, px, srgb))
            {
                r.image = std::move(t);
                return r;
            }
        }
        catch (const std::exception& e)
        {
            r.error = e.what();
            return r;
        }
        if (!px.floats.rgba.empty())
            shrinkToFit(px.floats);
        else
            px.rgba8 = px.rgba8.scaled(kMaxSize, kMaxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
    }
    r.image = toTexture(px, srgb);
    return r;
}

/** A UDIM set: every tile found on disk, each its own texture. */
Result loadTiles(const std::vector<std::pair<int, std::string>>& found, bool srgb)
{
    LILA_ZONE();

    Result r;
    for (const auto& [udim, file] : found)
        r.files.push_back(file);
    r.times = stamps(r.files);
    auto set = std::make_shared<render::TextureImage>();
    for (const auto& [udim, file] : found)
    {
        std::string err;
        Pixels px = readPixels(file, err, true);
        if (srgb)
            convertToSrgb(px);
        render::TexturePtr tile = px.empty() ? nullptr : toTexture(px, srgb);
        if (!tile)
        {
            r.error += (r.error.empty() ? "" : "; ") + file + ": " + err;
            continue;
        }
        if (set->tiles.empty())
        {
            set->width = tile->width;
            set->height = tile->height;
            set->format = tile->format;
            set->srgb = tile->srgb;
        }
        set->tiles.push_back({udim, std::move(tile)});
    }
    if (set->tiles.empty())
    {
        if (r.error.empty())
            r.error = "no UDIM tiles found";
        return r;
    }
    r.image = std::move(set);
    return r;
}

/** sRGB-encoded bytes to linear bytes. */
const std::array<uchar, 256>& srgbToLinear8()
{
    static const std::array<uchar, 256> lut = [] {
        std::array<uchar, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[size_t(i)] = uchar(std::lround(decodeSrgb(float(i) / 255.0f) * 255.0f));
        return t;
    }();
    return lut;
}

Result loadPacked(const std::vector<std::string>& files, std::array<int, 3> channels, uint32_t srgbMask)
{
    LILA_ZONE();

    const std::array<uchar, 256>& decode = srgbToLinear8();
    Result r;
    r.times = stamps(files);
    if (!files[0].empty())
    {
        // A compressed packed map in the usual order goes to the GPU as it is.
        if (channels == std::array<int, 3>{0, 1, 2} && !(srgbMask & 1u))
        {
            std::string err;
            if (Pixels px = readPixels(files[0], err, true); px.blocks)
            {
                r.image = toTexture(px, false);
                return r;
            }
        }
        QImage img = readImage(files[0], r.error);
        if (img.isNull())
            return r;
        if (channels != std::array<int, 3>{0, 1, 2} || (srgbMask & 1u))
            for (int y = 0; y < img.height(); ++y)
            {
                uchar* d = img.scanLine(y);
                for (int x = 0; x < img.width(); ++x)
                {
                    uchar* px = d + x * 4;
                    const uchar src[3] = {px[0], px[1], px[2]};
                    for (int i = 0; i < 3; ++i)
                    {
                        const uchar v = src[channels[size_t(i)]];
                        px[i] = (srgbMask & 1u) ? decode[v] : v;
                    }
                }
            }
        r.image = toTexture(img, false);
        return r;
    }
    QImage parts[3];
    int w = 1, h = 1;
    for (int i = 0; i < 3; ++i)
    {
        if (files[size_t(i) + 1].empty())
            continue;
        std::string err;
        parts[i] = readImage(files[size_t(i) + 1], err);
        if (parts[i].isNull())
            r.error += (r.error.empty() ? "" : "; ") + files[size_t(i) + 1] + ": " + err;
        w = std::max(w, parts[i].width());
        h = std::max(h, parts[i].height());
    }
    QImage out(w, h, QImage::Format_RGBA8888);
    out.fill(Qt::white);
    for (int i = 0; i < 3; ++i)
    {
        if (parts[i].isNull())
            continue;
        QImage src = parts[i].size() == out.size() ? parts[i] : parts[i].scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const bool encoded = srgbMask & (2u << i);
        for (int y = 0; y < h; ++y)
        {
            const uchar* s = src.constScanLine(y);
            uchar* d = out.scanLine(y);
            for (int x = 0; x < w; ++x)
                d[x * 4 + i] = encoded ? decode[s[x * 4]] : s[x * 4];
        }
    }
    r.image = toTexture(out, false);
    return r;
}

void finish(const std::string& key, Result r)
{
    auto& c = cache();
    auto it = c.entries.find(key);
    if (it == c.entries.end())
        return;
    Slot& s = it->second;
    s.loading = false;
    if (!r.files.empty())
        s.files = std::move(r.files);
    s.times = std::move(r.times);
    s.error = std::move(r.error);
    // A failed reload keeps the last good image on screen.
    if (r.image || s.error.empty())
        s.image = std::move(r.image);
    if (!r.thumb.isNull() || !s.error.empty())
        s.thumb = std::move(r.thumb);
    auto listeners = c.listeners;
    for (auto& [id, fn] : listeners)
        fn();
    viewportTools::requestRedraw();
}

void start(const std::string& key, Slot& s, std::function<Result()> work)
{
    s.loading = true;
    s.checked = Clock::now();
    s.work = work;
    QThreadPool::globalInstance()->start([key, work = std::move(work)] {
        Result r = work();
        mainq::post([key, r = std::move(r)]() mutable { finish(key, std::move(r)); });
    });
}

constexpr std::chrono::milliseconds kRecheck(1500);

/** Restarts a few slots whose files changed on disk, so an image reloads even while only cached materials draw it. */
void pollFiles()
{
    int budget = 16;
    for (auto& [key, s] : cache().entries)
    {
        if (s.loading || !s.work || Clock::now() - s.checked <= kRecheck)
            continue;
        s.checked = Clock::now();
        if (stamps(s.files) != s.times)
            start(key, s, s.work);
        if (--budget == 0)
            return;
    }
}

void ensurePolling()
{
    static QTimer* timer = nullptr;
    if (timer || !QCoreApplication::instance())
        return;
    timer = new QTimer(QCoreApplication::instance());
    timer->setInterval(250);
    QObject::connect(timer, &QTimer::timeout, [] { pollFiles(); });
    timer->start();
}

/** The slot for key, started on first use and restarted when one of its files changed on disk. */
Slot& entry(const std::string& key, std::vector<std::string> files, const std::function<Result()>& work)
{
    auto& c = cache();
    auto [it, added] = c.entries.try_emplace(key);
    Slot& s = it->second;
    if (added)
    {
        ensurePolling();
        s.files = std::move(files);
        start(key, s, work);
        return s;
    }
    if (!s.loading && Clock::now() - s.checked > kRecheck)
    {
        s.checked = Clock::now();
        if (stamps(s.files) != s.times)
            start(key, s, work);
    }
    return s;
}
} // namespace

render::TexturePtr get(const std::string& path, bool srgb, bool udimTiles)
{
    if (path.empty())
        return nullptr;
    std::string key = (srgb ? "c" : "d") + std::string(udimTiles ? "u|" : "|") + path;
    return entry(key, {path}, [path, srgb, udimTiles] {
               if (auto pattern = image::udimPattern(path, udimTiles))
               {
                   std::vector<std::pair<int, std::string>> found = image::findUdimTiles(*pattern);
                   // A lone name.1001.png stays a plain texture, so UVs outside 0..1 keep repeating it.
                   if (image::udimPattern(path, false) || found.size() > 1)
                       return loadTiles(found, srgb);
               }
               return loadSingle(path, srgb);
           }).image;
}

render::TexturePtr getOcio(const std::string& path, const std::string& config, const std::string& ocioSpace, bool srgb)
{
    if (path.empty())
        return nullptr;
    if (ocioSpace.empty())
        return get(path, srgb, true);
    const std::string key = "o" + config + "|" + ocioSpace + "|" + path;
    return entry(key, {path}, [path, config, ocioSpace, srgb] {
               Result r;
               r.times = stamps({path});
               Pixels px = readPixels(path, r.error, true, true);
               if (px.empty())
                   return r;
               std::vector<float> rgba;
               int w = 0, h = 0;
               if (!px.floats.rgba.empty())
               {
                   rgba = px.floats.rgba;
                   w = px.floats.width;
                   h = px.floats.height;
               }
               else if (!px.rgba8.isNull())
               {
                   const QImage img = px.rgba8.convertToFormat(QImage::Format_RGBA8888);
                   w = img.width();
                   h = img.height();
                   rgba.resize(size_t(w) * size_t(h) * 4);
                   for (int y = 0; y < h; ++y)
                   {
                       const uchar* s = img.constScanLine(y);
                       for (int x = 0; x < w * 4; ++x)
                           rgba[size_t(y) * size_t(w) * 4 + size_t(x)] = float(s[x]) / 255.0f;
                   }
               }
               else
               {
                   r.error = "OCIO colour spaces need a decoded image, not compressed blocks";
                   Result plain = loadSingle(path, srgb);
                   plain.error = r.error;
                   return plain;
               }
               std::string err;
               if (!render::ocio::toWorking(config, ocioSpace, rgba, &err))
               {
                   log::warn("textures: {} in OCIO space {}: {}; read as {}", path, ocioSpace, err, srgb ? "sRGB" : "linear");
                   Result plain = loadSingle(path, srgb);
                   plain.error = "OCIO " + ocioSpace + ": " + err;
                   return plain;
               }
               Pixels lin;
               lin.floats.width = w;
               lin.floats.height = h;
               lin.floats.rgba = std::move(rgba);
               lin.linear = true;
               if (w > kMaxSize || h > kMaxSize)
                   shrinkToFit(lin.floats);
               r.image = toTexture(lin, false);
               return r;
           }).image;
}

render::TexturePtr packed(const std::string& orm, const std::string& occlusion, const std::string& roughness, const std::string& metallic,
                          std::array<int, 3> channels, uint32_t srgbMask)
{
    if (orm.empty() && occlusion.empty() && roughness.empty() && metallic.empty())
        return nullptr;
    std::vector<std::string> files = {orm, orm.empty() ? occlusion : "", orm.empty() ? roughness : "", orm.empty() ? metallic : ""};
    if (orm.empty())
        channels = {0, 1, 2};
    std::string key = std::format("p{}{}{}{}|", channels[0], channels[1], channels[2], srgbMask) + files[0] + "|" + files[1] + "|" + files[2] + "|" +
                      files[3];
    return entry(key, files, [files, channels, srgbMask] {
               // A packed map can be a UDIM set too; separate maps cannot (their tiles would have to line up).
               if (auto pattern = image::udimPattern(files[0], true))
               {
                   std::vector<std::pair<int, std::string>> found = image::findUdimTiles(*pattern);
                   if (image::udimPattern(files[0], false) || found.size() > 1)
                   {
                       Result r;
                       auto set = std::make_shared<render::TextureImage>();
                       for (const auto& [udim, file] : found)
                       {
                           r.files.push_back(file);
                           Result tile = loadPacked({file, "", "", ""}, channels, srgbMask);
                           if (tile.image)
                               set->tiles.push_back({udim, tile.image});
                           else
                               r.error += (r.error.empty() ? "" : "; ") + file + ": " + tile.error;
                       }
                       r.times = stamps(r.files);
                       if (!set->tiles.empty())
                       {
                           set->width = set->tiles.front().image->width;
                           set->height = set->tiles.front().image->height;
                           set->srgb = false;
                           r.image = std::move(set);
                       }
                       else if (r.error.empty())
                           r.error = "no UDIM tiles found";
                       return r;
                   }
               }
               return loadPacked(files, channels, srgbMask);
           }).image;
}

render::TexturePtr heightNormal(const std::string& normal, const std::string& height, double slopeU, double slopeV, bool greenUp)
{
    if (height.empty())
        return nullptr;
    const std::string key = std::format("h{}|{}|{}|", slopeU, slopeV, greenUp ? 1 : 0) + normal + "|" + height;
    std::vector<std::string> files = {normal, height};
    return entry(key, files, [normal, height, slopeU, slopeV, greenUp] {
               LILA_ZONE_N("textures::heightNormal");
               Result r;
               r.times = stamps({normal, height});
               try
               {
                   const image::Image h = image::read(toPath(height));
                   image::Image detail = normal.empty() ? image::Image{} : image::read(toPath(normal));
                   if (detail.width <= 0)
                   {
                       if (!normal.empty())
                           r.error = normal + ": unreadable";
                       detail = {h.width, h.height, std::vector<float>(h.rgba.size())};
                       for (size_t i = 0; i < detail.rgba.size(); i += 4)
                           detail.rgba[i] = detail.rgba[i + 1] = 0.5f, detail.rgba[i + 2] = detail.rgba[i + 3] = 1.0f;
                   }
                   const image::Image n = bake::combineNormals(detail, h, {slopeU, slopeV}, greenUp);
                   QImage img(n.width, n.height, QImage::Format_RGBA8888);
                   for (int y = 0; y < n.height; ++y)
                   {
                       uchar* d = img.scanLine(y);
                       const float* src = &n.rgba[size_t(y) * size_t(n.width) * 4];
                       for (int x = 0; x < n.width * 4; ++x)
                           d[x] = uchar(std::clamp(std::lround(src[x] * 255.0f), 0L, 255L));
                   }
                   r.image = toTexture(img, false);
               }
               catch (const std::exception& e)
               {
                   r.error = height + ": " + e.what();
               }
               return r;
           }).image;
}

QImage thumbnail(const std::string& path, int size)
{
    if (path.empty())
        return {};
    std::string key = "t" + std::to_string(size) + "|" + path;
    return entry(key, {path}, [path, size] {
               Result r;
               r.times = stamps({path});
               std::string file = path;
               if (auto pattern = image::udimPattern(path, false))
               {
                   std::vector<std::pair<int, std::string>> found = image::findUdimTiles(*pattern);
                   if (!found.empty())
                       file = found.front().second;
               }
               QImage img = readImage(file, r.error, true);
               if (!img.isNull())
                   r.thumb = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
               return r;
           }).thumb;
}

std::string thumbnailError(const std::string& path, int size)
{
    auto& c = cache();
    auto it = c.entries.find("t" + std::to_string(size) + "|" + path);
    return it == c.entries.end() ? std::string() : it->second.error;
}

void release(const std::string& path, bool srgb)
{
    cache().entries.erase((srgb ? "c|" : "d|") + path);
}

std::string error(const std::string& path)
{
    auto& c = cache();
    for (const char* prefix : {"c|", "d|", "cu|", "du|"})
        if (auto it = c.entries.find(prefix + path); it != c.entries.end() && !it->second.error.empty())
            return it->second.error;
    return {};
}

int addListener(std::function<void()> fn)
{
    int id = cache().nextListener++;
    cache().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    cache().listeners.erase(id);
}

bool waitForLoads(double timeoutSeconds)
{
    LILA_ZONE();

    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeoutSeconds));
    auto loading = [] {
        for (const auto& [key, slot] : cache().entries)
            if (slot.loading)
                return true;
        return false;
    };
    while (true)
    {
        // Finished loads land through the main queue, so drain it ourselves: batch runs have no event loop to do it.
        mainq::drain();
        if (!loading())
            return true;
        if (Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace lila::ui::textures
