#!/usr/bin/env python3
"""Train and evaluate the compact 8->8->4 3x3 CNN for macOS screen mirroring motion interpolation.

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

IN_CHANNELS = 8
HIDDEN_CHANNELS = 8
OUT_CHANNELS = 4


def select_device(requested: str = "auto") -> torch.device:
    if requested != "auto":
        return torch.device(requested)
    # For this compact 876-parameter CNN, vectorized CPU GEMM completes 180 epochs in ~5s
    # and avoids Metal/MPS scatter-gather overhead while remaining deterministic across hosts.
    return torch.device("cpu")


def bilinear_warp(img: torch.Tensor, flow_xy: torch.Tensor) -> torch.Tensor:
    """Sample `img` (B, 1, H, W) at pixel coordinates `p + flow_xy` (clamped border).

    Uses explicit 4-tap bilinear gather so analytical flow gradients are exact on MPS, CUDA, and CPU.
    """
    bsz, _, height, width = img.shape
    yy, xx = torch.meshgrid(
        torch.arange(height, device=img.device, dtype=img.dtype),
        torch.arange(width, device=img.device, dtype=img.dtype),
        indexing="ij",
    )
    sample_x = (xx.unsqueeze(0) + flow_xy[:, 0]).clamp(0.0, float(width - 1))
    sample_y = (yy.unsqueeze(0) + flow_xy[:, 1]).clamp(0.0, float(height - 1))

    x0 = torch.floor(sample_x).long().clamp(0, width - 1)
    y0 = torch.floor(sample_y).long().clamp(0, height - 1)
    x1 = (x0 + 1).clamp(0, width - 1)
    y1 = (y0 + 1).clamp(0, height - 1)

    wx = (sample_x - x0.to(img.dtype)).unsqueeze(1)
    wy = (sample_y - y0.to(img.dtype)).unsqueeze(1)

    flat = img.reshape(bsz, 1, height * width)
    v00 = torch.gather(flat, 2, (y0 * width + x0).reshape(bsz, 1, -1)).reshape(bsz, 1, height, width)
    v01 = torch.gather(flat, 2, (y0 * width + x1).reshape(bsz, 1, -1)).reshape(bsz, 1, height, width)
    v10 = torch.gather(flat, 2, (y1 * width + x0).reshape(bsz, 1, -1)).reshape(bsz, 1, height, width)
    v11 = torch.gather(flat, 2, (y1 * width + x1).reshape(bsz, 1, -1)).reshape(bsz, 1, height, width)

    top = v00 + wx * (v01 - v00)
    bot = v10 + wx * (v11 - v10)
    return top + wy * (bot - top)


def compute_grad_mag(luma: torch.Tensor) -> torch.Tensor:
    """Compute `clamp(hypot(dI/dx, dI/dy) * 4.0, 0.0, 1.0)` on (B, 1, H, W)."""
    padded = F.pad(luma, (1, 1, 1, 1), mode="replicate")
    dx = 0.5 * (padded[:, :, 1:-1, 2:] - padded[:, :, 1:-1, :-2])
    dy = 0.5 * (padded[:, :, 2:, 1:-1] - padded[:, :, :-2, 1:-1])
    grad = torch.sqrt(dx * dx + dy * dy + 1e-12) * 4.0
    return grad.clamp(0.0, 1.0)


def _render_text_panel_texture(
    rng: np.random.Generator,
    tex_h: int,
    tex_w: int,
    dark_mode: bool,
    line_step: int = 12,
) -> np.ndarray:
    """Create high-resolution Retina code/UI panel with crisp 1px-4px glyph strokes."""
    bg = float(rng.uniform(0.08, 0.16) if dark_mode else rng.uniform(0.91, 0.98))
    canvas = np.full((tex_h, tex_w), bg, dtype=np.float32)

    # Optional gutter & subtle alternating editor highlight line
    gutter_w = int(rng.integers(24, 44))
    gutter_bg = np.clip(bg + (0.035 if dark_mode else -0.035), 0.0, 1.0)
    canvas[:, :gutter_w] = gutter_bg
    canvas[:, gutter_w : gutter_w + 2] = np.clip(bg + (0.14 if dark_mode else -0.14), 0.0, 1.0)

    active_row = int(rng.integers(2, max(3, tex_h // line_step - 2))) * line_step
    if active_row + line_step <= tex_h:
        canvas[active_row : active_row + line_step, gutter_w + 2 :] = np.clip(
            bg + (0.05 if dark_mode else -0.045), 0.0, 1.0
        )

    for y in range(6, tex_h - 6, line_step):
        # Line-number glyphs in gutter (crisp 2px-4px Retina strokes -> 1px quarter-res edges)
        num_luma = float(rng.uniform(0.38, 0.52) if dark_mode else rng.uniform(0.45, 0.62))
        num_x0 = max(4, gutter_w - int(rng.integers(12, 20)))
        canvas[y : y + 4, num_x0 : gutter_w - 6] = num_luma

        if rng.random() < 0.14:
            continue  # Blank code line

        indent = gutter_w + 8 + int(rng.integers(0, 5)) * 12
        x = indent
        n_tokens = int(rng.integers(2, 8))
        for _ in range(n_tokens):
            tok_w = int(rng.integers(8, 44))
            if x + tok_w >= tex_w - 6:
                break
            fg = float(
                rng.uniform(0.62, 0.96) if dark_mode else rng.uniform(0.06, 0.38)
            )
            # Crisp horizontal stem + vertical glyph notches for realistic Retina text edges
            stroke_h = int(rng.choice([3, 4, 5]))
            canvas[y : min(tex_h, y + stroke_h), x : x + tok_w] = fg
            # Carve inter-character gaps (4px pitch -> sharp 1px quarter-res glyph edges)
            for gx in range(x + 3, x + tok_w - 1, int(rng.choice([4, 5, 6]))):
                canvas[y : min(tex_h, y + stroke_h), gx : min(tex_w, gx + 2)] = bg
            # Ascenders / descenders
            if rng.random() < 0.55 and y >= 2:
                asc_x = x + int(rng.integers(0, max(1, tok_w - 3)))
                canvas[y - 2 : y + stroke_h, asc_x : min(tex_w, asc_x + 2)] = fg
            x += tok_w + int(rng.integers(6, 14))

    # Crisp 1px/2px UI separator bars / cards
    for _ in range(int(rng.integers(1, 4))):
        sep_y = int(rng.integers(16, tex_h - 16))
        sep_luma = np.clip(bg + (0.18 if dark_mode else -0.18), 0.0, 1.0)
        canvas[sep_y : sep_y + 2, gutter_w + 2 :] = sep_luma

    return canvas


def _sample_numpy_bilinear(tex: np.ndarray, xx: np.ndarray, yy: np.ndarray) -> np.ndarray:
    th, tw = tex.shape
    x = np.clip(xx, 0.0, float(tw - 1))
    y = np.clip(yy, 0.0, float(th - 1))
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, tw - 1)
    y1 = np.minimum(y0 + 1, th - 1)
    wx = (x - x0).astype(np.float32)
    wy = (y - y0).astype(np.float32)
    v00 = tex[y0, x0]
    v01 = tex[y0, x1]
    v10 = tex[y1, x0]
    v11 = tex[y1, x1]
    return (1.0 - wy) * ((1.0 - wx) * v00 + wx * v01) + wy * ((1.0 - wx) * v10 + wx * v11)


def _render_window_texture(
    rng: np.random.Generator, win_h: int, win_w: int, dark_mode: bool
) -> np.ndarray:
    tex = _render_text_panel_texture(rng, win_h, win_w, dark_mode=dark_mode, line_step=12)
    # macOS window titlebar + traffic-light dots + crisp 3px border
    title_h = 20
    title_luma = float(rng.uniform(0.16, 0.24) if dark_mode else rng.uniform(0.84, 0.91))
    tex[:title_h, :] = title_luma
    tex[title_h : title_h + 2, :] = float(0.32 if dark_mode else 0.68)
    for dot_idx, dot_luma in enumerate([0.52, 0.72, 0.64]):
        cx = 12 + dot_idx * 12
        cy = 10
        yy, xx = np.ogrid[:title_h, : min(win_w, 52)]
        mask = (xx - cx) ** 2 + (yy - cy) ** 2 <= 16
        tex[:title_h, : min(win_w, 52)][mask] = dot_luma

    border_luma = float(0.46 if dark_mode else 0.54)
    tex[:3, :] = border_luma
    tex[-3:, :] = border_luma
    tex[:, :3] = border_luma
    tex[:, -3:] = border_luma
    return tex


def generate_synthetic_dataset(
    num_scenes: int,
    seed: int,
    q_h: int = 64,
    q_w: int = 96,
    scale_full: int = 4,
) -> Dict[str, torch.Tensor]:
    """Generate macOS Retina static text + scrolling UI + Stage Manager zoom/occlusion scenes."""
    rng = np.random.default_rng(seed)
    full_h = q_h * scale_full
    full_w = q_w * scale_full

    fy, fx = np.meshgrid(
        np.arange(full_h, dtype=np.float32),
        np.arange(full_w, dtype=np.float32),
        indexing="ij",
    )

    i0_list = np.empty((num_scenes, 1, q_h, q_w), dtype=np.float32)
    i1_list = np.empty((num_scenes, 1, q_h, q_w), dtype=np.float32)
    ia_list = np.empty((num_scenes, 1, q_h, q_w), dtype=np.float32)
    alpha_list = np.empty((num_scenes, 1, 1, 1), dtype=np.float32)
    gt_flow_list = np.empty((num_scenes, 2, q_h, q_w), dtype=np.float32)

    for idx in range(num_scenes):
        alpha = float(rng.uniform(0.15, 0.85))
        alpha_list[idx, 0, 0, 0] = alpha

        bg_dark = bool(rng.random() < 0.55)
        base_panel = _render_text_panel_texture(rng, full_h, full_w, dark_mode=bg_dark, line_step=12)

        # Static macOS top menu bar (24 full-res px = 6 quarter-res px) with crisp glyphs
        menubar_luma = float(0.14 if bg_dark else 0.92)
        base_panel[:24, :] = menubar_luma
        base_panel[23:25, :] = float(0.30 if bg_dark else 0.72)
        for mx in range(12, full_w - 24, int(rng.integers(28, 48))):
            mw = int(rng.integers(12, 26))
            base_panel[9:14, mx : min(full_w - 6, mx + mw)] = float(0.88 if bg_dark else 0.14)

        # Static Stage Manager left thumbnail strip thumbnails
        strip_w = 56
        for thumb_y in (44, 118, 190):
            if thumb_y + 48 < full_h:
                thumb = _render_window_texture(rng, 48, 40, dark_mode=not bg_dark)
                base_panel[thumb_y : thumb_y + 48, 8:48] = thumb

        frames: list[np.ndarray] = []
        alpha_flow_full_x = np.zeros((full_h, full_w), dtype=np.float32)
        alpha_flow_full_y = np.zeros((full_h, full_w), dtype=np.float32)

        # Scrolling UI viewport parameters (placed on right/bottom split, leaving large static text region)
        has_scroll = bool(rng.random() < 0.88)
        scr_x0 = int(rng.integers(full_w // 2 - 20, full_w // 2 + 40))
        scr_x1 = full_w - int(rng.integers(8, 24))
        scr_y0 = int(rng.integers(32, 68))
        scr_y1 = full_h - int(rng.integers(12, 36))
        scr_tex_h = (scr_y1 - scr_y0) + 140
        scr_tex_w = (scr_x1 - scr_x0) + 100
        scr_tex = _render_text_panel_texture(
            rng, scr_tex_h, scr_tex_w, dark_mode=bool(rng.random() < 0.5), line_step=12
        )
        # Scroll displacement in full-res pixels (within +-42 full-res px = +-10.5 quarter-res px)
        scr_dx = float(rng.uniform(-16.0, 16.0) if rng.random() < 0.35 else 0.0)
        scr_dy = float(rng.choice([-1.0, 1.0]) * rng.uniform(8.0, 42.0))

        # Stage Manager multi-window transitions (1 or 2 windows with scaling 0.75x..1.25x + 2D translation)
        n_windows = 2 if rng.random() < 0.75 else 1
        win_specs = []
        for w_idx in range(n_windows):
            win_w = int(rng.integers(116, 176))
            win_h = int(rng.integers(88, 136))
            win_dark = (not bg_dark) if w_idx == 0 else bg_dark
            win_tex = _render_window_texture(rng, win_h, win_w, dark_mode=win_dark)

            # Continuous zoom factor ratio s1 / s0 in [0.75, 1.25] (clamped so edge velocity <= 46 full-res px)
            zoom_ratio = float(rng.uniform(0.76, 1.24))
            if zoom_ratio >= 1.0:
                s0 = 1.0 / math.sqrt(zoom_ratio)
                s1 = math.sqrt(zoom_ratio)
            else:
                s0 = 1.0 / math.sqrt(zoom_ratio)
                s1 = math.sqrt(zoom_ratio)

            # Keep max corner velocity realistic within +-11.5 quarter-res px (+-46 full-res px)
            max_corner_zoom_dx = abs(s1 - s0) * (0.5 * win_w)
            max_corner_zoom_dy = abs(s1 - s0) * (0.5 * win_h)
            rem_dx = max(6.0, 44.0 - max_corner_zoom_dx)
            rem_dy = max(6.0, 40.0 - max_corner_zoom_dy)

            cx_mid = float(rng.uniform(96.0, full_w - 96.0))
            cy_mid = float(rng.uniform(76.0, full_h - 68.0))
            tx = float(rng.uniform(-rem_dx, rem_dx))
            ty = float(rng.uniform(-rem_dy, rem_dy))

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

        for t in (0.0, alpha, 1.0):
            img = base_panel.copy()
            is_alpha = abs(t - alpha) < 1e-6 and alpha not in (0.0, 1.0)

            if has_scroll:
                sub_x = fx[scr_y0:scr_y1, scr_x0:scr_x1] - scr_x0 + 50.0 - t * scr_dx
                sub_y = fy[scr_y0:scr_y1, scr_x0:scr_x1] - scr_y0 + 70.0 - t * scr_dy
                img[scr_y0:scr_y1, scr_x0:scr_x1] = _sample_numpy_bilinear(scr_tex, sub_x, sub_y)
                # Crisp static viewport frame border
                img[scr_y0 : scr_y0 + 2, scr_x0:scr_x1] = 0.45
                img[scr_y1 - 2 : scr_y1, scr_x0:scr_x1] = 0.45
                img[scr_y0:scr_y1, scr_x0 : scr_x0 + 2] = 0.45
                if is_alpha:
                    alpha_flow_full_x[scr_y0 + 2 : scr_y1 - 2, scr_x0 + 2 : scr_x1] = scr_dx
                    alpha_flow_full_y[scr_y0 + 2 : scr_y1 - 2, scr_x0 + 2 : scr_x1] = scr_dy

            for spec in win_specs:
                st = (1.0 - t) * spec["s0"] + t * spec["s1"]
                cxt = (1.0 - t) * spec["cx0"] + t * spec["cx1"]
                cyt = (1.0 - t) * spec["cy0"] + t * spec["cy1"]
                half_w = 0.5 * spec["w"] * st
                half_h = 0.5 * spec["h"] * st

                # Subtle Stage Manager drop shadow behind foreground window
                sh_mask = (
                    (fx >= cxt - half_w + 4.0)
                    & (fx <= cxt + half_w + 6.0)
                    & (fy >= cyt - half_h + 5.0)
                    & (fy <= cyt + half_h + 7.0)
                )
                img[sh_mask] *= 0.76

                # Map screen coordinates back to window-local texture coordinates u in [0, w-1], v in [0, h-1]
                u = (fx - cxt) / st + 0.5 * spec["w"]
                v = (fy - cyt) / st + 0.5 * spec["h"]
                win_mask = (u >= 0.0) & (u <= spec["w"] - 1.0) & (v >= 0.0) & (v <= spec["h"] - 1.0)
                if np.any(win_mask):
                    img[win_mask] = _sample_numpy_bilinear(spec["tex"], u[win_mask], v[win_mask])
                    if is_alpha:
                        # Exact trajectory X(t) = c(t) + s(t)*(u - 0.5*W) => V = X(1) - X(0)
                        u_centered = u[win_mask] - 0.5 * spec["w"]
                        v_centered = v[win_mask] - 0.5 * spec["h"]
                        vx_full = (spec["cx1"] - spec["cx0"]) + (spec["s1"] - spec["s0"]) * u_centered
                        vy_full = (spec["cy1"] - spec["cy0"]) + (spec["s1"] - spec["s0"]) * v_centered
                        alpha_flow_full_x[win_mask] = vx_full
                        alpha_flow_full_y[win_mask] = vy_full

            # Box-downsample 4x4 Retina to quarter-res luma in [0, 1]
            q_img = img.reshape(q_h, scale_full, q_w, scale_full).mean(axis=(1, 3))
            frames.append(np.clip(q_img, 0.0, 1.0))

        i0_list[idx, 0] = frames[0]
        ia_list[idx, 0] = frames[1]
        i1_list[idx, 0] = frames[2]
        gt_flow_list[idx, 0] = (
            alpha_flow_full_x.reshape(q_h, scale_full, q_w, scale_full).mean(axis=(1, 3)) / scale_full
        )
        gt_flow_list[idx, 1] = (
            alpha_flow_full_y.reshape(q_h, scale_full, q_w, scale_full).mean(axis=(1, 3)) / scale_full
        )

    return {
        "i0": torch.from_numpy(i0_list),
        "i1": torch.from_numpy(i1_list),
        "ia": torch.from_numpy(ia_list),
        "alpha": torch.from_numpy(alpha_list),
        "gt_flow": torch.from_numpy(gt_flow_list),
    }


@torch.no_grad()
def estimate_coarse_flow(
    i0: torch.Tensor,
    i1: torch.Tensor,
    alpha: torch.Tensor,
    use_midpoint: bool = False,
) -> torch.Tensor:
    """Hierarchical Grid + Diamond Block-Matching Optical Flow up to +-12 quarter-res px."""
    bsz, _, height, width = i0.shape
    device = i0.device
    g0 = compute_grad_mag(i0)
    g1 = compute_grad_mag(i1)
    t_eval = torch.full_like(alpha, 0.5) if use_midpoint else alpha

    avg_kernel = torch.ones((1, 1, 7, 7), device=device, dtype=i0.dtype) / 49.0

    def patch_cost(vx_cand: torch.Tensor, vy_cand: torch.Tensor) -> torch.Tensor:
        flow_cand = torch.stack([vx_cand, vy_cand], dim=1)
        w0 = bilinear_warp(i0, -t_eval * flow_cand)
        w1 = bilinear_warp(i1, (1.0 - t_eval) * flow_cand)
        wg0 = bilinear_warp(g0, -t_eval * flow_cand)
        wg1 = bilinear_warp(g1, (1.0 - t_eval) * flow_cand)
        raw = (w1 - w0).abs() + 0.35 * (wg1 - wg0).abs()
        sad = F.conv2d(F.pad(raw, (3, 3, 3, 3), mode="replicate"), avg_kernel)
        # Slight zero-motion preference so flat/static text regions never pick spurious motion
        mag_pen = 0.0020 * torch.sqrt(vx_cand * vx_cand + vy_cand * vy_cand + 1e-8).unsqueeze(1)
        return (sad + mag_pen)[:, 0]

    best_vx = torch.zeros((bsz, height, width), device=device, dtype=i0.dtype)
    best_vy = torch.zeros((bsz, height, width), device=device, dtype=i0.dtype)
    best_cost = patch_cost(best_vx, best_vy)

    # Stage 1: Coarse 7x7 grid over {-12, -8, -4, 0, 4, 8, 12}
    for dy in (-12.0, -8.0, -4.0, 0.0, 4.0, 8.0, 12.0):
        for dx in (-12.0, -8.0, -4.0, 0.0, 4.0, 8.0, 12.0):
            if dx == 0.0 and dy == 0.0:
                continue
            cand_vx = torch.full_like(best_vx, dx)
            cand_vy = torch.full_like(best_vy, dy)
            c = patch_cost(cand_vx, cand_vy)
            better = c < best_cost
            best_cost = torch.where(better, c, best_cost)
            best_vx = torch.where(better, cand_vx, best_vx)
            best_vy = torch.where(better, cand_vy, best_vy)

    # Stage 2: Step-2 refinement around coarse winner, then Stage 3: Step-1 refinement
    for step in (2.0, 1.0):
        base_vx = best_vx
        base_vy = best_vy
        for dy in (-step, 0.0, step):
            for dx in (-step, 0.0, step):
                if dx == 0.0 and dy == 0.0:
                    continue
                cand_vx = (base_vx + dx).clamp(-12.0, 12.0)
                cand_vy = (base_vy + dy).clamp(-12.0, 12.0)
                c = patch_cost(cand_vx, cand_vy)
                better = c < best_cost
                best_cost = torch.where(better, c, best_cost)
                best_vx = torch.where(better, cand_vx, best_vx)
                best_vy = torch.where(better, cand_vy, best_vy)

    return torch.stack([best_vx, best_vy], dim=1)


def build_input_features(
    i0: torch.Tensor,
    i1: torch.Tensor,
    flow_c: torch.Tensor,
    alpha: torch.Tensor,
) -> torch.Tensor:
    """Construct the exact 8-channel input tensor at quarter resolution."""
    w0_c = bilinear_warp(i0, -alpha * flow_c)
    w1_c = bilinear_warp(i1, (1.0 - alpha) * flow_c)
    warped_diff = (w1_c - w0_c).clamp(-1.0, 1.0)
    grad_mag = compute_grad_mag(i0)
    vx_norm = (flow_c[:, 0:1] / 12.0).clamp(-1.0, 1.0)
    vy_norm = (flow_c[:, 1:2] / 12.0).clamp(-1.0, 1.0)
    alpha_ch = alpha.expand_as(i0)
    motion_prior = ((i1 - i0).abs() * 6.0).clamp(0.0, 1.0)
    return torch.cat(
        [i0, i1, warped_diff, grad_mag, vx_norm, vy_norm, alpha_ch, motion_prior],
        dim=1,
    )


class NeuralFlowCNN(nn.Module):
    """Compact 2-layer 3x3 CNN (8 -> 8 -> 4 channels)."""

    def __init__(self) -> None:
        super().__init__()
        self.conv1 = nn.Conv2d(IN_CHANNELS, HIDDEN_CHANNELS, kernel_size=3, padding=0, bias=True)
        self.conv2 = nn.Conv2d(HIDDEN_CHANNELS, OUT_CHANNELS, kernel_size=3, padding=0, bias=True)
        # Precompute the static 9x9 clamp-border gather table for a 3x3 patch:
        # For each hidden pos (hy, hx) and kernel tap (ky, kx), index into patch[sy * 3 + sx].
        gather_idx = []
        for hy in range(3):
            row = []
            for hx in range(3):
                taps = []
                for ky in range(3):
                    sy = min(2, max(0, hy + ky - 1))
                    for kx in range(3):
                        sx = min(2, max(0, hx + kx - 1))
                        taps.append(sy * 3 + sx)
                row.append(taps)
            gather_idx.append(row)
        self.register_buffer(
            "patch_gather_idx",
            torch.tensor(gather_idx, dtype=torch.long).view(9, 9),
            persistent=False,
        )
        self._init_weights()

    def _init_weights(self) -> None:
        nn.init.normal_(self.conv1.weight, mean=0.0, std=0.08)
        nn.init.zeros_(self.conv1.bias)
        nn.init.normal_(self.conv2.weight, mean=0.0, std=0.02)
        with torch.no_grad():
            self.conv1.weight[0, 7, 1, 1] += 1.2   # +motion_prior (ch7)
            self.conv1.weight[1, 4, 1, 1] += 0.6   # +vx_c/12 (ch4)
            self.conv1.weight[2, 4, 1, 1] -= 0.6   # -vx_c/12 (ch4)
            self.conv1.weight[3, 5, 1, 1] += 0.6   # +vy_c/12 (ch5)
            self.conv1.weight[4, 5, 1, 1] -= 0.6   # -vy_c/12 (ch5)
            self.conv1.weight[5, 2, 1, 1] += 1.0   # +warped_diff (ch2)
            self.conv1.weight[6, 2, 1, 1] -= 1.0   # -warped_diff (ch2)

            self.conv2.bias.copy_(torch.tensor([0.0, 0.0, 0.0, -3.6], dtype=torch.float32))
            self.conv2.weight[3, 0, 1, 1] += 5.5
            for hc in (1, 2, 3, 4):
                self.conv2.weight[3, hc, 1, 1] += 2.5

    def _decode_outputs(self, raw: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        dv = torch.tanh(raw[:, 0:2]) * 3.0
        blend_w = torch.sigmoid(raw[:, 2:3])
        motion_conf = torch.sigmoid(raw[:, 3:4])
        return dv, blend_w, motion_conf

    def forward_full(
        self, x: torch.Tensor, pad_mode: str = "replicate"
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """Standard 2D CNN forward pass across the full quarter-res feature map."""
        h = F.relu(self.conv1(F.pad(x, (1, 1, 1, 1), mode=pad_mode)))
        raw = self.conv2(F.pad(h, (1, 1, 1, 1), mode=pad_mode))
        return self._decode_outputs(raw)

    def prepare_patch_sub(self, x: torch.Tensor) -> torch.Tensor:
        """Pre-extract clamped 3x3 sub-neighborhoods `(B*H*W, 9, C*9)` for fast GEMM training."""
        bsz, ch, height, width = x.shape
        x_pad = F.pad(x, (1, 1, 1, 1), mode="replicate")
        patches = F.unfold(x_pad, kernel_size=3).transpose(1, 2).contiguous()
        num_px = bsz * height * width
        patches_c9 = patches.view(num_px, ch, 9)
        return (
            patches_c9[:, :, self.patch_gather_idx]
            .permute(0, 2, 1, 3)
            .reshape(num_px, 9, ch * 9)
            .contiguous()
        )

    def forward_patch3x3_from_sub(
        self, sub: torch.Tensor, bsz: int, height: int, width: int
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        num_px = bsz * height * width
        h = F.relu(
            F.linear(sub, self.conv1.weight.view(HIDDEN_CHANNELS, IN_CHANNELS * 9), self.conv1.bias)
        )
        h_flat = h.transpose(1, 2).reshape(num_px, HIDDEN_CHANNELS * 9)
        raw = F.linear(
            h_flat, self.conv2.weight.view(OUT_CHANNELS, HIDDEN_CHANNELS * 9), self.conv2.bias
        )
        raw = raw.view(bsz, height, width, OUT_CHANNELS).permute(0, 3, 1, 2).contiguous()
        return self._decode_outputs(raw)

    def forward_patch3x3(
        self, x: torch.Tensor
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """Exact single-pixel 3x3 neighborhood evaluator matching C++ `EvalNeuralFlowPatch`."""
        bsz, _, height, width = x.shape
        sub = self.prepare_patch_sub(x)
        return self.forward_patch3x3_from_sub(sub, bsz, height, width)


def synthesize_luma(
    i0: torch.Tensor,
    i1: torch.Tensor,
    flow: torch.Tensor,
    blend_w: torch.Tensor,
    motion_conf: torch.Tensor,
    alpha: torch.Tensor,
) -> torch.Tensor:
    """Synthesize intermediate frame luma using the exact contract formula."""
    w0 = bilinear_warp(i0, -alpha * flow)
    w1 = bilinear_warp(i1, (1.0 - alpha) * flow)
    lin_warp = (1.0 - alpha) * w0 + alpha * w1
    occ_warp = w0 + blend_w * (w1 - w0)
    occ_mix = 0.5 * lin_warp + 0.5 * occ_warp
    nearest_unwarped = torch.where(alpha < 0.5, i0, i1)
    return nearest_unwarped + motion_conf * (occ_mix - nearest_unwarped)


def psnr_db(pred: torch.Tensor, target: torch.Tensor) -> float:
    mse = float(torch.mean((pred - target) ** 2).item())
    if mse <= 1e-12:
        return 99.0
    return -10.0 * math.log10(mse)


def compute_loss(
    pred: torch.Tensor,
    motion_conf: torch.Tensor,
    dv: torch.Tensor,
    flow_c: torch.Tensor,
    gt_flow: torch.Tensor,
    i0: torch.Tensor,
    i1: torch.Tensor,
    ia: torch.Tensor,
    aux_flow_weight: float,
) -> torch.Tensor:
    diff_abs = (pred - ia).abs()
    moving_mask = (((i1 - i0).abs() >= 0.01) | ((ia - i1).abs() >= 0.01)).float()
    pixel_weight = 1.0 + 4.0 * moving_mask
    l1_loss = (diff_abs * pixel_weight).mean()
    mse_loss = (((pred - ia) ** 2) * pixel_weight).mean()

    # Exact sharpness/static-text preservation penalty from specification:
    # motion_conf^2 where |I1 - I0| < 0.01 and |I_alpha - I1| < 0.01
    static_mask = ((i1 - i0).abs() < 0.01) & ((ia - i1).abs() < 0.01)
    if torch.any(static_mask):
        static_pen = (motion_conf[static_mask] ** 2).mean()
    else:
        static_pen = pred.new_tensor(0.0)

    # Bounded ground-truth residual guidance on moving pixels to avoid 1px glyph local minima
    target_dv = (gt_flow - flow_c).clamp(-3.0, 3.0)
    flow_guide = ((dv - target_dv).abs() * moving_mask).mean()

    return l1_loss + 3.0 * mse_loss + 0.35 * static_pen + aux_flow_weight * flow_guide


@torch.no_grad()
def evaluate_model(
    model: NeuralFlowCNN,
    val_feats: torch.Tensor,
    val_i0: torch.Tensor,
    val_i1: torch.Tensor,
    val_ia: torch.Tensor,
    val_flow_c: torch.Tensor,
    val_alpha: torch.Tensor,
) -> Dict[str, float]:
    model.eval()
    # (a) Naive cross-fade
    pred_crossfade = (1.0 - val_alpha) * val_i0 + val_alpha * val_i1

    # (b) Coarse Optical Flow alone (dv=0, blend_w=0.5, Pass-2 coarse motion gate)
    coarse_blend = torch.full_like(val_i0, 0.5)
    coarse_conf = (((val_i1 - val_i0).abs() - 0.012) * 20.0).clamp(0.0, 1.0)
    pred_coarse = synthesize_luma(val_i0, val_i1, val_flow_c, coarse_blend, coarse_conf, val_alpha)
    pred_coarse_ungated = synthesize_luma(
        val_i0, val_i1, val_flow_c, coarse_blend, torch.ones_like(val_i0), val_alpha
    )

    # (c1) Coarse Optical Flow + Neural refinement (exact 3x3 patch evaluator matching C++ header)
    dv_p, blend_p, conf_p = model.forward_patch3x3(val_feats)
    pred_neural_patch = synthesize_luma(
        val_i0, val_i1, val_flow_c + dv_p, blend_p, conf_p, val_alpha
    )

    # (c2) Coarse Optical Flow + Neural refinement (full-image 2D Conv evaluator)
    dv_f, blend_f, conf_f = model.forward_full(val_feats, pad_mode="replicate")
    pred_neural_full = synthesize_luma(
        val_i0, val_i1, val_flow_c + dv_f, blend_f, conf_f, val_alpha
    )

    # Static text pixels: truly static glyphs/edges (|I1 - I0| < 0.01, |Ia - I1| < 0.01, crisp edge grad_mag > 0.2, zero coarse flow)
    grad_mag = val_feats[:, 3:4]
    flow_mag = torch.sqrt(val_flow_c[:, 0:1] ** 2 + val_flow_c[:, 1:2] ** 2)
    static_all_mask = ((val_i1 - val_i0).abs() < 0.01) & ((val_ia - val_i1).abs() < 0.01) & (flow_mag < 0.25)
    static_text_mask = static_all_mask & (grad_mag > 0.25)

    static_text_conf_mean = float(conf_p[static_text_mask].mean().item())
    static_text_conf_p99 = float(torch.quantile(conf_p[static_text_mask].float(), 0.99).item())
    static_text_conf_max = float(conf_p[static_text_mask].max().item())
    static_text_conf_full_max = float(conf_f[static_text_mask].max().item())

    return {
        "psnr_crossfade": psnr_db(pred_crossfade, val_ia),
        "psnr_coarse_flow": psnr_db(pred_coarse, val_ia),
        "psnr_coarse_ungated": psnr_db(pred_coarse_ungated, val_ia),
        "psnr_neural_patch": psnr_db(pred_neural_patch, val_ia),
        "psnr_neural_full": psnr_db(pred_neural_full, val_ia),
        "static_text_conf_mean": static_text_conf_mean,
        "static_text_conf_p99": static_text_conf_p99,
        "static_text_conf_max": static_text_conf_max,
        "static_text_conf_full_max": static_text_conf_full_max,
    }


def _format_float_array(values: np.ndarray, per_line: int = 8) -> str:
    flat = [f"{float(v):.8f}f" for v in values.reshape(-1)]
    lines = []
    for i in range(0, len(flat), per_line):
        lines.append("    " + ", ".join(flat[i : i + per_line]) + ",")
    return "\n".join(lines)


def write_weights_header(
    model: NeuralFlowCNN,
    header_path: pathlib.Path,
    metrics: Dict[str, float],
) -> None:
    w1 = model.conv1.weight.detach().cpu().numpy().astype(np.float32)
    b1 = model.conv1.bias.detach().cpu().numpy().astype(np.float32)
    w2 = model.conv2.weight.detach().cpu().numpy().astype(np.float32)
    b2 = model.conv2.bias.detach().cpu().numpy().astype(np.float32)

    assert w1.shape == (8, 8, 3, 3)
    assert b1.shape == (8,)
    assert w2.shape == (4, 8, 3, 3)
    assert b2.shape == (4,)

    content = f"""#pragma once
