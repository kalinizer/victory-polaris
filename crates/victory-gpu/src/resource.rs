//! Heaps, committed and placed resources, and resource sizes.
//!
//! Heaps and resources are host memory and a resource's GPU address is its
//! CPU address, as on the console; textures also get a host copy, and
//! buffers a host copy when a draw reads them (`shadow`).
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::alloc::{Layout, alloc_zeroed, dealloc};
use std::ffi::c_void;
use std::ptr::NonNull;

use victory_formats::texture;

use crate::object::{
    self, E_INVALIDARG, E_OUTOFMEMORY, HEAP, HRESULT, Iid, Object, RESOURCE, S_OK, Table, This,
    guid,
};

/// The interfaces every device child also answers for.
pub(crate) const PAGEABLE_IIDS: [Iid; 3] = [
    guid(
        0x63ee58fb,
        0x1268,
        0x4835,
        [0x86, 0xda, 0xf0, 0x08, 0xce, 0x62, 0xf0, 0xd6],
    ),
    guid(
        0x905db94b,
        0xa00c,
        0x4140,
        [0x9d, 0xf5, 0x2b, 0x64, 0xca, 0x9e, 0xa3, 0x57],
    ),
    guid(
        0xc4fec28f,
        0x7966,
        0x4e95,
        [0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8],
    ),
];
const HEAP_IIDS: [Iid; 4] = [
    guid(
        0x6b3b2502,
        0x6e51,
        0x45b3,
        [0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3],
    ),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
];
const RESOURCE_IIDS: [Iid; 4] = [
    guid(
        0x696442be,
        0xa72e,
        0x4059,
        [0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad],
    ),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
];

