// Draws the badge's e-ink screen with the badge's own font bytes and layout (B6).
// Layout constants mirror firmware/core/src/layout.cpp (via firmware/tools/bdf_to_c.py):
// 16 px margin, 40 px status bar, divider at 44, body from 56 in 16 x 32 glyphs, a 36 px footer
// in 12 x 24 glyphs. Landscape 800 x 480 gives 48 columns by 12 lines, portrait 480 x 800 gives
// 28 by 22: the text budget in BRIEFING section 2.
import { FONTS } from "./glyphs.js";

const MARGIN = 16, STATUS_H = 40, DIVIDER_Y = 44, BODY_TOP = 56, FOOTER_H = 36;
export function size(orientation) {
  const [W, H] = orientation === "portrait" ? [480, 800] : [800, 480];
  return { W, H, cols: Math.floor((W - 2 * MARGIN) / FONTS.body.w), rows: Math.floor((H - BODY_TOP - FOOTER_H) / FONTS.body.h) };
}

const decoded = {};
for (const [name, f] of Object.entries(FONTS)) {
  decoded[name] = Uint8Array.from(atob(f.data), (c) => c.charCodeAt(0));
}

function css(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function text(ctx, s, x, y, fontName, ink) {
  const f = FONTS[fontName], bytes = decoded[fontName];
  ctx.fillStyle = ink;
  for (let i = 0; i < s.length; i++) {
    let code = s.charCodeAt(i);
    if (code < f.first || code > f.last) code = 63; // '?': the badge font is ASCII only
    const base = (code - f.first) * f.h * f.stride;
    for (let ry = 0; ry < f.h; ry++) {
      let row = 0;
      for (let b = 0; b < f.stride; b++) row = (row << 8) | bytes[base + ry * f.stride + b];
      for (let bx = 0; bx < f.w; bx++) {
        if ((row >> (f.stride * 8 - 1 - bx)) & 1) ctx.fillRect(x + i * f.w + bx, y + ry, 1, 1);
      }
    }
  }
}

function fit(s, cols) {
  const t = String(s).replace(/[^\x20-\x7e]/g, "?");
  return t.length > cols ? t.slice(0, cols - 3) + "..." : t;
}

// screen = { status, right, lines: [string], highlight: index or -1, footer }
export function draw(canvas, screen, orientation = "landscape") {
  const { W, H, cols: COLS, rows: ROWS } = size(orientation);
  if (canvas.width !== W) { canvas.width = W; canvas.height = H; }
  canvas.classList.toggle("portrait", orientation === "portrait");
  const ctx = canvas.getContext("2d");
  const paper = css("--paper"), ink = css("--ink");
  ctx.fillStyle = paper;
  ctx.fillRect(0, 0, W, H);
  const sf = FONTS.status;
  const statusCols = Math.floor((W - 2 * MARGIN) / sf.w);
  const right = screen.right ? fit(screen.right, 12) : "";
  text(ctx, fit(screen.status || "", statusCols - right.length - 1), MARGIN, (STATUS_H - sf.h) / 2, "status", ink);
  if (right) text(ctx, right, W - MARGIN - right.length * sf.w, (STATUS_H - sf.h) / 2, "status", ink);
  ctx.fillStyle = ink;
  ctx.fillRect(MARGIN, DIVIDER_Y, W - 2 * MARGIN, 2);

  const lines = screen.lines || [];
  const hi = screen.highlight ?? -1;
  // Keep the highlighted row on screen (rotary scrolling), otherwise show from the top.
  const first = hi >= ROWS ? hi - ROWS + 1 : 0;
  for (let r = 0; r < ROWS && first + r < lines.length; r++) {
    const y = BODY_TOP + r * FONTS.body.h;
    if (first + r === hi) {
      ctx.fillStyle = ink;
      ctx.fillRect(MARGIN - 4, y, W - 2 * MARGIN + 8, FONTS.body.h);
      text(ctx, fit(lines[first + r], COLS), MARGIN, y, "body", paper);
    } else {
      text(ctx, fit(lines[first + r], COLS), MARGIN, y, "body", ink);
    }
  }
  if (screen.footer) text(ctx, fit(screen.footer, statusCols), MARGIN, H - FOOTER_H + 4, "status", ink);
}
