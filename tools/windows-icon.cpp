/* Writes the Windows application icon (Nacre.ico) from the shared shell mark.
 * Usage: nacre-icon <output.ico> [preview.png]
 * Each size is stored as a PNG frame, which Windows Vista and later accept. */

#include "../src/windows/branding.h"

#include <objidl.h>

#include <cstdio>
#include <vector>

#pragma pack(push, 1)
struct IconDir {
    WORD reserved, type, count;
};
struct IconEntry {
    BYTE width, height, colors, reserved;
    WORD planes, bits;
    DWORD bytes, offset;
};
#pragma pack(pop)

static bool png_encoder(CLSID *clsid) {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (!size) return false;
    std::vector<BYTE> buffer(size);
    Gdiplus::ImageCodecInfo *codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; i++) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

static bool render_png(int size, const CLSID &png, std::vector<BYTE> &out) {
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bitmap);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        nacre::draw_icon(g, (float)size);
    }
    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) return false;
    bool ok = bitmap.Save(stream, &png, NULL) == Gdiplus::Ok;
    if (ok) {
        HGLOBAL global = NULL;
        GetHGlobalFromStream(stream, &global);
        SIZE_T length = GlobalSize(global);
        const BYTE *data = static_cast<const BYTE *>(GlobalLock(global));
        out.assign(data, data + length);
        GlobalUnlock(global);
    }
    stream->Release();
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: nacre-icon <output.ico> [preview.png]\n");
        return 2;
    }
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, NULL) != Gdiplus::Ok) return 1;

    int status = 1;
    CLSID png;
    if (png_encoder(&png)) {
        static const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
        const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));
        std::vector<std::vector<BYTE>> frames(count);
        bool ok = true;
        for (int i = 0; i < count && ok; i++) ok = render_png(sizes[i], png, frames[i]);
        if (ok) {
            FILE *file = NULL;
            if (_wfopen_s(&file, argv[1], L"wb") == 0 && file) {
                IconDir dir = {0, 1, (WORD)count};
                fwrite(&dir, sizeof(dir), 1, file);
                DWORD offset = (DWORD)(sizeof(IconDir) + sizeof(IconEntry) * count);
                for (int i = 0; i < count; i++) {
                    IconEntry entry = {(BYTE)(sizes[i] >= 256 ? 0 : sizes[i]),
                                       (BYTE)(sizes[i] >= 256 ? 0 : sizes[i]),
                                       0, 0, 1, 32, (DWORD)frames[i].size(), offset};
                    fwrite(&entry, sizeof(entry), 1, file);
                    offset += (DWORD)frames[i].size();
                }
                for (int i = 0; i < count; i++)
                    fwrite(frames[i].data(), 1, frames[i].size(), file);
                status = fclose(file) == 0 ? 0 : 1;
            }
            if (status == 0 && argc > 2) {
                std::vector<BYTE> preview;
                if (render_png(512, png, preview)) {
                    FILE *out = NULL;
                    if (_wfopen_s(&out, argv[2], L"wb") == 0 && out) {
                        fwrite(preview.data(), 1, preview.size(), out);
                        fclose(out);
                    }
                }
            }
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return status;
}