const KIB_64: u64 = 64 << 10;
const MIB_4: u64 = 4 << 20;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub(crate) struct HeapProperties {
    kind: u32,
    cpu_pages: u32,
    pool: u32,
    creation_nodes: u32,
    visible_nodes: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub(crate) struct HeapDesc {
    size: u64,
    properties: HeapProperties,
    alignment: u64,
    flags: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub(crate) struct ResourceDesc {
    dimension: u32,
    alignment: u64,
    width: u64,
    height: u32,
    depth_or_array_size: u16,
    mip_levels: u16,
    format: u32,
    sample_count: u32,
    sample_quality: u32,
    layout: u32,
    flags: u32,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct AllocationInfo {
    size: u64,
    alignment: u64,
}

const _: () = assert!(size_of::<HeapProperties>() == 20);
const _: () = assert!(size_of::<HeapDesc>() == 48);
const _: () = assert!(size_of::<ResourceDesc>() == 56);

const BUFFER: u32 = 1;
const TEXTURE_2D: u32 = 3;
const TEXTURE_3D: u32 = 4;

/// Bytes per element and the element's width in texels (4 for block
/// compression), by DXGI format number. `None` for 1-bit and video
/// formats. The console adds 116/117 (10-bit float color), 118-120 (16-bit
/// depth with 8-bit stencil, two planes: 3 bytes per texel), 189 (signed
/// 10-bit color), 190 (two 4-bit channels) and 191 (24/8 color).
pub(crate) fn element(format: u32) -> Option<(u64, u64)> {
    Some(match format {
        1..=4 => (16, 1),
        5..=8 => (12, 1),
        9..=22 => (8, 1),
        23..=47 | 67 | 87..=93 | 116 | 117 | 189 | 191 => (4, 1),
        118..=120 => (3, 1),
        48..=59 | 68 | 69 | 85 | 86 | 115 => (2, 1),
        60..=65 | 190 => (1, 1),
        70..=72 | 79..=81 => (8, 4),
        73..=78 | 82..=84 | 94..=99 => (16, 4),
        _ => return None,
    })
}

/// A texture's layout in console memory, as the console's layout library
/// gives it (`victory-hle`'s `xg` serves the same): the description's
/// layout field is `0x100 | swizzle mode`, 1 linear, 0 the library's pick.
pub(crate) fn texture_layout(desc: &ResourceDesc) -> Option<texture::Texture> {
    use texture::{Dimension, Swizzle, TextureDesc};
    const TEXTURE_1D: u32 = 2;
    const RENDER_TARGET: u32 = 1;
    const DEPTH: u32 = 2;
    let dimension = match desc.dimension {
        TEXTURE_1D => Dimension::Tex1d,
        TEXTURE_2D => Dimension::Tex2d,
        TEXTURE_3D => Dimension::Tex3d,
        _ => return None,
    };
    let swizzle = match desc.layout {
        0 if dimension == Dimension::Tex1d => Swizzle::LINEAR,
        0 if desc.flags & DEPTH != 0 => Swizzle::SW_64KB_Z_X,
        0 if desc.flags & RENDER_TARGET != 0 => Swizzle::SW_64KB_R_X,
        0 => Swizzle::SW_4KB_S,
        1 => Swizzle::LINEAR,
        l if l & !0xff == 0x100 => Swizzle(l & 0xff),
        _ => return None,
    };
    texture::Texture::new(&TextureDesc {
        dimension,
        width: u32::try_from(desc.width).ok()?,
        height: desc.height,
        depth_or_array: u32::from(desc.depth_or_array_size),
        mips: u32::from(desc.mip_levels),
        format: desc.format,
        samples: desc.sample_count.max(1),
        swizzle,
    })
    .ok()
}

/// Bytes and alignment a resource takes: buffers their width (64 KiB
/// aligned), textures their console layout (formats it does not know:
/// every mip of every array slice and sample, packed, 64 KiB aligned),
/// rounded up to the alignment.
fn footprint(desc: &ResourceDesc) -> Option<AllocationInfo> {
    if desc.dimension != BUFFER
        && let Some(t) = texture_layout(desc)
    {
        // As the console: the layout's own alignment unless more is asked
        // (4 KiB for 4 KiB modes, where PC says 64 KiB). Grand Theft Auto
        // VI keeps these sizes in its texture tables, and its streaming
        // asserts when they disagree with the streamed layout.
        let alignment = desc.alignment.max(t.alignment);
        if !alignment.is_power_of_two() {
            return None;
        }
        return Some(AllocationInfo {
            size: t.size.checked_next_multiple_of(alignment)?,
            alignment,
        });
    }
    let bytes = if desc.dimension == BUFFER {
        desc.width
    } else {
        let (element, block) = element(desc.format)?;
        let (depth, slices) = if desc.dimension == TEXTURE_3D {
            (u64::from(desc.depth_or_array_size), 1)
        } else {
            (1, u64::from(desc.depth_or_array_size))
        };
        let height = u64::from(desc.height);
        let mips = mip_count(desc);
        let mut chain = 0u64;
        for mip in 0..mips.min(64) {
            let w = (desc.width >> mip).max(1).div_ceil(block);
            let h = (height >> mip).max(1).div_ceil(block);
            let d = (depth >> mip).max(1);
            chain = chain.checked_add(w.checked_mul(h)?.checked_mul(d)?.checked_mul(element)?)?;
        }
        chain
            .checked_mul(slices)?
            .checked_mul(u64::from(desc.sample_count.max(1)))?
    };
    if bytes == 0 {
        return None;
    }
    let minimum = if desc.sample_count > 1 { MIB_4 } else { KIB_64 };
    let alignment = desc.alignment.max(minimum);
    if !alignment.is_power_of_two() {
        return None;
    }
    Some(AllocationInfo {
        size: bytes.checked_next_multiple_of(KIB_64)?,
        alignment,
    })
}

/// A format as the host has it. The console's 16-bit depth with stencil
/// (and its typeless views) has no PC format: the 24-bit ones stand in.
pub(crate) fn pc_format(format: u32) -> u32 {
    match format {
        118 => 45,
        119 => 46,
        120 => 47,
        other => other,
    }
}

/// The opt-in packed-light format substitution.
///
/// `VICTORY_LIGHT_FORMAT_FLOAT=1` hosts the light image as
/// R16G16B16A16_FLOAT (10) instead of the title's packed R9G9B9E5 in
/// R32_UINT (42). The title packs bounced light and sky probes as
/// R9G9B9E5 in R32_UINT textures because compute shaders cannot write the
/// shared-exponent format; on some parts that format path reads back black
/// or corrupt. Keeping the host resource, its views, clears, copies and the
/// stand-in's `raise` program in one format avoids the broken conversion.
/// Unset, `0` or `off` keeps the title's formats byte for byte.
///
/// [`light_effective_format`] is the single cached decision every format
/// touchpoint reads, so the resource, its views and the ambient program can
/// no longer disagree about what the light image contains.
const LIGHT_NATIVE: u32 = 42;
const LIGHT_FLOAT: u32 = 10;
/// Title formats that hold packed light: the R9G9B9E5 description (67) and
/// the R32_UINT (42) the compute programs actually write.
const LIGHT_TARGETS: [u32; 2] = [67, 42];

fn light_format_for(float: bool) -> u32 {
    if float { LIGHT_FLOAT } else { LIGHT_NATIVE }
}

/// The one cached decision for the packed-light host format. `42` is the
/// title's native R32_UINT image holding packed R9G9B9E5; `10` is the opt-in
/// RGBA16F stand-in. Every format touchpoint reads this instead of the
/// environment, so creation, views, clears, copies and the `raise` program
/// cannot pick different formats.
pub(crate) fn light_effective_format() -> u32 {
    static EFFECTIVE: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    let effective = *EFFECTIVE.get_or_init(|| {
        let float = std::env::var("VICTORY_LIGHT_FORMAT_FLOAT")
            .is_ok_and(|v| v.trim() == "1" || v.trim().eq_ignore_ascii_case("on"));
        light_format_for(float)
    });
    static REPORTED: std::sync::OnceLock<()> = std::sync::OnceLock::new();
    REPORTED.get_or_init(|| {
        let mode = if effective == LIGHT_FLOAT { "float" } else { "native" };
        // The gate compares the creation table for the written light image
        // with the format the stand-in writes; both read this one decision.
        let gate = if target_format_for(42, effective) == effective { "ok" } else { "mismatch" };
        tracing::info!(
            mode = mode,
            effective = effective,
            gate = gate,
            "[lighting] format mode={mode} effective={effective} gate={gate}"
        );
    });
    effective
}

/// Whether the opt-in float stand-in is active. A view of
/// [`light_effective_format`], not a second read of the switch.
pub(crate) fn light_float_format() -> bool {
    light_effective_format() == LIGHT_FLOAT
}

/// Pure creation-format table: native is the identity; the float stand-in
/// maps only the packed-light targets to RGBA16F.
fn target_format_for(format: u32, effective: u32) -> u32 {
    if effective == LIGHT_FLOAT && LIGHT_TARGETS.contains(&format) {
        LIGHT_FLOAT
    } else {
        format
    }
}

/// Whether the host renders to and stores through typed UAVs of `format`.
/// Diagnostic only now: [`light_effective_format`] is the decision, so a
/// host that accepts the packed format still honours an explicit opt-in.
#[cfg(windows)]
fn packed_target_supported(format: u32) -> bool {
    use windows::Win32::Graphics::Direct3D12::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;
    static SUPPORTED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *SUPPORTED.get_or_init(|| {
        let Some(host) = crate::host::host() else {
            return false;
        };
        let mut support = D3D12_FEATURE_DATA_FORMAT_SUPPORT {
            Format: DXGI_FORMAT(format as i32),
            ..Default::default()
        };
        // SAFETY: a plain query into our own structure.
        let asked = unsafe {
            host.device.CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT,
                (&raw mut support).cast(),
                size_of_val(&support) as u32,
            )
        };
        let need1 = D3D12_FORMAT_SUPPORT1_RENDER_TARGET.0
            | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW.0;
        let need2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD.0
            | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE.0;
        let ok = asked.is_ok()
            && support.Support1.0 & need1 == need1
            && support.Support2.0 & need2 == need2;
        tracing::info!(format, supported = ok, "packed-light format support probed");
        ok
    })
}

/// The format of a texture the host renders to or writes as unordered
/// access, for the title's `format`. [`light_effective_format`] is the only
/// decision: both the R9G9B9E5 description (67) and the R32_UINT (42) the
/// compute programs write become RGBA16F under the opt-in. The capability
/// probe below is a one-time diagnostic, never a gate, so creation cannot
/// disagree with the views or the ambient program.
pub(crate) fn target_format(format: u32) -> u32 {
    let effective = light_effective_format();
    let mapped = target_format_for(format, effective);
    #[cfg(windows)]
    {
        if mapped != format {
            // The opt-in decides; the probe only reports what the host can do.
            let _ = packed_target_supported(format);
        }
    }
    mapped
}

/// Whether `this`'s host copy is in the float stand-in: its texels are not
/// laid out as the title's, so its views must name the stand-in format too.
#[cfg(windows)]
fn stands_in(this: This) -> bool {
    if this.is_null() {
        return false;
    }
    let r = resource(this);
    let own = pc_format(r.desc.format);
    let stand = target_format(own);
    stand != own
        && host_resource(this)
            .is_some_and(|host| unsafe { host.GetDesc() }.Format.0 as u32 == stand)
}

/// A view format the host must name over a texture in a float stand-in.
/// The title describes light as R9G9B9E5 (67), writes it through R32_UINT
/// (42) and reads typeless 9 views; all three name RGBA16F (10) once the
/// host texture is the stand-in, so the pipeline and the views agree.
pub(crate) fn stand_in_view_format(host: u32, requested: u32) -> u32 {
    stand_in_view_format_for(light_effective_format(), host, requested)
}

/// Pure view-format table for a host format and a requested format. Native
/// mode is the identity, byte for byte as before.
fn stand_in_view_format_for(effective: u32, host: u32, requested: u32) -> u32 {
    if effective == LIGHT_NATIVE {
        return requested;
    }
    if host == LIGHT_FLOAT && (requested == 9 || LIGHT_TARGETS.contains(&requested)) {
        // A stand-in target's light views: packed SRV, R32_UINT UAV, and
        // typeless 16-bit views all name the float format.
        LIGHT_FLOAT
    } else if host == 9 && requested == 9 {
        // A typeless 16-bit-per-channel host texture needs a typed view.
        LIGHT_FLOAT
    } else {
        requested
    }
}

/// The typed format for a view without a description of a typeless texture.
/// With the float substitution a typeless 16-bit-per-channel resource is
/// sampled as RGBA16F; otherwise the title's own format stands (the host
/// refuses typeless views). `None` leaves the view alone.
pub(crate) fn default_shader_format(format: u32) -> Option<u32> {
    default_shader_format_for(light_effective_format(), format)
}

/// Pure default-view format table, kept beside the other format tables.
fn default_shader_format_for(effective: u32, format: u32) -> Option<u32> {
    if effective == LIGHT_FLOAT && pc_format(format) == 9 {
        Some(LIGHT_FLOAT)
    } else {
        None
    }
}

/// The typeless format of a format's family (itself if it has none).
pub(crate) fn family(format: u32) -> u32 {
    const FAMILIES: [(u32, u32); 22] = [
        (1, 4),
        (5, 8),
        (9, 14),
        (15, 18),
        (19, 22),
        (23, 25),
        (27, 32),
        (33, 38),
        (39, 43),
        (44, 47),
        (48, 52),
        (53, 59),
        (60, 64),
        (70, 72),
        (73, 75),
        (76, 78),
        (79, 81),
        (82, 84),
        (90, 91),
        (92, 93),
        (94, 96),
        (97, 99),
    ];
    match format {
        87 => 90,
        88 => 92,
        f => FAMILIES
            .iter()
            .find(|&&(first, last)| (first..=last).contains(&f))
            .map_or(f, |&(first, _)| first),
    }
}

/// sRGB formats and their linear twins.
pub(crate) const SRGB: [(u32, u32); 3] = [(29, 28), (91, 87), (93, 88)];

/// The format a view of a title texture must name when its host copy is
/// typeless (see [`DEPTH_FORMATS`], [`SRGB`]): the title's own; `None` if
/// the host copy is typed.
pub(crate) fn view_format(this: This) -> Option<u32> {
    let own = pc_format(resource(this).desc.format);
    // A stand-in target is typed: views name the host's float format, not
    // the title's packed one (the host refuses a view of another family).
    if stands_in(this) {
        return None;
    }
    let host = host_resource(this)?;
    // SAFETY: a live resource.
    let format = unsafe { host.GetDesc() }.Format.0 as u32;
    (format != own).then_some(own)
}

/// Depth formats with their typeless family: depth textures that shaders
/// read are made typeless on the host, which only allows views of
/// typeless depth resources.
pub(crate) const DEPTH_FORMATS: [(u32, u32); 4] = [(20, 19), (40, 39), (45, 44), (55, 53)];

/// Mip levels, the full chain for 0.
fn mip_count(desc: &ResourceDesc) -> u32 {
    let depth = if desc.dimension == TEXTURE_3D {
        u64::from(desc.depth_or_array_size)
    } else {
        1
    };
    match desc.mip_levels {
        0 => {
            64 - desc
                .width
                .max(u64::from(desc.height))
                .max(depth)
                .max(1)
                .leading_zeros()
        }
        n => u32::from(n),
    }
}

/// How `VICTORY_TEXTURE_MAX` was configured.
#[derive(Clone, Copy)]
enum TextureCapMode {
    Fixed(u32),
    Auto,
}

/// `VICTORY_TEXTURE_MAX=<pixels>` caps a sampled host texture's largest
/// dimension by leaving out the top mips. `auto` starts at 1024 pixels and
/// drops to 512 / 256 as the card fills (92% / 97% of its DXGI budget), the
/// tiers used by the small-VRAM forks. Unset, `0` or an invalid value means
/// no cap: the title's own dimensions are kept byte for byte.
pub(crate) fn texture_cap() -> Option<u32> {
    static MODE: std::sync::OnceLock<Option<TextureCapMode>> = std::sync::OnceLock::new();
    let mode = (*MODE.get_or_init(|| {
        let Ok(raw) = std::env::var("VICTORY_TEXTURE_MAX") else {
            return None;
        };
        let raw = raw.trim();
        if raw.is_empty() || raw == "0" {
            return None;
        }
        if raw.eq_ignore_ascii_case("auto") {
            return Some(TextureCapMode::Auto);
        }
        raw.parse::<u32>()
            .ok()
            .filter(|&n| n >= 64)
            .map(TextureCapMode::Fixed)
    }))?;
    Some(match mode {
        TextureCapMode::Fixed(n) => n,
        TextureCapMode::Auto => {
            let [budget, used, ..] = crate::host::host()
                .and_then(|h| h.video_memory())
                .unwrap_or([0; 4]);
            let share = if budget > 0 {
                used as f64 / budget as f64
            } else {
                0.0
            };
            if share >= 0.97 {
                256
            } else if share >= 0.92 {
                512
            } else {
                1024
            }
        }
    })
}

/// `VICTORY_TEXTURE_MB=<n>` limits one sampled texture's host allocation to
/// `n` MiB, clamped to a quarter of the card's DXGI budget (the small-VRAM
/// presets' rule). Unset: no byte budget. This is a second, independent
/// reason to leave out top mips, alongside `VICTORY_TEXTURE_MAX`.
fn texture_budget_bytes() -> Option<u64> {
    static BUDGET: std::sync::OnceLock<Option<u64>> = std::sync::OnceLock::new();
    let mb = (*BUDGET.get_or_init(|| {
        std::env::var("VICTORY_TEXTURE_MB")
            .ok()?
            .trim()
            .parse::<u64>()
            .ok()
            .filter(|&n| n > 0)
    }))?;
    let budget = mb.saturating_mul(1 << 20);
    let quarter = crate::host::host()
        .and_then(|h| h.video_memory())
        .map_or(u64::MAX, |[local_budget, ..]| local_budget / 4);
    Some(budget.min(quarter).max(1 << 20))
}

/// The number of top mip levels a capped host texture leaves out.
///
/// Only power-of-two 2D textures the title merely samples qualify. Render
/// targets, depth and unordered-access textures are drawn at the title's own
/// sizes, and a non-power-of-two texture (a 1920x1080 UI plane) must keep its
/// exact dimensions. Never drops below a 256-pixel side.
/// Cap/placeholder counts for one diagnostics window: [clamped, placeholder].
pub(crate) static TEXCAP: [std::sync::atomic::AtomicU64; 2] =
    [const { std::sync::atomic::AtomicU64::new(0) }; 2];

/// Take and clear the texture-cap counts for this diagnostics window.
pub(crate) fn take_texcap() -> (u64, u64) {
    use std::sync::atomic::Ordering::Relaxed;
    (TEXCAP[0].swap(0, Relaxed), TEXCAP[1].swap(0, Relaxed))
}

pub(crate) fn texture_mip_drop(desc: &ResourceDesc) -> u32 {
    let cap = texture_cap();
    let budget = texture_budget_bytes();
    if cap.is_none() && budget.is_none() {
        return 0;
    }
    if desc.dimension != TEXTURE_2D || desc.flags & USES != 0 || desc.sample_count > 1 {
        return 0;
    }
    let (w, h) = (desc.width, u64::from(desc.height));
    if h == 0 || !w.is_power_of_two() || !h.is_power_of_two() {
        return 0;
    }
    // Console layout size is a close enough estimate of the host allocation
    // for a budget decision; every mip dropped quarters the bytes.
    let bytes = texture_layout(desc).map_or(0, |t| t.size);
    let drop = mip_drop_for(w, h, mip_count(desc), bytes, cap, budget);
    if drop > 0 {
        TEXCAP[0].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
    drop
}

/// Pure form of the cap decision, so it can be tested without a host or env.
fn mip_drop_for(
    w: u64,
    h: u64,
    mips: u32,
    bytes: u64,
    cap: Option<u32>,
    budget: Option<u64>,
) -> u32 {
    const FLOOR: u64 = 256;
    let mut drop = 0;
    loop {
        if drop + 1 >= mips {
            break;
        }
        if (w >> drop).max(h >> drop) <= cap.unwrap_or(u32::MAX) as u64
            && budget.is_none_or(|b| (bytes >> (2 * drop)) <= b)
        {
            break;
        }
        if (w >> (drop + 1)) < FLOOR || (h >> (drop + 1)) < FLOOR {
            break;
        }
        drop += 1;
    }
    drop
}

/// The host subresource for a title subresource of a mip-capped texture.
/// `None` means the title names a level the host left out, so the copy or
/// resolve must be dropped rather than redirected to the wrong mip.
pub(crate) fn host_subresource(drop: u32, sub: u32) -> Option<u32> {
    let host = sub.checked_sub(drop);
    if drop > 0 && host.is_none() {
        TEXCAP[1].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    }
    host
}

/// Shift an SRV's mip window from the title's chain onto a host texture with
/// `drop` top levels left out. Mip-capped textures are sampled-only, so
/// buffer views (and all target/UAV views) never need this.
#[cfg(windows)]
pub(crate) fn rebase_srv_mips(
    desc: &mut windows::Win32::Graphics::Direct3D12::D3D12_SHADER_RESOURCE_VIEW_DESC,
    drop: u32,
    host_mips: u32,
) {
    use windows::Win32::Graphics::Direct3D12::{
        D3D12_SRV_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE1DARRAY,
        D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
        D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE,
        D3D12_SRV_DIMENSION_TEXTURECUBEARRAY,
    };
    if drop == 0 {
        return;
    }
    let last = host_mips.saturating_sub(1);
    let rebase = |most: &mut u32, levels: &mut u32, min_lod: &mut f32| {
        let end = if *levels == u32::MAX {
            u32::MAX
        } else {
            most.saturating_add(*levels)
        };
        *most = most.saturating_sub(drop).min(last);
        *levels = if end == u32::MAX {
            u32::MAX
        } else {
            end.saturating_sub(drop).saturating_sub(*most).max(1)
        };
        *min_lod = (*min_lod - drop as f32).max(0.0);
    };
    // SAFETY: the union holds the struct named by `ViewDimension`.
    unsafe {
        match desc.ViewDimension {
            D3D12_SRV_DIMENSION_TEXTURE1D => {
                let v = &mut desc.Anonymous.Texture1D;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURE1DARRAY => {
                let v = &mut desc.Anonymous.Texture1DArray;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURE2D => {
                let v = &mut desc.Anonymous.Texture2D;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURE2DARRAY => {
                let v = &mut desc.Anonymous.Texture2DArray;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURE3D => {
                let v = &mut desc.Anonymous.Texture3D;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURECUBE => {
                let v = &mut desc.Anonymous.TextureCube;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            D3D12_SRV_DIMENSION_TEXTURECUBEARRAY => {
                let v = &mut desc.Anonymous.TextureCubeArray;
                rebase(&mut v.MostDetailedMip, &mut v.MipLevels, &mut v.ResourceMinLODClamp);
            }
            _ => {}
        }
    }
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct PlacedFootprint {
    pub(crate) offset: u64,
    format: u32,
    width: u32,
    height: u32,
    depth: u32,
    row_pitch: u32,
}

impl PlacedFootprint {
    /// The bytes it spans in a buffer: every row pitched but the last.
    pub(crate) fn bytes(&self) -> Option<u64> {
        let (element, block) = element(self.format)?;
        let lines = u64::from(self.height).div_ceil(block) * u64::from(self.depth);
        let last = u64::from(self.width).div_ceil(block) * element;
        Some(u64::from(self.row_pitch) * lines.saturating_sub(1) + last)
    }
}
const _: () = assert!(size_of::<PlacedFootprint>() == 32);

const NO_FOOTPRINT: PlacedFootprint = PlacedFootprint {
    offset: u64::MAX,
    format: 0,
    width: 0,
    height: 0,
    depth: 0,
    row_pitch: 0,
};

/// One subresource's upload footprint (offset 0), row count and unpadded row size.
fn subresource(desc: &ResourceDesc, index: u32) -> Option<(PlacedFootprint, u32, u64)> {
    const PITCH: u64 = 256;
    if desc.dimension == BUFFER {
        let footprint = PlacedFootprint {
            offset: 0,
            format: 0,
            width: u32::try_from(desc.width).ok()?,
            height: 1,
            depth: 1,
            row_pitch: u32::try_from(desc.width.next_multiple_of(PITCH)).ok()?,
        };
        return (index == 0).then_some((footprint, 1, desc.width));
    }
    let mips = mip_count(desc);
    let slices = if desc.dimension == TEXTURE_3D {
        1
    } else {
        u32::from(desc.depth_or_array_size).max(1)
    };
    let mip = index % mips;
    let plane = index / mips / slices;
    let depth_stencil = matches!(desc.format, 19..=22 | 44..=47);
    // Each plane contains all the mip levels and array slices. Depth and
    // stencil copies use separate formats and 512-byte row alignment.
    let (format, pitch_alignment) = match (depth_stencil, plane) {
        (true, 0) => (39, 512), // DXGI_FORMAT_R32_TYPELESS
        (true, 1) => (60, 512), // DXGI_FORMAT_R8_TYPELESS
        (false, 0) => (desc.format, PITCH),
        _ => return None,
    };
    let (element, block) = element(format)?;
    let width = (desc.width >> mip).max(1).next_multiple_of(block);
    let height = (u64::from(desc.height) >> mip)
        .max(1)
        .next_multiple_of(block);
    let depth = if desc.dimension == TEXTURE_3D {
        (u32::from(desc.depth_or_array_size) >> mip).max(1)
    } else {
        1
    };
    let row_size = width / block * element;
    let footprint = PlacedFootprint {
        offset: 0,
        format,
        width: u32::try_from(width).ok()?,
        height: u32::try_from(height).ok()?,
        depth,
        row_pitch: u32::try_from(row_size.next_multiple_of(pitch_alignment)).ok()?,
    };
    Some((footprint, u32::try_from(height / block).ok()?, row_size))
}

/// Places subresources at 512-byte boundaries in an upload buffer.
/// Row pitch is aligned to 512 bytes for depth/stencil planes and 256 bytes
/// otherwise. Unknown sizes give the documented all-ones values.
pub(crate) extern "system" fn copyable_footprints(
    _: This,
    desc: *const ResourceDesc,
    first: u32,
    count: u32,
    base: u64,
    layouts: *mut PlacedFootprint,
    rows: *mut u32,
    row_sizes: *mut u64,
    total: *mut u64,
) {
    const PLACEMENT: u64 = 512;
    // SAFETY: the title passes a description (null is tolerated).
    let desc = unsafe { desc.as_ref() };
    let mut end = base;
    let mut known = desc.is_some();
    for i in 0..count {
        let one = desc.and_then(|d| subresource(d, first + i));
        let (layout, row_count, row_size) = match one {
            Some((mut footprint, row_count, row_size)) => {
                footprint.offset = end.next_multiple_of(PLACEMENT);
                let pitch = u64::from(footprint.row_pitch);
                let lines = u64::from(row_count) * u64::from(footprint.depth);
                end = footprint.offset + pitch * lines.saturating_sub(1) + row_size;
                (footprint, row_count, row_size)
            }
            None => {
                known = false;
                (NO_FOOTPRINT, u32::MAX, u64::MAX)
            }
        };
        let i = i as usize;
        // SAFETY: each output is null or holds `count` entries.
        unsafe {
            if !layouts.is_null() {
                layouts.add(i).write(layout);
            }
            if !rows.is_null() {
                rows.add(i).write(row_count);
            }
            if !row_sizes.is_null() {
                row_sizes.add(i).write(row_size);
            }
        }
    }
    if !known {
        tracing::warn!(?desc, "no footprint for this resource");
    }
    if !total.is_null() {
        // SAFETY: checked non-null.
        unsafe { total.write(if known { end - base } else { u64::MAX }) };
    }
}

/// Device method: the footprint of resources placed one after another.
/// Returns a 16-byte value, through a hidden first argument. Invalid input
/// gives the documented size `u64::MAX`.
pub(crate) extern "system" fn allocation_info(
    out: *mut AllocationInfo,
    _: This,
    _visible: u32,
    count: u32,
    descs: *const ResourceDesc,
) -> *mut AllocationInfo {
    let descs = if descs.is_null() {
        &[][..]
    } else {
        // SAFETY: the title passes `count` descriptions.
        unsafe { std::slice::from_raw_parts(descs, count as usize) }
    };
    // SAFETY: the caller passes room for the value.
    unsafe { out.write(place(descs.iter(), |_, _| ())) };
    out
}

/// The newer description: the plain one, then a sampler feedback mip
/// region (width, height, depth).
#[repr(C)]
pub(crate) struct ResourceDesc1 {
    desc: ResourceDesc,
    mip_region: [u32; 3],
}
const _: () = assert!(size_of::<ResourceDesc1>() == 72);

/// Where one resource goes in [`allocation_info2`]'s placement.
#[repr(C)]
pub(crate) struct AllocationInfo1 {
    offset: u64,
    alignment: u64,
    size: u64,
}

/// Device method 0x360 (`ID3D12Device8`): [`allocation_info`] for newer
/// descriptions, also telling where each resource goes.
pub(crate) extern "system" fn allocation_info2(
    out: *mut AllocationInfo,
    _: This,
    _visible: u32,
    count: u32,
    descs: *const ResourceDesc1,
    each: *mut AllocationInfo1,
) -> *mut AllocationInfo {
    let descs = if descs.is_null() {
        &[][..]
    } else {
        // SAFETY: the title passes `count` descriptions.
        unsafe { std::slice::from_raw_parts(descs, count as usize) }
    };
    let total = place(descs.iter().map(|d| &d.desc), |i, info| {
        if !each.is_null() {
            // SAFETY: the title passes room for `count` entries.
            unsafe { each.add(i).write(info) };
        }
    });
    // SAFETY: the caller passes room for the value.
    unsafe { out.write(total) };
    out
}

/// Resources placed one after another: the whole footprint, telling `each`
/// where every resource goes. Unknown sizes give the documented `u64::MAX`.
fn place<'a>(
    descs: impl Iterator<Item = &'a ResourceDesc>,
    mut each: impl FnMut(usize, AllocationInfo1),
) -> AllocationInfo {
    let mut total = AllocationInfo {
        size: 0,
        alignment: KIB_64,
    };
    for (i, desc) in descs.enumerate() {
        let Some(one) = footprint(desc) else {
            tracing::warn!(?desc, "no size for this resource");
            each(
                i,
                AllocationInfo1 {
                    offset: u64::MAX,
                    alignment: 0,
                    size: u64::MAX,
                },
            );
            return AllocationInfo {
                size: u64::MAX,
                alignment: 0,
            };
        };
        tracing::debug!(?desc, ?one, "allocation info");
        let offset = total.size.next_multiple_of(one.alignment);
        each(
            i,
            AllocationInfo1 {
                offset,
                alignment: one.alignment,
                size: one.size,
            },
        );
        total.size = offset + one.size;
        total.alignment = total.alignment.max(one.alignment);
    }
    total
}

/// Zeroed host memory standing in for GPU memory.
pub(crate) struct Memory {
    ptr: NonNull<u8>,
    layout: Layout,
    /// Set when the memory came from the title's allocator, with these
    /// allocation attributes (it goes back there).
    attributes: Option<u64>,
}

pub type AllocRoutine = extern "system" fn(usize, u64) -> *mut c_void;
pub type FreeRoutine = extern "system" fn(*mut c_void, u64);
static ALLOCATOR: std::sync::OnceLock<(AllocRoutine, FreeRoutine)> = std::sync::OnceLock::new();

/// Committed resources get their memory from the title's general
/// allocator, as on the console, where titles hook it to place graphics
/// memory themselves (Grand Theft Auto VI places its back buffers in a
/// block of its own and checks they fill it). The runtime names it before
/// the device exists.
pub fn use_allocator(alloc: AllocRoutine, free: FreeRoutine) {
    let _ = ALLOCATOR.set((alloc, free));
}

/// Maps the physical pages behind one range of title memory at another,
/// `(to, from, len)` (`from` 0 unmaps); false on failure.
pub type PageMapper = fn(u64, u64, u64) -> bool;
static PAGE_MAPPER: std::sync::OnceLock<PageMapper> = std::sync::OnceLock::new();

/// The runtime's page mapper, for the queues' page-mapping copies (memory
/// is the runtime's, as on the console).
pub fn use_page_mapper(mapper: PageMapper) {
    let _ = PAGE_MAPPER.set(mapper);
}

pub(crate) fn page_mapper() -> Option<PageMapper> {
    PAGE_MAPPER.get().copied()
}

/// Allocation attributes for resource memory: allocator id, page size
/// (bits 14-15: 2 MiB pages from 2 MiB up, else 64 KiB), alignment as log2
/// (bits 24-28) and memory type (bits 29-32: write-combined for GPU-only
/// heaps, cacheable for the CPU-visible ones).
/// shortcut: the console runtime's own choice of page size and memory type
/// is not known; this is the plausible one.
fn resource_attributes(info: AllocationInfo, heap_kind: u32) -> u64 {
    const GRAPHICS_ALLOCATOR: u64 = 128 << 16;
    const DEFAULT_HEAP: u32 = 1;
    let pages: u64 = if info.size >= 2 << 20 { 2 } else { 1 };
    let memory: u64 = if heap_kind == DEFAULT_HEAP { 1 } else { 3 };
    let alignment = u64::from(info.alignment.max(1).trailing_zeros());
    GRAPHICS_ALLOCATOR | pages << 14 | alignment << 24 | memory << 29
}

impl Memory {
    /// A committed resource's memory: from the title's allocator if the
    /// runtime named one, else our own; zeroed either way.
    fn for_resource(info: AllocationInfo, heap_kind: u32) -> Result<Memory, HRESULT> {
        let Some(&(alloc, _)) = ALLOCATOR.get() else {
            return Memory::new(info);
        };
        let layout = Layout::from_size_align(info.size as usize, info.alignment as usize)
            .map_err(|_| E_INVALIDARG)?;
        let attributes = resource_attributes(info, heap_kind);
        let ptr =
            NonNull::new(alloc(layout.size(), attributes).cast::<u8>()).ok_or(E_OUTOFMEMORY)?;
        // SAFETY: the allocator handed out `size` bytes.
        unsafe { ptr.as_ptr().write_bytes(0, layout.size()) };
        Ok(Memory {
            ptr,
            layout,
            attributes: Some(attributes),
        })
    }

    /// `size` zeroed bytes aligned to `alignment` (a power of two).
    pub(crate) fn zeroed(size: u64, alignment: u64) -> Result<Memory, HRESULT> {
        Memory::new(AllocationInfo { size, alignment })
    }

    pub(crate) fn address(&self) -> u64 {
        self.ptr.as_ptr() as u64
    }

    fn new(info: AllocationInfo) -> Result<Memory, HRESULT> {
        let layout = usize::try_from(info.size)
            .ok()
            .zip(usize::try_from(info.alignment).ok())
            .filter(|&(size, _)| size > 0)
            .and_then(|(size, align)| Layout::from_size_align(size, align).ok())
            .ok_or(E_INVALIDARG)?;
        // SAFETY: the size is non-zero.
        let ptr = NonNull::new(unsafe { alloc_zeroed(layout) }).ok_or(E_OUTOFMEMORY)?;
        Ok(Memory {
            ptr,
            layout,
            attributes: None,
        })
    }
}

impl Drop for Memory {
    fn drop(&mut self) {
        match (self.attributes, ALLOCATOR.get()) {
            (Some(attributes), Some(&(_, free))) => free(self.ptr.as_ptr().cast(), attributes),
            // SAFETY: allocated in `new` with this layout.
            _ => unsafe { dealloc(self.ptr.as_ptr(), self.layout) },
        }
    }
}

#[repr(C)]
struct Heap {
    object: Object,
    desc: HeapDesc,
    memory: Memory,
}

pub(crate) extern "system" fn create_heap(
    _: This,
    desc: *const HeapDesc,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(&desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    tracing::debug!(
        size = desc.size,
        kind = desc.properties.kind,
        flags = format_args!("{:#x}", desc.flags),
        "heap"
    );
    if desc.alignment != 0 && !desc.alignment.is_power_of_two() {
        return E_INVALIDARG;
    }
    let info = AllocationInfo {
        size: desc.size,
        alignment: desc.alignment.max(KIB_64),
    };
    let memory = match Memory::new(info) {
        Ok(memory) => memory,
        Err(hr) => return hr,
    };
    let heap = Box::new(Heap {
        object: Object::new(&HEAP_FUNCTIONS),
        desc,
        memory,
    });
    object::hand_out(HEAP, heap, iid, &HEAP_IIDS, out)
}

extern "system" fn heap_query(this: This, iid: *const Iid, out: *mut This) -> HRESULT {
    object::answer(HEAP, iid, object::is(iid, &HEAP_IIDS).then_some(this), out)
}
extern "system" fn heap_release(this: This) -> u32 {
    object::release_boxed::<Heap>(this)
}
extern "system" fn heap_desc(this: This, out: *mut HeapDesc) {
    if !out.is_null() {
        // SAFETY: `this` is a heap; the title passes room for a description.
        unsafe { out.write((*this.cast::<Heap>()).desc) };
    }
}

/// A resource's memory: its own, or a range of a heap it holds a reference
/// to.
enum Backing {
    Committed(Memory),
    Placed {
        heap: This,
        offset: u64,
    },
    /// Memory the title mapped itself, at this address.
    Address(*mut u8),
}

impl Drop for Backing {
    fn drop(&mut self) {
        if let Backing::Placed { heap, .. } = *self {
            heap_release(heap);
        }
    }
}

#[repr(C)]
struct Resource {
    object: Object,
    desc: ResourceDesc,
    heap: HeapProperties,
    heap_flags: u32,
    backing: Backing,
    /// Textures also live on the host GPU (decision 0007); buffers do not
    /// yet.
    #[cfg(windows)]
    host: Option<std::sync::Arc<HostTexture>>,
}

/// A title texture's host copy and the state it is in. The title's barriers
/// name console states (looser than the host's), so we transition from this
/// one. Lists that use the texture hold it until they are reset: the title
/// may release a resource whose commands it recorded but not yet submitted
/// (console memory outlives the object; a host texture must outlive the
/// lists that name it).
///
/// One per texture memory (decision 0007, "One host texture per texture
/// memory"): title textures at the same address with the same shape and
/// element size share it, each viewing it in its own format.
#[cfg(windows)]
pub(crate) struct HostTexture {
    pub(crate) texture: windows::Win32::Graphics::Direct3D12::ID3D12Resource,
    pub(crate) state: std::sync::atomic::AtomicU32,
    /// Formats views may take besides the texture's own family: the host
    /// took them as castable formats when it made the texture.
    pub(crate) castable: Vec<u32>,
    /// A host copy or resolve wrote it: its contents are the host's, not
    /// title memory's (`crate::texture`).
    pub(crate) host_written: std::sync::atomic::AtomicBool,
    /// Its uploads from title memory (`crate::texture`), made on first use.
    pub(crate) staging: std::sync::Mutex<Option<std::sync::Arc<crate::texture::Staging>>>,
    /// A depth texture's writable twin (`crate::twin`), made with its first
    /// unordered access view; `None` inside if it has none.
    pub(crate) twin: std::sync::OnceLock<Option<crate::twin::Twin>>,
    /// Top mip levels left out under `VICTORY_TEXTURE_MAX`: host mip `i`
    /// holds the title's mip `i + mip_drop` (0 with no cap).
    pub(crate) mip_drop: u32,
    /// Bytes it takes on the host GPU.
    bytes: u64,
    /// Its contents come from title memory (not host-written).
    filled: bool,
    /// A list used it (counted once).
    used: std::sync::atomic::AtomicBool,
}

#[cfg(windows)]
impl HostTexture {
    /// A host texture made outside the title's resources (twins).
    pub(crate) fn new(
        texture: windows::Win32::Graphics::Direct3D12::ID3D12Resource,
        state: u32,
        castable: Vec<u32>,
        filled: bool,
        mip_drop: u32,
    ) -> Self {
        let bytes = crate::host::host().map_or(0, |h| {
            // SAFETY: plain queries on a live resource.
            unsafe { h.device.GetResourceAllocationInfo(0, &[texture.GetDesc()]) }.SizeInBytes
        });
        TEXTURES.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        TEXTURE_BYTES.fetch_add(bytes, std::sync::atomic::Ordering::Relaxed);
        if filled {
            FILLED_BYTES.fetch_add(bytes, std::sync::atomic::Ordering::Relaxed);
        }
        HostTexture {
            texture,
            state: state.into(),
            castable,
            host_written: false.into(),
            staging: std::sync::Mutex::new(None),
            twin: std::sync::OnceLock::new(),
            mip_drop,
            bytes,
            filled,
            used: false.into(),
        }
    }

    /// A list uses it: counted the first time (diagnostics of how much of
    /// the host textures' memory the GPU ever touches).
    pub(crate) fn note_used(&self) {
        if !self.used.swap(true, std::sync::atomic::Ordering::Relaxed) {
            USED.fetch_add(self.bytes, std::sync::atomic::Ordering::Relaxed);
        }
    }
}

/// Bytes of host textures some list used (while they live).
#[cfg(windows)]
static USED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// Host textures alive and the bytes they take (the fps line's companion).
#[cfg(windows)]
static TEXTURES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
#[cfg(windows)]
static TEXTURE_BYTES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// Title textures that found their memory's host texture made already.
#[cfg(windows)]
static SHARED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// Bytes the title textures that shared would have taken in host textures
/// of their own.
#[cfg(windows)]
static SAVED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
/// Title textures whose memory's host texture could not take them (more
/// uses, a format it cannot be viewed in): a new one replaced it.
#[cfg(windows)]
static REPLACED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// A title texture whose contents come from title memory: the host neither
/// renders to it nor writes it (no target/depth/unordered flag).
#[cfg(windows)]
fn fills(desc: &ResourceDesc) -> bool {
    desc.dimension != BUFFER && desc.flags & USES == 0
}

/// Bytes of live host textures the title fills.
#[cfg(windows)]
static FILLED_BYTES: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// Host textures alive, their bytes and the bytes of those lists used,
/// title textures that shared one (and the bytes that saved), those that
/// replaced their memory's.
#[cfg(windows)]
pub(crate) fn texture_counts() -> (u64, u64, u64, u64, u64, u64) {
    use std::sync::atomic::Ordering::Relaxed;
    let shared = (SHARED.load(Relaxed), SAVED.load(Relaxed), REPLACED.load(Relaxed));
    (TEXTURES.load(Relaxed), TEXTURE_BYTES.load(Relaxed), USED.load(Relaxed), shared.0, shared.1, shared.2)
}

#[cfg(windows)]
impl Drop for HostTexture {
    fn drop(&mut self) {
        TEXTURES.fetch_sub(1, std::sync::atomic::Ordering::Relaxed);
        TEXTURE_BYTES.fetch_sub(self.bytes, std::sync::atomic::Ordering::Relaxed);
        if self.filled {
            FILLED_BYTES.fetch_sub(self.bytes, std::sync::atomic::Ordering::Relaxed);
        }
        if *self.used.get_mut() {
            USED.fetch_sub(self.bytes, std::sync::atomic::Ordering::Relaxed);
        }
        #[cfg(feature = "diagnostics")]
        {
            use windows::core::Interface;
            life::note(self.texture.as_raw() as usize, life::LET_GO, 0, life::holders(&self.texture));
        }
    }
}

/// Diagnostics: the last events in host textures' lives, for
/// device-removed reports that find a fault inside a freed one (host
/// textures are named with their address, `@0x...`).
#[cfg(all(windows, feature = "diagnostics"))]
pub(crate) mod life {
    use std::collections::VecDeque;
    use std::sync::Mutex;

    /// The title released the resource the texture copies.
    pub(crate) const RELEASED: u8 = 1;
    /// A view heap slot (the value) stopped viewing it.
    pub(crate) const SLOT: u8 = 2;
    /// The shared host copy (title resource and lists) let go of it.
    pub(crate) const LET_GO: u8 = 3;
    /// A submission started (value: its number; no texture).
    pub(crate) const SUBMITTED: u8 = 4;
    /// That submission's lists have run.
    pub(crate) const RAN: u8 = 5;

    /// (texture, time, kind, value, references left after the event).
    type Event = (usize, std::time::Instant, u8, u64, u32);
    static EVENTS: Mutex<VecDeque<Event>> = Mutex::new(VecDeque::new());
    const KEPT: usize = 1 << 18;

    /// The references `texture` has besides the one about to go.
    pub(crate) fn holders(texture: &windows::Win32::Graphics::Direct3D12::ID3D12Resource) -> u32 {
        use windows::core::Interface;
        // SAFETY: a live COM object; one reference taken and given back.
        unsafe {
            let raw = texture.as_raw();
            let unknown = &*(raw as *const *const windows::core::IUnknown_Vtbl);
            ((**unknown).AddRef)(raw);
            ((**unknown).Release)(raw).saturating_sub(1)
        }
    }

    /// `left`: the texture's references once the event's own is gone.
    pub(crate) fn note(texture: usize, kind: u8, value: u64, left: u32) {
        let mut events = EVENTS.lock().unwrap_or_else(|e| e.into_inner());
        if events.len() >= KEPT {
            events.pop_front();
        }
        events.push_back((texture, std::time::Instant::now(), kind, value, left));
    }

    /// What happened to the texture named `name` (`...@0x<address>`), with
    /// the submissions around it, times relative to now.
    pub(crate) fn of(name: &str) -> String {
        let Some(at) = name
            .rsplit_once("@0x")
            .and_then(|(_, a)| usize::from_str_radix(a.split(|c: char| !c.is_ascii_hexdigit()).next()?, 16).ok())
        else {
            return String::new();
        };
        let now = std::time::Instant::now();
        let events = EVENTS.lock().unwrap_or_else(|e| e.into_inner());
        let mut text = Vec::new();
        let mut last_submission = None;
        for &(t, when, kind, value, left) in events.iter() {
            let ago = now.duration_since(when).as_secs_f64() * 1e3;
            match kind {
                SUBMITTED | RAN => last_submission = Some((kind, value, ago)),
                _ if t == at => {
                    if let Some((k, n, a)) = last_submission.take() {
                        let what = if k == SUBMITTED { "started" } else { "ran" };
                        text.push(format!("(submission {n} {what} {a:.1} ms ago)"));
                    }
                    let what = match kind {
                        RELEASED => "the title released its resource".to_owned(),
                        SLOT => format!("view heap slot {value} stopped viewing it"),
                        _ => "the shared host copy let go".to_owned(),
                    };
                    let freed = match (kind, left) {
                        (SLOT, 0) => ", its last holder (freed after the submission running then)",
                        (LET_GO, 0) => ", FREED",
                        _ => "",
                    };
                    text.push(format!("{ago:.1} ms ago {what}{freed}"));
                }
                _ => {}
            }
        }
        if let Some((k, n, a)) = last_submission {
            let what = if k == SUBMITTED { "started" } else { "ran" };
            text.push(format!("(last: submission {n} {what} {a:.1} ms ago)"));
        }
        format!(" [{name}: {}]", text.join("; "))
    }
}

/// Moves the host texture to `wanted` (a host texture state); the host
/// resource and the state it leaves, if that is a change.
#[cfg(windows)]
pub(crate) fn host_transition(this: This, wanted: u32) -> Option<(*mut c_void, u32)> {
    use windows::core::Interface;
    let host = resource(this).host.as_ref()?;
    let before = host
        .state
        .swap(wanted, std::sync::atomic::Ordering::Relaxed);
    (before != wanted).then(|| (host.texture.as_raw(), before))
}

/// The host GPU's copy of a title resource, if it has one.
#[cfg(windows)]
pub(crate) fn host_resource<'a>(
    this: This,
) -> Option<&'a windows::Win32::Graphics::Direct3D12::ID3D12Resource> {
    if this.is_null() {
        return None;
    }
    resource(this).host.as_deref().map(|h| &h.texture)
}

/// A title texture's host copy, shared, for a list to hold.
#[cfg(windows)]
pub(crate) fn host_copy(this: This) -> Option<std::sync::Arc<HostTexture>> {
    if this.is_null() {
        return None;
    }
    resource(this).host.clone()
}

/// The host description of a title texture: the same, without the
/// console's layout and flags, in a format the host takes for its use.
#[cfg(windows)]
fn host_desc(desc: &ResourceDesc) -> Option<ResourceDesc> {
    const PC_FLAGS: u32 = 0xf; // render target, depth, UAV, no shader access
    if desc.dimension == BUFFER {
        return None;
    }
    let mut d = *desc;
    d.layout = 0;
    d.alignment = 0;
    d.flags &= PC_FLAGS;
    // PC refuses depth targets that are also unordered-access (the console
    // allows both; Grand Theft Auto VI's shadow maps and material depth ask
    // for it): unordered views of such a texture write its twin
    // (`crate::twin`).
    if d.flags & DEPTH != 0 {
        d.flags &= !UNORDERED;
    }
    d.format = pc_format(d.format);
    // Opt-in packed-light substitution: a target/UAV holding packed light
    // (R9G9B9E5 or the R32_UINT it is written through) is hosted as RGBA16F
    // when `light_effective_format` says so. Its views are remapped to the
    // same format in `descriptor::host` so the pipeline stays consistent.
    if d.flags & USES & (RENDER_TARGET_FLAG | UNORDERED) != 0 {
        d.format = target_format(d.format);
    }
    const DENY_SHADER_RESOURCE: u32 = 8;
    if d.flags & DENY_SHADER_RESOURCE == 0
        && let Some(&(_, typeless)) = DEPTH_FORMATS.iter().find(|&&(f, _)| f == d.format)
    {
        d.format = typeless;
    }
    // The host has no sRGB unordered access: such textures are typeless,
    // their unordered access views plain UNORM.
    if d.flags & UNORDERED != 0 && SRGB.iter().any(|&(srgb, _)| srgb == d.format) {
        d.format = family(d.format);
    }
    // Opt-in `VICTORY_TEXTURE_MAX`: a sampled-only power-of-two texture
    // gives up its largest mips, so the host allocates only the levels the
    // cap allows. Render targets, depth and UAVs keep the title's sizes.
    let drop = texture_mip_drop(desc);
    if drop > 0 {
        d.width >>= drop;
        d.height >>= drop;
        d.mip_levels = (mip_count(desc) - drop) as u16;
    }
    Some(d)
}

const DEPTH: u32 = 2;
/// Render target: the console's flag 1, also the host's render-target use.
#[cfg(windows)]
const RENDER_TARGET_FLAG: u32 = 1;
#[cfg(windows)]
const UNORDERED: u32 = 4;
/// Render target, depth, unordered access: what a host texture allows.
#[cfg(windows)]
const USES: u32 = 0x7;

/// What a texture memory is, for the one host texture it gets: its address
/// and shape (the console's layout resolved, mips counted, and the bytes
/// and texels of an element), and whether it is a depth texture (which
/// cannot share with colour ones on the host).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct MemoryKey {
    address: u64,
    dimension: u32,
    width: u64,
    height: u32,
    depth_or_array: u16,
    mips: u32,
    samples: u32,
    swizzle: u32,
    element: (u64, u64),
    depth: bool,
}

/// The texture memory `desc` describes at `address`; `None` for buffers
/// and formats without a known element.
pub(crate) fn memory_key(address: u64, desc: &ResourceDesc) -> Option<MemoryKey> {
    if desc.dimension == BUFFER {
        return None;
    }
    Some(MemoryKey {
        address,
        dimension: desc.dimension,
        width: desc.width,
        height: desc.height,
        depth_or_array: desc.depth_or_array_size,
        mips: mip_count(desc),
        samples: desc.sample_count.max(1),
        swizzle: texture_layout(desc).map_or(desc.layout, |t| t.planes[0].layout.desc.swizzle.0),
        element: element(desc.format)?,
        depth: desc.flags & DEPTH != 0,
    })
}

/// The formats a host texture of `format` is made castable to: every
/// format of the same element (bytes and block texels), as the host's
/// format list casting allows (uncompressed among uncompressed, block
/// formats among block formats of the same block size), but depth ones
/// (refused in lists on the RX 6700 XT and WARP alike) and typeless ones
/// (no view takes them).
pub(crate) fn castable(format: u32) -> Vec<u32> {
    const DEPTH_FAMILIES: [u32; 10] = [19, 20, 21, 22, 40, 44, 45, 46, 47, 55];
    let Some(e) = element(format) else {
        return Vec::new();
    };
    (1..=115)
        .filter(|&f| f != format && element(f) == Some(e) && !DEPTH_FAMILIES.contains(&f))
        .filter(|&f| !(family(f) == f && family(f + 1) == f))
        .collect()
}

/// Live host textures by texture memory: title textures (resource objects
/// and placed views) of one memory share one.
#[cfg(windows)]
static MEMORY: std::sync::Mutex<
    Option<std::collections::HashMap<MemoryKey, std::sync::Weak<HostTexture>>>,
> = std::sync::Mutex::new(None);

/// The host texture of the memory at `address` that `desc` describes: the
/// live one if it takes `desc` (its uses, a format it can be viewed in),
/// else a new one, which becomes the memory's.
#[cfg(windows)]
fn shared_texture(
    address: u64,
    desc: &ResourceDesc,
    state: u32,
    clear: *const c_void,
) -> Option<std::sync::Arc<HostTexture>> {
    let d = host_desc(desc)?;
    let key = memory_key(address, desc);
    let mut map = MEMORY.lock().unwrap_or_else(|e| e.into_inner());
    let map = map.get_or_insert_with(Default::default);
    let live = key.and_then(|k| map.get(&k)?.upgrade());
    if let Some(t) = &live
        && takes(t, &d)
    {
        SHARED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if let Some(host) = crate::host::host() {
            // SAFETY: a plain query on a host description (same layout).
            let info = unsafe { host.device.GetResourceAllocationInfo(0, &[std::ptr::from_ref(&d).cast::<windows::Win32::Graphics::Direct3D12::D3D12_RESOURCE_DESC>().read_unaligned()]) };
            SAVED.fetch_add(info.SizeInBytes, std::sync::atomic::Ordering::Relaxed);
        }
        return live;
    }
    // shortcut: title textures of a replaced host texture keep it (placed
    // views move to the new one, `texture_at`); copies (`alias_source`)
    // bridge the two where the shapes allow.
    let t = std::sync::Arc::new(make_texture(&d, state, clear, texture_mip_drop(desc))?);
    if let Some(k) = key {
        if live.is_some() {
            REPLACED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            tracing::debug!(?k, "a texture memory's host texture replaced by one with more uses or another format");
        }
        if map.len().is_power_of_two() {
            map.retain(|_, w| w.strong_count() > 0);
        }
        map.insert(k, std::sync::Arc::downgrade(&t));
    }
    Some(t)
}

/// Whether host texture `t` takes a title texture of host description
/// `d` at its memory: its uses, and a view format of its family or
/// castable list.
#[cfg(windows)]
fn takes(t: &HostTexture, d: &ResourceDesc) -> bool {
    // SAFETY: a live resource.
    let td = unsafe { t.texture.GetDesc() };
    let (flags, own) = (td.Flags.0 as u32, td.Format.0 as u32);
    d.flags & USES & !flags == 0
        && (family(d.format) == family(own)
            || t.castable.iter().any(|&c| family(c) == family(d.format)))
}

/// Whether `this`'s host texture is still its memory's (not replaced by one
/// with more uses).
#[cfg(windows)]
fn current(this: This) -> bool {
    let r = resource(this);
    let (Some(h), Some(k)) = (&r.host, memory_key(r.data() as u64, &r.desc)) else {
        return true;
    };
    let map = MEMORY.lock().unwrap_or_else(|e| e.into_inner());
    map.as_ref()
        .and_then(|m| m.get(&k)?.upgrade())
        .is_none_or(|live| std::sync::Arc::ptr_eq(&live, h))
}

/// A host texture of host description `d` in GPU memory, castable to
/// [`castable`]'s formats (colour textures, through the enhanced-barrier
/// creation; it starts in the common state) where the host takes that.
#[cfg(windows)]
fn make_texture(
    d: &ResourceDesc,
    state: u32,
    clear: *const c_void,
    mip_drop: u32,
) -> Option<HostTexture> {
    use windows::Win32::Graphics::Direct3D12::*;
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;
    use windows::core::Interface;
    let host = crate::host::host()?;
    // The texture budget gate was removed: on an 8 GB card it refused
    // textures the world still needed (`filled_mb` ~2.8 GB), leaving holes in
    // the frame. VRAM pressure is handled by the host's own allocations.
    // The clear value names the format too: format, then color or depth
    // and stencil (5 words).
    let clear_value = (!clear.is_null()).then(|| {
        // SAFETY: the title passes a whole clear value.
        let mut value = unsafe { clear.cast::<[u32; 5]>().read_unaligned() };
        value[0] = pc_format(value[0]);
        // A stand-in target's clear value names the stand-in too: the
        // colour words are floats in both formats, so only the tag changes.
        if d.flags & (RENDER_TARGET_FLAG | UNORDERED) != 0 {
            value[0] = target_format(value[0]);
        }
        value
    });
    let clear_value = clear_value.as_ref().map(|v| std::ptr::from_ref(v).cast::<D3D12_CLEAR_VALUE>());
    let heap = D3D12_HEAP_PROPERTIES {
        Type: D3D12_HEAP_TYPE_DEFAULT,
        ..Default::default()
    };
    let mut texture: Option<ID3D12Resource> = None;
    let mut castable_formats = if d.flags & DEPTH == 0 { castable(d.format) } else { Vec::new() };
    let mut initial = crate::host::legal_for(d.flags, crate::host::texture_state(state));
    if !castable_formats.is_empty()
        && let Ok(device) = host.device.cast::<ID3D12Device10>()
    {
        // SAFETY: the description has the host layout (checked size).
        let p = unsafe { std::ptr::from_ref(d).cast::<D3D12_RESOURCE_DESC>().read_unaligned() };
        // The plain description plus the sampler feedback region (none).
        let desc1 = D3D12_RESOURCE_DESC1 {
            Dimension: p.Dimension,
            Alignment: p.Alignment,
            Width: p.Width,
            Height: p.Height,
            DepthOrArraySize: p.DepthOrArraySize,
            MipLevels: p.MipLevels,
            Format: p.Format,
            SampleDesc: p.SampleDesc,
            Layout: p.Layout,
            Flags: p.Flags,
            SamplerFeedbackMipRegion: Default::default(),
        };
        let formats: Vec<DXGI_FORMAT> = castable_formats.iter().map(|&f| DXGI_FORMAT(f as i32)).collect();
        // D3D12 rejects a non-NULL pCastableFormats with no formats. Most
        // textures have none, so do not send an empty list and take the
        // debug layer's error (and the fallback) on every single creation.
        let castable = (!formats.is_empty()).then_some(formats.as_slice());
        // SAFETY: a host description and our out pointer; the list and the
        // clear value live through the call.
        let made = unsafe {
            device.CreateCommittedResource3(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc1,
                D3D12_BARRIER_LAYOUT_COMMON,
                clear_value,
                None::<&ID3D12ProtectedResourceSession>,
                castable,
                &mut texture,
            )
        };
        if let Err(e) = made {
            static ONCE: std::sync::Once = std::sync::Once::new();
            ONCE.call_once(|| {
                tracing::warn!(error = %e, desc = ?d, "the host refuses castable formats: textures of one memory in other formats get their own host texture");
            });
            texture = None;
        } else {
            // The common layout is the common state.
            initial = 0;
        }
    }
    if texture.is_none() {
        castable_formats.clear();
        // SAFETY: the description has the host layout (checked size, same
        // fields); the out pointer is ours.
        let made = unsafe {
            host.device.CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                std::ptr::from_ref(d).cast(),
                D3D12_RESOURCE_STATES(initial as i32),
                clear_value,
                &mut texture,
            )
        };
        if let Err(e) = made {
            tracing::warn!(error = %e, desc = ?d, "no host texture");
        }
    }
    let texture = texture?;
    crate::host::name(&texture, || {
        // Its address, for the lifetime notes (`life`) of a freed one.
        format!("texture {}x{} f{} flags {:#x} @{:#x}", d.width, d.height, d.format, d.flags, texture.as_raw() as usize)
    });
    let initial = crate::host::legal_state(&texture, initial);
    Some(HostTexture::new(
        texture,
        initial,
        castable_formats,
        fills(d),
        mip_drop,
    ))
}

/// A texture's console memory and description; `None` for buffers.
pub(crate) fn texture_memory(this: This) -> Option<(*const u8, ResourceDesc)> {
    if this.is_null() {
        return None;
    }
    let r = resource(this);
    (r.desc.dimension != BUFFER).then(|| (r.data().cast_const(), r.desc))
}

/// The kind of a title texture: width, height and format.
#[cfg(windows)]
pub(crate) fn texture_kind(this: This) -> Option<(u64, u32, u32)> {
    if this.is_null() {
        return None;
    }
    let r = resource(this);
    const TEXTURE_2D: u32 = 3;
    (r.desc.dimension == TEXTURE_2D && r.desc.sample_count <= 1)
        .then_some((r.desc.width, r.desc.height, r.desc.format))
}

/// A title texture's console layout, allocation address and size.
#[cfg(windows)]
pub(crate) fn title_texture(this: This) -> Option<(victory_formats::texture::Layout, usize, usize)> {
    if this.is_null() {
        return None;
    }
    let r = resource(this);
    if r.desc.dimension == BUFFER {
        return None;
    }
    let t = texture_layout(&r.desc)?;
    let [plane] = &t.planes[..] else {
        return None;
    };
    Some((plane.layout.clone(), r.data() as usize, t.size as usize))
}

/// A texture's format as the title gave it.
pub(crate) fn title_format(this: This) -> Option<u32> {
    (!this.is_null()).then(|| resource(this).desc.format)
}

/// A buffer's host memory and size; `None` for textures.
pub(crate) fn buffer_bytes(this: This) -> Option<(*mut u8, u64)> {
    if this.is_null() {
        return None;
    }
    let r = resource(this);
    (r.desc.dimension == BUFFER).then(|| (r.data(), r.desc.width))
}

impl Resource {
    fn data(&self) -> *mut u8 {
        match self.backing {
            Backing::Committed(ref memory) => memory.ptr.as_ptr(),
            // SAFETY: placement checked the range lies inside the heap.
            Backing::Placed { heap, offset } => unsafe {
                (*heap.cast::<Heap>())
                    .memory
                    .ptr
                    .as_ptr()
                    .add(offset as usize)
            },
            Backing::Address(at) => at,
        }
    }
}

fn resource<'a>(this: This) -> &'a Resource {
    // SAFETY: called from resource methods only.
    unsafe { &*this.cast::<Resource>() }
}

// The creation methods' own arguments, passed on.
#[allow(clippy::too_many_arguments)]
fn hand_out_resource(
    desc: ResourceDesc,
    heap: HeapProperties,
    heap_flags: u32,
    backing: Backing,
    state: u32,
    clear: *const c_void,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    #[cfg(not(windows))]
    let _ = (state, clear);
    #[allow(unused_mut)] // the host texture is set on Windows only
    let mut resource = Box::new(Resource {
        object: Object::new(&RESOURCE_FUNCTIONS),
        desc,
        heap,
        heap_flags,
        backing,
        #[cfg(windows)]
        host: None,
    });
    #[cfg(windows)]
    {
        resource.host = shared_texture(resource.data() as u64, &desc, state, clear);
        if desc.dimension != BUFFER {
            crate::texback::replaced(resource.data() as usize);
        }
    }
    #[cfg(windows)]
    if desc.dimension == BUFFER {
        let at = std::ptr::from_ref::<Resource>(&resource) as usize;
        crate::shadow::register(at, resource.data() as u64, desc.width);
    } else if resource.host.is_some() {
        let at = std::ptr::from_ref::<Resource>(&resource) as usize;
        let mut map = TEXTURES_AT.lock().unwrap_or_else(|e| e.into_inner());
        map.entry(resource.data() as u64).or_default().push(at);
    }
    object::hand_out(RESOURCE, resource, iid, &RESOURCE_IIDS, out)
}

/// Title textures with a host copy, by the console memory they start at.
/// Title textures of one memory share a host texture where the host can
/// view it in each one's format; where it cannot (block-compressed against
/// plain formats, a cast the host refused, a host texture replaced),
/// [`alias_source`] finds the one the GPU writes.
#[cfg(windows)]
static TEXTURES_AT: std::sync::Mutex<std::collections::BTreeMap<u64, Vec<usize>>> =
    std::sync::Mutex::new(std::collections::BTreeMap::new());

/// For a texture made for a placed view (`texture_at`): a title texture
/// the GPU writes (render target, depth or unordered access) at the same
/// memory, in the same layout and with the same bytes per row and rows
/// (elements of the same size, block-compressed ones counted in blocks),
/// that has another host texture: what the GPU wrote there lives in that
/// one, to be copied over (`texture::alias_copy`).
///
/// This is the light/R9G9B9E5 case from the console: the title packs light
/// as shared-exponent words into R32_UINT textures with compute shaders
/// (which cannot write R9G9B9E5) and samples them through placed
/// R9G9B9E5 views. When both formats share one host texture (the common
/// case: R9G9B9E5 is castable over R32_UINT) no copy is needed and the
/// texels are read directly; when they do not (another mip or slice, or a
/// cast the host refused), this finds the written texture and the read
/// copies its bytes. With `VICTORY_LIGHT_FORMAT_FLOAT=1` a substituted
/// target (R9G9B9E5 or R32_UINT) is excluded: its host texels are RGBA16F,
/// not the title's packed words, so a raw copy would read garbage.
/// shortcut: same start only; overlapping ranges, other mips or slices of
/// one memory, and views of buffers are not matched.
#[cfg(windows)]
pub(crate) fn alias_source(placed: This) -> Option<This> {
    const WRITTEN: u32 = 0x7; // render target, depth, unordered access
    let b = resource(placed);
    let map = TEXTURES_AT.lock().unwrap_or_else(|e| e.into_inner());
    let bytes = |d: &ResourceDesc| {
        let (size, block) = element(d.format)?;
        let swizzle = texture_layout(d).map_or(d.layout, |t| t.planes[0].layout.desc.swizzle.0);
        let rows = u64::from(d.height).div_ceil(block);
        Some((d.dimension, d.width.div_ceil(block) * size, rows, d.depth_or_array_size, d.sample_count, swizzle))
    };
    let want = bytes(&b.desc)?;
    map.get(&(b.data() as u64))?
        .iter()
        .map(|&r| r as This)
        .filter(|&r| r != placed)
        .find(|&r| {
            let a = resource(r);
            // A substituted target lives on the host in RGBA16F, not in the
            // title's R9G9B9E5 texels: copying its bits into the placed view
            // would read garbage. The ordinary R32_UINT light texture is not
            // substituted, so the castable pair (or this copy) still works.
            a.desc.flags & WRITTEN != 0
                && target_format(a.desc.format) == a.desc.format
                && bytes(&a.desc) == Some(want)
                && matches!((&a.host, &b.host), (Some(x), Some(y)) if !std::sync::Arc::ptr_eq(x, y))
        })
}

#[cfg(windows)]
impl Drop for Resource {
    fn drop(&mut self) {
        #[cfg(feature = "diagnostics")]
        if self
            .host
            .as_ref()
            .is_some_and(|h| std::sync::Arc::strong_count(h) > 1)
        {
            // Before lists held their textures, this freed a texture that
            // lists not yet run still named (a GPU page fault).
            static OUTLIVED: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let n = OUTLIVED.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
            if n.is_power_of_two() {
                tracing::info!(
                    n,
                    desc = ?self.desc,
                    "a title texture was released while a list not yet reset names its host copy"
                );
            }
        }
        #[cfg(feature = "diagnostics")]
        if let Some(h) = &self.host {
            use windows::core::Interface;
            life::note(h.texture.as_raw() as usize, life::RELEASED, 0, life::holders(&h.texture));
        }
        crate::bindless::forget(std::ptr::from_ref::<Resource>(self) as usize);
        if self.desc.dimension != BUFFER {
            crate::texback::released(std::ptr::from_ref::<Resource>(self) as usize);
            let mut map = TEXTURES_AT.lock().unwrap_or_else(|e| e.into_inner());
            let at = self.data() as u64;
            if let Some(list) = map.get_mut(&at) {
                list.retain(|&r| r != std::ptr::from_ref::<Resource>(self) as usize);
                if list.is_empty() {
                    map.remove(&at);
                }
            }
        }
        crate::descriptor::forget(std::ptr::from_ref::<Resource>(self) as usize);
        if self.desc.dimension == BUFFER {
            crate::shadow::unregister(
                std::ptr::from_ref::<Resource>(self) as usize,
                self.data() as u64,
            );
        }
    }
}

pub(crate) extern "system" fn create_committed(
    _: This,
    heap: *const HeapProperties,
    heap_flags: u32,
    desc: *const ResourceDesc,
    state: u32,
    clear: *const c_void,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes both descriptions (null is tolerated).
    let (Some(&heap), Some(&desc)) = (unsafe { (heap.as_ref(), desc.as_ref()) }) else {
        return E_INVALIDARG;
    };
    let Some(info) = footprint(&desc) else {
        tracing::warn!(?desc, "no size for this resource");
        return E_INVALIDARG;
    };
    tracing::debug!(
        dimension = desc.dimension,
        size = info.size,
        heap = heap.kind,
        "committed resource"
    );
    let memory = match Memory::for_resource(info, heap.kind) {
        Ok(memory) => memory,
        Err(hr) => return hr,
    };
    hand_out_resource(
        desc,
        heap,
        heap_flags,
        Backing::Committed(memory),
        state,
        clear,
        iid,
        out,
    )
}

/// Device method 0x3f8, inferred from Grand Theft Auto VI's calls
/// (title+0x193e1b4: its back buffers, heap flag "allow display"): the
/// committed resource creation of the enhanced-barrier interfaces,
/// `(heap, heap flags, desc1, initial layout, clear value, protected
/// session, castable format count, castable formats, iid, out)`. The host
/// copy starts in the common state, as layouts are not tracked.
#[allow(clippy::too_many_arguments)] // the method's own arguments
pub(crate) extern "system" fn create_committed3(
    this: This,
    heap: *const HeapProperties,
    heap_flags: u32,
    desc: *const ResourceDesc,
    _layout: u32,
    clear: *const c_void,
    _protected: This,
    _castable_count: u32,
    _castable: *const u32,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    const COMMON: u32 = 0;
    create_committed(this, heap, heap_flags, desc, COMMON, clear, iid, out)
}

/// Registered page pools (64 KiB pages the title maps into tiled
/// resources by page index): first address and page count; a handle is an
/// index plus one, a removed pool leaves `None`.
static PAGE_POOLS: std::sync::Mutex<Vec<Option<(u64, u32)>>> = std::sync::Mutex::new(Vec::new());

fn page_pools() -> std::sync::MutexGuard<'static, Vec<Option<(u64, u32)>>> {
    PAGE_POOLS.lock().unwrap_or_else(|e| e.into_inner())
}

/// Device method 0x250: `(address, page count, handle out)`.
pub(crate) extern "system" fn register_page_pool(
    _: This,
    address: u64,
    pages: u32,
    out: *mut usize,
) -> HRESULT {
    if out.is_null() || pages == 0 || !address.is_multiple_of(KIB_64) {
        return E_INVALIDARG;
    }
    tracing::debug!(address = format_args!("{address:#x}"), pages, "page pool");
    let mut pools = page_pools();
    pools.push(Some((address, pages)));
    // SAFETY: checked non-null; the title passes room for a handle.
    unsafe { out.write(pools.len()) };
    S_OK
}

/// Device method 0x258.
pub(crate) extern "system" fn unregister_page_pool(_: This, handle: usize) -> HRESULT {
    match page_pools().get_mut(handle.wrapping_sub(1)) {
        Some(pool @ Some(_)) => {
            *pool = None;
            S_OK
        }
        _ => E_INVALIDARG,
    }
}

/// Device method 0x428, one of the `ID3D12Device9` to `ID3D12Device11`
/// additions. From Grand Theft Auto VI's call: a resource at a virtual
/// address the title mapped itself, `(address, desc1, initial barrier
/// layout, clear value, iid, out)`, as `CreatePlacedResourceX` with
/// enhanced barriers. GPU addresses are CPU addresses, so the resource is
/// a view of that memory. `D3D12_RESOURCE_DESC1` starts like the plain
/// description; its sampler feedback region is not needed.
pub(crate) extern "system" fn create_at_address(
    _: This,
    address: u64,
    desc: *const ResourceDesc,
    layout: u32,
    clear: *const c_void,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(&desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    if address == 0 {
        return E_INVALIDARG;
    }
    tracing::debug!(
        address = format_args!("{address:#x}"),
        dimension = desc.dimension,
        width = desc.width,
        layout,
        "resource at an address"
    );
    const COMMON: u32 = 0;
    const DEFAULT: HeapProperties = HeapProperties {
        kind: 1,
        cpu_pages: 0,
        pool: 0,
        creation_nodes: 1,
        visible_nodes: 1,
    };
    hand_out_resource(
        desc,
        DEFAULT,
        0,
        Backing::Address(address as *mut u8),
        COMMON,
        clear,
        iid,
        out,
    )
}

/// Textures the title placed in its own memory and views without a
/// resource object (device 0x230, Grand Theft Auto VI's streamed and
/// interface textures), by address and description: a resource at that
/// address, made the first time a draw reads one.
/// shortcut: never released (one host texture per distinct address and
/// description viewed); release them when the title unmaps or reuses the
/// memory once streaming grows VRAM use.
#[cfg(windows)]
static PLACED_TEXTURES: std::sync::Mutex<std::collections::BTreeMap<(u64, [u32; 14]), usize>> =
    std::sync::Mutex::new(std::collections::BTreeMap::new());

/// The resource for a texture placed at `address` with the resource
/// description `desc` (as words), made on first use.
#[cfg(windows)]
pub(crate) fn texture_at(address: u64, desc: &[u32; 14]) -> Option<This> {
    let mut map = PLACED_TEXTURES.lock().unwrap_or_else(|e| e.into_inner());
    // Words 1 and 13 are padding (stack bytes in the title's records).
    let mut desc = *desc;
    (desc[1], desc[13]) = (0, 0);
    let desc = &desc;
    // A resource whose memory's host texture was replaced since (one with
    // more uses) is made again, sharing the new one.
    // shortcut: the old one is kept (view heap slots made from it view its
    // host texture until the title rewrites them).
    if let Some(&r) = map.get(&(address, *desc))
        && current(r as This)
    {
        return Some(r as This);
    }
    // SAFETY: 56 bytes in the description's layout.
    let d = unsafe { desc.as_ptr().cast::<ResourceDesc>().read_unaligned() };
    if d.dimension == BUFFER {
        return None;
    }
    let mut out = std::ptr::null();
    let made = create_at_address(
        std::ptr::null(),
        address,
        &d,
        0,
        std::ptr::null(),
        &RESOURCE_IIDS[0],
        &mut out,
    );
    if made != S_OK {
        return None;
    }
    map.insert((address, *desc), out as usize);
    resource(out).host.is_some().then_some(out)
}

pub(crate) extern "system" fn create_placed(
    _: This,
    heap: This,
    offset: u64,
    desc: *const ResourceDesc,
    state: u32,
    clear: *const c_void,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(&desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    if heap.is_null() {
        return E_INVALIDARG;
    }
    // SAFETY: the title passes one of our heaps.
    let h = unsafe { &*heap.cast::<Heap>() };
    let fits = footprint(&desc).is_some_and(|info| {
        offset.is_multiple_of(info.alignment)
            && offset
                .checked_add(info.size)
                .is_some_and(|end| end <= h.desc.size)
    });
    if !fits {
        tracing::warn!(
            ?desc,
            offset,
            heap = h.desc.size,
            "placed resource does not fit"
        );
        return E_INVALIDARG;
    }
    object::add_ref(heap);
    let backing = Backing::Placed { heap, offset };
    hand_out_resource(
        desc,
        h.desc.properties,
        h.desc.flags,
        backing,
        state,
        clear,
        iid,
        out,
    )
}

extern "system" fn resource_query(this: This, iid: *const Iid, out: *mut This) -> HRESULT {
    object::answer(
        RESOURCE,
        iid,
        object::is(iid, &RESOURCE_IIDS).then_some(this),
        out,
    )
}
extern "system" fn resource_release(this: This) -> u32 {
    object::release_boxed::<Resource>(this)
}
/// Memory is always mapped (subresources are not told apart yet); a null
/// `out` only asks for residency.
extern "system" fn map(this: This, _sub: u32, _read: *const c_void, out: *mut *mut u8) -> HRESULT {
    if !out.is_null() {
        // SAFETY: checked non-null; the title passes room for a pointer.
        unsafe { out.write(resource(this).data()) };
    }
    S_OK
}
extern "system" fn unmap(_: This, _sub: u32, _written: *const c_void) {}
extern "system" fn resource_desc(this: This, out: *mut ResourceDesc) {
    if !out.is_null() {
        // SAFETY: checked non-null; the title passes room for a description.
        unsafe { out.write(resource(this).desc) };
    }
}
/// Device method `GetCustomHeapProperties`, returning its struct through a
/// hidden first argument: what a standard heap type is as a custom heap.
/// Memory is unified and cache-coherent (as the architecture query says),
/// so every type is in pool L0, default heaps are not CPU-visible and the
/// others are write-back.
pub(crate) extern "system" fn custom_heap_properties(
    out: *mut HeapProperties,
    _: This,
    nodes: u32,
    kind: u32,
) -> *mut HeapProperties {
    const CUSTOM: u32 = 4;
    const NOT_AVAILABLE: u32 = 1;
    const WRITE_BACK: u32 = 3;
    const L0: u32 = 1;
    let nodes = nodes.max(1);
    let cpu_pages = match kind {
        1 => NOT_AVAILABLE,
        2 | 3 => WRITE_BACK,
        _ => 0,
    };
    let properties = HeapProperties {
        kind: CUSTOM,
        cpu_pages,
        pool: if cpu_pages == 0 { 0 } else { L0 },
        creation_nodes: nodes,
        visible_nodes: nodes,
    };
    // SAFETY: the title passes room for the result (null is tolerated).
    if let Some(out) = unsafe { out.as_mut() } {
        *out = properties;
    }
    out
}

extern "system" fn gpu_address(this: This) -> u64 {
    let r = resource(this);
    let at = r.data() as u64;
    // The console's GPU addresses fit in 40 bits; its ray-tracing builder
    // refuses others (diagnostics: which memory breaks that).
    #[cfg(feature = "diagnostics")]
    if at >> 40 != 0 {
        static ONCE: [std::sync::Once; 2] = [std::sync::Once::new(), std::sync::Once::new()];
        let backing = match r.backing {
            Backing::Committed(ref m) => format!("committed, attributes {:?}", m.attributes),
            Backing::Placed { offset, .. } => format!("placed in a heap at {offset:#x}"),
            Backing::Address(_) => "at a title address".into(),
        };
        ONCE[usize::from(r.desc.dimension == BUFFER)].call_once(|| {
            tracing::warn!(address = format_args!("{at:#x}"), backing, desc = ?r.desc, "a GPU address above 40 bits")
        });
    }
    at
}
extern "system" fn heap_properties(
    this: This,
    properties: *mut HeapProperties,
    flags: *mut u32,
) -> HRESULT {
    let r = resource(this);
    // SAFETY: either pointer may be null; otherwise the title passes room.
    unsafe {
        if let Some(p) = properties.as_mut() {
            *p = r.heap;
        }
        if let Some(f) = flags.as_mut() {
            *f = r.heap_flags;
        }
    }
    S_OK
}

/// IUnknown 3, object 4, device child 1, heap 1.
static HEAP_FUNCTIONS: Table<24> = {
    let mut f = object::d3d12_object(object::missing_table::<HEAP, 24>());
    f[0] = heap_query as *const ();
    f[2] = heap_release as *const ();
    f[7] = object::parent_device as *const (); // 0x38
    f[8] = heap_desc as *const (); // 0x40
    Table(f)
};

/// IUnknown 3, object 4, device child 1, resource 10 (7 as on PC, then 3
/// console ones).
static RESOURCE_FUNCTIONS: Table<32> = {
    let mut f = object::d3d12_object(object::missing_table::<RESOURCE, 32>());
    f[0] = resource_query as *const ();
    f[2] = resource_release as *const ();
    f[7] = object::parent_device as *const (); // 0x38
    f[8] = map as *const (); // 0x40
    f[9] = unmap as *const (); // 0x48
    f[10] = resource_desc as *const (); // 0x50
    f[11] = gpu_address as *const (); // 0x58
    f[14] = heap_properties as *const (); // 0x70
    Table(f)
};

#[cfg(test)]
pub(crate) fn test_buffer(width: u64) -> This {
    let heap = HeapProperties {
        kind: 1,
        cpu_pages: 0,
        pool: 0,
        creation_nodes: 0,
        visible_nodes: 0,
    };
    let desc = ResourceDesc {
        dimension: BUFFER,
        alignment: 0,
        width,
        height: 1,
        depth_or_array_size: 1,
        mip_levels: 1,
        format: 0,
        sample_count: 1,
        sample_quality: 0,
        layout: 1,
        flags: 0,
    };
    let mut out = std::ptr::null();
    let hr = create_committed(
        std::ptr::null(),
        &heap,
        0,
        &desc,
        0,
        std::ptr::null(),
        &RESOURCE_IIDS[0],
        &mut out,
    );
    assert_eq!(hr, S_OK);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::object::slot;

    #[test]
    fn packed_light_substitution_is_opt_in() {
        // The switch is read from the environment once, and the test suite
        // does not set it: the title's formats must be kept by default.
        assert!(!light_float_format());
        assert_eq!(light_effective_format(), LIGHT_NATIVE);
        assert_eq!(target_format(67), 67);
        assert_eq!(target_format(42), 42);
        assert_eq!(stand_in_view_format(42, 67), 67);
        assert_eq!(stand_in_view_format(10, 9), 9);
        assert_eq!(default_shader_format(9), None);
    }

    #[test]
    fn light_format_table_maps_only_packed_light_targets() {
        // Pure tables, independent of the cached environment decision. Only
        // short synthetic format numbers appear here by repo policy.
        assert_eq!(light_format_for(false), LIGHT_NATIVE);
        assert_eq!(light_format_for(true), LIGHT_FLOAT);
        assert_eq!(LIGHT_TARGETS, [67, 42]);
        // Native is byte-identical: every title format and view is kept.
        for format in [7, 9, 10, 42, 67, 68] {
            assert_eq!(target_format_for(format, LIGHT_NATIVE), format);
            assert_eq!(stand_in_view_format_for(LIGHT_NATIVE, 10, format), format);
        }
        assert_eq!(default_shader_format_for(LIGHT_NATIVE, 9), None);
        // Float maps only the two packed-light targets, nothing else.
        assert_eq!(target_format_for(42, LIGHT_FLOAT), LIGHT_FLOAT);
        assert_eq!(target_format_for(67, LIGHT_FLOAT), LIGHT_FLOAT);
        for format in [7, 9, 10, 68] {
            assert_eq!(target_format_for(format, LIGHT_FLOAT), format);
        }
        // Views of the stand-in and typeless 16-bit textures name RGBA16F.
        assert_eq!(stand_in_view_format_for(LIGHT_FLOAT, 10, 9), LIGHT_FLOAT);
        assert_eq!(stand_in_view_format_for(LIGHT_FLOAT, 10, 42), LIGHT_FLOAT);
        assert_eq!(stand_in_view_format_for(LIGHT_FLOAT, 10, 67), LIGHT_FLOAT);
        assert_eq!(stand_in_view_format_for(LIGHT_FLOAT, 9, 9), LIGHT_FLOAT);
        assert_eq!(stand_in_view_format_for(LIGHT_FLOAT, 7, 7), 7);
        assert_eq!(default_shader_format_for(LIGHT_FLOAT, 9), Some(LIGHT_FLOAT));
        assert_eq!(default_shader_format_for(LIGHT_FLOAT, 42), None);
        // The written image's creation format is what the gate checks.
        assert_eq!(target_format_for(42, LIGHT_FLOAT), light_format_for(true));
        assert_eq!(target_format_for(42, LIGHT_NATIVE), light_format_for(false));
    }

    #[test]
    fn texture_cap_drops_only_the_levels_above_the_cap() {
        // 2048x2048, 12 mips; a 1024 cap drops one level and quarters the
        // allocation, but never below a 256-pixel side.
        assert_eq!(mip_drop_for(2048, 2048, 12, 1 << 26, Some(1024), None), 1);
        assert_eq!(mip_drop_for(2048, 2048, 12, 1 << 26, Some(512), None), 2);
        assert_eq!(mip_drop_for(2048, 2048, 12, 1 << 26, Some(256), None), 3);
        // A cap bigger than the texture changes nothing.
        assert_eq!(mip_drop_for(1024, 1024, 11, 1 << 24, Some(2048), None), 0);
        // No cap at all: no drop.
        assert_eq!(mip_drop_for(4096, 4096, 13, 1 << 28, None, None), 0);
    }

    #[test]
    fn texture_byte_budget_also_drops_levels() {
        // 64 MiB budget on a 2048 texture (~64 MiB at mip 0 with mips) needs
        // one level dropped; the pixel cap and the byte budget combine.
        let budget = 64 << 20;
        assert_eq!(mip_drop_for(2048, 2048, 12, 1 << 27, None, Some(budget)), 1);
        assert_eq!(mip_drop_for(4096, 4096, 13, 1 << 28, Some(1024), Some(budget)), 2);
    }

    #[test]
    fn host_subresource_of_a_dropped_mip_is_none() {
        assert_eq!(host_subresource(0, 3), Some(3));
        assert_eq!(host_subresource(2, 3), Some(1));
        assert_eq!(host_subresource(2, 1), None);
        assert_eq!(host_subresource(2, 0), None);
    }

    #[test]
    fn a8_unorm_is_not_in_the_r8_typeless_family() {
        // DXGI_FORMAT values: 60 R8_TYPELESS, 61 R8_UNORM, 62 R8_UINT,
        // 63 R8_SNORM, 64 R8_SINT, 65 A8_UNORM. The R8 typeless family is
        // 60..=64; A8_UNORM has no typeless family and cannot be cast to R8,
        // so it must stand alone rather than fold into R8_TYPELESS.
        assert_eq!(family(60), 60);
        for f in 61..=64 {
            assert_eq!(family(f), 60);
        }
        assert_eq!(family(65), 65);
        // Boundaries: 59 belongs to the preceding family, 66 is unmapped.
        assert_eq!(family(59), 53);
        assert_eq!(family(66), 66);
    }

    /// The console-only formats have element sizes and texture layouts; no
    /// existing mapping is changed, and they are left without a host stand-in
    /// (a PC format of another family would make their views fail `fits`).
    #[test]
    fn console_only_formats_have_sizes_and_layouts() {
        for &(format, bytes) in &[(189, 4), (190, 1), (191, 4)] {
            assert_eq!(element(format), Some((bytes, 1)), "format {format}");
            let desc = texture(64, 64, format, 1);
            assert!(texture_layout(&desc).is_some(), "format {format}");
            assert!(footprint(&desc).is_some_and(|i| i.size > 0), "format {format}");
        }
    }

    type Release = extern "system" fn(This) -> u32;
    type Map = extern "system" fn(This, u32, *const c_void, *mut *mut u8) -> HRESULT;
    type Address = extern "system" fn(This) -> u64;

    const PROPERTIES: HeapProperties = HeapProperties {
        kind: 1,
        cpu_pages: 0,
        pool: 0,
        creation_nodes: 0,
        visible_nodes: 0,
    };

    fn buffer(width: u64) -> ResourceDesc {
        ResourceDesc {
            dimension: BUFFER,
            alignment: 0,
            width,
            height: 1,
            depth_or_array_size: 1,
            mip_levels: 1,
            format: 0,
            sample_count: 1,
            sample_quality: 0,
            layout: 1,
            flags: 0,
        }
    }

    fn texture(width: u64, height: u32, format: u32, mips: u16) -> ResourceDesc {
        ResourceDesc {
            dimension: 3,
            width,
            height,
            format,
            mip_levels: mips,
            layout: 0,
            ..buffer(0)
        }
    }

    fn heap(size: u64) -> This {
        let desc = HeapDesc {
            size,
            properties: PROPERTIES,
            alignment: 0,
            flags: 0,
        };
        let mut heap = std::ptr::null();
        assert_eq!(
            create_heap(std::ptr::null(), &desc, &HEAP_IIDS[0], &mut heap),
            S_OK
        );
        heap
    }

    /// Title textures of one memory share a key in any format of the same
    /// element; another address, shape, element size or a depth texture is
    /// another memory. The library's own layout pick resolves as spelled.
    #[test]
    fn one_memory_one_key_in_any_format_of_its_element() {
        let a = ResourceDesc { flags: 4, ..texture(192, 108, 42, 1) };
        let b = ResourceDesc { format: 67, flags: 0, ..a };
        let key = memory_key(0x1_0000, &a);
        assert!(key.is_some());
        assert_eq!(key, memory_key(0x1_0000, &b));
        assert_eq!(key, memory_key(0x1_0000, &ResourceDesc { layout: 0x105, ..b }));
        assert_ne!(key, memory_key(0x2_0000, &a));
        assert_ne!(key, memory_key(0x1_0000, &ResourceDesc { width: 96, ..a }));
        assert_ne!(key, memory_key(0x1_0000, &ResourceDesc { format: 10, ..a }));
        assert_ne!(key, memory_key(0x1_0000, &ResourceDesc { layout: 0x102, ..a }));
        assert_ne!(key, memory_key(0x1_0000, &ResourceDesc { flags: 2, format: 40, ..a }));
        assert_eq!(memory_key(0x1_0000, &buffer(256)), None);
    }

    /// Castable formats: every format of the same element but typeless and
    /// depth ones; block formats among block formats of their block size.
    #[test]
    fn castable_formats_share_the_element() {
        let c = castable(42);
        for f in [67, 28, 29, 41, 24, 26, 87, 35] {
            assert!(c.contains(&f), "{f}");
        }
        for f in [42, 39, 40, 45, 27, 10, 56] {
            assert!(!c.contains(&f), "{f}");
        }
        let bc1 = castable(71);
        assert!(bc1.contains(&72) && bc1.contains(&80) && bc1.contains(&81));
        assert!(!bc1.contains(&70) && !bc1.contains(&98) && !bc1.contains(&17));
        assert!(castable(98).contains(&95) && castable(98).contains(&77));
        assert!(castable(53).contains(&56) && !castable(53).contains(&55));
    }

    #[test]
    fn heaps_are_zeroed_aligned_memory() {
        let heap = heap(1 << 20);
        // SAFETY: a heap we just made.
        let memory = unsafe { &(*heap.cast::<Heap>()).memory };
        assert_eq!(memory.ptr.as_ptr() as u64 % KIB_64, 0);
        let mut back = std::mem::MaybeUninit::<HeapDesc>::uninit();
        slot::<extern "system" fn(This, *mut HeapDesc)>(heap, 0x40)(heap, back.as_mut_ptr());
        // SAFETY: written above.
        assert_eq!(unsafe { back.assume_init() }.size, 1 << 20);
        assert_eq!(slot::<Release>(heap, 0x10)(heap), 0);

        let bad = HeapDesc {
            size: 1 << 20,
            properties: PROPERTIES,
            alignment: 3,
            flags: 0,
        };
        let mut out = std::ptr::null();
        assert_eq!(
            create_heap(std::ptr::null(), &bad, &HEAP_IIDS[0], &mut out),
            E_INVALIDARG
        );
    }

    #[test]
    fn device_setup_heap_properties_and_resources_at_addresses() {
        let device = crate::device::device();
        // GetCustomHeapProperties (0xd0): the struct comes back through the
        // hidden first argument, which is also returned.
        type Custom =
            extern "system" fn(*mut HeapProperties, This, u32, u32) -> *mut HeapProperties;
        let mut p = PROPERTIES;
        let back = slot::<Custom>(device, 0xd0)(&mut p, device, 1, 2);
        assert_eq!(back, &raw mut p);
        assert_eq!(
            (
                p.kind,
                p.cpu_pages,
                p.pool,
                p.creation_nodes,
                p.visible_nodes
            ),
            (4, 3, 1, 1, 1),
            "upload: custom, write-back, L0"
        );
        slot::<Custom>(device, 0xd0)(&mut p, device, 1, 1);
        assert_eq!(p.cpu_pages, 1, "default: not CPU-visible");

        // 0x428: a buffer over memory the title owns, as GTA VI asks.
        type At = extern "system" fn(
            This,
            u64,
            *const ResourceDesc,
            u32,
            *const c_void,
            *const Iid,
            *mut This,
        ) -> HRESULT;
        let memory = vec![0u8; 0x1_0000];
        let address = memory.as_ptr() as u64;
        let mut r = std::ptr::null();
        let create = slot::<At>(device, 0x428);
        let desc = buffer(0x1_0000);
        let null = std::ptr::null();
        assert_eq!(
            create(
                device,
                address,
                &desc,
                u32::MAX,
                null,
                &RESOURCE_IIDS[0],
                &mut r
            ),
            0
        );
        assert_eq!(slot::<Address>(r, 0x58)(r), address);
        assert_eq!(slot::<Release>(r, 0x10)(r), 0);
        // The title's memory is still there after the release.
        assert_eq!(memory[0xffff], 0);
        assert_ne!(
            create(device, 0, &desc, u32::MAX, null, &RESOURCE_IIDS[0], &mut r),
            0
        );
    }

    #[test]
    fn sizes_cover_every_mip() {
        let size = |d: &ResourceDesc| footprint(d).map(|i| i.size);
        assert_eq!(size(&buffer(256)), Some(KIB_64));
        // Textures take their console layout and its alignment (4 KiB
        // standard swizzle when the description leaves it to the library):
        // 512x512 BC7 with 10 mips is 0x56000 bytes, 4 KiB aligned, as on
        // the console.
        let layout = |d: &ResourceDesc| texture_layout(d).unwrap().size;
        let rgba = texture(256, 256, 28, 0);
        assert_eq!(size(&rgba), Some(layout(&rgba).next_multiple_of(4096)));
        let bc7 = footprint(&texture(512, 512, 98, 10));
        let console = AllocationInfo {
            size: 0x56000,
            alignment: 0x1000,
        };
        assert_eq!(bc7, Some(console));
        assert!(layout(&rgba) >= 4 * (256 * 256 * 4) / 3, "every mip");
        // BC1 is 8 bytes per 4x4 block: 1024x1024, one mip = 512 KiB.
        assert_eq!(size(&texture(1024, 1024, 71, 1)), Some(512 << 10));
        assert_eq!(size(&texture(16, 16, 66, 1)), None, "1-bit format");
        // Linear layout (1) and an explicit mode (64 KiB display, 0x10a).
        let linear = ResourceDesc {
            layout: 1,
            ..texture(100, 60, 28, 1)
        };
        assert_eq!(
            texture_layout(&linear).unwrap().planes[0]
                .layout
                .desc
                .swizzle
                .0,
            0
        );
        let display = ResourceDesc {
            layout: 0x10a,
            ..texture(100, 60, 28, 1)
        };
        assert_eq!(
            texture_layout(&display).unwrap().planes[0]
                .layout
                .desc
                .swizzle
                .0,
            10
        );

        let mut info = AllocationInfo {
            size: 0,
            alignment: 0,
        };
        let descs = [buffer(100), texture(256, 256, 28, 0)];
        allocation_info(&mut info, std::ptr::null(), 0, 2, descs.as_ptr());
        assert_eq!(
            info,
            AllocationInfo {
                size: KIB_64 + size(&descs[1]).unwrap(),
                alignment: KIB_64
            }
        );
        allocation_info(&mut info, std::ptr::null(), 0, 1, &texture(1, 1, 999, 1));
        assert_eq!(info.size, u64::MAX);
    }

    #[test]
    fn upload_footprints_follow_the_pc_rules() {
        let null = std::ptr::null();
        let mut layouts = [NO_FOOTPRINT; 2];
        let (mut rows, mut sizes, mut total) = ([0; 2], [0; 2], 0);
        let mut footprints = |desc: &ResourceDesc, first, count| {
            let (l, r, s) = (layouts.as_mut_ptr(), rows.as_mut_ptr(), sizes.as_mut_ptr());
            copyable_footprints(null, desc, first, count, 0, l, r, s, &mut total);
            (layouts, rows, sizes, total)
        };
        // 100x60 RGBA8, 2 mips: 400-byte rows pitched to 512.
        let (l, r, s, t) = footprints(&texture(100, 60, 28, 2), 0, 2);
        assert_eq!((l[0].row_pitch, r[0], s[0]), (512, 60, 400));
        // Mip 1 (50x30, 200-byte rows) starts after 59 pitched rows and one
        // packed row, rounded up to 512.
        assert_eq!(l[1].offset, (512 * 59 + 400u64).next_multiple_of(512));
        assert_eq!((l[1].width, l[1].height, r[1]), (50, 30, 30));
        assert_eq!(t, l[1].offset + 256 * 29 + 200);
        // BC1: rows of 4x4 blocks, 8 bytes each.
        let (_, r, s, _) = footprints(&texture(64, 64, 71, 1), 0, 1);
        assert_eq!((r[0], s[0]), (16, 128));
        let (_, _, _, t) = footprints(&texture(64, 64, 71, 1), 1, 1);
        assert_eq!(t, u64::MAX, "no second subresource");
    }

    #[test]
    fn depth_stencil_footprints_follow_the_pc_plane_order() {
        // Native D3D12 reports the same layout for both depth/stencil families
        // and their view formats. Mips vary fastest, then array slices, then planes.
        let expected = [
            (512, 39, 16, 8),
            (4_608, 39, 8, 4),
            (6_656, 39, 16, 8),
            (10_752, 39, 8, 4),
            (12_800, 60, 16, 8),
            (16_896, 60, 8, 4),
            (18_944, 60, 16, 8),
            (23_040, 60, 8, 4),
        ];
        for format in (19..=22).chain(44..=47) {
            let desc = ResourceDesc {
                depth_or_array_size: 2,
                ..texture(16, 8, format, 2)
            };
            let mut layouts = [NO_FOOTPRINT; 8];
            let (mut rows, mut sizes, mut total) = ([0; 8], [0; 8], 0);
            copyable_footprints(
                std::ptr::null(),
                &desc,
                0,
                8,
                512,
                layouts.as_mut_ptr(),
                rows.as_mut_ptr(),
                sizes.as_mut_ptr(),
                &mut total,
            );

            assert_eq!(
                layouts.map(|l| (l.offset, l.format, l.width, l.height)),
                expected,
                "format {format}"
            );
            assert!(
                layouts.iter().all(|l| l.depth == 1 && l.row_pitch == 512),
                "format {format}"
            );
            assert_eq!(rows, [8, 4, 8, 4, 8, 4, 8, 4], "format {format}");
            assert_eq!(sizes, [64, 32, 64, 32, 16, 8, 16, 8], "format {format}");
            assert_eq!(total, 24_072, "format {format}");
            assert!(subresource(&desc, 8).is_none(), "format {format}");
        }
    }

    /// A 4x4 texture filled from a title buffer through our list and queue,
    /// then read back from the host GPU.
    #[cfg(windows)]
    #[test]
    fn buffer_data_reaches_the_host_texture() {
        use windows::Win32::Graphics::Direct3D12::*;
        let Some(host) = crate::host::host() else {
            return; // no host GPU here
        };
        let null = std::ptr::null();
        let iid = &RESOURCE_IIDS[0];
        const COPY_DEST: u32 = 0x400;
        let (mut image, mut source) = (null, null);
        let rgba = texture(4, 4, 28, 1);
        assert_eq!(
            create_committed(
                null,
                &PROPERTIES,
                0,
                &rgba,
                COPY_DEST,
                null.cast(),
                iid,
                &mut image
            ),
            S_OK
        );
        assert_eq!(
            create_committed(
                null,
                &PROPERTIES,
                0,
                &buffer(1024),
                0,
                null.cast(),
                iid,
                &mut source
            ),
            S_OK
        );
        let (data, _) = buffer_bytes(source).unwrap();
        // SAFETY: the buffer holds 1024 bytes.
        let bytes = unsafe { std::slice::from_raw_parts_mut(data, 1024) };
        for (i, b) in bytes.iter_mut().enumerate() {
            *b = (i % 251) as u8;
        }

        let queue_iid = guid(
            0x0ec870a6,
            0x5d7e,
            0x4c22,
            [0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed],
        );
        let allocator_iid = guid(
            0x6102dee4,
            0xaf59,
            0x4b09,
            [0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24],
        );
        let list_iid = guid(
            0x5b160d0f,
            0xac1b,
            0x4185,
            [0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55],
        );
        let (mut queue, mut allocator, mut list) = (null, null, null);
        let queue_desc = [0u32; 4];
        assert_eq!(
            crate::queue::create(null, queue_desc.as_ptr().cast(), &queue_iid, &mut queue),
            S_OK
        );
        assert_eq!(
            crate::list::create_allocator(null, 0, &allocator_iid, &mut allocator),
            S_OK
        );
        assert_eq!(
            crate::list::create_list(null, 0, 0, allocator, null, &list_iid, &mut list),
            S_OK
        );

        #[repr(C)]
        struct Location {
            resource: This,
            kind: u32,
            footprint: PlacedFootprint,
        }
        let rows = PlacedFootprint {
            offset: 0,
            format: 28,
            width: 4,
            height: 4,
            depth: 1,
            row_pitch: 256,
        };
        let to = Location {
            resource: image,
            kind: 0,
            footprint: PlacedFootprint { offset: 0, ..rows },
        };
        let from = Location {
            resource: source,
            kind: 1,
            footprint: rows,
        };
        type Copy =
            extern "system" fn(This, *const Location, u32, u32, u32, *const Location, *const u8);
        slot::<Copy>(list, 0x80)(list, &to, 0, 0, 0, &from, null.cast());
        slot::<extern "system" fn(This) -> HRESULT>(list, 0x48)(list);
        slot::<extern "system" fn(This, u32, *const This)>(queue, 0x50)(queue, 1, &list);

        // Read the host texture back into a readback buffer.
        let readback_heap = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_READBACK,
            ..Default::default()
        };
        let readback_desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Width: 1024,
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            SampleDesc: windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            ..Default::default()
        };
        let mut readback: Option<ID3D12Resource> = None;
        // SAFETY: plain creation of a readback buffer.
        unsafe {
            host.device
                .CreateCommittedResource(
                    &readback_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &readback_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    None,
                    &mut readback,
                )
                .unwrap();
        }
        let readback = readback.unwrap();
        let host_image = host_resource(image).unwrap();
        let back = host.new_list().unwrap();
        let (_, before) = host_transition(image, 0x800).unwrap();
        // The upload list holds the texture until it is reset.
        assert!(std::sync::Arc::strong_count(&host_copy(image).unwrap()) > 2);
        use windows::core::Interface;
        let out_to = Location {
            resource: readback.as_raw().cast_const().cast(),
            kind: 1,
            footprint: rows,
        };
        let out_from = Location {
            resource: host_image.as_raw().cast_const().cast(),
            kind: 0,
            footprint: to.footprint,
        };
        // SAFETY: an open list, live resources, locations in the host layout.
        unsafe {
            back.list.ResourceBarrier(&[crate::host::transition(
                host_image,
                D3D12_RESOURCE_STATES(before as i32),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
            )]);
            back.list.CopyTextureRegion(
                (&raw const out_to).cast(),
                0,
                0,
                0,
                (&raw const out_from).cast(),
                None,
            );
        }
        back.close();
        host.execute(&[&back]);
        let mut mapped = std::ptr::null_mut();
        // SAFETY: the copy finished (execute waits); the buffer is 1024 bytes.
        let read = unsafe {
            readback
                .Map(
                    0,
                    Some(&D3D12_RANGE {
                        Begin: 0,
                        End: 1024,
                    }),
                    Some(&mut mapped),
                )
                .unwrap();
            std::slice::from_raw_parts(mapped.cast::<u8>(), 1024)
        };
        for row in 0..4 {
            let at = row * 256;
            assert_eq!(read[at..at + 16], bytes[at..at + 16], "row {row}");
        }
    }

    #[test]
    fn committed_and_placed_resources_map_at_their_gpu_address() {
        let null = std::ptr::null();
        let mut committed = null;
        let hr = create_committed(
            null,
            &PROPERTIES,
            0,
            &buffer(256),
            0,
            null.cast(),
            &RESOURCE_IIDS[0],
            &mut committed,
        );
        assert_eq!(hr, S_OK);
        let mut data = std::ptr::null_mut();
        assert_eq!(
            slot::<Map>(committed, 0x40)(committed, 0, null.cast(), &mut data),
            S_OK
        );
        assert_eq!(slot::<Address>(committed, 0x58)(committed), data as u64);
        // SAFETY: the buffer is at least 256 zeroed bytes.
        assert_eq!(unsafe { *data.add(255) }, 0);
        assert_eq!(slot::<Release>(committed, 0x10)(committed), 0);

        let heap = heap(4 * KIB_64);
        let place = |offset, desc: &ResourceDesc, out: &mut This| {
            create_placed(
                null,
                heap,
                offset,
                desc,
                0,
                null.cast(),
                &RESOURCE_IIDS[0],
                out,
            )
        };
        let mut placed = null;
        assert_eq!(place(KIB_64, &buffer(KIB_64), &mut placed), S_OK);
        // SAFETY: a heap we made.
        let base = unsafe { (*heap.cast::<Heap>()).memory.ptr.as_ptr() } as u64;
        assert_eq!(slot::<Address>(placed, 0x58)(placed), base + KIB_64);
        let mut other = null;
        assert_eq!(
            place(4 * KIB_64, &buffer(1), &mut other),
            E_INVALIDARG,
            "past the end"
        );
        assert_eq!(
            place(100, &buffer(1), &mut other),
            E_INVALIDARG,
            "misaligned"
        );
        // The resource keeps the heap alive.
        assert_eq!(slot::<Release>(heap, 0x10)(heap), 1);
        assert_eq!(slot::<Release>(placed, 0x10)(placed), 0);
    }
}
