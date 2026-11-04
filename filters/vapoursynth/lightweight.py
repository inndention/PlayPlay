"""Optional spatial filters. No temporal history, AI models or third-party VS plugins.
Import apply(clip, preset) from a .vpy script after opening a source with your source plugin.
"""
PRESETS = ("levels", "luma_lut", "chroma_smooth", "bilinear_720", "bicubic_720")

def apply(clip, preset):
    import vapoursynth as vs
    core = vs.core
    if clip.format is None or clip.format.id != vs.YUV420P8:
        raise ValueError("Use explicitly tagged 8-bit YUV420 input; set matrix and range during conversion")
    if preset == "levels":
        return core.std.Levels(clip, min_in=16, max_in=235, min_out=16, max_out=235, gamma=1.03, planes=[0])
    if preset == "luma_lut":
        lut = [max(16, min(235, round(16 + (value - 16) * 1.01))) for value in range(256)]
        return core.std.Lut(clip, planes=[0], lut=lut)
    if preset == "chroma_smooth":
        return core.std.BoxBlur(clip, planes=[1, 2], hradius=1, vradius=1, hpasses=1, vpasses=1)
    if preset == "bilinear_720":
        return core.resize.Bilinear(clip, width=1280, height=720)
    if preset == "bicubic_720":
        return core.resize.Bicubic(clip, width=1280, height=720)
    raise ValueError("Unknown lightweight filter: " + preset)