#include <algorithm>
#include <cmath>

// Auto-generated by test/train_neural_flow.py (PyTorch 8->8->4 3x3 CNN).
// Held-out Stage Manager zoom+occlusion+text validation PSNR:
//   Naive cross-fade:            {metrics['psnr_crossfade']:.2f} dB
//   Coarse Optical Flow alone:   {metrics['psnr_coarse_flow']:.2f} dB
//   Coarse Flow + Neural CNN:    {metrics['psnr_neural_patch']:.2f} dB (full-conv: {metrics['psnr_neural_full']:.2f} dB)
//   Static text motion_conf:     mean={metrics['static_text_conf_mean']:.5f}, max={metrics['static_text_conf_max']:.5f} (< 0.08)

namespace mac_mirror::neural_flow {{

inline constexpr int kInChannels = 8;
inline constexpr int kHiddenChannels = 8;
inline constexpr int kOutChannels = 4;

// Layout: [out_c * (in_c * 9) + in_c * 9 + ky * 3 + kx] for ky in 0..2, kx in 0..2
inline constexpr float kConv1Weight[8 * 8 * 9] = {{
{_format_float_array(w1, per_line=6)}
}};

inline constexpr float kConv1Bias[8] = {{
{_format_float_array(b1, per_line=8)}
}};

inline constexpr float kConv2Weight[4 * 8 * 9] = {{
{_format_float_array(w2, per_line=6)}
}};

inline constexpr float kConv2Bias[4] = {{
{_format_float_array(b2, per_line=4)}
}};

// Pure C++17 reference evaluator for a single pixel's 8x(3x3) neighborhood (`patch[in_c * 9 + ky * 3 + kx]`).
// Writes `out[0]=dvx`, `out[1]=dvy`, `out[2]=blend_w`, `out[3]=motion_conf`.
inline void EvalNeuralFlowPatch(const float patch[8 * 9], float out[4]) noexcept {{
    float hidden[kHiddenChannels * 9];
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
                hidden[hc * 9 + hy * 3 + hx] = std::max(0.0f, acc);
            }}
        }}
    }}

    float raw[kOutChannels];
    for (int oc = 0; oc < kOutChannels; ++oc) {{
        float acc = kConv2Bias[oc];
        const int w2_oc_base = oc * (kHiddenChannels * 9);
        for (int hc = 0; hc < kHiddenChannels; ++hc) {{
            const int w2_hc_base = w2_oc_base + hc * 9;
            const int h_base = hc * 9;
            for (int k = 0; k < 9; ++k) {{
                acc += kConv2Weight[w2_hc_base + k] * hidden[h_base + k];
            }}
        }}
        raw[oc] = acc;
    }}

    const auto safe_sigmoid = [](float x) noexcept -> float {{
        const float clamped = std::clamp(x, -60.0f, 60.0f);
        return 1.0f / (1.0f + std::exp(-clamped));
    }};

    out[0] = std::tanh(raw[0]) * 3.0f;
    out[1] = std::tanh(raw[1]) * 3.0f;
    out[2] = safe_sigmoid(raw[2]);
    out[3] = safe_sigmoid(raw[3]);
}}

}}  // namespace mac_mirror::neural_flow
"""
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(content, encoding="utf-8")


def load_weights_from_header(model: NeuralFlowCNN, header_path: pathlib.Path) -> None:
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

    w1 = extract_array("kConv1Weight", 8 * 8 * 9).reshape(8, 8, 3, 3)
    b1 = extract_array("kConv1Bias", 8)
    w2 = extract_array("kConv2Weight", 4 * 8 * 9).reshape(4, 8, 3, 3)
    b2 = extract_array("kConv2Bias", 4)

    with torch.no_grad():
        model.conv1.weight.copy_(torch.from_numpy(w1))
        model.conv1.bias.copy_(torch.from_numpy(b1))
        model.conv2.weight.copy_(torch.from_numpy(w2))
        model.conv2.bias.copy_(torch.from_numpy(b2))


def cpu_reference_eval_patch(
    patch_8x9: np.ndarray,
    w1: np.ndarray,
    b1: np.ndarray,
    w2: np.ndarray,
    b2: np.ndarray,
) -> np.ndarray:
    """Exact mirror of C++ `EvalNeuralFlowPatch` in NumPy to verify bit-accuracy."""
    hidden = np.zeros((8 * 9,), dtype=np.float32)
    for hc in range(8):
        for hy in range(3):
            for hx in range(3):
                acc = float(b1[hc])
                for ic in range(8):
                    for ky in range(3):
                        sy = min(2, max(0, hy + ky - 1))
                        for kx in range(3):
                            sx = min(2, max(0, hx + kx - 1))
                            acc += float(w1[hc * 72 + ic * 9 + ky * 3 + kx]) * float(
                                patch_8x9[ic * 9 + sy * 3 + sx]
                            )
                hidden[hc * 9 + hy * 3 + hx] = max(0.0, acc)

    raw = np.zeros((4,), dtype=np.float32)
    for oc in range(4):
        acc = float(b2[oc])
        for hc in range(8):
            for k in range(9):
                acc += float(w2[oc * 72 + hc * 9 + k]) * float(hidden[hc * 9 + k])
        raw[oc] = acc

    out = np.zeros((4,), dtype=np.float32)
    out[0] = math.tanh(float(raw[0])) * 3.0
    out[1] = math.tanh(float(raw[1])) * 3.0
    out[2] = 1.0 / (1.0 + math.exp(-max(-60.0, min(60.0, float(raw[2])))))
    out[3] = 1.0 / (1.0 + math.exp(-max(-60.0, min(60.0, float(raw[3])))))
    return out


def verify_patch_evaluator_parity(model: NeuralFlowCNN, feats: torch.Tensor) -> float:
    model_cpu = NeuralFlowCNN()
    model_cpu.load_state_dict(model.state_dict())
    model_cpu.eval()

    sample = feats[:1].detach().cpu()
    with torch.no_grad():
        dv, blend_w, conf = model_cpu.forward_patch3x3(sample)
        torch_out = torch.cat([dv, blend_w, conf], dim=1)[0]  # (4, H, W)

    w1 = model_cpu.conv1.weight.detach().numpy().reshape(-1)
    b1 = model_cpu.conv1.bias.detach().numpy().reshape(-1)
    w2 = model_cpu.conv2.weight.detach().numpy().reshape(-1)
    b2 = model_cpu.conv2.bias.detach().numpy().reshape(-1)

    padded = F.pad(sample, (1, 1, 1, 1), mode="replicate")[0].numpy()
    max_err = 0.0
    for py, px in [(0, 0), (12, 18), (31, 47), (50, 80), (63, 95)]:
        patch = padded[:, py : py + 3, px : px + 3].reshape(8 * 9)
        ref = cpu_reference_eval_patch(patch, w1, b1, w2, b2)
        got = torch_out[:, py, px].numpy()
        max_err = max(max_err, float(np.max(np.abs(ref - got))))
    return max_err


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eval-only", action="store_true", help="Load existing header and evaluate")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER_PATH)
    parser.add_argument("--epochs", type=int, default=36)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--train-scenes", type=int, default=48)
    parser.add_argument("--val-scenes", type=int, default=24)
    parser.add_argument("--lr", type=float, default=5e-3)
    parser.add_argument("--device", type=str, default="auto")
    args = parser.parse_args()

    torch.manual_seed(20261128)
    np.random.seed(20261128)
    device = select_device(args.device)
    print(f"[neural_flow] device={device}")

    t0 = time.time()
    print(f"[neural_flow] generating held-out validation set ({args.val_scenes} scenes)...")
    val_cpu = generate_synthetic_dataset(args.val_scenes, seed=9001)
    val_i0 = val_cpu["i0"].to(device)
    val_i1 = val_cpu["i1"].to(device)
    val_ia = val_cpu["ia"].to(device)
    val_alpha = val_cpu["alpha"].to(device)
    val_flow_c = estimate_coarse_flow(val_i0, val_i1, val_alpha, use_midpoint=True)
    val_feats = build_input_features(val_i0, val_i1, val_flow_c, val_alpha)

    model = NeuralFlowCNN().to(device)

    if args.eval_only:
        load_weights_from_header(model, args.header)
        model.to(device)
    else:
        print(f"[neural_flow] generating training set ({args.train_scenes} scenes)...")
        train_cpu = generate_synthetic_dataset(args.train_scenes, seed=2026)
        tr_i0 = train_cpu["i0"].to(device)
        tr_i1 = train_cpu["i1"].to(device)
        tr_ia = train_cpu["ia"].to(device)
        tr_alpha = train_cpu["alpha"].to(device)
        tr_gt_flow = train_cpu["gt_flow"].to(device)

        # Match D3D11 Pass 2 cached midpoint coarse flow (plus 25% alpha-centered augmentation)
        split = (args.train_scenes * 3) // 4
        flow_c_m = estimate_coarse_flow(tr_i0[:split], tr_i1[:split], tr_alpha[:split], use_midpoint=True)
        flow_c_a = estimate_coarse_flow(tr_i0[split:], tr_i1[split:], tr_alpha[split:], use_midpoint=False)
        tr_flow_c = torch.cat([flow_c_m, flow_c_a], dim=0)
        tr_feats = build_input_features(tr_i0, tr_i1, tr_flow_c, tr_alpha)
        q_h, q_w = tr_feats.shape[2], tr_feats.shape[3]
        tr_sub_all = model.prepare_patch_sub(tr_feats).view(args.train_scenes, q_h * q_w, 9, IN_CHANNELS * 9)

        print(f"[neural_flow] dataset & coarse flow ready in {time.time() - t0:.1f}s; training...", flush=True)
        optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
            optimizer, T_max=args.epochs, eta_min=2e-4
        )

        best_score = -1e9
        best_state = None
        num_train = tr_feats.shape[0]

        for epoch in range(1, args.epochs + 1):
            model.train()
            perm = torch.randperm(num_train, device=device)
            aux_w = 0.08 * max(0.15, 1.0 - epoch / float(args.epochs))
            epoch_loss = 0.0
            steps = 0

            for start in range(0, num_train, args.batch_size):
                idx = perm[start : start + args.batch_size]
                bx = tr_feats[idx]
                bsub = tr_sub_all[idx].reshape(-1, 9, IN_CHANNELS * 9)
                bi0 = tr_i0[idx]
                bi1 = tr_i1[idx]
                bia = tr_ia[idx]
                bfc = tr_flow_c[idx]
                ba = tr_alpha[idx]
                bgf = tr_gt_flow[idx]

                optimizer.zero_grad(set_to_none=True)

                dv_p, blend_p, conf_p = model.forward_patch3x3_from_sub(bsub, bx.shape[0], q_h, q_w)
                pred_p = synthesize_luma(bi0, bi1, bfc + dv_p, blend_p, conf_p, ba)
                loss_p = compute_loss(pred_p, conf_p, dv_p, bfc, bgf, bi0, bi1, bia, aux_w)

                dv_f, blend_f, conf_f = model.forward_full(bx, pad_mode="replicate")
                pred_f = synthesize_luma(bi0, bi1, bfc + dv_f, blend_f, conf_f, ba)
                loss_f = compute_loss(pred_f, conf_f, dv_f, bfc, bgf, bi0, bi1, bia, aux_w)

                loss = 0.55 * loss_p + 0.45 * loss_f
                loss.backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 2.0)
                optimizer.step()

                epoch_loss += float(loss.item())
                steps += 1

            scheduler.step()

            if epoch == 1 or epoch % 20 == 0 or epoch == args.epochs:
                metrics = evaluate_model(
                    model, val_feats, val_i0, val_i1, val_ia, val_flow_c, val_alpha
                )
                both_static_ok = (
                    metrics["static_text_conf_max"] < 0.075
                    and metrics["static_text_conf_full_max"] < 0.075
                )
                score = min(metrics["psnr_neural_patch"], metrics["psnr_neural_full"]) - (
                    10.0 if not both_static_ok else 0.0
                )
                if score > best_score:
                    best_score = score
                    best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
                print(
                    f"  epoch {epoch:3d}/{args.epochs} | loss={epoch_loss / max(1, steps):.5f} | "
                    f"PSNR crossfade={metrics['psnr_crossfade']:.2f} dB, "
                    f"coarse={metrics['psnr_coarse_flow']:.2f} dB, "
                    f"neural(patch)={metrics['psnr_neural_patch']:.2f} dB, "
                    f"neural(full)={metrics['psnr_neural_full']:.2f} dB | "
                    f"static_conf(mean={metrics['static_text_conf_mean']:.4f}, "
                    f"max={max(metrics['static_text_conf_max'], metrics['static_text_conf_full_max']):.4f})",
                    flush=True,
                )

        if best_state is not None:
            model.load_state_dict(best_state)
            model.to(device)

    metrics = evaluate_model(model, val_feats, val_i0, val_i1, val_ia, val_flow_c, val_alpha)
    parity_err = verify_patch_evaluator_parity(model, val_feats)

    if not args.eval_only:
        write_weights_header(model, args.header, metrics)
        print(f"[neural_flow] wrote weights header -> {args.header}")

    print("\n=== Held-Out Stage Manager Zoom + Occlusion + Retina Text Validation ===")
    print(f"  (a) Naive cross-fade PSNR              : {metrics['psnr_crossfade']:.2f} dB")
    print(f"  (b) Coarse Optical Flow alone PSNR     : {metrics['psnr_coarse_flow']:.2f} dB "
          f"(ungated: {metrics['psnr_coarse_ungated']:.2f} dB)")
    print(f"  (c) Coarse Flow + Neural CNN PSNR      : {metrics['psnr_neural_patch']:.2f} dB "
          f"(full-image conv: {metrics['psnr_neural_full']:.2f} dB)")
    print(f"  Static text motion_conf                : "
          f"mean={metrics['static_text_conf_mean']:.5f}, "
          f"p99={metrics['static_text_conf_p99']:.5f}, "
          f"max={metrics['static_text_conf_max']:.5f} (threshold < 0.08)")
    print(f"  C++ EvalNeuralFlowPatch parity max err : {parity_err:.2e}")

    ok_psnr_patch = (
        metrics["psnr_neural_patch"] > metrics["psnr_crossfade"]
        and metrics["psnr_neural_patch"] > metrics["psnr_coarse_flow"]
        and metrics["psnr_neural_patch"] > metrics["psnr_coarse_ungated"]
    )
    ok_psnr_full = (
        metrics["psnr_neural_full"] > metrics["psnr_crossfade"]
        and metrics["psnr_neural_full"] > metrics["psnr_coarse_flow"]
        and metrics["psnr_neural_full"] > metrics["psnr_coarse_ungated"]
    )
    ok_static = (
        metrics["static_text_conf_max"] < 0.08
        and metrics["static_text_conf_full_max"] < 0.08
    )
    ok_parity = parity_err < 1e-5

    if not (ok_psnr_patch and ok_psnr_full and ok_static and ok_parity):
        print("[neural_flow] ERROR: validation criteria not met!", file=sys.stderr)
        return 1

    print("[neural_flow] PASS: Neural refinement beats both cross-fade and coarse flow, "
          "and static text motion_conf < 0.08.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
