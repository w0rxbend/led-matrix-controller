#!/usr/bin/env python3
"""Generate and upload one custom animation slot.

The firmware accepts one custom animation slot. Each uploaded frame payload is:

  frame_index, frame_count, delay_lsb, delay_msb, 192 RGB bytes

The 192 RGB bytes are physical LED order. This example still works with
display-space x/y coordinates; every custom frame is rotated -90 degrees, then
matrix_client applies the soldered-panel row compensation before packing each
pixel into the payload.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from matrix_client import (  # noqa: E402
    COMMANDS,
    MatrixClient,
    display_physical_index,
    report_status,
    rotate_point,
)


WIDTH = 8
HEIGHT = 8
LED_COUNT = WIDTH * HEIGHT
FRAME_BYTES = LED_COUNT * 3
CUSTOM_FRAME_COUNT = 8
CUSTOM_ROTATION = -90


def clamp_byte(value: int) -> int:
    return max(0, min(255, value))


def empty_frame() -> bytearray:
    return bytearray(FRAME_BYTES)


def set_pixel(frame: bytearray, x: int, y: int, red: int, green: int, blue: int) -> None:
    if not 0 <= x < WIDTH or not 0 <= y < HEIGHT:
        return

    rotated_x, rotated_y = rotate_point(x, y, CUSTOM_ROTATION)
    base = display_physical_index(rotated_x, rotated_y) * 3
    frame[base] = clamp_byte(red)
    frame[base + 1] = clamp_byte(green)
    frame[base + 2] = clamp_byte(blue)


def pseudo_noise(x: int, y: int, frame_index: int, seed: int = 0) -> int:
    value = (x * 37) ^ (y * 67) ^ (frame_index * 97) ^ (seed * 131)
    value = (value * 1103515245 + 12345) & 0x7FFFFFFF
    return (value >> 8) & 0xFF


def heat_color(heat: int) -> tuple[int, int, int]:
    heat = clamp_byte(heat)
    if heat < 70:
        return heat * 3, 0, 0
    if heat < 150:
        return 255, (heat - 70) * 3, 0
    return 255, min(255, 120 + (heat - 150) * 2), min(90, (heat - 150) // 2)


def tint_color(red: int, green: int, blue: int, brightness: int) -> tuple[int, int, int]:
    return (
        (red * brightness) // 255,
        (green * brightness) // 255,
        (blue * brightness) // 255,
    )


def make_bouncing_dot_frames(red: int, green: int, blue: int) -> list[bytes]:
    positions = [
        (0, 0),
        (1, 1),
        (2, 2),
        (3, 3),
        (4, 4),
        (5, 5),
        (6, 6),
        (7, 7),
        (6, 6),
        (5, 5),
        (4, 4),
        (3, 3),
        (2, 2),
        (1, 1),
    ]

    frames: list[bytes] = []
    for x, y in positions:
        frame = empty_frame()
        set_pixel(frame, x, y, red, green, blue)
        frames.append(bytes(frame))
    return frames


def make_wipe_frames(red: int, green: int, blue: int) -> list[bytes]:
    frames: list[bytes] = []
    for count in range(1, LED_COUNT + 1):
        frame = empty_frame()
        for display_index in range(count):
            x = display_index % WIDTH
            y = display_index // WIDTH
            set_pixel(frame, x, y, red, green, blue)
        frames.append(bytes(frame))
    return frames


def make_fire_frames() -> list[bytes]:
    frames: list[bytes] = []
    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                height_heat = 235 - ((HEIGHT - 1 - y) * 31)
                flicker = pseudo_noise(x, y, frame_index, seed=11) // 4
                wave = int(
                    math.sin((x * 0.9) + (frame_index * 0.8) + (y * 0.7)) * 28
                )
                heat = height_heat + flicker + wave
                set_pixel(frame, x, y, *heat_color(heat))
        frames.append(bytes(frame))
    return frames


def make_flame_frames() -> list[bytes]:
    frames: list[bytes] = []
    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for y in range(HEIGHT):
            center = 3.5 + math.sin((frame_index * 0.9) + (y * 0.8)) * 0.75
            radius = 0.65 + (y * 0.42)
            vertical_cooling = (HEIGHT - 1 - y) * 24
            for x in range(WIDTH):
                distance = abs(x - center)
                edge_cooling = int((distance / max(radius, 0.1)) * 170)
                flicker = pseudo_noise(x, y, frame_index, seed=23) // 5
                heat = 250 - vertical_cooling - edge_cooling + flicker
                set_pixel(frame, x, y, *heat_color(heat))
        frames.append(bytes(frame))
    return frames


def make_stardust_frames(red: int, green: int, blue: int) -> list[bytes]:
    frames: list[bytes] = []
    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                phase = (x * 5 + y * 3 + frame_index) % CUSTOM_FRAME_COUNT
                sparkle = pseudo_noise(x, y, phase, seed=41)
                if sparkle > 218:
                    brightness = 180 + ((sparkle - 218) * 2)
                elif sparkle > 192:
                    brightness = 70
                else:
                    brightness = 8 if (x + y + frame_index) % 7 == 0 else 0

                if brightness > 0:
                    star_red, star_green, star_blue = tint_color(
                        red, green, blue, brightness
                    )
                    white = brightness // 3
                    set_pixel(
                        frame,
                        x,
                        y,
                        star_red + white,
                        star_green + white,
                        star_blue + white,
                    )
        frames.append(bytes(frame))
    return frames


def make_warp_frames(red: int, green: int, blue: int) -> list[bytes]:
    frames: list[bytes] = []
    center_x = (WIDTH - 1) / 2
    center_y = (HEIGHT - 1) / 2
    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                dx = x - center_x
                dy = y - center_y
                distance = math.sqrt((dx * dx) + (dy * dy))
                spoke = (abs(dx) * 4.0 + abs(dy) * 2.5 + frame_index * 1.7) % 6.0
                ring = (distance * 3.2 - frame_index * 1.4) % 5.0
                brightness = 0
                if spoke < 1.15:
                    brightness = max(brightness, int(230 - distance * 24))
                if ring < 0.85:
                    brightness = max(brightness, int(180 - distance * 18))
                if distance < 1.0:
                    brightness = max(brightness, 210)

                set_pixel(
                    frame,
                    x,
                    y,
                    *tint_color(red, green, blue, clamp_byte(brightness)),
                )
        frames.append(bytes(frame))
    return frames


def make_bell_frames(red: int, green: int, blue: int) -> list[bytes]:
    bell_pixels = (
        (3, 1),
        (4, 1),
        (2, 2),
        (3, 2),
        (4, 2),
        (5, 2),
        (2, 3),
        (3, 3),
        (4, 3),
        (5, 3),
        (1, 4),
        (2, 4),
        (3, 4),
        (4, 4),
        (5, 4),
        (6, 4),
        (1, 5),
        (2, 5),
        (3, 5),
        (4, 5),
        (5, 5),
        (6, 5),
        (2, 6),
        (3, 6),
        (4, 6),
        (5, 6),
        (3, 7),
        (4, 7),
    )
    motion_frames = (-1, 0, 1, 0, -1, 0, 1, 0)
    frames: list[bytes] = []

    for frame_index, shift in enumerate(motion_frames):
        frame = empty_frame()
        brightness = 210 if frame_index % 2 == 0 else 255

        for x, y in bell_pixels:
            pixel_red, pixel_green, pixel_blue = tint_color(red, green, blue, brightness)
            set_pixel(frame, x + shift, y, pixel_red, pixel_green, pixel_blue)

        clapper_x = 3 + (1 if shift > 0 else 0 if shift == 0 else -1)
        set_pixel(frame, clapper_x, 7, 255, 245, 180)

        if shift <= 0:
            set_pixel(frame, 0, 2, 255, 230, 110)
            set_pixel(frame, 0, 4, 220, 170, 40)
        if shift >= 0:
            set_pixel(frame, 7, 2, 255, 230, 110)
            set_pixel(frame, 7, 4, 220, 170, 40)

        frames.append(bytes(frame))
    return frames


def make_notification_frames(red: int, green: int, blue: int) -> list[bytes]:
    mark_pixels = (
        (3, 1),
        (4, 1),
        (3, 2),
        (4, 2),
        (3, 3),
        (4, 3),
        (3, 5),
        (4, 5),
    )
    pulse_levels = (80, 150, 255, 130, 70, 210, 255, 100)
    frames: list[bytes] = []

    for frame_index, brightness in enumerate(pulse_levels):
        frame = empty_frame()
        mark_red, mark_green, mark_blue = tint_color(red, green, blue, brightness)

        for x, y in mark_pixels:
            set_pixel(frame, x, y, mark_red, mark_green, mark_blue)

        if frame_index in (1, 2, 5, 6):
            accent = 120 if frame_index in (1, 5) else 220
            accent_red, accent_green, accent_blue = tint_color(red, green, blue, accent)
            set_pixel(frame, 2, 0, accent_red, accent_green, accent_blue)
            set_pixel(frame, 5, 0, accent_red, accent_green, accent_blue)
            set_pixel(frame, 1, 2, accent_red, accent_green, accent_blue)
            set_pixel(frame, 6, 2, accent_red, accent_green, accent_blue)

        if frame_index in (2, 6):
            set_pixel(frame, 0, 4, 255, 255, 180)
            set_pixel(frame, 7, 4, 255, 255, 180)
            set_pixel(frame, 2, 7, 255, 255, 180)
            set_pixel(frame, 5, 7, 255, 255, 180)

        frames.append(bytes(frame))
    return frames


def make_rocket_frames(red: int, green: int, blue: int) -> list[bytes]:
    body_pixels = (
        (3, 0),
        (4, 0),
        (2, 1),
        (3, 1),
        (4, 1),
        (5, 1),
        (2, 2),
        (3, 2),
        (4, 2),
        (5, 2),
        (2, 3),
        (3, 3),
        (4, 3),
        (5, 3),
        (1, 4),
        (2, 4),
        (5, 4),
        (6, 4),
    )
    window_pixels = ((3, 2), (4, 2))
    frames: list[bytes] = []

    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        y_offset = -1 if frame_index in (2, 3, 6, 7) else 0
        body_brightness = 210 if frame_index % 2 else 255
        body_red, body_green, body_blue = tint_color(red, green, blue, body_brightness)

        for x, y in body_pixels:
            set_pixel(frame, x, y + y_offset, body_red, body_green, body_blue)

        for x, y in window_pixels:
            set_pixel(frame, x, y + y_offset, 80, 220, 255)

        flame_height = 2 + (frame_index % 3)
        flame_top = 5 + y_offset
        for step in range(flame_height):
            heat = 255 - (step * 55)
            flame_width = 1 if step == flame_height - 1 else 2
            for x in range(4 - flame_width, 4 + flame_width):
                set_pixel(frame, x, flame_top + step, *heat_color(heat))

        if frame_index % 2 == 0:
            set_pixel(frame, 2, 7, 80, 80, 80)
            set_pixel(frame, 5, 7, 80, 80, 80)
        else:
            set_pixel(frame, 1, 7, 60, 60, 60)
            set_pixel(frame, 6, 7, 60, 60, 60)

        frames.append(bytes(frame))
    return frames


def make_air_raid_alert_frames(red: int, green: int, blue: int) -> list[bytes]:
    beacon_pixels = (
        (3, 2),
        (4, 2),
        (2, 3),
        (3, 3),
        (4, 3),
        (5, 3),
        (2, 4),
        (3, 4),
        (4, 4),
        (5, 4),
        (3, 5),
        (4, 5),
    )
    sweep_frames = (
        ((0, 0), (1, 1), (6, 6), (7, 7)),
        ((0, 1), (1, 2), (6, 5), (7, 6)),
        ((0, 3), (1, 3), (6, 4), (7, 4)),
        ((0, 5), (1, 4), (6, 3), (7, 2)),
        ((0, 7), (1, 6), (6, 1), (7, 0)),
        ((0, 5), (1, 4), (6, 3), (7, 2)),
        ((0, 3), (1, 3), (6, 4), (7, 4)),
        ((0, 1), (1, 2), (6, 5), (7, 6)),
    )
    pulse_levels = (180, 255, 120, 255, 180, 255, 120, 255)
    frames: list[bytes] = []

    for frame_index, sweep_pixels in enumerate(sweep_frames):
        frame = empty_frame()
        beacon_red, beacon_green, beacon_blue = tint_color(
            red, green, blue, pulse_levels[frame_index]
        )

        for x, y in beacon_pixels:
            set_pixel(frame, x, y, beacon_red, beacon_green, beacon_blue)

        for x, y in sweep_pixels:
            set_pixel(frame, x, y, 255, 40, 20)

        if frame_index % 2 == 1:
            for x in range(WIDTH):
                set_pixel(frame, x, 0, 90, 0, 0)
                set_pixel(frame, x, 7, 90, 0, 0)

        frames.append(bytes(frame))
    return frames


def make_telegram_frames(red: int, green: int, blue: int) -> list[bytes]:
    bubble_pixels = (
        (2, 1),
        (3, 1),
        (4, 1),
        (5, 1),
        (1, 2),
        (2, 2),
        (3, 2),
        (4, 2),
        (5, 2),
        (6, 2),
        (1, 3),
        (2, 3),
        (3, 3),
        (4, 3),
        (5, 3),
        (6, 3),
        (1, 4),
        (2, 4),
        (3, 4),
        (4, 4),
        (5, 4),
        (6, 4),
        (2, 5),
        (3, 5),
        (4, 5),
        (5, 5),
        (5, 6),
    )
    plane_pixels = (
        (2, 3),
        (3, 3),
        (4, 3),
        (5, 2),
        (4, 4),
        (3, 5),
    )
    pulse_levels = (70, 130, 255, 190, 120, 230, 150, 90)
    frames: list[bytes] = []

    for frame_index, brightness in enumerate(pulse_levels):
        frame = empty_frame()
        bubble_red, bubble_green, bubble_blue = tint_color(red, green, blue, brightness)

        y_shift = -1 if frame_index in (1, 2) else 0
        for x, y in bubble_pixels:
            set_pixel(frame, x, y + y_shift, bubble_red, bubble_green, bubble_blue)

        for x, y in plane_pixels:
            set_pixel(frame, x, y + y_shift, 230, 250, 255)

        if frame_index in (0, 1, 2, 5):
            set_pixel(frame, 0, 0, 40, 170, 255)
            set_pixel(frame, 7, 0, 40, 170, 255)
        if frame_index in (2, 5):
            set_pixel(frame, 0, 7, 150, 220, 255)
            set_pixel(frame, 7, 7, 150, 220, 255)

        frames.append(bytes(frame))
    return frames


def make_scala_logo_frames(red: int, green: int, blue: int) -> list[bytes]:
    logo_rows = (
        "01111110",
        "11000000",
        "11000000",
        "01111100",
        "00000110",
        "00000110",
        "11111100",
        "00000000",
    )
    shine_frames = (0, 1, 2, 3, 4, 5, 6, 7)
    frames: list[bytes] = []

    for frame_index, shine_column in enumerate(shine_frames):
        frame = empty_frame()
        brightness = 180 + ((frame_index % 4) * 20)

        for y, row in enumerate(logo_rows):
            for x, value in enumerate(row):
                if value != "1":
                    continue

                pixel_red, pixel_green, pixel_blue = tint_color(
                    red, green, blue, brightness
                )
                if x == shine_column or x == shine_column - 1:
                    pixel_red = min(255, pixel_red + 70)
                    pixel_green = min(255, pixel_green + 70)
                    pixel_blue = min(255, pixel_blue + 70)
                set_pixel(frame, x, y, pixel_red, pixel_green, pixel_blue)

        frames.append(bytes(frame))
    return frames


def make_ubuntu_logo_frames(red: int, green: int, blue: int) -> list[bytes]:
    ring_pixels = (
        (3, 1),
        (4, 1),
        (2, 2),
        (5, 2),
        (1, 3),
        (6, 3),
        (1, 4),
        (6, 4),
        (2, 5),
        (5, 5),
        (3, 6),
        (4, 6),
    )
    friend_pixels = (
        (3, 0),
        (4, 0),
        (0, 3),
        (0, 4),
        (6, 6),
        (7, 6),
    )
    connector_pixels = (
        (3, 2),
        (2, 3),
        (5, 4),
        (4, 5),
        (2, 4),
        (5, 3),
    )
    pulse_levels = (160, 210, 255, 210, 160, 230, 255, 190)
    frames: list[bytes] = []

    for frame_index, brightness in enumerate(pulse_levels):
        frame = empty_frame()
        logo_red, logo_green, logo_blue = tint_color(red, green, blue, brightness)

        for x, y in ring_pixels:
            set_pixel(frame, x, y, logo_red, logo_green, logo_blue)

        for x, y in connector_pixels:
            set_pixel(frame, x, y, logo_red // 2, logo_green // 2, logo_blue // 2)

        friend_brightness = 255 if frame_index % 2 == 0 else 210
        friend_red, friend_green, friend_blue = tint_color(
            red, green, blue, friend_brightness
        )
        for x, y in friend_pixels:
            set_pixel(frame, x, y, friend_red, friend_green, friend_blue)

        frames.append(bytes(frame))
    return frames


def make_matrix_rain_frames(red: int, green: int, blue: int) -> list[bytes]:
    column_offsets = (0, 3, 6, 1, 5, 2, 7, 4)
    frames: list[bytes] = []

    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for x, offset in enumerate(column_offsets):
            head_y = (frame_index + offset) % HEIGHT
            for tail_step in range(4):
                y = (head_y - tail_step) % HEIGHT
                brightness = 255 - (tail_step * 60)
                rain_red, rain_green, rain_blue = tint_color(
                    red, green, blue, brightness
                )
                if tail_step == 0:
                    rain_red = min(255, rain_red + 80)
                    rain_green = min(255, rain_green + 80)
                    rain_blue = min(255, rain_blue + 80)
                set_pixel(frame, x, y, rain_red, rain_green, rain_blue)

        frames.append(bytes(frame))
    return frames


def make_lofi_frames(red: int, green: int, blue: int) -> list[bytes]:
    frames: list[bytes] = []
    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        for y in range(HEIGHT):
            for x in range(WIDTH):
                horizontal = math.sin((x * 0.7) + (frame_index * 0.55))
                vertical = math.cos((y * 0.65) - (frame_index * 0.45))
                diagonal = math.sin(((x + y) * 0.45) + (frame_index * 0.35))
                haze = int(95 + (horizontal * 34) + (vertical * 28) + (diagonal * 22))
                haze = clamp_byte(haze)

                base_red = (red * min(255, haze + 26)) // 255
                base_green = (green * max(34, haze - 30)) // 255
                base_blue = (blue * min(255, haze + 86)) // 255

                vignette = int((abs(x - 3.5) + abs(y - 3.5)) * 7)
                base_red = max(0, base_red - vignette)
                base_green = max(0, base_green - vignette)
                base_blue = max(0, base_blue - (vignette // 2))

                if (x + (y * 2) + frame_index) % 13 == 0:
                    base_red += 32
                    base_green += 24
                    base_blue += 42

                set_pixel(frame, x, y, base_red, base_green, base_blue)

        frames.append(bytes(frame))
    return frames


def make_robot_face_frames(red: int, green: int, blue: int) -> list[bytes]:
    eye_frames = (
        ((1, 2), (2, 2), (1, 3), (2, 3), (5, 2), (6, 2), (5, 3), (6, 3)),
        ((1, 2), (2, 2), (1, 3), (2, 3), (5, 2), (6, 2), (5, 3), (6, 3)),
        ((1, 2), (2, 2), (1, 3), (2, 3), (5, 2), (6, 2), (5, 3), (6, 3)),
        ((1, 2), (2, 2), (1, 3), (2, 3), (5, 2), (6, 2), (5, 3), (6, 3)),
        ((1, 3), (2, 3), (5, 3), (6, 3)),
        (),
        ((1, 3), (2, 3), (5, 3), (6, 3)),
        ((1, 2), (2, 2), (1, 3), (2, 3), (5, 2), (6, 2), (5, 3), (6, 3)),
    )
    mouth_frames = (
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
        ((2, 5), (3, 6), (4, 6), (5, 5)),
    )
    frames: list[bytes] = []

    for frame_index in range(CUSTOM_FRAME_COUNT):
        frame = empty_frame()
        brightness = 210 if frame_index in (4, 5, 6) else 255
        face_red, face_green, face_blue = tint_color(red, green, blue, brightness)

        for x, y in eye_frames[frame_index]:
            set_pixel(frame, x, y, face_red, face_green, face_blue)

        if not eye_frames[frame_index]:
            set_pixel(frame, 1, 3, face_red, face_green, face_blue)
            set_pixel(frame, 2, 3, face_red, face_green, face_blue)
            set_pixel(frame, 5, 3, face_red, face_green, face_blue)
            set_pixel(frame, 6, 3, face_red, face_green, face_blue)

        for x, y in mouth_frames[frame_index]:
            set_pixel(frame, x, y, face_red, face_green, face_blue)

        frames.append(bytes(frame))
    return frames


def make_eyes_frames(red: int, green: int, blue: int) -> list[bytes]:
    open_eyes = (
        (1, 3),
        (2, 3),
        (1, 4),
        (2, 4),
        (5, 3),
        (6, 3),
        (5, 4),
        (6, 4),
    )
    half_blink = (
        (1, 4),
        (2, 4),
        (5, 4),
        (6, 4),
    )
    eye_frames = (
        open_eyes,
        open_eyes,
        open_eyes,
        half_blink,
        (),
        half_blink,
        open_eyes,
        open_eyes,
    )
    frames: list[bytes] = []

    for frame_index, pixels in enumerate(eye_frames):
        frame = empty_frame()
        brightness = 220 if frame_index in (3, 5) else 255
        eye_red, eye_green, eye_blue = tint_color(red, green, blue, brightness)

        for x, y in pixels:
            set_pixel(frame, x, y, eye_red, eye_green, eye_blue)

        if not pixels:
            set_pixel(frame, 1, 4, eye_red, eye_green, eye_blue)
            set_pixel(frame, 2, 4, eye_red, eye_green, eye_blue)
            set_pixel(frame, 5, 4, eye_red, eye_green, eye_blue)
            set_pixel(frame, 6, 4, eye_red, eye_green, eye_blue)

        frames.append(bytes(frame))
    return frames


def make_sleepy_eyes_frames(red: int, green: int, blue: int) -> list[bytes]:
    open_eyes = (
        (1, 3),
        (2, 3),
        (3, 3),
        (1, 4),
        (2, 4),
        (5, 3),
        (6, 3),
        (7, 3),
        (6, 4),
        (7, 4),
    )
    relaxed_eyes = (
        (1, 3),
        (2, 3),
        (3, 3),
        (2, 4),
        (5, 3),
        (6, 3),
        (7, 3),
        (6, 4),
    )
    closed_eyes = (
        (1, 3),
        (2, 3),
        (3, 3),
        (5, 3),
        (6, 3),
        (7, 3),
    )
    eye_frames = (
        open_eyes,
        open_eyes,
        relaxed_eyes,
        closed_eyes,
        closed_eyes,
        relaxed_eyes,
        open_eyes,
        open_eyes,
    )
    frames: list[bytes] = []

    for frame_index, pixels in enumerate(eye_frames):
        frame = empty_frame()
        brightness = 220 if frame_index in (2, 5) else 255
        eye_red, eye_green, eye_blue = tint_color(red, green, blue, brightness)

        for x, y in pixels:
            set_pixel(frame, x, y, eye_red, eye_green, eye_blue)

        frames.append(bytes(frame))
    return frames


def make_frames(mode: str, red: int, green: int, blue: int) -> list[bytes]:
    if mode == "dot":
        return make_bouncing_dot_frames(red, green, blue)
    if mode == "wipe":
        return make_wipe_frames(red, green, blue)
    if mode == "fire":
        return make_fire_frames()
    if mode == "flame":
        return make_flame_frames()
    if mode == "stardust":
        return make_stardust_frames(red, green, blue)
    if mode == "warp":
        return make_warp_frames(red, green, blue)
    if mode == "bell":
        return make_bell_frames(red, green, blue)
    if mode == "notification":
        return make_notification_frames(red, green, blue)
    if mode == "rocket":
        return make_rocket_frames(red, green, blue)
    if mode == "air_raid_alert":
        return make_air_raid_alert_frames(red, green, blue)
    if mode == "telegram":
        return make_telegram_frames(red, green, blue)
    if mode in ("scala", "scala_logo", "scals", "scals_logo"):
        return make_scala_logo_frames(red, green, blue)
    if mode in ("ubuntu", "ubuntu_logo"):
        return make_ubuntu_logo_frames(red, green, blue)
    if mode in ("matrix_rain", "matrixrain"):
        return make_matrix_rain_frames(red, green, blue)
    if mode in ("lofi", "lofi_ambient"):
        return make_lofi_frames(red, green, blue)
    if mode in ("robot", "robot_face"):
        return make_robot_face_frames(red, green, blue)
    if mode in ("eyes", "blinking_eyes"):
        return make_eyes_frames(red, green, blue)
    if mode in ("sleepy_eyes", "sleepy"):
        return make_sleepy_eyes_frames(red, green, blue)
    raise ValueError(f"unsupported mode: {mode}")


def upload_frame(client: MatrixClient, index: int, count: int, delay_ms: int, frame: bytes) -> int:
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"frame must be {FRAME_BYTES} bytes")

    payload = bytes((index, count, delay_ms & 0xFF, (delay_ms >> 8) & 0xFF)) + frame
    return client.send_command(COMMANDS["upload_custom_frame"], payload)


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload generated custom animation frames")
    parser.add_argument("--host", default="192.168.1.127")
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--delay", type=int, default=120, help="per-frame delay in ms")
    parser.add_argument(
        "--mode",
        choices=[
            "dot",
            "wipe",
            "fire",
            "flame",
            "stardust",
            "warp",
            "bell",
            "notification",
            "rocket",
            "air_raid_alert",
            "telegram",
            "scala",
            "scala_logo",
            "scals",
            "scals_logo",
            "ubuntu",
            "ubuntu_logo",
            "matrix_rain",
            "matrixrain",
            "lofi",
            "lofi_ambient",
            "robot",
            "robot_face",
            "eyes",
            "blinking_eyes",
            "sleepy",
            "sleepy_eyes",
        ],
        default="dot",
    )
    parser.add_argument("--r", type=int, default=0)
    parser.add_argument("--g", type=int, default=180)
    parser.add_argument("--b", type=int, default=255)
    args = parser.parse_args()

    frames = make_frames(args.mode, args.r, args.g, args.b)[:CUSTOM_FRAME_COUNT]

    with MatrixClient(args.host, args.port) as client:
        for index, frame in enumerate(frames):
            status = upload_frame(client, index, len(frames), args.delay, frame)
            report_status(status)
            if status != 0:
                return 1

    print(
        f"uploaded custom animation: mode={args.mode} "
        f"frames={len(frames)} delay={args.delay}ms"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
