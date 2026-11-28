#!/usr/bin/env python3
"""Train and evaluate the v2 3-layer 8->16->16->7 residual CNN with RGB disocclusion repair.

Matches `mac_mirror_viewer/win_ui/d3d_frame_interpolator.cpp` (v2 multi-scale 1/8->1/4->1/2
coarse-to-fine pyramid + 1/2-res 3-layer residual CNN + full-res RGB synthesis).

Usage:
  uv run --with torch --with numpy python3 test/train_neural_flow.py
  uv run --with torch --with numpy python3 test/train_neural_flow.py --eval-only
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys
import time
from typing import Dict, Tuple

import numpy as np

try:
    import torch
    import torch.nn as nn
    import torch.nn.functional as F
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "PyTorch is required. Run via: "
        "uv run --with torch --with numpy python3 test/train_neural_flow.py"
    ) from exc


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_HEADER_PATH = REPO_ROOT / "mac_mirror_viewer" / "win_ui" / "neural_flow_weights.h"

MODEL_VERSION = 2
IN_CHANNELS = 8
HIDDEN_CHANNELS = 16
OUT_CHANNELS = 7
FLOW_NORM_SCALE = 24.0
RESIDUAL_FLOW_SCALE = 6.0
RGB_RESIDUAL_SCALE = 0.25


def select_device(requested: str = "auto") -> torch.device:
    if requested != "auto":
        return torch.device(requested)
    # Deterministic vectorized CPU runs 60 epochs of this 4,503-param ResNet in ~60s
    # and avoids Metal/MPS command-buffer stalls across hosts.
    return torch.device("cpu")


def rgb_to_luma(rgb: torch.Tensor) -> torch.Tensor:
    """Rec.709 luma matching D3D11 shader `dot(rgb, float3(0.2126, 0.7152, 0.0722))`."""
    return (
        0.2126 * rgb[:, 0:1] + 0.7152 * rgb[:, 1:2] + 0.0722 * rgb[:, 2:3]
    ).clamp(0.0, 1.0)


_GRID_CACHE: Dict[Tuple[int, int, str, torch.dtype], Tuple[torch.Tensor, torch.Tensor]] = {}


def bilinear_warp(img: torch.Tensor, flow_xy: torch.Tensor) -> torch.Tensor:
    """Sample `img` (B, C, H, W) at pixel coordinates `p + flow_xy` (clamped border).

    Uses `F.grid_sample(..., align_corners=True, padding_mode='border')`, matching D3D11
    clamp-border bilinear texture sampling to < 4e-6 with fast Metal/MPS/CUDA/CPU gradients.
    """
    _, _, height, width = img.shape
    key = (height, width, str(img.device), img.dtype)
    if key not in _GRID_CACHE:
        yy, xx = torch.meshgrid(
            torch.arange(height, device=img.device, dtype=img.dtype),
            torch.arange(width, device=img.device, dtype=img.dtype),
            indexing="ij",
        )
        _GRID_CACHE[key] = (xx.unsqueeze(0), yy.unsqueeze(0))
    xx, yy = _GRID_CACHE[key]

    gx = ((xx + flow_xy[:, 0]).clamp(0.0, float(width - 1)) / float(max(1, width - 1))) * 2.0 - 1.0
    gy = ((yy + flow_xy[:, 1]).clamp(0.0, float(height - 1)) / float(max(1, height - 1))) * 2.0 - 1.0
    grid = torch.stack([gx, gy], dim=-1)
    return F.grid_sample(img, grid, mode="bilinear", padding_mode="border", align_corners=True)


def compute_spatial_grads(luma: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Compute central-difference `(dx, dy, grad_mag)` on (B, 1, H, W) matching HLSL."""
    padded = F.pad(luma, (1, 1, 1, 1), mode="replicate")
    dx = 0.5 * (padded[:, :, 1:-1, 2:] - padded[:, :, 1:-1, :-2])
    dy = 0.5 * (padded[:, :, 2:, 1:-1] - padded[:, :, :-2, 1:-1])
    grad_mag = (torch.sqrt(dx * dx + dy * dy + 1e-12) * 4.0).clamp(0.0, 1.0)
    return dx, dy, grad_mag


def compute_grad_mag(luma: torch.Tensor) -> torch.Tensor:
    """Compute `clamp(hypot(dI/dx, dI/dy) * 4.0, 0.0, 1.0)` on (B, 1, H, W)."""
    return compute_spatial_grads(luma)[2]


# Syntax-highlighting palettes (RGB in [0, 1]) resembling macOS Xcode / VS Code themes.
DARK_SYNTAX_COLORS = np.array(
    [
        [0.86, 0.88, 0.92],  # crisp identifier
        [0.78, 0.46, 0.86],  # keyword purple
        [0.36, 0.72, 0.96],  # function blue
        [0.92, 0.52, 0.36],  # string coral
        [0.42, 0.82, 0.64],  # type teal
        [0.90, 0.80, 0.42],  # constant amber
        [0.48, 0.54, 0.62],  # comment slate
    ],
    dtype=np.float32,
)

LIGHT_SYNTAX_COLORS = np.array(
    [
        [0.12, 0.14, 0.18],  # crisp identifier
        [0.58, 0.12, 0.62],  # keyword purple
        [0.08, 0.38, 0.74],  # function blue
        [0.76, 0.18, 0.14],  # string red
        [0.08, 0.48, 0.34],  # type green
        [0.64, 0.36, 0.06],  # constant ochre
        [0.42, 0.48, 0.54],  # comment gray
    ],
    dtype=np.float32,
)


