# Builds the PlayStation and Switch button prompts from the game's own icons (dumped with ASURA_MODDUMP into mods\dump).
# Each icon keeps its look (the button body, its colours and glow); only the label is replaced: the light letter is erased by filling it
# in from the surrounding body, and the new label (a PlayStation shape or Switch letter, L2 / ZL and so on) is drawn with the original
# letter's gradient. Text-only icons (a glowing "RT" with no button behind it) are rebuilt: new text and a new glow in the old colours.
# Output: assets\prompts\playstation\<hash>.png and assets\prompts\switch\<hash>.png at 4x the original size (the renderer
# replaces any texture whose hash starts the file name; xenos.cpp mods_texture). Game art stays out of git: assets\prompts is ignored.
#
# Usage: tools\make_prompts.ps1 [-Map tools\prompts_map.txt] [-Dump mods\dump] [-Out assets\prompts] [-Preview <png>]
# Map lines: <16-hex hash> <xbox button> [text]    xbox button: A B X Y LB RB LT RT LS RS BACK START; "text" marks a text-only icon.
param([string]$Map = "$PSScriptRoot\prompts_map.txt", [string]$Dump = "$PSScriptRoot\..\mods\dump", [string]$Out = "$PSScriptRoot\..\assets\prompts", [string]$Preview = "")
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Collections.Generic;

public static class Prompts {
    const int Scale = 4;
    static float Lum(Color c) { return 0.299f * c.R + 0.587f * c.G + 0.114f * c.B; }
    // the letter: light, low-saturation pixels with some opacity
    // a letter's darker grey part (the Y icon's letter fades from white to grey): neutral, fairly bright, mostly opaque; grown into from a light piece only
    static bool IsGreyLetter(Color c) { int mn = Math.Min(c.R, Math.Min(c.G, c.B)), mx = Math.Max(c.R, Math.Max(c.G, c.B)); return c.A > 150 && mx - mn <= 12 && (c.R + c.G + c.B) / 3 > 85; }
    // a letter's warm lower part (the orange Y icon's letter fades from white to pale yellow): grown into from a light piece only
    static bool IsWarmLetter(Color c) { return c.A > 150 && c.R > 230 && c.G > 188 && c.B > 25; }
    static bool IsLetter(Color c) { int mn = Math.Min(c.R, Math.Min(c.G, c.B)), mx = Math.Max(c.R, Math.Max(c.G, c.B)); return c.A > 90 && mn > 150 && mx - mn < 120; }

