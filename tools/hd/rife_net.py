"""RIFE v4.26 frame-interpolation network (IFNet) for the in-between frame experiments.

Architecture and trained weights are the work of the RIFE authors:
    "Real-Time Intermediate Flow Estimation for Video Frame Interpolation" (ECCV 2022),
    Practical-RIFE, https://github.com/hzwer/Practical-RIFE - Copyright (c) hzwer and the RIFE
    authors, MIT License ("Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software ... to deal in the Software without restriction ..."; full text in that
    repository).
This file is a re-implementation written for this project so that NO downloaded code is run: only
the weights file is used (flownet.pkl out of RIFEv4.26_0921.zip from https://huggingface.co/hzwer/RIFE,
kept in hd\\models_vfi\\rife_v4.26_dl\\), and it is read with torch.load(weights_only=True), which
accepts tensors and nothing else. Parameter names follow the weights file.

What the network does: given two pictures and a time t in 0..1 it estimates, coarse to fine over
five scales, where every pixel of the in-between picture came from in each of the two (two flow
fields) and how much to trust either (a mask), then moves both pictures along the flows and mixes
them. interpolate() also moves any extra planes (alpha, shadow) the same way, which a ready-made
tool would not let us do.
"""
from pathlib import Path

import torch
import torch.nn as nn
import torch.nn.functional as F


def _conv(cin: int, cout: int, stride: int) -> nn.Sequential:
    return nn.Sequential(nn.Conv2d(cin, cout, 3, stride, 1, bias=True), nn.LeakyReLU(0.2, True))


class Head(nn.Module):
    """Small feature extractor: 4 feature planes at the picture's resolution."""

    def __init__(self):
        super().__init__()
        self.cnn0 = nn.Conv2d(3, 16, 3, 2, 1)
        self.cnn1 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn2 = nn.Conv2d(16, 16, 3, 1, 1)
        self.cnn3 = nn.ConvTranspose2d(16, 4, 4, 2, 1)
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        x = self.relu(self.cnn0(x))
        x = self.relu(self.cnn1(x))
        x = self.relu(self.cnn2(x))
        return self.cnn3(x)


class ResConv(nn.Module):
    def __init__(self, c: int):
        super().__init__()
        self.conv = nn.Conv2d(c, c, 3, 1, 1)
        self.beta = nn.Parameter(torch.ones((1, c, 1, 1)))
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        return self.relu(self.conv(x) * self.beta + x)


