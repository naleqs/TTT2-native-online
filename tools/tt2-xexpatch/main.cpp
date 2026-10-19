// Applies a title update (Default.xexp, an XEX delta patch) to Default.xex, using XenonRecomp's
// XexPatcher (from Xenia's xex_module.cc).
//
//   tt2-xexpatch <base Default.xex> <Default.xexp> <output .xex>

#include <xex_patcher.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: %s <base.xex> <patch.xexp> <out.xex>\n", argv[0]);
        return 2;
    }
    const auto result = XexPatcher::apply(argv[1], argv[2], argv[3]);
    switch (result)
    {
    case XexPatcher::Result::Success:
        std::printf("patched: %s\n", argv[3]);
        return 0;
    case XexPatcher::Result::FileOpenFailed: std::fprintf(stderr, "cannot open the input files\n"); break;
    case XexPatcher::Result::FileWriteFailed: std::fprintf(stderr, "cannot write the output file\n"); break;
    case XexPatcher::Result::XexFileUnsupported: std::fprintf(stderr, "the base executable is not supported\n"); break;
    case XexPatcher::Result::XexFileInvalid: std::fprintf(stderr, "the base executable is invalid\n"); break;
    case XexPatcher::Result::PatchFileInvalid: std::fprintf(stderr, "the patch is invalid\n"); break;
    case XexPatcher::Result::PatchIncompatible: std::fprintf(stderr, "the patch is for a different executable\n"); break;
    case XexPatcher::Result::PatchFailed: std::fprintf(stderr, "applying the patch failed\n"); break;
    case XexPatcher::Result::PatchUnsupported: std::fprintf(stderr, "this kind of patch is not supported\n"); break;
    }
    return 1;
}