def _render_rgb_code_panel(
    rng: np.random.Generator,
    tex_h: int,
    tex_w: int,
    dark_mode: bool,
    line_step: int = 12,
) -> np.ndarray:
    """Render a color syntax-highlighted Retina code editor panel `(H, W, 3)` in [0, 1]."""
    if dark_mode:
        bg = np.array(
            [rng.uniform(0.08, 0.13), rng.uniform(0.09, 0.14), rng.uniform(0.11, 0.18)],
            dtype=np.float32,
        )
        palette = DARK_SYNTAX_COLORS
    else:
        bg = np.array(
            [rng.uniform(0.92, 0.97), rng.uniform(0.93, 0.98), rng.uniform(0.95, 0.99)],
            dtype=np.float32,
        )
        palette = LIGHT_SYNTAX_COLORS

    gy = np.linspace(-0.03, 0.03, tex_h, dtype=np.float32)[:, None, None]
    gx = np.linspace(-0.02, 0.02, tex_w, dtype=np.float32)[None, :, None]
    canvas = np.clip(bg[None, None, :] + gy + gx, 0.0, 1.0).astype(np.float32)

    # Editor gutter + divider line
    gutter_w = int(rng.integers(20, 34))
    gutter_bg = np.clip(bg + (0.04 if dark_mode else -0.045), 0.0, 1.0)
    canvas[:, :gutter_w] = gutter_bg
    canvas[:, gutter_w : min(tex_w, gutter_w + 2)] = np.clip(
        bg + (0.14 if dark_mode else -0.15), 0.0, 1.0
    )

    # Colored diff/section callout bands for strong macro-scale alignment cues
    for _ in range(2):
        band_y = int(rng.integers(max(10, tex_h // 8), max(14, (tex_h * 7) // 8)))
        band_h = min(int(rng.integers(14, 28)), max(6, tex_h - band_y))
        tint = (
            np.array([rng.uniform(0.04, 0.12), rng.uniform(0.04, 0.12), rng.uniform(0.06, 0.16)], dtype=np.float32)
            if dark_mode
            else np.array([-rng.uniform(0.05, 0.12), -rng.uniform(0.04, 0.10), -rng.uniform(0.02, 0.08)], dtype=np.float32)
        )
        canvas[band_y : band_y + band_h, gutter_w + 2 :] = np.clip(
            canvas[band_y : band_y + band_h, gutter_w + 2 :] + tint, 0.0, 1.0
        )

    # Crisp Retina colored syntax tokens
    for y in range(6, tex_h - 6, line_step):
        num_col = np.clip(bg + (0.30 if dark_mode else -0.35), 0.0, 1.0)
        num_x0 = max(3, gutter_w - int(rng.integers(10, 16)))
        canvas[y : min(tex_h, y + 3), num_x0 : max(num_x0 + 2, gutter_w - 5)] = num_col

        if rng.random() < 0.16:
            continue

        indent = gutter_w + 6 + int(rng.integers(0, 5)) * 12
        x = indent
        n_tokens = int(rng.integers(2, 7))
        for _ in range(n_tokens):
            tok_w = int(rng.integers(10, 42))
            if x + tok_w >= tex_w - 4:
                break
            col = palette[int(rng.integers(0, len(palette)))]
            col = np.clip(col + rng.uniform(-0.04, 0.04, size=3).astype(np.float32), 0.0, 1.0)
            stroke_h = int(rng.choice([3, 4]))
            canvas[y : min(tex_h, y + stroke_h), x : x + tok_w] = col
            pitch = int(rng.choice([4, 5, 6]))
            for gx_i in range(x + 2, x + tok_w - 1, pitch):
                canvas[y : min(tex_h, y + stroke_h), gx_i : min(tex_w, gx_i + 1)] = bg
            if rng.random() < 0.55 and y >= 2:
                asc_x = x + int(rng.integers(0, max(1, tok_w - 2)))
                canvas[y - 2 : min(tex_h, y + stroke_h), asc_x : min(tex_w, asc_x + 2)] = col
            x += tok_w + int(rng.integers(5, 12))

    # Crisp UI separator bars
    for _ in range(int(rng.integers(1, 3))):
        sep_y = int(rng.integers(14, max(15, tex_h - 14)))
        canvas[sep_y : sep_y + 2, gutter_w + 2 :] = np.clip(
            bg + (0.16 if dark_mode else -0.16), 0.0, 1.0
        )

    return canvas


def _render_rgb_ui_card_panel(
    rng: np.random.Generator,
    tex_h: int,
    tex_w: int,
    dark_mode: bool,
) -> np.ndarray:
    """Render a colorful macOS UI / dashboard / browser window with rich gradients & cards."""
    if dark_mode:
        c0 = np.array([rng.uniform(0.10, 0.17), rng.uniform(0.11, 0.19), rng.uniform(0.16, 0.26)], dtype=np.float32)
        c1 = np.array([rng.uniform(0.18, 0.28), rng.uniform(0.16, 0.24), rng.uniform(0.24, 0.36)], dtype=np.float32)
    else:
        c0 = np.array([rng.uniform(0.88, 0.96), rng.uniform(0.90, 0.97), rng.uniform(0.93, 0.99)], dtype=np.float32)
        c1 = np.array([rng.uniform(0.76, 0.88), rng.uniform(0.82, 0.92), rng.uniform(0.88, 0.96)], dtype=np.float32)

    gy = np.linspace(0.0, 1.0, tex_h, dtype=np.float32)[:, None, None]
    gx = np.linspace(0.0, 1.0, tex_w, dtype=np.float32)[None, :, None]
    canvas = (1.0 - 0.55 * gy - 0.45 * gx) * c0[None, None, :] + (0.55 * gy + 0.45 * gx) * c1[None, None, :]
    canvas = np.clip(canvas, 0.0, 1.0).astype(np.float32)

    side_w = max(20, int(tex_w * rng.uniform(0.20, 0.30)))
    sidebar_col = np.clip(
        c0 + (np.array([0.08, 0.06, 0.12], dtype=np.float32) if dark_mode else np.array([-0.12, -0.10, -0.06], dtype=np.float32)),
        0.0,
        1.0,
    )
    canvas[:, :side_w] = sidebar_col

    accents = np.array(
        [
            [0.20, 0.52, 0.94],
            [0.92, 0.34, 0.38],
            [0.22, 0.72, 0.50],
            [0.88, 0.58, 0.18],
            [0.62, 0.36, 0.88],
        ],
        dtype=np.float32,
    )
    for py in range(24, tex_h - 14, 16):
        pill = accents[(py // 16 + int(rng.integers(0, len(accents)))) % len(accents)]
        canvas[py : py + 8, 5 : side_w - 5] = 0.62 * pill + 0.38 * sidebar_col

    n_cards = int(rng.integers(2, 5))
    card_y = 22
    for c_idx in range(n_cards):
        ch = int(rng.integers(26, 48))
        if card_y + ch >= tex_h - 6:
            break
        cx0 = side_w + 8
        cx1 = tex_w - 8
        if cx1 - cx0 < 24:
            break
        card_bg = np.clip(c0 + (0.12 if dark_mode else 0.06), 0.0, 1.0)
        canvas[card_y : card_y + ch, cx0:cx1] = card_bg
        accent = accents[(c_idx + int(rng.integers(0, len(accents)))) % len(accents)]
        canvas[card_y : card_y + 10, cx0:cx1] = accent
        fg = np.array([0.88, 0.90, 0.94], dtype=np.float32) if dark_mode else np.array([0.14, 0.16, 0.20], dtype=np.float32)
        for ty in range(card_y + 14, card_y + ch - 4, 7):
            tw = int(rng.integers(16, max(18, (cx1 - cx0) - 10)))
            canvas[ty : ty + 3, cx0 + 6 : min(cx1 - 4, cx0 + 6 + tw)] = fg
        card_y += ch + 7

    return np.clip(canvas, 0.0, 1.0)


def _render_rgb_window_texture(
    rng: np.random.Generator, win_h: int, win_w: int, dark_mode: bool
) -> np.ndarray:
    if rng.random() < 0.45:
        tex = _render_rgb_code_panel(rng, win_h, win_w, dark_mode=dark_mode, line_step=12)
    else:
        tex = _render_rgb_ui_card_panel(rng, win_h, win_w, dark_mode=dark_mode)

    title_h = min(20, max(14, win_h // 7))
    title_col = (
        np.array([0.20, 0.21, 0.26], dtype=np.float32)
        if dark_mode
        else np.array([0.82, 0.85, 0.90], dtype=np.float32)
    )
    tex[:title_h, :] = title_col
    tex[title_h : min(win_h, title_h + 2), :] = 0.36 if dark_mode else 0.62

    traffic = [
        np.array([0.96, 0.36, 0.33], dtype=np.float32),
        np.array([0.98, 0.74, 0.22], dtype=np.float32),
        np.array([0.24, 0.78, 0.34], dtype=np.float32),
    ]
    sub_w = min(win_w, 50)
    yy, xx = np.ogrid[:title_h, :sub_w]
    for dot_idx, col in enumerate(traffic):
        cx = 11 + dot_idx * 11
        cy = title_h // 2
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= 10
        tex[:title_h, :sub_w][mask] = col

    border_col = (
        np.array([0.48, 0.51, 0.58], dtype=np.float32)
        if dark_mode
        else np.array([0.46, 0.50, 0.56], dtype=np.float32)
    )
    tex[:2, :] = border_col
    tex[-2:, :] = border_col
    tex[:, :2] = border_col
    tex[:, -2:] = border_col
    return np.clip(tex, 0.0, 1.0)


def _sample_numpy_bilinear_rgb(tex: np.ndarray, xx: np.ndarray, yy: np.ndarray) -> np.ndarray:
    """Bilinear sample `(H, W, 3)` texture at float arrays `(xx, yy)`."""
    th, tw, _ = tex.shape
    x = np.clip(xx, 0.0, float(tw - 1))
    y = np.clip(yy, 0.0, float(th - 1))
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, tw - 1)
    y1 = np.minimum(y0 + 1, th - 1)
    wx = (x - x0).astype(np.float32)[..., None]
    wy = (y - y0).astype(np.float32)[..., None]
    v00 = tex[y0, x0]
    v01 = tex[y0, x1]
    v10 = tex[y1, x0]
    v11 = tex[y1, x1]
    return (1.0 - wy) * ((1.0 - wx) * v00 + wx * v01) + wy * ((1.0 - wx) * v10 + wx * v11)


def generate_synthetic_rgb_dataset(
    num_scenes: int,
    seed: int,
    half_h: int = 128,
    half_w: int = 192,
) -> Dict[str, torch.Tensor]:
    """Generate full-res RGB macOS Stage Manager large-motion + disocclusion + static-text scenes.

    Full resolution is `(2 * half_h, 2 * half_w) = (256, 384)`.
    Includes:
      - Color syntax-highlighted static Retina code panels & macOS menu bar
      - Large-displacement Stage Manager sweeps (up to +-60 half-res px = +-120 full-res px)
      - Simultaneous 0.70x..1.30x scaling & multi-window overlapping disocclusion borders
      - Smooth scrolling code viewports & animated UI cards
    """
    rng = np.random.default_rng(seed)
    full_h = half_h * 2
    full_w = half_w * 2

    fy, fx = np.meshgrid(
        np.arange(full_h, dtype=np.float32),
        np.arange(full_w, dtype=np.float32),
        indexing="ij",
    )

    rgb0_list = np.empty((num_scenes, 3, full_h, full_w), dtype=np.float32)
    rgb1_list = np.empty((num_scenes, 3, full_h, full_w), dtype=np.float32)
    rgba_list = np.empty((num_scenes, 3, full_h, full_w), dtype=np.float32)
    alpha_list = np.empty((num_scenes, 1, 1, 1), dtype=np.float32)
    gt_flow_half_list = np.empty((num_scenes, 2, half_h, half_w), dtype=np.float32)

    for idx in range(num_scenes):
        # Realistic frame-generation interpolation phases: 2x (0.5) + 3x/4x (0.25..0.75)
        if rng.random() < 0.45:
            alpha = float(rng.uniform(0.42, 0.58))
        else:
            alpha = float(rng.uniform(0.22, 0.78))
        alpha_list[idx, 0, 0, 0] = alpha

        bg_dark = bool(rng.random() < 0.55)
        base_panel = _render_rgb_code_panel(rng, full_h, full_w, dark_mode=bg_dark, line_step=12)

        # Static macOS top menu bar (18 full-res px) with crisp glyphs
        menubar_col = (
            np.array([0.13, 0.14, 0.17], dtype=np.float32)
            if bg_dark
            else np.array([0.92, 0.93, 0.95], dtype=np.float32)
        )
        base_panel[:18, :] = menubar_col
        base_panel[17:19, :] = 0.28 if bg_dark else 0.72
        text_col = (
            np.array([0.90, 0.92, 0.96], dtype=np.float32)
            if bg_dark
            else np.array([0.12, 0.14, 0.18], dtype=np.float32)
        )
        for mx in range(10, full_w - 22, int(rng.integers(24, 42))):
            mw = int(rng.integers(10, 22))
            base_panel[7:11, mx : min(full_w - 6, mx + mw)] = text_col

        # Static Stage Manager left thumbnail strip
        for thumb_y in (30, 96, 162):
            if thumb_y + 44 < full_h:
                thumb = _render_rgb_window_texture(rng, 44, 38, dark_mode=not bg_dark)
                base_panel[thumb_y : thumb_y + 44, 6:44] = thumb

        # Scrolling code/UI viewport on right split (leaves left static code editor clean!)
        has_scroll = bool(rng.random() < 0.70)
        scr_x0 = int(rng.integers(full_w // 2 - 8, full_w // 2 + 32))
        scr_x1 = full_w - int(rng.integers(8, 20))
        scr_y0 = int(rng.integers(24, 48))
        scr_y1 = full_h - int(rng.integers(10, 26))
        scr_tex_h = (scr_y1 - scr_y0) + 180
        scr_tex_w = (scr_x1 - scr_x0) + 120
        scr_tex = _render_rgb_code_panel(
            rng, scr_tex_h, scr_tex_w, dark_mode=bool(rng.random() < 0.5), line_step=12
        )
        scr_dx = float(rng.uniform(-20.0, 20.0) if rng.random() < 0.30 else 0.0)
        scr_dy = float(rng.choice([-1.0, 1.0]) * rng.uniform(10.0, 48.0))

        # Stage Manager multi-window transitions:
        # 1..2 realistic windows (up to +-120 full-res px sweep + 0.70x..1.30x scaling)
        n_windows = 2 if rng.random() < 0.75 else 1
        win_specs = []
        for w_idx in range(n_windows):
            win_w = int(rng.integers(152, 216))
            win_h = int(rng.integers(112, 166))
            win_dark = (not bg_dark) if (w_idx % 2 == 0) else bg_dark
            win_tex = _render_rgb_window_texture(rng, win_h, win_w, dark_mode=win_dark)

            # Simultaneous 0.70x .. 1.30x scaling
            zoom_ratio = float(rng.uniform(0.70, 1.30))
            s0 = 1.0 / math.sqrt(zoom_ratio)
            s1 = math.sqrt(zoom_ratio)

            max_corner_zoom_dx = abs(s1 - s0) * (0.5 * win_w)
            max_corner_zoom_dy = abs(s1 - s0) * (0.5 * win_h)
            is_large_sweep = (w_idx == 0) and (rng.random() < 0.55)
            if is_large_sweep:
                # Wide cross-screen sweeps up to +-120 full-res px (+-60 half-res px)
                max_tx = max(36.0, 118.0 - max_corner_zoom_dx)
                max_ty = max(16.0, 64.0 - max_corner_zoom_dy)
                tx = float(rng.choice([-1.0, 1.0]) * rng.uniform(0.45 * max_tx, max_tx))
                ty = float(rng.uniform(-max_ty, max_ty))
            else:
                max_tx = max(16.0, 64.0 - max_corner_zoom_dx)
                max_ty = max(12.0, 44.0 - max_corner_zoom_dy)
                tx = float(rng.uniform(-max_tx, max_tx))
                ty = float(rng.uniform(-max_ty, max_ty))

            cx_min = max(112.0, 56.0 + 0.5 * abs(tx))
            cx_max = min(full_w - 64.0, full_w - 48.0 - 0.5 * abs(tx))
            cx_mid = 0.58 * full_w if cx_max <= cx_min else float(rng.uniform(cx_min, cx_max))

            cy_min = max(62.0, 36.0 + 0.5 * abs(ty))
            cy_max = min(full_h - 52.0, full_h - 36.0 - 0.5 * abs(ty))
            cy_mid = 0.52 * full_h if cy_max <= cy_min else float(rng.uniform(cy_min, cy_max))

            win_specs.append(
                {
                    "tex": win_tex,
                    "w": float(win_w),
                    "h": float(win_h),
                    "s0": s0,
                    "s1": s1,
                    "cx0": cx_mid - 0.5 * tx,
                    "cy0": cy_mid - 0.5 * ty,
                    "cx1": cx_mid + 0.5 * tx,
                    "cy1": cy_mid + 0.5 * ty,
                }
            )

        # Optional sliding animated UI card / notification banner
        if rng.random() < 0.50:
            card_w = int(rng.integers(88, 132))
            card_h = int(rng.integers(36, 56))
            card_tex = _render_rgb_ui_card_panel(rng, card_h, card_w, dark_mode=not bg_dark)
            card_tx = float(rng.choice([-1.0, 1.0]) * rng.uniform(16.0, 56.0))
            card_ty = float(rng.uniform(-16.0, 16.0))
            card_cx_mid = float(rng.uniform(130.0, full_w - 86.0))
            card_cy_mid = float(rng.uniform(46.0, full_h - 38.0))
            win_specs.append(
                {
                    "tex": card_tex,
                    "w": float(card_w),
                    "h": float(card_h),
                    "s0": 1.0,
                    "s1": 1.0,
                    "cx0": card_cx_mid - 0.5 * card_tx,
                    "cy0": card_cy_mid - 0.5 * card_ty,
                    "cx1": card_cx_mid + 0.5 * card_tx,
                    "cy1": card_cy_mid + 0.5 * card_ty,
                }
            )

        frames_rgb: list[np.ndarray] = []
        alpha_flow_full_x = np.zeros((full_h, full_w), dtype=np.float32)
        alpha_flow_full_y = np.zeros((full_h, full_w), dtype=np.float32)

        for t in (0.0, alpha, 1.0):
            img = base_panel.copy()
            is_alpha = abs(t - alpha) < 1e-6 and alpha not in (0.0, 1.0)

            if has_scroll:
                sub_x = fx[scr_y0:scr_y1, scr_x0:scr_x1] - scr_x0 + 60.0 - t * scr_dx
                sub_y = fy[scr_y0:scr_y1, scr_x0:scr_x1] - scr_y0 + 90.0 - t * scr_dy
                img[scr_y0:scr_y1, scr_x0:scr_x1] = _sample_numpy_bilinear_rgb(scr_tex, sub_x, sub_y)
                img[scr_y0 : scr_y0 + 2, scr_x0:scr_x1] = 0.46
                img[scr_y1 - 2 : scr_y1, scr_x0:scr_x1] = 0.46
                img[scr_y0:scr_y1, scr_x0 : scr_x0 + 2] = 0.46
                if is_alpha:
                    alpha_flow_full_x[scr_y0 + 2 : scr_y1 - 2, scr_x0 + 2 : scr_x1] = scr_dx
                    alpha_flow_full_y[scr_y0 + 2 : scr_y1 - 2, scr_x0 + 2 : scr_x1] = scr_dy

            for spec in win_specs:
                st = (1.0 - t) * spec["s0"] + t * spec["s1"]
                cxt = (1.0 - t) * spec["cx0"] + t * spec["cx1"]
                cyt = (1.0 - t) * spec["cy0"] + t * spec["cy1"]
                half_win_w = 0.5 * spec["w"] * st
                half_win_h = 0.5 * spec["h"] * st

                sh_mask = (
                    (fx >= cxt - half_win_w + 3.0)
                    & (fx <= cxt + half_win_w + 5.0)
                    & (fy >= cyt - half_win_h + 4.0)
                    & (fy <= cyt + half_win_h + 6.0)
                )
                img[sh_mask] *= 0.78

                u = (fx - cxt) / st + 0.5 * spec["w"]
                v = (fy - cyt) / st + 0.5 * spec["h"]
                win_mask = (
                    (u >= 0.0) & (u <= spec["w"] - 1.0) & (v >= 0.0) & (v <= spec["h"] - 1.0)
                )
                if np.any(win_mask):
                    img[win_mask] = _sample_numpy_bilinear_rgb(spec["tex"], u[win_mask], v[win_mask])
                    if is_alpha:
                        u_centered = u[win_mask] - 0.5 * spec["w"]
                        v_centered = v[win_mask] - 0.5 * spec["h"]
                        vx_full = (spec["cx1"] - spec["cx0"]) + (spec["s1"] - spec["s0"]) * u_centered
                        vy_full = (spec["cy1"] - spec["cy0"]) + (spec["s1"] - spec["s0"]) * v_centered
                        alpha_flow_full_x[win_mask] = vx_full
                        alpha_flow_full_y[win_mask] = vy_full

            frames_rgb.append(np.clip(img, 0.0, 1.0).transpose(2, 0, 1))

        rgb0_list[idx] = frames_rgb[0]
        rgba_list[idx] = frames_rgb[1]
        rgb1_list[idx] = frames_rgb[2]

        gt_flow_half_list[idx, 0] = (
            alpha_flow_full_x.reshape(half_h, 2, half_w, 2).mean(axis=(1, 3)) * 0.5
        )
        gt_flow_half_list[idx, 1] = (
            alpha_flow_full_y.reshape(half_h, 2, half_w, 2).mean(axis=(1, 3)) * 0.5
        )

    return {
        "rgb0": torch.from_numpy(rgb0_list),
        "rgb1": torch.from_numpy(rgb1_list),
        "rgba": torch.from_numpy(rgba_list),
        "alpha": torch.from_numpy(alpha_list),
        "gt_flow_half": torch.from_numpy(gt_flow_half_list),
    }


def build_luma_pyramid(
    rgb: torch.Tensor,
) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """2x2 box-downsample pyramid `(half 1/2, quarter 1/4, eighth 1/8)` matching D3D11 Pass 1."""
    luma_full = rgb_to_luma(rgb)
    half = F.avg_pool2d(luma_full, kernel_size=2, stride=2)
    qtr = F.avg_pool2d(half, kernel_size=2, stride=2)
    eig = F.avg_pool2d(qtr, kernel_size=2, stride=2)
    return half, qtr, eig


def _median3x3(flow: torch.Tensor) -> torch.Tensor:
    bsz, ch, height, width = flow.shape
    padded = F.pad(flow, (1, 1, 1, 1), mode="replicate")
    unfolded = F.unfold(padded, kernel_size=3).view(bsz, ch, 9, height, width)
    return unfolded.median(dim=2).values


@torch.no_grad()
def _search_level_flow(
    l0: torch.Tensor,
    l1: torch.Tensor,
    t_eval: torch.Tensor,
    init_flow: torch.Tensor | None,
    steps: Tuple[float, ...],
    max_disp: float,
    grid_vals: Tuple[float, ...] | None = None,
    patch_radius: int = 3,
    mag_weight: float = 0.0010,
    prop_iters: int = 0,
) -> torch.Tensor:
    """Vectorized block-matching flow search with OOB penalty & neighbor priors (matches Pass 2a-c)."""
    bsz, _, height, width = l0.shape
    device = l0.device
    dx0, dy0, _ = compute_spatial_grads(l0)
    dx1, dy1, _ = compute_spatial_grads(l1)
    feat0 = torch.cat([l0, dx0, dy0], dim=1)
    feat1 = torch.cat([l1, dx1, dy1], dim=1)

    k = 2 * patch_radius + 1
    avg_kernel = torch.ones((1, 1, k, k), device=device, dtype=l0.dtype) / float(k * k)
    yy, xx = torch.meshgrid(
        torch.arange(height, device=device, dtype=l0.dtype),
        torch.arange(width, device=device, dtype=l0.dtype),
        indexing="ij",
    )

    def patch_cost(vx_cand: torch.Tensor, vy_cand: torch.Tensor) -> torch.Tensor:
        flow_cand = torch.stack([vx_cand, vy_cand], dim=1)
        f0 = -t_eval * flow_cand
        f1 = (1.0 - t_eval) * flow_cand
        w0 = bilinear_warp(feat0, f0)
        w1 = bilinear_warp(feat1, f1)
        diff = w1 - w0
        raw = diff[:, 0:1].abs() + 0.5 * (diff[:, 1:2].abs() + diff[:, 2:3].abs())
        oob = (
            ((xx + f0[:, 0]) < 0.0)
            | ((xx + f0[:, 0]) > float(width - 1))
            | ((yy + f0[:, 1]) < 0.0)
            | ((yy + f0[:, 1]) > float(height - 1))
            | ((xx + f1[:, 0]) < 0.0)
            | ((xx + f1[:, 0]) > float(width - 1))
            | ((yy + f1[:, 1]) < 0.0)
            | ((yy + f1[:, 1]) > float(height - 1))
        ).to(l0.dtype).unsqueeze(1)
        raw = raw + 0.35 * oob
        sad = F.conv2d(F.pad(raw, (patch_radius,) * 4, mode="replicate"), avg_kernel)
        mag_pen = mag_weight * torch.sqrt(vx_cand * vx_cand + vy_cand * vy_cand + 1e-8).unsqueeze(1)
        return (sad + mag_pen)[:, 0]

    zero_vx = torch.zeros((bsz, height, width), device=device, dtype=l0.dtype)
    zero_vy = torch.zeros((bsz, height, width), device=device, dtype=l0.dtype)
    best_vx = zero_vx.clone()
    best_vy = zero_vy.clone()
    best_cost = patch_cost(best_vx, best_vy)

    if init_flow is not None:
        # Test center + cardinal neighbor priors from previous level (matches GPU Pass 2b/2c)
        pf = F.pad(init_flow, (2, 2, 2, 2), mode="replicate")
        for dy, dx in ((0, 0), (0, -1), (0, 1), (-1, 0), (1, 0), (0, -2), (0, 2), (-2, 0), (2, 0)):
            pr = pf[:, :, 2 + dy : 2 + dy + height, 2 + dx : 2 + dx + width]
            cand_vx = pr[:, 0].clamp(-max_disp, max_disp)
            cand_vy = pr[:, 1].clamp(-max_disp, max_disp)
            c = patch_cost(cand_vx, cand_vy)
            better = c < best_cost
            best_cost = torch.where(better, c, best_cost)
            best_vx = torch.where(better, cand_vx, best_vx)
            best_vy = torch.where(better, cand_vy, best_vy)

    if grid_vals is not None:
        for dy in grid_vals:
            for dx in grid_vals:
                if dx == 0.0 and dy == 0.0:
                    continue
                cand_vx = torch.full_like(best_vx, dx)
                cand_vy = torch.full_like(best_vy, dy)
                c = patch_cost(cand_vx, cand_vy)
                better = c < best_cost
                best_cost = torch.where(better, c, best_cost)
                best_vx = torch.where(better, cand_vx, best_vx)
                best_vy = torch.where(better, cand_vy, best_vy)

    for step in steps:
        base_vx = best_vx
        base_vy = best_vy
        for dy in (-step, 0.0, step):
            for dx in (-step, 0.0, step):
                if dx == 0.0 and dy == 0.0:
                    continue
                cand_vx = (base_vx + dx).clamp(-max_disp, max_disp)
                cand_vy = (base_vy + dy).clamp(-max_disp, max_disp)
                c = patch_cost(cand_vx, cand_vy)
                better = c < best_cost
                best_cost = torch.where(better, c, best_cost)
                best_vx = torch.where(better, cand_vx, best_vx)
                best_vy = torch.where(better, cand_vy, best_vy)

    for _ in range(prop_iters):
        cur = torch.stack([best_vx, best_vy], dim=1)
        pf = F.pad(cur, (2, 2, 2, 2), mode="replicate")
        for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0), (0, -2), (0, 2), (-2, 0), (2, 0)):
            pr = pf[:, :, 2 + dy : 2 + dy + height, 2 + dx : 2 + dx + width]
            c = patch_cost(pr[:, 0], pr[:, 1])
            better = c < best_cost
            best_cost = torch.where(better, c, best_cost)
            best_vx = torch.where(better, pr[:, 0], best_vx)
            best_vy = torch.where(better, pr[:, 1], best_vy)

    # Static-region early-out matching GPU `max_nb < 0.010` guard
    temp_diff = (l1 - l0).abs()
    local_max_diff = F.max_pool2d(temp_diff, kernel_size=5, stride=1, padding=2)[:, 0]
    static_region = local_max_diff < 0.010
    best_vx = torch.where(static_region, zero_vx, best_vx)
    best_vy = torch.where(static_region, zero_vy, best_vy)

    return _median3x3(torch.stack([best_vx, best_vy], dim=1))


@torch.no_grad()
def estimate_multiscale_coarse_flow(
    rgb0: torch.Tensor,
    rgb1: torch.Tensor,
    alpha: torch.Tensor,
    use_midpoint: bool = True,
) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """3-level coarse-to-fine pyramid optical flow (1/8 -> 1/4 -> 1/2), +-128+ full-res px.

    Returns `(flow_half, luma0_half, luma1_half)` at 1/2 resolution.
    """
    half0, qtr0, eig0 = build_luma_pyramid(rgb0)
    half1, qtr1, eig1 = build_luma_pyramid(rgb1)
    t_eval = torch.full_like(alpha, 0.5) if use_midpoint else alpha

    # Pass 2a: 1/8 res search over +-16 eighth-px (+-128 full-res px)
    flow_eig = _search_level_flow(
        eig0,
        eig1,
        t_eval,
        init_flow=None,
        steps=(2.0, 1.0),
        max_disp=16.0,
        grid_vals=(-16.0, -12.0, -8.0, -4.0, 0.0, 4.0, 8.0, 12.0, 16.0),
        patch_radius=4,
        mag_weight=0.0010,
        prop_iters=2,
    )

    # Pass 2b: 1/4 res refinement (+-4 quarter-px around 2x upsampled 1/8 prior + neighbor & zero priors)
    flow_qtr_prior = (
        F.interpolate(flow_eig, size=qtr0.shape[2:], mode="bilinear", align_corners=False) * 2.0
    )
    flow_qtr = _search_level_flow(
        qtr0,
        qtr1,
        t_eval,
        init_flow=flow_qtr_prior,
        steps=(2.0, 1.0),
        max_disp=36.0,
        grid_vals=None,
        patch_radius=3,
        mag_weight=0.0008,
        prop_iters=1,
    )

    # Pass 2c: 1/2 res refinement (+-3.5 half-px with 0.5 half-px subpixel step + neighbor & zero priors)
    flow_half_prior = (
        F.interpolate(flow_qtr, size=half0.shape[2:], mode="bilinear", align_corners=False) * 2.0
    )
    flow_half = _search_level_flow(
        half0,
        half1,
        t_eval,
        init_flow=flow_half_prior,
        steps=(2.0, 1.0, 0.5),
        max_disp=75.0,
        grid_vals=None,
        patch_radius=2,
        mag_weight=0.0005,
        prop_iters=0,
    )

    return flow_half, half0, half1


def build_input_features(
    i0_half: torch.Tensor,
    i1_half: torch.Tensor,
    flow_c_half: torch.Tensor,
    alpha: torch.Tensor,
) -> torch.Tensor:
    """Construct the exact 8-channel input tensor at 1/2 resolution matching `kNeuralConv1PS`."""
    w0_c = bilinear_warp(i0_half, -alpha * flow_c_half)
    w1_c = bilinear_warp(i1_half, (1.0 - alpha) * flow_c_half)
    warped_diff = (w1_c - w0_c).clamp(-1.0, 1.0)
    grad_mag = compute_grad_mag(i0_half)
    vx_norm = (flow_c_half[:, 0:1] / FLOW_NORM_SCALE).clamp(-1.0, 1.0)
    vy_norm = (flow_c_half[:, 1:2] / FLOW_NORM_SCALE).clamp(-1.0, 1.0)
    alpha_ch = alpha.expand_as(i0_half)
    motion_prior = ((i1_half - i0_half).abs() * 6.0).clamp(0.0, 1.0)
    return torch.cat(
        [i0_half, i1_half, warped_diff, grad_mag, vx_norm, vy_norm, alpha_ch, motion_prior],
        dim=1,
    )


class NeuralFlowResNetV2(nn.Module):
    """v2 3-layer 3x3 Residual CNN (8 -> 16 -> 16 -> 7, 4,503 params) with RGB disocclusion repair."""

    def __init__(self) -> None:
        super().__init__()
        self.conv1 = nn.Conv2d(IN_CHANNELS, HIDDEN_CHANNELS, kernel_size=3, padding=0, bias=True)
        self.conv2 = nn.Conv2d(
            HIDDEN_CHANNELS, HIDDEN_CHANNELS, kernel_size=3, padding=0, bias=True
        )
        self.conv3 = nn.Conv2d(HIDDEN_CHANNELS, OUT_CHANNELS, kernel_size=3, padding=0, bias=True)
        self._init_weights()

    def _init_weights(self) -> None:
        nn.init.normal_(self.conv1.weight, mean=0.0, std=0.04)
        nn.init.zeros_(self.conv1.bias)
        nn.init.normal_(self.conv2.weight, mean=0.0, std=0.02)
        nn.init.zeros_(self.conv2.bias)
        nn.init.normal_(self.conv3.weight, mean=0.0, std=0.015)
        nn.init.zeros_(self.conv3.bias)

        with torch.no_grad():
            self.conv1.weight[0, 7, 1, 1] += 1.2   # +motion_prior
            self.conv1.weight[1, 4, 1, 1] += 0.8   # +vx_norm
            self.conv1.weight[2, 4, 1, 1] -= 0.8   # -vx_norm
            self.conv1.weight[3, 5, 1, 1] += 0.8   # +vy_norm
            self.conv1.weight[4, 5, 1, 1] -= 0.8   # -vy_norm
            self.conv1.weight[5, 2, 1, 1] += 1.0   # +warped_diff
            self.conv1.weight[6, 2, 1, 1] -= 1.0   # -warped_diff
            self.conv1.weight[7, 3, 1, 1] += 0.6   # +grad_mag
            self.conv1.weight[8, 6, 1, 1] += 0.6   # +alpha

            # Start static pixels at motion_conf ~ sigmoid(-3.6) ~ 0.026 (< 0.08)
            self.conv3.bias[3] = -3.6
            self.conv3.weight[3, 0, 1, 1] += 5.5
            for hc in (1, 2, 3, 4):
                self.conv3.weight[3, hc, 1, 1] += 2.8

    def _decode_outputs(
        self, raw: torch.Tensor
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
        dv = torch.tanh(raw[:, 0:2]) * RESIDUAL_FLOW_SCALE
        blend_w = torch.sigmoid(raw[:, 2:3])
        motion_conf = torch.sigmoid(raw[:, 3:4])
        rgb_res = torch.tanh(raw[:, 4:7]) * RGB_RESIDUAL_SCALE
        return dv, blend_w, motion_conf, rgb_res

    def forward(
        self, x: torch.Tensor, pad_mode: str = "replicate"
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
        """Exact 3-pass D3D11 shader parity forward pass (clamp-border replicate padding)."""
        h1 = F.relu(self.conv1(F.pad(x, (1, 1, 1, 1), mode=pad_mode)))
        h2 = F.relu(self.conv2(F.pad(h1, (1, 1, 1, 1), mode=pad_mode)) + h1)
        raw = self.conv3(F.pad(h2, (1, 1, 1, 1), mode=pad_mode))
        return self._decode_outputs(raw)

    def forward_patch3x3_batch(
        self, patches: torch.Tensor
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
        """Exact differentiable mirror of C++ `EvalNeuralFlowPatch` on `(N, 8, 3, 3)` patches."""
        h1 = F.relu(self.conv1(F.pad(patches, (1, 1, 1, 1), mode="replicate")))
        h2 = F.relu(self.conv2(F.pad(h1, (1, 1, 1, 1), mode="replicate")) + h1)
        raw = self.conv3(h2)  # (N, 7, 1, 1)
        return self._decode_outputs(raw)


def synthesize_rgb_fullres(
    rgb0: torch.Tensor,
    rgb1: torch.Tensor,
    flow_half: torch.Tensor,
    blend_w_half: torch.Tensor,
    motion_conf_half: torch.Tensor,
    rgb_res_half: torch.Tensor | None,
    alpha: torch.Tensor,
) -> torch.Tensor:
    """Full-res RGB synthesis matching D3D11 Pass 4 (`kWarpPS`) 100%."""
    full_hw = rgb0.shape[2:]
    flow_full = (
        F.interpolate(flow_half, size=full_hw, mode="bilinear", align_corners=False) * 2.0
    )
    blend_w = F.interpolate(blend_w_half, size=full_hw, mode="bilinear", align_corners=False)
    motion_conf = F.interpolate(
        motion_conf_half, size=full_hw, mode="bilinear", align_corners=False
    )

    c0 = bilinear_warp(rgb0, -alpha * flow_full)
    c1 = bilinear_warp(rgb1, (1.0 - alpha) * flow_full)
    lin_warp = (1.0 - alpha) * c0 + alpha * c1
    occ_warp = c0 + blend_w * (c1 - c0)
    occ_mix = 0.5 * lin_warp + 0.5 * occ_warp

    if rgb_res_half is not None:
        rgb_res = F.interpolate(rgb_res_half, size=full_hw, mode="bilinear", align_corners=False)
        repaired = (occ_mix + rgb_res).clamp(0.0, 1.0)
    else:
        repaired = occ_mix.clamp(0.0, 1.0)

    nearest_unwarped = torch.where(alpha < 0.5, rgb0, rgb1)
    return nearest_unwarped + motion_conf * (repaired - nearest_unwarped)


def psnr_db(pred: torch.Tensor, target: torch.Tensor) -> float:
    mse = float(torch.mean((pred - target) ** 2).item())
    if mse <= 1e-12:
        return 99.0
    return -10.0 * math.log10(mse)


def charbonnier(diff: torch.Tensor, eps: float = 1e-3) -> torch.Tensor:
    return torch.sqrt(diff * diff + eps * eps)


def rgb_edge_loss(pred: torch.Tensor, target: torch.Tensor, weight: torch.Tensor) -> torch.Tensor:
    dx_p = pred[:, :, :, 1:] - pred[:, :, :, :-1]
    dx_t = target[:, :, :, 1:] - target[:, :, :, :-1]
    dy_p = pred[:, :, 1:, :] - pred[:, :, :-1, :]
    dy_t = target[:, :, 1:, :] - target[:, :, :-1, :]
    wx = weight[:, :, :, 1:]
    wy = weight[:, :, 1:, :]
    return ((dx_p - dx_t).abs() * wx).mean() + ((dy_p - dy_t).abs() * wy).mean()


def sample_synthetic_static_text_patches(batch_size: int, device: torch.device) -> torch.Tensor:
    """Generate diverse pure-static Retina glyph 3x3 patches for C++ patch evaluator regularization."""
    patches = torch.zeros((batch_size, IN_CHANNELS, 3, 3), device=device, dtype=torch.float32)
    bg = torch.rand((batch_size, 1, 1), device=device) * 0.9
    fg = (1.0 - bg).clamp(0.05, 0.98)
    mask = (torch.rand((batch_size, 3, 3), device=device) > 0.5).float()
    luma = bg * (1.0 - mask) + fg * mask
    patches[:, 0] = luma
    patches[:, 1] = luma
    dx = 0.5 * (luma[:, 1, 2] - luma[:, 1, 0])
    dy = 0.5 * (luma[:, 2, 1] - luma[:, 0, 1])
    g = (torch.sqrt(dx * dx + dy * dy + 1e-12) * 4.0).clamp(0.0, 1.0)
    patches[:, 3] = g.view(batch_size, 1, 1)
    patches[:, 6] = torch.rand((batch_size, 1, 1), device=device) * 0.7 + 0.15
    return patches


def compute_loss(
    model: NeuralFlowResNetV2,
    pred_rgb: torch.Tensor,
    target_rgb: torch.Tensor,
    px_w: torch.Tensor,
    dv_half: torch.Tensor,
    motion_conf_half: torch.Tensor,
    rgb_res_half: torch.Tensor,
    static_clean_f: torch.Tensor,
    moving_half_f: torch.Tensor,
    target_dv: torch.Tensor,
    aux_flow_weight: float,
) -> torch.Tensor:
    diff_full = pred_rgb - target_rgb

    l1_loss = (diff_full.abs() * px_w).mean()
    charb_loss = (charbonnier(diff_full) * px_w).mean()
    mse_loss = ((diff_full * diff_full) * px_w).mean()
    edge_loss = rgb_edge_loss(pred_rgb, target_rgb, px_w)

    # Pure float-mask static-text regularization (leaves disocclusion borders free for RGB repair!)
    st_denom = static_clean_f.sum().clamp_min(1.0)
    conf_hinge = F.relu(motion_conf_half - 0.042) * static_clean_f
    static_conf_pen = (
        ((motion_conf_half * motion_conf_half) * static_clean_f).sum() / st_denom
        + 10.0 * (conf_hinge * conf_hinge).sum() / st_denom
        + 0.35 * (conf_hinge.amax() ** 2)
    )

    res_hinge = F.relu(rgb_res_half.abs() - 0.0055) * static_clean_f
    static_res_pen = (
        ((rgb_res_half * rgb_res_half) * static_clean_f).sum() / (3.0 * st_denom)
        + 15.0 * (res_hinge * res_hinge).sum() / (3.0 * st_denom)
        + 0.40 * (res_hinge.amax() ** 2)
    )

    # Pure static-text 3x3 patch regularization (guarantees C++ EvalNeuralFlowPatch stays ultra-crisp)
    st_patches = sample_synthetic_static_text_patches(64, pred_rgb.device)
    _, _, p_conf, p_res = model.forward_patch3x3_batch(st_patches)
    p_conf_hinge = F.relu(p_conf - 0.040)
    p_res_hinge = F.relu(p_res.abs() - 0.0045)
    patch_static_pen = (
        (p_conf * p_conf).mean()
        + 10.0 * (p_conf_hinge * p_conf_hinge).mean()
        + 5.0 * (p_res * p_res).mean()
        + 20.0 * (p_res_hinge * p_res_hinge).mean()
    )

    # Decaying auxiliary residual-flow supervision on moving pixels
    flow_guide = ((dv_half - target_dv).abs() * moving_half_f).mean()

    return (
        0.40 * l1_loss
        + 0.40 * charb_loss
        + 10.0 * mse_loss
        + 0.18 * edge_loss
        + 0.55 * static_conf_pen
        + 0.80 * static_res_pen
        + 0.35 * patch_static_pen
        + aux_flow_weight * flow_guide
    )


def cpu_reference_eval_patch(
    patch_8x9: np.ndarray,
    w1: np.ndarray,
    b1: np.ndarray,
    w2: np.ndarray,
    b2: np.ndarray,
    w3: np.ndarray,
    b3: np.ndarray,
) -> np.ndarray:
    """Exact mirror of C++ `EvalNeuralFlowPatch` in NumPy (v2 8->16->16->7 ResNet)."""
    h1 = np.zeros((HIDDEN_CHANNELS * 9,), dtype=np.float32)
    for hc in range(HIDDEN_CHANNELS):
        w1_oc_base = hc * (IN_CHANNELS * 9)
        for hy in range(3):
            for hx in range(3):
                acc = float(b1[hc])
                for ic in range(IN_CHANNELS):
                    w1_ic_base = w1_oc_base + ic * 9
                    p_ic_base = ic * 9
                    for ky in range(3):
                        sy = min(2, max(0, hy + ky - 1))
                        for kx in range(3):
                            sx = min(2, max(0, hx + kx - 1))
                            acc += float(w1[w1_ic_base + ky * 3 + kx]) * float(
                                patch_8x9[p_ic_base + sy * 3 + sx]
                            )
                h1[hc * 9 + hy * 3 + hx] = max(0.0, acc)

    h2 = np.zeros((HIDDEN_CHANNELS * 9,), dtype=np.float32)
    for oc in range(HIDDEN_CHANNELS):
        w2_oc_base = oc * (HIDDEN_CHANNELS * 9)
        for hy in range(3):
            for hx in range(3):
                acc = float(b2[oc])
                for ic in range(HIDDEN_CHANNELS):
                    w2_ic_base = w2_oc_base + ic * 9
                    h1_ic_base = ic * 9
                    for ky in range(3):
                        sy = min(2, max(0, hy + ky - 1))
                        for kx in range(3):
                            sx = min(2, max(0, hx + kx - 1))
                            acc += float(w2[w2_ic_base + ky * 3 + kx]) * float(
                                h1[h1_ic_base + sy * 3 + sx]
                            )
                h2[oc * 9 + hy * 3 + hx] = max(0.0, acc + float(h1[oc * 9 + hy * 3 + hx]))

    raw = np.zeros((OUT_CHANNELS,), dtype=np.float32)
    for oc in range(OUT_CHANNELS):
        acc = float(b3[oc])
        w3_oc_base = oc * (HIDDEN_CHANNELS * 9)
        for ic in range(HIDDEN_CHANNELS):
            w3_ic_base = w3_oc_base + ic * 9
            h2_ic_base = ic * 9
            for k in range(9):
                acc += float(w3[w3_ic_base + k]) * float(h2[h2_ic_base + k])
        raw[oc] = acc

    def safe_sigmoid(x: float) -> float:
        return 1.0 / (1.0 + math.exp(-max(-60.0, min(60.0, x))))

    out = np.zeros((OUT_CHANNELS,), dtype=np.float32)
    out[0] = math.tanh(float(raw[0])) * RESIDUAL_FLOW_SCALE
    out[1] = math.tanh(float(raw[1])) * RESIDUAL_FLOW_SCALE
    out[2] = safe_sigmoid(float(raw[2]))
    out[3] = safe_sigmoid(float(raw[3]))
    out[4] = math.tanh(float(raw[4])) * RGB_RESIDUAL_SCALE
    out[5] = math.tanh(float(raw[5])) * RGB_RESIDUAL_SCALE
    out[6] = math.tanh(float(raw[6])) * RGB_RESIDUAL_SCALE
    return out


@torch.no_grad()
def evaluate_model(
    model: NeuralFlowResNetV2,
    val_feats: torch.Tensor,
    val_rgb0: torch.Tensor,
    val_rgb1: torch.Tensor,
    val_rgba: torch.Tensor,
    val_i0_half: torch.Tensor,
    val_i1_half: torch.Tensor,
    val_ia_half: torch.Tensor,
    val_flow_c: torch.Tensor,
    val_alpha: torch.Tensor,
) -> Dict[str, float]:
    model.eval()

    # (a) Naive RGB Cross-Fade
    pred_crossfade = (1.0 - val_alpha) * val_rgb0 + val_alpha * val_rgb1

    # (b) Coarse-to-Fine Optical Flow alone (with Pass 2c static-text gate)
    flow_mag = torch.sqrt(val_flow_c[:, 0:1] ** 2 + val_flow_c[:, 1:2] ** 2 + 1e-12)
    coarse_blend = torch.full_like(val_i0_half, 0.5)
    coarse_conf = torch.maximum(
        (((val_i1_half - val_i0_half).abs() - 0.012) * 20.0).clamp(0.0, 1.0),
        ((flow_mag - 0.35) * 2.0).clamp(0.0, 1.0),
    )
    pred_coarse_gated = synthesize_rgb_fullres(
        val_rgb0, val_rgb1, val_flow_c, coarse_blend, coarse_conf, None, val_alpha
    )
    pred_coarse_ungated = synthesize_rgb_fullres(
        val_rgb0, val_rgb1, val_flow_c, coarse_blend, torch.ones_like(val_i0_half), None, val_alpha
    )

    # (c) Coarse-to-Fine Optical Flow + 3-Layer Neural Residual CNN (with RGB disocclusion repair)
    dv, blend_w, motion_conf, rgb_res = model(val_feats, pad_mode="replicate")
    pred_neural = synthesize_rgb_fullres(
        val_rgb0, val_rgb1, val_flow_c + dv, blend_w, motion_conf, rgb_res, val_alpha
    )
    pred_neural_no_res = synthesize_rgb_fullres(
        val_rgb0, val_rgb1, val_flow_c + dv, blend_w, motion_conf, None, val_alpha
    )

    # Static text pixels: crisp static glyph edges in static neighborhoods
    temp_diff = (val_i1_half - val_i0_half).abs()
    local_diff = F.max_pool2d(temp_diff, kernel_size=5, stride=1, padding=2)
    local_flow = F.max_pool2d(flow_mag, kernel_size=5, stride=1, padding=2)
    grad_mag = val_feats[:, 3:4]
    static_text_mask = (
        (local_diff < 0.01)
        & ((val_ia_half - val_i1_half).abs() < 0.01)
        & (local_flow < 0.25)
        & (grad_mag > 0.25)
    )

    st_conf = motion_conf[static_text_mask]
    st_res_abs = rgb_res.abs().amax(dim=1, keepdim=True)[static_text_mask]

    # Verify C++ EvalNeuralFlowPatch on static-text patches
    w1 = model.conv1.weight.detach().cpu().numpy().reshape(-1)
    b1 = model.conv1.bias.detach().cpu().numpy().reshape(-1)
    w2 = model.conv2.weight.detach().cpu().numpy().reshape(-1)
    b2 = model.conv2.bias.detach().cpu().numpy().reshape(-1)
    w3 = model.conv3.weight.detach().cpu().numpy().reshape(-1)
    b3 = model.conv3.bias.detach().cpu().numpy().reshape(-1)

    patch_static_confs = []
    patch_static_res = []
    padded_val = F.pad(val_feats[:4].detach().cpu(), (1, 1, 1, 1), mode="replicate").numpy()
    mask_np = static_text_mask[:4, 0].detach().cpu().numpy()
    for b_idx in range(min(4, mask_np.shape[0])):
        ys, xs = np.where(mask_np[b_idx])
        if len(ys) == 0:
            continue
        stride = max(1, len(ys) // 10)
        for py, px in zip(ys[::stride][:10], xs[::stride][:10]):
            patch = padded_val[b_idx, :, py : py + 3, px : px + 3].reshape(IN_CHANNELS * 9)
            out7 = cpu_reference_eval_patch(patch, w1, b1, w2, b2, w3, b3)
            patch_static_confs.append(float(out7[3]))
            patch_static_res.append(float(np.max(np.abs(out7[4:7]))))

    for bg_v, fg_v in [(0.12, 0.90), (0.94, 0.10)]:
        synth_patch = np.zeros((IN_CHANNELS, 3, 3), dtype=np.float32)
        synth_patch[0] = np.array(
            [[bg_v, fg_v, bg_v], [bg_v, fg_v, bg_v], [bg_v, fg_v, bg_v]], dtype=np.float32
        )
        synth_patch[1] = synth_patch[0]
        synth_patch[3] = 0.85
        synth_patch[6] = 0.50
        synth_out = cpu_reference_eval_patch(synth_patch.reshape(-1), w1, b1, w2, b2, w3, b3)
        patch_static_confs.append(float(synth_out[3]))
        patch_static_res.append(float(np.max(np.abs(synth_out[4:7]))))

    return {
        "psnr_crossfade": psnr_db(pred_crossfade, val_rgba),
        "psnr_coarse_flow": psnr_db(pred_coarse_gated, val_rgba),
        "psnr_coarse_ungated": psnr_db(pred_coarse_ungated, val_rgba),
        "psnr_neural_no_res": psnr_db(pred_neural_no_res, val_rgba),
        "psnr_neural": psnr_db(pred_neural, val_rgba),
        "static_text_conf_mean": float(st_conf.mean().item()),
        "static_text_conf_p99": float(torch.quantile(st_conf.float(), 0.99).item()),
        "static_text_conf_max": float(st_conf.max().item()),
        "static_text_res_mean": float(st_res_abs.mean().item()),
        "static_text_res_max": float(st_res_abs.max().item()),
        "patch_static_conf_max": max(patch_static_confs) if patch_static_confs else 0.0,
        "patch_static_res_max": max(patch_static_res) if patch_static_res else 0.0,
    }


def verify_patch_evaluator_parity(model: NeuralFlowResNetV2) -> float:
    """Verify `cpu_reference_eval_patch` (exact C++ mirror) matches PyTorch `forward_patch3x3_batch`."""
    model_cpu = NeuralFlowResNetV2()
    model_cpu.load_state_dict({k: v.detach().cpu() for k, v in model.state_dict().items()})
    model_cpu.eval()

    w1 = model_cpu.conv1.weight.detach().numpy().reshape(-1)
    b1 = model_cpu.conv1.bias.detach().numpy().reshape(-1)
    w2 = model_cpu.conv2.weight.detach().numpy().reshape(-1)
    b2 = model_cpu.conv2.bias.detach().numpy().reshape(-1)
    w3 = model_cpu.conv3.weight.detach().numpy().reshape(-1)
    b3 = model_cpu.conv3.bias.detach().numpy().reshape(-1)

    rng = np.random.default_rng(12345)
    patches_np = rng.uniform(-0.8, 0.8, size=(6, IN_CHANNELS, 3, 3)).astype(np.float32)
    with torch.no_grad():
        dv, bw, mc, rr = model_cpu.forward_patch3x3_batch(torch.from_numpy(patches_np))
        got_all = torch.cat([dv, bw, mc, rr], dim=1)[:, :, 0, 0].numpy()

    max_err = 0.0
    for i in range(patches_np.shape[0]):
        ref = cpu_reference_eval_patch(patches_np[i].reshape(IN_CHANNELS * 9), w1, b1, w2, b2, w3, b3)
        max_err = max(max_err, float(np.max(np.abs(got_all[i] - ref))))
    return max_err


def _format_float_array(values: np.ndarray, per_line: int = 8) -> str:
    flat = [f"{float(v):.8f}f" for v in values.reshape(-1)]
    lines = []
    for i in range(0, len(flat), per_line):
        lines.append("    " + ", ".join(flat[i : i + per_line]) + ",")
    return "\n".join(lines)


def write_weights_header(
    model: NeuralFlowResNetV2,
    header_path: pathlib.Path,
    metrics: Dict[str, float],
) -> None:
    w1 = model.conv1.weight.detach().cpu().numpy().astype(np.float32)
    b1 = model.conv1.bias.detach().cpu().numpy().astype(np.float32)
    w2 = model.conv2.weight.detach().cpu().numpy().astype(np.float32)
    b2 = model.conv2.bias.detach().cpu().numpy().astype(np.float32)
    w3 = model.conv3.weight.detach().cpu().numpy().astype(np.float32)
    b3 = model.conv3.bias.detach().cpu().numpy().astype(np.float32)

    assert w1.shape == (16, 8, 3, 3)
    assert b1.shape == (16,)
    assert w2.shape == (16, 16, 3, 3)
    assert b2.shape == (16,)
    assert w3.shape == (7, 16, 3, 3)
    assert b3.shape == (7,)

    gain_vs_flow = metrics["psnr_neural"] - metrics["psnr_coarse_flow"]
    content = f"""#pragma once
#include <algorithm>
#include <cmath>

// Auto-generated by test/train_neural_flow.py (v2 3-layer 8->16->16->7 Residual CNN, 4,503 params).
// Held-out RGB Stage Manager large-motion + disocclusion + static-text validation PSNR:
//   Naive Cross-Fade:                              {metrics['psnr_crossfade']:.2f} dB
//   Coarse-to-Fine Optical Flow alone:             {metrics['psnr_coarse_flow']:.2f} dB
//   Coarse-to-Fine Flow + 3-Layer Neural RGB-Res:  {metrics['psnr_neural']:.2f} dB ({gain_vs_flow:+.2f} dB vs Flow alone)
//   Static text motion_conf:                       mean={metrics['static_text_conf_mean']:.5f}, max={metrics['static_text_conf_max']:.5f} (< 0.08)
//   Static text |rgb_residual|:                    mean={metrics['static_text_res_mean']:.5f}, max={metrics['static_text_res_max']:.5f} (< 0.01)

namespace mac_mirror::neural_flow {{

inline constexpr int kVersion = 2;
inline constexpr int kInChannels = 8;
inline constexpr int kHiddenChannels = 16;
inline constexpr int kOutChannels = 7;
inline constexpr float kFlowNormScale = 24.0f;
inline constexpr float kResidualFlowScale = 6.0f;
inline constexpr float kRgbResidualScale = 0.25f;

// Layout: [out_c * (in_c * 9) + in_c * 9 + ky * 3 + kx]
inline constexpr float kConv1Weight[16 * 8 * 9] = {{
{_format_float_array(w1, per_line=6)}
}};

inline constexpr float kConv1Bias[16] = {{
{_format_float_array(b1, per_line=8)}
}};

inline constexpr float kConv2Weight[16 * 16 * 9] = {{
{_format_float_array(w2, per_line=6)}
}};

inline constexpr float kConv2Bias[16] = {{
{_format_float_array(b2, per_line=8)}
}};

inline constexpr float kConv3Weight[7 * 16 * 9] = {{
{_format_float_array(w3, per_line=6)}
}};

inline constexpr float kConv3Bias[7] = {{
{_format_float_array(b3, per_line=7)}
}};

// Single-pixel reference evaluator (`patch[8 * 9]`, using spatially replicated h1/h2 for 3x3 layers 2 & 3).
// Writes out[0]=dvx, out[1]=dvy, out[2]=blend_w, out[3]=motion_conf, out[4]=res_r, out[5]=res_g, out[6]=res_b.
inline void EvalNeuralFlowPatch(const float patch[8 * 9], float out[7]) noexcept {{
    float h1[kHiddenChannels * 9];
    for (int hc = 0; hc < kHiddenChannels; ++hc) {{
        const int w1_oc_base = hc * (kInChannels * 9);
        for (int hy = 0; hy < 3; ++hy) {{
            for (int hx = 0; hx < 3; ++hx) {{
                float acc = kConv1Bias[hc];
                for (int ic = 0; ic < kInChannels; ++ic) {{
                    const int w1_ic_base = w1_oc_base + ic * 9;
                    const int p_ic_base = ic * 9;
                    for (int ky = 0; ky < 3; ++ky) {{
                        const int sy = std::clamp(hy + ky - 1, 0, 2);
                        for (int kx = 0; kx < 3; ++kx) {{
                            const int sx = std::clamp(hx + kx - 1, 0, 2);
                            acc += kConv1Weight[w1_ic_base + ky * 3 + kx] *
                                   patch[p_ic_base + sy * 3 + sx];
                        }}
                    }}
                }}
                h1[hc * 9 + hy * 3 + hx] = std::max(0.0f, acc);
            }}
        }}
    }}

    float h2[kHiddenChannels * 9];
    for (int oc = 0; oc < kHiddenChannels; ++oc) {{
        const int w2_oc_base = oc * (kHiddenChannels * 9);
        for (int hy = 0; hy < 3; ++hy) {{
            for (int hx = 0; hx < 3; ++hx) {{
                float acc = kConv2Bias[oc];
                for (int ic = 0; ic < kHiddenChannels; ++ic) {{
                    const int w2_ic_base = w2_oc_base + ic * 9;
                    const int h1_ic_base = ic * 9;
                    for (int ky = 0; ky < 3; ++ky) {{
                        const int sy = std::clamp(hy + ky - 1, 0, 2);
                        for (int kx = 0; kx < 3; ++kx) {{
                            const int sx = std::clamp(hx + kx - 1, 0, 2);
                            acc += kConv2Weight[w2_ic_base + ky * 3 + kx] *
                                   h1[h1_ic_base + sy * 3 + sx];
                        }}
                    }}
                }}
                const int idx = oc * 9 + hy * 3 + hx;
                h2[idx] = std::max(0.0f, acc + h1[idx]);
            }}
        }}
    }}

    float raw[kOutChannels];
    for (int oc = 0; oc < kOutChannels; ++oc) {{
        float acc = kConv3Bias[oc];
        const int w3_oc_base = oc * (kHiddenChannels * 9);
        for (int ic = 0; ic < kHiddenChannels; ++ic) {{
            const int w3_ic_base = w3_oc_base + ic * 9;
            const int h2_ic_base = ic * 9;
            for (int k = 0; k < 9; ++k) {{
                acc += kConv3Weight[w3_ic_base + k] * h2[h2_ic_base + k];
            }}
        }}
        raw[oc] = acc;
    }}

    const auto safe_sigmoid = [](float x) noexcept -> float {{
        const float clamped = std::clamp(x, -60.0f, 60.0f);
        return 1.0f / (1.0f + std::exp(-clamped));
    }};

    out[0] = std::tanh(raw[0]) * kResidualFlowScale;
    out[1] = std::tanh(raw[1]) * kResidualFlowScale;
    out[2] = safe_sigmoid(raw[2]);
    out[3] = safe_sigmoid(raw[3]);
    out[4] = std::tanh(raw[4]) * kRgbResidualScale;
    out[5] = std::tanh(raw[5]) * kRgbResidualScale;
    out[6] = std::tanh(raw[6]) * kRgbResidualScale;
}}

}}  // namespace mac_mirror::neural_flow
"""
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(content, encoding="utf-8")


def load_weights_from_header(model: NeuralFlowResNetV2, header_path: pathlib.Path) -> None:
    text = header_path.read_text(encoding="utf-8")

    def extract_array(name: str, expected_len: int) -> np.ndarray:
        pattern = rf"{name}\s*\[[^\]]+\]\s*=\s*\{{([^}}]+)\}};"
        match = re.search(pattern, text, flags=re.DOTALL)
        if not match:
            raise ValueError(f"Could not find array {name} in {header_path}")
        floats = [
            float(tok.rstrip("fF"))
            for tok in re.findall(r"[-+]?\d*\.\d+(?:[eE][-+]?\d+)?[fF]?", match.group(1))
        ]
        if len(floats) != expected_len:
            raise ValueError(f"Array {name} length {len(floats)} != {expected_len}")
        return np.array(floats, dtype=np.float32)

    w1 = extract_array("kConv1Weight", 16 * 8 * 9).reshape(16, 8, 3, 3)
    b1 = extract_array("kConv1Bias", 16)
    w2 = extract_array("kConv2Weight", 16 * 16 * 9).reshape(16, 16, 3, 3)
    b2 = extract_array("kConv2Bias", 16)
    w3 = extract_array("kConv3Weight", 7 * 16 * 9).reshape(7, 16, 3, 3)
    b3 = extract_array("kConv3Bias", 7)

    with torch.no_grad():
        model.conv1.weight.copy_(torch.from_numpy(w1))
        model.conv1.bias.copy_(torch.from_numpy(b1))
        model.conv2.weight.copy_(torch.from_numpy(w2))
        model.conv2.bias.copy_(torch.from_numpy(b2))
        model.conv3.weight.copy_(torch.from_numpy(w3))
        model.conv3.bias.copy_(torch.from_numpy(b3))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eval-only", action="store_true", help="Load existing header and evaluate")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER_PATH)
    parser.add_argument("--epochs", type=int, default=60)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--train-scenes", type=int, default=64)
    parser.add_argument("--val-scenes", type=int, default=24)
    parser.add_argument("--lr", type=float, default=6e-3)
    parser.add_argument("--device", type=str, default="auto")
    args = parser.parse_args()

    torch.manual_seed(20261128)
    np.random.seed(20261128)
    device = select_device(args.device)
    print(f"[neural_flow v2] device={device}", flush=True)

    t0 = time.time()
    print(f"[neural_flow v2] generating held-out RGB validation set ({args.val_scenes} scenes)...", flush=True)
    val_cpu = generate_synthetic_rgb_dataset(args.val_scenes, seed=9001)
    val_rgb0 = val_cpu["rgb0"].to(device)
    val_rgb1 = val_cpu["rgb1"].to(device)
    val_rgba = val_cpu["rgba"].to(device)
    val_alpha = val_cpu["alpha"].to(device)
    val_ia_half = build_luma_pyramid(val_rgba)[0]
    val_flow_c, val_i0_half, val_i1_half = estimate_multiscale_coarse_flow(
        val_rgb0, val_rgb1, val_alpha, use_midpoint=True
    )
    val_feats = build_input_features(val_i0_half, val_i1_half, val_flow_c, val_alpha)

    model = NeuralFlowResNetV2().to(device)
    n_params = sum(p.numel() for p in model.parameters())
    print(f"[neural_flow v2] model params={n_params} (expected 4503)", flush=True)

    if args.eval_only:
        load_weights_from_header(model, args.header)
        model.to(device)
    else:
        print(f"[neural_flow v2] generating RGB training set ({args.train_scenes} scenes)...", flush=True)
        train_cpu = generate_synthetic_rgb_dataset(args.train_scenes, seed=2026)
        tr_rgb0 = train_cpu["rgb0"].to(device)
        tr_rgb1 = train_cpu["rgb1"].to(device)
        tr_rgba = train_cpu["rgba"].to(device)
        tr_alpha = train_cpu["alpha"].to(device)
        tr_gt_flow = train_cpu["gt_flow_half"].to(device)
        tr_ia_half = build_luma_pyramid(tr_rgba)[0]

        split = (args.train_scenes * 3) // 4
        fc_m, i0_m, i1_m = estimate_multiscale_coarse_flow(
            tr_rgb0[:split], tr_rgb1[:split], tr_alpha[:split], use_midpoint=True
        )
        fc_a, i0_a, i1_a = estimate_multiscale_coarse_flow(
            tr_rgb0[split:], tr_rgb1[split:], tr_alpha[split:], use_midpoint=False
        )
        tr_flow_c = torch.cat([fc_m, fc_a], dim=0)
        tr_i0_half = torch.cat([i0_m, i0_a], dim=0)
        tr_i1_half = torch.cat([i1_m, i1_a], dim=0)
        tr_feats = build_input_features(tr_i0_half, tr_i1_half, tr_flow_c, tr_alpha)

        # Precompute training masks once (avoids per-step MPS sync overhead)
        tr_moving_full = (
            ((tr_rgb1 - tr_rgb0).abs().amax(dim=1, keepdim=True) >= 0.01)
            | ((tr_rgba - tr_rgb1).abs().amax(dim=1, keepdim=True) >= 0.01)
        ).float()
        tr_px_w = 1.0 + 4.5 * tr_moving_full
        tr_temp_diff = (tr_i1_half - tr_i0_half).abs()
        tr_flow_mag = torch.sqrt(tr_flow_c[:, 0:1] ** 2 + tr_flow_c[:, 1:2] ** 2 + 1e-12)
        tr_local_diff = F.max_pool2d(tr_temp_diff, kernel_size=5, stride=1, padding=2)
        tr_local_flow = F.max_pool2d(tr_flow_mag, kernel_size=5, stride=1, padding=2)
        tr_static_clean_f = (
            (tr_local_diff < 0.01)
            & ((tr_ia_half - tr_i1_half).abs() < 0.01)
            & (tr_local_flow < 0.25)
        ).float()
        tr_moving_half_f = (
            ((tr_i1_half - tr_i0_half).abs() >= 0.01) | ((tr_ia_half - tr_i1_half).abs() >= 0.01)
        ).float()
        tr_target_dv = (tr_gt_flow - tr_flow_c).clamp(-RESIDUAL_FLOW_SCALE, RESIDUAL_FLOW_SCALE)

        print(
            f"[neural_flow v2] dataset & 3-level pyramid flow ready in {time.time() - t0:.1f}s; training...",
            flush=True,
        )
        optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
            optimizer, T_max=args.epochs, eta_min=1.5e-4
        )

        best_score = -1e9
        best_state = None
        num_train = tr_feats.shape[0]

        for epoch in range(1, args.epochs + 1):
            model.train()
            perm = torch.randperm(num_train, device=device)
            aux_w = 0.05 * max(0.05, 1.0 - epoch / float(args.epochs))
            epoch_loss = 0.0
            steps = 0

            for start in range(0, num_train, args.batch_size):
                idx = perm[start : start + args.batch_size]
                bx = tr_feats[idx]
                br0 = tr_rgb0[idx]
                br1 = tr_rgb1[idx]
                bra = tr_rgba[idx]
                bpxw = tr_px_w[idx]
                bst = tr_static_clean_f[idx]
                bmh = tr_moving_half_f[idx]
                btdv = tr_target_dv[idx]
                bfc = tr_flow_c[idx]
                ba = tr_alpha[idx]

                optimizer.zero_grad(set_to_none=True)
                dv, blend_w, motion_conf, rgb_res = model(bx, pad_mode="replicate")
                pred_rgb = synthesize_rgb_fullres(br0, br1, bfc + dv, blend_w, motion_conf, rgb_res, ba)
                loss = compute_loss(
                    model,
                    pred_rgb,
                    bra,
                    bpxw,
                    dv,
                    motion_conf,
                    rgb_res,
                    bst,
                    bmh,
                    btdv,
                    aux_w,
                )
                loss.backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 2.0)
                optimizer.step()

                epoch_loss += float(loss.item())
                steps += 1

            scheduler.step()

            if epoch == 1 or epoch % 10 == 0 or epoch == args.epochs:
                metrics = evaluate_model(
                    model,
                    val_feats,
                    val_rgb0,
                    val_rgb1,
                    val_rgba,
                    val_i0_half,
                    val_i1_half,
                    val_ia_half,
                    val_flow_c,
                    val_alpha,
                )
                static_ok = (
                    metrics["static_text_conf_max"] < 0.075
                    and metrics["static_text_res_max"] < 0.0095
                    and metrics["patch_static_conf_max"] < 0.075
                    and metrics["patch_static_res_max"] < 0.0095
                )
                score = metrics["psnr_neural"] - (10.0 if not static_ok else 0.0)
                if score > best_score:
                    best_score = score
                    best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
                print(
                    f"  epoch {epoch:3d}/{args.epochs} | loss={epoch_loss / max(1, steps):.5f} | "
                    f"RGB PSNR xfade={metrics['psnr_crossfade']:.2f}, "
                    f"flow={metrics['psnr_coarse_flow']:.2f}, "
                    f"neural(no-res)={metrics['psnr_neural_no_res']:.2f}, "
                    f"neural(RGB-res)={metrics['psnr_neural']:.2f} "
                    f"({metrics['psnr_neural'] - metrics['psnr_coarse_flow']:+.2f} dB) | "
                    f"static conf(mean={metrics['static_text_conf_mean']:.4f}, max={metrics['static_text_conf_max']:.4f}), "
                    f"res(max={metrics['static_text_res_max']:.4f}, patch={metrics['patch_static_res_max']:.4f})",
                    flush=True,
                )

        if best_state is not None:
            model.load_state_dict(best_state)
            model.to(device)

    metrics = evaluate_model(
        model,
        val_feats,
        val_rgb0,
        val_rgb1,
        val_rgba,
        val_i0_half,
        val_i1_half,
        val_ia_half,
        val_flow_c,
        val_alpha,
    )
    parity_err = verify_patch_evaluator_parity(model)

    if not args.eval_only:
        write_weights_header(model, args.header, metrics)
        print(f"[neural_flow v2] wrote weights header -> {args.header}", flush=True)

    gain_vs_xfade = metrics["psnr_neural"] - metrics["psnr_crossfade"]
    gain_vs_flow = metrics["psnr_neural"] - metrics["psnr_coarse_flow"]
    gain_vs_nores = metrics["psnr_neural"] - metrics["psnr_neural_no_res"]

    print("\n=== Held-Out RGB Stage Manager Large-Motion + Disocclusion + Static-Text Validation ===")
    print(f"  (a) Naive Cross-Fade RGB PSNR                        : {metrics['psnr_crossfade']:.2f} dB")
    print(
        f"  (b) Coarse-to-Fine Optical Flow alone RGB PSNR       : {metrics['psnr_coarse_flow']:.2f} dB "
        f"(ungated: {metrics['psnr_coarse_ungated']:.2f} dB)"
    )
    print(
        f"  (c) Coarse-to-Fine + 3-Layer Neural (no RGB-res)     : {metrics['psnr_neural_no_res']:.2f} dB"
    )
    print(
        f"  (d) Coarse-to-Fine + 3-Layer Neural RGB-Residual     : {metrics['psnr_neural']:.2f} dB "
        f"({gain_vs_flow:+.2f} dB vs Flow, {gain_vs_xfade:+.2f} dB vs Cross-Fade, {gain_vs_nores:+.2f} dB from RGB-res)"
    )
    print(
        f"  Static text motion_conf                              : "
        f"mean={metrics['static_text_conf_mean']:.5f}, "
        f"p99={metrics['static_text_conf_p99']:.5f}, "
        f"max={metrics['static_text_conf_max']:.5f}, "
        f"patch_max={metrics['patch_static_conf_max']:.5f} (threshold < 0.08)"
    )
    print(
        f"  Static text |rgb_residual|                           : "
        f"mean={metrics['static_text_res_mean']:.5f}, "
        f"max={metrics['static_text_res_max']:.5f}, "
        f"patch_max={metrics['patch_static_res_max']:.5f} (threshold < 0.01)"
    )
    print(f"  C++ EvalNeuralFlowPatch parity max err               : {parity_err:.2e}")

    ok_psnr = (
        metrics["psnr_neural"] > metrics["psnr_crossfade"]
        and metrics["psnr_neural"] > metrics["psnr_coarse_flow"]
        and metrics["psnr_neural"] > metrics["psnr_coarse_ungated"]
    )
    ok_static_conf = (
        metrics["static_text_conf_mean"] < 0.08
        and metrics["static_text_conf_max"] < 0.08
        and metrics["patch_static_conf_max"] < 0.08
    )
    ok_static_res = (
        metrics["static_text_res_max"] < 0.01
        and metrics["patch_static_res_max"] < 0.01
    )
    ok_parity = parity_err < 1e-5

    if not (ok_psnr and ok_static_conf and ok_static_res and ok_parity):
        print("[neural_flow v2] ERROR: validation criteria not met!", file=sys.stderr)
        return 1

    print(
        "[neural_flow v2] PASS: v2 3-layer Residual CNN + RGB disocclusion repair beats both "
        "cross-fade and coarse-to-fine flow, with static text motion_conf < 0.08 and |rgb_res| < 0.01."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