    public static void Make(string src, string dst, string label, bool textOnly) {
        using (var orig = new Bitmap(src)) using (var big = Relabel(orig, label, textOnly)) big.Save(dst, ImageFormat.Png);
    }
    // The menus' button atlas (256x128, 32 px cells): row 0 A B X Y LB RB LT RT, row 1 LS RS dpad START BACK, and the game's own
    // PlayStation glyphs in rows 2 and 3 (cross circle square triangle L1 R1 L2 R2 / L R dpad START SELECT). PlayStation: the game's
    // rows 2-3 copied over rows 0-1. Switch: rows 0-1 relabelled (face buttons greyed, as a Switch's are), at 4x.
    public static void MakeAtlas(string src, string dstPs, string dstSw) {
        using (var a = new Bitmap(src)) {
            int c = a.Width / 8;
            using (var ps = new Bitmap(a)) {
                using (var g = Graphics.FromImage(ps)) { g.CompositingMode = CompositingMode.SourceCopy; g.DrawImage(a, new Rectangle(0, 0, a.Width, 2 * c), new Rectangle(0, 2 * c, a.Width, 2 * c), GraphicsUnit.Pixel); }
                ps.Save(dstPs, ImageFormat.Png);
            }
            // Switch: from the PlayStation shapes (dark disc with a white ring; dark rounded box): blank the symbol, draw the Switch label in white.
            string[] lab = { "B", "A", "Y", "X", "L", "R", "ZL", "ZR" };
            using (var baseAt = new Bitmap(a)) {
                using (var g0 = Graphics.FromImage(baseAt)) { g0.CompositingMode = CompositingMode.SourceCopy; g0.DrawImage(a, new Rectangle(0, 0, a.Width, 2 * c), new Rectangle(0, 2 * c, a.Width, 2 * c), GraphicsUnit.Pixel);
                    g0.DrawImage(a, new Rectangle(3 * c, c, 2 * c, c), new Rectangle(3 * c, c, 2 * c, c), GraphicsUnit.Pixel); }   // START / BACK as the game draws them
                Color disc = a.GetPixel(c + c / 2, 2 * c + c / 2);                   // inside the circle symbol: the disc's own colour
                Color box = a.GetPixel(4 * c + c / 2, 2 * c + (int)(c * 0.3));       // the L1 box above its text
                using (var sw = new Bitmap(a.Width * Scale, a.Height * Scale, PixelFormat.Format32bppArgb))
                using (var g = Graphics.FromImage(sw)) {
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.PixelOffsetMode = PixelOffsetMode.HighQuality; g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.DrawImage(baseAt, new Rectangle(0, 0, sw.Width, sw.Height), 0, 0, a.Width, a.Height, GraphicsUnit.Pixel);
                    float C = c * Scale;
                    for (int i = 0; i < 8; i++) {
                        float x0 = i * C, cx = x0 + C / 2, cy = C / 2;
                        RectangleF area;
                        if (i < 4) { float r = C * 0.37f; using (var b = new SolidBrush(disc)) g.FillEllipse(b, cx - r, cy - r, 2 * r, 2 * r); area = new RectangleF(cx - C * 0.18f, cy - C * 0.18f, C * 0.36f, C * 0.36f); }
                        else { var rc = i < 6 ? new RectangleF(x0 + C * 0.16f, C * 0.30f, C * 0.68f, C * 0.40f) : new RectangleF(x0 + C * 0.16f, C * 0.22f, C * 0.68f, C * 0.62f); using (var b = new SolidBrush(box)) g.FillRectangle(b, rc); area = new RectangleF(rc.X + C * 0.04f, rc.Y + C * 0.03f, rc.Width - C * 0.08f, rc.Height - C * 0.06f); }
                        var path = new GraphicsPath(); path.AddString(lab[i], new FontFamily("Arial Black"), (int)FontStyle.Regular, 100f, new PointF(0, 0), StringFormat.GenericTypographic);
                        var bb = path.GetBounds(); float sc = Math.Min(area.Height / bb.Height, area.Width / bb.Width);
                        var m = new Matrix(); m.Translate(area.X + area.Width / 2, area.Y + area.Height / 2); m.Scale(sc, sc); m.Translate(-(bb.X + bb.Width / 2), -(bb.Y + bb.Height / 2)); path.Transform(m);
                        using (var wb = new SolidBrush(Color.FromArgb(245, 245, 245))) g.FillPath(wb, path);
                    }
                    sw.Save(dstSw, ImageFormat.Png);
                }
            }
        }
    }
    static Bitmap Relabel(Bitmap orig, string label, bool textOnly) {
        {
            int w = orig.Width, h = orig.Height;
            var px = new Color[w, h]; var mask = new bool[w, h];
            int x0 = w, y0 = h, x1 = -1, y1 = -1; double tR = 0, tG = 0, tB = 0, bR = 0, bG = 0, bB = 0; int nt = 0, nb = 0;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { px[x, y] = orig.GetPixel(x, y); }
            // light pieces, as connected components; a piece nearly as wide as the icon is a rim or ring (the Y icon's white ring), not the letter
            int ox0 = w, ox1 = -1;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (px[x, y].A > 60) { ox0 = Math.Min(ox0, x); ox1 = Math.Max(ox1, x); }
            var seen = new bool[w, h];
            for (int sy = 0; sy < h; sy++) for (int sx = 0; sx < w; sx++) {
                if (seen[sx, sy] || !IsLetter(px[sx, sy])) continue;
                var comp = new List<int[]>(); var st = new Stack<int[]>(); st.Push(new int[] { sx, sy }); seen[sx, sy] = true;
                int cx0 = sx, cx1 = sx;
                while (st.Count > 0) { var p = st.Pop(); comp.Add(p); cx0 = Math.Min(cx0, p[0]); cx1 = Math.Max(cx1, p[0]);
                    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = p[0] + dx, yy = p[1] + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h || seen[xx, yy] || !(IsLetter(px[xx, yy]) || IsGreyLetter(px[xx, yy]) || IsWarmLetter(px[xx, yy]))) continue; seen[xx, yy] = true; st.Push(new int[] { xx, yy }); } }
                if (cx1 - cx0 + 1 > 0.85 * (ox1 - ox0 + 1) || comp.Count < 4) continue;
                foreach (var p in comp) { mask[p[0], p[1]] = true; x0 = Math.Min(x0, p[0]); x1 = Math.Max(x1, p[0]); y0 = Math.Min(y0, p[1]); y1 = Math.Max(y1, p[1]); }
            }
            if (x1 < 0) throw new Exception("no letter found");
            int midY = (y0 + y1) / 2;
            for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (mask[x, y]) { var c = px[x, y]; if (y < midY) { tR += c.R; tG += c.G; tB += c.B; nt++; } else { bR += c.R; bG += c.G; bB += c.B; nb++; } }
            Color top = Color.FromArgb((int)(tR / Math.Max(nt, 1)), (int)(tG / Math.Max(nt, 1)), (int)(tB / Math.Max(nt, 1)));
            Color bot = Color.FromArgb((int)(bR / Math.Max(nb, 1)), (int)(bG / Math.Max(nb, 1)), (int)(bB / Math.Max(nb, 1)));
            // the glow colour: the most saturated opaque-ish colour just outside the letter
            Color glow = Color.FromArgb(255, 120, 40); double best = -1;
            for (int y = Math.Max(0, y0 - 6); y <= Math.Min(h - 1, y1 + 6); y++) for (int x = Math.Max(0, x0 - 6); x <= Math.Min(w - 1, x1 + 6); x++) {
                if (mask[x, y]) continue; var c = px[x, y]; if (c.A < 60) continue;
                double sat = Math.Max(c.R, Math.Max(c.G, c.B)) - Math.Min(c.R, Math.Min(c.G, c.B)); if (sat * c.A > best) { best = sat * c.A; glow = Color.FromArgb(c.R, c.G, c.B); }
            }
            bool serif = textOnly && (Math.Max(glow.R, Math.Max(glow.G, glow.B)) - Math.Min(glow.R, Math.Min(glow.G, glow.B))) < 90;
            // grow the mask by 2 (anti-aliased edges of the letter), then fill it from the outside in
            var grown = new bool[w, h];
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (mask[x, y]) for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) { int xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < w && yy < h) grown[xx, yy] = true; }
            var fill = (Color[,])px.Clone();
            if (textOnly) { for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fill[x, y] = Color.FromArgb(0, 0, 0, 0); }
            else {
                var known = new bool[w, h]; int left = 0;
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { known[x, y] = !grown[x, y]; if (grown[x, y]) left++; }
                for (int pass = 0; pass < 200 && left > 0; pass++) {
                    var add = new List<int[]>();
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                        if (known[x, y]) continue;
                        double r = 0, g = 0, b = 0, a = 0; int n = 0;
                        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h || !known[xx, yy]) continue; var c = fill[xx, yy]; r += c.R; g += c.G; b += c.B; a += c.A; n++; }
                        if (n > 0) add.Add(new int[] { x, y, (int)(a / n), (int)(r / n), (int)(g / n), (int)(b / n) });
                    }
                    foreach (var p in add) { fill[p[0], p[1]] = Color.FromArgb(p[2], p[3], p[4], p[5]); known[p[0], p[1]] = true; left--; }
                }
            }
            using (var big = new Bitmap(w * Scale, h * Scale, PixelFormat.Format32bppArgb)) {
                using (var small = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) small.SetPixel(x, y, fill[x, y]);
                    using (var g = Graphics.FromImage(big)) {
                        g.InterpolationMode = InterpolationMode.HighQualityBicubic; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                        g.DrawImage(small, new Rectangle(0, 0, w * Scale, h * Scale), 0, 0, w, h, GraphicsUnit.Pixel);
                    }
                }
                using (var g = Graphics.FromImage(big)) {
                    g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit; g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    var box = new RectangleF(x0 * Scale, y0 * Scale, (x1 - x0 + 1) * Scale, (y1 - y0 + 1) * Scale);
                    float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2, side = box.Height;
                    var path = new GraphicsPath();
                    if (label.StartsWith("#")) {                                       // PlayStation shapes, drawn as outlines in the letter's height
                        float r = side * 0.46f, sw = side * 0.17f; string s = label.Substring(1);
                        var pen = new Pen(Color.Black, sw) { LineJoin = LineJoin.Round, StartCap = LineCap.Round, EndCap = LineCap.Round };
                        var shape = new GraphicsPath();
                        if (s == "circle") shape.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
                        else if (s == "square") shape.AddRectangle(new RectangleF(cx - r * 0.86f, cy - r * 0.86f, r * 1.72f, r * 1.72f));
                        else if (s == "triangle") shape.AddPolygon(new PointF[] { new PointF(cx, cy - r), new PointF(cx + r * 1.0f, cy + r * 0.78f), new PointF(cx - r * 1.0f, cy + r * 0.78f) });
                        else if (s == "cross") { shape.StartFigure(); shape.AddLine(cx - r * 0.85f, cy - r * 0.85f, cx + r * 0.85f, cy + r * 0.85f); shape.StartFigure(); shape.AddLine(cx + r * 0.85f, cy - r * 0.85f, cx - r * 0.85f, cy + r * 0.85f); }
                        shape.Widen(pen); path = shape;
                    } else {                                                           // text, fitted to the old letter's height (and width, if wider)
                        var ff = new FontFamily(serif ? "Times New Roman" : "Arial Black"); var fmt = StringFormat.GenericTypographic;
                        path.AddString(label, ff, (int)(serif ? FontStyle.Bold : FontStyle.Regular), 100f, new PointF(0, 0), fmt);
                        var b = path.GetBounds();
                        float sc = Math.Min(box.Height / b.Height, Math.Max(box.Width, box.Height * 0.8f * label.Length) / b.Width);
                        sc = Math.Min(sc, (w * Scale * 0.86f) / b.Width);
                        var m = new Matrix(); m.Translate(cx, cy); m.Scale(sc, sc); m.Translate(-(b.X + b.Width / 2), -(b.Y + b.Height / 2));
                        path.Transform(m);
                    }
                    if (textOnly && serif) {                                           // a pale serif label with a dark edge, as the game's "LT" legend
                        var rim = (GraphicsPath)path.Clone(); rim.Widen(new Pen(Color.Black, Scale * 2.5f) { LineJoin = LineJoin.Round });
                        using (var rb = new SolidBrush(Color.FromArgb(200, 20, 16, 8))) g.FillPath(rb, rim);
                    } else if (textOnly) {                                             // a new glow: the label widened and blurred in steps
                        for (int i = 8; i >= 1; i--) {
                            var halo = (GraphicsPath)path.Clone(); halo.Widen(new Pen(Color.Black, i * Scale * 1.6f) { LineJoin = LineJoin.Round });
                            using (var hb = new SolidBrush(Color.FromArgb(28, glow))) { g.FillPath(hb, halo); g.FillPath(hb, path); }
                        }
                    } else {                                                           // a faint dark rim so the label reads on any body
                        var rim = (GraphicsPath)path.Clone(); rim.Widen(new Pen(Color.Black, Scale * 1.2f) { LineJoin = LineJoin.Round });
                        using (var rb = new SolidBrush(Color.FromArgb(70, 0, 0, 0))) g.FillPath(rb, rim);
                    }
                    var pb = path.GetBounds();
                    using (var lg = new LinearGradientBrush(new RectangleF(pb.X, pb.Y - 1, Math.Max(pb.Width, 1), pb.Height + 2), top, bot, LinearGradientMode.Vertical)) g.FillPath(lg, path);
                }
                return (Bitmap)big.Clone();
            }
        }
    }
}
'@
$labels = @{
  "playstation" = @{ A = "#cross"; B = "#circle"; X = "#square"; Y = "#triangle"; LB = "L1"; RB = "R1"; LT = "L2"; RT = "R2"; LS = "L"; RS = "R"; BACK = "SHARE"; START = "OPTIONS" }
  "switch"      = @{ A = "B"; B = "A"; X = "Y"; Y = "X"; LB = "L"; RB = "R"; LT = "ZL"; RT = "ZR"; LS = "L"; RS = "R"; BACK = "-"; START = "+" }
}
$made = @()
foreach ($line in Get-Content $Map) {
  if ($line -match '^\s*(#|$)') { continue }
  $p = $line -split '\s+'; $hash = $p[0]; $btn = $p[1].ToUpper(); $textOnly = ($p.Count -gt 2 -and $p[2] -eq 'text')
  $src = Get-ChildItem $Dump -Filter "$hash*.png" | Select-Object -First 1
  if (-not $src) { Write-Warning "no dump for $hash"; continue }
  if ($btn -eq 'ATLAS') {
    foreach ($set in "playstation", "switch") { New-Item -ItemType Directory -Force (Join-Path $Out $set) | Out-Null }
    try { [Prompts]::MakeAtlas($src.FullName, (Join-Path $Out "playstation\$hash`_atlas.png"), (Join-Path $Out "switch\$hash`_atlas.png")); $made += 2 } catch { Write-Warning "$hash atlas: $_" }
    continue
  }
  foreach ($set in "playstation", "switch") {
    $dir = Join-Path $Out $set; New-Item -ItemType Directory -Force $dir | Out-Null
    $dst = Join-Path $dir "$hash`_$btn.png"
    try { [Prompts]::Make($src.FullName, $dst, $labels[$set][$btn], $textOnly); $made += $dst } catch { Write-Warning "$hash ${set}: $_" }
  }
}
"made $($made.Count) icons"