class IFBlock(nn.Module):
    """One refinement step, run at 1/scale of the picture: returns a flow update (4 planes: two
    2-D flows), the mixing mask (1 plane) and 8 feature planes handed to the next step."""

    def __init__(self, in_planes: int, c: int):
        super().__init__()
        self.conv0 = nn.Sequential(_conv(in_planes, c // 2, 2), _conv(c // 2, c, 2))
        self.convblock = nn.Sequential(*[ResConv(c) for _ in range(8)])
        self.lastconv = nn.Sequential(nn.ConvTranspose2d(c, 4 * 13, 4, 2, 1), nn.PixelShuffle(2))

    def forward(self, x, flow, scale: float):
        x = F.interpolate(x, scale_factor=1.0 / scale, mode="bilinear", align_corners=False)
        if flow is not None:
            flow = F.interpolate(flow, scale_factor=1.0 / scale, mode="bilinear", align_corners=False) / scale
            x = torch.cat((x, flow), 1)
        out = self.lastconv(self.convblock(self.conv0(x)))
        out = F.interpolate(out, scale_factor=scale, mode="bilinear", align_corners=False)
        return out[:, :4] * scale, out[:, 4:5], out[:, 5:]


def warp(img: torch.Tensor, flow: torch.Tensor) -> torch.Tensor:
    """Fetch every pixel from where `flow` (in pixels) says it came from."""
    n, _, h, w = img.shape
    xs = torch.linspace(-1.0, 1.0, w, device=img.device, dtype=img.dtype).view(1, 1, 1, w).expand(n, 1, h, w)
    ys = torch.linspace(-1.0, 1.0, h, device=img.device, dtype=img.dtype).view(1, 1, h, 1).expand(n, 1, h, w)
    grid = torch.cat((xs + flow[:, 0:1] / ((w - 1.0) / 2.0), ys + flow[:, 1:2] / ((h - 1.0) / 2.0)), 1)
    return F.grid_sample(img, grid.permute(0, 2, 3, 1), mode="bilinear", padding_mode="border", align_corners=True)


class IFNet(nn.Module):
    SCALES = (16.0, 8.0, 4.0, 2.0, 1.0)

    def __init__(self):
        super().__init__()
        self.block0 = IFBlock(3 + 3 + 4 + 4 + 1, c=192)
        self.block1 = IFBlock(3 + 3 + 4 + 4 + 1 + 1 + 8 + 4, c=128)
        self.block2 = IFBlock(3 + 3 + 4 + 4 + 1 + 1 + 8 + 4, c=96)
        self.block3 = IFBlock(3 + 3 + 4 + 4 + 1 + 1 + 8 + 4, c=64)
        self.block4 = IFBlock(3 + 3 + 4 + 4 + 1 + 1 + 8 + 4, c=32)
        self.encode = Head()

    def forward(self, img0, img1, t: float = 0.5, scales=None):
        """img0, img1: (N, 3, H, W) in 0..1 with H and W multiples of 64.
        Returns (in-between picture, flow (N, 4, H, W), mask (N, 1, H, W) = weight of img0)."""
        scales = scales or self.SCALES
        time = torch.full_like(img0[:, :1], t)
        f0, f1 = self.encode(img0), self.encode(img1)
        flow = mask = feat = None
        w0, w1 = img0, img1
        for block, scale in zip((self.block0, self.block1, self.block2, self.block3, self.block4), scales):
            if flow is None:
                flow, mask, feat = block(torch.cat((img0, img1, f0, f1, time), 1), None, scale)
            else:
                x = torch.cat((w0, w1, warp(f0, flow[:, :2]), warp(f1, flow[:, 2:4]), time, mask, feat), 1)
                delta, mask, feat = block(x, flow, scale)
                flow = flow + delta
            w0, w1 = warp(img0, flow[:, :2]), warp(img1, flow[:, 2:4])
        mask = torch.sigmoid(mask)
        return w0 * mask + w1 * (1 - mask), flow, mask


def load(weights: Path, device: str = "cuda") -> IFNet:
    state = torch.load(str(weights), map_location="cpu", weights_only=True)  # tensors only: no code can run
    state = {(k[7:] if k.startswith("module.") else k): v for k, v in state.items()}
    net = IFNet()
    own = set(net.state_dict().keys())
    missing = sorted(own - set(state.keys()))
    if missing:
        raise RuntimeError(f"weights file lacks {len(missing)} tensors of the network, e.g. {missing[:4]}")
    for k in own:
        if state[k].shape != net.state_dict()[k].shape:
            raise RuntimeError(f"tensor {k}: file has {tuple(state[k].shape)}, network wants {tuple(net.state_dict()[k].shape)}")
    net.load_state_dict({k: state[k] for k in own}, strict=True)
    return net.eval().to(device)


@torch.inference_mode()
def interpolate(net: IFNet, img0, img1, t: float = 0.5, extra0=None, extra1=None, scales=None):
    """img0/img1: float tensors (3, H, W) in 0..1 on the network's device, any size. extra0/extra1:
    optional (C, H, W) planes (alpha, shadow ...) that are moved and mixed exactly like the picture.
    Returns (picture (3, H, W), extra (C, H, W) or None, flow (4, H, W), mask (1, H, W))."""
    _, h, w = img0.shape
    ph, pw = (64 - h % 64) % 64, (64 - w % 64) % 64
    a = F.pad(img0[None], (0, pw, 0, ph), mode="replicate")
    b = F.pad(img1[None], (0, pw, 0, ph), mode="replicate")
    mid, flow, mask = net(a, b, t, scales)
    extra = None
    if extra0 is not None:
        e0 = F.pad(extra0[None], (0, pw, 0, ph), mode="replicate")
        e1 = F.pad(extra1[None], (0, pw, 0, ph), mode="replicate")
        extra = (warp(e0, flow[:, :2]) * mask + warp(e1, flow[:, 2:4]) * (1 - mask))[0, :, :h, :w]
    return mid[0, :, :h, :w].clamp(0, 1), extra, flow[0, :, :h, :w], mask[0, :, :h, :w]
